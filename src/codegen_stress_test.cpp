// Cross-checks the two increment-1/3 execution tiers against each other:
// every corpus file's `main()` is compiled and run through both the
// bytecode VM (src/bytecode_compiler + src/bytecode) and the LLVM JIT
// (src/llvm_codegen), and the two results (including which panic, if any)
// must agree exactly. Neither tier is treated as the oracle for the
// other — agreement between two independently written lowering passes
// from the same HIR is itself the thing being tested.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
#include "src/llvm_codegen/codegen.h"
#include "src/llvm_codegen/jit_support.h"
#include "src/parser/diagnostic.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/runtime/layout.h"
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
// answer" needs, including for floating-point results). For a heap-typed
// result, `bits` is instead a real process pointer into one tier's own
// arena — never equal to the other tier's pointer bit-for-bit even when the
// two values are equal, so callers compare those via `values_equal` (below)
// instead of raw `bits` equality.
struct outcome {
  bool panicked = false;
  bc::panic_reason panic{};
  bool has_value = false;
  uint64_t bits = 0;
};

// A heap-typed `outcome.bits` is a real pointer into memory owned by the
// tier's own compiled artifact (the bytecode module's `string_constants`
// table for a `str` literal; a JIT-compiled module's global-string data,
// owned by the `LLJIT` instance, for the LLVM tier's equivalent). Once
// `values_equal` needs to dereference that pointer (for deep equality,
// unlike the old raw-`bits` scalar comparison, which never dereferenced
// anything), whatever owns that memory must outlive the comparison — so
// each side's compiled artifact is returned to `run_one` and kept alive
// there for as long as its `outcome.bits` might still be read, rather than
// destroyed the moment the compiling function returns.
struct bytecode_run {
  bc::bytecode_module module;
  outcome result;
};

auto run_bytecode(const fs::path &path, const hir::hir_module &module,
                  const kira::semantic::type_table &types) -> bytecode_run {
  auto compiled = bcc::compile_module(module, types);
  expect(compiled.has_value(),
         std::format("`{}`: expected bytecode_compiler to accept this "
                     "corpus file: {}",
                     path.string(), compiled.error().message));

  auto index = size_t{0};
  for (; index < compiled->functions.size(); ++index) {
    if (compiled->functions[index].name == "main") {
      break;
    }
  }
  expect(index < compiled->functions.size(),
         std::format("`{}`: expected a `main` function", path.string()));

  const auto vm = bc::vm{*compiled};
  auto run =
      vm.run(static_cast<uint16_t>(index), std::array<bc::slot_value, 0>{});
  auto result = run.has_value()
                    ? outcome{.has_value = run->has_value, .bits = run->value.u}
                    : outcome{.panicked = true, .panic = run.error()};
  return bytecode_run{.module = std::move(*compiled), .result = result};
}

struct llvm_run {
  lc::jit_module jit;
  outcome result;
};

// `as_ptr` selects `jit_module::run_ptr_result` (heap-typed `main`) over
// `jit_module::run` with `return_kind` (scalar/unit `main`) — mirrors
// `run_one`'s own `is_heap_result` classification.
auto run_llvm(const fs::path &path, const hir::hir_module &module,
              const kira::semantic::type_table &types,
              std::optional<bc::numeric_kind> return_kind, bool as_ptr)
    -> llvm_run {
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

  auto run =
      as_ptr ? jit->run_ptr_result("main") : jit->run("main", return_kind);
  auto result = run.has_value()
                    ? outcome{.has_value = run->has_value, .bits = run->value.u}
                    : outcome{.panicked = true, .panic = run.error()};
  return llvm_run{.jit = std::move(*jit), .result = result};
}

// ==========================================================================
//  Structural (deep) equality for heap-typed results.
//
//  A heap-typed `main` result is a real process pointer (both tiers run
//  in-process — the bytecode VM's arena and the JIT's native heap are just
//  two independent allocators in the same address space), so it's always
//  safe to dereference either one directly; the two tiers' pointers
//  themselves are never expected to be equal (different allocators), only
//  the values they point at. `src/runtime/layout.h`'s "every non-scalar
//  value is one pointer, every field/element is one 8-byte slot regardless
//  of whether it itself holds a scalar or another heap pointer" layout means
//  no width/alignment bookkeeping is needed here — only, per `type_id`,
//  how many slots there are and what each one recursively means.
//
//  Deliberately scoped to `str`/`list[T]`/tuple/fixed `array[T, N]` (see
//  `is_deep_comparable`): a struct/sum type's *field* types require the
//  same generic-substitution-aware resolution `semantic::check.cpp`'s
//  private `struct_field_type`/`variant_payload_types` perform, which isn't
//  exposed for reuse outside the checker session that produced them — the
//  same gap `spec/codegen-design.md`'s planning notes already flagged.
//  Extending deep equality to those is future scope, not a soundness
//  shortcut taken here: `is_deep_comparable` fails closed, and `run_one`
//  falls back to today's scalar-only comparison whenever it does.
// ==========================================================================

