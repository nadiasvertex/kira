#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Same as `check_fixture`, but checks several files together as one
// session — needed to exercise cross-module call resolution, where the
// callee lives in a different file than the call site.
struct multi_checked_fixture {
  kira::source_manager sources;
  kira::diagnostic_bag diag{};
  std::vector<kira::ast::ptr<kira::ast::file>> ast_files;
  kira::semantic::checked_types checked;
};

auto check_fixture_multi(
    const std::vector<std::pair<std::string, std::string>> &files)
    -> multi_checked_fixture {
  auto fixture = multi_checked_fixture{};
  auto parsed_modules = std::vector<kira::semantic::parsed_module>{};
  auto file_ids = std::vector<kira::file_id_type>{};

  for (const auto &[path, text] : files) {
    const auto file_id = fixture.sources.add_file(path, text);
    expect(file_id.has_value(), "expected fixture source to register");
    const auto *file = fixture.sources.get(*file_id);
    expect(file != nullptr, "expected registered fixture source");

    auto lexer = kira::lexer(file->source(), file->id(), fixture.diag);
    auto tokens = lexer.tokenize();
    auto parser = kira::parser(std::move(tokens), file->id(), fixture.diag);
    auto ast_file = parser.parse_file();
    expect(fixture.diag.error_count() == 0,
           "expected fixture to parse cleanly");

    file_ids.push_back(*file_id);
    fixture.ast_files.push_back(std::move(ast_file));
  }

  for (size_t i = 0; i < fixture.ast_files.size(); ++i) {
    parsed_modules.push_back(kira::semantic::parsed_module{
        .file_id = file_ids[i], .ast_file = fixture.ast_files[i].get()});
  }

  auto file_has_errors = std::vector<bool>(file_ids.size(), false);
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

// A function generic over a compile-time *value* is compiled once per
// constant it is called with. The template itself is not a function anything
// can call — nothing passes `n` — so the module holds `get$3`, not `get`.
auto test_lowers_const_generic_instance_per_constant() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "def get[n: usize](v: array[int32, n], "
                    "i: usize) -> int32:\n"
                    "    return v[i]\n"
                    "def main() -> int32:\n"
                    "    let a: array[int32, 3] = [1, 2, 3]\n"
                    "    let b: array[int32, 5] = [1, 2, 3, 4, 5]\n"
                    "    return get(a, 0) + get(b, 0) + get(a, 2)\n");

  auto module = hir::lower_module(*fixture.ast_file, "sample", fixture.checked);
  expect(module.has_value(), "expected the module to lower");

  auto names = std::vector<std::string>{};
  for (const auto &function : (*module)->functions) {
    names.push_back(function->name);
  }
  const auto has = [&names](std::string_view name) -> bool {
    return std::ranges::find(names, name) != names.end();
  };
  expect(has("get$3"), "expected an instance compiled for `n == 3`");
  expect(has("get$5"), "expected an instance compiled for `n == 5`");
  expect(!has("get"), "expected the template itself never to be lowered");
  expect(names.size() == 3,
         "expected exactly one instance per distinct constant — the two calls "
         "at `n == 3` share `get$3`");
}

// The one thing monomorphization is *for*: with `n` a real number, a
// refinement over it has a bound to compare against, so `try_from` compiles
// into an ordinary runtime check instead of being refused.
auto test_lowers_try_from_on_a_const_generic_refinement() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "type index[n: usize] = usize where self < n\n"
                    "def at[n: usize](v: array[int32, n], raw: usize) "
                    "-> int32:\n"
                    "    if let @some(i) = index[n].try_from(raw):\n"
                    "        return v[i]\n"
                    "    return -1\n"
                    "def main() -> int32:\n"
                    "    let a: array[int32, 4] = [1, 2, 3, 4]\n"
                    "    return at(a, 2)\n");

  auto module = hir::lower_module(*fixture.ast_file, "sample", fixture.checked);
  expect(module.has_value(), "expected the module to lower");
  const auto lowered = std::ranges::any_of(
      (*module)->functions,
      [](const auto &function) -> bool { return function->name == "at$4"; });
  expect(lowered, "expected `at` to be compiled for `n == 4`, with its "
                  "`index[n].try_from` check against the constant `4`");
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
  expect(assign.op == kira::ast::assign_op::assign,
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
  expect(assign.op == kira::ast::assign_op::add_assign,
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

auto test_lowers_while_let() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def parse(v: option[int32]) -> int32:\n"
                               "    while let @some(n) = v:\n"
                               "        return n\n"
                               "    return -1\n");
  const auto &decl = find_func(*fixture.ast_file, "parse");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected `while let` to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 2,
         "expected the while-let loop and the trailing return");
  expect(function.body->stmts[0]->kind == hir::hir_node_kind::hir_while_let,
         "expected a hir_while_let node");
  const auto &loop =
      dynamic_cast<const hir::hir_while_let &>(*function.body->stmts[0]);
  expect(loop.subject->kind == hir::hir_node_kind::hir_local_ref,
         "expected the subject to be a reference to `v`");
  expect(loop.pattern->kind == hir::hir_node_kind::hir_constructor_pattern,
         "expected the pattern to be `@some(n)`");
  const auto &pattern =
      dynamic_cast<const hir::hir_constructor_pattern &>(*loop.pattern);
  expect(pattern.variant_name == "some", "expected the `some` variant");

  expect(loop.body->stmts.size() == 2,
         "expected the synthesized `let n = <payload>` plus the surface "
         "`return n`");
  const auto &n_let = dynamic_cast<const hir::hir_let &>(*loop.body->stmts[0]);
  expect(n_let.name == "n", "expected the bound name to be `n`");
  expect(n_let.initializer->kind == hir::hir_node_kind::hir_variant_payload,
         "expected `n` to be bound from the `some` payload");
  expect(loop.body->stmts[1]->kind == hir::hir_node_kind::hir_return,
         "expected the surface `return n` as the second statement");
}

