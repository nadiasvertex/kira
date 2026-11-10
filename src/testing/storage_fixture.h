#pragma once

// A checked program lowered to the shared storage form and verified, for the
// tests of the backends that compile that form (`bytecode_compiler/
// storage_compile_test.cpp`, `llvm_codegen/storage_codegen_test.cpp`).

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/comptime_vm/backend.h"
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

/// A program parsed, checked, lowered to storage form, and verified. The
/// object owns everything the storage form and its environment refer to, so
/// it is built on the heap and never moved.
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
};

inline auto build_storage_program(const std::string &text)
    -> std::unique_ptr<storage_program> {
  auto out = std::make_unique<storage_program>();
  out->stdlib = parse_stdlib(out->sources, out->diag);
  const auto file_id = out->sources.add_file("sample.cn", text);
  expect(file_id.has_value(), "expected fixture source to register");
  const auto *file = out->sources.get(*file_id);
  auto lexer = cinder::lexer(file->source(), file->id(), out->diag);
  auto tokens = lexer.tokenize();
  auto parser = cinder::parser(std::move(tokens), file->id(), out->diag);
  out->ast_file = parser.parse_file();
  if (out->diag.error_count() != 0) {
    std::cerr << diagnostic_renderer(out->sources, false).render_all(out->diag);
    fail("expected fixture to parse cleanly");
  }

  auto file_has_errors =
      std::vector<bool>(static_cast<size_t>(*file_id) + 1, false);
  auto parsed_modules = out->stdlib.modules;
  parsed_modules.push_back(semantic::parsed_module{
      .file_id = *file_id, .ast_file = out->ast_file.get()});
  auto comptime = comptime_vm::vm_backend{};
  out->checked = semantic::check_program(parsed_modules, out->diag,
                                         file_has_errors, &comptime);
  if (out->diag.error_count() != 0) {
    std::cerr << diagnostic_renderer(out->sources, false).render_all(out->diag);
    fail("expected fixture to check cleanly");
  }
  auto lowered = hir::lower_module(*out->ast_file, "sample", out->checked);
  expect(lowered.has_value(), "expected the fixture to lower to HIR");
  out->module = std::move(*lowered);
  out->layouts = std::make_unique<runtime::value_layouts>(out->checked.types,
                                                          out->checked.shapes);
  out->env = std::make_unique<hir::storage_env>(
      hir::storage_env{out->checked, *out->layouts});
  out->lowering = hir::lower_to_storage(*out->module, *out->env);
  for (const auto &refused : out->lowering.refused) {
    std::cerr << "refused " << refused.name << ": " << refused.error.message
              << "\n";
  }
  expect(out->lowering.refused.empty(),
         "expected every fixture function to lower to storage form");
  const auto verdict = hir::verify(out->lowering.module, *out->env);
  if (!verdict.has_value()) {
    std::cerr << verdict.error().function << ": " << verdict.error().message
              << "\n";
    fail("expected the storage form to verify");
  }
  return out;
}

} // namespace cinder::testing
