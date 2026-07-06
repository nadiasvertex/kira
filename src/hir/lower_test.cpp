#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "src/hir/lower.h"
#include "src/hir/nodes.h"
#include "src/k-parser/diagnostic.h"
#include "src/k-parser/lexer.h"
#include "src/k-parser/parser.h"
#include "src/k-parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/check.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;
using kira::testing::fail;
namespace hir = kira::hir;

// Parses and checks one fixture source, returning the AST root, the checked
// types, and the requested function's declaration for the caller to feed
// into `hir::lower_function`/`hir::lower_module`.
struct checked_fixture {
  kira::source_manager sources;
  kira::diagnostic_bag diag{};
  kira::ast::ptr<kira::ast::file> ast_file;
  kira::semantic::checked_types checked;
};

auto check_fixture(const std::string &text) -> checked_fixture {
  auto fixture = checked_fixture{};
  const auto file_id = fixture.sources.add_file("sample.kira", text);
  expect(file_id.has_value(), "expected fixture source to register");

  const auto *file = fixture.sources.get(*file_id);
  expect(file != nullptr, "expected registered fixture source");

  auto lexer = kira::lexer(file->source(), file->id(), fixture.diag);
  auto tokens = lexer.tokenize();
  auto parser = kira::parser(std::move(tokens), file->id(), fixture.diag);
  fixture.ast_file = parser.parse_file();
  expect(fixture.diag.error_count() == 0, "expected fixture to parse cleanly");

  auto file_has_errors =
      std::vector<bool>(static_cast<size_t>(*file_id) + 1, false);
  const auto parsed_modules = std::vector<kira::semantic::parsed_module>{
      kira::semantic::parsed_module{.file_id = *file_id,
                                    .ast_file = fixture.ast_file.get()},
  };
  fixture.checked = kira::semantic::check_program(parsed_modules, fixture.diag,
                                                  file_has_errors);
  expect(fixture.diag.error_count() == 0, "expected fixture to check cleanly");
  return fixture;
}

auto find_func(const kira::ast::file &file, std::string_view name)
    -> const kira::ast::func_decl & {
  for (const auto &item : file.items) {
    if (item != nullptr && item->kind == kira::ast::node_kind::func_decl) {
      const auto &decl = dynamic_cast<const kira::ast::func_decl &>(*item);
      if (decl.name == name) {
        return decl;
      }
    }
  }
  fail("expected to find a function declaration by name");
}

auto test_lowers_fully_annotated_function() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def add(x: int32, y: int32) -> int32:\n"
                               "    return x + y\n");
  const auto &decl = find_func(*fixture.ast_file, "add");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a fully-annotated function to lower");

  const auto &function = **result;
  expect(function.name == "add", "expected lowered function name to survive");
  expect(function.params.size() == 2, "expected two lowered parameters");

  const auto int32_type = fixture.checked.types.builtin("int32");
  expect(function.params[0].type == int32_type,
         "expected parameter x to be resolved to int32");
  expect(function.params[1].type == int32_type,
         "expected parameter y to be resolved to int32");
  expect(function.return_type == int32_type,
         "expected the declared return type to be resolved to int32");

  expect(function.body->stmts.size() == 1,
         "expected the function body to hold one statement");
  const auto *stmt = function.body->stmts.front().get();
  expect(stmt->kind == hir::hir_node_kind::hir_return,
         "expected the lone statement to be a hir_return");
  const auto &ret = dynamic_cast<const hir::hir_return &>(*stmt);
  expect(ret.value != nullptr, "expected return to carry a value");
  expect(ret.value->kind == hir::hir_node_kind::hir_binary,
         "expected the returned value to be a hir_binary");
  expect(ret.value->type == int32_type,
         "expected the sum's checked type to be int32");

  const auto &sum = dynamic_cast<const hir::hir_binary &>(*ret.value);
  expect(sum.lhs->kind == hir::hir_node_kind::hir_local_ref,
         "expected the left operand to be a local reference");
  expect(sum.rhs->kind == hir::hir_node_kind::hir_local_ref,
         "expected the right operand to be a local reference");
  const auto &lhs = dynamic_cast<const hir::hir_local_ref &>(*sum.lhs);
  const auto &rhs = dynamic_cast<const hir::hir_local_ref &>(*sum.rhs);
  expect(lhs.name == "x", "expected the left operand to reference x");
  expect(rhs.name == "y", "expected the right operand to reference y");
  expect(lhs.symbol != rhs.symbol,
         "expected distinct parameters to receive distinct lowering-local ids");
}

auto test_rejects_unannotated_parameter_with_specific_error() -> void {
  // Per spec/kira-reference.md, `x` here is legally unannotated and the
  // checker infers its usage type locally — but the first lowering
  // milestone explicitly only lowers *explicitly* annotated signatures
  // (spec/typed-ir-design.md Decision 1), so this must be rejected, not
  // silently skipped or lowered with a guessed type.
  auto fixture = check_fixture("module sample\n"
                               "def double(x) -> int32:\n"
                               "    return x * 2\n");
  const auto &decl = find_func(*fixture.ast_file, "double");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(!result.has_value(),
         "expected a function with an unannotated parameter to be rejected");
  expect(result.error().kind == hir::lowering_error_kind::unannotated_parameter,
         "expected the specific unannotated_parameter error kind");
}

