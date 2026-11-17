#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/core/adapter.h"
#include "src/core/nodes.h"
#include "src/parser/ast.h"
#include "src/semantic/checker_results.h"
#include "src/semantic/ownership_cfg.h"

/// Internal to the adapter (`adapter.h`): the per-function builder that turns
/// one declaration's AST and the checker's side tables into a core function.
/// Split across `adapter_expr.cpp` (expressions, places, calls),
/// `adapter_stmt.cpp` (statements, blocks, functions, statics) and
/// `adapter_pattern.cpp` (patterns and the forms that elaborate to `match`).
namespace cinder::core {

/// Why a construct could not be built. Becomes a `gap`.
struct failure {
  std::string construct;
  std::string detail;
  source_span span;
};

template <typename T> using result = std::expected<T, failure>;

/// The core signature of a declaration: what a `callee` names and what the
/// core `function` built from the same declaration holds.
struct signature {
  std::vector<type_id> params;
  /// What a caller sees: the task type of an `async def`.
  type_id result = k_unknown_type;
  /// What `return` takes in the body (`function::body_type`).
  type_id body = k_unknown_type;
  type_id yield_type = k_unknown_type;
  coroutine_kind coroutine = coroutine_kind::none;
  receiver_mode receiver = receiver_mode::none;
  type_id fn_type = k_unknown_type;
};

/// The functions a module declares by name: the table `check_ufcs` consults
/// for a bare call (`program_index` members, plus a functor instance's
/// clones). `adapt_program` looks names up in it (`function_lookup`).
using function_table =
    std::unordered_map<std::string, const ast::func_decl *>;

/// Every generic template of the program the adapter builds, to the type
/// ids of its type and value parameters (those of an enclosing generic
/// `impl`/`extend` first), in the order `function::generic_params` holds.
using template_table =
    std::unordered_map<const ast::func_decl *, std::vector<type_id>>;

/// The members of a struct or sum type: `shapes` for a closed type,
/// `open_shapes` for one that mentions a generic parameter. Null when the
/// checker recorded neither.
[[nodiscard]] inline auto find_shape(const semantic::checked_types &checked,
                                     type_id type)
    -> const semantic::aggregate_shape * {
  if (const auto found = checked.shapes.find(type);
      found != checked.shapes.end()) {
    return &found->second;
  }
  if (const auto found = checked.open_shapes.find(type);
      found != checked.open_shapes.end()) {
    return &found->second;
  }
  return nullptr;
}

/// Builds the function `decl` as `name` in `module`. `functions` finds a
/// module's functions by name: this module's for a bare call, and
/// `std.fmt`'s, which string interpolation calls. `generic_params` are the
/// parameters still open in a template's body (empty for anything else);
/// `templates` are the program's templates, so a call to another template
/// from a generic body is known as a decision deferred to the instances.
[[nodiscard]] auto build_function(semantic::checked_types &checked,
                                  const function_lookup &functions,
                                  const template_table &templates,
                                  const ast::func_decl &decl, std::string name,
                                  std::string module,
                                  std::vector<type_id> generic_params,
                                  const adapt_options &options)
    -> result<ptr<function>>;

/// Builds the compile-time expression `expr` as a function of its own
/// (`adapt_expression`).
[[nodiscard]] auto
build_expression_function(semantic::checked_types &checked,
                          const function_lookup &functions,
                          const ast::expr &expr,
                          std::span<const expression_capture> captures,
                          std::string name, std::string module)
    -> result<ptr<function>>;

/// The declared `fn` type of an `intrinsic def`. `generic_params` are the
/// declaration's own parameters, which the type may mention.
[[nodiscard]] auto build_intrinsic_signature(
    semantic::checked_types &checked, const ast::func_decl &decl,
    std::vector<type_id> generic_params) -> result<type_id>;

/// Builds a reified `static let`'s value.
[[nodiscard]] auto build_static_value(semantic::checked_types &checked,
                                      const semantic::reified_value &value,
                                      source_span span) -> result<ptr<expr>>;

/// One function body being built. Every method reads the checker's records
/// for the AST node it is given and reports a `failure` when one is absent.
class body_builder {
public:
  body_builder(semantic::checked_types &checked,
               const function_lookup *functions = nullptr)
      : checked_(checked), functions_(functions) {}

  [[nodiscard]] auto function_of(const ast::func_decl &decl, std::string name,
                                 std::string module) -> result<ptr<function>>;
  /// Opens `params` for the body about to be built: `settle` accepts a type
  /// that mentions them, and the function holds them in `generic_params`.
  auto open_generic_params(std::vector<type_id> params,
                           const template_table *templates) -> void;
  /// Whether `type` mentions a parameter open in this template: a decision
  /// about it (an operator, a literal's type, a cast) is the instances'.
  [[nodiscard]] auto is_open(type_id type) const -> bool;
  [[nodiscard]] auto is_open_operand(const ast::expr &operand) const -> bool;
  /// `is_open`, except for a raw pointer, which any instance treats alike.
  [[nodiscard]] auto is_open_scalar(type_id type) const -> bool;

