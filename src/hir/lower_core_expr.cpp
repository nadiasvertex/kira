#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>

#include "src/core/intrinsics.h"
#include "src/hir/lower_core_lowerer.h"

/// Expressions, places and calls.
namespace cinder::hir {

using semantic::type_kind;

namespace {

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
    case k::if_expr: {
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
  if (lowered.has_value() && (*lowered)->origin == nullptr) {
    (*lowered)->origin = source.origin;
  }
  return lowered;
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
    const auto found = symbols_.find(source.root.symbol);
    if (found == symbols_.end()) {
      return refuse("local declared outside the supported forms", span);
    }
    made = hir::make<hir_local_ref>(span, hir_type(source.root.type), found->second,
                               names_.at(found->second));
    break;
  }
  case core::place_root::origin::global:
    return refuse("global place", span);
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
      return refuse("variant_payload step", span);
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
  return ptr<hir_expr>(hir::make<hir_unary>(
      source.span, hir_type(source.type),
      source.is_mut ? ast::unary_op::addr_of_mut : ast::unary_op::addr_of,
      std::move(*of)));
}

auto core_lowerer::function_callee(const core::callee &target,
                                   source_span span) -> ptr<hir_expr> {
  auto [found, inserted] =
      callee_symbols_.try_emplace(target.name, next_symbol_);
  if (inserted) {
    ++next_symbol_;
  }
  // A function of the module being lowered is named without a module, as
  // the old lowerer names a bare call: both resolve in the current module.
  auto owner = target.owner_module == core_module_
                   ? std::optional<std::string>{}
                   : std::optional<std::string>{target.owner_module};
  return hir::make<hir_local_ref>(span, target.signature, found->second, target.name,
                             std::move(owner));
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
  case core::callee::form::value:
    return refuse("call through a function value", source.span);
  }
  if (target.receiver == core::receiver_mode::consuming) {
    return refuse("call with a consuming receiver", source.span);
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
  return ptr<hir_expr>(std::move(made));
}

auto core_lowerer::intrinsic_call(const core::call &source)
    -> core_result<ptr<hir_expr>> {
  const auto &name = source.target.name;
  if (is_cell_conversion(name)) {
    if (source.args.size() != 1) {
      internal_error("a cell conversion takes one operand", source);
    }
    return expr(*source.args.front());
  }
  return refuse(std::format("intrinsic {}", name), source.span);
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
    return refuse("repeated array_init", source.span);
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
