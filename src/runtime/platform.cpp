#include "src/runtime/platform.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>

#include "src/runtime/allocator.h"
#include "src/runtime/platform_query.h"
#include "src/runtime/result_out.h"

namespace {

namespace query = cinder::runtime::platform_query;

} // namespace

extern "C" {

// The `_into` entry points (`src/runtime/result_out.h`): the `result` is built
// in the caller's storage, with each `str` member an inline descriptor.
// Offsets follow `value_layout.h`; `value_layout_test` checks them against the
// declarations in `src/std/intrinsics.cn`.

namespace out = cinder::runtime::out;

void cinder_rt_uname_into(void *result) {
  constexpr auto size = out::result_size(5 * out::k_descriptor_size);
  const auto info = query::query_uname();
  if (!info) {
    out::write_err(result, size, errno);
    return;
  }
  out::begin_ok(result, size);
  const auto payload = out::k_result_payload_offset;
  out::store_str(result, payload + 0 * out::k_descriptor_size, info->sysname);
  out::store_str(result, payload + 1 * out::k_descriptor_size, info->nodename);
  out::store_str(result, payload + 2 * out::k_descriptor_size, info->release);
  out::store_str(result, payload + 3 * out::k_descriptor_size, info->version);
  out::store_str(result, payload + 4 * out::k_descriptor_size, info->machine);
}

void cinder_rt_gethostname_into(void *result) {
  const auto name = query::query_hostname();
  if (!name) {
    out::write_err(result, out::k_result_str_size, errno);
    return;
  }
  out::begin_ok(result, out::k_result_str_size);
  out::store_str(result, out::k_result_payload_offset, *name);
}

void cinder_rt_processor_name_into(void *result) {
  const auto name = query::query_processor_name();
  if (!name) {
    out::write_err(result, out::k_result_str_size, errno);
    return;
  }
  out::begin_ok(result, out::k_result_str_size);
  out::store_str(result, out::k_result_payload_offset, *name);
}

void cinder_rt_libc_version_into(void *result) {
  constexpr auto size = out::result_size(2 * out::k_descriptor_size);
  const auto info = query::query_libc_version();
  if (!info) {
    out::write_err(result, size, errno);
    return;
  }
  out::begin_ok(result, size);
  const auto payload = out::k_result_payload_offset;
  out::store_str(result, payload, info->name);
  out::store_str(result, payload + out::k_descriptor_size, info->version);
}

void cinder_rt_windows_version_into(void *result) {
  // `winver_raw`: four `uint32` at 0, 4, 8, 12, then the `str` at 16.
  constexpr auto size = out::result_size(32);
  const auto info = query::query_windows_version();
  if (!info) {
    out::write_err(result, size, errno);
    return;
  }
  out::begin_ok(result, size);
  const auto payload = out::k_result_payload_offset;
  out::store<uint32_t>(result, payload + 0, static_cast<uint32_t>(info->major));
  out::store<uint32_t>(result, payload + 4, static_cast<uint32_t>(info->minor));
  out::store<uint32_t>(result, payload + 8, static_cast<uint32_t>(info->build));
  out::store<uint32_t>(result, payload + 12,
                       static_cast<uint32_t>(info->platform_id));
  out::store_str(result, payload + 16, info->csd_version);
}

void cinder_rt_macos_version_into(void *result) {
  constexpr auto size = out::result_size(5 * out::k_descriptor_size);
  const auto info = query::query_macos_version();
  if (!info) {
    out::write_err(result, size, errno);
    return;
  }
  out::begin_ok(result, size);
  const auto payload = out::k_result_payload_offset;
  out::store_str(result, payload + 0 * out::k_descriptor_size, info->release);
  out::store_str(result, payload + 1 * out::k_descriptor_size, info->version);
  out::store_str(result, payload + 2 * out::k_descriptor_size, info->dev_stage);
  out::store_str(result, payload + 3 * out::k_descriptor_size,
                 info->non_release_version);
  out::store_str(result, payload + 4 * out::k_descriptor_size, info->machine);
}

} // extern "C"
