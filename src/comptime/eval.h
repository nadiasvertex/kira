#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/comptime/value.h"
#include "src/parser/ast.h"
#include "src/parser/diagnostic.h"

namespace kira::comptime {

/// A closed, tree-walking interpreter for the compile-time-evaluable subset
/// of Kira used by `static` declarations (`static let`, `static assert`,
/// `static if`, `static for`, and `static def` calls).
///
/// Deliberately closed: no I/O, no filesystem, no environment access, and
/// no mutable state beyond its own binding environment — the language's
/// "two-effect model" (compile-time code may only emit diagnostics or emit
/// code, nothing else observable) means this evaluator must never grow an
/// escape hatch to native calls the way the tier-0 bytecode VM's
/// `op_call_intrinsic` does; that VM backs *runtime* execution and answers
/// a different question.
///
/// Supports literals, arithmetic/comparison/logical operators, unary
/// negation, `static let`/`static def` references (including forward
/// references, resolved lazily against a whole-session pending table),
/// list/struct literals, field/index access, pattern-destructuring `let`,
/// `if`/`return` control flow, and calls between `static def` functions.
/// Still no quote/splice values yet — that's a separate follow-up
/// milestone; see the compile-time evaluation design plan.
class evaluator {
public:
  evaluator(diagnostic_bag &diag, file_id_type file_id)
      : diag_(diag), file_id_(file_id) {}

  /// Updates which file newly reported diagnostics are attributed to. A
  /// single `evaluator` accumulates `static let` globals across an entire
  /// checking session (so cross-file references converge on one evaluated
  /// value regardless of which file's checking reaches them first — see
  /// the design plan's confluence requirement), but each file being
  /// checked needs its own diagnostics attributed to its own `file_id`.
  void set_file(file_id_type file_id) { file_id_ = file_id; }

  /// Evaluates `expr` against the currently-bound `static let` globals,
  /// resolving forward references to not-yet-checked `static let`s and
  /// `static def` calls lazily. Returns `value::make_error()` (already
  /// diagnosed) if `expr` uses a construct this evaluator doesn't support,
  /// or if evaluation hits a genuine runtime-style failure (e.g. division
  /// by zero, unbounded compile-time recursion).
  [[nodiscard]] auto evaluate(const ast::expr &expr) -> value;

  /// Registers a `static let` binding's already-evaluated value so later
  /// `static` expressions in the same session can reference it by name.
  /// Re-binding the same name overwrites the previous value — callers are
  /// responsible for cycle/duplicate-definition diagnostics elsewhere
  /// (mirrors `checker`'s existing `statics_in_progress_` guard).
  void bind_global(const std::string &name, value v);

  /// Whether `name` already has a memoized value in `globals_`, so callers
  /// can avoid re-evaluating (and re-diagnosing) an initializer that a
  /// forward reference already resolved lazily.
  [[nodiscard]] auto has_global(const std::string &name) const -> bool {
    return globals_.contains(name);
  }

  /// The memoized value bound for `name`, or `nullptr` if `name` has no
  /// binding yet — callers that need the actual value (rather than just
  /// "is it bound") after a `has_global` check or their own `bind_global`
  /// call, e.g. `checker::resolve_ident` embedding a scalar `static let`'s
  /// value directly into referencing code.
  [[nodiscard]] auto global_value(const std::string &name) const
      -> const value * {
    const auto it = globals_.find(name);
    return it != globals_.end() ? &it->second : nullptr;
  }

  /// Registers a top-level `static let` for lazy, order-independent
  /// evaluation: if some other compile-time expression references `name`
  /// before `checker` reaches this declaration in file order, the
  /// evaluator evaluates `initializer` itself (attributing diagnostics to
  /// `owner_file`) instead of reporting "not a compile-time constant yet".
  /// Does not evaluate eagerly — evaluation only happens on first
  /// reference, and the result is memoized via `bind_global`.
  void register_pending_static(std::string name, const ast::expr &initializer,
                               file_id_type owner_file);

  /// Registers a top-level `static def` function so calls to `name`
  /// elsewhere in compile-time code can find and invoke it.
  void register_pending_function(std::string name, const ast::func_decl &decl);

