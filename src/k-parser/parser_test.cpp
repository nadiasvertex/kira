#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "parser.h"

namespace {

auto fail(std::string_view message) -> void {
  std::cerr << "parser_test failed: " << message << '\n';
  std::exit(1);
}

auto expect(bool condition, std::string_view message) -> void {
  if (!condition) {
    fail(message);
  }
}

template <typename T>
auto expect_node(kira::ast::node *node, kira::ast::node_kind kind,
                 std::string_view message) -> T * {
  expect(node != nullptr, message);
  expect(node->kind == kind, message);
  return static_cast<T *>(node);
}

template <typename T>
auto expect_expr(kira::ast::expr *expr, kira::ast::node_kind kind,
                 std::string_view message) -> T * {
  expect(expr != nullptr, message);
  expect(expr->kind == kind, message);
  return static_cast<T *>(expr);
}

template <typename T>
auto expect_pattern(kira::ast::pattern *pattern, kira::ast::node_kind kind,
                    std::string_view message) -> T * {
  expect(pattern != nullptr, message);
  expect(pattern->kind == kind, message);
  return static_cast<T *>(pattern);
}

struct parsed_source {
  kira::ast::ptr<kira::ast::file> file;
  std::string diagnostics;
  uint32_t error_count = 0;
};

auto parse_source(std::string_view source) -> parsed_source {
  kira::diagnostic_bag diag;
  kira::source_file file(0, "test.kira", std::string(source));
  kira::Lexer lexer(file.source(), file.id(), diag);
  auto tokens = lexer.tokenize();
  kira::parser parser(std::move(tokens), file.id(), diag);

  parsed_source parsed{
      .file = parser.parse_file(),
      .diagnostics = kira::diagnostic_renderer(file, false).render_all(diag),
      .error_count = diag.error_count(),
  };
  return parsed;
}

auto test_lexer_emits_indent_and_dedent() -> void {
  kira::diagnostic_bag diag;
  std::string source =
      "module sample\n"
      "\n"
      "def run():\n"
      "  let value = 1\n"
      "  return value\n";
  kira::Lexer lexer(source, 0, diag);
  auto tokens = lexer.tokenize();

  expect(!diag.has_errors(), "expected lexer test source to tokenize cleanly");

  const auto indent_count = std::count_if(
      tokens.begin(), tokens.end(), [](const kira::token &token) {
        return token.kind == kira::token_kind::indent;
      });
  const auto dedent_count = std::count_if(
      tokens.begin(), tokens.end(), [](const kira::token &token) {
        return token.kind == kira::token_kind::dedent;
      });

  expect(indent_count == 1, "expected one indent token for function body");
  expect(dedent_count == 1, "expected one dedent token for function body");
}

auto test_parser_builds_type_body_nodes() -> void {
  auto parsed = parse_source(
      "module sample\n"
      "\n"
      "type person = { name: str, age: int }\n"
      "type shape = | circle(float64) | point\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 2, "expected two top-level type declarations");

  auto *person_decl = expect_node<kira::ast::type_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::type_decl,
      "expected first item to be a type declaration");
  auto *person_def = expect_node<kira::ast::struct_type_def>(
      person_decl->definition.get(), kira::ast::node_kind::struct_type_def,
      "expected struct type definition node");
  expect(person_def->body.fields.size() == 2,
         "expected struct body to preserve both fields");
  expect(person_def->body.fields[0].name == "name",
         "expected first struct field name");
  expect(person_def->body.fields[1].name == "age",
         "expected second struct field name");

  auto *shape_decl = expect_node<kira::ast::type_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::type_decl,
      "expected second item to be a type declaration");
  auto *shape_def = expect_node<kira::ast::sum_type_def>(
      shape_decl->definition.get(), kira::ast::node_kind::sum_type_def,
      "expected sum type definition node");
  expect(shape_def->body.variants.size() == 2,
         "expected sum body to preserve both variants");
  expect(shape_def->body.variants[0].name == "circle",
         "expected first sum variant name");
  expect(shape_def->body.variants[0].payload_types.size() == 1,
         "expected payload type for first sum variant");
  expect(shape_def->body.variants[1].name == "point",
         "expected second sum variant name");
}

