#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/hir/ids.h"
#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/parser/token.h"
#include "src/semantic/types.h"

namespace kira::hir {

using semantic::k_unknown_type;
using semantic::type_id;

// ==========================================================================
//  hir_node_kind — flat tag, same shape as ast::node_kind (see
//  spec/typed-ir-design.md Decision 4). Kinds beyond this first lowering
//  milestone (tuple literal, struct-init literal, cast, lambda) are listed
//  here for the record — the milestone's Non-Goals explicitly defer them —
//  but have no concrete node struct below yet; nothing constructs them
//  until the step that adds their lowering. `match` and its patterns were
//  added lowering `match`/pattern aliases (Decision 6, items 1-2);
//  destructuring patterns (tuple/struct/constructor/range/array) and their
//  supporting projection expressions (`hir_tuple_index`,
//  `hir_variant_payload`) were added in the extension that tackles
//  destructuring. `hir_array_pattern` covers array/list/slice element
//  destructuring — this grammar has no rest/slice-capture syntax in pattern
//  position (`..` there is always an open-ended `range_pattern`, never a
//  "gather the rest" marker), so it's a plain fixed-arity structural
//  pattern exactly like `hir_tuple_pattern`, just over an indexable rather
//  than a tuple-shaped value; any length mismatch is a runtime dispatch
//  concern for codegen (phase 5), the same as a literal or tag comparison.
//  `hir_assign` and `hir_while` (plain-condition form only) were added in
//  the mutation/control-flow extension; `for` and `while let` remain
//  unsupported — `for` needs an iterator protocol this project hasn't
//  decided on yet (see spec/typed-ir-design.md's open questions), and
//  `while let` needs repeated structural dispatch (re-testing a pattern
//  every iteration) that hasn't been designed for loops. `hir_tuple` and
//  `hir_struct_init` (construction, not the pattern kinds of similar name)
//  got node structs in the literal-construction extension, alongside the
//  new `hir_array_init`; `hir_cast` got one in the extension after that.
//  `hir_lambda` and `where_expr`-as-`hir_block` (no new node needed) were
//  added in the "finish lowering" pass. Named/reordered call arguments
//  were added after that (`hir_call.args` stays purely positional — the
//  reordering happens during lowering, via a persisted per-call-site
//  argument-to-parameter mapping; see `hir::lower_call`); default
//  parameter values are still unsupported (a call omitting a defaulted
//  argument still rejects — the default expression's own evaluation
//  context isn't threaded through this pass). `hir_variant_init` (the
//  inverse of `hir_constructor_pattern` — see its doc comment) and
//  `try_expr` (which lowers to a synthetic two-arm `hir_match` using it)
//  were added after that. `for` loops resolved the open iterator-protocol
//  question (see spec/iterator-protocol-design.md — a closed, compiler-
//  recognized set of shapes, not a general trait, since a real protocol
//  needs mutable iterator state and borrow/ownership semantics don't
//  exist yet): range-literal loops came first (no new node needed, just
//  `hir_while`), then `array`/`list`/`slice`/`slice_mut`/`str` loops,
//  which needed one new node — `hir_container_len` — since a container's
//  element count isn't always statically known (an `array[T, N]`'s is,
//  and uses a plain `hir_literal` instead). `option` iteration (a plain
//  `hir_match`, no loop) and `while let` (`hir_while_let`, re-testing a
//  pattern every iteration and falling out on the first mismatch — see
//  its own doc comment) came after that. `for` comprehensions came last,
//  reusing every iterable shape above (nested when a comprehension has
//  more than one clause) plus `hir_array_init` for the fresh accumulator
//  and one new node — `hir_list_push` — to grow it, for the same reason
//  `hir_container_len` isn't a synthesized `.len()` call. Still explicitly
//  unsupported: the concurrency forms (`async`/`await`/`par`/`race`/
//  `crew`/`on`) and compile-time forms (`quote`/`splice`/`static`),
//  monomorphization (phase 5), and borrow/ownership metadata — all
//  explicit Non-Goals for this milestone in spec/typed-ir-design.md, not
//  oversights.
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
  hir_array_init,
  hir_cast,
  hir_block,
  hir_if,
  hir_match,
  hir_lambda,
  hir_tuple_index, ///< Static positional projection (pattern lowering only);
                   ///< used for both tuple slots and array/list/slice
                   ///< elements — see `hir_tuple_index`'s doc comment.
  hir_variant_payload, ///< Sum-type payload projection (pattern lowering only).
  hir_variant_init,    ///< Sum-type variant construction `@variant(args...)`.
  hir_container_len,   ///< A container's element count (`for`-loop lowering
                       ///< only).
  hir_generator_next,  ///< `g.next()` on a `generator[T]` value — no
                       ///< backing `func_decl`, same rationale as
                       ///< `hir_container_len`.
  // patterns (match arms only)
  hir_wildcard_pattern,
  hir_literal_pattern,
  hir_or_pattern,
  hir_tuple_pattern,
  hir_struct_pattern,
  hir_constructor_pattern,
  hir_range_pattern,
  hir_array_pattern,
  // statements
  hir_let,
  hir_let_else,
  hir_assign,
  hir_expr_stmt,
  hir_return,
  hir_yield, ///< `yield expr` inside a `generator def` — a suspension
             ///< point, not a block terminator (unlike `hir_return`,
             ///< lowering/codegen must keep walking `stmts` after it).
  hir_while,
  hir_while_let,
  hir_list_push,
  hir_contract_check, ///< A `pre`/`post`/`invariant` the checker could not
                      ///< discharge statically, reified as a runtime check.
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