[[nodiscard]] auto read_slot(uint64_t base, size_t slot_index) -> uint64_t {
  const auto *slots =
      reinterpret_cast<const uint64_t *>(static_cast<uintptr_t>(base));
  return slots[slot_index];
}

// Both tiers' array/list element storage is natural-stride
// (`runtime::layout_of`'s size, generalizing the old uniform-8-byte-slot
// scheme — `bytecode_compiler::function_compiler::element_stride` and
// `llvm_codegen::codegen.cpp`'s identically-named/-shaped helper are the
// production code this mirrors), so `values_equal` below reads both
// `a_bits` (the bytecode VM's result) and `b_bits` (the LLVM JIT's result)
// at the same computed stride via `read_at`, not `read_slot`'s fixed
// 8-byte one.
[[nodiscard]] auto element_stride(const kira::semantic::type_table &types,
                                  kira::semantic::type_id elem_type)
    -> uint8_t {
  const auto layout = kira::runtime::layout_of(types, elem_type);
  if (!layout.has_value() || layout->size_bytes == 0 ||
      layout->size_bytes > 8) {
    return 8;
  }
  return static_cast<uint8_t>(layout->size_bytes);
}

// Zero-extended `width`-byte (1/2/4/8) read at `base + byte_offset` — the
// width-aware sibling `read_slot` needs once bytecode-side elements are no
// longer uniformly 8 bytes apart.
[[nodiscard]] auto read_at(uint64_t base, uint64_t byte_offset, uint8_t width)
    -> uint64_t {
  const auto *data =
      reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(base)) +
      byte_offset;
  switch (width) {
  case 1:
    return *data;
  case 2: {
    uint16_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    return v;
  }
  case 4: {
    uint32_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    return v;
  }
  default: {
    uint64_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    return v;
  }
  }
}

[[nodiscard]] auto is_deep_comparable(const kira::semantic::type_table &types,
                                      kira::semantic::type_id id) -> bool {
  if (bc::numeric_kind_of(types, id).has_value()) {
    return true;
  }
  const auto &entry = types.entry(id);
  switch (entry.kind) {
  case kira::semantic::type_kind::builtin_kind:
    return entry.name == "str";
  case kira::semantic::type_kind::builtin_generic_kind:
    return entry.name == "list" && entry.args.size() == 1 &&
           is_deep_comparable(types, entry.args.front());
  case kira::semantic::type_kind::tuple_kind:
    return std::ranges::all_of(entry.args,
                               [&](kira::semantic::type_id arg) -> bool {
                                 return is_deep_comparable(types, arg);
                               });
  case kira::semantic::type_kind::array_kind:
    return entry.array_size.has_value() &&
           is_deep_comparable(types, entry.result);
  default:
    return false;
  }
}

[[nodiscard]] auto values_equal(const kira::semantic::type_table &types,
                                kira::semantic::type_id id, uint64_t a_bits,
                                uint64_t b_bits) -> bool {
  if (bc::numeric_kind_of(types, id).has_value()) {
    return a_bits == b_bits;
  }
  const auto &entry = types.entry(id);
  switch (entry.kind) {
  case kira::semantic::type_kind::builtin_kind: {
    // `str`: { u64 len; u8* data; } — compare length, then raw bytes.
    const auto len_a = read_slot(a_bits, 0);
    const auto len_b = read_slot(b_bits, 0);
    if (len_a != len_b) {
      return false;
    }
    const auto *data_a = reinterpret_cast<const char *>(
        static_cast<uintptr_t>(read_slot(a_bits, 1)));
    const auto *data_b = reinterpret_cast<const char *>(
        static_cast<uintptr_t>(read_slot(b_bits, 1)));
    return std::memcmp(data_a, data_b, len_a) == 0;
  }
  case kira::semantic::type_kind::builtin_generic_kind: {
    // `list[T]`: { u64 len; u64 cap; T* data; } — compare length, then
    // every element (each one slot, per this file's own top comment).
    const auto len_a = read_slot(a_bits, 0);
    const auto len_b = read_slot(b_bits, 0);
    if (len_a != len_b) {
      return false;
    }
    const auto data_a = read_slot(a_bits, 2);
    const auto data_b = read_slot(b_bits, 2);
    const auto elem_type = entry.args.front();
    const auto stride = element_stride(types, elem_type);
    for (uint64_t i = 0; i < len_a; ++i) {
      if (!values_equal(types, elem_type, read_at(data_a, i * stride, stride),
                        read_at(data_b, i * stride, stride))) {
        return false;
      }
    }
    return true;
  }
  case kira::semantic::type_kind::tuple_kind:
    for (size_t i = 0; i < entry.args.size(); ++i) {
      if (!values_equal(types, entry.args[i], read_slot(a_bits, i),
                        read_slot(b_bits, i))) {
        return false;
      }
    }
    return true;
  case kira::semantic::type_kind::array_kind: {
    const auto count = entry.array_size.value_or(0);
    const auto stride = element_stride(types, entry.result);
    for (uint64_t i = 0; i < count; ++i) {
      if (!values_equal(types, entry.result,
                        read_at(a_bits, i * stride, stride),
                        read_at(b_bits, i * stride, stride))) {
        return false;
      }
    }
    return true;
  }
  default:
    // `is_deep_comparable` must be checked before ever reaching here.
    return a_bits == b_bits;
  }
}