auto test_parser_preserves_associated_types_where_and_aliases() -> void {
  auto parsed = parse_source(
      "module sample\n"
      "\n"
      "trait iterable:\n"
      "  type item = int\n"
      "\n"
      "impl iterable for int:\n"
      "  type item = int\n"
      "\n"
      "def evaluate(x):\n"
      "  let value = x where:\n"
      "    base = 1\n"
      "  let chosen = match x:\n"
      "    some(item) as alias => alias\n"
      "    _ => value\n"
      "  return chosen\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 3, "expected trait, impl, and function items");

  auto *trait_decl = expect_node<kira::ast::trait_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::trait_decl,
      "expected first item to be a trait declaration");
  expect(trait_decl->items.size() == 1,
         "expected trait to preserve associated type item");
  auto *trait_assoc = expect_node<kira::ast::associated_type_decl_node>(
      trait_decl->items[0].get(), kira::ast::node_kind::associated_type_decl_node,
      "expected trait associated type node");
  expect(trait_assoc->value.name == "item",
         "expected trait associated type name");
  expect(trait_assoc->value.default_type != nullptr,
         "expected trait associated type default type");

  auto *impl_decl = expect_node<kira::ast::impl_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::impl_decl,
      "expected second item to be an impl declaration");
  expect(impl_decl->items.size() == 1,
         "expected impl to preserve associated type item");
  auto *impl_assoc = expect_node<kira::ast::associated_type_def_node>(
      impl_decl->items[0].get(), kira::ast::node_kind::associated_type_def_node,
      "expected impl associated type node");
  expect(impl_assoc->value.name == "item",
         "expected impl associated type name");
  expect(impl_assoc->value.type != nullptr,
         "expected impl associated type definition type");

  auto *func_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[2].get(), kira::ast::node_kind::func_decl,
      "expected third item to be a function declaration");
  expect(func_decl->body_stmts.size() == 3,
         "expected function block body to preserve statements");

  auto *let_stmt = expect_node<kira::ast::let_stmt>(
      func_decl->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
      "expected let statement in function body");
  auto *where_expr = expect_expr<kira::ast::where_expr>(
      let_stmt->initializer.get(), kira::ast::node_kind::where_expr,
      "expected let initializer to be a where expression");
  expect(where_expr->bindings.size() == 1,
         "expected one where binding");
  expect(where_expr->bindings[0].name == "base",
         "expected where binding name");

  auto *chosen_stmt = expect_node<kira::ast::let_stmt>(
      func_decl->body_stmts[1].get(), kira::ast::node_kind::let_stmt,
      "expected second statement to bind match expression");
  auto *match_expr = expect_expr<kira::ast::match_expr>(
      chosen_stmt->initializer.get(), kira::ast::node_kind::match_expr,
      "expected bound value to be a match expression");
  expect(match_expr->arms.size() == 2, "expected two match arms");

  auto *aliased_pattern = expect_pattern<kira::ast::group_pattern>(
      static_cast<kira::ast::pattern *>(match_expr->arms[0].pattern.get()),
      kira::ast::node_kind::group_pattern,
      "expected first match arm to preserve aliased pattern");
  expect(aliased_pattern->alias.has_value(),
         "expected aliased pattern name to be preserved");
  expect(*aliased_pattern->alias == "alias",
         "expected aliased pattern alias name");

  auto *option_pattern = expect_pattern<kira::ast::option_pattern>(
      aliased_pattern->inner.get(), kira::ast::node_kind::option_pattern,
      "expected inner pattern to remain the original option pattern");
  expect(option_pattern->option_kind == kira::ast::OptionResultKind::Some,
         "expected some-pattern to preserve its kind");

  auto *return_stmt = expect_node<kira::ast::return_stmt>(
      func_decl->body_stmts[2].get(), kira::ast::node_kind::return_stmt,
      "expected return statement in function body");
  auto *return_ident = expect_expr<kira::ast::ident_expr>(
      return_stmt->value.get(), kira::ast::node_kind::ident_expr,
      "expected return value to preserve chosen identifier");
  expect(return_ident->name == "chosen", "expected return identifier name");
}

