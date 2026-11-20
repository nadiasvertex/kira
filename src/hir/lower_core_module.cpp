#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "src/core/verify.h"
#include "src/hir/lower_core.h"
#include "src/hir/lower_core_lowerer.h"

/// Whole modules and programs from the core (`lower_core_program`,
/// `lower_program`).
namespace cinder::hir {

namespace {

/// A reified static's value (`build_static_value` in the adapter):
/// literals, aggregates, variants, and the `from_array` call that makes a
/// collection.
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
    // `T::from_array([...])`, named by the callee's bare name with no
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
/// `hir_function::nested`) into the list, skipping a name already there.
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
                          source_span span, bool feature,
                          std::string construct) {
      result.failures.push_back(
          core_module_failure{.module = hir_name,
                              .item = std::move(item),
                              .message = std::move(message),
                              .span = span,
                              .file_id = module->file_id,
                              .feature = feature,
                              .construct = std::move(construct)});
      failed = true;
    };
    // What the adapter left out of this module: a template is not lowered,
    // and an `intrinsic def` has no body to lower.
    for (const auto &gap : gaps) {
      if (gap.module != module->name || gap.in_template ||
          gap.item == core::gap::item_kind::intrinsic) {
        continue;
      }
      fail(gap.function,
           gap.feature ? gap.detail
                       : std::format("{} ({})", gap.construct, gap.detail),
           gap.span, gap.feature, gap.construct);
    }
    auto functions = ptr_vec<hir_function>{};
    for (const auto &function : module->functions) {
      if (!function->generic_params.empty()) {
        continue;
      }
      auto lowered = lower_core_function(*function, module->name, hir_name,
                                         checked, options);
      if (!lowered.has_value()) {
        const auto feature = !lowered.error().message.empty();
        fail(function->name,
             feature ? lowered.error().message : lowered.error().kind,
             lowered.error().span, feature, lowered.error().kind);
        continue;
      }
      functions.push_back(std::move(*lowered));
    }
    auto statics = std::vector<hir_static_global>{};
    for (const auto &global : module->statics) {
      auto lowered = lower_core_static(global);
      if (!lowered.has_value()) {
        const auto feature = !lowered.error().message.empty();
        fail(global.name,
             feature ? lowered.error().message : lowered.error().kind,
             lowered.error().span, feature, lowered.error().kind);
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

auto lower_program(std::span<const semantic::parsed_module> inputs,
                   const std::vector<bool> &file_has_errors,
                   semantic::checked_types &checked,
                   std::unordered_map<file_id_type, std::string>
                       file_module_names,
                   const lowering_options &options) -> lowered_program {
  auto out = lowered_program{
      .core = core::adapt_program(
          inputs, file_has_errors, checked,
          core::adapt_options{.contract_checks = options.contract_checks}),
      .hir = {}};
  auto program = std::vector<const core::module *>{};
  program.reserve(out.core.modules.size());
  for (const auto &module : out.core.modules) {
    program.push_back(module.get());
  }
  const auto input = core_lowering_input{
      .modules = program, .file_module_names = std::move(file_module_names)};
  out.hir = lower_core_program(input, out.core.gaps, checked, options);
  // `CINDER_CORE_VERIFY`, set by every test that compiles programs: a core
  // that breaks a verifier rule is an internal compiler error. The known
  // violations are skipped. Each names the todo item that removes it.
  struct known_violation {
    std::string_view rule;
    std::string_view module;   ///< Empty for any module.
    std::string_view function; ///< Empty for any function.
  };
  static constexpr auto known = std::array{
      // No record holds the generic arguments an instance was made with
      // (a phase 5 debt of spec/unified-checker-plan.md).
      known_violation{.rule = "C8", .module = "", .function = ""},
  };
  if (std::getenv("CINDER_CORE_VERIFY") != nullptr) {
    for (const auto &v : core::verify(program, checked)) {
      if (std::ranges::any_of(known, [&](const known_violation &k) {
            return k.rule == v.rule &&
                   (k.module.empty() || k.module == v.module) &&
                   (k.function.empty() || k.function == v.function);
          })) {
        continue;
      }
      auto file = std::optional<file_id_type>{};
      for (const auto *module : program) {
        if (module->name == v.module) {
          file = module->file_id;
        }
      }
      out.hir.failures.push_back(core_module_failure{
          .module = v.module,
          .item = v.function,
          .message = std::format("the typed core breaks verifier rule {}: {}",
                                 v.rule, v.message),
          .span = v.span,
          .file_id = file,
          .feature = false,
          .construct = std::format("core verifier rule {}", v.rule)});
    }
  }
  return out;
}

} // namespace cinder::hir
