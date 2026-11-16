#include "src/core/adapter.h"

#include <algorithm>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/comptime_vm/backend.h"
#include "src/core/nodes.h"
#include "src/core/verify.h"
#include "src/parser/parser.h"
#include "src/semantic/analysis.h"
#include "src/testing/stdlib_fixtures.h"
#include "src/testing/test_assert.h"

// Shape tests for `core::adapt_program`, one per mapping rule of the adapter
// brief: each builds a small program with the stdlib prelude, adapts it, and
// checks the core function the rule produces. The whole user module must
// also pass the core verifier.

namespace {

using cinder::testing::expect;
using cinder::testing::fail;
namespace core = cinder::core;
using core::node_kind;

constexpr auto k_program = R"(module adapter_test
type counter = { at: int32, step: int32 }

extend counter:
    def bump(mut self) -> unit:
        self.at = self.at + self.step

def add(a: int32, b: int32) -> int32:
    return a + b

def make() -> counter:
    return counter { step: 2, at: 1 }

def call_method() -> int32:
    var c = counter { at: 0, step: 1 }
    c.bump()
    return c.at

def unwrap_or_zero(x: option[int32]) -> int32:
    match x:
        @some(v) => return v
        @none => return 0

def through_ref(x: &option[str]) -> int32:
    match x:
        @some(s) => return 1
        @none => return 0

def if_let(x: option[int32]) -> int32:
    if let @some(v) = x:
        return v
    return 0

def drain(xs: option[int32]) -> int32:
    var total = 0
    var cur = xs
    while let @some(v) = cur:
        total = total + v
        cur = @none
    return total

def let_else(x: option[int32]) -> int32:
    let @some(v) = x else:
        return 0
    return v + 1

def or_else_zero(x: option[int32]) -> int32:
    return x or else 0

def try_it(x: option[int32]) -> option[int32]:
    let v = x?
    return @some(v + 1)

def is_odd(n: int32) -> bool:
    return n % 2 == 1

def sibling(n: int32) -> bool:
    return is_odd(n)

def apply(f: fn(int32) -> int32, x: int32) -> int32:
    return f(x)

def apply_ref(f: &fn(int32) -> int32, x: int32) -> int32:
    return f(x)

machine def read_at(p: *int32, i: usize) -> int32:
    return p[i]

machine def pass_mut(p: *mut int32) -> int32:
    return read_at(p, 0)

def classify(n: int32) -> int32:
    match n:
        ..0 => return 1
        100.. => return 2
        _ => return 3

def first(xs: array[int32, 2]) -> int32:
    match xs:
        [a, _] => return a

def sum_to(n: int32) -> int32:
    var total: int32 = 0
    for i in 0..=n:
        total = total + i
    return total

def sum_array(xs: array[int32, 3]) -> int32:
    var total: int32 = 0
    for x in xs:
        total = total + x
    return total

def squares(n: int32) -> list[int32]:
    return for i in 0..n => i * i

def middle(xs: slice[int32]) -> slice[int32]:
    return xs[1..3]

def middle_inclusive(xs: slice[int32]) -> slice[int32]:
    return xs[1..=2]

def span_of(a: usize, b: usize) -> range[usize]:
    return a..=b

def add_n(n: int32) -> int32:
    let f = (x: int32) => x + n
    return f(1)

def borrow_list() -> usize:
    let xs: list[int32] = [1, 2]
    let f = () => xs.len()
    return f()

def move_list() -> usize:
    let xs: list[int32] = [1, 2]
    let f = move [xs] () => xs.len()
    return f()

def twice_both() -> int32:
    let twice = (x) => x + x
    return twice(1) + int32(twice(1.5))

generator def count_up(n: int32) -> some iterator[int32]:
    var i: int32 = 0
    while i < n:
        yield i
        i = i + 1

async def later(x: int32) -> int32:
    return x + 1

async def chain(x: int32) -> int32:
    return (await later(x)) + 1

def greet(n: int32) -> str:
    return "n = {n}"

def cell_read(xs: &list[int32]) -> int32:
    let c = xs.cell(1)
    return *c

def squares_static() -> list[int32]:
    return static for i in 0..3 => i * i

def unrolled() -> int32:
    var total: int32 = 0
    static for i in 0..3:
        total = total + i
    return total

def pick[T]() -> int32:
    static if T.name() == "int8":
        return 1
    return 2

def pick_int8() -> int32:
    return pick[int8]()

def checked_half(x: int32) -> int32
pre x >= 0
post return >= 0
: x / 2

type positive = { pub value: int32 }
    invariant self.value > 0

def make_positive(n: int32) -> int32:
    var p = positive{value: n}
    p.value = n + 1
    return p.value

machine def buffer_len() -> usize:
    var buf = uninit[int32, 8]()
    return buf.len()

async def work(x: int32) -> int32:
    return x + 1

async def tasks(base: int32) -> int32:
    let t = async:
        base + 1
    let a = await t
    let b = await on(cpu):
        base * 2
    let (c, d) = await par:
        work(base)
        work(base + 3)
    var e: int32 = 0
    crew k:
        let u = k.spawn(work(base))
        e = 1
    return a + b + c + d + e

type pair[T] = { left: T, right: T }

def left_of[T](p: pair[T]) -> T:
    return p.left

def use_left() -> int32:
    return left_of(pair { left: 1, right: 2 })

def index_of(at: usize) -> usize:
    return at

def bump_at() -> int32:
    var xs: array[int32, 3] = [1, 2, 3]
    xs[index_of(1)] += 10
    return xs[1]

intrinsic def rt_panic(msg: str) -> never

def main() -> int32:
    return 0
)";

