#include "src/llvm_codegen/codegen.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wshadow"
#include <llvm/ADT/APInt.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>
#pragma clang diagnostic pop

#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/hir/captures.h"
#include "src/hir/ids.h"
#include "src/hir/live_across_yield.h"
#include "src/intrinsics.h"
#include "src/parser/ast.h"
#include "src/parser/text_escape.h"
#include "src/parser/token.h"
#include "src/runtime/layout.h"

namespace kira::llvm_codegen {

namespace {

using bytecode::bit_width;
using bytecode::is_float;
using bytecode::is_signed_integer;
using bytecode::numeric_kind;
using bytecode::numeric_kind_of;
using bytecode::panic_reason;
using hir::hir_node;
using hir::hir_node_kind;
using semantic::type_id;
using semantic::type_table;

// ==========================================================================
//  Literal decoding — duplicated from src/bytecode_compiler/compile.cpp
//  rather than shared: both are small, self-contained, and this mirrors
//  that file's own precedent of not exporting check.cpp's literal parser
//  across an unrelated module boundary for one caller (see its top comment).
// ==========================================================================

[[nodiscard]] auto parse_uint_literal(std::string_view text)
    -> std::optional<uint64_t> {
  auto cleaned = std::string{};
  cleaned.reserve(text.size());
  for (const auto ch : text) {
    if (ch != '_') {
      cleaned.push_back(ch);
    }
  }
  auto base = 10;
  auto digits = std::string_view(cleaned);
  if (digits.size() > 2 && digits[0] == '0') {
    if (digits[1] == 'x' || digits[1] == 'X') {
      base = 16;
      digits.remove_prefix(2);
    } else if (digits[1] == 'o' || digits[1] == 'O') {
      base = 8;
      digits.remove_prefix(2);
    } else if (digits[1] == 'b' || digits[1] == 'B') {
      base = 2;
      digits.remove_prefix(2);
    }
  }
  auto value = uint64_t{0};
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
  const char *digits_end = digits.data() + digits.size();
  const auto result = std::from_chars(digits.data(), digits_end, value, base);
  if (result.ec != std::errc{} || result.ptr != digits_end) {
    return std::nullopt;
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
  return value;
}

[[nodiscard]] auto parse_float_literal(std::string_view text)
    -> std::optional<double> {
  auto cleaned = std::string{};
  cleaned.reserve(text.size());
  for (const auto ch : text) {
    if (ch != '_') {
      cleaned.push_back(ch);
    }
  }
  auto value = double{0};
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const char *end = cleaned.data() + cleaned.size();
  const auto result = std::from_chars(cleaned.data(), end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return value;
}

[[nodiscard]] auto decode_utf8_scalar(std::string_view text, size_t &pos)
    -> std::optional<uint32_t> {
  if (pos >= text.size()) {
    return std::nullopt;
  }
  const auto first = static_cast<unsigned char>(text[pos]);
  if ((first & 0x80) == 0) {
    ++pos;
    return static_cast<uint32_t>(first);
  }
  auto extra_bytes = 0;
  auto value = uint32_t{0};
  if ((first & 0xE0) == 0xC0) {
    extra_bytes = 1;
    value = static_cast<uint32_t>(first & 0x1F);
  } else if ((first & 0xF0) == 0xE0) {
    extra_bytes = 2;
    value = static_cast<uint32_t>(first & 0x0F);
  } else if ((first & 0xF8) == 0xF0) {
    extra_bytes = 3;
    value = static_cast<uint32_t>(first & 0x07);
  } else {
    return std::nullopt;
  }
  ++pos;
  for (auto i = 0; i < extra_bytes; ++i) {
    if (pos >= text.size()) {
      return std::nullopt;
    }
    const auto cont = static_cast<unsigned char>(text[pos]);
    value = (value << 6) | static_cast<uint32_t>(cont & 0x3F);
    ++pos;
  }
  return value;
}

[[nodiscard]] auto decode_char_literal(std::string_view text)
    -> std::optional<uint32_t> {
  if (text.size() < 2 || text.front() != '\'' || text.back() != '\'') {
    return std::nullopt;
  }
  const auto inner = text.substr(1, text.size() - 2);
  if (inner.empty()) {
    return std::nullopt;
  }
  if (inner.front() != '\\') {
    auto pos = size_t{0};
    return decode_utf8_scalar(inner, pos);
  }
  if (inner.size() < 2) {
    return std::nullopt;
  }
  switch (inner[1]) {
  case 'n':
    return static_cast<uint32_t>('\n');
  case 't':
    return static_cast<uint32_t>('\t');
  case 'r':
    return static_cast<uint32_t>('\r');
  case '"':
    return static_cast<uint32_t>('"');
  case '\'':
    return static_cast<uint32_t>('\'');
  case '\\':
    return static_cast<uint32_t>('\\');
  case '{':
    return static_cast<uint32_t>('{');
  case '}':
    return static_cast<uint32_t>('}');
  case 'u': {
    if (inner.size() < 5 || inner[2] != '{' || inner.back() != '}') {
      return std::nullopt;
    }
    const auto hex = inner.substr(3, inner.size() - 4);
    auto value = uint32_t{0};
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
    const char *hex_end = hex.data() + hex.size();
    const auto result = std::from_chars(hex.data(), hex_end, value, 16);
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
    if (result.ec != std::errc{} || result.ptr != hex_end) {
      return std::nullopt;
    }
    return value;
  }
  default:
    return std::nullopt;
  }
}

// `decode_string_literal` used to be duplicated here (same reasoning as
// `decode_char_literal`/`encode_utf8_scalar` above) — now reuses the shared
// `kira::decode_string_literal` (`src/parser/text_escape.h`), the same way
// `bytecode_compiler/compile.cpp` already does, so the doubled-brace
// (`{{`/`}}`) literal-escape handling `spec/string-formatting-design.md`
// requires only has to exist in one place.
using kira::decode_string_literal;

/// Whether `id` is one of the heap-backed representations
/// `src/runtime/layout.h` describes (`str`, `list`/`option`/`result` and
/// other prelude generics, tuple, fixed array, struct, sum type, and now
/// `fn(...)` — a lambda/closure value, per increment 6) — every such value
/// is a single opaque pointer, as opposed to the closed scalar set
/// `numeric_kind_of` maps directly to an LLVM integer/float type.
[[nodiscard]] auto is_heap_type(const type_table &types, type_id id) -> bool {
  const auto &entry = types.entry(id);
  switch (entry.kind) {
  case semantic::type_kind::tuple_kind:
  case semantic::type_kind::array_kind:
  case semantic::type_kind::struct_kind:
  case semantic::type_kind::sum_kind:
  case semantic::type_kind::builtin_generic_kind:
  case semantic::type_kind::fn_kind:
    return true;
  case semantic::type_kind::builtin_kind:
    return entry.name == "str";
  default:
    return false;
  }
}

// ==========================================================================
//  numeric_kind <-> llvm::Type.
// ==========================================================================

[[nodiscard]] auto llvm_type_for(llvm::LLVMContext &ctx, numeric_kind kind)
    -> llvm::Type * {
  switch (kind) {
  case numeric_kind::boolean:
    return llvm::Type::getInt1Ty(ctx);
  case numeric_kind::f32:
    return llvm::Type::getFloatTy(ctx);
  case numeric_kind::f64:
    return llvm::Type::getDoubleTy(ctx);
  default:
    // Every remaining kind (i8/i16/i32/i64/u8/u16/u32/u64/character) is a
    // plain integer at its own bit width — `bit_width` already special-
    // cases `character` to 32 (widened Unicode scalar) the same way
    // `bytecode::slot_value` does.
    return llvm::Type::getIntNTy(ctx, static_cast<unsigned>(bit_width(kind)));
  }
}

/// Unwraps `&T`/`&mut T` down to `T` — mirrors `semantic::check.cpp`'s own
/// `strip_refs`. A parameter/local/field declared `&T` is represented
/// identically to a bare `T` for every `is_heap_type` kind (the same
/// passthrough `compile_unary`'s `addr_of`/`addr_of_mut` relies on: the
/// referent's own heap pointer already *is* the address a reference to it
/// would hold), so `storage_llvm_type` needs to see through the wrapper to
/// find that representation — neither `numeric_kind_of` nor `is_heap_type`
/// otherwise recognize `ref_kind` at all.
[[nodiscard]] auto strip_refs(const type_table &types, type_id id) -> type_id {
  const auto *entry = &types.entry(id);
  while (entry->kind == semantic::type_kind::ref_kind) {
    id = entry->result;
    entry = &types.entry(id);
  }
  return id;
}

/// Maps `id` to the LLVM type a value of that type is stored/passed as: the
/// scalar `numeric_kind` mapping for scalars, an opaque `ptr` for anything
/// `is_heap_type` recognizes, or `nullopt` for the (currently) genuinely
/// unrepresentable remainder (128-bit scalars). Shared by `compile_module`'s
/// function-signature builder and `function_compiler::storage_type_for` so
/// parameter/return/local storage typing is computed the same way in both
/// places.
[[nodiscard]] auto storage_llvm_type(const type_table &types,
                                     llvm::LLVMContext &ctx, type_id id)
    -> std::optional<llvm::Type *> {
  const auto stripped = strip_refs(types, id);
  const auto kind = numeric_kind_of(types, stripped);
  if (kind.has_value()) {
    return llvm_type_for(ctx, *kind);
  }
  if (is_heap_type(types, stripped)) {
    return llvm::PointerType::get(ctx, 0);
  }
  if (types.is_unit(stripped)) {
    // `unit` has exactly one value and carries no information — a
    // function's own `unit` *return* is already handled specially
    // (`return_is_unit_` emits `void`/`CreateRetVoid` instead of ever
    // reaching here), but `unit` can still appear as a parameter, local, or
    // intermediate expression type (e.g. `std.console.print`'s own
    // `-> unit` body discarding `write_all`'s `result[unit, io_error]`) —
    // an i64 zero placeholder, mirroring `bytecode_compiler::compile.cpp`'s
    // identical treatment of the `unit` literal, is enough since nothing
    // ever reads it back out.
    return llvm::Type::getInt64Ty(ctx);
  }
  return std::nullopt;
}

// ==========================================================================
//  Generator support helpers — counting yield points, building a
//  generator's state-block symbol layout, and recovering each
//  non-parameter state symbol's declared type (needed here, unlike the
//  bytecode tier, because LLVM allocas/loads are statically typed — a
//  slot's `llvm::Type*` has to be known up front, not just its byte
//  width). See `function_compiler::compile_generator_step`.
// ==========================================================================

[[nodiscard]] auto count_yields(const hir::hir_block &block) -> size_t;

[[nodiscard]] auto count_yields(const hir_node &node) -> size_t {
  switch (node.kind) {
  case hir_node_kind::hir_yield:
    return 1;
  case hir_node_kind::hir_let_else:
    return count_yields(
        *dynamic_cast<const hir::hir_let_else &>(node).else_body);
  case hir_node_kind::hir_while:
    return count_yields(*dynamic_cast<const hir::hir_while &>(node).body);
  case hir_node_kind::hir_while_let:
    return count_yields(*dynamic_cast<const hir::hir_while_let &>(node).body);
  case hir_node_kind::hir_if: {
    const auto &node2 = dynamic_cast<const hir::hir_if &>(node);
    size_t total = 0;
    for (const auto &branch : node2.branches) {
      total += count_yields(*branch.body);
    }
    if (node2.else_body != nullptr) {
      total += count_yields(*node2.else_body);
    }
    return total;
  }
  case hir_node_kind::hir_match: {
    const auto &node2 = dynamic_cast<const hir::hir_match &>(node);
    size_t total = 0;
    for (const auto &arm : node2.arms) {
      total += count_yields(*arm.body);
    }
    return total;
  }
  case hir_node_kind::hir_block:
    return count_yields(dynamic_cast<const hir::hir_block &>(node));
  default:
    return 0;
  }
}

auto count_yields(const hir::hir_block &block) -> size_t {
  size_t total = 0;
  for (const auto &stmt : block.stmts) {
    total += count_yields(*stmt);
  }
  return total;
}

/// The state-block layout for a `generator def`'s constructor/step
/// functions — see `bytecode_compiler::compile.cpp`'s identically-shaped
/// `generator_state_symbols`: every parameter, in declaration order,
/// followed by every non-parameter local `hir::live_across_yield` found
/// live, in the order it returns them.
[[nodiscard]] auto generator_state_symbols(const hir::hir_function &fn)
    -> std::vector<hir::symbol_id> {
  auto symbols = std::vector<hir::symbol_id>{};
  symbols.reserve(fn.params.size());
  for (const auto &param : fn.params) {
    symbols.push_back(param.symbol);
  }
  for (const auto sym : hir::live_across_yield(fn)) {
    if (std::ranges::find(symbols, sym) == symbols.end()) {
      symbols.push_back(sym);
    }
  }
  return symbols;
}

/// Recovers the declared type of every `hir_let`-bound symbol reachable
/// from `block` (not descending into a nested lambda's own body, mirroring
/// `hir::live_across_yield`'s own traversal boundary) — a generator's
/// non-parameter state-block symbols are always bound this way (per
/// `hir_pattern`'s doc comment, every surface pattern binding desugars to
/// an ordinary `hir_let`), so this is enough to type every state slot
/// `compile_generator_step` needs to load at entry.
auto collect_let_types(const hir::hir_block &block,
                       std::unordered_map<hir::symbol_id, type_id> &out)
    -> void;

auto collect_let_types(const hir_node &node,
                       std::unordered_map<hir::symbol_id, type_id> &out)
    -> void {
  switch (node.kind) {
  case hir_node_kind::hir_let: {
    const auto &let = dynamic_cast<const hir::hir_let &>(node);
    out.emplace(let.symbol, let.initializer->type);
    return;
  }
  case hir_node_kind::hir_let_else:
    collect_let_types(*dynamic_cast<const hir::hir_let_else &>(node).else_body,
                      out);
    return;
  case hir_node_kind::hir_while:
    collect_let_types(*dynamic_cast<const hir::hir_while &>(node).body, out);
    return;
  case hir_node_kind::hir_while_let:
    collect_let_types(*dynamic_cast<const hir::hir_while_let &>(node).body,
                      out);
    return;
  case hir_node_kind::hir_if: {
    const auto &node2 = dynamic_cast<const hir::hir_if &>(node);
    for (const auto &branch : node2.branches) {
      collect_let_types(*branch.body, out);
    }
    if (node2.else_body != nullptr) {
      collect_let_types(*node2.else_body, out);
    }
    return;
  }
  case hir_node_kind::hir_match: {
    const auto &node2 = dynamic_cast<const hir::hir_match &>(node);
    for (const auto &arm : node2.arms) {
      collect_let_types(*arm.body, out);
    }
    return;
  }
  case hir_node_kind::hir_block:
    collect_let_types(dynamic_cast<const hir::hir_block &>(node), out);
    return;
  default:
    return;
  }
}

auto collect_let_types(const hir::hir_block &block,
                       std::unordered_map<hir::symbol_id, type_id> &out)
    -> void {
  for (const auto &stmt : block.stmts) {
    collect_let_types(*stmt, out);
  }
}

// ==========================================================================
//  module_compiler — declares every function's signature up front (so
//  direct/recursive/mutually-recursive calls always find a real
//  llvm::Function*), then compiles each body.
// ==========================================================================

class function_compiler {
public:
  /// `entry_module_name`/`current_module_name` default to matching (empty)
  /// strings when omitted, so every existing single-module `compile_module`
  /// call site keeps treating every call as same-module — see
  /// `resolve_callee_key`, and its identical counterpart in
  /// `bytecode_compiler::function_compiler`.
  function_compiler(
      llvm::LLVMContext &ctx, const type_table &types,
      const std::unordered_map<std::string, llvm::Function *> &functions,
      llvm::Function *panic_fn, llvm::Function *alloc_fn,
      llvm::Function *list_reserve_slot_fn,
      const std::array<llvm::Function *, 23> &intrinsic_fns,
      std::string entry_module_name = {}, std::string current_module_name = {})
      : ctx_(ctx), types_(types), functions_(functions), panic_fn_(panic_fn),
        alloc_fn_(alloc_fn), list_reserve_slot_fn_(list_reserve_slot_fn),
        intrinsic_fns_(intrinsic_fns),
        entry_module_name_(std::move(entry_module_name)),
        current_module_name_(std::move(current_module_name)), builder_(ctx) {}

  [[nodiscard]] auto compile(const hir::hir_function &fn,
                             llvm::Function *llvm_fn)
      -> std::expected<void, codegen_error> {
    current_fn_ = llvm_fn;
    return_is_unit_ = types_.is_unit(fn.return_type);
    if (!return_is_unit_) {
      auto ty = storage_type_for(fn.return_type, fn.span);
      if (!ty.has_value()) {
        return std::unexpected(ty.error());
      }
      return_llvm_type_ = *ty;
    }

    auto *entry = llvm::BasicBlock::Create(ctx_, "entry", llvm_fn);
    builder_.SetInsertPoint(entry);
    // Every local (parameter or `let`) gets an alloca inserted here, at the
    // very front of the entry block, regardless of where in the function
    // body it's lexically declared — the usual "alloca marker" trick (also
    // how unoptimized Clang output is shaped): an alloca that executes
    // repeatedly (e.g. one lexically inside a `while` body) must not be
    // emitted at that repeated program point, or it dynamically grows the
    // stack once per loop iteration instead of naming one fixed frame slot.
    alloca_marker_ = builder_.CreateAlloca(llvm::Type::getInt1Ty(ctx_), nullptr,
                                           "alloca.marker");

    for (size_t i = 0; i < fn.params.size(); ++i) {
      const auto &param = fn.params[i];
      auto ty = storage_type_for(param.type, fn.span);
      if (!ty.has_value()) {
        return std::unexpected(ty.error());
      }
      auto *alloca = create_local_alloca(*ty, param.name);
      builder_.CreateStore(llvm_fn->getArg(static_cast<unsigned>(i)), alloca);
      locals_.emplace(param.symbol, alloca);
    }

    if (fn.is_generator) {
      return compile_generator_constructor(fn);
    }
    return compile_body_and_finish(*fn.body);
  }

  /// Compiles a `generator def`'s declared name into a small *constructor*:
  /// builds the generator's state block (populated with the current
  /// parameter values — every other state slot starts zeroed, courtesy of
  /// `kira_rt_alloc`'s zero-filled arena allocation, populated later by the
  /// step function the first time it becomes live), synthesizes and
  /// compiles the step function (an internal-linkage `llvm::Function`
  /// reachable only through the generator object's own slot, never called
  /// by name), and returns a 4-slot generator-object handle
  /// `{ step_fn; state_ptr; resume_index=0; finished=0 }`. Mirrors
  /// `bytecode_compiler::compile_generator_constructor`/`op_make_generator`
  /// exactly. Requires `compile`'s param-alloca prelude to have already run
  /// (this is only ever called from inside `compile`).
  [[nodiscard]] auto compile_generator_constructor(const hir::hir_function &fn)
      -> std::expected<void, codegen_error> {
    const auto state_symbols = generator_state_symbols(fn);
    auto *ptr_ty = llvm::PointerType::get(ctx_, 0);

    llvm::Value *state_block = nullptr;
    if (state_symbols.empty()) {
      state_block = llvm::ConstantPointerNull::get(ptr_ty);
    } else {
      state_block = compile_heap_alloc(state_symbols.size());
      for (size_t i = 0; i < fn.params.size(); ++i) {
        auto *alloca = lookup_local(state_symbols[i]);
        auto *value = builder_.CreateLoad(alloca->getAllocatedType(), alloca,
                                          "state.init");
        builder_.CreateStore(value, slot_address(state_block, i));
      }
    }

    auto *step_fn_type = llvm::FunctionType::get(
        ptr_ty, {ptr_ty, llvm::Type::getInt64Ty(ctx_), ptr_ty},
        /*isVarArg=*/false);
    auto *step_fn = llvm::Function::Create(
        step_fn_type, llvm::Function::InternalLinkage,
        std::format("generator.step.{}", reinterpret_cast<uintptr_t>(&fn)),
        current_fn_->getParent());

    auto nested = function_compiler(
        ctx_, types_, functions_, panic_fn_, alloc_fn_, list_reserve_slot_fn_,
        intrinsic_fns_, entry_module_name_, current_module_name_);
    auto step_compiled =
        nested.compile_generator_step(fn, state_symbols, step_fn);
    if (!step_compiled.has_value()) {
      return std::unexpected(step_compiled.error());
    }

    auto *handle = compile_heap_alloc(4);
    builder_.CreateStore(step_fn, slot_address(handle, size_t{0}));
    builder_.CreateStore(state_block, slot_address(handle, size_t{1}));
    builder_.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0),
        slot_address(handle, size_t{2}));
    builder_.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0),
        slot_address(handle, size_t{3}));
    builder_.CreateRet(handle);

