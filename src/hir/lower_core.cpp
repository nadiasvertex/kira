#include "src/hir/lower_core.h"

#include <format>
#include <string>
#include <unordered_map>
#include <utility>

#include "src/hir/lower_core_lowerer.h"
#include "src/hir/reference_check.h"
#include "src/hir/tail_calls.h"

/// Functions, and the substitution of core-lowered functions into the
/// modules the old lowerer made.
namespace cinder::hir {

namespace {

/// What of `schedule` this lowering cannot emit yet: nothing when the
/// function has nothing to drop.
[[nodiscard]] auto pending_drops(const drop_schedule &schedule)
    -> std::optional<std::string> {
  for (const auto &[key, drops] : schedule.exits) {
    if (!drops.empty()) {
      return "scope-exit drops";
    }
  }
  for (const auto &[key, assignment] : schedule.assignments) {
    if (assignment.drop_old || !assignment.sets.empty()) {
      return "assignment drops";
    }
  }
  if (schedule.flag_count != 0) {
    return "drop flags";
  }
  if (!schedule.consuming_calls.empty()) {
    return "consuming calls";
  }
  if (!schedule.owned_static_reads.empty()) {
    return "owned static reads";
  }
  if (!schedule.owned_captures.empty() || !schedule.leftover_drops.empty() ||
      !schedule.unmatched_drops.empty() || !schedule.owning_loops.empty()) {
    return "pattern or loop drops";
  }
  return std::nullopt;
}

} // namespace

auto core_kind_name(core::node_kind kind) -> std::string {
  using k = core::node_kind;
  switch (kind) {
  case k::literal: return "literal";
  case k::use: return "use";
  case k::borrow: return "borrow";
  case k::call: return "call";
  case k::function_ref: return "function_ref";
  case k::primitive: return "primitive";
  case k::cast: return "cast";
  case k::tuple_init: return "tuple_init";
  case k::array_init: return "array_init";
  case k::struct_init: return "struct_init";
  case k::variant_init: return "variant_init";
  case k::lambda: return "lambda";
  case k::block: return "block";
  case k::if_expr: return "if_expr";
  case k::match_expr: return "match_expr";
  case k::await_expr: return "await_expr";
  case k::let: return "let";
  case k::assign: return "assign";
  case k::expr_stmt: return "expr_stmt";
  case k::while_loop: return "while_loop";
  case k::break_stmt: return "break_stmt";
  case k::continue_stmt: return "continue_stmt";
  case k::return_stmt: return "return_stmt";
  case k::yield_stmt: return "yield_stmt";
  case k::contract_check: return "contract_check";
  }
  return "unknown";
}

auto core_lowerer::function(const core::function &source)
    -> core_result<ptr<hir_function>> {
  if (source.decl == nullptr || source.body == nullptr) {
    return refuse("function with no declaration", source.span);
  }
  if (source.coroutine != core::coroutine_kind::none) {
    return refuse("coroutine", source.span);
  }
  if (source.receiver == core::receiver_mode::consuming) {
    return refuse("consuming receiver", source.span);
  }
  // The drop schedule is keyed by AST node; the old lowerer computes it the
  // same way, from the declaration.
  drops_ = compute_drop_schedule(*source.decl, checked_);
  if (const auto pending = pending_drops(drops_)) {
    return refuse(*pending, source.span);
  }
  auto params = std::vector<hir_param>{};
  for (const auto &param : source.params) {
    params.push_back(hir_param{.symbol = declare(param.symbol, param.name),
                               .name = param.name,
                               .type = hir_type(param.type)});
  }
  const auto return_type = hir_type(source.return_type);
  auto body = block(*source.body, return_type);
  if (!body.has_value()) {
    return std::unexpected(body.error());
  }
  auto made = hir::make<hir_function>(source.span, source.name, std::move(params),
                                 return_type, std::move(*body));
  auto temporary_drop = std::optional<unsupported>{};
  make_references_explicit(
      *made, checked_, [this]() -> symbol_id { return mint(); },
      [&](const temporary &temp, ptr_vec<hir_node> &) -> void {
        if (!temporary_drop.has_value()) {
          temporary_drop = unsupported{
              .kind = std::format("drop of a temporary `{}`",
                                  checked_.types.display(temp.type)),
              .span = source.span};
        }
      },
      [this](const void *origin) -> std::optional<const void *> {
        const auto found = drops_.temporary_ends.find(origin);
        if (found == drops_.temporary_ends.end()) {
          return std::nullopt;
        }
        return found->second;
      });
  if (temporary_drop.has_value()) {
    return std::unexpected(*temporary_drop);
  }
  mark_tail_calls(*made);
  static_cast<void>(options_);
  return made;
}

auto lower_core_function(const core::function &function,
                         std::string_view core_module,
                         std::string_view hir_module,
                         const semantic::checked_types &checked,
                         const lowering_options &options)
    -> std::expected<ptr<hir_function>, unsupported> {
  auto lowerer = core_lowerer(checked, options, core_module, hir_module);
  return lowerer.function(function);
}

auto substitute_core_functions(ptr_vec<hir_module> &modules,
                               const core_lowering_input &input,
                               const semantic::checked_types &checked,
                               const lowering_options &options)
    -> std::vector<core_lowering_record> {
  // The core name of each file's own module: the first core module built
  // from that file.
  auto file_core_names = std::unordered_map<file_id_type, std::string>{};
  for (const auto *module : input.modules) {
    if (module->file_id.has_value()) {
      file_core_names.try_emplace(*module->file_id, module->name);
    }
  }
  const auto hir_name_of = [&](const core::module &module) -> std::string {
    if (!module.file_id.has_value()) {
      return module.name;
    }
    const auto hir = input.file_module_names.find(*module.file_id);
    const auto core = file_core_names.find(*module.file_id);
    if (hir == input.file_module_names.end() ||
        core == file_core_names.end() || !core->second.empty()) {
      return module.name;
    }
    return module.name.empty() ? hir->second
                               : std::format("{}.{}", hir->second, module.name);
  };
  // (HIR module, function name) to the core function and its module.
  struct source_function {
    const core::function *function = nullptr;
    const core::module *module = nullptr;
  };
  auto sources = std::unordered_map<std::string, source_function>{};
  for (const auto *module : input.modules) {
    const auto hir_module = hir_name_of(*module);
    for (const auto &function : module->functions) {
      if (!function->generic_params.empty()) {
        continue;
      }
      sources.try_emplace(std::format("{}\n{}", hir_module, function->name),
                          source_function{.function = function.get(),
                                          .module = module});
    }
  }
  auto records = std::vector<core_lowering_record>{};
  for (auto &module : modules) {
    for (auto &function : module->functions) {
      auto record = core_lowering_record{.module = module->module_name,
                                         .function = function->name,
                                         .from_core = false,
                                         .fallback = {},
                                         .span = function->span};
      const auto found = sources.find(
          std::format("{}\n{}", module->module_name, function->name));
      if (found == sources.end()) {
        record.fallback = "not built by the adapter";
        records.push_back(std::move(record));
        continue;
      }
      auto lowered =
          lower_core_function(*found->second.function,
                              found->second.module->name, module->module_name,
                              checked, options);
      if (!lowered.has_value()) {
        record.fallback = lowered.error().kind;
        record.span = lowered.error().span;
        records.push_back(std::move(record));
        continue;
      }
      record.from_core = true;
      function = std::move(*lowered);
      records.push_back(std::move(record));
    }
  }
  return records;
}

} // namespace cinder::hir
