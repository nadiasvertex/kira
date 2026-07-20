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
enum class token_kind : uint8_t {
  // ------------------------------------------------------------------
  //  Sentinel / special
  // ------------------------------------------------------------------
  eof = 0, ///< Explicit stream terminator every parser entry point can rely on.
  error, ///< Carries lexing failure text so parsing can continue with context.
  newline, ///< Logical line break used as a statement boundary when
           ///< significant.
  indent,  ///< Synthetic token marking entry into an indentation-defined block.
  dedent,  ///< Synthetic token marking exit from an indentation-defined block.
  placeholder, ///< Synthetic stand-in inserted by recovery, never by the source
               ///< text.
  doc_comment, ///< Documentation comment (`#:` line) whose `text` is the
               ///< trimmed body; attaches to the following declaration.

  // ------------------------------------------------------------------
  //  Identifiers and literals
  // ------------------------------------------------------------------
  ident,     ///< User-defined name that later phases must resolve semantically.
  int_lit,   ///< Integer literal text preserved for later base/suffix
             ///< interpretation.
  float_lit, ///< Floating-point literal text preserved for later numeric
             ///< validation.
  string_lit,  ///< Full string token, including interpolation syntax for later
               ///< splitting.
  char_lit,    ///< Character literal text for later escape and scalar-value
               ///< checking.
  asm_content, ///< Raw assembly payload forwarded mostly opaque beyond parsing.

  // ------------------------------------------------------------------
  //  Declaration keywords
  // ------------------------------------------------------------------
  kw_module,    ///< Starts module declarations and nested module items.
  kw_type,      ///< Introduces type declarations and associated type items.
  kw_trait,     ///< Introduces trait interfaces for later conformance checking.
  kw_signature, ///< Introduces a module signature (module-level concept).
  kw_impl,      ///< Introduces implementation blocks tying behavior to types.
  kw_extend, ///< Introduces extension-method blocks adding methods to any type.
  kw_concept, ///< Introduces compile-time concept constraints.
  kw_def,     ///< Reserved declaration introducer kept visible to the grammar.
  kw_let,     ///< Starts immutable binding statements and `let` conditions.
  kw_var,     ///< Starts mutable variable bindings.
  kw_static,  ///< Marks compile-time declarations or modifiers with
              ///< phase-sensitive meaning.
  kw_use,     ///< Introduces import declarations.
  kw_dep,     ///< Introduces dependency metadata declarations.

  // ------------------------------------------------------------------
  //  Function modifiers
  // ------------------------------------------------------------------
  kw_pure,    ///< Function or lambda modifier constraining observable effects.
  kw_async,   ///< Function or expression modifier introducing async execution.
  kw_machine, ///< Function modifier opting into machine-level arithmetic
              ///< semantics.
  kw_intrinsic, ///< Function modifier marking a signature-only declaration
                ///< backed by a native implementation per backend.
  kw_generator, ///< Function modifier compiling the body into a suspendable
                ///< coroutine satisfying `iterator[T]` via `yield`.

  // ------------------------------------------------------------------
  //  Type-declaration modifiers
  // ------------------------------------------------------------------
  kw_packed, ///< Struct-only `type` modifier: fields byte-packed back to
             ///< back, no alignment padding.

  // ------------------------------------------------------------------
  //  Visibility keywords
  // ------------------------------------------------------------------
  kw_pub,      ///< Widest visibility for exported declarations.
  kw_internal, ///< Visibility scoped to the current package or compilation
               ///< unit.
  kw_super,    ///< Visibility scoped to the parent module boundary.
  kw_priv,     ///< Narrowest explicit visibility.

  // ------------------------------------------------------------------
  //  Control flow keywords
  // ------------------------------------------------------------------
  kw_if,    ///< Starts conditional statements, expressions, and guards.
  kw_elif,  ///< Continues a prior conditional chain without closing the
            ///< construct.
  kw_else,  ///< Provides fallback control-flow or binding bodies.
  kw_for,   ///< Starts iteration statements and comprehension-like expressions.
  kw_while, ///< Starts looping statements.
  kw_match, ///< Starts pattern-dispatch statements and expressions.
  kw_return, ///< Terminates function evaluation with an optional value.
  kw_break,  ///< Exits the innermost enclosing loop.
  kw_continue, ///< Advances the innermost enclosing loop to its next iteration.
  kw_in, ///< Separates iteration patterns from iterables and participates in
         ///< comparisons.
  kw_as, ///< Introduces casts and aliases depending on grammar position.