  /// Registers a top-level `type` declaration so `T.fields()`/
  /// `.field_count()`/`.name()` (`reflect.cpp`) can resolve `name` to its
  /// declaration independently of `checker`'s own module-scoped lookup —
  /// see `checker::register_comptime_globals`, the only caller.
  void register_pending_type(std::string name, const ast::type_decl &decl);

  /// Evaluates `iterable` in `static for` position to a `list` value.
  /// Supports list/array literals and integer ranges (`a..b`, `a..=b`);
  /// anything else reports "not yet supported" and returns an error.
  [[nodiscard]] auto evaluate_iterable(const ast::expr &iterable) -> value;

  /// Binds `pattern` against `v` into `scope`, reporting a diagnostic and
  /// returning `false` on a shape mismatch or unsupported pattern form.
  [[nodiscard]] auto bind_pattern(const ast::pattern &pattern, const value &v,
                                  std::unordered_map<std::string, value> &scope)
      -> bool;

  /// Pushes/pops one local-variable scope frame, used by callers (e.g.
  /// `static for` loop bodies) that need to bind loop variables around a
  /// sequence of statements evaluated through `evaluate_stmts`.
  void push_locals(std::unordered_map<std::string, value> scope);
  void pop_locals();

  /// Outcome of executing a statement sequence: whether a `return` was hit
  /// and, if so, its value.
  struct exec_result {
    bool returned = false;
    bool errored = false;
    value result = value::make_unit();
  };

  /// Executes a statement/body-node sequence against the current locals
  /// scope, short-circuiting on `return` or on the first unsupported/
  /// erroring construct.
  [[nodiscard]] auto
  evaluate_stmts(const std::vector<ast::ptr<ast::node>> &body) -> exec_result;

  /// Moves out every AST-builder-constructed fragment (see
  /// `synthesized_fragments_`'s doc comment) so its owner outlives this
  /// evaluator — `checker::take_checked_types` calls this and stores the
  /// result in `checked_types`, since `hir::lower` reads spliced fragments
  /// (via `checked_types::spliced_fragments`) after the `checker`/
  /// `evaluator` that produced them has already been destroyed.
  [[nodiscard]] auto take_synthesized_fragments() -> ast::ptr_vec<ast::node> {
    return std::move(synthesized_fragments_);
  }

private:
  [[nodiscard]] auto eval_binary(const ast::binary_expr &bin) -> value;
  [[nodiscard]] auto eval_unary(const ast::unary_expr &un) -> value;
  [[nodiscard]] auto eval_literal(const ast::literal_expr &lit) -> value;
  [[nodiscard]] auto eval_ident(const ast::ident_expr &ident) -> value;
  [[nodiscard]] auto eval_array(const ast::array_expr &arr) -> value;
  [[nodiscard]] auto eval_struct(const ast::struct_expr &st) -> value;
  [[nodiscard]] auto eval_field(const ast::field_expr &fld) -> value;
  [[nodiscard]] auto eval_index(const ast::index_expr &idx) -> value;
  [[nodiscard]] auto eval_call(const ast::call_expr &call) -> value;
  [[nodiscard]] auto eval_tuple(const ast::tuple_expr &tup) -> value;
  [[nodiscard]] auto eval_module_path(const ast::module_path_expr &path)
      -> value;
  [[nodiscard]] auto eval_quote(const ast::quote_expr &quote) -> value;

  /// Recognizes `expr.lit(...)`/`expr.ident(...)` — the AST-builder
  /// intrinsics that construct a new `expr` quote-value programmatically
  /// (as opposed to capturing existing syntax via a backtick quote). `expr`
  /// here is a contextual pseudo-namespace, not a real bound value (nothing
  /// ever binds the name `expr` itself); `field` must be a direct call
  /// whose callee is a `field_expr` with an `ident_expr` object literally
  /// spelled `expr`. Returns `nullopt` if `call` doesn't match this shape
  /// at all, so `eval_call` can fall through to its ordinary `static def`
  /// dispatch.
  [[nodiscard]] auto try_eval_expr_builder_call(const ast::call_expr &call)
      -> std::optional<value>;

