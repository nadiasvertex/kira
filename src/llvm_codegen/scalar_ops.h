#pragma once

#include <expected>
#include <string_view>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wshadow"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#pragma clang diagnostic pop

#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/llvm_codegen/codegen.h"
#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/parser/token.h"

namespace cinder::llvm_codegen {

// ==========================================================================
//  Scalar operations shared by both LLVM compilers.
//
//  `codegen.cpp` lowers typed HIR and `storage_codegen.cpp` lowers the shared
//  storage form. Neither representation changes what `a + b` means on two
//  `int32`s, so the checked arithmetic, literals, negation, and casts live
//  here once. Every panic is the same closed `bytecode::panic_reason` the
//  bytecode VM raises for the same condition.
// ==========================================================================

/// The LLVM type a scalar of `kind` has as an SSA value. A `bool` is `i1`
/// here; its in-memory form is a byte, which callers handle where they load
/// and store.
[[nodiscard]] auto llvm_type_for(llvm::LLVMContext &ctx,
                                 bytecode::numeric_kind kind) -> llvm::Type *;

/// Emits scalar instructions through a caller-owned `IRBuilder`. Checked
/// operations split the current block: the failing edge calls the panic
/// function, and emission continues in the passing block.
class scalar_builder {
public:
  /// `builder` and `panic_fn` must outlive this object. The function being
  /// built is whatever function the builder's insertion block belongs to.
  scalar_builder(llvm::IRBuilder<> &builder, llvm::Function *panic_fn)
      : builder_(builder), panic_fn_(panic_fn) {}

  /// Branches to a panic with `reason` when `panic_if_true` holds; emission
  /// continues in the block taken otherwise.
  auto guard_panic(llvm::Value *panic_if_true, bytecode::panic_reason reason)
      -> void;

  /// A scalar literal of `kind`: `true`/`false`, integers (including 128-bit),
  /// floats, and characters.
  [[nodiscard]] auto literal(token_kind lit_kind, std::string_view text,
                             source_span span, bytecode::numeric_kind kind)
      -> std::expected<llvm::Value *, codegen_error>;

  /// An integer literal's negation, folded here: the magnitude of `-128` as
  /// an `int8` does not fit the type, so negating it at run time would panic.
  [[nodiscard]] auto negated_literal(std::string_view text, source_span span,
                                     bytecode::numeric_kind kind)
      -> std::expected<llvm::Value *, codegen_error>;

  /// Arithmetic, comparison, bitwise, and shift operators on two values of
  /// `kind`. Short-circuit `and`/`or` are control flow and not handled here.
  [[nodiscard]] auto binary(ast::binary_op op, bytecode::numeric_kind kind,
                            llvm::Value *lhs, llvm::Value *rhs,
                            source_span span)
      -> std::expected<llvm::Value *, codegen_error>;

  /// `-value`: floats negate plainly, signed integers panic on the minimum.
  [[nodiscard]] auto negate(bytecode::numeric_kind kind, llvm::Value *value)
      -> llvm::Value *;

  /// A numeric conversion with the bytecode VM's rules: integers extend from
  /// the source's signedness, then truncate to the destination width.
  [[nodiscard]] auto cast(bytecode::numeric_kind from,
                          bytecode::numeric_kind to, llvm::Value *value)
      -> llvm::Value *;

private:
  [[nodiscard]] auto checked_arith(llvm::Intrinsic::ID id, llvm::Value *lhs,
                                   llvm::Value *rhs) -> llvm::Value *;
  [[nodiscard]] auto checked_div(bool is_signed, int bits, llvm::Value *lhs,
                                 llvm::Value *rhs, bool is_mod)
      -> llvm::Value *;
  [[nodiscard]] auto checked_mul_sat(bool is_signed, int bits, llvm::Value *lhs,
                                     llvm::Value *rhs) -> llvm::Value *;
  [[nodiscard]] auto checked_shift(bool is_shl, bool is_signed, int bits,
                                   llvm::Value *lhs, llvm::Value *rhs)
      -> llvm::Value *;

  llvm::IRBuilder<> &builder_;
  llvm::Function *panic_fn_;
};

} // namespace cinder::llvm_codegen