auto test_lowers_if_let() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def unwrap_or(v: option[int32]) -> int32:\n"
                               "    if let @some(n) = v:\n"
                               "        return n\n"
                               "    else:\n"
                               "        return -1\n");
  const auto &decl = find_func(*fixture.ast_file, "unwrap_or");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected `if let` to lower");

  // An `if let` has no boolean condition to test, so it lowers to a `match`
  // over its scrutinee, not to a `hir_if` (see `lower_if`). This one is the
  // body's trailing statement, so it arrives wrapped as the block's tail
  // value (`lower_tail_control_flow_stmt`), exactly like a trailing `if`.
  const auto &function = **result;
  expect(function.body->stmts.size() == 1, "expected the one `if let`");
  const auto &tail =
      dynamic_cast<const hir::hir_expr_stmt &>(*function.body->stmts[0]);
  expect(tail.expr->kind == hir::hir_node_kind::hir_match,
         "expected `if let` to lower to a hir_match");
  const auto &match = dynamic_cast<const hir::hir_match &>(*tail.expr);
  expect(match.subject->kind == hir::hir_node_kind::hir_local_ref,
         "expected the subject to be a reference to `v`");
  expect(match.arms.size() == 2,
         "expected the pattern's arm plus a wildcard arm for the `else`");

  expect(match.arms[0].pattern->kind ==
             hir::hir_node_kind::hir_constructor_pattern,
         "expected the first arm to test `@some(n)`");
  const auto &pattern = dynamic_cast<const hir::hir_constructor_pattern &>(
      *match.arms[0].pattern);
  expect(pattern.variant_name == "some", "expected the `some` variant");
  expect(match.arms[0].body->stmts.size() == 2,
         "expected the synthesized `let n = <payload>` plus the surface "
         "`return n`");
  const auto &n_let =
      dynamic_cast<const hir::hir_let &>(*match.arms[0].body->stmts[0]);
  expect(n_let.name == "n", "expected the bound name to be `n`");
  expect(n_let.initializer->kind == hir::hir_node_kind::hir_variant_payload,
         "expected `n` to be bound from the `some` payload");
  expect(match.arms[0].body->stmts[1]->kind == hir::hir_node_kind::hir_return,
         "expected the surface `return n`");

  expect(match.arms[1].pattern->kind ==
             hir::hir_node_kind::hir_wildcard_pattern,
         "expected the fallback arm to be a wildcard");
  expect(match.arms[1].body->stmts.size() == 1,
         "expected the `else` body under the wildcard arm");
  expect(match.arms[1].body->stmts[0]->kind == hir::hir_node_kind::hir_return,
         "expected the `else` body's `return -1`");
}

auto test_lowers_elif_let_chain() -> void {
  // A chain that mixes both branch forms: the leading plain `if` stays a
  // `hir_if`, and everything after it — the `elif let` and the `else` — is
  // what that `hir_if`'s else block holds.
  auto fixture =
      check_fixture("module sample\n"
                    "def pick(flag: bool, v: option[int32]) -> int32:\n"
                    "    if flag:\n"
                    "        return 0\n"
                    "    elif let @some(n) = v:\n"
                    "        return n\n"
                    "    else:\n"
                    "        return -1\n");
  const auto &decl = find_func(*fixture.ast_file, "pick");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected an `elif let` chain to lower");

  const auto &function = **result;
  const auto &chain =
      dynamic_cast<const hir::hir_expr_stmt &>(*function.body->stmts[0]);
  expect(chain.expr->kind == hir::hir_node_kind::hir_if,
         "expected the leading plain `if` to stay a hir_if");
  const auto &conditional = dynamic_cast<const hir::hir_if &>(*chain.expr);
  expect(conditional.branches.size() == 1,
         "expected only the plain `if flag` branch on the hir_if itself");
  expect(conditional.else_body != nullptr,
         "expected the rest of the chain as the hir_if's else block");
  expect(conditional.else_body->stmts.size() == 1,
         "expected the rest of the chain to be the else block's one statement");

  const auto &tail = dynamic_cast<const hir::hir_expr_stmt &>(
      *conditional.else_body->stmts[0]);
  expect(tail.expr->kind == hir::hir_node_kind::hir_match,
         "expected the `elif let` to lower to a hir_match in the else block");
  const auto &match = dynamic_cast<const hir::hir_match &>(*tail.expr);
  expect(match.arms.size() == 2,
         "expected `@some(n)`'s arm plus a wildcard arm carrying the `else`");
  expect(match.arms[1].body->stmts.size() == 1,
         "expected the trailing `else` body under the wildcard arm");
}

auto test_rejects_for_loop_over_user_defined_type() -> void {
  // list/array/slice/str iteration lowers now (see
  // test_lowers_list_for_loop and friends) — a user-defined iterable is
  // the one shape spec/iterator-protocol-design.md still defers, since it
  // would need a real trait-based protocol this project doesn't have yet.
  auto fixture = check_fixture("module sample\n"
                               "type counter = { pub value: int32 }\n"
                               "def sum_counters(xs: counter) -> int32:\n"
                               "    var total = 0\n"
                               "    for x in xs:\n"
                               "        total = total + x\n"
                               "    return total\n");
  const auto &decl = find_func(*fixture.ast_file, "sum_counters");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(!result.has_value(),
         "expected a `for` loop over a user-defined type to be rejected");
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

auto test_lowers_lambda_expression() -> void {
  auto fixture = check_fixture(
      "module sample\n"
      "def apply_twice(f: fn(int32) -> int32, x: int32) -> int32:\n"
      "    return f(f(x))\n"
      "def double_value(x: int32) -> int32:\n"
      "    let inc = pure (n: int32) -> int32 => n + 1\n"
      "    return apply_twice(inc, x)\n");
  const auto &decl = find_func(*fixture.ast_file, "double_value");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a lambda expression to lower");

  const auto &function = **result;
  const auto &let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts.front());
  expect(let.name == "inc", "expected the let to bind `inc`");
  expect(let.initializer->kind == hir::hir_node_kind::hir_lambda,
         "expected the let's initializer to be a hir_lambda");

  const auto &lambda = dynamic_cast<const hir::hir_lambda &>(*let.initializer);
  expect(lambda.params.size() == 1, "expected one lambda parameter");
  expect(lambda.params[0].name == "n", "expected the parameter to be `n`");

  const auto int32_type = fixture.checked.types.builtin("int32");
  expect(lambda.params[0].type == int32_type,
         "expected the parameter's type to be int32");
  expect(lambda.return_type == int32_type,
         "expected the lambda's return type to be int32");

  expect(lambda.body->stmts.size() == 1,
         "expected the compact body to lower to a single implicit return");
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*lambda.body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_binary,
         "expected the lambda's returned value to be a hir_binary");

  const auto &sum = dynamic_cast<const hir::hir_binary &>(*ret.value);
  expect(sum.lhs->kind == hir::hir_node_kind::hir_local_ref,
         "expected the lambda body to reference its own parameter `n`");
  const auto &n_ref = dynamic_cast<const hir::hir_local_ref &>(*sum.lhs);
  expect(n_ref.symbol == lambda.params[0].symbol,
         "expected the reference to resolve to the lambda's own parameter, "
         "not some outer symbol");
}

auto test_lowers_where_expression() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def compute(v: int32) -> int32:\n"
                               "    return (a + b) where:\n"
                               "        a = v + 1\n"
                               "        b = v + 2\n");
  const auto &decl = find_func(*fixture.ast_file, "compute");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a where expression to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_block,
         "expected the returned value to be a hir_block (where's bindings "
         "plus its inner expression)");

  const auto &block = dynamic_cast<const hir::hir_block &>(*ret.value);
  expect(block.stmts.size() == 3,
         "expected two where bindings plus the trailing inner expression");
  expect(block.stmts[0]->kind == hir::hir_node_kind::hir_let,
         "expected the first statement to bind `a`");
  expect(block.stmts[1]->kind == hir::hir_node_kind::hir_let,
         "expected the second statement to bind `b`");
  const auto &a_let = dynamic_cast<const hir::hir_let &>(*block.stmts[0]);
  const auto &b_let = dynamic_cast<const hir::hir_let &>(*block.stmts[1]);
  expect(a_let.name == "a", "expected the first binding to be `a`");
  expect(b_let.name == "b", "expected the second binding to be `b`");

  expect(block.stmts[2]->kind == hir::hir_node_kind::hir_expr_stmt,
         "expected the trailing statement to be the inner `a + b` expression");
  const auto &inner_stmt =
      dynamic_cast<const hir::hir_expr_stmt &>(*block.stmts[2]);
  expect(inner_stmt.expr->kind == hir::hir_node_kind::hir_binary,
         "expected the inner expression to be a hir_binary");
}

