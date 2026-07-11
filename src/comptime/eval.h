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

  [[nodiscard]] auto call_function(const ast::func_decl &fn,
                                   const std::string &name,
                                   std::vector<value> args, source_span span)
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
