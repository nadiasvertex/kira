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
static ast::Ptr<ast::Expr> make_error_expr(Span span, std::string desc = "") {
  auto node = ast::make<ast::ErrorExpr>(span, std::move(desc));
  return node;
}

static ast::Ptr<ast::Pattern> make_error_pattern(Span span) {
  return ast::make<ast::ErrorPattern>(span);
}

static ast::Ptr<ast::Node> make_error_node(Span span, std::string desc = "") {
  return ast::make<ast::ErrorNode>(span, std::move(desc));
}

// ==========================================================================
//  Token stream navigation & error recovery
// ==========================================================================

bool Parser::expect_newline() {
  if (match(TokenKind::Newline)) return true;

  // At EOF or DEDENT, a newline is implicitly present.
  if (at_any(TokenKind::Eof, TokenKind::Dedent)) return true;

  // Missing newline — emit a helpful diagnostic.
  auto span = previous_span();
  emit(Diagnostic(DiagnosticLevel::Error,
                  "expected end of line after this",
                  file_id_)
           .with_label(span, "this should be followed by a new line")
           .with_help("Each statement in Kira goes on its own line. "
                      "If you need to continue a long expression across "
                      "lines, wrap it in parentheses."));

  // Recovery: skip to the next newline or statement-starting token.
  while (!at_eof() && !at(TokenKind::Newline) && !at(TokenKind::Dedent) &&
         !peek().can_start_stmt()) {
    advance();
  }
  match(TokenKind::Newline);
  return false;
}

