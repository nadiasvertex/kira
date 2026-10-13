#pragma once

#include "check.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "src/comptime/eval.h"
#include "src/intrinsics.h"
#include "src/parser/ast_clone.h"
#include "src/parser/text_escape.h"
#include "src/semantic/binding_walk.h"
#include "src/semantic/infer/blame.h"
#include "src/semantic/infer/infer_ctxt.h"
#include "src/semantic/infer/obligations.h"
#include "src/semantic/infer/rigid_match.h"
#include "src/semantic/infer/unify.h"
#include "src/semantic/instance_queue.h"
#include "src/semantic/instance_substitution.h"
#include "src/semantic/method_instances.h"
#include "src/semantic/module_index.h"
#include "src/semantic/reason.h"
#include "src/semantic/resolution.h"
#include "src/semantic/checker_results.h"
#include "src/semantic/program_index.h"

#include "check_helpers.h"
#include "method_resolution.h"

namespace cinder::semantic::detail {

/// What the first segment of a dotted name `a.b.c` stands for — see
/// `checker::classify_dotted_root`.
enum class dotted_root : uint8_t {
  local_value,  ///< A lexical binding (or `self`): field access on it.
  module_value, ///< A module-level `def`/`static`: field access on it.
  module,       ///< A module: the rest of the path is inside it.
};

/// How a value binding entered scope, used to decide whether it may be
/// reassigned and to word the "declared here" note on an immutability error.
enum class binding_origin : uint8_t {
  let_binding,     ///< `let` — immutable.
  var_binding,     ///< `var` — mutable.
  parameter,       ///< Function or lambda parameter.
  pattern_binding, ///< Bound by a pattern (match arm, `if let`, `for`, ...).
  mut_binding,     ///< A pattern binding written with a leading `mut`
                   ///< (`let mut x`, `mut self`) — mutable like `var`.
  synthetic,       ///< Introduced by the checker itself (e.g. a `crew`/`cancel`
                   ///< name).
};

/// What a call site has solved each of the callee's type parameters to,
/// keyed by the parameter itself: its interned `type_param` id, which is per
/// declaration (`type_table::type_param`). Two parameters that share a
/// spelling — the caller's `T` and the callee's — are two keys, so solving one
/// never answers the other.
using param_subst = std::unordered_map<type_id, type_id>;

/// One name bound in the current lexical scope stack, with enough
/// information to type future references and to diagnose illegal mutation.
struct value_binding {
  type_id type = k_unknown_type;
  binding_origin origin = binding_origin::let_binding;
  source_span
      span; ///< Where the binding was introduced, for "declared here" notes.
  /// Set when the binding is a generic lambda (`let f = (x) => x + x`): `f`
  /// has no type of its own, and each use checks a copy of this lambda —
  /// see `instantiate_generic_lambda`.
  const ast::lambda_expr *generic_lambda = nullptr;
};

/// A normalized function/method parameter, used by call-argument checking
/// regardless of whether the callee came from a `func_decl`, a trait method,
/// or a `fn(...)`-typed value.
struct fn_param_info {
  std::string name;
  type_id type = k_unknown_type;
  bool has_default = false;
  source_span span;
  /// The declaration's default-value expression, when it has one — carried
  /// through to `call_argument_mapping::defaults_by_param` so a call that
  /// omits this argument can lower the default in the caller's place.
  const ast::expr *default_value = nullptr;
};

/// One method available on a type, from an inherent/trait `impl` block
/// (`from_trait == nullptr`), a trait's default body reached through an
/// impl that didn't override it, or an `extend` block (`is_extension`).
/// Inherent/impl methods win over trait defaults, which win over
/// extensions — see `find_method`.
/// What a builtin inherent method returns, relative to its receiver.
///
/// An enum rather than a `type_id` because these are described in one static
/// table (`k_builtin_methods`) built before any `type_table` exists, and
/// several of them depend on the receiver's element type — `list[T]::pop`
/// returns `option[T]`, not one fixed type.
enum class builtin_result_shape : uint8_t {
  usize_result,                  ///< `usize`.
  bool_result,                   ///< `bool`.
  unit_result,                   ///< `unit`.
  element,                       ///< The receiver's element type, `T`.
  option_of_element,             ///< `option[T]`.
  byte_slice,                    ///< `slice[byte]`.
  cell_of_element,               ///< `cell[T]`.
  option_of_mut_cell_of_element, ///< `option[cell_mut[T]]`.
  ptr_to_element,                ///< `*T` -- `as_ptr`.
  mut_ptr_to_element,            ///< `*mut T` -- `as_mut_ptr`.
};

/// One builtin inherent method: the receiver type constructor it belongs to,
/// its name, and what it evaluates to.
struct builtin_method_signature {
  std::string_view owner;
  std::string_view name;
  builtin_result_shape result;
};

/// Every inherent method the compiler answers for a builtin receiver.
///
/// This is deliberately one table rather than a chain of `if` comparisons,
/// because two separate questions have to be answered from it and they must
/// never disagree: *what does `xs.pop()` evaluate to* (`builtin_method_result`)
/// and *what names does a `list` actually have* (`builtin_method_names`, which
/// feeds the "did you mean" suggestion on an unknown method). When those were
/// allowed to be two lists, a name added to one but not the other produced a
/// method that worked but could never be suggested, or a suggestion for a
/// method that did not exist.
///
/// Only methods with real lowering belong here, and an entry *shadows* any
/// `extend` method of the same name: this table is consulted first, and the
/// `extend` lookup runs only when it answers nothing. `str`'s `contains`,
/// `starts_with`, `split`, ... are genuine `extend str` methods in
/// `std.string`, and are found by `method_resolver` precisely
/// *because* they are absent here — adding them would shadow that module.
///
/// The direction of travel is toward nothing: `option`/`result`'s methods
/// moved out to `std.option`/`std.result` as ordinary Cinder, which is what
/// removing their entries here made possible. Prefer writing a method in the
/// stdlib over adding a row plus a matching interception in
/// `hir::lower_call` (spec/collections-algorithms-design.md).
inline constexpr auto k_builtin_methods =
    std::to_array<builtin_method_signature>({
        {.owner = "slice",
         .name = "len",
         .result = builtin_result_shape::usize_result},
        {.owner = "str",
         .name = "len",
         .result = builtin_result_shape::usize_result},
        {.owner = "str",
         .name = "as_bytes",
         .result = builtin_result_shape::byte_slice},
        {.owner = "generator",
         .name = "next",
         .result = builtin_result_shape::option_of_element},
        {.owner = "cell",
         .name = "get",
         .result = builtin_result_shape::element},
        {.owner = "cell_mut",
         .name = "get",
         .result = builtin_result_shape::element},
        {.owner = "cell_mut",
         .name = "set",
         .result = builtin_result_shape::unit_result},
        // `as_ptr`/`as_mut_ptr` (spec/specification/03-advanced/
        // 38-machine-layer.md): the address of element 0 of a container's
        // data block. This is the only way to obtain a raw pointer to
        // memory the language already owns -- `rt_alloc` being the only way
        // to obtain one to memory it does not. Both lower to the same
        // `hir_container_data` node, which reads the data pointer straight
        // out of the receiver's header; the two differ only in the
        // mutability of the resulting pointer type, which is what the
        // borrow/move checkers see.
        {.owner = "slice",
         .name = "as_ptr",
         .result = builtin_result_shape::ptr_to_element},
        {.owner = "slice",
         .name = "as_mut_ptr",
         .result = builtin_result_shape::mut_ptr_to_element},
        {.owner = "array",
         .name = "len",
         .result = builtin_result_shape::usize_result},
        {.owner = "array",
         .name = "as_ptr",
         .result = builtin_result_shape::ptr_to_element},
        {.owner = "array",
         .name = "as_mut_ptr",
         .result = builtin_result_shape::mut_ptr_to_element},
        {.owner = "str",
         .name = "as_ptr",
         .result = builtin_result_shape::ptr_to_element},
        // `uninit[T, N]` is a raw buffer, so `as_mut_ptr` is how anything is
        // ever written into it. `len` answers `N`, which is a compile-time
        // constant -- the buffer's *capacity*, not a count of initialized
        // slots, which the type by construction does not track.
        {.owner = "uninit",
         .name = "len",
         .result = builtin_result_shape::usize_result},
        {.owner = "uninit",
         .name = "as_ptr",
         .result = builtin_result_shape::ptr_to_element},
        {.owner = "uninit",
         .name = "as_mut_ptr",
         .result = builtin_result_shape::mut_ptr_to_element},
    });

/// Where a member lookup that may go through `deref` reads its receiver:
/// a `field_expr`'s `object` (field access and method calls alike), or
/// the prefix of a value-rooted dotted path ending before `segment`.
struct deref_site {
  const ast::expr *receiver = nullptr;
  const ast::module_path_expr *path = nullptr;
  size_t segment = 0;

  [[nodiscard]] auto active() const -> bool {
    return receiver != nullptr || path != nullptr;
  }
};

// ==========================================================================
//  checker — one instance per session run.
// ==========================================================================

/// Runs name resolution and type checking over a whole session. One
/// instance is used for the entire `check_program` call: `session_.types_` and
/// the caches persist across files so cross-file lookups and impl coherence
/// work, while per-file/per-function state is saved and restored around
/// each declaration.
class checker {
public:
  // ==========================================================================
  //  check.cpp
  // ==========================================================================

  checker(program_index &index, diagnostic_bag &diag,
          std::vector<bool> &file_has_errors);

  auto run(const std::vector<parsed_module> &inputs) -> void;

  auto take_synthesized_fragments() -> ast::ptr_vec<ast::node>;

  auto set_module_graph(const module_session_index &session_index,
                        const semantic_resolution_index &semantic_index)
      -> void;

  auto take_checked_types() -> checked_types;

  auto resolve_fmt_runtime_types() -> fmt_runtime_types;

  auto usize_type() -> type_id;

private:
  // Not `const`: `resolve_item_splices` (run before `build_method_table`/
  // `validate_impl_coherence`) injects splice-synthesized impls directly
  // into `index_.modules[...].impls`, so both of those functions — which
  // only ever iterate `index_.modules` — pick the synthesized impls up for
  // free, with no separate bookkeeping of their own.
  program_index &index_;

  diagnostic_bag &diag_;

  std::vector<bool> &file_has_errors_;

  /// Session-wide state: the type table, the node side tables, and the
  /// caches that persist across files and functions.
  struct session_state {

    /// Every diagnostic already emitted, keyed by level, file, message, and the
    /// spans it points at — see `emit_diag`, which is why this exists.
    std::unordered_set<std::string> emitted_diagnostics_;

    type_table types_;

    /// Resolved type of every expression node visited by `infer_expr`
    /// (recorded there — see its wrapper — plus every pattern node visited by
    /// `check_pattern`). Several direct-`resolve_ident`-shaped bypasses also
    /// record here manually, since each resolves an ident's type from a
    /// declaration or binding directly rather than through `infer_expr`:
    /// `check_assignment`'s two branches (an assignment target resolved from
    /// an existing scope binding, and one resolved because no such binding
    /// exists), and `infer_call`'s three branches for a plain-identifier
    /// callee resolved as a real function reference (a scope binding, a
    /// same-module function, or an imported function — the remaining
    /// callee-name branches there are prelude magic forms like `println`/
    /// `panic` with no real declared type to record). Handed to the caller
    /// via `take_checked_types`.
    std::unordered_map<const ast::node *, type_id> node_types_;

    /// Written alongside `session_.node_types_`, read only by the snapshot
    /// renderer — see `checked_types::node_files`.
    std::unordered_map<const ast::node *, file_id_type> node_files_;

    /// Resolved type of every non-rest `ast::field_pattern` visited by
    /// `check_pattern`'s `struct_pattern` case — needed for shorthand fields
    /// (`{x}`), which have no sub-pattern node of their own to key
    /// `session_.node_types_` against. Handed to the caller via
    /// `take_checked_types`.
    std::unordered_map<const ast::field_pattern *, type_id>
        struct_pattern_field_types_;

    /// Resolved type of every non-`error` `ast::struct_field_init` shorthand
    /// field (`{x}`, `field.value == nullptr`) visited by
    /// `check_struct_literal` — these read an in-scope value directly rather
    /// than through `infer_expr`, so (like
    /// `session_.struct_pattern_field_types_`) they need their own map. Handed
    /// to the caller via `take_checked_types`.
    std::unordered_map<const ast::struct_field_init *, type_id>
        struct_literal_field_types_;

    /// See `checked_types::struct_literal_field_expected`.
    std::unordered_map<const ast::struct_field_init *, type_id>
        struct_literal_field_expected_;

    /// Per-call-site argument-to-parameter mapping, recorded in
    /// `check_call_args_against` — the only place a call is matched against
    /// a real declared parameter list (see `call_argument_mapping`'s doc
    /// comment in types.h for why a bare `fn(...)`-typed callee never gets
    /// an entry here). Handed to the caller via `take_checked_types`.
    std::unordered_map<const ast::call_expr *, call_argument_mapping>
        call_argument_mappings_;

    /// The expected type each call site was inferred under, recorded on the way
    /// into `infer_call` — the *fourth* and last source of a generic solution
    /// (`spec/generic-inference-design.md` §2). Keyed by call rather than held
    /// in a single member because argument checking runs first and infers
    /// nested calls of its own, which would clobber one.
    ///
    /// Only ever a fallback: `solve_generic_params` consults it after explicit
    /// arguments and after unification against the arguments, and
    /// `match_params` declines to overwrite a name already bound. So a hint can
    /// turn an unsolvable call into a solved one and can never re-solve a call
    /// the arguments already answered — which is what keeps `k_unknown_type`'s
    /// unify-with-everything rule from letting an unknown context quietly
    /// retype a well-understood call.
    std::unordered_map<const ast::call_expr *, type_id> call_expected_types_;

    /// Per call site, the type parameters `solve_from_expected_type` answered
    /// rather than the arguments. Read only to build the second note of §5.4:
    /// once a parameter can be solved from context, an error inside the
    /// instance has to say *which* context, or the binding appears to come
    /// from nowhere the user wrote.
    std::unordered_map<const ast::node *, std::vector<std::string>>
        expected_solved_params_;

    std::vector<instantiation_frame> instantiation_sites_;

    /// Every module-qualified or type-qualified call resolved by
    /// `infer_qualified_call`. Handed to the caller via `take_checked_types`.
    std::unordered_map<const ast::call_expr *, resolved_callee>
        resolved_callees_;

    /// Every value-position reference to a module-level function, recorded by
    /// `record_fn_value_reference`. Handed to the caller via
    /// `take_checked_types` as `checked_types::resolved_fn_values`.
    std::unordered_map<const ast::node *, resolved_callee> resolved_fn_values_;

    /// Every arithmetic operator resolved against a user operand's overload
    /// trait method, recorded by `require_operand_trait`. Handed to the
    /// caller via `take_checked_types`.
    std::unordered_map<const ast::binary_expr *, resolved_callee>
        operator_dispatches_;

    /// For every `<`/`<=`/`>`/`>=` entry `wire_ord_dispatch` adds to
    /// `session_.operator_dispatches_`, the `cmp` method's resolved return type
    /// (`ordering`) — `hir::lower_binary` needs it to type the intermediate
    /// `cmp(...)` call it synthesizes, and has no type-resolution machinery of
    /// its own to derive it from `resolved_callee::decl->return_type`.
    std::unordered_map<const ast::binary_expr *, type_id>
        ord_dispatch_result_types_;

    /// Every `v[i]` read resolved against a user type's `index` impl, and
    /// every `v[i] = x` resolved against its `index_set` impl — recorded by
    /// `require_index_trait`/`require_index_set_trait`. Two maps because the
    /// same `index_expr` node means different things in the two positions.
    std::unordered_map<const ast::index_expr *, resolved_callee>
        index_dispatches_;

    std::unordered_map<const ast::index_expr *, resolved_callee>
        index_set_dispatches_;

    /// Every `&mut v[i]` resolved against a user type's `index_mut` impl —
    /// recorded by `require_index_mut_trait`, consulted by
    /// `hir::lower_unary`.
    std::unordered_map<const ast::index_expr *, resolved_callee>
        index_mut_dispatches_;

    /// Every `&v[i]` resolved against a user type's `index_ref` impl —
    /// recorded by `require_index_ref_trait`, consulted by
    /// `hir::lower_unary`.
    std::unordered_map<const ast::index_expr *, resolved_callee>
        index_ref_dispatches_;

    /// Sequence literals that construct a user collection through its
    /// `from_array` impl, recorded by `try_wire_from_array`.
    std::unordered_map<const ast::array_expr *, array_literal_conversion>
        array_literal_conversions_;

    /// Per-(target type, trait name) associated-type bindings captured while
    /// checking that trait's impl (`check_impl_decl`) — the only place
    /// `function_.self_assoc_types_` is ever populated. A method resolved by
    /// `find_method` from *outside* its own impl (an operator overload's
    /// dispatch, say) has no `function_.self_assoc_types_` context of its own
    /// by the time it's looked up, since that map is scoped to the impl body's
    /// own checking and restored afterward — so a return type mentioning
    /// `self.output` would otherwise resolve to `unknown`. Consulted by
    /// `resolve_operator_return_type`, which temporarily reinstates the
    /// right impl's bindings before resolving such a method's return type.
    std::unordered_map<type_id, std::unordered_map<std::string, type_scope>>
        impl_assoc_types_;

    /// Every interpolation segment's resolved rendering dispatch — see
    /// `interp_dispatch`'s doc comment in types.h. Populated by
    /// `check_interpolated_string`. Handed to the caller via
    /// `take_checked_types`.
    std::unordered_map<const ast::expr *, interp_dispatch> interp_dispatches_;

    /// Every `T.name()` reflection call resolved against a type parameter of
    /// the instance currently being checked — see `type_param_reflection` in
    /// types.h. Populated by `infer_call`'s `lookup_type_param` branch, handed
    /// to the caller via `take_checked_types`.
    std::unordered_map<const ast::call_expr *, type_param_reflection>
        type_param_reflections_;

    /// Every free, module-level `static def` registered for compile-time
    /// calling — see `checked_types::comptime_only_functions`'s doc comment,
    /// the reason this exists (a plain `is_static` check can't tell such a
    /// function apart from an ordinary static *method*). Populated
    /// alongside `comptime_eval_.register_pending_function` in `register_
    /// comptime_globals`, handed to the caller via `take_checked_types`.
    std::unordered_set<const ast::func_decl *> comptime_only_functions_;

    /// Every `for` loop over a user `std.iter.iterator[T]` — see
    /// `iterator_loop_dispatch` in types.h. Populated by `check_body_node`'s
    /// `for_stmt` case, handed to the caller via `take_checked_types`.
    std::unordered_map<const ast::for_stmt *, iterator_loop_dispatch>
        for_iterator_dispatches_;

    /// Comprehension clauses' iterator dispatches — see
    /// `checked_types::comprehension_iterator_dispatches`.
    std::unordered_map<const ast::node *, iterator_loop_dispatch>
        comprehension_iterator_dispatches_;

    /// Every struct/sum type found droppable — see `drop_plan` in types.h.
    /// Populated once by `resolve_drop_plans`, after the main per-function
    /// walk. Handed to the caller via `take_checked_types`.
    std::unordered_map<type_id, drop_plan> drop_plans_;

    /// `fn() -> unit`, the type of a closure's drop glue (`drop_plan`).
    type_id closure_glue_type_ = k_unknown_type;

