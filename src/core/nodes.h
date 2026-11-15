#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/parser/token.h"
#include "src/semantic/ids.h"
#include "src/semantic/type_table.h"

/// The typed core: the one program the checker hands to every later stage
/// (`spec/unified-checker-plan.md`). Nothing in it is implicit.
///
///   * Every expression has a settled type. A generic function keeps its type
///     and value parameters; an instance is the same tree with them replaced.
///   * Every call names its target (`callee`).
///   * Every read, write, borrow, copy and move names a `place` and says which
///     of those it is (`access`). There is no implicit borrow or dereference.
///   * Operators, method calls, conversions, default arguments and `for`
///     loops over iterators are calls.
///   * A `match` arm is a test over the subject's shape plus a list of
///     bindings, each a projection of the subject's place.
///
/// The core does not say where values are dropped: the ownership check decides
/// that from the core. It does not fix a layout: HIR lowering does.
///
/// Two checks read the core and nothing else: the core verifier
/// (`src/core/verify.h`), which re-checks every rule above without inference,
/// and the ownership check.
namespace cinder::core {

using semantic::k_unknown_type;
using semantic::symbol_id;
using semantic::type_id;

template <typename T> using ptr = std::unique_ptr<T>;
template <typename T> using ptr_vec = std::vector<ptr<T>>;

enum class node_kind : std::uint8_t {
  // expressions
  literal,
  use,        ///< Read a place by value: a copy or a move.
  borrow,     ///< `&place` / `&mut place`, written or inserted.
  call,
  function_ref, ///< A declared function used as a value.
  primitive,  ///< A builtin scalar operator; never a user impl.
  cast,
  tuple_init,
  array_init,
  struct_init,
  variant_init,
  lambda,
  block,
  if_expr,
  match_expr,
  await_expr,
  // statements
  let,
  assign,     ///< Write a value into a place.
  expr_stmt,
  while_loop,
  break_stmt,
  continue_stmt,
  return_stmt,
  yield_stmt,
  contract_check,
};

/// Common base. `span` is the source the node came from. Generated code keeps
/// the span of the quote in `span` and the splice that produced it in
/// `expansion`, so a diagnostic can name both.
struct node {
  node_kind kind;
  source_span span;
  std::optional<source_span> expansion;
  /// The AST node this was elaborated from; null for a synthesized node.
  /// Present only while the adapter builds the core from the checker's side
  /// tables (plan phases 2 to 11).
  const ast::node *origin = nullptr;

  node(node_kind k, source_span s) : kind(k), span(s) {}
  virtual ~node() = default;
  node(const node &) = delete;
  auto operator=(const node &) -> node & = delete;
  node(node &&) = default;
  auto operator=(node &&) -> node & = default;
};

/// A value-position node. `type` is never unknown or an error type.
///
/// An expression diverges when control never leaves it normally:
///
///   * an expression of type `never`;
///   * a block whose result diverges, or which has no result and ends in a
///     `return`, `break` or `continue`, or in a statement whose expression
///     diverges;
///   * an `if` with an `else`, or a `match`, whose every branch diverges;
///   * a block that ends in a `while_loop` with no condition, or the literal
///     `true` as its condition, and no `break` of that loop.
///
/// The type of a diverging expression is not compared with anything.
struct expr : node {
  type_id type = k_unknown_type;
  expr(node_kind k, source_span s, type_id t) : node(k, s), type(t) {}
};

struct stmt : node {
  using node::node;
};

// ==========================================================================
//  Places
// ==========================================================================

/// One step from a place to a part of it. `type` is the type of the place
/// after the step.
struct projection {
  enum class step : std::uint8_t {
    field,           ///< `.name` of a struct; `index` is the field's position.
    tuple_element,   ///< `.N` of a tuple.
    variant_payload, ///< Payload `index` of variant `name`, after a test.
    deref,           ///< `*` of a `&T`, `&mut T`, `*T` or `*mut T`.
    element,         ///< `[i]` of an array, slice, `str`, raw pointer or
                     ///< `uninit` buffer: a builtin element.
  };
  step kind;
  type_id type = k_unknown_type;
  std::string name;
  std::size_t index = 0;
  /// `element` only: the index. Evaluated once, before the access.
  ptr<expr> subscript;
  /// `element` only: the checker proved the index is in bounds.
  bool in_bounds = false;
};

/// Where a place starts.
struct place_root {
  enum class origin : std::uint8_t {
    local,     ///< A parameter, binding, or capture, named by `symbol`.
               ///< Symbols are unique within one function, lambdas included.
    global,    ///< A reified `static`, named by `name` in `module`.
    temporary, ///< The value of `value`, held until the statement ends.
               ///< It may be written and mutably borrowed.
  };
  origin kind = origin::local;
  symbol_id symbol = semantic::k_invalid_symbol_id;
  std::string name;
  std::string module;
  ptr<expr> value;
  type_id type = k_unknown_type;
};

/// A storage location: a root and the steps from it. Indexing a user
/// collection and dereferencing a user type are not steps: they are calls to
/// the `index`/`deref` impl whose `&T` result is a `temporary` root followed by
/// a `deref` step.
struct place {
  place_root root;
  std::vector<projection> path;

