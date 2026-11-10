#pragma once

#include <expected>
#include <optional>
#include <string_view>

#include "src/bytecode/opcodes.h"
#include "src/bytecode/value.h"
#include "src/bytecode_compiler/compile.h"
#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/parser/token.h"

namespace cinder::bytecode_compiler {

// ==========================================================================
//  Scalar encodings shared by the bytecode emitters: how a literal becomes a
//  register image, and which opcode an operator selects. Both emitters (HIR
//  and storage form) read these, so a literal or operator cannot mean
//  something different depending on which path compiled it.
// ==========================================================================

/// Encodes one literal as the register image its resolved `kind` expects.
/// Trusts the checker's work (fit, defaulting) and only reproduces the bit
/// pattern. A negative integer literal is its magnitude with `op_neg` applied
/// afterwards; see `negate_literal`.
[[nodiscard]] auto encode_literal(token_kind lit_kind, std::string_view value,
                                  source_span span, bytecode::numeric_kind kind)
    -> std::expected<bytecode::slot_value, compile_error>;

/// The register image of `-magnitude` for an integer `kind`, folded at compile
/// time. The checker accepts a literal's magnitude one past the type's positive
/// maximum when a negation follows (`-128` as `int8`); negating at run time
/// would trip `op_neg`'s overflow guard on the wrapped magnitude. Signed
/// values are sign-extended to the register width, as the VM stores them.
[[nodiscard]] auto negate_literal(bytecode::slot_value magnitude,
                                  bytecode::numeric_kind kind)
    -> bytecode::slot_value;

/// The ordinary (non-short-circuit) binary opcode for `op`, or `nullopt` for
/// the operators with none (`and`/`or` compile to jumps; ranges, pipes and
/// membership tests have no scalar form).
[[nodiscard]] auto binary_opcode_for(ast::binary_op op)
    -> std::optional<bytecode::opcode>;

/// The binary opcode a compound assignment (`x += y`, ...) desugars to.
[[nodiscard]] auto compound_assign_opcode_for(ast::assign_op op)
    -> std::optional<bytecode::opcode>;

} // namespace cinder::bytecode_compiler
