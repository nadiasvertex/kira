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
/// @brief Wraps a parsed AST node together with recovery-aware success queries.
///
/// Parser routines use `parse_result` when callers need to distinguish between
/// three outcomes that all carry different downstream meaning: a clean parse, a
/// recovered parse that still produced a node, and a hard failure that produced
/// nothing. Later phases should prefer checking `node->has_error` on the stored
/// AST node before performing semantic work.
///
/// @tparam T Concrete AST node type produced by the parse routine.
template <typename T> struct parse_result {
  ast::ptr<T> node; ///< Parsed node, possibly marked as recovered.

  /// Creates an empty parse result.
  parse_result() = default;
  /// Wraps a parsed node.
  ///
  /// @param n Parsed AST node to own.
  explicit parse_result(ast::ptr<T> n) : node(std::move(n)) {}
  /// Creates an explicit failure result with no node.
  parse_result(std::nullptr_t) : node(nullptr) {}

  /// @brief Returns whether parsing produced a non-error node.
  [[nodiscard]] auto ok() const noexcept -> bool {
    return node != nullptr && !node->has_error;
  }

  /// @brief Returns whether parsing produced any node at all.
  [[nodiscard]] auto has_value() const noexcept -> bool { return node != nullptr; }

  /// @brief Returns whether the parse failed or required recovery.
  [[nodiscard]] auto has_error() const noexcept -> bool {
    return node == nullptr || node->has_error;
  }

  /// Allows `if (result)` checks for whether a node exists.
  explicit operator bool() const noexcept { return has_value(); }

  /// Returns mutable access to the wrapped node.
  auto operator->() -> T * { return node.get(); }
  /// Returns const access to the wrapped node.
  auto operator->() const -> const T * { return node.get(); }
  /// Dereferences the wrapped node.
  auto operator*() -> T & { return *node; }
  /// Dereferences the wrapped node.
  auto operator*() const -> const T & { return *node; }

  /// @brief Moves the owned node out of the result.
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
class parser {
public:
  /// Construct a parser over a token stream.
  ///
  /// @param tokens   The token stream (must end with token_kind::eof).
  /// @param file_id  The source file identifier for diagnostics.
  /// @param diag     The diagnostic bag to report errors into.
  parser(std::vector<token> tokens, file_id_type file_id, diagnostic_bag &diag)
      : tokens_(std::move(tokens)), file_id_(file_id), diag_(diag) {
    assert(!tokens_.empty() && "token stream must contain at least Eof");
    assert(tokens_.back().kind == token_kind::eof &&
           "token stream must end with Eof");
  }

  // ========================================================================
  //  Top-level entry point
  // ========================================================================

  /// @brief Parses a complete Kira source file into the root AST node.
  ///
  /// This is the only public entry point for syntax analysis. It consumes the
  /// pre-tokenized stream, accumulates diagnostics into `diag_`, and returns a
  /// structurally valid file node even when recovery was required.
  [[nodiscard]] ast::ptr<ast::file> parse_file();

  // ========================================================================
  //  Accessors
  // ========================================================================

  /// @brief Returns whether parsing emitted any error diagnostics.
  [[nodiscard]] auto has_errors() const noexcept -> bool { return diag_.has_errors(); }

  /// @brief Returns the number of parse errors reported so far.
  [[nodiscard]] auto error_count() const noexcept -> uint32_t {
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

  /// @brief Returns the current token without consuming it.
  [[nodiscard]] auto peek() const noexcept -> const token & { return tokens_[pos_]; }

  /// @brief Returns the token `offset` positions ahead without consuming it.
  ///
  /// Lookahead is cheap because the lexer produced the full token vector up
  /// front. Callers should still treat the result as read-only grammar context.
  ///
  /// @param offset Relative lookahead distance from the current token.
  [[nodiscard]] auto peek_at(uint32_t offset) const noexcept -> const token & {
    auto idx = static_cast<size_t>(pos_) + offset;
    if (idx >= tokens_.size())
      idx = tokens_.size() - 1; // clamp to Eof
    return tokens_[idx];
  }

  /// @brief Returns the kind of the current token.
  [[nodiscard]] auto current() const noexcept -> token_kind { return peek().kind; }

  /// @brief Returns whether the current token has kind `kind`.
  ///
  /// @param kind Token kind to compare with the current token.
  [[nodiscard]] auto at(token_kind kind) const noexcept -> bool {
    return current() == kind;
  }

  /// @brief Returns whether the current token matches any provided kind.
  ///
  /// @tparam Kinds Token-kind enum parameters.
  /// @param kinds Candidate token kinds.
  template <typename... Kinds>
  [[nodiscard]] auto at_any(Kinds... kinds) const noexcept -> bool {
    return ((current() == kinds) || ...);
  }

  /// @brief Returns whether the parser is positioned at the EOF sentinel.
  [[nodiscard]] auto at_eof() const noexcept -> bool { return at(token_kind::eof); }

  /// @brief Consumes and returns the current token.
  ///
  /// Advancing is clamped at the EOF sentinel so recovery code can call it
  /// freely without risking out-of-bounds access.
  auto advance() noexcept -> token {
    token tok = tokens_[pos_];
    if (pos_ < tokens_.size() - 1) {
      ++pos_;
}
    return tok;
  }

  /// @brief Consumes the current token if it matches `kind`.
  ///
  /// @param kind Token kind to accept.
  /// @return `true` when the token was consumed.
  auto match(token_kind kind) noexcept -> bool {
    if (at(kind)) {
      advance();
      return true;
    }
    return false;
  }

  /// @brief Consumes and returns the current token if it matches `kind`.
  ///
  /// This is useful when callers need the consumed token's span or text but
  /// still want optional control flow.
  ///
  /// @param kind Token kind to accept.
  auto try_consume(token_kind kind) noexcept -> std::optional<token> {
    if (at(kind)) {
      return advance();
    }
    return std::nullopt;
  }

  /// @brief Returns the span of the most recently consumed token.
  ///
  /// Span-building code uses this to close nodes after the parser has already
  /// advanced beyond their final token.
  [[nodiscard]] source_span previous_span() const noexcept {
    if (pos_ == 0)
      return source_span::dummy();
    return tokens_[pos_ - 1].span;
  }

  /// @brief Returns the most recently consumed token.
  [[nodiscard]] auto previous() const noexcept -> const token & {
    if (pos_ == 0)
      return tokens_[0];
    return tokens_[pos_ - 1];
  }

  // ========================================================================
  //  Newline handling
  //
  //  Kira uses newlines as statement terminators, but they can often be
  //  inferred or skipped in certain contexts. These helpers manage that.
  // ========================================================================

  /// @brief Consumes any contiguous logical newlines.
  ///
  /// Many productions allow optional blank lines between constructs. Centralizing
  /// that policy keeps individual grammar functions simpler and more consistent.
  void skip_newlines() noexcept {
    while (at(token_kind::newline)) {
      advance();
}
  }

  /// @brief Requires a logical newline, recovering if one was omitted.
  ///
  /// Statement-oriented grammar uses this to preserve line sensitivity without
  /// making a missing newline fatal.
  auto expect_newline() -> bool;

  // ========================================================================
  //  Error recovery infrastructure
  //
  //  This is the heart of our friendly parser. Instead of aborting on
  //  the first error, we try to understand what went wrong and keep
  //  going.
  // ========================================================================

  /// @brief Requires a specific token kind and performs local recovery if absent.
  ///
  /// This is the parser's main "missing token" mechanism. Callers get back a
  /// real token when possible or a synthesized placeholder when recovery inserts
  /// the expected punctuation to preserve tree shape.
  ///
  /// @param expected Token kind required by the current grammar production.
  auto expect(token_kind expected) -> token;

  /// @brief Like `expect`, but with extra user-facing context in diagnostics.
  ///
  /// @param expected Token kind required by the current grammar production.
  /// @param context Explanation of the surrounding construct.
  auto expect_with_context(token_kind expected, std::string_view context) -> token;

  /// @brief Emits a friendly unexpected-token diagnostic at the current position.
  ///
  /// @param expected_description User-facing description of the missing syntax.
  void emit_unexpected(std::string_view expected_description);

  /// @brief Forwards a fully constructed diagnostic into the shared bag.
  void emit(diagnostic diag) { diag_.emit(std::move(diag)); }

  /// @brief Creates an error diagnostic anchored to the current file.
  ///
  /// @param message Primary user-facing message.
  [[nodiscard]] auto make_error(std::string message) const -> diagnostic {
    return diagnostic(diagnostic_level::error, std::move(message), file_id_);
  }

  /// @brief Creates an error diagnostic already labeled at `span`.
  ///
  /// @param span Source range to highlight.
  /// @param message Primary user-facing message.
  [[nodiscard]] auto make_error_at(source_span span, std::string message) const -> diagnostic {
    return diagnostic(diagnostic_level::error, std::move(message), file_id_)
        .with_label(span, "here");
  }

  /// @brief Skips ahead to a token that plausibly starts a fresh construct.
  ///
  /// This is the parser's coarse panic-mode recovery boundary for file and block
  /// parsing.
  void synchronize();

  /// @brief Skips to the next logical statement boundary.
  void synchronize_to_newline();

  /// @brief Skips tokens until one of the specified anchors is reached.
  ///
  /// @tparam Kinds Token-kind enum parameters.
  /// @param kinds Tokens that are safe to resume parsing at.
  template <typename... Kinds> void synchronize_to(Kinds... kinds) {
    while (!at_eof() && !((at(kinds)) || ...)) {
      advance();
    }
  }

  /// @brief Skips to the end of the current indentation-delimited block.
  void skip_block();

  /// @brief Returns whether the current token is a safe recovery boundary.
  ///
  /// Missing-token recovery consults this before deciding whether to insert the
  /// missing syntax or keep discarding input.
  [[nodiscard]] auto at_recovery_point() const noexcept -> bool;

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

  /// @brief Requires the standard block introducer sequence for a construct.
  ///
  /// @param construct_name User-facing name of the construct owning the block.
  auto expect_block_start(std::string_view construct_name) -> bool;

  /// @brief Requires the closing `dedent` for an indentation block.
  ///
  /// @param construct_name User-facing name of the construct owning the block.
  auto expect_block_end(std::string_view construct_name) -> bool;

  /// @brief Parses an indentation-delimited block as a homogeneous item list.
  ///
  /// This lets many grammar forms share the same layout handling while deciding
  /// themselves what an individual block item means.
  ///
  /// @tparam T AST node type returned by `parse_item`.
  /// @param construct_name User-facing name for diagnostics.
  /// @param parse_item Callback used to parse each block element.
  template <typename T>
  std::vector<ast::ptr<T>> parse_block(std::string_view construct_name,
                                       std::function<ast::ptr<T>()> parse_item);

  /// @brief Normalized representation of either inline or block bodies.
  ///
  /// Many constructs in Kira allow both `: expr` and `:` followed by an indented
  /// block. This record preserves which form the user wrote so later phases can
  /// keep the distinction when it matters.
  struct BodyResult {
    ast::ptr<ast::expr> inline_expr;           ///< Present for compact `: expr` bodies.
    std::vector<ast::ptr<ast::node>> stmts;    ///< Statement-form body payload.
    bool is_block = false;                     ///< Distinguishes original source form.
  };
  /// @brief Parses either a compact expression body or an indented statement body.
  ///
  /// @param construct_name User-facing construct name for diagnostics.
  auto parse_body(std::string_view construct_name) -> BodyResult;
  /// @brief Converts a `BodyResult` into a statement list for AST storage.
  ///
  /// Inline expression bodies are wrapped in `expr_stmt` nodes so later phases
  /// can consume a uniform statement list when needed.
  [[nodiscard]] std::vector<ast::ptr<ast::node>>
  body_to_stmt_list(BodyResult body);

  // ========================================================================
  //  Comma-separated list helper
  // ========================================================================

  /// @brief Parses a delimited comma-separated list.
  ///
  /// @tparam T Element type returned by `parse_element`.
  /// @param open Opening delimiter token.
  /// @param close Closing delimiter token.
  /// @param construct_name User-facing construct name for diagnostics.
  /// @param parse_element Callback that parses one element if possible.
  template <typename T>
  std::vector<T>
  parse_delimited_list(token_kind open, token_kind close,
                       std::string_view construct_name,
                       std::function<std::optional<T>()> parse_element);

  /// @brief Parses a comma-separated list without surrounding delimiters.
  ///
  /// @tparam T Element type returned by `parse_element`.
  /// @param parse_element Callback that parses one element if possible.
  /// @param at_end Callback that reports when the surrounding production ends.
  template <typename T>
  std::vector<T>
  parse_comma_list(std::function<std::optional<T>()> parse_element,
                   std::function<bool()> at_end);

  // ========================================================================
  //  Visibility
  // ========================================================================

  /// @brief Parses an optional leading visibility modifier.
  ///
  /// Declarations normalize visibility early so later AST consumers can reason
  /// over one enum instead of token kinds.
  [[nodiscard]] auto parse_optional_visibility() -> ast::visibility;

  // ========================================================================
  //  Module paths — `a.b.c`
  // ========================================================================

  /// @brief Parses a dotted module path into owned path segments.
  [[nodiscard]] std::vector<std::string> parse_module_path();

  // ========================================================================
  //  Top-level items
  // ========================================================================

  /// @brief Parses one top-level item or recovery placeholder.
  [[nodiscard]] ast::ptr<ast::node> parse_top_level_item();

  // ---- Module / use / dep ----

  /// Parses a file-level `module` declaration.
  [[nodiscard]] ast::ptr<ast::module_decl> parse_module_decl();
  /// Parses a `use` import with already-consumed visibility.
  [[nodiscard]] ast::ptr<ast::use_decl> parse_use_decl(ast::visibility vis);
  /// Parses a nested `module` item inside a file or module body.
  [[nodiscard]] ast::ptr<ast::sub_module_decl>
  parse_sub_module_decl(ast::visibility vis);
  /// Parses a dependency metadata declaration.
  [[nodiscard]] ast::ptr<ast::dep_decl> parse_dep_decl();

  // ---- Type declarations ----

  /// Parses a user-defined type declaration.
  [[nodiscard]] ast::ptr<ast::type_decl> parse_type_decl(ast::visibility vis);
  /// Parses the right-hand side of a type declaration after the name.
  [[nodiscard]] ast::ptr<ast::node> parse_type_def();
  /// Parses a struct-style field body for a type declaration.
  [[nodiscard]] auto parse_struct_body() -> ast::struct_body;
  /// Parses a sum-type variant body for a type declaration.
  [[nodiscard]] auto parse_sum_body() -> ast::sum_body;
  /// Parses one field inside a struct body.
  [[nodiscard]] auto parse_struct_field() -> ast::struct_field;
  /// Parses one variant inside a sum body.
  [[nodiscard]] auto parse_sum_variant() -> ast::sum_variant;

  // ---- Type expressions ----

  /// Parses any type expression accepted by the grammar.
  [[nodiscard]] ast::ptr<ast::type_expr> parse_type_expr();
  /// Parses a primary or atomic type expression before suffix handling.
  [[nodiscard]] ast::ptr<ast::type_expr> parse_prim_type_expr();
  /// Parses a named type path with optional generic/value arguments.
  [[nodiscard]] ast::ptr<ast::named_type> parse_named_type();
  /// Wraps a parsed bound list in a `bound_type` node for AST uniformity.
  [[nodiscard]] ast::ptr<ast::bound_type> make_bound_type(ast::bound bound);
  /// Parses an optional `: Type` annotation and returns null when absent.
  [[nodiscard]] ast::ptr<ast::type_expr> parse_optional_type_annotation();
  /// Parses tuple type syntax.
  [[nodiscard]] ast::ptr<ast::tuple_type> parse_tuple_type();
  /// Parses slice type syntax.
  [[nodiscard]] ast::ptr<ast::slice_type> parse_slice_type();
  /// Parses fixed-length array type syntax.
  [[nodiscard]] ast::ptr<ast::array_type> parse_array_type();
  /// Parses reference type syntax, including mutability.
  [[nodiscard]] ast::ptr<ast::ref_type> parse_ref_type();
  /// Parses raw pointer type syntax, including mutability.
  [[nodiscard]] ast::ptr<ast::ptr_type> parse_ptr_type();
  /// Parses function type syntax.
  [[nodiscard]] ast::ptr<ast::fn_type> parse_fn_type();

  // ---- Type parameters and bounds ----

  /// Parses a full type-parameter list.
  [[nodiscard]] std::vector<ast::type_param> parse_type_params();
  /// Parses one type or value parameter inside a parameter list.
  [[nodiscard]] auto parse_type_param() -> ast::type_param;
  /// Parses a `+`-separated bound list.
  [[nodiscard]] auto parse_bound() -> ast::bound;
  /// Parses one term inside a bound list.
  [[nodiscard]] auto parse_bound_term() -> ast::bound_term;
  /// Parses a trailing `where` clause into normalized constraints.
  [[nodiscard]] std::vector<ast::where_constraint> parse_where_clause();

  // ---- Trait / concept / impl declarations ----

  /// Parses a trait declaration body and signature.
  [[nodiscard]] ast::ptr<ast::trait_decl> parse_trait_decl(ast::visibility vis);
  /// Parses a concept declaration and its constraints.
  [[nodiscard]] ast::ptr<ast::concept_decl>
  parse_concept_decl(ast::visibility vis);
  /// Parses an `impl` block tying items to a concrete target type.
  [[nodiscard]] ast::ptr<ast::impl_decl> parse_impl_decl();

  // ---- Function declarations ----

  /// Parses a function declaration with modifiers already collected.
  [[nodiscard]] ast::ptr<ast::func_decl>
  parse_func_decl(ast::visibility vis, ast::func_modifiers mods,
                  bool allow_bodyless = false);
  /// Parses the leading modifier sequence for a function-like construct.
  [[nodiscard]] auto parse_func_modifiers() -> ast::func_modifiers;
  /// Parses a function parameter list.
  [[nodiscard]] std::vector<ast::Param> parse_param_list();
  /// Parses one function parameter, including defaults.
  [[nodiscard]] auto parse_param() -> ast::Param;
  /// Parses zero or more contract clauses attached to a function.
  [[nodiscard]] std::vector<ast::contract_clause> parse_contract_clauses();
  /// Parses a single `pre` or `post` contract clause.
  [[nodiscard]] auto parse_contract_clause() -> ast::contract_clause;

  // ---- Static declarations ----

  /// Parses a `static` declaration after visibility handling.
  [[nodiscard]] ast::ptr<ast::static_decl>
  parse_static_decl(ast::visibility vis);

  // ========================================================================
  //  Statements
  // ========================================================================

  /// Parses the next statement-level construct.
  [[nodiscard]] ast::ptr<ast::node> parse_stmt();
  /// Parses an immutable binding statement.
  [[nodiscard]] ast::ptr<ast::let_stmt> parse_let_stmt();
  /// Parses a mutable variable binding statement.
  [[nodiscard]] ast::ptr<ast::var_stmt> parse_var_stmt();
  /// Parses a `return` statement.
  [[nodiscard]] ast::ptr<ast::return_stmt> parse_return_stmt();
  /// Parses an `if` statement chain.
  [[nodiscard]] ast::ptr<ast::if_stmt> parse_if_stmt();
  /// Parses a `while` loop statement.
  [[nodiscard]] ast::ptr<ast::while_stmt> parse_while_stmt();
  /// Parses a `for` loop statement.
  [[nodiscard]] ast::ptr<ast::for_stmt> parse_for_stmt();
  /// Parses a `match` statement.
  [[nodiscard]] ast::ptr<ast::match_stmt> parse_match_stmt();
  /// Parses a `crew` statement.
  [[nodiscard]] ast::ptr<ast::crew_stmt> parse_crew_stmt();
  /// Parses an inline assembly statement.
  [[nodiscard]] ast::ptr<ast::asm_stmt> parse_asm_stmt();
  /// Parses a top-level or statement-position splice.
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

  /// Parses an expression from the lowest-precedence entry point.
  [[nodiscard]] ast::ptr<ast::expr> parse_expr();
  /// Parses pipe-precedence expressions.
  [[nodiscard]] ast::ptr<ast::expr> parse_pipe_expr();
  /// Parses logical `or` expressions.
  [[nodiscard]] ast::ptr<ast::expr> parse_or_expr();
  /// Parses logical `and` expressions.
  [[nodiscard]] ast::ptr<ast::expr> parse_and_expr();
  /// Parses unary `not` expressions.
  [[nodiscard]] ast::ptr<ast::expr> parse_not_expr();
  /// Parses comparison expressions.
  [[nodiscard]] ast::ptr<ast::expr> parse_cmp_expr();
  /// Parses additive expressions.
  [[nodiscard]] ast::ptr<ast::expr> parse_add_expr();
  /// Parses multiplicative expressions.
  [[nodiscard]] ast::ptr<ast::expr> parse_mul_expr();
  /// Parses prefix unary expressions.
  [[nodiscard]] ast::ptr<ast::expr> parse_unary_expr();
  /// Parses postfix expression chains such as calls and field access.
  [[nodiscard]] ast::ptr<ast::expr> parse_postfix_expr();
  /// Parses primary expression forms before suffix chaining.
  [[nodiscard]] ast::ptr<ast::expr> parse_primary_expr();

  // ---- Primary expression sub-parsers ----

  /// Parses a literal expression node.
  [[nodiscard]] ast::ptr<ast::expr> parse_literal_expr();
  /// Parses either a plain identifier or a dotted module/path expression.
  [[nodiscard]] ast::ptr<ast::expr> parse_ident_or_path_expr();
  /// Parses an `@`-prefixed variant constructor reference: `@name`, later
  /// applied to a call via ordinary postfix parsing for `@name(args)`.
  [[nodiscard]] ast::ptr<ast::expr> parse_variant_expr();
  /// Parses parenthesized expressions and tuple literals.
  [[nodiscard]] ast::ptr<ast::expr> parse_paren_expr();
  /// Parses bracket-based array or index-related expression forms.
  [[nodiscard]] ast::ptr<ast::expr> parse_bracket_expr();
  /// Parses brace-based struct or block-like expression forms.
  [[nodiscard]] ast::ptr<ast::expr> parse_brace_expr();
  /// Parses lambda expressions.
  [[nodiscard]] ast::ptr<ast::expr> parse_lambda_expr();
  /// Parses `match` in expression position.
  [[nodiscard]] ast::ptr<ast::match_expr> parse_match_expr();
  /// Parses `if` in expression position.
  [[nodiscard]] ast::ptr<ast::if_expr> parse_if_expr();
  /// Parses `for` comprehensions or generator-like expressions.
  [[nodiscard]] ast::ptr<ast::for_expr> parse_for_expr();
  /// Parses `await` expressions.
  [[nodiscard]] ast::ptr<ast::await_expr> parse_await_expr();
  /// Parses `async` expressions.
  [[nodiscard]] ast::ptr<ast::async_expr> parse_async_expr();
  /// Parses `par` expressions.
  [[nodiscard]] ast::ptr<ast::par_expr> parse_par_expr();
  /// Parses `race` expressions.
  [[nodiscard]] ast::ptr<ast::race_expr> parse_race_expr();
  /// Parses `crew` in expression position.
  [[nodiscard]] ast::ptr<ast::crew_expr> parse_crew_expr();
  /// Parses `on(...)` context expressions.
  [[nodiscard]] ast::ptr<ast::on_expr> parse_on_expr();
  /// Parses indentation-delimited block expressions.
  [[nodiscard]] ast::ptr<ast::block_expr> parse_block_expr();
  /// Parses quasi-quoted syntax expressions.
  [[nodiscard]] ast::ptr<ast::quote_expr> parse_quote_expr();
  /// Parses the inner operand of a splice expression.
  [[nodiscard]] ast::ptr<ast::splice_expr> parse_splice_expr_inner();
  /// Parses `static expr` metaprogramming forms.
  [[nodiscard]] ast::ptr<ast::static_expr> parse_static_expr();
  /// Parses a trailing `if` attached to an already-parsed expression.
  [[nodiscard]] ast::ptr<ast::expr> parse_trailing_if_expr(ast::ptr<ast::expr> then_expr);
  /// Parses a trailing `where` binding clause attached to an expression.
  [[nodiscard]] ast::ptr<ast::where_expr> parse_where_expr(ast::ptr<ast::expr> inner);

  // ---- Postfix suffixes ----

  /// Try to parse a postfix suffix (`.field`, `[index]`, `(args)`,
  /// `?`, `as Type`). Returns nullptr if the current token doesn't
  /// start a postfix suffix.
  [[nodiscard]] ast::ptr<ast::expr>
  parse_postfix_suffix(ast::ptr<ast::expr> base);

  // ---- Call arguments ----

  /// Parses a function or method call argument list.
  [[nodiscard]] std::vector<ast::call_arg> parse_call_args();

  // ---- Match arms ----

  /// Parses one `match` arm, including optional guards and body form.
  [[nodiscard]] auto parse_match_arm() -> ast::match_arm;

  // ========================================================================
  //  Patterns
  // ========================================================================

  /// Parses a full pattern from the lowest-precedence entry point.
  [[nodiscard]] ast::ptr<ast::pattern> parse_pattern();
  /// Parses `|`-separated alternative patterns.
  [[nodiscard]] ast::ptr<ast::pattern> parse_or_pattern();
  /// Parses a single non-alternating pattern form.
  [[nodiscard]] ast::ptr<ast::pattern> parse_atomic_pattern();
  // ---- Pattern sub-parsers ----

  /// Parses the wildcard `_` pattern.
  [[nodiscard]] ast::ptr<ast::pattern> parse_wildcard_pattern();
  /// Parses a literal pattern.
  [[nodiscard]] ast::ptr<ast::pattern> parse_literal_pattern();
  /// Parses a bare identifier as a binding pattern (or an `ident..ident` range).
  [[nodiscard]] ast::ptr<ast::pattern> parse_ident_or_constructor_pattern();
  /// Parses an `@`-prefixed variant constructor pattern: `@name`, `@name(...)`.
  [[nodiscard]] ast::ptr<ast::pattern> parse_variant_pattern();
  /// Parses parenthesized and tuple patterns.
  [[nodiscard]] ast::ptr<ast::pattern> parse_paren_pattern();
  /// Parses struct patterns.
  [[nodiscard]] ast::ptr<ast::pattern> parse_brace_pattern();
  /// Parses array and slice patterns.
  [[nodiscard]] ast::ptr<ast::pattern> parse_bracket_pattern();
  /// Parses `@some(...)`, `@ok(...)`, and `@err(...)` wrapper patterns.
  [[nodiscard]] ast::ptr<ast::pattern> parse_option_result_pattern();
  /// Parses reference patterns.
  [[nodiscard]] ast::ptr<ast::pattern> parse_ref_pattern();
  /// Heuristic for deciding whether a bare name should parse as a constructor.
  [[nodiscard]] auto is_constructor_like_name(std::string_view name) const -> bool;

  // ========================================================================
  //  For-loop variable parsing (shared by for-stmt and for-expr)
  // ========================================================================

  /// Parses the variable/pattern list shared by `for` statements and expressions.
  [[nodiscard]] std::vector<ast::ptr<ast::pattern>> parse_for_vars();

  // ========================================================================
  //  Helper: convert an assign-op token to the AssignOp enum.
  // ========================================================================

  [[nodiscard]] static std::optional<ast::assign_op>
  token_to_assign_op(token_kind kind) noexcept;

  // ========================================================================
  //  Helper: convert comparison token to BinaryOp.
  // ========================================================================

  [[nodiscard]] std::optional<ast::binary_op> token_to_cmp_op();
  [[nodiscard]] static std::optional<ast::binary_op>
  token_to_add_op(token_kind kind) noexcept;
  [[nodiscard]] static std::optional<ast::binary_op>
  token_to_mul_op(token_kind kind) noexcept;

  // ========================================================================
  //  Member variables
  // ========================================================================

  /// The token stream, ending with Eof.
  std::vector<token> tokens_;

  /// The source file identifier for diagnostics.
  file_id_type file_id_;

  /// The diagnostic bag we report errors to.
  diagnostic_bag &diag_;

  /// Current position in the token stream.
  uint32_t pos_{0};

  /// Some contexts, like guarded `for` expressions, need `=>` to act as a
  /// delimiter rather than starting a lambda.
  bool allow_lambda_expr_{true}; ///< Context flag that suppresses lambda parsing when `=>` is a delimiter.

  /// Some contexts, like `for ... in expr if guard`, need `if` to delimit the
  /// surrounding construct rather than start a trailing conditional expression.
  bool allow_trailing_if_expr_{true}; ///< Context flag that keeps `if` from stealing outer grammar roles.

  /// Compact statement termination is used when newline tokens are suppressed,
  /// such as block expressions nested inside parentheses.
  bool allow_compact_stmt_terminator_{false}; ///< Treats non-newline delimiters as statement terminators in compact contexts.
};

} // namespace kira
