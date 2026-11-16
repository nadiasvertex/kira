#include "src/hir/lower_core.h"

#include <format>
#include <string>
#include <unordered_map>
#include <utility>

#include "src/hir/lower_core_lowerer.h"
#include "src/hir/reference_check.h"
#include "src/hir/tail_calls.h"
#include "src/semantic/ownership_cfg.h"

/// Functions, and the substitution of core-lowered functions into the
/// modules the old lowerer made.
namespace cinder::hir {

namespace {

/// What of `schedule` this lowering cannot emit yet.
[[nodiscard]] auto pending_drops(const drop_schedule &schedule)
    -> std::optional<std::string> {
  if (!schedule.owned_static_reads.empty()) {
    return "owned static reads";
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
  const auto is_generator =
      source.coroutine == core::coroutine_kind::generator;
  const auto is_task = source.coroutine == core::coroutine_kind::task;
  in_task_body_ = is_task;
  // HIR passes a consumed receiver by reference to a callee that owns it
  // (`hir_function::owns_receiver`); the core's `self` is the value.
  const auto consuming = source.receiver == core::receiver_mode::consuming;
  // The drop schedule is keyed by AST node; the old lowerer computes it the
  // same way, from the declaration.
  drops_.start(compute_drop_schedule(*source.decl, checked_));
  if (const auto pending = pending_drops(drops_.schedule())) {
    return refuse(*pending, source.span);
  }
  if (source.params.size() != source.decl->params.size()) {
    return refuse("parameters the declaration does not have", source.span);
  }
  // The parameters' scope is the function's own (keyed by the declaration);
  // their drop flags are declared before anything else runs.
  push_scope();
  auto params = std::vector<hir_param>{};
  auto prelude = ptr_vec<hir_node>{};
  for (std::size_t i = 0; i < source.params.size(); ++i) {
    const auto &param = source.params[i];
    const auto symbol = declare(param.symbol, param.name);
    auto type = hir_type(param.type);
    // A consumed `self` is the value in the core and its reference in HIR.
    if (consuming && i == 0) {
      const auto found =
          checked_.node_types.find(source.decl->params[0].pattern.get());
      if (found == checked_.node_types.end()) {
        pop_scope();
        return refuse("consuming receiver with no checked type", source.span);
      }
      type = found->second;
      // Each use of the value reads through the reference HIR passes.
      if (type != hir_type(param.type) &&
          checked_.types.entry(type).kind == semantic::type_kind::ref_kind) {
        aliases_.insert_or_assign(
            param.symbol, [symbol, type, value = hir_type(param.type),
                           name = param.name, span = source.span] {
              return ptr<hir_expr>(hir::make<hir_unary>(
                  span, value, ast::unary_op::deref,
                  hir::make<hir_local_ref>(span, type, symbol, name)));
            });
      }
    }
    params.push_back(
        hir_param{.symbol = symbol, .name = param.name, .type = type});
    const auto *key = source.decl->params[i].pattern.get();
    own(key, symbol);
    drops_.declare_flags(key, source.decl->params[i].span, prelude);
  }
  // What the caller sees (the generator or the task), and what the body
  // returns. A generator's body has no value: a trailing `if` or `match`
  // stands alone.
  const auto return_type = hir_type(source.return_type);
  const auto body_type =
      is_generator ? k_unknown_type : hir_type(source.body_type);
  function_body_ = source.body.get();
  tail_exit_ = nullptr;
  tail_posts_.clear();
  auto body = block(*source.body, body_type);
  function_body_ = nullptr;
  if (!body.has_value()) {
    pop_scope();
    return std::unexpected(body.error());
  }
  auto &stmts = (*body)->stmts;
  // A coroutine dropped before its body first runs returns at once: the
  // check follows the parameters' flags and the preconditions.
  if ((is_generator || is_task) && source.body->origin != nullptr) {
    auto cancel = cancel_check(semantic::ownership::generator_start(*source.decl),
                               source.span);
    if (!cancel.has_value()) {
      pop_scope();
      return std::unexpected(cancel.error());
    }
    auto checks = std::size_t{0};
    while (checks < stmts.size() &&
           stmts[checks]->kind == hir_node_kind::hir_contract_check) {
      ++checks;
    }
    for (std::size_t i = 0; i < checks; ++i) {
      prelude.push_back(std::move(stmts[i]));
    }
    prelude.push_back(std::move(*cancel));
    for (auto i = checks; i < stmts.size(); ++i) {
      prelude.push_back(std::move(stmts[i]));
    }
    stmts = std::move(prelude);
  } else if (!prelude.empty()) {
    for (auto &stmt : stmts) {
      prelude.push_back(std::move(stmt));
    }
    stmts = std::move(prelude);
  }
  // The tail returns through the postconditions after the body's drops.
  if (auto finished = finish_contracts(stmts, source.span); !finished) {
    pop_scope();
    return std::unexpected(finished.error());
  }
  // A function whose body is an expression returns it, as the old lowerer
  // makes it, with no drops of its parameters.
  if (source.decl->body_expr == nullptr &&
      (stmts.empty() || !always_exits(*stmts.back()))) {
    const auto value_typed = body_type != k_unknown_type &&
                             !checked_.types.is_unit(body_type);
    auto dropped =
        value_typed
            ? drops_.emit_tail_scope_exit_drops(source.decl, stmts)
            : drops_.emit_scope_exit_drops(source.decl, stmts);
    if (!dropped.has_value()) {
      pop_scope();
      return refuse_drop(dropped.error());
    }
  }
  pop_scope();
  // A task's body ends by handing over its result: a trailing value, or
  // `unit`.
  if (is_task && (stmts.empty() || !always_returns(*stmts.back()))) {
    auto value = ptr<hir_expr>{};
    if (!stmts.empty() && stmts.back()->kind == hir_node_kind::hir_expr_stmt &&
        !checked_.types.is_unit(body_type)) {
      value = std::move(dynamic_cast<hir_expr_stmt &>(*stmts.back()).expr);
      stmts.pop_back();
    } else {
      value = hir::make<hir_literal>(source.span,
                                     interning_types().builtin("unit"),
                                     token_kind::kw_unit, "unit");
    }
    stmts.push_back(hir::make<hir_yield>(source.span, std::move(value)));
    stmts.push_back(hir::make<hir_return>(source.span, nullptr));
  }
  in_task_body_ = false;
  const auto item_type = [&]() -> type_id {
    if (is_task) {
      return body_type;
    }
    if (is_generator) {
      return hir_type(source.yield_type);
    }
    return k_unknown_type;
  }();
  auto made = hir::make<hir_function>(
      source.span, source.name, std::move(params), return_type,
      std::move(*body),
      is_task        ? coroutine_kind::task
      : is_generator ? coroutine_kind::generator
                     : coroutine_kind::none,
      item_type);
  made->owns_receiver =
      !made->params.empty() && owns_receiver(checked_, source.decl);
  auto temporary_drop = std::optional<unsupported>{};
  make_references_explicit(
      *made, checked_, [this]() -> symbol_id { return mint(); },
      [&](const temporary &temp, ptr_vec<hir_node> &out) -> void {
        if (temporary_drop.has_value()) {
          return;
        }
        if (auto dropped = drops_.drop_temporary(temp, out); !dropped) {
          temporary_drop = refuse_drop(dropped.error()).error();
        }
      },
      [this](const void *origin) -> std::optional<const void *> {
        return drops_.temporary_end(origin);
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
                               const lowering_options &options,
                               const replace_fn &on_replace)
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
      if (on_replace) {
        on_replace(*module, *function, **lowered);
      }
      function = std::move(*lowered);
      records.push_back(std::move(record));
    }
  }
  return records;
}

} // namespace cinder::hir
