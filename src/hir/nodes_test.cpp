#include <cstdlib>
#include <exception>
#include <iostream>
#include <utility>

#include "src/hir/ids.h"
#include "src/hir/nodes.h"
#include "src/k-parser/source_location.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using kira::source_span;
using kira::testing::expect;
namespace hir = kira::hir;

// Hand-builds the HIR for `def add(x: int32, y: int32) -> int32: return x +
// y`, since lowering (src/hir/lower.cpp) doesn't exist yet — this exercises
// only the node shapes themselves.
auto test_builds_add_function_by_hand() -> void {
  auto types = kira::semantic::type_table{};
  const auto int32_type = types.builtin("int32");

  const auto span = source_span{.start = 0, .end = 1};

  auto x_ref = hir::make<hir::hir_local_ref>(span, int32_type, 0, "x");
  auto y_ref = hir::make<hir::hir_local_ref>(span, int32_type, 1, "y");
  auto sum =
      hir::make<hir::hir_binary>(span, int32_type, kira::ast::binary_op::Add,
                                 std::move(x_ref), std::move(y_ref));

  auto ret = hir::make<hir::hir_return>(span, std::move(sum));

  auto body_stmts = hir::ptr_vec<hir::hir_node>{};
  body_stmts.push_back(std::move(ret));
  auto body =
      hir::make<hir::hir_block>(span, int32_type, std::move(body_stmts));

  auto params = std::vector<hir::hir_param>{
      hir::hir_param{.symbol = 0, .name = "x", .type = int32_type},
      hir::hir_param{.symbol = 1, .name = "y", .type = int32_type},
  };

  auto function = hir::make<hir::hir_function>(span, "add", std::move(params),
                                               int32_type, std::move(body));

  expect(function->name == "add", "expected lowered function name to survive");
  expect(function->params.size() == 2, "expected two lowered parameters");
  expect(function->params[0].name == "x", "expected first parameter name x");
  expect(function->return_type == int32_type,
         "expected function return type to be int32");

  expect(function->body->kind == hir::hir_node_kind::hir_block,
         "expected function body to be a hir_block");
  expect(function->body->stmts.size() == 1,
         "expected function body to hold one statement");

  const auto *returned = function->body->stmts.front().get();
  expect(returned->kind == hir::hir_node_kind::hir_return,
         "expected the lone statement to be a hir_return");
  const auto &return_stmt = static_cast<const hir::hir_return &>(*returned);

  expect(return_stmt.value != nullptr, "expected return to carry a value");
  expect(return_stmt.value->kind == hir::hir_node_kind::hir_binary,
         "expected returned value to be a hir_binary");
  expect(return_stmt.value->type == int32_type,
         "expected the sum's checked type to be int32");

  const auto &sum_expr =
      static_cast<const hir::hir_binary &>(*return_stmt.value);
  expect(sum_expr.op == kira::ast::binary_op::Add,
         "expected the operator to be addition");
  expect(sum_expr.lhs->kind == hir::hir_node_kind::hir_local_ref,
         "expected the left operand to be a local reference");
  const auto &lhs_ref = static_cast<const hir::hir_local_ref &>(*sum_expr.lhs);
  expect(lhs_ref.symbol == 0, "expected left operand to reference symbol 0");
  expect(lhs_ref.name == "x", "expected left operand's debug name to be x");
}

} // namespace

auto main() -> int {
  try {
    test_builds_add_function_by_hand();
  } catch (const std::exception &ex) {
    std::cerr << "nodes_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
