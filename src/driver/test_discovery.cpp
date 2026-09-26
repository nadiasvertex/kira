#include "test_discovery.h"

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
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

/// Renders the scanned source list for the "no tests" message: one path
/// named directly, several summarized with a count so the message stays
/// readable for a large build.
[[nodiscard]] auto describe_sources(const std::vector<std::string> &sources)
    -> std::string {
  if (sources.size() == 1) {
    return std::format("`{}`", sources.front());
  }
  return std::format("the {} source files given", sources.size());
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

/// Collects one suite per inline submodule named `tests` declared anywhere
/// under `items`, recursing through nested inline submodules so a `tests`
/// submodule of a submodule is discovered exactly like a top-level one —
/// inline submodules are ordinary modules, and 61-std-test.md's rule is
/// stated over every reachable module. `module_path` is the path of the
/// module whose items these are, so a nested suite is named after its own
/// parent (`demo.testkit.geometry`), not after the file's module.
///
/// A `tests` submodule is not itself descended into: a `tests` submodule
/// nested inside another `tests` submodule would have no meaningful parent
/// to name a suite after, and nothing in the spec asks for one.
[[nodiscard]] auto
collect_suites(const std::vector<std::unique_ptr<ast::node>> &items,
               const std::string &module_path,
               std::vector<discovered_suite> &out)
    -> std::expected<void, std::string> {
  for (const auto &item : items) {
    const auto *sub = dynamic_cast<const ast::sub_module_decl *>(item.get());
    if (sub == nullptr || sub->is_functor()) {
      continue;
    }
    const auto sub_path = module_path + "." + sub->name;
    if (sub->name != "tests") {
      auto nested = collect_suites(sub->items, sub_path, out);
      if (!nested.has_value()) {
        return nested;
      }
      continue;
    }
    auto suite = discovered_suite{
        .suite_name = module_path,
        .tests_module_path = sub_path,
    };
    auto classified = classify_tests_submodule(*sub, suite);
    if (!classified.has_value()) {
      return std::unexpected(classified.error());
    }
    if (!suite.cases.empty()) {
      out.push_back(std::move(suite));
    }
  }
  return {};
}

/// Builds the synthesized runner's full source text: one `suite(...)` entry
/// per discovered `tests` submodule, each case/hook a bare qualified
/// reference to the discovered function. Bare cross-module function values
/// now lower on both backends (`checked_types::resolved_fn_values`), so the
/// runner names the function directly instead of hiding it behind a
/// zero-arg lambda that only forwarded the call.
///
/// A hook a suite doesn't declare is simply left out of the call: `suite`
/// defaults every hook to `@none`, so only the hooks that exist are named.
///
/// The runner is ordinary Cinder and gets no special access, so it must be
/// able to *see* every suite: a path may only start at a module the file
/// imports (spec "Visible Modules"). Each suite is reached through an
/// import of its *anchor* — the topmost prefix of its path that some file
/// declares. Nothing above the anchor declares it, so no `module`-visibility
/// declaration can restrict the import; and everything below it, the inline
/// `tests` submodule included, is then an ordinary path through that alias.
[[nodiscard]] auto
render_runner_source(const std::vector<discovered_suite> &suites,
                     const std::set<std::string> &declared_modules)
    -> std::string {
  auto anchor_aliases = std::map<std::string, std::string>{};
  const auto anchor_of = [&](const std::string &path) -> std::string {
    for (auto dot = path.find('.'); dot != std::string::npos;
         dot = path.find('.', dot + 1)) {
      if (declared_modules.contains(path.substr(0, dot))) {
        return path.substr(0, dot);
      }
    }
    return path;
  };
  for (const auto &suite : suites) {
    const auto anchor = anchor_of(suite.tests_module_path);
    // A root module can't be renamed (`use a as b` is not a `use` form),
    // and needs no alias: distinct roots already have distinct names. A
    // deeper anchor is renamed, since two can share a leaf (`x.geometry`,
    // `y.geometry`).
    if (!anchor_aliases.contains(anchor)) {
      anchor_aliases.emplace(
          anchor, anchor.contains('.')
                      ? std::format("kira_suite_root_{}", anchor_aliases.size())
                      : anchor);
    }
  }
  // `path` always starts with its anchor, followed by `.`.
  const auto through_alias = [&](const std::string &path) -> std::string {
    const auto anchor = anchor_of(path);
    return anchor_aliases.at(anchor) + path.substr(anchor.size());
  };

  auto hook_arg =
      [&through_alias](std::string_view param,
                       const std::optional<std::string> &hook) -> std::string {
    if (!hook.has_value()) {
      return {};
    }
    return std::format(", {}: @some({})", param, through_alias(*hook));
  };

  auto source =
      std::string{"module kira_test_runner\n\n"
                  "use std.test.{case, skipped, suite, run_suites}\n"};
  for (const auto &[anchor, alias] : anchor_aliases) {
    source += alias == anchor ? std::format("use {}\n", anchor)
                              : std::format("use {} as {}\n", anchor, alias);
  }
  source += "\ndef main() -> int32:\n"
            "    return run_suites([\n";
  for (const auto &suite : suites) {
    source += std::format("        suite(\"{}\", [\n", suite.suite_name);
    for (const auto &c : suite.cases) {
      const auto *ctor = c.skip ? "skipped" : "case";
      source +=
          std::format("            {}(\"{}.{}\", {}.{}),\n", ctor,
                      suite.suite_name, c.function_name,
                      through_alias(suite.tests_module_path), c.function_name);
    }
    source += std::format("        ]{}{}{}{}),\n",
                          hook_arg("before_all", suite.before_all),
                          hook_arg("after_all", suite.after_all),
                          hook_arg("before_each", suite.before_each),
                          hook_arg("after_each", suite.after_each));
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
    auto collected = collect_suites(input.ast_file->items, module_path, suites);
    if (!collected.has_value()) {
      return std::unexpected(collected.error());
    }
  }

  if (suites.empty()) {
    // A parse error upstream is the likelier explanation than "no tests
    // here", and the real compile reports it with a location; say nothing
    // and let it.
    if (std::ranges::find(file_has_errors, true) != file_has_errors.end()) {
      return {};
    }
    return std::unexpected(std::format(
        "`--test` found no tests in {}\n"
        "  = help: a test is a function taking no parameters and returning "
        "`result[unit, test_failure]`, declared inside an inline submodule "
        "named `tests`:\n"
        "\n"
        "      module tests:\n"
        "          def test_area() -> result[unit, test_failure]:\n"
        "              return assert_eq(super.area(2.0, 3.0), 6.0)\n"
        "\n"
        "  = note: a `tests` submodule holding only hooks (`before_all`, "
        "`after_all`, `before_each`, `after_each`) and no cases contributes "
        "no suite, and a function with parameters or another return type "
        "inside `tests` is ordinary helper code, not a case\n"
        "  = note: to run a program instead of its tests, drop `--test`",
        describe_sources(scan_cfg.sources)));
  }

  auto declared_modules = std::set<std::string>{};
  for (const auto &input : parsed) {
    if (input.ast_file != nullptr && input.ast_file->module_decl != nullptr) {
      declared_modules.insert(
          join_module_path(input.ast_file->module_decl->path));
    }
  }
  const auto source = render_runner_source(suites, declared_modules);

  // Kept in memory rather than written to a fixed temp file: the runner
  // differs per project, so concurrent `--test` runs sharing one file could
  // compile each other's runner.
  const auto runner_name = std::string("<generated>/test-runner.cn");
  cfg.generated_sources.insert_or_assign(runner_name, source);

  if (cfg.stdlib_boundary.has_value()) {
    cfg.sources.insert(cfg.sources.begin() +
                           static_cast<std::ptrdiff_t>(*cfg.stdlib_boundary),
                       runner_name);
    *cfg.stdlib_boundary += 1;
  } else {
    cfg.sources.push_back(runner_name);
  }
  return {};
}

} // namespace kira::driver
