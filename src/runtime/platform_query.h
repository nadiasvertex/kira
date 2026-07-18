#pragma once

#include <cstdint>
#include <optional>
#include <string>

/// Host OS queries backing `std.platform`'s runtime intrinsics
/// (spec/std-reference.md). Both native implementations of each `rt_*`
/// intrinsic — `src/runtime/platform.h`/`.cpp` (the LLVM/AOT tier's
/// `extern "C"` ABI layer) and `src/bytecode/vm.cpp`'s `intrinsic_rt_*`
/// dispatch table (the bytecode tier) — call through this single set of
/// plain C++ query functions rather than each re-deriving the OS-specific
/// logic (`::uname`, `sysctlbyname`, `/proc/cpuinfo` parsing, ...): unlike
/// `src/runtime/io.h`'s eight I/O intrinsics (thin enough that each backend
/// duplicating the raw syscall is no real cost), the platform queries below
/// have enough OS-specific branching that sharing it here is what keeps the
/// two ABI layers from drifting apart on anything but value encoding.
///
/// Every query returns `std::nullopt` on failure or "not applicable on this
/// host" (with `errno` left set by the underlying OS call where relevant);
/// each ABI layer is responsible for turning that into its own
/// `result[T, io_errno]` encoding.
namespace kira::runtime::platform_query {

/// Mirrors `std.platform`'s `uname_raw` type field-for-field.
struct uname_info {
  std::string sysname;
  std::string nodename;
  std::string release;
  std::string version;
  std::string machine;
};

/// Mirrors `std.platform`'s `libc_version` type field-for-field.
struct libc_info {
  std::string name;
  std::string version;
};

/// Mirrors `std.platform`'s `winver_raw` type field-for-field.
struct windows_version_info {
  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t build = 0;
  uint32_t platform_id = 0;
  std::string csd_version;
};

/// Mirrors `std.platform`'s `macver_raw` type field-for-field.
struct macos_version_info {
  std::string release;
  std::string version;
  std::string dev_stage;
  std::string non_release_version;
  std::string machine;
};

[[nodiscard]] auto query_uname() -> std::optional<uname_info>;
[[nodiscard]] auto query_hostname() -> std::optional<std::string>;
[[nodiscard]] auto query_processor_name() -> std::optional<std::string>;
[[nodiscard]] auto query_libc_version() -> std::optional<libc_info>;
[[nodiscard]] auto query_windows_version()
    -> std::optional<windows_version_info>;
[[nodiscard]] auto query_macos_version() -> std::optional<macos_version_info>;

} // namespace kira::runtime::platform_query