auto test_lowers_call_to_named_function() -> void {
  // Regression guard: `infer_call` resolves a plain-identifier callee that
  // names a real function (a scope binding, a same-module function, or an
  // import) straight from its declaration rather than through `infer_expr`,
  // so the callee ident's own type needs its own persistence hook — same
  // class of gap as the assignment-target and parameter-pattern fixes.
  auto fixture = check_fixture("module sample\n"
                               "def helper(x: int32) -> int32:\n"
                               "    return x + 1\n"
                               "def caller(x: int32) -> int32:\n"
                               "    return helper(helper(x))\n");
  const auto &decl = find_func(*fixture.ast_file, "caller");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a call to a named function to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_call,
         "expected the returned value to be a hir_call");
  const auto &outer_call = dynamic_cast<const hir::hir_call &>(*ret.value);
  expect(outer_call.callee->kind == hir::hir_node_kind::hir_local_ref,
         "expected the callee to lower to a local reference");
  const auto &outer_callee =
      dynamic_cast<const hir::hir_local_ref &>(*outer_call.callee);
  expect(outer_callee.name == "helper",
         "expected the callee reference to name `helper`");

  expect(outer_call.args.size() == 1,
         "expected one argument to the outer call");
  expect(outer_call.args[0]->kind == hir::hir_node_kind::hir_call,
         "expected the argument to be the nested `helper(x)` call");
  const auto &inner_call =
      dynamic_cast<const hir::hir_call &>(*outer_call.args[0]);
  const auto &inner_callee =
      dynamic_cast<const hir::hir_local_ref &>(*inner_call.callee);
  expect(inner_callee.symbol == outer_callee.symbol,
         "expected both calls to `helper` to resolve to the same symbol");
}

auto test_lowers_module_qualified_call() -> void {
  auto fixture = check_fixture_multi({
      {"tools.kira", "module tools\n"
                     "pub def double(x: int32) -> int32:\n"
                     "    return x * 2\n"},
      {"app.kira", "module app\n"
                   "use tools\n"
                   "pub def run() -> int32:\n"
                   "    return tools.double(21)\n"},
  });
  const auto &decl = find_func(*fixture.ast_files[1], "run");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a module-qualified call to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_call,
         "expected the returned value to be a hir_call");
  const auto &call = dynamic_cast<const hir::hir_call &>(*ret.value);
  const auto &callee = dynamic_cast<const hir::hir_local_ref &>(*call.callee);
  expect(callee.name == "double", "expected the callee to name `double`");
  expect(callee.owner_module.has_value() && *callee.owner_module == "tools",
         "expected the callee to carry owner_module `tools`");
}

auto test_lowers_type_qualified_associated_call() -> void {
  auto fixture = check_fixture("module app\n"
                               "pub trait from[T]:\n"
                               "    def from(value: T) -> self\n"
                               "pub type wrapper = { pub value: int32 }\n"
                               "impl from[int32] for wrapper:\n"
                               "    def from(x: int32) -> wrapper:\n"
                               "        return wrapper{ value: x }\n"
                               "pub def run() -> wrapper:\n"
                               "    return wrapper.from(5)\n");

  auto module_result =
      hir::lower_module(*fixture.ast_file, "app", fixture.checked);
  expect(module_result.has_value(),
         "expected the module to lower, including the associated function");
  expect((*module_result)->functions.size() == 2,
         "expected both `run` and the lowered `wrapper::from` in the module");

  const hir::hir_function *run = nullptr;
  const hir::hir_function *wrapper_from = nullptr;
  for (const auto &fn : (*module_result)->functions) {
    if (fn->name == "run") {
      run = fn.get();
    } else if (fn->name == "wrapper::from") {
      wrapper_from = fn.get();
    }
  }
  expect(run != nullptr, "expected to find the lowered `run` function");
  expect(wrapper_from != nullptr,
         "expected the impl member to lower as `wrapper::from`");

  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*run->body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_call,
         "expected `run`'s returned value to be a hir_call");
  const auto &call = dynamic_cast<const hir::hir_call &>(*ret.value);
  const auto &callee = dynamic_cast<const hir::hir_local_ref &>(*call.callee);
  expect(callee.name == "wrapper::from",
         "expected the callee to name `wrapper::from`");
  expect(callee.owner_module.has_value() && *callee.owner_module == "app",
         "expected the callee to carry owner_module `app`");
}

auto test_lowers_named_call_arguments_in_declared_order() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def subtract(a: int32, b: int32) -> int32:\n"
                               "    return a - b\n"
                               "def caller(x: int32, y: int32) -> int32:\n"
                               "    return subtract(b: y, a: x)\n");
  const auto &decl = find_func(*fixture.ast_file, "caller");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a named-argument call to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  const auto &call = dynamic_cast<const hir::hir_call &>(*ret.value);
  expect(call.args.size() == 2, "expected two positional arguments");

  // Written as `subtract(b: y, a: x)` — lowering must still produce the
  // callee's declared order `(a, b)`, i.e. [x, y], not the written order.
  const auto &first_arg =
      dynamic_cast<const hir::hir_local_ref &>(*call.args[0]);
  const auto &second_arg =
      dynamic_cast<const hir::hir_local_ref &>(*call.args[1]);
  expect(first_arg.name == "x",
         "expected the first positional argument to be `x` (bound to `a`)");
  expect(second_arg.name == "y",
         "expected the second positional argument to be `y` (bound to `b`)");
  expect(first_arg.symbol == function.params[0].symbol,
         "expected the first argument to reference caller's `x` parameter");
  expect(second_arg.symbol == function.params[1].symbol,
         "expected the second argument to reference caller's `y` parameter");
}

auto test_rejects_call_relying_on_default_argument() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "def greet(name: str, greeting: str = \"hello\") -> str:\n"
                    "    return greeting\n"
                    "def caller(name: str) -> str:\n"
                    "    return greet(name)\n");
  const auto &decl = find_func(*fixture.ast_file, "caller");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(!result.has_value(),
         "expected a call relying on a default argument to be rejected");
  expect(result.error().kind == hir::lowering_error_kind::unsupported_construct,
         "expected the specific unsupported_construct error kind");
}

auto test_lowers_variant_construction_with_payload() -> void {
  auto fixture = check_fixture("module sample\n"
                               "type shape = @circle(int32) | @square(int32)\n"
                               "def make_circle(r: int32) -> shape:\n"
                               "    return @circle(r)\n");
  const auto &decl = find_func(*fixture.ast_file, "make_circle");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected variant construction to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_variant_init,
         "expected the returned value to be a hir_variant_init");
  const auto &init = dynamic_cast<const hir::hir_variant_init &>(*ret.value);
  expect(init.variant_name == "circle",
         "expected the constructed variant to be `circle`");
  expect(init.args.size() == 1, "expected one payload argument");
  expect(init.args[0]->kind == hir::hir_node_kind::hir_local_ref,
         "expected the payload argument to be a local reference to `r`");
}