  /// The checked type of the value this pattern is tested against, when
  /// lowering could resolve it — otherwise `k_unknown_type`.
  ///
  /// A backend needs the subject's type to test a pattern (which numeric
  /// kind to compare with, which variant tag a name denotes), and for most
  /// positions it can derive that structurally: a tuple pattern's elements
  /// come from the tuple type's own `args`, an array pattern's from its
  /// element type. Two positions it cannot, because the answer needs
  /// generic substitution that `runtime::layout.h` deliberately does not
  /// do (see its header comment): a sum-type variant's payload, and a
  /// struct's field. Lowering already resolves both — it must, to build
  /// the `hir_variant_payload`/`hir_field` place expression a binding
  /// reads through — so it records the type here rather than making each
  /// backend re-derive it. Only those positions set it today; everywhere
  /// else the structural derivation is already correct and cheaper.
  type_id subject_type = k_unknown_type;
};

/// One lowered parameter (of a function or a lambda) — resolved symbol plus
/// its checked type. Declared here, ahead of both "Expressions" (used by
/// `hir_lambda`) and "Items" (used by `hir_function`), since both need it.
struct hir_param {
  symbol_id symbol = k_invalid_symbol_id;
  std::string name;
  type_id type = k_unknown_type;
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
  /// A same-scope/same-module reference relies on `symbol` alone; a call
  /// target resolved against a real declaration (a same- or cross-module
  /// free function, or a `TargetType::method`-mangled associated function —
  /// see `lower_call`'s `resolved_callees` handling) also needs `name` for
  /// backend dispatch, since neither backend's function table is keyed by
  /// `symbol_id` (see compile.cpp/codegen.cpp's `functions_`).
  std::string name;
  /// Set only when this reference was resolved against a module- or
  /// type-qualified call site (`resolved_callee::owner_module`, `check.cpp`)
  /// — the module that declares the referenced function. `nullopt` for an
  /// ordinary local binding or same-module bare-name reference, which both
  /// backends already resolve correctly with no module qualifier.
  std::optional<std::string> owner_module;

  hir_local_ref(source_span s, type_id t, symbol_id sym, std::string n,
                std::optional<std::string> owner = std::nullopt)
      : hir_expr(hir_node_kind::hir_local_ref, s, t), symbol(sym),
        name(std::move(n)), owner_module(std::move(owner)) {}
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

/// Tuple literal `(a, b, c)`.
struct hir_tuple : hir_expr {
  ptr_vec<hir_expr> elements;