struct session {
  cinder::source_manager sources;
  cinder::diagnostic_bag diag;
  std::vector<cinder::ast::ptr<cinder::ast::file>> files;
  std::vector<cinder::semantic::parsed_module> inputs;
  std::vector<bool> file_has_errors;
  cinder::comptime_vm::vm_backend comptime;
  cinder::semantic::checked_types checked;
  core::adapted_program adapted;
  std::string user_module;
};

auto add_file(session &s, const std::string &path, const std::string &text)
    -> void {
  const auto id = s.sources.add_file(path, text);
  expect(id.has_value(), "expected the source to register");
  if (s.file_has_errors.size() <= static_cast<std::size_t>(*id)) {
    s.file_has_errors.resize(static_cast<std::size_t>(*id) + 1, false);
  }
  const auto *file = s.sources.get(*id);
  auto lexer = cinder::lexer(file->source(), file->id(), s.diag);
  auto parser = cinder::parser(lexer.tokenize(), file->id(), s.diag);
  auto ast = parser.parse_file();
  s.inputs.push_back(
      cinder::semantic::parsed_module{.file_id = *id, .ast_file = ast.get()});
  s.files.push_back(std::move(ast));
}

auto build_session(session &s) -> void {
  for (auto &source : cinder::testing::load_stdlib_sources()) {
    add_file(s, source.path, source.text);
  }
  add_file(s, "adapter_test.cn", k_program);
  s.checked = cinder::semantic::validate_semantics(
      s.inputs, s.diag, s.file_has_errors,
      cinder::semantic::semantic_options{.comptime = &s.comptime});
  if (s.diag.error_count() != 0) {
    std::cerr << cinder::diagnostic_renderer(s.sources, false)
                     .render_all(s.diag);
    fail("expected the adapter test program to check cleanly");
  }
  s.adapted = core::adapt_program(s.inputs, s.file_has_errors, s.checked);
  for (const auto &m : s.adapted.modules) {
    for (const auto &f : m->functions) {
      if (f->name == "add") {
        s.user_module = m->name;
      }
    }
  }
  expect(!s.user_module.empty(), "expected `add` to be built");
}

auto function_named(const session &s, std::string_view name)
    -> const core::function & {
  for (const auto &m : s.adapted.modules) {
    if (m->name != s.user_module) {
      continue;
    }
    for (const auto &f : m->functions) {
      if (f->name == name) {
        return *f;
      }
    }
  }
  for (const auto &gap : s.adapted.gaps) {
    if (gap.module == s.user_module && gap.function == name) {
      fail(std::format("`{}` was not built: {}: {}", name, gap.construct,
                       gap.detail));
    }
  }
  fail(std::format("no function `{}`", name));
}

template <typename T>
auto as(const core::node *n, node_kind kind, std::string_view what)
    -> const T & {
  expect(n != nullptr, std::format("{}: missing", what));
  expect(n->kind == kind, std::format("{}: wrong node kind", what));
  return static_cast<const T &>(*n);
}

/// The value of the `index`th statement of `f`'s body, which must be a
/// `return`.
auto returned(const core::function &f, std::size_t index)
    -> const core::expr & {
  expect(f.body != nullptr && f.body->statements.size() > index,
         std::format("{}: too few statements", f.name));
  const auto &ret = as<core::jump>(f.body->statements[index].get(),
                                   node_kind::return_stmt, f.name);
  expect(ret.value != nullptr, std::format("{}: return has no value", f.name));
  return *ret.value;
}

/// The `match` in the `index`th statement of `f`, an expression statement.
auto match_in(const core::function &f, std::size_t index)
    -> const core::match_expr & {
  // A trailing `match` of a valued body is the body's result.
  if (f.body != nullptr && f.body->statements.size() == index &&
      f.body->result != nullptr) {
    return as<core::match_expr>(f.body->result.get(), node_kind::match_expr,
                                f.name);
  }
  expect(f.body != nullptr && f.body->statements.size() > index,
         std::format("{}: too few statements", f.name));
  const auto &s = as<core::expr_stmt>(f.body->statements[index].get(),
                                      node_kind::expr_stmt, f.name);
  return as<core::match_expr>(s.value.get(), node_kind::match_expr, f.name);
}

