#pragma once

// A checked program lowered to the shared storage form and verified, for the
// tests of the backends that compile that form (`bytecode_compiler/
// storage_compile_test.cpp`, `llvm_codegen/storage_codegen_test.cpp`).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/comptime_vm/backend.h"
#include "src/hir/link.h"
#include "src/hir/lower.h"
#include "src/hir/storage.h"
#include "src/hir/storage_lower.h"
#include "src/hir/storage_verify.h"
#include "src/parser/diagnostic.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/runtime/value_layout.h"
#include "src/semantic/analysis.h"
#include "src/semantic/check.h"
#include "src/testing/stdlib_fixtures.h"
#include "src/testing/test_assert.h"
#include "src/testing/test_data.h"

namespace cinder::testing {

/// The text of a fixture file under `src/testdata/<dir>/`.
inline auto read_program(std::string_view dir, std::string_view name)
    -> std::string {
  const auto path = find_test_data_dir(dir);
  return load_test_data_file(path.string(), name);
}

/// Whether programs built from here on build a returned local in the result
/// storage. The backend tests run their cases with it on and again with it off,
/// and the outcomes must agree.
inline auto storage_nrvo_enabled() -> bool & {
  static auto enabled = true;
  return enabled;
}

/// One source file of a fixture program.
struct fixture_source {
  std::string path;
  std::string text;
};

/// A program parsed, checked, lowered to storage form, and verified. The
/// object owns everything the storage form and its environment refer to, so
/// it is built on the heap and never moved. The entry module is `ast_file`,
/// `module`, and `lowering`; a companion module the fixture names (see
/// `companion_names`) is lowered alongside it and compiled with it.
struct storage_program {
  source_manager sources;
  diagnostic_bag diag{};
  parsed_stdlib stdlib;
  ast::ptr<ast::file> ast_file;
  semantic::checked_types checked;
  hir::ptr<hir::hir_module> module;
  std::unique_ptr<runtime::value_layouts> layouts;
  std::unique_ptr<hir::storage_env> env;
  hir::storage_lowering lowering;
  std::vector<ast::ptr<ast::file>> companion_asts;
  std::vector<hir::ptr<hir::hir_module>> companion_modules;
  /// The whole stdlib in HIR; the modules the program reaches (through
  /// `panic`, say) are lowered to storage form after the companions.
  hir::ptr_vec<hir::hir_module> stdlib_modules;
  std::vector<hir::storage_lowering> companion_lowerings;

