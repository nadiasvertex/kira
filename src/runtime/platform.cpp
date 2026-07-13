#include "src/runtime/platform.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>

#include "src/runtime/arena.h"
#include "src/runtime/platform_query.h"

namespace {

using kira::runtime::global_arena;
namespace query = kira::runtime::platform_query;

[[nodiscard]] auto alloc_slots(size_t count) -> uint64_t * {
  return static_cast<uint64_t *>(
      global_arena().allocate(count * sizeof(uint64_t)));
}

[[nodiscard]] auto ptr_slot(void *raw) -> uint64_t {
  return reinterpret_cast<uint64_t>(raw); // NOLINT
}

[[nodiscard]] auto make_str(std::string_view text) -> uint64_t * {
  auto *bytes = static_cast<char *>(global_arena().allocate(text.size()));
  if (!text.empty()) {
    std::memcpy(bytes, text.data(), text.size());
  }
  auto *slots = alloc_slots(2);
  slots[0] = static_cast<uint64_t>(text.size());
  slots[1] = ptr_slot(bytes);
  return slots;
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
  slots[1] = ptr_slot(code_struct);
  return slots;
}

} // namespace

extern "C" {

auto kira_rt_uname() -> uint64_t * {
  const auto info = query::query_uname();
  if (!info) {
    return make_result_err(errno);
  }
  auto *slots = alloc_slots(5);
  slots[0] = ptr_slot(make_str(info->sysname));
  slots[1] = ptr_slot(make_str(info->nodename));
  slots[2] = ptr_slot(make_str(info->release));
  slots[3] = ptr_slot(make_str(info->version));
  slots[4] = ptr_slot(make_str(info->machine));
  return make_result_ok(ptr_slot(slots));
}

auto kira_rt_gethostname() -> uint64_t * {
  const auto name = query::query_hostname();
  if (!name) {
    return make_result_err(errno);
  }
  return make_result_ok(ptr_slot(make_str(*name)));
}

auto kira_rt_processor_name() -> uint64_t * {
  const auto name = query::query_processor_name();
  if (!name) {
    return make_result_err(errno);
  }
  return make_result_ok(ptr_slot(make_str(*name)));
}

auto kira_rt_libc_version() -> uint64_t * {
  const auto info = query::query_libc_version();
  if (!info) {
    return make_result_err(errno);
  }
  auto *slots = alloc_slots(2);
  slots[0] = ptr_slot(make_str(info->name));
  slots[1] = ptr_slot(make_str(info->version));
  return make_result_ok(ptr_slot(slots));
}

auto kira_rt_windows_version() -> uint64_t * {
  const auto info = query::query_windows_version();
  if (!info) {
    return make_result_err(errno);
  }
  auto *slots = alloc_slots(5);
  slots[0] = info->major;
  slots[1] = info->minor;
  slots[2] = info->build;
  slots[3] = info->platform_id;
  slots[4] = ptr_slot(make_str(info->csd_version));
  return make_result_ok(ptr_slot(slots));
}

auto kira_rt_macos_version() -> uint64_t * {
  const auto info = query::query_macos_version();
  if (!info) {
    return make_result_err(errno);
  }
  auto *slots = alloc_slots(5);
  slots[0] = ptr_slot(make_str(info->release));
  slots[1] = ptr_slot(make_str(info->version));
  slots[2] = ptr_slot(make_str(info->dev_stage));
  slots[3] = ptr_slot(make_str(info->non_release_version));
  slots[4] = ptr_slot(make_str(info->machine));
  return make_result_ok(ptr_slot(slots));
}

} // extern "C"