    /// Every `for ... => yield` comprehension's resolved constructor/push
    /// calls — see `comprehension_dispatch` in types.h. Populated by
    /// `infer_for_expr`.
    std::unordered_map<const ast::for_expr *, comprehension_dispatch>
        comprehension_dispatches_;

    /// `[v; n]` literals with a runtime repeat count — see
    /// `checked_types::runtime_fill_dispatches`.
    std::unordered_map<const ast::array_expr *, comprehension_dispatch>
        runtime_fill_dispatches_;

    /// Every `?` whose operand's error type needed a `from`-conversion to the
    /// enclosing function's declared error type, recorded by `infer_try`.
    /// Handed to the caller via `take_checked_types`.
    std::unordered_map<const ast::try_expr *, resolved_callee> try_conversions_;

    /// Paired with `session_.try_conversions_`: the enclosing function's full
    /// declared return type for each entry, needed by `hir::lower_try` to type
    /// the reconstructed `@err(...)` value. Handed to the caller via
    /// `take_checked_types`.
    std::unordered_map<const ast::try_expr *, type_id> try_conversion_types_;

    /// Owns every trait-default method clone `build_method_table` synthesizes
    /// (see `synthesized_method` in types.h) — lifetime must outlive
    /// `checked_types`, so both are moved out together in
    /// `take_checked_types`.
    ast::ptr_vec<ast::func_decl> synthesized_decls_;

    std::vector<synthesized_method> synthesized_trait_defaults_;

    /// Every const-generic instance `instantiate_generic_function` produced
    /// (see `const_generic_instance` in types.h). The clones themselves are
    /// owned by `session_.synthesized_decls_`, the same way trait-default
    /// clones are.
    std::vector<const_generic_instance> const_generic_instances_;

    /// The constants the const-generic instance currently being checked was
    /// instantiated with (`n` -> the `const_value` type slot, and `n` -> the
    /// plain integer). Empty while checking anything else, including the
    /// template these instances come from. `push_type_params` reads the first
    /// (so `index[n]` resolves to `index[3]` inside the instance, which is what
    /// gives a `try_from` real parameters to compare against), and
    /// `resolve_ident` reads the second (so a *value* use of `n` lowers to the
    /// literal `3` — there is no runtime parameter to load it from).
    type_scope const_param_slots_;

    /// While checking a monomorphized copy of a generic *method* (higher-
    /// kinded trait impls — `instantiate_hk_method`): each type parameter
    /// name bound to the concrete type the call solved for it. The type
    /// analog of `session_.const_param_slots_`, consulted by
    /// `push_type_params`.
    type_scope type_param_slots_;

    /// The block-level type parameters of the `impl`/`extend` a method
    /// currently being checked belongs to (`impl[n: usize] ... for buf[n]`'s
    /// `n`), or `nullptr` outside of one. These never appear in the method's
    /// own `decl.type_params`, so `bind_value_params(decl.type_params)` alone
    /// never binds a value-kind one (`n`) as an ordinary value — only
    /// `push_type_params` sees it, and only as a *type*. `check_function`
    /// additionally runs `bind_value_params` over this list so a value-kind
    /// block parameter reads as a value in the body (`n as int32`) exactly
    /// like one of the method's own, in both the unresolved template check
    /// (`check_impl_decl`/`check_extend_decl`, abstract) and a concrete
    /// instance's check (`find_or_check_generic_instance`, resolved to
    /// `usize` either way — the concrete value itself flows separately
    /// through `session_.const_param_values_`/`record_const_param_reference`).
    const std::vector<ast::type_param> *enclosing_block_type_params_ = nullptr;

    /// One monomorphized instance per (declaration, solution) — so a call made
    /// a thousand times with `n == 3` compiles one `get$3` and not a thousand.
    /// Shared by every instantiation path (free function, method, higher-kinded
    /// method); see `find_or_check_generic_instance`, which also explains why
    /// the entry is inserted *before* the clone's body is checked. Values point
    /// into `session_.synthesized_decls_`.
    std::unordered_map<std::string, const ast::func_decl *> hk_instance_cache_;

    std::unordered_map<std::string, int64_t> const_param_values_;

    /// Nesting depth of `instantiate_generic_function`, bounding a template
    /// that instantiates itself at an ever-changing constant (`f[n]` calling
    /// `f[n + 1]`) — which would otherwise never stop.
    size_t instantiation_depth_ = 0;

    /// Owns every `named_type` synthesized by `reinterpret_as_named_type`
    /// (see its doc comment) for the whole session — stable-address (a
    /// `vector<unique_ptr<T>>` only moves the pointer, never the pointee),
    /// same pattern as `session_.synthesized_decls_` above.
    ast::ptr_vec<ast::type_expr> synthesized_types_;

    /// Splice-node -> resolved-fragment mapping — see `checked_types::
    /// spliced_fragments`'s doc comment in types.h. Populated by the
    /// `splice_expr` case in `infer_expr_impl` and the `splice_stmt` case in
    /// `check_body_node`. Handed to the caller via `take_checked_types`.
    std::unordered_map<const ast::node *, const ast::node *> spliced_fragments_;

    /// Every item-level splice resolved to an injected `impl` block — see
    /// `synthesized_item_splice` in types.h. Populated by
    /// `resolve_item_splices`, moved out via `take_checked_types` for
    /// `hir::lower_module`.
    std::vector<synthesized_item_splice> synthesized_item_splices_;

    /// Which `(instantiation, trait)` pairs `ensure_derived_instance_impls`
    /// has already derived, as `"<type_id>#<trait>"`. Keyed by the *applied*
    /// type id rather than the declaration, so `box[int32]` and `box[str]`
    /// each derive their own impl instead of the first one seen serving both.
    std::unordered_set<std::string> derived_instances_;

    /// Top-level (item-position) `splice_stmt` node -> the `impl_decl` it
    /// resolved to, so `check_file`'s item loop knows to `check_item` the
    /// resolved impl instead of routing the splice node itself through
    /// `check_body_node` (which only understands statement/expression
    /// fragments) — see `resolve_item_splices`.
    std::unordered_map<const ast::node *, const ast::impl_decl *>
        item_splice_impls_;

    /// `type_decl` node -> every `impl_decl` `resolve_deriving_traits`
    /// spliced in for it (one per real-derivation-backed trait named in
    /// `deriving`, e.g. `show` *and* `eq`), so `check_file`'s item loop (and
    /// its `sub_module_decl` counterpart) know to *also* `check_item` each
    /// derived impl, in addition to (not instead of, unlike
    /// `session_.item_splice_impls_`) checking the `type_decl` itself normally
    /// — there is no literal splice AST node for `deriving` sugar to swap in
    /// for.
    std::unordered_map<const ast::node *, std::vector<const ast::impl_decl *>>
        derived_trait_impls_;

    /// Derived `impl_decl` -> the span of the `type ... deriving ...` that
    /// caused it. A derived impl's own `span` points into the quote it was
    /// built from in `src/std/deriving.cn`, so rendering a diagnostic at it
    /// against the *user's* file id lands on an arbitrary offset in the user's
    /// source — unreadable, and exactly the kind of message this compiler's
    /// stated philosophy forbids. `impl_report_span` consults this so an
    /// impl-level diagnostic about a derived impl points at the `deriving`
    /// clause the user actually wrote. Reachable in practice through `ord`,
    /// the first derived trait with a `requires` bound (`ord requires eq`):
    /// `type point = {...} deriving ord` alone is a real, reportable mistake.
    std::unordered_map<const ast::impl_decl *, source_span>
        derived_impl_origin_spans_;

    /// Owns every literal node synthesized by `materialize_const_literal` —
    /// see `checked_types::synthesized_const_literals`'s doc comment.
    ast::ptr_vec<ast::literal_expr> synthesized_const_literals_;

    /// See `checked_types::static_struct_values`'s doc comment.
    std::unordered_map<const ast::node *, const ast::expr *>
        static_struct_values_;

    ast::ptr_vec<ast::expr> synthesized_static_structs_;

    /// See `checked_types::unreadable_static_refs`'s doc comment.
    std::unordered_map<const ast::node *, std::string> unreadable_static_refs_;

    /// See `checked_types::static_const_values`'s doc comment. Populated by
    /// `resolve_ident`.
    std::unordered_map<const ast::node *, const ast::literal_expr *>
        static_const_values_;

    /// See `lowering_results::static_global_defs`'s doc comment. Populated by
    /// `reify_static_global`.
    std::unordered_map<const ast::static_decl *,
                       lowering_results::static_global_def>
        static_global_defs_;

    /// See `checked_types::static_if_taken_branch`'s doc comment. Populated by
    /// `check_static_decl`'s `conditional_compilation` case.
    std::unordered_map<const ast::static_decl *, bool> static_if_taken_branch_;

    /// See `checked_types::static_global_refs`'s doc comment. Populated by
    /// `record_static_const_reference`.
    std::unordered_map<const ast::node *, std::string> static_global_refs_;

    /// See `checked_types::value_path_types`. Populated by
    /// `infer_value_rooted_path`.
    std::unordered_map<const ast::module_path_expr *, std::vector<type_id>>
        value_path_types_;

    /// See `checked_types::deref_adjustments` and `path_deref_adjustments`.
    /// Populated by `record_deref_steps`.
    std::unordered_map<const ast::expr *, std::vector<deref_step>>
        deref_adjustments_;

    std::unordered_map<const ast::module_path_expr *,
                       std::vector<std::vector<deref_step>>>
        path_deref_adjustments_;

    /// Calls built only to resolve an implicit `deref()` through the ordinary
    /// method-call machinery (`resolve_deref_step`). Nothing outside the
    /// checker refers to them; they are kept alive because instance checking
    /// may still name one as the site that asked for an instance.
    ast::ptr_vec<ast::expr> deref_probe_calls_;

    /// The module graph `validate_module_reference` resolves a module-rooted
    /// dotted path against. Null when `check_program` was called without
    /// them, in which case no module-rooted path is validated here.
    const module_session_index *session_index_ = nullptr;

    const semantic_resolution_index *semantic_index_ = nullptr;

    /// Module-rooted paths already validated, so a generic body checked once
    /// abstractly and once per instantiation reports a bad path once.
    std::unordered_set<const ast::module_path_expr *> validated_module_paths_;

    /// Each file's module-import aliases, for `validate_module_path`.
    std::unordered_map<file_id_type, module_alias_map> file_module_aliases_;

    /// While a functor instantiation's body is checked, the module that
    /// declared the functor: `file_.module_name_` is then the instance's
    /// synthetic name, which the module graph has never heard of, but the body
    /// was written in — and sees what is visible from — the declaring module.
    /// Empty outside a functor body.
    std::string functor_owner_module_;

    /// See `checked_types::folded_comptime_calls`'s doc comment. Populated by
    /// `try_fold_comptime_only_call`, from `instantiate_generic_function`.
    std::unordered_map<const ast::call_expr *, const ast::literal_expr *>
        folded_comptime_calls_;

    /// The type parameters each call to a comptime-only generic `static def`
    /// solved to, recorded by `instantiate_generic_function` (in the caller's
    /// own type parameters when the caller is generic). Compile-time code runs
    /// the template body, so the evaluator binds these (`comptime_call_type_
    /// args`) for `T.name()` and for a method on a `T` value to read.
    std::unordered_map<const ast::call_expr *, type_scope>
        comptime_call_type_slots_;

    /// See `checked_types::layout_queries`. Populated by
    /// `infer_layout_query_call`.
    std::unordered_map<const ast::call_expr *, layout_query> layout_queries_;

    /// See `checked_types::ptr_casts`. Populated by `infer_ptr_cast_call`.
    std::unordered_map<const ast::call_expr *, type_id> ptr_casts_;

    std::unordered_set<const ast::call_expr *> slice_from_raw_parts_calls_;

    /// Calls with an argument already reported as the wrong type. A value
    /// parameter that argument would have fixed is left unsolved by it, and
    /// saying so again ("cannot tell what `n` is") would only restate the
    /// mismatch as a second, vaguer error.
    std::unordered_set<const ast::call_expr *> mismatched_argument_calls_;

    /// See `checked_types::stack_buffers`. Populated by `infer_uninit_call`.
    std::unordered_map<const ast::call_expr *, stack_buffer_request>
        stack_buffers_;

    /// See `checked_types::static_global_owners`'s doc comment. Populated by
    /// `reify_static_global`.
    std::unordered_map<std::string, std::string> static_global_owners_;

    /// See `checked_types::proven_in_bounds`'s doc comment. Populated by
    /// `check_index_in_bounds`.
    std::unordered_set<const ast::index_expr *> proven_in_bounds_;

    /// Owns the desugared body of every `try_from` call — see
    /// `build_try_from_fragment`. Handed to `checked_types::
    /// synthesized_fragments` at the end of checking, since `spliced_fragments`
    /// points into these and `hir::lower` runs after the checker is gone.
    ast::ptr_vec<ast::node> proof_fragments_;

    /// See `checked_types::elided_contracts`'s doc comment. Populated by
    /// `check_call_preconditions`.
    std::unordered_set<const ast::contract_clause *> elided_contracts_;

    /// Numbers the compiler-introduced binding each `try_from` desugaring
    /// needs, so two proofs in one function never collide.
    size_t proof_temporaries_ = 0;
  } session_;

  /// State for the file and module currently being checked.
  struct file_state {
    const module_members *module_ = nullptr;

    std::string module_name_;

    file_id_type file_id_ = 0;

    bool file_has_external_wildcard_ = false;

    bool file_no_prelude_ = false;
  } file_;

  /// One module parameter of a functor bound to its argument module during
  /// instantiation (`DB` → `main.postgres`).
  struct functor_arg_binding {
    std::string param_name;
    std::string arg_module_name;
  };

  /// A materialized functor instantiation whose cloned body has been
  /// *registered* (into `index_`, so its `impl`/`extend` members join the
  /// method table and coherence check) but not yet *checked*. Body checking is
  /// deferred until after `build_method_table`/`validate_impl_coherence` so a
  /// method call inside the body resolves against a complete method table.
  struct pending_functor_body {
    std::string synth_name;
    std::string owner_module; ///< Module the functor was declared in.
    file_id_type functor_file = 0;
    std::vector<functor_arg_binding> module_bindings;
    std::vector<const ast::type_decl *> types;
    std::vector<const ast::static_decl *> statics;
    std::vector<const ast::func_decl *> funcs;
    std::vector<const ast::impl_decl *> impls;
    std::vector<const ast::extend_decl *> extends;
    std::vector<const ast::trait_decl *> traits;
  };

  /// State for functor instantiation (modules as compile-time values).
  struct functor_state {
    /// AST nodes the checker synthesizes for functor instantiations (cloned
    /// functor-body items), owned here so they outlive the check.
    std::vector<ast::ptr<ast::node>> synthetic_nodes_;

    /// Memoized instantiations: canonical instantiation key → the sanitized
    /// module name its members were registered under. Guarantees one module
    /// identity per (functor, arguments) tuple, session-wide (applicative
    /// functor semantics, `spec/module-values-design.md` §4).
    std::unordered_map<std::string, std::string> functor_instances_;

    /// One entry per `def` cloned into a materialized functor instantiation,
    /// each tagged with its synthetic module name — moved into
    /// `checked_types::functor_instances` for `hir::lower_functor_modules`.
    std::vector<functor_instance> functor_instance_decls_;

    std::vector<pending_functor_body> pending_functor_bodies_;
  } functor_;

  /// One entry per lambda currently being checked, innermost last.
  /// `function_.scopes_` is a flat stack with no notion of a function boundary,
  /// so this is what records where each lambda began and, when it declared an
  /// explicit capture list, which outer names it is allowed to reach across
  /// that boundary. A lambda with no list pushes an entry with `allowed` unset,
  /// so nested lambdas still compose correctly.
  struct capture_barrier {
    size_t scope_floor = 0;
    const ast::lambda_expr *lambda = nullptr;
    std::optional<std::unordered_map<std::string, ast::capture_mode>> allowed;
    /// Listed names the body actually read, filled in as it is checked.
    std::unordered_set<std::string> used;
    /// Listed names already diagnosed, so the "captured but never used"
    /// warning doesn't pile a second complaint onto a broken entry.
    std::unordered_set<std::string> invalid;
  };

  /// State for the function, lambda, or block currently being checked.
  /// `check_function_impl` saves this whole value on entry and restores it
  /// on exit.
  struct function_state {
    std::vector<std::unordered_map<std::string, value_binding>> scopes_;

    std::vector<capture_barrier> capture_barriers_;

    std::vector<type_scope> type_params_;

    type_id self_type_ = k_unknown_type;

    /// Associated-type names in scope for `self.<name>` references, valid
    /// while checking a trait's or impl's own members (see `check_trait_decl`
    /// and `check_impl_decl`).
    type_scope self_assoc_types_;

    type_id return_type_ = k_unknown_type;

    bool return_annotated_ = false;

    /// While checking a function declared without a return type: the join of
    /// every `return <value>` and the tail seen so far.
    bool inferring_return_ = false;

    bool inferred_return_seen_ = false;

    type_id inferred_return_ = k_unknown_type;

    bool in_contract_ = false;

    /// Whether the contract condition currently being checked is a `post` —
    /// the only place `return` (the value the function returns, not the
    /// statement) may be named. See `resolve_ident`.
    bool in_postcondition_ = false;

    /// Whether the function body currently being checked belongs to a
    /// `generator def`; when true, `yield` is legal and its operand must
    /// match `function_.generator_item_type_` (see
    /// `infer_yield`/`check_function`).
    bool in_generator_ = false;

    /// Whether the function body currently being checked is a comptime-only
    /// `static def` (template or instantiated clone alike) — see
    /// `session_.comptime_only_functions_`'s doc comment and `check_function`,
    /// which sets/restores this. Consulted by `try_fold_comptime_only_call` to
    /// avoid folding a nested call from *inside* such a body: that call is
    /// already handled by direct evaluator execution whenever this function
    /// itself is invoked at compile time, and its own body is never lowered.
    bool in_comptime_only_function_ = false;

    /// Whether the function body currently being checked carries the `machine`
    /// modifier. Raw-pointer operations (`as_ptr`/`as_mut_ptr`, `ptr_cast`,
    /// indexing or dereferencing a `*T`/`*mut T`, and constructing a
    /// `uninit[T, N]`) are permitted only where this is set — that gating is
    /// the whole of what `machine` *means*
    /// (spec/specification/03-advanced/38-machine-layer.md), and until it
    /// existed the modifier was syntax with nothing behind it.
    bool in_machine_function_ = false;

    /// Nesting depth of "couldn't decide which branch, so check both" `static
    /// if` fallbacks (`resolve_static_if_branch` returned `nullopt` — an
    /// unresolved condition, most commonly one that depends on a still-abstract
    /// generic type parameter in the un-instantiated template pass every
    /// top-level function gets in addition to its per-call-site instantiation
    /// checks). A `static assert` reached only through one of those speculative
    /// branches has no more claim to being genuinely reached than its sibling
    /// branch does — the fallback exists to still catch real problems (typos,
    /// undefined names) in *both* candidates, not to certify either one as
    /// live. Diagnosing it as an unconditional failure would be a false
    /// positive for the extremely common "one `static if`/`static assert`
    /// chain per possible type, with a trailing `static assert false` standing
    /// in for the case no branch matches" pattern (`std.limits.max`): that
    /// trailing assert is only ever actually reached when every real
    /// instantiation's own concretely-bound condition already narrowed past
    /// it, which the *unresolved* pass can never know. See
    /// `check_static_decl`'s `assertion` case, the only consumer.
    int in_speculative_static_branch_ = 0;