auto test_rejects_function_without_declared_return_type() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def add(x: int32, y: int32):\n"
                               "    return x + y\n");
  const auto &decl = find_func(*fixture.ast_file, "add");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(!result.has_value(),
         "expected a function with no declared return type to be rejected");
  expect(result.error().kind == hir::lowering_error_kind::missing_return_type,
         "expected the specific missing_return_type error kind");
}

auto test_preserves_source_spans() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def add(x: int32, y: int32) -> int32:\n"
                               "    return x + y\n");
  const auto &decl = find_func(*fixture.ast_file, "add");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a fully-annotated function to lower");

  const auto &function = **result;
  expect(function.span.start == decl.span.start &&
             function.span.end == decl.span.end,
         "expected the function's span to match its originating declaration");

  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &return_stmt =
      dynamic_cast<const kira::ast::return_stmt &>(*decl.body_stmts.front());
  expect(
      ret.span.start == return_stmt.span.start &&
          ret.span.end == return_stmt.span.end,
      "expected the lowered return statement's span to match the AST return");
  expect(ret.value->span.start == return_stmt.value->span.start &&
             ret.value->span.end == return_stmt.value->span.end,
         "expected the lowered sum's span to match the AST `x + y`");
}

auto test_lowers_compact_expression_body() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def identity(x: int32) -> int32: x\n");
  const auto &decl = find_func(*fixture.ast_file, "identity");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a compact-body function to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 1,
         "expected the compact body to lower to a single implicit return");
  expect(function.body->stmts.front()->kind == hir::hir_node_kind::hir_return,
         "expected the compact body's value to become a hir_return");
}

auto test_lowers_if_expression_and_module() -> void {
  const auto fixture = check_fixture("module sample\n"
                                     "def max2(a: int32, b: int32) -> int32:\n"
                                     "    return (if a > b: a else: b)\n");

  auto module_result =
      hir::lower_module(*fixture.ast_file, "sample", fixture.checked);
  expect(module_result.has_value(), "expected the module to lower");
  expect((*module_result)->functions.size() == 1,
         "expected exactly one lowered function in the module");
  expect((*module_result)->functions.front()->name == "max2",
         "expected the lowered function to be max2");

  const auto &function = *(*module_result)->functions.front();
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_if,
         "expected the returned value to be a hir_if");
  const auto &if_expr = dynamic_cast<const hir::hir_if &>(*ret.value);
  expect(if_expr.branches.size() == 1, "expected a single if branch");
  expect(if_expr.else_body != nullptr, "expected an else block to be present");
}

auto test_rejects_generic_function() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def identity[T](x: T) -> T:\n"
                               "    return x\n");
  const auto &decl = find_func(*fixture.ast_file, "identity");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(!result.has_value(), "expected a generic function to be rejected");
  expect(result.error().kind == hir::lowering_error_kind::unsupported_construct,
         "expected the specific unsupported_construct error kind");
}

auto test_lowers_match_with_literal_and_wildcard_patterns() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def classify(x: int32) -> int32:\n"
                               "    return match x:\n"
                               "        0 => 1\n"
                               "        1 => 2\n"
                               "        _ => 0\n");
  const auto &decl = find_func(*fixture.ast_file, "classify");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a literal/wildcard match to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_match,
         "expected the returned value to be a hir_match");

  const auto &match = dynamic_cast<const hir::hir_match &>(*ret.value);
  expect(match.arms.size() == 3, "expected three match arms");
  expect(match.arms[0].pattern->kind == hir::hir_node_kind::hir_literal_pattern,
         "expected the first arm's pattern to be a literal pattern");
  expect(match.arms[1].pattern->kind == hir::hir_node_kind::hir_literal_pattern,
         "expected the second arm's pattern to be a literal pattern");
  expect(match.arms[2].pattern->kind ==
             hir::hir_node_kind::hir_wildcard_pattern,
         "expected the third arm's pattern to be the wildcard pattern");
  const auto &first_literal =
      dynamic_cast<const hir::hir_literal_pattern &>(*match.arms[0].pattern);
  expect(first_literal.value == "0", "expected the first arm to match `0`");
}

auto test_lowers_plain_binding_arm_as_wildcard_plus_let() -> void {
  // A bare name in arm position (`y => ...`) always matches — it isn't a
  // structural pattern at all, so it desugars to a wildcard pattern plus a
  // synthetic `hir_let` bound to the match subject (Decision 6, item 2),
  // instead of a dedicated binding-pattern node kind.
  auto fixture = check_fixture("module sample\n"
                               "def echo(x: int32) -> int32:\n"
                               "    return match x:\n"
                               "        y => y\n");
  const auto &decl = find_func(*fixture.ast_file, "echo");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a plain-binding match arm to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &match = dynamic_cast<const hir::hir_match &>(*ret.value);
  expect(match.arms.size() == 1, "expected one match arm");
  expect(match.arms[0].pattern->kind ==
             hir::hir_node_kind::hir_wildcard_pattern,
         "expected the binding arm's pattern to lower to a wildcard");

  expect(match.arms[0].body->stmts.size() == 2,
         "expected the synthetic let plus the arm's yielded expression");
  const auto *let_stmt = match.arms[0].body->stmts.front().get();
  expect(let_stmt->kind == hir::hir_node_kind::hir_let,
         "expected the arm body to start with the synthetic binding let");
  const auto &let = dynamic_cast<const hir::hir_let &>(*let_stmt);
  expect(let.name == "y", "expected the synthetic let to bind `y`");
  expect(let.initializer->kind == hir::hir_node_kind::hir_local_ref,
         "expected the let's initializer to reference the match subject");
  const auto &subject_ref =
      dynamic_cast<const hir::hir_local_ref &>(*let.initializer);
  expect(subject_ref.symbol == match.subject_symbol,
         "expected the let to reference the match's subject_symbol");
}

