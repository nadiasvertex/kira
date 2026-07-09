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
struct extend_decl;
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
struct param;
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
struct postfix_expr;
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

/// @brief Canonical owning pointer for AST nodes and helper records.
///
/// The tree is intentionally pointer-shaped so recursive syntax stays easy to
/// model and moving subtrees between phases remains explicit.
template <typename T> using ptr = std::unique_ptr<T>;

/// @brief Common container for owning a homogeneous list of AST nodes.
template <typename T> using ptr_vec = std::vector<ptr<T>>;

/// @brief Constructs an AST node with ownership wrapped in `ptr<T>`.
///
/// This keeps AST construction uniform across the parser and later
/// tree-building passes.
///
/// @tparam T Concrete node type to allocate.
/// @tparam Args Constructor argument pack forwarded to `T`.
template <typename T, typename... Args>
[[nodiscard]] auto make(Args &&...args) -> ptr<T> {
  return std::make_unique<T>(std::forward<Args>(args)...);
}

// ==========================================================================
//  NodeKind — tag for downcasting via the base class. Every concrete
//  node has a unique kind value.
// ==========================================================================

/// @brief Stable runtime tag for every concrete AST node category.
///
/// Later phases can use this for lightweight dispatch without paying for RTTI
/// or exposing parser-only type knowledge at API boundaries.
enum class node_kind : uint8_t {
  // Sentinel
  error_node, ///< Recovery placeholder used when no concrete syntax node
              ///< parsed.

  // Top-level
  file_node,   ///< Root syntax tree for one source file.
  module_decl, ///< File-level module declaration.

  // Items
  use_decl,                  ///< Import declaration item.
  type_decl,                 ///< User-defined type declaration.
  struct_type_def,           ///< Struct-style type body wrapper.
  sum_type_def,              ///< Sum-type body wrapper.
  trait_decl,                ///< Trait declaration item.
  impl_decl,                 ///< Implementation block item.
  extend_decl,               ///< Extension-method block item.
  concept_decl,              ///< Concept declaration item.
  func_decl,                 ///< Function declaration item.
  sub_module_decl,           ///< Nested module declaration item.
  dep_decl,                  ///< Dependency metadata item.
  static_decl,               ///< Compile-time declaration item.
  associated_type_decl_node, ///< Trait associated-type declaration wrapper.
  associated_type_def_node,  ///< Impl associated-type definition wrapper.
  splice_stmt,               ///< Statement-position splice node.

  // Types
  named_type,      ///< Named type path with optional arguments.
  bound_type,      ///< Bound list adapted into a type slot.
  tuple_type,      ///< Tuple type expression.
  slice_type,      ///< Slice type expression.
  array_type,      ///< Fixed-length array type expression.
  ref_type,        ///< Reference type expression.
  ptr_type,        ///< Raw pointer type expression.
  fn_type,         ///< Function type expression.
  quote_type,      ///< Quote-type marker for syntax values.
  union_type,      ///< Union-style type expression.
  refinement_type, ///< Refinement type with predicate clause.

  // Statements
  let_stmt,    ///< Immutable binding statement.
  var_stmt,    ///< Mutable binding statement.
  assign_stmt, ///< Assignment statement.
  expr_stmt,   ///< Expression used for side effects.
  return_stmt, ///< Function return statement.
  if_stmt,     ///< Conditional statement.
  while_stmt,  ///< While-loop statement.
  for_stmt,    ///< For-loop statement.
  match_stmt,  ///< Match statement.
  crew_stmt,   ///< Crew orchestration statement.
  asm_stmt,    ///< Inline assembly statement.

  // Expressions
  ident_expr,       ///< Identifier expression.
  literal_expr,     ///< Literal expression.
  binary_expr,      ///< Binary operator expression.
  unary_expr,       ///< Unary operator expression.
  postfix_expr,     ///< Reserved historical tag for postfix expressions.
  call_expr,        ///< Call expression.
  index_expr,       ///< Indexing expression.
  field_expr,       ///< Field access expression.
  cast_expr,        ///< Explicit cast expression.
  try_expr,         ///< Try-propagation expression.
  tuple_expr,       ///< Tuple literal expression.
  array_expr,       ///< Array literal expression.
  struct_expr,      ///< Struct literal expression.
  lambda_expr,      ///< Lambda expression.
  match_expr,       ///< Match expression.
  if_expr,          ///< Conditional expression.
  for_expr,         ///< Comprehension or generator expression.
  await_expr,       ///< Await expression.
  async_expr,       ///< Async block expression.
  par_expr,         ///< Parallel-branch expression.
  race_expr,        ///< Racing-branch expression.
  crew_expr,        ///< Crew expression.
  on_expr,          ///< Context-bound execution expression.
  block_expr,       ///< Indentation-delimited block expression.
  quote_expr,       ///< Quoted syntax expression.
  splice_expr,      ///< Splice expression.
  static_expr,      ///< Compile-time evaluation expression.
  module_path_expr, ///< Module-path expression.
  group_expr,       ///< Parenthesized grouping expression.
  where_expr,       ///< Expression with trailing local bindings.

  // Patterns
  wildcard_pattern,    ///< Wildcard pattern.
  literal_pattern,     ///< Literal-matching pattern.
  binding_pattern,     ///< Identifier-binding pattern.
  constructor_pattern, ///< Constructor destructuring pattern.
  tuple_pattern,       ///< Tuple destructuring pattern.
  struct_pattern,      ///< Struct destructuring pattern.
  array_pattern,       ///< Array or slice destructuring pattern.
  range_pattern,       ///< Range-matching pattern.
  option_pattern,      ///< `some(...)` option pattern.
  result_pattern,      ///< `ok(...)` or `err(...)` result pattern.
  ref_pattern,         ///< Reference pattern.
  or_pattern,          ///< Alternative pattern.
  group_pattern,       ///< Parenthesized pattern grouping.
};

// ==========================================================================
//  Base classes — Node, type_expr, Stmt, Expr, Pattern
//
//  All AST nodes inherit from Node which carries a span and an error flag.
//  The error flag (`has_error`) is set when the parser used error recovery
//  to produce this node. Later phases can check it to skip semantic
//  analysis of broken subtrees — this prevents cascading errors.
// ==========================================================================

/// @brief Common base for every AST node.
///
/// The parser normalizes all concrete syntax into nodes carrying two universal
/// facts: what kind of syntax they represent and which source bytes they came
/// from. `has_error` marks recovered nodes that later phases should treat as
/// structurally useful but semantically unreliable.
struct node {
  node_kind kind;   ///< Runtime tag for downcasting and diagnostics.
  source_span span; ///< Source range covering the full syntactic construct.
  bool has_error =
      false; ///< Set when recovery synthesized or repaired this node.

