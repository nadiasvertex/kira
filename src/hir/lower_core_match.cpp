#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "src/hir/lower_core_lowerer.h"

/// The forms that arrive as a core `match_expr`: `match`, `if let`,
/// `while let`, a pattern `let`, `or else`, `?` and a `for` loop's element.
/// Each is lowered to the HIR shape the old lowerer builds for it, which
/// storage lowering and the reference check read.
namespace cinder::hir {

namespace {

[[nodiscard]] auto find_shape(const semantic::checked_types &checked,
                              type_id type)
    -> const semantic::aggregate_shape * {
  if (const auto found = checked.shapes.find(type);
      found != checked.shapes.end()) {
    return &found->second;
  }
  if (const auto found = checked.open_shapes.find(type);
      found != checked.open_shapes.end()) {
    return &found->second;
  }
  return nullptr;
}

/// The name of the local the old lowerer binds a subject of `form` to.
[[nodiscard]] auto subject_name(core::match_form form) -> std::string {
  using f = core::match_form;
  switch (form) {
  case f::match: return "<match subject>";
  case f::if_let: return "<if subject>";
  case f::while_let: return "<while subject>";
  case f::let_pattern: return "<let subject>";
  case f::or_else: return "<or else subject>";
  case f::try_operator: return "<try subject>";
  case f::for_element:
  case f::for_next:
  case f::for_option: return "<for subject>";
  }
  return "<match subject>";
}

/// Whether `test` or a part of it leaves a part for a drop
/// (`leftover_drops`).
[[nodiscard]] auto leaves_parts(const core::pattern_test &test,
                                const drop_schedule &schedule) -> bool {
  if (test.origin != nullptr && schedule.leftover_drops.contains(test.origin)) {
    return true;
  }
  for (const auto &part : test.parts) {
    if (leaves_parts(part, schedule)) {
      return true;
    }
  }
  return false;
}

/// The arm an elaboration made up for "anything else".
[[nodiscard]] auto is_fallback(const core::match_arm &arm) -> bool {
  return arm.origin == nullptr && arm.alternatives.size() == 1 &&
         arm.alternatives.front().test.kind == core::pattern_test::form::any &&
         arm.alternatives.front().bindings.empty() && arm.guard == nullptr;
}

} // namespace

auto core_lowerer::subject_type(const core::match_expr &source) const
    -> type_id {
  const auto &path = source.subject.path;
  const auto kept = path.size() - std::min(path.size(), source.subject_derefs);
  return hir_type(kept == 0 ? source.subject.root.type : path[kept - 1].type);
}

auto core_lowerer::subject_value(const core::match_expr &source)
    -> core_result<ptr<hir_expr>> {
  auto made = place(source.subject, source.span);
  if (!made.has_value()) {
    return made;
  }
  for (std::size_t i = 0; i < source.subject_derefs; ++i) {
    auto *deref = dynamic_cast<hir_unary *>(made->get());
    if (deref == nullptr || deref->op != ast::unary_op::deref) {
      return refuse("match subject without its added derefs", source.span);
    }
    made = std::move(deref->operand);
  }
  if (source.subject_origin == nullptr) {
    return made;
  }
  // A temporary subject was read by `expr`, keyed by its own origin.
  const auto &root = source.subject.root;
  if (root.kind == core::place_root::origin::temporary &&
      root.value != nullptr && root.value->origin == source.subject_origin) {
    return made;
  }
  if ((*made)->origin == nullptr) {
    (*made)->origin = source.subject_origin;
  }
  auto cleared = drops_.clear_moved(source.subject_origin, std::move(*made));
  if (!cleared.has_value()) {
    return refuse_drop(cleared.error());
  }
  return std::move(*cleared);
}

auto core_lowerer::pattern(const core::pattern_test &test,
                           const place_fn &part,
                           std::vector<pattern_step> &path,
                           const core::pattern_alternative &alternative,
                           std::size_t &next_binding, bool own_bindings,
                           ptr_vec<hir_node> &pending)
    -> core_result<ptr<hir_pattern>> {
  using form = core::pattern_test::form;
  const auto span = test.span;
  // The parts first, each through its projection of `part`.
  const auto parts = [&](auto projection, auto step_of)
      -> core_result<ptr_vec<hir_pattern>> {
    auto made = ptr_vec<hir_pattern>{};
    for (std::size_t i = 0; i < test.parts.size(); ++i) {
      const auto &inner = test.parts[i];
      const auto inner_type = hir_type(inner.subject_type);
      const place_fn inner_place = [part, projection, inner_type, i,
                                    span]() -> ptr<hir_expr> {
        return projection(part(), inner_type, i, span);
      };
      path.push_back(step_of(i));
      auto lowered = pattern(inner, inner_place, path, alternative,
                             next_binding, own_bindings, pending);
      path.pop_back();
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      made.push_back(std::move(*lowered));
    }
    return made;
  };
  const auto positional = [](ptr<hir_expr> of, type_id type, std::size_t i,
                             source_span at) -> ptr<hir_expr> {
    return hir::make<hir_tuple_index>(at, type, std::move(of), i);
  };
  auto made = ptr<hir_pattern>{};
  switch (test.kind) {
  case form::any:
    // `_` binds nothing, so nothing else would drop this part.
    if (test.origin != nullptr &&
        drops_.schedule().leftover_drops.contains(test.origin)) {
      auto dropped = drops_.build_drop_calls(part, hir_type(test.subject_type),
                                             span, pending);
      if (!dropped.has_value()) {
        return refuse_drop(dropped.error());
      }
    }
    made = hir::make<hir_wildcard_pattern>(span);
    break;
  case form::literal:
    made = hir::make<hir_literal_pattern>(span, test.low->token,
                                          test.low->spelling);
    break;
  case form::range: {
    auto low = ptr<hir_expr>{};
    auto high = ptr<hir_expr>{};
    if (test.low != nullptr) {
      auto lowered = literal(*test.low);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      low = std::move(*lowered);
    }
    if (test.high != nullptr) {
      auto lowered = literal(*test.high);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      high = std::move(*lowered);
    }
    made = hir::make<hir_range_pattern>(span, std::move(low), std::move(high),
                                        test.high_inclusive);
    break;
  }
  case form::variant: {
    const auto *shape = find_shape(checked_, test.subject_type);
    if (shape == nullptr || test.variant_index >= shape->variants.size()) {
      return refuse("variant test with no shape", span);
    }
    const auto name = shape->variants[test.variant_index].name;
    auto args = parts(
        [name](ptr<hir_expr> of, type_id type, std::size_t i,
               source_span at) -> ptr<hir_expr> {
          return hir::make<hir_variant_payload>(at, type, std::move(of), name,
                                                i);
        },
        [&](std::size_t i) {
          return pattern_step{.kind = core::projection::step::variant_payload,
                              .index = i,
                              .name = name};
        });
    if (!args.has_value()) {
      return std::unexpected(args.error());
    }
    for (std::size_t i = 0; i < args->size(); ++i) {
      (*args)[i]->subject_type = hir_type(test.parts[i].subject_type);
    }
    made = hir::make<hir_constructor_pattern>(span, name, std::move(*args));
    break;
  }
  case form::tuple:
  case form::array: {
    const auto step = test.kind == form::tuple
                          ? core::projection::step::tuple_element
                          : core::projection::step::element;
    auto elements = parts(positional, [&](std::size_t i) {
      return pattern_step{.kind = step, .index = i, .name = {}};
    });
    if (!elements.has_value()) {
      return std::unexpected(elements.error());
    }
    if (test.kind == form::tuple) {
      made = hir::make<hir_tuple_pattern>(span, std::move(*elements));
    } else {
      made = hir::make<hir_array_pattern>(span, std::move(*elements));
    }
    break;
  }
  case form::record: {
    const auto *shape = find_shape(checked_, test.subject_type);
    if (shape == nullptr) {
      return refuse("record test with no shape", span);
    }
    auto names = std::vector<std::string>{};
    for (const auto index : test.field_indices) {
      if (index >= shape->fields.size()) {
        return refuse("record test with no such field", span);
      }
      names.push_back(shape->fields[index].name);
    }
    auto lowered = parts(
        [&names](ptr<hir_expr> of, type_id type, std::size_t i,
                 source_span at) -> ptr<hir_expr> {
          return hir::make<hir_field>(at, type, std::move(of), names[i]);
        },
        [&](std::size_t i) {
          return pattern_step{.kind = core::projection::step::field,
                              .index = test.field_indices[i],
                              .name = {}};
        });
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    auto fields = std::vector<hir_struct_pattern_field>{};
    for (std::size_t i = 0; i < lowered->size(); ++i) {
      // A shorthand `{x}` field has no pattern of its own.
      if (test.parts[i].origin != nullptr) {
        (*lowered)[i]->subject_type = hir_type(test.parts[i].subject_type);
      }
      fields.push_back(hir_struct_pattern_field{
          .name = names[i], .pattern = std::move((*lowered)[i])});
    }
    // `..` binds nothing, so the fields it skips would never be dropped.
    const auto struct_type = hir_type(test.subject_type);
    if (test.origin != nullptr &&
        drops_.schedule().leftover_drops.contains(test.origin)) {
      if (const auto plan = checked_.drop.drop_plans.find(struct_type);
          plan != checked_.drop.drop_plans.end()) {
        for (const auto &[field_name, field_type] :
             plan->second.droppable_fields) {
          if (std::ranges::find(names, field_name) != names.end()) {
            continue;
          }
          const place_fn skipped = [part, field_name, field_type,
                                    span]() -> ptr<hir_expr> {
            return hir::make<hir_field>(span, field_type, part(), field_name);
          };
          auto dropped =
              drops_.build_drop_calls(skipped, field_type, span, pending);
          if (!dropped.has_value()) {
            return refuse_drop(dropped.error());
          }
        }
      }
    }
    made = hir::make<hir_struct_pattern>(span, std::move(fields));
    break;
  }
  }
  // Then the names bound to this part, in the order the pattern wrote them.
  const auto at_path = [&](const core::pattern_binding &binding) {
    if (binding.path.size() != path.size()) {
      return false;
    }
    for (std::size_t i = 0; i < path.size(); ++i) {
      const auto &step = binding.path[i];
      if (step.kind != path[i].kind) {
        return false;
      }
      switch (step.kind) {
      case core::projection::step::element:
        if (step.subscript == nullptr ||
            step.subscript->kind != core::node_kind::literal ||
            static_cast<const core::literal &>(*step.subscript).spelling !=
                std::to_string(path[i].index)) {
          return false;
        }
        break;
      case core::projection::step::variant_payload:
        if (step.index != path[i].index || step.name != path[i].name) {
          return false;
        }
        break;
      case core::projection::step::deref:
        break;
      default:
        if (step.index != path[i].index) {
          return false;
        }
      }
    }
    return true;
  };
  const auto &bindings = alternative.bindings;
  while (next_binding < bindings.size() && at_path(bindings[next_binding])) {
    const auto &binding = bindings[next_binding++];
    if (binding.origin == nullptr) {
      // A name the elaboration made up: the part itself.
      aliases_.insert_or_assign(binding.symbol, part);
      continue;
    }
    const auto symbol = declare(binding.symbol, binding.name);
    pending.push_back(
        hir::make<hir_let>(binding.span, symbol, binding.name, part()));
    if (own_bindings &&
        drops_.schedule().owned_bindings.contains(binding.origin)) {
      own(binding.origin, symbol);
      drops_.declare_flags(binding.origin, binding.span, pending);
    }
  }
  return made;
}

auto core_lowerer::alternative_pattern(
    const core::pattern_alternative &alternative, const place_fn &subject,
    bool own_bindings, ptr_vec<hir_node> &pending)
    -> core_result<ptr<hir_pattern>> {
  auto path = std::vector<pattern_step>{};
  auto next = std::size_t{0};
  auto made = pattern(alternative.test, subject, path, alternative, next,
                      own_bindings, pending);
  if (made.has_value() && next != alternative.bindings.size()) {
    return refuse("pattern bindings out of order", alternative.test.span);
  }
  return made;
}

auto core_lowerer::arm_into(const core::match_expr &source,
                            const core::match_arm &arm,
                            const place_fn &subject, type_id body_type,
                            std::vector<hir_match_arm> &out)
    -> core_result<void> {
  // An or-pattern stays one arm unless an alternative binds a name or
  // drops a part; then each alternative is an arm with its own copy of the
  // body.
  auto split = false;
  if (arm.alternatives.size() > 1) {
    for (const auto &alternative : arm.alternatives) {
      split = split || !alternative.bindings.empty() ||
              leaves_parts(alternative.test, drops_.schedule());
    }
  }
  const auto one_arm = [&](std::span<const core::pattern_alternative>
                               alternatives) -> core_result<void> {
    push_scope();
    auto done = [&]() -> core_result<void> {
      auto pending = ptr_vec<hir_node>{};
      // The arm owns the whole subject: a `<subject>` local drops it with
      // the arm's scope.
      if (arm.origin != nullptr &&
          drops_.schedule().owning_subjects.contains(arm.origin)) {
        const auto symbol = mint();
        names_.insert_or_assign(symbol, "<subject>");
        pending.push_back(
            hir::make<hir_let>(arm.span, symbol, "<subject>", subject()));
        own(arm.origin, symbol);
        drops_.declare_flags(arm.origin, arm.span, pending);
      }
      auto patterns = ptr_vec<hir_pattern>{};
      for (const auto &alternative : alternatives) {
        auto lowered =
            alternative_pattern(alternative, subject, true, pending);
        if (!lowered.has_value()) {
          return std::unexpected(lowered.error());
        }
        patterns.push_back(std::move(*lowered));
      }
      auto made = patterns.size() == 1
                      ? std::move(patterns.front())
                      : ptr<hir_pattern>(hir::make<hir_or_pattern>(
                            arm.span, std::move(patterns)));
      auto guard = ptr<hir_expr>{};
      if (arm.guard != nullptr) {
        ++conditional_depth_;
        auto lowered = expr(*arm.guard);
        --conditional_depth_;
        if (!lowered.has_value()) {
          return std::unexpected(lowered.error());
        }
        guard = std::move(*lowered);
        // The guard reads the arm's bindings, so their `hir_let`s run
        // ahead of it.
        if (!pending.empty()) {
          const auto guard_span = guard->span;
          const auto guard_type = guard->type;
          auto stmts = std::move(pending);
          pending.clear();
          stmts.push_back(hir::make<hir_expr_stmt>(guard_span, std::move(guard)));
          guard = hir::make<hir_block>(guard_span, guard_type, std::move(stmts));
        }
      }
      auto body = block_in_scope(*arm.body, body_type);
      if (!body.has_value()) {
        return std::unexpected(body.error());
      }
      if (!pending.empty()) {
        for (auto &stmt : (*body)->stmts) {
          pending.push_back(std::move(stmt));
        }
        (*body)->stmts = std::move(pending);
      }
      out.push_back(hir_match_arm{.pattern = std::move(made),
                                  .guard = std::move(guard),
                                  .body = std::move(*body)});
      return {};
    }();
    pop_scope();
    return done;
  };
  (void)source;
  if (!split) {
    return one_arm(arm.alternatives);
  }
  for (std::size_t i = 0; i < arm.alternatives.size(); ++i) {
    if (auto done = one_arm(std::span(arm.alternatives).subspan(i, 1)); !done) {
      return done;
    }
  }
  return {};
}

auto core_lowerer::match(const core::match_expr &source, type_id type)
    -> core_result<ptr<hir_match>> {
  using f = core::match_form;
  // `?`'s failure arm leaves as the old lowerer's does: the crews it leaves
  // end and the drops run before the value handed back is made.
  if (source.form == f::try_operator && source.arms.size() == 2 &&
      source.arms[1].body != nullptr &&
      !source.arms[1].body->statements.empty()) {
    try_failure_exit_ =
        exit_block(source.arms[1].body->statements.front().get());
  }
  auto value = subject_value(source);
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  const auto symbol = mint();
  const auto name = subject_name(source.form);
  names_.insert_or_assign(symbol, name);
  const auto stype = subject_type(source);
  const auto subject_span = source.subject.root.kind ==
                                    core::place_root::origin::temporary &&
                                    source.subject.root.value != nullptr
                                ? source.subject.root.value->span
                                : source.span;
  const place_fn subject = [symbol, stype, subject_span,
                            name]() -> ptr<hir_expr> {
    return hir::make<hir_local_ref>(subject_span, stype, symbol, name);
  };
  // The other variant of an `option` or `result`, as a constructor
  // pattern: the arm `or else` and a `for` over an `option` take when the
  // value is absent.
  const auto other_variant = [&](const core::match_arm &first)
      -> core_result<ptr<hir_pattern>> {
    const auto &test = first.alternatives.front().test;
    const auto *shape = find_shape(checked_, test.subject_type);
    if (test.kind != core::pattern_test::form::variant || shape == nullptr ||
        shape->variants.size() != 2) {
      return refuse("fallback of a match on no wrapper", source.span);
    }
    const auto &variant = shape->variants[1 - test.variant_index];
    auto args = ptr_vec<hir_pattern>{};
    if (!variant.payload.empty()) {
      args.push_back(hir::make<hir_wildcard_pattern>(source.span));
    }
    return ptr<hir_pattern>(hir::make<hir_constructor_pattern>(
        source.span, variant.name, std::move(args)));
  };
  auto arms = std::vector<hir_match_arm>{};
  for (std::size_t i = 0; i < source.arms.size(); ++i) {
    const auto &arm = source.arms[i];
    // A `for` over an `option`: the body runs on `@some`.
    if (source.form == f::for_option && i == 0) {
      const auto &alternative = arm.alternatives.front();
      if (alternative.test.parts.size() != 1) {
        return refuse("option loop with no payload test", source.span);
      }
      const auto element_type = hir_type(alternative.test.parts[0].subject_type);
      const auto span = source.span;
      const place_fn element = [subject, element_type, span]() -> ptr<hir_expr> {
        return hir::make<hir_variant_payload>(span, element_type, subject(),
                                              "some", 0);
      };
      push_scope();
      auto stmts = ptr_vec<hir_node>{};
      auto done = loop_body_into(
          arm, element, alternative.test.parts[0],
          {pattern_step{.kind = core::projection::step::variant_payload,
                        .index = 0,
                        .name = "some"}},
          true, stmts);
      pop_scope();
      if (!done.has_value()) {
        return std::unexpected(done.error());
      }
      auto args = ptr_vec<hir_pattern>{};
      args.push_back(hir::make<hir_wildcard_pattern>(span));
      arms.push_back(hir_match_arm{
          .pattern = hir::make<hir_constructor_pattern>(span, "some",
                                                        std::move(args)),
          .guard = nullptr,
          .body = hir::make<hir_block>(span, k_unknown_type, std::move(stmts))});
      continue;
    }
    // `?` with nothing to convert hands back the subject itself.
    if (source.form == f::try_operator && i == 1 &&
        arm.body->statements.size() == 1 &&
        arm.body->statements[0]->kind == core::node_kind::return_stmt) {
      const auto &leave =
          static_cast<const core::jump &>(*arm.body->statements[0]);
      const auto *rebuilt = leave.value == nullptr
                                ? nullptr
                                : dynamic_cast<const core::variant_init *>(
                                      leave.value.get());
      const auto unconverted =
          rebuilt != nullptr &&
          (rebuilt->payload.empty() ||
           (rebuilt->payload.size() == 1 &&
            rebuilt->payload[0]->kind == core::node_kind::use &&
            !arm.alternatives.front().bindings.empty() &&
            static_cast<const core::use &>(*rebuilt->payload[0])
                    .from.root.symbol ==
                arm.alternatives.front().bindings.front().symbol));
      if (unconverted) {
        auto pending = ptr_vec<hir_node>{};
        auto made = alternative_pattern(arm.alternatives.front(), subject,
                                        true, pending);
        if (!made.has_value()) {
          return std::unexpected(made.error());
        }
        auto stmts = ptr_vec<hir_node>{};
        if (leave.origin != nullptr) {
          auto dropped = drops_.emit_jump_drops(
              leave.origin, jump_exit::function, leave.span, stmts);
          if (!dropped.has_value()) {
            return refuse_drop(dropped.error());
          }
        }
        // A task hands the failure back with a `yield`, then finishes.
        if (in_task_body_) {
          stmts.push_back(hir::make<hir_yield>(leave.span, subject()));
          stmts.push_back(hir::make<hir_return>(leave.span, nullptr));
        } else {
          stmts.push_back(hir::make<hir_return>(leave.span, subject()));
        }
        arms.push_back(hir_match_arm{
            .pattern = std::move(*made),
            .guard = nullptr,
            .body = hir::make<hir_block>(source.span, k_unknown_type,
                                         std::move(stmts))});
        continue;
      }
    }
    if (i == 0 || !is_fallback(arm)) {
      const auto body_type = source.form == f::or_else ||
                                     source.form == f::try_operator
                                 ? k_unknown_type
                                 : type;
      if (auto done = arm_into(source, arm, subject, body_type, arms); !done) {
        return std::unexpected(done.error());
      }
      continue;
    }
    // The arm for anything else.
    auto made = ptr<hir_pattern>{};
    if (source.form == f::or_else || source.form == f::for_option) {
      auto other = other_variant(source.arms.front());
      if (!other.has_value()) {
        return std::unexpected(other.error());
      }
      made = std::move(*other);
    } else {
      made = hir::make<hir_wildcard_pattern>(arm.span);
    }
    auto body = ptr<hir_block>{};
    // A jump as `or else`'s fallback is the arm's statements.
    const auto *jumped =
        source.form == f::or_else && arm.body->result != nullptr &&
                arm.body->result->kind == core::node_kind::block
            ? static_cast<const core::block *>(arm.body->result.get())
            : nullptr;
    if (jumped != nullptr && jumped->result == nullptr &&
        jumped->statements.size() == 1 &&
        (jumped->statements[0]->kind == core::node_kind::return_stmt ||
         jumped->statements[0]->kind == core::node_kind::break_stmt ||
         jumped->statements[0]->kind == core::node_kind::continue_stmt)) {
      auto lowered = block(*jumped, k_unknown_type);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      body = std::move(*lowered);
    } else {
      auto lowered =
          block(*arm.body, source.form == f::or_else ? k_unknown_type : type);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      body = std::move(*lowered);
    }
    // The subject was moved into the test and nothing bound it.
    const auto *key = source.form == f::or_else ? source.origin
                                                : source.arms.front().origin;
    if (key != nullptr && drops_.schedule().unmatched_drops.contains(key)) {
      auto drops = ptr_vec<hir_node>{};
      auto dropped = drops_.build_drop_calls(subject, stype, source.span, drops);
      if (!dropped.has_value()) {
        return refuse_drop(dropped.error());
      }
      for (auto &stmt : body->stmts) {
        drops.push_back(std::move(stmt));
      }
      body->stmts = std::move(drops);
    }
    arms.push_back(hir_match_arm{
        .pattern = std::move(made), .guard = nullptr, .body = std::move(body)});
  }
  return hir::make<hir_match>(source.span, type, std::move(*value), symbol,
                              std::move(arms));
}

auto core_lowerer::let_pattern_into(const core::match_expr &source,
                                    ptr_vec<hir_node> &out)
    -> core_result<const core::block *> {
  const auto &arm = source.arms.front();
  if (arm.alternatives.size() != 1 || arm.guard != nullptr) {
    return refuse("pattern let with alternatives", source.span);
  }
  auto value = subject_value(source);
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  const auto symbol = mint();
  const auto name = std::string("<let subject>");
  names_.insert_or_assign(symbol, name);
  const auto stype = subject_type(source);
  const auto span = source.span;
  const place_fn subject = [symbol, stype, span, name]() -> ptr<hir_expr> {
    return hir::make<hir_local_ref>(span, stype, symbol, name);
  };
  // The bindings live in the enclosing block's scope.
  auto pending = ptr_vec<hir_node>{};
  if (arm.origin != nullptr &&
      drops_.schedule().owning_subjects.contains(arm.origin)) {
    const auto whole = mint();
    names_.insert_or_assign(whole, "<subject>");
    pending.push_back(hir::make<hir_let>(span, whole, "<subject>", subject()));
    own(arm.origin, whole);
    drops_.declare_flags(arm.origin, span, pending);
  }
  auto made =
      alternative_pattern(arm.alternatives.front(), subject, true, pending);
  if (!made.has_value()) {
    return std::unexpected(made.error());
  }
  if (source.arms.size() == 1) {
    out.push_back(hir::make<hir_let>(span, symbol, name, std::move(*value)));
  } else {
    auto otherwise = block(*source.arms[1].body, k_unknown_type);
    if (!otherwise.has_value()) {
      return std::unexpected(otherwise.error());
    }
    if (arm.origin != nullptr &&
        drops_.schedule().unmatched_drops.contains(arm.origin)) {
      auto drops = ptr_vec<hir_node>{};
      auto dropped = drops_.build_drop_calls(subject, stype, span, drops);
      if (!dropped.has_value()) {
        return refuse_drop(dropped.error());
      }
      for (auto &stmt : (*otherwise)->stmts) {
        drops.push_back(std::move(stmt));
      }
      (*otherwise)->stmts = std::move(drops);
    }
    out.push_back(hir::make<hir_let_else>(span, symbol, std::move(*value),
                                          std::move(*made),
                                          std::move(*otherwise)));
  }
  for (auto &stmt : pending) {
    out.push_back(std::move(stmt));
  }
  return arm.body.get();
}

auto core_lowerer::loop_body_into(const core::match_arm &arm,
                                  const place_fn &element,
                                  const core::pattern_test &head,
                                  std::vector<pattern_step> prefix,
                                  bool own_plain, ptr_vec<hir_node> &out)
    -> core_result<void> {
  const auto &alternative = arm.alternatives.front();
  const auto &bindings = alternative.bindings;
  const auto span = arm.span;
  const auto plain = head.kind == core::pattern_test::form::any &&
                     bindings.size() == 1 &&
                     bindings.front().path.size() == prefix.size() &&
                     bindings.front().origin != nullptr &&
                     !drops_.schedule().leftover_drops.contains(head.origin);
  if (plain) {
    const auto &binding = bindings.front();
    const auto symbol = declare(binding.symbol, binding.name);
    out.push_back(hir::make<hir_let>(span, symbol, binding.name, element()));
    if (own_plain &&
        drops_.schedule().owned_bindings.contains(binding.origin)) {
      own(binding.origin, symbol);
      drops_.declare_flags(binding.origin, span, out);
    }
  } else {
    // A destructuring head binds the element once, then reads the names
    // out of it.
    const auto symbol = mint();
    const auto name = std::format("<for subject {}>", symbol);
    names_.insert_or_assign(symbol, name);
    auto value = element();
    const auto type = value->type;
    out.push_back(hir::make<hir_let>(span, symbol, name, std::move(value)));
    const place_fn subject = [symbol, type, span, name]() -> ptr<hir_expr> {
      return hir::make<hir_local_ref>(span, type, symbol, name);
    };
    auto next = std::size_t{0};
    auto made =
        pattern(head, subject, prefix, alternative, next, true, out);
    if (!made.has_value()) {
      return std::unexpected(made.error());
    }
    if (next != bindings.size()) {
      return refuse("loop pattern bindings out of order", span);
    }
  }
  // The body's own block, spliced; a guard wraps it in an `if`.
  if (arm.body->origin != nullptr) {
    auto body = block(*arm.body, k_unknown_type);
    if (!body.has_value()) {
      return std::unexpected(body.error());
    }
    for (auto &stmt : (*body)->stmts) {
      out.push_back(std::move(stmt));
    }
    return {};
  }
  for (const auto &stmt : arm.body->statements) {
    if (auto done = stmt_into(*stmt, out, std::nullopt); !done) {
      return done;
    }
  }
  return {};
}

auto core_lowerer::for_element_into(const core::match_expr &source,
                                    ptr_vec<hir_node> &out)
    -> core_result<void> {
  if (source.arms.size() != 1 || source.arms[0].alternatives.size() != 1) {
    return refuse("loop element match", source.span);
  }
  auto element = place(source.subject, source.span);
  if (!element.has_value()) {
    return std::unexpected(element.error());
  }
  // The element is read once, by the one `let` that binds it.
  auto held = std::make_shared<ptr<hir_expr>>(std::move(*element));
  const place_fn read = [held]() -> ptr<hir_expr> { return std::move(*held); };
  const auto &arm = source.arms[0];
  return loop_body_into(arm, read, arm.alternatives.front().test, {}, false,
                        out);
}

auto core_lowerer::while_let_into(const core::match_expr &source,
                                  ptr_vec<hir_node> &out)
    -> core_result<void> {
  using f = core::match_form;
  if (source.arms.size() != 2 || !is_fallback(source.arms[1])) {
    return refuse("while let shape", source.span);
  }
  // In a task, what the subject waits for runs at the top of every
  // iteration.
  auto hoisted = ptr_vec<hir_node>{};
  auto value = [&] {
    if (source.form == f::while_let && in_task_body_ && hoist_ != nullptr) {
      auto *const enclosing = std::exchange(hoist_, &hoisted);
      auto made = subject_value(source);
      hoist_ = enclosing;
      return made;
    }
    return subject_value(source);
  }();
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  const auto symbol = mint();
  const auto name = subject_name(source.form);
  names_.insert_or_assign(symbol, name);
  const auto stype = subject_type(source);
  const auto span = source.span;
  const place_fn subject = [symbol, stype, span, name]() -> ptr<hir_expr> {
    return hir::make<hir_local_ref>(span, stype, symbol, name);
  };
  const auto &arm = source.arms.front();
  if (source.form == f::for_next) {
    const auto &test = arm.alternatives.front().test;
    if (test.parts.size() != 1) {
      return refuse("for loop with no element test", span);
    }
    const auto payload_type = hir_type(test.parts[0].subject_type);
    // A loop over a borrowed collection of `copy` elements gets each `&T`
    // from `iter` and binds the element it points at
    // (`iterator_loop_dispatch::copy_elements`): the name's path goes
    // through a dereference of the payload.
    const auto &head_bindings = arm.alternatives.front().bindings;
    const auto copied =
        test.parts[0].kind == core::pattern_test::form::any &&
        head_bindings.size() == 1 && head_bindings.front().path.size() == 2 &&
        head_bindings.front().path[1].kind == core::projection::step::deref;
    const auto element_type =
        copied ? hir_type(head_bindings.front().type) : payload_type;
    const place_fn element = [subject, element_type, payload_type, copied,
                              span]() -> ptr<hir_expr> {
      auto payload = ptr<hir_expr>(hir::make<hir_variant_payload>(
          span, payload_type, subject(), "some", 0));
      if (!copied) {
        return payload;
      }
      return hir::make<hir_unary>(span, element_type, ast::unary_op::deref,
                                  std::move(payload));
    };
    auto prefix = std::vector<pattern_step>{
        pattern_step{.kind = core::projection::step::variant_payload,
                     .index = 0,
                     .name = "some"}};
    if (copied) {
      prefix.push_back(pattern_step{
          .kind = core::projection::step::deref, .index = 0, .name = {}});
    }
    push_scope();
    auto stmts = ptr_vec<hir_node>{};
    auto done = loop_body_into(arm, element, test.parts[0], std::move(prefix),
                               true, stmts);
    pop_scope();
    if (!done.has_value()) {
      return done;
    }
    auto args = ptr_vec<hir_pattern>{};
    args.push_back(hir::make<hir_wildcard_pattern>(span));
    out.push_back(hir::make<hir_while_let>(
        span, std::move(*value), symbol,
        hir::make<hir_constructor_pattern>(span, "some", std::move(args)),
        hir::make<hir_block>(span, k_unknown_type, std::move(stmts))));
    return {};
  }
  auto arms = std::vector<hir_match_arm>{};
  if (auto done = arm_into(source, arm, subject, k_unknown_type, arms); !done) {
    return done;
  }
  if (arms.size() != 1) {
    return refuse("while let with binding alternatives", span);
  }
  const auto drops_unmatched =
      drops_.schedule().unmatched_drops.contains(arm.origin);
  if (!drops_unmatched && hoisted.empty()) {
    out.push_back(hir::make<hir_while_let>(span, std::move(*value), symbol,
                                           std::move(arms[0].pattern),
                                           std::move(arms[0].body)));
    return {};
  }
  // A miss still owns the subject: `while true: match subject: pattern =>
  // body, _ => drop it and break`. A subject that waits for a task takes
  // the same form, with the wait at the top of the loop.
  auto exit = ptr_vec<hir_node>{};
  if (drops_unmatched) {
    auto dropped = drops_.build_drop_calls(subject, stype, span, exit);
    if (!dropped.has_value()) {
      return refuse_drop(dropped.error());
    }
  }
  exit.push_back(hir::make<hir_break>(span));
  arms.push_back(hir_match_arm{
      .pattern = hir::make<hir_wildcard_pattern>(span),
      .guard = nullptr,
      .body = hir::make<hir_block>(span, k_unknown_type, std::move(exit))});
  auto body = std::move(hoisted);
  body.push_back(hir::make<hir_expr_stmt>(
      span, hir::make<hir_match>(span, k_unknown_type, std::move(*value),
                                 symbol, std::move(arms))));
  out.push_back(hir::make<hir_while>(
      span,
      hir::make<hir_literal>(span, checked_.types.bool_type(),
                             token_kind::kw_true, "true"),
      hir::make<hir_block>(span, k_unknown_type, std::move(body))));
  return {};
}

} // namespace cinder::hir
