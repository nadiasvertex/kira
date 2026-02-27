#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "source_location.h"

namespace kira {

// ==========================================================================
//  TokenKind — every distinct lexical element in the Kira language.
//
//  Naming conventions:
//    Kw*      — keywords
//    Lit*     — literal tokens
//    Punct*   — punctuation / operators
//    Special* — synthetic tokens produced by the lexer (INDENT, etc.)
// ==========================================================================
enum class TokenKind : uint16_t {
  // ------------------------------------------------------------------
  //  Sentinel / special
  // ------------------------------------------------------------------
  eof = 0,       ///< End of file
  error,         ///< Lexer error token — carries an error message
  newline,       ///< Logical newline (suppressed inside brackets)
  indent,        ///< Increase in indentation level
  dedent,        ///< Decrease in indentation level
  placeholder,   ///< Synthesized by error recovery — not real source

  // ------------------------------------------------------------------
  //  Identifiers and literals
  // ------------------------------------------------------------------
  ident,         ///< User identifier
  int_lit,       ///< Integer literal (decimal, hex, octal, binary)
  float_lit,     ///< Floating-point literal
  string_lit,    ///< String literal (including interpolations)
  char_lit,      ///< Character literal
  asm_content,   ///< Raw content of an asm{} block

  // ------------------------------------------------------------------
  //  Declaration keywords
  // ------------------------------------------------------------------
  kw_module,     ///< `module`
  kw_type,       ///< `type`
  kw_trait,      ///< `trait`
  kw_impl,       ///< `impl`
  kw_concept,    ///< `concept`
  kw_def,        ///< `def`
  kw_let,        ///< `let`
  kw_var,        ///< `var`
  kw_static,     ///< `static`
  kw_use,        ///< `use`
  kw_dep,        ///< `dep`

  // ------------------------------------------------------------------
  //  Function modifiers
  // ------------------------------------------------------------------
  kw_pure,       ///< `pure`
  kw_async,      ///< `async`
  kw_machine,    ///< `machine`

  // ------------------------------------------------------------------
  //  Visibility keywords
  // ------------------------------------------------------------------
  kw_pub,        ///< `pub`
  kw_internal,   ///< `internal`
  kw_super,      ///< `super`
  kw_priv,       ///< `priv`

  // ------------------------------------------------------------------
  //  Control flow keywords
  // ------------------------------------------------------------------
  kw_if,         ///< `if`
  kw_elif,       ///< `elif`
  kw_else,       ///< `else`
  kw_for,        ///< `for`
  kw_while,      ///< `while`
  kw_match,      ///< `match`
  kw_return,     ///< `return`
  kw_in,         ///< `in`
  kw_as,         ///< `as`

  // ------------------------------------------------------------------
  //  Pattern keywords
  // ------------------------------------------------------------------
  kw_underscore, ///< `_` (wildcard pattern)
  kw_some,       ///< `some`
  kw_ok,         ///< `ok`
  kw_err,        ///< `err`

  // ------------------------------------------------------------------
  //  Expression keywords
  // ------------------------------------------------------------------
  kw_and,        ///< `and`
  kw_or,         ///< `or`
  kw_not,        ///< `not`
  kw_await,      ///< `await`
  kw_yield,      ///< `yield`
  kw_par,        ///< `par`
  kw_race,       ///< `race`
  kw_on,         ///< `on`
  kw_crew,       ///< `crew`
  kw_asm,        ///< `asm`

  // ------------------------------------------------------------------
  //  Contract keywords
  // ------------------------------------------------------------------
  kw_pre,        ///< `pre`
  kw_post,       ///< `post`
  kw_invariant,  ///< `invariant`

  // ------------------------------------------------------------------
  //  Compile-time keywords
  // ------------------------------------------------------------------
  kw_where,      ///< `where`
  kw_requires,   ///< `requires`
  kw_deriving,   ///< `deriving`
  kw_no_prelude, ///< `no_prelude`

  // ------------------------------------------------------------------
  //  Capture / ownership keywords
  // ------------------------------------------------------------------
  kw_move,       ///< `move`
  kw_shared,     ///< `shared`
  kw_mut,        ///< `mut`