    /// Whether a preceding sibling statement in the body list `check_body_
    /// nodes` is currently walking is already known (for the concrete
    /// instantiation being checked right now) to have returned — see
    /// `check_body_nodes`'s own comment for why every statement is still
    /// visited regardless, and `stmt_definitely_returns` for what counts.
    /// Saved/restored per `check_body_nodes` call so it never leaks into an
    /// unrelated sibling block (a nested `static if` branch, a lambda body,
    /// ...). Consulted by `check_static_decl`'s `assertion` case for the same
    /// reason `function_.in_speculative_static_branch_` is: a `static assert`
    /// reached only past a provably-dead earlier `return` has no more claim to
    /// being live than one reached through an unresolved branch does.
    bool in_known_unreachable_code_ = false;

    /// Enclosing `while`/`for` loops in the body currently being checked, used
    /// to reject `break`/`continue` that have no loop to act on. Saved and
    /// reset across every function and lambda body: a loop in the *enclosing*
    /// function is not a loop this body can break out of.
    unsigned loop_depth_ = 0;

    type_id generator_item_type_ = k_unknown_type;

    /// Whether the function body currently being checked declares a general
    /// `some Trait[Args]` existential return type (never true together with
    /// `function_.in_generator_` — a `generator def`'s `some iterator[T]` stays
    /// on its own dedicated path, see `check_function`). While true, every
    /// return-point's inferred type is joined into
    /// `function_.existential_underlying_` instead of being checked against
    /// `function_.return_type_` directly, since `function_.return_type_` here
    /// is the fresh opaque id itself, not a real shape any concrete expression
    /// could match.
    bool checking_existential_return_ = false;

    type_id existential_underlying_ = k_unknown_type;

    std::unordered_set<std::string> reported_undefined_;

    /// Everything known about the values in scope at the point currently being
    /// checked — value-parameter domains, refined parameters' predicates, array
    /// lengths, preconditions, and (inside a branch) path conditions. Rebuilt
    /// per function by `collect_function_facts`, extended and truncated
    /// lexically by `fact_scope`. This is the left-hand side of every proof the
    /// compiler attempts; see `src/semantic/reason.h`.
    fact_set facts_;

    /// Locals bound to `T.name()`, so `let n = T.name()` followed by
    /// `static if n == "int8"` narrows `T` the same way the direct form does.
    type_scope type_name_aliases_;

    /// The template currently being checked, or `nullptr` outside one.
    const ast::func_decl *current_template_ = nullptr;
  } function_;

  /// What an inline `static for`'s yield reads, found while checking it
  /// (keyed by the declaration as written, never an instance's clone).
  struct static_for_reads {
    /// The yield reads a binding from outside the `static for` (a parameter,
    /// a local), so it must run when the program runs.
    bool runtime = false;
    /// Every expression in the yield rooted at one of the loop's own
    /// bindings — the name itself, `f.name`, `p.x.y` — outermost first.
    std::vector<const ast::expr *> loop_reads;
  };

  /// Compile-time evaluator backing `static let`/`static assert`/
  /// `static if`. One instance for the whole session (parallel to
  /// `cache_.static_types_`/`session_.types_`) so `static let` bindings
  /// evaluated while checking one file stay visible by name to `static`
  /// expressions in later files — see the compile-time evaluation design plan's
  /// confluence requirement.
  comptime::evaluator comptime_eval_;

  /// Caches of compile-time results, keyed by declaration or node.
  struct cache_state {
    std::unordered_map<const ast::static_decl *, type_id> static_types_;

    /// The type inferred for `static for pat in iter => yield_expr`'s
    /// `for_yield` expression, keyed by the enclosing `static_decl`. Populated
    /// once in `check_static_decl` (the yield expression is type-checked a
    /// single time against the iterable's element type, not once per
    /// unrolled iteration); consulted by `check_body_node` so an inline
    /// `static for` used as a block's tail statement reports the type it
    /// actually produces instead of `unit`.
    std::unordered_map<const ast::static_decl *, type_id>
        static_for_yield_types_;

    /// What each inline `static for` yielded, evaluated by `check_static_decl`
    /// outside a generic template — read back by `unroll_static_for_inline`.
    /// One entry per iteration its guard keeps: the loop's bindings, by name.
    std::unordered_map<
        const ast::static_decl *,
        std::vector<std::unordered_map<std::string, comptime::value>>>
        static_for_values_;

    /// See `checked_types::static_for_elements`/`static_for_dispatches`/
    /// `static_for_iterations`.
    std::unordered_map<const ast::static_decl *, std::vector<const ast::expr *>>
        static_for_elements_;

    std::unordered_map<const ast::static_decl *, comprehension_dispatch>
        static_for_dispatches_;

    std::unordered_map<const ast::static_decl *,
                       std::vector<static_for_iteration>>
        static_for_iterations_;

    std::unordered_map<const ast::static_decl *, static_for_reads>
        static_for_reads_;

    /// The inline `static for`s whose yield is being checked, innermost last,
    /// each with the index of the scope holding its loop bindings.
    std::vector<std::pair<const ast::static_decl *, size_t>> static_for_yields_;

    /// The element type the context expects of the inline `static for` about
    /// to be checked as a block's statement (`check_body_node`).
    type_id static_for_expected_element_ = k_unknown_type;

    std::unordered_set<const ast::static_decl *> statics_in_progress_;

    /// Unannotated `static let` bindings checked ahead of file order by
    /// `check_static_binding_early`, so file order does not check them again.
    std::unordered_set<const ast::static_decl *> early_checked_static_bindings_;

    /// Each `static let`'s evaluated compile-time value, keyed by declaration
    /// so same-named statics in different modules stay distinct — see
    /// `ensure_static_binding_evaluated`. `nullopt` records an initializer
    /// that failed to evaluate, so it is not retried (and re-diagnosed) at
    /// every later reference.
    std::unordered_map<const ast::static_decl *, std::optional<comptime::value>>
        static_binding_values_;

    /// The `static` bindings whose initializer failed with a reported error.
    /// A file that refers to one is marked failing (see
    /// `ensure_static_binding_evaluated`), so lowering never sees a reference
    /// to a binding that has no value.
    std::unordered_set<const ast::static_decl *> failed_static_bindings_;

    /// `static` bindings whose initializer `ensure_static_binding_evaluated`
    /// is evaluating right now — its cycle guard.
    std::unordered_set<const ast::static_decl *> static_bindings_evaluating_;

    /// Every module-level function's declaring module and file, plus each
    /// monomorphized instance's (its template's) — see `function_owner`.
    std::unordered_map<const ast::func_decl *,
                       std::pair<const module_members *, file_id_type>>
        function_owners_;

    /// `index_.modules.size()` when `cache_.function_owners_` last scanned it.
    size_t function_owners_modules_ = 0;

    std::unordered_set<const ast::type_decl *> aliases_in_progress_;

    /// Cache of `param_types_for`'s result, so `check_function`'s own body
    /// check and `signature_params`'s call-site view agree on the same
    /// inferred types instead of independently guessing. Indices align 1:1
    /// with `decl.params`.
    std::unordered_map<const ast::func_decl *, std::vector<type_id>>
        inferred_param_types_;

    bool methods_built_ = false;

    /// Every impl, trait-default, and `extend` method, filed for
    /// `method_resolver`. Filled by `build_method_table`, and appended to
    /// by `ensure_derived_instance_impls` for a generic `deriving` type.
    method_tables method_tables_;

    /// Memoizes `resolve_existential_type`'s minted `existential_kind` id per
    /// AST node, so the many independent `resolve_type` calls that can all
    /// reach the same `some Trait[Args]` node (a function's own return-type
    /// resolution in `check_function`, plus every external caller's view of
    /// it via `signature_return_type`) agree on one id instead of each
    /// minting a fresh, structurally-identical-but-distinct existential type.
    std::unordered_map<const ast::existential_type *, type_id>
        existential_types_;
  } cache_;

  // ==========================================================================
  //  check_diagnostics.cpp
  // ==========================================================================

  auto mark_error() -> void;

  auto emit_diag(const diagnostic &diag) -> void;

  /// Every error `emit_diag` has let through, so a pass can tell whether it
  /// reported anything.
  size_t errors_emitted_ = 0;

  auto with_instantiation_notes(const diagnostic &diag) -> diagnostic;

  auto error(source_span span, const std::string &message,
             const std::string &label) -> void;

  auto error_with_help(source_span span, const std::string &message,
                       const std::string &label, const std::string &help)
      -> void;

  auto value_slot_conflict(type_id expected, type_id found)
      -> std::optional<std::string>;

  [[nodiscard]] auto settle(type_id id) -> type_id;

  auto type_params_in_scope(type_id id) -> bool;

  auto type_params_in_scope_impl(type_id id, std::unordered_set<type_id> &seen)
      -> bool;

  auto flush_for(std::span<const type_id> roots,
                 const std::function<bool()> &answered = nullptr) -> void;

  auto demand(type_id id) -> type_id;

  auto demand_until(type_id settled, const std::function<bool()> &answered)
      -> type_id;

  auto demand_shape(type_id id) -> type_id;

  auto solve_leaves(type_id expected, type_id found) -> void;

  auto agrees(type_id expected, type_id found) -> bool;

  [[nodiscard]] auto needs_explicit_deref(type_id id) -> bool;

  auto report_deref_if_missing(const ast::expr *value, type_id expected,
                               type_id found) -> bool;

  auto require_explicit_deref(const ast::expr &operand, type_id raw) -> bool;

  auto report_missing_deref(const ast::expr &value, type_id found) -> void;

  auto type_mismatch(source_span span, type_id expected, type_id found,
                     std::string_view context, const ast::expr *value = nullptr)
      -> void;

  // ==========================================================================
  //  check_scopes.cpp
  // ==========================================================================

  auto push_scope() -> void;

  auto pop_scope() -> void;

  auto bind_value(std::string_view name, type_id type, binding_origin origin,
                  source_span span) -> void;

  auto lookup_value(std::string_view name) -> const value_binding *;

  auto note_capture_use(std::string_view name, size_t depth) -> void;

  /// The result of a scope-stack walk: the binding that was found (if any),
  /// and the innermost capture list that stands between the reference and
  /// that binding (if any). A non-null `blocked_by` with a non-null
  /// `binding` is the interesting case — the name exists in an enclosing
  /// function, but this lambda declared it out of reach.
  struct scope_lookup {
    const value_binding *binding = nullptr;
    const capture_barrier *blocked_by = nullptr;
    size_t depth =
        0; ///< Index in `function_.scopes_` the binding was found at.
  };

  auto find_value_in_scopes(std::string_view name) -> scope_lookup;

  auto capture_barrier_blocking(std::string_view name)
      -> const capture_barrier *;

  auto emit_capture_not_listed(source_span span, std::string_view name,
                               const capture_barrier &barrier) -> void;

  static auto format_capture_names(
      const std::unordered_map<std::string, ast::capture_mode> &allowed)
      -> std::string;

  auto push_type_params(const std::vector<ast::type_param> &params) -> void;

  auto pop_type_params() -> void;

  auto bind_value_params(const std::vector<ast::type_param> &params) -> void;

  [[nodiscard]] auto in_abstract_type_param_scope() -> bool;

  auto lookup_type_param(std::string_view name) -> std::optional<type_id>;

  [[nodiscard]] auto
  find_session_module_of_path(const std::vector<std::string> &path) const
      -> const module_members *;

  [[nodiscard]] auto
  find_visible_module_of_path(const std::vector<std::string> &path) const
      -> const module_members *;

  [[nodiscard]] auto
  find_submodule_of_current_module(const std::vector<std::string> &path) const
      -> const module_members *;

  template <typename OwnsFn>
  [[nodiscard]] auto
  find_member_owner_of_path(const std::vector<std::string> &path,
                            const OwnsFn &owns) const -> const module_members *;

  [[nodiscard]] auto
  find_static_owner_of_path(const std::vector<std::string> &path) const
      -> const module_members *;

  [[nodiscard]] auto
  find_fn_owner_of_path(const std::vector<std::string> &path) const
      -> const module_members *;

  [[nodiscard]] auto
  session_owns_path_root(const std::vector<std::string> &path) const -> bool;

  [[nodiscard]] auto imports_for_current_file() const
      -> const std::vector<import_binding> *;

  [[nodiscard]] auto find_renamed_import(std::string_view name) const
      -> const import_binding *;

  [[nodiscard]] auto find_import(std::string_view name) const
      -> const import_binding *;

  [[nodiscard]] auto wildcard_import_sources() const
      -> std::vector<const module_members *>;

  [[nodiscard]] auto import_source_module(const import_binding &binding) const
      -> const module_members *;

  [[nodiscard]] auto imported_member_name(const import_binding &binding) const
      -> std::string;

  // ==========================================================================
  //  Type resolution
  //
  //  `quiet` suppresses diagnostics; it is used when resolving foreign
  //  declarations (callee signatures, alias expansion) whose own checking
  //  pass reports problems at the declaration site.
  // ==========================================================================

  /// Ambient state for resolving a type expression: which module's members
  /// are in scope, an optional substitution for generic parameters (used
  /// when instantiating a generic declaration), and whether to also consult
  /// the live `function_.type_params_` scope stack (only true for types
  /// resolved in the context of the function/type/impl currently being
  /// checked).
  struct resolve_ctx {
    const module_members *module = nullptr;
    const type_scope *param_bindings = nullptr;
    bool use_type_param_stack = false;
    bool quiet = false;
    /// Whether `some Trait[Args]` (`existential_type`) may resolve to a real
    /// opaque type here. Only set for a function's own return-type position
    /// (`check_function`'s `decl.return_type` resolution and
    /// `signature_return_type`'s external view of the same node) — every
    /// other position (parameters, `let`/`var` annotations, struct fields,
    /// `where` bounds) rejects it with a targeted diagnostic instead, since
    /// there is no monomorphization to give it meaning anywhere else (see
    /// the existential-type-system plan's scope notes).
    bool existential_allowed = false;
    /// When set, a *bare* generic name (`option`, a user generic struct/sum)
    /// may resolve to an unapplied constructor (`ctor_ref_kind`) of this
    /// arity — kind-directed resolution, only enabled where a higher-kinded
    /// slot is being filled: a trait argument for an `F[_]` parameter, or an
    /// `impl <hk-trait> for <ctor>` target. Everywhere else a bare generic
    /// name keeps its historical meaning (an instantiation with unknown
    /// arguments), so ordinary code is untouched.
    std::optional<size_t> expected_ctor_arity;
    /// Set alongside `existential_allowed` when resolving a `generator
    /// def`'s own return type. `some iterator[T]` there is pure syntactic
    /// sugar `check_function` substitutes for the concrete `generator[T]`
    /// (see its doc comment) — it never actually needs `iterator` to be a
    /// real, in-scope trait declaration (unlike the general existential
    /// mechanism), so `resolve_existential_type` skips trait-existence
    /// validation here. Without this, every generator fixture across the
    /// codebase (many written standalone, without `std.iter` in scope —
    /// generator semantics predate the general existential system and
    /// were never meant to depend on it) would need to import `std.iter`
    /// purely to satisfy a validation generator lowering doesn't use.
    bool is_generator_return = false;
  };

  // ==========================================================================
  //  check_type_resolution.cpp
  // ==========================================================================

  auto current_resolve_ctx() -> resolve_ctx;

  // ==========================================================================
  //  Facts about type parameters (ch. 19, Bounded Generics,
  //  rule 1)
  //
  //  What a generic body may rely on about `T` is exactly what its
  //  declaration says: `T`'s own bounds, its `where` bounds, everything those
  //  `require`, and the bounds of an enclosing generic block. An operation on
  //  a value of type `T` that no fact justifies is a mistake at the line that
  //  performs it — reported once, there, rather than once per instance from
  //  inside whatever called it.
  // ==========================================================================

  /// One fact: the parameter implements `trait[args]`. The declaration is
  /// resolved where the bound was written, not looked up again by name where
  /// it is used — two modules may each declare an `iterator`, and the one a
  /// bound means is the one its own module sees.
  struct type_fact {
    const ast::trait_decl *trait = nullptr;
    std::vector<type_id> args;
  };

  /// Declared facts, keyed by the parameter's own `type_id`.
  ///
  /// Not scoped: a parameter is keyed on the declaration that introduced it
  /// (`type_table::type_param`), so its facts are the same wherever it
  /// appears — including in a nested generic that reads its enclosing
  /// block's `T`.
  std::unordered_map<type_id, std::vector<type_fact>> declared_facts_;

  /// Facts that hold only in part of a program, innermost last. `self` inside
  /// a trait body is the case that needs it today: it is one by-name
  /// `type_param` shared by every trait, so what it implements depends on
  /// which trait is being checked.
  std::vector<std::pair<type_id, std::vector<type_fact>>> scoped_facts_;

  // ==========================================================================
  //  check_bounds.cpp
  // ==========================================================================

  static auto bound_terms(const ast::type_expr &bound)
      -> std::vector<const ast::type_expr *>;

  auto trait_home(const ast::trait_decl &trait)
      -> std::pair<const module_members *, file_id_type>;

  auto find_concept_decl_by_name(std::string_view name)
      -> std::optional<concept_decl_ref>;

  template <typename Body>
  auto in_scope_of(const module_members *module, file_id_type file, Body body)
      -> decltype(body());

  auto add_fact_closure(const ast::trait_decl &trait, std::vector<type_id> args,
                        std::vector<type_fact> &out) -> void;

  auto add_bound_facts(type_id subject, const ast::type_expr &bound,
                       const resolve_ctx &ctx) -> void;

  auto names_trait_or_concept(const ast::type_expr &bound) -> bool;

  auto record_declared_facts(
      const std::vector<ast::type_param> &params,
      const std::vector<ast::where_constraint> *where_constraints) -> void;

  // ------------------------------------------------------------------------
  //  Domains (ch. 19, Bounded Generics, rules 4 and 5)
  //
  //  A type parameter's domain is the finite set of builtin types it can
  //  still be, or no restriction at all. A category bound (`T: numeric`)
  //  gives it one; a `static if` on `T` narrows it inside the branch and past
  //  a branch that returns. What a body may do with a `T` beyond its trait
  //  facts is exactly what every member of the domain allows: a literal, a
  //  numeric cast, a builtin operator. An empty domain is code no `T` reaches,
  //  and every such rule holds of it vacuously.
  // ------------------------------------------------------------------------

  /// Sorted, duplicate-free.
  using type_domain = std::vector<type_id>;

  /// From category bounds, keyed like `declared_facts_`.
  std::unordered_map<type_id, type_domain> declared_domains_;

  /// From `static if` narrowing, innermost last.
  std::vector<std::pair<type_id, type_domain>> scoped_domains_;

  auto category_members(std::string_view category)
      -> std::optional<type_domain>;

  auto concept_category(const ast::concept_decl &concept_decl)
      -> std::optional<type_domain>;

  auto self_param_type(const ast::param &param) -> type_id;

  auto is_copy_concept(const ast::concept_decl &concept_decl) -> bool;

  /// The type parameters declared with a `copy` bound (`record_declared_
  /// facts`), for `checked_types::copy_type_params`.
  std::unordered_set<type_id> copy_type_params_;

  auto names_copy(const ast::named_type &named) -> bool;

  auto type_is_copy(type_id subject) -> bool;

  [[nodiscard]] auto is_sum_like(type_id id) const -> bool;

