#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "parser.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;
using kira::testing::fail;

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
  auto sources = kira::source_manager{};
  auto file_id = sources.add_file("test.kira", std::string(source));
  expect(file_id.has_value(), "expected test source to register");

  auto *file = sources.get(*file_id);
  expect(file != nullptr, "expected registered test source");

  kira::lexer lexer(file->source(), file->id(), diag);
  auto tokens = lexer.tokenize();
  kira::parser parser(std::move(tokens), file->id(), diag);

  parsed_source parsed{
      .file = parser.parse_file(),
      .diagnostics = kira::diagnostic_renderer(sources, false).render_all(diag),
      .error_count = diag.error_count(),
  };
  return parsed;
}

auto test_lexer_emits_indent_and_dedent() -> void {
  kira::diagnostic_bag diag;
  std::string source = "module sample\n"
                       "\n"
                       "def run():\n"
                       "  let value = 1\n"
                       "  return value\n";
  kira::lexer lexer(source, 0, diag);
  auto tokens = lexer.tokenize();

  expect(!diag.has_errors(), "expected lexer test source to tokenize cleanly");

  const auto indent_count = std::count_if(
      tokens.begin(), tokens.end(), [](const kira::token &token) -> bool {
        return token.kind == kira::token_kind::indent;
      });
  const auto dedent_count = std::count_if(
      tokens.begin(), tokens.end(), [](const kira::token &token) -> bool {
        return token.kind == kira::token_kind::dedent;
      });

  expect(indent_count == 1, "expected one indent token for function body");
  expect(dedent_count == 1, "expected one dedent token for function body");
}

auto test_parser_builds_type_body_nodes() -> void {
  auto parsed = parse_source("module sample\n"
                             "\n"
                             "type person = { name: str, age: int }\n"
                             "type shape = | @circle(float64) | @point\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 2,
         "expected two top-level type declarations");

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

auto test_parser_captures_doc_comments() -> void {
  auto parsed = parse_source("#: The sample module.\n"
                             "module sample\n"
                             "\n"
                             "#: Adds two numbers.\n"
                             "#: Wraps on overflow.\n"
                             "def add(a: int, b: int) -> int:\n"
                             "    a + b\n"
                             "\n"
                             "# an ordinary comment\n"
                             "def plain() -> int:\n"
                             "    0\n"
                             "\n"
                             "#: A person.\n"
                             "type person = {\n"
                             "    #: The person's name.\n"
                             "    name: str,\n"
                             "    age: int,\n"
                             "}\n"
                             "\n"
                             "#: A shape.\n"
                             "trait shape:\n"
                             "    #: Returns the area.\n"
                             "    def area(self) -> int\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 4, "expected four top-level items");

  // Module docstring lives on the module declaration.
  expect(parsed.file->module_decl != nullptr, "expected a module declaration");
  expect(parsed.file->module_decl->documentation == "The sample module.",
         "expected module docstring on module_decl");

  // Multi-line doc comment concatenates with `\n`.
  auto *add_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
      "expected first item to be a function");
  expect(add_decl->documentation == "Adds two numbers.\nWraps on overflow.",
         "expected joined multi-line docstring on `add`");

  // A plain `#` comment leaves no documentation.
  auto *plain_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::func_decl,
      "expected second item to be a function");
  expect(plain_decl->documentation.empty(),
         "expected no docstring from an ordinary `#` comment");

  // Type docstring, plus a field docstring inside the `{ ... }` body.
  auto *person_decl = expect_node<kira::ast::type_decl>(
      parsed.file->items[2].get(), kira::ast::node_kind::type_decl,
      "expected third item to be a type declaration");
  expect(person_decl->documentation == "A person.",
         "expected docstring on `person` type");
  auto *person_def = expect_node<kira::ast::struct_type_def>(
      person_decl->definition.get(), kira::ast::node_kind::struct_type_def,
      "expected struct type definition node");
  expect(person_def->body.fields.size() == 2, "expected two struct fields");
  expect(person_def->body.fields[0].documentation == "The person's name.",
         "expected docstring on the `name` field");
  expect(person_def->body.fields[1].documentation.empty(),
         "expected no docstring on the undocumented `age` field");

  // Trait docstring, plus a docstring on a nested method.
  auto *shape_decl = expect_node<kira::ast::trait_decl>(
      parsed.file->items[3].get(), kira::ast::node_kind::trait_decl,
      "expected fourth item to be a trait declaration");
  expect(shape_decl->documentation == "A shape.",
         "expected docstring on `shape` trait");
  expect(shape_decl->items.size() == 1, "expected one trait member");
  expect(shape_decl->items[0]->documentation == "Returns the area.",
         "expected docstring on the nested `area` method");
}

auto test_parser_preserves_associated_types_where_and_aliases() -> void {
  auto parsed = parse_source("module sample\n"
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
                             "    @some(item) as alias => alias\n"
                             "    _ => value\n"
                             "  return chosen\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 3,
         "expected trait, impl, and function items");

  auto *trait_decl = expect_node<kira::ast::trait_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::trait_decl,
      "expected first item to be a trait declaration");
  expect(trait_decl->items.size() == 1,
         "expected trait to preserve associated type item");
  auto *trait_assoc = expect_node<kira::ast::associated_type_decl_node>(
      trait_decl->items[0].get(),
      kira::ast::node_kind::associated_type_decl_node,
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
  expect(where_expr->bindings.size() == 1, "expected one where binding");
  expect(where_expr->bindings[0].name == "base", "expected where binding name");

  auto *chosen_stmt = expect_node<kira::ast::let_stmt>(
      func_decl->body_stmts[1].get(), kira::ast::node_kind::let_stmt,
      "expected second statement to bind match expression");
  auto *match_expr = expect_expr<kira::ast::match_expr>(
      chosen_stmt->initializer.get(), kira::ast::node_kind::match_expr,
      "expected bound value to be a match expression");
  expect(match_expr->arms.size() == 2, "expected two match arms");

  auto *aliased_pattern = expect_pattern<kira::ast::group_pattern>(
      dynamic_cast<kira::ast::pattern *>(match_expr->arms[0].pattern.get()),
      kira::ast::node_kind::group_pattern,
      "expected first match arm to preserve aliased pattern");
  expect(aliased_pattern->alias.has_value(),
         "expected aliased pattern name to be preserved");
  if (aliased_pattern->alias.has_value()) {
    expect(*aliased_pattern->alias == "alias",
           "expected aliased pattern alias name");
  }

  auto *option_pattern = expect_pattern<kira::ast::option_pattern>(
      aliased_pattern->inner.get(), kira::ast::node_kind::option_pattern,
      "expected inner pattern to remain the original option pattern");
  expect(option_pattern->option_kind == kira::ast::option_result_kind::some,
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
      "  let branch = if x: y else: 0\n"
      "  if x:\n"
      "    return y\n"
      "  elif y:\n"
      "    return x\n"
      "  else:\n"
      "    return 0\n"
      "  while let @some(item) = stream:\n"
      "    process(item)\n"
      "  for key, value in entries if ready:\n"
      "    consume(key)\n"
      "  let produced = for entry in entries if ready => entry\n"
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
  expect(drive_decl->body_stmts.size() == 7,
         "expected seven statements in function body");

  auto *branch_stmt = expect_node<kira::ast::let_stmt>(
      drive_decl->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
      "expected branch binding statement");
  auto *if_expr = expect_expr<kira::ast::if_expr>(
      branch_stmt->initializer.get(), kira::ast::node_kind::if_expr,
      "expected inline if-expression initializer");
  expect(if_expr->branches.size() == 1,
         "expected single branch in inline if-expression");
  expect(if_expr->else_body.size() == 1,
         "expected else body in inline if-expression");

  auto *if_stmt = expect_node<kira::ast::if_stmt>(
      drive_decl->body_stmts[1].get(), kira::ast::node_kind::if_stmt,
      "expected if statement");
  expect(if_stmt->branches.size() == 2, "expected if and elif branches");
  expect(if_stmt->else_body.size() == 1, "expected else body");

  auto *while_stmt = expect_node<kira::ast::while_stmt>(
      drive_decl->body_stmts[2].get(), kira::ast::node_kind::while_stmt,
      "expected while statement");
  expect(while_stmt->let_pattern != nullptr,
         "expected while-let pattern to be preserved");
  expect(while_stmt->let_expr != nullptr,
         "expected while-let expression to be preserved");
  expect(while_stmt->body.size() == 1, "expected single while body statement");

  auto *for_stmt = expect_node<kira::ast::for_stmt>(
      drive_decl->body_stmts[3].get(), kira::ast::node_kind::for_stmt,
      "expected for statement");
  expect(for_stmt->patterns.size() == 2, "expected two for-loop patterns");
  expect(for_stmt->guard != nullptr, "expected for-loop guard");
  expect(for_stmt->body.size() == 1, "expected single for-loop body statement");

  auto *produced_stmt = expect_node<kira::ast::let_stmt>(
      drive_decl->body_stmts[4].get(), kira::ast::node_kind::let_stmt,
      "expected produced binding statement");
  auto *for_expr = expect_expr<kira::ast::for_expr>(
      produced_stmt->initializer.get(), kira::ast::node_kind::for_expr,
      "expected guarded for-expression initializer");
  expect(for_expr->clauses.size() == 1, "expected one for-expression clause");
  expect(for_expr->guard != nullptr, "expected guarded for-expression guard");
  expect(for_expr->yield_expr != nullptr,
         "expected guarded for-expression yield value");

  auto *processed_stmt = expect_node<kira::ast::let_stmt>(
      drive_decl->body_stmts[5].get(), kira::ast::node_kind::let_stmt,
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
      drive_decl->body_stmts[6].get(), kira::ast::node_kind::return_stmt,
      "expected final return statement");
  expect(return_stmt->value != nullptr, "expected final return value");
}

/// `static def` is a *function* wherever a static binding is also legal.
///
/// `kira-grammar.ebnf` lists `static` as an unrestricted `func_modifier`, but
/// the parser used to honor it only at module scope — a `trait` body read
/// `static` as the start of `static NAME: Type` and failed on the `def`.
/// The disambiguation (`parser::at_static_func_decl`) has to look past a run
/// of modifiers, so the interesting cases are the ones where `def` is not the
/// very next token.
auto test_parser_accepts_static_def_in_member_blocks() -> void {
  auto parsed = parse_source("module sample\n"
                             "\n"
                             "trait maker:\n"
                             "  static def make(v: int) -> self\n"
                             "  static def build(v: int) -> self\n"
                             "\n"
                             "impl maker for int:\n"
                             "  static def make(v: int) -> int: v\n"
                             "  static def build(v: int) -> int: v\n"
                             "\n"
                             "extend int:\n"
                             "  static def zero() -> int: 0\n");

  expect(parsed.error_count == 0, parsed.diagnostics);

  auto *trait_decl = expect_node<kira::ast::trait_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::trait_decl,
      "expected trait declaration");
  expect(trait_decl->items.size() == 2, "expected two trait members");
  for (const auto &item : trait_decl->items) {
    auto *method = expect_node<kira::ast::func_decl>(
        item.get(), kira::ast::node_kind::func_decl,
        "a `static def` in a trait is a function, not a static binding");
    expect(method->modifiers.is_static,
           "the `static` modifier must survive onto the declaration");
  }

  auto *impl_decl = expect_node<kira::ast::impl_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::impl_decl,
      "expected impl declaration");
  expect(impl_decl->items.size() == 2, "expected two impl members");
  for (const auto &item : impl_decl->items) {
    auto *method = expect_node<kira::ast::func_decl>(
        item.get(), kira::ast::node_kind::func_decl,
        "a `static def` in an impl is a function, not a static binding");
    expect(method->modifiers.is_static,
           "the `static` modifier must survive onto the declaration");
  }

  auto *extend_decl = expect_node<kira::ast::extend_decl>(
      parsed.file->items[2].get(), kira::ast::node_kind::extend_decl,
      "expected extend declaration");
  expect(extend_decl->items.size() == 1, "expected one extend member");
  auto *extend_method = expect_node<kira::ast::func_decl>(
      extend_decl->items[0].get(), kira::ast::node_kind::func_decl,
      "a `static def` in an extend block is a function");
  expect(extend_method->modifiers.is_static,
         "the `static` modifier must survive onto the declaration");
}