  // ------------------------------------------------------------------
  //  Value literal keywords
  // ------------------------------------------------------------------
  kw_true,       ///< `true`
  kw_false,      ///< `false`
  kw_unit,       ///< `unit`

  // ------------------------------------------------------------------
  //  Type-related keywords
  // ------------------------------------------------------------------
  kw_array,      ///< `array`
  kw_fn,         ///< `fn`
  kw_expr,       ///< `expr` (quote type)
  kw_stmt,       ///< `stmt` (quote type)
  kw_def_expr,   ///< `def_expr` (quote type)
  kw_type_expr,  ///< `type_expr` (quote type)
  kw_assert,     ///< `assert` (used in static assert)

  // ------------------------------------------------------------------
  //  Punctuation — single character
  // ------------------------------------------------------------------
  lparen,        ///< `(`
  rparen,        ///< `)`
  lbracket,      ///< `[`
  rbracket,      ///< `]`
  lbrace,        ///< `{`
  rbrace,        ///< `}`
  colon,         ///< `:`
  comma,         ///< `,`
  dot,           ///< `.`
  semicolon,     ///< `;`
  hash,          ///< `#`
  at,            ///< `@`
  backtick,      ///< `` ` ``
  question,      ///< `?`

  // ------------------------------------------------------------------
  //  Operators — single character
  // ------------------------------------------------------------------
  eq,            ///< `=`
  plus,          ///< `+`
  minus,         ///< `-`
  star,          ///< `*`
  slash,         ///< `/`
  percent,       ///< `%`
  amp,           ///< `&`
  pipe,          ///< `|`
  caret,         ///< `^`
  tilde,         ///< `~`
  lt,            ///< `<`
  gt,            ///< `>`

  // ------------------------------------------------------------------
  //  Operators — multi-character
  // ------------------------------------------------------------------
  eq_eq,         ///< `==`
  bang_eq,       ///< `!=`
  lt_eq,         ///< `<=`
  gt_eq,         ///< `>=`
  arrow,         ///< `->`
  fat_arrow,     ///< `=>`
  dot_dot,       ///< `..`
  dot_dot_eq,    ///< `..=`
  lt_lt,         ///< `<<`
  gt_gt,         ///< `>>`

  // ------------------------------------------------------------------
  //  Compound assignment operators
  // ------------------------------------------------------------------
  plus_eq,       ///< `+=`
  minus_eq,      ///< `-=`
  star_eq,       ///< `*=`
  slash_eq,      ///< `/=`
  percent_eq,    ///< `%=`
  amp_eq,        ///< `&=`
  pipe_eq,       ///< `|=`
  caret_eq,      ///< `^=`
  lt_lt_eq,      ///< `<<=`
  gt_gt_eq,      ///< `>>=`

  // ------------------------------------------------------------------
  //  Wrapping arithmetic operators (machine mode)
  // ------------------------------------------------------------------
  plus_percent,   ///< `+%`  (wrapping add)
  minus_percent,  ///< `-%`  (wrapping sub)
  star_percent,   ///< `*%`  (wrapping mul)
  plus_percent_eq, ///< `+%=` (wrapping add assign)
  minus_percent_eq,///< `-%=` (wrapping sub assign)
  star_percent_eq, ///< `*%=` (wrapping mul assign)

  // ------------------------------------------------------------------
  //  Saturating arithmetic operators (machine mode)
  // ------------------------------------------------------------------
  plus_pipe,     ///< `+|`  (saturating add)
  minus_pipe,    ///< `-|`  (saturating sub)
  star_pipe,     ///< `*|`  (saturating mul)
  plus_pipe_eq,  ///< `+|=` (saturating add assign)
  minus_pipe_eq, ///< `-|=` (saturating sub assign)
  star_pipe_eq,  ///< `*|=` (saturating mul assign)

  // Keep this last — used for iteration bounds.
  _count,
};

// ==========================================================================
//  Token — a single lexical token with its kind, location, and text.
// ==========================================================================
struct Token {
  TokenKind kind = TokenKind::eof;
  Span span;

