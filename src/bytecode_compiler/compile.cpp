#include "src/bytecode_compiler/compile.h"

#include <bit>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/bytecode/opcodes.h"
#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/hir/captures.h"
#include "src/hir/ids.h"
#include "src/hir/nodes.h"
#include "src/intrinsics.h"
#include "src/parser/ast.h"
#include "src/parser/text_escape.h"
#include "src/parser/token.h"
#include "src/runtime/layout.h"

namespace kira::bytecode_compiler {

namespace {

using bytecode::chunk_writer;
using bytecode::numeric_kind;
using bytecode::numeric_kind_of;
using bytecode::opcode;
using bytecode::slot_value;
using hir::hir_node;
using hir::hir_node_kind;
using semantic::type_id;
using semantic::type_table;

// ==========================================================================
//  Literal decoding — numeric and text escape handling.
//  Text escape decoding (decode_char_literal, decode_string_literal) is
//  exposed from `src/parser/text_escape.h` to avoid duplication across
//  modules — the lexer only *validates* escapes, leaving actual decoding to
//  downstream consumers.
// ==========================================================================

using kira::decode_char_literal;
using kira::decode_string_literal;

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

/// Decodes one UTF-8 scalar value starting at `text[pos]`, advancing `pos`
/// past it. Mirrors the continuation-byte counting `lexer.h`'s char-literal
/// scanning already relies on to know where a multi-byte character ends —
/// this just also computes the resulting code point instead of only
/// skipping past it.
[[nodiscard]] auto encode_f32(float v) -> slot_value {
  return slot_value{static_cast<uint64_t>(std::bit_cast<uint32_t>(v))};
}

/// Encodes one `hir_literal` as the `slot_value` its resolved `numeric_kind`
/// expects. Trusts the checker's own work (`check_integer_fit`,
/// `infer_literal`'s defaulting) rather than re-validating fit or
/// defaulting rules here — this only has to reproduce the *bit pattern*,
/// not re-derive which type the literal ended up with.
[[nodiscard]] auto encode_literal(token_kind lit_kind, std::string_view value,
                                  source_span span, numeric_kind kind)
    -> std::expected<slot_value, compile_error> {
  switch (lit_kind) {
  case token_kind::kw_true:
    return slot_value{uint64_t{1}};
  case token_kind::kw_false:
    return slot_value{uint64_t{0}};
  case token_kind::int_lit: {
    const auto parsed = parse_uint_literal(value);
    if (!parsed.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = span,
          .message =
              std::format("could not parse integer literal `{}`", value)});
    }
    if (bytecode::is_float(kind)) {
      const auto as_double = static_cast<double>(*parsed);
      return kind == numeric_kind::f32
                 ? encode_f32(static_cast<float>(as_double))
                 : slot_value{as_double};
    }
    return slot_value{*parsed};
  }
  case token_kind::float_lit: {
    const auto parsed = parse_float_literal(value);
    if (!parsed.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = span,
          .message = std::format("could not parse float literal `{}`", value)});
    }
    return kind == numeric_kind::f32 ? encode_f32(static_cast<float>(*parsed))
                                     : slot_value{*parsed};
  }
  case token_kind::char_lit: {
    const auto parsed = decode_char_literal(value);
    if (!parsed.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = span,
          .message =
              std::format("could not decode character literal `{}`", value)});
    }
    return slot_value{static_cast<uint64_t>(*parsed)};
  }
  default:
    return std::unexpected(compile_error{
        .kind = compile_error_kind::unsupported_construct,
        .span = span,
        .message = "string literals and other non-scalar literal kinds are not "
                   "supported by the bytecode VM yet (heap types land in "
                   "spec/codegen-design.md increment 6)"});
  }
}

// ==========================================================================
//  Operator -> opcode tables.
// ==========================================================================

/// Ordinary (non-short-circuit) binary opcode for `op`, or `nullopt` for
/// `Pipe`/`And`/`Or`/`In`/`NotIn`/`Range`/`RangeInclusive` — `And`/`Or` are
/// compiled specially (short-circuit jumps, see `compile_expr_into`), and
/// the rest have no bytecode representation in this increment's scope
/// (membership tests need a container, ranges are pattern-only surface
/// syntax with no expression-position `hir_binary` shape to reach here).
[[nodiscard]] auto binary_opcode_for(ast::binary_op op)
    -> std::optional<opcode> {
  switch (op) {
  case ast::binary_op::eq_eq:
    return opcode::op_eq;
  case ast::binary_op::bang_eq:
    return opcode::op_ne;
  case ast::binary_op::lt:
    return opcode::op_lt;
  case ast::binary_op::lt_eq:
    return opcode::op_le;
  case ast::binary_op::gt:
    return opcode::op_gt;
  case ast::binary_op::gt_eq:
    return opcode::op_ge;
  case ast::binary_op::add:
    return opcode::op_add;
  case ast::binary_op::sub:
    return opcode::op_sub;
  case ast::binary_op::mul:
    return opcode::op_mul;
  case ast::binary_op::div:
    return opcode::op_div;
  case ast::binary_op::mod:
    return opcode::op_mod;
  case ast::binary_op::add_wrap:
    return opcode::op_add_wrap;
  case ast::binary_op::sub_wrap:
    return opcode::op_sub_wrap;
  case ast::binary_op::mul_wrap:
    return opcode::op_mul_wrap;
  case ast::binary_op::add_sat:
    return opcode::op_add_sat;
  case ast::binary_op::sub_sat:
    return opcode::op_sub_sat;
  case ast::binary_op::mul_sat:
    return opcode::op_mul_sat;
  case ast::binary_op::shl:
    return opcode::op_shl;
  case ast::binary_op::shr:
    return opcode::op_shr;
  case ast::binary_op::bit_and:
    return opcode::op_bitand;
  case ast::binary_op::bit_or:
    return opcode::op_bitor;
  case ast::binary_op::bit_xor:
    return opcode::op_bitxor;
  default:
    return std::nullopt;
  }
}

/// The plain binary opcode a compound assignment op (`x += y`, ...)
/// desugars to (`Assign` itself has none — handled separately).
[[nodiscard]] auto compound_assign_opcode_for(ast::assign_op op)
    -> std::optional<opcode> {
  switch (op) {
  case ast::assign_op::add_assign:
    return opcode::op_add;
  case ast::assign_op::sub_assign:
    return opcode::op_sub;
  case ast::assign_op::mul_assign:
    return opcode::op_mul;
  case ast::assign_op::div_assign:
    return opcode::op_div;
  case ast::assign_op::mod_assign:
    return opcode::op_mod;
  case ast::assign_op::and_assign:
    return opcode::op_bitand;
  case ast::assign_op::or_assign:
    return opcode::op_bitor;
  case ast::assign_op::xor_assign:
    return opcode::op_bitxor;
  case ast::assign_op::shl_assign:
    return opcode::op_shl;
  case ast::assign_op::shr_assign:
    return opcode::op_shr;
  case ast::assign_op::add_wrap_assign:
    return opcode::op_add_wrap;
  case ast::assign_op::sub_wrap_assign:
    return opcode::op_sub_wrap;
  case ast::assign_op::mul_wrap_assign:
    return opcode::op_mul_wrap;
  case ast::assign_op::add_sat_assign:
    return opcode::op_add_sat;
  case ast::assign_op::sub_sat_assign:
    return opcode::op_sub_sat;
  case ast::assign_op::mul_sat_assign:
    return opcode::op_mul_sat;
  case ast::assign_op::assign:
    return std::nullopt;
  }
  return std::nullopt;
}

// ==========================================================================
//  function_compiler — walks one hir_function, emitting into its own
//  chunk_writer. Not reusable across functions (mirrors hir::lowerer's own
//  "one instance per function" shape).
// ==========================================================================

class function_compiler {
public:
  /// `entry_module_name`/`current_module_name` default to matching (empty)
  /// strings when omitted, so `compile_function`'s standalone single-function
  /// API and every existing single-module `compile_module` call site keep
  /// treating every call as same-module — see `resolve_callee_key`.
  function_compiler(const type_table &types,
                    const std::unordered_map<std::string, uint16_t> &functions,
                    std::vector<bytecode::bytecode_function> &lambda_functions,
                    size_t function_table_base,
                    std::string entry_module_name = {},
                    std::string current_module_name = {})
      : types_(types), functions_(functions),
        lambda_functions_(lambda_functions),
        function_table_base_(function_table_base),
        entry_module_name_(std::move(entry_module_name)),
        current_module_name_(std::move(current_module_name)) {}

  [[nodiscard]] auto compile(const hir::hir_function &fn)
      -> std::expected<bytecode::bytecode_function, compile_error> {
    for (const auto &param : fn.params) {
      const auto reg = alloc_register(fn.span);
      if (!reg.has_value()) {
        return std::unexpected(reg.error());
      }
      locals_.emplace(param.symbol, *reg);
    }
    return compile_body_and_finish(*fn.body, fn.span, fn.name,
                                   static_cast<uint16_t>(fn.params.size()));
  }

