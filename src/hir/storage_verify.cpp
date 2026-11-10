#include "src/hir/storage_verify.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cinder::hir {

namespace {

using semantic::type_id;

/// Largest array whose elements the replay tracks one by one. A bigger array
/// must be initialized as a whole.
constexpr uint64_t k_max_tracked_elements = 4096;

/// Which parts of one slot hold a value. A node is either a whole value or a
/// set of initialized children; an absent child is uninitialized.
struct init_node {
  bool whole = false;
  std::map<std::string, init_node> children;

  [[nodiscard]] auto empty() const -> bool {
    return !whole && children.empty();
  }

  [[nodiscard]] auto operator==(const init_node &) const -> bool = default;
};

[[nodiscard]] auto member_key(size_t index) -> std::string {
  return "m" + std::to_string(index);
}
[[nodiscard]] auto element_key(size_t index) -> std::string {
  return "e" + std::to_string(index);
}
[[nodiscard]] auto payload_key(const std::string &variant, size_t index)
    -> std::string {
  return "p" + variant + ":" + std::to_string(index);
}

[[nodiscard]] auto step_key(const projection &step) -> std::string {
  switch (step.kind) {
  case projection_kind::member:
    return member_key(step.index);
  case projection_kind::element:
    return element_key(step.index);
  case projection_kind::payload:
    return payload_key(step.name, step.index);
  case projection_kind::deref:
    return "*";
  }
  return "?";
}

/// Whether two places can name the same bytes: same slot, and one path is a
/// prefix of the other.
[[nodiscard]] auto overlaps(const place &a, const place &b) -> bool {
  if (a.base != b.base) {
    return false;
  }
  const auto shared = std::min(a.steps.size(), b.steps.size());
  for (size_t i = 0; i < shared; ++i) {
    if (!(a.steps[i] == b.steps[i])) {
      return false;
    }
  }
  return true;
}

/// The place a place names, once resolved against layouts.
struct resolved_place {
  /// `types[i]` is the type of the value after `i` steps; `types[0]` is the
  /// slot's type.
  std::vector<type_id> types;
  /// How many leading steps the replay tracks: all of them, or those before
  /// the first `deref`. Bytes behind an address are borrowed and untracked.
  size_t tracked = 0;
  bool through_deref = false;
  /// Whether one of the dereferences is of a raw pointer. Raw memory has no
  /// owner the compiler tracks (chapter 38): a read gives the reader the
  /// value, and a write never drops what the slot held.
  bool through_raw = false;
  /// Whether the slot is a variable the closure captured. The closure owns an
  /// environment value, or borrows the variable it points at, and either way a
  /// call to the closure can happen again, so nothing moves out of it.
  bool captured = false;
};

class replay {
public:
  replay(const storage_function &function, storage_env &env,
         const storage_module *callees, const verify_options &options)
      : fn_(function), env_(env), callees_(callees), options_(options),
        state_(function.slots.size()), dropped_(function.slots.size()),
        dead_(function.slots.size(), false) {}

  [[nodiscard]] auto run() -> std::expected<void, verify_error> {
    if (auto ok = check_slots(); !ok.has_value()) {
      return ok;
    }
    for (op_index_ = 0; op_index_ < fn_.ops.size(); ++op_index_) {
      const auto &op = fn_.ops[op_index_];
      span_ = op.span;
      if (!reachable_ && !continues_dead_path(op.kind)) {
        return fail(verify_error_kind::code_after_return,
                    "an operation follows a return or an exit");
      }
      if (auto ok = apply(op); !ok.has_value()) {
        return ok;
      }
    }
    op_index_ = static_cast<size_t>(-1);
    if (!regions_.empty()) {
      return fail(verify_error_kind::bad_operation,
                  "a conditional, loop, or block is never closed");
    }
    if (reachable_) {
      return fail(verify_error_kind::missing_return,
                  "the function can end without returning");
    }
    return {};
  }

private:
  using result = std::expected<void, verify_error>;

  [[nodiscard]] auto types() const -> const semantic::type_table & {
    return env_.types();
  }

  [[nodiscard]] auto fail(verify_error_kind kind, std::string message) const
      -> std::unexpected<verify_error> {
    return std::unexpected(verify_error{.kind = kind,
                                        .function = fn_.name,
                                        .op_index = op_index_,
                                        .span = span_,
                                        .message = std::move(message)});
  }

  [[nodiscard]] auto name_of(type_id id) const -> std::string {
    return "`" + types().display(id) + "`";
  }

  [[nodiscard]] auto same_type(type_id a, type_id b) const -> bool {
    return types().strip_refinement(a) == types().strip_refinement(b);
  }

  [[nodiscard]] auto layout_of(type_id id)
      -> std::expected<const runtime::value_layout *, verify_error> {
    const auto layout = env_.layouts.layout(id);
    if (!layout.has_value()) {
      return fail(verify_error_kind::layout_failed, layout.error().message);
    }
    return *layout;
  }

  // ------------------------------------------------------------------
  //  Slots and signature
  // ------------------------------------------------------------------

  [[nodiscard]] auto check_slots() -> result {
    op_index_ = static_cast<size_t>(-1);
    for (size_t i = 0; i < fn_.slots.size(); ++i) {
      const auto &slot = fn_.slots[i];
      if (slot.id != i) {
        return fail(verify_error_kind::bad_slot,
                    "slot s" + std::to_string(i) + " carries id s" +
                        std::to_string(slot.id));
      }
      if (slot.owner >= fn_.slots.size()) {
        return fail(verify_error_kind::bad_slot,
                    "slot s" + std::to_string(i) + " has no valid owner");
      }
      span_ = slot.span;
      if (auto layout = layout_of(slot.type); !layout.has_value()) {
        return std::unexpected(layout.error());
      }
      if (slot.cls == storage_class::closure_environment &&
          (!slot.initially_initialized || !fn_.signature.takes_environment)) {
        return fail(verify_error_kind::bad_slot,
                    "slot s" + std::to_string(i) +
                        " is a captured value of a function with no "
                        "environment, or one that starts empty");
      }
      state_[i].whole = slot.initially_initialized;
    }
    if (fn_.param_slots.size() != fn_.signature.params.size()) {
      return fail(verify_error_kind::bad_slot,
                  "the signature has " +
                      std::to_string(fn_.signature.params.size()) +
                      " parameters but " +
                      std::to_string(fn_.param_slots.size()) +
                      " parameter slots");
    }
    for (size_t i = 0; i < fn_.param_slots.size(); ++i) {
      const auto id = fn_.param_slots[i];
      if (id >= fn_.slots.size() ||
          fn_.slots[id].cls != storage_class::parameter ||
          !fn_.slots[id].initially_initialized ||
          !same_type(fn_.slots[id].type, fn_.signature.params[i].type)) {
        return fail(verify_error_kind::bad_slot,
                    "parameter " + std::to_string(i) +
                        " is not an initialized parameter slot of its type");
      }
    }
    const auto through_address = fn_.signature.returns_through_address();
    if (through_address) {
      if (fn_.result_slot >= fn_.slots.size() ||
          fn_.slots[fn_.result_slot].cls != storage_class::caller_result ||
          fn_.slots[fn_.result_slot].initially_initialized ||
          !same_type(fn_.slots[fn_.result_slot].type,
                     fn_.signature.result.type)) {
        return fail(verify_error_kind::bad_slot,
                    "the result needs an uninitialized caller_result slot of "
                    "its type");
      }
    } else if (fn_.result_slot != k_invalid_storage) {
      return fail(verify_error_kind::bad_slot,
                  "a result returned directly has no result slot");
    }
    for (const auto &slot : fn_.slots) {
      if (slot.cls == storage_class::caller_result &&
          slot.id != fn_.result_slot) {
        return fail(verify_error_kind::bad_slot,
                    "slot s" + std::to_string(slot.id) +
                        " is a second caller_result");
      }
    }
    return {};
  }