auto test_lowers_bare_unit_variant() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def none_int() -> option[int32]:\n"
                               "    return @none\n");
  const auto &decl = find_func(*fixture.ast_file, "none_int");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a bare unit variant to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts.front());
  expect(ret.value->kind == hir::hir_node_kind::hir_variant_init,
         "expected the returned value to be a hir_variant_init");
  const auto &init = dynamic_cast<const hir::hir_variant_init &>(*ret.value);
  expect(init.variant_name == "none",
         "expected the constructed variant to be `none`");
  expect(init.args.empty(), "expected no payload arguments for `@none`");
}

auto test_lowers_try_expr_on_result() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "def parse(v: result[int32, str]) -> result[int32, str]:\n"
                    "    let n = v?\n"
                    "    return @ok(n)\n");
  const auto &decl = find_func(*fixture.ast_file, "parse");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a `?` on a result to lower");

  const auto &function = **result;
  const auto &let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts.front());
  expect(let.name == "n", "expected the let to bind `n`");
  expect(let.initializer->kind == hir::hir_node_kind::hir_match,
         "expected `v?` to lower to a hir_match");

  const auto &match = dynamic_cast<const hir::hir_match &>(*let.initializer);
  expect(match.subject->kind == hir::hir_node_kind::hir_local_ref,
         "expected the match subject to be a reference to `v`");
  expect(match.arms.size() == 2, "expected exactly two arms (ok and err)");

  const auto &ok_pattern = dynamic_cast<const hir::hir_constructor_pattern &>(
      *match.arms[0].pattern);
  expect(ok_pattern.variant_name == "ok",
         "expected the first arm's pattern to match `ok`");
  expect(ok_pattern.args.size() == 1,
         "expected the `ok` pattern to destructure one payload slot");
  expect(match.arms[0].body->stmts.size() == 1,
         "expected the success arm to hold one statement");
  const auto &success_stmt = dynamic_cast<const hir::hir_expr_stmt &>(
      *match.arms[0].body->stmts.front());
  expect(success_stmt.expr->kind == hir::hir_node_kind::hir_variant_payload,
         "expected the success arm to yield a payload projection");
  const auto &success_payload =
      dynamic_cast<const hir::hir_variant_payload &>(*success_stmt.expr);
  expect(success_payload.variant_name == "ok",
         "expected the payload projection's variant to be `ok`");
  const auto &success_subject =
      dynamic_cast<const hir::hir_local_ref &>(*success_payload.object);
  expect(success_subject.symbol == match.subject_symbol,
         "expected the payload projection to read the match subject");

  const auto &err_pattern = dynamic_cast<const hir::hir_constructor_pattern &>(
      *match.arms[1].pattern);
  expect(err_pattern.variant_name == "err",
         "expected the second arm's pattern to match `err`");
  expect(match.arms[1].body->stmts.size() == 1,
         "expected the failure arm to hold one statement");
  const auto &failure_stmt =
      dynamic_cast<const hir::hir_return &>(*match.arms[1].body->stmts.front());
  expect(failure_stmt.value->kind == hir::hir_node_kind::hir_local_ref,
         "expected the failure arm to return the subject unchanged");
  const auto &failure_ref =
      dynamic_cast<const hir::hir_local_ref &>(*failure_stmt.value);
  expect(failure_ref.symbol == match.subject_symbol,
         "expected the failure arm's return to reference the match subject");
}

auto test_lowers_try_expr_on_option() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def parse(v: option[int32]) -> option[int32]:\n"
                               "    let n = v?\n"
                               "    return @some(n)\n");
  const auto &decl = find_func(*fixture.ast_file, "parse");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a `?` on an option to lower");

  const auto &function = **result;
  const auto &let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts.front());
  const auto &match = dynamic_cast<const hir::hir_match &>(*let.initializer);

  const auto &some_pattern = dynamic_cast<const hir::hir_constructor_pattern &>(
      *match.arms[0].pattern);
  expect(some_pattern.variant_name == "some",
         "expected the first arm's pattern to match `some`");

  const auto &none_pattern = dynamic_cast<const hir::hir_constructor_pattern &>(
      *match.arms[1].pattern);
  expect(none_pattern.variant_name == "none",
         "expected the second arm's pattern to match `none`");
  expect(none_pattern.args.empty(),
         "expected the `none` pattern to have no payload slots");
}

auto test_lowers_range_for_loop() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def sum_up_to(n: int32) -> int32:\n"
                               "    var total = 0\n"
                               "    for i in 0..n:\n"
                               "        total = total + i\n"
                               "    return total\n");
  const auto &decl = find_func(*fixture.ast_file, "sum_up_to");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a range `for` loop to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 6,
         "expected total-let, start-let, end-let, index-let, while, return");

  const auto &start_let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[1]);
  expect(start_let.name == "<for start>",
         "expected the start bound to be bound to a synthetic let");
  expect(start_let.initializer->kind == hir::hir_node_kind::hir_literal,
         "expected the start bound to be the literal `0`");

  const auto &end_let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[2]);
  expect(end_let.name == "<for end>",
         "expected the end bound to be bound to a synthetic let");
  expect(end_let.initializer->kind == hir::hir_node_kind::hir_local_ref,
         "expected the end bound to reference `n`");

  const auto &index_let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[3]);
  expect(index_let.name == "<for index>", "expected a synthetic index let");
  expect(index_let.is_mut, "expected the index to be mutable");

  expect(function.body->stmts[4]->kind == hir::hir_node_kind::hir_while,
         "expected a hir_while loop");
  const auto &loop =
      dynamic_cast<const hir::hir_while &>(*function.body->stmts[4]);
  expect(loop.condition->kind == hir::hir_node_kind::hir_binary,
         "expected the loop condition to be a comparison");
  const auto &condition =
      dynamic_cast<const hir::hir_binary &>(*loop.condition);
  expect(condition.op == kira::ast::binary_op::lt,
         "expected `..` to lower to a `<` bound check");
  expect(condition.type == fixture.checked.types.bool_type(),
         "expected the condition's type to be bool");

  expect(loop.body->stmts.size() == 3,
         "expected the loop-var let, the assignment, and the increment");
  const auto &loop_var_let =
      dynamic_cast<const hir::hir_let &>(*loop.body->stmts[0]);
  expect(loop_var_let.name == "i", "expected the loop variable to be `i`");
  expect(loop.body->stmts[1]->kind == hir::hir_node_kind::hir_assign,
         "expected the loop body's statement to be the assignment to `total`");
  const auto &increment =
      dynamic_cast<const hir::hir_assign &>(*loop.body->stmts[2]);
  expect(increment.op == kira::ast::assign_op::add_assign,
         "expected the index increment to be a compound `+=`");
}

