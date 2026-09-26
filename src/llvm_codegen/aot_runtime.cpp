#include "src/llvm_codegen/runtime.h"

#include <cstdio>
#include <cstdlib>
#include <print>

#include "src/bytecode/panic.h"

// The AOT counterpart of `runtime.cpp`'s throwing `cinder_codegen_panic`:
// `cinder build` output has no C++ exception handler waiting at the top of
// the (real OS) process, so unwinding isn't the right shape here the way
// it is for `jit_support.h`'s in-process JIT execution. Instead this
// prints a friendly message (CLAUDE.md's "compiler is a teacher" applies
// to a shipped binary's runtime failures too, not just compile-time
// diagnostics) and exits with `k_panic_exit_code` — see its doc comment
// (`src/bytecode/panic.h`) for why 101 and not `abort`'s 134.
//
// Deliberately duplicates `panic_reason_message` (panic.cpp) rather than
// calling it: this is a standalone AOT runtime meant to link against
// nothing but the object `emit_object_file` produces, and calling into
// `panic.cpp` would pull `//src/bytecode:bytecode`'s archive into every
// `cinder build` output for one string table — cheaper to keep this handful of
// messages in sync by hand than to grow this runtime's link graph for it.
extern "C" [[noreturn]] void cinder_codegen_panic(uint8_t reason) {
  const char *message;
  switch (static_cast<cinder::bytecode::panic_reason>(reason)) {
  case cinder::bytecode::panic_reason::integer_overflow:
    message = "integer overflow";
    break;
  case cinder::bytecode::panic_reason::integer_divide_by_zero:
    message = "integer divide by zero";
    break;
  case cinder::bytecode::panic_reason::explicit_panic:
    message = "explicit panic";
    break;
  case cinder::bytecode::panic_reason::stack_overflow:
    message = "stack overflow";
    break;
  // Reached here, unlike in the in-process handler (`runtime.cpp`), which
  // routes through `raise_panic` and terminates on a fatal reason
  // (`bytecode::is_fatal`) before any message lookup. This runtime
  // terminates for every reason anyway, so the two agree on what a bounds
  // violation prints and what status it leaves.
  case cinder::bytecode::panic_reason::index_out_of_bounds:
    message = "index out of bounds";
    break;
  case cinder::bytecode::panic_reason::precondition_violated:
    message = "precondition violated";
    break;
  case cinder::bytecode::panic_reason::postcondition_violated:
    message = "postcondition violated";
    break;
  case cinder::bytecode::panic_reason::invariant_violated:
    message = "invariant violated";
    break;
  default:
    message = "unknown panic";
    break;
  }
  std::println(stderr, "panic: {}", message);
  std::exit(cinder::bytecode::k_panic_exit_code);
}
