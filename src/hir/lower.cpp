#include "src/hir/lower.h"

#include <algorithm>
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

/// Whether `decl` is one of the monomorphized copies the checker made of a
/// const-generic template (`semantic::const_generic_instance`) — the one kind
/// of declaration that still carries generic parameters and is nonetheless
/// fully concrete, because every one of them was substituted before its body
/// was checked.
[[nodiscard]] auto is_const_generic_instance(const ast::func_decl &decl,
                                             const checked_types &checked)
    -> bool {
  return std::ranges::any_of(
      checked.const_generic_instances,
      [&decl](const auto &instance) -> bool { return instance.decl == &decl; });
}

/// Whether `decl` is a template generic over compile-time *values* only. It
/// has no runtime form of its own — `n` is not a parameter anything passes —
/// so `lower_module` skips it and lowers the instances the checker made of it
/// instead. This deliberately does not skip a *type*-generic function: that
/// one is still an unhandled construct, and lowering fails closed on it
/// rather than quietly dropping a function a call site expects to exist.
[[nodiscard]] auto is_const_generic_template(const ast::func_decl &decl)
    -> bool {
  return !decl.type_params.empty() &&
         std::ranges::all_of(decl.type_params, [](const auto &param) -> bool {
           return param.is_value_param && !param.name.empty();
         });
}

/// Whether `decl` is a template generic over *types* only (`def identity[T]`).
/// Like a const-generic template it has no runtime form of its own — there is
/// no single body that serves every type — so `lower_module` skips it and
/// lowers the instances the checker monomorphized instead
/// (`semantic::checker::instantiate_type_generic`, registered as
/// `const_generic_instance`s). A template mixing value and type parameters is
/// deliberately not matched here: neither monomorphization path handles it, so
/// it must reach `lower_function` and fail closed rather than be dropped.
[[nodiscard]] auto is_type_generic_template(const ast::func_decl &decl)
    -> bool {
  return !decl.type_params.empty() &&
         std::ranges::all_of(decl.type_params, [](const auto &param) -> bool {
           return !param.is_value_param && !param.name.empty();
         });
}

/// Wraps a freshly-built derived node into the `ptr<hir_expr>` result type
/// every expression-lowering helper returns.
template <typename T>
[[nodiscard]] auto ok_expr(ptr<T> node)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  return ptr<hir_expr>(std::move(node));
}

/// Re-encodes an already escape-decoded literal-text segment
/// (`ast::interp_segment::literal_text`) back into the quoted, escaped
/// source spelling `decode_string_literal` (both backends' codegen) expects
/// a `string_lit`'s `hir_literal::value` to be — used by
/// `lower_interpolated_string` so a literal-text segment can still ride the
/// ordinary `hir_literal`/`compile_string_literal` path with no new HIR node
/// kind or backend change.
[[nodiscard]] auto quote_and_escape_for_literal(std::string_view text)
    -> std::string {
  auto out = std::string("\"");
  for (const char c : text) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\0':
      out += "\\0";
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  out += "\"";
  return out;
}

/// Builds a `char_lit`-shaped source spelling for a single ASCII byte (a
/// format spec's `fill` character or a synthesized `'0'` zero-pad digit) —
/// same escape set as `quote_and_escape_for_literal`, single-quoted, for
/// `decode_char_literal` to invert.
[[nodiscard]] auto quote_char_for_literal(char c) -> std::string {
  switch (c) {
  case '\'':
    return "'\\''";
  case '\\':
    return "'\\\\'";
  case '\n':
    return "'\\n'";
  case '\t':
    return "'\\t'";
  case '\r':
    return "'\\r'";
  case '\0':
    return "'\\0'";
  default:
    return std::string("'") + c + "'";
  }
}

