#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "source_location.h"
#include "token.h"

namespace kira::ast {

// ==========================================================================
//  Forward declarations — every AST node type.
// ==========================================================================

// Top-level
struct file;
struct module_decl;

// Items
struct use_decl;
struct type_decl;
struct trait_decl;
struct impl_decl;
struct concept_decl;
struct func_decl;
struct sub_module_decl;
struct dep_decl;
struct static_decl;
struct splice_stmt;

// Type system
struct named_type;
struct tuple_type;
struct slice_type;
struct array_type;
struct ref_type;
struct ptr_type;
struct fn_type;
struct quote_type;
struct union_type;
struct refinement_type;

// Type params & bounds
struct type_param;
struct type_arg;
struct bound_term;
struct where_constraint;

// Struct / sum
struct struct_field;
struct struct_body;
struct sum_variant;
struct sum_body;

// Trait / impl internals
struct associated_type_decl;
struct associated_type_def;

// Concept internals
struct concept_param;
struct concept_constraint;

// Function-related
struct Param;
struct contract_clause;

// Statements
struct let_stmt;
struct var_stmt;
struct assign_stmt;
struct expr_stmt;
struct return_stmt;
struct if_stmt;
struct while_stmt;
struct for_stmt;
struct match_stmt;
struct crew_stmt;
struct asm_stmt;

// Expressions
struct ident_expr;
struct literal_expr;
struct binary_expr;
struct unary_expr;
struct PostfixExpr;
struct call_expr;
struct index_expr;
struct field_expr;
struct cast_expr;
struct try_expr;
struct tuple_expr;
struct array_expr;
struct struct_expr;
struct lambda_expr;
struct match_expr;
struct if_expr;
struct for_expr;
struct await_expr;
struct par_expr;
struct race_expr;
struct on_expr;
struct block_expr;
struct quote_expr;
struct splice_expr;
struct static_expr;
struct module_path_expr;
struct group_expr;

// Patterns
struct wildcard_pattern;
struct literal_pattern;
struct binding_pattern;
struct constructor_pattern;
struct tuple_pattern;
struct struct_pattern;
struct array_pattern;
struct range_pattern;
struct option_pattern;
struct result_pattern;
struct ref_pattern;
struct or_pattern;
struct group_pattern;

// Match arms
struct match_arm;

// ==========================================================================
//  Smart pointer aliases — every AST node is heap-allocated and owned
//  via unique_ptr. This keeps node sizes small and allows recursive types.
// ==========================================================================

template <typename T> using ptr = std::unique_ptr<T>;

template <typename T> using ptr_vec = std::vector<ptr<T>>;

/// Helper to create an AST node. Usage: `auto n = make<ident_expr>(...);`
template <typename T, typename... Args>
[[nodiscard]] ptr<T> make(Args &&...args) {
  return std::make_unique<T>(std::forward<Args>(args)...);
}

// ==========================================================================
//  NodeKind — tag for downcasting via the base class. Every concrete
//  node has a unique kind value.
// ==========================================================================

enum class NodeKind : uint8_t {
  // Sentinel
  error_node,

  // Top-level
  File,
  module_decl,

  // Items
  use_decl,
  type_decl,
  trait_decl,
  impl_decl,
  concept_decl,
  func_decl,
  sub_module_decl,
  dep_decl,
  static_decl,
  splice_stmt,

  // Types
  named_type,
  tuple_type,
  slice_type,
  array_type,
  ref_type,
  ptr_type,
  fn_type,
  quote_type,
  union_type,
  refinement_type,

  // Statements
  let_stmt,
  var_stmt,
  assign_stmt,
  expr_stmt,
  return_stmt,
  if_stmt,
  while_stmt,
  for_stmt,
  match_stmt,
  crew_stmt,
  asm_stmt,

  // Expressions
  ident_expr,
  literal_expr,
  binary_expr,
  unary_expr,
  PostfixExpr,
  call_expr,
  index_expr,
  field_expr,
  cast_expr,
  try_expr,
  tuple_expr,
  array_expr,
  struct_expr,
  lambda_expr,
  match_expr,
  if_expr,
  for_expr,
  await_expr,
  par_expr,
  race_expr,
  on_expr,
  block_expr,
  quote_expr,
  splice_expr,
  static_expr,
  module_path_expr,
  group_expr,