    alloca_marker_->eraseFromParent();
    return {};
  }

  /// Compiles a generator's actual body into its step function
  /// (`llvm_fn`, already declared by `compile_generator_constructor` with
  /// signature `[ptr state, i64 resume_index, ptr generator_self] -> ptr`).
  /// Every `state_symbols` entry is loaded out of the state block into a
  /// fresh local alloca up front — exactly like a closure loads its
  /// captures — typed via `fn.params[i].type` for parameters or
  /// `collect_let_types`'s scan for every other live local. A leading
  /// resume-dispatch chain (`resume_index == k` ⇒ branch to the block right
  /// after the `k`th `hir_yield`) makes `resume_index == 0` fall through to
  /// start the body from the top, mirroring
  /// `bytecode_compiler::compile_generator_step` exactly, just built from
  /// real `llvm::BasicBlock`s instead of a bytecode jump-patch table.
  [[nodiscard]] auto
  compile_generator_step(const hir::hir_function &fn,
                         const std::vector<hir::symbol_id> &state_symbols,
                         llvm::Function *llvm_fn)
      -> std::expected<void, codegen_error> {
    current_fn_ = llvm_fn;
    return_is_unit_ = false;
    return_llvm_type_ = llvm::PointerType::get(ctx_, 0);

    auto *entry = llvm::BasicBlock::Create(ctx_, "entry", llvm_fn);
    builder_.SetInsertPoint(entry);
    alloca_marker_ = builder_.CreateAlloca(llvm::Type::getInt1Ty(ctx_), nullptr,
                                           "alloca.marker");

    auto *state_arg = llvm_fn->getArg(0);
    auto *resume_arg = llvm_fn->getArg(1);
    auto *self_arg = llvm_fn->getArg(2);
    is_generator_step_ = true;
    generator_state_arg_ = state_arg;
    generator_self_arg_ = self_arg;
    generator_state_layout_ = state_symbols;

    auto let_types = std::unordered_map<hir::symbol_id, type_id>{};
    collect_let_types(*fn.body, let_types);

    for (size_t i = 0; i < state_symbols.size(); ++i) {
      auto sym_type = hir::k_unknown_type;
      if (i < fn.params.size()) {
        sym_type = fn.params[i].type;
      } else {
        const auto found = let_types.find(state_symbols[i]);
        if (found == let_types.end()) {
          return std::unexpected(codegen_error{
              .kind = codegen_error_kind::unsupported_construct,
              .span = fn.span,
              .message = "a generator's live-across-yield local could not "
                         "be typed — capture analysis and codegen have "
                         "gotten out of sync"});
        }
        sym_type = found->second;
      }
      auto ty = storage_type_for(sym_type, fn.span);
      if (!ty.has_value()) {
        return std::unexpected(ty.error());
      }
      auto *alloca = create_local_alloca(*ty, "state");
      auto *loaded =
          builder_.CreateLoad(*ty, slot_address(state_arg, i), "state");
      builder_.CreateStore(loaded, alloca);
      locals_.emplace(state_symbols[i], alloca);
    }

    const auto yield_total = count_yields(*fn.body);
    generator_resume_blocks_.clear();
    generator_resume_blocks_.reserve(yield_total);
    auto *body_start = llvm::BasicBlock::Create(ctx_, "resume.0", llvm_fn);
    for (size_t k = 1; k <= yield_total; ++k) {
      generator_resume_blocks_.push_back(
          llvm::BasicBlock::Create(ctx_, std::format("resume.{}", k), llvm_fn));
    }
    for (size_t k = 1; k <= yield_total; ++k) {
      auto *cmp = builder_.CreateICmpEQ(
          resume_arg, llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), k),
          "resume.check");
      auto *next_check = llvm::BasicBlock::Create(
          ctx_, std::format("resume.check.{}", k + 1), llvm_fn);
      builder_.CreateCondBr(cmp, generator_resume_blocks_[k - 1], next_check);
      builder_.SetInsertPoint(next_check);
    }
    builder_.CreateBr(body_start);
    builder_.SetInsertPoint(body_start);

    generator_next_yield_ordinal_ = 1;

    auto terminated = false;
    for (const auto &stmt : fn.body->stmts) {
      auto result = compile_stmt(*stmt);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      terminated = *result;
      if (terminated) {
        break;
      }
    }
    if (!terminated) {
      auto result = emit_generator_exhausted_return(fn.span);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
    }

    // A resume block reserved for a `yield` that turned out to be
    // unreachable (dead code after a bare `return`, an exhaustive `if`/
    // `match`, etc.) never gets `SetInsertPoint`-ed into by the body
    // compile above, and so is left with no terminator — the LLVM verifier
    // rejects that regardless of the block's real unreachability. Its only
    // predecessor is the dispatch chain's own comparison against an
    // ordinal no `hir_yield` ever actually stores into a generator object
    // (since the yield that would have produced it never compiled), so the
    // edge is real but provably never taken at runtime; `unreachable` is
    // the correct terminator for it.
    for (auto *block : generator_resume_blocks_) {
      if (block->getTerminator() == nullptr) {
        builder_.SetInsertPoint(block);
        builder_.CreateUnreachable();
      }
    }

    alloca_marker_->eraseFromParent();
    return {};
  }

  /// Compiles a lambda's body into a freshly-created `llvm::Function`
  /// (`llvm_fn`, already declared by `compile_lambda_value` with the
  /// closure calling convention's signature: env `ptr` first, then the
  /// lambda's own declared params), loading each entry in `free_vars` back
  /// out of the env argument's slots (`capture_types[i]` is the exact LLVM
  /// type `compile_lambda_value` stored at slot `i`, computed from the
  /// captured variable's own alloca in the *enclosing* function) before the
  /// body runs — mirrors `compile`, just with a different argument-to-local
  /// binding prelude.
  [[nodiscard]] auto
  compile_lambda_body(const hir::hir_lambda &lambda,
                      const std::vector<hir::symbol_id> &free_vars,
                      const std::vector<llvm::Type *> &capture_types,
                      llvm::Function *llvm_fn)
      -> std::expected<void, codegen_error> {
    current_fn_ = llvm_fn;
    return_is_unit_ = types_.is_unit(lambda.return_type);
    if (!return_is_unit_) {
      auto ty = storage_type_for(lambda.return_type, lambda.span);
      if (!ty.has_value()) {
        return std::unexpected(ty.error());
      }
      return_llvm_type_ = *ty;
    }

    auto *entry = llvm::BasicBlock::Create(ctx_, "entry", llvm_fn);
    builder_.SetInsertPoint(entry);
    alloca_marker_ = builder_.CreateAlloca(llvm::Type::getInt1Ty(ctx_), nullptr,
                                           "alloca.marker");

    auto *env_arg = llvm_fn->getArg(0);
    for (size_t i = 0; i < lambda.params.size(); ++i) {
      const auto &param = lambda.params[i];
      auto ty = storage_type_for(param.type, lambda.span);
      if (!ty.has_value()) {
        return std::unexpected(ty.error());
      }
      auto *alloca = create_local_alloca(*ty, param.name);
      builder_.CreateStore(llvm_fn->getArg(static_cast<unsigned>(i + 1)),
                           alloca);
      locals_.emplace(param.symbol, alloca);
    }
    for (size_t i = 0; i < free_vars.size(); ++i) {
      auto *alloca = create_local_alloca(capture_types[i], "capture");
      auto *loaded = builder_.CreateLoad(capture_types[i],
                                         slot_address(env_arg, i), "capture");
      builder_.CreateStore(loaded, alloca);
      locals_.emplace(free_vars[i], alloca);
    }

    return compile_body_and_finish(*lambda.body);
  }