  // ------------------------------------------------------------------
  //  Pattern keywords
  // ------------------------------------------------------------------
  kw_underscore, ///< Wildcard token kept distinct so parser need not
                 ///< special-case ident text.
  kw_some,       ///< Option-pattern/value constructor keyword.
  kw_ok,         ///< Result success constructor keyword.
  kw_err,        ///< Result failure constructor keyword.

  // ------------------------------------------------------------------
  //  Expression keywords
  // ------------------------------------------------------------------
  kw_and,   ///< Logical conjunction token kept word-based for readability.
  kw_or,    ///< Logical disjunction token kept word-based for readability.
  kw_not,   ///< Logical negation token and part of `not in` comparisons.
  kw_await, ///< Suspends on async computation results or `yield` points.
  kw_yield, ///< Marks yielded values in async/coroutine contexts.
  kw_par,   ///< Starts parallel branch expressions.
  kw_race,  ///< Starts first-completer-wins branch expressions.
  kw_on,    ///< Starts context-bound execution expressions.
  kw_crew,  ///< Starts crew orchestration constructs.
  kw_asm,   ///< Starts raw assembly statement parsing.

  // ------------------------------------------------------------------
  //  Contract keywords
  // ------------------------------------------------------------------
  kw_pre,       ///< Introduces a precondition contract clause.
  kw_post,      ///< Introduces a postcondition contract clause.
  kw_invariant, ///< Introduces a type invariant checked after parsing/analysis.

  // ------------------------------------------------------------------
  //  Compile-time keywords
  // ------------------------------------------------------------------
  kw_where,      ///< Introduces trailing constraints or local binding clauses.
  kw_requires,   ///< Introduces trait or concept requirement clauses.
  kw_deriving,   ///< Introduces derived-behavior requests for declarations.
  kw_no_prelude, ///< File-level opt-out of implicit prelude imports.

  // ------------------------------------------------------------------
  //  Capture / ownership keywords
  // ------------------------------------------------------------------
  kw_move,   ///< Requests capture or transfer by move semantics.
  kw_shared, ///< Marks shared capture or ownership intent.
  kw_mut,    ///< Marks mutable references, bindings, or parameters.

  // ------------------------------------------------------------------
  //  Value literal keywords
  // ------------------------------------------------------------------
  kw_true,  ///< Boolean literal token preserved distinctly from identifiers.
  kw_false, ///< Boolean literal token preserved distinctly from identifiers.
  kw_unit,  ///< Unit literal keyword.

  // ------------------------------------------------------------------
  //  Type-related keywords
  // ------------------------------------------------------------------
  kw_array, ///< Type-level array constructor keyword.
  kw_fn,    ///< Function type introducer.
  // Note: `expr`/`stmt`/`def_expr`/`type_expr` are *not* keywords here —
  // they're contextual: an ordinary identifier everywhere except type
  // position, where `parser::parse_prim_type_expr` recognizes the bare
  // spelling and builds an `ast::quote_type` directly (see
  // `classify_ident`'s comment below for why they're deliberately absent
  // from this keyword table).
  kw_assert, ///< Static assertion introducer after `static`.

  // ------------------------------------------------------------------
  //  Punctuation — single character
  // ------------------------------------------------------------------
  lparen,    ///< Opens grouping, tuples, parameter lists, and calls.
  rparen,    ///< Closes grouping, tuples, parameter lists, and calls.
  lbracket,  ///< Opens indexing, array literals, and generic argument forms.
  rbracket,  ///< Closes indexing, array literals, and generic argument forms.
  lbrace,    ///< Opens blocks, struct literals, and pattern bodies.
  rbrace,    ///< Closes blocks, struct literals, and pattern bodies.
  colon,     ///< Separates heads from bodies and names from annotated values.
  comma,     ///< Separates list elements while preserving trailing-comma style.
  dot,       ///< Joins paths and field accesses.
  semicolon, ///< Explicit separator available where newline sensitivity is
             ///< awkward.
  hash,      ///< Reserved punctuation token for future surface syntax.
  at,        ///< Reserved punctuation token for attributes or annotations.
  backtick,  ///< Delimits quoted syntax regions for macros/metaprogramming.
  question,  ///< Starts try-propagation postfix forms.