  // Patterns
  wildcard_pattern,
  literal_pattern,
  binding_pattern,
  constructor_pattern,
  tuple_pattern,
  struct_pattern,
  array_pattern,
  range_pattern,
  option_pattern,
  result_pattern,
  ref_pattern,
  or_pattern,
  group_pattern,
};

// ==========================================================================
//  Base classes — Node, type_expr, Stmt, Expr, Pattern
//
//  All AST nodes inherit from Node which carries a span and an error flag.
//  The error flag (`has_error`) is set when the parser used error recovery
//  to produce this node. Later phases can check it to skip semantic
//  analysis of broken subtrees — this prevents cascading errors.
// ==========================================================================

struct node {
  NodeKind kind;
  span span;
  bool has_error = false;

  explicit node(NodeKind k, span s = span::dummy()) : kind(k), span(s) {}
  virtual ~node() = default;

  // Non-copyable, movable.
  node(const node &) = delete;
  node &operator=(const node &) = delete;
  node(node &&) = default;
  node &operator=(node &&) = default;
};

/// An AST node that was synthesized during error recovery. It stands in
/// for whatever the parser couldn't understand. This lets the rest of the
/// tree remain well-formed so downstream phases can keep going.
struct error_node : node {
  std::string description; ///< What we tried and failed to parse.

  explicit error_node(span s = span::dummy(), std::string desc = "")
      : node(NodeKind::error_node, s), description(std::move(desc)) {
    has_error = true;
  }
};

// --------------------------------------------------------------------------
//  Visibility
// --------------------------------------------------------------------------

enum class visibility : uint8_t {
  def,      ///< No explicit visibility keyword
  pub,      ///< `pub`
  internal, ///< `internal`
  super,    ///< `super`
  priv,     ///< `priv`
};

[[nodiscard]] inline visibility token_to_visibility(token_kind kind) noexcept {
  switch (kind) {
  case token_kind::kw_pub:
    return visibility::pub;
  case token_kind::kw_internal:
    return visibility::internal;
  case token_kind::kw_super:
    return visibility::super;
  case token_kind::kw_priv:
    return visibility::priv;
  default:
    return visibility::def;
  }
}

// ==========================================================================
//  Type expression nodes
// ==========================================================================

struct type_expr : node {
  using node::node;
};

/// A type expression that the parser couldn't understand. Stands in for
/// the real thing so the tree remains structurally valid.
struct error_type_expr : type_expr {
  explicit error_type_expr(span s = span::dummy())
      : type_expr(NodeKind::named_type,
                  s) { // reuse kind; has_error flag distinguishes
    has_error = true;
  }
};

struct named_type : type_expr {
  std::vector<std::string> path; ///< e.g., ["std", "collections", "Map"]
  std::vector<ptr<type_expr>> type_args; ///< Generic arguments, if any.
  // type_arg can also carry value-level expressions; we handle that in
  // a wrapper node below.

  named_type() : type_expr(NodeKind::named_type) {}
};

struct tuple_type : type_expr {
  std::vector<ptr<type_expr>> elements;

  tuple_type() : type_expr(NodeKind::tuple_type) {}
};

struct slice_type : type_expr {
  ptr<type_expr> element;

  slice_type() : type_expr(NodeKind::slice_type) {}
};

struct array_type : type_expr {
  ptr<type_expr> element;
  ptr<node> size; ///< An expression giving the array length.

  array_type() : type_expr(NodeKind::array_type) {}
};

struct ref_type : type_expr {
  ptr<type_expr> inner;
  bool is_mut = false;

  ref_type() : type_expr(NodeKind::ref_type) {}
};

struct ptr_type : type_expr {
  ptr<type_expr> inner;
  bool is_mut = false;

  ptr_type() : type_expr(NodeKind::ptr_type) {}
};

struct fn_type : type_expr {
  std::vector<ptr<type_expr>> param_types;
  ptr<type_expr> return_type; ///< nullptr if no return type.

  fn_type() : type_expr(NodeKind::fn_type) {}
};

struct quote_type : type_expr {
  token_kind quote_kind; ///< KwExpr, KwStmt, KwDefExpr, or KwTypeExpr

  explicit quote_type(token_kind qk = token_kind::kw_expr)
      : type_expr(NodeKind::quote_type), quote_kind(qk) {}
};

struct union_type : type_expr {
  std::vector<ptr<type_expr>> alternatives;

  union_type() : type_expr(NodeKind::union_type) {}
};

struct refinement_type : type_expr {
  ptr<type_expr> base;
  ptr<node> predicate; ///< The `where expr` part.

