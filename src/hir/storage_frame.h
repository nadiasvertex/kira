#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "src/hir/frame_budget.h"
#include "src/hir/storage.h"
#include "src/parser/source_location.h"

namespace cinder::hir {

/// The bytes one slot asks of its function's frame.
struct frame_request {
  uint64_t size = 0;
  uint64_t align = 1;
};

/// Where a frame's bytes went.
struct frame_contributor {
  storage_id slot = k_invalid_storage;
  /// The source name, empty for a temporary.
  std::string name;
  source_span span;
  /// The slot's size, without alignment padding.
  uint64_t bytes = 0;
};

/// The frame bytes of one function in storage form.
///
/// A frame holds the `frame_local` and `outgoing_argument` slots of nonzero
/// size, each at its own alignment, in slot order. Parameters, the caller's
/// result, globals, owner fields, closure environments, and coroutine frames
/// live in storage somebody else owns and add nothing. No two slots share
/// bytes, so a loop cannot grow its function's frame.
struct frame_plan {
  std::string function;
  /// Total bytes with alignment padding; `UINT64_MAX` when the sum overflowed.
  uint64_t bytes = 0;
  std::vector<frame_contributor> contributors;

  [[nodiscard]] auto within(uint64_t limit) const -> bool { return bytes <= limit; }
};

/// The padded total of `requests` placed in order, or `UINT64_MAX` if any step
/// overflows. Arithmetic is checked, never wrapped.
[[nodiscard]] auto lay_out_frame(std::span<const frame_request> requests)
    -> uint64_t;

/// The frame plan of `function` under the concrete layouts of `env`. Fails with
/// the layout error's message when a slot's type has no layout.
[[nodiscard]] auto plan_frame(const storage_function &function, storage_env &env)
    -> std::expected<frame_plan, std::string>;

/// Why `plan` is over `limit`: the total, the limit, and the largest
/// contributors (up to `shown`), largest first. Empty when it is within.
[[nodiscard]] auto describe_frame_overflow(const frame_plan &plan, uint64_t limit,
                                           size_t shown = 3) -> std::string;

} // namespace cinder::hir
