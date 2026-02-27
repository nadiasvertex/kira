#pragma once

// ==========================================================================
//  Kira Language — Recursive Descent Parser
//
//  Design philosophy:
//    The compiler is a teacher, not an enemy. Every error message should
//    help the user understand what went wrong and how to fix it. We never
//    just say "syntax error" — we explain what we expected, what we found,
//    and what the user probably meant.
//
//  Error recovery strategy:
//    We use a combination of techniques to keep parsing after errors:
//
//    1. **Synchronization**: When we hit an unexpected token, we skip
//       forward to a "synchronization point" — a token that's likely to
//       start the next valid construct (e.g., a keyword like `def`, `let`,
//       `type`, or a DEDENT/NEWLINE). This prevents one error from
//       cascading into dozens of follow-up errors.
//
//    2. **Error nodes**: When a production fails, we insert an error_node
//       (error_expr, error_pattern, error_stmt) into the AST. This keeps the
//       tree structurally valid so downstream phases can still analyze the
//       parts that parsed correctly.
//
//    3. **Expected-token recovery**: When we expect a specific token (like
//       `)` or `:`), we check if the token we found is plausibly correct
//       for a *later* production. If so, we insert the missing token and
//       continue — the user gets a "missing X" diagnostic rather than a
//       confusing "unexpected Y" one.
//
//    4. **Panic-mode with anchor sets**: Each parsing function receives
//       (implicitly via the parser state) a set of "anchor" tokens that
//       mark safe recovery points. When we need to skip tokens, we stop
//       at any anchor token rather than skipping past the entire file.
//
//    5. **Error budgeting**: We limit the total number of errors to
//       prevent overwhelming the user. After the limit, we stop reporting
//       new errors but continue parsing so the user gets a complete set
//       of *good* diagnostics for the first N problems.
// ==========================================================================

#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast.h"
#include "diagnostic.h"
#include "lexer.h"
#include "source_location.h"
#include "token.h"

namespace kira {

// ==========================================================================
//  ParseResult — wraps an AST node pointer with success/failure status.
//
//  This is used throughout the parser to distinguish between:
//    - A successfully parsed node (possibly with warnings).
//    - A node that was produced via error recovery (has_error = true on
//      the node itself).
//    - A complete failure where no node could be produced at all
//      (nullptr).
//
//  The parser always tries to return *something* rather than nullptr,
//  because having a partially-correct AST is more useful than having
//  holes. The has_error flag on each node lets later phases skip broken
//  subtrees.
// ==========================================================================
template <typename T>
struct ParseResult {
  ast::ptr<T> node;

  ParseResult() = default;
  explicit ParseResult(ast::ptr<T> n) : node(std::move(n)) {}
  ParseResult(std::nullptr_t) : node(nullptr) {}

  [[nodiscard]] bool ok() const noexcept {
    return node != nullptr && !node->has_error;
  }

  [[nodiscard]] bool has_value() const noexcept {
    return node != nullptr;
  }

  [[nodiscard]] bool has_error() const noexcept {
    return node == nullptr || node->has_error;
  }

  explicit operator bool() const noexcept { return has_value(); }

  T* operator->() { return node.get(); }
  const T* operator->() const { return node.get(); }
  T& operator*() { return *node; }
  const T& operator*() const { return *node; }

