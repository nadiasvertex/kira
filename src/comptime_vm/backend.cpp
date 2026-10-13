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
  const auto &compiled = *found->second;
  if (args.size() != compiled.param_types.size()) {
    return unsupported(std::format("`{}` takes {} argument(s), but {} were "
                                   "given",
                                   fn.hir_name, compiled.param_types.size(),
                                   args.size()));
  }
  auto slots = std::vector<bytecode::slot_value>{};
  slots.reserve(args.size());
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto slot = to_vm(program, compiled.param_types[i], args[i]);
    if (!slot.has_value()) {
      return unsupported(slot.error());
    }
    slots.push_back(*slot);
  }

  const auto machine = bytecode::vm(compiled.module);
  const auto limits =
      bytecode::run_limits{.step_budget = step_budget_, .allow_effects = false};
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
  auto value = from_vm(program, compiled.return_type, result->value);
  if (!value.has_value()) {
    return unsupported(value.error());
  }
  return std::move(*value);
}

auto vm_backend::verify(const semantic::checked_types &checked) const
    -> std::vector<std::string> {
  auto differences = std::vector<std::string>{};
  for (const auto &[decl, fingerprint] : fingerprints_) {
    auto lowered = hir::lower_function(*decl, checked);
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