auto test_scalar_body(const session &s) -> void {
  const auto &value = returned(function_named(s, "add"), 0);
  const auto &sum =
      as<core::primitive>(&value, node_kind::primitive, "add's sum");
  expect(sum.binary == cinder::ast::binary_op::add, "add: `+` operator");
  expect(sum.operands.size() == 2, "add: two operands");
  for (const auto &operand : sum.operands) {
    const auto &read = as<core::use>(operand.get(), node_kind::use, "operand");
    expect(read.how == core::use::mode::copy, "add: operands are copies");
    expect(read.from.root.kind == core::place_root::origin::local,
           "add: operands are locals");
  }
}

auto test_struct_literal(const session &s) -> void {
  const auto &value = returned(function_named(s, "make"), 0);
  const auto &made = as<core::aggregate_init>(&value, node_kind::struct_init,
                                              "make's literal");
  // Source order, with each field's position in the declaration.
  expect(made.field_names == std::vector<std::string>{"step", "at"},
         "make: fields in source order");
  expect(made.field_indices == std::vector<std::size_t>{1, 0},
         "make: field indices name the declaration positions");
  expect(made.elements.size() == 2, "make: two elements");
}

auto test_method_receiver(const session &s) -> void {
  const auto &f = function_named(s, "call_method");
  const auto &stmt = as<core::expr_stmt>(f.body->statements[1].get(),
                                         node_kind::expr_stmt, "c.bump()");
  const auto &bump =
      as<core::call>(stmt.value.get(), node_kind::call, "c.bump()");
  expect(bump.target.kind == core::callee::form::function,
         "c.bump(): a function callee");
  expect(bump.target.receiver == core::receiver_mode::exclusive,
         "c.bump(): an exclusive receiver");
  expect(bump.args.size() == 1, "c.bump(): the receiver is the argument");
  const auto &lent =
      as<core::borrow>(bump.args[0].get(), node_kind::borrow, "receiver");
  expect(lent.is_mut && lent.inserted, "c.bump(): an inserted `&mut c`");
}

auto test_match(const session &s) -> void {
  const auto &m = match_in(function_named(s, "unwrap_or_zero"), 0);
  expect(m.arms.size() == 2, "match: two arms");
  const auto &some = m.arms[0].alternatives.at(0);
  expect(some.test.kind == core::pattern_test::form::variant,
         "match: a variant test");
  expect(some.bindings.size() == 1 && some.bindings[0].name == "v",
         "match: `v` is bound");
  expect(some.bindings[0].how == core::pattern_binding::mode::copy,
         "match: an `int32` binding copies");
  expect(some.bindings[0].path.size() == 1 &&
             some.bindings[0].path[0].kind ==
                 core::projection::step::variant_payload,
         "match: the binding is the payload");
}

auto test_alias_binding(const session &s) -> void {
  const auto &m = match_in(function_named(s, "through_ref"), 0);
  expect(!m.subject.path.empty() &&
             m.subject.path.back().kind == core::projection::step::deref,
         "through_ref: the subject is dereferenced");
  const auto &binding = m.arms[0].alternatives.at(0).bindings.at(0);
  expect(binding.how == core::pattern_binding::mode::alias,
         "through_ref: a binding behind `&` is an alias");
  expect(!binding.writable, "through_ref: not writable through `&`");
}

auto test_if_let(const session &s) -> void {
  const auto &m = match_in(function_named(s, "if_let"), 0);
  expect(m.arms.size() == 2, "if let: two arms");
  expect(m.arms[1].alternatives.at(0).test.kind ==
             core::pattern_test::form::any,
         "if let: the else arm matches anything");
}

auto test_while_let(const session &s) -> void {
  const auto &f = function_named(s, "drain");
  const auto &loop = as<core::while_loop>(f.body->statements[2].get(),
                                          node_kind::while_loop, "while let");
  expect(loop.condition == nullptr, "while let: no condition");
  const auto &s0 = as<core::expr_stmt>(loop.body->statements.at(0).get(),
                                       node_kind::expr_stmt, "while let body");
  const auto &m =
      as<core::match_expr>(s0.value.get(), node_kind::match_expr, "while let");
  expect(m.arms.size() == 2, "while let: two arms");
  as<core::jump>(m.arms[1].body->statements.at(0).get(), node_kind::break_stmt,
                 "while let: the other arm breaks");
}

auto test_let_else(const session &s) -> void {
  const auto &m = match_in(function_named(s, "let_else"), 0);
  expect(m.arms.size() == 2, "let else: two arms");
  // The statements after the `let` move into the matching arm.
  as<core::jump>(m.arms[0].body->statements.at(0).get(), node_kind::return_stmt,
                 "let else: `return v + 1` is in the arm");
}

auto test_or_else(const session &s) -> void {
  const auto &value = returned(function_named(s, "or_else_zero"), 0);
  const auto &m =
      as<core::match_expr>(&value, node_kind::match_expr, "or else");
  expect(m.arms.size() == 2, "or else: two arms");
  expect(m.arms[1].body->result != nullptr &&
             m.arms[1].body->result->kind == node_kind::literal,
         "or else: the fallback is the other arm's value");
}

