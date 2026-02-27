#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "source_location.h"
#include "token.h"
#include "diagnostic.h"

namespace kira {

// ==========================================================================
//  Lexer — tokenizes Kira source code.
//
//  Key responsibilities:
//    - Produce a flat stream of tokens from UTF-8 source text.
//    - Track indentation levels and emit INDENT / DEDENT / NEWLINE tokens.
//    - Suppress NEWLINE inside balanced bracket pairs (parens, brackets,
//      braces, backtick quotes).
//    - Produce helpful error tokens with explanations when something goes
//      wrong — never just "unexpected character", always context about what
//      the lexer was trying to do and what it found instead.
//
//  Design notes:
//    - The lexer does NOT allocate AST nodes. It produces Token values that
//      carry string_view references into the original source buffer.
//    - All tokens are produced eagerly and stored in a vector. The parser
//      then walks this vector. This makes lookahead trivial and allows us
//      to do fixup passes (e.g., inserting missing DEDENT at EOF).
//    - We use `string_view` throughout — the source text must outlive
//      the Lexer and all tokens it produces.
// ==========================================================================
class Lexer {
 public:
  /// Construct a lexer for the given source text. `file_id` is the
  /// identifier for the source file, used in diagnostics. `diag` is the
  /// diagnostic bag to emit errors into.
  Lexer(std::string_view source, FileId file_id, diagnostic_bag& diag)
      : source_(source),
        file_id_(file_id),
        diag_(diag),
        pos_(0),
        indent_stack_{0} {}

  /// Tokenize the entire source and return the token stream.
  /// The returned vector always ends with an Eof token.
  [[nodiscard]] std::vector<Token> tokenize() {
    tokens_.clear();
    pos_ = 0;
    indent_stack_.clear();
    indent_stack_.push_back(0);
    bracket_depth_ = 0;
    at_line_start_ = true;

    while (pos_ < source_.size()) {
      scan_token();
    }

    // At end of file, emit DEDENT for every remaining indent level.
    emit_pending_dedents();

    // Always end with EOF.
    tokens_.push_back(Token{
        .kind = token_kind::eof,
        .span = Span{static_cast<byte_offset>(source_.size()),
                     static_cast<byte_offset>(source_.size())},
        .text = {},
        .error_message = {},
    });

    return std::move(tokens_);
  }

 private:
  // ==========================================================================
  //  Character inspection helpers
  // ==========================================================================

  [[nodiscard]] char peek() const noexcept {
    if (pos_ >= source_.size()) return '\0';
    return source_[pos_];
  }

  [[nodiscard]] char peek_at(uint32_t offset) const noexcept {
    auto idx = pos_ + offset;
    if (idx >= source_.size()) return '\0';
    return source_[idx];
  }

  [[nodiscard]] bool at_end() const noexcept {
    return pos_ >= source_.size();
  }

  char advance() noexcept {
    if (pos_ >= source_.size()) return '\0';
    return source_[pos_++];
  }

  bool match(char expected) noexcept {
    if (pos_ < source_.size() && source_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  [[nodiscard]] std::string_view text_from(byte_offset start) const noexcept {
    return source_.substr(start, pos_ - start);
  }

  [[nodiscard]] Span span_from(byte_offset start) const noexcept {
    return Span{start, static_cast<byte_offset>(pos_)};
  }

  [[nodiscard]] static bool is_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  }

  [[nodiscard]] static bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
  }

  [[nodiscard]] static bool is_hex_digit(char c) noexcept {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  }

  [[nodiscard]] static bool is_oct_digit(char c) noexcept {
    return c >= '0' && c <= '7';
  }

  [[nodiscard]] static bool is_bin_digit(char c) noexcept {
    return c == '0' || c == '1';
  }

  [[nodiscard]] static bool is_ident_start(char c) noexcept {
    return is_alpha(c) || c == '_';
  }

  [[nodiscard]] static bool is_ident_continue(char c) noexcept {
    return is_alpha(c) || is_digit(c) || c == '_';
  }