  auto value_is_copy(type_id id) -> bool;

  auto value_is_copy(type_id id, std::unordered_set<type_id> &visiting) -> bool;

  static auto intersect_domains(const type_domain &a, const type_domain &b)
      -> type_domain;

  static auto subtract_domains(const type_domain &a, const type_domain &b)
      -> type_domain;

  static auto unite_domains(const type_domain &a, const type_domain &b)
      -> type_domain;

  auto domain_of(type_id param) -> std::optional<type_domain>;

  auto is_rigid_param(type_id id) -> bool;

  /// Whether every member of `param`'s domain satisfies `pred` — false when
  /// the domain is unrestricted, since then nothing says any of it does.
  template <typename Pred> auto domain_all(type_id param, Pred pred) -> bool {
    const auto domain = domain_of(param);
    return domain.has_value() && std::ranges::all_of(*domain, pred);
  }

  auto check_literal_as_param(const ast::literal_expr &lit, type_id param,
                              bool negated) -> type_id;

  auto check_cast_with_param(const ast::cast_expr &cast, type_id operand,
                             type_id target) -> void;

  // ------------------------------------------------------------------------
  //  `static if` narrowing (phase 9, rule 5)
  // ------------------------------------------------------------------------

  /// What a `static if` condition says about one type parameter: the domain
  /// in its branch, and — when the parameter's domain was already finite, so
  /// that "everything but these" is a set — the domain in its `else`.
  struct narrowing {
    type_id param = k_unknown_type;
    type_domain then_domain;
    std::optional<type_domain> else_domain;
  };

  auto reflected_name_of(const ast::expr &expr) -> std::optional<type_id>;

  auto builtin_named_by(const ast::expr &expr) -> std::optional<type_id>;

  auto with_else(type_id param, const type_domain &then_domain) -> narrowing;

  auto narrowing_of(const ast::expr &condition) -> std::optional<narrowing>;

  /// The `std.traits` traits every member of `domain` implements — what a
  /// category bound promises beyond its literals and builtin operators.
  /// `T: numeric` lets a body call `x.show()` and pass `T` where `T: add` is
  /// needed, because every builtin number is both. Cached: a domain is a
  /// small sorted set, and the answer never changes within a session.
  std::map<type_domain, std::vector<type_fact>> domain_facts_cache_;

  auto domain_facts(const type_domain &domain) -> std::vector<type_fact>;

  auto facts_of(type_id param) -> std::vector<type_fact>;

  auto resolve_type(const ast::type_expr &type, const resolve_ctx &ctx)
      -> type_id;

  auto resolve_existential_type(const ast::existential_type &ety,
                                const resolve_ctx &ctx) -> type_id;

  auto reinterpret_as_named_type(const ast::node &fragment)
      -> const ast::type_expr *;

  auto resolve_splice_type(const ast::splice_type &splice,
                           const resolve_ctx &ctx) -> type_id;

  // ==========================================================================
  //  check_type_resolution.cpp
  // ==========================================================================

  auto lookup_value_binding(std::string_view name, const resolve_ctx &ctx)
      -> std::optional<type_id>;

  auto name_poly(std::string_view name, const resolve_ctx &ctx)
      -> std::optional<linear_poly>;

  auto value_poly(const ast::node &node, const resolve_ctx &ctx)
      -> std::optional<linear_poly>;

  auto value_arg_name(const ast::node &node) -> std::optional<std::string>;

  auto resolve_value_arg(const ast::node &node, const ast::type_param &param,
                         const resolve_ctx &ctx, source_span span) -> type_id;

  auto quiet_ctx(const resolve_ctx &ctx) -> resolve_ctx;

  auto resolve_length_arg(const ast::node &node, const resolve_ctx &ctx)
      -> type_id;

  auto closed_length(type_id length) -> std::optional<uint64_t>;

  auto array_with_length(type_id element, type_id length) -> type_id;

  auto
  resolve_type_args(const std::vector<ast::type_arg> &type_args,
                    const resolve_ctx &ctx,
                    const std::vector<ast::type_param> *decl_params = nullptr)
      -> std::vector<type_id>;

  // ==========================================================================
  //  Reasoning — building facts and goals from source
  //
  //  The bridge between the AST and `src/semantic/reason.h`. A predicate the
  //  user wrote (`self < n`, `x > 0`, `i < v.len()`) becomes a `goal_form`
  //  when it must be *proved* and a `fact_set` when it may be *assumed* —
  //  the same translation either way, differing only in what happens to the
  //  parts that fall outside the fragment: an unrepresentable goal cannot be
  //  proved, while an unrepresentable fact is simply forgotten.
  //
  //  Both translations carry a substitution, because a predicate is always
  //  written about `self` and the declaring type's value parameters, and must
  //  be re-stated about the actual value and the actual arguments before it
  //  means anything at a use site: `index[n]`'s `self < n`, applied to the
  //  argument `i` at a call in a function with its own `n`, is the goal
  //  `i < n`.
  // ==========================================================================

  // ==========================================================================
  //  check_reasoning.cpp
  // ==========================================================================

  auto param_id(const ast::type_param &param) -> type_id;

  auto value_var(type_id param) -> std::string;

  auto as_value_slot(type_id id) -> type_id;

  auto ident_var(const std::string &name) -> std::string;

  auto bindings_by_name(const param_subst &bindings) -> type_scope;

  /// One explicit compile-time argument, as the parser managed to write it
  /// down.
  ///
  /// The brackets of `zeros[8]()` sit in expression position, where the
  /// parser cannot know a type was meant, so they arrive as an `expr` that
  /// `explicit_type_argument` maps back to a type when the declaration says
  /// the parameter is one. The brackets of `b.take[int64](5)` sit in a
  /// method-name suffix, which the grammar spells as a `type_arg_list`
  /// outright, so those arrive already parsed as a `type_expr`. Exactly one
  /// of the two is set; a value parameter can only be answered by the first,
  /// since a type never folds to a constant.
  struct explicit_generic_arg {
    const ast::expr *value = nullptr;     ///< Written in expression position.
    const ast::type_expr *type = nullptr; ///< Written in type position.
    source_span span;                     ///< Span of the argument itself.
  };

  /// A call's explicit compile-time arguments, in written order. Empty when
  /// the call names its callee bare and every parameter has to be read off
  /// the arguments instead.
  using explicit_generic_args = std::vector<explicit_generic_arg>;

  /// One call argument matched to the parameter it fills, held until
  /// `check_call_args_against` decides the order to infer them in.
  struct pending_argument {
    const ast::expr *value = nullptr;
    const fn_param_info *target = nullptr;
  };

  /// What `check_call_args_against` needs to solve a generic callee's type
  /// parameters partway through checking its arguments, so a deferred lambda
  /// can be checked against a substituted parameter type. Absent for a call
  /// to a non-generic callee, which has nothing to substitute.
  struct generic_call_context {
    const ast::func_decl *decl = nullptr;
    const module_members *owner = nullptr;
    const explicit_generic_args *explicit_args = nullptr;
    const ast::expr *ufcs_receiver = nullptr;
    /// Bindings the caller already solved and is not re-deriving here. A UFCS
    /// call solves its first parameter against the receiver before the
    /// written arguments are looked at (`check_ufcs_call`), and that binding
    /// is exactly the one a bound needs as its subject — `it.map(f)` knows
    /// `I` from `it` and needs `where I: iterator[T]` to get `T`.
    const param_subst *seed = nullptr;
  };

  /// A narrowing obligation raised by one call argument, held back until the
  /// whole argument list has been seen — see `check_call_args_against`.
  struct deferred_narrowing {
    type_id expected = k_unknown_type;
    type_id found = k_unknown_type;
    const ast::expr *value = nullptr;
    source_span span;
  };

  auto atom_key(const ast::expr &expr, const predicate_subst &subst)
      -> std::optional<std::string>;

  auto solver_poly(const ast::expr &expr, const predicate_subst &subst)
      -> std::optional<linear_poly>;

  /// A translator that resolves bare names through `ident_var`.
  auto translator() -> predicate_translator;

  auto describe(const ast::expr &expr, const predicate_subst &subst)
      -> std::string;

  auto goal_from(const ast::expr &expr, const predicate_subst &subst,
                 bool negated) -> goal_form;

  auto facts_from(const ast::expr &expr, const predicate_subst &subst,
                  bool negated) -> fact_set;

  /// The narrowing engine over this function's facts. The engine reasons;
  /// the checker words and reports what it finds.
  auto fact_engine() -> narrowing_engine;

  auto known_facts_note(const goal_form &goal) -> std::optional<std::string>;

  auto goal_label(const goal_form &goal, std::string_view fallback)
      -> std::string;

  auto check_narrowing(type_id expected, type_id found, const ast::expr *value,
                       source_span span, std::string_view context,
                       const value_bindings *solved = nullptr) -> void;

  auto match_value_params(type_id param, type_id argument, value_bindings &out)
      -> void;

  auto assume_binding(std::string_view name, type_id type) -> void;

  auto assume_condition(const ast::expr *condition, bool negated) -> void;

  auto invalidate_facts(std::string_view name) -> void;

  auto collect_function_facts(const ast::func_decl &decl) -> void;

  auto type_name_candidates() -> std::vector<std::string>;

  auto emit_undefined_type(const ast::named_type &named, std::string_view name)
      -> void;

  auto instantiate_user_type(const ast::type_decl &decl,
                             std::string_view owner_module,
                             const ast::named_type &named,
                             const resolve_ctx &ctx) -> type_id;

  auto bindings_for_decl(const ast::type_decl &decl,
                         const std::vector<type_id> &args) -> type_scope;

  auto make_refinement_type(const ast::type_decl &decl,
                            std::string_view owner_module,
                            std::vector<type_id> args) -> type_id;

  auto make_user_type(const ast::type_decl &decl, std::string_view owner_module,
                      std::vector<type_id> args) -> type_id;

  auto argument_ctx(const resolve_ctx &ctx) -> resolve_ctx;

  auto check_ctor_against_expected(type_id head, std::string_view name,
                                   size_t arity, source_span span,
                                   const resolve_ctx &ctx) -> type_id;

  auto apply_resolved_head(type_id head, const ast::named_type &named,
                           const resolve_ctx &ctx) -> type_id;

  auto resolve_named_type(const ast::named_type &named, const resolve_ctx &ctx)
      -> type_id;

  auto user_type_shadows_builtin(std::string_view name, const resolve_ctx &ctx)
      -> bool;

  auto resolve_mut_type(const ast::mut_type &mt, const resolve_ctx &ctx)
      -> type_id;

  auto base_shape(type_id id) -> type_id;

  auto strip_refs(type_id id) -> type_id;

  auto
  type_param_comptime_values(const std::vector<ast::type_param> &type_params,
                             const type_scope &slots)
      -> std::vector<std::pair<std::string, comptime::value>>;

  // ==========================================================================
  //  check_type_resolution.cpp
  // ==========================================================================

  auto param_bindings_for_instance(const type_entry &instance) -> type_scope;

  auto member_resolve_ctx(const type_entry &instance,
                          const type_scope &bindings) -> resolve_ctx;

  [[nodiscard]] auto enter_declaring_file(const type_entry &instance);

  auto struct_fields_of(const type_entry &instance)
      -> const std::vector<ast::struct_field> *;

  auto struct_field_type(const type_entry &instance, std::string_view name)
      -> std::optional<type_id>;

  auto sum_variants_of(const type_entry &instance)
      -> const std::vector<ast::sum_variant> *;

  auto find_variant(const type_entry &instance, std::string_view name)
      -> const ast::sum_variant *;

  auto variant_payload_types(const type_entry &instance,
                             const ast::sum_variant &variant)
      -> std::vector<type_id>;

  auto type_carries_borrow(type_id id, std::unordered_set<type_id> &visited,
                           bool params_bear = true) -> bool;

  auto compute_borrow_bearing_types(std::size_t snapshot,
                                    bool params_bear = true)
      -> std::unordered_set<type_id>;

  auto resolve_own_drop(const type_entry &entry, type_id id)
      -> std::optional<resolved_callee>;

  auto resolve_drop_plan(type_id id, std::unordered_set<type_id> &visiting)
      -> std::optional<drop_plan>;

  auto mentions_type_var(type_id id, std::unordered_set<type_id> &seen) -> bool;

  auto mentions_type_var(type_id id) -> bool;

  auto collect_type_vars(type_id id, std::vector<type_id> &out,
                         std::unordered_set<type_id> &seen) -> void;

  auto resolve_drop_plans() -> void;

  auto param_name_of(const ast::param &param) -> std::string;

  auto param_types_for(const ast::func_decl &decl, const module_members *owner)
      -> const std::vector<type_id> &;

  auto has_unannotated_params(const ast::func_decl &decl) -> bool;

  auto generic_param_bindings(
      const ast::func_decl &decl,
      const std::vector<ast::type_param> *enclosing_block_params) -> type_scope;

  auto signature_params(
      const ast::func_decl &decl, const module_members *owner, bool skip_self,
      const std::vector<ast::type_param> *enclosing_block_params = nullptr)
      -> std::vector<fn_param_info>;

  auto note_inferred_return(type_id found, source_span span) -> void;

  auto inferred_return_of(const ast::func_decl &decl,
                          const module_members *owner) -> type_id;

  std::unordered_map<const ast::func_decl *, type_id> inferred_returns_;

  std::unordered_set<const ast::func_decl *> return_inference_started_;

  std::unordered_map<const ast::func_decl *, type_id> recursive_returns_;

  std::unordered_set<const ast::func_decl *> early_checked_;

  auto signature_return_type(
      const ast::func_decl &decl, const module_members *owner,
      const std::vector<ast::type_param> *enclosing_block_params = nullptr)
      -> type_id;

  auto fn_type_of(const ast::func_decl &decl, const module_members *owner)
      -> type_id;

  // ==========================================================================
  //  check_calls.cpp
  // ==========================================================================

  [[nodiscard]] auto passing_of(type_id type) const -> param_passing;

  auto explicit_value_bindings(const generic_call_context *generic)
      -> value_bindings;

  auto check_call_args_against(const ast::call_expr &call,
                               const std::vector<fn_param_info> &params,
                               std::string_view callee_name,
                               source_location decl_location,
                               value_bindings *solved_out = nullptr,
                               const generic_call_context *generic = nullptr)
      -> void;

  /// What `check_call_against_decl` needs to know about a method call
  /// (`p.tag(3)`) beyond the declaration: the method's table entry, the
  /// impl target's display name, and the receiver. Absent for a free
  /// function.
  struct method_call_site {
    const method_entry *method = nullptr;
    std::string_view target_type_name;
    const ast::expr *receiver = nullptr;
    type_id receiver_type = k_unknown_type;
  };

  auto check_call_against_decl(const ast::call_expr &call,
                               const ast::func_decl &decl,
                               const module_members *owner,
                               file_id_type decl_file, bool skip_self,
                               const explicit_generic_args &explicit_args = {},
                               const method_call_site *method_site = nullptr)
      -> type_id;

  // ==========================================================================
  //  check_generic_calls.cpp
  // ==========================================================================

  [[nodiscard]] static auto is_generic_template(const ast::func_decl &decl)
      -> bool;

  [[nodiscard]] auto is_free_function(const ast::func_decl &decl,
                                      const module_members *owner) const
      -> bool;

  // ------------------------------------------------------------------
  //  One solution, one instance
  //
  //  Value parameters and type parameters are two answers to the same
  //  question — what does this call pin the template's parameters down to —
  //  and the checker already had two nearly identical machines for them.
  //  They are one machine below. A parameter's answer is a constant or a
  //  concrete type; either way it becomes a binding the instance's body is
  //  re-checked under (`session_.const_param_slots_` /
  //  `session_.type_param_slots_`, which `push_type_params` consults side by
  //  side) and a segment of the instance's name. A template mixing the two
  //  (`[n: usize, T]`) is then not a special case at all — it is just a
  //  solution with both kinds of binding in it.
  // ------------------------------------------------------------------

  auto bind_generic_constant(generic_solution &solution,
                             const ast::type_param &param, type_id underlying,
                             int64_t constant) -> void;

  auto solution_subst(const std::vector<ast::type_param> &params,
                      const generic_solution &solution) -> param_subst;

  auto bind_generic_type(generic_solution &solution,
                         const ast::type_param &param, type_id solved) -> void;

  [[nodiscard]] static auto instance_cardinality(const ast::func_decl &decl)
      -> std::string_view;

  /// The instance for one (template, solution) pair — reused if it already
  /// exists, cloned and checked under the solution's bindings if it doesn't.
  /// This is the single place an instance comes into being, for free
  /// functions and impl/extend methods alike: to lowering all of them are the
  /// same thing, a checked clone named for its instantiation, lowered once in
  /// its owning module while the template itself is skipped.
  ///
  /// The cache entry is published *before* the clone's body is checked, and
  /// that ordering is what makes an ordinarily recursive generic function
  /// work: the recursive call inside the body finds the instance it is
  /// currently inside, instead of instantiating a second copy of it forever.
  /// A body that recurses at a *different* argument each time (`f[n]` calling
  /// `f[n + 1]`) has no such fixed point, and is caught by the depth bound
  /// instead.
  ///
  /// `decl_file` is the file the template was written in, and checking the
  /// instance under it is load-bearing rather than cosmetic: name resolution
  /// reads a file's imports out of `file_.file_id_`, so an instance checked
  /// under the caller's file sees the caller's imports and not the template's.
  /// A generic method calling a function its own module imported then failed
  /// with `undefined name`, against a span in a file that did not contain the
  /// call. `nullopt` leaves the current file in place, for the callers that
  /// genuinely have no better answer. `fixed_type_params`
  /// carries a trait default's own bindings (`F := option`), which scope the
  /// whole instance check.
  ///
  /// `self_type` is the impl/extend target for a `self`-taking method, whose
  /// body names `self` without ever annotating it — `check_function` reads
  /// that type out of `function_.self_type_`, so an instance checked with it
  /// unset would find no type for `self` at all. `k_unknown_type` for a free
  /// function, which has no receiver to bind.
  /// `site` is the syntax that asked for this instance. It is a `node` rather
  /// than a `call_expr` because not every instantiation comes from a written
  /// call: `for x in adapter:` desugars to `adapter.next()`, and the `next`
  /// it needs a compiled copy of is reached from the `for` statement itself
  /// (`try_resolve_iterator`). Only the span and the map identity are used,
  /// both of which every node has.
  // ------------------------------------------------------------------------
  //  Leaf unknowns
  //
  //  An empty `[]` with nothing to read a type from used to be rejected
  //  outright, because a `list[?]` reaching elaboration would mint an
  //  instance that nothing ever compiled (`spec/todo.md` item 20). Phase 6
  //  moved instantiation behind a flush, so the literal can now stand for a
  //  metavariable and be pinned by a later use.
  //
  //  One store for the whole session rather than one per body: a leaf minted
  //  in one function can be solved by a constraint in another (a returned
  //  `[]` pinned by its caller), and two stores would need a protocol to say
  //  so.
  // ------------------------------------------------------------------------

  /// The leaf unknowns and their solutions.
  infer::infer_ctxt leaf_ctxt_{session_.types_};

  /// The one unifier, used to solve them where a value meets a declared type.
  infer::unifier leaf_engine_{session_.types_, leaf_ctxt_};

