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
  [[nodiscard]] auto has_value() const noexcept -> bool {
    return node != nullptr;
  }

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
  auto take() -> ast::ptr<T> { return std::move(node); }
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
  [[nodiscard]] auto parse_file() -> ast::ptr<ast::file>;

  // ========================================================================
  //  Accessors
  // ========================================================================

  /// @brief Returns whether parsing emitted any error diagnostics.
  [[nodiscard]] auto has_errors() const noexcept -> bool {
    return diag_.has_errors();
  }

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
  [[nodiscard]] auto peek() const noexcept -> const token & {
    return tokens_[pos_];
  }

  /// @brief Returns the token `offset` positions ahead without consuming it.
  ///
  /// Lookahead is cheap because the lexer produced the full token vector up
  /// front. Callers should still treat the result as read-only grammar context.
  ///
  /// @param offset Relative lookahead distance from the current token.
  [[nodiscard]] auto peek_at(uint32_t offset) const noexcept -> const token & {
    auto idx = static_cast<size_t>(pos_) + offset;
    if (idx >= tokens_.size()) {
      idx = tokens_.size() - 1; // clamp to Eof
    }
    return tokens_[idx];
  }

  /// @brief Returns the kind of the current token.
  [[nodiscard]] auto current() const noexcept -> token_kind {
    return peek().kind;
  }

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
  [[nodiscard]] auto at_eof() const noexcept -> bool {
    return at(token_kind::eof);
  }

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
  [[nodiscard]] auto previous_span() const noexcept -> source_span {
    if (pos_ == 0) {
      return source_span::dummy();
    }
    return tokens_[pos_ - 1].span;
  }

  /// @brief Returns the most recently consumed token.
  [[nodiscard]] auto previous() const noexcept -> const token & {
    if (pos_ == 0) {
      return tokens_[0];
    }
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
  /// Many productions allow optional blank lines between constructs.
  /// Centralizing that policy keeps individual grammar functions simpler and
  /// more consistent.
  void skip_newlines() noexcept {
    while (at(token_kind::newline)) {
      advance();
    }
  }

  /// @brief Consumes consecutive `#:` doc-comment tokens and joins their
  /// bodies.
  ///
  /// Doc comments never emit NEWLINE tokens (see the lexer), so a run of `#:`
  /// lines produces adjacent `doc_comment` tokens here. Returns the joined text
  /// (one `\n` between lines), or an empty string when the cursor isn't on a
  /// doc comment.
  [[nodiscard]] auto collect_doc_comments() -> std::string {
    std::string doc;
    while (at(token_kind::doc_comment)) {
      if (!doc.empty()) {
        doc.push_back('\n');
      }
      doc.append(peek().text);
      advance();
    }
    return doc;
  }

  /// @brief Attaches a collected doc comment to the item just pushed onto a
  /// member list, if the list grew and the new item is non-null.
  ///
  /// Member loops (`trait`/`impl`/`extend`/`signature`) push through several
  /// branches; this centralizes "hang the doc on whatever we just parsed"
  /// without threading the string into each branch.
  ///
  /// @param items The member list a branch may have appended to.
  /// @param before Its size captured before the branch ran.
  /// @param doc The collected doc comment (moved from).
  static void attach_member_doc(std::vector<ast::ptr<ast::node>> &items,
                                size_t before, std::string doc) {
    if (doc.empty()) {
      return;
    }
    if (items.size() > before && items.back() != nullptr) {
      items.back()->documentation = std::move(doc);
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

  /// @brief Requires a specific token kind and performs local recovery if
  /// absent.
  ///
  /// This is the parser's main "missing token" mechanism. Callers get back a
  /// real token when possible or a synthesized placeholder when recovery
  /// inserts the expected punctuation to preserve tree shape.
  ///
  /// @param expected Token kind required by the current grammar production.
  auto expect(token_kind expected) -> token;

  /// @brief Like `expect`, but with extra user-facing context in diagnostics.
  ///
  /// @param expected Token kind required by the current grammar production.
  /// @param context Explanation of the surrounding construct.
  auto expect_with_context(token_kind expected, std::string_view context)
      -> token;

  /// @brief Emits a friendly unexpected-token diagnostic at the current
  /// position.
  ///
  /// @param expected_description User-facing description of the missing syntax.
  void emit_unexpected(std::string_view expected_description);

  /// @brief Forwards a fully constructed diagnostic into the shared bag.
  void emit(const diagnostic &diag) { diag_.emit(diag); }

  /// @brief Creates an error diagnostic anchored to the current file.
  ///
  /// @param message Primary user-facing message.
  [[nodiscard]] auto make_error(std::string message) const -> diagnostic {
    return {diagnostic_level::error, std::move(message), file_id_};
  }

  /// @brief Creates an error diagnostic already labeled at `span`.
  ///
  /// @param span Source range to highlight.
  /// @param message Primary user-facing message.
  [[nodiscard]] auto make_error_at(source_span span, std::string message) const
      -> diagnostic {
    return diagnostic(diagnostic_level::error, std::move(message), file_id_)
        .with_label(span, "here");
  }

  /// @brief Skips ahead to a token that plausibly starts a fresh construct.
  ///
  /// This is the parser's coarse panic-mode recovery boundary for file and
  /// block parsing.
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
  auto parse_block(std::string_view construct_name,
                   const std::function<ast::ptr<T>()> &parse_item)
      -> std::vector<ast::ptr<T>>;

  /// @brief Normalized representation of either inline or block bodies.
  ///
  /// Many constructs in Kira allow both `: expr` and `:` followed by an
  /// indented block. This record preserves which form the user wrote so later
  /// phases can keep the distinction when it matters.
  struct body_result {
    ast::ptr<ast::expr> inline_expr; ///< Present for compact `: expr` bodies.
    std::vector<ast::ptr<ast::node>> stmts; ///< Statement-form body payload.
    bool is_block = false; ///< Distinguishes original source form.
  };
  /// @brief Parses either a compact expression body or an indented statement
  /// body.
  ///
  /// @param construct_name User-facing construct name for diagnostics.
  auto parse_body(std::string_view construct_name) -> body_result;
  /// @brief Converts a `BodyResult` into a statement list for AST storage.
  ///
  /// Inline expression bodies are wrapped in `expr_stmt` nodes so later phases
  /// can consume a uniform statement list when needed.
  [[nodiscard]] auto body_to_stmt_list(body_result body)
      -> std::vector<ast::ptr<ast::node>>;

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
  auto parse_delimited_list(token_kind open, token_kind close,
                            std::string_view construct_name,
                            std::function<std::optional<T>()> parse_element)
      -> std::vector<T>;

  /// @brief Parses a comma-separated list without surrounding delimiters.
  ///
  /// @tparam T Element type returned by `parse_element`.
  /// @param parse_element Callback that parses one element if possible.
  /// @param at_end Callback that reports when the surrounding production ends.
  template <typename T>
  auto parse_comma_list(std::function<std::optional<T>()> parse_element,
                        const std::function<bool()> &at_end) -> std::vector<T>;

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
  [[nodiscard]] auto parse_module_path() -> std::vector<std::string>;

  // ========================================================================
  //  Top-level items
  // ========================================================================

  /// @brief Parses one top-level item or recovery placeholder.
  ///
  /// @param allow_script_stmts When true (only for the top level of a
  /// `module main` script file), non-declaration tokens parse as ordinary
  /// statements instead of erroring; `parse_file` later gathers them into a
  /// synthesized `main` function.
  [[nodiscard]] auto parse_top_level_item(bool allow_script_stmts = false)
      -> ast::ptr<ast::node>;

  /// @brief Dispatches on the leading token to parse one top-level declaration.
  ///
  /// `parse_top_level_item` wraps this to first capture any preceding `#:` doc
  /// comment and attach it to the returned node; keeping the dispatch separate
  /// leaves the many `return parse_X(...)` branches untouched.
  [[nodiscard]] auto parse_top_level_item_dispatch(bool allow_script_stmts)
      -> ast::ptr<ast::node>;

  /// @brief Gathers a `module main` script file's top-level statements into a
  /// synthesized `def main` appended to `file.items`.
  ///
  /// Emits an error (and drops the statements) when the file also declares an
  /// explicit `main`, since the spec says a file may use one form or the
  /// other, but not both.
  void synthesize_script_main(ast::file &file);

  // ---- Module / use / dep ----

  /// Parses a file-level `module` declaration.
  [[nodiscard]] auto parse_module_decl() -> ast::ptr<ast::module_decl>;
  /// Parses a `use` import with already-consumed visibility.
  [[nodiscard]] auto parse_use_decl(ast::visibility vis)
      -> ast::ptr<ast::use_decl>;
  /// Parses a nested `module` item inside a file or module body.
  [[nodiscard]] auto parse_sub_module_decl(ast::visibility vis)
      -> ast::ptr<ast::sub_module_decl>;
  /// Parses a dependency metadata declaration.
  [[nodiscard]] auto parse_dep_decl() -> ast::ptr<ast::dep_decl>;

  // ---- Type declarations ----

  /// Parses a user-defined type declaration. `mods` carries any leading
  /// type-modifier keywords (`packed`) already consumed by the caller —
  /// mirrors `parse_func_decl` taking already-parsed `func_modifiers`.
  [[nodiscard]] auto parse_type_decl(ast::visibility vis,
                                     ast::type_modifiers mods = {})
      -> ast::ptr<ast::type_decl>;
  /// Parses a run of leading `type`-declaration modifier keywords
  /// (currently just `packed`) before a `type` keyword — mirrors
  /// `parse_func_modifiers`.
  [[nodiscard]] auto parse_type_modifiers() -> ast::type_modifiers;
  /// Parses the right-hand side of a type declaration after the name.
  [[nodiscard]] auto parse_type_def() -> ast::ptr<ast::node>;
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
  [[nodiscard]] auto parse_type_expr() -> ast::ptr<ast::type_expr>;
  /// Parses a primary or atomic type expression before suffix handling.
  [[nodiscard]] auto parse_prim_type_expr() -> ast::ptr<ast::type_expr>;
  /// Parses a named type path with optional generic/value arguments.
  [[nodiscard]] auto parse_named_type() -> ast::ptr<ast::named_type>;
  /// Parses an optional `[T, U, ...]` suffix directly onto an
  /// already-path-populated `named_type` — see its doc comment in
  /// parser.cpp.
  auto parse_generic_args_suffix(ast::named_type &named) -> void;
  /// Parses a bracketed `[T, U, ...]` type/value argument list (the current
  /// token must be `[`). Shared by generic-type application and functor
  /// instantiation in `use`.
  [[nodiscard]] auto parse_type_arg_list() -> std::vector<ast::type_arg>;
  /// Wraps a parsed bound list in a `bound_type` node for AST uniformity.
  [[nodiscard]] auto make_bound_type(ast::bound bound)
      -> ast::ptr<ast::bound_type>;
  /// Parses an optional `: Type` annotation and returns null when absent.
  [[nodiscard]] auto parse_optional_type_annotation()
      -> ast::ptr<ast::type_expr>;
  /// Parses tuple type syntax.
  [[nodiscard]] auto parse_tuple_type() -> ast::ptr<ast::tuple_type>;
  /// Parses slice type syntax.
  [[nodiscard]] auto parse_slice_type() -> ast::ptr<ast::slice_type>;
  /// Parses fixed-length array type syntax.
  [[nodiscard]] auto parse_array_type() -> ast::ptr<ast::array_type>;
  /// Parses reference type syntax, including mutability.
  [[nodiscard]] auto parse_ref_type() -> ast::ptr<ast::ref_type>;
  /// Parses raw pointer type syntax, including mutability.
  [[nodiscard]] auto parse_ptr_type() -> ast::ptr<ast::ptr_type>;
  /// Parses function type syntax.
  [[nodiscard]] auto parse_fn_type() -> ast::ptr<ast::fn_type>;

  // ---- Type parameters and bounds ----

  /// Parses a full type-parameter list.
  [[nodiscard]] auto parse_type_params() -> std::vector<ast::type_param>;
  /// Parses one type or value parameter inside a parameter list.
  [[nodiscard]] auto parse_type_param() -> ast::type_param;
  /// Parses a `+`-separated bound list.
  [[nodiscard]] auto parse_bound() -> ast::bound;
  /// Parses one term inside a bound list.
  [[nodiscard]] auto parse_bound_term() -> ast::bound_term;
  /// Parses a trailing `where` clause into normalized constraints.
  [[nodiscard]] auto parse_where_clause() -> std::vector<ast::where_constraint>;

  // ---- Trait / concept / impl declarations ----

  /// Parses a trait declaration body and signature.
  [[nodiscard]] auto parse_trait_decl(ast::visibility vis)
      -> ast::ptr<ast::trait_decl>;
  /// Parses a module `signature` declaration and its member requirements.
  [[nodiscard]] auto parse_signature_decl(ast::visibility vis)
      -> ast::ptr<ast::signature_decl>;
  /// Parses a concept declaration and its constraints.
  [[nodiscard]] auto parse_concept_decl(ast::visibility vis)
      -> ast::ptr<ast::concept_decl>;
  /// Parses an `impl` block tying items to a concrete target type.
  [[nodiscard]] auto parse_impl_decl() -> ast::ptr<ast::impl_decl>;
  /// Parses an `extend` block adding methods to a concrete target type.
  [[nodiscard]] auto parse_extend_decl() -> ast::ptr<ast::extend_decl>;

  // ---- Function declarations ----

  /// Parses a function declaration with modifiers already collected.
  [[nodiscard]] auto parse_func_decl(ast::visibility vis,
                                     ast::func_modifiers mods,
                                     bool allow_bodyless = false)
      -> ast::ptr<ast::func_decl>;
  /// Parses the leading modifier sequence for a function-like construct.
  [[nodiscard]] auto parse_func_modifiers() -> ast::func_modifiers;

  /// Whether the `static` at the cursor opens a *function* (`static def`,
  /// `static pure def`, ...) rather than a static binding
  /// (`static NAME: Type`).
  ///
  /// `static` is the one function modifier that is also a declaration keyword
  /// in its own right, so every construct that accepts both has to look past
  /// the run of modifiers for a `def` before committing. Two call sites used
  /// to unroll that by hand to a fixed depth of three tokens; sharing one
  /// scan is what lets `trait`, `impl`, and `extend` bodies accept
  /// `static def` on the same terms as module scope, which is what
  /// `kira-grammar.ebnf` describes (`static` is listed there as an
  /// unrestricted `func_modifier`).
  [[nodiscard]] auto at_static_func_decl() const noexcept -> bool;
  /// True when the balanced `[...]` group at the cursor is immediately
  /// followed by `(`.
  ///
  /// This is how `obj.field[...]` is split between explicit generic method
  /// arguments and indexing a field. Both can hold a bare name, so the
  /// bracket contents cannot decide it; a following call can, because
  /// generic arguments name the method that is about to be called.
  [[nodiscard]] auto bracket_group_precedes_call() const noexcept -> bool;
  /// Parses a function parameter list.
  [[nodiscard]] auto parse_param_list() -> std::vector<ast::param>;
  /// Parses one function parameter, including defaults.
  [[nodiscard]] auto parse_param() -> ast::param;
  /// Parses zero or more contract clauses attached to a function.
  [[nodiscard]] auto parse_contract_clauses()
      -> std::vector<ast::contract_clause>;
  /// Parses a single `pre` or `post` contract clause.
  [[nodiscard]] auto parse_contract_clause() -> ast::contract_clause;

  // ---- Static declarations ----

  /// Parses a `static` declaration after visibility handling.
  [[nodiscard]] auto parse_static_decl(ast::visibility vis)
      -> ast::ptr<ast::static_decl>;

  // ========================================================================
  //  Statements
  // ========================================================================

  /// Parses the next statement-level construct.
  [[nodiscard]] auto parse_stmt() -> ast::ptr<ast::node>;
  /// Parses an immutable binding statement.
  [[nodiscard]] auto parse_let_stmt() -> ast::ptr<ast::let_stmt>;
  /// Parses a mutable variable binding statement.
  [[nodiscard]] auto parse_var_stmt() -> ast::ptr<ast::var_stmt>;
  /// Parses a `return` statement.
  [[nodiscard]] auto parse_return_stmt() -> ast::ptr<ast::return_stmt>;
  /// Parses an `if` statement chain.
  [[nodiscard]] auto parse_if_stmt() -> ast::ptr<ast::if_stmt>;
  /// Parses a `while` loop statement.
  [[nodiscard]] auto parse_while_stmt() -> ast::ptr<ast::while_stmt>;
  /// Parses a `for` loop statement.
  [[nodiscard]] auto parse_for_stmt() -> ast::ptr<ast::for_stmt>;
  /// Parses a `match` statement.
  [[nodiscard]] auto parse_match_stmt() -> ast::ptr<ast::match_stmt>;
  /// Parses a `crew` statement.
  [[nodiscard]] auto parse_crew_stmt() -> ast::ptr<ast::crew_stmt>;
  /// Parses an inline assembly statement.
  [[nodiscard]] auto parse_asm_stmt() -> ast::ptr<ast::asm_stmt>;
  /// Parses a top-level or statement-position splice.
  [[nodiscard]] auto parse_splice_stmt() -> ast::ptr<ast::splice_stmt>;

  /// Parse an expression statement or assignment statement.
  /// We parse the LHS as an expression, then check if it's followed
  /// by an assignment operator.
  [[nodiscard]] auto parse_expr_or_assign_stmt() -> ast::ptr<ast::node>;

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
  [[nodiscard]] auto parse_expr() -> ast::ptr<ast::expr>;
  /// Parses pipe-precedence expressions.
  [[nodiscard]] auto parse_pipe_expr() -> ast::ptr<ast::expr>;
  /// Parses logical `or` expressions.
  [[nodiscard]] auto parse_or_expr() -> ast::ptr<ast::expr>;
  /// Parses logical `and` expressions.
  [[nodiscard]] auto parse_and_expr() -> ast::ptr<ast::expr>;
  /// Parses unary `not` expressions.
  [[nodiscard]] auto parse_not_expr() -> ast::ptr<ast::expr>;
  /// Parses comparison expressions.
  [[nodiscard]] auto parse_cmp_expr() -> ast::ptr<ast::expr>;
  /// Parses additive expressions.
  [[nodiscard]] auto parse_add_expr() -> ast::ptr<ast::expr>;
  /// Parses multiplicative expressions.
  [[nodiscard]] auto parse_mul_expr() -> ast::ptr<ast::expr>;
  /// Parses prefix unary expressions.
  [[nodiscard]] auto parse_unary_expr() -> ast::ptr<ast::expr>;
  /// Parses postfix expression chains such as calls and field access.
  [[nodiscard]] auto parse_postfix_expr() -> ast::ptr<ast::expr>;
  /// Parses primary expression forms before suffix chaining.
  [[nodiscard]] auto parse_primary_expr() -> ast::ptr<ast::expr>;

  // ---- Primary expression sub-parsers ----

  /// Parses a literal expression node.
  [[nodiscard]] auto parse_literal_expr() -> ast::ptr<ast::expr>;
  /// Parses a `string_lit` token, splitting it into an
  /// `interpolated_string_expr` when it contains `{...}` interpolation, or a
  /// plain `literal_expr` otherwise (`spec/string-formatting-design.md`).
  [[nodiscard]] auto parse_string_literal_expr() -> ast::ptr<ast::expr>;
  /// Parses a byte range of source text (usually extracted from inside a
  /// string literal's `{...}`) as a standalone expression, by re-lexing it
  /// with the given absolute file offset so token spans stay correct.
  ///
  /// @param text Raw source text to parse (no surrounding braces/quotes).
  /// @param absolute_offset File offset `text[0]` corresponds to.
  [[nodiscard]] auto parse_sub_expr(std::string_view text,
                                    byte_offset absolute_offset)
      -> ast::ptr<ast::expr>;
  /// Parses one `format_spec` mini-grammar's text (the part after `:` in
  /// `{expr :spec}`), including any dynamic `{expr}` width/precision.
  ///
  /// @param text Raw format-spec source text.
  /// @param absolute_offset File offset `text[0]` corresponds to.
  /// @param whole_span Source span covering the whole spec, for diagnostics.
  [[nodiscard]] auto parse_format_spec_text(std::string_view text,
                                            byte_offset absolute_offset,
                                            source_span whole_span)
      -> ast::format_spec;
  /// Parses either a plain identifier or a dotted module/path expression.
  [[nodiscard]] auto parse_ident_or_path_expr() -> ast::ptr<ast::expr>;
  /// Parses an `@`-prefixed variant constructor reference: `@name`, later
  /// applied to a call via ordinary postfix parsing for `@name(args)`.
  [[nodiscard]] auto parse_variant_expr() -> ast::ptr<ast::expr>;
  /// Parses parenthesized expressions and tuple literals.
  [[nodiscard]] auto parse_paren_expr() -> ast::ptr<ast::expr>;
  /// Parses bracket-based array or index-related expression forms.
  [[nodiscard]] auto parse_bracket_expr() -> ast::ptr<ast::expr>;
  /// Parses brace-based struct or block-like expression forms.
  [[nodiscard]] auto parse_brace_expr() -> ast::ptr<ast::expr>;
  /// Parses lambda expressions.
  [[nodiscard]] auto parse_lambda_expr() -> ast::ptr<ast::expr>;
  /// Returns whether the current `(` opens a lambda parameter list rather
  /// than a tuple or a grouped expression.
  [[nodiscard]] auto at_lambda_param_list() const noexcept -> bool;
  /// Parses `match` in expression position.
  [[nodiscard]] auto parse_match_expr() -> ast::ptr<ast::match_expr>;
  /// Parses `if` in expression position.
  [[nodiscard]] auto parse_if_expr() -> ast::ptr<ast::if_expr>;
  /// Parses `for` comprehensions or generator-like expressions.
  [[nodiscard]] auto parse_for_expr() -> ast::ptr<ast::for_expr>;
  /// Parses `await` expressions.
  [[nodiscard]] auto parse_await_expr() -> ast::ptr<ast::await_expr>;
  /// Parses value-carrying `yield` expressions (generator suspension
  /// points).
  [[nodiscard]] auto parse_yield_expr() -> ast::ptr<ast::yield_expr>;
  /// Parses `async` expressions.
  [[nodiscard]] auto parse_async_expr() -> ast::ptr<ast::async_expr>;
  /// Parses `par` expressions.
  [[nodiscard]] auto parse_par_expr() -> ast::ptr<ast::par_expr>;
  /// Parses `race` expressions.
  [[nodiscard]] auto parse_race_expr() -> ast::ptr<ast::race_expr>;
  /// Parses `crew` in expression position.
  [[nodiscard]] auto parse_crew_expr() -> ast::ptr<ast::crew_expr>;
  /// Parses `on(...)` context expressions.
  [[nodiscard]] auto parse_on_expr() -> ast::ptr<ast::on_expr>;
  /// Parses indentation-delimited block expressions.
  [[nodiscard]] auto parse_block_expr() -> ast::ptr<ast::block_expr>;
  /// Parses quasi-quoted syntax expressions.
  [[nodiscard]] auto parse_quote_expr() -> ast::ptr<ast::quote_expr>;
  /// Re-parses a quote's already-captured raw `tokens` into a structured
  /// subtree, populating `qexpr.parsed_body`/`fragment_kind`. Runs a fresh,
  /// nested `parser` instance over the captured tokens (sharing this
  /// parser's `diag_` bag and `file_id_`) rather than trying to splice the
  /// content back into the outer token stream, so recursive-descent
  /// functions don't need to know they're re-entering already-consumed
  /// input.
  void classify_and_parse_quote_fragment(ast::quote_expr &qexpr);
  /// Parses the inner operand of a splice expression.
  [[nodiscard]] auto parse_splice_expr_inner() -> ast::ptr<ast::splice_expr>;
  /// Parses `static expr` metaprogramming forms.
  [[nodiscard]] auto parse_static_expr() -> ast::ptr<ast::static_expr>;
  /// Parses a trailing `if` attached to an already-parsed expression.
  [[nodiscard]] auto parse_trailing_if_expr(ast::ptr<ast::expr> then_expr)
      -> ast::ptr<ast::expr>;
  /// Parses a trailing `where` binding clause attached to an expression.
  [[nodiscard]] auto parse_where_expr(ast::ptr<ast::expr> inner)
      -> ast::ptr<ast::where_expr>;

  // ---- Postfix suffixes ----

  /// Try to parse a postfix suffix (`.field`, `[index]`, `(args)`,
  /// `?`, `as Type`). Returns nullptr if the current token doesn't
  /// start a postfix suffix.
  [[nodiscard]] auto parse_postfix_suffix(ast::ptr<ast::expr> base)
      -> ast::ptr<ast::expr>;

  // ---- Call arguments ----

  /// Parses a function or method call argument list.
  [[nodiscard]] auto parse_call_args() -> std::vector<ast::call_arg>;

  // ---- Match arms ----

  /// Parses one `match` arm, including optional guards and body form.
  [[nodiscard]] auto parse_match_arm() -> ast::match_arm;

  // ========================================================================
  //  Patterns
  // ========================================================================

  /// Parses a full pattern from the lowest-precedence entry point.
  [[nodiscard]] auto parse_pattern() -> ast::ptr<ast::pattern>;
  /// Parses `|`-separated alternative patterns.
  [[nodiscard]] auto parse_or_pattern() -> ast::ptr<ast::pattern>;
  /// Parses a single non-alternating pattern form.
  [[nodiscard]] auto parse_atomic_pattern() -> ast::ptr<ast::pattern>;
  // ---- Pattern sub-parsers ----

  /// Parses the wildcard `_` pattern.
  [[nodiscard]] auto parse_wildcard_pattern() -> ast::ptr<ast::pattern>;
  /// Parses a literal pattern.
  [[nodiscard]] auto parse_literal_pattern() -> ast::ptr<ast::pattern>;
  /// Parses a bare identifier as a binding pattern (or an `ident..ident`
  /// range).
  [[nodiscard]] auto parse_ident_or_constructor_pattern()
      -> ast::ptr<ast::pattern>;
  /// Parses a `mut`-prefixed binding pattern: `mut name`.
  [[nodiscard]] auto parse_mut_binding_pattern() -> ast::ptr<ast::pattern>;
  /// Parses an `@`-prefixed variant constructor pattern: `@name`, `@name(...)`.
  [[nodiscard]] auto parse_variant_pattern() -> ast::ptr<ast::pattern>;
  /// Parses parenthesized and tuple patterns.
  [[nodiscard]] auto parse_paren_pattern() -> ast::ptr<ast::pattern>;
  /// Parses struct patterns.
  [[nodiscard]] auto parse_brace_pattern() -> ast::ptr<ast::pattern>;
  /// Parses array and slice patterns.
  [[nodiscard]] auto parse_bracket_pattern() -> ast::ptr<ast::pattern>;
  /// Parses `@some(...)`, `@ok(...)`, and `@err(...)` wrapper patterns.
  [[nodiscard]] auto parse_option_result_pattern() -> ast::ptr<ast::pattern>;
  /// Parses reference patterns.
  [[nodiscard]] auto parse_ref_pattern() -> ast::ptr<ast::pattern>;
  /// Heuristic for deciding whether a bare name should parse as a constructor.
  [[nodiscard]] auto is_constructor_like_name(std::string_view name) const
      -> bool;

  // ========================================================================
  //  For-loop variable parsing (shared by for-stmt and for-expr)
  // ========================================================================

  /// Parses the variable/pattern list shared by `for` statements and
  /// expressions.
  [[nodiscard]] auto parse_for_vars() -> std::vector<ast::ptr<ast::pattern>>;

  // ========================================================================
  //  Helper: convert an assign-op token to the AssignOp enum.
  // ========================================================================

  [[nodiscard]] static auto token_to_assign_op(token_kind kind) noexcept
      -> std::optional<ast::assign_op>;

  // ========================================================================
  //  Helper: convert comparison token to BinaryOp.
  // ========================================================================

  [[nodiscard]] auto token_to_cmp_op() -> std::optional<ast::binary_op>;
  [[nodiscard]] static auto token_to_add_op(token_kind kind) noexcept
      -> std::optional<ast::binary_op>;
  [[nodiscard]] static auto token_to_mul_op(token_kind kind) noexcept
      -> std::optional<ast::binary_op>;

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
  bool allow_lambda_expr_{true}; ///< Context flag that suppresses lambda
                                 ///< parsing when `=>` is a delimiter.

  /// Inside a `pre`/`post` condition (`parse_contract_clause`), the keyword
  /// `return` names the returned value rather than starting a return
  /// statement — see `parse_primary_expr`.
  bool contract_return_allowed_{false};

  /// Some contexts, like `for ... in expr if guard`, need `if` to delimit the
  /// surrounding construct rather than start a trailing conditional expression.
  bool allow_trailing_if_expr_{true}; ///< Context flag that keeps `if` from
                                      ///< stealing outer grammar roles.

  /// Compact statement termination is used when newline tokens are suppressed,
  /// such as block expressions nested inside parentheses.
  bool allow_compact_stmt_terminator_{
      false}; ///< Treats non-newline delimiters as statement terminators in
              ///< compact contexts.
};

} // namespace kira
