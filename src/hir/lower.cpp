#include "src/hir/lower.h"

#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/hir/ids.h"
#include "src/hir/nodes.h"
#include "src/parser/ast.h"
#include "src/semantic/types.h"

namespace kira::hir {

namespace {

using semantic::checked_types;
using semantic::k_error_type;
using semantic::k_unknown_type;
using semantic::type_id;
using semantic::type_kind;

[[nodiscard]] auto fail(lowering_error_kind kind, source_span span,
                        std::string message)
    -> std::unexpected<lowering_error> {
  return std::unexpected(lowering_error{
      .kind = kind, .span = span, .message = std::move(message)});
}

/// Whether `param` is the leading `self`/`mut self` receiver — the same
/// shape `semantic::check.cpp`'s own `is_self_param` checks for, redone
/// here since lowering has no access to that checker-private helper.
[[nodiscard]] auto is_self_param(const ast::param &param) -> bool {
  if (param.pattern == nullptr ||
      param.pattern->kind != ast::node_kind::binding_pattern) {
    return false;
  }
  return dynamic_cast<const ast::binding_pattern &>(*param.pattern).name ==
         "self";
}

/// Wraps a freshly-built derived node into the `ptr<hir_expr>` result type
/// every expression-lowering helper returns.
template <typename T>
[[nodiscard]] auto ok_expr(ptr<T> node)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  return ptr<hir_expr>(std::move(node));
}

/// A variant-constructor expression (`@some(x)`) parses to an `ident_expr`
/// whose span still covers the leading `@` — this must stay exactly in
/// sync with the identically-named, identically-implemented predicate in
/// check.cpp, which is what actually decided whether each ident here was
/// checked as a variant constructor or an ordinary name.
[[nodiscard]] auto is_variant_ident(const ast::ident_expr &ident) -> bool {
  return ident.span.len() > ident.name.size();
}

/// Performs the AST-to-HIR walk for one function at a time. Not reusable
/// across functions: `scopes_`/`global_refs_`/`next_symbol_` are lowering-
/// local bookkeeping, reset per `lower_function` call (see the class-level
/// comment on `symbol_id` below).
class lowerer {
public:
  explicit lowerer(const checked_types &checked) : checked_(checked) {}

  [[nodiscard]] auto lower_function(const ast::func_decl &decl)
      -> std::expected<ptr<hir_function>, lowering_error>;

private:
  // ------------------------------------------------------------------
  //  Local identity
  //
  //  Decision 7 (spec/typed-ir-design.md) says HIR should reuse the
  //  semantic session's `symbol_id` rather than reinvent identity — but
  //  `checked_types` (src/semantic/types.h) only persists *types*, not the
  //  session's symbol table, so that real id isn't available here yet.
  //  Until that's threaded through, this class mints its own ordinals: a
  //  fresh id per parameter/`let` binding as it's declared, and one fixed
  //  id per distinct free-standing name (e.g. a called function) the first
  //  time it's referenced. These ids are only unique *within one
  //  `lower_function` call* — they are not stable across functions and
  //  must not be persisted or compared across separate lowering calls.
  // ------------------------------------------------------------------

  auto push_scope() -> void { scopes_.emplace_back(); }
  auto pop_scope() -> void { scopes_.pop_back(); }

  /// Declares a local binding named `name` with checked type `type`,
  /// returning its fresh symbol id. `type` is recorded in `local_types_` so
  /// a later `module_path_expr` (a bare `x.field` chain — see
  /// `lower_expr`'s case for it) rooted at this name can rebuild a real
  /// `hir_field` without needing a second AST node to look a type up
  /// against; every call site already has this binding's type in hand
  /// (either directly, or via its already-lowered initializer/pattern
  /// expression's own `.type`), so this doesn't add a new type-resolution
  /// obligation, just persists one that already existed.
  [[nodiscard]] auto declare_local(std::string_view name, type_id type)
      -> symbol_id {
    const auto id = next_symbol_++;
    scopes_.back().emplace(std::string(name), id);
    local_types_.emplace(id, type);
    return id;
  }

  /// Mints an id with no name of its own — used for a `match` subject's
  /// synthetic binding, which arm code never refers to by spelling (see
  /// `lower_match`).
  [[nodiscard]] auto mint_symbol() -> symbol_id { return next_symbol_++; }

  /// Looks up `name` among locals currently in scope only — unlike
  /// `resolve_reference`, never falls back to minting/reusing a global
  /// reference id, so a `nullopt` result reliably means "not a local
  /// binding" (used to decide whether a `module_path_expr`'s root is a
  /// local being field-accessed vs. a genuine module-qualified path).
  [[nodiscard]] auto lookup_local(std::string_view name) const
      -> std::optional<symbol_id> {
    for (const auto &scope : std::views::reverse(scopes_)) {
      if (const auto found = scope.find(std::string(name));
          found != scope.end()) {
        return found->second;
      }
    }
    return std::nullopt;
  }

  /// The checked type `declare_local` recorded for `symbol`, or `nullopt`
  /// if `symbol` was never declared through `declare_local` (e.g. it's a
  /// `mint_symbol`-only synthetic id).
  [[nodiscard]] auto local_type_of(symbol_id symbol) const
      -> std::optional<type_id> {
    if (const auto found = local_types_.find(symbol);
        found != local_types_.end()) {
      return found->second;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto resolve_reference(std::string_view name) -> symbol_id {
    if (const auto local = lookup_local(name); local.has_value()) {
      return *local;
    }
    auto key = std::string(name);
    if (const auto found = global_refs_.find(key);
        found != global_refs_.end()) {
      return found->second;
    }
    const auto id = next_symbol_++;
    global_refs_.emplace(std::move(key), id);
    return id;
  }

  // ------------------------------------------------------------------
  //  Checked-type lookup
  // ------------------------------------------------------------------

  [[nodiscard]] auto checked_type_of(const ast::node &node)
      -> std::expected<type_id, lowering_error> {
    const auto found = checked_.node_types.find(&node);
    if (found == checked_.node_types.end() || found->second == k_unknown_type ||
        found->second == k_error_type) {
      return fail(lowering_error_kind::unresolved_type, node.span,
                  "no concrete checked type is available for this node; "
                  "lowering only accepts fully type-checked, fully-annotated "
                  "code (spec/typed-ir-design.md Decision 1)");
    }
    return found->second;
  }

  // ------------------------------------------------------------------
  //  Expressions
  // ------------------------------------------------------------------

  [[nodiscard]] auto lower_expr(const ast::expr &expr)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_literal(const ast::literal_expr &lit)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_ident(const ast::ident_expr &ident)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_binary(const ast::binary_expr &bin)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_unary(const ast::unary_expr &un)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_call(const ast::call_expr &call)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_field(const ast::field_expr &field)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_module_path(const ast::module_path_expr &path)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_index(const ast::index_expr &index)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_tuple(const ast::tuple_expr &tuple)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_array(const ast::array_expr &array)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_struct(const ast::struct_expr &literal)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_cast(const ast::cast_expr &cast)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_lambda(const ast::lambda_expr &lambda)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_where(const ast::where_expr &where)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_try(const ast::try_expr &try_expr)
      -> std::expected<ptr<hir_expr>, lowering_error>;

  // ------------------------------------------------------------------
  //  Statements, blocks, and if (shared between statement and expression
  //  position — see hir::hir_if's doc comment).
  // ------------------------------------------------------------------

  [[nodiscard]] auto lower_block(const std::vector<ast::ptr<ast::node>> &stmts,
                                 source_span span,
                                 type_id type = k_unknown_type)
      -> std::expected<ptr<hir_block>, lowering_error>;
  /// Lowers a trailing `if`/`match` *statement* as this block's tail value
  /// — see `lower_block`'s call site for why this needs to exist
  /// separately from the ordinary `if_stmt`/`match_stmt` cases in
  /// `lower_stmt` (which always type as `k_unknown_type` and are never
  /// wrapped in `hir_expr_stmt`, since a mid-body `if`/`match` is a plain
  /// effect-only statement, not a value the rest of the pipeline needs to
  /// see).
  [[nodiscard]] auto lower_tail_control_flow_stmt(const ast::node &node,
                                                  type_id type)
      -> std::expected<ptr<hir_node>, lowering_error>;
  /// Lowers one statement to zero or more HIR nodes: almost always exactly
  /// one, except a destructuring `let` (`let (a, b) = pair`), which expands
  /// to a synthetic subject-binding `hir_let` plus one `hir_let` per name
  /// the pattern binds (see the `let_stmt` case) — there's no single HIR
  /// node that could represent "one surface statement" once one surface
  /// statement can introduce several bindings.
  [[nodiscard]] auto lower_stmt(const ast::node &node)
      -> std::expected<ptr_vec<hir_node>, lowering_error>;
  [[nodiscard]] auto lower_if(const std::vector<ast::if_branch> &branches,
                              const std::vector<ast::ptr<ast::node>> &else_body,
                              source_span span, type_id type)
      -> std::expected<ptr<hir_if>, lowering_error>;
  /// Dispatches a `for` loop to whichever iterable shape it matches (see
  /// spec/iterator-protocol-design.md): a range literal written directly
  /// in the loop header, a checked `option`, or a checked
  /// `array`/`list`/`slice`/`slice_mut`/`str` iterable. A user-defined
  /// iterable still rejects. The three shape-specific lowerers below are
  /// shared with `lower_for_expr` (comprehensions) — only what runs inside
  /// the loop body differs (a `for` statement's own AST body vs. a
  /// comprehension's nested clause / guarded push), threaded through as
  /// `inner_stmts`.
  [[nodiscard]] auto lower_for_stmt(const ast::for_stmt &for_stmt)
      -> std::expected<ptr_vec<hir_node>, lowering_error>;
  /// The range-literal shape: `for x in a..b: ...` / `for x in a..=b: ...`
  /// lowers to a counting `hir_while` loop with no new node kinds.
  /// `inner_stmts` runs at the top of the loop body, right after the loop
  /// variable is bound, and must be built inside that same scope (so it
  /// can reference the loop variable).
  [[nodiscard]] auto lower_range_loop(
      source_span span, const ast::binary_expr &range,
      const ast::binding_pattern &loop_var,
      const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
          &inner_stmts) -> std::expected<ptr_vec<hir_node>, lowering_error>;
  /// The indexed-container shape: `array`/`list`/`slice`/`slice_mut`/`str`
  /// all lower to the same counting loop bounded by a length (statically
  /// known for `array`, via `hir_container_len` otherwise) instead of a
  /// literal bound, indexing the container at each step. `inner_stmts` is
  /// the same contract as `lower_range_loop`'s.
  [[nodiscard]] auto lower_indexed_loop(
      source_span span, const ast::expr &iterable, type_id iterable_type,
      const ast::binding_pattern &loop_var,
      const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
          &inner_stmts) -> std::expected<ptr_vec<hir_node>, lowering_error>;
  /// The `option` shape: `for x in opt: ...` isn't a loop at all — it runs
  /// `inner_stmts` zero or one times, so it lowers to a plain two-arm
  /// `hir_match` (`@some(_) => { let x = <payload>; inner_stmts() }`,
  /// `@none => {}`) rather than anything involving `hir_while`.
  [[nodiscard]] auto lower_option_loop(
      source_span span, const ast::expr &iterable, type_id iterable_type,
      const ast::binding_pattern &loop_var,
      const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
          &inner_stmts) -> std::expected<ptr_vec<hir_node>, lowering_error>;
  /// Shared tail `lower_range_loop`/`lower_indexed_loop` need: binds the
  /// loop variable to `loop_var_value()` at the top of the loop body,
  /// splices in `inner_stmts()`, then increments the index by one.
  /// Returns the assembled loop body, not the loop itself — each caller
  /// still builds its own start/end/condition and wraps the result in a
  /// `hir_while`.
  [[nodiscard]] auto build_for_loop_body(
      source_span span, symbol_id index_symbol, type_id counter_type,
      const ast::binding_pattern &loop_var,
      const std::function<ptr<hir_expr>()> &loop_var_value,
      const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
          &inner_stmts) -> std::expected<ptr<hir_block>, lowering_error>;
  /// A `for` *statement*'s `inner_stmts`: lowers the surface body, wrapping
  /// it in `if guard: ...` when a guard is present (see the `for_stmt` case
  /// in `lower_stmt` for why that's a plain conditional and not a
  /// `continue`).
  [[nodiscard]] auto
  lower_guarded_for_body(const ast::expr *guard_ast,
                         const std::vector<ast::ptr<ast::node>> &body_ast,
                         source_span span)
      -> std::expected<ptr_vec<hir_node>, lowering_error>;
  /// `while let pattern = expr: body` (see `hir_while_let`'s doc comment).
  /// Reuses the general `lower_pattern` (unlike a `for` loop, which only
  /// ever binds a single plain loop variable), since `pattern` here can be
  /// any pattern shape.
  [[nodiscard]] auto lower_while_let_stmt(const ast::while_stmt &while_stmt)
      -> std::expected<ptr_vec<hir_node>, lowering_error>;
  /// `for` comprehensions (`for x in a..b => expr`, `for x in a, y in b if
  /// guard => expr`): builds a fresh `list[T]` accumulator, lowers the
  /// clause chain into nested loops (reusing the same three shape lowerers
  /// as `for` statements, one nested inside the next), and appends the
  /// yielded value — filtered by `guard`, if present — via `hir_list_push`
  /// at the innermost position. See spec/iterator-protocol-design.md.
  [[nodiscard]] auto lower_for_expr(const ast::for_expr &for_expr)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  /// Recursively lowers `clauses[index:]` into nested loops, calling
  /// `innermost` at the deepest level (once every clause's loop variable
  /// is bound and in scope). `fallback_span` is used only for a diagnostic
  /// that should be unreachable on a well-formed parse (a clause missing
  /// its iterable).
  [[nodiscard]] auto lower_comprehension_clause(
      const std::vector<ast::for_expr::iter_clause> &clauses, size_t index,
      source_span fallback_span,
      const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
          &innermost) -> std::expected<ptr_vec<hir_node>, lowering_error>;

  // ------------------------------------------------------------------
  //  match / patterns
  // ------------------------------------------------------------------

  /// Lowers one surface pattern to a *structural* `hir_pattern` (wildcard,
  /// literal, a `|` of those, or a tuple/struct/constructor/range
  /// destructuring of them). A plain binding or a `pattern as name` alias
  /// contributes no structure of its own — instead it appends a synthetic
  /// `hir_let` to `pending`, initialized from `make_place()`, which the
  /// caller (`lower_match`, or a recursive call for a nested destructuring
  /// position) splices onto the front of the arm's body.
  ///
  /// `make_place` builds a fresh, correctly-typed expression referencing
  /// whatever value `pattern` matches against — a `hir_local_ref` to the
  /// match subject at the top level, or a `hir_field`/`hir_tuple_index`/
  /// `hir_variant_payload` projection one level down for a nested
  /// destructuring position. It's a factory rather than a single built
  /// expression because it may be invoked more than once (once per binding
  /// or alias at this position, plus once per recursive call into a
  /// sub-pattern) — always safe here since every projection this pass
  /// builds is a pure read with no side effects to duplicate.
  [[nodiscard]] auto
  lower_pattern(const ast::node &pattern,
                const std::function<ptr<hir_expr>()> &make_place,
                std::vector<ptr<hir_node>> &pending)
      -> std::expected<ptr<hir_pattern>, lowering_error>;
  [[nodiscard]] auto lower_match(const ast::expr &subject_ast,
                                 const std::vector<ast::match_arm> &arms,
                                 source_span span, type_id type)
      -> std::expected<ptr<hir_match>, lowering_error>;

  const checked_types &checked_;
  std::vector<std::unordered_map<std::string, symbol_id>> scopes_;
  std::unordered_map<std::string, symbol_id> global_refs_;
  std::unordered_map<symbol_id, type_id> local_types_;
  symbol_id next_symbol_ = 0;
};

auto lowerer::lower_expr(const ast::expr &expr)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  if (expr.has_error) {
    return fail(
        lowering_error_kind::unsupported_construct, expr.span,
        "expression carries a parse/recovery error and cannot be lowered");
  }
  switch (expr.kind) {
  case ast::node_kind::group_expr: {
    const auto &group = dynamic_cast<const ast::group_expr &>(expr);
    if (group.inner == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, expr.span,
                  "parenthesized expression has no inner expression");
    }
    return lower_expr(*group.inner);
  }
  case ast::node_kind::literal_expr:
    return lower_literal(dynamic_cast<const ast::literal_expr &>(expr));
  case ast::node_kind::ident_expr:
    return lower_ident(dynamic_cast<const ast::ident_expr &>(expr));
  case ast::node_kind::binary_expr:
    return lower_binary(dynamic_cast<const ast::binary_expr &>(expr));
  case ast::node_kind::unary_expr:
    return lower_unary(dynamic_cast<const ast::unary_expr &>(expr));
  case ast::node_kind::call_expr:
    return lower_call(dynamic_cast<const ast::call_expr &>(expr));
  case ast::node_kind::field_expr:
    return lower_field(dynamic_cast<const ast::field_expr &>(expr));
  case ast::node_kind::index_expr:
    return lower_index(dynamic_cast<const ast::index_expr &>(expr));
  case ast::node_kind::block_expr: {
    const auto &block = dynamic_cast<const ast::block_expr &>(expr);
    auto type = checked_type_of(block);
    if (!type.has_value()) {
      return std::unexpected(type.error());
    }
    auto lowered = lower_block(block.stmts, block.span, *type);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    return ok_expr(std::move(*lowered));
  }
  case ast::node_kind::if_expr: {
    const auto &if_e = dynamic_cast<const ast::if_expr &>(expr);
    auto type = checked_type_of(if_e);
    if (!type.has_value()) {
      return std::unexpected(type.error());
    }
    auto lowered = lower_if(if_e.branches, if_e.else_body, if_e.span, *type);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    return ok_expr(std::move(*lowered));
  }
  case ast::node_kind::match_expr: {
    const auto &match_e = dynamic_cast<const ast::match_expr &>(expr);
    if (match_e.subject == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, match_e.span,
                  "match expression has no subject");
    }
    auto type = checked_type_of(match_e);
    if (!type.has_value()) {
      return std::unexpected(type.error());
    }
    auto lowered =
        lower_match(*match_e.subject, match_e.arms, match_e.span, *type);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    return ok_expr(std::move(*lowered));
  }
  case ast::node_kind::tuple_expr:
    return lower_tuple(dynamic_cast<const ast::tuple_expr &>(expr));
  case ast::node_kind::array_expr:
    return lower_array(dynamic_cast<const ast::array_expr &>(expr));
  case ast::node_kind::struct_expr:
    return lower_struct(dynamic_cast<const ast::struct_expr &>(expr));
  case ast::node_kind::cast_expr:
    return lower_cast(dynamic_cast<const ast::cast_expr &>(expr));
  case ast::node_kind::lambda_expr:
    return lower_lambda(dynamic_cast<const ast::lambda_expr &>(expr));
  case ast::node_kind::where_expr:
    return lower_where(dynamic_cast<const ast::where_expr &>(expr));
  case ast::node_kind::try_expr:
    return lower_try(dynamic_cast<const ast::try_expr &>(expr));
  case ast::node_kind::for_expr:
    return lower_for_expr(dynamic_cast<const ast::for_expr &>(expr));
  case ast::node_kind::module_path_expr:
    return lower_module_path(dynamic_cast<const ast::module_path_expr &>(expr));
  default:
    return fail(lowering_error_kind::unsupported_construct, expr.span,
                std::format("expression kind {} is not lowered by the first "
                            "milestone (spec/typed-ir-design.md)",
                            static_cast<int>(expr.kind)));
  }
}