  [[nodiscard]] auto reified(const semantic::reified_value &value,
                             source_span span) -> result<ptr<expr>>;

  // ---- types (adapter_expr.cpp) -----------------------------------------
  [[nodiscard]] auto settle(type_id type, source_span span) -> result<type_id>;
  [[nodiscard]] auto type_of(const ast::node &node) -> result<type_id>;
  [[nodiscard]] auto signature_of(const ast::func_decl &decl)
      -> result<signature>;
  [[nodiscard]] auto strip_refs(type_id type) const -> type_id;
  [[nodiscard]] auto unit_type() -> type_id;
  [[nodiscard]] auto is_ref(type_id type) const -> bool;

  // ---- places and values (adapter_expr.cpp) ------------------------------
  [[nodiscard]] auto place_of(const ast::expr &source) -> result<place>;
  /// `place_of(expr)` followed by the `deref_adjustments` recorded on it.
  [[nodiscard]] auto receiver_place(const ast::expr &source) -> result<place>;
  /// The value of `source`, its `origin` set to `source` unless an inner
  /// expression it stands for (a parenthesized one, a splice) already set
  /// it: the ownership check names the innermost.
  [[nodiscard]] auto value_of(const ast::expr &source) -> result<ptr<expr>>;
  [[nodiscard]] auto value_of_node(const ast::expr &source)
      -> result<ptr<expr>>;
  /// `value_of(expr)` made to have type `need` (`coerce`).
  [[nodiscard]] auto value_as(const ast::expr &source, type_id need)
      -> result<ptr<expr>>;
  /// Makes one reference layer between `value` and `need` explicit, as
  /// `hir::make_references_explicit` does; anything else is left for the
  /// verifier to judge.
  [[nodiscard]] auto coerce(ptr<expr> value, type_id need) -> result<ptr<expr>>;
  /// The intrinsic that converts `have` to `need` when they are one of the
  /// four cell/reference pairs the checker accepts without a record
  /// (todo 38): `cell[T]`/`&T` and `cell_mut[T]`/`&mut T`, either way.
  [[nodiscard]] auto cell_conversion_name(type_id have, type_id need) const
      -> std::optional<std::string>;
  [[nodiscard]] auto is_cell(type_id type) const -> bool;
  /// The place a `cell[T]` or `cell_mut[T]` value addresses: a temporary
  /// holding its conversion to `&T`/`&mut T`, then a `deref` step.
  [[nodiscard]] auto cell_place(ptr<expr> cell) -> result<place>;
  [[nodiscard]] auto use_of(place from, source_span span) -> ptr<expr>;
  [[nodiscard]] auto temporary(ptr<expr> value) -> place;
  /// The place `value` reads, or a temporary holding it.
  [[nodiscard]] auto place_from_value(ptr<expr> value) -> place;
  auto add_deref(place &p) -> void;

  // ---- calls (adapter_expr.cpp) ------------------------------------------
  [[nodiscard]] auto callee_of(const semantic::resolved_callee &resolved,
                               source_span span) -> result<callee>;
  [[nodiscard]] auto argument(const ast::expr &arg, type_id param,
                              std::optional<semantic::param_passing> passing)
      -> result<ptr<expr>>;
  [[nodiscard]] auto argument_of(const ast::expr &arg, type_id param,
                              std::optional<semantic::param_passing> passing)
      -> result<ptr<expr>>;
  /// The receiver argument for a parameter of type `param`, from `p`.
  [[nodiscard]] auto receiver_argument(place p, type_id param, source_span span)
      -> ptr<expr>;
  /// The callee value of a call through a function value of type `fn_type`
  /// (rule C4): the value of a temporary, else a lending of the place.
  [[nodiscard]] auto function_value(const ast::expr &source, type_id fn_type)
      -> result<ptr<expr>>;
  /// The callee of a bare `f(x)` naming a function of the current module.
  /// `check_ufcs` types such a call against the declaration but records no
  /// `resolved_callees` entry (every other branch there does); this is the
  /// one place the adapter finds a callee by name. Phase 5 of the plan
  /// removes it, when the checker records every callee.
  [[nodiscard]] auto same_module_callee(const ast::call_expr &call)
      -> std::optional<result<callee>>;
  [[nodiscard]] auto call_of(const ast::call_expr &call) -> result<ptr<expr>>;
  [[nodiscard]] auto builtin_method(const ast::call_expr &call,
                                    const ast::field_expr &field)
      -> std::optional<result<ptr<expr>>>;
  [[nodiscard]] auto intrinsic_call(std::string name, type_id type,
                                    ptr_vec<expr> args,
                                    std::vector<type_id> generic_args,
                                    source_span span) -> ptr<expr>;
  [[nodiscard]] auto direct_call(callee target, ptr_vec<expr> args,
                                 source_span span) -> ptr<expr>;
  /// A call to `resolved` with `receiver` and `rest` as its arguments, in
  /// parameter order, each passed as the callee's parameter type says.
  [[nodiscard]] auto dispatch_call(const semantic::resolved_callee &resolved,
                                   const ast::expr &receiver,
                                   std::vector<const ast::expr *> rest,
                                   source_span span) -> result<ptr<expr>>;
  [[nodiscard]] auto apply_deref_steps(place p,
                                       const std::vector<semantic::deref_step>
                                           &steps,
                                       source_span span) -> result<place>;

