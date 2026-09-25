#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/hir/inline.h"
#include "src/hir/lower.h"
#include "src/hir/nodes.h"
#include "src/parser/diagnostic.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/check.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;
namespace hir = kira::hir;

/// A checked, lowered, *inlined* single-file program. Owns everything the
/// HIR borrows from.
struct inlined_program {
  kira::source_manager sources;
  kira::diagnostic_bag diag{};
  kira::ast::ptr<kira::ast::file> ast_file;
  kira::semantic::checked_types checked;
  hir::ptr_vec<hir::hir_module> modules;
  hir::inline_stats stats;
};

auto inline_program(const std::string &text) -> inlined_program {
  auto program = inlined_program{};
  const auto file_id = program.sources.add_file("sample.kira", text);
  expect(file_id.has_value(), "expected fixture source to register");
  const auto *file = program.sources.get(*file_id);
  expect(file != nullptr, "expected registered fixture source");

  auto lexer = kira::lexer(file->source(), file->id(), program.diag);
  auto parser = kira::parser(lexer.tokenize(), file->id(), program.diag);
  program.ast_file = parser.parse_file();
  expect(program.diag.error_count() == 0, "expected fixture to parse cleanly");

  auto file_has_errors =
      std::vector<bool>(static_cast<size_t>(*file_id) + 1, false);
  const auto parsed_modules = std::vector<kira::semantic::parsed_module>{
      kira::semantic::parsed_module{.file_id = *file_id,
                                    .ast_file = program.ast_file.get()},
  };
  program.checked = kira::semantic::check_program(parsed_modules, program.diag,
                                                  file_has_errors);
  expect(program.diag.error_count() == 0, "expected fixture to check cleanly");

  auto lowered = hir::lower_module(*program.ast_file, "sample", program.checked);
  expect(lowered.has_value(), "expected fixture to lower to HIR");
  program.modules.push_back(std::move(*lowered));
  program.stats =
      hir::inline_small_calls(program.modules, program.checked.types);
  return program;
}

auto function_named(const inlined_program &program, std::string_view name)
    -> const hir::hir_function & {
  for (const auto &fn : program.modules.front()->functions) {
    if (fn->name == name) {
      return *fn;
    }
  }
  kira::testing::fail(std::format("expected a function named `{}`", name));
}

/// Pre-order walk over `node`'s subtree, following the child fields the
/// fixtures below produce.
template <typename F> auto walk(const hir::hir_node &node, F &&visit) -> void {
  visit(node);
  const auto child = [&](const auto &ptr) -> void {
    if (ptr != nullptr) {
      walk(*ptr, visit);
    }
  };
  switch (node.kind) {
  case hir::hir_node_kind::hir_block:
    for (const auto &stmt : dynamic_cast<const hir::hir_block &>(node).stmts) {
      child(stmt);
    }
    return;
  case hir::hir_node_kind::hir_let:
    child(dynamic_cast<const hir::hir_let &>(node).initializer);
    return;
  case hir::hir_node_kind::hir_expr_stmt:
    child(dynamic_cast<const hir::hir_expr_stmt &>(node).expr);
    return;
  case hir::hir_node_kind::hir_return:
    child(dynamic_cast<const hir::hir_return &>(node).value);
    return;
  case hir::hir_node_kind::hir_binary: {
    const auto &n = dynamic_cast<const hir::hir_binary &>(node);
    child(n.lhs);
    child(n.rhs);
    return;
  }
  case hir::hir_node_kind::hir_call: {
    const auto &n = dynamic_cast<const hir::hir_call &>(node);
    child(n.callee);
    for (const auto &arg : n.args) {
      child(arg);
    }
    return;
  }
  case hir::hir_node_kind::hir_if: {
    const auto &n = dynamic_cast<const hir::hir_if &>(node);
    for (const auto &branch : n.branches) {
      child(branch.condition);
      child(branch.body);
    }
    child(n.else_body);
    return;
  }
  case hir::hir_node_kind::hir_while: {
    const auto &n = dynamic_cast<const hir::hir_while &>(node);
    child(n.condition);
    child(n.body);
    child(n.step);
    return;
  }
  case hir::hir_node_kind::hir_variant_init:
    for (const auto &arg :
         dynamic_cast<const hir::hir_variant_init &>(node).args) {
      child(arg);
    }
    return;
  default:
    return;
  }
}

