#include "src/runtime/reactor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#else
#include <sys/event.h>
#include <sys/time.h>
#endif

#include "src/runtime/allocator.h"
#include "src/runtime/coroutine.h"
#include "src/runtime/scheduler.h"

namespace cinder::runtime::reactor {

namespace co = cinder::runtime::coroutine;

namespace {

/// Where a waiter is in its wait, kept in its second value slot. Only the
/// reactor moves it to `ready`, with the reactor's lock held.
enum class stage : uint8_t {
  fresh = 0,      ///< Not yet registered.
  registered = 1, ///< In a waiter list or the timer queue.
  ready = 2,      ///< Woken by the reactor; the next poll finishes.
};

constexpr size_t k_stage_slot = co::k_value_slot + 1;

constexpr int k_panic_exit_code = 101; // `bytecode::k_panic_exit_code`

/// How many readiness events one wait collects.
constexpr int k_event_batch = 64;

struct fd_waiters {
  std::vector<uint64_t *> readers;
  std::vector<uint64_t *> writers;
};

struct timer {
  uint64_t deadline = 0;
  uint64_t *task = nullptr;

  friend auto operator>(const timer &a, const timer &b) -> bool {
    return a.deadline > b.deadline;
  }
};

struct reactor_state {
  std::mutex mutex;
  /// The kqueue or epoll descriptor.
  int queue = -1;
  /// A pipe whose read end is in `queue`. A byte written to it ends the
  /// reactor's wait, so it sees a timer earlier than the one it waits for.
  int wake_read = -1;
  int wake_write = -1;
  std::unordered_map<int, fd_waiters> fds;
  std::priority_queue<timer, std::vector<timer>, std::greater<>> timers;
};

[[noreturn]] void fail_to_start() {
  scheduler::report_panic("could not start the io event loop");
  std::_Exit(k_panic_exit_code);
}

auto stage_of(uint64_t *task) -> stage {
  return static_cast<stage>(std::atomic_ref(task[k_stage_slot]).load());
}

void set_stage(uint64_t *task, stage s) {
  std::atomic_ref(task[k_stage_slot]).store(static_cast<uint64_t>(s));
}

auto set_nonblocking_cloexec(int fd) -> bool {
  const int flags = ::fcntl(fd, F_GETFL); // NOLINT
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && // NOLINT
         ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;                         // NOLINT
}

#if defined(__linux__)

auto make_queue() -> int { return ::epoll_create1(EPOLL_CLOEXEC); }

auto watch_wake_pipe(int queue, int fd) -> bool {
  auto event = epoll_event{};
  event.events = EPOLLIN;
  event.data.fd = fd;
  return ::epoll_ctl(queue, EPOLL_CTL_ADD, fd, &event) == 0;
}

/// Arms `fd` for every direction that has waiters. epoll keeps one
/// registration per descriptor, and a one-shot one disables all of it once
/// it fires.
auto arm(reactor_state &r, int fd, const fd_waiters &waiting, interest /*what*/)
    -> bool {
  auto event = epoll_event{};
  event.events = EPOLLONESHOT;
  if (!waiting.readers.empty()) {
    event.events |= EPOLLIN | EPOLLRDHUP;
  }
  if (!waiting.writers.empty()) {
    event.events |= EPOLLOUT;
  }
  event.data.fd = fd;
  if (::epoll_ctl(r.queue, EPOLL_CTL_MOD, fd, &event) == 0) {
    return true;
  }
  return errno == ENOENT && ::epoll_ctl(r.queue, EPOLL_CTL_ADD, fd, &event) == 0;
}

struct ready_event {
  int fd = -1;
  bool readable = false;
  bool writable = false;
};

auto wait_events(const reactor_state &r, std::optional<uint64_t> timeout_ns,
                 std::vector<ready_event> &out) -> void {
  auto events = std::array<epoll_event, k_event_batch>{};
  int timeout_ms = -1;
  if (timeout_ns.has_value()) {
    // Round up, so the wait never ends before the deadline it waits for.
    timeout_ms = static_cast<int>(
        std::min<uint64_t>((*timeout_ns + 999'999) / 1'000'000, 60'000));
  }
  const int count = ::epoll_wait(r.queue, events.data(), k_event_batch,
                                 timeout_ms);
  for (int i = 0; i < count; ++i) {
    const auto &e = events[static_cast<size_t>(i)];
    const auto flags = e.events;
    out.push_back(ready_event{
        .fd = e.data.fd,
        .readable = (flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0,
        .writable = (flags & (EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0});
  }
}

/// Whether a fired descriptor must be armed again for the waiters that the
/// event did not wake: epoll disabled the whole registration.
constexpr bool k_rearm_after_event = true;

#else

auto make_queue() -> int {
  const int queue = ::kqueue();
  if (queue >= 0) {
    (void)::fcntl(queue, F_SETFD, FD_CLOEXEC); // NOLINT
  }
  return queue;
}

auto change_for(int fd, int16_t filter, uint16_t flags) -> struct kevent {
  struct kevent change{};
  change.ident = static_cast<uintptr_t>(fd);
  change.filter = filter;
  change.flags = flags;
  return change;
}

auto watch_wake_pipe(int queue, int fd) -> bool {
  const auto change = change_for(fd, EVFILT_READ, EV_ADD);
  return ::kevent(queue, &change, 1, nullptr, 0, nullptr) == 0;
}

/// Arms the one filter `what` names. kqueue keeps the read and write filters
/// of a descriptor apart, and a one-shot filter only removes itself.
auto arm(reactor_state &r, int fd, const fd_waiters & /*waiting*/,
         interest what) -> bool {
  const auto filter =
      static_cast<int16_t>(what == interest::read ? EVFILT_READ : EVFILT_WRITE);
  const auto change =
      change_for(fd, filter, static_cast<uint16_t>(EV_ADD | EV_ONESHOT));
  return ::kevent(r.queue, &change, 1, nullptr, 0, nullptr) == 0;
}

struct ready_event {
  int fd = -1;
  bool readable = false;
  bool writable = false;
};

auto wait_events(const reactor_state &r, std::optional<uint64_t> timeout_ns,
                 std::vector<ready_event> &out) -> void {
  auto events = std::array<struct kevent, k_event_batch>{};
  auto timeout = timespec{};
  const timespec *wait_for = nullptr;
  if (timeout_ns.has_value()) {
    timeout.tv_sec = static_cast<time_t>(*timeout_ns / 1'000'000'000);
    timeout.tv_nsec = static_cast<long>(*timeout_ns % 1'000'000'000);
    wait_for = &timeout;
  }
  const int count =
      ::kevent(r.queue, nullptr, 0, events.data(), k_event_batch, wait_for);
  for (int i = 0; i < count; ++i) {
    const auto &e = events[static_cast<size_t>(i)];
    out.push_back(ready_event{.fd = static_cast<int>(e.ident),
                              .readable = e.filter == EVFILT_READ,
                              .writable = e.filter == EVFILT_WRITE});
  }
}

constexpr bool k_rearm_after_event = false;

#endif

void run_loop(reactor_state &r);

/// Leaked on purpose, like the scheduler's state: the reactor thread uses it
/// until the process ends.
auto state() -> reactor_state & {
  static auto *const instance = [] {
    auto *r = new reactor_state;
    auto ends = std::array<int, 2>{};
    r->queue = make_queue();
    if (r->queue < 0 || ::pipe(ends.data()) != 0) {
      fail_to_start();
    }
    r->wake_read = ends[0];
    r->wake_write = ends[1];
    if (!set_nonblocking_cloexec(r->wake_read) ||
        !set_nonblocking_cloexec(r->wake_write) ||
        !watch_wake_pipe(r->queue, r->wake_read)) {
      fail_to_start();
    }
    try {
      std::thread([r] { run_loop(*r); }).detach();
    } catch (const std::system_error &) {
      fail_to_start();
    }
    return r;
  }();
  return *instance;
}

void ring(const reactor_state &r) {
  const char byte = 0;
  // A full pipe already holds a wake, so a failed write loses nothing.
  (void)::write(r.wake_write, &byte, 1);
}

void drain(const reactor_state &r) {
  auto bytes = std::array<char, 64>{};
  while (::read(r.wake_read, bytes.data(), bytes.size()) > 0) {
  }
}

/// Marks every task in `list` ready and moves it to `out`. Lock held.
void take_all(std::vector<uint64_t *> &list, std::vector<uint64_t *> &out) {
  for (auto *task : list) {
    set_stage(task, stage::ready);
    out.push_back(task);
  }
  list.clear();
}

void run_loop(reactor_state &r) {
  auto events = std::vector<ready_event>{};
  auto ready = std::vector<uint64_t *>{};
  for (;;) {
    auto timeout = std::optional<uint64_t>{};
    {
      const auto lock = std::scoped_lock(r.mutex);
      if (!r.timers.empty()) {
        const auto now = monotonic_now();
        const auto deadline = r.timers.top().deadline;
        timeout = deadline > now ? deadline - now : 0;
      }
    }
    events.clear();
    wait_events(r, timeout, events);
    {
      const auto lock = std::scoped_lock(r.mutex);
      for (const auto &event : events) {
        if (event.fd == r.wake_read) {
          drain(r);
          continue;
        }
        const auto found = r.fds.find(event.fd);
        if (found == r.fds.end()) {
          continue;
        }
        auto &waiting = found->second;
        if (event.readable) {
          take_all(waiting.readers, ready);
        }
        if (event.writable) {
          take_all(waiting.writers, ready);
        }
        if (waiting.readers.empty() && waiting.writers.empty()) {
          r.fds.erase(found);
        } else if (k_rearm_after_event &&
                   !arm(r, event.fd, waiting, interest::read)) {
          take_all(waiting.readers, ready);
          take_all(waiting.writers, ready);
          r.fds.erase(found);
        }
      }
      const auto now = monotonic_now();
      while (!r.timers.empty() && r.timers.top().deadline <= now) {
        auto *task = r.timers.top().task;
        r.timers.pop();
        set_stage(task, stage::ready);
        ready.push_back(task);
      }
    }
    // The scheduler takes its own lock; never hold the reactor's with it.
    for (auto *task : ready) {
      scheduler::wake(task);
    }
    ready.clear();
  }
}

/// Registers a descriptor waiter. False when the descriptor cannot be
/// watched, in which case the waiter finishes at once.
auto watch_fd(uint64_t *task) -> bool {
  const auto packed = task[co::k_value_slot];
  const auto fd = static_cast<int>(static_cast<uint32_t>(packed));
  const auto what = static_cast<interest>(packed >> 32U);
  auto &r = state();
  const auto lock = std::scoped_lock(r.mutex);
  auto &waiting = r.fds[fd];
  auto &list = what == interest::read ? waiting.readers : waiting.writers;
  list.push_back(task);
  if (arm(r, fd, waiting, what)) {
    return true;
  }
  list.pop_back();
  if (waiting.readers.empty() && waiting.writers.empty()) {
    r.fds.erase(fd);
  }
  return false;
}

/// Registers a timer. False when its deadline has already passed.
auto watch_timer(uint64_t *task) -> bool {
  const auto deadline = task[co::k_value_slot];
  if (monotonic_now() >= deadline) {
    return false;
  }
  auto &r = state();
  const auto lock = std::scoped_lock(r.mutex);
  const bool earliest = r.timers.empty() || deadline < r.timers.top().deadline;
  r.timers.push(timer{.deadline = deadline, .task = task});
  if (earliest) {
    ring(r);
  }
  return true;
}

/// The poll both waiters share; `watch` registers one.
auto poll_waiter(uint64_t *task, auto (*watch)(uint64_t *)->bool) -> uint64_t {
  if (task[co::k_status_slot] == co::raw(co::status::cancel_requested)) {
    // A cancelled waiter may still be in a list. The reactor's later wake of
    // a finished task does nothing.
    return co::raw(co::status::done);
  }
  switch (stage_of(task)) {
  case stage::ready:
    return co::raw(co::status::done);
  case stage::registered:
    return co::raw(co::status::pending);
  case stage::fresh:
    break;
  }
  set_stage(task, stage::registered);
  if (!watch(task)) {
    return co::raw(co::status::done);
  }
  return co::raw(co::status::pending);
}

auto fd_waiter_step(void * /*state*/, uint64_t /*resume_index*/, void *object)
    -> uint64_t {
  return poll_waiter(static_cast<uint64_t *>(object), watch_fd);
}

auto timer_waiter_step(void * /*state*/, uint64_t /*resume_index*/,
                       void *object) -> uint64_t {
  return poll_waiter(static_cast<uint64_t *>(object), watch_timer);
}

auto make_waiter(uint64_t (*step)(void *, uint64_t, void *), uint64_t value)
    -> uint64_t * {
  auto *task = static_cast<uint64_t *>(
      cinder_heap_alloc(co::k_slot_count * sizeof(uint64_t)));
  task[co::k_step_function_slot] = reinterpret_cast<uint64_t>(step); // NOLINT
  task[co::k_state_ptr_slot] = 0;
  task[co::k_resume_index_slot] = 0;
  task[co::k_status_slot] = co::raw(co::status::suspended);
  task[co::k_record_slot] = 0;
  task[co::k_value_slot] = value;
  task[k_stage_slot] = static_cast<uint64_t>(stage::fresh);
  scheduler::spawn(task, scheduler::pool::io, scheduler::native_runner());
  return task;
}

} // namespace

auto fd_waiter(int fd, interest what) -> uint64_t * {
  const auto packed = uint64_t{static_cast<uint32_t>(fd)} |
                      (uint64_t{static_cast<uint8_t>(what)} << 32U);
  return make_waiter(fd_waiter_step, packed);
}

auto timer_waiter(uint64_t deadline) -> uint64_t * {
  return make_waiter(timer_waiter_step, deadline);
}

auto monotonic_now() -> uint64_t {
  const auto since = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(since).count());
}

} // namespace cinder::runtime::reactor

extern "C" {

auto cinder_rt_fd_readable(int64_t fd) -> uint64_t * {
  return cinder::runtime::reactor::fd_waiter(
      static_cast<int>(fd), cinder::runtime::reactor::interest::read);
}

auto cinder_rt_fd_writable(int64_t fd) -> uint64_t * {
  return cinder::runtime::reactor::fd_waiter(
      static_cast<int>(fd), cinder::runtime::reactor::interest::write);
}

auto cinder_rt_sleep_until(uint64_t deadline) -> uint64_t * {
  return cinder::runtime::reactor::timer_waiter(deadline);
}

auto cinder_rt_monotonic_now() -> uint64_t {
  return cinder::runtime::reactor::monotonic_now();
}
}