auto test_lowers_pattern_alias_to_synthetic_let() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def classify(x: int32) -> int32:\n"
                               "    return match x:\n"
                               "        (0 | 1) as small => small\n"
                               "        _ => x\n");
  const auto &decl = find_func(*fixture.ast_file, "classify");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a pattern-alias match arm to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &match = dynamic_cast<const hir::hir_match &>(*ret.value);

  // The alias doesn't change the structural pattern: it's still the `0 | 1`
  // dispatch, plus one extra synthetic let for `small`.
  expect(match.arms[0].pattern->kind == hir::hir_node_kind::hir_or_pattern,
         "expected the aliased arm's structural pattern to still be the "
         "`0 | 1` or-pattern");
  expect(match.arms[0].body->stmts.size() == 2,
         "expected the synthetic alias let plus the arm's yielded expression");
  const auto &let =
      dynamic_cast<const hir::hir_let &>(*match.arms[0].body->stmts.front());
  expect(let.name == "small", "expected the synthetic let to bind `small`");
  const auto &subject_ref =
      dynamic_cast<const hir::hir_local_ref &>(*let.initializer);
  expect(subject_ref.symbol == match.subject_symbol,
         "expected the alias let to reference the match's subject_symbol");
}

auto test_rejects_binding_inside_or_pattern_alternative() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def classify(x: int32) -> int32:\n"
                               "    return match x:\n"
                               "        0 | y => y\n"
                               "        _ => x\n");
  const auto &decl = find_func(*fixture.ast_file, "classify");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(!result.has_value(),
         "expected a binding inside a `|` alternative to be rejected");
  expect(result.error().kind == hir::lowering_error_kind::unsupported_construct,
         "expected the specific unsupported_construct error kind");
}

auto test_lowers_struct_pattern_field_destructuring() -> void {
  auto fixture = check_fixture("module sample\n"
                               "type container = { pub value: int32 }\n"
                               "def unwrap(b: container) -> int32:\n"
                               "    return match b:\n"
                               "        { value: v } => v\n");
  const auto &decl = find_func(*fixture.ast_file, "unwrap");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected struct field destructuring to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &match = dynamic_cast<const hir::hir_match &>(*ret.value);
  expect(match.arms[0].pattern->kind == hir::hir_node_kind::hir_struct_pattern,
         "expected the arm's pattern to be a hir_struct_pattern");

  const auto &struct_pat =
      dynamic_cast<const hir::hir_struct_pattern &>(*match.arms[0].pattern);
  expect(struct_pat.fields.size() == 1, "expected one destructured field");
  expect(struct_pat.fields[0].name == "value",
         "expected the destructured field to be `value`");
  expect(struct_pat.fields[0].pattern->kind ==
             hir::hir_node_kind::hir_wildcard_pattern,
         "expected the explicit `v` sub-pattern to desugar to a wildcard");

  expect(match.arms[0].body->stmts.size() == 2,
         "expected the synthetic let plus the arm's yielded expression");
  const auto &let =
      dynamic_cast<const hir::hir_let &>(*match.arms[0].body->stmts.front());
  expect(let.name == "v", "expected the synthetic let to bind `v`");
  expect(let.initializer->kind == hir::hir_node_kind::hir_field,
         "expected the let to be initialized from a field projection");
  const auto &field_ref =
      dynamic_cast<const hir::hir_field &>(*let.initializer);
  expect(field_ref.field_name == "value",
         "expected the field projection to read `value`");
}

auto test_lowers_struct_pattern_shorthand_field() -> void {
  auto fixture = check_fixture("module sample\n"
                               "type container = { pub value: int32 }\n"
                               "def unwrap(b: container) -> int32:\n"
                               "    return match b:\n"
                               "        { value } => value\n");
  const auto &decl = find_func(*fixture.ast_file, "unwrap");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(),
         "expected a shorthand struct field pattern to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &match = dynamic_cast<const hir::hir_match &>(*ret.value);
  const auto &struct_pat =
      dynamic_cast<const hir::hir_struct_pattern &>(*match.arms[0].pattern);
  expect(struct_pat.fields[0].pattern->kind ==
             hir::hir_node_kind::hir_wildcard_pattern,
         "expected the shorthand field to desugar to a wildcard");

  const auto &let =
      dynamic_cast<const hir::hir_let &>(*match.arms[0].body->stmts.front());
  expect(let.name == "value", "expected the synthetic let to bind `value`");
  expect(let.initializer->kind == hir::hir_node_kind::hir_field,
         "expected the shorthand field to still project via hir_field");
}

