// Tests for the metavariable store (`spec/inference-rewrite.md` phase 1).
//
// The assertion that earns this file its place is `test_zonk_is_canonical`:
// a zonked type must be *id-equal* to the same type written directly. The
// whole engine rests on that — Kira's dependent fragment is canonical by
// construction, which is why unification never needs a definitional-equality
// check, and that property survives only if every solution is substituted
// back through the `type_table`'s constructors rather than patched in place.

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "src/semantic/infer/infer_ctxt.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using kira::semantic::type_id;
using kira::semantic::type_table;
using kira::semantic::infer::bind_failure;
using kira::semantic::infer::cause;
using kira::semantic::infer::infer_ctxt;
using kira::semantic::infer::k_no_cause;
using kira::semantic::infer::meta_sort;
using kira::testing::expect;

auto nowhere() -> kira::source_location { return {}; }

/// Mints two independent variables and merges them; a solution recorded for
/// either must be visible from both.
auto test_union_find_merges_classes() -> void {
  auto table = type_table{};
  auto ctx = infer_ctxt(table);

  const auto a = ctx.fresh_type("a", nowhere());
  const auto b = ctx.fresh_type("b", nowhere());
  expect(a != b, "expected fresh variables to be distinct ids");
  expect(ctx.is_meta(a) && ctx.is_meta(b), "expected both to be variables");
  expect(ctx.find(a) != ctx.find(b), "expected distinct classes before a bind");

  expect(ctx.bind(a, b, k_no_cause).has_value(), "expected the merge to hold");
  expect(ctx.find(a) == ctx.find(b), "expected one class after the merge");

  const auto int32 = table.builtin("int32");
  expect(ctx.bind(b, int32, k_no_cause).has_value(),
         "expected the solution to hold");
  expect(ctx.zonk(a) == int32, "expected the solution to reach the merged var");
  expect(ctx.unsolved().empty(), "expected no variable left open");
}

/// `?a := list[?a]` is an infinite type and must be refused rather than
/// looping forever in `zonk`.
auto test_occurs_check() -> void {
  auto table = type_table{};
  auto ctx = infer_ctxt(table);

  const auto a = ctx.fresh_type("a", nowhere());
  const auto list_a = table.builtin_generic("list", {a});
  const auto result = ctx.bind(a, list_a, k_no_cause);
  expect(!result.has_value(), "expected the occurs check to refuse the bind");
  expect(result.error().failure == bind_failure::occurs_check,
         "expected an occurs-check failure");
}

/// Sorts are checked on binding, so a value can never stand for a type.
auto test_sorts_are_enforced() -> void {
  auto table = type_table{};
  auto ctx = infer_ctxt(table);

  const auto usize = table.usize_type();
  const auto three = table.const_value(usize, 3);

  const auto t = ctx.fresh_type("T", nowhere());
  const auto to_value = ctx.bind(t, three, k_no_cause);
  expect(!to_value.has_value(), "expected a value not to solve a type var");
  expect(to_value.error().failure == bind_failure::sort_mismatch,
         "expected a sort mismatch");

  const auto n = ctx.fresh_value(usize, "n", nowhere());
  const auto to_type = ctx.bind(n, table.builtin("int32"), k_no_cause);
  expect(!to_type.has_value(), "expected a type not to solve a value var");
  expect(to_type.error().failure == bind_failure::sort_mismatch,
         "expected a sort mismatch");
  expect(ctx.bind(n, three, k_no_cause).has_value(),
         "expected a value to solve a value var");

  // Kinds are arities: a two-argument constructor cannot stand for `F[_]`.
  const auto f = ctx.fresh_ctor(1, "F", nowhere());
  const auto result_ctor = table.ctor_ref("result", "", nullptr, 2);
  const auto wrong_arity = ctx.bind(f, result_ctor, k_no_cause);
  expect(!wrong_arity.has_value(), "expected the arity check to refuse");
  expect(wrong_arity.error().failure == bind_failure::arity_mismatch,
         "expected an arity mismatch");
  expect(ctx.meta(f) != nullptr && ctx.meta(f)->sort == meta_sort::ctor_sort,
         "expected the constructor variable to keep its sort");
}