auto test_try(const session &s) -> void {
  const auto &f = function_named(s, "try_it");
  const auto &let = as<core::let>(f.body->statements.at(0).get(),
                                  node_kind::let, "let v = x?");
  const auto &m =
      as<core::match_expr>(let.value.get(), node_kind::match_expr, "x?");
  expect(m.arms.size() == 2, "?: two arms");
  const auto &leave = as<core::jump>(m.arms[1].body->statements.at(0).get(),
                                     node_kind::return_stmt, "?: returns");
  const auto &none = as<core::variant_init>(
      leave.value.get(), node_kind::variant_init, "?: returns `@none`");
  expect(none.variant == "none" && none.type == f.body_type,
         "?: `@none` of the function's return type");
}

auto test_same_module_call(const session &s) -> void {
  const auto &value = returned(function_named(s, "sibling"), 0);
  const auto &called = as<core::call>(&value, node_kind::call, "is_odd(n)");
  expect(called.target.kind == core::callee::form::function &&
             called.target.decl != nullptr &&
             called.target.decl->name == "is_odd",
         "sibling: a bare same-module call names its function");
  expect(called.target.owner_module == s.user_module,
         "sibling: the callee is owned by the current module");
}

auto test_call_through_value(const session &s) -> void {
  const auto &value = returned(function_named(s, "apply"), 0);
  const auto &called = as<core::call>(&value, node_kind::call, "f(x)");
  expect(called.target.kind == core::callee::form::value, "apply: a value callee");
  const auto &lent = as<core::borrow>(called.target.value.get(),
                                      node_kind::borrow, "apply: callee");
  expect(lent.inserted && !lent.is_mut && lent.of.path.empty(),
         "apply: the local closure is called through an inserted `&`");

  const auto &by_ref = returned(function_named(s, "apply_ref"), 0);
  const auto &ref_call = as<core::call>(&by_ref, node_kind::call, "f(x)");
  const auto &copied = as<core::use>(ref_call.target.value.get(),
                                     node_kind::use, "apply_ref: callee");
  expect(copied.how == core::use::mode::copy && copied.from.path.empty(),
         "apply_ref: a `&fn` is copied, not dereferenced");
}

auto test_pointer_index(const session &s) -> void {
  const auto &value = returned(function_named(s, "read_at"), 0);
  const auto &read = as<core::use>(&value, node_kind::use, "p[i]");
  expect(read.from.path.size() == 1 &&
             read.from.path[0].kind == core::projection::step::element,
         "read_at: a raw pointer index is an element step");
}

auto test_pointer_cast(const session &s) -> void {
  const auto &value = returned(function_named(s, "pass_mut"), 0);
  const auto &called = as<core::call>(&value, node_kind::call, "read_at(p, 0)");
  as<core::cast>(called.args.at(0).get(), node_kind::cast,
                 "pass_mut: `*mut int32` as `*int32` is a cast");
}

auto test_open_range(const session &s) -> void {
  const auto &m = match_in(function_named(s, "classify"), 0);
  const auto &below = m.arms.at(0).alternatives.at(0).test;
  expect(below.kind == core::pattern_test::form::range && below.low == nullptr &&
             below.high != nullptr,
         "classify: `..0` has only a high bound");
  const auto &above = m.arms.at(1).alternatives.at(0).test;
  expect(above.kind == core::pattern_test::form::range && above.low != nullptr &&
             above.high == nullptr,
         "classify: `100..` has only a low bound");
}

auto test_element_binding(const session &s) -> void {
  const auto &m = match_in(function_named(s, "first"), 0);
  const auto &binding = m.arms.at(0).alternatives.at(0).bindings.at(0);
  expect(binding.how == core::pattern_binding::mode::alias,
         "first: a binding of an array element is an alias");
  expect(!binding.writable, "first: an undeclared alias is not writable");
}

/// The block a `for` statement of `f` becomes: an expression statement
/// holding the loop's `let`s and its `while`.
auto for_block(const core::function &f) -> const core::block & {
  for (const auto &st : f.body->statements) {
    if (st == nullptr || st->kind != node_kind::expr_stmt) {
      continue;
    }
    const auto &e = static_cast<const core::expr_stmt &>(*st);
    if (e.value != nullptr && e.value->kind == node_kind::block) {
      return static_cast<const core::block &>(*e.value);
    }
  }
  fail(std::format("{}: no `for` block", f.name));
}

/// The `while` that ends a `for` block.
auto loop_of(const core::block &b, std::string_view what)
    -> const core::while_loop & {
  expect(!b.statements.empty(), std::format("{}: an empty block", what));
  return as<core::while_loop>(b.statements.back().get(), node_kind::while_loop,
                              what);
}

auto test_range_loop(const session &s) -> void {
  const auto &b = for_block(function_named(s, "sum_to"));
  const auto &loop = loop_of(b, "sum_to");
  expect(b.statements.size() == 4 &&
             b.statements[0]->kind == node_kind::let &&
             b.statements[1]->kind == node_kind::let &&
             b.statements[2]->kind == node_kind::let,
         "sum_to: start, end and index locals before the loop");
  const auto &test = as<core::primitive>(loop.condition.get(),
                                         node_kind::primitive, "sum_to: test");
  expect(test.binary == cinder::ast::binary_op::lt_eq,
         "sum_to: `..=` counts while index <= end");
  expect(loop.step != nullptr && !loop.step->statements.empty(),
         "sum_to: the index advances in the step block");
}

