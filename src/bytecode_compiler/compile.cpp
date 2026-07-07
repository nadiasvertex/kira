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
#include "src/hir/ids.h"
#include "src/hir/nodes.h"
#include "src/k-parser/ast.h"
#include "src/k-parser/token.h"
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
//  Literal decoding — text/escape handling this compiler owns for itself.
//
//  `semantic::check.cpp` already has an integer-literal parser
//  (`parse_integer_literal`), but it's a local, unexported free function in
//  an anonymous namespace there, not a shared utility — reimplemented here
//  rather than exporting it across an unrelated module boundary for one
//  caller. Char-literal escape decoding has no existing implementation
//  anywhere (the lexer only *validates* escapes, it never needs the decoded
//  scalar value itself), so this is genuinely new, kept intentionally small
//  and mirroring exactly the escape set `src/k-parser/lexer.h`'s
//  `scan_escape_sequence` accepts.
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

/// Decodes one UTF-8 scalar value starting at `text[pos]`, advancing `pos`
/// past it. Mirrors the continuation-byte counting `lexer.h`'s char-literal
/// scanning already relies on to know where a multi-byte character ends —
/// this just also computes the resulting code point instead of only
/// skipping past it.
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

/// Decodes a `char_lit` token's raw text (quotes and all, e.g. `'a'`,
/// `'\n'`, `'\u{1F600}'`) into the Unicode scalar value it names.
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
    // `\u{XXXX}`.
    if (inner.size() < 5 || inner[2] != '{' || inner.back() != '}') {
      return std::nullopt;
    }
    const auto hex = inner.substr(3, inner.size() - 4);
    auto value = uint32_t{0};
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const char *hex_end = hex.data() + hex.size();
    const auto result = std::from_chars(hex.data(), hex_end, value, 16);
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (result.ec != std::errc{} || result.ptr != hex_end) {
      return std::nullopt;
    }
    return value;
  }
  default:
    return std::nullopt;
  }
}

/// Appends `scalar`'s UTF-8 encoding to `out` — the encode-side counterpart
/// to `decode_utf8_scalar` above, needed once a string literal's `\u{...}`
/// escape has to be turned back into bytes for the heap `str` value's data.
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

/// Decodes a `string_lit` token's raw text (opening/closing quotes and all)
/// into the UTF-8 byte content the runtime `str` value should hold. Source
/// bytes that aren't part of an escape sequence are copied through
/// unchanged (multi-byte UTF-8 sequences already in the source need no
/// re-decoding, unlike `decode_char_literal`, which has to produce a single
/// scalar value rather than a byte range) — only `\...` escapes need
/// translating, mirroring the same escape set `decode_char_literal` accepts.
/// String interpolation (`"...{expr}..."`) is not handled here: nothing
/// downstream of the lexer currently splits interpolation into separate
/// expressions (confirmed by grepping `hir::lower.cpp`/`semantic::check.cpp`
/// for any interpolation handling — there is none yet), so a `{`/`}` in the
/// source is treated as a literal character, same as the lexer's own
/// `scan_string` currently does upstream of this pass.
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
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      const char *hex_end = hex.data() + hex.size();
      const auto result = std::from_chars(hex.data(), hex_end, value, 16);
      // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
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

[[nodiscard]] auto encode_f32(float v) -> slot_value {
  return slot_value{static_cast<uint64_t>(std::bit_cast<uint32_t>(v))};
}

