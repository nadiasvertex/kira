#include "src/core/verify.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/core/intrinsics.h"
#include "src/semantic/aggregate_shape.h"
#include "src/semantic/linear_poly.h"

/// The core verifier. Every rule in `verify.h` is checked by the function
/// named after its group: `check_type` (T), `check_place`/`walk_steps` (P),
/// `check_use`/`check_borrow`/`check_move` (A), `check_call`/`check_callee`
/// (C), the `check_*` expression functions (E), `check_stmt` (S), and
/// `check_function`/`check_static` (F). A malformed node (a null child, or a
/// node whose kind does not match its class) is reported as "F0".
namespace cinder::core {
namespace {

using semantic::field_shape;
using semantic::type_entry;
using semantic::type_kind;
using semantic::variant_shape;

/// Types nest only a few levels deep; anything deeper is a cycle.
constexpr int k_max_type_depth = 64;

/// How a local came into scope. Decides P4 at the root, A2 for a move from
/// it, and S2's "a whole local whose `let` had no value".
enum class local_kind : std::uint8_t {
  param,
  let_with_value,
  let_without_value,
  binding,
  alias_binding,  ///< An `alias` pattern binding: a place alias.
  capture_value,  ///< A `copy` or `move` capture.
  capture_borrow, ///< A `borrow` or `borrow_mut` capture: a place alias.
};

struct local {
  symbol_id symbol = semantic::k_invalid_symbol_id;
  type_id type = k_unknown_type;
  writability root = writability::immutable_binding;
  local_kind kind = local_kind::let_with_value;
};

/// What walking a place found: its type, its writability (P4), and why a
/// move out of it would break A2 (empty when a move is allowed). `valid` is
/// false once a step could not be followed; nothing after it is checked.
struct place_facts {
  type_id type = k_unknown_type;
  writability access = writability::writable;
  std::string_view move_block = {};
  bool valid = true;
};

/// The function or lambda body being checked: what `return` and `yield`
/// take, and how many loops enclose the current point.
struct body_frame {
  type_id return_type = k_unknown_type;
  bool generator = false;
  type_id yield_type = k_unknown_type;
  int loops = 0;
};

/// The first offending sub-type for each of T1, T2 and T3.
using type_problems = std::array<std::optional<type_id>, 3>;

[[nodiscard]] auto describe(writability access) -> std::string_view {
  switch (access) {
  case writability::writable:
    return "writable";
  case writability::immutable_binding:
    return "an immutable binding";
  case writability::behind_shared_ref:
    return "behind a shared reference";
  case writability::static_value:
    return "a static";
  }
  return "of unknown writability";
}

[[nodiscard]] auto describe(receiver_mode mode) -> std::string_view {
  switch (mode) {
  case receiver_mode::none:
    return "none";
  case receiver_mode::shared:
    return "shared";
  case receiver_mode::exclusive:
    return "exclusive";
  case receiver_mode::by_value:
    return "by_value";
  case receiver_mode::consuming:
    return "consuming";
  }
  return "unknown";
}

/// The node as its concrete class, or null when the kind lies about it.
template <typename T> [[nodiscard]] auto as(const node &n) -> const T * {
  return dynamic_cast<const T *>(&n);
}

[[nodiscard]] auto has_break(const node *n, bool in_loop) -> bool;

[[nodiscard]] auto has_break_in_place(const place &p, bool in_loop) -> bool {
  if (has_break(p.root.value.get(), in_loop)) {
    return true;
  }
  return std::ranges::any_of(
      p.path, [&](const projection &s) { return has_break(s.subscript.get(), in_loop); });
}

/// Whether `n` holds a `break` that leaves the loop being examined. `in_loop`
/// is true inside a loop nested in it, whose `break`s belong to that loop. A
/// `break` cannot cross a lambda, so lambda bodies are not entered.
[[nodiscard]] auto has_break(const node *n, bool in_loop) -> bool {
  if (n == nullptr) {
    return false;
  }
  const auto any = [&](const auto &nodes) {
    return std::ranges::any_of(nodes, [&](const auto &c) { return has_break(c.get(), in_loop); });
  };
  switch (n->kind) {
  case node_kind::break_stmt:
    return !in_loop;
  case node_kind::literal:
  case node_kind::function_ref:
  case node_kind::lambda:
  case node_kind::continue_stmt:
    return false;
  case node_kind::use:
    return has_break_in_place(static_cast<const use *>(n)->from, in_loop);
  case node_kind::borrow:
    return has_break_in_place(static_cast<const borrow *>(n)->of, in_loop);
  case node_kind::call: {
    const auto *c = static_cast<const call *>(n);
    return has_break(c->target.value.get(), in_loop) || any(c->args);
  }
  case node_kind::primitive:
    return any(static_cast<const primitive *>(n)->operands);
  case node_kind::cast:
    return has_break(static_cast<const cast *>(n)->operand.get(), in_loop);
  case node_kind::tuple_init:
  case node_kind::array_init:
  case node_kind::struct_init:
    return any(static_cast<const aggregate_init *>(n)->elements);
  case node_kind::variant_init:
    return any(static_cast<const variant_init *>(n)->payload);
  case node_kind::block: {
    const auto *b = static_cast<const block *>(n);
    return any(b->statements) || has_break(b->result.get(), in_loop);
  }
  case node_kind::if_expr: {
    const auto *i = static_cast<const if_expr *>(n);
    return has_break(i->condition.get(), in_loop) || has_break(i->then_branch.get(), in_loop) ||
           has_break(i->else_branch.get(), in_loop);
  }
  case node_kind::match_expr: {
    const auto *m = static_cast<const match_expr *>(n);
    return has_break_in_place(m->subject, in_loop) ||
           std::ranges::any_of(m->arms, [&](const match_arm &arm) {
             return has_break(arm.guard.get(), in_loop) || has_break(arm.body.get(), in_loop);
           });
  }
  case node_kind::await_expr:
    return has_break(static_cast<const await_expr *>(n)->task.get(), in_loop);
  case node_kind::let:
    return has_break(static_cast<const let *>(n)->value.get(), in_loop);
  case node_kind::assign: {
    const auto *a = static_cast<const assign *>(n);
    return has_break_in_place(a->target, in_loop) || has_break(a->value.get(), in_loop);
  }
  case node_kind::expr_stmt:
    return has_break(static_cast<const expr_stmt *>(n)->value.get(), in_loop);
  case node_kind::while_loop: {
    const auto *w = static_cast<const while_loop *>(n);
    return has_break(w->condition.get(), in_loop) || has_break(w->body.get(), true) ||
           has_break(w->step.get(), true);
  }
  case node_kind::return_stmt:
  case node_kind::yield_stmt:
    return has_break(static_cast<const jump *>(n)->value.get(), in_loop);
  case node_kind::contract_check:
    return has_break(static_cast<const contract_check *>(n)->condition.get(), in_loop);
  }
  return false;
}

/// A loop with no condition, or the literal `true`, and no `break` of its own.
[[nodiscard]] auto never_exits(const semantic::type_table &types, const while_loop &w) -> bool {
  if (w.condition != nullptr) {
    const auto *lit = as<literal>(*w.condition);
    if (lit == nullptr || w.condition->kind != node_kind::literal || lit->spelling != "true" ||
        lit->type != types.bool_type()) {
      return false;
    }
  }
  return !has_break(w.body.get(), false) && !has_break(w.step.get(), false);
}

/// The one definition of "diverges" (`nodes.h`, `expr`).
[[nodiscard]] auto diverges(const semantic::type_table &types, const expr *e) -> bool {
  if (e == nullptr) {
    return false;
  }
  if (const auto &entry = types.entry(e->type);
      entry.kind == semantic::type_kind::builtin_kind && entry.name == "never") {
    return true;
  }
  if (const auto *b = as<block>(*e); b != nullptr && e->kind == node_kind::block) {
    if (b->result != nullptr) {
      return diverges(types, b->result.get());
    }
    if (b->statements.empty() || b->statements.back() == nullptr) {
      return false;
    }
    const auto &last = *b->statements.back();
    if (last.kind == node_kind::return_stmt || last.kind == node_kind::break_stmt ||
        last.kind == node_kind::continue_stmt) {
      return true;
    }
    if (const auto *s = as<expr_stmt>(last); s != nullptr && last.kind == node_kind::expr_stmt) {
      return diverges(types, s->value.get());
    }
    if (const auto *w = as<while_loop>(last); w != nullptr && last.kind == node_kind::while_loop) {
      return never_exits(types, *w);
    }
    return false;
  }
  if (const auto *i = as<if_expr>(*e); i != nullptr && e->kind == node_kind::if_expr) {
    return i->else_branch != nullptr && diverges(types, i->then_branch.get()) &&
           diverges(types, i->else_branch.get());
  }
  if (const auto *m = as<match_expr>(*e); m != nullptr && e->kind == node_kind::match_expr) {
    if (m->arms.empty()) {
      return false;
    }
    for (const auto &arm : m->arms) {
      if (!diverges(types, arm.body.get())) {
        return false;
      }
    }
    return true;
  }
  return false;
}

class verifier {
public:
  verifier(std::span<const module *const> program, const semantic::checked_types &checked)
      : program_(program), checked_(checked), types_(checked.types) {
    for (const auto *m : program_) {
      if (m == nullptr) {
        continue;
      }
      for (const auto &s : m->statics) {
        statics_.emplace(static_key(m->name, s.name), s.type);
      }
      for (const auto &f : m->functions) {
        if (f != nullptr && f->decl != nullptr) {
          functions_.emplace(f->decl, f.get());
        }
      }
      for (const auto &d : m->intrinsics) {
        declared_intrinsics_.emplace(d.name, &d);
      }
    }
  }