/// Every call in `fn` whose callee is the function `name`.
auto calls_to(const hir::hir_function &fn, std::string_view name)
    -> std::vector<const hir::hir_call *> {
  auto found = std::vector<const hir::hir_call *>{};
  walk(*fn.body, [&](const hir::hir_node &n) -> void {
    if (n.kind != hir::hir_node_kind::hir_call) {
      return;
    }
    const auto &call = dynamic_cast<const hir::hir_call &>(n);
    if (call.callee->kind == hir::hir_node_kind::hir_local_ref &&
        dynamic_cast<const hir::hir_local_ref &>(*call.callee).name == name) {
      found.push_back(&call);
    }
  });
  return found;
}

/// The initializer of `fn`'s top-level `let` named `name`.
auto let_initializer(const hir::hir_function &fn, std::string_view name)
    -> const hir::hir_expr & {
  for (const auto &stmt : fn.body->stmts) {
    if (stmt->kind == hir::hir_node_kind::hir_let) {
      const auto &let = dynamic_cast<const hir::hir_let &>(*stmt);
      if (let.name == name) {
        return *let.initializer;
      }
    }
  }
  kira::testing::fail(std::format("expected a `let {}`", name));
}

auto test_guard_clause_becomes_if_else_value() -> void {
  const auto program = inline_program(R"(module sample

def safe_div(a: int32, b: int32) -> option[int32]:
    if b == 0:
        return @none
    return @some(a / b)

def main() -> int32:
    let r = safe_div(84, 2)
    return 0
)");
  const auto &main_fn = function_named(program, "main");
  expect(calls_to(main_fn, "safe_div").empty(),
         "expected the call to `safe_div` to be inlined");
  const auto &init = let_initializer(main_fn, "r");
  expect(init.kind == hir::hir_node_kind::hir_block,
         "expected the call site to become a block");
  const auto &block = dynamic_cast<const hir::hir_block &>(init);
  expect(block.stmts.size() == 3,
         "expected one `let` per argument, then the body's value");
  expect(block.stmts[0]->kind == hir::hir_node_kind::hir_let &&
             block.stmts[1]->kind == hir::hir_node_kind::hir_let,
         "expected both arguments bound first, in order");
  expect(block.stmts[2]->kind == hir::hir_node_kind::hir_expr_stmt,
         "expected the body to end in the block's value");
  const auto &value = *dynamic_cast<const hir::hir_expr_stmt &>(*block.stmts[2])
                           .expr;
  expect(value.kind == hir::hir_node_kind::hir_if,
         "expected the guard clause and the code after it to become one "
         "if/else value");
  const auto &iff = dynamic_cast<const hir::hir_if &>(value);
  expect(iff.else_body != nullptr && iff.branches.size() == 1,
         "expected the code after the guard to move into the `else`");
  expect(iff.type == init.type,
         "expected the if/else to carry the call's result type");
}

auto test_copies_get_fresh_symbols() -> void {
  const auto program = inline_program(R"(module sample

def sq_plus(x: int32) -> int32:
    let y = x * x
    return y + 1

def main() -> int32:
    let n = 4
    return sq_plus(2) + sq_plus(n)
)");
  const auto &main_fn = function_named(program, "main");
  expect(calls_to(main_fn, "sq_plus").empty(),
         "expected both calls to `sq_plus` to be inlined");
  auto seen = std::unordered_set<hir::symbol_id>{};
  auto lets = 0;
  walk(*main_fn.body, [&](const hir::hir_node &n) -> void {
    if (n.kind == hir::hir_node_kind::hir_let) {
      ++lets;
      const auto symbol = dynamic_cast<const hir::hir_let &>(n).symbol;
      expect(seen.insert(symbol).second,
             std::format("expected every `let` in `main` to bind its own "
                         "symbol, but {} is bound twice",
                         symbol));
    }
  });
  // `n`, plus `x` and `y` for each of the two copies.
  expect(lets == 5, std::format("expected 5 `let`s in `main`, found {}", lets));
}

auto test_recursion_is_bounded() -> void {
  const auto program = inline_program(R"(module sample

def fact(n: int32) -> int32:
    if n <= 1:
        return 1
    return n * fact(n - 1)

def main() -> int32:
    return fact(5)
)");
  const auto &main_fn = function_named(program, "main");
  expect(calls_to(main_fn, "fact").size() == 1,
         "expected a recursive callee to be expanded a bounded number of "
         "times, ending in exactly one real call");
  const auto &fact_fn = function_named(program, "fact");
  auto copies = 0;
  walk(*fact_fn.body, [&](const hir::hir_node &n) -> void {
    copies += n.kind == hir::hir_node_kind::hir_let ? 1 : 0;
  });
  expect(copies == 0 && calls_to(fact_fn, "fact").size() == 1,
         "expected a recursive function never to be inlined into itself");
}

