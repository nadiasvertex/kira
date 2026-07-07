#include "src/llvm_codegen/codegen.h"

#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/hir/ids.h"
#include "src/parser/ast.h"
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

/// Appends `scalar`'s UTF-8 encoding to `out` — duplicated from
/// `bytecode_compiler/compile.cpp`'s own `encode_utf8_scalar`, same
/// duplication precedent as `decode_char_literal` above.
auto encode_utf8_scalar(uint32_t scalar, std::string &out) -> void {
  if (scalar < 0x80) {
    out.push_back(static_cast<char>(scalar));
  } else if (scalar < 0x800) {
    out.push_back(static_cast<char>(0xC0U | (scalar >> 6)));
    out.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
  } else if (scalar < 0x10000) {
    out.push_back(static_cast<char>(0xE0U | (scalar >> 12)));
    out.push_back(static_cast<char>(0x80U | ((scalar >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (scalar >> 18)));
    out.push_back(static_cast<char>(0x80U | ((scalar >> 12) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((scalar >> 6) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (scalar & 0x3FU)));
  }
}

/// Decodes a `string_lit` token's raw text into UTF-8 byte content —
/// duplicated from `bytecode_compiler/compile.cpp`'s `decode_string_literal`
/// (see its doc comment for why interpolation isn't handled here either).
[[nodiscard]] auto decode_string_literal(std::string_view text)
    -> std::optional<std::string> {
  if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
    return std::nullopt;
  }
  const auto inner = text.substr(1, text.size() - 2);
  auto out = std::string{};
  out.reserve(inner.size());
  size_t pos = 0;
  while (pos < inner.size()) {
    if (inner[pos] != '\\') {
      out.push_back(inner[pos]);
      ++pos;
      continue;
    }
    if (pos + 1 >= inner.size()) {
      return std::nullopt;
    }
    switch (inner[pos + 1]) {
    case 'n':
      out.push_back('\n');
      pos += 2;
      break;
    case 't':
      out.push_back('\t');
      pos += 2;
      break;
    case 'r':
      out.push_back('\r');
      pos += 2;
      break;
    case '"':
      out.push_back('"');
      pos += 2;
      break;
    case '\'':
      out.push_back('\'');
      pos += 2;
      break;
    case '\\':
      out.push_back('\\');
      pos += 2;
      break;
    case '{':
      out.push_back('{');
      pos += 2;
      break;
    case '}':
      out.push_back('}');
      pos += 2;
      break;
    case 'u': {
      if (pos + 2 >= inner.size() || inner[pos + 2] != '{') {
        return std::nullopt;
      }
      const auto close = inner.find('}', pos + 3);
      if (close == std::string_view::npos) {
        return std::nullopt;
      }
      const auto hex = inner.substr(pos + 3, close - (pos + 3));
      auto value = uint32_t{0};
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
      const char *hex_end = hex.data() + hex.size();
      const auto result = std::from_chars(hex.data(), hex_end, value, 16);
      // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
      if (result.ec != std::errc{} || result.ptr != hex_end) {
        return std::nullopt;
      }
      encode_utf8_scalar(value, out);
      pos = close + 1;
      break;
    }
    default:
      return std::nullopt;
    }
  }
  return out;
}

/// Whether `id` is one of the heap-backed representations
/// `src/runtime/layout.h` describes (`str`, `list`/`option`/`result` and
/// other prelude generics, tuple, fixed array, struct, sum type) — every
/// such value is a single opaque pointer, as opposed to the closed scalar
/// set `numeric_kind_of` maps directly to an LLVM integer/float type.
[[nodiscard]] auto is_heap_type(const type_table &types, type_id id) -> bool {
  const auto &entry = types.entry(id);
  switch (entry.kind) {
  case semantic::type_kind::tuple_kind:
  case semantic::type_kind::array_kind:
  case semantic::type_kind::struct_kind:
  case semantic::type_kind::sum_kind:
  case semantic::type_kind::builtin_generic_kind:
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
  const auto kind = numeric_kind_of(types, id);
  if (kind.has_value()) {
    return llvm_type_for(ctx, *kind);
  }
  if (is_heap_type(types, id)) {
    return llvm::PointerType::get(ctx, 0);
  }
  return std::nullopt;
}

// ==========================================================================
//  module_compiler — declares every function's signature up front (so
//  direct/recursive/mutually-recursive calls always find a real
//  llvm::Function*), then compiles each body.
// ==========================================================================

class function_compiler {
public:
  function_compiler(
      llvm::LLVMContext &ctx, const type_table &types,
      const std::unordered_map<std::string, llvm::Function *> &functions,
      llvm::Function *panic_fn, llvm::Function *alloc_fn)
      : ctx_(ctx), types_(types), functions_(functions), panic_fn_(panic_fn),
        alloc_fn_(alloc_fn), builder_(ctx) {}

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

    const auto &stmts = fn.body->stmts;
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
          if (return_is_unit_) {
            auto value = compile_expr(*expr_stmt.expr);
            if (!value.has_value()) {
              return std::unexpected(value.error());
            }
            builder_.CreateRetVoid();
          } else {
            auto value = compile_expr(*expr_stmt.expr);
            if (!value.has_value()) {
              return std::unexpected(value.error());
            }
            builder_.CreateRet(*value);
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

private:
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

  // ------------------------------------------------------------------
  //  Panics — every checked-arithmetic guard funnels through this, so a
  //  panic always looks like: compute a bool "should panic" condition,
  //  branch to a block that calls the runtime and terminates with
  //  `unreachable`, otherwise fall through to a fresh block that carries on
  //  — mirroring `bytecode::vm`'s `throw panic_error(...)` sites exactly
  //  (same closed `panic_reason` set, same trigger conditions), just
  //  reified as control flow instead of a C++ exception.
  // ------------------------------------------------------------------

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
    case hir_node_kind::hir_if: {
      const auto &node = dynamic_cast<const hir::hir_if &>(expr);
      auto kind = numeric_kind_for(expr.type, expr.span);
      if (!kind.has_value()) {
        return std::unexpected(kind.error());
      }
      auto *result =
          create_local_alloca(llvm_type_for(ctx_, *kind), "if.result");
      auto terminated = compile_if(node, result);
      if (!terminated.has_value()) {
        return std::unexpected(terminated.error());
      }
      // Scope limitation, documented rather than silently assumed: this
      // spike does not thread "never" (all-branches-diverge) typing through
      // expression position the way `hir_if`-as-statement can. Every stress
      // corpus program keeps an if-expression's branches value-producing,
      // not diverging, so `result` is always written before this load runs.
      return builder_.CreateLoad(result->getAllocatedType(), result,
                                 "if.value");
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
      // See hir_if's own scope note just above: every stress corpus match
      // expression keeps every arm value-producing, not diverging.
      return builder_.CreateLoad(*ty, result, "match.value");
    }
    default:
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = expr.span,
          .message =
              "this expression form is outside llvm_codegen's current scope "
              "— lambdas still need work spec/codegen-design.md's later "
              "increments haven't landed yet"});
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
    auto kind = numeric_kind_for(lit.type, lit.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    return compile_literal_value(lit.lit_kind, lit.value, lit.span, *kind);
  }

  [[nodiscard]] auto compile_binary(const hir::hir_binary &bin)
      -> std::expected<llvm::Value *, codegen_error> {
    if (bin.op == ast::binary_op::And || bin.op == ast::binary_op::Or) {
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
    if (bin.op == ast::binary_op::And) {
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
    case binary_op::EqEq:
      return is_float(kind) ? builder_.CreateFCmpOEQ(lhs, rhs)
                            : builder_.CreateICmpEQ(lhs, rhs);
    case binary_op::BangEq:
      return is_float(kind) ? builder_.CreateFCmpONE(lhs, rhs)
                            : builder_.CreateICmpNE(lhs, rhs);
    case binary_op::Lt:
      if (is_float(kind)) {
        return builder_.CreateFCmpOLT(lhs, rhs);
      }
      return signed_kind ? builder_.CreateICmpSLT(lhs, rhs)
                         : builder_.CreateICmpULT(lhs, rhs);
    case binary_op::LtEq:
      if (is_float(kind)) {
        return builder_.CreateFCmpOLE(lhs, rhs);
      }
      return signed_kind ? builder_.CreateICmpSLE(lhs, rhs)
                         : builder_.CreateICmpULE(lhs, rhs);
    case binary_op::Gt:
      if (is_float(kind)) {
        return builder_.CreateFCmpOGT(lhs, rhs);
      }
      return signed_kind ? builder_.CreateICmpSGT(lhs, rhs)
                         : builder_.CreateICmpUGT(lhs, rhs);
    case binary_op::GtEq:
      if (is_float(kind)) {
        return builder_.CreateFCmpOGE(lhs, rhs);
      }
      return signed_kind ? builder_.CreateICmpSGE(lhs, rhs)
                         : builder_.CreateICmpUGE(lhs, rhs);
    case binary_op::Add:
      if (is_float(kind)) {
        return builder_.CreateFAdd(lhs, rhs);
      }
      return checked_arith(signed_kind ? llvm::Intrinsic::sadd_with_overflow
                                       : llvm::Intrinsic::uadd_with_overflow,
                           lhs, rhs);
    case binary_op::Sub:
      if (is_float(kind)) {
        return builder_.CreateFSub(lhs, rhs);
      }
      return checked_arith(signed_kind ? llvm::Intrinsic::ssub_with_overflow
                                       : llvm::Intrinsic::usub_with_overflow,
                           lhs, rhs);
    case binary_op::Mul:
      if (is_float(kind)) {
        return builder_.CreateFMul(lhs, rhs);
      }
      return checked_arith(signed_kind ? llvm::Intrinsic::smul_with_overflow
                                       : llvm::Intrinsic::umul_with_overflow,
                           lhs, rhs);
    case binary_op::Div:
      if (is_float(kind)) {
        return builder_.CreateFDiv(lhs, rhs);
      }
      return checked_div(signed_kind, bits, lhs, rhs, /*is_mod=*/false);
    case binary_op::Mod:
      if (is_float(kind)) {
        return builder_.CreateFRem(lhs, rhs);
      }
      return checked_div(signed_kind, bits, lhs, rhs, /*is_mod=*/true);
    case binary_op::AddWrap:
      return builder_.CreateAdd(lhs, rhs);
    case binary_op::SubWrap:
      return builder_.CreateSub(lhs, rhs);
    case binary_op::MulWrap:
      return builder_.CreateMul(lhs, rhs);
    case binary_op::AddSat:
      return builder_.CreateBinaryIntrinsic(
          signed_kind ? llvm::Intrinsic::sadd_sat : llvm::Intrinsic::uadd_sat,
          lhs, rhs);
    case binary_op::SubSat:
      return builder_.CreateBinaryIntrinsic(
          signed_kind ? llvm::Intrinsic::ssub_sat : llvm::Intrinsic::usub_sat,
          lhs, rhs);
    case binary_op::Shl:
    case binary_op::Shr:
      return checked_shift(bin.op == binary_op::Shl, signed_kind, bits, lhs,
                           rhs);
    case binary_op::BitAnd:
      return builder_.CreateAnd(lhs, rhs);
    case binary_op::BitOr:
      return builder_.CreateOr(lhs, rhs);
    case binary_op::BitXor:
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

  [[nodiscard]] auto compile_unary(const hir::hir_unary &un)
      -> std::expected<llvm::Value *, codegen_error> {
    if (un.op == ast::unary_op::Not) {
      auto src = compile_expr(*un.operand);
      if (!src.has_value()) {
        return std::unexpected(src.error());
      }
      return builder_.CreateNot(*src);
    }
    if (un.op != ast::unary_op::Neg && un.op != ast::unary_op::BitNot) {
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
    if (un.op == ast::unary_op::BitNot) {
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

  [[nodiscard]] auto compile_call(const hir::hir_call &call)
      -> std::expected<llvm::Value *, codegen_error> {
    if (call.callee->kind != hir_node_kind::hir_local_ref) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = call.span,
          .message = "only direct calls to a named function are supported "
                     "yet — calling through a computed callee expression is "
                     "not"});
    }
    const auto &ref = dynamic_cast<const hir::hir_local_ref &>(*call.callee);
    if (lookup_local(ref.symbol) != nullptr) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = call.span,
          .message = std::format(
              "calling `{}`, a function value held in a local variable, is "
              "not supported yet — only direct calls to a named module "
              "function are",
              ref.name)});
    }
    const auto found = functions_.find(ref.name);
    if (found == functions_.end()) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unknown_callee,
          .span = call.span,
          .message = std::format("call to `{}` could not be resolved to a "
                                 "function in this compiled module",
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
    for (size_t i = 0; i < values.size(); ++i) {
      auto value = compile_expr(*values[i]);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      builder_.CreateStore(*value, slot_address(block, i));
    }
    return block;
  }

  [[nodiscard]] auto compile_struct_init(const hir::hir_struct_init &init)
      -> std::expected<llvm::Value *, codegen_error> {
    const auto field_count =
        runtime::struct_field_names(types_, init.type).size();
    auto *block = compile_heap_alloc(field_count);
    for (const auto &field : init.fields) {
      const auto slot =
          runtime::struct_field_slot(types_, init.type, field.name);
      if (!slot.has_value()) {
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
      builder_.CreateStore(*value, slot_address(block, *slot));
    }
    return block;
  }

  [[nodiscard]] auto compile_array_init(const hir::hir_array_init &init)
      -> std::expected<llvm::Value *, codegen_error> {
    if (init.fill_value == nullptr) {
      return compile_slots_init(init.elements);
    }
    const auto &array_entry = types_.entry(init.type);
    if (!array_entry.array_size.has_value()) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = init.span,
          .message = "this array literal's fill count is not statically "
                     "known, which llvm_codegen needs to size its heap "
                     "allocation"});
    }
    const auto count = *array_entry.array_size;
    auto *block = compile_heap_alloc(count);
    // Evaluated once, then its bits are stored into every element slot —
    // see bytecode_compiler::compile_array_init's doc comment for why this
    // is correct (and required) rather than re-evaluating per element.
    auto fill_value = compile_expr(*init.fill_value);
    if (!fill_value.has_value()) {
      return std::unexpected(fill_value.error());
    }
    for (uint64_t i = 0; i < count; ++i) {
      builder_.CreateStore(*fill_value, slot_address(block, i));
    }
    return block;
  }

  [[nodiscard]] auto compile_field(const hir::hir_field &field)
      -> std::expected<llvm::Value *, codegen_error> {
    const auto slot = runtime::struct_field_slot(types_, field.object->type,
                                                 field.field_name);
    if (!slot.has_value()) {
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
    return builder_.CreateLoad(*elem_ty, slot_address(*object, *slot));
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

  /// Fixed `array[T, N]` element access only — see
  /// `bytecode_compiler::compile_index`'s doc comment for why `list`/
  /// `slice`/`str` indexing is out of scope until increment 5.
  [[nodiscard]] auto compile_index(const hir::hir_index &node)
      -> std::expected<llvm::Value *, codegen_error> {
    const auto &object_entry = types_.entry(node.object->type);
    if (object_entry.kind != semantic::type_kind::array_kind ||
        !object_entry.array_size.has_value()) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = node.span,
          .message = "indexing is only supported for a fixed-size array "
                     "with a statically known length yet — list/slice/str "
                     "indexing needs increment 5's growable-container "
                     "support"});
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

    auto *len = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_),
                                       *object_entry.array_size);
    auto *out_of_bounds = builder_.CreateICmpUGE(index64, len, "index.oob");
    guard_panic(out_of_bounds, panic_reason::index_out_of_bounds);

    auto elem_ty = storage_type_for(node.type, node.span);
    if (!elem_ty.has_value()) {
      return std::unexpected(elem_ty.error());
    }
    return builder_.CreateLoad(*elem_ty, slot_address(*object, index64));
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
    for (size_t i = 0; i < init.args.size(); ++i) {
      auto value = compile_expr(*init.args[i]);
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
      llvm::Value *acc = nullptr;
      for (size_t i = 0; i < arr.elements.size(); ++i) {
        auto *elem_val = builder_.CreateLoad(*elem_ty, slot_address(value, i));
        auto sub = compile_pattern_test(*arr.elements[i], elem_val,
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
    auto any_reaches_merge = false;

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
          any_reaches_merge = true;
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
        any_reaches_merge = true;
      }
      builder_.SetInsertPoint(next_bb);
    }

    // Exhaustiveness is checker-guaranteed; this is a defensive fallback,
    // not an expected runtime path.
    guard_panic(llvm::ConstantInt::getTrue(ctx_), panic_reason::explicit_panic);
    builder_.CreateBr(merge_bb);
    any_reaches_merge = true;

    builder_.SetInsertPoint(merge_bb);
    if (!any_reaches_merge) {
      builder_.CreateUnreachable();
    }
    return !any_reaches_merge;
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
    case hir_node_kind::hir_return: {
      const auto &ret = dynamic_cast<const hir::hir_return &>(node);
      if (ret.value == nullptr) {
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
    default:
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = node.span,
          .message = "this statement form is outside llvm_codegen's current "
                     "scope — while-let, destructuring let/else, and "
                     "comprehension pushes all need increment 5's growable-"
                     "container support"});
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
    auto *alloca = create_local_alloca(*ty, let.name);
    builder_.CreateStore(*value, alloca);
    locals_.emplace(let.symbol, alloca);
    return false;
  }

  [[nodiscard]] auto compile_assign(const hir::hir_assign &assign)
      -> std::expected<bool, codegen_error> {
    if (assign.target->kind != hir_node_kind::hir_local_ref) {
      return std::unexpected(codegen_error{
          .kind = codegen_error_kind::unsupported_construct,
          .span = assign.span,
          .message = "assigning to a field or index target needs a heap/"
                     "aggregate representation llvm_codegen doesn't have "
                     "yet — only assigning directly to a local variable is "
                     "supported"});
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
    if (assign.op == ast::assign_op::Assign) {
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
    case assign_op::AddAssign:
      equivalent = ast::binary_op::Add;
      break;
    case assign_op::SubAssign:
      equivalent = ast::binary_op::Sub;
      break;
    case assign_op::MulAssign:
      equivalent = ast::binary_op::Mul;
      break;
    case assign_op::DivAssign:
      equivalent = ast::binary_op::Div;
      break;
    case assign_op::ModAssign:
      equivalent = ast::binary_op::Mod;
      break;
    case assign_op::AndAssign:
      equivalent = ast::binary_op::BitAnd;
      break;
    case assign_op::OrAssign:
      equivalent = ast::binary_op::BitOr;
      break;
    case assign_op::XorAssign:
      equivalent = ast::binary_op::BitXor;
      break;
    case assign_op::ShlAssign:
      equivalent = ast::binary_op::Shl;
      break;
    case assign_op::ShrAssign:
      equivalent = ast::binary_op::Shr;
      break;
    case assign_op::AddWrapAssign:
      equivalent = ast::binary_op::AddWrap;
      break;
    case assign_op::SubWrapAssign:
      equivalent = ast::binary_op::SubWrap;
      break;
    case assign_op::MulWrapAssign:
      equivalent = ast::binary_op::MulWrap;
      break;
    case assign_op::AddSatAssign:
      equivalent = ast::binary_op::AddSat;
      break;
    case assign_op::SubSatAssign:
      equivalent = ast::binary_op::SubSat;
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

  [[nodiscard]] auto compile_while(const hir::hir_while &loop)
      -> std::expected<bool, codegen_error> {
    auto *cond_bb = llvm::BasicBlock::Create(ctx_, "while.cond", current_fn_);
    auto *body_bb = llvm::BasicBlock::Create(ctx_, "while.body", current_fn_);
    auto *end_bb = llvm::BasicBlock::Create(ctx_, "while.end", current_fn_);

    builder_.CreateBr(cond_bb);
    builder_.SetInsertPoint(cond_bb);
    auto cond = compile_expr(*loop.condition);
    if (!cond.has_value()) {
      return std::unexpected(cond.error());
    }
    builder_.CreateCondBr(*cond, body_bb, end_bb);

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

  llvm::LLVMContext &ctx_;
  const type_table &types_;
  const std::unordered_map<std::string, llvm::Function *> &functions_;
  llvm::Function *panic_fn_;
  llvm::Function *alloc_fn_;
  llvm::IRBuilder<> builder_;
  llvm::Function *current_fn_ = nullptr;
  llvm::AllocaInst *alloca_marker_ = nullptr;
  std::unordered_map<hir::symbol_id, llvm::AllocaInst *> locals_;
  bool return_is_unit_ = false;
  llvm::Type *return_llvm_type_ = nullptr;
};

} // namespace

auto compile_module(const hir::hir_module &module, const type_table &types)
    -> std::expected<compiled_module, codegen_error> {
  auto result = compiled_module{
      .context = std::make_unique<llvm::LLVMContext>(),
      .module = nullptr,
  };
  result.module =
      std::make_unique<llvm::Module>(module.module_name, *result.context);
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

  // Every function's signature is declared before any body is compiled, so
  // a call (forward-referenced, recursive, or mutually recursive) always
  // finds a real `llvm::Function*` — see codegen.h's doc comment.
  auto functions = std::unordered_map<std::string, llvm::Function *>{};
  functions.reserve(module.functions.size());
  for (const auto &fn : module.functions) {
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

    auto *fn_type = llvm::FunctionType::get(return_ty, param_types,
                                            /*isVarArg=*/false);
    auto *llvm_fn = llvm::Function::Create(
        fn_type, llvm::Function::ExternalLinkage, fn->name, llvm_module);
    functions.emplace(fn->name, llvm_fn);
  }

  for (const auto &fn : module.functions) {
    auto *llvm_fn = functions.at(fn->name);
    auto compiler =
        function_compiler(ctx, types, functions, panic_fn, alloc_fn);
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
        .span = module.span,
        .message =
            std::format("llvm_codegen produced a module that failed "
                        "verification (this is a codegen bug, not a source "
                        "error): {}",
                        verify_message)});
  }

  return result;
}

} // namespace kira::llvm_codegen
