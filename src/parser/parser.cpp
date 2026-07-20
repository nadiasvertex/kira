#include "parser.h"

#include <cassert>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "src/parser/interp_string.h"
#include "src/parser/text_escape.h"

namespace kira {

// ==========================================================================
//  Helper: make an error expression for recovery.
// ==========================================================================
static auto make_error_expr(source_span span, std::string desc = "")
    -> ast::ptr<ast::expr> {
  auto node = ast::make<ast::error_expr>(span, std::move(desc));
  return node;
}

static auto make_error_pattern(source_span span) -> ast::ptr<ast::pattern> {
  return ast::make<ast::error_pattern>(span);
}

static auto make_error_node(source_span span, std::string desc = "")
    -> ast::ptr<ast::node> {
  return ast::make<ast::error_node>(span, std::move(desc));
}

[[nodiscard]] static auto can_start_sum_variant(token_kind kind) -> bool {
  return kind == token_kind::ident || kind == token_kind::kw_some ||
         kind == token_kind::kw_ok || kind == token_kind::kw_err;
}

[[nodiscard]] static auto token_text_string(const token &tok) -> std::string {
  return std::string(tok.text);
}

// ==========================================================================
//  Token stream navigation & error recovery
// ==========================================================================

auto parser::expect_newline() -> bool {
  if (match(token_kind::newline)) {
    return true;
  }

  if (allow_compact_stmt_terminator_ &&
      (peek().can_start_stmt() ||
       at_any(token_kind::rparen, token_kind::rbracket, token_kind::rbrace,
              token_kind::comma, token_kind::eof, token_kind::dedent))) {
    return true;
  }

  // At EOF or DEDENT, a newline is implicitly present.
  if (at_any(token_kind::eof, token_kind::dedent)) {
    return true;
  }

  // Block expressions consume their closing DEDENT internally, so the
  // enclosing statement sees the next statement-starting token directly.
  if (previous().is(token_kind::dedent)) {
    return true;
  }

  // Missing newline — emit a helpful diagnostic.
  auto span = previous_span();
  emit(diagnostic(diagnostic_level::error, "expected end of line after this",
                  file_id_)
           .with_label(span, "this should be followed by a new line")
           .with_help("Each statement in Kira goes on its own line. "
                      "If you need to continue a long expression across "
                      "lines, wrap it in parentheses."));

  // Recovery: skip to the next newline or statement-starting token.
  while (!at_eof() && !at(token_kind::newline) && !at(token_kind::dedent) &&
         !peek().can_start_stmt()) {
    advance();
  }
  match(token_kind::newline);
  return false;
}

auto parser::expect(token_kind expected) -> token {
  if (at(expected)) {
    return advance();
  }

  // Build a friendly error message.
  auto expected_desc = token_kind_description(expected);
  auto found = peek();

  std::string message;
  if (found.is_eof()) {
    message = std::format("expected {} but reached end of file", expected_desc);
  } else {
    message = std::format("expected {} but found {}", expected_desc,
                          token_kind_name(found.kind));
  }

  auto diag = diagnostic(diagnostic_level::error, message, file_id_)
                  .with_label(found.span,
                              std::format("expected {} here", expected_desc));

  // Context-specific help for common mistakes.
  if (expected == token_kind::rparen && found.is(token_kind::newline)) {
    diag.with_help("It looks like a closing `)` is missing. Check that "
                   "every `(` has a matching `)` on the same line or "
                   "within the same indented block.");
  } else if (expected == token_kind::rbracket &&
             found.is(token_kind::newline)) {
    diag.with_help("It looks like a closing `]` is missing.");
  } else if (expected == token_kind::rbrace && found.is(token_kind::newline)) {
    diag.with_help("It looks like a closing `}` is missing.");
  } else if (expected == token_kind::colon) {
    diag.with_help("In Kira, a `:` introduces the body of a block. "
                   "For example: `if condition:` or `def name():` — "
                   "the `:` goes right before the body.");
    diag.with_fix("add `:`", found.span, ": ");
  } else if (expected == token_kind::eq) {
    diag.with_help("An `=` is needed here to assign a value.");
  }

  emit(diag);

  // Recovery strategy: if the current token is something that plausibly
  // starts the *next* construct, don't consume it — just pretend the
  // expected token was there. This is called "insertion" recovery.
  // Otherwise, consume the bad token to make progress.
  if (!at_recovery_point() && !at_eof()) {
    // The token doesn't look like it starts something new, so it's
    // probably a typo or extra token. Consume it.
    // But only if it isn't a structural token we need.
    if (!at_any(token_kind::indent, token_kind::dedent, token_kind::newline)) {
      advance();
    }
  }

  // Return a synthetic placeholder token.
  return token{
      .kind = expected,
      .span = found.span,
      .text = {},
      .error_message = {},
  };
}

auto parser::expect_with_context(token_kind expected, std::string_view context)
    -> token {
  if (at(expected)) {
    return advance();
  }

  auto expected_desc = token_kind_description(expected);
  auto found = peek();

  std::string message =
      std::format("expected {} {} but found {}", expected_desc, context,
                  token_kind_name(found.kind));

  auto diag = diagnostic(diagnostic_level::error, message, file_id_)
                  .with_label(found.span,
                              std::format("expected {} here", expected_desc));

  emit(diag);

  if (!at_recovery_point() && !at_eof() &&
      !at_any(token_kind::indent, token_kind::dedent, token_kind::newline)) {
    advance();
  }

  return token{
      .kind = expected,
      .span = found.span,
      .text = {},
      .error_message = {},
  };
}

void parser::emit_unexpected(std::string_view expected_description) {
  auto found = peek();

  std::string message;
  if (found.is_eof()) {
    message = std::format("expected {} but reached end of file",
                          expected_description);
  } else if (found.is_error()) {
    // Lexer already reported this — don't double-report.
    return;
  } else {
    message = std::format("expected {} but found {}", expected_description,
                          token_kind_name(found.kind));
  }

  emit(diagnostic(diagnostic_level::error, message, file_id_)
           .with_label(found.span,
                       std::format("expected {}", expected_description)));
}

auto parser::at_recovery_point() const noexcept -> bool {
  return peek().can_start_stmt() || at_any(token_kind::eof, token_kind::dedent);
}

void parser::synchronize() {
  // Skip tokens until we find something that looks like the start of
  // a new statement or declaration. We track indent/dedent balance so
  // we don't skip past the end of the current block.
  int depth = 0;

  while (!at_eof()) {
    if (at(token_kind::newline)) {
      advance();
      // After a newline, check if the next token starts a new statement.
      skip_newlines();
      if (peek().can_start_stmt() || at_eof() || at(token_kind::dedent)) {
        return;
      }
      continue;
    }

    if (at(token_kind::indent)) {
      ++depth;
      advance();
      continue;
    }

    if (at(token_kind::dedent)) {
      if (depth > 0) {
        --depth;
        advance();
        continue;
      }
      // Don't consume the DEDENT — it belongs to our parent.
      return;
    }

    advance();
  }
}

void parser::synchronize_to_newline() {
  while (!at_eof() && !at_any(token_kind::newline, token_kind::dedent)) {
    advance();
  }
  match(token_kind::newline);
}

void parser::skip_block() {
  // Skip a NEWLINE INDENT ... DEDENT sequence.
  skip_newlines();
  if (!match(token_kind::indent)) {
    return;
  }

  int depth = 1;
  while (!at_eof() && depth > 0) {
    if (at(token_kind::indent)) {
      ++depth;
    } else if (at(token_kind::dedent)) {
      --depth;
    }
    advance();
  }
}

// ==========================================================================
//  Block parsing helpers
// ==========================================================================

auto parser::expect_block_start(std::string_view construct_name) -> bool {
  if (!at(token_kind::colon)) {
    auto span = previous_span();
    emit(diagnostic(diagnostic_level::error,
                    std::format("expected `:` to start the body of this {}",
                                construct_name),
                    file_id_)
             .with_label(span, "expected `:` after this")
             .with_help(std::format(
                 "In Kira, the body of a {} starts with `:` followed by "
                 "either an expression on the same line, or a new line with "
                 "an indented block.",
                 construct_name))
             .with_fix("add `:`",
                       source_span{.start = span.end, .end = span.end}, ":"));
    // Recovery: if NEWLINE+INDENT follows, proceed as if `:` was there.
    if (at(token_kind::newline) && peek_at(1).is(token_kind::indent)) {
      // Proceed — the block is clearly intended.
    } else {
      return false;
    }
  } else {
    advance(); // consume `:`
  }

  // Now expect NEWLINE INDENT for block form.
  // (Caller handles inline form separately.)
  skip_newlines();
  if (!match(token_kind::indent)) {
    // This might be an inline body — let the caller handle it.
    return false;
  }

  return true;
}

auto parser::expect_block_end(std::string_view construct_name) -> bool {
  if (match(token_kind::dedent)) {
    return true;
  }

  // Missing DEDENT — emit diagnostic.
  emit(diagnostic(
           diagnostic_level::error,
           std::format("expected end of {} block (dedent)", construct_name),
           file_id_)
           .with_label(peek().span, "expected the block to end here")
           .with_note("This usually means the indentation is inconsistent. "
                      "Check that all lines in this block are indented "
                      "by the same amount."));
  return false;
}

template <typename T>
auto parser::parse_block(std::string_view construct_name,
                         const std::function<ast::ptr<T>()> &parse_item)
    -> std::vector<ast::ptr<T>> {
  std::vector<ast::ptr<T>> items;

  while (!at_any(token_kind::dedent, token_kind::eof)) {
    skip_newlines();
    if (at_any(token_kind::dedent, token_kind::eof)) {
      break;
    }

    auto item = parse_item();
    if (item) {
      items.push_back(std::move(item));
    } else {
      // Parse failed — synchronize to the next statement.
      synchronize();
    }
  }

  expect_block_end(construct_name);
  return items;
}

// Explicit template instantiation for common types.
template std::vector<ast::ptr<ast::node>>
parser::parse_block<ast::node>(std::string_view,
                               const std::function<ast::ptr<ast::node>()> &);

auto parser::parse_body(std::string_view construct_name)
    -> parser::body_result {
  body_result result;

  if (!at(token_kind::colon)) {
    auto span = previous_span();
    emit(diagnostic(diagnostic_level::error,
                    std::format("expected `:` to start the body of this {}",
                                construct_name),
                    file_id_)
             .with_label(span, "expected `:` after this")
             .with_help(
                 std::format("In Kira, the body of a {} is introduced by `:`. "
                             "For a single expression: `{}: expr`. "
                             "For a block of statements: `{}:` followed by an "
                             "indented block on the next line.",
                             construct_name, construct_name, construct_name)));
    return result;
  }
  advance(); // consume `:`

  // Check for block form: NEWLINE INDENT ...
  if (at(token_kind::newline)) {
    skip_newlines();
    if (at(token_kind::indent)) {
      advance(); // consume INDENT
      result.is_block = true;

      while (!at_any(token_kind::dedent, token_kind::eof)) {
        skip_newlines();
        if (at_any(token_kind::dedent, token_kind::eof)) {
          break;
        }

        auto stmt = parse_stmt();
        if (stmt) {
          result.stmts.push_back(std::move(stmt));
        } else {
          synchronize();
        }
      }

      expect_block_end(construct_name);
      return result;
    }

    // `:` followed by newline but no indent — error.
    emit(diagnostic(
             diagnostic_level::error,
             std::format("expected an indented block after `:` for this {}",
                         construct_name),
             file_id_)
             .with_label(previous_span(), "the `:` is here")
             .with_help("After `:` and a new line, the next line should be "
                        "indented to start a block. If you want a single "
                        "expression, put it on the same line as the `:`."));
    return result;
  }

  // Inline form: `:` expr NEWLINE
  auto expr_start = pos_;
  result.inline_expr = parse_expr();
  if (!result.inline_expr) {
    pos_ = expr_start;
    emit(diagnostic(diagnostic_level::error,
                    std::format("expected an expression after `:` in this {}",
                                construct_name),
                    file_id_)
             .with_label(previous_span(), "the `:` is here")
             .with_help("Put a single expression on the same line as the `:`, "
                        "or move the body to the next line and indent it."));
    return result;
  }
  expect_newline();
  return result;
}

auto parser::body_to_stmt_list(body_result body)
    -> std::vector<ast::ptr<ast::node>> {
  if (body.inline_expr) {
    auto stmt = ast::make<ast::expr_stmt>();
    stmt->expr = std::move(body.inline_expr);

    std::vector<ast::ptr<ast::node>> stmts;
    stmts.push_back(std::move(stmt));
    return stmts;
  }

  return std::move(body.stmts);
}

// ==========================================================================
//  Comma-separated list helpers
// ==========================================================================

template <typename T>
auto parser::parse_delimited_list(
    token_kind open, token_kind close, std::string_view construct_name,
    std::function<std::optional<T>()> parse_element) -> std::vector<T> {
  std::vector<T> items;

  auto open_tok = expect(open);
  auto open_span = open_tok.span;

  while (!at(close) && !at_eof()) {
    skip_newlines();

    if (at(close)) {
      break;
    }

    auto elem = parse_element();
    if (elem) {
      items.push_back(std::move(*elem));
    } else {
      // Skip to the next comma or closing delimiter.
      while (!at_any(token_kind::comma, token_kind::eof) && !at(close)) {
        if (at_any(token_kind::newline, token_kind::dedent)) {
          break;
        }
        advance();
      }
    }

    skip_newlines();

    // Expect comma or closing delimiter.
    if (at(close)) {
      break;
    }
    if (!match(token_kind::comma)) {
      if (at(close)) {
        break; // trailing item without comma is ok before close
      }

      // Missing comma — try to give a helpful error.
      auto span = previous_span();
      emit(diagnostic(diagnostic_level::error,
                      std::format("expected `,` or {} in {}",
                                  token_kind_name(close), construct_name),
                      file_id_)
               .with_label(span, "maybe a `,` is missing here?")
               .with_fix("add a comma",
                         source_span{.start = span.end, .end = span.end},
                         ", "));
      // Don't consume — the next iteration will try to parse the element.
    }
  }

  if (!at(close)) {
    emit(diagnostic(
             diagnostic_level::error,
             std::format("unclosed {} — expected {} to match the {} at line",
                         construct_name, token_kind_name(close),
                         token_kind_name(open)),
             file_id_)
             .with_label(peek().span,
                         std::format("expected {}", token_kind_name(close)))
             .with_secondary_label(
                 open_span,
                 std::format("this {} was opened here", token_kind_name(open)))
             .with_help("Make sure every opening delimiter has a matching "
                        "closing delimiter."));
  } else {
    advance(); // consume close
  }

  return items;
}

template <typename T>
auto parser::parse_comma_list(std::function<std::optional<T>()> parse_element,
                              const std::function<bool()> &at_end_fn)
    -> std::vector<T> {
  std::vector<T> items;

  while (!at_end_fn() && !at_eof()) {
    auto elem = parse_element();
    if (elem) {
      items.push_back(std::move(*elem));
    } else {
      break;
    }

    if (!match(token_kind::comma)) {
      break;
    }
  }

  return items;
}

// ==========================================================================
//  Visibility
// ==========================================================================

auto parser::parse_optional_visibility() -> ast::visibility {
  if (peek().is_visibility()) {
    auto tok = advance();
    return ast::token_to_visibility(tok.kind);
  }
  return ast::visibility::def;
}

// ==========================================================================
//  Module paths
// ==========================================================================

auto parser::parse_module_path() -> std::vector<std::string> {
  std::vector<std::string> segments;

  token first;
  if (at(token_kind::ident) || at(token_kind::kw_super)) {
    first = advance();
  } else {
    first = expect(token_kind::ident);
  }
  segments.emplace_back(first.text);

  while (at(token_kind::dot) && (peek_at(1).is(token_kind::ident) ||
                                 peek_at(1).is(token_kind::kw_super))) {
    advance();            // consume `.`
    auto seg = advance(); // consume ident/`super`
    segments.emplace_back(seg.text);
  }

  return segments;
}

// ==========================================================================
//  Top-level: parse_file
// ==========================================================================

auto parser::parse_file() -> ast::ptr<ast::file> {
  auto file = ast::make<ast::file>();
  auto file_start = peek().span;

  skip_newlines();

  // A `#:` doc comment above the `module` line documents the module itself.
  auto module_doc = collect_doc_comments();

  // Parse module declaration.
  if (at(token_kind::kw_module)) {
    file->module_decl = parse_module_decl();
    if (file->module_decl && !module_doc.empty()) {
      file->module_decl->documentation = std::move(module_doc);
    }
  } else {
    emit(diagnostic(
             diagnostic_level::error,
             "every Kira source file must start with a `module` declaration",
             file_id_)
             .with_label(peek().span, "expected `module` here")
             .with_help(
                 "Add a module declaration at the top of the file, like:\n"
                 "  module my_project.my_module"));
    file->module_decl = ast::make<ast::module_decl>();
    file->module_decl->has_error = true;
  }

  skip_newlines();

  // Optional `no_prelude`.
  if (at(token_kind::kw_no_prelude)) {
    advance();
    file->no_prelude = true;
    expect_newline();
    skip_newlines();
  }

  // A file whose module is exactly `main` is a script: its top level may mix
  // declarations with executable statements, which become the body of a
  // synthesized `main` function (see the "Scripts" section of the language
  // reference).
  const bool script_file = file->module_decl != nullptr &&
                           !file->module_decl->has_error &&
                           file->module_decl->path.size() == 1 &&
                           file->module_decl->path.front() == "main";

  // Parse top-level items.
  while (!at_eof()) {
    skip_newlines();
    // Recovery from malformed nested blocks can bubble one stray DEDENT up to
    // file scope. Consume it here so the file loop always makes progress.
    if (at(token_kind::dedent)) {
      advance();
      continue;
    }
    if (at_eof()) {
      break;
    }

    auto item = parse_top_level_item(script_file);
    if (item) {
      file->items.push_back(std::move(item));
    } else {
      // Recovery: skip to the next top-level item.
      synchronize();
    }
  }

  if (script_file) {
    synthesize_script_main(*file);
  }

  file->span = file_start.merge(previous_span());
  return file;
}

/// Returns whether a parsed top-level node is an executable statement (as
/// opposed to a declaration) for script-mode `main` synthesis.
[[nodiscard]] static auto is_script_stmt(const ast::node &node) noexcept
    -> bool {
  switch (node.kind) {
  case ast::node_kind::let_stmt:
  case ast::node_kind::var_stmt:
  case ast::node_kind::assign_stmt:
  case ast::node_kind::expr_stmt:
  case ast::node_kind::return_stmt:
  case ast::node_kind::if_stmt:
  case ast::node_kind::while_stmt:
  case ast::node_kind::for_stmt:
  case ast::node_kind::match_stmt:
  case ast::node_kind::crew_stmt:
  case ast::node_kind::asm_stmt:
    return true;
  default:
    return false;
  }
}

void parser::synthesize_script_main(ast::file &file) {
  std::vector<ast::ptr<ast::node>> decls;
  std::vector<ast::ptr<ast::node>> stmts;
  decls.reserve(file.items.size());

  std::optional<source_span> explicit_main_span;
  for (auto &item : file.items) {
    if (item == nullptr) {
      continue;
    }
    if (is_script_stmt(*item)) {
      stmts.push_back(std::move(item));
      continue;
    }
    if (item->kind == ast::node_kind::func_decl &&
        dynamic_cast<const ast::func_decl &>(*item).name == "main") {
      explicit_main_span = item->span;
    }
    decls.push_back(std::move(item));
  }

  if (stmts.empty()) {
    // Declarations only — the file behaves like any other module (it may
    // still declare `main` explicitly).
    file.items = std::move(decls);
    return;
  }

  if (explicit_main_span.has_value()) {
    emit(diagnostic(diagnostic_level::error,
                    "this file mixes top-level statements with an explicit "
                    "`def main`",
                    file_id_)
             .with_label(stmts.front()->span,
                         "this statement runs as part of the implicit `main`")
             .with_secondary_label(*explicit_main_span,
                                   "but `main` is already declared here")
             .with_help("A `module main` script gets its `main` function in "
                        "one of two ways — from top-level statements, or from "
                        "an explicit `def main` — but never both. Either move "
                        "these statements into `def main`, or delete the "
                        "declaration and keep the statements."));
    // Drop the statements: the file already failed, and keeping a second
    // `main` around would only cascade into duplicate-symbol noise.
    file.items = std::move(decls);
    return;
  }

  auto fn = ast::make<ast::func_decl>();
  fn->name = "main";
  fn->span = stmts.front()->span.merge(stmts.back()->span);
  // Give the implicit `main` the same signature as the spec's simplest
  // explicit form, `def main() -> unit`. The annotation also matters
  // practically: HIR lowering only accepts explicitly annotated signatures.
  auto ret = ast::make<ast::named_type>();
  ret->span = fn->span;
  ret->path.emplace_back("unit");
  fn->return_type = std::move(ret);
  fn->body_stmts = std::move(stmts);
  decls.push_back(std::move(fn));
  file.items = std::move(decls);
}

// ==========================================================================
//  Module declaration
// ==========================================================================

auto parser::parse_module_decl() -> ast::ptr<ast::module_decl> {
  auto decl = ast::make<ast::module_decl>();
  auto start = peek().span;

  expect(token_kind::kw_module);
  decl->path = parse_module_path();
  expect_newline();

  decl->span = start.merge(previous_span());
  return decl;
}

// ==========================================================================
//  Top-level items
// ==========================================================================

auto parser::parse_top_level_item(bool allow_script_stmts)
    -> ast::ptr<ast::node> {
  skip_newlines();
  if (at_eof()) {
    return nullptr;
  }

  // Capture any `#:` doc comment lines that precede this declaration and hang
  // them on whatever node the dispatch produces.
  auto doc = collect_doc_comments();
  auto item = parse_top_level_item_dispatch(allow_script_stmts);
  if (item && !doc.empty()) {
    item->documentation = std::move(doc);
  }
  return item;
}

auto parser::parse_top_level_item_dispatch(bool allow_script_stmts)
    -> ast::ptr<ast::node> {
  skip_newlines();
  if (at_eof()) {
    return nullptr;
  }

  // Parse optional visibility modifier.
  auto vis = parse_optional_visibility();

  // Dispatch on the current token.
  switch (current()) {
  case token_kind::kw_use:
    return parse_use_decl(vis);

  case token_kind::kw_type:
    return parse_type_decl(vis);

  case token_kind::kw_trait:
    return parse_trait_decl(vis);

  case token_kind::kw_signature:
    return parse_signature_decl(vis);

  case token_kind::kw_impl:
    if (vis != ast::visibility::def) {
      emit(
          diagnostic(diagnostic_level::warning,
                     "visibility modifiers are not meaningful on `impl` blocks",
                     file_id_)
              .with_label(peek().span, "this `impl`")
              .with_help("Remove the visibility modifier — `impl` blocks "
                         "inherit visibility from the trait they implement."));
    }
    return parse_impl_decl();

  case token_kind::kw_extend:
    if (vis != ast::visibility::def) {
      emit(diagnostic(
               diagnostic_level::warning,
               "visibility modifiers are not meaningful on `extend` blocks",
               file_id_)
               .with_label(peek().span, "this `extend`")
               .with_help("Remove the visibility modifier — an `extend` "
                          "block's methods are visible wherever its module "
                          "is `use`d."));
    }
    return parse_extend_decl();

  case token_kind::kw_concept:
    return parse_concept_decl(vis);

  case token_kind::kw_def:
    return parse_func_decl(vis, ast::func_modifiers{});

  case token_kind::kw_pure:
  case token_kind::kw_async:
  case token_kind::kw_machine:
  case token_kind::kw_intrinsic:
  case token_kind::kw_generator: {
    auto mods = parse_func_modifiers();
    if (at(token_kind::kw_def)) {
      return parse_func_decl(vis, std::move(mods));
    }
    emit_unexpected("`def` after function modifiers");
    synchronize_to_newline();
    return make_error_node(peek().span, "expected function declaration");
  }

  case token_kind::kw_packed: {
    auto mods = parse_type_modifiers();
    if (at(token_kind::kw_type)) {
      return parse_type_decl(vis, mods);
    }
    emit_unexpected("`type` after type modifiers");
    synchronize_to_newline();
    return make_error_node(peek().span, "expected type declaration");
  }

  case token_kind::kw_static: {
    if (at_static_func_decl()) {
      auto mods = parse_func_modifiers();
      if (at(token_kind::kw_def)) {
        return parse_func_decl(vis, std::move(mods));
      }
    }
    return parse_static_decl(vis);
  }

  case token_kind::kw_module:
    return parse_sub_module_decl(vis);

  case token_kind::kw_dep:
    if (vis != ast::visibility::def) {
      emit(diagnostic(
               diagnostic_level::warning,
               "visibility modifiers are not meaningful on `dep` declarations",
               file_id_)
               .with_label(peek().span, "here"));
    }
    return parse_dep_decl();

  case token_kind::tilde:
    return parse_splice_stmt();

  case token_kind::newline:
    advance();
    return parse_top_level_item(allow_script_stmts); // Skip blank lines.

  default:
    if (vis != ast::visibility::def) {
      emit(diagnostic(
               diagnostic_level::error,
               std::format("expected a declaration after visibility modifier, "
                           "but found {}",
                           token_kind_name(current())),
               file_id_)
               .with_label(peek().span, "expected a declaration here")
               .with_help("Visibility modifiers (`pub`, `priv`, etc.) can "
                          "only appear before declarations like `def`, "
                          "`type`, `trait`, `use`, etc."));
      synchronize_to_newline();
      return make_error_node(peek().span);
    }

    if (allow_script_stmts) {
      // Script mode (`module main`): the top level accepts ordinary
      // statements, which `synthesize_script_main` later folds into an
      // implicit `main` function.
      return parse_stmt();
    }

    emit(diagnostic(diagnostic_level::error,
                    std::format("unexpected {} at the top level of the file",
                                token_kind_name(current())),
                    file_id_)
             .with_label(peek().span, "this can't appear here")
             .with_help("At the top level, only declarations are allowed: "
                        "`use`, `type`, `trait`, `impl`, `extend`, "
                        "`concept`, `def`, `static`, `module`, `dep`. "
                        "Top-level statements are allowed only in a "
                        "`module main` script file, where they become the "
                        "body of `main`."));
    synchronize_to_newline();
    return make_error_node(peek().span);
  }
}

// ==========================================================================
//  Use declarations
// ==========================================================================

auto parser::parse_use_decl(ast::visibility vis) -> ast::ptr<ast::use_decl> {
  auto decl = ast::make<ast::use_decl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(token_kind::kw_use);

  // Parse the use path: `module_path [ "[" args "]" ] [. use_selector]`
  decl->path = parse_module_path();

  // A bracketed argument list instantiates a parameterized module (functor):
  // `use audited[postgres] as db`. Arguments reuse the type-arg grammar.
  if (at(token_kind::lbracket)) {
    decl->instantiation_args = parse_type_arg_list();
  }

  auto imported_name_span = previous_span();
  if (!decl->instantiation_args.empty() && at(token_kind::kw_as)) {
    // `use audited[postgres] as db`: the alias names the whole instantiation,
    // so the functor path stays intact (nothing is popped). The alias is
    // recorded as a single-item selector whose `name` is the functor's own
    // last path segment.
    advance(); // consume `as`
    ast::use_selector sel;
    sel.kind = ast::use_selector_kind::single;
    ast::use_item item;
    item.name = decl->path.back();
    auto alias_tok = expect(token_kind::ident);
    item.span = imported_name_span.merge(alias_tok.span);
    item.alias = std::string(alias_tok.text);
    sel.span = item.span;
    sel.items.push_back(std::move(item));
    decl->selector = std::move(sel);
  } else if (match(token_kind::kw_as)) {
    if (decl->path.size() < 2) {
      emit(diagnostic(
               diagnostic_level::error,
               "expected `module_path.name as alias` in this `use` declaration",
               file_id_)
               .with_label(previous_span(), "the alias starts here"));
    } else {
      ast::use_selector sel;
      sel.span = imported_name_span;
      sel.kind = ast::use_selector_kind::single;

      ast::use_item item;
      item.name = decl->path.back();
      decl->path.pop_back();
      auto alias_tok = expect(token_kind::ident);
      item.span = imported_name_span.merge(alias_tok.span);
      item.alias = std::string(alias_tok.text);
      sel.items.push_back(std::move(item));
      sel.span = sel.items.front().span;
      decl->selector = std::move(sel);
    }
  }

  // Check for selector: `.{items}` or `.*` or `.Name [as Alias]`
  if (!decl->selector.has_value() && at(token_kind::dot)) {
    // Peek at what follows the dot.
    auto after_dot = peek_at(1);

    if (after_dot.is(token_kind::lbrace)) {
      advance(); // consume `.`
      advance(); // consume `{`

      ast::use_selector sel;
      sel.span = after_dot.span;
      sel.kind = ast::use_selector_kind::group;

      while (!at(token_kind::rbrace) && !at_eof()) {
        skip_newlines();
        if (at(token_kind::rbrace)) {
          break;
        }

        ast::use_item item;
        auto item_tok = expect(token_kind::ident);
        item.span = item_tok.span;
        item.name = std::string(item_tok.text);

        if (match(token_kind::kw_as)) {
          auto alias_tok = expect(token_kind::ident);
          item.alias = std::string(alias_tok.text);
        }

        sel.items.push_back(std::move(item));

        if (!match(token_kind::comma)) {
          skip_newlines();
          break;
        }
        skip_newlines();
      }

      expect(token_kind::rbrace);
      decl->selector = std::move(sel);
    } else if (after_dot.is(token_kind::star)) {
      advance(); // consume `.`
      advance(); // consume `*`

      ast::use_selector sel;
      sel.span = after_dot.span;
      sel.kind = ast::use_selector_kind::wildcard;
      decl->selector = std::move(sel);
    } else if (after_dot.is(token_kind::ident)) {
      advance(); // consume `.`

      ast::use_selector sel;
      sel.span = after_dot.span;
      sel.kind = ast::use_selector_kind::single;

      ast::use_item item;
      auto item_tok = expect(token_kind::ident);
      item.span = item_tok.span;
      item.name = std::string(item_tok.text);

      if (match(token_kind::kw_as)) {
        auto alias_tok = expect(token_kind::ident);
        item.alias = std::string(alias_tok.text);
      }

      sel.items.push_back(std::move(item));
      decl->selector = std::move(sel);
    }
    // Otherwise the dot was part of the module path (already consumed).
  }

  expect_newline();
  decl->span = start.merge(previous_span());
  return decl;
}

// ==========================================================================
//  Sub-module declarations
// ==========================================================================

auto parser::parse_sub_module_decl(ast::visibility vis)
    -> ast::ptr<ast::sub_module_decl> {
  auto decl = ast::make<ast::sub_module_decl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(token_kind::kw_module);
  token name_tok;
  if (at(token_kind::ident) || at(token_kind::kw_pure)) {
    name_tok = advance();
  } else {
    name_tok = expect(token_kind::ident);
  }
  decl->name = std::string(name_tok.text);

  // Optional compile-time parameters make this a parameterized module
  // (functor): `module audited[DB: backend]:`.
  if (at(token_kind::lbracket)) {
    decl->type_params = parse_type_params();
  }

  if (at(token_kind::colon)) {
    advance(); // consume `:`
    skip_newlines();

    if (match(token_kind::indent)) {
      while (!at_any(token_kind::dedent, token_kind::eof)) {
        skip_newlines();
        if (at_any(token_kind::dedent, token_kind::eof)) {
          break;
        }

        auto item = parse_top_level_item();
        if (item) {
          decl->items.push_back(std::move(item));
        } else {
          synchronize();
        }
      }
      expect_block_end("module");
    }
  } else {
    expect_newline();
  }

  decl->span = start.merge(previous_span());
  return decl;
}

// ==========================================================================
//  Dep declarations
// ==========================================================================

auto parser::parse_dep_decl() -> ast::ptr<ast::dep_decl> {
  auto decl = ast::make<ast::dep_decl>();
  auto start = peek().span;

  expect(token_kind::kw_dep);
  auto name_tok = expect(token_kind::ident);
  decl->name = std::string(name_tok.text);

  expect(token_kind::colon);
  skip_newlines();

  if (match(token_kind::indent)) {
    while (!at_any(token_kind::dedent, token_kind::eof)) {
      skip_newlines();
      if (at_any(token_kind::dedent, token_kind::eof)) {
        break;
      }

      ast::dep_field field;
      field.span = peek().span;
      auto key_tok = expect(token_kind::ident);
      field.key = std::string(key_tok.text);
      expect(token_kind::eq);
      auto val_tok = expect(token_kind::string_lit);
      field.value = std::string(val_tok.text);
      expect_newline();

      decl->fields.push_back(std::move(field));
    }
    expect_block_end("dep");
  }

  decl->span = start.merge(previous_span());
  return decl;
}

// ==========================================================================
//  Type declarations
// ==========================================================================

auto parser::parse_type_modifiers() -> ast::type_modifiers {
  ast::type_modifiers mods;
  bool keep_going = true;
  while (keep_going) {
    switch (current()) {
    case token_kind::kw_packed:
      if (mods.is_packed) {
        emit(diagnostic(diagnostic_level::warning,
                        "duplicate `packed` modifier — you only need it once",
                        file_id_)
                 .with_label(peek().span, "this `packed` is redundant"));
      }
      mods.is_packed = true;
      advance();
      break;
    default:
      keep_going = false;
      break;
    }
  }
  return mods;
}

auto parser::parse_type_decl(ast::visibility vis, ast::type_modifiers mods)
    -> ast::ptr<ast::type_decl> {
  auto decl = ast::make<ast::type_decl>();
  auto start = peek().span;
  decl->visibility = vis;
  decl->modifiers = mods;

  expect(token_kind::kw_type);
  auto name_tok = expect(token_kind::ident);
  decl->name = std::string(name_tok.text);

  // Optional type parameters.
  if (at(token_kind::lbracket)) {
    decl->type_params = parse_type_params();
  }

  expect(token_kind::eq);

  // Parse the type definition.
  decl->definition = parse_type_def();

  // Check for inline deriving.
  if (at(token_kind::kw_deriving)) {
    advance();
    // Parse deriving list.
    auto id_tok = expect(token_kind::ident);
    decl->deriving.emplace_back(id_tok.text);
    while (match(token_kind::comma)) {
      auto next_tok = expect(token_kind::ident);
      decl->deriving.emplace_back(next_tok.text);
    }
  }

  // If there's a newline followed by an indent, check for deriving/invariant
  // clauses.
  if (at(token_kind::newline)) {
    auto saved_pos = pos_;
    advance();
    skip_newlines();

    if (at(token_kind::indent)) {
      advance();
      // Look for deriving and invariant clauses.
      while (!at_any(token_kind::dedent, token_kind::eof)) {
        skip_newlines();
        if (at_any(token_kind::dedent, token_kind::eof)) {
          break;
        }

        if (at(token_kind::kw_deriving)) {
          advance();
          auto id_tok = expect(token_kind::ident);
          decl->deriving.emplace_back(id_tok.text);
          while (match(token_kind::comma)) {
            auto next_tok = expect(token_kind::ident);
            decl->deriving.emplace_back(next_tok.text);
          }
          expect_newline();
        } else if (at(token_kind::kw_invariant)) {
          advance();
          decl->invariant = parse_expr();
          expect_newline();
        } else {
          break;
        }
      }
      expect_block_end("type");
    } else {
      // No indent block — restore position.
      pos_ = saved_pos;
      expect_newline();
    }
  } else {
    expect_newline();
  }

  decl->span = start.merge(previous_span());
  return decl;
}

auto parser::parse_type_def() -> ast::ptr<ast::node> {
  // Sum body: either `| @variant ...` or the unprefixed documented form
  // `@variant | @other_variant`. Every variant starts with `@`, so no
  // lookahead beyond the next token is needed to disambiguate.
  if (at(token_kind::pipe) || at(token_kind::at)) {
    auto body = parse_sum_body();
    auto result = ast::make<ast::sum_type_def>();
    result->span = body.span;
    result->body = std::move(body);
    return result;
  }

  // Struct body: starts with `{`
  if (at(token_kind::lbrace)) {
    auto body = parse_struct_body();
    auto result = ast::make<ast::struct_type_def>();
    result->span = body.span;
    result->body = std::move(body);
    return result;
  }

  // Otherwise it's a type expression (or refinement type).
  auto type = parse_type_expr();
  if (!type) {
    return make_error_node(peek().span, "expected type definition");
  }

  // Check for refinement: `type_expr where expr`
  // e.g. `type positive = int32 where self > 0`
  if (at(token_kind::kw_where)) {
    advance(); // consume `where`
    auto refinement = ast::make<ast::refinement_type>();
    refinement->span = type->span;
    refinement->base = std::move(type);
    refinement->predicate = parse_expr();
    if (!refinement->predicate) {
      emit_unexpected(
          "a predicate expression after `where` in refinement type");
      refinement->predicate =
          make_error_node(previous_span(), "expected refinement predicate");
      refinement->has_error = true;
    }
    refinement->span.extend_to(previous_span());
    return refinement;
  }

  return type;
}

auto parser::parse_struct_body() -> ast::struct_body {
  ast::struct_body body;
  body.span = peek().span;

  expect(token_kind::lbrace);

  while (!at(token_kind::rbrace) && !at_eof()) {
    skip_newlines();
    if (at(token_kind::rbrace)) {
      break;
    }

    body.fields.push_back(parse_struct_field());

    if (!match(token_kind::comma)) {
      skip_newlines();
      break;
    }
    skip_newlines();
  }

  auto close = expect(token_kind::rbrace);
  body.span.extend_to(close.span);
  return body;
}

auto parser::parse_struct_field() -> ast::struct_field {
  ast::struct_field field;
  // A `#:` doc comment on its own line inside the `{ ... }` body documents the
  // field that follows it.
  field.documentation = collect_doc_comments();
  field.span = peek().span;
  field.visibility = parse_optional_visibility();

  auto name_tok = expect(token_kind::ident);
  field.name = std::string(name_tok.text);
  expect(token_kind::colon);
  field.type = parse_type_expr();

  field.span.extend_to(previous_span());
  return field;
}

auto parser::parse_sum_body() -> ast::sum_body {
  ast::sum_body body;
  body.span = peek().span;

  // The reference allows both `| a | b` and `a | b` forms.
  match(token_kind::pipe);
  body.variants.push_back(parse_sum_variant());

  while (match(token_kind::pipe)) {
    body.variants.push_back(parse_sum_variant());
  }

  body.span.extend_to(previous_span());
  return body;
}

auto parser::parse_sum_variant() -> ast::sum_variant {
  ast::sum_variant variant;
  // Attach any preceding `#:` doc comment. Sum variants are written on one
  // line today, so this rarely fires, but keeps variant docs in the AST if a
  // documented multi-line form is ever supported.
  variant.documentation = collect_doc_comments();
  variant.span = peek().span;

  expect(token_kind::at);

  token name_tok;
  if (can_start_sum_variant(current())) {
    name_tok = advance();
  } else {
    name_tok = expect(token_kind::ident);
  }
  variant.name = token_text_string(name_tok);

  if (match(token_kind::lparen)) {
    while (!at(token_kind::rparen) && !at_eof()) {
      auto type = parse_type_expr();
      if (type) {
        variant.payload_types.push_back(std::move(type));
      }
      if (!match(token_kind::comma)) {
        break;
      }
    }
    expect(token_kind::rparen);
  }

  variant.span.extend_to(previous_span());
  return variant;
}

// ==========================================================================
//  Type expressions
// ==========================================================================

auto parser::parse_type_expr() -> ast::ptr<ast::type_expr> {
  auto first = parse_prim_type_expr();
  if (!first) {
    return nullptr;
  }

  // Union type: `type | type | ...`
  if (at(token_kind::pipe)) {
    auto union_type = ast::make<ast::union_type>();
    union_type->span = first->span;
    union_type->alternatives.push_back(std::move(first));

    while (match(token_kind::pipe)) {
      auto next = parse_prim_type_expr();
      if (next) {
        union_type->alternatives.push_back(std::move(next));
      }
    }

    union_type->span.extend_to(previous_span());
    return union_type;
  }

  return first;
}

auto parser::parse_prim_type_expr() -> ast::ptr<ast::type_expr> {
  switch (current()) {
  case token_kind::ident: {
    // `expr`/`stmt`/`def_expr`/`type_expr` are contextual keywords: an
    // ordinary identifier everywhere else (so `expr.lit(5)` can parse as a
    // field access), but in type position their bare spelling names one of
    // the four quote-value types instead of an ordinary named type.
    const auto &tok = peek();
    if (tok.text == "expr" || tok.text == "stmt" || tok.text == "def_expr" ||
        tok.text == "type_expr") {
      const auto kind = tok.text == "expr"   ? ast::quote_fragment_kind::expr
                        : tok.text == "stmt" ? ast::quote_fragment_kind::stmt
                        : tok.text == "def_expr"
                            ? ast::quote_fragment_kind::def_expr
                            : ast::quote_fragment_kind::type_expr;
      advance();
      auto qt = ast::make<ast::quote_type>(kind);
      qt->span = tok.span;
      return qt;
    }
    return parse_named_type();
  }
  case token_kind::kw_super:
    return parse_named_type();

  case token_kind::kw_unit: {
    // `unit` is a keyword token, but in type position it names the unit type.
    auto tok = advance();
    auto named = ast::make<ast::named_type>();
    named->span = tok.span;
    named->path.emplace_back("unit");
    return named;
  }

  case token_kind::kw_generator: {
    // `generator` is a keyword (function modifier), but in type position
    // `generator[T]` names the concrete type a `generator def` produces —
    // the structural escape hatch `some iterator[T]` sugars over (see
    // `check_function`'s generator handling). Reuses the same `[...]`
    // argument grammar `parse_named_type` uses, just without routing
    // through `parse_module_path` (which only recognizes ordinary
    // identifiers).
    auto tok = advance();
    auto named = ast::make<ast::named_type>();
    named->span = tok.span;
    named->path.emplace_back("generator");
    parse_generic_args_suffix(*named);
    named->span.extend_to(previous_span());
    return named;
  }

  case token_kind::kw_some: {
    // `some Bound` — an existential type. Reuses the exact `+`-joined
    // bound-term parsing `where`/type-param constraints already use, just
    // legal here in type position too.
    auto start = peek().span;
    advance(); // consume `some`
    auto bound = parse_bound();
    auto ety = ast::make<ast::existential_type>();
    ety->value = std::move(bound);
    ety->span = start.merge(previous_span());
    return ety;
  }

  case token_kind::lparen: {
    // Could be tuple type or grouped type.
    auto start = peek().span;
    advance(); // consume `(`

    auto first = parse_type_expr();
    if (!first) {
      expect(token_kind::rparen);
      auto err = ast::make<ast::error_type_expr>(start.merge(previous_span()));
      return err;
    }

    if (at(token_kind::comma)) {
      // Tuple type.
      auto tuple = ast::make<ast::tuple_type>();
      tuple->span = start;
      tuple->elements.push_back(std::move(first));

      while (match(token_kind::comma)) {
        if (at(token_kind::rparen)) {
          break; // trailing comma
        }
        auto elem = parse_type_expr();
        if (elem) {
          tuple->elements.push_back(std::move(elem));
        }
      }

      expect(token_kind::rparen);
      tuple->span.extend_to(previous_span());
      return tuple;
    }

    // Parenthesized type expr.
    expect(token_kind::rparen);
    first->span = start.merge(previous_span());
    return first;
  }

  case token_kind::lbracket:
    return parse_slice_type();

  case token_kind::kw_array:
    return parse_array_type();

  case token_kind::amp:
    return parse_ref_type();

  case token_kind::star:
    return parse_ptr_type();

  case token_kind::kw_fn:
    return parse_fn_type();

  case token_kind::tilde: {
    // Type-position splice: `~(expr)` — mirrors `parse_splice_expr_inner`
    // (expression position), just building a `splice_type` instead.
    auto sty = ast::make<ast::splice_type>();
    auto start = peek().span;
    advance(); // consume `~`
    if (at(token_kind::lparen)) {
      advance(); // consume `(`
      sty->operand = parse_expr();
      expect(token_kind::rparen);
    } else if (peek().can_start_expr()) {
      sty->operand = parse_postfix_expr();
    } else {
      emit_unexpected("an expression after `~` in type position");
      sty->operand = make_error_expr(start);
    }
    sty->span = start.merge(previous_span());
    return sty;
  }

  default:
    return nullptr;
  }
}

auto parser::make_bound_type(ast::bound bound) -> ast::ptr<ast::bound_type> {
  auto type = ast::make<ast::bound_type>();
  type->span = bound.span;
  type->value = std::move(bound);
  return type;
}

auto parser::parse_optional_type_annotation() -> ast::ptr<ast::type_expr> {
  if (!match(token_kind::colon)) {
    return nullptr;
  }

  if (match(token_kind::kw_shared)) {
    auto shared_type = ast::make<ast::named_type>();
    shared_type->span = previous_span();
    shared_type->path.emplace_back("shared");

    auto inner_type = parse_type_expr();
    if (inner_type) {
      ast::type_arg inner_arg;
      inner_arg.span = inner_type->span;
      inner_arg.value = std::move(inner_type);
      shared_type->type_args.push_back(std::move(inner_arg));
      shared_type->span.extend_to(previous_span());
    }

    return shared_type;
  }

  return parse_type_expr();
}

/// Parses an optional `[T, U, ...]` generic/value argument suffix directly
/// onto `named` (which must already have its `path` set) — shared by
/// `parse_named_type` and any other type-position production that builds a
/// `named_type` from a keyword rather than an ordinary path (e.g.
/// `generator[T]` — `generator` is a keyword, so it can't route through
/// `parse_module_path`, but still wants the same `[...]` argument grammar).
auto parser::parse_type_arg_list() -> std::vector<ast::type_arg> {
  std::vector<ast::type_arg> args;
  advance(); // consume `[`
  while (!at(token_kind::rbracket) && !at_eof()) {
    skip_newlines();
    if (at(token_kind::rbracket)) {
      break;
    }

    ast::type_arg type_arg;
    type_arg.span = peek().span;

    if (at(token_kind::ident) && peek_at(1).is(token_kind::colon)) {
      auto name_tok = advance();
      type_arg.name = std::string(name_tok.text);
      advance(); // consume `:`
      type_arg.value = parse_type_expr();
    } else {
      auto arg_start = pos_;
      auto maybe_type = parse_type_expr();
      if (maybe_type && at_any(token_kind::comma, token_kind::rbracket)) {
        type_arg.value = std::move(maybe_type);
      } else {
        pos_ = arg_start;
        type_arg.value = parse_expr();
      }
    }

    if (type_arg.value) {
      type_arg.span.extend_to(type_arg.value->span);
      args.push_back(std::move(type_arg));
    }

    if (!match(token_kind::comma)) {
      break;
    }
    skip_newlines();
  }
  expect(token_kind::rbracket);
  return args;
}

auto parser::parse_generic_args_suffix(ast::named_type &named) -> void {
  if (!at(token_kind::lbracket)) {
    return;
  }
  named.type_args = parse_type_arg_list();
}

auto parser::parse_named_type() -> ast::ptr<ast::named_type> {
  auto named = ast::make<ast::named_type>();
  named->span = peek().span;

  named->path = parse_module_path();
  parse_generic_args_suffix(*named);

  named->span.extend_to(previous_span());
  return named;
}

auto parser::parse_tuple_type() -> ast::ptr<ast::tuple_type> {
  // This is called when we already know we have a tuple.
  // The LParen has NOT been consumed yet.
  auto tuple = ast::make<ast::tuple_type>();
  tuple->span = peek().span;

  expect(token_kind::lparen);
  while (!at(token_kind::rparen) && !at_eof()) {
    auto elem = parse_type_expr();
    if (elem) {
      tuple->elements.push_back(std::move(elem));
    }
    if (!match(token_kind::comma)) {
      break;
    }
  }
  expect(token_kind::rparen);

  tuple->span.extend_to(previous_span());
  return tuple;
}

auto parser::parse_slice_type() -> ast::ptr<ast::slice_type> {
  auto slice = ast::make<ast::slice_type>();
  slice->span = peek().span;

  expect(token_kind::lbracket);
  slice->element = parse_type_expr();
  expect(token_kind::rbracket);

  slice->span.extend_to(previous_span());
  return slice;
}

auto parser::parse_array_type() -> ast::ptr<ast::array_type> {
  auto arr = ast::make<ast::array_type>();
  arr->span = peek().span;

  expect(token_kind::kw_array);
  expect(token_kind::lbracket);
  arr->element = parse_type_expr();
  expect(token_kind::comma);
  arr->size = parse_expr();
  expect(token_kind::rbracket);

  arr->span.extend_to(previous_span());
  return arr;
}

auto parser::parse_ref_type() -> ast::ptr<ast::ref_type> {
  auto ref = ast::make<ast::ref_type>();
  ref->span = peek().span;

  expect(token_kind::amp);
  ref->is_mut = match(token_kind::kw_mut);
  ref->inner = parse_type_expr();

  ref->span.extend_to(previous_span());
  return ref;
}

auto parser::parse_ptr_type() -> ast::ptr<ast::ptr_type> {
  auto ptr = ast::make<ast::ptr_type>();
  ptr->span = peek().span;

  expect(token_kind::star);
  ptr->is_mut = match(token_kind::kw_mut);
  ptr->inner = parse_type_expr();

  ptr->span.extend_to(previous_span());
  return ptr;
}

auto parser::parse_fn_type() -> ast::ptr<ast::fn_type> {
  auto fn = ast::make<ast::fn_type>();
  fn->span = peek().span;

  expect(token_kind::kw_fn);
  expect(token_kind::lparen);

  while (!at(token_kind::rparen) && !at_eof()) {
    auto param = parse_type_expr();
    if (param) {
      fn->param_types.push_back(std::move(param));
    }
    if (!match(token_kind::comma)) {
      break;
    }
  }

  expect(token_kind::rparen);

  if (match(token_kind::arrow)) {
    fn->return_type = parse_type_expr();
  }

  fn->span.extend_to(previous_span());
  return fn;
}

// ==========================================================================
//  Type parameters and bounds
// ==========================================================================

auto parser::parse_type_params() -> std::vector<ast::type_param> {
  std::vector<ast::type_param> params;

  expect(token_kind::lbracket);

  while (!at(token_kind::rbracket) && !at_eof()) {
    skip_newlines();
    if (at(token_kind::rbracket)) {
      break;
    }

    params.push_back(parse_type_param());

    if (!match(token_kind::comma)) {
      break;
    }
    skip_newlines();
  }

  expect(token_kind::rbracket);
  return params;
}

auto parser::parse_type_param() -> ast::type_param {
  ast::type_param param;
  param.span = peek().span;

  auto name_tok = expect(token_kind::ident);
  param.name = std::string(name_tok.text);

  if (match(token_kind::lbracket)) {
    expect(token_kind::kw_underscore);
    param.higher_kinded_arity = 1;
    while (match(token_kind::comma)) {
      expect(token_kind::kw_underscore);
      ++param.higher_kinded_arity;
    }
    expect(token_kind::rbracket);
  }

  if (match(token_kind::colon)) {
    auto saved_pos = pos_;
    auto maybe_type = parse_type_expr();
    if (maybe_type && at_any(token_kind::comma, token_kind::rbracket)) {
      param.bound_or_type = std::move(maybe_type);
      param.is_value_param = true;
    } else {
      pos_ = saved_pos;
      auto bound = parse_bound();
      param.bound_or_type = make_bound_type(std::move(bound));
    }
  }

  param.span.extend_to(previous_span());
  return param;
}

auto parser::parse_bound() -> ast::bound {
  ast::bound bound;
  bound.span = peek().span;

  bound.terms.push_back(parse_bound_term());

  while (match(token_kind::plus)) {
    bound.terms.push_back(parse_bound_term());
  }

  bound.span.extend_to(previous_span());
  return bound;
}

auto parser::parse_bound_term() -> ast::bound_term {
  ast::bound_term term;
  term.span = peek().span;

  if (at(token_kind::kw_fn)) {
    term.type = parse_fn_type();
  } else {
    term.type = parse_named_type();
  }

  term.span.extend_to(previous_span());
  return term;
}

auto parser::parse_where_clause() -> std::vector<ast::where_constraint> {
  std::vector<ast::where_constraint> constraints;

  expect(token_kind::kw_where);

  while (true) {
    ast::where_constraint c;
    c.span = peek().span;
    c.subject = parse_type_expr();
    expect(token_kind::colon);
    auto bound = parse_bound();
    if (bound.terms.size() > 1 ||
        (bound.terms.size() == 1 && bound.terms.front().type != nullptr &&
         bound.terms.front().type->kind == ast::node_kind::fn_type)) {
      c.bound_or_type = make_bound_type(std::move(bound));
    } else if (bound.terms.size() == 1 && bound.terms.front().type != nullptr) {
      c.bound_or_type = std::move(bound.terms.front().type);
    }
    c.span.extend_to(previous_span());
    constraints.push_back(std::move(c));
    if (!match(token_kind::comma)) {
      break;
    }
  }

  return constraints;
}

// ==========================================================================
//  Trait declarations
// ==========================================================================

auto parser::parse_trait_decl(ast::visibility vis)
    -> ast::ptr<ast::trait_decl> {
  auto decl = ast::make<ast::trait_decl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(token_kind::kw_trait);
  auto name_tok = expect(token_kind::ident);
  decl->name = std::string(name_tok.text);

  if (at(token_kind::lbracket)) {
    decl->type_params = parse_type_params();
  }

  if (at(token_kind::kw_requires)) {
    advance();
    decl->requires_bound = parse_bound();
  }

  expect(token_kind::colon);
  skip_newlines();

  if (match(token_kind::indent)) {
    while (!at_any(token_kind::dedent, token_kind::eof)) {
      skip_newlines();
      if (at_any(token_kind::dedent, token_kind::eof)) {
        break;
      }

      // Parse trait items: func_decl, static_decl, associated_type_decl.
      auto item_doc = collect_doc_comments();
      auto item_vis = parse_optional_visibility();
      auto items_before = decl->items.size();

      if (at(token_kind::kw_static) && !at_static_func_decl()) {
        decl->items.push_back(parse_static_decl(item_vis));
      } else if (at(token_kind::kw_def) ||
                 at_any(token_kind::kw_pure, token_kind::kw_async,
                        token_kind::kw_machine, token_kind::kw_generator,
                        token_kind::kw_static)) {
        auto mods = parse_func_modifiers();
        if (at(token_kind::kw_def)) {
          decl->items.push_back(
              parse_func_decl(item_vis, std::move(mods), true));
        } else {
          emit_unexpected("`def` in trait body");
          synchronize_to_newline();
        }
      } else if (at(token_kind::kw_type)) {
        // Associated type declaration.
        auto assoc = ast::make<ast::associated_type_decl_node>();
        assoc->value.visibility = item_vis;
        assoc->span = peek().span;
        advance(); // consume `type`
        auto aname_tok = expect(token_kind::ident);
        assoc->value.name = std::string(aname_tok.text);
        if (match(token_kind::eq)) {
          assoc->value.default_type = parse_type_expr();
        }
        expect_newline();
        assoc->value.span = assoc->span.merge(previous_span());
        assoc->span = assoc->value.span;
        decl->items.push_back(std::move(assoc));
      } else if (at(token_kind::newline)) {
        advance();
      } else {
        emit_unexpected("a trait member (function, type, or static)");
        synchronize_to_newline();
      }

      attach_member_doc(decl->items, items_before, std::move(item_doc));
    }
    expect_block_end("trait");
  }

  decl->span = start.merge(previous_span());
  return decl;
}

// ==========================================================================
//  Signature declarations
// ==========================================================================

auto parser::parse_signature_decl(ast::visibility vis)
    -> ast::ptr<ast::signature_decl> {
  auto decl = ast::make<ast::signature_decl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(token_kind::kw_signature);
  auto name_tok = expect(token_kind::ident);
  decl->name = std::string(name_tok.text);

  expect(token_kind::colon);
  skip_newlines();

  if (match(token_kind::indent)) {
    while (!at_any(token_kind::dedent, token_kind::eof)) {
      skip_newlines();
      if (at_any(token_kind::dedent, token_kind::eof)) {
        break;
      }

      auto item_doc = collect_doc_comments();
      auto item_vis = parse_optional_visibility();
      auto items_before = decl->items.size();

      if (at(token_kind::kw_static)) {
        // Required constant: `static NAME: Type` (no initializer). A signature
        // states the required type only; the providing module supplies the
        // value, so `parse_static_decl` (which demands `= expr`) is not used.
        auto stat = ast::make<ast::static_decl>();
        stat->visibility = item_vis;
        stat->decl_kind = ast::static_decl_kind::binding;
        stat->span = peek().span;
        advance(); // consume `static`
        auto sname_tok = expect(token_kind::ident);
        stat->name = std::string(sname_tok.text);
        expect(token_kind::colon);
        stat->type_annotation = parse_type_expr();
        expect_newline();
        stat->span = stat->span.merge(previous_span());
        decl->items.push_back(std::move(stat));
      } else if (at(token_kind::kw_def) ||
                 at_any(token_kind::kw_pure, token_kind::kw_async,
                        token_kind::kw_machine, token_kind::kw_generator)) {
        // Required function: a body-less `def` signature.
        auto mods = parse_func_modifiers();
        if (at(token_kind::kw_def)) {
          decl->items.push_back(
              parse_func_decl(item_vis, std::move(mods), true));
        } else {
          emit_unexpected("`def` in signature body");
          synchronize_to_newline();
        }
      } else if (at(token_kind::kw_type)) {
        // Abstract type member: `type NAME` (no definition).
        auto assoc = ast::make<ast::associated_type_decl_node>();
        assoc->value.visibility = item_vis;
        assoc->span = peek().span;
        advance(); // consume `type`
        auto aname_tok = expect(token_kind::ident);
        assoc->value.name = std::string(aname_tok.text);
        if (at(token_kind::colon)) {
          emit(
              diagnostic(
                  diagnostic_level::error,
                  "bounds on a signature's abstract type are not supported yet",
                  file_id_)
                  .with_label(peek().span, "remove this bound")
                  .with_help("An abstract type in a `signature` is written "
                             "`type name` with no bound. Constrain the "
                             "concrete type at the point that provides it "
                             "instead."));
          synchronize_to_newline();
        } else {
          expect_newline();
        }
        assoc->value.span = assoc->span.merge(previous_span());
        assoc->span = assoc->value.span;
        decl->items.push_back(std::move(assoc));
      } else if (at(token_kind::newline)) {
        advance();
      } else {
        emit_unexpected("a signature member (`type`, `def`, or `static`)");
        synchronize_to_newline();
      }

      attach_member_doc(decl->items, items_before, std::move(item_doc));
    }
    expect_block_end("signature");
  }

  decl->span = start.merge(previous_span());
  return decl;
}

// ==========================================================================
//  Concept declarations
// ==========================================================================

auto parser::parse_concept_decl(ast::visibility vis)
    -> ast::ptr<ast::concept_decl> {
  auto decl = ast::make<ast::concept_decl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(token_kind::kw_concept);
  auto name_tok = expect(token_kind::ident);
  decl->name = std::string(name_tok.text);

  // Parse concept parameters.
  expect(token_kind::lbracket);
  while (!at(token_kind::rbracket) && !at_eof()) {
    ast::concept_param param;
    param.span = peek().span;
    auto pname_tok = expect(token_kind::ident);
    param.name = std::string(pname_tok.text);

    if (match(token_kind::lbracket)) {
      expect(token_kind::kw_underscore);
      param.higher_kinded_arity = 1;
      while (match(token_kind::comma)) {
        expect(token_kind::kw_underscore);
        ++param.higher_kinded_arity;
      }
      expect(token_kind::rbracket);
    }

    decl->params.push_back(std::move(param));
    if (!match(token_kind::comma)) {
      break;
    }
  }
  expect(token_kind::rbracket);

  expect(token_kind::colon);
  skip_newlines();

  if (match(token_kind::indent)) {
    while (!at_any(token_kind::dedent, token_kind::eof)) {
      skip_newlines();
      if (at_any(token_kind::dedent, token_kind::eof)) {
        break;
      }

      // Parse concept constraints.
      ast::concept_constraint constraint;
      constraint.span = peek().span;
      // Try to parse `type : bound` or just `expr`.
      // We parse as type expr, then check for `:`.
      auto constraint_start = pos_;
      auto subject = parse_type_expr();
      if (at(token_kind::colon)) {
        advance();
        constraint.subject = std::move(subject);
        constraint.bound_or_expr = parse_type_expr();
      } else {
        // Value constraint — reparse from the start as an expression so
        // operators like `+` or calls are preserved.
        pos_ = constraint_start;
        constraint.bound_or_expr = parse_expr();
      }
      expect_newline();
      decl->constraints.push_back(std::move(constraint));
    }
    expect_block_end("concept");
  }

  decl->span = start.merge(previous_span());
  return decl;
}

// ==========================================================================
//  Impl declarations
// ==========================================================================

auto parser::parse_impl_decl() -> ast::ptr<ast::impl_decl> {
  auto decl = ast::make<ast::impl_decl>();
  auto start = peek().span;

  expect(token_kind::kw_impl);

  // Optional type parameters.
  if (at(token_kind::lbracket)) {
    decl->type_params = parse_type_params();
  }

  decl->trait_type = parse_named_type();
  if (match(token_kind::kw_for)) {
    decl->for_type = parse_type_expr();
  } else if (!at_any(token_kind::kw_where, token_kind::colon,
                     token_kind::newline, token_kind::eof)) {
    expect_with_context(token_kind::kw_for, "in `impl Trait for Type`");
    decl->for_type = parse_type_expr();
  }

  // Optional where clause.
  if (at(token_kind::kw_where)) {
    decl->where_constraints = parse_where_clause();
  }

  expect(token_kind::colon);
  skip_newlines();

  if (match(token_kind::indent)) {
    while (!at_any(token_kind::dedent, token_kind::eof)) {
      skip_newlines();
      if (at_any(token_kind::dedent, token_kind::eof)) {
        break;
      }

      auto item_doc = collect_doc_comments();
      auto item_vis = parse_optional_visibility();
      auto items_before = decl->items.size();

      if (at(token_kind::kw_static) && !at_static_func_decl()) {
        decl->items.push_back(parse_static_decl(item_vis));
      } else if (at(token_kind::kw_def) ||
                 at_any(token_kind::kw_pure, token_kind::kw_async,
                        token_kind::kw_machine, token_kind::kw_generator,
                        token_kind::kw_static)) {
        auto mods = parse_func_modifiers();
        if (at(token_kind::kw_def)) {
          decl->items.push_back(
              parse_func_decl(item_vis, std::move(mods), true));
        } else {
          emit_unexpected("`def` in impl body");
          synchronize_to_newline();
        }
      } else if (at(token_kind::kw_type)) {
        // Associated type definition.
        auto assoc = ast::make<ast::associated_type_def_node>();
        assoc->span = peek().span;
        advance(); // consume `type`
        auto assoc_name_tok = expect(token_kind::ident);
        assoc->value.name = std::string(assoc_name_tok.text);
        expect(token_kind::eq);
        assoc->value.type = parse_type_expr();
        expect_newline();
        assoc->value.span = assoc->span.merge(previous_span());
        assoc->span = assoc->value.span;
        decl->items.push_back(std::move(assoc));
      } else if (at(token_kind::newline)) {
        advance();
      } else {
        emit_unexpected("an impl member (function, type, or static)");
        synchronize_to_newline();
      }

      attach_member_doc(decl->items, items_before, std::move(item_doc));
    }
    expect_block_end("impl");
  }

  decl->span = start.merge(previous_span());
  return decl;
}

/// Parses an `extend` block: `extend Type: def method(self) -> ...`.
/// Unlike `impl`, there is no trait, no `where` clause, and no associated
/// types — extend only ever adds methods to a concrete target type.
auto parser::parse_extend_decl() -> ast::ptr<ast::extend_decl> {
  auto decl = ast::make<ast::extend_decl>();
  auto start = peek().span;

  expect(token_kind::kw_extend);

  // Optional type parameters, as on `impl`: `extend[T] gen[T]:`. Without
  // these a parameterized type has no inherent-method form at all — `impl`
  // demands a trait, and a bare `extend gen:` cannot name `T` to talk about
  // the field types it stores.
  if (at(token_kind::lbracket)) {
    decl->type_params = parse_type_params();
  }

  decl->for_type = parse_type_expr();

  expect(token_kind::colon);
  skip_newlines();

  if (match(token_kind::indent)) {
    while (!at_any(token_kind::dedent, token_kind::eof)) {
      skip_newlines();
      if (at_any(token_kind::dedent, token_kind::eof)) {
        break;
      }

      auto item_doc = collect_doc_comments();
      auto item_vis = parse_optional_visibility();
      auto items_before = decl->items.size();

      if (at(token_kind::kw_def) ||
          at_any(token_kind::kw_pure, token_kind::kw_async,
                 token_kind::kw_machine, token_kind::kw_generator,
                 token_kind::kw_static)) {
        auto mods = parse_func_modifiers();
        if (at(token_kind::kw_def)) {
          decl->items.push_back(parse_func_decl(item_vis, std::move(mods)));
        } else {
          emit_unexpected("`def` in extend body");
          synchronize_to_newline();
        }
      } else if (at(token_kind::newline)) {
        advance();
      } else {
        emit_unexpected("an extend member (function)");
        synchronize_to_newline();
      }

      attach_member_doc(decl->items, items_before, std::move(item_doc));
    }
    expect_block_end("extend");
  }

  decl->span = start.merge(previous_span());
  return decl;
}

// ==========================================================================
//  Function declarations
// ==========================================================================

auto parser::at_static_func_decl() const noexcept -> bool {
  auto ahead = uint32_t{1}; // past the `static` sitting at the cursor
  while (peek_at(ahead).is_func_modifier()) {
    ++ahead;
    // `async` carries an optional `[Context]`. Skipping the bracket group
    // keeps the `def` behind it reachable — a fixed-depth lookahead could
    // not see past one, so `static async[io] def` used to read as a static
    // binding and fail on the `def`.
    auto depth = 0;
    while (peek_at(ahead).is(token_kind::lbracket) || depth > 0) {
      if (peek_at(ahead).is(token_kind::eof)) {
        return false;
      }
      if (peek_at(ahead).is(token_kind::lbracket)) {
        ++depth;
      } else if (peek_at(ahead).is(token_kind::rbracket)) {
        --depth;
      }
      ++ahead;
    }
  }
  return peek_at(ahead).is(token_kind::kw_def);
}

auto parser::bracket_group_precedes_call() const noexcept -> bool {
  auto ahead = uint32_t{0};
  auto depth = 0;
  while (peek_at(ahead).is(token_kind::lbracket) || depth > 0) {
    if (peek_at(ahead).is(token_kind::eof)) {
      return false;
    }
    if (peek_at(ahead).is(token_kind::lbracket)) {
      ++depth;
    } else if (peek_at(ahead).is(token_kind::rbracket)) {
      --depth;
    }
    ++ahead;
  }
  return peek_at(ahead).is(token_kind::lparen);
}

auto parser::parse_func_modifiers() -> ast::func_modifiers {
  ast::func_modifiers mods;

  // Parse modifiers in any order — canonical is static pure async machine
  // but we accept any combination and let semantic analysis sort it out.
  bool keep_going = true;
  while (keep_going) {
    switch (current()) {
    case token_kind::kw_pure:
      if (mods.is_pure) {
        emit(diagnostic(diagnostic_level::warning,
                        "duplicate `pure` modifier — you only need it once",
                        file_id_)
                 .with_label(peek().span, "this `pure` is redundant"));
      }
      mods.is_pure = true;
      advance();
      break;
    case token_kind::kw_async:
      if (mods.is_async) {
        emit(diagnostic(diagnostic_level::warning,
                        "duplicate `async` modifier — you only need it once",
                        file_id_)
                 .with_label(peek().span, "this `async` is redundant"));
      }
      mods.is_async = true;
      advance();
      // Optional async context: `async[ContextType]`
      if (at(token_kind::lbracket)) {
        advance();
        mods.async_context = parse_type_expr();
        expect(token_kind::rbracket);
      }
      break;
    case token_kind::kw_machine:
      if (mods.is_machine) {
        emit(diagnostic(diagnostic_level::warning,
                        "duplicate `machine` modifier", file_id_)
                 .with_label(peek().span, "redundant"));
      }
      mods.is_machine = true;
      advance();
      break;
    case token_kind::kw_static:
      if (mods.is_static) {
        emit(diagnostic(diagnostic_level::warning,
                        "duplicate `static` modifier", file_id_)
                 .with_label(peek().span, "redundant"));
      }
      mods.is_static = true;
      advance();
      break;
    case token_kind::kw_intrinsic:
      if (mods.is_intrinsic) {
        emit(diagnostic(diagnostic_level::warning,
                        "duplicate `intrinsic` modifier", file_id_)
                 .with_label(peek().span, "redundant"));
      }
      mods.is_intrinsic = true;
      advance();
      break;
    case token_kind::kw_generator:
      if (mods.is_generator) {
        emit(diagnostic(diagnostic_level::warning,
                        "duplicate `generator` modifier", file_id_)
                 .with_label(peek().span, "redundant"));
      }
      mods.is_generator = true;
      advance();
      break;
    default:
      keep_going = false;
      break;
    }
  }

  return mods;
}

auto parser::parse_func_decl(ast::visibility vis, ast::func_modifiers mods,
                             bool allow_bodyless) -> ast::ptr<ast::func_decl> {
  auto decl = ast::make<ast::func_decl>();
  auto start = peek().span;
  decl->visibility = vis;
  decl->modifiers = std::move(mods);

  expect(token_kind::kw_def);
  token name_tok;
  if (at(token_kind::ident) || at(token_kind::kw_pure)) {
    name_tok = advance();
  } else {
    name_tok = expect(token_kind::ident);
  }
  decl->name = std::string(name_tok.text);

  // Optional type parameters.
  if (at(token_kind::lbracket)) {
    decl->type_params = parse_type_params();
  }

  // Parameter list.
  expect(token_kind::lparen);
  if (!at(token_kind::rparen)) {
    decl->params = parse_param_list();
  }
  expect(token_kind::rparen);

  // Optional return type.
  if (match(token_kind::arrow)) {
    decl->return_type = parse_type_expr();
  }

  // Optional where clause.
  if (at(token_kind::kw_where)) {
    decl->where_constraints = parse_where_clause();
  }

  // Optional contract clauses. They appear on following indented lines, so
  // only consume line breaks when a `pre`/`post` clause actually follows —
  // otherwise the newline stays put and terminates a bodyless declaration.
  {
    auto lookahead = uint32_t{0};
    while (peek_at(lookahead).kind == token_kind::newline ||
           peek_at(lookahead).kind == token_kind::indent) {
      ++lookahead;
    }
    if (peek_at(lookahead).kind == token_kind::kw_pre ||
        peek_at(lookahead).kind == token_kind::kw_post) {
      skip_newlines();
      decl->contracts = parse_contract_clauses();
    }
  }

  // `intrinsic def` is signature-only regardless of the surrounding
  // context's default — it names a native implementation, not a body.
  bool bodyless_ok = allow_bodyless || decl->modifiers.is_intrinsic;

  if (bodyless_ok &&
      at_any(token_kind::newline, token_kind::dedent, token_kind::eof)) {
    expect_newline();
    decl->span = start.merge(previous_span());
    return decl;
  }

  if (decl->modifiers.is_intrinsic) {
    emit(diagnostic(diagnostic_level::error,
                    "`intrinsic` function `" + decl->name +
                        "` cannot have a body",
                    file_id_)
             .with_label(peek().span,
                         "unexpected body — intrinsic functions are "
                         "implemented natively, per backend")
             .with_help("remove the body, or drop the `intrinsic` modifier "
                        "if this function has real Kira code"));
  }

  // Function body.
  auto body = parse_body("function");
  decl->body_expr = std::move(body.inline_expr);
  decl->body_stmts = std::move(body.stmts);

  decl->span = start.merge(previous_span());
  return decl;
}

auto parser::parse_param_list() -> std::vector<ast::param> {
  std::vector<ast::param> params;

  while (true) {
    if (at(token_kind::rparen)) {
      break;
    }
    params.push_back(parse_param());
    if (!match(token_kind::comma)) {
      break;
    }
  }

  return params;
}

auto parser::parse_param() -> ast::param {
  ast::param param;
  param.span = peek().span;

  param.pattern = parse_pattern();

  if (match(token_kind::colon)) {
    param.type_annotation = parse_type_expr();
  }

  if (match(token_kind::eq)) {
    param.default_value = parse_expr();
  }

  param.span.extend_to(previous_span());
  return param;
}

auto parser::parse_contract_clauses() -> std::vector<ast::contract_clause> {
  std::vector<ast::contract_clause> clauses;

  skip_newlines();
  while (at_any(token_kind::kw_pre, token_kind::kw_post)) {
    clauses.push_back(parse_contract_clause());
    skip_newlines();
  }

  return clauses;
}

auto parser::parse_contract_clause() -> ast::contract_clause {
  ast::contract_clause clause;
  clause.span = peek().span;

  if (at(token_kind::kw_pre)) {
    clause.is_pre = true;
    advance();
  } else {
    clause.is_pre = false;
    expect(token_kind::kw_post);
  }

  // `return` parses in *either* clause kind, even though only a postcondition
  // may legitimately name the returned value: `pre return > 0` is far better
  // served by the checker's explanation of why there is no returned value yet
  // on entry (`check_function`, semantic/check.cpp) than by a bare syntax
  // error here.
  contract_return_allowed_ = true;
  clause.condition = parse_expr();
  contract_return_allowed_ = false;

  if (match(token_kind::comma)) {
    auto msg_tok = expect(token_kind::string_lit);
    clause.message = std::string(msg_tok.text);
  }

  expect_newline();

  clause.span.extend_to(previous_span());
  return clause;
}

// ==========================================================================
//  Static declarations
// ==========================================================================

auto parser::parse_static_decl(ast::visibility vis)
    -> ast::ptr<ast::static_decl> {
  auto decl = ast::make<ast::static_decl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(token_kind::kw_static);

  // Disambiguate: static if, static assert, static for, static binding.
  if (at(token_kind::kw_if)) {
    // static if expr: block [else: block]
    decl->decl_kind = ast::static_decl_kind::conditional_compilation;
    advance(); // consume `if`
    decl->if_condition = parse_expr();
    expect(token_kind::colon);
    skip_newlines();

    if (match(token_kind::indent)) {
      while (!at_any(token_kind::dedent, token_kind::eof)) {
        skip_newlines();
        if (at_any(token_kind::dedent, token_kind::eof)) {
          break;
        }
        auto item = parse_top_level_item();
        if (item) {
          decl->if_body.push_back(std::move(item));
        } else {
          synchronize();
        }
      }
      expect_block_end("static if");
    }

    if (at(token_kind::kw_else)) {
      advance();
      expect(token_kind::colon);
      skip_newlines();

      if (match(token_kind::indent)) {
        while (!at_any(token_kind::dedent, token_kind::eof)) {
          skip_newlines();
          if (at_any(token_kind::dedent, token_kind::eof)) {
            break;
          }
          auto item = parse_top_level_item();
          if (item) {
            decl->else_body.push_back(std::move(item));
          } else {
            synchronize();
          }
        }
        expect_block_end("static else");
      }
    }
  } else if (at(token_kind::kw_assert)) {
    // static assert expr [, message]
    decl->decl_kind = ast::static_decl_kind::assertion;
    advance(); // consume `assert`
    decl->assert_condition = parse_expr();
    if (match(token_kind::comma)) {
      auto msg_tok = expect(token_kind::string_lit);
      decl->assert_message = std::string(msg_tok.text);
    }
    expect_newline();
  } else if (at(token_kind::kw_for)) {
    // static for vars in expr [if guard] => expr   OR
    // static for vars in expr [if guard]: block
    advance(); // consume `for`
    decl->for_patterns = parse_for_vars();
    expect_with_context(token_kind::kw_in, "in `static for ... in ...`");
    {
      auto saved_allow_lambda = allow_lambda_expr_;
      auto saved_allow_trailing_if = allow_trailing_if_expr_;
      allow_lambda_expr_ = false;
      allow_trailing_if_expr_ = false;
      decl->for_iterable = parse_expr();
      allow_lambda_expr_ = saved_allow_lambda;
      allow_trailing_if_expr_ = saved_allow_trailing_if;
    }

    if (match(token_kind::kw_if)) {
      auto saved_allow_lambda = allow_lambda_expr_;
      allow_lambda_expr_ = false;
      decl->for_guard = parse_expr();
      allow_lambda_expr_ = saved_allow_lambda;
    }

    if (at(token_kind::fat_arrow)) {
      // Inline form.
      decl->decl_kind = ast::static_decl_kind::for_inline;
      advance();
      decl->for_yield = parse_expr();
      expect_newline();
    } else {
      // Block form.
      decl->decl_kind = ast::static_decl_kind::for_block;
      expect(token_kind::colon);
      skip_newlines();
      if (match(token_kind::indent)) {
        while (!at_any(token_kind::dedent, token_kind::eof)) {
          skip_newlines();
          if (at_any(token_kind::dedent, token_kind::eof)) {
            break;
          }
          auto stmt = parse_stmt();
          if (stmt) {
            decl->for_body.push_back(std::move(stmt));
          } else {
            synchronize();
          }
        }
        expect_block_end("static for");
      }
    }
  } else {
    // static binding: static [let] Name [: Type] = expr
    decl->decl_kind = ast::static_decl_kind::binding;
    match(token_kind::kw_let);
    auto name_tok = expect(token_kind::ident);
    decl->name = std::string(name_tok.text);

    decl->type_annotation = parse_optional_type_annotation();

    expect(token_kind::eq);
    decl->initializer = parse_expr();
    expect_newline();
  }

  decl->span = start.merge(previous_span());
  return decl;
}

// ==========================================================================
//  Statements
// ==========================================================================

auto parser::parse_stmt() -> ast::ptr<ast::node> {
  skip_newlines();

  if (at_eof() || at(token_kind::dedent)) {
    return nullptr;
  }

  // Check for statements that start with a keyword.
  switch (current()) {
  case token_kind::kw_let:
    return parse_let_stmt();
  case token_kind::kw_var:
    return parse_var_stmt();
  case token_kind::kw_return:
    return parse_return_stmt();
  case token_kind::kw_break:
    return parse_break_stmt();
  case token_kind::kw_continue:
    return parse_continue_stmt();
  case token_kind::kw_if:
    return parse_if_stmt();
  case token_kind::kw_while:
    return parse_while_stmt();
  case token_kind::kw_for:
    return parse_for_stmt();
  case token_kind::kw_match:
    return parse_match_stmt();
  case token_kind::kw_crew:
    return parse_crew_stmt();
  case token_kind::kw_asm:
    return parse_asm_stmt();
  case token_kind::tilde:
    return parse_splice_stmt();

  case token_kind::kw_use:
    return parse_use_decl(ast::visibility::def);
  case token_kind::kw_type:
    return parse_type_decl(ast::visibility::def);
  case token_kind::kw_packed: {
    auto mods = parse_type_modifiers();
    if (at(token_kind::kw_type)) {
      return parse_type_decl(ast::visibility::def, mods);
    }
    emit_unexpected("`type` after type modifiers");
    synchronize_to_newline();
    return make_error_node(peek().span);
  }
  case token_kind::kw_static: {
    if (at_static_func_decl()) {
      auto mods = parse_func_modifiers();
      if (at(token_kind::kw_def)) {
        return parse_func_decl(ast::visibility::def, std::move(mods));
      }
    }
    return parse_static_decl(ast::visibility::def);
  }

  case token_kind::kw_pub:
  case token_kind::kw_internal:
  case token_kind::kw_super:
  case token_kind::kw_priv: {
    auto vis = parse_optional_visibility();
    if (at(token_kind::kw_def) || peek().is_func_modifier()) {
      auto mods = parse_func_modifiers();
      if (at(token_kind::kw_def)) {
        return parse_func_decl(vis, std::move(mods));
      }
    }
    if (at(token_kind::kw_type) || peek().is_type_modifier()) {
      auto mods = parse_type_modifiers();
      return parse_type_decl(vis, mods);
    }
    if (at(token_kind::kw_static)) {
      return parse_static_decl(vis);
    }
    if (at(token_kind::kw_use)) {
      return parse_use_decl(vis);
    }
    emit_unexpected("a declaration after visibility modifier");
    synchronize_to_newline();
    return make_error_node(peek().span);
  }

  case token_kind::kw_def:
    return parse_func_decl(ast::visibility::def, ast::func_modifiers{});

  case token_kind::kw_pure:
  case token_kind::kw_async:
  case token_kind::kw_machine:
  case token_kind::kw_generator: {
    auto mods = parse_func_modifiers();
    if (at(token_kind::kw_def)) {
      return parse_func_decl(ast::visibility::def, std::move(mods));
    }
    emit_unexpected("`def` after function modifiers");
    synchronize_to_newline();
    return make_error_node(peek().span);
  }

  default:
    // expression or assignment statement.
    return parse_expr_or_assign_stmt();
  }
}

auto parser::parse_let_stmt() -> ast::ptr<ast::let_stmt> {
  auto stmt = ast::make<ast::let_stmt>();
  auto start = peek().span;

  expect(token_kind::kw_let);
  stmt->pattern = parse_pattern();

  stmt->type_annotation = parse_optional_type_annotation();

  expect(token_kind::eq);
  stmt->initializer = parse_expr();

  // Check for `else:` block.
  if (at(token_kind::kw_else)) {
    advance();
    expect(token_kind::colon);
    skip_newlines();

    if (match(token_kind::indent)) {
      while (!at_any(token_kind::dedent, token_kind::eof)) {
        skip_newlines();
        if (at_any(token_kind::dedent, token_kind::eof)) {
          break;
        }
        auto s = parse_stmt();
        if (s) {
          stmt->else_body.push_back(std::move(s));
        } else {
          synchronize();
        }
      }
      expect_block_end("let-else");
    }
  } else {
    expect_newline();
  }

  stmt->span = start.merge(previous_span());
  return stmt;
}

auto parser::parse_var_stmt() -> ast::ptr<ast::var_stmt> {
  auto stmt = ast::make<ast::var_stmt>();
  auto start = peek().span;

  expect(token_kind::kw_var);
  auto name_tok = expect(token_kind::ident);
  stmt->name = std::string(name_tok.text);

  if (match(token_kind::colon)) {
    stmt->type_annotation = parse_type_expr();
  }

  expect(token_kind::eq);
  stmt->initializer = parse_expr();
  expect_newline();

  stmt->span = start.merge(previous_span());
  return stmt;
}

auto parser::parse_return_stmt() -> ast::ptr<ast::return_stmt> {
  auto stmt = ast::make<ast::return_stmt>();
  auto start = peek().span;

  expect(token_kind::kw_return);

  // Return value is optional.
  if (!at_any(token_kind::newline, token_kind::dedent, token_kind::eof)) {
    stmt->value = parse_expr();
  }

  expect_newline();

  stmt->span = start.merge(previous_span());
  return stmt;
}

auto parser::parse_break_stmt() -> ast::ptr<ast::break_stmt> {
  auto stmt = ast::make<ast::break_stmt>();
  auto start = peek().span;
  expect(token_kind::kw_break);
  expect_newline();
  stmt->span = start.merge(previous_span());
  return stmt;
}

auto parser::parse_continue_stmt() -> ast::ptr<ast::continue_stmt> {
  auto stmt = ast::make<ast::continue_stmt>();
  auto start = peek().span;
  expect(token_kind::kw_continue);
  expect_newline();
  stmt->span = start.merge(previous_span());
  return stmt;
}

auto parser::parse_if_stmt() -> ast::ptr<ast::if_stmt> {
  auto stmt = ast::make<ast::if_stmt>();
  auto start = peek().span;

  auto parse_if_condition = [this](ast::if_branch &branch) -> void {
    if (match(token_kind::kw_let)) {
      branch.let_pattern = parse_pattern();
      expect(token_kind::eq);
      branch.let_expr = parse_expr();
      branch.condition = make_error_expr(previous_span(), "if let condition");
    } else {
      branch.condition = parse_expr();
    }
  };

  // First `if` branch.
  expect(token_kind::kw_if);
  {
    ast::if_branch branch;
    branch.span = start;
    parse_if_condition(branch);
    branch.body = body_to_stmt_list(parse_body("if"));
    branch.span.extend_to(previous_span());
    stmt->branches.push_back(std::move(branch));
  }

  // `elif` branches.
  while (at(token_kind::kw_elif)) {
    advance();
    ast::if_branch branch;
    branch.span = previous_span();
    parse_if_condition(branch);
    branch.body = body_to_stmt_list(parse_body("elif"));
    branch.span.extend_to(previous_span());
    stmt->branches.push_back(std::move(branch));
  }

  // Optional `else` branch.
  if (at(token_kind::kw_else)) {
    advance();
    stmt->else_body = body_to_stmt_list(parse_body("else"));
  }

  stmt->span = start.merge(previous_span());
  return stmt;
}

auto parser::parse_while_stmt() -> ast::ptr<ast::while_stmt> {
  auto stmt = ast::make<ast::while_stmt>();
  auto start = peek().span;

  expect(token_kind::kw_while);

  if (at(token_kind::kw_let)) {
    // while let pattern = expr: block
    advance();
    stmt->let_pattern = parse_pattern();
    expect(token_kind::eq);
    stmt->let_expr = parse_expr();
  } else {
    stmt->condition = parse_expr();
  }

  stmt->body = body_to_stmt_list(parse_body("while"));

  stmt->span = start.merge(previous_span());
  return stmt;
}

auto parser::parse_for_stmt() -> ast::ptr<ast::for_stmt> {
  auto stmt = ast::make<ast::for_stmt>();
  auto start = peek().span;

  expect(token_kind::kw_for);
  stmt->patterns = parse_for_vars();
  expect_with_context(token_kind::kw_in, "in `for ... in ...`");
  {
    auto saved_allow_lambda = allow_lambda_expr_;
    auto saved_allow_trailing_if = allow_trailing_if_expr_;
    allow_lambda_expr_ = false;
    allow_trailing_if_expr_ = false;
    stmt->iterable = parse_expr();
    allow_lambda_expr_ = saved_allow_lambda;
    allow_trailing_if_expr_ = saved_allow_trailing_if;
  }

  if (match(token_kind::kw_if)) {
    stmt->guard = parse_expr();
  }

  stmt->body = body_to_stmt_list(parse_body("for"));

  stmt->span = start.merge(previous_span());
  return stmt;
}

auto parser::parse_match_stmt() -> ast::ptr<ast::match_stmt> {
  auto stmt = ast::make<ast::match_stmt>();
  auto start = peek().span;

  expect(token_kind::kw_match);
  stmt->subject = parse_expr();
  expect(token_kind::colon);
  skip_newlines();

  if (match(token_kind::indent)) {
    while (!at_any(token_kind::dedent, token_kind::eof)) {
      skip_newlines();
      if (at_any(token_kind::dedent, token_kind::eof)) {
        break;
      }
      stmt->arms.push_back(parse_match_arm());
    }
    expect_block_end("match");
  }

  stmt->span = start.merge(previous_span());
  return stmt;
}

auto parser::parse_crew_stmt() -> ast::ptr<ast::crew_stmt> {
  auto stmt = ast::make<ast::crew_stmt>();
  auto start = peek().span;

  expect(token_kind::kw_crew);
  auto name_tok = expect(token_kind::ident);
  stmt->name = std::string(name_tok.text);

  if (match(token_kind::lparen)) {
    while (!at(token_kind::rparen) && !at_eof()) {
      ast::crew_option opt;
      opt.span = peek().span;
      auto key_tok = expect(token_kind::ident);
      opt.name = std::string(key_tok.text);
      expect(token_kind::colon);
      auto val_tok = expect(token_kind::ident);
      opt.value = std::string(val_tok.text);
      stmt->options.push_back(std::move(opt));
      if (!match(token_kind::comma)) {
        break;
      }
    }
    expect(token_kind::rparen);
  }

  stmt->body = body_to_stmt_list(parse_body("crew"));

  stmt->span = start.merge(previous_span());
  return stmt;
}

auto parser::parse_asm_stmt() -> ast::ptr<ast::asm_stmt> {
  auto stmt = ast::make<ast::asm_stmt>();
  auto start = peek().span;

  expect(token_kind::kw_asm);
  expect(token_kind::lbrace);

  // Consume everything until matching `}`.
  int depth = 1;
  while (!at_eof() && depth > 0) {
    if (at(token_kind::lbrace)) {
      ++depth;
    } else if (at(token_kind::rbrace)) {
      --depth;
    }
    if (depth > 0) {
      advance();
    }
  }

  // Collect the text between the braces.
  // For now, just record that we have ASM content.
  stmt->content = "/* asm content */";
  expect(token_kind::rbrace);
  expect_newline();

  stmt->span = start.merge(previous_span());
  return stmt;
}

auto parser::parse_splice_stmt() -> ast::ptr<ast::splice_stmt> {
  auto stmt = ast::make<ast::splice_stmt>();
  auto start = peek().span;

  expect(token_kind::tilde);
  stmt->expr = parse_expr();
  expect_newline();

  stmt->span = start.merge(previous_span());
  return stmt;
}

auto parser::parse_expr_or_assign_stmt() -> ast::ptr<ast::node> {
  auto start = peek().span;

  auto expr = parse_expr();
  if (!expr) {
    emit_unexpected("a statement");
    synchronize_to_newline();
    return make_error_node(start);
  }

  // Check for assignment operator.
  auto assign_op = token_to_assign_op(current());
  if (assign_op) {
    advance(); // consume the assign op

    auto assign = ast::make<ast::assign_stmt>();
    assign->target = std::move(expr);
    assign->op = *assign_op;
    assign->value = parse_expr();
    expect_newline();
    assign->span = start.merge(previous_span());
    return assign;
  }

  // Plain expression statement.
  auto es = ast::make<ast::expr_stmt>();
  es->expr = std::move(expr);
  expect_newline();
  es->span = start.merge(previous_span());
  return es;
}

// ==========================================================================
//  Expressions
// ==========================================================================

/// parse_expr is the top-level expression entry point. It parses a
/// pipe_expr and then checks whether a `where:` clause follows. If it
/// does, the expression is wrapped in a where_expr node whose bindings
/// bring locally-scoped immutable names into scope for the inner expression.
///
/// Grammar:
///   expr = pipe_expr [ "where" ":" NEWLINE INDENT { where_binding } DEDENT ]
///   where_binding = IDENT "=" expr NEWLINE
auto parser::parse_expr() -> ast::ptr<ast::expr> {
  auto inner = parse_pipe_expr();
  if (!inner) {
    return nullptr;
  }

  if (allow_trailing_if_expr_ && at(token_kind::kw_if)) {
    inner = parse_trailing_if_expr(std::move(inner));
  }

  // Optional trailing `where:` clause.
  if (at(token_kind::kw_where)) {
    return parse_where_expr(std::move(inner));
  }

  return inner;
}

auto parser::parse_pipe_expr() -> ast::ptr<ast::expr> {
  auto lhs = parse_or_expr();
  if (!lhs) {
    return nullptr;
  }

  // The `|` token is overloaded (pipe vs union type vs or-pattern).
  // In expression context, `|` after an expression is the pipe operator.
  while (at(token_kind::pipe)) {
    auto op_tok = advance();
    auto rhs = parse_or_expr();
    if (!rhs) {
      emit_unexpected("an expression after `|`");
      rhs = make_error_expr(op_tok.span);
    }

    auto bin = ast::make<ast::binary_expr>();
    bin->span = lhs->span.merge(rhs->span);
    bin->op = ast::binary_op::pipe;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    lhs = std::move(bin);
  }

  return lhs;
}

auto parser::parse_or_expr() -> ast::ptr<ast::expr> {
  auto lhs = parse_and_expr();
  if (!lhs) {
    return nullptr;
  }

  while (at(token_kind::kw_or)) {
    advance();
    auto rhs = parse_and_expr();
    if (!rhs) {
      emit_unexpected("an expression after `or`");
      rhs = make_error_expr(previous_span());
    }

    auto bin = ast::make<ast::binary_expr>();
    bin->span = lhs->span.merge(rhs->span);
    bin->op = ast::binary_op::logical_or;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    lhs = std::move(bin);
  }

  return lhs;
}

auto parser::parse_and_expr() -> ast::ptr<ast::expr> {
  auto lhs = parse_not_expr();
  if (!lhs) {
    return nullptr;
  }

  while (at(token_kind::kw_and)) {
    advance();
    auto rhs = parse_not_expr();
    if (!rhs) {
      emit_unexpected("an expression after `and`");
      rhs = make_error_expr(previous_span());
    }

    auto bin = ast::make<ast::binary_expr>();
    bin->span = lhs->span.merge(rhs->span);
    bin->op = ast::binary_op::logical_and;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    lhs = std::move(bin);
  }

  return lhs;
}

auto parser::parse_not_expr() -> ast::ptr<ast::expr> {
  if (at(token_kind::kw_not)) {
    auto op_tok = advance();
    auto operand = parse_not_expr();
    if (!operand) {
      emit_unexpected("an expression after `not`");
      operand = make_error_expr(op_tok.span);
    }

    auto unary = ast::make<ast::unary_expr>();
    unary->span = op_tok.span.merge(operand->span);
    unary->op = ast::unary_op::logical_not;
    unary->operand = std::move(operand);
    return unary;
  }

  return parse_cmp_expr();
}

auto parser::parse_cmp_expr() -> ast::ptr<ast::expr> {
  auto lhs = parse_add_expr();
  if (!lhs) {
    return nullptr;
  }

  if (at_any(token_kind::dot_dot, token_kind::dot_dot_eq)) {
    auto inclusive = at(token_kind::dot_dot_eq);
    advance();

    auto range = ast::make<ast::binary_expr>();
    range->span = lhs->span;
    range->op =
        inclusive ? ast::binary_op::range_inclusive : ast::binary_op::range;
    range->lhs = std::move(lhs);

    if (!at_any(token_kind::newline, token_kind::dedent, token_kind::eof,
                token_kind::comma, token_kind::colon, token_kind::kw_if,
                token_kind::fat_arrow, token_kind::rparen, token_kind::rbracket,
                token_kind::rbrace)) {
      range->rhs = parse_add_expr();
    }

    range->span.extend_to(previous_span());
    lhs = std::move(range);
  }

  auto maybe_op = token_to_cmp_op();
  if (maybe_op) {
    auto op_tok = advance();
    auto op = *maybe_op;

    // Handle `not in` (two-token operator).
    if (op_tok.kind == token_kind::kw_not) {
      if (at(token_kind::kw_in)) {
        advance(); // consume `in`
        op = ast::binary_op::not_in;
      } else {
        // Just `not` in comparison position — this is odd.
        // The `not` was already consumed as a cmp_op candidate.
        // Let's treat it as `not in` and emit an error.
        emit(diagnostic(diagnostic_level::error,
                        "expected `in` after `not` in comparison — "
                        "did you mean `not in`?",
                        file_id_)
                 .with_label(op_tok.span, "this `not`")
                 .with_help("The comparison operator is `not in`, "
                            "not bare `not` in this position."));
        op = ast::binary_op::not_in;
      }
    }

    auto rhs = parse_add_expr();
    if (!rhs) {
      emit_unexpected("an expression after comparison operator");
      rhs = make_error_expr(op_tok.span);
    }

    auto bin = ast::make<ast::binary_expr>();
    bin->span = lhs->span.merge(rhs->span);
    bin->op = op;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    lhs = std::move(bin);
  }

  return lhs;
}

auto parser::parse_add_expr() -> ast::ptr<ast::expr> {
  auto lhs = parse_mul_expr();
  if (!lhs) {
    return nullptr;
  }

  while (true) {
    if (at(token_kind::amp)) {
      auto op_tok = advance();
      auto rhs = parse_mul_expr();
      if (!rhs) {
        emit_unexpected("an expression after `&`");
        rhs = make_error_expr(op_tok.span);
      }

      auto bin = ast::make<ast::binary_expr>();
      bin->span = lhs->span.merge(rhs->span);
      bin->op = ast::binary_op::bit_and;
      bin->lhs = std::move(lhs);
      bin->rhs = std::move(rhs);
      lhs = std::move(bin);
      continue;
    }
    if (at(token_kind::caret)) {
      auto op_tok = advance();
      auto rhs = parse_mul_expr();
      if (!rhs) {
        emit_unexpected("an expression after `^`");
        rhs = make_error_expr(op_tok.span);
      }

      auto bin = ast::make<ast::binary_expr>();
      bin->span = lhs->span.merge(rhs->span);
      bin->op = ast::binary_op::bit_xor;
      bin->lhs = std::move(lhs);
      bin->rhs = std::move(rhs);
      lhs = std::move(bin);
      continue;
    }
    if (at(token_kind::lt_lt) || at(token_kind::gt_gt)) {
      auto op_tok = advance();
      auto rhs = parse_mul_expr();
      if (!rhs) {
        emit_unexpected("an expression after shift operator");
        rhs = make_error_expr(op_tok.span);
      }

      auto bin = ast::make<ast::binary_expr>();
      bin->span = lhs->span.merge(rhs->span);
      bin->op = op_tok.kind == token_kind::lt_lt ? ast::binary_op::shl
                                                 : ast::binary_op::shr;
      bin->lhs = std::move(lhs);
      bin->rhs = std::move(rhs);
      lhs = std::move(bin);
      continue;
    }

    auto maybe_op = token_to_add_op(current());
    if (!maybe_op) {
      break;
    }

    advance();
    auto op = *maybe_op;

    auto rhs = parse_mul_expr();
    if (!rhs) {
      emit_unexpected("an expression after arithmetic operator");
      rhs = make_error_expr(previous_span());
    }

    auto bin = ast::make<ast::binary_expr>();
    bin->span = lhs->span.merge(rhs->span);
    bin->op = op;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    lhs = std::move(bin);
  }

  return lhs;
}

auto parser::parse_mul_expr() -> ast::ptr<ast::expr> {
  auto lhs = parse_unary_expr();
  if (!lhs) {
    return nullptr;
  }

  while (true) {
    auto maybe_op = token_to_mul_op(current());
    if (!maybe_op) {
      break;
    }

    advance();
    auto op = *maybe_op;

    auto rhs = parse_unary_expr();
    if (!rhs) {
      emit_unexpected("an expression after arithmetic operator");
      rhs = make_error_expr(previous_span());
    }

    auto bin = ast::make<ast::binary_expr>();
    bin->span = lhs->span.merge(rhs->span);
    bin->op = op;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    lhs = std::move(bin);
  }

  return lhs;
}

auto parser::parse_unary_expr() -> ast::ptr<ast::expr> {
  switch (current()) {
  case token_kind::minus: {
    auto op_tok = advance();
    auto operand = parse_unary_expr();
    if (!operand) {
      emit_unexpected("an expression after `-`");
      operand = make_error_expr(op_tok.span);
    }
    auto unary = ast::make<ast::unary_expr>();
    unary->span = op_tok.span.merge(operand->span);
    unary->op = ast::unary_op::neg;
    unary->operand = std::move(operand);
    return unary;
  }

  case token_kind::tilde: {
    // At expression depth 0, `~(expr)` is a splice, `~anything_else`
    // is bitwise complement. We check for `(` to disambiguate.
    if (peek_at(1).is(token_kind::lparen)) {
      return parse_splice_expr_inner();
    }
    if (peek_at(1).can_start_expr()) {
      // Could also be a splice: `~name`
      return parse_splice_expr_inner();
    }
    auto op_tok = advance();
    auto operand = parse_unary_expr();
    if (!operand) {
      emit_unexpected("an expression after `~`");
      operand = make_error_expr(op_tok.span);
    }
    auto unary = ast::make<ast::unary_expr>();
    unary->span = op_tok.span.merge(operand->span);
    unary->op = ast::unary_op::bit_not;
    unary->operand = std::move(operand);
    return unary;
  }

  case token_kind::star: {
    auto op_tok = advance();
    auto operand = parse_unary_expr();
    if (!operand) {
      emit_unexpected("an expression after `*`");
      operand = make_error_expr(op_tok.span);
    }
    auto unary = ast::make<ast::unary_expr>();
    unary->span = op_tok.span.merge(operand->span);
    unary->op = ast::unary_op::deref;
    unary->operand = std::move(operand);
    return unary;
  }

  case token_kind::amp: {
    auto op_tok = advance();
    bool is_mut = match(token_kind::kw_mut);
    auto operand = parse_unary_expr();
    if (!operand) {
      emit_unexpected("an expression after `&`");
      operand = make_error_expr(op_tok.span);
    }
    auto unary = ast::make<ast::unary_expr>();
    unary->span = op_tok.span.merge(operand->span);
    unary->op = is_mut ? ast::unary_op::addr_of_mut : ast::unary_op::addr_of;
    unary->operand = std::move(operand);
    return unary;
  }

  default:
    return parse_postfix_expr();
  }
}

auto parser::parse_postfix_expr() -> ast::ptr<ast::expr> {
  auto base = parse_primary_expr();
  if (!base) {
    return nullptr;
  }

  while (true) {
    // Check if the current token can start a postfix suffix before
    // moving base into parse_postfix_suffix. This avoids losing
    // ownership of base when there's no suffix to parse.
    if (!at_any(token_kind::dot, token_kind::lbracket, token_kind::lparen,
                token_kind::question, token_kind::kw_as)) {
      break;
    }

    if (at(token_kind::lbracket) &&
        !(peek_at(1).can_start_expr() && !peek_at(1).is(token_kind::rbracket) &&
          !peek_at(2).is(token_kind::colon))) {
      break;
    }

    auto suffix = parse_postfix_suffix(std::move(base));
    if (!suffix) {
      // This shouldn't happen given the guard above, but be safe.
      return make_error_expr(previous_span(), "postfix expression");
    }
    base = std::move(suffix);
  }

  return base;
}

auto parser::parse_postfix_suffix(ast::ptr<ast::expr> base)
    -> ast::ptr<ast::expr> {
  switch (current()) {
  case token_kind::dot: {
    advance(); // consume `.`
    // `kw_pure` is accepted as a field/method name because `pure` doubles
    // as a declarable method name (`def pure` — see parse_func_decl's name
    // handling and the spec's `monad` trait, whose lifting method is
    // canonically called `pure`).
    // `t.0` — positional access into a tuple. The index is carried as the
    // `field_name` text rather than in a dedicated AST node: a tuple's
    // element is exactly a field selected by position instead of by name,
    // and everything downstream of the parser (`infer_field`, lowering to
    // `hir_tuple_index`) has to branch on the *object's* type anyway.
    if (at(token_kind::int_lit)) {
      auto index_tok = advance();
      auto field = ast::make<ast::field_expr>();
      field->span = base->span.merge(index_tok.span);
      field->object = std::move(base);
      field->field_name = std::string(index_tok.text);
      return field;
    }
    // `q.0.1` — a nested tuple access, which the lexer has already
    // committed to reading as the single float literal `0.1`, since it
    // scans a number without knowing a `.` preceded it. Rather than
    // complicate the lexer with parser context, split the token back into
    // the two positions it spells. Only a plain `digits.digits` float
    // qualifies: anything carrying an exponent, a suffix, or a `_`
    // separator was written as a number and is reported as one.
    if (at(token_kind::float_lit)) {
      const auto text = peek().text;
      const auto dot = text.find('.');
      const auto digits_only = [](std::string_view s) -> bool {
        return !s.empty() && std::ranges::all_of(s, [](char c) -> bool {
          return c >= '0' && c <= '9';
        });
      };
      if (dot != std::string_view::npos && digits_only(text.substr(0, dot)) &&
          digits_only(text.substr(dot + 1))) {
        auto index_tok = advance();
        const auto split = index_tok.span.start + static_cast<uint32_t>(dot);
        auto outer = ast::make<ast::field_expr>();
        outer->span = base->span.merge(
            source_span{.start = index_tok.span.start, .end = split});
        outer->object = std::move(base);
        outer->field_name = std::string(text.substr(0, dot));

        auto inner = ast::make<ast::field_expr>();
        inner->span = outer->span.merge(
            source_span{.start = split + 1, .end = index_tok.span.end});
        inner->object = std::move(outer);
        inner->field_name = std::string(text.substr(dot + 1));
        return inner;
      }
    }
    if (at(token_kind::ident) || at(token_kind::kw_pure)) {
      auto field_tok = advance();
      auto field = ast::make<ast::field_expr>();
      field->span = base->span.merge(field_tok.span);
      field->object = std::move(base);
      field->field_name = std::string(field_tok.text);

      // `obj.field[...]` is ambiguous: explicit generic method arguments
      // (`p.echo[int32](n)`) or indexing a field (`h.xs[0]`). A bare name
      // inside the brackets could be either a type or a value, so no rule
      // over the bracket contents alone can separate them.
      //
      // What does separate them is the token *after* the group. Generic
      // arguments name the method being called, so a call always follows;
      // an index stands on its own. So `.name[...](` is type arguments and
      // `.name[...]` anything else is an index, which is left for the
      // `lbracket` case below to build on the field this returns.
      //
      // This bracket used to be consumed as type arguments unconditionally,
      // which made indexing a field unparseable — `h.xs[0]` reported
      // ``expected `]` but found integer literal`` and `h.xs[h.at]`
      // reported ``qualified type path `h.at` does not resolve``, since an
      // integer and a field access are not types. Every iterator in
      // `std.iter` reads `self.src[self.at]`, so this was load-bearing.
      //
      // The residual case the rule gets wrong is calling a function stored
      // in a field-held container — `h.fns[0](x)` reads as type arguments.
      // That is narrower than what it replaces, and it is diagnosable:
      // the brackets reach `check_method_accepts_generic_args`, which
      // reports a method that has nothing for them to name.
      if (at(token_kind::lbracket) && bracket_group_precedes_call()) {
        advance();
        while (!at(token_kind::rbracket) && !at_eof()) {
          auto arg = parse_type_expr();
          if (arg) {
            field->generic_args.push_back(std::move(arg));
          }
          if (!match(token_kind::comma)) {
            break;
          }
        }
        expect(token_kind::rbracket);
        field->span.extend_to(previous_span());
      }

      return field;
    } else if (at(token_kind::lparen)) {
      // `.()` — field via parens (unusual but grammatical).
      advance();
      expect(token_kind::rparen);
      auto field = ast::make<ast::field_expr>();
      field->span = base->span.merge(previous_span());
      field->object = std::move(base);
      return field;
    } else {
      emit_unexpected("a field name after `.`");
      return base;
    }
  }

  case token_kind::lbracket: {
    // `name[...]` is ambiguous at parse time: it spells both real indexing
    // (`values[0]`) and explicit generic instantiation (`identity[int32]`,
    // `channel[str]`). Real indexing always takes exactly one unnamed key,
    // so a single unnamed bracket argument is parsed as an `index_expr` and
    // left to the semantic checker's `infer_index`/`ident_names_callable_decl`,
    // which knows whether the base names a value or a function/type.
    // Multiple or named bracket arguments can only be generic instantiation,
    // since indexing never carries argument names or commas.
    if (base->kind == ast::node_kind::ident_expr ||
        base->kind == ast::node_kind::field_expr ||
        base->kind == ast::node_kind::module_path_expr) {
      const auto start_span = base->span;

      advance(); // consume `[`
      auto args = std::vector<ast::call_arg>{};
      while (!at(token_kind::rbracket) && !at_eof()) {
        ast::call_arg arg;
        arg.span = peek().span;

        if (at(token_kind::ident) && peek_at(1).is(token_kind::colon)) {
          auto name_tok = advance();
          advance(); // consume `:`
          arg.name = std::string(name_tok.text);
          arg.value = parse_expr();
        } else {
          arg.value = parse_expr();
        }

        if (arg.value) {
          arg.span.extend_to(arg.value->span);
          args.push_back(std::move(arg));
        }

        if (!match(token_kind::comma)) {
          break;
        }
      }
      expect(token_kind::rbracket);

      if (args.size() == 1 && !args.front().name.has_value()) {
        auto idx = ast::make<ast::index_expr>();
        idx->span = start_span.merge(previous_span());
        idx->object = std::move(base);
        idx->index = std::move(args.front().value);
        return idx;
      }

      auto call = ast::make<ast::call_expr>();
      call->span = start_span;
      call->callee = std::move(base);
      call->args = std::move(args);
      call->span.extend_to(previous_span());
      return call;
    }

    advance(); // consume `[`
    auto index = parse_expr();
    expect(token_kind::rbracket);

    auto idx = ast::make<ast::index_expr>();
    idx->span = base->span.merge(previous_span());
    idx->object = std::move(base);
    idx->index = std::move(index);
    return idx;
  }

  case token_kind::lparen: {
    auto call = ast::make<ast::call_expr>();
    call->span = base->span;
    call->callee = std::move(base);

    advance(); // consume `(`
    if (!at(token_kind::rparen)) {
      call->args = parse_call_args();
    }
    expect(token_kind::rparen);

    call->span.extend_to(previous_span());
    return call;
  }

  case token_kind::question: {
    auto q_tok = advance();
    auto try_expr = ast::make<ast::try_expr>();
    try_expr->span = base->span.merge(q_tok.span);
    try_expr->operand = std::move(base);
    return try_expr;
  }

  case token_kind::kw_as: {
    advance();
    auto target_type = parse_type_expr();
    auto cast = ast::make<ast::cast_expr>();
    cast->span = base->span.merge(previous_span());
    cast->operand = std::move(base);
    cast->target_type = std::move(target_type);
    return cast;
  }

  default:
    // No postfix suffix — return nullptr to signal "done".
    return nullptr;
  }
}

auto parser::parse_primary_expr() -> ast::ptr<ast::expr> {
  // Inside a postcondition, `return` is not the statement — it *names* the
  // value the function returns (spec/kira-reference.md, "Contracts": "It is
  // deliberately not called `result`, which would collide with the `result`
  // type"). It parses as an ordinary identifier spelled `return`, which no
  // other production can produce (`return` is a keyword everywhere else), so
  // the checker and HIR lowering can resolve it by name like any other
  // binding instead of carrying a node kind that exists for one construct.
  if (contract_return_allowed_ && at(token_kind::kw_return)) {
    auto ident = ast::make<ast::ident_expr>();
    ident->span = peek().span;
    ident->name = "return";
    advance();
    return ident;
  }

  switch (current()) {
  // Literals.
  case token_kind::int_lit:
  case token_kind::float_lit:
  case token_kind::char_lit:
  case token_kind::kw_true:
  case token_kind::kw_false:
  case token_kind::kw_unit:
    return parse_literal_expr();

  case token_kind::string_lit:
    return parse_string_literal_expr();

  // Identifiers and module paths.
  case token_kind::ident:
  case token_kind::kw_some:
  case token_kind::kw_ok:
  case token_kind::kw_err:
  case token_kind::kw_super:
    return parse_ident_or_path_expr();

  // Variant constructor: `@name`, `@name(args)`.
  case token_kind::at:
    return parse_variant_expr();

  // Parenthesized expr, tuple, or grouped -- unless the parens turn out to
  // be a lambda's parameter list, which only what follows the `)` reveals.
  case token_kind::lparen:
    if (at_lambda_param_list()) {
      return parse_lambda_expr();
    }
    return parse_paren_expr();

  // Array literal or fill.
  case token_kind::lbracket:
    return parse_bracket_expr();

  // Struct literal.
  case token_kind::lbrace:
    return parse_brace_expr();

  // Lambda expressions.
  case token_kind::kw_pure:
  case token_kind::kw_move:
    return parse_lambda_expr();

  // Match expression.
  case token_kind::kw_match:
    return parse_match_expr();

  // If expression.
  case token_kind::kw_if:
    return parse_if_expr();

  // For comprehension.
  case token_kind::kw_for:
    return parse_for_expr();

  // Await expression.
  case token_kind::kw_await:
    return parse_await_expr();

  // Value-carrying yield (generator suspension point).
  case token_kind::kw_yield:
    return parse_yield_expr();

  case token_kind::kw_async:
    return parse_async_expr();

  // Par expression.
  case token_kind::kw_par:
    return parse_par_expr();

  // Race expression.
  case token_kind::kw_race:
    return parse_race_expr();

  // On expression.
  case token_kind::kw_on:
    return parse_on_expr();

  case token_kind::kw_crew:
    return parse_crew_expr();

  // Quote expression.
  case token_kind::backtick:
    return parse_quote_expr();

  // Static expression.
  case token_kind::kw_static:
    return parse_static_expr();

  case token_kind::kw_shared: {
    auto tok = advance();
    auto shared = ast::make<ast::unary_expr>();
    shared->span = tok.span;
    shared->op = ast::unary_op::addr_of;
    shared->operand = parse_expr();
    if (shared->operand) {
      shared->span.extend_to(shared->operand->span);
    }
    return shared;
  }

  default:
    return nullptr;
  }
}

auto parser::parse_literal_expr() -> ast::ptr<ast::expr> {
  auto tok = advance();
  auto lit = ast::make<ast::literal_expr>();
  lit->span = tok.span;
  lit->lit_kind = tok.kind;
  lit->value = std::string(tok.text);
  return lit;
}

auto parser::parse_sub_expr(std::string_view text, byte_offset absolute_offset)
    -> ast::ptr<ast::expr> {
  // Trim surrounding whitespace so the sub-lexer's leading-line indentation
  // tracking (which measures indentation from column 0 of a logical line)
  // never mistakes leading spaces inside `{...}` for an INDENT.
  size_t start = 0;
  while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) {
    ++start;
  }
  size_t end = text.size();
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
    --end;
  }
  const auto trimmed = text.substr(start, end - start);
  if (trimmed.empty()) {
    return make_error_expr(
        source_span{.start = absolute_offset,
                    .end = absolute_offset +
                           static_cast<byte_offset>(text.size())},
        "expected an expression here");
  }

  auto sub_lexer = lexer(trimmed, file_id_, diag_,
                         absolute_offset + static_cast<byte_offset>(start));
  auto sub_tokens = sub_lexer.tokenize();
  auto sub_parser = parser(std::move(sub_tokens), file_id_, diag_);
  return sub_parser.parse_expr();
}