auto test_closure_parameter_stays_a_call() -> void {
  const auto program = inline_program(R"(module sample

def apply_twice(f: fn(int32) -> int32, x: int32) -> int32:
    return f(f(x))

def main() -> int32:
    let triple: fn(int32) -> int32 = k => k * 3
    return apply_twice(triple, 2)
)");
  const auto &main_fn = function_named(program, "main");
  expect(calls_to(main_fn, "apply_twice").empty(),
         "expected `apply_twice` itself to be inlined");
  expect(calls_to(main_fn, "f").size() == 2,
         "expected both calls through the `fn` parameter to stay calls");
}

auto test_declines_what_it_cannot_rewrite() -> void {
  const auto program = inline_program(R"(module sample

def first_over(limit: int32) -> int32:
    var i = 0
    while i < 100:
        if i * i > limit:
            return i
        i = i + 1
    return -1

def main() -> int32:
    return first_over(50)
)");
  const auto &main_fn = function_named(program, "main");
  expect(calls_to(main_fn, "first_over").size() == 1,
         "expected a callee returning from inside a loop to stay a call");
  expect(program.stats.call_sites == 0,
         "expected nothing to be inlined in this program");
}

auto test_copied_function_references_name_their_module() -> void {
  const auto program = inline_program(R"(module sample

def first_over(limit: int32) -> int32:
    var i = 0
    while i < 100:
        if i * i > limit:
            return i
        i = i + 1
    return -1

def wrapper(limit: int32) -> int32:
    return first_over(limit) + 1

def main() -> int32:
    return wrapper(50)
)");
  const auto &main_fn = function_named(program, "main");
  expect(calls_to(main_fn, "wrapper").empty(),
         "expected `wrapper` to be inlined into `main`");
  const auto copied = calls_to(main_fn, "first_over");
  expect(copied.size() == 1, "expected the copy to still call `first_over`");
  const auto &ref =
      dynamic_cast<const hir::hir_local_ref &>(*copied.front()->callee);
  expect(ref.owner_module == std::optional<std::string>{"sample"},
         "expected a bare-name call copied out of its module to be "
         "qualified with that module");
}

} // namespace

/// A callee with an `uninit` buffer is never copied into its caller: that
/// would move the buffer into the caller's frame, where it counts against
/// the caller's `hir::k_max_frame_stack_bytes` budget instead of its own —
/// a program that checked clean could then fail after inlining. The same
/// buffer also keeps its owner from making tail calls, since a callee may
/// still point into it.
auto test_stack_buffer_callee_stays_a_call() -> void {
  const auto program = inline_program(R"(module sample

machine def scratch() -> int64:
    var buf = uninit[int64, 2]()
    buf[0] = 1
    return buf[0]

machine def owner() -> int64:
    var buf = uninit[int64, 2]()
    buf[0] = 2
    return scratch()

def main() -> int64:
    return scratch()
)");
  const auto main_calls = calls_to(function_named(program, "main"), "scratch");
  expect(main_calls.size() == 1,
         "expected a callee with an `uninit` buffer to stay a call");
  expect(main_calls.front()->is_tail_call,
         "expected a call out of a buffer-free frame to stay a tail call");
  const auto owner_calls =
      calls_to(function_named(program, "owner"), "scratch");
  expect(owner_calls.size() == 1, "expected `owner` to call `scratch`");
  expect(!owner_calls.front()->is_tail_call,
         "expected no tail call out of a frame that owns an `uninit` buffer");
}

auto main() -> int {
  try {
    test_guard_clause_becomes_if_else_value();
    test_copies_get_fresh_symbols();
    test_recursion_is_bounded();
    test_closure_parameter_stays_a_call();
    test_declines_what_it_cannot_rewrite();
    test_copied_function_references_name_their_module();
    test_stack_buffer_callee_stays_a_call();
  } catch (const std::exception &ex) {
    std::cerr << "inline_test failed: unhandled exception: " << ex.what()
              << "\n";
    return EXIT_FAILURE;
  }
  std::cout << "inline_test passed\n";
  return EXIT_SUCCESS;
}
