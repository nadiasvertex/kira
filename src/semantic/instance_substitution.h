#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/parser/ast.h"
#include "src/parser/ast_clone.h"
#include "src/parser/source_location.h"
#include "src/semantic/linear_poly.h"
#include "src/semantic/types.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Instance substitution — a template's records, under an instance's answers
//
//  A generic body is checked once, as the template. An instance is not
//  checked again: its records are the template's with each abstract
//  parameter replaced by the instance's answer, copied onto the clone's
//  nodes. This file holds that copy and nothing that decides anything.
//
//  Deciding what an instance's answers are (`instance_subst_for`) and
//  running the decisions the template deferred (`template_elaboration`)
//  need the checker and stay there. The engine here reads and writes the
//  record tables it is handed and the type table, and holds no other state.
// ==========================================================================

/// Maps a type to its fully-resolved form, applied before a type is
/// substituted. The checker passes its inference context's zonk; a caller
/// with no unresolved leaves passes the identity.
using type_settler = std::function<type_id(type_id)>;

/// One instance, stated against its template.
struct instance_subst {
  /// Each abstract parameter of the template (as it was interned while the
  /// template was checked) and the instance's answer for it.
  std::unordered_map<type_id, type_id> params;
  /// The value parameters' constants, for open polynomials (`n + 1`).
  std::unordered_map<std::string, linear_poly> values;
  /// Template node to clone node.
  const ast::clone_map *pairs = nullptr;
  /// The template, and the instance being made from it.
  const ast::func_decl *tmpl = nullptr;
  const ast::func_decl *instance = nullptr;
  /// The source ranges of `static if` branches this instance does not
  /// take. Nothing inside one is elaborated, and its records are dropped.
  std::vector<source_span> dead;
  /// Types already substituted under this instance, so each is interned once.
  std::unordered_map<type_id, type_id> memo;
};

/// The clone of a template node, or the node itself when it has none (it is
/// not part of the template body — a callee's default argument, say).
template <typename T>
[[nodiscard]] auto clone_of(const instance_subst &subst, const T *node)
    -> const T * {
  if (node == nullptr || subst.pairs == nullptr) {
    return node;
  }
  const auto found = subst.pairs->find(node);
  return found == subst.pairs->end() ? node
                                     : static_cast<const T *>(found->second);
}

/// Whether a template node lies in a branch this instance does not take.
[[nodiscard]] auto is_dead(const instance_subst &subst, source_span span)
    -> bool;

/// `id` with every abstract parameter replaced by the instance's answer.
/// Interns the result into `types`, and memoizes it in `subst`.
[[nodiscard]] auto substitute_type(type_table &types,
                                   const type_settler &settle,
                                   instance_subst &subst, type_id id)
    -> type_id;

/// The checker's per-node record tables that an instance copies from its
/// template: references, not owners. The checker builds one over its own
/// maps; a test builds one over local maps.
struct template_record_tables {
  std::unordered_map<const ast::node *, type_id> &node_types;
  std::unordered_map<const ast::node *, file_id_type> &node_files;
  std::unordered_map<const ast::field_pattern *, type_id>
      &struct_pattern_field_types;
  std::unordered_map<const ast::struct_field_init *, type_id>
      &struct_literal_field_types;
  std::unordered_map<const ast::struct_field_init *, type_id>
      &struct_literal_field_expected;
  std::unordered_map<const ast::call_expr *, type_id> &call_expected_types;
  std::unordered_map<const ast::call_expr *, call_argument_mapping>
      &call_argument_mappings;
  std::unordered_map<const ast::call_expr *, resolved_callee> &resolved_callees;
  std::unordered_map<const ast::node *, resolved_callee> &resolved_fn_values;
  std::unordered_map<const ast::binary_expr *, resolved_callee>
      &operator_dispatches;
  std::unordered_map<const ast::binary_expr *, type_id>
      &ord_dispatch_result_types;
  std::unordered_map<const ast::index_expr *, resolved_callee> &index_dispatches;
  std::unordered_map<const ast::index_expr *, resolved_callee>
      &index_set_dispatches;
  std::unordered_map<const ast::index_expr *, resolved_callee>
      &index_mut_dispatches;
  std::unordered_map<const ast::index_expr *, resolved_callee>
      &index_ref_dispatches;
  std::unordered_map<const ast::try_expr *, resolved_callee> &try_conversions;
  std::unordered_map<const ast::try_expr *, type_id> &try_conversion_types;
  std::unordered_map<const ast::array_expr *, array_literal_conversion>
      &array_literal_conversions;
  std::unordered_map<const ast::expr *, interp_dispatch> &interp_dispatches;
  std::unordered_map<const ast::call_expr *, type_param_reflection>
      &type_param_reflections;
  std::unordered_map<const ast::for_stmt *, iterator_loop_dispatch>
      &for_iterator_dispatches;
  std::unordered_map<const ast::node *, iterator_loop_dispatch>
      &comprehension_iterator_dispatches;
  std::unordered_map<const ast::for_expr *, comprehension_dispatch>
      &comprehension_dispatches;
  std::unordered_map<const ast::array_expr *, comprehension_dispatch>
      &runtime_fill_dispatches;
  std::unordered_map<const ast::call_expr *, const ast::literal_expr *>
      &folded_comptime_calls;
  std::unordered_map<const ast::call_expr *, layout_query> &layout_queries;
  std::unordered_map<const ast::call_expr *, type_id> &ptr_casts;
  std::unordered_set<const ast::call_expr *> &slice_from_raw_parts_calls;
  std::unordered_map<const ast::call_expr *, stack_buffer_request>
      &stack_buffers;
  std::unordered_map<const ast::static_decl *, bool> &static_if_taken_branch;
  std::unordered_map<const ast::node *, std::string> &static_global_refs;
  std::unordered_map<const ast::node *, const ast::literal_expr *>
      &static_const_values;
  std::unordered_map<const ast::module_path_expr *, std::vector<type_id>>
      &value_path_types;
  std::unordered_set<const ast::index_expr *> &proven_in_bounds;
  std::unordered_set<const ast::contract_clause *> &elided_contracts;
  std::unordered_map<const ast::node *, const ast::node *> &spliced_fragments;
};

/// Copies every record the template made onto the instance's clone nodes,
/// with its types substituted. Records already on a clone are replaced.
auto copy_template_records(const template_record_tables &tables,
                           type_table &types, const type_settler &settle,
                           instance_subst &subst) -> void;

/// Removes every record the clones of a `static if` branch the instance does
/// not take carry. A no-op when the instance takes every branch.
auto drop_dead_records(const template_record_tables &tables,
                       const instance_subst &subst) -> void;

} // namespace cinder::semantic::detail
