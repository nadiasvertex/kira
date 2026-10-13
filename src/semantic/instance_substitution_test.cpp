#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "src/parser/ast.h"
#include "src/semantic/instance_substitution.h"
#include "src/semantic/checker_results.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::semantic::resolved_callee;
using cinder::semantic::type_id;
using cinder::semantic::type_table;
using cinder::semantic::detail::clone_of;
using cinder::semantic::detail::copy_template_records;
using cinder::semantic::detail::drop_dead_records;
using cinder::semantic::detail::instance_subst;
using cinder::semantic::detail::is_dead;
using cinder::semantic::detail::substitute_type;
using cinder::semantic::detail::template_record_tables;
using cinder::semantic::detail::type_settler;
using cinder::testing::expect;
namespace ast = cinder::ast;

/// One map per record table, so a test can read back what a copy wrote.
struct record_maps {
  std::unordered_map<const ast::node *, type_id> node_types;
  std::unordered_map<const ast::node *, cinder::file_id_type>
      node_files;
  std::unordered_map<const ast::field_pattern *, type_id> struct_pattern;
  std::unordered_map<const ast::struct_field_init *, type_id> literal_types;
  std::unordered_map<const ast::struct_field_init *, type_id> literal_expected;
  std::unordered_map<const ast::call_expr *, type_id> call_expected;
  std::unordered_map<const ast::call_expr *,
                     cinder::semantic::call_argument_mapping>
      argument_mappings;
  std::unordered_map<const ast::call_expr *, resolved_callee> callees;
  std::unordered_map<const ast::node *, resolved_callee> fn_values;
  std::unordered_map<const ast::binary_expr *, resolved_callee> operators;
  std::unordered_map<const ast::binary_expr *, type_id> ord_results;
  std::unordered_map<const ast::index_expr *, resolved_callee> index;
  std::unordered_map<const ast::index_expr *, resolved_callee> index_set;
  std::unordered_map<const ast::index_expr *, resolved_callee> index_mut;
  std::unordered_map<const ast::index_expr *, resolved_callee> index_ref;
  std::unordered_map<const ast::try_expr *, resolved_callee> tries;
  std::unordered_map<const ast::try_expr *, type_id> try_types;
  std::unordered_map<const ast::array_expr *,
                     cinder::semantic::array_literal_conversion>
      array_conversions;
  std::unordered_map<const ast::expr *, cinder::semantic::interp_dispatch>
      interps;
  std::unordered_map<const ast::call_expr *,
                     cinder::semantic::type_param_reflection>
      reflections;
  std::unordered_map<const ast::for_stmt *,
                     cinder::semantic::iterator_loop_dispatch>
      for_loops;
  std::unordered_map<const ast::node *, cinder::semantic::iterator_loop_dispatch>
      comprehension_loops;
  std::unordered_map<const ast::for_expr *,
                     cinder::semantic::comprehension_dispatch>
      comprehensions;
  std::unordered_map<const ast::array_expr *,
                     cinder::semantic::comprehension_dispatch>
      fills;
  std::unordered_map<const ast::call_expr *, const ast::literal_expr *> folded;
  std::unordered_map<const ast::call_expr *, cinder::semantic::layout_query>
      layouts;
  std::unordered_map<const ast::call_expr *, type_id> ptr_casts;
  std::unordered_set<const ast::call_expr *> raw_slices;
  std::unordered_map<const ast::call_expr *,
                     cinder::semantic::stack_buffer_request>
      buffers;
  std::unordered_map<const ast::static_decl *, bool> static_if;
  std::unordered_map<const ast::node *, std::string> global_refs;
  std::unordered_map<const ast::node *, const ast::literal_expr *> consts;
  std::unordered_map<const ast::module_path_expr *, std::vector<type_id>>
      value_paths;
  std::unordered_set<const ast::index_expr *> in_bounds;
  std::unordered_set<const ast::contract_clause *> elided;
  std::unordered_map<const ast::node *, const ast::node *> fragments;

  auto tables() -> template_record_tables {
    return template_record_tables{
        node_types,       node_files,   struct_pattern,
        literal_types,    literal_expected, call_expected,
        argument_mappings, callees,     fn_values,
        operators,        ord_results,  index,
        index_set,        index_mut,    index_ref,
        tries,            try_types,    array_conversions,
        interps,          reflections,  for_loops,
        comprehension_loops, comprehensions, fills,
        folded,           layouts,      ptr_casts,
        raw_slices,       buffers,      static_if,
        global_refs,      consts,       value_paths,
        in_bounds,        elided,       fragments};
  }
};

const type_settler identity = [](type_id id) -> type_id { return id; };

/// A template parameter `T`, an instance that answers `T := int32`, and one
/// template/clone pair of nodes.
struct fixture {
  type_table types;
  type_id int32 = types.builtin("int32");
  type_id str = types.builtin("str");
  type_id t = types.type_param("T");
  ast::literal_expr template_node;
  ast::literal_expr clone_node;
  ast::call_expr template_call;
  ast::call_expr clone_call;
  ast::clone_map pairs;
  instance_subst subst;

  fixture() {
    pairs.emplace(&template_node, &clone_node);
    pairs.emplace(&template_call, &clone_call);
    subst.params.emplace(t, int32);
    subst.pairs = &pairs;
  }
};