  /// The raw source text of this token. For most tokens this is a view into
  /// the original source buffer; for synthesized tokens (error recovery) it
  /// may be empty or hold a placeholder string.
  std::string_view text;

  /// For Error tokens, this carries a short human-readable description of
  /// what went wrong during lexing (e.g., "unterminated string literal").
  /// Empty for all non-error tokens.
  std::string_view error_message;

  // Convenience predicates ------------------------------------------------

  [[nodiscard]] constexpr bool is(TokenKind k) const noexcept {
    return kind == k;
  }

  [[nodiscard]] constexpr bool is_not(TokenKind k) const noexcept {
    return kind != k;
  }

  template <typename... Kinds>
  [[nodiscard]] constexpr bool is_one_of(Kinds... kinds) const noexcept {
    return ((kind == kinds) || ...);
  }

  [[nodiscard]] constexpr bool is_eof() const noexcept {
    return kind == TokenKind::eof;
  }

  [[nodiscard]] constexpr bool is_error() const noexcept {
    return kind == TokenKind::error;
  }

  [[nodiscard]] constexpr bool is_newline() const noexcept {
    return kind == TokenKind::newline;
  }

  [[nodiscard]] constexpr bool is_keyword() const noexcept {
    return kind >= TokenKind::kw_module && kind <= TokenKind::kw_assert;
  }

  [[nodiscard]] constexpr bool is_literal() const noexcept {
    return kind >= TokenKind::int_lit && kind <= TokenKind::char_lit;
  }

  [[nodiscard]] constexpr bool is_literal_keyword() const noexcept {
    return kind == TokenKind::kw_true ||
           kind == TokenKind::kw_false ||
           kind == TokenKind::kw_unit;
  }

  [[nodiscard]] constexpr bool is_visibility() const noexcept {
    return kind == TokenKind::kw_pub ||
           kind == TokenKind::kw_internal ||
           kind == TokenKind::kw_super ||
           kind == TokenKind::kw_priv;
  }

  [[nodiscard]] constexpr bool is_func_modifier() const noexcept {
    return kind == TokenKind::kw_pure ||
           kind == TokenKind::kw_async ||
           kind == TokenKind::kw_machine ||
           kind == TokenKind::kw_static;
  }

  [[nodiscard]] constexpr bool is_assign_op() const noexcept {
    return kind == TokenKind::eq ||
           kind == TokenKind::plus_eq ||
           kind == TokenKind::minus_eq ||
           kind == TokenKind::star_eq ||
           kind == TokenKind::slash_eq ||
           kind == TokenKind::percent_eq ||
           kind == TokenKind::amp_eq ||
           kind == TokenKind::pipe_eq ||
           kind == TokenKind::caret_eq ||
           kind == TokenKind::lt_lt_eq ||
           kind == TokenKind::gt_gt_eq ||
           kind == TokenKind::plus_percent_eq ||
           kind == TokenKind::minus_percent_eq ||
           kind == TokenKind::star_percent_eq ||
           kind == TokenKind::plus_pipe_eq ||
           kind == TokenKind::minus_pipe_eq ||
           kind == TokenKind::star_pipe_eq;
  }

  [[nodiscard]] constexpr bool is_cmp_op() const noexcept {
    return kind == TokenKind::eq_eq ||
           kind == TokenKind::bang_eq ||
           kind == TokenKind::lt ||
           kind == TokenKind::lt_eq ||
           kind == TokenKind::gt ||
           kind == TokenKind::gt_eq ||
           kind == TokenKind::kw_in ||
           kind == TokenKind::kw_not;  // `not in`
  }

  [[nodiscard]] constexpr bool is_add_op() const noexcept {
    return kind == TokenKind::plus ||
           kind == TokenKind::minus ||
           kind == TokenKind::plus_percent ||
           kind == TokenKind::minus_percent ||
           kind == TokenKind::plus_pipe ||
           kind == TokenKind::minus_pipe;
  }

  [[nodiscard]] constexpr bool is_mul_op() const noexcept {
    return kind == TokenKind::star ||
           kind == TokenKind::slash ||
           kind == TokenKind::percent ||
           kind == TokenKind::star_percent ||
           kind == TokenKind::star_pipe;
  }