/// The other side of the same disambiguation: `static` followed by anything
/// that is not eventually a `def` is still a static *binding*. Without this,
/// a lookahead that guessed "function" too eagerly would break every existing
/// `static counter = 0` and nothing above would notice.
auto test_parser_still_reads_static_bindings_as_bindings() -> void {
  auto parsed = parse_source("module sample\n"
                             "\n"
                             "impl worker for int:\n"
                             "  static counter = 0\n"
                             "  static def make(v: int) -> int: v\n");

  expect(parsed.error_count == 0, parsed.diagnostics);

  auto *impl_decl = expect_node<kira::ast::impl_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::impl_decl,
      "expected impl declaration");
  expect(impl_decl->items.size() == 2, "expected binding and function");
  expect_node<kira::ast::static_decl>(
      impl_decl->items[0].get(), kira::ast::node_kind::static_decl,
      "`static counter = 0` is a binding, not a function");
  expect_node<kira::ast::func_decl>(
      impl_decl->items[1].get(), kira::ast::node_kind::func_decl,
      "`static def` beside a binding is still a function");
}

auto test_parser_preserves_trait_impl_and_block_expressions() -> void {
  auto parsed = parse_source("module sample\n"
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
                             "  let handled = on(int, ctx): source\n"
                             "  return source\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 3,
         "expected trait, impl, and function declarations");

  auto *trait_decl = expect_node<kira::ast::trait_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::trait_decl,
      "expected trait declaration");
  expect(trait_decl->type_params.size() == 1, "expected trait type parameter");
  expect(trait_decl->requires_bound.has_value(),
         "expected trait requires bound");
  expect(trait_decl->items.size() == 2,
         "expected static and function trait items");
  auto *trait_static = expect_node<kira::ast::static_decl>(
      trait_decl->items[0].get(), kira::ast::node_kind::static_decl,
      "expected trait static item");
  expect(trait_static->visibility == kira::ast::visibility::pub,
         "expected trait static visibility");
  auto *trait_func = expect_node<kira::ast::func_decl>(
      trait_decl->items[1].get(), kira::ast::node_kind::func_decl,
      "expected trait function item");
  expect(trait_func->return_type != nullptr,
         "expected trait function return type");

  auto *impl_decl = expect_node<kira::ast::impl_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::impl_decl,
      "expected impl declaration");
  expect(impl_decl->where_constraints.size() == 1,
         "expected impl where constraint");
  expect(impl_decl->items.size() == 2,
         "expected static and function impl items");

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
  expect(on_expr->context_type != nullptr,
         "expected on-expression context type");
  expect(on_expr->sender != nullptr, "expected on-expression sender");
  expect(on_expr->body.size() == 1, "expected on-expression body statement");
  auto *on_stmt_expr = expect_node<kira::ast::expr_stmt>(
      on_expr->body[0].get(), kira::ast::node_kind::expr_stmt,
      "expected inline on-expression body to become an expression statement");
  expect(on_stmt_expr->expr != nullptr,
         "expected preserved inline expression inside on-expression body");

  auto *return_stmt = expect_node<kira::ast::return_stmt>(
      func_decl->body_stmts[3].get(), kira::ast::node_kind::return_stmt,
      "expected final return statement");
  expect(return_stmt->value != nullptr, "expected final return value");
}