  [[nodiscard]] static bool is_whitespace_not_newline(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r';
  }

  // ==========================================================================
  //  Token emission helpers
  // ==========================================================================

  void emit(token_kind kind, byte_offset start) {
    tokens_.push_back(Token{
        .kind = kind,
        .span = span_from(start),
        .text = text_from(start),
        .error_message = {},
    });
  }

  void emit_synthetic(token_kind kind, Span span, std::string_view text = {}) {
    tokens_.push_back(Token{
        .kind = kind,
        .span = span,
        .text = text,
        .error_message = {},
    });
  }

  void emit_error(byte_offset start, std::string_view message) {
    auto sp = span_from(start);
    tokens_.push_back(Token{
        .kind = token_kind::error,
        .span = sp,
        .text = text_from(start),
        .error_message = message,
    });

    // Also report to the diagnostic bag with friendly context.
    diag_.error(file_id_, std::string(message), sp,
                "this is where the problem is");
  }

  void emit_pending_dedents() {
    auto eof_pos = static_cast<byte_offset>(source_.size());
    while (indent_stack_.size() > 1) {
      indent_stack_.pop_back();
      emit_synthetic(token_kind::dedent,
                     Span{eof_pos, eof_pos});
    }
  }

  // ==========================================================================
  //  Main scan dispatch
  // ==========================================================================