auto lowerer::lower_literal(const ast::literal_expr &lit)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(lit);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  return ok_expr(make<hir_literal>(lit.span, *type, lit.lit_kind, lit.value));
}

auto lowerer::lower_ident(const ast::ident_expr &ident)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(ident);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (is_variant_ident(ident)) {
    // A bare unit-variant reference, e.g. `@none` with no call parens —
    // still variant construction (see hir_variant_init), just with no
    // payload arguments to lower.
    return ok_expr(make<hir_variant_init>(ident.span, *type, ident.name,
                                          ptr_vec<hir_expr>{}));
  }
  const auto symbol = resolve_reference(ident.name);
  return ok_expr(make<hir_local_ref>(ident.span, *type, symbol, ident.name));
}

auto lowerer::lower_binary(const ast::binary_expr &bin)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(bin);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (bin.lhs == nullptr || bin.rhs == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, bin.span,
                "binary expression is missing an operand");
  }
  auto lhs = lower_expr(*bin.lhs);
  if (!lhs.has_value()) {
    return std::unexpected(lhs.error());
  }
  auto rhs = lower_expr(*bin.rhs);
  if (!rhs.has_value()) {
    return std::unexpected(rhs.error());
  }
  return ok_expr(hir::make<hir_binary>(bin.span, *type, bin.op, std::move(*lhs),
                                       std::move(*rhs)));
}

auto lowerer::lower_unary(const ast::unary_expr &un)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(un);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (un.operand == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, un.span,
                "unary expression is missing its operand");
  }
  auto operand = lower_expr(*un.operand);
  if (!operand.has_value()) {
    return std::unexpected(operand.error());
  }
  return ok_expr(
      hir::make<hir_unary>(un.span, *type, un.op, std::move(*operand)));
}

auto lowerer::lower_call(const ast::call_expr &call)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(call);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (call.callee == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, call.span,
                "call expression is missing its callee");
  }

  // `x.len()`/`x.as_bytes()` on a builtin container/`str` — these have no
  // `func_decl` backing them at all (`semantic::check.cpp`'s
  // `builtin_method_result` is a hardcoded type-rule table, not a real
  // declaration `resolved_callees_` could ever point at), so they can't go
  // through the ordinary method-call lowering below. Guarded on the absence
  // of a `resolved_callees_` entry so a real user-defined `len`/`as_bytes`
  // method (an inherent method or `extend` override, which *does* get one —
  // see `record_instance_method_callee`) always wins.
  if (call.callee->kind == ast::node_kind::field_expr &&
      !checked_.resolved_callees.contains(&call)) {
    const auto &field = dynamic_cast<const ast::field_expr &>(*call.callee);
    if (field.object != nullptr && field.field_name == "len") {
      auto object = lower_expr(*field.object);
      if (!object.has_value()) {
        return std::unexpected(object.error());
      }
      return ok_expr(
          make<hir_container_len>(call.span, *type, std::move(*object)));
    }
    if (field.object != nullptr && field.field_name == "as_bytes") {
      // `str` and `slice[byte]` share the same `{len; data_ptr}` runtime
      // representation (`src/runtime/io.h`, and `bytes_of` in
      // `src/bytecode/vm.cpp`), so this is a pure type-level reinterpret —
      // the `str` value lowers completely unchanged.
      return lower_expr(*field.object);
    }
  }

  // `@variant(args...)` parses as a call whose callee is the variant's
  // ident (see `is_variant_ident`) — this is construction, not an ordinary
  // call, so it never goes through the callee-resolution machinery below
  // (there's no function being called, real or otherwise).
  if (call.callee->kind == ast::node_kind::ident_expr) {
    const auto &callee_ident =
        dynamic_cast<const ast::ident_expr &>(*call.callee);
    if (is_variant_ident(callee_ident)) {
      auto args = ptr_vec<hir_expr>{};
      args.reserve(call.args.size());
      for (const auto &arg : call.args) {
        if (arg.name.has_value()) {
          return fail(lowering_error_kind::unsupported_construct, arg.span,
                      "named arguments are not supported constructing a "
                      "sum-type variant");
        }
        if (arg.value == nullptr) {
          return fail(lowering_error_kind::unsupported_construct, arg.span,
                      "constructor argument is missing its value");
        }
        auto lowered = lower_expr(*arg.value);
        if (!lowered.has_value()) {
          return std::unexpected(lowered.error());
        }
        args.push_back(std::move(*lowered));
      }
      return ok_expr(make<hir_variant_init>(call.span, *type, callee_ident.name,
                                            std::move(args)));
    }
  }

  // A module- or type-qualified call (`std.io.open(...)`, `io_error.
  // from(...)`) was resolved against a real declaration by `check.cpp`'s
  // `infer_qualified_call` — rebuild the callee directly from that
  // resolution instead of `lower_module_path`, which only ever handles a
  // local-value root. The local name matches exactly what `lower_module`
  // names the same declaration when it lowers it (bare for a free
  // function, `TargetType::method` for an associated function), so both
  // backends' cross-module dispatch (see `hir_local_ref::owner_module`)
  // finds the same function under the same key from either side.
  auto callee = std::expected<ptr<hir_expr>, lowering_error>{};
  const ast::expr *receiver_ast = nullptr;
  if (const auto found = checked_.resolved_callees.find(&call);
      found != checked_.resolved_callees.end()) {
    const auto &resolved = found->second;
    receiver_ast = resolved.receiver;
    const auto local_name =
        resolved.impl_target_type.empty()
            ? resolved.decl->name
            : std::format("{}::{}", resolved.impl_target_type,
                          resolved.decl->name);
    auto callee_type = checked_type_of(*call.callee);
    if (!callee_type.has_value()) {
      return std::unexpected(callee_type.error());
    }
    const auto symbol = resolve_reference(local_name);
    callee =
        ok_expr(make<hir_local_ref>(call.callee->span, *callee_type, symbol,
                                    local_name, resolved.owner_module));
  } else {
    callee = lower_expr(*call.callee);
  }
  if (!callee.has_value()) {
    return std::unexpected(callee.error());
  }

  // An instance-method call's receiver (`resolved_callee::receiver`) is the
  // callee's hidden first (`self`) argument — evaluated once, ahead of every
  // declared argument, matching where `field.object` is written at the call
  // site.
  auto receiver_arg = ptr<hir_expr>{};
  if (receiver_ast != nullptr) {
    auto lowered_receiver = lower_expr(*receiver_ast);
    if (!lowered_receiver.has_value()) {
      return std::unexpected(lowered_receiver.error());
    }
    receiver_arg = std::move(*lowered_receiver);
  }

  // A call resolved against a real declaration (free function, method, or
  // trait default) has a persisted argument-to-parameter mapping (see
  // `call_argument_mapping` in types.h) — reorder named/positional
  // arguments into the callee's declared parameter order using it. A bare
  // `fn(...)`-typed callee (no parameter names to resolve against) never
  // gets an entry, so falls through to the positional-only path below.
  //
  // Known limitation: argument expressions are lowered (and will be
  // evaluated) in *declared-parameter* order here, not necessarily the
  // order written at the call site — a call that both reorders named
  // arguments and relies on side effects between them could observe a
  // different evaluation order than what's written. Positional-only calls
  // (the overwhelming majority) are unaffected, since parameter order and
  // written order are the same thing there.
  if (const auto mapping = checked_.call_argument_mappings.find(&call);
      mapping != checked_.call_argument_mappings.end()) {
    auto args = ptr_vec<hir_expr>{};
    args.reserve(mapping->second.args_by_param.size() +
                 (receiver_arg != nullptr ? 1 : 0));
    if (receiver_arg != nullptr) {
      args.push_back(std::move(receiver_arg));
    }
    for (const auto *arg_expr : mapping->second.args_by_param) {
      if (arg_expr == nullptr) {
        return fail(lowering_error_kind::unsupported_construct, call.span,
                    "calls that rely on a parameter's default value are "
                    "not lowered yet — the default expression's evaluation "
                    "context isn't threaded through this pass");
      }
      auto lowered = lower_expr(*arg_expr);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      args.push_back(std::move(*lowered));
    }
    return ok_expr(
        make<hir_call>(call.span, *type, std::move(*callee), std::move(args)));
  }

  for (const auto &arg : call.args) {
    if (arg.name.has_value()) {
      return fail(lowering_error_kind::unsupported_construct, arg.span,
                  "named arguments are only supported calling a named "
                  "function or method, not a bare function value");
    }
  }
  auto args = ptr_vec<hir_expr>{};
  args.reserve(call.args.size());
  for (const auto &arg : call.args) {
    if (arg.value == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, arg.span,
                  "call argument is missing its value");
    }
    auto lowered = lower_expr(*arg.value);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    args.push_back(std::move(*lowered));
  }
  return ok_expr(
      make<hir_call>(call.span, *type, std::move(*callee), std::move(args)));
}