  // ------------------------------------------------------------------
  //  Places
  // ------------------------------------------------------------------

  [[nodiscard]] auto resolve(const place &where)
      -> std::expected<resolved_place, verify_error> {
    if (where.base >= fn_.slots.size()) {
      return fail(verify_error_kind::bad_place,
                  "place names slot s" + std::to_string(where.base) +
                      ", which does not exist");
    }
    if (dead_[where.base]) {
      return fail(verify_error_kind::use_after_dead,
                  "slot s" + std::to_string(where.base) +
                      " is used after its storage ended");
    }
    auto out = resolved_place{};
    out.tracked = where.steps.size();
    out.captured =
        fn_.slots[where.base].cls == storage_class::closure_environment;
    auto current = fn_.slots[where.base].type;
    out.types.push_back(current);
    for (size_t i = 0; i < where.steps.size(); ++i) {
      const auto &step = where.steps[i];
      if (step.kind == projection_kind::deref) {
        const auto &entry = types().entry(types().strip_refinement(current));
        if (entry.kind != semantic::type_kind::ref_kind &&
            entry.kind != semantic::type_kind::ptr_kind) {
          return fail(verify_error_kind::bad_place,
                      "cannot dereference " + name_of(current));
        }
        if (!out.through_deref) {
          out.through_deref = true;
          out.tracked = i;
        }
        if (entry.kind == semantic::type_kind::ptr_kind) {
          out.through_raw = true;
        }
        current = entry.result;
        out.types.push_back(current);
        continue;
      }
      const auto layout = layout_of(current);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      const auto &found = **layout;
      switch (step.kind) {
      case projection_kind::member: {
        if (found.kind != runtime::value_layout_kind::structure &&
            found.kind != runtime::value_layout_kind::tuple) {
          return fail(verify_error_kind::bad_place,
                      name_of(current) + " has no members");
        }
        if (step.index >= found.members.size()) {
          return fail(verify_error_kind::bad_place,
                      "member " + std::to_string(step.index) + " is past the " +
                          std::to_string(found.members.size()) +
                          " members of " + name_of(current));
        }
        const auto &member = found.members[step.index];
        if (!step.name.empty() && step.name != member.name) {
          return fail(verify_error_kind::bad_place,
                      "member " + std::to_string(step.index) + " of " +
                          name_of(current) + " is `" + member.name +
                          "`, not `" + step.name + "`");
        }
        current = member.type;
        break;
      }
      case projection_kind::element:
        if (found.kind != runtime::value_layout_kind::array) {
          return fail(verify_error_kind::bad_place,
                      name_of(current) + " is not an array");
        }
        if (step.index >= found.count) {
          return fail(verify_error_kind::bad_place,
                      "element " + std::to_string(step.index) +
                          " is past the end of " + name_of(current));
        }
        current = found.element;
        break;
      case projection_kind::payload: {
        if (found.kind != runtime::value_layout_kind::sum) {
          return fail(verify_error_kind::bad_place,
                      name_of(current) + " is not a sum");
        }
        const auto *variant = found.find_variant(step.name);
        if (variant == nullptr) {
          return fail(verify_error_kind::bad_place,
                      name_of(current) + " has no variant `" + step.name +
                          "`");
        }
        if (step.index >= variant->payload.size()) {
          return fail(verify_error_kind::bad_place,
                      "payload slot " + std::to_string(step.index) +
                          " is past the end of variant `" + step.name + "`");
        }
        current = variant->payload[step.index].type;
        break;
      }
      case projection_kind::deref:
        break;
      }
      out.types.push_back(current);
    }
    if (!same_type(current, where.type)) {
      return fail(verify_error_kind::type_mismatch,
                  "place is typed " + name_of(where.type) +
                      " but its projections arrive at " + name_of(current));
    }
    return out;
  }

  // ------------------------------------------------------------------
  //  Initialization state
  // ------------------------------------------------------------------

  [[nodiscard]] auto is_sum(type_id id) -> bool {
    const auto layout = env_.layouts.layout(id);
    return layout.has_value() &&
           (*layout)->kind == runtime::value_layout_kind::sum;
  }

  /// Whether `node`, holding a `type`, is a complete value.
  [[nodiscard]] auto covered(const init_node &node, type_id type) -> bool {
    if (node.whole) {
      return true;
    }
    const auto layout = env_.layouts.layout(type);
    if (!layout.has_value()) {
      return false;
    }
    const auto &found = **layout;
    switch (found.kind) {
    case runtime::value_layout_kind::structure:
    case runtime::value_layout_kind::tuple:
      if (found.members.empty()) {
        return false;
      }
      for (size_t i = 0; i < found.members.size(); ++i) {
        const auto child = node.children.find(member_key(i));
        if (child == node.children.end() ||
            !covered(child->second, found.members[i].type)) {
          return false;
        }
      }
      return true;
    case runtime::value_layout_kind::array:
      if (found.count == 0 || found.count > k_max_tracked_elements) {
        return false;
      }
      for (size_t i = 0; i < found.count; ++i) {
        const auto child = node.children.find(element_key(i));
        if (child == node.children.end() ||
            !covered(child->second, found.element)) {
          return false;
        }
      }
      return true;
    default:
      return false;
    }
  }

  /// The same state with every complete part folded into a whole value, so
  /// two ways of building one value compare equal.
  [[nodiscard]] auto normalize(init_node node, type_id type) -> init_node {
    if (node.whole) {
      return node;
    }
    if (covered(node, type)) {
      auto out = init_node{};
      out.whole = true;
      return out;
    }
    const auto layout = env_.layouts.layout(type);
    if (!layout.has_value()) {
      return node;
    }
    const auto &found = **layout;
    const auto is_aggregate = found.kind == runtime::value_layout_kind::structure ||
                              found.kind == runtime::value_layout_kind::tuple;
    if (is_aggregate) {
      for (size_t i = 0; i < found.members.size(); ++i) {
        const auto child = node.children.find(member_key(i));
        if (child != node.children.end()) {
          child->second = normalize(child->second, found.members[i].type);
        }
      }
    } else if (found.kind == runtime::value_layout_kind::array &&
               found.count <= k_max_tracked_elements) {
      for (size_t i = 0; i < found.count; ++i) {
        const auto child = node.children.find(element_key(i));
        if (child != node.children.end()) {
          child->second = normalize(child->second, found.element);
        }
      }
    }
    return node;
  }

  /// Whether any byte of the first `count` steps of `where` holds a value.
  [[nodiscard]] auto any_init(const place &where, size_t count) const -> bool {
    const auto *node = &state_[where.base];
    for (size_t i = 0; i < count; ++i) {
      if (node->whole) {
        return true;
      }
      const auto child = node->children.find(step_key(where.steps[i]));
      if (child == node->children.end()) {
        return false;
      }
      node = &child->second;
    }
    return !node->empty();
  }