  refinement_type() : type_expr(NodeKind::refinement_type) {}
};

// ==========================================================================
//  Type parameters and bounds
// ==========================================================================

struct type_param {
  span span;
  std::string name;
  /// For type params: an optional trait/concept bound.
  /// For value params: the type of the value.
  ptr<type_expr> bound_or_type;
  bool is_value_param = false; ///< True if this is `name : type` (value param).
};

struct type_arg {
  span span;
  /// A type argument is either a type expression or a value expression.
  /// We store both as Node* and disambiguate later in semantic analysis.
  ptr<node> value;
  std::optional<std::string> name; ///< Named argument: `name: type`
};

struct bound_term {
  span span;
  /// A trait bound like `Trait[Args]` or a function type bound.
  ptr<type_expr> type;
};

struct Bound {
  span span;
  std::vector<bound_term> terms; ///< Connected by `+`.
};

struct where_constraint {
  span span;
  ptr<type_expr> subject;
  /// Either a trait bound or an associated type equality constraint.
  ptr<type_expr> bound_or_type;
};

// ==========================================================================
//  Struct and sum type bodies
// ==========================================================================

struct struct_field {
  span span;
  visibility visibility = visibility::def;
  std::string name;
  ptr<type_expr> type;
};

struct struct_body {
  span span;
  std::vector<struct_field> fields;
};

struct sum_variant {
  span span;
  std::string name;
  std::vector<ptr<type_expr>> payload_types; ///< Empty for unit variants.
};

struct sum_body {
  span span;
  std::vector<sum_variant> variants;
};

// ==========================================================================
//  Expression nodes
// ==========================================================================

struct Expr : node {
  using node::node;
};

/// A placeholder expression for error recovery.
struct error_expr : Expr {
  std::string description;

  explicit error_expr(span s = span::dummy(), std::string desc = "")
      : Expr(NodeKind::ident_expr, s), description(std::move(desc)) {
    has_error = true;
  }
};

struct ident_expr : Expr {
  std::string name;

  ident_expr() : Expr(NodeKind::ident_expr) {}
};

struct literal_expr : Expr {
  token_kind lit_kind; ///< IntLit, FloatLit, StringLit, CharLit, KwTrue,
                       ///< KwFalse, KwUnit
  std::string value;   ///< The raw text of the literal.

  literal_expr() : Expr(NodeKind::literal_expr) {}
};

enum class BinaryOp : uint8_t {
  // Pipe
  Pipe,
  // Logical
  Or,
  And,
  // Comparison
  EqEq,
  BangEq,
  Lt,
  LtEq,
  Gt,
  GtEq,
  In,
  NotIn,
  // Arithmetic
  Add,
  Sub,
  Mul,
  Div,
  Mod,
  // Wrapping
  AddWrap,
  SubWrap,
  MulWrap,
  // Saturating
  AddSat,
  SubSat,
  MulSat,
  // Shifts
  Shl,
  Shr,
  // Bitwise
  BitAnd,
  BitOr,
  BitXor,
  // Range (used in patterns)
  Range,
  RangeInclusive,
};

[[nodiscard]] inline std::string_view binary_op_name(BinaryOp op) noexcept {
  switch (op) {
  case BinaryOp::Pipe:
    return "|";
  case BinaryOp::Or:
    return "or";
  case BinaryOp::And:
    return "and";
  case BinaryOp::EqEq:
    return "==";
  case BinaryOp::BangEq:
    return "!=";
  case BinaryOp::Lt:
    return "<";
  case BinaryOp::LtEq:
    return "<=";
  case BinaryOp::Gt:
    return ">";
  case BinaryOp::GtEq:
    return ">=";
  case BinaryOp::In:
    return "in";
  case BinaryOp::NotIn:
    return "not in";
  case BinaryOp::Add:
    return "+";
  case BinaryOp::Sub:
    return "-";
  case BinaryOp::Mul:
    return "*";
  case BinaryOp::Div:
    return "/";
  case BinaryOp::Mod:
    return "%";
  case BinaryOp::AddWrap:
    return "+%";
  case BinaryOp::SubWrap:
    return "-%";
  case BinaryOp::MulWrap:
    return "*%";
  case BinaryOp::AddSat:
    return "+|";
  case BinaryOp::SubSat:
    return "-|";
  case BinaryOp::MulSat:
    return "*|";
  case BinaryOp::Shl:
    return "<<";
  case BinaryOp::Shr:
    return ">>";
  case BinaryOp::BitAnd:
    return "&";
  case BinaryOp::BitOr:
    return "|";
  case BinaryOp::BitXor:
    return "^";
  case BinaryOp::Range:
    return "..";
  case BinaryOp::RangeInclusive:
    return "..=";
  }
  return "<?>";
}

struct binary_expr : Expr {
  BinaryOp op;
  ptr<Expr> lhs;
  ptr<Expr> rhs;