auto parser::parse_format_spec_text(std::string_view text,
                                    byte_offset absolute_offset,
                                    source_span whole_span)
    -> ast::format_spec {
  auto spec = ast::format_spec{};
  spec.span = whole_span;
  size_t pos = 0;
  const size_t len = text.size();

  const auto is_type_char = [](char c) -> bool {
    return c == 's' || c == '?' || c == 'd' || c == 'x' || c == 'X' ||
           c == 'o' || c == 'b' || c == 'e' || c == 'E' || c == 'f' ||
           c == 'g' || c == 'G' || c == 'c';
  };
  const auto align_of = [](char c) -> ast::format_align {
    return c == '<'   ? ast::format_align::left
           : c == '>' ? ast::format_align::right
                      : ast::format_align::center;
  };

  if (pos + 1 < len &&
      (text[pos + 1] == '<' || text[pos + 1] == '>' || text[pos + 1] == '^')) {
    spec.fill = text[pos];
    spec.align = align_of(text[pos + 1]);
    spec.has_explicit_align = true;
    pos += 2;
  } else if (pos < len &&
             (text[pos] == '<' || text[pos] == '>' || text[pos] == '^')) {
    spec.align = align_of(text[pos]);
    spec.has_explicit_align = true;
    pos += 1;
  }

  if (pos < len && (text[pos] == '+' || text[pos] == '-' || text[pos] == ' ')) {
    spec.sign = text[pos] == '+'   ? ast::format_sign::always
                : text[pos] == '-' ? ast::format_sign::negative_only
                                   : ast::format_sign::space;
    pos += 1;
  }

  if (pos < len && text[pos] == '#') {
    spec.alternate = true;
    pos += 1;
  }

  if (pos < len && text[pos] == '0') {
    spec.zero_pad = true;
    pos += 1;
  }

  const auto parse_width_or_precision =
      [&](std::variant<std::monostate, size_t, ast::ptr<ast::expr>> &out)
      -> void {
    if (pos < len && text[pos] == '{') {
      const auto close = find_matching_brace(text, pos);
      if (!close.has_value()) {
        emit(diagnostic(diagnostic_level::error,
                        "unterminated dynamic width/precision in "
                        "format spec",
                        file_id_)
                 .with_label(whole_span, "the `{` here is never closed")
                 .with_help("Close the dynamic width/precision with a "
                            "matching `}`."));
        pos = len;
        return;
      }
      const auto inner = text.substr(pos + 1, *close - pos - 1);
      out = parse_sub_expr(inner,
                           absolute_offset + static_cast<byte_offset>(pos + 1));
      pos = *close + 1;
      return;
    }
    const size_t digits_start = pos;
    while (pos < len && text[pos] >= '0' && text[pos] <= '9') {
      ++pos;
    }
    if (pos > digits_start) {
      size_t value = 0;
      for (size_t i = digits_start; i < pos; ++i) {
        value = (value * 10) + static_cast<size_t>(text[i] - '0');
      }
      out = value;
    }
  };

  parse_width_or_precision(spec.width);

  if (pos < len && text[pos] == '.') {
    pos += 1;
    parse_width_or_precision(spec.precision);
  }

  if (pos < len && is_type_char(text[pos])) {
    spec.type_char = text[pos];
    pos += 1;
  }

  if (pos != len) {
    emit(diagnostic(diagnostic_level::error,
                    std::format("unrecognized format spec text `{}`",
                                text.substr(pos)),
                    file_id_)
             .with_label(whole_span,
                         "could not parse the rest of this format spec")
             .with_help("A format spec looks like "
                        "`[fill_align][sign][#][0][width][.precision][type]` — "
                        "see spec/string-formatting-design.md for the full "
                        "grammar."));
  }

  return spec;
}