  [[nodiscard]] constexpr bool is_unary_op() const noexcept {
    return kind == TokenKind::minus ||
           kind == TokenKind::tilde ||
           kind == TokenKind::star ||
           kind == TokenKind::amp;
  }

  /// Returns true if this token can start an expression.
  [[nodiscard]] constexpr bool can_start_expr() const noexcept {
    switch (kind) {
      case TokenKind::ident:
      case TokenKind::int_lit:
      case TokenKind::float_lit:
      case TokenKind::string_lit:
      case TokenKind::char_lit:
      case TokenKind::kw_true:
      case TokenKind::kw_false:
      case TokenKind::kw_unit:
      case TokenKind::lparen:
      case TokenKind::lbracket:
      case TokenKind::lbrace:
      case TokenKind::minus:
      case TokenKind::tilde:
      case TokenKind::star:
      case TokenKind::amp:
      case TokenKind::kw_not:
      case TokenKind::kw_if:
      case TokenKind::kw_match:
      case TokenKind::kw_for:
      case TokenKind::kw_await:
      case TokenKind::kw_par:
      case TokenKind::kw_race:
      case TokenKind::kw_on:
      case TokenKind::kw_pure:
      case TokenKind::kw_move:
      case TokenKind::kw_static:
      case TokenKind::backtick:
        return true;
      default:
        return false;
    }
  }

  /// Returns true if this token can start a statement.
  [[nodiscard]] constexpr bool can_start_stmt() const noexcept {
    switch (kind) {
      case TokenKind::kw_let:
      case TokenKind::kw_var:
      case TokenKind::kw_return:
      case TokenKind::kw_if:
      case TokenKind::kw_while:
      case TokenKind::kw_for:
      case TokenKind::kw_match:
      case TokenKind::kw_crew:
      case TokenKind::kw_asm:
      case TokenKind::kw_use:
      case TokenKind::kw_type:
      case TokenKind::kw_def:
      case TokenKind::kw_static:
      case TokenKind::kw_pub:
      case TokenKind::kw_internal:
      case TokenKind::kw_super:
      case TokenKind::kw_priv:
      case TokenKind::kw_pure:
      case TokenKind::kw_async:
      case TokenKind::kw_machine:
      case TokenKind::tilde:
      case TokenKind::newline:
        return true;
      default:
        return can_start_expr();
    }
  }

