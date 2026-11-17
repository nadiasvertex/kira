#include "src/hir/lower_core.h"

#include <algorithm>
#include <format>
#include <string>
#include <unordered_map>
#include <utility>

#include "src/hir/lower_core_lowerer.h"
#include "src/hir/reference_check.h"
#include "src/hir/tail_calls.h"

/// Functions.
namespace cinder::hir {

auto hir_module_names(const core_lowering_input &input)
    -> std::unordered_map<const core::module *, std::string> {
  // The core name of each file's own module: the first core module built
  // from that file.
  auto file_core_names = std::unordered_map<file_id_type, std::string>{};
  for (const auto *module : input.modules) {
    if (module->file_id.has_value()) {
      file_core_names.try_emplace(*module->file_id, module->name);
    }
  }
  auto names = std::unordered_map<const core::module *, std::string>{};
  for (const auto *module : input.modules) {
    auto name = module->name;
    if (module->file_id.has_value()) {
      const auto hir = input.file_module_names.find(*module->file_id);
      const auto core = file_core_names.find(*module->file_id);
      if (hir != input.file_module_names.end() &&
          core != file_core_names.end() && core->second.empty()) {
        name = module->name.empty()
                   ? hir->second
                   : std::format("{}.{}", hir->second, module->name);
      }
    }
    names.emplace(module, std::move(name));
  }
  return names;
}

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
  case k::host_call: return "host_call";
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
  hoist_ = nullptr;
  compile_time_ = source.compile_time;
  function_name_ = source.declared_name;
  // HIR passes a consumed receiver by reference to a callee that owns it
  // (`hir_function::owns_receiver`); the core's `self` is the value.
  const auto consuming = source.receiver == core::receiver_mode::consuming;
  // The drop schedule is keyed by AST node, and computed from the
  // declaration's ownership CFG.
  drops_.start(compute_drop_schedule(*source.decl, checked_));
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
      if (source.receiver_type == k_unknown_type) {
        pop_scope();
        return refuse("consuming receiver with no checked type", source.span);
      }
      type = source.receiver_type;
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
    own(param.origin, symbol);
    drops_.declare_flags(param.origin, param.span, prelude);
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
  // The body shares the parameters' scope: a destructuring parameter's
  // bindings, which the body's first pattern `let` makes, belong to the
  // function's scope (keyed by the declaration), not the body's.
  auto body = block_in_scope(*source.body, body_type);
  function_body_ = nullptr;
  if (!body.has_value()) {
    pop_scope();
    return std::unexpected(body.error());
  }
  auto &stmts = (*body)->stmts;
  // A coroutine dropped before its body first runs returns at once: the
  // check follows the parameters' flags and the preconditions.
  if ((is_generator || is_task) && source.body->origin != nullptr) {
    if (source.start == nullptr) {
      pop_scope();
      return refuse("coroutine with no start key", source.span);
    }
    auto cancel = cancel_check(source.start, source.span);
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
  // A function whose body is an expression returns it, with no drops of its
  // parameters.
  if (!source.expression_body &&
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
      !made->params.empty() &&
      source.receiver == core::receiver_mode::consuming;
  if (auto finished = explicit_references(*made); !finished) {
    return std::unexpected(finished.error());
  }
  mark_tail_calls(*made);
  // The task blocks' functions, which the module holds beside this one.
  made->nested = take_nested();
  static_cast<void>(options_);
  return made;
}

auto core_lowerer::expression_function(const core::function &source)
    -> core_result<ptr<hir_function>> {
  // Nothing it owns is dropped: it runs once, while compiling.
  drops_.start({});
  compile_time_ = true;
  function_name_ = source.name;
  if (source.body == nullptr || source.body->result == nullptr ||
      !source.body->statements.empty()) {
    return refuse("compile-time expression with no value", source.span);
  }
  push_scope();
  auto params = std::vector<hir_param>{};
  for (const auto &param : source.params) {
    const auto symbol = declare(param.symbol, param.name);
    params.push_back(hir_param{
        .symbol = symbol, .name = param.name, .type = hir_type(param.type)});
  }
  auto value = expr(*source.body->result);
  pop_scope();
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  const auto return_type = hir_type(source.return_type);
  auto stmts = ptr_vec<hir_node>{};
  stmts.push_back(hir::make<hir_return>(source.span, std::move(*value)));
  auto made = hir::make<hir_function>(
      source.span, source.name, std::move(params), return_type,
      hir::make<hir_block>(source.span, return_type, std::move(stmts)),
      coroutine_kind::none, k_unknown_type);
  if (auto finished = explicit_references(*made); !finished) {
    return std::unexpected(finished.error());
  }
  mark_tail_calls(*made);
  made->nested = take_nested();
  return made;
}

auto lower_core_comptime_function(const core::function &function,
                                  std::string_view module,
                                  const semantic::checked_types &checked)
    -> std::expected<ptr<hir_function>, unsupported> {
  static const auto options =
      lowering_options{.contract_checks = true, .compile_time = true};
  auto lowerer = core_lowerer(checked, options, module, module);
  if (function.decl == nullptr) {
    return lowerer.expression_function(function);
  }
  return lowerer.function(function);
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

} // namespace cinder::hir