  ast::ptr<T> take() { return std::move(node); }
};

// ==========================================================================
//  Parser — recursive descent parser for the Kira language.
//
//  Usage:
//    source_file file(0, "example.kira", source_text);
//    diagnostic_bag diag;
//    Lexer lexer(source_text, 0, diag);
//    auto tokens = lexer.tokenize();
//    Parser parser(tokens, 0, diag);
//    auto ast = parser.parse_file();
//
//  The parser consumes the token stream produced by the Lexer and builds
//  an AST. It reports diagnostics to a diagnostic_bag and uses error
//  recovery to keep going after syntax errors.
// ==========================================================================
class Parser {
 public:
  /// Construct a parser over a token stream.
  ///
  /// @param tokens   The token stream (must end with token_kind::eof).
  /// @param file_id  The source file identifier for diagnostics.
  /// @param diag     The diagnostic bag to report errors into.
  Parser(std::vector<Token> tokens, FileId file_id, diagnostic_bag& diag)
      : tokens_(std::move(tokens)),
        file_id_(file_id),
        diag_(diag),
        pos_(0) {
    assert(!tokens_.empty() && "token stream must contain at least Eof");
    assert(tokens_.back().kind == token_kind::eof &&
           "token stream must end with Eof");
  }

  // ========================================================================
  //  Top-level entry point
  // ========================================================================

  /// Parse a complete Kira source file.
  [[nodiscard]] ast::ptr<ast::file> parse_file();

  // ========================================================================
  //  Accessors
  // ========================================================================

  /// Returns true if any errors were encountered during parsing.
  [[nodiscard]] bool has_errors() const noexcept {
    return diag_.has_errors();
  }

  /// Returns the number of errors encountered.
  [[nodiscard]] uint32_t error_count() const noexcept {
    return diag_.error_count();
  }

 private:
  // ========================================================================
  //  Token stream navigation
  //
  //  These are the fundamental building blocks. Every parsing function
  //  uses these to inspect and consume tokens. They're designed to be
  //  safe (never read past EOF) and informative (diagnostics always
  //  know where they are).
  // ========================================================================

  /// Returns the current token without consuming it.
  [[nodiscard]] const Token& peek() const noexcept {
    return tokens_[pos_];
  }

  /// Returns the token `offset` positions ahead without consuming.
  [[nodiscard]] const Token& peek_at(uint32_t offset) const noexcept {
    auto idx = static_cast<size_t>(pos_) + offset;
    if (idx >= tokens_.size()) idx = tokens_.size() - 1;  // clamp to Eof
    return tokens_[idx];
  }

  /// Returns the kind of the current token.
  [[nodiscard]] token_kind current() const noexcept {
    return peek().kind;
  }

  /// Returns true if the current token has the given kind.
  [[nodiscard]] bool at(token_kind kind) const noexcept {
    return current() == kind;
  }

  /// Returns true if the current token is one of the given kinds.
  template <typename... Kinds>
  [[nodiscard]] bool at_any(Kinds... kinds) const noexcept {
    return ((current() == kinds) || ...);
  }

  /// Returns true if the current token is EOF.
  [[nodiscard]] bool at_eof() const noexcept {
    return at(token_kind::eof);
  }

  /// Consume the current token and return it.
  Token advance() noexcept {
    Token tok = tokens_[pos_];
    if (pos_ < tokens_.size() - 1) ++pos_;
    return tok;
  }

  /// Consume the current token if it matches `kind`, and return true.
  /// Otherwise return false without consuming.
  bool match(token_kind kind) noexcept {
    if (at(kind)) {
      advance();
      return true;
    }
    return false;
  }

  /// If the current token matches, consume it and return it.
  /// Otherwise return std::nullopt.
  std::optional<Token> try_consume(token_kind kind) noexcept {
    if (at(kind)) {
      return advance();
    }
    return std::nullopt;
  }

  /// Return the span of the previous token. Used for building spans
  /// that end "just after" the last consumed token.
  [[nodiscard]] Span previous_span() const noexcept {
    if (pos_ == 0) return Span::dummy();
    return tokens_[pos_ - 1].span;
  }

  /// Return the previous token.
  [[nodiscard]] const Token& previous() const noexcept {
    if (pos_ == 0) return tokens_[0];
    return tokens_[pos_ - 1];
  }

  // ========================================================================
  //  Newline handling
  //
  //  Kira uses newlines as statement terminators, but they can often be
  //  inferred or skipped in certain contexts. These helpers manage that.
  // ========================================================================

