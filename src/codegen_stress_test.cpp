// Cross-checks the two increment-1/3 execution tiers against each other:
// every corpus file's `main()` is compiled and run through both the
// bytecode VM (src/bytecode_compiler + src/bytecode) and the LLVM JIT
// (src/llvm_codegen), and the two results (including which panic, if any)
// must agree exactly. Neither tier is treated as the oracle for the
// other — agreement between two independently written lowering passes
// from the same HIR is itself the thing being tested.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/bytecode/vm.h"
#include "src/bytecode_compiler/compile.h"
#include "src/hir/lower.h"
#include "src/hir/nodes.h"
#include "src/k-parser/diagnostic.h"
#include "src/k-parser/lexer.h"
#include "src/k-parser/parser.h"
#include "src/k-parser/source_location.h"
#include "src/llvm_codegen/codegen.h"
#include "src/llvm_codegen/jit_support.h"
#include "src/semantic/analysis.h"
#include "src/semantic/check.h"
#include "src/semantic/types.h"

namespace {

namespace fs = std::filesystem;
namespace hir = kira::hir;
namespace bc = kira::bytecode;
namespace bcc = kira::bytecode_compiler;
namespace lc = kira::llvm_codegen;

[[noreturn]] auto fail(const std::string &message) -> void {
  std::cerr << "codegen_stress_test failed: " << message << '\n';
  std::exit(1);
}

auto expect(bool condition, const std::string &message) -> void {
  if (!condition) {
    fail(message);
  }
}

auto candidate_corpus_dirs(std::string_view argv0) -> std::vector<fs::path> {
  auto candidates = std::vector<fs::path>{};

  if (const auto *srcdir = std::getenv("TEST_SRCDIR"); srcdir != nullptr) {
    if (const auto *workspace = std::getenv("TEST_WORKSPACE");
        workspace != nullptr && *workspace != '\0') {
      candidates.emplace_back(fs::path(srcdir) / workspace /
                              "src/testdata/codegen_stress");
    }
    candidates.emplace_back(fs::path(srcdir) / "_main" /
                            "src/testdata/codegen_stress");
  }

  if (!argv0.empty()) {
    candidates.emplace_back(fs::path(std::string(argv0) + ".runfiles") /
                            "_main" / "src/testdata/codegen_stress");
  }

  candidates.emplace_back("src/testdata/codegen_stress");
  return candidates;
}

auto find_corpus_dir(std::string_view argv0) -> fs::path {
  for (const auto &candidate : candidate_corpus_dirs(argv0)) {
    auto ec = std::error_code{};
    if (fs::is_directory(candidate, ec)) {
      return candidate;
    }
  }
  fail("could not locate codegen stress corpus directory");
  std::abort();
}

auto list_corpus_files(const fs::path &corpus_dir) -> std::vector<fs::path> {
  auto files = std::vector<fs::path>{};
  for (const auto &entry : fs::directory_iterator(corpus_dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".kira") {
      files.push_back(entry.path());
    }
  }
  std::ranges::sort(files);
  return files;
}

auto read_file(const fs::path &path) -> std::string {
  auto in = std::ifstream(path, std::ios::binary);
  expect(static_cast<bool>(in),
         std::format("failed to open `{}`", path.string()));
  auto buffer = std::ostringstream{};
  buffer << in.rdbuf();
  return buffer.str();
}

struct checked_fixture {
  kira::source_manager sources;
  kira::diagnostic_bag diag{};
  kira::ast::ptr<kira::ast::file> ast_file;
  kira::semantic::checked_types checked;
};

auto check_source(const std::string &text, const fs::path &path)
    -> checked_fixture {
  auto fixture = checked_fixture{};
  const auto file_id = fixture.sources.add_file(path.string(), text);
  expect(file_id.has_value(),
         std::format("`{}`: expected source to register", path.string()));

  const auto *file = fixture.sources.get(*file_id);
  auto lexer = kira::lexer(file->source(), file->id(), fixture.diag);
  auto tokens = lexer.tokenize();
  auto parser = kira::parser(std::move(tokens), file->id(), fixture.diag);
  fixture.ast_file = parser.parse_file();
  expect(fixture.diag.error_count() == 0,
         std::format("`{}`: expected corpus file to parse cleanly",
                     path.string()));

  auto file_has_errors =
      std::vector<bool>(static_cast<size_t>(*file_id) + 1, false);
  const auto parsed_modules = std::vector<kira::semantic::parsed_module>{
      kira::semantic::parsed_module{.file_id = *file_id,
                                    .ast_file = fixture.ast_file.get()},
  };
  fixture.checked = kira::semantic::check_program(parsed_modules, fixture.diag,
                                                  file_has_errors);
  expect(fixture.diag.error_count() == 0,
         std::format("`{}`: expected corpus file to check cleanly",
                     path.string()));
  return fixture;
}

// Both tiers' results, normalized to the same shape for comparison: a
// panic reason, or a value's raw bit pattern (comparing the union's `u`
// field compares bits regardless of which member the producer actually
// wrote through, which is exactly what "the two tiers computed the same
// answer" needs, including for floating-point results).
struct outcome {
  bool panicked = false;
  bc::panic_reason panic{};
  bool has_value = false;
  uint64_t bits = 0;
};

auto bytecode_outcome(const fs::path &path, const hir::hir_module &module,
                      const kira::semantic::type_table &types) -> outcome {
  auto compiled = bcc::compile_module(module, types);
  expect(compiled.has_value(),
         std::format("`{}`: expected bytecode_compiler to accept this "
                     "corpus file: {}",
                     path.string(), compiled.error().message));

  auto index = uint16_t{0};
  for (; index < compiled->functions.size(); ++index) {
    if (compiled->functions[index].name == "main") {
      break;
    }
  }
  expect(index < compiled->functions.size(),
         std::format("`{}`: expected a `main` function", path.string()));

  const auto vm = bc::vm{*compiled};
  auto result = vm.run(index, std::array<bc::slot_value, 0>{});
  if (!result.has_value()) {
    return outcome{.panicked = true, .panic = result.error()};
  }
  return outcome{.has_value = result->has_value, .bits = result->value.u};
}

auto llvm_outcome(const fs::path &path, const hir::hir_module &module,
                  const kira::semantic::type_table &types,
                  std::optional<bc::numeric_kind> return_kind) -> outcome {
  auto compiled = lc::compile_module(module, types);
  expect(
      compiled.has_value(),
      std::format("`{}`: expected llvm_codegen to accept this corpus file: {}",
                  path.string(), compiled.error().message));

  auto jit = lc::jit_module::create(std::move(*compiled));
  expect(jit.has_value(),
         std::format("`{}`: expected the compiled module to JIT "
                     "successfully: {}",
                     path.string(), jit.error()));

  auto result = jit->run("main", return_kind);
  if (!result.has_value()) {
    return outcome{.panicked = true, .panic = result.error()};
  }
  return outcome{.has_value = result->has_value, .bits = result->value.u};
}

auto run_one(const fs::path &path) -> void {
  const auto text = read_file(path);
  auto fixture = check_source(text, path);

  auto module_name = std::string{"sample"};
  if (fixture.ast_file->module_decl != nullptr) {
    module_name.clear();
    for (const auto &part : fixture.ast_file->module_decl->path) {
      if (!module_name.empty()) {
        module_name += '.';
      }
      module_name += part;
    }
  }
  auto lowered =
      hir::lower_module(*fixture.ast_file, module_name, fixture.checked);
  expect(lowered.has_value(),
         std::format("`{}`: expected corpus file to lower to HIR: {}",
                     path.string(), lowered.error().message));

  const auto *main_fn = static_cast<const hir::hir_function *>(nullptr);
  for (const auto &fn : (*lowered)->functions) {
    if (fn != nullptr && fn->name == "main") {
      main_fn = fn.get();
      break;
    }
  }
  expect(main_fn != nullptr,
         std::format("`{}`: expected a `main` function", path.string()));

  const auto return_kind =
      fixture.checked.types.is_unit(main_fn->return_type)
          ? std::nullopt
          : bc::numeric_kind_of(fixture.checked.types, main_fn->return_type);
  expect(return_kind.has_value() ||
             fixture.checked.types.is_unit(main_fn->return_type),
         std::format("`{}`: `main`'s return type has no scalar "
                     "representation",
                     path.string()));

  const auto vm_result =
      bytecode_outcome(path, **lowered, fixture.checked.types);
  const auto jit_result =
      llvm_outcome(path, **lowered, fixture.checked.types, return_kind);

  if (vm_result.panicked || jit_result.panicked) {
    expect(vm_result.panicked && jit_result.panicked,
           std::format("`{}`: one tier panicked and the other didn't (VM "
                       "panicked: {}, JIT panicked: {})",
                       path.string(), vm_result.panicked, jit_result.panicked));
    expect(vm_result.panic == jit_result.panic,
           std::format("`{}`: tiers panicked for different reasons",
                       path.string()));
    return;
  }

  expect(vm_result.has_value == jit_result.has_value,
         std::format("`{}`: tiers disagree on whether `main` produced a "
                     "value",
                     path.string()));
  if (vm_result.has_value) {
    expect(vm_result.bits == jit_result.bits,
           std::format("`{}`: bytecode VM and LLVM JIT disagree on `main`'s "
                       "result",
                       path.string()));
  }
}

} // namespace

auto main(int argc, char *argv[]) -> int {
  try {
    auto corpus_dir = find_corpus_dir(argc > 0 ? std::string_view(*argv)
                                               : std::string_view{});
    auto files = list_corpus_files(corpus_dir);

    expect(!files.empty(),
           "expected codegen stress corpus to contain .kira files");
    expect(files.size() >= 20,
           std::format(
               "expected a broad codegen stress corpus, found only {} files",
               files.size()));

    for (const auto &file : files) {
      run_one(file);
    }

    std::cout << "codegen_stress_test: " << files.size()
              << " program(s) agreed between the bytecode VM and the LLVM "
                 "JIT\n";
  } catch (const std::exception &ex) {
    std::cerr << "codegen_stress_test failed: unhandled exception: "
              << ex.what() << '\n';
    std::exit(1);
  }
  return 0;
}
