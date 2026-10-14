#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cinder::ast {
struct node;
struct type_decl;
} // namespace cinder::ast

namespace cinder::comptime {

/// Which shape a `value` currently holds.
enum class value_kind : uint8_t {
  unit,
  boolean,
  integer,
  floating,
  string,
  /// A compile-time list or tuple; both are represented the same way since
  /// neither has element-count-dependent typing at this layer.
  list,
  /// A struct value: `type_name` and `fields`.
  struct_instance,
  /// A sum-type variant value: the type's name in `type_name`, the
  /// variant's tag (`"some"`, `"x86_64"`) in `variant_tag`, and its payload
  /// values, in order, in `elements`.
  variant_instance,
  /// A quoted expression fragment ( Cinder type `expr`) — a boxed
  /// `const ast::expr *` into `fragment` (stored as `const ast::node *`
  /// since the four quote-value kinds share one storage field; the actual
  /// dynamic type always matches `ast::quote_expr::fragment_kind`).
  expr_fragment,
  /// A quoted statement fragment ( Cinder type `stmt`).
  stmt_fragment,
  /// A quoted item/definition fragment ( Cinder type `def_expr`).
  def_expr_fragment,
  /// A quoted type fragment ( Cinder type `type_expr`).
  type_expr_fragment,
  /// A synthesized pattern fragment ( Cinder type `pattern`) — a boxed
  /// `const ast::pattern *` into `fragment`, built only by the
  /// `expr.ctor_pattern(...)` AST-builder intrinsic (`fragments.cpp`) and
  /// consumed only by `expr.arm(...)`. Never produced by an ordinary
  /// backtick quote (there is no `` `(pattern-syntax)` `` quote form), so
  /// unlike the four kinds above it never needs `require_quote_value`
  /// splice-position checking.
  pattern_fragment,
  /// A bound generic type argument (e.g. `T` inside `static def
  /// derive_show[T]()` called as `derive_show[point]()`): the type's name,
  /// its declaration when it has one, and its `checked_type`.
  type_value,
  /// Sentinel meaning "evaluation already failed and a diagnostic was
  /// already reported" — mirrors `k_unknown_type`'s role in the type
  /// checker (`src/semantic/types.h`): once emitted, it propagates through
  /// the rest of the expression tree so one root cause doesn't cascade
  /// into a wall of follow-on diagnostics.
  diagnostic_marker,
};

/// A compile-time value: what compile-time code the VM ran returned, or a
/// binding the checker holds for it. Integers are held as a signed 64-bit
/// value regardless of their declared Cinder width; the checker knows the
/// type.
struct value {
  value_kind kind = value_kind::unit;
  bool boolean = false;
  int64_t integer = 0;
  double floating = 0.0;
  std::string string;

  /// Elements for `list`.
  std::vector<value> elements;

  /// Declared/inferred type name for `struct_instance` (empty if the
  /// literal had no explicit type head), and the type's name for
  /// `type_value`.
  std::string type_name;

  /// Field values for `struct_instance`.
  std::unordered_map<std::string, value> fields;

  /// Variant tag for `variant_instance` (e.g. `"unix"`, `"x86_64"`,
  /// `"other"`); `type_name` (above) holds the sum type's own name.
  std::string variant_tag;

  /// Bound type declaration for `type_value`; never owning — declarations
  /// live for the whole checking session.
  const ast::type_decl *type_decl = nullptr;

  /// For `type_value`, the checker's interned `type_id` for the bound type,
  /// or 0 (the checker's unknown type) when the type was named without the
  /// checker. A name and declaration alone cannot tell `pair[int32]` from
  /// `pair[str]`, so the checker reads this to resolve a method on a value
  /// typed with a type parameter.
  uint32_t checked_type = 0;

  /// Boxed AST fragment for `expr_fragment`/`stmt_fragment`/
  /// `def_expr_fragment`/`type_expr_fragment` — always a
  /// `ast::quote_expr::parsed_body` (or a sub-node reached by tearing one
  /// down), owned by the file's own AST tree, which outlives the whole
  /// checking session; never owning here.
  const ast::node *fragment = nullptr;

  [[nodiscard]] static auto make_unit() -> value {
    return value{.kind = value_kind::unit};
  }
  [[nodiscard]] static auto make_bool(bool v) -> value {
    return value{.kind = value_kind::boolean, .boolean = v};
  }
  [[nodiscard]] static auto make_int(int64_t v) -> value {
    return value{.kind = value_kind::integer, .integer = v};
  }
  [[nodiscard]] static auto make_float(double v) -> value {
    return value{.kind = value_kind::floating, .floating = v};
  }
  [[nodiscard]] static auto make_string(std::string v) -> value {
    return value{.kind = value_kind::string, .string = std::move(v)};
  }
  [[nodiscard]] static auto make_list(std::vector<value> elements) -> value {
    return value{.kind = value_kind::list, .elements = std::move(elements)};
  }
  [[nodiscard]] static auto
  make_struct(std::string type_name,
              std::unordered_map<std::string, value> fields) -> value {
    return value{.kind = value_kind::struct_instance,
                 .type_name = std::move(type_name),
                 .fields = std::move(fields)};
  }
  [[nodiscard]] static auto make_variant(std::string type_name,
                                         std::string variant_tag,
                                         std::vector<value> payload) -> value {
    return value{.kind = value_kind::variant_instance,
                 .elements = std::move(payload),
                 .type_name = std::move(type_name),
                 .variant_tag = std::move(variant_tag)};
  }
  [[nodiscard]] static auto make_expr_fragment(const ast::node *fragment)
      -> value {
    return value{.kind = value_kind::expr_fragment, .fragment = fragment};
  }
  [[nodiscard]] static auto make_stmt_fragment(const ast::node *fragment)
      -> value {
    return value{.kind = value_kind::stmt_fragment, .fragment = fragment};
  }
  [[nodiscard]] static auto make_def_expr_fragment(const ast::node *fragment)
      -> value {
    return value{.kind = value_kind::def_expr_fragment, .fragment = fragment};
  }
  [[nodiscard]] static auto make_type_expr_fragment(const ast::node *fragment)
      -> value {
    return value{.kind = value_kind::type_expr_fragment, .fragment = fragment};
  }
  [[nodiscard]] static auto make_pattern_fragment(const ast::node *fragment)
      -> value {
    return value{.kind = value_kind::pattern_fragment, .fragment = fragment};
  }
  [[nodiscard]] static auto make_type_value(std::string name,
                                            const ast::type_decl *decl,
                                            uint32_t checked_type = 0)
      -> value {
    return value{.kind = value_kind::type_value,
                 .type_name = std::move(name),
                 .type_decl = decl,
                 .checked_type = checked_type};
  }
  [[nodiscard]] static auto make_error() -> value {
    return value{.kind = value_kind::diagnostic_marker};
  }

  /// Whether `kind` is one of the four quote-value kinds.
  [[nodiscard]] auto is_quote_fragment() const noexcept -> bool {
    return kind == value_kind::expr_fragment ||
           kind == value_kind::stmt_fragment ||
           kind == value_kind::def_expr_fragment ||
           kind == value_kind::type_expr_fragment;
  }

  [[nodiscard]] auto is_error() const noexcept -> bool {
    return kind == value_kind::diagnostic_marker;
  }

  /// Truthiness for use as a `static assert`/`static if` condition; only a
  /// real `boolean` value is meaningful (checking already required
  /// `bool`-ness, so anything else here means an upstream error already
  /// happened).
  [[nodiscard]] auto is_true() const noexcept -> bool {
    return kind == value_kind::boolean && boolean;
  }
};

} // namespace cinder::comptime
