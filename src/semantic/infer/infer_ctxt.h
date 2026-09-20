#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/parser/source_location.h"
#include "src/semantic/types.h"

namespace kira::semantic::infer {

// ==========================================================================
//  The metavariable store
//
//  Phase 1 of `spec/inference-rewrite.md`: one union-find over `type_id`
//  carrying every unknown the checker can have, whatever its sort, plus the
//  substitution (`zonk`) and the provenance chain (`cause`) the later phases
//  hang off it.
//
//  Nothing in the checker calls this yet. It is deliberately standalone so
//  it can be tested as an algorithm before phase 6 puts it on the critical
//  path.
// ==========================================================================

/// What a metavariable ranges over.
///
/// Three sorts rather than three stores, because `type_table` already
/// represents all three as `type_id`s (`types.h:41`): a type, a *constructor*
/// of a declared arity (`ctor_ref_kind`, the `option` in `monad[option]`),
/// and a compile-time *value* (`const_value_kind` and friends, the `n` in
/// `vec[T, n]`). One union-find therefore covers generic parameters, const
/// generics, and higher-kinded heads with no second mechanism — which is the
/// single coherent unification mechanism the rewrite exists to get.
enum class meta_sort : uint8_t {
  type_sort,  ///< An ordinary type: `T` in `def id[T](x: T) -> T`.
  ctor_sort,  ///< A constructor of `arity` arguments: `F` in `functor[F[_]]`.
  value_sort, ///< A compile-time value: `n` in `vec[T, n]`.
};

/// The sort's name as it appears in a diagnostic.
[[nodiscard]] auto sort_name(meta_sort sort) -> std::string_view;

/// Index into an `infer_ctxt`'s cause arena; `k_no_cause` means "no
/// recorded context", which is legal and means a diagnostic simply has no
/// parent frame to print.
using cause_id = uint32_t;

/// The absent cause. Always index 0 of the arena, so a default-constructed
/// `cause_id` is safe to look up.
inline constexpr cause_id k_no_cause = 0;

/// Why a constraint exists — the raw material for both the "While
/// processing..." context stack and the blame pass of phase 5.
///
/// Recorded when the constraint is *created*, never reconstructed at failure
/// time. That ordering is the whole point: by the time unification fails, the
/// syntactic context that explains the constraint is gone, and a compiler that
/// waits until then can only say "expected X, found Y". Idris 2 recorded this
/// from its first release; Rust retrofitted `ObligationCause` and still pays
/// for it.
struct cause {
  /// Where in source the constraint came from.
  source_location where;
  /// What the checker was doing: "argument 2 of `push`", "the `else` branch",
  /// "the declared return type". A sentence fragment, lowercase, no period —
  /// it is printed inside a larger sentence.
  std::string reason;
  /// How the required side should be described to a user, when the type
  /// spelling alone would not explain it ("the element type of `xs`").
  std::string expected_desc;
  /// The same for the offered side.
  std::string found_desc;
  /// The enclosing constraint, forming the context stack. `k_no_cause` at the
  /// outermost frame.
  cause_id parent = k_no_cause;
};

/// One unknown in the store.
struct meta_var {
  meta_sort sort = meta_sort::type_sort;
  /// For `ctor_sort`, how many arguments the constructor takes. Kinds are
  /// arities and nothing more (ch. 37), so this is the entire kind. 0 for
  /// the other sorts.
  size_t arity = 0;
  /// For `value_sort`, the scalar type the value inhabits (`usize` for an
  /// array length). `k_unknown_type` for the other sorts.
  type_id underlying = k_unknown_type;
  /// What this variable stands for, for diagnostics: "`T` of `push`".
  std::string origin;
  /// Where it was introduced.
  source_location where;
};

/// Why a `bind` was refused.
enum class bind_failure : uint8_t {
  not_a_variable, ///< The left side was not an unsolved metavariable.
  sort_mismatch,  ///< A type where a value was wanted, or similar.
  arity_mismatch, ///< A constructor of the wrong kind: `F[_]` got `result`.
  occurs_check,   ///< `?a := list[?a]` — an infinite type.
};

/// A refused `bind`, carrying enough to report it without re-deriving it.
struct bind_error {
  bind_failure failure = bind_failure::not_a_variable;
  type_id var = k_unknown_type;
  type_id value = k_unknown_type;
  cause_id why = k_no_cause;
  /// A rendered explanation of this specific refusal.
  std::string detail;
};

/// The metavariable store: fresh variables, the substitution, and the causes.
///
/// Deliberately *not* a solver. It knows how to record that a variable stands
/// for a type and how to push that solution through the `type_table`; it does
/// not know how to decompose `list[?a] ~ list[int32]`. That is phase 2, and
/// keeping the split means the store can be tested for the invariant that
/// actually matters — canonicity — without a unifier in the way.
class infer_ctxt {
public:
  /// Borrows the session's `type_table`; every solution is substituted back
  /// through it, so the two have the same lifetime.
  explicit infer_ctxt(type_table &table);