auto test_array_loop(const session &s) -> void {
  const auto &loop = loop_of(for_block(function_named(s, "sum_array")),
                             "sum_array");
  const auto &test = as<core::primitive>(
      loop.condition.get(), node_kind::primitive, "sum_array: test");
  expect(test.operands.size() == 2 &&
             test.operands[1]->kind == node_kind::literal,
         "sum_array: the bound is the array's size");
}

auto test_comprehension(const session &s) -> void {
  const auto &value = returned(function_named(s, "squares"), 0);
  const auto &b = as<core::block>(&value, node_kind::block, "squares");
  expect(b.result != nullptr && b.result->kind == node_kind::use,
         "squares: the block's value is the accumulator");
  expect(b.statements.size() >= 2 &&
             b.statements.front()->kind == node_kind::let,
         "squares: the accumulator is a `let` first");
}

auto test_range_index(const session &s) -> void {
  const auto &value = returned(function_named(s, "middle"), 0);
  const auto &view = as<core::use>(&value, node_kind::use, "xs[1..3]");
  expect(view.from.root.kind == core::place_root::origin::temporary &&
             view.from.root.value != nullptr,
         "middle: the view is a temporary");
  const auto &made = as<core::call>(view.from.root.value.get(), node_kind::call,
                                    "middle: range_view");
  expect(made.target.kind == core::callee::form::intrinsic &&
             made.target.name == "range_view" && made.args.size() == 3,
         "middle: `range_view(xs, 1, 3)`");
  const auto &inclusive_value =
      returned(function_named(s, "middle_inclusive"), 0);
  const auto &inclusive_view = as<core::use>(&inclusive_value, node_kind::use,
                                             "xs[1..=2]");
  const auto &inclusive = as<core::call>(
      inclusive_view.from.root.value.get(), node_kind::call,
      "middle_inclusive: range_view_inclusive");
  expect(inclusive.target.name == "range_view_inclusive" &&
             inclusive.args.size() == 3 &&
             inclusive.args[2]->kind == node_kind::literal,
         "middle_inclusive: `range_view_inclusive(xs, 1, 2)` keeps `2` as "
         "written");
}

auto test_range_value(const session &s) -> void {
  const auto &value = returned(function_named(s, "span_of"), 0);
  const auto &made =
      as<core::aggregate_init>(&value, node_kind::struct_init, "a..=b");
  expect(made.field_names == std::vector<std::string>{"start", "end"},
         "span_of: a range value sets start and end");
  const auto &end = as<core::primitive>(made.elements.at(1).get(),
                                        node_kind::primitive, "span_of: end");
  expect(end.binary == cinder::ast::binary_op::add,
         "span_of: `a..=b` stores `b + 1` as its end");
}

/// The lambda bound by the `index`th statement of `f`, a `let`.
auto lambda_in(const core::function &f, std::size_t index)
    -> const core::lambda & {
  expect(f.body != nullptr && f.body->statements.size() > index,
         std::format("{}: too few statements", f.name));
  const auto &bound = as<core::let>(f.body->statements[index].get(),
                                    node_kind::let, f.name);
  return as<core::lambda>(bound.value.get(), node_kind::lambda, f.name);
}

auto test_lambda_captures(const session &s) -> void {
  const auto &f = function_named(s, "add_n");
  const auto &copied = lambda_in(f, 0);
  expect(copied.captures.size() == 1 &&
             copied.captures[0].how == core::capture::mode::copy,
         "add_n: `n` is captured by copy");
  expect(copied.captures[0].symbol != f.params.at(0).symbol &&
             copied.captures[0].from.root.symbol == f.params.at(0).symbol,
         "add_n: the capture has a fresh symbol naming the parameter");

  const auto &lent = lambda_in(function_named(s, "borrow_list"), 1);
  expect(lent.captures.size() == 1 &&
             lent.captures[0].how == core::capture::mode::borrow,
         "borrow_list: a non-copy local the body reads is borrowed");

  const auto &moved = lambda_in(function_named(s, "move_list"), 1);
  expect(moved.captures.size() == 1 &&
             moved.captures[0].how == core::capture::mode::move,
         "move_list: `move [xs]` moves the list");
}

auto test_generic_lambda(const session &s) -> void {
  const auto &f = function_named(s, "twice_both");
  const auto &first = lambda_in(f, 0);
  const auto &second = lambda_in(f, 1);
  expect(first.type != second.type,
         "twice_both: one closure per copy, each at its own type");
}

auto test_generator(const session &s) -> void {
  const auto &f = function_named(s, "count_up");
  expect(f.coroutine == core::coroutine_kind::generator,
         "count_up: a generator coroutine");
  expect(s.checked.types.display(f.yield_type) == "int32" &&
             s.checked.types.display(f.body_type) == "unit",
         "count_up: yields `int32`, returns `unit` in its body");
  const auto &loop = as<core::while_loop>(f.body->statements.at(1).get(),
                                          node_kind::while_loop, "count_up");
  as<core::jump>(loop.body->statements.at(0).get(), node_kind::yield_stmt,
                 "count_up: `yield i`");
}