auto lowerer::lower_field(const ast::field_expr &field)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(field);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (!field.generic_args.empty()) {
    return fail(lowering_error_kind::unsupported_construct, field.span,
                "generic method-call arguments are not lowered by the first "
                "milestone");
  }
  if (field.object == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, field.span,
                "field access is missing its target expression");
  }
  auto object = lower_expr(*field.object);
  if (!object.has_value()) {
    return std::unexpected(object.error());
  }
  return ok_expr(
      make<hir_field>(field.span, *type, std::move(*object), field.field_name));
}

/// `a.b` parses ambiguously — the parser cannot tell "a value being
/// field-accessed" from "a module path" without symbol info (`parser.cpp`'s
/// `parse_ident_or_path_expr`), so it always produces `module_path_expr` for
/// a bare leading identifier followed by `.name`; `field_expr` only comes
/// from a postfix chain whose base wasn't a bare identifier (`foo().field`,
/// `arr[0].field`, ...). The checker's `infer_module_path`
/// (`semantic/check.cpp`) resolves this ambiguity for typing purposes by
/// checking whether the first segment is a known value binding, but never
/// rewrites the AST — so lowering has to redo the same disambiguation here.
///
/// Only the two-segment case (`root.field`) is supported: `checked_.
/// node_types` records a type for the *whole* `module_path_expr` node, not
/// per-segment prefixes, so a chain's only two type_ids this pass can ever
/// recover are the root's (via `local_type_of`, since `declare_local`
/// records it) and the final result's (via `checked_type_of` on `path`
/// itself, which — for exactly two segments — *is* the field's type). A
/// longer chain (`a.b.c`) would need `b`'s type too, which isn't tracked
/// anywhere lowering can reach; that's left as `unsupported_construct`
/// rather than guessed. A root that isn't a local binding in scope here is
/// a genuine module-qualified reference, which this milestone doesn't
/// lower yet either.
auto lowerer::lower_module_path(const ast::module_path_expr &path)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  if (path.segments.size() != 2) {
    return fail(lowering_error_kind::unsupported_construct, path.span,
                "only a two-segment `value.field` path is lowered by the "
                "first milestone — a longer chain, or a module-qualified "
                "reference, is not supported yet");
  }
  const auto root_symbol = lookup_local(path.segments[0]);
  if (!root_symbol.has_value()) {
    return fail(lowering_error_kind::unsupported_construct, path.span,
                std::format("`{}` does not resolve to a local binding — "
                            "module-qualified value references are not "
                            "lowered by the first milestone yet",
                            path.segments[0]));
  }
  const auto root_type = local_type_of(*root_symbol);
  if (!root_type.has_value()) {
    return fail(lowering_error_kind::unresolved_type, path.span,
                "no concrete checked type is available for this local "
                "binding; lowering only accepts fully type-checked, "
                "fully-annotated code (spec/typed-ir-design.md Decision 1)");
  }
  auto type = checked_type_of(path);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  auto root = ptr<hir_expr>(make<hir_local_ref>(
      path.span, *root_type, *root_symbol, path.segments[0]));
  return ok_expr(
      make<hir_field>(path.span, *type, std::move(root), path.segments[1]));
}

auto lowerer::lower_index(const ast::index_expr &index)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(index);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (index.object == nullptr || index.index == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, index.span,
                "index expression is missing its target or index");
  }
  auto object = lower_expr(*index.object);
  if (!object.has_value()) {
    return std::unexpected(object.error());
  }
  auto idx = lower_expr(*index.index);
  if (!idx.has_value()) {
    return std::unexpected(idx.error());
  }
  return ok_expr(
      make<hir_index>(index.span, *type, std::move(*object), std::move(*idx)));
}

auto lowerer::lower_tuple(const ast::tuple_expr &tuple)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(tuple);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  auto elements = ptr_vec<hir_expr>{};
  elements.reserve(tuple.elements.size());
  for (const auto &element_ast : tuple.elements) {
    if (element_ast == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, tuple.span,
                  "tuple literal has a missing element");
    }
    auto lowered = lower_expr(*element_ast);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    elements.push_back(std::move(*lowered));
  }
  return ok_expr(make<hir_tuple>(tuple.span, *type, std::move(elements)));
}

auto lowerer::lower_array(const ast::array_expr &array)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(array);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (array.fill_value != nullptr) {
    auto fill_value = lower_expr(*array.fill_value);
    if (!fill_value.has_value()) {
      return std::unexpected(fill_value.error());
    }
    auto fill_count = ptr<hir_expr>{};
    if (array.fill_count != nullptr) {
      auto lowered_count = lower_expr(*array.fill_count);
      if (!lowered_count.has_value()) {
        return std::unexpected(lowered_count.error());
      }
      fill_count = std::move(*lowered_count);
    }
    return ok_expr(make<hir_array_init>(array.span, *type, ptr_vec<hir_expr>{},
                                        std::move(*fill_value),
                                        std::move(fill_count)));
  }

  auto elements = ptr_vec<hir_expr>{};
  elements.reserve(array.elements.size());
  for (const auto &element_ast : array.elements) {
    if (element_ast == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, array.span,
                  "array literal has a missing element");
    }
    auto lowered = lower_expr(*element_ast);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    elements.push_back(std::move(*lowered));
  }
  return ok_expr(make<hir_array_init>(array.span, *type, std::move(elements),
                                      nullptr, nullptr));
}

auto lowerer::lower_struct(const ast::struct_expr &literal)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(literal);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  auto fields = std::vector<hir_struct_init_field>{};
  fields.reserve(literal.fields.size());
  for (const auto &field : literal.fields) {
    if (field.value != nullptr) {
      auto lowered = lower_expr(*field.value);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      fields.push_back(hir_struct_init_field{.name = field.name,
                                             .value = std::move(*lowered)});
      continue;
    }
    // Shorthand `{x}` reads the in-scope value `x` directly — desugars to
    // `{x: x}` (see hir_struct_init's doc comment), so lowering builds the
    // implied `x` reference itself rather than recursing through
    // lower_expr on a node that doesn't exist.
    const auto found = checked_.struct_literal_field_types.find(&field);
    if (found == checked_.struct_literal_field_types.end() ||
        found->second == k_unknown_type || found->second == k_error_type) {
      return fail(lowering_error_kind::unresolved_type, field.span,
                  "no concrete checked type is available for this struct "
                  "literal's shorthand field; lowering only accepts fully "
                  "type-checked, fully-annotated code (spec/"
                  "typed-ir-design.md Decision 1)");
    }
    const auto symbol = resolve_reference(field.name);
    fields.push_back(hir_struct_init_field{
        .name = field.name,
        .value = ptr<hir_expr>(make<hir_local_ref>(field.span, found->second,
                                                   symbol, field.name))});
  }
  return ok_expr(make<hir_struct_init>(literal.span, *type, std::move(fields)));
}

auto lowerer::lower_cast(const ast::cast_expr &cast)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(cast);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (cast.operand == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, cast.span,
                "cast expression has no operand");
  }
  auto operand = lower_expr(*cast.operand);
  if (!operand.has_value()) {
    return std::unexpected(operand.error());
  }
  return ok_expr(make<hir_cast>(cast.span, *type, std::move(*operand)));
}

auto lowerer::lower_lambda(const ast::lambda_expr &lambda)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto lambda_type = checked_type_of(lambda);
  if (!lambda_type.has_value()) {
    return std::unexpected(lambda_type.error());
  }
  // Unlike a free function (Decision 1: every parameter always explicitly
  // annotated), a lambda's own checked type already carries a concrete
  // type per parameter regardless of whether that came from an annotation
  // or the lambda's use-site context (see infer_lambda in check.cpp) — so
  // decomposing the lambda's own `fn(...)` type is simpler and more
  // complete than re-deriving each parameter's type from its pattern node.
  const auto &entry = checked_.types.entry(*lambda_type);
  if (entry.kind != type_kind::fn_kind) {
    return fail(lowering_error_kind::unresolved_type, lambda.span,
                "lambda did not resolve to a concrete function type");
  }
  if (entry.args.size() != lambda.params.size()) {
    return fail(lowering_error_kind::unsupported_construct, lambda.span,
                "lambda parameter count does not match its checked "
                "function type");
  }

  push_scope();

  auto params = std::vector<hir_param>{};
  auto param_prelude = ptr_vec<hir_node>{};
  params.reserve(lambda.params.size());
  for (size_t i = 0; i < lambda.params.size(); ++i) {
    const auto &param = lambda.params[i];
    const auto ptype = entry.args[i];
    if (param.pattern == nullptr || param.pattern->has_error) {
      pop_scope();
      return fail(lowering_error_kind::unsupported_construct, param.span,
                  "lambda parameter has no usable pattern");
    }

    if (param.pattern->kind == ast::node_kind::binding_pattern) {
      const auto &binding =
          dynamic_cast<const ast::binding_pattern &>(*param.pattern);
      const auto symbol = declare_local(binding.name, ptype);
      params.push_back(
          hir_param{.symbol = symbol, .name = binding.name, .type = ptype});
      continue;
    }

    // A destructuring lambda parameter, same treatment as a destructuring
    // free-function parameter (see lower_function): synthetic identity,
    // bindings collected into a prelude spliced onto the body below.
    const auto symbol = mint_symbol();
    const auto pspan = param.span;
    const std::function<ptr<hir_expr>()> make_place =
        [symbol, ptype, pspan]() -> ptr<hir_expr> {
      return ptr<hir_expr>(
          make<hir_local_ref>(pspan, ptype, symbol, std::string("<param>")));
    };
    auto pending = std::vector<ptr<hir_node>>{};
    auto pattern = lower_pattern(*param.pattern, make_place, pending);
    if (!pattern.has_value()) {
      pop_scope();
      return std::unexpected(pattern.error());
    }
    params.push_back(hir_param{
        .symbol = symbol, .name = std::format("<param {}>", i), .type = ptype});
    for (auto &binding : pending) {
      param_prelude.push_back(std::move(binding));
    }
  }

  const auto return_type = entry.result;

  auto body = std::expected<ptr<hir_block>, lowering_error>{};
  if (lambda.body_expr != nullptr) {
    auto expr_type = checked_type_of(*lambda.body_expr);
    if (!expr_type.has_value()) {
      pop_scope();
      return std::unexpected(expr_type.error());
    }
    auto value = lower_expr(*lambda.body_expr);
    if (!value.has_value()) {
      pop_scope();
      return std::unexpected(value.error());
    }
    auto ret = ptr<hir_node>(
        make<hir_return>(lambda.body_expr->span, std::move(*value)));
    auto stmts = std::move(param_prelude);
    stmts.push_back(std::move(ret));
    body =
        make<hir_block>(lambda.body_expr->span, return_type, std::move(stmts));
  } else {
    body = lower_block(lambda.body_stmts, lambda.span, return_type);
    if (body.has_value() && !param_prelude.empty()) {
      auto merged = std::move(param_prelude);
      for (auto &stmt_ptr : (*body)->stmts) {
        merged.push_back(std::move(stmt_ptr));
      }
      (*body)->stmts = std::move(merged);
    }
  }
  pop_scope();
  if (!body.has_value()) {
    return std::unexpected(body.error());
  }

  return ok_expr(make<hir_lambda>(lambda.span, *lambda_type, std::move(params),
                                  return_type, std::move(*body)));
}