auto parser::parse_string_literal_expr() -> ast::ptr<ast::expr> {
  auto tok = advance();

  const auto make_plain_literal = [&] -> ast::ptr<ast::expr> {
    auto lit = ast::make<ast::literal_expr>();
    lit->span = tok.span;
    lit->lit_kind = tok.kind;
    lit->value = std::string(tok.text);
    return lit;
  };

  if (tok.text.size() < 2) {
    return make_plain_literal();
  }

  const auto content = tok.text.substr(1, tok.text.size() - 2);
  const auto content_base = static_cast<byte_offset>(tok.span.start + 1);

  if (!has_interpolation(content)) {
    return make_plain_literal();
  }

  auto scanned = scan_interpolated_content(content);
  if (!scanned.has_value()) {
    emit(diagnostic(diagnostic_level::error,
                    "unterminated string interpolation — a `{` inside this "
                    "string was never closed with a matching `}`",
                    file_id_)
             .with_label(tok.span, "in this string")
             .with_help("Make sure every `{` inside a string has a matching "
                        "`}`. If you want a literal `{`, write it twice: "
                        "`{{`."));
    auto lit = make_plain_literal();
    lit->has_error = true;
    return lit;
  }

  auto node = ast::make<ast::interpolated_string_expr>();
  node->span = tok.span;

  for (auto &run : *scanned) {
    if (run.is_literal) {
      auto seg = ast::interp_segment{};
      seg.is_literal = true;
      if (auto decoded = decode_string_body(run.literal_text);
          decoded.has_value()) {
        seg.literal_text = std::move(*decoded);
      } else {
        emit(diagnostic(diagnostic_level::error,
                        "invalid escape sequence in string literal", file_id_)
                 .with_label(tok.span, "inside this string")
                 .with_help("Valid escapes are \\n \\t \\r \\0 \\\" \\' \\\\ "
                            "\\{ \\} and \\u{...}."));
        seg.literal_text = run.literal_text;
        node->has_error = true;
      }
      node->segments.push_back(std::move(seg));
      continue;
    }

    auto seg = ast::interp_segment{};
    seg.is_literal = false;

    const auto expr_text =
        content.substr(run.expr_start, run.expr_end - run.expr_start);
    seg.source_text = std::string(expr_text);
    while (!seg.source_text.empty() && (seg.source_text.front() == ' ' ||
                                        seg.source_text.front() == '\t')) {
      seg.source_text.erase(seg.source_text.begin());
    }
    while (!seg.source_text.empty() &&
           (seg.source_text.back() == ' ' || seg.source_text.back() == '\t')) {
      seg.source_text.pop_back();
    }

    seg.value = parse_sub_expr(
        expr_text, content_base + static_cast<byte_offset>(run.expr_start));
    if (!seg.value) {
      seg.value =
          make_error_expr(tok.span, "expected an expression inside `{...}`");
      node->has_error = true;
    }

    seg.self_doc = run.self_doc;
    seg.has_spec = run.has_spec;
    if (run.has_spec) {
      const auto spec_text =
          content.substr(run.spec_start, run.spec_end - run.spec_start);
      const auto spec_span = source_span{
          .start = content_base + static_cast<byte_offset>(run.spec_start),
          .end = content_base + static_cast<byte_offset>(run.spec_end)};
      seg.spec = parse_format_spec_text(
          spec_text, content_base + static_cast<byte_offset>(run.spec_start),
          spec_span);
    }

    node->segments.push_back(std::move(seg));
  }

  return node;
}