  auto run() -> std::vector<violation> {
    for (const auto *m : program_) {
      if (m == nullptr) {
        report("F0", {}, "the program holds a null module");
        continue;
      }
      module_ = m->name;
      for (const auto &f : m->functions) {
        if (f == nullptr) {
          function_.clear();
          report("F0", {}, "the module holds a null function");
          continue;
        }
        check_function(*f);
      }
      for (const auto &s : m->statics) {
        check_static(s);
      }
    }
    return std::move(violations_);
  }

private:
  std::span<const module *const> program_;
  const semantic::checked_types &checked_;
  const semantic::type_table &types_;
  std::unordered_map<std::string, type_id> statics_;
  std::unordered_multimap<const ast::func_decl *, const function *> functions_;
  std::unordered_multimap<std::string, const intrinsic_decl *> declared_intrinsics_;

  std::vector<violation> violations_;
  std::string module_;
  std::string function_;
  std::unordered_set<type_id> generic_params_;
  std::unordered_set<symbol_id> introduced_; ///< F1, per function.
  std::vector<local> env_;
  body_frame frame_;

  // ------------------------------------------------------------------------
  //  Reporting and type queries
  // ------------------------------------------------------------------------

  static auto static_key(std::string_view module_name, std::string_view name) -> std::string {
    return std::format("{}\x1f{}", module_name, name);
  }

  auto report(std::string_view rule, source_span span, std::string message) -> void {
    violations_.push_back(violation{.rule = std::string(rule),
                                    .module = module_,
                                    .function = function_,
                                    .span = span,
                                    .message = std::move(message)});
  }

  /// Reports `rule` when `found` is not `expected`.
  auto expect_type(std::string_view rule, source_span span, type_id found, type_id expected,
                   std::string_view what) -> void {
    if (found != expected) {
      report(rule, span,
             std::format("{} has type `{}`, expected `{}`", what, show(found), show(expected)));
    }
  }

  [[nodiscard]] auto show(type_id id) const -> std::string { return types_.display(id); }
  [[nodiscard]] auto entry(type_id id) const -> const type_entry & { return types_.entry(id); }

  [[nodiscard]] auto is_builtin(type_id id, std::string_view name) const -> bool {
    const auto &e = entry(id);
    return e.kind == type_kind::builtin_kind && e.name == name;
  }
  [[nodiscard]] auto is_generic(type_id id, std::string_view name) const -> bool {
    const auto &e = entry(id);
    return e.kind == type_kind::builtin_generic_kind && e.name == name;
  }
  [[nodiscard]] auto is_unit(type_id id) const -> bool { return is_builtin(id, "unit"); }
  [[nodiscard]] auto is_bool(type_id id) const -> bool { return id == types_.bool_type(); }
  [[nodiscard]] auto is_integer(type_id id) const -> bool {
    return entry(id).kind == type_kind::builtin_kind && types_.is_integer(id);
  }
  /// A builtin scalar: an integer, a float, `bool` or `char`.
  [[nodiscard]] auto is_scalar(type_id id) const -> bool {
    return entry(id).kind == type_kind::builtin_kind &&
           (types_.is_numeric(id) || is_bool(id) || is_builtin(id, "char"));
  }
  [[nodiscard]] auto is_raw_pointer(type_id id) const -> bool {
    return entry(id).kind == type_kind::ptr_kind;
  }

  /// The recorded shape of `id`: `checked.shapes`, then `open_shapes` for a
  /// type that mentions a generic parameter. Null when neither has one.
  [[nodiscard]] auto shape_of(type_id id) const -> const semantic::aggregate_shape * {
    if (const auto found = checked_.shapes.find(id); found != checked_.shapes.end()) {
      return &found->second;
    }
    if (const auto found = checked_.open_shapes.find(id); found != checked_.open_shapes.end()) {
      return &found->second;
    }
    return nullptr;
  }

  /// A struct's fields, from the recorded shape; null when none is recorded.
  [[nodiscard]] auto fields_of(type_id id) const -> const std::vector<field_shape> * {
    if (entry(id).kind != type_kind::struct_kind) {
      return nullptr;
    }
    const auto *shape = shape_of(id);
    return shape == nullptr ? nullptr : &shape->fields;
  }

  /// A sum's variants. From `checked.shapes` when recorded; for `option` and
  /// `result` without a shape, from the type's arguments in the order the
  /// checker records them (`some`, `none`; `ok`, `err`).
  [[nodiscard]] auto variants_of(type_id id) const -> std::optional<std::vector<variant_shape>> {
    const auto &e = entry(id);
    const bool prelude_sum = e.kind == type_kind::builtin_generic_kind &&
                             (e.name == "option" || e.name == "result");
    if (e.kind != type_kind::sum_kind && !prelude_sum) {
      return std::nullopt;
    }
    if (const auto *shape = shape_of(id)) {
      return shape->variants;
    }
    if (e.name == "option" && e.args.size() == 1) {
      return std::vector<variant_shape>{{.name = "some", .payload = {e.args[0]}},
                                        {.name = "none", .payload = {}}};
    }
    if (e.name == "result" && e.args.size() == 2) {
      return std::vector<variant_shape>{{.name = "ok", .payload = {e.args[0]}},
                                        {.name = "err", .payload = {e.args[1]}}};
    }
    return std::nullopt;
  }

  // ------------------------------------------------------------------------
  //  Types: T1, T2, T3
  // ------------------------------------------------------------------------

  auto find_type_problems(type_id id, int depth, type_problems &out) const -> void {
    if (depth > k_max_type_depth) {
      return;
    }
    const auto &e = entry(id);
    const auto note = [&](std::size_t rule) {
      if (!out.at(rule)) {
        out.at(rule) = id;
      }
    };
    const auto all = [&](const std::vector<type_id> &ids) {
      for (const auto arg : ids) {
        find_type_problems(arg, depth + 1, out);
      }
    };
    switch (e.kind) {
    case type_kind::unknown_kind:
    case type_kind::error_kind:
    case type_kind::type_var_kind:
      note(0);
      return;
    case type_kind::refinement_kind:
    case type_kind::existential_kind:
    case type_kind::ctor_ref_kind:
      note(1);
      return;
    case type_kind::type_param_kind:
      if (!generic_params_.contains(id)) {
        note(2);
      }
      return;
    case type_kind::symbolic_value_kind:
      for (const auto &term : e.value.terms) {
        const auto param = semantic::poly_var_identity(term.var);
        if (!param || !generic_params_.contains(*param)) {
          note(2);
        }
      }
      return;
    case type_kind::const_value_kind:
    case type_kind::const_variant_kind:
      return;
    case type_kind::ref_kind:
    case type_kind::ptr_kind:
    case type_kind::array_kind:
    case type_kind::fn_kind:
    case type_kind::param_app_kind:
      find_type_problems(e.result, depth + 1, out);
      all(e.args);
      return;
    case type_kind::builtin_kind:
    case type_kind::builtin_generic_kind:
    case type_kind::tuple_kind:
    case type_kind::struct_kind:
    case type_kind::sum_kind:
    case type_kind::opaque_kind:
      all(e.args);
      return;
    }
  }

  [[nodiscard]] auto problems_of(type_id id) const -> type_problems {
    auto out = type_problems{};
    find_type_problems(id, 0, out);
    return out;
  }

  /// T1–T3 for one type held by a node. `settled_rule` overrides the id
  /// reported for T1 and T2 (C6 reports unsettled generic arguments).
  auto check_type(type_id id, source_span span, std::string_view what,
                  std::string_view settled_rule = {}) -> void {
    static constexpr std::array<std::string_view, 3> rules{"T1", "T2", "T3"};
    static constexpr std::array<std::string_view, 3> reasons{
        "is not settled",
        "is a refinement, an existential or an unapplied constructor",
        "is a parameter this function does not list in `generic_params`",
    };
    const auto found = problems_of(id);
    for (std::size_t rule = 0; rule < rules.size(); ++rule) {
      if (!found.at(rule)) {
        continue;
      }
      const auto id_for = rule < 2 && !settled_rule.empty() ? settled_rule : rules.at(rule);
      report(id_for, span,
             std::format("{} has type `{}`, in which `{}` {}", what, show(id), show(*found.at(rule)),
                         reasons.at(rule)));
    }
  }

  // ------------------------------------------------------------------------
  //  Scope
  // ------------------------------------------------------------------------

