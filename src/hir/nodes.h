#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/hir/ids.h"
#include "src/k-parser/ast.h"
#include "src/k-parser/source_location.h"
#include "src/k-parser/token.h"
#include "src/semantic/types.h"

namespace kira::hir {

using semantic::k_unknown_type;
using semantic::type_id;

// ==========================================================================
//  hir_node_kind — flat tag, same shape as ast::node_kind (see
//  spec/typed-ir-design.md Decision 4). Kinds beyond this first lowering
//  milestone (tuple/struct-init/cast/lambda) are listed here for the record
//  — the milestone's Non-Goals explicitly defer them — but have no concrete
//  node struct below yet; nothing constructs them until the step that adds
//  their lowering. `match` and its patterns were added in the extension
//  that lowers `match`/pattern aliases (Decision 6, items 1-2); only the
//  structural-dispatch pattern kinds below have node structs — destructuring
//  patterns (constructor/tuple/struct/...) still need projection
//  expressions this milestone doesn't have.
// ==========================================================================
enum class hir_node_kind : uint8_t {
  // expressions
  hir_literal,
  hir_local_ref,
  hir_binary,
  hir_unary,
  hir_call,
  hir_field,
  hir_index,
  hir_tuple,
  hir_struct_init,
  hir_cast,
  hir_block,
  hir_if,
  hir_match,
  hir_lambda,
  // patterns (match arms only)
  hir_wildcard_pattern,
  hir_literal_pattern,
  hir_or_pattern,
  // statements
  hir_let,
  hir_assign,
  hir_expr_stmt,
  hir_return,
  // items
  hir_function,
  hir_module,
};

/// @brief Common base for every HIR node.
///
/// Mirrors `ast::node`: a runtime kind tag plus the source span it was
/// lowered from (Decision 5 — spans are copied, never re-synthesized).
/// `type` is only meaningful on expression nodes, where lowering guarantees
/// it is always concrete (Decision 1: `k_unknown_type`/`k_error_type`
/// reaching this far is a lowering failure, not a value that survives into
/// HIR); statement and item nodes leave it at the default, unused.
struct hir_node {
  hir_node_kind kind;
  source_span span;
  type_id type = k_unknown_type;

  explicit hir_node(hir_node_kind k, source_span s, type_id t = k_unknown_type)
      : kind(k), span(s), type(t) {}
  virtual ~hir_node() = default;

  // Non-copyable, movable — same rationale as ast::node.
  hir_node(const hir_node &) = delete;
  auto operator=(const hir_node &) -> hir_node & = delete;
  hir_node(hir_node &&) = default;
  auto operator=(hir_node &&) -> hir_node & = default;
};

/// @brief Canonical owning pointer for HIR nodes, mirroring `ast::ptr`.
template <typename T> using ptr = std::unique_ptr<T>;

/// @brief Owning list of homogeneous HIR nodes, mirroring `ast::ptr_vec`.
template <typename T> using ptr_vec = std::vector<ptr<T>>;

/// @brief Constructs a HIR node with ownership wrapped in `ptr<T>`, mirroring
/// `ast::make`.
template <typename T, typename... Args>
[[nodiscard]] auto make(Args &&...args) -> ptr<T> {
  return std::make_unique<T>(std::forward<Args>(args)...);
}

/// Base for value-position HIR nodes; lowering guarantees `type` is always
/// concrete on these (Decision 1).
struct hir_expr : hir_node {
  using hir_node::hir_node;
};

/// Base for statement-position HIR nodes.
struct hir_stmt : hir_node {
  using hir_node::hir_node;
};

/// Base for top-level item HIR nodes (functions, modules).
struct hir_item : hir_node {
  using hir_node::hir_node;
};

/// Base for pattern-position HIR nodes, used only inside `hir_match_arm`.
/// Patterns describe *structural* dispatch only — matches unconditionally,
/// or compares against a literal. A name a surface pattern would bind
/// (`x`, or a `pattern as name` alias) is never represented as a pattern
/// node: it lowers to an ordinary `hir_let` prepended to the arm's body
/// instead (Decision 6, item 2 — reuse `hir_let` rather than invent a
/// binding-carrying pattern kind), so no pattern struct below carries a
/// `symbol_id`.
struct hir_pattern : hir_node {
  using hir_node::hir_node;
};

// ==========================================================================
//  Expressions
// ==========================================================================

/// Literal value with its checked type already resolved. The raw spelling
/// is kept rather than pre-parsed into a machine value — same deferral as
/// `ast::literal_expr` — so codegen picks the concrete representation once.
struct hir_literal : hir_expr {
  token_kind lit_kind;
  std::string value;

