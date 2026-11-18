#include "src/core/verify.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/core/nodes.h"
#include "src/semantic/aggregate_shape.h"
#include "src/semantic/linear_poly.h"
#include "src/testing/test_assert.h"

// Tests for the core verifier (`src/core/verify.h`). One function per rule,
// each with at least one program that satisfies the rule (no violations at
// all) and one that breaks only that rule (exactly that rule id is reported).
// Every program is built by hand from the node types in `nodes.h`.
//
// P4 (writability) has no id of its own in the verifier's output: a place that
// is not writable is reported where it is used, as A4 (a mutable borrow), S2
// (an assignment), E8 (a writable alias) or E10 (a `borrow_mut` capture). The
// P4 tests therefore expect those ids.

namespace {

namespace ast = cinder::ast;
namespace sem = cinder::semantic;
using namespace cinder::core;
using cinder::source_span;
using cinder::token_kind;
using cinder::testing::expect;
using cinder::testing::fail;
using tid = sem::type_id;

constexpr auto k_span = source_span::dummy();

// ===========================================================================
//  Result checking
// ===========================================================================

std::string g_rule;                  // the rule group being tested
std::map<std::string, int> g_counts; // checks per rule group

auto begin(std::string rule) -> void { g_rule = std::move(rule); }

auto dump(const std::vector<violation> &violations) -> std::string {
  auto out = std::string{};
  for (const auto &v : violations) {
    out += "\n    [" + v.rule + "] " + v.function + ": " + v.message;
  }
  return out;
}

/// The program satisfies every rule: no violations at all.
auto ok(const std::vector<violation> &violations, std::string_view label) -> void {
  ++g_counts[g_rule];
  if (!violations.empty()) {
    fail(std::string(label) + " (should be clean, got:" + dump(violations) + ")");
  }
}

/// The program breaks exactly the rules in `rules`, and no other.
auto bad(const std::vector<violation> &violations, std::initializer_list<std::string_view> rules,
         std::string_view label) -> void {
  ++g_counts[g_rule];
  auto want = std::set<std::string>{};
  for (const auto r : rules) {
    want.insert(std::string(r));
  }
  auto got = std::set<std::string>{};
  for (const auto &v : violations) {
    got.insert(v.rule);
  }
  if (got != want) {
    auto wanted = std::string{};
    for (const auto &r : want) {
      wanted += r + " ";
    }
    fail(std::string(label) + " (expected only " + wanted + "but got:" +
         (violations.empty() ? std::string(" nothing") : dump(violations)) + ")");
  }
}

auto bad(const std::vector<violation> &violations, std::string_view rule, std::string_view label)
    -> void {
  bad(violations, {rule}, label);
}

// ===========================================================================
//  The type world
// ===========================================================================

/// Symbols of the standard parameters.
namespace sym {
constexpr symbol_id n = 1;     // int32
constexpr symbol_id p = 2;     // pair
constexpr symbol_id rp = 3;    // &pair
constexpr symbol_id rm = 4;    // &mut pair
constexpr symbol_id tup = 5;   // (int32, bool)
constexpr symbol_id arr = 6;   // array[int32, 3]
constexpr symbol_id sl = 7;    // slice[int32]
constexpr symbol_id st = 8;    // str
constexpr symbol_id sh = 9;    // shape
constexpr symbol_id h = 10;    // holder
constexpr symbol_id rsh = 11;  // &shape
constexpr symbol_id raw = 12;  // *pair
constexpr symbol_id rawm = 13; // *mut pair
constexpr symbol_id slm = 14;  // slice_mut[int32]
constexpr symbol_id fnv = 15;  // fn(int32) -> int32
constexpr symbol_id task = 16; // task[int32, never]
constexpr symbol_id rh = 17;   // &holder
constexpr symbol_id b = 18;    // bool
constexpr symbol_id ri = 19;   // &int32
constexpr symbol_id task_e = 20; // task[int32, str]
constexpr symbol_id raw_i = 21;  // *int32
constexpr symbol_id fl = 22;     // float64
constexpr symbol_id raw_mi = 23; // *mut int32
constexpr symbol_id un = 24;     // uninit[int32, 4]
constexpr symbol_id rfn = 25;    // &fn(int32) -> int32
constexpr symbol_id rmfn = 26;   // &mut fn(int32) -> int32
constexpr symbol_id raw_b = 27;  // *bool
} // namespace sym

struct world {
  sem::checked_types checked;
  tid i32{}, i64{}, f64{}, boolean{}, unit{}, str{}, usize{}, chr{}, byte{}, never{};
  tid list_i32{}, pair{}, holder{}, shape{}, tuple{}, arr{}, slice{}, slice_mut{};
  tid ref_pair{}, mut_ref_pair{}, ref_shape{}, ref_holder{}, raw_pair{}, raw_mut_pair{};
  tid ref_i32{}, raw_i32{}, fn_i32{}, task_i32{}, task_err{}, result_i32_str{}, option_i32{};
  tid typ_t{}, raw_mut_i32{}, uninit_i32{}, ref_fn{}, mut_ref_fn{}, raw_bool{};
  ast::type_decl pair_decl, holder_decl, shape_decl;
  ast::func_decl add_decl, method_decl, generic_decl, instance_decl;
  symbol_id next_symbol = 100;
  std::map<symbol_id, tid> std_types;

  world() {
    auto &t = checked.types;
    i32 = t.builtin("int32");
    i64 = t.builtin("int64");
    f64 = t.builtin("float64");
    boolean = t.bool_type();
    unit = t.builtin("unit");
    str = t.builtin("str");
    usize = t.usize_type();
    chr = t.char_type();
    byte = t.builtin("byte");
    never = t.builtin("never");
    list_i32 = t.builtin_generic("list", {i32});
    option_i32 = t.builtin_generic("option", {i32});
    typ_t = t.type_param("T");

    pair_decl.name = "pair";
    pair_decl.definition = ast::make<ast::struct_type_def>();
    holder_decl.name = "holder";
    holder_decl.definition = ast::make<ast::struct_type_def>();
    shape_decl.name = "shape";
    shape_decl.definition = ast::make<ast::sum_type_def>();
    pair = t.user_type(pair_decl, "m", {});
    holder = t.user_type(holder_decl, "m", {});
    shape = t.user_type(shape_decl, "m", {});
    checked.shapes[pair].fields = {{.name = "a", .type = i32}, {.name = "b", .type = boolean}};
    checked.shapes[holder].fields = {{.name = "items", .type = list_i32},
                                     {.name = "n", .type = i32}};
    checked.shapes[shape].variants = {{.name = "circle", .payload = {i32}},
                                      {.name = "empty", .payload = {}},
                                      {.name = "rect", .payload = {i32, boolean}}};

    tuple = t.tuple_of({i32, boolean});
    arr = t.array_of(i32, 3, t.const_value(usize, 3));
    slice = t.builtin_generic("slice", {i32});
    slice_mut = t.builtin_generic("slice_mut", {i32});
    ref_pair = t.ref_to(pair, false);
    mut_ref_pair = t.ref_to(pair, true);
    ref_shape = t.ref_to(shape, false);
    ref_holder = t.ref_to(holder, false);
    raw_pair = t.ptr_to(pair, false);
    raw_mut_pair = t.ptr_to(pair, true);
    ref_i32 = t.ref_to(i32, false);
    raw_i32 = t.ptr_to(i32, false);
    fn_i32 = t.fn_of({i32}, i32);
    raw_mut_i32 = t.ptr_to(i32, true);
    uninit_i32 = t.builtin_generic("uninit", {i32, t.const_value(usize, 4)});
    ref_fn = t.ref_to(fn_i32, false);
    mut_ref_fn = t.ref_to(fn_i32, true);
    raw_bool = t.ptr_to(boolean, false);
    task_i32 = t.builtin_generic("task", {i32, never});
    task_err = t.builtin_generic("task", {i32, str});
    result_i32_str = t.builtin_generic("result", {i32, str});

    std_types = {{sym::n, i32},         {sym::p, pair},          {sym::rp, ref_pair},
                 {sym::rm, mut_ref_pair}, {sym::tup, tuple},     {sym::arr, arr},
                 {sym::sl, slice},      {sym::st, str},          {sym::sh, shape},
                 {sym::h, holder},      {sym::rsh, ref_shape},   {sym::raw, raw_pair},
                 {sym::rawm, raw_mut_pair}, {sym::slm, slice_mut}, {sym::fnv, fn_i32},
                 {sym::task, task_i32}, {sym::rh, ref_holder},   {sym::b, boolean},
                 {sym::ri, ref_i32},    {sym::task_e, task_err}, {sym::raw_i, raw_i32},
                 {sym::fl, f64},
                 {sym::raw_mi, raw_mut_i32}, {sym::un, uninit_i32}, {sym::rfn, ref_fn},
                 {sym::rmfn, mut_ref_fn}, {sym::raw_b, raw_bool}};
  }
  world(const world &) = delete;
  auto operator=(const world &) -> world & = delete;

  auto fresh() -> symbol_id { return next_symbol++; }

  [[nodiscard]] auto std_params() const -> std::vector<param> {
    auto out = std::vector<param>{};
    for (const auto &[s, t] : std_types) {
      out.push_back(param{.symbol = s, .name = "p" + std::to_string(s), .type = t});
    }
    return out;
  }
};

// ===========================================================================
//  Builders
// ===========================================================================

template <typename T, typename... A> auto vec_of(A &&...a) -> std::vector<T> {
  auto v = std::vector<T>{};
  v.reserve(sizeof...(A));
  (v.push_back(std::forward<A>(a)), ...);
  return v;
}
template <typename... A> auto S(A &&...a) -> ptr_vec<stmt> {
  return vec_of<ptr<stmt>>(std::forward<A>(a)...);
}
template <typename... A> auto E(A &&...a) -> ptr_vec<expr> {
  return vec_of<ptr<expr>>(std::forward<A>(a)...);
}
template <typename... A> auto STEPS(A &&...a) -> std::vector<projection> {
  return vec_of<projection>(std::forward<A>(a)...);
}
template <typename... A> auto TESTS(A &&...a) -> std::vector<pattern_test> {
  return vec_of<pattern_test>(std::forward<A>(a)...);
}
template <typename... A> auto BINDS(A &&...a) -> std::vector<pattern_binding> {
  return vec_of<pattern_binding>(std::forward<A>(a)...);
}

// ---- literals -------------------------------------------------------------

auto make_lit(tid t, token_kind k, std::string spelling) -> ptr<literal> {
  auto l = std::make_unique<literal>(k_span, t);
  l->token = k;
  l->spelling = std::move(spelling);
  return l;
}
auto ilit(tid t, std::string s = "1") -> ptr<literal> {
  return make_lit(t, token_kind::int_lit, std::move(s));
}
auto blit(const world &w, bool v = true) -> ptr<literal> {
  return make_lit(w.boolean, v ? token_kind::kw_true : token_kind::kw_false, v ? "true" : "false");
}

// ---- places ---------------------------------------------------------------

auto pl(symbol_id s, tid t) -> place {
  auto p = place{};
  p.root.kind = place_root::origin::local;
  p.root.symbol = s;
  p.root.type = t;
  return p;
}
auto loc(const world &w, symbol_id s) -> place { return pl(s, w.std_types.at(s)); }
auto global_pl(std::string module, std::string name, tid t) -> place {
  auto p = place{};
  p.root.kind = place_root::origin::global;
  p.root.module = std::move(module);
  p.root.name = std::move(name);
  p.root.type = t;
  return p;
}
auto temp_pl(ptr<expr> value, tid t) -> place {
  auto p = place{};
  p.root.kind = place_root::origin::temporary;
  p.root.value = std::move(value);
  p.root.type = t;
  return p;
}
auto proj(projection::step k, tid t, std::string name = {}, std::size_t index = 0) -> projection {
  auto p = projection{};
  p.kind = k;
  p.type = t;
  p.name = std::move(name);
  p.index = index;
  return p;
}
auto field_step(tid t, std::string name, std::size_t index) -> projection {
  return proj(projection::step::field, t, std::move(name), index);
}
auto tuple_step(tid t, std::size_t index) -> projection {
  return proj(projection::step::tuple_element, t, {}, index);
}
auto payload_step(tid t, std::string variant, std::size_t index) -> projection {
  return proj(projection::step::variant_payload, t, std::move(variant), index);
}
auto deref_step(tid t) -> projection { return proj(projection::step::deref, t); }
auto element_step(tid t, ptr<expr> subscript) -> projection {
  auto p = proj(projection::step::element, t);
  p.subscript = std::move(subscript);
  return p;
}
auto with(place p, projection step) -> place {
  p.path.push_back(std::move(step));
  return p;
}

// ---- access ---------------------------------------------------------------

auto use_as(place p, tid type, use::mode how) -> ptr<expr> {
  auto u = std::make_unique<use>(k_span, type);
  u->from = std::move(p);
  u->how = how;
  return u;
}
/// A `use` with the type of its place and the mode its type requires.
auto use_of(const world &w, place p) -> ptr<expr> {
  const auto t = p.type();
  return use_as(std::move(p), t, w.checked.is_copy(t) ? use::mode::copy : use::mode::move);
}
auto borrow_of(const world &w, place p, bool is_mut = false, bool inserted = false,
               bool two_phase = false) -> ptr<expr> {
  auto t = const_cast<world &>(w).checked.types.ref_to(p.type(), is_mut);
  auto b = std::make_unique<borrow>(k_span, t);
  b->of = std::move(p);
  b->is_mut = is_mut;
  b->inserted = inserted;
  b->two_phase = two_phase;
  return b;
}
auto borrow_as(place p, tid type, bool is_mut) -> ptr<expr> {
  auto b = std::make_unique<borrow>(k_span, type);
  b->of = std::move(p);
  b->is_mut = is_mut;
  return b;
}

// ---- statements -----------------------------------------------------------

auto mk_let(symbol_id s, tid t, ptr<expr> value = nullptr, bool is_mut = false) -> ptr<stmt> {
  auto l = std::make_unique<let>(k_span);
  l->symbol = s;
  l->name = "v" + std::to_string(s);
  l->type = t;
  l->is_mut = is_mut;
  l->value = std::move(value);
  return l;
}
auto ex(ptr<expr> value) -> ptr<stmt> {
  auto s = std::make_unique<expr_stmt>(k_span);
  s->value = std::move(value);
  return s;
}
auto assign_to(place target, ptr<expr> value) -> ptr<stmt> {
  auto a = std::make_unique<assign>(k_span);
  a->target = std::move(target);
  a->value = std::move(value);
  return a;
}
auto jump_of(node_kind k, ptr<expr> value = nullptr) -> ptr<stmt> {
  auto j = std::make_unique<jump>(k, k_span);
  j->value = std::move(value);
  return j;
}
auto ret(ptr<expr> value = nullptr) -> ptr<stmt> {
  return jump_of(node_kind::return_stmt, std::move(value));
}
auto brk() -> ptr<stmt> { return jump_of(node_kind::break_stmt); }
auto cont() -> ptr<stmt> { return jump_of(node_kind::continue_stmt); }
auto contract(ptr<expr> condition) -> ptr<stmt> {
  auto c = std::make_unique<contract_check>(k_span);
  c->condition = std::move(condition);
  c->message = "pre";
  return c;
}

auto blk(tid t, ptr_vec<stmt> statements = {}, ptr<expr> result = nullptr) -> ptr<block> {
  auto b = std::make_unique<block>(k_span, t);
  b->statements = std::move(statements);
  b->result = std::move(result);
  return b;
}
auto ublock(const world &w, ptr_vec<stmt> statements = {}) -> ptr<block> {
  return blk(w.unit, std::move(statements));
}
auto loop_of(ptr<expr> condition, ptr<block> body) -> ptr<stmt> {
  auto l = std::make_unique<while_loop>(k_span);
  l->condition = std::move(condition);
  l->body = std::move(body);
  return l;
}

// ---- expressions ----------------------------------------------------------

auto prim_bin(ast::binary_op op, tid t, ptr<expr> a, ptr<expr> b) -> ptr<expr> {
  auto p = std::make_unique<primitive>(k_span, t);
  p->binary = op;
  p->operands.push_back(std::move(a));
  p->operands.push_back(std::move(b));
  return p;
}
auto prim_un(ast::unary_op op, tid t, ptr<expr> a) -> ptr<expr> {
  auto p = std::make_unique<primitive>(k_span, t);
  p->unary = op;
  p->operands.push_back(std::move(a));
  return p;
}
auto cast_to(tid t, ptr<expr> operand) -> ptr<expr> {
  auto c = std::make_unique<cast>(k_span, t);
  c->operand = std::move(operand);
  return c;
}
/// `indices` defaults, for a struct, to declaration order (`0..names.size()`).
auto agg(node_kind k, tid t, ptr_vec<expr> elements, std::vector<std::string> names = {},
         std::optional<std::uint64_t> repeat = std::nullopt,
         std::optional<std::vector<std::size_t>> indices = std::nullopt) -> ptr<expr> {
  auto a = std::make_unique<aggregate_init>(k, k_span, t);
  a->elements = std::move(elements);
  a->field_names = std::move(names);
  if (indices) {
    a->field_indices = std::move(*indices);
  } else if (k == node_kind::struct_init) {
    for (std::size_t i = 0; i < a->field_names.size(); ++i) {
      a->field_indices.push_back(i);
    }
  }
  a->repeat = repeat;
  return a;
}
auto variant_of(tid t, std::string name, std::size_t index, ptr_vec<expr> payload) -> ptr<expr> {
  auto v = std::make_unique<variant_init>(k_span, t);
  v->variant = std::move(name);
  v->variant_index = index;
  v->payload = std::move(payload);
  return v;
}
auto if_of(tid t, ptr<expr> condition, ptr<block> then_branch, ptr<block> else_branch = nullptr)
    -> ptr<expr> {
  auto i = std::make_unique<if_expr>(k_span, t);
  i->condition = std::move(condition);
  i->then_branch = std::move(then_branch);
  i->else_branch = std::move(else_branch);
  return i;
}
auto await_of(tid t, ptr<expr> task) -> ptr<expr> {
  auto a = std::make_unique<await_expr>(k_span, t);
  a->task = std::move(task);
  return a;
}

// ---- patterns and match ---------------------------------------------------

auto make_test(pattern_test::form f, tid t) -> pattern_test {
  auto p = pattern_test{};
  p.kind = f;
  p.span = k_span;
  p.subject_type = t;
  return p;
}
auto t_any(tid t) -> pattern_test { return make_test(pattern_test::form::any, t); }
auto t_variant(tid t, std::size_t index, std::vector<pattern_test> parts = {}) -> pattern_test {
  auto p = make_test(pattern_test::form::variant, t);
  p.variant_index = index;
  p.parts = std::move(parts);
  return p;
}
auto t_tuple(tid t, std::vector<pattern_test> parts) -> pattern_test {
  auto p = make_test(pattern_test::form::tuple, t);
  p.parts = std::move(parts);
  return p;
}
auto t_record(tid t, std::vector<std::size_t> indices, std::vector<pattern_test> parts)
    -> pattern_test {
  auto p = make_test(pattern_test::form::record, t);
  p.field_indices = std::move(indices);
  p.parts = std::move(parts);
  return p;
}
auto t_array(tid t, std::vector<pattern_test> parts) -> pattern_test {
  auto p = make_test(pattern_test::form::array, t);
  p.parts = std::move(parts);
  return p;
}
auto t_lit(tid t, ptr<literal> low) -> pattern_test {
  auto p = make_test(pattern_test::form::literal, t);
  p.low = std::move(low);
  return p;
}
auto t_range(tid t, ptr<literal> low, ptr<literal> high) -> pattern_test {
  auto p = make_test(pattern_test::form::range, t);
  p.low = std::move(low);
  p.high = std::move(high);
  return p;
}

auto bnd(symbol_id s, tid type, std::vector<projection> path,
         pattern_binding::mode how = pattern_binding::mode::copy, bool writable = false)
    -> pattern_binding {
  auto b = pattern_binding{};
  b.symbol = s;
  b.name = "b" + std::to_string(s);
  b.type = type;
  b.path = std::move(path);
  b.how = how;
  b.writable = writable;
  b.span = k_span;
  return b;
}

auto alt_of(pattern_test t, std::vector<pattern_binding> bindings = {}) -> pattern_alternative {
  auto a = pattern_alternative{};
  a.test = std::move(t);
  a.bindings = std::move(bindings);
  return a;
}
template <typename... A> auto ALTS(A &&...a) -> std::vector<pattern_alternative> {
  return vec_of<pattern_alternative>(std::forward<A>(a)...);
}
auto arm_of(std::vector<pattern_alternative> alts, ptr<block> body, ptr<expr> guard = nullptr)
    -> match_arm {
  auto m = match_arm{};
  m.alternatives = std::move(alts);
  m.body = std::move(body);
  m.guard = std::move(guard);
  m.span = k_span;
  return m;
}
template <typename... A> auto ARMS(A &&...a) -> std::vector<match_arm> {
  return vec_of<match_arm>(std::forward<A>(a)...);
}
auto match_of(tid t, place subject, std::vector<match_arm> arms) -> ptr<expr> {
  auto m = std::make_unique<match_expr>(k_span, t);
  m->subject = std::move(subject);
  m->arms = std::move(arms);
  return m;
}
/// A unit `match` statement with one arm of one alternative and an empty body.
auto match1(const world &w, place subject, pattern_test t, std::vector<pattern_binding> bs = {},
            ptr<block> body = nullptr) -> ptr<stmt> {
  return ex(match_of(w.unit, std::move(subject),
                     ARMS(arm_of(ALTS(alt_of(std::move(t), std::move(bs))),
                                 body ? std::move(body) : ublock(w)))));
}

// ---- lambdas --------------------------------------------------------------

auto cap(symbol_id s, place from, capture::mode how, tid type) -> capture {
  auto c = capture{};
  c.symbol = s;
  c.from = std::move(from);
  c.how = how;
  c.type = type;
  return c;
}
template <typename... A> auto CAPS(A &&...a) -> std::vector<capture> {
  return vec_of<capture>(std::forward<A>(a)...);
}
auto prm(symbol_id s, tid t) -> param {
  return param{.symbol = s, .name = "q" + std::to_string(s), .type = t};
}
auto lambda_of(world &w, std::vector<param> params, std::vector<capture> caps, tid ret,
               ptr<block> body) -> ptr<expr> {
  auto types = std::vector<tid>{};
  for (const auto &p : params) {
    types.push_back(p.type);
  }
  auto l = std::make_unique<lambda>(k_span, w.checked.types.fn_of(types, ret));
  l->params = std::move(params);
  l->captures = std::move(caps);
  l->return_type = ret;
  l->body = std::move(body);
  return l;
}

// ---- calls ----------------------------------------------------------------

auto sig_of(world &w, std::vector<tid> params, tid ret) -> tid {
  return w.checked.types.fn_of(std::move(params), ret);
}
/// The `intrinsic def`s the next `verify_all` puts in its module.
std::vector<intrinsic_decl> g_decls;

/// An intrinsic callee that no `intrinsic def` declares.
auto raw_intrinsic(tid signature, std::string name, receiver_mode r = receiver_mode::none)
    -> callee {
  auto c = callee{};
  c.kind = callee::form::intrinsic;
  c.name = std::move(name);
  c.signature = signature;
  c.receiver = r;
  return c;
}
/// An intrinsic callee of a made-up name. Tests of the other rules use it, so
/// it declares itself with exactly its own signature.
auto intrinsic(tid signature, std::string name = "f", receiver_mode r = receiver_mode::none)
    -> callee {
  if (!name.empty()) {
    g_decls.push_back(intrinsic_decl{.name = name, .signature = signature, .generic = false});
  }
  return raw_intrinsic(signature, std::move(name), r);
}
auto fn_callee(const ast::func_decl *decl, tid signature, std::string name = "add1",
               receiver_mode r = receiver_mode::none) -> callee {
  auto c = callee{};
  c.kind = callee::form::function;
  c.decl = decl;
  c.name = std::move(name);
  c.signature = signature;
  c.receiver = r;
  return c;
}
auto value_callee(tid signature, ptr<expr> value) -> callee {
  auto c = callee{};
  c.kind = callee::form::value;
  c.name = "closure";
  c.signature = signature;
  c.value = std::move(value);
  return c;
}
auto call_of(tid result, callee target, ptr_vec<expr> args) -> ptr<expr> {
  auto c = std::make_unique<call>(k_span, result);
  c->target = std::move(target);
  c->args = std::move(args);
  return c;
}
auto fnref_of(tid type, callee target) -> ptr<expr> {
  auto f = std::make_unique<function_ref>(k_span, type);
  f->target = std::move(target);
  return f;
}

// ---- functions and programs -----------------------------------------------

auto make_fn(std::string name, std::vector<param> params, tid ret, ptr<block> body)
    -> ptr<function> {
  auto f = std::make_unique<function>();
  f->name = std::move(name);
  f->module = "m";
  f->span = k_span;
  f->params = std::move(params);
  f->return_type = ret;
  f->body_type = ret;
  f->body = std::move(body);
  return f;
}

/// A function over the standard parameters.
auto std_fn(const world &w, tid ret, ptr_vec<stmt> statements, ptr<expr> result = nullptr)
    -> ptr<function> {
  return make_fn("f", w.std_params(), ret, blk(ret, std::move(statements), std::move(result)));
}

/// Verifies `functions` and `statics` in module `m`, which always also holds
/// the statics `g: int32 = 1` and `gp: pair = pair{1, true}`.
auto verify_all(world &w, std::vector<ptr<function>> functions,
                std::vector<static_global> statics = {}) -> std::vector<violation> {
  auto m = module{};
  m.name = "m";
  m.intrinsics = std::exchange(g_decls, {});
  for (auto &f : functions) {
    m.functions.push_back(std::move(f));
  }
  m.statics.push_back(
      static_global{.name = "g", .module = "m", .type = w.i32, .value = ilit(w.i32)});
  m.statics.push_back(static_global{
      .name = "gp",
      .module = "m",
      .type = w.pair,
      .value = agg(node_kind::struct_init, w.pair, E(ilit(w.i32), blit(w)), {"a", "b"})});
  for (auto &s : statics) {
    m.statics.push_back(std::move(s));
  }
  const auto *program = &m;
  return verify(std::span<const module *const>(&program, 1), w.checked);
}
auto verify_one(world &w, ptr<function> f) -> std::vector<violation> {
  auto fs = std::vector<ptr<function>>{};
  fs.push_back(std::move(f));
  return verify_all(w, std::move(fs));
}
/// Verifies a unit function over the standard parameters.
auto run_std(world &w, ptr_vec<stmt> statements) -> std::vector<violation> {
  return verify_one(w, std_fn(w, w.unit, std::move(statements)));
}
/// A bare function with the given parameters, return type and statements.
auto run_fn(world &w, std::vector<param> params, tid ret, ptr_vec<stmt> statements,
            ptr<expr> result = nullptr) -> std::vector<violation> {
  return verify_one(
      w, make_fn("f", std::move(params), ret, blk(ret, std::move(statements), std::move(result))));
}

auto num(world &w) -> ptr<expr> { return ilit(w.i32); }

// ===========================================================================
//  T: types
// ===========================================================================

auto let_of_type(world &w, tid t) -> std::vector<violation> {
  return run_std(w, S(mk_let(w.fresh(), t)));
}

auto test_t1_settled() -> void {
  begin("T1");
  auto w = world{};
  auto &t = w.checked.types;
  ok(let_of_type(w, w.list_i32), "a let of `list[int32]` is settled");
  ok(let_of_type(w, w.arr), "an array with its length slot is settled");
  bad(let_of_type(w, sem::k_unknown_type), "T1", "an unknown type");
  bad(let_of_type(w, sem::k_error_type), "T1", "an error type");
  bad(let_of_type(w, t.fresh_type_var()), "T1", "an inference variable");
  bad(let_of_type(w, t.builtin_generic("list", {sem::k_unknown_type})), "T1",
      "an unknown type inside `list[...]`");
  bad(let_of_type(w, t.ref_to(t.tuple_of({w.i32, t.fresh_type_var()}), false)), "T1",
      "an inference variable nested in a tuple behind a reference");
  bad(let_of_type(w, t.array_of(w.i32, 3)), "T1", "an array without its length slot");
  bad(run_fn(w, {prm(w.fresh(), sem::k_unknown_type)}, w.unit, S()), "T1",
      "a parameter of unknown type");
  bad(run_std(w, S(ex(lambda_of(w, {prm(w.fresh(), t.fresh_type_var())}, {}, w.unit, ublock(w))))),
      "T1", "a lambda parameter that is an inference variable");
  ok(run_std(w, S(ex(lambda_of(w, {prm(w.fresh(), w.i32)}, {}, w.unit, ublock(w))))),
     "a lambda parameter of a settled type");
  bad(run_std(w, S(ex(ilit(sem::k_unknown_type)))), {"T1", "E1"},
      "a literal of unknown type (also breaks E1: no token fits `unknown`)");
}

auto test_t2_erased() -> void {
  begin("T2");
  auto w = world{};
  auto &t = w.checked.types;
  ok(let_of_type(w, t.fn_of({w.i32}, w.list_i32)), "a plain fn type has none of them");
  const auto refined = t.refinement_of(nullptr, "pos", "m", w.i32, {}, nullptr);
  bad(let_of_type(w, refined), "T2", "a refinement");
  bad(let_of_type(w, t.builtin_generic("list", {refined})), "T2", "a refinement inside `list`");
  bad(let_of_type(w, t.fresh_existential("some x", {})), "T2", "an existential");
  bad(let_of_type(w, t.tuple_of({w.i32, t.fresh_existential("some y", {})})), "T2",
      "an existential inside a tuple");
  bad(let_of_type(w, t.ctor_ref("option", "", nullptr, 1)), "T2", "an unapplied constructor");
  bad(let_of_type(w, t.ref_to(t.ctor_ref("list", "", nullptr, 1), true)), "T2",
      "an unapplied constructor behind a reference");
}

auto test_t3_generic_scope() -> void {
  begin("T3");
  auto w = world{};
  auto &t = w.checked.types;
  const auto T = w.typ_t;

  // def ident[T](x: T) -> T { x }
  const auto x = w.fresh();
  auto generic = make_fn("ident", {prm(x, T)}, T, blk(T, {}, use_of(w, pl(x, T))));
  generic->generic_params = {T};
  {
    auto fs = std::vector<ptr<function>>{};
    fs.push_back(std::move(generic));
    ok(verify_all(w, std::move(fs)), "a type parameter in a function that lists it");
  }
  bad(let_of_type(w, T), "T3", "a type parameter in a function that does not list it");
  bad(let_of_type(w, t.builtin_generic("list", {T})), "T3", "a parameter nested in `list`");

  // The same parameter is fine in the function that lists it and wrong in the
  // other function of the module.
  {
    auto lists = make_fn("lists", {}, w.unit, blk(w.unit, S(mk_let(w.fresh(), T))));
    lists->generic_params = {T};
    auto omits = make_fn("omits", {}, w.unit, blk(w.unit, S(mk_let(w.fresh(), T))));
    auto fs = std::vector<ptr<function>>{};
    fs.push_back(std::move(lists));
    fs.push_back(std::move(omits));
    const auto v = verify_all(w, std::move(fs));
    bad(v, "T3", "a parameter listed by one function and not by the next");
    expect(v.size() == 1 && v[0].function == "omits", "T3 is reported against the second function");
  }

  // A parameter application `F[A]`.
  const auto F = t.type_param("F", 1);
  const auto app = t.param_app(F, {w.i32});
  bad(let_of_type(w, app), "T3", "a parameter application in a function that omits it");
  {
    auto f = make_fn("hk", {}, w.unit, blk(w.unit, S(mk_let(w.fresh(), app))));
    f->generic_params = {F};
    ok(verify_one(w, std::move(f)), "a parameter application in a function that lists its head");
  }

  // A symbolic value `n + 1` over the value parameter `n`.
  const auto n = t.type_param("n");
  auto poly = sem::poly_variable(sem::poly_param_var("n", n));
  const auto sym_value = t.symbolic_value(w.usize, poly);
  const auto vec_n = t.builtin_generic("list", {w.i32, sym_value});
  bad(let_of_type(w, vec_n), "T3", "a symbolic value in a function that omits its parameter");
  {
    auto f = make_fn("sized", {}, w.unit, blk(w.unit, S(mk_let(w.fresh(), vec_n))));
    f->generic_params = {n};
    ok(verify_one(w, std::move(f)), "a symbolic value in a function that lists its parameter");
  }
}

// ===========================================================================
//  P: places
// ===========================================================================

auto test_p1_roots() -> void {
  begin("P1");
  auto w = world{};
  const auto x = w.fresh();
  // local
  ok(run_std(w, S(mk_let(x, w.i32, use_of(w, loc(w, sym::n))), ex(use_of(w, pl(x, w.i32))))),
     "a local read after its let");
  bad(run_std(w, S(ex(use_of(w, pl(777, w.i32))))), "P1", "a local that is not in scope");
  bad(run_std(w, S(mk_let(x, w.i32, use_of(w, loc(w, sym::n))), ex(use_of(w, pl(x, w.i64))))),
      "P1", "a local read at a type other than its declared type");
  // global
  ok(run_std(w, S(ex(use_of(w, global_pl("m", "g", w.i32))))), "a global that is a static");
  bad(run_std(w, S(ex(use_of(w, global_pl("m", "nope", w.i32))))), "P1",
      "a global that names no static");
  bad(run_std(w, S(ex(use_of(w, global_pl("other", "g", w.i32))))), "P1",
      "a global named in the wrong module");
  bad(run_std(w, S(ex(use_of(w, global_pl("m", "g", w.i64))))), "P1",
      "a global at a type other than the static's");
  // temporary
  ok(run_std(w, S(ex(use_of(w, temp_pl(num(w), w.i32))))), "a temporary root holding a value");
  bad(run_std(w, S(ex(use_as(temp_pl(nullptr, w.i32), w.i32, use::mode::copy)))), "P1",
      "a temporary root with no value");
  bad(run_std(w, S(ex(use_as(temp_pl(num(w), w.i64), w.i64, use::mode::copy)))), "P1",
      "a temporary root whose type is not its value's type");
}

auto test_p2_field_step() -> void {
  begin("P2");
  auto w = world{};
  const auto p = [&] { return loc(w, sym::p); };
  ok(run_std(w, S(ex(use_of(w, with(p(), field_step(w.i32, "a", 0)))),
                  ex(use_of(w, with(p(), field_step(w.boolean, "b", 1)))))),
     "field steps of a struct, with index, name and type from the shape");
  bad(run_std(w, S(ex(use_of(w, with(p(), field_step(w.i32, "wrong", 0)))))), "P2",
      "a field step whose name is not the shape's");
  bad(run_std(w, S(ex(use_of(w, with(p(), field_step(w.i32, "a", 5)))))), "P2",
      "a field step whose index is out of range");
  bad(run_std(w, S(ex(use_of(w, with(p(), field_step(w.i64, "a", 0)))))), "P2",
      "a field step whose type is not the field's");
  bad(run_std(w, S(ex(use_of(w, with(p(), field_step(w.boolean, "b", 0)))))), "P2",
      "a field step whose index names another field");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::n), field_step(w.i32, "a", 0)))))), "P2",
      "a field step on an integer");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::tup), field_step(w.i32, "a", 0)))))), "P2",
      "a field step on a tuple");
}