  binary_expr() : Expr(NodeKind::binary_expr) {}
};

enum class UnaryOp : uint8_t {
  Neg,       ///< `-`
  BitNot,    ///< `~`
  Deref,     ///< `*`
  AddrOf,    ///< `&`
  AddrOfMut, ///< `&mut`
  Not,       ///< `not`
};

[[nodiscard]] inline std::string_view unary_op_name(UnaryOp op) noexcept {
  switch (op) {
  case UnaryOp::Neg:
    return "-";
  case UnaryOp::BitNot:
    return "~";
  case UnaryOp::Deref:
    return "*";
  case UnaryOp::AddrOf:
    return "&";
  case UnaryOp::AddrOfMut:
    return "&mut";
  case UnaryOp::Not:
    return "not";
  }
  return "<?>";
}

struct unary_expr : Expr {
  UnaryOp op;
  ptr<Expr> operand;

  unary_expr() : Expr(NodeKind::unary_expr) {}
};

/// Field access: `expr.name`
struct field_expr : Expr {
  ptr<Expr> object;
  std::string field_name;
  std::vector<ptr<type_expr>> generic_args; ///< For `expr.method[T]`

  field_expr() : Expr(NodeKind::field_expr) {}
};

/// Index: `expr[index]`
struct index_expr : Expr {
  ptr<Expr> object;
  ptr<Expr> index;

  index_expr() : Expr(NodeKind::index_expr) {}
};

/// Function/method call: `expr(args...)`
struct call_arg {
  span span;
  std::optional<std::string> name; ///< Named arg: `name: value`
  ptr<Expr> value;
};

struct call_expr : Expr {
  ptr<Expr> callee;
  std::vector<call_arg> args;

  call_expr() : Expr(NodeKind::call_expr) {}
};

/// Type cast: `expr as Type`
struct cast_expr : Expr {
  ptr<Expr> operand;
  ptr<type_expr> target_type;

  cast_expr() : Expr(NodeKind::cast_expr) {}
};

/// Try propagation: `expr?` or `expr?.field`
struct try_expr : Expr {
  ptr<Expr> operand;

  try_expr() : Expr(NodeKind::try_expr) {}
};

/// Tuple literal: `(a, b, c)`
struct tuple_expr : Expr {
  std::vector<ptr<Expr>> elements;

  tuple_expr() : Expr(NodeKind::tuple_expr) {}
};

/// Array literal: `[a, b, c]` or fill: `[val; count]`
struct array_expr : Expr {
  std::vector<ptr<Expr>> elements;
  ptr<Expr> fill_value; ///< Non-null for `[val; count]` form.
  ptr<Expr> fill_count;

  array_expr() : Expr(NodeKind::array_expr) {}
};

/// Struct literal field: `name: expr` or shorthand `name`
struct struct_field_init {
  span span;
  std::string name;
  ptr<Expr> value; ///< nullptr for shorthand `{x}` (means `{x: x}`)
};

/// Struct literal: `{a: 1, b: 2}`
struct struct_expr : Expr {
  std::vector<struct_field_init> fields;

  struct_expr() : Expr(NodeKind::struct_expr) {}
};

/// Lambda / closure: `x => x + 1` or `(a, b) -> int => a + b`
struct lambda_param {
  span span;
  ptr<node> pattern;              ///< The parameter pattern.
  ptr<type_expr> type_annotation; ///< Optional type annotation.
};

struct lambda_expr : Expr {
  bool is_pure = false;
  bool is_move = false;
  std::vector<lambda_param> params;
  ptr<type_expr> return_type; ///< Optional return type annotation.
  /// Body is either a single expression or a block (vector of statements).
  ptr<Expr> body_expr;
  std::vector<ptr<node>> body_stmts;

  lambda_expr() : Expr(NodeKind::lambda_expr) {}
};

/// Module path expression: `a.b.c`
struct module_path_expr : Expr {
  std::vector<std::string> segments;