auto test_parser_reports_missing_module_and_recovers() -> void {
  auto parsed = parse_source("def greet(name):\n"
                             "  return name\n");

  expect(parsed.error_count > 0,
         "expected parser to diagnose missing module declaration");
  expect(parsed.diagnostics.find(
             "every Kira source file must start with a `module` declaration") !=
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

auto test_parser_recovers_missing_colon_in_where_clause() -> void {
  auto parsed = parse_source("module sample\n"
                             "\n"
                             "def compute(x):\n"
                             "  let value = x where\n"
                             "    base = 1\n"
                             "  return value\n");

  expect(parsed.error_count > 0,
         "expected malformed where clause to produce diagnostics");
  expect(
      parsed.diagnostics.find("expected `:` after `where` but found newline") !=
          std::string::npos,
      "expected precise missing-colon diagnostic for where clause");
  expect(parsed.diagnostics.find(
             "Write it as `expr where:` followed by an indented block") !=
             std::string::npos,
         "expected recovery help text for malformed where clause");

  auto *func_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
      "expected function declaration despite malformed where clause");
  auto *let_stmt = expect_node<kira::ast::let_stmt>(
      func_decl->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
      "expected let statement despite malformed where clause");
  auto *where_expr = expect_expr<kira::ast::where_expr>(
      let_stmt->initializer.get(), kira::ast::node_kind::where_expr,
      "expected recovered where-expression node");
  expect(where_expr->has_error,
         "expected recovered where-expression to be marked erroneous");
  expect(where_expr->bindings.size() == 1,
         "expected recovered where-expression binding");
  expect(where_expr->bindings[0].name == "base",
         "expected recovered where binding name");
}

auto test_parser_accepts_spec_valid_regressions() -> void {
  auto parsed = parse_source("module sample\n"
                             "\n"
                             "use std.io.reader as rdr\n"
                             "\n"
                             "concept ready[T]:\n"
                             "  value + 1\n"
                             "\n"
                             "static for item in items => item\n"
                             "\n"
                             "def run(flag, items):\n"
                             "  let label = \"pass\" if flag else \"fail\"\n"
                             "  let point = point { x: 1, y: 2 }\n"
                             "  let produced = for item in items => item\n"
                             "  return produced\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 4,
         "expected use, concept, static, and function items");

  auto *use_decl = expect_node<kira::ast::use_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::use_decl,
      "expected first item to be a use declaration");
  expect(use_decl->path.size() == 2,
         "expected aliased import path to keep the module path only");
  expect(use_decl->path[0] == "std", "expected first import path segment");
  expect(use_decl->path[1] == "io", "expected second import path segment");
  expect(use_decl->selector.has_value(),
         "expected aliased import to produce a selector");
  if (use_decl->selector.has_value()) {
    const auto &selector = *use_decl->selector;
    expect(selector.kind == kira::ast::use_selector_kind::single,
           "expected aliased import selector kind");
    expect(selector.items.size() == 1,
           "expected one imported item in aliased import");
    expect(selector.items[0].name == "reader", "expected imported item name");
    const auto &alias = selector.items[0].alias;
    expect(alias.has_value(), "expected imported item alias");
    if (alias.has_value()) {
      expect(*alias == "rdr", "expected imported item alias name");
    }
  }

  auto *concept_decl = expect_node<kira::ast::concept_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::concept_decl,
      "expected second item to be a concept declaration");
  expect(concept_decl->constraints.size() == 1,
         "expected one concept constraint");
  expect(concept_decl->constraints[0].subject == nullptr,
         "expected value constraint to leave type subject empty");
  auto *concept_expr = expect_node<kira::ast::binary_expr>(
      concept_decl->constraints[0].bound_or_expr.get(),
      kira::ast::node_kind::binary_expr,
      "expected concept value constraint expression");
  expect(concept_expr->op == kira::ast::binary_op::add,
         "expected concept value constraint to preserve addition");

  auto *static_decl = expect_node<kira::ast::static_decl>(
      parsed.file->items[2].get(), kira::ast::node_kind::static_decl,
      "expected third item to be a static declaration");
  expect(static_decl->decl_kind == kira::ast::static_decl_kind::for_inline,
         "expected static for inline declaration kind");
  auto *static_iterable = expect_expr<kira::ast::ident_expr>(
      static_decl->for_iterable.get(), kira::ast::node_kind::ident_expr,
      "expected bare identifier iterable in static for");
  expect(static_iterable->name == "items",
         "expected static for iterable identifier name");

  auto *func_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[3].get(), kira::ast::node_kind::func_decl,
      "expected final item to be a function declaration");
  expect(func_decl->body_stmts.size() == 4,
         "expected four statements in regression function");

  auto *label_stmt = expect_node<kira::ast::let_stmt>(
      func_decl->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
      "expected label binding statement");
  auto *label_if = expect_expr<kira::ast::if_expr>(
      label_stmt->initializer.get(), kira::ast::node_kind::if_expr,
      "expected trailing conditional expression to parse as if-expression");
  expect(label_if->branches.size() == 1,
         "expected one branch in trailing conditional expression");
  expect(label_if->else_body.size() == 1,
         "expected else body in trailing conditional expression");

  auto *point_stmt = expect_node<kira::ast::let_stmt>(
      func_decl->body_stmts[1].get(), kira::ast::node_kind::let_stmt,
      "expected point binding statement");
  auto *point_expr = expect_expr<kira::ast::struct_expr>(
      point_stmt->initializer.get(), kira::ast::node_kind::struct_expr,
      "expected typed struct literal initializer");
  expect(point_expr->type_name != nullptr,
         "expected typed struct literal to preserve its type name");
  auto *point_type = expect_expr<kira::ast::ident_expr>(
      point_expr->type_name.get(), kira::ast::node_kind::ident_expr,
      "expected typed struct literal type name expression");
  expect(point_type->name == "point",
         "expected typed struct literal type name");
  expect(point_expr->fields.size() == 2,
         "expected both typed struct literal fields");

  auto *produced_stmt = expect_node<kira::ast::let_stmt>(
      func_decl->body_stmts[2].get(), kira::ast::node_kind::let_stmt,
      "expected produced binding statement");
  auto *produced_expr = expect_expr<kira::ast::for_expr>(
      produced_stmt->initializer.get(), kira::ast::node_kind::for_expr,
      "expected bare iterable for-expression initializer");
  expect(produced_expr->clauses.size() == 1,
         "expected one for-expression clause");
  auto *produced_iterable = expect_expr<kira::ast::ident_expr>(
      produced_expr->clauses[0].iterable.get(),
      kira::ast::node_kind::ident_expr,
      "expected bare identifier iterable in for-expression");
  expect(produced_iterable->name == "items",
         "expected for-expression iterable identifier name");
}

auto test_parser_accepts_remaining_phase1_constructs() -> void {
  auto parsed = parse_source(
      "module sample\n"
      "\n"
      "type direction = @north | @south | @east | @west\n"
      "type option[T] = @some(T) | @none\n"
      "\n"
      "trait functor[F[_]]:\n"
      "  def map[A, B](fa: F[A], f: fn(A) -> B) -> F[B]\n"
      "\n"
      "static pure def align_up(n: usize, align: usize) -> usize:\n"
      "  (n + align - 1) & ~(align - 1)\n"
      "\n"
      "static def derive_show[T]() -> def_expr:\n"
      "  let fields = T.fields()\n"
      "  let target = describe[T]()\n"
      "  return `impl show for point:`\n"
      "\n"
      "static let increment: expr = `(x + 1)`\n"
      "\n"
      "def run[T: show + eq, n: usize](raw_index, items) -> option[value]:\n"
      "  let shared_cfg: shared config_t = shared load_config(\"app.toml\")\n"
      "  if let @some(i) = index[n].try_from(raw_index):\n"
      "    let sender = channel[str](capacity: 32)\n"
      "    let watcher = watch[config](initial: default_config())\n"
      "    let evens = for x in 0..20 if x % 2 == 0 => x\n"
      "    let handle = crew c(on_error: collect):\n"
      "      c.spawn(async:\n"
      "        while let @some(line) = await receiver.recv():\n"
      "          process(line)\n"
      "      )\n"
      "    match @some(items):\n"
      "      @some(value) => return @some(value)\n"
      "      @none => return @none\n"
      "  return @err(@cancelled)\n"
      "\n"
      "~derive_show[point]()\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 8,
         "expected full phase 1 regression source to parse cleanly");

  auto *direction_decl = expect_node<kira::ast::type_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::type_decl,
      "expected direction type declaration");
  auto *direction_sum = expect_node<kira::ast::sum_type_def>(
      direction_decl->definition.get(), kira::ast::node_kind::sum_type_def,
      "expected unprefixed sum type to parse as sum body");
  expect(direction_sum->body.variants.size() == 4,
         "expected all direction variants to be preserved");

  auto *option_decl = expect_node<kira::ast::type_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::type_decl,
      "expected option type declaration");
  auto *option_sum = expect_node<kira::ast::sum_type_def>(
      option_decl->definition.get(), kira::ast::node_kind::sum_type_def,
      "expected payload sum type to parse as sum body");
  expect(option_sum->body.variants.size() == 2,
         "expected option payload and nullary variants");

  auto *trait_decl = expect_node<kira::ast::trait_decl>(
      parsed.file->items[2].get(), kira::ast::node_kind::trait_decl,
      "expected higher-kinded trait declaration");
  expect(trait_decl->type_params.size() == 1,
         "expected higher-kinded trait parameter");
  expect(trait_decl->type_params[0].higher_kinded_arity == 1,
         "expected higher-kinded trait parameter arity of 1");

  auto *static_pure_func = expect_node<kira::ast::func_decl>(
      parsed.file->items[3].get(), kira::ast::node_kind::func_decl,
      "expected static pure def to parse as function declaration");
  expect(static_pure_func->modifiers.is_static,
         "expected static function modifier to be preserved");
  expect(static_pure_func->modifiers.is_pure,
         "expected pure function modifier to be preserved");

  auto *static_def = expect_node<kira::ast::func_decl>(
      parsed.file->items[4].get(), kira::ast::node_kind::func_decl,
      "expected static def to parse as function declaration");
  expect(static_def->modifiers.is_static,
         "expected static def modifier to be preserved");

  auto *static_let = expect_node<kira::ast::static_decl>(
      parsed.file->items[5].get(), kira::ast::node_kind::static_decl,
      "expected static let to parse as static declaration");
  expect(static_let->decl_kind == kira::ast::static_decl_kind::binding,
         "expected static let binding kind");

  auto *run_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[6].get(), kira::ast::node_kind::func_decl,
      "expected run function declaration");
  expect(run_decl->type_params.size() == 2,
         "expected bounded and value type parameters");
  expect(run_decl->type_params[1].is_value_param,
         "expected value type parameter to be preserved");
  expect(run_decl->return_type != nullptr,
         "expected super-qualified return type");

  expect(parsed.file->items[7]->kind == kira::ast::node_kind::splice_stmt,
         "expected top-level derive splice statement");
}