auto test_lowers_tuple_pattern_destructuring() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def first(pair: (int32, str)) -> int32:\n"
                               "    return match pair:\n"
                               "        (a, _) => a\n");
  const auto &decl = find_func(*fixture.ast_file, "first");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected tuple pattern destructuring to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &match = dynamic_cast<const hir::hir_match &>(*ret.value);
  expect(match.arms[0].pattern->kind == hir::hir_node_kind::hir_tuple_pattern,
         "expected the arm's pattern to be a hir_tuple_pattern");

  const auto &tuple_pat =
      dynamic_cast<const hir::hir_tuple_pattern &>(*match.arms[0].pattern);
  expect(tuple_pat.elements.size() == 2, "expected two tuple slots");
  expect(tuple_pat.elements[0]->kind ==
             hir::hir_node_kind::hir_wildcard_pattern,
         "expected the bound first slot to desugar to a wildcard");
  expect(tuple_pat.elements[1]->kind ==
             hir::hir_node_kind::hir_wildcard_pattern,
         "expected the plain `_` slot to already be a wildcard");

  const auto &let =
      dynamic_cast<const hir::hir_let &>(*match.arms[0].body->stmts.front());
  expect(let.name == "a", "expected the synthetic let to bind `a`");
  expect(let.initializer->kind == hir::hir_node_kind::hir_tuple_index,
         "expected the let to be initialized from a tuple-index projection");
  const auto &tuple_ref =
      dynamic_cast<const hir::hir_tuple_index &>(*let.initializer);
  expect(tuple_ref.index == 0, "expected the projection to read slot 0");
}

auto test_lowers_constructor_pattern_destructuring() -> void {
  auto fixture = check_fixture("module sample\n"
                               "type shape = @circle(int32) | @square(int32)\n"
                               "def area_hint(s: shape) -> int32:\n"
                               "    return match s:\n"
                               "        @circle(r) => r\n"
                               "        @square(side) => side\n");
  const auto &decl = find_func(*fixture.ast_file, "area_hint");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(),
         "expected sum-type constructor destructuring to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &match = dynamic_cast<const hir::hir_match &>(*ret.value);
  expect(match.arms[0].pattern->kind ==
             hir::hir_node_kind::hir_constructor_pattern,
         "expected the arm's pattern to be a hir_constructor_pattern");

  const auto &ctor_pat = dynamic_cast<const hir::hir_constructor_pattern &>(
      *match.arms[0].pattern);
  expect(ctor_pat.variant_name == "circle",
         "expected the constructor pattern's variant to be `circle`");
  expect(ctor_pat.args.size() == 1, "expected one destructured payload arg");
  expect(ctor_pat.args[0]->kind == hir::hir_node_kind::hir_wildcard_pattern,
         "expected the bound payload arg to desugar to a wildcard");

  const auto &let =
      dynamic_cast<const hir::hir_let &>(*match.arms[0].body->stmts.front());
  expect(let.name == "r", "expected the synthetic let to bind `r`");
  expect(
      let.initializer->kind == hir::hir_node_kind::hir_variant_payload,
      "expected the let to be initialized from a variant-payload projection");
  const auto &payload_ref =
      dynamic_cast<const hir::hir_variant_payload &>(*let.initializer);
  expect(payload_ref.variant_name == "circle",
         "expected the payload projection's variant to be `circle`");
  expect(payload_ref.index == 0,
         "expected the payload projection to read slot 0");
}

auto test_lowers_option_and_result_pattern_sugar() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "def unwrap_or_zero(x: option[int32]) -> int32:\n"
                    "    return match x:\n"
                    "        @some(v) => v\n"
                    "        @none => 0\n");
  const auto &decl = find_func(*fixture.ast_file, "unwrap_or_zero");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected option pattern sugar to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &match = dynamic_cast<const hir::hir_match &>(*ret.value);

  // `@some(v)` parses as a plain constructor pattern (not the dedicated
  // `ast::option_pattern` sugar node, which is only for bare `some(...)`
  // without `@`) — still lowers to the same hir_constructor_pattern shape.
  const auto &some_pat = dynamic_cast<const hir::hir_constructor_pattern &>(
      *match.arms[0].pattern);
  expect(some_pat.variant_name == "some",
         "expected the first arm's variant to be `some`");
  expect(some_pat.args.size() == 1, "expected `some` to destructure one arg");

  const auto &none_pat = dynamic_cast<const hir::hir_constructor_pattern &>(
      *match.arms[1].pattern);
  expect(none_pat.variant_name == "none",
         "expected the second arm's variant to be `none`");
  expect(none_pat.args.empty(), "expected `none` to have no payload args");
}

auto test_lowers_range_pattern() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def bucket(x: int32) -> int32:\n"
                               "    return match x:\n"
                               "        0..10 => 1\n"
                               "        _ => 0\n");
  const auto &decl = find_func(*fixture.ast_file, "bucket");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a range pattern to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &match = dynamic_cast<const hir::hir_match &>(*ret.value);
  expect(match.arms[0].pattern->kind == hir::hir_node_kind::hir_range_pattern,
         "expected the first arm's pattern to be a hir_range_pattern");
  const auto &range_pat =
      dynamic_cast<const hir::hir_range_pattern &>(*match.arms[0].pattern);
  expect(range_pat.start != nullptr,
         "expected the range to have a start bound");
  expect(range_pat.end != nullptr, "expected the range to have an end bound");
  expect(!range_pat.inclusive, "expected `..` to be exclusive");
}

