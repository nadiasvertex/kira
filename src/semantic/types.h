#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/k-parser/ast.h"
#include "src/k-parser/source_location.h"
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

using type_id = uint32_t;

inline constexpr type_id k_unknown_type = 0;
inline constexpr type_id k_error_type = 1;

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
};

struct type_entry {
  type_kind kind = type_kind::unknown_kind;
  std::string name;                      ///< Builtin/user/parameter name.
  std::string module_name;               ///< Owning module for user types.
  const ast::type_decl *decl = nullptr;  ///< Declaration for user types.
  std::vector<type_id> args;             ///< Element/parameter/generic args.
  type_id result = k_unknown_type;       ///< fn result or ref/ptr/array inner.
  bool is_mut = false;                   ///< Mutability for ref/ptr types.
  std::optional<uint64_t> array_size;    ///< Array length when statically known.
};

class type_table {
public:
  type_table();

  [[nodiscard]] auto builtin(std::string_view name) -> type_id;
  [[nodiscard]] auto builtin_generic(std::string_view name,
                                     std::vector<type_id> args) -> type_id;
  [[nodiscard]] auto tuple_of(std::vector<type_id> elements) -> type_id;
  [[nodiscard]] auto array_of(type_id element,
                              std::optional<uint64_t> size) -> type_id;
  [[nodiscard]] auto fn_of(std::vector<type_id> params, type_id result)
      -> type_id;
  [[nodiscard]] auto ref_to(type_id inner, bool is_mut) -> type_id;
  [[nodiscard]] auto ptr_to(type_id inner, bool is_mut) -> type_id;
  [[nodiscard]] auto user_type(const ast::type_decl &decl,
                               std::string_view module_name,
                               std::vector<type_id> args) -> type_id;
  [[nodiscard]] auto type_param(std::string_view name) -> type_id;

  [[nodiscard]] auto entry(type_id id) const -> const type_entry &;
  [[nodiscard]] auto display(type_id id) const -> std::string;

  [[nodiscard]] auto is_unknown(type_id id) const -> bool;
  [[nodiscard]] auto is_boolean(type_id id) const -> bool;
  [[nodiscard]] auto is_integer(type_id id) const -> bool;
  [[nodiscard]] auto is_float(type_id id) const -> bool;
  [[nodiscard]] auto is_numeric(type_id id) const -> bool;
  [[nodiscard]] auto is_unit(type_id id) const -> bool;

  /// Whether a value of `found` is acceptable where `expected` is required.
  /// Unknown and error types are compatible with everything by design.
  [[nodiscard]] auto compatible(type_id expected, type_id found) const -> bool;

private:
  [[nodiscard]] auto intern(std::string key, type_entry entry) -> type_id;

  std::vector<type_entry> entries_;
  std::unordered_map<std::string, type_id> interned_;
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

/// Whether `name` is a trait available from the prelude without any `use`.
[[nodiscard]] auto is_prelude_trait_name(std::string_view name) -> bool;

// ==========================================================================
//  Program index — session-wide declaration lookup tables.
// ==========================================================================

struct type_decl_ref {
  const ast::type_decl *decl = nullptr;
  file_id_type file_id = 0;
};

struct trait_decl_ref {
  const ast::trait_decl *decl = nullptr;
  file_id_type file_id = 0;
};

struct concept_decl_ref {
  const ast::concept_decl *decl = nullptr;
  file_id_type file_id = 0;
};

struct func_decl_ref {
  const ast::func_decl *decl = nullptr;
  file_id_type file_id = 0;
};

struct static_decl_ref {
  const ast::static_decl *decl = nullptr;
  file_id_type file_id = 0;
};

struct variant_ref {
  const ast::type_decl *sum_decl = nullptr;
  const ast::sum_variant *variant = nullptr;
};

struct impl_ref {
  const ast::impl_decl *decl = nullptr;
  std::string module_name;
  file_id_type file_id = 0;
};

struct import_binding {
  std::string local_name;         ///< Name the import introduces locally.
  std::vector<std::string> path;  ///< Source module path of the import.
  std::string leaf_name;          ///< Imported member name; empty for modules.
  bool is_wildcard = false;
  source_span span;
};

struct module_members {
  std::string module_name;
  std::unordered_map<std::string, type_decl_ref> types;
  std::unordered_map<std::string, trait_decl_ref> traits;
  std::unordered_map<std::string, concept_decl_ref> concepts;
  std::unordered_map<std::string, func_decl_ref> functions;
  std::unordered_map<std::string, static_decl_ref> statics;
  std::unordered_map<std::string, variant_ref> variants;
  std::vector<impl_ref> impls;
};

struct program_index {
  std::unordered_map<std::string, module_members> modules;
  std::unordered_map<file_id_type, std::vector<import_binding>> imports;

  [[nodiscard]] auto find_module(std::string_view module_name) const
      -> const module_members *;
};

[[nodiscard]] auto build_program_index(const std::vector<parsed_module> &inputs)
    -> program_index;

} // namespace kira::semantic