auto test_p2_tuple_step() -> void {
  begin("P2");
  auto w = world{};
  const auto t = [&] { return loc(w, sym::tup); };
  ok(run_std(w, S(ex(use_of(w, with(t(), tuple_step(w.i32, 0)))),
                  ex(use_of(w, with(t(), tuple_step(w.boolean, 1)))))),
     "tuple element steps");
  bad(run_std(w, S(ex(use_of(w, with(t(), tuple_step(w.i32, 2)))))), "P2",
      "a tuple element index out of range");
  bad(run_std(w, S(ex(use_of(w, with(t(), tuple_step(w.boolean, 0)))))), "P2",
      "a tuple element step whose type is not the element's");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::p), tuple_step(w.i32, 0)))))), "P2",
      "a tuple element step on a struct");
}

auto test_p2_variant_payload_step() -> void {
  begin("P2");
  auto w = world{};
  // Each case matches `sh` with a `circle` or `rect` test and one binding.
  const auto circle_test = [&] { return t_variant(w.shape, 0, TESTS(t_any(w.i32))); };
  const auto rect_test = [&] {
    return t_variant(w.shape, 2, TESTS(t_any(w.i32), t_any(w.boolean)));
  };
  const auto x = w.fresh();
  const auto run = [&](pattern_test t, pattern_binding b) {
    return run_std(w, S(match1(w, loc(w, sym::sh), std::move(t), BINDS(std::move(b)))));
  };
  ok(run(circle_test(), bnd(x, w.i32, STEPS(payload_step(w.i32, "circle", 0)))),
     "a payload step of the tested variant");
  ok(run(rect_test(), bnd(x, w.boolean, STEPS(payload_step(w.boolean, "rect", 1)))),
     "the second payload of a two-payload variant");
  bad(run(circle_test(), bnd(x, w.i32, STEPS(payload_step(w.i32, "nope", 0)))), "P2",
      "a payload step naming no variant");
  bad(run(circle_test(), bnd(x, w.i32, STEPS(payload_step(w.i32, "circle", 1)))), "P2",
      "a payload index out of range");
  bad(run(circle_test(), bnd(x, w.boolean, STEPS(payload_step(w.boolean, "circle", 0)))), "P2",
      "a payload step whose type is not the payload's");
  bad(run_std(w, S(match1(w, loc(w, sym::p), t_any(w.pair),
                          BINDS(bnd(x, w.i32, STEPS(payload_step(w.i32, "circle", 0))))))),
      "P2", "a payload step on a struct");
  // `option` has no recorded shape: its variants come from its argument.
  const auto opt = w.fresh();
  const auto some_test = t_variant(w.option_i32, 0, TESTS(t_any(w.i32)));
  ok(run_fn(w, {prm(opt, w.option_i32)}, w.unit,
            S(match1(w, pl(opt, w.option_i32), t_variant(w.option_i32, 0, TESTS(t_any(w.i32))),
                     BINDS(bnd(x, w.i32, STEPS(payload_step(w.i32, "some", 0))))))),
     "a payload step of `option`");
  (void)some_test;
}

auto test_p2_deref_step() -> void {
  begin("P2");
  auto w = world{};
  ok(run_std(w, S(ex(use_of(w, with(loc(w, sym::ri), deref_step(w.i32)))))), "deref of `&T`");
  ok(run_std(w, S(ex(use_of(w, with(with(loc(w, sym::rp), deref_step(w.pair)),
                                    field_step(w.i32, "a", 0)))))),
     "a field read through a `&T` of a struct");
  ok(run_std(w, S(ex(use_of(w, with(loc(w, sym::rawm), deref_step(w.pair)))))),
     "deref of `*mut T`");
  ok(run_std(w, S(ex(use_of(w, with(loc(w, sym::raw_i), deref_step(w.i32)))))), "deref of `*T`");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::n), deref_step(w.i32)))))), "P2",
      "deref of an integer");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::p), deref_step(w.pair)))))), "P2",
      "deref of a struct");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::ri), deref_step(w.i64)))))), "P2",
      "deref whose type is not the referent");
}

auto test_p2_element_step() -> void {
  begin("P2");
  auto w = world{};
  const auto idx = [&] { return ilit(w.usize, "0"); };
  ok(run_std(w, S(ex(use_of(w, with(loc(w, sym::arr), element_step(w.i32, idx())))))),
     "an element of an array");
  ok(run_std(w, S(ex(use_of(w, with(loc(w, sym::sl), element_step(w.i32, idx())))))),
     "an element of a slice");
  ok(run_std(w, S(ex(use_of(w, with(loc(w, sym::slm), element_step(w.i32, idx())))))),
     "an element of a mutable slice");
  ok(run_std(w, S(ex(use_of(w, with(loc(w, sym::st), element_step(w.byte, idx())))))),
     "an element of `str`");
  ok(run_std(w, S(ex(use_of(w, with(loc(w, sym::arr), element_step(w.i32, num(w))))))),
     "a subscript of any integer type");
  ok(run_std(w, S(ex(use_of(w, with(loc(w, sym::raw_i), element_step(w.i32, idx())))))),
     "an element of a `*T`");
  ok(run_std(w, S(ex(use_of(w, with(loc(w, sym::raw_mi), element_step(w.i32, idx())))))),
     "an element of a `*mut T`");
  ok(run_std(w, S(ex(use_of(w, with(loc(w, sym::un), element_step(w.i32, idx())))))),
     "an element of an `uninit` buffer");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::raw_i), element_step(w.i64, idx())))))), "P2",
      "a `*T` element whose type is not `T`");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::raw_mi), element_step(w.boolean, idx())))))), "P2",
      "a `*mut T` element whose type is not `T`");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::un), element_step(w.i64, idx())))))), "P2",
      "an `uninit` element whose type is not `T`");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::arr), element_step(w.i32, blit(w))))))), "P2",
      "a subscript that is a bool");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::arr), element_step(w.i32, nullptr))))))
      , "P2", "an element step with no subscript");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::arr), element_step(w.boolean, idx())))))), "P2",
      "an array element whose type is not the element type");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::sl), element_step(w.i64, idx())))))), "P2",
      "a slice element whose type is not the element type");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::st), element_step(w.chr, idx())))))), "P2",
      "an element of `str` that is not `byte`");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::n), element_step(w.i32, idx())))))), "P2",
      "an element step on an integer");
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::p), element_step(w.i32, idx())))))), "P2",
      "an element step on a struct");
}

auto test_p3_variant_payload_position() -> void {
  begin("P3");
  auto w = world{};
  const auto x = w.fresh();
  const auto payload = [&] { return STEPS(payload_step(w.i32, "circle", 0)); };
  ok(run_std(w, S(match1(w, loc(w, sym::sh), t_variant(w.shape, 0, TESTS(t_any(w.i32))),
                         BINDS(bnd(x, w.i32, payload()))))),
     "a payload step under a `variant` test of the same variant");
  // Outside a pattern binding's path.
  bad(run_std(w, S(ex(use_of(w, with(loc(w, sym::sh), payload_step(w.i32, "circle", 0)))))), "P3",
      "a payload step in an ordinary place");
  // Under no test, or a test that is not a variant test.
  bad(run_std(w, S(match1(w, loc(w, sym::sh), t_any(w.shape), BINDS(bnd(x, w.i32, payload()))))),
      "P3", "a payload step under an `any` test");
  // Under a variant test of a different variant.
  bad(run_std(w, S(match1(w, loc(w, sym::sh), t_variant(w.shape, 1),
                          BINDS(bnd(x, w.i32, payload()))))),
      "P3", "a payload step under a test of another variant");
  // At the right position: a tuple whose first part is the variant test.
  const auto tuple_shape = w.checked.types.tuple_of({w.shape, w.i32});
  const auto subject = w.fresh();
  const auto tuple_path = [&](std::size_t part) {
    return STEPS(tuple_step(part == 0 ? w.shape : w.i32, part));
  };
  (void)tuple_path;
  const auto make_case = [&](pattern_test t) {
    auto path = STEPS(tuple_step(w.shape, 0), payload_step(w.i32, "circle", 0));
    return run_fn(w, {prm(subject, tuple_shape)}, w.unit,
                  S(match1(w, pl(subject, tuple_shape), std::move(t),
                           BINDS(bnd(x, w.i32, std::move(path))))));
  };
  ok(make_case(t_tuple(tuple_shape, TESTS(t_variant(w.shape, 0, TESTS(t_any(w.i32))),
                                          t_any(w.i32)))),
     "a payload step under the variant test at the same tuple position");
  bad(make_case(t_tuple(tuple_shape, TESTS(t_any(w.shape), t_variant(w.i32, 0)))), {"P3", "E7"},
      "a variant test at a different tuple position (the other part is not even a sum)");
  bad(make_case(t_tuple(tuple_shape, TESTS(t_any(w.shape), t_any(w.i32)))), "P3",
      "no variant test at the position of the payload step");
}

// ---- P4, observed through A4, S2, E8 and E10 -------------------------------