auto test_lowers_plain_let_as_single_statement() -> void {
  // Regression guard: a plain `let name = expr` must keep lowering to
  // exactly one `hir_let` — the fast path in the `let_stmt` case — not the
  // two-statement synthetic-subject form destructuring patterns need.
  auto fixture = check_fixture("module sample\n"
                               "def double(x: int32) -> int32:\n"
                               "    let y = x\n"
                               "    return y\n");
  const auto &decl = find_func(*fixture.ast_file, "double");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a plain let binding to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 2,
         "expected exactly one let statement plus the return");
  expect(function.body->stmts[0]->kind == hir::hir_node_kind::hir_let,
         "expected the first statement to be the plain hir_let");
  const auto &let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[0]);
  expect(let.name == "y", "expected the let to bind `y`");
}

auto test_lowers_tuple_pattern_let_destructuring() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "def first_of_pair(pair: (int32, str)) -> int32:\n"
                    "    let (n, _) = pair\n"
                    "    return n\n");
  const auto &decl = find_func(*fixture.ast_file, "first_of_pair");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a destructuring let binding to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 3,
         "expected the synthetic subject let, the `n` let, and the return");

  const auto &subject_let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[0]);
  expect(subject_let.name == "<let subject>",
         "expected the first statement to bind the synthetic let subject");
  expect(subject_let.initializer->kind == hir::hir_node_kind::hir_local_ref,
         "expected the synthetic subject to be initialized from `pair`");

  const auto &n_let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[1]);
  expect(n_let.name == "n", "expected the second statement to bind `n`");
  expect(n_let.initializer->kind == hir::hir_node_kind::hir_tuple_index,
         "expected `n` to be initialized from a tuple-index projection");
  const auto &tuple_ref =
      dynamic_cast<const hir::hir_tuple_index &>(*n_let.initializer);
  expect(tuple_ref.index == 0, "expected the projection to read slot 0");
  const auto &subject_ref =
      dynamic_cast<const hir::hir_local_ref &>(*tuple_ref.object);
  expect(subject_ref.symbol == subject_let.symbol,
         "expected the projection to read the synthetic subject let");

  expect(function.body->stmts[2]->kind == hir::hir_node_kind::hir_return,
         "expected the final statement to be the return");
}

auto test_lowers_struct_pattern_let_destructuring() -> void {
  auto fixture = check_fixture("module sample\n"
                               "type container = { pub value: int32 }\n"
                               "def read(c: container) -> int32:\n"
                               "    let { value } = c\n"
                               "    return value\n");
  const auto &decl = find_func(*fixture.ast_file, "read");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(),
         "expected a destructuring struct let binding to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 3,
         "expected the synthetic subject let, the `value` let, and the return");
  const auto &value_let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[1]);
  expect(value_let.name == "value",
         "expected the second statement to bind `value`");
  expect(value_let.initializer->kind == hir::hir_node_kind::hir_field,
         "expected `value` to be initialized from a field projection");
}

auto test_lowers_tuple_destructuring_parameter() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def first((a, _): (int32, int32)) -> int32:\n"
                               "    return a\n");
  const auto &decl = find_func(*fixture.ast_file, "first");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(),
         "expected a destructuring tuple parameter to lower");

  const auto &function = **result;
  expect(function.params.size() == 1, "expected one lowered parameter");
  expect(function.params[0].name != "a",
         "expected the destructured parameter itself to get a synthetic "
         "name, not the bound name `a`");

  expect(function.body->stmts.size() == 2,
         "expected the synthetic `a` let plus the return");
  const auto &a_let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[0]);
  expect(a_let.name == "a", "expected the prelude let to bind `a`");
  expect(a_let.initializer->kind == hir::hir_node_kind::hir_tuple_index,
         "expected `a` to be initialized from a tuple-index projection");
  const auto &tuple_ref =
      dynamic_cast<const hir::hir_tuple_index &>(*a_let.initializer);
  expect(tuple_ref.index == 0, "expected the projection to read slot 0");
  const auto &param_ref =
      dynamic_cast<const hir::hir_local_ref &>(*tuple_ref.object);
  expect(param_ref.symbol == function.params[0].symbol,
         "expected the projection to read the synthetic parameter");
}

auto test_lowers_struct_destructuring_parameter() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "type container = { pub value: int32 }\n"
                    "def read_value({ value }: container) -> int32:\n"
                    "    return value\n");
  const auto &decl = find_func(*fixture.ast_file, "read_value");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(),
         "expected a destructuring struct parameter to lower");

  const auto &function = **result;
  expect(function.params.size() == 1, "expected one lowered parameter");
  expect(function.body->stmts.size() == 2,
         "expected the synthetic `value` let plus the return");
  const auto &value_let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[0]);
  expect(value_let.name == "value", "expected the prelude let to bind `value`");
  expect(value_let.initializer->kind == hir::hir_node_kind::hir_field,
         "expected `value` to be initialized from a field projection");
}

