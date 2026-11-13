// The codegen stress corpus through the storage form on the bytecode VM: how
// much of it the driver could run once it switches to that form.
//
// Each program is lowered to storage form, verified, compiled, and run, and
// its `main` is compared with the file's `# expect:` value or, without one,
// with the pointer-per-aggregate VM's answer. Files listed in
// `src/testdata/storage_corpus/baseline.txt` must pass; any other file that
// passes is reported so the list can grow. `CINDER_STORAGE_SURVEY=1` prints
// every file's status, and `CINDER_CORPUS_FILTER` runs only matching files.
//
// A file may bound the heap blocks `main` leaves allocated here with
// `# storage-max-live-blocks: <integer>`, apart from the
// `# max-live-blocks:` that `codegen_stress_test` checks on the
// pointer-per-aggregate tiers, whose struct shells are never freed.
//
// A `main` that returns a `str`, a `list[T]`, a tuple, or a fixed array is
// compared as text: the storage form's inline bytes and the legacy VM's
// pointer value are each rendered from their own layout.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "src/bytecode/chunk.h"
#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/bytecode/vm.h"
#include "src/bytecode_compiler/compile.h"
#include "src/bytecode_compiler/storage_compile.h"
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
#include "src/runtime/aligned_buffer.h"
#include "src/runtime/allocator.h"
#include "src/runtime/layout.h"
#include "src/runtime/scheduler.h"
#include "src/runtime/value_layout.h"
#include "src/semantic/analysis.h"
#include "src/semantic/check.h"
#include "src/testing/stdlib_fixtures.h"
#include "src/testing/test_assert.h"
#include "src/testing/test_data.h"

namespace {

namespace fs = std::filesystem;
namespace bc = cinder::bytecode;
namespace hir = cinder::hir;
namespace semantic = cinder::semantic;
using cinder::testing::expect;
using cinder::testing::fail;

auto read_file(const fs::path &path) -> std::string {
  auto in = std::ifstream(path);
  auto text = std::stringstream{};
  text << in.rdbuf();
  return text.str();
}

/// The integer after `marker` at the start of a line, if any.
auto directive_value(std::string_view text, std::string_view marker)
    -> std::optional<int64_t> {
  auto rest = text;
  while (!rest.empty()) {
    const auto end = rest.find('\n');
    const auto line = rest.substr(0, end);
    if (line.starts_with(marker)) {
      try {
        return std::stoll(std::string(line.substr(marker.size())));
      } catch (const std::exception &) {
        return std::nullopt;
      }
    }
    rest = end == std::string_view::npos ? std::string_view{}
                                         : rest.substr(end + 1);
  }
  return std::nullopt;
}

/// What a run of `main` gave: a panic, nothing, or a scalar widened to 64 bits
/// from the width of `main`'s result type.
struct outcome {
  bool panicked = false;
  bc::panic_reason panic{};
  bool has_value = false;
  int64_t value = 0;

