#include "src/runtime/io.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>

#include "src/runtime/arena.h"

namespace {

using kira::runtime::global_arena;

[[nodiscard]] auto alloc_slots(size_t count) -> uint64_t * {
  return static_cast<uint64_t *>(
      global_arena().allocate(count * sizeof(uint64_t)));
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

} // namespace

extern "C" {

auto kira_rt_stdin() -> uint64_t * { return make_raw_fd(0); }
auto kira_rt_stdout() -> uint64_t * { return make_raw_fd(1); }
auto kira_rt_stderr() -> uint64_t * { return make_raw_fd(2); }

auto kira_rt_open(uint64_t *path, uint64_t *opts) -> uint64_t * {
  const auto path_bytes = bytes_of(path);
  const auto path_str = std::string(path_bytes.data(), path_bytes.size());
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

  const int fd = ::open(path_str.c_str(), flags, 0644); // NOLINT
  if (fd < 0) {
    return make_result_err(errno);
  }
  return make_result_ok(reinterpret_cast<uint64_t>(make_raw_fd(fd))); // NOLINT
}

auto kira_rt_close(uint64_t *fd) -> uint64_t * {
  if (::close(raw_fd_of(fd)) != 0) {
    return make_result_err(errno);
  }
  return make_result_ok(0);
}

auto kira_rt_read(uint64_t *fd, uint64_t *buf) -> uint64_t * {
  const auto bytes = bytes_of(buf);
  const auto n = ::read(raw_fd_of(fd), bytes.data(), bytes.size());
  if (n < 0) {
    return make_result_err(errno);
  }
  return make_result_ok(static_cast<uint64_t>(n));
}

auto kira_rt_write(uint64_t *fd, uint64_t *buf) -> uint64_t * {
  const auto bytes = bytes_of(buf);
  const auto n = ::write(raw_fd_of(fd), bytes.data(), bytes.size());
  if (n < 0) {
    return make_result_err(errno);
  }
  return make_result_ok(static_cast<uint64_t>(n));
}

auto kira_rt_flush(uint64_t * /*fd*/) -> uint64_t * {
  // No userspace buffering layer sits above the raw ::read/::write syscalls
  // above, so there is nothing for this tier to flush either.
  return make_result_ok(0);
}

[[noreturn]] auto kira_rt_panic(uint64_t *msg) -> uint64_t * {
  // Written straight to fd 2 rather than through `std.io`: a panic must still
  // report itself when the failure being reported is in the I/O layer.
  const auto text = bytes_of(msg);
  static constexpr auto k_prefix = std::string_view{"panic: "};
  (void)::write(2, k_prefix.data(), k_prefix.size());
  (void)::write(2, text.data(), text.size());
  (void)::write(2, "\n", 1);
  std::abort();
}

} // extern "C"
