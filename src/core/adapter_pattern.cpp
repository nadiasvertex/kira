#include <format>
#include <string>
#include <utility>
#include <vector>

#include "src/core/adapter_builder.h"
#include "src/parser/ast_traversal.h"

/// Patterns and the forms that elaborate to `match`: `match`, `if let`,
/// `while let`, `let ... else`, a destructuring `let`, `or else` and `?`.
namespace cinder::core {

using semantic::type_kind;

namespace {

[[nodiscard]] auto step_of(projection::step kind, type_id type) -> projection {
  auto step = projection{};
  step.kind = kind;
  step.type = type;
  return step;
}

/// A copy of `path` with `step` appended. Projections own their subscript,
/// so a path is copied step by step; a pattern's paths carry no subscript
/// expressions except the literal indices of an array pattern.
[[nodiscard]] auto extended(const std::vector<projection> &path,
                            projection step) -> std::vector<projection>;

[[nodiscard]] auto copy_step(const projection &from) -> projection {
  auto step = step_of(from.kind, from.type);
  step.name = from.name;
  step.index = from.index;
  step.in_bounds = from.in_bounds;
  if (from.subscript != nullptr && from.subscript->kind == node_kind::literal) {
    const auto &lit = static_cast<const literal &>(*from.subscript);
    auto copy = make_node<literal>(lit.span, lit.type);
    copy->token = lit.token;
    copy->spelling = lit.spelling;
    step.subscript = std::move(copy);
  }
  return step;
}

auto extended(const std::vector<projection> &path, projection step)
    -> std::vector<projection> {
  auto out = std::vector<projection>{};
  out.reserve(path.size() + 1);
  for (const auto &existing : path) {
    out.push_back(copy_step(existing));
  }
  out.push_back(std::move(step));
  return out;
}

[[nodiscard]] auto copy_path(const std::vector<projection> &path)
    -> std::vector<projection> {
  auto out = std::vector<projection>{};
  out.reserve(path.size());
  for (const auto &existing : path) {
    out.push_back(copy_step(existing));
  }
  return out;
}

[[nodiscard]] auto any_test(type_id type, source_span span) -> pattern_test {
  auto test = pattern_test{};
  test.kind = pattern_test::form::any;
  test.span = span;
  test.subject_type = type;
  return test;
}

[[nodiscard]] auto as_literal(ptr<expr> value) -> ptr<literal> {
  if (value == nullptr || value->kind != node_kind::literal) {
    return nullptr;
  }
  return ptr<literal>(static_cast<literal *>(value.release()));
}

/// The local a binding introduces, as a place.
[[nodiscard]] auto local_place(const pattern_binding &binding) -> place {
  auto p = place{};
  p.root.kind = place_root::origin::local;
  p.root.symbol = binding.symbol;
  p.root.name = binding.name;
  p.root.type = binding.type;
  return p;
}

} // namespace

// ==========================================================================
//  Tests and bindings
// ==========================================================================

auto body_builder::binding_of(const std::string &name, const void *key,
                              bool is_mut, type_id type,
                              const std::vector<projection> &path,
                              source_span span, binding_context &context)
    -> pattern_binding {
  auto made = pattern_binding{};
  if (const auto found = context.symbols.find(name);
      found != context.symbols.end()) {
    made.symbol = found->second;
  } else {
    made.symbol = mint();
    context.symbols.emplace(name, made.symbol);
  }
  made.name = name;
  made.type = type;
  made.path = copy_path(path);
  made.span = span;
  made.origin = key;
  // Rule A2 decides: a part that could not be moved out of the subject is
  // the part itself; otherwise the binding copies or moves it.
  auto access = context.subject_access;
  auto before = context.subject_type;
  for (const auto &step : path) {
    walk_access(access, before, step);
    before = step.type;
  }
  if (!access.movable) {
    made.how = pattern_binding::mode::alias;
  } else {
    made.how = checked_.is_copy(type) ? pattern_binding::mode::copy
                                      : pattern_binding::mode::move;
  }
  const auto declared_mut =
      is_mut && !checked_.immutable_pattern_bindings.contains(key);
  made.writable = made.how == pattern_binding::mode::alias
                      ? access.writable && (declared_mut || access.through_ref)
                      : declared_mut;
  return made;
}

auto body_builder::array_test_of(
    const std::vector<ast::ptr<ast::pattern>> &elements,
    const std::vector<projection> &path, binding_context &context,
    std::vector<pattern_binding> &bindings, pattern_test test,
    source_span span) -> result<pattern_test> {
  test.kind = pattern_test::form::array;
  for (std::size_t i = 0; i < elements.size(); ++i) {
    if (elements[i] == nullptr) {
      return fail("array_pattern", "a missing element", span);
    }
    const auto &element = *elements[i];
    const auto found = checked_.node_types.find(&element);
    if (found == checked_.node_types.end()) {
      return fail("missing record: node_types",
                  std::format("no type for a {}", kind_name(element.kind)),
                  element.span);
    }
    CORE_TRY(element_type, settle(found->second, element.span));
    auto subscript =
        make_node<literal>(element.span, checked_.types.usize_type());
    subscript->token = token_kind::int_lit;
    subscript->spelling = std::to_string(i);
    auto step = step_of(projection::step::element, element_type);
    step.subscript = std::move(subscript);
    CORE_TRY(part, test_of(element, element_type,
                           extended(path, std::move(step)), context, bindings));
    test.parts.push_back(std::move(part));
  }
  return test;
}

auto body_builder::test_of(const ast::node &pattern, type_id type,
                           const std::vector<projection> &path,
                           binding_context &context,
                           std::vector<pattern_binding> &bindings)
    -> result<pattern_test> {
  if (pattern.has_error) {
    return fail("error node", "the pattern carries a parse error",
                pattern.span);
  }
  const auto part_type = [&](const ast::node &part) -> result<type_id> {
    const auto found = checked_.node_types.find(&part);
    if (found == checked_.node_types.end()) {
      return fail("missing record: node_types",
                  std::format("no type for a {}", kind_name(part.kind)),
                  part.span);
    }
    return settle(found->second, part.span);
  };
  auto test = any_test(type, pattern.span);
  test.origin = &pattern;
  switch (pattern.kind) {
  case ast::node_kind::wildcard_pattern:
    return test;
  case ast::node_kind::binding_pattern: {
    const auto &binding = ast::cast<const ast::binding_pattern>(pattern);
    bindings.push_back(binding_of(binding.name, &pattern, binding.is_mut, type,
                                  path, pattern.span,
                                  context));
    return test;
  }
  case ast::node_kind::literal_pattern: {
    const auto &lit = ast::cast<const ast::literal_pattern>(pattern);
    test.kind = pattern_test::form::literal;
    test.low = make_node<literal>(pattern.span, type);
    test.low->token = lit.lit_kind;
    test.low->spelling = lit.value;
    return test;
  }
  case ast::node_kind::range_pattern: {
    const auto &range = ast::cast<const ast::range_pattern>(pattern);
    test.kind = pattern_test::form::range;
    test.high_inclusive = range.inclusive;
    for (const auto &[bound, out] :
         {std::pair{range.start.get(), &test.low},
          std::pair{range.end.get(), &test.high}}) {
      if (bound == nullptr) {
        continue;
      }
      CORE_TRY(value, value_as(*bound, type));
      auto lit = as_literal(std::move(value));
      if (lit == nullptr) {
        return fail("range pattern bound", "a bound that is not a literal",
                    bound->span);
      }
      *out = std::move(lit);
    }
    if (test.low == nullptr && test.high == nullptr) {
      return fail("range_pattern", "a range pattern with no bound",
                  pattern.span);
    }
    return test;
  }
  case ast::node_kind::group_pattern: {
    const auto &group = ast::cast<const ast::group_pattern>(pattern);
    if (group.inner == nullptr) {
      return fail("group_pattern", "no inner pattern", pattern.span);
    }
    CORE_TRY(inner, test_of(*group.inner, type, path, context, bindings));
    if (group.alias.has_value()) {
      bindings.push_back(binding_of(*group.alias, &group, false, type, path,
                                    pattern.span,
                                    context));
    }
    return inner;
  }
  case ast::node_kind::ref_pattern: {
    const auto &ref = ast::cast<const ast::ref_pattern>(pattern);
    if (ref.inner == nullptr) {
      return fail("ref_pattern", "no inner pattern", pattern.span);
    }
    const auto &entry = checked_.types.entry(type);
    if (entry.kind != type_kind::ref_kind) {
      return test_of(*ref.inner, type, path, context,
                     bindings);
    }
    // A test of the referenced value: the core has no test of a reference
    // itself, so the pattern's test moves past the `deref`.
    return fail("ref_pattern", "a `&` pattern on a reference", pattern.span);
  }
  case ast::node_kind::or_pattern: {
    // A nested `|`: the alternative `alternatives_of` chose for this
    // build. A destructuring parameter or a `for` pattern is not expanded.
    const auto &alternatives =
        ast::cast<const ast::or_pattern>(pattern).alternatives;
    if (or_choices_ == nullptr) {
      return fail("nested or pattern", "a `|` below the top of a pattern",
                  pattern.span);
    }
    if (alternatives.empty()) {
      return fail("or_pattern", "no alternatives", pattern.span);
    }
    const auto taken = or_choices_->taken.find(&pattern);
    const auto index =
        taken == or_choices_->taken.end() ? std::size_t{0} : taken->second;
    or_choices_->met.emplace_back(&pattern, alternatives.size());
    if (alternatives[index] == nullptr) {
      return fail("or_pattern", "a missing alternative", pattern.span);
    }
    return test_of(*alternatives[index], type, path, context, bindings);
  }
  case ast::node_kind::tuple_pattern: {
    const auto &tuple = ast::cast<const ast::tuple_pattern>(pattern);
    const auto &entry = checked_.types.entry(type);
    // `(a, b)` against an array of that length tests its elements, as an
    // array pattern does.
    if (entry.kind == type_kind::array_kind &&
        entry.array_size == tuple.elements.size()) {
      return array_test_of(tuple.elements, path, context, bindings,
                           std::move(test), pattern.span);
    }
    if (entry.kind != type_kind::tuple_kind ||
        entry.args.size() != tuple.elements.size()) {
      return fail("tuple_pattern",
                  std::format("a tuple pattern against `{}`",
                              checked_.types.display(type)),
                  pattern.span);
    }
    test.kind = pattern_test::form::tuple;
    for (std::size_t i = 0; i < tuple.elements.size(); ++i) {
      if (tuple.elements[i] == nullptr) {
        return fail("tuple_pattern", "a missing element", pattern.span);
      }
      const auto &element = *tuple.elements[i];
      CORE_TRY(element_type, part_type(element));
      auto step = step_of(projection::step::tuple_element, element_type);
      step.index = i;
      CORE_TRY(part, test_of(element, element_type,
                             extended(path, std::move(step)), context, bindings));
      test.parts.push_back(std::move(part));
    }
    return test;
  }
  case ast::node_kind::array_pattern: {
    const auto &array = ast::cast<const ast::array_pattern>(pattern);
    return array_test_of(array.elements, path, context, bindings,
                         std::move(test), pattern.span);
  }
  case ast::node_kind::struct_pattern: {
    const auto &record = ast::cast<const ast::struct_pattern>(pattern);
    const auto *shape = find_shape(checked_, type);
    if (shape == nullptr) {
      return fail("missing record: shapes",
                  std::format("no shape for `{}`", checked_.types.display(type)),
                  pattern.span);
    }
    test.kind = pattern_test::form::record;
    for (const auto &field : record.fields) {
      if (field.is_rest || field.name.empty()) {
        continue;
      }
      auto index = std::optional<std::size_t>{};
      for (std::size_t i = 0; i < shape->fields.size(); ++i) {
        if (shape->fields[i].name == field.name) {
          index = i;
        }
      }
      if (!index.has_value()) {
        return fail("missing record: shapes",
                    std::format("`{}` has no field `{}`",
                                checked_.types.display(type), field.name),
                    field.span);
      }
      auto field_type = type_id{k_unknown_type};
      if (field.pattern != nullptr) {
        CORE_TRY(found, part_type(*field.pattern));
        field_type = found;
      } else {
        const auto found =
            checked_.lowering.struct_pattern_field_types.find(&field);
        if (found == checked_.lowering.struct_pattern_field_types.end()) {
          return fail("missing record: struct_pattern_field_types",
                      std::format("no type for the field `{}`", field.name),
                      field.span);
        }
        CORE_TRY(settled, settle(found->second, field.span));
        field_type = settled;
      }
      auto step = step_of(projection::step::field, field_type);
      step.name = field.name;
      step.index = *index;
      auto field_path = extended(path, std::move(step));
      test.field_indices.push_back(*index);
      if (field.pattern != nullptr) {
        CORE_TRY(part, test_of(*field.pattern, field_type, field_path,
                               context, bindings));
        test.parts.push_back(std::move(part));
      } else {
        bindings.push_back(binding_of(field.name, &field, false, field_type,
                                      field_path, field.span, context));
        test.parts.push_back(any_test(field_type, field.span));
      }
    }
    return test;
  }
  case ast::node_kind::constructor_pattern:
  case ast::node_kind::option_pattern:
  case ast::node_kind::result_pattern: {
    auto name = std::string{};
    auto args = std::vector<const ast::node *>{};
    if (pattern.kind == ast::node_kind::constructor_pattern) {
      const auto &ctor = ast::cast<const ast::constructor_pattern>(pattern);
      name = ctor.name;
      for (const auto &arg : ctor.args) {
        if (arg == nullptr) {
          return fail("constructor_pattern", "a missing payload",
                      pattern.span);
        }
        args.push_back(arg.get());
      }
    } else if (pattern.kind == ast::node_kind::option_pattern) {
      const auto &option = ast::cast<const ast::option_pattern>(pattern);
      name = "some";
      if (option.inner != nullptr) {
        args.push_back(option.inner.get());
      }
    } else {
      const auto &wrapped = ast::cast<const ast::result_pattern>(pattern);
      name = wrapped.result_kind == ast::option_result_kind::err ? "err" : "ok";
      if (wrapped.inner != nullptr) {
        args.push_back(wrapped.inner.get());
      }
    }
    CORE_TRY(index, variant_index(type, name, pattern.span));
    test.kind = pattern_test::form::variant;
    test.variant_index = index;
    for (std::size_t i = 0; i < args.size(); ++i) {
      CORE_TRY(payload_type, part_type(*args[i]));
      auto step = step_of(projection::step::variant_payload, payload_type);
      step.name = name;
      step.index = i;
      CORE_TRY(part, test_of(*args[i], payload_type,
                             extended(path, std::move(step)), context, bindings));
      test.parts.push_back(std::move(part));
    }
    return test;
  }
  default:
    return fail(kind_name(pattern.kind), "a pattern with no core test",
                pattern.span);
  }
}

auto body_builder::alternative_of(const ast::node &pattern, type_id subject,
                                  binding_context &context)
    -> result<pattern_alternative> {
  auto made = pattern_alternative{};
  CORE_TRY(test, test_of(pattern, subject, {}, context, made.bindings));
  made.test = std::move(test);
  return made;
}

auto body_builder::bind_all(const std::vector<pattern_binding> &bindings)
    -> void {
  for (const auto &binding : bindings) {
    scopes_.back().insert_or_assign(
        binding.name,
        local_info{.symbol = binding.symbol,
                   .type = binding.type,
                   .writable = binding.writable,
                   .pinned = binding.how == pattern_binding::mode::alias});
  }
}

auto body_builder::subject_of(const ast::expr &subject,
                              binding_context &context) -> result<place> {
  CORE_TRY(p, place_of(subject));
  while (is_ref(p.type())) {
    add_deref(p);
    ++context.subject_derefs;
  }
  context.subject_access = access_of(p);
  context.subject_type = p.type();
  return p;
}

auto body_builder::arm_of(std::vector<pattern_alternative> alternatives,
                          const ast::expr *guard,
                          const std::function<result<ptr<block>>()> &body,
                          source_span span, const void *origin)
    -> result<match_arm> {
  auto arm = match_arm{};
  arm.span = span;
  arm.origin = origin;
  push_scope();
  if (!alternatives.empty()) {
    bind_all(alternatives.front().bindings);
  }
  const auto built = [&]() -> result<void> {
    if (guard != nullptr) {
      CORE_TRY(condition, value_as(*guard, checked_.types.bool_type()));
      arm.guard = std::move(condition);
    }
    CORE_TRY(made, body());
    arm.body = std::move(made);
    return {};
  }();
  pop_scope();
  if (!built.has_value()) {
    return std::unexpected(built.error());
  }
  arm.alternatives = std::move(alternatives);
  return arm;
}

auto body_builder::fallback_arm(type_id subject, ptr<block> body,
                                source_span span) -> match_arm {
  auto arm = match_arm{};
  arm.span = span;
  auto alternative = pattern_alternative{};
  alternative.test = any_test(subject, span);
  arm.alternatives.push_back(std::move(alternative));
  arm.body = std::move(body);
  return arm;
}

// ==========================================================================
//  The matching forms
// ==========================================================================

namespace {

/// The patterns of one arm: each alternative of a top-level `|`, or the
/// pattern itself.
[[nodiscard]] auto top_alternatives(const ast::node &pattern)
    -> std::vector<const ast::node *> {
  const auto *node = &pattern;
  while (node->kind == ast::node_kind::group_pattern) {
    const auto &group = ast::cast<const ast::group_pattern>(*node);
    if (group.alias.has_value() || group.inner == nullptr) {
      break;
    }
    node = group.inner.get();
  }
  if (node->kind != ast::node_kind::or_pattern) {
    return {&pattern};
  }
  auto out = std::vector<const ast::node *>{};
  for (const auto &alternative :
       ast::cast<const ast::or_pattern>(*node).alternatives) {
    out.push_back(alternative.get());
  }
  return out;
}

/// The most arm alternatives one arm's nested `|`s may expand to.
constexpr auto k_max_or_alternatives = std::size_t{64};

} // namespace

auto body_builder::alternatives_of(const ast::node &pattern, type_id subject,
                                   binding_context &context, source_span span)
    -> result<std::vector<pattern_alternative>> {
  auto out = std::vector<pattern_alternative>{};
  for (const auto *top : top_alternatives(pattern)) {
    if (top == nullptr) {
      return fail("or_pattern", "a missing alternative", span);
    }
    // An odometer over the nested `|`s, the leftmost slowest: each build
    // records the `|`s it met; the next advances the last one that has an
    // alternative left and forgets the choices after it, which belonged
    // to the alternative it moved away from.
    auto choices = or_choices{};
    while (true) {
      choices.met.clear();
      or_choices_ = &choices;
      auto made = alternative_of(*top, subject, context);
      or_choices_ = nullptr;
      if (!made.has_value()) {
        return std::unexpected(made.error());
      }
      out.push_back(std::move(*made));
      if (out.size() > k_max_or_alternatives) {
        return fail_feature(
            "nested or pattern",
            std::format(
                "this pattern's `|`s combine into more than {} alternatives, "
                "more than the compiler can expand; split it into several "
                "arms, or match the inner value in a nested `match`",
                k_max_or_alternatives),
            pattern.span);
      }
      auto advanced = false;
      for (auto i = choices.met.size(); i > 0 && !advanced; --i) {
        const auto [node, count] = choices.met[i - 1];
        auto &index = choices.taken[node];
        if (index + 1 < count) {
          ++index;
          for (auto j = i; j < choices.met.size(); ++j) {
            choices.taken.erase(choices.met[j].first);
          }
          advanced = true;
        }
      }
      if (!advanced) {
        break;
      }
    }
  }
  return out;
}

auto body_builder::match_of(const ast::expr &subject,
                            const std::vector<ast::match_arm> &arms,
                            source_span span, type_id type)
    -> result<ptr<expr>> {
  auto context = binding_context{};
  CORE_TRY(subject_place, subject_of(subject, context));
  const auto subject_type = subject_place.type();
  auto made = make_node<match_expr>(span, type);
  made->subject = std::move(subject_place);
  made->subject_origin = &subject;
  made->subject_derefs = context.subject_derefs;
  for (const auto &arm_ast : arms) {
    if (arm_ast.has_error || arm_ast.pattern == nullptr) {
      return fail("error node", "the arm carries a parse error", arm_ast.span);
    }
    auto arm_context = binding_context{
        .symbols = {},
        .subject_access = context.subject_access,
        .subject_type = context.subject_type};
    CORE_TRY(alternatives, alternatives_of(*arm_ast.pattern, subject_type,
                                           arm_context, arm_ast.span));
    CORE_TRY(arm, arm_of(
                      std::move(alternatives), arm_ast.guard.get(),
                      [&]() -> result<ptr<block>> {
                        if (arm_ast.body_expr != nullptr) {
                          // The arm's scope is keyed as if its value were
                          // its one statement.
                          auto body = make_node<block>(arm_ast.span, type);
                          body->origin = &arm_ast.body_stmts;
                          if (checked_.types.is_unit(type)) {
                            CORE_TRY(value, value_of(*arm_ast.body_expr));
                            body->statements.push_back(
                                expr_stmt_of(std::move(value), arm_ast.span));
                          } else {
                            CORE_TRY(value,
                                     value_as(*arm_ast.body_expr, type));
                            body->result = std::move(value);
                          }
                          return body;
                        }
                        return block_of(arm_ast.body_stmts, arm_ast.span,
                                        type);
                      },
                      arm_ast.span, arm_ast.pattern.get()));
    made->arms.push_back(std::move(arm));
  }
  return ptr<expr>(std::move(made));
}

auto body_builder::if_let_of(const std::vector<ast::if_branch> &branches,
                             const std::vector<ast::ptr<ast::node>> &else_body,
                             std::size_t index, source_span span, type_id type)
    -> result<ptr<expr>> {
  const auto &branch = branches[index];
  if (branch.let_expr == nullptr || branch.let_pattern == nullptr) {
    return fail("if let", "a branch with no pattern or subject", branch.span);
  }
  auto context = binding_context{};
  CORE_TRY(subject_place, subject_of(*branch.let_expr, context));
  const auto subject_type = subject_place.type();
  auto made = make_node<match_expr>(span, type);
  made->subject = std::move(subject_place);
  made->form = match_form::if_let;
  made->subject_origin = branch.let_expr.get();
  made->subject_derefs = context.subject_derefs;
  CORE_TRY(alternatives,
           alternatives_of(*branch.let_pattern, subject_type, context, branch.span));
  CORE_TRY(arm, arm_of(
                    std::move(alternatives), nullptr,
                    [&]() { return block_of(branch.body, branch.span, type); },
                    branch.span, branch.let_pattern.get()));
  made->arms.push_back(std::move(arm));
  auto otherwise = ptr<block>{};
  if (index + 1 < branches.size()) {
    CORE_TRY(rest, if_of(branches, else_body, index + 1, span, type));
    otherwise = make_node<block>(span, type);
    if (checked_.types.is_unit(type)) {
      otherwise->statements.push_back(expr_stmt_of(std::move(rest), span));
    } else {
      otherwise->result = std::move(rest);
    }
  } else {
    CORE_TRY(rest, block_of(else_body, span, type));
    otherwise = std::move(rest);
  }
  made->arms.push_back(fallback_arm(subject_type, std::move(otherwise), span));
  return ptr<expr>(std::move(made));
}

auto body_builder::while_let_of(const ast::while_stmt &loop)
    -> result<ptr<stmt>> {
  if (loop.let_expr == nullptr || loop.let_pattern == nullptr) {
    return fail("while let", "no pattern or subject", loop.span);
  }
  const auto unit = unit_type();
  auto context = binding_context{};
  CORE_TRY(subject_place, subject_of(*loop.let_expr, context));
  const auto subject_type = subject_place.type();
  auto matched = make_node<match_expr>(loop.span, unit);
  matched->subject = std::move(subject_place);
  matched->form = match_form::while_let;
  matched->subject_origin = loop.let_expr.get();
  matched->subject_derefs = context.subject_derefs;
  CORE_TRY(alternatives,
           alternatives_of(*loop.let_pattern, subject_type, context, loop.span));
  CORE_TRY(arm, arm_of(
                    std::move(alternatives), nullptr,
                    [&]() { return block_of(loop.body, loop.span, unit); },
                    loop.span, loop.let_pattern.get()));
  matched->arms.push_back(std::move(arm));
  auto stop = make_node<block>(loop.span, unit);
  stop->statements.push_back(make_node<jump>(node_kind::break_stmt, loop.span));
  matched->arms.push_back(fallback_arm(subject_type, std::move(stop), loop.span));
  auto made = make_node<while_loop>(loop.span);
  made->body = make_node<block>(loop.span, unit);
  made->body->statements.push_back(expr_stmt_of(std::move(matched), loop.span));
  return ptr<stmt>(std::move(made));
}

auto body_builder::let_pattern_of(const ast::let_stmt &let_ast,
                                  std::span<const ast::ptr<ast::node>> rest,
                                  source_span block_span, type_id block_type,
                                  const void *scope_key)
    -> result<ptr<expr>> {
  if (let_ast.pattern == nullptr || let_ast.initializer == nullptr) {
    return fail("let pattern", "no pattern or initializer", let_ast.span);
  }
  auto context = binding_context{};
  CORE_TRY(subject_place, subject_of(*let_ast.initializer, context));
  const auto subject_type = subject_place.type();
  auto made = make_node<match_expr>(let_ast.span, block_type);
  made->subject = std::move(subject_place);
  made->form = match_form::let_pattern;
  made->subject_origin = let_ast.initializer.get();
  made->subject_derefs = context.subject_derefs;
  CORE_TRY(alternatives,
           alternatives_of(*let_ast.pattern, subject_type, context, let_ast.span));
  CORE_TRY(arm, arm_of(
                    std::move(alternatives), nullptr,
                    [&]() {
                      // The rest of the block is still the block's scope.
                      return block_from(rest, block_span, block_type,
                                        scope_key);
                    },
                    let_ast.span, let_ast.pattern.get()));
  made->arms.push_back(std::move(arm));
  if (!let_ast.else_body.empty()) {
    CORE_TRY(otherwise, block_of(let_ast.else_body, let_ast.span, block_type));
    made->arms.push_back(
        fallback_arm(subject_type, std::move(otherwise), let_ast.span));
  }
  return ptr<expr>(std::move(made));
}

namespace {

/// `option` or `result`, and the names of its two variants.
struct wrapper_kind {
  bool is_result = false;
  std::string success;
  std::string failure;
};

[[nodiscard]] auto wrapper_of(const semantic::type_table &types, type_id type)
    -> std::optional<wrapper_kind> {
  const auto &entry = types.entry(type);
  if (entry.kind != type_kind::builtin_generic_kind) {
    return std::nullopt;
  }
  if (entry.name == "result") {
    return wrapper_kind{.is_result = true, .success = "ok", .failure = "err"};
  }
  if (entry.name == "option") {
    return wrapper_kind{.is_result = false, .success = "some",
                        .failure = "none"};
  }
  return std::nullopt;
}

} // namespace

auto body_builder::or_else_of(const ast::or_else_expr &or_else)
    -> result<ptr<expr>> {
  if (or_else.operand == nullptr || or_else.fallback == nullptr) {
    return fail("or_else_expr", "no operand or fallback", or_else.span);
  }
  CORE_TRY(type, type_of(or_else));
  auto context = binding_context{};
  CORE_TRY(subject_place, subject_of(*or_else.operand, context));
  const auto subject_type = subject_place.type();
  const auto wrapper = wrapper_of(checked_.types, subject_type);
  if (!wrapper.has_value()) {
    return fail("or_else_expr",
                std::format("`or else` on `{}`",
                            checked_.types.display(subject_type)),
                or_else.span);
  }
  CORE_TRY(index, variant_index(subject_type, wrapper->success, or_else.span));
  const auto &payload = find_shape(checked_, subject_type)->variants[index].payload;
  if (payload.size() != 1) {
    return fail("or_else_expr", "the success variant has no single payload",
                or_else.span);
  }
  auto made = make_node<match_expr>(or_else.span, type);
  made->subject = std::move(subject_place);
  made->form = match_form::or_else;
  made->subject_origin = or_else.operand.get();
  made->subject_derefs = context.subject_derefs;

  auto step = step_of(projection::step::variant_payload, payload.front());
  step.name = wrapper->success;
  auto alternative = pattern_alternative{};
  alternative.test.kind = pattern_test::form::variant;
  alternative.test.span = or_else.span;
  alternative.test.subject_type = subject_type;
  alternative.test.variant_index = index;
  alternative.test.parts.push_back(any_test(payload.front(), or_else.span));
  auto path = std::vector<projection>{};
  path.push_back(std::move(step));
  auto value = binding_of("<or else value>", nullptr, false,
                                payload.front(), path,
                                or_else.span,
                                context);
  auto success = make_node<block>(or_else.span, type);
  CORE_TRY(read, coerce(use_of(local_place(value), or_else.span), type));
  success->result = std::move(read);
  alternative.bindings.push_back(std::move(value));
  auto success_arm = match_arm{};
  success_arm.span = or_else.span;
  success_arm.alternatives.push_back(std::move(alternative));
  success_arm.body = std::move(success);
  made->arms.push_back(std::move(success_arm));

  CORE_TRY(fallback, value_as(*or_else.fallback, type));
  auto otherwise = make_node<block>(or_else.span, type);
  otherwise->result = std::move(fallback);
  made->arms.push_back(
      fallback_arm(subject_type, std::move(otherwise), or_else.span));
  return ptr<expr>(std::move(made));
}

auto body_builder::try_of(const ast::try_expr &try_expr) -> result<ptr<expr>> {
  if (try_expr.operand == nullptr) {
    return fail("try_expr", "no operand", try_expr.span);
  }
  CORE_TRY(type, type_of(try_expr));
  auto context = binding_context{};
  CORE_TRY(subject_place, subject_of(*try_expr.operand, context));
  const auto subject_type = subject_place.type();
  const auto wrapper = wrapper_of(checked_.types, subject_type);
  const auto returned = wrapper_of(checked_.types, body_type_);
  if (!wrapper.has_value() || !returned.has_value() ||
      wrapper->is_result != returned->is_result) {
    return fail_feature(
        "try_expr",
        std::format("`?` operand did not resolve to a concrete result/option "
                    "type: `?` on `{}` in a function returning `{}`",
                            checked_.types.display(subject_type),
                            checked_.types.display(body_type_)),
                try_expr.span);
  }
  const auto *variants = find_shape(checked_, subject_type);
  if (variants == nullptr) {
    return fail("missing record: shapes", "no shape for the `?` operand",
                try_expr.span);
  }
  CORE_TRY(success_index,
           variant_index(subject_type, wrapper->success, try_expr.span));
  CORE_TRY(failure_index,
           variant_index(subject_type, wrapper->failure, try_expr.span));
  CORE_TRY(returned_failure,
           variant_index(body_type_, wrapper->failure, try_expr.span));

  auto made = make_node<match_expr>(try_expr.span, type);
  made->subject = std::move(subject_place);
  made->form = match_form::try_operator;
  made->subject_origin = try_expr.operand.get();
  made->subject_derefs = context.subject_derefs;

  // `@ok(v)` / `@some(v)` => v
  const auto &success_payload =
      variants->variants[success_index].payload;
  if (success_payload.size() != 1) {
    return fail("try_expr", "the success variant has no single payload",
                try_expr.span);
  }
  {
    auto step = step_of(projection::step::variant_payload,
                        success_payload.front());
    step.name = wrapper->success;
    auto path = std::vector<projection>{};
    path.push_back(std::move(step));
    auto value = binding_of("<try value>", nullptr, false,
                                  success_payload.front(), path,
                                  try_expr.span,
                                  context);
    auto alternative = pattern_alternative{};
    alternative.test.kind = pattern_test::form::variant;
    alternative.test.span = try_expr.span;
    alternative.test.subject_type = subject_type;
    alternative.test.variant_index = success_index;
    alternative.test.parts.push_back(
        any_test(success_payload.front(), try_expr.span));
    auto body = make_node<block>(try_expr.span, type);
    CORE_TRY(read, coerce(use_of(local_place(value), try_expr.span), type));
    body->result = std::move(read);
    alternative.bindings.push_back(std::move(value));
    auto arm = match_arm{};
    arm.span = try_expr.span;
    arm.alternatives.push_back(std::move(alternative));
    arm.body = std::move(body);
    made->arms.push_back(std::move(arm));
  }

  // `@err(x)` => return @err(convert(x)); `@none` => return @none
  {
    auto alternative = pattern_alternative{};
    alternative.test.kind = pattern_test::form::variant;
    alternative.test.span = try_expr.span;
    alternative.test.subject_type = subject_type;
    alternative.test.variant_index = failure_index;
    auto returned_value = make_node<variant_init>(try_expr.span, body_type_);
    returned_value->variant = wrapper->failure;
    returned_value->variant_index = returned_failure;
    if (wrapper->is_result) {
      const auto &payload = variants->variants[failure_index].payload;
      const auto &target_payload =
          find_shape(checked_, body_type_)->variants[returned_failure].payload;
      if (payload.size() != 1 || target_payload.size() != 1) {
        return fail("try_expr", "the `err` variant has no single payload",
                    try_expr.span);
      }
      auto step = step_of(projection::step::variant_payload, payload.front());
      step.name = wrapper->failure;
      auto path = std::vector<projection>{};
      path.push_back(std::move(step));
      auto error = binding_of("<try error>", nullptr, false,
                                    payload.front(), path,
                                    try_expr.span, context);
      alternative.test.parts.push_back(
          any_test(payload.front(), try_expr.span));
      auto read = use_of(local_place(error), try_expr.span);
      alternative.bindings.push_back(std::move(error));
      if (const auto conversion =
              checked_.lowering.try_conversions.find(&try_expr);
          conversion != checked_.lowering.try_conversions.end()) {
        CORE_TRY(target, callee_of(conversion->second, try_expr.span));
        const auto params = checked_.types.entry(target.signature).args;
        if (params.size() != 1) {
          return fail("missing record: try_conversions",
                      "the conversion does not take one parameter",
                      try_expr.span);
        }
        CORE_TRY(argument, coerce(std::move(read), params.front()));
        auto args = ptr_vec<expr>{};
        args.push_back(std::move(argument));
        read = direct_call(std::move(target), std::move(args), try_expr.span);
      }
      CORE_TRY(converted, coerce(std::move(read), target_payload.front()));
      returned_value->payload.push_back(std::move(converted));
    }
    auto body = make_node<block>(try_expr.span, type);
    CORE_TRY(leave,
             return_of(std::move(returned_value), try_expr.span, &try_expr));
    body->statements.push_back(std::move(leave));
    auto arm = match_arm{};
    arm.span = try_expr.span;
    arm.alternatives.push_back(std::move(alternative));
    arm.body = std::move(body);
    made->arms.push_back(std::move(arm));
  }
  return ptr<expr>(std::move(made));
}

} // namespace cinder::core