auto parser::parse_ident_or_path_expr() -> ast::ptr<ast::expr> {
  // Check if this looks like a lambda: `ident => ...`, or the same with a
  // declared result, `ident -> type => ...`. An `->` here can only be a
  // lambda's return type; every other use of it sits in type position.
  if (allow_lambda_expr_ && (peek_at(1).is(token_kind::fat_arrow) ||
                             peek_at(1).is(token_kind::arrow))) {
    return parse_lambda_expr();
  }

  auto tok = advance();
  auto ident = ast::make<ast::ident_expr>();
  ident->span = tok.span;
  ident->name = std::string(tok.text);

  if (at(token_kind::lbrace)) {
    auto expr = parse_brace_expr();
    if (expr && expr->kind == ast::node_kind::struct_expr) {
      auto *struct_expr = dynamic_cast<ast::struct_expr *>(expr.get());
      struct_expr->type_name = std::move(ident);
      struct_expr->span = struct_expr->type_name->span.merge(struct_expr->span);
    }
    return expr;
  }

  if (at(token_kind::dot) &&
      (peek_at(1).is(token_kind::ident) ||
       peek_at(1).is(token_kind::kw_super)) &&
      !peek_at(2).is(token_kind::lparen) &&
      !peek_at(2).is(token_kind::lbracket)) {
    auto path = ast::make<ast::module_path_expr>();
    path->span = ident->span;
    path->segments.push_back(ident->name);

    while (at(token_kind::dot) &&
           (peek_at(1).is(token_kind::ident) ||
            peek_at(1).is(token_kind::kw_super)) &&
           !peek_at(2).is(token_kind::lparen) &&
           !peek_at(2).is(token_kind::lbracket)) {
      advance();
      auto seg_tok = advance();
      path->segments.push_back(token_text_string(seg_tok));
      path->span.extend_to(seg_tok.span);
    }

    // A `{` after the path opens a struct literal with a module-qualified
    // head, `q.holder { value: 7 }` — the same reading a bare `holder {`
    // already gets above. Without this the path is returned as a complete
    // expression and the `{` becomes a stray statement, which reported
    // "expected end of line after this" pointing at the type name.
    if (at(token_kind::lbrace)) {
      auto expr = parse_brace_expr();
      if (expr && expr->kind == ast::node_kind::struct_expr) {
        auto *literal = dynamic_cast<ast::struct_expr *>(expr.get());
        literal->span = path->span.merge(literal->span);
        literal->type_name = std::move(path);
      }
      return expr;
    }

    return path;
  }

  return ident;
}