  // ------------------------------------------------------------------
  //  Operators — single character
  // ------------------------------------------------------------------
  eq,      ///< Assignment or binding operator depending on parser context.
  plus,    ///< Additive operator or unary sign context marker.
  minus,   ///< Subtractive operator or unary negation marker.
  star,    ///< Multiplication operator or dereference marker.
  slash,   ///< Division operator.
  percent, ///< Remainder operator.
  amp,     ///< Bitwise-and operator or address-of marker.
  pipe,    ///< Pipe or bitwise-or operator depending on precedence layer.
  caret,   ///< Bitwise exclusive-or operator.
  tilde, ///< Bitwise-not operator and splice introducer in statement contexts.
  lt,    ///< Comparison operator or generic-bracket introducer by context.
  gt,    ///< Comparison operator or generic-bracket closer by context.

  // ------------------------------------------------------------------
  //  Operators — multi-character
  // ------------------------------------------------------------------
  eq_eq,      ///< Equality comparison operator.
  bang_eq,    ///< Inequality comparison operator.
  lt_eq,      ///< Less-than-or-equal comparison operator.
  gt_eq,      ///< Greater-than-or-equal comparison operator.
  arrow,      ///< Introduces function return types or arrowed syntax forms.
  fat_arrow,  ///< Separates lambda heads and comprehension yields from bodies.
  dot_dot,    ///< Half-open range operator.
  dot_dot_eq, ///< Inclusive range operator.
  lt_lt,      ///< Left-shift operator.
  gt_gt,      ///< Right-shift operator.

  // ------------------------------------------------------------------
  //  Compound assignment operators
  // ------------------------------------------------------------------
  plus_eq,    ///< Compound add assignment mapped to `assign_op::AddAssign`.
  minus_eq,   ///< Compound subtract assignment.
  star_eq,    ///< Compound multiply assignment.
  slash_eq,   ///< Compound divide assignment.
  percent_eq, ///< Compound remainder assignment.
  amp_eq,     ///< Compound bitwise-and assignment.
  pipe_eq,    ///< Compound bitwise-or assignment.
  caret_eq,   ///< Compound bitwise-xor assignment.
  lt_lt_eq,   ///< Compound left-shift assignment.
  gt_gt_eq,   ///< Compound right-shift assignment.

  // ------------------------------------------------------------------
  //  Wrapping arithmetic operators (machine mode)
  // ------------------------------------------------------------------
  plus_percent,  ///< Machine-mode wrapping add that preserves overflow intent.
  minus_percent, ///< Machine-mode wrapping subtract.
  star_percent,  ///< Machine-mode wrapping multiply.
  plus_percent_eq,  ///< Wrapping add assignment.
  minus_percent_eq, ///< Wrapping subtract assignment.
  star_percent_eq,  ///< Wrapping multiply assignment.

  // ------------------------------------------------------------------
  //  Saturating arithmetic operators (machine mode)
  // ------------------------------------------------------------------
  plus_pipe,     ///< Machine-mode saturating add.
  minus_pipe,    ///< Machine-mode saturating subtract.
  star_pipe,     ///< Machine-mode saturating multiply.
  plus_pipe_eq,  ///< Saturating add assignment.
  minus_pipe_eq, ///< Saturating subtract assignment.
  star_pipe_eq,  ///< Saturating multiply assignment.

  // Keep this last — table walks and iteration bounds depend on it.
  count,
};

// ==========================================================================
//  Token — a single lexical token with its kind, location, and text.
// ==========================================================================
struct token {
  token_kind kind = token_kind::eof; ///< Lexical category chosen by the lexer.
  source_span span; ///< Exact byte range in the originating file.

  /// The raw source text of this token. For most tokens this is a view into
  /// the original source buffer; for synthesized tokens (error recovery) it
  /// may be empty or hold a placeholder string.
  std::string_view text; ///< Borrowed source slice backing exact spelling.

  /// For Error tokens, this carries a short human-readable description of
  /// what went wrong during lexing (e.g., "unterminated string literal").
  /// Empty for all non-error tokens.
  std::string_view
      error_message; ///< Lexer-provided explanation for `error` tokens.

  // Convenience predicates ------------------------------------------------

  /// @brief Returns whether this token has exactly kind `k`.
  [[nodiscard]] constexpr auto is(token_kind k) const noexcept -> bool {
    return kind == k;
  }

  /// @brief Returns whether this token is anything except `k`.
  [[nodiscard]] constexpr auto is_not(token_kind k) const noexcept -> bool {
    return kind != k;
  }

  /// @brief Returns whether this token matches any of the provided kinds.
  ///
  /// @tparam Kinds Token-kind enum parameters to compare against.
  /// @param kinds Candidate token kinds.
  template <typename... Kinds>
  [[nodiscard]] constexpr auto is_one_of(Kinds... kinds) const noexcept
      -> bool {
    return ((kind == kinds) || ...);
  }