auto test_parser_accepts_if_let_expression() -> void {
  auto parsed = parse_source(
      "module sample\n"
      "\n"
      "def run(v: option[int], w: option[int]) -> int:\n"
      "  let n = if let @some(x) = v: x elif let @some(y) = w: y else: 0\n"
      "  return n\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 1,
         "expected a single top-level function declaration");

  auto *run_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
      "expected run function declaration");
  expect(run_decl->body_stmts.size() == 2,
         "expected two statements in function body");

  auto *let_stmt = expect_node<kira::ast::let_stmt>(
      run_decl->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
      "expected let-binding statement");
  auto *if_expr = expect_expr<kira::ast::if_expr>(
      let_stmt->initializer.get(), kira::ast::node_kind::if_expr,
      "expected `if let` initializer to parse as an if-expression");

  expect(if_expr->branches.size() == 2,
         "expected the `if let` and `elif let` branches");
  expect(if_expr->else_body.size() == 1, "expected the `else` body");

  const auto &if_branch = if_expr->branches[0];
  expect(if_branch.let_pattern != nullptr,
         "expected `if let` pattern to be preserved");
  expect(if_branch.let_expr != nullptr,
         "expected `if let` scrutinee to be preserved");
  expect(if_branch.body.size() == 1, "expected the `if let` branch body");

  const auto &elif_branch = if_expr->branches[1];
  expect(elif_branch.let_pattern != nullptr,
         "expected `elif let` pattern to be preserved");
  expect(elif_branch.let_expr != nullptr,
         "expected `elif let` scrutinee to be preserved");
}

auto test_parser_accepts_multi_arity_higher_kinded_params() -> void {
  auto parsed =
      parse_source("module sample\n"
                   "\n"
                   "trait bifunctor[F[_, _]]:\n"
                   "    def bimap[A, B, C, D](fab: F[A, B], f: fn(A) -> C,"
                   " g: fn(B) -> D) -> F[C, D]\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  auto *trait_decl = expect_node<kira::ast::trait_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::trait_decl,
      "expected bifunctor trait declaration");
  expect(trait_decl->type_params.size() == 1,
         "expected one trait type parameter");
  expect(trait_decl->type_params[0].higher_kinded_arity == 2,
         "expected `F[_, _]` to record a higher-kinded arity of 2");
  expect(trait_decl->items.size() == 1, "expected the bimap signature");
}

auto test_parser_accepts_phase1_audit_regressions() -> void {
  auto parsed = parse_source("module sample\n"
                             "\n"
                             "# leading comment\n"
                             "internal use std.io\n"
                             "super module parent_only\n"
                             "static search_path = [\"src\", \"vendor\"]\n"
                             "\n"
                             "trait monad[M[_]]:\n"
                             "  def pure[A](a: A) -> M[A]\n"
                             "\n"
                             "impl monad[option]:\n"
                             "  def pure[A](a: A) -> option[A]: @some(a)\n"
                             "\n"
                             "async def handle(pool, req) -> http_response:\n"
                             "  let result = await on(pool):\n"
                             "    expensive_computation(req.body)\n"
                             "  crew c:\n"
                             "    let task = c.spawn(fetch(req))\n"
                             "  return result\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 6,
         "expected audit regression source to parse cleanly");

  auto *internal_use = expect_node<kira::ast::use_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::use_decl,
      "expected internal use declaration");
  expect(internal_use->visibility == kira::ast::visibility::internal,
         "expected internal visibility on use declaration");

  auto *super_module = expect_node<kira::ast::sub_module_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::sub_module_decl,
      "expected super-visible submodule declaration");
  expect(super_module->visibility == kira::ast::visibility::super,
         "expected super visibility on module declaration");

  auto *impl_decl = expect_node<kira::ast::impl_decl>(
      parsed.file->items[4].get(), kira::ast::node_kind::impl_decl,
      "expected higher-kinded impl declaration");
  expect(impl_decl->trait_type != nullptr, "expected impl trait type");
  expect(impl_decl->for_type == nullptr,
         "expected impl without explicit `for` type to omit for_type");

  auto *handle_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[5].get(), kira::ast::node_kind::func_decl,
      "expected async handle function");
  expect(handle_decl->body_stmts.size() == 3,
         "expected handle body statements to be preserved");

  auto *on_stmt = expect_node<kira::ast::let_stmt>(
      handle_decl->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
      "expected await on binding");
  auto *await_expr = expect_expr<kira::ast::await_expr>(
      on_stmt->initializer.get(), kira::ast::node_kind::await_expr,
      "expected await expression");
  auto *on_expr = expect_expr<kira::ast::on_expr>(
      await_expr->operand.get(), kira::ast::node_kind::on_expr,
      "expected single-argument on expression");
  expect(on_expr->context_type != nullptr, "expected on context type/value");
  expect(on_expr->sender == nullptr, "expected no second on argument");

  auto *crew_stmt = expect_node<kira::ast::crew_stmt>(
      handle_decl->body_stmts[1].get(), kira::ast::node_kind::crew_stmt,
      "expected plain crew statement");
  expect(crew_stmt->options.empty(), "expected plain crew without options");
}

auto test_parser_disambiguates_index_from_generic_instantiation() -> void {
  auto parsed = parse_source("module sample\n"
                             "def run(values, identity) -> int32:\n"
                             "  let a = values[0]\n"
                             "  let b = identity[int32](5)\n"
                             "  return a\n");

  expect(parsed.error_count == 0, parsed.diagnostics);

  auto *run_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
      "expected run function");
  expect(run_decl->body_stmts.size() == 3,
         "expected three statements in run's body");

  // A single unnamed bracket argument on a bare identifier is real indexing.
  auto *index_stmt = expect_node<kira::ast::let_stmt>(
      run_decl->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
      "expected indexing let binding");
  expect_expr<kira::ast::index_expr>(
      index_stmt->initializer.get(), kira::ast::node_kind::index_expr,
      "expected `values[0]` to parse as an index expression");

  // Bracket args immediately followed by a parenthesized call remain an
  // (outer) call expression — explicit generic instantiation.
  auto *call_stmt = expect_node<kira::ast::let_stmt>(
      run_decl->body_stmts[1].get(), kira::ast::node_kind::let_stmt,
      "expected generic-instantiation let binding");
  expect_expr<kira::ast::call_expr>(
      call_stmt->initializer.get(), kira::ast::node_kind::call_expr,
      "expected `identity[int32](5)` to parse as a call expression");
}

auto test_parser_accepts_extend_block() -> void {
  auto parsed = parse_source("module sample\n"
                             "extend str:\n"
                             "  def is_palindrome(self) -> bool:\n"
                             "    self == self.reversed()\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 1, "expected one top-level extend item");

  auto *extend_decl = expect_node<kira::ast::extend_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::extend_decl,
      "expected extend declaration");
  expect(extend_decl->for_type != nullptr,
         "expected extend block to carry a target type");
  expect(extend_decl->items.size() == 1,
         "expected one method in the extend block");
  expect_node<kira::ast::func_decl>(
      extend_decl->items[0].get(), kira::ast::node_kind::func_decl,
      "expected extend member to be a function declaration");
}

auto test_parser_accepts_intrinsic_def() -> void {
  auto parsed =
      parse_source("module sample\n"
                   "intrinsic def rt_write(fd: raw_fd, buf: slice[byte]) -> "
                   "result[usize, io_errno]\n"
                   "pub intrinsic def rt_stdout() -> raw_fd\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 2, "expected two intrinsic decls");

  auto *rt_write = expect_node<kira::ast::func_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
      "expected rt_write to parse as a function declaration");
  expect(rt_write->modifiers.is_intrinsic,
         "expected rt_write to carry the intrinsic modifier");
  expect(rt_write->body_expr == nullptr && rt_write->body_stmts.empty(),
         "expected intrinsic declaration to have no body");
  expect(rt_write->params.size() == 2, "expected two rt_write parameters");
  expect(rt_write->return_type != nullptr,
         "expected rt_write to carry a return type");

  auto *rt_stdout = expect_node<kira::ast::func_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::func_decl,
      "expected rt_stdout to parse as a function declaration");
  expect(rt_stdout->modifiers.is_intrinsic,
         "expected rt_stdout to carry the intrinsic modifier");
  expect(rt_stdout->visibility == kira::ast::visibility::pub,
         "expected pub intrinsic def to preserve visibility");
}

