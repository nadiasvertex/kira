#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>

#include "src/core/nodes.h"
#include "src/hir/drop_schedule.h"
#include "src/hir/lower_core.h"
#include "src/hir/nodes.h"

/// Internal to `lower_core.h`: the lowerer of one core function, one method
/// per core node kind. Split across `lower_core.cpp` (functions and the
/// substitution), `lower_core_expr.cpp` (expressions, places, calls) and
/// `lower_core_stmt.cpp` (statements and blocks).
namespace cinder::hir {

template <typename T> using core_result = std::expected<T, unsupported>;

/// The name of a core node kind, as an `unsupported` reports it.
[[nodiscard]] auto core_kind_name(core::node_kind kind) -> std::string;

class core_lowerer {
public:
  core_lowerer(const semantic::checked_types &checked,
               const lowering_options &options, std::string_view core_module,
               std::string_view hir_module)
      : checked_(checked), options_(options), core_module_(core_module),
        hir_module_(hir_module) {}

  [[nodiscard]] auto function(const core::function &source)
      -> core_result<ptr<hir_function>>;

  // ---- expressions (lower_core_expr.cpp) ---------------------------------
  [[nodiscard]] auto expr(const core::expr &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto literal(const core::literal &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto use(const core::use &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto borrow(const core::borrow &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto call(const core::call &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto intrinsic_call(const core::call &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto primitive(const core::primitive &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto cast(const core::cast &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto aggregate(const core::aggregate_init &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto variant(const core::variant_init &source)
      -> core_result<ptr<hir_expr>>;
  /// An `if`, typed `type` in HIR (see `hir_type_in`).
  [[nodiscard]] auto if_expr(const core::if_expr &source, type_id type)
      -> core_result<ptr<hir_if>>;
  /// The place `source` names, as the chain of projections HIR reads and
  /// writes through.
  [[nodiscard]] auto place(const core::place &source, source_span span)
      -> core_result<ptr<hir_expr>>;
  /// The callee reference of a call to a declared function.
  [[nodiscard]] auto function_callee(const core::callee &target,
                                     source_span span) -> ptr<hir_expr>;

  // ---- statements (lower_core_stmt.cpp) ----------------------------------
  /// `source` as a HIR block of type `type`: a result becomes the trailing
  /// expression statement.
  [[nodiscard]] auto block(const core::block &source, type_id type)
      -> core_result<ptr<hir_block>>;
  /// Appends the HIR statements for `source`. `tail_type` is set for the
  /// last statement of a block, whose `if` is the block's tail.
  auto stmt_into(const core::stmt &source, ptr_vec<hir_node> &out,
                 std::optional<type_id> tail_type) -> core_result<void>;
  auto let_into(const core::let &source, ptr_vec<hir_node> &out)
      -> core_result<void>;
  auto assign_into(const core::assign &source, ptr_vec<hir_node> &out)
      -> core_result<void>;
  auto expr_stmt_into(const core::expr_stmt &source, ptr_vec<hir_node> &out,
                      std::optional<type_id> tail_type) -> core_result<void>;
  auto while_into(const core::while_loop &source, ptr_vec<hir_node> &out)
      -> core_result<void>;
  auto jump_into(const core::jump &source, ptr_vec<hir_node> &out)
      -> core_result<void>;

  // ---- helpers -----------------------------------------------------------
  /// `type` as HIR holds it: an opaque type is the type it stands for.
  [[nodiscard]] auto hir_type(type_id type) const -> type_id;
  /// The type of a block or `if` in statement position: a `unit` one is
  /// untyped in HIR, as the old lowerer leaves it.
  [[nodiscard]] auto hir_type_in_statement(type_id type) const -> type_id;
  [[nodiscard]] auto mint() -> symbol_id { return next_symbol_++; }
  /// A fresh HIR symbol for the core local `symbol`, named `name`.
  auto declare(symbol_id symbol, const std::string &name) -> symbol_id;
  [[nodiscard]] static auto refuse(std::string kind, source_span span)
      -> std::unexpected<unsupported> {
    return std::unexpected(unsupported{.kind = std::move(kind), .span = span});
  }
  [[nodiscard]] static auto refuse(const core::node &node)
      -> std::unexpected<unsupported> {
    return refuse(core_kind_name(node.kind), node.span);
  }

private:
  const semantic::checked_types &checked_;
  const lowering_options &options_;
  std::string core_module_;
  std::string hir_module_;
  /// Each core local's HIR symbol.
  std::unordered_map<symbol_id, symbol_id> symbols_;
  /// Each local's name, for the references to it (diagnostics only).
  std::unordered_map<symbol_id, std::string> names_;
  /// One symbol per function a call names, as the old lowerer mints them.
  std::unordered_map<std::string, symbol_id> callee_symbols_;
  symbol_id next_symbol_ = 1;
  drop_schedule drops_;
};

} // namespace cinder::hir
