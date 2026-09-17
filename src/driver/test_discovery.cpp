#include "test_discovery.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <vector>

#include "parse_stage.h"
#include "src/parser/ast.h"
#include "src/parser/diagnostic.h"
#include "src/parser/source_location.h"

namespace kira::driver {
namespace {

struct discovered_case {
  std::string function_name;
  bool skip = false;
};

/// One `tests` submodule's worth of discovered members, keyed by the
/// fully-qualified path a synthesized call needs (`<parent module>.tests`).
struct discovered_suite {
  std::string suite_name;        ///< The parent module's own path.
  std::string tests_module_path; ///< `suite_name` + `.tests`.
  std::vector<discovered_case> cases;
  std::optional<std::string> before_all;
  std::optional<std::string> after_all;
  std::optional<std::string> before_each;
  std::optional<std::string> after_each;
};

[[nodiscard]] auto is_simple_named(const ast::node *node,
                                   std::string_view expected_name) -> bool {
  const auto *named = dynamic_cast<const ast::named_type *>(node);
  return named != nullptr && named->path.size() == 1 &&
        named->path.front() == expected_name;
}

/// Structural (syntax-only) match for `result[unit, test_failure]` — no
/// semantic analysis has run yet at this point in the pipeline, so this
/// looks only at the parsed shape, exactly as the spec's discovery rules
/// describe ("a return `type_expr` that structurally matches").
[[nodiscard]] auto is_result_unit_test_failure(const ast::type_expr *type)
    -> bool {
  const auto *named = dynamic_cast<const ast::named_type *>(type);
  if (named == nullptr || named->path.size() != 1 ||
      named->path.front() != "result" || named->type_args.size() != 2) {
    return false;
  }
  return is_simple_named(named->type_args[0].value.get(), "unit") &&
        is_simple_named(named->type_args[1].value.get(), "test_failure");
}

[[nodiscard]] auto is_test_shaped(const ast::func_decl &fn) -> bool {
  return fn.params.empty() && fn.return_type != nullptr &&
        is_result_unit_test_failure(fn.return_type.get());
}

[[nodiscard]] auto file_declares_main(const ast::file &f) -> bool {
  return std::ranges::any_of(f.items, [](const auto &item) -> bool {
    const auto *fn = dynamic_cast<const ast::func_decl *>(item.get());
    return fn != nullptr && fn->name == "main";
  });
}

[[nodiscard]] auto join_module_path(const std::vector<std::string> &segments)
    -> std::string {
  auto out = std::string{};
  for (const auto &segment : segments) {
    if (!out.empty()) {
      out += ".";
    }
    out += segment;
  }
  return out;
}

/// Classifies every test-shaped function declared directly inside `sub`
/// (the `tests` submodule) by name, per 61-std-test.md's Discovery rules.
/// Returns an error naming the first hook declared more than once.
[[nodiscard]] auto classify_tests_submodule(const ast::sub_module_decl &sub,
                                            discovered_suite &out)
    -> std::expected<void, std::string> {
  auto seen_before_all = false;
  auto seen_after_all = false;
  auto seen_before_each = false;
  auto seen_after_each = false;

  for (const auto &item : sub.items) {
    const auto *fn = dynamic_cast<const ast::func_decl *>(item.get());
    if (fn == nullptr || !is_test_shaped(*fn)) {
      continue;
    }
    const auto qualified = out.tests_module_path + "." + fn->name;
    if (fn->name == "before_all") {
      if (seen_before_all) {
        return std::unexpected(std::format(
            "`{}` declares more than one `before_all`", out.tests_module_path));
      }
      seen_before_all = true;
      out.before_all = qualified;
    } else if (fn->name == "after_all") {
      if (seen_after_all) {
        return std::unexpected(std::format(
            "`{}` declares more than one `after_all`", out.tests_module_path));
      }
      seen_after_all = true;
      out.after_all = qualified;
    } else if (fn->name == "before_each") {
      if (seen_before_each) {
        return std::unexpected(
            std::format("`{}` declares more than one `before_each`",
                        out.tests_module_path));
      }
      seen_before_each = true;
      out.before_each = qualified;
    } else if (fn->name == "after_each") {
      if (seen_after_each) {
        return std::unexpected(std::format(
            "`{}` declares more than one `after_each`", out.tests_module_path));
      }
      seen_after_each = true;
      out.after_each = qualified;
    } else if (fn->name.starts_with("skip_")) {
      out.cases.push_back({.function_name = fn->name, .skip = true});
    } else {
      out.cases.push_back({.function_name = fn->name, .skip = false});
    }
  }
  return {};
}

/// Builds the synthesized runner's full source text: one `suite(...)` entry
/// per discovered `tests` submodule, each case/hook a zero-arg lambda
/// wrapping a qualified call — not a bare cross-module function reference,
/// which the checker does not yet record a lowerable type for outside call
/// position (see the `check.cpp`/`codegen.cpp` fixes this module's sibling
/// commit made for the *struct-field* case; a bare cross-module value
/// reference is a separate, broader gap this sidesteps rather than chases).
[[nodiscard]] auto render_runner_source(
    const std::vector<discovered_suite> &suites) -> std::string {
  auto hook_expr = [](const std::optional<std::string> &hook) -> std::string {
    if (!hook.has_value()) {
      return "@none";
    }
    return std::format("@some(() => {}())", *hook);
  };

  auto source = std::string{"module kira_test_runner\n\n"
                            "use std.test.{case, skipped, suite, run_suites}\n\n"
                            "def main() -> int32:\n"
                            "    return run_suites([\n"};
  for (const auto &suite : suites) {
    source += std::format("        suite(\"{}\", [\n", suite.suite_name);
    for (const auto &c : suite.cases) {
      const auto *ctor = c.skip ? "skipped" : "case";
      source += std::format("            {}(\"{}.{}\", () => {}.{}()),\n", ctor,
                            suite.suite_name, c.function_name,
                            suite.tests_module_path, c.function_name);
    }
    source += std::format("        ], {}, {}, {}, {}),\n",
                          hook_expr(suite.before_all), hook_expr(suite.after_all),
                          hook_expr(suite.before_each),
                          hook_expr(suite.after_each));
  }
  source += "    ])\n";
  return source;
}

} // namespace

auto discover_and_inject_test_runner(cli_config &cfg)
    -> std::expected<void, std::string> {
  if (!cfg.test_mode) {
    return {};
  }

  const auto user_source_count =
      cfg.stdlib_boundary.value_or(static_cast<unsigned>(cfg.sources.size()));

  auto scan_cfg = cfg;
  scan_cfg.sources.resize(user_source_count);

  auto sources = source_manager{};
  auto diagnostics = diagnostic_bag{};
  auto file_has_errors = std::vector<bool>{};
  auto driver_diagnostics = std::string{};
  const auto parsed = parse_sources(scan_cfg, sources, diagnostics,
                                    file_has_errors, driver_diagnostics);

  for (const auto &input : parsed) {
    if (input.ast_file != nullptr && file_declares_main(*input.ast_file)) {
      return {};
    }
  }

  auto suites = std::vector<discovered_suite>{};
  for (const auto &input : parsed) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr) {
      continue;
    }
    const auto module_path =
        join_module_path(input.ast_file->module_decl->path);
    for (const auto &item : input.ast_file->items) {
      const auto *sub = dynamic_cast<const ast::sub_module_decl *>(item.get());
      if (sub == nullptr || sub->name != "tests" || sub->is_functor()) {
        continue;
      }
      auto suite = discovered_suite{
          .suite_name = module_path,
          .tests_module_path = module_path + ".tests",
      };
      auto classified = classify_tests_submodule(*sub, suite);
      if (!classified.has_value()) {
        return std::unexpected(classified.error());
      }
      if (!suite.cases.empty()) {
        suites.push_back(std::move(suite));
      }
    }
  }

  if (suites.empty()) {
    return {};
  }

  const auto source = render_runner_source(suites);

  auto ec = std::error_code{};
  const auto out_path =
      std::filesystem::temp_directory_path(ec) / "kira-test-runner.kira";
  if (ec) {
    return std::unexpected(
        "could not resolve a temp directory for the synthesized test runner");
  }
  auto out = std::ofstream(out_path, std::ios::trunc);
  if (!out) {
    return std::unexpected(std::format(
        "could not write the synthesized test runner to `{}`",
        out_path.string()));
  }
  out << source;
  out.close();
  if (out.fail()) {
    return std::unexpected(
        std::format("failed writing the synthesized test runner to `{}`",
                    out_path.string()));
  }

  if (cfg.stdlib_boundary.has_value()) {
    cfg.sources.insert(cfg.sources.begin() +
                           static_cast<std::ptrdiff_t>(*cfg.stdlib_boundary),
                       out_path.string());
    *cfg.stdlib_boundary += 1;
  } else {
    cfg.sources.push_back(out_path.string());
  }
  return {};
}

} // namespace kira::driver