auto test_lowers_inclusive_range_for_loop_with_guard() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def count_evens(n: int32) -> int32:\n"
                               "    var total = 0\n"
                               "    for i in 0..=n if i % 2 == 0:\n"
                               "        total = total + 1\n"
                               "    return total\n");
  const auto &decl = find_func(*fixture.ast_file, "count_evens");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(),
         "expected an inclusive-range `for` loop with a guard to lower");

  const auto &function = **result;
  const auto &loop =
      dynamic_cast<const hir::hir_while &>(*function.body->stmts[4]);
  const auto &condition =
      dynamic_cast<const hir::hir_binary &>(*loop.condition);
  expect(condition.op == kira::ast::binary_op::lt_eq,
         "expected `..=` to lower to a `<=` bound check");

  expect(loop.body->stmts.size() == 3,
         "expected the loop-var let, the guarded body, and the increment");
  expect(loop.body->stmts[1]->kind == hir::hir_node_kind::hir_if,
         "expected the guard to wrap the body in a hir_if rather than "
         "using a `continue`-style jump (this language has none)");
  const auto &guarded = dynamic_cast<const hir::hir_if &>(*loop.body->stmts[1]);
  expect(guarded.branches.size() == 1, "expected a single guard branch");
  expect(guarded.else_body == nullptr,
         "expected no else branch on the guard conditional");
}

auto test_lowers_list_for_loop() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def sum_list(xs: list[int32]) -> int32:\n"
                               "    var total = 0\n"
                               "    for x in xs:\n"
                               "        total = total + x\n"
                               "    return total\n");
  const auto &decl = find_func(*fixture.ast_file, "sum_list");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a `for` loop over a list to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 5,
         "expected total-let, container-let, index-let, while, return");

  const auto &container_let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[1]);
  expect(container_let.name == "<for container>",
         "expected the iterable to be bound to a synthetic let");
  expect(container_let.initializer->kind == hir::hir_node_kind::hir_local_ref,
         "expected the iterable to be a reference to `xs`");

  const auto &index_let =
      dynamic_cast<const hir::hir_let &>(*function.body->stmts[2]);
  expect(index_let.name == "<for index>", "expected a synthetic index let");
  expect(index_let.is_mut, "expected the index to be mutable");
  const auto usize_type = fixture.checked.types.usize_type();
  expect(index_let.initializer->type == usize_type,
         "expected the index to start at a `usize` literal `0`");

  const auto &loop =
      dynamic_cast<const hir::hir_while &>(*function.body->stmts[3]);
  const auto &condition =
      dynamic_cast<const hir::hir_binary &>(*loop.condition);
  expect(condition.op == kira::ast::binary_op::lt,
         "expected the bound check to be `<`");
  expect(condition.rhs->kind == hir::hir_node_kind::hir_container_len,
         "expected the bound to be the list's runtime length");
  const auto &len =
      dynamic_cast<const hir::hir_container_len &>(*condition.rhs);
  expect(len.object->kind == hir::hir_node_kind::hir_local_ref,
         "expected the length projection to read the synthetic container");

  const auto &loop_var_let =
      dynamic_cast<const hir::hir_let &>(*loop.body->stmts[0]);
  expect(loop_var_let.name == "x", "expected the loop variable to be `x`");
  expect(loop_var_let.initializer->kind == hir::hir_node_kind::hir_index,
         "expected the loop variable to be indexed out of the container");
  const auto &index_expr =
      dynamic_cast<const hir::hir_index &>(*loop_var_let.initializer);
  expect(index_expr.object->kind == hir::hir_node_kind::hir_local_ref,
         "expected the index expression's object to be the container");
}

auto test_lowers_array_for_loop_with_static_length() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def sum_array(xs: array[int32, 3]) -> int32:\n"
                               "    var total = 0\n"
                               "    for x in xs:\n"
                               "        total = total + x\n"
                               "    return total\n");
  const auto &decl = find_func(*fixture.ast_file, "sum_array");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a `for` loop over an array to lower");

  const auto &function = **result;
  const auto &loop =
      dynamic_cast<const hir::hir_while &>(*function.body->stmts[3]);
  const auto &condition =
      dynamic_cast<const hir::hir_binary &>(*loop.condition);
  expect(condition.rhs->kind == hir::hir_node_kind::hir_literal,
         "expected an array's static length to lower to a plain literal, "
         "not a hir_container_len");
  const auto &bound = dynamic_cast<const hir::hir_literal &>(*condition.rhs);
  expect(bound.value == "3", "expected the literal bound to be the array's "
                             "declared length 3");
}

auto test_lowers_str_for_loop_yields_char() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def count_chars(s: str) -> int32:\n"
                               "    var total = 0\n"
                               "    for c in s:\n"
                               "        total = total + 1\n"
                               "    return total\n");
  const auto &decl = find_func(*fixture.ast_file, "count_chars");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a `for` loop over a str to lower");

  const auto &function = **result;
  const auto &loop =
      dynamic_cast<const hir::hir_while &>(*function.body->stmts[3]);
  const auto &loop_var_let =
      dynamic_cast<const hir::hir_let &>(*loop.body->stmts[0]);
  expect(loop_var_let.name == "c", "expected the loop variable to be `c`");
  expect(loop_var_let.initializer->type == fixture.checked.types.char_type(),
         "expected iterating a str to yield char elements");
}

auto test_lowers_option_for_loop() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def sum_opt(o: option[int32]) -> int32:\n"
                               "    var total = 0\n"
                               "    for x in o:\n"
                               "        total = total + x\n"
                               "    return total\n");
  const auto &decl = find_func(*fixture.ast_file, "sum_opt");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected `option` iteration to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 3,
         "expected total-let, the match statement, and the return");
  expect(function.body->stmts[1]->kind == hir::hir_node_kind::hir_expr_stmt,
         "expected the desugared `for` to be an expression statement");
  const auto &stmt =
      dynamic_cast<const hir::hir_expr_stmt &>(*function.body->stmts[1]);
  expect(stmt.expr->kind == hir::hir_node_kind::hir_match,
         "expected `option` iteration to desugar to a hir_match, not a loop");
  const auto &match = dynamic_cast<const hir::hir_match &>(*stmt.expr);
  expect(match.subject->kind == hir::hir_node_kind::hir_local_ref,
         "expected the match subject to be a reference to `o`");
  expect(match.arms.size() == 2, "expected exactly two match arms");

  const auto &some_pattern = dynamic_cast<const hir::hir_constructor_pattern &>(
      *match.arms[0].pattern);
  expect(some_pattern.variant_name == "some",
         "expected the first arm to match `some`");
  expect(match.arms[0].body->stmts.size() == 2,
         "expected the synthesized `let x = <payload>` plus the guarded body");
  const auto &x_let =
      dynamic_cast<const hir::hir_let &>(*match.arms[0].body->stmts[0]);
  expect(x_let.name == "x", "expected the bound name to be `x`");
  expect(x_let.initializer->kind == hir::hir_node_kind::hir_variant_payload,
         "expected `x` to be bound from the `some` payload");

  const auto &none_pattern = dynamic_cast<const hir::hir_constructor_pattern &>(
      *match.arms[1].pattern);
  expect(none_pattern.variant_name == "none",
         "expected the second arm to match `none`");
  expect(match.arms[1].body->stmts.empty(),
         "expected the `none` arm to be an empty block");
}