auto test_parser_preserves_function_signature_and_control_flow() -> void {
  auto parsed = parse_source(
      "module sample\n"
      "\n"
      "pub async[ctx] def compute(x: int, y = 1) -> int where int: number: 0\n"
      "def drive(stream, entries):\n"
      "  if x:\n"
      "    return y\n"
      "  elif y:\n"
      "    return x\n"
      "  else:\n"
      "    return 0\n"
      "  while let some(item) = stream:\n"
      "    process(item)\n"
      "  for key, value in entries if ready:\n"
      "    consume(key)\n"
      "  let processed = await source as int?\n"
      "  return y\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 2, "expected two function declarations");

  auto *func_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
      "expected top-level signature function declaration");
  expect(func_decl->visibility == kira::ast::visibility::pub,
         "expected function visibility to be preserved");
  expect(func_decl->modifiers.is_async, "expected async modifier");
  expect(func_decl->modifiers.async_context != nullptr,
         "expected async context type");
  expect(func_decl->params.size() == 2, "expected two function parameters");
  expect(func_decl->params[0].type_annotation != nullptr,
         "expected first parameter type annotation");
  expect(func_decl->params[1].default_value != nullptr,
         "expected second parameter default value");
  expect(func_decl->return_type != nullptr, "expected return type");
  expect(func_decl->where_constraints.size() == 1,
         "expected one where constraint");
  expect(func_decl->body_expr != nullptr,
         "expected first function to preserve inline body");

  auto *drive_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::func_decl,
      "expected second control-flow function declaration");
  expect(drive_decl->body_stmts.size() == 5,
         "expected five statements in function body");

  auto *if_stmt = expect_node<kira::ast::if_stmt>(
      drive_decl->body_stmts[0].get(), kira::ast::node_kind::if_stmt,
      "expected if statement");
  expect(if_stmt->branches.size() == 2, "expected if and elif branches");
  expect(if_stmt->else_body.size() == 1, "expected else body");

  auto *while_stmt = expect_node<kira::ast::while_stmt>(
      drive_decl->body_stmts[1].get(), kira::ast::node_kind::while_stmt,
      "expected while statement");
  expect(while_stmt->let_pattern != nullptr,
         "expected while-let pattern to be preserved");
  expect(while_stmt->let_expr != nullptr,
         "expected while-let expression to be preserved");
  expect(while_stmt->body.size() == 1, "expected single while body statement");

  auto *for_stmt = expect_node<kira::ast::for_stmt>(
      drive_decl->body_stmts[2].get(), kira::ast::node_kind::for_stmt,
      "expected for statement");
  expect(for_stmt->patterns.size() == 2, "expected two for-loop patterns");
  expect(for_stmt->guard != nullptr, "expected for-loop guard");
  expect(for_stmt->body.size() == 1, "expected single for-loop body statement");

  auto *processed_stmt = expect_node<kira::ast::let_stmt>(
      drive_decl->body_stmts[3].get(), kira::ast::node_kind::let_stmt,
      "expected processed binding statement");
  auto *await_expr = expect_expr<kira::ast::await_expr>(
      processed_stmt->initializer.get(), kira::ast::node_kind::await_expr,
      "expected await-expression initializer");
  auto *try_expr = expect_expr<kira::ast::try_expr>(
      await_expr->operand.get(), kira::ast::node_kind::try_expr,
      "expected try expression inside await expression");
  auto *cast_expr = expect_expr<kira::ast::cast_expr>(
      try_expr->operand.get(), kira::ast::node_kind::cast_expr,
      "expected cast expression inside try expression");
  expect(cast_expr->target_type != nullptr, "expected cast target type");

  auto *return_stmt = expect_node<kira::ast::return_stmt>(
      drive_decl->body_stmts[4].get(), kira::ast::node_kind::return_stmt,
      "expected final return statement");
  expect(return_stmt->value != nullptr, "expected final return value");
}