auto lowerer::lower_where(const ast::where_expr &where)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(where);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  push_scope();
  auto stmts = ptr_vec<hir_node>{};
  for (const auto &binding : where.bindings) {
    if (binding.value == nullptr) {
      pop_scope();
      return fail(lowering_error_kind::unsupported_construct, binding.span,
                  "where binding has no value");
    }
    auto value = lower_expr(*binding.value);
    if (!value.has_value()) {
      pop_scope();
      return std::unexpected(value.error());
    }
    const auto symbol = declare_local(binding.name, (*value)->type);
    stmts.push_back(ptr<hir_node>(
        make<hir_let>(binding.span, symbol, binding.name, std::move(*value))));
  }
  if (where.inner == nullptr) {
    pop_scope();
    return fail(lowering_error_kind::unsupported_construct, where.span,
                "where expression has no inner expression");
  }
  auto inner = lower_expr(*where.inner);
  if (!inner.has_value()) {
    pop_scope();
    return std::unexpected(inner.error());
  }
  stmts.push_back(
      ptr<hir_node>(make<hir_expr_stmt>(where.inner->span, std::move(*inner))));
  pop_scope();
  return ok_expr(make<hir_block>(where.span, *type, std::move(stmts)));
}

auto lowerer::lower_try(const ast::try_expr &try_expr)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  // `x?` desugars to a two-arm match on the wrapper's runtime tag:
  //   match x: @ok(_)/@some(_) => <the unwrapped payload>
  //            @err(_)/@none  => return <x, unchanged>
  // The failure arm returns the *original* subject value rather than
  // reconstructing `@err(e)`/`@none` — it's already exactly that value;
  // the checker only requires the enclosing function to also return a
  // result/option (not that the two share the same success type), so
  // nothing needs rebuilding here.
  auto type = checked_type_of(try_expr);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (try_expr.operand == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, try_expr.span,
                "try expression has no operand");
  }
  auto operand_type = checked_type_of(*try_expr.operand);
  if (!operand_type.has_value()) {
    return std::unexpected(operand_type.error());
  }
  const auto &entry = checked_.types.entry(*operand_type);
  const auto is_result =
      entry.kind == type_kind::builtin_generic_kind && entry.name == "result";
  const auto is_option =
      entry.kind == type_kind::builtin_generic_kind && entry.name == "option";
  if (!is_result && !is_option) {
    return fail(lowering_error_kind::unresolved_type, try_expr.span,
                "`?` operand did not resolve to a concrete result/option type");
  }

  auto operand = lower_expr(*try_expr.operand);
  if (!operand.has_value()) {
    return std::unexpected(operand.error());
  }

  const auto subject_symbol = mint_symbol();
  const auto subject_type = *operand_type;
  const auto subject_span = try_expr.operand->span;
  const std::function<ptr<hir_expr>()> make_place =
      [subject_symbol, subject_type, subject_span]() -> ptr<hir_expr> {
    return ptr<hir_expr>(make<hir_local_ref>(subject_span, subject_type,
                                             subject_symbol,
                                             std::string("<try subject>")));
  };

  const auto success_variant = std::string(is_result ? "ok" : "some");
  const auto failure_variant = std::string(is_result ? "err" : "none");

  auto success_args = ptr_vec<hir_pattern>{};
  success_args.push_back(
      ptr<hir_pattern>(make<hir_wildcard_pattern>(try_expr.span)));
  auto success_pattern = ptr<hir_pattern>(make<hir_constructor_pattern>(
      try_expr.span, success_variant, std::move(success_args)));
  auto success_value = ptr<hir_expr>(make<hir_variant_payload>(
      try_expr.span, *type, make_place(), success_variant, size_t{0}));
  auto success_stmts = ptr_vec<hir_node>{};
  success_stmts.push_back(ptr<hir_node>(
      make<hir_expr_stmt>(try_expr.span, std::move(success_value))));
  auto success_arm =
      hir_match_arm{.pattern = std::move(success_pattern),
                    .guard = nullptr,
                    .body = make<hir_block>(try_expr.span, k_unknown_type,
                                            std::move(success_stmts))};

  auto failure_args = ptr_vec<hir_pattern>{};
  if (is_result) {
    failure_args.push_back(
        ptr<hir_pattern>(make<hir_wildcard_pattern>(try_expr.span)));
  }
  auto failure_pattern = ptr<hir_pattern>(make<hir_constructor_pattern>(
      try_expr.span, failure_variant, std::move(failure_args)));
  auto failure_stmts = ptr_vec<hir_node>{};
  failure_stmts.push_back(
      ptr<hir_node>(make<hir_return>(try_expr.span, make_place())));
  auto failure_arm =
      hir_match_arm{.pattern = std::move(failure_pattern),
                    .guard = nullptr,
                    .body = make<hir_block>(try_expr.span, k_unknown_type,
                                            std::move(failure_stmts))};

  auto arms = std::vector<hir_match_arm>{};
  arms.push_back(std::move(success_arm));
  arms.push_back(std::move(failure_arm));

  return ok_expr(make<hir_match>(try_expr.span, *type, std::move(*operand),
                                 subject_symbol, std::move(arms)));
}

auto lowerer::lower_tail_control_flow_stmt(const ast::node &node, type_id type)
    -> std::expected<ptr<hir_node>, lowering_error> {
  if (node.kind == ast::node_kind::if_stmt) {
    const auto &if_s = dynamic_cast<const ast::if_stmt &>(node);
    auto lowered = lower_if(if_s.branches, if_s.else_body, if_s.span, type);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    return ptr<hir_node>(
        make<hir_expr_stmt>(if_s.span, ptr<hir_expr>(std::move(*lowered))));
  }
  const auto &match_s = dynamic_cast<const ast::match_stmt &>(node);
  if (match_s.subject == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, match_s.span,
                "match statement has no subject");
  }
  auto lowered =
      lower_match(*match_s.subject, match_s.arms, match_s.span, type);
  if (!lowered.has_value()) {
    return std::unexpected(lowered.error());
  }
  return ptr<hir_node>(
      make<hir_expr_stmt>(match_s.span, ptr<hir_expr>(std::move(*lowered))));
}

auto lowerer::lower_block(const std::vector<ast::ptr<ast::node>> &stmts,
                          source_span span, type_id type)
    -> std::expected<ptr<hir_block>, lowering_error> {
  push_scope();
  auto lowered_stmts = ptr_vec<hir_node>{};
  lowered_stmts.reserve(stmts.size());

  // A trailing `if`/`match` *statement* is exactly as value-producing as
  // any other expression per spec/kira-reference.md's "Control Flow" — the
  // same "implicit tail expression is the return value" rule an ordinary
  // `expr_stmt` already gets. Route only the last non-null statement
  // through `lower_tail_control_flow_stmt` so it's typed with this block's
  // real `type` and wrapped in `hir_expr_stmt`; everything else (including
  // a mid-body `if`/`match`, which the checker itself types as `unit`)
  // goes through the ordinary `lower_stmt` path unchanged.
  auto last_index = std::optional<size_t>{};
  for (size_t i = stmts.size(); i-- > 0;) {
    if (stmts[i] != nullptr) {
      last_index = i;
      break;
    }
  }

  for (size_t i = 0; i < stmts.size(); ++i) {
    const auto &stmt_ptr = stmts[i];
    if (stmt_ptr == nullptr) {
      continue;
    }
    if (last_index.has_value() && i == *last_index &&
        (stmt_ptr->kind == ast::node_kind::if_stmt ||
         stmt_ptr->kind == ast::node_kind::match_stmt)) {
      auto tail = lower_tail_control_flow_stmt(*stmt_ptr, type);
      if (!tail.has_value()) {
        pop_scope();
        return std::unexpected(tail.error());
      }
      lowered_stmts.push_back(std::move(*tail));
      continue;
    }
    auto lowered = lower_stmt(*stmt_ptr);
    if (!lowered.has_value()) {
      pop_scope();
      return std::unexpected(lowered.error());
    }
    for (auto &node : *lowered) {
      lowered_stmts.push_back(std::move(node));
    }
  }
  pop_scope();
  return make<hir_block>(span, type, std::move(lowered_stmts));
}

/// Wraps a single lowered node into the one-or-more-nodes result
/// `lower_stmt` returns.
[[nodiscard]] auto one_stmt(ptr<hir_node> node) -> ptr_vec<hir_node> {
  auto result = ptr_vec<hir_node>{};
  result.push_back(std::move(node));
  return result;
}

auto lowerer::lower_stmt(const ast::node &node)
    -> std::expected<ptr_vec<hir_node>, lowering_error> {
  if (node.has_error) {
    return fail(
        lowering_error_kind::unsupported_construct, node.span,
        "statement carries a parse/recovery error and cannot be lowered");
  }
  switch (node.kind) {
  case ast::node_kind::let_stmt: {
    const auto &let = dynamic_cast<const ast::let_stmt &>(node);
    if (let.pattern == nullptr || let.pattern->has_error) {
      return fail(lowering_error_kind::unsupported_construct, let.span,
                  "let binding has no usable pattern");
    }
    if (let.initializer == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, let.span,
                  "let binding has no initializer");
    }
    auto initializer = lower_expr(*let.initializer);
    if (!initializer.has_value()) {
      return std::unexpected(initializer.error());
    }

    // Fast path: a plain `let name = expr` (no `else`) binds the
    // initializer directly — no synthetic subject temporary needed, since
    // there's nothing to project a sub-value out of and nothing to test.
    if (let.else_body.empty() &&
        let.pattern->kind == ast::node_kind::binding_pattern) {
      const auto &binding =
          dynamic_cast<const ast::binding_pattern &>(*let.pattern);
      const auto symbol = declare_local(binding.name, (*initializer)->type);
      return one_stmt(ptr<hir_node>(make<hir_let>(
          let.span, symbol, binding.name, std::move(*initializer))));
    }

    // Either a destructuring pattern or a `let ... else`, both of which
    // need the initializer evaluated exactly once; bind it to a synthetic
    // subject first (mirrors `lower_match`'s `subject_symbol`), then let
    // `lower_pattern` desugar every binding or alias in the pattern into a
    // `hir_let` reading from that subject — spliced in after the
    // subject-binding node itself, below.
    auto init_type = checked_type_of(*let.initializer);
    if (!init_type.has_value()) {
      return std::unexpected(init_type.error());
    }
    const auto subject_symbol = mint_symbol();
    const auto subj_type = *init_type;
    const auto subject_span = let.initializer->span;
    const std::function<ptr<hir_expr>()> make_place =
        [subject_symbol, subj_type, subject_span]() -> ptr<hir_expr> {
      return ptr<hir_expr>(make<hir_local_ref>(subject_span, subj_type,
                                               subject_symbol,
                                               std::string("<let subject>")));
    };
    auto pending = std::vector<ptr<hir_node>>{};
    auto pattern = lower_pattern(*let.pattern, make_place, pending);
    if (!pattern.has_value()) {
      return std::unexpected(pattern.error());
    }

    auto result = ptr_vec<hir_node>{};
    if (let.else_body.empty()) {
      // Irrefutable: the pattern's *structural* shape (`pattern` above) is
      // discarded here — a plain destructuring `let` never branches on it,
      // only the bindings `lower_pattern` accumulated in `pending` matter.
      result.push_back(ptr<hir_node>(make<hir_let>(let.span, subject_symbol,
                                                   std::string("<let subject>"),
                                                   std::move(*initializer))));
    } else {
      // Fallible: unlike an irrefutable destructuring let, the structural
      // pattern *is* kept — `hir_let_else` needs it to know what to test
      // the subject against before falling through to `pending`'s
      // bindings.
      auto else_block = lower_block(let.else_body, let.span);
      if (!else_block.has_value()) {
        return std::unexpected(else_block.error());
      }
      result.push_back(ptr<hir_node>(
          make<hir_let_else>(let.span, subject_symbol, std::move(*initializer),
                             std::move(*pattern), std::move(*else_block))));
    }
    for (auto &binding : pending) {
      result.push_back(std::move(binding));
    }
    return result;
  }
  case ast::node_kind::expr_stmt: {
    const auto &expr_stmt = dynamic_cast<const ast::expr_stmt &>(node);
    if (expr_stmt.expr == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, expr_stmt.span,
                  "expression statement has no expression");
    }
    // `list.push(x)` as a bare statement — like `.len()`/`.as_bytes()`
    // (`lower_call`), `push` on a builtin `list[T]` has no `func_decl`
    // backing it (`semantic::check.cpp`'s `builtin_method_result`), so it
    // can't lower through the ordinary call path either. Unlike those two,
    // it has no expression-shaped HIR node — `hir_list_push` is a
    // statement, matching how it's actually written (`out.push(x)` on its
    // own line, never as a value) — so it's intercepted here rather than in
    // `lower_call`. Guarded on the absence of a `resolved_callees_` entry so
    // a real user-defined `push` method still wins.
    if (expr_stmt.expr->kind == ast::node_kind::call_expr) {
      const auto &call = dynamic_cast<const ast::call_expr &>(*expr_stmt.expr);
      if (call.callee != nullptr &&
          call.callee->kind == ast::node_kind::field_expr &&
          !checked_.resolved_callees.contains(&call)) {
        const auto &field = dynamic_cast<const ast::field_expr &>(*call.callee);
        if (field.field_name == "push" && field.object != nullptr &&
            call.args.size() == 1 && call.args.front().value != nullptr) {
          auto target = lower_expr(*field.object);
          if (!target.has_value()) {
            return std::unexpected(target.error());
          }
          auto value = lower_expr(*call.args.front().value);
          if (!value.has_value()) {
            return std::unexpected(value.error());
          }
          return one_stmt(ptr<hir_node>(make<hir_list_push>(
              expr_stmt.span, std::move(*target), std::move(*value))));
        }
      }
    }
    auto lowered = lower_expr(*expr_stmt.expr);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    return one_stmt(ptr<hir_node>(
        make<hir_expr_stmt>(expr_stmt.span, std::move(*lowered))));
  }
  case ast::node_kind::return_stmt: {
    const auto &ret = dynamic_cast<const ast::return_stmt &>(node);
    if (ret.value == nullptr) {
      return one_stmt(ptr<hir_node>(make<hir_return>(ret.span, nullptr)));
    }
    auto value = lower_expr(*ret.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    return one_stmt(
        ptr<hir_node>(make<hir_return>(ret.span, std::move(*value))));
  }
  case ast::node_kind::if_stmt: {
    const auto &if_s = dynamic_cast<const ast::if_stmt &>(node);
    auto lowered =
        lower_if(if_s.branches, if_s.else_body, if_s.span, k_unknown_type);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    return one_stmt(ptr<hir_node>(std::move(*lowered)));
  }
  case ast::node_kind::match_stmt: {
    const auto &match_s = dynamic_cast<const ast::match_stmt &>(node);
    if (match_s.subject == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, match_s.span,
                  "match statement has no subject");
    }
    auto lowered = lower_match(*match_s.subject, match_s.arms, match_s.span,
                               k_unknown_type);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    return one_stmt(ptr<hir_node>(std::move(*lowered)));
  }
  case ast::node_kind::var_stmt: {
    const auto &var = dynamic_cast<const ast::var_stmt &>(node);
    if (var.initializer == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, var.span,
                  "var binding has no initializer");
    }
    auto initializer = lower_expr(*var.initializer);
    if (!initializer.has_value()) {
      return std::unexpected(initializer.error());
    }
    const auto symbol = declare_local(var.name, (*initializer)->type);
    return one_stmt(ptr<hir_node>(make<hir_let>(
        var.span, symbol, var.name, std::move(*initializer), /*mut=*/true)));
  }
  case ast::node_kind::assign_stmt: {
    const auto &assign = dynamic_cast<const ast::assign_stmt &>(node);
    if (assign.target == nullptr || assign.value == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, assign.span,
                  "assignment is missing its target or value");
    }
    auto target = lower_expr(*assign.target);
    if (!target.has_value()) {
      return std::unexpected(target.error());
    }
    auto value = lower_expr(*assign.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    return one_stmt(ptr<hir_node>(hir::make<hir_assign>(
        assign.span, assign.op, std::move(*target), std::move(*value))));
  }
  case ast::node_kind::while_stmt: {
    const auto &while_s = dynamic_cast<const ast::while_stmt &>(node);
    if (while_s.let_pattern != nullptr && while_s.let_expr != nullptr) {
      return lower_while_let_stmt(while_s);
    }
    if (while_s.let_pattern != nullptr || while_s.let_expr != nullptr) {
      return fail(lowering_error_kind::unsupported_construct, while_s.span,
                  "`while let` is missing its pattern or its subject "
                  "expression");
    }
    if (while_s.condition == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, while_s.span,
                  "while loop has no condition");
    }
    auto condition = lower_expr(*while_s.condition);
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    auto body = lower_block(while_s.body, while_s.span);
    if (!body.has_value()) {
      return std::unexpected(body.error());
    }
    return one_stmt(ptr<hir_node>(make<hir_while>(
        while_s.span, std::move(*condition), std::move(*body))));
  }
  case ast::node_kind::for_stmt:
    return lower_for_stmt(dynamic_cast<const ast::for_stmt &>(node));
  default:
    return fail(lowering_error_kind::unsupported_construct, node.span,
                std::format("statement kind {} is not lowered by the first "
                            "milestone (spec/typed-ir-design.md)",
                            static_cast<int>(node.kind)));
  }
}

