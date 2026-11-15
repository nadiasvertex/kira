#include "src/core/nodes.h"

#include <utility>

#include "src/testing/test_assert.h"

namespace {

using cinder::testing::expect;
namespace core = cinder::core;

// A place's type is its root's type until a step is taken, then the type
// after the last step.
auto place_type_follows_the_path() -> void {
  auto p = core::place{};
  p.root.type = 7;
  expect(p.type() == 7, "a bare root has the root's type");

  auto field = core::projection{};
  field.kind = core::projection::step::field;
  field.type = 9;
  p.path.push_back(std::move(field));
  auto deref = core::projection{};
  deref.kind = core::projection::step::deref;
  deref.type = 11;
  p.path.push_back(std::move(deref));
  expect(p.type() == 11, "a place has the type after its last step");
}

// An or-pattern arm holds one test and one binding list per alternative.
auto arm_holds_one_binding_list_per_alternative() -> void {
  auto arm = core::match_arm{};
  for (auto index = std::size_t{0}; index < 2; ++index) {
    auto alternative = core::pattern_alternative{};
    alternative.test.kind = core::pattern_test::form::variant;
    alternative.test.variant_index = index;
    auto binding = core::pattern_binding{};
    binding.name = "n";
    alternative.bindings.push_back(std::move(binding));
    arm.alternatives.push_back(std::move(alternative));
  }
  expect(arm.alternatives.size() == 2, "two alternatives");
  expect(arm.alternatives[1].test.variant_index == 1,
         "each alternative keeps its own test");
  expect(arm.alternatives[1].bindings.front().name == "n",
         "each alternative binds the name");
}

} // namespace

auto main() -> int {
  place_type_follows_the_path();
  arm_holds_one_binding_list_per_alternative();
  return 0;
}
