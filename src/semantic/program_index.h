#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"

namespace cinder::semantic {

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

/// A `signature` declaration plus the file it was declared in.
struct signature_decl_ref {
  const ast::signature_decl *decl = nullptr;
  file_id_type file_id = 0;
};

/// A parameterized module (functor) declaration plus the file it was declared
/// in. Kept distinct from ordinary submodules because a functor's body is not
/// elaborated as a module in its own right — it is instantiated per argument
/// tuple by a `use m[args]` import.
struct functor_decl_ref {
  const ast::sub_module_decl *decl = nullptr;
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
  std::unordered_map<std::string, signature_decl_ref> signatures;
  std::unordered_map<std::string, functor_decl_ref> functors;
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

} // namespace cinder::semantic