auto lowerer::lower_if(const std::vector<ast::if_branch> &branches,
                       const std::vector<ast::ptr<ast::node>> &else_body,
                       source_span span, type_id type)
    -> std::expected<ptr<hir_if>, lowering_error> {
  if (branches.empty()) {
    return fail(lowering_error_kind::unsupported_construct, span,
                "if has no branches");
  }
  auto hir_branches = std::vector<hir_if_branch>{};
  hir_branches.reserve(branches.size());
  for (const auto &branch : branches) {
    if (branch.let_pattern != nullptr || branch.let_expr != nullptr) {
      return fail(lowering_error_kind::unsupported_construct, branch.span,
                  "`if let` is not lowered by the first milestone");
    }
    if (branch.condition == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, branch.span,
                  "if branch is missing a condition");
    }
    auto condition = lower_expr(*branch.condition);
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    auto body = lower_block(branch.body, branch.span, type);
    if (!body.has_value()) {
      return std::unexpected(body.error());
    }
    hir_branches.push_back(hir_if_branch{.condition = std::move(*condition),
                                         .body = std::move(*body)});
  }
  auto else_block = ptr<hir_block>{};
  if (!else_body.empty()) {
    auto lowered_else = lower_block(else_body, span, type);
    if (!lowered_else.has_value()) {
      return std::unexpected(lowered_else.error());
    }
    else_block = std::move(*lowered_else);
  }
  return make<hir_if>(span, type, std::move(hir_branches),
                      std::move(else_block));
}

auto lowerer::lower_for_stmt(const ast::for_stmt &for_stmt)
    -> std::expected<ptr_vec<hir_node>, lowering_error> {
  if (for_stmt.iterable == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, for_stmt.span,
                "for loop has no iterable");
  }
  if (for_stmt.patterns.size() != 1 || for_stmt.patterns.front() == nullptr ||
      for_stmt.patterns.front()->has_error ||
      for_stmt.patterns.front()->kind != ast::node_kind::binding_pattern) {
    return fail(lowering_error_kind::unsupported_construct, for_stmt.span,
                "only a single plain loop variable is lowered yet (no "
                "destructuring patterns)");
  }
  const auto &loop_var =
      dynamic_cast<const ast::binding_pattern &>(*for_stmt.patterns.front());

  const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
      inner_stmts =
          [this,
           &for_stmt]() -> std::expected<ptr_vec<hir_node>, lowering_error> {
    return lower_guarded_for_body(for_stmt.guard.get(), for_stmt.body,
                                  for_stmt.span);
  };

  if (for_stmt.iterable->kind == ast::node_kind::binary_expr) {
    const auto &range =
        dynamic_cast<const ast::binary_expr &>(*for_stmt.iterable);
    if (range.op == ast::binary_op::range ||
        range.op == ast::binary_op::range_inclusive) {
      return lower_range_loop(for_stmt.span, range, loop_var, inner_stmts);
    }
  }

  auto iterable_type = checked_type_of(*for_stmt.iterable);
  if (!iterable_type.has_value()) {
    return std::unexpected(iterable_type.error());
  }
  const auto &iterable_entry = checked_.types.entry(*iterable_type);
  if (iterable_entry.kind == type_kind::builtin_generic_kind &&
      iterable_entry.name == "option") {
    return lower_option_loop(for_stmt.span, *for_stmt.iterable, *iterable_type,
                             loop_var, inner_stmts);
  }
  return lower_indexed_loop(for_stmt.span, *for_stmt.iterable, *iterable_type,
                            loop_var, inner_stmts);
}

auto lowerer::lower_guarded_for_body(
    const ast::expr *guard_ast,
    const std::vector<ast::ptr<ast::node>> &body_ast, source_span span)
    -> std::expected<ptr_vec<hir_node>, lowering_error> {
  auto lowered_body = lower_block(body_ast, span);
  if (!lowered_body.has_value()) {
    return std::unexpected(lowered_body.error());
  }

  auto result = ptr_vec<hir_node>{};
  if (guard_ast != nullptr) {
    // No `continue` exists in this language (it isn't even tokenized), so
    // "skip this iteration" is an ordinary conditional wrapping the body,
    // not a jump — see spec/iterator-protocol-design.md.
    auto guard = lower_expr(*guard_ast);
    if (!guard.has_value()) {
      return std::unexpected(guard.error());
    }
    auto branches = std::vector<hir_if_branch>{};
    branches.push_back(hir_if_branch{.condition = std::move(*guard),
                                     .body = std::move(*lowered_body)});
    result.push_back(ptr<hir_node>(
        make<hir_if>(span, k_unknown_type, std::move(branches), nullptr)));
  } else {
    for (auto &stmt_ptr : (*lowered_body)->stmts) {
      result.push_back(std::move(stmt_ptr));
    }
  }
  return result;
}

auto lowerer::lower_range_loop(
    source_span span, const ast::binary_expr &range,
    const ast::binding_pattern &loop_var,
    const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
        &inner_stmts) -> std::expected<ptr_vec<hir_node>, lowering_error> {
  if (range.lhs == nullptr || range.rhs == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, span,
                "range is missing a bound");
  }

  auto bound_type = checked_type_of(*range.lhs);
  if (!bound_type.has_value()) {
    return std::unexpected(bound_type.error());
  }
  auto start_value = lower_expr(*range.lhs);
  if (!start_value.has_value()) {
    return std::unexpected(start_value.error());
  }
  auto end_value = lower_expr(*range.rhs);
  if (!end_value.has_value()) {
    return std::unexpected(end_value.error());
  }

  auto result = ptr_vec<hir_node>{};

  const auto start_symbol = mint_symbol();
  result.push_back(ptr<hir_node>(make<hir_let>(range.lhs->span, start_symbol,
                                               std::string("<for start>"),
                                               std::move(*start_value))));
  const auto end_symbol = mint_symbol();
  result.push_back(ptr<hir_node>(make<hir_let>(range.rhs->span, end_symbol,
                                               std::string("<for end>"),
                                               std::move(*end_value))));
  const auto index_symbol = mint_symbol();
  result.push_back(ptr<hir_node>(make<hir_let>(
      span, index_symbol, std::string("<for index>"),
      ptr<hir_expr>(make<hir_local_ref>(range.lhs->span, *bound_type,
                                        start_symbol,
                                        std::string("<for start>"))),
      /*mut=*/true)));

  const auto condition_op = range.op == ast::binary_op::range_inclusive
                                ? ast::binary_op::lt_eq
                                : ast::binary_op::lt;
  auto condition = ptr<hir_expr>(hir::make<hir_binary>(
      span, checked_.types.bool_type(), condition_op,
      ptr<hir_expr>(make<hir_local_ref>(span, *bound_type, index_symbol,
                                        std::string("<for index>"))),
      ptr<hir_expr>(make<hir_local_ref>(span, *bound_type, end_symbol,
                                        std::string("<for end>")))));

  const auto element_type = *bound_type;
  const std::function<ptr<hir_expr>()> loop_var_value =
      [span, element_type, index_symbol]() -> ptr<hir_expr> {
    return ptr<hir_expr>(make<hir_local_ref>(span, element_type, index_symbol,
                                             std::string("<for index>")));
  };
  auto body_block = build_for_loop_body(span, index_symbol, *bound_type,
                                        loop_var, loop_var_value, inner_stmts);
  if (!body_block.has_value()) {
    return std::unexpected(body_block.error());
  }

  result.push_back(ptr<hir_node>(
      make<hir_while>(span, std::move(condition), std::move(*body_block))));

  return result;
}

auto lowerer::lower_indexed_loop(
    source_span span, const ast::expr &iterable, type_id iterable_type,
    const ast::binding_pattern &loop_var,
    const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
        &inner_stmts) -> std::expected<ptr_vec<hir_node>, lowering_error> {
  const auto &entry = checked_.types.entry(iterable_type);
  auto element_type = k_unknown_type;
  auto static_size = std::optional<uint64_t>{};
  auto is_array = false;
  if (entry.kind == type_kind::array_kind) {
    element_type = entry.result;
    static_size = entry.array_size;
    is_array = true;
    if (!static_size.has_value()) {
      return fail(lowering_error_kind::unsupported_construct, span,
                  "iterating an array whose length isn't statically known "
                  "is not lowered yet");
    }
  } else if (entry.kind == type_kind::builtin_generic_kind &&
             (entry.name == "list" || entry.name == "slice" ||
              entry.name == "slice_mut")) {
    element_type = entry.args.empty() ? k_unknown_type : entry.args[0];
  } else if (entry.kind == type_kind::builtin_kind && entry.name == "str") {
    element_type = checked_.types.char_type();
  } else {
    return fail(lowering_error_kind::unsupported_construct, span,
                "only range/array/list/slice/string/option iteration is "
                "lowered yet (see spec/iterator-protocol-design.md) — any "
                "user-defined iterable is still unsupported");
  }
  if (element_type == k_unknown_type || element_type == k_error_type) {
    return fail(lowering_error_kind::unresolved_type, span,
                "the container's element type did not resolve to a "
                "concrete type");
  }

  auto container_value = lower_expr(iterable);
  if (!container_value.has_value()) {
    return std::unexpected(container_value.error());
  }

  auto result = ptr_vec<hir_node>{};
  const auto container_symbol = mint_symbol();
  result.push_back(ptr<hir_node>(make<hir_let>(iterable.span, container_symbol,
                                               std::string("<for container>"),
                                               std::move(*container_value))));

  const auto usize_type = checked_.types.usize_type();
  const auto index_symbol = mint_symbol();
  result.push_back(ptr<hir_node>(make<hir_let>(
      span, index_symbol, std::string("<for index>"),
      ptr<hir_expr>(make<hir_literal>(span, usize_type, token_kind::int_lit,
                                      std::string("0"))),
      /*mut=*/true)));

  auto end_value = ptr<hir_expr>{};
  if (is_array) {
    end_value = ptr<hir_expr>(make<hir_literal>(
        span, usize_type, token_kind::int_lit, std::to_string(*static_size)));
  } else {
    end_value = ptr<hir_expr>(make<hir_container_len>(
        span, usize_type,
        ptr<hir_expr>(make<hir_local_ref>(iterable.span, iterable_type,
                                          container_symbol,
                                          std::string("<for container>")))));
  }
  auto condition = ptr<hir_expr>(hir::make<hir_binary>(
      span, checked_.types.bool_type(), ast::binary_op::lt,
      ptr<hir_expr>(make<hir_local_ref>(span, usize_type, index_symbol,
                                        std::string("<for index>"))),
      std::move(end_value)));

  const std::function<ptr<hir_expr>()> loop_var_value =
      [span, iterable_type, element_type, container_symbol, usize_type,
       index_symbol]() -> ptr<hir_expr> {
    return ptr<hir_expr>(make<hir_index>(
        span, element_type,
        ptr<hir_expr>(make<hir_local_ref>(span, iterable_type, container_symbol,
                                          std::string("<for container>"))),
        ptr<hir_expr>(make<hir_local_ref>(span, usize_type, index_symbol,
                                          std::string("<for index>")))));
  };
  auto body_block = build_for_loop_body(span, index_symbol, usize_type,
                                        loop_var, loop_var_value, inner_stmts);
  if (!body_block.has_value()) {
    return std::unexpected(body_block.error());
  }

  result.push_back(ptr<hir_node>(
      make<hir_while>(span, std::move(condition), std::move(*body_block))));

  return result;
}