  // ---- expressions (adapter_expr.cpp) ------------------------------------
  [[nodiscard]] auto literal_of(const ast::literal_expr &lit)
      -> result<ptr<expr>>;
  [[nodiscard]] auto ident_value(const ast::ident_expr &ident)
      -> result<ptr<expr>>;
  [[nodiscard]] auto static_value(const ast::expr &ref)
      -> std::optional<result<ptr<expr>>>;
  /// Whether the read `ref` of a reified static is a fresh value of its
  /// own: one the reader owns (`owned_static_reads`), or any read in a
  /// compile-time expression.
  [[nodiscard]] auto fresh_static_read(const ast::expr &ref) -> bool;
  [[nodiscard]] auto owned_static_reads()
      -> const std::unordered_set<const void *> &;
  /// The fresh value of the reified static `name` (`static_global_defs`).
  [[nodiscard]] auto static_global_value(const std::string &name,
                                         source_span span) -> result<ptr<expr>>;
  // ---- compile-time syntax (adapter_expr.cpp) ----------------------------
  /// A `host_call` building syntax, in compile-time code; a gap elsewhere.
  [[nodiscard]] auto host_call_of(std::string operation, const void *subject,
                                  ptr_vec<expr> args, type_id type,
                                  source_span span) -> result<ptr<expr>>;
  [[nodiscard]] auto quote_of(const ast::quote_expr &quote)
      -> result<ptr<expr>>;
  /// `expr.<name>(args...)`, a builder of quoted syntax.
  [[nodiscard]] auto fragment_builder_of(const ast::call_expr &call,
                                         std::string_view name, type_id type)
      -> result<ptr<expr>>;
  /// A read of a `static` that holds syntax (`static_fragment_refs`).
  [[nodiscard]] auto static_fragment_of(const ast::expr &ref)
      -> std::optional<result<ptr<expr>>>;
  [[nodiscard]] auto binary_of(const ast::binary_expr &bin)
      -> result<ptr<expr>>;
  [[nodiscard]] auto unary_of(const ast::unary_expr &un) -> result<ptr<expr>>;
  // ---- interpolation (adapter_interp.cpp) ------------------------------
  /// `"...{x:spec}..."`: each segment rendered by its `interp_dispatches`
  /// entry through `std.fmt`'s helpers, then joined with `rt_str_concat`,
  /// as lowering builds it.
  [[nodiscard]] auto interpolation_of(const ast::interpolated_string_expr &node)
      -> result<ptr<expr>>;
  /// A call of the `std.fmt` helper `name`. The interpolation protocol fixes
  /// which helper renders which segment; the checker records no callee.
  [[nodiscard]] auto fmt_call(std::string_view name, ptr_vec<expr> args,
                              source_span span) -> result<ptr<expr>>;
  [[nodiscard]] auto format_spec_of(const ast::interp_segment &seg,
                                    source_span span) -> result<ptr<expr>>;
  [[nodiscard]] auto await_of(const ast::await_expr &awaited)
      -> result<ptr<expr>>;
  /// The task type an `async def` whose body returns `body` hands its
  /// caller: `task[T, E, io]` for `result[T, E]`, else `task[body, never,
  /// io]`. The checker's `task_of_declared_result`, which records no table.
  [[nodiscard]] auto task_of(type_id body) -> type_id;
  /// `&p[i]` / `&mut p[i]` on a raw pointer: the `ptr_offset` intrinsic.
  [[nodiscard]] auto element_address(const ast::unary_expr &un, type_id type)
      -> result<ptr<expr>>;
  /// A range's start and end. With `exclusive_end`, `a..=b` gives `b + 1`
  /// as the end; without it, `b` as written.
  [[nodiscard]] auto range_bounds(const ast::binary_expr &range,
                                  bool exclusive_end = true)
      -> result<std::pair<ptr<expr>, ptr<expr>>>;
  /// `a..b` as a value: a `range[T]` struct built from its two bounds.
  [[nodiscard]] auto range_value(const ast::binary_expr &range, type_id type)
      -> result<ptr<expr>>;
  /// `xs[a..b]` on an `array`, `slice`, `slice_mut` or `str`: the
  /// `range_view` intrinsic over the container and the two bounds, or
  /// `range_view_inclusive` for `xs[a..=b]`.
  [[nodiscard]] auto range_index(const ast::index_expr &index,
                                 const ast::binary_expr &range, type_id type)
      -> result<ptr<expr>>;
  [[nodiscard]] auto struct_of(const ast::struct_expr &literal)
      -> result<ptr<expr>>;
  [[nodiscard]] auto tuple_of(const ast::tuple_expr &tuple)
      -> result<ptr<expr>>;
  [[nodiscard]] auto array_of(const ast::array_expr &array)
      -> result<ptr<expr>>;
  [[nodiscard]] auto array_value(const ast::array_expr &array, type_id type)
      -> result<ptr<expr>>;
  [[nodiscard]] auto variant_of(const std::string &name, type_id type,
                                const std::vector<const ast::expr *> &args,
                                source_span span) -> result<ptr<expr>>;
  [[nodiscard]] auto function_ref_of(const semantic::resolved_callee &resolved,
                                     source_span span) -> result<ptr<expr>>;
  [[nodiscard]] auto where_of(const ast::where_expr &where)
      -> result<ptr<expr>>;
  [[nodiscard]] auto ordering_to_bool(ptr<expr> ordering, ast::binary_op op,
                                      source_span span) -> result<ptr<expr>>;
  [[nodiscard]] auto variant_index(type_id type, std::string_view name,
                                   source_span span) -> result<std::size_t>;
  [[nodiscard]] auto bool_literal(bool value, source_span span) -> ptr<expr>;