  /// Skip zero or more NEWLINE tokens.
  void skip_newlines() noexcept {
    while (at(token_kind::newline)) advance();
  }

  /// Expect and consume a NEWLINE. If missing, emit a diagnostic but
  /// continue — the user probably just forgot it.
  bool expect_newline();

  // ========================================================================
  //  Error recovery infrastructure
  //
  //  This is the heart of our friendly parser. Instead of aborting on
  //  the first error, we try to understand what went wrong and keep
  //  going.
  // ========================================================================

  /// Expect a specific token kind. If the current token doesn't match:
  ///   1. Emit a friendly diagnostic explaining what was expected.
  ///   2. Check if the current token plausibly starts a later production
  ///      (in which case the expected token was probably just missing).
  ///   3. If not, consume the unexpected token.
  /// Returns the consumed token (or a synthetic placeholder if we
  /// inserted one during recovery).
  Token expect(token_kind expected);

  /// Like expect(), but also provides a custom message for the diagnostic.
  /// The message explains *why* the token is expected, giving the user
  /// more context about what construct we're in the middle of parsing.
  Token expect_with_context(token_kind expected, std::string_view context);

  /// Emit a diagnostic for an unexpected token. This is the main
  /// "something went wrong" entry point. The message is constructed
  /// to be as helpful as possible.
  void emit_unexpected(std::string_view expected_description);

  /// Emit a full diagnostic object.
  void emit(diagnostic diag) { diag_.emit(std::move(diag)); }

  /// Build a diagnostic for the current position.
  [[nodiscard]] diagnostic make_error(std::string message) const {
    return diagnostic(diagnostic_level::error, std::move(message), file_id_);
  }

  /// Build a diagnostic for a specific span.
  [[nodiscard]] diagnostic make_error_at(Span span, std::string message) const {
    return diagnostic(diagnostic_level::error, std::move(message), file_id_)
        .with_label(span, "here");
  }

  /// Synchronize: skip tokens until we find one that looks like it could
  /// start a new statement, declaration, or the end of a block. This is
  /// our main panic-mode recovery: we give up on the current construct
  /// and try to resume at the next one.
  void synchronize();

  /// Synchronize to the end of the current statement (skip until NEWLINE
  /// or DEDENT).
  void synchronize_to_newline();

  /// Synchronize to a set of specific token kinds.
  template <typename... Kinds>
  void synchronize_to(Kinds... kinds) {
    while (!at_eof() && !((at(kinds)) || ...)) {
      advance();
    }
  }

  /// Skip to the end of the current block (matching INDENT/DEDENT pairs).
  void skip_block();

  /// Returns true if the current token looks like it starts a new
  /// statement or declaration. Used during recovery to decide whether
  /// to insert a missing token or skip forward.
  [[nodiscard]] bool at_recovery_point() const noexcept;

  // ========================================================================
  //  Block parsing helpers
  //
  //  Many Kira constructs use the same pattern:
  //    `:` NEWLINE INDENT body DEDENT
  //  or the inline form:
  //    `:` expr NEWLINE
  //
  //  These helpers factor out the common block-parsing logic.
  // ========================================================================

  /// Expect `:` NEWLINE INDENT, reporting friendly errors if any part
  /// is missing. Returns true if we successfully entered a block.
  bool expect_block_start(std::string_view construct_name);

  /// Expect DEDENT at the end of a block. Handles missing DEDENT
  /// gracefully.
  bool expect_block_end(std::string_view construct_name);

  /// Parse a block body: a sequence of statements between INDENT/DEDENT.
  /// The `parse_item` callback is invoked for each item in the block.
  /// Returns the list of parsed items.
  template <typename T>
  std::vector<ast::ptr<T>> parse_block(
      std::string_view construct_name,
      std::function<ast::ptr<T>()> parse_item);