private:
  [[nodiscard]] auto compile_body_and_finish(const hir::hir_block &body)
      -> std::expected<void, codegen_error> {
    const auto &stmts = body.stmts;
    auto terminated = false;
    for (size_t i = 0; i + 1 < stmts.size() && !terminated; ++i) {
      auto result = compile_stmt(*stmts[i]);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      terminated = *result;
    }

    if (!terminated) {
      if (stmts.empty()) {
        builder_.CreateRetVoid();
      } else {
        const auto &last = *stmts.back();
        if (last.kind == hir_node_kind::hir_expr_stmt) {
          const auto &expr_stmt =
              dynamic_cast<const hir::hir_expr_stmt &>(last);
          auto value = compile_expr(*expr_stmt.expr);
          if (!value.has_value()) {
            return std::unexpected(value.error());
          }
          // A tail `if`/`match`/block whose every branch/arm diverges (e.g.
          // via `return`) already ends the current block with its own
          // terminator (`compile_if`/`compile_match`/`compile_block_as_value`
          // emit `unreachable` when nothing reaches their merge point) — in
          // that case nothing reaches here, and adding another terminator
          // (`ret`) would insert an instruction after one, which LLVM's
          // verifier rejects.
          if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
            if (return_is_unit_) {
              builder_.CreateRetVoid();
            } else {
              builder_.CreateRet(*value);
            }
          }
        } else {
          auto result = compile_stmt(last);
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          if (!*result) {
            builder_.CreateRetVoid();
          }
        }
      }
    }

    alloca_marker_->eraseFromParent();
    return {};
  }
  // ------------------------------------------------------------------
  //  Locals and types.
  // ------------------------------------------------------------------

  [[nodiscard]] auto create_local_alloca(llvm::Type *ty,
                                         const std::string &name)
      -> llvm::AllocaInst * {
    auto tmp = llvm::IRBuilder<>(alloca_marker_);
    return tmp.CreateAlloca(ty, nullptr, name);
  }

  [[nodiscard]] auto lookup_local(hir::symbol_id symbol) const
      -> llvm::AllocaInst * {
    const auto found = locals_.find(symbol);
    return found == locals_.end() ? nullptr : found->second;
  }

  [[nodiscard]] auto numeric_kind_for(type_id id, source_span span)
      -> std::expected<numeric_kind, codegen_error> {
    const auto kind = numeric_kind_of(types_, id);
    if (!kind.has_value()) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_type,
          .span = span,
          .message = std::format(
              "type `{}` has no scalar llvm_codegen representation yet — "
              "non-scalar types and 128-bit widths are not supported until "
              "spec/codegen-design.md increment 6 (heap types) lands",
              types_.display(id))});
    }
    return *kind;
  }

  /// Like `numeric_kind_for`, but also accepts heap-backed types
  /// (`is_heap_type`), returning an opaque `ptr` for those instead of
  /// failing closed — used anywhere a local/field/element's LLVM storage
  /// type is needed (locals, aggregate slot loads), as opposed to
  /// `numeric_kind_for`'s call sites, which are genuinely scalar-only
  /// (arithmetic/comparison operands, casts).
  [[nodiscard]] auto storage_type_for(type_id id, source_span span)
      -> std::expected<llvm::Type *, codegen_error> {
    const auto ty = storage_llvm_type(types_, ctx_, id);
    if (!ty.has_value()) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_type,
          .span = span,
          .message = std::format("type `{}` has no scalar or heap-value "
                                 "llvm_codegen representation yet",
                                 types_.display(id))});
    }
    return *ty;
  }

  /// Whether `id` is the prelude's growable `list[T]` container — as
  /// opposed to a fixed `array[T, N]`, which shares construction/indexing
  /// codegen for everything except sizing/length (see `compile_array_init`/
  /// `compile_index`).
  [[nodiscard]] auto is_list_type(type_id id) const -> bool {
    const auto &entry = types_.entry(strip_refs(types_, id));
    return entry.kind == semantic::type_kind::builtin_generic_kind &&
           entry.name == "list";
  }

  /// Whether `id` is a fixed `array[byte, N]` — mirrors
  /// `bytecode_compiler::compile.cpp`'s `is_byte_array_type` exactly (see
  /// its doc comment): the one array element type given its own tightly
  /// byte-packed (1 byte/element) representation instead of the generic
  /// 8-bytes/element slot layout, so `buf[a..b]` can alias a real
  /// `str`/`slice[byte]` view with no copy.
  [[nodiscard]] auto is_byte_array_type(type_id id) const -> bool {
    const auto &entry = types_.entry(strip_refs(types_, id));
    if (entry.kind != semantic::type_kind::array_kind ||
        !entry.array_size.has_value()) {
      return false;
    }
    const auto &elem = types_.entry(entry.result);
    return elem.kind == semantic::type_kind::builtin_kind &&
           elem.name == "byte";
  }

  /// An element type's byte stride within a fixed `array[T,N]` or a
  /// `list[T]`'s growable backing store — `runtime::layout_of`'s size,
  /// clamped/defaulted to one of 1/2/4/8. Mirrors
  /// `bytecode_compiler::compile.cpp`'s identically-named/-shaped helper
  /// exactly, so both backends agree on how many bytes a narrow element
  /// (e.g. `int16`, `bool`) costs in contiguous storage; `array[byte,N]`'s
  /// old special-cased 1-byte stride (`is_byte_array_type`) is now just the
  /// size==1 case of this general rule.
  [[nodiscard]] auto element_stride(type_id element_type) const -> uint8_t {
    const auto layout = runtime::layout_of(types_, element_type);
    if (!layout.has_value() || layout->size_bytes == 0 ||
        layout->size_bytes > 8) {
      return 8;
    }
    return static_cast<uint8_t>(layout->size_bytes);
  }

  // ------------------------------------------------------------------
  //  Panics — every checked-arithmetic guard funnels through this, so a
  //  panic always looks like: compute a bool "should panic" condition,
  //  branch to a block that calls the runtime and terminates with
  //  `unreachable`, otherwise fall through to a fresh block that carries on
  //  — mirroring `bytecode::vm`'s `throw panic_error(...)` sites exactly
  //  (same closed `panic_reason` set, same trigger conditions), just
  //  reified as control flow instead of a C++ exception.
  // ------------------------------------------------------------------

  /// Which panic a failing contract raises — kept identical to
  /// `bytecode_compiler`'s own mapping so a `pre` that panics on the VM
  /// panics the same way, and says the same thing, once compiled.
  [[nodiscard]] static auto panic_reason_for(hir::contract_kind kind) noexcept
      -> panic_reason {
    switch (kind) {
    case hir::contract_kind::precondition:
      return panic_reason::precondition_violated;
    case hir::contract_kind::postcondition:
      return panic_reason::postcondition_violated;
    case hir::contract_kind::invariant:
      return panic_reason::invariant_violated;
    }
    return panic_reason::explicit_panic;
  }

  auto guard_panic(llvm::Value *panic_if_true, panic_reason reason) -> void {
    auto *panic_bb = llvm::BasicBlock::Create(ctx_, "panic", current_fn_);
    auto *ok_bb = llvm::BasicBlock::Create(ctx_, "ok", current_fn_);
    builder_.CreateCondBr(panic_if_true, panic_bb, ok_bb);
    builder_.SetInsertPoint(panic_bb);
    builder_.CreateCall(panic_fn_,
                        {llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_),
                                                static_cast<uint8_t>(reason))});
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(ok_bb);
  }

  // ------------------------------------------------------------------
  //  Heap values (spec/codegen-design.md increment 6) — src/runtime/
  //  layout.h's flat "N 8-byte slots" representation, shared byte-for-byte
  //  with bytecode::vm's op_alloc/op_load_slot/op_store_slot (Decision 3).
  //  Opaque pointers make this simple: a slot's address is just a byte
  //  offset GEP off the block's base pointer, and `CreateLoad`/`CreateStore`
  //  can target that address with whatever concrete LLVM type the caller
  //  needs (i64/double/ptr) with no bitcast — the same "reinterpret raw
  //  bytes per the statically-known kind" approach `bytecode::vm`'s
  //  `slot_value` union already takes.
  // ------------------------------------------------------------------

  [[nodiscard]] auto compile_heap_alloc(size_t slot_count) -> llvm::Value * {
    auto *size =
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), slot_count * 8);
    return builder_.CreateCall(alloc_fn_, {size});
  }

  /// `compile_heap_alloc`'s byte-precise sibling — struct construction
  /// (`compile_struct_init`) allocates exactly `runtime::struct_layout`'s
  /// size (padded or, with `packed`, byte-tight) rather than
  /// `field_count * 8`, mirroring `bytecode_compiler::compile.cpp`'s own
  /// `emit_alloc` byte-size callers.
  [[nodiscard]] auto compile_heap_alloc_bytes(size_t byte_size)
      -> llvm::Value * {
    auto *size =
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), byte_size);
    return builder_.CreateCall(alloc_fn_, {size});
  }

  [[nodiscard]] auto slot_address(llvm::Value *block_ptr, size_t slot_index)
      -> llvm::Value * {
    auto *offset =
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), slot_index * 8);
    return builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), block_ptr, offset);
  }

  /// The runtime-indexed counterpart above — used for fixed `array[T, N]`
  /// element access, where the index is an ordinary runtime value the
  /// compiler bounds-checks itself (via `guard_panic`) before emitting this,
  /// not a compile-time constant.
  [[nodiscard]] auto slot_address(llvm::Value *block_ptr,
                                  llvm::Value *dynamic_slot_index)
      -> llvm::Value * {
    auto *byte_offset = builder_.CreateMul(
        dynamic_slot_index,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 8));
    return builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), block_ptr,
                              byte_offset);
  }

  /// Byte-granular counterparts of the two `slot_address` overloads above —
  /// used only for `array[byte, N]`'s tightly-packed representation
  /// (`is_byte_array_type`): a raw byte offset/index, not scaled by 8, into
  /// the block `compile_heap_alloc((N + 7) / 8)` reserved for it.
  [[nodiscard]] auto byte_address(llvm::Value *block_ptr, size_t byte_offset)
      -> llvm::Value * {
    auto *offset =
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), byte_offset);
    return builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), block_ptr, offset);
  }
  [[nodiscard]] auto byte_address(llvm::Value *block_ptr,
                                  llvm::Value *dynamic_byte_index)
      -> llvm::Value * {
    return builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), block_ptr,
                              dynamic_byte_index);
  }

  // ------------------------------------------------------------------
  //  Expressions — return the computed SSA value directly (no destination-
  //  register threading the way `bytecode_compiler` needs; LLVM's own SSA
  //  form makes that unnecessary).
  // ------------------------------------------------------------------

  [[nodiscard]] auto compile_expr(const hir::hir_expr &expr)
      -> std::expected<llvm::Value *, codegen_error> {
    switch (expr.kind) {
    case hir_node_kind::hir_literal:
      return compile_literal(dynamic_cast<const hir::hir_literal &>(expr));
    case hir_node_kind::hir_local_ref: {
      const auto &ref = dynamic_cast<const hir::hir_local_ref &>(expr);
      auto *alloca = lookup_local(ref.symbol);
      if (alloca == nullptr) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = expr.span,
            .message = std::format(
                "reference to `{}` is not a local binding — a bare function "
                "value used outside of call position is not supported yet",
                ref.name)});
      }
      return builder_.CreateLoad(alloca->getAllocatedType(), alloca, ref.name);
    }
    case hir_node_kind::hir_binary:
      return compile_binary(dynamic_cast<const hir::hir_binary &>(expr));
    case hir_node_kind::hir_unary:
      return compile_unary(dynamic_cast<const hir::hir_unary &>(expr));
    case hir_node_kind::hir_cast:
      return compile_cast(dynamic_cast<const hir::hir_cast &>(expr));
    case hir_node_kind::hir_call:
      return compile_call(dynamic_cast<const hir::hir_call &>(expr));
    case hir_node_kind::hir_lambda:
      return compile_lambda_value(dynamic_cast<const hir::hir_lambda &>(expr));
    case hir_node_kind::hir_if: {
      // `storage_type_for`, not `numeric_kind_for`: an `if` in expression
      // position yields a heap value as readily as a scalar one (`if c: "a"
      // else: "b"`, or the `if`/`else` a refinement's `try_from` desugars
      // into, whose value is an `option`), exactly like the `hir_match` and
      // `hir_block` cases below.
      const auto &node = dynamic_cast<const hir::hir_if &>(expr);
      auto ty = storage_type_for(expr.type, expr.span);
      if (!ty.has_value()) {
        return std::unexpected(ty.error());
      }
      auto *result = create_local_alloca(*ty, "if.result");
      auto terminated = compile_if(node, result);
      if (!terminated.has_value()) {
        return std::unexpected(terminated.error());
      }
      if (*terminated) {
        // Every branch diverged (e.g. via `return`) — `compile_if` already
        // ended the current block with `unreachable`, so `result` was
        // never written and nothing after this point can execute. Loading
        // from `result` here would insert an instruction after that
        // `unreachable` terminator, which LLVM's verifier rejects
        // ("Terminator found in the middle of a basic block"); return an
        // undef value of the right type instead — it is provably dead,
        // never actually observed by anything.
        return llvm::UndefValue::get(*ty);
      }
      return builder_.CreateLoad(*ty, result, "if.value");
    }
    case hir_node_kind::hir_tuple:
      return compile_slots_init(
          dynamic_cast<const hir::hir_tuple &>(expr).elements);
    case hir_node_kind::hir_struct_init:
      return compile_struct_init(
          dynamic_cast<const hir::hir_struct_init &>(expr));
    case hir_node_kind::hir_array_init:
      return compile_array_init(
          dynamic_cast<const hir::hir_array_init &>(expr));
    case hir_node_kind::hir_field:
      return compile_field(dynamic_cast<const hir::hir_field &>(expr));
    case hir_node_kind::hir_index:
      return compile_index(dynamic_cast<const hir::hir_index &>(expr));
    case hir_node_kind::hir_tuple_index:
      return compile_tuple_index(
          dynamic_cast<const hir::hir_tuple_index &>(expr));
    case hir_node_kind::hir_variant_init:
      return compile_variant_init(
          dynamic_cast<const hir::hir_variant_init &>(expr));
    case hir_node_kind::hir_variant_payload:
      return compile_variant_payload(
          dynamic_cast<const hir::hir_variant_payload &>(expr));
    case hir_node_kind::hir_match: {
      const auto &node = dynamic_cast<const hir::hir_match &>(expr);
      auto ty = storage_type_for(expr.type, expr.span);
      if (!ty.has_value()) {
        return std::unexpected(ty.error());
      }
      auto *result = create_local_alloca(*ty, "match.result");
      auto terminated = compile_match(node, result);
      if (!terminated.has_value()) {
        return std::unexpected(terminated.error());
      }
      if (*terminated) {
        // See hir_if's own note just above — every arm diverged, `result`
        // was never written, and the load below would land after
        // `compile_match`'s `unreachable` terminator.
        return llvm::UndefValue::get(*ty);
      }
      return builder_.CreateLoad(*ty, result, "match.value");
    }
    case hir_node_kind::hir_container_len:
      return compile_container_len(
          dynamic_cast<const hir::hir_container_len &>(expr));
    case hir_node_kind::hir_generator_next:
      return compile_generator_next(
          dynamic_cast<const hir::hir_generator_next &>(expr));
    case hir_node_kind::hir_block: {
      // A block used in expression position (e.g. a comprehension's
      // desugared accumulator block, `hir::lower_comprehension`) — mirrors
      // the `hir_match`/`hir_if` expression cases just above: alloca a
      // result slot, compile the block's statements into it, load it back.
      const auto &node = dynamic_cast<const hir::hir_block &>(expr);
      auto ty = storage_type_for(expr.type, expr.span);
      if (!ty.has_value()) {
        return std::unexpected(ty.error());
      }
      auto *result = create_local_alloca(*ty, "block.result");
      auto terminated = compile_block_as_value(node, result);
      if (!terminated.has_value()) {
        return std::unexpected(terminated.error());
      }
      if (*terminated) {
        // See hir_if's own note above — the block diverged before
        // reaching its own tail value.
        return llvm::UndefValue::get(*ty);
      }
      return builder_.CreateLoad(*ty, result, "block.value");
    }
    default:
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = expr.span,
          .message = "this expression form is outside llvm_codegen's current "
                     "scope"});
    }
  }

  /// Shared by `compile_literal` and `compile_pattern_test`'s literal-
  /// pattern case — the actual constant-folding logic for every scalar
  /// literal kind, independent of whether the caller has a `hir_literal`
  /// (which carries this same `lit_kind`/`value`/`span` shape) or a
  /// `hir_literal_pattern` (which carries it too, just not as a `hir_expr`).
  [[nodiscard]] auto compile_literal_value(token_kind lit_kind,
                                           std::string_view value,
                                           source_span span, numeric_kind kind)
      -> std::expected<llvm::Value *, codegen_error> {
    auto *ty = llvm_type_for(ctx_, kind);
    switch (lit_kind) {
    case token_kind::kw_true:
      return llvm::ConstantInt::get(ty, 1);
    case token_kind::kw_false:
      return llvm::ConstantInt::get(ty, 0);
    case token_kind::int_lit: {
      const auto parsed = parse_uint_literal(value);
      if (!parsed.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = span,
            .message =
                std::format("could not parse integer literal `{}`", value)});
      }
      if (is_float(kind)) {
        return llvm::ConstantFP::get(ty, static_cast<double>(*parsed));
      }
      return llvm::ConstantInt::get(ty, *parsed);
    }
    case token_kind::float_lit: {
      const auto parsed = parse_float_literal(value);
      if (!parsed.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = span,
            .message =
                std::format("could not parse float literal `{}`", value)});
      }
      return llvm::ConstantFP::get(ty, *parsed);
    }
    case token_kind::char_lit: {
      const auto parsed = decode_char_literal(value);
      if (!parsed.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = span,
            .message =
                std::format("could not decode character literal `{}`", value)});
      }
      return llvm::ConstantInt::get(ty, *parsed);
    }
    default:
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = span,
          .message = "string literals and other non-scalar literal kinds are "
                     "not supported yet (heap types land in "
                     "spec/codegen-design.md increment 6)"});
    }
  }

  [[nodiscard]] auto compile_literal(const hir::hir_literal &lit)
      -> std::expected<llvm::Value *, codegen_error> {
    if (lit.lit_kind == token_kind::string_lit) {
      return compile_string_literal(lit);
    }
    if (lit.lit_kind == token_kind::kw_unit) {
      // `unit` has exactly one value and carries no information — nothing
      // downstream ever reads it back out (a `unit`-typed return discards
      // this value in favor of `CreateRetVoid`, see `return_is_unit_`
      // above), so a zero placeholder is enough. Bypasses `numeric_kind_for`
      // below, which has no scalar kind for `unit` at all.
      return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0);
    }
    auto kind = numeric_kind_for(lit.type, lit.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    return compile_literal_value(lit.lit_kind, lit.value, lit.span, *kind);
  }

  [[nodiscard]] auto compile_binary(const hir::hir_binary &bin)
      -> std::expected<llvm::Value *, codegen_error> {
    if (bin.op == ast::binary_op::logical_and ||
        bin.op == ast::binary_op::logical_or) {
      return compile_short_circuit(bin);
    }

    auto kind = numeric_kind_for(bin.lhs->type, bin.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    auto lhs = compile_expr(*bin.lhs);
    if (!lhs.has_value()) {
      return std::unexpected(lhs.error());
    }
    auto rhs = compile_expr(*bin.rhs);
    if (!rhs.has_value()) {
      return std::unexpected(rhs.error());
    }
    return compile_binary_op(bin, *kind, *lhs, *rhs);
  }

  [[nodiscard]] auto compile_short_circuit(const hir::hir_binary &bin)
      -> std::expected<llvm::Value *, codegen_error> {
    auto *result =
        create_local_alloca(llvm::Type::getInt1Ty(ctx_), "sc.result");
    auto lhs = compile_expr(*bin.lhs);
    if (!lhs.has_value()) {
      return std::unexpected(lhs.error());
    }
    builder_.CreateStore(*lhs, result);

    auto *rhs_bb = llvm::BasicBlock::Create(ctx_, "sc.rhs", current_fn_);
    auto *merge_bb = llvm::BasicBlock::Create(ctx_, "sc.end", current_fn_);
    if (bin.op == ast::binary_op::logical_and) {
      builder_.CreateCondBr(*lhs, rhs_bb, merge_bb);
    } else {
      builder_.CreateCondBr(*lhs, merge_bb, rhs_bb);
    }

    builder_.SetInsertPoint(rhs_bb);
    auto rhs = compile_expr(*bin.rhs);
    if (!rhs.has_value()) {
      return std::unexpected(rhs.error());
    }
    builder_.CreateStore(*rhs, result);
    builder_.CreateBr(merge_bb);

    builder_.SetInsertPoint(merge_bb);
    return builder_.CreateLoad(llvm::Type::getInt1Ty(ctx_), result, "sc.value");
  }

  [[nodiscard]] auto compile_binary_op(const hir::hir_binary &bin,
                                       numeric_kind kind, llvm::Value *lhs,
                                       llvm::Value *rhs)
      -> std::expected<llvm::Value *, codegen_error> {
    using ast::binary_op;
    const auto bits = bit_width(kind);
    const auto signed_kind = is_signed_integer(kind);

    switch (bin.op) {
    case binary_op::eq_eq:
      return is_float(kind) ? builder_.CreateFCmpOEQ(lhs, rhs)
                            : builder_.CreateICmpEQ(lhs, rhs);
    case binary_op::bang_eq:
      return is_float(kind) ? builder_.CreateFCmpONE(lhs, rhs)
                            : builder_.CreateICmpNE(lhs, rhs);
    case binary_op::lt:
      if (is_float(kind)) {
        return builder_.CreateFCmpOLT(lhs, rhs);
      }
      return signed_kind ? builder_.CreateICmpSLT(lhs, rhs)
                         : builder_.CreateICmpULT(lhs, rhs);
    case binary_op::lt_eq:
      if (is_float(kind)) {
        return builder_.CreateFCmpOLE(lhs, rhs);
      }
      return signed_kind ? builder_.CreateICmpSLE(lhs, rhs)
                         : builder_.CreateICmpULE(lhs, rhs);
    case binary_op::gt:
      if (is_float(kind)) {
        return builder_.CreateFCmpOGT(lhs, rhs);
      }
      return signed_kind ? builder_.CreateICmpSGT(lhs, rhs)
                         : builder_.CreateICmpUGT(lhs, rhs);
    case binary_op::gt_eq:
      if (is_float(kind)) {
        return builder_.CreateFCmpOGE(lhs, rhs);
      }
      return signed_kind ? builder_.CreateICmpSGE(lhs, rhs)
                         : builder_.CreateICmpUGE(lhs, rhs);
    case binary_op::add:
      if (is_float(kind)) {
        return builder_.CreateFAdd(lhs, rhs);
      }
      return checked_arith(signed_kind ? llvm::Intrinsic::sadd_with_overflow
                                       : llvm::Intrinsic::uadd_with_overflow,
                           lhs, rhs);
    case binary_op::sub:
      if (is_float(kind)) {
        return builder_.CreateFSub(lhs, rhs);
      }
      return checked_arith(signed_kind ? llvm::Intrinsic::ssub_with_overflow
                                       : llvm::Intrinsic::usub_with_overflow,
                           lhs, rhs);
    case binary_op::mul:
      if (is_float(kind)) {
        return builder_.CreateFMul(lhs, rhs);
      }
      return checked_arith(signed_kind ? llvm::Intrinsic::smul_with_overflow
                                       : llvm::Intrinsic::umul_with_overflow,
                           lhs, rhs);
    case binary_op::div:
      if (is_float(kind)) {
        return builder_.CreateFDiv(lhs, rhs);
      }
      return checked_div(signed_kind, bits, lhs, rhs, /*is_mod=*/false);
    case binary_op::mod:
      if (is_float(kind)) {
        return builder_.CreateFRem(lhs, rhs);
      }
      return checked_div(signed_kind, bits, lhs, rhs, /*is_mod=*/true);
    case binary_op::add_wrap:
      return builder_.CreateAdd(lhs, rhs);
    case binary_op::sub_wrap:
      return builder_.CreateSub(lhs, rhs);
    case binary_op::mul_wrap:
      return builder_.CreateMul(lhs, rhs);
    case binary_op::add_sat:
      return builder_.CreateBinaryIntrinsic(
          signed_kind ? llvm::Intrinsic::sadd_sat : llvm::Intrinsic::uadd_sat,
          lhs, rhs);
    case binary_op::sub_sat:
      return builder_.CreateBinaryIntrinsic(
          signed_kind ? llvm::Intrinsic::ssub_sat : llvm::Intrinsic::usub_sat,
          lhs, rhs);
    case binary_op::shl:
    case binary_op::shr:
      return checked_shift(bin.op == binary_op::shl, signed_kind, bits, lhs,
                           rhs);
    case binary_op::bit_and:
      return builder_.CreateAnd(lhs, rhs);
    case binary_op::bit_or:
      return builder_.CreateOr(lhs, rhs);
    case binary_op::bit_xor:
      return builder_.CreateXor(lhs, rhs);
    default:
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = bin.span,
          .message = std::format("operator `{}` is not supported by "
                                 "llvm_codegen yet",
                                 ast::binary_op_name(bin.op))});
    }
  }

  [[nodiscard]] auto checked_arith(llvm::Intrinsic::ID id, llvm::Value *lhs,
                                   llvm::Value *rhs) -> llvm::Value * {
    auto *pair = builder_.CreateBinaryIntrinsic(id, lhs, rhs);
    // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) - false positive:
    // the analyzer's path-sensitive trace bottoms out in LLVM's own
    // OperandTraits/User operand-accessor macros (vendored code we don't
    // control), not in anything this call site does.
    auto *value = builder_.CreateExtractValue(pair, 0);
    auto *overflowed = builder_.CreateExtractValue(pair, 1);
    guard_panic(overflowed, panic_reason::integer_overflow);
    return value;
  }

  [[nodiscard]] auto checked_div(bool is_signed, int bits, llvm::Value *lhs,
                                 llvm::Value *rhs, bool is_mod)
      -> llvm::Value * {
    auto *ty = lhs->getType();
    auto *zero = llvm::ConstantInt::get(ty, 0);
    guard_panic(builder_.CreateICmpEQ(rhs, zero),
                panic_reason::integer_divide_by_zero);
    if (is_signed) {
      auto *min_value = llvm::ConstantInt::get(
          ty, llvm::APInt::getSignedMinValue(static_cast<unsigned>(bits)));
      auto *neg_one = llvm::ConstantInt::get(ty, static_cast<uint64_t>(-1),
                                             /*isSigned=*/true);
      auto *is_min = builder_.CreateICmpEQ(lhs, min_value);
      auto *is_neg_one = builder_.CreateICmpEQ(rhs, neg_one);
      guard_panic(builder_.CreateAnd(is_min, is_neg_one),
                  panic_reason::integer_overflow);
      return is_mod ? builder_.CreateSRem(lhs, rhs)
                    : builder_.CreateSDiv(lhs, rhs);
    }
    return is_mod ? builder_.CreateURem(lhs, rhs)
                  : builder_.CreateUDiv(lhs, rhs);
  }

  [[nodiscard]] auto checked_shift(bool is_shl, bool is_signed, int bits,
                                   llvm::Value *lhs, llvm::Value *rhs)
      -> llvm::Value * {
    auto *bits_const =
        llvm::ConstantInt::get(rhs->getType(), static_cast<uint64_t>(bits));
    guard_panic(builder_.CreateICmpUGE(rhs, bits_const),
                panic_reason::integer_overflow);
    if (is_shl) {
      return builder_.CreateShl(lhs, rhs);
    }
    return is_signed ? builder_.CreateAShr(lhs, rhs)
                     : builder_.CreateLShr(lhs, rhs);
  }

  /// Whether `id`'s runtime representation is already a single pointer into
  /// a flat N-slot heap block (`src/runtime/layout.h`) rather than an
  /// inline scalar value. For exactly these kinds, `&`/`&mut` needs no new
  /// instruction at all: the compiled value already *is* the address a
  /// reference would hold — see `compile_unary`'s `addr_of`/`addr_of_mut`
  /// passthrough below.
  [[nodiscard]] auto is_heap_pointer_value(type_id id) const -> bool {
    const auto &entry = types_.entry(id);
    switch (entry.kind) {
    case semantic::type_kind::struct_kind:
    case semantic::type_kind::sum_kind:
    case semantic::type_kind::opaque_kind:
    case semantic::type_kind::fn_kind:
    case semantic::type_kind::tuple_kind:
    case semantic::type_kind::array_kind:
      return true;
    case semantic::type_kind::builtin_kind:
      return entry.name == "str";
    case semantic::type_kind::builtin_generic_kind:
      return entry.name == "list" || entry.name == "slice" ||
             entry.name == "slice_mut" || entry.name == "option" ||
             entry.name == "result";
    default:
      return false;
    }
  }

  [[nodiscard]] auto compile_unary(const hir::hir_unary &un)
      -> std::expected<llvm::Value *, codegen_error> {
    if (un.op == ast::unary_op::logical_not) {
      auto src = compile_expr(*un.operand);
      if (!src.has_value()) {
        return std::unexpected(src.error());
      }
      return builder_.CreateNot(*src);
    }
    if ((un.op == ast::unary_op::addr_of ||
         un.op == ast::unary_op::addr_of_mut) &&
        is_heap_pointer_value(un.operand->type)) {
      // The operand's compiled value already is the pointer a reference to
      // it would hold — compile it straight through, no new instruction.
      return compile_expr(*un.operand);
    }
    if (un.op != ast::unary_op::neg && un.op != ast::unary_op::bit_not) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = un.span,
          .message =
              std::format("operator `{}` needs a reference/pointer model "
                          "llvm_codegen doesn't have yet",
                          ast::unary_op_name(un.op))});
    }
    auto kind = numeric_kind_for(un.operand->type, un.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    auto src = compile_expr(*un.operand);
    if (!src.has_value()) {
      return std::unexpected(src.error());
    }
    if (un.op == ast::unary_op::bit_not) {
      return builder_.CreateNot(*src);
    }
    // Neg: float negates plainly; signed integer negation panics exactly
    // when negating the type's minimum value (mirrors `vm.cpp`'s
    // `checked_neg_signed` — the checker guarantees `Neg` never reaches an
    // unsigned operand, matching `exec_neg`'s own precondition comment).
    if (is_float(*kind)) {
      return builder_.CreateFNeg(*src);
    }
    const auto bits = bit_width(*kind);
    auto *min_value = llvm::ConstantInt::get(
        (*src)->getType(),
        llvm::APInt::getSignedMinValue(static_cast<unsigned>(bits)));
    guard_panic(builder_.CreateICmpEQ(*src, min_value),
                panic_reason::integer_overflow);
    return builder_.CreateNeg(*src);
  }

  [[nodiscard]] auto compile_cast(const hir::hir_cast &cast)
      -> std::expected<llvm::Value *, codegen_error> {
    auto from_kind = numeric_kind_for(cast.operand->type, cast.span);
    if (!from_kind.has_value()) {
      return std::unexpected(from_kind.error());
    }
    auto to_kind = numeric_kind_for(cast.type, cast.span);
    if (!to_kind.has_value()) {
      return std::unexpected(to_kind.error());
    }
    auto src = compile_expr(*cast.operand);
    if (!src.has_value()) {
      return std::unexpected(src.error());
    }
    auto *to_ty = llvm_type_for(ctx_, *to_kind);
    const auto from_float = is_float(*from_kind);
    const auto to_float = is_float(*to_kind);

    if (from_float && to_float) {
      return *to_kind == numeric_kind::f32 ? builder_.CreateFPTrunc(*src, to_ty)
                                           : builder_.CreateFPExt(*src, to_ty);
    }
    if (from_float) {
      return is_signed_integer(*to_kind) ? builder_.CreateFPToSI(*src, to_ty)
                                         : builder_.CreateFPToUI(*src, to_ty);
    }
    if (to_float) {
      return is_signed_integer(*from_kind) ? builder_.CreateSIToFP(*src, to_ty)
                                           : builder_.CreateUIToFP(*src, to_ty);
    }
    // Integer-to-integer: sign-extend if the *source* was signed, zero-
    // extend otherwise, then truncate to the destination width — matches
    // `bytecode::vm`'s `exec_cast` exactly.
    return is_signed_integer(*from_kind)
               ? builder_.CreateSExtOrTrunc(*src, to_ty)
               : builder_.CreateZExtOrTrunc(*src, to_ty);
  }

  /// Identical scheme to `bytecode_compiler::function_compiler`'s own
  /// `resolve_callee_key` (see its doc comment) — the `functions_` key a
  /// call to `ref` resolves against.
  [[nodiscard]] auto resolve_callee_key(const hir::hir_local_ref &ref) const
      -> std::string {
    if (ref.owner_module.has_value()) {
      return *ref.owner_module == entry_module_name_
                 ? ref.name
                 : *ref.owner_module + "::" + ref.name;
    }
    return current_module_name_ == entry_module_name_
               ? ref.name
               : current_module_name_ + "::" + ref.name;
  }

  [[nodiscard]] auto compile_call(const hir::hir_call &call)
      -> std::expected<llvm::Value *, codegen_error> {
    // Direct call to a named module-level function, resolved at compile
    // time to a real `llvm::Function*` — the common case.
    if (call.callee->kind == hir_node_kind::hir_local_ref) {
      const auto &ref = dynamic_cast<const hir::hir_local_ref &>(*call.callee);
      if (lookup_local(ref.symbol) == nullptr) {
        // `intrinsic def` declarations never enter `functions_` (mirrors
        // `bytecode_compiler::compile_call` — `hir::lower_module` skips
        // them, there is no body to lower), so a call to a known intrinsic
        // name is recognized here instead and dispatched straight to its
        // `kira_rt_*` C-ABI symbol (declared in `compile_module`,
        // implemented in `src/runtime/io.h`).
        if (const auto intrinsic_id = kira::intrinsic_index_of(ref.name);
            intrinsic_id.has_value()) {
          auto args = std::vector<llvm::Value *>{};
          args.reserve(call.args.size());
          for (const auto &arg : call.args) {
            auto value = compile_expr(*arg);
            if (!value.has_value()) {
              return std::unexpected(value.error());
            }
            args.push_back(*value);
          }
          return builder_.CreateCall(intrinsic_fns_.at(*intrinsic_id), args);
        }

        const auto found = functions_.find(resolve_callee_key(ref));
        if (found == functions_.end()) {
          return std::unexpected(codegen_error{
              .kind = codegen_error_kind::unknown_callee,
              .span = call.span,
              .message = std::format("call to `{}` could not be resolved to "
                                     "a function in this compiled module",
                                     ref.name)});
        }
        auto args = std::vector<llvm::Value *>{};
        args.reserve(call.args.size());
        for (const auto &arg : call.args) {
          auto value = compile_expr(*arg);
          if (!value.has_value()) {
            return std::unexpected(value.error());
          }
          args.push_back(*value);
        }
        return builder_.CreateCall(found->second, args);
      }
    }

    // Indirect call — the callee is a closure value (a local variable
    // holding a `fn(...)`-typed value, an immediately-invoked lambda
    // literal, or any other computed callee expression). The callee's
    // static `fn_kind` type (`call.callee->type`) tells us its declared
    // signature, since the closure heap value itself carries no type
    // information at runtime — matches `op_call_indirect`'s bytecode-side
    // convention: the environment pointer is the callee's hidden first
    // argument, ahead of the declared args.
    const auto &callee_entry = types_.entry(call.callee->type);
    auto param_types =
        std::vector<llvm::Type *>{llvm::PointerType::get(ctx_, 0)};
    param_types.reserve(1 + callee_entry.args.size());
    for (const auto arg_type : callee_entry.args) {
      auto ty = storage_type_for(arg_type, call.span);
      if (!ty.has_value()) {
        return std::unexpected(ty.error());
      }
      param_types.push_back(*ty);
    }
    llvm::Type *ret_ty = llvm::Type::getVoidTy(ctx_);
    if (!types_.is_unit(callee_entry.result)) {
      auto ty = storage_type_for(callee_entry.result, call.span);
      if (!ty.has_value()) {
        return std::unexpected(ty.error());
      }
      ret_ty = *ty;
    }
    auto *fn_type = llvm::FunctionType::get(ret_ty, param_types,
                                            /*isVarArg=*/false);

    auto closure = compile_expr(*call.callee);
    if (!closure.has_value()) {
      return std::unexpected(closure.error());
    }
    auto *ptr_ty = llvm::PointerType::get(ctx_, 0);
    auto *fn_ptr = builder_.CreateLoad(
        ptr_ty, slot_address(*closure, size_t{0}), "closure.fn");
    auto *env_ptr = builder_.CreateLoad(
        ptr_ty, slot_address(*closure, size_t{1}), "closure.env");

    auto args = std::vector<llvm::Value *>{env_ptr};
    args.reserve(1 + call.args.size());
    for (const auto &arg : call.args) {
      auto value = compile_expr(*arg);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      args.push_back(*value);
    }
    return builder_.CreateCall(fn_type, fn_ptr, args);
  }

  /// Compiles a lambda literal into a closure heap value `{ fn_ptr; env_ptr
  /// }`: allocates and populates an environment block from every free
  /// variable `hir::free_variables` finds (loaded out of the *enclosing*
  /// function's own locals), synthesizes a fresh `llvm::Function` for the
  /// lambda's body (env `ptr` first parameter, then the lambda's declared
  /// params) and compiles it immediately via a nested `function_compiler`
  /// (safe to reference from a call before this expression returns, unlike
  /// top-level functions, since nothing else can call an anonymous lambda
  /// before it's constructed), then ties the two together.
  [[nodiscard]] auto compile_lambda_value(const hir::hir_lambda &lambda)
      -> std::expected<llvm::Value *, codegen_error> {
    const auto free_vars = hir::free_variables(lambda);

    auto *ptr_ty = llvm::PointerType::get(ctx_, 0);
    llvm::Value *env_ptr = nullptr;
    auto capture_types = std::vector<llvm::Type *>{};
    if (free_vars.empty()) {
      env_ptr = llvm::ConstantPointerNull::get(ptr_ty);
    } else {
      auto *env_block = compile_heap_alloc(free_vars.size());
      capture_types.reserve(free_vars.size());
      for (size_t i = 0; i < free_vars.size(); ++i) {
        auto *alloca = lookup_local(free_vars[i]);
        if (alloca == nullptr) {
          return std::unexpected(codegen_error{
              .kind = codegen_error_kind::unsupported_construct,
              .span = lambda.span,
              .message = "a captured variable could not be found among this "
                         "function's locals — capture analysis and codegen "
                         "have gotten out of sync"});
        }
        auto *ty = alloca->getAllocatedType();
        capture_types.push_back(ty);
        auto *value = builder_.CreateLoad(ty, alloca, "capture");
        builder_.CreateStore(value, slot_address(env_block, i));
      }
      env_ptr = env_block;
    }

    auto param_types = std::vector<llvm::Type *>{ptr_ty};
    param_types.reserve(1 + lambda.params.size());
    for (const auto &param : lambda.params) {
      auto ty = storage_type_for(param.type, lambda.span);
      if (!ty.has_value()) {
        return std::unexpected(ty.error());
      }
      param_types.push_back(*ty);
    }
    llvm::Type *ret_ty = llvm::Type::getVoidTy(ctx_);
    if (!types_.is_unit(lambda.return_type)) {
      auto ty = storage_type_for(lambda.return_type, lambda.span);
      if (!ty.has_value()) {
        return std::unexpected(ty.error());
      }
      ret_ty = *ty;
    }
    auto *fn_type = llvm::FunctionType::get(ret_ty, param_types,
                                            /*isVarArg=*/false);
    auto *lambda_fn = llvm::Function::Create(
        fn_type, llvm::Function::InternalLinkage,
        std::format("lambda.{}", reinterpret_cast<uintptr_t>(&lambda)),
        current_fn_->getParent());

    auto nested = function_compiler(
        ctx_, types_, functions_, panic_fn_, alloc_fn_, list_reserve_slot_fn_,
        intrinsic_fns_, entry_module_name_, current_module_name_);
    auto compiled =
        nested.compile_lambda_body(lambda, free_vars, capture_types, lambda_fn);
    if (!compiled.has_value()) {
      return std::unexpected(compiled.error());
    }

    auto *closure_block = compile_heap_alloc(2);
    builder_.CreateStore(lambda_fn, slot_address(closure_block, size_t{0}));
    builder_.CreateStore(env_ptr, slot_address(closure_block, size_t{1}));
    return closure_block;
  }

  /// Compiles an `if`/`elif`/`else` chain. `want_result`, when non-null, is
  /// the alloca every reaching branch's tail value gets stored into before
  /// branching to the merge block — the same alloca+load merge strategy
  /// used everywhere else in this file instead of building PHI nodes by
  /// hand. Returns whether the merge point is unreachable (every branch
  /// terminated, e.g. via `return`) — the caller must not add a further
  /// unconditional terminator in that case, `compile_if` already added one.
  [[nodiscard]] auto compile_if(const hir::hir_if &node,
                                llvm::AllocaInst *want_result)
      -> std::expected<bool, codegen_error> {
    auto *merge_bb = llvm::BasicBlock::Create(ctx_, "if.end", current_fn_);
    auto any_reaches_merge = false;

    for (size_t i = 0; i < node.branches.size(); ++i) {
      const auto &branch = node.branches[i];
      auto cond = compile_expr(*branch.condition);
      if (!cond.has_value()) {
        return std::unexpected(cond.error());
      }
      auto *then_bb = llvm::BasicBlock::Create(ctx_, "if.then", current_fn_);
      const auto has_more =
          i + 1 < node.branches.size() || node.else_body != nullptr;
      auto *next_bb =
          has_more ? llvm::BasicBlock::Create(ctx_, "if.next", current_fn_)
                   : merge_bb;
      builder_.CreateCondBr(*cond, then_bb, next_bb);
      if (next_bb == merge_bb) {
        // No more conditions and no `else`: the condition's false edge is
        // an implicit "do nothing" arm that lands directly on `merge_bb`,
        // which is therefore reachable even if `then_bb` itself terminates.
        any_reaches_merge = true;
      }

      builder_.SetInsertPoint(then_bb);
      auto terminated = compile_block_as_value(*branch.body, want_result);
      if (!terminated.has_value()) {
        return std::unexpected(terminated.error());
      }
      if (!*terminated) {
        builder_.CreateBr(merge_bb);
        any_reaches_merge = true;
      }

      builder_.SetInsertPoint(next_bb);
      if (next_bb == merge_bb) {
        break;
      }
    }

    if (node.else_body != nullptr) {
      auto terminated = compile_block_as_value(*node.else_body, want_result);
      if (!terminated.has_value()) {
        return std::unexpected(terminated.error());
      }
      if (!*terminated) {
        builder_.CreateBr(merge_bb);
        any_reaches_merge = true;
      }
      builder_.SetInsertPoint(merge_bb);
    }

    if (!any_reaches_merge) {
      builder_.CreateUnreachable();
    }
    return !any_reaches_merge;
  }

  // ------------------------------------------------------------------
  //  Heap values (spec/codegen-design.md increment 6) — same flat "N
  //  8-byte slots" representation as bytecode::vm's op_alloc/op_load_slot/
  //  op_store_slot (Decision 3), built from `compile_heap_alloc`/
  //  `slot_address` above.
  // ------------------------------------------------------------------

  [[nodiscard]] auto compile_string_literal(const hir::hir_literal &lit)
      -> std::expected<llvm::Value *, codegen_error> {
    auto text = decode_string_literal(lit.value);
    if (!text.has_value()) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = lit.span,
          .message =
              std::format("could not decode string literal `{}`", lit.value)});
    }
    auto *data_ptr = builder_.CreateGlobalString(*text, "str.lit");
    auto *header = compile_heap_alloc(2);
    builder_.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), text->size()),
        slot_address(header, size_t{0}));
    builder_.CreateStore(data_ptr, slot_address(header, size_t{1}));
    return header;
  }

  /// Allocates a `values.size()`-slot heap block and stores each element's
  /// compiled value at its corresponding slot, in order — the shared shape
  /// behind tuple construction and the explicit-list form of an array
  /// literal, mirroring `bytecode_compiler::compile_slots_init`.
  [[nodiscard]] auto
  compile_slots_init(const hir::ptr_vec<hir::hir_expr> &values)
      -> std::expected<llvm::Value *, codegen_error> {
    auto *block = compile_heap_alloc(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto &elem = values[i];
      auto value = compile_expr(*elem);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      builder_.CreateStore(*value, slot_address(block, i));
    }
    return block;
  }

  /// Allocates a byte-precise struct block (`runtime::struct_layout` —
  /// padded by default, byte-packed with no padding if the struct's own
  /// `type` declaration carries `packed`) and stores each field at its own
  /// `runtime::struct_field_offset` via `byte_address` (a plain byte-offset
  /// GEP, not `slot_address`'s uniform 8x-scaled one) — `CreateStore`
  /// writes exactly `*value`'s own (possibly narrower-than-8-byte) LLVM
  /// type at that address, which is what makes a packed field's bytes land
  /// immediately after its neighbor with no gap, mirroring
  /// `bytecode_compiler::compile.cpp`'s `compile_struct_init`.
  [[nodiscard]] auto compile_struct_init(const hir::hir_struct_init &init)
      -> std::expected<llvm::Value *, codegen_error> {
    const auto layout = runtime::struct_layout(types_, init.type);
    auto *block = compile_heap_alloc_bytes(layout.size_bytes);
    for (const auto &field : init.fields) {
      const auto offset =
          runtime::struct_field_offset(types_, init.type, field.name);
      if (!offset.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = init.span,
            .message = std::format(
                "field `{}` does not resolve to a declared struct field — "
                "this should have been rejected by the type checker",
                field.name)});
      }
      auto value = compile_expr(*field.value);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      builder_.CreateStore(*value, byte_address(block, *offset));
    }
    return block;
  }

  /// Allocates a fixed `array[T, N]`'s `N * element_stride(T)`-byte block
  /// and stores each element at its own natural-stride `byte_address` — one
  /// path for every element type (`array[byte,N]`'s old tightly-packed
  /// special case, `compile_byte_array_init`, is now just the
  /// `element_stride == 1` instance of this general rule), mirroring
  /// `bytecode_compiler::compile.cpp`'s `compile_array_init` exactly.
  [[nodiscard]] auto compile_array_init(const hir::hir_array_init &init)
      -> std::expected<llvm::Value *, codegen_error> {
    if (is_list_type(init.type)) {
      return compile_list_init(init);
    }
    const auto &array_entry = types_.entry(init.type);
    const auto elem_size = element_stride(array_entry.result);
    if (init.fill_value == nullptr) {
      // `check.cpp` requires an explicit-elements array literal's element
      // count to match its declared size exactly.
      auto *block = compile_heap_alloc_bytes(init.elements.size() * elem_size);
      for (size_t i = 0; i < init.elements.size(); ++i) {
        auto value = compile_expr(*init.elements[i]);
        if (!value.has_value()) {
          return std::unexpected(value.error());
        }
        builder_.CreateStore(*value, byte_address(block, i * elem_size));
      }
      return block;
    }
    if (!array_entry.array_size.has_value()) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = init.span,
          .message = "this array literal's fill count is not statically "
                     "known, which llvm_codegen needs to size its heap "
                     "allocation"});
    }
    const auto count = *array_entry.array_size;
    auto *block = compile_heap_alloc_bytes(count * elem_size);
    // Evaluated once, then its bits are stored into every element — see
    // bytecode_compiler::compile_array_init's doc comment for why this is
    // correct (and required) rather than re-evaluating per element.
    auto fill_value = compile_expr(*init.fill_value);
    if (!fill_value.has_value()) {
      return std::unexpected(fill_value.error());
    }
    for (uint64_t i = 0; i < count; ++i) {
      builder_.CreateStore(*fill_value, byte_address(block, i * elem_size));
    }
    return block;
  }

  /// Constructs a growable `list[T]` heap value: an empty 3-slot header
  /// `{ len; cap; data }` (`src/runtime/layout.h`) grown one element at a
  /// time via `list_reserve_slot_fn_` (which returns the address to store
  /// each pushed value at directly — see that function's own doc comment
  /// for why the runtime never takes the value itself as a parameter). The
  /// explicit-elements form pushes each literal element in turn; the fill
  /// form `[val; count]` evaluates its value once and pushes it `count`
  /// times in a runtime loop — unlike a fixed array's statically-required
  /// `N`, a list's fill count may be an arbitrary runtime `usize`
  /// expression (`infer_array` in `check.cpp` only requires the *element*
  /// type, not the count, to be resolvable), so a loop is the only
  /// construction strategy that works uniformly either way.
  [[nodiscard]] auto compile_list_init(const hir::hir_array_init &init)
      -> std::expected<llvm::Value *, codegen_error> {
    auto *header = compile_heap_alloc(3);
    const auto &list_entry = types_.entry(init.type);
    const auto elem_size = list_entry.args.empty()
                               ? uint8_t{8}
                               : element_stride(list_entry.args.front());
    auto *elem_size_const =
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), elem_size);

    if (init.fill_value == nullptr) {
      for (const auto &elem : init.elements) {
        auto value = compile_expr(*elem);
        if (!value.has_value()) {
          return std::unexpected(value.error());
        }
        auto *slot = builder_.CreateCall(list_reserve_slot_fn_,
                                         {header, elem_size_const});
        builder_.CreateStore(*value, slot);
      }
      return header;
    }

    // Evaluated once, then pushed `count` times — see
    // `compile_array_init`'s fixed-array fill form for why a
    // side-effecting fill expression must only run once.
    auto fill_value = compile_expr(*init.fill_value);
    if (!fill_value.has_value()) {
      return std::unexpected(fill_value.error());
    }
    auto count_kind = numeric_kind_for(init.fill_count->type, init.span);
    if (!count_kind.has_value()) {
      return std::unexpected(count_kind.error());
    }
    auto count_value = compile_expr(*init.fill_count);
    if (!count_value.has_value()) {
      return std::unexpected(count_value.error());
    }
    auto *count64 =
        builder_.CreateIntCast(*count_value, llvm::Type::getInt64Ty(ctx_),
                               is_signed_integer(*count_kind), "count.i64");

    auto *idx_alloca =
        create_local_alloca(llvm::Type::getInt64Ty(ctx_), "list.fill.idx");
    builder_.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0), idx_alloca);

    auto *cond_bb =
        llvm::BasicBlock::Create(ctx_, "list.fill.cond", current_fn_);
    auto *body_bb =
        llvm::BasicBlock::Create(ctx_, "list.fill.body", current_fn_);
    auto *end_bb = llvm::BasicBlock::Create(ctx_, "list.fill.end", current_fn_);

    builder_.CreateBr(cond_bb);
    builder_.SetInsertPoint(cond_bb);
    auto *idx = builder_.CreateLoad(llvm::Type::getInt64Ty(ctx_), idx_alloca);
    // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) - false positive:
    // the analyzer's path-sensitive trace bottoms out in LLVM's own
    // OperandTraits/User operand-accessor macros (vendored code we don't
    // control), not in anything this call site does.
    auto *cond = builder_.CreateICmpULT(idx, count64, "list.fill.test");
    builder_.CreateCondBr(cond, body_bb, end_bb);

    builder_.SetInsertPoint(body_bb);
    auto *slot =
        builder_.CreateCall(list_reserve_slot_fn_, {header, elem_size_const});
    builder_.CreateStore(*fill_value, slot);
    auto *next_idx = builder_.CreateAdd(
        idx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 1));
    builder_.CreateStore(next_idx, idx_alloca);
    builder_.CreateBr(cond_bb);

    builder_.SetInsertPoint(end_bb);
    return header;
  }

  [[nodiscard]] auto compile_field(const hir::hir_field &field)
      -> std::expected<llvm::Value *, codegen_error> {
    const auto offset = runtime::struct_field_offset(
        types_, strip_refs(types_, field.object->type), field.field_name);
    if (!offset.has_value()) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = field.span,
          .message = std::format(
              "field access `.{}` is only supported on struct values by "
              "llvm_codegen yet",
              field.field_name)});
    }
    auto object = compile_expr(*field.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    auto elem_ty = storage_type_for(field.type, field.span);
    if (!elem_ty.has_value()) {
      return std::unexpected(elem_ty.error());
    }
    return builder_.CreateLoad(*elem_ty, byte_address(*object, *offset));
  }

  [[nodiscard]] auto compile_tuple_index(const hir::hir_tuple_index &node)
      -> std::expected<llvm::Value *, codegen_error> {
    auto object = compile_expr(*node.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    auto elem_ty = storage_type_for(node.type, node.span);
    if (!elem_ty.has_value()) {
      return std::unexpected(elem_ty.error());
    }
    return builder_.CreateLoad(*elem_ty, slot_address(*object, node.index));
  }

  /// Whether `id` is `str`, `slice[T]`, `slice_mut[T]`, or `array[byte, N]`
  /// — the kinds `compile_range_index` can slice by pure pointer arithmetic
  /// without needing an element-size-aware copy. Mirrors
  /// `bytecode_compiler::compile.cpp`'s identically-named function (see its
  /// doc comment): the first three share a byte-addressed 2-slot
  /// `{ len; data }` header (`src/runtime/io.h`); `array[byte, N]` has no
  /// such header (a fixed array's "data pointer" is the object itself, its
  /// length the statically-known `N`), but is *also* byte-addressed because
  /// `is_byte_array_type` gives it its own tightly-packed representation
  /// for exactly this reason. Every other `array[T,N]`/`list[T]` stores one
  /// 8-byte slot per element regardless of `T` (`compile_array_init`) and
  /// so cannot be range-indexed this way.
  [[nodiscard]] auto is_byte_addressed_slice_type(type_id id) const -> bool {
    const auto &entry = types_.entry(strip_refs(types_, id));
    if (entry.kind == semantic::type_kind::builtin_kind) {
      return entry.name == "str";
    }
    if (entry.kind == semantic::type_kind::builtin_generic_kind) {
      return entry.name == "slice" || entry.name == "slice_mut";
    }
    return is_byte_array_type(id);
  }

  /// `buf[a..b]`/`buf[a..=b]` on a `str`/`slice`/`slice_mut`/
  /// `array[byte, N]` source — builds a fresh 2-slot `{ len; data }` header
  /// whose `data` points into the source's own backing bytes, no copy. For
  /// the three header-based kinds, `len`/`data` are read from the source's
  /// own heap header; for `array[byte, N]` (`is_byte_array_type`), there is
  /// no header — `len` is the statically-known `N` and `data` is the
  /// array's own pointer, mirroring `compile_index`'s non-range element
  /// access. Returning a *view* rather than a copy matters for correctness,
  /// not just performance: `std.io.reader::read_to_end`'s
  /// `self.read(&mut buf[0..4096])` passes this as a `&mut` out-parameter
  /// `rt_read` writes through, and `read_to_end` reads the result back out
  /// of `buf` itself afterward — a copy-based view would silently drop
  /// every byte `rt_read` wrote.
  [[nodiscard]] auto compile_range_index(const hir::hir_index &node,
                                         const hir::hir_binary &range)
      -> std::expected<llvm::Value *, codegen_error> {
    if (range.lhs == nullptr || range.rhs == nullptr) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = node.span,
          .message = "a range used as an index needs both a start and an "
                     "end bound"});
    }
    auto object = compile_expr(*node.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    auto start_kind = numeric_kind_for(range.lhs->type, node.span);
    if (!start_kind.has_value()) {
      return std::unexpected(start_kind.error());
    }
    auto start_value = compile_expr(*range.lhs);
    if (!start_value.has_value()) {
      return std::unexpected(start_value.error());
    }
    auto *start64 = builder_.CreateIntCast(
        *start_value, llvm::Type::getInt64Ty(ctx_),
        is_signed_integer(*start_kind), "range_start.i64");

    auto end_kind = numeric_kind_for(range.rhs->type, node.span);
    if (!end_kind.has_value()) {
      return std::unexpected(end_kind.error());
    }
    auto end_value = compile_expr(*range.rhs);
    if (!end_value.has_value()) {
      return std::unexpected(end_value.error());
    }
    auto *end64 =
        builder_.CreateIntCast(*end_value, llvm::Type::getInt64Ty(ctx_),
                               is_signed_integer(*end_kind), "range_end.i64");
    // Normalize `a..=b` (an inclusive bound, `b` itself a valid index) to
    // the same exclusive-end shape `a..b` already is (`b` itself out of
    // bounds), so everything below treats `end64` uniformly either way.
    if (range.op == ast::binary_op::range_inclusive) {
      end64 = builder_.CreateAdd(
          end64, llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 1),
          "range_end.inclusive_adjust");
    }

    llvm::Value *len = nullptr;
    llvm::Value *data = nullptr;
    if (is_byte_array_type(node.object->type)) {
      len = llvm::ConstantInt::get(
          llvm::Type::getInt64Ty(ctx_),
          types_.entry(strip_refs(types_, node.object->type))
              .array_size.value());
      data = *object;
    } else {
      len = builder_.CreateLoad(llvm::Type::getInt64Ty(ctx_),
                                slot_address(*object, size_t{0}), "slice.len");
      data =
          builder_.CreateLoad(llvm::PointerType::get(ctx_, 0),
                              slot_address(*object, size_t{1}), "slice.data");
    }
    // `end > len` and `start > end` are both out-of-bounds — checked as two
    // separate panics so an inverted range still reports as an inverted
    // range even when `start` also happens to exceed `len`.
    guard_panic(builder_.CreateICmpUGT(end64, len, "range_end.oob"),
                panic_reason::index_out_of_bounds);
    guard_panic(builder_.CreateICmpUGT(start64, end64, "range_start.oob"),
                panic_reason::index_out_of_bounds);

    auto *new_data = builder_.CreateGEP(llvm::Type::getInt8Ty(ctx_), data,
                                        start64, "range_slice.data");
    auto *new_len = builder_.CreateSub(end64, start64, "range_slice.len");

    auto *header = compile_heap_alloc(2);
    builder_.CreateStore(new_len, slot_address(header, size_t{0}));
    builder_.CreateStore(new_data, slot_address(header, size_t{1}));
    return header;
  }

  /// Fixed `array[T, N]` and growable `list[T]` element access —
  /// `slice`/`str` indexing still needs a byte/view model this backend
  /// doesn't have yet. A fixed array's elements live directly in its own
  /// heap block (so its "data pointer" is the object itself, and its
  /// length is the statically-known `N`); a list's elements live in a
  /// separate block reached through its 3-slot header's `data` slot
  /// (`src/runtime/layout.h`), with its length read from the header's
  /// `len` slot at runtime — everything past that point (the bounds check,
  /// the indexed load) is shared.
  [[nodiscard]] auto compile_index(const hir::hir_index &node)
      -> std::expected<llvm::Value *, codegen_error> {
    if (node.index != nullptr &&
        node.index->kind == hir_node_kind::hir_binary) {
      const auto &maybe_range =
          dynamic_cast<const hir::hir_binary &>(*node.index);
      if (maybe_range.op == ast::binary_op::range ||
          maybe_range.op == ast::binary_op::range_inclusive) {
        if (!is_byte_addressed_slice_type(node.object->type)) {
          return std::unexpected(codegen_error{
              .kind = codegen_error_kind::unsupported_construct,
              .span = node.span,
              .message =
                  "range-indexing is only supported on `str`/`slice`/"
                  "`slice_mut`/`array[byte, N]` yet — any other fixed array "
                  "or list source needs an element-size-aware view model "
                  "this backend doesn't have yet"});
        }
        return compile_range_index(node, maybe_range);
      }
    }
    const auto &object_entry =
        types_.entry(strip_refs(types_, node.object->type));
    const bool indexing_list = is_list_type(node.object->type);
    if (!indexing_list &&
        (object_entry.kind != semantic::type_kind::array_kind ||
         !object_entry.array_size.has_value())) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = node.span,
          .message = "indexing is only supported for a fixed-size array "
                     "with a statically known length, or a list, yet — "
                     "slice/str indexing needs a byte/view model this "
                     "backend doesn't have yet"});
    }
    auto object = compile_expr(*node.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    auto index_kind = numeric_kind_for(node.index->type, node.span);
    if (!index_kind.has_value()) {
      return std::unexpected(index_kind.error());
    }
    auto index_value = compile_expr(*node.index);
    if (!index_value.has_value()) {
      return std::unexpected(index_value.error());
    }
    auto *index64 =
        builder_.CreateIntCast(*index_value, llvm::Type::getInt64Ty(ctx_),
                               is_signed_integer(*index_kind), "index.i64");

    llvm::Value *len = nullptr;
    llvm::Value *data = nullptr;
    uint8_t elem_size = 8;
    if (indexing_list) {
      len = builder_.CreateLoad(llvm::Type::getInt64Ty(ctx_),
                                slot_address(*object, size_t{0}), "list.len");
      data = builder_.CreateLoad(llvm::PointerType::get(ctx_, 0),
                                 slot_address(*object, size_t{2}), "list.data");
      elem_size = object_entry.args.empty()
                      ? uint8_t{8}
                      : element_stride(object_entry.args.front());
    } else {
      len = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_),
                                   *object_entry.array_size);
      data = *object;
      elem_size = element_stride(object_entry.result);
    }
    auto *out_of_bounds = builder_.CreateICmpUGE(index64, len, "index.oob");
    guard_panic(out_of_bounds, panic_reason::index_out_of_bounds);

    auto elem_ty = storage_type_for(node.type, node.span);
    if (!elem_ty.has_value()) {
      return std::unexpected(elem_ty.error());
    }
    // A plain `slot_address`-style 8x-scaled offset is only correct when
    // `elem_size == 8`; every element type now has its own natural stride
    // (`element_stride`), so the address is always computed as an explicit
    // byte offset instead — `array[byte,N]`'s old special case is just
    // `elem_size == 1` here.
    auto *stride_const =
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), elem_size);
    auto *byte_offset =
        builder_.CreateMul(index64, stride_const, "index.byte_offset");
    return builder_.CreateLoad(*elem_ty, byte_address(data, byte_offset));
  }

  /// A container's runtime element count (`for`/`while` loop bound
  /// checking over a `list`/`str`, whose length isn't statically known) —
  /// both layouts (`src/runtime/layout.h`) put their length at slot 0, so
  /// this is the same one load regardless of which container kind
  /// `node.object` actually is.
  [[nodiscard]] auto compile_container_len(const hir::hir_container_len &node)
      -> std::expected<llvm::Value *, codegen_error> {
    auto object = compile_expr(*node.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    return builder_.CreateLoad(llvm::Type::getInt64Ty(ctx_),
                               slot_address(*object, size_t{0}),
                               "container.len");
  }

  /// Builds `option::some(payload)`/`option::none` as a 2-slot
  /// `{ tag; payload }` heap block — mirrors `compile_variant_init`'s shape,
  /// but with the tag/payload-slot-count hardcoded (`some`=0, `none`=1, one
  /// payload slot) rather than looked up via `runtime::sum_variant_tag`
  /// against a real `option[T]` `type_id`: a generator's `.next()` result
  /// type is synthesized here, not read back from a `hir_variant_init` node
  /// the checker already resolved, so there's no guaranteed-interned
  /// `option[T]` instance to look one up from `types_` (a `const
  /// type_table&` at this layer, same constraint as the bytecode tier).
  /// Matches `bytecode::vm.cpp`'s `op_yield`/`op_generator_next` exactly,
  /// which hardcode the identical convention for the identical reason.
  [[nodiscard]] auto build_option_some(llvm::Value *payload) -> llvm::Value * {
    auto *block = compile_heap_alloc(2);
    builder_.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0),
        slot_address(block, size_t{0}));
    builder_.CreateStore(payload, slot_address(block, size_t{1}));
    return block;
  }

  [[nodiscard]] auto build_option_none() -> llvm::Value * {
    auto *block = compile_heap_alloc(2);
    builder_.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 1),
        slot_address(block, size_t{0}));
    return block;
  }

  /// Builds `option::none`, marks the generator object (`generator_self_
  /// arg_`) finished, and returns it — the step function's behavior on a
  /// bare `return` or falling off the end of the body. Syncs the state
  /// block first (harmless if nothing will read it again).
  [[nodiscard]] auto emit_generator_exhausted_return(source_span /*span*/)
      -> std::expected<void, codegen_error> {
    for (size_t i = 0; i < generator_state_layout_.size(); ++i) {
      auto *alloca = lookup_local(generator_state_layout_[i]);
      if (alloca != nullptr) {
        auto *loaded = builder_.CreateLoad(alloca->getAllocatedType(), alloca,
                                           "state.sync");
        builder_.CreateStore(loaded, slot_address(generator_state_arg_, i));
      }
    }
    builder_.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 1),
        slot_address(generator_self_arg_, size_t{3}));
    builder_.CreateRet(build_option_none());
    return {};
  }

  /// `g.next()` on a `generator[T]` value — mirrors
  /// `bytecode_compiler::compile_generator_next`/`op_generator_next`: if the
  /// generator object's `finished` slot is set, produce `none` directly (no
  /// call); otherwise call through its `step_function`/`state`/`resume_
  /// index` slots (register/argument order `{state; resume_index; self}`,
  /// matching the bytecode tier's convention exactly) and use whatever it
  /// returns. Merges the two paths through a shared result alloca
  /// (`compile_if`'s `want_result` pattern) rather than an LLVM `phi`, this
  /// file's uniform style for merging branch values.
  [[nodiscard]] auto compile_generator_next(const hir::hir_generator_next &node)
      -> std::expected<llvm::Value *, codegen_error> {
    auto object = compile_expr(*node.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    auto *gen = *object;
    auto *ptr_ty = llvm::PointerType::get(ctx_, 0);
    auto *result_alloca = create_local_alloca(ptr_ty, "gen.next.result");

    auto *finished =
        builder_.CreateLoad(llvm::Type::getInt64Ty(ctx_),
                            slot_address(gen, size_t{3}), "gen.finished");
    auto *is_finished = builder_.CreateICmpNE(
        finished, llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0),
        "gen.finished.bool");

    auto *finished_bb =
        llvm::BasicBlock::Create(ctx_, "gen.next.finished", current_fn_);
    auto *call_bb =
        llvm::BasicBlock::Create(ctx_, "gen.next.call", current_fn_);
    auto *merge_bb =
        llvm::BasicBlock::Create(ctx_, "gen.next.merge", current_fn_);
    builder_.CreateCondBr(is_finished, finished_bb, call_bb);

    builder_.SetInsertPoint(finished_bb);
    builder_.CreateStore(build_option_none(), result_alloca);
    builder_.CreateBr(merge_bb);

    builder_.SetInsertPoint(call_bb);
    auto *step_fn_ptr = builder_.CreateLoad(
        ptr_ty, slot_address(gen, size_t{0}), "gen.step_fn");
    auto *state_ptr =
        builder_.CreateLoad(ptr_ty, slot_address(gen, size_t{1}), "gen.state");
    auto *resume_idx =
        builder_.CreateLoad(llvm::Type::getInt64Ty(ctx_),
                            slot_address(gen, size_t{2}), "gen.resume");
    auto *step_fn_type = llvm::FunctionType::get(
        ptr_ty, {ptr_ty, llvm::Type::getInt64Ty(ctx_), ptr_ty},
        /*isVarArg=*/false);
    auto *call_result =
        builder_.CreateCall(step_fn_type, step_fn_ptr,
                            {state_ptr, resume_idx, gen}, "gen.next.call");
    builder_.CreateStore(call_result, result_alloca);
    builder_.CreateBr(merge_bb);

    builder_.SetInsertPoint(merge_bb);
    return builder_.CreateLoad(ptr_ty, result_alloca, "gen.next.value");
  }

  /// Sum-type variant construction `@variant(args...)` — mirrors
  /// `bytecode_compiler::compile_variant_init`: allocates a block sized to
  /// the widest variant's payload (slot 0 is the tag, so any variant of
  /// this sum type can later be reinterpreted from the same block), stores
  /// the tag, then each argument at its 1-based payload slot.
  [[nodiscard]] auto compile_variant_init(const hir::hir_variant_init &init)
      -> std::expected<llvm::Value *, codegen_error> {
    const auto tag =
        runtime::sum_variant_tag(types_, init.type, init.variant_name);
    if (!tag.has_value()) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = init.span,
          .message = std::format(
              "variant `{}` does not resolve to a declared sum-type "
              "variant — this should have been rejected by the type "
              "checker",
              init.variant_name)});
    }
    const auto payload_slots =
        runtime::sum_max_payload_slots(types_, init.type);
    auto *block = compile_heap_alloc(1 + payload_slots);
    builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_),
                                                static_cast<uint64_t>(*tag)),
                         slot_address(block, size_t{0}));
    for (std::size_t i = 0; i < init.args.size(); ++i) {
      const auto &arg = init.args[i];
      auto value = compile_expr(*arg);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      builder_.CreateStore(*value, slot_address(block, 1 + i));
    }
    return block;
  }

  /// Sum-type payload projection: well-defined only where a
  /// `hir_constructor_pattern` has already confirmed the runtime tag
  /// matches `variant_name` (see the node's own doc comment) — codegen
  /// just reads the corresponding payload slot without re-checking the tag.
  [[nodiscard]] auto
  compile_variant_payload(const hir::hir_variant_payload &node)
      -> std::expected<llvm::Value *, codegen_error> {
    const auto slot = runtime::sum_variant_payload_slots(
        types_, node.object->type, node.variant_name);
    if (!slot.has_value() || node.index >= *slot) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = node.span,
          .message = std::format(
              "variant `{}` does not resolve to a declared sum-type "
              "variant with a payload at index {} — this should have "
              "been rejected by the type checker",
              node.variant_name, node.index)});
    }
    auto object = compile_expr(*node.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    auto elem_ty = storage_type_for(node.type, node.span);
    if (!elem_ty.has_value()) {
      return std::unexpected(elem_ty.error());
    }
    return builder_.CreateLoad(*elem_ty, slot_address(*object, 1 + node.index));
  }

  // ------------------------------------------------------------------
  //  match + patterns (spec/codegen-design.md increment 4) — mirrors
  //  `bytecode_compiler`'s own pattern-testing scope exactly (see that
  //  file's doc comment on `compile_pattern_test` for the full rationale):
  //  every structural sub-test is safe to evaluate unconditionally (arity
  //  is already checker-verified), combined here via plain `CreateAnd`/
  //  `CreateOr` on `i1` rather than any short-circuiting. Struct field and
  //  sum-variant payload sub-patterns are only supported as a wildcard/
  //  binding — this increment doesn't resolve field/variant *types*, only
  //  their slot order (`runtime::layout.h`), and a non-wildcard sub-pattern
  //  there would need one to keep testing.
  // ------------------------------------------------------------------

  [[nodiscard]] auto compile_pattern_test(const hir::hir_pattern &pattern,
                                          llvm::Value *value,
                                          std::optional<type_id> value_type)
      -> std::expected<llvm::Value *, codegen_error> {
    switch (pattern.kind) {
    case hir_node_kind::hir_wildcard_pattern:
      return llvm::ConstantInt::getTrue(ctx_);
    case hir_node_kind::hir_literal_pattern: {
      const auto &lit = dynamic_cast<const hir::hir_literal_pattern &>(pattern);
      if (!value_type.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "a literal pattern here needs a statically tracked "
                       "subject type, which this position doesn't have "
                       "yet (see compile_pattern_test's doc comment)"});
      }
      if (lit.lit_kind == token_kind::string_lit) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "string literal patterns need increment 5's "
                       "growable-string comparison, not supported yet"});
      }
      auto kind = numeric_kind_for(*value_type, pattern.span);
      if (!kind.has_value()) {
        return std::unexpected(kind.error());
      }
      auto constant =
          compile_literal_value(lit.lit_kind, lit.value, pattern.span, *kind);
      if (!constant.has_value()) {
        return std::unexpected(constant.error());
      }
      return is_float(*kind)
                 ? builder_.CreateFCmpOEQ(value, *constant, "pat.eq")
                 : builder_.CreateICmpEQ(value, *constant, "pat.eq");
    }
    case hir_node_kind::hir_or_pattern: {
      const auto &or_pat = dynamic_cast<const hir::hir_or_pattern &>(pattern);
      if (or_pat.alternatives.empty()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "an `or` pattern needs at least one alternative"});
      }
      auto result =
          compile_pattern_test(*or_pat.alternatives[0], value, value_type);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      auto *acc = *result;
      for (size_t i = 1; i < or_pat.alternatives.size(); ++i) {
        auto alt =
            compile_pattern_test(*or_pat.alternatives[i], value, value_type);
        if (!alt.has_value()) {
          return std::unexpected(alt.error());
        }
        acc = builder_.CreateOr(acc, *alt, "pat.or");
      }
      return acc;
    }
    case hir_node_kind::hir_tuple_pattern: {
      const auto &tup = dynamic_cast<const hir::hir_tuple_pattern &>(pattern);
      if (!value_type.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "a tuple pattern here needs a statically tracked "
                       "subject type, which this position doesn't have yet"});
      }
      const auto &entry = types_.entry(*value_type);
      llvm::Value *acc = nullptr;
      for (size_t i = 0; i < tup.elements.size(); ++i) {
        const auto elem_type = i < entry.args.size()
                                   ? std::optional<type_id>(entry.args[i])
                                   : std::nullopt;
        auto elem_ty =
            elem_type.has_value()
                ? storage_type_for(*elem_type, pattern.span)
                : std::unexpected(codegen_error{
                      .kind = codegen_error_kind::unsupported_construct,
                      .span = pattern.span,
                      .message = "tuple pattern element has no "
                                 "tracked type"});
        if (!elem_ty.has_value()) {
          return std::unexpected(elem_ty.error());
        }
        auto *elem_val = builder_.CreateLoad(*elem_ty, slot_address(value, i));
        auto sub = compile_pattern_test(*tup.elements[i], elem_val, elem_type);
        if (!sub.has_value()) {
          return std::unexpected(sub.error());
        }
        acc = acc == nullptr ? *sub : builder_.CreateAnd(acc, *sub, "pat.and");
      }
      return acc == nullptr ? llvm::ConstantInt::getTrue(ctx_) : acc;
    }
    case hir_node_kind::hir_array_pattern: {
      const auto &arr = dynamic_cast<const hir::hir_array_pattern &>(pattern);
      if (!value_type.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "an array pattern here needs a statically tracked "
                       "subject type, which this position doesn't have yet"});
      }
      const auto &entry = types_.entry(*value_type);
      auto elem_ty = storage_type_for(entry.result, pattern.span);
      if (!elem_ty.has_value()) {
        return std::unexpected(elem_ty.error());
      }
      const auto elem_size = element_stride(entry.result);
      llvm::Value *acc = nullptr;
      for (std::size_t i = 0; i < arr.elements.size(); ++i) {
        const auto &element = arr.elements[i];
        auto *elem_val =
            builder_.CreateLoad(*elem_ty, byte_address(value, i * elem_size));
        auto sub = compile_pattern_test(*element, elem_val,
                                        std::optional<type_id>(entry.result));
        if (!sub.has_value()) {
          return std::unexpected(sub.error());
        }
        acc = acc == nullptr ? *sub : builder_.CreateAnd(acc, *sub, "pat.and");
      }
      return acc == nullptr ? llvm::ConstantInt::getTrue(ctx_) : acc;
    }
    case hir_node_kind::hir_struct_pattern: {
      const auto &st = dynamic_cast<const hir::hir_struct_pattern &>(pattern);
      for (const auto &field : st.fields) {
        if (field.pattern->kind != hir_node_kind::hir_wildcard_pattern) {
          return std::unexpected(codegen_error{
              .kind = codegen_error_kind::unsupported_construct,
              .span = pattern.span,
              .message = std::format(
                  "struct field pattern `{}` needs a non-wildcard "
                  "sub-pattern, which needs struct field type tracking "
                  "this increment doesn't support yet — only plain "
                  "bindings (`{{{}}}`) are supported for struct field "
                  "patterns",
                  field.name, field.name)});
        }
      }
      return llvm::ConstantInt::getTrue(ctx_);
    }
    case hir_node_kind::hir_constructor_pattern: {
      const auto &ctor =
          dynamic_cast<const hir::hir_constructor_pattern &>(pattern);
      if (!value_type.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "a constructor pattern here needs a statically "
                       "tracked subject type, which this position doesn't "
                       "have yet"});
      }
      const auto tag =
          runtime::sum_variant_tag(types_, *value_type, ctor.variant_name);
      if (!tag.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = std::format(
                "variant `{}` does not resolve to a declared sum-type "
                "variant — this should have been rejected by the type "
                "checker",
                ctor.variant_name)});
      }
      for (const auto &arg : ctor.args) {
        if (arg->kind != hir_node_kind::hir_wildcard_pattern) {
          return std::unexpected(codegen_error{
              .kind = codegen_error_kind::unsupported_construct,
              .span = pattern.span,
              .message = std::format(
                  "variant `{}`'s payload pattern needs a non-wildcard "
                  "sub-pattern, which needs sum-type payload type "
                  "tracking this increment doesn't support yet — only "
                  "plain bindings (`@{}(x)`) are supported for payload "
                  "patterns",
                  ctor.variant_name, ctor.variant_name)});
        }
      }
      auto *tag_val = builder_.CreateLoad(llvm::Type::getInt64Ty(ctx_),
                                          slot_address(value, size_t{0}));
      auto *tag_const = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_),
                                               static_cast<uint64_t>(*tag));
      return builder_.CreateICmpEQ(tag_val, tag_const, "pat.tag_eq");
    }
    case hir_node_kind::hir_range_pattern: {
      const auto &range = dynamic_cast<const hir::hir_range_pattern &>(pattern);
      if (!value_type.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "a range pattern here needs a statically tracked "
                       "subject type, which this position doesn't have yet"});
      }
      auto kind = numeric_kind_for(*value_type, pattern.span);
      if (!kind.has_value()) {
        return std::unexpected(kind.error());
      }
      const auto signed_int = is_signed_integer(*kind);
      llvm::Value *acc = nullptr;
      if (range.start != nullptr) {
        auto start = compile_expr(*range.start);
        if (!start.has_value()) {
          return std::unexpected(start.error());
        }
        acc = is_float(*kind)
                  ? builder_.CreateFCmpOGE(value, *start, "pat.range.ge")
              : signed_int
                  ? builder_.CreateICmpSGE(value, *start, "pat.range.ge")
                  : builder_.CreateICmpUGE(value, *start, "pat.range.ge");
      }
      if (range.end != nullptr) {
        auto end = compile_expr(*range.end);
        if (!end.has_value()) {
          return std::unexpected(end.error());
        }
        llvm::Value *cmp = nullptr;
        if (is_float(*kind)) {
          cmp = range.inclusive
                    ? builder_.CreateFCmpOLE(value, *end, "pat.range.le")
                    : builder_.CreateFCmpOLT(value, *end, "pat.range.lt");
        } else if (signed_int) {
          cmp = range.inclusive
                    ? builder_.CreateICmpSLE(value, *end, "pat.range.le")
                    : builder_.CreateICmpSLT(value, *end, "pat.range.lt");
        } else {
          cmp = range.inclusive
                    ? builder_.CreateICmpULE(value, *end, "pat.range.le")
                    : builder_.CreateICmpULT(value, *end, "pat.range.lt");
        }
        acc = acc == nullptr ? cmp : builder_.CreateAnd(acc, cmp, "pat.and");
      }
      return acc == nullptr ? llvm::ConstantInt::getTrue(ctx_) : acc;
    }
    default:
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = pattern.span,
          .message = "this pattern form is not supported by llvm_codegen "
                     "yet"});
    }
  }

  /// `match`, usable as either a statement (`want_result == nullptr`) or an
  /// expression (`want_result` is where each arm's value is stored) —
  /// mirrors `compile_if`'s own shape and control-flow style exactly. A
  /// trailing `guard_panic(true, ...)` guards the (checker-guaranteed-
  /// unreachable) case where no arm matched, the same defense-in-depth role
  /// it plays for array bounds checks.
  [[nodiscard]] auto compile_match(const hir::hir_match &match,
                                   llvm::AllocaInst *want_result)
      -> std::expected<bool, codegen_error> {
    auto subject = compile_expr(*match.subject);
    if (!subject.has_value()) {
      return std::unexpected(subject.error());
    }
    auto subject_ty = storage_type_for(match.subject->type, match.span);
    if (!subject_ty.has_value()) {
      return std::unexpected(subject_ty.error());
    }
    auto *subject_alloca = create_local_alloca(*subject_ty, "match.subject");
    builder_.CreateStore(*subject, subject_alloca);
    locals_.emplace(match.subject_symbol, subject_alloca);
    const auto subject_type = std::optional<type_id>(match.subject->type);

    auto *merge_bb = llvm::BasicBlock::Create(ctx_, "match.end", current_fn_);

    for (const auto &arm : match.arms) {
      auto test = compile_pattern_test(*arm.pattern, *subject, subject_type);
      if (!test.has_value()) {
        return std::unexpected(test.error());
      }
      auto *cond = *test;
      if (arm.guard != nullptr) {
        auto *guard_bb =
            llvm::BasicBlock::Create(ctx_, "match.guard", current_fn_);
        auto *skip_guard_bb =
            llvm::BasicBlock::Create(ctx_, "match.next", current_fn_);
        builder_.CreateCondBr(cond, guard_bb, skip_guard_bb);
        builder_.SetInsertPoint(guard_bb);
        auto guard_value = compile_expr(*arm.guard);
        if (!guard_value.has_value()) {
          return std::unexpected(guard_value.error());
        }
        auto *then_bb =
            llvm::BasicBlock::Create(ctx_, "match.then", current_fn_);
        builder_.CreateCondBr(*guard_value, then_bb, skip_guard_bb);

        builder_.SetInsertPoint(then_bb);
        auto terminated = compile_block_as_value(*arm.body, want_result);
        if (!terminated.has_value()) {
          return std::unexpected(terminated.error());
        }
        if (!*terminated) {
          builder_.CreateBr(merge_bb);
        }
        builder_.SetInsertPoint(skip_guard_bb);
        continue;
      }

      auto *then_bb = llvm::BasicBlock::Create(ctx_, "match.then", current_fn_);
      auto *next_bb = llvm::BasicBlock::Create(ctx_, "match.next", current_fn_);
      builder_.CreateCondBr(cond, then_bb, next_bb);

      builder_.SetInsertPoint(then_bb);
      auto terminated = compile_block_as_value(*arm.body, want_result);
      if (!terminated.has_value()) {
        return std::unexpected(terminated.error());
      }
      if (!*terminated) {
        builder_.CreateBr(merge_bb);
      }
      builder_.SetInsertPoint(next_bb);
    }

    // Exhaustiveness is checker-guaranteed; this is a defensive fallback,
    // not an expected runtime path. It unconditionally branches into
    // `merge_bb`, so `merge_bb` always has at least one predecessor and is
    // never actually unreachable.
    guard_panic(llvm::ConstantInt::getTrue(ctx_), panic_reason::explicit_panic);
    builder_.CreateBr(merge_bb);

    builder_.SetInsertPoint(merge_bb);
    return false;
  }

  // ------------------------------------------------------------------
  //  Statements and blocks. Every statement/block compiler returns whether
  //  it left the current block terminated (a `ret` was emitted, or an `if`
  //  where every branch terminated) — callers stop compiling further
  //  sibling statements once true, since anything after is unreachable and
  //  LLVM basic blocks may not contain instructions past a terminator.
  // ------------------------------------------------------------------

  [[nodiscard]] auto compile_block_as_value(const hir::hir_block &block,
                                            llvm::AllocaInst *want_result)
      -> std::expected<bool, codegen_error> {
    const auto &stmts = block.stmts;
    for (size_t i = 0; i < stmts.size(); ++i) {
      const auto &stmt = *stmts[i];
      const auto is_last = i + 1 == stmts.size();
      if (is_last && want_result != nullptr &&
          stmt.kind == hir_node_kind::hir_expr_stmt) {
        const auto &expr_stmt = dynamic_cast<const hir::hir_expr_stmt &>(stmt);
        auto value = compile_expr(*expr_stmt.expr);
        if (!value.has_value()) {
          return std::unexpected(value.error());
        }
        // See `compile_body_and_finish`'s identical check: a tail
        // `if`/`match`/block whose every branch/arm diverges already
        // terminated this block itself, so storing into `want_result` (and
        // reporting "not terminated" to the caller) would be wrong.
        if (builder_.GetInsertBlock()->getTerminator() != nullptr) {
          return true;
        }
        // A tail call whose declared result is `unit` (e.g. `println(...)`)
        // compiles to a `void`-returning `CreateCall` — there is no value to
        // store, and handing a `void` operand to `CreateStore` sends LLVM
        // spinning in `getABITypeAlign` on a type with no ABI size.
        // `want_result` is the `unit` storage slot (an `i64` placeholder, see
        // `storage_llvm_type`), so write the same zero placeholder the `unit`
        // literal uses; nothing ever reads it back out. `compile_body_and_finish`
        // dodges this by emitting `CreateRetVoid` instead of ever touching the
        // value, but a block-as-value tail has a real slot to populate.
        if ((*value)->getType()->isVoidTy()) {
          builder_.CreateStore(
              llvm::ConstantInt::get(want_result->getAllocatedType(), 0),
              want_result);
          return false;
        }
        builder_.CreateStore(*value, want_result);
        return false;
      }
      auto result = compile_stmt(stmt);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      if (*result) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] auto compile_stmt(const hir_node &node)
      -> std::expected<bool, codegen_error> {
    switch (node.kind) {
    case hir_node_kind::hir_let:
      return compile_let(dynamic_cast<const hir::hir_let &>(node));
    case hir_node_kind::hir_assign:
      return compile_assign(dynamic_cast<const hir::hir_assign &>(node));
    case hir_node_kind::hir_expr_stmt: {
      const auto &expr_stmt = dynamic_cast<const hir::hir_expr_stmt &>(node);
      auto result = compile_expr(*expr_stmt.expr);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      return false;
    }
    case hir_node_kind::hir_contract_check: {
      // The same shape as every other guard in this file: compute a
      // "should panic" bool — here, the contract condition *negated* — and
      // hand it to `guard_panic`, which branches to a call into the runtime
      // followed by `unreachable`. Control falls through to a fresh block on
      // the condition-held path, so this statement never terminates its
      // block.
      const auto &check = dynamic_cast<const hir::hir_contract_check &>(node);
      auto condition = compile_expr(*check.condition);
      if (!condition.has_value()) {
        return std::unexpected(condition.error());
      }
      guard_panic(builder_.CreateNot(*condition, "contract.violated"),
                  panic_reason_for(check.kind));
      return false;
    }
    case hir_node_kind::hir_return: {
      const auto &ret = dynamic_cast<const hir::hir_return &>(node);
      if (ret.value == nullptr) {
        // Inside a generator's step function, a bare `return` marks
        // exhaustion, not the ordinary no-value return — semantic analysis
        // already rejects `return <value>` inside a generator, so this is
        // the only `hir_return` shape reachable here while
        // `is_generator_step_`.
        if (is_generator_step_) {
          auto result = emit_generator_exhausted_return(ret.span);
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          return true;
        }
        builder_.CreateRetVoid();
        return true;
      }
      auto value = compile_expr(*ret.value);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      if (return_is_unit_) {
        builder_.CreateRetVoid();
      } else {
        builder_.CreateRet(*value);
      }
      return true;
    }
    case hir_node_kind::hir_yield: {
      const auto &y = dynamic_cast<const hir::hir_yield &>(node);
      if (!is_generator_step_ ||
          generator_next_yield_ordinal_ > generator_resume_blocks_.size()) {
        // A `yield` nested inside a lambda literal within a generator body
        // isn't part of the generator's own suspension protocol (a lambda
        // is its own call frame) — reject cleanly rather than indexing
        // past `generator_resume_blocks_`.
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = y.span,
            .message = "`yield` is only supported directly inside a "
                       "`generator def`'s own body, not inside a nested "
                       "lambda"});
      }
      auto value = compile_expr(*y.value);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      // Resync every generator-state symbol's current value back into the
      // state block immediately before suspending.
      for (size_t i = 0; i < generator_state_layout_.size(); ++i) {
        auto *alloca = lookup_local(generator_state_layout_[i]);
        if (alloca != nullptr) {
          auto *loaded = builder_.CreateLoad(alloca->getAllocatedType(), alloca,
                                             "state.sync");
          builder_.CreateStore(loaded, slot_address(generator_state_arg_, i));
        }
      }
      const auto ordinal = generator_next_yield_ordinal_++;
      builder_.CreateStore(
          llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), ordinal),
          slot_address(generator_self_arg_, size_t{2}));
      builder_.CreateRet(build_option_some(*value));

      // The landing block a resumed call jumps to — pre-created by
      // `compile_generator_step`'s dispatch chain; subsequent statements
      // compile into it, exactly like `compile_if`'s merge block.
      builder_.SetInsertPoint(generator_resume_blocks_[ordinal - 1]);
      return false;
    }
    case hir_node_kind::hir_while:
      return compile_while(dynamic_cast<const hir::hir_while &>(node));
    case hir_node_kind::hir_if: {
      auto terminated =
          compile_if(dynamic_cast<const hir::hir_if &>(node), nullptr);
      if (!terminated.has_value()) {
        return std::unexpected(terminated.error());
      }
      return *terminated;
    }
    case hir_node_kind::hir_match:
      return compile_match(dynamic_cast<const hir::hir_match &>(node), nullptr);
    case hir_node_kind::hir_while_let:
      return compile_while_let(dynamic_cast<const hir::hir_while_let &>(node));
    case hir_node_kind::hir_let_else:
      return compile_let_else(dynamic_cast<const hir::hir_let_else &>(node));
    case hir_node_kind::hir_list_push:
      return compile_list_push(dynamic_cast<const hir::hir_list_push &>(node));
    default:
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = node.span,
          .message = "this statement form is outside llvm_codegen's current "
                     "scope"});
    }
  }

  [[nodiscard]] auto compile_let(const hir::hir_let &let)
      -> std::expected<bool, codegen_error> {
    auto ty = storage_type_for(let.initializer->type, let.span);
    if (!ty.has_value()) {
      return std::unexpected(ty.error());
    }
    auto value = compile_expr(*let.initializer);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    // A generator step function's entry preloads an alloca for every
    // live-across-yield state symbol *before* compiling the body (so a
    // resumed call, which jumps straight past this `hir_let`, still has a
    // valid alloca to read) — when body compilation then reaches the
    // symbol's own `hir_let` on an actual (non-resumed) run, reuse that
    // same preloaded alloca instead of allocating a second one:
    // `locals_.emplace` silently no-ops on an already-present key, so
    // allocating fresh here would store the initializer's real value into
    // an alloca nothing ever reads, while every reference to the symbol
    // keeps resolving to the untouched (zero-initialized) preloaded one.
    auto *existing = lookup_local(let.symbol);
    auto *alloca =
        existing != nullptr ? existing : create_local_alloca(*ty, let.name);
    builder_.CreateStore(*value, alloca);
    if (existing == nullptr) {
      locals_.emplace(let.symbol, alloca);
    }
    return false;
  }

  [[nodiscard]] auto compile_assign(const hir::hir_assign &assign)
      -> std::expected<bool, codegen_error> {
    if (assign.target->kind == hir_node_kind::hir_field) {
      // `self.field = value` — mirrors `compile_field`'s own read through
      // `byte_address`, just a store instead of a load.
      const auto &field = dynamic_cast<const hir::hir_field &>(*assign.target);
      const auto offset = runtime::struct_field_offset(
          types_, field.object->type, field.field_name);
      if (!offset.has_value()) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = assign.span,
            .message = std::format(
                "field `.{}` is only assignable on struct values by "
                "llvm_codegen yet",
                field.field_name)});
      }
      if (assign.op != ast::assign_op::assign) {
        return std::unexpected(codegen_error{
            .kind = codegen_error_kind::unsupported_construct,
            .span = assign.span,
            .message = "compound assignment to a struct field is not "
                       "supported by llvm_codegen yet — only plain `=` is"});
      }
      auto object = compile_expr(*field.object);
      if (!object.has_value()) {
        return std::unexpected(object.error());
      }
      auto value = compile_expr(*assign.value);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      builder_.CreateStore(*value, byte_address(*object, *offset));
      return false;
    }
    if (assign.target->kind != hir_node_kind::hir_local_ref) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = assign.span,
          .message = "assigning to an index target needs a heap/aggregate "
                     "representation llvm_codegen doesn't have yet — only "
                     "assigning directly to a local variable or a struct "
                     "field is supported"});
    }
    const auto &target =
        dynamic_cast<const hir::hir_local_ref &>(*assign.target);
    auto *alloca = lookup_local(target.symbol);
    if (alloca == nullptr) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = assign.span,
          .message = std::format("assignment target `{}` does not resolve "
                                 "to a local variable",
                                 target.name)});
    }
    if (assign.op == ast::assign_op::assign) {
      auto value = compile_expr(*assign.value);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      builder_.CreateStore(*value, alloca);
      return false;
    }

    auto kind = numeric_kind_for(target.type, assign.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    auto rhs = compile_expr(*assign.value);
    if (!rhs.has_value()) {
      return std::unexpected(rhs.error());
    }
    auto *current =
        builder_.CreateLoad(alloca->getAllocatedType(), alloca, target.name);
    auto new_value = compound_assign_value(assign, *kind, current, *rhs);
    if (!new_value.has_value()) {
      return std::unexpected(new_value.error());
    }
    builder_.CreateStore(*new_value, alloca);
    return false;
  }

  [[nodiscard]] auto
  compound_assign_value(const hir::hir_assign &assign, numeric_kind kind,
                        llvm::Value *current, llvm::Value *rhs)
      -> std::expected<llvm::Value *, codegen_error> {
    using ast::assign_op;
    ast::binary_op equivalent{};
    switch (assign.op) {
    case assign_op::add_assign:
      equivalent = ast::binary_op::add;
      break;
    case assign_op::sub_assign:
      equivalent = ast::binary_op::sub;
      break;
    case assign_op::mul_assign:
      equivalent = ast::binary_op::mul;
      break;
    case assign_op::div_assign:
      equivalent = ast::binary_op::div;
      break;
    case assign_op::mod_assign:
      equivalent = ast::binary_op::mod;
      break;
    case assign_op::and_assign:
      equivalent = ast::binary_op::bit_and;
      break;
    case assign_op::or_assign:
      equivalent = ast::binary_op::bit_or;
      break;
    case assign_op::xor_assign:
      equivalent = ast::binary_op::bit_xor;
      break;
    case assign_op::shl_assign:
      equivalent = ast::binary_op::shl;
      break;
    case assign_op::shr_assign:
      equivalent = ast::binary_op::shr;
      break;
    case assign_op::add_wrap_assign:
      equivalent = ast::binary_op::add_wrap;
      break;
    case assign_op::sub_wrap_assign:
      equivalent = ast::binary_op::sub_wrap;
      break;
    case assign_op::mul_wrap_assign:
      equivalent = ast::binary_op::mul_wrap;
      break;
    case assign_op::add_sat_assign:
      equivalent = ast::binary_op::add_sat;
      break;
    case assign_op::sub_sat_assign:
      equivalent = ast::binary_op::sub_sat;
      break;
    default:
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = assign.span,
          .message = "this compound assignment operator is not supported "
                     "by llvm_codegen yet"});
    }
    // `compile_binary_op` only reads `bin.op`/`bin.span` off the synthetic
    // node below — building one avoids a second copy of the same opcode
    // dispatch table `compile_binary_op` already has.
    auto synthetic = hir::hir_binary(assign.span, hir::k_unknown_type,
                                     equivalent, nullptr, nullptr);
    return compile_binary_op(synthetic, kind, current, rhs);
  }

  /// Whether `expr` is the literal `true` — `while true: ...` is Kira's
  /// only spelling for an unconditional loop, since the language has no
  /// `break`/`continue` (no `hir_break`/`hir_continue` node kind exists at
  /// all: see `src/hir/nodes.h`'s doc comment on this). That means such a
  /// loop's only possible exit is an internal `return`/panic inside its own
  /// body — its condition can never itself become false, so the `end_bb`
  /// `compile_while` would otherwise leave open for a "falls through"
  /// caller is genuinely, statically unreachable, not just dynamically
  /// unlikely.
  [[nodiscard]] auto is_literal_true(const hir::hir_expr &expr) const -> bool {
    return expr.kind == hir_node_kind::hir_literal &&
           dynamic_cast<const hir::hir_literal &>(expr).lit_kind ==
               token_kind::kw_true;
  }

  [[nodiscard]] auto compile_while(const hir::hir_while &loop)
      -> std::expected<bool, codegen_error> {
    const auto is_infinite =
        loop.condition != nullptr && is_literal_true(*loop.condition);
    auto *cond_bb = llvm::BasicBlock::Create(ctx_, "while.cond", current_fn_);
    auto *body_bb = llvm::BasicBlock::Create(ctx_, "while.body", current_fn_);
    auto *end_bb = llvm::BasicBlock::Create(ctx_, "while.end", current_fn_);

    builder_.CreateBr(cond_bb);
    builder_.SetInsertPoint(cond_bb);
    if (is_infinite) {
      builder_.CreateBr(body_bb);
    } else {
      auto cond = compile_expr(*loop.condition);
      if (!cond.has_value()) {
        return std::unexpected(cond.error());
      }
      builder_.CreateCondBr(*cond, body_bb, end_bb);
    }

    builder_.SetInsertPoint(body_bb);
    auto terminated = compile_block_as_value(*loop.body, nullptr);
    if (!terminated.has_value()) {
      return std::unexpected(terminated.error());
    }
    if (!*terminated) {
      builder_.CreateBr(cond_bb);
    }

    builder_.SetInsertPoint(end_bb);
    if (is_infinite) {
      // Nothing ever branches here (no `break`, and the condition can never
      // itself become false) — mark it unreachable rather than leaving an
      // open block for a caller to plant a "falls through" return in, which
      // would emit a `ret` whose value/type can't agree with the function's
      // real return type (this loop's exits are all real `return`s inside
      // its own body, already correctly typed).
      builder_.CreateUnreachable();
      return true;
    }
    return false;
  }

  /// `while let pattern = expr: body` — like `compile_while`, but the loop
  /// condition is a fresh pattern test against a freshly re-evaluated
  /// `subject` every iteration (see `hir_while_let`'s doc comment) rather
  /// than a plain boolean expression; falls out of the loop the first time
  /// the pattern fails to match, with no `else`.
  [[nodiscard]] auto compile_while_let(const hir::hir_while_let &loop)
      -> std::expected<bool, codegen_error> {
    auto subject_ty = storage_type_for(loop.subject->type, loop.span);
    if (!subject_ty.has_value()) {
      return std::unexpected(subject_ty.error());
    }
    auto *subject_alloca =
        create_local_alloca(*subject_ty, "while_let.subject");
    locals_.emplace(loop.subject_symbol, subject_alloca);
    const auto subject_type = std::optional<type_id>(loop.subject->type);

    auto *cond_bb =
        llvm::BasicBlock::Create(ctx_, "while_let.cond", current_fn_);
    auto *body_bb =
        llvm::BasicBlock::Create(ctx_, "while_let.body", current_fn_);
    auto *end_bb = llvm::BasicBlock::Create(ctx_, "while_let.end", current_fn_);

    builder_.CreateBr(cond_bb);
    builder_.SetInsertPoint(cond_bb);
    auto subject_value = compile_expr(*loop.subject);
    if (!subject_value.has_value()) {
      return std::unexpected(subject_value.error());
    }
    builder_.CreateStore(*subject_value, subject_alloca);
    auto test =
        compile_pattern_test(*loop.pattern, *subject_value, subject_type);
    if (!test.has_value()) {
      return std::unexpected(test.error());
    }
    builder_.CreateCondBr(*test, body_bb, end_bb);

    builder_.SetInsertPoint(body_bb);
    auto terminated = compile_block_as_value(*loop.body, nullptr);
    if (!terminated.has_value()) {
      return std::unexpected(terminated.error());
    }
    if (!*terminated) {
      builder_.CreateBr(cond_bb);
    }

    builder_.SetInsertPoint(end_bb);
    return false;
  }

  /// `let pattern = expr else: block` — evaluates `initializer` into
  /// `subject_symbol` once, tests it against `pattern`, and runs
  /// `else_body` when it fails to match; `pattern`'s own bindings are
  /// ordinary `hir_let`s lowering already spliced in immediately after this
  /// node (see `hir_let_else`'s doc comment), so this only has to compile
  /// the test and the branch, not any binding of its own. `else_body` must
  /// diverge — a language-level invariant the checker enforces; the
  /// trailing `guard_panic` is a defensive fallback for that invariant,
  /// mirroring `compile_match`'s own "no arm matched" guard.
  [[nodiscard]] auto compile_let_else(const hir::hir_let_else &node)
      -> std::expected<bool, codegen_error> {
    auto subject_ty = storage_type_for(node.initializer->type, node.span);
    if (!subject_ty.has_value()) {
      return std::unexpected(subject_ty.error());
    }
    auto value = compile_expr(*node.initializer);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    auto *subject_alloca = create_local_alloca(*subject_ty, "let_else.subject");
    builder_.CreateStore(*value, subject_alloca);
    locals_.emplace(node.subject_symbol, subject_alloca);
    const auto subject_type = std::optional<type_id>(node.initializer->type);

    auto test = compile_pattern_test(*node.pattern, *value, subject_type);
    if (!test.has_value()) {
      return std::unexpected(test.error());
    }

    auto *else_bb =
        llvm::BasicBlock::Create(ctx_, "let_else.else", current_fn_);
    auto *continue_bb =
        llvm::BasicBlock::Create(ctx_, "let_else.continue", current_fn_);
    builder_.CreateCondBr(*test, continue_bb, else_bb);

    builder_.SetInsertPoint(else_bb);
    auto terminated = compile_block_as_value(*node.else_body, nullptr);
    if (!terminated.has_value()) {
      return std::unexpected(terminated.error());
    }
    if (!*terminated) {
      guard_panic(llvm::ConstantInt::getTrue(ctx_),
                  panic_reason::explicit_panic);
      builder_.CreateBr(continue_bb);
    }

    builder_.SetInsertPoint(continue_bb);
    return false;
  }

  /// Appends `value` onto the `list[T]` place `target` (see
  /// `hir_list_push`'s doc comment) — the comprehension accumulator's
  /// growth statement.
  [[nodiscard]] auto compile_list_push(const hir::hir_list_push &node)
      -> std::expected<bool, codegen_error> {
    auto target = compile_expr(*node.target);
    if (!target.has_value()) {
      return std::unexpected(target.error());
    }
    auto value = compile_expr(*node.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    const auto elem_size = element_stride(node.value->type);
    auto *slot = builder_.CreateCall(
        list_reserve_slot_fn_,
        {*target,
         llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), elem_size)});
    builder_.CreateStore(*value, slot);
    return false;
  }

  llvm::LLVMContext &ctx_;
  const type_table &types_;
  const std::unordered_map<std::string, llvm::Function *> &functions_;
  llvm::Function *panic_fn_;
  llvm::Function *alloc_fn_;
  llvm::Function *list_reserve_slot_fn_;
  std::array<llvm::Function *, 23> intrinsic_fns_;
  std::string entry_module_name_;
  std::string current_module_name_;
  llvm::IRBuilder<> builder_;
  llvm::Function *current_fn_ = nullptr;
  llvm::AllocaInst *alloca_marker_ = nullptr;
  std::unordered_map<hir::symbol_id, llvm::AllocaInst *> locals_;
  bool return_is_unit_ = false;
  llvm::Type *return_llvm_type_ = nullptr;

  // ------------------------------------------------------------------
  //  Generator step-function state — only meaningful while
  //  `is_generator_step_` (set by `compile_generator_step`). Mirrors
  //  `bytecode_compiler::function_compiler`'s identically-named fields;
  //  `generator_resume_blocks_[k-1]` is the landing `llvm::BasicBlock*` for
  //  the `k`th `hir_yield`, created up front by the resume-dispatch chain
  //  and `SetInsertPoint`-ed into right after that yield's suspend sequence.
  // ------------------------------------------------------------------
  bool is_generator_step_ = false;
  llvm::Value *generator_state_arg_ = nullptr;
  llvm::Value *generator_self_arg_ = nullptr;
  std::vector<hir::symbol_id> generator_state_layout_;
  std::vector<llvm::BasicBlock *> generator_resume_blocks_;
  size_t generator_next_yield_ordinal_ = 1;
};

} // namespace

