#include "src/runtime/platform_query.h"

#include <array>
#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(__linux__)
#include <fstream>
#include <string>
#if defined(__GLIBC__)
#include <gnu/libc-version.h>
#endif
#endif

namespace kira::runtime::platform_query {

#if !defined(_WIN32)

auto query_uname() -> std::optional<uname_info> {
  struct utsname buf{};
  if (::uname(&buf) != 0) {
    return std::nullopt;
  }
  return uname_info{
      .sysname = buf.sysname,
      .nodename = buf.nodename,
      .release = buf.release,
      .version = buf.version,
      .machine = buf.machine,
  };
}

auto query_hostname() -> std::optional<std::string> {
  std::array<char, 256> buf{};
  if (::gethostname(buf.data(), buf.size()) != 0) {
    return std::nullopt;
  }
  return std::string(buf.data());
}

#else // defined(_WIN32)

auto query_uname() -> std::optional<uname_info> {
  // `uname(2)` has no Windows equivalent; `std.platform`'s Windows branch
  // never declares `rt_uname` at all (spec/std-reference.md gates it behind
  // `static if TARGET_OS_FAMILY == "unix"`), so this native symbol only
  // needs to exist for the table below to link, never to succeed.
  errno = ENOSYS;
  return std::nullopt;
}

auto query_hostname() -> std::optional<std::string> {
  std::array<char, MAX_COMPUTERNAME_LENGTH + 1> buf{};
  auto size = static_cast<DWORD>(buf.size());
  if (!GetComputerNameA(buf.data(), &size)) {
    return std::nullopt;
  }
  return std::string(buf.data());
}

#endif

auto query_processor_name() -> std::optional<std::string> {
#if defined(__APPLE__)
  std::array<char, 256> buf{};
  auto size = buf.size();
  if (::sysctlbyname("machdep.cpu.brand_string", buf.data(), &size, nullptr,
                     0) != 0) {
    return std::nullopt;
  }
  return std::string(buf.data());
#elif defined(__linux__)
  auto cpuinfo = std::ifstream("/proc/cpuinfo");
  if (!cpuinfo) {
    errno = ENOSYS;
    return std::nullopt;
  }
  auto line = std::string{};
  while (std::getline(cpuinfo, line)) {
    // Most architectures use "model name"; some (arm) use "Model" instead.
    for (const auto *prefix : {"model name", "Model"}) {
      if (line.starts_with(prefix)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
          continue;
        }
        auto value = line.substr(colon + 1);
        const auto first = value.find_first_not_of(' ');
        if (first == std::string::npos) {
          continue;
        }
        return value.substr(first);
      }
    }
  }
  errno = ENOSYS;
  return std::nullopt;
#elif defined(_WIN32)
  std::array<char, 256> buf{};
  auto size = static_cast<unsigned long>(buf.size()); // NOLINT
  const auto *key_path = R"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)";
  if (RegGetValueA(HKEY_LOCAL_MACHINE, key_path, "ProcessorNameString",
                   RRF_RT_REG_SZ, nullptr, buf.data(),
                   &size) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return std::string(buf.data());
#else
  return std::nullopt;
#endif
}

auto query_libc_version() -> std::optional<libc_info> {
#if defined(__linux__) && defined(__GLIBC__)
  return libc_info{.name = "glibc", .version = ::gnu_get_libc_version()};
#elif defined(__linux__)
  return libc_info{.name = "musl", .version = "unknown"};
#else
  // Not applicable off Linux — `std.platform` only ever calls this from the
  // `unix`-family branch, and even there only genuine glibc/musl hosts have
  // a meaningful answer.
  errno = ENOSYS;
  return std::nullopt;
#endif
}

auto query_windows_version() -> std::optional<windows_version_info> {
#if defined(_WIN32)
  // `GetVersionEx` lies (application-manifest-gated) on modern Windows;
  // `RtlGetVersion` (ntdll, undocumented but stable ABI) reports the real
  // OS version regardless of manifest.
  using rtl_get_version_fn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
  auto *ntdll = GetModuleHandleA("ntdll.dll");
  if (ntdll == nullptr) {
    return std::nullopt;
  }
  auto *proc = GetProcAddress(ntdll, "RtlGetVersion");
  if (proc == nullptr) {
    return std::nullopt;
  }
  auto rtl_get_version = reinterpret_cast<rtl_get_version_fn>(proc); // NOLINT
  RTL_OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (rtl_get_version(&info) != 0) {
    return std::nullopt;
  }
  auto csd = std::wstring(info.szCSDVersion);
  return windows_version_info{
      .major = info.dwMajorVersion,
      .minor = info.dwMinorVersion,
      .build = info.dwBuildNumber,
      .platform_id = info.dwPlatformId,
      .csd_version = std::string(csd.begin(), csd.end()),
  };
#else
  errno = ENOSYS;
  return std::nullopt;
#endif
}

auto query_macos_version() -> std::optional<macos_version_info> {
#if defined(__APPLE__)
  struct utsname uts{};
  if (::uname(&uts) != 0) {
    return std::nullopt;
  }
  std::array<char, 64> product_version{};
  auto size = product_version.size();
  auto version = std::string{};
  if (::sysctlbyname("kern.osproductversion", product_version.data(), &size,
                     nullptr, 0) == 0) {
    version = product_version.data();
  }
  return macos_version_info{
      .release = uts.release,
      .version = version,
      // Legacy fields from the classic Mac OS X `sw_vers`/Gestalt era with
      // no modern source of truth — CPython's own `platform.mac_ver()`
      // leaves these two empty on current macOS as well.
      .dev_stage = "",
      .non_release_version = "",
      .machine = uts.machine,
  };
#else
  errno = ENOSYS;
  return std::nullopt;
#endif
}

} // namespace kira::runtime::platform_query