  [[nodiscard]] auto type() const -> type_id {
    return path.empty() ? root.type : path.back().type;
  }
};

/// Whether a place may be written, and why not if it may not. Computed once
/// by the elaborator (`place_mutability` in the plan's phase 4) and checked
/// again by the verifier from the root and path alone.
enum class writability : std::uint8_t {
  writable,
  immutable_binding, ///< Rooted at a `let`, or a by-value pattern binding.
  behind_shared_ref, ///< Reached through a `&T`, a `*T`, or a shared view.
  static_value,      ///< Rooted at a `static`.
};

// ==========================================================================
//  Expressions
// ==========================================================================

struct literal : expr {
  token_kind token;
  std::string spelling;
  literal(source_span s, type_id t) : expr(node_kind::literal, s, t) {}
};

/// Reads a place by value.
struct use : expr {
  enum class mode : std::uint8_t {
    copy, ///< The type is `copy`; the place keeps its value.
    move, ///< The place is left without a value.
  };
  place from;
  mode how = mode::copy;
  use(source_span s, type_id t) : expr(node_kind::use, s, t) {}
};

/// Borrows a place. `type` is `&T` or `&mut T`, or a view type for a range.
struct borrow : expr {
  place of;
  bool is_mut = false;
  /// The source did not write `&`: an argument to a reference parameter, a
  /// method receiver, a closure capture, or a `for` loop's source.
  bool inserted = false;
  /// A `&mut` receiver or argument that is not active until the call
  /// starts, so the other arguments may still read the place.
  bool two_phase = false;
  borrow(source_span s, type_id t) : expr(node_kind::borrow, s, t) {}
};

/// How a function treats its receiver. A property of the signature, read
/// the same way for every call. The mode fixes the type of the first
/// parameter, with `S` the type the method is on:
///
///   shared     `&S`      exclusive  `&mut S`
///   by_value   `S`, and `S` is `copy`
///   consuming  `S`, and the callee owns it
enum class receiver_mode : std::uint8_t {
  none,     ///< Not a method.
  shared,   ///< `self`
  exclusive, ///< `mut self`
  by_value, ///< A scalar `self`, passed by value.
  consuming, ///< Takes ownership of the receiver (`into_iter`).
};

/// What a call calls.
struct callee {
  enum class form : std::uint8_t {
    function,  ///< A declared function or method, or an instance of one.
    value,     ///< A closure or function value: `value` has the signature's
               ///< `fn` type, or a `&` of it. A call does not consume a
               ///< closure, so one held in a place is called through an
               ///< inserted borrow of that place.
    intrinsic, ///< A compiler intrinsic or host call named by `name`.
  };
  form kind = form::function;
  /// `function`: the declaration, and the template it was instantiated from
  /// when it is an instance.
  const ast::func_decl *decl = nullptr;
  const ast::func_decl *instance_of = nullptr;
  std::string name;
  std::string owner_module;
  /// The type an associated function or method was resolved on; empty for a
  /// free function.
  std::string impl_target;
  /// The trait whose method this implements; empty otherwise.
  std::string trait_name;
  /// The type and value arguments the call was solved with, in the
  /// template's parameter order. Empty for a non-generic callee.
  std::vector<type_id> generic_args;
  type_id signature = k_unknown_type; ///< The callee's `fn` type, substituted.
  receiver_mode receiver = receiver_mode::none;
  ptr<expr> value;
};

/// A call. `args` has one entry per parameter, in parameter order, with the
/// receiver first: defaults are filled in, and an argument to a reference
/// parameter is a `borrow`.
struct call : expr {
  callee target;
  ptr_vec<expr> args;
  call(source_span s, type_id t) : expr(node_kind::call, s, t) {}
};

/// A declared function named in value position: bound, passed or returned
/// without being called. `type` is the target's signature.
struct function_ref : expr {
  callee target;
  function_ref(source_span s, type_id t)
      : expr(node_kind::function_ref, s, t) {}
};

/// A builtin operator on scalars, or a comparison of raw pointers.
struct primitive : expr {
  std::optional<ast::binary_op> binary;
  std::optional<ast::unary_op> unary;
  ptr_vec<expr> operands;
  primitive(source_span s, type_id t) : expr(node_kind::primitive, s, t) {}
};

struct cast : expr {
  ptr<expr> operand;
  cast(source_span s, type_id t) : expr(node_kind::cast, s, t) {}
};

/// Tuple, array and struct construction. `elements` are evaluated in order.
/// A tuple's and an array's are in member order. A struct's are in source
/// order: `elements[i]` initializes the field named `field_names[i]`, whose
/// position in the declaration is `field_indices[i]`.
struct aggregate_init : expr {
  ptr_vec<expr> elements;
  std::vector<std::string> field_names;   ///< `struct_init` only.
  std::vector<std::size_t> field_indices; ///< `struct_init` only.
  /// `array_init` only: `[value; count]`.
  std::optional<std::uint64_t> repeat;
  aggregate_init(node_kind k, source_span s, type_id t) : expr(k, s, t) {}
};

struct variant_init : expr {
  std::string variant;
  std::size_t variant_index = 0;
  ptr_vec<expr> payload;
  variant_init(source_span s, type_id t)
      : expr(node_kind::variant_init, s, t) {}
};

/// A sequence of statements with an optional value.
struct block : expr {
  ptr_vec<stmt> statements;
  ptr<expr> result;
  block(source_span s, type_id t) : expr(node_kind::block, s, t) {}
};

/// One parameter of a function or lambda.
struct param {
  symbol_id symbol = semantic::k_invalid_symbol_id;
  std::string name;
  type_id type = k_unknown_type;
};

/// How a closure holds one value from its environment. A `copy` or `move`
/// capture is the closure's own value and may be written. Inside the body
/// `symbol` is a local of type `type`, which is the type of `from` in every
/// mode: a borrowed capture names the captured place, as an `alias` pattern
/// binding does. The body may root a local place only at its own parameters,
/// captures and bindings.
struct capture {
  enum class mode : std::uint8_t { copy, move, borrow, borrow_mut };
  symbol_id symbol = semantic::k_invalid_symbol_id; ///< Name inside the body.
  place from;
  mode how = mode::borrow;
  type_id type = k_unknown_type;
};

struct lambda : expr {
  std::vector<param> params;
  std::vector<capture> captures;
  type_id return_type = k_unknown_type;
  ptr<block> body;
  lambda(source_span s, type_id t) : expr(node_kind::lambda, s, t) {}
};

struct if_expr : expr {
  ptr<expr> condition;
  ptr<block> then_branch;
  ptr<block> else_branch; ///< Null when there is no `else`.
  if_expr(source_span s, type_id t) : expr(node_kind::if_expr, s, t) {}
};

// ==========================================================================
//  Patterns
// ==========================================================================

/// A structural test of a value. It binds no names.
struct pattern_test {
  enum class form : std::uint8_t {
    any,     ///< Matches everything.
    literal, ///< Equal to `low`.
    range,   ///< Between `low` and `high`; one of them may be absent.
    variant, ///< Is variant `variant_index`; `parts` test its payloads.
    tuple,   ///< `parts` test the elements.
    record,  ///< `parts` test the fields named by `field_indices`.
    array,   ///< `parts` test the elements.
  };
  form kind = form::any;
  source_span span;
  type_id subject_type = k_unknown_type;
  ptr<literal> low;
  ptr<literal> high;
  bool high_inclusive = false;
  std::size_t variant_index = 0;
  std::vector<std::size_t> field_indices;
  std::vector<pattern_test> parts;
};

/// One name a pattern binds: a part of the subject's place. `type` is the
/// type of that part in every mode. An `alias` binding is not a new value:
/// a place rooted at it is the part itself, so a write to it writes the
/// subject.
struct pattern_binding {
  enum class mode : std::uint8_t {
    copy,  ///< The part is `copy`.
    move,  ///< The part moves out of the subject.
    /// The part cannot be moved (verifier rule A2): it is behind a
    /// reference, an element, or part of a static. The name is that part.
    alias,
  };
  symbol_id symbol = semantic::k_invalid_symbol_id;
  std::string name;
  type_id type = k_unknown_type;
  /// Steps from the subject's place to the bound part.
  std::vector<projection> path;
  mode how = mode::copy;
  /// The name may be assigned through: declared `mut`, or an `alias` of a
  /// part reached through `&mut`.
  bool writable = false;
  source_span span;
};

/// One way an arm can match. An or-pattern has several; each binds the same
/// names at the same types.
struct pattern_alternative {
  pattern_test test;
  std::vector<pattern_binding> bindings;
};

struct match_arm {
  std::vector<pattern_alternative> alternatives;
  ptr<expr> guard; ///< Null when there is none.
  ptr<block> body;
  source_span span;
};

/// `match`, `if let`, `while let`'s test, `let ... else`, and a destructuring
/// `let` all elaborate to this. `subject` is evaluated once.
struct match_expr : expr {
  place subject;
  std::vector<match_arm> arms;
  match_expr(source_span s, type_id t) : expr(node_kind::match_expr, s, t) {}
};

struct await_expr : expr {
  ptr<expr> task;
  await_expr(source_span s, type_id t) : expr(node_kind::await_expr, s, t) {}
};

// ==========================================================================
//  Statements
// ==========================================================================

/// Introduces one local and gives it a value. A destructuring `let` is a
/// `match_expr` with one arm.
struct let : stmt {
  symbol_id symbol = semantic::k_invalid_symbol_id;
  std::string name;
  type_id type = k_unknown_type;
  bool is_mut = false;
  ptr<expr> value; ///< Null for a declaration assigned later.
  explicit let(source_span s) : stmt(node_kind::let, s) {}
};

/// Writes `value` into `target`. A compound assignment and an assignment
/// through a user `index_set` impl are elaborated before this point.
struct assign : stmt {
  place target;
  ptr<expr> value;
  explicit assign(source_span s) : stmt(node_kind::assign, s) {}
};

struct expr_stmt : stmt {
  ptr<expr> value;
  explicit expr_stmt(source_span s) : stmt(node_kind::expr_stmt, s) {}
};

/// Every loop. A `for` over a range, a collection or an iterator is a `let`
/// of its state followed by this.
struct while_loop : stmt {
  ptr<expr> condition; ///< Null for an unconditional loop.
  ptr<block> body;
  ptr<block> step; ///< Run after the body and on `continue`; may be null.
  explicit while_loop(source_span s) : stmt(node_kind::while_loop, s) {}
};

struct jump : stmt {
  ptr<expr> value; ///< `return`'s and `yield`'s value; null otherwise.
  jump(node_kind k, source_span s) : stmt(k, s) {}
};

/// A `pre`, `post` or `invariant` the checker could not prove.
struct contract_check : stmt {
  ptr<expr> condition;
  std::string message;
  explicit contract_check(source_span s)
      : stmt(node_kind::contract_check, s) {}
};

// ==========================================================================
//  Items
// ==========================================================================

enum class coroutine_kind : std::uint8_t { none, generator, task };

/// One function, method, generated impl method, or instance.
struct function {
  std::string name;
  std::string module;
  source_span span;
  const ast::func_decl *decl = nullptr;
  /// The template this is an instance of; null for a template or an ordinary
  /// function.
  const function *instance_of = nullptr;
  /// The type and value parameters still open in this body. Empty for an
  /// ordinary function and for an instance.
  std::vector<type_id> generic_params;
  std::vector<param> params;
  /// The type a caller sees: `generator[T]` for a `generator def`, the task
  /// type for an async function.
  type_id return_type = k_unknown_type;
  /// What `return` takes inside the body. Equal to `return_type` except in a
  /// coroutine: `unit` in a generator, the awaited type in a task.
  type_id body_type = k_unknown_type;
  /// `generator` only: what `yield` takes.
  type_id yield_type = k_unknown_type;
  receiver_mode receiver = receiver_mode::none;
  coroutine_kind coroutine = coroutine_kind::none;
  /// Produced by a `deriving` clause or an item splice.
  bool generated = false;
  ptr<block> body;
};

struct static_global {
  std::string name;
  std::string module;
  type_id type = k_unknown_type;
  ptr<expr> value;
};

/// An `intrinsic def`: a function the runtime supplies. An `intrinsic` callee
/// names either one of these or an operation of the compiler's own
/// (`src/core/intrinsics.h`).
struct intrinsic_decl {
  std::string name;
  /// The declared `fn` type. For a generic declaration it mentions the
  /// declaration's parameters, and a call's signature is an instance of it.
  type_id signature = k_unknown_type;
  bool generic = false;
};

/// One module's functions and statics. Type declarations, impl tables and
/// trait bounds stay in the session's type table and method table; the core
/// refers to them by `type_id` and by `callee`.
struct module {
  std::string name;
  std::optional<file_id_type> file_id;
  std::vector<ptr<function>> functions;
  std::vector<static_global> statics;
  /// The `intrinsic def`s this module declares.
  std::vector<intrinsic_decl> intrinsics;
};

} // namespace cinder::core
