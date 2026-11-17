#include <format>
#include <string>
#include <utility>
#include <vector>

#include "src/core/adapter_builder.h"
#include "src/parser/ast_traversal.h"
#include "src/parser/operators.h"

/// `for` loops, comprehensions and run-time fills. Each is a `let` of its
/// loop state followed by a `while_loop`, as `hir::lower_for_stmt` and its
/// siblings build them; the loop variables are a one-arm `match` on the
/// element, so their binding modes follow the same rule as any pattern.
namespace cinder::core {

using semantic::type_kind;

namespace {

[[nodiscard]] auto local_root(const let &bound) -> place {
  auto p = place{};
  p.root.kind = place_root::origin::local;
  p.root.symbol = bound.symbol;
  p.root.name = bound.name;
  p.root.type = bound.type;
  return p;
}

} // namespace

auto body_builder::let_of(std::string name, type_id type, bool is_mut,
                          ptr<expr> value, source_span span) -> ptr<let> {
  auto made = make_node<let>(span);
  made->name = std::move(name);
  made->type = type;
  made->is_mut = is_mut;
  made->value = std::move(value);
  made->symbol = declare(made->name, type, is_mut);
  return made;
}

auto body_builder::read_local(const let &bound, source_span span)
    -> ptr<expr> {
  return use_of(local_root(bound), span);
}

auto body_builder::int_literal(std::uint64_t value, type_id type,
                               source_span span) -> ptr<expr> {
  auto lit = make_node<literal>(span, type);
  lit->token = token_kind::int_lit;
  lit->spelling = std::to_string(value);
  return lit;
}

auto body_builder::head_test(std::span<const ast::node *const> patterns,
                             type_id type, const std::vector<projection> &path,
                             binding_context &context,
                             std::vector<pattern_binding> &bindings)
    -> result<pattern_test> {
  if (patterns.size() == 1) {
    return test_of(*patterns.front(), type, path, context, bindings);
  }
  // `for k, v in ...`: the patterns test the elements of a tuple.
  const auto &entry = checked_.types.entry(type);
  if (entry.kind != type_kind::tuple_kind ||
      entry.args.size() != patterns.size()) {
    return fail("for_stmt",
                std::format("{} loop patterns against `{}`", patterns.size(),
                            checked_.types.display(type)),
                patterns.front()->span);
  }
  auto test = pattern_test{};
  test.kind = pattern_test::form::tuple;
  test.span = patterns.front()->span;
  test.subject_type = type;
  for (std::size_t i = 0; i < patterns.size(); ++i) {
    auto step = projection{};
    step.kind = projection::step::tuple_element;
    step.type = entry.args[i];
    step.index = i;
    auto element_path = std::vector<projection>{};
    for (const auto &existing : path) {
      auto copy = projection{};
      copy.kind = existing.kind;
      copy.type = existing.type;
      copy.name = existing.name;
      copy.index = existing.index;
      element_path.push_back(std::move(copy));
    }
    element_path.push_back(std::move(step));
    CORE_TRY(part, test_of(*patterns[i], entry.args[i], element_path, context,
                           bindings));
    test.parts.push_back(std::move(part));
  }
  return test;
}

auto body_builder::bind_element(place element,
                                std::span<const ast::node *const> patterns,
                                const loop_body &body, source_span span)
    -> result<ptr<expr>> {
  auto context = binding_context{};
  context.subject_access = access_of(element);
  context.subject_type = element.type();
  auto alternative = pattern_alternative{};
  CORE_TRY(test, head_test(patterns, element.type(), {}, context,
                           alternative.bindings));
  alternative.test = std::move(test);
  auto alternatives = std::vector<pattern_alternative>{};
  alternatives.push_back(std::move(alternative));
  CORE_TRY(arm, arm_of(std::move(alternatives), nullptr, body, span, nullptr));
  auto made = make_node<match_expr>(span, unit_type());
  made->subject = std::move(element);
  made->form = match_form::for_element;
  made->arms.push_back(std::move(arm));
  return ptr<expr>(std::move(made));
}

auto body_builder::next_loop(ptr<expr> next, type_id element_type,
                             std::span<const ast::node *const> patterns,
                             const loop_body &body, source_span span)
    -> result<ptr<stmt>> {
  // `while_loop{null, match next: @some(x) => body, _ => break}`
  const auto option_type = next->type;
  CORE_TRY(some, variant_index(option_type, "some", span));
  auto context = binding_context{};
  auto subject = temporary(std::move(next));
  context.subject_access = access_of(subject);
  context.subject_type = option_type;
  auto alternative = pattern_alternative{};
  alternative.test.kind = pattern_test::form::variant;
  alternative.test.span = span;
  alternative.test.subject_type = option_type;
  alternative.test.variant_index = some;
  auto step = projection{};
  step.kind = projection::step::variant_payload;
  step.type = element_type;
  step.name = "some";
  auto path = std::vector<projection>{};
  path.push_back(std::move(step));
  CORE_TRY(part, head_test(patterns, element_type, path, context,
                           alternative.bindings));
  alternative.test.parts.push_back(std::move(part));
  auto alternatives = std::vector<pattern_alternative>{};
  alternatives.push_back(std::move(alternative));
  CORE_TRY(arm, arm_of(std::move(alternatives), nullptr, body, span, nullptr));
  auto matched = make_node<match_expr>(span, unit_type());
  matched->subject = std::move(subject);
  matched->form = match_form::for_next;
  matched->arms.push_back(std::move(arm));
  auto stop = make_node<block>(span, unit_type());
  stop->statements.push_back(make_node<jump>(node_kind::break_stmt, span));
  matched->arms.push_back(fallback_arm(option_type, std::move(stop), span));
  auto loop = make_node<while_loop>(span);
  loop->body = make_node<block>(span, unit_type());
  loop->body->statements.push_back(expr_stmt_of(std::move(matched), span));
  return ptr<stmt>(std::move(loop));
}

auto body_builder::counting_loop(const let &counter, ptr<expr> bound,
                                 bool inclusive, ptr<block> body,
                                 source_span span) -> ptr<stmt> {
  const auto type = counter.type;
  auto condition = make_node<primitive>(span, checked_.types.bool_type());
  condition->binary =
      inclusive ? ast::binary_op::lt_eq : ast::binary_op::lt;
  condition->operands.push_back(read_local(counter, span));
  condition->operands.push_back(std::move(bound));
  // The increment is the loop's step, so `continue` still advances.
  auto next = make_node<primitive>(span, type);
  next->binary = ast::binary_op::add;
  next->operands.push_back(read_local(counter, span));
  next->operands.push_back(int_literal(1, type, span));
  auto advance = make_node<assign>(span);
  advance->target = local_root(counter);
  advance->value = std::move(next);
  auto loop = make_node<while_loop>(span);
  loop->condition = std::move(condition);
  loop->body = std::move(body);
  loop->step = make_node<block>(span, unit_type());
  loop->step->statements.push_back(std::move(advance));
  return loop;
}

auto body_builder::container_len(place container, source_span span)
    -> ptr<expr> {
  const auto type = container.type();
  auto args = ptr_vec<expr>{};
  if (checked_.types.is_view(type) && checked_.is_copy(type)) {
    args.push_back(use_of(std::move(container), span));
  } else {
    args.push_back(receiver_argument(std::move(container),
                                     checked_.types.ref_to(type, false), span));
  }
  return intrinsic_call("len", checked_.types.usize_type(), std::move(args),
                        {}, span);
}

auto body_builder::loop_of(const ast::expr &iterable,
                           std::span<const ast::node *const> patterns,
                           const semantic::iterator_loop_dispatch *dispatch,
                           const loop_body &body, source_span span,
                           const void *handle_key, const void *loop_key)
    -> result<ptr<expr>> {
  push_scope();
  auto made = make_node<block>(span, unit_type());
  made->origin = handle_key;
  auto built = [&]() -> result<void> {
    // A range: `let start; let end; let mut i = start; while i < end`.
    if (iterable.kind == ast::node_kind::binary_expr) {
      const auto &range = ast::cast<const ast::binary_expr>(iterable);
      if (ast::is_range(range.op)) {
        if (range.lhs == nullptr || range.rhs == nullptr) {
          return fail("for_stmt", "a range with a missing bound", span);
        }
        CORE_TRY(bound_type, type_of(*range.lhs));
        CORE_TRY(start, value_as(*range.lhs, bound_type));
        CORE_TRY(end, value_as(*range.rhs, bound_type));
        auto start_let =
            let_of("<for start>", bound_type, false, std::move(start), span);
        auto end_let =
            let_of("<for end>", bound_type, false, std::move(end), span);
        auto index_let = let_of("<for index>", bound_type, true,
                                read_local(*start_let, span), span);
        CORE_TRY(matched,
                 bind_element(local_root(*index_let), patterns, body, span));
        auto loop_body_block = make_node<block>(span, unit_type());
        loop_body_block->statements.push_back(
            expr_stmt_of(std::move(matched), span));
        auto loop = counting_loop(*index_let, read_local(*end_let, span),
                                  range.op == ast::binary_op::range_inclusive,
                                  std::move(loop_body_block), span);
        made->statements.push_back(std::move(start_let));
        made->statements.push_back(std::move(end_let));
        made->statements.push_back(std::move(index_let));
        loop->origin = loop_key;
        made->statements.push_back(std::move(loop));
        return {};
      }
    }
    CORE_TRY(iterable_type, type_of(iterable));
    const auto &entry = checked_.types.entry(iterable_type);
    const auto generic_named = [&](std::string_view name) {
      return entry.kind == type_kind::builtin_generic_kind && entry.name == name;
    };
    // An `option`: the body runs once on `@some(x)`.
    if (generic_named("option")) {
      if (entry.args.size() != 1) {
        return fail("for_stmt", "an `option` with no element type", span);
      }
      CORE_TRY(subject, place_of(iterable));
      CORE_TRY(some, variant_index(iterable_type, "some", span));
      auto context = binding_context{};
      context.subject_access = access_of(subject);
      context.subject_type = iterable_type;
      auto alternative = pattern_alternative{};
      alternative.test.kind = pattern_test::form::variant;
      alternative.test.span = span;
      alternative.test.subject_type = iterable_type;
      alternative.test.variant_index = some;
      auto step = projection{};
      step.kind = projection::step::variant_payload;
      step.type = entry.args[0];
      step.name = "some";
      auto path = std::vector<projection>{};
      path.push_back(std::move(step));
      CORE_TRY(part, head_test(patterns, entry.args[0], path, context,
                               alternative.bindings));
      alternative.test.parts.push_back(std::move(part));
      auto alternatives = std::vector<pattern_alternative>{};
      alternatives.push_back(std::move(alternative));
      CORE_TRY(arm,
               arm_of(std::move(alternatives), nullptr, body, span, nullptr));
      auto matched = make_node<match_expr>(span, unit_type());
      matched->subject = std::move(subject);
      matched->form = match_form::for_option;
      matched->subject_origin = &iterable;
      matched->arms.push_back(std::move(arm));
      matched->arms.push_back(
          fallback_arm(iterable_type, make_node<block>(span, unit_type()), span));
      made->statements.push_back(expr_stmt_of(std::move(matched), span));
      return {};
    }
    // A generator: `let mut g = e; while: match generator_next(&mut g)`.
    if (generic_named("generator")) {
      if (entry.args.empty()) {
        return fail("for_stmt", "a generator with no item type", span);
      }
      CORE_TRY(handle, value_of(iterable));
      auto handle_let =
          let_of("<for iterator>", iterable_type, true, std::move(handle), span);
      handle_let->binding_origin = handle_key;
      const auto option_type =
          checked_.types.builtin_generic("option", {entry.args[0]});
      auto args = ptr_vec<expr>{};
      args.push_back(receiver_argument(local_root(*handle_let),
                                       checked_.types.ref_to(iterable_type, true),
                                       span));
      auto next = intrinsic_call("generator_next", option_type, std::move(args),
                                 {}, span);
      CORE_TRY(loop, next_loop(std::move(next), entry.args[0], patterns, body,
                               span));
      made->statements.push_back(std::move(handle_let));
      loop->origin = loop_key;
      made->statements.push_back(std::move(loop));
      return {};
    }
    // An iterator, or a collection through `into_iter`/`iter`/`iter_mut`.
    if (dispatch != nullptr) {
      CORE_TRY(element_type, settle(dispatch->element_type, span));
      auto handle = ptr<expr>{};
      if (dispatch->adapter_decl != nullptr) {
        CORE_TRY(adapter,
                 callee_of(semantic::resolved_callee{
                               .decl = dispatch->adapter_decl,
                               .owner_module = dispatch->adapter_owner_module,
                               .impl_target_type =
                                   dispatch->adapter_impl_target_type,
                               .receiver = nullptr,
                               .trait_name = {}},
                           span));
        const auto params = checked_.types.entry(adapter.signature).args;
        if (params.size() != 1) {
          return fail("missing record: for_iterator_dispatches",
                      "the adapter does not take one parameter", span);
        }
        auto args = ptr_vec<expr>{};
        if (!is_ref(params[0]) && is_ref(iterable_type) &&
            !checked_.is_copy(params[0])) {
          // Todo 39: the checker lets a borrowed collection of `copy`
          // elements be copied into a consuming `into_iter` (the old
          // lowerer's `receiver_borrowed`), and the iterator is never
          // dropped. The copy is explicit here: `into_iter(bitwise_copy(
          // xs))`. Removed with `bitwise_copy` in phase 7 of
          // spec/unified-checker-plan.md.
          if (dispatch->adapter_borrows ||
              checked_.types.entry(iterable_type).result != params[0]) {
            return fail("for over borrowed collection",
                        "the collection is not the adapter's receiver", span);
          }
          if (!checked_.is_copy(element_type)) {
            return fail("for over borrowed collection",
                        "the elements are not `copy`", span);
          }
          CORE_TRY(borrowed, value_of(iterable));
          auto copy_args = ptr_vec<expr>{};
          copy_args.push_back(std::move(borrowed));
          args.push_back(intrinsic_call("bitwise_copy", params[0],
                                        std::move(copy_args), {params[0]},
                                        span));
        } else {
          CORE_TRY(argument, argument(iterable, params[0], std::nullopt));
          args.push_back(std::move(argument));
        }
        handle = direct_call(std::move(adapter), std::move(args), span);
      } else {
        CORE_TRY(value, value_of(iterable));
        handle = std::move(value);
      }
      const auto handle_type = handle->type;
      auto handle_let =
          let_of("<for iterator>", handle_type, true, std::move(handle), span);
      handle_let->binding_origin = handle_key;
      CORE_TRY(next_target,
               callee_of(semantic::resolved_callee{
                             .decl = dispatch->decl,
                             .owner_module = dispatch->owner_module,
                             .impl_target_type = dispatch->impl_target_type,
                             .receiver = nullptr,
                             .trait_name = {}},
                         span));
      const auto params = checked_.types.entry(next_target.signature).args;
      if (params.size() != 1) {
        return fail("missing record: for_iterator_dispatches",
                    "`next` does not take one parameter", span);
      }
      auto args = ptr_vec<expr>{};
      args.push_back(receiver_argument(local_root(*handle_let), params[0], span));
      auto next = direct_call(std::move(next_target), std::move(args), span);
      CORE_TRY(loop,
               next_loop(std::move(next), element_type, patterns, body, span));
      made->statements.push_back(std::move(handle_let));
      loop->origin = loop_key;
      made->statements.push_back(std::move(loop));
      return {};
    }
    // A container indexed by a counter: `let c = e; let mut i = 0;
    // while i < len(c): match c[i]`. `str` decodes scalars instead.
    CORE_TRY(container, value_of(iterable));
    auto container_let = let_of("<for container>", iterable_type, false,
                                std::move(container), span);
    const auto stripped = strip_refs(iterable_type);
    const auto &stripped_entry = checked_.types.entry(stripped);
    const auto usize = checked_.types.usize_type();
    auto container_place = [&]() {
      auto p = local_root(*container_let);
      while (is_ref(p.type())) {
        add_deref(p);
      }
      return p;
    };
    if (stripped_entry.kind == type_kind::builtin_kind &&
        stripped_entry.name == "str") {
      auto cursor_let =
          let_of("<for cursor>", usize, true, int_literal(0, usize, span), span);
      const auto char_type = checked_.types.char_type();
      const auto str_arg = [&]() { return use_of(container_place(), span); };
      auto decode_args = ptr_vec<expr>{};
      decode_args.push_back(str_arg());
      decode_args.push_back(read_local(*cursor_let, span));
      auto decoded = intrinsic_call("str_decode_scalar", char_type,
                                    std::move(decode_args), {}, span);
      CORE_TRY(matched, bind_element(temporary(std::move(decoded)), patterns,
                                     body, span));
      auto loop_body_block = make_node<block>(span, unit_type());
      loop_body_block->statements.push_back(
          expr_stmt_of(std::move(matched), span));
      auto condition = make_node<primitive>(span, checked_.types.bool_type());
      condition->binary = ast::binary_op::lt;
      condition->operands.push_back(read_local(*cursor_let, span));
      condition->operands.push_back(container_len(container_place(), span));
      auto width_args = ptr_vec<expr>{};
      width_args.push_back(str_arg());
      width_args.push_back(read_local(*cursor_let, span));
      auto advance_value = make_node<primitive>(span, usize);
      advance_value->binary = ast::binary_op::add;
      advance_value->operands.push_back(read_local(*cursor_let, span));
      advance_value->operands.push_back(intrinsic_call(
          "str_scalar_width", usize, std::move(width_args), {}, span));
      auto advance = make_node<assign>(span);
      advance->target = local_root(*cursor_let);
      advance->value = std::move(advance_value);
      auto loop = make_node<while_loop>(span);
      loop->condition = std::move(condition);
      loop->body = std::move(loop_body_block);
      loop->step = make_node<block>(span, unit_type());
      loop->step->statements.push_back(std::move(advance));
      made->statements.push_back(std::move(container_let));
      made->statements.push_back(std::move(cursor_let));
      loop->origin = loop_key;
      made->statements.push_back(std::move(loop));
      return {};
    }
    auto element_type = k_unknown_type;
    auto bound = ptr<expr>{};
    if (stripped_entry.kind == type_kind::array_kind &&
        stripped_entry.array_size.has_value()) {
      element_type = stripped_entry.result;
      bound = int_literal(*stripped_entry.array_size, usize, span);
    } else if (stripped_entry.kind == type_kind::builtin_generic_kind &&
               (stripped_entry.name == "slice" ||
                stripped_entry.name == "slice_mut") &&
               stripped_entry.args.size() == 1) {
      element_type = stripped_entry.args[0];
      bound = container_len(container_place(), span);
    } else {
      return fail("for_stmt",
                  std::format("iterating a `{}` with no iterator record",
                              checked_.types.display(iterable_type)),
                  span);
    }
    auto index_let =
        let_of("<for index>", usize, true, int_literal(0, usize, span), span);
    auto element = container_place();
    auto step = projection{};
    step.kind = projection::step::element;
    step.type = element_type;
    step.subscript = read_local(*index_let, span);
    element.path.push_back(std::move(step));
    CORE_TRY(matched, bind_element(std::move(element), patterns, body, span));
    auto loop_body_block = make_node<block>(span, unit_type());
    loop_body_block->statements.push_back(expr_stmt_of(std::move(matched), span));
    auto loop = counting_loop(*index_let, std::move(bound), false,
                              std::move(loop_body_block), span);
    made->statements.push_back(std::move(container_let));
    made->statements.push_back(std::move(index_let));
    loop->origin = loop_key;
    made->statements.push_back(std::move(loop));
    return {};
  }();
  pop_scope();
  if (!built.has_value()) {
    return std::unexpected(built.error());
  }
  return ptr<expr>(std::move(made));
}

auto body_builder::guarded_body(const ast::expr *guard,
                                const std::vector<ast::ptr<ast::node>> &stmts,
                                source_span span) -> result<ptr<block>> {
  if (guard == nullptr) {
    return block_of(stmts, span, unit_type());
  }
  CORE_TRY(condition, value_as(*guard, checked_.types.bool_type()));
  CORE_TRY(inner, block_of(stmts, span, unit_type()));
  auto check = make_node<if_expr>(span, unit_type());
  check->condition = std::move(condition);
  check->then_branch = std::move(inner);
  auto made = make_node<block>(span, unit_type());
  made->statements.push_back(expr_stmt_of(std::move(check), span));
  return made;
}

auto body_builder::for_of(const ast::for_stmt &loop) -> result<ptr<stmt>> {
  if (loop.iterable == nullptr || loop.patterns.empty()) {
    return fail("for_stmt", "no iterable or loop variable", loop.span);
  }
  auto patterns = std::vector<const ast::node *>{};
  for (const auto &pattern : loop.patterns) {
    if (pattern == nullptr || pattern->has_error) {
      return fail("error node", "a loop pattern carries a parse error",
                  loop.span);
    }
    patterns.push_back(pattern.get());
  }
  const auto found = checked_.for_iterator_dispatches.find(&loop);
  const auto *dispatch =
      found != checked_.for_iterator_dispatches.end() ? &found->second : nullptr;
  CORE_TRY(made, loop_of(*loop.iterable, patterns, dispatch,
                         [&]() -> result<ptr<block>> {
                           return guarded_body(loop.guard.get(), loop.body,
                                               loop.span);
                         },
                         loop.span, &loop.iterable, &loop));
  return expr_stmt_of(std::move(made), loop.span);
}

auto body_builder::list_builder_call(const semantic::resolved_callee &resolved,
                                     ptr_vec<expr> args, source_span span)
    -> result<ptr<expr>> {
  CORE_TRY(target, callee_of(resolved, span));
  const auto params = checked_.types.entry(target.signature).args;
  if (params.size() != args.size()) {
    return fail("missing record: comprehension_dispatches",
                std::format("`{}` takes {} parameters, given {}", target.name,
                            params.size(), args.size()),
                span);
  }
  for (std::size_t i = 0; i < args.size(); ++i) {
    CORE_TRY(converted, coerce(std::move(args[i]), params[i]));
    args[i] = std::move(converted);
  }
  return direct_call(std::move(target), std::move(args), span);
}

auto body_builder::push_onto(const let &acc,
                             const semantic::comprehension_dispatch &dispatch,
                             ptr<expr> value, source_span span)
    -> result<ptr<stmt>> {
  CORE_TRY(target, callee_of(dispatch.push_callee, span));
  const auto params = checked_.types.entry(target.signature).args;
  if (params.size() != 2) {
    return fail("missing record: comprehension_dispatches",
                "`push` does not take two parameters", span);
  }
  auto args = ptr_vec<expr>{};
  args.push_back(receiver_argument(local_root(acc), params[0], span));
  CORE_TRY(converted, coerce(std::move(value), params[1]));
  args.push_back(std::move(converted));
  return expr_stmt_of(direct_call(std::move(target), std::move(args), span),
                      span);
}

auto body_builder::comprehension_of(const ast::for_expr &comprehension)
    -> result<ptr<expr>> {
  const auto found =
      checked_.lowering.comprehension_dispatches.find(&comprehension);
  if (found == checked_.lowering.comprehension_dispatches.end()) {
    return fail("missing record: comprehension_dispatches",
                "a comprehension with no `new`/`push`", comprehension.span);
  }
  if (comprehension.yield_expr == nullptr || comprehension.clauses.empty()) {
    return fail("for_expr", "no yield or no clauses", comprehension.span);
  }
  const auto &dispatch = found->second;
  const auto span = comprehension.span;
  CORE_TRY(type, type_of(comprehension));
  push_scope();
  auto made = make_node<block>(span, type);
  auto built = [&]() -> result<void> {
    CORE_TRY(fresh, list_builder_call(dispatch.new_callee, {}, span));
    auto acc = let_of("<comprehension result>", fresh->type, true,
                      std::move(fresh), span);
    // The innermost body pushes the yield; each clause wraps the next.
    std::function<result<ptr<block>>(std::size_t)> clause_body =
        [&](std::size_t index) -> result<ptr<block>> {
      if (index == comprehension.clauses.size()) {
        auto inner = make_node<block>(span, unit_type());
        CORE_TRY(value, value_of(*comprehension.yield_expr));
        CORE_TRY(push, push_onto(*acc, dispatch, std::move(value), span));
        if (comprehension.guard == nullptr) {
          inner->statements.push_back(std::move(push));
          return inner;
        }
        CORE_TRY(condition,
                 value_as(*comprehension.guard, checked_.types.bool_type()));
        auto then_branch = make_node<block>(span, unit_type());
        then_branch->statements.push_back(std::move(push));
        auto check = make_node<if_expr>(span, unit_type());
        check->condition = std::move(condition);
        check->then_branch = std::move(then_branch);
        inner->statements.push_back(expr_stmt_of(std::move(check), span));
        return inner;
      }
      const auto &clause = comprehension.clauses[index];
      if (clause.iterable == nullptr || clause.patterns.empty()) {
        return fail("for_expr", "a clause with no iterable or variable", span);
      }
      auto patterns = std::vector<const ast::node *>{};
      for (const auto &pattern : clause.patterns) {
        if (pattern == nullptr || pattern->has_error) {
          return fail("error node", "a loop pattern carries a parse error",
                      span);
        }
        patterns.push_back(pattern.get());
      }
      const auto record = checked_.comprehension_iterator_dispatches.find(
          static_cast<const ast::node *>(clause.iterable.get()));
      const auto *loop_dispatch =
          record != checked_.comprehension_iterator_dispatches.end()
              ? &record->second
              : nullptr;
      CORE_TRY(loop, loop_of(*clause.iterable, patterns, loop_dispatch,
                             [&]() { return clause_body(index + 1); },
                             clause.iterable->span, &clause.iterable,
                             &clause));
      auto wrapper = make_node<block>(span, unit_type());
      wrapper->statements.push_back(expr_stmt_of(std::move(loop), span));
      return wrapper;
    };
    CORE_TRY(loops, clause_body(0));
    const auto acc_type = acc->type;
    auto result_place = local_root(*acc);
    made->statements.push_back(std::move(acc));
    made->statements.push_back(expr_stmt_of(std::move(loops), span));
    CORE_TRY(result_value, coerce(use_of(std::move(result_place), span), type));
    (void)acc_type;
    made->result = std::move(result_value);
    return {};
  }();
  pop_scope();
  if (!built.has_value()) {
    return std::unexpected(built.error());
  }
  return ptr<expr>(std::move(made));
}

auto body_builder::runtime_fill_of(const ast::array_expr &array,
                                   const semantic::comprehension_dispatch
                                       &dispatch) -> result<ptr<expr>> {
  if (array.fill_value == nullptr || array.fill_count == nullptr) {
    return fail("array_expr", "a fill literal needs a value and a count",
                array.span);
  }
  const auto span = array.span;
  CORE_TRY(type, type_of(array));
  const auto usize = checked_.types.usize_type();
  push_scope();
  auto made = make_node<block>(span, type);
  auto built = [&]() -> result<void> {
    CORE_TRY(fresh, list_builder_call(dispatch.new_callee, {}, span));
    auto acc =
        let_of("<fill result>", fresh->type, true, std::move(fresh), span);
    CORE_TRY(count, value_as(*array.fill_count, usize));
    auto count_let = let_of("<fill count>", usize, false, std::move(count), span);
    auto index_let =
        let_of("<fill index>", usize, true, int_literal(0, usize, span), span);
    CORE_TRY(value, value_of(*array.fill_value));
    CORE_TRY(push, push_onto(*acc, dispatch, std::move(value), span));
    auto body = make_node<block>(span, unit_type());
    body->statements.push_back(std::move(push));
    auto loop = counting_loop(*index_let, read_local(*count_let, span), false,
                              std::move(body), span);
    auto result_place = local_root(*acc);
    made->statements.push_back(std::move(acc));
    made->statements.push_back(std::move(count_let));
    made->statements.push_back(std::move(index_let));
    made->statements.push_back(std::move(loop));
    CORE_TRY(result_value, coerce(use_of(std::move(result_place), span), type));
    made->result = std::move(result_value);
    return {};
  }();
  pop_scope();
  if (!built.has_value()) {
    return std::unexpected(built.error());
  }
  return ptr<expr>(std::move(made));
}

} // namespace cinder::core
