#pragma once

#include <string>
#include <unordered_map>

#include "src/comptime/value.h"
#include "src/parser/ast.h"
#include "src/parser/diagnostic.h"

namespace kira::comptime {

/// A closed, tree-walking interpreter for the compile-time-evaluable subset
/// of Kira used by `static` declarations (`static let`, `static assert`,
/// `static if`).
///
/// Deliberately closed: no I/O, no filesystem, no environment access, and
/// no mutable state beyond its own binding environment — the language's
/// "two-effect model" (compile-time code may only emit diagnostics or emit
/// code, nothing else observable) means this evaluator must never grow an
/// escape hatch to native calls the way the tier-0 bytecode VM's
/// `op_call_intrinsic` does; that VM backs *runtime* execution and answers
/// a different question.
///
/// This first milestone supports only literals, arithmetic/comparison/
/// logical operators, unary negation, and references to previously
/// evaluated `static let` bindings — no function calls (`static def`),
/// `static for` iteration, structs, or quote/splice values yet. Those are
/// intentionally separate follow-up milestones; see the compile-time
/// evaluation design plan.
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

  /// Evaluates `expr` against the currently-bound `static let` globals.
  /// Returns `value::make_error()` (already diagnosed) if `expr` uses a
  /// construct this milestone doesn't support, or if evaluation hits a
  /// genuine runtime-style failure (e.g. division by zero).
  [[nodiscard]] auto evaluate(const ast::expr &expr) -> value;

  /// Registers a `static let` binding's already-evaluated value so later
  /// `static` expressions in the same session can reference it by name.
  /// Re-binding the same name overwrites the previous value — callers are
  /// responsible for cycle/duplicate-definition diagnostics elsewhere
  /// (mirrors `checker`'s existing `statics_in_progress_` guard).
  void bind_global(const std::string &name, value v);

private:
  [[nodiscard]] auto eval_binary(const ast::binary_expr &bin) -> value;
  [[nodiscard]] auto eval_unary(const ast::unary_expr &un) -> value;
  [[nodiscard]] auto eval_literal(const ast::literal_expr &lit) -> value;
  [[nodiscard]] auto eval_ident(const ast::ident_expr &ident) -> value;

  /// Reports an evaluation-time diagnostic and returns the error sentinel,
  /// so call sites can `return report(...)` in one expression.
  auto report(source_span span, std::string message) -> value;

  diagnostic_bag &diag_;
  file_id_type file_id_;
  std::unordered_map<std::string, value> globals_;
};

} // namespace kira::comptime