  [[nodiscard]] auto lookup(symbol_id symbol) const -> const local * {
    for (auto it = env_.rbegin(); it != env_.rend(); ++it) {
      if (it->symbol == symbol) {
        return &*it;
      }
    }
    return nullptr;
  }

  /// Brings a symbol into scope. F1: once per function, never invalid.
  auto introduce(symbol_id symbol, type_id type, writability root, local_kind kind,
                 source_span span) -> void {
    if (symbol == semantic::k_invalid_symbol_id) {
      report("F1", span, "a local is introduced with the invalid symbol id");
    } else if (!introduced_.insert(symbol).second) {
      report("F1", span, std::format("symbol {} is introduced more than once", symbol));
    }
    env_.push_back(local{.symbol = symbol, .type = type, .root = root, .kind = kind});
  }

  auto leave_scope(std::size_t mark) -> void {
    env_.erase(env_.begin() + static_cast<std::ptrdiff_t>(mark), env_.end());
  }

  // ------------------------------------------------------------------------
  //  Places: P1, P2, P3, P4
  // ------------------------------------------------------------------------

  /// P1 and the root half of P4.
  auto check_root(const place_root &root, source_span span) -> place_facts {
    auto facts = place_facts{.type = root.type};
    check_type(root.type, span, "a place root");
    switch (root.kind) {
    case place_root::origin::local: {
      const auto *found = lookup(root.symbol);
      if (found == nullptr) {
        report("P1", span, std::format("a local root names symbol {}, which is not in scope here",
                                       root.symbol));
        facts.valid = false;
        return facts;
      }
      expect_type("P1", span, root.type, found->type, "a local root");
      facts.access = found->root;
      if (found->kind == local_kind::alias_binding || found->kind == local_kind::capture_borrow) {
        facts.move_block = "rooted at an alias of another place";
      }
      return facts;
    }
    case place_root::origin::global: {
      const auto found = statics_.find(static_key(root.module, root.name));
      if (found == statics_.end()) {
        report("P1", span,
               std::format("a global root names `{}.{}`, which is not a static of the program",
                           root.module, root.name));
      } else {
        expect_type("P1", span, root.type, found->second, "a global root");
      }
      facts.access = writability::static_value;
      facts.move_block = "rooted at a static";
      return facts;
    }
    case place_root::origin::temporary:
      facts.access = writability::writable;
      if (root.value == nullptr) {
        report("P1", span, "a temporary root has no value");
        facts.valid = false;
        return facts;
      }
      check_expr(*root.value);
      expect_type("P1", span, root.type, root.value->type, "a temporary root");
      return facts;
    }
    report("F0", span, "a place root has an unknown origin");
    facts.valid = false;
    return facts;
  }

  /// P2 for every step, P3 for `variant_payload`, and the step half of P4.
  /// `test` is the pattern test at the current position when the steps are a
  /// binding's path (`in_binding`), and null where no test applies.
  auto walk_steps(place_facts &facts, const std::vector<projection> &path, source_span span,
                  const pattern_test *test, bool in_binding) -> void {
    for (const auto &step : path) {
      if (!facts.valid) {
        return;
      }
      check_type(step.type, span, "a projection");
      const auto expected = step_target(facts, step, span, test, in_binding);
      if (!expected) {
        facts.valid = false;
        return;
      }
      if (*expected != step.type) {
        report("P2", span,
               std::format("a projection says it gives `{}`, but the step from `{}` gives `{}`",
                           show(step.type), show(facts.type), show(*expected)));
      }
      facts.type = step.type;
    }
  }

  /// The type one step gives from `facts.type`, or nullopt after reporting
  /// why the step is invalid there.
  auto step_target(place_facts &facts, const projection &step, source_span span,
                   const pattern_test *&test, bool in_binding) -> std::optional<type_id> {
    const auto base = facts.type;
    const auto &e = entry(base);
    const auto invalid = [&](std::string message) -> std::optional<type_id> {
      report("P2", span, std::move(message));
      return std::nullopt;
    };
    switch (step.kind) {
    case projection::step::field: {
      test = record_part(test, step.index);
      if (e.kind != type_kind::struct_kind) {
        return invalid(std::format("a field step on `{}`, which is not a struct", show(base)));
      }
      const auto *fields = fields_of(base);
      if (fields == nullptr) {
        return invalid(std::format("a field step on `{}`, which has no recorded shape", show(base)));
      }
      if (step.index >= fields->size()) {
        return invalid(std::format("field index {} is out of range for `{}`, which has {} fields",
                                   step.index, show(base), fields->size()));
      }
      const auto &field = (*fields)[step.index];
      if (field.name != step.name) {
        report("P2", span,
               std::format("field {} of `{}` is `{}`, but the step names `{}`", step.index,
                           show(base), field.name, step.name));
      }
      return field.type;
    }
    case projection::step::tuple_element:
      test = test != nullptr && test->kind == pattern_test::form::tuple &&
                     step.index < test->parts.size()
                 ? &test->parts[step.index]
                 : nullptr;
      if (e.kind != type_kind::tuple_kind) {
        return invalid(std::format("a tuple element step on `{}`, which is not a tuple", show(base)));
      }
      if (step.index >= e.args.size()) {
        return invalid(std::format("tuple index {} is out of range for `{}`", step.index, show(base)));
      }
      return e.args[step.index];
    case projection::step::variant_payload:
      return payload_target(step, span, base, test, in_binding);
    case projection::step::deref:
      test = nullptr;
      if (e.kind != type_kind::ref_kind && e.kind != type_kind::ptr_kind) {
        return invalid(std::format("a deref step on `{}`, which is not a reference or raw pointer",
                                   show(base)));
      }
      facts.access = e.is_mut ? writability::writable : writability::behind_shared_ref;
      if (e.kind == type_kind::ptr_kind) {
        // What a raw pointer points at is storage of its own: nothing before
        // the pointer limits a move out of it.
        facts.move_block = {};
      } else if (facts.move_block.empty()) {
        facts.move_block = "behind a reference";
      }
      return e.result;
    case projection::step::element:
      test = nullptr;
      return element_target(facts, step, span, base);
    }
    report("F0", span, "a projection has an unknown step kind");
    return std::nullopt;
  }

  /// The part of a `record` test that tests field `index`, if any.
  [[nodiscard]] static auto record_part(const pattern_test *test, std::size_t index)
      -> const pattern_test * {
    if (test == nullptr || test->kind != pattern_test::form::record) {
      return nullptr;
    }
    for (std::size_t k = 0; k < test->field_indices.size() && k < test->parts.size(); ++k) {
      if (test->field_indices[k] == index) {
        return &test->parts[k];
      }
    }
    return nullptr;
  }

  auto payload_target(const projection &step, source_span span, type_id base,
                      const pattern_test *&test, bool in_binding) -> std::optional<type_id> {
    if (!in_binding) {
      report("P3", span, "a variant_payload step appears outside a pattern binding's path");
    }
    const auto variants = variants_of(base);
    if (!variants) {
      report("P2", span,
             std::format("a variant payload step on `{}`, which is not a sum with a known shape",
                         show(base)));
      return std::nullopt;
    }
    auto variant = variants->size();
    for (std::size_t k = 0; k < variants->size(); ++k) {
      if ((*variants)[k].name == step.name) {
        variant = k;
      }
    }
    if (variant == variants->size()) {
      report("P2", span, std::format("`{}` has no variant `{}`", show(base), step.name));
      return std::nullopt;
    }
    const auto &payload = (*variants)[variant].payload;
    if (step.index >= payload.size()) {
      report("P2", span,
             std::format("payload index {} is out of range for variant `{}` of `{}`", step.index,
                         step.name, show(base)));
      return std::nullopt;
    }
    if (in_binding) {
      const bool tested = test != nullptr && test->kind == pattern_test::form::variant &&
                          test->variant_index == variant;
      if (!tested) {
        report("P3", span,
               std::format("a payload of variant `{}` is bound without a `variant` test of `{}` "
                           "at that position",
                           step.name, step.name));
      }
      test = tested && step.index < test->parts.size() ? &test->parts[step.index] : nullptr;
    }
    return payload[step.index];
  }

