// Exercises `emit_object_file` end to end: compile a small program, emit a
// native object file, link it against a minimal hand-written C stub for
// `kira_codegen_panic` (standing in for `:aot_runtime.cpp`'s real
// implementation -- this test is about `emit_object_file` producing a
// linkable, runnable object, not about `aot_runtime.cpp`'s message text),
// run the resulting standalone binary as a real child process, and check
// its exit code. Same "prove it by actually running the output" spirit as
// codegen_test.cpp, one tier further down the pipeline (AOT instead of
// JIT).
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "src/hir/lower.h"
#include "src/hir/nodes.h"
#include "src/llvm_codegen/aot.h"
#include "src/llvm_codegen/codegen.h"
#include "src/parser/diagnostic.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/check.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

namespace fs = std::filesystem;
using kira::testing::expect;
namespace hir = kira::hir;
namespace lc = kira::llvm_codegen;

auto make_temp_dir() -> fs::path {
  auto base =
      fs::temp_directory_path() /
      std::format("kira_aot_test_{}", static_cast<unsigned>(::getpid()));
  auto ec = std::error_code{};
  fs::create_directories(base, ec);
  expect(!ec, "expected to create a temporary directory");
  return base;
}

auto write_file(const fs::path &path, std::string_view contents) -> void {
  auto out = std::ofstream(path, std::ios::binary);
  expect(static_cast<bool>(out),
         std::format("expected to open `{}` for writing", path.string()));
  out << contents;
}

auto compile_to_object(const std::string &text, const fs::path &object_path)
    -> void {
  auto sources = kira::source_manager{};
  auto diag = kira::diagnostic_bag{};
  const auto file_id = sources.add_file("sample.kira", text);
  expect(file_id.has_value(), "expected fixture source to register");
  const auto *file = sources.get(*file_id);

  auto lexer = kira::lexer(file->source(), file->id(), diag);
  auto tokens = lexer.tokenize();
  auto parser = kira::parser(std::move(tokens), file->id(), diag);
  auto ast_file = parser.parse_file();
  expect(diag.error_count() == 0, "expected fixture to parse cleanly");

  auto file_has_errors =
      std::vector<bool>(static_cast<size_t>(*file_id) + 1, false);
  const auto parsed_modules = std::vector<kira::semantic::parsed_module>{
      kira::semantic::parsed_module{.file_id = *file_id,
                                    .ast_file = ast_file.get()},
  };
  auto checked =
      kira::semantic::check_program(parsed_modules, diag, file_has_errors);
  expect(diag.error_count() == 0, "expected fixture to check cleanly");

  auto lowered = hir::lower_module(*ast_file, "sample", checked);
  expect(lowered.has_value(), "expected fixture to lower to HIR");

  auto compiled = lc::compile_module(**lowered, checked.types);
  expect(compiled.has_value(),
         "expected fixture to compile to an llvm::Module");

  auto emitted =
      lc::emit_object_file(std::move(*compiled), "main", object_path.string());
  expect(emitted.has_value(), emitted.has_value()
                                  ? std::string{}
                                  : std::format("expected emit_object_file to "
                                                "succeed: {}",
                                                emitted.error().message));
}

auto build_and_run(const fs::path &dir, const std::string &kira_source) -> int {
  const auto object_path = dir / "program.o";
  const auto stub_path = dir / "panic_stub.c";
  const auto output_path = dir / "program";

  compile_to_object(kira_source, object_path);
  // `kira_rt_alloc` (`src/runtime/arena.h`'s real bump-allocator
  // implementation, not linked into this hand-built test binary) backs
  // every heap allocation a compiled program's struct/array/str/list
  // construction needs — a plain `malloc` stands in here, matching this
  // stub's own "not about the real runtime's behavior, just about
  // producing a linkable, runnable object" scope.
  write_file(stub_path, "#include <stdint.h>\n"
                        "#include <stdlib.h>\n"
                        "void kira_codegen_panic(unsigned char reason) {\n"
                        "  exit(100 + reason);\n"
                        "}\n"
                        "void *kira_rt_alloc(uint64_t bytes) {\n"
                        "  return calloc(1, bytes);\n"
                        "}\n");

  const auto link_command =
      std::format(R"(cc "{}" "{}" -o "{}")", object_path.string(),
                  stub_path.string(), output_path.string());
  expect(std::system(link_command.c_str()) == 0,
         std::format("expected linking to succeed: `{}`", link_command));

  return std::system(output_path.string().c_str());
}