  void scan_token() {
    // At the start of a logical line, handle indentation.
    if (at_line_start_) {
      handle_line_start();
      return;
    }

    // Skip horizontal whitespace within a line.
    skip_horizontal_whitespace();
    if (at_end()) return;

    byte_offset start = static_cast<byte_offset>(pos_);
    char c = advance();

    switch (c) {
      // ---- Newlines ----
      case '\n':
        handle_newline(start);
        return;

      // ---- Comments ----
      case '#':
        skip_comment();
        // After a comment, we've consumed up to (but not including) the
        // newline. The newline itself will be handled next iteration.
        return;

      // ---- Single-character punctuation ----
      case '(':
        ++bracket_depth_;
        emit(token_kind::lparen, start);
        return;
      case ')':
        if (bracket_depth_ > 0) --bracket_depth_;
        emit(token_kind::rparen, start);
        return;
      case '[':
        ++bracket_depth_;
        emit(token_kind::lbracket, start);
        return;
      case ']':
        if (bracket_depth_ > 0) --bracket_depth_;
        emit(token_kind::rbracket, start);
        return;
      case '{':
        ++bracket_depth_;
        emit(token_kind::lbrace, start);
        return;
      case '}':
        if (bracket_depth_ > 0) --bracket_depth_;
        emit(token_kind::rbrace, start);
        return;
      case ',':
        emit(token_kind::comma, start);
        return;
      case ';':
        emit(token_kind::semicolon, start);
        return;
      case '@':
        emit(token_kind::at, start);
        return;
      case '?':
        emit(token_kind::question, start);
        return;
      case '`':
        // Backtick — quote region delimiter. We treat open/close the same
        // and let the parser track nesting.
        emit(token_kind::backtick, start);
        return;

      // ---- Colon ----
      case ':':
        emit(token_kind::colon, start);
        return;

      // ---- Dot ----
      case '.':
        if (match('.')) {
          if (match('=')) {
            emit(token_kind::dot_dot_eq, start);
          } else {
            emit(token_kind::dot_dot, start);
          }
        } else {
          emit(token_kind::dot, start);
        }
        return;

      // ---- Operators ----
      case '=':
        if (match('=')) {
          emit(token_kind::eq_eq, start);
        } else if (match('>')) {
          emit(token_kind::fat_arrow, start);
        } else {
          emit(token_kind::eq, start);
        }
        return;

      case '!':
        if (match('=')) {
          emit(token_kind::bang_eq, start);
        } else {
          // Bare '!' is not a Kira operator. Give a helpful message.
          emit_error(start,
              "unexpected `!` — Kira uses `not` for logical negation, "
              "not `!`");
        }
        return;

      case '+':
        if (match('%')) {
          if (match('=')) {
            emit(token_kind::plus_percent_eq, start);
          } else {
            emit(token_kind::plus_percent, start);
          }
        } else if (match('|')) {
          if (match('=')) {
            emit(token_kind::plus_pipe_eq, start);
          } else {
            emit(token_kind::plus_pipe, start);
          }
        } else if (match('=')) {
          emit(token_kind::plus_eq, start);
        } else {
          emit(token_kind::plus, start);
        }
        return;

      case '-':
        if (match('>')) {
          emit(token_kind::arrow, start);
        } else if (match('%')) {
          if (match('=')) {
            emit(token_kind::minus_percent_eq, start);
          } else {
            emit(token_kind::minus_percent, start);
          }
        } else if (match('|')) {
          if (match('=')) {
            emit(token_kind::minus_pipe_eq, start);
          } else {
            emit(token_kind::minus_pipe, start);
          }
        } else if (match('=')) {
          emit(token_kind::minus_eq, start);
        } else {
          emit(token_kind::minus, start);
        }
        return;

      case '*':
        if (match('%')) {
          if (match('=')) {
            emit(token_kind::star_percent_eq, start);
          } else {
            emit(token_kind::star_percent, start);
          }
        } else if (match('|')) {
          if (match('=')) {
            emit(token_kind::star_pipe_eq, start);
          } else {
            emit(token_kind::star_pipe, start);
          }
        } else if (match('=')) {
          emit(token_kind::star_eq, start);
        } else {
          emit(token_kind::star, start);
        }
        return;

      case '/':
        if (match('=')) {
          emit(token_kind::slash_eq, start);
        } else {
          emit(token_kind::slash, start);
        }
        return;

      case '%':
        if (match('=')) {
          emit(token_kind::percent_eq, start);
        } else {
          emit(token_kind::percent, start);
        }
        return;

      case '&':
        if (match('=')) {
          emit(token_kind::amp_eq, start);
        } else {
          emit(token_kind::amp, start);
        }
        return;

      case '|':
        if (match('=')) {
          emit(token_kind::pipe_eq, start);
        } else {
          emit(token_kind::pipe, start);
        }
        return;

      case '^':
        if (match('=')) {
          emit(token_kind::caret_eq, start);
        } else {
          emit(token_kind::caret, start);
        }
        return;

      case '~':
        emit(token_kind::tilde, start);
        return;

      case '<':
        if (match('<')) {
          if (match('=')) {
            emit(token_kind::lt_lt_eq, start);
          } else {
            emit(token_kind::lt_lt, start);
          }
        } else if (match('=')) {
          emit(token_kind::lt_eq, start);
        } else {
          emit(token_kind::lt, start);
        }
        return;

      case '>':
        if (match('>')) {
          if (match('=')) {
            emit(token_kind::gt_gt_eq, start);
          } else {
            emit(token_kind::gt_gt, start);
          }
        } else if (match('=')) {
          emit(token_kind::gt_eq, start);
        } else {
          emit(token_kind::gt, start);
        }
        return;

      // ---- String literals ----
      case '"':
        scan_string(start);
        return;

      // ---- Character literals ----
      case '\'':
        scan_char(start);
        return;

      // ---- Numbers ----
      case '0':
      case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
        scan_number(start);
        return;

      default:
        // ---- Identifiers and keywords ----
        if (is_ident_start(c)) {
          scan_identifier(start);
          return;
        }

        // If we reach here, it's a character we truly don't understand.
        // Give a helpful error based on what it looks like.
        if (static_cast<unsigned char>(c) > 127) {
          emit_error(start,
              "unexpected non-ASCII character — Kira source files must "
              "be UTF-8, and this byte doesn't appear to be the start "
              "of a valid token");
        } else {
          std::string msg = "unexpected character `";
          msg += c;
          msg += "` — this isn't recognized as part of Kira's syntax";
          emit_error(start, msg);
        }
        return;
    }
  }