  /// Returns true if this token can start a top-level item.
  [[nodiscard]] constexpr bool can_start_top_level() const noexcept {
    switch (kind) {
      case TokenKind::kw_use:
      case TokenKind::kw_type:
      case TokenKind::kw_trait:
      case TokenKind::kw_impl:
      case TokenKind::kw_concept:
      case TokenKind::kw_def:
      case TokenKind::kw_static:
      case TokenKind::kw_module:
      case TokenKind::kw_dep:
      case TokenKind::kw_pub:
      case TokenKind::kw_internal:
      case TokenKind::kw_super:
      case TokenKind::kw_priv:
      case TokenKind::kw_pure:
      case TokenKind::kw_async:
      case TokenKind::kw_machine:
      case TokenKind::tilde:
      case TokenKind::newline:
        return true;
      default:
        return false;
    }
  }
};

// ==========================================================================
//  Keyword lookup — maps identifier text to keyword TokenKind, or
//  returns TokenKind::Ident if the string is not a keyword.
// ==========================================================================
[[nodiscard]] inline TokenKind classify_ident(std::string_view text) noexcept {
  // We use a simple if-chain. For ~50 keywords this is perfectly fine and
  // avoids pulling in a hash map dependency. The compiler will likely
  // optimize this into a switch on the first character anyway.

  // Sort by frequency of occurrence in typical Kira code to get early-out
  // on the most common identifiers, which are NOT keywords.
  if (text.empty()) return TokenKind::ident;

  switch (text[0]) {
    case 'a':
      if (text == "and")       return TokenKind::kw_and;
      if (text == "as")        return TokenKind::kw_as;
      if (text == "async")     return TokenKind::kw_async;
      if (text == "await")     return TokenKind::kw_await;
      if (text == "asm")       return TokenKind::kw_asm;
      if (text == "array")     return TokenKind::kw_array;
      if (text == "assert")    return TokenKind::kw_assert;
      break;
    case 'c':
      if (text == "concept")   return TokenKind::kw_concept;
      if (text == "crew")      return TokenKind::kw_crew;
      break;
    case 'd':
      if (text == "def")       return TokenKind::kw_def;
      if (text == "dep")       return TokenKind::kw_dep;
      if (text == "deriving")  return TokenKind::kw_deriving;
      if (text == "def_expr")  return TokenKind::kw_def_expr;
      break;
    case 'e':
      if (text == "else")      return TokenKind::kw_else;
      if (text == "elif")      return TokenKind::kw_elif;
      if (text == "err")       return TokenKind::kw_err;
      if (text == "expr")      return TokenKind::kw_expr;
      break;
    case 'f':
      if (text == "for")       return TokenKind::kw_for;
      if (text == "false")     return TokenKind::kw_false;
      if (text == "fn")        return TokenKind::kw_fn;
      break;
    case 'i':
      if (text == "if")        return TokenKind::kw_if;
      if (text == "in")        return TokenKind::kw_in;
      if (text == "impl")      return TokenKind::kw_impl;
      if (text == "internal")  return TokenKind::kw_internal;
      if (text == "invariant") return TokenKind::kw_invariant;
      break;
    case 'l':
      if (text == "let")       return TokenKind::kw_let;
      break;
    case 'm':
      if (text == "match")     return TokenKind::kw_match;
      if (text == "module")    return TokenKind::kw_module;
      if (text == "mut")       return TokenKind::kw_mut;
      if (text == "move")      return TokenKind::kw_move;
      if (text == "machine")   return TokenKind::kw_machine;
      break;
    case 'n':
      if (text == "not")       return TokenKind::kw_not;
      if (text == "no_prelude") return TokenKind::kw_no_prelude;
      break;
    case 'o':
      if (text == "or")        return TokenKind::kw_or;
      if (text == "ok")        return TokenKind::kw_ok;
      if (text == "on")        return TokenKind::kw_on;
      break;
    case 'p':
      if (text == "pub")       return TokenKind::kw_pub;
      if (text == "pure")      return TokenKind::kw_pure;
      if (text == "par")       return TokenKind::kw_par;
      if (text == "pre")       return TokenKind::kw_pre;
      if (text == "post")      return TokenKind::kw_post;
      if (text == "priv")      return TokenKind::kw_priv;
      break;
    case 'r':
      if (text == "return")    return TokenKind::kw_return;
      if (text == "race")      return TokenKind::kw_race;
      if (text == "requires")  return TokenKind::kw_requires;
      break;
    case 's':
      if (text == "static")    return TokenKind::kw_static;
      if (text == "some")      return TokenKind::kw_some;
      if (text == "super")     return TokenKind::kw_super;
      if (text == "shared")    return TokenKind::kw_shared;
      if (text == "stmt")      return TokenKind::kw_stmt;
      break;
    case 't':
      if (text == "type")      return TokenKind::kw_type;
      if (text == "trait")     return TokenKind::kw_trait;
      if (text == "true")      return TokenKind::kw_true;
      if (text == "type_expr") return TokenKind::kw_type_expr;
      break;
    case 'u':
      if (text == "use")       return TokenKind::kw_use;
      if (text == "unit")      return TokenKind::kw_unit;
      break;
    case 'v':
      if (text == "var")       return TokenKind::kw_var;
      break;
    case 'w':
      if (text == "while")     return TokenKind::kw_while;
      if (text == "where")     return TokenKind::kw_where;
      break;
    case 'y':
      if (text == "yield")     return TokenKind::kw_yield;
      break;
    default:
      break;
  }

  // `_` is a special keyword (wildcard pattern) but only when it's exactly
  // a lone underscore. Identifiers like `_foo` are normal identifiers.
  if (text == "_") return TokenKind::kw_underscore;

  return TokenKind::ident;
}

// ==========================================================================
//  token_kind_name — returns a human-readable name for a TokenKind.
//
//  This is used in diagnostic messages. The names are chosen to be
//  understandable to someone learning Kira — not compiler-internals jargon.
// ==========================================================================
[[nodiscard]] constexpr std::string_view token_kind_name(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::eof:              return "end of file";
    case TokenKind::error:            return "error";
    case TokenKind::newline:          return "newline";
    case TokenKind::indent:           return "indent";
    case TokenKind::dedent:           return "dedent";
    case TokenKind::placeholder:      return "<placeholder>";

    case TokenKind::ident:            return "identifier";
    case TokenKind::int_lit:          return "integer literal";
    case TokenKind::float_lit:        return "float literal";
    case TokenKind::string_lit:       return "string literal";
    case TokenKind::char_lit:         return "character literal";
    case TokenKind::asm_content:      return "assembly content";

    case TokenKind::kw_module:        return "`module`";
    case TokenKind::kw_type:          return "`type`";
    case TokenKind::kw_trait:         return "`trait`";
    case TokenKind::kw_impl:          return "`impl`";
    case TokenKind::kw_concept:       return "`concept`";
    case TokenKind::kw_def:           return "`def`";
    case TokenKind::kw_let:           return "`let`";
    case TokenKind::kw_var:           return "`var`";
    case TokenKind::kw_static:        return "`static`";
    case TokenKind::kw_use:           return "`use`";
    case TokenKind::kw_dep:           return "`dep`";

    case TokenKind::kw_pure:          return "`pure`";
    case TokenKind::kw_async:         return "`async`";
    case TokenKind::kw_machine:       return "`machine`";

    case TokenKind::kw_pub:           return "`pub`";
    case TokenKind::kw_internal:      return "`internal`";
    case TokenKind::kw_super:         return "`super`";
    case TokenKind::kw_priv:          return "`priv`";

    case TokenKind::kw_if:            return "`if`";
    case TokenKind::kw_elif:          return "`elif`";
    case TokenKind::kw_else:          return "`else`";
    case TokenKind::kw_for:           return "`for`";
    case TokenKind::kw_while:         return "`while`";
    case TokenKind::kw_match:         return "`match`";
    case TokenKind::kw_return:        return "`return`";
    case TokenKind::kw_in:            return "`in`";
    case TokenKind::kw_as:            return "`as`";

    case TokenKind::kw_underscore:    return "`_`";
    case TokenKind::kw_some:          return "`some`";
    case TokenKind::kw_ok:            return "`ok`";
    case TokenKind::kw_err:           return "`err`";

    case TokenKind::kw_and:           return "`and`";
    case TokenKind::kw_or:            return "`or`";
    case TokenKind::kw_not:           return "`not`";
    case TokenKind::kw_await:         return "`await`";
    case TokenKind::kw_yield:         return "`yield`";
    case TokenKind::kw_par:           return "`par`";
    case TokenKind::kw_race:          return "`race`";
    case TokenKind::kw_on:            return "`on`";
    case TokenKind::kw_crew:          return "`crew`";
    case TokenKind::kw_asm:           return "`asm`";

    case TokenKind::kw_pre:           return "`pre`";
    case TokenKind::kw_post:          return "`post`";
    case TokenKind::kw_invariant:     return "`invariant`";

    case TokenKind::kw_where:         return "`where`";
    case TokenKind::kw_requires:      return "`requires`";
    case TokenKind::kw_deriving:      return "`deriving`";
    case TokenKind::kw_no_prelude:    return "`no_prelude`";

    case TokenKind::kw_move:          return "`move`";
    case TokenKind::kw_shared:        return "`shared`";
    case TokenKind::kw_mut:           return "`mut`";

    case TokenKind::kw_true:          return "`true`";
    case TokenKind::kw_false:         return "`false`";
    case TokenKind::kw_unit:          return "`unit`";

    case TokenKind::kw_array:         return "`array`";
    case TokenKind::kw_fn:            return "`fn`";
    case TokenKind::kw_expr:          return "`expr`";
    case TokenKind::kw_stmt:          return "`stmt`";
    case TokenKind::kw_def_expr:      return "`def_expr`";
    case TokenKind::kw_type_expr:     return "`type_expr`";
    case TokenKind::kw_assert:        return "`assert`";

    case TokenKind::lparen:           return "`(`";
    case TokenKind::rparen:           return "`)`";
    case TokenKind::lbracket:         return "`[`";
    case TokenKind::rbracket:         return "`]`";
    case TokenKind::lbrace:           return "`{`";
    case TokenKind::rbrace:           return "`}`";
    case TokenKind::colon:            return "`:`";
    case TokenKind::comma:            return "`,`";
    case TokenKind::dot:              return "`.`";
    case TokenKind::semicolon:        return "`;`";
    case TokenKind::hash:             return "`#`";
    case TokenKind::at:               return "`@`";
    case TokenKind::backtick:         return "`` ` ``";
    case TokenKind::question:         return "`?`";

    case TokenKind::eq:               return "`=`";
    case TokenKind::plus:             return "`+`";
    case TokenKind::minus:            return "`-`";
    case TokenKind::star:             return "`*`";
    case TokenKind::slash:            return "`/`";
    case TokenKind::percent:          return "`%`";
    case TokenKind::amp:              return "`&`";
    case TokenKind::pipe:             return "`|`";
    case TokenKind::caret:            return "`^`";
    case TokenKind::tilde:            return "`~`";
    case TokenKind::lt:               return "`<`";
    case TokenKind::gt:               return "`>`";

    case TokenKind::eq_eq:            return "`==`";
    case TokenKind::bang_eq:          return "`!=`";
    case TokenKind::lt_eq:            return "`<=`";
    case TokenKind::gt_eq:            return "`>=`";
    case TokenKind::arrow:            return "`->`";
    case TokenKind::fat_arrow:        return "`=>`";
    case TokenKind::dot_dot:          return "`..`";
    case TokenKind::dot_dot_eq:       return "`..=`";
    case TokenKind::lt_lt:            return "`<<`";
    case TokenKind::gt_gt:            return "`>>`";

    case TokenKind::plus_eq:          return "`+=`";
    case TokenKind::minus_eq:         return "`-=`";
    case TokenKind::star_eq:          return "`*=`";
    case TokenKind::slash_eq:         return "`/=`";
    case TokenKind::percent_eq:       return "`%=`";
    case TokenKind::amp_eq:           return "`&=`";
    case TokenKind::pipe_eq:          return "`|=`";
    case TokenKind::caret_eq:         return "`^=`";
    case TokenKind::lt_lt_eq:         return "`<<=`";
    case TokenKind::gt_gt_eq:         return "`>>=`";

    case TokenKind::plus_percent:     return "`+%`";
    case TokenKind::minus_percent:    return "`-%`";
    case TokenKind::star_percent:     return "`*%`";
    case TokenKind::plus_percent_eq:  return "`+%=`";
    case TokenKind::minus_percent_eq: return "`-%=`";
    case TokenKind::star_percent_eq:  return "`*%=`";

    case TokenKind::plus_pipe:        return "`+|`";
    case TokenKind::minus_pipe:       return "`-|`";
    case TokenKind::star_pipe:        return "`*|`";
    case TokenKind::plus_pipe_eq:     return "`+|=`";
    case TokenKind::minus_pipe_eq:    return "`-|=`";
    case TokenKind::star_pipe_eq:     return "`*|=`";

    case TokenKind::_count:           return "<invalid>";
  }
  return "<unknown>";
}

/// Returns a short, user-friendly description of what token was expected.
/// For punctuation and keywords, returns the literal text. For categories
/// like Ident, returns a prose description.
[[nodiscard]] constexpr std::string_view token_kind_description(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::ident:     return "a name";
    case TokenKind::int_lit:   return "a number";
    case TokenKind::float_lit: return "a number";
    case TokenKind::string_lit: return "a string";
    case TokenKind::char_lit:  return "a character";
    case TokenKind::newline:   return "end of line";
    case TokenKind::indent:    return "an indented block";
    case TokenKind::dedent:    return "end of block";
    case TokenKind::eof:       return "end of file";
    default:                   return token_kind_name(kind);
  }
}

}  // namespace kira