  auto element_target(place_facts &facts, const projection &step, source_span span,
                      type_id base) -> std::optional<type_id> {
    if (step.subscript == nullptr) {
      report("P2", span, "an element step has no subscript");
    } else {
      check_expr(*step.subscript);
      if (!is_integer(step.subscript->type)) {
        report("P2", span,
               std::format("an element subscript has type `{}`, which is not an integer",
                           show(step.subscript->type)));
      }
    }
    // A2: an element of an array, a slice or `str` cannot be moved; an element
    // of a raw pointer or an `uninit` buffer can (the machine layer's read).
    const auto block_move = [&] {
      if (facts.move_block.empty()) {
        facts.move_block = "an element";
      }
    };
    const auto &e = entry(base);
    if (e.kind == type_kind::array_kind) {
      block_move();
      return e.result;
    }
    if ((is_generic(base, "slice") || is_generic(base, "slice_mut")) && e.args.size() == 1) {
      block_move();
      facts.access =
          is_generic(base, "slice_mut") ? writability::writable : writability::behind_shared_ref;
      return e.args[0];
    }
    if (is_raw_pointer(base)) {
      facts.access = e.is_mut ? writability::writable : writability::behind_shared_ref;
      facts.move_block = {};
      return e.result;
    }
    if (is_generic(base, "uninit") && !e.args.empty()) {
      facts.access = writability::writable;
      return e.args[0];
    }
    if (is_builtin(base, "str")) {
      block_move();
      facts.access = writability::behind_shared_ref;
      if (!is_builtin(step.type, "byte")) {
        report("P2", span,
               std::format("an element of `str` is `byte`, but the step says `{}`",
                           show(step.type)));
        return std::nullopt;
      }
      return step.type;
    }
    report("P2", span,
           std::format("an element step on `{}`, which is not an array, slice, `str`, raw pointer or "
                       "`uninit` buffer",
                       show(base)));
    return std::nullopt;
  }

  auto check_place(const place &p, source_span span) -> place_facts {
    auto facts = check_root(p.root, span);
    walk_steps(facts, p.path, span, nullptr, false);
    return facts;
  }

  // ------------------------------------------------------------------------
  //  Access: A1–A5
  // ------------------------------------------------------------------------

  /// A2 for a place that is moved out of.
  auto check_move(const place_facts &facts, source_span span, std::string_view what) -> void {
    if (facts.valid && !facts.move_block.empty()) {
      report("A2", span, std::format("{} moves out of a place {}", what, facts.move_block));
    }
  }

  auto check_use(const use &u) -> void {
    const auto facts = check_place(u.from, u.span);
    expect_type("A1", u.span, u.type, u.from.type(), "a `use`");
    const bool copy = checked_.is_copy(u.from.type());
    if ((u.how == use::mode::copy) != copy) {
      report("A1", u.span,
             std::format("a `use` of `{}` is a {}, but the type is {}", show(u.from.type()),
                         u.how == use::mode::copy ? "copy" : "move",
                         copy ? "`copy`" : "not `copy`"));
    }
    if (u.how == use::mode::move) {
      check_move(facts, u.span, "a `use`");
    }
  }

  auto check_borrow(const borrow &b, bool call_argument) -> void {
    const auto facts = check_place(b.of, b.span);
    const auto &t = entry(b.type);
    if (t.kind != type_kind::ref_kind || t.result != b.of.type() || t.is_mut != b.is_mut) {
      report("A3", b.span,
             std::format("a {} of a place of type `{}` has type `{}`",
                         b.is_mut ? "`&mut` borrow" : "`&` borrow", show(b.of.type()),
                         show(b.type)));
    }
    if (b.is_mut && facts.valid && facts.access != writability::writable) {
      report("A4", b.span,
             std::format("a mutable borrow of a place that is {}", describe(facts.access)));
    }
    if (b.two_phase && !(call_argument && b.inserted && b.is_mut)) {
      report("A5", b.span,
             "`two_phase` is set on a borrow that is not an inserted mutable call argument");
    }
  }

  // ------------------------------------------------------------------------
  //  Calls: C1–C7
  // ------------------------------------------------------------------------

  /// C3 and F3: why `mode` does not fit a first parameter in `params`.
  [[nodiscard]] auto receiver_problem(receiver_mode mode, const std::vector<type_id> &params) const
      -> std::optional<std::string> {
    if (mode == receiver_mode::none) {
      return std::nullopt;
    }
    if (params.empty()) {
      return std::format("receiver mode `{}` needs a first parameter, and there is none",
                         describe(mode));
    }
    const auto first = params.front();
    const auto &e = entry(first);
    const bool is_ref = e.kind == type_kind::ref_kind;
    const bool fits = mode == receiver_mode::shared      ? is_ref && !e.is_mut
                      : mode == receiver_mode::exclusive ? is_ref && e.is_mut
                      : mode == receiver_mode::by_value  ? !is_ref && checked_.is_copy(first)
                                                         : !is_ref;
    if (fits) {
      return std::nullopt;
    }
    return std::format("receiver mode `{}` does not fit a first parameter of type `{}`",
                       describe(mode), show(first));
  }

  [[nodiscard]] auto signature_of(const function &f, type_id signature) const -> bool {
    const auto &e = entry(signature);
    if (e.kind != type_kind::fn_kind || e.result != f.return_type ||
        e.args.size() != f.params.size()) {
      return false;
    }
    for (std::size_t k = 0; k < e.args.size(); ++k) {
      if (e.args[k] != f.params[k].type) {
        return false;
      }
    }
    return true;
  }

  /// C2: a function callee's declaration and, when the program holds it, its
  /// signature and receiver. C8: the generic argument count.
  auto check_function_callee(const callee &t, source_span span) -> void {
    if (t.decl == nullptr) {
      report("C2", span, std::format("function callee `{}` has no declaration", t.name));
      return;
    }
    if (t.value != nullptr) {
      report("C2", span, std::format("function callee `{}` also holds a value", t.name));
    }
    const auto [first, last] = functions_.equal_range(t.decl);
    if (first != last) {
      bool matched = false;
      for (auto it = first; it != last; ++it) {
        matched = matched || (signature_of(*it->second, t.signature) &&
                              it->second->receiver == t.receiver);
      }
      if (!matched) {
        const auto &f = *first->second;
        report("C2", span,
               std::format("callee `{}` has signature `{}` and receiver `{}`, but no core function "
                           "for its declaration has them (e.g. `{}` has receiver `{}`)",
                           t.name, show(t.signature), describe(t.receiver), f.name,
                           describe(f.receiver)));
      }
    }
    const auto *template_decl = t.instance_of != nullptr ? t.instance_of : t.decl;
    const auto [tfirst, tlast] = functions_.equal_range(template_decl);
    for (auto it = tfirst; it != tlast; ++it) {
      const auto &tmpl = *it->second;
      if (tmpl.instance_of == nullptr && tmpl.generic_params.size() != t.generic_args.size()) {
        report("C8", span,
               std::format("callee `{}` has {} generic arguments, but its template `{}` has {} "
                           "parameters",
                           t.name, t.generic_args.size(), tmpl.name, tmpl.generic_params.size()));
        break;
      }
    }
  }

  /// C5: a compiler operation has its shape (`intrinsics.h`); any other name
  /// is an `intrinsic def` of the program with the declared signature.
  auto check_intrinsic_callee(const callee &t, const type_entry *sig, source_span span) -> void {
    if (is_compiler_operation(t.name)) {
      if (const auto problem = check_intrinsic(t.name, t.signature, t.generic_args, checked_)) {
        report("C5", span, *problem);
      }
      return;
    }
    const auto [first, last] = declared_intrinsics_.equal_range(t.name);
    if (first == last) {
      report("C5", span,
             std::format("intrinsic `{}` is neither a compiler operation nor declared by an "
                         "`intrinsic def` of the program",
                         t.name));
      return;
    }
    if (sig == nullptr) {
      return; // C1 reports a signature that is not a `fn` type.
    }
    for (auto it = first; it != last; ++it) {
      const auto &d = *it->second;
      if (d.signature == t.signature) {
        return;
      }
      const auto &declared = entry(d.signature);
      if (d.generic && declared.kind == type_kind::fn_kind &&
          declared.args.size() == sig->args.size()) {
        return;
      }
    }
    report("C5", span,
           std::format("intrinsic `{}` has signature `{}`, which matches no declaration of it "
                       "(e.g. `{}`)",
                       t.name, show(t.signature), show(first->second->signature)));
  }

  /// C1 (the signature is a `fn`), C2–C7 for one callee. Returns the
  /// signature's entry when it is a `fn` type.
  auto check_callee(const callee &t, source_span span, bool function_ref) -> const type_entry * {
    check_type(t.signature, span, "a callee signature");
    for (const auto arg : t.generic_args) {
      check_type(arg, span, "a generic argument", "C6");
    }
    const type_entry *sig = &entry(t.signature);
    if (sig->kind != type_kind::fn_kind) {
      report(function_ref ? "C7" : "C1", span,
             std::format("callee `{}` has signature `{}`, which is not a fn type", t.name,
                         show(t.signature)));
      sig = nullptr;
    }
    switch (t.kind) {
    case callee::form::function:
      check_function_callee(t, span);
      break;
    case callee::form::value:
      if (function_ref) {
        break;
      }
      if (t.decl != nullptr || t.instance_of != nullptr) {
        report("C4", span, "a value callee has a declaration");
      }
      if (t.value == nullptr) {
        report("C4", span, "a value callee has no value");
      } else {
        check_expr(*t.value);
        const auto &held = entry(t.value->type);
        const bool through_shared_ref = held.kind == type_kind::ref_kind && !held.is_mut &&
                                        held.result == t.signature;
        if (t.value->type != t.signature && !through_shared_ref) {
          report("C4", span,
                 std::format("a value callee's value has type `{}`, expected `{}` or a `&` of it",
                             show(t.value->type), show(t.signature)));
        }
      }
      break;
    case callee::form::intrinsic:
      if (function_ref) {
        break;
      }
      if (t.name.empty()) {
        report("C5", span, "an intrinsic callee has no name");
      }
      if (t.decl != nullptr || t.instance_of != nullptr || t.value != nullptr) {
        report("C5", span,
               std::format("intrinsic callee `{}` has a declaration or a value", t.name));
      }
      if (!t.name.empty()) {
        check_intrinsic_callee(t, sig, span);
      }
      break;
    default:
      report("F0", span, "a callee has an unknown form");
      return nullptr;
    }
    if (function_ref && t.kind != callee::form::function) {
      report("C7", span, "a function_ref's callee is not a `function` callee");
    }
    if (sig != nullptr) {
      if (const auto problem = receiver_problem(t.receiver, sig->args)) {
        report("C3", span, std::format("callee `{}`: {}", t.name, *problem));
      }
    }
    return sig;
  }