auto test_parser_rejects_intrinsic_def_with_body() -> void {
  auto parsed = parse_source("module sample\n"
                             "intrinsic def rt_noop() -> unit:\n"
                             "  unit\n");

  expect(parsed.error_count > 0,
         "expected a body on an intrinsic def to be reported as an error");
}

auto test_parser_accepts_generator_def_and_yield() -> void {
  auto parsed =
      parse_source("module sample\n"
                   "generator def counter() -> some iterator[int32]:\n"
                   "  yield 1\n"
                   "  yield 2\n"
                   "pure generator def counter2() -> some iterator[int32]:\n"
                   "  yield 1\n"
                   "generator pure def counter3() -> some iterator[int32]:\n"
                   "  yield 1\n"
                   "def check_await(source):\n"
                   "  await yield\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 4, "expected four function declarations");

  auto *counter = expect_node<kira::ast::func_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
      "expected counter to parse as a function declaration");
  expect(counter->modifiers.is_generator,
         "expected counter to carry the generator modifier");
  expect(counter->return_type != nullptr, "expected counter return type");
  expect(counter->return_type->kind == kira::ast::node_kind::existential_type,
         "expected `some iterator[int32]` to parse as an existential type");
  auto *ety =
      dynamic_cast<kira::ast::existential_type *>(counter->return_type.get());
  expect(ety->value.terms.size() == 1, "expected one bound term");
  expect(ety->value.terms[0].type != nullptr,
         "expected the bound term's type to be present");
  expect(ety->value.terms[0].type->kind == kira::ast::node_kind::named_type,
         "expected `iterator[int32]` to parse as a named type");
  auto *iterator_type =
      dynamic_cast<kira::ast::named_type *>(ety->value.terms[0].type.get());
  expect(iterator_type->path.size() == 1 &&
             iterator_type->path[0] == "iterator",
         "expected bound term to name `iterator`");
  expect(iterator_type->type_args.size() == 1,
         "expected `iterator[int32]` to carry one type argument");

  expect(counter->body_stmts.size() == 2,
         "expected two yield statements in counter's body");
  auto *first_yield_stmt = expect_node<kira::ast::expr_stmt>(
      counter->body_stmts[0].get(), kira::ast::node_kind::expr_stmt,
      "expected first yield to parse as an expression statement");
  auto *first_yield = expect_expr<kira::ast::yield_expr>(
      first_yield_stmt->expr.get(), kira::ast::node_kind::yield_expr,
      "expected `yield 1` to parse as a yield expression");
  expect(first_yield->value != nullptr, "expected yield to carry a value");

  auto *counter2 = expect_node<kira::ast::func_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::func_decl,
      "expected counter2 to parse as a function declaration");
  expect(counter2->modifiers.is_pure && counter2->modifiers.is_generator,
         "expected `pure generator def` to set both modifiers");

  auto *counter3 = expect_node<kira::ast::func_decl>(
      parsed.file->items[2].get(), kira::ast::node_kind::func_decl,
      "expected counter3 to parse as a function declaration");
  expect(counter3->modifiers.is_pure && counter3->modifiers.is_generator,
         "expected `generator pure def` to set both modifiers regardless of "
         "order");

  // Regression: `await yield` (the no-value coroutine handoff form) must
  // still parse unchanged now that bare `yield <expr>` is also a primary
  // expression — the two are disambiguated by leading token.
  auto *check_await = expect_node<kira::ast::func_decl>(
      parsed.file->items[3].get(), kira::ast::node_kind::func_decl,
      "expected check_await to parse as a function declaration");
  expect(check_await->body_stmts.size() == 1,
         "expected a single statement in check_await's body");
  auto *await_stmt = expect_node<kira::ast::expr_stmt>(
      check_await->body_stmts[0].get(), kira::ast::node_kind::expr_stmt,
      "expected `await yield` to parse as an expression statement");
  auto *await_yield = expect_expr<kira::ast::await_expr>(
      await_stmt->expr.get(), kira::ast::node_kind::await_expr,
      "expected `await yield` to parse as an await expression");
  expect(await_yield->is_yield,
         "expected `await yield` to set the no-value coroutine handoff flag");
  expect(await_yield->operand == nullptr,
         "expected `await yield` to carry no operand");
}

auto test_parser_accepts_mut_binding_pattern() -> void {
  auto parsed = parse_source("module sample\n"
                             "trait drop:\n"
                             "  def drop(mut self) -> unit\n"
                             "def run():\n"
                             "  let mut count = 0\n"
                             "  count = count + 1\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 2,
         "expected trait and function declarations");

  auto *trait_decl = expect_node<kira::ast::trait_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::trait_decl,
      "expected trait declaration");
  auto *drop_func = expect_node<kira::ast::func_decl>(
      trait_decl->items[0].get(), kira::ast::node_kind::func_decl,
      "expected drop function item");
  expect(drop_func->params.size() == 1, "expected a single self parameter");
  auto *self_pattern = expect_pattern<kira::ast::binding_pattern>(
      drop_func->params[0].pattern.get(), kira::ast::node_kind::binding_pattern,
      "expected self to parse as a binding pattern");
  expect(self_pattern->name == "self", "expected the parameter named self");
  expect(self_pattern->is_mut, "expected `mut self` to mark is_mut");

  auto *run_func = expect_node<kira::ast::func_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::func_decl,
      "expected run function declaration");
  auto *let_stmt = expect_node<kira::ast::let_stmt>(
      run_func->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
      "expected let mut statement");
  auto *count_pattern = expect_pattern<kira::ast::binding_pattern>(
      let_stmt->pattern.get(), kira::ast::node_kind::binding_pattern,
      "expected count to parse as a binding pattern");
  expect(count_pattern->is_mut, "expected `let mut count` to mark is_mut");
}