  hir_tuple(source_span s, type_id t, ptr_vec<hir_expr> elems)
      : hir_expr(hir_node_kind::hir_tuple, s, t), elements(std::move(elems)) {}
};

/// Array/list/slice literal — either the explicit-list form `[a, b, c]`
/// (`elements` populated, `fill_value`/`fill_count` null) or the fill form
/// `[val; count]` (`fill_value`/`fill_count` populated, `elements` empty),
/// mirroring `ast::array_expr`'s two-forms-in-one-node shape. `fill_count`
/// is null when the count itself was omitted or isn't a literal codegen
/// can read directly; the checker still resolved a length for the overall
/// literal's type in that case (see `infer_array`), so this only affects
/// how codegen reconstructs the repeat count, not what `type` says the
/// literal is.
struct hir_array_init : hir_expr {
  ptr_vec<hir_expr> elements;
  ptr<hir_expr> fill_value;
  ptr<hir_expr> fill_count;

  hir_array_init(source_span s, type_id t, ptr_vec<hir_expr> elems,
                 ptr<hir_expr> fill_val, ptr<hir_expr> fill_cnt)
      : hir_expr(hir_node_kind::hir_array_init, s, t),
        elements(std::move(elems)), fill_value(std::move(fill_val)),
        fill_count(std::move(fill_cnt)) {}
};

/// One field initializer inside a `hir_struct_init`.
struct hir_struct_init_field {
  std::string name;
  ptr<hir_expr> value;
};

/// Struct literal `{a: 1, b: 2}`. The explicit type head some surface
/// literals carry (`Point {...}`) isn't kept here — once the checker has
/// resolved which struct type this constructs, that's exactly this node's
/// own `type`, so the syntax that got it there has nothing left to add.
/// Shorthand fields (`{x}`) are already expanded to `{x: x}` by lowering —
/// see `struct_literal_field_types` — so every field here always has a
/// `value`.
struct hir_struct_init : hir_expr {
  std::vector<hir_struct_init_field> fields;

  hir_struct_init(source_span s, type_id t,
                  std::vector<hir_struct_init_field> f)
      : hir_expr(hir_node_kind::hir_struct_init, s, t), fields(std::move(f)) {}
};

/// Explicit cast `expr as Type`. The destination type isn't kept as its own
/// field — the checker already resolved it, and that resolution *is* this
/// node's own `type` (same reasoning as dropping `hir_struct_init`'s
/// explicit type head): nothing about the surface `as Type` syntax survives
/// lowering that isn't already captured there.
struct hir_cast : hir_expr {
  ptr<hir_expr> operand;

  hir_cast(source_span s, type_id t, ptr<hir_expr> op)
      : hir_expr(hir_node_kind::hir_cast, s, t), operand(std::move(op)) {}
};

/// Static positional projection `object.index`. There's no surface tuple-
/// index syntax to lower from — this exists purely so pattern lowering can
/// bind a name to a tuple pattern's `index`-th slot, or an array/list/slice
/// pattern's `index`-th element (see `hir_match_arm`), without inventing a
/// runtime-indexed `hir_index` for what's actually a fixed,
/// statically-known position. Codegen tells the two uses apart from
/// `object`'s type, the same way it already must for `hir_field`.
struct hir_tuple_index : hir_expr {
  ptr<hir_expr> object;
  size_t index = 0;

  hir_tuple_index(source_span s, type_id t, ptr<hir_expr> obj, size_t idx)
      : hir_expr(hir_node_kind::hir_tuple_index, s, t), object(std::move(obj)),
        index(idx) {}
};

/// Sum-type payload projection: `object`'s `index`-th payload slot for the
/// `variant_name` variant. Well-defined only where a `hir_constructor_pattern`
/// has already confirmed `object`'s runtime tag is `variant_name` — codegen's
/// responsibility to place this only where that's true, the same discipline
/// `hir_field`/`hir_tuple_index` already rely on for their own preconditions.
/// Like `hir_tuple_index`, this only ever comes from pattern lowering.
struct hir_variant_payload : hir_expr {
  ptr<hir_expr> object;
  std::string variant_name;
  size_t index = 0;