  // ---- statements (adapter_stmt.cpp) -------------------------------------
  /// `stmts` as a block of type `type`. A non-unit block takes its last
  /// expression statement, or a trailing `if`/`match`, as its result.
  /// The block's `origin` is `&stmts`, the key of its scope.
  [[nodiscard]] auto block_of(const std::vector<ast::ptr<ast::node>> &stmts,
                              source_span span, type_id type)
      -> result<ptr<block>>;
  /// `block_of` for statements that are not a whole AST block (the rest of
  /// one after a `let ... else`): the block is no scope of its own.
  [[nodiscard]] auto block_from(std::span<const ast::ptr<ast::node>> stmts,
                                source_span span, type_id type,
                                const void *key) -> result<ptr<block>>;
  /// Appends `node`'s statements to `out`. A destructuring `let` is handled
  /// by `block_of`, which hands it the rest of its block.
  auto stmt_into(const ast::node &node, ptr_vec<stmt> &out) -> result<void>;
  [[nodiscard]] auto if_of(const std::vector<ast::if_branch> &branches,
                           const std::vector<ast::ptr<ast::node>> &else_body,
                           std::size_t index, source_span span, type_id type)
      -> result<ptr<expr>>;
  [[nodiscard]] auto expr_stmt_of(ptr<expr> value, source_span span)
      -> ptr<stmt>;
  auto assign_into(const ast::assign_stmt &assign, ptr_vec<stmt> &out)
      -> result<void>;
  /// Binds each part of `target` that evaluating it again would repeat (a
  /// temporary root, a subscript that is not a literal or a local) with a
  /// `let` appended to `out`, and reads that local in its place.
  auto bind_impure_parts(place &target, source_span span, ptr_vec<stmt> &out)
      -> void;
  /// A copy of `p`, whose root is not a temporary and whose subscripts are
  /// literals or locals (`bind_impure_parts`).
  [[nodiscard]] auto pure_copy(const place &p) -> place;
  /// Whether `let` needs a `match` (a destructuring pattern or an `else`).
  [[nodiscard]] static auto is_pattern_let(const ast::node &node) -> bool;