auto test_async(const session &s) -> void {
  const auto &f = function_named(s, "later");
  expect(f.coroutine == core::coroutine_kind::task, "later: a task");
  expect(s.checked.types.display(f.return_type) == "task[int32, never, io]" &&
             s.checked.types.display(f.body_type) == "int32",
         "later: callers see `task[int32, never, io]`, the body `int32`");
  const auto &sum = as<core::primitive>(&returned(function_named(s, "chain"), 0),
                                        node_kind::primitive, "chain");
  as<core::await_expr>(sum.operands.at(0).get(), node_kind::await_expr,
                       "chain: `await later(x)`");
}

auto test_interpolation(const session &s) -> void {
  const auto &joined = as<core::call>(&returned(function_named(s, "greet"), 0),
                                      node_kind::call, "greet");
  expect(joined.target.kind == core::callee::form::intrinsic &&
             joined.target.name == "rt_str_concat" && joined.args.size() == 2,
         "greet: the pieces are joined by `rt_str_concat`");
  const auto &shown =
      as<core::call>(joined.args[1].get(), node_kind::call, "greet: `{n}`");
  expect(shown.target.decl != nullptr &&
             shown.target.decl->name == "fmt_show_i64" &&
             shown.args.at(0)->kind == node_kind::cast,
         "greet: `{n}` is `fmt_show_i64(n as int64, spec)`");
}

/// What a function's tree holds, for the mappings whose shape is spread
/// over several nodes: intrinsic callees in tree order, contract checks,
/// and lambdas.
struct tree_facts {
  std::vector<std::string> intrinsics;
  std::size_t contract_checks = 0;
  std::vector<const core::lambda *> lambdas;
  std::vector<const core::call *> calls;
};

auto collect(const core::expr *e, tree_facts &facts) -> void;
auto collect(const core::stmt *s, tree_facts &facts) -> void;

auto collect(const core::place &p, tree_facts &facts) -> void {
  collect(p.root.value.get(), facts);
  for (const auto &step : p.path) {
    collect(step.subscript.get(), facts);
  }
}

auto collect(const core::block *b, tree_facts &facts) -> void {
  if (b == nullptr) {
    return;
  }
  for (const auto &s : b->statements) {
    collect(s.get(), facts);
  }
  collect(b->result.get(), facts);
}

auto collect(const core::expr *e, tree_facts &facts) -> void {
  if (e == nullptr) {
    return;
  }
  switch (e->kind) {
  case node_kind::use:
    collect(static_cast<const core::use &>(*e).from, facts);
    return;
  case node_kind::borrow:
    collect(static_cast<const core::borrow &>(*e).of, facts);
    return;
  case node_kind::call: {
    const auto &c = static_cast<const core::call &>(*e);
    facts.calls.push_back(&c);
    if (c.target.kind == core::callee::form::intrinsic) {
      facts.intrinsics.push_back(c.target.name);
    }
    collect(c.target.value.get(), facts);
    for (const auto &a : c.args) {
      collect(a.get(), facts);
    }
    return;
  }
  case node_kind::primitive:
    for (const auto &o : static_cast<const core::primitive &>(*e).operands) {
      collect(o.get(), facts);
    }
    return;
  case node_kind::cast:
    collect(static_cast<const core::cast &>(*e).operand.get(), facts);
    return;
  case node_kind::tuple_init:
  case node_kind::array_init:
  case node_kind::struct_init:
    for (const auto &x :
         static_cast<const core::aggregate_init &>(*e).elements) {
      collect(x.get(), facts);
    }
    return;
  case node_kind::variant_init:
    for (const auto &x : static_cast<const core::variant_init &>(*e).payload) {
      collect(x.get(), facts);
    }
    return;
  case node_kind::lambda: {
    const auto &l = static_cast<const core::lambda &>(*e);
    facts.lambdas.push_back(&l);
    collect(l.body.get(), facts);
    return;
  }
  case node_kind::block:
    collect(static_cast<const core::block *>(e), facts);
    return;
  case node_kind::if_expr: {
    const auto &i = static_cast<const core::if_expr &>(*e);
    collect(i.condition.get(), facts);
    collect(i.then_branch.get(), facts);
    collect(i.else_branch.get(), facts);
    return;
  }
  case node_kind::match_expr: {
    const auto &m = static_cast<const core::match_expr &>(*e);
    collect(m.subject, facts);
    for (const auto &arm : m.arms) {
      collect(arm.guard.get(), facts);
      collect(arm.body.get(), facts);
    }
    return;
  }
  case node_kind::await_expr:
    collect(static_cast<const core::await_expr &>(*e).task.get(), facts);
    return;
  default:
    return;
  }
}