  hir_variant_payload(source_span s, type_id t, ptr<hir_expr> obj,
                      std::string variant, size_t idx)
      : hir_expr(hir_node_kind::hir_variant_payload, s, t),
        object(std::move(obj)), variant_name(std::move(variant)), index(idx) {}
};

/// Sum-type variant construction `@variant(args...)` (empty `args` for a
/// unit variant, e.g. `@none`) — the direct inverse of
/// `hir_constructor_pattern`: that node *tests* a value's runtime tag,
/// this one *creates* a value with a given tag. Covers both a
/// user-declared sum type's variants and the prelude's `some`/`none`/
/// `ok`/`err` (there's no separate node for those, matching how
/// `hir_constructor_pattern` already unifies them on the pattern side).
struct hir_variant_init : hir_expr {
  std::string variant_name;
  ptr_vec<hir_expr> args;

  hir_variant_init(source_span s, type_id t, std::string variant,
                   ptr_vec<hir_expr> a)
      : hir_expr(hir_node_kind::hir_variant_init, s, t),
        variant_name(std::move(variant)), args(std::move(a)) {}
};

/// A container's element count (`type` is always `usize`) — used only by
/// `for`-loop lowering over a `list`/`slice`/`slice_mut`/`str` (see
/// spec/iterator-protocol-design.md): a dedicated node rather than a
/// synthesized `.len()` method call, since there's no real call here to
/// model (no callee to resolve, no arguments) — just like
/// `hir_tuple_index`, there's no surface syntax this lowers *from*, only
/// a well-defined operation lowering *needs*. An `array[T, N]`'s element
/// count is statically known instead (`N`), so array iteration uses a
/// plain `hir_literal` and never constructs this node at all.
struct hir_container_len : hir_expr {
  ptr<hir_expr> object;

  hir_container_len(source_span s, type_id t, ptr<hir_expr> obj)
      : hir_expr(hir_node_kind::hir_container_len, s, t),
        object(std::move(obj)) {}
};

/// `g.next()` on a `generator[T]` value. Same rationale as
/// `hir_container_len`: `builtin_method_result` (`semantic::check.cpp`)
/// types this call with no backing `func_decl` to resolve, so it can't go
/// through the ordinary call-lowering path — a dedicated node with no
/// callee/args to compile instead. `type` is always `option[T]`.
struct hir_generator_next : hir_expr {
  ptr<hir_expr> object;

  hir_generator_next(source_span s, type_id t, ptr<hir_expr> obj)
      : hir_expr(hir_node_kind::hir_generator_next, s, t),
        object(std::move(obj)) {}
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

/// Lambda/closure `x => x + 1` or `(a, b) -> int => a + b`. Kira
/// monomorphizes closures (there is no separate closure type distinct from
/// the function-value type it's assigned/passed as — see
/// `infer_lambda`'s doc comment in check.cpp), so this is shaped exactly
/// like `hir_function` minus a name: same `hir_param` list, same
/// `return_type` plus `body`, and this node's own `type` is the lambda's
/// `fn(...)` type. Unlike a free function's parameters (always explicitly
/// annotated — Decision 1), a lambda parameter's type may come from
/// context (an expected `fn(...)` type at the lambda's use site) instead
/// of an annotation; lowering doesn't distinguish the two sources, only
/// requiring that a concrete type was resolved one way or another.
struct hir_lambda : hir_expr {
  std::vector<hir_param> params;
  type_id return_type = k_unknown_type;
  ptr<hir_block> body;

  hir_lambda(source_span s, type_id t, std::vector<hir_param> p, type_id ret,
             ptr<hir_block> b)
      : hir_expr(hir_node_kind::hir_lambda, s, t), params(std::move(p)),
        return_type(ret), body(std::move(b)) {}
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

/// Structural test on a tuple's elements, e.g. `(1, y)`. Always matches
/// structurally — the checker already validated arity — only `elements[i]`
/// itself can fail to match.
struct hir_tuple_pattern : hir_pattern {
  ptr_vec<hir_pattern> elements;

