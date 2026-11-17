#include <format>
#include <string>
#include <unordered_set>
#include <utility>

#include "src/hir/lower_core.h"
#include "src/hir/lower_core_lowerer.h"

/// Whole modules from the core (`lower_core_program`): what `lower_module`,
/// `lower_inline_submodules` and `lower_functor_modules` make from the AST.
namespace cinder::hir {

namespace {

/// A reified static's value (`build_static_value` in the adapter) as
/// `lower_reified_value` builds it: literals, aggregates, variants, and
/// the `from_array` call that makes a collection.
auto static_value(const core::expr &value)
    -> std::expected<ptr<hir_expr>, unsupported> {
  const auto parts = [&](const ptr_vec<core::expr> &from)
      -> std::expected<ptr_vec<hir_expr>, unsupported> {
    auto out = ptr_vec<hir_expr>{};
    out.reserve(from.size());
    for (const auto &part : from) {
      auto lowered = static_value(*part);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      out.push_back(std::move(*lowered));
    }
    return out;
  };
  const auto span = value.span;
  switch (value.kind) {
  case core::node_kind::literal: {
    const auto &lit = static_cast<const core::literal &>(value);
    return ptr<hir_expr>(
        hir::make<hir_literal>(span, value.type, lit.token, lit.spelling));
  }
  case core::node_kind::array_init:
  case core::node_kind::tuple_init:
  case core::node_kind::struct_init: {
    const auto &init = static_cast<const core::aggregate_init &>(value);
    auto elements = parts(init.elements);
    if (!elements.has_value()) {
      return std::unexpected(elements.error());
    }
    if (value.kind == core::node_kind::array_init) {
      if (init.repeat.has_value()) {
        break;
      }
      return ptr<hir_expr>(hir::make<hir_array_init>(
          span, value.type, std::move(*elements), nullptr, nullptr));
    }
    if (value.kind == core::node_kind::tuple_init) {
      return ptr<hir_expr>(
          hir::make<hir_tuple>(span, value.type, std::move(*elements)));
    }
    if (init.field_names.size() != elements->size()) {
      break;
    }
    auto fields = std::vector<hir_struct_init_field>{};
    for (std::size_t i = 0; i < elements->size(); ++i) {
      fields.push_back(hir_struct_init_field{
          .name = init.field_names[i],
          .value = std::move((*elements)[i]),
          .declared = init.elements[i]->type});
    }
    return ptr<hir_expr>(
        hir::make<hir_struct_init>(span, value.type, std::move(fields)));
  }
  case core::node_kind::variant_init: {
    const auto &init = static_cast<const core::variant_init &>(value);
    auto payload = parts(init.payload);
    if (!payload.has_value()) {
      return std::unexpected(payload.error());
    }
    return ptr<hir_expr>(hir::make<hir_variant_init>(
        span, value.type, init.variant, std::move(*payload)));
  }
  case core::node_kind::call: {
    // `T::from_array([...])`, named as the old lowerer names it, with no
    // symbol: a static's initializer is no function's body.
    const auto &made = static_cast<const core::call &>(value);
    if (made.target.kind != core::callee::form::function ||
        made.args.size() != 1 ||
        made.args.front()->kind != core::node_kind::array_init) {
      break;
    }
    auto args = parts(made.args);
    if (!args.has_value()) {
      return std::unexpected(args.error());
    }
    auto call = hir::make<hir_call>(
        span, value.type,
        hir::make<hir_local_ref>(span, k_unknown_type, k_invalid_symbol_id,
                                 made.target.name, made.target.owner_module),
        std::move(*args));
    call->target = made.target.decl;
    return ptr<hir_expr>(std::move(call));
  }
  default:
    break;
  }
  return std::unexpected(unsupported{
      .kind = std::format("static value {}", core_kind_name(value.kind)),
      .span = span});
}

/// Moves the functions lowering made inside `functions` (task blocks, see
/// `hir_function::nested`) into the list, skipping a name already there, as
/// the old lowerer's `hoist_nested_functions` does.
auto hoist_nested(ptr_vec<hir_function> &functions) -> void {
  auto names = std::unordered_set<std::string>{};
  for (const auto &function : functions) {
    names.insert(function->name);
  }
  for (std::size_t i = 0; i < functions.size(); ++i) {
    auto nested = std::move(functions[i]->nested);
    functions[i]->nested.clear();
    for (auto &function : nested) {
      if (names.insert(function->name).second) {
        functions.push_back(std::move(function));
      }
    }
  }
}

} // namespace

auto lower_core_static(const core::static_global &global)
    -> std::expected<hir_static_global, unsupported> {
  if (global.value == nullptr) {
    return std::unexpected(unsupported{.kind = "static with no value",
                                       .span = source_span::dummy()});
  }
  auto value = static_value(*global.value);
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  return hir_static_global{
      .name = global.name, .type = global.type, .value = std::move(*value)};
}

auto lower_core_program(const core_lowering_input &input,
                        std::span<const core::gap> gaps,
                        const semantic::checked_types &checked,
                        const lowering_options &options)
    -> core_program_lowering {
  auto result = core_program_lowering{};
  const auto names = hir_module_names(input);
  // The driver gives a file's own module its file, not its submodules.
  auto files_seen = std::unordered_set<file_id_type>{};
  for (const auto *module : input.modules) {
    const auto own_file =
        module->file_id.has_value() && files_seen.insert(*module->file_id).second
            ? module->file_id
            : std::nullopt;
    const auto &hir_name = names.at(module);
    auto failed = false;
    const auto fail = [&](std::string item, std::string message,
                          source_span span) {
      result.failures.push_back(core_module_failure{.module = hir_name,
                                                    .item = std::move(item),
                                                    .message =
                                                        std::move(message),
                                                    .span = span,
                                                    .file_id =
                                                        module->file_id});
      failed = true;
    };
    // What the adapter left out of this module: a template is not lowered,
    // and an `intrinsic def` has no body to lower.
    for (const auto &gap : gaps) {
      if (gap.module != module->name || gap.in_template ||
          gap.item == core::gap::item_kind::intrinsic) {
        continue;
      }
      result.records.push_back(core_lowering_record{
          .module = hir_name,
          .function = gap.function,
          .from_core = false,
          .fallback = std::format("not built by the adapter: {}",
                                  gap.construct),
          .span = gap.span});
      fail(gap.function,
           gap.feature ? gap.detail
                       : std::format("`{}` could not be compiled: {} ({})",
                                     gap.function, gap.construct, gap.detail),
           gap.span);
    }
    auto functions = ptr_vec<hir_function>{};
    for (const auto &function : module->functions) {
      if (!function->generic_params.empty() &&
          !is_type_erased_default_copy(*function, *module, checked)) {
        continue;
      }
      auto record = core_lowering_record{.module = hir_name,
                                         .function = function->name,
                                         .from_core = false,
                                         .fallback = {},
                                         .span = function->span};
      auto lowered = lower_core_function(*function, module->name, hir_name,
                                         checked, options);
      if (!lowered.has_value()) {
        record.fallback = lowered.error().kind;
        record.span = lowered.error().span;
        result.records.push_back(std::move(record));
        fail(function->name,
             !lowered.error().message.empty()
                 ? lowered.error().message
                 : std::format("`{}` could not be compiled: {}",
                               function->name, lowered.error().kind),
             lowered.error().span);
        continue;
      }
      record.from_core = true;
      result.records.push_back(std::move(record));
      for (const auto &made : (*lowered)->nested) {
        result.records.push_back(core_lowering_record{.module = hir_name,
                                                      .function = made->name,
                                                      .from_core = true,
                                                      .fallback = {},
                                                      .span = made->span});
      }
      functions.push_back(std::move(*lowered));
    }
    auto statics = std::vector<hir_static_global>{};
    for (const auto &global : module->statics) {
      auto lowered = lower_core_static(global);
      if (!lowered.has_value()) {
        fail(global.name,
             !lowered.error().message.empty()
                 ? lowered.error().message
                 : std::format("the static `{}` could not be compiled: {}",
                               global.name, lowered.error().kind),
             lowered.error().span);
        continue;
      }
      statics.push_back(std::move(*lowered));
    }
    if (failed) {
      continue;
    }
    hoist_nested(functions);
    const auto span =
        functions.empty() ? source_span::dummy() : functions.front()->span;
    auto made = hir::make<hir_module>(span, hir_name, std::move(functions));
    made->statics = std::move(statics);
    made->file_id = own_file;
    result.modules.push_back(std::move(made));
  }
  return result;
}

} // namespace cinder::hir
