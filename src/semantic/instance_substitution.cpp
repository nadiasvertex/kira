#include "src/semantic/instance_substitution.h"

#include <algorithm>
#include <utility>

namespace cinder::semantic::detail {

namespace {

auto substitute_uncached(type_table &types, const type_settler &settle,
                         instance_subst &subst, type_id id) -> type_id;

auto substitute(type_table &types, const type_settler &settle,
                instance_subst &subst, type_id id) -> type_id {
  id = settle(id);
  if (const auto found = subst.memo.find(id); found != subst.memo.end()) {
    return found->second;
  }
  const auto result = substitute_uncached(types, settle, subst, id);
  subst.memo.emplace(id, result);
  return result;
}

auto substitute_uncached(type_table &types, const type_settler &settle,
                         instance_subst &subst, type_id id) -> type_id {
  const auto item = types.entry(id); // copy: interning below can push
  const auto one = [&](type_id inner) -> type_id {
    return substitute(types, settle, subst, inner);
  };
  const auto each =
      [&](const std::vector<type_id> &args) -> std::vector<type_id> {
    auto out = std::vector<type_id>{};
    out.reserve(args.size());
    for (const auto arg : args) {
      out.push_back(one(arg));
    }
    return out;
  };
  switch (item.kind) {
  case type_kind::type_param_kind: {
    const auto found = subst.params.find(id);
    return found == subst.params.end() ? id : found->second;
  }
  case type_kind::param_app_kind: {
    const auto found = subst.params.find(item.result);
    if (found == subst.params.end()) {
      return id;
    }
    const auto ctor = types.entry(found->second);
    if (ctor.kind != type_kind::ctor_ref_kind) {
      return id;
    }
    auto args = each(item.args);
    return ctor.decl != nullptr
               ? types.user_type(*ctor.decl, ctor.module_name, std::move(args))
               : types.builtin_generic(ctor.name, std::move(args));
  }
  case type_kind::symbolic_value_kind:
    return types.symbolic_value(item.result,
                                poly_substitute(item.value, subst.values));
  case type_kind::builtin_generic_kind:
    return types.builtin_generic(item.name, each(item.args));
  case type_kind::tuple_kind:
    return types.tuple_of(each(item.args));
  case type_kind::fn_kind:
    return types.fn_of(each(item.args), one(item.result));
  case type_kind::ref_kind:
    return types.ref_to(one(item.result), item.is_mut);
  case type_kind::ptr_kind:
    return types.ptr_to(one(item.result), item.is_mut);
  case type_kind::array_kind: {
    const auto element = one(item.result);
    const auto length =
        item.args.empty() ? k_unknown_type : one(item.args.front());
    const auto &length_entry = types.entry(length);
    auto size = item.array_size;
    if (length_entry.kind == type_kind::const_value_kind) {
      size = static_cast<uint64_t>(length_entry.value.constant);
    }
    return types.array_of(element, size, length);
  }
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::opaque_kind:
    return item.decl != nullptr
               ? types.user_type(*item.decl, item.module_name, each(item.args))
               : id;
  case type_kind::refinement_kind:
    return types.refinement_of(item.decl, item.name, item.module_name,
                               one(item.result), each(item.args),
                               item.predicate);
  default:
    return id;
  }
}

/// Copies one record map's template entries onto the clone's nodes.
template <typename Map, typename Transform>
auto copy_map(Map &map, const instance_subst &subst, Transform transform)
    -> void {
  using key_type = typename Map::key_type;
  auto copied = std::vector<std::pair<key_type, typename Map::mapped_type>>{};
  for (const auto &[original, clone] : *subst.pairs) {
    const auto found = map.find(static_cast<key_type>(original));
    if (found != map.end()) {
      copied.emplace_back(static_cast<key_type>(clone),
                          transform(found->second));
    }
  }
  for (auto &[key, value] : copied) {
    map.insert_or_assign(key, std::move(value));
  }
}

template <typename Set>
auto copy_set(Set &set, const instance_subst &subst) -> void {
  using key_type = typename Set::key_type;
  auto copied = std::vector<key_type>{};
  for (const auto &[original, clone] : *subst.pairs) {
    if (set.contains(static_cast<key_type>(original))) {
      copied.push_back(static_cast<key_type>(clone));
    }
  }
  set.insert(copied.begin(), copied.end());
}

/// Removes the entries keyed by clone nodes in a dead branch.
template <typename Map>
auto drop_from(Map &map, const instance_subst &subst) -> void {
  using key_type = typename Map::key_type;
  for (const auto &[original, clone] : *subst.pairs) {
    const auto *node = static_cast<key_type>(clone);
    if (map.contains(node) && is_dead(subst, node->span)) {
      map.erase(node);
    }
  }
}

} // namespace

auto is_dead(const instance_subst &subst, source_span span) -> bool {
  return std::ranges::any_of(subst.dead, [&](source_span range) -> bool {
    return !span.empty() && range.start <= span.start && span.end <= range.end;
  });
}

auto substitute_type(type_table &types, const type_settler &settle,
                     instance_subst &subst, type_id id) -> type_id {
  return substitute(types, settle, subst, id);
}

auto copy_template_records(const template_record_tables &tables,
                           type_table &types, const type_settler &settle,
                           instance_subst &subst) -> void {
  const auto type = [&](type_id id) -> type_id {
    return substitute(types, settle, subst, id);
  };
  const auto callee = [&](resolved_callee record) -> resolved_callee {
    record.receiver = clone_of(subst, record.receiver);
    return record;
  };
  const auto same = [](const auto &value) -> auto { return value; };
  copy_map(tables.node_types, subst, type);
  copy_map(tables.node_files, subst, same);
  copy_map(tables.struct_pattern_field_types, subst, type);
  copy_map(tables.struct_literal_field_types, subst, type);
  copy_map(tables.struct_literal_field_expected, subst, type);
  copy_map(tables.call_expected_types, subst, type);
  copy_map(tables.call_argument_mappings, subst,
           [&](call_argument_mapping mapping) -> call_argument_mapping {
             for (auto &arg : mapping.args_by_param) {
               arg = clone_of(subst, arg);
             }
             return mapping;
           });
  copy_map(tables.resolved_callees, subst, callee);
  copy_map(tables.resolved_fn_values, subst, callee);
  copy_map(tables.operator_dispatches, subst, callee);
  copy_map(tables.ord_dispatch_result_types, subst, type);
  copy_map(tables.index_dispatches, subst, callee);
  copy_map(tables.index_set_dispatches, subst, callee);
  copy_map(tables.index_mut_dispatches, subst, callee);
  copy_map(tables.index_ref_dispatches, subst, callee);
  copy_map(tables.try_conversions, subst, callee);
  copy_map(tables.try_conversion_types, subst, type);
  copy_map(
      tables.array_literal_conversions, subst,
      [&](array_literal_conversion conversion) -> array_literal_conversion {
        conversion.callee = callee(conversion.callee);
        conversion.array_type = type(conversion.array_type);
        return conversion;
      });
  copy_map(tables.interp_dispatches, subst,
           [&](interp_dispatch dispatch) -> interp_dispatch {
             dispatch.value_type = type(dispatch.value_type);
             return dispatch;
           });
  copy_map(tables.type_param_reflections, subst, same);
  const auto loop =
      [&](iterator_loop_dispatch dispatch) -> iterator_loop_dispatch {
    dispatch.element_type = type(dispatch.element_type);
    dispatch.adapter_result_type = type(dispatch.adapter_result_type);
    return dispatch;
  };
  copy_map(tables.for_iterator_dispatches, subst, loop);
  copy_map(tables.comprehension_iterator_dispatches, subst, loop);
  const auto comprehension =
      [&](comprehension_dispatch dispatch) -> comprehension_dispatch {
    dispatch.new_callee = callee(dispatch.new_callee);
    dispatch.push_callee = callee(dispatch.push_callee);
    dispatch.list_type = type(dispatch.list_type);
    return dispatch;
  };
  copy_map(tables.comprehension_dispatches, subst, comprehension);
  copy_map(tables.runtime_fill_dispatches, subst, comprehension);
  copy_map(tables.folded_comptime_calls, subst, same);
  copy_map(tables.layout_queries, subst,
           [&](layout_query query) -> layout_query {
             query.operand = type(query.operand);
             return query;
           });
  copy_map(tables.ptr_casts, subst, type);
  copy_set(tables.slice_from_raw_parts_calls, subst);
  copy_map(tables.stack_buffers, subst,
           [&](stack_buffer_request request) -> stack_buffer_request {
             request.element = type(request.element);
             return request;
           });
  copy_map(tables.static_if_taken_branch, subst, same);
  copy_map(tables.static_global_refs, subst, same);
  copy_map(tables.static_const_values, subst, same);
  copy_map(tables.value_path_types, subst,
           [&](std::vector<type_id> path) -> std::vector<type_id> {
             for (auto &segment : path) {
               segment = type(segment);
             }
             return path;
           });
  copy_set(tables.proven_in_bounds, subst);
  copy_set(tables.elided_contracts, subst);
  copy_map(tables.spliced_fragments, subst, same);
}

auto drop_dead_records(const template_record_tables &tables,
                       const instance_subst &subst) -> void {
  if (subst.dead.empty()) {
    return;
  }
  drop_from(tables.node_types, subst);
  drop_from(tables.call_expected_types, subst);
  drop_from(tables.resolved_callees, subst);
  drop_from(tables.resolved_fn_values, subst);
  drop_from(tables.operator_dispatches, subst);
  drop_from(tables.index_dispatches, subst);
  drop_from(tables.index_set_dispatches, subst);
  drop_from(tables.index_mut_dispatches, subst);
  drop_from(tables.index_ref_dispatches, subst);
  drop_from(tables.try_conversions, subst);
  drop_from(tables.array_literal_conversions, subst);
  drop_from(tables.interp_dispatches, subst);
  drop_from(tables.type_param_reflections, subst);
  drop_from(tables.for_iterator_dispatches, subst);
  drop_from(tables.comprehension_iterator_dispatches, subst);
  drop_from(tables.comprehension_dispatches, subst);
  drop_from(tables.runtime_fill_dispatches, subst);
  drop_from(tables.folded_comptime_calls, subst);
  drop_from(tables.layout_queries, subst);
}

} // namespace cinder::semantic::detail