  hir_tuple_pattern(source_span s, ptr_vec<hir_pattern> elems)
      : hir_pattern(hir_node_kind::hir_tuple_pattern, s),
        elements(std::move(elems)) {}
};

/// Structural test on an array/list/slice's elements, e.g. `[a, 0, c]`.
/// Shaped exactly like `hir_tuple_pattern` (see `hir_node_kind`'s doc
/// comment on why: this grammar has no rest/slice-capture pattern syntax),
/// but kept as a distinct kind since codegen compiles it differently — a
/// `hir_tuple_pattern` never needs a length check, but this may, depending
/// on whether the subject's length is statically known.
struct hir_array_pattern : hir_pattern {
  ptr_vec<hir_pattern> elements;

  hir_array_pattern(source_span s, ptr_vec<hir_pattern> elems)
      : hir_pattern(hir_node_kind::hir_array_pattern, s),
        elements(std::move(elems)) {}
};

/// One named field inside a `hir_struct_pattern`.
struct hir_struct_pattern_field {
  std::string name;
  ptr<hir_pattern> pattern;
};

/// Struct destructuring, e.g. `{x: 1, y}`. Unlike a sum type, a struct has
/// exactly one shape, so this always matches structurally; only its
/// fields' own patterns can fail to match. A field the surface pattern
/// omitted (including via a trailing `..`) simply has no entry here.
struct hir_struct_pattern : hir_pattern {
  std::vector<hir_struct_pattern_field> fields;

  hir_struct_pattern(source_span s, std::vector<hir_struct_pattern_field> f)
      : hir_pattern(hir_node_kind::hir_struct_pattern, s),
        fields(std::move(f)) {}
};

/// `@variant(args...)` — matches if the subject's runtime tag is
/// `variant_name`; `args` destructure its payload slots (empty for a unit
/// variant, e.g. `none`). Also used to lower the `some(...)`/`ok(...)`/
/// `err(...)` sugar forms (`ast::option_pattern`/`ast::result_pattern`),
/// which are just single-payload constructor patterns against the
/// prelude's option/result sum types — there's no separate HIR pattern
/// kind for those.
struct hir_constructor_pattern : hir_pattern {
  std::string variant_name;
  ptr_vec<hir_pattern> args;

  hir_constructor_pattern(source_span s, std::string name,
                          ptr_vec<hir_pattern> a)
      : hir_pattern(hir_node_kind::hir_constructor_pattern, s),
        variant_name(std::move(name)), args(std::move(a)) {}
};

/// `start..end` / `start..=end` — matches if the subject falls in range.
/// Unlike the other structural patterns, its bounds are ordinary
/// expressions (they can be arbitrary constants, not just literals), so
/// they're lowered with `lower_expr` rather than recursed into as patterns.
struct hir_range_pattern : hir_pattern {
  ptr<hir_expr> start; ///< Null for an open-start range.
  ptr<hir_expr> end;   ///< Null for an open-end range.
  bool inclusive = false;

  hir_range_pattern(source_span s, ptr<hir_expr> lo, ptr<hir_expr> hi,
                    bool incl)
      : hir_pattern(hir_node_kind::hir_range_pattern, s), start(std::move(lo)),
        end(std::move(hi)), inclusive(incl) {}
};

// ==========================================================================
//  Statements
// ==========================================================================

/// `let name = expr`, or one binding a destructuring `let` pattern
/// introduces — a plain `let (a, b) = pair` lowers to one `hir_let` for a
/// synthetic subject (see `lower_pattern`'s doc comment) plus one more per
/// name the pattern binds, each reading a projection of that subject. Also
/// reused, synthetically, to represent a `match` arm's plain binding or
/// pattern alias (see `hir_match_arm`), and a `let ... else`'s bindings
/// (see `hir_let_else`), with `initializer` referencing the relevant
/// subject symbol instead of a source-level expression in both cases.
/// `is_mut` is true only for a `var name = expr` binding — every synthetic
/// or destructured use above is always an ordinary immutable `let`.
struct hir_let : hir_stmt {
  symbol_id symbol = k_invalid_symbol_id;
  std::string name; ///< Preserved for diagnostics/debugging only.
  ptr<hir_expr> initializer;
  bool is_mut = false;

