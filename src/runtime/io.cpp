#include "src/runtime/io.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <expected>
#include <fcntl.h>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>

#include "src/runtime/allocator.h"
#include "src/runtime/result_out.h"
#include "src/runtime/scheduler.h"

namespace {

[[nodiscard]] auto alloc_slots(size_t count) -> uint64_t * {
  return static_cast<uint64_t *>(cinder_heap_alloc(count * sizeof(uint64_t)));
}

[[nodiscard]] auto raw_fd_of(const uint64_t *fd_struct) -> int {
  return static_cast<int>(static_cast<int64_t>(fd_struct[0]));
}

[[nodiscard]] auto make_raw_fd(int64_t fd) -> uint64_t * {
  auto *slots = alloc_slots(1);
  slots[0] = static_cast<uint64_t>(fd);
  return slots;
}

[[nodiscard]] auto bytes_of(const uint64_t *header) -> std::span<char> {
  auto *data = reinterpret_cast<char *>(header[1]); // NOLINT
  return {data, static_cast<size_t>(header[0])};
}

[[nodiscard]] auto make_result_ok(uint64_t payload) -> uint64_t * {
  auto *slots = alloc_slots(2);
  slots[0] = 0;
  slots[1] = payload;
  return slots;
}

[[nodiscard]] auto make_result_err(int errno_code) -> uint64_t * {
  auto *code_struct = alloc_slots(1);
  code_struct[0] = static_cast<uint64_t>(static_cast<int64_t>(errno_code));
  auto *slots = alloc_slots(2);
  slots[0] = 1;
  slots[1] = reinterpret_cast<uint64_t>(code_struct); // NOLINT
  return slots;
}

/// A system call's outcome: its value, or the `errno` it failed with. Both the
/// older heap-block entry points and the `_into` ones below run these, so the
/// two cannot disagree about what a call does.
using outcome = std::expected<int64_t, int>;

[[nodiscard]] auto failed() -> outcome { return std::unexpected(errno); }

[[nodiscard]] auto open_file(std::string_view path, const uint8_t *opts)
    -> outcome {
  // `open_options` is five `bool`s, one byte each at offsets 0..4
  // (`runtime::struct_layout`), not five 8-byte slots.
  const bool want_read = opts[0] != 0;
  const bool want_write = opts[1] != 0;
  const bool append = opts[2] != 0;
  const bool create = opts[3] != 0;
  const bool truncate = opts[4] != 0;

  int flags = O_RDONLY;
  if (want_read && want_write) {
    flags = O_RDWR;
  } else if (want_write) {
    flags = O_WRONLY;
  }
  if (append) {
    flags |= O_APPEND;
  }
  if (create) {
    flags |= O_CREAT;
  }
  if (truncate) {
    flags |= O_TRUNC;
  }
  const int fd = ::open(std::string(path).c_str(), flags, 0644); // NOLINT
  return fd < 0 ? failed() : outcome{fd};
}

[[nodiscard]] auto close_file(int fd) -> outcome {
  return ::close(fd) != 0 ? failed() : outcome{0};
}

[[nodiscard]] auto read_file(int fd, std::span<char> buffer) -> outcome {
  const auto n = ::read(fd, buffer.data(), buffer.size());
  return n < 0 ? failed() : outcome{static_cast<int64_t>(n)};
}

[[nodiscard]] auto write_file(int fd, std::span<char> bytes) -> outcome {
  const auto n = ::write(fd, bytes.data(), bytes.size());
  return n < 0 ? failed() : outcome{static_cast<int64_t>(n)};
}

[[nodiscard]] auto make_pipe() -> std::expected<std::array<int, 2>, int> {
  auto ends = std::array<int, 2>{};
  if (::pipe(ends.data()) != 0) {
    return std::unexpected(errno);
  }
  for (const int end : ends) {
    (void)::fcntl(end, F_SETFD, FD_CLOEXEC); // NOLINT
  }
  return ends;
}

[[nodiscard]] auto make_nonblocking(int descriptor) -> outcome {
  const int flags = ::fcntl(descriptor, F_GETFL); // NOLINT
  if (flags < 0 ||
      ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) { // NOLINT
    return failed();
  }
  return 0;
}

} // namespace

