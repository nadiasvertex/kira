#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"

namespace kira::semantic {

// ==========================================================================
//  Type model
//
//  Types are interned into a `type_table` so equality is id equality.
//  `unknown` deliberately unifies with everything: it is produced wherever
//  the checker cannot know a type (unannotated parameters, external modules,
//  unresolved generics) and it silences downstream diagnostics so one gap in
//  knowledge never causes cascading errors.
// ==========================================================================

/// Interned index into a `type_table`; equality is id equality.
using type_id = uint32_t;

/// The always-present "not known" type; compatible with every other type.
inline constexpr type_id k_unknown_type = 0;
/// The always-present type produced after an error has already been
/// reported for an expression, so its type doesn't need to be guessed.
inline constexpr type_id k_error_type = 1;

/// Which shape of type a `type_entry` describes; determines which of its
/// fields are meaningful.
enum class type_kind : uint8_t {
  unknown_kind,         ///< Not known; compatible with every type.
  error_kind,           ///< Produced after an error was already reported.
  builtin_kind,         ///< Scalar builtin such as `int32`, `str`, `bool`.
  builtin_generic_kind, ///< Prelude container such as `list[T]`, `option[T]`.
  tuple_kind,           ///< `(A, B, C)`.
  array_kind,           ///< `array[T, n]`.
  fn_kind,              ///< `fn(A, B) -> R`.
  ref_kind,             ///< `&T` / `&mut T`.
  ptr_kind,             ///< `*T` / `*mut T`.
  struct_kind,          ///< User struct type, possibly instantiated.
  sum_kind,             ///< User sum type, possibly instantiated.
  opaque_kind,          ///< User alias/opaque type declaration.
  type_param_kind,      ///< In-scope generic parameter such as `T`.
  type_var_kind,        ///< Fresh inference variable; see `fresh_type_var`.
  const_value_kind,     ///< A literal compile-time value used as a const
                        ///< generic argument, e.g. the `3` in `vec[T, 3]`.
};

/// The interned data behind one `type_id`. Which fields are meaningful
/// depends on `kind`: e.g. `args` holds tuple elements for `tuple_kind` but
/// generic arguments for `builtin_generic_kind`/`struct_kind`/`sum_kind`, and
/// `result` holds the referenced/pointed-to/array-element type for
/// `ref_kind`/`ptr_kind`/`array_kind` but the return type for `fn_kind`.
struct type_entry {
  type_kind kind = type_kind::unknown_kind;
  std::string name;                     ///< Builtin/user/parameter name.
  std::string module_name;              ///< Owning module for user types.
  const ast::type_decl *decl = nullptr; ///< Declaration for user types.
  std::vector<type_id> args;            ///< Element/parameter/generic args.
  type_id result = k_unknown_type;      ///< fn result or ref/ptr/array inner.
  bool is_mut = false;                  ///< Mutability for ref/ptr types.
  std::optional<uint64_t> array_size;   ///< Array length when statically known.
};

/// Owns every `type_entry` produced while checking one session, interning
/// structurally-identical types (same kind, name/decl, and arguments) to the
/// same `type_id` so type equality is just id equality.
class type_table {
public:
  /// Seeds the table with `k_unknown_type` and `k_error_type` at their fixed
  /// ids, plus `bool` (see `bool_type`).
  type_table();