  /// Parse either an inline body (`:` expr NEWLINE) or a block body
  /// (`:` NEWLINE INDENT stmts DEDENT). Returns the body as a vector
  /// of statement nodes. For inline form, wraps the expr in an expr_stmt.
  struct BodyResult {
    ast::ptr<ast::Expr> inline_expr;
    std::vector<ast::ptr<ast::node>> stmts;
    bool is_block = false;
  };
  BodyResult parse_body(std::string_view construct_name);

  // ========================================================================
  //  Comma-separated list helper
  // ========================================================================

  /// Parse a comma-separated list between `open` and `close` delimiters.
  /// Handles trailing commas, missing commas (with helpful diagnostics),
  /// and unclosed delimiters.
  template <typename T>
  std::vector<T> parse_delimited_list(
      token_kind open, token_kind close,
      std::string_view construct_name,
      std::function<std::optional<T>()> parse_element);

  /// Parse a comma-separated list without surrounding delimiters.
  /// Stops when the current token can't start a new element.
  template <typename T>
  std::vector<T> parse_comma_list(
      std::function<std::optional<T>()> parse_element,
      std::function<bool()> at_end);

  // ========================================================================
  //  Visibility
  // ========================================================================

  /// If the current token is a visibility keyword, consume it and return
  /// the visibility. Otherwise return Default.
  [[nodiscard]] ast::visibility parse_optional_visibility();

  // ========================================================================
  //  Module paths — `a.b.c`
  // ========================================================================

  /// Parse a dotted module path: `IDENT { "." IDENT }`.
  [[nodiscard]] std::vector<std::string> parse_module_path();

  // ========================================================================
  //  Top-level items
  // ========================================================================

  /// Parse a single top-level item (use, type, trait, impl, func, etc.).
  [[nodiscard]] ast::ptr<ast::node> parse_top_level_item();

  // ---- Module / use / dep ----

  [[nodiscard]] ast::ptr<ast::module_decl> parse_module_decl();
  [[nodiscard]] ast::ptr<ast::use_decl> parse_use_decl(ast::visibility vis);
  [[nodiscard]] ast::ptr<ast::sub_module_decl> parse_sub_module_decl(
      ast::visibility vis);
  [[nodiscard]] ast::ptr<ast::dep_decl> parse_dep_decl();

  // ---- Type declarations ----

  [[nodiscard]] ast::ptr<ast::type_decl> parse_type_decl(ast::visibility vis);
  [[nodiscard]] ast::ptr<ast::node> parse_type_def();
  [[nodiscard]] ast::struct_body parse_struct_body();
  [[nodiscard]] ast::sum_body parse_sum_body();
  [[nodiscard]] ast::struct_field parse_struct_field();
  [[nodiscard]] ast::sum_variant parse_sum_variant();

  // ---- Type expressions ----

  [[nodiscard]] ast::ptr<ast::type_expr> parse_type_expr();
  [[nodiscard]] ast::ptr<ast::type_expr> parse_prim_type_expr();
  [[nodiscard]] ast::ptr<ast::named_type> parse_named_type();
  [[nodiscard]] ast::ptr<ast::tuple_type> parse_tuple_type();
  [[nodiscard]] ast::ptr<ast::slice_type> parse_slice_type();
  [[nodiscard]] ast::ptr<ast::array_type> parse_array_type();
  [[nodiscard]] ast::ptr<ast::ref_type> parse_ref_type();
  [[nodiscard]] ast::ptr<ast::ptr_type> parse_ptr_type();
  [[nodiscard]] ast::ptr<ast::fn_type> parse_fn_type();

  // ---- Type parameters and bounds ----

  [[nodiscard]] std::vector<ast::type_param> parse_type_params();
  [[nodiscard]] ast::type_param parse_type_param();
  [[nodiscard]] ast::Bound parse_bound();
  [[nodiscard]] ast::bound_term parse_bound_term();
  [[nodiscard]] std::vector<ast::where_constraint> parse_where_clause();