  hir_let(source_span s, symbol_id sym, std::string n, ptr<hir_expr> init,
          bool mut = false)
      : hir_stmt(hir_node_kind::hir_let, s), symbol(sym), name(std::move(n)),
        initializer(std::move(init)), is_mut(mut) {}
};

/// `let pattern = expr else: block` — a fallible destructure. `initializer`
/// is evaluated exactly once into `subject_symbol`, then tested against
/// `pattern`: if it matches, every `hir_let` `pattern` bound is spliced
/// immediately after this node by lowering (exactly like an irrefutable
/// destructuring `let`'s own bindings — see `hir_let`), so they're in scope
/// for the rest of the enclosing block; if it doesn't match, `else_body`
/// runs instead. `else_body` must diverge (return, break, continue, or
/// otherwise never fall through) — there's no other way execution could
/// reach the statements after this one with `pattern`'s bindings left
/// uninitialized. This is a language-level requirement the checker is
/// responsible for enforcing, not something represented or verified here.
struct hir_let_else : hir_stmt {
  symbol_id subject_symbol = k_invalid_symbol_id;
  ptr<hir_expr> initializer;
  ptr<hir_pattern> pattern;
  ptr<hir_block> else_body;

  hir_let_else(source_span s, symbol_id sym, ptr<hir_expr> init,
               ptr<hir_pattern> pat, ptr<hir_block> else_b)
      : hir_stmt(hir_node_kind::hir_let_else, s), subject_symbol(sym),
        initializer(std::move(init)), pattern(std::move(pat)),
        else_body(std::move(else_b)) {}
};

/// `target op= value` (`op` is `Assign` for plain `=`); reuses `ast::assign_op`
/// rather than reinventing operator identity (Decision 7's rationale for
/// `ast::binary_op`/`ast::unary_op` applies equally here). `target` is
/// whatever lvalue-shaped expression the surface assignment used — a
/// `hir_local_ref`, `hir_field`, or `hir_index` — lowered the same way any
/// other expression is; nothing about assignment targets is special-cased
/// beyond that.
struct hir_assign : hir_stmt {
  ast::assign_op op;
  ptr<hir_expr> target;
  ptr<hir_expr> value;

  hir_assign(source_span s, ast::assign_op o, ptr<hir_expr> t, ptr<hir_expr> v)
      : hir_stmt(hir_node_kind::hir_assign, s), op(o), target(std::move(t)),
        value(std::move(v)) {}
};

/// `while condition: body` — the plain-condition form. See `hir_while_let`
/// for `while let pattern = expr: body`.
struct hir_while : hir_stmt {
  ptr<hir_expr> condition;
  ptr<hir_block> body;

  hir_while(source_span s, ptr<hir_expr> cond, ptr<hir_block> b)
      : hir_stmt(hir_node_kind::hir_while, s), condition(std::move(cond)),
        body(std::move(b)) {}
};

/// `while let pattern = expr: body`. Unlike a `for` loop's iterable
/// (evaluated once) or `hir_let_else`'s subject (also evaluated once),
/// `subject` here is conceptually re-evaluated *every iteration* — the
/// same lowered expression tree is executed repeatedly by the loop
/// itself, exactly the way `hir_while.condition` already is; there's no
/// separate mechanism for "re-lowering" it per pass. Each iteration:
/// evaluate `subject`, test it against `pattern`; if it matches, bind
/// `pattern`'s names (reading from `subject_symbol`, the same convention
/// `hir_match`/`hir_let_else` use) and run `body`, then loop again; if it
/// doesn't match, exit the loop. There is no `else` — falling out of the
/// loop on a failed match is simply what happens, matching how this
/// language has no `break`/`continue` to express it any other way.
/// `pattern` can be any pattern `lower_pattern` supports (not just a
/// simple binding), unlike a `for` loop's single plain loop variable.
struct hir_while_let : hir_stmt {
  ptr<hir_expr> subject;
  symbol_id subject_symbol = k_invalid_symbol_id;
  ptr<hir_pattern> pattern;
  ptr<hir_block> body;

