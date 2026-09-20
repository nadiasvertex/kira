// Tests for the unifier (`spec/inference-rewrite.md` phase 2).
//
// The shapes below are the ones the 28 `unify_rigid` call sites in
// `check.cpp` between them hand to the old matcher — nominal applications,
// functions, tuples, references, arrays, higher-kinded applications, and
// value slots. That list *is* the test plan, per the phase description.
//
// Two of these assert behaviour the old matcher did **not** have, and they
// are the point of the phase: an array's length is unified along with its
// element type, and a constraint that cannot be decided yet is postponed
// rather than silently dropped.

#include <iostream>
#include <string>

#include "src/parser/source_location.h"
#include "src/semantic/infer/infer_ctxt.h"
#include "src/semantic/infer/unify.h"
#include "src/semantic/linear_poly.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using kira::semantic::linear_poly;
using kira::semantic::poly_add;
using kira::semantic::poly_constant;
using kira::semantic::poly_variable;
using kira::semantic::type_id;
using kira::semantic::type_table;
using kira::semantic::infer::infer_ctxt;
using kira::semantic::infer::k_no_cause;
using kira::semantic::infer::unifier;
using kira::semantic::infer::unify_failure;
using kira::testing::expect;

auto nowhere() -> kira::source_location { return {}; }

/// A store, a table and a unifier over both, since every test needs all three.
struct fixture {
  type_table table;
  infer_ctxt ctx{table};
  unifier engine{table, ctx};

  auto unify(type_id a, type_id b) { return engine.unify(a, b, k_no_cause); }
  auto var(std::string name) -> type_id {
    return ctx.fresh_type(std::move(name), nowhere());
  }
};

/// Structural descent through the shapes the call sites actually pass.
auto test_rigid_structures() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto str = f.table.builtin("str");

  // `list[?a] ~ list[int32]` solves `?a`.
  const auto a = f.var("a");
  expect(f.unify(f.table.builtin_generic("list", {a}),
                 f.table.builtin_generic("list", {int32}))
             .has_value(),
         "expected a nominal application to unify");
  expect(f.ctx.zonk(a) == int32, "expected the argument to be solved");

  // Functions unify parameters and result.
  const auto b = f.var("b");
  const auto c = f.var("c");
  expect(
      f.unify(f.table.fn_of({b}, c), f.table.fn_of({int32}, str)).has_value(),
      "expected functions to unify");
  expect(f.ctx.zonk(b) == int32 && f.ctx.zonk(c) == str,
         "expected both sides of the arrow to solve");

  // Tuples and references, including through a reference.
  const auto d = f.var("d");
  expect(f.unify(f.table.tuple_of({f.table.ref_to(d, true), int32}),
                 f.table.tuple_of({f.table.ref_to(str, true), int32}))
             .has_value(),
         "expected tuples of references to unify");
  expect(f.ctx.zonk(d) == str, "expected the referent to solve");
}

/// Two different constructors are a refusal, and the error names both the
/// outermost pair the caller asked about and the innermost pair that clashed.
auto test_mismatch_reports_both_pairs() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto str = f.table.builtin("str");
  const auto expected =
      f.table.fn_of({int32}, f.table.builtin_generic("list", {str}));
  const auto found =
      f.table.fn_of({int32}, f.table.builtin_generic("list", {int32}));

  const auto result = f.unify(expected, found);
  expect(!result.has_value(), "expected the mismatch to be refused");
  expect(result.error().failure == unify_failure::mismatch,
         "expected a mismatch failure");
  expect(result.error().expected == expected && result.error().found == found,
         "expected the outer pair to be what the caller asked about");
  expect(result.error().expected_part == str &&
             result.error().found_part == int32,
         "expected the inner pair to be where it actually went wrong");

  // A different nominal constructor at the same arity is also a refusal.
  auto g = fixture{};
  const auto refused =
      g.unify(g.table.builtin_generic("list", {g.table.builtin("int32")}),
              g.table.builtin_generic("option", {g.table.builtin("int32")}));
  expect(!refused.has_value(), "expected different constructors to refuse");
}

