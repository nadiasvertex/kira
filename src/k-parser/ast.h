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
struct struct_type_def;
struct sum_type_def;
struct trait_decl;
struct impl_decl;
struct concept_decl;
struct func_decl;
struct sub_module_decl;
struct dep_decl;
struct static_decl;
struct associated_type_decl_node;
struct associated_type_def_node;
struct splice_stmt;

// Type system
struct bound_type;
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
struct pattern;
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
struct async_expr;
struct par_expr;
struct race_expr;
struct crew_expr;
struct on_expr;
struct block_expr;
struct quote_expr;
struct splice_expr;
struct static_expr;
struct module_path_expr;
struct group_expr;
struct where_expr;

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
template <typename T> using Ptr = std::unique_ptr<T>; ///< Alias for ptr<T>; used in parser.

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

enum class node_kind : uint8_t {
  // Sentinel
  error_node,

  // Top-level
  file_node,
  module_decl,

  // Items
  use_decl,
  type_decl,
  struct_type_def,
  sum_type_def,
  trait_decl,
  impl_decl,
  concept_decl,
  func_decl,
  sub_module_decl,
  dep_decl,
  static_decl,
  associated_type_decl_node,
  associated_type_def_node,
  splice_stmt,

  // Types
  named_type,
  bound_type,
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
  async_expr,
  par_expr,
  race_expr,
  crew_expr,
  on_expr,
  block_expr,
  quote_expr,
  splice_expr,
  static_expr,
  module_path_expr,
  group_expr,
  where_expr,

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
  node_kind kind;
  source_span span;
  bool has_error = false;

  explicit node(node_kind k, source_span s = source_span::dummy()) : kind(k), span(s) {}
  virtual ~node() = default;

  // Non-copyable, movable.
  node(const node &) = delete;
  auto operator=(const node &) -> node & = delete;
  node(node &&) = default;
  auto operator=(node &&) -> node & = default;
};

/// An AST node that was synthesized during error recovery. It stands in
/// for whatever the parser couldn't understand. This lets the rest of the
/// tree remain well-formed so downstream phases can keep going.
struct error_node : node {
  std::string description; ///< What we tried and failed to parse.