  /// Whether the first `count` steps of `where` name a complete value.
  [[nodiscard]] auto fully_init(const place &where, const resolved_place &at,
                                size_t count) -> bool {
    const auto *node = &state_[where.base];
    for (size_t i = 0; i < count; ++i) {
      if (node->whole) {
        return true;
      }
      if (is_sum(at.types[i])) {
        return false; // The variant is not published yet.
      }
      const auto child = node->children.find(step_key(where.steps[i]));
      if (child == node->children.end()) {
        return false;
      }
      node = &child->second;
    }
    return covered(*node, at.types[count]);
  }

  auto mark_init(const place &where, size_t count) -> void {
    auto *node = &state_[where.base];
    for (size_t i = 0; i < count; ++i) {
      node = &node->children[step_key(where.steps[i])];
    }
    node->whole = true;
    node->children.clear();
  }

  /// Splits a whole value into its members so one can leave.
  [[nodiscard]] auto explode(init_node &node, type_id type) -> result {
    const auto layout = layout_of(type);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto &found = **layout;
    if (found.kind == runtime::value_layout_kind::structure ||
        found.kind == runtime::value_layout_kind::tuple) {
      for (size_t i = 0; i < found.members.size(); ++i) {
        node.children[member_key(i)].whole = true;
      }
      node.whole = false;
      return {};
    }
    return fail(verify_error_kind::partial_move,
                "cannot take part of " + name_of(type) +
                    "; a sum or an array is moved or dropped whole");
  }

  /// Marks the first `count` steps of `where` as holding no value.
  [[nodiscard]] auto invalidate(const place &where, const resolved_place &at,
                                size_t count) -> result {
    if (count == 0) {
      state_[where.base] = init_node{};
      return {};
    }
    auto trail = std::vector<std::pair<init_node *, std::string>>{};
    auto *node = &state_[where.base];
    for (size_t i = 0; i < count; ++i) {
      if (node->whole) {
        if (auto ok = explode(*node, at.types[i]); !ok.has_value()) {
          return ok;
        }
      }
      auto key = step_key(where.steps[i]);
      const auto child = node->children.find(key);
      if (child == node->children.end()) {
        return {};
      }
      trail.emplace_back(node, std::move(key));
      node = &child->second;
    }
    trail.back().first->children.erase(trail.back().second);
    for (size_t j = trail.size() - 1; j-- > 0;) {
      const auto child = trail[j].first->children.find(trail[j].second);
      if (child != trail[j].first->children.end() && child->second.empty()) {
        trail[j].first->children.erase(child);
      }
    }
    return {};
  }

  /// A value the operation reads must be complete. Behind an address the
  /// referent is borrowed, so only the address itself is checked.
  [[nodiscard]] auto require_init(const place &where, const resolved_place &at,
                                  bool allow_dropped = false) -> result {
    if (!fully_init(where, at, at.tracked)) {
      return fail(verify_error_kind::uninitialized,
                  "s" + std::to_string(where.base) + " is read before it is " +
                      "completely initialized");
    }
    if (!allow_dropped && touches_dropped(where, at.tracked)) {
      return fail(verify_error_kind::use_after_drop,
                  describe(where, at.tracked) +
                      " is used after its drop ran");
    }
    return {};
  }

  /// A destination must hold no value, hold one that needs no drop, or hold
  /// one whose drops have all run.
  [[nodiscard]] auto require_writable(const place &where,
                                      const resolved_place &at) -> result {
    if (at.through_deref) {
      return require_init(where, at);
    }
    // Writing a field of a value whose own drop ran would build on a value
    // that is already finished.
    if (above_dropped(where, at.tracked)) {
      return fail(verify_error_kind::use_after_drop,
                  describe(where, at.tracked) +
                      " is written inside a value whose drop ran");
    }
    if (!any_init(where, at.tracked)) {
      return {};
    }
    if (!env_.needs_drop(where.type)) {
      if (fully_init(where, at, at.tracked)) {
        return {};
      }
    } else if (!owed_drop(where, at.tracked).has_value()) {
      return {};
    }
    return fail(verify_error_kind::already_initialized,
                "s" + std::to_string(where.base) +
                    " already holds a value; drop it before writing again");
  }

  auto commit_write(const place &where, const resolved_place &at) -> void {
    if (!at.through_deref) {
      mark_init(where, at.tracked);
      forget_drops(where, at.tracked);
    }
  }

  // ------------------------------------------------------------------
  //  Drops
  // ------------------------------------------------------------------

  [[nodiscard]] static auto path_key(const place &where, size_t count)
      -> std::string {
    auto out = std::string{};
    for (size_t i = 0; i < count; ++i) {
      out += "/" + step_key(where.steps[i]);
    }
    return out;
  }

  [[nodiscard]] static auto is_prefix(const std::string &prefix,
                                      const std::string &path) -> bool {
    return path == prefix ||
           (path.size() > prefix.size() && path.starts_with(prefix) &&
            path[prefix.size()] == '/');
  }

  [[nodiscard]] auto describe(const place &where, size_t count) const
      -> std::string {
    return "s" + std::to_string(where.base) + path_key(where, count);
  }

  /// Whether the drop of `where`, of anything it contains, or of anything
  /// that contains it has run.
  [[nodiscard]] auto touches_dropped(const place &where, size_t count) const
      -> bool {
    const auto path = path_key(where, count);
    for (const auto &done : dropped_[where.base]) {
      if (is_prefix(done, path) || is_prefix(path, done)) {
        return true;
      }
    }
    return false;
  }

  /// Whether a value that contains `where` has had its own drop run.
  [[nodiscard]] auto above_dropped(const place &where, size_t count) const
      -> bool {
    const auto path = path_key(where, count);
    for (const auto &done : dropped_[where.base]) {
      if (done != path && is_prefix(done, path)) {
        return true;
      }
    }
    return false;
  }

  /// A fresh value at `where` owes its drops again.
  auto forget_drops(const place &where, size_t count) -> void {
    const auto path = path_key(where, count);
    auto &done = dropped_[where.base];
    for (auto it = done.begin(); it != done.end();) {
      it = is_prefix(path, *it) ? done.erase(it) : std::next(it);
    }
  }

  /// The first place under `node` whose drop is still owed, if any. `node` is
  /// null for a part that was never initialized or was moved out.
  [[nodiscard]] auto first_owed(const init_node *node, type_id type,
                                storage_id slot, const std::string &path,
                                const std::set<std::string> &dropped)
      -> std::optional<std::string> {
    static const auto whole_value = [] {
      auto node = init_node{};
      node.whole = true;
      return node;
    }();
    if (node == nullptr || node->empty()) {
      return std::nullopt;
    }
    const auto stripped = types().strip_refinement(type);
    const auto here = "s" + std::to_string(slot) + path;
    const auto child_of = [&](const std::string &key) -> const init_node * {
      if (node->whole) {
        return &whole_value;
      }
      const auto found = node->children.find(key);
      return found == node->children.end() ? nullptr : &found->second;
    };
    const auto *plan = env_.drop_plan(stripped);
    if (plan != nullptr) {
      const auto has_own = plan->own_drop.has_value() ||
                           plan->closure_glue.has_value() ||
                           plan->cancels_generator;
      if (has_own && !dropped.contains(path)) {
        return here;
      }
      if (plan->closure_glue.has_value() || plan->cancels_generator) {
        return std::nullopt;
      }
      if (!plan->variant_drops.empty()) {
        return here; // Only `drop_in_place` ends a sum.
      }
      const auto layout = env_.layouts.layout(stripped);
      if (!layout.has_value()) {
        return here;
      }
      for (const auto &[name, field_type] : plan->droppable_fields) {
        for (size_t i = 0; i < (*layout)->members.size(); ++i) {
          if ((*layout)->members[i].name != name) {
            continue;
          }
          if (auto owed = first_owed(child_of(member_key(i)), field_type, slot,
                                     path + "/" + member_key(i), dropped);
              owed.has_value()) {
            return owed;
          }
        }
      }
      return std::nullopt;
    }
    if (types().entry(stripped).kind == semantic::type_kind::fn_kind) {
      return dropped.contains(path) ? std::nullopt
                                           : std::optional{here};
    }
    const auto layout = env_.layouts.layout(stripped);
    if (!layout.has_value()) {
      return std::nullopt;
    }
    const auto &found = **layout;
    if (found.kind == runtime::value_layout_kind::tuple ||
        found.kind == runtime::value_layout_kind::structure) {
      for (size_t i = 0; i < found.members.size(); ++i) {
        if (auto owed = first_owed(child_of(member_key(i)), found.members[i].type,
                                   slot, path + "/" + member_key(i), dropped);
            owed.has_value()) {
          return owed;
        }
      }
    } else if (found.kind == runtime::value_layout_kind::array &&
               env_.needs_drop(found.element)) {
      return here; // Only `drop_in_place` ends an array's elements.
    }
    return std::nullopt;
  }