/// Mutability is part of a reference's identity, so `&mut T` and `&T` are
/// not the same type. (Whether one *coerces* to the other is a coercion
/// question, asked at the call site, not here.)
auto test_mutability_is_not_ignored() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto result =
      f.unify(f.table.ref_to(int32, true), f.table.ref_to(int32, false));
  expect(!result.has_value(), "expected mutability to matter");
  expect(result.error().failure == unify_failure::mutability,
         "expected a mutability failure");
}

/// The pattern fragment: a flex head matches the outermost nominal
/// constructor, and the arguments recurse positionally.
auto test_pattern_fragment() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");

  const auto head = f.ctx.fresh_ctor(1, "F", nowhere());
  const auto arg = f.var("A");
  const auto application = f.table.param_app(head, {arg});

  expect(f.unify(application, f.table.builtin_generic("option", {int32}))
             .has_value(),
         "expected `F[A]` to match `option[int32]`");
  expect(f.ctx.zonk(head) == f.table.ctor_ref("option", "", nullptr, 1),
         "expected the head to solve to the `option` constructor");
  expect(f.ctx.zonk(arg) == int32, "expected the argument to solve");
  expect(f.ctx.zonk(application) == f.table.builtin_generic("option", {int32}),
         "expected the application to collapse to the ordinary type");
}

/// Outside the fragment, the two cases are genuinely different: a flex head
/// against something that can never be an application is a real failure,
/// while two flex heads are merely undecided and must wait.
auto test_outside_the_fragment() -> void {
  auto f = fixture{};
  const auto head = f.ctx.fresh_ctor(1, "F", nowhere());
  const auto application = f.table.param_app(head, {f.var("A")});
  const auto result = f.unify(application, f.table.builtin("int32"));
  expect(!result.has_value(), "expected a non-application to refuse");
  expect(result.error().failure == unify_failure::out_of_scope,
         "expected an out-of-scope failure");

  auto g = fixture{};
  const auto left =
      g.table.param_app(g.ctx.fresh_ctor(1, "F", nowhere()), {g.var("A")});
  const auto right =
      g.table.param_app(g.ctx.fresh_ctor(1, "G", nowhere()), {g.var("B")});
  expect(g.unify(left, right).has_value(),
         "expected two flexible heads to postpone rather than fail");
  expect(g.engine.deferred().size() == 1,
         "expected the undecided constraint to be kept");
}

/// An array's length is unified along with its element type. The old
/// `unify_rigid` descended into the element only, which is precisely why an
/// `n` in `array[T, n]` came back looking unsolved and needed a second
/// solver beside it.
auto test_array_length_participates() -> void {
  auto f = fixture{};
  const auto usize = f.table.usize_type();
  const auto element = f.var("T");
  const auto length = f.ctx.fresh_value(usize, "n", nowhere());
  const auto pattern = f.table.array_of(element, std::nullopt, length);
  const auto concrete = f.table.array_of(f.table.builtin("int32"), 4,
                                         f.table.const_value(usize, 4));

  expect(f.unify(pattern, concrete).has_value(), "expected arrays to unify");
  expect(f.ctx.zonk(element) == f.table.builtin("int32"),
         "expected the element type to solve");
  expect(f.ctx.zonk(length) == f.table.const_value(usize, 4),
         "expected the length to solve — this is what the old matcher skipped");
}

