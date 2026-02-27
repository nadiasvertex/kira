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
struct File;
struct ModuleDecl;

// Items
struct UseDecl;
struct TypeDecl;
struct TraitDecl;
struct ImplDecl;
struct ConceptDecl;
struct FuncDecl;
struct SubModuleDecl;
struct DepDecl;
struct StaticDecl;
struct SpliceStmt;

// Type system
struct NamedType;
struct TupleType;
struct SliceType;
struct ArrayType;
struct RefType;
struct PtrType;
struct FnType;
struct QuoteType;
struct UnionType;
struct RefinementType;

// Type params & bounds
struct TypeParam;
struct TypeArg;
struct BoundTerm;
struct WhereConstraint;

// Struct / sum
struct StructField;
struct StructBody;
struct SumVariant;
struct SumBody;

// Trait / impl internals
struct AssociatedTypeDecl;
struct AssociatedTypeDef;

// Concept internals
struct ConceptParam;
struct ConceptConstraint;

// Function-related
struct Param;
struct ContractClause;

// Statements
struct LetStmt;
struct VarStmt;
struct AssignStmt;
struct ExprStmt;
struct ReturnStmt;
struct IfStmt;
struct WhileStmt;
struct ForStmt;
struct MatchStmt;
struct CrewStmt;
struct AsmStmt;

// Expressions
struct IdentExpr;
struct LiteralExpr;
struct BinaryExpr;
struct UnaryExpr;
struct PostfixExpr;
struct CallExpr;
struct IndexExpr;
struct FieldExpr;
struct CastExpr;
struct TryExpr;
struct TupleExpr;
struct ArrayExpr;
struct StructExpr;
struct LambdaExpr;
struct MatchExpr;
struct IfExpr;
struct ForExpr;
struct AwaitExpr;
struct ParExpr;
struct RaceExpr;
struct OnExpr;
struct BlockExpr;
struct QuoteExpr;
struct SpliceExpr;
struct StaticExpr;
struct ModulePathExpr;
struct GroupExpr;

// Patterns
struct WildcardPattern;
struct LiteralPattern;
struct BindingPattern;
struct ConstructorPattern;
struct TuplePattern;
struct StructPattern;
struct ArrayPattern;
struct RangePattern;
struct OptionPattern;
struct ResultPattern;
struct RefPattern;
struct OrPattern;
struct GroupPattern;

// Match arms
struct MatchArm;

// ==========================================================================
//  Smart pointer aliases — every AST node is heap-allocated and owned
//  via unique_ptr. This keeps node sizes small and allows recursive types.
// ==========================================================================

template <typename T>
using Ptr = std::unique_ptr<T>;

template <typename T>
using PtrVec = std::vector<Ptr<T>>;

/// Helper to create an AST node. Usage: `auto n = make<IdentExpr>(...);`
template <typename T, typename... Args>
[[nodiscard]] Ptr<T> make(Args&&... args) {
  return std::make_unique<T>(std::forward<Args>(args)...);
}

// ==========================================================================
//  NodeKind — tag for downcasting via the base class. Every concrete
//  node has a unique kind value.
// ==========================================================================

enum class NodeKind : uint8_t {
  // Sentinel
  ErrorNode,

  // Top-level
  File,
  ModuleDecl,

  // Items
  UseDecl,
  TypeDecl,
  TraitDecl,
  ImplDecl,
  ConceptDecl,
  FuncDecl,
  SubModuleDecl,
  DepDecl,
  StaticDecl,
  SpliceStmt,

  // Types
  NamedType,
  TupleType,
  SliceType,
  ArrayType,
  RefType,
  PtrType,
  FnType,
  QuoteType,
  UnionType,
  RefinementType,

  // Statements
  LetStmt,
  VarStmt,
  AssignStmt,
  ExprStmt,
  ReturnStmt,
  IfStmt,
  WhileStmt,
  ForStmt,
  MatchStmt,
  CrewStmt,
  AsmStmt,

  // Expressions
  IdentExpr,
  LiteralExpr,
  BinaryExpr,
  UnaryExpr,
  PostfixExpr,
  CallExpr,
  IndexExpr,
  FieldExpr,
  CastExpr,
  TryExpr,
  TupleExpr,
  ArrayExpr,
  StructExpr,
  LambdaExpr,
  MatchExpr,
  IfExpr,
  ForExpr,
  AwaitExpr,
  ParExpr,
  RaceExpr,
  OnExpr,
  BlockExpr,
  QuoteExpr,
  SpliceExpr,
  StaticExpr,
  ModulePathExpr,
  GroupExpr,