  // ==========================================================================
  //  Indentation handling
  //
  //  At the start of each logical line, we measure the indentation and
  //  compare it to the current indent stack to emit INDENT / DEDENT tokens.
  //  Inside brackets, we suppress newline/indent handling entirely.
  // ==========================================================================

  void handle_line_start() {
    // Measure leading whitespace.
    uint32_t indent = 0;
    byte_offset line_begin = static_cast<byte_offset>(pos_);

    while (!at_end()) {
      char c = peek();
      if (c == ' ') {
        ++indent;
        advance();
      } else if (c == '\t') {
        // Tab = advance to next multiple of 4.
        indent = (indent + 4) & ~3u;
        advance();
      } else {
        break;
      }
    }

    // Blank line or comment-only line: skip entirely (no NEWLINE emitted).
    if (at_end() || peek() == '\n') {
      if (!at_end()) advance();  // consume the newline
      // Stay at_line_start_ = true for the next line.
      return;
    }
    if (peek() == '#') {
      skip_comment();
      // After the comment the newline (or EOF) will be handled by
      // the next scan_token iteration.
      return;
    }

    at_line_start_ = false;

    // Inside brackets, indentation is not significant.
    if (bracket_depth_ > 0) {
      return;
    }

    uint32_t current_indent = indent_stack_.back();

    if (indent > current_indent) {
      // Increased indentation — emit INDENT.
      indent_stack_.push_back(indent);
      emit_synthetic(token_kind::indent,
                     Span{line_begin, static_cast<byte_offset>(pos_)});
    } else if (indent < current_indent) {
      // Decreased indentation — emit one or more DEDENT tokens.
      while (indent_stack_.size() > 1 && indent < indent_stack_.back()) {
        indent_stack_.pop_back();
        emit_synthetic(token_kind::dedent,
                       Span{line_begin, static_cast<byte_offset>(pos_)});
      }

      // Check that the dedent lands on a known indent level.
      if (!indent_stack_.empty() && indent != indent_stack_.back()) {
        auto sp = Span{line_begin, static_cast<byte_offset>(pos_)};
        diag_.emit(
            diagnostic(diagnostic_level::error,
                       "inconsistent indentation — this line's indentation "
                       "doesn't match any previous indentation level",
                       file_id_)
                .with_label(sp, "this line has unexpected indentation")
                .with_help(
                    "Kira uses consistent indentation to define code blocks. "
                    "Make sure each line is indented by the same amount as "
                    "other lines at the same nesting level. Using spaces "
                    "(not tabs) is recommended."));
        // Recovery: treat it as if the indent matches the current level.
      }
    }
    // If indent == current_indent, no token needed — same level.
  }

  void handle_newline(byte_offset start) {
    // Inside brackets, newlines are suppressed.
    if (bracket_depth_ > 0) {
      // Don't emit anything — just continue.
      at_line_start_ = false;
      return;
    }

    // Emit a NEWLINE token and mark that the next thing is a line start.
    emit(token_kind::newline, start);
    at_line_start_ = true;
  }

  void skip_horizontal_whitespace() {
    while (!at_end() && is_whitespace_not_newline(peek())) {
      advance();
    }
  }

  void skip_comment() {
    // We're already past the '#'. Consume everything until newline or EOF.
    while (!at_end() && peek() != '\n') {
      advance();
    }
    // Don't consume the newline — let it be handled as a regular newline.
  }

  // ==========================================================================
  //  Identifier / keyword scanning
  // ==========================================================================

  void scan_identifier(byte_offset start) {
    while (!at_end() && is_ident_continue(peek())) {
      advance();
    }

    auto text = text_from(start);
    auto kind = classify_ident(text);
    tokens_.push_back(Token{
        .kind = kind,
        .span = span_from(start),
        .text = text,
        .error_message = {},
    });
  }

