#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "source_location.h"

namespace kira {

// ==========================================================================
//  token_kind — every distinct lexical element in the Kira language.
//
//  Naming conventions:
//    Kw*      — keywords
//    Lit*     — literal tokens
//    Punct*   — punctuation / operators
//    Special* — synthetic tokens produced by the lexer (INDENT, etc.)
// ==========================================================================
enum class token_kind : uint16_t {
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
  token_kind kind = token_kind::eof;
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

  [[nodiscard]] constexpr bool is(token_kind k) const noexcept {
    return kind == k;
  }

  [[nodiscard]] constexpr bool is_not(token_kind k) const noexcept {
    return kind != k;
  }

  template <typename... Kinds>
  [[nodiscard]] constexpr bool is_one_of(Kinds... kinds) const noexcept {
    return ((kind == kinds) || ...);
  }

  [[nodiscard]] constexpr bool is_eof() const noexcept {
    return kind == token_kind::eof;
  }

  [[nodiscard]] constexpr bool is_error() const noexcept {
    return kind == token_kind::error;
  }

  [[nodiscard]] constexpr bool is_newline() const noexcept {
    return kind == token_kind::newline;
  }

  [[nodiscard]] constexpr bool is_keyword() const noexcept {
    return kind >= token_kind::kw_module && kind <= token_kind::kw_assert;
  }

  [[nodiscard]] constexpr bool is_literal() const noexcept {
    return kind >= token_kind::int_lit && kind <= token_kind::char_lit;
  }

  [[nodiscard]] constexpr bool is_literal_keyword() const noexcept {
    return kind == token_kind::kw_true ||
           kind == token_kind::kw_false ||
           kind == token_kind::kw_unit;
  }

  [[nodiscard]] constexpr bool is_visibility() const noexcept {
    return kind == token_kind::kw_pub ||
           kind == token_kind::kw_internal ||
           kind == token_kind::kw_super ||
           kind == token_kind::kw_priv;
  }

  [[nodiscard]] constexpr bool is_func_modifier() const noexcept {
    return kind == token_kind::kw_pure ||
           kind == token_kind::kw_async ||
           kind == token_kind::kw_machine ||
           kind == token_kind::kw_static;
  }

  [[nodiscard]] constexpr bool is_assign_op() const noexcept {
    return kind == token_kind::eq ||
           kind == token_kind::plus_eq ||
           kind == token_kind::minus_eq ||
           kind == token_kind::star_eq ||
           kind == token_kind::slash_eq ||
           kind == token_kind::percent_eq ||
           kind == token_kind::amp_eq ||
           kind == token_kind::pipe_eq ||
           kind == token_kind::caret_eq ||
           kind == token_kind::lt_lt_eq ||
           kind == token_kind::gt_gt_eq ||
           kind == token_kind::plus_percent_eq ||
           kind == token_kind::minus_percent_eq ||
           kind == token_kind::star_percent_eq ||
           kind == token_kind::plus_pipe_eq ||
           kind == token_kind::minus_pipe_eq ||
           kind == token_kind::star_pipe_eq;
  }

  [[nodiscard]] constexpr bool is_cmp_op() const noexcept {
    return kind == token_kind::eq_eq ||
           kind == token_kind::bang_eq ||
           kind == token_kind::lt ||
           kind == token_kind::lt_eq ||
           kind == token_kind::gt ||
           kind == token_kind::gt_eq ||
           kind == token_kind::kw_in ||
           kind == token_kind::kw_not;  // `not in`
  }

  [[nodiscard]] constexpr bool is_add_op() const noexcept {
    return kind == token_kind::plus ||
           kind == token_kind::minus ||
           kind == token_kind::plus_percent ||
           kind == token_kind::minus_percent ||
           kind == token_kind::plus_pipe ||
           kind == token_kind::minus_pipe;
  }

  [[nodiscard]] constexpr bool is_mul_op() const noexcept {
    return kind == token_kind::star ||
           kind == token_kind::slash ||
           kind == token_kind::percent ||
           kind == token_kind::star_percent ||
           kind == token_kind::star_pipe;
  }