  auto check_call(const call &c) -> void {
    const auto *sig = check_callee(c.target, c.span, false);
    for (const auto &arg : c.args) {
      if (arg == nullptr) {
        report("F0", c.span, "a call has a null argument");
      } else {
        check_expr(*arg, true);
      }
    }
    if (sig == nullptr) {
      return;
    }
    if (sig->args.size() != c.args.size()) {
      report("C1", c.span,
             std::format("a call to `{}` has {} arguments for {} parameters", c.target.name,
                         c.args.size(), sig->args.size()));
    } else {
      for (std::size_t k = 0; k < c.args.size(); ++k) {
        if (c.args[k] != nullptr && !diverges(types_, c.args[k].get())) {
          expect_type("C1", c.args[k]->span, c.args[k]->type, sig->args[k],
                      std::format("argument {} of a call to `{}`", k, c.target.name));
        }
      }
    }
    expect_type("C1", c.span, c.type, sig->result,
                std::format("a call to `{}`", c.target.name));
  }

  auto check_function_ref(const function_ref &f) -> void {
    if (check_callee(f.target, f.span, true) != nullptr) {
      expect_type("C7", f.span, f.type, f.target.signature, "a function_ref");
    }
  }

  // ------------------------------------------------------------------------
  //  Expressions: E1–E11
  // ------------------------------------------------------------------------

  /// Runs `check` on `n` as a `T`; reports F0 when the node's class does
  /// not match its kind.
  template <typename T, typename... Args>
  auto dispatch(const node &n, auto (verifier::*check)(const T &, Args...)->void, Args... args)
      -> void {
    if (const auto *typed = as<T>(n)) {
      (this->*check)(*typed, args...);
    } else {
      report("F0", n.span, "a node's kind does not match its class");
    }
  }

  auto check_expr(const expr &e, bool call_argument = false) -> void {
    check_type(e.type, e.span, "an expression");
    using k = node_kind;
    switch (e.kind) {
    case k::literal: return dispatch(e, &verifier::check_literal);
    case k::use: return dispatch(e, &verifier::check_use);
    case k::borrow: return dispatch(e, &verifier::check_borrow, call_argument);
    case k::call: return dispatch(e, &verifier::check_call);
    case k::function_ref: return dispatch(e, &verifier::check_function_ref);
    case k::primitive: return dispatch(e, &verifier::check_primitive);
    case k::cast: return dispatch(e, &verifier::check_cast);
    case k::tuple_init:
    case k::array_init:
    case k::struct_init: return dispatch(e, &verifier::check_aggregate);
    case k::variant_init: return dispatch(e, &verifier::check_variant_init);
    case k::lambda: return dispatch(e, &verifier::check_lambda);
    case k::block: return dispatch(e, &verifier::check_block);
    case k::if_expr: return dispatch(e, &verifier::check_if);
    case k::match_expr: return dispatch(e, &verifier::check_match);
    case k::await_expr: return dispatch(e, &verifier::check_await);
    default: return report("F0", e.span, "an expression node has a statement kind");
    }
  }

  auto check_literal(const literal &l) -> void {
    const auto t = l.type;
    bool fits = false;
    switch (l.token) {
    case token_kind::int_lit:
      fits = is_integer(t) || (entry(t).kind == type_kind::builtin_kind && types_.is_float(t));
      break;
    case token_kind::float_lit:
      fits = entry(t).kind == type_kind::builtin_kind && types_.is_float(t);
      break;
    case token_kind::kw_true:
    case token_kind::kw_false:
      fits = is_bool(t);
      break;
    case token_kind::string_lit:
      fits = is_builtin(t, "str");
      break;
    case token_kind::char_lit:
      fits = is_builtin(t, "char");
      break;
    case token_kind::kw_unit:
      fits = is_unit(t);
      break;
    default:
      report("E1", l.span, std::format("literal `{}` has a token that is not a literal", l.spelling));
      return;
    }
    if (!fits) {
      report("E1", l.span,
             std::format("literal `{}` has type `{}`, which its token cannot have", l.spelling,
                         show(t)));
    }
  }

  auto check_primitive(const primitive &p) -> void {
    if (p.binary.has_value() == p.unary.has_value()) {
      report("E2", p.span, "a primitive must have exactly one operator");
      return;
    }
    for (const auto &operand : p.operands) {
      if (operand == nullptr) {
        report("F0", p.span, "a primitive has a null operand");
        return;
      }
      check_expr(*operand);
      if (!is_scalar(operand->type) && !is_raw_pointer(operand->type)) {
        report("E2", operand->span,
               std::format("a primitive operand has type `{}`, which is not a builtin scalar or "
                           "raw pointer",
                           show(operand->type)));
      }
    }
    const std::size_t arity = p.binary ? 2 : 1;
    if (p.operands.size() != arity) {
      report("E2", p.span,
             std::format("a primitive has {} operands, expected {}", p.operands.size(), arity));
      return;
    }
    const auto a = p.operands[0]->type;
    const auto b = arity == 2 ? p.operands[1]->type : a;
    const auto both = [&](type_id t) {
      expect_type("E2", p.span, a, t, "the first operand");
      expect_type("E2", p.span, b, t, "the second operand");
    };
    if (p.unary) {
      switch (*p.unary) {
      case ast::unary_op::neg:
      case ast::unary_op::bit_not:
        expect_type("E2", p.span, a, p.type, "the operand");
        return;
      case ast::unary_op::logical_not:
        both(types_.bool_type());
        expect_type("E2", p.span, p.type, types_.bool_type(), "`not`");
        return;
      default:
        report("E2", p.span,
               std::format("`{}` is not a primitive operator", ast::unary_op_name(*p.unary)));
        return;
      }
    }
    switch (*p.binary) {
    case ast::binary_op::add:
    case ast::binary_op::sub:
    case ast::binary_op::mul:
    case ast::binary_op::div:
    case ast::binary_op::mod:
    case ast::binary_op::add_wrap:
    case ast::binary_op::sub_wrap:
    case ast::binary_op::mul_wrap:
    case ast::binary_op::add_sat:
    case ast::binary_op::sub_sat:
    case ast::binary_op::mul_sat:
    case ast::binary_op::bit_and:
    case ast::binary_op::bit_or:
    case ast::binary_op::bit_xor:
      both(p.type);
      return;
    case ast::binary_op::shl:
    case ast::binary_op::shr:
      expect_type("E2", p.span, a, p.type, "a shift's first operand");
      if (!is_integer(a) || !is_integer(b)) {
        report("E2", p.span,
               std::format("a shift of `{}` by `{}` needs two integers", show(a), show(b)));
      }
      return;
    case ast::binary_op::eq_eq:
    case ast::binary_op::bang_eq:
    case ast::binary_op::lt:
    case ast::binary_op::lt_eq:
    case ast::binary_op::gt:
    case ast::binary_op::gt_eq:
      expect_type("E2", p.span, b, a, "a comparison's second operand");
      expect_type("E2", p.span, p.type, types_.bool_type(), "a comparison");
      return;
    case ast::binary_op::logical_and:
    case ast::binary_op::logical_or:
      both(types_.bool_type());
      expect_type("E2", p.span, p.type, types_.bool_type(), "`and`/`or`");
      return;
    default:
      report("E2", p.span,
             std::format("`{}` is not a primitive operator", ast::binary_op_name(*p.binary)));
      return;
    }
  }

  auto check_cast(const cast &c) -> void {
    if (c.operand == nullptr) {
      report("F0", c.span, "a cast has no operand");
      return;
    }
    check_expr(*c.operand);
    const auto from = c.operand->type;
    const auto to = c.type;
    const auto usize = types_.usize_type();
    const bool ok = (is_scalar(from) && is_scalar(to)) ||
                    (is_raw_pointer(from) && to == usize) || (from == usize && is_raw_pointer(to)) ||
                    (is_raw_pointer(from) && is_raw_pointer(to));
    if (!ok) {
      report("E3", c.span,
             std::format("a cast from `{}` to `{}` is not between builtin scalars or between a raw "
                         "pointer and `usize`, or between raw pointers",
                         show(from), show(to)));
    }
  }