  /// @brief Returns whether this is the parser's guaranteed stream terminator.
  [[nodiscard]] constexpr auto is_eof() const noexcept -> bool {
    return kind == token_kind::eof;
  }

  /// @brief Returns whether lexing could not classify this source region.
  [[nodiscard]] constexpr auto is_error() const noexcept -> bool {
    return kind == token_kind::error;
  }

  /// @brief Returns whether this token ends a logical statement line.
  [[nodiscard]] constexpr auto is_newline() const noexcept -> bool {
    return kind == token_kind::newline;
  }

  /// @brief Returns whether this token is one of the reserved word tokens.
  ///
  /// Parser code uses this for grammar decisions before semantic analysis has
  /// resolved an identifier's role.
  [[nodiscard]] constexpr auto is_keyword() const noexcept -> bool {
    return kind >= token_kind::kw_module && kind <= token_kind::kw_assert;
  }

  /// @brief Returns whether this token is a lexical literal token.
  [[nodiscard]] constexpr auto is_literal() const noexcept -> bool {
    return kind >= token_kind::int_lit && kind <= token_kind::char_lit;
  }

  /// @brief Returns whether this token is a keyword that behaves as a literal.
  [[nodiscard]] constexpr auto is_literal_keyword() const noexcept -> bool {
    return kind == token_kind::kw_true || kind == token_kind::kw_false ||
           kind == token_kind::kw_unit;
  }

  /// @brief Returns whether this token starts a visibility modifier sequence.
  [[nodiscard]] constexpr auto is_visibility() const noexcept -> bool {
    return kind == token_kind::kw_pub || kind == token_kind::kw_internal ||
           kind == token_kind::kw_super || kind == token_kind::kw_priv;
  }

  /// @brief Returns whether this token is a function-level modifier keyword.
  [[nodiscard]] constexpr auto is_func_modifier() const noexcept -> bool {
    return kind == token_kind::kw_pure || kind == token_kind::kw_async ||
           kind == token_kind::kw_machine || kind == token_kind::kw_static ||
           kind == token_kind::kw_intrinsic || kind == token_kind::kw_generator;
  }

  /// @brief Returns whether this token is a `type`-declaration modifier
  /// keyword — currently just `packed`. A dedicated predicate (rather than
  /// checking `kw_packed` directly at the one call site) mirrors
  /// `is_func_modifier`'s shape so a future addition (`spec/kira-
  /// reference.md`'s aspirational `layout`/`align`/`offset`) has one place
  /// to join without another rename.
  [[nodiscard]] constexpr auto is_type_modifier() const noexcept -> bool {
    return kind == token_kind::kw_packed;
  }

  /// @brief Returns whether this token can form an assignment statement
  /// operator.
  ///
  /// The parser uses this to decide when an expression statement should instead
  /// be reinterpreted as assignment syntax.
  [[nodiscard]] constexpr auto is_assign_op() const noexcept -> bool {
    return kind == token_kind::eq || kind == token_kind::plus_eq ||
           kind == token_kind::minus_eq || kind == token_kind::star_eq ||
           kind == token_kind::slash_eq || kind == token_kind::percent_eq ||
           kind == token_kind::amp_eq || kind == token_kind::pipe_eq ||
           kind == token_kind::caret_eq || kind == token_kind::lt_lt_eq ||
           kind == token_kind::gt_gt_eq ||
           kind == token_kind::plus_percent_eq ||
           kind == token_kind::minus_percent_eq ||
           kind == token_kind::star_percent_eq ||
           kind == token_kind::plus_pipe_eq ||
           kind == token_kind::minus_pipe_eq ||
           kind == token_kind::star_pipe_eq;
  }

  /// @brief Returns whether this token participates in comparison expressions.
  ///
  /// `not` is included because the parser handles `not in` as a comparison pair
  /// rather than as two unrelated precedence levels.
  [[nodiscard]] constexpr auto is_cmp_op() const noexcept -> bool {
    return kind == token_kind::eq_eq || kind == token_kind::bang_eq ||
           kind == token_kind::lt || kind == token_kind::lt_eq ||
           kind == token_kind::gt || kind == token_kind::gt_eq ||
           kind == token_kind::kw_in || kind == token_kind::kw_not; // `not in`
  }

  /// @brief Returns whether this token belongs to the additive precedence tier.
  [[nodiscard]] constexpr auto is_add_op() const noexcept -> bool {
    return kind == token_kind::plus || kind == token_kind::minus ||
           kind == token_kind::plus_percent ||
           kind == token_kind::minus_percent || kind == token_kind::plus_pipe ||
           kind == token_kind::minus_pipe;
  }

