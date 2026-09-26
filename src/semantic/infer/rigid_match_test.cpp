// Tests for `rigid_match` (`spec/inference-rewrite.md` phase 7).
//
// Two things are under test. First, that matching a declared pattern against
// a concrete type through the one unifier produces the bindings the 28
// `unify_rigid` sites expect — otherwise the migration is not a migration.
// Second, and more important, that each of `unify_rigid`'s four defects is
// gone: array lengths solve, mutability is read, a mismatch is reported, and
// the reference allowance that stood in for a real rule of the language has
// been replaced by that rule written out (`coerce_at_call_site`) rather than
// by a matcher that cannot see references.

#include <iostream>
#include <string>

#include "src/semantic/infer/rigid_match.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::semantic::type_id;
using cinder::semantic::type_table;
using cinder::semantic::infer::match_pattern;
using cinder::testing::expect;

struct fixture {
  type_table table;

  auto param(const std::string &name) -> type_id {
    return table.type_param(name);
  }
  auto ctor_param(const std::string &name, size_t arity) -> type_id {
    return table.type_param(name, arity);
  }
};

auto bound(const cinder::semantic::infer::rigid_match_result &result,
           const std::string &name) -> type_id {
  const auto it = result.bindings.find(name);
  return it == result.bindings.end() ? cinder::semantic::k_unknown_type
                                     : it->second;
}

/// The everyday case every call site depends on: a generic container pattern
/// solving its element from the argument.
auto test_solves_a_nominal_argument() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto pattern = f.table.builtin_generic("list", {f.param("T")});
  const auto concrete = f.table.builtin_generic("list", {int32});

  const auto result = match_pattern(f.table, pattern, concrete);
  expect(!result.failure.has_value(), "expected the match to succeed");
  expect(bound(result, "T") == int32, "expected `T` to solve to `int32`");
}

/// Nested, and through a function type — the shape a lambda parameter takes.
auto test_solves_through_structure() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto boolean = f.table.builtin("bool");
  const auto pattern = f.table.fn_of(
      {f.table.builtin_generic("list", {f.param("T")})}, f.param("U"));
  const auto concrete =
      f.table.fn_of({f.table.builtin_generic("list", {int32})}, boolean);

  const auto result = match_pattern(f.table, pattern, concrete);
  expect(!result.failure.has_value(), "expected the match to succeed");
  expect(bound(result, "T") == int32, "expected the parameter to solve");
  expect(bound(result, "U") == boolean, "expected the result to solve");
}

/// A higher-kinded head, which is the pattern fragment and the reason ch. 37's
/// restriction is worth naming.
auto test_solves_a_constructor_head() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto pattern = f.table.param_app(f.ctor_param("F", 1), {f.param("A")});
  const auto concrete = f.table.builtin_generic("option", {int32});

  const auto result = match_pattern(f.table, pattern, concrete);
  expect(!result.failure.has_value(), "expected the match to succeed");
  expect(bound(result, "A") == int32, "expected the argument to solve");
  expect(bound(result, "F") == f.table.ctor_ref("option", "", nullptr, 1),
         "expected the head to solve to the `option` constructor");
}

/// Defect 1, fixed. `unify_rigid` descended into an array's element and never
/// its length, so `n` was left for a later site to guess at — todo 20 in the
/// dependent fragment. Turning this on changed no recorded decision, so the
/// compat flag that held it back is gone.
auto test_array_length_now_solves() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto usize = f.table.usize_type();
  const auto length = f.table.const_value(usize, 4);
  const auto pattern =
      f.table.array_of(f.param("T"), std::nullopt, f.param("n"));
  const auto concrete = f.table.array_of(int32, 4, length);

  const auto strict = match_pattern(f.table, pattern, concrete);
  expect(bound(strict, "T") == int32, "expected the element to solve");
  expect(bound(strict, "n") == length,
         "expected the length to solve — the gap this replaces");
}

/// Defect 2, fixed. `&T` and `&mut T` differ in a flag `unify_rigid` never
/// read, so a signature wanting a mutable borrow accepted a shared one.
/// Turning this on changed no recorded decision either.
auto test_mutability_now_matters() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto pattern = f.table.ref_to(f.param("T"), /*is_mut=*/true);
  const auto concrete = f.table.ref_to(int32, /*is_mut=*/false);

  const auto strict = match_pattern(f.table, pattern, concrete);
  expect(strict.failure.has_value(), "expected `&mut T` not to match `&int32`");
}

/// Defect 3, retired. A reference pattern met a bare type at *any* depth,
/// which is a blind spot rather than a rule. What it was standing in for is a
/// real rule — the call site borrows or dereferences — and that is now
/// `coerce_at_call_site`, applied at the outermost position only.
auto test_the_call_site_borrows() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto list_int32 = f.table.builtin_generic("list", {int32});

  // `def iter[T](self: &list[T])` called on a `list[int32]` value.
  const auto pattern =
      f.table.ref_to(f.table.builtin_generic("list", {f.param("T")}),
                     /*is_mut=*/false);
  const auto result = match_pattern(f.table, pattern, list_int32);
  expect(!result.failure.has_value(),
         "expected the receiver to be borrowed for a `&self` method");
  expect(bound(result, "T") == int32,
         "expected `T` to solve through the implicit borrow");
}

