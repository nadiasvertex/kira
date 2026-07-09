#pragma once

#include <string>

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
