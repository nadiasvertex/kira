#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "src/llvm_codegen/codegen.h"

namespace kira::llvm_codegen {

/// Why `emit_object_file` refused to produce an object file.
struct aot_error {
  std::string message;
};

/// Synthesizes a standard C-ABI `int main(void)` entry point in `module`
/// that calls `entry_function_name` (which must be zero-argument, matching
/// the same convention `cli.cpp`'s `--run` already uses — this increment
/// has no argv-marshalling story either) and translates its result into a
/// process exit code — `unit` becomes `0`, `bool`/an integer kind is
/// widened/truncated to `int`, and a float-returning entry function is
/// rejected outright (not a sensible exit code) — then runs `module`
/// through `TargetMachine::addPassesToEmitFile` to write a native
/// relocatable object file to `object_path`.
///
/// This produces a plain `.o` with no reference to libLLVM's own runtime
/// (`spec/codegen-design.md` Decision 5) — the caller is responsible for
/// linking it (with a system linker and Kira's native runtime support
/// library, see `aot_runtime.h`) into a standalone executable; this
/// function only emits the object file.
[[nodiscard]] auto emit_object_file(compiled_module module,
                                    std::string_view entry_function_name,
                                    const std::string &object_path)
    -> std::expected<void, aot_error>;

} // namespace kira::llvm_codegen