auto collect(const core::stmt *s, tree_facts &facts) -> void {
  if (s == nullptr) {
    return;
  }
  switch (s->kind) {
  case node_kind::let:
    collect(static_cast<const core::let &>(*s).value.get(), facts);
    return;
  case node_kind::assign: {
    const auto &a = static_cast<const core::assign &>(*s);
    collect(a.target, facts);
    collect(a.value.get(), facts);
    return;
  }
  case node_kind::expr_stmt:
    collect(static_cast<const core::expr_stmt &>(*s).value.get(), facts);
    return;
  case node_kind::while_loop: {
    const auto &w = static_cast<const core::while_loop &>(*s);
    collect(w.condition.get(), facts);
    collect(w.body.get(), facts);
    collect(w.step.get(), facts);
    return;
  }
  case node_kind::return_stmt:
  case node_kind::yield_stmt:
    collect(static_cast<const core::jump &>(*s).value.get(), facts);
    return;
  case node_kind::contract_check:
    ++facts.contract_checks;
    collect(static_cast<const core::contract_check &>(*s).condition.get(),
            facts);
    return;
  default:
    return;
  }
}

auto facts_of(const core::function &f) -> tree_facts {
  auto facts = tree_facts{};
  collect(f.body.get(), facts);
  return facts;
}

auto count_of(const tree_facts &facts, std::string_view name) -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count(facts.intrinsics, name));
}

/// `xs[f()] += v` evaluates its place once: the call in the subscript is
/// bound by a `let` that the assignment's two uses of the place read.
auto test_compound_assign(const session &s) -> void {
  const auto &f = function_named(s, "bump_at");
  const auto facts = facts_of(f);
  const auto calls = std::ranges::count_if(
      facts.calls,
      [](const core::call *c) { return c->target.name == "index_of"; });
  expect(calls == 1, std::format("bump_at: `index_of` is called once, not {}",
                                 calls));
  const auto &stmts = f.body->statements;
  const auto assign = std::ranges::find_if(stmts, [](const auto &stmt) {
    return stmt->kind == node_kind::assign;
  });
  expect(assign != stmts.end(), "bump_at: an assignment");
  expect((*assign)->continues_statement,
         "bump_at: the assignment continues the subscript's `let`");
  expect((*assign)->origin != nullptr, "bump_at: the assignment has an origin");
  const auto &held = *assign[-1];
  expect(held.kind == node_kind::let && !held.continues_statement,
         "bump_at: a `let` of the subscript starts the statement");
}

auto test_cell_deref(const session &s) -> void {
  const auto facts = facts_of(function_named(s, "cell_read"));
  expect(count_of(facts, "cell_to_ref") == 1,
         "cell_read: `*c` converts the cell with `cell_to_ref`");
  const auto &read = returned(function_named(s, "cell_read"), 1);
  const auto &u = as<core::use>(&read, node_kind::use, "cell_read: `*c`");
  expect(u.from.root.kind == core::place_root::origin::temporary &&
             u.from.path.size() == 1 &&
             u.from.path[0].kind == core::projection::step::deref,
         "cell_read: `*c` is a deref of a temporary holding the conversion");
  const auto &conv = as<core::call>(u.from.root.value.get(), node_kind::call,
                                    "cell_read: the conversion");
  expect(conv.target.generic_args.size() == 1 &&
             s.checked.types.display(conv.target.generic_args[0]) == "int32",
         "cell_read: the conversion carries T in generic_args");
}

auto test_static_for(const session &s) -> void {
  const auto &f = function_named(s, "unrolled");
  // `var total`, three iteration blocks, `return`.
  expect(f.body->statements.size() == 5,
         "unrolled: one block per `static for` iteration");
  const auto &third = as<core::expr_stmt>(f.body->statements[3].get(),
                                          node_kind::expr_stmt, "unrolled");
  const auto &iteration = as<core::block>(third.value.get(), node_kind::block,
                                          "unrolled: iteration 2");
  const auto &bound = as<core::let>(iteration.statements.at(0).get(),
                                    node_kind::let, "unrolled: `let i`");
  const auto &two = as<core::literal>(bound.value.get(), node_kind::literal,
                                      "unrolled: `i` is a literal");
  expect(two.spelling == "2", "unrolled: the third iteration binds `i = 2`");

  const auto inline_facts = facts_of(function_named(s, "squares_static"));
  auto pushes = std::size_t{0};
  for (const auto *c : inline_facts.calls) {
    if (c->target.decl != nullptr && c->target.decl->name.starts_with("list::push")) {
      ++pushes;
    }
  }
  expect(pushes == 3, "squares_static: the inline form pushes each element");
}

auto test_static_if(const session &s) -> void {
  const auto &f = function_named(s, "pick$int8");
  const auto &taken = as<core::literal>(&returned(f, 0), node_kind::literal,
                                        "pick$int8: the taken branch");
  expect(taken.spelling == "1", "pick$int8: `static if` takes the int8 branch");
}

auto test_contracts(const session &s) -> void {
  const auto &f = function_named(s, "checked_half");
  as<core::contract_check>(f.body->statements.at(0).get(),
                           node_kind::contract_check, "checked_half: `pre`");
  expect(facts_of(f).contract_checks == 2,
         "checked_half: one `pre` and one `post` check");
  const auto &positive = facts_of(function_named(s, "make_positive"));
  expect(positive.contract_checks == 2,
         "make_positive: the invariant is checked at construction and after "
         "the field write");
}