auto test_parser_splits_string_interpolation() -> void {
  // Plain strings (even with a doubled `{{`/`}}` escape) stay a single
  // `literal_expr`, matching the "zero-cost by default" design goal.
  {
    auto parsed = parse_source("module sample\n"
                               "def run():\n"
                               "  let a = \"hello\"\n"
                               "  let b = \"{{x}} stays literal\"\n");
    expect(parsed.error_count == 0, parsed.diagnostics);
    auto *run_func = expect_node<kira::ast::func_decl>(
        parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
        "expected run function declaration");
    auto *let_a = expect_node<kira::ast::let_stmt>(
        run_func->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
        "expected let a statement");
    expect_expr<kira::ast::literal_expr>(
        let_a->initializer.get(), kira::ast::node_kind::literal_expr,
        "expected plain string literal to stay a literal_expr");
    auto *let_b = expect_node<kira::ast::let_stmt>(
        run_func->body_stmts[1].get(), kira::ast::node_kind::let_stmt,
        "expected let b statement");
    auto *lit_b = expect_expr<kira::ast::literal_expr>(
        let_b->initializer.get(), kira::ast::node_kind::literal_expr,
        "expected doubled-brace string to stay a literal_expr");
    expect(lit_b->value.find("{{") != std::string::npos,
           "expected the doubled braces to remain in the raw token text "
           "unchanged (un-doubling happens later, when the literal is "
           "decoded)");
  }

  // A basic `{expr}` interpolation with no spec.
  {
    auto parsed = parse_source("module sample\n"
                               "def run():\n"
                               "  let msg = \"Hello, {name}!\"\n");
    expect(parsed.error_count == 0, parsed.diagnostics);
    auto *run_func = expect_node<kira::ast::func_decl>(
        parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
        "expected run function declaration");
    auto *let_msg = expect_node<kira::ast::let_stmt>(
        run_func->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
        "expected let msg statement");
    auto *interp = expect_expr<kira::ast::interpolated_string_expr>(
        let_msg->initializer.get(),
        kira::ast::node_kind::interpolated_string_expr,
        "expected an interpolated_string_expr");
    expect(interp->segments.size() == 3,
           "expected literal/expr/literal segments");
    expect(interp->segments[0].is_literal &&
               interp->segments[0].literal_text == "Hello, ",
           "expected the leading literal segment text");
    expect(!interp->segments[1].is_literal,
           "expected the middle segment to be an embedded expression");
    auto *name_ident = expect_expr<kira::ast::ident_expr>(
        interp->segments[1].value.get(), kira::ast::node_kind::ident_expr,
        "expected the embedded expression to be a bare identifier");
    expect(name_ident->name == "name", "expected identifier name `name`");
    expect(!interp->segments[1].self_doc && !interp->segments[1].has_spec,
           "expected no self-doc/spec on a plain `{name}`");
    expect(interp->segments[2].is_literal &&
               interp->segments[2].literal_text == "!",
           "expected the trailing literal segment text");
  }

  // A format spec, a dynamic width, and self-documenting `=`.
  {
    auto parsed = parse_source("module sample\n"
                               "def run():\n"
                               "  let a = \"{total :.2f}\"\n"
                               "  let b = \"{val :{width}.{prec}f}\"\n"
                               "  let c = \"{total=}\"\n");
    expect(parsed.error_count == 0, parsed.diagnostics);
    auto *run_func = expect_node<kira::ast::func_decl>(
        parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
        "expected run function declaration");

    auto *let_a = expect_node<kira::ast::let_stmt>(
        run_func->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
        "expected let a statement");
    auto *interp_a = expect_expr<kira::ast::interpolated_string_expr>(
        let_a->initializer.get(),
        kira::ast::node_kind::interpolated_string_expr,
        "expected an interpolated_string_expr for `{total :.2f}`");
    expect(interp_a->segments.size() == 1, "expected a single segment");
    expect(interp_a->segments[0].has_spec, "expected a parsed format spec");
    expect(interp_a->segments[0].spec.type_char == 'f',
           "expected type char `f`");
    expect(
        std::holds_alternative<size_t>(interp_a->segments[0].spec.precision) &&
            std::get<size_t>(interp_a->segments[0].spec.precision) == 2,
        "expected literal precision 2");

    auto *let_b = expect_node<kira::ast::let_stmt>(
        run_func->body_stmts[1].get(), kira::ast::node_kind::let_stmt,
        "expected let b statement");
    auto *interp_b = expect_expr<kira::ast::interpolated_string_expr>(
        let_b->initializer.get(),
        kira::ast::node_kind::interpolated_string_expr,
        "expected an interpolated_string_expr for the dynamic-width case");
    expect(interp_b->segments.size() == 1, "expected a single segment");
    expect(std::holds_alternative<kira::ast::ptr<kira::ast::expr>>(
               interp_b->segments[0].spec.width),
           "expected a dynamic (sub-expression) width");
    expect(std::holds_alternative<kira::ast::ptr<kira::ast::expr>>(
               interp_b->segments[0].spec.precision),
           "expected a dynamic (sub-expression) precision");

    auto *let_c = expect_node<kira::ast::let_stmt>(
        run_func->body_stmts[2].get(), kira::ast::node_kind::let_stmt,
        "expected let c statement");
    auto *interp_c = expect_expr<kira::ast::interpolated_string_expr>(
        let_c->initializer.get(),
        kira::ast::node_kind::interpolated_string_expr,
        "expected an interpolated_string_expr for `{total=}`");
    expect(interp_c->segments.size() == 1, "expected a single segment");
    expect(interp_c->segments[0].self_doc, "expected self_doc to be set");
    expect(interp_c->segments[0].source_text == "total",
           "expected the self-documenting source text to be `total`");
    expect(!interp_c->segments[0].has_spec,
           "expected no format spec on `{total=}`");
  }

  // A named-argument `:` inside a nested call, and a nested struct literal's
  // own `{...}`, must not be mistaken for the interpolation's own `=`/`:`.
  {
    auto parsed = parse_source("module sample\n"
                               "def run():\n"
                               "  let a = \"{f(x: 1)}\"\n"
                               "  let b = \"{point { x: 1, y: 2 } }\"\n");
    expect(parsed.error_count == 0, parsed.diagnostics);
    auto *run_func = expect_node<kira::ast::func_decl>(
        parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
        "expected run function declaration");
    auto *let_a = expect_node<kira::ast::let_stmt>(
        run_func->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
        "expected let a statement");
    auto *interp_a = expect_expr<kira::ast::interpolated_string_expr>(
        let_a->initializer.get(),
        kira::ast::node_kind::interpolated_string_expr,
        "expected an interpolated_string_expr for `{f(x: 1)}`");
    expect(interp_a->segments.size() == 1, "expected a single segment");
    expect(!interp_a->segments[0].has_spec,
           "expected the named-arg `:` to not be mistaken for a format spec");
    expect_expr<kira::ast::call_expr>(
        interp_a->segments[0].value.get(), kira::ast::node_kind::call_expr,
        "expected the embedded expression to be a call");

    auto *let_b = expect_node<kira::ast::let_stmt>(
        run_func->body_stmts[1].get(), kira::ast::node_kind::let_stmt,
        "expected let b statement");
    auto *interp_b = expect_expr<kira::ast::interpolated_string_expr>(
        let_b->initializer.get(),
        kira::ast::node_kind::interpolated_string_expr,
        "expected an interpolated_string_expr for the struct-literal case");
    expect(interp_b->segments.size() == 1, "expected a single segment");
    expect(!interp_b->segments[0].has_spec,
           "expected the struct literal's own fields to not be mistaken for "
           "a format spec");
    expect_expr<kira::ast::struct_expr>(
        interp_b->segments[0].value.get(), kira::ast::node_kind::struct_expr,
        "expected the embedded expression to be a struct literal");
  }
}

auto test_parser_classifies_quote_fragment_kind() -> void {
  // `value` — a bare identifier, no statement structure: expr fragment.
  {
    auto parsed = parse_source("module sample\n"
                               "\n"
                               "def run(value):\n"
                               "  let quoted = `value`\n");
    expect(parsed.error_count == 0, parsed.diagnostics);
    auto *run_func = expect_node<kira::ast::func_decl>(
        parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
        "expected run function");
    auto *let_quoted = expect_node<kira::ast::let_stmt>(
        run_func->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
        "expected let-quoted statement");
    auto *quote = expect_expr<kira::ast::quote_expr>(
        let_quoted->initializer.get(), kira::ast::node_kind::quote_expr,
        "expected a quote_expr initializer");
    expect(quote->fragment_kind == kira::ast::quote_fragment_kind::expr,
           "expected `value` to classify as an expr fragment");
    expect_node<kira::ast::ident_expr>(quote->parsed_body.get(),
                                       kira::ast::node_kind::ident_expr,
                                       "expected the fragment body to be the "
                                       "bare identifier `value`");
  }

  // `(value + 1)` — the outer `(...)` right after the opening backtick is
  // consumed by `parse_quote_expr` itself as a grouping delimiter for the
  // quote syntax (so nested backticks stay unambiguous), not literal
  // parenthesized-expression content — so the captured tokens are just
  // `value + 1`, an ordinary binary expression, still an expr fragment.
  {
    auto parsed = parse_source("module sample\n"
                               "\n"
                               "def run(value):\n"
                               "  let grouped = `(value + 1)`\n");
    expect(parsed.error_count == 0, parsed.diagnostics);
    auto *run_func = expect_node<kira::ast::func_decl>(
        parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
        "expected run function");
    auto *let_grouped = expect_node<kira::ast::let_stmt>(
        run_func->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
        "expected let-grouped statement");
    auto *quote = expect_expr<kira::ast::quote_expr>(
        let_grouped->initializer.get(), kira::ast::node_kind::quote_expr,
        "expected a quote_expr initializer");
    expect(quote->fragment_kind == kira::ast::quote_fragment_kind::expr,
           "expected `(value + 1)` to classify as an expr fragment");
    expect_node<kira::ast::binary_expr>(
        quote->parsed_body.get(), kira::ast::node_kind::binary_expr,
        "expected the fragment body to be the binary expression `value + 1`, "
        "with the wrapping parens consumed as quote-grouping syntax rather "
        "than becoming a group_expr node");
  }

  // `x = 5` — real statement structure: stmt fragment.
  {
    auto parsed = parse_source("module sample\n"
                               "\n"
                               "def run(x):\n"
                               "  let assigned = `x = 5`\n");
    expect(parsed.error_count == 0, parsed.diagnostics);
    auto *run_func = expect_node<kira::ast::func_decl>(
        parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
        "expected run function");
    auto *let_assigned = expect_node<kira::ast::let_stmt>(
        run_func->body_stmts[0].get(), kira::ast::node_kind::let_stmt,
        "expected let-assigned statement");
    auto *quote = expect_expr<kira::ast::quote_expr>(
        let_assigned->initializer.get(), kira::ast::node_kind::quote_expr,
        "expected a quote_expr initializer");
    expect(quote->fragment_kind == kira::ast::quote_fragment_kind::stmt,
           "expected `x = 5` to classify as a stmt fragment");
    expect_node<kira::ast::assign_stmt>(
        quote->parsed_body.get(), kira::ast::node_kind::assign_stmt,
        "expected the fragment body to be an assignment statement");
  }

  // `impl show for point:` — an item: def_expr fragment. `static def`
  // doesn't exist in the grammar yet (see comptime-evaluation plan's R1),
  // so this uses a plain `static let` binding instead of the reference
  // doc's `static def derive_show[T]()` shape — classification doesn't
  // care which kind of `static` form is holding the quote.
  {
    auto parsed = parse_source("module sample\n"
                               "\n"
                               "static let derive_show: def_expr = "
                               "`impl show for point:`\n");
    expect(parsed.error_count == 0, parsed.diagnostics);
    auto *derive_decl = expect_node<kira::ast::static_decl>(
        parsed.file->items[0].get(), kira::ast::node_kind::static_decl,
        "expected the derive_show static let");
    auto *quote = expect_expr<kira::ast::quote_expr>(
        derive_decl->initializer.get(), kira::ast::node_kind::quote_expr,
        "expected the static binding's initializer to be a quote_expr");
    expect(quote->fragment_kind == kira::ast::quote_fragment_kind::def_expr,
           "expected `impl show for point:` to classify as a def_expr "
           "fragment");
    expect_node<kira::ast::impl_decl>(
        quote->parsed_body.get(), kira::ast::node_kind::impl_decl,
        "expected the fragment body to be an impl declaration");
  }
}