  /// Compiles a lambda's body into its own `bytecode_function`, using the
  /// closure calling convention: register 0 is always the (possibly-null)
  /// environment pointer, declared params occupy the registers right after
  /// it, and every captured free variable in `free_vars` is loaded out of
  /// the environment block (via `op_load_slot`) before the body runs — see
  /// `compile_lambda_value`, which builds that environment block and emits
  /// the matching `op_make_closure`.
  [[nodiscard]] auto
  compile_lambda_body(const hir::hir_lambda &lambda,
                      const std::vector<hir::symbol_id> &free_vars)
      -> std::expected<bytecode::bytecode_function, compile_error> {
    const auto env_reg = alloc_register(lambda.span);
    if (!env_reg.has_value()) {
      return std::unexpected(env_reg.error());
    }
    for (const auto &param : lambda.params) {
      const auto reg = alloc_register(lambda.span);
      if (!reg.has_value()) {
        return std::unexpected(reg.error());
      }
      locals_.emplace(param.symbol, *reg);
    }
    for (size_t i = 0; i < free_vars.size(); ++i) {
      const auto reg = alloc_register(lambda.span);
      if (!reg.has_value()) {
        return std::unexpected(reg.error());
      }
      writer_.emit_opcode(opcode::op_load_slot);
      writer_.emit_u8(*reg);
      writer_.emit_u8(*env_reg);
      writer_.emit_u16(static_cast<uint16_t>(i));
      locals_.emplace(free_vars[i], *reg);
    }
    return compile_body_and_finish(
        *lambda.body, lambda.span, "<lambda>",
        static_cast<uint16_t>(1 + lambda.params.size()));
  }

private:
  [[nodiscard]] auto
  compile_body_and_finish(const hir::hir_block &body, source_span span,
                          const std::string &name, uint16_t param_count)
      -> std::expected<bytecode::bytecode_function, compile_error> {
    const auto &stmts = body.stmts;
    for (size_t i = 0; i + 1 < stmts.size(); ++i) {
      if (auto result = compile_stmt(*stmts[i]); !result.has_value()) {
        return std::unexpected(result.error());
      }
    }
    if (stmts.empty()) {
      writer_.emit_opcode(opcode::op_return_unit);
    } else {
      const auto &last = *stmts.back();
      if (last.kind == hir_node_kind::hir_expr_stmt) {
        const auto &expr_stmt = dynamic_cast<const hir::hir_expr_stmt &>(last);
        auto reg = compile_expr(*expr_stmt.expr);
        if (!reg.has_value()) {
          return std::unexpected(reg.error());
        }
        writer_.emit_opcode(opcode::op_return_value);
        writer_.emit_u8(*reg);
      } else {
        if (auto result = compile_stmt(last); !result.has_value()) {
          return std::unexpected(result.error());
        }
        writer_.emit_opcode(opcode::op_return_unit);
      }
    }

    if (next_register_ > 256) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::register_limit_exceeded,
          .span = span,
          .message = std::format("function `{}` needs more than 256 "
                                 "registers, which this bytecode format's "
                                 "u8 register operands cannot address",
                                 name)});
    }
    return std::move(writer_).finish(name, param_count,
                                     static_cast<uint16_t>(next_register_));
  }
  // ------------------------------------------------------------------
  //  Registers and locals.
  // ------------------------------------------------------------------

  [[nodiscard]] auto alloc_register(source_span span)
      -> std::expected<uint8_t, compile_error> {
    if (next_register_ >= 256) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::register_limit_exceeded,
          .span = span,
          .message = "this function needs more than 256 registers, which this "
                     "bytecode format's u8 register operands cannot address"});
    }
    return static_cast<uint8_t>(next_register_++);
  }

  [[nodiscard]] auto lookup_local(hir::symbol_id symbol) const
      -> std::optional<uint8_t> {
    if (const auto found = locals_.find(symbol); found != locals_.end()) {
      return found->second;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto numeric_kind_for(type_id id, source_span span)
      -> std::expected<numeric_kind, compile_error> {
    const auto kind = numeric_kind_of(types_, id);
    if (!kind.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_type,
          .span = span,
          .message = std::format(
              "type `{}` has no scalar bytecode representation yet — "
              "non-scalar types and 128-bit widths are not supported by "
              "the bytecode VM until spec/codegen-design.md increment 6 "
              "(heap types) lands",
              types_.display(id))});
    }
    return *kind;
  }

  /// Whether `id` is the prelude's growable `list[T]` container — as
  /// opposed to a fixed `array[T, N]`, which shares construction/indexing
  /// codegen for everything except sizing/length (see `compile_array_init`/
  /// `compile_index`).
  [[nodiscard]] auto is_list_type(type_id id) const -> bool {
    const auto &entry = types_.entry(strip_refs(id));
    return entry.kind == semantic::type_kind::builtin_generic_kind &&
           entry.name == "list";
  }

  /// Unwraps `&T`/`&mut T` down to `T` — mirrors `semantic::check.cpp`'s own
  /// `strip_refs`. A reference's compiled *value* is already representation-
  /// identical to the referent for every heap-pointer-backed type
  /// (`is_heap_pointer_value`'s `addr_of`/`addr_of_mut` passthrough), but
  /// `hir_index`/`hir_field`'s `object->type` still carries the unstripped
  /// `&T` the checker recorded (`infer_index`/`infer_method_call` strip
  /// refs only for their own local dispatch, not for what they record) — so
  /// any type-kind check made *against* that recorded type (e.g. "is this a
  /// `slice`?") has to strip it first, the same way the checker's own
  /// dispatch already does.
  [[nodiscard]] auto strip_refs(type_id id) const -> type_id {
    const auto *entry = &types_.entry(id);
    while (entry->kind == semantic::type_kind::ref_kind) {
      id = entry->result;
      entry = &types_.entry(id);
    }
    return id;
  }

  /// Whether `id` is a fixed `array[byte, N]` — the one array element type
  /// that is stored tightly byte-packed (1 byte/element via `op_store_byte`/
  /// `op_load_byte_indexed`) rather than the generic 8-bytes/element slot
  /// layout every other `array[T, N]` uses (`compile_array_init`'s doc
  /// comment). Byte-packing is what lets `buf[a..b]` alias a real
  /// `str`/`slice[byte]` view with no copy (`compile_range_index`) — needed
  /// so a mutation through the view (e.g. `rt_read` filling it) is visible
  /// back through `buf` itself, which a copy-based view would silently
  /// break for `std.io.reader::read_to_end`'s `buf[0..4096]`.
  [[nodiscard]] auto is_byte_array_type(type_id id) const -> bool {
    const auto &entry = types_.entry(strip_refs(id));
    if (entry.kind != semantic::type_kind::array_kind ||
        !entry.array_size.has_value()) {
      return false;
    }
    const auto &elem = types_.entry(entry.result);
    return elem.kind == semantic::type_kind::builtin_kind &&
           elem.name == "byte";
  }

  // ------------------------------------------------------------------
  //  Expressions.
  // ------------------------------------------------------------------

  [[nodiscard]] auto compile_expr(const hir::hir_expr &expr)
      -> std::expected<uint8_t, compile_error> {
    auto reg = alloc_register(expr.span);
    if (!reg.has_value()) {
      return std::unexpected(reg.error());
    }
    if (auto result = compile_expr_into(expr, *reg); !result.has_value()) {
      return std::unexpected(result.error());
    }
    return *reg;
  }

  [[nodiscard]] auto compile_expr_into(const hir::hir_expr &expr, uint8_t dst)
      -> std::expected<void, compile_error> {
    switch (expr.kind) {
    case hir_node_kind::hir_literal: {
      const auto &lit = dynamic_cast<const hir::hir_literal &>(expr);
      if (lit.lit_kind == token_kind::string_lit) {
        return compile_string_literal(lit, dst);
      }
      if (lit.lit_kind == token_kind::kw_unit) {
        // `unit` has exactly one value and carries no information — nothing
        // downstream ever reads it back out, so a zeroed placeholder slot
        // (mirroring `op_return_unit`'s own no-value return) is enough.
        // Bypasses `numeric_kind_for` below, which has no scalar kind for
        // `unit` at all.
        const auto index = writer_.add_constant(slot_value{uint64_t{0}});
        writer_.emit_opcode(opcode::op_load_const);
        writer_.emit_u8(dst);
        writer_.emit_u16(index);
        return {};
      }
      auto kind = numeric_kind_for(expr.type, expr.span);
      if (!kind.has_value()) {
        return std::unexpected(kind.error());
      }
      auto value = encode_literal(lit.lit_kind, lit.value, lit.span, *kind);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      const auto index = writer_.add_constant(*value);
      writer_.emit_opcode(opcode::op_load_const);
      writer_.emit_u8(dst);
      writer_.emit_u16(index);
      return {};
    }
    case hir_node_kind::hir_local_ref: {
      const auto &ref = dynamic_cast<const hir::hir_local_ref &>(expr);
      const auto src = lookup_local(ref.symbol);
      if (!src.has_value()) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_construct,
            .span = expr.span,
            .message = std::format(
                "reference to `{}` is not a local binding — a bare "
                "function value used outside of call position is not "
                "supported yet",
                ref.name)});
      }
      if (*src != dst) {
        writer_.emit_opcode(opcode::op_move);
        writer_.emit_u8(dst);
        writer_.emit_u8(*src);
      }
      return {};
    }
    case hir_node_kind::hir_binary:
      return compile_binary(dynamic_cast<const hir::hir_binary &>(expr), dst);
    case hir_node_kind::hir_unary:
      return compile_unary(dynamic_cast<const hir::hir_unary &>(expr), dst);
    case hir_node_kind::hir_cast:
      return compile_cast(dynamic_cast<const hir::hir_cast &>(expr), dst);
    case hir_node_kind::hir_call:
      return compile_call(dynamic_cast<const hir::hir_call &>(expr), dst);
    case hir_node_kind::hir_lambda:
      return compile_lambda_value(dynamic_cast<const hir::hir_lambda &>(expr),
                                  dst);
    case hir_node_kind::hir_if:
      return compile_if(dynamic_cast<const hir::hir_if &>(expr), dst);
    case hir_node_kind::hir_tuple: {
      const auto &tup = dynamic_cast<const hir::hir_tuple &>(expr);
      return compile_slots_init(tup.elements, dst, tup.span);
    }
    case hir_node_kind::hir_struct_init:
      return compile_struct_init(
          dynamic_cast<const hir::hir_struct_init &>(expr), dst);
    case hir_node_kind::hir_array_init:
      return compile_array_init(dynamic_cast<const hir::hir_array_init &>(expr),
                                dst);
    case hir_node_kind::hir_field:
      return compile_field(dynamic_cast<const hir::hir_field &>(expr), dst);
    case hir_node_kind::hir_index:
      return compile_index(dynamic_cast<const hir::hir_index &>(expr), dst);
    case hir_node_kind::hir_tuple_index:
      return compile_tuple_index(
          dynamic_cast<const hir::hir_tuple_index &>(expr), dst);
    case hir_node_kind::hir_variant_init:
      return compile_variant_init(
          dynamic_cast<const hir::hir_variant_init &>(expr), dst);
    case hir_node_kind::hir_variant_payload:
      return compile_variant_payload(
          dynamic_cast<const hir::hir_variant_payload &>(expr), dst);
    case hir_node_kind::hir_match:
      return compile_match(dynamic_cast<const hir::hir_match &>(expr), dst);
    case hir_node_kind::hir_container_len:
      return compile_container_len(
          dynamic_cast<const hir::hir_container_len &>(expr), dst);
    case hir_node_kind::hir_block:
      // A block used in expression position (e.g. a comprehension's
      // desugared accumulator block, `hir::lower_comprehension`) — its
      // trailing statement's value is compiled directly into `dst`, the
      // same way an `if`/`match` arm's body already is via
      // `compile_block_as_value`.
      return compile_block_as_value(dynamic_cast<const hir::hir_block &>(expr),
                                    std::optional<uint8_t>(dst));
    default:
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = expr.span,
          .message = "this expression form is outside the bytecode compiler's "
                     "current scope"});
    }
  }

  [[nodiscard]] auto compile_binary(const hir::hir_binary &bin, uint8_t dst)
      -> std::expected<void, compile_error> {
    if (bin.op == ast::binary_op::logical_and ||
        bin.op == ast::binary_op::logical_or) {
      // Short-circuit via jumps (opcodes.h's documented backend-only
      // choice) — evaluate lhs into dst, skip rhs (leaving dst as the
      // final result) exactly when short-circuiting applies.
      if (auto result = compile_expr_into(*bin.lhs, dst); !result.has_value()) {
        return std::unexpected(result.error());
      }
      writer_.emit_opcode(bin.op == ast::binary_op::logical_and
                              ? opcode::op_jump_if_false
                              : opcode::op_jump_if_true);
      writer_.emit_u8(dst);
      const auto end_placeholder = writer_.emit_jump_placeholder();
      if (auto result = compile_expr_into(*bin.rhs, dst); !result.has_value()) {
        return std::unexpected(result.error());
      }
      writer_.patch_jump_to_here(end_placeholder);
      return {};
    }

    const auto op = binary_opcode_for(bin.op);
    if (!op.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = bin.span,
          .message =
              std::format("operator `{}` is not supported by the bytecode "
                          "compiler yet",
                          ast::binary_op_name(bin.op))});
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
    writer_.emit_opcode(*op);
    writer_.emit_u8(dst);
    writer_.emit_u8(*lhs);
    writer_.emit_u8(*rhs);
    writer_.emit_numeric_kind(*kind);
    return {};
  }

  /// Whether `id`'s runtime representation is already a single pointer into
  /// a flat N-slot heap block (`src/runtime/layout.h`) rather than an
  /// inline scalar bit pattern. For exactly these kinds, `&`/`&mut` needs no
  /// new instruction at all: the compiled value already *is* the address a
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

  [[nodiscard]] auto compile_unary(const hir::hir_unary &un, uint8_t dst)
      -> std::expected<void, compile_error> {
    if (un.op == ast::unary_op::logical_not) {
      auto src = compile_expr(*un.operand);
      if (!src.has_value()) {
        return std::unexpected(src.error());
      }
      writer_.emit_opcode(opcode::op_not_bool);
      writer_.emit_u8(dst);
      writer_.emit_u8(*src);
      return {};
    }
    if ((un.op == ast::unary_op::addr_of ||
         un.op == ast::unary_op::addr_of_mut) &&
        is_heap_pointer_value(un.operand->type)) {
      // The operand's compiled value already is the pointer a reference to
      // it would hold — compile it straight into `dst`, no new opcode.
      return compile_expr_into(*un.operand, dst);
    }
    if (un.op != ast::unary_op::neg && un.op != ast::unary_op::bit_not) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = un.span,
          .message =
              std::format("operator `{}` needs a reference/pointer model this "
                          "bytecode compiler doesn't have yet",
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
    writer_.emit_opcode(un.op == ast::unary_op::neg ? opcode::op_neg
                                                    : opcode::op_bitnot);
    writer_.emit_u8(dst);
    writer_.emit_u8(*src);
    writer_.emit_numeric_kind(*kind);
    return {};
  }

  [[nodiscard]] auto compile_cast(const hir::hir_cast &cast, uint8_t dst)
      -> std::expected<void, compile_error> {
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
    writer_.emit_opcode(opcode::op_cast);
    writer_.emit_u8(dst);
    writer_.emit_u8(*src);
    writer_.emit_numeric_kind(*from_kind);
    writer_.emit_numeric_kind(*to_kind);
    return {};
  }

  /// The `functions_` key a call to `ref` resolves against: bare `ref.name`
  /// when the target is the entry module (whether that's implicit — no
  /// `owner_module`, and this function's own module is the entry — or
  /// explicit via `owner_module` naming the entry module itself), otherwise
  /// `module::name` naming whichever non-entry module owns the target. This
  /// is the single place that knows the mangling scheme `compile_module`'s
  /// multi-module overload builds `functions_` with, so a lookup here always
  /// agrees with how the table was built.
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

  [[nodiscard]] auto compile_call(const hir::hir_call &call, uint8_t dst)
      -> std::expected<void, compile_error> {
    if (call.args.size() > 255) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = call.span,
          .message =
              "more than 255 call arguments is not supported by this bytecode "
              "format's u8 argument-count operand"});
    }
    const auto argc = call.args.size();

    // Direct call to a named module-level function, resolved at compile
    // time to a fixed function-table index — the common case, and the only
    // shape `op_call` (rather than `op_call_indirect`) can express.
    if (call.callee->kind == hir_node_kind::hir_local_ref) {
      const auto &ref = dynamic_cast<const hir::hir_local_ref &>(*call.callee);
      if (!lookup_local(ref.symbol).has_value()) {
        // `intrinsic def` declarations never enter `functions_` (hir::
        // lower_module skips them — there is no body to lower), so a call
        // to a known intrinsic name is recognized here instead and compiled
        // to `op_call_intrinsic` rather than `op_call`.
        if (const auto intrinsic_id = kira::intrinsic_index_of(ref.name);
            intrinsic_id.has_value()) {
          if (next_register_ + argc > 256) {
            return std::unexpected(compile_error{
                .kind = compile_error_kind::register_limit_exceeded,
                .span = call.span,
                .message =
                    "this call needs more than 256 registers, which this "
                    "bytecode format's u8 register operands cannot address"});
          }
          const auto first_arg_reg = static_cast<uint8_t>(next_register_);
          next_register_ += argc;
          for (size_t i = 0; i < call.args.size(); ++i) {
            const auto arg_reg = static_cast<uint8_t>(first_arg_reg + i);
            if (auto result = compile_expr_into(*call.args[i], arg_reg);
                !result.has_value()) {
              return std::unexpected(result.error());
            }
          }
          writer_.emit_opcode(opcode::op_call_intrinsic);
          writer_.emit_u8(dst);
          writer_.emit_u8(*intrinsic_id);
          writer_.emit_u8(first_arg_reg);
          writer_.emit_u8(static_cast<uint8_t>(argc));
          return {};
        }

        const auto found = functions_.find(resolve_callee_key(ref));
        if (found == functions_.end()) {
          return std::unexpected(compile_error{
              .kind = compile_error_kind::unknown_callee,
              .span = call.span,
              .message = std::format(
                  "call to `{}` could not be resolved to a function in this "
                  "compiled module",
                  ref.name)});
        }
        if (next_register_ + argc > 256) {
          return std::unexpected(compile_error{
              .kind = compile_error_kind::register_limit_exceeded,
              .span = call.span,
              .message = "this function needs more than 256 registers, which "
                         "this bytecode format's u8 register operands cannot "
                         "address"});
        }
        // Reserve a contiguous register block up front (before compiling
        // any argument subexpression) so op_call's "argc consecutive
        // registers" precondition holds regardless of how many temporaries
        // each argument expression needs internally — those get allocated
        // after this block automatically, since next_register_ has already
        // moved past it.
        const auto first_arg_reg = static_cast<uint8_t>(next_register_);
        next_register_ += argc;
        for (size_t i = 0; i < call.args.size(); ++i) {
          const auto arg_reg = static_cast<uint8_t>(first_arg_reg + i);
          if (auto result = compile_expr_into(*call.args[i], arg_reg);
              !result.has_value()) {
            return std::unexpected(result.error());
          }
        }
        writer_.emit_opcode(opcode::op_call);
        writer_.emit_u8(dst);
        writer_.emit_u16(found->second);
        writer_.emit_u8(first_arg_reg);
        writer_.emit_u8(static_cast<uint8_t>(argc));
        return {};
      }
    }

    // Indirect call — the callee is a closure value: a local variable
    // holding a `fn(...)`-typed value, an immediately-invoked lambda
    // literal, or any other computed callee expression. Compile the callee
    // generically and dispatch through `op_call_indirect`, which reads the
    // `{ function_index; env_ptr }` pair out of the resulting heap value.
    const auto closure_reg = compile_expr(*call.callee);
    if (!closure_reg.has_value()) {
      return std::unexpected(closure_reg.error());
    }
    if (next_register_ + argc > 256) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::register_limit_exceeded,
          .span = call.span,
          .message = "this function needs more than 256 registers, which this "
                     "bytecode format's u8 register operands cannot address"});
    }
    const auto first_arg_reg = static_cast<uint8_t>(next_register_);
    next_register_ += argc;
    for (size_t i = 0; i < call.args.size(); ++i) {
      const auto arg_reg = static_cast<uint8_t>(first_arg_reg + i);
      if (auto result = compile_expr_into(*call.args[i], arg_reg);
          !result.has_value()) {
        return std::unexpected(result.error());
      }
    }
    writer_.emit_opcode(opcode::op_call_indirect);
    writer_.emit_u8(dst);
    writer_.emit_u8(*closure_reg);
    writer_.emit_u8(first_arg_reg);
    writer_.emit_u8(static_cast<uint8_t>(argc));
    return {};
  }

  /// Compiles a lambda literal into a closure heap value: allocates (and
  /// populates) an environment block holding every free variable
  /// `hir::free_variables` finds, compiles the lambda's body into its own
  /// `bytecode_function` (appended to `lambda_functions_`, see
  /// `compile_lambda_body`), and emits `op_make_closure` to tie the two
  /// together at `dst`.
  [[nodiscard]] auto compile_lambda_value(const hir::hir_lambda &lambda,
                                          uint8_t dst)
      -> std::expected<void, compile_error> {
    const auto free_vars = hir::free_variables(lambda);

    uint8_t env_reg = 0;
    if (free_vars.empty()) {
      const auto reg = alloc_register(lambda.span);
      if (!reg.has_value()) {
        return std::unexpected(reg.error());
      }
      env_reg = *reg;
      const auto index = writer_.add_constant(slot_value{uint64_t{0}});
      writer_.emit_opcode(opcode::op_load_const);
      writer_.emit_u8(env_reg);
      writer_.emit_u16(index);
    } else {
      const auto reg = alloc_register(lambda.span);
      if (!reg.has_value()) {
        return std::unexpected(reg.error());
      }
      env_reg = *reg;
      writer_.emit_opcode(opcode::op_alloc);
      writer_.emit_u8(env_reg);
      writer_.emit_u16(static_cast<uint16_t>(free_vars.size()));
      for (size_t i = 0; i < free_vars.size(); ++i) {
        const auto src = lookup_local(free_vars[i]);
        if (!src.has_value()) {
          return std::unexpected(compile_error{
              .kind = compile_error_kind::unsupported_construct,
              .span = lambda.span,
              .message = "a captured variable could not be found among this "
                         "function's locals — capture analysis and register "
                         "allocation have gotten out of sync"});
        }
        writer_.emit_opcode(opcode::op_store_slot);
        writer_.emit_u8(env_reg);
        writer_.emit_u16(static_cast<uint16_t>(i));
        writer_.emit_u8(*src);
      }
    }

    auto compiled = function_compiler(types_, functions_, lambda_functions_,
                                      function_table_base_, entry_module_name_,
                                      current_module_name_)
                        .compile_lambda_body(lambda, free_vars);
    if (!compiled.has_value()) {
      return std::unexpected(compiled.error());
    }
    lambda_functions_.push_back(std::move(*compiled));
    const auto fn_index = static_cast<uint16_t>(function_table_base_ +
                                                lambda_functions_.size() - 1);

    writer_.emit_opcode(opcode::op_make_closure);
    writer_.emit_u8(dst);
    writer_.emit_u16(fn_index);
    writer_.emit_u8(env_reg);
    return {};
  }

  /// Compiles an `if`/`elif`/`else` chain. `want_result` is the register
  /// every branch's (and the else's) tail value is copied into, when this
  /// `hir_if` is used in expression position; `nullopt` for statement
  /// position, where branch bodies are compiled for their side effects only
  /// (mirrors `hir_if`'s own doc comment on the two being the same node).
  [[nodiscard]] auto compile_if(const hir::hir_if &node,
                                std::optional<uint8_t> want_result)
      -> std::expected<void, compile_error> {
    auto end_placeholders = std::vector<size_t>{};
    auto next_branch_placeholder = std::optional<size_t>{};

    for (const auto &branch : node.branches) {
      if (next_branch_placeholder.has_value()) {
        writer_.patch_jump_to_here(*next_branch_placeholder);
        next_branch_placeholder.reset();
      }
      auto cond = compile_expr(*branch.condition);
      if (!cond.has_value()) {
        return std::unexpected(cond.error());
      }
      writer_.emit_opcode(opcode::op_jump_if_false);
      writer_.emit_u8(*cond);
      next_branch_placeholder = writer_.emit_jump_placeholder();

      if (auto result = compile_block_as_value(*branch.body, want_result);
          !result.has_value()) {
        return std::unexpected(result.error());
      }
      writer_.emit_opcode(opcode::op_jump);
      end_placeholders.push_back(writer_.emit_jump_placeholder());
    }

    if (next_branch_placeholder.has_value()) {
      writer_.patch_jump_to_here(*next_branch_placeholder);
    }
    if (node.else_body != nullptr) {
      if (auto result = compile_block_as_value(*node.else_body, want_result);
          !result.has_value()) {
        return std::unexpected(result.error());
      }
    }
    for (const auto placeholder : end_placeholders) {
      writer_.patch_jump_to_here(placeholder);
    }
    return {};
  }

  // ------------------------------------------------------------------
  //  Heap values (spec/codegen-design.md increment 6): `str` literals,
  //  fixed `array[T, N]`/tuple/struct construction, and field/element
  //  reads — every non-scalar value is a single pointer into
  //  `src/runtime/arena.h`'s bump allocator, and every aggregate is a flat
  //  block of 8-byte slots (`src/runtime/layout.h`), so construction is
  //  always "alloc N slots, store each element/field," and reading is
  //  always one `op_load_slot`/`op_load_indexed`.
  // ------------------------------------------------------------------

  [[nodiscard]] auto compile_string_literal(const hir::hir_literal &lit,
                                            uint8_t dst)
      -> std::expected<void, compile_error> {
    auto text = decode_string_literal(lit.value);
    if (!text.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = lit.span,
          .message =
              std::format("could not decode string literal `{}`", lit.value)});
    }
    const auto index = writer_.add_string_constant(std::move(*text));
    writer_.emit_opcode(opcode::op_load_str_const);
    writer_.emit_u8(dst);
    writer_.emit_u16(index);
    return {};
  }

  /// Allocates a `values.size()`-slot heap block into `dst` and stores each
  /// element's compiled value at its corresponding slot, in order — the
  /// shared shape behind tuple construction, the explicit-list form of an
  /// array literal, and (via `compile_struct_init`) struct construction.
  [[nodiscard]] auto
  compile_slots_init(const hir::ptr_vec<hir::hir_expr> &values, uint8_t dst,
                     source_span span) -> std::expected<void, compile_error> {
    if (values.size() > 0xFFFF) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::register_limit_exceeded,
          .span = span,
          .message = "this literal has more than 65535 elements/fields, "
                     "which this bytecode format's u16 slot-count operand "
                     "cannot address"});
    }
    writer_.emit_opcode(opcode::op_alloc);
    writer_.emit_u8(dst);
    writer_.emit_u16(static_cast<uint16_t>(values.size()));
    for (size_t i = 0; i < values.size(); ++i) {
      auto value_reg = compile_expr(*values[i]);
      if (!value_reg.has_value()) {
        return std::unexpected(value_reg.error());
      }
      writer_.emit_opcode(opcode::op_store_slot);
      writer_.emit_u8(dst);
      writer_.emit_u16(static_cast<uint16_t>(i));
      writer_.emit_u8(*value_reg);
    }
    return {};
  }

  [[nodiscard]] auto compile_struct_init(const hir::hir_struct_init &init,
                                         uint8_t dst)
      -> std::expected<void, compile_error> {
    const auto field_count =
        runtime::struct_field_names(types_, init.type).size();
    writer_.emit_opcode(opcode::op_alloc);
    writer_.emit_u8(dst);
    writer_.emit_u16(static_cast<uint16_t>(field_count));
    for (const auto &field : init.fields) {
      const auto slot =
          runtime::struct_field_slot(types_, init.type, field.name);
      if (!slot.has_value()) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_construct,
            .span = init.span,
            .message = std::format(
                "field `{}` does not resolve to a declared struct field — "
                "this should have been rejected by the type checker",
                field.name)});
      }
      auto value_reg = compile_expr(*field.value);
      if (!value_reg.has_value()) {
        return std::unexpected(value_reg.error());
      }
      writer_.emit_opcode(opcode::op_store_slot);
      writer_.emit_u8(dst);
      writer_.emit_u16(static_cast<uint16_t>(*slot));
      writer_.emit_u8(*value_reg);
    }
    return {};
  }

  /// Allocates a byte-packed `array[byte, N]` (`is_byte_array_type`) —
  /// `ceil(N/8)` slots' worth of raw bytes, one byte per element via
  /// `op_store_byte`, mirroring `compile_slots_init`/the fill-form branch of
  /// `compile_array_init` exactly except for storing a byte instead of a
  /// whole slot per element. `N` is always a compile-time constant here
  /// (checked already, by the caller and by `check.cpp`'s requirement that a
  /// fixed array's size be statically known), so — like the slot-packed fill
  /// form this mirrors — the per-element stores are unrolled at compile
  /// time rather than emitting a runtime loop.
  [[nodiscard]] auto compile_byte_array_init(const hir::hir_array_init &init,
                                             uint64_t count, uint8_t dst)
      -> std::expected<void, compile_error> {
    const auto slot_count = static_cast<uint16_t>((count + 7) / 8);
    writer_.emit_opcode(opcode::op_alloc);
    writer_.emit_u8(dst);
    writer_.emit_u16(slot_count);
    if (init.fill_value == nullptr) {
      for (size_t i = 0; i < init.elements.size(); ++i) {
        auto value_reg = compile_expr(*init.elements[i]);
        if (!value_reg.has_value()) {
          return std::unexpected(value_reg.error());
        }
        writer_.emit_opcode(opcode::op_store_byte);
        writer_.emit_u8(dst);
        writer_.emit_u16(static_cast<uint16_t>(i));
        writer_.emit_u8(*value_reg);
      }
      return {};
    }
    // See `compile_array_init`'s fill form for why `fill_value` is evaluated
    // once, not re-evaluated per element.
    auto fill_reg = compile_expr(*init.fill_value);
    if (!fill_reg.has_value()) {
      return std::unexpected(fill_reg.error());
    }
    for (uint64_t i = 0; i < count; ++i) {
      writer_.emit_opcode(opcode::op_store_byte);
      writer_.emit_u8(dst);
      writer_.emit_u16(static_cast<uint16_t>(i));
      writer_.emit_u8(*fill_reg);
    }
    return {};
  }

  [[nodiscard]] auto compile_array_init(const hir::hir_array_init &init,
                                        uint8_t dst)
      -> std::expected<void, compile_error> {
    if (is_list_type(init.type)) {
      return compile_list_init(init, dst);
    }
    if (is_byte_array_type(init.type)) {
      // `check.cpp` requires an explicit-elements array literal's element
      // count to match its declared size exactly, so `array_size` is the
      // right count either way — fill or explicit-elements form.
      const auto count = *types_.entry(init.type).array_size;
      if (count > 0xFFFF) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::register_limit_exceeded,
            .span = init.span,
            .message = "this array literal has more than 65535 elements, "
                       "which this bytecode format's u16 byte-offset operand "
                       "cannot address"});
      }
      return compile_byte_array_init(init, count, dst);
    }
    if (init.fill_value == nullptr) {
      return compile_slots_init(init.elements, dst, init.span);
    }
    const auto &array_entry = types_.entry(init.type);
    if (!array_entry.array_size.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = init.span,
          .message = "this array literal's fill count is not statically "
                     "known, which the bytecode compiler needs to size its "
                     "heap allocation"});
    }
    const auto count = *array_entry.array_size;
    if (count > 0xFFFF) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::register_limit_exceeded,
          .span = init.span,
          .message = "this array literal has more than 65535 elements, "
                     "which this bytecode format's u16 slot-count operand "
                     "cannot address"});
    }
    writer_.emit_opcode(opcode::op_alloc);
    writer_.emit_u8(dst);
    writer_.emit_u16(static_cast<uint16_t>(count));
    // Evaluated once, then its slot bits are copied into every element —
    // aliasing rather than re-evaluating `fill_value` per element, correct
    // for value semantics and the only sane choice if `fill_value` has a
    // side effect (a `[f(); n]` literal must call `f()` exactly once, the
    // same way every mainstream language with fill-array syntax works).
    auto fill_reg = compile_expr(*init.fill_value);
    if (!fill_reg.has_value()) {
      return std::unexpected(fill_reg.error());
    }
    for (uint64_t i = 0; i < count; ++i) {
      writer_.emit_opcode(opcode::op_store_slot);
      writer_.emit_u8(dst);
      writer_.emit_u16(static_cast<uint16_t>(i));
      writer_.emit_u8(*fill_reg);
    }
    return {};
  }

  /// Constructs a growable `list[T]` heap value: a 3-slot header
  /// `{ len; cap; data }` (`src/runtime/layout.h`), zero-initialized by
  /// `op_alloc` into an empty list (`len == cap == 0`, `data == null`) and
  /// then grown via `op_list_push` — for the explicit-elements form, one
  /// push per literal element; for the fill form `[val; count]`, `count`
  /// pushes of the same evaluated-once value in a runtime loop. Unlike a
  /// fixed array's `[T, N]` (whose `N` the checker requires to be
  /// statically known), a list's fill count may be an arbitrary runtime
  /// `usize` expression (`infer_array` in `check.cpp` only requires the
  /// *element* type, not the count, to be resolvable) — the push-loop
  /// shape is the only construction strategy that works uniformly whether
  /// `count` happens to be a literal or not.
  [[nodiscard]] auto compile_list_init(const hir::hir_array_init &init,
                                       uint8_t dst)
      -> std::expected<void, compile_error> {
    writer_.emit_opcode(opcode::op_alloc);
    writer_.emit_u8(dst);
    writer_.emit_u16(3);

    if (init.fill_value == nullptr) {
      for (const auto &elem : init.elements) {
        auto value_reg = compile_expr(*elem);
        if (!value_reg.has_value()) {
          return std::unexpected(value_reg.error());
        }
        writer_.emit_opcode(opcode::op_list_push);
        writer_.emit_u8(dst);
        writer_.emit_u8(*value_reg);
      }
      return {};
    }

    // Evaluated once, then pushed `count` times — see
    // `compile_array_init`'s fixed-array fill form for why a side-effecting
    // fill expression must only run once.
    auto fill_reg = compile_expr(*init.fill_value);
    if (!fill_reg.has_value()) {
      return std::unexpected(fill_reg.error());
    }
    auto count_kind = numeric_kind_for(init.fill_count->type, init.span);
    if (!count_kind.has_value()) {
      return std::unexpected(count_kind.error());
    }
    auto count_reg = compile_expr(*init.fill_count);
    if (!count_reg.has_value()) {
      return std::unexpected(count_reg.error());
    }

    auto idx_reg = alloc_register(init.span);
    if (!idx_reg.has_value()) {
      return std::unexpected(idx_reg.error());
    }
    const auto zero_const = writer_.add_constant(slot_value{uint64_t{0}});
    writer_.emit_opcode(opcode::op_load_const);
    writer_.emit_u8(*idx_reg);
    writer_.emit_u16(zero_const);

    const auto loop_start = writer_.current_offset();
    auto cmp_reg = alloc_register(init.span);
    if (!cmp_reg.has_value()) {
      return std::unexpected(cmp_reg.error());
    }
    writer_.emit_opcode(opcode::op_lt);
    writer_.emit_u8(*cmp_reg);
    writer_.emit_u8(*idx_reg);
    writer_.emit_u8(*count_reg);
    writer_.emit_numeric_kind(*count_kind);
    writer_.emit_opcode(opcode::op_jump_if_false);
    writer_.emit_u8(*cmp_reg);
    const auto exit_placeholder = writer_.emit_jump_placeholder();

    writer_.emit_opcode(opcode::op_list_push);
    writer_.emit_u8(dst);
    writer_.emit_u8(*fill_reg);

    const auto one_const = writer_.add_constant(slot_value{uint64_t{1}});
    auto one_reg = alloc_register(init.span);
    if (!one_reg.has_value()) {
      return std::unexpected(one_reg.error());
    }
    writer_.emit_opcode(opcode::op_load_const);
    writer_.emit_u8(*one_reg);
    writer_.emit_u16(one_const);
    writer_.emit_opcode(opcode::op_add);
    writer_.emit_u8(*idx_reg);
    writer_.emit_u8(*idx_reg);
    writer_.emit_u8(*one_reg);
    writer_.emit_numeric_kind(*count_kind);

    writer_.emit_opcode(opcode::op_jump);
    const auto after_operand =
        static_cast<int64_t>(writer_.current_offset()) + 4;
    const auto back_offset =
        static_cast<int32_t>(static_cast<int64_t>(loop_start) - after_operand);
    writer_.emit_i32(back_offset);

    writer_.patch_jump_to_here(exit_placeholder);
    return {};
  }

  /// A container's runtime element count (`for`-loop/`while`-loop bound
  /// checking over a `list`/`str`, whose length isn't statically known) —
  /// both layouts (`src/runtime/layout.h`) put their length at slot 0, so
  /// this is the same one `op_load_slot` regardless of which container
  /// kind `node.object` actually is.
  [[nodiscard]] auto compile_container_len(const hir::hir_container_len &node,
                                           uint8_t dst)
      -> std::expected<void, compile_error> {
    auto object_reg = compile_expr(*node.object);
    if (!object_reg.has_value()) {
      return std::unexpected(object_reg.error());
    }
    writer_.emit_opcode(opcode::op_load_slot);
    writer_.emit_u8(dst);
    writer_.emit_u8(*object_reg);
    writer_.emit_u16(0);
    return {};
  }

  [[nodiscard]] auto compile_field(const hir::hir_field &field, uint8_t dst)
      -> std::expected<void, compile_error> {
    const auto slot = runtime::struct_field_slot(types_, field.object->type,
                                                 field.field_name);
    if (!slot.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = field.span,
          .message = std::format(
              "field access `.{}` is only supported on struct values by "
              "the bytecode compiler yet",
              field.field_name)});
    }
    auto object_reg = compile_expr(*field.object);
    if (!object_reg.has_value()) {
      return std::unexpected(object_reg.error());
    }
    writer_.emit_opcode(opcode::op_load_slot);
    writer_.emit_u8(dst);
    writer_.emit_u8(*object_reg);
    writer_.emit_u16(static_cast<uint16_t>(*slot));
    return {};
  }

  [[nodiscard]] auto compile_tuple_index(const hir::hir_tuple_index &node,
                                         uint8_t dst)
      -> std::expected<void, compile_error> {
    auto object_reg = compile_expr(*node.object);
    if (!object_reg.has_value()) {
      return std::unexpected(object_reg.error());
    }
    writer_.emit_opcode(opcode::op_load_slot);
    writer_.emit_u8(dst);
    writer_.emit_u8(*object_reg);
    writer_.emit_u16(static_cast<uint16_t>(node.index));
    return {};
  }

  /// Whether `id` is `str`, `slice[T]`, `slice_mut[T]`, or `array[byte, N]`
  /// — the kinds `compile_range_index` can slice by pure pointer arithmetic
  /// without needing an element-size-aware copy. The first three share a
  /// byte-addressed 2-slot `{ len; data }` header (`src/runtime/io.h`),
  /// where `data` is a raw byte pointer (confirmed by `op_load_str_const`'s
  /// own construction of a `str` value and `bytecode::vm.cpp`'s
  /// `bytes_of`); `array[byte, N]` has no such header (a fixed array's
  /// "data pointer" is the object itself, its length the statically-known
  /// `N` — see `compile_index`), but is *also* byte-addressed, because
  /// `is_byte_array_type` gives it its own tightly-packed (not slot-packed)
  /// representation for exactly this reason. Every other `array[T,N]`/
  /// `list[T]` stores one 8-byte slot per element regardless of `T`
  /// (`compile_array_init`/`compile_list_init`) and so cannot be
  /// range-indexed this way.
  [[nodiscard]] auto is_byte_addressed_slice_type(type_id id) const -> bool {
    const auto &entry = types_.entry(strip_refs(id));
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
  /// pointing `data` into the source's own backing bytes, no copy. For the
  /// three header-based kinds, `len`/`data` are read from the source's own
  /// heap header; for `array[byte, N]` (`is_byte_array_type`), there is no
  /// header to read — `len` is the statically-known `N` and `data` is the
  /// array's own pointer, mirroring `compile_index`'s non-range element
  /// access exactly. Returning a *view* rather than a copy matters for
  /// correctness, not just performance: `std.io.reader::read_to_end`'s
  /// `self.read(&mut buf[0..4096])` passes this as a `&mut` out-parameter
  /// that `rt_read` writes through, and `read_to_end` reads the result back
  /// out of `buf` itself afterward — a copy-based view would silently drop
  /// every byte `rt_read` wrote.
  [[nodiscard]] auto compile_range_index(const hir::hir_index &node,
                                         const hir::hir_binary &range,
                                         uint8_t dst)
      -> std::expected<void, compile_error> {
    if (range.lhs == nullptr || range.rhs == nullptr) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = node.span,
          .message = "a range used as an index needs both a start and an "
                     "end bound"});
    }
    auto object_reg = compile_expr(*node.object);
    if (!object_reg.has_value()) {
      return std::unexpected(object_reg.error());
    }
    auto start_reg = compile_expr(*range.lhs);
    if (!start_reg.has_value()) {
      return std::unexpected(start_reg.error());
    }
    auto end_reg = compile_expr(*range.rhs);
    if (!end_reg.has_value()) {
      return std::unexpected(end_reg.error());
    }
    // Normalize `a..=b` (an inclusive bound, `b` itself a valid index) to
    // the same exclusive-end shape `a..b` already is (`b` itself out of
    // bounds) by computing `b + 1` once, up front — everything below this
    // point then treats `end_reg` uniformly regardless of which range form
    // produced it.
    if (range.op == ast::binary_op::range_inclusive) {
      const auto one_const = writer_.add_constant(slot_value{uint64_t{1}});
      auto one_reg = alloc_register(node.span);
      if (!one_reg.has_value()) {
        return std::unexpected(one_reg.error());
      }
      writer_.emit_opcode(opcode::op_load_const);
      writer_.emit_u8(*one_reg);
      writer_.emit_u16(one_const);
      auto inclusive_end_reg = alloc_register(node.span);
      if (!inclusive_end_reg.has_value()) {
        return std::unexpected(inclusive_end_reg.error());
      }
      writer_.emit_opcode(opcode::op_add);
      writer_.emit_u8(*inclusive_end_reg);
      writer_.emit_u8(*end_reg);
      writer_.emit_u8(*one_reg);
      writer_.emit_numeric_kind(numeric_kind::u64);
      end_reg = inclusive_end_reg;
    }

    uint8_t len_reg;
    uint8_t data_reg;
    if (is_byte_array_type(node.object->type)) {
      const auto len_const =
          writer_.add_constant(slot_value{static_cast<uint64_t>(
              *types_.entry(strip_refs(node.object->type)).array_size)});
      auto len_reg_exp = alloc_register(node.span);
      if (!len_reg_exp.has_value()) {
        return std::unexpected(len_reg_exp.error());
      }
      writer_.emit_opcode(opcode::op_load_const);
      writer_.emit_u8(*len_reg_exp);
      writer_.emit_u16(len_const);
      len_reg = *len_reg_exp;
      data_reg = *object_reg;
    } else {
      auto len_reg_exp = alloc_register(node.span);
      if (!len_reg_exp.has_value()) {
        return std::unexpected(len_reg_exp.error());
      }
      writer_.emit_opcode(opcode::op_load_slot);
      writer_.emit_u8(*len_reg_exp);
      writer_.emit_u8(*object_reg);
      writer_.emit_u16(0);
      len_reg = *len_reg_exp;

      auto data_reg_exp = alloc_register(node.span);
      if (!data_reg_exp.has_value()) {
        return std::unexpected(data_reg_exp.error());
      }
      writer_.emit_opcode(opcode::op_load_slot);
      writer_.emit_u8(*data_reg_exp);
      writer_.emit_u8(*object_reg);
      writer_.emit_u16(1);
      data_reg = *data_reg_exp;
    }

    // `end > len` and `start > end` are both out-of-bounds — checked as two
    // separate panics (rather than one combined condition) so an inverted
    // range still reports as an inverted range even when `start` also
    // happens to exceed `len`.
    const auto emit_bounds_check =
        [&](uint8_t lhs_reg,
            uint8_t rhs_reg) -> std::expected<void, compile_error> {
      auto oob_reg = alloc_register(node.span);
      if (!oob_reg.has_value()) {
        return std::unexpected(oob_reg.error());
      }
      writer_.emit_opcode(opcode::op_gt);
      writer_.emit_u8(*oob_reg);
      writer_.emit_u8(lhs_reg);
      writer_.emit_u8(rhs_reg);
      writer_.emit_numeric_kind(numeric_kind::u64);
      writer_.emit_opcode(opcode::op_panic_if);
      writer_.emit_u8(*oob_reg);
      writer_.emit_u8(
          static_cast<uint8_t>(bytecode::panic_reason::index_out_of_bounds));
      return {};
    };
    if (auto result = emit_bounds_check(*end_reg, len_reg);
        !result.has_value()) {
      return std::unexpected(result.error());
    }
    if (auto result = emit_bounds_check(*start_reg, *end_reg);
        !result.has_value()) {
      return std::unexpected(result.error());
    }

    auto new_data_reg_exp = alloc_register(node.span);
    if (!new_data_reg_exp.has_value()) {
      return std::unexpected(new_data_reg_exp.error());
    }
    writer_.emit_opcode(opcode::op_add);
    writer_.emit_u8(*new_data_reg_exp);
    writer_.emit_u8(data_reg);
    writer_.emit_u8(*start_reg);
    writer_.emit_numeric_kind(numeric_kind::u64);

    auto new_len_reg_exp = alloc_register(node.span);
    if (!new_len_reg_exp.has_value()) {
      return std::unexpected(new_len_reg_exp.error());
    }
    writer_.emit_opcode(opcode::op_sub);
    writer_.emit_u8(*new_len_reg_exp);
    writer_.emit_u8(*end_reg);
    writer_.emit_u8(*start_reg);
    writer_.emit_numeric_kind(numeric_kind::u64);

    writer_.emit_opcode(opcode::op_alloc);
    writer_.emit_u8(dst);
    writer_.emit_u16(2);
    writer_.emit_opcode(opcode::op_store_slot);
    writer_.emit_u8(dst);
    writer_.emit_u16(0);
    writer_.emit_u8(*new_len_reg_exp);
    writer_.emit_opcode(opcode::op_store_slot);
    writer_.emit_u8(dst);
    writer_.emit_u16(1);
    writer_.emit_u8(*new_data_reg_exp);
    return {};
  }

  /// Fixed `array[T, N]` and growable `list[T]` element access —
  /// `slice`/`str` indexing still needs a byte/view model this bytecode
  /// compiler doesn't have yet and is rejected here for now. A fixed
  /// array's elements live directly in its own heap block, so its "data
  /// pointer" is the object itself and its length is the statically-known
  /// `N`; a list's elements live in a separate block reached through its
  /// 3-slot header's `data` slot (`src/runtime/layout.h`), with its length
  /// read from the header's `len` slot at runtime — everything past that
  /// point (the bounds check, the indexed load) is shared.
  [[nodiscard]] auto compile_index(const hir::hir_index &node, uint8_t dst)
      -> std::expected<void, compile_error> {
    if (node.index != nullptr &&
        node.index->kind == hir_node_kind::hir_binary) {
      const auto &maybe_range =
          dynamic_cast<const hir::hir_binary &>(*node.index);
      if (maybe_range.op == ast::binary_op::range ||
          maybe_range.op == ast::binary_op::range_inclusive) {
        if (!is_byte_addressed_slice_type(node.object->type)) {
          return std::unexpected(compile_error{
              .kind = compile_error_kind::unsupported_construct,
              .span = node.span,
              .message =
                  "range-indexing is only supported on `str`/`slice`/"
                  "`slice_mut`/`array[byte, N]` yet — any other fixed array "
                  "or list source needs an element-size-aware view model "
                  "this bytecode compiler doesn't have yet"});
        }
        return compile_range_index(node, maybe_range, dst);
      }
    }
    const auto &object_entry = types_.entry(strip_refs(node.object->type));
    const bool indexing_list = is_list_type(node.object->type);
    if (!indexing_list &&
        (object_entry.kind != semantic::type_kind::array_kind ||
         !object_entry.array_size.has_value())) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = node.span,
          .message = "indexing is only supported for a fixed-size array "
                     "with a statically known length, or a list, yet — "
                     "slice/str indexing needs a byte/view model this "
                     "bytecode compiler doesn't have yet"});
    }
    auto object_reg = compile_expr(*node.object);
    if (!object_reg.has_value()) {
      return std::unexpected(object_reg.error());
    }
    auto index_reg = compile_expr(*node.index);
    if (!index_reg.has_value()) {
      return std::unexpected(index_reg.error());
    }

    uint8_t len_reg;
    uint8_t data_reg;
    if (indexing_list) {
      auto len_reg_exp = alloc_register(node.span);
      if (!len_reg_exp.has_value()) {
        return std::unexpected(len_reg_exp.error());
      }
      writer_.emit_opcode(opcode::op_load_slot);
      writer_.emit_u8(*len_reg_exp);
      writer_.emit_u8(*object_reg);
      writer_.emit_u16(0);
      len_reg = *len_reg_exp;

      auto data_reg_exp = alloc_register(node.span);
      if (!data_reg_exp.has_value()) {
        return std::unexpected(data_reg_exp.error());
      }
      writer_.emit_opcode(opcode::op_load_slot);
      writer_.emit_u8(*data_reg_exp);
      writer_.emit_u8(*object_reg);
      writer_.emit_u16(2);
      data_reg = *data_reg_exp;
    } else {
      const auto len_const = writer_.add_constant(
          slot_value{static_cast<uint64_t>(*object_entry.array_size)});
      auto len_reg_exp = alloc_register(node.span);
      if (!len_reg_exp.has_value()) {
        return std::unexpected(len_reg_exp.error());
      }
      writer_.emit_opcode(opcode::op_load_const);
      writer_.emit_u8(*len_reg_exp);
      writer_.emit_u16(len_const);
      len_reg = *len_reg_exp;
      data_reg = *object_reg;
    }

    auto oob_reg = alloc_register(node.span);
    if (!oob_reg.has_value()) {
      return std::unexpected(oob_reg.error());
    }
    writer_.emit_opcode(opcode::op_ge);
    writer_.emit_u8(*oob_reg);
    writer_.emit_u8(*index_reg);
    writer_.emit_u8(len_reg);
    writer_.emit_numeric_kind(numeric_kind::u64);
    writer_.emit_opcode(opcode::op_panic_if);
    writer_.emit_u8(*oob_reg);
    writer_.emit_u8(
        static_cast<uint8_t>(bytecode::panic_reason::index_out_of_bounds));

    writer_.emit_opcode(is_byte_array_type(node.object->type)
                            ? opcode::op_load_byte_indexed
                            : opcode::op_load_indexed);
    writer_.emit_u8(dst);
    writer_.emit_u8(data_reg);
    writer_.emit_u8(*index_reg);
    return {};
  }

  /// Sum-type variant construction `@variant(args...)`: allocates a heap
  /// block sized to the widest variant's payload (so a later `match` can
  /// safely reinterpret it as any variant), stores the runtime tag at
  /// slot 0, then each argument at its 1-based payload slot.
  [[nodiscard]] auto compile_variant_init(const hir::hir_variant_init &init,
                                          uint8_t dst)
      -> std::expected<void, compile_error> {
    const auto tag =
        runtime::sum_variant_tag(types_, init.type, init.variant_name);
    if (!tag.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = init.span,
          .message = std::format(
              "variant `{}` does not resolve to a declared sum-type "
              "variant — this should have been rejected by the type "
              "checker",
              init.variant_name)});
    }
    const auto payload_slots =
        runtime::sum_max_payload_slots(types_, init.type);
    writer_.emit_opcode(opcode::op_alloc);
    writer_.emit_u8(dst);
    writer_.emit_u16(static_cast<uint16_t>(1 + payload_slots));

    const auto tag_const =
        writer_.add_constant(slot_value{static_cast<int64_t>(*tag)});
    auto tag_reg = alloc_register(init.span);
    if (!tag_reg.has_value()) {
      return std::unexpected(tag_reg.error());
    }
    writer_.emit_opcode(opcode::op_load_const);
    writer_.emit_u8(*tag_reg);
    writer_.emit_u16(tag_const);
    writer_.emit_opcode(opcode::op_store_slot);
    writer_.emit_u8(dst);
    writer_.emit_u16(0);
    writer_.emit_u8(*tag_reg);

    for (size_t i = 0; i < init.args.size(); ++i) {
      auto value_reg = compile_expr(*init.args[i]);
      if (!value_reg.has_value()) {
        return std::unexpected(value_reg.error());
      }
      writer_.emit_opcode(opcode::op_store_slot);
      writer_.emit_u8(dst);
      writer_.emit_u16(static_cast<uint16_t>(1 + i));
      writer_.emit_u8(*value_reg);
    }
    return {};
  }

  /// Sum-type payload projection: well-defined only where a
  /// `hir_constructor_pattern` has already confirmed the runtime tag
  /// matches `variant_name` (see the node's own doc comment) — codegen
  /// just reads the corresponding payload slot without re-checking the tag.
  [[nodiscard]] auto
  compile_variant_payload(const hir::hir_variant_payload &node, uint8_t dst)
      -> std::expected<void, compile_error> {
    const auto slot = runtime::sum_variant_payload_slots(
        types_, node.object->type, node.variant_name);
    if (!slot.has_value() || node.index >= *slot) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = node.span,
          .message = std::format(
              "variant `{}` does not resolve to a declared sum-type "
              "variant with a payload at index {} — this should have "
              "been rejected by the type checker",
              node.variant_name, node.index)});
    }
    auto object_reg = compile_expr(*node.object);
    if (!object_reg.has_value()) {
      return std::unexpected(object_reg.error());
    }
    writer_.emit_opcode(opcode::op_load_slot);
    writer_.emit_u8(dst);
    writer_.emit_u8(*object_reg);
    writer_.emit_u16(static_cast<uint16_t>(1 + node.index));
    return {};
  }

  // ------------------------------------------------------------------
  //  match + patterns (spec/codegen-design.md increment 4). Every pattern
  //  kind compiles to a boolean register: a scalar comparison for literals,
  //  a runtime tag check for a constructor pattern, or `op_bitand`/
  //  `op_bitor`-combined sub-tests for the structural kinds (tuple/array/
  //  or). `op_bitand`/`op_bitor` — not short-circuit jumps — are used to
  //  combine sub-tests because every structural pattern's sub-positions are
  //  always safe to test unconditionally (arity is already checker-
  //  verified; see each pattern struct's own doc comment in hir/nodes.h),
  //  so there is no side effect or safety reason to avoid evaluating all of
  //  them.
  //
  //  `value_type` is the statically-tracked type of the value being tested,
  //  when known. Tuple/array element types come straight from the
  //  subject's own interned `type_entry` (`args`/`result`), needing no
  //  further resolution. Struct field types and sum-variant payload types
  //  are *not* tracked this increment — `runtime::layout.h` deliberately
  //  only exposes field/variant *order*, not resolved field *types* (see
  //  its own doc comment), and resolving them would need
  //  `semantic::check.cpp`'s private, generic-substitution-aware
  //  `struct_field_type`/`variant_payload_types`, which isn't exposed
  //  outside the checker. So a struct/constructor sub-pattern is only
  //  supported when it's a wildcard/binding (`hir_wildcard_pattern` — the
  //  common case, e.g. `{x, y}` or `@some(x)`); anything else at that
  //  position (a literal, a nested pattern) is rejected with a clear
  //  "not supported yet" error rather than silently mistyped.
  // ------------------------------------------------------------------

  [[nodiscard]] auto compile_pattern_test(const hir::hir_pattern &pattern,
                                          uint8_t value_reg,
                                          std::optional<type_id> value_type)
      -> std::expected<uint8_t, compile_error> {
    switch (pattern.kind) {
    case hir_node_kind::hir_wildcard_pattern: {
      auto reg = alloc_register(pattern.span);
      if (!reg.has_value()) {
        return std::unexpected(reg.error());
      }
      const auto idx = writer_.add_constant(slot_value{uint64_t{1}});
      writer_.emit_opcode(opcode::op_load_const);
      writer_.emit_u8(*reg);
      writer_.emit_u16(idx);
      return *reg;
    }
    case hir_node_kind::hir_literal_pattern: {
      const auto &lit = dynamic_cast<const hir::hir_literal_pattern &>(pattern);
      if (!value_type.has_value()) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "a literal pattern here needs a statically tracked "
                       "subject type, which this position doesn't have "
                       "yet (see compile_pattern_test's doc comment)"});
      }
      if (lit.lit_kind == token_kind::string_lit) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "string literal patterns need increment 5's "
                       "growable-string comparison, not supported yet"});
      }
      auto kind = numeric_kind_for(*value_type, pattern.span);
      if (!kind.has_value()) {
        return std::unexpected(kind.error());
      }
      auto encoded =
          encode_literal(lit.lit_kind, lit.value, pattern.span, *kind);
      if (!encoded.has_value()) {
        return std::unexpected(encoded.error());
      }
      auto const_reg = alloc_register(pattern.span);
      if (!const_reg.has_value()) {
        return std::unexpected(const_reg.error());
      }
      const auto idx = writer_.add_constant(*encoded);
      writer_.emit_opcode(opcode::op_load_const);
      writer_.emit_u8(*const_reg);
      writer_.emit_u16(idx);
      auto result_reg = alloc_register(pattern.span);
      if (!result_reg.has_value()) {
        return std::unexpected(result_reg.error());
      }
      writer_.emit_opcode(opcode::op_eq);
      writer_.emit_u8(*result_reg);
      writer_.emit_u8(value_reg);
      writer_.emit_u8(*const_reg);
      writer_.emit_numeric_kind(*kind);
      return *result_reg;
    }
    case hir_node_kind::hir_or_pattern: {
      const auto &or_pat = dynamic_cast<const hir::hir_or_pattern &>(pattern);
      if (or_pat.alternatives.empty()) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "an `or` pattern needs at least one alternative"});
      }
      auto result =
          compile_pattern_test(*or_pat.alternatives[0], value_reg, value_type);
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      const auto result_reg = *result;
      for (size_t i = 1; i < or_pat.alternatives.size(); ++i) {
        auto alt = compile_pattern_test(*or_pat.alternatives[i], value_reg,
                                        value_type);
        if (!alt.has_value()) {
          return std::unexpected(alt.error());
        }
        writer_.emit_opcode(opcode::op_bitor);
        writer_.emit_u8(result_reg);
        writer_.emit_u8(result_reg);
        writer_.emit_u8(*alt);
        writer_.emit_numeric_kind(numeric_kind::boolean);
      }
      return result_reg;
    }
    case hir_node_kind::hir_tuple_pattern: {
      const auto &tup = dynamic_cast<const hir::hir_tuple_pattern &>(pattern);
      if (!value_type.has_value()) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "a tuple pattern here needs a statically tracked "
                       "subject type, which this position doesn't have yet"});
      }
      const auto &entry = types_.entry(*value_type);
      auto result_reg = std::optional<uint8_t>{};
      for (size_t i = 0; i < tup.elements.size(); ++i) {
        auto elem_reg = alloc_register(pattern.span);
        if (!elem_reg.has_value()) {
          return std::unexpected(elem_reg.error());
        }
        writer_.emit_opcode(opcode::op_load_slot);
        writer_.emit_u8(*elem_reg);
        writer_.emit_u8(value_reg);
        writer_.emit_u16(static_cast<uint16_t>(i));
        const auto elem_type = i < entry.args.size()
                                   ? std::optional<type_id>(entry.args[i])
                                   : std::nullopt;
        auto sub = compile_pattern_test(*tup.elements[i], *elem_reg, elem_type);
        if (!sub.has_value()) {
          return std::unexpected(sub.error());
        }
        if (!result_reg.has_value()) {
          result_reg = *sub;
        } else {
          writer_.emit_opcode(opcode::op_bitand);
          writer_.emit_u8(*result_reg);
          writer_.emit_u8(*result_reg);
          writer_.emit_u8(*sub);
          writer_.emit_numeric_kind(numeric_kind::boolean);
        }
      }
      if (!result_reg.has_value()) {
        auto reg = alloc_register(pattern.span);
        if (!reg.has_value()) {
          return std::unexpected(reg.error());
        }
        const auto idx = writer_.add_constant(slot_value{uint64_t{1}});
        writer_.emit_opcode(opcode::op_load_const);
        writer_.emit_u8(*reg);
        writer_.emit_u16(idx);
        return *reg;
      }
      return *result_reg;
    }
    case hir_node_kind::hir_array_pattern: {
      const auto &arr = dynamic_cast<const hir::hir_array_pattern &>(pattern);
      if (!value_type.has_value()) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "an array pattern here needs a statically tracked "
                       "subject type, which this position doesn't have yet"});
      }
      const auto &entry = types_.entry(*value_type);
      auto result_reg = std::optional<uint8_t>{};
      for (size_t i = 0; i < arr.elements.size(); ++i) {
        auto elem_reg = alloc_register(pattern.span);
        if (!elem_reg.has_value()) {
          return std::unexpected(elem_reg.error());
        }
        writer_.emit_opcode(opcode::op_load_slot);
        writer_.emit_u8(*elem_reg);
        writer_.emit_u8(value_reg);
        writer_.emit_u16(static_cast<uint16_t>(i));
        auto sub = compile_pattern_test(*arr.elements[i], *elem_reg,
                                        std::optional<type_id>(entry.result));
        if (!sub.has_value()) {
          return std::unexpected(sub.error());
        }
        if (!result_reg.has_value()) {
          result_reg = *sub;
        } else {
          writer_.emit_opcode(opcode::op_bitand);
          writer_.emit_u8(*result_reg);
          writer_.emit_u8(*result_reg);
          writer_.emit_u8(*sub);
          writer_.emit_numeric_kind(numeric_kind::boolean);
        }
      }
      if (!result_reg.has_value()) {
        auto reg = alloc_register(pattern.span);
        if (!reg.has_value()) {
          return std::unexpected(reg.error());
        }
        const auto idx = writer_.add_constant(slot_value{uint64_t{1}});
        writer_.emit_opcode(opcode::op_load_const);
        writer_.emit_u8(*reg);
        writer_.emit_u16(idx);
        return *reg;
      }
      return *result_reg;
    }
    case hir_node_kind::hir_struct_pattern: {
      const auto &st = dynamic_cast<const hir::hir_struct_pattern &>(pattern);
      for (const auto &field : st.fields) {
        if (field.pattern->kind != hir_node_kind::hir_wildcard_pattern) {
          return std::unexpected(compile_error{
              .kind = compile_error_kind::unsupported_construct,
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
      auto reg = alloc_register(pattern.span);
      if (!reg.has_value()) {
        return std::unexpected(reg.error());
      }
      const auto idx = writer_.add_constant(slot_value{uint64_t{1}});
      writer_.emit_opcode(opcode::op_load_const);
      writer_.emit_u8(*reg);
      writer_.emit_u16(idx);
      return *reg;
    }
    case hir_node_kind::hir_constructor_pattern: {
      const auto &ctor =
          dynamic_cast<const hir::hir_constructor_pattern &>(pattern);
      if (!value_type.has_value()) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "a constructor pattern here needs a statically "
                       "tracked subject type, which this position doesn't "
                       "have yet"});
      }
      const auto tag =
          runtime::sum_variant_tag(types_, *value_type, ctor.variant_name);
      if (!tag.has_value()) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = std::format(
                "variant `{}` does not resolve to a declared sum-type "
                "variant — this should have been rejected by the type "
                "checker",
                ctor.variant_name)});
      }
      for (const auto &arg : ctor.args) {
        if (arg->kind != hir_node_kind::hir_wildcard_pattern) {
          return std::unexpected(compile_error{
              .kind = compile_error_kind::unsupported_construct,
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
      auto tag_reg = alloc_register(pattern.span);
      if (!tag_reg.has_value()) {
        return std::unexpected(tag_reg.error());
      }
      writer_.emit_opcode(opcode::op_load_slot);
      writer_.emit_u8(*tag_reg);
      writer_.emit_u8(value_reg);
      writer_.emit_u16(0);
      auto const_reg = alloc_register(pattern.span);
      if (!const_reg.has_value()) {
        return std::unexpected(const_reg.error());
      }
      const auto idx =
          writer_.add_constant(slot_value{static_cast<int64_t>(*tag)});
      writer_.emit_opcode(opcode::op_load_const);
      writer_.emit_u8(*const_reg);
      writer_.emit_u16(idx);
      auto result_reg = alloc_register(pattern.span);
      if (!result_reg.has_value()) {
        return std::unexpected(result_reg.error());
      }
      writer_.emit_opcode(opcode::op_eq);
      writer_.emit_u8(*result_reg);
      writer_.emit_u8(*tag_reg);
      writer_.emit_u8(*const_reg);
      writer_.emit_numeric_kind(numeric_kind::i64);
      return *result_reg;
    }
    case hir_node_kind::hir_range_pattern: {
      const auto &range = dynamic_cast<const hir::hir_range_pattern &>(pattern);
      if (!value_type.has_value()) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_construct,
            .span = pattern.span,
            .message = "a range pattern here needs a statically tracked "
                       "subject type, which this position doesn't have yet"});
      }
      auto kind = numeric_kind_for(*value_type, pattern.span);
      if (!kind.has_value()) {
        return std::unexpected(kind.error());
      }
      auto result_reg = std::optional<uint8_t>{};
      if (range.start != nullptr) {
        auto start_reg = compile_expr(*range.start);
        if (!start_reg.has_value()) {
          return std::unexpected(start_reg.error());
        }
        auto cmp_reg = alloc_register(pattern.span);
        if (!cmp_reg.has_value()) {
          return std::unexpected(cmp_reg.error());
        }
        writer_.emit_opcode(opcode::op_ge);
        writer_.emit_u8(*cmp_reg);
        writer_.emit_u8(value_reg);
        writer_.emit_u8(*start_reg);
        writer_.emit_numeric_kind(*kind);
        result_reg = *cmp_reg;
      }
      if (range.end != nullptr) {
        auto end_reg = compile_expr(*range.end);
        if (!end_reg.has_value()) {
          return std::unexpected(end_reg.error());
        }
        auto cmp_reg = alloc_register(pattern.span);
        if (!cmp_reg.has_value()) {
          return std::unexpected(cmp_reg.error());
        }
        writer_.emit_opcode(range.inclusive ? opcode::op_le : opcode::op_lt);
        writer_.emit_u8(*cmp_reg);
        writer_.emit_u8(value_reg);
        writer_.emit_u8(*end_reg);
        writer_.emit_numeric_kind(*kind);
        if (result_reg.has_value()) {
          writer_.emit_opcode(opcode::op_bitand);
          writer_.emit_u8(*result_reg);
          writer_.emit_u8(*result_reg);
          writer_.emit_u8(*cmp_reg);
          writer_.emit_numeric_kind(numeric_kind::boolean);
        } else {
          result_reg = *cmp_reg;
        }
      }
      if (!result_reg.has_value()) {
        auto reg = alloc_register(pattern.span);
        if (!reg.has_value()) {
          return std::unexpected(reg.error());
        }
        const auto idx = writer_.add_constant(slot_value{uint64_t{1}});
        writer_.emit_opcode(opcode::op_load_const);
        writer_.emit_u8(*reg);
        writer_.emit_u16(idx);
        return *reg;
      }
      return *result_reg;
    }
    default:
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = pattern.span,
          .message = "this pattern form is not supported by the bytecode "
                     "compiler yet"});
    }
  }

  /// `match`, usable as either a statement (`want_result == nullopt`) or an
  /// expression (`want_result` names the destination register) — mirrors
  /// `compile_if`'s own shape. Each arm compiles to "test the pattern (and
  /// guard, if any, short-circuited exactly like `and`), jump past the body
  /// if it fails, else run the body and jump to the end." A trailing
  /// `op_panic` guards the (checker-guaranteed-unreachable) case where no
  /// arm matched, the same defense-in-depth role `op_panic_if` plays for
  /// array bounds.
  [[nodiscard]] auto compile_match(const hir::hir_match &match,
                                   std::optional<uint8_t> want_result)
      -> std::expected<void, compile_error> {
    auto subject_reg = compile_expr(*match.subject);
    if (!subject_reg.has_value()) {
      return std::unexpected(subject_reg.error());
    }
    locals_.emplace(match.subject_symbol, *subject_reg);
    const auto subject_type = std::optional<type_id>(match.subject->type);

    auto end_placeholders = std::vector<size_t>{};
    for (const auto &arm : match.arms) {
      auto test_reg =
          compile_pattern_test(*arm.pattern, *subject_reg, subject_type);
      if (!test_reg.has_value()) {
        return std::unexpected(test_reg.error());
      }
      if (arm.guard != nullptr) {
        writer_.emit_opcode(opcode::op_jump_if_false);
        writer_.emit_u8(*test_reg);
        const auto guard_skip = writer_.emit_jump_placeholder();
        if (auto result = compile_expr_into(*arm.guard, *test_reg);
            !result.has_value()) {
          return std::unexpected(result.error());
        }
        writer_.patch_jump_to_here(guard_skip);
      }
      writer_.emit_opcode(opcode::op_jump_if_false);
      writer_.emit_u8(*test_reg);
      const auto next_arm = writer_.emit_jump_placeholder();

      if (auto result = compile_block_as_value(*arm.body, want_result);
          !result.has_value()) {
        return std::unexpected(result.error());
      }
      writer_.emit_opcode(opcode::op_jump);
      end_placeholders.push_back(writer_.emit_jump_placeholder());
      writer_.patch_jump_to_here(next_arm);
    }
    writer_.emit_opcode(opcode::op_panic);
    for (const auto placeholder : end_placeholders) {
      writer_.patch_jump_to_here(placeholder);
    }
    return {};
  }

  // ------------------------------------------------------------------
  //  Statements and blocks.
  // ------------------------------------------------------------------

  /// Compiles every statement in `block`. When `want_result` is set and the
  /// block's last statement is a `hir_expr_stmt` (the surface shape a
  /// trailing value-producing expression takes — see `hir_expr_stmt`'s doc
  /// comment), that expression is compiled directly into `*want_result`
  /// instead of being evaluated for its side effect and discarded.
  [[nodiscard]] auto compile_block_as_value(const hir::hir_block &block,
                                            std::optional<uint8_t> want_result)
      -> std::expected<void, compile_error> {
    const auto &stmts = block.stmts;
    for (size_t i = 0; i < stmts.size(); ++i) {
      const auto &stmt = *stmts[i];
      const auto is_last = i + 1 == stmts.size();
      if (is_last && want_result.has_value() &&
          stmt.kind == hir_node_kind::hir_expr_stmt) {
        const auto &expr_stmt = dynamic_cast<const hir::hir_expr_stmt &>(stmt);
        if (auto result = compile_expr_into(*expr_stmt.expr, *want_result);
            !result.has_value()) {
          return std::unexpected(result.error());
        }
        continue;
      }
      if (auto result = compile_stmt(stmt); !result.has_value()) {
        return std::unexpected(result.error());
      }
    }
    return {};
  }

  [[nodiscard]] auto compile_stmt(const hir_node &node)
      -> std::expected<void, compile_error> {
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
      return {};
    }
    case hir_node_kind::hir_return: {
      const auto &ret = dynamic_cast<const hir::hir_return &>(node);
      if (ret.value == nullptr) {
        writer_.emit_opcode(opcode::op_return_unit);
        return {};
      }
      auto reg = compile_expr(*ret.value);
      if (!reg.has_value()) {
        return std::unexpected(reg.error());
      }
      writer_.emit_opcode(opcode::op_return_value);
      writer_.emit_u8(*reg);
      return {};
    }
    case hir_node_kind::hir_while:
      return compile_while(dynamic_cast<const hir::hir_while &>(node));
    case hir_node_kind::hir_if:
      return compile_if(dynamic_cast<const hir::hir_if &>(node), std::nullopt);
    case hir_node_kind::hir_match:
      return compile_match(dynamic_cast<const hir::hir_match &>(node),
                           std::nullopt);
    case hir_node_kind::hir_while_let:
      return compile_while_let(dynamic_cast<const hir::hir_while_let &>(node));
    case hir_node_kind::hir_let_else:
      return compile_let_else(dynamic_cast<const hir::hir_let_else &>(node));
    case hir_node_kind::hir_list_push:
      return compile_list_push(dynamic_cast<const hir::hir_list_push &>(node));
    default:
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = node.span,
          .message = "this statement form is outside the bytecode compiler's "
                     "current scope"});
    }
  }

  [[nodiscard]] auto compile_let(const hir::hir_let &let)
      -> std::expected<void, compile_error> {
    auto reg = alloc_register(let.span);
    if (!reg.has_value()) {
      return std::unexpected(reg.error());
    }
    if (auto result = compile_expr_into(*let.initializer, *reg);
        !result.has_value()) {
      return std::unexpected(result.error());
    }
    locals_.emplace(let.symbol, *reg);
    return {};
  }

  [[nodiscard]] auto compile_assign(const hir::hir_assign &assign)
      -> std::expected<void, compile_error> {
    if (assign.target->kind == hir_node_kind::hir_field) {
      // `self.field = value` — the struct's own value is already a heap
      // pointer (`compile_field`'s own `op_load_slot` reads through it the
      // same way), so writing a field is just `op_store_slot` through that
      // same pointer at the field's statically-known slot index.
      const auto &field = dynamic_cast<const hir::hir_field &>(*assign.target);
      const auto slot = runtime::struct_field_slot(types_, field.object->type,
                                                   field.field_name);
      if (!slot.has_value()) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_construct,
            .span = assign.span,
            .message = std::format(
                "field `.{}` is only assignable on struct values by the "
                "bytecode compiler yet",
                field.field_name)});
      }
      if (assign.op != ast::assign_op::assign) {
        return std::unexpected(compile_error{
            .kind = compile_error_kind::unsupported_construct,
            .span = assign.span,
            .message = "compound assignment to a struct field is not "
                       "supported by the bytecode compiler yet — only plain "
                       "`=` is"});
      }
      auto object_reg = compile_expr(*field.object);
      if (!object_reg.has_value()) {
        return std::unexpected(object_reg.error());
      }
      auto value_reg = compile_expr(*assign.value);
      if (!value_reg.has_value()) {
        return std::unexpected(value_reg.error());
      }
      writer_.emit_opcode(opcode::op_store_slot);
      writer_.emit_u8(*object_reg);
      writer_.emit_u16(static_cast<uint16_t>(*slot));
      writer_.emit_u8(*value_reg);
      return {};
    }
    if (assign.target->kind != hir_node_kind::hir_local_ref) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = assign.span,
          .message =
              "assigning to an index target needs a heap/aggregate "
              "representation this bytecode compiler doesn't have yet — only "
              "assigning directly to a local variable or a struct field is "
              "supported"});
    }
    const auto &target =
        dynamic_cast<const hir::hir_local_ref &>(*assign.target);
    const auto reg = lookup_local(target.symbol);
    if (!reg.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = assign.span,
          .message =
              std::format("assignment target `{}` does not resolve to a local "
                          "variable",
                          target.name)});
    }
    if (assign.op == ast::assign_op::assign) {
      return compile_expr_into(*assign.value, *reg);
    }
    const auto op = compound_assign_opcode_for(assign.op);
    if (!op.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = assign.span,
          .message =
              "this compound assignment operator is not supported by the "
              "bytecode compiler yet"});
    }
    auto kind = numeric_kind_for(target.type, assign.span);
    if (!kind.has_value()) {
      return std::unexpected(kind.error());
    }
    auto rhs = compile_expr(*assign.value);
    if (!rhs.has_value()) {
      return std::unexpected(rhs.error());
    }
    writer_.emit_opcode(*op);
    writer_.emit_u8(*reg);
    writer_.emit_u8(*reg);
    writer_.emit_u8(*rhs);
    writer_.emit_numeric_kind(*kind);
    return {};
  }

  [[nodiscard]] auto compile_while(const hir::hir_while &loop)
      -> std::expected<void, compile_error> {
    const auto loop_start = writer_.current_offset();
    auto cond = compile_expr(*loop.condition);
    if (!cond.has_value()) {
      return std::unexpected(cond.error());
    }
    writer_.emit_opcode(opcode::op_jump_if_false);
    writer_.emit_u8(*cond);
    const auto exit_placeholder = writer_.emit_jump_placeholder();

    if (auto result = compile_block_as_value(*loop.body, std::nullopt);
        !result.has_value()) {
      return std::unexpected(result.error());
    }

    writer_.emit_opcode(opcode::op_jump);
    const auto after_operand =
        static_cast<int64_t>(writer_.current_offset()) + 4;
    const auto back_offset =
        static_cast<int32_t>(static_cast<int64_t>(loop_start) - after_operand);
    writer_.emit_i32(back_offset);

    writer_.patch_jump_to_here(exit_placeholder);
    return {};
  }

  /// `while let pattern = expr: body` — like `compile_while`, but the loop
  /// condition is a fresh pattern test against a freshly re-evaluated
  /// `subject` every iteration (see `hir_while_let`'s doc comment) rather
  /// than a plain boolean expression; falls out of the loop the first time
  /// the pattern fails to match, with no `else`.
  [[nodiscard]] auto compile_while_let(const hir::hir_while_let &loop)
      -> std::expected<void, compile_error> {
    const auto loop_start = writer_.current_offset();
    auto subject_reg = compile_expr(*loop.subject);
    if (!subject_reg.has_value()) {
      return std::unexpected(subject_reg.error());
    }
    locals_.insert_or_assign(loop.subject_symbol, *subject_reg);
    const auto subject_type = std::optional<type_id>(loop.subject->type);

    auto test_reg =
        compile_pattern_test(*loop.pattern, *subject_reg, subject_type);
    if (!test_reg.has_value()) {
      return std::unexpected(test_reg.error());
    }
    writer_.emit_opcode(opcode::op_jump_if_false);
    writer_.emit_u8(*test_reg);
    const auto exit_placeholder = writer_.emit_jump_placeholder();

    if (auto result = compile_block_as_value(*loop.body, std::nullopt);
        !result.has_value()) {
      return std::unexpected(result.error());
    }

    writer_.emit_opcode(opcode::op_jump);
    const auto after_operand =
        static_cast<int64_t>(writer_.current_offset()) + 4;
    const auto back_offset =
        static_cast<int32_t>(static_cast<int64_t>(loop_start) - after_operand);
    writer_.emit_i32(back_offset);

    writer_.patch_jump_to_here(exit_placeholder);
    return {};
  }

  /// `let pattern = expr else: block` — evaluates `initializer` into
  /// `subject_symbol` once, tests it against `pattern`, and runs
  /// `else_body` when it fails to match; `pattern`'s own bindings are
  /// ordinary `hir_let`s lowering already spliced in immediately after this
  /// node (see `hir_let_else`'s doc comment), so this only has to compile
  /// the test and the branch, not any binding of its own. `else_body` must
  /// diverge — a language-level invariant the checker enforces, not
  /// something re-verified here (mirrors the node's own doc comment).
  [[nodiscard]] auto compile_let_else(const hir::hir_let_else &node)
      -> std::expected<void, compile_error> {
    auto subject_reg = compile_expr(*node.initializer);
    if (!subject_reg.has_value()) {
      return std::unexpected(subject_reg.error());
    }
    locals_.emplace(node.subject_symbol, *subject_reg);
    const auto subject_type = std::optional<type_id>(node.initializer->type);

    auto test_reg =
        compile_pattern_test(*node.pattern, *subject_reg, subject_type);
    if (!test_reg.has_value()) {
      return std::unexpected(test_reg.error());
    }
    writer_.emit_opcode(opcode::op_jump_if_true);
    writer_.emit_u8(*test_reg);
    const auto continue_placeholder = writer_.emit_jump_placeholder();

    if (auto result = compile_block_as_value(*node.else_body, std::nullopt);
        !result.has_value()) {
      return std::unexpected(result.error());
    }

    writer_.patch_jump_to_here(continue_placeholder);
    return {};
  }

  /// Appends `value` onto the `list[T]` place `target` (see
  /// `hir_list_push`'s doc comment) — the comprehension accumulator's
  /// growth statement.
  [[nodiscard]] auto compile_list_push(const hir::hir_list_push &node)
      -> std::expected<void, compile_error> {
    auto target_reg = compile_expr(*node.target);
    if (!target_reg.has_value()) {
      return std::unexpected(target_reg.error());
    }
    auto value_reg = compile_expr(*node.value);
    if (!value_reg.has_value()) {
      return std::unexpected(value_reg.error());
    }
    writer_.emit_opcode(opcode::op_list_push);
    writer_.emit_u8(*target_reg);
    writer_.emit_u8(*value_reg);
    return {};
  }

  const type_table &types_;
  const std::unordered_map<std::string, uint16_t> &functions_;
  /// Every lambda compiled anywhere in the current module (by this function
  /// or any nested lambda within it) is appended here, shared by reference
  /// across every `function_compiler` instance for the module — lambdas get
  /// stable function-table indices starting right after the module's named
  /// top-level functions (`function_table_base_`), assigned once each
  /// lambda finishes compiling (see `compile_lambda_value`), and
  /// `compile_module` appends this vector to the module's function table
  /// after every top-level function has compiled.
  std::vector<bytecode::bytecode_function> &lambda_functions_;
  size_t function_table_base_;
  std::string entry_module_name_;
  std::string current_module_name_;
  chunk_writer writer_;
  std::unordered_map<hir::symbol_id, uint8_t> locals_;
  size_t next_register_ = 0;
};

} // namespace