  module_path_expr() : Expr(NodeKind::module_path_expr) {}
};

/// Parenthesized expression: `(expr)`
struct group_expr : Expr {
  ptr<Expr> inner;

  group_expr() : Expr(NodeKind::group_expr) {}
};

/// `if expr: ... elif ...: ... else: ...` (expression form)
struct if_branch {
  span span;
  ptr<Expr> condition;
  std::vector<ptr<node>> body; ///< Block of statements/exprs.
};

struct if_expr : Expr {
  std::vector<if_branch> branches; ///< `if` + zero or more `elif`
  std::vector<ptr<node>> else_body;

  if_expr() : Expr(NodeKind::if_expr) {}
};

/// `match subject: ...`
struct match_arm {
  span span;
  ptr<node> pattern;
  ptr<Expr> guard; ///< Optional `if expr` guard.
  /// Body: inline expression or block.
  ptr<Expr> body_expr;
  std::vector<ptr<node>> body_stmts;
  bool has_error = false;
};

struct match_expr : Expr {
  ptr<Expr> subject;
  std::vector<match_arm> arms;

  match_expr() : Expr(NodeKind::match_expr) {}
};

/// `for vars in iter [if guard] => yield_expr`
struct for_expr : Expr {
  struct IterClause {
    std::vector<ptr<node>> patterns;
    ptr<Expr> iterable;
  };
  std::vector<IterClause> clauses;
  ptr<Expr> guard;      ///< Optional `if` filter.
  ptr<Expr> yield_expr; ///< The `=> expr` part.

  for_expr() : Expr(NodeKind::for_expr) {}
};

/// `await expr` or `await yield`
struct await_expr : Expr {
  ptr<Expr> operand; ///< nullptr for `await yield`.
  bool is_yield = false;

  await_expr() : Expr(NodeKind::await_expr) {}
};

/// `par: ...`
struct par_expr : Expr {
  std::vector<ptr<Expr>> branches;

  par_expr() : Expr(NodeKind::par_expr) {}
};

/// `race: ...`
struct race_expr : Expr {
  std::vector<ptr<Expr>> branches;

  race_expr() : Expr(NodeKind::race_expr) {}
};

/// `on(Type): block` or `on(Type, sender)`
struct on_expr : Expr {
  ptr<type_expr> context_type;
  ptr<Expr> sender; ///< Optional second argument.
  std::vector<ptr<node>> body;

  on_expr() : Expr(NodeKind::on_expr) {}
};

/// `block_expr` — `INDENT stmts DEDENT`
struct block_expr : Expr {
  std::vector<ptr<node>> stmts;

  block_expr() : Expr(NodeKind::block_expr) {}
};

/// Quasi-quote: `` `(content)` `` or `` `content` ``
struct quote_expr : Expr {
  std::vector<Token> tokens; ///< Raw tokens inside the quote.

  quote_expr() : Expr(NodeKind::quote_expr) {}
};

/// Splice: `~(expr)` or `~ident`
struct splice_expr : Expr {
  ptr<Expr> operand;

  splice_expr() : Expr(NodeKind::splice_expr) {}
};

/// `static expr`
struct static_expr : Expr {
  ptr<Expr> operand;

  static_expr() : Expr(NodeKind::static_expr) {}
};

// ==========================================================================
//  Pattern nodes
// ==========================================================================

struct Pattern : node {
  using node::node;
};

struct error_pattern : Pattern {
  explicit error_pattern(span s = span::dummy())
      : Pattern(NodeKind::wildcard_pattern, s) {
    has_error = true;
  }
};

/// `_`
struct wildcard_pattern : Pattern {
  wildcard_pattern() : Pattern(NodeKind::wildcard_pattern) {}
};

/// Literal in pattern position: `42`, `"hello"`, `true`
struct literal_pattern : Pattern {
  token_kind lit_kind;
  std::string value;

  literal_pattern() : Pattern(NodeKind::literal_pattern) {}
};

/// Simple name binding: `x`
struct binding_pattern : Pattern {
  std::string name;

  binding_pattern() : Pattern(NodeKind::binding_pattern) {}
};

/// Constructor: `Some(x)`, `Point(a, b)`
struct constructor_pattern : Pattern {
  std::string name;
  std::vector<ptr<Pattern>> args;

  constructor_pattern() : Pattern(NodeKind::constructor_pattern) {}
};

/// Tuple: `(a, b, c)`
struct tuple_pattern : Pattern {
  std::vector<ptr<Pattern>> elements;

