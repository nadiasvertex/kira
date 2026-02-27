#include "parser.h"

#include <cassert>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kira {

// ==========================================================================
//  Helper: make an error expression for recovery.
// ==========================================================================
static ast::ptr<ast::Expr> make_error_expr(span span, std::string desc = "") {
  auto node = ast::make<ast::error_expr>(span, std::move(desc));
  return node;
}

static ast::ptr<ast::Pattern> make_error_pattern(span span) {
  return ast::make<ast::error_pattern>(span);
}

static ast::ptr<ast::node> make_error_node(span span, std::string desc = "") {
  return ast::make<ast::error_node>(span, std::move(desc));
}

// ==========================================================================
//  Token stream navigation & error recovery
// ==========================================================================

auto Parser::expect_newline() -> bool {
  if (match(token_kind::newline)) {
    return true;
}

  // At EOF or DEDENT, a newline is implicitly present.
  if (at_any(token_kind::eof, token_kind::dedent)) {
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

auto Parser::expect(token_kind expected) -> Token {
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

  emit(std::move(diag));

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
  return Token{
      .kind = expected,
      .span = found.span,
      .text = {},
      .error_message = {},
  };
}

auto Parser::expect_with_context(token_kind expected,
                                  std::string_view context) -> Token {
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

  emit(std::move(diag));

  if (!at_recovery_point() && !at_eof() &&
      !at_any(token_kind::indent, token_kind::dedent, token_kind::newline)) {
    advance();
  }

  return Token{
      .kind = expected,
      .span = found.span,
      .text = {},
      .error_message = {},
  };
}

void Parser::emit_unexpected(std::string_view expected_description) {
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

auto Parser::at_recovery_point() const noexcept -> bool {
  return peek().can_start_stmt() || at_any(token_kind::eof, token_kind::dedent);
}

void Parser::synchronize() {
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

void Parser::synchronize_to_newline() {
  while (!at_eof() && !at_any(token_kind::newline, token_kind::dedent)) {
    advance();
  }
  match(token_kind::newline);
}

void Parser::skip_block() {
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

auto Parser::expect_block_start(std::string_view construct_name) -> bool {
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
             .with_fix("add `:`", Span{span.end, span.end}, ":"));
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

auto Parser::expect_block_end(std::string_view construct_name) -> bool {
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
std::vector<ast::ptr<T>>
Parser::parse_block(std::string_view construct_name,
                    std::function<ast::ptr<T>()> parse_item) {
  std::vector<ast::ptr<T>> items;

  while (!at_any(token_kind::dedent, token_kind::eof)) {
    skip_newlines();
    if (at_any(token_kind::dedent, token_kind::eof))
      break;

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
template std::vector<ast::Ptr<ast::Node>>
    Parser::parse_block<ast::Node>(std::string_view,
                                   std::function<ast::Ptr<ast::Node>()>);

auto Parser::parse_body(std::string_view construct_name) -> Parser::BodyResult {
  BodyResult result;

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
  result.inline_expr = parse_expr();
  expect_newline();
  return result;
}

// ==========================================================================
//  Comma-separated list helpers
// ==========================================================================

template <typename T>
std::vector<T>
Parser::parse_delimited_list(token_kind open, token_kind close,
                             std::string_view construct_name,
                             std::function<std::optional<T>()> parse_element) {
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
               .with_fix("add a comma", Span{span.end, span.end}, ", "));
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
std::vector<T>
Parser::parse_comma_list(std::function<std::optional<T>()> parse_element,
                         std::function<bool()> at_end_fn) {
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

auto Parser::parse_optional_visibility() -> ast::visibility {
  if (peek().is_visibility()) {
    auto tok = advance();
    return ast::token_to_visibility(tok.kind);
  }
  return ast::visibility::def;
}

// ==========================================================================
//  Module paths
// ==========================================================================

std::vector<std::string> Parser::parse_module_path() {
  std::vector<std::string> segments;

  auto ident = expect(token_kind::ident);
  segments.emplace_back(ident.text);

  while (at(token_kind::dot) && peek_at(1).is(token_kind::ident)) {
    advance();            // consume `.`
    auto seg = advance(); // consume ident
    segments.emplace_back(seg.text);
  }

  return segments;
}

// ==========================================================================
//  Top-level: parse_file
// ==========================================================================

ast::Ptr<ast::File> Parser::parse_file() {
  auto file = ast::make<ast::file>();
  auto file_start = peek().span;

  skip_newlines();

  // Parse module declaration.
  if (at(token_kind::kw_module)) {
    file->module_decl = parse_module_decl();
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

  // Parse top-level items.
  while (!at_eof()) {
    skip_newlines();
    if (at_eof()) {
      break;
}

    auto item = parse_top_level_item();
    if (item) {
      file->items.push_back(std::move(item));
    } else {
      // Recovery: skip to the next top-level item.
      synchronize();
    }
  }

  file->span = file_start.merge(previous_span());
  return file;
}

// ==========================================================================
//  Module declaration
// ==========================================================================

ast::Ptr<ast::module_decl> Parser::parse_module_decl() {
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

ast::Ptr<ast::Node> Parser::parse_top_level_item() {
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

  case token_kind::kw_concept:
    return parse_concept_decl(vis);

  case token_kind::kw_def:
    return parse_func_decl(vis, ast::func_modifiers{});

  case token_kind::kw_pure:
  case token_kind::kw_async:
  case token_kind::kw_machine: {
    auto mods = parse_func_modifiers();
    if (at(token_kind::kw_def)) {
      return parse_func_decl(vis, std::move(mods));
    }
    emit_unexpected("`def` after function modifiers");
    synchronize_to_newline();
    return make_error_node(peek().span, "expected function declaration");
  }

  case token_kind::kw_static:
    return parse_static_decl(vis);

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
    return parse_top_level_item(); // Skip blank lines.

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

    emit(diagnostic(diagnostic_level::error,
                    std::format("unexpected {} at the top level of the file",
                                token_kind_name(current())),
                    file_id_)
             .with_label(peek().span, "this can't appear here")
             .with_help("At the top level, only declarations are allowed: "
                        "`use`, `type`, `trait`, `impl`, `concept`, `def`, "
                        "`static`, `module`, `dep`."));
    synchronize_to_newline();
    return make_error_node(peek().span);
  }
}

// ==========================================================================
//  Use declarations
// ==========================================================================

ast::Ptr<ast::use_decl> Parser::parse_use_decl(ast::visibility vis) {
  auto decl = ast::make<ast::use_decl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(token_kind::kw_use);

  // Parse the use path: `module_path [. use_selector]`
  decl->path = parse_module_path();

  // Check for selector: `.{items}` or `.*` or `.Name [as Alias]`
  if (at(token_kind::dot)) {
    // Peek at what follows the dot.
    auto after_dot = peek_at(1);

    if (after_dot.is(token_kind::lbrace)) {
      advance(); // consume `.`
      advance(); // consume `{`

      ast::use_selector sel;
      sel.span = after_dot.span;
      sel.kind = ast::UseSelectorKind::Group;

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
      sel.kind = ast::UseSelectorKind::Wildcard;
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

ast::Ptr<ast::sub_module_decl>
Parser::parse_sub_module_decl(ast::visibility vis) {
  auto decl = ast::make<ast::sub_module_decl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(token_kind::kw_module);
  auto name_tok = expect(token_kind::ident);
  decl->name = std::string(name_tok.text);

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

ast::Ptr<ast::dep_decl> Parser::parse_dep_decl() {
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

ast::Ptr<ast::type_decl> Parser::parse_type_decl(ast::visibility vis) {
  auto decl = ast::make<ast::type_decl>();
  auto start = peek().span;
  decl->visibility = vis;

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

ast::Ptr<ast::Node> Parser::parse_type_def() {
  // Sum body: starts with `|`
  if (at(token_kind::pipe)) {
    auto body = parse_sum_body();
    auto node = ast::make<ast::error_node>(body.span, "sum body");
    // We wrap sum_body into a Node — in practice, the type_decl stores it
    // as a generic Node* and we check the structure later.
    // For a cleaner design we could use variant, but this keeps things simple.
    // Actually, let's just return a named_type that wraps the concept.
    // For now, store as error_node with the sum body embedded — the real
    // solution would be a dedicated SumBodyNode. Let's create an inline
    // placeholder that is not an error.
    auto result = ast::make<ast::named_type>();
    result->span = body.span;
    // Store variant info in the named type — semantic layer will handle this.
    // TODO: proper sum body node
    return result;
  }

  // Struct body: starts with `{`
  if (at(token_kind::lbrace)) {
    auto body = parse_struct_body();
    auto result = ast::make<ast::named_type>();
    result->span = body.span;
    // TODO: proper struct body node
    return result;
  }

  // Otherwise it's a type expression (or refinement type).
  auto type = parse_type_expr();
  if (!type) {
    return make_error_node(peek().span, "expected type definition");
  }

  // Check for refinement: `type_expr where expr`
  if (at(token_kind::kw_where)) {
    // This is actually a `where` clause on the type, not a refinement.
    // Refinement uses `where` after the type expr directly.
    // For now, return the type expr — semantic analysis will handle it.
  }

  return type;
}

auto Parser::parse_struct_body() -> ast::struct_body {
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

auto Parser::parse_struct_field() -> ast::struct_field {
  ast::struct_field field;
  field.span = peek().span;
  field.visibility = parse_optional_visibility();

  auto name_tok = expect(token_kind::ident);
  field.name = std::string(name_tok.text);
  expect(token_kind::colon);
  field.type = parse_type_expr();

  field.span.extend_to(previous_span());
  return field;
}

auto Parser::parse_sum_body() -> ast::sum_body {
  ast::sum_body body;
  body.span = peek().span;

  // First `|` is required.
  expect(token_kind::pipe);
  body.variants.push_back(parse_sum_variant());

  while (match(token_kind::pipe)) {
    body.variants.push_back(parse_sum_variant());
  }

  body.span.extend_to(previous_span());
  return body;
}

auto Parser::parse_sum_variant() -> ast::sum_variant {
  ast::sum_variant variant;
  variant.span = peek().span;

  auto name_tok = expect(token_kind::ident);
  variant.name = std::string(name_tok.text);

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

ast::Ptr<ast::type_expr> Parser::parse_type_expr() {
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

ast::Ptr<ast::type_expr> Parser::parse_prim_type_expr() {
  switch (current()) {
  case token_kind::ident:
    return parse_named_type();

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

  case token_kind::kw_expr:
  case token_kind::kw_stmt:
  case token_kind::kw_def_expr:
  case token_kind::kw_type_expr: {
    auto tok = advance();
    auto qt = ast::make<ast::quote_type>(tok.kind);
    qt->span = tok.span;
    return qt;
  }

  default:
    return nullptr;
  }
}

ast::Ptr<ast::named_type> Parser::parse_named_type() {
  auto named = ast::make<ast::named_type>();
  named->span = peek().span;

  named->path = parse_module_path();

  // Optional type arguments: `[T, U, ...]`
  if (at(token_kind::lbracket)) {
    advance(); // consume `[`
    while (!at(token_kind::rbracket) && !at_eof()) {
      skip_newlines();
      if (at(token_kind::rbracket)) {
        break;
}

      auto type_arg = parse_type_expr();
      if (type_arg) {
        named->type_args.push_back(std::move(type_arg));
      }

      if (!match(token_kind::comma)) {
        break;
}
      skip_newlines();
    }
    expect(token_kind::rbracket);
  }

  named->span.extend_to(previous_span());
  return named;
}

ast::Ptr<ast::tuple_type> Parser::parse_tuple_type() {
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

ast::Ptr<ast::slice_type> Parser::parse_slice_type() {
  auto slice = ast::make<ast::slice_type>();
  slice->span = peek().span;

  expect(token_kind::lbracket);
  slice->element = parse_type_expr();
  expect(token_kind::rbracket);

  slice->span.extend_to(previous_span());
  return slice;
}

ast::Ptr<ast::array_type> Parser::parse_array_type() {
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

ast::Ptr<ast::ref_type> Parser::parse_ref_type() {
  auto ref = ast::make<ast::ref_type>();
  ref->span = peek().span;

  expect(token_kind::amp);
  ref->is_mut = match(token_kind::kw_mut);
  ref->inner = parse_type_expr();

  ref->span.extend_to(previous_span());
  return ref;
}

ast::Ptr<ast::ptr_type> Parser::parse_ptr_type() {
  auto ptr = ast::make<ast::ptr_type>();
  ptr->span = peek().span;

  expect(token_kind::star);
  ptr->is_mut = match(token_kind::kw_mut);
  ptr->inner = parse_type_expr();

  ptr->span.extend_to(previous_span());
  return ptr;
}

ast::Ptr<ast::fn_type> Parser::parse_fn_type() {
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

std::vector<ast::type_param> Parser::parse_type_params() {
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

auto Parser::parse_type_param() -> ast::type_param {
  ast::type_param param;
  param.span = peek().span;

  auto name_tok = expect(token_kind::ident);
  param.name = std::string(name_tok.text);

  if (match(token_kind::colon)) {
    param.bound_or_type = parse_type_expr();
    // Heuristic: if the type is a simple named type that looks like a trait,
    // it's a bound. If it has value-like syntax, it's a value parameter type.
    // For now, we store it uniformly and let semantic analysis decide.
  }

  param.span.extend_to(previous_span());
  return param;
}

auto Parser::parse_bound() -> ast::Bound {
  ast::Bound bound;
  bound.span = peek().span;

  bound.terms.push_back(parse_bound_term());

  while (match(token_kind::plus)) {
    bound.terms.push_back(parse_bound_term());
  }

  bound.span.extend_to(previous_span());
  return bound;
}

auto Parser::parse_bound_term() -> ast::bound_term {
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

std::vector<ast::where_constraint> Parser::parse_where_clause() {
  std::vector<ast::where_constraint> constraints;

  expect(token_kind::kw_where);

  do {
    ast::where_constraint c;
    c.span = peek().span;
    c.subject = parse_type_expr();
    expect(token_kind::colon);
    c.bound_or_type = parse_type_expr();
    c.span.extend_to(previous_span());
    constraints.push_back(std::move(c));
  } while (match(token_kind::comma));

  return constraints;
}

// ==========================================================================
//  Trait declarations
// ==========================================================================

ast::Ptr<ast::trait_decl> Parser::parse_trait_decl(ast::visibility vis) {
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
      auto item_vis = parse_optional_visibility();

      if (at(token_kind::kw_def) || peek().is_func_modifier()) {
        auto mods = parse_func_modifiers();
        if (at(token_kind::kw_def)) {
          decl->items.push_back(parse_func_decl(item_vis, std::move(mods)));
        } else {
          emit_unexpected("`def` in trait body");
          synchronize_to_newline();
        }
      } else if (at(token_kind::kw_static)) {
        decl->items.push_back(parse_static_decl(item_vis));
      } else if (at(token_kind::kw_type)) {
        // Associated type declaration.
        advance(); // consume `type`
        [[maybe_unused]] auto aname_tok = expect(token_kind::ident);
        // TODO: build associated_type_decl node properly
        // For now, skip to newline.
        if (match(token_kind::eq)) {
          [[maybe_unused]] auto default_type = parse_type_expr();
        }
        expect_newline();
      } else if (at(token_kind::newline)) {
        advance();
      } else {
        emit_unexpected("a trait member (function, type, or static)");
        synchronize_to_newline();
      }
    }
    expect_block_end("trait");
  }

  decl->span = start.merge(previous_span());
  return decl;
}

// ==========================================================================
//  Concept declarations
// ==========================================================================

ast::Ptr<ast::concept_decl> Parser::parse_concept_decl(ast::visibility vis) {
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
      expect(token_kind::rbracket);
      param.is_higher_kinded = true;
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
      auto subject = parse_type_expr();
      if (at(token_kind::colon)) {
        advance();
        constraint.subject = std::move(subject);
        constraint.bound_or_expr = parse_type_expr();
      } else {
        // Value constraint — the subject is actually an expression.
        constraint.bound_or_expr = std::move(subject);
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

ast::Ptr<ast::impl_decl> Parser::parse_impl_decl() {
  auto decl = ast::make<ast::impl_decl>();
  auto start = peek().span;

  expect(token_kind::kw_impl);

  // Optional type parameters.
  if (at(token_kind::lbracket)) {
    decl->type_params = parse_type_params();
  }

  decl->trait_type = parse_named_type();
  expect_with_context(token_kind::kw_for, "in `impl Trait for Type`");
  decl->for_type = parse_type_expr();

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

      auto item_vis = parse_optional_visibility();

      if (at(token_kind::kw_def) || peek().is_func_modifier()) {
        auto mods = parse_func_modifiers();
        if (at(token_kind::kw_def)) {
          decl->items.push_back(parse_func_decl(item_vis, std::move(mods)));
        } else {
          emit_unexpected("`def` in impl body");
          synchronize_to_newline();
        }
      } else if (at(token_kind::kw_static)) {
        decl->items.push_back(parse_static_decl(item_vis));
      } else if (at(token_kind::kw_type)) {
        // Associated type definition.
        advance(); // consume `type`
        [[maybe_unused]] auto assoc_name_tok = expect(token_kind::ident);
        expect(token_kind::eq);
        [[maybe_unused]] auto assoc_type = parse_type_expr();
        expect_newline();
        // TODO: build associated_type_def node
      } else if (at(token_kind::newline)) {
        advance();
      } else {
        emit_unexpected("an impl member (function, type, or static)");
        synchronize_to_newline();
      }
    }
    expect_block_end("impl");
  }

  decl->span = start.merge(previous_span());
  return decl;
}

// ==========================================================================
//  Function declarations
// ==========================================================================

auto Parser::parse_func_modifiers() -> ast::func_modifiers {
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
    default:
      keep_going = false;
      break;
    }
  }

  return mods;
}

ast::Ptr<ast::func_decl> Parser::parse_func_decl(ast::visibility vis,
                                                 ast::func_modifiers mods) {
  auto decl = ast::make<ast::func_decl>();
  auto start = peek().span;
  decl->visibility = vis;
  decl->modifiers = std::move(mods);

  expect(token_kind::kw_def);
  auto name_tok = expect(token_kind::ident);
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

  // Optional contract clauses.
  decl->contracts = parse_contract_clauses();

  // Function body.
  auto body = parse_body("function");
  decl->body_expr = std::move(body.inline_expr);
  decl->body_stmts = std::move(body.stmts);

  decl->span = start.merge(previous_span());
  return decl;
}

std::vector<ast::Param> Parser::parse_param_list() {
  std::vector<ast::Param> params;

  do {
    if (at(token_kind::rparen)) {
      break;
}
    params.push_back(parse_param());
  } while (match(token_kind::comma));

  return params;
}

auto Parser::parse_param() -> ast::Param {
  ast::Param param;
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

std::vector<ast::contract_clause> Parser::parse_contract_clauses() {
  std::vector<ast::contract_clause> clauses;

  while (at_any(token_kind::kw_pre, token_kind::kw_post)) {
    clauses.push_back(parse_contract_clause());
  }

  return clauses;
}

auto Parser::parse_contract_clause() -> ast::contract_clause {
  ast::contract_clause clause;
  clause.span = peek().span;

  if (at(token_kind::kw_pre)) {
    clause.is_pre = true;
    advance();
  } else {
    clause.is_pre = false;
    expect(token_kind::kw_post);
  }

  clause.condition = parse_expr();

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

ast::Ptr<ast::static_decl> Parser::parse_static_decl(ast::visibility vis) {
  auto decl = ast::make<ast::static_decl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(token_kind::kw_static);

  // Disambiguate: static if, static assert, static for, static binding.
  if (at(token_kind::kw_if)) {
    // static if expr: block [else: block]
    decl->decl_kind = ast::StaticDeclKind::ConditionalCompilation;
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
    decl->decl_kind = ast::StaticDeclKind::Assert;
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
    decl->for_iterable = parse_expr();

    if (match(token_kind::kw_if)) {
      decl->for_guard = parse_expr();
    }

    if (at(token_kind::fat_arrow)) {
      // Inline form.
      decl->decl_kind = ast::StaticDeclKind::ForInline;
      advance();
      decl->for_yield = parse_expr();
      expect_newline();
    } else {
      // Block form.
      decl->decl_kind = ast::StaticDeclKind::ForBlock;
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
    // static binding: static Name [: Type] = expr
    decl->decl_kind = ast::StaticDeclKind::Binding;
    auto name_tok = expect(token_kind::ident);
    decl->name = std::string(name_tok.text);

    if (match(token_kind::colon)) {
      decl->type_annotation = parse_type_expr();
    }

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

ast::Ptr<ast::Node> Parser::parse_stmt() {
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
    return parse_use_decl(ast::Visibility::Default);
  case token_kind::kw_type:
    return parse_type_decl(ast::Visibility::Default);
  case token_kind::kw_static:
    return parse_static_decl(ast::Visibility::Default);

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
    if (at(token_kind::kw_type))
      return parse_type_decl(vis);
    if (at(token_kind::kw_static))
      return parse_static_decl(vis);
    if (at(token_kind::kw_use))
      return parse_use_decl(vis);
    emit_unexpected("a declaration after visibility modifier");
    synchronize_to_newline();
    return make_error_node(peek().span);
  }

  case token_kind::kw_def:
    return parse_func_decl(ast::Visibility::Default, ast::func_modifiers{});

  case token_kind::kw_pure:
  case token_kind::kw_async:
  case token_kind::kw_machine: {
    auto mods = parse_func_modifiers();
    if (at(token_kind::kw_def)) {
      return parse_func_decl(ast::Visibility::Default, std::move(mods));
    }
    emit_unexpected("`def` after function modifiers");
    synchronize_to_newline();
    return make_error_node(peek().span);
  }

  default:
    // Expression or assignment statement.
    return parse_expr_or_assign_stmt();
  }
}

ast::Ptr<ast::let_stmt> Parser::parse_let_stmt() {
  auto stmt = ast::make<ast::let_stmt>();
  auto start = peek().span;

  expect(token_kind::kw_let);
  stmt->pattern = parse_pattern();

  if (match(token_kind::colon)) {
    stmt->type_annotation = parse_type_expr();
  }

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

ast::Ptr<ast::var_stmt> Parser::parse_var_stmt() {
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

ast::Ptr<ast::return_stmt> Parser::parse_return_stmt() {
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

ast::Ptr<ast::if_stmt> Parser::parse_if_stmt() {
  auto stmt = ast::make<ast::if_stmt>();
  auto start = peek().span;

  // First `if` branch.
  expect(token_kind::kw_if);
  {
    ast::if_branch branch;
    branch.span = start;
    branch.condition = parse_expr();
    auto body = parse_body("if");
    if (body.inline_expr) {
      auto es = ast::make<ast::expr_stmt>();
      es->expr = std::move(body.inline_expr);
      branch.body.push_back(std::move(es));
    } else {
      branch.body = std::move(body.stmts);
    }
    branch.span.extend_to(previous_span());
    stmt->branches.push_back(std::move(branch));
  }

  // `elif` branches.
  while (at(token_kind::kw_elif)) {
    advance();
    ast::if_branch branch;
    branch.span = previous_span();
    branch.condition = parse_expr();
    auto body = parse_body("elif");
    if (body.inline_expr) {
      auto es = ast::make<ast::expr_stmt>();
      es->expr = std::move(body.inline_expr);
      branch.body.push_back(std::move(es));
    } else {
      branch.body = std::move(body.stmts);
    }
    branch.span.extend_to(previous_span());
    stmt->branches.push_back(std::move(branch));
  }

  // Optional `else` branch.
  if (at(token_kind::kw_else)) {
    advance();
    auto body = parse_body("else");
    if (body.inline_expr) {
      auto es = ast::make<ast::expr_stmt>();
      es->expr = std::move(body.inline_expr);
      stmt->else_body.push_back(std::move(es));
    } else {
      stmt->else_body = std::move(body.stmts);
    }
  }

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::while_stmt> Parser::parse_while_stmt() {
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

  auto body = parse_body("while");
  if (body.inline_expr) {
    auto es = ast::make<ast::expr_stmt>();
    es->expr = std::move(body.inline_expr);
    stmt->body.push_back(std::move(es));
  } else {
    stmt->body = std::move(body.stmts);
  }

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::for_stmt> Parser::parse_for_stmt() {
  auto stmt = ast::make<ast::for_stmt>();
  auto start = peek().span;

  expect(token_kind::kw_for);
  stmt->patterns = parse_for_vars();
  expect_with_context(token_kind::kw_in, "in `for ... in ...`");
  stmt->iterable = parse_expr();

  if (match(token_kind::kw_if)) {
    stmt->guard = parse_expr();
  }

  auto body = parse_body("for");
  if (body.inline_expr) {
    auto es = ast::make<ast::expr_stmt>();
    es->expr = std::move(body.inline_expr);
    stmt->body.push_back(std::move(es));
  } else {
    stmt->body = std::move(body.stmts);
  }

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::match_stmt> Parser::parse_match_stmt() {
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

ast::Ptr<ast::crew_stmt> Parser::parse_crew_stmt() {
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

  auto body = parse_body("crew");
  if (body.inline_expr) {
    auto es = ast::make<ast::expr_stmt>();
    es->expr = std::move(body.inline_expr);
    stmt->body.push_back(std::move(es));
  } else {
    stmt->body = std::move(body.stmts);
  }

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::asm_stmt> Parser::parse_asm_stmt() {
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

ast::Ptr<ast::splice_stmt> Parser::parse_splice_stmt() {
  auto stmt = ast::make<ast::splice_stmt>();
  auto start = peek().span;

  expect(token_kind::tilde);
  stmt->expr = parse_expr();
  expect_newline();

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::Node> Parser::parse_expr_or_assign_stmt() {
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

ast::Ptr<ast::Expr> Parser::parse_expr() { return parse_pipe_expr(); }

ast::Ptr<ast::Expr> Parser::parse_pipe_expr() {
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
    bin->op = ast::BinaryOp::Pipe;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    lhs = std::move(bin);
  }

  return lhs;
}

ast::Ptr<ast::Expr> Parser::parse_or_expr() {
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
    bin->op = ast::BinaryOp::Or;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    lhs = std::move(bin);
  }

  return lhs;
}

ast::Ptr<ast::Expr> Parser::parse_and_expr() {
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
    bin->op = ast::BinaryOp::And;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    lhs = std::move(bin);
  }

  return lhs;
}

ast::Ptr<ast::Expr> Parser::parse_not_expr() {
  if (at(token_kind::kw_not)) {
    auto op_tok = advance();
    auto operand = parse_not_expr();
    if (!operand) {
      emit_unexpected("an expression after `not`");
      operand = make_error_expr(op_tok.span);
    }

    auto unary = ast::make<ast::unary_expr>();
    unary->span = op_tok.span.merge(operand->span);
    unary->op = ast::UnaryOp::Not;
    unary->operand = std::move(operand);
    return unary;
  }

  return parse_cmp_expr();
}

ast::Ptr<ast::Expr> Parser::parse_cmp_expr() {
  auto lhs = parse_add_expr();
  if (!lhs) {
    return nullptr;
}

  auto maybe_op = token_to_cmp_op();
  if (maybe_op) {
    auto op_tok = advance();
    auto op = *maybe_op;

    // Handle `not in` (two-token operator).
    if (op_tok.kind == token_kind::kw_not) {
      if (at(token_kind::kw_in)) {
        advance(); // consume `in`
        op = ast::BinaryOp::NotIn;
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
        op = ast::BinaryOp::NotIn;
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

ast::Ptr<ast::Expr> Parser::parse_add_expr() {
  auto lhs = parse_mul_expr();
  if (!lhs) {
    return nullptr;
}

  while (true) {
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

ast::Ptr<ast::Expr> Parser::parse_mul_expr() {
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

ast::Ptr<ast::Expr> Parser::parse_unary_expr() {
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
    unary->op = ast::UnaryOp::Neg;
    unary->operand = std::move(operand);
    return unary;
  }

  case token_kind::tilde: {
    // At expression depth 0, `~(expr)` is a splice, `~anything_else`
    // is bitwise complement. We check for `(` to disambiguate.
    if (peek_at(1).is(token_kind::lparen)) {
      return parse_splice_expr_inner();
    }
    if (peek_at(1).is(token_kind::ident)) {
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
    unary->op = ast::UnaryOp::BitNot;
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
    unary->op = ast::UnaryOp::Deref;
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
    unary->op = is_mut ? ast::UnaryOp::AddrOfMut : ast::UnaryOp::AddrOf;
    unary->operand = std::move(operand);
    return unary;
  }

  default:
    return parse_postfix_expr();
  }
}

ast::Ptr<ast::Expr> Parser::parse_postfix_expr() {
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

    auto suffix = parse_postfix_suffix(std::move(base));
    if (!suffix) {
      // This shouldn't happen given the guard above, but be safe.
      return make_error_expr(previous_span(), "postfix expression");
    }
    base = std::move(suffix);
  }

  return base;
}

ast::Ptr<ast::Expr> Parser::parse_postfix_suffix(ast::Ptr<ast::Expr> base) {
  switch (current()) {
  case token_kind::dot: {
    advance(); // consume `.`
    if (at(token_kind::ident)) {
      auto field_tok = advance();
      auto field = ast::make<ast::field_expr>();
      field->span = base->span.merge(field_tok.span);
      field->object = std::move(base);
      field->field_name = std::string(field_tok.text);

      // Check for generic method: `.method[T, U]`
      if (at(token_kind::lbracket)) {
        advance();
        while (!at(token_kind::rbracket) && !at_eof()) {
          auto arg = parse_type_expr();
          if (arg)
            field->generic_args.push_back(std::move(arg));
          if (!match(token_kind::comma))
            break;
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

ast::Ptr<ast::Expr> Parser::parse_primary_expr() {
  switch (current()) {
  // Literals.
  case token_kind::int_lit:
  case token_kind::float_lit:
  case token_kind::string_lit:
  case token_kind::char_lit:
  case token_kind::kw_true:
  case token_kind::kw_false:
  case token_kind::kw_unit:
    return parse_literal_expr();

  // Identifiers and module paths.
  case token_kind::ident:
    return parse_ident_or_path_expr();

  // Parenthesized expr, tuple, or grouped.
  case token_kind::lparen:
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

  // Par expression.
  case token_kind::kw_par:
    return parse_par_expr();

  // Race expression.
  case token_kind::kw_race:
    return parse_race_expr();

  // On expression.
  case token_kind::kw_on:
    return parse_on_expr();

  // Quote expression.
  case token_kind::backtick:
    return parse_quote_expr();

  // Static expression.
  case token_kind::kw_static:
    return parse_static_expr();

  default:
    return nullptr;
  }
}

ast::Ptr<ast::Expr> Parser::parse_literal_expr() {
  auto tok = advance();
  auto lit = ast::make<ast::literal_expr>();
  lit->span = tok.span;
  lit->lit_kind = tok.kind;
  lit->value = std::string(tok.text);
  return lit;
}

ast::Ptr<ast::Expr> Parser::parse_ident_or_path_expr() {
  // Check if this looks like a lambda: `ident => ...`
  if (peek_at(1).is(token_kind::fat_arrow)) {
    return parse_lambda_expr();
  }

  auto tok = advance();
  auto ident = ast::make<ast::ident_expr>();
  ident->span = tok.span;
  ident->name = std::string(tok.text);

  // Check for module path: `a.b.c` where next is `.` followed by ident
  // but NOT followed by `(` or `[` (those are field access/method call).
  if (at(token_kind::dot) && peek_at(1).is(token_kind::ident) &&
      !peek_at(2).is(token_kind::lparen) &&
      !peek_at(2).is(token_kind::lbracket)) {
    // Might be a module path. But field access `obj.field` is more common.
    // We return the ident and let postfix parsing handle `.field`.
  }

  return ident;
}

ast::Ptr<ast::Expr> Parser::parse_paren_expr() {
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

ast::Ptr<ast::Expr> Parser::parse_bracket_expr() {
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

ast::Ptr<ast::Expr> Parser::parse_brace_expr() {
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

ast::Ptr<ast::Expr> Parser::parse_lambda_expr() {
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

ast::Ptr<ast::match_expr> Parser::parse_match_expr() {
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

auto Parser::parse_match_arm() -> ast::match_arm {
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
    expect_newline();
  }

  arm.span.extend_to(previous_span());
  return arm;
}

ast::Ptr<ast::if_expr> Parser::parse_if_expr() {
  auto iexpr = ast::make<ast::if_expr>();
  auto start = peek().span;

  expect(token_kind::kw_if);

  // First branch.
  {
    ast::if_branch branch;
    branch.span = start;
    branch.condition = parse_expr();
    auto body = parse_body("if");
    if (body.inline_expr) {
      auto es = ast::make<ast::expr_stmt>();
      es->expr = std::move(body.inline_expr);
      branch.body.push_back(std::move(es));
    } else {
      branch.body = std::move(body.stmts);
    }
    branch.span.extend_to(previous_span());
    iexpr->branches.push_back(std::move(branch));
  }

  // elif branches.
  while (at(token_kind::kw_elif)) {
    advance();
    ast::if_branch branch;
    branch.span = previous_span();
    branch.condition = parse_expr();
    auto body = parse_body("elif");
    if (body.inline_expr) {
      auto es = ast::make<ast::expr_stmt>();
      es->expr = std::move(body.inline_expr);
      branch.body.push_back(std::move(es));
    } else {
      branch.body = std::move(body.stmts);
    }
    branch.span.extend_to(previous_span());
    iexpr->branches.push_back(std::move(branch));
  }

  // `else` is required for if-expressions (they must produce a value).
  if (at(token_kind::kw_else)) {
    advance();
    auto body = parse_body("else");
    if (body.inline_expr) {
      auto es = ast::make<ast::expr_stmt>();
      es->expr = std::move(body.inline_expr);
      iexpr->else_body.push_back(std::move(es));
    } else {
      iexpr->else_body = std::move(body.stmts);
    }
  }

  iexpr->span = start.merge(previous_span());
  return iexpr;
}

ast::Ptr<ast::for_expr> Parser::parse_for_expr() {
  auto fexpr = ast::make<ast::for_expr>();
  auto start = peek().span;

  expect(token_kind::kw_for);

  // Parse one or more iteration clauses.
  do {
    ast::for_expr::IterClause clause;
    auto pats = parse_for_vars();
    for (auto &p : pats) {
      clause.patterns.push_back(std::move(p));
    }
    expect_with_context(token_kind::kw_in, "in `for ... in ...`");
    clause.iterable = parse_expr();
    fexpr->clauses.push_back(std::move(clause));
  } while (match(token_kind::comma) && at(token_kind::ident));

  if (match(token_kind::kw_if)) {
    fexpr->guard = parse_expr();
  }

  expect(token_kind::fat_arrow);
  fexpr->yield_expr = parse_expr();

  fexpr->span = start.merge(previous_span());
  return fexpr;
}

ast::Ptr<ast::await_expr> Parser::parse_await_expr() {
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

ast::Ptr<ast::par_expr> Parser::parse_par_expr() {
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

ast::Ptr<ast::race_expr> Parser::parse_race_expr() {
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

ast::Ptr<ast::on_expr> Parser::parse_on_expr() {
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
    auto body = parse_body("on");
    if (body.inline_expr) {
      auto es = ast::make<ast::expr_stmt>();
      es->expr = std::move(body.inline_expr);
      oexpr->body.push_back(std::move(es));
    } else {
      oexpr->body = std::move(body.stmts);
    }
  }

  oexpr->span = start.merge(previous_span());
  return oexpr;
}

ast::Ptr<ast::block_expr> Parser::parse_block_expr() {
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

ast::Ptr<ast::quote_expr> Parser::parse_quote_expr() {
  auto qexpr = ast::make<ast::quote_expr>();
  auto start = peek().span;

  expect(token_kind::backtick);

  // Collect tokens until the closing backtick.
  // Handle nested backticks by tracking depth.
  int depth = 1;
  bool has_paren = match(token_kind::lparen);

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
    } else if (at(token_kind::rparen) && has_paren && depth == 1) {
      advance(); // consume `)`
      has_paren = false;
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
  return qexpr;
}

ast::Ptr<ast::splice_expr> Parser::parse_splice_expr_inner() {
  auto sexpr = ast::make<ast::splice_expr>();
  auto start = peek().span;

  expect(token_kind::tilde);

  if (at(token_kind::lparen)) {
    advance(); // consume `(`
    sexpr->operand = parse_expr();
    expect(token_kind::rparen);
  } else if (at(token_kind::ident)) {
    auto tok = advance();
    auto ident = ast::make<ast::ident_expr>();
    ident->span = tok.span;
    ident->name = std::string(tok.text);
    sexpr->operand = std::move(ident);
  } else {
    emit_unexpected("an expression or identifier after `~`");
    sexpr->operand = make_error_expr(start);
  }

  sexpr->span = start.merge(previous_span());
  return sexpr;
}

ast::Ptr<ast::static_expr> Parser::parse_static_expr() {
  auto sexpr = ast::make<ast::static_expr>();
  auto start = peek().span;

  expect(token_kind::kw_static);
  sexpr->operand = parse_expr();

  sexpr->span = start.merge(previous_span());
  return sexpr;
}

// ==========================================================================
//  Call arguments
// ==========================================================================

std::vector<ast::call_arg> Parser::parse_call_args() {
  std::vector<ast::call_arg> args;

  do {
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
  } while (match(token_kind::comma));

  return args;
}

// ==========================================================================
//  Patterns
// ==========================================================================

ast::Ptr<ast::Pattern> Parser::parse_pattern() {
  auto pat = parse_or_pattern();
  if (!pat)
    return make_error_pattern(peek().span);

  // Optional alias: `pattern as name`
  if (at(token_kind::kw_as)) {
    advance();
    auto name_tok = expect(token_kind::ident);
    // Wrap in an or_pattern with alias... for now, just annotate the
    // binding name on a wrapping group pattern.
    // TODO: proper pattern alias support
    auto group = ast::make<ast::group_pattern>();
    group->span = pat->span.merge(name_tok.span);
    group->inner = std::move(pat);
    return group;
  }

  return pat;
}

ast::Ptr<ast::Pattern> Parser::parse_or_pattern() {
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

ast::Ptr<ast::Pattern> Parser::parse_atomic_pattern() {
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

  case token_kind::lparen:
    return parse_paren_pattern();

  case token_kind::lbrace:
    return parse_brace_pattern();

  case token_kind::lbracket:
    return parse_bracket_pattern();

  case token_kind::kw_some:
  case token_kind::kw_ok:
  case token_kind::kw_err:
    return parse_option_result_pattern();

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

ast::Ptr<ast::Pattern> Parser::parse_wildcard_pattern() {
  auto tok = advance();
  auto wc = ast::make<ast::wildcard_pattern>();
  wc->span = tok.span;
  return wc;
}

ast::Ptr<ast::Pattern> Parser::parse_literal_pattern() {
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

ast::Ptr<ast::Pattern> Parser::parse_ident_or_constructor_pattern() {
  auto tok = advance();
  auto start = tok.span;

  // Constructor: `Name(patterns...)`
  if (at(token_kind::lparen)) {
    advance(); // consume `(`
    auto ctor = ast::make<ast::constructor_pattern>();
    ctor->span = start;
    ctor->name = std::string(tok.text);

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
    ctor->span.extend_to(previous_span());
    return ctor;
  }

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

ast::Ptr<ast::Pattern> Parser::parse_paren_pattern() {
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

ast::Ptr<ast::Pattern> Parser::parse_brace_pattern() {
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

ast::Ptr<ast::Pattern> Parser::parse_bracket_pattern() {
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

ast::Ptr<ast::Pattern> Parser::parse_option_result_pattern() {
  auto start = peek().span;
  auto kw = advance();

  expect(token_kind::lparen);
  auto inner = parse_pattern();
  if (!inner)
    inner = make_error_pattern(peek().span);
  expect(token_kind::rparen);

  if (kw.kind == token_kind::kw_some) {
    auto pat = ast::make<ast::option_pattern>();
    pat->span = start.merge(previous_span());
    pat->option_kind = ast::OptionResultKind::Some;
    pat->inner = std::move(inner);
    return pat;
  }

  auto pat = ast::make<ast::result_pattern>();
  pat->span = start.merge(previous_span());
  pat->result_kind = (kw.kind == token_kind::kw_ok)
                         ? ast::OptionResultKind::Ok
                         : ast::OptionResultKind::Err;
  pat->inner = std::move(inner);
  return pat;
}

ast::Ptr<ast::Pattern> Parser::parse_ref_pattern() {
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

std::vector<ast::Ptr<ast::Pattern>> Parser::parse_for_vars() {
  std::vector<ast::Ptr<ast::Pattern>> patterns;

  patterns.push_back(parse_pattern());

  while (match(token_kind::comma)) {
    // Check if the next thing is `in` — if so, the comma was part of
    // the for syntax, not another variable.
    if (at(token_kind::kw_in))
      break;
    patterns.push_back(parse_pattern());
  }

  return patterns;
}

// ==========================================================================
//  Operator conversion helpers
// ==========================================================================

std::optional<ast::AssignOp>
Parser::token_to_assign_op(token_kind kind) noexcept {
  switch (kind) {
  case token_kind::eq:
    return ast::AssignOp::Assign;
  case token_kind::plus_eq:
    return ast::AssignOp::AddAssign;
  case token_kind::minus_eq:
    return ast::AssignOp::SubAssign;
  case token_kind::star_eq:
    return ast::AssignOp::MulAssign;
  case token_kind::slash_eq:
    return ast::AssignOp::DivAssign;
  case token_kind::percent_eq:
    return ast::AssignOp::ModAssign;
  case token_kind::amp_eq:
    return ast::AssignOp::AndAssign;
  case token_kind::pipe_eq:
    return ast::AssignOp::OrAssign;
  case token_kind::caret_eq:
    return ast::AssignOp::XorAssign;
  case token_kind::lt_lt_eq:
    return ast::AssignOp::ShlAssign;
  case token_kind::gt_gt_eq:
    return ast::AssignOp::ShrAssign;
  case token_kind::plus_percent_eq:
    return ast::AssignOp::AddWrapAssign;
  case token_kind::minus_percent_eq:
    return ast::AssignOp::SubWrapAssign;
  case token_kind::star_percent_eq:
    return ast::AssignOp::MulWrapAssign;
  case token_kind::plus_pipe_eq:
    return ast::AssignOp::AddSatAssign;
  case token_kind::minus_pipe_eq:
    return ast::AssignOp::SubSatAssign;
  case token_kind::star_pipe_eq:
    return ast::AssignOp::MulSatAssign;
  default:
    return std::nullopt;
  }
}

std::optional<ast::BinaryOp> Parser::token_to_cmp_op() {
  switch (current()) {
  case token_kind::eq_eq:
    return ast::BinaryOp::EqEq;
  case token_kind::bang_eq:
    return ast::BinaryOp::BangEq;
  case token_kind::lt:
    return ast::BinaryOp::Lt;
  case token_kind::lt_eq:
    return ast::BinaryOp::LtEq;
  case token_kind::gt:
    return ast::BinaryOp::Gt;
  case token_kind::gt_eq:
    return ast::BinaryOp::GtEq;
  case token_kind::kw_in:
    return ast::BinaryOp::In;
  case token_kind::kw_not:
    // `not in` — peek ahead.
    if (peek_at(1).is(token_kind::kw_in)) {
      return ast::BinaryOp::NotIn;
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

std::optional<ast::BinaryOp> Parser::token_to_add_op(token_kind kind) noexcept {
  switch (kind) {
  case token_kind::plus:
    return ast::BinaryOp::Add;
  case token_kind::minus:
    return ast::BinaryOp::Sub;
  case token_kind::plus_percent:
    return ast::BinaryOp::AddWrap;
  case token_kind::minus_percent:
    return ast::BinaryOp::SubWrap;
  case token_kind::plus_pipe:
    return ast::BinaryOp::AddSat;
  case token_kind::minus_pipe:
    return ast::BinaryOp::SubSat;
  default:
    return std::nullopt;
  }
}

std::optional<ast::BinaryOp> Parser::token_to_mul_op(token_kind kind) noexcept {
  switch (kind) {
  case token_kind::star:
    return ast::BinaryOp::Mul;
  case token_kind::slash:
    return ast::BinaryOp::Div;
  case token_kind::percent:
    return ast::BinaryOp::Mod;
  case token_kind::star_percent:
    return ast::BinaryOp::MulWrap;
  case token_kind::star_pipe:
    return ast::BinaryOp::MulSat;
  default:
    return std::nullopt;
  }
}

} // namespace kira
