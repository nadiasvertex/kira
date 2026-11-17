#pragma once

// Shared stdlib injection for tests that drive `check_program` directly.
//
// `list`, `option`, `result` and friends are not compiler builtins: they are
// ordinary Cinder types declared in `src/std/*.cn` and found through the
// prelude (`checker::find_prelude_type`). A test that checks a bare module on
// its own therefore cannot so much as name `list[int32]` — it gets
// "undefined type `list`" — which is why every harness that calls
// `check_program` has to inject the same set of stdlib sources the real
// driver injects. `inject_stdlib_prelude` (`src/driver/driver.cpp`) is the
// production copy of this list; keep the two in step.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <system_error>
#include <vector>

#include "src/hir/lower_core.h"
#include "src/hir/nodes.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/testing/test_assert.h"
#include "src/testing/test_data.h"

namespace cinder::testing {

/// One injected stdlib source: the path it is registered under, and its text.
struct stdlib_source {
  std::string path; ///< Session path, e.g. `std/list.cn`.
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
/// Two of the driver's files are deliberately absent. `platform.cn` is not
/// injected verbatim by the driver either: it is spliced together with a
/// generated `TARGET_*`/`CINDER_*` constants block
/// (`assemble_platform_module_source`), which only the driver can produce,
/// and reproducing that here would drag `//src/driver` (and LLVM with it)
/// into every semantic test. `fs/path.cn` is its only consumer in the
/// stdlib, so it goes too. A test that needs either should go through the
/// driver (`src/cli_test.cpp`) instead.
inline auto stdlib_filenames() -> std::span<const char *const> {
  static constexpr std::array names = {
      "intrinsics.cn",
      "traits.cn",
      "traits.ord.cn",
      "traits.show.cn",
      "traits.numeric.cn",
      "traits.conversion.cn",
      "traits.category.cn",
      "traits.index.cn",
      "traits.hash.cn",
      "traits.scalar.cn",
      "traits.structural.cn",
      "limits.cn",
      "iter.cn",
      "prelude.cn",
      "panic.cn",
      "option.cn",
      "result.cn",
      "mem.cn",
      "list.cn",
      "shared.cn",
      "sync.cn",
      "io.cn",
      "time.cn",
      "console.cn",
      "algo.cn",
      "fmt.cn",
      "unicode_tables.cn",
      "unicode.cn",
      "string.cn",
      "deriving.cn",
      "test.cn",
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

/// Lowers a checked program to HIR as the driver does (`hir::lower_program`)
/// and fails the test when a module cannot be lowered. `entry` is the file
/// whose module comes first. `names` names the module of each file with no
/// `module` declaration; one it leaves out is named after its file id. The
/// modules after the entry's are in the core's order: its inline submodules,
/// the other files' modules (the stdlib's among them), and functor
/// instantiations.
inline auto lower_test_program(std::span<const semantic::parsed_module> parsed,
                               semantic::checked_types &checked,
                               file_id_type entry,
                               std::unordered_map<file_id_type, std::string>
                                   names,
                               const hir::lowering_options &options = {})
    -> hir::ptr_vec<hir::hir_module> {
  auto last = file_id_type{0};
  for (const auto &module : parsed) {
    names.try_emplace(module.file_id, std::format("file{}", module.file_id));
    last = std::max(last, module.file_id);
  }
  const auto no_errors = std::vector<bool>(static_cast<size_t>(last) + 1, false);
  auto lowered =
      hir::lower_program(parsed, no_errors, checked, std::move(names), options);
  for (const auto &failure : lowered.hir.failures) {
    std::cerr << std::format("module `{}` not lowered: `{}`: {} (at bytes "
                             "{}..{})\n",
                             failure.module, failure.item, failure.message,
                             failure.span.start, failure.span.end);
  }
  expect(lowered.hir.failures.empty(),
         "expected every module of the program to lower to HIR");
  auto out = hir::ptr_vec<hir::hir_module>{};
  auto &modules = lowered.hir.modules;
  for (auto &module : modules) {
    if (module->file_id == std::optional<file_id_type>(entry)) {
      out.push_back(std::move(module));
    }
  }
  expect(!out.empty(), "expected the entry file's module to be lowered");
  for (auto &module : modules) {
    if (module != nullptr) {
      out.push_back(std::move(module));
    }
  }
  return out;
}

/// The function `name` of the HIR module `module` among `modules`, or null.
inline auto find_lowered_function(const hir::ptr_vec<hir::hir_module> &modules,
                                  std::string_view module,
                                  std::string_view name)
    -> hir::hir_function * {
  for (const auto &candidate : modules) {
    if (candidate->module_name != module) {
      continue;
    }
    for (const auto &function : candidate->functions) {
      if (function->name == name) {
        return function.get();
      }
    }
  }
  return nullptr;
}

/// Takes the function `name` of the HIR module `module` out of `modules`, or
/// says it is not there (a generic template, say, which is not lowered).
inline auto take_lowered_function(hir::ptr_vec<hir::hir_module> &modules,
                                  std::string_view module,
                                  std::string_view name)
    -> std::expected<hir::ptr<hir::hir_function>, std::string> {
  for (auto &candidate : modules) {
    if (candidate->module_name != module) {
      continue;
    }
    for (auto &function : candidate->functions) {
      if (function != nullptr && function->name == name) {
        return std::move(function);
      }
    }
  }
  return std::unexpected(
      std::format("no function `{}` was lowered in `{}`", name, module));
}

} // namespace cinder::testing
