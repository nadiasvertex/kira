#pragma once

#include <cstdint>

namespace cinder::runtime::reactor {

// ==========================================================================
//  The event loop behind the `io` context.
//
//  One reactor thread waits on the operating system's readiness queue
//  (kqueue on macOS and the BSDs, epoll on Linux) and on a set of timers.
//  A task that waits for a descriptor or a deadline is a small coroutine
//  object of its own, like `scheduler::word_waiter`: its first poll registers
//  it here and returns `pending`, and the reactor wakes it through the
//  scheduler when the descriptor is ready or the deadline has passed. No
//  worker thread is held while a task waits.
//
//  The reactor thread starts on first use and lives for the rest of the
//  process. Its state is leaked on purpose, as the scheduler's is.
// ==========================================================================

/// The direction a descriptor waiter waits for.
enum class interest : uint8_t {
  read = 0,
  write = 1,
};

/// A task that finishes once `fd` is ready for `what`. An error or hang-up on
/// the descriptor also finishes it, so the read or write that follows reports
/// the condition. So does a descriptor the readiness queue cannot watch, such
/// as a regular file under epoll, or an invalid one. The scheduler has
/// already taken the task over, on the `io` pool.
[[nodiscard]] auto fd_waiter(int fd, interest what) -> uint64_t *;

/// A task that finishes once the monotonic clock reaches `deadline`
/// nanoseconds (`monotonic_now`).
[[nodiscard]] auto timer_waiter(uint64_t deadline) -> uint64_t *;

/// Nanoseconds on a clock that never goes backwards, from an arbitrary start.
[[nodiscard]] auto monotonic_now() -> uint64_t;

} // namespace cinder::runtime::reactor

extern "C" {
/// The intrinsic ABI face of the calls above (`src/intrinsics.h`). A task
/// crosses as the pointer to its coroutine object.
auto cinder_rt_fd_readable(int64_t fd) -> uint64_t *;
auto cinder_rt_fd_writable(int64_t fd) -> uint64_t *;
auto cinder_rt_sleep_until(uint64_t deadline) -> uint64_t *;
auto cinder_rt_monotonic_now() -> uint64_t;
}
