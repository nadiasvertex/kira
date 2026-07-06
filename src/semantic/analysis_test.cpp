#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analysis.h"
#include "src/k-parser/parser.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;
using kira::testing::fail;

struct source_fixture {
  std::string path;
  std::string text;
};

struct analyzed_session {
  std::string diagnostics;
  uint32_t error_count = 0;
};

auto analyze_sources(const std::vector<source_fixture> &fixtures)
    -> analyzed_session {
  auto sources = kira::source_manager{};
  auto diag = kira::diagnostic_bag{};
  auto file_has_errors = std::vector<bool>{};
  auto ast_files = std::vector<kira::ast::ptr<kira::ast::file>>{};
  auto parsed_modules = std::vector<kira::semantic::parsed_module>{};
  ast_files.reserve(fixtures.size());
  parsed_modules.reserve(fixtures.size());

  for (const auto &fixture : fixtures) {
    auto file_id = sources.add_file(fixture.path, fixture.text);
    expect(file_id.has_value(), "expected fixture source to register");

    if (file_has_errors.size() <= static_cast<size_t>(*file_id)) {
      file_has_errors.resize(static_cast<size_t>(*file_id) + 1, false);
    }

    const auto *file = sources.get(*file_id);
    expect(file != nullptr, "expected registered fixture source");

    const auto errors_before = diag.error_count();
    auto lexer = kira::lexer(file->source(), file->id(), diag);
    auto tokens = lexer.tokenize();
    auto parser = kira::parser(std::move(tokens), file->id(), diag);
    auto ast_file = parser.parse_file();

    if (diag.error_count() > errors_before) {
      file_has_errors[*file_id] = true;
    }

    parsed_modules.push_back(kira::semantic::parsed_module{
        .file_id = *file_id,
        .ast_file = ast_file.get(),
    });
    ast_files.push_back(std::move(ast_file));
  }

  [[maybe_unused]] const auto checked =
      kira::semantic::validate_semantics(parsed_modules, diag, file_has_errors);

  return analyzed_session{
      .diagnostics = kira::diagnostic_renderer(sources, false).render_all(diag),
      .error_count = diag.error_count(),
  };
}

auto test_validate_semantics_accepts_clean_session() -> void {
  const auto analyzed = analyze_sources({
      {
          .path = "geometry.kira",
          .text = "module geometry\n"
                  "module shapes:\n"
                  "  pub type circle = { pub radius: float64 }\n"
                  "module transform\n",
      },
      {
          .path = "geometry_transform.kira",
          .text = "module geometry.transform\n"
                  "pub def rotate(p: super.shapes.circle) -> super.shapes.circle:\n"
                  "  return p\n",
      },
  });

  expect(analyzed.error_count == 0, "expected clean session to validate");
  expect(analyzed.diagnostics.empty(),
         "expected no diagnostics for clean semantic session");
}

auto test_validate_semantics_reports_duplicate_module_paths() -> void {
  const auto analyzed = analyze_sources({
      {
          .path = "first.kira",
          .text = "module sample.tools\n"
                  "pub def run():\n"
                  "  return 1\n",
      },
      {
          .path = "second.kira",
          .text = "module sample.tools\n"
                  "pub def build():\n"
                  "  return 2\n",
      },
  });

  expect(analyzed.error_count > 0,
         "expected duplicate module paths to fail semantic validation");
  expect(analyzed.diagnostics.find("duplicate module path `sample.tools`") !=
             std::string::npos,
         "expected duplicate-module-path diagnostic");
}

auto test_validate_semantics_reports_missing_parent_module_declaration() -> void {
  const auto analyzed = analyze_sources({
      {
          .path = "geometry.kira",
          .text = "module geometry\n"
                  "pub def run():\n"
                  "  return 1\n",
      },
      {
          .path = "transform.kira",
          .text = "module geometry.transform\n"
                  "pub def rotate():\n"
                  "  return 2\n",
      },
  });

  expect(analyzed.error_count > 0,
         "expected missing parent declaration to fail semantic validation");
  expect(analyzed.diagnostics.find(
             "module `geometry.transform` is not declared by parent module `geometry`") !=
             std::string::npos,
         "expected missing-parent-module diagnostic");
}