  [[nodiscard]] auto operator==(const outcome &) const -> bool = default;
};

auto describe(const outcome &o) -> std::string {
  if (o.panicked) {
    return std::format("panic ({})", bc::panic_reason_message(o.panic));
  }
  return o.has_value ? std::to_string(o.value) : std::string{"()"};
}

/// `bits` as `kind` would read it, widened to 64 bits.
auto widen(bc::numeric_kind kind, uint64_t bits) -> int64_t {
  const auto sign = [](uint64_t v, unsigned width) {
    const auto shift = 64U - width;
    return static_cast<int64_t>(v << shift) >> shift;
  };
  switch (kind) {
  case bc::numeric_kind::i8:
    return sign(bits, 8);
  case bc::numeric_kind::i16:
    return sign(bits, 16);
  case bc::numeric_kind::i32:
    return sign(bits, 32);
  case bc::numeric_kind::u8:
    return static_cast<int64_t>(bits & 0xffU);
  case bc::numeric_kind::u16:
    return static_cast<int64_t>(bits & 0xffffU);
  case bc::numeric_kind::u32:
    return static_cast<int64_t>(bits & 0xffffffffU);
  case bc::numeric_kind::boolean:
    return static_cast<int64_t>(bits & 1U);
  default:
    return static_cast<int64_t>(bits);
  }
}

/// `width` bytes at `at`, zero-extended.
auto load_bits(const std::byte *at, size_t width) -> uint64_t {
  auto value = uint64_t{0};
  std::memcpy(&value, at, std::min<size_t>(width, sizeof(value)));
  return value;
}

auto address_bits(uint64_t bits) -> const std::byte * {
  return reinterpret_cast<const std::byte *>(static_cast<uintptr_t>(bits));
}

auto is_list(const semantic::type_entry &entry) -> bool {
  return (entry.kind == semantic::type_kind::builtin_generic_kind ||
          entry.kind == semantic::type_kind::struct_kind) &&
         entry.name == "list" && entry.args.size() == 1;
}

auto quoted(const std::byte *data, uint64_t length) -> std::string {
  return "\"" +
         std::string(reinterpret_cast<const char *>(data),
                     static_cast<size_t>(length)) +
         "\"";
}

/// `count` elements, each rendered by `one` from its byte offset.
template <typename render_one>
auto render_sequence(uint64_t count, size_t stride, const render_one &one)
    -> std::optional<std::string> {
  auto out = std::string{"["};
  for (uint64_t i = 0; i < count; ++i) {
    const auto element = one(i * stride);
    if (!element.has_value()) {
      return std::nullopt;
    }
    out += (i == 0 ? "" : ", ") + *element;
  }
  return out + "]";
}

struct program {
  cinder::source_manager sources;
  cinder::diagnostic_bag diag{};
  cinder::testing::parsed_stdlib stdlib;
  cinder::ast::ptr<cinder::ast::file> ast_file;
  semantic::checked_types checked;
  hir::ptr_vec<hir::hir_module> modules;
  std::vector<const hir::hir_module *> reached;
  std::unique_ptr<cinder::runtime::value_layouts> layouts;
  std::unique_ptr<hir::storage_env> env;
  const hir::hir_function *main_fn = nullptr;
};

/// Unset when the file does not get as far as HIR; the reason is in `why`.
auto load(const fs::path &path, const std::string &text, std::string &why)
    -> std::unique_ptr<program> {
  auto out = std::make_unique<program>();
  out->stdlib = cinder::testing::parse_stdlib(out->sources, out->diag);
  const auto file_id = out->sources.add_file(path.string(), text);
  expect(file_id.has_value(), "expected the corpus file to register");
  const auto *file = out->sources.get(*file_id);
  auto lexer = cinder::lexer(file->source(), file->id(), out->diag);
  auto tokens = lexer.tokenize();
  auto parser = cinder::parser(std::move(tokens), file->id(), out->diag);
  out->ast_file = parser.parse_file();
  auto file_has_errors =
      std::vector<bool>(static_cast<size_t>(*file_id) + 1, false);
  auto parsed = out->stdlib.modules;
  parsed.push_back(semantic::parsed_module{.file_id = *file_id,
                                           .ast_file = out->ast_file.get()});
  auto comptime = cinder::comptime_vm::vm_backend{};
  out->checked =
      semantic::check_program(parsed, out->diag, file_has_errors, &comptime);
  if (out->diag.error_count() != 0) {
    why = "does not check";
    return nullptr;
  }
  auto name = cinder::testing::module_name_of(*out->ast_file);
  if (name.empty()) {
    name = "sample";
  }
  auto lowered = hir::lower_module(*out->ast_file, name, out->checked);
  if (!lowered.has_value()) {
    why = "HIR: " + lowered.error().message;
    return nullptr;
  }
  auto submodules =
      hir::lower_inline_submodules(*out->ast_file, name, out->checked);
  if (!submodules.has_value()) {
    why = "HIR submodules: " + submodules.error().message;
    return nullptr;
  }
  out->modules.push_back(std::move(*lowered));
  for (auto &submodule : *submodules) {
    out->modules.push_back(std::move(submodule));
  }
  cinder::testing::lower_stdlib_modules(out->stdlib, out->checked, out->modules);
  out->reached = hir::find_reachable_modules(*out->modules.front(), out->modules);
  for (const auto &fn : out->modules.front()->functions) {
    if (fn != nullptr && fn->name == "main") {
      out->main_fn = fn.get();
    }
  }
  if (out->main_fn == nullptr) {
    why = "no `main`";
    return nullptr;
  }
  out->layouts = std::make_unique<cinder::runtime::value_layouts>(
      out->checked.types, out->checked.shapes);
  out->env = std::make_unique<hir::storage_env>(
      hir::storage_env{out->checked, *out->layouts});
  return out;
}

/// A `str`, `list[T]`, tuple, fixed array, or scalar in the inline layout at
/// `at`, as text; none for any other type.
auto render_inline(program &p, semantic::type_id type, const std::byte *at)
    -> std::optional<std::string> {
  const auto &types = p.checked.types;
  const auto id = types.strip_refinement(type);
  const auto layout = p.layouts->layout(id);
  if (!layout.has_value()) {
    return std::nullopt;
  }
  const auto &shape = **layout;
  if (const auto kind = bc::numeric_kind_of(types, id); kind.has_value()) {
    return std::to_string(widen(*kind, load_bits(at, shape.size)));
  }
  const auto &entry = types.entry(id);
  if (is_list(entry)) {
    const auto *length = shape.find_field("len");
    const auto *data = shape.find_field("data");
    const auto element = p.layouts->layout(entry.args.front());
    if (length == nullptr || data == nullptr || !element.has_value()) {
      return std::nullopt;
    }
    const auto align = std::max<size_t>((*element)->align, 1);
    const auto stride = ((*element)->size + align - 1) / align * align;
    const auto *elements = address_bits(load_bits(at + data->offset, 8));
    return render_sequence(
        load_bits(at + length->offset, 8), stride, [&](uint64_t offset) {
          return render_inline(p, entry.args.front(), elements + offset);
        });
  }
  if (entry.kind == semantic::type_kind::builtin_kind && entry.name == "str") {
    return quoted(address_bits(load_bits(at + 8, 8)), load_bits(at, 8));
  }
  if (entry.kind == semantic::type_kind::tuple_kind) {
    auto out = std::string{"("};
    for (size_t i = 0; i < shape.members.size(); ++i) {
      const auto part =
          render_inline(p, shape.members[i].type, at + shape.members[i].offset);
      if (!part.has_value()) {
        return std::nullopt;
      }
      out += (i == 0 ? "" : ", ") + *part;
    }
    return out + ")";
  }
  if (entry.kind == semantic::type_kind::array_kind) {
    return render_sequence(shape.count, shape.stride, [&](uint64_t offset) {
      return render_inline(p, shape.element, at + offset);
    });
  }
  return std::nullopt;
}

/// The byte distance between elements in the pointer-per-aggregate layout.
auto legacy_stride(const semantic::type_table &types, semantic::type_id element)
    -> uint8_t {
  const auto layout = cinder::runtime::layout_of(types, element);
  if (!layout.has_value() || layout->size_bytes == 0 || layout->size_bytes > 8) {
    return 8;
  }
  return static_cast<uint8_t>(layout->size_bytes);
}

/// The same types as `render_inline`, from a pointer-per-aggregate value:
/// `bits` is a scalar, or the address of the value's block.
auto render_legacy(const semantic::type_table &types, semantic::type_id type,
                   uint64_t bits) -> std::optional<std::string> {
  const auto id = types.strip_refinement(type);
  if (const auto kind = bc::numeric_kind_of(types, id); kind.has_value()) {
    return std::to_string(widen(*kind, bits));
  }
  const auto &entry = types.entry(id);
  const auto *block = address_bits(bits);
  if (is_list(entry)) {
    // `{len, cap, data}`, one word each.
    const auto stride = legacy_stride(types, entry.args.front());
    const auto *elements = address_bits(load_bits(block + 16, 8));
    return render_sequence(load_bits(block, 8), stride, [&](uint64_t offset) {
      return render_legacy(types, entry.args.front(),
                           load_bits(elements + offset, stride));
    });
  }
  if (entry.kind == semantic::type_kind::builtin_kind && entry.name == "str") {
    return quoted(address_bits(load_bits(block + 8, 8)), load_bits(block, 8));
  }
  if (entry.kind == semantic::type_kind::tuple_kind) {
    auto out = std::string{"("};
    for (size_t i = 0; i < entry.args.size(); ++i) {
      const auto offset = cinder::runtime::tuple_element_offset(types, id, i);
      if (!offset.has_value()) {
        return std::nullopt;
      }
      const auto part = render_legacy(
          types, entry.args[i],
          load_bits(block + *offset, legacy_stride(types, entry.args[i])));
      if (!part.has_value()) {
        return std::nullopt;
      }
      out += (i == 0 ? "" : ", ") + *part;
    }
    return out + ")";
  }
  if (entry.kind == semantic::type_kind::array_kind &&
      entry.array_size.has_value()) {
    const auto stride = legacy_stride(types, entry.result);
    return render_sequence(*entry.array_size, stride, [&](uint64_t offset) {
      return render_legacy(types, entry.result, load_bits(block + offset, stride));
    });
  }
  return std::nullopt;
}

/// The legacy VM's `main` rendered by `render_legacy`, or the panic.
auto legacy_rendering(const program &p) -> std::optional<std::string> {
  auto compiled = cinder::bytecode_compiler::compile_module(
      std::span<const hir::hir_module *const>(p.reached), p.checked.types);
  if (!compiled.has_value()) {
    return std::nullopt;
  }
  auto index = uint16_t{0};
  while (index < compiled->functions.size() &&
         compiled->functions[index].name != "main") {
    ++index;
  }
  const auto machine = bc::vm{*compiled};
  if (compiled->static_init_function.has_value() &&
      !machine.run(*compiled->static_init_function, std::span<const bc::slot_value>{})
           .has_value()) {
    return std::nullopt;
  }
  const auto run = machine.run(index, std::span<const bc::slot_value>{});
  if (!run.has_value()) {
    return describe(outcome{.panicked = true, .panic = run.error()});
  }
  return render_legacy(p.checked.types, p.main_fn->return_type, run->value.u);
}

/// The pointer-per-aggregate VM's answer, for a file with no `# expect:`.
auto legacy_outcome(const program &p, bc::numeric_kind kind)
    -> std::optional<outcome> {
  auto compiled = cinder::bytecode_compiler::compile_module(
      std::span<const hir::hir_module *const>(p.reached), p.checked.types);
  if (!compiled.has_value()) {
    return std::nullopt;
  }
  auto index = uint16_t{0};
  while (index < compiled->functions.size() &&
         compiled->functions[index].name != "main") {
    ++index;
  }
  const auto machine = bc::vm{*compiled};
  if (compiled->static_init_function.has_value() &&
      !machine.run(*compiled->static_init_function, std::span<const bc::slot_value>{})
           .has_value()) {
    return std::nullopt;
  }
  const auto run = machine.run(index, std::span<const bc::slot_value>{});
  if (!run.has_value()) {
    return outcome{.panicked = true, .panic = run.error()};
  }
  return outcome{.has_value = run->has_value,
                 .value = run->has_value ? widen(kind, run->value.u) : 0};
}

struct verdict {
  bool passed = false;
  std::string status;
};

auto survey(const fs::path &path) -> verdict {
  const auto text = read_file(path);
  auto why = std::string{};
  auto p = load(path, text, why);
  if (p == nullptr) {
    return {.status = why};
  }
  const auto &types = p->checked.types;
  const auto is_unit = types.is_unit(p->main_fn->return_type);
  const auto kind = bc::numeric_kind_of(types, p->main_fn->return_type);
  const auto aggregate = !is_unit && !kind.has_value();
  if (aggregate && !legacy_rendering(*p).has_value()) {
    return {.status = "`main` returns a value this test cannot compare"};
  }

  auto lowerings = std::vector<hir::storage_lowering>{};
  for (const auto *module : p->reached) {
    lowerings.push_back(hir::lower_to_storage(*module, *p->env));
    if (!lowerings.back().refused.empty()) {
      const auto &first = lowerings.back().refused.front();
      return {.status = std::format("refused {}.{}: {}", module->module_name,
                                    first.name, first.error.message)};
    }
  }
  auto modules = std::vector<const hir::storage_module *>{};
  for (const auto &lowering : lowerings) {
    modules.push_back(&lowering.module);
  }
  const auto program_span = std::span<const hir::storage_module *const>(modules);
  for (const auto *module : modules) {
    const auto checked =
        hir::verify(*module, *p->env, hir::verify_options{.program = program_span});
    if (!checked.has_value()) {
      return {.status = std::format("unverified {}: {}", checked.error().function,
                                    checked.error().message)};
    }
  }
  auto compiled = cinder::bytecode_compiler::compile_storage_module(program_span,
                                                                     *p->env);
  if (!compiled.has_value()) {
    return {.status = "VM compile: " + compiled.error().message};
  }
  auto index = uint16_t{0};
  while (index < compiled->functions.size() &&
         compiled->functions[index].name != "main") {
    ++index;
  }
  if (index == compiled->functions.size()) {
    return {.status = "no compiled `main`"};
  }

  const auto live_bound = directive_value(text, "# storage-max-live-blocks:");
  cinder::runtime::scheduler::wait_idle();
  const auto live_before = cinder::runtime::heap_live_block_count();
  const auto hosted = bc::hosted_panic_scope{};
  const auto machine = bc::vm{*compiled};
  for (const auto init : compiled->static_initializers) {
    if (!machine.run(init, std::span<const bc::slot_value>{}).has_value()) {
      return {.status = "static init panicked"};
    }
  }
  if (aggregate) {
    const auto &abi = compiled->functions[index].abi;
    if (!abi.has_value()) {
      return {.status = "compiled `main` has no inline-value signature"};
    }
    auto result = cinder::runtime::aligned_buffer(
        std::max<size_t>(abi->result.size, 8), std::max<size_t>(abi->result.align, 8));
    auto got = std::optional<std::string>{};
    if (abi->returns_through_address) {
      const auto run = machine.run_into(
          index, std::span<std::byte>(result.data(), result.size()),
          std::span<const bc::slot_value>{});
      got = run.has_value()
                ? render_inline(*p, p->main_fn->return_type, result.data())
                : describe(outcome{.panicked = true, .panic = run.error().reason});
    } else {
      const auto run = machine.run(index, std::span<const bc::slot_value>{});
      if (run.has_value()) {
        std::memcpy(result.data(), &run->value.u, sizeof(run->value.u));
        got = render_inline(*p, p->main_fn->return_type, result.data());
      } else {
        got = describe(outcome{.panicked = true, .panic = run.error()});
      }
    }
    const auto want = legacy_rendering(*p);
    if (!got.has_value()) {
      return {.status = "`main`'s inline result cannot be rendered"};
    }
    if (got != want) {
      return {.status = std::format("main gave {}, expected {}", *got,
                                    want.value_or("?"))};
    }
    return {.passed = true, .status = "passed"};
  }
  const auto run = machine.run(index, std::span<const bc::slot_value>{});
  cinder::runtime::scheduler::wait_idle();
  const auto got =
      run.has_value()
          ? outcome{.has_value = run->has_value,
                    .value = run->has_value && kind.has_value()
                                 ? widen(*kind, run->value.u)
                                 : 0}
          : outcome{.panicked = true, .panic = run.error()};

  auto want = std::optional<outcome>{};
  if (const auto expected = directive_value(text, "# expect:");
      expected.has_value()) {
    want = outcome{.has_value = true, .value = *expected};
  } else {
    want = legacy_outcome(*p, kind.value_or(bc::numeric_kind::i64));
    if (!want.has_value()) {
      return {.status = "no reference answer (the legacy VM failed)"};
    }
    if (is_unit) {
      want->has_value = got.has_value;
    }
  }
  if (got != *want) {
    return {.status = std::format("main gave {}, expected {}", describe(got),
                                  describe(*want))};
  }
  if (live_bound.has_value()) {
    const auto live = cinder::runtime::heap_live_block_count() - live_before;
    if (live > *live_bound) {
      return {.status = std::format("main left {} heap blocks allocated, but "
                                    "`# storage-max-live-blocks:` allows {}",
                                    live, *live_bound)};
    }
  }
  return {.passed = true, .status = "passed"};
}

auto read_baseline(const fs::path &file) -> std::set<std::string> {
  auto out = std::set<std::string>{};
  auto in = std::ifstream(file);
  auto line = std::string{};
  while (std::getline(in, line)) {
    if (!line.empty() && !line.starts_with('#')) {
      out.insert(line);
    }
  }
  return out;
}

} // namespace