/// The value a corpus file declares `main` should return, written as a
/// `# expect: <integer>` comment anywhere in the file, or `nullopt` if it
/// declares none.
///
/// Cross-tier *agreement* is this harness's main check, and it is a strong
/// one — but it is blind to a bug both tiers share, which is exactly what
/// they do when they share a wrong assumption about the layout they both
/// read from `runtime::layout.h`. A real instance: `&mut xs[2]` on a `list`
/// computed `header + 2 * stride` in both backends, addressing bytes inside
/// the `{len, cap, data}` header instead of the third element — both tiers
/// returned the same wrong answer and agreement alone was happy. Declaring
/// the expected value turns that from a silent pass into a failure.
///
/// Opt-in: a file with no `# expect:` keeps agreement-only checking, so
/// this costs nothing for the cases where the value isn't the point.
[[nodiscard]] auto expected_result_of(std::string_view text)
    -> std::optional<int64_t> {
  constexpr auto marker = std::string_view{"# expect:"};
  const auto at = text.find(marker);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  auto rest = text.substr(at + marker.size());
  rest = rest.substr(0, rest.find('\n'));
  const auto first = rest.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  rest = rest.substr(first);
  auto value = int64_t{0};
  const auto *begin = rest.data();
  const auto result = std::from_chars(begin, begin + rest.size(), value);
  if (result.ec != std::errc{}) {
    return std::nullopt;
  }
  return value;
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

  const auto &types = fixture.checked.types;
  const auto is_unit = types.is_unit(main_fn->return_type);
  const auto return_kind =
      is_unit ? std::nullopt : bc::numeric_kind_of(types, main_fn->return_type);
  const auto is_heap_result = !is_unit && !return_kind.has_value() &&
                              is_deep_comparable(types, main_fn->return_type);
  expect(is_unit || return_kind.has_value() || is_heap_result,
         std::format("`{}`: `main`'s return type has no scalar or "
                     "deep-comparable heap representation",
                     path.string()));

  // Kept alive for the whole function (not just the compile-and-run call
  // above) since a heap-typed result's `bits` may point into memory either
  // one owns — see `bytecode_run`/`llvm_run`'s doc comment.
  auto bc_run = run_bytecode(path, **lowered, fixture.checked.types);
  auto llvm_run_result = run_llvm(path, **lowered, fixture.checked.types,
                                  return_kind, is_heap_result);
  const auto &vm_result = bc_run.result;
  const auto &jit_result = llvm_run_result.result;

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
    const auto agree = is_heap_result
                           ? values_equal(types, main_fn->return_type,
                                          vm_result.bits, jit_result.bits)
                           : vm_result.bits == jit_result.bits;
    expect(agree,
           std::format("`{}`: bytecode VM and LLVM JIT disagree on `main`'s "
                       "result",
                       path.string()));
    if (const auto expected = expected_result_of(text); expected.has_value()) {
      expect(!is_heap_result,
             std::format("`{}`: `# expect:` only applies to a scalar `main` "
                         "result",
                         path.string()));
      expect(vm_result.bits == static_cast<uint64_t>(*expected),
             std::format("`{}`: `main` returned {}, but `# expect:` says {}",
                         path.string(), vm_result.bits, *expected));
    }
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
