#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/bytecode/chunk.h"
#include "src/hir/nodes.h"
#include "src/semantic/comptime_backend.h"

namespace cinder::comptime_vm {

/// A compile-time call's callee and everything it reaches, lowered on demand
/// and compiled into one bytecode module.
struct compiled_program {
  bytecode::bytecode_module module;
  /// Index of the called function in `module.functions`.
  std::uint16_t entry_index = 0;
  /// The called function's parameter and return types.
  std::vector<semantic::type_id> param_types;
  semantic::type_id return_type = semantic::k_unknown_type;
  /// Every lowered function by `module::name`, for mapping a failure inside
  /// one back to its declaration.
  std::unordered_map<std::string, const ast::func_decl *> functions;
  /// Each lowered function's `hir_fingerprint`, for `vm_backend::verify`.
  std::unordered_map<const ast::func_decl *, std::string> fingerprints;
};

/// The shape of `fn`'s lowered body as text: every node's kind, type, and
/// the names, operators, literals and call targets it carries. Two lowerings
/// of one function agree exactly when their fingerprints do.
[[nodiscard]] auto hir_fingerprint(hir::hir_function &fn) -> std::string;

/// Lowers `entry` and, transitively, every function its lowered code names,
/// asking `program` to make each ready first, then compiles them together.
/// Fails with `comptime_failure::kind::unsupported` when any needed function
/// cannot be lowered or compiled; the message says which and why.
[[nodiscard]] auto compile_program(semantic::comptime_program &program,
                                   const semantic::comptime_function &entry)
    -> std::expected<compiled_program, semantic::comptime_failure>;

/// What `compile_expression` names the function it makes of the
/// expression. Not a name a declaration can have.
inline constexpr std::string_view k_expression_function = "<compile-time>";

/// Lowers `expr` as a function of its own (`hir::lower_comptime_expression`)
/// and, like `compile_program`, everything it reaches, then compiles them
/// together.
[[nodiscard]] auto compile_expression(semantic::comptime_program &program,
                                      const semantic::comptime_expression &expr)
    -> std::expected<compiled_program, semantic::comptime_failure>;

} // namespace cinder::comptime_vm