/// Encodes one `hir_literal` as the `slot_value` its resolved `numeric_kind`
/// expects. Trusts the checker's own work (`check_integer_fit`,
/// `infer_literal`'s defaulting) rather than re-validating fit or
/// defaulting rules here — this only has to reproduce the *bit pattern*,
/// not re-derive which type the literal ended up with.
[[nodiscard]] auto encode_literal(const hir::hir_literal &lit,
                                  numeric_kind kind)
    -> std::expected<slot_value, compile_error> {
  switch (lit.lit_kind) {
  case token_kind::kw_true:
    return slot_value{uint64_t{1}};
  case token_kind::kw_false:
    return slot_value{uint64_t{0}};
  case token_kind::int_lit: {
    const auto value = parse_uint_literal(lit.value);
    if (!value.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = lit.span,
          .message =
              std::format("could not parse integer literal `{}`", lit.value)});
    }
    if (bytecode::is_float(kind)) {
      const auto as_double = static_cast<double>(*value);
      return kind == numeric_kind::f32
                 ? encode_f32(static_cast<float>(as_double))
                 : slot_value{as_double};
    }
    return slot_value{*value};
  }
  case token_kind::float_lit: {
    const auto value = parse_float_literal(lit.value);
    if (!value.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = lit.span,
          .message =
              std::format("could not parse float literal `{}`", lit.value)});
    }
    return kind == numeric_kind::f32 ? encode_f32(static_cast<float>(*value))
                                     : slot_value{*value};
  }
  case token_kind::char_lit: {
    const auto value = decode_char_literal(lit.value);
    if (!value.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = lit.span,
          .message = std::format("could not decode character literal `{}`",
                                 lit.value)});
    }
    return slot_value{static_cast<uint64_t>(*value)};
  }
  default:
    return std::unexpected(compile_error{
        .kind = compile_error_kind::unsupported_construct,
        .span = lit.span,
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
  case ast::binary_op::EqEq:
    return opcode::op_eq;
  case ast::binary_op::BangEq:
    return opcode::op_ne;
  case ast::binary_op::Lt:
    return opcode::op_lt;
  case ast::binary_op::LtEq:
    return opcode::op_le;
  case ast::binary_op::Gt:
    return opcode::op_gt;
  case ast::binary_op::GtEq:
    return opcode::op_ge;
  case ast::binary_op::Add:
    return opcode::op_add;
  case ast::binary_op::Sub:
    return opcode::op_sub;
  case ast::binary_op::Mul:
    return opcode::op_mul;
  case ast::binary_op::Div:
    return opcode::op_div;
  case ast::binary_op::Mod:
    return opcode::op_mod;
  case ast::binary_op::AddWrap:
    return opcode::op_add_wrap;
  case ast::binary_op::SubWrap:
    return opcode::op_sub_wrap;
  case ast::binary_op::MulWrap:
    return opcode::op_mul_wrap;
  case ast::binary_op::AddSat:
    return opcode::op_add_sat;
  case ast::binary_op::SubSat:
    return opcode::op_sub_sat;
  case ast::binary_op::MulSat:
    return opcode::op_mul_sat;
  case ast::binary_op::Shl:
    return opcode::op_shl;
  case ast::binary_op::Shr:
    return opcode::op_shr;
  case ast::binary_op::BitAnd:
    return opcode::op_bitand;
  case ast::binary_op::BitOr:
    return opcode::op_bitor;
  case ast::binary_op::BitXor:
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
  case ast::assign_op::AddAssign:
    return opcode::op_add;
  case ast::assign_op::SubAssign:
    return opcode::op_sub;
  case ast::assign_op::MulAssign:
    return opcode::op_mul;
  case ast::assign_op::DivAssign:
    return opcode::op_div;
  case ast::assign_op::ModAssign:
    return opcode::op_mod;
  case ast::assign_op::AndAssign:
    return opcode::op_bitand;
  case ast::assign_op::OrAssign:
    return opcode::op_bitor;
  case ast::assign_op::XorAssign:
    return opcode::op_bitxor;
  case ast::assign_op::ShlAssign:
    return opcode::op_shl;
  case ast::assign_op::ShrAssign:
    return opcode::op_shr;
  case ast::assign_op::AddWrapAssign:
    return opcode::op_add_wrap;
  case ast::assign_op::SubWrapAssign:
    return opcode::op_sub_wrap;
  case ast::assign_op::MulWrapAssign:
    return opcode::op_mul_wrap;
  case ast::assign_op::AddSatAssign:
    return opcode::op_add_sat;
  case ast::assign_op::SubSatAssign:
    return opcode::op_sub_sat;
  case ast::assign_op::MulSatAssign:
    return opcode::op_mul_sat;
  case ast::assign_op::Assign:
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
  function_compiler(const type_table &types,
                    const std::unordered_map<std::string, uint16_t> &functions)
      : types_(types), functions_(functions) {}

  [[nodiscard]] auto compile(const hir::hir_function &fn)
      -> std::expected<bytecode::bytecode_function, compile_error> {
    for (const auto &param : fn.params) {
      const auto reg = alloc_register(fn.span);
      if (!reg.has_value()) {
        return std::unexpected(reg.error());
      }
      locals_.emplace(param.symbol, *reg);
    }

    const auto &stmts = fn.body->stmts;
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
          .span = fn.span,
          .message = std::format("function `{}` needs more than 256 "
                                 "registers, which this bytecode format's "
                                 "u8 register operands cannot address",
                                 fn.name)});
    }
    return std::move(writer_).finish(fn.name,
                                     static_cast<uint16_t>(fn.params.size()),
                                     static_cast<uint16_t>(next_register_));
  }