/// Value slots: equal agrees, provably unsolvable refuses, still-open waits.
auto test_value_slots() -> void {
  auto f = fixture{};
  const auto usize = f.table.usize_type();
  const auto n_plus_one = f.table.symbolic_value(
      usize, poly_add(poly_variable("n"), poly_constant(1)));

  // `n + 1 ~ 0` has no solution over an unsigned domain: this is what
  // rejects `head` on an empty vector.
  const auto impossible =
      f.unify(f.table.builtin_generic("vec", {n_plus_one}),
              f.table.builtin_generic("vec", {f.table.const_value(usize, 0)}));
  expect(!impossible.has_value(), "expected an unsolvable equation to refuse");
  expect(impossible.error().failure == unify_failure::value,
         "expected a value failure");

  // `n + 1 ~ 3` is *solved*, not merely accepted: `n := 2`, and every type
  // mentioning `n` sees it. This is the gap ch. 33 records — "the compiler
  // does not solve for `n` and propagate it" — closed.
  auto g = fixture{};
  const auto g_usize = g.table.usize_type();
  const auto g_pattern = g.table.symbolic_value(
      g_usize, poly_add(poly_variable("n"), poly_constant(1)));
  expect(g.unify(g_pattern, g.table.const_value(g_usize, 3)).has_value(),
         "expected a solvable equation to be accepted");
  expect(g.engine.deferred().empty(),
         "expected the equation to be solved rather than deferred");
  const auto n = g.ctx.value_param_named("n");
  expect(n.has_value(), "expected `n` to have entered the store");
  expect(g.ctx.zonk(*n) == g.table.const_value(g_usize, 2),
         "expected `n` to have been solved to 2");
  expect(g.ctx.zonk(g_pattern) == g.table.const_value(g_usize, 3),
         "expected `n + 1` to zonk to the interned `3`");

  // Variants are identities, not quantities.
  auto h = fixture{};
  const auto h_usize = h.table.usize_type();
  expect(
      !h.unify(h.table.const_value(h_usize, 1), h.table.const_value(h_usize, 2))
           .has_value(),
      "expected two different constants to refuse");
}

/// A deferred constraint becomes decidable once the variable it was waiting
/// on is solved, and `retry_deferred` reports the progress.
auto test_deferred_constraints_retry() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");

  const auto left_head = f.ctx.fresh_ctor(1, "F", nowhere());
  const auto right_head = f.ctx.fresh_ctor(1, "G", nowhere());
  const auto left = f.table.param_app(left_head, {f.var("A")});
  const auto right = f.table.param_app(right_head, {int32});

  expect(f.unify(left, right).has_value(), "expected the pair to postpone");
  expect(f.engine.deferred().size() == 1, "expected one deferred constraint");

  auto no_progress = f.engine.retry_deferred();
  expect(no_progress.has_value() && !*no_progress,
         "expected no progress while both heads are unsolved");
  expect(f.engine.deferred().size() == 1, "expected it to stay deferred");

  expect(f.ctx
             .bind(right_head, f.table.ctor_ref("option", "", nullptr, 1),
                   k_no_cause)
             .has_value(),
         "expected the head to solve");
  auto progress = f.engine.retry_deferred();
  expect(progress.has_value() && *progress,
         "expected the retry to make progress once one head is known");
  expect(f.engine.deferred().empty(), "expected the constraint to be resolved");
  expect(f.ctx.zonk(left) == f.table.builtin_generic("option", {int32}),
         "expected both sides to have become the same type");
}

/// `unknown` and `error` unify with everything, so a single gap in knowledge
/// never cascades into unrelated errors — the rule that outranks every other
/// one here.
auto test_absent_types_never_fail() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  expect(f.unify(kira::semantic::k_unknown_type, int32).has_value(),
         "expected `unknown` to unify with anything");
  expect(
      f.unify(f.table.builtin_generic("list", {kira::semantic::k_error_type}),
              f.table.builtin_generic("list", {int32}))
          .has_value(),
      "expected `error` to unify with anything");
}

} // namespace

auto main() -> int {
  test_rigid_structures();
  test_mismatch_reports_both_pairs();
  test_mutability_is_not_ignored();
  test_pattern_fragment();
  test_outside_the_fragment();
  test_array_length_participates();
  test_value_slots();
  test_deferred_constraints_retry();
  test_absent_types_never_fail();
  std::cout << "unify_test passed\n";
  return 0;
}