/// Whether control can never reach the statement *after* `node` — every path
/// through it returns. Postcondition lowering asks this about a function
/// body's tail: a tail that always returns has already checked the
/// postcondition at each of those returns, and appending anything after it
/// would be not just dead code but invalid (both backends terminate the
/// current basic block on the last `return`, and LLVM's verifier rejects an
/// instruction that follows a terminator).
[[nodiscard]] auto always_returns(const hir_node &node) -> bool {
  switch (node.kind) {
  case hir_node_kind::hir_return:
    return true;
  case hir_node_kind::hir_expr_stmt:
    return always_returns(*dynamic_cast<const hir_expr_stmt &>(node).expr);
  case hir_node_kind::hir_block: {
    const auto &block = dynamic_cast<const hir_block &>(node);
    return std::ranges::any_of(block.stmts, [](const auto &stmt) -> bool {
      return always_returns(*stmt);
    });
  }
  case hir_node_kind::hir_if: {
    const auto &node2 = dynamic_cast<const hir_if &>(node);
    // Without an `else`, the condition can simply be false, and control
    // arrives at whatever follows.
    return node2.else_body != nullptr && always_returns(*node2.else_body) &&
           std::ranges::all_of(node2.branches, [](const auto &branch) -> bool {
             return always_returns(*branch.body);
           });
  }
  case hir_node_kind::hir_match: {
    const auto &node2 = dynamic_cast<const hir_match &>(node);
    return std::ranges::all_of(node2.arms, [](const auto &arm) -> bool {
      return always_returns(*arm.body);
    });
  }
  default:
    return false;
  }
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
  lowerer(const checked_types &checked, const lowering_options &options)
      : checked_(checked), options_(options) {}

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
    return resolve_opaque(found->second);
  }

  /// Unwraps an `existential_kind` type (`some Trait[Args]`) to its concrete
  /// backing type. Opacity is purely a checker-level view — `type_entry`'s
  /// doc comment (`semantic/types.h`) — enforced by restricting which
  /// operations `infer_method_call` allowed on a value of this kind; by the
  /// time lowering runs, every function's opaque return type has already
  /// been backfilled with a real concrete type, and HIR/bytecode/LLVM never
  /// need to know an opaque view existed. A non-existential id passes
  /// through unchanged.
  [[nodiscard]] auto resolve_opaque(type_id id) const -> type_id {
    const auto &entry = checked_.types.entry(id);
    return entry.kind == type_kind::existential_kind ? entry.result : id;
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
  [[nodiscard]] auto
  lower_interpolated_string(const ast::interpolated_string_expr &node)
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
  /// An `if`/`elif`/`else` chain. Ordinary condition branches lower to the
  /// `hir_if` you'd expect; an `if let`/`elif let` branch has no boolean
  /// condition to test, so it lowers instead to a two-arm `hir_match` over
  /// its scrutinee — the pattern's arm carries the branch body, and a
  /// wildcard arm carries *everything after this branch* (the remaining
  /// `elif`s and the `else`, lowered by recursing into `lower_if_chain`).
  /// A chain that mixes both forms therefore lowers to alternating
  /// `hir_if`/`hir_match` nodes, which is why this returns a plain
  /// `hir_expr`: the top of a chain is a `hir_match` whenever its first
  /// branch is an `if let`.
  [[nodiscard]] auto lower_if(const std::vector<ast::if_branch> &branches,
                              const std::vector<ast::ptr<ast::node>> &else_body,
                              source_span span, type_id type)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  /// Lowers `branches[index:]` plus `else_body` — the tail of a chain whose
  /// earlier branches have already been lowered.
  [[nodiscard]] auto
  lower_if_chain(const std::vector<ast::if_branch> &branches,
                 const std::vector<ast::ptr<ast::node>> &else_body,
                 size_t index, source_span span, type_id type)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  /// The `if let`/`elif let` case of `lower_if_chain` (see `lower_if`).
  [[nodiscard]] auto
  lower_if_let_chain(const std::vector<ast::if_branch> &branches,
                     const std::vector<ast::ptr<ast::node>> &else_body,
                     size_t index, source_span span, type_id type)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  /// The block an unmatched/false branch at `index` falls through to: the
  /// rest of the chain (`branches[index:]` recursively lowered, wrapped as
  /// the block's tail value), or the `else` body, or nothing at all.
  /// Returns null only when there is neither — a caller that needs a real
  /// block (a `match` arm's body, which cannot be null) substitutes an
  /// empty one.
  [[nodiscard]] auto
  lower_if_fallback(const std::vector<ast::if_branch> &branches,
                    const std::vector<ast::ptr<ast::node>> &else_body,
                    size_t index, source_span span, type_id type)
      -> std::expected<ptr<hir_block>, lowering_error>;
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
  /// The `generator[T]` shape: `for x in g(): ...` evaluates the generator
  /// handle exactly once (a `hir_let`, so a call like `g()` isn't
  /// re-invoked every iteration), then desugars to the same shape as a
  /// hand-written `while let @some(x) = <that handle>.next(): ...` —
  /// `hir_while_let`'s `subject` (a fresh `hir_generator_next` each
  /// iteration) is what actually drives repeated evaluation; no new node
  /// kind or backend support is needed; both `compile_while_let`s already
  /// re-run `subject` every pass.
  [[nodiscard]] auto lower_generator_loop(
      source_span span, const ast::expr &iterable, type_id iterable_type,
      const ast::binding_pattern &loop_var,
      const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
          &inner_stmts) -> std::expected<ptr_vec<hir_node>, lowering_error>;
  /// The user-`iterator[T]` shape: `for x in it: ...` where `it`'s type
  /// implements `std.iter.iterator[T]`. Structurally identical to
  /// `lower_generator_loop` — evaluate the handle once, then loop `while let
  /// @some(x) = <handle>.next(): ...` — except the `while_let` subject is a
  /// real `hir_call` to the resolved `Type::next` method (rebuilt from the
  /// `iterator_loop_dispatch` the checker recorded) rather than a
  /// `hir_generator_next` node. No new node kind or backend support needed.
  [[nodiscard]] auto lower_iterator_loop(
      source_span span, const ast::expr &iterable, type_id iterable_type,
      const ast::binding_pattern &loop_var,
      const semantic::iterator_loop_dispatch &dispatch,
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
  //  Contracts (spec/kira-reference.md, "Contracts")
  //
  //  The checker has already had its say by the time these run: a contract
  //  it refuted is a compile error the program never got past, and one it
  //  proved is in `checked_.elided_contracts` and lowers to nothing at all.
  //  What is left is the residue — conditions whose truth depends on values
  //  that don't exist until the program runs — and this is where they become
  //  code: a `hir_contract_check` the backends turn into "test it, panic if
  //  it's false".
  // ------------------------------------------------------------------

  /// Whether `contract` still needs a runtime check: it has a usable
  /// condition, the checker didn't prove it, and the build didn't ask for
  /// contracts to be dropped wholesale (`lowering_options::contract_checks`).
  [[nodiscard]] auto
  needs_runtime_check(const ast::contract_clause &contract) const -> bool {
    return options_.contract_checks && contract.condition != nullptr &&
           !contract.condition->has_error &&
           !checked_.elided_contracts.contains(&contract);
  }

  /// Lowers one contract condition into the check statement that enforces it.
  /// The condition is lowered in whatever scope the caller has set up, which
  /// is the whole trick: a `pre` sees the parameters, a `post` additionally
  /// sees `return` (bound by `lower_return_value`), and an `invariant` sees
  /// `self` (bound by `check_invariant_of`).
  [[nodiscard]] auto lower_contract_check(const ast::contract_clause &contract,
                                          contract_kind kind)
      -> std::expected<ptr<hir_node>, lowering_error> {
    auto condition = lower_expr(*contract.condition);
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    return ptr<hir_node>(
        make<hir_contract_check>(contract.span, std::move(*condition), kind,
                                 contract.message.value_or(std::string{})));
  }

  /// Builds a function exit: binds the value being returned to `return` — the
  /// name a postcondition uses for it — checks every postcondition against
  /// that binding, then returns it. With no postconditions to check this is
  /// just `return value`, which is why every exit can route through here.
  ///
  /// Each exit gets its own `return` binding rather than one shared function-
  /// wide slot: two `hir_let`s carrying the same `symbol_id` would collide in
  /// the backends' `locals_` maps (whose `emplace` keeps the *first* register
  /// bound to a symbol), so the second exit would read the first one's value.
  [[nodiscard]] auto lower_return_value(source_span span, ptr<hir_expr> value)
      -> std::expected<ptr_vec<hir_node>, lowering_error> {
    auto stmts = ptr_vec<hir_node>{};
    if (post_contracts_.empty()) {
      stmts.push_back(ptr<hir_node>(make<hir_return>(span, std::move(value))));
      return stmts;
    }

    const auto type = value->type;
    push_scope();
    const auto symbol = declare_local("return", type);
    stmts.push_back(ptr<hir_node>(
        make<hir_let>(span, symbol, std::string("return"), std::move(value))));
    for (const auto *contract : post_contracts_) {
      auto check =
          lower_contract_check(*contract, contract_kind::postcondition);
      if (!check.has_value()) {
        pop_scope();
        return std::unexpected(check.error());
      }
      stmts.push_back(std::move(*check));
    }
    pop_scope();
    stmts.push_back(ptr<hir_node>(make<hir_return>(
        span, ptr<hir_expr>(make<hir_local_ref>(span, type, symbol,
                                                std::string("return"))))));
    return stmts;
  }

  /// Rebuilds a *place* expression — a chain of pure projections rooted at a
  /// local (`p`, `p.inner`, `p.0`) — so it can be evaluated a second time.
  /// Only these shapes: re-reading them is free of side effects and yields
  /// the same value, which is exactly the property `lower_pattern`'s
  /// `make_place` factories rely on. Anything else (a call, an index with a
  /// computed subscript) is refused rather than silently duplicated.
  [[nodiscard]] auto clone_place(const hir_expr &place)
      -> std::expected<ptr<hir_expr>, lowering_error> {
    switch (place.kind) {
    case hir_node_kind::hir_local_ref: {
      const auto &ref = dynamic_cast<const hir_local_ref &>(place);
      return ptr<hir_expr>(make<hir_local_ref>(ref.span, ref.type, ref.symbol,
                                               ref.name, ref.owner_module));
    }
    case hir_node_kind::hir_field: {
      const auto &field = dynamic_cast<const hir_field &>(place);
      auto object = clone_place(*field.object);
      if (!object.has_value()) {
        return std::unexpected(object.error());
      }
      return ptr<hir_expr>(make<hir_field>(
          field.span, field.type, std::move(*object), field.field_name));
    }
    case hir_node_kind::hir_tuple_index: {
      const auto &index = dynamic_cast<const hir_tuple_index &>(place);
      auto object = clone_place(*index.object);
      if (!object.has_value()) {
        return std::unexpected(object.error());
      }
      return ptr<hir_expr>(make<hir_tuple_index>(
          index.span, index.type, std::move(*object), index.index));
    }
    default:
      return fail(lowering_error_kind::unsupported_construct, place.span,
                  "this value's type declares an `invariant`, but the "
                  "expression being assigned through is not a simple place "
                  "(a local, or a field/tuple projection of one), so the "
                  "invariant cannot be re-checked after the write yet");
    }
  }

  /// The `invariant` clause declared on `type`'s `type` declaration, if it
  /// has one and it is still worth checking at run time.
  [[nodiscard]] auto invariant_of(type_id type) const -> const ast::expr * {
    if (!options_.contract_checks) {
      return nullptr;
    }
    const auto &entry = checked_.types.entry(type);
    if (entry.decl == nullptr || entry.decl->invariant == nullptr ||
        entry.decl->invariant->has_error) {
      return nullptr;
    }
    return entry.decl->invariant.get();
  }

  /// Checks `type`'s invariant against the value bound to `self_symbol`. The
  /// invariant is written in terms of `self` (`invariant self.value > 0`), so
  /// lowering it is a matter of putting `self` in scope pointing at the value
  /// whose construction or mutation is being guarded.
  [[nodiscard]] auto check_invariant_of(const ast::expr &invariant,
                                        type_id type, symbol_id self_symbol,
                                        source_span span)
      -> std::expected<ptr<hir_node>, lowering_error> {
    push_scope();
    scopes_.back().emplace("self", self_symbol);
    local_types_.emplace(self_symbol, type);
    auto condition = lower_expr(invariant);
    pop_scope();
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    return ptr<hir_node>(make<hir_contract_check>(
        span, std::move(*condition), contract_kind::invariant, std::string{}));
  }

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
  lowering_options options_;
  std::vector<std::unordered_map<std::string, symbol_id>> scopes_;
  std::unordered_map<std::string, symbol_id> global_refs_;
  std::unordered_map<symbol_id, type_id> local_types_;
  symbol_id next_symbol_ = 0;
  /// The enclosing function's postconditions that still need checking — read
  /// by `lower_return_value` at every exit. Empty while lowering a lambda
  /// body, where a `return` returns from the lambda and settles nothing about
  /// the enclosing function's promise (see `lower_lambda`).
  std::vector<const ast::contract_clause *> post_contracts_;
};

auto lowerer::lower_expr(const ast::expr &expr)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  if (expr.has_error) {
    return fail(
        lowering_error_kind::unsupported_construct, expr.span,
        "expression carries a parse/recovery error and cannot be lowered");
  }

  // An expression the *checker* rewrote — a `splice` resolved to its quoted
  // fragment, or a refinement's `try_from` desugared into a plain
  // predicate-and-option check (`spec/dependent-types-design.md` §7) — lowers
  // as the syntax it was rewritten to, which the checker already typed node by
  // node. Redirecting here rather than in each case keeps the rewrite a
  // property of the *node*, so a new desugaring needs no change to lowering at
  // all: record the fragment, and it lowers.
  if (const auto rewritten = checked_.spliced_fragments.find(&expr);
      rewritten != checked_.spliced_fragments.end()) {
    if (const auto *fragment =
            dynamic_cast<const ast::expr *>(rewritten->second)) {
      return lower_expr(*fragment);
    }
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
  case ast::node_kind::interpolated_string_expr:
    return lower_interpolated_string(
        dynamic_cast<const ast::interpolated_string_expr &>(expr));
  case ast::node_kind::splice_expr: {
    // `checker::infer_expr`'s `splice_expr` case resolves the operand's
    // compile-time value to a real AST fragment and records it in
    // `spliced_fragments` — lowering just needs to redirect to that
    // fragment as if it were physically present here. An absent entry
    // means the splice never resolved to usable syntax, which checking
    // would already have diagnosed.
    const auto found = checked_.spliced_fragments.find(&expr);
    if (found == checked_.spliced_fragments.end()) {
      return fail(lowering_error_kind::unsupported_construct, expr.span,
                  "this splice did not resolve to a quoted expression during "
                  "checking");
    }
    const auto *fragment_expr = dynamic_cast<const ast::expr *>(found->second);
    if (fragment_expr == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, expr.span,
                  "this splice's resolved fragment is not an expression");
    }
    return lower_expr(*fragment_expr);
  }
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
  // A reference to a scalar (integer/float/bool) top-level `static let` —
  // `semantic::checker::resolve_ident` already evaluated it and recorded
  // the literal to embed here. There's no other route to a runtime
  // representation for a plain `static let`: it's neither a real local
  // (no `declare_local` call ever registers one for it) nor a function,
  // so falling through to the ordinary `hir_local_ref` path below would
  // produce a symbol nothing can ever resolve.
  if (const auto it = checked_.static_const_values.find(&ident);
      it != checked_.static_const_values.end()) {
    return lower_literal(*it->second);
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
    if (field.object != nullptr && field.field_name == "next") {
      auto object_type = checked_type_of(*field.object);
      if (object_type.has_value()) {
        const auto &object_entry = checked_.types.entry(*object_type);
        if (object_entry.kind == type_kind::builtin_generic_kind &&
            object_entry.name == "generator") {
          auto object = lower_expr(*field.object);
          if (!object.has_value()) {
            return std::unexpected(object.error());
          }
          return ok_expr(
              make<hir_generator_next>(call.span, *type, std::move(*object)));
        }
      }
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
  auto init = ptr<hir_expr>(
      make<hir_struct_init>(literal.span, *type, std::move(fields)));

  // "Struct invariants are checked at construction and mutation boundaries"
  // (spec/kira-reference.md). This is the construction boundary: the value
  // exists, nothing has looked at it yet, and it must already be true of
  // itself. The check needs a name to talk about it by, so the literal
  // becomes a small block — bind, check, hand the value back — which both
  // backends already compile as an expression (`compile_block_as_value`).
  const auto *invariant = invariant_of(*type);
  if (invariant == nullptr) {
    return ok_expr(std::move(init));
  }
  const auto self_symbol = mint_symbol();
  auto stmts = ptr_vec<hir_node>{};
  stmts.push_back(ptr<hir_node>(make<hir_let>(
      literal.span, self_symbol, std::string("self"), std::move(init))));
  auto check = check_invariant_of(*invariant, *type, self_symbol, literal.span);
  if (!check.has_value()) {
    return std::unexpected(check.error());
  }
  stmts.push_back(std::move(*check));
  stmts.push_back(ptr<hir_node>(make<hir_expr_stmt>(
      literal.span,
      ptr<hir_expr>(make<hir_local_ref>(literal.span, *type, self_symbol,
                                        std::string("self"))))));
  return ok_expr(make<hir_block>(literal.span, *type, std::move(stmts)));
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

  // A `return` inside a lambda body returns from the *lambda* — it says
  // nothing about whether the enclosing function kept its promise, and the
  // lambda has no return value of its own to hand a `post` condition. So the
  // enclosing function's postconditions are out of scope for the duration.
  auto enclosing_posts = std::exchange(post_contracts_, {});
  const auto restore_posts = [&] -> void {
    post_contracts_ = std::move(enclosing_posts);
  };

  auto body = std::expected<ptr<hir_block>, lowering_error>{};
  if (lambda.body_expr != nullptr) {
    auto expr_type = checked_type_of(*lambda.body_expr);
    if (!expr_type.has_value()) {
      pop_scope();
      restore_posts();
      return std::unexpected(expr_type.error());
    }
    auto value = lower_expr(*lambda.body_expr);
    if (!value.has_value()) {
      pop_scope();
      restore_posts();
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
  restore_posts();
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

/// Desugars a `"{expr :spec}"` interpolated string literal
/// (`spec/string-formatting-design.md`) into a sequence of calls to
/// `std.fmt`'s fixed-signature rendering helpers (`fmt_show_i64`,
/// `fmt_radix_u64`, `pad_str`, ...) plus `std.fmt.rt_str_concat` to fold
/// every segment together — see the design doc's "Desugaring via Quoting
/// and Splicing" section, emitted here directly rather than through an
/// interpreted macro (`spec/string-formatting-design.md`, "Implementation
/// Staging"). Because this bottoms out entirely in ordinary function calls
/// and struct/variant construction — both already lowered/codegen'd by both
/// backends — no bytecode-compiler or LLVM-codegen change is needed beyond
/// the new intrinsics' native bodies (`src/bytecode/vm.cpp`,
/// `src/runtime/fmt.cpp`).
auto lowerer::lower_interpolated_string(
    const ast::interpolated_string_expr &node)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto result_type = checked_type_of(node);
  if (!result_type.has_value()) {
    return std::unexpected(result_type.error());
  }

  const auto &fmt = checked_.fmt_types;
  if (fmt.format_spec == 0) {
    return fail(lowering_error_kind::unsupported_construct, node.span,
                "string interpolation requires `std.fmt` to be part of the "
                "compiled session");
  }
  const auto span = node.span;
  const auto str_type = fmt.str_type;
  const auto bool_type = checked_.types.bool_type();
  const auto usize_type = checked_.types.usize_type();

  const auto str_lit = [&](std::string_view text) -> ptr<hir_expr> {
    return ptr<hir_expr>(make<hir_literal>(span, str_type,
                                           token_kind::string_lit,
                                           quote_and_escape_for_literal(text)));
  };
  const auto bool_lit = [&](bool v) -> ptr<hir_expr> {
    return ptr<hir_expr>(make<hir_literal>(
        span, bool_type, v ? token_kind::kw_true : token_kind::kw_false,
        std::string(v ? "true" : "false")));
  };
  const auto usize_lit = [&](uint64_t v) -> ptr<hir_expr> {
    return ptr<hir_expr>(make<hir_literal>(
        span, usize_type, token_kind::int_lit, std::to_string(v)));
  };
  const auto uint8_lit = [&](uint64_t v) -> ptr<hir_expr> {
    return ptr<hir_expr>(make<hir_literal>(
        span, fmt.uint8_type, token_kind::int_lit, std::to_string(v)));
  };
  const auto char_lit = [&](char c) -> ptr<hir_expr> {
    return ptr<hir_expr>(make<hir_literal>(span, checked_.types.char_type(),
                                           token_kind::char_lit,
                                           quote_char_for_literal(c)));
  };
  const auto unit_variant = [&](type_id type,
                                std::string_view name) -> ptr<hir_expr> {
    return ptr<hir_expr>(make<hir_variant_init>(span, type, std::string(name),
                                                ptr_vec<hir_expr>{}));
  };
  const auto some_of = [&](type_id option_type,
                           ptr<hir_expr> payload) -> ptr<hir_expr> {
    auto args = ptr_vec<hir_expr>{};
    args.push_back(std::move(payload));
    return ptr<hir_expr>(
        make<hir_variant_init>(span, option_type, "some", std::move(args)));
  };
  const auto none_of = [&](type_id option_type) -> ptr<hir_expr> {
    return ptr<hir_expr>(
        make<hir_variant_init>(span, option_type, "none", ptr_vec<hir_expr>{}));
  };
  const auto call_fmt = [&](std::string_view name, type_id ret_type,
                            ptr_vec<hir_expr> args) -> ptr<hir_expr> {
    auto callee = ptr<hir_expr>(make<hir_local_ref>(
        span, k_unknown_type, resolve_reference(std::string(name)),
        std::string(name), std::string("std.fmt")));
    return ptr<hir_expr>(
        make<hir_call>(span, ret_type, std::move(callee), std::move(args)));
  };
  const auto cast_to = [&](ptr<hir_expr> value,
                           type_id target) -> ptr<hir_expr> {
    return ptr<hir_expr>(make<hir_cast>(span, target, std::move(value)));
  };

  // A dynamic `{expr}` width/precision was already type-checked as `usize`
  // by `check_interpolated_string`; a literal one is just a compile-time
  // constant. Either way this yields a plain `usize`-typed value expression
  // (not wrapped in `option`) — callers wrap it in `@some(...)` themselves
  // where the field needs that (`format_spec.width`/`.precision`), or use it
  // bare where a helper takes the precision directly (`fmt_float_fixed`).
  const auto lower_usize_slot =
      [&](const std::variant<std::monostate, size_t, ast::ptr<ast::expr>> *slot,
          uint64_t default_value)
      -> std::expected<ptr<hir_expr>, lowering_error> {
    if (slot == nullptr) {
      return usize_lit(default_value);
    }
    if (const auto *literal = std::get_if<size_t>(slot)) {
      return usize_lit(*literal);
    }
    if (const auto *dynamic = std::get_if<ast::ptr<ast::expr>>(slot);
        dynamic != nullptr && *dynamic != nullptr) {
      return lower_expr(**dynamic);
    }
    return usize_lit(default_value);
  };

  const auto build_spec = [&](const ast::interp_segment &seg)
      -> std::expected<ptr<hir_expr>, lowering_error> {
    const auto has_spec = seg.has_spec;
    const auto fill = has_spec ? seg.spec.fill : ' ';

    auto align_value = ptr<hir_expr>{};
    if (has_spec && seg.spec.align.has_value()) {
      const auto name = *seg.spec.align == ast::format_align::left ? "left"
                        : *seg.spec.align == ast::format_align::right
                            ? "right"
                            : "center";
      align_value =
          some_of(fmt.option_align_mode, unit_variant(fmt.align_mode, name));
    } else {
      align_value = none_of(fmt.option_align_mode);
    }

    const auto sign_name =
        !has_spec                                   ? "negative_only"
        : seg.spec.sign == ast::format_sign::always ? "always"
        : seg.spec.sign == ast::format_sign::space  ? "space"
                                                    : "negative_only";

    auto width_payload =
        lower_usize_slot(has_spec ? &seg.spec.width : nullptr, 0);
    if (!width_payload.has_value()) {
      return std::unexpected(width_payload.error());
    }
    const bool has_width =
        has_spec && !std::holds_alternative<std::monostate>(seg.spec.width);

    auto precision_payload =
        lower_usize_slot(has_spec ? &seg.spec.precision : nullptr, 0);
    if (!precision_payload.has_value()) {
      return std::unexpected(precision_payload.error());
    }
    const bool has_precision =
        has_spec && !std::holds_alternative<std::monostate>(seg.spec.precision);

    auto fields = std::vector<hir_struct_init_field>{};
    fields.push_back({.name = "fill", .value = char_lit(fill)});
    fields.push_back({.name = "align", .value = std::move(align_value)});
    fields.push_back(
        {.name = "sign", .value = unit_variant(fmt.sign_mode, sign_name)});
    fields.push_back({.name = "alternate",
                      .value = bool_lit(has_spec && seg.spec.alternate)});
    fields.push_back(
        {.name = "zero_pad", .value = bool_lit(has_spec && seg.spec.zero_pad)});
    fields.push_back({.name = "width",
                      .value = has_width ? some_of(fmt.option_usize,
                                                   std::move(*width_payload))
                                         : none_of(fmt.option_usize)});
    fields.push_back(
        {.name = "precision",
         .value = has_precision
                      ? some_of(fmt.option_usize, std::move(*precision_payload))
                      : none_of(fmt.option_usize)});
    return ptr<hir_expr>(
        make<hir_struct_init>(span, fmt.format_spec, std::move(fields)));
  };

  auto pieces = ptr_vec<hir_expr>{};
  for (const auto &seg : node.segments) {
    if (seg.is_literal) {
      pieces.push_back(str_lit(seg.literal_text));
      continue;
    }
    if (seg.value == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, span,
                  "interpolation segment is missing its expression");
    }
    if (seg.self_doc) {
      pieces.push_back(str_lit(seg.source_text + "="));
    }

    const auto dispatch_it = checked_.interp_dispatches.find(seg.value.get());
    if (dispatch_it == checked_.interp_dispatches.end()) {
      return fail(lowering_error_kind::unresolved_type, seg.value->span,
                  "this interpolated expression was never resolved by the "
                  "capability check");
    }
    const auto &dispatch = dispatch_it->second;

    auto value = lower_expr(*seg.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    auto spec_expr = build_spec(seg);
    if (!spec_expr.has_value()) {
      return std::unexpected(spec_expr.error());
    }

    const auto &value_entry = checked_.types.entry(dispatch.value_type);
    const bool is_signed =
        value_entry.name.starts_with("int") || value_entry.name == "isize";

    // `dispatch.type_char` selected a float style ('e'/'E'/'f'/'g'/'G') only
    // when `dispatch.kind == builtin_float`; every other kind ignores
    // `precision_value` entirely, so it's fine to always compute it from
    // the same spec slot up front rather than threading it out of
    // `build_spec` (which independently derives the same slot for
    // `format_spec.precision`).
    const auto precision_default = uint64_t{6};
    auto lowered_precision = lower_usize_slot(
        seg.has_spec ? &seg.spec.precision : nullptr, precision_default);
    if (!lowered_precision.has_value()) {
      return std::unexpected(lowered_precision.error());
    }
    auto precision_value = std::move(*lowered_precision);

    auto rendered = ptr<hir_expr>{};
    switch (dispatch.kind) {
    case semantic::interp_dispatch::kind_t::trait_method: {
      if (dispatch.decl == nullptr) {
        return fail(
            lowering_error_kind::unsupported_construct, seg.value->span,
            "this type's capability comes only from `deriving`, which has "
            "no runtime body yet — implement it with an explicit `impl` "
            "block instead");
      }
      const auto local_name =
          dispatch.impl_target_type.empty()
              ? dispatch.decl->name
              : std::format("{}::{}", dispatch.impl_target_type,
                            dispatch.decl->name);
      auto callee = ptr<hir_expr>(make<hir_local_ref>(
          seg.value->span, k_unknown_type, resolve_reference(local_name),
          local_name, dispatch.owner_module));
      auto call_args = ptr_vec<hir_expr>{};
      call_args.push_back(std::move(*value));
      auto method_result = ptr<hir_expr>(make<hir_call>(
          seg.value->span, str_type, std::move(callee), std::move(call_args)));

      const auto is_radix_trait =
          dispatch.type_char == 'x' || dispatch.type_char == 'X' ||
          dispatch.type_char == 'o' || dispatch.type_char == 'b';
      if (is_radix_trait) {
        const auto prefix = dispatch.type_char == 'o'   ? "0o"
                            : dispatch.type_char == 'b' ? "0b"
                                                        : "0x";
        auto args = ptr_vec<hir_expr>{};
        args.push_back(bool_lit(false));
        args.push_back(str_lit(prefix));
        args.push_back(std::move(method_result));
        args.push_back(std::move(*spec_expr));
        rendered = call_fmt("pad_integral", str_type, std::move(args));
      } else {
        auto args = ptr_vec<hir_expr>{};
        args.push_back(std::move(method_result));
        args.push_back(std::move(*spec_expr));
        rendered = call_fmt("pad_str", str_type, std::move(args));
      }
      break;
    }
    case semantic::interp_dispatch::kind_t::builtin_show:
    case semantic::interp_dispatch::kind_t::builtin_debug: {
      const auto debug =
          dispatch.kind == semantic::interp_dispatch::kind_t::builtin_debug;
      if (value_entry.kind == type_kind::builtin_kind &&
          value_entry.name == "str") {
        auto args = ptr_vec<hir_expr>{};
        args.push_back(std::move(*value));
        args.push_back(std::move(*spec_expr));
        rendered = call_fmt(debug ? "fmt_debug_str" : "fmt_show_str", str_type,
                            std::move(args));
      } else if (checked_.types.is_boolean(dispatch.value_type)) {
        auto args = ptr_vec<hir_expr>{};
        args.push_back(std::move(*value));
        args.push_back(std::move(*spec_expr));
        rendered = call_fmt("fmt_show_bool", str_type, std::move(args));
      } else if (value_entry.kind == type_kind::builtin_kind &&
                 value_entry.name == "char") {
        auto args = ptr_vec<hir_expr>{};
        args.push_back(std::move(*value));
        args.push_back(std::move(*spec_expr));
        rendered = call_fmt("fmt_show_char", str_type, std::move(args));
      } else if (checked_.types.is_integer(dispatch.value_type)) {
        auto args = ptr_vec<hir_expr>{};
        args.push_back(cast_to(std::move(*value),
                               is_signed ? fmt.int64_type : fmt.uint64_type));
        args.push_back(std::move(*spec_expr));
        rendered = call_fmt(is_signed ? "fmt_show_i64" : "fmt_show_u64",
                            str_type, std::move(args));
      } else if (checked_.types.is_float(dispatch.value_type)) {
        auto args = ptr_vec<hir_expr>{};
        args.push_back(cast_to(std::move(*value), fmt.float64_type));
        args.push_back(usize_lit(precision_default));
        args.push_back(std::move(*spec_expr));
        rendered = call_fmt("fmt_float_general", str_type, std::move(args));
      } else {
        return fail(lowering_error_kind::unsupported_construct, seg.value->span,
                    "no builtin rendering exists for this type");
      }
      break;
    }
    case semantic::interp_dispatch::kind_t::builtin_radix: {
      const auto radix =
          dispatch.type_char == 'o'                                ? 8
          : dispatch.type_char == 'b'                              ? 2
          : dispatch.type_char == 'x' || dispatch.type_char == 'X' ? 16
                                                                   : 10;
      const auto uppercase = dispatch.type_char == 'X';
      const auto prefix =
          dispatch.type_char == 'o'                                  ? "0o"
          : dispatch.type_char == 'b'                                ? "0b"
          : (dispatch.type_char == 'x' || dispatch.type_char == 'X') ? "0x"
                                                                     : "";
      auto args = ptr_vec<hir_expr>{};
      args.push_back(cast_to(std::move(*value),
                             is_signed ? fmt.int64_type : fmt.uint64_type));
      args.push_back(uint8_lit(static_cast<uint64_t>(radix)));
      args.push_back(bool_lit(uppercase));
      args.push_back(str_lit(prefix));
      args.push_back(std::move(*spec_expr));
      rendered = call_fmt(is_signed ? "fmt_radix_i64" : "fmt_radix_u64",
                          str_type, std::move(args));
      break;
    }
    case semantic::interp_dispatch::kind_t::builtin_float: {
      auto args = ptr_vec<hir_expr>{};
      args.push_back(cast_to(std::move(*value), fmt.float64_type));
      args.push_back(std::move(precision_value));
      const auto style = dispatch.type_char;
      if (style == 'e' || style == 'E') {
        args.push_back(bool_lit(style == 'E'));
        args.push_back(std::move(*spec_expr));
        rendered = call_fmt("fmt_float_sci", str_type, std::move(args));
      } else if (style == 'g' || style == 'G') {
        args.push_back(std::move(*spec_expr));
        rendered = call_fmt("fmt_float_general", str_type, std::move(args));
      } else {
        args.push_back(std::move(*spec_expr));
        rendered = call_fmt("fmt_float_fixed", str_type, std::move(args));
      }
      break;
    }
    case semantic::interp_dispatch::kind_t::builtin_char: {
      auto args = ptr_vec<hir_expr>{};
      args.push_back(cast_to(std::move(*value), fmt.uint32_type));
      args.push_back(std::move(*spec_expr));
      rendered = call_fmt("fmt_char_codepoint", str_type, std::move(args));
      break;
    }
    }

    if (rendered == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, seg.value->span,
                  "no rendering was produced for this interpolation segment");
    }
    pieces.push_back(std::move(rendered));
  }

  if (pieces.empty()) {
    return str_lit("");
  }
  auto folded = std::move(pieces.front());
  for (size_t i = 1; i < pieces.size(); ++i) {
    auto args = ptr_vec<hir_expr>{};
    args.push_back(std::move(folded));
    args.push_back(std::move(pieces[i]));
    folded = call_fmt("rt_str_concat", str_type, std::move(args));
  }
  return folded;
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
    // `yield expr` — a generator's suspension point. Lowered directly to
    // `hir_yield` (a statement peer of `hir_return`) rather than through
    // the ordinary `lower_expr`/`hir_expr_stmt` path, mirroring how
    // `return_stmt` below builds `hir_return` directly. `yield` in any
    // other expression position (e.g. `let x = yield v`) is rejected by
    // `lower_expr`'s dispatch instead — see its `yield_expr` case.
    if (expr_stmt.expr->kind == ast::node_kind::yield_expr) {
      const auto &yield_ast =
          dynamic_cast<const ast::yield_expr &>(*expr_stmt.expr);
      if (yield_ast.value == nullptr) {
        return fail(lowering_error_kind::unsupported_construct, yield_ast.span,
                    "`yield` with no value is not supported");
      }
      auto value = lower_expr(*yield_ast.value);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      return one_stmt(
          ptr<hir_node>(make<hir_yield>(yield_ast.span, std::move(*value))));
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
    // Not necessarily one statement: an exit from a function with
    // postconditions is a whole little sequence (bind, check, return).
    return lower_return_value(ret.span, std::move(*value));
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
    // The mutation boundary (spec/kira-reference.md: "Struct invariants are
    // checked at construction and mutation boundaries"): writing `p.value`
    // can only break `p`'s invariant, so the check goes on `p` — the object
    // the field belongs to — right after the write lands. The object is read
    // off the *lowered* target rather than the AST one, because `p.value`
    // parses as a `module_path_expr` and only becomes a recognizable
    // field access once `lower_module_path` has resolved its root (see its
    // doc comment).
    const auto *object =
        (*target)->kind == hir_node_kind::hir_field
            ? dynamic_cast<const hir_field &>(**target).object.get()
            : nullptr;
    const auto *invariant =
        object == nullptr ? nullptr : invariant_of(object->type);

    // Moving the `ptr` doesn't move the node, so `object` stays valid — it
    // points into the `hir_assign` this now owns.
    auto stmts = one_stmt(ptr<hir_node>(hir::make<hir_assign>(
        assign.span, assign.op, std::move(*target), std::move(*value))));
    if (invariant == nullptr) {
      return stmts;
    }

    // `self` names the mutated object. Binding it with a `let` re-reads the
    // object rather than reusing the assignment's own target expression
    // (which is now owned by the `hir_assign`), so the check sees the value
    // the write just produced.
    auto self_value = clone_place(*object);
    if (!self_value.has_value()) {
      return std::unexpected(self_value.error());
    }
    const auto object_type = object->type;
    const auto self_symbol = mint_symbol();
    stmts.push_back(ptr<hir_node>(make<hir_let>(assign.span, self_symbol,
                                                std::string("self"),
                                                std::move(*self_value))));
    auto check =
        check_invariant_of(*invariant, object_type, self_symbol, assign.span);
    if (!check.has_value()) {
      return std::unexpected(check.error());
    }
    stmts.push_back(std::move(*check));
    return stmts;
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
  case ast::node_kind::splice_stmt: {
    // Mirrors the `splice_expr` case in `lower_expr` above: `checker::
    // check_body_node`'s `splice_stmt` case records the resolved fragment
    // in `spliced_fragments`, keyed by this statement node. The fragment
    // may be a real statement (lowered directly through `lower_stmt`) or a
    // bare expression (wrapped in `hir_expr_stmt`, same as an ordinary
    // `expr_stmt`).
    const auto found = checked_.spliced_fragments.find(&node);
    if (found == checked_.spliced_fragments.end()) {
      return fail(lowering_error_kind::unsupported_construct, node.span,
                  "this splice did not resolve to quoted syntax during "
                  "checking");
    }
    if (const auto *fragment_expr =
            dynamic_cast<const ast::expr *>(found->second)) {
      auto lowered = lower_expr(*fragment_expr);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      return one_stmt(
          ptr<hir_node>(make<hir_expr_stmt>(node.span, std::move(*lowered))));
    }
    return lower_stmt(*found->second);
  }
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
    -> std::expected<ptr<hir_expr>, lowering_error> {
  if (branches.empty()) {
    return fail(lowering_error_kind::unsupported_construct, span,
                "if has no branches");
  }
  return lower_if_chain(branches, else_body, 0, span, type);
}

auto lowerer::lower_if_chain(const std::vector<ast::if_branch> &branches,
                             const std::vector<ast::ptr<ast::node>> &else_body,
                             size_t index, source_span span, type_id type)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  if (branches[index].let_pattern != nullptr) {
    return lower_if_let_chain(branches, else_body, index, span, type);
  }

  // A run of ordinary condition branches collapses into one `hir_if`; the
  // run ends at the first `let` branch (which becomes that `hir_if`'s else
  // block, via `lower_if_fallback`) or at the end of the chain.
  auto hir_branches = std::vector<hir_if_branch>{};
  auto next = index;
  for (; next < branches.size() && branches[next].let_pattern == nullptr;
       ++next) {
    const auto &branch = branches[next];
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

  auto else_block = lower_if_fallback(branches, else_body, next, span, type);
  if (!else_block.has_value()) {
    return std::unexpected(else_block.error());
  }
  return ptr<hir_expr>(make<hir_if>(span, type, std::move(hir_branches),
                                    std::move(*else_block)));
}

auto lowerer::lower_if_let_chain(
    const std::vector<ast::if_branch> &branches,
    const std::vector<ast::ptr<ast::node>> &else_body, size_t index,
    source_span span, type_id type)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  const auto &branch = branches[index];
  if (branch.let_expr == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, branch.span,
                "`if let` branch has no scrutinee expression");
  }
  auto subject_type = checked_type_of(*branch.let_expr);
  if (!subject_type.has_value()) {
    return std::unexpected(subject_type.error());
  }
  auto subject_value = lower_expr(*branch.let_expr);
  if (!subject_value.has_value()) {
    return std::unexpected(subject_value.error());
  }

  const auto subject_symbol = mint_symbol();
  const auto subj_type = *subject_type;
  const auto subject_span = branch.let_expr->span;
  const std::function<ptr<hir_expr>()> make_place =
      [subject_symbol, subj_type, subject_span]() -> ptr<hir_expr> {
    return {make<hir_local_ref>(subject_span, subj_type, subject_symbol,
                                std::string("<if subject>"))};
  };

  push_scope();
  auto pending = std::vector<ptr<hir_node>>{};
  auto pattern = lower_pattern(*branch.let_pattern, make_place, pending);
  if (!pattern.has_value()) {
    pop_scope();
    return std::unexpected(pattern.error());
  }
  auto matched_body = lower_block(branch.body, branch.span, type);
  if (!matched_body.has_value()) {
    pop_scope();
    return std::unexpected(matched_body.error());
  }
  pop_scope();

  // The names the pattern binds are `hir_let`s, not part of the pattern (see
  // `hir_pattern`) — they belong at the front of the arm that matched.
  if (!pending.empty()) {
    auto stmts = std::move(pending);
    for (auto &stmt_ptr : (*matched_body)->stmts) {
      stmts.push_back(std::move(stmt_ptr));
    }
    (*matched_body)->stmts = std::move(stmts);
  }

  auto unmatched_body =
      lower_if_fallback(branches, else_body, index + 1, span, type);
  if (!unmatched_body.has_value()) {
    return std::unexpected(unmatched_body.error());
  }
  if (*unmatched_body == nullptr) {
    *unmatched_body = make<hir_block>(span, type, ptr_vec<hir_node>{});
  }

  auto arms = std::vector<hir_match_arm>{};
  arms.push_back(hir_match_arm{.pattern = std::move(*pattern),
                               .guard = nullptr,
                               .body = std::move(*matched_body)});
  arms.push_back(hir_match_arm{
      .pattern = ptr<hir_pattern>(make<hir_wildcard_pattern>(branch.span)),
      .guard = nullptr,
      .body = std::move(*unmatched_body)});

  return ptr<hir_expr>(make<hir_match>(span, type, std::move(*subject_value),
                                       subject_symbol, std::move(arms)));
}

auto lowerer::lower_if_fallback(
    const std::vector<ast::if_branch> &branches,
    const std::vector<ast::ptr<ast::node>> &else_body, size_t index,
    source_span span, type_id type)
    -> std::expected<ptr<hir_block>, lowering_error> {
  if (index >= branches.size()) {
    if (else_body.empty()) {
      return ptr<hir_block>{};
    }
    return lower_block(else_body, span, type);
  }
  auto rest = lower_if_chain(branches, else_body, index, span, type);
  if (!rest.has_value()) {
    return std::unexpected(rest.error());
  }
  const auto rest_span = branches[index].span;
  auto stmts = ptr_vec<hir_node>{};
  stmts.push_back(
      ptr<hir_node>(make<hir_expr_stmt>(rest_span, std::move(*rest))));
  return make<hir_block>(rest_span, type, std::move(stmts));
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
  if (iterable_entry.kind == type_kind::builtin_generic_kind &&
      iterable_entry.name == "generator") {
    return lower_generator_loop(for_stmt.span, *for_stmt.iterable,
                                *iterable_type, loop_var, inner_stmts);
  }
  if (const auto it = checked_.for_iterator_dispatches.find(&for_stmt);
      it != checked_.for_iterator_dispatches.end()) {
    return lower_iterator_loop(for_stmt.span, *for_stmt.iterable,
                               *iterable_type, loop_var, it->second,
                               inner_stmts);
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

auto lowerer::lower_generator_loop(
    source_span span, const ast::expr &iterable, type_id iterable_type,
    const ast::binding_pattern &loop_var,
    const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
        &inner_stmts) -> std::expected<ptr_vec<hir_node>, lowering_error> {
  const auto &entry = checked_.types.entry(iterable_type);
  const auto element_type = entry.args.empty() ? k_unknown_type : entry.args[0];
  if (element_type == k_unknown_type || element_type == k_error_type) {
    return fail(lowering_error_kind::unresolved_type, span,
                "the generator's item type did not resolve to a concrete "
                "type");
  }

  // Evaluate the generator handle exactly once — `for x in g(): ...` must
  // not call `g()` again on every iteration, only `.next()` on the handle
  // it already produced.
  auto generator_value = lower_expr(iterable);
  if (!generator_value.has_value()) {
    return std::unexpected(generator_value.error());
  }
  auto result = ptr_vec<hir_node>{};
  const auto generator_symbol = mint_symbol();
  result.push_back(ptr<hir_node>(make<hir_let>(iterable.span, generator_symbol,
                                               std::string("<for generator>"),
                                               std::move(*generator_value))));

  // `option[T]` for this exact `T` may not already be interned — a
  // for-loop's desugaring never writes a literal `.next()` call for the
  // checker to have interned one for. Safe to intern now anyway: a
  // `type_table`'s `intern` is a pure, memoized, append-only operation,
  // and lowering only ever runs after checking has fully populated the
  // table, so this can't retroactively change anything checking already
  // decided — it only adds an entry checking simply never had occasion to
  // add itself.
  const auto option_type = const_cast<semantic::type_table &>(checked_.types)
                               .builtin_generic("option", {element_type});

  const auto subject_symbol = mint_symbol();
  auto subject = ptr<hir_expr>(make<hir_generator_next>(
      span, option_type,
      ptr<hir_expr>(make<hir_local_ref>(iterable.span, iterable_type,
                                        generator_symbol,
                                        std::string("<for generator>")))));

  push_scope();
  const auto loop_var_symbol = declare_local(loop_var.name, element_type);
  auto body_stmts = ptr_vec<hir_node>{};
  body_stmts.push_back(ptr<hir_node>(make<hir_let>(
      span, loop_var_symbol, loop_var.name,
      ptr<hir_expr>(make<hir_variant_payload>(
          span, element_type,
          ptr<hir_expr>(make<hir_local_ref>(span, option_type, subject_symbol,
                                            std::string("<for subject>"))),
          std::string("some"), size_t{0})))));

  auto inner = inner_stmts();
  if (!inner.has_value()) {
    pop_scope();
    return std::unexpected(inner.error());
  }
  for (auto &stmt_ptr : *inner) {
    body_stmts.push_back(std::move(stmt_ptr));
  }
  auto body_block =
      make<hir_block>(span, k_unknown_type, std::move(body_stmts));
  pop_scope();

  auto some_args = ptr_vec<hir_pattern>{};
  some_args.push_back(ptr<hir_pattern>(make<hir_wildcard_pattern>(span)));
  auto pattern = ptr<hir_pattern>(make<hir_constructor_pattern>(
      span, std::string("some"), std::move(some_args)));

  result.push_back(ptr<hir_node>(
      make<hir_while_let>(span, std::move(subject), subject_symbol,
                          std::move(pattern), std::move(body_block))));
  return result;
}

auto lowerer::lower_iterator_loop(
    source_span span, const ast::expr &iterable, type_id iterable_type,
    const ast::binding_pattern &loop_var,
    const semantic::iterator_loop_dispatch &dispatch,
    const std::function<std::expected<ptr_vec<hir_node>, lowering_error>()>
        &inner_stmts) -> std::expected<ptr_vec<hir_node>, lowering_error> {
  const auto element_type = dispatch.element_type;
  if (element_type == k_unknown_type || element_type == k_error_type) {
    return fail(lowering_error_kind::unresolved_type, span,
                "the iterator's item type did not resolve to a concrete "
                "type");
  }

  // Evaluate the iterator handle exactly once — `for x in make_iter(): ...`
  // must not re-run `make_iter()` per iteration, only `.next()` on the handle
  // it produced. `next(mut self)` mutates that handle's fields through the
  // pointer the local holds, so the binding itself never needs reassigning.
  auto handle_value = lower_expr(iterable);
  if (!handle_value.has_value()) {
    return std::unexpected(handle_value.error());
  }
  auto result = ptr_vec<hir_node>{};
  const auto handle_symbol = mint_symbol();
  result.push_back(ptr<hir_node>(make<hir_let>(iterable.span, handle_symbol,
                                               std::string("<for iterator>"),
                                               std::move(*handle_value))));

  // See `lower_generator_loop` for why interning `option[T]` here is safe.
  const auto option_type = const_cast<semantic::type_table &>(checked_.types)
                               .builtin_generic("option", {element_type});

  // Rebuild the `handle.next()` method call directly from the resolved
  // dispatch — the same `Type::method`-mangled `hir_local_ref` callee shape
  // `lower_call` produces for a hand-written method call (both backends
  // dispatch it by name + `owner_module`, see their `resolve_callee_key`).
  const auto method_name =
      dispatch.impl_target_type.empty()
          ? dispatch.decl->name
          : std::format("{}::{}", dispatch.impl_target_type,
                        dispatch.decl->name);
  const auto callee_symbol = resolve_reference(method_name);
  auto callee = ptr<hir_expr>(make<hir_local_ref>(
      span, k_unknown_type, callee_symbol, method_name, dispatch.owner_module));
  auto call_args = ptr_vec<hir_expr>{};
  call_args.push_back(ptr<hir_expr>(
      make<hir_local_ref>(iterable.span, iterable_type, handle_symbol,
                          std::string("<for iterator>"))));

  const auto subject_symbol = mint_symbol();
  auto subject = ptr<hir_expr>(make<hir_call>(
      span, option_type, std::move(callee), std::move(call_args)));

  push_scope();
  const auto loop_var_symbol = declare_local(loop_var.name, element_type);
  auto body_stmts = ptr_vec<hir_node>{};
  body_stmts.push_back(ptr<hir_node>(make<hir_let>(
      span, loop_var_symbol, loop_var.name,
      ptr<hir_expr>(make<hir_variant_payload>(
          span, element_type,
          ptr<hir_expr>(make<hir_local_ref>(span, option_type, subject_symbol,
                                            std::string("<for subject>"))),
          std::string("some"), size_t{0})))));

  auto inner = inner_stmts();
  if (!inner.has_value()) {
    pop_scope();
    return std::unexpected(inner.error());
  }
  for (auto &stmt_ptr : *inner) {
    body_stmts.push_back(std::move(stmt_ptr));
  }
  auto body_block =
      make<hir_block>(span, k_unknown_type, std::move(body_stmts));
  pop_scope();

  auto some_args = ptr_vec<hir_pattern>{};
  some_args.push_back(ptr<hir_pattern>(make<hir_wildcard_pattern>(span)));
  auto pattern = ptr<hir_pattern>(make<hir_constructor_pattern>(
      span, std::string("some"), std::move(some_args)));

  result.push_back(ptr<hir_node>(
      make<hir_while_let>(span, std::move(subject), subject_symbol,
                          std::move(pattern), std::move(body_block))));
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
  // A const-generic *instance* keeps the template's `[n: usize]` parameter
  // list, but every type in it was already resolved against the constant that
  // instance stands for (`semantic::checker::instantiate_const_generic`), so
  // there is nothing generic left to lower — `checked_type_of` reads back
  // `array[int32, 3]`, not `array[int32, n]`. Every other generic function is
  // still a template, with no runtime form until it is instantiated.
  if (!decl.type_params.empty() && !is_const_generic_instance(decl, checked_)) {
    return fail(lowering_error_kind::unsupported_construct, decl.span,
                "generic functions over *types* are not lowered until type "
                "monomorphization exists (spec/typed-ir-design.md Decision 2, "
                "phase 5); a function generic only over compile-time values "
                "is compiled once per constant it is called with");
  }
  if (decl.return_type == nullptr) {
    return fail(lowering_error_kind::missing_return_type, decl.span,
                std::format("function `{}` has no declared return type; the "
                            "first lowering milestone only lowers explicitly "
                            "annotated signatures",
                            decl.name));
  }
  // A generator's body isn't a function body: it compiles into a step
  // function resumed once per `next()`, so a `pre` prepended to it would run
  // on every resumption rather than once on entry, and its `return` statements
  // mean "exhausted", not "here is the result". Both faces of a contract lose
  // their meaning under that translation, so refuse rather than emit checks
  // that fire at the wrong times (Decision 1: lowering fails closed).
  if (decl.modifiers.is_generator && !decl.contracts.empty()) {
    return fail(lowering_error_kind::unsupported_construct, decl.span,
                std::format("`pre`/`post` conditions on the `generator def` "
                            "`{}` are checked statically but are not lowered "
                            "to runtime checks yet, because a generator's body "
                            "runs in steps rather than as one call",
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

  // Preconditions run before the body does — that is the whole of what "pre"
  // means — so they go at the front, after the parameter prelude that binds
  // the names they talk about. Postconditions can't be placed until an exit
  // exists to place them at, so they're parked here for `lower_return_value`
  // to pick up at each one.
  for (const auto &contract : decl.contracts) {
    if (!needs_runtime_check(contract)) {
      continue;
    }
    if (!contract.is_pre) {
      post_contracts_.push_back(&contract);
      continue;
    }
    auto check = lower_contract_check(contract, contract_kind::precondition);
    if (!check.has_value()) {
      pop_scope();
      post_contracts_.clear();
      return std::unexpected(check.error());
    }
    param_prelude.push_back(std::move(*check));
  }

  auto body = std::expected<ptr<hir_block>, lowering_error>{};
  if (decl.body_expr != nullptr) {
    auto expr_type = checked_type_of(*decl.body_expr);
    if (!expr_type.has_value()) {
      pop_scope();
      post_contracts_.clear();
      return std::unexpected(expr_type.error());
    }
    auto value = lower_expr(*decl.body_expr);
    if (!value.has_value()) {
      pop_scope();
      post_contracts_.clear();
      return std::unexpected(value.error());
    }
    auto exit = lower_return_value(decl.body_expr->span, std::move(*value));
    if (!exit.has_value()) {
      pop_scope();
      post_contracts_.clear();
      return std::unexpected(exit.error());
    }
    auto stmts = std::move(param_prelude);
    for (auto &stmt_ptr : *exit) {
      stmts.push_back(std::move(stmt_ptr));
    }
    body =
        make<hir_block>(decl.body_expr->span, *return_type, std::move(stmts));
  } else {
    // A generator body's tail isn't a return value the way an ordinary
    // function's is (its only output channel is `yield`, checked at the
    // semantic layer against the item type, not the function's own
    // `generator[T]` return type) — pass `k_unknown_type` so a trailing
    // `if`/`match` statement isn't force-typed against `generator[T]`.
    body = lower_block(decl.body_stmts, decl.span,
                       decl.modifiers.is_generator ? k_unknown_type
                                                   : *return_type);
    if (body.has_value() && !param_prelude.empty()) {
      auto merged = std::move(param_prelude);
      for (auto &stmt_ptr : (*body)->stmts) {
        merged.push_back(std::move(stmt_ptr));
      }
      (*body)->stmts = std::move(merged);
    }
    // Falling off the end of the body is an exit too, and the one a `post`
    // most often has to cover — every explicit `return` inside the body was
    // already routed through `lower_return_value` by `lower_stmt`. There are
    // three shapes of end:
    //
    //   - The body never reaches its end (`always_returns`): every exit is a
    //     `return` that has already been checked. Nothing to add — and adding
    //     anything would be code after a terminator, which LLVM rejects.
    //   - It ends in a trailing expression, which *is* the returned value: it
    //     becomes an explicit `return` (which both backends compile
    //     identically to the implicit one) with the postconditions checked
    //     against it.
    //   - It just runs off the end, returning `unit`. There is nothing for
    //     `return` to name (the checker rejects `return` in a `unit`
    //     function's `post`), so the checks simply go last.
    if (body.has_value() && !post_contracts_.empty()) {
      auto &stmts = (*body)->stmts;
      auto *tail = stmts.empty() ? nullptr : stmts.back().get();
      if (tail != nullptr && always_returns(*tail)) {
        // Nothing to do — see the first case above.
      } else if (tail != nullptr &&
                 tail->kind == hir_node_kind::hir_expr_stmt) {
        auto &tail_stmt = dynamic_cast<hir_expr_stmt &>(*tail);
        auto exit =
            lower_return_value(tail_stmt.span, std::move(tail_stmt.expr));
        if (!exit.has_value()) {
          pop_scope();
          post_contracts_.clear();
          return std::unexpected(exit.error());
        }
        stmts.pop_back();
        for (auto &stmt_ptr : *exit) {
          stmts.push_back(std::move(stmt_ptr));
        }
      } else {
        for (const auto *contract : post_contracts_) {
          auto check =
              lower_contract_check(*contract, contract_kind::postcondition);
          if (!check.has_value()) {
            pop_scope();
            post_contracts_.clear();
            return std::unexpected(check.error());
          }
          stmts.push_back(std::move(*check));
        }
      }
    }
  }
  pop_scope();
  post_contracts_.clear();
  if (!body.has_value()) {
    return std::unexpected(body.error());
  }

  // A `generator def`'s declared return type resolved (via `check_function`,
  // `src/semantic/check.cpp`) to the concrete `generator[T]` — extract `T`
  // the same way `?`/`for`-loop lowering elsewhere in this file unwraps a
  // builtin generic's type argument.
  auto item_type = k_unknown_type;
  if (decl.modifiers.is_generator) {
    const auto &return_entry = checked_.types.entry(*return_type);
    if (!return_entry.args.empty()) {
      item_type = return_entry.args.front();
    }
  }

  return make<hir_function>(decl.span, decl.name, std::move(params),
                            *return_type, std::move(*body),
                            decl.modifiers.is_generator, item_type);
}

} // namespace

auto lower_function(const ast::func_decl &decl,
                    const semantic::checked_types &checked,
                    const lowering_options &options)
    -> std::expected<ptr<hir_function>, lowering_error> {
  auto walker = lowerer(checked, options);
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
    const lowering_options &options, ptr_vec<hir_function> &functions)
    -> std::expected<void, lowering_error> {
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
    if (decl.modifiers.is_intrinsic) {
      continue;
    }
    auto lowered = lower_function(decl, checked, options);
    if (!lowered.has_value()) {
      // A method generic over its own type parameters (`def pure[A](a: A)`,
      // the shape every higher-kinded trait method takes) lowers erased,
      // exactly like a generic free function — but a body relying on a
      // construct erasure can't express is skipped rather than failing the
      // whole module, matching the historical behavior (generic impl
      // methods used to be skipped unconditionally). Non-generic methods
      // keep the hard failure: for them an error is always a real bug.
      if (!decl.type_params.empty()) {
        continue;
      }
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
                                        const lowering_options &options,
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
    auto lowered = lower_function(decl, checked, options);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    (*lowered)->name = std::format("{}::{}", *target_name, decl.name);
    functions.push_back(std::move(*lowered));
  }
  return {};
}

auto lower_module(const ast::file &file, std::string module_name,
                  const semantic::checked_types &checked,
                  const lowering_options &options)
    -> std::expected<ptr<hir_module>, lowering_error> {
  auto functions = ptr_vec<hir_function>{};
  for (const auto &item : file.items) {
    if (item == nullptr) {
      continue;
    }
    if (item->kind == ast::node_kind::impl_decl) {
      auto result = lower_impl_associated_functions(
          dynamic_cast<const ast::impl_decl &>(*item), checked, options,
          functions);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      continue;
    }
    if (item->kind == ast::node_kind::extend_decl) {
      auto result =
          lower_extend_methods(dynamic_cast<const ast::extend_decl &>(*item),
                               checked, options, functions);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      continue;
    }
    if (item->kind != ast::node_kind::func_decl) {
      continue;
    }
    const auto &decl = dynamic_cast<const ast::func_decl &>(*item);
    if (is_const_generic_template(decl) || is_type_generic_template(decl)) {
      // Compiled once per constant (or concrete type) it is called with, from
      // the instances loop below — never as itself. The instances the checker
      // monomorphized are registered as `const_generic_instance`s regardless
      // of whether the template was generic over values or types.
      continue;
    }
    if (decl.modifiers.is_intrinsic) {
      // No body to lower — the bytecode compiler recognizes calls to a
      // known intrinsic name by itself (src/intrinsics.h) and emits
      // `op_call_intrinsic` directly, so this function never needs an entry
      // in the module's `hir_function` table.
      continue;
    }
    if (decl.modifiers.is_static) {
      // A `static def` only ever runs inside `comptime::evaluator` (see
      // `semantic::checker::register_comptime_globals`) — it has no
      // runtime call site and no runtime representation. Its body may
      // even use constructs ordinary lowering can't handle at all (a
      // quote expression, for instance), so attempting to lower it is
      // both unnecessary and can be a hard failure for otherwise-valid
      // code.
      continue;
    }
    auto lowered = lower_function(decl, checked, options);
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
    auto lowered = lower_function(*synthesized.decl, checked, options);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    (*lowered)->name = std::format("{}::{}", synthesized.target_type_name,
                                   synthesized.decl->name);
    functions.push_back(std::move(*lowered));
  }
  // One `hir_function` per constant some call site instantiated a
  // const-generic template with (`semantic::checker::instantiate_const_
  // generic`) — `get$3` and `get$8` are two ordinary functions here, and the
  // template they came from (skipped in the item walk above) is none. Each
  // instance's clone already carries its mangled name, which is the same name
  // `lower_call` emits for the call sites that asked for it.
  for (const auto &instance : checked.const_generic_instances) {
    if (instance.owner_module != module_name || instance.decl == nullptr) {
      continue;
    }
    auto lowered = lower_function(*instance.decl, checked, options);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    functions.push_back(std::move(*lowered));
  }
  // Item-level splices (`semantic::checker::resolve_item_splices`,
  // check.cpp) injected these `impl` blocks directly into `program_index`
  // rather than into any file's `items` — lower them here the same way, by
  // module name, that trait defaults are lowered just above; they'd
  // otherwise never be reached by the `file.items` walk further up.
  for (const auto &splice : checked.synthesized_item_splices) {
    if (splice.owner_module != module_name || splice.impl == nullptr) {
      continue;
    }
    auto result = lower_impl_associated_functions(*splice.impl, checked,
                                                  options, functions);
    if (!result.has_value()) {
      return std::unexpected(result.error());
    }
  }
  return make<hir_module>(file.span, std::move(module_name),
                          std::move(functions));
}

auto lower_functor_modules(const semantic::checked_types &checked,
                           const lowering_options &options)
    -> std::expected<ptr_vec<hir_module>, lowering_error> {
  // Group the cloned `def`s by their synthetic module name, preserving first-
  // seen order so the emitted modules are deterministic. Each group becomes
  // one standalone `hir_module`, named exactly as the `db.f(...)` call sites
  // record their callee's owner module.
  auto module_order = std::vector<std::string>{};
  auto grouped = std::unordered_map<std::string, ptr_vec<hir_function>>{};
  for (const auto &instance : checked.functor_instances) {
    if (instance.decl == nullptr) {
      continue;
    }
    auto lowered = lower_function(*instance.decl, checked, options);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    // A functor-body `impl`/`extend` method lowers under its target-qualified
    // name (`handle::show`) — the same key a `receiver.method()` call records —
    // so cross-module dispatch within the synthetic module links up. The
    // owner module (a unique per-instantiation synthetic name) keeps two
    // instantiations' `handle::show` distinct.
    if (!instance.impl_target.empty()) {
      (*lowered)->name =
          std::format("{}::{}", instance.impl_target, instance.decl->name);
    }
    auto [it, inserted] = grouped.try_emplace(instance.owner_module);
    if (inserted) {
      module_order.push_back(instance.owner_module);
    }
    it->second.push_back(std::move(*lowered));
  }

  auto modules = ptr_vec<hir_module>{};
  for (const auto &name : module_order) {
    auto &functions = grouped.at(name);
    // A synthetic module has no source span of its own; use its first
    // function's span so diagnostics have somewhere to point.
    const auto span =
        functions.empty() ? source_span{} : functions.front()->span;
    modules.push_back(make<hir_module>(span, name, std::move(functions)));
  }
  return modules;
}

} // namespace kira::hir