  /// E4: one element per member, in order, each of the member's type.
  auto check_members(const expr &at, const ptr_vec<expr> &elements,
                     const std::vector<type_id> &members, std::string_view what) -> void {
    for (const auto &element : elements) {
      if (element == nullptr) {
        report("F0", at.span, std::format("{} has a null element", what));
      } else {
        check_expr(*element);
      }
    }
    if (elements.size() != members.size()) {
      report("E4", at.span,
             std::format("{} of `{}` has {} elements, expected {}", what, show(at.type),
                         elements.size(), members.size()));
      return;
    }
    for (std::size_t k = 0; k < elements.size(); ++k) {
      if (elements[k] != nullptr && !diverges(types_, elements[k].get())) {
        expect_type("E4", elements[k]->span, elements[k]->type, members[k],
                    std::format("element {} of {}", k, what));
      }
    }
  }

  auto check_aggregate(const aggregate_init &a) -> void {
    const auto &e = entry(a.type);
    if (a.kind != node_kind::struct_init && (!a.field_names.empty() || !a.field_indices.empty())) {
      report("E4", a.span, "only a struct construction has field names or indices");
    }
    if (a.kind != node_kind::array_init && a.repeat) {
      report("E4", a.span, "only an array construction has `repeat`");
    }
    if (a.kind == node_kind::tuple_init) {
      if (e.kind != type_kind::tuple_kind) {
        report("E4", a.span, std::format("a tuple construction has type `{}`", show(a.type)));
        return;
      }
      check_members(a, a.elements, e.args, "a tuple");
      return;
    }
    if (a.kind == node_kind::array_init) {
      if (e.kind != type_kind::array_kind || !e.array_size) {
        report("E4", a.span,
               std::format("an array construction has type `{}`, which is not an array of known "
                           "length",
                           show(a.type)));
        return;
      }
      if (a.repeat && *a.repeat != *e.array_size) {
        report("E4", a.span,
               std::format("an array repeat count is {}, but `{}` has length {}", *a.repeat,
                           show(a.type), *e.array_size));
      }
      const auto count = a.repeat ? std::size_t{1} : static_cast<std::size_t>(*e.array_size);
      check_members(a, a.elements, std::vector<type_id>(count, e.result), "an array");
      return;
    }
    const auto *fields = fields_of(a.type);
    if (fields == nullptr) {
      report("E4", a.span,
             std::format("a struct construction has type `{}`, which is not a struct with a "
                         "recorded shape",
                         show(a.type)));
      return;
    }
    check_struct_members(a, *fields);
  }

  /// E4 for a struct: `field_indices` name each field once, `field_names`
  /// agree with them, and element `i` has the type of field `field_indices[i]`.
  auto check_struct_members(const aggregate_init &a, const std::vector<field_shape> &fields)
      -> void {
    const auto n = fields.size();
    bool sound = a.field_indices.size() == n && a.field_names.size() == n && a.elements.size() == n;
    if (!sound) {
      report("E4", a.span,
             std::format("a construction of `{}` has {} elements, {} field names and {} field "
                         "indices for {} fields",
                         show(a.type), a.elements.size(), a.field_names.size(),
                         a.field_indices.size(), n));
    } else {
      auto seen = std::vector<bool>(n, false);
      for (std::size_t k = 0; k < n; ++k) {
        const auto index = a.field_indices[k];
        if (index >= n) {
          report("E4", a.span,
                 std::format("a construction of `{}` initializes field index {}, which is out of "
                             "range",
                             show(a.type), index));
          sound = false;
        } else if (seen[index]) {
          report("E4", a.span,
                 std::format("a construction of `{}` initializes field `{}` more than once",
                             show(a.type), fields[index].name));
          sound = false;
        } else {
          seen[index] = true;
          if (a.field_names[k] != fields[index].name) {
            report("E4", a.span,
                   std::format("field index {} of `{}` is `{}`, but the construction names `{}`",
                               index, show(a.type), fields[index].name, a.field_names[k]));
          }
        }
      }
    }
    for (const auto &element : a.elements) {
      if (element == nullptr) {
        report("F0", a.span, "a struct has a null element");
      } else {
        check_expr(*element);
      }
    }
    if (!sound) {
      return;
    }
    for (std::size_t k = 0; k < n; ++k) {
      if (a.elements[k] != nullptr && !diverges(types_, a.elements[k].get())) {
        expect_type("E4", a.elements[k]->span, a.elements[k]->type, fields[a.field_indices[k]].type,
                    std::format("the initializer of field `{}` of a struct", a.field_names[k]));
      }
    }
  }

  auto check_variant_init(const variant_init &v) -> void {
    const auto variants = variants_of(v.type);
    if (!variants) {
      report("E4", v.span,
             std::format("a variant construction has type `{}`, which is not a sum with a known "
                         "shape",
                         show(v.type)));
      for (const auto &part : v.payload) {
        if (part != nullptr) {
          check_expr(*part);
        }
      }
      return;
    }
    if (v.variant_index >= variants->size()) {
      report("E4", v.span,
             std::format("variant index {} is out of range for `{}`", v.variant_index, show(v.type)));
      return;
    }
    const auto &variant = (*variants)[v.variant_index];
    if (variant.name != v.variant) {
      report("E4", v.span,
             std::format("variant {} of `{}` is `{}`, but the construction names `{}`",
                         v.variant_index, show(v.type), variant.name, v.variant));
    }
    check_members(v, v.payload, variant.payload, std::format("variant `{}`", variant.name));
  }

  auto check_block(const block &b) -> void {
    const auto mark = env_.size();
    for (const auto &s : b.statements) {
      if (s == nullptr) {
        report("F0", b.span, "a block has a null statement");
      } else {
        check_stmt(*s);
      }
    }
    if (b.result != nullptr) {
      check_expr(*b.result);
    }
    leave_scope(mark);
    if (diverges(types_, &b)) {
      return;
    }
    if (b.result == nullptr) {
      if (!is_unit(b.type)) {
        report("E5", b.span,
               std::format("a block without a result has type `{}`, expected `unit`", show(b.type)));
      }
    } else if (!diverges(types_, b.result.get())) {
      expect_type("E5", b.span, b.type, b.result->type, "a block (compared with its result)");
    }
  }

  auto check_if(const if_expr &i) -> void {
    if (i.condition == nullptr) {
      report("F0", i.span, "an `if` has no condition");
    } else {
      check_expr(*i.condition);
      expect_type("E6", i.condition->span, i.condition->type, types_.bool_type(),
                  "an `if` condition");
    }
    if (i.then_branch == nullptr) {
      report("F0", i.span, "an `if` has no then branch");
      return;
    }
    check_expr(*i.then_branch);
    if (i.else_branch == nullptr) {
      if (!is_unit(i.type)) {
        report("E6", i.span,
               std::format("an `if` without `else` has type `{}`, expected `unit`", show(i.type)));
      }
      if (!diverges(types_, i.then_branch.get()) && !is_unit(i.then_branch->type)) {
        report("E6", i.then_branch->span,
               std::format("the branch of an `if` without `else` has type `{}`, expected `unit`",
                           show(i.then_branch->type)));
      }
      return;
    }
    check_expr(*i.else_branch);
    for (const auto *branch : {i.then_branch.get(), i.else_branch.get()}) {
      if (!diverges(types_, branch)) {
        expect_type("E6", branch->span, branch->type, i.type, "an `if` branch");
      }
    }
  }

  /// E7: a test has the type of the part it tests, and its parts fit.
  auto check_test(const pattern_test &t, type_id part) -> void {
    check_type(t.subject_type, t.span, "a pattern test");
    expect_type("E7", t.span, t.subject_type, part, "a pattern test");
    const auto bad = [&](std::string message) { report("E7", t.span, std::move(message)); };
    const auto each = [&](const std::vector<type_id> &members) {
      if (t.parts.size() != members.size()) {
        bad(std::format("a test of `{}` has {} parts, expected {}", show(part), t.parts.size(),
                        members.size()));
        return;
      }
      for (std::size_t k = 0; k < members.size(); ++k) {
        check_test(t.parts[k], members[k]);
      }
    };
    const auto &e = entry(part);
    switch (t.kind) {
    case pattern_test::form::any:
      return;
    case pattern_test::form::literal:
    case pattern_test::form::range:
      if (t.kind == pattern_test::form::literal && t.low == nullptr) {
        bad("a literal test has no value");
      }
      if (t.kind == pattern_test::form::range && t.low == nullptr && t.high == nullptr) {
        bad("a range test has neither a low nor a high bound");
      }
      for (const auto *bound : {t.low.get(), t.high.get()}) {
        if (bound != nullptr) {
          check_expr(*bound);
          expect_type("E7", t.span, bound->type, part, "a pattern bound");
        }
      }
      if (t.kind == pattern_test::form::range && !is_scalar(part)) {
        bad(std::format("a range test of `{}`, which is not a builtin scalar", show(part)));
      }
      return;
    case pattern_test::form::variant: {
      const auto variants = variants_of(part);
      if (!variants) {
        bad(std::format("a variant test of `{}`, which is not a sum with a known shape", show(part)));
      } else if (t.variant_index >= variants->size()) {
        bad(std::format("variant index {} is out of range for `{}`", t.variant_index, show(part)));
      } else {
        each((*variants)[t.variant_index].payload);
      }
      return;
    }
    case pattern_test::form::tuple:
      if (e.kind != type_kind::tuple_kind) {
        bad(std::format("a tuple test of `{}`", show(part)));
      } else {
        each(e.args);
      }
      return;
    case pattern_test::form::record: {
      const auto *fields = fields_of(part);
      if (fields == nullptr) {
        bad(std::format("a record test of `{}`, which is not a struct with a recorded shape",
                        show(part)));
        return;
      }
      if (t.field_indices.size() != t.parts.size()) {
        bad("a record test has a different number of field indices and parts");
        return;
      }
      for (std::size_t k = 0; k < t.parts.size(); ++k) {
        if (t.field_indices[k] >= fields->size()) {
          bad(std::format("field index {} is out of range for `{}`", t.field_indices[k], show(part)));
        } else {
          check_test(t.parts[k], (*fields)[t.field_indices[k]].type);
        }
      }
      return;
    }
    case pattern_test::form::array:
      if (e.kind != type_kind::array_kind || !e.array_size) {
        bad(std::format("an array test of `{}`, which is not an array of known length", show(part)));
      } else {
        each(std::vector<type_id>(static_cast<std::size_t>(*e.array_size), e.result));
      }
      return;
    }
    report("F0", t.span, "a pattern test has an unknown form");
  }