  // ==========================================================================
  //  Number scanning
  // ==========================================================================

  void scan_number(byte_offset start) {
    // We've already consumed the first digit.
    char first = source_[start];

    if (first == '0' && !at_end()) {
      char next = peek();
      if (next == 'x' || next == 'X') {
        advance();  // consume 'x'
        scan_hex_digits(start);
        emit(token_kind::int_lit, start);
        return;
      }
      if (next == 'o' || next == 'O') {
        advance();  // consume 'o'
        scan_oct_digits(start);
        emit(token_kind::int_lit, start);
        return;
      }
      if (next == 'b' || next == 'B') {
        advance();  // consume 'b'
        scan_bin_digits(start);
        emit(token_kind::int_lit, start);
        return;
      }
    }

    // Decimal integer or float.
    scan_dec_digits();

    // Check for float: either '.' followed by digit, or 'e'/'E'.
    if (!at_end() && peek() == '.' && peek_at(1) != '.' &&
        is_digit(peek_at(1))) {
      advance();  // consume '.'
      scan_dec_digits();
      scan_optional_exponent();
      emit(token_kind::float_lit, start);
      return;
    }

    if (!at_end() && (peek() == 'e' || peek() == 'E')) {
      scan_optional_exponent();
      emit(token_kind::float_lit, start);
      return;
    }

    emit(token_kind::int_lit, start);
  }

  void scan_dec_digits() {
    while (!at_end() && (is_digit(peek()) || peek() == '_')) {
      advance();
    }
  }

  void scan_hex_digits(byte_offset start) {
    if (at_end() || !is_hex_digit(peek())) {
      emit_error(start,
          "expected hexadecimal digits after `0x` — for example, `0xFF` or "
          "`0x1A2B`");
      return;
    }
    while (!at_end() && (is_hex_digit(peek()) || peek() == '_')) {
      advance();
    }
  }

  void scan_oct_digits(byte_offset start) {
    if (at_end() || !is_oct_digit(peek())) {
      emit_error(start,
          "expected octal digits (0-7) after `0o` — for example, `0o777`");
      return;
    }
    while (!at_end() && (is_oct_digit(peek()) || peek() == '_')) {
      // Check for invalid digits in octal.
      if (is_digit(peek()) && !is_oct_digit(peek()) && peek() != '_') {
        auto bad_start = static_cast<byte_offset>(pos_);
        advance();
        auto sp = Span{bad_start, static_cast<byte_offset>(pos_)};
        diag_.emit(
            diagnostic(diagnostic_level::error,
                       std::format("the digit `{}` is not valid in an octal "
                                   "literal — octal digits are 0 through 7",
                                   source_[bad_start]),
                       file_id_)
                .with_label(sp, "not a valid octal digit")
                .with_help("Octal literals use digits 0-7. If you meant a "
                           "decimal number, remove the `0o` prefix."));
      }
      advance();
    }
  }

  void scan_bin_digits(byte_offset start) {
    if (at_end() || !is_bin_digit(peek())) {
      emit_error(start,
          "expected binary digits (0 or 1) after `0b` — for example, "
          "`0b1010`");
      return;
    }
    while (!at_end() && (is_bin_digit(peek()) || peek() == '_')) {
      if (is_digit(peek()) && !is_bin_digit(peek()) && peek() != '_') {
        auto bad_start = static_cast<byte_offset>(pos_);
        advance();
        auto sp = Span{bad_start, static_cast<byte_offset>(pos_)};
        diag_.emit(
            diagnostic(diagnostic_level::error,
                       std::format("the digit `{}` is not valid in a binary "
                                   "literal — only `0` and `1` are allowed",
                                   source_[bad_start]),
                       file_id_)
                .with_label(sp, "not a valid binary digit")
                .with_help("Binary literals use only 0 and 1. If you meant "
                           "a decimal number, remove the `0b` prefix."));
      }
      advance();
    }
  }