auto test_lowers_generator_for_loop() -> void {
  auto fixture = check_fixture(
      "module sample\n"
      "generator def counter(limit: int32) -> some iterator[int32]:\n"
      "    yield 1\n"
      "def sum_gen() -> int32:\n"
      "    var total = 0\n"
      "    for x in counter(5):\n"
      "        total = total + x\n"
      "    return total\n");
  const auto &decl = find_func(*fixture.ast_file, "sum_gen");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected `generator[T]` iteration to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 4,
         "expected total-let, the generator-handle let, the desugared "
         "while_let, and the return");
  expect(function.body->stmts[1]->kind == hir::hir_node_kind::hir_let,
         "expected the generator handle to be bound once, ahead of the loop");
  expect(function.body->stmts[2]->kind == hir::hir_node_kind::hir_while_let,
         "expected `generator[T]` iteration to desugar to a hir_while_let, "
         "not a counting loop");
  const auto &loop =
      dynamic_cast<const hir::hir_while_let &>(*function.body->stmts[2]);
  expect(loop.subject->kind == hir::hir_node_kind::hir_generator_next,
         "expected the while_let subject to be a fresh `.next()` call, "
         "re-evaluated every pass");
  const auto &next_call =
      dynamic_cast<const hir::hir_generator_next &>(*loop.subject);
  expect(next_call.object->kind == hir::hir_node_kind::hir_local_ref,
         "expected `.next()` to be called on the once-evaluated generator "
         "handle, not a fresh call to `counter(5)` every iteration");

  const auto &pattern =
      dynamic_cast<const hir::hir_constructor_pattern &>(*loop.pattern);
  expect(pattern.variant_name == "some",
         "expected the while_let pattern to match `some`");
  expect(loop.body->stmts.size() == 2,
         "expected the synthesized `let x = <payload>` plus the loop body");
  const auto &x_let = dynamic_cast<const hir::hir_let &>(*loop.body->stmts[0]);
  expect(x_let.name == "x", "expected the bound name to be `x`");
  expect(x_let.initializer->kind == hir::hir_node_kind::hir_variant_payload,
         "expected `x` to be bound from the `some` payload");
}

auto test_lowers_simple_comprehension() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def squares(n: int32) -> list[int32]:\n"
                               "    return for x in 0..n => x * x\n");
  const auto &decl = find_func(*fixture.ast_file, "squares");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a simple comprehension to lower");

  const auto &function = **result;
  expect(function.body->stmts.size() == 1,
         "expected a single `return` statement");
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts[0]);
  expect(ret.value != nullptr &&
             ret.value->kind == hir::hir_node_kind::hir_block,
         "expected the comprehension to lower to a block expression");
  const auto &comprehension = dynamic_cast<const hir::hir_block &>(*ret.value);
  // acc-let, then the range clause's own start/end/index lets plus its
  // while loop (see test_lowers_range_for_loop — same shape reused here),
  // then the trailing value.
  expect(comprehension.stmts.size() == 6,
         "expected acc-let, start-let, end-let, index-let, while, and the "
         "trailing value");

  const auto &acc_let =
      dynamic_cast<const hir::hir_let &>(*comprehension.stmts[0]);
  expect(acc_let.name == "<comprehension result>",
         "expected a synthetic accumulator let");
  expect(acc_let.is_mut, "expected the accumulator to be mutable");
  expect(acc_let.initializer->kind == hir::hir_node_kind::hir_array_init,
         "expected the accumulator to start from an empty array/list "
         "literal");
  const auto &empty_list =
      dynamic_cast<const hir::hir_array_init &>(*acc_let.initializer);
  expect(empty_list.elements.empty(),
         "expected the initial accumulator literal to have no elements");

  expect(comprehension.stmts[4]->kind == hir::hir_node_kind::hir_while,
         "expected the range clause to lower to a hir_while loop");
  const auto &loop =
      dynamic_cast<const hir::hir_while &>(*comprehension.stmts[4]);
  expect(loop.body->stmts.size() == 3,
         "expected the loop-var let, the push, and the increment");
  expect(loop.body->stmts[1]->kind == hir::hir_node_kind::hir_list_push,
         "expected the yielded value to be appended via hir_list_push");
  const auto &push =
      dynamic_cast<const hir::hir_list_push &>(*loop.body->stmts[1]);
  expect(push.target->kind == hir::hir_node_kind::hir_local_ref,
         "expected the push target to reference the accumulator");
  const auto &push_target =
      dynamic_cast<const hir::hir_local_ref &>(*push.target);
  expect(push_target.symbol == acc_let.symbol,
         "expected the push target to be the same accumulator symbol");
  expect(push.value->kind == hir::hir_node_kind::hir_binary,
         "expected the pushed value to be the yielded `x * x`");

  expect(comprehension.stmts[5]->kind == hir::hir_node_kind::hir_expr_stmt,
         "expected the block's trailing statement to be an expression "
         "statement");
  const auto &trailing =
      dynamic_cast<const hir::hir_expr_stmt &>(*comprehension.stmts[5]);
  expect(trailing.expr->kind == hir::hir_node_kind::hir_local_ref,
         "expected the trailing value to be the accumulator itself");
  const auto &trailing_ref =
      dynamic_cast<const hir::hir_local_ref &>(*trailing.expr);
  expect(trailing_ref.symbol == acc_let.symbol,
         "expected the trailing reference to be the accumulator symbol");
}

auto test_lowers_comprehension_with_guard() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def evens(n: int32) -> list[int32]:\n"
                               "    return for x in 0..n if x % 2 == 0 => x\n");
  const auto &decl = find_func(*fixture.ast_file, "evens");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a guarded comprehension to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts[0]);
  const auto &comprehension = dynamic_cast<const hir::hir_block &>(*ret.value);
  const auto &loop =
      dynamic_cast<const hir::hir_while &>(*comprehension.stmts[4]);

  expect(loop.body->stmts[1]->kind == hir::hir_node_kind::hir_if,
         "expected the guard to wrap the push in a hir_if");
  const auto &guarded = dynamic_cast<const hir::hir_if &>(*loop.body->stmts[1]);
  expect(guarded.branches.size() == 1, "expected a single guard branch");
  expect(guarded.branches[0].body->stmts.size() == 1,
         "expected the guarded branch to contain just the push");
  expect(guarded.branches[0].body->stmts[0]->kind ==
             hir::hir_node_kind::hir_list_push,
         "expected the guarded statement to be the push");
}

auto test_lowers_nested_comprehension() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "def products(n: int32, m: int32) -> list[int32]:\n"
                    "    return for x in 0..n, y in 0..m => x * y\n");
  const auto &decl = find_func(*fixture.ast_file, "products");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a nested comprehension to lower");

  const auto &function = **result;
  const auto &ret =
      dynamic_cast<const hir::hir_return &>(*function.body->stmts[0]);
  const auto &comprehension = dynamic_cast<const hir::hir_block &>(*ret.value);
  const auto &outer_loop =
      dynamic_cast<const hir::hir_while &>(*comprehension.stmts[4]);

  // outer loop-var let, then the inner clause's own start/end/index lets
  // and its while (the same range-clause shape reused, nested), then the
  // outer increment — no push at this level.
  expect(outer_loop.body->stmts.size() == 6,
         "expected the outer loop-var let, the inner clause's lets and "
         "while, and the outer increment");
  expect(outer_loop.body->stmts[4]->kind == hir::hir_node_kind::hir_while,
         "expected the second clause to lower to a nested hir_while");
  const auto &inner_loop =
      dynamic_cast<const hir::hir_while &>(*outer_loop.body->stmts[4]);
  expect(inner_loop.body->stmts.size() == 3,
         "expected the inner loop-var let, the push, and the increment");
  expect(inner_loop.body->stmts[1]->kind == hir::hir_node_kind::hir_list_push,
         "expected the push to happen only at the innermost level");
}

