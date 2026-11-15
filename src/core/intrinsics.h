#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "src/semantic/checker_results.h"
#include "src/semantic/ids.h"
#include "src/semantic/type_table.h"

/// The compiler's own operations: the `intrinsic` callees the elaborator emits
/// for builtin addressing, views, cells, raw pointers, layout queries, string
/// iteration and task forms. Each has a fixed shape (operand kinds, how the
/// result relates to the operands and to `generic_args`), which the verifier
/// checks under rule C5. Any other intrinsic name must be declared by an
/// `intrinsic def` of the program.
namespace cinder::core {

/// Whether `name` is one of the compiler's own operations.
[[nodiscard]] auto is_compiler_operation(std::string_view name) -> bool;

/// Checks a call to the compiler operation `name`: `signature` is the callee's
/// `fn` type and `generic_args` its generic arguments. Returns a message when
/// the shape is wrong, and nothing when it is right. A `signature` that is not
/// a `fn` type is not reported here (rule C1 reports it). `name` must satisfy
/// `is_compiler_operation`.
[[nodiscard]] auto check_intrinsic(std::string_view name, semantic::type_id signature,
                                   std::span<const semantic::type_id> generic_args,
                                   const semantic::checked_types &checked)
    -> std::optional<std::string>;

/// Whether `awaited` is what awaiting `task` gives: for `task[T, E, C]` with
/// `E` neither absent nor `never`, `result[T, E]`; otherwise `T`. False when
/// `task` is not a task.
[[nodiscard]] auto task_awaits(const semantic::type_table &types, semantic::type_id task,
                               semantic::type_id awaited) -> bool;

} // namespace cinder::core