  // ---- patterns (adapter_pattern.cpp) ------------------------------------
  [[nodiscard]] auto match_of(const ast::expr &subject,
                              const std::vector<ast::match_arm> &arms,
                              source_span span, type_id type)
      -> result<ptr<expr>>;
  /// What rule A2 and P4 say about a place, from its root and path: whether
  /// it could be moved out of, whether it is writable, and whether it was
  /// reached through a reference.
  struct access_state {
    bool movable = true;
    bool writable = false;
    bool through_ref = false;
  };
  [[nodiscard]] auto access_of(const place &p) const -> access_state;
  /// `state` after `step`, taken from a part of type `before`.
  auto walk_access(access_state &state, type_id before,
                   const projection &step) const -> void;
  /// What a pattern's bindings share: the names already given symbols in
  /// this arm (every `|` alternative binds the same symbols), and the access
  /// to the subject's place.
  struct binding_context {
    std::unordered_map<std::string, symbol_id> symbols;
    access_state subject_access;
    type_id subject_type = k_unknown_type;
    /// The `deref` steps `subject_of` added (`match_expr::subject_derefs`).
    std::size_t subject_derefs = 0;
  };
  /// One arm alternative for `pattern` against a subject of type `subject`.
  [[nodiscard]] auto alternative_of(const ast::node &pattern, type_id subject,
                                    binding_context &context)
      -> result<pattern_alternative>;
  /// The test for `pattern` against the part at `path`, of type `type`.
  [[nodiscard]] auto test_of(const ast::node &pattern, type_id type,
                             const std::vector<projection> &path,
                             binding_context &context,
                             std::vector<pattern_binding> &bindings)
      -> result<pattern_test>;
  /// `test` as an array test of `elements`, each the element at its
  /// position.
  [[nodiscard]] auto array_test_of(
      const std::vector<ast::ptr<ast::pattern>> &elements,
      const std::vector<projection> &path, binding_context &context,
      std::vector<pattern_binding> &bindings, pattern_test test,
      source_span span) -> result<pattern_test>;
  /// A binding of the part at `path` under `name`.
  [[nodiscard]] auto binding_of(const std::string &name, const void *key,
                                bool is_mut, type_id type,
                                const std::vector<projection> &path,
                                source_span span, binding_context &context)
      -> pattern_binding;
  /// The subject of a matching form, dereferenced to the value it names.
  [[nodiscard]] auto subject_of(const ast::expr &subject,
                                binding_context &context) -> result<place>;
  /// An arm for `alternatives`, with `body` built in a scope holding their
  /// bindings. `origin` is the AST pattern the arm matches
  /// (`match_arm::origin`).
  [[nodiscard]] auto
  arm_of(std::vector<pattern_alternative> alternatives,
         const ast::expr *guard,
         const std::function<result<ptr<block>>()> &body, source_span span,
         const void *origin) -> result<match_arm>;
  /// An arm that matches anything and runs `body`.
  [[nodiscard]] auto fallback_arm(type_id subject, ptr<block> body,
                                  source_span span) -> match_arm;
  [[nodiscard]] auto if_let_of(const std::vector<ast::if_branch> &branches,
                               const std::vector<ast::ptr<ast::node>> &else_body,
                               std::size_t index, source_span span,
                               type_id type) -> result<ptr<expr>>;
  [[nodiscard]] auto while_let_of(const ast::while_stmt &loop)
      -> result<ptr<stmt>>;
  /// A destructuring `let` or `let ... else`. `rest` are the statements
  /// after it in its block; they move into the matching arm, a block keyed
  /// `scope_key` like the block they came from.
  [[nodiscard]] auto let_pattern_of(const ast::let_stmt &let,
                                    std::span<const ast::ptr<ast::node>> rest,
                                    source_span block_span, type_id block_type,
                                    const void *scope_key)
      -> result<ptr<expr>>;
  [[nodiscard]] auto or_else_of(const ast::or_else_expr &or_else)
      -> result<ptr<expr>>;
  [[nodiscard]] auto try_of(const ast::try_expr &try_expr)
      -> result<ptr<expr>>;
  /// Declares `bindings` in the current scope.
  auto bind_all(const std::vector<pattern_binding> &bindings) -> void;

