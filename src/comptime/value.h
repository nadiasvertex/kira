#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace kira::ast {
struct func_decl;
struct node;
struct type_decl;
} // namespace kira::ast

namespace kira::comptime {

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
  /// A compile-time struct instance built from a `struct_expr` literal.
  struct_instance,
  /// A reference to a `static def` function, callable from other
  /// compile-time expressions.
  closure,
  /// A quoted expression fragment (Kira type `expr`) — a boxed
  /// `const ast::expr *` into `fragment` (stored as `const ast::node *`
  /// since the four quote-value kinds share one storage field; the actual
  /// dynamic type always matches `ast::quote_expr::fragment_kind`).
  expr_fragment,
  /// A quoted statement fragment (Kira type `stmt`).
  stmt_fragment,
  /// A quoted item/definition fragment (Kira type `def_expr`).
  def_expr_fragment,
  /// A quoted type fragment (Kira type `type_expr`).
  type_expr_fragment,
  /// A bound generic type argument (e.g. `T` inside `static def
  /// derive_show[T]()`, bound by a compile-time generic call like
  /// `derive_show[point]()`) — a boxed `const ast::type_decl *`, resolved
  /// the same way `reflect.cpp`'s `pending_types_`-by-literal-name lookup
  /// already resolves a type reflection target, just reached through a
  /// local binding instead of a literal identifier spelling.
  type_value,
  /// Sentinel meaning "evaluation already failed and a diagnostic was
  /// already reported" — mirrors `k_unknown_type`'s role in the type
  /// checker (`src/semantic/types.h`): once emitted, it propagates through
  /// the rest of the expression tree so one root cause doesn't cascade
  /// into a wall of follow-on diagnostics.
  diagnostic_marker,
};

/// A compile-time value produced by `evaluator`. Integers are held as a
/// signed 64-bit value regardless of the expression's declared Kira width
/// — by the time the evaluator runs, `checker::infer_expr` has already
/// validated the expression against its declared type, so the evaluator
/// itself doesn't need to re-derive or enforce width/signedness; it only
/// needs a value.
struct value {
  value_kind kind = value_kind::unit;
  bool boolean = false;
  int64_t integer = 0;
  double floating = 0.0;
  std::string string;

  /// Elements for `list`.
  std::vector<value> elements;

  /// Declared/inferred type name for `struct_instance` (empty if the
  /// literal had no explicit type head) and, for `closure`, the function's
  /// name (used in diagnostics).
  std::string type_name;

  /// Field values for `struct_instance`.
  std::unordered_map<std::string, value> fields;

  /// Target function for `closure`; never owning — function declarations
  /// live for the whole checking session.
  const ast::func_decl *function = nullptr;

  /// Bound type declaration for `type_value`; never owning, same lifetime
  /// rationale as `function`.
  const ast::type_decl *type_decl = nullptr;

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
  [[nodiscard]] static auto make_closure(std::string name,
                                         const ast::func_decl *function)
      -> value {
    return value{.kind = value_kind::closure,
                 .type_name = std::move(name),
                 .function = function};
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
  [[nodiscard]] static auto make_type_value(std::string name,
                                            const ast::type_decl *decl)
      -> value {
    return value{.kind = value_kind::type_value,
                 .type_name = std::move(name),
                 .type_decl = decl};
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

} // namespace kira::comptime