  /// Empty literals whose element type is still open, with the leaf standing
  /// for it. Their `from_array` wiring is deferred to `flush_leaf_literals`,
  /// because wiring it now would name an instance of `list[?]`.
  struct pending_leaf_literal {
    const ast::array_expr *literal = nullptr;
    type_id element = k_unknown_type;
    /// `nullopt` for `[v; n]` with a count only known at runtime, which is
    /// what `wire_default_list` distinguishes on. An empty `[]` is `0`.
    std::optional<uint64_t> count = 0;
    source_span span;
    file_id_type file = 0;
    /// The context the wiring was deferred *from*, restored before it runs.
    ///
    /// Deferred work has to carry the context it was deferred from, or it
    /// does something different from what it would have done in place —
    /// which is the same lesson `pending_instance` learned about its site
    /// chain and its depth. Here it is the template the literal's decisions
    /// are deferred to: a generic template's `[]` of `T`s records its
    /// conversion and leaves the `from_array` to each instance, and at flush
    /// time the walk is long over.
    const module_members *module = nullptr;
    const ast::func_decl *current_template = nullptr;
    /// Set when the body this literal is in turned out to be an implicit
    /// generic (`classify_param_decls`): its instances mint their own.
    bool skip = false;
    /// Already wired through its expected type's `from_array`, with a leaf
    /// for the hole that type had (`fill_target_holes`). Only reported if
    /// the leaf never settles.
    bool report_only = false;
  };

  std::vector<pending_leaf_literal> pending_leaf_literals_;

  /// The one queue for decisions that cannot be made yet (phase 4).
  ///
  /// It borrows the leaf store and the unifier because their postponed
  /// constraints are part of the same fixpoint: a deferred method call and a
  /// deferred `F[A] ~ G[B]` can unblock each other, and draining them in
  /// separate loops is what makes inference depend on which drained first.
  infer::obligation_queue leaf_queue_{leaf_ctxt_, leaf_engine_};

  /// Set once, lazily: the resolvers close over `this`, which is not
  /// available in a member initializer.
  bool leaf_queue_wired_ = false;

  /// A call reached while its receiver was still an open leaf.
  ///
  /// The two paths that need this — a method through a generic `impl`, and a
  /// free generic function reached by UFCS — differ only in what running the
  /// call *does*, so that is the only thing held as a closure. Everything
  /// else is shared: the receiver being waited on, and the context the work
  /// was deferred from. Carrying that context is the same discipline
  /// `pending_leaf_literal` and `pending_instance` follow, for the same
  /// reason — deferred work that does not carry its context does something
  /// other than what it would have done in place.
  ///
  /// One record and one resolver rather than two of each: `obligations.h`
  /// says outright that a queue which grows a `method_obligation` beside a
  /// `trait_obligation` has rebuilt the state the rewrite set out to
  /// replace, and two pending tables for one `method_call` kind is that
  /// mistake one level down.
  struct pending_method_call {
    /// Runs the call against the now-concrete receiver.
    std::function<void(type_id settled_receiver)> finish;
    /// The receiver whose metavariables this is waiting on. Deliberately not
    /// its `type_entry`: `list[?a]` and `list[int32]` are different entries
    /// with different method tables, and the settled one is the one the call
    /// actually means, so every lookup is redone from it.
    type_id receiver_type = k_unknown_type;
    /// How the obligation describes itself in a stall.
    std::string goal;
    file_id_type file = 0;
    const module_members *module = nullptr;
    const ast::func_decl *current_template = nullptr;
    /// The uses that asked for the copy this call stands in, so an error the
    /// re-run finds still names them (`with_instantiation_notes`).
    std::vector<instantiation_frame> sites;
  };

  std::vector<pending_method_call> pending_method_calls_;

  /// The obligations the leaves raise, and the fixpoint that discharges them.
  ///
  /// Only `defaulting` is wired today. The queue is still the right home for
  /// it rather than a list of "unsolved literals, bind them at the end":
  /// `flush` runs defaults *one at a time, each followed by another full
  /// fixpoint*, which is the difference between a last resort and a race. An
  /// integer literal defaulted to `int32` can be the thing that solves the
  /// variable a second literal was waiting on, and that second literal must
  /// then take the answer it was given rather than its own default.

  /// An integer literal whose type nothing has said yet, with the leaf
  /// standing for it.
  ///
  /// The fit check travels with it because it can no longer run at the
  /// literal: `let x = 300` is only too large for an `int8` once something
  /// has said `int8`, and until the fixpoint settles nothing has said
  /// anything.
  struct pending_leaf_literal_value {
    const ast::literal_expr *literal = nullptr;
    type_id leaf = k_unknown_type;
    bool negated = false;
    file_id_type file = 0;
  };

  std::vector<pending_leaf_literal_value> pending_leaf_values_;

  /// The leaves standing for an integer literal. Such a leaf can only ever
  /// be a number, which is a fact about the *literal*, not something the
  /// unifier can see.
  std::unordered_set<type_id> integer_literal_leaves_;

  auto find_or_check_generic_instance(
      const ast::node &call, const ast::func_decl &decl,
      const module_members *owner, std::optional<file_id_type> decl_file,
      const generic_solution &solution, const std::string &name,
      const type_scope *fixed_type_params, type_id self_type = k_unknown_type,
      const std::vector<ast::type_param> *block_type_params = nullptr,
      const std::vector<type_id> *param_types = nullptr)
      -> const ast::func_decl *;

  auto flush_leaf_literals() -> void;

  auto defer_method_call(std::string_view name, type_id receiver_type,
                         std::optional<std::vector<type_id>> binds,
                         std::function<void(type_id)> finish) -> void;

  /// Where a body was standing when a call in it was deferred: everything a
  /// re-run needs to check an expression as if it were still there.
  ///
  /// Captured by value. The bindings are immutable once made, and a re-run
  /// only reads them, so a copy stands in for the scopes that are gone.
  struct body_context {
    std::vector<std::unordered_map<std::string, value_binding>> scopes;
    std::vector<capture_barrier> capture_barriers;
    std::vector<type_scope> type_params;
    type_scope type_param_slots;
    type_scope const_param_slots;
    type_id self_type = k_unknown_type;
    type_scope self_assoc_types;
    fact_set facts;
    type_scope type_name_aliases;
    const std::vector<ast::type_param> *enclosing_block_type_params = nullptr;
    bool in_machine_function = false;
  };

  [[nodiscard]] auto capture_body_context() const -> body_context;

  auto swap_body_context(body_context &context) -> void;

  // ------------------------------------------------------------------------
  //  Generic lambdas
  //
  //  `let f = (x) => x + x` gives `x` no annotation and nothing to read one
  //  from. The spec infers such a parameter the way it infers an unannotated
  //  `def` parameter: it is an implicit type parameter. So `f` is generic,
  //  and each use of it checks its own copy of the lambda — `f(3)` one for
  //  `int32`, `f(2.5)` another for `float64` — exactly as each call to an
  //  implicit-generic function compiles its own instance.
  //
  //  A copy is checked in the scope the `let` stood in (`body_context`), not
  //  the one the use stands in: the lambda captures what was visible where it
  //  was written, and a later `let` that shadows a captured name must not
  //  change what the lambda means. Each unannotated parameter starts as a
  //  fresh leaf, so the use's arguments and the lambda's own body solve it
  //  together, before any literal is defaulted. Lowering creates one closure
  //  per copy where the `let` stands (`checked_types::
  //  generic_lambda_instances`), and none for a lambda that is never used.
  // ------------------------------------------------------------------------

  /// Where a generic lambda was bound, for checking its copies there.
  struct generic_lambda_site {
    body_context context;
    std::string name;
    const ast::node *pattern = nullptr;
  };

  std::unordered_map<const ast::lambda_expr *, generic_lambda_site>
      generic_lambda_sites_;

  std::unordered_map<const ast::lambda_expr *,
                     std::vector<const ast::lambda_expr *>>
      generic_lambda_instances_;

  std::unordered_map<const void *, const ast::lambda_expr *>
      generic_lambda_refs_;

  std::unordered_map<const ast::lambda_expr *,
                     ownership_results::generic_lambda_use>
      generic_lambda_uses_;

  ast::ptr_vec<ast::expr> synthesized_lambdas_;

  [[nodiscard]] static auto generic_lambda_of(const ast::let_stmt &stmt)
      -> const ast::lambda_expr *;

  [[nodiscard]] static auto
  has_unannotated_param(const ast::lambda_expr &lambda) -> bool;

  auto report_mutable_generic_lambda(const ast::expr *initializer,
                                     bool annotated, std::string_view name)
      -> bool;

  auto bind_generic_lambda(const ast::let_stmt &stmt,
                           const ast::lambda_expr &lambda) -> void;

  /// A use of a generic lambda's name: the node lowering finds it by (an
  /// `ast::ident_expr`, or a shorthand `ast::struct_field_init`), and where
  /// it was written.
  struct generic_lambda_use {
    const void *node = nullptr;
    std::string_view name;
    source_span span;
  };

  auto instantiate_generic_lambda(const ast::lambda_expr &tmpl,
                                  const generic_lambda_use &ident,
                                  const std::vector<type_id> *params)
      -> type_id;

  auto generic_lambda_value(const generic_lambda_use &ident,
                            const value_binding &binding, type_id expected)
      -> type_id;

  auto invoked_lambda_expectation(const ast::expr &callee) -> type_id;

  auto clone_generic_lambdas_for(pending_instance &item) -> void;

  auto defer_open_receiver_call(const ast::call_expr &call,
                                const ast::field_expr &field)
      -> std::optional<type_id>;

  /// What a deferred call's receiver and arguments answer with while it is
  /// re-run; see `defer_open_receiver_call`.
  std::unordered_map<const ast::expr *, type_id> replayed_exprs_;

  auto wire_leaf_queue() -> void;

  auto resolve_deferred_method_call(pending_method_call &deferred)
      -> infer::obligation_report;

  /// A call to a function whose parameter has no annotation, waiting for that
  /// function's body to say what the parameter is.
  ///
  /// The spec's rule is that an unannotated parameter is inferred from its
  /// *own function's body, never from a call site*. So a call cannot decide
  /// anything until the callee has been checked, and the callee may be in a
  /// file not walked yet — which is why the decision is deferred rather than
  /// made by checking the callee's body on demand in the middle of another
  /// function's walk. Two outcomes once the body has spoken:
  ///
  ///   - the body pinned the parameter (`wide(x)`): the function is an
  ///     ordinary concrete one, and this call's argument must simply agree;
  ///   - the body left it open (`x + x`): the parameter is an implicit type
  ///     parameter, and this call gets an instance compiled for its argument.
  struct pending_open_param_call {
    const ast::call_expr *call = nullptr;
    const ast::func_decl *decl = nullptr;
    const module_members *owner = nullptr;
    file_id_type decl_file = 0;
    /// The types this call checked its arguments against; an unannotated
    /// parameter's entry is this call's private copy of its leaf.
    std::vector<type_id> call_params;
    file_id_type file = 0;
    const module_members *module = nullptr;
    /// The receiver of a UFCS call (`x.probe()`) or of a method call
    /// (`p.tag(3)`), which is the callee's first argument; `nullptr` for an
    /// ordinary call.
    const ast::expr *receiver = nullptr;
    /// Set when the callee is a method of a non-generic `impl`/`extend` block
    /// rather than a free function: its instance is named for the block's
    /// target and checked with `self` bound to `self_type`.
    std::optional<method_entry> method;
    std::string target_type_name;
    type_id self_type = k_unknown_type;
    /// Set when the method's block is itself generic (`extend[T] box[T]`):
    /// the instance is named for the receiver too, and the block's
    /// parameters are solved from it (`open_call_block_bindings`).
    bool block_generic = false;
    /// The call's result when the callee declares no return type: a leaf that
    /// waits for the callee's inferred one, which for an open callee is the
    /// instance's, not the template's.
    type_id result_leaf = k_unknown_type;
    /// The probed function whose body made this call, if any: its verdict
    /// decides whether the call stands (`resolve_open_param_calls`).
    const ast::func_decl *probe = nullptr;
    /// Whether the call's types have been solved against its callee's
    /// signature (`type_open_param_call`).
    bool typed = false;
  };

  std::vector<pending_open_param_call> pending_open_param_calls_;

  auto mint_open_result(const ast::func_decl &decl, source_span span) -> void;

  [[nodiscard]] auto open_call_result(const ast::call_expr &call,
                                      type_id declared) const -> type_id;

  /// Whether each probed function's body left a parameter open — an
  /// implicit generic — or pinned them all. Decided once, right after the
  /// body is checked and *before* the literal-defaulting flush: an integer
  /// literal in the body (`x * 2`) would otherwise default `x` to `int32`
  /// and turn a function generic over every number into an `int32` one. A
  /// pending call is only resolved once its callee is in here.
  std::unordered_map<const ast::func_decl *, bool> open_param_decls_;

  /// A probed function not classified yet, and the deferred work its body
  /// queued (as index ranges into the two queues), which has to be dropped
  /// if the function turns out to be an implicit generic: that work is about
  /// a body checked against an open leaf, and each instance queues its own.
  struct unclassified_param_decl {
    const ast::func_decl *decl = nullptr;
    size_t literal_begin = 0;
    size_t literal_end = 0;
    size_t call_begin = 0;
    size_t call_end = 0;
  };

  std::vector<unclassified_param_decl> unclassified_param_decls_;

  /// See `implicit_signature` in types.h; each call instantiates the leaves
  /// afresh (`link_open_param_call`).
  std::unordered_map<const ast::func_decl *, implicit_signature>
      implicit_signatures_;

  /// See `checked_types::instance_templates`.
  std::unordered_map<const ast::func_decl *, const ast::func_decl *>
      instance_templates_;

  /// A body's inferred return type that was still open when the body
  /// ended; the implicit generic's result type if it is open on the
  /// parameters.
  std::unordered_map<const ast::func_decl *, type_id> open_returns_;

  auto classify_param_decls() -> void;

  auto classify_probe(
      const unclassified_param_decl &probe,
      const std::function<bool(const ast::func_decl *)> &unclassified) -> void;

  auto type_group_call(pending_open_param_call &call) -> void;

  auto has_open_param(const ast::func_decl &decl,
                      const std::vector<type_id> &types) -> bool;

  auto resolve_open_param_calls() -> void;

  auto type_open_param_call(pending_open_param_call &item) -> void;

  auto name_open_param_instance(pending_open_param_call item) -> void;

  auto open_call_block_bindings(const pending_open_param_call &item)
      -> param_subst;

  auto name_block_method_instance(const pending_open_param_call &item) -> void;

  auto name_impl_method_instance(const ast::call_expr &call,
                                 std::string_view method_name,
                                 const ast::expr &receiver, type_id settled)
      -> void;

  auto queue_open_param_call(pending_open_param_call item) -> void;

  auto instantiate_open_param_call(const pending_open_param_call &item,
                                   const std::vector<type_id> &callee_types)
      -> void;

  struct pending_open_result {
    const ast::call_expr *call = nullptr;
    const ast::func_decl *instance = nullptr;
    type_id leaf = k_unknown_type;
    std::vector<type_id> fn_params;
    file_id_type file = 0;
  };

  std::vector<pending_open_result> pending_open_results_;

  auto finish_open_results(bool last = false) -> void;

  auto signature_param_type(const ast::func_decl &decl,
                            const module_members *owner, size_t index)
      -> type_id;

  auto flush_deferred() -> void;

  auto flush_leaf_values() -> void;

  /// Decides when each generic instance is made; see instance_queue.h.
  instance_queue instances_;

  /// The checker's half of `instance_queue`: what making one instance does.
  class instance_maker;

  auto walks_instance(const pending_instance &item) -> bool;

  auto record_instance(const pending_instance &item) -> void;

  auto instance_answers_are_concrete(const pending_instance &item) -> bool;

  auto flush_pending_instances() -> void;

  auto flush_substitutions() -> void;

  auto flush_unchecked_templates() -> void;

  auto check_instance(pending_instance &item) -> void;

  template <typename Body>
  auto with_instance_context(pending_instance item, Body body) -> void;

  // ------------------------------------------------------------------------
  //  Instances by substitution
  //
  //  A generic body is checked once, as the template, against the facts its
  //  signature declares (phase 9). An instance is then *not* checked again:
  //  its types are the template's records with each abstract parameter
  //  replaced by the instance's answer, copied onto the clone's nodes.
  //
  //  What cannot be copied is a decision the template had no concrete type
  //  to make — which instance of `list::push` a `xs.push(x)` on a `list[T]`
  //  calls, which `static if` branch a `T.name() == "int8"` takes. The
  //  template records each such decision as a *template elaboration* at the
  //  line that needed it, and each instance runs them in source order under
  //  its own substitution. This is the whole of what an instance does.
  // ------------------------------------------------------------------------

  /// A decision the template deferred to its instances. `span` is the
  /// template syntax that needed it, which is how a decision inside a branch
  /// the instance does not take is skipped.
  struct template_elaboration {
    source_span span;
    std::function<void(instance_subst &)> run;
  };

  std::unordered_map<const ast::func_decl *, std::vector<template_elaboration>>
      template_elaborations_;

  /// The abstract parameters in scope while each template was checked, by
  /// name — what an instance's answers are matched against.
  std::unordered_map<const ast::func_decl *, type_scope>
      template_abstract_params_;

  // ==========================================================================
  //  check_instances.cpp
  // ==========================================================================

  auto defer_to_instances(const ast::node &site,
                          std::function<void(instance_subst &)> run) -> void;

  /// `id` under `subst`; see `substitute_type` in instance_substitution.h.
  auto substitute_type(type_id id, instance_subst &subst) -> type_id;

  /// The record tables an instance copies from its template.
  auto template_records() -> template_record_tables;

  auto copy_all_template_records(instance_subst &subst) -> void;

  auto drop_all_dead_records(const instance_subst &subst) -> void;

  auto instance_subst_for(const pending_instance &item) -> instance_subst;

  auto substitute_instance(pending_instance &item) -> void;

  auto explicit_generic_callee(const ast::expr &callee,
                               explicit_generic_args &args_out)
      -> const ast::expr *;

  auto explicit_type_argument(const explicit_generic_arg &arg,
                              bool *already_reported = nullptr)
      -> std::optional<type_id>;

  auto expr_as_named_type(const ast::expr &arg) -> ast::ptr<ast::named_type>;

  auto explicit_value_argument(const ast::expr &arg) -> std::optional<int64_t>;

  auto value_param_underlying(const ast::type_param &param,
                              const module_members *owner)
      -> std::optional<type_id>;

  auto settle_bindings(param_subst &bindings) -> void;

  auto
  solve_generic_params(const ast::call_expr &call, const ast::func_decl &decl,
                       const module_members *owner,
                       const value_bindings &solved, param_subst &type_bindings,
                       const explicit_generic_args &explicit_args)
      -> std::optional<generic_solution>;

  auto solve_from_argument_types(const ast::call_expr &call,
                                 const std::vector<fn_param_info> &params,
                                 param_subst &bindings,
                                 const ast::expr *ufcs_receiver = nullptr,
                                 bool bind_open = false) -> void;

  auto preliminary_type_bindings(const ast::call_expr &call,
                                 const std::vector<fn_param_info> &params,
                                 const generic_call_context &generic)
      -> param_subst;

  [[nodiscard]] static auto
  ufcs_argument_for(size_t index,
                    const std::vector<const ast::expr *> &args_by_param,
                    const ast::expr *ufcs_receiver) -> const ast::expr *;

  auto check_args_against_solution(
      const ast::call_expr &call, const std::vector<fn_param_info> &params,
      const param_subst &bindings, std::string_view callee_name,
      const ast::expr *ufcs_receiver = nullptr,
      const std::unordered_set<type_id> &fixed_params = {}) -> bool;

