#include "src/llvm_codegen/runtime.h"

#include <cstdio>
#include <cstdlib>
#include <print>

#include "src/bytecode/panic.h"

// The AOT counterpart of `runtime.cpp`'s throwing `kira_codegen_panic`:
// `kira build` output has no C++ exception handler waiting at the top of
// the (real OS) process, so unwinding isn't the right shape here the way
// it is for `jit_support.h`'s in-process JIT execution. Instead this
// prints a friendly message (CLAUDE.md's "compiler is a teacher" applies
// to a shipped binary's runtime failures too, not just compile-time
// diagnostics) and exits with a fixed nonzero status — 101, matching the
// convention several other languages already use for "the program itself
// signaled a panic," so it's distinguishable from an ordinary `exit(1)`.
//
// Deliberately duplicates `panic_reason_message` (panic.cpp) rather than
// calling it: this is a standalone AOT runtime meant to link against
// nothing but the object `emit_object_file` produces, and calling into
// `panic.cpp` would pull `//src/bytecode:bytecode`'s archive into every
// `kira build` output for one string table — cheaper to keep four
// messages in sync by hand than to grow this runtime's link graph for it.
extern "C" [[noreturn]] void kira_codegen_panic(uint8_t reason) {
  const char *message = "unknown panic";
  switch (static_cast<kira::bytecode::panic_reason>(reason)) {
  case kira::bytecode::panic_reason::integer_overflow:
    message = "integer overflow";
    break;
  case kira::bytecode::panic_reason::integer_divide_by_zero:
    message = "integer divide by zero";
    break;
  case kira::bytecode::panic_reason::explicit_panic:
    message = "explicit panic";
    break;
  case kira::bytecode::panic_reason::stack_overflow:
    message = "stack overflow";
    break;
  case kira::bytecode::panic_reason::index_out_of_bounds:
    message = "index out of bounds";
    break;
  }
  std::println(stderr, "panic: {}", message);
  std::exit(101);
}