/// A `&mut` of `target` as a statement.
auto mut_borrow(const world &w, place target) -> ptr<stmt> {
  return ex(borrow_of(w, std::move(target), true));
}
/// `let x: type` without a value, `mut` or not, then a `&mut` of it.
auto borrow_local(world &w, tid type, bool is_mut) -> std::vector<violation> {
  const auto x = w.fresh();
  return run_std(w, S(mk_let(x, type, nullptr, is_mut), mut_borrow(w, pl(x, type))));
}

auto test_p4_roots() -> void {
  begin("P4");
  auto w = world{};
  ok(borrow_local(w, w.pair, true), "a `mut` local is writable");
  bad(borrow_local(w, w.pair, false), "A4", "a local `let` is an immutable binding");
  ok(run_std(w, S(mut_borrow(w, loc(w, sym::p)))), "a by-value parameter is writable");
  bad(run_std(w, S(mut_borrow(w, global_pl("m", "g", w.i32)))), "A4", "a global is a static value");
  ok(run_std(w, S(mut_borrow(w, temp_pl(num(w), w.i32)))), "a temporary is writable");
  ok(run_std(w, S(mut_borrow(w, with(temp_pl(use_of(w, loc(w, sym::rm)), w.mut_ref_pair),
                                     deref_step(w.pair))))),
     "a deref of a `&mut T` held in a temporary is writable");
  bad(run_std(w, S(mut_borrow(w, with(temp_pl(use_of(w, loc(w, sym::ri)), w.ref_i32),
                                      deref_step(w.i32))))),
      "A4", "a deref of a `&T` held in a temporary is behind a shared reference");

  // A pattern binding is writable only when its `writable` flag says so.
  const auto x = w.fresh();
  const auto binding_case = [&](bool writable) {
    const auto body = [&] {
      return blk(w.unit, S(mut_borrow(w, pl(x, w.i32))));
    };
    return run_std(
        w, S(match1(w, loc(w, sym::p), t_any(w.pair),
                    BINDS(bnd(x, w.i32, STEPS(field_step(w.i32, "a", 0)),
                              pattern_binding::mode::copy, writable)),
                    body())));
  };
  ok(binding_case(true), "a writable binding is writable");
  bad(binding_case(false), "A4", "a binding that is not writable is an immutable binding");

  // Captures: `copy`, `move` and `borrow_mut` are writable; `borrow` is not.
  const auto c = w.fresh();
  const auto capture_case = [&](capture::mode how, tid type, place from) {
    auto body = blk(w.unit, S(mut_borrow(w, pl(c, type))));
    auto lam = lambda_of(w, {}, CAPS(cap(c, std::move(from), how, type)), w.unit, std::move(body));
    return run_std(w, S(ex(std::move(lam))));
  };
  ok(capture_case(capture::mode::move, w.pair, loc(w, sym::p)), "a `move` capture is writable");
  ok(capture_case(capture::mode::borrow_mut, w.pair, loc(w, sym::p)),
     "a `borrow_mut` capture is writable");
  ok(capture_case(capture::mode::copy, w.i32, loc(w, sym::n)), "a `copy` capture is writable");
  const auto assign_case = [&](capture::mode how, tid type, place from) {
    auto body = blk(w.unit, S(assign_to(pl(c, type), ilit(type))));
    auto lam = lambda_of(w, {}, CAPS(cap(c, std::move(from), how, type)), w.unit, std::move(body));
    return run_std(w, S(ex(std::move(lam))));
  };
  ok(assign_case(capture::mode::copy, w.i32, loc(w, sym::n)),
     "a lambda body assigns to its `copy` capture");
  bad(assign_case(capture::mode::borrow, w.i32, loc(w, sym::n)), "S2",
      "a lambda body assigns to its `borrow` capture");
  bad(capture_case(capture::mode::borrow, w.pair, loc(w, sym::p)), "A4",
      "a `borrow` capture is an immutable binding");
}

auto test_p4_steps() -> void {
  begin("P4");
  auto w = world{};
  const auto idx = [&] { return ilit(w.usize, "0"); };
  const auto local_of = [&](tid type, bool is_mut, auto make_place) {
    const auto x = w.fresh();
    return run_std(w, S(mk_let(x, type, nullptr, is_mut), mut_borrow(w, make_place(pl(x, type)))));
  };
  const auto deref_of = [&](place p, tid referent) { return with(std::move(p), deref_step(referent)); };

  // deref of `&mut T` makes the place writable whatever came before.
  ok(local_of(w.mut_ref_pair, false,
              [&](place p) { return deref_of(std::move(p), w.pair); }),
     "deref of `&mut T` through an immutable binding");
  ok(run_std(w, S(mut_borrow(w, deref_of(loc(w, sym::rm), w.pair)))),
     "deref of a `&mut T` parameter");
  ok(run_std(w, S(mut_borrow(w, deref_of(loc(w, sym::rawm), w.pair)))), "deref of `*mut T`");
  // deref of `&T` / `*T` makes it behind a shared reference, even from a `mut` local.
  bad(local_of(w.ref_pair, true, [&](place p) { return deref_of(std::move(p), w.pair); }), "A4",
      "deref of `&T` from a `mut` local");
  bad(run_std(w, S(mut_borrow(w, deref_of(loc(w, sym::rp), w.pair)))), "A4",
      "deref of a `&T` parameter");
  bad(run_std(w, S(mut_borrow(w, deref_of(loc(w, sym::raw), w.pair)))), "A4", "deref of `*T`");
  // A field after a deref takes the deref's writability.
  ok(run_std(w, S(mut_borrow(w, with(deref_of(loc(w, sym::rm), w.pair), field_step(w.i32, "a", 0))))),
     "a field behind `&mut`");
  bad(run_std(w, S(mut_borrow(w, with(deref_of(loc(w, sym::rp), w.pair), field_step(w.i32, "a", 0))))),
      "A4", "a field behind `&`");
  // element of a mutable slice is writable; of a shared slice or `str` it is not.
  ok(local_of(w.slice_mut, false,
              [&](place p) { return with(std::move(p), element_step(w.i32, idx())); }),
     "an element of a mutable slice through an immutable binding");
  bad(local_of(w.slice, true,
               [&](place p) { return with(std::move(p), element_step(w.i32, idx())); }),
      "A4", "an element of a shared slice from a `mut` local");
  bad(local_of(w.str, true,
               [&](place p) { return with(std::move(p), element_step(w.byte, idx())); }),
      "A4", "an element of `str` from a `mut` local");
  // element of a `*mut T` or an `uninit` buffer is writable; of a `*T` it is not.
  ok(local_of(w.raw_mut_i32, false,
              [&](place p) { return with(std::move(p), element_step(w.i32, idx())); }),
     "an element of a `*mut T` through an immutable binding");
  ok(local_of(w.uninit_i32, false,
              [&](place p) { return with(std::move(p), element_step(w.i32, idx())); }),
     "an element of an `uninit` buffer through an immutable binding");
  bad(local_of(w.raw_i32, true,
               [&](place p) { return with(std::move(p), element_step(w.i32, idx())); }),
      "A4", "an element of a `*T` from a `mut` local");
  // any other step keeps what came before.
  ok(local_of(w.pair, true, [&](place p) { return with(std::move(p), field_step(w.i32, "a", 0)); }),
     "a field of a `mut` local");
  bad(local_of(w.pair, false,
               [&](place p) { return with(std::move(p), field_step(w.i32, "a", 0)); }),
      "A4", "a field of an immutable local");
  ok(local_of(w.tuple, true, [&](place p) { return with(std::move(p), tuple_step(w.i32, 0)); }),
     "a tuple element of a `mut` local");
  bad(local_of(w.tuple, false, [&](place p) { return with(std::move(p), tuple_step(w.i32, 0)); }),
      "A4", "a tuple element of an immutable local");
  ok(local_of(w.arr, true,
              [&](place p) { return with(std::move(p), element_step(w.i32, idx())); }),
     "an element of an array held in a `mut` local");
  bad(local_of(w.arr, false,
               [&](place p) { return with(std::move(p), element_step(w.i32, idx())); }),
      "A4", "an element of an array held in an immutable local");
  bad(run_std(w, S(mut_borrow(w, with(global_pl("m", "gp", w.pair), field_step(w.i32, "a", 0))))),
      "A4", "a field of a static");
}

auto test_p4_through_assignment() -> void {
  begin("P4");
  auto w = world{};
  const auto x = w.fresh();
  ok(run_std(w, S(mk_let(x, w.i32, num(w), true), assign_to(pl(x, w.i32), num(w)))),
     "assignment to a `mut` local");
  bad(run_std(w, S(mk_let(x, w.i32, num(w), false), assign_to(pl(x, w.i32), num(w)))), "S2",
      "assignment to an immutable local");
  ok(run_std(w, S(assign_to(with(loc(w, sym::rm), deref_step(w.pair)), use_of(w, loc(w, sym::p))))),
     "assignment through `&mut`");
  bad(run_std(w, S(assign_to(with(loc(w, sym::rp), deref_step(w.pair)), use_of(w, loc(w, sym::p))))),
      "S2", "assignment through `&`");
  bad(run_std(w, S(assign_to(global_pl("m", "g", w.i32), num(w)))), "S2",
      "assignment to a static");
}

// ===========================================================================
//  A: access
// ===========================================================================

auto test_a1_use() -> void {
  begin("A1");
  auto w = world{};
  ok(run_std(w, S(ex(use_as(loc(w, sym::n), w.i32, use::mode::copy)))), "a copy of a `copy` type");
  ok(run_std(w, S(ex(use_as(loc(w, sym::p), w.pair, use::mode::move)))),
     "a move of a non-`copy` type");
  ok(run_std(w, S(ex(use_as(loc(w, sym::ri), w.ref_i32, use::mode::copy)))), "a copy of `&T`");
  bad(run_std(w, S(ex(use_as(loc(w, sym::n), w.i64, use::mode::copy)))), "A1",
      "a `use` whose type is not its place's");
  bad(run_std(w, S(ex(use_as(loc(w, sym::n), w.i32, use::mode::move)))), "A1",
      "a move of a `copy` type");
  bad(run_std(w, S(ex(use_as(loc(w, sym::p), w.pair, use::mode::copy)))), "A1",
      "a copy of a non-`copy` type");
  bad(run_std(w, S(ex(use_as(loc(w, sym::rm), w.mut_ref_pair, use::mode::copy)))), "A1",
      "a copy of `&mut T`");
}

auto test_a2_moves() -> void {
  begin("A2");
  auto w = world{};
  const auto mv = [&](place p) {
    const auto t = p.type();
    return run_std(w, S(ex(use_as(std::move(p), t, use::mode::move))));
  };
  ok(mv(loc(w, sym::p)), "a move of a whole local");
  ok(mv(with(loc(w, sym::h), field_step(w.list_i32, "items", 0))), "a move of a field");
  ok(mv(with(loc(w, sym::raw), deref_step(w.pair))),
     "a move out of a raw pointer is not a move out of a reference");
  bad(mv(with(loc(w, sym::rp), deref_step(w.pair))), "A2", "a move out of `*r` for `r: &T`");
  bad(mv(with(loc(w, sym::rm), deref_step(w.pair))), "A2", "a move out of `*r` for `r: &mut T`");
  bad(mv(with(with(loc(w, sym::rh), deref_step(w.holder)), field_step(w.list_i32, "items", 0))),
      "A2", "a move of a field behind a reference");
  {
    const auto pairs = w.checked.types.array_of(w.pair, 2, w.checked.types.const_value(w.usize, 2));
    const auto a = w.fresh();
    bad(run_fn(w, {prm(a, pairs)}, w.unit,
               S(ex(use_as(with(pl(a, pairs), element_step(w.pair, ilit(w.usize, "0"))), w.pair,
                           use::mode::move)))),
        "A2", "a move of an array element");
  }
  bad(mv(global_pl("m", "gp", w.pair)), "A2", "a move out of a static");

  // Raw pointers and `uninit` buffers: the machine layer's reads may move.
  {
    auto &t = w.checked.types;
    const auto idx = [&] { return ilit(w.usize, "0"); };
    const auto raw_list = t.ptr_to(w.list_i32, false);
    const auto uninit_list = t.builtin_generic("uninit", {w.list_i32, t.const_value(w.usize, 4)});
    const auto lists = t.array_of(w.list_i32, 2, t.const_value(w.usize, 2));
    const auto slice_list = t.builtin_generic("slice", {w.list_i32});
    const auto move_of = [&](tid root, projection step) {
      const auto a = w.fresh();
      return run_fn(w, {prm(a, root)}, w.unit,
                    S(ex(use_as(with(pl(a, root), std::move(step)), w.list_i32,
                                use::mode::move))));
    };
    ok(move_of(raw_list, deref_step(w.list_i32)), "a move of a non-`copy` `*raw` deref");
    ok(move_of(raw_list, element_step(w.list_i32, idx())),
       "a move of a non-`copy` element of a raw pointer");
    ok(move_of(uninit_list, element_step(w.list_i32, idx())),
       "a move of a non-`copy` element of an `uninit` buffer");
    bad(move_of(lists, element_step(w.list_i32, idx())), "A2",
        "a move of a non-`copy` element of an array");
    bad(move_of(slice_list, element_step(w.list_i32, idx())), "A2",
        "a move of a non-`copy` element of a slice");
    bad(move_of(t.ref_to(w.list_i32, false), deref_step(w.list_i32)), "A2",
        "a move of a non-`copy` deref of a reference");
    // `str` elements are `byte`, which is `copy`, so a `move` use also breaks A1.
    bad(run_std(w, S(ex(use_as(with(loc(w, sym::st), element_step(w.byte, idx())), w.byte,
                                use::mode::move)))),
        {"A1", "A2"}, "a move of an element of `str`");
    // E8 follows A2: a `copy` binding through a raw-pointer element is fine.
    const auto bound = w.fresh();
    ok(run_std(w, S(match1(w, loc(w, sym::raw_i), t_any(w.raw_i32),
                           BINDS(bnd(bound, w.i32, STEPS(element_step(w.i32, idx())),
                                     pattern_binding::mode::copy))))),
       "a `copy` binding through a raw-pointer element");
    bad(run_std(w, S(match1(w, loc(w, sym::arr), t_any(w.arr),
                            BINDS(bnd(bound, w.i32, STEPS(element_step(w.i32, idx())),
                                      pattern_binding::mode::copy))))),
        "E8", "a `copy` binding through an array element");
  }

  // A `move` pattern binding.
  const auto x = w.fresh();
  const auto items = [&] { return field_step(w.list_i32, "items", 0); };
  ok(run_std(w, S(match1(w, loc(w, sym::h), t_any(w.holder),
                         BINDS(bnd(x, w.list_i32, STEPS(items()), pattern_binding::mode::move))))),
     "a `move` binding of a field of an owned subject");
  bad(run_std(w, S(match1(w, loc(w, sym::rh), t_any(w.ref_holder),
                          BINDS(bnd(x, w.holder, STEPS(deref_step(w.holder)),
                                    pattern_binding::mode::move))))),
      "A2", "a `move` binding whose path derefs a reference");
  bad(run_std(w, S(match1(w, with(loc(w, sym::rh), deref_step(w.holder)), t_any(w.holder),
                          BINDS(bnd(x, w.list_i32, STEPS(items()), pattern_binding::mode::move))))),
      "A2", "a `move` binding under a subject that derefs a reference");

  // A `move` capture.
  const auto c = w.fresh();
  const auto capture_move = [&](place from, tid t) {
    return run_std(w, S(ex(lambda_of(w, {}, CAPS(cap(c, std::move(from), capture::mode::move, t)),
                                     w.unit, ublock(w)))));
  };
  ok(capture_move(loc(w, sym::p), w.pair), "a `move` capture of a local");
  bad(capture_move(with(loc(w, sym::rh), deref_step(w.holder)), w.holder), "A2",
      "a `move` capture out of a reference");
  bad(capture_move(global_pl("m", "gp", w.pair), w.pair), "A2", "a `move` capture of a static");
}

auto test_a2_alias_and_borrowed_capture() -> void {
  begin("A2");
  auto w = world{};
  // Intended strict reading: moving out of an `alias` binding or a borrowed
  // capture breaks A2.
  const auto x = w.fresh();
  const auto body = [&] {
    return blk(w.unit, S(ex(use_as(pl(x, w.list_i32), w.list_i32, use::mode::move))));
  };
  bad(run_std(w, S(match1(w, loc(w, sym::h), t_any(w.holder),
                          BINDS(bnd(x, w.list_i32, STEPS(field_step(w.list_i32, "items", 0)),
                                    pattern_binding::mode::alias)),
                          body()))),
      "A2", "a move out of an `alias` binding");
  const auto c = w.fresh();
  const auto inner = w.fresh();
  bad(run_std(w, S(ex(lambda_of(
                     w, {}, CAPS(cap(c, loc(w, sym::p), capture::mode::borrow, w.pair)), w.unit,
                     blk(w.unit, S(ex(use_as(pl(c, w.pair), w.pair, use::mode::move)))))))),
      "A2", "a move out of a borrowed capture");
  bad(run_std(w, S(ex(lambda_of(
                     w, {}, CAPS(cap(c, loc(w, sym::p), capture::mode::borrow_mut, w.pair)), w.unit,
                     blk(w.unit, S(ex(use_as(pl(c, w.pair), w.pair, use::mode::move)))))))),
      "A2", "a move out of a `borrow_mut` capture");
  (void)inner;
  ok(run_std(w, S(ex(lambda_of(w, {}, CAPS(cap(c, loc(w, sym::p), capture::mode::move, w.pair)),
                               w.unit,
                               blk(w.unit, S(ex(use_as(pl(c, w.pair), w.pair, use::mode::move)))))))),
     "a move out of a `move` capture");
}

/// A2 through raw pointers: a `deref` or an `element` of a raw pointer starts
/// the path over, so only the steps after it count.
auto test_a2_through_raw_pointers() -> void {
  begin("A2");
  auto w = world{};
  auto &t = w.checked.types;
  auto s_decl = ast::type_decl{};
  s_decl.name = "s";
  s_decl.definition = ast::make<ast::struct_type_def>();
  const auto s_type = t.user_type(s_decl, "m", {});
  const auto raw_list = t.ptr_to(w.list_i32, false);
  w.checked.shapes[s_type].fields = {{.name = "data", .type = raw_list},
                                     {.name = "items", .type = w.list_i32}};
  const auto ref_s = t.ref_to(s_type, false);
  const auto ref_list = t.ref_to(w.list_i32, false);
  const auto raw_ref_list = t.ptr_to(ref_list, false);
  const auto self = w.fresh(), p = w.fresh();
  const auto mv = [&](place at) {
    const auto type = at.type();
    return run_fn(w, {prm(self, ref_s), prm(p, raw_ref_list)}, w.unit,
                  S(ex(use_as(std::move(at), type, use::mode::move))));
  };
  const auto from_self = [&] { return with(pl(self, ref_s), deref_step(s_type)); };
  ok(mv(with(with(from_self(), field_step(raw_list, "data", 0)),
             element_step(w.list_i32, num(w)))),
     "a move of `self.data[i]` where `data` is a raw pointer behind `&S`");
  bad(mv(with(from_self(), field_step(w.list_i32, "items", 1))), "A2",
      "a move of `(*self).items`, which is behind a reference");
  bad(mv(with(with(pl(p, raw_ref_list), deref_step(ref_list)), deref_step(w.list_i32))), "A2",
      "a move through a reference reached by a raw pointer");
}

/// A generic function reads `pair[T]`, whose shape is only in `open_shapes`.
auto test_open_shapes() -> void {
  begin("P2");
  auto w = world{};
  auto &t = w.checked.types;
  const auto T = w.typ_t;
  const auto pair_t = t.user_type(w.pair_decl, "m", {T});
  const auto run = [&](tid field_type, bool register_shape) {
    if (register_shape) {
      w.checked.open_shapes[pair_t].fields = {{.name = "a", .type = T}, {.name = "b", .type = w.boolean}};
    }
    const auto x = w.fresh();
    auto f = make_fn("g", {prm(x, pair_t)}, w.unit,
                     blk(w.unit, S(mk_let(w.fresh(), field_type,
                                         use_of(w, with(pl(x, pair_t),
                                                        field_step(field_type, "a", 0)))))));
    f->generic_params = {T};
    return verify_one(w, std::move(f));
  };
  ok(run(T, true), "a field read of a generic struct whose shape is only in `open_shapes`");
  bad(run(w.i32, true), "P2", "a field read of a generic struct with the wrong field type");
  // Without any shape the step cannot be followed.
  auto w2 = world{};
  const auto pair2 = w2.checked.types.user_type(w2.pair_decl, "m", {w2.typ_t});
  const auto x = w2.fresh();
  auto f = make_fn("g", {prm(x, pair2)}, w2.unit,
                   blk(w2.unit, S(mk_let(w2.fresh(), w2.typ_t,
                                         use_of(w2, with(pl(x, pair2),
                                                         field_step(w2.typ_t, "a", 0)))))));
  f->generic_params = {w2.typ_t};
  bad(verify_one(w2, std::move(f)), "P2", "a field read of a generic struct with no recorded shape");
}

auto test_a3_borrow_type() -> void {
  begin("A3");
  auto w = world{};
  ok(run_std(w, S(ex(borrow_of(w, loc(w, sym::p))))), "`&place` has type `&T`");
  ok(run_std(w, S(ex(borrow_of(w, loc(w, sym::p), true)))), "`&mut place` has type `&mut T`");
  bad(run_std(w, S(ex(borrow_as(loc(w, sym::p), w.ref_i32, false)))), "A3",
      "a borrow of a pair typed `&int32`");
  bad(run_std(w, S(ex(borrow_as(loc(w, sym::p), w.mut_ref_pair, false)))), "A3",
      "a shared borrow typed `&mut T`");
  bad(run_std(w, S(ex(borrow_as(loc(w, sym::p), w.ref_pair, true)))), "A3",
      "a mutable borrow typed `&T`");
  bad(run_std(w, S(ex(borrow_as(loc(w, sym::p), w.pair, false)))), "A3",
      "a borrow whose type is not a reference");
  bad(run_std(w, S(ex(borrow_as(loc(w, sym::p), w.checked.types.ptr_to(w.pair, false), false)))),
      "A3", "a borrow typed as a raw pointer");
}