  // Patterns
  WildcardPattern,
  LiteralPattern,
  BindingPattern,
  ConstructorPattern,
  TuplePattern,
  StructPattern,
  ArrayPattern,
  RangePattern,
  OptionPattern,
  ResultPattern,
  RefPattern,
  OrPattern,
  GroupPattern,
};

// ==========================================================================
//  Base classes — Node, TypeExpr, Stmt, Expr, Pattern
//
//  All AST nodes inherit from Node which carries a span and an error flag.
//  The error flag (`has_error`) is set when the parser used error recovery
//  to produce this node. Later phases can check it to skip semantic
//  analysis of broken subtrees — this prevents cascading errors.
// ==========================================================================

struct Node {
  NodeKind kind;
  Span span;
  bool has_error = false;

  explicit Node(NodeKind k, Span s = Span::dummy()) : kind(k), span(s) {}
  virtual ~Node() = default;

  // Non-copyable, movable.
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;
  Node(Node&&) = default;
  Node& operator=(Node&&) = default;
};

/// An AST node that was synthesized during error recovery. It stands in
/// for whatever the parser couldn't understand. This lets the rest of the
/// tree remain well-formed so downstream phases can keep going.
struct ErrorNode : Node {
  std::string description;  ///< What we tried and failed to parse.

  explicit ErrorNode(Span s = Span::dummy(), std::string desc = "")
      : Node(NodeKind::ErrorNode, s), description(std::move(desc)) {
    has_error = true;
  }
};

// --------------------------------------------------------------------------
//  Visibility
// --------------------------------------------------------------------------

enum class Visibility : uint8_t {
  Default,   ///< No explicit visibility keyword
  Pub,       ///< `pub`
  Internal,  ///< `internal`
  Super,     ///< `super`
  Priv,      ///< `priv`
};

[[nodiscard]] inline Visibility token_to_visibility(TokenKind kind) noexcept {
  switch (kind) {
    case TokenKind::KwPub:      return Visibility::Pub;
    case TokenKind::KwInternal: return Visibility::Internal;
    case TokenKind::KwSuper:    return Visibility::Super;
    case TokenKind::KwPriv:     return Visibility::Priv;
    default:                    return Visibility::Default;
  }
}

// ==========================================================================
//  Type expression nodes
// ==========================================================================

struct TypeExpr : Node {
  using Node::Node;
};

/// A type expression that the parser couldn't understand. Stands in for
/// the real thing so the tree remains structurally valid.
struct ErrorTypeExpr : TypeExpr {
  explicit ErrorTypeExpr(Span s = Span::dummy())
      : TypeExpr(NodeKind::NamedType, s) {  // reuse kind; has_error flag distinguishes
    has_error = true;
  }
};

struct NamedType : TypeExpr {
  std::vector<std::string> path;        ///< e.g., ["std", "collections", "Map"]
  std::vector<Ptr<TypeExpr>> type_args;  ///< Generic arguments, if any.
  // TypeArg can also carry value-level expressions; we handle that in
  // a wrapper node below.

  NamedType() : TypeExpr(NodeKind::NamedType) {}
};

struct TupleType : TypeExpr {
  std::vector<Ptr<TypeExpr>> elements;

  TupleType() : TypeExpr(NodeKind::TupleType) {}
};

struct SliceType : TypeExpr {
  Ptr<TypeExpr> element;

  SliceType() : TypeExpr(NodeKind::SliceType) {}
};

struct ArrayType : TypeExpr {
  Ptr<TypeExpr> element;
  Ptr<Node> size;  ///< An expression giving the array length.

  ArrayType() : TypeExpr(NodeKind::ArrayType) {}
};

struct RefType : TypeExpr {
  Ptr<TypeExpr> inner;
  bool is_mut = false;

  RefType() : TypeExpr(NodeKind::RefType) {}
};

struct PtrType : TypeExpr {
  Ptr<TypeExpr> inner;
  bool is_mut = false;

  PtrType() : TypeExpr(NodeKind::PtrType) {}
};

struct FnType : TypeExpr {
  std::vector<Ptr<TypeExpr>> param_types;
  Ptr<TypeExpr> return_type;  ///< nullptr if no return type.

  FnType() : TypeExpr(NodeKind::FnType) {}
};

struct QuoteType : TypeExpr {
  TokenKind quote_kind;  ///< KwExpr, KwStmt, KwDefExpr, or KwTypeExpr

  explicit QuoteType(TokenKind qk = TokenKind::KwExpr)
      : TypeExpr(NodeKind::QuoteType), quote_kind(qk) {}
};

struct UnionType : TypeExpr {
  std::vector<Ptr<TypeExpr>> alternatives;