  /// @brief Returns whether this token belongs to the multiplicative tier.
  [[nodiscard]] constexpr auto is_mul_op() const noexcept -> bool {
    return kind == token_kind::star || kind == token_kind::slash ||
           kind == token_kind::percent || kind == token_kind::star_percent ||
           kind == token_kind::star_pipe;
  }

  /// @brief Returns whether this token can begin a unary operator expression.
  [[nodiscard]] constexpr auto is_unary_op() const noexcept -> bool {
    return kind == token_kind::minus || kind == token_kind::tilde ||
           kind == token_kind::star || kind == token_kind::amp;
  }

  /// @brief Returns whether this token can begin an expression parse.
  ///
  /// This intentionally over-approximates valid expression starts so error
  /// recovery can stop at plausible boundaries instead of requiring perfect
  /// grammar knowledge at every call site.
  [[nodiscard]] constexpr auto can_start_expr() const noexcept -> bool {
    switch (kind) {
    case token_kind::ident:
    case token_kind::int_lit:
    case token_kind::float_lit:
    case token_kind::string_lit:
    case token_kind::char_lit:
    case token_kind::kw_some:
    case token_kind::kw_ok:
    case token_kind::kw_err:
    case token_kind::kw_super:
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
    case token_kind::kw_yield:
    case token_kind::kw_async:
    case token_kind::kw_par:
    case token_kind::kw_race:
    case token_kind::kw_on:
    case token_kind::kw_crew:
    case token_kind::kw_pure:
    case token_kind::kw_move:
    case token_kind::kw_shared:
    case token_kind::kw_static:
    case token_kind::backtick:
      return true;
    default:
      return false;
    }
  }

  /// @brief Returns whether this token can begin a statement parse.
  ///
  /// Recovery and top-level dispatch depend on this staying permissive enough
  /// to recognize declaration-like statements as well as expression statements.
  [[nodiscard]] constexpr auto can_start_stmt() const noexcept -> bool {
    switch (kind) {
    case token_kind::kw_let:
    case token_kind::kw_var:
    case token_kind::kw_return:
    case token_kind::kw_break:
    case token_kind::kw_continue:
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
    case token_kind::kw_intrinsic:
    case token_kind::kw_generator:
    case token_kind::kw_packed:
    case token_kind::tilde:
    case token_kind::newline:
      return true;
    default:
      return can_start_expr();
    }
  }

  /// @brief Returns whether this token can begin a top-level item.
  ///
  /// This is the parser's coarse synchronization boundary for file-level error
  /// recovery.
  [[nodiscard]] constexpr auto can_start_top_level() const noexcept -> bool {
    switch (kind) {
    case token_kind::kw_use:
    case token_kind::kw_type:
    case token_kind::kw_trait:
    case token_kind::kw_signature:
    case token_kind::kw_impl:
    case token_kind::kw_extend:
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
    case token_kind::kw_intrinsic:
    case token_kind::kw_generator:
    case token_kind::kw_packed:
    case token_kind::tilde:
    case token_kind::newline:
      return true;
    default:
      return false;
    }
  }
};