  tuple_pattern() : Pattern(NodeKind::tuple_pattern) {}
};

/// Struct pattern field: `name: pattern`, `name`, or `..`
struct field_pattern {
  span span;
  std::string name;     ///< Empty for `..` (rest).
  ptr<Pattern> pattern; ///< nullptr for shorthand `{name}`.
  bool is_rest = false; ///< True for `..`.
};

/// Struct pattern: `{x: a, y: b, ..}`
struct struct_pattern : Pattern {
  std::vector<field_pattern> fields;

  struct_pattern() : Pattern(NodeKind::struct_pattern) {}
};

/// Array/slice pattern: `[a, b, c]`
struct array_pattern : Pattern {
  std::vector<ptr<Pattern>> elements;

  array_pattern() : Pattern(NodeKind::array_pattern) {}
};

/// Range: `1..10`, `1..=10`, `1..`, `..10`
struct range_pattern : Pattern {
  ptr<Expr> start;        ///< nullptr for `..end`
  ptr<Expr> end;          ///< nullptr for `start..`
  bool inclusive = false; ///< True for `..=`

  range_pattern() : Pattern(NodeKind::range_pattern) {}
};

/// `some(pattern)`, `ok(pattern)`, `err(pattern)`
enum class OptionResultKind : uint8_t { Some, Ok, Err };

struct option_pattern : Pattern {
  OptionResultKind option_kind;
  ptr<Pattern> inner;

  option_pattern() : Pattern(NodeKind::option_pattern) {}
};

/// Result pattern — reuses option_pattern but with different kind.
struct result_pattern : Pattern {
  OptionResultKind result_kind; ///< Ok or Err
  ptr<Pattern> inner;

  result_pattern() : Pattern(NodeKind::result_pattern) {}
};

/// `&pattern`
struct ref_pattern : Pattern {
  ptr<Pattern> inner;

  ref_pattern() : Pattern(NodeKind::ref_pattern) {}
};

/// `a | b | c`
struct or_pattern : Pattern {
  std::vector<ptr<Pattern>> alternatives;

  or_pattern() : Pattern(NodeKind::or_pattern) {}
};

/// `(pattern)` — parenthesized pattern for grouping.
struct group_pattern : Pattern {
  ptr<Pattern> inner;

  group_pattern() : Pattern(NodeKind::group_pattern) {}
};

// ==========================================================================
//  Statement nodes
// ==========================================================================

struct Stmt : node {
  using node::node;
};

struct error_stmt : Stmt {
  explicit error_stmt(span s = span::dummy()) : Stmt(NodeKind::expr_stmt, s) {
    has_error = true;
  }
};

/// `let pattern [: type] = expr`
/// `let pattern [: type] = expr else: block`
struct let_stmt : Stmt {
  ptr<Pattern> pattern;
  ptr<type_expr> type_annotation; ///< Optional.
  ptr<Expr> initializer;
  std::vector<ptr<node>> else_body; ///< Non-empty for `let ... else:` form.

  let_stmt() : Stmt(NodeKind::let_stmt) {}
};

/// `var name [: type] = expr`
struct var_stmt : Stmt {
  std::string name;
  ptr<type_expr> type_annotation; ///< Optional.
  ptr<Expr> initializer;

  var_stmt() : Stmt(NodeKind::var_stmt) {}
};

/// Assignment operators.
enum class AssignOp : uint8_t {
  Assign,        // =
  AddAssign,     // +=
  SubAssign,     // -=
  MulAssign,     // *=
  DivAssign,     // /=
  ModAssign,     // %=
  AndAssign,     // &=
  OrAssign,      // |=
  XorAssign,     // ^=
  ShlAssign,     // <<=
  ShrAssign,     // >>=
  AddWrapAssign, // +%=
  SubWrapAssign, // -%=
  MulWrapAssign, // *%=
  AddSatAssign,  // +|=
  SubSatAssign,  // -|=
  MulSatAssign,  // *|=
};

struct assign_stmt : Stmt {
  ptr<Expr> target;
  AssignOp op;
  ptr<Expr> value;

  assign_stmt() : Stmt(NodeKind::assign_stmt) {}
};

/// Expression used as a statement.
struct expr_stmt : Stmt {
  ptr<Expr> expr;

  expr_stmt() : Stmt(NodeKind::expr_stmt) {}
};

/// `return [expr]`
struct return_stmt : Stmt {
  ptr<Expr> value; ///< nullptr if bare `return`.