auto test_a4_mutable_borrow_is_writable() -> void {
  begin("A4");
  auto w = world{};
  // The writability cases themselves are in test_p4_*; these are the A4 basics.
  const auto x = w.fresh();
  ok(run_std(w, S(mk_let(x, w.pair, nullptr, true), mut_borrow(w, pl(x, w.pair)))),
     "a mutable borrow of a `mut` local");
  ok(run_std(w, S(ex(borrow_of(w, with(loc(w, sym::rp), deref_step(w.pair)))))),
     "a shared borrow of a place that is not writable");
  bad(run_std(w, S(mk_let(x, w.pair, nullptr, false), mut_borrow(w, pl(x, w.pair)))), "A4",
      "a mutable borrow of an immutable local");
  bad(run_std(w, S(mut_borrow(w, with(loc(w, sym::rp), deref_step(w.pair))))), "A4",
      "a mutable borrow through `&`");
}

auto test_a5_two_phase() -> void {
  begin("A5");
  auto w = world{};
  const auto sig = sig_of(w, {w.mut_ref_pair}, w.unit);
  const auto call_with = [&](ptr<expr> arg) {
    return run_std(w, S(ex(call_of(w.unit, intrinsic(sig), E(std::move(arg))))));
  };
  ok(call_with(borrow_of(w, loc(w, sym::p), true, true, true)),
     "`two_phase` on an inserted `&mut` call argument");
  ok(call_with(borrow_of(w, loc(w, sym::p), true, true, false)), "an inserted `&mut` without it");
  bad(call_with(borrow_of(w, loc(w, sym::p), true, false, true)), "A5",
      "`two_phase` on a borrow the source wrote");
  {
    const auto sig_ref = sig_of(w, {w.ref_pair}, w.unit);
    bad(run_std(w, S(ex(call_of(w.unit, intrinsic(sig_ref),
                                E(borrow_of(w, loc(w, sym::p), false, true, true)))))),
        "A5", "`two_phase` on a shared borrow");
  }
  bad(run_std(w, S(ex(borrow_of(w, loc(w, sym::p), true, true, true)))), "A5",
      "`two_phase` on a borrow that is not a call argument");
  bad(run_std(w, S(mk_let(w.fresh(), w.mut_ref_pair, borrow_of(w, loc(w, sym::p), true, true, true)))),
      "A5", "`two_phase` on a borrow bound by a `let`");
}

// ===========================================================================
//  C: calls
// ===========================================================================

/// An argument of type `t` over the standard parameters.
auto arg_for(world &w, tid t) -> ptr<expr> {
  if (t == w.ref_pair) {
    return borrow_of(w, loc(w, sym::p));
  }
  if (t == w.mut_ref_pair) {
    return borrow_of(w, loc(w, sym::p), true, true);
  }
  if (t == w.pair) {
    return use_of(w, loc(w, sym::p));
  }
  if (t == w.ref_i32) {
    return borrow_of(w, loc(w, sym::n));
  }
  return use_of(w, loc(w, sym::n));
}

auto test_c1_signature_and_arguments() -> void {
  begin("C1");
  auto w = world{};
  const auto sig = sig_of(w, {w.i32, w.boolean}, w.i32);
  const auto call_args = [&](tid result, ptr_vec<expr> args, tid signature) {
    return run_std(w, S(ex(call_of(result, intrinsic(signature), std::move(args)))));
  };
  ok(call_args(w.i32, E(num(w), blit(w)), sig), "one argument per parameter, exact types");
  ok(call_args(w.unit, E(), sig_of(w, {}, w.unit)), "a call with no parameters");
  bad(call_args(w.i32, E(num(w), blit(w)), w.i32), "C1", "a signature that is not a `fn` type");
  bad(call_args(w.i32, E(num(w)), sig), "C1", "too few arguments");
  bad(call_args(w.i32, E(num(w), blit(w), num(w)), sig), "C1", "too many arguments");
  bad(call_args(w.i32, E(blit(w), num(w)), sig), "C1", "arguments in the wrong order");
  bad(call_args(w.i32, E(ilit(w.i64), blit(w)), sig), "C1", "an argument of another type");
  bad(call_args(w.i64, E(num(w), blit(w)), sig), "C1", "a result type other than the signature's");
}

auto test_c2_function_callee() -> void {
  begin("C2");
  auto w = world{};
  // def add1(x: int32) -> int32 { x }   and   def get(self: &pair) -> int32 { 0 }
  const auto make_add1 = [&] {
    const auto x = w.fresh();
    auto f = make_fn("add1", {prm(x, w.i32)}, w.i32, blk(w.i32, {}, use_of(w, pl(x, w.i32))));
    f->decl = &w.add_decl;
    return f;
  };
  const auto make_get = [&](receiver_mode r) {
    const auto s = w.fresh();
    auto f = make_fn("get", {prm(s, w.ref_pair)}, w.i32, blk(w.i32, {}, num(w)));
    f->decl = &w.method_decl;
    f->receiver = r;
    return f;
  };
  const auto caller = [&](callee target, ptr_vec<expr> args, tid result) {
    return make_fn("caller", w.std_params(), w.unit,
                   blk(w.unit, S(ex(call_of(result, std::move(target), std::move(args))))));
  };
  const auto add1_sig = sig_of(w, {w.i32}, w.i32);
  const auto get_sig = sig_of(w, {w.ref_pair}, w.i32);
  const auto run2 = [&](ptr<function> callee_fn, ptr<function> caller_fn) {
    auto fs = std::vector<ptr<function>>{};
    fs.push_back(std::move(callee_fn));
    fs.push_back(std::move(caller_fn));
    return verify_all(w, std::move(fs));
  };

  ok(run2(make_add1(), caller(fn_callee(&w.add_decl, add1_sig), E(num(w)), w.i32)),
     "a function callee whose signature is the core function's");
  ok(run2(make_get(receiver_mode::shared),
          caller(fn_callee(&w.method_decl, get_sig, "get", receiver_mode::shared),
                 E(borrow_of(w, loc(w, sym::p))), w.i32)),
     "a method callee whose receiver mode is the core function's");
  bad(run2(make_add1(), caller(fn_callee(nullptr, add1_sig), E(num(w)), w.i32)), "C2",
      "a function callee with no declaration");
  bad(run2(make_add1(), caller(fn_callee(&w.add_decl, sig_of(w, {w.i64}, w.i32)),
                               E(ilit(w.i64)), w.i32)),
      "C2", "a callee whose parameter types differ from the core function's");
  bad(run2(make_add1(), caller(fn_callee(&w.add_decl, sig_of(w, {w.i32}, w.i64)), E(num(w)),
                               w.i64)),
      "C2", "a callee whose result type differs from the core function's");
  bad(run2(make_add1(), caller(fn_callee(&w.add_decl, sig_of(w, {w.i32, w.i32}, w.i32)),
                               E(num(w), num(w)), w.i32)),
      "C2", "a callee with a different parameter count");
  bad(run2(make_get(receiver_mode::shared),
           caller(fn_callee(&w.method_decl, get_sig, "get", receiver_mode::none),
                  E(borrow_of(w, loc(w, sym::p))), w.i32)),
      "C2", "a callee whose receiver mode differs from the core function's");
  // A declaration the program holds no function for is not checked further.
  ok(verify_one(w, caller(fn_callee(&w.add_decl, add1_sig), E(num(w)), w.i32)),
     "a declaration the program holds no core function for");
  {
    auto c = fn_callee(&w.add_decl, add1_sig);
    c.value = use_of(w, loc(w, sym::fnv));
    bad(verify_one(w, caller(std::move(c), E(num(w)), w.i32)), "C2",
        "a function callee that also holds a value");
  }
}

auto test_c3_receiver_modes() -> void {
  begin("C3");
  auto w = world{};
  const auto run = [&](receiver_mode mode, tid first) {
    const auto sig = sig_of(w, {first}, w.unit);
    return run_std(w, S(ex(call_of(w.unit, intrinsic(sig, "m", mode), E(arg_for(w, first))))));
  };
  // shared: `&S`
  ok(run(receiver_mode::shared, w.ref_pair), "shared receiver with `&S`");
  bad(run(receiver_mode::shared, w.mut_ref_pair), "C3", "shared receiver with `&mut S`");
  bad(run(receiver_mode::shared, w.pair), "C3", "shared receiver with `S`");
  // exclusive: `&mut S`
  ok(run(receiver_mode::exclusive, w.mut_ref_pair), "exclusive receiver with `&mut S`");
  bad(run(receiver_mode::exclusive, w.ref_pair), "C3", "exclusive receiver with `&S`");
  bad(run(receiver_mode::exclusive, w.pair), "C3", "exclusive receiver with `S`");
  // by_value: `S` with `S` copy
  ok(run(receiver_mode::by_value, w.i32), "by-value receiver with a `copy` type");
  bad(run(receiver_mode::by_value, w.pair), "C3", "by-value receiver with a type that is not `copy`");
  bad(run(receiver_mode::by_value, w.ref_i32), "C3", "by-value receiver with a reference");
  // consuming: `S`
  ok(run(receiver_mode::consuming, w.pair), "consuming receiver with `S`");
  bad(run(receiver_mode::consuming, w.ref_pair), "C3", "consuming receiver with `&S`");
  bad(run(receiver_mode::consuming, w.mut_ref_pair), "C3", "consuming receiver with `&mut S`");
  // none: no constraint.
  ok(run(receiver_mode::none, w.ref_pair), "no receiver mode over a reference");
  ok(run(receiver_mode::none, w.pair), "no receiver mode over a value");
  // A receiver mode needs a first parameter.
  bad(run_std(w, S(ex(call_of(w.unit, intrinsic(sig_of(w, {}, w.unit), "m", receiver_mode::shared),
                              E())))),
      "C3", "a receiver mode with no parameters");
}

auto test_c4_value_callee() -> void {
  begin("C4");
  auto w = world{};
  ok(run_std(w, S(ex(call_of(w.i32, value_callee(w.fn_i32, use_of(w, loc(w, sym::fnv))),
                             E(num(w)))))),
     "a value callee whose value has the signature's type");
  ok(run_std(w, S(ex(call_of(w.i32, value_callee(w.fn_i32, use_of(w, loc(w, sym::rfn))),
                             E(num(w)))))),
     "a value callee whose value is a `&` of the signature's type");
  bad(run_std(w, S(ex(call_of(w.i32, value_callee(w.fn_i32, use_of(w, loc(w, sym::rmfn))),
                             E(num(w)))))),
      "C4", "a value callee whose value is a `&mut` of the signature's type");
  bad(run_std(w, S(ex(call_of(w.i32, value_callee(w.fn_i32, nullptr), E(num(w)))))), "C4",
      "a value callee with no value");
  bad(run_std(w, S(ex(call_of(w.i32, value_callee(sig_of(w, {w.i32}, w.i32), use_of(w, loc(w, sym::task))),
                             E(num(w)))))),
      "C4", "a value callee whose value is not of the signature's type");
  {
    auto c = value_callee(w.fn_i32, use_of(w, loc(w, sym::fnv)));
    c.decl = &w.add_decl;
    bad(run_std(w, S(ex(call_of(w.i32, std::move(c), E(num(w)))))), "C4",
        "a value callee that has a declaration");
  }
}

auto test_c5_intrinsic_callee() -> void {
  begin("C5");
  auto w = world{};
  const auto sig = sig_of(w, {w.i32}, w.i32);
  const auto run = [&](callee c) {
    return run_std(w, S(ex(call_of(w.i32, std::move(c), E(num(w))))));
  };
  ok(run(intrinsic(sig, "abs")), "an intrinsic with a name and nothing else");
  bad(run(intrinsic(sig, "")), "C5", "an intrinsic with no name");
  {
    auto c = intrinsic(sig, "abs");
    c.decl = &w.add_decl;
    bad(run(std::move(c)), "C5", "an intrinsic with a declaration");
  }
  {
    auto c = intrinsic(sig, "abs");
    c.value = use_of(w, loc(w, sym::fnv));
    bad(run(std::move(c)), "C5", "an intrinsic with a value");
  }
}

/// A function over parameters of the given types, calling the intrinsic `name`
/// (not declared by the program) on them. Each argument reads its parameter,
/// so the call differs from a well-formed one only in its signature.
auto op_call(world &w, std::string name, std::vector<tid> params, tid result,
             std::vector<tid> generics = {}) -> std::vector<violation> {
  auto ps = std::vector<param>{};
  auto args = ptr_vec<expr>{};
  for (const auto t : params) {
    const auto s = w.fresh();
    ps.push_back(prm(s, t));
    args.push_back(use_of(w, pl(s, t)));
  }
  auto c = raw_intrinsic(sig_of(w, params, result), std::move(name));
  c.generic_args = std::move(generics);
  return run_fn(w, std::move(ps), w.unit, S(ex(call_of(result, std::move(c), std::move(args)))));
}

