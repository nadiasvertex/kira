#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "src/hir/nodes.h"
#include "src/parser/source_location.h"

namespace cinder::hir {

/// The most frame-local `uninit[T, N]` storage one function may use, in
/// bytes. A language rule, not a backend property: both tiers reject exactly
/// the programs this check rejects, and accept everything under it (the
/// bytecode tier reserves the storage on the heap per frame, the LLVM tier
/// with an entry-block `alloca` behind stack probes). 1 MiB keeps a frame
/// well inside the 8 MiB default main-thread stack.
inline constexpr uint64_t k_max_frame_stack_bytes = uint64_t{1} << 20;

/// One `uninit` buffer counted against a frame.
struct frame_buffer {
  source_span span;
  uint64_t byte_size = 0;
};

/// A function (or lambda body) whose `uninit` buffers together exceed
/// `k_max_frame_stack_bytes`.
struct frame_budget_violation {
  /// Ready to print: "`name`", or "a lambda in `name`".
  std::string frame_name;
  source_span frame_span;
  uint64_t total_bytes = 0;
  std::vector<frame_buffer> buffers;
};

/// Every frame in `module` over the budget, in declaration order.
///
/// A frame is a function body or a lambda body: a lambda's buffers belong to
/// the lambda's own frame, not its enclosing function's, on both tiers. The
/// total is the plain sum of buffer sizes — alignment padding is a layout
/// detail that differs between tiers and is bounded by a few bytes per
/// buffer, so it is not part of the rule.
///
/// Run on freshly lowered HIR. `inline_small_calls` never copies a callee
/// that contains a buffer into its caller, so inlining cannot push a frame
/// over the budget after this check has passed.
[[nodiscard]] auto find_frame_budget_violations(hir_module &module)
    -> std::vector<frame_budget_violation>;

} // namespace cinder::hir