  /// Mints an unsolved variable of each sort. The returned `type_id` is a
  /// fresh `type_var_kind` id from the table, so an unsolved variable that
  /// escapes into a checked type behaves exactly like `unknown` everywhere
  /// (`is_unknown`, `compatible`, `display`) rather than crashing a consumer
  /// that never heard of inference.
  [[nodiscard]] auto fresh_type(std::string origin, source_location where)
      -> type_id;
  [[nodiscard]] auto fresh_ctor(size_t arity, std::string origin,
                                source_location where) -> type_id;
  [[nodiscard]] auto fresh_value(type_id underlying, std::string origin,
                                 source_location where) -> type_id;

  /// Whether `id` is one of this store's variables (solved or not).
  [[nodiscard]] auto is_meta(type_id id) const -> bool;
  /// The variable record behind `id`, or `nullptr` when `id` is not one.
  [[nodiscard]] auto meta(type_id id) const -> const meta_var *;

  /// The representative of `id`'s equivalence class, with path compression.
  /// Returns `id` unchanged when it is not a variable.
  [[nodiscard]] auto find(type_id id) const -> type_id;

  /// `find`, then one step through the solution if there is one. The result
  /// is either a non-variable type or an unsolved representative — it is
  /// *not* recursively substituted, which is what `zonk` is for. This is the
  /// operation a unifier wants at the top of every step.
  [[nodiscard]] auto shallow_resolve(type_id id) const -> type_id;

  /// The solution of `id`'s class, if it has one.
  [[nodiscard]] auto solution(type_id id) const -> std::optional<type_id>;

  /// Records that `var` stands for `value`.
  ///
  /// `var` must be an unsolved variable; `value` may be anything, including
  /// another variable (in which case the two classes are merged). Refuses on
  /// a sort or arity mismatch and on the occurs check, returning the refusal
  /// rather than reporting it — who reports, and how, is phase 5's business.
  auto bind(type_id var, type_id value, cause_id why)
      -> std::expected<void, bind_error>;

  /// Applies the whole substitution to `id`, recursively.
  ///
  /// **Every solution is substituted back through the `type_table`'s
  /// constructors, never by patching an entry in place.** That is the
  /// load-bearing invariant of the entire design: Kira's dependent fragment
  /// is canonical by construction, so unification never needs a definitional
  /// equality check — but only for as long as two equal types remain the
  /// same `type_id`. Rebuilding through the constructors re-interns, so
  /// zonking `F[A]` under `F := option, A := int32` yields the very id that
  /// `option[int32]` written in source has. `infer_ctxt_test` asserts exactly
  /// that.
  ///
  /// Memoized; the memo is dropped whenever a `bind` succeeds.
  [[nodiscard]] auto zonk(type_id id) -> type_id;

  /// Records a cause and returns its handle.
  auto add_cause(cause why) -> cause_id;
  /// The cause behind `id`; the empty cause for `k_no_cause`.
  [[nodiscard]] auto cause_at(cause_id id) const -> const cause &;
  /// `id` and its parents, innermost first — the context stack to print.
  [[nodiscard]] auto cause_chain(cause_id id) const -> std::vector<cause_id>;

  /// How many variables have been minted.
  [[nodiscard]] auto meta_count() const -> size_t;
  /// Every variable still without a solution, in mint order. What a caller
  /// reports as "cannot infer" once the obligation queue has stalled.
  [[nodiscard]] auto unsolved() const -> std::vector<type_id>;

private:
  /// Whether `value` may stand for a variable of `sort`/`arity`, and why not
  /// if it may not.
  [[nodiscard]] auto check_sort(const meta_var &var, type_id value) const
      -> std::optional<bind_error>;
  /// Whether `root` occurs anywhere inside `value` under the current
  /// substitution.
  [[nodiscard]] auto occurs(type_id root, type_id value) const -> bool;
  /// Rebuilds `id` with already-zonked children, through the table's
  /// constructors.
  [[nodiscard]] auto rebuild(type_id id) -> type_id;
  /// Applies a `ctor_ref_kind` constructor to `args`, yielding the ordinary
  /// interned application (`option` + `[int32]` is the `option[int32]` id).
  [[nodiscard]] auto apply_ctor(type_id ctor, std::vector<type_id> args)
      -> type_id;

  type_table *table_;
  /// Variable records, keyed by the `type_id` the table minted.
  std::unordered_map<type_id, meta_var> vars_;
  /// Mint order, so `unsolved` is deterministic.
  std::vector<type_id> mint_order_;
  /// Union-find parent links between variables. Mutable so `find` can
  /// compress a path without being a mutation in the caller's eyes.
  mutable std::unordered_map<type_id, type_id> parent_;
  /// Solutions, keyed by class representative.
  std::unordered_map<type_id, type_id> solution_;
  /// `zonk` memo, dropped on every successful `bind`.
  std::unordered_map<type_id, type_id> zonk_memo_;
  std::vector<cause> causes_;
};

} // namespace kira::semantic::infer