  hir_literal(source_span s, type_id t, token_kind lk, std::string v)
      : hir_expr(hir_node_kind::hir_literal, s, t), lit_kind(lk),
        value(std::move(v)) {}
};

/// Reference to a resolved local binding (parameter, `let`/`var`, or
/// function name) — see Decision 7: reuses `semantic::symbol_id` rather
/// than minting a parallel HIR-local identity scheme.
struct hir_local_ref : hir_expr {
  symbol_id symbol = k_invalid_symbol_id;
  std::string name; ///< Preserved for diagnostics/debugging only.

  hir_local_ref(source_span s, type_id t, symbol_id sym, std::string n)
      : hir_expr(hir_node_kind::hir_local_ref, s, t), symbol(sym),
        name(std::move(n)) {}
};

/// Binary operator application; reuses `ast::binary_op` rather than
/// reinventing operator identity (Decision 7's rationale applies equally
/// here).
struct hir_binary : hir_expr {
  ast::binary_op op;
  ptr<hir_expr> lhs;
  ptr<hir_expr> rhs;

  hir_binary(source_span s, type_id t, ast::binary_op o, ptr<hir_expr> l,
             ptr<hir_expr> r)
      : hir_expr(hir_node_kind::hir_binary, s, t), op(o), lhs(std::move(l)),
        rhs(std::move(r)) {}
};

/// Unary operator application; reuses `ast::unary_op`.
struct hir_unary : hir_expr {
  ast::unary_op op;
  ptr<hir_expr> operand;

  hir_unary(source_span s, type_id t, ast::unary_op o,
            ptr<hir_expr> operand_arg)
      : hir_expr(hir_node_kind::hir_unary, s, t), op(o),
        operand(std::move(operand_arg)) {}
};

/// Function/method call. Arguments are already positional — named-argument
/// order is resolved against the callee's signature during type checking,
/// before lowering.
struct hir_call : hir_expr {
  ptr<hir_expr> callee;
  ptr_vec<hir_expr> args;

  hir_call(source_span s, type_id t, ptr<hir_expr> c, ptr_vec<hir_expr> a)
      : hir_expr(hir_node_kind::hir_call, s, t), callee(std::move(c)),
        args(std::move(a)) {}
};

/// Field access `expr.name`.
struct hir_field : hir_expr {
  ptr<hir_expr> object;
  std::string field_name;

  hir_field(source_span s, type_id t, ptr<hir_expr> obj, std::string name)
      : hir_expr(hir_node_kind::hir_field, s, t), object(std::move(obj)),
        field_name(std::move(name)) {}
};

/// Index access `expr[index]`.
struct hir_index : hir_expr {
  ptr<hir_expr> object;
  ptr<hir_expr> index;

  hir_index(source_span s, type_id t, ptr<hir_expr> obj, ptr<hir_expr> idx)
      : hir_expr(hir_node_kind::hir_index, s, t), object(std::move(obj)),
        index(std::move(idx)) {}
};

/// Indentation-delimited block. In expression position its `type` is the
/// type of the trailing expression (or `unit` if the block ends in a
/// non-expression statement); statement-only blocks (a function body, an
/// `if` arm) reuse the same node with `type` left at the unused default.
struct hir_block : hir_expr {
  ptr_vec<hir_node> stmts;

  hir_block(source_span s, type_id t, ptr_vec<hir_node> body)
      : hir_expr(hir_node_kind::hir_block, s, t), stmts(std::move(body)) {}
};

/// One `if`/`elif` branch: condition plus the block executed when it holds.
struct hir_if_branch {
  ptr<hir_expr> condition;
  ptr<hir_block> body;
};

/// Conditional, usable as either a statement or an expression (Decision 6,
/// item 1) — the two share this one node kind, distinguished only by
/// whether the checker resolved a non-`unit` type for it, matching how
/// `ast::if_stmt`/`ast::if_expr` already share the same `if_branch` shape.
struct hir_if : hir_expr {
  std::vector<hir_if_branch> branches;
  ptr<hir_block> else_body; ///< Null when there is no `else`.

  hir_if(source_span s, type_id t, std::vector<hir_if_branch> b,
         ptr<hir_block> else_b)
      : hir_expr(hir_node_kind::hir_if, s, t), branches(std::move(b)),
        else_body(std::move(else_b)) {}
};

/// One `case`/arm of a `match`. `guard` is null when the arm has no `if`
/// guard. See `hir_pattern`'s doc comment: any name the surface arm binds
/// (a plain binding or a `pattern as name` alias) shows up as an ordinary
/// `hir_let` at the front of `body`'s statements, not as part of `pattern`.
struct hir_match_arm {
  ptr<hir_pattern> pattern;
  ptr<hir_expr> guard; ///< Null when the arm has no guard.
  ptr<hir_block> body;
};

/// Pattern-dispatch, usable as either a statement or an expression
/// (Decision 6, item 1), matching how `ast::match_stmt`/`ast::match_expr`
/// already share `ast::match_arm`. `subject` is evaluated exactly once;
/// later codegen binds that one value to `subject_symbol`, which is what
/// every arm's dispatch — and any `hir_let` a plain binding or pattern
/// alias generates in an arm's body — refers back to, instead of
/// re-evaluating `subject`.
struct hir_match : hir_expr {
  ptr<hir_expr> subject;
  symbol_id subject_symbol = k_invalid_symbol_id;
  std::vector<hir_match_arm> arms;