  // ---- loops (adapter_loop.cpp) ------------------------------------------
  /// Builds a loop body in the scope of the loop variables' bindings.
  using loop_body = std::function<result<ptr<block>>()>;
  /// A `let` of a synthesized local, declared in the current scope.
  [[nodiscard]] auto let_of(std::string name, type_id type, bool is_mut,
                            ptr<expr> value, source_span span) -> ptr<let>;
  [[nodiscard]] auto read_local(const let &bound, source_span span)
      -> ptr<expr>;
  [[nodiscard]] auto int_literal(std::uint64_t value, type_id type,
                                 source_span span) -> ptr<expr>;
  /// The test for a loop head's patterns (one pattern, or the elements of a
  /// tuple for `for k, v in ...`) against the part at `path`.
  [[nodiscard]] auto head_test(std::span<const ast::node *const> patterns,
                               type_id type,
                               const std::vector<projection> &path,
                               binding_context &context,
                               std::vector<pattern_binding> &bindings)
      -> result<pattern_test>;
  /// `match element: patterns => body`, one arm.
  [[nodiscard]] auto bind_element(place element,
                                  std::span<const ast::node *const> patterns,
                                  const loop_body &body, source_span span)
      -> result<ptr<expr>>;
  /// `while_loop{null, match next: @some(patterns) => body, _ => break}`.
  [[nodiscard]] auto next_loop(ptr<expr> next, type_id element_type,
                               std::span<const ast::node *const> patterns,
                               const loop_body &body, source_span span)
      -> result<ptr<stmt>>;
  /// `while counter < bound (or <=): body`, stepping `counter` by one.
  [[nodiscard]] auto counting_loop(const let &counter, ptr<expr> bound,
                                   bool inclusive, ptr<block> body,
                                   source_span span) -> ptr<stmt>;
  /// The `len` intrinsic of a builtin container.
  [[nodiscard]] auto container_len(place container, source_span span)
      -> ptr<expr>;
  /// One loop over `iterable` binding `patterns`, as a unit block holding
  /// the loop state and the `while_loop`. `dispatch` is the iterator record
  /// when the checker left one. `handle_key` (the loop's `iterable` member)
  /// keys the block and the iterator the loop holds; `loop_key` (the `for`
  /// statement or comprehension clause) is the `while_loop`'s origin.
  [[nodiscard]] auto loop_of(const ast::expr &iterable,
                             std::span<const ast::node *const> patterns,
                             const semantic::iterator_loop_dispatch *dispatch,
                             const loop_body &body, source_span span,
                             const void *handle_key, const void *loop_key)
      -> result<ptr<expr>>;
  [[nodiscard]] auto guarded_body(const ast::expr *guard,
                                  const std::vector<ast::ptr<ast::node>> &stmts,
                                  source_span span) -> result<ptr<block>>;
  [[nodiscard]] auto for_of(const ast::for_stmt &loop) -> result<ptr<stmt>>;
  /// A call of a recorded `list[T]::new`/`push`, its arguments made to fit.
  [[nodiscard]] auto list_builder_call(const semantic::resolved_callee &resolved,
                                       ptr_vec<expr> args, source_span span)
      -> result<ptr<expr>>;
  [[nodiscard]] auto push_onto(const let &acc,
                               const semantic::comprehension_dispatch &dispatch,
                               ptr<expr> value, source_span span)
      -> result<ptr<stmt>>;
  [[nodiscard]] auto comprehension_of(const ast::for_expr &comprehension)
      -> result<ptr<expr>>;
  [[nodiscard]] auto runtime_fill_of(const ast::array_expr &array,
                                     const semantic::comprehension_dispatch
                                         &dispatch) -> result<ptr<expr>>;

  // ---- contracts and invariants (adapter_contract.cpp) --------------------
  [[nodiscard]] auto contract_check_of(const ast::expr &condition,
                                       std::string message, source_span span,
                                       contract_check::clause kind)
      -> result<ptr<stmt>>;
  /// The checks of the current function's run-time postconditions.
  auto post_checks_into(ptr_vec<stmt> &out) -> result<void>;
  /// `return value`, through the postconditions when there are any:
  /// `{ let return = value; <checks>; return return }`.
  /// `origin` is the AST node the `return` jump carries.
  [[nodiscard]] auto return_of(ptr<expr> value, source_span span,
                               const void *origin = nullptr)
      -> result<ptr<stmt>>;
  /// Puts `decl`'s run-time preconditions first in `body`, and its
  /// postconditions on the exit by the body's end.
  auto contracts_into(const ast::func_decl &decl, block &body) -> result<void>;
  [[nodiscard]] auto invariant_of(type_id type) const -> const ast::expr *;
  /// `let self = self_value` and the check of `invariant` against it.
  [[nodiscard]] auto invariant_check_of(const ast::expr &invariant,
                                        ptr<expr> self_value, source_span span,
                                        ptr_vec<stmt> &out)
      -> result<symbol_id>;
  /// `value`, checked against its type's invariant when it has one:
  /// `{ let self = value; <check>; self }`.
  [[nodiscard]] auto checked_construction(ptr<expr> value) -> result<ptr<expr>>;
  /// `p` again, when it can be read again: no temporary, no element.
  [[nodiscard]] auto clone_place(const place &p, source_span span)
      -> result<place>;
  /// After a write to `target`: when it is a field of a type with an
  /// invariant, the check of that invariant against a borrow of the object.
  auto field_write_checks_into(const place &target, source_span span,
                               ptr_vec<stmt> &out) -> result<void>;