/// A `` `(...)` `` quote wraps its content in a real, literal `(` — which
/// the lexer treats as an ordinary bracket, suppressing NEWLINE/INDENT/
/// DEDENT synthesis for its whole span (`bracket_depth_ > 0`, `lexer.h`).
/// Left unhandled, that means indentation-block-sensitive content (an
/// `impl` with a method body) can never lex correctly inside `` `(...)` ``:
/// the lexer would never see the INDENT that starts the block, so the
/// parser would see an empty body with no diagnostic at all. `lexer.h`'s
/// `(`/`)` cases special-case the wrapper paren pair (not bumping
/// `bracket_depth_`, and force-emitting the DEDENTs the quoted block owes
/// right at the closing `)`, regardless of nested real parens like a
/// `(self)` parameter list in between — mirrored by `parse_quote_expr`'s
/// own real-paren-nesting tracking, so lexer and parser agree on exactly
/// which `)` closes the wrapper. This test is the regression check for
/// that whole mechanism, using the shape `semantic::checker::
/// resolve_item_splices` (item-level splice, check.cpp) actually needs:
/// a multi-line `impl` block, with a method that has a parenthesized
/// parameter list, quoted inside `` `(...)` ``.
auto test_parser_parses_multiline_indented_paren_quote() -> void {
  auto parsed = parse_source("module sample\n"
                             "\n"
                             "static def make_show_impl() -> def_expr:\n"
                             "    return `(impl show for point:\n"
                             "        def show(self) -> int32:\n"
                             "            return 42)`\n");
  expect(parsed.error_count == 0, parsed.diagnostics);
  auto *fn_decl = expect_node<kira::ast::func_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
      "expected the make_show_impl function");
  auto *return_stmt = expect_node<kira::ast::return_stmt>(
      fn_decl->body_stmts[0].get(), kira::ast::node_kind::return_stmt,
      "expected a return statement");
  auto *quote = expect_expr<kira::ast::quote_expr>(
      return_stmt->value.get(), kira::ast::node_kind::quote_expr,
      "expected the returned value to be a quote_expr");
  expect(quote->fragment_kind == kira::ast::quote_fragment_kind::def_expr,
         "expected the quoted `impl` to classify as a def_expr fragment");
  auto *impl = expect_node<kira::ast::impl_decl>(
      quote->parsed_body.get(), kira::ast::node_kind::impl_decl,
      "expected the fragment body to be an impl declaration");
  expect(impl->items.size() == 1,
         "expected the quoted impl to contain exactly one method — if the "
         "lexer failed to preserve indentation inside `` `(...)` ``, this "
         "would be empty instead");
  auto *method = expect_node<kira::ast::func_decl>(
      impl->items[0].get(), kira::ast::node_kind::func_decl,
      "expected the quoted impl's item to be the `show` method");
  expect(method->name == "show", "expected the method to be named `show`");
  expect(method->body_stmts.size() == 1,
         "expected `show`'s body to contain exactly one statement — if the "
         "closing `)` didn't emit the DEDENTs the block owed, this body "
         "would be empty instead");
}

auto test_script_module_synthesizes_main() -> void {
  auto parsed = parse_source("module main\n"
                             "\n"
                             "def greet(name: str) -> unit:\n"
                             "    println(\"Hi, {name}\")\n"
                             "\n"
                             "let who = \"kira\"\n"
                             "greet(who)\n");
  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 2,
         "expected the greet declaration plus one synthesized `main`");
  auto *greet = expect_node<kira::ast::func_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
      "expected the greet declaration to stay a top-level item");
  expect(greet->name == "greet", "expected greet to keep its name");
  auto *synthesized = expect_node<kira::ast::func_decl>(
      parsed.file->items[1].get(), kira::ast::node_kind::func_decl,
      "expected a synthesized function appended after the declarations");
  expect(synthesized->name == "main",
         "expected the synthesized function to be named `main`");
  expect(synthesized->body_stmts.size() == 2,
         "expected both top-level statements to become `main`'s body");
  expect_node<kira::ast::let_stmt>(synthesized->body_stmts[0].get(),
                                   kira::ast::node_kind::let_stmt,
                                   "expected the `let` to run first in `main`");
  auto *ret = expect_node<kira::ast::named_type>(
      synthesized->return_type.get(), kira::ast::node_kind::named_type,
      "expected the synthesized `main` to be annotated `-> unit`");
  expect(ret->path.size() == 1 && ret->path[0] == "unit",
         "expected the synthesized return type to name `unit`");
}

auto test_script_module_rejects_statements_with_explicit_main() -> void {
  auto parsed = parse_source("module main\n"
                             "\n"
                             "println(\"top level\")\n"
                             "\n"
                             "def main() -> unit:\n"
                             "    println(\"explicit\")\n");
  expect(parsed.error_count == 1,
         "expected exactly one error for mixing top-level statements with an "
         "explicit `def main`");
  expect(parsed.diagnostics.find("mixes top-level statements") !=
             std::string::npos,
         parsed.diagnostics);
}

auto test_script_module_without_statements_keeps_explicit_main() -> void {
  auto parsed = parse_source("module main\n"
                             "\n"
                             "def main() -> unit:\n"
                             "    println(\"hello\")\n");
  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 1,
         "expected no synthesized second `main` for a declaration-only file");
}

auto test_non_main_module_rejects_top_level_statements() -> void {
  auto parsed = parse_source("module other\n"
                             "\n"
                             "println(\"nope\")\n");
  expect(parsed.error_count > 0,
         "expected top-level statements outside `module main` to error");
  expect(parsed.diagnostics.find("module main") != std::string::npos,
         "expected the diagnostic to teach that `module main` scripts allow "
         "top-level statements");
}

auto test_parser_accepts_signature_decl() -> void {
  auto parsed = parse_source("module sample\n"
                             "\n"
                             "signature backend:\n"
                             "    type conn\n"
                             "    def connect(url: str) -> conn\n"
                             "    def query(c: &conn, sql: str) -> conn\n"
                             "    static default_port: int32\n");
  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 1, "expected one signature item");

  auto *sig = expect_node<kira::ast::signature_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::signature_decl,
      "expected a signature_decl");
  expect(sig->name == "backend", "expected signature name `backend`");
  expect(sig->items.size() == 4, "expected four signature members");
  expect(sig->items[0]->kind == kira::ast::node_kind::associated_type_decl_node,
         "expected abstract `type conn` member");
  expect(sig->items[1]->kind == kira::ast::node_kind::func_decl,
         "expected `def connect` member");
  expect(sig->items[3]->kind == kira::ast::node_kind::static_decl,
         "expected `static default_port` member");
}

auto test_parser_rejects_signature_abstract_type_bound() -> void {
  auto parsed = parse_source("module sample\n"
                             "\n"
                             "signature backend:\n"
                             "    type conn: comparable\n");
  expect(parsed.error_count > 0,
         "expected a bound on a signature abstract type to error");
  expect(parsed.diagnostics.find("not supported yet") != std::string::npos,
         parsed.diagnostics);
}

auto test_parser_accepts_parameterized_module() -> void {
  auto parsed = parse_source("module sample\n"
                             "\n"
                             "module audited[DB: backend]:\n"
                             "    pub def query(c: &DB.conn, sql: str) -> "
                             "DB.conn:\n"
                             "        DB.query(c, sql)\n");
  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 1, "expected one module item");

  auto *mod = expect_node<kira::ast::sub_module_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::sub_module_decl,
      "expected a sub_module_decl");
  expect(mod->name == "audited", "expected module name `audited`");
  expect(mod->is_functor(), "expected a parameterized module (functor)");
  expect(mod->type_params.size() == 1, "expected one module parameter");
  expect(mod->type_params[0].name == "DB", "expected parameter named `DB`");
  expect(mod->type_params[0].bound_or_type != nullptr,
         "expected the `DB` parameter to carry a signature bound");
  expect(mod->items.size() == 1, "expected one item in the functor body");
}

