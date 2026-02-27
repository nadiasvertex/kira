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
  Eof = 0,       ///< End of file
  Error,         ///< Lexer error token — carries an error message
  Newline,       ///< Logical newline (suppressed inside brackets)
  Indent,        ///< Increase in indentation level
  Dedent,        ///< Decrease in indentation level
  Placeholder,   ///< Synthesized by error recovery — not real source

  // ------------------------------------------------------------------
  //  Identifiers and literals
  // ------------------------------------------------------------------
  Ident,         ///< User identifier
  IntLit,        ///< Integer literal (decimal, hex, octal, binary)
  FloatLit,      ///< Floating-point literal
  StringLit,     ///< String literal (including interpolations)
  CharLit,       ///< Character literal
  AsmContent,    ///< Raw content of an asm{} block

  // ------------------------------------------------------------------
  //  Declaration keywords
  // ------------------------------------------------------------------
  KwModule,      ///< `module`
  KwType,        ///< `type`
  KwTrait,       ///< `trait`
  KwImpl,        ///< `impl`
  KwConcept,     ///< `concept`
  KwDef,         ///< `def`
  KwLet,         ///< `let`
  KwVar,         ///< `var`
  KwStatic,      ///< `static`
  KwUse,         ///< `use`
  KwDep,         ///< `dep`

  // ------------------------------------------------------------------
  //  Function modifiers
  // ------------------------------------------------------------------
  KwPure,        ///< `pure`
  KwAsync,       ///< `async`
  KwMachine,     ///< `machine`

  // ------------------------------------------------------------------
  //  Visibility keywords
  // ------------------------------------------------------------------
  KwPub,         ///< `pub`
  KwInternal,    ///< `internal`
  KwSuper,       ///< `super`
  KwPriv,        ///< `priv`

  // ------------------------------------------------------------------
  //  Control flow keywords
  // ------------------------------------------------------------------
  KwIf,          ///< `if`
  KwElif,        ///< `elif`
  KwElse,        ///< `else`
  KwFor,         ///< `for`
  KwWhile,       ///< `while`
  KwMatch,       ///< `match`
  KwReturn,      ///< `return`
  KwIn,          ///< `in`
  KwAs,          ///< `as`

  // ------------------------------------------------------------------
  //  Pattern keywords
  // ------------------------------------------------------------------
  KwUnderscore,  ///< `_` (wildcard pattern)
  KwSome,        ///< `some`
  KwOk,          ///< `ok`
  KwErr,         ///< `err`

  // ------------------------------------------------------------------
  //  Expression keywords
  // ------------------------------------------------------------------
  KwAnd,         ///< `and`
  KwOr,          ///< `or`
  KwNot,         ///< `not`
  KwAwait,       ///< `await`
  KwYield,       ///< `yield`
  KwPar,         ///< `par`
  KwRace,        ///< `race`
  KwOn,          ///< `on`
  KwCrew,        ///< `crew`
  KwAsm,         ///< `asm`

  // ------------------------------------------------------------------
  //  Contract keywords
  // ------------------------------------------------------------------
  KwPre,         ///< `pre`
  KwPost,        ///< `post`
  KwInvariant,   ///< `invariant`

  // ------------------------------------------------------------------
  //  Compile-time keywords
  // ------------------------------------------------------------------
  KwWhere,       ///< `where`
  KwRequires,    ///< `requires`
  KwDeriving,    ///< `deriving`
  KwNoPrelude,   ///< `no_prelude`

  // ------------------------------------------------------------------
  //  Capture / ownership keywords
  // ------------------------------------------------------------------
  KwMove,        ///< `move`
  KwShared,      ///< `shared`
  KwMut,         ///< `mut`

  // ------------------------------------------------------------------
  //  Value literal keywords
  // ------------------------------------------------------------------
  KwTrue,        ///< `true`
  KwFalse,       ///< `false`
  KwUnit,        ///< `unit`

  // ------------------------------------------------------------------
  //  Type-related keywords
  // ------------------------------------------------------------------
  KwArray,       ///< `array`
  KwFn,          ///< `fn`
  KwExpr,        ///< `expr` (quote type)
  KwStmt,        ///< `stmt` (quote type)
  KwDefExpr,     ///< `def_expr` (quote type)
  KwTypeExpr,    ///< `type_expr` (quote type)
  KwAssert,      ///< `assert` (used in static assert)

  // ------------------------------------------------------------------
  //  Punctuation — single character
  // ------------------------------------------------------------------
  LParen,        ///< `(`
  RParen,        ///< `)`
  LBracket,      ///< `[`
  RBracket,      ///< `]`
  LBrace,        ///< `{`
  RBrace,        ///< `}`
  Colon,         ///< `:`
  Comma,         ///< `,`
  Dot,           ///< `.`
  Semicolon,     ///< `;`
  Hash,          ///< `#`
  At,            ///< `@`
  Backtick,      ///< `` ` ``
  Question,      ///< `?`

  // ------------------------------------------------------------------
  //  Operators — single character
  // ------------------------------------------------------------------
  Eq,            ///< `=`
  Plus,          ///< `+`
  Minus,         ///< `-`
  Star,          ///< `*`
  Slash,         ///< `/`
  Percent,       ///< `%`
  Amp,           ///< `&`
  Pipe,          ///< `|`
  Caret,         ///< `^`
  Tilde,         ///< `~`
  Lt,            ///< `<`
  Gt,            ///< `>`

  // ------------------------------------------------------------------
  //  Operators — multi-character
  // ------------------------------------------------------------------
  EqEq,          ///< `==`
  BangEq,        ///< `!=`
  LtEq,          ///< `<=`
  GtEq,          ///< `>=`
  Arrow,         ///< `->`
  FatArrow,      ///< `=>`
  DotDot,        ///< `..`
  DotDotEq,      ///< `..=`
  LtLt,          ///< `<<`
  GtGt,          ///< `>>`

  // ------------------------------------------------------------------
  //  Compound assignment operators
  // ------------------------------------------------------------------
  PlusEq,        ///< `+=`
  MinusEq,       ///< `-=`
  StarEq,        ///< `*=`
  SlashEq,       ///< `/=`
  PercentEq,     ///< `%=`
  AmpEq,         ///< `&=`
  PipeEq,        ///< `|=`
  CaretEq,       ///< `^=`
  LtLtEq,        ///< `<<=`
  GtGtEq,        ///< `>>=`

  // ------------------------------------------------------------------
  //  Wrapping arithmetic operators (machine mode)
  // ------------------------------------------------------------------
  PlusPercent,   ///< `+%`  (wrapping add)
  MinusPercent,  ///< `-%`  (wrapping sub)
  StarPercent,   ///< `*%`  (wrapping mul)
  PlusPercentEq, ///< `+%=` (wrapping add assign)
  MinusPercentEq,///< `-%=` (wrapping sub assign)
  StarPercentEq, ///< `*%=` (wrapping mul assign)

  // ------------------------------------------------------------------
  //  Saturating arithmetic operators (machine mode)
  // ------------------------------------------------------------------
  PlusPipe,      ///< `+|`  (saturating add)
  MinusPipe,     ///< `-|`  (saturating sub)
  StarPipe,      ///< `*|`  (saturating mul)
  PlusPipeEq,    ///< `+|=` (saturating add assign)
  MinusPipeEq,   ///< `-|=` (saturating sub assign)
  StarPipeEq,    ///< `*|=` (saturating mul assign)

  // Keep this last — used for iteration bounds.
  _Count,
};