auto compile_function(
    const hir::hir_function &fn, const type_table &types,
    const std::unordered_map<std::string, uint16_t> &function_index)
    -> std::expected<bytecode::bytecode_function, compile_error> {
  auto lambda_functions = std::vector<bytecode::bytecode_function>{};
  auto compiler = function_compiler(types, function_index, lambda_functions, 1);
  return compiler.compile(fn);
}

auto compile_module(std::span<const hir::hir_module *const> modules,
                    const type_table &types)
    -> std::expected<bytecode::bytecode_module, compile_error> {
  const auto &entry_name = modules.front()->module_name;

  // Flatten every module's functions into one compiled unit, keyed the same
  // way `function_compiler::resolve_callee_key` computes a callee's lookup
  // key: bare for the entry module, `module::name` for everything else —
  // see `compile_module`'s (span overload) doc comment in compile.h.
  auto function_index = std::unordered_map<std::string, uint16_t>{};
  auto ordered_functions = std::vector<
      std::pair<const hir::hir_module *, const hir::hir_function *>>{};
  for (const auto *module : modules) {
    for (const auto &fn : module->functions) {
      const auto key = module->module_name == entry_name
                           ? fn->name
                           : module->module_name + "::" + fn->name;
      function_index.emplace(key,
                             static_cast<uint16_t>(ordered_functions.size()));
      ordered_functions.emplace_back(module, fn.get());
    }
  }

  // Every lambda encountered while compiling any top-level function (or any
  // lambda nested within one) is appended here and given a stable
  // function-table index starting right after every module's named
  // top-level functions — see `function_compiler::lambda_functions_`'s doc
  // comment.
  auto lambda_functions = std::vector<bytecode::bytecode_function>{};
  const auto function_table_base = ordered_functions.size();

  auto result =
      bytecode::bytecode_module{.module_name = entry_name, .functions = {}};
  result.functions.reserve(ordered_functions.size());
  for (const auto &[module, fn] : ordered_functions) {
    auto compiler =
        function_compiler(types, function_index, lambda_functions,
                          function_table_base, entry_name, module->module_name);
    auto compiled = compiler.compile(*fn);
    if (!compiled.has_value()) {
      return std::unexpected(compiled.error());
    }
    result.functions.push_back(std::move(*compiled));
  }
  for (auto &lambda_fn : lambda_functions) {
    result.functions.push_back(std::move(lambda_fn));
  }
  return result;
}

auto compile_module(const hir::hir_module &module, const type_table &types)
    -> std::expected<bytecode::bytecode_module, compile_error> {
  const auto *module_ptr = &module;
  return compile_module(std::span<const hir::hir_module *const>(&module_ptr, 1),
                        types);
}

} // namespace kira::bytecode_compiler