  // ---- static if / static for (adapter_static.cpp) ------------------------
  /// The literal the checker folded `source` to in the current `static for`
  /// iteration, unless a local shadows the binding it reads.
  [[nodiscard]] auto static_fold(const ast::expr &source) const
      -> const ast::expr *;
  /// The branch a `static if` takes: the current iteration's decision when
  /// it reads the loop bindings, else `static_if_taken_branch`.
  [[nodiscard]] auto static_if_taken(const ast::static_decl &decl)
      -> result<bool>;
  /// One unrolled iteration: a block of a `let` per binding with a literal
  /// form, then `body` built with the iteration's folds and decisions.
  [[nodiscard]] auto
  iteration_of(const semantic::static_for_iteration &iteration,
               source_span span,
               const std::function<result<ptr<expr>>()> &body)
      -> result<ptr<expr>>;
  auto static_for_block_into(const ast::static_decl &decl, ptr_vec<stmt> &out)
      -> result<void>;
  /// An inline `static for`: the list lowering builds with the recorded
  /// `new`/`push` pair, from the recorded elements or iterations.
  [[nodiscard]] auto static_for_inline_of(const ast::static_decl &decl)
      -> result<ptr<expr>>;
  /// A `static` statement in a body: assertion, `static if`, `static for`.
  auto static_decl_into(const ast::static_decl &decl, ptr_vec<stmt> &out)
      -> result<void>;
  /// A `static` statement as the value of a block of type `type`; none when
  /// the form has no value.
  [[nodiscard]] auto static_tail_of(const ast::static_decl &decl, type_id type)
      -> std::optional<result<ptr<expr>>>;

  // ---- scopes -------------------------------------------------------------
  struct local_info {
    symbol_id symbol = semantic::k_invalid_symbol_id;
    type_id type = k_unknown_type;
    /// A place rooted here is writable (P4).
    bool writable = false;
    /// The name is another place (an `alias` binding or a borrowed
    /// capture), so nothing moves out of it (A2).
    bool pinned = false;
  };
  auto push_scope() -> void { scopes_.emplace_back(); }
  auto pop_scope() -> void { scopes_.pop_back(); }
  auto declare(const std::string &name, type_id type, bool writable = false,
               bool pinned = false) -> symbol_id;
  /// The local `name` names. Found outside a lambda being built, it is
  /// captured into each lambda it crosses.
  [[nodiscard]] auto lookup(const std::string &name)
      -> std::optional<local_info>;
  [[nodiscard]] auto mint() -> symbol_id { return next_symbol_++; }

  // ---- concurrency (adapter_task.cpp) ------------------------------------
  /// A crew whose body is being built: `crew_end` runs on every exit from it.
  struct open_crew {
    symbol_id symbol = 0;
    std::string name;
  };
  /// `task_block(fn() -> declared: body)`, the task an `async:` or `on(...)`
  /// block makes; its captures are the `task_capture` records under `key`.
  [[nodiscard]] auto task_block_of(const void *key,
                                   const std::vector<ast::ptr<ast::node>> &body,
                                   type_id declared, source_span span)
      -> result<ptr<expr>>;
  /// What a task of `task_type` returns: `T`, or `result[T, E]`.
  [[nodiscard]] auto task_result_type(type_id task_type, source_span span)
      -> result<type_id>;
  /// `on_cpu(task)` or `on_io(task)`, the task made from the sender or body.
  [[nodiscard]] auto on_task_of(const ast::on_expr &on, type_id declared,
                                type_id task_type) -> result<ptr<expr>>;
  /// `await` of `par`, `race` or `on(...)`; nullopt for any other operand.
  [[nodiscard]] auto await_task_form(const ast::await_expr &awaited)
      -> std::optional<result<ptr<expr>>>;
  /// `{ let name = crew_new(); body; crew_end(name) }`.
  /// `key` is the crew statement or expression.
  [[nodiscard]] auto crew_of(const void *key, const std::string &name,
                             const std::vector<ast::ptr<ast::node>> &body,
                             source_span span, type_id type)
      -> result<ptr<expr>>;
  [[nodiscard]] auto crew_end_of(const open_crew &crew, source_span span)
      -> ptr<stmt>;
  /// `crew_end` for each crew a jump leaves: all of them for a `return`,
  /// those inside the innermost loop for `break` and `continue`.
  auto crew_exits_into(bool loop_exit, source_span span, ptr_vec<stmt> &out)
      -> void;
  /// `spawn`, `get` and `errors` on a task or crew; nullopt for others.
  [[nodiscard]] auto task_method(const ast::call_expr &call,
                                 const ast::field_expr &field, type_id type)
      -> std::optional<result<ptr<expr>>>;
  std::vector<open_crew> open_crews_;
  std::size_t crews_outside_loop_ = 0;