  // ---- Trait / concept / impl declarations ----

  [[nodiscard]] ast::ptr<ast::trait_decl> parse_trait_decl(
      ast::visibility vis);
  [[nodiscard]] ast::ptr<ast::concept_decl> parse_concept_decl(
      ast::visibility vis);
  [[nodiscard]] ast::ptr<ast::impl_decl> parse_impl_decl();

  // ---- Function declarations ----

  [[nodiscard]] ast::ptr<ast::func_decl> parse_func_decl(
      ast::visibility vis, ast::func_modifiers mods);
  [[nodiscard]] ast::func_modifiers parse_func_modifiers();
  [[nodiscard]] std::vector<ast::Param> parse_param_list();
  [[nodiscard]] ast::Param parse_param();
  [[nodiscard]] std::vector<ast::contract_clause> parse_contract_clauses();
  [[nodiscard]] ast::contract_clause parse_contract_clause();

  // ---- Static declarations ----

  [[nodiscard]] ast::ptr<ast::static_decl> parse_static_decl(
      ast::visibility vis);

  // ========================================================================
  //  Statements
  // ========================================================================

  [[nodiscard]] ast::ptr<ast::node> parse_stmt();
  [[nodiscard]] ast::ptr<ast::let_stmt> parse_let_stmt();
  [[nodiscard]] ast::ptr<ast::var_stmt> parse_var_stmt();
  [[nodiscard]] ast::ptr<ast::return_stmt> parse_return_stmt();
  [[nodiscard]] ast::ptr<ast::if_stmt> parse_if_stmt();
  [[nodiscard]] ast::ptr<ast::while_stmt> parse_while_stmt();
  [[nodiscard]] ast::ptr<ast::for_stmt> parse_for_stmt();
  [[nodiscard]] ast::ptr<ast::match_stmt> parse_match_stmt();
  [[nodiscard]] ast::ptr<ast::crew_stmt> parse_crew_stmt();
  [[nodiscard]] ast::ptr<ast::asm_stmt> parse_asm_stmt();
  [[nodiscard]] ast::ptr<ast::splice_stmt> parse_splice_stmt();

  /// Parse an expression statement or assignment statement.
  /// We parse the LHS as an expression, then check if it's followed
  /// by an assignment operator.
  [[nodiscard]] ast::ptr<ast::node> parse_expr_or_assign_stmt();

  // ========================================================================
  //  Expressions
  //
  //  The expression grammar is organized by precedence, highest to lowest:
  //    pipe_expr       →  or_expr { "|" or_expr }
  //    or_expr         →  and_expr { "or" and_expr }
  //    and_expr        →  not_expr { "and" not_expr }
  //    not_expr        →  "not" not_expr | cmp_expr
  //    cmp_expr        →  add_expr { cmp_op add_expr }
  //    add_expr        →  mul_expr { add_op mul_expr }
  //    mul_expr        →  unary_expr { mul_op unary_expr }
  //    unary_expr      →  unary_op unary_expr | postfix_expr
  //    postfix_expr    →  primary_expr { postfix_suffix }
  //    primary_expr    →  literal | ident | (...) | [...] | {...} | ...
  // ========================================================================

  [[nodiscard]] ast::ptr<ast::Expr> parse_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_pipe_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_or_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_and_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_not_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_cmp_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_add_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_mul_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_unary_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_postfix_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_primary_expr();

  // ---- Primary expression sub-parsers ----