  void scan_optional_exponent() {
    if (at_end() || (peek() != 'e' && peek() != 'E')) return;
    advance();  // consume 'e' or 'E'
    if (!at_end() && (peek() == '+' || peek() == '-')) {
      advance();
    }
    if (at_end() || !is_digit(peek())) {
      auto pos = static_cast<byte_offset>(pos_);
      diag_.emit(
          diagnostic(diagnostic_level::error,
                     "expected digits after the exponent `e` in a float "
                     "literal — for example, `1.5e10` or `2.0e-3`",
                     file_id_)
              .with_label(Span{pos, pos + 1}, "expected a digit here"));
      return;
    }
    scan_dec_digits();
  }

  // ==========================================================================
  //  String literal scanning
  // ==========================================================================

  void scan_string(byte_offset start) {
    // We've already consumed the opening `"`.
    // Track the opening quote position for error messages.
    byte_offset open_quote = start;

    while (!at_end()) {
      char c = peek();

      if (c == '"') {
        advance();  // consume closing quote
        emit(token_kind::string_lit, start);
        return;
      }

      if (c == '\n') {
        // Unterminated string at end of line.
          diag_.emit(
            diagnostic(diagnostic_level::error,
                       "unterminated string literal — the string started here "
                       "but the line ended before a closing `\"` was found",
                       file_id_)
                .with_label(Span{open_quote, open_quote + 1},
                            "string starts here")
                .with_label(Span{static_cast<byte_offset>(pos_),
                                 static_cast<byte_offset>(pos_) + 1},
                            "expected a closing `\"` before end of line")
                .with_help("Add a closing `\"` to end the string, or if you "
                           "need a multi-line string, use string "
                           "concatenation."));
        // Recovery: emit what we have as a string token and let the
        // parser continue.
        emit(token_kind::string_lit, start);
        return;
      }

      if (c == '\\') {
        advance();  // consume backslash
        scan_escape_sequence(open_quote);
        continue;
      }

      if (c == '{') {
        // String interpolation — we emit the whole string as one token.
        // The parser will handle splitting interpolation expressions.
        // For now, find the matching '}'.
        advance();  // consume '{'
        int depth = 1;
        while (!at_end() && depth > 0) {
          char ic = advance();
          if (ic == '{') ++depth;
          else if (ic == '}') --depth;
          else if (ic == '"') {
            // Nested string inside interpolation — scan recursively.
            // For simplicity, just skip balanced quotes.
            while (!at_end() && peek() != '"') {
              if (peek() == '\\') advance();
              advance();
            }
            if (!at_end()) advance();  // consume closing quote
          }
        }
        if (depth > 0) {
          diag_.emit(
              diagnostic(diagnostic_level::error,
                         "unterminated string interpolation — the `{` inside "
                         "this string was never closed with a matching `}`",
                         file_id_)
                  .with_label(Span{open_quote, open_quote + 1},
                              "in this string")
                  .with_help("Make sure every `{` inside a string has a "
                             "matching `}`. If you want a literal `{`, "
                             "escape it as `\\{`."));
        }
        continue;
      }

      advance();  // normal character
    }

    // Reached EOF without closing quote.
    diag_.emit(
        diagnostic(diagnostic_level::error,
                   "unterminated string literal — reached end of file without "
                   "finding a closing `\"`",
                   file_id_)
            .with_label(Span{open_quote, open_quote + 1},
                        "string starts here")
            .with_help("Add a closing `\"` to end this string."));
    emit(token_kind::string_lit, start);
  }

