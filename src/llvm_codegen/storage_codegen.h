#pragma once

#include <expected>
#include <span>
#include <string>

#include "src/hir/storage.h"
#include "src/llvm_codegen/codegen.h"

namespace cinder::llvm_codegen {

// ==========================================================================
//  LLVM from the shared storage form (representation v2).
//
//  `compile_module` in `codegen.h` builds IR from typed HIR, where every
//  aggregate is a pointer to a heap block. This compiler builds it from
//  `hir::storage_function`s instead, the form the bytecode compiler reads
//  (`bytecode_compiler/storage_compile.h`). An aggregate or descriptor is
//  inline bytes, and a call builds its result in storage the caller supplies.
//
//  Where values live:
//
//  - Every slot a function owns is an entry-block `alloca` of exactly its
//    layout size and alignment, addressed as bytes. A scalar is loaded and
//    stored at its own width; LLVM's SROA turns the ones nothing takes the
//    address of back into registers. There is no second copy elision here:
//    the storage form has already built values in their final place.
//  - A parameter that travels by address, and the hidden result, are `ptr`
//    arguments to the caller's bytes. The callee owns a by-address parameter
//    for the call.
//
//  Internal signature, matching `hir::abi_signature::operands()`: the hidden
//  result address first (when the result is indirect), then each
//  non-zero-sized parameter. A scalar, address, or handle is passed by value
//  (`ptr` for the last two); a descriptor or aggregate is a `ptr`. The return
//  type is `void` unless the result is a scalar, address, or handle.
//
//  Alignment. Frame slots and the caller storage behind a by-address operand
//  are assumed aligned to their type's layout alignment, which the ABI
//  promises. A packed projection is accessed at the alignment its offset
//  allows, and anything reached through a loaded address is accessed with
//  alignment 1: the referent's alignment is not inferred from its type.
//
//  The two representations must not meet in one program, so a module built
//  here calls only functions built here. Nothing in the driver selects this
//  compiler yet.
// ==========================================================================

/// The prefix of the entry thunk emitted for each function when
/// `storage_codegen_options::entry_thunks` is set. The thunk for `f` is
///
///     void __cinder_entry_f(const uint64_t *args, void *result)
///
/// `args` holds one 8-byte slot per non-zero-sized source parameter, in
/// order: a by-address parameter's slot holds its address, any other slot
/// holds the value in its low bytes at the parameter's own width. `result` is
/// the storage a by-address result is built in, or where a scalar, address, or
/// handle result is stored; it is unused for a zero-sized result. A host calls
/// the thunk instead of the function so it needs no per-signature call shape.
inline constexpr const char *k_storage_entry_prefix = "__cinder_entry_";

struct storage_codegen_options {
  /// Emit an entry thunk beside every function (see above).
  bool entry_thunks = false;
};

/// Compiles a program of several modules into one verified `llvm::Module`
/// named after the first, the entry module. The entry module's functions keep
/// their names as symbols; a function of any other module is `module::name`
/// (`hir::program_function_name`), so modules may share function names and a
/// call into another module is an ordinary direct call. Entry thunks are
/// named after the symbol they wrap.
///
/// Each function should already satisfy `hir::verify`: this compiler relies on
/// it for initialization, ownership and place validity. It fails closed with a
/// named reason on anything it cannot place: `drop_in_place`, a slot that does
/// not live in the frame or the caller, and a 128-bit scalar.
[[nodiscard]] auto
compile_storage_module(std::span<const hir::storage_module *const> modules,
                       hir::storage_env &env,
                       const storage_codegen_options &options = {})
    -> std::expected<compiled_module, codegen_error>;

/// Compiles one module as a program of its own.
[[nodiscard]] auto
compile_storage_module(const hir::storage_module &module, hir::storage_env &env,
                       const storage_codegen_options &options = {})
    -> std::expected<compiled_module, codegen_error>;

} // namespace cinder::llvm_codegen
