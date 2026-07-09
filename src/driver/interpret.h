#pragma once

#include <span>
#include <string>
#include <string_view>

#include "src/bytecode/value.h"
#include "src/hir/nodes.h"
#include "src/semantic/types.h"

namespace kira::driver {

/// Outcome of executing `cli_config::run_function` when `cli_config::run` is
/// set. Bytecode compilation only covers `spec/codegen-design.md` increment
/// 1's narrow scalar/control-flow subset (see
/// `src/bytecode_compiler/compile.h`), so this is a distinct, best-effort
/// step layered on top of `compile_report::hir_modules` rather than folded
/// into `error_count` — a program that compiles and lowers cleanly may still
/// be unrunnable yet (heap types, generics, ...).
struct run_outcome {
  bool succeeded = false; ///< True when the function was found, compiled, and
                          ///< ran to completion without panicking.
  std::string message;    ///< Rendered return value on success; the reason
                          ///< execution didn't happen or panicked otherwise.
};

} // namespace kira::driver

namespace kira::driver {

/// Renders a VM return value for `--run`'s output, using the function's
/// checked return type to pick how `slot_value`'s untagged union should be
/// read back (mirrors `bytecode_compiler::encode_literal`'s reverse
/// direction).
///
/// @param types Checked type table the function's `return_type` indexes
/// into.
/// @param return_type Checked return type of the executed function.
/// @param value Raw VM result to render.
[[nodiscard]] auto render_run_value(const semantic::type_table &types,
                                    semantic::type_id return_type,
                                    const bytecode::slot_value &value)
    -> std::string;

/// Compiles `modules` to bytecode and executes `function_name` with no
/// arguments, producing the `--run` outcome shown in the CLI summary.
/// `--run` targets exactly `spec/codegen-design.md` increment 1's subset —
/// this project has no CLI-level argument-marshalling story yet, so only
/// zero-parameter functions are runnable — and fails closed (a message, not
/// a crash) on every other reason execution can't proceed.
///
/// @param modules The entry module (`modules.front()`, which owns
/// `function_name`) plus every module `hir::find_reachable_modules` found it
/// transitively needs — passed straight through to
/// `bytecode_compiler::compile_module`'s multi-module overload.
/// @param types Checked type table the modules' HIR indexes into.
/// @param function_name Name of the zero-argument function to execute.
[[nodiscard]] auto
run_hir_module(std::span<const hir::hir_module *const> modules,
               const semantic::type_table &types,
               std::string_view function_name) -> run_outcome;

} // namespace kira::driver