  /// The first drop still owed by the value at `where`, if any.
  [[nodiscard]] auto owed_drop(const place &where, size_t count)
      -> std::optional<std::string> {
    const auto *node = &state_[where.base];
    for (size_t i = 0; i < count; ++i) {
      if (node->whole) {
        node = nullptr;
        break;
      }
      const auto child = node->children.find(step_key(where.steps[i]));
      if (child == node->children.end()) {
        return std::nullopt;
      }
      node = &child->second;
    }
    if (node == nullptr) {
      // A whole ancestor: the value here is initialized and untouched.
      static const auto whole_value = [] {
        auto node = init_node{};
        node.whole = true;
        return node;
      }();
      node = &whole_value;
    }
    return first_owed(node, where.type, where.base, path_key(where, count),
                      dropped_[where.base]);
  }

  // ------------------------------------------------------------------
  //  Operands
  // ------------------------------------------------------------------

  [[nodiscard]] auto check_operand(const operand &value) -> result {
    switch (value.kind) {
    case operand_kind::constant:
      return {};
    case operand_kind::address_of: {
      const auto at = resolve(value.where);
      if (!at.has_value()) {
        return std::unexpected(at.error());
      }
      if (auto ok = require_init(value.where, *at); !ok.has_value()) {
        return ok;
      }
      const auto &entry =
          types().entry(types().strip_refinement(value.type));
      if ((entry.kind != semantic::type_kind::ref_kind &&
           entry.kind != semantic::type_kind::ptr_kind) ||
          !same_type(entry.result, value.where.type)) {
        return fail(verify_error_kind::type_mismatch,
                    "the address of " + name_of(value.where.type) +
                        " is typed " + name_of(value.type));
      }
      return {};
    }
    case operand_kind::read: {
      const auto at = resolve(value.where);
      if (!at.has_value()) {
        return std::unexpected(at.error());
      }
      if (!same_type(value.type, value.where.type)) {
        return fail(verify_error_kind::type_mismatch,
                    "operand is typed " + name_of(value.type) +
                        " but reads " + name_of(value.where.type));
      }
      const auto cls = classify_value(env_.layouts, value.type);
      if (!cls.has_value()) {
        return fail(verify_error_kind::layout_failed, cls.error().message);
      }
      if (is_indirect(cls->cls)) {
        return fail(verify_error_kind::bad_operation,
                    name_of(value.type) +
                        " travels by address and cannot be an operand");
      }
      if (auto ok = require_init(value.where, *at); !ok.has_value()) {
        return ok;
      }
      if (value.consume) {
        if (at->through_deref) {
          if (at->through_raw) {
            return {}; // The reader owns the value; the slot is now empty.
          }
          return fail(verify_error_kind::move_from_reference,
                      "cannot take ownership of a value behind a reference");
        }
        if (at->captured) {
          return fail(verify_error_kind::move_from_reference,
                      "cannot take ownership of a captured value");
        }
        return invalidate(value.where, *at, at->tracked);
      }
      return {};
    }
    }
    return {};
  }

  // ------------------------------------------------------------------
  //  Operations
  // ------------------------------------------------------------------

  [[nodiscard]] auto apply(const storage_op &op) -> result {
    switch (op.kind) {
    case op_kind::compute:
      return do_compute(op);
    case op_kind::init_empty:
      return do_init_empty(op);
    case op_kind::copy_into:
    case op_kind::move_into:
      return do_transfer(op);
    case op_kind::publish_variant:
      return do_publish(op);
    case op_kind::drop_in_place:
      return do_drop(op);
    case op_kind::call_into:
      return do_call(op);
    case op_kind::make_closure:
      return do_make_closure(op);
    case op_kind::release_environment:
      if (!fn_.signature.takes_environment) {
        return fail(verify_error_kind::bad_operation,
                    "only a closure body or its drop glue releases an "
                    "environment");
      }
      return {};
    case op_kind::storage_dead:
      return do_dead(op);
    case op_kind::return_value:
      return do_return(op);
    case op_kind::if_begin:
      return do_if_begin(op);
    case op_kind::else_begin:
      return do_else();
    case op_kind::if_end:
      return do_if_end();
    case op_kind::loop_begin:
      return open_region(region_kind::loop);
    case op_kind::loop_end:
      return do_loop_end();
    case op_kind::block_begin:
      return open_region(region_kind::block);
    case op_kind::block_end:
      return do_block_end();
    case op_kind::exit:
      return do_exit(op);
    }
    return {};
  }

  // ------------------------------------------------------------------
  //  Control flow
  // ------------------------------------------------------------------

  /// Everything the replay knows at one point of the function.
  struct flow_state {
    std::vector<init_node> state;
    std::vector<std::set<std::string>> dropped;
    std::vector<bool> dead;
  };
  using maybe_flow = std::optional<flow_state>;

  enum class region_kind : uint8_t { branch, loop, block };

  struct region {
    region_kind kind = region_kind::branch;
    flow_state entry;
    /// A branch: the state at the end of the `then` side, once `else` began.
    maybe_flow then_end;
    bool has_else = false;
    /// A loop or block: the join of the states at each `exit` that reaches it.
    maybe_flow exits;
  };

  [[nodiscard]] static auto continues_dead_path(op_kind kind) -> bool {
    return kind == op_kind::else_begin || kind == op_kind::if_end ||
           kind == op_kind::loop_end || kind == op_kind::block_end;
  }

  [[nodiscard]] auto snapshot() const -> flow_state {
    return flow_state{state_, dropped_, dead_};
  }

  auto restore(flow_state from) -> void {
    state_ = std::move(from.state);
    dropped_ = std::move(from.dropped);
    dead_ = std::move(from.dead);
  }

  [[nodiscard]] static auto intersect(const init_node &a, const init_node &b)
      -> init_node {
    if (a.whole && b.whole) {
      auto out = init_node{};
      out.whole = true;
      return out;
    }
    if (a.whole) {
      return b;
    }
    if (b.whole) {
      return a;
    }
    auto out = init_node{};
    for (const auto &[key, child] : a.children) {
      const auto other = b.children.find(key);
      if (other == b.children.end()) {
        continue;
      }
      auto both = intersect(child, other->second);
      if (!both.empty()) {
        out.children.emplace(key, std::move(both));
      }
    }
    return out;
  }