auto test_c5_compiler_operations() -> void {
  begin("C5");
  auto w = world{};
  auto &t = w.checked.types;
  const auto i32 = w.i32, usize = w.usize, boolean = w.boolean, unit = w.unit, str = w.str;
  const auto u8 = t.builtin("uint8");
  const auto list = w.list_i32, arr = w.arr, slice = w.slice, slice_mut = w.slice_mut;
  const auto raw = w.raw_i32, raw_mut = w.raw_mut_i32;
  const auto ref_list = t.ref_to(list, false), mut_ref_list = t.ref_to(list, true);
  const auto ref_str = t.ref_to(str, false), ref_arr = t.ref_to(arr, false);
  const auto cell = t.builtin_generic("cell", {i32}), cell_mut = t.builtin_generic("cell_mut", {i32});
  const auto mut_ref_i32 = t.ref_to(i32, true);
  const auto gen = t.builtin_generic("generator", {i32}), gen_unit = t.builtin_generic("generator", {unit});
  const auto mut_ref_gen = t.ref_to(gen, true), ref_gen = t.ref_to(gen, false);
  const auto un_bool = t.builtin_generic("uninit", {boolean, t.const_value(usize, 4)});
  const auto fn0_i32 = t.fn_of({}, i32), fn1_i32 = t.fn_of({i32}, i32);
  const auto fn0_res = t.fn_of({}, w.result_i32_str);
  const auto task_bool = t.builtin_generic("task", {boolean, w.never});
  const auto crew = t.builtin_generic("crew", {});
  const auto ref_crew = t.ref_to(crew, false), ref_task = t.ref_to(w.task_i32, false);
  const auto ref_task_err = t.ref_to(w.task_err, false);
  const auto tuple_ok = t.tuple_of({i32, w.result_i32_str}), tuple_bad = t.tuple_of({i32, i32});
  const auto list_str = t.builtin_generic("list", {str});

  // ptr_offset
  ok(op_call(w, "ptr_offset", {raw, usize}, raw, {i32}), "ptr_offset: a pointer and a `usize`");
  bad(op_call(w, "ptr_offset", {raw}, raw, {i32}), "C5", "ptr_offset: one operand");
  bad(op_call(w, "ptr_offset", {i32, usize}, i32, {i32}), "C5", "ptr_offset: a non-pointer base");
  bad(op_call(w, "ptr_offset", {raw, i32}, raw, {i32}), "C5", "ptr_offset: a non-`usize` offset");
  bad(op_call(w, "ptr_offset", {raw, usize}, raw_mut, {i32}), "C5",
      "ptr_offset: a result that is not the operand's type");
  bad(op_call(w, "ptr_offset", {raw, usize}, raw, {boolean}), "C5",
      "ptr_offset: a generic argument that is not the pointee");
  bad(op_call(w, "ptr_offset", {raw, usize}, raw, {}), "C5", "ptr_offset: no generic argument");

  // range_view
  ok(op_call(w, "range_view", {ref_list, usize, usize}, slice), "range_view: `&list` to a slice");
  ok(op_call(w, "range_view", {mut_ref_list, usize, usize}, slice_mut),
     "range_view: `&mut list` to a `slice_mut`");
  ok(op_call(w, "range_view", {ref_str, usize, usize}, str), "range_view: `&str` to `str`");
  ok(op_call(w, "range_view", {slice, i32, i32}, slice), "range_view: a slice by value");
  ok(op_call(w, "range_view", {ref_arr, usize, usize}, slice), "range_view: `&array` to a slice");
  bad(op_call(w, "range_view", {ref_list, usize}, slice), "C5", "range_view: two operands");
  bad(op_call(w, "range_view", {i32, usize, usize}, slice), "C5", "range_view: a non-container");
  bad(op_call(w, "range_view", {list, usize, usize}, slice), "C5",
      "range_view: a list by value");
  bad(op_call(w, "range_view", {ref_list, boolean, usize}, slice), "C5",
      "range_view: a non-integer bound");
  ok(op_call(w, "range_view_inclusive", {ref_list, usize, usize}, slice),
     "range_view_inclusive: `&list` to a slice");
  ok(op_call(w, "range_view_inclusive", {ref_str, usize, usize}, str),
     "range_view_inclusive: `&str` to `str`");
  bad(op_call(w, "range_view_inclusive", {list, usize, usize}, slice), "C5",
      "range_view_inclusive: a list by value");
  bad(op_call(w, "range_view_inclusive", {ref_list, boolean, usize}, slice),
      "C5", "range_view_inclusive: a non-integer bound");
  bad(op_call(w, "range_view", {ref_list, usize, usize}, slice_mut), "C5",
      "range_view: a `slice_mut` from a shared borrow");
  bad(op_call(w, "range_view", {ref_list, usize, usize}, t.builtin_generic("slice", {boolean})),
      "C5", "range_view: a slice of another element type");
  bad(op_call(w, "range_view", {ref_str, usize, usize}, slice), "C5",
      "range_view: a slice from a `str`");
  bad(op_call(w, "range_view", {ref_list, usize, usize}, i32), "C5",
      "range_view: a result that is not a view");

  // len
  ok(op_call(w, "len", {ref_list}, usize), "len: `&list`");
  ok(op_call(w, "len", {ref_str}, usize), "len: `&str`");
  ok(op_call(w, "len", {ref_arr}, usize), "len: `&array`");
  ok(op_call(w, "len", {slice}, usize), "len: a slice by value");
  bad(op_call(w, "len", {ref_list}, i32), "C5", "len: a result that is not `usize`");
  bad(op_call(w, "len", {i32}, usize), "C5", "len: a non-container");
  bad(op_call(w, "len", {list}, usize), "C5", "len: a list by value");
  bad(op_call(w, "len", {}, usize), "C5", "len: no operand");

  // container_data
  ok(op_call(w, "container_data", {ref_list}, raw), "container_data: `&list` to `*T`");
  ok(op_call(w, "container_data", {mut_ref_list}, raw_mut), "container_data: `&mut list` to `*mut T`");
  ok(op_call(w, "container_data", {slice}, raw), "container_data: a slice by value");
  ok(op_call(w, "container_data", {ref_str}, t.ptr_to(u8, false)), "container_data: `&str`");
  ok(op_call(w, "container_data", {t.ref_to(w.uninit_i32, false)}, raw), "container_data: `&uninit`");
  ok(op_call(w, "container_data", {t.ref_to(w.uninit_i32, true)}, raw_mut),
     "container_data: `&mut uninit` to `*mut T`");
  bad(op_call(w, "container_data", {t.ref_to(w.uninit_i32, false)}, raw_mut), "C5",
      "container_data: `*mut T` from a shared `&uninit`");
  bad(op_call(w, "container_data", {t.ref_to(w.uninit_i32, false)}, w.raw_bool), "C5",
      "container_data: an `&uninit` buffer's pointer to another element type");
  bad(op_call(w, "container_data", {w.uninit_i32}, raw), "C5", "container_data: a buffer by value");
  bad(op_call(w, "container_data", {ref_list}, raw_mut), "C5",
      "container_data: `*mut T` from a shared borrow");
  bad(op_call(w, "container_data", {ref_list}, w.raw_bool), "C5",
      "container_data: a pointer to another element type");
  bad(op_call(w, "container_data", {ref_list}, i32), "C5", "container_data: a non-pointer result");
  bad(op_call(w, "container_data", {i32}, raw), "C5", "container_data: a non-container");

  // as_bytes
  ok(op_call(w, "as_bytes", {str}, t.builtin_generic("slice", {u8})), "as_bytes: `str`");
  bad(op_call(w, "as_bytes", {i32}, t.builtin_generic("slice", {u8})), "C5", "as_bytes: a non-`str`");
  bad(op_call(w, "as_bytes", {str}, slice), "C5", "as_bytes: `slice[int32]`");
  bad(op_call(w, "as_bytes", {str, str}, t.builtin_generic("slice", {u8})), "C5",
      "as_bytes: two operands");

  // generator_next
  ok(op_call(w, "generator_next", {mut_ref_gen}, w.option_i32), "generator_next: `&mut generator`");
  bad(op_call(w, "generator_next", {ref_gen}, w.option_i32), "C5", "generator_next: a shared borrow");
  bad(op_call(w, "generator_next", {gen}, w.option_i32), "C5", "generator_next: a generator by value");
  bad(op_call(w, "generator_next", {mut_ref_gen}, t.builtin_generic("option", {boolean})), "C5",
      "generator_next: an option of another item type");
  bad(op_call(w, "generator_next", {mut_ref_gen}, i32), "C5", "generator_next: a non-option result");

  // cell_get, cell_set
  ok(op_call(w, "cell_get", {cell}, i32), "cell_get: `cell`");
  ok(op_call(w, "cell_get", {cell_mut}, i32), "cell_get: `cell_mut`");
  bad(op_call(w, "cell_get", {cell}, boolean), "C5", "cell_get: another element type");
  bad(op_call(w, "cell_get", {i32}, i32), "C5", "cell_get: a non-cell");
  ok(op_call(w, "cell_set", {cell_mut, i32}, unit), "cell_set: `cell_mut` and a value");
  bad(op_call(w, "cell_set", {cell, i32}, unit), "C5", "cell_set: a shared cell");
  bad(op_call(w, "cell_set", {cell_mut, boolean}, unit), "C5", "cell_set: a value of another type");
  bad(op_call(w, "cell_set", {cell_mut, i32}, i32), "C5", "cell_set: a result that is not `unit`");

  // The four cell conversions.
  ok(op_call(w, "cell_to_ref", {cell}, w.ref_i32, {i32}), "cell_to_ref: `cell[T]` to `&T`");
  bad(op_call(w, "cell_to_ref", {cell}, mut_ref_i32, {i32}), "C5", "cell_to_ref: a `&mut` result");
  bad(op_call(w, "cell_to_ref", {cell_mut}, w.ref_i32, {i32}), "C5", "cell_to_ref: a `cell_mut`");
  bad(op_call(w, "cell_to_ref", {cell}, w.ref_i32, {boolean}), "C5",
      "cell_to_ref: a generic argument that is not the element");
  bad(op_call(w, "cell_to_ref", {cell}, w.ref_i32), "C5", "cell_to_ref: no generic argument");
  ok(op_call(w, "cell_mut_to_ref_mut", {cell_mut}, mut_ref_i32, {i32}),
     "cell_mut_to_ref_mut: `cell_mut[T]` to `&mut T`");
  bad(op_call(w, "cell_mut_to_ref_mut", {cell_mut}, w.ref_i32, {i32}), "C5",
      "cell_mut_to_ref_mut: a shared result");
  ok(op_call(w, "ref_to_cell", {w.ref_i32}, cell, {i32}), "ref_to_cell: `&T` to `cell[T]`");
  bad(op_call(w, "ref_to_cell", {mut_ref_i32}, cell, {i32}), "C5", "ref_to_cell: a `&mut` operand");
  bad(op_call(w, "ref_to_cell", {w.ref_i32}, cell_mut, {i32}), "C5", "ref_to_cell: a `cell_mut` result");
  ok(op_call(w, "ref_mut_to_cell_mut", {mut_ref_i32}, cell_mut, {i32}),
     "ref_mut_to_cell_mut: `&mut T` to `cell_mut[T]`");
  bad(op_call(w, "ref_mut_to_cell_mut", {mut_ref_i32}, cell, {i32}), "C5",
      "ref_mut_to_cell_mut: a shared cell result");

  // uninit_len, stack_buffer
  ok(op_call(w, "uninit_len", {}, usize, {w.uninit_i32}), "uninit_len: a buffer type");
  bad(op_call(w, "uninit_len", {}, usize, {i32}), "C5", "uninit_len: a generic argument that is not a buffer");
  bad(op_call(w, "uninit_len", {}, i32, {w.uninit_i32}), "C5", "uninit_len: a result that is not `usize`");
  bad(op_call(w, "uninit_len", {w.uninit_i32}, usize, {w.uninit_i32}), "C5", "uninit_len: an operand");
  ok(op_call(w, "stack_buffer", {}, w.uninit_i32, {w.uninit_i32}), "stack_buffer: its own type");
  bad(op_call(w, "stack_buffer", {}, un_bool, {w.uninit_i32}), "C5",
      "stack_buffer: a result that is not the generic argument");
  bad(op_call(w, "stack_buffer", {}, i32, {i32}), "C5", "stack_buffer: a non-buffer type");

  // ptr_cast, slice_from_raw_parts
  ok(op_call(w, "ptr_cast", {raw}, w.raw_bool, {boolean}), "ptr_cast: to another pointee");
  ok(op_call(w, "ptr_cast", {raw}, raw_mut, {i32}), "ptr_cast: to a `*mut`");
  ok(op_call(w, "ptr_cast", {t.ref_to(i32, true)}, raw_mut, {i32}), "ptr_cast: `&mut T` operand");
  ok(op_call(w, "ptr_cast", {t.ref_to(i32, false)}, w.raw_bool, {boolean}), "ptr_cast: `&T` operand");
  bad(op_call(w, "ptr_cast", {t.ref_to(i32, true)}, raw_mut, {boolean}), "C5",
      "ptr_cast: a reference operand with the wrong generic argument");
  bad(op_call(w, "ptr_cast", {raw}, w.raw_bool, {i32}), "C5", "ptr_cast: a generic argument that is not the target");
  bad(op_call(w, "ptr_cast", {i32}, w.raw_bool, {boolean}), "C5", "ptr_cast: a non-pointer operand");
  bad(op_call(w, "ptr_cast", {raw}, boolean, {boolean}), "C5", "ptr_cast: a non-pointer result");
  ok(op_call(w, "slice_from_raw_parts", {raw, usize}, slice), "slice_from_raw_parts: `*T`");
  ok(op_call(w, "slice_from_raw_parts", {raw_mut, usize}, slice_mut),
     "slice_from_raw_parts: `*mut T` to `slice_mut`");
  bad(op_call(w, "slice_from_raw_parts", {raw, usize}, slice_mut), "C5",
      "slice_from_raw_parts: `*T` to `slice_mut`");
  bad(op_call(w, "slice_from_raw_parts", {raw, usize}, t.builtin_generic("slice", {boolean})), "C5",
      "slice_from_raw_parts: another element type");
  bad(op_call(w, "slice_from_raw_parts", {raw, i32}, slice), "C5",
      "slice_from_raw_parts: a non-`usize` length");

  // size_of, align_of, needs_drop
  ok(op_call(w, "size_of", {}, usize, {i32}), "size_of");
  ok(op_call(w, "align_of", {}, usize, {i32}), "align_of");
  bad(op_call(w, "size_of", {}, i32, {i32}), "C5", "size_of: a result that is not `usize`");
  bad(op_call(w, "align_of", {}, usize), "C5", "align_of: no generic argument");
  bad(op_call(w, "size_of", {i32}, usize, {i32}), "C5", "size_of: an operand");
  ok(op_call(w, "needs_drop", {}, boolean, {list}), "needs_drop");
  bad(op_call(w, "needs_drop", {}, usize, {list}), "C5", "needs_drop: a result that is not `bool`");
  bad(op_call(w, "needs_drop", {}, boolean, {list, i32}), "C5", "needs_drop: two generic arguments");

  // bitwise_copy (todo 39)
  ok(op_call(w, "bitwise_copy", {ref_list}, list, {list}), "bitwise_copy: `&list` to `list`");
  bad(op_call(w, "bitwise_copy", {list}, list, {list}), "C5", "bitwise_copy: a value operand");
  bad(op_call(w, "bitwise_copy", {ref_list}, i32, {list}), "C5",
      "bitwise_copy: a result that is not the referent");
  bad(op_call(w, "bitwise_copy", {ref_list}, list), "C5", "bitwise_copy: no generic argument");

  // Strings.
  ok(op_call(w, "rt_str_concat", {str, str}, str), "rt_str_concat");
  bad(op_call(w, "rt_str_concat", {str, i32}, str), "C5", "rt_str_concat: a non-`str` operand");
  bad(op_call(w, "rt_str_concat", {str, str}, i32), "C5", "rt_str_concat: a non-`str` result");
  ok(op_call(w, "str_decode_scalar", {str, usize}, w.chr), "str_decode_scalar");
  bad(op_call(w, "str_decode_scalar", {str, usize}, usize), "C5", "str_decode_scalar: a result that is not `char`");
  ok(op_call(w, "str_scalar_width", {str, usize}, usize), "str_scalar_width");
  bad(op_call(w, "str_scalar_width", {str}, usize), "C5", "str_scalar_width: one operand");

  // Tasks.
  ok(op_call(w, "task_block", {fn0_i32}, w.task_i32), "task_block: an infallible task");
  ok(op_call(w, "task_block", {fn0_res}, w.task_err), "task_block: a fallible task awaits `result`");
  bad(op_call(w, "task_block", {fn1_i32}, w.task_i32), "C5", "task_block: a closure with a parameter");
  bad(op_call(w, "task_block", {fn0_i32}, task_bool), "C5", "task_block: a task of another type");
  bad(op_call(w, "task_block", {fn0_i32}, w.task_err), "C5",
      "task_block: a fallible task whose closure does not return `result`");
  bad(op_call(w, "task_block", {fn0_i32}, i32), "C5", "task_block: a non-task result");
  ok(op_call(w, "task_yield", {}, unit), "task_yield: `await yield`");
  bad(op_call(w, "task_yield", {i32}, unit), "C5", "task_yield: an operand");
  bad(op_call(w, "task_yield", {}, i32), "C5", "task_yield: a result that is not `unit`");
  ok(op_call(w, "on_cpu", {w.task_i32}, w.task_i32), "on_cpu");
  ok(op_call(w, "on_io", {w.task_err}, w.task_err), "on_io");
  bad(op_call(w, "on_cpu", {w.task_i32}, w.task_err), "C5", "on_cpu: another task type");
  bad(op_call(w, "on_io", {i32}, i32), "C5", "on_io: a non-task");
  ok(op_call(w, "await_par", {w.task_i32, w.task_err}, tuple_ok), "await_par: awaited types in order");
  bad(op_call(w, "await_par", {w.task_i32, w.task_err}, tuple_bad), "C5",
      "await_par: a fallible task's element is not `result`");
  bad(op_call(w, "await_par", {w.task_i32, w.task_err}, i32), "C5", "await_par: a non-tuple result");
  bad(op_call(w, "await_par", {w.task_i32}, tuple_ok), "C5", "await_par: a tuple of another length");
  bad(op_call(w, "await_par", {i32}, t.tuple_of({i32})), "C5", "await_par: a non-task operand");
  ok(op_call(w, "await_race", {w.task_i32, w.task_i32}, i32), "await_race: tasks awaiting one type");
  ok(op_call(w, "await_race", {w.task_err}, w.result_i32_str), "await_race: a fallible task");
  bad(op_call(w, "await_race", {w.task_i32, w.task_err}, i32), "C5",
      "await_race: tasks awaiting different types");
  bad(op_call(w, "await_race", {w.task_i32}, boolean), "C5", "await_race: a result no task awaits");
  bad(op_call(w, "await_race", {}, i32), "C5", "await_race: no tasks");

  // Crews.
  ok(op_call(w, "crew_new", {}, crew), "crew_new");
  bad(op_call(w, "crew_new", {}, unit), "C5", "crew_new: a result that is not a crew");
  ok(op_call(w, "crew_end", {crew}, unit), "crew_end");
  bad(op_call(w, "crew_end", {ref_crew}, unit), "C5", "crew_end: a borrowed crew");
  bad(op_call(w, "crew_end", {crew}, i32), "C5", "crew_end: a result that is not `unit`");
  ok(op_call(w, "task_spawn", {ref_task}, unit), "task_spawn");
  bad(op_call(w, "task_spawn", {w.task_i32}, unit), "C5", "task_spawn: a task by value");
  bad(op_call(w, "task_spawn", {ref_task}, i32), "C5", "task_spawn: a result that is not `unit`");
  ok(op_call(w, "crew_spawn", {ref_crew, w.task_i32}, unit), "crew_spawn");
  ok(op_call(w, "crew_spawn", {ref_crew, w.task_i32}, i32), "crew_spawn: any result is accepted");
  bad(op_call(w, "crew_spawn", {crew, w.task_i32}, unit), "C5", "crew_spawn: a crew by value");
  bad(op_call(w, "crew_spawn", {ref_crew, i32}, unit), "C5", "crew_spawn: a non-task");
  bad(op_call(w, "crew_spawn", {ref_crew}, unit), "C5", "crew_spawn: one operand");
  ok(op_call(w, "task_get", {ref_task}, i32), "task_get");
  ok(op_call(w, "task_get", {ref_task_err}, w.result_i32_str), "task_get: a fallible task");
  bad(op_call(w, "task_get", {ref_task}, boolean), "C5", "task_get: a result the task does not await");
  bad(op_call(w, "task_get", {ref_task_err}, i32), "C5", "task_get: a fallible task's bare value");
  // `crew_errors[task](&crew, new, push)`.
  const auto errors = [&](tid crew_operand, tid result, tid task, tid made, tid push) {
    return op_call(w, "crew_errors", {crew_operand, made, push}, result, {task});
  };
  const auto new_of = [&](tid result) { return t.fn_of({}, result); };
  const auto push_of = [&](tid result, tid element) {
    return t.fn_of({t.ref_to(result, true), element}, unit);
  };
  ok(errors(ref_crew, list_str, w.task_err, new_of(list_str), push_of(list_str, str)),
     "crew_errors");
  bad(errors(ref_crew, i32, w.task_err, new_of(i32), push_of(i32, str)), "C5",
      "crew_errors: a non-list result");
  bad(op_call(w, "crew_errors", {ref_crew}, list_str, {w.task_err}), "C5",
      "crew_errors: no `new` and `push`");
  bad(errors(ref_crew, list_str, w.task_i32, new_of(list_str), push_of(list_str, str)), "C5",
      "crew_errors: an infallible task");
  bad(op_call(w, "crew_errors", {ref_crew, new_of(list_str), push_of(list_str, str)}, list_str),
      "C5", "crew_errors: no task type");
  bad(errors(ref_crew, list_str, w.task_err, new_of(list), push_of(list_str, str)), "C5",
      "crew_errors: a `new` making another list");
  bad(errors(ref_crew, list_str, w.task_err, new_of(list_str), push_of(list_str, i32)), "C5",
      "crew_errors: a `push` of another element");
  {
    auto list_decl = ast::type_decl{};
    list_decl.name = "list";
    list_decl.definition = ast::make<ast::struct_type_def>();
    auto other_decl = ast::type_decl{};
    other_decl.name = "other";
    other_decl.definition = ast::make<ast::struct_type_def>();
    const auto std_list = t.user_type(list_decl, "std.list", {str});
    ok(errors(ref_crew, std_list, w.task_err, new_of(std_list), push_of(std_list, str)),
       "crew_errors: the standard library's `list[E]` struct");
    auto user_list_decl = ast::type_decl{};
    user_list_decl.name = "list";
    user_list_decl.definition = ast::make<ast::struct_type_def>();
    const auto user_list = t.user_type(user_list_decl, "m", {str});
    bad(errors(ref_crew, user_list, w.task_err, new_of(user_list), push_of(user_list, str)),
        "C5", "crew_errors: a `list` struct of another module");
    const auto other = t.user_type(other_decl, "std.list", {str});
    bad(errors(ref_crew, other, w.task_err, new_of(other), push_of(other, str)), "C5",
        "crew_errors: a struct of `std.list` that is not `list`");
    const auto bare = t.user_type(list_decl, "std.list", {});
    bad(errors(ref_crew, bare, w.task_err, new_of(bare), push_of(bare, str)), "C5",
        "crew_errors: a `list` struct with no element type");
  }
  bad(errors(crew, list_str, w.task_err, new_of(list_str), push_of(list_str, str)), "C5",
      "crew_errors: a crew by value");
  (void)gen_unit;
}

auto test_c5_declared_intrinsics() -> void {
  begin("C5");
  auto w = world{};
  const auto call_with = [&](tid sig, std::string name) {
    auto args = ptr_vec<expr>{};
    for (std::size_t k = 0; k < w.checked.types.entry(sig).args.size(); ++k) {
      args.push_back(ilit(w.i32));
    }
    const auto result = w.checked.types.entry(sig).result;
    return run_std(w, S(ex(call_of(result, raw_intrinsic(sig, std::move(name)), std::move(args)))));
  };
  const auto fn1 = sig_of(w, {w.i32}, w.i32);
  const auto fn2 = sig_of(w, {w.i32, w.i32}, w.i32);
  const auto declare = [&](std::string name, tid sig, bool generic) {
    g_decls.push_back(intrinsic_decl{.name = std::move(name), .signature = sig, .generic = generic});
  };
  declare("rt_abs", fn1, false);
  ok(call_with(fn1, "rt_abs"), "a declared intrinsic with its exact signature");
  declare("rt_abs", fn1, false);
  bad(call_with(fn2, "rt_abs"), "C5", "a declared intrinsic with another signature");
  declare("rt_abs", fn1, false);
  bad(call_with(sig_of(w, {w.i32}, w.i64), "rt_abs"), "C5",
      "a declared intrinsic with another result");
  // A generic declaration: any `fn` type with as many parameters.
  declare("rt_id", sig_of(w, {w.typ_t}, w.typ_t), true);
  ok(call_with(fn1, "rt_id"), "a generic declaration with an instance of its arity");
  declare("rt_id", sig_of(w, {w.typ_t}, w.typ_t), true);
  bad(call_with(fn2, "rt_id"), "C5", "a generic declaration with another number of parameters");
  declare("rt_id", sig_of(w, {w.typ_t}, w.typ_t), false);
  bad(call_with(fn1, "rt_id"), "C5", "a declaration that is not generic needs the exact signature");
  bad(call_with(fn1, "no_such_operation"), "C5", "a name that is neither an operation nor declared");
  bad(call_with(fn1, "len"), "C5", "a compiler operation with the wrong shape is not rescued by a name");
  // A declaration with the name of a compiler operation does not replace it.
  declare("len", fn1, false);
  bad(call_with(fn1, "len"), "C5", "a declaration named like a compiler operation");

  // The name may be declared by any module of the program.
  {
    auto first = module{};
    first.name = "m";
    first.intrinsics.push_back(intrinsic_decl{.name = "rt_two", .signature = fn2, .generic = false});
    auto second = module{};
    second.name = "n";
    second.intrinsics.push_back(intrinsic_decl{.name = "rt_two", .signature = fn1, .generic = false});
    auto caller = module{};
    caller.name = "caller";
    const auto build = [&](tid sig) {
      auto m = module{};
      m.name = "c";
      auto args = ptr_vec<expr>{};
      for (std::size_t k = 0; k < w.checked.types.entry(sig).args.size(); ++k) {
        args.push_back(ilit(w.i32));
      }
      m.functions.push_back(make_fn(
          "f", {}, w.unit,
          blk(w.unit, S(ex(call_of(w.i32, raw_intrinsic(sig, "rt_two"), std::move(args)))))));
      return m;
    };
    const auto run_all = [&](tid sig) {
      const auto user = build(sig);
      const std::array<const module *, 3> program{&first, &second, &user};
      return verify(std::span<const module *const>(program), w.checked);
    };
    ok(run_all(fn1), "a name declared by two modules matches the second");
    ok(run_all(fn2), "a name declared by two modules matches the first");
    bad(run_all(sig_of(w, {}, w.i32)), "C5", "a name declared by two modules matches neither");
  }
}

auto test_c6_generic_arguments() -> void {
  begin("C6");
  auto w = world{};
  const auto T = w.typ_t;
  // def ident[T](x: T) -> T { x }   and its instance for int32.
  const auto build = [&](tid arg, bool with_count) {
    const auto x = w.fresh();
    auto tmpl = make_fn("ident", {prm(x, T)}, T, blk(T, {}, use_of(w, pl(x, T))));
    tmpl->decl = &w.generic_decl;
    tmpl->generic_params = {T};
    const auto y = w.fresh();
    auto inst = make_fn("ident_i32", {prm(y, w.i32)}, w.i32, blk(w.i32, {}, use_of(w, pl(y, w.i32))));
    inst->decl = &w.instance_decl;
    inst->instance_of = tmpl.get();
    auto c = fn_callee(&w.instance_decl, sig_of(w, {w.i32}, w.i32), "ident_i32");
    c.instance_of = &w.generic_decl;
    if (with_count) {
      c.generic_args = {arg};
    }
    auto caller = make_fn("caller", w.std_params(), w.unit,
                          blk(w.unit, S(ex(call_of(w.i32, std::move(c), E(num(w)))))));
    auto fs = std::vector<ptr<function>>{};
    fs.push_back(std::move(tmpl));
    fs.push_back(std::move(inst));
    fs.push_back(std::move(caller));
    return verify_all(w, std::move(fs));
  };
  ok(build(w.i32, true), "an instance call with settled generic arguments");
  bad(build(sem::k_unknown_type, true), "C6", "an unsettled generic argument");
  bad(build(w.checked.types.fresh_type_var(), true), "C6", "an inference variable as an argument");
  bad(build(w.checked.types.refinement_of(nullptr, "pos", "m", w.i32, {}, nullptr), true), "C6",
      "a refinement as a generic argument");
}

auto test_c8_generic_argument_count() -> void {
  begin("C8");
  auto w = world{};
  const auto T = w.typ_t;
  const auto build = [&](std::vector<tid> args) {
    const auto x = w.fresh();
    auto tmpl = make_fn("ident", {prm(x, T)}, T, blk(T, {}, use_of(w, pl(x, T))));
    tmpl->decl = &w.generic_decl;
    tmpl->generic_params = {T};
    const auto y = w.fresh();
    auto inst = make_fn("ident_i32", {prm(y, w.i32)}, w.i32, blk(w.i32, {}, use_of(w, pl(y, w.i32))));
    inst->decl = &w.instance_decl;
    inst->instance_of = tmpl.get();
    auto c = fn_callee(&w.instance_decl, sig_of(w, {w.i32}, w.i32), "ident_i32");
    c.instance_of = &w.generic_decl;
    c.generic_args = std::move(args);
    auto caller = make_fn("caller", w.std_params(), w.unit,
                          blk(w.unit, S(ex(call_of(w.i32, std::move(c), E(num(w)))))));
    auto fs = std::vector<ptr<function>>{};
    fs.push_back(std::move(tmpl));
    fs.push_back(std::move(inst));
    fs.push_back(std::move(caller));
    return verify_all(w, std::move(fs));
  };
  ok(build({w.i32}), "an instance call with as many generic arguments as the template has");
  bad(build({}), "C8", "an instance call with too few generic arguments");
  bad(build({w.i32, w.i64}), "C8", "an instance call with too many generic arguments");
  // With no template in the program the count is not checked.
  {
    auto c = intrinsic(sig_of(w, {w.i32}, w.i32));
    c.generic_args = {w.i32, w.i64};
    ok(run_std(w, S(ex(call_of(w.i32, std::move(c), E(num(w)))))),
       "generic arguments with no template in the program");
  }
}

auto test_c7_function_ref() -> void {
  begin("C7");
  auto w = world{};
  const auto sig = sig_of(w, {w.i32}, w.i32);
  ok(run_std(w, S(ex(fnref_of(sig, fn_callee(&w.add_decl, sig))))),
     "a function_ref with a function callee and the signature's type");
  bad(run_std(w, S(ex(fnref_of(sig_of(w, {w.i64}, w.i32), fn_callee(&w.add_decl, sig))))), "C7",
      "a function_ref whose type is not the signature");
  bad(run_std(w, S(ex(fnref_of(w.i32, fn_callee(&w.add_decl, w.i32))))), "C7",
      "a function_ref whose signature is not a `fn` type");
  bad(run_std(w, S(ex(fnref_of(sig, intrinsic(sig, "abs"))))), "C7",
      "a function_ref of an intrinsic");
  bad(run_std(w, S(ex(fnref_of(sig, value_callee(sig, use_of(w, loc(w, sym::fnv))))))), "C7",
      "a function_ref of a value callee");
}