  UnionType() : TypeExpr(NodeKind::UnionType) {}
};

struct RefinementType : TypeExpr {
  Ptr<TypeExpr> base;
  Ptr<Node> predicate;  ///< The `where expr` part.

  RefinementType() : TypeExpr(NodeKind::RefinementType) {}
};

// ==========================================================================
//  Type parameters and bounds
// ==========================================================================

struct TypeParam {
  Span span;
  std::string name;
  /// For type params: an optional trait/concept bound.
  /// For value params: the type of the value.
  Ptr<TypeExpr> bound_or_type;
  bool is_value_param = false;  ///< True if this is `name : type` (value param).
};

struct TypeArg {
  Span span;
  /// A type argument is either a type expression or a value expression.
  /// We store both as Node* and disambiguate later in semantic analysis.
  Ptr<Node> value;
  std::optional<std::string> name;  ///< Named argument: `name: type`
};

struct BoundTerm {
  Span span;
  /// A trait bound like `Trait[Args]` or a function type bound.
  Ptr<TypeExpr> type;
};

struct Bound {
  Span span;
  std::vector<BoundTerm> terms;  ///< Connected by `+`.
};

struct WhereConstraint {
  Span span;
  Ptr<TypeExpr> subject;
  /// Either a trait bound or an associated type equality constraint.
  Ptr<TypeExpr> bound_or_type;
};

// ==========================================================================
//  Struct and sum type bodies
// ==========================================================================

struct StructField {
  Span span;
  Visibility visibility = Visibility::Default;
  std::string name;
  Ptr<TypeExpr> type;
};

struct StructBody {
  Span span;
  std::vector<StructField> fields;
};

struct SumVariant {
  Span span;
  std::string name;
  std::vector<Ptr<TypeExpr>> payload_types;  ///< Empty for unit variants.
};

struct SumBody {
  Span span;
  std::vector<SumVariant> variants;
};

// ==========================================================================
//  Expression nodes
// ==========================================================================

struct Expr : Node {
  using Node::Node;
};

/// A placeholder expression for error recovery.
struct ErrorExpr : Expr {
  std::string description;

  explicit ErrorExpr(Span s = Span::dummy(), std::string desc = "")
      : Expr(NodeKind::IdentExpr, s), description(std::move(desc)) {
    has_error = true;
  }
};

struct IdentExpr : Expr {
  std::string name;

  IdentExpr() : Expr(NodeKind::IdentExpr) {}
};

struct LiteralExpr : Expr {
  TokenKind lit_kind;    ///< IntLit, FloatLit, StringLit, CharLit, KwTrue, KwFalse, KwUnit
  std::string value;     ///< The raw text of the literal.

  LiteralExpr() : Expr(NodeKind::LiteralExpr) {}
};

enum class BinaryOp : uint8_t {
  // Pipe
  Pipe,
  // Logical
  Or, And,
  // Comparison
  EqEq, BangEq, Lt, LtEq, Gt, GtEq, In, NotIn,
  // Arithmetic
  Add, Sub, Mul, Div, Mod,
  // Wrapping
  AddWrap, SubWrap, MulWrap,
  // Saturating
  AddSat, SubSat, MulSat,
  // Shifts
  Shl, Shr,
  // Bitwise
  BitAnd, BitOr, BitXor,
  // Range (used in patterns)
  Range, RangeInclusive,
};

[[nodiscard]] inline std::string_view binary_op_name(BinaryOp op) noexcept {
  switch (op) {
    case BinaryOp::Pipe:           return "|";
    case BinaryOp::Or:             return "or";
    case BinaryOp::And:            return "and";
    case BinaryOp::EqEq:          return "==";
    case BinaryOp::BangEq:        return "!=";
    case BinaryOp::Lt:            return "<";
    case BinaryOp::LtEq:          return "<=";
    case BinaryOp::Gt:            return ">";
    case BinaryOp::GtEq:          return ">=";
    case BinaryOp::In:            return "in";
    case BinaryOp::NotIn:         return "not in";
    case BinaryOp::Add:           return "+";
    case BinaryOp::Sub:           return "-";
    case BinaryOp::Mul:           return "*";
    case BinaryOp::Div:           return "/";
    case BinaryOp::Mod:           return "%";
    case BinaryOp::AddWrap:       return "+%";
    case BinaryOp::SubWrap:       return "-%";
    case BinaryOp::MulWrap:       return "*%";
    case BinaryOp::AddSat:        return "+|";
    case BinaryOp::SubSat:        return "-|";
    case BinaryOp::MulSat:        return "*|";
    case BinaryOp::Shl:           return "<<";
    case BinaryOp::Shr:           return ">>";
    case BinaryOp::BitAnd:        return "&";
    case BinaryOp::BitOr:         return "|";
    case BinaryOp::BitXor:        return "^";
    case BinaryOp::Range:         return "..";
    case BinaryOp::RangeInclusive: return "..=";
  }
  return "<?>";
}