// ==========================================================================
//  Keyword lookup — classifies identifier spellings without leaking the
//  keyword table into the lexer control flow.
// ==========================================================================
/// @brief Classifies identifier text as a reserved word or plain identifier.
///
/// The lexer keeps keyword recognition centralized here so the language surface
/// can evolve without scattering string comparisons through tokenization logic.
///
/// @param text Raw identifier spelling from the source buffer.
[[nodiscard]] inline auto classify_ident(std::string_view text) noexcept
    -> token_kind {
  // We use a simple if-chain. For ~50 keywords this is perfectly fine and
  // avoids pulling in a hash map dependency. The compiler will likely
  // optimize this into a switch on the first character anyway.

  // Sort by frequency of occurrence in typical Kira code to get early-out
  // on the most common identifiers, which are NOT keywords.
  if (text.empty()) {
    return token_kind::ident;
  }

  switch (text[0]) {
  case 'a':
    if (text == "and") {
      return token_kind::kw_and;
    }
    if (text == "as") {
      return token_kind::kw_as;
    }
    if (text == "async") {
      return token_kind::kw_async;
    }
    if (text == "await") {
      return token_kind::kw_await;
    }
    if (text == "asm") {
      return token_kind::kw_asm;
    }
    if (text == "array") {
      return token_kind::kw_array;
    }
    if (text == "assert") {
      return token_kind::kw_assert;
    }
    break;
  case 'b':
    if (text == "break") {
      return token_kind::kw_break;
    }
    break;
  case 'c':
    if (text == "concept") {
      return token_kind::kw_concept;
    }
    if (text == "continue") {
      return token_kind::kw_continue;
    }
    if (text == "crew") {
      return token_kind::kw_crew;
    }
    break;
  case 'd':
    if (text == "def") {
      return token_kind::kw_def;
    }
    if (text == "dep") {
      return token_kind::kw_dep;
    }
    if (text == "deriving") {
      return token_kind::kw_deriving;
    }
    break;
  case 'e':
    if (text == "else") {
      return token_kind::kw_else;
    }
    if (text == "elif") {
      return token_kind::kw_elif;
    }
    if (text == "err") {
      return token_kind::kw_err;
    }
    if (text == "extend") {
      return token_kind::kw_extend;
    }
    break;
  case 'f':
    if (text == "for") {
      return token_kind::kw_for;
    }
    if (text == "false") {
      return token_kind::kw_false;
    }
    if (text == "fn") {
      return token_kind::kw_fn;
    }
    break;
  case 'g':
    if (text == "generator") {
      return token_kind::kw_generator;
    }
    break;
  case 'i':
    if (text == "if") {
      return token_kind::kw_if;
    }
    if (text == "in") {
      return token_kind::kw_in;
    }
    if (text == "impl") {
      return token_kind::kw_impl;
    }
    if (text == "internal") {
      return token_kind::kw_internal;
    }
    if (text == "invariant") {
      return token_kind::kw_invariant;
    }
    if (text == "intrinsic") {
      return token_kind::kw_intrinsic;
    }
    break;
  case 'l':
    if (text == "let") {
      return token_kind::kw_let;
    }
    break;
  case 'm':
    if (text == "match") {
      return token_kind::kw_match;
    }
    if (text == "module") {
      return token_kind::kw_module;
    }
    if (text == "mut") {
      return token_kind::kw_mut;
    }
    if (text == "move") {
      return token_kind::kw_move;
    }
    if (text == "machine") {
      return token_kind::kw_machine;
    }
    break;
  case 'n':
    if (text == "not") {
      return token_kind::kw_not;
    }
    if (text == "no_prelude") {
      return token_kind::kw_no_prelude;
    }
    break;
  case 'o':
    if (text == "or") {
      return token_kind::kw_or;
    }
    if (text == "ok") {
      return token_kind::kw_ok;
    }
    if (text == "on") {
      return token_kind::kw_on;
    }
    break;
  case 'p':
    if (text == "pub") {
      return token_kind::kw_pub;
    }
    if (text == "pure") {
      return token_kind::kw_pure;
    }
    if (text == "par") {
      return token_kind::kw_par;
    }
    if (text == "packed") {
      return token_kind::kw_packed;
    }
    if (text == "pre") {
      return token_kind::kw_pre;
    }
    if (text == "post") {
      return token_kind::kw_post;
    }
    if (text == "priv") {
      return token_kind::kw_priv;
    }
    break;
  case 'r':
    if (text == "return") {
      return token_kind::kw_return;
    }
    if (text == "race") {
      return token_kind::kw_race;
    }
    if (text == "requires") {
      return token_kind::kw_requires;
    }
    break;
  case 's':
    if (text == "static") {
      return token_kind::kw_static;
    }
    if (text == "signature") {
      return token_kind::kw_signature;
    }
    if (text == "some") {
      return token_kind::kw_some;
    }
    if (text == "super") {
      return token_kind::kw_super;
    }
    if (text == "shared") {
      return token_kind::kw_shared;
    }
    break;
  case 't':
    if (text == "type") {
      return token_kind::kw_type;
    }
    if (text == "trait") {
      return token_kind::kw_trait;
    }
    if (text == "true") {
      return token_kind::kw_true;
    }
    break;
  case 'u':
    if (text == "use") {
      return token_kind::kw_use;
    }
    if (text == "unit") {
      return token_kind::kw_unit;
    }
    break;
  case 'v':
    if (text == "var") {
      return token_kind::kw_var;
    }
    break;
  case 'w':
    if (text == "while") {
      return token_kind::kw_while;
    }
    if (text == "where") {
      return token_kind::kw_where;
    }
    break;
  case 'y':
    if (text == "yield") {
      return token_kind::kw_yield;
    }
    break;
  default:
    break;
  }

  // `_` is a special keyword (wildcard pattern) but only when it's exactly
  // a lone underscore. Identifiers like `_foo` are normal identifiers.
  if (text == "_") {
    return token_kind::kw_underscore;
  }

  return token_kind::ident;
}