/// The mirror: a by-value parameter given a reference.
auto test_the_call_site_dereferences() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto pattern = f.table.builtin_generic("list", {f.param("T")});
  const auto concrete = f.table.ref_to(f.table.builtin_generic("list", {int32}),
                                       /*is_mut=*/false);

  const auto result = match_pattern(f.table, pattern, concrete);
  expect(!result.failure.has_value(),
         "expected the reference to be read through");
  expect(bound(result, "T") == int32, "expected `T` to solve through it");
}

/// The line between a coercion and a blind spot. The old allowance fired at
/// every depth, so `list[&T]` matched `list[int32]` and nobody noticed; the
/// rule applies at the outermost position and nowhere else.
auto test_a_nested_reference_is_still_a_mismatch() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto pattern = f.table.builtin_generic(
      "list", {f.table.ref_to(f.param("T"), /*is_mut=*/false)});
  const auto concrete = f.table.builtin_generic("list", {int32});

  const auto result = match_pattern(f.table, pattern, concrete);
  expect(result.failure.has_value(),
         "expected `list[&T]` against `list[int32]` to stay a disagreement");
}

/// A bare parameter meeting a reference keeps the reference — at the top
/// level and nested alike.
///
/// A type parameter is not a by-value parameter: it is willing to *be* a
/// reference, so dereferencing on its behalf discards what the call site
/// said. `describe_view[T](x: T)` given `&n` must infer `T = &int32`, and
/// `std.traits.is_view[T]()` inside it reads exactly that. This is also the
/// case that cannot be expressed by erasing references from the inputs, which
/// is how the first attempt at retiring the old allowance broke `std_test`.
auto test_a_bare_parameter_keeps_the_reference() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto ref_int32 = f.table.ref_to(int32, /*is_mut=*/false);

  const auto top = match_pattern(f.table, f.param("T"), ref_int32);
  expect(bound(top, "T") == ref_int32,
         "expected a bare `T` to solve to `&int32`, not to `int32`");

  const auto nested =
      match_pattern(f.table, f.table.builtin_generic("list", {f.param("T")}),
                    f.table.builtin_generic("list", {ref_int32}));
  expect(bound(nested, "T") == ref_int32,
         "expected the same nested, where no coercion applies at all");
}

/// A real disagreement is reported rather than dropped. `unify_rigid` had
/// nowhere to put this, which is how a mismatched signature reached
/// elaboration as a half-solved binding map.
auto test_a_mismatch_is_reported() -> void {
  auto f = fixture{};
  const auto pattern = f.table.builtin_generic("list", {f.param("T")});
  const auto concrete =
      f.table.builtin_generic("option", {f.table.builtin("int32")});

  const auto result = match_pattern(f.table, pattern, concrete);
  expect(result.failure.has_value(),
         "expected `list[T]` against `option[int32]` to be refused");
}

/// A parameter the concrete type does not mention stays absent rather than
/// binding to `unknown`. Callers read membership, so the distinction is the
/// interface.
auto test_unpinned_parameters_stay_absent() -> void {
  auto f = fixture{};
  const auto pattern = f.table.fn_of({f.param("T")}, f.param("U"));
  const auto concrete =
      f.table.fn_of({f.table.builtin("int32")}, cinder::semantic::k_unknown_type);

  const auto result = match_pattern(f.table, pattern, concrete);
  expect(result.bindings.contains("T"), "expected the pinned one to solve");
  expect(!result.bindings.contains("U"),
         "expected the unpinned one to be absent, not bound to `unknown`");
}

/// A value parameter is spelled exactly like a type parameter, so the store
/// must not refuse to bind it to a compile-time value. Getting this wrong
/// breaks every const generic while leaving ordinary generics green.
auto test_a_value_parameter_binds_to_a_value() -> void {
  auto f = fixture{};
  const auto usize = f.table.usize_type();
  const auto three = f.table.const_value(usize, 3);
  const auto pattern =
      f.table.builtin_generic("vec", {f.param("T"), f.param("n")});
  const auto concrete =
      f.table.builtin_generic("vec", {f.table.builtin("int32"), three});

  const auto result = match_pattern(f.table, pattern, concrete);
  expect(!result.failure.has_value(),
         "expected a value to be an acceptable solution for `n`");
  expect(bound(result, "n") == three, "expected `n` to solve to the value");
}

/// Adoption must not leak: the store is local, so the same `T` matched twice
/// against different types solves independently. A shared store would make
/// the second match a contradiction.
auto test_matches_do_not_share_parameters() -> void {
  auto f = fixture{};
  const auto int32 = f.table.builtin("int32");
  const auto str = f.table.builtin("str");
  const auto pattern = f.table.builtin_generic("list", {f.param("T")});

  const auto first =
      match_pattern(f.table, pattern, f.table.builtin_generic("list", {int32}));
  const auto second =
      match_pattern(f.table, pattern, f.table.builtin_generic("list", {str}));
  expect(bound(first, "T") == int32, "expected the first match to stand");
  expect(bound(second, "T") == str,
         "expected the second match to be independent of the first");
}

} // namespace

auto main() -> int {
  test_solves_a_nominal_argument();
  test_solves_through_structure();
  test_solves_a_constructor_head();
  test_array_length_now_solves();
  test_mutability_now_matters();
  test_the_call_site_borrows();
  test_the_call_site_dereferences();
  test_a_nested_reference_is_still_a_mismatch();
  test_a_bare_parameter_keeps_the_reference();
  test_a_mismatch_is_reported();
  test_unpinned_parameters_stay_absent();
  test_a_value_parameter_binds_to_a_value();
  test_matches_do_not_share_parameters();
  std::cout << "rigid_match_test passed\n";
  return 0;
}