  [[nodiscard]] constexpr bool is_unary_op() const noexcept {
    return kind == token_kind::minus ||
           kind == token_kind::tilde ||
           kind == token_kind::star ||
           kind == token_kind::amp;
  }

  /// Returns true if this token can start an expression.
  [[nodiscard]] constexpr bool can_start_expr() const noexcept {
    switch (kind) {
      case token_kind::ident:
      case token_kind::int_lit:
      case token_kind::float_lit:
      case token_kind::string_lit:
      case token_kind::char_lit:
      case token_kind::kw_true:
      case token_kind::kw_false:
      case token_kind::kw_unit:
      case token_kind::lparen:
      case token_kind::lbracket:
      case token_kind::lbrace:
      case token_kind::minus:
      case token_kind::tilde:
      case token_kind::star:
      case token_kind::amp:
      case token_kind::kw_not:
      case token_kind::kw_if:
      case token_kind::kw_match:
      case token_kind::kw_for:
      case token_kind::kw_await:
      case token_kind::kw_par:
      case token_kind::kw_race:
      case token_kind::kw_on:
      case token_kind::kw_pure:
      case token_kind::kw_move:
      case token_kind::kw_static:
      case token_kind::backtick:
        return true;
      default:
        return false;
    }
  }

  /// Returns true if this token can start a statement.
  [[nodiscard]] constexpr bool can_start_stmt() const noexcept {
    switch (kind) {
      case token_kind::kw_let:
      case token_kind::kw_var:
      case token_kind::kw_return:
      case token_kind::kw_if:
      case token_kind::kw_while:
      case token_kind::kw_for:
      case token_kind::kw_match:
      case token_kind::kw_crew:
      case token_kind::kw_asm:
      case token_kind::kw_use:
      case token_kind::kw_type:
      case token_kind::kw_def:
      case token_kind::kw_static:
      case token_kind::kw_pub:
      case token_kind::kw_internal:
      case token_kind::kw_super:
      case token_kind::kw_priv:
      case token_kind::kw_pure:
      case token_kind::kw_async:
      case token_kind::kw_machine:
      case token_kind::tilde:
      case token_kind::newline:
        return true;
      default:
        return can_start_expr();
    }
  }