// ==========================================================================
//  token_kind_name — returns a human-readable name for a token_kind.
//
//  This is used in diagnostic messages. The names are chosen to be
//  understandable to someone learning Kira — not compiler-internals jargon.
// ==========================================================================
[[nodiscard]] constexpr auto token_kind_name(token_kind kind) noexcept
    -> std::string_view {
  switch (kind) {
  case token_kind::eof:
    return "end of file";
  case token_kind::error:
    return "error";
  case token_kind::newline:
    return "newline";
  case token_kind::indent:
    return "indent";
  case token_kind::dedent:
    return "dedent";
  case token_kind::placeholder:
    return "<placeholder>";
  case token_kind::doc_comment:
    return "doc comment";

  case token_kind::ident:
    return "identifier";
  case token_kind::int_lit:
    return "integer literal";
  case token_kind::float_lit:
    return "float literal";
  case token_kind::string_lit:
    return "string literal";
  case token_kind::char_lit:
    return "character literal";
  case token_kind::asm_content:
    return "assembly content";

  case token_kind::kw_module:
    return "`module`";
  case token_kind::kw_type:
    return "`type`";
  case token_kind::kw_trait:
    return "`trait`";
  case token_kind::kw_signature:
    return "`signature`";
  case token_kind::kw_impl:
    return "`impl`";
  case token_kind::kw_extend:
    return "`extend`";
  case token_kind::kw_concept:
    return "`concept`";
  case token_kind::kw_def:
    return "`def`";
  case token_kind::kw_let:
    return "`let`";
  case token_kind::kw_var:
    return "`var`";
  case token_kind::kw_static:
    return "`static`";
  case token_kind::kw_use:
    return "`use`";
  case token_kind::kw_dep:
    return "`dep`";

  case token_kind::kw_pure:
    return "`pure`";
  case token_kind::kw_async:
    return "`async`";
  case token_kind::kw_machine:
    return "`machine`";
  case token_kind::kw_intrinsic:
    return "`intrinsic`";
  case token_kind::kw_generator:
    return "`generator`";

  case token_kind::kw_packed:
    return "`packed`";

  case token_kind::kw_pub:
    return "`pub`";
  case token_kind::kw_internal:
    return "`internal`";
  case token_kind::kw_super:
    return "`super`";
  case token_kind::kw_priv:
    return "`priv`";

  case token_kind::kw_if:
    return "`if`";
  case token_kind::kw_elif:
    return "`elif`";
  case token_kind::kw_else:
    return "`else`";
  case token_kind::kw_for:
    return "`for`";
  case token_kind::kw_while:
    return "`while`";
  case token_kind::kw_match:
    return "`match`";
  case token_kind::kw_return:
    return "`return`";
  case token_kind::kw_break:
    return "`break`";
  case token_kind::kw_continue:
    return "`continue`";
  case token_kind::kw_in:
    return "`in`";
  case token_kind::kw_as:
    return "`as`";

  case token_kind::kw_underscore:
    return "`_`";
  case token_kind::kw_some:
    return "`some`";
  case token_kind::kw_ok:
    return "`ok`";
  case token_kind::kw_err:
    return "`err`";

  case token_kind::kw_and:
    return "`and`";
  case token_kind::kw_or:
    return "`or`";
  case token_kind::kw_not:
    return "`not`";
  case token_kind::kw_await:
    return "`await`";
  case token_kind::kw_yield:
    return "`yield`";
  case token_kind::kw_par:
    return "`par`";
  case token_kind::kw_race:
    return "`race`";
  case token_kind::kw_on:
    return "`on`";
  case token_kind::kw_crew:
    return "`crew`";
  case token_kind::kw_asm:
    return "`asm`";

  case token_kind::kw_pre:
    return "`pre`";
  case token_kind::kw_post:
    return "`post`";
  case token_kind::kw_invariant:
    return "`invariant`";

  case token_kind::kw_where:
    return "`where`";
  case token_kind::kw_requires:
    return "`requires`";
  case token_kind::kw_deriving:
    return "`deriving`";
  case token_kind::kw_no_prelude:
    return "`no_prelude`";

  case token_kind::kw_move:
    return "`move`";
  case token_kind::kw_shared:
    return "`shared`";
  case token_kind::kw_mut:
    return "`mut`";

  case token_kind::kw_true:
    return "`true`";
  case token_kind::kw_false:
    return "`false`";
  case token_kind::kw_unit:
    return "`unit`";

  case token_kind::kw_array:
    return "`array`";
  case token_kind::kw_fn:
    return "`fn`";
  case token_kind::kw_assert:
    return "`assert`";

  case token_kind::lparen:
    return "`(`";
  case token_kind::rparen:
    return "`)`";
  case token_kind::lbracket:
    return "`[`";
  case token_kind::rbracket:
    return "`]`";
  case token_kind::lbrace:
    return "`{`";
  case token_kind::rbrace:
    return "`}`";
  case token_kind::colon:
    return "`:`";
  case token_kind::comma:
    return "`,`";
  case token_kind::dot:
    return "`.`";
  case token_kind::semicolon:
    return "`;`";
  case token_kind::hash:
    return "`#`";
  case token_kind::at:
    return "`@`";
  case token_kind::backtick:
    return "`` ` ``";
  case token_kind::question:
    return "`?`";

  case token_kind::eq:
    return "`=`";
  case token_kind::plus:
    return "`+`";
  case token_kind::minus:
    return "`-`";
  case token_kind::star:
    return "`*`";
  case token_kind::slash:
    return "`/`";
  case token_kind::percent:
    return "`%`";
  case token_kind::amp:
    return "`&`";
  case token_kind::pipe:
    return "`|`";
  case token_kind::caret:
    return "`^`";
  case token_kind::tilde:
    return "`~`";
  case token_kind::lt:
    return "`<`";
  case token_kind::gt:
    return "`>`";

  case token_kind::eq_eq:
    return "`==`";
  case token_kind::bang_eq:
    return "`!=`";
  case token_kind::lt_eq:
    return "`<=`";
  case token_kind::gt_eq:
    return "`>=`";
  case token_kind::arrow:
    return "`->`";
  case token_kind::fat_arrow:
    return "`=>`";
  case token_kind::dot_dot:
    return "`..`";
  case token_kind::dot_dot_eq:
    return "`..=`";
  case token_kind::lt_lt:
    return "`<<`";
  case token_kind::gt_gt:
    return "`>>`";

  case token_kind::plus_eq:
    return "`+=`";
  case token_kind::minus_eq:
    return "`-=`";
  case token_kind::star_eq:
    return "`*=`";
  case token_kind::slash_eq:
    return "`/=`";
  case token_kind::percent_eq:
    return "`%=`";
  case token_kind::amp_eq:
    return "`&=`";
  case token_kind::pipe_eq:
    return "`|=`";
  case token_kind::caret_eq:
    return "`^=`";
  case token_kind::lt_lt_eq:
    return "`<<=`";
  case token_kind::gt_gt_eq:
    return "`>>=`";

  case token_kind::plus_percent:
    return "`+%`";
  case token_kind::minus_percent:
    return "`-%`";
  case token_kind::star_percent:
    return "`*%`";
  case token_kind::plus_percent_eq:
    return "`+%=`";
  case token_kind::minus_percent_eq:
    return "`-%=`";
  case token_kind::star_percent_eq:
    return "`*%=`";

  case token_kind::plus_pipe:
    return "`+|`";
  case token_kind::minus_pipe:
    return "`-|`";
  case token_kind::star_pipe:
    return "`*|`";
  case token_kind::plus_pipe_eq:
    return "`+|=`";
  case token_kind::minus_pipe_eq:
    return "`-|=`";
  case token_kind::star_pipe_eq:
    return "`*|=`";

  case token_kind::count:
    return "<invalid>";
  }
  return "<unknown>";
}

/// @brief Returns user-facing expectation text for parser diagnostics.
///
/// This intentionally uses prose for token categories so diagnostics can say
/// "expected a name" instead of leaking parser jargon like `ident`.
///
/// @param kind Token kind the parser hoped to see.
[[nodiscard]] constexpr auto token_kind_description(token_kind kind) noexcept
    -> std::string_view {
  switch (kind) {
  case token_kind::ident:
    return "a name";
  case token_kind::int_lit:
  case token_kind::float_lit:
    return "a number";
  case token_kind::string_lit:
    return "a string";
  case token_kind::char_lit:
    return "a character";
  case token_kind::newline:
    return "end of line";
  case token_kind::indent:
    return "an indented block";
  case token_kind::dedent:
    return "end of block";
  case token_kind::eof:
    return "end of file";
  default:
    return token_kind_name(kind);
  }
}

} // namespace kira