  [[nodiscard]] auto interchangeable(type_id a, type_id b) const -> bool;

  auto
  report_argument_conflicts(const std::vector<fn_param_info> &params,
                            const param_subst &bindings,
                            std::string_view callee_name,
                            const std::vector<const ast::expr *> &args_by_param,
                            const ast::expr *ufcs_receiver,
                            const std::unordered_set<type_id> &fixed_params)
      -> std::unordered_set<const ast::expr *>;

  auto report_split_argument(const ast::expr &argument, type_id declared,
                             type_id found,
                             const infer::rigid_match_result &matched,
                             std::string_view callee_name) -> bool;

  auto solve_from_expected_type(const ast::call_expr &call,
                                const ast::func_decl &decl,
                                const module_members *owner,
                                param_subst &bindings,
                                const explicit_generic_args &explicit_args)
      -> void;

  auto solve_call(const ast::call_expr &call, const ast::func_decl &decl,
                  const module_members *owner, const value_bindings &solved,
                  param_subst &bindings,
                  const explicit_generic_args &explicit_args)
      -> std::optional<generic_solution>;

  auto solve_generic_call(const ast::call_expr &call,
                          const ast::func_decl &decl,
                          const module_members *owner, file_id_type decl_file,
                          const value_bindings &solved,
                          const std::vector<fn_param_info> &params,
                          const explicit_generic_args &explicit_args,
                          const ast::expr *ufcs_receiver)
      -> std::optional<generic_solution>;

  auto solved_signature(const ast::func_decl &decl, const module_members *owner,
                        file_id_type decl_file,
                        const generic_solution &solution)
      -> std::pair<std::vector<type_id>, type_id>;

  auto mentions_rigid_param(type_id id) -> bool;

  auto open_foreign_params(type_id id) -> type_id;

  auto solution_mentions_template_param(const generic_solution &solution)
      -> bool;

  // ==========================================================================
  //  check_bounds.cpp
  // ==========================================================================

  auto builtin_has_trait(type_id member, const ast::trait_decl &trait) -> bool;

  auto satisfies_trait(type_id subject, const ast::trait_decl &trait,
                       const std::vector<type_id> &args) -> bool;

  auto satisfies_concept(type_id subject, const ast::concept_decl &concept_decl)
      -> bool;

  auto bound_holds(type_id subject, const ast::type_expr &bound,
                   const resolve_ctx &ctx) -> std::vector<std::string>;

  auto check_call_bounds(const ast::call_expr &call, const ast::func_decl &decl,
                         const module_members *owner, file_id_type decl_file,
                         const generic_solution &solution,
                         const type_scope *fixed = nullptr) -> bool;

  auto instantiate_generic_function(
      const ast::call_expr &call, const ast::func_decl &decl,
      const module_members *owner, file_id_type decl_file,
      const value_bindings &solved, const std::vector<fn_param_info> &params,
      const explicit_generic_args &explicit_args,
      const ast::expr *ufcs_receiver = nullptr) -> std::optional<type_id>;

  auto solution_is_open(const generic_solution &solution) -> bool;

  auto when_solution_settles(
      const ast::func_decl &decl, const generic_solution &solution,
      std::function<void(const generic_solution &)> name_instance) -> void;

  auto defer_generic_call(const ast::call_expr &call,
                          const ast::func_decl &decl,
                          const module_members *owner, file_id_type decl_file,
                          const generic_solution &solution,
                          const ast::expr *ufcs_receiver) -> void;

  auto substitute_solution(const ast::func_decl &decl,
                           const generic_solution &solution,
                           instance_subst &subst)
      -> std::optional<generic_solution>;

  auto settled_solution(const ast::func_decl &decl,
                        const generic_solution &solution)
      -> std::optional<generic_solution>;

  auto map_solution(const ast::func_decl &decl,
                    const generic_solution &solution,
                    const std::function<type_id(type_id)> &map)
      -> std::optional<generic_solution>;

  auto instantiate_solved_call(const ast::call_expr &call,
                               const ast::func_decl &decl,
                               const module_members *owner,
                               file_id_type decl_file,
                               const generic_solution &solved_call,
                               const ast::expr *ufcs_receiver)
      -> std::optional<type_id>;

  auto report_unsolved_value_param(const ast::call_expr &call,
                                   const ast::func_decl &decl,
                                   const ast::type_param &param) -> void;

  auto report_unsolved_type_param(const ast::call_expr &call,
                                  const ast::func_decl &decl,
                                  const ast::type_param &param) -> void;

  auto report_non_numeric_value_param(const ast::call_expr &call,
                                      const ast::func_decl &decl,
                                      const ast::type_param &param) -> void;

  // ==========================================================================
  //  check_instances.cpp
  // ==========================================================================

  auto mangle_type_for_instance(type_id id) -> std::string;

  auto instantiate_hk_method(const ast::call_expr &call,
                             const method_entry &method,
                             std::string_view target_type_name,
                             param_subst &bindings,
                             const resolved_callee &callee,
                             const explicit_generic_args &explicit_args = {},
                             const value_bindings &solved = {},
                             type_id self_type = k_unknown_type,
                             const type_scope *extra_fixed = nullptr) -> bool;

  auto check_call_preconditions(const ast::call_expr &call,
                                const ast::func_decl &decl,
                                const std::vector<fn_param_info> &params,
                                const ast::expr *ufcs_receiver = nullptr)
      -> void;

  auto infer_call_args_loosely(const ast::call_expr &call) -> void;

  auto check_variant_construction(const ast::call_expr &call,
                                  const type_entry &instance,
                                  const ast::sum_variant &variant,
                                  type_id instance_id) -> type_id;

  auto check_prelude_variant(std::string_view name, const ast::call_expr *call,
                             source_span span, type_id expected) -> type_id;

  auto find_module_variant(std::string_view name) -> std::optional<
      std::pair<const ast::type_decl *, const ast::sum_variant *>>;

  auto resolve_variant_value(std::string_view name, const ast::call_expr *call,
                             source_span span, type_id expected)
      -> std::optional<type_id>;

  // ==========================================================================
  //  check_scopes.cpp
  // ==========================================================================

  auto is_type_like_name(std::string_view name) -> bool;

  auto value_name_candidates() -> std::vector<std::string>;

  auto emit_undefined_name(source_span span, std::string_view name) -> void;

  auto is_prelude_value_name(std::string_view name) -> bool;

  /// A module-level value a bare name reaches with no local binding in the
  /// way: a `def` or `static` of the module being checked, then an imported
  /// one (`use p.{f}`), then one re-exported by a session-owned wildcard
  /// import. Exactly one of `function`/`static_binding` is set.
  struct module_value_ref {
    const ast::func_decl *function = nullptr;
    const ast::static_decl *static_binding = nullptr;
    const module_members *owner = nullptr;
  };

  [[nodiscard]] auto find_module_scope_value(std::string_view name) const
      -> std::optional<module_value_ref>;

  auto resolve_ident(const ast::ident_expr &ident, type_id expected) -> type_id;

  auto emit_bare_variant(source_span span, std::string_view name, bool is_call)
      -> void;

  auto emit_undefined_variant(source_span span, std::string_view name,
                              type_id expected) -> void;

  auto static_binding_type(const ast::static_decl &decl,
                           const module_members *owner) -> type_id;

  auto check_static_binding_early(const ast::static_decl &decl,
                                  const module_members *owner) -> void;

  auto ensure_static_binding_evaluated(const ast::static_decl &decl,
                                       const module_members *owner = nullptr)
      -> const comptime::value *;

  auto record_fn_value_reference(const ast::expr &reference,
                                 const ast::func_decl &decl,
                                 std::string_view owner_module) -> void;

  auto record_static_const_reference(const ast::expr &reference,
                                     const ast::static_decl &decl, type_id type,
                                     std::string_view owner_module) -> void;

  auto reify_static_global(const ast::static_decl &decl,
                           const comptime::value &value, type_id type,
                           std::string_view owner_module)
      -> std::optional<std::string>;

  auto reify_value(const comptime::value &value, type_id type, source_span span)
      -> std::optional<reified_value>;

  auto record_const_param_reference(const ast::ident_expr &ident, type_id type)
      -> void;

  auto materialize_const_literal(const comptime::value &value, source_span span,
                                 type_id type) -> const ast::literal_expr *;

  auto build_const_literal(const comptime::value &value, source_span span,
                           type_id type) -> ast::ptr<ast::literal_expr>;

  auto build_static_struct_expr(const comptime::value &value, source_span span,
                                type_id type) -> ast::ptr<ast::expr>;

  auto try_fold_comptime_only_call(const ast::call_expr &call,
                                   const ast::func_decl &decl,
                                   const ast::func_decl &instance,
                                   type_id result_type,
                                   const type_scope &type_slots)
      -> const ast::literal_expr *;

  // ==========================================================================
  //  check_operators.cpp
  // ==========================================================================

  auto check_integer_fit(const ast::literal_expr &lit, type_id target,
                         bool negated = false) -> void;

  auto open_integer_literal(const ast::literal_expr &lit, type_id expected,
                            bool negated) -> type_id;

  auto infer_literal(const ast::literal_expr &lit, type_id expected,
                     bool negated = false) -> type_id;

  auto operator_trait_for(ast::binary_op op) -> std::string_view;

  auto parameterized_trait_key(std::string_view trait_name, type_id argument)
      -> std::string;

  auto impl_trait_arguments_key(const ast::impl_decl &decl,
                                std::string_view trait_name,
                                const resolve_ctx &ctx) -> std::string;

  auto resolve_operator_return_type(type_id target, std::string_view trait_name,
                                    const method_entry &method) -> type_id;

  auto require_operand_trait(const ast::binary_expr &binary, type_id operand,
                             std::string_view trait_name,
                             std::string_view op_name, bool wire_dispatch)
      -> type_id;

  auto report_non_numeric_arithmetic(const ast::binary_expr &binary,
                                     std::string_view op_name, type_id operand)
      -> void;

  auto defer_open_arithmetic(const ast::binary_expr &binary, type_id open)
      -> type_id;

  enum class operator_kind : uint8_t { arithmetic, equality, ordering };

  auto defer_operator_dispatch(const ast::binary_expr &binary, type_id lhs,
                               operator_kind kind) -> void;

  auto infer_arithmetic(const ast::binary_expr &binary, type_id expected)
      -> type_id;

  auto infer_comparison(const ast::binary_expr &binary, bool is_equality)
      -> type_id;

  auto wire_ord_dispatch(const ast::binary_expr &binary, type_id lhs) -> void;

  auto wire_str_equality_dispatch(const ast::binary_expr &binary, type_id lhs)
      -> void;

  auto require_bool(const ast::expr &expr, std::string_view context) -> void;

  static auto quote_fragment_kind_name(comptime::value_kind kind)
      -> std::string_view;

  auto require_quote_value(type_id raw_found, source_span span,
                           std::string_view context) -> void;

  auto infer_binary(const ast::binary_expr &binary, type_id expected)
      -> type_id;

  auto raw_pointer_element_address(const ast::unary_expr &unary,
                                   type_id expected) -> std::optional<type_id>;

  auto infer_unary(const ast::unary_expr &unary, type_id expected) -> type_id;

  // ==========================================================================
  //  Impl / method tables
  // ==========================================================================

  /// Maps `"{trait_name}:{type_key}"` to the location of the first impl
  /// seen for that (trait, type) pair, session-wide — built by
  /// `validate_impl_coherence` and consulted by `type_has_trait`.
  /// One recorded trait impl, keyed by `type_key_of` (declaration-level, so
  /// `type_has_trait` finds an impl through any instantiation). `target` is
  /// the impl's actual target type, which is what conflict detection needs
  /// and the key deliberately drops.
  struct recorded_impl {
    source_location location;
    type_id target = k_unknown_type;
  };

  std::unordered_map<std::string, recorded_impl> impl_trait_index_;

  /// One impl as conflict detection sees it: where it is, what it targets,
  /// and which single trait argument it supplies (`k_unknown_type` for a
  /// trait that takes none).
  struct coherence_record {
    source_location location;
    /// The trait's *declaration*, not its spelling. Two different traits can
    /// share a name — a module defining its own `trait from_iter` alongside
    /// `std.iter`'s — and implementing both for one type is no conflict at
    /// all. The key below carries only the name, so this is what actually
    /// decides identity.
    const ast::trait_decl *trait_decl = nullptr;
    type_id target = k_unknown_type;
    type_id trait_arg = k_unknown_type;
  };

  /// Every impl seen, grouped by `"{trait_name}:{type_key}"` — the *bare*
  /// key, with no trait arguments in it.
  ///
  /// Conflict detection cannot key on the instantiated trait
  /// (`"index[usize]:list"`) the way `impl_trait_index_` does: two impls
  /// that key differently may still both apply. `impl get_it[T] for
  /// boxed[T]` and `impl get_it[int32] for boxed[int32]` key as
  /// `get_it[T]:boxed` and `get_it[int32]:boxed`, and both answer
  /// `boxed[int32].get()`. So every impl for a (trait, type declaration)
  /// pair is kept here and compared pairwise on *both* halves — the target
  /// and the trait argument — which keeps `index[usize]` and
  /// `index[range[usize]]` on one `list[T]` legal (their arguments are
  /// disjoint concrete types) without letting a blanket impl slip past a
  /// concrete sibling.
  std::unordered_map<std::string, std::vector<coherence_record>>
      impl_coherence_records_;

  /// Reverse of `impl_trait_index_`, keyed by `type_key_of` alone (not
  /// `"{trait_name}:{type_key}"`): every trait name a type has a kind-`*`
  /// impl for. Built alongside `impl_trait_index_` in `validate_impl_
  /// coherence` and mirrored into the comptime evaluator (`register_
  /// comptime_coherence_info`) so `implements[T, Trait]()`/`T.traits()`
  /// (`spec/specification/04-stdlib/type-traits/{58,60}-*.md`) can answer
  /// without the evaluator reaching back into `checker`.
  std::unordered_map<std::string, std::vector<std::string>> traits_by_type_key_;

  // ==========================================================================
  //  check_methods.cpp
  // ==========================================================================

  auto trait_name_of_impl(const ast::impl_decl &impl) -> std::string;

  auto type_key_of(const type_entry &entry) -> std::string;


  auto impl_targets_overlap(type_id first, type_id second) -> bool;

  auto find_session_trait(std::string_view name)
      -> std::optional<trait_decl_ref>;

  /// The stdlib modules whose members are reachable without an explicit
  /// `use`. Shared by the type and variant lookups below, which have to agree:
  /// a sum type reachable by name whose variants were not would let
  /// `ordering` be written in a signature and never constructed.
  static constexpr std::array<std::string_view, 4> k_prelude_reexport_modules =
      {"prelude", "std.traits", "std.iter", "std.list"};

  auto find_prelude_trait(std::string_view name)
      -> std::optional<trait_decl_ref>;

  auto find_prelude_concept(std::string_view name)
      -> std::optional<concept_decl_ref>;

  auto find_prelude_type(std::string_view name)
      -> std::optional<std::pair<const ast::type_decl *, std::string>>;

  /// Finds a sum variant named `name` declared by a prelude-re-exported type
  /// — `@less`/`@equal`/`@greater` of `ordering`. Returns the declaring type
  /// and module alongside the variant, for the same interning reason
  /// `find_prelude_type` does.
  struct prelude_variant_ref {
    const ast::type_decl *sum_decl;
    const ast::sum_variant *variant;
    std::string module_name;
  };

  auto find_prelude_variant(std::string_view name)
      -> std::optional<prelude_variant_ref>;

  auto find_prelude_function(std::string_view name)
      -> std::optional<std::pair<func_decl_ref, const module_members *>>;

  auto resolve_trait_decl_from(const std::string &trait_name,
                               const module_members *home)
      -> const ast::trait_decl *;

  auto impl_expected_ctor_arity(const ast::impl_decl &impl,
                                const module_members *home)
      -> std::optional<size_t>;

  auto resolve_impl_trait_key(const impl_ref &impl, std::string_view trait_name)
      -> std::string;

  auto resolve_impl_trait_argument(const impl_ref &impl) -> type_id;

  auto resolve_impl_target(const impl_ref &impl) -> type_id;

  auto resolve_extend_target(const extend_ref &ext) -> type_id;

  auto build_method_table() -> void;

  auto monomorphize_trait_default(const ast::func_decl &decl, type_id target,
                                  std::string_view target_type_name,
                                  const ast::trait_decl *trait_decl,
                                  const module_members *trait_module,
                                  file_id_type trait_file_id,
                                  std::deque<method_entry> &methods) -> void;


  /// A view of the method tables for a lookup that needs none of the
  /// checker's own work (no table build, no derivation).
  auto method_lookup() -> method_resolver;

  /// The one query over the method tables, for a receiver `instance` that
  /// is the type `id` (`k_unknown_type` when the caller has only the entry).
  /// Builds the tables first, and when nothing is declared, derives the
  /// methods of a generic `deriving` type for this instantiation and asks
  /// again — see `method_resolver::resolve` for the rules of the search.
  auto resolve_method(const type_entry &instance, type_id id,
                      std::string_view name, const method_filter &accepts = {})
      -> method_resolution;

  auto resolve_method(type_id receiver, std::string_view name,
                      const method_filter &accepts = {}) -> method_resolution;

  /// The method `resolve_method` finds when it is a declared one, which is
  /// the only kind a user type can have. A builtin receiver answers null.
  auto find_method(const type_entry &instance, std::string_view name,
                   type_id instance_id = k_unknown_type,
                   const method_filter &accepts = {}) -> const method_entry *;

  [[nodiscard]] static auto
  has_builtin_inherent_method(const type_entry &object, std::string_view name)
      -> bool;

  auto match_params(type_id pattern, type_id concrete, param_subst &bindings,
                    bool allow_override = false) -> void;

  auto mentions_abstract_type(type_id id, bool leaves_are_answers = false)
      -> bool;

  auto mentions_template_param(type_id id) -> bool;

  auto mentions_type_param(type_id id) -> bool;

  auto mentions_unknown(type_id id) -> bool;

  auto substitute_solved(type_id id, const param_subst &bindings) -> type_id;

  auto type_has_trait(const type_entry &instance, std::string_view trait_name)
      -> bool;

  auto trait_args_of_impl_for(type_id concrete, std::string_view trait_name)
      -> std::optional<std::vector<type_id>>;

  auto solve_from_bounds(const ast::func_decl &decl,
                         const module_members *owner, param_subst &bindings)
      -> void;

  auto derived_method_result(const type_entry &instance, std::string_view name)
      -> std::optional<type_id>;

  // ==========================================================================
  //  check_calls.cpp
  // ==========================================================================

  auto check_call_against_fn_type(const ast::call_expr &call,
                                  const type_entry &fn_entry,
                                  std::string_view callee_name) -> type_id;

  auto builtin_method_result(const type_entry &object, std::string_view name)
      -> type_id;

  [[nodiscard]] auto report_unknown_builtin_method(const ast::call_expr &call,
                                                   const ast::field_expr &field,
                                                   const type_entry &entry,
                                                   type_id object)
      -> std::optional<type_id>;

  [[nodiscard]] static auto builtin_method_names(const type_entry &object)
      -> std::vector<std::string>;

  auto available_method_name_list(const type_entry &instance)
      -> std::vector<std::string>;

  auto available_method_names(const type_entry &instance) -> std::string;