extern "C" {

auto cinder_rt_stdin() -> uint64_t * { return make_raw_fd(0); }
auto cinder_rt_stdout() -> uint64_t * { return make_raw_fd(1); }
auto cinder_rt_stderr() -> uint64_t * { return make_raw_fd(2); }

auto cinder_rt_open(uint64_t *path, uint64_t *opts_struct) -> uint64_t * {
  const auto path_bytes = bytes_of(path);
  const auto opened = open_file(
      std::string_view(path_bytes.data(), path_bytes.size()),
      reinterpret_cast<const uint8_t *>(opts_struct)); // NOLINT
  if (!opened) {
    return make_result_err(opened.error());
  }
  return make_result_ok(reinterpret_cast<uint64_t>(make_raw_fd(*opened))); // NOLINT
}

auto cinder_rt_close(uint64_t *fd) -> uint64_t * {
  const auto closed = close_file(raw_fd_of(fd));
  return closed ? make_result_ok(0) : make_result_err(closed.error());
}

auto cinder_rt_read(uint64_t *fd, uint64_t *buf) -> uint64_t * {
  const auto n = read_file(raw_fd_of(fd), bytes_of(buf));
  return n ? make_result_ok(static_cast<uint64_t>(*n))
           : make_result_err(n.error());
}

auto cinder_rt_write(uint64_t *fd, uint64_t *buf) -> uint64_t * {
  const auto n = write_file(raw_fd_of(fd), bytes_of(buf));
  return n ? make_result_ok(static_cast<uint64_t>(*n))
           : make_result_err(n.error());
}

auto cinder_rt_flush(uint64_t * /*fd*/) -> uint64_t * {
  // No userspace buffering layer sits above the raw ::read/::write syscalls
  // above, so there is nothing for this tier to flush either.
  return make_result_ok(0);
}

auto cinder_rt_pipe() -> uint64_t * {
  const auto ends = make_pipe();
  if (!ends) {
    return make_result_err(ends.error());
  }
  auto *pair = alloc_slots(2);
  pair[0] = static_cast<uint64_t>(int64_t{(*ends)[0]});
  pair[1] = static_cast<uint64_t>(int64_t{(*ends)[1]});
  return make_result_ok(reinterpret_cast<uint64_t>(pair)); // NOLINT
}

auto cinder_rt_set_nonblocking(uint64_t *fd) -> uint64_t * {
  const auto done = make_nonblocking(raw_fd_of(fd));
  return done ? make_result_ok(0) : make_result_err(done.error());
}

auto cinder_rt_errno_would_block() -> int32_t { return EAGAIN; }

// ---------------------------------------------------------------------------
// The `_into` entry points (`src/intrinsics.h`, `intrinsic_signature::into`).
// Each writes its result into the caller's storage `result`, in the layouts
// `src/runtime/result_out.h` lists, and allocates nothing but the bytes a
// `str` result owns. A `raw_fd`, `open_options`, or descriptor argument is
// the address of the caller's own bytes: a read-only view for the call.
// ---------------------------------------------------------------------------

namespace out = cinder::runtime::out;

void cinder_rt_stdin_into(void *result) {
  out::clear(result, out::k_raw_fd_size);
  out::store<int64_t>(result, 0, 0);
}
void cinder_rt_stdout_into(void *result) {
  out::clear(result, out::k_raw_fd_size);
  out::store<int64_t>(result, 0, 1);
}
void cinder_rt_stderr_into(void *result) {
  out::clear(result, out::k_raw_fd_size);
  out::store<int64_t>(result, 0, 2);
}

void cinder_rt_open_into(void *result, const void *path, const void *opts) {
  const auto opened = open_file(out::view_of(path),
                                static_cast<const uint8_t *>(opts));
  if (!opened) {
    out::write_err(result, out::k_result_word_size, opened.error());
    return;
  }
  out::begin_ok(result, out::k_result_word_size);
  out::store<int64_t>(result, out::k_result_payload_offset, *opened);
}

void cinder_rt_close_into(void *result, const void *fd) {
  const auto closed = close_file(raw_fd_of(static_cast<const uint64_t *>(fd)));
  if (!closed) {
    out::write_err(result, out::k_result_unit_size, closed.error());
    return;
  }
  out::begin_ok(result, out::k_result_unit_size);
}

void cinder_rt_read_into(void *result, const void *fd, const void *buf) {
  const auto n = read_file(raw_fd_of(static_cast<const uint64_t *>(fd)),
                           bytes_of(static_cast<const uint64_t *>(buf)));
  if (!n) {
    out::write_err(result, out::k_result_word_size, n.error());
    return;
  }
  out::begin_ok(result, out::k_result_word_size);
  out::store<int64_t>(result, out::k_result_payload_offset, *n);
}

void cinder_rt_write_into(void *result, const void *fd, const void *buf) {
  const auto n = write_file(raw_fd_of(static_cast<const uint64_t *>(fd)),
                            bytes_of(static_cast<const uint64_t *>(buf)));
  if (!n) {
    out::write_err(result, out::k_result_word_size, n.error());
    return;
  }
  out::begin_ok(result, out::k_result_word_size);
  out::store<int64_t>(result, out::k_result_payload_offset, *n);
}

void cinder_rt_flush_into(void *result, const void * /*fd*/) {
  out::begin_ok(result, out::k_result_unit_size);
}

void cinder_rt_pipe_into(void *result) {
  constexpr auto size = out::result_size(2 * sizeof(int64_t));
  const auto ends = make_pipe();
  if (!ends) {
    out::write_err(result, size, ends.error());
    return;
  }
  out::begin_ok(result, size);
  out::store<int64_t>(result, out::k_result_payload_offset, (*ends)[0]);
  out::store<int64_t>(result,
                      out::k_result_payload_offset + sizeof(int64_t),
                      (*ends)[1]);
}

void cinder_rt_set_nonblocking_into(void *result, const void *fd) {
  const auto done = make_nonblocking(raw_fd_of(static_cast<const uint64_t *>(fd)));
  if (!done) {
    out::write_err(result, out::k_result_unit_size, done.error());
    return;
  }
  out::begin_ok(result, out::k_result_unit_size);
}

[[noreturn]] auto cinder_rt_panic(uint64_t *msg) -> uint64_t * {
  // Reported straight to fd 2 rather than through `std.io`: a panic must still
  // report itself when the failure being reported is in the I/O layer.
  const auto text = bytes_of(msg);
  cinder::runtime::scheduler::raise_native_panic(
      std::string_view(text.data(), text.size()));
}

} // extern "C"