auto compile_module(std::span<const hir::hir_module *const> modules,
                    const type_table &types)
    -> std::expected<compiled_module, codegen_error> {
  const auto &entry_name = modules.front()->module_name;

  auto result = compiled_module{
      .context = std::make_unique<llvm::LLVMContext>(),
      .module = nullptr,
  };
  result.module = std::make_unique<llvm::Module>(entry_name, *result.context);
  auto &ctx = *result.context;
  auto &llvm_module = *result.module;

  auto *panic_fn = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(ctx),
                              {llvm::Type::getInt8Ty(ctx)},
                              /*isVarArg=*/false),
      llvm::Function::ExternalLinkage, kPanicSymbolName, llvm_module);
  panic_fn->setDoesNotReturn();

  auto *alloc_fn = llvm::Function::Create(
      llvm::FunctionType::get(llvm::PointerType::get(ctx, 0),
                              {llvm::Type::getInt64Ty(ctx)},
                              /*isVarArg=*/false),
      llvm::Function::ExternalLinkage, kAllocSymbolName, llvm_module);

  // `runtime::list_reserve_slot`/`kira_rt_list_reserve_slot` (layout.h) grew
  // an `elem_size` parameter to generalize a `list[T]`'s push stride beyond
  // a hardcoded 8 bytes/element — `function_compiler::compile_list_init`/
  // `compile_list_push` (below) now pass each call site's own
  // `element_stride`-computed value, matching the bytecode tier's own
  // natural-stride `list[T]` storage. The parameter still has to be declared
  // here to keep the real native function's ABI (2 args, not 1) correct
  // across the JIT/AOT call boundary.
  auto *list_reserve_slot_fn = llvm::Function::Create(
      llvm::FunctionType::get(
          llvm::PointerType::get(ctx, 0),
          {llvm::PointerType::get(ctx, 0), llvm::Type::getInt64Ty(ctx)},
          /*isVarArg=*/false),
      llvm::Function::ExternalLinkage, kListReserveSlotSymbolName, llvm_module);

  // `intrinsic def` declarations (src/intrinsics.h): fixed native entry
  // points, each taking/returning opaque heap pointers (see
  // src/runtime/io.h's and src/runtime/fmt.h's doc comments for the exact
  // layout each argument's Kira type maps to). Declared once here, the same
  // way the three runtime externs just above are, and resolved the same way
  // (JIT: process-symbol lookup against `//src/runtime:runtime`, which now
  // also builds `io.cpp`/`fmt.cpp`; AOT: ordinary static linking against
  // that same archive).
  auto *ptr_ty = llvm::PointerType::get(ctx, 0);
  auto intrinsic_fns = std::array<llvm::Function *, 23>{};
  for (size_t i = 0; i < kira::known_intrinsic_names.size(); ++i) {
    auto param_types =
        std::vector<llvm::Type *>(kira::known_intrinsic_arities[i], ptr_ty);
    auto *fn_type =
        llvm::FunctionType::get(ptr_ty, param_types, /*isVarArg=*/false);
    const auto symbol_name =
        std::format("kira_{}", kira::known_intrinsic_names[i]);
    intrinsic_fns[i] = llvm::Function::Create(
        fn_type, llvm::Function::ExternalLinkage, symbol_name, llvm_module);
  }

  // Every function's signature is declared before any body is compiled, so
  // a call (forward-referenced, recursive, or mutually recursive) always
  // finds a real `llvm::Function*` — see codegen.h's doc comment. Keyed the
  // same way `bytecode_compiler::compile_module`'s span overload keys its
  // own function table: bare for the entry module, `module::name` for
  // everything else.
  auto functions = std::unordered_map<std::string, llvm::Function *>{};
  auto ordered_functions = std::vector<
      std::pair<const hir::hir_module *, const hir::hir_function *>>{};
  for (const auto *module : modules) {
    for (const auto &fn : module->functions) {
      auto param_types = std::vector<llvm::Type *>{};
      param_types.reserve(fn->params.size());
      for (const auto &param : fn->params) {
        const auto ty = storage_llvm_type(types, ctx, param.type);
        if (!ty.has_value()) {
          return std::unexpected(codegen_error{
              .kind = codegen_error_kind::unsupported_type,
              .span = fn->span,
              .message = std::format("parameter `{}` of `{}` has no scalar or "
                                     "heap-value llvm_codegen representation "
                                     "yet",
                                     param.name, fn->name)});
        }
        param_types.push_back(*ty);
      }

      llvm::Type *return_ty = nullptr;
      if (types.is_unit(fn->return_type)) {
        return_ty = llvm::Type::getVoidTy(ctx);
      } else {
        const auto ty = storage_llvm_type(types, ctx, fn->return_type);
        if (!ty.has_value()) {
          return std::unexpected(codegen_error{
              .kind = codegen_error_kind::unsupported_type,
              .span = fn->span,
              .message = std::format("return type of `{}` has no scalar or "
                                     "heap-value llvm_codegen representation "
                                     "yet",
                                     fn->name)});
        }
        return_ty = *ty;
      }

      const auto key = module->module_name == entry_name
                           ? fn->name
                           : module->module_name + "::" + fn->name;
      auto *fn_type = llvm::FunctionType::get(return_ty, param_types,
                                              /*isVarArg=*/false);
      auto *llvm_fn = llvm::Function::Create(
          fn_type, llvm::Function::ExternalLinkage, key, llvm_module);
      functions.emplace(key, llvm_fn);
      ordered_functions.emplace_back(module, fn.get());
    }
  }

  for (const auto &[module, fn] : ordered_functions) {
    const auto key = module->module_name == entry_name
                         ? fn->name
                         : module->module_name + "::" + fn->name;
    auto *llvm_fn = functions.at(key);
    auto compiler = function_compiler(ctx, types, functions, panic_fn, alloc_fn,
                                      list_reserve_slot_fn, intrinsic_fns,
                                      entry_name, module->module_name);
    auto compiled = compiler.compile(*fn, llvm_fn);
    if (!compiled.has_value()) {
      return std::unexpected(compiled.error());
    }
  }

  auto verify_message = std::string{};
  auto verify_stream = llvm::raw_string_ostream(verify_message);
  if (llvm::verifyModule(llvm_module, &verify_stream)) {
    return std::unexpected(codegen_error{
        .kind = codegen_error_kind::unsupported_construct,
        .span = modules.front()->span,
        .message =
            std::format("llvm_codegen produced a module that failed "
                        "verification (this is a codegen bug, not a source "
                        "error): {}",
                        verify_message)});
  }

  return result;
}