auto test_parser_preserves_trait_impl_and_block_expressions() -> void {
  auto parsed = parse_source(
      "module sample\n"
      "\n"
      "trait worker[T] requires runnable + sendable:\n"
      "  pub static helper = seed\n"
      "  def process(item: T) -> int: item\n"
      "\n"
      "impl worker[int] for int where int: runnable:\n"
      "  static counter = 0\n"
      "  def process(item: int) -> int: item\n"
      "\n"
      "def orchestrate(source, ctx) -> int:\n"
      "  let fanout = par:\n"
      "    source\n"
      "    source\n"
      "  let winner = race:\n"
      "    source\n"
      "    source\n"
      "  let handled = on(int, ctx):\n"
      "    return source\n"
      "  return source\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 3,
         "expected trait, impl, and function declarations");

  auto *trait_decl = expect_node<kira::ast::trait_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::trait_decl,
      "expected trait declaration");
  expect(trait_decl->type_params.size() == 1, "expected trait type parameter");
  expect(trait_decl->requires_bound.has_value(), "expected trait requires bound");
  expect(trait_decl->items.size() == 2, "expected static and function trait items");
  auto *trait_static = expect_node<kira::ast::static_decl>(
      trait_decl->items[0].get(), kira::ast::node_kind::static_decl,
      "expected trait static item");
  expect(trait_static->visibility == kira::ast::visibility::pub,
         "expected trait static visibility");
  auto *trait_func = expect_node<kira::ast::func_decl>(
      trait_decl->items[1].get(), kira::ast::node_kind::func_decl,
      "expected trait function item");
  expect(trait_func->return_type != nullptr, "expected trait function return type");

  auto *impl_decl = expect_node<kira::ast::impl_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::impl_decl,
      "expected impl declaration");
  expect(impl_decl->where_constraints.size() == 1,
         "expected impl where constraint");
  expect(impl_decl->items.size() == 2, "expected static and function impl items");

  auto *func_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[2].get(), kira::ast::node_kind::func_decl,
      "expected orchestrate function declaration");
  expect(func_decl->body_stmts.size() == 4,
         "expected four statements in orchestrate body");

  auto *par_binding = expect_node<kira::ast::let_stmt>(
      func_decl->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
      "expected par-binding let statement");
  auto *par_expr = expect_expr<kira::ast::par_expr>(
      par_binding->initializer.get(), kira::ast::node_kind::par_expr,
      "expected par expression initializer");
  expect(par_expr->branches.size() == 2, "expected two par branches");

  auto *race_binding = expect_node<kira::ast::let_stmt>(
      func_decl->body_stmts[1].get(), kira::ast::node_kind::let_stmt,
      "expected race-binding let statement");
  auto *race_expr = expect_expr<kira::ast::race_expr>(
      race_binding->initializer.get(), kira::ast::node_kind::race_expr,
      "expected race expression initializer");
  expect(race_expr->branches.size() == 2, "expected two race branches");

  auto *on_binding = expect_node<kira::ast::let_stmt>(
      func_decl->body_stmts[2].get(), kira::ast::node_kind::let_stmt,
      "expected on-binding let statement");
  auto *on_expr = expect_expr<kira::ast::on_expr>(
      on_binding->initializer.get(), kira::ast::node_kind::on_expr,
      "expected on expression initializer");
  expect(on_expr->context_type != nullptr, "expected on-expression context type");
  expect(on_expr->sender != nullptr, "expected on-expression sender");
  expect(on_expr->body.size() == 1, "expected on-expression body statement");

  auto *return_stmt = expect_node<kira::ast::return_stmt>(
      func_decl->body_stmts[3].get(), kira::ast::node_kind::return_stmt,
      "expected final return statement");
  expect(return_stmt->value != nullptr, "expected final return value");
}

auto test_parser_reports_missing_module_and_recovers() -> void {
  auto parsed = parse_source(
      "def greet(name):\n"
      "  return name\n");

  expect(parsed.error_count > 0,
         "expected parser to diagnose missing module declaration");
  expect(parsed.diagnostics.find("every Kira source file must start with a `module` declaration") !=
             std::string::npos,
         "expected missing-module diagnostic message");
  expect(parsed.file->module_decl != nullptr,
         "expected synthesized module declaration during recovery");
  expect(parsed.file->module_decl->has_error,
         "expected synthesized module declaration to be marked erroneous");
  expect(parsed.file->items.size() == 1,
         "expected parser to recover and preserve following function");
  auto *func_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
      "expected recovered function declaration");
  expect(func_decl->name == "greet", "expected recovered function name");
}

struct named_test {
  const char *name;
  void (*fn)();
};

} // namespace

auto main(int argc, char *argv[]) -> int {
  const named_test tests[] = {
      {"lexer_indent_dedent", test_lexer_emits_indent_and_dedent},
      {"type_body_nodes", test_parser_builds_type_body_nodes},
      {"associated_types_where_aliases",
       test_parser_preserves_associated_types_where_and_aliases},
      {"function_signature_and_control_flow",
       test_parser_preserves_function_signature_and_control_flow},
      {"trait_impl_and_block_expressions",
       test_parser_preserves_trait_impl_and_block_expressions},
      {"missing_module_recovery", test_parser_reports_missing_module_and_recovers},
  };

  if (argc > 1) {
    for (const auto &test : tests) {
      if (std::strcmp(argv[1], test.name) == 0) {
        test.fn();
        return 0;
      }
    }
    fail("unknown test name");
  }

  for (const auto &test : tests) {
    test.fn();
  }
  return 0;
}