auto test_emits_links_and_runs_to_the_right_exit_code() -> void {
  auto dir = make_temp_dir();
  const auto status = build_and_run(dir, "module sample\n"
                                         "def main() -> int32:\n"
                                         "    return 40 + 2\n");
  expect(WIFEXITED(status) != 0, "expected the program to exit normally");
  expect(WEXITSTATUS(status) == 42,
         std::format("expected exit code 42, got {}", WEXITSTATUS(status)));
}

auto test_unit_returning_main_exits_zero() -> void {
  auto dir = make_temp_dir();
  const auto status = build_and_run(dir, "module sample\n"
                                         "def main() -> unit:\n"
                                         "    var x = 1 + 1\n");
  expect(WIFEXITED(status) != 0, "expected the program to exit normally");
  expect(WEXITSTATUS(status) == 0,
         std::format("expected exit code 0, got {}", WEXITSTATUS(status)));
}

auto test_panic_reaches_the_process_exit_code() -> void {
  auto dir = make_temp_dir();
  const auto status = build_and_run(dir, "module sample\n"
                                         "def main() -> int8:\n"
                                         "    var x: int8 = 100\n"
                                         "    return x + x\n");
  expect(WIFEXITED(status) != 0, "expected the program to exit normally");
  // The stub maps `panic_reason` to `100 + reason`; `integer_overflow` is
  // reason 0 (src/bytecode/panic.h), so this is exit code 100.
  expect(WEXITSTATUS(status) == 100,
         std::format("expected the panic to reach the process exit code as "
                     "100, got {}",
                     WEXITSTATUS(status)));
}

auto test_packed_struct_and_narrow_array_exit_codes() -> void {
  // Closes the gap the byte-precise layout work (`runtime::layout.h`'s
  // `struct_field_offset`/`struct_layout`, the `packed` modifier) left in
  // AOT-specific coverage: everything else exercising it runs through the
  // bytecode VM or the JIT, never a real linked `kira build` binary.
  auto dir = make_temp_dir();
  const auto status =
      build_and_run(dir, "module sample\n"
                         "packed type header = { pub magic: uint16, pub "
                         "flags: byte, pub len: uint32 }\n"
                         "def main() -> int8:\n"
                         "    let h: header = { magic: 7, flags: 3, len: 42 }\n"
                         "    let xs: array[int16, 5] = [10, 20, 30, 40, 50]\n"
                         "    return (h.flags as int8) + (xs[0] as int8) + "
                         "(xs[4] as int8)\n");
  expect(WIFEXITED(status) != 0, "expected the program to exit normally");
  expect(WEXITSTATUS(status) == 63,
         std::format("expected 3 (packed struct field) + 10 + 50 (narrow "
                     "array elements) == 63, got {}",
                     WEXITSTATUS(status)));
}

auto test_generator_drives_a_loop_to_the_right_exit_code() -> void {
  // Closes the gap generator support leaves in AOT-specific coverage:
  // src/bytecode_compiler/compile_test.cpp and this file's own
  // codegen_test.cpp sibling exercise `generator def`/`yield`/`.next()`
  // through the bytecode VM and the in-process JIT respectively, but
  // neither runs through a real linked `kira build` binary — a generator's
  // constructor/step-function split is a codegen-shape change AOT-specific
  // linking could plausibly break even when the JIT agrees.
  auto dir = make_temp_dir();
  const auto status = build_and_run(
      dir, "module sample\n"
           "generator def counter(limit: int32) -> some iterator[int32]:\n"
           "    var n = 0\n"
           "    while n < limit:\n"
           "        yield n\n"
           "        n = n + 1\n"
           "def main() -> int32:\n"
           "    let g = counter(5)\n"
           "    var total = 0\n"
           "    while let @some(x) = g.next():\n"
           "        total = total + x\n"
           "    return total\n");
  expect(WIFEXITED(status) != 0, "expected the program to exit normally");
  expect(WEXITSTATUS(status) == 10,
         std::format("expected 0+1+2+3+4 == 10, got {}", WEXITSTATUS(status)));
}

} // namespace

auto main() -> int {
  try {
    test_emits_links_and_runs_to_the_right_exit_code();
    test_unit_returning_main_exits_zero();
    test_panic_reaches_the_process_exit_code();
    test_packed_struct_and_narrow_array_exit_codes();
    test_generator_drives_a_loop_to_the_right_exit_code();
  } catch (const std::exception &ex) {
    std::cerr << "aot_test failed: unhandled exception: " << ex.what() << '\n';
    std::exit(1);
  }
  return 0;
}