// ===========================================================================
//  E: expressions
// ===========================================================================

auto test_e1_literals() -> void {
  begin("E1");
  auto w = world{};
  const auto lit_stmt = [&](tid t, token_kind k, std::string s) {
    return run_std(w, S(ex(make_lit(t, k, std::move(s)))));
  };
  ok(lit_stmt(w.i32, token_kind::int_lit, "1"), "an integer literal of an integer type");
  ok(lit_stmt(w.usize, token_kind::int_lit, "1"), "an integer literal of `usize`");
  ok(lit_stmt(w.f64, token_kind::float_lit, "1.5"), "a float literal of a float type");
  ok(lit_stmt(w.boolean, token_kind::kw_true, "true"), "`true`");
  ok(lit_stmt(w.boolean, token_kind::kw_false, "false"), "`false`");
  ok(lit_stmt(w.str, token_kind::string_lit, "\"s\""), "a string literal of `str`");
  ok(lit_stmt(w.chr, token_kind::char_lit, "'c'"), "a char literal of `char`");
  ok(lit_stmt(w.unit, token_kind::kw_unit, "()"), "a unit literal of `unit`");
  bad(lit_stmt(w.boolean, token_kind::int_lit, "1"), "E1", "an integer literal of `bool`");
  bad(lit_stmt(w.str, token_kind::int_lit, "1"), "E1", "an integer literal of `str`");
  bad(lit_stmt(w.i32, token_kind::float_lit, "1.5"), "E1", "a float literal of an integer type");
  bad(lit_stmt(w.i32, token_kind::kw_true, "true"), "E1", "`true` of an integer type");
  bad(lit_stmt(w.str, token_kind::kw_false, "false"), "E1", "`false` of `str`");
  bad(lit_stmt(w.i32, token_kind::string_lit, "\"s\""), "E1", "a string literal of an integer type");
  bad(lit_stmt(w.str, token_kind::char_lit, "'c'"), "E1", "a char literal of `str`");
  bad(lit_stmt(w.i32, token_kind::kw_unit, "()"), "E1", "a unit literal of an integer type");
  bad(lit_stmt(w.pair, token_kind::int_lit, "1"), "E1", "an integer literal of a struct");
}

auto test_e2_primitives() -> void {
  begin("E2");
  auto w = world{};
  using bop = ast::binary_op;
  using uop = ast::unary_op;
  const auto n = [&] { return use_of(w, loc(w, sym::n)); };
  const auto flag = [&] { return use_of(w, loc(w, sym::b)); };
  const auto run = [&](ptr<expr> e) { return run_std(w, S(ex(std::move(e)))); };

  // Arithmetic and bitwise operators.
  ok(run(prim_bin(bop::add, w.i32, n(), num(w))), "`+` of two operands of the result type");
  ok(run(prim_bin(bop::bit_xor, w.i32, n(), n())), "`^` of two operands of the result type");
  ok(run(prim_bin(bop::mul_wrap, w.i32, n(), n())), "`*%` of two operands of the result type");
  ok(run(prim_bin(bop::add, w.f64, use_of(w, loc(w, sym::fl)), use_of(w, loc(w, sym::fl)))),
     "`+` of floats");
  bad(run(prim_bin(bop::add, w.i32, n(), ilit(w.i64))), "E2", "`+` of two integer types");
  bad(run(prim_bin(bop::add, w.i64, n(), n())), "E2", "`+` whose result is not the operand type");
  bad(run(prim_bin(bop::bit_and, w.i32, n(), flag())), "E2", "`&` of an integer and a bool");
  // Shifts.
  ok(run(prim_bin(bop::shl, w.i32, n(), ilit(w.i64))), "a shift of an integer by another integer type");
  ok(run(prim_bin(bop::shr, w.i32, n(), n())), "a shift by the same type");
  bad(run(prim_bin(bop::shl, w.i64, n(), n())), "E2", "a shift whose result is not its first operand's type");
  bad(run(prim_bin(bop::shl, w.i32, n(), flag())), "E2", "a shift by a bool");
  bad(run(prim_bin(bop::shr, w.boolean, flag(), n())), "E2", "a shift of a bool");
  // Comparisons.
  ok(run(prim_bin(bop::lt, w.boolean, n(), num(w))), "`<` of two operands of one type");
  ok(run(prim_bin(bop::eq_eq, w.boolean, flag(), blit(w))), "`==` of bools");
  ok(run(prim_bin(bop::eq_eq, w.boolean, use_of(w, loc(w, sym::raw)), use_of(w, loc(w, sym::raw)))),
     "`==` of raw pointers");
  bad(run(prim_bin(bop::lt, w.boolean, n(), ilit(w.i64))), "E2", "`<` of two types");
  bad(run(prim_bin(bop::gt_eq, w.i32, n(), n())), "E2", "a comparison whose result is not `bool`");
  // Logical operators.
  ok(run(prim_bin(bop::logical_and, w.boolean, flag(), blit(w))), "`and` of bools");
  ok(run(prim_bin(bop::logical_or, w.boolean, flag(), blit(w))), "`or` of bools");
  ok(run(prim_un(uop::logical_not, w.boolean, flag())), "`not` of a bool");
  bad(run(prim_bin(bop::logical_and, w.boolean, n(), flag())), "E2", "`and` of an integer");
  bad(run(prim_bin(bop::logical_or, w.i32, flag(), flag())), "E2", "`or` giving an integer");
  bad(run(prim_un(uop::logical_not, w.boolean, n())), "E2", "`not` of an integer");
  bad(run(prim_un(uop::logical_not, w.i32, flag())), "E2", "`not` giving an integer");
  // Negation.
  ok(run(prim_un(uop::neg, w.i32, n())), "negation of the result type");
  bad(run(prim_un(uop::neg, w.i64, n())), "E2", "negation changing the type");
  ok(run(prim_un(uop::bit_not, w.i32, n())), "bitwise not of the result type");
  bad(run(prim_un(uop::bit_not, w.i64, n())), "E2", "bitwise not changing the type");
  // The operator and its operands.
  {
    auto none = std::make_unique<primitive>(k_span, w.i32);
    none->operands.push_back(n());
    bad(run(std::move(none)), "E2", "a primitive with no operator");
  }
  {
    auto both = std::make_unique<primitive>(k_span, w.i32);
    both->binary = bop::add;
    both->unary = uop::neg;
    both->operands.push_back(n());
    both->operands.push_back(n());
    bad(run(std::move(both)), "E2", "a primitive with two operators");
  }
  {
    auto one = prim_bin(bop::add, w.i32, n(), n());
    static_cast<primitive &>(*one).operands.pop_back();
    bad(run(std::move(one)), "E2", "a binary operator with one operand");
  }
  // Operands that are not scalars or raw pointers.
  bad(run(prim_bin(bop::eq_eq, w.boolean, use_of(w, loc(w, sym::p)), use_of(w, loc(w, sym::p)))),
      "E2", "`==` of structs");
  bad(run(prim_bin(bop::eq_eq, w.boolean, use_of(w, loc(w, sym::sh)), use_of(w, loc(w, sym::sh)))),
      "E2", "`==` of sums");
  bad(run(prim_bin(bop::eq_eq, w.boolean, use_of(w, loc(w, sym::tup)), use_of(w, loc(w, sym::tup)))),
      "E2", "`==` of tuples");
  bad(run(prim_bin(bop::eq_eq, w.boolean, use_of(w, loc(w, sym::arr)), use_of(w, loc(w, sym::arr)))),
      "E2", "`==` of arrays");
  bad(run(prim_bin(bop::eq_eq, w.boolean, use_of(w, loc(w, sym::st)), use_of(w, loc(w, sym::st)))),
      "E2", "`==` of `str`");
  bad(run(prim_bin(bop::eq_eq, w.boolean, use_of(w, loc(w, sym::sl)), use_of(w, loc(w, sym::sl)))),
      "E2", "`==` of slices");
  bad(run(prim_bin(bop::eq_eq, w.boolean, use_of(w, loc(w, sym::ri)), use_of(w, loc(w, sym::ri)))),
      "E2", "`==` of references");
}

auto test_e3_casts() -> void {
  begin("E3");
  auto w = world{};
  const auto run = [&](tid to, place from) {
    return run_std(w, S(ex(cast_to(to, use_of(w, std::move(from))))));
  };
  ok(run(w.f64, loc(w, sym::n)), "a cast between builtin scalars");
  ok(run(w.i64, loc(w, sym::b)), "a cast of a bool to an integer");
  ok(run(w.usize, loc(w, sym::raw)), "a cast of a raw pointer to `usize`");
  {
    const auto u = w.fresh();
    ok(run_fn(w, {prm(u, w.usize)}, w.unit, S(ex(cast_to(w.raw_pair, use_of(w, pl(u, w.usize)))))),
       "a cast of `usize` to a raw pointer");
    bad(run_fn(w, {prm(u, w.usize)}, w.unit, S(ex(cast_to(w.pair, use_of(w, pl(u, w.usize)))))),
        "E3", "a cast of `usize` to a struct");
  }
  ok(run(w.raw_i32, loc(w, sym::raw)), "a cast of a raw pointer to another raw pointer type");
  ok(run(w.raw_mut_pair, loc(w, sym::raw)), "a cast of `*T` to `*mut T`");
  ok(run(w.raw_bool, loc(w, sym::raw_mi)), "a cast of `*mut int32` to `*bool`");
  bad(run(w.ref_pair, loc(w, sym::raw)), "E3", "a cast of a raw pointer to a reference");
  bad(run(w.raw_i32, loc(w, sym::rp)), "E3", "a cast of a reference to a raw pointer");
  bad(run(w.raw_pair, loc(w, sym::n)), "E3", "a cast of `int32` to a raw pointer");
  bad(run(w.i32, loc(w, sym::raw)), "E3", "a cast of a raw pointer to `int32`");
  bad(run(w.i32, loc(w, sym::p)), "E3", "a cast of a struct to an integer");
  bad(run(w.pair, loc(w, sym::n)), "E3", "a cast of an integer to a struct");
  bad(run(w.i32, loc(w, sym::st)), "E3", "a cast of `str` to an integer");
  bad(run(w.i32, loc(w, sym::ri)), "E3", "a cast of a reference to an integer");
}

auto test_e4_tuple() -> void {
  begin("E4");
  auto w = world{};
  const auto run = [&](tid t, ptr_vec<expr> elements) {
    return run_std(w, S(ex(agg(node_kind::tuple_init, t, std::move(elements)))));
  };
  ok(run(w.tuple, E(num(w), blit(w))), "a tuple of one element per member");
  bad(run(w.tuple, E(num(w))), "E4", "a tuple with too few elements");
  bad(run(w.tuple, E(num(w), blit(w), num(w))), "E4", "a tuple with too many elements");
  bad(run(w.tuple, E(blit(w), num(w))), "E4", "a tuple with elements out of order");
  bad(run(w.i32, E(num(w))), "E4", "a tuple construction whose type is not a tuple");
}

auto test_e4_array() -> void {
  begin("E4");
  auto w = world{};
  const auto run = [&](tid t, ptr_vec<expr> elements, std::optional<std::uint64_t> repeat = {}) {
    return run_std(w, S(ex(agg(node_kind::array_init, t, std::move(elements), {}, repeat))));
  };
  ok(run(w.arr, E(num(w), num(w), num(w))), "an array of one element per slot");
  ok(run(w.arr, E(num(w)), 3), "a repeat array with one element and the array's length");
  bad(run(w.arr, E(num(w), num(w))), "E4", "an array with too few elements");
  bad(run(w.arr, E(num(w), num(w), num(w), num(w))), "E4", "an array with too many elements");
  bad(run(w.arr, E(num(w), num(w), blit(w))), "E4", "an array element of the wrong type");
  bad(run(w.arr, E(num(w)), 2), "E4", "a repeat count that is not the length");
  bad(run(w.arr, E(num(w), num(w), num(w)), 3), "E4", "a repeat array with several elements");
  bad(run(w.i32, E(num(w))), "E4", "an array construction whose type is not an array");
  bad(run(w.checked.types.array_of(w.i32, std::nullopt,
                                   w.checked.types.symbolic_value(w.usize, sem::poly_variable("n"))),
          E(num(w))),
      {"E4", "T3"}, "an array with no known length (also an unsettled symbolic length)");
}

auto test_e4_struct() -> void {
  begin("E4");
  auto w = world{};
  const auto run = [&](tid t, ptr_vec<expr> elements, std::vector<std::string> names,
                       std::optional<std::vector<std::size_t>> indices = std::nullopt) {
    return run_std(w, S(ex(agg(node_kind::struct_init, t, std::move(elements), std::move(names),
                               std::nullopt, std::move(indices)))));
  };
  using idx = std::vector<std::size_t>;
  ok(run(w.pair, E(num(w), blit(w)), {"a", "b"}), "a struct with fields in declaration order");
  ok(run(w.pair, E(blit(w), num(w)), {"b", "a"}, idx{1, 0}),
     "a struct with fields in source order, not declaration order");
  bad(run(w.pair, E(blit(w), num(w)), {"b", "a"}, idx{0, 1}), "E4",
      "names that disagree with the field indices");
  bad(run(w.pair, E(blit(w), num(w)), {"b", "a"}), {"E4"},
      "source-order names under declaration-order indices");
  bad(run(w.pair, E(num(w), num(w)), {"a", "a"}, idx{0, 0}), "E4", "a field initialized twice");
  bad(run(w.pair, E(num(w), blit(w)), {"a", "b"}, idx{0, 2}), "E4", "a field index out of range");
  bad(run(w.pair, E(num(w), blit(w)), {"a", "b"}, idx{0}), "E4", "too few field indices");
  bad(run(w.pair, E(num(w), blit(w)), {"a", "b"}, idx{0, 1, 1}), "E4", "too many field indices");
  bad(run(w.pair, E(num(w), blit(w)), {"b", "a"}, idx{1, 0}), "E4",
      "source-order elements typed by declaration order");
  bad(run(w.pair, E(num(w), blit(w)), {"a", "zzz"}), "E4", "a field name that is not the shape's");
  bad(run(w.pair, E(num(w), blit(w)), {"a"}), "E4", "too few field names");
  bad(run(w.pair, E(num(w)), {"a", "b"}), "E4", "too few elements");
  bad(run(w.pair, E(num(w), num(w)), {"a", "b"}), "E4", "an element of the wrong type");
  bad(run(w.i32, E(num(w)), {"a"}), "E4", "a struct construction whose type is not a struct");
  bad(run(w.shape, E(num(w)), {"a"}), "E4", "a struct construction of a sum");
}

auto test_e4_variant() -> void {
  begin("E4");
  auto w = world{};
  const auto run = [&](tid t, std::string name, std::size_t index, ptr_vec<expr> payload) {
    return run_std(w, S(ex(variant_of(t, std::move(name), index, std::move(payload)))));
  };
  ok(run(w.shape, "circle", 0, E(num(w))), "a variant with its payload");
  ok(run(w.shape, "empty", 1, E()), "a variant with no payload");
  ok(run(w.shape, "rect", 2, E(num(w), blit(w))), "a variant with two payloads");
  ok(run(w.option_i32, "some", 0, E(num(w))), "`option`'s `some`");
  ok(run(w.option_i32, "none", 1, E()), "`option`'s `none`");
  bad(run(w.shape, "circle", 0, E()), "E4", "a variant with too few payloads");
  bad(run(w.shape, "circle", 0, E(num(w), num(w))), "E4", "a variant with too many payloads");
  bad(run(w.shape, "circle", 0, E(blit(w))), "E4", "a payload of the wrong type");
  bad(run(w.shape, "rect", 2, E(blit(w), num(w))), "E4", "payloads out of order");
  bad(run(w.shape, "square", 0, E(num(w))), "E4", "a variant name that is not at its index");
  bad(run(w.shape, "circle", 9, E(num(w))), "E4", "a variant index out of range");
  bad(run(w.pair, "circle", 0, E(num(w))), "E4", "a variant construction of a struct");
}

auto test_e5_blocks() -> void {
  begin("E5");
  auto w = world{};
  const auto x = w.fresh();
  ok(run_std(w, S(ex(blk(w.unit)))), "an empty block of type `unit`");
  ok(run_std(w, S(mk_let(x, w.i32, blk(w.i32, S(ex(num(w))), num(w))))),
     "a block with the type of its result");
  bad(run_std(w, S(mk_let(x, w.i64, blk(w.i64, S(), num(w))))), "E5",
      "a block whose type is not its result's");
  bad(run_std(w, S(mk_let(x, w.i32, blk(w.i32, S(ex(num(w))), nullptr)))), "E5",
      "a block of type `int32` with no result");
  // A diverging block is not compared with its type.
  ok(verify_one(w, make_fn("f", {}, w.i32,
                           blk(w.i32, S(mk_let(x, w.i32, blk(w.str, S(ret(num(w))), nullptr))),
                               use_of(w, pl(x, w.i32))))),
     "a block that ends in `return` has any type");
  // Scope of a `let`.
  ok(run_std(w, S(mk_let(x, w.i32, num(w)), ex(use_of(w, pl(x, w.i32))))),
     "a `let` in scope for the statements after it");
  ok(verify_one(w, make_fn("f", {}, w.i32,
                           blk(w.i32, S(mk_let(x, w.i32, num(w))), use_of(w, pl(x, w.i32))))),
     "a `let` in scope for the result");
  bad(run_std(w, S(ex(use_of(w, pl(x, w.i32))), mk_let(x, w.i32, num(w)))), "P1",
      "a `let` used before it");
  bad(run_std(w, S(ex(blk(w.unit, S(mk_let(x, w.i32, num(w))))), ex(use_of(w, pl(x, w.i32))))),
      "P1", "a `let` of an inner block used after the block");
  bad(verify_one(w, make_fn("f", {}, w.i32,
                            blk(w.i32,
                                S(ex(blk(w.unit, S(mk_let(x, w.i32, num(w)))))),
                                use_of(w, pl(x, w.i32))))),
      "P1", "a `let` of an inner block used by the outer result");
  bad(run_std(w, S(mk_let(x, w.i32, use_of(w, pl(x, w.i32))))), "P1",
      "a `let` used in its own value");
}

auto test_e6_if() -> void {
  begin("E6");
  auto w = world{};
  const auto cond = [&] { return use_of(w, loc(w, sym::b)); };
  const auto x = w.fresh();
  // With an else.
  ok(run_std(w, S(mk_let(x, w.i32, if_of(w.i32, cond(), blk(w.i32, S(), num(w)), blk(w.i32, S(), num(w)))))),
     "an `if` with an `else`, each branch of the `if`'s type");
  bad(run_std(w, S(mk_let(x, w.i32, if_of(w.i32, num(w), blk(w.i32, S(), num(w)), blk(w.i32, S(), num(w)))))),
      "E6", "a condition that is not `bool`");
  bad(run_std(w, S(mk_let(x, w.i32, if_of(w.i32, cond(), blk(w.i64, S(), ilit(w.i64)), blk(w.i32, S(), num(w)))))),
      "E6", "a `then` branch of another type");
  bad(run_std(w, S(mk_let(x, w.i32, if_of(w.i32, cond(), blk(w.i32, S(), num(w)), blk(w.boolean, S(), blit(w)))))),
      "E6", "an `else` branch of another type");
  bad(run_std(w, S(mk_let(x, w.i32, if_of(w.boolean, cond(), blk(w.i32, S(), num(w)), blk(w.i32, S(), num(w)))))),
      {"E6", "S1"}, "an `if` whose type is not its branches' (the let's type also differs)");
  // A branch that diverges is not compared.
  ok(verify_one(w, make_fn("f", w.std_params(), w.i32,
                           blk(w.i32, S(mk_let(x, w.i32,
                                               if_of(w.i32, cond(), blk(w.i32, S(), num(w)),
                                                     blk(w.str, S(ret(num(w))), nullptr)))),
                               use_of(w, pl(x, w.i32))))),
     "an `else` branch that returns");
  // Without an else.
  ok(run_std(w, S(ex(if_of(w.unit, cond(), blk(w.unit, S(ex(num(w)))))))),
     "an `if` without `else` of type `unit`");
  bad(run_std(w, S(ex(if_of(w.i32, cond(), blk(w.i32, S(), num(w)))))), "E6",
      "an `if` without `else` of a type other than `unit`");
  bad(run_std(w, S(ex(if_of(w.unit, cond(), blk(w.i32, S(), num(w)))))), "E6",
      "an `if` without `else` whose branch is not `unit`");
  ok(verify_one(w, make_fn("f", w.std_params(), w.i32,
                           blk(w.i32, S(ex(if_of(w.unit, cond(), blk(w.str, S(ret(num(w))), nullptr)))),
                               num(w)))),
     "an `if` without `else` whose branch diverges");
}

