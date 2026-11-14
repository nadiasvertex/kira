// Runs every codegen stress corpus program through the driver's pipeline on
// both tiers: each is checked (types and ownership), lowered to HIR and to the
// storage form, verified, compiled for the bytecode VM and for the LLVM JIT,
// and run, then lowered again with small calls inlined and run again.
//
// `main`'s answer is checked on both tiers against the file's directive:
// `# expect: <integer>` for a scalar result, `# expect-panic: <reason>` for a
// panic, and `# expect-text: <rendering>` for a `str`, `list[T]`, tuple, or
// fixed array, rendered from its inline bytes. A `main` returning `unit`
// needs none. Agreement between the tiers is not the check: both read the
// same layouts, so they fail together (CLAUDE.md).
//
// `main` must free every heap block it allocates: `# max-live-blocks:
// <integer>` allows a file to leave some, and `# max-allocations: <integer>`
// bounds the blocks it allocates at all, on any thread. Both are checked on
// each tier and counted from after the statics are built.
//
// `CINDER_CORPUS_SURVEY=1` prints every file's status, and
// `CINDER_CORPUS_FILTER` runs only the files whose name contains it.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <print>
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
#include "src/bytecode_compiler/storage_compile.h"
#include "src/comptime_vm/backend.h"
#include "src/hir/inline.h"
#include "src/hir/link.h"
#include "src/hir/lower.h"
#include "src/hir/storage.h"
#include "src/hir/storage_lower.h"
#include "src/hir/storage_verify.h"
#include "src/llvm_codegen/jit_support.h"
#include "src/llvm_codegen/storage_codegen.h"
#include "src/parser/diagnostic.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/runtime/aligned_buffer.h"
#include "src/runtime/allocator.h"
#include "src/semantic/ownership_check.h"
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
namespace llvm_codegen = cinder::llvm_codegen;
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

/// The text after `marker` on the line that starts with it, if any.
auto directive_text(std::string_view text, std::string_view marker)
    -> std::optional<std::string> {
  auto rest = text;
  while (!rest.empty()) {
    const auto end = rest.find('\n');
    const auto line = rest.substr(0, end);
    if (line.starts_with(marker)) {
      auto value = std::string(line.substr(marker.size()));
      while (!value.empty() && value.front() == ' ') {
        value.erase(value.begin());
      }
      while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) {
        value.pop_back();
      }
      return value;
    }
    rest = end == std::string_view::npos ? std::string_view{}
                                         : rest.substr(end + 1);
  }
  return std::nullopt;
}

/// The heap counters just before a tier runs `main`, so the blocks it
/// allocates and the blocks it leaves behind can be bounded on each tier.
struct heap_mark {
  uint64_t allocations = cinder::runtime::heap_allocation_count_all_threads();
  int64_t live = cinder::runtime::heap_live_block_count();
};