  hir_match(source_span s, type_id t, ptr<hir_expr> subj, symbol_id subj_sym,
            std::vector<hir_match_arm> a)
      : hir_expr(hir_node_kind::hir_match, s, t), subject(std::move(subj)),
        subject_symbol(subj_sym), arms(std::move(a)) {}
};

/// `_` — matches unconditionally. Also stands in for a plain name binding
/// (`x`) and for `pattern as name`: both reduce to "match unconditionally,
/// then let-bind" (see `hir_match_arm`), so neither needs its own pattern
/// kind.
struct hir_wildcard_pattern : hir_pattern {
  explicit hir_wildcard_pattern(source_span s)
      : hir_pattern(hir_node_kind::hir_wildcard_pattern, s) {}
};

/// A literal value pattern, e.g. `42`, `"hello"`, `true`.
struct hir_literal_pattern : hir_pattern {
  token_kind lit_kind;
  std::string value;

  hir_literal_pattern(source_span s, token_kind lk, std::string v)
      : hir_pattern(hir_node_kind::hir_literal_pattern, s), lit_kind(lk),
        value(std::move(v)) {}
};

/// `a | b | c` — matches if any alternative matches. Lowering rejects an
/// alternative that binds a name: a binding in one `|` branch but not the
/// others has no sound meaning without richer exhaustiveness bookkeeping
/// this milestone doesn't implement.
struct hir_or_pattern : hir_pattern {
  ptr_vec<hir_pattern> alternatives;

  hir_or_pattern(source_span s, ptr_vec<hir_pattern> alts)
      : hir_pattern(hir_node_kind::hir_or_pattern, s),
        alternatives(std::move(alts)) {}
};

// ==========================================================================
//  Statements
// ==========================================================================

/// `let name = expr`. Lowering only handles the simple single-name binding
/// case — destructuring `let` patterns are still deferred — so this stores
/// a resolved `symbol_id` directly rather than a pattern tree. Also reused,
/// synthetically, to represent a `match` arm's plain binding or pattern
/// alias (see `hir_match_arm`), with `initializer` referencing the match's
/// `subject_symbol` instead of a source-level expression.
struct hir_let : hir_stmt {
  symbol_id symbol = k_invalid_symbol_id;
  std::string name; ///< Preserved for diagnostics/debugging only.
  ptr<hir_expr> initializer;

  hir_let(source_span s, symbol_id sym, std::string n, ptr<hir_expr> init)
      : hir_stmt(hir_node_kind::hir_let, s), symbol(sym), name(std::move(n)),
        initializer(std::move(init)) {}
};

/// Expression evaluated for its side effect (or, as a block's trailing
/// statement, its value) — mirrors `ast::expr_stmt`.
struct hir_expr_stmt : hir_stmt {
  ptr<hir_expr> expr;

  hir_expr_stmt(source_span s, ptr<hir_expr> e)
      : hir_stmt(hir_node_kind::hir_expr_stmt, s), expr(std::move(e)) {}
};

/// `return [expr]`.
struct hir_return : hir_stmt {
  ptr<hir_expr> value; ///< Null for a bare `return`.

  hir_return(source_span s, ptr<hir_expr> v)
      : hir_stmt(hir_node_kind::hir_return, s), value(std::move(v)) {}
};

// ==========================================================================
//  Items
// ==========================================================================

/// One lowered function parameter — resolved symbol plus its checked type.
struct hir_param {
  symbol_id symbol = k_invalid_symbol_id;
  std::string name;
  type_id type = k_unknown_type;
};

/// Lowered function. Decision 1 guarantees every parameter and the return
/// type are concrete by the time this node exists — a function with an
/// unannotated parameter fails lowering before a `hir_function` is built.
struct hir_function : hir_item {
  std::string name;
  std::vector<hir_param> params;
  type_id return_type = k_unknown_type;
  ptr<hir_block> body;

  hir_function(source_span s, std::string n, std::vector<hir_param> p,
               type_id ret, ptr<hir_block> b)
      : hir_item(hir_node_kind::hir_function, s), name(std::move(n)),
        params(std::move(p)), return_type(ret), body(std::move(b)) {}
};

/// Lowered module: every function lowered from one module's contributing
/// files (Decision 3 keeps HIR a separate tree rather than decorating the
/// AST in place).
struct hir_module : hir_item {
  std::string module_name;
  ptr_vec<hir_function> functions;

  hir_module(source_span s, std::string name, ptr_vec<hir_function> funcs)
      : hir_item(hir_node_kind::hir_module, s), module_name(std::move(name)),
        functions(std::move(funcs)) {}
};

} // namespace kira::hir