  /// @brief Creates a node shell with kind and source coverage.
  ///
  /// @param k Concrete runtime kind tag.
  /// @param s Source span covering the node.
  explicit node(node_kind k, source_span s = source_span::dummy())
      : kind(k), span(s) {}
  virtual ~node() = default;

  // Non-copyable, movable.
  node(const node &) = delete;
  auto operator=(const node &) -> node & = delete;
  node(node &&) = default;
  auto operator=(node &&) -> node & = default;
};

/// @brief Recovery placeholder used when parsing cannot form a concrete node.
///
/// Later phases should propagate or skip `error_node` subtrees rather than
/// attempting normal semantic work on them. The `description` field preserves
/// what the parser was trying to parse when recovery intervened.
struct error_node : node {
  std::string description; ///< What construct the parser failed to understand.

  /// @brief Creates an error placeholder for a failed parse region.
  ///
  /// @param s Source span where parsing failed.
  /// @param desc Short explanation of the missing or invalid construct.
  explicit error_node(source_span s = source_span::dummy(),
                      std::string desc = "")
      : node(node_kind::error_node, s), description(std::move(desc)) {
    has_error = true;
  }
};

// --------------------------------------------------------------------------
//  Visibility
// --------------------------------------------------------------------------

/// @brief Normalized visibility used by declaration nodes.
///
/// Parsing converts surface keywords into this enum immediately so later phases
/// can reason about access control without depending on token spellings.
enum class visibility : uint8_t {
  def, ///< No explicit modifier; use the language's default visibility rules.
  pub, ///< Publicly visible outside the defining module boundary.
  internal, ///< Visible within the current package or internal compilation
            ///< unit.
  super,    ///< Visible to the parent module scope.
  priv,     ///< Visible only within the immediately enclosing scope/module.
};