  auto instantiate_impl_method_for(const ast::node &site,
                                   const method_entry &method,
                                   const type_entry &receiver_entry,
                                   type_id receiver_type,
                                   std::string_view discriminator = {})
      -> const ast::func_decl *;

  auto record_instance_method_callee(const ast::call_expr &call,
                                     const method_entry &method,
                                     std::string_view target_type_name,
                                     const ast::expr &receiver) -> void;

  auto check_generic_instance_method_call(
      const ast::call_expr &call, const method_entry &method,
      std::string_view target_type_name, const ast::expr &receiver,
      type_id receiver_type, const explicit_generic_args &explicit_args = {})
      -> std::optional<type_id>;

  auto impl_needs_instance(const method_entry &method,
                           const type_entry &receiver) -> bool;

  auto solve_impl_value_params(const method_entry &method, type_id concrete,
                               param_subst &bindings) -> void;

  auto carry_impl_value_slots(const method_entry &method,
                              const param_subst &bindings,
                              generic_solution &solution) -> void;

  auto defer_impl_method_call(const ast::call_expr &call, type_id receiver_type)
      -> void;

  auto method_field_of(const ast::call_expr &call,
                       explicit_generic_args &explicit_args)
      -> const ast::field_expr *;

  auto template_impl_method_call(const ast::call_expr &call,
                                 const method_entry &method,
                                 const type_entry &receiver_entry,
                                 const ast::expr &receiver,
                                 type_id receiver_type)
      -> std::optional<type_id>;

  auto check_impl_generic_method_call(const ast::call_expr &call,
                                      const method_entry &method,
                                      const type_entry &receiver_entry,
                                      const ast::expr &receiver,
                                      type_id receiver_type)
      -> std::optional<type_id>;

  auto finish_impl_generic_method_call(
      const ast::call_expr &call, const method_entry &method,
      std::string_view receiver_name, const ast::expr &receiver,
      type_id receiver_type, const param_subst &bindings,
      const type_scope &scoped_params, const generic_solution &solution)
      -> std::optional<type_id>;

  auto check_impl_generic_static_call(const ast::call_expr &call,
                                      const method_entry &method,
                                      type_id target, param_subst &bindings)
      -> const ast::func_decl *;

  auto receiver_fills_first_param(const method_entry &method,
                                  const type_entry &instance) -> bool;

  auto check_receiver_call(const ast::call_expr &call,
                           const method_entry &method,
                           std::string_view target_type_name,
                           const ast::expr &receiver, type_id receiver_type,
                           const explicit_generic_args &explicit_args = {})
      -> type_id;

  auto method_explicit_generic_args(const ast::field_expr &field)
      -> explicit_generic_args;

  auto check_method_accepts_generic_args(
      const ast::field_expr &field, const ast::func_decl &decl,
      const explicit_generic_args &explicit_args) -> bool;

  // ==========================================================================
  //  UFCS — free functions called with method syntax
  //  (`spec/collections-algorithms-design.md` §3)
  //
  //  `recv.f(a...)` falls back to `f(recv, a...)` when no method `f` exists.
  //  This is the *last* arm of `infer_method_call`'s lookup ladder, always,
  //  which is the property that makes it safe to ship: a free function can
  //  never shadow a method, so adding one to the standard library can never
  //  silently change the meaning of code that already compiles.
  //
  //  Nothing new is needed downstream. Lowering already emits a receiver as
  //  a prepended first argument for any `resolved_callee` carrying one (see
  //  `resolved_callee::receiver`), and an empty `impl_target_type` already
  //  means "call this by its bare name" — which is exactly a free function.
  //  So a UFCS call lowers as the plain call it desugars to, and both
  //  backends never learn the feature exists.
  // ==========================================================================

  /// One free function that `recv.f(...)` might mean.
  /// Where a UFCS candidate was found, lowest number = highest precedence.
  /// Mirrors the order `infer_call` resolves a bare `name(...)` in, which is
  /// what makes `x.f()` and `f(x)` agree on which `f` they mean.
  enum class ufcs_origin : uint8_t {
    this_module = 0,     ///< Declared right here.
    explicit_import = 1, ///< Named by a `use` of its own.
    wildcard_import = 2, ///< Reached through a `use pkg.*`.
    prelude = 3,         ///< Implicitly in scope everywhere.
  };

  struct ufcs_candidate {
    const ast::func_decl *decl = nullptr;
    const module_members *owner = nullptr;
    file_id_type file_id = 0;
    ufcs_origin origin = ufcs_origin::this_module;
  };

  // ==========================================================================
  //  check_ufcs.cpp
  // ==========================================================================

  [[nodiscard]] auto is_ufcs_eligible(const ast::func_decl &decl,
                                      bool same_module) -> bool;

  [[nodiscard]] auto collect_ufcs_candidates(const std::string &name)
      -> std::vector<ufcs_candidate>;

  /// How a receiver of type `receiver` sits against a first parameter
  /// declared `param` (§3.2).
  enum class receiver_fit : uint8_t {
    fits,         ///< Callable as written.
    needs_mut,    ///< Shape matches, but `&mut T` wants a mutable receiver.
    does_not_fit, ///< Different type entirely; not a candidate.
  };

  [[nodiscard]] auto receiver_is_mutable_lvalue(const ast::expr &receiver,
                                                type_id receiver_type) -> bool;

  [[nodiscard]] auto classify_receiver_fit(type_id param, type_id receiver,
                                           bool receiver_is_mutable)
      -> receiver_fit;

  [[nodiscard]] auto describe_ufcs_candidate(const ufcs_candidate &candidate)
      -> std::string;

  [[nodiscard]] static auto ufcs_candidate_path(const ufcs_candidate &candidate)
      -> std::string;

  auto check_ufcs_call(const ast::call_expr &call, const ast::field_expr &field,
                       const ufcs_candidate &candidate, type_id receiver_type)
      -> type_id;

  auto pin_literal_receiver_by_ufcs(const ast::field_expr &field,
                                    type_id receiver) -> bool;

  auto try_ufcs_call(const ast::call_expr &call, const ast::field_expr &field,
                     type_id receiver_type) -> std::optional<type_id>;

  auto infer_method_call(const ast::call_expr &call,
                         const ast::field_expr &field) -> type_id;

  [[nodiscard]] auto is_literal_tree(const ast::expr &expr, type_id target)
      -> bool;

  auto check_conversion_call(const ast::call_expr &call,
                             std::string_view target_name) -> type_id;

  auto resolve_applied_type_receiver(const ast::expr &object)
      -> std::optional<type_id>;

  auto resolve_type_param_static(const ast::call_expr &call,
                                 const ast::field_expr &field, type_id target,
                                 const std::string &fn_name)
      -> std::optional<type_id>;

  auto report_missing_type_param_static(const ast::field_expr &field,
                                        const std::string &param_name,
                                        type_id target,
                                        const std::string &fn_name) -> void;

  auto check_bare_generic_static_call(const ast::call_expr &call,
                                      const ast::field_expr &field,
                                      const method_entry &method,
                                      const std::string &type_name)
      -> std::optional<type_id>;

  auto infer_qualified_call(const ast::call_expr &call,
                            const ast::field_expr &field)
      -> std::optional<type_id>;

  /// Types a call expression by dispatching on its callee's shape: a method
  /// call (field-expr callee) goes to `infer_method_call`; a computed
  /// callable value is checked against its `fn(...)` type; a variant-ident
  /// callee constructs that variant; otherwise the identifier is resolved in
  /// order through local bindings, the current module's functions, imports,
  /// a real declaration in the auto-imported prelude (`find_prelude_function`
  /// — `println`/`print`/`eprintln`/`eprint`), a handful of remaining
  /// prelude forms with their own hard-coded signature (`panic`, `assert`,
  /// ...), a builtin conversion call, a bare variant spelling, or an
  /// undefined-name error.
  /// A function reachable from the current module by bare name, together with
  /// where it came from — the module that owns it (its own or the one an
  /// import points at) and the file it was declared in.
  struct callable_lookup {
    const ast::func_decl *decl = nullptr;
    const module_members *owner = nullptr;
    file_id_type file_id{};
  };

  auto find_callable_decl(std::string_view name)
      -> std::optional<callable_lookup>;

  auto infer_explicit_generic_call(const ast::call_expr &call)
      -> std::optional<type_id>;

  auto infer_layout_query_call(const ast::call_expr &call)
      -> std::optional<type_id>;

  auto require_machine_context(source_span span, std::string_view operation)
      -> bool;

  auto infer_uninit_call(const ast::call_expr &call) -> std::optional<type_id>;

  auto infer_ptr_cast_call(const ast::call_expr &call)
      -> std::optional<type_id>;

  auto infer_slice_from_raw_parts_call(const ast::call_expr &call)
      -> std::optional<type_id>;

  auto infer_comptime_generic_call(const ast::call_expr &call,
                                   const ast::index_expr &index)
      -> std::optional<type_id>;

  auto infer_implements_call(const ast::call_expr &call)
      -> std::optional<type_id>;

  auto infer_call(const ast::call_expr &call, type_id expected) -> type_id;

  [[nodiscard]] static auto
  called_name_for_diagnostic(const ast::call_expr &call) -> std::string;

  auto infer_call_unguarded(const ast::call_expr &call, type_id expected)
      -> type_id;

  // ==========================================================================
  //  check_expr.cpp
  // ==========================================================================

  auto infer_type_name_member(const ast::field_expr &field)
      -> std::optional<type_id>;

  auto struct_field_names(const type_entry &instance) -> std::string;

  // ==========================================================================
  //  check_methods.cpp
  // ==========================================================================

  auto type_has_member(type_id type, std::string_view name) -> bool;

  auto deref_method_of(type_id type) -> const method_entry *;

  auto resolve_deref_step(const ast::expr &receiver, type_id handle,
                          source_span span) -> std::optional<deref_step>;

  auto deref_chain(const ast::expr &receiver, type_id object,
                   std::string_view name, source_span span)
      -> std::optional<std::pair<std::vector<deref_step>, type_id>>;

  auto record_deref_steps(const deref_site &site, std::vector<deref_step> steps)
      -> void;

  [[nodiscard]] static auto deref_site_receiver(const deref_site &site)
      -> const ast::expr &;

  auto lookup_through_deref(const deref_site &site, type_id object,
                            std::string_view name, source_span span)
      -> std::optional<type_id>;

  auto defer_deref_steps(const deref_site &site, type_id object,
                         std::string name, source_span span) -> void;

  auto deref_operator_target(const ast::expr &operand, type_id handle,
                             source_span span) -> std::optional<type_id>;

  [[nodiscard]] static auto cannot_be_dereferenced(const type_entry &entry)
      -> bool;

  auto report_deref_of_non_reference(const ast::unary_expr &unary, type_id type)
      -> void;

  auto deref_search_note(type_id type) -> std::optional<std::string>;

  auto place_through_deref(const ast::expr &target) -> std::optional<type_id>;

  auto report_write_through_deref(source_span span, type_id handle,
                                  std::string_view what) -> void;

  auto method_callable_through_deref(const ast::call_expr &call,
                                     const ast::field_expr &field,
                                     const method_entry &method, type_id handle)
      -> bool;

  auto field_info_type() -> type_id;

  auto field_info_member_type(std::string_view name, source_span span)
      -> type_id;

  auto static_for_element_type(const ast::expr &iterable) -> type_id;

  auto field_access_type(type_id object, std::string_view name,
                         source_span span, const deref_site &site = {})
      -> type_id;

  [[nodiscard]] auto
  report_unknown_builtin_field(std::string_view name, source_span span,
                               const type_entry &entry, type_id object)
      -> std::optional<type_id>;

  auto infer_field(const ast::field_expr &field) -> type_id;

  auto classify_dotted_root(std::string_view root, bool root_is_local)
      -> dotted_root;

  auto dotted_root_is_value(std::string_view root) -> bool;

  auto infer_module_path(const ast::module_path_expr &path) -> type_id;

  /// The `static` a module-rooted path names, and where its fields start:
  /// `geo.origin` (first field past the end) or `geo.origin.y` (first field
  /// `y`) — the longest prefix naming a `static` wins.
  struct module_static_prefix {
    const ast::static_decl *decl = nullptr;
    const module_members *owner = nullptr;
    size_t first_field = 0;
  };

  [[nodiscard]] auto
  find_module_static_prefix(const std::vector<std::string> &segments) const
      -> std::optional<module_static_prefix>;

  auto infer_module_static_projection(const ast::module_path_expr &path)
      -> type_id;

  /// Runs `body` with `owner` (declared in `file`) as the module being
  /// checked — module, file, and imports — so the names it reads resolve
  /// the way they do at the declaration. Compile-time code crosses modules
  /// this way: a `static` initializer reached from another module, and a
  /// `static def` body called from one.
  template <typename F>
  auto in_module_context(const module_members &owner, file_id_type file,
                         F &&body) -> decltype(body()) {
    if (&owner == file_.module_ && file == file_.file_id_) {
      return std::forward<F>(body)();
    }
    const auto saved_module = file_.module_;
    const auto saved_module_name = file_.module_name_;
    const auto saved_functor_owner = session_.functor_owner_module_;
    const auto saved_file = file_.file_id_;
    file_.module_ = &owner;
    file_.module_name_ = owner.module_name;
    session_.functor_owner_module_.clear();
    file_.file_id_ = file;
    comptime_eval_.set_file(file_.file_id_);
    auto result = std::forward<F>(body)();
    file_.module_ = saved_module;
    file_.module_name_ = saved_module_name;
    session_.functor_owner_module_ = saved_functor_owner;
    file_.file_id_ = saved_file;
    comptime_eval_.set_file(file_.file_id_);
    return result;
  }

  auto function_owner(const ast::func_decl &fn)
      -> std::optional<std::pair<const module_members *, file_id_type>>;

  auto comptime_static_value(const ast::static_decl &decl,
                             const module_members *owner, std::string cycle)
      -> std::expected<comptime::value, std::string>;

  auto find_comptime_method(const ast::expr &receiver_expr,
                            const comptime::value &receiver,
                            const std::string &name)
      -> std::expected<const ast::func_decl *, std::string>;

  auto comptime_call_type_args(const ast::call_expr &call)
      -> std::optional<std::vector<std::pair<std::string, comptime::value>>>;

  auto substitute_comptime_type_params(type_id type) -> type_id;

  auto read_bare_name_for_comptime(const std::string &name)
      -> comptime::evaluator::bare_name_reading;

  auto read_dotted_path_for_comptime(const ast::module_path_expr &path,
                                     bool root_is_local)
      -> comptime::evaluator::dotted_path_reading;

  auto infer_value_rooted_path(const ast::module_path_expr &path) -> type_id;

  auto fold_static_field_path(const ast::module_path_expr &path,
                              const ast::static_decl &decl,
                              const module_members *owner, size_t first_field,
                              type_id type) -> void;

  [[nodiscard]] auto visibility_module() const -> const std::string &;

  auto validate_module_path(const ast::module_path_expr &path) -> bool;

  auto ident_names_callable_decl(const ast::ident_expr &ident) -> bool;

  auto check_index_in_bounds(const ast::index_expr &index, type_id object,
                             type_id key) -> void;

  // ==========================================================================
  //  check_reasoning.cpp
  // ==========================================================================

  auto try_from_target(const ast::call_expr &call) -> std::optional<type_id>;

  auto infer_refinement_try_from(const ast::call_expr &call)
      -> std::optional<type_id>;

  auto build_try_from_fragment(const ast::call_expr &call,
                               const ast::expr &value, const type_entry &entry,
                               type_id base, type_id refined)
      -> const ast::node *;

  auto resolve_index_output(type_id target, std::string_view trait_name,
                            const method_entry &method) -> type_id;

  auto resolve_index_key_type(type_id target, std::string_view trait_name,
                              const method_entry &method) -> type_id;

  auto recorded_key_type(const ast::index_expr &index) -> type_id;

  auto index_instance_discriminator(type_id target, std::string_view trait_name,
                                    const method_entry &method) -> std::string;

  auto index_key_filter(type_id target, std::string_view trait_name,
                        type_id key) -> method_filter;

  auto check_index_key(const ast::index_expr &index, type_id target,
                       std::string_view trait_name, const method_entry &method,
                       type_id key) -> bool;

  auto dispatch_index(
      std::unordered_map<const ast::index_expr *, resolved_callee> &dispatches,
      const ast::index_expr &index, const method_entry &method, type_id target,
      std::string_view trait) -> void;

  auto dispatch_index_when_settled(
      std::unordered_map<const ast::index_expr *, resolved_callee> &dispatches,
      const ast::index_expr &index, const method_entry &method, type_id target,
      std::string_view trait) -> void;

  auto require_index_trait(const ast::index_expr &index, type_id object,
                           const type_entry &entry, type_id key) -> type_id;

  auto require_index_ref_trait(const ast::index_expr &index, type_id object,
                               const type_entry &entry, type_id key) -> type_id;

  auto resolve_index_mut_output(type_id target, std::string_view trait_key,
                                const method_entry &method) -> type_id;

  auto require_index_mut_trait(const ast::index_expr &index, type_id object,
                               const type_entry &entry, type_id key) -> type_id;

  auto resolve_impl_param_types(type_id target, std::string_view trait_name,
                                const method_entry &method)
      -> std::vector<fn_param_info>;

  auto require_index_set_trait(const ast::index_expr &index, type_id object,
                               const type_entry &entry, type_id key) -> type_id;

  auto expected_index_key(const ast::index_expr &index) -> type_id;

  auto infer_index(const ast::index_expr &index) -> type_id;

  // ==========================================================================
  //  check_expr.cpp
  // ==========================================================================

  auto find_type_decl_by_path(const std::vector<std::string> &path)
      -> std::optional<std::pair<const ast::type_decl *, std::string>>;

  auto find_type_decl_by_name(std::string_view name)
      -> std::optional<std::pair<const ast::type_decl *, std::string>>;

  auto find_trait_decl_by_name(std::string_view name)
      -> std::optional<const ast::trait_decl *>;

  auto resolve_struct_literal_head(const ast::type_decl &decl,
                                   std::string_view owner_module,
                                   const std::vector<ast::type_arg> &type_args,
                                   source_span head_span) -> type_id;

  auto check_struct_literal(const ast::struct_expr &expr, type_id expected)
      -> type_id;

  auto check_body_nodes(const std::vector<ast::ptr<ast::node>> &items,
                        type_id expected_tail) -> type_id;

  static auto capture_allow_set(const ast::lambda_expr &lambda)
      -> std::optional<std::unordered_map<std::string, ast::capture_mode>>;

  auto is_module_level_value(std::string_view name) -> bool;

  auto warn_unused_captures(const ast::lambda_expr &lambda,
                            const capture_barrier &barrier) -> void;

  auto check_capture_list(const ast::lambda_expr &lambda)
      -> std::unordered_set<std::string>;

  auto infer_lambda(const ast::lambda_expr &lambda, type_id expected)
      -> type_id;

  auto check_if_branch_header(const ast::if_branch &branch) -> void;

  auto join_branch_type(type_id current, type_id found, source_span span,
                        std::string_view construct) -> type_id;

  auto infer_if_expr(const ast::if_expr &expr, type_id expected) -> type_id;

  auto resolve_loop_iterable(const ast::expr &iterable_expr,
                             const ast::node &site, iterator_loop_dispatch &out)
      -> type_id;

  auto defer_loop_route(const ast::expr &iterable_expr, const ast::node &site,
                        type_id iterable) -> void;