  void scan_escape_sequence([[maybe_unused]] byte_offset string_start) {
    if (at_end()) {
      diag_.emit(
          diagnostic(diagnostic_level::error,
                     "incomplete escape sequence at end of file — a `\\` was "
                     "found but nothing follows it",
                     file_id_)
              .with_label(Span{static_cast<byte_offset>(pos_ - 1),
                               static_cast<byte_offset>(pos_)},
                          "this backslash needs something after it")
              .with_help("Valid escape sequences include: \\n (newline), "
                         "\\t (tab), \\r (carriage return), \\\" (quote), "
                         "\\\\ (backslash), \\{ and \\} (braces), "
                         "\\u{...} (Unicode)."));
      return;
    }

    char c = advance();
    switch (c) {
      case 'n': case 't': case 'r': case '"': case '\'':
      case '\\': case '{': case '}':
        return;  // Valid simple escape.

      case 'u': {
        // Unicode escape: \u{XXXX}
        if (!match('{')) {
          diag_.emit(
              diagnostic(diagnostic_level::error,
                         "expected `{` after `\\u` in Unicode escape — the "
                         "correct syntax is `\\u{XXXX}` where XXXX are hex digits",
                         file_id_)
                  .with_label(Span{static_cast<byte_offset>(pos_ - 2),
                                   static_cast<byte_offset>(pos_)},
                              "here")
                  .with_help("Unicode escapes use the format `\\u{1F600}` — "
                             "hex digits inside curly braces."));
          return;
        }
        if (at_end() || !is_hex_digit(peek())) {
          diag_.emit(
              diagnostic(diagnostic_level::error,
                         "expected hex digits inside `\\u{...}` Unicode escape",
                         file_id_)
                  .with_label(Span{static_cast<byte_offset>(pos_ - 3),
                                   static_cast<byte_offset>(pos_)},
                              "here")
                  .with_help("Put one to six hexadecimal digits inside the "
                             "braces, like `\\u{41}` for 'A' or `\\u{1F600}` "
                             "for a smiley emoji."));
          return;
        }
        uint32_t hex_count = 0;
        while (!at_end() && is_hex_digit(peek()) && hex_count < 6) {
          advance();
          ++hex_count;
        }
        if (!match('}')) {
          diag_.emit(
              diagnostic(diagnostic_level::error,
                         "expected closing `}` for Unicode escape `\\u{...}`",
                         file_id_)
                  .with_label(Span{static_cast<byte_offset>(pos_ - hex_count - 3),
                                   static_cast<byte_offset>(pos_)},
                              "this Unicode escape is missing its closing `}`")
                  .with_help("Make sure the escape is written as "
                             "`\\u{digits}` with a closing brace."));
        }
        return;
      }

      default: {
        // Unknown escape sequence — give a helpful error.
        auto esc_span = Span{static_cast<byte_offset>(pos_ - 2),
                             static_cast<byte_offset>(pos_)};
        std::string msg = "unknown escape sequence `\\";
        msg += c;
        msg += "`";

        auto diag = diagnostic(diagnostic_level::error, msg, file_id_)
            .with_label(esc_span, "not a recognized escape");

        // Suggest common mistakes.
        if (c == '0') {
          diag.with_help(
              "Kira doesn't have a `\\0` escape for null. If you need a "
              "null byte, use `\\u{0}` instead.");
        } else if (c == 'x') {
          diag.with_help(
              "Kira doesn't have `\\xNN` hex escapes. Use `\\u{NN}` for "
              "Unicode code points instead.");
        } else if (c == 'a' || c == 'b' || c == 'f' || c == 'v') {
          diag.with_help(
              "The recognized escape sequences are: \\n \\t \\r \\\" \\' "
              "\\\\ \\{ \\} and \\u{XXXX} for Unicode.");
        } else {
          diag.with_help(
              "Valid escape sequences: \\n (newline), \\t (tab), "
              "\\r (carriage return), \\\" (quote), \\' (single quote), "
              "\\\\ (backslash), \\{ \\} (braces), \\u{XXXX} (Unicode).");
        }

        diag_.emit(std::move(diag));
        return;
      }
    }
  }

  // ==========================================================================
  //  Character literal scanning
  // ==========================================================================