auto parser::parse_variant_expr() -> ast::ptr<ast::expr> {
  auto start = peek().span;
  advance(); // consume `@`

  if (!at_any(token_kind::ident, token_kind::kw_some, token_kind::kw_ok,
              token_kind::kw_err)) {
    emit_unexpected("a variant name after `@`");
    return make_error_expr(start, "expected variant name");
  }

  auto tok = advance();
  auto ident = ast::make<ast::ident_expr>();
  ident->span = start.merge(tok.span);
  ident->name = std::string(tok.text);

  // Trailing `(args)`, `.field`, `?`, etc. are handled uniformly by the
  // ordinary postfix-suffix loop in parse_postfix_expr — a variant
  // constructor call is structurally just a call to this identifier.
  return ident;
}

auto parser::parse_trailing_if_expr(ast::ptr<ast::expr> then_expr)
    -> ast::ptr<ast::expr> {
  auto iexpr = ast::make<ast::if_expr>();
  iexpr->span = then_expr->span;

  ast::if_branch branch;
  branch.span = then_expr->span;
  {
    auto stmt = ast::make<ast::expr_stmt>();
    stmt->expr = std::move(then_expr);
    branch.body.push_back(std::move(stmt));
  }

  expect(token_kind::kw_if);
  branch.condition = parse_expr();
  branch.span.extend_to(previous_span());
  iexpr->branches.push_back(std::move(branch));

  expect_with_context(token_kind::kw_else, "in conditional expression");
  auto else_expr = parse_expr();
  if (!else_expr) {
    emit(diagnostic(
             diagnostic_level::error,
             "expected an expression after `else` in conditional expression",
             file_id_)
             .with_label(previous_span(), "the `else` is here"));
    else_expr = make_error_expr(previous_span());
    iexpr->has_error = true;
  }

  {
    auto stmt = ast::make<ast::expr_stmt>();
    stmt->expr = std::move(else_expr);
    iexpr->else_body.push_back(std::move(stmt));
  }

  iexpr->span.extend_to(previous_span());
  return iexpr;
}