  // ---- lambdas (adapter_lambda.cpp) --------------------------------------
  /// A lambda being built: where its scopes start, and the captures made so
  /// far. A name looked up from inside it and found below `scope_base` is
  /// captured, with a fresh symbol, the first time it is used (rule E10).
  struct lambda_frame {
    /// Null for a task block, whose captures `task_key` names.
    const ast::lambda_expr *source = nullptr;
    const void *task_key = nullptr;
    std::size_t scope_base = 0;
    std::vector<capture> captures;
    /// A name the body uses that its written capture list does not hold.
    std::optional<std::string> unlisted;
  };
  /// The ownership checker's capture records for the function being built:
  /// what each lambda without a capture list borrows.
  using borrowed_records =
      std::unordered_map<const void *,
                         std::vector<semantic::ownership::borrowed_capture>>;
  [[nodiscard]] auto lambda_of(const ast::lambda_expr &lambda)
      -> result<ptr<expr>>;
  /// Captures `outer` (found outside `frame`) into `frame` as `name`.
  auto capture_into(lambda_frame &frame, const std::string &name,
                    const local_info &outer) -> local_info;
  [[nodiscard]] auto capture_mode_of(const lambda_frame &frame,
                                     const std::string &name, type_id type)
      -> capture::mode;
  [[nodiscard]] auto borrowed_captures() -> const borrowed_records &;
  /// The ownership checker's `task_capture` records: what each `async:` or
  /// `on(...)` block of the function being built captures, and whether it
  /// owns it.
  using task_records =
      std::unordered_map<const void *,
                         std::vector<semantic::ownership::task_capture>>;
  [[nodiscard]] auto task_captures() -> const task_records &;
  auto load_capture_records() -> void;
  /// `let f = <generic lambda>`: one local per copy the checker made.
  [[nodiscard]] auto generic_lambda_let(const ast::let_stmt &let,
                                        const ast::lambda_expr &written,
                                        ptr_vec<stmt> &out) -> result<void>;
  /// The local name a use of a generic lambda's copy is declared under.
  [[nodiscard]] static auto copy_name(const std::string &name,
                                      const ast::lambda_expr &copy)
      -> std::string;


  /// Whether contracts and invariants are checked at run time
  /// (`adapt_options::contract_checks`).
  bool contract_checks_ = true;
  semantic::checked_types &checked_;
  /// Finds a module's functions by name; null when nothing is looked up.
  const function_lookup *functions_ = nullptr;
  /// The code runs only while compiling (`adapt_options::compile_time`).
  bool compile_time_ = false;
  /// Building a compile-time expression's function: every read of a
  /// reified static is a fresh value, as the old lowerer makes it.
  bool fresh_static_reads_ = false;
  /// The reads of a reified static the reader owns
  /// (`function_cfg::owned_static_reads`), loaded with the capture records.
  std::optional<std::unordered_set<const void *>> owned_static_reads_;
  std::string module_name_;
  std::vector<std::unordered_map<std::string, local_info>> scopes_;
  std::vector<lambda_frame> lambdas_;
  const ast::func_decl *decl_ = nullptr;
  std::optional<borrowed_records> borrowed_captures_;
  std::optional<task_records> task_captures_;
  /// Each generic lambda's name, to the local names of its copies.
  std::unordered_map<std::string, std::vector<std::string>>
      generic_lambda_names_;
  /// The `static for` iterations being built, innermost last, and the
  /// folded reads of the innermost.
  std::vector<const semantic::static_for_iteration *> static_frames_;
  const std::unordered_map<const ast::expr *, const ast::expr *>
      *static_folds_ = nullptr;
  /// The run-time postconditions of the function being built; empty
  /// inside a lambda.
  std::vector<const ast::contract_clause *> post_contracts_;
  /// The parameters open in a template's body; empty otherwise.
  std::vector<type_id> generic_params_;
  std::unordered_set<type_id> open_params_;
  const template_table *templates_ = nullptr;
  symbol_id next_symbol_ = 1;
  type_id body_type_ = k_unknown_type;
  type_id yield_type_ = k_unknown_type;
  std::unordered_map<const ast::func_decl *, signature> signatures_;
};

[[nodiscard]] inline auto fail(std::string construct, std::string detail,
                               source_span span) -> std::unexpected<failure> {
  return std::unexpected(failure{.construct = std::move(construct),
                                 .detail = std::move(detail),
                                 .span = span});
}

/// The AST node kind's name, for a gap named after it.
[[nodiscard]] auto kind_name(ast::node_kind kind) -> std::string;

template <typename T, typename... Args>
[[nodiscard]] auto make_node(Args &&...args) -> ptr<T> {
  return std::make_unique<T>(std::forward<Args>(args)...);
}

} // namespace cinder::core

/// Unwraps a `result`, returning its failure from the enclosing function.
#define CORE_TRY(var, expr)                                                    \
  auto var##_result = (expr);                                                  \
  if (!var##_result.has_value()) {                                             \
    return std::unexpected(std::move(var##_result.error()));                   \
  }                                                                            \
  auto var = std::move(*var##_result)

/// Returns a `result<void>`'s failure from the enclosing function.
#define CORE_CHECK(expr)                                                       \
  do {                                                                         \
    auto core_check_result = (expr);                                           \
    if (!core_check_result.has_value()) {                                      \
      return std::unexpected(std::move(core_check_result.error()));            \
    }                                                                          \
  } while (false)