auto test_substitution_replaces_the_parameter_at_every_depth() -> void {
  auto f = fixture{};
  const auto list_t = f.types.builtin_generic("list", {f.t});
  const auto list_i32 = f.types.builtin_generic("list", {f.int32});
  expect(substitute_type(f.types, identity, f.subst, f.t) == f.int32,
         "a bare parameter becomes the instance's answer");
  expect(substitute_type(f.types, identity, f.subst, list_t) == list_i32,
         "a parameter inside a generic application is replaced");
  const auto fn_t = f.types.fn_of({f.t, f.str}, f.types.tuple_of({f.t}));
  const auto fn_i32 = f.types.fn_of({f.int32, f.str}, f.types.tuple_of({f.int32}));
  expect(substitute_type(f.types, identity, f.subst, fn_t) == fn_i32,
         "parameters in a function's arguments and result are replaced");
  const auto ref_t = f.types.ref_to(f.t, true);
  expect(substitute_type(f.types, identity, f.subst, ref_t) ==
             f.types.ref_to(f.int32, true),
         "a reference keeps its mutability");
}

auto test_substitution_leaves_unrelated_types_alone() -> void {
  auto f = fixture{};
  const auto other = f.types.type_param("U");
  expect(substitute_type(f.types, identity, f.subst, other) == other,
         "a parameter the instance has no answer for is unchanged");
  expect(substitute_type(f.types, identity, f.subst, f.str) == f.str,
         "a concrete type is unchanged");
}

auto test_substitution_settles_before_looking_up() -> void {
  auto f = fixture{};
  const auto open = f.types.builtin("str");
  const auto settle = type_settler{
      [&](type_id id) -> type_id { return id == f.str ? f.t : id; }};
  expect(substitute_type(f.types, settle, f.subst, open) == f.int32,
         "the settler's answer is what gets substituted");
}

auto test_array_length_follows_a_substituted_constant() -> void {
  auto f = fixture{};
  const auto n = f.types.type_param("n");
  const auto four = f.types.const_value(f.types.builtin("usize"), 4);
  f.subst.params.emplace(n, four);
  const auto array_n = f.types.array_of(f.t, std::nullopt, n);
  const auto result = substitute_type(f.types, identity, f.subst, array_n);
  const auto &entry = f.types.entry(result);
  expect(entry.kind == cinder::semantic::type_kind::array_kind &&
             entry.result == f.int32 && entry.array_size == 4,
         "an array's element and length are both substituted");
}

auto test_copy_writes_to_the_clone_with_types_substituted() -> void {
  auto f = fixture{};
  auto maps = record_maps{};
  maps.node_types.emplace(&f.template_node,
                          f.types.builtin_generic("list", {f.t}));
  maps.callees.emplace(&f.template_call, resolved_callee{});
  maps.ptr_casts.emplace(&f.template_call, f.t);
  maps.raw_slices.insert(&f.template_call);
  copy_template_records(maps.tables(), f.types, identity, f.subst);
  expect(maps.node_types.at(&f.clone_node) ==
             f.types.builtin_generic("list", {f.int32}),
         "the clone's type is the template's, substituted");
  expect(maps.node_types.at(&f.template_node) ==
             f.types.builtin_generic("list", {f.t}),
         "the template's own record is not rewritten");
  expect(maps.callees.contains(&f.clone_call),
         "a callee record is copied to the clone");
  expect(maps.ptr_casts.at(&f.clone_call) == f.int32,
         "a cast target is substituted");
  expect(maps.raw_slices.contains(&f.clone_call),
         "a set membership is copied to the clone");
}

auto test_copy_points_a_receiver_at_its_clone() -> void {
  auto f = fixture{};
  auto maps = record_maps{};
  auto record = resolved_callee{};
  record.receiver = &f.template_call;
  maps.callees.emplace(&f.template_call, record);
  copy_template_records(maps.tables(), f.types, identity, f.subst);
  expect(maps.callees.at(&f.clone_call).receiver == &f.clone_call,
         "a receiver inside the template is replaced by its clone");
  expect(clone_of(f.subst, &f.template_call) == &f.clone_call,
         "clone_of finds the paired node");
  const auto outside = ast::call_expr{};
  expect(clone_of(f.subst, &outside) == &outside,
         "clone_of returns a node outside the template unchanged");
}

auto test_dead_branch_records_are_dropped() -> void {
  auto f = fixture{};
  f.clone_node.span = {.start = 10, .end = 20};
  f.clone_call.span = {.start = 40, .end = 50};
  f.subst.dead.push_back({.start = 5, .end = 25});
  expect(is_dead(f.subst, f.clone_node.span),
         "a span inside a dead range is dead");
  expect(!is_dead(f.subst, f.clone_call.span),
         "a span outside every dead range is live");
  auto maps = record_maps{};
  maps.node_types.emplace(&f.clone_node, f.int32);
  maps.callees.emplace(&f.clone_call, resolved_callee{});
  drop_dead_records(maps.tables(), f.subst);
  expect(!maps.node_types.contains(&f.clone_node),
         "a record on a dead clone is removed");
  expect(maps.callees.contains(&f.clone_call),
         "a record on a live clone is kept");
}

} // namespace

auto main() -> int {
  test_substitution_replaces_the_parameter_at_every_depth();
  test_substitution_leaves_unrelated_types_alone();
  test_substitution_settles_before_looking_up();
  test_array_length_follows_a_substituted_constant();
  test_copy_writes_to_the_clone_with_types_substituted();
  test_copy_points_a_receiver_at_its_clone();
  test_dead_branch_records_are_dropped();
  return 0;
}