auto test_lowers_array_pattern_destructuring() -> void {
  // This grammar has no rest/slice-capture pattern syntax (`..` in pattern
  // position is always an open-ended range_pattern, never "gather the
  // rest") — so an array pattern is a plain fixed-arity structural
  // destructure, same shape as a tuple pattern.
  auto fixture = check_fixture("module sample\n"
                               "def head(xs: array[int32, 3]) -> int32:\n"
                               "    return match xs:\n"
                               "        [a, _, _] => a\n");
  const auto &decl = find_func(*fixture.ast_file, "head");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected an array pattern to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &match = dynamic_cast<const hir::hir_match &>(*ret.value);
  expect(match.arms[0].pattern->kind == hir::hir_node_kind::hir_array_pattern,
         "expected the arm's pattern to be a hir_array_pattern");

  const auto &array_pat =
      dynamic_cast<const hir::hir_array_pattern &>(*match.arms[0].pattern);
  expect(array_pat.elements.size() == 3, "expected three array slots");
  expect(array_pat.elements[0]->kind ==
             hir::hir_node_kind::hir_wildcard_pattern,
         "expected the bound first slot to desugar to a wildcard");
  expect(array_pat.elements[1]->kind ==
             hir::hir_node_kind::hir_wildcard_pattern,
         "expected the plain `_` slots to already be wildcards");

  const auto &let =
      dynamic_cast<const hir::hir_let &>(*match.arms[0].body->stmts.front());
  expect(let.name == "a", "expected the synthetic let to bind `a`");
  expect(let.initializer->kind == hir::hir_node_kind::hir_tuple_index,
         "expected `a` to be initialized from a positional projection "
         "(array elements reuse hir_tuple_index)");
  const auto &index_ref =
      dynamic_cast<const hir::hir_tuple_index &>(*let.initializer);
  expect(index_ref.index == 0, "expected the projection to read slot 0");
}

auto test_lowers_array_pattern_let_destructuring() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def second(xs: array[int32, 3]) -> int32:\n"
                               "    let [_, b, _] = xs\n"
                               "    return b\n");
  const auto &decl = find_func(*fixture.ast_file, "second");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(),
         "expected a destructuring array let binding to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 3,
         "expected the synthetic subject let, the `b` let, and the return");
  const auto &b_let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[1]);
  expect(b_let.name == "b", "expected the second statement to bind `b`");
  const auto &index_ref =
      dynamic_cast<const hir::hir_tuple_index &>(*b_let.initializer);
  expect(index_ref.index == 1, "expected the projection to read slot 1");
}

auto test_lowers_let_else_fallible_destructuring() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def parse(v: option[int32]) -> int32:\n"
                               "    let @some(n) = v else:\n"
                               "        return -1\n"
                               "    return n\n");
  const auto &decl = find_func(*fixture.ast_file, "parse");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(),
         "expected a let-else fallible destructure to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 3,
         "expected the hir_let_else, the `n` let, and the final return");

  expect(function.body->stmts[0]->kind == hir::hir_node_kind::hir_let_else,
         "expected the first statement to be a hir_let_else");
  const auto &let_else =
      dynamic_cast<const hir::hir_let_else &>(*function.body->stmts[0]);
  expect(let_else.initializer->kind == hir::hir_node_kind::hir_local_ref,
         "expected the let-else to be initialized from `v`");
  expect(let_else.pattern->kind == hir::hir_node_kind::hir_constructor_pattern,
         "expected the let-else's structural pattern to be a "
         "hir_constructor_pattern (from `@some(n)`)");
  const auto &ctor_pat =
      dynamic_cast<const hir::hir_constructor_pattern &>(*let_else.pattern);
  expect(ctor_pat.variant_name == "some",
         "expected the structural pattern's variant to be `some`");
  expect(ctor_pat.args.size() == 1 &&
             ctor_pat.args[0]->kind == hir::hir_node_kind::hir_wildcard_pattern,
         "expected the bound payload arg to desugar to a wildcard, "
         "identically to how match-arm destructuring works");

  expect(let_else.else_body->stmts.size() == 1,
         "expected the else block to hold the one `return -1` statement");
  expect(let_else.else_body->stmts.front()->kind ==
             hir::hir_node_kind::hir_return,
         "expected the else block's statement to be a hir_return");

  const auto &n_let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[1]);
  expect(n_let.name == "n", "expected the second statement to bind `n`");
  expect(n_let.initializer->kind == hir::hir_node_kind::hir_variant_payload,
         "expected `n` to be initialized from a variant-payload projection");
  const auto &payload_ref =
      dynamic_cast<const hir::hir_variant_payload &>(*n_let.initializer);
  expect(payload_ref.variant_name == "some",
         "expected the payload projection's variant to be `some`");
  const auto &subject_ref =
      dynamic_cast<const hir::hir_local_ref &>(*payload_ref.object);
  expect(subject_ref.symbol == let_else.subject_symbol,
         "expected the payload projection to read the let-else's subject");

  expect(function.body->stmts[2]->kind == hir::hir_node_kind::hir_return,
         "expected the final statement to be `return n`");
}