  explicit error_node(source_span s = source_span::dummy(), std::string desc = "")
      : node(node_kind::error_node, s), description(std::move(desc)) {
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

[[nodiscard]] inline auto token_to_visibility(token_kind kind) noexcept -> visibility {
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
  explicit error_type_expr(source_span s = source_span::dummy())
      : type_expr(node_kind::named_type,
                  s) { // reuse kind; has_error flag distinguishes
    has_error = true;
  }
};

struct tuple_type : type_expr {
  std::vector<ptr<type_expr>> elements;

  tuple_type() : type_expr(node_kind::tuple_type) {}
};

struct slice_type : type_expr {
  ptr<type_expr> element;

  slice_type() : type_expr(node_kind::slice_type) {}
};

struct array_type : type_expr {
  ptr<type_expr> element;
  ptr<node> size; ///< An expression giving the array length.

  array_type() : type_expr(node_kind::array_type) {}
};

struct ref_type : type_expr {
  ptr<type_expr> inner;
  bool is_mut = false;

  ref_type() : type_expr(node_kind::ref_type) {}
};

struct ptr_type : type_expr {
  ptr<type_expr> inner;
  bool is_mut = false;

  ptr_type() : type_expr(node_kind::ptr_type) {}
};

struct fn_type : type_expr {
  std::vector<ptr<type_expr>> param_types;
  ptr<type_expr> return_type; ///< nullptr if no return type.

  fn_type() : type_expr(node_kind::fn_type) {}
};

struct quote_type : type_expr {
  token_kind quote_kind; ///< KwExpr, KwStmt, KwDefExpr, or KwTypeExpr

  explicit quote_type(token_kind qk = token_kind::kw_expr)
      : type_expr(node_kind::quote_type), quote_kind(qk) {}
};

struct union_type : type_expr {
  std::vector<ptr<type_expr>> alternatives;

  union_type() : type_expr(node_kind::union_type) {}
};

struct refinement_type : type_expr {
  ptr<type_expr> base;
  ptr<node> predicate; ///< The `where expr` part.

  refinement_type() : type_expr(node_kind::refinement_type) {}
};

// ==========================================================================
//  Type parameters and bounds
// ==========================================================================

struct type_param {
  source_span span;
  std::string name;
  /// For type params: an optional trait/concept bound.
  /// For value params: the type of the value.
  ptr<type_expr> bound_or_type;
  bool is_value_param = false; ///< True if this is `name : type` (value param).
  bool is_higher_kinded = false; ///< True if this is `Name[_]`.
};

struct type_arg {
  source_span span;
  /// A type argument is either a type expression or a value expression.
  /// We store both as Node* and disambiguate later in semantic analysis.
  ptr<node> value;
  std::optional<std::string> name; ///< Named argument: `name: type`
};

struct named_type : type_expr {
  std::vector<std::string> path; ///< e.g., ["std", "collections", "Map"]
  std::vector<type_arg> type_args; ///< Generic or value arguments, if any.

  named_type() : type_expr(node_kind::named_type) {}
};

struct bound_term {
  source_span span;
  /// A trait bound like `Trait[Args]` or a function type bound.
  ptr<type_expr> type;
};

struct bound {
  source_span span;
  std::vector<bound_term> terms; ///< Connected by `+`.
};

/// A bound list represented in a type-expression slot.
struct bound_type : type_expr {
  bound value;

  bound_type() : type_expr(node_kind::bound_type) {}
};

struct where_constraint {
  source_span span;
  ptr<type_expr> subject;
  /// Either a trait bound or an associated type equality constraint.
  ptr<type_expr> bound_or_type;
};

// ==========================================================================
//  Struct and sum type bodies
// ==========================================================================

struct struct_field {
  source_span span;
  visibility visibility = visibility::def;
  std::string name;
  ptr<type_expr> type;
};

struct struct_body {
  source_span span;
  std::vector<struct_field> fields;
};

struct sum_variant {
  source_span span;
  std::string name;
  std::vector<ptr<type_expr>> payload_types; ///< Empty for unit variants.
};

struct sum_body {
  source_span span;
  std::vector<sum_variant> variants;
};

// ==========================================================================
//  Expression nodes
// ==========================================================================

struct expr : node {
  using node::node;
};

/// A placeholder expression for error recovery.
struct error_expr : expr {
  std::string description;

  explicit error_expr(source_span s = source_span::dummy(), std::string desc = "")
      : expr(node_kind::ident_expr, s), description(std::move(desc)) {
    has_error = true;
  }
};

struct ident_expr : expr {
  std::string name;

  ident_expr() : expr(node_kind::ident_expr) {}
};

struct literal_expr : expr {
  token_kind lit_kind; ///< IntLit, FloatLit, StringLit, CharLit, KwTrue,
                       ///< KwFalse, KwUnit
  std::string value;   ///< The raw text of the literal.

  literal_expr() : expr(node_kind::literal_expr) {}
};

enum class binary_op : uint8_t {
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

[[nodiscard]] inline auto binary_op_name(binary_op op) noexcept -> std::string_view {
  switch (op) {
  case binary_op::Pipe:
    return "|";
  case binary_op::Or:
    return "or";
  case binary_op::And:
    return "and";
  case binary_op::EqEq:
    return "==";
  case binary_op::BangEq:
    return "!=";
  case binary_op::Lt:
    return "<";
  case binary_op::LtEq:
    return "<=";
  case binary_op::Gt:
    return ">";
  case binary_op::GtEq:
    return ">=";
  case binary_op::In:
    return "in";
  case binary_op::NotIn:
    return "not in";
  case binary_op::Add:
    return "+";
  case binary_op::Sub:
    return "-";
  case binary_op::Mul:
    return "*";
  case binary_op::Div:
    return "/";
  case binary_op::Mod:
    return "%";
  case binary_op::AddWrap:
    return "+%";
  case binary_op::SubWrap:
    return "-%";
  case binary_op::MulWrap:
    return "*%";
  case binary_op::AddSat:
    return "+|";
  case binary_op::SubSat:
    return "-|";
  case binary_op::MulSat:
    return "*|";
  case binary_op::Shl:
    return "<<";
  case binary_op::Shr:
    return ">>";
  case binary_op::BitAnd:
    return "&";
  case binary_op::BitOr:
    return "|";
  case binary_op::BitXor:
    return "^";
  case binary_op::Range:
    return "..";
  case binary_op::RangeInclusive:
    return "..=";
  }
  return "<?>";
}

struct binary_expr : expr {
  binary_op op;
  ptr<expr> lhs;
  ptr<expr> rhs;

  binary_expr() : expr(node_kind::binary_expr) {}
};

enum class unary_op : uint8_t {
  Neg,       ///< `-`
  BitNot,    ///< `~`
  Deref,     ///< `*`
  AddrOf,    ///< `&`
  AddrOfMut, ///< `&mut`
  Not,       ///< `not`
};

[[nodiscard]] inline auto unary_op_name(unary_op op) noexcept -> std::string_view {
  switch (op) {
  case unary_op::Neg:
    return "-";
  case unary_op::BitNot:
    return "~";
  case unary_op::Deref:
    return "*";
  case unary_op::AddrOf:
    return "&";
  case unary_op::AddrOfMut:
    return "&mut";
  case unary_op::Not:
    return "not";
  }
  return "<?>";
}

struct unary_expr : expr {
  unary_op op;
  ptr<expr> operand;

  unary_expr() : expr(node_kind::unary_expr) {}
};

/// Field access: `expr.name`
struct field_expr : expr {
  ptr<expr> object;
  std::string field_name;
  std::vector<ptr<type_expr>> generic_args; ///< For `expr.method[T]`

  field_expr() : expr(node_kind::field_expr) {}
};

/// Index: `expr[index]`
struct index_expr : expr {
  ptr<expr> object;
  ptr<expr> index;

  index_expr() : expr(node_kind::index_expr) {}
};

/// Function/method call: `expr(args...)`
struct call_arg {
  source_span span;
  std::optional<std::string> name; ///< Named arg: `name: value`
  ptr<expr> value;
};

struct call_expr : expr {
  ptr<expr> callee;
  std::vector<call_arg> args;

  call_expr() : expr(node_kind::call_expr) {}
};

/// Type cast: `expr as Type`
struct cast_expr : expr {
  ptr<expr> operand;
  ptr<type_expr> target_type;

  cast_expr() : expr(node_kind::cast_expr) {}
};

/// Try propagation: `expr?` or `expr?.field`
struct try_expr : expr {
  ptr<expr> operand;

  try_expr() : expr(node_kind::try_expr) {}
};

/// Tuple literal: `(a, b, c)`
struct tuple_expr : expr {
  std::vector<ptr<expr>> elements;

  tuple_expr() : expr(node_kind::tuple_expr) {}
};

/// Array literal: `[a, b, c]` or fill: `[val; count]`
struct array_expr : expr {
  std::vector<ptr<expr>> elements;
  ptr<expr> fill_value; ///< Non-null for `[val; count]` form.
  ptr<expr> fill_count;

  array_expr() : expr(node_kind::array_expr) {}
};

/// Struct literal field: `name: expr` or shorthand `name`
struct struct_field_init {
  source_span span;
  std::string name;
  ptr<expr> value; ///< nullptr for shorthand `{x}` (means `{x: x}`)
};

/// Struct literal: `{a: 1, b: 2}`
struct struct_expr : expr {
  ptr<expr> type_name; ///< Optional typed form: `Point { x: 1 }`
  std::vector<struct_field_init> fields;

  struct_expr() : expr(node_kind::struct_expr) {}
};

/// Lambda / closure: `x => x + 1` or `(a, b) -> int => a + b`
struct lambda_param {
  source_span span;
  ptr<node> pattern;              ///< The parameter pattern.
  ptr<type_expr> type_annotation; ///< Optional type annotation.
};

struct lambda_expr : expr {
  bool is_pure = false;
  bool is_move = false;
  std::vector<lambda_param> params;
  ptr<type_expr> return_type; ///< Optional return type annotation.
  /// Body is either a single expression or a block (vector of statements).
  ptr<expr> body_expr;
  std::vector<ptr<node>> body_stmts;

  lambda_expr() : expr(node_kind::lambda_expr) {}
};

/// Module path expression: `a.b.c`
struct module_path_expr : expr {
  std::vector<std::string> segments;

  module_path_expr() : expr(node_kind::module_path_expr) {}
};

/// Parenthesized expression: `(expr)`
struct group_expr : expr {
  ptr<expr> inner;

  group_expr() : expr(node_kind::group_expr) {}
};

/// `if expr: ... elif ...: ... else: ...` (expression form)
struct if_branch {
  source_span span;
  ptr<expr> condition;
  ptr<node> let_pattern; ///< Non-null for `if let` / `elif let`.
  ptr<expr> let_expr;    ///< The expression in `if let pat = expr`.
  std::vector<ptr<node>> body; ///< Block of statements/exprs.
};

struct if_expr : expr {
  std::vector<if_branch> branches; ///< `if` + zero or more `elif`
  std::vector<ptr<node>> else_body;

  if_expr() : expr(node_kind::if_expr) {}
};

/// `match subject: ...`
struct match_arm {
  source_span span;
  ptr<node> pattern;
  ptr<expr> guard; ///< Optional `if expr` guard.
  /// Body: inline expression or block.
  ptr<expr> body_expr;
  std::vector<ptr<node>> body_stmts;
  bool has_error = false;
};

struct match_expr : expr {
  ptr<expr> subject;
  std::vector<match_arm> arms;

  match_expr() : expr(node_kind::match_expr) {}
};

/// `for vars in iter [if guard] => yield_expr`
struct for_expr : expr {
  struct iter_clause {
    std::vector<ptr<node>> patterns;
    ptr<expr> iterable;
  };
  std::vector<iter_clause> clauses;
  ptr<expr> guard;      ///< Optional `if` filter.
  ptr<expr> yield_expr; ///< The `=> expr` part.

  for_expr() : expr(node_kind::for_expr) {}
};

/// `await expr` or `await yield`
struct await_expr : expr {
  ptr<expr> operand; ///< nullptr for `await yield`.
  bool is_yield = false;

  await_expr() : expr(node_kind::await_expr) {}
};

/// `async: ...`
struct async_expr : expr {
  std::vector<ptr<node>> body;

  async_expr() : expr(node_kind::async_expr) {}
};

/// `par: ...`
struct par_expr : expr {
  std::vector<ptr<expr>> branches;

  par_expr() : expr(node_kind::par_expr) {}
};

/// `race: ...`
struct race_expr : expr {
  std::vector<ptr<expr>> branches;

  race_expr() : expr(node_kind::race_expr) {}
};

/// `on(Type): block` or `on(Type, sender)`
struct on_expr : expr {
  ptr<type_expr> context_type;
  ptr<expr> sender; ///< Optional second argument.
  std::vector<ptr<node>> body;

  on_expr() : expr(node_kind::on_expr) {}
};

/// `block_expr` — `INDENT stmts DEDENT`
struct block_expr : expr {
  std::vector<ptr<node>> stmts;

  block_expr() : expr(node_kind::block_expr) {}
};

/// Quasi-quote: `` `(content)` `` or `` `content` ``
struct quote_expr : expr {
  std::vector<token> tokens; ///< Raw tokens inside the quote.

  quote_expr() : expr(node_kind::quote_expr) {}
};

/// Splice: `~(expr)` or `~ident`
struct splice_expr : expr {
  ptr<expr> operand;

  splice_expr() : expr(node_kind::splice_expr) {}
};

/// `static expr`
struct static_expr : expr {
  ptr<expr> operand;
  static_expr() : expr(node_kind::static_expr) {}
};

/// A single binding inside a `where` clause: `name = expr`
struct where_binding {
  source_span span;
  std::string name;  ///< The bound identifier.
  ptr<expr> value;   ///< The right-hand side expression.
};

/// `expr where: INDENT name = expr ... DEDENT`
///
/// Attaches locally-scoped immutable bindings to any expression.
/// The bindings are evaluated in order and are not mutually recursive.
///
///   let x = do_something(a, b, c) where:
///               a = find_a_thing(v1, v2)
///               b = substr(v2, 5)
///               c = compute_a_thing(v, 10)
struct where_expr : expr {
  ptr<expr> inner;                       ///< The wrapped expression.
  std::vector<where_binding> bindings;   ///< The where bindings, in order.

  where_expr() : expr(node_kind::where_expr) {}
};

// ==========================================================================
//  Pattern nodes
// ==========================================================================

struct pattern : node {
  using node::node;
};

struct error_pattern : pattern {
  explicit error_pattern(source_span s = source_span::dummy())
      : pattern(node_kind::wildcard_pattern, s) {
    has_error = true;
  }
};

/// `_`
struct wildcard_pattern : pattern {
  wildcard_pattern() : pattern(node_kind::wildcard_pattern) {}
};

/// Literal in pattern position: `42`, `"hello"`, `true`
struct literal_pattern : pattern {
  token_kind lit_kind;
  std::string value;

  literal_pattern() : pattern(node_kind::literal_pattern) {}
};

/// Simple name binding: `x`
struct binding_pattern : pattern {
  std::string name;

  binding_pattern() : pattern(node_kind::binding_pattern) {}
};

/// Constructor: `Some(x)`, `Point(a, b)`
struct constructor_pattern : pattern {
  std::string name;
  std::vector<ptr<pattern>> args;

  constructor_pattern() : pattern(node_kind::constructor_pattern) {}
};

/// Tuple: `(a, b, c)`
struct tuple_pattern : pattern {
  std::vector<ptr<pattern>> elements;

  tuple_pattern() : pattern(node_kind::tuple_pattern) {}
};

/// Struct pattern field: `name: pattern`, `name`, or `..`
struct field_pattern {
  source_span span;
  std::string name;     ///< Empty for `..` (rest).
  ptr<pattern> pattern; ///< nullptr for shorthand `{name}`.
  bool is_rest = false; ///< True for `..`.
};

/// Struct pattern: `{x: a, y: b, ..}`
struct struct_pattern : pattern {
  std::vector<field_pattern> fields;

  struct_pattern() : pattern(node_kind::struct_pattern) {}
};

/// Array/slice pattern: `[a, b, c]`
struct array_pattern : pattern {
  std::vector<ptr<pattern>> elements;

  array_pattern() : pattern(node_kind::array_pattern) {}
};

/// Range: `1..10`, `1..=10`, `1..`, `..10`
struct range_pattern : pattern {
  ptr<expr> start;        ///< nullptr for `..end`
  ptr<expr> end;          ///< nullptr for `start..`
  bool inclusive = false; ///< True for `..=`

  range_pattern() : pattern(node_kind::range_pattern) {}
};

/// `some(pattern)`, `ok(pattern)`, `err(pattern)`
enum class OptionResultKind : uint8_t { Some, Ok, Err };

struct option_pattern : pattern {
  OptionResultKind option_kind;
  ptr<pattern> inner;

  option_pattern() : pattern(node_kind::option_pattern) {}
};

/// Result pattern — reuses option_pattern but with different kind.
struct result_pattern : pattern {
  OptionResultKind result_kind; ///< Ok or Err
  ptr<pattern> inner;

  result_pattern() : pattern(node_kind::result_pattern) {}
};

/// `&pattern`
struct ref_pattern : pattern {
  ptr<pattern> inner;

  ref_pattern() : pattern(node_kind::ref_pattern) {}
};

/// `a | b | c`
struct or_pattern : pattern {
  std::vector<ptr<pattern>> alternatives;

  or_pattern() : pattern(node_kind::or_pattern) {}
};

/// `(pattern)` — parenthesized pattern for grouping.
struct group_pattern : pattern {
  ptr<pattern> inner;
  std::optional<std::string> alias;

  group_pattern() : pattern(node_kind::group_pattern) {}
};

// ==========================================================================
//  Statement nodes
// ==========================================================================

struct stmt : node {
  using node::node;
};

struct error_stmt : stmt {
  explicit error_stmt(source_span s = source_span::dummy()) : stmt(node_kind::expr_stmt, s) {
    has_error = true;
  }
};

/// `let pattern [: type] = expr`
/// `let pattern [: type] = expr else: block`
struct let_stmt : stmt {
  ptr<pattern> pattern;
  ptr<type_expr> type_annotation; ///< Optional.
  ptr<expr> initializer;
  std::vector<ptr<node>> else_body; ///< Non-empty for `let ... else:` form.

  let_stmt() : stmt(node_kind::let_stmt) {}
};

/// `var name [: type] = expr`
struct var_stmt : stmt {
  std::string name;
  ptr<type_expr> type_annotation; ///< Optional.
  ptr<expr> initializer;

  var_stmt() : stmt(node_kind::var_stmt) {}
};

/// Assignment operators.
enum class assign_op : uint8_t {
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

struct assign_stmt : stmt {
  ptr<expr> target;
  assign_op op;
  ptr<expr> value;

  assign_stmt() : stmt(node_kind::assign_stmt) {}
};

/// Expression used as a statement.
struct expr_stmt : stmt {
  ptr<expr> expr;

  expr_stmt() : stmt(node_kind::expr_stmt) {}
};

/// `return [expr]`
struct return_stmt : stmt {
  ptr<expr> value; ///< nullptr if bare `return`.

  return_stmt() : stmt(node_kind::return_stmt) {}
};

/// `if expr: block { elif expr: block } [else: block]`
struct if_stmt : stmt {
  std::vector<if_branch> branches;
  std::vector<ptr<node>> else_body;

  if_stmt() : stmt(node_kind::if_stmt) {}
};

/// `while expr: block` or `while let pattern = expr: block`
struct while_stmt : stmt {
  ptr<expr> condition;      ///< nullptr for `while let` form.
  ptr<pattern> let_pattern; ///< Non-null for `while let` form.
  ptr<expr> let_expr;       ///< The expr in `while let pat = expr`.
  std::vector<ptr<node>> body;

  while_stmt() : stmt(node_kind::while_stmt) {}
};

/// `for vars in expr [if guard]: block`
struct for_stmt : stmt {
  std::vector<ptr<pattern>> patterns;
  ptr<expr> iterable;
  ptr<expr> guard; ///< Optional `if` filter.
  std::vector<ptr<node>> body;

  for_stmt() : stmt(node_kind::for_stmt) {}
};

/// `match subject: ...`
struct match_stmt : stmt {
  ptr<expr> subject;
  std::vector<match_arm> arms;

  match_stmt() : stmt(node_kind::match_stmt) {}
};

/// Crew option: `name: value`
struct crew_option {
  source_span span;
  std::string name;
  std::string value;
};

/// `crew name(options): block` in expression position.
struct crew_expr : expr {
  std::string name;
  std::vector<crew_option> options;
  std::vector<ptr<node>> body;

  crew_expr() : expr(node_kind::crew_expr) {}
};

/// `crew name(options): block`
struct crew_stmt : stmt {
  std::string name;
  std::vector<crew_option> options;
  std::vector<ptr<node>> body;

  crew_stmt() : stmt(node_kind::crew_stmt) {}
};

/// `asm { content }`
struct asm_stmt : stmt {
  std::string content;

  asm_stmt() : stmt(node_kind::asm_stmt) {}
};

/// `~expr`
struct splice_stmt : stmt {
  ptr<expr> expr;

  splice_stmt() : stmt(node_kind::splice_stmt) {}
};

// ==========================================================================
//  Declaration / item nodes
// ==========================================================================

/// `use` path handling.
struct use_item {
  source_span span;
  std::string name;
  std::optional<std::string> alias; ///< `as` alias.
};

enum class UseSelectorKind : uint8_t {
  Single,   ///< `use a.b.Foo` or `use a.b.Foo as Bar`
  Group,    ///< `use a.b.{Foo, Bar}`
  Wildcard, ///< `use a.b.*`
};

struct use_selector {
  source_span span;
  UseSelectorKind kind;
  std::vector<use_item> items; ///< For group selector; single has one item.
};

struct use_decl : node {
  visibility visibility = visibility::def;
  std::vector<std::string> path;
  std::optional<use_selector> selector;

  use_decl() : node(node_kind::use_decl) {}
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
  ptr<expr> invariant;

  type_decl() : node(node_kind::type_decl) {}
};

struct struct_type_def : node {
  struct_body body;

  struct_type_def() : node(node_kind::struct_type_def) {}
};

struct sum_type_def : node {
  sum_body body;

  sum_type_def() : node(node_kind::sum_type_def) {}
};

/// Associated type in a trait: `type Name [= Default]`
struct associated_type_decl {
  source_span span;
  visibility visibility = visibility::def;
  std::string name;
  ptr<type_expr> default_type;
};

struct associated_type_decl_node : node {
  associated_type_decl value;

  associated_type_decl_node()
      : node(node_kind::associated_type_decl_node) {}
};

/// Associated type in an impl: `type Name = ConcreteType`
struct associated_type_def {
  source_span span;
  std::string name;
  ptr<type_expr> type;
};

struct associated_type_def_node : node {
  associated_type_def value;

  associated_type_def_node()
      : node(node_kind::associated_type_def_node) {}
};

/// Trait declaration.
struct trait_decl : node {
  visibility visibility = visibility::def;
  std::string name;
  std::vector<type_param> type_params;
  std::optional<bound> requires_bound;
  std::vector<ptr<node>>
      items; ///< func_decl, static_decl, associated_type_decl nodes.

  trait_decl() : node(node_kind::trait_decl) {}
};

/// Concept declaration.
struct concept_param {
  source_span span;
  std::string name;
  bool is_higher_kinded = false; ///< `Name[_]`
};

struct concept_constraint {
  source_span span;
  ptr<type_expr> subject;  ///< Non-null for `type : bound` constraints.
  ptr<node> bound_or_expr; ///< Bound for type constraints, expr for value
                           ///< constraints.
};

struct concept_decl : node {
  visibility visibility = visibility::def;
  std::string name;
  std::vector<concept_param> params;
  std::vector<concept_constraint> constraints;

  concept_decl() : node(node_kind::concept_decl) {}
};

/// Impl declaration.
struct impl_decl : node {
  std::vector<type_param> type_params;
  ptr<type_expr> trait_type;
  ptr<type_expr> for_type;
  std::vector<where_constraint> where_constraints;
  std::vector<ptr<node>>
      items; ///< func_decl, static_decl, associated_type_def.

  impl_decl() : node(node_kind::impl_decl) {}
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
  source_span span;
  bool is_pre; ///< true = `pre`, false = `post`
  ptr<expr> condition;
  std::optional<std::string> message;
};

/// Parameter in a function signature.
struct Param {
  source_span span;
  ptr<pattern> pattern;
  ptr<type_expr> type_annotation; ///< Optional.
  ptr<expr> default_value;        ///< Optional.
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
  ptr<expr> body_expr;
  std::vector<ptr<node>> body_stmts;

  func_decl() : node(node_kind::func_decl) {}
};

/// Sub-module: `module Name` or `module Name: ...`
struct sub_module_decl : node {
  visibility visibility = visibility::def;
  std::string name;
  std::vector<ptr<node>> items; ///< Empty for forward declarations.

  sub_module_decl() : node(node_kind::sub_module_decl) {}
};

/// Dependency declaration: `dep Name: ...`
struct dep_field {
  source_span span;
  std::string key;
  std::string value;
};

struct dep_decl : node {
  std::string name;
  std::vector<dep_field> fields;

  dep_decl() : node(node_kind::dep_decl) {}
};

/// Static declarations — multiple forms:
///   - `static if expr: ...`
///   - `static Name : Type = expr`
///   - `static assert expr [, message]`
///   - `static for vars in expr [if guard] => expr`
///   - `static for vars in expr [if guard]: block`
enum class static_decl_kind : uint8_t {
  conditional_compilation, ///< `static if`
  binding,                ///< `static Name = expr`
  assertion,                 ///< `static assert expr`
  for_inline,              ///< `static for ... => expr`
  for_block,               ///< `static for ... : block`
};

struct static_decl : node {
  visibility visibility = visibility::def;
  static_decl_kind decl_kind{static_decl_kind::binding};

  // For Binding:
  std::string name;
  ptr<type_expr> type_annotation;
  ptr<expr> initializer;

  // For Assert:
  ptr<expr> assert_condition;
  std::optional<std::string> assert_message;

  // For ConditionalCompilation:
  ptr<expr> if_condition;
  std::vector<ptr<node>> if_body;
  std::vector<ptr<node>> else_body;

  // For ForInline / ForBlock:
  std::vector<ptr<pattern>> for_patterns;
  ptr<expr> for_iterable;
  ptr<expr> for_guard;
  ptr<expr> for_yield;             ///< For ForInline.
  std::vector<ptr<node>> for_body; ///< For ForBlock.

  static_decl()
      : node(node_kind::static_decl) {}
};

// ==========================================================================
//  Module declaration and file root
// ==========================================================================

struct module_decl : node {
  std::vector<std::string> path;

  module_decl() : node(node_kind::module_decl) {}
};

struct file : node {
  ptr<module_decl> module_decl;
  bool no_prelude = false;
  std::vector<ptr<node>> items;

  file() : node(node_kind::file_node) {}
};

// ==========================================================================
//  Pattern alias wrapper — for `pattern as name`
// ==========================================================================

struct aliased_pattern {
  ptr<pattern> pattern;
  std::optional<std::string> alias;
  source_span span;
};

} // namespace kira::ast