  /// E8 (with P2, P3, A2) for one binding of an alternative.
  auto check_binding(const pattern_binding &b, const place_facts &subject,
                     const pattern_test &test) -> void {
    check_type(b.type, b.span, std::format("binding `{}`", b.name));
    if (!subject.valid) {
      return;
    }
    auto facts = subject;
    walk_steps(facts, b.path, b.span, &test, true);
    if (!facts.valid) {
      return;
    }
    expect_type("E8", b.span, b.type, facts.type, std::format("binding `{}`", b.name));
    switch (b.how) {
    case pattern_binding::mode::copy:
      if (!checked_.is_copy(b.type)) {
        report("E8", b.span,
               std::format("binding `{}` copies `{}`, which is not `copy`", b.name, show(b.type)));
      }
      if (!facts.move_block.empty()) {
        report("E8", b.span,
               std::format("binding `{}` is a `copy` binding of a place {}; it must be an `alias`",
                           b.name, facts.move_block));
      }
      return;
    case pattern_binding::mode::move:
      check_move(facts, b.span, std::format("binding `{}`", b.name));
      return;
    case pattern_binding::mode::alias:
      if (b.writable && facts.access != writability::writable) {
        report("E8", b.span,
               std::format("binding `{}` is a writable alias of a part that is {}", b.name,
                           describe(facts.access)));
      }
      return;
    }
  }

  /// E7: every alternative binds what the first does.
  auto check_alternatives_agree(const match_arm &arm) -> void {
    const auto &first = arm.alternatives.front().bindings;
    for (std::size_t k = 1; k < arm.alternatives.size(); ++k) {
      const auto &other = arm.alternatives[k].bindings;
      bool same = other.size() == first.size();
      for (const auto &b : first) {
        bool found = false;
        for (const auto &o : other) {
          found = found || (o.symbol == b.symbol && o.type == b.type && o.how == b.how &&
                            o.writable == b.writable);
        }
        same = same && found;
      }
      if (!same) {
        report("E7", arm.span,
               std::format("alternative {} of a match arm does not bind the same symbols, types, "
                           "modes and writability as the first",
                           k));
      }
    }
  }

  auto check_match(const match_expr &m) -> void {
    const auto subject = check_place(m.subject, m.span);
    for (const auto &arm : m.arms) {
      if (arm.alternatives.empty()) {
        report("E7", arm.span, "a match arm has no alternatives");
      }
      for (const auto &alternative : arm.alternatives) {
        check_test(alternative.test, m.subject.type());
        for (const auto &b : alternative.bindings) {
          check_binding(b, subject, alternative.test);
        }
      }
      const auto mark = env_.size();
      if (!arm.alternatives.empty()) {
        check_alternatives_agree(arm);
        for (const auto &b : arm.alternatives.front().bindings) {
          const auto root = b.writable ? writability::writable : writability::immutable_binding;
          const auto kind = b.how == pattern_binding::mode::alias ? local_kind::alias_binding
                                                                  : local_kind::binding;
          introduce(b.symbol, b.type, root, kind, b.span);
        }
      }
      if (arm.guard != nullptr) {
        check_expr(*arm.guard);
        expect_type("E9", arm.guard->span, arm.guard->type, types_.bool_type(), "a match guard");
      }
      if (arm.body == nullptr) {
        report("F0", arm.span, "a match arm has no body");
      } else {
        check_expr(*arm.body);
        if (!diverges(types_, arm.body.get())) {
          expect_type("E9", arm.body->span, arm.body->type, m.type, "a match arm body");
        }
      }
      leave_scope(mark);
    }
  }

  auto check_lambda(const lambda &l) -> void {
    check_type(l.return_type, l.span, "a lambda's return type");
    for (const auto &c : l.captures) {
      check_type(c.type, l.span, "a capture");
      const auto facts = check_place(c.from, l.span);
      if (facts.valid) {
        expect_type("E10", l.span, c.from.type(), c.type, "a capture's place");
      }
      if (c.how == capture::mode::copy && !checked_.is_copy(c.type)) {
        report("E10", l.span, std::format("a `copy` capture of `{}`, which is not `copy`", show(c.type)));
      }
      if (c.how == capture::mode::borrow_mut && facts.valid &&
          facts.access != writability::writable) {
        report("E10", l.span,
               std::format("a `borrow_mut` capture of a place that is {}", describe(facts.access)));
      }
      if (c.how == capture::mode::move) {
        check_move(facts, l.span, "a `move` capture");
      }
    }
    const auto &t = entry(l.type);
    bool fits = t.kind == type_kind::fn_kind && t.result == l.return_type &&
                t.args.size() == l.params.size();
    for (std::size_t k = 0; fits && k < l.params.size(); ++k) {
      fits = t.args[k] == l.params[k].type;
    }
    if (!fits) {
      report("E10", l.span,
             std::format("a lambda has type `{}`, which is not `fn` of its parameters to `{}`",
                         show(l.type), show(l.return_type)));
    }
    auto saved_env = std::exchange(env_, {});
    auto saved_frame = std::exchange(frame_, body_frame{.return_type = l.return_type});
    for (const auto &p : l.params) {
      check_type(p.type, l.span, std::format("lambda parameter `{}`", p.name));
      introduce(p.symbol, p.type, writability::writable, local_kind::param, l.span);
    }
    for (const auto &c : l.captures) {
      const bool borrowed = c.how == capture::mode::borrow || c.how == capture::mode::borrow_mut;
      const bool writable = c.how != capture::mode::borrow;
      introduce(c.symbol, c.type, writable ? writability::writable : writability::immutable_binding,
                borrowed ? local_kind::capture_borrow : local_kind::capture_value, l.span);
    }
    if (l.body == nullptr) {
      report("F0", l.span, "a lambda has no body");
    } else {
      check_expr(*l.body);
      if (!diverges(types_, l.body.get())) {
        expect_type("E10", l.body->span, l.body->type, l.return_type, "a lambda body");
      }
    }
    env_ = std::move(saved_env);
    frame_ = saved_frame;
  }

  /// E11. A `task[T, E, C]` awaits to `result[T, E]` unless `E` is `never`
  /// (or absent), and to `T` then; this is the checker's `awaited_type`.
  auto check_await(const await_expr &a) -> void {
    if (a.task == nullptr) {
      report("E11", a.span, "an `await` has no task");
      return;
    }
    check_expr(*a.task);
    const auto &t = entry(a.task->type);
    if (t.kind != type_kind::builtin_generic_kind || t.name != "task" || t.args.empty()) {
      report("E11", a.span,
             std::format("`await` takes a value of type `{}`, which is not a task",
                         show(a.task->type)));
      return;
    }
    if (t.args.size() >= 2 && !is_builtin(t.args[1], "never")) {
      if (!task_awaits(types_, a.task->type, a.type)) {
        report("E11", a.span,
               std::format("awaiting `{}` has type `{}`, expected `result[{}, {}]`",
                           show(a.task->type), show(a.type), show(t.args[0]), show(t.args[1])));
      }
      return;
    }
    expect_type("E11", a.span, a.type, t.args[0], "an `await`");
  }

  // ------------------------------------------------------------------------
  //  Statements: S1–S5
  // ------------------------------------------------------------------------