  /// The state where two paths meet. A value that owes a drop must be in the
  /// same state on both; anything else is reduced to what both paths know.
  [[nodiscard]] auto join(const flow_state &a, const flow_state &b)
      -> std::expected<flow_state, verify_error> {
    auto out = a;
    for (size_t i = 0; i < fn_.slots.size(); ++i) {
      const auto &slot = fn_.slots[i];
      const auto id = static_cast<storage_id>(i);
      const auto state_a = normalize(a.state[i], slot.type);
      const auto state_b = normalize(b.state[i], slot.type);
      const auto same = state_a == state_b && a.dropped[i] == b.dropped[i];
      if (same && a.dead[i] == b.dead[i]) {
        continue;
      }
      if (a.dead[i] != b.dead[i]) {
        const auto &live = a.dead[i] ? state_b : state_a;
        if (!live.empty()) {
          return fail(verify_error_kind::join_mismatch,
                      "s" + std::to_string(i) +
                          " ends on one path while still holding a value on "
                          "the other");
        }
        out.dead[i] = true;
        out.state[i] = init_node{};
        out.dropped[i].clear();
        continue;
      }
      if (env_.needs_drop(slot.type)) {
        const auto owes_a =
            first_owed(&state_a, slot.type, id, "", a.dropped[i]);
        const auto owes_b =
            first_owed(&state_b, slot.type, id, "", b.dropped[i]);
        if (owes_a.has_value() || owes_b.has_value()) {
          return fail(verify_error_kind::join_mismatch,
                      "s" + std::to_string(i) + " owes a drop on one path " +
                          "and not on the other; that needs a drop flag");
        }
        out.state[i] = init_node{}; // Neither path owes anything.
      } else {
        out.state[i] = intersect(state_a, state_b);
      }
      out.dropped[i].clear();
    }
    return out;
  }

  [[nodiscard]] auto join(const maybe_flow &a, const maybe_flow &b)
      -> std::expected<maybe_flow, verify_error> {
    if (!a.has_value()) {
      return b;
    }
    if (!b.has_value()) {
      return a;
    }
    auto joined = join(*a, *b);
    if (!joined.has_value()) {
      return std::unexpected(joined.error());
    }
    return maybe_flow{std::move(*joined)};
  }

  /// Continues from `where`, or marks the path unreachable.
  auto resume(maybe_flow where) -> void {
    reachable_ = where.has_value();
    if (where.has_value()) {
      restore(std::move(*where));
    }
  }

  [[nodiscard]] auto current() const -> maybe_flow {
    return reachable_ ? maybe_flow{snapshot()} : std::nullopt;
  }

  [[nodiscard]] auto open_region(region_kind kind) -> result {
    regions_.push_back(region{.kind = kind, .entry = snapshot()});
    return {};
  }

  [[nodiscard]] auto innermost(region_kind kind, std::string_view what)
      -> std::expected<region *, verify_error> {
    if (regions_.empty() || regions_.back().kind != kind) {
      return fail(verify_error_kind::bad_operation,
                  std::string{what} + " does not close the innermost region");
    }
    return &regions_.back();
  }

  [[nodiscard]] auto do_if_begin(const storage_op &op) -> result {
    if (auto ok = check_operand(op.condition); !ok.has_value()) {
      return ok;
    }
    return open_region(region_kind::branch);
  }

  [[nodiscard]] auto do_else() -> result {
    const auto top = innermost(region_kind::branch, "`else`");
    if (!top.has_value()) {
      return std::unexpected(top.error());
    }
    auto &reg = **top;
    if (reg.has_else) {
      return fail(verify_error_kind::bad_operation, "a second `else`");
    }
    reg.has_else = true;
    reg.then_end = current();
    resume(reg.entry);
    return {};
  }

  [[nodiscard]] auto do_if_end() -> result {
    const auto top = innermost(region_kind::branch, "`end_if`");
    if (!top.has_value()) {
      return std::unexpected(top.error());
    }
    auto reg = std::move(**top);
    regions_.pop_back();
    const auto joined = reg.has_else
                            ? join(reg.then_end, current())
                            : join(current(), maybe_flow{reg.entry});
    if (!joined.has_value()) {
      return std::unexpected(joined.error());
    }
    resume(*joined);
    return {};
  }

  /// A loop runs its body again from the top, so the state at the end of an
  /// iteration must be the state the loop was entered with. Storage that
  /// ended inside the body starts over on the next iteration.
  [[nodiscard]] auto do_loop_end() -> result {
    const auto top = innermost(region_kind::loop, "`end_loop`");
    if (!top.has_value()) {
      return std::unexpected(top.error());
    }
    auto reg = std::move(**top);
    regions_.pop_back();
    if (reachable_) {
      for (size_t i = 0; i < fn_.slots.size(); ++i) {
        if (dead_[i] && !reg.entry.dead[i]) {
          continue;
        }
        const auto type = fn_.slots[i].type;
        if (dead_[i] != reg.entry.dead[i] ||
            normalize(state_[i], type) != normalize(reg.entry.state[i], type) ||
            dropped_[i] != reg.entry.dropped[i]) {
          return fail(verify_error_kind::join_mismatch,
                      "s" + std::to_string(i) +
                          " is in a different state at the end of the loop "
                          "body than at its start");
        }
      }
    }
    resume(reg.exits);
    return {};
  }

  [[nodiscard]] auto do_block_end() -> result {
    const auto top = innermost(region_kind::block, "`end_block`");
    if (!top.has_value()) {
      return std::unexpected(top.error());
    }
    auto reg = std::move(**top);
    regions_.pop_back();
    const auto joined = join(current(), reg.exits);
    if (!joined.has_value()) {
      return std::unexpected(joined.error());
    }
    resume(*joined);
    return {};
  }

  [[nodiscard]] auto do_exit(const storage_op &op) -> result {
    auto seen = size_t{0};
    for (auto it = regions_.rbegin(); it != regions_.rend(); ++it) {
      if (it->kind == region_kind::branch) {
        continue;
      }
      if (seen++ != op.depth) {
        continue;
      }
      const auto joined = join(it->exits, current());
      if (!joined.has_value()) {
        return std::unexpected(joined.error());
      }
      it->exits = *joined;
      reachable_ = false;
      return {};
    }
    return fail(verify_error_kind::bad_operation,
                "`exit " + std::to_string(op.depth) +
                    "` names a loop or block that is not open");
  }

  /// A raw pointer is retyped freely (`ptr_cast`): every one has the same
  /// representation, so a copy between two of them changes only the type.
  [[nodiscard]] auto is_raw_pointer(type_id id) -> bool {
    return types().entry(types().strip_refinement(id)).kind ==
           semantic::type_kind::ptr_kind;
  }

