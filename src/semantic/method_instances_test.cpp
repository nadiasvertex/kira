#include <memory>
#include <string>
#include <unordered_set>

#include "src/parser/ast.h"
#include "src/semantic/method_instances.h"
#include "src/semantic/type_table.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::semantic::type_table;
using cinder::semantic::detail::classify_trait_item;
using cinder::semantic::detail::method_instance_name;
using cinder::semantic::detail::trait_default_bindings;
using cinder::semantic::detail::trait_item_action;
using cinder::testing::expect;
namespace ast = cinder::ast;

auto method(std::string name, bool with_self, bool with_body)
    -> ast::func_decl {
  auto decl = ast::func_decl{};
  decl.name = std::move(name);
  if (with_self) {
    decl.params.emplace_back();
  }
  if (with_body) {
    decl.body_expr = std::make_unique<ast::literal_expr>();
  }
  return decl;
}

auto test_instance_name_joins_target_method_and_suffix() -> void {
  expect(method_instance_name("option", "pure", "$int32") ==
             "option::pure$int32",
         "name is target::method followed by the suffix");
  expect(method_instance_name("vec", "at", "") == "vec::at",
         "an empty suffix leaves the bare member name");
}

auto test_overridden_default_is_not_inherited() -> void {
  const auto decl = method("show", true, true);
  const auto action =
      classify_trait_item(decl, "self", std::unordered_set<std::string>{"show"});
  expect(action == trait_item_action::overridden,
         "a method the impl writes is not derived from the trait");
}

auto test_default_with_self_and_body_is_derived() -> void {
  const auto decl = method("show", true, true);
  expect(classify_trait_item(decl, "self", {}) ==
             trait_item_action::derive_default,
         "a default body that takes self is derived per impl");
}

auto test_required_or_selfless_item_is_registered_as_declared() -> void {
  const auto required = method("write", true, false);
  expect(classify_trait_item(required, "self", {}) ==
             trait_item_action::register_as_declared,
         "a method with no default body has nothing to derive");
  const auto selfless = method("new", false, true);
  expect(classify_trait_item(selfless, "", {}) ==
             trait_item_action::register_as_declared,
         "a function with no parameters has no self to derive against");
  const auto other_first = method("make", true, true);
  expect(classify_trait_item(other_first, "value", {}) ==
             trait_item_action::register_as_declared,
         "a first parameter that is not `self` is not a receiver");
}

auto test_hk_trait_binds_its_constructor_to_the_target() -> void {
  auto types = type_table{};
  auto trait = ast::trait_decl{};
  trait.name = "functor";
  auto param = ast::type_param{};
  param.name = "F";
  param.higher_kinded_arity = 1;
  trait.type_params.push_back(std::move(param));
  const auto option = types.ctor_ref("option", "", nullptr, 1);
  const auto bound = trait_default_bindings(types, &trait, option);
  expect(bound.size() == 1 && bound.at("F") == option,
         "the constructor parameter is bound to the impl's target");
  const auto plain = types.builtin("int32");
  expect(trait_default_bindings(types, &trait, plain).empty(),
         "a target that is not a constructor binds nothing");
  trait.type_params.front().higher_kinded_arity = 0;
  expect(trait_default_bindings(types, &trait, option).empty(),
         "a first-order trait parameter binds nothing");
  expect(trait_default_bindings(types, nullptr, option).empty(),
         "no trait binds nothing");
}

} // namespace

auto main() -> int {
  test_instance_name_joins_target_method_and_suffix();
  test_overridden_default_is_not_inherited();
  test_default_with_self_and_body_is_derived();
  test_required_or_selfless_item_is_registered_as_declared();
  test_hk_trait_binds_its_constructor_to_the_target();
  return 0;
}