struct BinaryExpr : Expr {
  BinaryOp op;
  Ptr<Expr> lhs;
  Ptr<Expr> rhs;

  BinaryExpr() : Expr(NodeKind::BinaryExpr) {}
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
    case UnaryOp::Neg:       return "-";
    case UnaryOp::BitNot:    return "~";
    case UnaryOp::Deref:     return "*";
    case UnaryOp::AddrOf:    return "&";
    case UnaryOp::AddrOfMut: return "&mut";
    case UnaryOp::Not:       return "not";
  }
  return "<?>";
}

struct UnaryExpr : Expr {
  UnaryOp op;
  Ptr<Expr> operand;

  UnaryExpr() : Expr(NodeKind::UnaryExpr) {}
};

/// Field access: `expr.name`
struct FieldExpr : Expr {
  Ptr<Expr> object;
  std::string field_name;
  std::vector<Ptr<TypeExpr>> generic_args;  ///< For `expr.method[T]`

  FieldExpr() : Expr(NodeKind::FieldExpr) {}
};

/// Index: `expr[index]`
struct IndexExpr : Expr {
  Ptr<Expr> object;
  Ptr<Expr> index;

  IndexExpr() : Expr(NodeKind::IndexExpr) {}
};

/// Function/method call: `expr(args...)`
struct CallArg {
  Span span;
  std::optional<std::string> name;  ///< Named arg: `name: value`
  Ptr<Expr> value;
};

struct CallExpr : Expr {
  Ptr<Expr> callee;
  std::vector<CallArg> args;

  CallExpr() : Expr(NodeKind::CallExpr) {}
};

/// Type cast: `expr as Type`
struct CastExpr : Expr {
  Ptr<Expr> operand;
  Ptr<TypeExpr> target_type;

  CastExpr() : Expr(NodeKind::CastExpr) {}
};

/// Try propagation: `expr?` or `expr?.field`
struct TryExpr : Expr {
  Ptr<Expr> operand;

  TryExpr() : Expr(NodeKind::TryExpr) {}
};

/// Tuple literal: `(a, b, c)`
struct TupleExpr : Expr {
  std::vector<Ptr<Expr>> elements;

  TupleExpr() : Expr(NodeKind::TupleExpr) {}
};

/// Array literal: `[a, b, c]` or fill: `[val; count]`
struct ArrayExpr : Expr {
  std::vector<Ptr<Expr>> elements;
  Ptr<Expr> fill_value;  ///< Non-null for `[val; count]` form.
  Ptr<Expr> fill_count;

  ArrayExpr() : Expr(NodeKind::ArrayExpr) {}
};

/// Struct literal field: `name: expr` or shorthand `name`
struct StructFieldInit {
  Span span;
  std::string name;
  Ptr<Expr> value;        ///< nullptr for shorthand `{x}` (means `{x: x}`)
};

/// Struct literal: `{a: 1, b: 2}`
struct StructExpr : Expr {
  std::vector<StructFieldInit> fields;

  StructExpr() : Expr(NodeKind::StructExpr) {}
};

/// Lambda / closure: `x => x + 1` or `(a, b) -> int => a + b`
struct LambdaParam {
  Span span;
  Ptr<Node> pattern;               ///< The parameter pattern.
  Ptr<TypeExpr> type_annotation;   ///< Optional type annotation.
};

struct LambdaExpr : Expr {
  bool is_pure = false;
  bool is_move = false;
  std::vector<LambdaParam> params;
  Ptr<TypeExpr> return_type;        ///< Optional return type annotation.
  /// Body is either a single expression or a block (vector of statements).
  Ptr<Expr> body_expr;
  std::vector<Ptr<Node>> body_stmts;

  LambdaExpr() : Expr(NodeKind::LambdaExpr) {}
};

/// Module path expression: `a.b.c`
struct ModulePathExpr : Expr {
  std::vector<std::string> segments;

  ModulePathExpr() : Expr(NodeKind::ModulePathExpr) {}
};

/// Parenthesized expression: `(expr)`
struct GroupExpr : Expr {
  Ptr<Expr> inner;

  GroupExpr() : Expr(NodeKind::GroupExpr) {}
};

