#pragma once

// Shared stdlib injection for tests that drive `check_program` directly.
//
// `list`, `option`, `result` and friends are not compiler builtins: they are
// ordinary Kira types declared in `src/std/*.kira` and found through the
// prelude (`checker::find_prelude_type`). A test that checks a bare module on
// its own therefore cannot so much as name `list[int32]` — it gets
// "undefined type `list`" — which is why every harness that calls
// `check_program` has to inject the same set of stdlib sources the real
// driver injects. `inject_stdlib_prelude` (`src/driver/driver.cpp`) is the
// production copy of this list; keep the two in step.

#include <array>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "src/hir/lower.h"
#include "src/hir/nodes.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/testing/test_assert.h"
#include "src/testing/test_data.h"

namespace kira::testing {

/// One injected stdlib source: the path it is registered under, and its text.
struct stdlib_source {
  std::string path; ///< Session path, e.g. `std/list.kira`.
  std::string text; ///< File contents.
};

/// Locates `src/std` from a test binary's runfiles, mirroring
/// `find_test_data_dir`'s candidate search.
inline auto find_std_dir() -> fs::path {
  auto candidates = std::vector<fs::path>{};

  if (const auto *srcdir = std::getenv("TEST_SRCDIR"); srcdir != nullptr) {
    if (const auto *workspace = std::getenv("TEST_WORKSPACE");
        workspace != nullptr && *workspace != '\0') {
      candidates.emplace_back(fs::path(srcdir) / workspace / "src/std");
    }
    candidates.emplace_back(fs::path(srcdir) / "_main" / "src/std");
  }
  candidates.emplace_back("src/std");

  for (const auto &candidate : candidates) {
    auto ec = std::error_code{};
    if (fs::is_directory(candidate, ec)) {
      return candidate;
    }
  }

  fail("could not locate src/std directory");
  std::abort();
}

/// The stdlib files the driver injects, in the driver's own order.
///
/// Order is presentational rather than semantic — the whole session's module
/// index is built before any file is checked — but keeping it identical to
/// `inject_stdlib_prelude`'s makes drift between the two obvious.
///
/// Two of the driver's files are deliberately absent. `platform.kira` is not
/// injected verbatim by the driver either: it is spliced together with a
/// generated `TARGET_*`/`KIRA_*` constants block
/// (`assemble_platform_module_source`), which only the driver can produce,
/// and reproducing that here would drag `//src/driver` (and LLVM with it)
/// into every semantic test. `fs/path.kira` is its only consumer in the
/// stdlib, so it goes too. A test that needs either should go through the
/// driver (`src/cli_test.cpp`) instead.
inline auto stdlib_filenames() -> std::span<const char *const> {
  static constexpr std::array names = {
      "intrinsics.kira",
      "traits.kira",
      "traits.ord.kira",
      "traits.show.kira",
      "traits.numeric.kira",
      "traits.conversion.kira",
      "traits.category.kira",
      "traits.index.kira",
      "traits.hash.kira",
      "limits.kira",
      "iter.kira",
      "prelude.kira",
      "panic.kira",
      "option.kira",
      "result.kira",
      "mem.kira",
      "list.kira",
      "io.kira",
      "console.kira",
      "algo.kira",
      "fmt.kira",
      "unicode_tables.kira",
      "unicode.kira",
      "string.kira",
      "deriving.kira",
      "test.kira",
  };
  return names;
}

/// Reads every stdlib source the driver injects, ready to be prepended to a
/// test's own fixtures as one compilation session.
inline auto load_stdlib_sources() -> std::vector<stdlib_source> {
  const auto std_dir = find_std_dir();
  auto sources = std::vector<stdlib_source>{};
  sources.reserve(stdlib_filenames().size());
  for (const auto *filename : stdlib_filenames()) {
    sources.push_back(stdlib_source{
        .path = std::string("std/") + filename,
        .text = load_test_data_file(std_dir.string(), filename),
    });
  }
  return sources;
}

/// The parsed stdlib, owned by the caller for as long as the session it is
/// part of: `parsed_module` borrows its AST, so `ast_files` has to outlive
/// `modules`.
struct parsed_stdlib {
  std::vector<ast::ptr<ast::file>> ast_files;
  std::vector<semantic::parsed_module> modules;
};

/// Registers and parses every injected stdlib source into `sources`, ready to
/// be prepended to a test's own `parsed_module`s so `check_program` sees one
/// session — the same shape the driver builds.
///
/// Parsing the stdlib is not free, so a harness that checks many fixtures
/// should hold one `parsed_stdlib` per `source_manager` rather than rebuild
/// it per fixture.
inline auto parse_stdlib(source_manager &sources, diagnostic_bag &diag)
    -> parsed_stdlib {
  auto parsed = parsed_stdlib{};
  const auto sources_text = load_stdlib_sources();
  parsed.ast_files.reserve(sources_text.size());
  auto file_ids = std::vector<file_id_type>{};
  file_ids.reserve(sources_text.size());

  for (const auto &source : sources_text) {
    const auto file_id = sources.add_file(source.path, source.text);
    expect(file_id.has_value(), "expected stdlib source to register");
    const auto *file = sources.get(*file_id);
    expect(file != nullptr, "expected registered stdlib source");

    auto lex = lexer(file->source(), file->id(), diag);
    auto parse = parser(lex.tokenize(), file->id(), diag);
    parsed.ast_files.push_back(parse.parse_file());
    file_ids.push_back(*file_id);
  }
  expect(diag.error_count() == 0, "expected the stdlib to parse cleanly");

  parsed.modules.reserve(parsed.ast_files.size());
  for (size_t i = 0; i < parsed.ast_files.size(); ++i) {
    parsed.modules.push_back(semantic::parsed_module{
        .file_id = file_ids[i], .ast_file = parsed.ast_files[i].get()});
  }
  return parsed;
}

/// The dotted module name a parsed file declares (`std.iter` for
/// `module std.iter`), which is the name lowering files its functions under
/// and call sites name their callee's owner by.
inline auto module_name_of(const ast::file &file) -> std::string {
  if (file.module_decl == nullptr) {
    return {};
  }
  auto name = std::string{};
  for (const auto &segment : file.module_decl->path) {
    if (!name.empty()) {
      name += '.';
    }
    name += segment;
  }
  return name;
}

/// Lowers every injected stdlib module to HIR, so a fixture that calls into
/// the stdlib (`xs.push(x)` on a `list[T]`, say) has real functions to be
/// linked against. Appended to `modules`, which the caller then hands to
/// `hir::find_reachable_modules` along with its own entry module.
inline auto lower_stdlib_modules(const parsed_stdlib &stdlib,
                                 const semantic::checked_types &checked,
                                 hir::ptr_vec<hir::hir_module> &modules)
    -> void {
  for (const auto &ast_file : stdlib.ast_files) {
    auto lowered =
        hir::lower_module(*ast_file, module_name_of(*ast_file), checked);
    expect(lowered.has_value(), "expected a stdlib module to lower");
    modules.push_back(std::move(*lowered));

    auto submodules = hir::lower_inline_submodules(
        *ast_file, module_name_of(*ast_file), checked);
    expect(submodules.has_value(),
           "expected a stdlib module's inline submodules to lower");
    for (auto &submodule : *submodules) {
      modules.push_back(std::move(submodule));
    }
  }
}

} // namespace kira::testing