auto test_uninit_len(const session &s) -> void {
  const auto facts = facts_of(function_named(s, "buffer_len"));
  expect(count_of(facts, "uninit_len") == 1,
         "buffer_len: `buf.len()` is `uninit_len`");
}

auto test_tasks(const session &s) -> void {
  const auto facts = facts_of(function_named(s, "tasks"));
  expect(count_of(facts, "task_block") == 2,
         "tasks: the `async:` and `on(cpu):` bodies are `task_block`s");
  expect(count_of(facts, "on_cpu") == 1, "tasks: `on(cpu)` is `on_cpu`");
  expect(count_of(facts, "await_par") == 1, "tasks: `await par` is `await_par`");
  expect(count_of(facts, "crew_new") == 1 && count_of(facts, "crew_end") == 1 &&
             count_of(facts, "crew_spawn") == 1,
         "tasks: the crew is made, spawned on and ended");
  expect(facts.lambdas.size() == 2 && facts.lambdas[0]->params.empty() &&
             facts.lambdas[0]->captures.size() == 1 &&
             facts.lambdas[0]->captures[0].how == core::capture::mode::copy,
         "tasks: a task body is a lambda capturing `base` by copy");
}

auto test_generic_template(const session &s) -> void {
  const auto &tmpl = function_named(s, "left_of");
  expect(tmpl.generic_params.size() == 1 &&
             s.checked.types.entry(tmpl.generic_params[0]).name == "T",
         "left_of: the template lists its type parameter `T`");
  const auto &read = as<core::use>(&returned(tmpl, 0), node_kind::use,
                                   "left_of: `p.left`");
  expect(read.from.path.size() == 1 &&
             read.from.path[0].kind == core::projection::step::field &&
             read.from.path[0].name == "left",
         "left_of: `p.left` on `pair[T]` is a `field` step");
  const auto *instance = static_cast<const core::function *>(nullptr);
  for (const auto &m : s.adapted.modules) {
    for (const auto &f : m->functions) {
      if (f->name.starts_with("left_of$")) {
        instance = f.get();
      }
    }
  }
  expect(instance != nullptr, "left_of: an instance is built");
  expect(instance->generic_params.empty() && instance->instance_of == &tmpl,
         std::format("{}: the instance points at its template", instance->name));
}

auto test_intrinsic_decl(const session &s) -> void {
  for (const auto &m : s.adapted.modules) {
    if (m->name != s.user_module) {
      continue;
    }
    const auto found = std::ranges::find(m->intrinsics, std::string("rt_panic"),
                                         &core::intrinsic_decl::name);
    expect(found != m->intrinsics.end(),
           "the user module lists its `intrinsic def rt_panic`");
    expect(!found->generic &&
               s.checked.types.entry(found->signature).kind ==
                   cinder::semantic::type_kind::fn_kind,
           "rt_panic: the declaration carries its `fn` signature");
    return;
  }
  fail("no user module");
}

auto test_verifier_accepts(const session &s) -> void {
  auto program = std::vector<const core::module *>{};
  for (const auto &m : s.adapted.modules) {
    if (m->name == s.user_module) {
      program.push_back(m.get());
    }
  }
  auto violations = core::verify(program, s.checked);
  // C8: the checker records no generic arguments for a call to an instance
  // (`left_of$int32` from `use_left`), so such a call has none while its
  // template has one parameter. `core_known_violations.txt` lists the same.
  std::erase_if(violations, [](const auto &v) {
    return v.rule == "C8" && v.function == "use_left";
  });
  for (const auto &v : violations) {
    std::cerr << v.rule << ' ' << v.function << ": " << v.message << '\n';
  }
  expect(violations.empty(), "the verifier accepts the adapted user module");
}

} // namespace

auto main() -> int {
  // `session` holds members with explicit default constructors.
  const auto owned = std::make_unique<session>();
  auto &s = *owned;
  build_session(s);
  test_scalar_body(s);
  test_struct_literal(s);
  test_method_receiver(s);
  test_match(s);
  test_alias_binding(s);
  test_if_let(s);
  test_while_let(s);
  test_let_else(s);
  test_or_else(s);
  test_try(s);
  test_same_module_call(s);
  test_call_through_value(s);
  test_pointer_index(s);
  test_pointer_cast(s);
  test_open_range(s);
  test_element_binding(s);
  test_range_loop(s);
  test_array_loop(s);
  test_comprehension(s);
  test_range_index(s);
  test_range_value(s);
  test_lambda_captures(s);
  test_generic_lambda(s);
  test_generator(s);
  test_async(s);
  test_interpolation(s);
  test_cell_deref(s);
  test_compound_assign(s);
  test_static_for(s);
  test_static_if(s);
  test_contracts(s);
  test_uninit_len(s);
  test_tasks(s);
  test_generic_template(s);
  test_intrinsic_decl(s);
  test_verifier_accepts(s);
  std::cout << "adapter_test: ok\n";
  return 0;
}