auto test_rejects_comprehension_over_user_defined_type() -> void {
  auto fixture = check_fixture("module sample\n"
                               "type counter = { pub value: int32 }\n"
                               "def values(xs: counter) -> list[int32]:\n"
                               "    return for x in xs => x\n");
  const auto &decl = find_func(*fixture.ast_file, "values");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(!result.has_value(),
         "expected a comprehension over a user-defined type to be rejected");
  expect(result.error().kind == hir::lowering_error_kind::unsupported_construct,
         "expected the specific unsupported_construct error kind");
}

auto test_lowers_generator_function() -> void {
  auto fixture =
      check_fixture("module sample\n"
                    "generator def counter() -> some iterator[int32]:\n"
                    "    yield 1\n"
                    "    if true:\n"
                    "        yield 2\n");
  const auto &decl = find_func(*fixture.ast_file, "counter");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a generator function to lower");
  expect((*result)->is_generator,
         "expected the lowered function to be marked as a generator");
  expect(!fixture.checked.types.is_unknown((*result)->item_type),
         "expected the generator's item type to resolve");

  expect((*result)->body->stmts.size() == 2,
         "expected two top-level statements: the first yield and the if");
  expect((*result)->body->stmts.front()->kind == hir::hir_node_kind::hir_yield,
         "expected the first statement to lower directly to hir_yield");

  const auto &tail_stmt =
      dynamic_cast<const hir::hir_expr_stmt &>(*(*result)->body->stmts.back());
  const auto &if_node = dynamic_cast<const hir::hir_if &>(*tail_stmt.expr);
  expect(if_node.branches.size() == 1, "expected a single if branch");
  expect(if_node.branches.front().body->stmts.front()->kind ==
             hir::hir_node_kind::hir_yield,
         "expected the nested yield inside the if branch to also lower to "
         "hir_yield");
}

// A general `some Trait[Args]` existential return type is a checker-only
// fiction (see `semantic::type_kind::existential_kind`'s doc comment):
// lowering must unwrap it back to the real concrete backing type
// everywhere, via `hir::lowerer::resolve_opaque`, so the bytecode/LLVM
// tiers never need to know opacity existed. Confirms both ends of that: the
// producing function's own `hir_function::return_type` is the concrete
// `counter` struct type (not a synthetic existential id), and a caller
// binding the result via `let` gets that same concrete type on its local.
auto test_lowers_existential_return_type_to_concrete_backing_type() -> void {
  auto fixture = check_fixture("module sample\n"
                               "trait describable:\n"
                               "    def describe(self) -> int32\n"
                               "type counter = { pub n: int32 }\n"
                               "impl describable for counter:\n"
                               "    def describe(self) -> int32:\n"
                               "        return self.n\n"
                               "def make_thing() -> some describable:\n"
                               "    return counter{ n: 42 }\n"
                               "def use_thing() -> int32:\n"
                               "    let thing = make_thing()\n"
                               "    return thing.describe()\n");

  const auto &counter_decl = find_func(*fixture.ast_file, "make_thing");
  auto producer = hir::lower_function(counter_decl, fixture.checked);
  expect(producer.has_value(),
         "expected the existential-returning function to lower");
  const auto &counter_entry =
      fixture.checked.types.entry((*producer)->return_type);
  expect(counter_entry.kind == kira::semantic::type_kind::struct_kind &&
             counter_entry.name == "counter",
         "expected the lowered function's return type to be the concrete "
         "`counter` struct, not the existential wrapper");

  const auto &use_decl = find_func(*fixture.ast_file, "use_thing");
  auto consumer = hir::lower_function(use_decl, fixture.checked);
  expect(consumer.has_value(),
         "expected the existential-consuming function to lower");
  const auto &let_node =
      dynamic_cast<const hir::hir_let &>(*(*consumer)->body->stmts.front());
  expect(let_node.initializer->type == (*producer)->return_type,
         "expected the caller's `let thing = make_thing()` to bind the same "
         "concrete backing type the producer returns");
}

// An unproven `pre` becomes a check at the function's entry — before the body
// it guards, and once per call, not once per call site.
auto test_lowers_unproven_precondition_to_entry_check() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def half(x: int32) -> int32\n"
                               "pre x >= 0, \"x must be non-negative\"\n"
                               ": x / 2\n");
  const auto &decl = find_func(*fixture.ast_file, "half");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a function with a `pre` to lower");

  const auto &stmts = (*result)->body->stmts;
  expect(stmts.size() == 2, "expected the entry check plus the body");
  expect(stmts.front()->kind == hir::hir_node_kind::hir_contract_check,
         "expected the precondition check to come first");
  const auto &check =
      dynamic_cast<const hir::hir_contract_check &>(*stmts.front());
  expect(check.kind == hir::contract_kind::precondition,
         "expected the check to be tagged as a precondition");
  // Carried verbatim from the AST, quotes and all (`contract_clause::message`
  // holds the literal's source spelling).
  expect(check.message == "\"x must be non-negative\"",
         "expected the contract's message to survive lowering");
  expect(check.condition->type == fixture.checked.types.bool_type(),
         "expected the lowered condition to be a bool");
  expect(stmts.back()->kind == hir::hir_node_kind::hir_return,
         "expected the body to follow the check");
}

// A `post` is checked at every exit, against the value that exit returns —
// which the condition names `return`.
auto test_lowers_postcondition_at_each_exit() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def clamp_low(x: int32) -> int32\n"
                               "post return >= 0\n"
                               ":\n"
                               "    if x < 0:\n"
                               "        return 0 - x\n"
                               "    x\n");
  const auto &decl = find_func(*fixture.ast_file, "clamp_low");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected a function with a `post` to lower");

  // Each exit expands to `let return = <value>`, the check, then the return.
  const auto count_checks = [](const hir::hir_block &block) -> size_t {
    auto found = size_t{0};
    for (const auto &stmt : block.stmts) {
      if (stmt->kind == hir::hir_node_kind::hir_contract_check) {
        ++found;
      }
    }
    return found;
  };

  const auto &body = *(*result)->body;
  expect(count_checks(body) == 1,
         "expected the fall-off-the-end exit to check the postcondition");
  const auto &early = dynamic_cast<const hir::hir_if &>(*body.stmts.front());
  expect(count_checks(*early.branches.front().body) == 1,
         "expected the early `return` to check the postcondition too");

  // The checked value is the one being returned, bound as `return`.
  const auto &bound = dynamic_cast<const hir::hir_let &>(
      *early.branches.front().body->stmts[0]);
  expect(bound.name == "return",
         "expected the returned value to be bound as `return`");
}

