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

[[nodiscard]] auto raw_fd_of(const uint64_t *fd_struct) -> int {
  return static_cast<int>(static_cast<int64_t>(fd_struct[0]));
}

[[nodiscard]] auto bytes_of(const uint64_t *header) -> std::span<char> {
  auto *data = reinterpret_cast<char *>(header[1]); // NOLINT
  return {data, static_cast<size_t>(header[0])};
}

/// A system call's outcome: its value, or the `errno` it failed with.
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