  /// Returns true if this token can start a top-level item.
  [[nodiscard]] constexpr bool can_start_top_level() const noexcept {
    switch (kind) {
      case token_kind::kw_use:
      case token_kind::kw_type:
      case token_kind::kw_trait:
      case token_kind::kw_impl:
      case token_kind::kw_concept:
      case token_kind::kw_def:
      case token_kind::kw_static:
      case token_kind::kw_module:
      case token_kind::kw_dep:
      case token_kind::kw_pub:
      case token_kind::kw_internal:
      case token_kind::kw_super:
      case token_kind::kw_priv:
      case token_kind::kw_pure:
      case token_kind::kw_async:
      case token_kind::kw_machine:
      case token_kind::tilde:
      case token_kind::newline:
        return true;
      default:
        return false;
    }
  }
};

// ==========================================================================
//  Keyword lookup — maps identifier text to keyword token_kind, or
//  returns token_kind::ident if the string is not a keyword.
// ==========================================================================
[[nodiscard]] inline token_kind classify_ident(std::string_view text) noexcept {
  // We use a simple if-chain. For ~50 keywords this is perfectly fine and
  // avoids pulling in a hash map dependency. The compiler will likely
  // optimize this into a switch on the first character anyway.

  // Sort by frequency of occurrence in typical Kira code to get early-out
  // on the most common identifiers, which are NOT keywords.
  if (text.empty()) return token_kind::ident;

  switch (text[0]) {
    case 'a':
      if (text == "and")       return token_kind::kw_and;
      if (text == "as")        return token_kind::kw_as;
      if (text == "async")     return token_kind::kw_async;
      if (text == "await")     return token_kind::kw_await;
      if (text == "asm")       return token_kind::kw_asm;
      if (text == "array")     return token_kind::kw_array;
      if (text == "assert")    return token_kind::kw_assert;
      break;
    case 'c':
      if (text == "concept")   return token_kind::kw_concept;
      if (text == "crew")      return token_kind::kw_crew;
      break;
    case 'd':
      if (text == "def")       return token_kind::kw_def;
      if (text == "dep")       return token_kind::kw_dep;
      if (text == "deriving")  return token_kind::kw_deriving;
      if (text == "def_expr")  return token_kind::kw_def_expr;
      break;
    case 'e':
      if (text == "else")      return token_kind::kw_else;
      if (text == "elif")      return token_kind::kw_elif;
      if (text == "err")       return token_kind::kw_err;
      if (text == "expr")      return token_kind::kw_expr;
      break;
    case 'f':
      if (text == "for")       return token_kind::kw_for;
      if (text == "false")     return token_kind::kw_false;
      if (text == "fn")        return token_kind::kw_fn;
      break;
    case 'i':
      if (text == "if")        return token_kind::kw_if;
      if (text == "in")        return token_kind::kw_in;
      if (text == "impl")      return token_kind::kw_impl;
      if (text == "internal")  return token_kind::kw_internal;
      if (text == "invariant") return token_kind::kw_invariant;
      break;
    case 'l':
      if (text == "let")       return token_kind::kw_let;
      break;
    case 'm':
      if (text == "match")     return token_kind::kw_match;
      if (text == "module")    return token_kind::kw_module;
      if (text == "mut")       return token_kind::kw_mut;
      if (text == "move")      return token_kind::kw_move;
      if (text == "machine")   return token_kind::kw_machine;
      break;
    case 'n':
      if (text == "not")       return token_kind::kw_not;
      if (text == "no_prelude") return token_kind::kw_no_prelude;
      break;
    case 'o':
      if (text == "or")        return token_kind::kw_or;
      if (text == "ok")        return token_kind::kw_ok;
      if (text == "on")        return token_kind::kw_on;
      break;
    case 'p':
      if (text == "pub")       return token_kind::kw_pub;
      if (text == "pure")      return token_kind::kw_pure;
      if (text == "par")       return token_kind::kw_par;
      if (text == "pre")       return token_kind::kw_pre;
      if (text == "post")      return token_kind::kw_post;
      if (text == "priv")      return token_kind::kw_priv;
      break;
    case 'r':
      if (text == "return")    return token_kind::kw_return;
      if (text == "race")      return token_kind::kw_race;
      if (text == "requires")  return token_kind::kw_requires;
      break;
    case 's':
      if (text == "static")    return token_kind::kw_static;
      if (text == "some")      return token_kind::kw_some;
      if (text == "super")     return token_kind::kw_super;
      if (text == "shared")    return token_kind::kw_shared;
      if (text == "stmt")      return token_kind::kw_stmt;
      break;
    case 't':
      if (text == "type")      return token_kind::kw_type;
      if (text == "trait")     return token_kind::kw_trait;
      if (text == "true")      return token_kind::kw_true;
      if (text == "type_expr") return token_kind::kw_type_expr;
      break;
    case 'u':
      if (text == "use")       return token_kind::kw_use;
      if (text == "unit")      return token_kind::kw_unit;
      break;
    case 'v':
      if (text == "var")       return token_kind::kw_var;
      break;
    case 'w':
      if (text == "while")     return token_kind::kw_while;
      if (text == "where")     return token_kind::kw_where;
      break;
    case 'y':
      if (text == "yield")     return token_kind::kw_yield;
      break;
    default:
      break;
  }

  // `_` is a special keyword (wildcard pattern) but only when it's exactly
  // a lone underscore. Identifiers like `_foo` are normal identifiers.
  if (text == "_") return token_kind::kw_underscore;

  return token_kind::ident;
}

// ==========================================================================
//  token_kind_name — returns a human-readable name for a token_kind.
//
//  This is used in diagnostic messages. The names are chosen to be
//  understandable to someone learning Kira — not compiler-internals jargon.
// ==========================================================================
[[nodiscard]] constexpr std::string_view token_kind_name(token_kind kind) noexcept {
  switch (kind) {
    case token_kind::eof:              return "end of file";
    case token_kind::error:            return "error";
    case token_kind::newline:          return "newline";
    case token_kind::indent:           return "indent";
    case token_kind::dedent:           return "dedent";
    case token_kind::placeholder:      return "<placeholder>";

    case token_kind::ident:            return "identifier";
    case token_kind::int_lit:          return "integer literal";
    case token_kind::float_lit:        return "float literal";
    case token_kind::string_lit:       return "string literal";
    case token_kind::char_lit:         return "character literal";
    case token_kind::asm_content:      return "assembly content";

    case token_kind::kw_module:        return "`module`";
    case token_kind::kw_type:          return "`type`";
    case token_kind::kw_trait:         return "`trait`";
    case token_kind::kw_impl:          return "`impl`";
    case token_kind::kw_concept:       return "`concept`";
    case token_kind::kw_def:           return "`def`";
    case token_kind::kw_let:           return "`let`";
    case token_kind::kw_var:           return "`var`";
    case token_kind::kw_static:        return "`static`";
    case token_kind::kw_use:           return "`use`";
    case token_kind::kw_dep:           return "`dep`";

    case token_kind::kw_pure:          return "`pure`";
    case token_kind::kw_async:         return "`async`";
    case token_kind::kw_machine:       return "`machine`";

    case token_kind::kw_pub:           return "`pub`";
    case token_kind::kw_internal:      return "`internal`";
    case token_kind::kw_super:         return "`super`";
    case token_kind::kw_priv:          return "`priv`";

    case token_kind::kw_if:            return "`if`";
    case token_kind::kw_elif:          return "`elif`";
    case token_kind::kw_else:          return "`else`";
    case token_kind::kw_for:           return "`for`";
    case token_kind::kw_while:         return "`while`";
    case token_kind::kw_match:         return "`match`";
    case token_kind::kw_return:        return "`return`";
    case token_kind::kw_in:            return "`in`";
    case token_kind::kw_as:            return "`as`";

    case token_kind::kw_underscore:    return "`_`";
    case token_kind::kw_some:          return "`some`";
    case token_kind::kw_ok:            return "`ok`";
    case token_kind::kw_err:           return "`err`";

    case token_kind::kw_and:           return "`and`";
    case token_kind::kw_or:            return "`or`";
    case token_kind::kw_not:           return "`not`";
    case token_kind::kw_await:         return "`await`";
    case token_kind::kw_yield:         return "`yield`";
    case token_kind::kw_par:           return "`par`";
    case token_kind::kw_race:          return "`race`";
    case token_kind::kw_on:            return "`on`";
    case token_kind::kw_crew:          return "`crew`";
    case token_kind::kw_asm:           return "`asm`";

    case token_kind::kw_pre:           return "`pre`";
    case token_kind::kw_post:          return "`post`";
    case token_kind::kw_invariant:     return "`invariant`";

    case token_kind::kw_where:         return "`where`";
    case token_kind::kw_requires:      return "`requires`";
    case token_kind::kw_deriving:      return "`deriving`";
    case token_kind::kw_no_prelude:    return "`no_prelude`";

    case token_kind::kw_move:          return "`move`";
    case token_kind::kw_shared:        return "`shared`";
    case token_kind::kw_mut:           return "`mut`";

    case token_kind::kw_true:          return "`true`";
    case token_kind::kw_false:         return "`false`";
    case token_kind::kw_unit:          return "`unit`";

    case token_kind::kw_array:         return "`array`";
    case token_kind::kw_fn:            return "`fn`";
    case token_kind::kw_expr:          return "`expr`";
    case token_kind::kw_stmt:          return "`stmt`";
    case token_kind::kw_def_expr:      return "`def_expr`";
    case token_kind::kw_type_expr:     return "`type_expr`";
    case token_kind::kw_assert:        return "`assert`";

    case token_kind::lparen:           return "`(`";
    case token_kind::rparen:           return "`)`";
    case token_kind::lbracket:         return "`[`";
    case token_kind::rbracket:         return "`]`";
    case token_kind::lbrace:           return "`{`";
    case token_kind::rbrace:           return "`}`";
    case token_kind::colon:            return "`:`";
    case token_kind::comma:            return "`,`";
    case token_kind::dot:              return "`.`";
    case token_kind::semicolon:        return "`;`";
    case token_kind::hash:             return "`#`";
    case token_kind::at:               return "`@`";
    case token_kind::backtick:         return "`` ` ``";
    case token_kind::question:         return "`?`";

    case token_kind::eq:               return "`=`";
    case token_kind::plus:             return "`+`";
    case token_kind::minus:            return "`-`";
    case token_kind::star:             return "`*`";
    case token_kind::slash:            return "`/`";
    case token_kind::percent:          return "`%`";
    case token_kind::amp:              return "`&`";
    case token_kind::pipe:             return "`|`";
    case token_kind::caret:            return "`^`";
    case token_kind::tilde:            return "`~`";
    case token_kind::lt:               return "`<`";
    case token_kind::gt:               return "`>`";

    case token_kind::eq_eq:            return "`==`";
    case token_kind::bang_eq:          return "`!=`";
    case token_kind::lt_eq:            return "`<=`";
    case token_kind::gt_eq:            return "`>=`";
    case token_kind::arrow:            return "`->`";
    case token_kind::fat_arrow:        return "`=>`";
    case token_kind::dot_dot:          return "`..`";
    case token_kind::dot_dot_eq:       return "`..=`";
    case token_kind::lt_lt:            return "`<<`";
    case token_kind::gt_gt:            return "`>>`";

    case token_kind::plus_eq:          return "`+=`";
    case token_kind::minus_eq:         return "`-=`";
    case token_kind::star_eq:          return "`*=`";
    case token_kind::slash_eq:         return "`/=`";
    case token_kind::percent_eq:       return "`%=`";
    case token_kind::amp_eq:           return "`&=`";
    case token_kind::pipe_eq:          return "`|=`";
    case token_kind::caret_eq:         return "`^=`";
    case token_kind::lt_lt_eq:         return "`<<=`";
    case token_kind::gt_gt_eq:         return "`>>=`";

    case token_kind::plus_percent:     return "`+%`";
    case token_kind::minus_percent:    return "`-%`";
    case token_kind::star_percent:     return "`*%`";
    case token_kind::plus_percent_eq:  return "`+%=`";
    case token_kind::minus_percent_eq: return "`-%=`";
    case token_kind::star_percent_eq:  return "`*%=`";

    case token_kind::plus_pipe:        return "`+|`";
    case token_kind::minus_pipe:       return "`-|`";
    case token_kind::star_pipe:        return "`*|`";
    case token_kind::plus_pipe_eq:     return "`+|=`";
    case token_kind::minus_pipe_eq:    return "`-|=`";
    case token_kind::star_pipe_eq:     return "`*|=`";

    case token_kind::_count:           return "<invalid>";
  }
  return "<unknown>";
}

/// Returns a short, user-friendly description of what token was expected.
/// For punctuation and keywords, returns the literal text. For categories
/// like Ident, returns a prose description.
[[nodiscard]] constexpr std::string_view token_kind_description(token_kind kind) noexcept {
  switch (kind) {
    case token_kind::ident:     return "a name";
    case token_kind::int_lit:   return "a number";
    case token_kind::float_lit: return "a number";
    case token_kind::string_lit: return "a string";
    case token_kind::char_lit:  return "a character";
    case token_kind::newline:   return "end of line";
    case token_kind::indent:    return "an indented block";
    case token_kind::dedent:    return "end of block";
    case token_kind::eof:       return "end of file";
    default:                   return token_kind_name(kind);
  }
}

}  // namespace kira