/// The load-bearing invariant. Zonking a solved structure must produce the
/// same `type_id` the structure written directly interns to — including the
/// higher-kinded case, where solving the head of an `F[A]` has to collapse
/// the application into the ordinary `option[int32]` rather than leaving a
/// `param_app` with a solved head behind.
auto test_zonk_is_canonical() -> void {
  auto table = type_table{};
  auto ctx = infer_ctxt(table);
  const auto int32 = table.builtin("int32");

  // Nested: `list[?a]` with `?a := int32`.
  const auto a = ctx.fresh_type("a", nowhere());
  const auto list_a = table.builtin_generic("list", {a});
  expect(ctx.zonk(list_a) == list_a,
         "expected an unsolved structure to zonk to itself");
  expect(ctx.bind(a, int32, k_no_cause).has_value(), "expected the bind");
  expect(ctx.zonk(list_a) == table.builtin_generic("list", {int32}),
         "expected `list[?a]` to zonk to the interned `list[int32]`");

  // Functions, tuples and references, one level down.
  const auto b = ctx.fresh_type("b", nowhere());
  const auto shape =
      table.fn_of({table.ref_to(b, true), table.tuple_of({b, int32})}, b);
  expect(ctx.bind(b, table.builtin("str"), k_no_cause).has_value(),
         "expected the bind");
  const auto str = table.builtin("str");
  expect(ctx.zonk(shape) ==
             table.fn_of(
                 {table.ref_to(str, true), table.tuple_of({str, int32})}, str),
         "expected a composite to zonk to the id written directly");

  // Higher-kinded: `F[A]` with `F := option`, `A := int32`.
  const auto f = ctx.fresh_ctor(1, "F", nowhere());
  const auto arg = ctx.fresh_type("A", nowhere());
  const auto application = table.param_app(f, {arg});
  expect(ctx.bind(f, table.ctor_ref("option", "", nullptr, 1), k_no_cause)
             .has_value(),
         "expected the head to solve");
  expect(ctx.bind(arg, int32, k_no_cause).has_value(),
         "expected the argument to solve");
  expect(ctx.zonk(application) == table.builtin_generic("option", {int32}),
         "expected `F[A]` to zonk to the interned `option[int32]`");
}

/// The memo must not outlive the substitution it was computed under.
auto test_zonk_memo_is_invalidated() -> void {
  auto table = type_table{};
  auto ctx = infer_ctxt(table);

  const auto a = ctx.fresh_type("a", nowhere());
  const auto list_a = table.builtin_generic("list", {a});
  const auto before = ctx.zonk(list_a);
  expect(before == list_a, "expected the unsolved form first");

  const auto int32 = table.builtin("int32");
  expect(ctx.bind(a, int32, k_no_cause).has_value(), "expected the bind");
  expect(ctx.zonk(list_a) == table.builtin_generic("list", {int32}),
         "expected the memo to have been dropped by the bind");
}

/// Causes form a stack, innermost first, and `k_no_cause` is always safe to
/// look up.
auto test_cause_chain() -> void {
  auto table = type_table{};
  auto ctx = infer_ctxt(table);

  expect(ctx.cause_at(k_no_cause).reason.empty(),
         "expected the absent cause to be empty rather than out of range");

  const auto outer =
      ctx.add_cause(cause{.reason = "checking the body of `main`"});
  const auto inner =
      ctx.add_cause(cause{.reason = "argument 2 of `push`", .parent = outer});

  const auto chain = ctx.cause_chain(inner);
  expect(chain.size() == 2, "expected two frames");
  expect(ctx.cause_at(chain[0]).reason == "argument 2 of `push`",
         "expected the innermost frame first");
  expect(ctx.cause_at(chain[1]).reason == "checking the body of `main`",
         "expected the outer frame second");
  expect(ctx.cause_chain(k_no_cause).empty(), "expected no frames at the root");
}

/// Unsolved variables come back in mint order, so a stalled-queue diagnostic
/// reports them in the order the user wrote them.
auto test_unsolved_is_ordered() -> void {
  auto table = type_table{};
  auto ctx = infer_ctxt(table);

  const auto a = ctx.fresh_type("a", nowhere());
  const auto b = ctx.fresh_type("b", nowhere());
  const auto c = ctx.fresh_type("c", nowhere());
  expect(ctx.meta_count() == 3, "expected three variables");
  expect(ctx.bind(b, table.builtin("int32"), k_no_cause).has_value(),
         "expected the bind");

  const auto open = ctx.unsolved();
  expect(open.size() == 2, "expected two variables still open");
  expect(open[0] == a && open[1] == c, "expected mint order");
}

} // namespace

auto main() -> int {
  test_union_find_merges_classes();
  test_occurs_check();
  test_sorts_are_enforced();
  test_zonk_is_canonical();
  test_zonk_memo_is_invalidated();
  test_cause_chain();
  test_unsolved_is_ordered();
  std::cout << "infer_ctxt_test passed\n";
  return 0;
}