/// `if expr: ... elif ...: ... else: ...` (expression form)
struct IfBranch {
  Span span;
  Ptr<Expr> condition;
  std::vector<Ptr<Node>> body;  ///< Block of statements/exprs.
};

struct IfExpr : Expr {
  std::vector<IfBranch> branches;  ///< `if` + zero or more `elif`
  std::vector<Ptr<Node>> else_body;

  IfExpr() : Expr(NodeKind::IfExpr) {}
};

/// `match subject: ...`
struct MatchArm {
  Span span;
  Ptr<Node> pattern;
  Ptr<Expr> guard;  ///< Optional `if expr` guard.
  /// Body: inline expression or block.
  Ptr<Expr> body_expr;
  std::vector<Ptr<Node>> body_stmts;
  bool has_error = false;
};

struct MatchExpr : Expr {
  Ptr<Expr> subject;
  std::vector<MatchArm> arms;

  MatchExpr() : Expr(NodeKind::MatchExpr) {}
};

/// `for vars in iter [if guard] => yield_expr`
struct ForExpr : Expr {
  struct IterClause {
    std::vector<Ptr<Node>> patterns;
    Ptr<Expr> iterable;
  };
  std::vector<IterClause> clauses;
  Ptr<Expr> guard;         ///< Optional `if` filter.
  Ptr<Expr> yield_expr;    ///< The `=> expr` part.

  ForExpr() : Expr(NodeKind::ForExpr) {}
};

/// `await expr` or `await yield`
struct AwaitExpr : Expr {
  Ptr<Expr> operand;  ///< nullptr for `await yield`.
  bool is_yield = false;

  AwaitExpr() : Expr(NodeKind::AwaitExpr) {}
};

/// `par: ...`
struct ParExpr : Expr {
  std::vector<Ptr<Expr>> branches;

  ParExpr() : Expr(NodeKind::ParExpr) {}
};

/// `race: ...`
struct RaceExpr : Expr {
  std::vector<Ptr<Expr>> branches;

  RaceExpr() : Expr(NodeKind::RaceExpr) {}
};

/// `on(Type): block` or `on(Type, sender)`
struct OnExpr : Expr {
  Ptr<TypeExpr> context_type;
  Ptr<Expr> sender;         ///< Optional second argument.
  std::vector<Ptr<Node>> body;

  OnExpr() : Expr(NodeKind::OnExpr) {}
};

/// `block_expr` — `INDENT stmts DEDENT`
struct BlockExpr : Expr {
  std::vector<Ptr<Node>> stmts;

  BlockExpr() : Expr(NodeKind::BlockExpr) {}
};

/// Quasi-quote: `` `(content)` `` or `` `content` ``
struct QuoteExpr : Expr {
  std::vector<Token> tokens;  ///< Raw tokens inside the quote.

  QuoteExpr() : Expr(NodeKind::QuoteExpr) {}
};

/// Splice: `~(expr)` or `~ident`
struct SpliceExpr : Expr {
  Ptr<Expr> operand;

  SpliceExpr() : Expr(NodeKind::SpliceExpr) {}
};

/// `static expr`
struct StaticExpr : Expr {
  Ptr<Expr> operand;

  StaticExpr() : Expr(NodeKind::StaticExpr) {}
};

// ==========================================================================
//  Pattern nodes
// ==========================================================================

struct Pattern : Node {
  using Node::Node;
};

struct ErrorPattern : Pattern {
  explicit ErrorPattern(Span s = Span::dummy())
      : Pattern(NodeKind::WildcardPattern, s) {
    has_error = true;
  }
};

/// `_`
struct WildcardPattern : Pattern {
  WildcardPattern() : Pattern(NodeKind::WildcardPattern) {}
};

/// Literal in pattern position: `42`, `"hello"`, `true`
struct LiteralPattern : Pattern {
  TokenKind lit_kind;
  std::string value;

  LiteralPattern() : Pattern(NodeKind::LiteralPattern) {}
};

/// Simple name binding: `x`
struct BindingPattern : Pattern {
  std::string name;

  BindingPattern() : Pattern(NodeKind::BindingPattern) {}
};

/// Constructor: `Some(x)`, `Point(a, b)`
struct ConstructorPattern : Pattern {
  std::string name;
  std::vector<Ptr<Pattern>> args;

  ConstructorPattern() : Pattern(NodeKind::ConstructorPattern) {}
};

/// Tuple: `(a, b, c)`
struct TuplePattern : Pattern {
  std::vector<Ptr<Pattern>> elements;

  TuplePattern() : Pattern(NodeKind::TuplePattern) {}
};