auto parser::parse_paren_expr() -> ast::ptr<ast::expr> {
  auto start = peek().span;
  advance(); // consume `(`

  // Empty parens: `()` — unit literal.
  if (at(token_kind::rparen)) {
    advance();
    auto lit = ast::make<ast::literal_expr>();
    lit->span = start.merge(previous_span());
    lit->lit_kind = token_kind::kw_unit;
    lit->value = "()";
    return lit;
  }

  auto first = parse_expr();
  if (!first) {
    expect(token_kind::rparen);
    return make_error_expr(start.merge(previous_span()));
  }

  // Tuple: `(a, b, ...)`
  if (at(token_kind::comma)) {
    auto tuple = ast::make<ast::tuple_expr>();
    tuple->span = start;
    tuple->elements.push_back(std::move(first));

    while (match(token_kind::comma)) {
      if (at(token_kind::rparen)) {
        break; // trailing comma
      }
      auto elem = parse_expr();
      if (elem) {
        tuple->elements.push_back(std::move(elem));
      } else {
        break;
      }
    }

    expect(token_kind::rparen);
    tuple->span.extend_to(previous_span());
    return tuple;
  }

  // Grouped expression: `(expr)`
  expect(token_kind::rparen);
  auto group = ast::make<ast::group_expr>();
  group->span = start.merge(previous_span());
  group->inner = std::move(first);
  return group;
}

auto parser::parse_bracket_expr() -> ast::ptr<ast::expr> {
  auto start = peek().span;
  advance(); // consume `[`

  // Empty array: `[]`
  if (at(token_kind::rbracket)) {
    advance();
    auto arr = ast::make<ast::array_expr>();
    arr->span = start.merge(previous_span());
    return arr;
  }

  auto first = parse_expr();
  if (!first) {
    expect(token_kind::rbracket);
    return make_error_expr(start.merge(previous_span()));
  }

  // Fill form: `[val; count]`
  if (at(token_kind::semicolon)) {
    advance();
    auto count = parse_expr();
    expect(token_kind::rbracket);

    auto arr = ast::make<ast::array_expr>();
    arr->span = start.merge(previous_span());
    arr->fill_value = std::move(first);
    arr->fill_count = std::move(count);
    return arr;
  }

  // Array literal: `[a, b, c]`
  auto arr = ast::make<ast::array_expr>();
  arr->span = start;
  arr->elements.push_back(std::move(first));

  while (match(token_kind::comma)) {
    if (at(token_kind::rbracket)) {
      break; // trailing comma
    }
    auto elem = parse_expr();
    if (elem) {
      arr->elements.push_back(std::move(elem));
    } else {
      break;
    }
  }

  expect(token_kind::rbracket);
  arr->span.extend_to(previous_span());
  return arr;
}

auto parser::parse_brace_expr() -> ast::ptr<ast::expr> {
  auto start = peek().span;
  advance(); // consume `{`

  // Empty struct literal: `{}`
  if (at(token_kind::rbrace)) {
    advance();
    auto s = ast::make<ast::struct_expr>();
    s->span = start.merge(previous_span());
    return s;
  }

  auto s = ast::make<ast::struct_expr>();
  s->span = start;

  while (!at(token_kind::rbrace) && !at_eof()) {
    skip_newlines();
    if (at(token_kind::rbrace)) {
      break;
    }

    ast::struct_field_init field;
    field.span = peek().span;

    auto name_tok = expect(token_kind::ident);
    field.name = std::string(name_tok.text);

    if (match(token_kind::colon)) {
      field.value = parse_expr();
    }
    // else: shorthand {x} means {x: x}

    field.span.extend_to(previous_span());
    s->fields.push_back(std::move(field));

    if (!match(token_kind::comma)) {
      skip_newlines();
      break;
    }
    skip_newlines();
  }

  expect(token_kind::rbrace);
  s->span.extend_to(previous_span());
  return s;
}

auto parser::at_lambda_param_list() const noexcept -> bool {
  if (!allow_lambda_expr_ || !at(token_kind::lparen)) {
    return false;
  }

  // A `(` opens a lambda head only if the `)` closing it is followed by `=>`
  // or by a `->` return type. Both are decisive: `->` occurs nowhere in
  // expression position but a lambda's return type, and `=>` after a closed
  // group is otherwise a syntax error. So one scan to the matching `)`
  // settles the tuple-versus-lambda question without backtracking.
  //
  // Depth counts every bracket flavor, not just parens, so a parenthesized
  // default or a nested call inside the list cannot end the scan early.
  auto depth = 0;
  for (uint32_t offset = 0; !peek_at(offset).is(token_kind::eof); ++offset) {
    switch (peek_at(offset).kind) {
    case token_kind::lparen:
    case token_kind::lbracket:
    case token_kind::lbrace:
      ++depth;
      break;

    case token_kind::rparen:
    case token_kind::rbracket:
    case token_kind::rbrace:
      --depth;
      if (depth == 0) {
        return peek_at(offset + 1).is(token_kind::fat_arrow) ||
               peek_at(offset + 1).is(token_kind::arrow);
      }
      break;

    default:
      break;
    }
  }

  // Unbalanced: let the ordinary paren path report it.
  return false;
}

auto parser::parse_lambda_expr() -> ast::ptr<ast::expr> {
  auto lambda = ast::make<ast::lambda_expr>();
  auto start = peek().span;

  // Optional `pure` and `move` prefixes.
  lambda->is_pure = match(token_kind::kw_pure);
  lambda->is_move = match(token_kind::kw_move);

  // Lambda params: either a single ident or `(param_list)`.
  if (at(token_kind::ident) && peek_at(1).is(token_kind::fat_arrow)) {
    // Single parameter shorthand: `x => expr`
    auto param_tok = advance();
    ast::lambda_param param;
    param.span = param_tok.span;
    auto binding = ast::make<ast::binding_pattern>();
    binding->span = param_tok.span;
    binding->name = std::string(param_tok.text);
    param.pattern = std::move(binding);
    lambda->params.push_back(std::move(param));
  } else if (at(token_kind::lparen)) {
    advance(); // consume `(`
    while (!at(token_kind::rparen) && !at_eof()) {
      ast::lambda_param param;
      param.span = peek().span;
      param.pattern = parse_pattern();
      if (match(token_kind::colon)) {
        param.type_annotation = parse_type_expr();
      }
      param.span.extend_to(previous_span());
      lambda->params.push_back(std::move(param));
      if (!match(token_kind::comma)) {
        break;
      }
    }
    expect(token_kind::rparen);
  } else if (at(token_kind::ident)) {
    // Single ident without `=>` — might have a return type annotation.
    auto param_tok = advance();
    ast::lambda_param param;
    param.span = param_tok.span;
    auto binding = ast::make<ast::binding_pattern>();
    binding->span = param_tok.span;
    binding->name = std::string(param_tok.text);
    param.pattern = std::move(binding);
    lambda->params.push_back(std::move(param));
  }

  // Optional return type.
  if (match(token_kind::arrow)) {
    lambda->return_type = parse_type_expr();
  }

  // `=>` introduces the body.
  if (!at(token_kind::fat_arrow)) {
    emit(diagnostic(diagnostic_level::error,
                    "expected `=>` to start the lambda body", file_id_)
             .with_label(peek().span, "expected `=>` here")
             .with_help("Lambda expressions use `=>` between the parameters "
                        "and the body. For example: `x => x + 1` or "
                        "`(a, b) => a + b`."));
  } else {
    advance(); // consume `=>`
  }

  // Body: inline expr or block.
  if (at(token_kind::colon) && peek_at(1).is(token_kind::newline)) {
    // Block body: `=> : NEWLINE INDENT stmts DEDENT`
    advance(); // consume `:`
    skip_newlines();
    if (match(token_kind::indent)) {
      while (!at_any(token_kind::dedent, token_kind::eof)) {
        skip_newlines();
        if (at_any(token_kind::dedent, token_kind::eof)) {
          break;
        }
        auto stmt = parse_stmt();
        if (stmt) {
          lambda->body_stmts.push_back(std::move(stmt));
        } else {
          synchronize();
        }
      }
      expect_block_end("lambda");
    }
  } else {
    lambda->body_expr = parse_expr();
  }

  lambda->span = start.merge(previous_span());
  return lambda;
}