  /// The storage modules a backend compiles together, the entry first.
  [[nodiscard]] auto modules() const
      -> std::vector<const hir::storage_module *> {
    auto out = std::vector<const hir::storage_module *>{&lowering.module};
    for (const auto &companion : companion_lowerings) {
      out.push_back(&companion.module);
    }
    return out;
  }
};

/// The companion files a fixture names with `# with: <file>` lines, which are
/// read from the same directory and lowered with it.
inline auto companion_names(std::string_view text) -> std::vector<std::string> {
  auto out = std::vector<std::string>{};
  constexpr auto marker = std::string_view{"# with: "};
  auto rest = text;
  while (!rest.empty()) {
    const auto end = rest.find('\n');
    const auto line = rest.substr(0, end);
    if (line.starts_with(marker)) {
      out.emplace_back(line.substr(marker.size()));
    }
    rest = end == std::string_view::npos ? std::string_view{}
                                         : rest.substr(end + 1);
  }
  return out;
}

/// Lowers one checked module to HIR and then to storage form, refusing
/// nothing.
inline auto lower_storage_module(storage_program &program,
                                 const ast::file &ast_file,
                                 const std::string &module_name,
                                 uint64_t max_frame_bytes)
    -> std::pair<hir::ptr<hir::hir_module>, hir::storage_lowering> {
  auto lowered = hir::lower_module(ast_file, module_name, program.checked);
  if (!lowered.has_value()) {
    std::cerr << "HIR lowering of `" << module_name << "` at byte "
              << lowered.error().span.start << ": " << lowered.error().message
              << "\n";
  }
  expect(lowered.has_value(), "expected the fixture to lower to HIR");
  auto lowering = hir::lower_to_storage(
      **lowered, *program.env,
      hir::storage_lower_options{.nrvo = storage_nrvo_enabled(),
                                 .max_frame_bytes = max_frame_bytes});
  for (const auto &refused : lowering.refused) {
    std::cerr << "refused " << refused.name << ": " << refused.error.message
              << " (at byte " << refused.error.span.start << ")\n";
  }
  expect(lowering.refused.empty(),
         "expected every fixture function to lower to storage form");
  return {std::move(*lowered), std::move(lowering)};
}

/// Verifies every module of the program, each against the others.
inline auto verify_storage_program(const storage_program &program) -> void {
  const auto modules = program.modules();
  for (const auto *module : modules) {
    const auto verdict = hir::verify(
        *module, *program.env,
        hir::verify_options{
            .program = std::span<const hir::storage_module *const>(modules)});
    if (!verdict.has_value()) {
      std::cerr << verdict.error().function << ": " << verdict.error().message
                << "\n";
      for (const auto &function : module->functions) {
        if (function.name == verdict.error().function) {
          std::cerr << hir::dump(function, program.env->checked.types) << "\n";
        }
      }
      fail("expected the storage form to verify");
    }
  }
}

/// Builds a program from its sources, the entry module first. The entry
/// module is named by its `module` declaration, or `sample` without one.
/// `max_frame_bytes` is the lowering's frame limit; a test that wants to reach
/// a backend's own limit raises it.
inline auto build_storage_program(
    const std::vector<fixture_source> &sources,
    uint64_t max_frame_bytes = hir::k_max_frame_stack_bytes)
    -> std::unique_ptr<storage_program> {
  expect(!sources.empty(), "expected an entry module");
  auto out = std::make_unique<storage_program>();
  out->stdlib = parse_stdlib(out->sources, out->diag);
  auto parsed_modules = out->stdlib.modules;
  auto asts = std::vector<ast::ptr<ast::file>>{};
  auto last_file = size_t{0};
  for (const auto &source : sources) {
    const auto file_id = out->sources.add_file(source.path, source.text);
    expect(file_id.has_value(), "expected fixture source to register");
    const auto *file = out->sources.get(*file_id);
    auto lexer = cinder::lexer(file->source(), file->id(), out->diag);
    auto tokens = lexer.tokenize();
    auto parser = cinder::parser(std::move(tokens), file->id(), out->diag);
    asts.push_back(parser.parse_file());
    if (out->diag.error_count() != 0) {
      std::cerr << diagnostic_renderer(out->sources, false).render_all(out->diag);
      fail("expected fixture to parse cleanly");
    }
    parsed_modules.push_back(semantic::parsed_module{
        .file_id = *file_id, .ast_file = asts.back().get()});
    last_file = std::max(last_file, static_cast<size_t>(*file_id));
  }

  auto file_has_errors = std::vector<bool>(last_file + 1, false);
  auto comptime = comptime_vm::vm_backend{};
  out->checked = semantic::check_program(parsed_modules, out->diag,
                                         file_has_errors, &comptime);
  if (out->diag.error_count() != 0) {
    std::cerr << diagnostic_renderer(out->sources, false).render_all(out->diag);
    fail("expected fixture to check cleanly");
  }
  out->layouts = std::make_unique<runtime::value_layouts>(out->checked.types,
                                                          out->checked.shapes);
  out->env = std::make_unique<hir::storage_env>(
      hir::storage_env{out->checked, *out->layouts});

  const auto name_of = [](const ast::file &file) {
    auto name = module_name_of(file);
    return name.empty() ? std::string("sample") : name;
  };
  out->ast_file = std::move(asts.front());
  auto [entry_module, entry_lowering] = lower_storage_module(
      *out, *out->ast_file, name_of(*out->ast_file), max_frame_bytes);
  out->module = std::move(entry_module);
  out->lowering = std::move(entry_lowering);
  for (size_t i = 1; i < asts.size(); ++i) {
    out->companion_asts.push_back(std::move(asts[i]));
    const auto &ast_file = *out->companion_asts.back();
    auto [module, lowering] =
        lower_storage_module(*out, ast_file, name_of(ast_file), max_frame_bytes);
    out->companion_modules.push_back(std::move(module));
    out->companion_lowerings.push_back(std::move(lowering));
  }
  // The stdlib modules the program reaches are part of it. Every function of
  // a reached module must lower, as for the fixture's own modules.
  lower_stdlib_modules(out->stdlib, out->checked, out->stdlib_modules);
  auto all = hir::ptr_vec<hir::hir_module>{};
  all.push_back(std::move(out->module));
  for (auto &module : out->companion_modules) {
    all.push_back(std::move(module));
  }
  for (auto &module : out->stdlib_modules) {
    all.push_back(std::move(module));
  }
  auto reached = std::vector<std::string>{};
  for (const auto *module : hir::find_reachable_modules(*all.front(), all)) {
    reached.push_back(module->module_name);
  }
  out->module = std::move(all[0]);
  for (size_t i = 0; i < out->companion_modules.size(); ++i) {
    out->companion_modules[i] = std::move(all[i + 1]);
  }
  for (size_t i = 0; i < out->stdlib_modules.size(); ++i) {
    out->stdlib_modules[i] = std::move(all[i + 1 + out->companion_modules.size()]);
  }
  for (const auto &module : out->stdlib_modules) {
    if (std::ranges::find(reached, module->module_name) == reached.end()) {
      continue;
    }
    auto lowering = hir::lower_to_storage(
        *module, *out->env,
        hir::storage_lower_options{.nrvo = storage_nrvo_enabled(),
                                   .max_frame_bytes = max_frame_bytes});
    for (const auto &refused : lowering.refused) {
      std::cerr << "refused " << module->module_name << "." << refused.name
                << ": " << refused.error.message << "\n";
    }
    expect(lowering.refused.empty(),
           "expected every function of a reached stdlib module to lower");
    out->companion_lowerings.push_back(std::move(lowering));
  }
  verify_storage_program(*out);
  return out;
}

/// Builds a program from one source, the usual fixture.
inline auto build_storage_program(
    const std::string &text,
    uint64_t max_frame_bytes = hir::k_max_frame_stack_bytes)
    -> std::unique_ptr<storage_program> {
  return build_storage_program(
      std::vector<fixture_source>{{.path = "sample.cn", .text = text}},
      max_frame_bytes);
}

/// Builds the fixture file `name` under `src/testdata/<dir>/` together with
/// the companions it names.
inline auto open_storage_program(
    std::string_view dir, std::string_view name,
    uint64_t max_frame_bytes = hir::k_max_frame_stack_bytes)
    -> std::unique_ptr<storage_program> {
  auto sources = std::vector<fixture_source>{
      {.path = std::string(name), .text = read_program(dir, name)}};
  for (const auto &companion : companion_names(sources.front().text)) {
    sources.push_back(
        {.path = companion, .text = read_program(dir, companion)});
  }
  return build_storage_program(sources, max_frame_bytes);
}

} // namespace cinder::testing