  [[nodiscard]] ast::ptr<ast::Expr> parse_literal_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_ident_or_path_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_paren_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_bracket_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_brace_expr();
  [[nodiscard]] ast::ptr<ast::Expr> parse_lambda_expr();
  [[nodiscard]] ast::ptr<ast::match_expr> parse_match_expr();
  [[nodiscard]] ast::ptr<ast::if_expr> parse_if_expr();
  [[nodiscard]] ast::ptr<ast::for_expr> parse_for_expr();
  [[nodiscard]] ast::ptr<ast::await_expr> parse_await_expr();
  [[nodiscard]] ast::ptr<ast::par_expr> parse_par_expr();
  [[nodiscard]] ast::ptr<ast::race_expr> parse_race_expr();
  [[nodiscard]] ast::ptr<ast::on_expr> parse_on_expr();
  [[nodiscard]] ast::ptr<ast::block_expr> parse_block_expr();
  [[nodiscard]] ast::ptr<ast::quote_expr> parse_quote_expr();
  [[nodiscard]] ast::ptr<ast::splice_expr> parse_splice_expr_inner();
  [[nodiscard]] ast::ptr<ast::static_expr> parse_static_expr();

  // ---- Postfix suffixes ----

  /// Try to parse a postfix suffix (`.field`, `[index]`, `(args)`,
  /// `?`, `as Type`). Returns nullptr if the current token doesn't
  /// start a postfix suffix.
  [[nodiscard]] ast::ptr<ast::Expr> parse_postfix_suffix(
      ast::ptr<ast::Expr> base);

  // ---- Call arguments ----

  [[nodiscard]] std::vector<ast::call_arg> parse_call_args();

  // ---- Match arms ----

  [[nodiscard]] ast::match_arm parse_match_arm();

  // ========================================================================
  //  Patterns
  // ========================================================================

  [[nodiscard]] ast::ptr<ast::Pattern> parse_pattern();
  [[nodiscard]] ast::ptr<ast::Pattern> parse_or_pattern();
  [[nodiscard]] ast::ptr<ast::Pattern> parse_atomic_pattern();
  [[nodiscard]] std::optional<std::string> parse_pattern_alias();

  // ---- Pattern sub-parsers ----

  [[nodiscard]] ast::ptr<ast::Pattern> parse_wildcard_pattern();
  [[nodiscard]] ast::ptr<ast::Pattern> parse_literal_pattern();
  [[nodiscard]] ast::ptr<ast::Pattern> parse_ident_or_constructor_pattern();
  [[nodiscard]] ast::ptr<ast::Pattern> parse_paren_pattern();
  [[nodiscard]] ast::ptr<ast::Pattern> parse_brace_pattern();
  [[nodiscard]] ast::ptr<ast::Pattern> parse_bracket_pattern();
  [[nodiscard]] ast::ptr<ast::Pattern> parse_option_result_pattern();
  [[nodiscard]] ast::ptr<ast::Pattern> parse_ref_pattern();

  // ========================================================================
  //  For-loop variable parsing (shared by for-stmt and for-expr)
  // ========================================================================

  [[nodiscard]] std::vector<ast::ptr<ast::Pattern>> parse_for_vars();

  // ========================================================================
  //  Helper: convert an assign-op token to the AssignOp enum.
  // ========================================================================

  [[nodiscard]] static std::optional<ast::AssignOp> token_to_assign_op(
      token_kind kind) noexcept;

  // ========================================================================
  //  Helper: convert comparison token to BinaryOp.
  // ========================================================================

  [[nodiscard]] std::optional<ast::BinaryOp> token_to_cmp_op();
  [[nodiscard]] static std::optional<ast::BinaryOp> token_to_add_op(
      token_kind kind) noexcept;
  [[nodiscard]] static std::optional<ast::BinaryOp> token_to_mul_op(
      token_kind kind) noexcept;

  // ========================================================================
  //  Member variables
  // ========================================================================

  /// The token stream, ending with Eof.
  std::vector<Token> tokens_;

  /// The source file identifier for diagnostics.
  FileId file_id_;

  /// The diagnostic bag we report errors to.
  diagnostic_bag& diag_;

  /// Current position in the token stream.
  uint32_t pos_;
};

}  // namespace kira