// ==========================================================================
//  Token — a single lexical token with its kind, location, and text.
// ==========================================================================
struct Token {
  TokenKind kind = TokenKind::Eof;
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
    return kind == TokenKind::Eof;
  }

  [[nodiscard]] constexpr bool is_error() const noexcept {
    return kind == TokenKind::Error;
  }

  [[nodiscard]] constexpr bool is_newline() const noexcept {
    return kind == TokenKind::Newline;
  }

  [[nodiscard]] constexpr bool is_keyword() const noexcept {
    return kind >= TokenKind::KwModule && kind <= TokenKind::KwAssert;
  }

  [[nodiscard]] constexpr bool is_literal() const noexcept {
    return kind >= TokenKind::IntLit && kind <= TokenKind::CharLit;
  }

  [[nodiscard]] constexpr bool is_literal_keyword() const noexcept {
    return kind == TokenKind::KwTrue ||
           kind == TokenKind::KwFalse ||
           kind == TokenKind::KwUnit;
  }

  [[nodiscard]] constexpr bool is_visibility() const noexcept {
    return kind == TokenKind::KwPub ||
           kind == TokenKind::KwInternal ||
           kind == TokenKind::KwSuper ||
           kind == TokenKind::KwPriv;
  }

  [[nodiscard]] constexpr bool is_func_modifier() const noexcept {
    return kind == TokenKind::KwPure ||
           kind == TokenKind::KwAsync ||
           kind == TokenKind::KwMachine ||
           kind == TokenKind::KwStatic;
  }

  [[nodiscard]] constexpr bool is_assign_op() const noexcept {
    return kind == TokenKind::Eq ||
           kind == TokenKind::PlusEq ||
           kind == TokenKind::MinusEq ||
           kind == TokenKind::StarEq ||
           kind == TokenKind::SlashEq ||
           kind == TokenKind::PercentEq ||
           kind == TokenKind::AmpEq ||
           kind == TokenKind::PipeEq ||
           kind == TokenKind::CaretEq ||
           kind == TokenKind::LtLtEq ||
           kind == TokenKind::GtGtEq ||
           kind == TokenKind::PlusPercentEq ||
           kind == TokenKind::MinusPercentEq ||
           kind == TokenKind::StarPercentEq ||
           kind == TokenKind::PlusPipeEq ||
           kind == TokenKind::MinusPipeEq ||
           kind == TokenKind::StarPipeEq;
  }

  [[nodiscard]] constexpr bool is_cmp_op() const noexcept {
    return kind == TokenKind::EqEq ||
           kind == TokenKind::BangEq ||
           kind == TokenKind::Lt ||
           kind == TokenKind::LtEq ||
           kind == TokenKind::Gt ||
           kind == TokenKind::GtEq ||
           kind == TokenKind::KwIn ||
           kind == TokenKind::KwNot;  // `not in`
  }

  [[nodiscard]] constexpr bool is_add_op() const noexcept {
    return kind == TokenKind::Plus ||
           kind == TokenKind::Minus ||
           kind == TokenKind::PlusPercent ||
           kind == TokenKind::MinusPercent ||
           kind == TokenKind::PlusPipe ||
           kind == TokenKind::MinusPipe;
  }

  [[nodiscard]] constexpr bool is_mul_op() const noexcept {
    return kind == TokenKind::Star ||
           kind == TokenKind::Slash ||
           kind == TokenKind::Percent ||
           kind == TokenKind::StarPercent ||
           kind == TokenKind::StarPipe;
  }

  [[nodiscard]] constexpr bool is_unary_op() const noexcept {
    return kind == TokenKind::Minus ||
           kind == TokenKind::Tilde ||
           kind == TokenKind::Star ||
           kind == TokenKind::Amp;
  }

  /// Returns true if this token can start an expression.
  [[nodiscard]] constexpr bool can_start_expr() const noexcept {
    switch (kind) {
      case TokenKind::Ident:
      case TokenKind::IntLit:
      case TokenKind::FloatLit:
      case TokenKind::StringLit:
      case TokenKind::CharLit:
      case TokenKind::KwTrue:
      case TokenKind::KwFalse:
      case TokenKind::KwUnit:
      case TokenKind::LParen:
      case TokenKind::LBracket:
      case TokenKind::LBrace:
      case TokenKind::Minus:
      case TokenKind::Tilde:
      case TokenKind::Star:
      case TokenKind::Amp:
      case TokenKind::KwNot:
      case TokenKind::KwIf:
      case TokenKind::KwMatch:
      case TokenKind::KwFor:
      case TokenKind::KwAwait:
      case TokenKind::KwPar:
      case TokenKind::KwRace:
      case TokenKind::KwOn:
      case TokenKind::KwPure:
      case TokenKind::KwMove:
      case TokenKind::KwStatic:
      case TokenKind::Backtick:
        return true;
      default:
        return false;
    }
  }

  /// Returns true if this token can start a statement.
  [[nodiscard]] constexpr bool can_start_stmt() const noexcept {
    switch (kind) {
      case TokenKind::KwLet:
      case TokenKind::KwVar:
      case TokenKind::KwReturn:
      case TokenKind::KwIf:
      case TokenKind::KwWhile:
      case TokenKind::KwFor:
      case TokenKind::KwMatch:
      case TokenKind::KwCrew:
      case TokenKind::KwAsm:
      case TokenKind::KwUse:
      case TokenKind::KwType:
      case TokenKind::KwDef:
      case TokenKind::KwStatic:
      case TokenKind::KwPub:
      case TokenKind::KwInternal:
      case TokenKind::KwSuper:
      case TokenKind::KwPriv:
      case TokenKind::KwPure:
      case TokenKind::KwAsync:
      case TokenKind::KwMachine:
      case TokenKind::Tilde:
      case TokenKind::Newline:
        return true;
      default:
        return can_start_expr();
    }
  }

  /// Returns true if this token can start a top-level item.
  [[nodiscard]] constexpr bool can_start_top_level() const noexcept {
    switch (kind) {
      case TokenKind::KwUse:
      case TokenKind::KwType:
      case TokenKind::KwTrait:
      case TokenKind::KwImpl:
      case TokenKind::KwConcept:
      case TokenKind::KwDef:
      case TokenKind::KwStatic:
      case TokenKind::KwModule:
      case TokenKind::KwDep:
      case TokenKind::KwPub:
      case TokenKind::KwInternal:
      case TokenKind::KwSuper:
      case TokenKind::KwPriv:
      case TokenKind::KwPure:
      case TokenKind::KwAsync:
      case TokenKind::KwMachine:
      case TokenKind::Tilde:
      case TokenKind::Newline:
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
  if (text.empty()) return TokenKind::Ident;

  switch (text[0]) {
    case 'a':
      if (text == "and")       return TokenKind::KwAnd;
      if (text == "as")        return TokenKind::KwAs;
      if (text == "async")     return TokenKind::KwAsync;
      if (text == "await")     return TokenKind::KwAwait;
      if (text == "asm")       return TokenKind::KwAsm;
      if (text == "array")     return TokenKind::KwArray;
      if (text == "assert")    return TokenKind::KwAssert;
      break;
    case 'c':
      if (text == "concept")   return TokenKind::KwConcept;
      if (text == "crew")      return TokenKind::KwCrew;
      break;
    case 'd':
      if (text == "def")       return TokenKind::KwDef;
      if (text == "dep")       return TokenKind::KwDep;
      if (text == "deriving")  return TokenKind::KwDeriving;
      if (text == "def_expr")  return TokenKind::KwDefExpr;
      break;
    case 'e':
      if (text == "else")      return TokenKind::KwElse;
      if (text == "elif")      return TokenKind::KwElif;
      if (text == "err")       return TokenKind::KwErr;
      if (text == "expr")      return TokenKind::KwExpr;
      break;
    case 'f':
      if (text == "for")       return TokenKind::KwFor;
      if (text == "false")     return TokenKind::KwFalse;
      if (text == "fn")        return TokenKind::KwFn;
      break;
    case 'i':
      if (text == "if")        return TokenKind::KwIf;
      if (text == "in")        return TokenKind::KwIn;
      if (text == "impl")      return TokenKind::KwImpl;
      if (text == "internal")  return TokenKind::KwInternal;
      if (text == "invariant") return TokenKind::KwInvariant;
      break;
    case 'l':
      if (text == "let")       return TokenKind::KwLet;
      break;
    case 'm':
      if (text == "match")     return TokenKind::KwMatch;
      if (text == "module")    return TokenKind::KwModule;
      if (text == "mut")       return TokenKind::KwMut;
      if (text == "move")      return TokenKind::KwMove;
      if (text == "machine")   return TokenKind::KwMachine;
      break;
    case 'n':
      if (text == "not")       return TokenKind::KwNot;
      if (text == "no_prelude") return TokenKind::KwNoPrelude;
      break;
    case 'o':
      if (text == "or")        return TokenKind::KwOr;
      if (text == "ok")        return TokenKind::KwOk;
      if (text == "on")        return TokenKind::KwOn;
      break;
    case 'p':
      if (text == "pub")       return TokenKind::KwPub;
      if (text == "pure")      return TokenKind::KwPure;
      if (text == "par")       return TokenKind::KwPar;
      if (text == "pre")       return TokenKind::KwPre;
      if (text == "post")      return TokenKind::KwPost;
      if (text == "priv")      return TokenKind::KwPriv;
      break;
    case 'r':
      if (text == "return")    return TokenKind::KwReturn;
      if (text == "race")      return TokenKind::KwRace;
      if (text == "requires")  return TokenKind::KwRequires;
      break;
    case 's':
      if (text == "static")    return TokenKind::KwStatic;
      if (text == "some")      return TokenKind::KwSome;
      if (text == "super")     return TokenKind::KwSuper;
      if (text == "shared")    return TokenKind::KwShared;
      if (text == "stmt")      return TokenKind::KwStmt;
      break;
    case 't':
      if (text == "type")      return TokenKind::KwType;
      if (text == "trait")     return TokenKind::KwTrait;
      if (text == "true")      return TokenKind::KwTrue;
      if (text == "type_expr") return TokenKind::KwTypeExpr;
      break;
    case 'u':
      if (text == "use")       return TokenKind::KwUse;
      if (text == "unit")      return TokenKind::KwUnit;
      break;
    case 'v':
      if (text == "var")       return TokenKind::KwVar;
      break;
    case 'w':
      if (text == "while")     return TokenKind::KwWhile;
      if (text == "where")     return TokenKind::KwWhere;
      break;
    case 'y':
      if (text == "yield")     return TokenKind::KwYield;
      break;
    default:
      break;
  }

  // `_` is a special keyword (wildcard pattern) but only when it's exactly
  // a lone underscore. Identifiers like `_foo` are normal identifiers.
  if (text == "_") return TokenKind::KwUnderscore;

  return TokenKind::Ident;
}

// ==========================================================================
//  token_kind_name — returns a human-readable name for a TokenKind.
//
//  This is used in diagnostic messages. The names are chosen to be
//  understandable to someone learning Kira — not compiler-internals jargon.
// ==========================================================================
[[nodiscard]] constexpr std::string_view token_kind_name(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::Eof:              return "end of file";
    case TokenKind::Error:            return "error";
    case TokenKind::Newline:          return "newline";
    case TokenKind::Indent:           return "indent";
    case TokenKind::Dedent:           return "dedent";
    case TokenKind::Placeholder:      return "<placeholder>";

    case TokenKind::Ident:            return "identifier";
    case TokenKind::IntLit:           return "integer literal";
    case TokenKind::FloatLit:         return "float literal";
    case TokenKind::StringLit:        return "string literal";
    case TokenKind::CharLit:          return "character literal";
    case TokenKind::AsmContent:       return "assembly content";

    case TokenKind::KwModule:         return "`module`";
    case TokenKind::KwType:           return "`type`";
    case TokenKind::KwTrait:          return "`trait`";
    case TokenKind::KwImpl:           return "`impl`";
    case TokenKind::KwConcept:        return "`concept`";
    case TokenKind::KwDef:            return "`def`";
    case TokenKind::KwLet:            return "`let`";
    case TokenKind::KwVar:            return "`var`";
    case TokenKind::KwStatic:         return "`static`";
    case TokenKind::KwUse:            return "`use`";
    case TokenKind::KwDep:            return "`dep`";

    case TokenKind::KwPure:           return "`pure`";
    case TokenKind::KwAsync:          return "`async`";
    case TokenKind::KwMachine:        return "`machine`";

    case TokenKind::KwPub:            return "`pub`";
    case TokenKind::KwInternal:       return "`internal`";
    case TokenKind::KwSuper:          return "`super`";
    case TokenKind::KwPriv:           return "`priv`";

    case TokenKind::KwIf:             return "`if`";
    case TokenKind::KwElif:           return "`elif`";
    case TokenKind::KwElse:           return "`else`";
    case TokenKind::KwFor:            return "`for`";
    case TokenKind::KwWhile:          return "`while`";
    case TokenKind::KwMatch:          return "`match`";
    case TokenKind::KwReturn:         return "`return`";
    case TokenKind::KwIn:             return "`in`";
    case TokenKind::KwAs:             return "`as`";

    case TokenKind::KwUnderscore:     return "`_`";
    case TokenKind::KwSome:           return "`some`";
    case TokenKind::KwOk:             return "`ok`";
    case TokenKind::KwErr:            return "`err`";

    case TokenKind::KwAnd:            return "`and`";
    case TokenKind::KwOr:             return "`or`";
    case TokenKind::KwNot:            return "`not`";
    case TokenKind::KwAwait:          return "`await`";
    case TokenKind::KwYield:          return "`yield`";
    case TokenKind::KwPar:            return "`par`";
    case TokenKind::KwRace:           return "`race`";
    case TokenKind::KwOn:             return "`on`";
    case TokenKind::KwCrew:           return "`crew`";
    case TokenKind::KwAsm:            return "`asm`";

    case TokenKind::KwPre:            return "`pre`";
    case TokenKind::KwPost:           return "`post`";
    case TokenKind::KwInvariant:      return "`invariant`";

    case TokenKind::KwWhere:          return "`where`";
    case TokenKind::KwRequires:       return "`requires`";
    case TokenKind::KwDeriving:       return "`deriving`";
    case TokenKind::KwNoPrelude:      return "`no_prelude`";

    case TokenKind::KwMove:           return "`move`";
    case TokenKind::KwShared:         return "`shared`";
    case TokenKind::KwMut:            return "`mut`";

    case TokenKind::KwTrue:           return "`true`";
    case TokenKind::KwFalse:          return "`false`";
    case TokenKind::KwUnit:           return "`unit`";

    case TokenKind::KwArray:          return "`array`";
    case TokenKind::KwFn:             return "`fn`";
    case TokenKind::KwExpr:           return "`expr`";
    case TokenKind::KwStmt:           return "`stmt`";
    case TokenKind::KwDefExpr:        return "`def_expr`";
    case TokenKind::KwTypeExpr:       return "`type_expr`";
    case TokenKind::KwAssert:         return "`assert`";

    case TokenKind::LParen:           return "`(`";
    case TokenKind::RParen:           return "`)`";
    case TokenKind::LBracket:         return "`[`";
    case TokenKind::RBracket:         return "`]`";
    case TokenKind::LBrace:           return "`{`";
    case TokenKind::RBrace:           return "`}`";
    case TokenKind::Colon:            return "`:`";
    case TokenKind::Comma:            return "`,`";
    case TokenKind::Dot:              return "`.`";
    case TokenKind::Semicolon:        return "`;`";
    case TokenKind::Hash:             return "`#`";
    case TokenKind::At:               return "`@`";
    case TokenKind::Backtick:         return "`` ` ``";
    case TokenKind::Question:         return "`?`";

    case TokenKind::Eq:               return "`=`";
    case TokenKind::Plus:             return "`+`";
    case TokenKind::Minus:            return "`-`";
    case TokenKind::Star:             return "`*`";
    case TokenKind::Slash:            return "`/`";
    case TokenKind::Percent:          return "`%`";
    case TokenKind::Amp:              return "`&`";
    case TokenKind::Pipe:             return "`|`";
    case TokenKind::Caret:            return "`^`";
    case TokenKind::Tilde:            return "`~`";
    case TokenKind::Lt:               return "`<`";
    case TokenKind::Gt:               return "`>`";

    case TokenKind::EqEq:             return "`==`";
    case TokenKind::BangEq:           return "`!=`";
    case TokenKind::LtEq:             return "`<=`";
    case TokenKind::GtEq:             return "`>=`";
    case TokenKind::Arrow:            return "`->`";
    case TokenKind::FatArrow:         return "`=>`";
    case TokenKind::DotDot:           return "`..`";
    case TokenKind::DotDotEq:         return "`..=`";
    case TokenKind::LtLt:             return "`<<`";
    case TokenKind::GtGt:             return "`>>`";

    case TokenKind::PlusEq:           return "`+=`";
    case TokenKind::MinusEq:          return "`-=`";
    case TokenKind::StarEq:           return "`*=`";
    case TokenKind::SlashEq:          return "`/=`";
    case TokenKind::PercentEq:        return "`%=`";
    case TokenKind::AmpEq:            return "`&=`";
    case TokenKind::PipeEq:           return "`|=`";
    case TokenKind::CaretEq:          return "`^=`";
    case TokenKind::LtLtEq:           return "`<<=`";
    case TokenKind::GtGtEq:           return "`>>=`";

    case TokenKind::PlusPercent:      return "`+%`";
    case TokenKind::MinusPercent:     return "`-%`";
    case TokenKind::StarPercent:      return "`*%`";
    case TokenKind::PlusPercentEq:    return "`+%=`";
    case TokenKind::MinusPercentEq:   return "`-%=`";
    case TokenKind::StarPercentEq:    return "`*%=`";

    case TokenKind::PlusPipe:         return "`+|`";
    case TokenKind::MinusPipe:        return "`-|`";
    case TokenKind::StarPipe:         return "`*|`";
    case TokenKind::PlusPipeEq:       return "`+|=`";
    case TokenKind::MinusPipeEq:      return "`-|=`";
    case TokenKind::StarPipeEq:       return "`*|=`";

    case TokenKind::_Count:           return "<invalid>";
  }
  return "<unknown>";
}

/// Returns a short, user-friendly description of what token was expected.
/// For punctuation and keywords, returns the literal text. For categories
/// like Ident, returns a prose description.
[[nodiscard]] constexpr std::string_view token_kind_description(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::Ident:     return "a name";
    case TokenKind::IntLit:    return "a number";
    case TokenKind::FloatLit:  return "a number";
    case TokenKind::StringLit: return "a string";
    case TokenKind::CharLit:   return "a character";
    case TokenKind::Newline:   return "end of line";
    case TokenKind::Indent:    return "an indented block";
    case TokenKind::Dedent:    return "end of block";
    case TokenKind::Eof:       return "end of file";
    default:                   return token_kind_name(kind);
  }
}

}  // namespace kira