// When every path out of the body is an explicit `return`, nothing falls off
// the end — so no check may be appended there. (Doing so is not merely dead
// code: it puts an instruction after a terminator, which LLVM's verifier
// rejects outright.)
auto test_postcondition_adds_nothing_after_a_diverging_tail() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def abs_val(x: int32) -> int32\n"
                               "post return >= 0\n"
                               ":\n"
                               "    if x < 0:\n"
                               "        return 0 - x\n"
                               "    else:\n"
                               "        return x\n");
  const auto &decl = find_func(*fixture.ast_file, "abs_val");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected the function to lower");

  const auto &stmts = (*result)->body->stmts;
  expect(stmts.size() == 1,
         "expected the body to be exactly the `if`, with nothing appended "
         "after it");
  const auto &tail = *stmts.front();
  expect(tail.kind == hir::hir_node_kind::hir_expr_stmt,
         "expected the tail `if` to be the whole body");
  const auto &branch = dynamic_cast<const hir::hir_if &>(
      *dynamic_cast<const hir::hir_expr_stmt &>(tail).expr);
  expect(branch.branches.front().body->stmts.size() == 3 &&
             branch.branches.front().body->stmts[1]->kind ==
                 hir::hir_node_kind::hir_contract_check,
         "expected each branch's own `return` to carry the check instead");
}

// A `pre` the checker proved from the parameters' own types is not a runtime
// check at all — `positive`'s refinement already guarantees it on every call.
auto test_omits_proved_precondition() -> void {
  auto fixture = check_fixture("module sample\n"
                               "type positive = int32 where self > 0\n"
                               "def scale(p: positive) -> int32\n"
                               "pre p > 0\n"
                               ": p * 2\n");
  const auto &decl = find_func(*fixture.ast_file, "scale");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected the function to lower");
  for (const auto &stmt : (*result)->body->stmts) {
    expect(stmt->kind != hir::hir_node_kind::hir_contract_check,
           "expected a precondition implied by the parameter's refinement to "
           "compile away entirely");
  }
}

// `--no-contract-checks`: the release elision the spec allows.
auto test_contract_checks_can_be_disabled() -> void {
  auto fixture = check_fixture("module sample\n"
                               "def half(x: int32) -> int32\n"
                               "pre x >= 0\n"
                               "post return >= 0\n"
                               ": x / 2\n");
  const auto &decl = find_func(*fixture.ast_file, "half");

  auto result = hir::lower_function(decl, fixture.checked,
                                    hir::lowering_options{
                                        .contract_checks = false,
                                    });
  expect(result.has_value(), "expected the function to lower");
  for (const auto &stmt : (*result)->body->stmts) {
    expect(stmt->kind != hir::hir_node_kind::hir_contract_check,
           "expected `--no-contract-checks` to emit no checks at all");
  }
}

// A struct invariant is checked where the value is made, and again wherever a
// field of it is written.
auto test_lowers_struct_invariant_at_construction_and_mutation() -> void {
  auto fixture = check_fixture("module sample\n"
                               "type positive_int = { pub value: int32 }\n"
                               "    invariant self.value > 0\n"
                               "def make(n: int32) -> int32:\n"
                               "    var p = positive_int{value: n}\n"
                               "    p.value = n\n"
                               "    return p.value\n");
  const auto &decl = find_func(*fixture.ast_file, "make");

  auto result = hir::lower_function(decl, fixture.checked);
  expect(result.has_value(), "expected the function to lower");

  const auto &stmts = (*result)->body->stmts;
  // `let p = { let self = <init>; check; self }`, then the assignment, then a
  // re-bind of `self` and a second check, then the return.
  const auto &binding = dynamic_cast<const hir::hir_let &>(*stmts.front());
  expect(binding.initializer->kind == hir::hir_node_kind::hir_block,
         "expected construction to become a block that checks the invariant "
         "before handing the value back");
  const auto &construction =
      dynamic_cast<const hir::hir_block &>(*binding.initializer);
  expect(construction.stmts.size() == 3 &&
             construction.stmts[1]->kind ==
                 hir::hir_node_kind::hir_contract_check,
         "expected the construction block to bind, check, then yield");
  expect(dynamic_cast<const hir::hir_contract_check &>(*construction.stmts[1])
                 .kind == hir::contract_kind::invariant,
         "expected the check to be tagged as an invariant");

  auto after_write = size_t{0};
  for (size_t i = 0; i < stmts.size(); ++i) {
    if (stmts[i]->kind == hir::hir_node_kind::hir_assign) {
      after_write = i;
    }
  }
  expect(after_write != 0, "expected the field assignment to lower");
  expect(stmts[after_write + 2]->kind == hir::hir_node_kind::hir_contract_check,
         "expected the field write to be followed by a re-bound `self` and a "
         "fresh invariant check");
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
    test_lowers_const_generic_instance_per_constant();
    test_lowers_try_from_on_a_const_generic_refinement();
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
    test_lowers_while_let();
    test_lowers_if_let();
    test_lowers_elif_let_chain();
    test_rejects_for_loop_over_user_defined_type();
    test_lowers_tuple_literal();
    test_lowers_array_literal();
    test_lowers_array_fill_literal();
    test_lowers_struct_literal_explicit_field();
    test_lowers_struct_literal_shorthand_field();
    test_lowers_cast_expression();
    test_lowers_lambda_expression();
    test_lowers_where_expression();
    test_lowers_call_to_named_function();
    test_lowers_module_qualified_call();
    test_lowers_type_qualified_associated_call();
    test_lowers_named_call_arguments_in_declared_order();
    test_rejects_call_relying_on_default_argument();
    test_lowers_variant_construction_with_payload();
    test_lowers_bare_unit_variant();
    test_lowers_try_expr_on_result();
    test_lowers_try_expr_on_option();
    test_lowers_plain_let_as_single_statement();
    test_lowers_tuple_pattern_let_destructuring();
    test_lowers_struct_pattern_let_destructuring();
    test_lowers_tuple_destructuring_parameter();
    test_lowers_struct_destructuring_parameter();
    test_lowers_range_for_loop();
    test_lowers_inclusive_range_for_loop_with_guard();
    test_lowers_list_for_loop();
    test_lowers_array_for_loop_with_static_length();
    test_lowers_str_for_loop_yields_char();
    test_lowers_option_for_loop();
    test_lowers_generator_for_loop();
    test_lowers_simple_comprehension();
    test_lowers_comprehension_with_guard();
    test_lowers_nested_comprehension();
    test_rejects_comprehension_over_user_defined_type();
    test_lowers_generator_function();
    test_lowers_existential_return_type_to_concrete_backing_type();
    test_lowers_unproven_precondition_to_entry_check();
    test_lowers_postcondition_at_each_exit();
    test_postcondition_adds_nothing_after_a_diverging_tail();
    test_omits_proved_precondition();
    test_contract_checks_can_be_disabled();
    test_lowers_struct_invariant_at_construction_and_mutation();
  } catch (const std::exception &ex) {
    std::cerr << "lower_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