/// Struct pattern field: `name: pattern`, `name`, or `..`
struct FieldPattern {
  Span span;
  std::string name;         ///< Empty for `..` (rest).
  Ptr<Pattern> pattern;     ///< nullptr for shorthand `{name}`.
  bool is_rest = false;     ///< True for `..`.
};

/// Struct pattern: `{x: a, y: b, ..}`
struct StructPattern : Pattern {
  std::vector<FieldPattern> fields;

  StructPattern() : Pattern(NodeKind::StructPattern) {}
};

/// Array/slice pattern: `[a, b, c]`
struct ArrayPattern : Pattern {
  std::vector<Ptr<Pattern>> elements;

  ArrayPattern() : Pattern(NodeKind::ArrayPattern) {}
};

/// Range: `1..10`, `1..=10`, `1..`, `..10`
struct RangePattern : Pattern {
  Ptr<Expr> start;  ///< nullptr for `..end`
  Ptr<Expr> end;    ///< nullptr for `start..`
  bool inclusive = false;  ///< True for `..=`

  RangePattern() : Pattern(NodeKind::RangePattern) {}
};

/// `some(pattern)`, `ok(pattern)`, `err(pattern)`
enum class OptionResultKind : uint8_t { Some, Ok, Err };

struct OptionPattern : Pattern {
  OptionResultKind option_kind;
  Ptr<Pattern> inner;

  OptionPattern() : Pattern(NodeKind::OptionPattern) {}
};

/// Result pattern — reuses OptionPattern but with different kind.
struct ResultPattern : Pattern {
  OptionResultKind result_kind;  ///< Ok or Err
  Ptr<Pattern> inner;

  ResultPattern() : Pattern(NodeKind::ResultPattern) {}
};

/// `&pattern`
struct RefPattern : Pattern {
  Ptr<Pattern> inner;

  RefPattern() : Pattern(NodeKind::RefPattern) {}
};

/// `a | b | c`
struct OrPattern : Pattern {
  std::vector<Ptr<Pattern>> alternatives;

  OrPattern() : Pattern(NodeKind::OrPattern) {}
};

/// `(pattern)` — parenthesized pattern for grouping.
struct GroupPattern : Pattern {
  Ptr<Pattern> inner;

  GroupPattern() : Pattern(NodeKind::GroupPattern) {}
};

// ==========================================================================
//  Statement nodes
// ==========================================================================

struct Stmt : Node {
  using Node::Node;
};

struct ErrorStmt : Stmt {
  explicit ErrorStmt(Span s = Span::dummy())
      : Stmt(NodeKind::ExprStmt, s) {
    has_error = true;
  }
};

/// `let pattern [: type] = expr`
/// `let pattern [: type] = expr else: block`
struct LetStmt : Stmt {
  Ptr<Pattern> pattern;
  Ptr<TypeExpr> type_annotation;  ///< Optional.
  Ptr<Expr> initializer;
  std::vector<Ptr<Node>> else_body;  ///< Non-empty for `let ... else:` form.

  LetStmt() : Stmt(NodeKind::LetStmt) {}
};

/// `var name [: type] = expr`
struct VarStmt : Stmt {
  std::string name;
  Ptr<TypeExpr> type_annotation;  ///< Optional.
  Ptr<Expr> initializer;

  VarStmt() : Stmt(NodeKind::VarStmt) {}
};

/// Assignment operators.
enum class AssignOp : uint8_t {
  Assign,     // =
  AddAssign,  // +=
  SubAssign,  // -=
  MulAssign,  // *=
  DivAssign,  // /=
  ModAssign,  // %=
  AndAssign,  // &=
  OrAssign,   // |=
  XorAssign,  // ^=
  ShlAssign,  // <<=
  ShrAssign,  // >>=
  AddWrapAssign,  // +%=
  SubWrapAssign,  // -%=
  MulWrapAssign,  // *%=
  AddSatAssign,   // +|=
  SubSatAssign,   // -|=
  MulSatAssign,   // *|=
};

struct AssignStmt : Stmt {
  Ptr<Expr> target;
  AssignOp op;
  Ptr<Expr> value;

  AssignStmt() : Stmt(NodeKind::AssignStmt) {}
};

/// Expression used as a statement.
struct ExprStmt : Stmt {
  Ptr<Expr> expr;

  ExprStmt() : Stmt(NodeKind::ExprStmt) {}
};

/// `return [expr]`
struct ReturnStmt : Stmt {
  Ptr<Expr> value;  ///< nullptr if bare `return`.

  ReturnStmt() : Stmt(NodeKind::ReturnStmt) {}
};

/// `if expr: block { elif expr: block } [else: block]`
struct IfStmt : Stmt {
  std::vector<IfBranch> branches;
  std::vector<Ptr<Node>> else_body;