auto main() -> int {
  const auto corpus = cinder::testing::find_test_data_dir("codegen_stress");
  const auto baseline_file =
      cinder::testing::find_test_data_dir("storage_corpus") / "baseline.txt";
  const auto baseline = read_baseline(baseline_file);
  const auto *filter = std::getenv("CINDER_CORPUS_FILTER");
  const auto *verbose = std::getenv("CINDER_STORAGE_SURVEY");

  auto files = std::vector<fs::path>{};
  for (const auto &entry : fs::directory_iterator(corpus)) {
    if (entry.path().extension() == ".cn") {
      files.push_back(entry.path());
    }
  }
  std::ranges::sort(files);

  auto passed = size_t{0};
  auto regressions = std::vector<std::string>{};
  auto newly = std::vector<std::string>{};
  auto reasons = std::map<std::string, size_t>{};
  for (const auto &file : files) {
    const auto name = file.filename().string();
    if (filter != nullptr && *filter != '\0' && !name.contains(filter)) {
      continue;
    }
    const auto result = survey(file);
    if (verbose != nullptr) {
      std::cout << name << ": " << result.status << "\n";
    }
    if (result.passed) {
      ++passed;
      if (!baseline.contains(name)) {
        newly.push_back(name);
      }
    } else {
      reasons[result.status.substr(0, result.status.find(':'))] += 1;
      if (baseline.contains(name)) {
        regressions.push_back(name + ": " + result.status);
      }
    }
  }
  std::cout << "storage_corpus_test: " << passed << " of " << files.size()
            << " corpus programs ran correctly through the storage form\n";
  for (const auto &[reason, count] : reasons) {
    std::cout << "  " << count << " x " << reason << "\n";
  }
  for (const auto &name : newly) {
    std::cout << "  now passing (add to the baseline): " << name << "\n";
  }
  if (!regressions.empty()) {
    for (const auto &line : regressions) {
      std::cerr << "regressed: " << line << "\n";
    }
    fail("expected every baseline program to run through the storage form");
  }
  return 0;
}