auto test_parser_plain_submodule_is_not_functor() -> void {
  auto parsed = parse_source("module sample\n"
                             "\n"
                             "module inner:\n"
                             "    def f() -> unit:\n"
                             "        unit\n");
  expect(parsed.error_count == 0, parsed.diagnostics);
  auto *mod = expect_node<kira::ast::sub_module_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::sub_module_decl,
      "expected a sub_module_decl");
  expect(!mod->is_functor(), "a plain submodule must not be a functor");
  expect(mod->type_params.empty(), "expected no module parameters");
}

auto test_parser_accepts_functor_instantiation_use() -> void {
  auto parsed = parse_source("module sample\n"
                             "\n"
                             "use audited[postgres] as db\n");
  expect(parsed.error_count == 0, parsed.diagnostics);
  expect(parsed.file->items.size() == 1, "expected one use item");

  auto *use = expect_node<kira::ast::use_decl>(parsed.file->items[0].get(),
                                               kira::ast::node_kind::use_decl,
                                               "expected a use_decl");
  expect(use->path.size() == 1 && use->path[0] == "audited",
         "expected functor path `audited` to be preserved intact");
  expect(use->instantiation_args.size() == 1,
         "expected one instantiation argument");
  expect(use->selector.has_value(), "expected an `as` alias selector");
  expect(use->selector->kind == kira::ast::use_selector_kind::single,
         "expected a single-item selector for the alias");
  expect(use->selector->items[0].alias == "db",
         "expected the instantiation to be aliased `db`");
}

auto test_parser_accepts_nested_functor_instantiation() -> void {
  auto parsed = parse_source("module sample\n"
                             "\n"
                             "use audited[cached[postgres]]\n");
  expect(parsed.error_count == 0, parsed.diagnostics);
  auto *use = expect_node<kira::ast::use_decl>(parsed.file->items[0].get(),
                                               kira::ast::node_kind::use_decl,
                                               "expected a use_decl");
  expect(use->instantiation_args.size() == 1,
         "expected one (nested) instantiation argument");
  expect(!use->selector.has_value(),
         "expected no alias for an un-aliased instantiation");
}

auto test_parser_accepts_lambda_result_and_paren_params() -> void {
  // Every one of these but the `pure`-prefixed form used to be a syntax
  // error: an unprefixed `(` reached the tuple parser, and an `ident ->`
  // head was never recognized as a lambda at all. Only `pure`/`move` got
  // routed to the lambda parser, which had handled both shapes all along.
  auto parsed = parse_source("module sample\n"
                             "def run():\n"
                             "  let a = x => x + 1\n"
                             "  let b = pure (x: int32) -> int32 => x * 2\n"
                             "  let c = (x: int32) -> int32 => x * 3\n"
                             "  let d = x -> int32 => x + 5\n"
                             "  let e = (x, y) => x + y\n"
                             "  let g = (3 + 4) * 2\n");

  expect(parsed.error_count == 0, parsed.diagnostics);
  auto *run_func = expect_node<kira::ast::func_decl>(
      parsed.file->items[0].get(), kira::ast::node_kind::func_decl,
      "expected run function declaration");

  const auto lambda_at = [&](size_t index) -> kira::ast::lambda_expr * {
    auto *let_stmt = expect_node<kira::ast::let_stmt>(
        run_func->body_stmts[index].get(), kira::ast::node_kind::let_stmt,
        "expected a let statement");
    return expect_node<kira::ast::lambda_expr>(
        let_stmt->initializer.get(), kira::ast::node_kind::lambda_expr,
        "expected the initializer to parse as a lambda");
  };

  // A declared result type has to survive, not merely parse: it is what the
  // checker propagates into the body as the expected type.
  expect(lambda_at(2)->return_type != nullptr,
         "expected `(x: int32) -> int32 =>` to keep its declared result");
  expect(lambda_at(2)->params.size() == 1, "expected one parenthesized param");
  expect(lambda_at(3)->return_type != nullptr,
         "expected `x -> int32 =>` to keep its declared result");
  expect(lambda_at(4)->params.size() == 2,
         "expected `(x, y) =>` to parse as two params, not a tuple");

  // The disambiguation must not swallow ordinary grouping.
  auto *group_let = expect_node<kira::ast::let_stmt>(
      run_func->body_stmts[5].get(), kira::ast::node_kind::let_stmt,
      "expected a let statement");
  expect(group_let->initializer->kind != kira::ast::node_kind::lambda_expr,
         "expected `(3 + 4) * 2` to stay a grouped expression");
}

struct named_test {
  const char *name;
  void (*fn)();
};

} // namespace

auto main(int argc, char *argv[]) -> int {
  const std::array<named_test, 35> tests = {{
      {.name = "lexer_indent_dedent", .fn = test_lexer_emits_indent_and_dedent},
      {.name = "type_body_nodes", .fn = test_parser_builds_type_body_nodes},
      {.name = "doc_comments", .fn = test_parser_captures_doc_comments},
      {.name = "associated_types_where_aliases",
       .fn = test_parser_preserves_associated_types_where_and_aliases},
      {.name = "function_signature_and_control_flow",
       .fn = test_parser_preserves_function_signature_and_control_flow},
      {.name = "trait_impl_and_block_expressions",
       .fn = test_parser_preserves_trait_impl_and_block_expressions},
      {.name = "missing_module_recovery",
       .fn = test_parser_reports_missing_module_and_recovers},
      {.name = "missing_where_colon_recovery",
       .fn = test_parser_recovers_missing_colon_in_where_clause},
      {.name = "spec_valid_regressions",
       .fn = test_parser_accepts_spec_valid_regressions},
      {.name = "remaining_phase1_constructs",
       .fn = test_parser_accepts_remaining_phase1_constructs},
      {.name = "if_let_expression",
       .fn = test_parser_accepts_if_let_expression},
      {.name = "multi_arity_higher_kinded_params",
       .fn = test_parser_accepts_multi_arity_higher_kinded_params},
      {.name = "phase1_audit_regressions",
       .fn = test_parser_accepts_phase1_audit_regressions},
      {.name = "index_vs_generic_instantiation",
       .fn = test_parser_disambiguates_index_from_generic_instantiation},
      {.name = "extend_block", .fn = test_parser_accepts_extend_block},
      {.name = "intrinsic_def", .fn = test_parser_accepts_intrinsic_def},
      {.name = "intrinsic_def_rejects_body",
       .fn = test_parser_rejects_intrinsic_def_with_body},
      {.name = "generator_def_and_yield",
       .fn = test_parser_accepts_generator_def_and_yield},
      {.name = "mut_binding_pattern",
       .fn = test_parser_accepts_mut_binding_pattern},
      {.name = "string_interpolation",
       .fn = test_parser_splits_string_interpolation},
      {.name = "quote_fragment_kind",
       .fn = test_parser_classifies_quote_fragment_kind},
      {.name = "multiline_indented_paren_quote",
       .fn = test_parser_parses_multiline_indented_paren_quote},
      {.name = "script_module_synthesizes_main",
       .fn = test_script_module_synthesizes_main},
      {.name = "script_module_rejects_statements_with_explicit_main",
       .fn = test_script_module_rejects_statements_with_explicit_main},
      {.name = "script_module_without_statements_keeps_explicit_main",
       .fn = test_script_module_without_statements_keeps_explicit_main},
      {.name = "non_main_module_rejects_top_level_statements",
       .fn = test_non_main_module_rejects_top_level_statements},
      {.name = "signature_decl", .fn = test_parser_accepts_signature_decl},
      {.name = "signature_abstract_type_bound",
       .fn = test_parser_rejects_signature_abstract_type_bound},
      {.name = "parameterized_module",
       .fn = test_parser_accepts_parameterized_module},
      {.name = "plain_submodule_not_functor",
       .fn = test_parser_plain_submodule_is_not_functor},
      {.name = "functor_instantiation_use",
       .fn = test_parser_accepts_functor_instantiation_use},
      {.name = "nested_functor_instantiation",
       .fn = test_parser_accepts_nested_functor_instantiation},
      {.name = "lambda_result_and_paren_params",
       .fn = test_parser_accepts_lambda_result_and_paren_params},
      {.name = "static_def_in_member_blocks",
       .fn = test_parser_accepts_static_def_in_member_blocks},
      {.name = "static_bindings_stay_bindings",
       .fn = test_parser_still_reads_static_bindings_as_bindings},
  }};

  const std::span<char *> args(argv, static_cast<size_t>(argc));

  if (argc > 1) {
    for (const auto &test : tests) {
      if (std::strcmp(args[1], test.name) == 0) {
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