auto parser::parse_match_expr() -> ast::ptr<ast::match_expr> {
  auto mexpr = ast::make<ast::match_expr>();
  auto start = peek().span;

  expect(token_kind::kw_match);
  mexpr->subject = parse_expr();
  expect(token_kind::colon);
  skip_newlines();

  if (match(token_kind::indent)) {
    while (!at_any(token_kind::dedent, token_kind::eof)) {
      skip_newlines();
      if (at_any(token_kind::dedent, token_kind::eof)) {
        break;
      }
      mexpr->arms.push_back(parse_match_arm());
    }
    expect_block_end("match");
  }

  mexpr->span = start.merge(previous_span());
  return mexpr;
}

auto parser::parse_match_arm() -> ast::match_arm {
  ast::match_arm arm;
  arm.span = peek().span;

  arm.pattern = parse_pattern();

  // Optional guard: `if expr`
  if (match(token_kind::kw_if)) {
    arm.guard = parse_expr();
  }

  // `=>` introduces the arm body.
  if (!match(token_kind::fat_arrow)) {
    emit(diagnostic(diagnostic_level::error,
                    "expected `=>` after match pattern", file_id_)
             .with_label(peek().span, "expected `=>` here")
             .with_help("Each match arm uses `=>` between the pattern and "
                        "the result. For example:\n"
                        "  match x:\n"
                        "    0 => \"zero\"\n"
                        "    _ => \"other\""));
    arm.has_error = true;
  }

  // Body: inline expr or block.
  if (at(token_kind::colon) && peek_at(1).is(token_kind::newline)) {
    advance(); // consume `:`
    skip_newlines();
    if (match(token_kind::indent)) {
      while (!at_any(token_kind::dedent, token_kind::eof)) {
        skip_newlines();
        if (at_any(token_kind::dedent, token_kind::eof)) {
          break;
        }
        auto stmt = parse_stmt();
        if (stmt) {
          arm.body_stmts.push_back(std::move(stmt));
        } else {
          synchronize();
        }
      }
      expect_block_end("match arm");
    }
  } else {
    arm.body_expr = parse_expr();
    if (arm.body_expr) {
      expect_newline();
    } else if (peek().can_start_stmt()) {
      // Inline `match` arms can still contain statement-only forms like
      // `return some(value)`. Parse them as a single statement so the arm
      // always makes progress.
      auto stmt = parse_stmt();
      if (stmt) {
        arm.body_stmts.push_back(std::move(stmt));
      } else {
        arm.has_error = true;
        synchronize_to_newline();
      }
    } else {
      arm.has_error = true;
      emit(diagnostic(diagnostic_level::error,
                      "expected a match arm body after `=>`", file_id_)
               .with_label(previous_span(), "the `=>` is here")
               .with_help("Write either a single expression like `x => value`, "
                          "or a statement like `x => return value`."));
      synchronize_to_newline();
    }
  }

  arm.span.extend_to(previous_span());
  return arm;
}

auto parser::parse_if_expr() -> ast::ptr<ast::if_expr> {
  auto iexpr = ast::make<ast::if_expr>();
  auto start = peek().span;

  auto parse_if_expr_condition = [this](ast::if_branch &branch) -> void {
    if (match(token_kind::kw_let)) {
      branch.let_pattern = parse_pattern();
      expect(token_kind::eq);
      branch.let_expr = parse_expr();
      branch.condition = make_error_expr(previous_span(), "if let condition");
    } else {
      branch.condition = parse_expr();
    }
  };

  auto wrap_inline_body =
      [](ast::ptr<ast::expr> expr) -> std::vector<ast::ptr<ast::node>> {
    std::vector<ast::ptr<ast::node>> body;
    if (expr) {
      auto stmt = ast::make<ast::expr_stmt>();
      stmt->expr = std::move(expr);
      body.push_back(std::move(stmt));
    }
    return body;
  };

  auto parse_if_inline_body = [this](std::string_view construct_name) -> auto {
    auto body = parse_pipe_expr();
    if (!body) {
      emit(
          diagnostic(diagnostic_level::error,
                     std::format("expected an expression after `:` in this {}",
                                 construct_name),
                     file_id_)
              .with_label(previous_span(), "the `:` is here")
              .with_help("Put a single expression on the same line as the `:`, "
                         "or move the body to the next line and indent it."));
      return make_error_expr(previous_span(),
                             "expected inline if-expression body");
    }
    return body;
  };

  expect(token_kind::kw_if);

  // First branch.
  {
    ast::if_branch branch;
    branch.span = start;
    parse_if_expr_condition(branch);
    expect(token_kind::colon);
    if (at(token_kind::newline)) {
      auto body = parse_body("if");
      if (body.inline_expr) {
        branch.body = wrap_inline_body(std::move(body.inline_expr));
      } else {
        branch.body = std::move(body.stmts);
      }
    } else {
      branch.body = wrap_inline_body(parse_if_inline_body("if"));
    }
    branch.span.extend_to(previous_span());
    iexpr->branches.push_back(std::move(branch));
  }

  // elif branches.
  while (at(token_kind::kw_elif)) {
    advance();
    ast::if_branch branch;
    branch.span = previous_span();
    parse_if_expr_condition(branch);
    expect(token_kind::colon);
    if (at(token_kind::newline)) {
      auto body = parse_body("elif");
      if (body.inline_expr) {
        branch.body = wrap_inline_body(std::move(body.inline_expr));
      } else {
        branch.body = std::move(body.stmts);
      }
    } else {
      branch.body = wrap_inline_body(parse_if_inline_body("elif"));
    }
    branch.span.extend_to(previous_span());
    iexpr->branches.push_back(std::move(branch));
  }

  // `else` is required for if-expressions (they must produce a value).
  if (at(token_kind::kw_else)) {
    advance();
    expect(token_kind::colon);
    if (at(token_kind::newline)) {
      auto body = parse_body("else");
      if (body.inline_expr) {
        iexpr->else_body = wrap_inline_body(std::move(body.inline_expr));
      } else {
        iexpr->else_body = std::move(body.stmts);
      }
    } else {
      iexpr->else_body = wrap_inline_body(parse_if_inline_body("else"));
    }
  } else {
    emit(diagnostic(diagnostic_level::error,
                    "an `if` expression must have an `else` branch", file_id_)
             .with_label(previous_span(), "this `if` expression ends here")
             .with_help(
                 "Unlike an `if` statement, an `if` expression must "
                 "produce a value in every case. Add an `else:` branch."));
    iexpr->has_error = true;
  }

  iexpr->span = start.merge(previous_span());
  return iexpr;
}

auto parser::parse_for_expr() -> ast::ptr<ast::for_expr> {
  auto fexpr = ast::make<ast::for_expr>();
  auto start = peek().span;

  expect(token_kind::kw_for);

  // Parse one or more iteration clauses.
  while (true) {
    ast::for_expr::iter_clause clause;
    auto pats = parse_for_vars();
    for (auto &p : pats) {
      clause.patterns.push_back(std::move(p));
    }
    expect_with_context(token_kind::kw_in, "in `for ... in ...`");
    {
      auto saved_allow_lambda = allow_lambda_expr_;
      auto saved_allow_trailing_if = allow_trailing_if_expr_;
      allow_lambda_expr_ = false;
      allow_trailing_if_expr_ = false;
      clause.iterable = parse_expr();
      allow_lambda_expr_ = saved_allow_lambda;
      allow_trailing_if_expr_ = saved_allow_trailing_if;
    }
    if (!clause.iterable) {
      emit_unexpected("an iterable expression after `in`");
      clause.iterable = make_error_expr(previous_span(), "expected iterable");
    }
    fexpr->clauses.push_back(std::move(clause));
    if (!(match(token_kind::comma) &&
          !at_any(token_kind::kw_if, token_kind::fat_arrow))) {
      break;
    }
  }

  if (match(token_kind::kw_if)) {
    auto saved_pos = pos_;
    auto saved_allow_lambda = allow_lambda_expr_;
    allow_lambda_expr_ = false;
    fexpr->guard = parse_expr();
    allow_lambda_expr_ = saved_allow_lambda;
    if (!at(token_kind::fat_arrow)) {
      pos_ = saved_pos;
      allow_lambda_expr_ = false;
      fexpr->guard = parse_cmp_expr();
      allow_lambda_expr_ = saved_allow_lambda;
    }
    if (!fexpr->guard) {
      emit(diagnostic(diagnostic_level::error,
                      "expected a guard expression after `if` in `for` "
                      "expression",
                      file_id_)
               .with_label(previous_span(), "the `if` is here")
               .with_help("A guarded `for` expression looks like: `for x in "
                          "items if predicate(x) => value`."));
      fexpr->guard = make_error_expr(previous_span(), "expected guard");
    }
  }

  expect(token_kind::fat_arrow);
  fexpr->yield_expr = parse_expr();

  fexpr->span = start.merge(previous_span());
  return fexpr;
}

auto parser::parse_await_expr() -> ast::ptr<ast::await_expr> {
  auto aexpr = ast::make<ast::await_expr>();
  auto start = peek().span;

  expect(token_kind::kw_await);

  if (at(token_kind::kw_yield)) {
    advance();
    aexpr->is_yield = true;
  } else {
    aexpr->operand = parse_expr();
  }

  aexpr->span = start.merge(previous_span());
  return aexpr;
}

/// `yield expr` — a generator's value-carrying suspension point. Unlike
/// `parse_await_expr`'s `await yield` (no operand), this form always
/// requires one; the two are disambiguated purely by leading token
/// (`kw_await` vs `kw_yield`), so there's no lookahead conflict.
auto parser::parse_yield_expr() -> ast::ptr<ast::yield_expr> {
  auto yexpr = ast::make<ast::yield_expr>();
  auto start = peek().span;

  expect(token_kind::kw_yield);
  yexpr->value = parse_expr();

  yexpr->span = start.merge(previous_span());
  return yexpr;
}

auto parser::parse_async_expr() -> ast::ptr<ast::async_expr> {
  auto aexpr = ast::make<ast::async_expr>();
  auto start = peek().span;

  expect(token_kind::kw_async);
  expect(token_kind::colon);
  auto saved_allow_compact = allow_compact_stmt_terminator_;
  allow_compact_stmt_terminator_ = true;
  skip_newlines();

  if (match(token_kind::indent)) {
    while (!at_any(token_kind::dedent, token_kind::eof)) {
      skip_newlines();
      if (at_any(token_kind::dedent, token_kind::eof)) {
        break;
      }
      auto stmt = parse_stmt();
      if (stmt) {
        aexpr->body.push_back(std::move(stmt));
      } else {
        synchronize();
      }
    }
    expect_block_end("async");
  } else {
    while (!at_any(token_kind::rparen, token_kind::rbracket, token_kind::rbrace,
                   token_kind::comma, token_kind::eof, token_kind::dedent)) {
      auto stmt = parse_stmt();
      if (stmt) {
        aexpr->body.push_back(std::move(stmt));
      } else {
        synchronize();
        break;
      }
      if (at_any(token_kind::rparen, token_kind::rbracket, token_kind::rbrace,
                 token_kind::comma, token_kind::eof, token_kind::dedent)) {
        break;
      }
    }
  }

  allow_compact_stmt_terminator_ = saved_allow_compact;

  aexpr->span = start.merge(previous_span());
  return aexpr;
}

auto parser::parse_par_expr() -> ast::ptr<ast::par_expr> {
  auto pexpr = ast::make<ast::par_expr>();
  auto start = peek().span;

  expect(token_kind::kw_par);
  expect(token_kind::colon);
  skip_newlines();

  if (match(token_kind::indent)) {
    while (!at_any(token_kind::dedent, token_kind::eof)) {
      skip_newlines();
      if (at_any(token_kind::dedent, token_kind::eof)) {
        break;
      }
      auto expr = parse_expr();
      if (expr) {
        pexpr->branches.push_back(std::move(expr));
      }
      expect_newline();
    }
    expect_block_end("par");
  }

  pexpr->span = start.merge(previous_span());
  return pexpr;
}

auto parser::parse_race_expr() -> ast::ptr<ast::race_expr> {
  auto rexpr = ast::make<ast::race_expr>();
  auto start = peek().span;

  expect(token_kind::kw_race);
  expect(token_kind::colon);
  skip_newlines();

  if (match(token_kind::indent)) {
    while (!at_any(token_kind::dedent, token_kind::eof)) {
      skip_newlines();
      if (at_any(token_kind::dedent, token_kind::eof)) {
        break;
      }
      auto expr = parse_expr();
      if (expr) {
        rexpr->branches.push_back(std::move(expr));
      }
      expect_newline();
    }
    expect_block_end("race");
  }

  rexpr->span = start.merge(previous_span());
  return rexpr;
}

auto parser::parse_crew_expr() -> ast::ptr<ast::crew_expr> {
  auto expr = ast::make<ast::crew_expr>();
  auto start = peek().span;

  expect(token_kind::kw_crew);
  auto name_tok = expect(token_kind::ident);
  expr->name = std::string(name_tok.text);

  if (match(token_kind::lparen)) {
    while (!at(token_kind::rparen) && !at_eof()) {
      ast::crew_option opt;
      opt.span = peek().span;
      auto key_tok = expect(token_kind::ident);
      opt.name = std::string(key_tok.text);
      expect(token_kind::colon);
      auto val_tok = expect(token_kind::ident);
      opt.value = std::string(val_tok.text);
      opt.span.extend_to(previous_span());
      expr->options.push_back(std::move(opt));
      if (!match(token_kind::comma)) {
        break;
      }
    }
    expect(token_kind::rparen);
  }

  expr->body = body_to_stmt_list(parse_body("crew"));

  expr->span = start.merge(previous_span());
  return expr;
}

auto parser::parse_on_expr() -> ast::ptr<ast::on_expr> {
  auto oexpr = ast::make<ast::on_expr>();
  auto start = peek().span;

  expect(token_kind::kw_on);
  expect(token_kind::lparen);
  oexpr->context_type = parse_type_expr();

  if (match(token_kind::comma)) {
    oexpr->sender = parse_expr();
  }
  expect(token_kind::rparen);

  if (at(token_kind::colon)) {
    advance(); // consume `:`
    if (at(token_kind::newline)) {
      skip_newlines();
      if (match(token_kind::indent)) {
        while (!at_any(token_kind::dedent, token_kind::eof)) {
          skip_newlines();
          if (at_any(token_kind::dedent, token_kind::eof)) {
            break;
          }
          auto stmt = parse_stmt();
          if (stmt) {
            oexpr->body.push_back(std::move(stmt));
          } else {
            synchronize();
          }
        }
        expect_block_end("on");
      } else {
        emit(diagnostic(diagnostic_level::error,
                        "expected an indented block after `:` for this on",
                        file_id_)
                 .with_label(previous_span(), "the `:` is here")
                 .with_help(
                     "After `on(...):` and a new line, indent the body. "
                     "For a single expression, keep it on the same line."));
      }
    } else {
      auto inline_expr = parse_expr();
      if (!inline_expr) {
        emit(
            diagnostic(diagnostic_level::error,
                       "expected an expression after `:` in this on", file_id_)
                .with_label(previous_span(), "the `:` is here")
                .with_help("Write `on(context): expr` for a single expression, "
                           "or start a new indented block on the next line."));
      } else {
        auto es = ast::make<ast::expr_stmt>();
        es->expr = std::move(inline_expr);
        oexpr->body.push_back(std::move(es));
      }
    }
  }

  oexpr->span = start.merge(previous_span());
  return oexpr;
}

auto parser::parse_block_expr() -> ast::ptr<ast::block_expr> {
  auto bexpr = ast::make<ast::block_expr>();
  auto start = peek().span;

  expect(token_kind::colon);
  skip_newlines();

  if (match(token_kind::indent)) {
    while (!at_any(token_kind::dedent, token_kind::eof)) {
      skip_newlines();
      if (at_any(token_kind::dedent, token_kind::eof)) {
        break;
      }
      auto stmt = parse_stmt();
      if (stmt) {
        bexpr->stmts.push_back(std::move(stmt));
      } else {
        synchronize();
      }
    }
    expect_block_end("block");
  }

  bexpr->span = start.merge(previous_span());
  return bexpr;
}

auto parser::parse_quote_expr() -> ast::ptr<ast::quote_expr> {
  auto qexpr = ast::make<ast::quote_expr>();
  auto start = peek().span;

  expect(token_kind::backtick);

  // Collect tokens until the closing backtick.
  // Handle nested backticks by tracking depth.
  int depth = 1;
  bool has_paren = match(token_kind::lparen);
  // Real `(...)` nesting *inside* a `` `(...)` `` wrapper (an `(self)`
  // parameter list, a call, ...) — tracked so the wrapper's own closing
  // `)` is only recognized once every such nested paren has itself closed,
  // rather than treating the very first `)` encountered as the wrapper's
  // close. Mirrors the lexer's own `bracket_depth_ == 0` check in its `)`
  // case (`lexer.h`), which the two must agree on for indentation-
  // sensitive quoted content (an `impl`/`def` block) to lex and parse
  // consistently.
  int paren_nesting = 0;

  while (!at_eof() && depth > 0) {
    if (at(token_kind::backtick)) {
      if (has_paren) {
        // Inside `(...)` — backtick is nested.
        qexpr->tokens.push_back(advance());
        ++depth;
      } else {
        --depth;
        if (depth == 0) {
          break;
        }
        qexpr->tokens.push_back(advance());
      }
    } else if (has_paren && at(token_kind::lparen)) {
      ++paren_nesting;
      qexpr->tokens.push_back(advance());
    } else if (at(token_kind::rparen) && has_paren && depth == 1) {
      if (paren_nesting > 0) {
        --paren_nesting;
        qexpr->tokens.push_back(advance());
      } else {
        advance(); // consume the wrapper's own closing `)`
        has_paren = false;
      }
    } else {
      qexpr->tokens.push_back(advance());
    }
  }

  if (at(token_kind::backtick)) {
    advance(); // consume closing backtick
  } else {
    emit(diagnostic(diagnostic_level::error,
                    "unterminated quote expression — expected closing `` ` ``",
                    file_id_)
             .with_label(start, "quote starts here")
             .with_help("Every opening `` ` `` needs a matching closing "
                        "`` ` ``."));
  }

  qexpr->span = start.merge(previous_span());
  classify_and_parse_quote_fragment(*qexpr);
  return qexpr;
}

namespace {

/// Whether `kind` plausibly opens an item-level declaration (as opposed to
/// an ordinary statement or expression) — the same leading-keyword set
/// `parse_top_level_item`/`parse_stmt` dispatch on for `impl`/`trait`/
/// `extend`/`concept`, which `parse_stmt` itself never handles at
/// statement scope. Visibility and function/type modifier keywords are
/// included too, since they only ever prefix a declaration.
auto starts_item(token_kind kind) -> bool {
  switch (kind) {
  case token_kind::kw_use:
  case token_kind::kw_type:
  case token_kind::kw_trait:
  case token_kind::kw_impl:
  case token_kind::kw_extend:
  case token_kind::kw_concept:
  case token_kind::kw_def:
  case token_kind::kw_module:
  case token_kind::kw_dep:
  case token_kind::kw_pub:
  case token_kind::kw_internal:
  case token_kind::kw_super:
  case token_kind::kw_priv:
  case token_kind::kw_pure:
  case token_kind::kw_async:
  case token_kind::kw_machine:
  case token_kind::kw_generator:
  case token_kind::kw_static:
  case token_kind::kw_packed:
    return true;
  default:
    return false;
  }
}

} // namespace

void parser::classify_and_parse_quote_fragment(ast::quote_expr &qexpr) {
  if (qexpr.tokens.empty()) {
    return;
  }

  auto content = qexpr.tokens;
  auto eof = token{};
  eof.kind = token_kind::eof;
  eof.span = previous_span();
  content.push_back(eof);

  auto sub = parser(std::move(content), file_id_, diag_);

  if (starts_item(sub.current())) {
    auto item = sub.parse_top_level_item();
    if (item != nullptr) {
      qexpr.fragment_kind = ast::quote_fragment_kind::def_expr;
      qexpr.parsed_body = std::move(item);
      qexpr.has_error = qexpr.has_error || qexpr.parsed_body->has_error;
    }
    return;
  }

  auto parsed = sub.parse_stmt();
  if (parsed == nullptr) {
    return;
  }
  // `error_stmt` also tags itself `node_kind::expr_stmt` (it has no
  // dedicated kind of its own), so a plain kind check isn't enough to tell
  // it apart from a real `expr_stmt` — use a pointer cast and fall back to
  // treating anything that doesn't actually downcast as a bare statement.
  if (auto *wrapper = dynamic_cast<ast::expr_stmt *>(parsed.get());
      wrapper != nullptr) {
    qexpr.fragment_kind = ast::quote_fragment_kind::expr;
    qexpr.parsed_body = std::move(wrapper->expr);
  } else {
    qexpr.fragment_kind = ast::quote_fragment_kind::stmt;
    qexpr.parsed_body = std::move(parsed);
  }
  qexpr.has_error = qexpr.has_error || qexpr.parsed_body->has_error;
}