  return_stmt() : Stmt(NodeKind::return_stmt) {}
};

/// `if expr: block { elif expr: block } [else: block]`
struct if_stmt : Stmt {
  std::vector<if_branch> branches;
  std::vector<ptr<node>> else_body;

  if_stmt() : Stmt(NodeKind::if_stmt) {}
};

/// `while expr: block` or `while let pattern = expr: block`
struct while_stmt : Stmt {
  ptr<Expr> condition;      ///< nullptr for `while let` form.
  ptr<Pattern> let_pattern; ///< Non-null for `while let` form.
  ptr<Expr> let_expr;       ///< The expr in `while let pat = expr`.
  std::vector<ptr<node>> body;

  while_stmt() : Stmt(NodeKind::while_stmt) {}
};

/// `for vars in expr [if guard]: block`
struct for_stmt : Stmt {
  std::vector<ptr<Pattern>> patterns;
  ptr<Expr> iterable;
  ptr<Expr> guard; ///< Optional `if` filter.
  std::vector<ptr<node>> body;

  for_stmt() : Stmt(NodeKind::for_stmt) {}
};

/// `match subject: ...`
struct match_stmt : Stmt {
  ptr<Expr> subject;
  std::vector<match_arm> arms;

  match_stmt() : Stmt(NodeKind::match_stmt) {}
};

/// Crew option: `name: value`
struct crew_option {
  span span;
  std::string name;
  std::string value;
};

/// `crew name(options): block`
struct crew_stmt : Stmt {
  std::string name;
  std::vector<crew_option> options;
  std::vector<ptr<node>> body;

  crew_stmt() : Stmt(NodeKind::crew_stmt) {}
};

/// `asm { content }`
struct asm_stmt : Stmt {
  std::string content;

  asm_stmt() : Stmt(NodeKind::asm_stmt) {}
};

/// `~expr`
struct splice_stmt : Stmt {
  ptr<Expr> expr;

  splice_stmt() : Stmt(NodeKind::splice_stmt) {}
};

// ==========================================================================
//  Declaration / item nodes
// ==========================================================================

/// `use` path handling.
struct use_item {
  span span;
  std::string name;
  std::optional<std::string> alias; ///< `as` alias.
};

enum class UseSelectorKind : uint8_t {
  Single,   ///< `use a.b.Foo` or `use a.b.Foo as Bar`
  Group,    ///< `use a.b.{Foo, Bar}`
  Wildcard, ///< `use a.b.*`
};

struct use_selector {
  span span;
  UseSelectorKind kind;
  std::vector<use_item> items; ///< For group selector; single has one item.
};

struct use_decl : node {
  visibility visibility = visibility::def;
  std::vector<std::string> path;
  std::optional<use_selector> selector;

  use_decl() : node(NodeKind::use_decl) {}
};

/// `type Name[Params] = TypeDef`
struct type_decl : node {
  visibility visibility = visibility::def;
  std::string name;
  std::vector<type_param> type_params;
  /// The type definition body — one of: struct_body, sum_body, type_expr,
  /// refinement_type.
  ptr<node> definition;
  std::vector<std::string> deriving;
  ptr<Expr> invariant;

  type_decl() : node(NodeKind::type_decl) {}
};

/// Associated type in a trait: `type Name [= Default]`
struct associated_type_decl {
  span span;
  visibility visibility = visibility::def;
  std::string name;
  ptr<type_expr> default_type;
};

/// Associated type in an impl: `type Name = ConcreteType`
struct associated_type_def {
  span span;
  std::string name;
  ptr<type_expr> type;
};

/// Trait declaration.
struct trait_decl : node {
  visibility visibility = visibility::def;
  std::string name;
  std::vector<type_param> type_params;
  std::optional<Bound> requires_bound;
  std::vector<ptr<node>>
      items; ///< func_decl, static_decl, associated_type_decl nodes.

  trait_decl() : node(NodeKind::trait_decl) {}
};

/// Concept declaration.
struct concept_param {
  span span;
  std::string name;
  bool is_higher_kinded = false; ///< `Name[_]`
};

struct concept_constraint {
  span span;
  ptr<type_expr> subject;  ///< Non-null for `type : bound` constraints.
  ptr<node> bound_or_expr; ///< Bound for type constraints, Expr for value
                           ///< constraints.
};

struct concept_decl : node {
  visibility visibility = visibility::def;
  std::string name;
  std::vector<concept_param> params;
  std::vector<concept_constraint> constraints;

