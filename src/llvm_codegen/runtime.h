#pragma once

#include <cstdint>

namespace kira::llvm_codegen {

/// Called from generated IR wherever the bytecode VM would `throw
/// panic_error` (checked-arithmetic overflow, divide-by-zero, an out-of-
/// range shift amount, ...) — `reason` is a `bytecode::panic_reason` cast to
/// `uint8_t` (matching the value `codegen.cpp` bakes into the call as an
/// `i8` constant, since LLVM IR has no direct handle on a C++ enum type).
/// `[[noreturn]]`: `codegen.cpp` always places an `unreachable` terminator
/// immediately after the call in generated IR, so this must never return.
///
/// Declared `extern "C"` so it has an unmangled name generated IR can call
/// by name (`kPanicSymbolName`, codegen.h), and so it resolves via ordinary
/// process-symbol lookup once the JIT looks it up (`jit_support.h`) — this
/// is a test/JIT-execution concern only, not part of the AOT runtime
/// support library `spec/codegen-design.md` increment 4 will eventually add.
extern "C" [[noreturn]] void kira_codegen_panic(uint8_t reason);

} // namespace kira::llvm_codegen