auto lowerer::build_for_loop_body(
    source_span span, symbol_id index_symbol, type_id counter_type,
    const ast::binding_pattern &loop_var,
    const std::function<ptr<hir_expr>()> &loop_var_value,
    const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
        &inner_stmts) -> std::expected<ptr<hir_block>, lowering_error> {
  push_scope();
  auto loop_var_place = loop_var_value();
  const auto loop_var_symbol =
      declare_local(loop_var.name, loop_var_place->type);
  auto body_stmts = ptr_vec<hir_node>{};
  body_stmts.push_back(ptr<hir_node>(make<hir_let>(
      span, loop_var_symbol, loop_var.name, std::move(loop_var_place))));

  auto inner = inner_stmts();
  if (!inner.has_value()) {
    pop_scope();
    return std::unexpected(inner.error());
  }
  for (auto &stmt_ptr : *inner) {
    body_stmts.push_back(std::move(stmt_ptr));
  }

  body_stmts.push_back(ptr<hir_node>(hir::make<hir_assign>(
      span, ast::assign_op::add_assign,
      ptr<hir_expr>(make<hir_local_ref>(span, counter_type, index_symbol,
                                        std::string("<for index>"))),
      ptr<hir_expr>(make<hir_literal>(span, counter_type, token_kind::int_lit,
                                      std::string("1"))))));

  auto body_block =
      make<hir_block>(span, k_unknown_type, std::move(body_stmts));
  pop_scope();
  return body_block;
}

auto lowerer::lower_option_loop(
    source_span span, const ast::expr &iterable, type_id iterable_type,
    const ast::binding_pattern &loop_var,
    const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
        &inner_stmts) -> std::expected<ptr_vec<hir_node>, lowering_error> {
  const auto &entry = checked_.types.entry(iterable_type);
  const auto element_type = entry.args.empty() ? k_unknown_type : entry.args[0];
  if (element_type == k_unknown_type || element_type == k_error_type) {
    return fail(lowering_error_kind::unresolved_type, span,
                "the option's element type did not resolve to a concrete "
                "type");
  }

  auto subject_value = lower_expr(iterable);
  if (!subject_value.has_value()) {
    return std::unexpected(subject_value.error());
  }
  const auto subject_symbol = mint_symbol();
  const auto subject_span = iterable.span;

  push_scope();
  const auto loop_var_symbol = declare_local(loop_var.name, element_type);
  auto some_stmts = ptr_vec<hir_node>{};
  some_stmts.push_back(ptr<hir_node>(make<hir_let>(
      span, loop_var_symbol, loop_var.name,
      ptr<hir_expr>(make<hir_variant_payload>(
          span, element_type,
          ptr<hir_expr>(make<hir_local_ref>(subject_span, iterable_type,
                                            subject_symbol,
                                            std::string("<for subject>"))),
          std::string("some"), size_t{0})))));

  auto inner = inner_stmts();
  if (!inner.has_value()) {
    pop_scope();
    return std::unexpected(inner.error());
  }
  for (auto &stmt_ptr : *inner) {
    some_stmts.push_back(std::move(stmt_ptr));
  }
  auto some_body = make<hir_block>(span, k_unknown_type, std::move(some_stmts));
  pop_scope();

  auto some_args = ptr_vec<hir_pattern>{};
  some_args.push_back(ptr<hir_pattern>(make<hir_wildcard_pattern>(span)));
  auto some_arm =
      hir_match_arm{.pattern = ptr<hir_pattern>(make<hir_constructor_pattern>(
                        span, std::string("some"), std::move(some_args))),
                    .guard = nullptr,
                    .body = std::move(some_body)};

  auto none_arm = hir_match_arm{
      .pattern = ptr<hir_pattern>(make<hir_constructor_pattern>(
          span, std::string("none"), ptr_vec<hir_pattern>{})),
      .guard = nullptr,
      .body = make<hir_block>(span, k_unknown_type, ptr_vec<hir_node>{})};

  auto arms = std::vector<hir_match_arm>{};
  arms.push_back(std::move(some_arm));
  arms.push_back(std::move(none_arm));

  auto match_expr = ptr<hir_expr>(
      make<hir_match>(span, k_unknown_type, std::move(*subject_value),
                      subject_symbol, std::move(arms)));

  auto result = ptr_vec<hir_node>{};
  result.push_back(
      ptr<hir_node>(make<hir_expr_stmt>(span, std::move(match_expr))));
  return result;
}

auto lowerer::lower_for_expr(const ast::for_expr &for_expr)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(for_expr);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  const auto &result_entry = checked_.types.entry(*type);
  if (result_entry.kind != type_kind::builtin_generic_kind ||
      result_entry.name != "list" || result_entry.args.empty()) {
    return fail(lowering_error_kind::unresolved_type, for_expr.span,
                "comprehension did not resolve to a concrete list[T] "
                "result");
  }
  if (for_expr.yield_expr == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, for_expr.span,
                "comprehension has no yield expression");
  }
  if (for_expr.clauses.empty()) {
    return fail(lowering_error_kind::unsupported_construct, for_expr.span,
                "comprehension has no iteration clauses");
  }

  const auto span = for_expr.span;
  const auto list_type = *type;
  const auto acc_symbol = mint_symbol();

  auto stmts = ptr_vec<hir_node>{};
  stmts.push_back(ptr<hir_node>(make<hir_let>(
      span, acc_symbol, std::string("<comprehension result>"),
      ptr<hir_expr>(make<hir_array_init>(span, list_type, ptr_vec<hir_expr>{},
                                         nullptr, nullptr)),
      /*mut=*/true)));

  const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
      innermost = [this, &for_expr, acc_symbol, list_type,
                   span]() -> std::expected<ptr_vec<hir_node>, lowering_error> {
    auto yield_value = lower_expr(*for_expr.yield_expr);
    if (!yield_value.has_value()) {
      return std::unexpected(yield_value.error());
    }
    auto push_stmt = ptr<hir_node>(
        make<hir_list_push>(span,
                            ptr<hir_expr>(make<hir_local_ref>(
                                span, list_type, acc_symbol,
                                std::string("<comprehension result>"))),
                            std::move(*yield_value)));

    auto result = ptr_vec<hir_node>{};
    if (for_expr.guard != nullptr) {
      auto guard = lower_expr(*for_expr.guard);
      if (!guard.has_value()) {
        return std::unexpected(guard.error());
      }
      auto guarded_stmts = ptr_vec<hir_node>{};
      guarded_stmts.push_back(std::move(push_stmt));
      auto guarded_block =
          make<hir_block>(span, k_unknown_type, std::move(guarded_stmts));
      auto branches = std::vector<hir_if_branch>{};
      branches.push_back(hir_if_branch{.condition = std::move(*guard),
                                       .body = std::move(guarded_block)});
      result.push_back(ptr<hir_node>(
          make<hir_if>(span, k_unknown_type, std::move(branches), nullptr)));
    } else {
      result.push_back(std::move(push_stmt));
    }
    return result;
  };

  auto loop_stmts =
      lower_comprehension_clause(for_expr.clauses, 0, span, innermost);
  if (!loop_stmts.has_value()) {
    return std::unexpected(loop_stmts.error());
  }
  for (auto &stmt_ptr : *loop_stmts) {
    stmts.push_back(std::move(stmt_ptr));
  }

  stmts.push_back(ptr<hir_node>(
      make<hir_expr_stmt>(span, ptr<hir_expr>(make<hir_local_ref>(
                                    span, list_type, acc_symbol,
                                    std::string("<comprehension result>"))))));

  return ok_expr(make<hir_block>(span, list_type, std::move(stmts)));
}

auto lowerer::lower_comprehension_clause(
    const std::vector<ast::for_expr::iter_clause> &clauses, size_t index,
    source_span fallback_span,
    const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
        &innermost) -> std::expected<ptr_vec<hir_node>, lowering_error> {
  if (index >= clauses.size()) {
    return innermost();
  }
  const auto &clause = clauses[index];
  if (clause.iterable == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, fallback_span,
                "comprehension clause has no iterable");
  }
  if (clause.patterns.size() != 1 || clause.patterns.front() == nullptr ||
      clause.patterns.front()->has_error ||
      clause.patterns.front()->kind != ast::node_kind::binding_pattern) {
    return fail(lowering_error_kind::unsupported_construct,
                clause.iterable->span,
                "only a single plain loop variable is lowered yet for "
                "comprehension clauses (no destructuring patterns)");
  }
  const auto &loop_var =
      dynamic_cast<const ast::binding_pattern &>(*clause.patterns.front());
  const auto span = clause.iterable->span;

  const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
      nested =
          [this, &clauses, index, fallback_span,
           &innermost]() -> std::expected<ptr_vec<hir_node>, lowering_error> {
    return lower_comprehension_clause(clauses, index + 1, fallback_span,
                                      innermost);
  };

  if (clause.iterable->kind == ast::node_kind::binary_expr) {
    const auto &range =
        dynamic_cast<const ast::binary_expr &>(*clause.iterable);
    if (range.op == ast::binary_op::range ||
        range.op == ast::binary_op::range_inclusive) {
      return lower_range_loop(span, range, loop_var, nested);
    }
  }

  auto iterable_type = checked_type_of(*clause.iterable);
  if (!iterable_type.has_value()) {
    return std::unexpected(iterable_type.error());
  }
  const auto &entry = checked_.types.entry(*iterable_type);
  if (entry.kind == type_kind::builtin_generic_kind && entry.name == "option") {
    return lower_option_loop(span, *clause.iterable, *iterable_type, loop_var,
                             nested);
  }
  return lower_indexed_loop(span, *clause.iterable, *iterable_type, loop_var,
                            nested);
}

auto lowerer::lower_while_let_stmt(const ast::while_stmt &while_stmt)
    -> std::expected<ptr_vec<hir_node>, lowering_error> {
  auto subject_type = checked_type_of(*while_stmt.let_expr);
  if (!subject_type.has_value()) {
    return std::unexpected(subject_type.error());
  }
  auto subject_value = lower_expr(*while_stmt.let_expr);
  if (!subject_value.has_value()) {
    return std::unexpected(subject_value.error());
  }

  const auto subject_symbol = mint_symbol();
  const auto subj_type = *subject_type;
  const auto subject_span = while_stmt.let_expr->span;
  const std::function<ptr<hir_expr>()> make_place =
      [subject_symbol, subj_type, subject_span]() -> ptr<hir_expr> {
    return ptr<hir_expr>(make<hir_local_ref>(subject_span, subj_type,
                                             subject_symbol,
                                             std::string("<while subject>")));
  };

  push_scope();
  auto pending = std::vector<ptr<hir_node>>{};
  auto pattern = lower_pattern(*while_stmt.let_pattern, make_place, pending);
  if (!pattern.has_value()) {
    pop_scope();
    return std::unexpected(pattern.error());
  }

  auto lowered_body = lower_block(while_stmt.body, while_stmt.span);
  if (!lowered_body.has_value()) {
    pop_scope();
    return std::unexpected(lowered_body.error());
  }
  pop_scope();

  auto body_stmts = std::move(pending);
  for (auto &stmt_ptr : (*lowered_body)->stmts) {
    body_stmts.push_back(std::move(stmt_ptr));
  }
  (*lowered_body)->stmts = std::move(body_stmts);

  auto result = ptr_vec<hir_node>{};
  result.push_back(ptr<hir_node>(make<hir_while_let>(
      while_stmt.span, std::move(*subject_value), subject_symbol,
      std::move(*pattern), std::move(*lowered_body))));
  return result;
}