auto test_e7_match_tests() -> void {
  begin("E7");
  auto w = world{};
  const auto on = [&](symbol_id s, pattern_test t) {
    return run_std(w, S(match1(w, loc(w, s), std::move(t))));
  };
  const auto subject_lit = [&](tid t) { return ilit(t, "1"); };
  // Each form, valid.
  ok(on(sym::n, t_any(w.i32)), "an `any` test");
  ok(on(sym::n, t_lit(w.i32, subject_lit(w.i32))), "a literal test");
  ok(on(sym::n, t_range(w.i32, subject_lit(w.i32), subject_lit(w.i32))), "a range test");
  ok(on(sym::sh, t_variant(w.shape, 2, TESTS(t_any(w.i32), t_lit(w.boolean, blit(w))))),
     "a variant test with a part per payload");
  ok(on(sym::sh, t_variant(w.shape, 1)), "a variant test of a unit variant");
  ok(on(sym::tup, t_tuple(w.tuple, TESTS(t_any(w.i32), t_any(w.boolean)))), "a tuple test");
  ok(on(sym::p, t_record(w.pair, {1}, TESTS(t_any(w.boolean)))), "a record test of one field");
  ok(on(sym::p, t_record(w.pair, {0, 1}, TESTS(t_any(w.i32), t_any(w.boolean)))),
     "a record test of every field");
  ok(on(sym::arr, t_array(w.arr, TESTS(t_any(w.i32), t_any(w.i32), t_lit(w.i32, subject_lit(w.i32))))),
     "an array test with a part per element");
  // Each form, broken.
  bad(on(sym::n, t_any(w.i64)), "E7", "a test whose type is not the subject's");
  ok(on(sym::n, t_range(w.i32, subject_lit(w.i32), nullptr)), "a range test with only a low bound");
  ok(on(sym::n, t_range(w.i32, nullptr, subject_lit(w.i32))), "a range test with only a high bound");
  bad(on(sym::n, t_range(w.i32, nullptr, nullptr)), "E7", "a range test with no bound");
  bad(on(sym::n, t_range(w.i32, subject_lit(w.i64), nullptr)), "E7",
      "a low bound of another type with no high bound");
  bad(on(sym::n, t_range(w.i32, nullptr, subject_lit(w.i64))), "E7",
      "a high bound of another type with no low bound");
  bad(on(sym::n, t_lit(w.i32, nullptr)), "E7", "a literal test with no value");
  bad(on(sym::n, t_lit(w.i32, subject_lit(w.i64))), "E7", "a literal bound of another type");
  bad(on(sym::n, t_range(w.i32, subject_lit(w.i32), subject_lit(w.i64))), "E7",
      "a range bound of another type");
  bad(on(sym::p, t_range(w.pair, ilit(w.pair), ilit(w.pair))), {"E7", "E1"},
      "a range test of a struct (the bounds are not valid literals either)");
  bad(on(sym::sh, t_variant(w.shape, 2, TESTS(t_any(w.i32)))), "E7",
      "a variant test with too few parts");
  bad(on(sym::sh, t_variant(w.shape, 1, TESTS(t_any(w.i32)))), "E7",
      "a variant test of a unit variant with a part");
  bad(on(sym::sh, t_variant(w.shape, 0, TESTS(t_any(w.boolean)))), "E7",
      "a variant part of the wrong type");
  bad(on(sym::sh, t_variant(w.shape, 9)), "E7", "a variant index out of range");
  bad(on(sym::n, t_variant(w.i32, 0)), "E7", "a variant test of an integer");
  bad(on(sym::tup, t_tuple(w.tuple, TESTS(t_any(w.i32)))), "E7", "a tuple test with too few parts");
  bad(on(sym::tup, t_tuple(w.tuple, TESTS(t_any(w.boolean), t_any(w.boolean)))), "E7",
      "a tuple part of the wrong type");
  bad(on(sym::p, t_tuple(w.pair, TESTS(t_any(w.i32), t_any(w.boolean)))), "E7",
      "a tuple test of a struct");
  bad(on(sym::p, t_record(w.pair, {5}, TESTS(t_any(w.i32)))), "E7",
      "a record test with a field index out of range");
  bad(on(sym::p, t_record(w.pair, {0}, TESTS(t_any(w.boolean)))), "E7",
      "a record part of the wrong type");
  bad(on(sym::n, t_record(w.i32, {0}, TESTS(t_any(w.i32)))), "E7", "a record test of an integer");
  bad(on(sym::arr, t_array(w.arr, TESTS(t_any(w.i32), t_any(w.i32)))), "E7",
      "an array test with too few parts");
  bad(on(sym::n, t_array(w.i32, TESTS(t_any(w.i32)))), "E7", "an array test of an integer");
}

auto test_e7_arms() -> void {
  begin("E7");
  auto w = world{};
  const auto x = w.fresh();
  const auto circle = [&] { return t_variant(w.shape, 0, TESTS(t_any(w.i32))); };
  const auto rect = [&] { return t_variant(w.shape, 2, TESTS(t_any(w.i32), t_any(w.boolean))); };
  const auto circle_x = [&](symbol_id s, pattern_binding::mode m = pattern_binding::mode::copy,
                            bool writable = false) {
    return BINDS(bnd(s, w.i32, STEPS(payload_step(w.i32, "circle", 0)), m, writable));
  };
  const auto rect_x = [&](symbol_id s, tid t = 0, pattern_binding::mode m = pattern_binding::mode::copy,
                          bool writable = false) {
    return BINDS(bnd(s, t == 0 ? w.i32 : t,
                     STEPS(payload_step(t == 0 ? w.i32 : t, "rect", t == w.boolean ? 1 : 0)), m, writable));
  };
  const auto run = [&](std::vector<pattern_alternative> alts) {
    return run_std(w, S(ex(match_of(w.unit, loc(w, sym::sh), ARMS(arm_of(std::move(alts), ublock(w)))))));
  };
  ok(run(ALTS(alt_of(circle(), circle_x(x)), alt_of(rect(), rect_x(x)))),
     "an or-pattern whose alternatives bind the same symbol, type and mode");
  bad(run(ALTS()), "E7", "an arm with no alternatives");
  bad(run(ALTS(alt_of(circle(), circle_x(x)), alt_of(rect(), rect_x(w.fresh())))), "E7",
      "alternatives that bind different symbols");
  bad(run(ALTS(alt_of(circle(), circle_x(x)), alt_of(rect(), rect_x(x, w.boolean)))), "E7",
      "alternatives that bind different types");
  bad(run(ALTS(alt_of(circle(), circle_x(x)), alt_of(rect(), BINDS()))), "E7",
      "an alternative that binds nothing");
  bad(run(ALTS(alt_of(circle(), circle_x(x)), alt_of(rect(), rect_x(x, 0, pattern_binding::mode::alias)))),
      "E7", "alternatives that bind with different modes");
  bad(run(ALTS(alt_of(circle(), circle_x(x, pattern_binding::mode::alias, false)),
               alt_of(rect(), rect_x(x, 0, pattern_binding::mode::alias, true)))),
      "E7", "alternatives that differ in `writable`");
}

auto test_e8_bindings() -> void {
  begin("E8");
  auto w = world{};
  const auto x = w.fresh();
  const auto bind_on = [&](symbol_id subject, pattern_test t, pattern_binding b) {
    return run_std(w, S(match1(w, loc(w, subject), std::move(t), BINDS(std::move(b)))));
  };
  ok(bind_on(sym::p, t_any(w.pair), bnd(x, w.i32, STEPS(field_step(w.i32, "a", 0)))),
     "a binding whose type is the part's type");
  bad(bind_on(sym::p, t_any(w.pair), bnd(x, w.i64, STEPS(field_step(w.i32, "a", 0)))), "E8",
      "a binding whose type is not the part's type");
  bad(bind_on(sym::p, t_any(w.pair), bnd(x, w.i32, STEPS(field_step(w.i32, "zzz", 0)))), "P2",
      "a binding whose path is invalid");
  // copy
  ok(bind_on(sym::h, t_any(w.holder), bnd(x, w.i32, STEPS(field_step(w.i32, "n", 1)),
                                          pattern_binding::mode::copy)),
     "a `copy` binding of a `copy` part");
  bad(bind_on(sym::h, t_any(w.holder), bnd(x, w.list_i32, STEPS(field_step(w.list_i32, "items", 0)),
                                           pattern_binding::mode::copy)),
      "E8", "a `copy` binding of a part that is not `copy`");
  // copy needs a part A2 would let move
  bad(bind_on(sym::rp, t_any(w.ref_pair),
              bnd(x, w.i32, STEPS(deref_step(w.pair), field_step(w.i32, "a", 0)),
                  pattern_binding::mode::copy)),
      "E8", "a `copy` binding of a part behind a reference");
  bad(bind_on(sym::arr, t_any(w.arr),
              bnd(x, w.i32, STEPS(element_step(w.i32, ilit(w.usize, "0"))),
                  pattern_binding::mode::copy)),
      "E8", "a `copy` binding of an array element");
  bad(run_std(w, S(match1(w, global_pl("m", "gp", w.pair), t_any(w.pair),
                          BINDS(bnd(x, w.i32, STEPS(field_step(w.i32, "a", 0)),
                                    pattern_binding::mode::copy))))),
      "E8", "a `copy` binding of a part of a static");
  ok(bind_on(sym::raw, t_any(w.raw_pair),
             bnd(x, w.i32, STEPS(deref_step(w.pair), field_step(w.i32, "a", 0)),
                 pattern_binding::mode::copy)),
     "a `copy` binding of a part behind a raw pointer");
  // move (A2)
  ok(bind_on(sym::h, t_any(w.holder), bnd(x, w.list_i32, STEPS(field_step(w.list_i32, "items", 0)),
                                          pattern_binding::mode::move)),
     "a `move` binding out of an owned subject");
  bad(bind_on(sym::rh, t_any(w.ref_holder),
              bnd(x, w.list_i32,
                  STEPS(deref_step(w.holder), field_step(w.list_i32, "items", 0)),
                  pattern_binding::mode::move)),
      "A2", "a `move` binding out of a reference");
  // alias: a writable alias needs a writable part (P4)
  ok(bind_on(sym::rm, t_any(w.mut_ref_pair),
             bnd(x, w.i32, STEPS(deref_step(w.pair), field_step(w.i32, "a", 0)),
                 pattern_binding::mode::alias, true)),
     "a writable `alias` of a part behind `&mut`");
  ok(bind_on(sym::rp, t_any(w.ref_pair),
             bnd(x, w.i32, STEPS(deref_step(w.pair), field_step(w.i32, "a", 0)),
                 pattern_binding::mode::alias, false)),
     "a read-only `alias` of a part behind `&`");
  bad(bind_on(sym::rp, t_any(w.ref_pair),
              bnd(x, w.i32, STEPS(deref_step(w.pair), field_step(w.i32, "a", 0)),
                  pattern_binding::mode::alias, true)),
      "E8", "a writable `alias` of a part behind `&`");
  ok(bind_on(sym::p, t_any(w.pair), bnd(x, w.i32, STEPS(field_step(w.i32, "a", 0)),
                                        pattern_binding::mode::alias, true)),
     "a writable `alias` of a field of a writable parameter");
  {
    const auto v = w.fresh();
    bad(run_std(w, S(mk_let(v, w.pair, nullptr, false),
                     match1(w, pl(v, w.pair), t_any(w.pair),
                            BINDS(bnd(x, w.i32, STEPS(field_step(w.i32, "a", 0)),
                                      pattern_binding::mode::alias, true))))),
        "E8", "a writable `alias` of a part of an immutable local");
  }
}

auto test_e9_guards_and_bodies() -> void {
  begin("E9");
  auto w = world{};
  const auto x = w.fresh();
  const auto run_arm = [&](ptr<expr> guard, ptr<block> body, tid match_type) {
    return run_std(
        w, S(ex(match_of(match_type, loc(w, sym::n),
                         ARMS(arm_of(ALTS(alt_of(t_any(w.i32))), std::move(body), std::move(guard)))))));
  };
  ok(run_arm(use_of(w, loc(w, sym::b)), ublock(w), w.unit), "a `bool` guard");
  bad(run_arm(num(w), ublock(w), w.unit), "E9", "a guard that is not `bool`");
  ok(run_arm(nullptr, blk(w.i32, S(), num(w)), w.i32), "an arm body of the match's type");
  bad(run_arm(nullptr, blk(w.boolean, S(), blit(w)), w.i32), "E9", "an arm body of another type");
  ok(verify_one(w, make_fn("f", w.std_params(), w.i32,
                           blk(w.i32, S(), match_of(w.i32, loc(w, sym::n),
                                                     ARMS(arm_of(ALTS(alt_of(t_lit(w.i32, ilit(w.i32)))),
                                                                 blk(w.i32, S(), num(w))),
                                                          arm_of(ALTS(alt_of(t_any(w.i32))),
                                                                 blk(w.str, S(ret(num(w))), nullptr))))))),
     "an arm body that returns is not compared");
  // A guard sees the arm's bindings.
  const auto guarded = [&](tid binding_type, std::string_view field, std::size_t index) {
    auto alt = alt_of(t_any(w.pair),
                      BINDS(bnd(x, binding_type, STEPS(field_step(binding_type, std::string(field), index)))));
    auto arm = arm_of(ALTS(std::move(alt)), ublock(w), use_of(w, pl(x, binding_type)));
    return run_std(w, S(ex(match_of(w.unit, loc(w, sym::p), ARMS(std::move(arm))))));
  };
  ok(guarded(w.boolean, "b", 1), "a guard that reads a binding of its arm");
  bad(guarded(w.i32, "a", 0), "E9", "a guard that is an integer binding");
}

auto test_e10_lambda() -> void {
  begin("E10");
  auto w = world{};
  const auto q = w.fresh();
  const auto c = w.fresh();
  // fn(q: int32) -> int32 capturing `n` by copy as `c`.
  const auto make = [&](tid ret, ptr<block> body, std::vector<capture> caps) {
    return run_std(w, S(ex(lambda_of(w, {prm(q, w.i32)}, std::move(caps), ret, std::move(body)))));
  };
  const auto cap_n = [&](capture::mode how, tid t, place from) {
    return CAPS(cap(c, std::move(from), how, t));
  };
  ok(make(w.i32, blk(w.i32, S(), prim_bin(ast::binary_op::add, w.i32, use_of(w, pl(q, w.i32)),
                                          use_of(w, pl(c, w.i32)))),
          cap_n(capture::mode::copy, w.i32, loc(w, sym::n))),
     "a lambda with a parameter and a `copy` capture");
  // Type and body.
  {
    auto lam = lambda_of(w, {prm(q, w.i32)}, {}, w.i32, blk(w.i32, S(), num(w)));
    lam->type = w.i32;
    bad(run_std(w, S(ex(std::move(lam)))), "E10", "a lambda whose type is not a `fn` type");
  }
  {
    auto lam = lambda_of(w, {prm(q, w.i32)}, {}, w.i32, blk(w.i32, S(), num(w)));
    lam->type = w.checked.types.fn_of({w.i64}, w.i32);
    bad(run_std(w, S(ex(std::move(lam)))), "E10", "a lambda type with other parameter types");
  }
  {
    auto lam = lambda_of(w, {prm(q, w.i32)}, {}, w.i32, blk(w.i32, S(), num(w)));
    lam->type = w.checked.types.fn_of({w.i32}, w.i64);
    bad(run_std(w, S(ex(std::move(lam)))), "E10", "a lambda type with another result");
  }
  bad(make(w.i32, blk(w.boolean, S(), blit(w)), {}), "E10", "a body of another type than `return_type`");
  ok(make(w.i32, blk(w.str, S(ret(num(w))), nullptr), {}), "a body that returns");
  // Captures.
  ok(make(w.unit, ublock(w), cap_n(capture::mode::move, w.pair, loc(w, sym::p))),
     "a `move` capture of a non-`copy` type");
  ok(make(w.unit, ublock(w), cap_n(capture::mode::borrow, w.pair, loc(w, sym::p))),
     "a `borrow` capture");
  ok(make(w.unit, ublock(w), cap_n(capture::mode::borrow_mut, w.pair, loc(w, sym::p))),
     "a `borrow_mut` capture of a writable place");
  bad(make(w.unit, ublock(w), cap_n(capture::mode::copy, w.i64, loc(w, sym::n))), "E10",
      "a capture whose type is not its place's");
  bad(make(w.unit, ublock(w), cap_n(capture::mode::copy, w.pair, loc(w, sym::p))), "E10",
      "a `copy` capture of a type that is not `copy`");
  bad(make(w.unit, ublock(w), cap_n(capture::mode::borrow_mut, w.pair, with(loc(w, sym::rp), deref_step(w.pair)))),
      "E10", "a `borrow_mut` capture of a place behind `&`");
  {
    const auto v = w.fresh();
    bad(run_std(w, S(mk_let(v, w.pair, nullptr, false),
                     ex(lambda_of(w, {}, CAPS(cap(c, pl(v, w.pair), capture::mode::borrow_mut, w.pair)),
                                  w.unit, ublock(w))))),
        "E10", "a `borrow_mut` capture of an immutable local");
  }
  bad(make(w.unit, ublock(w), cap_n(capture::mode::copy, w.i32, pl(888, w.i32))), "P1",
      "a capture of a place not in the enclosing scope");
  // Scope inside the body.
  bad(make(w.i32, blk(w.i32, S(), use_of(w, loc(w, sym::n))), {}), "P1",
      "a body that names a local of the enclosing function without capturing it");
  ok(make(w.i32, blk(w.i32, S(mk_let(w.fresh(), w.i32, num(w))), use_of(w, pl(q, w.i32))), {}),
     "a body that names its own parameter and bindings");
  // After the lambda, its own symbols are out of scope.
  bad(run_std(w, S(ex(lambda_of(w, {prm(q, w.i32)}, {}, w.unit, ublock(w))),
                   ex(use_of(w, pl(q, w.i32))))),
      "P1", "a lambda parameter used after the lambda");
}

auto test_e11_await() -> void {
  begin("E11");
  auto w = world{};
  const auto run = [&](tid result, symbol_id task) {
    return run_std(w, S(ex(await_of(result, use_of(w, loc(w, task))))));
  };
  ok(run(w.i32, sym::task), "`await` of a task that cannot fail gives its value type");
  ok(run(w.result_i32_str, sym::task_e), "`await` of a task that can fail gives a `result`");
  bad(run(w.i64, sym::task), "E11", "`await` giving another type");
  bad(run(w.i32, sym::task_e), "E11", "`await` of a fallible task giving its value type");
  bad(run(w.i32, sym::n), "E11", "`await` of an integer");
  bad(run(w.pair, sym::p), "E11", "`await` of a struct");
}

auto host_of(tid t, std::string operation, const void *subject, ptr_vec<expr> args)
    -> ptr<expr> {
  auto h = std::make_unique<host_call>(k_span, t);
  h->operation = std::move(operation);
  h->subject = subject;
  h->args = std::move(args);
  return h;
}

auto test_e12_host_call() -> void {
  begin("E12");
  auto w = world{};
  // Any address stands for the syntax a host call is about.
  static const int quoted = 0;
  const auto run = [&](ptr<expr> call, bool compile_time = true) {
    auto f = std_fn(w, w.unit, S(ex(std::move(call))));
    f->compile_time = compile_time;
    return verify_one(w, std::move(f));
  };
  ok(run(host_of(w.i32, "quote", &quoted, E(num(w), use_of(w, loc(w, sym::n))))),
     "a quote with its splices' values, in compile-time code");
  ok(run(host_of(w.i32, "static", &quoted, {})), "the syntax a `static` holds");
  ok(run(host_of(w.i32, "expr.lit", nullptr, E(num(w)))), "a fragment builder");
  bad(run(host_of(w.i32, "quote", &quoted, {}), false), "E12",
      "a quote in a function that runs after compiling");
  bad(run(host_of(w.i32, "quote", nullptr, {})), "E12", "a quote with no subject");
  bad(run(host_of(w.i32, "static", nullptr, {})), "E12", "a static's syntax with no subject");
  bad(run(host_of(w.i32, "expr.lit", &quoted, E(num(w)))), "E12", "a builder with a subject");
  bad(run(host_of(w.i32, "print", nullptr, {})), "E12", "an operation the host does not have");
  bad(run(host_of(w.i32, "expr.", nullptr, {})), "E12", "a builder with no name");
  {
    auto args = ptr_vec<expr>{};
    args.push_back(nullptr);
    bad(run(host_of(w.i32, "expr.lit", nullptr, std::move(args))), "E12",
        "a builder with a null argument");
  }
  bad(run(host_of(w.i32, "expr.lit", nullptr, E(use_of(w, pl(888, w.i32))))), "P1",
      "an argument that is not well formed");
}

// ===========================================================================
//  S: statements
// ===========================================================================

auto test_s1_let() -> void {
  begin("S1");
  auto w = world{};
  const auto x = w.fresh();
  ok(run_std(w, S(mk_let(x, w.i32, num(w)))), "a `let` whose value has its type");
  ok(run_std(w, S(mk_let(x, w.i32))), "a `let` with no value");
  bad(run_std(w, S(mk_let(x, w.i64, num(w)))), "S1", "a `let` whose value has another type");
  bad(run_std(w, S(mk_let(x, w.pair, use_of(w, loc(w, sym::rp))))), "S1",
      "a `let` of a struct bound to a reference");
  ok(verify_one(w, make_fn("f", {}, w.i32,
                           blk(w.i32, S(mk_let(x, w.pair, blk(w.str, S(ret(num(w))), nullptr))),
                               num(w)))),
     "a `let` whose value diverges");
}

auto test_s2_assign() -> void {
  begin("S2");
  auto w = world{};
  const auto x = w.fresh();
  ok(run_std(w, S(mk_let(x, w.i32, num(w), true), assign_to(pl(x, w.i32), num(w)))),
     "an assignment to a `mut` local");
  bad(run_std(w, S(mk_let(x, w.i32, num(w), true), assign_to(pl(x, w.i32), blit(w)))), "S2",
      "an assigned value of another type");
  ok(run_std(w, S(assign_to(loc(w, sym::n), num(w)))), "an assignment to a by-value parameter");
  // A whole local whose `let` had no value may be assigned once.
  ok(run_std(w, S(mk_let(x, w.i32), assign_to(pl(x, w.i32), num(w)))),
     "an assignment to a whole local declared without a value");
  bad(run_std(w, S(mk_let(x, w.pair), assign_to(with(pl(x, w.pair), field_step(w.i32, "a", 0)), num(w)))),
      "S2", "an assignment to a field of a local declared without a value");
  bad(run_std(w, S(mk_let(x, w.i32, num(w)), assign_to(pl(x, w.i32), num(w)))), "S2",
      "an assignment to an immutable local that has a value");
  bad(run_std(w, S(assign_to(with(loc(w, sym::rp), deref_step(w.pair)), use_of(w, loc(w, sym::p))))),
      "S2", "an assignment through `&`");
  ok(run_std(w, S(assign_to(with(loc(w, sym::rm), deref_step(w.pair)), use_of(w, loc(w, sym::p))))),
     "an assignment through `&mut`");
  bad(run_std(w, S(assign_to(global_pl("m", "g", w.i32), num(w)))), "S2", "an assignment to a static");
  ok(run_std(w, S(assign_to(temp_pl(num(w), w.i32), num(w)))), "an assignment to a temporary");
  ok(run_std(w, S(assign_to(with(loc(w, sym::slm), element_step(w.i32, ilit(w.usize, "0"))), num(w)))),
     "an assignment to an element of a mutable slice");
  bad(run_std(w, S(assign_to(with(loc(w, sym::sl), element_step(w.i32, ilit(w.usize, "0"))), num(w)))),
      "S2", "an assignment to an element of a shared slice");
}

