#include <string>
#include <utility>
#include <vector>

#include "src/hir/link.h"
#include "src/hir/lower.h"
#include "src/hir/nodes.h"
#include "src/parser/diagnostic.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/check.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"
#include "src/util/str.h"

namespace {

using kira::testing::expect;
namespace hir = kira::hir;

// Parses, checks, and lowers several files together as one session, keyed by
// module name — mirrors `lower_test.cpp`'s `check_fixture_multi` plus the
// `hir::lower_module` step, since `find_reachable_modules` operates on
// already-lowered `hir_module`s.
struct lowered_fixture {
  kira::source_manager sources;
  kira::diagnostic_bag diag{};
  std::vector<kira::ast::ptr<kira::ast::file>> ast_files;
  kira::semantic::checked_types checked;
  hir::ptr_vec<hir::hir_module> modules;
};

auto lower_fixture(
    const std::vector<std::pair<std::string, std::string>> &files)
    -> lowered_fixture {
  auto fixture = lowered_fixture{};
  auto parsed_modules = std::vector<kira::semantic::parsed_module>{};
  auto file_ids = std::vector<kira::file_id_type>{};

  for (const auto &[path, text] : files) {
    const auto file_id = fixture.sources.add_file(path, text);
    expect(file_id.has_value(), "expected fixture source to register");
    const auto *file = fixture.sources.get(*file_id);
    expect(file != nullptr, "expected registered fixture source");

    auto lexer = kira::lexer(file->source(), file->id(), fixture.diag);
    auto tokens = lexer.tokenize();
    auto parser = kira::parser(std::move(tokens), file->id(), fixture.diag);
    auto ast_file = parser.parse_file();
    expect(fixture.diag.error_count() == 0,
           "expected fixture to parse cleanly");

    file_ids.push_back(*file_id);
    fixture.ast_files.push_back(std::move(ast_file));
  }

  for (size_t i = 0; i < fixture.ast_files.size(); ++i) {
    parsed_modules.push_back(kira::semantic::parsed_module{
        .file_id = file_ids[i], .ast_file = fixture.ast_files[i].get()});
  }

  auto file_has_errors = std::vector<bool>(file_ids.size(), false);
  fixture.checked = kira::semantic::check_program(parsed_modules, fixture.diag,
                                                  file_has_errors);
  expect(fixture.diag.error_count() == 0, "expected fixture to check cleanly");

  for (size_t i = 0; i < fixture.ast_files.size(); ++i) {
    auto module_name = fixture.ast_files[i]->module_decl != nullptr
                           ? kira::util::join_strings(
                                 fixture.ast_files[i]->module_decl->path, ".")
                           : files[i].first;
    auto lowered =
        hir::lower_module(*fixture.ast_files[i], module_name, fixture.checked);
    expect(lowered.has_value(), "expected every fixture module to lower");
    fixture.modules.push_back(std::move(*lowered));
  }

  return fixture;
}

auto find_module(const hir::ptr_vec<hir::hir_module> &modules,
                 std::string_view name) -> const hir::hir_module & {
  for (const auto &module : modules) {
    if (module->module_name == name) {
      return *module;
    }
  }
  kira::testing::fail("expected to find a lowered module by name");
}

auto test_finds_only_entry_when_nothing_qualified() -> void {
  auto fixture = lower_fixture({
      {"app.kira", "module app\n"
                   "pub def run() -> int32:\n"
                   "    return 1\n"},
  });
  const auto &entry = find_module(fixture.modules, "app");

  const auto reachable = hir::find_reachable_modules(entry, fixture.modules);
  expect(reachable.size() == 1, "expected only the entry module");
  expect(reachable.front() == &entry, "expected the entry module itself");
}

auto test_discovers_direct_dependency() -> void {
  auto fixture = lower_fixture({
      {"tools.kira", "module tools\n"
                     "pub def double(x: int32) -> int32:\n"
                     "    return x * 2\n"},
      {"app.kira", "module app\n"
                   "use tools\n"
                   "pub def run() -> int32:\n"
                   "    return tools.double(21)\n"},
  });
  const auto &entry = find_module(fixture.modules, "app");

  const auto reachable = hir::find_reachable_modules(entry, fixture.modules);
  expect(reachable.size() == 2, "expected the entry plus its one dependency");
  expect(reachable.front() == &entry, "expected the entry module first");
  expect(reachable.back()->module_name == "tools",
         "expected `tools` to be discovered as a dependency");
}

auto test_discovers_transitive_dependency() -> void {
  auto fixture = lower_fixture({
      {"leaf.kira", "module leaf\n"
                    "pub def value() -> int32:\n"
                    "    return 7\n"},
      {"mid.kira", "module mid\n"
                   "use leaf\n"
                   "pub def relay() -> int32:\n"
                   "    return leaf.value()\n"},
      {"app.kira", "module app\n"
                   "use mid\n"
                   "pub def run() -> int32:\n"
                   "    return mid.relay()\n"},
  });
  const auto &entry = find_module(fixture.modules, "app");

  const auto reachable = hir::find_reachable_modules(entry, fixture.modules);
  expect(reachable.size() == 3,
         "expected the entry plus both transitive dependencies");
  expect(reachable.front() == &entry, "expected the entry module first");

  auto has_mid = false;
  auto has_leaf = false;
  for (const auto *module : reachable) {
    has_mid = has_mid || module->module_name == "mid";
    has_leaf = has_leaf || module->module_name == "leaf";
  }
  expect(has_mid, "expected `mid` to be discovered");
  expect(has_leaf,
         "expected `leaf` to be discovered transitively through `mid`");
}

} // namespace

auto main() -> int {
  test_finds_only_entry_when_nothing_qualified();
  test_discovers_direct_dependency();
  test_discovers_transitive_dependency();
  return 0;
}