  concept_decl() : node(NodeKind::concept_decl) {}
};

/// Impl declaration.
struct impl_decl : node {
  std::vector<type_param> type_params;
  ptr<type_expr> trait_type;
  ptr<type_expr> for_type;
  std::vector<where_constraint> where_constraints;
  std::vector<ptr<node>>
      items; ///< func_decl, static_decl, associated_type_def.

  impl_decl() : node(NodeKind::impl_decl) {}
};

/// Function modifier flags.
struct func_modifiers {
  bool is_pure = false;
  bool is_async = false;
  bool is_machine = false;
  bool is_static = false;
  ptr<type_expr> async_context; ///< Optional async context type.
};

/// Contract clause: `pre expr [, message]` or `post expr [, message]`
struct contract_clause {
  span span;
  bool is_pre; ///< true = `pre`, false = `post`
  ptr<Expr> condition;
  std::optional<std::string> message;
};

/// Parameter in a function signature.
struct Param {
  span span;
  ptr<Pattern> pattern;
  ptr<type_expr> type_annotation; ///< Optional.
  ptr<Expr> default_value;        ///< Optional.
};

/// Function declaration.
struct func_decl : node {
  visibility visibility = visibility::def;
  func_modifiers modifiers;
  std::string name;
  std::vector<type_param> type_params;
  std::vector<Param> params;
  ptr<type_expr> return_type; ///< Optional.
  std::vector<where_constraint> where_constraints;
  std::vector<contract_clause> contracts;
  /// Body: either an inline expression or a block of statements.
  ptr<Expr> body_expr;
  std::vector<ptr<node>> body_stmts;

  func_decl() : node(NodeKind::func_decl) {}
};

/// Sub-module: `module Name` or `module Name: ...`
struct sub_module_decl : node {
  visibility visibility = visibility::def;
  std::string name;
  std::vector<ptr<node>> items; ///< Empty for forward declarations.

  sub_module_decl() : node(NodeKind::sub_module_decl) {}
};

/// Dependency declaration: `dep Name: ...`
struct dep_field {
  span span;
  std::string key;
  std::string value;
};

struct dep_decl : node {
  std::string name;
  std::vector<dep_field> fields;

  dep_decl() : node(NodeKind::dep_decl) {}
};

/// Static declarations — multiple forms:
///   - `static if expr: ...`
///   - `static Name : Type = expr`
///   - `static assert expr [, message]`
///   - `static for vars in expr [if guard] => expr`
///   - `static for vars in expr [if guard]: block`
enum class StaticDeclKind : uint8_t {
  ConditionalCompilation, ///< `static if`
  Binding,                ///< `static Name = expr`
  Assert,                 ///< `static assert expr`
  ForInline,              ///< `static for ... => expr`
  ForBlock,               ///< `static for ... : block`
};

struct static_decl : node {
  visibility visibility = visibility::def;
  StaticDeclKind decl_kind;

  // For Binding:
  std::string name;
  ptr<type_expr> type_annotation;
  ptr<Expr> initializer;

  // For Assert:
  ptr<Expr> assert_condition;
  std::optional<std::string> assert_message;

  // For ConditionalCompilation:
  ptr<Expr> if_condition;
  std::vector<ptr<node>> if_body;
  std::vector<ptr<node>> else_body;

  // For ForInline / ForBlock:
  std::vector<ptr<Pattern>> for_patterns;
  ptr<Expr> for_iterable;
  ptr<Expr> for_guard;
  ptr<Expr> for_yield;             ///< For ForInline.
  std::vector<ptr<node>> for_body; ///< For ForBlock.

  static_decl()
      : node(NodeKind::static_decl), decl_kind(StaticDeclKind::Binding) {}
};

// ==========================================================================
//  Module declaration and file root
// ==========================================================================

struct module_decl : node {
  std::vector<std::string> path;

  module_decl() : node(NodeKind::module_decl) {}
};

struct file : node {
  ptr<module_decl> module_decl;
  bool no_prelude = false;
  std::vector<ptr<node>> items;

  file() : node(NodeKind::File) {}
};

// ==========================================================================
//  Pattern alias wrapper — for `pattern as name`
// ==========================================================================

struct aliased_pattern {
  ptr<Pattern> pattern;
  std::optional<std::string> alias;
  span span;
};

} // namespace kira::ast
