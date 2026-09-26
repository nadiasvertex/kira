#include "src/llvm_codegen/runtime.h"

#include "src/bytecode/panic.h"

// `raise_panic` rather than a bare `throw`: a fatal reason (`is_fatal`,
// `src/bytecode/panic.h`) terminates instead of unwinding, so JIT-executed
// code agrees with the bytecode VM and with an AOT-built binary about what
// an out-of-range index does.
extern "C" [[noreturn]] void cinder_codegen_panic(uint8_t reason) {
  cinder::bytecode::raise_panic(
      static_cast<cinder::bytecode::panic_reason>(reason));
}
