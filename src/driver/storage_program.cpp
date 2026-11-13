#include "storage_program.h"

#include <cstdlib>
#include <format>
#include <print>
#include <string_view>

#include "src/hir/storage_verify.h"

namespace cinder::driver {

auto storage_form_enabled() -> bool {
  // The storage form is the default; `CINDER_STORAGE_FORM=0` selects the
  // legacy pointer form while it remains.
  const auto *flag = std::getenv("CINDER_STORAGE_FORM");
  return flag == nullptr || std::string_view{flag} != "0";
}

auto prepare_storage_program(std::span<const hir::hir_module *const> modules,
                             semantic::checked_types &checked)
    -> std::expected<std::unique_ptr<storage_program>, std::string> {
  auto program = std::make_unique<storage_program>();
  program->layouts =
      std::make_unique<runtime::value_layouts>(checked.types, checked.shapes);
  program->env = std::make_unique<hir::storage_env>(
      hir::storage_env{checked, *program->layouts});
  program->lowerings.reserve(modules.size());
  for (const auto *module : modules) {
    program->lowerings.push_back(hir::lower_to_storage(*module, *program->env));
    const auto &lowering = program->lowerings.back();
    if (!lowering.refused.empty()) {
      const auto &first = lowering.refused.front();
      return std::unexpected(std::format(
          "`{}.{}` cannot be compiled yet: {} (a gap in the compiler, not a "
          "mistake in your program)",
          module->module_name, first.name, first.error.message));
    }
  }
  for (const auto &lowering : program->lowerings) {
    program->modules.push_back(&lowering.module);
  }
  const auto span =
      std::span<const hir::storage_module *const>(program->modules);
  // `CINDER_STORAGE_DUMP=all` prints every function's storage form.
  if (const auto *dump = std::getenv("CINDER_STORAGE_DUMP");
      dump != nullptr && std::string_view{dump} == "all") {
    for (const auto *module : program->modules) {
      for (const auto &function : module->functions) {
        std::println(stderr, "{}", hir::dump(function, checked.types));
      }
    }
  }
  for (const auto *module : program->modules) {
    if (const auto checked_module = hir::verify(
            *module, *program->env, hir::verify_options{.program = span});
        !checked_module.has_value()) {
      // `CINDER_STORAGE_DUMP=1` prints the function the verifier rejected.
      if (std::getenv("CINDER_STORAGE_DUMP") != nullptr) {
        for (const auto &function : module->functions) {
          if (function.name == checked_module.error().function) {
            std::println(stderr, "{}", hir::dump(function, checked.types));
          }
        }
      }
      return std::unexpected(std::format(
          "internal error: the storage form of `{}.{}` did not verify: {}",
          module->name, checked_module.error().function,
          checked_module.error().message));
    }
  }
  return program;
}

} // namespace cinder::driver