  void scan_char(byte_offset start) {
    // We've already consumed the opening `'`.
    if (at_end()) {
      diag_.emit(
          diagnostic(diagnostic_level::error,
                     "unterminated character literal — expected a character "
                     "and closing `'`",
                     file_id_)
              .with_label(Span{start, static_cast<byte_offset>(pos_)},
                          "this `'` starts a character literal")
              .with_help("Character literals contain exactly one character: "
                         "`'a'`, `'\\n'`, `'\\u{1F600}'`."));
      emit(token_kind::char_lit, start);
      return;
    }

    if (peek() == '\'') {
      // Empty character literal.
      advance();
      diag_.emit(
          diagnostic(diagnostic_level::error,
                     "empty character literal — character literals must "
                     "contain exactly one character",
                     file_id_)
              .with_label(span_from(start), "this is empty")
              .with_help("Did you mean `' '` (a space), or perhaps a "
                         "string `\"\"`?"));
      emit(token_kind::char_lit, start);
      return;
    }

    if (peek() == '\\') {
      advance();  // consume backslash
      scan_escape_sequence(start);
    } else if (peek() == '\n') {
      diag_.emit(
          diagnostic(diagnostic_level::error,
                     "unterminated character literal — the line ended before "
                     "the closing `'` was found",
                     file_id_)
              .with_label(Span{start, static_cast<byte_offset>(pos_)},
                          "character literal starts here")
              .with_help("Add a closing `'` after the character."));
      emit(token_kind::char_lit, start);
      return;
    } else {
      advance();  // consume the character

      // Handle multi-byte UTF-8: consume continuation bytes.
      unsigned char first_byte = static_cast<unsigned char>(source_[pos_ - 1]);
      int extra_bytes = 0;
      if ((first_byte & 0xE0) == 0xC0) extra_bytes = 1;
      else if ((first_byte & 0xF0) == 0xE0) extra_bytes = 2;
      else if ((first_byte & 0xF8) == 0xF0) extra_bytes = 3;
      for (int i = 0; i < extra_bytes && !at_end(); ++i) {
        advance();
      }
    }

    // Expect closing quote.
    if (at_end() || peek() != '\'') {
      // Check if there are more characters — maybe they meant a string?
      if (!at_end() && peek() != '\n' && peek() != '\'') {
        // Multiple characters in char literal.
          while (!at_end() && peek() != '\'' && peek() != '\n') {
          advance();
        }
        bool has_close = !at_end() && peek() == '\'';
        if (has_close) advance();

        diag_.emit(
            diagnostic(diagnostic_level::error,
                       "character literal contains more than one character",
                       file_id_)
                .with_label(span_from(start), "this is too long for a `char`")
                .with_help("Character literals hold exactly one character. "
                           "If you need multiple characters, use a string "
                           "with double quotes: `\"...\"`."));
        emit(token_kind::char_lit, start);
        return;
      }

      diag_.emit(
          diagnostic(diagnostic_level::error,
                     "unterminated character literal — expected a closing `'`",
                     file_id_)
              .with_label(Span{start, static_cast<byte_offset>(pos_)},
                          "character literal starts here"));
      emit(token_kind::char_lit, start);
      return;
    }

    advance();  // consume closing quote
    emit(token_kind::char_lit, start);
  }

  // ==========================================================================
  //  Member variables
  // ==========================================================================

  std::string_view source_;
  FileId file_id_;
  diagnostic_bag& diag_;

  size_t pos_;
  std::vector<Token> tokens_;

  /// Stack of indentation levels. The bottom is always 0 (column 0).
  /// Each entry is the column number of an INDENT. We push on INDENT
  /// and pop on DEDENT.
  std::vector<uint32_t> indent_stack_;

  /// Number of currently open brackets (parens, square brackets, braces).
  /// While > 0, NEWLINE / INDENT / DEDENT are suppressed.
  int bracket_depth_ = 0;

  /// True if we're at the beginning of a new logical line and need to
  /// measure indentation before scanning tokens.
  bool at_line_start_ = true;
};

}  // namespace kira