#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/bytecode/chunk.h"
#include "src/hir/nodes.h"
#include "src/hir/storage_program.h"
#include "src/semantic/comptime_backend.h"

namespace cinder::comptime_vm {

/// How compile-time code is lowered. It is built by the core adapter
/// (`core::adapt_function`, `adapt_expression`, `adapt_static`) and lowered
/// from the core (`hir::lower_core_comptime_function`, `lower_core_static`).
struct comptime_lowering {
  /// Run `core::verify` over the core of every program built, and refuse a
  /// program that breaks a rule. On with `CINDER_CORE_VERIFY`.
  bool verify_core = false;

  /// The mode the environment asks for.
  [[nodiscard]] static auto from_environment() -> comptime_lowering;
};

/// Where a function lowered for compile-time code is named, so it can be
/// built again from the final results (`vm_backend::verify`).
struct lowered_name {
  std::string module;
  std::string name;
};

/// A compile-time call's callee and everything it reaches, lowered on demand
/// and compiled into one bytecode module.
struct compiled_program {
  /// The lowered functions, and their storage form (with the layouts the
  /// values passed in and out are read with), which `module` was compiled
  /// from.
  hir::ptr_vec<hir::hir_module> lowered;
  std::unique_ptr<hir::storage_program> storage;
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
  /// Each lowered function's module and name.
  std::unordered_map<const ast::func_decl *, lowered_name> names;
};

/// The functions a bare name found while compile-time code was built from
/// the core (`core::function_lookup`), by `module` and `name`, so the same
/// function can be built again from the final results, which hold no
/// program index.
using lookup_memo =
    std::unordered_map<std::string, const ast::func_decl *>;

/// The shape of `fn`'s lowered body as text: every node's kind, type, and
/// the names, operators, literals and call targets it carries. Two lowerings
/// of one function agree exactly when their fingerprints do.
[[nodiscard]] auto hir_fingerprint(hir::hir_function &fn) -> std::string;

/// Lowers `entry` and, transitively, every function its lowered code names,
/// asking `program` to make each ready first, then compiles them together.
/// Fails with `comptime_failure::kind::unsupported` when any needed function
/// cannot be lowered or compiled; the message says which and why.
[[nodiscard]] auto compile_program(semantic::comptime_program &program,
                                   const semantic::comptime_function &entry,
                                   const comptime_lowering &mode = {},
                                   lookup_memo *memo = nullptr)
    -> std::expected<compiled_program, semantic::comptime_failure>;

/// What `compile_expression` names the function it makes of the
/// expression. Not a name a declaration can have.
inline constexpr std::string_view k_expression_function = "<compile-time>";

/// Lowers `expr` as a function of its own (`core::adapt_expression`) and,
/// like `compile_program`, everything it reaches, then compiles them
/// together.
[[nodiscard]] auto compile_expression(semantic::comptime_program &program,
                                      const semantic::comptime_expression &expr,
                                      const comptime_lowering &mode = {},
                                      lookup_memo *memo = nullptr)
    -> std::expected<compiled_program, semantic::comptime_failure>;

/// `decl` lowered for compile-time code from the core, built from `checked`
/// as `compile_program` builds it, with the bare names `memo` recorded:
/// what `vm_backend::verify` compares against. The error says why not.
[[nodiscard]] auto lower_again(const ast::func_decl &decl,
                                         const lowered_name &name,
                                         semantic::checked_types &checked,
                                         const lookup_memo &memo)
    -> std::expected<hir::ptr<hir::hir_function>, std::string>;

} // namespace cinder::comptime_vm