  [[nodiscard]] auto do_compute(const storage_op &op) -> result {
    const auto at = resolve(op.dest);
    if (!at.has_value()) {
      return std::unexpected(at.error());
    }
    const auto cls = classify_value(env_.layouts, op.dest.type);
    if (!cls.has_value()) {
      return fail(verify_error_kind::layout_failed, cls.error().message);
    }
    if (cls->cls != pass_class::scalar && cls->cls != pass_class::address) {
      return fail(verify_error_kind::type_mismatch,
                  "compute writes a scalar or an address, not " +
                      name_of(op.dest.type));
    }
    const auto expected_operands =
        op.computation == compute_kind::constant ? 0U
        : op.computation == compute_kind::binary ||
                  op.computation == compute_kind::pointer_offset
            ? 2U
            : 1U;
    if (op.operands.size() != expected_operands) {
      return fail(verify_error_kind::bad_operation,
                  "compute has " + std::to_string(op.operands.size()) +
                      " operands, expected " +
                      std::to_string(expected_operands));
    }
    for (const auto &value : op.operands) {
      if (auto ok = check_operand(value); !ok.has_value()) {
        return ok;
      }
    }
    if (op.computation == compute_kind::copy &&
        !same_type(op.operands[0].type, op.dest.type) &&
        !(is_raw_pointer(op.operands[0].type) && is_raw_pointer(op.dest.type))) {
      return fail(verify_error_kind::type_mismatch,
                  "a copy joins " + name_of(op.operands[0].type) + " and " +
                      name_of(op.dest.type));
    }
    if (op.computation == compute_kind::pointer_offset) {
      if (!is_raw_pointer(op.operands[0].type) ||
          !is_raw_pointer(op.dest.type)) {
        return fail(verify_error_kind::type_mismatch,
                    "an offset advances a raw pointer and gives one, not " +
                        name_of(op.operands[0].type) + " to " +
                        name_of(op.dest.type));
      }
    }
    if (auto ok = require_writable(op.dest, *at); !ok.has_value()) {
      return ok;
    }
    commit_write(op.dest, *at);
    return {};
  }

  [[nodiscard]] auto do_init_empty(const storage_op &op) -> result {
    const auto at = resolve(op.dest);
    if (!at.has_value()) {
      return std::unexpected(at.error());
    }
    const auto layout = layout_of(op.dest.type);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    if (!(*layout)->is_zero_sized()) {
      return fail(verify_error_kind::type_mismatch,
                  "init_empty on " + name_of(op.dest.type) +
                      ", which has bytes");
    }
    if (auto ok = require_writable(op.dest, *at); !ok.has_value()) {
      return ok;
    }
    commit_write(op.dest, *at);
    return {};
  }

  [[nodiscard]] auto do_transfer(const storage_op &op) -> result {
    const auto moving = op.kind == op_kind::move_into;
    const auto from = resolve(op.source);
    if (!from.has_value()) {
      return std::unexpected(from.error());
    }
    const auto to = resolve(op.dest);
    if (!to.has_value()) {
      return std::unexpected(to.error());
    }
    if (!same_type(op.source.type, op.dest.type)) {
      return fail(verify_error_kind::type_mismatch,
                  std::string{moving ? "move" : "copy"} + " joins " +
                      name_of(op.source.type) + " and " +
                      name_of(op.dest.type));
    }
    if (overlaps(op.source, op.dest)) {
      return fail(verify_error_kind::overlapping_transfer,
                  "source and destination share bytes");
    }
    if (auto ok = require_init(op.source, *from); !ok.has_value()) {
      return ok;
    }
    if (moving) {
      if (from->through_deref && !from->through_raw) {
        return fail(verify_error_kind::move_from_reference,
                    "cannot move " + name_of(op.source.type) +
                        " out of a reference");
      }
      if (from->captured && !from->through_deref) {
        return fail(verify_error_kind::move_from_reference,
                    "cannot move " + name_of(op.source.type) +
                        " out of a closure's environment");
      }
    } else if (!env_.is_copy(op.source.type)) {
      return fail(verify_error_kind::not_copy,
                  name_of(op.source.type) +
                      " is moved, not copied; use a move or an explicit clone");
    }
    if (auto ok = require_writable(op.dest, *to); !ok.has_value()) {
      return ok;
    }
    if (moving) {
      if (auto ok = invalidate(op.source, *from, from->tracked); !ok.has_value()) {
        return ok;
      }
    }
    commit_write(op.dest, *to);
    return {};
  }

  [[nodiscard]] auto do_publish(const storage_op &op) -> result {
    const auto at = resolve(op.dest);
    if (!at.has_value()) {
      return std::unexpected(at.error());
    }
    if (at->through_deref) {
      return fail(verify_error_kind::bad_operation,
                  "a sum is built in its own storage, not behind a reference");
    }
    const auto layout = layout_of(op.dest.type);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto &found = **layout;
    if (found.kind != runtime::value_layout_kind::sum) {
      return fail(verify_error_kind::type_mismatch,
                  name_of(op.dest.type) + " is not a sum");
    }
    const auto *variant = found.find_variant(op.variant);
    if (variant == nullptr) {
      return fail(verify_error_kind::bad_place,
                  name_of(op.dest.type) + " has no variant `" + op.variant +
                      "`");
    }
    auto *node = &state_[op.dest.base];
    for (size_t i = 0; i < at->tracked; ++i) {
      if (node->whole) {
        break;
      }
      const auto child = node->children.find(step_key(op.dest.steps[i]));
      if (child == node->children.end()) {
        node = nullptr;
        break;
      }
      node = &child->second;
    }
    if (node != nullptr && node->whole) {
      return fail(verify_error_kind::already_initialized,
                  "the sum's variant is already published");
    }
    for (size_t i = 0; i < variant->payload.size(); ++i) {
      const auto child =
          node == nullptr ? std::map<std::string, init_node>::const_iterator{}
                          : node->children.find(payload_key(op.variant, i));
      if (node == nullptr || child == node->children.end() ||
          !covered(child->second, variant->payload[i].type)) {
        return fail(verify_error_kind::incomplete_variant,
                    "payload slot " + std::to_string(i) + " of `" + op.variant +
                        "` is not initialized when the variant is published");
      }
    }
    if (node != nullptr && node->children.size() != variant->payload.size()) {
      return fail(verify_error_kind::incomplete_variant,
                  "payload of another variant is still present");
    }
    mark_init(op.dest, at->tracked);
    return {};
  }

  [[nodiscard]] auto do_drop(const storage_op &op) -> result {
    const auto at = resolve(op.dest);
    if (!at.has_value()) {
      return std::unexpected(at.error());
    }
    if (auto ok = require_init(op.dest, *at, true); !ok.has_value()) {
      return ok;
    }
    if (at->through_deref) {
      return {}; // The referent's owner tracks what it holds.
    }
    forget_drops(op.dest, at->tracked);
    return invalidate(op.dest, *at, at->tracked);
  }

  /// A synthesized `drop` call: its one argument borrows a value this
  /// function owns, and the call runs that value's own drop once. The value
  /// stays in place, because its droppable fields are dropped by later calls.
  [[nodiscard]] auto check_drop_call(const storage_op &op) -> result {
    if (!op.callee.is_direct() || op.args.size() != 1 ||
        op.args[0].cls != pass_class::address ||
        op.args[0].value.kind != operand_kind::address_of) {
      return fail(verify_error_kind::bad_argument,
                  "a drop call takes the address of one value");
    }
    const auto &target = op.args[0].value.where;
    const auto at = resolve(target);
    if (!at.has_value()) {
      return std::unexpected(at.error());
    }
    if (at->through_deref) {
      return fail(verify_error_kind::move_from_reference,
                  "a drop of bytes behind a reference; their owner drops them");
    }
    if (auto ok = require_init(target, *at, true); !ok.has_value()) {
      return ok;
    }
    const auto *plan = env_.drop_plan(target.type);
    if (plan == nullptr || !plan->own_drop.has_value()) {
      return fail(verify_error_kind::bad_operation,
                  name_of(target.type) + " has no drop of its own to call");
    }
    const auto path = path_key(target, at->tracked);
    if (!dropped_[target.base].insert(path).second) {
      return fail(verify_error_kind::double_drop,
                  describe(target, at->tracked) + " is dropped twice");
    }
    return {};
  }

