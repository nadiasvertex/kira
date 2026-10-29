#include "src/comptime_vm/backend.h"

#include <format>
#include <utility>
#include <vector>

#include "src/bytecode/vm.h"
#include "src/comptime_vm/values.h"
#include "src/hir/lower.h"

namespace cinder::comptime_vm {

namespace {

/// How many active calls a failure lists, innermost first. Deep recursion
/// would otherwise print thousands of identical frames.
constexpr std::size_t k_max_reported_frames = 8;

[[nodiscard]] auto unsupported(std::string message)
    -> std::unexpected<semantic::comptime_failure> {
  return std::unexpected(semantic::comptime_failure{
      .reason = semantic::comptime_failure::kind::unsupported,
      .message = std::move(message),
      .frames = {}});
}

/// What went wrong, in words, for a failed run.
[[nodiscard]] auto describe(const bytecode::vm_failure &failure)
    -> std::string {
  switch (failure.reason) {
  case bytecode::panic_reason::explicit_panic:
    return std::format("it panicked: {}", failure.message);
  case bytecode::panic_reason::step_budget_exhausted:
    return "it ran too long — it did not finish within the compile-time "
           "step budget, which usually means a loop or recursion that never "
           "ends";
  case bytecode::panic_reason::stack_overflow:
    return "it recursed too deeply, which usually means recursion that never "
           "ends";
  case bytecode::panic_reason::effect_refused:
  case bytecode::panic_reason::host_failed:
    return failure.message;
  case bytecode::panic_reason::integer_overflow:
    return "an integer overflowed its type";
  case bytecode::panic_reason::integer_divide_by_zero:
    return "it divided by zero";
  case bytecode::panic_reason::index_out_of_bounds:
    return "it indexed past the end of a collection";
  default:
    return std::format("it failed with {}", failure.message);
  }
}

} // namespace

auto vm_backend::call(semantic::comptime_program &program,
                      const semantic::comptime_function &fn,
                      std::span<const comptime::value> args)
    -> std::expected<comptime::value, semantic::comptime_failure> {
  auto found = programs_.find(fn.decl);
  if (found == programs_.end()) {
    auto compiled = compile_program(program, fn);
    if (!compiled.has_value()) {
      return std::unexpected(compiled.error());
    }
    for (const auto &[decl, fingerprint] : compiled->fingerprints) {
      fingerprints_.emplace(decl, fingerprint);
    }
    found =
        programs_
            .emplace(fn.decl,
                     std::make_unique<compiled_program>(std::move(*compiled)))
            .first;
  }
  return run(program, *found->second, fn.hir_name, args);
}

auto vm_backend::evaluate(semantic::comptime_program &program,
                          const semantic::comptime_expression &expr,
                          std::span<const comptime::value> args)
    -> std::expected<comptime::value, semantic::comptime_failure> {
  // Keyed by the captures' types as well: the same expression can be run
  // with differently typed loop variables.
  auto key = std::format("{}", static_cast<const void *>(expr.expr));
  for (const auto &[name, type] : expr.captures) {
    key += std::format(";{}:{}", name, type);
  }
  auto found = expression_programs_.find(key);
  if (found == expression_programs_.end()) {
    auto compiled = compile_expression(program, expr);
    if (!compiled.has_value()) {
      return std::unexpected(compiled.error());
    }
    for (const auto &[decl, fingerprint] : compiled->fingerprints) {
      fingerprints_.emplace(decl, fingerprint);
    }
    found =
        expression_programs_
            .emplace(std::move(key),
                     std::make_unique<compiled_program>(std::move(*compiled)))
            .first;
  }
  return run(program, *found->second, "the expression", args);
}

auto vm_backend::run(semantic::comptime_program &program,
                     const compiled_program &compiled, std::string_view name,
                     std::span<const comptime::value> args) const
    -> std::expected<comptime::value, semantic::comptime_failure> {
  if (args.size() != compiled.param_types.size()) {
    return unsupported(std::format("`{}` takes {} argument(s), but {} were "
                                   "given",
                                   name, compiled.param_types.size(),
                                   args.size()));
  }
  // Quoted syntax the run holds, by handle.
  auto handles = handle_table{};
  auto slots = std::vector<bytecode::slot_value>{};
  slots.reserve(args.size());
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto slot = to_vm(program, handles, compiled.param_types[i], args[i]);
    if (!slot.has_value()) {
      return unsupported(slot.error());
    }
    slots.push_back(*slot);
  }

  // The compiler builds quoted syntax when the run asks it to.
  const auto host = bytecode::host_fn(
      [&](const bytecode::host_site &site,
          std::span<const bytecode::slot_value> operands)
          -> std::expected<bytecode::slot_value, std::string> {
        auto values = std::vector<comptime::value>{};
        for (std::size_t i = 0; i < operands.size(); ++i) {
          auto value =
              from_vm(program, handles, site.arg_types.at(i), operands[i]);
          if (!value.has_value()) {
            return std::unexpected(value.error());
          }
          values.push_back(*std::move(value));
        }
        auto result =
            program.host_call(site.operation, site.subject, values, site.span);
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        return to_vm(program, handles, site.result_type, *result);
      });
  const auto machine = bytecode::vm(compiled.module);
  const auto limits = bytecode::run_limits{
      .step_budget = step_budget_, .allow_effects = false, .host = &host};
  if (compiled.module.static_init_function.has_value()) {
    auto init =
        machine.run_hosted(*compiled.module.static_init_function, {}, limits);
    if (!init.has_value()) {
      return unsupported("its static data could not be initialized");
    }
  }
  auto result = machine.run_hosted(compiled.entry_index, slots, limits);
  if (!result.has_value()) {
    auto failure = semantic::comptime_failure{
        .reason = semantic::comptime_failure::kind::failed,
        .message = describe(result.error()),
        .frames = {}};
    for (const auto &location : result.error().frames) {
      if (failure.frames.size() == k_max_reported_frames) {
        break;
      }
      const auto &function =
          compiled.module.functions.at(location.function_index);
      auto frame = semantic::comptime_failure::frame{
          .function = function.source_function,
          .file = std::nullopt,
          .span = bytecode::span_at(function, location.offset)};
      const auto decl = compiled.functions.find(std::format(
          "{}::{}", function.source_module, function.source_function));
      if (decl != compiled.functions.end()) {
        frame.file = program.file_of(*decl->second);
      }
      failure.frames.push_back(std::move(frame));
    }
    return std::unexpected(std::move(failure));
  }
  if (!result->has_value) {
    return comptime::value::make_unit();
  }
  auto value = from_vm(program, handles, compiled.return_type, result->value);
  if (!value.has_value()) {
    return unsupported(value.error());
  }
  return std::move(*value);
}

auto vm_backend::verify(const semantic::checked_types &checked) const
    -> std::vector<std::string> {
  auto differences = std::vector<std::string>{};
  for (const auto &[decl, fingerprint] : fingerprints_) {
    auto lowered = hir::lower_function(
        *decl, checked,
        hir::lowering_options{.contract_checks = true, .compile_time = true});
    if (!lowered.has_value()) {
      differences.push_back(std::format(
          "`{}` lowered for compile-time code but not from the final results: "
          "{}",
          decl->name, lowered.error().message));
      continue;
    }
    if (hir_fingerprint(**lowered) != fingerprint) {
      differences.push_back(std::format(
          "`{}` lowered differently for compile-time code than from the final "
          "results",
          decl->name));
    }
  }
  return differences;
}

} // namespace cinder::comptime_vm
