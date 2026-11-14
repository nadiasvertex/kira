#include "src/hir/storage_frame.h"

#include <algorithm>
#include <format>
#include <limits>

namespace cinder::hir {

namespace {

constexpr auto k_overflow = std::numeric_limits<uint64_t>::max();

/// `value` rounded up to a multiple of `align`, or `k_overflow`.
[[nodiscard]] auto align_up(uint64_t value, uint64_t align) -> uint64_t {
  if (align <= 1) {
    return value;
  }
  const auto rest = value % align;
  if (rest == 0) {
    return value;
  }
  const auto pad = align - rest;
  return value > k_overflow - pad ? k_overflow : value + pad;
}

} // namespace

auto lay_out_frame(std::span<const frame_request> requests) -> uint64_t {
  auto total = uint64_t{0};
  for (const auto &request : requests) {
    if (request.size == 0) {
      continue;
    }
    total = align_up(total, request.align);
    if (total == k_overflow || request.size > k_overflow - 1 - total) {
      return k_overflow;
    }
    total += request.size;
  }
  return total;
}

auto plan_frame(const storage_function &function, storage_env &env)
    -> std::expected<frame_plan, std::string> {
  auto plan = frame_plan{.function = function.name};
  auto requests = std::vector<frame_request>{};
  for (const auto &slot : function.slots) {
    if (slot.cls != storage_class::frame_local &&
        slot.cls != storage_class::outgoing_argument) {
      continue;
    }
    const auto size = env.layouts.size_of(slot.type);
    if (!size.has_value()) {
      return std::unexpected(size.error().message);
    }
    if (*size == 0) {
      continue;
    }
    const auto align = env.layouts.align_of(slot.type);
    if (!align.has_value()) {
      return std::unexpected(align.error().message);
    }
    requests.push_back(frame_request{.size = *size, .align = *align});
    plan.contributors.push_back(frame_contributor{
        .slot = slot.id, .name = slot.name, .span = slot.span, .bytes = *size});
  }
  plan.bytes = lay_out_frame(requests);
  return plan;
}

auto describe_frame_overflow(const frame_plan &plan, uint64_t limit,
                             size_t shown) -> std::string {
  if (plan.within(limit)) {
    return {};
  }
  auto ordered = plan.contributors;
  std::ranges::stable_sort(
      ordered, [](const auto &a, const auto &b) { return a.bytes > b.bytes; });
  auto text = plan.bytes == k_overflow
                  ? std::format("`{}` needs more frame storage than can be "
                                "counted; the limit is {} bytes",
                                plan.function, limit)
                  : std::format("`{}` needs {} bytes of frame storage; the "
                                "limit is {} bytes",
                                plan.function, plan.bytes, limit);
  text += "; the largest are";
  for (size_t i = 0; i < ordered.size() && i < shown; ++i) {
    text += std::format("{} {} ({} bytes)", i == 0 ? "" : ",",
                        ordered[i].name.empty() ? "a temporary"
                                                : "`" + ordered[i].name + "`",
                        ordered[i].bytes);
  }
  return text;
}

} // namespace cinder::hir