  auto check_stmt(const stmt &s) -> void {
    using k = node_kind;
    switch (s.kind) {
    case k::let: return dispatch(s, &verifier::check_let);
    case k::assign: return dispatch(s, &verifier::check_assign);
    case k::expr_stmt: return dispatch(s, &verifier::check_expr_stmt);
    case k::while_loop: return dispatch(s, &verifier::check_while);
    case k::break_stmt:
    case k::continue_stmt:
    case k::return_stmt:
    case k::yield_stmt: return dispatch(s, &verifier::check_jump);
    case k::contract_check: return dispatch(s, &verifier::check_contract);
    default: return report("F0", s.span, "a statement node has an expression kind");
    }
  }

  auto check_expr_stmt(const expr_stmt &s) -> void {
    if (s.value == nullptr) {
      report("F0", s.span, "an expression statement has no value");
    } else {
      check_expr(*s.value);
    }
  }

  auto check_contract(const contract_check &c) -> void {
    if (c.condition == nullptr) {
      report("F0", c.span, "a contract check has no condition");
      return;
    }
    check_expr(*c.condition);
    expect_type("S5", c.span, c.condition->type, types_.bool_type(), "a contract check's condition");
  }

  auto check_let(const let &l) -> void {
    check_type(l.type, l.span, std::format("`let {}`", l.name));
    if (l.value != nullptr) {
      check_expr(*l.value);
      if (!diverges(types_, l.value.get())) {
        expect_type("S1", l.span, l.value->type, l.type, std::format("the value of `let {}`", l.name));
      }
    }
    introduce(l.symbol, l.type, l.is_mut ? writability::writable : writability::immutable_binding,
              l.value != nullptr ? local_kind::let_with_value : local_kind::let_without_value,
              l.span);
  }

  auto check_assign(const assign &a) -> void {
    const auto facts = check_place(a.target, a.span);
    if (a.value == nullptr) {
      report("F0", a.span, "an assignment has no value");
    } else {
      check_expr(*a.value);
      if (!diverges(types_, a.value.get())) {
        expect_type("S2", a.span, a.value->type, a.target.type(), "an assigned value");
      }
    }
    if (!facts.valid || facts.access == writability::writable) {
      return;
    }
    const auto *root = a.target.root.kind == place_root::origin::local && a.target.path.empty()
                           ? lookup(a.target.root.symbol)
                           : nullptr;
    if (root == nullptr || root->kind != local_kind::let_without_value) {
      report("S2", a.span,
             std::format("an assignment to a place that is {}", describe(facts.access)));
    }
  }

  auto check_while(const while_loop &w) -> void {
    if (w.condition != nullptr) {
      check_expr(*w.condition);
      expect_type("S3", w.condition->span, w.condition->type, types_.bool_type(),
                  "a loop condition");
    }
    ++frame_.loops;
    if (w.body == nullptr) {
      report("F0", w.span, "a loop has no body");
    } else {
      check_expr(*w.body);
    }
    if (w.step != nullptr) {
      check_expr(*w.step);
    }
    --frame_.loops;
  }

  auto check_jump(const jump &j) -> void {
    switch (j.kind) {
    case node_kind::break_stmt:
    case node_kind::continue_stmt:
      if (frame_.loops == 0) {
        report("S3", j.span, "a `break` or `continue` is not inside a loop of this body");
      }
      if (j.value != nullptr) {
        report("F0", j.span, "a `break` or `continue` carries a value");
      }
      return;
    case node_kind::return_stmt:
      if (j.value == nullptr) {
        if (!is_unit(frame_.return_type)) {
          report("S4", j.span,
                 std::format("a `return` without a value in a body of type `{}`",
                             show(frame_.return_type)));
        }
        return;
      }
      check_expr(*j.value);
      if (!diverges(types_, j.value.get())) {
        expect_type("S4", j.span, j.value->type, frame_.return_type, "a returned value");
      }
      return;
    default:
      if (!frame_.generator) {
        report("S4", j.span, "a `yield` outside a generator");
      }
      if (j.value == nullptr) {
        report("S4", j.span, "a `yield` has no value");
        return;
      }
      check_expr(*j.value);
      if (frame_.generator && !diverges(types_, j.value.get())) {
        expect_type("S4", j.span, j.value->type, frame_.yield_type, "a yielded value");
      }
      return;
    }
  }

  // ------------------------------------------------------------------------
  //  Functions and statics: F1–F4
  // ------------------------------------------------------------------------

  /// F2's relation between `return_type` and `body_type` (`nodes.h`).
  auto check_coroutine_types(const function &f) -> void {
    const auto &r = entry(f.return_type);
    switch (f.coroutine) {
    case coroutine_kind::none:
      expect_type("F2", f.span, f.body_type, f.return_type, "the body type of a plain function");
      return;
    case coroutine_kind::generator:
      if (!is_unit(f.body_type)) {
        report("F2", f.span, std::format("a generator's body type is `{}`, expected `unit`",
                                         show(f.body_type)));
      }
      if (!is_generic(f.return_type, "generator") || r.args.size() != 1 ||
          r.args[0] != f.yield_type) {
        report("F2", f.span,
               std::format("a generator returns `{}`, expected `generator[{}]`", show(f.return_type),
                           show(f.yield_type)));
      }
      return;
    case coroutine_kind::task: {
      const auto &b = entry(f.body_type);
      const bool fallible = is_generic(f.body_type, "result") && b.args.size() == 2;
      const bool fits =
          is_generic(f.return_type, "task") && r.args.size() >= 2 &&
          (fallible ? r.args[0] == b.args[0] && r.args[1] == b.args[1]
                    : r.args[0] == f.body_type && is_builtin(r.args[1], "never"));
      if (!fits) {
        report("F2", f.span,
               std::format("an async function returns `{}`, which is not the task of body type `{}`",
                           show(f.return_type), show(f.body_type)));
      }
      return;
    }
    }
  }

  auto check_function(const function &f) -> void {
    function_ = f.name;
    generic_params_ = {f.generic_params.begin(), f.generic_params.end()};
    introduced_.clear();
    env_.clear();
    frame_ = body_frame{.return_type = f.body_type,
                        .generator = f.coroutine == coroutine_kind::generator,
                        .yield_type = f.yield_type};
    check_type(f.return_type, f.span, "the return type");
    check_type(f.body_type, f.span, "the body type");
    if (frame_.generator) {
      check_type(f.yield_type, f.span, "the yield type");
    }
    auto param_types = std::vector<type_id>{};
    for (const auto &p : f.params) {
      check_type(p.type, f.span, std::format("parameter `{}`", p.name));
      introduce(p.symbol, p.type, writability::writable, local_kind::param, f.span);
      param_types.push_back(p.type);
    }
    if (const auto problem = receiver_problem(f.receiver, param_types)) {
      report("F3", f.span, *problem);
    }
    check_coroutine_types(f);
    if (f.body == nullptr) {
      report("F0", f.span, "a function has no body");
      return;
    }
    check_expr(*f.body);
    if (!diverges(types_, f.body.get())) {
      expect_type("F2", f.body->span, f.body->type, f.body_type, "the function body");
    }
  }

  /// F4: the first node of a static's value that is not a literal, an
  /// aggregate or variant construction, or a call.
  [[nodiscard]] static auto non_constant_part(const expr &e) -> const expr * {
    const auto first_of = [](const ptr_vec<expr> &parts) -> const expr * {
      for (const auto &part : parts) {
        if (part == nullptr) {
          continue;
        }
        if (const auto *bad = non_constant_part(*part)) {
          return bad;
        }
      }
      return nullptr;
    };
    switch (e.kind) {
    case node_kind::literal:
      return nullptr;
    case node_kind::tuple_init:
    case node_kind::array_init:
    case node_kind::struct_init: {
      const auto *a = as<aggregate_init>(e);
      return a == nullptr ? &e : first_of(a->elements);
    }
    case node_kind::variant_init: {
      const auto *v = as<variant_init>(e);
      return v == nullptr ? &e : first_of(v->payload);
    }
    case node_kind::call: {
      const auto *c = as<call>(e);
      if (c == nullptr) {
        return &e;
      }
      if (c->target.value != nullptr) {
        if (const auto *bad = non_constant_part(*c->target.value)) {
          return bad;
        }
      }
      return first_of(c->args);
    }
    default:
      return &e;
    }
  }

  auto check_static(const static_global &s) -> void {
    function_.clear();
    generic_params_.clear();
    introduced_.clear();
    env_.clear();
    frame_ = body_frame{};
    check_type(s.type, {}, std::format("static `{}`", s.name));
    if (s.value == nullptr) {
      report("F4", {}, std::format("static `{}` has no value", s.name));
      return;
    }
    check_expr(*s.value);
    if (!diverges(types_, s.value.get())) {
      expect_type("F4", s.value->span, s.value->type, s.type,
                  std::format("the value of static `{}`", s.name));
    }
    if (const auto *bad = non_constant_part(*s.value)) {
      report("F4", bad->span,
             std::format("the value of static `{}` contains a node that is not a literal, "
                         "construction or call",
                         s.name));
    }
  }
};

} // namespace

auto verify(std::span<const module *const> program, const semantic::checked_types &checked)
    -> std::vector<violation> {
  return verifier(program, checked).run();
}

} // namespace cinder::core