auto test_s3_loops() -> void {
  begin("S3");
  auto w = world{};
  const auto cond = [&] { return use_of(w, loc(w, sym::b)); };
  ok(run_std(w, S(loop_of(cond(), ublock(w, S(brk()))))), "a loop with a `bool` condition and a `break`");
  ok(run_std(w, S(loop_of(nullptr, ublock(w, S(cont()))))), "an unconditional loop with a `continue`");
  ok(run_std(w, S(loop_of(cond(), ublock(w, S(ex(if_of(w.unit, cond(), blk(w.unit, S(brk())))))))))
     , "a `break` in an `if` inside a loop");
  ok(run_std(w, S(loop_of(cond(), ublock(w, S(loop_of(cond(), ublock(w, S(brk()))), brk()))))),
     "a `break` after a nested loop");
  bad(run_std(w, S(loop_of(num(w), ublock(w)))), "S3", "a loop condition that is not `bool`");
  bad(run_std(w, S(brk())), "S3", "a `break` outside a loop");
  bad(run_std(w, S(cont())), "S3", "a `continue` outside a loop");
  bad(run_std(w, S(loop_of(cond(), ublock(w)), brk())), "S3", "a `break` after a loop");
  bad(run_std(w, S(loop_of(cond(), ublock(w, S(ex(lambda_of(w, {}, {}, w.unit, ublock(w, S(brk()))))))))),
      "S3", "a `break` in a lambda inside a loop of the enclosing function");
}

auto test_s4_return_and_yield() -> void {
  begin("S4");
  auto w = world{};
  // return in a function returning int32
  ok(run_fn(w, {}, w.i32, S(ret(num(w)))), "`return` of the body type");
  ok(run_fn(w, {}, w.unit, S(ret())), "`return` with no value in a `unit` function");
  bad(run_fn(w, {}, w.i32, S(ret(blit(w)))), "S4", "`return` of another type");
  bad(run_fn(w, {}, w.i32, S(ret())), "S4", "`return` with no value where a value is needed");
  ok(run_fn(w, {}, w.unit, S(ret(make_lit(w.unit, token_kind::kw_unit, "()")))),
     "`return` of a `unit` value");
  // return in a lambda takes the lambda's return type.
  ok(run_std(w, S(ex(lambda_of(w, {}, {}, w.i32, blk(w.i32, S(ret(num(w))), nullptr))))),
     "`return` of a lambda's return type");
  bad(run_std(w, S(ex(lambda_of(w, {}, {}, w.i32, blk(w.i32, S(ret(blit(w))), nullptr))))), "S4",
      "`return` in a lambda of another type");
  bad(run_std(w, S(ex(lambda_of(w, {}, {}, w.i32, blk(w.i32, S(ret()), nullptr))))), "S4",
      "`return` with no value in a lambda that returns `int32`");
  // yield
  const auto generator = [&](tid yielded, ptr_vec<stmt> body) {
    auto f = make_fn("gen", {}, w.checked.types.builtin_generic("generator", {w.i32}), nullptr);
    f->body_type = w.unit;
    f->body = blk(w.unit, std::move(body));
    f->coroutine = coroutine_kind::generator;
    f->yield_type = w.i32;
    (void)yielded;
    return verify_one(w, std::move(f));
  };
  ok(generator(w.i32, S(jump_of(node_kind::yield_stmt, num(w)))), "`yield` of the yield type in a generator");
  bad(generator(w.i32, S(jump_of(node_kind::yield_stmt, blit(w)))), "S4",
      "`yield` of another type in a generator");
  bad(generator(w.i32, S(jump_of(node_kind::yield_stmt))), "S4", "`yield` with no value");
  bad(run_fn(w, {}, w.unit, S(jump_of(node_kind::yield_stmt, num(w)))), "S4",
      "`yield` outside a generator");
  bad(run_std(w, S(ex(lambda_of(w, {}, {}, w.unit, ublock(w, S(jump_of(node_kind::yield_stmt, num(w)))))))),
      "S4", "`yield` in a lambda");
  // A generator's `return` takes `unit`.
  ok(generator(w.i32, S(ret())), "`return` with no value in a generator");
}

auto test_s5_contract() -> void {
  begin("S5");
  auto w = world{};
  ok(run_std(w, S(contract(use_of(w, loc(w, sym::b))))), "a `bool` contract condition");
  bad(run_std(w, S(contract(num(w)))), "S5", "an integer contract condition");
  bad(run_std(w, S(contract(use_of(w, loc(w, sym::p))))), "S5", "a struct contract condition");
}

// ===========================================================================
//  F: functions and statics
// ===========================================================================

auto test_f1_symbols() -> void {
  begin("F1");
  auto w = world{};
  const auto x = w.fresh();
  const auto y = w.fresh();
  ok(run_std(w, S(mk_let(x, w.i32, num(w)), mk_let(y, w.i32, num(w)))), "two lets with two symbols");
  bad(run_std(w, S(mk_let(x, w.i32, num(w)), mk_let(x, w.i32, num(w)))), "F1", "one symbol in two lets");
  bad(run_std(w, S(mk_let(sym::n, w.i32, num(w)))), "F1", "a `let` with a parameter's symbol");
  bad(run_fn(w, {prm(x, w.i32), prm(x, w.i32)}, w.unit, S()), "F1", "two parameters with one symbol");
  bad(run_std(w, S(ex(blk(w.unit, S(mk_let(x, w.i32, num(w))))), mk_let(x, w.i32, num(w)))), "F1",
      "one symbol in a `let` of a block and a later `let`");
  // Pattern bindings.
  const auto circle = [&] { return t_variant(w.shape, 0, TESTS(t_any(w.i32))); };
  const auto bind_x = [&](symbol_id s) {
    return BINDS(bnd(s, w.i32, STEPS(payload_step(w.i32, "circle", 0))));
  };
  ok(run_std(w, S(match1(w, loc(w, sym::sh), circle(), bind_x(x)))), "a binding with a fresh symbol");
  bad(run_std(w, S(mk_let(x, w.i32, num(w)), match1(w, loc(w, sym::sh), circle(), bind_x(x)))), "F1",
      "a binding with the symbol of a `let`");
  bad(run_std(w, S(match1(w, loc(w, sym::sh), circle(), bind_x(sym::n)))), "F1",
      "a binding with a parameter's symbol");
  ok(run_std(w, S(ex(match_of(w.unit, loc(w, sym::sh),
                              ARMS(arm_of(ALTS(alt_of(circle(), bind_x(x)),
                                               alt_of(t_variant(w.shape, 2, TESTS(t_any(w.i32), t_any(w.boolean))),
                                                      BINDS(bnd(x, w.i32, STEPS(payload_step(w.i32, "rect", 0)))))),
                                          ublock(w))))))),
     "the alternatives of one arm share their symbols");
  // Captures and lambdas.
  const auto z = w.fresh();
  bad(run_std(w, S(mk_let(z, w.i32, num(w)),
                   ex(lambda_of(w, {}, CAPS(cap(z, loc(w, sym::n), capture::mode::copy, w.i32)),
                                w.unit, ublock(w))))),
      "F1", "a capture with the symbol of a `let`");
  bad(run_std(w, S(mk_let(z, w.i32, num(w)), ex(lambda_of(w, {prm(z, w.i32)}, {}, w.unit, ublock(w))))),
      "F1", "a lambda parameter with the symbol of a `let`");
  ok(run_std(w, S(ex(lambda_of(w, {prm(z, w.i32)}, {}, w.unit, ublock(w))))),
     "a lambda parameter with a fresh symbol");
  // The same symbol in another function is a different local.
  {
    auto f1 = make_fn("one", {prm(x, w.i32)}, w.unit, blk(w.unit));
    auto f2 = make_fn("two", {prm(x, w.i32)}, w.unit, blk(w.unit));
    auto fs = std::vector<ptr<function>>{};
    fs.push_back(std::move(f1));
    fs.push_back(std::move(f2));
    ok(verify_all(w, std::move(fs)), "one symbol in two functions");
  }
}

auto test_f2_body() -> void {
  begin("F2");
  auto w = world{};
  ok(run_fn(w, {}, w.i32, S(), num(w)), "a body of the body type");
  ok(run_fn(w, {}, w.unit, S()), "an empty `unit` body");
  bad(verify_one(w, make_fn("f", {}, w.i32, blk(w.boolean, S(), blit(w)))), "F2", "a body of another type");
  ok(run_fn(w, {}, w.i32, S(ret(num(w)))), "a body that diverges");
  bad(verify_one(w, make_fn("f", {}, w.i32, blk(w.unit))), "F2", "a body that is `unit` where a value is needed");
  // The plain function's return type equals its body type.
  {
    auto f = make_fn("f", {}, w.i32, blk(w.i32, S(), num(w)));
    f->body_type = w.i64;
    bad(verify_one(w, std::move(f)), "F2", "a plain function whose body type differs from its return type");
  }
  // Divergence (`nodes.h`, `expr`), one test per clause.
  {
    const auto never_call = [&] {
      return call_of(w.never, intrinsic(sig_of(w, {}, w.never), "panic"), E());
    };
    ok(run_fn(w, {}, w.i32, S(), never_call()), "a body whose result has type `never`");
    ok(verify_one(w, make_fn("f", {}, w.i32, blk(w.unit, S(ex(never_call()))))),
       "a body that ends in a statement whose expression diverges");
    bad(verify_one(w, make_fn("f", {}, w.i32, blk(w.unit, S(ex(num(w)))))), "F2",
        "a body that ends in a statement whose expression does not diverge");
    ok(run_fn(w, {}, w.i32, S(), blk(w.str, S(ret(num(w))))),
       "a body whose result is a block that diverges");
    const auto cond = [&] { return blit(w); };
    ok(run_fn(w, {}, w.i32, S(),
              if_of(w.i32, cond(), blk(w.unit, S(ret(num(w)))), blk(w.str, S(ret(num(w)))))),
       "a body that ends in an `if` whose branches both return");
    ok(verify_one(w, make_fn("f", {}, w.i32,
                             blk(w.unit, S(ex(if_of(w.unit, cond(), blk(w.unit, S(ret(num(w)))),
                                                    blk(w.unit, S(ret(num(w)))))))))),
       "a body whose last statement is an `if` whose branches both return");
    bad(verify_one(w, make_fn("f", {}, w.i32,
                              blk(w.unit, S(ex(if_of(w.unit, cond(), blk(w.unit, S(ret(num(w)))),
                                                     blk(w.unit))))))),
        "F2", "an `if` with one branch that does not diverge");
    bad(verify_one(w, make_fn("f", {}, w.i32,
                              blk(w.unit, S(ex(if_of(w.unit, cond(), blk(w.unit, S(ret(num(w)))))))))),
        "F2",
        "an `if` with no `else` does not diverge");
    const auto arms = [&](bool second_returns) {
      return match_of(
          w.unit, loc(w, sym::n),
          ARMS(arm_of(ALTS(alt_of(t_lit(w.i32, ilit(w.i32)))), blk(w.unit, S(ret(num(w))))),
               arm_of(ALTS(alt_of(t_any(w.i32))),
                      second_returns ? blk(w.unit, S(ret(num(w)))) : blk(w.unit))));
    };
    auto with_params = [&](ptr<stmt> st) {
      return verify_one(w, make_fn("f", w.std_params(), w.i32, blk(w.unit, S(std::move(st)))));
    };
    ok(with_params(ex(arms(true))), "a body that ends in a `match` whose arms all return");
    bad(with_params(ex(arms(false))), "F2", "a `match` with an arm that does not diverge");
    ok(run_fn(w, {}, w.unit,
              S(loop_of(blit(w), ublock(w, S(ex(if_of(w.i32, blit(w), blk(w.str, S(brk())),
                                                      blk(w.i64, S(cont()))))))))),
       "an `if` whose branches end in `break` and `continue`");
  }
  // An unconditional loop diverges unless it holds a `break` of its own.
  {
    const auto fn_ending_in = [&](ptr<stmt> st) {
      return verify_one(w, make_fn("f", w.std_params(), w.i32, blk(w.unit, S(std::move(st)))));
    };
    const auto brk_in_if = [&] {
      return ex(if_of(w.unit, blit(w), blk(w.unit, S(brk()))));
    };
    ok(fn_ending_in(loop_of(nullptr, ublock(w))), "a body that ends in a loop with no condition");
    ok(fn_ending_in(loop_of(blit(w), ublock(w, S(cont())))),
       "a body that ends in a `while true` loop with no `break`");
    bad(fn_ending_in(loop_of(blit(w), ublock(w, S(brk())))), "F2",
        "a body that ends in a `while true` loop with a `break`");
    bad(fn_ending_in(loop_of(nullptr, ublock(w, S(brk_in_if())))), "F2",
        "a `break` inside an `if` in the loop body");
    bad(fn_ending_in(loop_of(nullptr, ublock(w, S(ex(blk(w.unit, S(brk()))))))), "F2",
        "a `break` inside a nested block");
    ok(fn_ending_in(loop_of(nullptr, ublock(w, S(loop_of(blit(w), ublock(w, S(brk()))))))),
       "a `break` only inside a nested loop still diverges");
    ok(fn_ending_in(loop_of(nullptr, ublock(w, S(ex(lambda_of(w, {}, {}, w.unit,
                                                               ublock(w, S(ret())))))))),
       "a loop whose body only holds a lambda diverges");
    bad(fn_ending_in(loop_of(use_of(w, loc(w, sym::b)), ublock(w))), "F2",
        "a loop with a non-literal condition does not diverge");
    bad(fn_ending_in(loop_of(blit(w, false), ublock(w))), "F2",
        "a `while false` loop does not diverge");
  }
  // Generator: `generator[Y]`, body `unit`.
  const auto gen_type = w.checked.types.builtin_generic("generator", {w.i32});
  {
    auto f = make_fn("gen", {}, gen_type, blk(w.unit));
    f->body_type = w.unit;
    f->yield_type = w.i32;
    f->coroutine = coroutine_kind::generator;
    ok(verify_one(w, std::move(f)), "a generator over `generator[Y]` with a `unit` body");
  }
  {
    auto f = make_fn("gen", {}, gen_type, blk(w.unit));
    f->body_type = w.unit;
    f->yield_type = w.i64;
    f->coroutine = coroutine_kind::generator;
    bad(verify_one(w, std::move(f)), "F2", "a generator whose return type is not `generator[yield_type]`");
  }
  {
    auto f = make_fn("gen", {}, gen_type, blk(w.i32, S(), num(w)));
    f->body_type = w.i32;
    f->yield_type = w.i32;
    f->coroutine = coroutine_kind::generator;
    bad(verify_one(w, std::move(f)), "F2", "a generator whose body type is not `unit`");
  }
  // Task: infallible `task[T, never]`, body `T`; fallible `task[T, E]`, body `result[T, E]`.
  {
    auto f = make_fn("job", {}, w.task_i32, blk(w.i32, S(), num(w)));
    f->body_type = w.i32;
    f->coroutine = coroutine_kind::task;
    ok(verify_one(w, std::move(f)), "an async function over `task[T, never]`");
  }
  {
    auto f = make_fn("job", {}, w.task_err, blk(w.result_i32_str));
    f->body = blk(w.result_i32_str, S(ret(variant_of(w.result_i32_str, "ok", 0, E(num(w))))), nullptr);
    f->body_type = w.result_i32_str;
    f->coroutine = coroutine_kind::task;
    ok(verify_one(w, std::move(f)), "an async function over `task[T, E]` with a `result` body");
  }
  {
    auto f = make_fn("job", {}, w.task_i32, blk(w.i64, S(), ilit(w.i64)));
    f->body_type = w.i64;
    f->coroutine = coroutine_kind::task;
    bad(verify_one(w, std::move(f)), "F2", "an async function whose body type is not the task's value type");
  }
}

auto test_f3_receiver_and_generics() -> void {
  begin("F3");
  auto w = world{};
  const auto s = w.fresh();
  const auto method = [&](receiver_mode mode, tid first) {
    auto f = make_fn("m", {prm(s, first)}, w.unit, blk(w.unit));
    f->receiver = mode;
    return verify_one(w, std::move(f));
  };
  ok(method(receiver_mode::shared, w.ref_pair), "a shared receiver over `&S`");
  ok(method(receiver_mode::exclusive, w.mut_ref_pair), "an exclusive receiver over `&mut S`");
  ok(method(receiver_mode::by_value, w.i32), "a by-value receiver over a `copy` type");
  ok(method(receiver_mode::consuming, w.pair), "a consuming receiver over `S`");
  ok(method(receiver_mode::none, w.pair), "no receiver over any first parameter");
  bad(method(receiver_mode::shared, w.pair), "F3", "a shared receiver over `S`");
  bad(method(receiver_mode::shared, w.mut_ref_pair), "F3", "a shared receiver over `&mut S`");
  bad(method(receiver_mode::exclusive, w.ref_pair), "F3", "an exclusive receiver over `&S`");
  bad(method(receiver_mode::by_value, w.pair), "F3", "a by-value receiver over a type that is not `copy`");
  bad(method(receiver_mode::consuming, w.ref_pair), "F3", "a consuming receiver over `&S`");
  {
    auto f = make_fn("m", {}, w.unit, blk(w.unit));
    f->receiver = receiver_mode::shared;
    bad(verify_one(w, std::move(f)), "F3", "a receiver mode with no parameters");
  }
  // A function without generic parameters mentions none.
  bad(let_of_type(w, w.typ_t), "T3", "a function without generic parameters that mentions one");
  {
    auto f = make_fn("g", {prm(s, w.typ_t)}, w.unit, blk(w.unit));
    f->generic_params = {w.typ_t};
    ok(verify_one(w, std::move(f)), "a function that lists the parameter it mentions");
  }
  bad(verify_one(w, make_fn("g", {prm(s, w.typ_t)}, w.unit, blk(w.unit))), "T3",
      "a parameter of a type parameter that the function does not list");
}

auto test_f4_statics() -> void {
  begin("F4");
  auto w = world{};
  const auto run = [&](tid type, ptr<expr> value) {
    auto statics = std::vector<static_global>{};
    statics.push_back(static_global{.name = "s", .module = "m", .type = type, .value = std::move(value)});
    return verify_all(w, {}, std::move(statics));
  };
  ok(run(w.i32, num(w)), "a static built from a literal");
  ok(run(w.pair, agg(node_kind::struct_init, w.pair, E(num(w), blit(w)), {"a", "b"})),
     "a static built from a struct construction");
  ok(run(w.tuple, agg(node_kind::tuple_init, w.tuple, E(num(w), blit(w)))),
     "a static built from a tuple construction");
  ok(run(w.arr, agg(node_kind::array_init, w.arr, E(num(w)), {}, 3)), "a static built from an array");
  ok(run(w.shape, variant_of(w.shape, "circle", 0, E(num(w)))), "a static built from a variant");
  ok(run(w.i32, call_of(w.i32, intrinsic(sig_of(w, {w.i32}, w.i32), "abs"), E(num(w)))),
     "a static built from a call of literals");
  ok(run(w.pair,
         agg(node_kind::struct_init, w.pair,
             E(call_of(w.i32, intrinsic(sig_of(w, {w.i32}, w.i32), "abs"), E(num(w))), blit(w)),
             {"a", "b"})),
     "a static whose construction holds a call");
  bad(run(w.i64, num(w)), "F4", "a static whose value has another type");
  bad(run(w.i32, use_of(w, global_pl("m", "g", w.i32))), "F4", "a static that reads another static");
  bad(run(w.i32, prim_bin(ast::binary_op::add, w.i32, num(w), num(w))), "F4",
      "a static built from an operator");
  bad(run(w.i32, cast_to(w.i32, num(w))), {"F4"}, "a static built from a cast");
  bad(run(w.i32, blk(w.i32, S(), num(w))), "F4", "a static whose value is a block");
  bad(run(w.ref_i32, borrow_of(w, global_pl("m", "g", w.i32))), "F4", "a static whose value is a borrow");
  bad(run(w.pair, agg(node_kind::struct_init, w.pair,
                      E(prim_bin(ast::binary_op::add, w.i32, num(w), num(w)), blit(w)), {"a", "b"})),
      "F4", "a static whose construction holds an operator");
  bad(run(w.i32, call_of(w.i32, intrinsic(sig_of(w, {w.i32}, w.i32), "abs"),
                         E(use_of(w, global_pl("m", "g", w.i32))))),
      "F4", "a static whose call argument reads another static");
  bad(run(w.i32, if_of(w.i32, blit(w), blk(w.i32, S(), num(w)), blk(w.i32, S(), num(w)))), "F4",
      "a static whose value is an `if`");
}

} // namespace

auto main() -> int {
  test_t1_settled();
  test_t2_erased();
  test_t3_generic_scope();
  test_p1_roots();
  test_p2_field_step();
  test_p2_tuple_step();
  test_p2_variant_payload_step();
  test_p2_deref_step();
  test_p2_element_step();
  test_p3_variant_payload_position();
  test_p4_roots();
  test_p4_steps();
  test_p4_through_assignment();
  test_a1_use();
  test_a2_moves();
  test_a2_alias_and_borrowed_capture();
  test_a2_through_raw_pointers();
  test_open_shapes();
  test_a3_borrow_type();
  test_a4_mutable_borrow_is_writable();
  test_a5_two_phase();
  test_c1_signature_and_arguments();
  test_c2_function_callee();
  test_c3_receiver_modes();
  test_c4_value_callee();
  test_c5_intrinsic_callee();
  test_c5_compiler_operations();
  test_c5_declared_intrinsics();
  test_c6_generic_arguments();
  test_c7_function_ref();
  test_c8_generic_argument_count();
  test_e1_literals();
  test_e2_primitives();
  test_e3_casts();
  test_e4_tuple();
  test_e4_array();
  test_e4_struct();
  test_e4_variant();
  test_e5_blocks();
  test_e6_if();
  test_e7_match_tests();
  test_e7_arms();
  test_e8_bindings();
  test_e9_guards_and_bodies();
  test_e10_lambda();
  test_e11_await();
  test_e12_host_call();
  test_s1_let();
  test_s2_assign();
  test_s3_loops();
  test_s4_return_and_yield();
  test_s5_contract();
  test_f1_symbols();
  test_f2_body();
  test_f3_receiver_and_generics();
  test_f4_statics();

  for (const auto &[rule, count] : g_counts) {
    std::cout << rule << ": " << count << " checks\n";
  }
  return 0;
}