  [[nodiscard]] auto do_call(const storage_op &op) -> result {
    const auto &sig = op.signature;
    if (sig.params.size() != op.args.size()) {
      return fail(verify_error_kind::bad_argument,
                  "call passes " + std::to_string(op.args.size()) +
                      " arguments to a signature of " +
                      std::to_string(sig.params.size()));
    }
    if (op.callee.is_direct()) {
      // A function in another module may share this module's name for it.
      if (callees_ != nullptr &&
          (!op.callee.owner_module.has_value() ||
           *op.callee.owner_module == callees_->name)) {
        const auto *target = callees_->find(op.callee.name);
        if (target != nullptr && !target->signature.agrees_with(sig)) {
          return fail(verify_error_kind::signature_mismatch,
                      "the call to `" + op.callee.name + "` is " +
                          format_signature(sig) + " but the function is " +
                          format_signature(target->signature));
        }
      }
    } else {
      const auto &callable = *op.callee.callable;
      const auto at = resolve(callable);
      if (!at.has_value()) {
        return std::unexpected(at.error());
      }
      if (auto ok = require_init(callable, *at); !ok.has_value()) {
        return ok;
      }
      const auto indirect = classify_fn_type(env_.layouts, types(), callable.type);
      if (!indirect.has_value()) {
        return fail(verify_error_kind::signature_mismatch,
                    indirect.error().message);
      }
      if (!indirect->agrees_with(sig)) {
        return fail(verify_error_kind::signature_mismatch,
                    "the call is " + format_signature(sig) +
                        " but the function value is " +
                        format_signature(*indirect));
      }
    }
    const auto dest = resolve(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    if (!same_type(op.dest.type, sig.result.type)) {
      return fail(verify_error_kind::type_mismatch,
                  "the call returns " + name_of(sig.result.type) +
                      " but its destination is " + name_of(op.dest.type));
    }
    if (op.is_drop) {
      if (auto ok = check_drop_call(op); !ok.has_value()) {
        return ok;
      }
    }
    auto used = std::unordered_set<storage_id>{};
    for (size_t i = 0; i < op.args.size(); ++i) {
      const auto &arg = op.args[i];
      if (arg.source_index != i || arg.cls != sig.params[i].cls) {
        return fail(verify_error_kind::bad_argument,
                    "argument " + std::to_string(i) +
                        " does not match the signature");
      }
      if (arg.cls == pass_class::zero_sized) {
        continue;
      }
      if (is_indirect(arg.cls)) {
        const auto at = resolve(arg.slot);
        if (!at.has_value()) {
          return std::unexpected(at.error());
        }
        if (fn_.slots[arg.slot.base].cls != storage_class::outgoing_argument ||
            !arg.slot.steps.empty() || !used.insert(arg.slot.base).second ||
            !same_type(arg.slot.type, sig.params[i].type)) {
          return fail(verify_error_kind::bad_argument,
                      "argument " + std::to_string(i) +
                          " must be its own outgoing slot of its type");
        }
        if (auto ok = require_init(arg.slot, *at); !ok.has_value()) {
          return ok;
        }
        if (auto ok = invalidate(arg.slot, *at, at->tracked); !ok.has_value()) {
          return ok;
        }
        continue;
      }
      if (!op.is_drop) { // `check_drop_call` read the borrowed value.
        if (auto ok = check_operand(arg.value); !ok.has_value()) {
          return ok;
        }
      }
      const auto cls = classify_value(env_.layouts, arg.value.type);
      if (!cls.has_value() || cls->cls != arg.cls) {
        return fail(verify_error_kind::bad_argument,
                    "argument " + std::to_string(i) + " is not a " +
                        pass_class_name(arg.cls));
      }
      // A callee that builds its result in caller storage writes it while it
      // runs, so a borrow of that storage would see it half built. A result
      // returned in a register is written after the call and cannot clash.
      if (sig.returns_through_address() &&
          arg.value.kind == operand_kind::address_of &&
          overlaps(arg.value.where, op.dest)) {
        return fail(verify_error_kind::result_aliases_argument,
                    "the result is built in s" + std::to_string(op.dest.base) +
                        ", which an argument borrows");
      }
    }
    if (auto ok = require_writable(op.dest, *dest); !ok.has_value()) {
      return ok;
    }
    commit_write(op.dest, *dest);
    return {};
  }

  /// Builds a `fn` value in `dest`. The body and drop glue are functions of the
  /// module that take the environment first; every captured variable is filled
  /// into the environment block at an offset that suits its type.
  [[nodiscard]] auto do_make_closure(const storage_op &op) -> result {
    const auto dest = resolve(op.dest);
    if (!dest.has_value()) {
      return std::unexpected(dest.error());
    }
    if (dest->through_deref ||
        types().entry(types().strip_refinement(op.dest.type)).kind !=
            semantic::type_kind::fn_kind) {
      return fail(verify_error_kind::type_mismatch,
                  "a closure is built in a `fn` value of its own storage, not " +
                      name_of(op.dest.type));
    }
    if (callees_ != nullptr) {
      const auto indirect =
          classify_fn_type(env_.layouts, types(), op.dest.type);
      if (!indirect.has_value()) {
        return fail(verify_error_kind::signature_mismatch,
                    indirect.error().message);
      }
      const auto *entry = callees_->find(op.entry);
      if (entry == nullptr || !entry->signature.takes_environment ||
          !entry->signature.agrees_with(*indirect)) {
        return fail(verify_error_kind::signature_mismatch,
                    "`" + op.entry + "` is not a closure body of " +
                        name_of(op.dest.type));
      }
      if (op.env_size != 0) {
        const auto *glue = callees_->find(op.glue);
        if (glue == nullptr || !glue->signature.takes_environment ||
            !glue->signature.params.empty() ||
            glue->signature.result.cls != pass_class::zero_sized) {
          return fail(verify_error_kind::signature_mismatch,
                      "`" + op.glue +
                          "` is not drop glue taking only an environment");
        }
      }
    }
    if (op.env_size == 0 && (!op.glue.empty() || !op.captures.empty())) {
      return fail(verify_error_kind::bad_operation,
                  "a closure with an empty environment has no glue and no "
                  "captures");
    }
    for (const auto &capture : op.captures) {
      const auto at = resolve(capture.source);
      if (!at.has_value()) {
        return std::unexpected(at.error());
      }
      const auto layout = layout_of(capture.source.type);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      const auto size = capture.kind == capture_kind::reference
                            ? runtime::k_word_size
                            : (*layout)->size;
      const auto align = capture.kind == capture_kind::reference
                             ? runtime::k_word_size
                             : (*layout)->align;
      if (capture.offset % align != 0 || capture.offset + size > op.env_size ||
          align > op.env_align) {
        return fail(verify_error_kind::bad_operation,
                    "a capture of " + name_of(capture.source.type) + " at " +
                        std::to_string(capture.offset) +
                        " does not fit the environment of " +
                        std::to_string(op.env_size) + " bytes");
      }
      if (capture.kind == capture_kind::reference) {
        continue; // Only the variable's address is taken; the owner keeps it.
      }
      if (auto ok = require_init(capture.source, *at); !ok.has_value()) {
        return ok;
      }
      if (capture.kind == capture_kind::move) {
        if (at->through_deref && !at->through_raw) {
          return fail(verify_error_kind::move_from_reference,
                      "cannot move " + name_of(capture.source.type) +
                          " out of a reference into a closure");
        }
        if (at->captured) {
          return fail(verify_error_kind::move_from_reference,
                      "cannot move a captured value into another closure");
        }
        if (!at->through_deref) {
          if (auto ok = invalidate(capture.source, *at, at->tracked);
              !ok.has_value()) {
            return ok;
          }
        }
      }
    }
    if (auto ok = require_writable(op.dest, *dest); !ok.has_value()) {
      return ok;
    }
    commit_write(op.dest, *dest);
    return {};
  }

  [[nodiscard]] auto do_dead(const storage_op &op) -> result {
    if (op.slot >= fn_.slots.size()) {
      return fail(verify_error_kind::bad_slot,
                  "storage_dead names slot s" + std::to_string(op.slot));
    }
    const auto &slot = fn_.slots[op.slot];
    if (dead_[op.slot]) {
      return fail(verify_error_kind::use_after_dead,
                  "slot s" + std::to_string(op.slot) + " ends twice");
    }
    if (slot.cls != storage_class::frame_local &&
        slot.cls != storage_class::outgoing_argument &&
        slot.cls != storage_class::parameter) {
      return fail(verify_error_kind::bad_lifetime_end,
                  std::string{"a "} + storage_class_name(slot.cls) +
                      " slot does not end inside the function");
    }
    const auto holds = !state_[op.slot].empty();
    if (slot.cls == storage_class::outgoing_argument && holds) {
      return fail(verify_error_kind::bad_lifetime_end,
                  "outgoing slot s" + std::to_string(op.slot) +
                      " still holds an argument no call consumed");
    }
    if (options_.check_leaks && holds && env_.needs_drop(slot.type)) {
      if (const auto owed =
              first_owed(&state_[op.slot], slot.type, op.slot, "",
                         dropped_[op.slot]);
          owed.has_value()) {
        return fail(verify_error_kind::leaked_value,
                    "slot s" + std::to_string(op.slot) +
                        " ends while " + *owed + " still owes its drop");
      }
    }
    dead_[op.slot] = true;
    state_[op.slot] = init_node{};
    dropped_[op.slot].clear();
    return {};
  }

  [[nodiscard]] auto do_return(const storage_op &op) -> result {
    const auto &sig = fn_.signature;
    if (sig.returns_through_address()) {
      if (op.value.has_value()) {
        return fail(verify_error_kind::bad_result,
                    "a result built in caller storage returns no value");
      }
      if (dead_[fn_.result_slot] || !covered(state_[fn_.result_slot], sig.result.type)) {
        return fail(verify_error_kind::bad_result,
                    "the result is not completely initialized on return");
      }
    } else if (sig.result.cls == pass_class::zero_sized) {
      if (op.value.has_value()) {
        return fail(verify_error_kind::bad_result,
                    "a zero-sized result returns no value");
      }
    } else {
      if (!op.value.has_value()) {
        return fail(verify_error_kind::bad_result,
                    "return carries no value for a " +
                        std::string{pass_class_name(sig.result.cls)} +
                        " result");
      }
      if (auto ok = check_operand(*op.value); !ok.has_value()) {
        return ok;
      }
      if (!same_type(op.value->type, sig.result.type)) {
        return fail(verify_error_kind::bad_result,
                    "return carries " + name_of(op.value->type) +
                        " for a result of " + name_of(sig.result.type));
      }
    }
    for (const auto &slot : fn_.slots) {
      if (dead_[slot.id] || slot.id == fn_.result_slot) {
        continue;
      }
      if (slot.cls == storage_class::outgoing_argument &&
          !state_[slot.id].empty()) {
        return fail(verify_error_kind::bad_lifetime_end,
                    "outgoing slot s" + std::to_string(slot.id) +
                        " still holds an argument no call consumed");
      }
      if (options_.check_leaks &&
          (slot.cls == storage_class::frame_local ||
           slot.cls == storage_class::parameter) &&
          !state_[slot.id].empty() && env_.needs_drop(slot.type)) {
        if (const auto owed =
                first_owed(&state_[slot.id], slot.type, slot.id, "",
                           dropped_[slot.id]);
            owed.has_value()) {
          return fail(verify_error_kind::leaked_value,
                      *owed + " still owes its drop at return");
        }
      }
    }
    reachable_ = false;
    return {};
  }

  const storage_function &fn_;
  storage_env &env_;
  const storage_module *callees_;
  verify_options options_;
  std::vector<init_node> state_;
  /// Per slot, the places whose own drop has run. A drop call leaves the
  /// bytes in place, because the fields still need their drops, so this is
  /// kept apart from `state_`.
  std::vector<std::set<std::string>> dropped_;
  std::vector<bool> dead_;
  size_t op_index_ = static_cast<size_t>(-1);
  source_span span_;
  bool reachable_ = true;
  std::vector<region> regions_;
};

} // namespace

auto verify_error_kind_name(verify_error_kind kind) -> const char * {
  switch (kind) {
  case verify_error_kind::bad_slot:
    return "bad_slot";
  case verify_error_kind::bad_place:
    return "bad_place";
  case verify_error_kind::type_mismatch:
    return "type_mismatch";
  case verify_error_kind::bad_operation:
    return "bad_operation";
  case verify_error_kind::uninitialized:
    return "uninitialized";
  case verify_error_kind::already_initialized:
    return "already_initialized";
  case verify_error_kind::move_from_reference:
    return "move_from_reference";
  case verify_error_kind::partial_move:
    return "partial_move";
  case verify_error_kind::not_copy:
    return "not_copy";
  case verify_error_kind::incomplete_variant:
    return "incomplete_variant";
  case verify_error_kind::overlapping_transfer:
    return "overlapping_transfer";
  case verify_error_kind::signature_mismatch:
    return "signature_mismatch";
  case verify_error_kind::bad_argument:
    return "bad_argument";
  case verify_error_kind::result_aliases_argument:
    return "result_aliases_argument";
  case verify_error_kind::use_after_dead:
    return "use_after_dead";
  case verify_error_kind::bad_lifetime_end:
    return "bad_lifetime_end";
  case verify_error_kind::bad_result:
    return "bad_result";
  case verify_error_kind::leaked_value:
    return "leaked_value";
  case verify_error_kind::join_mismatch:
    return "join_mismatch";
  case verify_error_kind::double_drop:
    return "double_drop";
  case verify_error_kind::use_after_drop:
    return "use_after_drop";
  case verify_error_kind::missing_return:
    return "missing_return";
  case verify_error_kind::code_after_return:
    return "code_after_return";
  case verify_error_kind::layout_failed:
    return "layout_failed";
  }
  return "?";
}

auto verify(const storage_function &function, storage_env &env,
            const storage_module *callees, const verify_options &options)
    -> std::expected<void, verify_error> {
  return replay(function, env, callees, options).run();
}

auto verify(const storage_module &module, storage_env &env,
            const verify_options &options)
    -> std::expected<void, verify_error> {
  for (const auto &function : module.functions) {
    if (auto ok = verify(function, env, &module, options); !ok.has_value()) {
      return ok;
    }
  }
  return {};
}

} // namespace cinder::hir