/// @brief Maps a visibility keyword token into the AST visibility enum.
///
/// Non-visibility tokens intentionally map to `visibility::def` so optional
/// visibility parsing can stay branch-light.
///
/// @param kind Token that may represent a visibility modifier.
[[nodiscard]] inline auto token_to_visibility(token_kind kind) noexcept
    -> visibility {
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

/// @brief Base class for all type-position syntax nodes.
///
/// Separating type syntax from value syntax lets later phases perform type- and
/// expression-specific traversals without rechecking every concrete node kind.
struct type_expr : node {
  using node::node;
};

/// A type expression that the parser couldn't understand. Stands in for
/// the real thing so the tree remains structurally valid.
struct error_type_expr : type_expr {
  /// @brief Creates a placeholder type node after type parsing fails.
  ///
  /// @param s Source span where type parsing failed.
  explicit error_type_expr(source_span s = source_span::dummy())
      : type_expr(node_kind::named_type,
                  s) { // reuse kind; has_error flag distinguishes
    has_error = true;
  }
};

/// @brief Tuple type syntax such as `(A, B, C)`.
struct tuple_type : type_expr {
  std::vector<ptr<type_expr>> elements; ///< Element types in source order.

  tuple_type() : type_expr(node_kind::tuple_type) {}
};

/// @brief Slice type syntax for borrowed contiguous sequences.
struct slice_type : type_expr {
  ptr<type_expr> element; ///< Element type shared by all slice members.

  slice_type() : type_expr(node_kind::slice_type) {}
};

/// @brief Fixed-length array type syntax.
struct array_type : type_expr {
  ptr<type_expr> element; ///< Element type stored in the array.
  ptr<node> size;         ///< Length expression preserved for later evaluation.

  array_type() : type_expr(node_kind::array_type) {}
};

/// @brief Reference type syntax.
struct ref_type : type_expr {
  ptr<type_expr> inner; ///< Referenced type.
  bool is_mut = false;  ///< Whether the reference grants mutation.

  ref_type() : type_expr(node_kind::ref_type) {}
};

/// @brief Raw pointer type syntax.
struct ptr_type : type_expr {
  ptr<type_expr> inner; ///< Pointed-to type.
  bool is_mut =
      false; ///< Whether the pointed-to value is mutable through the pointer.

  ptr_type() : type_expr(node_kind::ptr_type) {}
};

/// @brief Function type syntax with parameter and return types.
struct fn_type : type_expr {
  std::vector<ptr<type_expr>>
      param_types; ///< Parameter types in declaration order.
  ptr<type_expr>
      return_type; ///< Optional return type; null means implicit unit.

  fn_type() : type_expr(node_kind::fn_type) {}
};

/// @brief Type-level representation of quoted syntax categories.
struct quote_type : type_expr {
  token_kind quote_kind; ///< Which syntax category this quoted value contains.

  /// @brief Creates a quote-type marker.
  ///
  /// @param qk Quote keyword describing the syntax category.
  explicit quote_type(token_kind qk = token_kind::kw_expr)
      : type_expr(node_kind::quote_type), quote_kind(qk) {}
};

/// @brief Union-style type expression listing alternative accepted types.
struct union_type : type_expr {
  std::vector<ptr<type_expr>>
      alternatives; ///< Candidate alternative member types.

  union_type() : type_expr(node_kind::union_type) {}
};

/// @brief Type refined by a predicate expression.
///
/// Later semantic phases should interpret `predicate` in a type-checking or
/// proof-oriented context rather than as an ordinary runtime expression.
struct refinement_type : type_expr {
  ptr<type_expr> base; ///< Underlying type being constrained.
  ptr<node>
      predicate; ///< Predicate syntax attached by the trailing `where` clause.

  refinement_type() : type_expr(node_kind::refinement_type) {}
};

// ==========================================================================
//  Type parameters and bounds
// ==========================================================================

/// @brief Parsed generic parameter, which may itself be a type or value slot.
///
/// The parser preserves both forms in one record because later phases decide
/// how a parameter participates in generic substitution and constraint
/// checking.
struct type_param {
  source_span span; ///< Full source coverage of the parameter declaration.
  std::string name; ///< Parameter name as written.
  /// For type params: an optional trait/concept bound.
  /// For value params: the type of the value.
  ptr<type_expr>
      bound_or_type; ///< Bound or value type depending on parameter form.
  bool is_value_param =
      false; ///< True when the parameter binds a compile-time value slot.
  bool is_higher_kinded =
      false; ///< True for higher-kinded parameters like `Name[_]`.
};

/// @brief One supplied generic argument in a named-type application.
///
/// Arguments stay intentionally weakly typed at parse time because the grammar
/// permits both type and value arguments in similar surface forms.
struct type_arg {
  source_span span; ///< Full source range of the argument.
  /// A type argument is either a type expression or a value expression.
  /// We store both as Node* and disambiguate later in semantic analysis.
  ptr<node>
      value; ///< Parsed argument payload awaiting semantic classification.
  std::optional<std::string> name; ///< Named-argument label when present.
};

/// @brief Named type reference with optional generic or value arguments.
struct named_type : type_expr {
  std::vector<std::string>
      path; ///< Qualified type path, e.g. `std.collections.Map`.
  std::vector<type_arg>
      type_args; ///< Generic or value arguments attached to the path.

  named_type() : type_expr(node_kind::named_type) {}
};

/// @brief One term in a `+`-joined bound list.
struct bound_term {
  source_span span; ///< Source range of the bound term.
  /// A trait bound like `Trait[Args]` or a function type bound.
  ptr<type_expr>
      type; ///< Syntax describing the required capability or callable shape.
};

/// @brief Normalized representation of a trait/concept bound list.
struct bound {
  source_span span; ///< Source range of the full bound expression.
  std::vector<bound_term>
      terms; ///< Required terms joined by `+` in source order.
};

/// A bound list represented in a type-expression slot.
struct bound_type : type_expr {
  bound value;

  bound_type() : type_expr(node_kind::bound_type) {}
};

/// @brief Constraint from a trailing `where` clause.
///
/// These are kept as syntax-level records so semantic analysis can later decide
/// whether each clause is a trait requirement, associated-type equality, or
/// some richer relation.
struct where_constraint {
  source_span span;       ///< Full source range of the clause.
  ptr<type_expr> subject; ///< Thing being constrained.
  /// Either a trait bound or an associated type equality constraint.
  ptr<type_expr>
      bound_or_type; ///< Constraint payload interpreted by later phases.
};

// ==========================================================================
//  Struct and sum type bodies
// ==========================================================================

/// @brief One field in a struct-style type definition.
struct struct_field {
  source_span span; ///< Source range of the full field declaration.
  visibility visibility = visibility::def; ///< Field-level visibility modifier.
  std::string name;                        ///< Field name.
  ptr<type_expr> type;                     ///< Declared field type.
};

/// @brief Struct-style type body stored as a reusable helper record.
struct struct_body {
  source_span span;                 ///< Source range of the field list.
  std::vector<struct_field> fields; ///< Fields in declaration order.
};

/// @brief One variant inside a sum type definition.
struct sum_variant {
  source_span span; ///< Source range of the variant declaration.
  std::string name; ///< Variant constructor name.
  std::vector<ptr<type_expr>>
      payload_types; ///< Payload member types; empty for unit variants.
};

/// @brief Sum-type body stored as a reusable helper record.
struct sum_body {
  source_span span;                  ///< Source range of the variant list.
  std::vector<sum_variant> variants; ///< Variants in declaration order.
};

// ==========================================================================
//  Expression nodes
// ==========================================================================

/// @brief Base class for all value-position syntax nodes.
struct expr : node {
  using node::node;
};

/// A placeholder expression for error recovery.
struct error_expr : expr {
  std::string description; ///< What expression shape recovery expected here.

  /// @brief Creates a placeholder expression after parse recovery.
  ///
  /// @param s Source span where expression parsing failed.
  /// @param desc Short explanation of the missing or malformed expression.
  explicit error_expr(source_span s = source_span::dummy(),
                      std::string desc = "")
      : expr(node_kind::ident_expr, s), description(std::move(desc)) {
    has_error = true;
  }
};

/// @brief Identifier expression before name resolution.
struct ident_expr : expr {
  std::string name; ///< Raw identifier spelling to resolve later.

  ident_expr() : expr(node_kind::ident_expr) {}
};

/// @brief Literal expression preserving the original token spelling.
struct literal_expr : expr {
  token_kind lit_kind =
      token_kind::eof; ///< Literal token category chosen by the lexer.
  std::string
      value; ///< Raw source spelling, kept for exact later interpretation.

  literal_expr() : expr(node_kind::literal_expr) {}
};

/// @brief Normalized binary operators produced by expression and pattern
/// parsing.
///
/// Surface syntax maps onto this enum so later phases can reason about operator
/// semantics without carrying token kinds around.
enum class binary_op : uint8_t {
  // Pipe
  pipe, ///< Pipeline or low-precedence `|` operator.
  // Logical
  logical_or,  ///< Logical disjunction.
  logical_and, ///< Logical conjunction.
  // Comparison
  eq_eq,   ///< Equality comparison.
  bang_eq, ///< Inequality comparison.
  lt,      ///< Less-than comparison.
  lt_eq,   ///< Less-than-or-equal comparison.
  gt,      ///< Greater-than comparison.
  gt_eq,   ///< Greater-than-or-equal comparison.
  in,      ///< Membership comparison.
  not_in,  ///< Negated membership comparison.
  // Arithmetic
  add, ///< Addition.
  sub, ///< Subtraction.
  mul, ///< Multiplication.
  div, ///< Division.
  mod, ///< Remainder.
  // Wrapping
  add_wrap, ///< Wrapping addition.
  sub_wrap, ///< Wrapping subtraction.
  mul_wrap, ///< Wrapping multiplication.
  // Saturating
  add_sat, ///< Saturating addition.
  sub_sat, ///< Saturating subtraction.
  mul_sat, ///< Saturating multiplication.
  // Shifts
  shl, ///< Left shift.
  shr, ///< Right shift.
  // Bitwise
  bit_and, ///< Bitwise conjunction.
  bit_or,  ///< Bitwise disjunction.
  bit_xor, ///< Bitwise exclusive-or.
  // Range (used in patterns)
  range,           ///< Half-open range operator.
  range_inclusive, ///< Inclusive range operator.
};

/// @brief Returns the canonical surface spelling for a binary operator.
///
/// This is primarily for diagnostics and debugging; semantic code should switch
/// on the enum directly.
///
/// @param op Binary operator to describe.
[[nodiscard]] inline auto binary_op_name(binary_op op) noexcept
    -> std::string_view {
  switch (op) {
  case binary_op::pipe:
    return "|";
  case binary_op::logical_or:
    return "or";
  case binary_op::logical_and:
    return "and";
  case binary_op::eq_eq:
    return "==";
  case binary_op::bang_eq:
    return "!=";
  case binary_op::lt:
    return "<";
  case binary_op::lt_eq:
    return "<=";
  case binary_op::gt:
    return ">";
  case binary_op::gt_eq:
    return ">=";
  case binary_op::in:
    return "in";
  case binary_op::not_in:
    return "not in";
  case binary_op::add:
    return "+";
  case binary_op::sub:
    return "-";
  case binary_op::mul:
    return "*";
  case binary_op::div:
    return "/";
  case binary_op::mod:
    return "%";
  case binary_op::add_wrap:
    return "+%";
  case binary_op::sub_wrap:
    return "-%";
  case binary_op::mul_wrap:
    return "*%";
  case binary_op::add_sat:
    return "+|";
  case binary_op::sub_sat:
    return "-|";
  case binary_op::mul_sat:
    return "*|";
  case binary_op::shl:
    return "<<";
  case binary_op::shr:
    return ">>";
  case binary_op::bit_and:
    return "&";
  case binary_op::bit_or:
    return "|";
  case binary_op::bit_xor:
    return "^";
  case binary_op::range:
    return "..";
  case binary_op::range_inclusive:
    return "..=";
  }
  return "<?>";
}

/// @brief Binary operator application with already-associated precedence.
struct binary_expr : expr {
  binary_op op;  ///< Normalized operator semantics.
  ptr<expr> lhs; ///< Left operand.
  ptr<expr> rhs; ///< Right operand.

  binary_expr() : expr(node_kind::binary_expr) {}
};

/// @brief Normalized unary operators.
enum class unary_op : uint8_t {
  neg,         ///< Arithmetic negation.
  bit_not,     ///< Bitwise inversion.
  deref,       ///< Indirection / dereference.
  addr_of,     ///< Address-of or borrow.
  addr_of_mut, ///< Mutable address-of or borrow.
  logical_not, ///< Logical negation.
};

/// @brief Returns the canonical surface spelling for a unary operator.
///
/// @param op Unary operator to describe.
[[nodiscard]] inline auto unary_op_name(unary_op op) noexcept
    -> std::string_view {
  switch (op) {
  case unary_op::neg:
    return "-";
  case unary_op::bit_not:
    return "~";
  case unary_op::deref:
    return "*";
  case unary_op::addr_of:
    return "&";
  case unary_op::addr_of_mut:
    return "&mut";
  case unary_op::logical_not:
    return "not";
  }
  return "<?>";
}

/// @brief Unary operator application.
struct unary_expr : expr {
  unary_op op;       ///< Normalized operator semantics.
  ptr<expr> operand; ///< Operand expression.

  unary_expr() : expr(node_kind::unary_expr) {}
};

/// Field access: `expr.name`
struct field_expr : expr {
  ptr<expr> object;       ///< Base expression being projected from.
  std::string field_name; ///< Selected field or method name.
  std::vector<ptr<type_expr>>
      generic_args; ///< Optional method generic arguments.

  field_expr() : expr(node_kind::field_expr) {}
};

/// Index: `expr[index]`
struct index_expr : expr {
  ptr<expr> object; ///< Expression providing the indexed container.
  ptr<expr> index;  ///< Index or key expression.

  index_expr() : expr(node_kind::index_expr) {}
};

/// Function/method call: `expr(args...)`
struct call_arg {
  source_span span;                ///< Full source range of the argument.
  std::optional<std::string> name; ///< Named-argument label when present.
  ptr<expr> value;                 ///< Argument value expression.
};

/// @brief Function or method invocation.
struct call_expr : expr {
  ptr<expr> callee;           ///< Expression producing the callable target.
  std::vector<call_arg> args; ///< Arguments in evaluation order.

  call_expr() : expr(node_kind::call_expr) {}
};

/// Type cast: `expr as Type`
struct cast_expr : expr {
  ptr<expr> operand;          ///< Value being reinterpreted or converted.
  ptr<type_expr> target_type; ///< Destination type syntax.

  cast_expr() : expr(node_kind::cast_expr) {}
};

/// Try propagation: `expr?` or `expr?.field`
struct try_expr : expr {
  ptr<expr> operand; ///< Expression whose failure should propagate outward.

  try_expr() : expr(node_kind::try_expr) {}
};

/// Tuple literal: `(a, b, c)`
struct tuple_expr : expr {
  std::vector<ptr<expr>> elements; ///< Elements in source order.

  tuple_expr() : expr(node_kind::tuple_expr) {}
};

/// Array literal: `[a, b, c]` or fill: `[val; count]`
struct array_expr : expr {
  std::vector<ptr<expr>>
      elements;         ///< Element expressions for explicit-list form.
  ptr<expr> fill_value; ///< Repeated value for `[val; count]` form.
  ptr<expr> fill_count; ///< Repetition count for `[val; count]` form.

  array_expr() : expr(node_kind::array_expr) {}
};

/// Struct literal field: `name: expr` or shorthand `name`
struct struct_field_init {
  source_span span; ///< Full source range of the field initializer.
  std::string name; ///< Field name being initialized.
  ptr<expr>
      value; ///< Null for shorthand `{x}`, which later lowers to `{x: x}`.
};

/// Struct literal: `{a: 1, b: 2}`
struct struct_expr : expr {
  ptr<expr> type_name; ///< Optional explicit type head, e.g. `Point` in `Point
                       ///< {...}`.
  std::vector<struct_field_init>
      fields; ///< Field initializers in source order.

  struct_expr() : expr(node_kind::struct_expr) {}
};

/// Lambda / closure: `x => x + 1` or `(a, b) -> int => a + b`
struct lambda_param {
  source_span span;               ///< Full source range of the parameter.
  ptr<node> pattern;              ///< Parameter binding pattern.
  ptr<type_expr> type_annotation; ///< Optional parameter type annotation.
};

/// @brief Lambda or closure expression.
///
/// The parser preserves both compact expression bodies and block bodies so
/// later lowering can choose the most natural internal form.
struct lambda_expr : expr {
  bool is_pure = false;             ///< Whether the lambda was declared `pure`.
  bool is_move = false;             ///< Whether captures should be by move.
  std::vector<lambda_param> params; ///< Parameter list in source order.
  ptr<type_expr> return_type;       ///< Optional explicit return type.
  /// Body is either a single expression or a block (vector of statements).
  ptr<expr> body_expr;               ///< Present for compact expression bodies.
  std::vector<ptr<node>> body_stmts; ///< Present for block bodies.

  lambda_expr() : expr(node_kind::lambda_expr) {}
};

/// Module path expression: `a.b.c`
struct module_path_expr : expr {
  std::vector<std::string> segments; ///< Path segments before name resolution.

  module_path_expr() : expr(node_kind::module_path_expr) {}
};

/// Parenthesized expression: `(expr)`
struct group_expr : expr {
  ptr<expr>
      inner; ///< Wrapped expression preserved for source-faithful AST shape.

  group_expr() : expr(node_kind::group_expr) {}
};

/// `if expr: ... elif ...: ... else: ...` (expression form)
struct if_branch {
  source_span span;      ///< Full source range of the branch header and body.
  ptr<expr> condition;   ///< Conditional expression for ordinary branches.
  ptr<node> let_pattern; ///< Pattern for `if let` / `elif let` branches.
  ptr<expr> let_expr;    ///< Scrutinee expression for `if let` branches.
  std::vector<ptr<node>> body; ///< Branch body normalized as node list.
};

/// @brief Conditional expression with one or more branches.
struct if_expr : expr {
  std::vector<if_branch>
      branches; ///< `if` branch followed by any `elif` branches.
  std::vector<ptr<node>> else_body; ///< Optional fallback body.

  if_expr() : expr(node_kind::if_expr) {}
};

/// `match subject: ...`
struct match_arm {
  source_span span; ///< Full source range of the arm.
  ptr<node>
      pattern;     ///< Pattern syntax, stored as node for recovery flexibility.
  ptr<expr> guard; ///< Optional guard expression narrowing this arm.
  /// Body: inline expression or block.
  ptr<expr> body_expr; ///< Present for compact `=> expr`-style bodies.
  std::vector<ptr<node>> body_stmts; ///< Present for block bodies.
  bool has_error = false;            ///< Set when recovery repaired the arm.
};

/// @brief Pattern-dispatch expression.
struct match_expr : expr {
  ptr<expr> subject;           ///< Expression being matched on.
  std::vector<match_arm> arms; ///< Arms evaluated in source order.

  match_expr() : expr(node_kind::match_expr) {}
};

/// `for vars in iter [if guard] => yield_expr`
struct for_expr : expr {
  /// One `vars in iterable` clause in a comprehension chain.
  struct iter_clause {
    std::vector<ptr<node>>
        patterns;       ///< Iteration patterns introduced by the clause.
    ptr<expr> iterable; ///< Source expression producing values to iterate.
  };
  std::vector<iter_clause>
      clauses;          ///< One or more iteration clauses in source order.
  ptr<expr> guard;      ///< Optional trailing filter expression.
  ptr<expr> yield_expr; ///< Expression yielded for each surviving iteration.

  for_expr() : expr(node_kind::for_expr) {}
};

/// `await expr` or `await yield`
struct await_expr : expr {
  ptr<expr> operand; ///< Awaited expression; null for `await yield` syntax.
  bool is_yield =
      false; ///< Distinguishes the coroutine handoff form from ordinary await.

  await_expr() : expr(node_kind::await_expr) {}
};

/// `async: ...`
struct async_expr : expr {
  std::vector<ptr<node>>
      body; ///< Async body normalized as a statement/node list.

  async_expr() : expr(node_kind::async_expr) {}
};

/// `par: ...`
struct par_expr : expr {
  std::vector<ptr<expr>>
      branches; ///< Branch expressions that should run in parallel.

  par_expr() : expr(node_kind::par_expr) {}
};

/// `race: ...`
struct race_expr : expr {
  std::vector<ptr<expr>> branches; ///< Competing branch expressions.

  race_expr() : expr(node_kind::race_expr) {}
};

/// `on(Type): block` or `on(Type, sender)`
struct on_expr : expr {
  ptr<type_expr> context_type; ///< Context type that scopes the body.
  ptr<expr> sender;            ///< Optional sender/source expression.
  std::vector<ptr<node>> body; ///< Body normalized as statement/node list.

  on_expr() : expr(node_kind::on_expr) {}
};

/// `block_expr` — `INDENT stmts DEDENT`
struct block_expr : expr {
  std::vector<ptr<node>> stmts; ///< Statements or nested items in block order.

  block_expr() : expr(node_kind::block_expr) {}
};

/// Quasi-quote: `` `(content)` `` or `` `content` ``
struct quote_expr : expr {
  std::vector<token>
      tokens; ///< Raw quoted tokens preserved for macro-like consumers.

  quote_expr() : expr(node_kind::quote_expr) {}
};

/// Splice: `~(expr)` or `~ident`
struct splice_expr : expr {
  ptr<expr> operand; ///< Expression whose value should be injected into quoted
                     ///< syntax.

  splice_expr() : expr(node_kind::splice_expr) {}
};

/// `static expr`
struct static_expr : expr {
  ptr<expr> operand; ///< Expression to be evaluated in compile-time context.
  static_expr() : expr(node_kind::static_expr) {}
};

/// A single binding inside a `where` clause: `name = expr`
struct where_binding {
  source_span span; ///< Full source range of the binding clause.
  std::string name; ///< Identifier introduced by the binding.
  ptr<expr> value;  ///< Right-hand side expression evaluated for the binding.
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
  ptr<expr> inner; ///< Expression whose evaluation sees the bindings.
  std::vector<where_binding>
      bindings; ///< Locally scoped bindings in evaluation order.

  where_expr() : expr(node_kind::where_expr) {}
};

// ==========================================================================
//  Pattern nodes
// ==========================================================================

/// @brief Base class for all pattern-position syntax nodes.
struct pattern : node {
  using node::node;
};

/// @brief Recovery placeholder for malformed patterns.
struct error_pattern : pattern {
  /// @brief Creates an error-marked pattern placeholder.
  ///
  /// @param s Source span where pattern parsing failed.
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
  token_kind lit_kind; ///< Literal token category being matched.
  std::string value;   ///< Raw literal spelling to compare semantically later.

  literal_pattern() : pattern(node_kind::literal_pattern) {}
};

/// Simple name binding: `x`, or a mutable one: `mut x`
struct binding_pattern : pattern {
  std::string name; ///< Bound identifier introduced when the pattern matches.
  bool is_mut = false; ///< Written with a leading `mut` (e.g. `mut self`).

  binding_pattern() : pattern(node_kind::binding_pattern) {}
};

/// Constructor: `Some(x)`, `Point(a, b)`
struct constructor_pattern : pattern {
  std::string name; ///< Constructor or variant name.
  std::vector<ptr<pattern>>
      args; ///< Nested subpatterns for payload destructuring.

  constructor_pattern() : pattern(node_kind::constructor_pattern) {}
};

/// Tuple: `(a, b, c)`
struct tuple_pattern : pattern {
  std::vector<ptr<pattern>> elements; ///< Element patterns in tuple order.

  tuple_pattern() : pattern(node_kind::tuple_pattern) {}
};

/// Struct pattern field: `name: pattern`, `name`, or `..`
struct field_pattern {
  source_span span; ///< Full source range of the field clause.
  std::string name; ///< Field name; empty only for the rest pattern `..`.
  ptr<pattern>
      pattern; ///< Explicit subpattern; null for shorthand `{name}` binding.
  bool is_rest = false; ///< Whether this entry is the struct-rest marker `..`.
};

/// Struct pattern: `{x: a, y: b, ..}`
struct struct_pattern : pattern {
  std::vector<field_pattern>
      fields; ///< Field destructuring clauses in source order.

  struct_pattern() : pattern(node_kind::struct_pattern) {}
};

/// Array/slice pattern: `[a, b, c]`
struct array_pattern : pattern {
  std::vector<ptr<pattern>> elements; ///< Element patterns in source order.

  array_pattern() : pattern(node_kind::array_pattern) {}
};

/// Range: `1..10`, `1..=10`, `1..`, `..10`
struct range_pattern : pattern {
  ptr<expr> start;        ///< Lower bound; null for open-start ranges.
  ptr<expr> end;          ///< Upper bound; null for open-end ranges.
  bool inclusive = false; ///< Whether the upper bound is included.

  range_pattern() : pattern(node_kind::range_pattern) {}
};

/// `some(pattern)`, `ok(pattern)`, `err(pattern)`
/// @brief Shared discriminator for option/result wrapper patterns.
enum class option_result_kind : uint8_t {
  some, ///< Option success/presence wrapper.
  ok,   ///< Result success wrapper.
  err,  ///< Result failure wrapper.
};

/// @brief Pattern matching the option `some(...)` wrapper form.
struct option_pattern : pattern {
  option_result_kind option_kind; ///< Which wrapper keyword was parsed.
  ptr<pattern> inner;             ///< Inner payload pattern.

  option_pattern() : pattern(node_kind::option_pattern) {}
};

/// Result pattern — reuses option_pattern but with different kind.
struct result_pattern : pattern {
  option_result_kind result_kind; ///< Distinguishes `ok(...)` from `err(...)`.
  ptr<pattern> inner;             ///< Inner payload pattern.

  result_pattern() : pattern(node_kind::result_pattern) {}
};

/// `&pattern`
struct ref_pattern : pattern {
  ptr<pattern> inner; ///< Referenced subpattern.

  ref_pattern() : pattern(node_kind::ref_pattern) {}
};

/// `a | b | c`
struct or_pattern : pattern {
  std::vector<ptr<pattern>>
      alternatives; ///< Alternatives tried from left to right.

  or_pattern() : pattern(node_kind::or_pattern) {}
};

/// `(pattern)` — parenthesized pattern for grouping.
struct group_pattern : pattern {
  ptr<pattern> inner; ///< Grouped inner pattern.
  std::optional<std::string>
      alias; ///< Optional alias introduced after grouping.

  group_pattern() : pattern(node_kind::group_pattern) {}
};

// ==========================================================================
//  Statement nodes
// ==========================================================================

/// @brief Base class for all statement-position syntax nodes.
struct stmt : node {
  using node::node;
};

/// @brief Recovery placeholder for malformed statements.
struct error_stmt : stmt {
  /// @brief Creates an error-marked statement placeholder.
  ///
  /// @param s Source span where statement parsing failed.
  explicit error_stmt(source_span s = source_span::dummy())
      : stmt(node_kind::expr_stmt, s) {
    has_error = true;
  }
};

/// `let pattern [: type] = expr`
/// `let pattern [: type] = expr else: block`
struct let_stmt : stmt {
  ptr<pattern> pattern; ///< Binding pattern introduced by the statement.
  ptr<type_expr>
      type_annotation;   ///< Optional explicit annotation for the bound value.
  ptr<expr> initializer; ///< Expression whose result is destructured or bound.
  std::vector<ptr<node>>
      else_body; ///< Failure path for `let ... else` destructuring.

  let_stmt() : stmt(node_kind::let_stmt) {}
};

/// `var name [: type] = expr`
struct var_stmt : stmt {
  std::string name;               ///< Mutable variable name.
  ptr<type_expr> type_annotation; ///< Optional explicit variable type.
  ptr<expr> initializer;          ///< Initial assigned value.

  var_stmt() : stmt(node_kind::var_stmt) {}
};

/// @brief Normalized assignment operators.
enum class assign_op : uint8_t {
  assign,          ///< Plain assignment `=`.
  add_assign,      ///< Compound add assignment `+=`.
  sub_assign,      ///< Compound subtract assignment `-=`.
  mul_assign,      ///< Compound multiply assignment `*=`.
  div_assign,      ///< Compound divide assignment `/=`.
  mod_assign,      ///< Compound remainder assignment `%=`.
  and_assign,      ///< Compound bitwise-and assignment `&=`.
  or_assign,       ///< Compound bitwise-or assignment `|=`.
  xor_assign,      ///< Compound bitwise-xor assignment `^=`.
  shl_assign,      ///< Compound left-shift assignment `<<=`.
  shr_assign,      ///< Compound right-shift assignment `>>=`.
  add_wrap_assign, ///< Compound wrapping add assignment `+%=`.
  sub_wrap_assign, ///< Compound wrapping subtract assignment `-%=`.
  mul_wrap_assign, ///< Compound wrapping multiply assignment `*%=`.
  add_sat_assign,  ///< Compound saturating add assignment `+|=`.
  sub_sat_assign,  ///< Compound saturating subtract assignment `-|=`.
  mul_sat_assign,  ///< Compound saturating multiply assignment `*|=`.
};

/// @brief Assignment statement with a normalized operator.
struct assign_stmt : stmt {
  ptr<expr> target; ///< Expression designating the assignment destination.
  assign_op op;     ///< Assignment flavor selected by the parser.
  ptr<expr> value;  ///< Right-hand side expression.

  assign_stmt() : stmt(node_kind::assign_stmt) {}
};

/// Expression used as a statement.
struct expr_stmt : stmt {
  ptr<expr> expr; ///< Expression evaluated for side effects or discarded value.

  expr_stmt() : stmt(node_kind::expr_stmt) {}
};

/// `return [expr]`
struct return_stmt : stmt {
  ptr<expr> value; ///< Returned expression; null for bare `return`.

  return_stmt() : stmt(node_kind::return_stmt) {}
};

/// `if expr: block { elif expr: block } [else: block]`
struct if_stmt : stmt {
  std::vector<if_branch>
      branches; ///< `if` branch followed by any `elif` branches.
  std::vector<ptr<node>> else_body; ///< Optional fallback body.

  if_stmt() : stmt(node_kind::if_stmt) {}
};

/// `while expr: block` or `while let pattern = expr: block`
struct while_stmt : stmt {
  ptr<expr> condition;         ///< Loop condition for ordinary `while` form.
  ptr<pattern> let_pattern;    ///< Pattern for `while let` destructuring form.
  ptr<expr> let_expr;          ///< Scrutinee expression for `while let` form.
  std::vector<ptr<node>> body; ///< Loop body nodes.

  while_stmt() : stmt(node_kind::while_stmt) {}
};

/// `for vars in expr [if guard]: block`
struct for_stmt : stmt {
  std::vector<ptr<pattern>> patterns; ///< Loop binding patterns.
  ptr<expr> iterable;                 ///< Source of values to iterate over.
  ptr<expr> guard;                    ///< Optional per-iteration filter.
  std::vector<ptr<node>> body;        ///< Loop body nodes.

  for_stmt() : stmt(node_kind::for_stmt) {}
};

/// `match subject: ...`
struct match_stmt : stmt {
  ptr<expr> subject;           ///< Expression being matched on.
  std::vector<match_arm> arms; ///< Arms evaluated in source order.

  match_stmt() : stmt(node_kind::match_stmt) {}
};

/// Crew option: `name: value`
struct crew_option {
  source_span span;  ///< Source range of the option entry.
  std::string name;  ///< Option key.
  std::string value; ///< Raw option value text.
};

/// `crew name(options): block` in expression position.
struct crew_expr : expr {
  std::string name;                 ///< Crew target or backend name.
  std::vector<crew_option> options; ///< Static configuration options.
  std::vector<ptr<node>> body;      ///< Body executed within the crew context.

  crew_expr() : expr(node_kind::crew_expr) {}
};

/// `crew name(options): block`
struct crew_stmt : stmt {
  std::string name;                 ///< Crew target or backend name.
  std::vector<crew_option> options; ///< Static configuration options.
  std::vector<ptr<node>> body;      ///< Body executed within the crew context.

  crew_stmt() : stmt(node_kind::crew_stmt) {}
};

/// `asm { content }`
struct asm_stmt : stmt {
  std::string
      content; ///< Raw assembly body preserved for later backend handling.

  asm_stmt() : stmt(node_kind::asm_stmt) {}
};

/// `~expr`
struct splice_stmt : stmt {
  ptr<expr> expr; ///< Expression whose generated syntax should be spliced here.

  splice_stmt() : stmt(node_kind::splice_stmt) {}
};

// ==========================================================================
//  Declaration / item nodes
// ==========================================================================

/// `use` path handling.
struct use_item {
  source_span span;                 ///< Source range of the imported leaf item.
  std::string name;                 ///< Imported name.
  std::optional<std::string> alias; ///< Optional rename introduced by `as`.
};

/// @brief Shape of the selector portion of a `use` declaration.
enum class use_selector_kind : uint8_t {
  single,   ///< Import exactly one trailing item, optionally renamed.
  group,    ///< Import an explicit brace-delimited item set.
  wildcard, ///< Import every visible item from the path prefix.
};

/// @brief Parsed selector detail for a `use` declaration.
struct use_selector {
  source_span span;            ///< Source range of the selector portion.
  use_selector_kind kind;      ///< Selector strategy chosen by the user.
  std::vector<use_item> items; ///< Imported items for single/group selectors.
};

/// @brief Import declaration at file or module scope.
struct use_decl : node {
  visibility visibility =
      visibility::def;           ///< Visibility of the imported binding(s).
  std::vector<std::string> path; ///< Module path prefix being imported from.
  std::optional<use_selector> selector; ///< Optional leaf selection strategy.

  use_decl() : node(node_kind::use_decl) {}
};

/// `type Name[Params] = TypeDef`
struct type_decl : node {
  visibility visibility = visibility::def; ///< Declared visibility of the type.
  std::string name; ///< Type name introduced by the declaration.
  std::vector<type_param>
      type_params; ///< Generic parameters in declaration order.
  /// The type definition body — one of: struct_body, sum_body, type_expr,
  /// refinement_type.
  ptr<node> definition; ///< Parsed right-hand-side definition payload.
  std::vector<std::string>
      deriving;        ///< Derived traits or behaviors requested by name.
  ptr<expr> invariant; ///< Optional type invariant expression.

  type_decl() : node(node_kind::type_decl) {}
};

/// @brief Wrapper node for a struct-style type body.
struct struct_type_def : node {
  struct_body body; ///< Field list preserved as a dedicated helper record.

  struct_type_def() : node(node_kind::struct_type_def) {}
};

/// @brief Wrapper node for a sum-type body.
struct sum_type_def : node {
  sum_body body; ///< Variant list preserved as a dedicated helper record.

  sum_type_def() : node(node_kind::sum_type_def) {}
};

/// Associated type in a trait: `type Name [= Default]`
struct associated_type_decl {
  source_span span; ///< Source range of the declaration.
  visibility visibility =
      visibility::def;         ///< Visibility inside the trait surface.
  std::string name;            ///< Associated type name.
  ptr<type_expr> default_type; ///< Optional default implementation type.
};

/// @brief Node wrapper allowing associated-type declarations in generic item
/// lists.
struct associated_type_decl_node : node {
  associated_type_decl value; ///< Wrapped associated-type declaration payload.

  associated_type_decl_node() : node(node_kind::associated_type_decl_node) {}
};

/// Associated type in an impl: `type Name = ConcreteType`
struct associated_type_def {
  source_span span;    ///< Source range of the definition.
  std::string name;    ///< Associated type name being defined.
  ptr<type_expr> type; ///< Concrete type provided by the impl.
};

/// @brief Node wrapper allowing associated-type definitions in generic item
/// lists.
struct associated_type_def_node : node {
  associated_type_def value; ///< Wrapped associated-type definition payload.

  associated_type_def_node() : node(node_kind::associated_type_def_node) {}
};

/// Trait declaration.
struct trait_decl : node {
  visibility visibility =
      visibility::def; ///< Declared visibility of the trait.
  std::string name;    ///< Trait name.
  std::vector<type_param>
      type_params; ///< Generic parameters accepted by the trait.
  std::optional<bound>
      requires_bound; ///< Optional supertrait-style requirements.
  std::vector<ptr<node>>
      items; ///< Member items such as functions, statics, and associated types.

  trait_decl() : node(node_kind::trait_decl) {}
};

/// Concept declaration.
struct concept_param {
  source_span span; ///< Source range of the concept parameter.
  std::string name; ///< Parameter name used within concept constraints.
  bool is_higher_kinded =
      false; ///< Whether the parameter accepts a type constructor.
};

struct concept_constraint {
  source_span span;       ///< Full source range of the constraint.
  ptr<type_expr> subject; ///< Subject type for type-oriented constraints.
  ptr<node>
      bound_or_expr; ///< Bound or value expression payload, resolved later.
};

struct concept_decl : node {
  visibility visibility =
      visibility::def; ///< Declared visibility of the concept.
  std::string name;    ///< Concept name.
  std::vector<concept_param>
      params; ///< Concept parameters in declaration order.
  std::vector<concept_constraint>
      constraints; ///< Constraint clauses defining the concept.

  concept_decl() : node(node_kind::concept_decl) {}
};

/// Impl declaration.
struct impl_decl : node {
  std::vector<type_param>
      type_params; ///< Generic parameters scoped to the impl.
  ptr<type_expr>
      trait_type; ///< Implemented trait/concept; null for inherent impls.
  ptr<type_expr>
      for_type; ///< Concrete target type receiving the implementation.
  std::vector<where_constraint>
      where_constraints;        ///< Additional applicability constraints.
  std::vector<ptr<node>> items; ///< Member definitions such as functions,
                                ///< statics, and associated types.

  impl_decl() : node(node_kind::impl_decl) {}
};

/// Extension-method declaration: `extend Type: ...`. Adds methods to `Type`
/// without claiming trait conformance, so unlike `impl_decl` it carries no
/// trait, no `where` clause, and no associated-type members.
struct extend_decl : node {
  ptr<type_expr> for_type;      ///< Concrete target type receiving the methods.
  std::vector<ptr<node>> items; ///< Method (`func_decl`) definitions.

  extend_decl() : node(node_kind::extend_decl) {}
};

/// Function modifier flags.
struct func_modifiers {
  bool is_pure = false;    ///< Whether the declaration promises purity.
  bool is_async = false;   ///< Whether the function executes asynchronously.
  bool is_machine = false; ///< Whether machine-mode arithmetic semantics apply.
  bool is_static =
      false; ///< Whether the function is static rather than instance-oriented.
  bool is_intrinsic = false;    ///< Whether this is a signature-only
                                ///< declaration backed by a native
                                ///< implementation per backend, not a body.
  ptr<type_expr> async_context; ///< Optional explicit async context type.
};

/// Contract clause: `pre expr [, message]` or `post expr [, message]`
struct contract_clause {
  source_span span;    ///< Source range of the contract clause.
  bool is_pre;         ///< True for preconditions, false for postconditions.
  ptr<expr> condition; ///< Condition expression to validate.
  std::optional<std::string>
      message; ///< Optional user-facing failure explanation.
};

/// Parameter in a function signature.
struct param {
  source_span span;     ///< Full source range of the parameter.
  ptr<pattern> pattern; ///< Binding pattern introduced by the parameter.
  ptr<type_expr> type_annotation; ///< Optional explicit parameter type.
  ptr<expr> default_value;        ///< Optional default argument expression.
};

/// Function declaration.
struct func_decl : node {
  visibility visibility =
      visibility::def;      ///< Declared visibility of the function.
  func_modifiers modifiers; ///< Semantic modifiers attached to the declaration.
  std::string name;         ///< Function name.
  std::vector<type_param>
      type_params;            ///< Generic parameters in declaration order.
  std::vector<param> params;  ///< Call parameters in declaration order.
  ptr<type_expr> return_type; ///< Optional explicit return type.
  std::vector<where_constraint>
      where_constraints; ///< Additional applicability constraints.
  std::vector<contract_clause>
      contracts; ///< Pre/postconditions attached to the function.
  /// Body: either an inline expression or a block of statements.
  ptr<expr> body_expr; ///< Present for compact expression-bodied functions.
  std::vector<ptr<node>> body_stmts; ///< Present for block-bodied functions.

  func_decl() : node(node_kind::func_decl) {}
};

/// Sub-module: `module Name` or `module Name: ...`
struct sub_module_decl : node {
  visibility visibility =
      visibility::def; ///< Declared visibility of the nested module.
  std::string name;    ///< Nested module name.
  std::vector<ptr<node>>
      items; ///< Nested items; empty for declaration-only form.

  sub_module_decl() : node(node_kind::sub_module_decl) {}
};

/// Dependency declaration: `dep Name: ...`
struct dep_field {
  source_span span;  ///< Source range of the metadata field.
  std::string key;   ///< Metadata key.
  std::string value; ///< Raw metadata value text.
};

struct dep_decl : node {
  std::string name;              ///< Dependency name or identifier.
  std::vector<dep_field> fields; ///< Parsed metadata fields.

  dep_decl() : node(node_kind::dep_decl) {}
};

/// Static declarations — multiple forms:
///   - `static if expr: ...`
///   - `static Name : Type = expr`
///   - `static assert expr [, message]`
///   - `static for vars in expr [if guard] => expr`
///   - `static for vars in expr [if guard]: block`
enum class static_decl_kind : uint8_t {
  conditional_compilation, ///< Compile-time branch selection form.
  binding,                 ///< Compile-time named binding form.
  assertion,               ///< Compile-time assertion form.
  for_inline,              ///< Compile-time `for` yielding inline expressions.
  for_block,               ///< Compile-time `for` executing a statement block.
};

/// @brief Compile-time declaration whose fields are interpreted by `decl_kind`.
///
/// The parser keeps all `static` forms in one node so later compile-time phases
/// can dispatch from a single surface construct while still preserving the
/// exact syntax the user wrote.
struct static_decl : node {
  visibility visibility = visibility::def; ///< Declared visibility when the
                                           ///< form introduces a binding.
  static_decl_kind decl_kind{
      static_decl_kind::binding}; ///< Which `static` form this node represents.

  // For Binding:
  std::string name; ///< Binding name for `static Name = expr`.
  ptr<type_expr>
      type_annotation;   ///< Optional type annotation for binding form.
  ptr<expr> initializer; ///< Initializer for binding form.

  // For Assert:
  ptr<expr> assert_condition; ///< Condition expression for assertion form.
  std::optional<std::string>
      assert_message; ///< Optional user-facing assertion message.

  // For ConditionalCompilation:
  ptr<expr> if_condition;         ///< Compile-time condition for `static if`.
  std::vector<ptr<node>> if_body; ///< Body chosen when the condition is true.
  std::vector<ptr<node>>
      else_body; ///< Optional body chosen when the condition is false.

  // For ForInline / ForBlock:
  std::vector<ptr<pattern>>
      for_patterns;       ///< Loop patterns for compile-time iteration forms.
  ptr<expr> for_iterable; ///< Compile-time iterable expression.
  ptr<expr> for_guard;    ///< Optional compile-time loop guard.
  ptr<expr> for_yield;    ///< Yielded expression for inline `static for`.
  std::vector<ptr<node>> for_body; ///< Statement body for block `static for`.

  static_decl() : node(node_kind::static_decl) {}
};

// ==========================================================================
//  Module declaration and file root
// ==========================================================================

/// @brief File-level module declaration identifying the source file's logical
/// module.
struct module_decl : node {
  std::vector<std::string> path; ///< Qualified module path in source order.

  module_decl() : node(node_kind::module_decl) {}
};

/// @brief Root AST node for a parsed source file.
///
/// Later phases should treat this as the unit of module loading, name binding,
/// and per-file diagnostics. It intentionally retains file-level directives
/// like `no_prelude` alongside the declared items.
struct file : node {
  ptr<module_decl> module_decl; ///< Optional explicit module declaration.
  bool no_prelude = false; ///< Whether implicit prelude imports are disabled.
  std::vector<ptr<node>> items; ///< Top-level items in source order.

  file() : node(node_kind::file_node) {}
};

// ==========================================================================
//  Pattern alias wrapper — for `pattern as name`
// ==========================================================================

/// @brief Helper record for grouped patterns that optionally bind an alias.
///
/// This is not itself a node because it exists only as parser plumbing when a
/// construct needs both the underlying pattern and a trailing alias name.
struct aliased_pattern {
  ptr<pattern> pattern; ///< Underlying parsed pattern.
  std::optional<std::string>
      alias;        ///< Optional alias introduced by surface syntax.
  source_span span; ///< Combined source range of pattern and alias.
};

} // namespace kira::ast