auto lowerer::lower_pattern(const ast::node &pattern,
                            const std::function<ptr<hir_expr>()> &make_place,
                            std::vector<ptr<hir_node>> &pending)
    -> std::expected<ptr<hir_pattern>, lowering_error> {
  if (pattern.has_error) {
    return fail(lowering_error_kind::unsupported_construct, pattern.span,
                "pattern carries a parse/recovery error and cannot be lowered");
  }
  switch (pattern.kind) {
  case ast::node_kind::wildcard_pattern:
    return ptr<hir_pattern>(make<hir_wildcard_pattern>(pattern.span));
  case ast::node_kind::literal_pattern: {
    const auto &lit = dynamic_cast<const ast::literal_pattern &>(pattern);
    return ptr<hir_pattern>(
        make<hir_literal_pattern>(pattern.span, lit.lit_kind, lit.value));
  }
  case ast::node_kind::binding_pattern: {
    const auto &binding = dynamic_cast<const ast::binding_pattern &>(pattern);
    auto place = make_place();
    const auto symbol = declare_local(binding.name, place->type);
    pending.push_back(ptr<hir_node>(
        make<hir_let>(pattern.span, symbol, binding.name, std::move(place))));
    return ptr<hir_pattern>(make<hir_wildcard_pattern>(pattern.span));
  }
  case ast::node_kind::group_pattern: {
    const auto &group = dynamic_cast<const ast::group_pattern &>(pattern);
    if (group.inner == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, pattern.span,
                  "grouped pattern has no inner pattern");
    }
    auto inner = lower_pattern(*group.inner, make_place, pending);
    if (!inner.has_value()) {
      return std::unexpected(inner.error());
    }
    if (group.alias.has_value()) {
      auto place = make_place();
      const auto symbol = declare_local(*group.alias, place->type);
      pending.push_back(ptr<hir_node>(
          make<hir_let>(pattern.span, symbol, *group.alias, std::move(place))));
    }
    return inner;
  }
  case ast::node_kind::ref_pattern: {
    // `&pattern` doesn't change which value is matched at the HIR level —
    // there's no separate "place vs. reference to a place" distinction yet
    // — so this is a transparent pass-through to the inner pattern.
    const auto &ref = dynamic_cast<const ast::ref_pattern &>(pattern);
    if (ref.inner == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, pattern.span,
                  "reference pattern has no inner pattern");
    }
    return lower_pattern(*ref.inner, make_place, pending);
  }
  case ast::node_kind::or_pattern: {
    const auto &alt = dynamic_cast<const ast::or_pattern &>(pattern);
    if (alt.alternatives.empty()) {
      return fail(lowering_error_kind::unsupported_construct, pattern.span,
                  "`|` pattern has no alternatives");
    }
    auto alternatives = ptr_vec<hir_pattern>{};
    alternatives.reserve(alt.alternatives.size());
    for (const auto &alternative : alt.alternatives) {
      if (alternative == nullptr) {
        return fail(lowering_error_kind::unsupported_construct, pattern.span,
                    "`|` pattern has a missing alternative");
      }
      auto local_pending = std::vector<ptr<hir_node>>{};
      auto lowered = lower_pattern(*alternative, make_place, local_pending);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      if (!local_pending.empty()) {
        return fail(lowering_error_kind::unsupported_construct,
                    alternative->span,
                    "a name bound (directly, or via destructuring) inside "
                    "one `|` alternative is not supported by this milestone");
      }
      alternatives.push_back(std::move(*lowered));
    }
    return ptr<hir_pattern>(
        make<hir_or_pattern>(pattern.span, std::move(alternatives)));
  }
  case ast::node_kind::tuple_pattern: {
    const auto &tuple = dynamic_cast<const ast::tuple_pattern &>(pattern);
    auto elements = ptr_vec<hir_pattern>{};
    elements.reserve(tuple.elements.size());
    for (std::size_t i = 0; i < tuple.elements.size(); ++i) {
      const auto &element_ast = tuple.elements[i];
      if (element_ast == nullptr) {
        return fail(lowering_error_kind::unsupported_construct, pattern.span,
                    "tuple pattern has a missing element");
      }
      auto element_type = checked_type_of(*element_ast);
      if (!element_type.has_value()) {
        return std::unexpected(element_type.error());
      }
      const auto elem_type = *element_type;
      const auto elem_span = element_ast->span;
      auto element_place = [make_place, elem_type, i,
                            elem_span]() -> ptr<hir_expr> {
        return {make<hir_tuple_index>(elem_span, elem_type, make_place(), i)};
      };
      auto lowered = lower_pattern(*element_ast, element_place, pending);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      elements.push_back(std::move(*lowered));
    }
    return ptr<hir_pattern>(
        make<hir_tuple_pattern>(pattern.span, std::move(elements)));
  }
  case ast::node_kind::struct_pattern: {
    const auto &struct_pat = dynamic_cast<const ast::struct_pattern &>(pattern);
    auto fields = std::vector<hir_struct_pattern_field>{};
    fields.reserve(struct_pat.fields.size());
    for (const auto &field : struct_pat.fields) {
      if (field.is_rest) {
        continue;
      }
      const auto field_span = field.span;
      const auto field_name = field.name;
      if (field.pattern != nullptr) {
        auto field_type = checked_type_of(*field.pattern);
        if (!field_type.has_value()) {
          return std::unexpected(field_type.error());
        }
        const auto ftype = *field_type;
        auto field_place = [make_place, ftype, field_name,
                            field_span]() -> ptr<hir_expr> {
          return {make<hir_field>(field_span, ftype, make_place(), field_name)};
        };
        auto lowered = lower_pattern(*field.pattern, field_place, pending);
        if (!lowered.has_value()) {
          return std::unexpected(lowered.error());
        }
        fields.push_back(hir_struct_pattern_field{
            .name = field_name, .pattern = std::move(*lowered)});
        continue;
      }
      if (field_name.empty()) {
        continue;
      }
      // Shorthand `{x}` has no sub-pattern node to key `node_types` against,
      // so its type lives in the dedicated `struct_pattern_field_types` map
      // instead (see checked_types's doc comment). Desugars exactly like a
      // plain binding: a wildcard structural match plus a synthetic let.
      const auto found = checked_.struct_pattern_field_types.find(&field);
      if (found == checked_.struct_pattern_field_types.end() ||
          found->second == k_unknown_type || found->second == k_error_type) {
        return fail(lowering_error_kind::unresolved_type, field_span,
                    "no concrete checked type is available for this struct "
                    "pattern field; lowering only accepts fully "
                    "type-checked, fully-annotated code (spec/"
                    "typed-ir-design.md Decision 1)");
      }
      const auto ftype = found->second;
      const auto symbol = declare_local(field_name, ftype);
      pending.push_back(ptr<hir_node>(
          make<hir_let>(field_span, symbol, field_name,
                        ptr<hir_expr>(make<hir_field>(
                            field_span, ftype, make_place(), field_name)))));
      fields.push_back(hir_struct_pattern_field{
          .name = field_name,
          .pattern = ptr<hir_pattern>(make<hir_wildcard_pattern>(field_span))});
    }
    return ptr<hir_pattern>(
        make<hir_struct_pattern>(pattern.span, std::move(fields)));
  }
  case ast::node_kind::constructor_pattern: {
    const auto &ctor = dynamic_cast<const ast::constructor_pattern &>(pattern);
    auto args = ptr_vec<hir_pattern>{};
    args.reserve(ctor.args.size());
    for (size_t i = 0; i < ctor.args.size(); ++i) {
      const auto &arg_ast = ctor.args[i];
      if (arg_ast == nullptr) {
        return fail(lowering_error_kind::unsupported_construct, pattern.span,
                    "constructor pattern has a missing argument");
      }
      auto arg_type = checked_type_of(*arg_ast);
      if (!arg_type.has_value()) {
        return std::unexpected(arg_type.error());
      }
      const auto atype = *arg_type;
      const auto arg_span = arg_ast->span;
      const auto variant = ctor.name;
      auto arg_place = [make_place, atype, variant, i,
                        arg_span]() -> ptr<hir_expr> {
        return {make<hir_variant_payload>(arg_span, atype, make_place(),
                                          variant, i)};
      };
      auto lowered = lower_pattern(*arg_ast, arg_place, pending);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      args.push_back(std::move(*lowered));
    }
    return ptr<hir_pattern>(make<hir_constructor_pattern>(
        pattern.span, ctor.name, std::move(args)));
  }
  case ast::node_kind::option_pattern: {
    // `some(inner)` — the only form `ast::option_pattern` represents
    // (`none` arrives as an ordinary zero-arg `constructor_pattern`); lowers
    // to the same `hir_constructor_pattern` shape a user sum type would.
    const auto &option = dynamic_cast<const ast::option_pattern &>(pattern);
    auto args = ptr_vec<hir_pattern>{};
    if (option.inner != nullptr) {
      auto inner_type = checked_type_of(*option.inner);
      if (!inner_type.has_value()) {
        return std::unexpected(inner_type.error());
      }
      const auto itype = *inner_type;
      const auto inner_span = option.inner->span;
      auto inner_place = [make_place, itype, inner_span]() -> ptr<hir_expr> {
        return ptr<hir_expr>(make<hir_variant_payload>(
            inner_span, itype, make_place(), std::string("some"), size_t{0}));
      };
      auto lowered = lower_pattern(*option.inner, inner_place, pending);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      args.push_back(std::move(*lowered));
    }
    return ptr<hir_pattern>(make<hir_constructor_pattern>(
        pattern.span, std::string("some"), std::move(args)));
  }
  case ast::node_kind::result_pattern: {
    const auto &result = dynamic_cast<const ast::result_pattern &>(pattern);
    const auto variant = result.result_kind == ast::option_result_kind::err
                             ? std::string("err")
                             : std::string("ok");
    auto args = ptr_vec<hir_pattern>{};
    if (result.inner != nullptr) {
      auto inner_type = checked_type_of(*result.inner);
      if (!inner_type.has_value()) {
        return std::unexpected(inner_type.error());
      }
      const auto itype = *inner_type;
      const auto inner_span = result.inner->span;
      auto inner_place = [make_place, itype, variant,
                          inner_span]() -> ptr<hir_expr> {
        return ptr<hir_expr>(make<hir_variant_payload>(
            inner_span, itype, make_place(), variant, size_t{0}));
      };
      auto lowered = lower_pattern(*result.inner, inner_place, pending);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      args.push_back(std::move(*lowered));
    }
    return ptr<hir_pattern>(
        make<hir_constructor_pattern>(pattern.span, variant, std::move(args)));
  }
  case ast::node_kind::range_pattern: {
    const auto &range = dynamic_cast<const ast::range_pattern &>(pattern);
    auto start = ptr<hir_expr>{};
    if (range.start != nullptr) {
      auto lowered = lower_expr(*range.start);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      start = std::move(*lowered);
    }
    auto end = ptr<hir_expr>{};
    if (range.end != nullptr) {
      auto lowered = lower_expr(*range.end);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      end = std::move(*lowered);
    }
    return ptr<hir_pattern>(make<hir_range_pattern>(
        pattern.span, std::move(start), std::move(end), range.inclusive));
  }
  case ast::node_kind::array_pattern: {
    // Shaped exactly like tuple_pattern above — this grammar has no
    // rest/slice-capture pattern syntax (see hir_array_pattern's doc
    // comment), so an array/list/slice pattern is just a fixed-arity
    // positional destructure, reusing hir_tuple_index for element
    // projections the same way a tuple pattern does.
    const auto &array = dynamic_cast<const ast::array_pattern &>(pattern);
    auto elements = ptr_vec<hir_pattern>{};
    elements.reserve(array.elements.size());
    for (std::size_t i = 0; i < array.elements.size(); ++i) {
      const auto &element_ast = array.elements[i];
      if (element_ast == nullptr) {
        return fail(lowering_error_kind::unsupported_construct, pattern.span,
                    "array pattern has a missing element");
      }
      auto element_type = checked_type_of(*element_ast);
      if (!element_type.has_value()) {
        return std::unexpected(element_type.error());
      }
      const auto elem_type = *element_type;
      const auto elem_span = element_ast->span;
      auto element_place = [make_place, elem_type, i,
                            elem_span]() -> ptr<hir_expr> {
        return {make<hir_tuple_index>(elem_span, elem_type, make_place(), i)};
      };
      auto lowered = lower_pattern(*element_ast, element_place, pending);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      elements.push_back(std::move(*lowered));
    }
    return ptr<hir_pattern>(
        make<hir_array_pattern>(pattern.span, std::move(elements)));
  }
  default:
    return fail(lowering_error_kind::unsupported_construct, pattern.span,
                std::format("pattern kind {} is not lowered by this pass",
                            static_cast<int>(pattern.kind)));
  }
}

