#include "src/semantic/instance_queue.h"

#include <optional>
#include <utility>

namespace cinder::semantic::detail {

auto instance_queue::request(pending_instance item) -> void {
  walks_.push_back(std::move(item));
}

auto instance_queue::template_checked(const ast::func_decl &tmpl, bool failed)
    -> void {
  if (failed) {
    failed_.insert(&tmpl);
  }
  checked_.insert(&tmpl);
  if (const auto waiting = waiting_.find(&tmpl); waiting != waiting_.end()) {
    for (auto &item : waiting->second) {
      substitutions_.push_back(std::move(item));
    }
    waiting_.erase(waiting);
  }
}

auto instance_queue::is_checked(const ast::func_decl &tmpl) const -> bool {
  return checked_.contains(&tmpl);
}

auto instance_queue::is_failed(const ast::func_decl &tmpl) const -> bool {
  return failed_.contains(&tmpl);
}

auto instance_queue::make_now(pending_instance &item, instance_hooks &hooks)
    -> void {
  if (is_failed(*item.tmpl)) {
    return;
  }
  if (!hooks.walks(item)) {
    substitutions_.push_back(item);
    return;
  }
  hooks.check_clone(item);
  hooks.record(item);
  hooks.walked(item);
}

namespace {

/// The queued item in `items` whose clone is `instance`, by index.
[[nodiscard]] auto index_of(const std::vector<pending_instance> &items,
                            const ast::func_decl &instance)
    -> std::optional<size_t> {
  for (size_t i = 0; i < items.size(); ++i) {
    if (items[i].instance == &instance) {
      return i;
    }
  }
  return std::nullopt;
}

} // namespace

auto instance_queue::queued_template(const ast::func_decl &instance) const
    -> const ast::func_decl * {
  if (made_.contains(&instance)) {
    return nullptr;
  }
  for (const auto *items : {&walks_, &substitutions_}) {
    if (const auto at = index_of(*items, instance)) {
      return (*items)[*at].tmpl;
    }
  }
  for (const auto &[tmpl, items] : waiting_) {
    if (index_of(items, instance).has_value()) {
      return tmpl;
    }
  }
  return nullptr;
}

auto instance_queue::make_queued_now(const ast::func_decl &instance,
                                     instance_hooks &hooks) -> bool {
  if (made_.contains(&instance)) {
    return false;
  }
  // Copied out and marked made before making it, rather than erased: a
  // flush may be iterating these vectors by index right now.
  auto item = std::optional<pending_instance>{};
  for (auto *items : {&walks_, &substitutions_}) {
    if (const auto at = index_of(*items, instance)) {
      item = (*items)[*at];
      break;
    }
  }
  if (!item.has_value()) {
    for (auto &[tmpl, items] : waiting_) {
      if (const auto at = index_of(items, instance)) {
        item = items[*at];
        break;
      }
    }
  }
  if (!item.has_value() || is_failed(*item->tmpl)) {
    return false;
  }
  if (hooks.walks(*item)) {
    made_.insert(&instance);
    hooks.check_clone(*item);
    hooks.record(*item);
    hooks.walked(*item);
    return true;
  }
  if (!is_checked(*item->tmpl) || !hooks.answers_are_concrete(*item)) {
    return false;
  }
  made_.insert(&instance);
  hooks.substitute(*item);
  hooks.record(*item);
  return true;
}

auto instance_queue::flush_walks(instance_hooks &hooks) -> void {
  auto processed = size_t{0};
  while (processed < walks_.size()) {
    // By index, not by iterator or reference: making this instance can
    // request more, which reallocates.
    auto item = walks_[processed];
    ++processed;
    if (made_.contains(item.instance)) {
      continue;
    }
    make_now(item, hooks);
  }
  walks_.clear();
}

auto instance_queue::flush_substitutions(instance_hooks &hooks) -> void {
  auto processed = size_t{0};
  while (processed < substitutions_.size()) {
    // By index: substituting runs elaborations, which queue instances.
    auto item = substitutions_[processed];
    ++processed;
    if (is_failed(*item.tmpl) || made_.contains(item.instance)) {
      continue;
    }
    if (!is_checked(*item.tmpl)) {
      waiting_[item.tmpl].push_back(std::move(item));
      continue;
    }
    make_by_substitution(item, hooks);
  }
  substitutions_.clear();
}

auto instance_queue::make_by_substitution(pending_instance &item,
                                          instance_hooks &hooks) -> void {
  if (!hooks.answers_are_concrete(item)) {
    return;
  }
  hooks.substitute(item);
  hooks.record(item);
}

auto instance_queue::flush_unchecked(instance_hooks &hooks) -> void {
  auto waiting = std::move(waiting_);
  waiting_.clear();
  for (auto &[tmpl, items] : waiting) {
    for (auto &item : items) {
      if (is_failed(*item.tmpl) || made_.contains(item.instance)) {
        continue;
      }
      // A body that is concrete already — a method `deriving` synthesized
      // for one instantiation — is checked once now, in its instance's
      // context, and substituted like any other.
      const auto concrete = item.tmpl->type_params.empty() &&
                            (item.block_type_params == nullptr ||
                             item.block_type_params->empty());
      if (concrete && !is_checked(*item.tmpl)) {
        hooks.check_template(item);
      }
      if (is_checked(*item.tmpl)) {
        make_by_substitution(item, hooks);
        continue;
      }
      hooks.check_clone(item);
      hooks.record(item);
    }
  }
}

} // namespace cinder::semantic::detail