  /// Deep-clones a fragment reached through a boxed `expr_fragment` value so
  /// it can become an owned child of a newly synthesized node (e.g.
  /// `expr.field`'s `object`) without aliasing a `unique_ptr` some other
  /// node/arena already owns. Deliberately narrow — only the node shapes
  /// reachable via the AST-builder intrinsics or a typical simple quoted
  /// expression (`ident_expr`, `field_expr`, `literal_expr`) are supported;
  /// returns `nullptr` for anything structurally richer, so the caller can
  /// report a clear "too complex" diagnostic instead of guessing.
  [[nodiscard]] auto clone_expr_fragment(const ast::node &node)
      -> ast::ptr<ast::expr>;

  /// Deep-clones one `interp_segment` (a literal-text run, or a value
  /// segment with its `expr` cloned via `clone_expr_fragment` and its
  /// format spec's scalar fields copied — a dynamic `{expr}` width/
  /// precision is dropped rather than cloned, since nothing this
  /// milestone's callers build ever sets one). Returns `nullopt` if the
  /// segment's value expression can't be cloned.
  [[nodiscard]] auto clone_interp_segment(const ast::interp_segment &segment)
      -> std::optional<ast::interp_segment>;

  /// Appends the segment(s) needed to represent `fragment` inside a larger
  /// `interpolated_string_expr` being assembled by `expr.interp_concat` —
  /// see the `.cpp` doc comment for the exact per-shape rules. Reports and
  /// returns `false` if `fragment`'s shape can't be represented.
  [[nodiscard]] auto
  append_interp_segments(const ast::node &fragment, source_span span,
                         std::vector<ast::interp_segment> &out) -> bool;

  /// Whether `node` (restricted to the structural shapes
  /// `materialize_quote` understands — see its own doc comment) contains a
  /// nested `~` splice anywhere beneath it, so `eval_quote` knows whether
  /// this fragment needs eager materialization at all. Best-effort/
  /// conservative: a node kind not specifically recognized here is assumed
  /// splice-free, matching this quote's pre-materialization behavior (leave
  /// it as literal syntax, resolved later at the eventual splice site)
  /// rather than risking a clone `materialize_quote` couldn't reconstruct.
  [[nodiscard]] auto quote_body_has_nested_splice(const ast::node &node)
      -> bool;

  /// Deep-clones `node`, resolving any nested `~` splice found along the
  /// way *now* — while the locals/globals active at the point this quote is
  /// being constructed are still live — instead of leaving it as literal,
  /// unresolved syntax to be re-evaluated later at whatever scope the
  /// eventual splice site happens to have (which, for a quote returned from
  /// a `static def` call, is *after* that call's own locals have already
  /// been popped — see the design plan's M7 addendum for the concrete
  /// failure this fixes: `static def make_adder(n) -> expr:
  /// `(x + ~(expr.lit(n)))`` couldn't resolve `n` at the eventual splice
  /// site without this). Only called when `quote_body_has_nested_splice`
  /// found something to resolve. Narrow, not a general AST clone: supports
  /// exactly the node shapes needed to quote a small generated `impl`
  /// (`impl_decl`, `func_decl`, `return_stmt`, `named_type`, `splice_type`,
  /// `splice_expr`, plus the expr leaf kinds `clone_expr_fragment` already
  /// handles). Returns `nullptr` (after reporting, for the cases that can
  /// fail) for anything else.
  [[nodiscard]] auto materialize_quote(const ast::node &node)
      -> ast::ptr<ast::node>;

  /// Recognizes `T.fields()`/`T.field_count()`/`T.name()` — compile-time
  /// reflection over a registered `type` declaration's own syntax (design
  /// plan section 4; implemented in `reflect.cpp`). `T` must be an
  /// `ident_expr` naming an entry in `pending_types_`. Returns `nullopt`
  /// if `call` doesn't match this shape at all (including "names a type,
  /// but not one of the three recognized calls"), so `eval_call` can fall
  /// through to its ordinary dispatch.
  [[nodiscard]] auto try_eval_type_reflection_call(const ast::call_expr &call)
      -> std::optional<value>;

  /// Recognizes `name[T](...)` — a compile-time generic call to a `static
  /// def` function with one type parameter (e.g. `derive_show[point]()`).
  /// `name` must be in `pending_functions_` with a non-empty `type_params`;
  /// `T` must resolve via `resolve_type_reference` (a real declared type, or
  /// — for a nested generic call forwarding its own type parameter — an
  /// already-bound `type_value` local). Returns `nullopt` if the shape
  /// doesn't match at all, so `eval_call` can fall through to its ordinary
  /// direct-call dispatch (which itself will reject an `index_expr` callee).
  [[nodiscard]] auto try_eval_comptime_generic_call(const ast::call_expr &call)
      -> std::optional<value>;