  /// Interns a scalar builtin type such as `int32` or `str`.
  [[nodiscard]] auto builtin(std::string_view name) -> type_id;
  /// Interns a prelude container instantiation such as `list[T]`.
  [[nodiscard]] auto builtin_generic(std::string_view name,
                                     std::vector<type_id> args) -> type_id;
  /// Interns a tuple type `(A, B, C)` from its element types.
  [[nodiscard]] auto tuple_of(std::vector<type_id> elements) -> type_id;
  /// Interns `array[T, n]`; `size` is `nullopt` when the length is not
  /// statically known (e.g. a non-literal size expression).
  [[nodiscard]] auto array_of(type_id element, std::optional<uint64_t> size)
      -> type_id;
  /// Interns a function type `fn(params...) -> result`.
  [[nodiscard]] auto fn_of(std::vector<type_id> params, type_id result)
      -> type_id;
  /// Interns a reference type `&T` (or `&mut T` when `is_mut`).
  [[nodiscard]] auto ref_to(type_id inner, bool is_mut) -> type_id;
  /// Interns a raw pointer type `*T` (or `*mut T` when `is_mut`).
  [[nodiscard]] auto ptr_to(type_id inner, bool is_mut) -> type_id;
  /// Interns an instantiation of a user `type` declaration (struct, sum, or
  /// alias/opaque) with the given generic `args`.
  [[nodiscard]] auto user_type(const ast::type_decl &decl,
                               std::string_view module_name,
                               std::vector<type_id> args) -> type_id;
  /// Interns an in-scope generic type/value parameter, identified by name.
  [[nodiscard]] auto type_param(std::string_view name) -> type_id;
  /// Interns a literal compile-time value used as a const generic argument
  /// (e.g. the `3` in `vec[T, 3]`), keyed on `underlying` (the value's
  /// scalar type, e.g. `usize`) and `value` so `vec[T, 3]` and `vec[T, 5]`
  /// are distinct types while two instantiations with the same literal
  /// collapse to one id.
  [[nodiscard]] auto const_value(type_id underlying, uint64_t value) -> type_id;
  /// Mints a fresh, globally-unique inference variable for local parameter
  /// inference (see `src/semantic/check.cpp`'s unification engine). Unlike
  /// every other `type_table` constructor, this never structurally interns —
  /// each call returns a distinct id, since two variables must stay
  /// independently solvable even if they happen to arise identically. A
  /// variable left unsolved after inference behaves exactly like
  /// `k_unknown_type` everywhere (`is_unknown`, `compatible`, `display`), so
  /// a var that escapes unresolved is harmless rather than a false positive.
  [[nodiscard]] auto fresh_type_var() -> type_id;

  /// Looks up the data behind `id`; returns the `unknown` entry for an
  /// out-of-range id.
  [[nodiscard]] auto entry(type_id id) const -> const type_entry &;
  /// Renders `id` as the user-facing type spelling used in diagnostics
  /// (e.g. `list[int32]`, `fn(int32) -> str`).
  [[nodiscard]] auto display(type_id id) const -> std::string;

  /// The interned id for the builtin `bool` type. Unlike every other
  /// builtin scalar (interned lazily on first use via `builtin`), `bool`
  /// is pre-interned unconditionally by the constructor, so this is a
  /// `const` lookup — no interning, no mutation. Exists for callers that
  /// hold a `const type_table&` and need a definite `bool` type_id for a
  /// type the checked program may never have happened to reference itself
  /// (lowering synthesizing a comparison for a desugared `for` loop, e.g.).
  [[nodiscard]] auto bool_type() const -> type_id;
  /// The interned id for the builtin `usize` type, pre-interned for the
  /// same reason and by the same mechanism as `bool_type` — used for a
  /// desugared indexed-container `for` loop's index/length values.
  [[nodiscard]] auto usize_type() const -> type_id;
  /// The interned id for the builtin `char` type, pre-interned for the
  /// same reason and by the same mechanism as `bool_type` — used as the
  /// element type of a desugared `for` loop over a `str`.
  [[nodiscard]] auto char_type() const -> type_id;

  /// Whether `id` is `unknown`, `error`, or a type parameter — anything the
  /// checker treats as "don't know, don't complain."
  [[nodiscard]] auto is_unknown(type_id id) const -> bool;
  /// Whether `id` is the builtin `bool`.
  [[nodiscard]] auto is_boolean(type_id id) const -> bool;
  /// Whether `id` is a builtin signed/unsigned integer type (including
  /// `byte`, `isize`, `usize`).
  [[nodiscard]] auto is_integer(type_id id) const -> bool;
  /// Whether `id` is a builtin `float32`/`float64`/`float128`.
  [[nodiscard]] auto is_float(type_id id) const -> bool;
  /// Whether `id` is an integer or float builtin.
  [[nodiscard]] auto is_numeric(type_id id) const -> bool;
  /// Whether `id` is the builtin `unit`.
  [[nodiscard]] auto is_unit(type_id id) const -> bool;

  /// Whether a value of `found` is acceptable where `expected` is required.
  /// Unknown and error types are compatible with everything by design.
  [[nodiscard]] auto compatible(type_id expected, type_id found) const -> bool;

private:
  /// Returns the existing id for `key` if already interned, otherwise
  /// stores `entry` and returns its new id.
  [[nodiscard]] auto intern(std::string key, type_entry entry) -> type_id;

