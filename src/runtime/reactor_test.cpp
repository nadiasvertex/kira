#include "src/runtime/reactor.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "src/runtime/scheduler.h"
#include "src/testing/test_assert.h"

// The event loop behind the `io` context. These tests run with one `io`
// worker (`CINDER_IO_WORKERS=1` in BUILD.bazel), so a waiter that held its
// worker while it waited would hold up every other waiter.

namespace {

using cinder::testing::expect;
namespace reactor = cinder::runtime::reactor;
namespace sched = cinder::runtime::scheduler;

constexpr uint64_t k_millisecond = 1'000'000;

void wait_for(uint64_t *task) {
  sched::block_on(task, sched::pool::io, sched::native_runner());
}

/// Long enough for a waiter that should stay pending to have been polled,
/// registered, and (wrongly) finished.
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(30)); }

struct fd_pair {
  int first = -1;
  int second = -1;
};

auto make_pipe() -> fd_pair {
  auto ends = std::array<int, 2>{};
  expect(::pipe(ends.data()) == 0, "expected a pipe");
  return fd_pair{.first = ends[0], .second = ends[1]};
}

auto make_socket_pair() -> fd_pair {
  auto ends = std::array<int, 2>{};
  expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) == 0,
         "expected a socket pair");
  return fd_pair{.first = ends[0], .second = ends[1]};
}

void write_byte(int fd) {
  const char byte = 'x';
  expect(::write(fd, &byte, 1) == 1, "expected to write one byte");
}

auto test_a_timer_finishes_after_its_deadline() -> void {
  const auto deadline = reactor::monotonic_now() + 30 * k_millisecond;
  wait_for(reactor::timer_waiter(deadline));
  expect(reactor::monotonic_now() >= deadline,
         "expected the timer to finish no earlier than its deadline");
}

auto test_a_past_deadline_finishes_at_once() -> void {
  const auto start = reactor::monotonic_now();
  wait_for(reactor::timer_waiter(0));
  expect(reactor::monotonic_now() - start < 500 * k_millisecond,
         "expected a timer whose deadline has passed to finish at once");
}

auto test_an_earlier_timer_finishes_first() -> void {
  const auto now = reactor::monotonic_now();
  auto *late = reactor::timer_waiter(now + 300 * k_millisecond);
  settle(); // the late timer is the one the loop waits for
  auto *early = reactor::timer_waiter(now + 40 * k_millisecond);
  wait_for(early);
  expect(!sched::is_finished(late),
         "expected the later timer to still be waiting");
  expect(reactor::monotonic_now() < now + 300 * k_millisecond,
         "expected an earlier timer added later to end the loop's wait");
  wait_for(late);
}

auto test_many_timers_share_one_worker() -> void {
  // Blocking the one `io` worker for each would take 100 * 50 ms.
  const auto start = reactor::monotonic_now();
  auto timers = std::vector<uint64_t *>{};
  for (int i = 0; i < 100; ++i) {
    timers.push_back(reactor::timer_waiter(start + 50 * k_millisecond));
  }
  sched::block_on_all(timers.data(), timers.size(), sched::pool::io,
                      sched::native_runner());
  const auto elapsed = reactor::monotonic_now() - start;
  expect(elapsed < 1000 * k_millisecond,
         "expected 100 timers to wait together, not one after another (took " +
             std::to_string(elapsed / k_millisecond) + " ms)");
}

auto test_a_reader_waits_for_data() -> void {
  const auto p = make_pipe();
  auto *reader = reactor::fd_waiter(p.first, reactor::interest::read);
  settle();
  expect(!sched::is_finished(reader),
         "expected a reader on an empty pipe to wait");
  write_byte(p.second);
  wait_for(reader);
  ::close(p.first);
  ::close(p.second);
}

auto test_one_write_wakes_every_reader() -> void {
  const auto p = make_pipe();
  auto *a = reactor::fd_waiter(p.first, reactor::interest::read);
  auto *b = reactor::fd_waiter(p.first, reactor::interest::read);
  settle();
  expect(!sched::is_finished(a) && !sched::is_finished(b),
         "expected both readers to wait");
  write_byte(p.second);
  wait_for(a);
  wait_for(b);
  ::close(p.first);
  ::close(p.second);
}