  /// Resolves `ident` to a registered `type` declaration for reflection/
  /// generic-call purposes: first checks whether `ident` names a local
  /// bound to a `type_value` (a generic parameter received by the enclosing
  /// `static def`, e.g. `T`), then falls back to `pending_types_`-by-
  /// literal-name (an ordinary top-level type name, e.g. `point`). Returns
  /// `nullptr` if neither resolves.
  [[nodiscard]] auto resolve_type_reference(const ast::ident_expr &ident)
      -> const ast::type_decl *;

  [[nodiscard]] auto
  call_function(const ast::func_decl &fn, const std::string &name,
                std::vector<value> args, source_span span,
                std::vector<std::pair<std::string, value>> type_args = {})
      -> value;

  [[nodiscard]] auto evaluate_stmt(const ast::node &node) -> exec_result;

  /// Looks up `name` in the local-scope stack (innermost first).
  [[nodiscard]] auto lookup_local(const std::string &name) -> const value *;

  /// Resolves a bare name (locals, then globals, then a pending `static
  /// let`/`static def`), reporting "not a compile-time constant" if none
  /// match. Shared by `eval_ident` and `eval_module_path`, since the parser
  /// can't distinguish `a.b` field access from a module-qualified path.
  [[nodiscard]] auto resolve_name(const std::string &name, source_span span)
      -> value;

  /// Resolves a not-yet-bound global by evaluating its pending `static
  /// let` initializer, with cycle detection.
  [[nodiscard]] auto resolve_pending_static(const std::string &name,
                                            source_span span) -> const value *;

  /// Reports an evaluation-time diagnostic and returns the error sentinel,
  /// so call sites can `return report(...)` in one expression.
  auto report(source_span span, std::string message) -> value;

  diagnostic_bag &diag_;
  file_id_type file_id_;
  std::unordered_map<std::string, value> globals_;

  struct pending_static {
    const ast::expr *initializer = nullptr;
    file_id_type owner_file{};
  };
  std::unordered_map<std::string, pending_static> pending_statics_;
  std::unordered_set<std::string> statics_in_progress_;
  std::unordered_map<std::string, const ast::func_decl *> pending_functions_;
  /// Every registered `type` declaration, by name — see
  /// `register_pending_type` and `try_eval_type_reflection_call`.
  std::unordered_map<std::string, const ast::type_decl *> pending_types_;

  std::vector<std::unordered_map<std::string, value>> locals_;
  int call_depth_ = 0;

  /// Owns every AST node synthesized by `try_eval_expr_builder_call`
  /// (`expr.lit(...)`/`expr.ident(...)`) — unlike a quoted `` `(...)` ``
  /// fragment (an ordinary child of the file's own AST, see `ast::
  /// quote_expr::parsed_body`'s doc comment), a programmatically
  /// constructed fragment has no natural file owner, so it needs its own
  /// session-lifetime storage. Stable-address: a `vector<unique_ptr<T>>`
  /// only moves the pointer on growth, never the pointee — mirrors
  /// `checker::synthesized_decls_`/`synthesized_types_` (`check.cpp`).
  ast::ptr_vec<ast::node> synthesized_fragments_;

  /// Monotonic counter feeding `comptime::rename_internal_bindings`'s
  /// synthetic names — session-lifetime (not reset per fragment) so two
  /// different quoted fragments, each with their own internal `let`, never
  /// mint the same fresh name and collide with *each other* once both are
  /// spliced into the same scope. See `eval_quote`'s doc comment.
  std::uint64_t hygiene_next_id_ = 0;

  /// Fragments already renamed by `eval_quote`, so evaluating the same
  /// `quote_expr` node more than once (e.g. one written inline inside a
  /// `static for` loop body, rather than behind a memoized `static let`)
  /// doesn't rename its bindings again on top of already-renamed ones.
  std::unordered_set<const ast::node *> hygiene_renamed_;
};

} // namespace kira::comptime
