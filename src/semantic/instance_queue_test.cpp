#include <memory>
#include <string>
#include <vector>

#include "src/parser/ast.h"
#include "src/semantic/instance_queue.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::semantic::detail::instance_hooks;
using cinder::semantic::detail::instance_queue;
using cinder::semantic::detail::pending_instance;
using cinder::testing::expect;
namespace ast = cinder::ast;

/// Records each call as `"<what>:<instance name>"`, in order.
struct recording_hooks final : instance_hooks {
  std::vector<std::string> log;
  bool walk = true;
  bool concrete = true;
  /// Requested from inside `check_clone` — a body that asks for another
  /// instance.
  instance_queue *queue = nullptr;
  std::unique_ptr<pending_instance> follow_up;

  auto walks(const pending_instance &) -> bool override { return walk; }
  auto answers_are_concrete(const pending_instance &) -> bool override {
    return concrete;
  }
  auto check_clone(const pending_instance &item) -> void override {
    log.push_back("clone:" + item.instance->name);
    if (follow_up != nullptr) {
      queue->request(*follow_up);
      follow_up.reset();
    }
  }
  auto check_template(const pending_instance &item) -> void override {
    log.push_back("template:" + item.tmpl->name);
  }
  auto substitute(pending_instance &item) -> void override {
    log.push_back("substitute:" + item.instance->name);
  }
  auto record(const pending_instance &item) -> void override {
    log.push_back("record:" + item.instance->name);
  }
  auto walked(const pending_instance &item) -> void override {
    log.push_back("walked:" + item.instance->name);
  }
};

auto named(std::string name) -> std::unique_ptr<ast::func_decl> {
  auto decl = std::make_unique<ast::func_decl>();
  decl->name = std::move(name);
  return decl;
}

auto instance_of(const ast::func_decl &instance, const ast::func_decl &tmpl)
    -> pending_instance {
  auto item = pending_instance{};
  item.instance = &instance;
  item.tmpl = &tmpl;
  return item;
}

auto test_walked_instance_is_checked_recorded_then_marked() -> void {
  auto tmpl = named("t");
  auto inst = named("t$1");
  auto queue = instance_queue{};
  auto hooks = recording_hooks{};
  queue.request(instance_of(*inst, *tmpl));
  queue.flush_walks(hooks);
  expect(hooks.log == std::vector<std::string>{"clone:t$1", "record:t$1",
                                               "walked:t$1"},
         "a walked instance is checked, recorded, then reported walked");
  expect(!queue.has_walks(), "the walk queue is empty after a flush");
}

auto test_substituted_instance_waits_for_its_template() -> void {
  auto tmpl = named("t");
  auto inst = named("t$1");
  auto queue = instance_queue{};
  auto hooks = recording_hooks{};
  hooks.walk = false;
  queue.request(instance_of(*inst, *tmpl));
  queue.flush_walks(hooks);
  expect(queue.has_substitutions(), "a non-walked instance is queued");
  expect(hooks.log.empty(), "nothing is made before the template is checked");
  queue.flush_substitutions(hooks);
  expect(hooks.log.empty(), "an instance of an unchecked template waits");
  expect(!queue.has_substitutions(), "the waiting instance left the queue");
  queue.template_checked(*tmpl, false);
  expect(queue.has_substitutions(), "checking the template requeues it");
  queue.flush_substitutions(hooks);
  expect(hooks.log ==
             std::vector<std::string>{"substitute:t$1", "record:t$1"},
         "the instance is substituted, then recorded");
}

auto test_instance_of_a_failed_template_is_never_made() -> void {
  auto tmpl = named("t");
  auto inst = named("t$1");
  auto queue = instance_queue{};
  auto hooks = recording_hooks{};
  queue.template_checked(*tmpl, true);
  expect(queue.is_failed(*tmpl) && queue.is_checked(*tmpl),
         "a failed template is also checked");
  queue.request(instance_of(*inst, *tmpl));
  queue.flush_walks(hooks);
  hooks.walk = false;
  auto other = pending_instance{instance_of(*inst, *tmpl)};
  queue.make_now(other, hooks);
  queue.flush_substitutions(hooks);
  expect(hooks.log.empty(), "no hook runs for an instance of a failed template");
}

auto test_instance_with_open_answers_is_not_made() -> void {
  auto tmpl = named("t");
  auto inst = named("t$1");
  auto queue = instance_queue{};
  auto hooks = recording_hooks{};
  hooks.walk = false;
  hooks.concrete = false;
  queue.template_checked(*tmpl, false);
  queue.request(instance_of(*inst, *tmpl));
  queue.flush_walks(hooks);
  queue.flush_substitutions(hooks);
  expect(hooks.log.empty(),
         "an instance asked for at an unsolved type is left alone");
}

auto test_instances_requested_while_flushing_are_made() -> void {
  auto tmpl = named("t");
  auto first = named("t$1");
  auto second = named("t$2");
  auto queue = instance_queue{};
  auto hooks = recording_hooks{};
  hooks.queue = &queue;
  hooks.follow_up =
      std::make_unique<pending_instance>(instance_of(*second, *tmpl));
  queue.request(instance_of(*first, *tmpl));
  queue.flush_walks(hooks);
  expect(hooks.log == std::vector<std::string>{"clone:t$1", "record:t$1",
                                               "walked:t$1", "clone:t$2",
                                               "record:t$2", "walked:t$2"},
         "an instance requested by a body is made in the same flush");
}

auto test_unchecked_concrete_template_is_checked_then_substituted() -> void {
  auto tmpl = named("t");
  auto inst = named("t$1");
  auto queue = instance_queue{};
  auto hooks = recording_hooks{};
  hooks.walk = false;
  queue.request(instance_of(*inst, *tmpl));
  queue.flush_walks(hooks);
  queue.flush_substitutions(hooks); // parks it as waiting
  // The template body gets checked by `flush_unchecked`'s hook; the fake
  // does not mark it, so the clone is walked instead.
  queue.flush_unchecked(hooks);
  expect(hooks.log ==
             std::vector<std::string>{"template:t", "clone:t$1", "record:t$1"},
         "a concrete unchecked template is checked, then its clone walked "
         "when the template still is not marked checked");
}

auto test_unchecked_generic_template_walks_its_clone() -> void {
  auto tmpl = named("t");
  tmpl->type_params.emplace_back();
  auto inst = named("t$1");
  auto queue = instance_queue{};
  auto hooks = recording_hooks{};
  hooks.walk = false;
  queue.request(instance_of(*inst, *tmpl));
  queue.flush_walks(hooks);
  queue.flush_substitutions(hooks);
  queue.flush_unchecked(hooks);
  expect(hooks.log == std::vector<std::string>{"clone:t$1", "record:t$1"},
         "a generic template is never checked on its own at the end");
}

} // namespace

auto main() -> int {
  test_walked_instance_is_checked_recorded_then_marked();
  test_substituted_instance_waits_for_its_template();
  test_instance_of_a_failed_template_is_never_made();
  test_instance_with_open_answers_is_not_made();
  test_instances_requested_while_flushing_are_made();
  test_unchecked_concrete_template_is_checked_then_substituted();
  test_unchecked_generic_template_walks_its_clone();
  return 0;
}