auto compile_module(const hir::hir_module &module, const type_table &types)
    -> std::expected<compiled_module, codegen_error> {
  const auto *module_ptr = &module;
  return compile_module(std::span<const hir::hir_module *const>(&module_ptr, 1),
                        types);
}

auto parse_optimization_level(std::string_view text)
    -> std::optional<optimization_level> {
  if (text == "0") {
    return optimization_level::o0;
  }
  if (text == "1") {
    return optimization_level::o1;
  }
  if (text == "2") {
    return optimization_level::o2;
  }
  if (text == "3") {
    return optimization_level::o3;
  }
  return std::nullopt;
}

auto optimize_module(llvm::Module &module, optimization_level level) -> void {
  if (level == optimization_level::o0) {
    // Matches `compile_module`'s own pre-existing behavior exactly (no
    // passes ever ran before this function existed) — `o0` stays a true
    // no-op rather than running an "empty" pipeline, so a `-O0` build's IR
    // is byte-for-byte what `compile_module` alone produces.
    return;
  }
  auto llvm_level = llvm::OptimizationLevel::O1;
  switch (level) {
  case optimization_level::o2:
    llvm_level = llvm::OptimizationLevel::O2;
    break;
  case optimization_level::o3:
    llvm_level = llvm::OptimizationLevel::O3;
    break;
  default:
    break;
  }

  // Standard `llvm::PassBuilder` setup for the new pass manager — the same
  // analysis-manager wiring every out-of-tree LLVM frontend uses to run its
  // default per-module optimization pipeline (mirrors `clang -O2`'s own
  // pipeline construction, just without clang's extra frontend-specific
  // pass customization, which this teaching compiler has no need for).
  auto loop_analysis_manager = llvm::LoopAnalysisManager{};
  auto function_analysis_manager = llvm::FunctionAnalysisManager{};
  auto cgscc_analysis_manager = llvm::CGSCCAnalysisManager{};
  auto module_analysis_manager = llvm::ModuleAnalysisManager{};

  auto pass_builder = llvm::PassBuilder{};
  pass_builder.registerModuleAnalyses(module_analysis_manager);
  pass_builder.registerCGSCCAnalyses(cgscc_analysis_manager);
  pass_builder.registerFunctionAnalyses(function_analysis_manager);
  pass_builder.registerLoopAnalyses(loop_analysis_manager);
  pass_builder.crossRegisterProxies(
      loop_analysis_manager, function_analysis_manager, cgscc_analysis_manager,
      module_analysis_manager);

  auto module_pass_manager =
      pass_builder.buildPerModuleDefaultPipeline(llvm_level);
  module_pass_manager.run(module, module_analysis_manager);
}

} // namespace kira::llvm_codegen
