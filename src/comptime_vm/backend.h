#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/comptime_vm/program.h"
#include "src/semantic/comptime_backend.h"

namespace cinder::comptime_vm {

/// Runs compile-time calls on the bytecode VM: lowers the callee and what it
/// reaches (`compile_program`), converts the arguments into VM values, runs
/// with a step budget and no access outside the VM, and converts the result
/// back.
class vm_backend final : public semantic::comptime_backend {
public:
  /// Instructions one compile-time call may run, so a loop that never ends
  /// becomes an error instead of a hung compiler.
  static constexpr std::uint64_t k_default_step_budget = 50'000'000;

  explicit vm_backend(std::uint64_t step_budget = k_default_step_budget)
      : step_budget_(step_budget) {}

  [[nodiscard]] auto call(semantic::comptime_program &program,
                          const semantic::comptime_function &fn,
                          std::span<const comptime::value> args)
      -> std::expected<comptime::value, semantic::comptime_failure> override;

  [[nodiscard]] auto evaluate(semantic::comptime_program &program,
                              const semantic::comptime_expression &expr,
                              std::span<const comptime::value> args)
      -> std::expected<comptime::value, semantic::comptime_failure> override;

  /// Checks that every function lowered for a compile-time call lowers the
  /// same from `checked`, the checker's final results, as it did from the
  /// view the checker kept while checking. A difference means the view was
  /// missing a record lowering reads — compile-time code then ran something
  /// other than what the program says. Returns one message per difference.
  [[nodiscard]] auto verify(const semantic::checked_types &checked) const
      -> std::vector<std::string>;

private:
  /// Runs `compiled`'s entry, which `name` names in a failure, with `args`.
  [[nodiscard]] auto run(semantic::comptime_program &program,
                         const compiled_program &compiled,
                         std::string_view name,
                         std::span<const comptime::value> args) const
      -> std::expected<comptime::value, semantic::comptime_failure>;

  std::uint64_t step_budget_;
  /// Every function any compiled program lowered, with its fingerprint.
  std::unordered_map<const ast::func_decl *, std::string> fingerprints_;
  /// One compiled program per function called at compile time. A function
  /// is only compiled once every function it reaches is checked and
  /// settled, and none of that changes afterwards.
  std::unordered_map<const ast::func_decl *, std::unique_ptr<compiled_program>>
      programs_;
  /// One compiled program per compile-time expression, by the expression
  /// and its captures' types.
  std::unordered_map<std::string, std::unique_ptr<compiled_program>>
      expression_programs_;
};

} // namespace cinder::comptime_vm
