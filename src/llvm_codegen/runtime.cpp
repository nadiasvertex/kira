#include "src/llvm_codegen/runtime.h"

#include "src/bytecode/panic.h"

extern "C" [[noreturn]] void kira_codegen_panic(uint8_t reason) {
  throw kira::bytecode::panic_error(
      static_cast<kira::bytecode::panic_reason>(reason));
}
