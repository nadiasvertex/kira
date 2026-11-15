// String interpolation (spec/unified-checker-plan.md, phase 2).
//
// Each `{value:spec}` segment is rendered by the `std.fmt` helper its
// `interp_dispatches` entry selects, with a `format_spec` built from the
// written spec, and the pieces are joined left to right with the
// `rt_str_concat` intrinsic. This mirrors `lower_interpolated_string`.

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <variant>

#include "src/core/adapter_builder.h"
#include "src/parser/text_escape.h"

namespace cinder::core {

using semantic::type_kind;

namespace {

/// A `char_lit` spelling for one ASCII byte (lowering's
/// `quote_char_for_literal`).
auto quote_char(char c) -> std::string {
  switch (c) {
  case '\'':
    return "'\\''";
  case '\\':
    return "'\\\\'";
  case '\n':
    return "'\\n'";
  case '\t':
    return "'\\t'";
  case '\r':
    return "'\\r'";
  case '\0':
    return "'\\0'";
  default:
    return std::string("'") + c + "'";
  }
}

} // namespace

auto body_builder::fmt_call(std::string_view name, ptr_vec<expr> args,
                            source_span span) -> result<ptr<expr>> {
  const auto found = fmt_functions_ == nullptr
                         ? nullptr
                         : [&]() -> const ast::func_decl * {
                             const auto it =
                                 fmt_functions_->find(std::string(name));
                             return it == fmt_functions_->end() ? nullptr
                                                                : it->second;
                           }();
  if (found == nullptr) {
    return fail("missing record: fmt_types",
                std::format("`std.fmt` has no `{}`", name), span);
  }
  CORE_TRY(target, callee_of(semantic::resolved_callee{.decl = found,
                                                       .owner_module = "std.fmt",
                                                       .impl_target_type = {},
                                                       .receiver = nullptr,
                                                       .trait_name = {}},
                             span));
  const auto params = checked_.types.entry(target.signature).args;
  if (params.size() != args.size()) {
    return fail("interpolated_string_expr",
                std::format("`std.fmt.{}` takes {} arguments", name,
                            params.size()),
                span);
  }
  for (std::size_t i = 0; i < args.size(); ++i) {
    CORE_TRY(fitted, coerce(std::move(args[i]), params[i]));
    args[i] = std::move(fitted);
  }
  return direct_call(std::move(target), std::move(args), span);
}

auto body_builder::format_spec_of(const ast::interp_segment &seg,
                                  source_span span) -> result<ptr<expr>> {
  const auto &fmt = checked_.lowering.fmt_types;
  const auto usize = checked_.types.usize_type();
  const auto variant = [&](type_id type, std::string_view name,
                           ptr<expr> payload) -> result<ptr<expr>> {
    CORE_TRY(index, variant_index(type, name, span));
    auto made = make_node<variant_init>(span, type);
    made->variant = std::string(name);
    made->variant_index = index;
    if (payload != nullptr) {
      made->payload.push_back(std::move(payload));
    }
    return ptr<expr>(std::move(made));
  };
  // A width or precision: absent, a literal, or a `usize` expression.
  const auto slot =
      [&](const std::variant<std::monostate, size_t, ast::ptr<ast::expr>>
              &written) -> result<ptr<expr>> {
    if (const auto *literal = std::get_if<size_t>(&written)) {
      return variant(fmt.option_usize, "some",
                     int_literal(*literal, usize, span));
    }
    if (const auto *dynamic = std::get_if<ast::ptr<ast::expr>>(&written);
        dynamic != nullptr && *dynamic != nullptr) {
      CORE_TRY(value, value_as(**dynamic, usize));
      return variant(fmt.option_usize, "some", std::move(value));
    }
    return variant(fmt.option_usize, "none", nullptr);
  };
  const auto has = seg.has_spec;
  auto values = ptr_vec<expr>{};
  auto names = std::vector<std::string>{};
  const auto add = [&](std::string name, ptr<expr> value) {
    names.push_back(std::move(name));
    values.push_back(std::move(value));
  };
  auto fill = make_node<literal>(span, checked_.types.char_type());
  fill->token = token_kind::char_lit;
  fill->spelling = quote_char(has ? seg.spec.fill : ' ');
  add("fill", std::move(fill));
  if (has && seg.spec.align.has_value()) {
    const auto *name = *seg.spec.align == ast::format_align::left    ? "left"
                       : *seg.spec.align == ast::format_align::right ? "right"
                                                                      : "center";
    CORE_TRY(mode, variant(fmt.align_mode, name, nullptr));
    CORE_TRY(align, variant(fmt.option_align_mode, "some", std::move(mode)));
    add("align", std::move(align));
  } else {
    CORE_TRY(align, variant(fmt.option_align_mode, "none", nullptr));
    add("align", std::move(align));
  }
  const auto *sign = !has ? "negative_only"
                     : seg.spec.sign == ast::format_sign::always ? "always"
                     : seg.spec.sign == ast::format_sign::space  ? "space"
                                                                 : "negative_only";
  CORE_TRY(sign_value, variant(fmt.sign_mode, sign, nullptr));
  add("sign", std::move(sign_value));
  add("alternate", bool_literal(has && seg.spec.alternate, span));
  add("zero_pad", bool_literal(has && seg.spec.zero_pad, span));
  const auto none = std::variant<std::monostate, size_t, ast::ptr<ast::expr>>{};
  CORE_TRY(width, slot(has ? seg.spec.width : none));
  add("width", std::move(width));
  CORE_TRY(precision, slot(has ? seg.spec.precision : none));
  add("precision", std::move(precision));

  const auto *shape = find_shape(checked_, fmt.format_spec);
  if (shape == nullptr) {
    return fail("missing record: shapes", "no shape for `format_spec`", span);
  }
  const auto &fields = shape->fields;
  auto made = make_node<aggregate_init>(node_kind::struct_init, span,
                                        fmt.format_spec);
  for (std::size_t i = 0; i < names.size(); ++i) {
    const auto field =
        std::ranges::find(fields, names[i], &semantic::field_shape::name);
    if (field == fields.end()) {
      return fail("missing record: shapes",
                  std::format("`format_spec` has no field `{}`", names[i]),
                  span);
    }
    CORE_TRY(value, coerce(std::move(values[i]), field->type));
    made->field_names.push_back(names[i]);
    made->field_indices.push_back(
        static_cast<std::size_t>(field - fields.begin()));
    made->elements.push_back(std::move(value));
  }
  if (made->elements.size() != fields.size()) {
    return fail("interpolated_string_expr",
                "`format_spec` has fields the interpolation does not set",
                span);
  }
  return ptr<expr>(std::move(made));
}

auto body_builder::interpolation_of(const ast::interpolated_string_expr &node)
    -> result<ptr<expr>> {
  const auto &fmt = checked_.lowering.fmt_types;
  const auto span = node.span;
  if (fmt.format_spec == 0) {
    return fail("missing record: fmt_types", "`std.fmt` is not compiled",
                span);
  }
  const auto str = fmt.str_type;
  const auto text = [&](std::string_view written) -> ptr<expr> {
    auto lit = make_node<literal>(span, str);
    lit->token = token_kind::string_lit;
    lit->spelling = encode_string_literal(written);
    return lit;
  };
  const auto as_type = [&](ptr<expr> value, type_id target) -> ptr<expr> {
    if (value->type == target) {
      return value;
    }
    auto made = make_node<cast>(span, target);
    made->operand = std::move(value);
    return made;
  };
  auto pieces = ptr_vec<expr>{};
  for (const auto &seg : node.segments) {
    if (seg.is_literal) {
      pieces.push_back(text(seg.literal_text));
      continue;
    }
    if (seg.value == nullptr) {
      return fail("interpolated_string_expr", "a segment has no expression",
                  span);
    }
    if (seg.self_doc) {
      pieces.push_back(text(seg.source_text + "="));
    }
    const auto found = checked_.lowering.interp_dispatches.find(seg.value.get());
    if (found == checked_.lowering.interp_dispatches.end()) {
      return fail("missing record: interp_dispatches",
                  "an interpolated expression", seg.value->span);
    }
    const auto &dispatch = found->second;
    using kind = semantic::interp_dispatch::kind_t;
    CORE_TRY(spec, format_spec_of(seg, seg.value->span));
    CORE_TRY(value_type, settle(dispatch.value_type, seg.value->span));
    const auto &entry = checked_.types.entry(value_type);
    const auto is_signed =
        entry.name.starts_with("int") || entry.name == "isize";
    const auto is_wide = entry.kind == type_kind::builtin_kind &&
                         (entry.name == "int128" || entry.name == "uint128");
    auto args = ptr_vec<expr>{};
    auto helper = std::string{};
    if (dispatch.kind == kind::trait_method) {
      if (dispatch.decl == nullptr) {
        return fail("interpolated_string_expr",
                    "a capability that comes only from `deriving`",
                    seg.value->span);
      }
      CORE_TRY(rendered,
               dispatch_call(semantic::resolved_callee{
                                 .decl = dispatch.decl,
                                 .owner_module = dispatch.owner_module,
                                 .impl_target_type = dispatch.impl_target_type,
                                 .receiver = seg.value.get(),
                                 .trait_name = {}},
                             *seg.value, {}, seg.value->span));
      const auto radix = dispatch.type_char == 'x' ||
                         dispatch.type_char == 'X' ||
                         dispatch.type_char == 'o' || dispatch.type_char == 'b';
      if (radix) {
        args.push_back(bool_literal(false, span));
        args.push_back(text(dispatch.type_char == 'o'   ? "0o"
                            : dispatch.type_char == 'b' ? "0b"
                                                        : "0x"));
        helper = "pad_integral";
      } else {
        helper = "pad_str";
      }
      args.push_back(std::move(rendered));
      args.push_back(std::move(spec));
    } else {
      CORE_TRY(value, value_as(*seg.value, value_type));
      const auto precision = [&](std::uint64_t fallback) -> result<ptr<expr>> {
        const auto usize = checked_.types.usize_type();
        if (seg.has_spec) {
          if (const auto *literal = std::get_if<size_t>(&seg.spec.precision)) {
            return int_literal(*literal, usize, span);
          }
          if (const auto *dynamic =
                  std::get_if<ast::ptr<ast::expr>>(&seg.spec.precision);
              dynamic != nullptr && *dynamic != nullptr) {
            return value_as(**dynamic, usize);
          }
        }
        return int_literal(fallback, usize, span);
      };
      switch (dispatch.kind) {
      case kind::builtin_show:
      case kind::builtin_debug: {
        const auto debug = dispatch.kind == kind::builtin_debug;
        if (entry.kind == type_kind::builtin_kind && entry.name == "str") {
          helper = debug ? "fmt_debug_str" : "fmt_show_str";
          args.push_back(std::move(value));
        } else if (checked_.types.is_boolean(value_type)) {
          helper = "fmt_show_bool";
          args.push_back(std::move(value));
        } else if (entry.kind == type_kind::builtin_kind &&
                   entry.name == "char") {
          helper = "fmt_show_char";
          args.push_back(std::move(value));
        } else if (is_wide) {
          helper = is_signed ? "fmt_show_i128" : "fmt_show_u128";
          args.push_back(std::move(value));
        } else if (checked_.types.is_integer(value_type)) {
          helper = is_signed ? "fmt_show_i64" : "fmt_show_u64";
          args.push_back(as_type(std::move(value),
                                 is_signed ? fmt.int64_type : fmt.uint64_type));
        } else if (checked_.types.is_float(value_type)) {
          helper = "fmt_float_general";
          args.push_back(as_type(std::move(value), fmt.float64_type));
          args.push_back(int_literal(6, checked_.types.usize_type(), span));
        } else {
          return fail("interpolated_string_expr",
                      std::format("no builtin rendering of `{}`",
                                  checked_.types.display(value_type)),
                      seg.value->span);
        }
        break;
      }
      case kind::builtin_radix: {
        const auto c = dispatch.type_char;
        const auto radix = c == 'o' ? 8 : c == 'b' ? 2
                           : c == 'x' || c == 'X'  ? 16
                                                   : 10;
        helper = is_wide ? (is_signed ? "fmt_radix_i128" : "fmt_radix_u128")
                         : (is_signed ? "fmt_radix_i64" : "fmt_radix_u64");
        args.push_back(is_wide ? std::move(value)
                               : as_type(std::move(value),
                                         is_signed ? fmt.int64_type
                                                   : fmt.uint64_type));
        args.push_back(
            int_literal(static_cast<std::uint64_t>(radix), fmt.uint8_type, span));
        args.push_back(bool_literal(c == 'X', span));
        args.push_back(text(c == 'o'                ? "0o"
                            : c == 'b'              ? "0b"
                            : c == 'x' || c == 'X' ? "0x"
                                                    : ""));
        break;
      }
      case kind::builtin_float: {
        const auto c = dispatch.type_char;
        args.push_back(as_type(std::move(value), fmt.float64_type));
        CORE_TRY(digits, precision(6));
        args.push_back(std::move(digits));
        if (c == 'e' || c == 'E') {
          helper = "fmt_float_sci";
          args.push_back(bool_literal(c == 'E', span));
        } else {
          helper = c == 'g' || c == 'G' ? "fmt_float_general"
                                        : "fmt_float_fixed";
        }
        break;
      }
      case kind::builtin_char:
        helper = "fmt_char_codepoint";
        args.push_back(as_type(std::move(value), fmt.uint32_type));
        break;
      case kind::trait_method:
        break;
      }
      args.push_back(std::move(spec));
    }
    CORE_TRY(rendered, fmt_call(helper, std::move(args), seg.value->span));
    pieces.push_back(std::move(rendered));
  }
  if (pieces.empty()) {
    return text("");
  }
  auto folded = std::move(pieces.front());
  for (std::size_t i = 1; i < pieces.size(); ++i) {
    auto args = ptr_vec<expr>{};
    args.push_back(std::move(folded));
    args.push_back(std::move(pieces[i]));
    folded = intrinsic_call("rt_str_concat", str, std::move(args), {}, span);
  }
  return folded;
}

} // namespace cinder::core