  IfStmt() : Stmt(NodeKind::IfStmt) {}
};

/// `while expr: block` or `while let pattern = expr: block`
struct WhileStmt : Stmt {
  Ptr<Expr> condition;      ///< nullptr for `while let` form.
  Ptr<Pattern> let_pattern; ///< Non-null for `while let` form.
  Ptr<Expr> let_expr;       ///< The expr in `while let pat = expr`.
  std::vector<Ptr<Node>> body;

  WhileStmt() : Stmt(NodeKind::WhileStmt) {}
};

/// `for vars in expr [if guard]: block`
struct ForStmt : Stmt {
  std::vector<Ptr<Pattern>> patterns;
  Ptr<Expr> iterable;
  Ptr<Expr> guard;  ///< Optional `if` filter.
  std::vector<Ptr<Node>> body;

  ForStmt() : Stmt(NodeKind::ForStmt) {}
};

/// `match subject: ...`
struct MatchStmt : Stmt {
  Ptr<Expr> subject;
  std::vector<MatchArm> arms;

  MatchStmt() : Stmt(NodeKind::MatchStmt) {}
};

/// Crew option: `name: value`
struct CrewOption {
  Span span;
  std::string name;
  std::string value;
};

/// `crew name(options): block`
struct CrewStmt : Stmt {
  std::string name;
  std::vector<CrewOption> options;
  std::vector<Ptr<Node>> body;

  CrewStmt() : Stmt(NodeKind::CrewStmt) {}
};

/// `asm { content }`
struct AsmStmt : Stmt {
  std::string content;

  AsmStmt() : Stmt(NodeKind::AsmStmt) {}
};

/// `~expr`
struct SpliceStmt : Stmt {
  Ptr<Expr> expr;

  SpliceStmt() : Stmt(NodeKind::SpliceStmt) {}
};

// ==========================================================================
//  Declaration / item nodes
// ==========================================================================

/// `use` path handling.
struct UseItem {
  Span span;
  std::string name;
  std::optional<std::string> alias;  ///< `as` alias.
};

enum class UseSelectorKind : uint8_t {
  Single,    ///< `use a.b.Foo` or `use a.b.Foo as Bar`
  Group,     ///< `use a.b.{Foo, Bar}`
  Wildcard,  ///< `use a.b.*`
};

struct UseSelector {
  Span span;
  UseSelectorKind kind;
  std::vector<UseItem> items;  ///< For group selector; single has one item.
};

struct UseDecl : Node {
  Visibility visibility = Visibility::Default;
  std::vector<std::string> path;
  std::optional<UseSelector> selector;

  UseDecl() : Node(NodeKind::UseDecl) {}
};

/// `type Name[Params] = TypeDef`
struct TypeDecl : Node {
  Visibility visibility = Visibility::Default;
  std::string name;
  std::vector<TypeParam> type_params;
  /// The type definition body — one of: StructBody, SumBody, TypeExpr, RefinementType.
  Ptr<Node> definition;
  std::vector<std::string> deriving;
  Ptr<Expr> invariant;

  TypeDecl() : Node(NodeKind::TypeDecl) {}
};

/// Associated type in a trait: `type Name [= Default]`
struct AssociatedTypeDecl {
  Span span;
  Visibility visibility = Visibility::Default;
  std::string name;
  Ptr<TypeExpr> default_type;
};

/// Associated type in an impl: `type Name = ConcreteType`
struct AssociatedTypeDef {
  Span span;
  std::string name;
  Ptr<TypeExpr> type;
};

/// Trait declaration.
struct TraitDecl : Node {
  Visibility visibility = Visibility::Default;
  std::string name;
  std::vector<TypeParam> type_params;
  std::optional<Bound> requires_bound;
  std::vector<Ptr<Node>> items;  ///< FuncDecl, StaticDecl, AssociatedTypeDecl nodes.

  TraitDecl() : Node(NodeKind::TraitDecl) {}
};

/// Concept declaration.
struct ConceptParam {
  Span span;
  std::string name;
  bool is_higher_kinded = false;  ///< `Name[_]`
};

struct ConceptConstraint {
  Span span;
  Ptr<TypeExpr> subject;    ///< Non-null for `type : bound` constraints.
  Ptr<Node> bound_or_expr;  ///< Bound for type constraints, Expr for value constraints.
};

struct ConceptDecl : Node {
  Visibility visibility = Visibility::Default;
  std::string name;
  std::vector<ConceptParam> params;
  std::vector<ConceptConstraint> constraints;