  hir_while_let(source_span s, ptr<hir_expr> subj, symbol_id subj_sym,
                ptr<hir_pattern> pat, ptr<hir_block> b)
      : hir_stmt(hir_node_kind::hir_while_let, s), subject(std::move(subj)),
        subject_symbol(subj_sym), pattern(std::move(pat)), body(std::move(b)) {}
};

/// Appends `value` onto `target` (a `list[T]` place) — the counterpart of
/// `hir_container_len` for `for`-comprehension lowering (see
/// spec/iterator-protocol-design.md): a dedicated node rather than a
/// synthesized `.push()` method call, for the same reason
/// `hir_container_len` isn't a synthesized `.len()` call — there's no
/// callee to resolve, just a well-defined mutating operation lowering
/// needs to grow the comprehension's accumulator.
struct hir_list_push : hir_stmt {
  ptr<hir_expr> target;
  ptr<hir_expr> value;

  hir_list_push(source_span s, ptr<hir_expr> t, ptr<hir_expr> v)
      : hir_stmt(hir_node_kind::hir_list_push, s), target(std::move(t)),
        value(std::move(v)) {}
};

/// Which face of a contract a `hir_contract_check` enforces. Backends map
/// this straight onto a `bytecode::panic_reason`, so a failing check says
/// *whose* promise broke: a `pre` blames the caller, a `post` blames the
/// function, an `invariant` blames whoever last wrote the value.
enum class contract_kind : uint8_t {
  precondition,
  postcondition,
  invariant,
};

/// A contract condition (`pre`/`post`/`invariant`) that survived static
/// reasoning: `condition` is a `bool` expression, and reaching this statement
/// with it false panics. Everything the checker *proved* is gone by now
/// (`semantic::checked_types::elided_contracts`), so a check that exists here
/// is one whose truth genuinely depends on runtime values — the case the
/// spec's "enforces them at runtime otherwise" is about. Lowering places a
/// `precondition` at the function's entry, a `postcondition` at each of its
/// exits (with `return` bound to the value being returned), and an
/// `invariant` after each construction or field-assignment of the type that
/// declares it.
struct hir_contract_check : hir_stmt {
  ptr<hir_expr> condition;
  contract_kind kind;
  /// The contract's `, "message"`, if it was given one. Unused by the
  /// backends today — this tier's panics carry a fixed `panic_reason` and no
  /// string (see bytecode/panic.h) — but carried so that whichever tier grows
  /// real panic messages first doesn't have to re-derive it from the AST.
  std::string message;

  hir_contract_check(source_span s, ptr<hir_expr> c, contract_kind k,
                     std::string msg)
      : hir_stmt(hir_node_kind::hir_contract_check, s), condition(std::move(c)),
        kind(k), message(std::move(msg)) {}
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

/// `yield value` — a generator's suspension point. Unlike `hir_return`,
/// this does not terminate the enclosing block; lowering/codegen must keep
/// walking `stmts` after it. `value` is never null — the surface grammar
/// has no bare `yield` form.
struct hir_yield : hir_stmt {
  ptr<hir_expr> value;

  hir_yield(source_span s, ptr<hir_expr> v)
      : hir_stmt(hir_node_kind::hir_yield, s), value(std::move(v)) {}
};

// ==========================================================================
//  Items
// ==========================================================================

/// Lowered function. Decision 1 guarantees every parameter and the return
/// type are concrete by the time this node exists — a function with an
/// unannotated parameter fails lowering before a `hir_function` is built.
struct hir_function : hir_item {
  std::string name;
  std::vector<hir_param> params;
  type_id return_type = k_unknown_type;
  ptr<hir_block> body;
  /// Whether this function was declared `generator def`. When true,
  /// `return_type` is the concrete `generator[T]` value the function
  /// itself produces, and `item_type` is `T` — the type each `hir_yield`
  /// in `body` hands back to the caller. See `hir::live_across_yield` for
  /// the analysis backends need to lower a generator body.
  bool is_generator = false;
  type_id item_type = k_unknown_type;

  hir_function(source_span s, std::string n, std::vector<hir_param> p,
               type_id ret, ptr<hir_block> b, bool generator = false,
               type_id item = k_unknown_type)
      : hir_item(hir_node_kind::hir_function, s), name(std::move(n)),
        params(std::move(p)), return_type(ret), body(std::move(b)),
        is_generator(generator), item_type(item) {}
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
