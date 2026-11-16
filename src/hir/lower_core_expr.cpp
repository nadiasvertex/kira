#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>

#include "src/core/intrinsics.h"
#include "src/hir/lower_core_lowerer.h"
#include "src/parser/ast_traversal.h"
#include "src/runtime/value_layout.h"

/// Expressions, places and calls.
namespace cinder::hir {

using semantic::type_kind;

namespace {

/// Whether the branching expression `origin` (its AST node) can suspend a
/// task from inside a branch, by the test the old lowerer applies
/// (`lowerer::branch_value_can_suspend`). The old lowerer then moves the
/// branches into statements ahead of the enclosing one, which this
/// lowering does not do yet.
[[nodiscard]] auto branch_can_suspend(const void *origin) -> bool {
  if (origin == nullptr) {
    return false;
  }
  auto found = false;
  ast::walk(*static_cast<const ast::node *>(origin),
            [&found](const ast::node &n) -> bool {
              switch (n.kind) {
              case ast::node_kind::await_expr:
              case ast::node_kind::try_expr:
              case ast::node_kind::return_stmt:
              case ast::node_kind::break_stmt:
              case ast::node_kind::continue_stmt:
              case ast::node_kind::crew_stmt:
              case ast::node_kind::crew_expr:
              case ast::node_kind::par_expr:
              case ast::node_kind::race_expr:
                found = true;
                return false;
              case ast::node_kind::lambda_expr:
              case ast::node_kind::async_expr:
              case ast::node_kind::on_expr:
              case ast::node_kind::func_decl:
                return false;
              default:
                return !found;
              }
            });
  return found;
}

/// A verified core tree broke a rule the lowering relies on.
[[noreturn]] auto internal_error(std::string_view what, const core::node &node)
    -> void {
  std::fprintf(stderr,
               "internal compiler error: lowering a verified core `%s` at "
               "%u..%u: %.*s\n",
               core_kind_name(node.kind).c_str(), node.span.start,
               node.span.end, static_cast<int>(what.size()), what.data());
  std::abort();
}

/// The four conversions between a cell and a reference: the same address in
/// HIR, so the operand is the result.
[[nodiscard]] auto is_cell_conversion(std::string_view name) -> bool {
  return name == "cell_to_ref" || name == "cell_mut_to_ref_mut" ||
         name == "ref_to_cell" || name == "ref_mut_to_cell_mut";
}

} // namespace

auto core_lowerer::hir_type(type_id type) const -> type_id {
  const auto &entry = checked_.types.entry(type);
  return entry.kind == type_kind::existential_kind ? entry.result : type;
}

auto core_lowerer::hir_type_in_statement(type_id type) const -> type_id {
  return checked_.types.is_unit(type) ? k_unknown_type : hir_type(type);
}

auto core_lowerer::expr(const core::expr &source)
    -> core_result<ptr<hir_expr>> {
  auto lowered = [&]() -> core_result<ptr<hir_expr>> {
    using k = core::node_kind;
    switch (source.kind) {
    case k::literal:
      return literal(static_cast<const core::literal &>(source));
    case k::use:
      return use(static_cast<const core::use &>(source));
    case k::borrow:
      return borrow(static_cast<const core::borrow &>(source));
    case k::call:
      return call(static_cast<const core::call &>(source));
    case k::primitive:
      return primitive(static_cast<const core::primitive &>(source));
    case k::cast:
      return cast(static_cast<const core::cast &>(source));
    case k::tuple_init:
    case k::array_init:
    case k::struct_init:
      return aggregate(static_cast<const core::aggregate_init &>(source));
    case k::variant_init:
      return variant(static_cast<const core::variant_init &>(source));
    case k::block: {
      const auto &inner = static_cast<const core::block &>(source);
      auto made = block(inner, hir_type(inner.type));
      if (!made.has_value()) {
        return std::unexpected(made.error());
      }
      return ptr<hir_expr>(std::move(*made));
    }
    case k::match_expr: {
      const auto &matched = static_cast<const core::match_expr &>(source);
      using f = core::match_form;
      if (matched.form != f::match && matched.form != f::if_let &&
          matched.form != f::or_else && matched.form != f::try_operator) {
        return refuse("match_expr in this position", source.span);
      }
      if (in_task_body_ && matched.form != f::try_operator &&
          matched.form != f::if_let && !checked_.types.is_unit(hir_type(source.type)) &&
          branch_can_suspend(source.origin)) {
        return refuse("suspending value branch", source.span);
      }
      auto made = match(matched, hir_type(source.type));
      if (!made.has_value()) {
        return std::unexpected(made.error());
      }
      return ptr<hir_expr>(std::move(*made));
    }
    case k::lambda:
      return lambda(static_cast<const core::lambda &>(source));
    case k::function_ref: {
      // A function named as a value is named as a call names it.
      const auto &named = static_cast<const core::function_ref &>(source);
      auto made = function_callee(named.target, source.span);
      made->type = hir_type(source.type);
      return made;
    }
    case k::if_expr: {
      if (in_task_body_ && !checked_.types.is_unit(hir_type(source.type)) &&
          branch_can_suspend(source.origin)) {
        return refuse("suspending value branch", source.span);
      }
      auto made = if_expr(static_cast<const core::if_expr &>(source),
                          hir_type(source.type));
      if (!made.has_value()) {
        return std::unexpected(made.error());
      }
      return ptr<hir_expr>(std::move(*made));
    }
    default:
      return refuse(source);
    }
  }();
  if (!lowered.has_value() || source.origin == nullptr) {
    return lowered;
  }
  if ((*lowered)->origin == nullptr) {
    (*lowered)->origin = source.origin;
  }
  // What evaluating it moves no longer owns its value: clear its drop flag.
  auto cleared = drops_.clear_moved(source.origin, std::move(*lowered));
  if (!cleared.has_value()) {
    return refuse_drop(cleared.error());
  }
  return std::move(*cleared);
}

auto core_lowerer::literal(const core::literal &source)
    -> core_result<ptr<hir_expr>> {
  return ptr<hir_expr>(hir::make<hir_literal>(source.span, hir_type(source.type),
                                         source.token, source.spelling));
}

auto core_lowerer::place(const core::place &source, source_span span)
    -> core_result<ptr<hir_expr>> {
  auto made = ptr<hir_expr>{};
  switch (source.root.kind) {
  case core::place_root::origin::local: {
    if (const auto alias = aliases_.find(source.root.symbol);
        alias != aliases_.end()) {
      made = alias->second();
      break;
    }
    const auto found = symbols_.find(source.root.symbol);
    if (found == symbols_.end()) {
      return refuse("local declared outside the supported forms", span);
    }
    made = hir::make<hir_local_ref>(span, hir_type(source.root.type), found->second,
                               names_.at(found->second));
    break;
  }
  case core::place_root::origin::global:
    // A reified `static`, read in place: an owned read is refused before
    // the body is lowered (`pending_drops`).
    made = hir::make<hir_global_ref>(span, hir_type(source.root.type),
                                     source.root.name, source.root.module);
    break;
  case core::place_root::origin::temporary: {
    if (source.root.value == nullptr) {
      return refuse("temporary with no value", span);
    }
    // The value itself: `make_references_explicit` stores it in a local
    // where it is borrowed or projected, as it does for the old lowerer.
    auto value = expr(*source.root.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    made = std::move(*value);
    break;
  }
  }
  for (const auto &step : source.path) {
    const auto type = hir_type(step.type);
    switch (step.kind) {
    case core::projection::step::field:
      made = hir::make<hir_field>(span, type, std::move(made), step.name);
      break;
    case core::projection::step::tuple_element:
      made = hir::make<hir_tuple_index>(span, type, std::move(made), step.index);
      break;
    case core::projection::step::deref:
      made = hir::make<hir_unary>(span, type, ast::unary_op::deref, std::move(made));
      break;
    case core::projection::step::element: {
      if (step.subscript == nullptr) {
        return refuse("element step with no subscript", span);
      }
      auto index = expr(*step.subscript);
      if (!index.has_value()) {
        return std::unexpected(index.error());
      }
      made = hir::make<hir_index>(span, type, std::move(made), std::move(*index));
      break;
    }
    case core::projection::step::variant_payload:
      made = hir::make<hir_variant_payload>(span, type, std::move(made),
                                            step.name, step.index);
      break;
    }
  }
  return made;
}

auto core_lowerer::use(const core::use &source) -> core_result<ptr<hir_expr>> {
  // A copy and a move read the place alike; what a move leaves behind is the
  // drop schedule's concern.
  return place(source.from, source.span);
}

auto core_lowerer::borrow(const core::borrow &source)
    -> core_result<ptr<hir_expr>> {
  if (checked_.types.entry(source.type).kind != type_kind::ref_kind) {
    return refuse("borrow as a view", source.span);
  }
  auto of = place(source.of, source.span);
  if (!of.has_value()) {
    return std::unexpected(of.error());
  }
  // An inserted reborrow `&*r` of a reference `r` passes `r` itself, as the
  // old lowerer does: the address is the same.
  if (source.inserted && (*of)->kind == hir_node_kind::hir_unary) {
    auto &deref = dynamic_cast<hir_unary &>(**of);
    if (deref.op == ast::unary_op::deref &&
        checked_.types.entry(deref.operand->type).kind == type_kind::ref_kind) {
      return std::move(deref.operand);
    }
  }
  return ptr<hir_expr>(hir::make<hir_unary>(
      source.span, hir_type(source.type),
      source.is_mut ? ast::unary_op::addr_of_mut : ast::unary_op::addr_of,
      std::move(*of)));
}

auto core_lowerer::function_callee(const core::callee &target,
                                   source_span span) -> ptr<hir_expr> {
  const auto symbol = callee_symbol(target.name);
  // A function of the module being lowered is named without a module, as
  // the old lowerer names a bare call: both resolve in the current module.
  auto owner = target.owner_module == core_module_
                   ? std::optional<std::string>{}
                   : std::optional<std::string>{target.owner_module};
  return hir::make<hir_local_ref>(span, target.signature, symbol, target.name,
                             std::move(owner));
}

auto core_lowerer::callee_symbol(std::string_view name) -> symbol_id {
  auto [found, inserted] =
      callee_symbols_.try_emplace(std::string(name), next_symbol_);
  if (inserted) {
    ++next_symbol_;
  }
  return found->second;
}

auto core_lowerer::call(const core::call &source)
    -> core_result<ptr<hir_expr>> {
  const auto &target = source.target;
  switch (target.kind) {
  case core::callee::form::function:
    break;
  case core::callee::form::intrinsic:
    if (core::is_compiler_operation(target.name)) {
      return intrinsic_call(source);
    }
    // An `intrinsic def`: a function the runtime supplies, called by name.
    break;
  case core::callee::form::value: {
    // The function value is the callee, as written: an inserted borrow of
    // the place holding it is that place.
    if (target.value == nullptr) {
      internal_error("a call through a function value has no value", source);
    }
    auto callee = receiver_operand(*target.value);
    if (!callee.has_value()) {
      return callee;
    }
    auto args = ptr_vec<hir_expr>{};
    for (const auto &arg : source.args) {
      auto lowered = expr(*arg);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      args.push_back(std::move(*lowered));
    }
    return ptr<hir_expr>(hir::make<hir_call>(source.span,
                                             hir_type(source.type),
                                             std::move(*callee),
                                             std::move(args)));
  }
  }
  // A consuming receiver is passed by value in the core and as its place in
  // HIR, where the call is marked as consuming it.
  const auto consumes = target.receiver == core::receiver_mode::consuming;
  // A call the elaboration made (a `for` loop's adapter) has no record;
  // the `let` holding it marks it.
  if (consumes && (source.origin != nullptr || &source != for_adapter_) &&
      !drops_.schedule().consuming_calls.contains(source.origin)) {
    return refuse("consuming receiver with no ownership record", source.span);
  }
  auto args = ptr_vec<hir_expr>{};
  args.reserve(source.args.size());
  for (const auto &arg : source.args) {
    auto lowered = expr(*arg);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    args.push_back(std::move(*lowered));
  }
  auto made = hir::make<hir_call>(source.span, hir_type(source.type),
                             function_callee(target, source.span),
                             std::move(args));
  made->target = target.decl;
  if (consumes && source.origin != nullptr) {
    made->consumes_receiver = true;
    made->receiver_by_value = owns_receiver(checked_, target.decl);
  }
  return ptr<hir_expr>(std::move(made));
}

auto core_lowerer::intrinsic_call(const core::call &source)
    -> core_result<ptr<hir_expr>> {
  const auto &name = source.target.name;
  if (is_cell_conversion(name)) {
    if (source.args.size() != 1) {
      internal_error("a cell conversion takes one operand", source);
    }
    // Retyped as the conversion's result, which is what the old lowerer
    // gives the value (a `cell[T]` call result, say).
    auto operand = expr(*source.args.front());
    if (operand.has_value()) {
      (*operand)->type = hir_type(source.type);
    }
    return operand;
  }
  const auto type = hir_type(source.type);
  const auto operands = [&](std::size_t count) {
    if (source.args.size() != count) {
      internal_error(std::format("`{}` takes {} operand(s)", name, count),
                     source);
    }
  };
  const auto type_operand = [&]() -> type_id {
    if (source.target.generic_args.size() != 1) {
      internal_error(std::format("`{}` takes one type operand", name), source);
    }
    return source.target.generic_args.front();
  };
  if (name == "stack_buffer") {
    operands(0);
    return ptr<hir_expr>(hir::make<hir_stack_buffer>(source.span, type));
  }
  // A raw pointer is an address whatever it points at: the operand is the
  // result, retyped so the next `p[i]` reads the new element stride.
  if (name == "ptr_cast") {
    operands(1);
    auto operand = expr(*source.args.front());
    if (operand.has_value()) {
      (*operand)->type = type;
    }
    return operand;
  }
  if (name == "slice_from_raw_parts") {
    operands(2);
    auto pointer = expr(*source.args[0]);
    if (!pointer.has_value()) {
      return std::unexpected(pointer.error());
    }
    auto len = expr(*source.args[1]);
    if (!len.has_value()) {
      return std::unexpected(len.error());
    }
    return ptr<hir_expr>(hir::make<hir_slice_from_raw_parts>(
        source.span, type, std::move(*pointer), std::move(*len)));
  }
  // The storage form answers a layout question from `value_layouts`, and
  // `needs_drop` from the drop plans every drop decision reads.
  if (name == "size_of" || name == "align_of") {
    operands(0);
    auto size = ptr<hir_literal>(
        hir::make<hir_literal>(source.span, type, token_kind::int_lit, ""));
    size->question = layout_question{.wants_alignment = name == "align_of",
                                     .type = type_operand()};
    return ptr<hir_expr>(std::move(size));
  }
  if (name == "needs_drop") {
    operands(0);
    const auto drops = checked_.drop.drop_plans.contains(type_operand());
    return ptr<hir_expr>(hir::make<hir_literal>(
        source.span, type, drops ? token_kind::kw_true : token_kind::kw_false,
        drops ? "true" : "false"));
  }
  if (name == "uninit_len") {
    operands(0);
    const auto count = runtime::uninit_slot_count(
        checked_.types, checked_.types.entry(type_operand()));
    if (!count.has_value()) {
      internal_error("an `uninit` buffer with no slot count", source);
    }
    return ptr<hir_expr>(hir::make<hir_literal>(
        source.span, type, token_kind::int_lit, std::to_string(*count)));
  }
  // The container operations read the receiver as written: a value, or the
  // reference it is reached through. The storage form follows references.
  if (name == "len" || name == "container_data" || name == "as_bytes") {
    operands(1);
    auto object = receiver_operand(*source.args.front());
    if (!object.has_value() || name == "as_bytes") {
      // `str` and `slice[byte]` are both `{len, data}`: no conversion.
      return object;
    }
    if (name == "len") {
      return ptr<hir_expr>(
          hir::make<hir_container_len>(source.span, type, std::move(*object)));
    }
    return ptr<hir_expr>(
        hir::make<hir_container_data>(source.span, type, std::move(*object)));
  }
  // A generator resumes through its handle, as the old lowerer names it.
  if (name == "generator_next") {
    operands(1);
    auto object = receiver_operand(*source.args.front());
    if (!object.has_value()) {
      return object;
    }
    return ptr<hir_expr>(
        hir::make<hir_generator_next>(source.span, type, std::move(*object)));
  }
  if (name == "str_decode_scalar" || name == "str_scalar_width") {
    operands(2);
    auto object = receiver_operand(*source.args[0]);
    if (!object.has_value()) {
      return object;
    }
    auto offset = expr(*source.args[1]);
    if (!offset.has_value()) {
      return offset;
    }
    if (name == "str_decode_scalar") {
      return ptr<hir_expr>(hir::make<hir_str_decode_scalar>(
          source.span, type, std::move(*object), std::move(*offset)));
    }
    return ptr<hir_expr>(hir::make<hir_str_scalar_width>(
        source.span, type, std::move(*object), std::move(*offset)));
  }
  // `&p[i]` on a raw pointer: the old lowerer takes the address of the
  // element, which the backends compute as an offset.
  if (name == "ptr_offset") {
    operands(2);
    auto pointer = expr(*source.args[0]);
    if (!pointer.has_value()) {
      return pointer;
    }
    auto offset = expr(*source.args[1]);
    if (!offset.has_value()) {
      return offset;
    }
    const auto &pointer_entry = checked_.types.entry(source.type);
    auto element = hir::make<hir_index>(source.span,
                                        hir_type(type_operand()),
                                        std::move(*pointer), std::move(*offset));
    return ptr<hir_expr>(hir::make<hir_unary>(
        source.span, type,
        pointer_entry.is_mut ? ast::unary_op::addr_of_mut
                             : ast::unary_op::addr_of,
        std::move(element)));
  }
  // Range-indexing a builtin sequence is an index by the range, `a..b` or
  // `a..=b`, which the storage form reads.
  if (name == "range_view" || name == "range_view_inclusive") {
    operands(3);
    auto object = receiver_operand(*source.args[0]);
    if (!object.has_value()) {
      return object;
    }
    auto start = expr(*source.args[1]);
    if (!start.has_value()) {
      return start;
    }
    auto end = expr(*source.args[2]);
    if (!end.has_value()) {
      return end;
    }
    auto bounds = hir::make<hir_binary>(
        source.span, k_unknown_type,
        name == "range_view" ? ast::binary_op::range
                             : ast::binary_op::range_inclusive,
        std::move(*start), std::move(*end));
    return ptr<hir_expr>(hir::make<hir_index>(
        source.span, type, std::move(*object), std::move(bounds)));
  }
  // Joining interpolated pieces calls the runtime helper by name, as the
  // old lowerer's interpolation does.
  if (name == "rt_str_concat") {
    operands(2);
    auto args = ptr_vec<hir_expr>{};
    for (const auto &arg : source.args) {
      auto made = expr(*arg);
      if (!made.has_value()) {
        return made;
      }
      args.push_back(std::move(*made));
    }
    // A call written in the source names the declaration; one an
    // interpolation made names the helper in `std.fmt`.
    if (source.target.decl != nullptr) {
      auto made = hir::make<hir_call>(source.span, type,
                                      function_callee(source.target,
                                                      source.span),
                                      std::move(args));
      made->target = source.target.decl;
      return ptr<hir_expr>(std::move(made));
    }
    auto callee = hir::make<hir_local_ref>(source.span, k_unknown_type,
                                           callee_symbol(name),
                                           std::string(name),
                                           std::string("std.fmt"));
    return ptr<hir_expr>(hir::make<hir_call>(source.span, type,
                                             std::move(callee),
                                             std::move(args)));
  }
  // A cell is the element's address: `get` reads through it.
  if (name == "cell_get") {
    operands(1);
    auto cell = expr(*source.args.front());
    if (!cell.has_value()) {
      return cell;
    }
    return ptr<hir_expr>(hir::make<hir_unary>(
        source.span, type, ast::unary_op::deref, std::move(*cell)));
  }
  if (name == "cell_set") {
    operands(2);
    auto cell = expr(*source.args[0]);
    if (!cell.has_value()) {
      return cell;
    }
    auto value = expr(*source.args[1]);
    if (!value.has_value()) {
      return value;
    }
    return ptr<hir_expr>(hir::make<hir_cell_set>(
        source.span, type, std::move(*cell), std::move(*value)));
  }
  return refuse(std::format("intrinsic {}", name), source.span);
}

auto core_lowerer::receiver_operand(const core::expr &arg)
    -> core_result<ptr<hir_expr>> {
  // An inserted `&x` of a place not reached through a reference is `x`.
  if (arg.kind == core::node_kind::borrow) {
    const auto &lent = static_cast<const core::borrow &>(arg);
    if (lent.inserted &&
        (lent.of.path.empty() ||
         lent.of.path.back().kind != core::projection::step::deref)) {
      return place(lent.of, lent.span);
    }
  }
  return expr(arg);
}

auto core_lowerer::primitive(const core::primitive &source)
    -> core_result<ptr<hir_expr>> {
  const auto type = hir_type(source.type);
  if (source.binary.has_value()) {
    if (source.operands.size() != 2) {
      internal_error("a binary operator takes two operands", source);
    }
    auto lhs = expr(*source.operands[0]);
    if (!lhs.has_value()) {
      return std::unexpected(lhs.error());
    }
    auto rhs = expr(*source.operands[1]);
    if (!rhs.has_value()) {
      return std::unexpected(rhs.error());
    }
    return ptr<hir_expr>(hir::make<hir_binary>(source.span, type, *source.binary,
                                          std::move(*lhs), std::move(*rhs)));
  }
  if (!source.unary.has_value() || source.operands.size() != 1) {
    internal_error("a unary operator takes one operand", source);
  }
  auto operand = expr(*source.operands[0]);
  if (!operand.has_value()) {
    return std::unexpected(operand.error());
  }
  return ptr<hir_expr>(
      hir::make<hir_unary>(source.span, type, *source.unary, std::move(*operand)));
}

auto core_lowerer::cast(const core::cast &source)
    -> core_result<ptr<hir_expr>> {
  auto operand = expr(*source.operand);
  if (!operand.has_value()) {
    return std::unexpected(operand.error());
  }
  // `*mut T` to `*T` changes no bits: the pointer passes through, as the
  // old lowerer leaves it, since no tier has a cast between pointers.
  const auto &to = checked_.types.entry(source.type);
  const auto &from = checked_.types.entry(source.operand->type);
  if (to.kind == type_kind::ptr_kind && from.kind == type_kind::ptr_kind &&
      to.result == from.result) {
    return operand;
  }
  return ptr<hir_expr>(
      hir::make<hir_cast>(source.span, hir_type(source.type), std::move(*operand)));
}

auto core_lowerer::aggregate(const core::aggregate_init &source)
    -> core_result<ptr<hir_expr>> {
  const auto type = hir_type(source.type);
  auto elements = ptr_vec<hir_expr>{};
  elements.reserve(source.elements.size());
  for (const auto &element : source.elements) {
    auto lowered = expr(*element);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    elements.push_back(std::move(*lowered));
  }
  switch (source.kind) {
  case core::node_kind::tuple_init:
    return ptr<hir_expr>(
        hir::make<hir_tuple>(source.span, type, std::move(elements)));
  case core::node_kind::array_init: {
    if (!source.repeat.has_value()) {
      return ptr<hir_expr>(hir::make<hir_array_init>(
          source.span, type, std::move(elements), nullptr, nullptr));
    }
    if (elements.size() != 1) {
      internal_error("a repeated array has one element", source);
    }
    // The fill form `[v; n]`: the backends repeat the value.
    auto count = hir::make<hir_literal>(source.span,
                                        checked_.types.usize_type(),
                                        token_kind::int_lit,
                                        std::to_string(*source.repeat));
    return ptr<hir_expr>(hir::make<hir_array_init>(
        source.span, type, ptr_vec<hir_expr>{}, std::move(elements.front()),
        std::move(count)));
  }
  case core::node_kind::struct_init: {
    const auto shape = checked_.shapes.find(source.type);
    if (shape == checked_.shapes.end()) {
      return refuse("struct_init with no shape", source.span);
    }
    auto fields = std::vector<hir_struct_init_field>{};
    for (std::size_t i = 0; i < elements.size(); ++i) {
      const auto index = source.field_indices.at(i);
      fields.push_back(hir_struct_init_field{
          .name = source.field_names.at(i),
          .value = std::move(elements[i]),
          .declared = shape->second.fields.at(index).type});
    }
    return ptr<hir_expr>(
        hir::make<hir_struct_init>(source.span, type, std::move(fields)));
  }
  default:
    internal_error("not an aggregate", source);
  }
}

auto core_lowerer::variant(const core::variant_init &source)
    -> core_result<ptr<hir_expr>> {
  auto args = ptr_vec<hir_expr>{};
  for (const auto &part : source.payload) {
    auto lowered = expr(*part);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    args.push_back(std::move(*lowered));
  }
  return ptr<hir_expr>(hir::make<hir_variant_init>(
      source.span, hir_type(source.type), source.variant, std::move(args)));
}

} // namespace cinder::hir