auto test_lowers_var_and_plain_assignment() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def counter(start: int32) -> int32:\n"
                               "    var total = start\n"
                               "    total = start + 1\n"
                               "    return total\n");
  const auto &decl = find_func(*fixture.ast_file, "counter");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a var binding plus assignment to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 3,
         "expected the var let, the assignment, and the return");

  expect(function.body->stmts[0]->kind == hir::hir_node_kind::hir_let,
         "expected the first statement to be a hir_let");
  const auto &var_let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[0]);
  expect(var_let.name == "total", "expected the var to bind `total`");
  expect(var_let.is_mut, "expected a `var` binding to be marked mutable");

  expect(function.body->stmts[1]->kind == hir::hir_node_kind::hir_assign,
         "expected the second statement to be a hir_assign");
  const auto &assign =
      dynamic_cast<const hir::hir_assign &>(*function.body->stmts[1]);
  expect(assign.op == kira::ast::assign_op::Assign,
         "expected a plain `=` to lower to assign_op::Assign");
  expect(assign.target->kind == hir::hir_node_kind::hir_local_ref,
         "expected the assignment target to be a local reference");
  const auto &target = dynamic_cast<const hir::hir_local_ref &>(*assign.target);
  expect(target.symbol == var_let.symbol,
         "expected the assignment to target the same symbol `var` declared");
  expect(assign.value->kind == hir::hir_node_kind::hir_binary,
         "expected the assigned value to be a hir_binary");
}

auto test_lowers_compound_assignment() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def counter(start: int32) -> int32:\n"
                               "    var total = start\n"
                               "    total += 1\n"
                               "    return total\n");
  const auto &decl = find_func(*fixture.ast_file, "counter");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a compound assignment to lower");

  const auto &function = **result;
  const auto &assign =
      dynamic_cast<const hir::hir_assign &>(*function.body->stmts[1]);
  expect(assign.op == kira::ast::assign_op::AddAssign,
         "expected `+=` to lower to assign_op::AddAssign");
}

auto test_lowers_while_loop() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def count_down(n: int32) -> int32:\n"
                               "    var x = n\n"
                               "    while x > 0:\n"
                               "        x = x - 1\n"
                               "    return x\n");
  const auto &decl = find_func(*fixture.ast_file, "count_down");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a while loop to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 3,
         "expected the var let, the while loop, and the return");
  expect(function.body->stmts[1]->kind == hir::hir_node_kind::hir_while,
         "expected the second statement to be a hir_while");

  const auto &while_loop =
      dynamic_cast<const hir::hir_while &>(*function.body->stmts[1]);
  expect(while_loop.condition->kind == hir::hir_node_kind::hir_binary,
         "expected the loop condition to be a hir_binary comparison");
  expect(while_loop.body->stmts.size() == 1,
         "expected the loop body to hold the one assignment statement");
  expect(while_loop.body->stmts.front()->kind == hir::hir_node_kind::hir_assign,
         "expected the loop body's statement to be a hir_assign");
}

auto test_rejects_while_let() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def parse(v: option[int32]) -> int32:\n"
                               "    while let @some(n) = v:\n"
                               "        return n\n"
                               "    return -1\n");
  const auto &decl = find_func(*fixture.ast_file, "parse");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(!result.has_value(), "expected `while let` to be rejected");
  expect(result.error().kind == hir::lowering_error_kind::unsupported_construct,
         "expected the specific unsupported_construct error kind");
}

auto test_rejects_for_loop() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def sum_list(xs: list[int32]) -> int32:\n"
                               "    var total = 0\n"
                               "    for x in xs:\n"
                               "        total = total + x\n"
                               "    return total\n");
  const auto &decl = find_func(*fixture.ast_file, "sum_list");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(!result.has_value(), "expected a `for` loop to be rejected");
  expect(result.error().kind == hir::lowering_error_kind::unsupported_construct,
         "expected the specific unsupported_construct error kind");
}

auto test_lowers_tuple_literal() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def make_pair(a: int32, b: int32) -> "
                               "(int32, int32):\n"
                               "    return (a, b)\n");
  const auto &decl = find_func(*fixture.ast_file, "make_pair");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a tuple literal to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_tuple,
         "expected the returned value to be a hir_tuple");
  const auto &tuple = dynamic_cast<const hir::hir_tuple &>(*ret.value);
  expect(tuple.elements.size() == 2, "expected two tuple elements");
  expect(tuple.elements[0]->kind == hir::hir_node_kind::hir_local_ref,
         "expected the first element to be a local reference");
}

auto test_lowers_array_literal() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def make_array(a: int32, b: int32, "
                               "c: int32) -> array[int32, 3]:\n"
                               "    return [a, b, c]\n");
  const auto &decl = find_func(*fixture.ast_file, "make_array");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected an array literal to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_array_init,
         "expected the returned value to be a hir_array_init");
  const auto &array = dynamic_cast<const hir::hir_array_init &>(*ret.value);
  expect(array.elements.size() == 3, "expected three array elements");
  expect(array.fill_value == nullptr,
         "expected the explicit-list form to have no fill value");
}