auto test_validate_semantics_reports_unresolved_session_import() -> void {
  const auto analyzed = analyze_sources({
      {
          .path = "package.kira",
          .text = "module package\n"
                  "module tools\n",
      },
      {
          .path = "package_tools.kira",
          .text = "module package.tools\n"
                  "module app\n",
      },
      {
          .path = "package_tools_app.kira",
          .text = "module package.tools.app\n"
                  "use package.tools.missing\n"
                  "pub def run():\n"
                  "  return 1\n",
      },
  });

  expect(analyzed.error_count > 0,
         "expected unresolved session import to fail semantic validation");
  expect(analyzed.diagnostics.find(
             "import `package.tools.missing` does not resolve in this compilation session") !=
             std::string::npos,
         "expected unresolved-import diagnostic");
}

auto test_validate_semantics_reports_duplicate_module_scope_symbol() -> void {
  const auto analyzed = analyze_sources({
      {
          .path = "sample_tools.kira",
          .text = "module sample.tools\n"
                  "type point = int32\n"
                  "trait point:\n"
                  "  def show(self) -> str\n",
      },
  });

  expect(analyzed.error_count > 0,
         "expected duplicate module-scope symbol to fail semantic validation");
  expect(analyzed.diagnostics.find(
             "duplicate declaration name `point` in module `sample.tools`") !=
             std::string::npos,
         "expected duplicate-declaration diagnostic");
}

auto test_validate_semantics_reports_unresolved_qualified_type_path() -> void {
  const auto analyzed = analyze_sources({
      {
          .path = "package.kira",
          .text = "module package\n"
                  "module tools\n",
      },
      {
          .path = "package_tools.kira",
          .text = "module package.tools\n"
                  "module app\n",
      },
      {
          .path = "package_tools_app.kira",
          .text = "module package.tools.app\n"
                  "pub def run(value: package.tools.missing) -> int:\n"
                  "  return 1\n",
      },
  });

  expect(analyzed.error_count > 0,
         "expected unresolved qualified type path to fail semantic validation");
  expect(analyzed.diagnostics.find(
             "qualified type path `package.tools.missing` does not resolve from module `package.tools.app`") !=
             std::string::npos,
         "expected unresolved-qualified-type diagnostic");
}

auto test_validate_semantics_reports_unresolved_module_qualified_reference()
    -> void {
  const auto analyzed = analyze_sources({
      {
          .path = "package.kira",
          .text = "module package\n"
                  "module tools\n",
      },
      {
          .path = "package_tools.kira",
          .text = "module package.tools\n"
                  "module app\n",
      },
      {
          .path = "package_tools_app.kira",
          .text = "module package.tools.app\n"
                  "pub def run() -> int:\n"
                  "  package.tools.missing\n"
                  "  return 1\n",
      },
  });

  expect(analyzed.error_count > 0,
         "expected unresolved module-qualified reference to fail semantic validation");
  expect(analyzed.diagnostics.find(
             "module-qualified reference `package.tools.missing` does not resolve from module `package.tools.app`") !=
             std::string::npos,
         "expected unresolved-module-reference diagnostic");
}

} // namespace

auto main() -> int {
  try {
    test_validate_semantics_accepts_clean_session();
    test_validate_semantics_reports_duplicate_module_paths();
    test_validate_semantics_reports_missing_parent_module_declaration();
    test_validate_semantics_reports_unresolved_session_import();
    test_validate_semantics_reports_duplicate_module_scope_symbol();
    test_validate_semantics_reports_unresolved_qualified_type_path();
    test_validate_semantics_reports_unresolved_module_qualified_reference();
  } catch (const std::exception &ex) {
    std::cerr << "analysis_test failed: unhandled exception: " << ex.what() << '\n';
    std::exit(1);
  }
  return 0;
}