auto lowerer::lower_match(const ast::expr &subject_ast,
                          const std::vector<ast::match_arm> &arms,
                          source_span span, type_id type)
    -> std::expected<ptr<hir_match>, lowering_error> {
  auto subject_type = checked_type_of(subject_ast);
  if (!subject_type.has_value()) {
    return std::unexpected(subject_type.error());
  }
  auto subject = lower_expr(subject_ast);
  if (!subject.has_value()) {
    return std::unexpected(subject.error());
  }
  const auto subject_symbol = mint_symbol();
  const auto subj_type = *subject_type;
  const auto subject_span = subject_ast.span;
  const std::function<ptr<hir_expr>()> make_subject_place =
      [subject_symbol, subj_type, subject_span]() -> ptr<hir_expr> {
    return {make<hir_local_ref>(subject_span, subj_type, subject_symbol,
                                std::string("<match subject>"))};
  };

  auto hir_arms = std::vector<hir_match_arm>{};
  hir_arms.reserve(arms.size());
  for (const auto &arm : arms) {
    if (arm.has_error) {
      return fail(lowering_error_kind::unsupported_construct, arm.span,
                  "match arm carries a parse/recovery error and cannot be "
                  "lowered");
    }
    if (arm.pattern == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, arm.span,
                  "match arm has no pattern");
    }
    push_scope();
    auto pending = std::vector<ptr<hir_node>>{};
    auto pattern = lower_pattern(*arm.pattern, make_subject_place, pending);
    if (!pattern.has_value()) {
      pop_scope();
      return std::unexpected(pattern.error());
    }

    auto guard = ptr<hir_expr>{};
    if (arm.guard != nullptr) {
      auto lowered_guard = lower_expr(*arm.guard);
      if (!lowered_guard.has_value()) {
        pop_scope();
        return std::unexpected(lowered_guard.error());
      }
      guard = std::move(*lowered_guard);
    }

    auto body = ptr<hir_block>{};
    if (arm.body_expr != nullptr) {
      auto value = lower_expr(*arm.body_expr);
      if (!value.has_value()) {
        pop_scope();
        return std::unexpected(value.error());
      }
      auto stmts = std::move(pending);
      stmts.push_back(ptr<hir_node>(
          make<hir_expr_stmt>(arm.body_expr->span, std::move(*value))));
      body = make<hir_block>(arm.span, k_unknown_type, std::move(stmts));
    } else {
      auto lowered_body = lower_block(arm.body_stmts, arm.span, type);
      if (!lowered_body.has_value()) {
        pop_scope();
        return std::unexpected(lowered_body.error());
      }
      body = std::move(*lowered_body);
      if (!pending.empty()) {
        auto merged = std::move(pending);
        for (auto &stmt_ptr : body->stmts) {
          merged.push_back(std::move(stmt_ptr));
        }
        body->stmts = std::move(merged);
      }
    }
    pop_scope();

    hir_arms.push_back(hir_match_arm{.pattern = std::move(*pattern),
                                     .guard = std::move(guard),
                                     .body = std::move(body)});
  }

  return make<hir_match>(span, type, std::move(*subject), subject_symbol,
                         std::move(hir_arms));
}

auto lowerer::lower_function(const ast::func_decl &decl)
    -> std::expected<ptr<hir_function>, lowering_error> {
  if (decl.has_error) {
    return fail(lowering_error_kind::unsupported_construct, decl.span,
                "function declaration carries a parse/recovery error and "
                "cannot be lowered");
  }
  if (!decl.type_params.empty()) {
    return fail(lowering_error_kind::unsupported_construct, decl.span,
                "generic functions are not lowered until monomorphization "
                "exists (spec/typed-ir-design.md Decision 2, phase 5)");
  }
  if (decl.return_type == nullptr) {
    return fail(lowering_error_kind::missing_return_type, decl.span,
                std::format("function `{}` has no declared return type; the "
                            "first lowering milestone only lowers explicitly "
                            "annotated signatures",
                            decl.name));
  }

  push_scope();

  auto params = std::vector<hir_param>{};
  // Bindings a destructuring parameter pattern (e.g. `(a, b): (int32,
  // int32)`) accumulates — spliced onto the front of the function body
  // below, exactly like a match arm's pattern bindings (`lower_match`).
  auto param_prelude = ptr_vec<hir_node>{};
  params.reserve(decl.params.size());
  for (size_t i = 0; i < decl.params.size(); ++i) {
    const auto &param = decl.params[i];
    // `self`/`mut self` is always unannotated by design (its type is the
    // enclosing impl/extend target, not written out) — `check_function`
    // (`semantic/check.cpp`) already resolves and records its type from
    // `self_type_` regardless, so `checked_type_of` below has a real answer
    // even though there's no `type_annotation` node to point at.
    if (param.type_annotation == nullptr && !(i == 0 && is_self_param(param))) {
      pop_scope();
      return fail(lowering_error_kind::unannotated_parameter, param.span,
                  std::format("parameter in function `{}` has no explicit "
                              "type annotation; the first lowering milestone "
                              "only lowers explicitly annotated signatures",
                              decl.name));
    }
    if (param.default_value != nullptr) {
      pop_scope();
      return fail(lowering_error_kind::unsupported_construct, param.span,
                  "default parameter values are not lowered by the first "
                  "milestone");
    }
    if (param.pattern == nullptr || param.pattern->has_error) {
      pop_scope();
      return fail(lowering_error_kind::unsupported_construct, param.span,
                  "parameter has no usable pattern");
    }
    auto type = checked_type_of(*param.pattern);
    if (!type.has_value()) {
      pop_scope();
      return std::unexpected(type.error());
    }

    if (param.pattern->kind == ast::node_kind::binding_pattern) {
      const auto &binding =
          dynamic_cast<const ast::binding_pattern &>(*param.pattern);
      const auto symbol = declare_local(binding.name, *type);
      params.push_back(
          hir_param{.symbol = symbol, .name = binding.name, .type = *type});
      continue;
    }

    // A destructuring parameter pattern has no single surface name, so the
    // parameter itself gets a synthetic identity; `lower_pattern` desugars
    // every binding/alias inside the pattern into a `hir_let` reading from
    // that synthetic parameter, collected into `param_prelude`.
    const auto symbol = mint_symbol();
    const auto ptype = *type;
    const auto pspan = param.span;
    const std::function<ptr<hir_expr>()> make_place =
        [symbol, ptype, pspan]() -> ptr<hir_expr> {
      return ptr<hir_expr>(
          make<hir_local_ref>(pspan, ptype, symbol, std::string("<param>")));
    };
    auto pending = std::vector<ptr<hir_node>>{};
    auto pattern = lower_pattern(*param.pattern, make_place, pending);
    if (!pattern.has_value()) {
      pop_scope();
      return std::unexpected(pattern.error());
    }
    params.push_back(hir_param{
        .symbol = symbol, .name = std::format("<param {}>", i), .type = *type});
    for (auto &binding : pending) {
      param_prelude.push_back(std::move(binding));
    }
  }

  auto return_type = checked_type_of(*decl.return_type);
  if (!return_type.has_value()) {
    pop_scope();
    return std::unexpected(return_type.error());
  }

  auto body = std::expected<ptr<hir_block>, lowering_error>{};
  if (decl.body_expr != nullptr) {
    auto expr_type = checked_type_of(*decl.body_expr);
    if (!expr_type.has_value()) {
      pop_scope();
      return std::unexpected(expr_type.error());
    }
    auto value = lower_expr(*decl.body_expr);
    if (!value.has_value()) {
      pop_scope();
      return std::unexpected(value.error());
    }
    auto ret = ptr<hir_node>(
        make<hir_return>(decl.body_expr->span, std::move(*value)));
    auto stmts = std::move(param_prelude);
    stmts.push_back(std::move(ret));
    body =
        make<hir_block>(decl.body_expr->span, *return_type, std::move(stmts));
  } else {
    body = lower_block(decl.body_stmts, decl.span, *return_type);
    if (body.has_value() && !param_prelude.empty()) {
      auto merged = std::move(param_prelude);
      for (auto &stmt_ptr : (*body)->stmts) {
        merged.push_back(std::move(stmt_ptr));
      }
      (*body)->stmts = std::move(merged);
    }
  }
  pop_scope();
  if (!body.has_value()) {
    return std::unexpected(body.error());
  }

  return make<hir_function>(decl.span, decl.name, std::move(params),
                            *return_type, std::move(*body));
}

} // namespace

auto lower_function(const ast::func_decl &decl,
                    const semantic::checked_types &checked)
    -> std::expected<ptr<hir_function>, lowering_error> {
  auto walker = lowerer(checked);
  return walker.lower_function(decl);
}

/// The bare target-type name an `impl ... for <type>` block lowers its
/// associated functions under (`TargetType::method`, matching
/// `resolved_callee::impl_target_type` in `check.cpp`) — only a simple,
/// non-generic named type is handled; anything else (a generic
/// instantiation, a ref/tuple/slice target, ...) isn't a shape
/// `infer_qualified_call` resolves a type-qualified call against either, so
/// there is nothing to lower it for yet.
[[nodiscard]] auto simple_impl_target_name(const ast::type_expr *for_type)
    -> std::optional<std::string> {
  if (for_type == nullptr || for_type->kind != ast::node_kind::named_type) {
    return std::nullopt;
  }
  const auto &named = dynamic_cast<const ast::named_type &>(*for_type);
  if (named.path.empty() || !named.type_args.empty()) {
    return std::nullopt;
  }
  return named.path.back();
}

/// Lowers every eligible associated function or method (no generics on the
/// impl block or the function itself) declared in `impl` into a
/// `hir_function` named `TargetType::method`, appending each to
/// `functions`. A `self`-taking method lowers exactly like any other
/// function — `self` is just an ordinary first parameter by the time
/// `lower_function` sees it (its type comes from `checked_type_of`, not
/// `param.type_annotation`, which is always null for `self`; see
/// `lower_function`'s guard). A generic impl/method is still left alone —
/// monomorphization is out of scope (`spec/typed-ir-design.md`'s generics
/// non-goal).
[[nodiscard]] auto lower_impl_associated_functions(
    const ast::impl_decl &impl, const semantic::checked_types &checked,
    ptr_vec<hir_function> &functions) -> std::expected<void, lowering_error> {
  if (!impl.type_params.empty()) {
    return {};
  }
  const auto target_name = simple_impl_target_name(impl.for_type.get());
  if (!target_name.has_value()) {
    return {};
  }
  for (const auto &item : impl.items) {
    if (item == nullptr || item->kind != ast::node_kind::func_decl) {
      continue;
    }
    const auto &decl = dynamic_cast<const ast::func_decl &>(*item);
    if (decl.modifiers.is_intrinsic || !decl.type_params.empty()) {
      continue;
    }
    auto lowered = lower_function(decl, checked);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    (*lowered)->name = std::format("{}::{}", *target_name, decl.name);
    functions.push_back(std::move(*lowered));
  }
  return {};
}

/// Lowers every method declared in an `extend TargetType:` block into a
/// `hir_function` named `TargetType::method`, the same naming convention
/// `lower_impl_associated_functions` uses — an `extend` block makes no
/// trait-conformance claim, so there's no coherence bookkeeping to skip,
/// just a plain per-item lowering.
[[nodiscard]] auto lower_extend_methods(const ast::extend_decl &extend,
                                        const semantic::checked_types &checked,
                                        ptr_vec<hir_function> &functions)
    -> std::expected<void, lowering_error> {
  const auto target_name = simple_impl_target_name(extend.for_type.get());
  if (!target_name.has_value()) {
    return {};
  }
  for (const auto &item : extend.items) {
    if (item == nullptr || item->kind != ast::node_kind::func_decl) {
      continue;
    }
    const auto &decl = dynamic_cast<const ast::func_decl &>(*item);
    if (decl.modifiers.is_intrinsic || !decl.type_params.empty()) {
      continue;
    }
    auto lowered = lower_function(decl, checked);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    (*lowered)->name = std::format("{}::{}", *target_name, decl.name);
    functions.push_back(std::move(*lowered));
  }
  return {};
}

auto lower_module(const ast::file &file, std::string module_name,
                  const semantic::checked_types &checked)
    -> std::expected<ptr<hir_module>, lowering_error> {
  auto functions = ptr_vec<hir_function>{};
  for (const auto &item : file.items) {
    if (item == nullptr) {
      continue;
    }
    if (item->kind == ast::node_kind::impl_decl) {
      auto result = lower_impl_associated_functions(
          dynamic_cast<const ast::impl_decl &>(*item), checked, functions);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      continue;
    }
    if (item->kind == ast::node_kind::extend_decl) {
      auto result = lower_extend_methods(
          dynamic_cast<const ast::extend_decl &>(*item), checked, functions);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      continue;
    }
    if (item->kind != ast::node_kind::func_decl) {
      continue;
    }
    const auto &decl = dynamic_cast<const ast::func_decl &>(*item);
    if (decl.modifiers.is_intrinsic) {
      // No body to lower — the bytecode compiler recognizes calls to a
      // known intrinsic name by itself (src/intrinsics.h) and emits
      // `op_call_intrinsic` directly, so this function never needs an entry
      // in the module's `hir_function` table.
      continue;
    }
    auto lowered = lower_function(decl, checked);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    functions.push_back(std::move(*lowered));
  }
  // Trait-default methods `semantic::checker::monomorphize_trait_default`
  // (`check.cpp`) cloned and concretely type-checked for one impl each —
  // they have no `impl_decl`/`func_decl` item of their own in `file` to
  // walk above, so they're lowered here instead, once per module, the same
  // `TargetType::method` naming convention every other impl member uses.
  for (const auto &synthesized : checked.synthesized_trait_defaults) {
    if (synthesized.owner_module != module_name) {
      continue;
    }
    auto lowered = lower_function(*synthesized.decl, checked);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    (*lowered)->name = std::format("{}::{}", synthesized.target_type_name,
                                   synthesized.decl->name);
    functions.push_back(std::move(*lowered));
  }
  return make<hir_module>(file.span, std::move(module_name),
                          std::move(functions));
}

} // namespace kira::hir