Token Parser::expect(TokenKind expected) {
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
    message = std::format("expected {} but found {}",
                          expected_desc,
                          token_kind_name(found.kind));
  }

  auto diag = Diagnostic(DiagnosticLevel::Error, message, file_id_)
      .with_label(found.span, std::format("expected {} here", expected_desc));

  // Context-specific help for common mistakes.
  if (expected == TokenKind::RParen && found.is(TokenKind::Newline)) {
    diag.with_help("It looks like a closing `)` is missing. Check that "
                   "every `(` has a matching `)` on the same line or "
                   "within the same indented block.");
  } else if (expected == TokenKind::RBracket && found.is(TokenKind::Newline)) {
    diag.with_help("It looks like a closing `]` is missing.");
  } else if (expected == TokenKind::RBrace && found.is(TokenKind::Newline)) {
    diag.with_help("It looks like a closing `}` is missing.");
  } else if (expected == TokenKind::Colon) {
    diag.with_help("In Kira, a `:` introduces the body of a block. "
                   "For example: `if condition:` or `def name():` — "
                   "the `:` goes right before the body.");
    diag.with_fix("add `:`", found.span, ": ");
  } else if (expected == TokenKind::Eq) {
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
    if (!at_any(TokenKind::Indent, TokenKind::Dedent, TokenKind::Newline)) {
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

Token Parser::expect_with_context(TokenKind expected, std::string_view context) {
  if (at(expected)) {
    return advance();
  }

  auto expected_desc = token_kind_description(expected);
  auto found = peek();

  std::string message = std::format("expected {} {} but found {}",
                                    expected_desc,
                                    context,
                                    token_kind_name(found.kind));

  auto diag = Diagnostic(DiagnosticLevel::Error, message, file_id_)
      .with_label(found.span, std::format("expected {} here", expected_desc));

  emit(std::move(diag));

  if (!at_recovery_point() && !at_eof() &&
      !at_any(TokenKind::Indent, TokenKind::Dedent, TokenKind::Newline)) {
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
    message = std::format("expected {} but found {}",
                          expected_description,
                          token_kind_name(found.kind));
  }

  emit(Diagnostic(DiagnosticLevel::Error, message, file_id_)
           .with_label(found.span,
                       std::format("expected {}", expected_description)));
}

bool Parser::at_recovery_point() const noexcept {
  return peek().can_start_stmt() || at_any(TokenKind::Eof, TokenKind::Dedent);
}

void Parser::synchronize() {
  // Skip tokens until we find something that looks like the start of
  // a new statement or declaration. We track indent/dedent balance so
  // we don't skip past the end of the current block.
  int depth = 0;

  while (!at_eof()) {
    if (at(TokenKind::Newline)) {
      advance();
      // After a newline, check if the next token starts a new statement.
      skip_newlines();
      if (peek().can_start_stmt() || at_eof() || at(TokenKind::Dedent)) {
        return;
      }
      continue;
    }

    if (at(TokenKind::Indent)) {
      ++depth;
      advance();
      continue;
    }

    if (at(TokenKind::Dedent)) {
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
  while (!at_eof() && !at_any(TokenKind::Newline, TokenKind::Dedent)) {
    advance();
  }
  match(TokenKind::Newline);
}

void Parser::skip_block() {
  // Skip a NEWLINE INDENT ... DEDENT sequence.
  skip_newlines();
  if (!match(TokenKind::Indent)) return;

  int depth = 1;
  while (!at_eof() && depth > 0) {
    if (at(TokenKind::Indent)) ++depth;
    else if (at(TokenKind::Dedent)) --depth;
    advance();
  }
}

// ==========================================================================
//  Block parsing helpers
// ==========================================================================

bool Parser::expect_block_start(std::string_view construct_name) {
  if (!at(TokenKind::Colon)) {
    auto span = previous_span();
    emit(Diagnostic(DiagnosticLevel::Error,
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
    if (at(TokenKind::Newline) &&
        peek_at(1).is(TokenKind::Indent)) {
      // Proceed — the block is clearly intended.
    } else {
      return false;
    }
  } else {
    advance();  // consume `:`
  }

  // Now expect NEWLINE INDENT for block form.
  // (Caller handles inline form separately.)
  skip_newlines();
  if (!match(TokenKind::Indent)) {
    // This might be an inline body — let the caller handle it.
    return false;
  }

  return true;
}

bool Parser::expect_block_end(std::string_view construct_name) {
  if (match(TokenKind::Dedent)) return true;

  // Missing DEDENT — emit diagnostic.
  emit(Diagnostic(DiagnosticLevel::Error,
                  std::format("expected end of {} block (dedent)", construct_name),
                  file_id_)
           .with_label(peek().span, "expected the block to end here")
           .with_note("This usually means the indentation is inconsistent. "
                      "Check that all lines in this block are indented "
                      "by the same amount."));
  return false;
}

template <typename T>
std::vector<ast::Ptr<T>> Parser::parse_block(
    std::string_view construct_name,
    std::function<ast::Ptr<T>()> parse_item) {
  std::vector<ast::Ptr<T>> items;

  while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
    skip_newlines();
    if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;

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
template std::vector<ast::Ptr<ast::Node>> Parser::parse_block<ast::Node>(
    std::string_view, std::function<ast::Ptr<ast::Node>()>);

Parser::BodyResult Parser::parse_body(std::string_view construct_name) {
  BodyResult result;

  if (!at(TokenKind::Colon)) {
    auto span = previous_span();
    emit(Diagnostic(DiagnosticLevel::Error,
                    std::format("expected `:` to start the body of this {}",
                                construct_name),
                    file_id_)
             .with_label(span, "expected `:` after this")
             .with_help(std::format(
                 "In Kira, the body of a {} is introduced by `:`. "
                 "For a single expression: `{}: expr`. "
                 "For a block of statements: `{}:` followed by an "
                 "indented block on the next line.",
                 construct_name, construct_name, construct_name)));
    return result;
  }
  advance();  // consume `:`

  // Check for block form: NEWLINE INDENT ...
  if (at(TokenKind::Newline)) {
    skip_newlines();
    if (at(TokenKind::Indent)) {
      advance();  // consume INDENT
      result.is_block = true;

      while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
        skip_newlines();
        if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;

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
    emit(Diagnostic(DiagnosticLevel::Error,
                    std::format("expected an indented block after `:` for this {}",
                                construct_name),
                    file_id_)
             .with_label(previous_span(), "the `:` is here")
             .with_help(
                 "After `:` and a new line, the next line should be "
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
std::vector<T> Parser::parse_delimited_list(
    TokenKind open, TokenKind close,
    std::string_view construct_name,
    std::function<std::optional<T>()> parse_element) {
  std::vector<T> items;

  auto open_tok = expect(open);
  auto open_span = open_tok.span;

  while (!at(close) && !at_eof()) {
    skip_newlines();

    if (at(close)) break;

    auto elem = parse_element();
    if (elem) {
      items.push_back(std::move(*elem));
    } else {
      // Skip to the next comma or closing delimiter.
      while (!at_any(TokenKind::Comma, TokenKind::Eof) && !at(close)) {
        if (at_any(TokenKind::Newline, TokenKind::Dedent)) break;
        advance();
      }
    }

    skip_newlines();

    // Expect comma or closing delimiter.
    if (at(close)) break;
    if (!match(TokenKind::Comma)) {
      if (at(close)) break;  // trailing item without comma is ok before close

      // Missing comma — try to give a helpful error.
      auto span = previous_span();
      emit(Diagnostic(DiagnosticLevel::Error,
                      std::format("expected `,` or {} in {}",
                                  token_kind_name(close), construct_name),
                      file_id_)
               .with_label(span, "maybe a `,` is missing here?")
               .with_fix("add a comma", Span{span.end, span.end}, ", "));
      // Don't consume — the next iteration will try to parse the element.
    }
  }

  if (!at(close)) {
    emit(Diagnostic(DiagnosticLevel::Error,
                    std::format("unclosed {} — expected {} to match the {} at line",
                                construct_name,
                                token_kind_name(close),
                                token_kind_name(open)),
                    file_id_)
             .with_label(peek().span,
                         std::format("expected {}", token_kind_name(close)))
             .with_secondary_label(open_span,
                                   std::format("this {} was opened here",
                                               token_kind_name(open)))
             .with_help("Make sure every opening delimiter has a matching "
                        "closing delimiter."));
  } else {
    advance();  // consume close
  }

  return items;
}

template <typename T>
std::vector<T> Parser::parse_comma_list(
    std::function<std::optional<T>()> parse_element,
    std::function<bool()> at_end_fn) {
  std::vector<T> items;

  while (!at_end_fn() && !at_eof()) {
    auto elem = parse_element();
    if (elem) {
      items.push_back(std::move(*elem));
    } else {
      break;
    }

    if (!match(TokenKind::Comma)) {
      break;
    }
  }

  return items;
}

// ==========================================================================
//  Visibility
// ==========================================================================

ast::Visibility Parser::parse_optional_visibility() {
  if (peek().is_visibility()) {
    auto tok = advance();
    return ast::token_to_visibility(tok.kind);
  }
  return ast::Visibility::Default;
}

// ==========================================================================
//  Module paths
// ==========================================================================

std::vector<std::string> Parser::parse_module_path() {
  std::vector<std::string> segments;

  auto ident = expect(TokenKind::Ident);
  segments.emplace_back(ident.text);

  while (at(TokenKind::Dot) && peek_at(1).is(TokenKind::Ident)) {
    advance();  // consume `.`
    auto seg = advance();  // consume ident
    segments.emplace_back(seg.text);
  }

  return segments;
}

// ==========================================================================
//  Top-level: parse_file
// ==========================================================================

ast::Ptr<ast::File> Parser::parse_file() {
  auto file = ast::make<ast::File>();
  auto file_start = peek().span;

  skip_newlines();

  // Parse module declaration.
  if (at(TokenKind::KwModule)) {
    file->module_decl = parse_module_decl();
  } else {
    emit(Diagnostic(DiagnosticLevel::Error,
                    "every Kira source file must start with a `module` declaration",
                    file_id_)
             .with_label(peek().span, "expected `module` here")
             .with_help("Add a module declaration at the top of the file, like:\n"
                        "  module my_project.my_module"));
    file->module_decl = ast::make<ast::ModuleDecl>();
    file->module_decl->has_error = true;
  }

  skip_newlines();

  // Optional `no_prelude`.
  if (at(TokenKind::KwNoPrelude)) {
    advance();
    file->no_prelude = true;
    expect_newline();
    skip_newlines();
  }

  // Parse top-level items.
  while (!at_eof()) {
    skip_newlines();
    if (at_eof()) break;

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

ast::Ptr<ast::ModuleDecl> Parser::parse_module_decl() {
  auto decl = ast::make<ast::ModuleDecl>();
  auto start = peek().span;

  expect(TokenKind::KwModule);
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
  if (at_eof()) return nullptr;

  // Parse optional visibility modifier.
  auto vis = parse_optional_visibility();

  // Dispatch on the current token.
  switch (current()) {
    case TokenKind::KwUse:
      return parse_use_decl(vis);

    case TokenKind::KwType:
      return parse_type_decl(vis);

    case TokenKind::KwTrait:
      return parse_trait_decl(vis);

    case TokenKind::KwImpl:
      if (vis != ast::Visibility::Default) {
        emit(Diagnostic(DiagnosticLevel::Warning,
                        "visibility modifiers are not meaningful on `impl` blocks",
                        file_id_)
                 .with_label(peek().span, "this `impl`")
                 .with_help("Remove the visibility modifier — `impl` blocks "
                            "inherit visibility from the trait they implement."));
      }
      return parse_impl_decl();

    case TokenKind::KwConcept:
      return parse_concept_decl(vis);

    case TokenKind::KwDef:
      return parse_func_decl(vis, ast::FuncModifiers{});

    case TokenKind::KwPure:
    case TokenKind::KwAsync:
    case TokenKind::KwMachine: {
      auto mods = parse_func_modifiers();
      if (at(TokenKind::KwDef)) {
        return parse_func_decl(vis, std::move(mods));
      }
      emit_unexpected("`def` after function modifiers");
      synchronize_to_newline();
      return make_error_node(peek().span, "expected function declaration");
    }

    case TokenKind::KwStatic:
      return parse_static_decl(vis);

    case TokenKind::KwModule:
      return parse_sub_module_decl(vis);

    case TokenKind::KwDep:
      if (vis != ast::Visibility::Default) {
        emit(Diagnostic(DiagnosticLevel::Warning,
                        "visibility modifiers are not meaningful on `dep` declarations",
                        file_id_)
                 .with_label(peek().span, "here"));
      }
      return parse_dep_decl();

    case TokenKind::Tilde:
      return parse_splice_stmt();

    case TokenKind::Newline:
      advance();
      return parse_top_level_item();  // Skip blank lines.

    default:
      if (vis != ast::Visibility::Default) {
        emit(Diagnostic(DiagnosticLevel::Error,
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

      emit(Diagnostic(DiagnosticLevel::Error,
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

ast::Ptr<ast::UseDecl> Parser::parse_use_decl(ast::Visibility vis) {
  auto decl = ast::make<ast::UseDecl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(TokenKind::KwUse);

  // Parse the use path: `module_path [. use_selector]`
  decl->path = parse_module_path();

  // Check for selector: `.{items}` or `.*` or `.Name [as Alias]`
  if (at(TokenKind::Dot)) {
    // Peek at what follows the dot.
    auto after_dot = peek_at(1);

    if (after_dot.is(TokenKind::LBrace)) {
      advance();  // consume `.`
      advance();  // consume `{`

      ast::UseSelector sel;
      sel.span = after_dot.span;
      sel.kind = ast::UseSelectorKind::Group;

      while (!at(TokenKind::RBrace) && !at_eof()) {
        skip_newlines();
        if (at(TokenKind::RBrace)) break;

        ast::UseItem item;
        auto item_tok = expect(TokenKind::Ident);
        item.span = item_tok.span;
        item.name = std::string(item_tok.text);

        if (match(TokenKind::KwAs)) {
          auto alias_tok = expect(TokenKind::Ident);
          item.alias = std::string(alias_tok.text);
        }

        sel.items.push_back(std::move(item));

        if (!match(TokenKind::Comma)) {
          skip_newlines();
          break;
        }
        skip_newlines();
      }

      expect(TokenKind::RBrace);
      decl->selector = std::move(sel);
    } else if (after_dot.is(TokenKind::Star)) {
      advance();  // consume `.`
      advance();  // consume `*`

      ast::UseSelector sel;
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

ast::Ptr<ast::SubModuleDecl> Parser::parse_sub_module_decl(ast::Visibility vis) {
  auto decl = ast::make<ast::SubModuleDecl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(TokenKind::KwModule);
  auto name_tok = expect(TokenKind::Ident);
  decl->name = std::string(name_tok.text);

  if (at(TokenKind::Colon)) {
    advance();  // consume `:`
    skip_newlines();

    if (match(TokenKind::Indent)) {
      while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
        skip_newlines();
        if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;

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

ast::Ptr<ast::DepDecl> Parser::parse_dep_decl() {
  auto decl = ast::make<ast::DepDecl>();
  auto start = peek().span;

  expect(TokenKind::KwDep);
  auto name_tok = expect(TokenKind::Ident);
  decl->name = std::string(name_tok.text);

  expect(TokenKind::Colon);
  skip_newlines();

  if (match(TokenKind::Indent)) {
    while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
      skip_newlines();
      if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;

      ast::DepField field;
      field.span = peek().span;
      auto key_tok = expect(TokenKind::Ident);
      field.key = std::string(key_tok.text);
      expect(TokenKind::Eq);
      auto val_tok = expect(TokenKind::StringLit);
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

ast::Ptr<ast::TypeDecl> Parser::parse_type_decl(ast::Visibility vis) {
  auto decl = ast::make<ast::TypeDecl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(TokenKind::KwType);
  auto name_tok = expect(TokenKind::Ident);
  decl->name = std::string(name_tok.text);

  // Optional type parameters.
  if (at(TokenKind::LBracket)) {
    decl->type_params = parse_type_params();
  }

  expect(TokenKind::Eq);

  // Parse the type definition.
  decl->definition = parse_type_def();

  // Check for inline deriving.
  if (at(TokenKind::KwDeriving)) {
    advance();
    // Parse deriving list.
    auto id_tok = expect(TokenKind::Ident);
    decl->deriving.emplace_back(id_tok.text);
    while (match(TokenKind::Comma)) {
      auto next_tok = expect(TokenKind::Ident);
      decl->deriving.emplace_back(next_tok.text);
    }
  }

  // If there's a newline followed by an indent, check for deriving/invariant clauses.
  if (at(TokenKind::Newline)) {
    auto saved_pos = pos_;
    advance();
    skip_newlines();

    if (at(TokenKind::Indent)) {
      advance();
      // Look for deriving and invariant clauses.
      while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
        skip_newlines();
        if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;

        if (at(TokenKind::KwDeriving)) {
          advance();
          auto id_tok = expect(TokenKind::Ident);
          decl->deriving.emplace_back(id_tok.text);
          while (match(TokenKind::Comma)) {
            auto next_tok = expect(TokenKind::Ident);
            decl->deriving.emplace_back(next_tok.text);
          }
          expect_newline();
        } else if (at(TokenKind::KwInvariant)) {
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
  if (at(TokenKind::Pipe)) {
    auto body = parse_sum_body();
    auto node = ast::make<ast::ErrorNode>(body.span, "sum body");
    // We wrap SumBody into a Node — in practice, the TypeDecl stores it
    // as a generic Node* and we check the structure later.
    // For a cleaner design we could use variant, but this keeps things simple.
    // Actually, let's just return a NamedType that wraps the concept.
    // For now, store as ErrorNode with the sum body embedded — the real
    // solution would be a dedicated SumBodyNode. Let's create an inline
    // placeholder that is not an error.
    auto result = ast::make<ast::NamedType>();
    result->span = body.span;
    // Store variant info in the named type — semantic layer will handle this.
    // TODO: proper sum body node
    return result;
  }

  // Struct body: starts with `{`
  if (at(TokenKind::LBrace)) {
    auto body = parse_struct_body();
    auto result = ast::make<ast::NamedType>();
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
  if (at(TokenKind::KwWhere)) {
    // This is actually a `where` clause on the type, not a refinement.
    // Refinement uses `where` after the type expr directly.
    // For now, return the type expr — semantic analysis will handle it.
  }

  return type;
}

ast::StructBody Parser::parse_struct_body() {
  ast::StructBody body;
  body.span = peek().span;

  expect(TokenKind::LBrace);

  while (!at(TokenKind::RBrace) && !at_eof()) {
    skip_newlines();
    if (at(TokenKind::RBrace)) break;

    body.fields.push_back(parse_struct_field());

    if (!match(TokenKind::Comma)) {
      skip_newlines();
      break;
    }
    skip_newlines();
  }

  auto close = expect(TokenKind::RBrace);
  body.span.extend_to(close.span);
  return body;
}

ast::StructField Parser::parse_struct_field() {
  ast::StructField field;
  field.span = peek().span;
  field.visibility = parse_optional_visibility();

  auto name_tok = expect(TokenKind::Ident);
  field.name = std::string(name_tok.text);
  expect(TokenKind::Colon);
  field.type = parse_type_expr();

  field.span.extend_to(previous_span());
  return field;
}

ast::SumBody Parser::parse_sum_body() {
  ast::SumBody body;
  body.span = peek().span;

  // First `|` is required.
  expect(TokenKind::Pipe);
  body.variants.push_back(parse_sum_variant());

  while (match(TokenKind::Pipe)) {
    body.variants.push_back(parse_sum_variant());
  }

  body.span.extend_to(previous_span());
  return body;
}

ast::SumVariant Parser::parse_sum_variant() {
  ast::SumVariant variant;
  variant.span = peek().span;

  auto name_tok = expect(TokenKind::Ident);
  variant.name = std::string(name_tok.text);

  if (match(TokenKind::LParen)) {
    while (!at(TokenKind::RParen) && !at_eof()) {
      auto type = parse_type_expr();
      if (type) {
        variant.payload_types.push_back(std::move(type));
      }
      if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RParen);
  }

  variant.span.extend_to(previous_span());
  return variant;
}

// ==========================================================================
//  Type expressions
// ==========================================================================

ast::Ptr<ast::TypeExpr> Parser::parse_type_expr() {
  auto first = parse_prim_type_expr();
  if (!first) return nullptr;

  // Union type: `type | type | ...`
  if (at(TokenKind::Pipe)) {
    auto union_type = ast::make<ast::UnionType>();
    union_type->span = first->span;
    union_type->alternatives.push_back(std::move(first));

    while (match(TokenKind::Pipe)) {
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

ast::Ptr<ast::TypeExpr> Parser::parse_prim_type_expr() {
  switch (current()) {
    case TokenKind::Ident:
      return parse_named_type();

    case TokenKind::LParen: {
      // Could be tuple type or grouped type.
      auto start = peek().span;
      advance();  // consume `(`

      auto first = parse_type_expr();
      if (!first) {
        expect(TokenKind::RParen);
        auto err = ast::make<ast::ErrorTypeExpr>(start.merge(previous_span()));
        return err;
      }

      if (at(TokenKind::Comma)) {
        // Tuple type.
        auto tuple = ast::make<ast::TupleType>();
        tuple->span = start;
        tuple->elements.push_back(std::move(first));

        while (match(TokenKind::Comma)) {
          if (at(TokenKind::RParen)) break;  // trailing comma
          auto elem = parse_type_expr();
          if (elem) tuple->elements.push_back(std::move(elem));
        }

        expect(TokenKind::RParen);
        tuple->span.extend_to(previous_span());
        return tuple;
      }

      // Parenthesized type expr.
      expect(TokenKind::RParen);
      first->span = start.merge(previous_span());
      return first;
    }

    case TokenKind::LBracket:
      return parse_slice_type();

    case TokenKind::KwArray:
      return parse_array_type();

    case TokenKind::Amp:
      return parse_ref_type();

    case TokenKind::Star:
      return parse_ptr_type();

    case TokenKind::KwFn:
      return parse_fn_type();

    case TokenKind::KwExpr:
    case TokenKind::KwStmt:
    case TokenKind::KwDefExpr:
    case TokenKind::KwTypeExpr: {
      auto tok = advance();
      auto qt = ast::make<ast::QuoteType>(tok.kind);
      qt->span = tok.span;
      return qt;
    }

    default:
      return nullptr;
  }
}

ast::Ptr<ast::NamedType> Parser::parse_named_type() {
  auto named = ast::make<ast::NamedType>();
  named->span = peek().span;

  named->path = parse_module_path();

  // Optional type arguments: `[T, U, ...]`
  if (at(TokenKind::LBracket)) {
    advance();  // consume `[`
    while (!at(TokenKind::RBracket) && !at_eof()) {
      skip_newlines();
      if (at(TokenKind::RBracket)) break;

      auto type_arg = parse_type_expr();
      if (type_arg) {
        named->type_args.push_back(std::move(type_arg));
      }

      if (!match(TokenKind::Comma)) break;
      skip_newlines();
    }
    expect(TokenKind::RBracket);
  }

  named->span.extend_to(previous_span());
  return named;
}

ast::Ptr<ast::TupleType> Parser::parse_tuple_type() {
  // This is called when we already know we have a tuple.
  // The LParen has NOT been consumed yet.
  auto tuple = ast::make<ast::TupleType>();
  tuple->span = peek().span;

  expect(TokenKind::LParen);
  while (!at(TokenKind::RParen) && !at_eof()) {
    auto elem = parse_type_expr();
    if (elem) tuple->elements.push_back(std::move(elem));
    if (!match(TokenKind::Comma)) break;
  }
  expect(TokenKind::RParen);

  tuple->span.extend_to(previous_span());
  return tuple;
}

ast::Ptr<ast::SliceType> Parser::parse_slice_type() {
  auto slice = ast::make<ast::SliceType>();
  slice->span = peek().span;

  expect(TokenKind::LBracket);
  slice->element = parse_type_expr();
  expect(TokenKind::RBracket);

  slice->span.extend_to(previous_span());
  return slice;
}

ast::Ptr<ast::ArrayType> Parser::parse_array_type() {
  auto arr = ast::make<ast::ArrayType>();
  arr->span = peek().span;

  expect(TokenKind::KwArray);
  expect(TokenKind::LBracket);
  arr->element = parse_type_expr();
  expect(TokenKind::Comma);
  arr->size = parse_expr();
  expect(TokenKind::RBracket);

  arr->span.extend_to(previous_span());
  return arr;
}

ast::Ptr<ast::RefType> Parser::parse_ref_type() {
  auto ref = ast::make<ast::RefType>();
  ref->span = peek().span;

  expect(TokenKind::Amp);
  ref->is_mut = match(TokenKind::KwMut);
  ref->inner = parse_type_expr();

  ref->span.extend_to(previous_span());
  return ref;
}

ast::Ptr<ast::PtrType> Parser::parse_ptr_type() {
  auto ptr = ast::make<ast::PtrType>();
  ptr->span = peek().span;

  expect(TokenKind::Star);
  ptr->is_mut = match(TokenKind::KwMut);
  ptr->inner = parse_type_expr();

  ptr->span.extend_to(previous_span());
  return ptr;
}

ast::Ptr<ast::FnType> Parser::parse_fn_type() {
  auto fn = ast::make<ast::FnType>();
  fn->span = peek().span;

  expect(TokenKind::KwFn);
  expect(TokenKind::LParen);

  while (!at(TokenKind::RParen) && !at_eof()) {
    auto param = parse_type_expr();
    if (param) fn->param_types.push_back(std::move(param));
    if (!match(TokenKind::Comma)) break;
  }

  expect(TokenKind::RParen);

  if (match(TokenKind::Arrow)) {
    fn->return_type = parse_type_expr();
  }

  fn->span.extend_to(previous_span());
  return fn;
}

// ==========================================================================
//  Type parameters and bounds
// ==========================================================================

std::vector<ast::TypeParam> Parser::parse_type_params() {
  std::vector<ast::TypeParam> params;

  expect(TokenKind::LBracket);

  while (!at(TokenKind::RBracket) && !at_eof()) {
    skip_newlines();
    if (at(TokenKind::RBracket)) break;

    params.push_back(parse_type_param());

    if (!match(TokenKind::Comma)) break;
    skip_newlines();
  }

  expect(TokenKind::RBracket);
  return params;
}

ast::TypeParam Parser::parse_type_param() {
  ast::TypeParam param;
  param.span = peek().span;

  auto name_tok = expect(TokenKind::Ident);
  param.name = std::string(name_tok.text);

  if (match(TokenKind::Colon)) {
    param.bound_or_type = parse_type_expr();
    // Heuristic: if the type is a simple named type that looks like a trait,
    // it's a bound. If it has value-like syntax, it's a value parameter type.
    // For now, we store it uniformly and let semantic analysis decide.
  }

  param.span.extend_to(previous_span());
  return param;
}

ast::Bound Parser::parse_bound() {
  ast::Bound bound;
  bound.span = peek().span;

  bound.terms.push_back(parse_bound_term());

  while (match(TokenKind::Plus)) {
    bound.terms.push_back(parse_bound_term());
  }

  bound.span.extend_to(previous_span());
  return bound;
}

ast::BoundTerm Parser::parse_bound_term() {
  ast::BoundTerm term;
  term.span = peek().span;

  if (at(TokenKind::KwFn)) {
    term.type = parse_fn_type();
  } else {
    term.type = parse_named_type();
  }

  term.span.extend_to(previous_span());
  return term;
}

std::vector<ast::WhereConstraint> Parser::parse_where_clause() {
  std::vector<ast::WhereConstraint> constraints;

  expect(TokenKind::KwWhere);

  do {
    ast::WhereConstraint c;
    c.span = peek().span;
    c.subject = parse_type_expr();
    expect(TokenKind::Colon);
    c.bound_or_type = parse_type_expr();
    c.span.extend_to(previous_span());
    constraints.push_back(std::move(c));
  } while (match(TokenKind::Comma));

  return constraints;
}

// ==========================================================================
//  Trait declarations
// ==========================================================================

ast::Ptr<ast::TraitDecl> Parser::parse_trait_decl(ast::Visibility vis) {
  auto decl = ast::make<ast::TraitDecl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(TokenKind::KwTrait);
  auto name_tok = expect(TokenKind::Ident);
  decl->name = std::string(name_tok.text);

  if (at(TokenKind::LBracket)) {
    decl->type_params = parse_type_params();
  }

  if (at(TokenKind::KwRequires)) {
    advance();
    decl->requires_bound = parse_bound();
  }

  expect(TokenKind::Colon);
  skip_newlines();

  if (match(TokenKind::Indent)) {
    while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
      skip_newlines();
      if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;

      // Parse trait items: func_decl, static_decl, associated_type_decl.
      auto item_vis = parse_optional_visibility();

      if (at(TokenKind::KwDef) || peek().is_func_modifier()) {
        auto mods = parse_func_modifiers();
        if (at(TokenKind::KwDef)) {
          decl->items.push_back(parse_func_decl(item_vis, std::move(mods)));
        } else {
          emit_unexpected("`def` in trait body");
          synchronize_to_newline();
        }
      } else if (at(TokenKind::KwStatic)) {
        decl->items.push_back(parse_static_decl(item_vis));
      } else if (at(TokenKind::KwType)) {
        // Associated type declaration.
        advance();  // consume `type`
        [[maybe_unused]] auto aname_tok = expect(TokenKind::Ident);
        // TODO: build AssociatedTypeDecl node properly
        // For now, skip to newline.
        if (match(TokenKind::Eq)) {
          [[maybe_unused]] auto default_type = parse_type_expr();
        }
        expect_newline();
      } else if (at(TokenKind::Newline)) {
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

ast::Ptr<ast::ConceptDecl> Parser::parse_concept_decl(ast::Visibility vis) {
  auto decl = ast::make<ast::ConceptDecl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(TokenKind::KwConcept);
  auto name_tok = expect(TokenKind::Ident);
  decl->name = std::string(name_tok.text);

  // Parse concept parameters.
  expect(TokenKind::LBracket);
  while (!at(TokenKind::RBracket) && !at_eof()) {
    ast::ConceptParam param;
    param.span = peek().span;
    auto pname_tok = expect(TokenKind::Ident);
    param.name = std::string(pname_tok.text);

    if (match(TokenKind::LBracket)) {
      expect(TokenKind::KwUnderscore);
      expect(TokenKind::RBracket);
      param.is_higher_kinded = true;
    }

    decl->params.push_back(std::move(param));
    if (!match(TokenKind::Comma)) break;
  }
  expect(TokenKind::RBracket);

  expect(TokenKind::Colon);
  skip_newlines();

  if (match(TokenKind::Indent)) {
    while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
      skip_newlines();
      if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;

      // Parse concept constraints.
      ast::ConceptConstraint constraint;
      constraint.span = peek().span;
      // Try to parse `type : bound` or just `expr`.
      // We parse as type expr, then check for `:`.
      auto subject = parse_type_expr();
      if (at(TokenKind::Colon)) {
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

ast::Ptr<ast::ImplDecl> Parser::parse_impl_decl() {
  auto decl = ast::make<ast::ImplDecl>();
  auto start = peek().span;

  expect(TokenKind::KwImpl);

  // Optional type parameters.
  if (at(TokenKind::LBracket)) {
    decl->type_params = parse_type_params();
  }

  decl->trait_type = parse_named_type();
  expect_with_context(TokenKind::KwFor, "in `impl Trait for Type`");
  decl->for_type = parse_type_expr();

  // Optional where clause.
  if (at(TokenKind::KwWhere)) {
    decl->where_constraints = parse_where_clause();
  }

  expect(TokenKind::Colon);
  skip_newlines();

  if (match(TokenKind::Indent)) {
    while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
      skip_newlines();
      if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;

      auto item_vis = parse_optional_visibility();

      if (at(TokenKind::KwDef) || peek().is_func_modifier()) {
        auto mods = parse_func_modifiers();
        if (at(TokenKind::KwDef)) {
          decl->items.push_back(parse_func_decl(item_vis, std::move(mods)));
        } else {
          emit_unexpected("`def` in impl body");
          synchronize_to_newline();
        }
      } else if (at(TokenKind::KwStatic)) {
        decl->items.push_back(parse_static_decl(item_vis));
      } else if (at(TokenKind::KwType)) {
        // Associated type definition.
        advance();  // consume `type`
        [[maybe_unused]] auto assoc_name_tok = expect(TokenKind::Ident);
        expect(TokenKind::Eq);
        [[maybe_unused]] auto assoc_type = parse_type_expr();
        expect_newline();
        // TODO: build AssociatedTypeDef node
      } else if (at(TokenKind::Newline)) {
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

ast::FuncModifiers Parser::parse_func_modifiers() {
  ast::FuncModifiers mods;

  // Parse modifiers in any order — canonical is static pure async machine
  // but we accept any combination and let semantic analysis sort it out.
  bool keep_going = true;
  while (keep_going) {
    switch (current()) {
      case TokenKind::KwPure:
        if (mods.is_pure) {
          emit(Diagnostic(DiagnosticLevel::Warning,
                          "duplicate `pure` modifier — you only need it once",
                          file_id_)
                   .with_label(peek().span, "this `pure` is redundant"));
        }
        mods.is_pure = true;
        advance();
        break;
      case TokenKind::KwAsync:
        if (mods.is_async) {
          emit(Diagnostic(DiagnosticLevel::Warning,
                          "duplicate `async` modifier — you only need it once",
                          file_id_)
                   .with_label(peek().span, "this `async` is redundant"));
        }
        mods.is_async = true;
        advance();
        // Optional async context: `async[ContextType]`
        if (at(TokenKind::LBracket)) {
          advance();
          mods.async_context = parse_type_expr();
          expect(TokenKind::RBracket);
        }
        break;
      case TokenKind::KwMachine:
        if (mods.is_machine) {
          emit(Diagnostic(DiagnosticLevel::Warning,
                          "duplicate `machine` modifier",
                          file_id_)
                   .with_label(peek().span, "redundant"));
        }
        mods.is_machine = true;
        advance();
        break;
      case TokenKind::KwStatic:
        if (mods.is_static) {
          emit(Diagnostic(DiagnosticLevel::Warning,
                          "duplicate `static` modifier",
                          file_id_)
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

ast::Ptr<ast::FuncDecl> Parser::parse_func_decl(
    ast::Visibility vis, ast::FuncModifiers mods) {
  auto decl = ast::make<ast::FuncDecl>();
  auto start = peek().span;
  decl->visibility = vis;
  decl->modifiers = std::move(mods);

  expect(TokenKind::KwDef);
  auto name_tok = expect(TokenKind::Ident);
  decl->name = std::string(name_tok.text);

  // Optional type parameters.
  if (at(TokenKind::LBracket)) {
    decl->type_params = parse_type_params();
  }

  // Parameter list.
  expect(TokenKind::LParen);
  if (!at(TokenKind::RParen)) {
    decl->params = parse_param_list();
  }
  expect(TokenKind::RParen);

  // Optional return type.
  if (match(TokenKind::Arrow)) {
    decl->return_type = parse_type_expr();
  }

  // Optional where clause.
  if (at(TokenKind::KwWhere)) {
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
    if (at(TokenKind::RParen)) break;
    params.push_back(parse_param());
  } while (match(TokenKind::Comma));

  return params;
}

ast::Param Parser::parse_param() {
  ast::Param param;
  param.span = peek().span;

  param.pattern = parse_pattern();

  if (match(TokenKind::Colon)) {
    param.type_annotation = parse_type_expr();
  }

  if (match(TokenKind::Eq)) {
    param.default_value = parse_expr();
  }

  param.span.extend_to(previous_span());
  return param;
}

std::vector<ast::ContractClause> Parser::parse_contract_clauses() {
  std::vector<ast::ContractClause> clauses;

  while (at_any(TokenKind::KwPre, TokenKind::KwPost)) {
    clauses.push_back(parse_contract_clause());
  }

  return clauses;
}

ast::ContractClause Parser::parse_contract_clause() {
  ast::ContractClause clause;
  clause.span = peek().span;

  if (at(TokenKind::KwPre)) {
    clause.is_pre = true;
    advance();
  } else {
    clause.is_pre = false;
    expect(TokenKind::KwPost);
  }

  clause.condition = parse_expr();

  if (match(TokenKind::Comma)) {
    auto msg_tok = expect(TokenKind::StringLit);
    clause.message = std::string(msg_tok.text);
  }

  expect_newline();

  clause.span.extend_to(previous_span());
  return clause;
}

// ==========================================================================
//  Static declarations
// ==========================================================================

ast::Ptr<ast::StaticDecl> Parser::parse_static_decl(ast::Visibility vis) {
  auto decl = ast::make<ast::StaticDecl>();
  auto start = peek().span;
  decl->visibility = vis;

  expect(TokenKind::KwStatic);

  // Disambiguate: static if, static assert, static for, static binding.
  if (at(TokenKind::KwIf)) {
    // static if expr: block [else: block]
    decl->decl_kind = ast::StaticDeclKind::ConditionalCompilation;
    advance();  // consume `if`
    decl->if_condition = parse_expr();
    expect(TokenKind::Colon);
    skip_newlines();

    if (match(TokenKind::Indent)) {
      while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
        skip_newlines();
        if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;
        auto item = parse_top_level_item();
        if (item) decl->if_body.push_back(std::move(item));
        else synchronize();
      }
      expect_block_end("static if");
    }

    if (at(TokenKind::KwElse)) {
      advance();
      expect(TokenKind::Colon);
      skip_newlines();

      if (match(TokenKind::Indent)) {
        while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
          skip_newlines();
          if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;
          auto item = parse_top_level_item();
          if (item) decl->else_body.push_back(std::move(item));
          else synchronize();
        }
        expect_block_end("static else");
      }
    }
  } else if (at(TokenKind::KwAssert)) {
    // static assert expr [, message]
    decl->decl_kind = ast::StaticDeclKind::Assert;
    advance();  // consume `assert`
    decl->assert_condition = parse_expr();
    if (match(TokenKind::Comma)) {
      auto msg_tok = expect(TokenKind::StringLit);
      decl->assert_message = std::string(msg_tok.text);
    }
    expect_newline();
  } else if (at(TokenKind::KwFor)) {
    // static for vars in expr [if guard] => expr   OR
    // static for vars in expr [if guard]: block
    advance();  // consume `for`
    decl->for_patterns = parse_for_vars();
    expect_with_context(TokenKind::KwIn, "in `static for ... in ...`");
    decl->for_iterable = parse_expr();

    if (match(TokenKind::KwIf)) {
      decl->for_guard = parse_expr();
    }

    if (at(TokenKind::FatArrow)) {
      // Inline form.
      decl->decl_kind = ast::StaticDeclKind::ForInline;
      advance();
      decl->for_yield = parse_expr();
      expect_newline();
    } else {
      // Block form.
      decl->decl_kind = ast::StaticDeclKind::ForBlock;
      expect(TokenKind::Colon);
      skip_newlines();
      if (match(TokenKind::Indent)) {
        while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
          skip_newlines();
          if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;
          auto stmt = parse_stmt();
          if (stmt) decl->for_body.push_back(std::move(stmt));
          else synchronize();
        }
        expect_block_end("static for");
      }
    }
  } else {
    // static binding: static Name [: Type] = expr
    decl->decl_kind = ast::StaticDeclKind::Binding;
    auto name_tok = expect(TokenKind::Ident);
    decl->name = std::string(name_tok.text);

    if (match(TokenKind::Colon)) {
      decl->type_annotation = parse_type_expr();
    }

    expect(TokenKind::Eq);
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

  if (at_eof() || at(TokenKind::Dedent)) return nullptr;

  // Check for statements that start with a keyword.
  switch (current()) {
    case TokenKind::KwLet:
      return parse_let_stmt();
    case TokenKind::KwVar:
      return parse_var_stmt();
    case TokenKind::KwReturn:
      return parse_return_stmt();
    case TokenKind::KwIf:
      return parse_if_stmt();
    case TokenKind::KwWhile:
      return parse_while_stmt();
    case TokenKind::KwFor:
      return parse_for_stmt();
    case TokenKind::KwMatch:
      return parse_match_stmt();
    case TokenKind::KwCrew:
      return parse_crew_stmt();
    case TokenKind::KwAsm:
      return parse_asm_stmt();
    case TokenKind::Tilde:
      return parse_splice_stmt();

    case TokenKind::KwUse:
      return parse_use_decl(ast::Visibility::Default);
    case TokenKind::KwType:
      return parse_type_decl(ast::Visibility::Default);
    case TokenKind::KwStatic:
      return parse_static_decl(ast::Visibility::Default);

    case TokenKind::KwPub:
    case TokenKind::KwInternal:
    case TokenKind::KwSuper:
    case TokenKind::KwPriv: {
      auto vis = parse_optional_visibility();
      if (at(TokenKind::KwDef) || peek().is_func_modifier()) {
        auto mods = parse_func_modifiers();
        if (at(TokenKind::KwDef)) {
          return parse_func_decl(vis, std::move(mods));
        }
      }
      if (at(TokenKind::KwType)) return parse_type_decl(vis);
      if (at(TokenKind::KwStatic)) return parse_static_decl(vis);
      if (at(TokenKind::KwUse)) return parse_use_decl(vis);
      emit_unexpected("a declaration after visibility modifier");
      synchronize_to_newline();
      return make_error_node(peek().span);
    }

    case TokenKind::KwDef:
      return parse_func_decl(ast::Visibility::Default, ast::FuncModifiers{});

    case TokenKind::KwPure:
    case TokenKind::KwAsync:
    case TokenKind::KwMachine: {
      auto mods = parse_func_modifiers();
      if (at(TokenKind::KwDef)) {
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

ast::Ptr<ast::LetStmt> Parser::parse_let_stmt() {
  auto stmt = ast::make<ast::LetStmt>();
  auto start = peek().span;

  expect(TokenKind::KwLet);
  stmt->pattern = parse_pattern();

  if (match(TokenKind::Colon)) {
    stmt->type_annotation = parse_type_expr();
  }

  expect(TokenKind::Eq);
  stmt->initializer = parse_expr();

  // Check for `else:` block.
  if (at(TokenKind::KwElse)) {
    advance();
    expect(TokenKind::Colon);
    skip_newlines();

    if (match(TokenKind::Indent)) {
      while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
        skip_newlines();
        if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;
        auto s = parse_stmt();
        if (s) stmt->else_body.push_back(std::move(s));
        else synchronize();
      }
      expect_block_end("let-else");
    }
  } else {
    expect_newline();
  }

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::VarStmt> Parser::parse_var_stmt() {
  auto stmt = ast::make<ast::VarStmt>();
  auto start = peek().span;

  expect(TokenKind::KwVar);
  auto name_tok = expect(TokenKind::Ident);
  stmt->name = std::string(name_tok.text);

  if (match(TokenKind::Colon)) {
    stmt->type_annotation = parse_type_expr();
  }

  expect(TokenKind::Eq);
  stmt->initializer = parse_expr();
  expect_newline();

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::ReturnStmt> Parser::parse_return_stmt() {
  auto stmt = ast::make<ast::ReturnStmt>();
  auto start = peek().span;

  expect(TokenKind::KwReturn);

  // Return value is optional.
  if (!at_any(TokenKind::Newline, TokenKind::Dedent, TokenKind::Eof)) {
    stmt->value = parse_expr();
  }

  expect_newline();

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::IfStmt> Parser::parse_if_stmt() {
  auto stmt = ast::make<ast::IfStmt>();
  auto start = peek().span;

  // First `if` branch.
  expect(TokenKind::KwIf);
  {
    ast::IfBranch branch;
    branch.span = start;
    branch.condition = parse_expr();
    auto body = parse_body("if");
    if (body.inline_expr) {
      auto es = ast::make<ast::ExprStmt>();
      es->expr = std::move(body.inline_expr);
      branch.body.push_back(std::move(es));
    } else {
      branch.body = std::move(body.stmts);
    }
    branch.span.extend_to(previous_span());
    stmt->branches.push_back(std::move(branch));
  }

  // `elif` branches.
  while (at(TokenKind::KwElif)) {
    advance();
    ast::IfBranch branch;
    branch.span = previous_span();
    branch.condition = parse_expr();
    auto body = parse_body("elif");
    if (body.inline_expr) {
      auto es = ast::make<ast::ExprStmt>();
      es->expr = std::move(body.inline_expr);
      branch.body.push_back(std::move(es));
    } else {
      branch.body = std::move(body.stmts);
    }
    branch.span.extend_to(previous_span());
    stmt->branches.push_back(std::move(branch));
  }

  // Optional `else` branch.
  if (at(TokenKind::KwElse)) {
    advance();
    auto body = parse_body("else");
    if (body.inline_expr) {
      auto es = ast::make<ast::ExprStmt>();
      es->expr = std::move(body.inline_expr);
      stmt->else_body.push_back(std::move(es));
    } else {
      stmt->else_body = std::move(body.stmts);
    }
  }

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::WhileStmt> Parser::parse_while_stmt() {
  auto stmt = ast::make<ast::WhileStmt>();
  auto start = peek().span;

  expect(TokenKind::KwWhile);

  if (at(TokenKind::KwLet)) {
    // while let pattern = expr: block
    advance();
    stmt->let_pattern = parse_pattern();
    expect(TokenKind::Eq);
    stmt->let_expr = parse_expr();
  } else {
    stmt->condition = parse_expr();
  }

  auto body = parse_body("while");
  if (body.inline_expr) {
    auto es = ast::make<ast::ExprStmt>();
    es->expr = std::move(body.inline_expr);
    stmt->body.push_back(std::move(es));
  } else {
    stmt->body = std::move(body.stmts);
  }

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::ForStmt> Parser::parse_for_stmt() {
  auto stmt = ast::make<ast::ForStmt>();
  auto start = peek().span;

  expect(TokenKind::KwFor);
  stmt->patterns = parse_for_vars();
  expect_with_context(TokenKind::KwIn, "in `for ... in ...`");
  stmt->iterable = parse_expr();

  if (match(TokenKind::KwIf)) {
    stmt->guard = parse_expr();
  }

  auto body = parse_body("for");
  if (body.inline_expr) {
    auto es = ast::make<ast::ExprStmt>();
    es->expr = std::move(body.inline_expr);
    stmt->body.push_back(std::move(es));
  } else {
    stmt->body = std::move(body.stmts);
  }

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::MatchStmt> Parser::parse_match_stmt() {
  auto stmt = ast::make<ast::MatchStmt>();
  auto start = peek().span;

  expect(TokenKind::KwMatch);
  stmt->subject = parse_expr();
  expect(TokenKind::Colon);
  skip_newlines();

  if (match(TokenKind::Indent)) {
    while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
      skip_newlines();
      if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;
      stmt->arms.push_back(parse_match_arm());
    }
    expect_block_end("match");
  }

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::CrewStmt> Parser::parse_crew_stmt() {
  auto stmt = ast::make<ast::CrewStmt>();
  auto start = peek().span;

  expect(TokenKind::KwCrew);
  auto name_tok = expect(TokenKind::Ident);
  stmt->name = std::string(name_tok.text);

  if (match(TokenKind::LParen)) {
    while (!at(TokenKind::RParen) && !at_eof()) {
      ast::CrewOption opt;
      opt.span = peek().span;
      auto key_tok = expect(TokenKind::Ident);
      opt.name = std::string(key_tok.text);
      expect(TokenKind::Colon);
      auto val_tok = expect(TokenKind::Ident);
      opt.value = std::string(val_tok.text);
      stmt->options.push_back(std::move(opt));
      if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RParen);
  }

  auto body = parse_body("crew");
  if (body.inline_expr) {
    auto es = ast::make<ast::ExprStmt>();
    es->expr = std::move(body.inline_expr);
    stmt->body.push_back(std::move(es));
  } else {
    stmt->body = std::move(body.stmts);
  }

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::AsmStmt> Parser::parse_asm_stmt() {
  auto stmt = ast::make<ast::AsmStmt>();
  auto start = peek().span;

  expect(TokenKind::KwAsm);
  expect(TokenKind::LBrace);

  // Consume everything until matching `}`.
  int depth = 1;
  while (!at_eof() && depth > 0) {
    if (at(TokenKind::LBrace)) ++depth;
    else if (at(TokenKind::RBrace)) --depth;
    if (depth > 0) advance();
  }

  // Collect the text between the braces.
  // For now, just record that we have ASM content.
  stmt->content = "/* asm content */";
  expect(TokenKind::RBrace);
  expect_newline();

  stmt->span = start.merge(previous_span());
  return stmt;
}

ast::Ptr<ast::SpliceStmt> Parser::parse_splice_stmt() {
  auto stmt = ast::make<ast::SpliceStmt>();
  auto start = peek().span;

  expect(TokenKind::Tilde);
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
    advance();  // consume the assign op

    auto assign = ast::make<ast::AssignStmt>();
    assign->target = std::move(expr);
    assign->op = *assign_op;
    assign->value = parse_expr();
    expect_newline();
    assign->span = start.merge(previous_span());
    return assign;
  }

  // Plain expression statement.
  auto es = ast::make<ast::ExprStmt>();
  es->expr = std::move(expr);
  expect_newline();
  es->span = start.merge(previous_span());
  return es;
}

// ==========================================================================
//  Expressions
// ==========================================================================

ast::Ptr<ast::Expr> Parser::parse_expr() {
  return parse_pipe_expr();
}

ast::Ptr<ast::Expr> Parser::parse_pipe_expr() {
  auto lhs = parse_or_expr();
  if (!lhs) return nullptr;

  // The `|` token is overloaded (pipe vs union type vs or-pattern).
  // In expression context, `|` after an expression is the pipe operator.
  while (at(TokenKind::Pipe)) {
    auto op_tok = advance();
    auto rhs = parse_or_expr();
    if (!rhs) {
      emit_unexpected("an expression after `|`");
      rhs = make_error_expr(op_tok.span);
    }

    auto bin = ast::make<ast::BinaryExpr>();
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
  if (!lhs) return nullptr;

  while (at(TokenKind::KwOr)) {
    advance();
    auto rhs = parse_and_expr();
    if (!rhs) {
      emit_unexpected("an expression after `or`");
      rhs = make_error_expr(previous_span());
    }

    auto bin = ast::make<ast::BinaryExpr>();
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
  if (!lhs) return nullptr;

  while (at(TokenKind::KwAnd)) {
    advance();
    auto rhs = parse_not_expr();
    if (!rhs) {
      emit_unexpected("an expression after `and`");
      rhs = make_error_expr(previous_span());
    }

    auto bin = ast::make<ast::BinaryExpr>();
    bin->span = lhs->span.merge(rhs->span);
    bin->op = ast::BinaryOp::And;
    bin->lhs = std::move(lhs);
    bin->rhs = std::move(rhs);
    lhs = std::move(bin);
  }

  return lhs;
}

ast::Ptr<ast::Expr> Parser::parse_not_expr() {
  if (at(TokenKind::KwNot)) {
    auto op_tok = advance();
    auto operand = parse_not_expr();
    if (!operand) {
      emit_unexpected("an expression after `not`");
      operand = make_error_expr(op_tok.span);
    }

    auto unary = ast::make<ast::UnaryExpr>();
    unary->span = op_tok.span.merge(operand->span);
    unary->op = ast::UnaryOp::Not;
    unary->operand = std::move(operand);
    return unary;
  }

  return parse_cmp_expr();
}

ast::Ptr<ast::Expr> Parser::parse_cmp_expr() {
  auto lhs = parse_add_expr();
  if (!lhs) return nullptr;

  auto maybe_op = token_to_cmp_op();
  if (maybe_op) {
    auto op_tok = advance();
    auto op = *maybe_op;

    // Handle `not in` (two-token operator).
    if (op_tok.kind == TokenKind::KwNot) {
      if (at(TokenKind::KwIn)) {
        advance();  // consume `in`
        op = ast::BinaryOp::NotIn;
      } else {
        // Just `not` in comparison position — this is odd.
        // The `not` was already consumed as a cmp_op candidate.
        // Let's treat it as `not in` and emit an error.
        emit(Diagnostic(DiagnosticLevel::Error,
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

    auto bin = ast::make<ast::BinaryExpr>();
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
  if (!lhs) return nullptr;

  while (true) {
    auto maybe_op = token_to_add_op(current());
    if (!maybe_op) break;

    advance();
    auto op = *maybe_op;

    auto rhs = parse_mul_expr();
    if (!rhs) {
      emit_unexpected("an expression after arithmetic operator");
      rhs = make_error_expr(previous_span());
    }

    auto bin = ast::make<ast::BinaryExpr>();
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
  if (!lhs) return nullptr;

  while (true) {
    auto maybe_op = token_to_mul_op(current());
    if (!maybe_op) break;

    advance();
    auto op = *maybe_op;

    auto rhs = parse_unary_expr();
    if (!rhs) {
      emit_unexpected("an expression after arithmetic operator");
      rhs = make_error_expr(previous_span());
    }

    auto bin = ast::make<ast::BinaryExpr>();
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
    case TokenKind::Minus: {
      auto op_tok = advance();
      auto operand = parse_unary_expr();
      if (!operand) {
        emit_unexpected("an expression after `-`");
        operand = make_error_expr(op_tok.span);
      }
      auto unary = ast::make<ast::UnaryExpr>();
      unary->span = op_tok.span.merge(operand->span);
      unary->op = ast::UnaryOp::Neg;
      unary->operand = std::move(operand);
      return unary;
    }

    case TokenKind::Tilde: {
      // At expression depth 0, `~(expr)` is a splice, `~anything_else`
      // is bitwise complement. We check for `(` to disambiguate.
      if (peek_at(1).is(TokenKind::LParen)) {
        return parse_splice_expr_inner();
      }
      if (peek_at(1).is(TokenKind::Ident)) {
        // Could also be a splice: `~name`
        return parse_splice_expr_inner();
      }
      auto op_tok = advance();
      auto operand = parse_unary_expr();
      if (!operand) {
        emit_unexpected("an expression after `~`");
        operand = make_error_expr(op_tok.span);
      }
      auto unary = ast::make<ast::UnaryExpr>();
      unary->span = op_tok.span.merge(operand->span);
      unary->op = ast::UnaryOp::BitNot;
      unary->operand = std::move(operand);
      return unary;
    }

    case TokenKind::Star: {
      auto op_tok = advance();
      auto operand = parse_unary_expr();
      if (!operand) {
        emit_unexpected("an expression after `*`");
        operand = make_error_expr(op_tok.span);
      }
      auto unary = ast::make<ast::UnaryExpr>();
      unary->span = op_tok.span.merge(operand->span);
      unary->op = ast::UnaryOp::Deref;
      unary->operand = std::move(operand);
      return unary;
    }

    case TokenKind::Amp: {
      auto op_tok = advance();
      bool is_mut = match(TokenKind::KwMut);
      auto operand = parse_unary_expr();
      if (!operand) {
        emit_unexpected("an expression after `&`");
        operand = make_error_expr(op_tok.span);
      }
      auto unary = ast::make<ast::UnaryExpr>();
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
  if (!base) return nullptr;

  while (true) {
    // Check if the current token can start a postfix suffix before
    // moving base into parse_postfix_suffix. This avoids losing
    // ownership of base when there's no suffix to parse.
    if (!at_any(TokenKind::Dot, TokenKind::LBracket, TokenKind::LParen,
                TokenKind::Question, TokenKind::KwAs)) {
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
    case TokenKind::Dot: {
      advance();  // consume `.`
      if (at(TokenKind::Ident)) {
        auto field_tok = advance();
        auto field = ast::make<ast::FieldExpr>();
        field->span = base->span.merge(field_tok.span);
        field->object = std::move(base);
        field->field_name = std::string(field_tok.text);

        // Check for generic method: `.method[T, U]`
        if (at(TokenKind::LBracket)) {
          advance();
          while (!at(TokenKind::RBracket) && !at_eof()) {
            auto arg = parse_type_expr();
            if (arg) field->generic_args.push_back(std::move(arg));
            if (!match(TokenKind::Comma)) break;
          }
          expect(TokenKind::RBracket);
          field->span.extend_to(previous_span());
        }

        return field;
      } else if (at(TokenKind::LParen)) {
        // `.()` — field via parens (unusual but grammatical).
        advance();
        expect(TokenKind::RParen);
        auto field = ast::make<ast::FieldExpr>();
        field->span = base->span.merge(previous_span());
        field->object = std::move(base);
        return field;
      } else {
        emit_unexpected("a field name after `.`");
        return base;
      }
    }

    case TokenKind::LBracket: {
      advance();  // consume `[`
      auto index = parse_expr();
      expect(TokenKind::RBracket);

      auto idx = ast::make<ast::IndexExpr>();
      idx->span = base->span.merge(previous_span());
      idx->object = std::move(base);
      idx->index = std::move(index);
      return idx;
    }

    case TokenKind::LParen: {
      auto call = ast::make<ast::CallExpr>();
      call->span = base->span;
      call->callee = std::move(base);

      advance();  // consume `(`
      if (!at(TokenKind::RParen)) {
        call->args = parse_call_args();
      }
      expect(TokenKind::RParen);

      call->span.extend_to(previous_span());
      return call;
    }

    case TokenKind::Question: {
      auto q_tok = advance();
      auto try_expr = ast::make<ast::TryExpr>();
      try_expr->span = base->span.merge(q_tok.span);
      try_expr->operand = std::move(base);
      return try_expr;
    }

    case TokenKind::KwAs: {
      advance();
      auto target_type = parse_type_expr();
      auto cast = ast::make<ast::CastExpr>();
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
    case TokenKind::IntLit:
    case TokenKind::FloatLit:
    case TokenKind::StringLit:
    case TokenKind::CharLit:
    case TokenKind::KwTrue:
    case TokenKind::KwFalse:
    case TokenKind::KwUnit:
      return parse_literal_expr();

    // Identifiers and module paths.
    case TokenKind::Ident:
      return parse_ident_or_path_expr();

    // Parenthesized expr, tuple, or grouped.
    case TokenKind::LParen:
      return parse_paren_expr();

    // Array literal or fill.
    case TokenKind::LBracket:
      return parse_bracket_expr();

    // Struct literal.
    case TokenKind::LBrace:
      return parse_brace_expr();

    // Lambda expressions.
    case TokenKind::KwPure:
    case TokenKind::KwMove:
      return parse_lambda_expr();

    // Match expression.
    case TokenKind::KwMatch:
      return parse_match_expr();

    // If expression.
    case TokenKind::KwIf:
      return parse_if_expr();

    // For comprehension.
    case TokenKind::KwFor:
      return parse_for_expr();

    // Await expression.
    case TokenKind::KwAwait:
      return parse_await_expr();

    // Par expression.
    case TokenKind::KwPar:
      return parse_par_expr();

    // Race expression.
    case TokenKind::KwRace:
      return parse_race_expr();

    // On expression.
    case TokenKind::KwOn:
      return parse_on_expr();

    // Quote expression.
    case TokenKind::Backtick:
      return parse_quote_expr();

    // Static expression.
    case TokenKind::KwStatic:
      return parse_static_expr();

    default:
      return nullptr;
  }
}

ast::Ptr<ast::Expr> Parser::parse_literal_expr() {
  auto tok = advance();
  auto lit = ast::make<ast::LiteralExpr>();
  lit->span = tok.span;
  lit->lit_kind = tok.kind;
  lit->value = std::string(tok.text);
  return lit;
}

ast::Ptr<ast::Expr> Parser::parse_ident_or_path_expr() {
  // Check if this looks like a lambda: `ident => ...`
  if (peek_at(1).is(TokenKind::FatArrow)) {
    return parse_lambda_expr();
  }

  auto tok = advance();
  auto ident = ast::make<ast::IdentExpr>();
  ident->span = tok.span;
  ident->name = std::string(tok.text);

  // Check for module path: `a.b.c` where next is `.` followed by ident
  // but NOT followed by `(` or `[` (those are field access/method call).
  if (at(TokenKind::Dot) && peek_at(1).is(TokenKind::Ident) &&
      !peek_at(2).is(TokenKind::LParen) &&
      !peek_at(2).is(TokenKind::LBracket)) {
    // Might be a module path. But field access `obj.field` is more common.
    // We return the ident and let postfix parsing handle `.field`.
  }

  return ident;
}

ast::Ptr<ast::Expr> Parser::parse_paren_expr() {
  auto start = peek().span;
  advance();  // consume `(`

  // Empty parens: `()` — unit literal.
  if (at(TokenKind::RParen)) {
    advance();
    auto lit = ast::make<ast::LiteralExpr>();
    lit->span = start.merge(previous_span());
    lit->lit_kind = TokenKind::KwUnit;
    lit->value = "()";
    return lit;
  }

  auto first = parse_expr();
  if (!first) {
    expect(TokenKind::RParen);
    return make_error_expr(start.merge(previous_span()));
  }

  // Tuple: `(a, b, ...)`
  if (at(TokenKind::Comma)) {
    auto tuple = ast::make<ast::TupleExpr>();
    tuple->span = start;
    tuple->elements.push_back(std::move(first));

    while (match(TokenKind::Comma)) {
      if (at(TokenKind::RParen)) break;  // trailing comma
      auto elem = parse_expr();
      if (elem) {
        tuple->elements.push_back(std::move(elem));
      } else {
        break;
      }
    }

    expect(TokenKind::RParen);
    tuple->span.extend_to(previous_span());
    return tuple;
  }

  // Grouped expression: `(expr)`
  expect(TokenKind::RParen);
  auto group = ast::make<ast::GroupExpr>();
  group->span = start.merge(previous_span());
  group->inner = std::move(first);
  return group;
}

ast::Ptr<ast::Expr> Parser::parse_bracket_expr() {
  auto start = peek().span;
  advance();  // consume `[`

  // Empty array: `[]`
  if (at(TokenKind::RBracket)) {
    advance();
    auto arr = ast::make<ast::ArrayExpr>();
    arr->span = start.merge(previous_span());
    return arr;
  }

  auto first = parse_expr();
  if (!first) {
    expect(TokenKind::RBracket);
    return make_error_expr(start.merge(previous_span()));
  }

  // Fill form: `[val; count]`
  if (at(TokenKind::Semicolon)) {
    advance();
    auto count = parse_expr();
    expect(TokenKind::RBracket);

    auto arr = ast::make<ast::ArrayExpr>();
    arr->span = start.merge(previous_span());
    arr->fill_value = std::move(first);
    arr->fill_count = std::move(count);
    return arr;
  }

  // Array literal: `[a, b, c]`
  auto arr = ast::make<ast::ArrayExpr>();
  arr->span = start;
  arr->elements.push_back(std::move(first));

  while (match(TokenKind::Comma)) {
    if (at(TokenKind::RBracket)) break;  // trailing comma
    auto elem = parse_expr();
    if (elem) {
      arr->elements.push_back(std::move(elem));
    } else {
      break;
    }
  }

  expect(TokenKind::RBracket);
  arr->span.extend_to(previous_span());
  return arr;
}

ast::Ptr<ast::Expr> Parser::parse_brace_expr() {
  auto start = peek().span;
  advance();  // consume `{`

  // Empty struct literal: `{}`
  if (at(TokenKind::RBrace)) {
    advance();
    auto s = ast::make<ast::StructExpr>();
    s->span = start.merge(previous_span());
    return s;
  }

  auto s = ast::make<ast::StructExpr>();
  s->span = start;

  while (!at(TokenKind::RBrace) && !at_eof()) {
    skip_newlines();
    if (at(TokenKind::RBrace)) break;

    ast::StructFieldInit field;
    field.span = peek().span;

    auto name_tok = expect(TokenKind::Ident);
    field.name = std::string(name_tok.text);

    if (match(TokenKind::Colon)) {
      field.value = parse_expr();
    }
    // else: shorthand {x} means {x: x}

    field.span.extend_to(previous_span());
    s->fields.push_back(std::move(field));

    if (!match(TokenKind::Comma)) {
      skip_newlines();
      break;
    }
    skip_newlines();
  }

  expect(TokenKind::RBrace);
  s->span.extend_to(previous_span());
  return s;
}

ast::Ptr<ast::Expr> Parser::parse_lambda_expr() {
  auto lambda = ast::make<ast::LambdaExpr>();
  auto start = peek().span;

  // Optional `pure` and `move` prefixes.
  lambda->is_pure = match(TokenKind::KwPure);
  lambda->is_move = match(TokenKind::KwMove);

  // Lambda params: either a single ident or `(param_list)`.
  if (at(TokenKind::Ident) && peek_at(1).is(TokenKind::FatArrow)) {
    // Single parameter shorthand: `x => expr`
    auto param_tok = advance();
    ast::LambdaParam param;
    param.span = param_tok.span;
    auto binding = ast::make<ast::BindingPattern>();
    binding->span = param_tok.span;
    binding->name = std::string(param_tok.text);
    param.pattern = std::move(binding);
    lambda->params.push_back(std::move(param));
  } else if (at(TokenKind::LParen)) {
    advance();  // consume `(`
    while (!at(TokenKind::RParen) && !at_eof()) {
      ast::LambdaParam param;
      param.span = peek().span;
      param.pattern = parse_pattern();
      if (match(TokenKind::Colon)) {
        param.type_annotation = parse_type_expr();
      }
      param.span.extend_to(previous_span());
      lambda->params.push_back(std::move(param));
      if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RParen);
  } else if (at(TokenKind::Ident)) {
    // Single ident without `=>` — might have a return type annotation.
    auto param_tok = advance();
    ast::LambdaParam param;
    param.span = param_tok.span;
    auto binding = ast::make<ast::BindingPattern>();
    binding->span = param_tok.span;
    binding->name = std::string(param_tok.text);
    param.pattern = std::move(binding);
    lambda->params.push_back(std::move(param));
  }

  // Optional return type.
  if (match(TokenKind::Arrow)) {
    lambda->return_type = parse_type_expr();
  }

  // `=>` introduces the body.
  if (!at(TokenKind::FatArrow)) {
    emit(Diagnostic(DiagnosticLevel::Error,
                    "expected `=>` to start the lambda body",
                    file_id_)
             .with_label(peek().span, "expected `=>` here")
             .with_help("Lambda expressions use `=>` between the parameters "
                        "and the body. For example: `x => x + 1` or "
                        "`(a, b) => a + b`."));
  } else {
    advance();  // consume `=>`
  }

  // Body: inline expr or block.
  if (at(TokenKind::Colon) && peek_at(1).is(TokenKind::Newline)) {
    // Block body: `=> : NEWLINE INDENT stmts DEDENT`
    advance();  // consume `:`
    skip_newlines();
    if (match(TokenKind::Indent)) {
      while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
        skip_newlines();
        if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;
        auto stmt = parse_stmt();
        if (stmt) lambda->body_stmts.push_back(std::move(stmt));
        else synchronize();
      }
      expect_block_end("lambda");
    }
  } else {
    lambda->body_expr = parse_expr();
  }

  lambda->span = start.merge(previous_span());
  return lambda;
}

ast::Ptr<ast::MatchExpr> Parser::parse_match_expr() {
  auto mexpr = ast::make<ast::MatchExpr>();
  auto start = peek().span;

  expect(TokenKind::KwMatch);
  mexpr->subject = parse_expr();
  expect(TokenKind::Colon);
  skip_newlines();

  if (match(TokenKind::Indent)) {
    while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
      skip_newlines();
      if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;
      mexpr->arms.push_back(parse_match_arm());
    }
    expect_block_end("match");
  }

  mexpr->span = start.merge(previous_span());
  return mexpr;
}

ast::MatchArm Parser::parse_match_arm() {
  ast::MatchArm arm;
  arm.span = peek().span;

  arm.pattern = parse_pattern();

  // Optional guard: `if expr`
  if (match(TokenKind::KwIf)) {
    arm.guard = parse_expr();
  }

  // `=>` introduces the arm body.
  if (!match(TokenKind::FatArrow)) {
    emit(Diagnostic(DiagnosticLevel::Error,
                    "expected `=>` after match pattern",
                    file_id_)
             .with_label(peek().span, "expected `=>` here")
             .with_help("Each match arm uses `=>` between the pattern and "
                        "the result. For example:\n"
                        "  match x:\n"
                        "    0 => \"zero\"\n"
                        "    _ => \"other\""));
    arm.has_error = true;
  }

  // Body: inline expr or block.
  if (at(TokenKind::Colon) && peek_at(1).is(TokenKind::Newline)) {
    advance();  // consume `:`
    skip_newlines();
    if (match(TokenKind::Indent)) {
      while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
        skip_newlines();
        if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;
        auto stmt = parse_stmt();
        if (stmt) arm.body_stmts.push_back(std::move(stmt));
        else synchronize();
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

ast::Ptr<ast::IfExpr> Parser::parse_if_expr() {
  auto iexpr = ast::make<ast::IfExpr>();
  auto start = peek().span;

  expect(TokenKind::KwIf);

  // First branch.
  {
    ast::IfBranch branch;
    branch.span = start;
    branch.condition = parse_expr();
    auto body = parse_body("if");
    if (body.inline_expr) {
      auto es = ast::make<ast::ExprStmt>();
      es->expr = std::move(body.inline_expr);
      branch.body.push_back(std::move(es));
    } else {
      branch.body = std::move(body.stmts);
    }
    branch.span.extend_to(previous_span());
    iexpr->branches.push_back(std::move(branch));
  }

  // elif branches.
  while (at(TokenKind::KwElif)) {
    advance();
    ast::IfBranch branch;
    branch.span = previous_span();
    branch.condition = parse_expr();
    auto body = parse_body("elif");
    if (body.inline_expr) {
      auto es = ast::make<ast::ExprStmt>();
      es->expr = std::move(body.inline_expr);
      branch.body.push_back(std::move(es));
    } else {
      branch.body = std::move(body.stmts);
    }
    branch.span.extend_to(previous_span());
    iexpr->branches.push_back(std::move(branch));
  }

  // `else` is required for if-expressions (they must produce a value).
  if (at(TokenKind::KwElse)) {
    advance();
    auto body = parse_body("else");
    if (body.inline_expr) {
      auto es = ast::make<ast::ExprStmt>();
      es->expr = std::move(body.inline_expr);
      iexpr->else_body.push_back(std::move(es));
    } else {
      iexpr->else_body = std::move(body.stmts);
    }
  }

  iexpr->span = start.merge(previous_span());
  return iexpr;
}

ast::Ptr<ast::ForExpr> Parser::parse_for_expr() {
  auto fexpr = ast::make<ast::ForExpr>();
  auto start = peek().span;

  expect(TokenKind::KwFor);

  // Parse one or more iteration clauses.
  do {
    ast::ForExpr::IterClause clause;
    auto pats = parse_for_vars();
    for (auto& p : pats) {
      clause.patterns.push_back(std::move(p));
    }
    expect_with_context(TokenKind::KwIn, "in `for ... in ...`");
    clause.iterable = parse_expr();
    fexpr->clauses.push_back(std::move(clause));
  } while (match(TokenKind::Comma) && at(TokenKind::Ident));

  if (match(TokenKind::KwIf)) {
    fexpr->guard = parse_expr();
  }

  expect(TokenKind::FatArrow);
  fexpr->yield_expr = parse_expr();

  fexpr->span = start.merge(previous_span());
  return fexpr;
}

ast::Ptr<ast::AwaitExpr> Parser::parse_await_expr() {
  auto aexpr = ast::make<ast::AwaitExpr>();
  auto start = peek().span;

  expect(TokenKind::KwAwait);

  if (at(TokenKind::KwYield)) {
    advance();
    aexpr->is_yield = true;
  } else {
    aexpr->operand = parse_expr();
  }

  aexpr->span = start.merge(previous_span());
  return aexpr;
}

ast::Ptr<ast::ParExpr> Parser::parse_par_expr() {
  auto pexpr = ast::make<ast::ParExpr>();
  auto start = peek().span;

  expect(TokenKind::KwPar);
  expect(TokenKind::Colon);
  skip_newlines();

  if (match(TokenKind::Indent)) {
    while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
      skip_newlines();
      if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;
      auto expr = parse_expr();
      if (expr) pexpr->branches.push_back(std::move(expr));
      expect_newline();
    }
    expect_block_end("par");
  }

  pexpr->span = start.merge(previous_span());
  return pexpr;
}

ast::Ptr<ast::RaceExpr> Parser::parse_race_expr() {
  auto rexpr = ast::make<ast::RaceExpr>();
  auto start = peek().span;

  expect(TokenKind::KwRace);
  expect(TokenKind::Colon);
  skip_newlines();

  if (match(TokenKind::Indent)) {
    while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
      skip_newlines();
      if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;
      auto expr = parse_expr();
      if (expr) rexpr->branches.push_back(std::move(expr));
      expect_newline();
    }
    expect_block_end("race");
  }

  rexpr->span = start.merge(previous_span());
  return rexpr;
}

ast::Ptr<ast::OnExpr> Parser::parse_on_expr() {
  auto oexpr = ast::make<ast::OnExpr>();
  auto start = peek().span;

  expect(TokenKind::KwOn);
  expect(TokenKind::LParen);
  oexpr->context_type = parse_type_expr();

  if (match(TokenKind::Comma)) {
    oexpr->sender = parse_expr();
  }
  expect(TokenKind::RParen);

  if (at(TokenKind::Colon)) {
    auto body = parse_body("on");
    if (body.inline_expr) {
      auto es = ast::make<ast::ExprStmt>();
      es->expr = std::move(body.inline_expr);
      oexpr->body.push_back(std::move(es));
    } else {
      oexpr->body = std::move(body.stmts);
    }
  }

  oexpr->span = start.merge(previous_span());
  return oexpr;
}

ast::Ptr<ast::BlockExpr> Parser::parse_block_expr() {
  auto bexpr = ast::make<ast::BlockExpr>();
  auto start = peek().span;

  expect(TokenKind::Colon);
  skip_newlines();

  if (match(TokenKind::Indent)) {
    while (!at_any(TokenKind::Dedent, TokenKind::Eof)) {
      skip_newlines();
      if (at_any(TokenKind::Dedent, TokenKind::Eof)) break;
      auto stmt = parse_stmt();
      if (stmt) bexpr->stmts.push_back(std::move(stmt));
      else synchronize();
    }
    expect_block_end("block");
  }

  bexpr->span = start.merge(previous_span());
  return bexpr;
}

ast::Ptr<ast::QuoteExpr> Parser::parse_quote_expr() {
  auto qexpr = ast::make<ast::QuoteExpr>();
  auto start = peek().span;

  expect(TokenKind::Backtick);

  // Collect tokens until the closing backtick.
  // Handle nested backticks by tracking depth.
  int depth = 1;
  bool has_paren = match(TokenKind::LParen);

  while (!at_eof() && depth > 0) {
    if (at(TokenKind::Backtick)) {
      if (has_paren) {
        // Inside `(...)` — backtick is nested.
        qexpr->tokens.push_back(advance());
        ++depth;
      } else {
        --depth;
        if (depth == 0) break;
        qexpr->tokens.push_back(advance());
      }
    } else if (at(TokenKind::RParen) && has_paren && depth == 1) {
      advance();  // consume `)`
      has_paren = false;
    } else {
      qexpr->tokens.push_back(advance());
    }
  }

  if (at(TokenKind::Backtick)) {
    advance();  // consume closing backtick
  } else {
    emit(Diagnostic(DiagnosticLevel::Error,
                    "unterminated quote expression — expected closing `` ` ``",
                    file_id_)
             .with_label(start, "quote starts here")
             .with_help("Every opening `` ` `` needs a matching closing "
                        "`` ` ``."));
  }

  qexpr->span = start.merge(previous_span());
  return qexpr;
}

ast::Ptr<ast::SpliceExpr> Parser::parse_splice_expr_inner() {
  auto sexpr = ast::make<ast::SpliceExpr>();
  auto start = peek().span;

  expect(TokenKind::Tilde);

  if (at(TokenKind::LParen)) {
    advance();  // consume `(`
    sexpr->operand = parse_expr();
    expect(TokenKind::RParen);
  } else if (at(TokenKind::Ident)) {
    auto tok = advance();
    auto ident = ast::make<ast::IdentExpr>();
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

ast::Ptr<ast::StaticExpr> Parser::parse_static_expr() {
  auto sexpr = ast::make<ast::StaticExpr>();
  auto start = peek().span;

  expect(TokenKind::KwStatic);
  sexpr->operand = parse_expr();

  sexpr->span = start.merge(previous_span());
  return sexpr;
}

// ==========================================================================
//  Call arguments
// ==========================================================================

std::vector<ast::CallArg> Parser::parse_call_args() {
  std::vector<ast::CallArg> args;

  do {
    skip_newlines();
    if (at(TokenKind::RParen)) break;

    ast::CallArg arg;
    arg.span = peek().span;

    // Check for named argument: `name: expr`
    if (at(TokenKind::Ident) && peek_at(1).is(TokenKind::Colon)) {
      auto name_tok = advance();
      advance();  // consume `:`
      arg.name = std::string(name_tok.text);
      arg.value = parse_expr();
    } else {
      arg.value = parse_expr();
    }

    arg.span.extend_to(previous_span());
    args.push_back(std::move(arg));

    skip_newlines();
  } while (match(TokenKind::Comma));

  return args;
}

// ==========================================================================
//  Patterns
// ==========================================================================

ast::Ptr<ast::Pattern> Parser::parse_pattern() {
  auto pat = parse_or_pattern();
  if (!pat) return make_error_pattern(peek().span);

  // Optional alias: `pattern as name`
  if (at(TokenKind::KwAs)) {
    advance();
    auto name_tok = expect(TokenKind::Ident);
    // Wrap in an or_pattern with alias... for now, just annotate the
    // binding name on a wrapping group pattern.
    // TODO: proper pattern alias support
    auto group = ast::make<ast::GroupPattern>();
    group->span = pat->span.merge(name_tok.span);
    group->inner = std::move(pat);
    return group;
  }

  return pat;
}

ast::Ptr<ast::Pattern> Parser::parse_or_pattern() {
  auto first = parse_atomic_pattern();
  if (!first) return nullptr;

  if (!at(TokenKind::Pipe)) return first;

  auto or_pat = ast::make<ast::OrPattern>();
  or_pat->span = first->span;
  or_pat->alternatives.push_back(std::move(first));

  while (match(TokenKind::Pipe)) {
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
    case TokenKind::KwUnderscore:
      return parse_wildcard_pattern();

    case TokenKind::IntLit:
    case TokenKind::FloatLit:
    case TokenKind::StringLit:
    case TokenKind::CharLit:
    case TokenKind::KwTrue:
    case TokenKind::KwFalse:
    case TokenKind::KwUnit:
      return parse_literal_pattern();

    case TokenKind::Ident:
      return parse_ident_or_constructor_pattern();

    case TokenKind::LParen:
      return parse_paren_pattern();

    case TokenKind::LBrace:
      return parse_brace_pattern();

    case TokenKind::LBracket:
      return parse_bracket_pattern();

    case TokenKind::KwSome:
    case TokenKind::KwOk:
    case TokenKind::KwErr:
      return parse_option_result_pattern();

    case TokenKind::Amp:
      return parse_ref_pattern();

    case TokenKind::DotDot: {
      // Range pattern with no start: `..end`
      advance();
      auto range = ast::make<ast::RangePattern>();
      range->span = previous_span();
      if (at_any(TokenKind::IntLit, TokenKind::FloatLit, TokenKind::Ident)) {
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
  auto wc = ast::make<ast::WildcardPattern>();
  wc->span = tok.span;
  return wc;
}

ast::Ptr<ast::Pattern> Parser::parse_literal_pattern() {
  auto tok = advance();
  auto lit = ast::make<ast::LiteralPattern>();
  lit->span = tok.span;
  lit->lit_kind = tok.kind;
  lit->value = std::string(tok.text);

  // Check for range: `lit..lit` or `lit..=lit` or `lit..`
  if (at_any(TokenKind::DotDot, TokenKind::DotDotEq)) {
    bool inclusive = at(TokenKind::DotDotEq);
    advance();
    auto range = ast::make<ast::RangePattern>();
    range->span = lit->span;
    range->inclusive = inclusive;

    // Create an expression from the literal for the start.
    auto start_expr = ast::make<ast::LiteralExpr>();
    start_expr->span = lit->span;
    start_expr->lit_kind = lit->lit_kind;
    start_expr->value = std::move(lit->value);
    range->start = std::move(start_expr);

    // Parse end expression if present.
    if (peek().is_literal() || at(TokenKind::Ident)) {
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
  if (at(TokenKind::LParen)) {
    advance();  // consume `(`
    auto ctor = ast::make<ast::ConstructorPattern>();
    ctor->span = start;
    ctor->name = std::string(tok.text);

    while (!at(TokenKind::RParen) && !at_eof()) {
      auto pat = parse_pattern();
      if (pat) ctor->args.push_back(std::move(pat));
      if (!match(TokenKind::Comma)) break;
    }
    expect(TokenKind::RParen);
    ctor->span.extend_to(previous_span());
    return ctor;
  }

  // Check for range: `ident..ident`
  if (at_any(TokenKind::DotDot, TokenKind::DotDotEq)) {
    bool inclusive = at(TokenKind::DotDotEq);
    advance();
    auto range = ast::make<ast::RangePattern>();
    range->span = start;
    range->inclusive = inclusive;

    auto start_expr = ast::make<ast::IdentExpr>();
    start_expr->span = tok.span;
    start_expr->name = std::string(tok.text);
    range->start = std::move(start_expr);

    if (peek().is_literal() || at(TokenKind::Ident)) {
      range->end = parse_expr();
    }

    range->span.extend_to(previous_span());
    return range;
  }

  // Simple binding pattern.
  auto binding = ast::make<ast::BindingPattern>();
  binding->span = start;
  binding->name = std::string(tok.text);
  return binding;
}

ast::Ptr<ast::Pattern> Parser::parse_paren_pattern() {
  auto start = peek().span;
  advance();  // consume `(`

  // Empty tuple pattern.
  if (at(TokenKind::RParen)) {
    advance();
    auto tuple = ast::make<ast::TuplePattern>();
    tuple->span = start.merge(previous_span());
    return tuple;
  }

  auto first = parse_pattern();
  if (!first) {
    expect(TokenKind::RParen);
    return make_error_pattern(start.merge(previous_span()));
  }

  // Tuple pattern: `(a, b, c)`
  if (at(TokenKind::Comma)) {
    auto tuple = ast::make<ast::TuplePattern>();
    tuple->span = start;
    tuple->elements.push_back(std::move(first));

    while (match(TokenKind::Comma)) {
      if (at(TokenKind::RParen)) break;
      auto elem = parse_pattern();
      if (elem) tuple->elements.push_back(std::move(elem));
      else break;
    }

    expect(TokenKind::RParen);
    tuple->span.extend_to(previous_span());
    return tuple;
  }

  // Grouped pattern: `(pattern)`
  expect(TokenKind::RParen);
  auto group = ast::make<ast::GroupPattern>();
  group->span = start.merge(previous_span());
  group->inner = std::move(first);
  return group;
}

ast::Ptr<ast::Pattern> Parser::parse_brace_pattern() {
  auto start = peek().span;
  advance();  // consume `{`

  auto spat = ast::make<ast::StructPattern>();
  spat->span = start;

  while (!at(TokenKind::RBrace) && !at_eof()) {
    skip_newlines();
    if (at(TokenKind::RBrace)) break;

    ast::FieldPattern fp;
    fp.span = peek().span;

    if (at(TokenKind::DotDot)) {
      advance();
      fp.is_rest = true;
      spat->fields.push_back(std::move(fp));
      break;  // `..` must be last
    }

    auto name_tok = expect(TokenKind::Ident);
    fp.name = std::string(name_tok.text);

    if (match(TokenKind::Colon)) {
      fp.pattern = parse_pattern();
    }
    // else: shorthand {name}

    fp.span.extend_to(previous_span());
    spat->fields.push_back(std::move(fp));

    if (!match(TokenKind::Comma)) {
      skip_newlines();
      break;
    }
    skip_newlines();
  }

  expect(TokenKind::RBrace);
  spat->span.extend_to(previous_span());
  return spat;
}

ast::Ptr<ast::Pattern> Parser::parse_bracket_pattern() {
  auto start = peek().span;
  advance();  // consume `[`

  auto apat = ast::make<ast::ArrayPattern>();
  apat->span = start;

  while (!at(TokenKind::RBracket) && !at_eof()) {
    auto elem = parse_pattern();
    if (elem) apat->elements.push_back(std::move(elem));
    if (!match(TokenKind::Comma)) break;
  }

  expect(TokenKind::RBracket);
  apat->span.extend_to(previous_span());
  return apat;
}

ast::Ptr<ast::Pattern> Parser::parse_option_result_pattern() {
  auto start = peek().span;
  auto kw = advance();

  expect(TokenKind::LParen);
  auto inner = parse_pattern();
  if (!inner) inner = make_error_pattern(peek().span);
  expect(TokenKind::RParen);

  if (kw.kind == TokenKind::KwSome) {
    auto pat = ast::make<ast::OptionPattern>();
    pat->span = start.merge(previous_span());
    pat->option_kind = ast::OptionResultKind::Some;
    pat->inner = std::move(inner);
    return pat;
  }

  auto pat = ast::make<ast::ResultPattern>();
  pat->span = start.merge(previous_span());
  pat->result_kind = (kw.kind == TokenKind::KwOk)
                         ? ast::OptionResultKind::Ok
                         : ast::OptionResultKind::Err;
  pat->inner = std::move(inner);
  return pat;
}

ast::Ptr<ast::Pattern> Parser::parse_ref_pattern() {
  auto start = peek().span;
  advance();  // consume `&`

  auto inner = parse_pattern();
  if (!inner) {
    emit_unexpected("a pattern after `&`");
    inner = make_error_pattern(start);
  }

  auto ref_pat = ast::make<ast::RefPattern>();
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

  while (match(TokenKind::Comma)) {
    // Check if the next thing is `in` — if so, the comma was part of
    // the for syntax, not another variable.
    if (at(TokenKind::KwIn)) break;
    patterns.push_back(parse_pattern());
  }

  return patterns;
}

// ==========================================================================
//  Operator conversion helpers
// ==========================================================================

std::optional<ast::AssignOp> Parser::token_to_assign_op(
    TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::Eq:             return ast::AssignOp::Assign;
    case TokenKind::PlusEq:         return ast::AssignOp::AddAssign;
    case TokenKind::MinusEq:        return ast::AssignOp::SubAssign;
    case TokenKind::StarEq:         return ast::AssignOp::MulAssign;
    case TokenKind::SlashEq:        return ast::AssignOp::DivAssign;
    case TokenKind::PercentEq:      return ast::AssignOp::ModAssign;
    case TokenKind::AmpEq:          return ast::AssignOp::AndAssign;
    case TokenKind::PipeEq:         return ast::AssignOp::OrAssign;
    case TokenKind::CaretEq:        return ast::AssignOp::XorAssign;
    case TokenKind::LtLtEq:         return ast::AssignOp::ShlAssign;
    case TokenKind::GtGtEq:         return ast::AssignOp::ShrAssign;
    case TokenKind::PlusPercentEq:  return ast::AssignOp::AddWrapAssign;
    case TokenKind::MinusPercentEq: return ast::AssignOp::SubWrapAssign;
    case TokenKind::StarPercentEq:  return ast::AssignOp::MulWrapAssign;
    case TokenKind::PlusPipeEq:     return ast::AssignOp::AddSatAssign;
    case TokenKind::MinusPipeEq:    return ast::AssignOp::SubSatAssign;
    case TokenKind::StarPipeEq:     return ast::AssignOp::MulSatAssign;
    default:                        return std::nullopt;
  }
}

std::optional<ast::BinaryOp> Parser::token_to_cmp_op() {
  switch (current()) {
    case TokenKind::EqEq:    return ast::BinaryOp::EqEq;
    case TokenKind::BangEq:  return ast::BinaryOp::BangEq;
    case TokenKind::Lt:      return ast::BinaryOp::Lt;
    case TokenKind::LtEq:    return ast::BinaryOp::LtEq;
    case TokenKind::Gt:      return ast::BinaryOp::Gt;
    case TokenKind::GtEq:    return ast::BinaryOp::GtEq;
    case TokenKind::KwIn:    return ast::BinaryOp::In;
    case TokenKind::KwNot:
      // `not in` — peek ahead.
      if (peek_at(1).is(TokenKind::KwIn)) {
        return ast::BinaryOp::NotIn;
      }
      return std::nullopt;
    default:                 return std::nullopt;
  }
}

std::optional<ast::BinaryOp> Parser::token_to_add_op(
    TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::Plus:          return ast::BinaryOp::Add;
    case TokenKind::Minus:         return ast::BinaryOp::Sub;
    case TokenKind::PlusPercent:   return ast::BinaryOp::AddWrap;
    case TokenKind::MinusPercent:  return ast::BinaryOp::SubWrap;
    case TokenKind::PlusPipe:      return ast::BinaryOp::AddSat;
    case TokenKind::MinusPipe:     return ast::BinaryOp::SubSat;
    default:                       return std::nullopt;
  }
}

std::optional<ast::BinaryOp> Parser::token_to_mul_op(
    TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::Star:          return ast::BinaryOp::Mul;
    case TokenKind::Slash:         return ast::BinaryOp::Div;
    case TokenKind::Percent:       return ast::BinaryOp::Mod;
    case TokenKind::StarPercent:   return ast::BinaryOp::MulWrap;
    case TokenKind::StarPipe:      return ast::BinaryOp::MulSat;
    default:                       return std::nullopt;
  }
}

}  // namespace kira