auto parser::parse_splice_expr_inner() -> ast::ptr<ast::splice_expr> {
  auto sexpr = ast::make<ast::splice_expr>();
  auto start = peek().span;

  expect(token_kind::tilde);

  if (at(token_kind::lparen)) {
    advance(); // consume `(`
    sexpr->operand = parse_expr();
    expect(token_kind::rparen);
  } else if (peek().can_start_expr()) {
    sexpr->operand = parse_postfix_expr();
  } else {
    emit_unexpected("an expression or identifier after `~`");
    sexpr->operand = make_error_expr(start);
  }

  sexpr->span = start.merge(previous_span());
  return sexpr;
}

auto parser::parse_static_expr() -> ast::ptr<ast::static_expr> {
  auto sexpr = ast::make<ast::static_expr>();
  auto start = peek().span;

  expect(token_kind::kw_static);
  sexpr->operand = parse_expr();

  sexpr->span = start.merge(previous_span());
  return sexpr;
}

/// parse_where_expr — called when a `where` keyword follows an expression.
///
/// Grammar:
///   where_clause_expr = "where" ":" NEWLINE INDENT { where_binding } DEDENT
///   where_binding     = IDENT "=" expr NEWLINE
///
/// The `inner` argument is the already-parsed leading expression that the
/// where clause is being attached to.
auto parser::parse_where_expr(ast::ptr<ast::expr> inner)
    -> ast::ptr<ast::where_expr> {
  auto wexpr = ast::make<ast::where_expr>();
  wexpr->span = inner->span;
  wexpr->inner = std::move(inner);

  expect(token_kind::kw_where);

  // Expect the `:` that opens the where block.
  if (!at(token_kind::colon)) {
    auto found = peek();
    auto diag =
        diagnostic(diagnostic_level::error,
                   std::format("expected `:` after `where` but found {}",
                               found.is_eof() ? "end of file"
                                              : token_kind_name(found.kind)),
                   file_id_)
            .with_label(found.span, "expected `:` here")
            .with_note("A `where` clause introduces a local binding "
                       "block attached to the preceding expression.")
            .with_help("Write it as `expr where:` followed by an "
                       "indented block of `name = expr` bindings.");
    if (at(token_kind::newline) && peek_at(1).is(token_kind::indent)) {
      diag.with_fix("insert `:` before the newline", found.span, ":");
    }
    emit(diag);
    wexpr->has_error = true;

    // Recovery: if the user started a binding block on the next line, treat
    // this as a missing colon and keep parsing the where-bindings.
    if (at(token_kind::newline) && peek_at(1).is(token_kind::indent)) {
      advance();
      match(token_kind::indent);

      while (!at_any(token_kind::dedent, token_kind::eof)) {
        skip_newlines();
        if (at_any(token_kind::dedent, token_kind::eof)) {
          break;
        }

        ast::where_binding binding;
        binding.span = peek().span;

        if (!at(token_kind::ident)) {
          emit_unexpected("an identifier for the `where` binding name");
          synchronize_to_newline();
          continue;
        }

        auto name_tok = advance();
        binding.name = std::string(name_tok.text);

        if (!match(token_kind::eq)) {
          emit(diagnostic(diagnostic_level::error,
                          std::format("expected `=` after `{}` in `where` "
                                      "binding",
                                      binding.name),
                          file_id_)
                   .with_label(previous_span(), "expected `=` after this name")
                   .with_help(
                       "Each `where` binding has the form `name = expr`."));
          synchronize_to_newline();
          continue;
        }

        binding.value = parse_expr();
        if (!binding.value) {
          emit_unexpected("an expression after `=` in `where` binding");
          binding.value = make_error_expr(previous_span());
        }

        binding.span.extend_to(previous_span());
        wexpr->bindings.push_back(std::move(binding));
        expect_newline();
      }

      expect_block_end("where");
      wexpr->span.extend_to(previous_span());
      return wexpr;
    }

    wexpr->span.extend_to(found.span);
    return wexpr;
  }
  advance(); // consume `:`

  // Expect NEWLINE then INDENT to open the block.
  skip_newlines();
  if (!match(token_kind::indent)) {
    emit(diagnostic(diagnostic_level::error,
                    "expected an indented block of bindings after `where:`",
                    file_id_)
             .with_label(previous_span(),
                         "expected an indented block to follow here")
             .with_help("Each binding should be on its own indented line: "
                        "`name = expr`"));
    wexpr->has_error = true;
    wexpr->span.extend_to(previous_span());
    return wexpr;
  }

  // Parse one or more `name = expr NEWLINE` bindings.
  while (!at_any(token_kind::dedent, token_kind::eof)) {
    skip_newlines();
    if (at_any(token_kind::dedent, token_kind::eof)) {
      break;
    }

    ast::where_binding binding;
    binding.span = peek().span;

    // Expect an identifier as the binding name.
    if (!at(token_kind::ident)) {
      emit_unexpected("an identifier for the `where` binding name");
      wexpr->has_error = true;
      synchronize_to_newline();
      continue;
    }
    auto name_tok = advance();
    binding.name = std::string(name_tok.text);

    // Expect `=`.
    if (!at(token_kind::eq)) {
      emit(diagnostic(diagnostic_level::error,
                      std::format("expected `=` after `{}` in `where` binding",
                                  binding.name),
                      file_id_)
               .with_label(previous_span(), "expected `=` after this name")
               .with_help("Each `where` binding has the form `name = expr`."));
      wexpr->has_error = true;
      synchronize_to_newline();
      continue;
    }
    advance(); // consume `=`

    // Parse the right-hand side expression.
    binding.value = parse_expr();
    if (!binding.value) {
      emit_unexpected("an expression after `=` in `where` binding");
      wexpr->has_error = true;
      binding.value = make_error_expr(previous_span());
    }

    binding.span.extend_to(previous_span());
    wexpr->bindings.push_back(std::move(binding));

    expect_newline();
  }

  if (wexpr->bindings.empty() && !wexpr->has_error) {
    emit(diagnostic(diagnostic_level::error,
                    "a `where` clause must have at least one binding", file_id_)
             .with_label(previous_span(), "empty `where` block")
             .with_help("Add at least one `name = expr` binding, or remove "
                        "the `where` clause entirely."));
    wexpr->has_error = true;
  }

  expect_block_end("where");

  wexpr->span.extend_to(previous_span());
  return wexpr;
}

// ==========================================================================
//  Call arguments
// ==========================================================================

auto parser::parse_call_args() -> std::vector<ast::call_arg> {
  std::vector<ast::call_arg> args;

  while (true) {
    skip_newlines();
    if (at(token_kind::rparen)) {
      break;
    }

    ast::call_arg arg;
    arg.span = peek().span;

    // Check for named argument: `name: expr`
    if (at(token_kind::ident) && peek_at(1).is(token_kind::colon)) {
      auto name_tok = advance();
      advance(); // consume `:`
      arg.name = std::string(name_tok.text);
      arg.value = parse_expr();
    } else {
      arg.value = parse_expr();
    }

    arg.span.extend_to(previous_span());
    args.push_back(std::move(arg));

    skip_newlines();
    if (!match(token_kind::comma)) {
      break;
    }
  }

  return args;
}

// ==========================================================================
//  patterns
// ==========================================================================

auto parser::parse_pattern() -> ast::ptr<ast::pattern> {
  auto pat = parse_or_pattern();
  if (!pat) {
    return make_error_pattern(peek().span);
  }

  // Optional alias: `pattern as name`
  if (at(token_kind::kw_as)) {
    advance();
    auto name_tok = expect(token_kind::ident);
    auto group = ast::make<ast::group_pattern>();
    group->span = pat->span.merge(name_tok.span);
    group->inner = std::move(pat);
    group->alias = std::string(name_tok.text);
    return group;
  }

  return pat;
}

auto parser::parse_or_pattern() -> ast::ptr<ast::pattern> {
  auto first = parse_atomic_pattern();
  if (!first) {
    return nullptr;
  }

  if (!at(token_kind::pipe)) {
    return first;
  }

  auto or_pat = ast::make<ast::or_pattern>();
  or_pat->span = first->span;
  or_pat->alternatives.push_back(std::move(first));

  while (match(token_kind::pipe)) {
    auto alt = parse_atomic_pattern();
    if (alt) {
      or_pat->alternatives.push_back(std::move(alt));
    } else {
      emit_unexpected("a pattern after `|`");
      or_pat->has_error = true;
      break;
    }
  }

  or_pat->span.extend_to(previous_span());
  return or_pat;
}

auto parser::parse_atomic_pattern() -> ast::ptr<ast::pattern> {
  switch (current()) {
  case token_kind::kw_underscore:
    return parse_wildcard_pattern();

  case token_kind::int_lit:
  case token_kind::float_lit:
  case token_kind::string_lit:
  case token_kind::char_lit:
  case token_kind::kw_true:
  case token_kind::kw_false:
  case token_kind::kw_unit:
    return parse_literal_pattern();

  case token_kind::ident:
    return parse_ident_or_constructor_pattern();

  case token_kind::kw_mut:
    return parse_mut_binding_pattern();

  case token_kind::at:
    return parse_variant_pattern();

  case token_kind::lparen:
    return parse_paren_pattern();

  case token_kind::lbrace:
    return parse_brace_pattern();

  case token_kind::lbracket:
    return parse_bracket_pattern();

  case token_kind::amp:
    return parse_ref_pattern();

  case token_kind::dot_dot: {
    // Range pattern with no start: `..end`
    advance();
    auto range = ast::make<ast::range_pattern>();
    range->span = previous_span();
    if (at_any(token_kind::int_lit, token_kind::float_lit, token_kind::ident)) {
      range->end = parse_expr();
      range->span.extend_to(previous_span());
    }
    return range;
  }

  default:
    return nullptr;
  }
}

auto parser::parse_wildcard_pattern() -> ast::ptr<ast::pattern> {
  auto tok = advance();
  auto wc = ast::make<ast::wildcard_pattern>();
  wc->span = tok.span;
  return wc;
}

auto parser::parse_literal_pattern() -> ast::ptr<ast::pattern> {
  auto tok = advance();
  auto lit = ast::make<ast::literal_pattern>();
  lit->span = tok.span;
  lit->lit_kind = tok.kind;
  lit->value = std::string(tok.text);

  // Check for range: `lit..lit` or `lit..=lit` or `lit..`
  if (at_any(token_kind::dot_dot, token_kind::dot_dot_eq)) {
    bool inclusive = at(token_kind::dot_dot_eq);
    advance();
    auto range = ast::make<ast::range_pattern>();
    range->span = lit->span;
    range->inclusive = inclusive;

    // Create an expression from the literal for the start.
    auto start_expr = ast::make<ast::literal_expr>();
    start_expr->span = lit->span;
    start_expr->lit_kind = lit->lit_kind;
    start_expr->value = std::move(lit->value);
    range->start = std::move(start_expr);

    // Parse end expression if present.
    if (peek().is_literal() || at(token_kind::ident)) {
      range->end = parse_expr();
    }

    range->span.extend_to(previous_span());
    return range;
  }

  return lit;
}

auto parser::parse_ident_or_constructor_pattern() -> ast::ptr<ast::pattern> {
  auto tok = advance();
  auto start = tok.span;

  // A bare identifier is always a variable — never a constructor. Variant
  // constructors are always written with a leading `@` (see
  // parse_variant_pattern), so a plain IDENT here can only be a binding
  // or the start of an `ident..ident` range.

  // Check for range: `ident..ident`
  if (at_any(token_kind::dot_dot, token_kind::dot_dot_eq)) {
    bool inclusive = at(token_kind::dot_dot_eq);
    advance();
    auto range = ast::make<ast::range_pattern>();
    range->span = start;
    range->inclusive = inclusive;

    auto start_expr = ast::make<ast::ident_expr>();
    start_expr->span = tok.span;
    start_expr->name = std::string(tok.text);
    range->start = std::move(start_expr);

    if (peek().is_literal() || at(token_kind::ident)) {
      range->end = parse_expr();
    }

    range->span.extend_to(previous_span());
    return range;
  }

  // Simple binding pattern.
  auto binding = ast::make<ast::binding_pattern>();
  binding->span = start;
  binding->name = std::string(tok.text);
  return binding;
}

/// `mut` prefix on a binding pattern (`mut self`, `mut x`) — marks the
/// bound name reassignable, matching `let mut`/`var` semantics rather than
/// the default immutable `let`/pattern binding. Only a plain identifier may
/// follow `mut`; it is not a constructor or range start.
auto parser::parse_mut_binding_pattern() -> ast::ptr<ast::pattern> {
  auto mut_tok = advance(); // consume `mut`
  auto name_tok = expect(token_kind::ident);

  auto binding = ast::make<ast::binding_pattern>();
  binding->span = mut_tok.span.merge(name_tok.span);
  binding->name = std::string(name_tok.text);
  binding->is_mut = true;
  return binding;
}

auto parser::parse_variant_pattern() -> ast::ptr<ast::pattern> {
  auto at_start = peek().span;
  advance(); // consume `@`

  // `@some(...)`, `@ok(...)`, `@err(...)` reuse the dedicated option/result
  // pattern nodes; parse_option_result_pattern expects to see the keyword
  // token as the current token, which is now guaranteed by the `@` we just
  // consumed.
  if (at_any(token_kind::kw_some, token_kind::kw_ok, token_kind::kw_err)) {
    auto pat = parse_option_result_pattern();
    if (pat) {
      pat->span = at_start.merge(pat->span);
    }
    return pat;
  }

  if (!at(token_kind::ident)) {
    emit_unexpected("a variant name after `@`");
    return make_error_pattern(at_start);
  }

  auto tok = advance();
  auto ctor = ast::make<ast::constructor_pattern>();
  ctor->span = at_start;
  ctor->name = std::string(tok.text);

  if (match(token_kind::lparen)) {
    while (!at(token_kind::rparen) && !at_eof()) {
      auto pat = parse_pattern();
      if (pat) {
        ctor->args.push_back(std::move(pat));
      }
      if (!match(token_kind::comma)) {
        break;
      }
    }
    expect(token_kind::rparen);
  }

  ctor->span.extend_to(previous_span());
  return ctor;
}

auto parser::parse_paren_pattern() -> ast::ptr<ast::pattern> {
  auto start = peek().span;
  advance(); // consume `(`

  // Empty tuple pattern.
  if (at(token_kind::rparen)) {
    advance();
    auto tuple = ast::make<ast::tuple_pattern>();
    tuple->span = start.merge(previous_span());
    return tuple;
  }

  auto first = parse_pattern();
  if (!first) {
    expect(token_kind::rparen);
    return make_error_pattern(start.merge(previous_span()));
  }

  // Tuple pattern: `(a, b, c)`
  if (at(token_kind::comma)) {
    auto tuple = ast::make<ast::tuple_pattern>();
    tuple->span = start;
    tuple->elements.push_back(std::move(first));

    while (match(token_kind::comma)) {
      if (at(token_kind::rparen)) {
        break;
      }
      auto elem = parse_pattern();
      if (elem) {
        tuple->elements.push_back(std::move(elem));
      } else {
        break;
      }
    }

    expect(token_kind::rparen);
    tuple->span.extend_to(previous_span());
    return tuple;
  }

  // Grouped pattern: `(pattern)`
  expect(token_kind::rparen);
  auto group = ast::make<ast::group_pattern>();
  group->span = start.merge(previous_span());
  group->inner = std::move(first);
  return group;
}

auto parser::parse_brace_pattern() -> ast::ptr<ast::pattern> {
  auto start = peek().span;
  advance(); // consume `{`

  auto spat = ast::make<ast::struct_pattern>();
  spat->span = start;

  while (!at(token_kind::rbrace) && !at_eof()) {
    skip_newlines();
    if (at(token_kind::rbrace)) {
      break;
    }

    ast::field_pattern fp;
    fp.span = peek().span;

    if (at(token_kind::dot_dot)) {
      advance();
      fp.is_rest = true;
      spat->fields.push_back(std::move(fp));
      break; // `..` must be last
    }

    auto name_tok = expect(token_kind::ident);
    fp.name = std::string(name_tok.text);

    if (match(token_kind::colon)) {
      fp.pattern = parse_pattern();
    }
    // else: shorthand {name}

    fp.span.extend_to(previous_span());
    spat->fields.push_back(std::move(fp));

    if (!match(token_kind::comma)) {
      skip_newlines();
      break;
    }
    skip_newlines();
  }

  expect(token_kind::rbrace);
  spat->span.extend_to(previous_span());
  return spat;
}

auto parser::parse_bracket_pattern() -> ast::ptr<ast::pattern> {
  auto start = peek().span;
  advance(); // consume `[`

  auto apat = ast::make<ast::array_pattern>();
  apat->span = start;

  while (!at(token_kind::rbracket) && !at_eof()) {
    auto elem = parse_pattern();
    if (elem) {
      apat->elements.push_back(std::move(elem));
    }
    if (!match(token_kind::comma)) {
      break;
    }
  }

  expect(token_kind::rbracket);
  apat->span.extend_to(previous_span());
  return apat;
}

auto parser::parse_option_result_pattern() -> ast::ptr<ast::pattern> {
  auto start = peek().span;
  auto kw = advance();

  expect(token_kind::lparen);
  auto inner = parse_pattern();
  if (!inner) {
    inner = make_error_pattern(peek().span);
  }
  expect(token_kind::rparen);

  if (kw.kind == token_kind::kw_some) {
    auto pat = ast::make<ast::option_pattern>();
    pat->span = start.merge(previous_span());
    pat->option_kind = ast::option_result_kind::some;
    pat->inner = std::move(inner);
    return pat;
  }

  auto pat = ast::make<ast::result_pattern>();
  pat->span = start.merge(previous_span());
  pat->result_kind = (kw.kind == token_kind::kw_ok)
                         ? ast::option_result_kind::ok
                         : ast::option_result_kind::err;
  pat->inner = std::move(inner);
  return pat;
}

auto parser::parse_ref_pattern() -> ast::ptr<ast::pattern> {
  auto start = peek().span;
  advance(); // consume `&`

  auto inner = parse_pattern();
  if (!inner) {
    emit_unexpected("a pattern after `&`");
    inner = make_error_pattern(start);
  }

  auto ref_pat = ast::make<ast::ref_pattern>();
  ref_pat->span = start.merge(previous_span());
  ref_pat->inner = std::move(inner);
  return ref_pat;
}

// ==========================================================================
//  For-loop variable parsing
// ==========================================================================

auto parser::parse_for_vars() -> std::vector<ast::ptr<ast::pattern>> {
  std::vector<ast::ptr<ast::pattern>> patterns;

  patterns.push_back(parse_pattern());

  while (match(token_kind::comma)) {
    // Check if the next thing is `in` — if so, the comma was part of
    // the for syntax, not another variable.
    if (at(token_kind::kw_in)) {
      break;
    }
    patterns.push_back(parse_pattern());
  }

  return patterns;
}

// ==========================================================================
//  Operator conversion helpers
// ==========================================================================

auto parser::token_to_assign_op(token_kind kind) noexcept
    -> std::optional<ast::assign_op> {
  switch (kind) {
  case token_kind::eq:
    return ast::assign_op::assign;
  case token_kind::plus_eq:
    return ast::assign_op::add_assign;
  case token_kind::minus_eq:
    return ast::assign_op::sub_assign;
  case token_kind::star_eq:
    return ast::assign_op::mul_assign;
  case token_kind::slash_eq:
    return ast::assign_op::div_assign;
  case token_kind::percent_eq:
    return ast::assign_op::mod_assign;
  case token_kind::amp_eq:
    return ast::assign_op::and_assign;
  case token_kind::pipe_eq:
    return ast::assign_op::or_assign;
  case token_kind::caret_eq:
    return ast::assign_op::xor_assign;
  case token_kind::lt_lt_eq:
    return ast::assign_op::shl_assign;
  case token_kind::gt_gt_eq:
    return ast::assign_op::shr_assign;
  case token_kind::plus_percent_eq:
    return ast::assign_op::add_wrap_assign;
  case token_kind::minus_percent_eq:
    return ast::assign_op::sub_wrap_assign;
  case token_kind::star_percent_eq:
    return ast::assign_op::mul_wrap_assign;
  case token_kind::plus_pipe_eq:
    return ast::assign_op::add_sat_assign;
  case token_kind::minus_pipe_eq:
    return ast::assign_op::sub_sat_assign;
  case token_kind::star_pipe_eq:
    return ast::assign_op::mul_sat_assign;
  default:
    return std::nullopt;
  }
}

auto parser::token_to_cmp_op() -> std::optional<ast::binary_op> {
  switch (current()) {
  case token_kind::eq_eq:
    return ast::binary_op::eq_eq;
  case token_kind::bang_eq:
    return ast::binary_op::bang_eq;
  case token_kind::lt:
    return ast::binary_op::lt;
  case token_kind::lt_eq:
    return ast::binary_op::lt_eq;
  case token_kind::gt:
    return ast::binary_op::gt;
  case token_kind::gt_eq:
    return ast::binary_op::gt_eq;
  case token_kind::kw_in:
    return ast::binary_op::in;
  case token_kind::kw_not:
    // `not in` — peek ahead.
    if (peek_at(1).is(token_kind::kw_in)) {
      return ast::binary_op::not_in;
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

auto parser::token_to_add_op(token_kind kind) noexcept
    -> std::optional<ast::binary_op> {
  switch (kind) {
  case token_kind::plus:
    return ast::binary_op::add;
  case token_kind::minus:
    return ast::binary_op::sub;
  case token_kind::plus_percent:
    return ast::binary_op::add_wrap;
  case token_kind::minus_percent:
    return ast::binary_op::sub_wrap;
  case token_kind::plus_pipe:
    return ast::binary_op::add_sat;
  case token_kind::minus_pipe:
    return ast::binary_op::sub_sat;
  default:
    return std::nullopt;
  }
}

auto parser::token_to_mul_op(token_kind kind) noexcept
    -> std::optional<ast::binary_op> {
  switch (kind) {
  case token_kind::star:
    return ast::binary_op::mul;
  case token_kind::slash:
    return ast::binary_op::div;
  case token_kind::percent:
    return ast::binary_op::mod;
  case token_kind::star_percent:
    return ast::binary_op::mul_wrap;
  case token_kind::star_pipe:
    return ast::binary_op::mul_sat;
  default:
    return std::nullopt;
  }
}

} // namespace kira