  std::vector<type_entry> entries_;
  std::unordered_map<std::string, type_id> interned_;
};

/// The persisted result of type-checking one session: the interned
/// `type_table` every `type_id` below indexes into, plus the resolved type
/// of every expression node the checker actually visited. Several
/// non-expression declaration nodes are recorded in `node_types` too, since
/// a later pass needs their types but they never pass through `infer_expr`:
/// a function parameter's `pattern` node (keyed by `param.pattern.get()`),
/// a function's declared return-type node (keyed by
/// `decl.return_type.get()`), and every `ast::pattern` node checked via
/// `check_pattern` (keyed by the pattern node itself, recording the type of
/// the value it matches) — all recorded in `check_function`/`check_pattern`.
/// Two shorthand shapes have no node to key against, for the same reason:
/// a struct pattern's shorthand field (`{x}`, matching
/// `ast::field_pattern.pattern == nullptr`) binds a name straight from a
/// plain struct field with no dedicated sub-pattern node, and a struct
/// *literal*'s shorthand field (`{x}`, matching
/// `ast::struct_field_init.value == nullptr`) reads an in-scope value the
/// same way with no dedicated value node — so each is recorded separately,
/// in `struct_pattern_field_types` (keyed by the owning `ast::field_pattern`)
/// and `struct_literal_field_types` (keyed by the owning
/// `ast::struct_field_init`) respectively; neither key type is itself an
/// `ast::node`, but both are stable for the AST's lifetime. A node the
/// checker never reached (inside a file already marked failing, or simply
/// never checked) has no entry in any of these maps — look it up with
/// `.find`, not `.at`. This is what a later typed-lowering pass
/// (`spec/typed-ir-design.md`) reads instead of re-deriving types from the
/// AST a second time.
/// Per-call-site argument-to-parameter mapping for a call resolved against
/// a real `ast::func_decl` (a free function, a method, or a trait default
/// body) via `check_call_against_decl` — the only call form that supports
/// named arguments and defaults, since a bare `fn(...)`-typed callee has no
/// parameter names to resolve a named argument against.
/// `args_by_param[i]` is the AST expression supplying the i-th declared
/// parameter (in the same order `signature_params` produces, i.e. with
/// `self` already skipped for a method call) — whichever call argument
/// that turned out to be, positional or named; null means the argument was
/// omitted and the parameter's default value applies.
struct call_argument_mapping {
  std::vector<const ast::expr *> args_by_param;
};

/// The persisted result of type-checking one session: the interned
/// `type_table` every `type_id` below indexes into, plus the resolved type
/// of every expression node the checker actually visited. Several
/// non-expression declaration nodes are recorded in `node_types` too, since
/// a later pass needs their types but they never pass through `infer_expr`:
/// a function parameter's `pattern` node (keyed by `param.pattern.get()`),
/// a function's declared return-type node (keyed by
/// `decl.return_type.get()`), and every `ast::pattern` node checked via
/// `check_pattern` (keyed by the pattern node itself, recording the type of
/// the value it matches) — all recorded in `check_function`/`check_pattern`.
/// Two shorthand shapes have no node to key against, for the same reason:
/// a struct pattern's shorthand field (`{x}`, matching
/// `ast::field_pattern.pattern == nullptr`) binds a name straight from a
/// plain struct field with no dedicated sub-pattern node, and a struct
/// *literal*'s shorthand field (`{x}`, matching
/// `ast::struct_field_init.value == nullptr`) reads an in-scope value the
/// same way with no dedicated value node — so each is recorded separately,
/// in `struct_pattern_field_types` (keyed by the owning `ast::field_pattern`)
/// and `struct_literal_field_types` (keyed by the owning
/// `ast::struct_field_init`) respectively; neither key type is itself an
/// `ast::node`, but both are stable for the AST's lifetime. A node the
/// checker never reached (inside a file already marked failing, or simply
/// never checked) has no entry in any of these maps — look it up with
/// `.find`, not `.at`. This is what a later typed-lowering pass
/// (`spec/typed-ir-design.md`) reads instead of re-deriving types from the
/// AST a second time.
struct checked_types {
  type_table types;
  std::unordered_map<const ast::node *, type_id> node_types;
  std::unordered_map<const ast::field_pattern *, type_id>
      struct_pattern_field_types;
  std::unordered_map<const ast::struct_field_init *, type_id>
      struct_literal_field_types;
  std::unordered_map<const ast::call_expr *, call_argument_mapping>
      call_argument_mappings;
};

/// Whether `name` is a builtin scalar type (`int32`, `str`, `bool`, ...).
[[nodiscard]] auto is_builtin_scalar_name(std::string_view name) -> bool;

/// Expected generic-argument arity for a prelude container name, as an
/// inclusive [min, max] pair; `nullopt` when the name is not a prelude
/// container.
[[nodiscard]] auto builtin_generic_arity(std::string_view name)
    -> std::optional<std::pair<size_t, size_t>>;

/// Inclusive unsigned upper bound of a builtin integer type when it fits in
/// 64 bits; `nullopt` for non-integers and for the 128-bit types.
[[nodiscard]] auto integer_max_value(std::string_view name)
    -> std::optional<uint64_t>;

// ==========================================================================
//  Program index — session-wide declaration lookup tables.
// ==========================================================================

/// A `type` declaration plus the file it was declared in.
struct type_decl_ref {
  const ast::type_decl *decl = nullptr;
  file_id_type file_id = 0;
};

/// A `trait` declaration plus the file it was declared in.
struct trait_decl_ref {
  const ast::trait_decl *decl = nullptr;
  file_id_type file_id = 0;
};

/// A `concept` declaration plus the file it was declared in.
struct concept_decl_ref {
  const ast::concept_decl *decl = nullptr;
  file_id_type file_id = 0;
};

/// A `def` declaration plus the file it was declared in.
struct func_decl_ref {
  const ast::func_decl *decl = nullptr;
  file_id_type file_id = 0;
};

/// A `static` binding declaration plus the file it was declared in.
struct static_decl_ref {
  const ast::static_decl *decl = nullptr;
  file_id_type file_id = 0;
};

/// One sum-type variant, plus the `type` declaration it belongs to — lets a
/// bare variant name (`@some`) be looked up without first knowing its sum
/// type.
struct variant_ref {
  const ast::type_decl *sum_decl = nullptr;
  const ast::sum_variant *variant = nullptr;
};

/// One `impl` block, plus the module and file it was declared in.
struct impl_ref {
  const ast::impl_decl *decl = nullptr;
  std::string module_name;
  file_id_type file_id = 0;
};

/// One `extend` block, plus the module and file it was declared in.
struct extend_ref {
  const ast::extend_decl *decl = nullptr;
  std::string module_name;
  file_id_type file_id = 0;
};

/// One name a `use` declaration binds into a file's local scope — either the
/// final path segment (`use a.b.c` binds `c`), a selected/renamed item
/// (`use a.b.{c as d}`), or a wildcard marker (`use a.b.*`).
struct import_binding {
  std::string local_name;        ///< Name the import introduces locally.
  std::vector<std::string> path; ///< Source module path of the import.
  std::string leaf_name;         ///< Imported member name; empty for modules.
  bool is_wildcard = false;      ///< Whether this is a `use a.b.*` wildcard.
  source_span span;              ///< Location of the imported item/selector.
};

/// Every declaration directly owned by one module, aggregated across every
/// file that contributes to it (a module may span several files).
struct module_members {
  std::string module_name;
  std::unordered_map<std::string, type_decl_ref> types;
  std::unordered_map<std::string, trait_decl_ref> traits;
  std::unordered_map<std::string, concept_decl_ref> concepts;
  std::unordered_map<std::string, func_decl_ref> functions;
  std::unordered_map<std::string, static_decl_ref> statics;
  std::unordered_map<std::string, variant_ref> variants;
  std::vector<impl_ref> impls;
  std::vector<extend_ref> extends;
};

/// Session-wide index of every module's declarations and every file's
/// imports, used by the checker to resolve names and types without
/// re-walking the AST for each lookup.
struct program_index {
  std::unordered_map<std::string, module_members> modules;
  std::unordered_map<file_id_type, std::vector<import_binding>> imports;

  /// Looks up a module by its fully-qualified dotted name, or `nullptr` if
  /// no file in the session declares it.
  [[nodiscard]] auto find_module(std::string_view module_name) const
      -> const module_members *;
};

/// Builds a `program_index` by walking every input file's items, recording
/// each module's direct declarations and each file's `use` bindings
/// (recursing into inline submodules under their qualified name).
[[nodiscard]] auto build_program_index(const std::vector<parsed_module> &inputs)
    -> program_index;

} // namespace kira::semantic