private:
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
      auto kind = numeric_kind_for(expr.type, expr.span);
      if (!kind.has_value()) {
        return std::unexpected(kind.error());
      }
      auto value = encode_literal(lit, *kind);
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
    default:
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = expr.span,
          .message =
              "this expression form is outside the bytecode compiler's "
              "current scope — match, lambdas, and sum-type variants still "
              "need work spec/codegen-design.md's later increments haven't "
              "landed yet"});
    }
  }

  [[nodiscard]] auto compile_binary(const hir::hir_binary &bin, uint8_t dst)
      -> std::expected<void, compile_error> {
    if (bin.op == ast::binary_op::And || bin.op == ast::binary_op::Or) {
      // Short-circuit via jumps (opcodes.h's documented backend-only
      // choice) — evaluate lhs into dst, skip rhs (leaving dst as the
      // final result) exactly when short-circuiting applies.
      if (auto result = compile_expr_into(*bin.lhs, dst); !result.has_value()) {
        return std::unexpected(result.error());
      }
      writer_.emit_opcode(bin.op == ast::binary_op::And
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

  [[nodiscard]] auto compile_unary(const hir::hir_unary &un, uint8_t dst)
      -> std::expected<void, compile_error> {
    if (un.op == ast::unary_op::Not) {
      auto src = compile_expr(*un.operand);
      if (!src.has_value()) {
        return std::unexpected(src.error());
      }
      writer_.emit_opcode(opcode::op_not_bool);
      writer_.emit_u8(dst);
      writer_.emit_u8(*src);
      return {};
    }
    if (un.op != ast::unary_op::Neg && un.op != ast::unary_op::BitNot) {
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
    writer_.emit_opcode(un.op == ast::unary_op::Neg ? opcode::op_neg
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

  [[nodiscard]] auto compile_call(const hir::hir_call &call, uint8_t dst)
      -> std::expected<void, compile_error> {
    if (call.callee->kind != hir_node_kind::hir_local_ref) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = call.span,
          .message =
              "only direct calls to a named function are supported yet — "
              "calling through a computed callee expression is not"});
    }
    const auto &ref = dynamic_cast<const hir::hir_local_ref &>(*call.callee);
    if (lookup_local(ref.symbol).has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = call.span,
          .message =
              std::format("calling `{}`, a function value held in a local "
                          "variable, is not supported yet — only direct calls "
                          "to a named module function are",
                          ref.name)});
    }
    const auto found = functions_.find(ref.name);
    if (found == functions_.end()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unknown_callee,
          .span = call.span,
          .message =
              std::format("call to `{}` could not be resolved to a function in "
                          "this compiled module",
                          ref.name)});
    }
    if (call.args.size() > 255) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = call.span,
          .message =
              "more than 255 call arguments is not supported by this bytecode "
              "format's u8 argument-count operand"});
    }
    const auto argc = call.args.size();
    if (next_register_ + argc > 256) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::register_limit_exceeded,
          .span = call.span,
          .message = "this function needs more than 256 registers, which this "
                     "bytecode format's u8 register operands cannot address"});
    }
    // Reserve a contiguous register block up front (before compiling any
    // argument subexpression) so op_call's "argc consecutive registers"
    // precondition holds regardless of how many temporaries each argument
    // expression needs internally — those get allocated after this block
    // automatically, since next_register_ has already moved past it.
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

  [[nodiscard]] auto compile_array_init(const hir::hir_array_init &init,
                                        uint8_t dst)
      -> std::expected<void, compile_error> {
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

  /// Fixed `array[T, N]` element access only — `list`/`slice`/`str`
  /// indexing needs increment 5's growable-container support and is
  /// rejected here for now, same as every other not-yet-scoped construct.
  [[nodiscard]] auto compile_index(const hir::hir_index &node, uint8_t dst)
      -> std::expected<void, compile_error> {
    const auto &object_entry = types_.entry(node.object->type);
    if (object_entry.kind != semantic::type_kind::array_kind ||
        !object_entry.array_size.has_value()) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = node.span,
          .message = "indexing is only supported for a fixed-size array "
                     "with a statically known length yet — list/slice/str "
                     "indexing needs increment 5's growable-container "
                     "support"});
    }
    auto object_reg = compile_expr(*node.object);
    if (!object_reg.has_value()) {
      return std::unexpected(object_reg.error());
    }
    auto index_reg = compile_expr(*node.index);
    if (!index_reg.has_value()) {
      return std::unexpected(index_reg.error());
    }

    const auto len_const = writer_.add_constant(
        slot_value{static_cast<uint64_t>(*object_entry.array_size)});
    auto len_reg = alloc_register(node.span);
    if (!len_reg.has_value()) {
      return std::unexpected(len_reg.error());
    }
    writer_.emit_opcode(opcode::op_load_const);
    writer_.emit_u8(*len_reg);
    writer_.emit_u16(len_const);

    auto oob_reg = alloc_register(node.span);
    if (!oob_reg.has_value()) {
      return std::unexpected(oob_reg.error());
    }
    writer_.emit_opcode(opcode::op_ge);
    writer_.emit_u8(*oob_reg);
    writer_.emit_u8(*index_reg);
    writer_.emit_u8(*len_reg);
    writer_.emit_numeric_kind(numeric_kind::u64);
    writer_.emit_opcode(opcode::op_panic_if);
    writer_.emit_u8(*oob_reg);
    writer_.emit_u8(
        static_cast<uint8_t>(bytecode::panic_reason::index_out_of_bounds));

    writer_.emit_opcode(opcode::op_load_indexed);
    writer_.emit_u8(dst);
    writer_.emit_u8(*object_reg);
    writer_.emit_u8(*index_reg);
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
    default:
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = node.span,
          .message =
              "this statement form is outside the bytecode compiler's "
              "scalar/control-flow scope (spec/codegen-design.md increment 1) "
              "— match, while-let, destructuring let/else, and comprehension "
              "pushes all need a heap/aggregate representation increment 6 "
              "hasn't landed yet"});
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
    if (assign.target->kind != hir_node_kind::hir_local_ref) {
      return std::unexpected(compile_error{
          .kind = compile_error_kind::unsupported_construct,
          .span = assign.span,
          .message =
              "assigning to a field or index target needs a heap/aggregate "
              "representation this bytecode compiler doesn't have yet — only "
              "assigning directly to a local variable is supported"});
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
    if (assign.op == ast::assign_op::Assign) {
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

  const type_table &types_;
  const std::unordered_map<std::string, uint16_t> &functions_;
  chunk_writer writer_;
  std::unordered_map<hir::symbol_id, uint8_t> locals_;
  size_t next_register_ = 0;
};

} // namespace

auto compile_function(
    const hir::hir_function &fn, const type_table &types,
    const std::unordered_map<std::string, uint16_t> &function_index)
    -> std::expected<bytecode::bytecode_function, compile_error> {
  auto compiler = function_compiler(types, function_index);
  return compiler.compile(fn);
}

auto compile_module(const hir::hir_module &module, const type_table &types)
    -> std::expected<bytecode::bytecode_module, compile_error> {
  auto function_index = std::unordered_map<std::string, uint16_t>{};
  function_index.reserve(module.functions.size());
  for (size_t i = 0; i < module.functions.size(); ++i) {
    function_index.emplace(module.functions[i]->name, static_cast<uint16_t>(i));
  }

  auto result = bytecode::bytecode_module{.module_name = module.module_name,
                                          .functions = {}};
  result.functions.reserve(module.functions.size());
  for (const auto &fn : module.functions) {
    auto compiled = compile_function(*fn, types, function_index);
    if (!compiled.has_value()) {
      return std::unexpected(compiled.error());
    }
    result.functions.push_back(std::move(*compiled));
  }
  return result;
}

} // namespace kira::bytecode_compiler