auto test_hang_up_wakes_a_reader() -> void {
  const auto p = make_pipe();
  auto *reader = reactor::fd_waiter(p.first, reactor::interest::read);
  settle();
  expect(!sched::is_finished(reader), "expected the reader to wait");
  ::close(p.second);
  wait_for(reader);
  ::close(p.first);
}

auto test_a_full_pipe_waits_until_drained() -> void {
  const auto p = make_pipe();
  const int flags = ::fcntl(p.second, F_GETFL);               // NOLINT
  expect(::fcntl(p.second, F_SETFL, flags | O_NONBLOCK) == 0, // NOLINT
         "expected to make the write end non-blocking");
  auto chunk = std::array<char, 4096>{};
  while (::write(p.second, chunk.data(), chunk.size()) > 0) {
  }
  expect(errno == EAGAIN, "expected the pipe to fill up");
  auto *writer = reactor::fd_waiter(p.second, reactor::interest::write);
  settle();
  expect(!sched::is_finished(writer),
         "expected a writer on a full pipe to wait");
  // Drain all of it: a pipe reports writable only once a page is free.
  const int read_flags = ::fcntl(p.first, F_GETFL);               // NOLINT
  expect(::fcntl(p.first, F_SETFL, read_flags | O_NONBLOCK) == 0, // NOLINT
         "expected to make the read end non-blocking");
  while (::read(p.first, chunk.data(), chunk.size()) > 0) {
  }
  wait_for(writer);
  ::close(p.first);
  ::close(p.second);
}

auto test_a_reader_and_a_writer_on_one_descriptor() -> void {
  // epoll keeps one registration per descriptor for both directions, and
  // disables all of it when either fires; the reader must stay registered.
  const auto s = make_socket_pair();
  auto *reader = reactor::fd_waiter(s.first, reactor::interest::read);
  settle();
  auto *writer = reactor::fd_waiter(s.first, reactor::interest::write);
  wait_for(writer);
  settle();
  expect(!sched::is_finished(reader),
         "expected the reader to keep waiting after the writer finished");
  write_byte(s.second);
  wait_for(reader);
  ::close(s.first);
  ::close(s.second);
}

auto test_an_invalid_descriptor_finishes_at_once() -> void {
  const auto p = make_pipe();
  ::close(p.first);
  ::close(p.second);
  wait_for(reactor::fd_waiter(p.first, reactor::interest::read));
  wait_for(reactor::fd_waiter(-1, reactor::interest::write));
}

auto test_a_regular_file_finishes_at_once() -> void {
  const auto *dir = std::getenv("TEST_TMPDIR");
  const auto path =
      std::string(dir != nullptr ? dir : "/tmp") + "/reactor_test_regular_file";
  const int fd =
      ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600); // NOLINT
  expect(fd >= 0, "expected to create a regular file");
  wait_for(reactor::fd_waiter(fd, reactor::interest::read));
  wait_for(reactor::fd_waiter(fd, reactor::interest::write));
  ::close(fd);
  ::unlink(path.c_str());
}

auto test_a_cancelled_waiter_finishes_and_ignores_a_later_wake() -> void {
  const auto p = make_pipe();
  auto *reader = reactor::fd_waiter(p.first, reactor::interest::read);
  auto *timer =
      reactor::timer_waiter(reactor::monotonic_now() + 50 * k_millisecond);
  settle();
  expect(!sched::is_finished(reader), "expected the reader to wait");
  expect(sched::cancel(reader), "expected the scheduler to own the reader");
  expect(sched::cancel(timer), "expected the scheduler to own the timer");
  wait_for(reader);
  wait_for(timer);
  // The reactor still lists both; waking a finished task does nothing.
  write_byte(p.second);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  sched::wait_idle();
  ::close(p.first);
  ::close(p.second);
}

} // namespace

auto main() -> int {
  test_a_timer_finishes_after_its_deadline();
  test_a_past_deadline_finishes_at_once();
  test_an_earlier_timer_finishes_first();
  test_many_timers_share_one_worker();
  test_a_reader_waits_for_data();
  test_one_write_wakes_every_reader();
  test_hang_up_wakes_a_reader();
  test_a_full_pipe_waits_until_drained();
  test_a_reader_and_a_writer_on_one_descriptor();
  test_an_invalid_descriptor_finishes_at_once();
  test_a_regular_file_finishes_at_once();
  test_a_cancelled_waiter_finishes_and_ignores_a_later_wake();
  return 0;
}