  ConceptDecl() : Node(NodeKind::ConceptDecl) {}
};

/// Impl declaration.
struct ImplDecl : Node {
  std::vector<TypeParam> type_params;
  Ptr<TypeExpr> trait_type;
  Ptr<TypeExpr> for_type;
  std::vector<WhereConstraint> where_constraints;
  std::vector<Ptr<Node>> items;  ///< FuncDecl, StaticDecl, AssociatedTypeDef.

  ImplDecl() : Node(NodeKind::ImplDecl) {}
};

/// Function modifier flags.
struct FuncModifiers {
  bool is_pure = false;
  bool is_async = false;
  bool is_machine = false;
  bool is_static = false;
  Ptr<TypeExpr> async_context;  ///< Optional async context type.
};

/// Contract clause: `pre expr [, message]` or `post expr [, message]`
struct ContractClause {
  Span span;
  bool is_pre;  ///< true = `pre`, false = `post`
  Ptr<Expr> condition;
  std::optional<std::string> message;
};

/// Parameter in a function signature.
struct Param {
  Span span;
  Ptr<Pattern> pattern;
  Ptr<TypeExpr> type_annotation;  ///< Optional.
  Ptr<Expr> default_value;        ///< Optional.
};

/// Function declaration.
struct FuncDecl : Node {
  Visibility visibility = Visibility::Default;
  FuncModifiers modifiers;
  std::string name;
  std::vector<TypeParam> type_params;
  std::vector<Param> params;
  Ptr<TypeExpr> return_type;  ///< Optional.
  std::vector<WhereConstraint> where_constraints;
  std::vector<ContractClause> contracts;
  /// Body: either an inline expression or a block of statements.
  Ptr<Expr> body_expr;
  std::vector<Ptr<Node>> body_stmts;

  FuncDecl() : Node(NodeKind::FuncDecl) {}
};

/// Sub-module: `module Name` or `module Name: ...`
struct SubModuleDecl : Node {
  Visibility visibility = Visibility::Default;
  std::string name;
  std::vector<Ptr<Node>> items;  ///< Empty for forward declarations.

  SubModuleDecl() : Node(NodeKind::SubModuleDecl) {}
};

/// Dependency declaration: `dep Name: ...`
struct DepField {
  Span span;
  std::string key;
  std::string value;
};

struct DepDecl : Node {
  std::string name;
  std::vector<DepField> fields;

  DepDecl() : Node(NodeKind::DepDecl) {}
};

/// Static declarations — multiple forms:
///   - `static if expr: ...`
///   - `static Name : Type = expr`
///   - `static assert expr [, message]`
///   - `static for vars in expr [if guard] => expr`
///   - `static for vars in expr [if guard]: block`
enum class StaticDeclKind : uint8_t {
  ConditionalCompilation,  ///< `static if`
  Binding,                 ///< `static Name = expr`
  Assert,                  ///< `static assert expr`
  ForInline,               ///< `static for ... => expr`
  ForBlock,                ///< `static for ... : block`
};

struct StaticDecl : Node {
  Visibility visibility = Visibility::Default;
  StaticDeclKind decl_kind;

  // For Binding:
  std::string name;
  Ptr<TypeExpr> type_annotation;
  Ptr<Expr> initializer;

  // For Assert:
  Ptr<Expr> assert_condition;
  std::optional<std::string> assert_message;

  // For ConditionalCompilation:
  Ptr<Expr> if_condition;
  std::vector<Ptr<Node>> if_body;
  std::vector<Ptr<Node>> else_body;

  // For ForInline / ForBlock:
  std::vector<Ptr<Pattern>> for_patterns;
  Ptr<Expr> for_iterable;
  Ptr<Expr> for_guard;
  Ptr<Expr> for_yield;  ///< For ForInline.
  std::vector<Ptr<Node>> for_body;  ///< For ForBlock.

  StaticDecl() : Node(NodeKind::StaticDecl), decl_kind(StaticDeclKind::Binding) {}
};

// ==========================================================================
//  Module declaration and file root
// ==========================================================================

struct ModuleDecl : Node {
  std::vector<std::string> path;

  ModuleDecl() : Node(NodeKind::ModuleDecl) {}
};

struct File : Node {
  Ptr<ModuleDecl> module_decl;
  bool no_prelude = false;
  std::vector<Ptr<Node>> items;

  File() : Node(NodeKind::File) {}
};

// ==========================================================================
//  Pattern alias wrapper — for `pattern as name`
// ==========================================================================

struct AliasedPattern {
  Ptr<Pattern> pattern;
  std::optional<std::string> alias;
  Span span;
};

}  // namespace kira::ast