/// Checks `# max-live-blocks:` (blocks `main` left allocated) and
/// `# max-allocations:` (blocks it allocated at all, on any thread,
/// so a task's work on a worker counts too) against what
/// happened since `before`, once the scheduler is idle. `tier` prefixes the
/// message.
auto heap_bounds_status(std::string_view text, const heap_mark &before,
                        std::string_view tier) -> std::optional<std::string> {
  cinder::runtime::scheduler::wait_idle();
  const auto live = cinder::runtime::heap_live_block_count() - before.live;
  // Every block `main` allocates must be freed by the time it returns, unless
  // the file says otherwise.
  const auto live_bound =
      directive_value(text, "# max-live-blocks:").value_or(0);
  if (live > live_bound) {
    return std::format("{}main left {} heap blocks allocated, but "
                       "`# max-live-blocks:` allows {}",
                       tier, live, live_bound);
  }
  const auto allocated = static_cast<int64_t>(
      cinder::runtime::heap_allocation_count_all_threads() - before.allocations);
  if (const auto bound = directive_value(text, "# max-allocations:");
      bound.has_value() && allocated > *bound) {
    return std::format("{}main allocated {} heap blocks, but "
                       "`# max-allocations:` allows {}",
                       tier, allocated, *bound);
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

/// Call sites `hir::inline_small_calls` replaced across the inlined passes:
/// checked in `main`, so the inlined pass cannot quietly become a re-run of
/// the plain one.
auto inlined_call_sites = size_t{0};

/// Unset when the file does not get as far as HIR; the reason is in `why`.
/// With `inline_calls`, small calls are inlined first, as the driver does.
auto load(const fs::path &path, const std::string &text, std::string &why,
          bool inline_calls) -> std::unique_ptr<program> {
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
  // The driver holds every program to the ownership rules (ch. 14), and the
  // storage form's drops rely on them, so a corpus program is checked too.
  semantic::check_ownership(parsed, out->checked, out->diag, file_has_errors,
                            std::numeric_limits<unsigned>::max());
  if (out->diag.error_count() != 0) {
    why = "ownership: " + out->diag.diagnostics().front().message;
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
  if (inline_calls) {
    inlined_call_sites +=
        hir::inline_small_calls(out->modules, out->checked.types).call_sites;
  }
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

struct verdict {
  bool passed = false;
  std::string status;
  /// The LLVM tier, tried once the VM tier passes.
  bool llvm_passed = false;
  std::string llvm_status = "not run (the VM tier failed)";
};

/// What `main` must give: a rendering of an aggregate result, or an outcome.
struct reference {
  std::optional<std::string> rendering;
  outcome result;
  bool panics = false;
};

/// The source a span names, when it is in the corpus file.
auto quote_span(const std::string &text, const cinder::source_span &span)
    -> std::string {
  if (span.end > span.start && span.end <= text.size()) {
    return std::format(" (at `{}`)",
                       text.substr(span.start, std::min<size_t>(span.end - span.start, 60)));
  }
  return {};
}

/// The LLVM tier: the same storage form compiled to LLVM and run in the JIT,
/// which runs the static inits as it loads the module. A program whose answer
/// is a panic is compiled but not run, because an explicit panic ends the
/// process in the JIT.
auto llvm_tier(program &p, std::span<const hir::storage_module *const> modules,
               const std::string &text, const reference &want)
    -> std::pair<bool, std::string> {
  auto compiled = llvm_codegen::compile_storage_module(
      modules, *p.env, llvm_codegen::storage_codegen_options{.entry_thunks = true});
  if (!compiled.has_value()) {
    return {false, "LLVM compile: " + compiled.error().message +
                       quote_span(text, compiled.error().span)};
  }
  auto jit = llvm_codegen::jit_module::create(std::move(*compiled));
  if (!jit.has_value()) {
    return {false, "LLVM load: " + jit.error()};
  }
  if (want.panics) {
    return {true, "passed (compiled; a panic is not run in the JIT)"};
  }
  const auto *main_fn = modules.front()->find("main");
  if (main_fn == nullptr) {
    return {false, "no storage `main`"};
  }
  const auto &result_abi = main_fn->signature.result;
  auto result = cinder::runtime::aligned_buffer(std::max<size_t>(result_abi.size, 16),
                                                std::max<size_t>(result_abi.align, 16));
  const auto hosted = bc::hosted_panic_scope{};
  cinder::runtime::scheduler::wait_idle();
  const auto mark = heap_mark{};
  const auto ran = jit->run_storage("main", std::span<const uint64_t>{}, result.data());
  cinder::runtime::scheduler::wait_idle();
  const auto heap = ran.has_value() ? heap_bounds_status(text, mark, "LLVM ")
                                    : std::nullopt;
  if (want.rendering.has_value()) {
    const auto got = ran.has_value()
                         ? render_inline(p, p.main_fn->return_type, result.data())
                         : describe(outcome{.panicked = true, .panic = ran.error()});
    if (got != want.rendering) {
      return {false, std::format("LLVM main gave {}, expected {}", got.value_or("?"),
                                 *want.rendering)};
    }
    if (heap.has_value()) {
      return {false, *heap};
    }
    return {true, "passed"};
  }
  const auto &types = p.checked.types;
  const auto kind = bc::numeric_kind_of(types, p.main_fn->return_type);
  const auto got =
      !ran.has_value()
          ? outcome{.panicked = true, .panic = ran.error()}
          : outcome{.has_value = want.result.has_value,
                    .value = want.result.has_value && kind.has_value()
                                 ? widen(*kind, load_bits(result.data(), result_abi.size))
                                 : 0};
  if (got != want.result) {
    return {false, std::format("LLVM main gave {}, expected {}", describe(got),
                               describe(want.result))};
  }
  if (heap.has_value()) {
    return {false, *heap};
  }
  return {true, "passed"};
}

auto survey(const fs::path &path, bool inline_calls) -> verdict {
  const auto text = read_file(path);
  auto why = std::string{};
  auto p = load(path, text, why, inline_calls);
  if (p == nullptr) {
    return {.status = why};
  }
  const auto &types = p->checked.types;
  const auto is_unit = types.is_unit(p->main_fn->return_type);
  const auto kind = bc::numeric_kind_of(types, p->main_fn->return_type);
  const auto aggregate = !is_unit && !kind.has_value();
  const auto expected_text = directive_text(text, "# expect-text:");
  if (aggregate && !expected_text.has_value()) {
    return {.status = "`main` returns an aggregate, and the file has no "
                      "`# expect-text:`"};
  }

  auto lowerings = std::vector<hir::storage_lowering>{};
  for (const auto *module : p->reached) {
    lowerings.push_back(hir::lower_to_storage(*module, *p->env));
    if (!lowerings.back().refused.empty()) {
      const auto &first = lowerings.back().refused.front();
      // The source the refusal names, when it is in the corpus file.
      auto where = std::string{};
      const auto span = first.error.span;
      if (module == p->reached.front() && span.end > span.start &&
          span.end <= text.size()) {
        where = std::format(" (at `{}`)",
                            text.substr(span.start, std::min<size_t>(
                                                        span.end - span.start, 60)));
      }
      return {.status = std::format("refused {}.{}: {}{}", module->module_name,
                                    first.name, first.error.message, where)};
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
      // `CINDER_STORAGE_DUMP=1` prints the function the verifier rejected.
      if (std::getenv("CINDER_STORAGE_DUMP") != nullptr) {
        for (const auto &function : module->functions) {
          if (function.name == checked.error().function) {
            std::println(stderr, "{}", hir::dump(function, p->checked.types));
          }
        }
      }
      return {.status = std::format("unverified {}: {}", checked.error().function,
                                    checked.error().message)};
    }
  }
  auto compiled = cinder::bytecode_compiler::compile_storage_module(program_span,
                                                                     *p->env);
  if (!compiled.has_value()) {
    // `CINDER_STORAGE_DUMP=1` prints the function the VM compiler rejected.
    if (std::getenv("CINDER_STORAGE_DUMP") != nullptr) {
      for (const auto *module : modules) {
        for (const auto &function : module->functions) {
          if (compiled.error().message.contains(
                  std::format("in `{}.{}`:", module->name, function.name))) {
            std::println(stderr, "{}", hir::dump(function, p->checked.types));
          }
        }
      }
    }
    auto where = std::string{};
    const auto span = compiled.error().span;
    if (span.end > span.start && span.end <= text.size()) {
      where = std::format(" (at `{}`)", text.substr(span.start, std::min<size_t>(
                                                                 span.end - span.start, 60)));
    }
    return {.status = "VM compile: " + compiled.error().message + where};
  }
  auto index = uint16_t{0};
  while (index < compiled->functions.size() &&
         compiled->functions[index].name != "main") {
    ++index;
  }
  if (index == compiled->functions.size()) {
    return {.status = "no compiled `main`"};
  }

  const auto hosted = bc::hosted_panic_scope{};
  const auto machine = bc::vm{*compiled};
  for (const auto init : compiled->static_initializers) {
    if (!machine.run(init, std::span<const bc::slot_value>{}).has_value()) {
      return {.status = "static init panicked"};
    }
  }
  // Statics are the program's for its lifetime, so the bounds count from
  // after they are built.
  cinder::runtime::scheduler::wait_idle();
  if (aggregate) {
    const auto &abi = compiled->functions[index].abi;
    if (!abi.has_value()) {
      return {.status = "compiled `main` has no inline-value signature"};
    }
    auto result = cinder::runtime::aligned_buffer(
        std::max<size_t>(abi->result.size, 8), std::max<size_t>(abi->result.align, 8));
    // After the result buffer, which is the harness's, not `main`'s.
    const auto mark = heap_mark{};
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
    // Blocks the result owns (a `list`, a `str`) are `main`'s and still
    // live, so they need an allowance in the file.
    const auto heap = heap_bounds_status(text, mark, "");
    const auto &want = expected_text;
    if (!got.has_value()) {
      return {.status = "`main`'s inline result cannot be rendered"};
    }
    if (got != want) {
      return {.status = std::format("main gave {}, expected {}", *got,
                                    want.value_or("?"))};
    }
    if (heap.has_value()) {
      return {.status = *heap};
    }
    const auto [llvm_passed, llvm_status] =
        llvm_tier(*p, program_span, text, reference{.rendering = want});
    return {.passed = true,
            .status = "passed",
            .llvm_passed = llvm_passed,
            .llvm_status = llvm_status};
  }
  const auto mark = heap_mark{};
  const auto run = machine.run(index, std::span<const bc::slot_value>{});
  // A panic unwinds without drops, so it is not bounded.
  const auto heap = run.has_value() ? heap_bounds_status(text, mark, "")
                                    : std::optional<std::string>{};
  const auto got =
      run.has_value()
          ? outcome{.has_value = run->has_value,
                    .value = run->has_value && kind.has_value()
                                 ? widen(*kind, run->value.u)
                                 : 0}
          : outcome{.panicked = true, .panic = run.error()};

  // What the file says `main` gives, in `describe`'s words.
  auto want = std::string{};
  if (const auto panic = directive_text(text, "# expect-panic:");
      panic.has_value()) {
    want = std::format("panic ({})", *panic);
  } else if (const auto expected = directive_value(text, "# expect:");
             expected.has_value()) {
    want = std::to_string(*expected);
  } else if (is_unit) {
    want = describe(outcome{});
  } else {
    return {.status = "the file has no `# expect:` or `# expect-panic:`"};
  }
  if (describe(got) != want) {
    return {.status = std::format("main gave {}, expected {}", describe(got),
                                  want)};
  }
  if (heap.has_value()) {
    return {.status = *heap};
  }
  const auto [llvm_passed, llvm_status] = llvm_tier(
      *p, program_span, text, reference{.result = got, .panics = got.panicked});
  return {.passed = true,
          .status = "passed",
          .llvm_passed = llvm_passed,
          .llvm_status = llvm_status};
}

} // namespace

auto main() -> int {
  // Run `io` tasks on several workers so the corpus exercises real threads;
  // a program's result must not depend on how many there are. Read when the
  // first task is spawned.
  setenv("CINDER_IO_WORKERS", "4", /*overwrite=*/0);
  const auto corpus = cinder::testing::find_test_data_dir("codegen_stress");
  const auto *filter = std::getenv("CINDER_CORPUS_FILTER");
  const auto *verbose = std::getenv("CINDER_CORPUS_SURVEY");

  auto files = std::vector<fs::path>{};
  for (const auto &entry : fs::directory_iterator(corpus)) {
    if (entry.path().extension() == ".cn") {
      files.push_back(entry.path());
    }
  }
  std::ranges::sort(files);
  expect(files.size() >= 20,
         std::format("expected a broad codegen stress corpus, found only {} "
                     "files",
                     files.size()));

  auto ran = size_t{0};
  auto failures = std::vector<std::string>{};
  for (const auto &file : files) {
    const auto name = file.filename().string();
    if (filter != nullptr && *filter != '\0' && !name.contains(filter)) {
      continue;
    }
    ++ran;
    // Each program runs as lowered and again with small calls inlined, as
    // the driver runs it: both must pass on both tiers.
    auto result = survey(file, false);
    if (result.passed) {
      const auto inlined = survey(file, true);
      if (!inlined.passed) {
        result.passed = false;
        result.status = "inlined: " + inlined.status;
      } else if (result.llvm_passed && !inlined.llvm_passed) {
        result.llvm_passed = false;
        result.llvm_status = "inlined: " + inlined.llvm_status;
      }
    }
    if (verbose != nullptr) {
      std::cout << name << ": " << result.status << "\n";
      if (result.passed) {
        std::cout << name << " [llvm]: " << result.llvm_status << "\n";
      }
    }
    if (!result.passed) {
      failures.push_back(name + ": " + result.status);
    } else if (!result.llvm_passed) {
      failures.push_back(name + " [llvm]: " + result.llvm_status);
    }
  }
  if (!failures.empty()) {
    for (const auto &line : failures) {
      std::cerr << "failed: " << line << "\n";
    }
    fail(std::format("expected every corpus program to run correctly on both "
                     "tiers; {} of {} did not",
                     failures.size(), ran));
  }
  if (filter == nullptr || *filter == '\0') {
    expect(inlined_call_sites >= 100,
           std::format("expected the inlined pass to inline a real share of "
                       "the corpus's calls, but it inlined only {}",
                       inlined_call_sites));
  }
  std::cout << "codegen_stress_test: " << ran
            << " program(s) ran correctly on the bytecode VM and the LLVM JIT, "
               "with and without inlining ("
            << inlined_call_sites << " call sites inlined)\n";
  return 0;
}