auto test_lowers_array_fill_literal() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def make_filled(v: int32) -> array[int32, 3]:\n"
                               "    return [v; 3]\n");
  const auto &decl = find_func(*fixture.ast_file, "make_filled");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected an array fill literal to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &array = dynamic_cast<const hir::hir_array_init &>(*ret.value);
  expect(array.elements.empty(),
         "expected the fill form to have no explicit element list");
  expect(array.fill_value != nullptr, "expected a fill value");
  expect(array.fill_value->kind == hir::hir_node_kind::hir_local_ref,
         "expected the fill value to be a local reference to `v`");
  expect(array.fill_count != nullptr, "expected a fill count");
  expect(array.fill_count->kind == hir::hir_node_kind::hir_literal,
         "expected the fill count to be a literal");
}

auto test_lowers_struct_literal_explicit_field() -> void {
  auto fixture = check_fixture("module sample\n"
                               "type container = { pub value: int32 }\n"
                               "def make_container(v: int32) -> container:\n"
                               "    return { value: v }\n");
  const auto &decl = find_func(*fixture.ast_file, "make_container");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a struct literal to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_struct_init,
         "expected the returned value to be a hir_struct_init");
  const auto &init = dynamic_cast<const hir::hir_struct_init &>(*ret.value);
  expect(init.fields.size() == 1, "expected one initialized field");
  expect(init.fields[0].name == "value",
         "expected the field to be named `value`");
  expect(init.fields[0].value->kind == hir::hir_node_kind::hir_local_ref,
         "expected the field's value to be a local reference to `v`");
}

auto test_lowers_struct_literal_shorthand_field() -> void {
  auto fixture = check_fixture("module sample\n"
                               "type container = { pub value: int32 }\n"
                               "def wrap(value: int32) -> container:\n"
                               "    return { value }\n");
  const auto &decl = find_func(*fixture.ast_file, "wrap");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(),
         "expected a shorthand struct literal field to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &init = dynamic_cast<const hir::hir_struct_init &>(*ret.value);
  expect(init.fields.size() == 1, "expected one initialized field");
  expect(init.fields[0].name == "value",
         "expected the shorthand field to be named `value`");
  expect(init.fields[0].value->kind == hir::hir_node_kind::hir_local_ref,
         "expected the shorthand field to desugar to a local reference");
  const auto &value_ref =
      dynamic_cast<const hir::hir_local_ref &>(*init.fields[0].value);
  expect(value_ref.name == "value",
         "expected the desugared reference to read the `value` parameter");
  expect(value_ref.symbol == function.params[0].symbol,
         "expected the desugared reference to resolve to the same symbol "
         "as the `value` parameter");
}

auto test_lowers_cast_expression() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def widen(x: int32) -> int64:\n"
                               "    return x as int64\n");
  const auto &decl = find_func(*fixture.ast_file, "widen");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a cast expression to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_cast,
         "expected the returned value to be a hir_cast");
  const auto &cast = dynamic_cast<const hir::hir_cast &>(*ret.value);
  expect(cast.type == function.return_type,
         "expected the cast's type to be the destination type int64");
  expect(cast.operand->kind == hir::hir_node_kind::hir_local_ref,
         "expected the cast's operand to be a local reference to `x`");
  const auto &operand_ref =
      dynamic_cast<const hir::hir_local_ref &>(*cast.operand);
  expect(operand_ref.symbol == function.params[0].symbol,
         "expected the cast's operand to reference the `x` parameter");
}

} // namespace

auto main() -> int {
  try {
    test_lowers_fully_annotated_function();
    test_rejects_unannotated_parameter_with_specific_error();
    test_rejects_function_without_declared_return_type();
    test_preserves_source_spans();
    test_lowers_compact_expression_body();
    test_lowers_if_expression_and_module();
    test_rejects_generic_function();
    test_lowers_match_with_literal_and_wildcard_patterns();
    test_lowers_plain_binding_arm_as_wildcard_plus_let();
    test_lowers_pattern_alias_to_synthetic_let();
    test_rejects_binding_inside_or_pattern_alternative();
    test_lowers_struct_pattern_field_destructuring();
    test_lowers_struct_pattern_shorthand_field();
    test_lowers_tuple_pattern_destructuring();
    test_lowers_constructor_pattern_destructuring();
    test_lowers_option_and_result_pattern_sugar();
    test_lowers_range_pattern();
    test_lowers_array_pattern_destructuring();
    test_lowers_array_pattern_let_destructuring();
    test_lowers_let_else_fallible_destructuring();
    test_lowers_var_and_plain_assignment();
    test_lowers_compound_assignment();
    test_lowers_while_loop();
    test_rejects_while_let();
    test_rejects_for_loop();
    test_lowers_tuple_literal();
    test_lowers_array_literal();
    test_lowers_array_fill_literal();
    test_lowers_struct_literal_explicit_field();
    test_lowers_struct_literal_shorthand_field();
    test_lowers_cast_expression();
    test_lowers_plain_let_as_single_statement();
    test_lowers_tuple_pattern_let_destructuring();
    test_lowers_struct_pattern_let_destructuring();
    test_lowers_tuple_destructuring_parameter();
    test_lowers_struct_destructuring_parameter();
  } catch (const std::exception &ex) {
    std::cerr << "lower_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