  auto resolve_loop_route(const ast::expr &iterable_expr, type_id iterable,
                          const ast::node *site, iterator_loop_dispatch &out)
      -> type_id;

  auto infer_for_expr(const ast::for_expr &expr) -> type_id;

  auto maybe_wire_try_conversion(const ast::try_expr &expr, type_id operand_err,
                                 type_id fn_err, type_id fn_return_type)
      -> void;

  auto infer_try(const ast::try_expr &expr) -> type_id;

  auto infer_await(const ast::await_expr &expr) -> type_id;

  auto infer_yield(const ast::yield_expr &expr) -> type_id;

  auto try_resolve_iter_borrow(type_id operand, bool is_mut,
                               const ast::node *site)
      -> std::optional<iterator_loop_dispatch>;

  auto try_resolve_into_iterator(type_id iterable, const ast::node *site)
      -> std::optional<iterator_loop_dispatch>;

  auto try_resolve_iterator(type_id iterable, const ast::node *site = nullptr)
      -> std::optional<iterator_loop_dispatch>;

  auto element_type_of(type_id iterable, source_span span) -> type_id;

  auto record_expr_type(const ast::node &node, type_id type) -> type_id;

  auto infer_expr(const ast::expr &expr, type_id expected) -> type_id;

  auto note_static_for_loop_read(const ast::expr &expr) -> void;

  [[nodiscard]] static auto static_for_read_root(const ast::expr &expr)
      -> const std::string *;

  auto infer_expr_impl(const ast::expr &expr, type_id expected) -> type_id;

  auto finish_interp_segment(const ast::interp_segment &seg, type_id value_type,
                             const ast::expr *value_key = nullptr) -> void;

  /// An interpolation segment whose value type was still an open call-result
  /// leaf (spec/todo.md item 19: an implicit-/explicit-generic call with an
  /// omitted return type, used directly inside a `"{...}"` segment) when
  /// `check_interpolated_string` first walked it. `demand` only flushes the
  /// integer-literal queue, not `pending_open_results_`, so such a leaf comes
  /// back unresolved; it is only pinned once `finish_open_results` runs.
  struct pending_interp_segment {
    const ast::interp_segment *seg = nullptr;
    type_id leaf = k_unknown_type;
  };

  std::vector<pending_interp_segment> pending_interp_segments_;

  auto finish_pending_interp_segments() -> void;

  auto reject_interpolated_reference(const ast::expr &value, type_id ref_type)
      -> void;

  auto check_interpolated_string(const ast::interpolated_string_expr &node)
      -> type_id;

  auto fill_count_of(const ast::array_expr &array) -> std::optional<uint64_t>;

  auto instantiate_from_array_for(const ast::array_expr &array, type_id target,
                                  uint64_t length) -> void;

  auto resolve_array_literal_conversion(const ast::array_expr &array,
                                        type_id &target)
      -> std::optional<type_id>;

  auto fill_target_holes(const ast::array_expr &array,
                         const method_entry &method, param_subst &bindings,
                         type_id &target) -> void;

  auto try_wire_from_array(const ast::array_expr &array, type_id expected,
                           type_id &target) -> std::optional<type_id>;

  auto resolve_list_type(type_id element) -> type_id;

  auto resolve_range_type(type_id element) -> type_id;

  auto range_element_type(type_id type) -> std::optional<type_id>;

  auto is_range_type(type_id type) -> bool;

  auto resolve_comprehension_dispatch(const ast::for_expr &expr,
                                      type_id list_type) -> void;

  /// `resolve_new_push_dispatch` into `table[&site]` — now, or, for a list
  /// whose element is still a leaf (`for x in 0..5 => x * x` before anything
  /// has said what `x` is), once it settles: the instances are named after
  /// the element, and `list::new$list___` is a function nothing compiles.
  template <typename Site>
  auto record_new_push_dispatch(
      const Site &site, type_id list_type,
      std::unordered_map<const Site *, comprehension_dispatch> &table) -> void {
    if (!mentions_type_var(list_type)) {
      if (const auto dispatch = resolve_new_push_dispatch(site, list_type)) {
        table[&site] = *dispatch;
      }
      if (mentions_template_param(list_type)) {
        defer_to_instances(
            site,
            [this, &site, &table, list_type](instance_subst &subst) -> auto {
              const auto &clone = *clone_of(subst, &site);
              if (const auto dispatch = resolve_new_push_dispatch(
                      clone, substitute_type(list_type, subst))) {
                table[&clone] = *dispatch;
              }
            });
      }
      return;
    }
    defer_method_call("`new`/`push` for this list", list_type,
                      std::vector<type_id>{},
                      [this, &site, &table](type_id settled) -> void {
                        record_new_push_dispatch(site, settled, table);
                      });
  }

  auto resolve_new_push_dispatch(const ast::node &site, type_id list_type)
      -> std::optional<comprehension_dispatch>;

  auto wire_default_list(const ast::array_expr &array, type_id element,
                         std::optional<uint64_t> count) -> type_id;

  auto check_fill_count(const ast::expr &count) -> void;

  auto defer_open_literal(const ast::array_expr &array, type_id element,
                          std::optional<uint64_t> count) -> bool;

  auto infer_array(const ast::array_expr &array, type_id expected) -> type_id;

  // ==========================================================================
  //  check_patterns.cpp
  // ==========================================================================

  auto
  check_loop_head_patterns(const std::vector<const ast::pattern *> &patterns,
                           type_id raw_element) -> void;

  auto check_pattern(const ast::pattern &pattern, type_id raw_subject) -> void;

  auto check_constructor_pattern(const ast::constructor_pattern &ctor,
                                 type_id subject) -> void;

  auto pattern_is_irrefutable(const ast::pattern &pattern) -> bool;

  auto collect_covered_names(const ast::pattern &pattern,
                             std::unordered_set<std::string> &covered) -> void;

  auto check_match_exhaustiveness(type_id subject, source_span span,
                                  const std::vector<ast::match_arm> &arms)
      -> void;

  auto check_match(const ast::expr *subject,
                   const std::vector<ast::match_arm> &arms, type_id expected,
                   source_span span) -> type_id;

  // ==========================================================================
  //  check_stmt.cpp
  // ==========================================================================

  auto binding_origin_description(binding_origin origin) -> std::string_view;

  auto assignment_root_ident(const ast::expr &target)
      -> const ast::ident_expr *;

  auto check_user_index_assignment(const ast::expr &target)
      -> std::optional<type_id>;

  auto check_assignment(const ast::assign_stmt &stmt) -> void;

  auto check_body_node(const ast::node &node, type_id expected_tail) -> type_id;

  // ==========================================================================
  //  check_decls.cpp
  // ==========================================================================

  auto check_intrinsic_decl(const ast::func_decl &decl, bool at_module_scope)
      -> void;

  static auto
  block_provides_function_value(const std::vector<ast::ptr<ast::node>> &stmts)
      -> bool;

  static auto is_while_true(const ast::node &node) -> bool;

  static auto node_provides_function_value(const ast::node &node) -> bool;

  auto check_function(const ast::func_decl &decl, bool at_module_scope) -> void;

  /// Diagnostics held while probing a function with unannotated parameters.
  struct param_probe {
    const ast::func_decl *decl = nullptr;
    std::vector<diagnostic> diags;
    file_id_type file = 0;
  };

  std::vector<diagnostic> *probe_capture_ = nullptr;

  /// The function being probed, while its body is checked.
  const ast::func_decl *probing_decl_ = nullptr;

  std::vector<param_probe> param_probes_;

  auto finish_param_probes() -> void;

  auto check_function_impl(const ast::func_decl &decl, bool at_module_scope)
      -> void;

  auto check_type_decl(const ast::type_decl &decl) -> void;

  // ==========================================================================
  //  check_methods.cpp
  // ==========================================================================

  auto find_trait_anywhere(std::string_view name) -> std::optional<
      std::pair<const ast::trait_decl *, const module_members *>>;

  // ==========================================================================
  //  check_reasoning.cpp
  // ==========================================================================

  auto display_fact(const type_fact &fact) -> std::string;

  // ==========================================================================
  //  check_bounds.cpp
  // ==========================================================================

  auto check_bound_method_call(const ast::call_expr &call,
                               const ast::field_expr &field, type_id receiver,
                               const explicit_generic_args &explicit_args,
                               bool static_call = false) -> type_id;

  auto find_method_on(type_id target, std::string_view name)
      -> const method_entry *;

  auto
  solve_values_from_argument_types(const ast::call_expr &call,
                                   const std::vector<fn_param_info> &params,
                                   value_bindings &solved) -> void;

  auto replay_open_method_call(const ast::call_expr &call,
                               const method_entry &method,
                               std::string_view receiver_name,
                               const ast::expr &receiver, type_id target)
      -> void;

  auto replay_method_call(const ast::call_expr &call,
                          const ast::field_expr &field, type_id target,
                          bool static_call,
                          const explicit_generic_args &explicit_args = {})
      -> void;

  auto report_unbound_method(const ast::field_expr &field,
                             const std::string &receiver_name,
                             const std::vector<type_fact> &facts,
                             bool static_call = false) -> void;

  auto facts_note(const std::string &subject_name,
                  const std::vector<type_fact> &facts) -> std::string;

  auto require_bound_operator(source_span span, type_id subject,
                              std::string_view trait_name,
                              std::string_view op_name)
      -> std::optional<type_fact>;

  auto bound_assoc_type(const type_fact &fact, type_id subject,
                        std::string_view name) -> type_id;

  auto check_bound_arithmetic(const ast::binary_expr &binary, type_id lhs,
                              type_id rhs) -> type_id;

  // ==========================================================================
  //  check_methods.cpp
  // ==========================================================================

  auto trait_declares_method(const ast::trait_decl &trait,
                             std::string_view name) -> bool;

  // ==========================================================================
  //  check_type_resolution.cpp
  // ==========================================================================

  auto existential_bound_names(const type_entry &entry) -> std::string;

  // ==========================================================================
  //  check_decls.cpp
  // ==========================================================================

  auto check_impl_decl(const ast::impl_decl &decl) -> void;

  auto check_extend_decl(const ast::extend_decl &decl) -> void;

  static auto passing_mode(const ast::type_expr *type) -> std::string_view;

  auto check_impl_signatures(const ast::impl_decl &impl,
                             const ast::trait_decl &trait,
                             const module_members *trait_owner,
                             std::string_view trait_name) -> void;

  auto check_impl_members(const ast::impl_decl &impl,
                          const ast::trait_decl &trait,
                          std::string_view trait_name, const type_entry &target)
      -> void;

  auto check_trait_decl(const ast::trait_decl &decl) -> void;

  auto check_concept_decl(const ast::concept_decl &decl) -> void;

  auto check_signature_decl(const ast::signature_decl &decl) -> void;

  [[nodiscard]] auto
  resolve_bound_signature(const ast::type_expr &bound_expr) const
      -> const ast::signature_decl *;

  /// A resolved functor: its declaration/file and the module that owns it.
  struct resolved_functor {
    const functor_decl_ref *ref = nullptr;
    std::string owner_module; ///< Qualified name of the owning module.
  };

  [[nodiscard]] auto resolve_functor(const std::vector<std::string> &path) const
      -> std::optional<resolved_functor>;

  [[nodiscard]] auto resolve_arg_module(const ast::named_type &nt) const
      -> const module_members *;

  [[nodiscard]] auto types_satisfy(type_id want, type_id got) const -> bool;

  [[nodiscard]] auto
  module_satisfies_signature(const module_members &mod,
                             const ast::signature_decl &sig,
                             std::vector<std::string> &failures) -> bool;

  auto check_functor_instantiation(const ast::use_decl &decl) -> void;

  [[nodiscard]] static auto functor_alias_name(const ast::use_decl &decl,
                                               const std::string &functor_name)
      -> std::string;

  [[nodiscard]] static auto sanitize_module_name(std::string_view key)
      -> std::string;

  [[nodiscard]] static auto impl_target_name(const ast::type_expr *for_type)
      -> std::string;

  [[nodiscard]] auto
  materialize_functor(const ast::sub_module_decl &fdecl,
                      const resolved_functor &functor,
                      const std::vector<functor_arg_binding> &module_bindings,
                      const std::string &instantiation_key,
                      source_span use_span) -> std::optional<std::string>;

  auto check_pending_functor_bodies() -> void;

  // ==========================================================================
  //  check_diagnostics.cpp
  // ==========================================================================

  auto mark_file_error() -> void;

  // ==========================================================================
  //  check_patterns.cpp
  // ==========================================================================

  auto resolve_variant_tag(const ast::node &node)
      -> std::optional<std::pair<std::string, std::string>>;

  // ==========================================================================
  //  check_comptime.cpp
  // ==========================================================================

  auto inside_unbound_generic_scope() -> bool;

  /// The scope a loop over a compile-time list binds: each name the loop
  /// pattern binds, with its value.
  using static_for_scope = std::unordered_map<std::string, comptime::value>;

  auto evaluate_static_for_inline(const ast::static_decl &decl)
      -> std::optional<std::vector<static_for_scope>>;

  auto list_element_type(type_id type) -> type_id;

  auto comptime_values_type(const std::vector<comptime::value> &values)
      -> type_id;

  auto record_static_for_inline(const ast::static_decl &decl,
                                type_id expected_tail) -> type_id;

  /// The inline `static for` being unrolled: `decl` is the one lowering
  /// reads (an instance's clone in a template), `written` the one checked,
  /// which the records made while checking it are keyed by. `clone` and
  /// `substitute` take a node or a type of `written` to `decl`'s.
  struct static_for_unrolling {
    const ast::static_decl &decl;
    const ast::static_decl &written;
    std::function<const ast::node *(const ast::node *)> clone;
    std::function<type_id(type_id)> substitute;
  };

  auto static_for_node_type(const static_for_unrolling &unrolling,
                            const ast::node *node) -> type_id;

  auto materialize_static_value(const comptime::value &value, source_span span,
                                type_id type) -> const ast::expr *;

  auto unroll_static_for_inline(const static_for_unrolling &unrolling,
                                const std::vector<static_for_scope> &iterations,
                                type_id yield_type, type_id expected_tail)
      -> type_id;

  auto fold_static_for_reads(
      const static_for_unrolling &unrolling, const static_for_reads &reads,
      const static_for_scope &scope,
      const std::unordered_set<std::string> &compile_time_only,
      static_for_iteration &iteration) -> bool;

  auto materialize_string_literal(const std::string &text, source_span span,
                                  type_id type) -> const ast::literal_expr *;

  auto defer_static_if(const ast::static_decl &decl) -> void;

  auto resolve_static_if_branch(const ast::static_decl &decl)
      -> std::optional<bool>;

  auto stmt_definitely_returns(const ast::node &node) -> bool;

  auto check_narrowed_static_if(const ast::static_decl &decl,
                                type_id expected_tail)
      -> std::optional<std::pair<type_id, type_id>>;

  auto narrowing_past(const ast::node &node) -> std::optional<narrowing>;

  auto check_static_decl(const ast::static_decl &decl) -> void;

  // ==========================================================================
  //  check_decls.cpp
  // ==========================================================================

  auto check_item(const ast::node &item, bool at_module_scope) -> void;

  auto validate_impl_coherence() -> void;

  // ==========================================================================
  //  check.cpp
  // ==========================================================================

  auto compute_external_wildcard() -> void;

  auto check_file(const ast::file &file) -> void;

  // ==========================================================================
  //  check_comptime.cpp
  // ==========================================================================

  auto register_comptime_globals(const std::vector<ast::ptr<ast::node>> &items)
      -> void;

  // ==========================================================================
  //  check.cpp
  // ==========================================================================

  [[nodiscard]] auto names_reflectable_module(std::string_view name) const
      -> bool;

  // ==========================================================================
  //  check_comptime.cpp
  // ==========================================================================

  auto register_comptime_modules() -> void;

  auto register_comptime_coherence_info() -> void;

  /// Recursively resolves every item-level splice (`~expr` used directly
  /// among `items`, as opposed to inside a function body) into an injected
  /// `impl` block, descending into inline submodules the same way
  /// `register_comptime_globals` does. Must run after that pass (so
  /// `static def`/`static let` globals it references are registered) but
  /// before `build_method_table`/`validate_impl_coherence` — see `index_`'s
  /// doc comment for why running here, rather than during ordinary
  /// per-file checking, is what makes the injected impl's methods visible
  /// to coherence checking and to every other file's method lookups
  /// regardless of file-checking order.
  ///
  /// Only a `def_expr` fragment wrapping a single `impl_decl` is supported
  /// — item-level splice producing a `type`/`trait`/`def` is not
  /// implemented yet, and reports a clear diagnostic rather than silently
  /// doing nothing. Evaluates each splice's operand exactly once: the
  /// result (found or not) is recorded in `session_.item_splice_impls_` so
  /// `check_file`'s item loop never falls through to re-evaluating it via
  /// `check_body_node`.
  /// Every `deriving`-able trait with a real `static def derive_<name>[T]()`
  /// in `std.derive` (`src/std/deriving.cn`) — the traits that are
  /// re-derived for real, as opposed to left on the old, type-only
  /// `derived_method_result` path. That list is now all five: `hash` was the
  /// last holdout, on the theory that no builtin scalar implemented
  /// `.hash()` and that there was no hash-combining primitive to fold field
  /// hashes with. The second half was simply untrue — `*%`/`+%` (wrapping
  /// arithmetic, `ast::binary_op::mul_wrap`) are implemented in both
  /// backends, and they are exactly what an FNV-1a fold needs, since plain
  /// `*`/`+` are overflow-checked and would panic on the second field. With
  /// those, `std.traits` can carry real `impl hash` blocks for the scalars
  /// and the `hash_seed`/`hash_combine`/`hash_value`/`hash_tag` combinators,
  /// all in ordinary Cinder. Unlike `ord`, `hash` has no operator dispatching
  /// to it, so those scalar impls carry none of the infinite recursion that
  /// keeps `impl ord for int32` from existing (see `ord_cmp`).
  ///
  /// `derived_method_result` still lists all five names, for the
  /// `type_has_trait` bookkeeping every `deriving`d trait needs and as the
  /// fallback for shapes this path declines.
  static constexpr std::array<std::string_view, 5> k_real_derive_traits = {
      "show", "eq", "debug", "ord", "hash"};

  // ==========================================================================
  //  check.cpp
  // ==========================================================================

  [[nodiscard]] auto impl_report_span(const ast::impl_decl &impl) const
      -> source_span;

  [[nodiscard]] auto evaluate_derive_call(const ast::type_decl &decl,
                                          std::string_view trait_name,
                                          type_id instance_id)
      -> const ast::impl_decl *;

  [[nodiscard]] static auto derivable_shape(const ast::type_decl &decl) -> bool;

  [[nodiscard]] static auto derive_trait_for_method(std::string_view name)
      -> std::string_view;

  auto ensure_derived_instance_impls(type_id instance_id,
                                     std::string_view method_name) -> bool;

  auto resolve_deriving_traits(const ast::type_decl &decl,
                               file_id_type owner_file) -> void;

  auto
  discover_functor_instantiations(const std::vector<ast::ptr<ast::node>> &items)
      -> void;

  auto resolve_item_splices(const std::vector<ast::ptr<ast::node>> &items,
                            file_id_type owner_file) -> void;

public:
  auto settle_bounded_params(const std::vector<ast::ptr<ast::node>> &items)
      -> void;

  auto run_impl(const std::vector<parsed_module> &inputs) -> void;
};

} // namespace cinder::semantic::detail
