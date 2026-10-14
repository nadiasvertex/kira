#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/parser/ast.h"
#include "src/semantic/type_table.h"

namespace cinder::semantic {

// ==========================================================================
//  Checker results — what the checker hands to later stages.
// ==========================================================================

/// The persisted result of type-checking one session: the interned
/// `type_table` every `type_id` below indexes into, plus the resolved type
/// of every expression node the checker actually visited. Several
/// non-expression declaration nodes are recorded in `node_types` too, since
/// a later pass needs their types but they never pass through `infer_expr`:
/// a function parameter's `pattern` node (keyed by `param.pattern.get()`),
/// a function's declared return-type node (keyed by
/// `decl.return_type.get()`), and every `ast::pattern` node checked via
/// `check_pattern` (keyed by the pattern node itself, recording the type of
/// the value it matches) — all recorded in `check_function`/`check_pattern`.
/// Two shorthand shapes have no node to key against, for the same reason:
/// a struct pattern's shorthand field (`{x}`, matching
/// `ast::field_pattern.pattern == nullptr`) binds a name straight from a
/// plain struct field with no dedicated sub-pattern node, and a struct
/// *literal*'s shorthand field (`{x}`, matching
/// `ast::struct_field_init.value == nullptr`) reads an in-scope value the
/// same way with no dedicated value node — so each is recorded separately,
/// in `struct_pattern_field_types` (keyed by the owning `ast::field_pattern`)
/// and `struct_literal_field_types` (keyed by the owning
/// `ast::struct_field_init`) respectively; neither key type is itself an
/// `ast::node`, but both are stable for the AST's lifetime. A node the
/// checker never reached (inside a file already marked failing, or simply
/// never checked) has no entry in any of these maps — look it up with
/// `.find`, not `.at`. This is what a later typed-lowering pass
/// (`spec/typed-ir-design.md`) reads instead of re-deriving types from the
/// AST a second time.
/// Per-call-site argument-to-parameter mapping for a call resolved against
/// a real `ast::func_decl` (a free function, a method, or a trait default
/// body) via `check_call_against_decl` — the only call form that supports
/// named arguments and defaults, since a bare `fn(...)`-typed callee has no
/// parameter names to resolve a named argument against.
/// `args_by_param[i]` is the AST expression supplying the i-th declared
/// parameter (in the same order `signature_params` produces, i.e. with
/// `self` already skipped for a method call) — whichever call argument
/// that turned out to be, positional or named; null means the argument was
/// omitted and the parameter's default value applies.
///
/// `defaults_by_param[i]` is that parameter's declared default-value
/// expression (null when it has none), and `param_names[i]` its spelling.
/// Both are copied from the callee's signature at the point the call is
/// checked because lowering has no route back to the declaration a call
/// resolved against for every call form — and an omitted argument is lowered
/// by lowering the callee's default expression right here at the call site
/// (see `lower_call`), which needs exactly these two pieces.
///
/// `passing_by_param[i]` is how that parameter receives its argument — by
/// value, or through a `&`/`&mut` reference (a bare place passed there is
/// implicitly borrowed). Recorded here, once, from the parameter's declared
/// type, so the ownership checker never re-derives a call's passing modes
/// from the callee's syntax.
enum class param_passing : std::uint8_t {
  by_value,   ///< The argument is moved (or copied) into the callee.
  shared_ref, ///< `&T`: the argument is lent for the call.
  mut_ref,    ///< `&mut T`: the argument is lent mutably for the call.
};

struct call_argument_mapping {
  std::vector<const ast::expr *> args_by_param;
  std::vector<const ast::expr *> defaults_by_param;
  std::vector<std::string> param_names;
  std::vector<param_passing> passing_by_param;
};

/// An implicit generic's signature (`def double(x): return x + x`), as its
/// probe left it: each unannotated parameter's leaf (`k_unknown_type` for
/// the rest) and the result type in terms of them. The leaves — inference
/// variables — are the function's implicit type parameters.
struct implicit_signature {
  std::vector<type_id> params;
  type_id result = k_unknown_type;
};

/// The declaration a module-qualified free-function call (`std.io.open(...)`),
/// a type-qualified associated-function call (`io_error.from(...)`), or a
/// genuine instance-method call (`x.method(...)`) resolved to — recorded by
/// `infer_qualified_call`/`infer_method_call` (`check.cpp`) since lowering
/// has no way to redo this resolution itself (it never re-walks
/// `program_index`). `owner_module` is the module that declares `decl`;
/// `impl_target_type` is the bare name of the target type an associated
/// function or method was resolved on (e.g. `io_error` for `io_error.from`,
/// `file` for `file_value.write(...)`), empty for an ordinary
/// module-qualified free function. `receiver` is the AST expression the
/// method was called on (`field.object`) for a real instance-method call —
/// lowering evaluates it and passes it as the callee's hidden first (`self`)
/// argument; null for every other call shape (there is no receiver to pass).
struct resolved_callee {
  const ast::func_decl *decl = nullptr;
  std::string owner_module;
  std::string impl_target_type;
  const ast::expr *receiver = nullptr;
  /// The trait this call's method implements, if resolution went through a
  /// trait impl (`method_entry::trait_name`); empty for an inherent/`extend`
  /// method, a plain free function, or an unresolved call. Lets a caller
  /// identify a specific, known-consuming trait method
  /// (`into_iterator::into_iter`) without a general by-value-`self`
  /// convention to key off — see `receiver_mode` in `ownership_cfg.cpp`.
  std::string trait_name;
};

/// One implicit `deref()` call that member lookup inserted: a field or method
/// was not found on the receiver's own type, so it was looked up on the
/// target of the receiver type's `std.traits.deref` impl instead
/// (`spec/specification/02-intermediate/18-traits.md`, `deref`). `callee` is
/// that impl's `deref` method, with `receiver` unset (the receiver is
/// whatever the step is applied to); `callee_type` is its `fn` type and
/// `result` the `&target` it returns.
struct deref_step {
  resolved_callee callee;
  type_id callee_type = 0;
  type_id result = 0;
};

/// The resolved `next`-method dispatch for a `for x in it: ...` loop whose
/// iterable is a user type implementing `std.iter.iterator[T]` — recorded by
/// `check_body_node`'s `for_stmt` case (lowering has no way to redo the method
/// lookup itself) and read by `hir::lower_iterator_loop`, which synthesizes
/// the `it.next()` method call from it.
/// `decl`/`owner_module`/`impl_target_type` carry the same meaning as the
/// matching `resolved_callee` fields (the mangled call `hir::lower_call` would
/// emit for a hand-written `it.next()` is `impl_target_type::next`);
/// `element_type` is the `T` the loop variable binds.
/// A sequence literal that constructs a user collection through its
/// `std.traits.from_array` impl: the constructor to call, and the
/// `array[T, n]` type the literal itself still has. Lowering needs the array
/// type explicitly because the *expression's* checked type is now the
/// collection, not the array it is built from.
struct array_literal_conversion {
  resolved_callee callee;
  type_id array_type = 0;
};

/// A reified `static let`'s compile-time value, with the type of every part
/// resolved so lowering can rebuild it without the checker's type queries.
/// `leaf` holds a `scalar`'s number, boolean or string. `names` holds a
/// `structure`'s field names (parallel to `parts`) or a `variant`'s tag (its
/// only entry). A `collection` is built by handing an array of `parts` to
/// `from_array`: `conversion` is the synthesized literal whose
/// `checked_types::array_literal_conversions` entry names that constructor.
struct reified_value {
  enum class shape : uint8_t {
    scalar,
    array,
    tuple,
    collection,
    structure,
    variant
  };
  shape kind = shape::scalar;
  type_id type = 0;
  comptime::value leaf;
  std::vector<std::string> names;
  std::vector<reified_value> parts;
  const ast::array_expr *conversion = nullptr;
};

/// What it takes to drop one value of a struct/sum type — see
/// `checker::resolve_drop_plans` (`check.cpp`). `own_drop` is this type's own
/// `impl drop`, if it has one; `droppable_fields` are the struct fields (in
/// declaration order) that are themselves droppable, whether or not this
/// type has an `own_drop` of its own — the spec's implicit field-wise rule
/// applies regardless. A sum type instead lists `variant_drops`: for each
/// variant with a droppable payload slot, that variant's arity and the
/// droppable slots, so lowering can match on the active variant and drop
/// only that variant's payload. A `type_id` absent from
/// `checked_types::drop_plans` is not droppable at all; that absence is the
/// scope-exit drop pass's only "should I even look at this local" test.
struct variant_drop {
  std::string variant;
  size_t arity = 0; ///< Total payload slots, droppable or not.
  std::vector<std::pair<size_t, type_id>> droppable_payloads;
};

struct drop_plan {
  std::optional<resolved_callee> own_drop;
  std::vector<std::pair<std::string, type_id>> droppable_fields;
  std::vector<variant_drop> variant_drops; ///< Sum types only.
  /// A `generator[T]`: dropping one that has not finished resumes it once to
  /// return, which drops what its body still owns.
  bool cancels_generator = false;
  /// A `fn` value: what its closure environment owns depends on which lambda
  /// made it, so dropping one runs the drop glue the closure carries, if any.
  /// The glue is a `fn() -> unit` of this type over the same environment.
  std::optional<type_id> closure_glue;
};

/// The resolved `list[T]::new`/`list[T]::push` calls a `for ... => yield`
/// comprehension (`checker::infer_for_expr`) desugars into — see
/// `hir::lower_for_expr`. Recorded once at type-checking time because
/// nothing in the source names either call (the comprehension syntax is the
/// whole of it), the same reason `array_literal_conversion` exists for
/// literal-to-constructor calls.
struct comprehension_dispatch {
  resolved_callee new_callee;
  resolved_callee push_callee;
  type_id list_type = 0;
};

/// One iteration of an inline `static for` whose yield runs at run time
/// (`checked_types::static_for_iterations`).
struct static_for_iteration {
  /// Each loop binding whose value has a literal form, with that literal:
  /// lowered as a `let` in front of this iteration's copy of the yield.
  std::vector<std::pair<std::string, const ast::expr *>> bindings;
  /// Reads in the yield rooted at a loop binding with no literal form (a
  /// `field_info`'s `f.name`), each replaced by the literal it evaluated to.
  std::unordered_map<const ast::expr *, const ast::expr *> folded;
  /// Each block `static for` directly inside this one's body, unrolled with
  /// this iteration's bindings in place.
  std::vector<
      std::pair<const ast::static_decl *, std::vector<static_for_iteration>>>
      nested;
};

struct iterator_loop_dispatch {
  const ast::func_decl *decl = nullptr;
  std::string owner_module;
  std::string impl_target_type;
  type_id element_type = 0;
  /// Set when the loop's iterable is not itself an iterator but implements
  /// `std.iter.into_iterator[T]` — the `into_iter` method to call *once*,
  /// before the loop, to obtain the thing `decl` (`next`) is then called on.
  /// Null for a type that is directly iterable, which is the common case and
  /// the one that existed before.
  ///
  /// This is what lets a *collection* be iterated, as opposed to only an
  /// iterator: `for x in v` over a `vector[T]` has nowhere to put a `next`,
  /// because the collection is not consumed by iterating it.
  const ast::func_decl *adapter_decl = nullptr;
  std::string adapter_owner_module;
  std::string adapter_impl_target_type;
  /// The iterator type `adapter_decl` returns — the type of the loop's
  /// internal handle once the adapter has run. Meaningless when
  /// `adapter_decl` is null.
  type_id adapter_result_type = 0;
};

/// How one interpolation segment's embedded expression
/// (`ast::interp_segment::value`, `spec/string-formatting-design.md`) should
/// render at runtime — resolved once by `check.cpp`'s interpolation
/// capability check so `hir::lower` never has to re-derive which style
/// (builtin intrinsic vs. a real trait method) a segment's format-spec type
/// char selected.
struct interp_dispatch {
  enum class kind_t : uint8_t {
    builtin_show,  ///< *(none)*/`s` on a builtin primitive.
    builtin_debug, ///< `?` on a builtin primitive.
    builtin_radix, ///< `d`/`x`/`X`/`o`/`b` on a builtin integer.
    builtin_float, ///< `e`/`E`/`f`/`g`/`G` on a builtin float.
    builtin_char,  ///< `c` on a builtin integer (codepoint source).
    trait_method,  ///< `show`/`debug`/`hex`/`octal`/`binary` via a real
                   ///< user-defined `impl`, resolved the same way
                   ///< `resolved_callee` records an ordinary method call.
  };
  kind_t kind = kind_t::builtin_show;
  char type_char = 0;     ///< The format spec's type char, or `0` for *(none)*.
  type_id value_type = 0; ///< The segment expression's resolved static type.
  /// Valid when `kind == trait_method`, same meaning as `resolved_callee`.
  const ast::func_decl *decl = nullptr;
  std::string owner_module;
  std::string impl_target_type;
};

/// The persisted result of type-checking one session: the interned
/// `type_table` every `type_id` below indexes into, plus the resolved type
/// of every expression node the checker actually visited. Several
/// non-expression declaration nodes are recorded in `node_types` too, since
/// a later pass needs their types but they never pass through `infer_expr`:
/// a function parameter's `pattern` node (keyed by `param.pattern.get()`),
/// a function's declared return-type node (keyed by
/// `decl.return_type.get()`), and every `ast::pattern` node checked via
/// `check_pattern` (keyed by the pattern node itself, recording the type of
/// the value it matches) — all recorded in `check_function`/`check_pattern`.
/// Two shorthand shapes have no node to key against, for the same reason:
/// a struct pattern's shorthand field (`{x}`, matching
/// `ast::field_pattern.pattern == nullptr`) binds a name straight from a
/// plain struct field with no dedicated sub-pattern node, and a struct
/// *literal*'s shorthand field (`{x}`, matching
/// `ast::struct_field_init.value == nullptr`) reads an in-scope value the
/// same way with no dedicated value node — so each is recorded separately,
/// in `struct_pattern_field_types` (keyed by the owning `ast::field_pattern`)
/// and `struct_literal_field_types` (keyed by the owning
/// `ast::struct_field_init`) respectively; neither key type is itself an
/// `ast::node`, but both are stable for the AST's lifetime. A node the
/// checker never reached (inside a file already marked failing, or simply
/// never checked) has no entry in any of these maps — look it up with
/// `.find`, not `.at`. This is what a later typed-lowering pass
/// (`spec/typed-ir-design.md`) reads instead of re-deriving types from the
/// AST a second time.
/// A trait-default method body cloned and type-checked concretely for one
/// impl that doesn't override it — see `checker::build_method_table`
/// (`check.cpp`), the only place these are created. `decl` is owned by the
/// checker's `synthesized_decls_` list, moved into `checked_types` alongside
/// this record so it outlives the checker; `hir::lower_module` lowers each
/// one whose `owner_module` matches the module it's currently lowering, the
/// same way it lowers every other impl member, naming it
/// `target_type_name::<decl->name>`.
struct synthesized_method {
  const ast::func_decl *decl = nullptr;
  std::string target_type_name;
  std::string owner_module;
};

/// One item-level splice (`~expr` used directly among a file/module's
/// top-level items, as opposed to inside a function body) that resolved to
/// an injected `impl` block — see `checker::resolve_item_splices`
/// (`check.cpp`), which runs before `build_method_table`/
/// `validate_impl_coherence` so the injected impl's methods participate in
/// ordinary method lookup and coherence checking exactly like an impl
/// written directly in source. `impl` is owned by whichever file's AST the
/// quoted content came from (an ordinary quote fragment, per `quote_expr::
/// parsed_body`'s doc comment — no separate arena needed). `hir::lower_
/// module` lowers each one whose `owner_module` matches the module it's
/// currently lowering, the same way it lowers `synthesized_trait_defaults`
/// above, since the injected impl has no item of its own in any file's
/// `items` to walk.
struct synthesized_item_splice {
  const ast::impl_decl *impl = nullptr;
  std::string owner_module;
};

/// One monomorphized instance of a function generic over compile-time *value*
/// parameters only (`def get[n: usize](v: array[int32, n], i: index[n])`) —
/// see `checker::instantiate_generic_function` (`check.cpp`), the only place
/// these are created.
///
/// A value parameter has no runtime existence: `n` is not passed, and code
/// that mentions it (`index[n]`'s bound, a `for i in 0..n`, the runtime check
/// an `index[n].try_from(raw)` compiles into) can only be emitted once `n` is
/// a real number. So the template itself is never lowered; instead each call
/// site whose arguments pin `n` down to a constant gets an instance — a clone
/// of the declaration, re-checked with `n` bound to that constant, named
/// `get$3` and lowered like any other function. `decl` is owned by
/// `checked_types::synthesized_decls`, and its `name` is already the mangled
/// instance name, so `hir::lower_call` needs nothing beyond the ordinary
/// `resolved_callee` the call site records against it.
struct const_generic_instance {
  const ast::func_decl *decl = nullptr;
  std::string owner_module;
};

/// One `def` cloned into a materialized functor instantiation — see
/// `checker::materialize_functor` (`check.cpp`), the only place these are
/// created. A parameterized `module m[P: sig]` has no runtime form of its
/// own; each `use m[args]` instantiation clones the body's `def`s, checks
/// them concretely with the module parameter bound as an import alias, and
/// records one of these per clone. `owner_module` is the *synthetic* module
/// name (the sanitized instantiation key) — the same name a `db.f(...)` call
/// site records as its callee's `owner_module`, so `lower_functor_modules`
/// groups the clones under it into a standalone `hir_module` that
/// cross-module dispatch then finds by name, exactly as it would a
/// hand-written top-level module in its own file. `decl` is owned by
/// `checked_types::synthesized_functor_nodes`.
struct functor_instance {
  const ast::func_decl *decl = nullptr;
  std::string owner_module;
  /// Non-empty when `decl` is a method of a functor-body `impl`/`extend`
  /// block: the bare name of the target type, so `lower_functor_modules`
  /// emits the method as `impl_target::decl->name` — the same key a
  /// `receiver.method()` call site records for it — instead of a bare
  /// free-function name. Empty for an ordinary functor-body `def`.
  std::string impl_target;
};

/// Type ids for `std.fmt`'s runtime-support types (`src/std/fmt.cn`),
/// resolved once after checking finishes and handed to `hir::lower` so it
/// can build `format_spec` struct literals for interpolation lowering
/// without a name-based lookup of its own — `type_table` only
/// supports looking a user type up via its declaring `ast::type_decl`, which
/// lowering has no route to independent of an AST node it's already
/// visiting (see `interp_dispatch`'s use in `hir::lower`). Left as
/// `k_unknown_type` (0) if `std.fmt` wasn't part of the session — lowering
/// only reads these when a `interp_dispatch` entry exists, and one can only
/// exist if a source file had string interpolation, which the driver never
/// allows without also injecting `std.fmt` (`inject_stdlib_prelude`).
/// A compile-time type-reflection call (`T.name()`) whose subject is one of
/// the enclosing function's own type parameters, resolved once that
/// parameter is bound to a concrete type inside a monomorphized instance —
/// see the `lookup_type_param` branch of `checker::infer_call` (`check.cpp`).
///
/// Reflection (`T.name()`/`T.fields()`/`T.field_count()`) otherwise only
/// evaluates inside `static` constructs, via `comptime::evaluator` — that
/// evaluator has no notion of "the type a generic instance's own parameter
/// was monomorphized with," since it is a wholly separate subsystem from the
/// checker's per-instance `type_param_slots_` binding. So a reflection call
/// on a type parameter, written in *ordinary* (non-static) code, type-checks
/// fine (the checker already knows the concrete type once inside a checked
/// instance) but has nothing backing it at lowering time — this record is
/// exactly that missing backing: `hir::lower` folds the call straight into
/// the recorded name rather than trying to lower a call to something with no
/// runtime existence. Absent for a reflection call on a real declared type
/// (`point.name()`), which is unrelated (and unsupported outside `static`
/// contexts, same as ever) — only a type *parameter*'s answer is knowable
/// purely from which instance is being checked.
struct type_param_reflection {
  std::string type_name;
};

struct fmt_runtime_types {
  type_id format_spec = 0;
  type_id align_mode = 0;
  type_id sign_mode = 0;
  /// Builtin scalar types lowering needs for casts ahead of a `std.fmt`
  /// helper call (e.g. widening an `int32` to `int64` before
  /// `fmt_show_i64`) — resolved here for the same reason the `box_*`/
  /// `format_spec` ids above are: `type_table::builtin` needs mutable
  /// access to intern-or-fetch, which `hir::lower` (holding only a `const
  /// type_table&`) doesn't have. `bool`/`usize`/`char` aren't included
  /// since `type_table` already exposes those three as const lookups.
  type_id str_type = 0;
  type_id int64_type = 0;
  type_id uint64_type = 0;
  type_id uint32_type = 0;
  type_id uint8_type = 0;
  type_id float64_type = 0;
  /// `option[align_mode]`/`option[usize]` — needed to build a structurally
  /// correct `@some(...)`/`@none` `hir_variant_init` for `format_spec.align`/
  /// `.width`/`.precision`, since a sum-type value's tag/slot layout depends
  /// on its exact `type_id`, not just its variant name.
  type_id option_align_mode = 0;
  type_id option_usize = 0;
};

/// Which question a `checked_types::layout_queries` entry asks about its
/// operand type.
enum class layout_query_kind : uint8_t {
  size_of,  ///< `size_of[T]()` — the bytes one `T` occupies.
  align_of, ///< `align_of[T]()` — the byte boundary a `T` must start on.
  /// `needs_drop[T]()` — whether dropping a `T` does anything. Answered from
  /// `checked_types::drop_plans`, which is only complete after checking ends.
  needs_drop,
};

/// A resolved `size_of[T]()` / `align_of[T]()` — which type, which question.
struct layout_query {
  type_id operand = 0;
  layout_query_kind kind = layout_query_kind::size_of;
};

/// A resolved `uninit[T, N]()` — `N` slots, each sized and aligned for `T`.
struct stack_buffer_request {
  type_id element = 0;
  uint64_t count = 0;
};

/// Decisions the checker records for HIR lowering: operator, index, and
/// iterator dispatches, synthesized declarations and literals, instance
/// records, and compile-time-folded values. Read by `hir::lower*`.
struct lowering_results {
  std::unordered_map<const ast::field_pattern *, type_id>
      struct_pattern_field_types;
  std::unordered_map<const ast::struct_field_init *, type_id>
      struct_literal_field_types;
  /// Each struct literal field's *declared* type, as the literal's struct
  /// instance spells it (`list[int32]` for `list_into_iter[int32]`'s `src`).
  /// Lowering reads it to make a borrow or deref between the value and the
  /// field explicit (`hir::make_references_explicit`).
  std::unordered_map<const ast::struct_field_init *, type_id>
      struct_literal_field_expected;
  /// Every `ident_expr` that names a module-level function in *value*
  /// position — bound to a `let`, passed as an argument, returned — rather
  /// than being called. `checker::resolve_ident` records the declaration and
  /// its owning module here for exactly the same reason `resolved_callees`
  /// exists for the call case: lowering cannot redo the import/wildcard
  /// resolution itself, and without `owner_module` both backends key the
  /// reference against the *referencing* module and fail to find it
  /// ("reference to `f` is not a local binding"). Only `decl`/`owner_module`/
  /// `impl_target_type` are meaningful; `receiver` is always null (a bare
  /// value reference has no receiver to pass).
  std::unordered_map<const ast::node *, resolved_callee> resolved_fn_values;
  /// Every arithmetic operator (`+`/`-`/`*`/`/`/`%`) resolved against a
  /// user struct/sum operand's `add`/`sub`/`mul`/`div`/`rem` impl — see
  /// `checker::require_operand_trait` (`check.cpp`). `receiver` is always
  /// `binary_expr::lhs` (the operator's overload trait method takes the
  /// other operand as its sole explicit argument). Absent for a numeric
  /// operator or one whose operand trait requirement failed to resolve to a
  /// real method (e.g. a missing impl, already diagnosed separately).
  std::unordered_map<const ast::binary_expr *, resolved_callee>
      operator_dispatches;
  /// For every `<`/`<=`/`>`/`>=` entry in `operator_dispatches` (`ord`'s
  /// `cmp` method, not called through directly the way `eq`'s same-named
  /// method is), the resolved return type (`ordering`) of that `cmp` call —
  /// see `checker::wire_ord_dispatch`. `hir::lower_binary` needs this to
  /// type the intermediate `cmp(...)` call it synthesizes before matching
  /// the result down to `bool`.
  std::unordered_map<const ast::binary_expr *, type_id>
      ord_dispatch_result_types;
  /// Every `v[i]` *read* resolved against a user type's `std.traits.index`
  /// impl — see `checker::require_index_trait` (`check.cpp`). `receiver` is
  /// the `index_expr::object`; the subscript becomes `at`'s sole explicit
  /// argument.
  ///
  /// Keyed separately from `operator_dispatches` rather than sharing it: an
  /// index expression has a receiver and a subscript, not two operands, and
  /// the same node can appear as either a value (`x = v[i]`, dispatching to
  /// `at`) or a place (`v[i] = x`, dispatching to `set_at` and recorded in
  /// `index_set_dispatches` instead). One map keyed by node could not hold
  /// both answers for the same syntax.
  ///
  /// Absent for every builtin container (`list`/`slice`/`str`/`array`/
  /// `uninit`/`*T`), which keeps its direct addressing — `hir::lower_index`
  /// emits an `hir_index` when no entry is found.
  std::unordered_map<const ast::index_expr *, resolved_callee> index_dispatches;
  /// Every `v[i] = x` resolved against a user type's `std.traits.index_set`
  /// impl, keyed by the assignment *target* (the `index_expr` on the left).
  /// `receiver` is that expression's `object`; `set_at` takes the subscript
  /// and the assigned value as its two explicit arguments.
  std::unordered_map<const ast::index_expr *, resolved_callee>
      index_set_dispatches;
  /// Every `&mut v[i]` resolved against a user type's `std.traits.index_mut`
  /// impl, keyed by the `index_expr` the `&mut` wraps. `receiver` is that
  /// expression's `object`; `at_mut` takes the subscript as its sole
  /// explicit argument and returns the `cell_mut[T]` the whole `&mut v[i]`
  /// expression evaluates to. Consulted only by `hir::lower_unary`: the
  /// enclosing `&mut` is what selects `at_mut` over `index_dispatches`'
  /// read-only `at`, so this dispatch is never reached through
  /// `hir::lower_index` the way the other two are.
  std::unordered_map<const ast::index_expr *, resolved_callee>
      index_mut_dispatches;
  /// Every `&v[i]` resolved against a user type's `std.traits.index_ref`
  /// impl, keyed by the `index_expr` the `&` wraps. `receiver` is that
  /// expression's `object`; `at_ref` takes the subscript as its sole
  /// explicit argument and returns the `cell[T]` the whole `&v[i]`
  /// expression evaluates to. Consulted only by `hir::lower_unary`, the same
  /// way `index_mut_dispatches` is: the enclosing `&` is what selects
  /// `at_ref` over `index_dispatches`' read-only `at`.
  std::unordered_map<const ast::index_expr *, resolved_callee>
      index_ref_dispatches;
  /// Every sequence literal (`[a, b, c]`, `[v; n]`) written where a user
  /// type implementing `std.traits.from_array` was expected — see
  /// `checker::try_wire_from_array`. The literal itself still lowers to the
  /// `array[T, n]` it always did; `hir::lower_array` then wraps that in a
  /// call to the recorded `from_array`, so literal syntax reaches any
  /// collection that says what a literal of it means rather than only the
  /// compiler-known `list`.
  std::unordered_map<const ast::array_expr *, array_literal_conversion>
      array_literal_conversions;
  /// Every interpolation segment's resolved rendering dispatch — see
  /// `interp_dispatch`'s doc comment. Keyed by the segment's `value`
  /// expression pointer (`ast::interp_segment::value.get()`).
  std::unordered_map<const ast::expr *, interp_dispatch> interp_dispatches;
  /// Every `T.name()` reflection call resolved against a type *parameter* —
  /// see `type_param_reflection`'s doc comment. Keyed by the `call_expr`
  /// node itself. Absent for a reflection call over a real declared type,
  /// which stays unsupported outside `static` contexts as before.
  std::unordered_map<const ast::call_expr *, type_param_reflection>
      type_param_reflections;
  /// Every reflection call on a concrete type or module (`T.fields()`,
  /// `T.kind()`, `M.functions()`, ...), mapped to its answer. Reflection
  /// is a constant once the type is known, so lowering emits the value in
  /// place of the call.
  std::unordered_map<const ast::call_expr *, reified_value> reflection_folds;
  /// Every `for ... => yield` comprehension's resolved `list[T]::new`/
  /// `list[T]::push` calls — see `comprehension_dispatch`'s doc comment.
  /// Keyed by the `ast::for_expr` node.
  std::unordered_map<const ast::for_expr *, comprehension_dispatch>
      comprehension_dispatches;
  /// Every fill literal whose repeat count is only known at runtime
  /// (`[v; n]` for a non-constant `n`), with the same `list[T]::new`/
  /// `list[T]::push` pair a comprehension uses. A constant count builds an
  /// `array[T, n]` and goes through `from_array`
  /// (`array_literal_conversions`) instead; a runtime count has no
  /// compile-time-sized array to hand it, so it is filled by a counting loop
  /// — see `hir::lower_array`. Keyed by the `ast::array_expr` node.
  std::unordered_map<const ast::array_expr *, comprehension_dispatch>
      runtime_fill_dispatches;
  /// Every inline `static for pat in xs => e` in a function body, unrolled:
  /// the values it yields, computed at compile time and embedded as
  /// literals (or struct literals of literals) in iteration order, plus the
  /// `list[T]::new`/`list[T]::push` pair that builds the list it evaluates to.
  /// `hir::lower_stmt` builds that list from them. A generic function's are
  /// recorded per instance, on the instance's clone of the declaration.
  std::unordered_map<const ast::static_decl *, std::vector<const ast::expr *>>
      static_for_elements;
  std::unordered_map<const ast::static_decl *, comprehension_dispatch>
      static_for_dispatches;
  /// Every inline `static for` whose yield runs when the program runs (it
  /// reads a runtime value such as a parameter, or yields something with no
  /// literal form), unrolled to one copy of the yield per iteration instead
  /// of a list of values. `hir::lower_stmt` lowers the yield once per entry,
  /// with that iteration's loop bindings and folded reads in place, and
  /// builds the list with the decl's `static_for_dispatches` pair.
  std::unordered_map<const ast::static_decl *,
                     std::vector<static_for_iteration>>
      static_for_iterations;
  /// Every `?` (`try_expr`) whose operand's `result[_, E1]` error type
  /// differs from the enclosing function's declared `result[_, E2]` error
  /// type and resolved against a real `impl from[E1] for E2` — see
  /// `checker::infer_try`. `receiver` is always unset (`from` is an
  /// associated function, not a `self`-receiver method); `impl_target_type`
  /// names `E2`. Absent when the two error types are the same (no
  /// conversion needed) or when no such impl exists (already diagnosed).
  std::unordered_map<const ast::try_expr *, resolved_callee> try_conversions;
  /// For every entry in `try_conversions`, the enclosing function's full
  /// declared return type (`result[_, E2]`) — `hir::lower_try` needs this
  /// to type the reconstructed `@err(E2::from(e))` value it builds in the
  /// failure arm, since that value's type is the *function's* return type,
  /// not the operand's.
  std::unordered_map<const ast::try_expr *, type_id> try_conversion_types;
  /// Resolved once from `std.fmt`'s own type declarations — see
  /// `fmt_runtime_types`'s doc comment.
  fmt_runtime_types fmt_types;
  /// Every trait-default method cloned and monomorphized for a concrete impl
  /// — see `synthesized_method`'s doc comment. Owns the clones themselves
  /// (moved here from the checker) so their lifetime outlives type-checking.
  ast::ptr_vec<ast::func_decl> synthesized_decls;
  std::vector<synthesized_method> synthesized_trait_defaults;
  /// Every const-generic function instantiated at some call site — see
  /// `const_generic_instance`'s doc comment. `hir::lower_module` lowers each
  /// one whose `owner_module` matches the module it's lowering, exactly as it
  /// does a trait default, and skips the templates they came from (a template
  /// has no runtime form of its own).
  std::vector<const_generic_instance> const_generic_instances;
  /// Functions with an unannotated parameter their own body leaves open
  /// (`def double(x): return x + x`). Such a parameter is an implicit type
  /// parameter, so the function has no runtime form of its own: the checker
  /// monomorphized an instance per call type, registered in
  /// `const_generic_instances`, and `hir::lower_module` skips these
  /// templates exactly as it skips an explicit generic.
  std::unordered_set<const ast::func_decl *> open_param_templates;
  /// Owns the copies in `generic_lambda_instances`.
  ast::ptr_vec<ast::expr> synthesized_lambdas;
  /// Every free, module-level `static def` registered with `comptime::
  /// evaluator` for compile-time calling (`checker::register_comptime_
  /// globals`'s `register_pending_function` call) — as opposed to an
  /// ordinary `static def` *method* declared inside an `impl`/`extend`/
  /// `trait` block (`zero`, `one`, `from_iter`, ...), which shares the same
  /// `func_decl::modifiers.is_static` flag but has a real runtime body and
  /// must still be lowered normally. `hir::lower_module` uses this set,
  /// not the bare flag, to decide which `const_generic_instance`s to skip
  /// lowering for — see its doc comment there.
  std::unordered_set<const ast::func_decl *> comptime_only_functions;
  /// Owns every `def` cloned into a materialized functor instantiation — see
  /// `functor_instance`'s doc comment. Kept alive here (moved out of the
  /// checker) so the clones outlive type-checking, exactly like
  /// `synthesized_decls`.
  ast::ptr_vec<ast::node> synthesized_functor_nodes;
  /// Every functor-instantiation clone plus the synthetic module it belongs
  /// to — see `functor_instance`'s doc comment. `driver::lower_and_emit_
  /// modules` calls `hir::lower_functor_modules` on these after lowering the
  /// real source files, appending one synthetic `hir_module` per distinct
  /// `owner_module`.
  std::vector<functor_instance> functor_instances;
  /// Every `named_type` synthesized by `checker::reinterpret_as_named_type`
  /// when a quoted `expr` fragment (a bare name/dotted path, ambiguous at
  /// parse time between an expression and a type) is used somewhere a type
  /// is expected — see its doc comment. Owned here for the same lifetime
  /// reason as `synthesized_decls` above.
  ast::ptr_vec<ast::type_expr> synthesized_types;
  /// Owns every AST node the `comptime::evaluator` constructed
  /// programmatically (`expr.lit(...)`/`expr.ident(...)`, see `evaluator::
  /// synthesized_fragments_`'s doc comment) — moved out of the evaluator
  /// (which is destroyed along with `checker` once `check_program`
  /// returns) so anything `spliced_fragments` points at stays alive for
  /// `hir::lower`, which runs afterward.
  ast::ptr_vec<ast::node> synthesized_fragments;
  /// Every item-level splice resolved to an injected `impl` block — see
  /// `synthesized_item_splice`'s doc comment.
  std::vector<synthesized_item_splice> synthesized_item_splices;
  /// Owns every literal node synthesized to embed a scalar (integer/
  /// float/bool) top-level `static let`'s compile-time-evaluated value
  /// directly into referencing code — see `checker::materialize_const_
  /// literal`. Same lifetime rationale as `synthesized_decls`.
  ast::ptr_vec<ast::literal_expr> synthesized_const_literals;
  /// Every `ident_expr` that resolved to a scalar-valued top-level
  /// `static let`, mapped to the literal node embedding its value — see
  /// `checker::resolve_ident`. `hir::lower_ident` looks a reference up
  /// here first and, when present, lowers the literal in its place
  /// instead of emitting an unresolvable `hir_local_ref`: a plain scalar
  /// `static let` has no other route to a runtime representation — there
  /// is no HIR/bytecode notion of "load global constant", only locals and
  /// (indirectly, via splicing) quoted fragments.
  std::unordered_map<const ast::node *, const ast::literal_expr *>
      static_const_values;
  /// Every ordinary-code call to a comptime-only `static def` instance
  /// (`comptime_only_functions`) that `checker::try_fold_comptime_only_call`
  /// evaluated outright, mapped to the literal node embedding its result —
  /// see todo item 8. `hir::lower_call` looks a call up here first and, when
  /// present, lowers the literal in its place instead of emitting a call to
  /// a function `hir::lower_module` deliberately never lowers.
  std::unordered_map<const ast::call_expr *, const ast::literal_expr *>
      folded_comptime_calls;
  /// Every `size_of[T]()` / `align_of[T]()` call, mapped to the type it
  /// asks about. The *answer* is deliberately not stored here: a type's
  /// size and alignment are `src/runtime/layout.h`'s to define — it is the
  /// single source both backends read their layouts from — and
  /// `//src/runtime` depends on this library, so the checker cannot call
  /// into it without a cycle. The checker therefore resolves only the
  /// question (which type, which query), and `hir::lower_call` answers it
  /// against `runtime::layout_of` and splices in an integer literal. That
  /// also keeps the two in step by construction: a `size_of[T]()` can never
  /// disagree with the stride the same `T` actually occupies in a struct
  /// field or list element, because both come from the one function.
  std::unordered_map<const ast::call_expr *, layout_query> layout_queries;
  /// Every `ptr_cast[U](p)` call, mapped to the pointer type it produces.
  /// A raw pointer's runtime representation is an address and nothing else,
  /// identical for every pointee type, so this generates no code at all:
  /// `hir::lower_call` lowers the operand and retypes the resulting node.
  /// The entry exists because that retype is the *whole* operation — without
  /// it the node would keep the source pointee type, and the next `p[i]`
  /// through it would scale by the wrong element stride.
  std::unordered_map<const ast::call_expr *, type_id> ptr_casts;
  /// Every `std.mem.slice_from_raw_parts(p, len)`/
  /// `slice_mut_from_raw_parts(p, len)` call — see
  /// `checker::infer_slice_from_raw_parts_call`. `hir::lower_call` builds an
  /// `hir::hir_slice_from_raw_parts` from it directly (the call's own
  /// recorded checked type already says `slice[T]` vs `slice_mut[T]`); no
  /// `func_decl` backs either name (same rationale as
  /// `layout_queries`/`ptr_casts`).
  std::unordered_set<const ast::call_expr *> slice_from_raw_parts_calls;
  /// Every `uninit[T, N]()` call, mapped to the buffer it asks for: the
  /// element type and the slot count. Like `layout_queries`, the *size* is
  /// deliberately not stored — `hir::lower_call` derives both the byte size
  /// and the alignment from `runtime::layout_of(T)`, so a `uninit[T, N]`'s
  /// stride can never disagree with the stride a `*mut T` through it uses.
  std::unordered_map<const ast::call_expr *, stack_buffer_request>
      stack_buffers;
  /// One top-level `static let` reified as real backing data — an array,
  /// list, tuple, variant, or struct holding one of those, which
  /// `checker::materialize_const_literal` cannot inline the way it does a
  /// single scalar. `value` is its compile-time value with every part typed
  /// (see `reified_value`). `hir::lower_module_items` looks a module's own
  /// `static_decl` items up here (by declaration pointer) and lowers a
  /// matching entry into an `hir_static_global`; `name` is unique across the
  /// whole program, so every backend can key one flat global table by it
  /// regardless of which module a reference lives in — mirroring how the
  /// existing cross-module function table already works.
  struct static_global_def {
    std::string name;
    type_id type;
    reified_value value;
  };
  std::unordered_map<const ast::static_decl *, static_global_def>
      static_global_defs;
  /// Every function-body-scope `static if`/`static if`-`else` (`decl_kind ==
  /// conditional_compilation`) whose condition `checker::resolve_static_if_
  /// branch` resolved to a concrete boolean, mapped to which side was taken
  /// (`true` selects `if_body`, `false` selects `else_body`). `hir::lower_
  /// stmt`'s `static_decl` case looks a node up here instead of re-running
  /// compile-time evaluation itself, so the lowerer's branch selection can
  /// never drift from the checker's. Absent for a `static_decl` the checker
  /// couldn't resolve (an un-instantiated generic template's speculative
  /// pass) — such a node is never reached by `hir::lower_stmt` because a
  /// template body is never lowered directly, only its concrete
  /// instantiations are.
  std::unordered_map<const ast::static_decl *, bool> static_if_taken_branch;
  /// Every `ident_expr`/`module_path_expr` that resolved to a struct-valued
  /// `static let`, mapped to a synthesized struct literal rebuilding its
  /// compile-time value (owned by `synthesized_static_structs`).
  /// `hir::lower_ident`/`lower_module_path` lower that literal in place, so
  /// each use constructs the struct from constants.
  std::unordered_map<const ast::node *, const ast::expr *> static_struct_values;
  ast::ptr_vec<ast::expr> synthesized_static_structs;
  /// Every `ident_expr`/`module_path_expr` that resolved to an evaluated
  /// `static let` whose value has none of the run-time routes above (a list
  /// of strings, a variant, ...), mapped to the static's name. Recorded
  /// rather than reported by the checker because compile-time code may read
  /// such a static freely; `hir::lower_ident`/`lower_module_path` report it
  /// only when run-time code is actually lowered.
  std::unordered_map<const ast::node *, std::string> unreadable_static_refs;
  /// Every `ident_expr`/`module_path_expr` that resolved to a `static let`
  /// holding quoted syntax, mapped to the static. Compile-time code asks the
  /// compiler for its value (`hir_host_call` `static`).
  std::unordered_map<const ast::node *, const ast::static_decl *>
      static_fragment_refs;
  /// Reified global name -> the module whose `static_decl` it was reified
  /// from (`reify_static_global`'s `owner` parameter). `hir::lower_ident`/
  /// `lower_module_path` consult this to fill in `hir_global_ref::
  /// owner_module`, so `hir::find_reachable_modules`'s walker can tell that a
  /// module referenced only through one of its globals (no function call
  /// into it) still needs to be compiled — a cross-module `static` table
  /// with no functions of its own, like `std.unicode_tables`, has no other
  /// way to end up in the reachable set.
  std::unordered_map<std::string, std::string> static_global_owners;
  /// Every `v[i]` the reasoning solver proved in bounds
  /// (`checker::check_index_in_bounds`) — an index whose safety is a
  /// *compile-time* fact, so lowering may omit the runtime bounds check
  /// entirely. This is what a dependent signature buys: `safe_get(v: array[T,
  /// n], i: index[n])` indexes with no check at all, because `i < n` and
  /// `len(v) == n` were established before the program ever ran. An index
  /// absent from this set is unproven, not unsafe — it simply keeps its
  /// check, exactly as every index does today.
  std::unordered_set<const ast::index_expr *> proven_in_bounds;
  /// Every `pre` condition the solver proved from the callee's own parameter
  /// types — a refinement, a `usize` domain, an earlier `pre` — and so from
  /// nothing the caller supplies (`checker::collect_function_facts`).
  /// Lowering emits nothing at all for these: the contract is satisfied by
  /// construction on *every* call, so its check (`hir_contract_check`, emitted
  /// once at the callee's entry) would be dead code that can never fail. A
  /// contract absent from this set was not disproved — it was simply not
  /// proved, which is the ordinary case and the reason contracts have a
  /// runtime form at all.
  std::unordered_set<const ast::contract_clause *> elided_contracts;
};

/// Facts the ownership checker (`ownership_cfg`, `ownership_check`) reads:
/// which types borrow, which types are Copy, and the template records for
/// generic lambdas and functions.
struct ownership_results {
  /// Lambdas bound by `let f = (x) => ...` with an unannotated parameter.
  /// Like an unannotated `def` parameter, that parameter is an implicit type
  /// parameter, so the lambda written in the source has no runtime form of
  /// its own: each use of `f` checked a copy of it for that use's types.
  /// Maps the written lambda to those copies, in the order they were made;
  /// `hir::lowerer` binds one closure per copy where the `let` stands, and
  /// none at all for a lambda that is never used.
  std::unordered_map<const ast::lambda_expr *,
                     std::vector<const ast::lambda_expr *>>
      generic_lambda_instances;
  /// Each use of a generic lambda's name, to the copy it was checked against.
  /// A use is an `ast::ident_expr` or a shorthand `ast::struct_field_init`.
  std::unordered_map<const void *, const ast::lambda_expr *>
      generic_lambda_refs;
  /// Where a copy in `generic_lambda_instances` was asked for: the name of
  /// the generic lambda and the use that needed the copy. Read by the
  /// ownership checker to explain a capture each copy moves.
  struct generic_lambda_use {
    std::string name;
    source_span span;
  };
  std::unordered_map<const ast::lambda_expr *, generic_lambda_use>
      generic_lambda_uses;
  /// Each `open_param_templates` entry's signature.
  std::unordered_map<const ast::func_decl *, implicit_signature>
      implicit_signatures;
  /// Each `const_generic_instances` entry's template: the generic
  /// declaration it was made from, whose signature still names the type
  /// parameters the instance's has substituted away.
  std::unordered_map<const ast::func_decl *, const ast::func_decl *>
      instance_templates;
  /// Every interned `type_id` whose values can carry a borrow: a view
  /// (`slice`/`mut slice`, `cell`/`mut cell`), a reference (`&T`/`&mut T`),
  /// a callable (`fn(...)`, since a closure may hold `&`/`&mut` captures), or
  /// a type parameter (it may be instantiated with any of these) — directly
  /// or inside a struct field, sum variant payload, tuple element, array
  /// element, or generic argument. Computed over the whole table once
  /// checking finishes (`checker::type_carries_borrow`), because resolving a
  /// struct's field types needs the generic substitution only the checker
  /// has. The ownership checker (`check_ownership`) reads it to decide
  /// whether copying or moving a value also copies the borrows it holds.
  /// `str` is deliberately *excluded* even though the language models it as
  /// a view: the implementation treats `str` as an owned value everywhere.
  std::unordered_set<type_id> borrow_bearing_types;
  /// The subset of `borrow_bearing_types` that carries a borrow of its own,
  /// not only through a type parameter: `&T` and `box[&int32]` are in it,
  /// `T` and `box[T]` are not. A call whose declared result is outside this
  /// set can only hand back borrows that arrived through an argument typed
  /// with one of the result's type parameters.
  std::unordered_set<type_id> own_borrow_bearing_types;
  /// The type parameters declared with a `copy` bound, inline or in a
  /// `where` clause. Keyed like every type parameter, on the declaration that
  /// introduced it, so the set means the same thing in every body.
  std::unordered_set<type_id> copy_type_params;
  /// The sum types (user sums, `option`, `result`) whose every variant
  /// payload is `copy` and that have no `drop`. A sum value never changes
  /// after it is built, so sharing its heap block is a copy (ch. 14, Copy and
  /// move). Computed over the whole table once checking finishes, because a
  /// generic sum's payload types need the checker's substitution.
  std::unordered_set<type_id> copy_sum_types;
};

/// Drop plans for every droppable struct and sum type. Read by
/// `hir::compute_drop_schedule` to build scope-exit drop calls.
struct drop_results {
  /// Every struct/sum type found droppable — see `drop_plan`'s doc comment.
  /// Populated once, after the main per-function walk, by
  /// `checker::resolve_drop_plans` (`check.cpp`) over every type interned in
  /// `types`. Consumed by `hir::compute_drop_schedule`/`hir::lowerer` to
  /// build scope-exit drop calls (`spec/todo.md` item 6).
  std::unordered_map<type_id, drop_plan> drop_plans;
};

struct checked_types {
  type_table types;
  std::unordered_map<const ast::node *, type_id> node_types;
  /// The file each node in `node_types` was checked under, recorded at the
  /// same moment and from the same `file_id_` the checker was standing in.
  ///
  /// An `ast::node` carries only a `source_span` — byte offsets with no file
  /// — so a node pointer alone cannot say where it came from, and every file
  /// in a session has a byte 0. Nothing in the compiler needed the answer
  /// before: a pass that holds a node is already standing in its file.
  /// `semantic::render_snapshot` (`snapshot.h`) is the exception — it walks
  /// the decision maps from outside any file, and an offset with no file name
  /// is not a reviewable diff.
  ///
  /// Recorded only in `record_expr_type`, which is the single funnel every
  /// typed expression passes through. That covers the great majority of the
  /// nodes the other decision maps key against too (a dispatch is always
  /// recorded for an expression the checker also typed), so those maps
  /// resolve their file by looking the node up here; a key that isn't
  /// present renders with its offset alone rather than a guess.
  std::unordered_map<const ast::node *, file_id_type> node_files;
  std::unordered_map<const ast::call_expr *, call_argument_mapping>
      call_argument_mappings;
  /// Every call resolved by `infer_qualified_call` — see `resolved_callee`'s
  /// doc comment. A node absent here was either resolved some other way
  /// (a plain same-module/imported bare-name call, a method call) or never
  /// resolved at all.
  std::unordered_map<const ast::call_expr *, resolved_callee> resolved_callees;
  /// Every `for` loop whose iterable is a user type implementing
  /// `std.iter.iterator[T]` — see `iterator_loop_dispatch`'s doc comment.
  /// Keyed by the `ast::for_stmt` node. Absent for range/option/generator/
  /// indexable iterables, which lower through their own dedicated shapes.
  std::unordered_map<const ast::for_stmt *, iterator_loop_dispatch>
      for_iterator_dispatches;
  /// The same, for a `for ... => yield` comprehension's iteration clauses.
  /// A clause reaches the identical loop lowerers a statement `for` does, so
  /// it needs the identical dispatch record — without one, `for (k, v) in
  /// pairs => k * v` over a `list` fell through to the indexed-loop shape,
  /// which stopped applying when `list` became an ordinary stdlib type.
  /// Keyed by the clause's own `iterable` expression node, which is unique
  /// per clause. Absent for range/option/indexable iterables.
  std::unordered_map<const ast::node *, iterator_loop_dispatch>
      comprehension_iterator_dispatches;
  /// The return type the checker inferred from the body of each function
  /// declared without one. Lowering reads it where it would read the
  /// annotation.
  std::unordered_map<const ast::func_decl *, type_id> inferred_return_types;
  /// Every `splice_expr`/`splice_stmt` node resolved (via compile-time
  /// evaluation of its operand) to the exact AST fragment it should be
  /// lowered as, keyed by the splice node itself. `hir::lower` looks a
  /// splice node up here and lowers the resolved fragment in its place —
  /// see `checker::infer_expr`'s `splice_expr` case and
  /// `checker::check_body_node`'s `splice_stmt` case. Absent entries mean
  /// the splice never resolved to usable syntax (already diagnosed).
  std::unordered_map<const ast::node *, const ast::node *> spliced_fragments;
  /// Every `ident_expr`/`module_path_expr` that resolved to a reified static
  /// global (see `static_global_defs`), mapped to that global's `name` —
  /// `hir::lower_ident`/`lower_module_path` look this up before falling back
  /// to `hir_local_ref`, mirroring `static_const_values` but for the
  /// aggregate case that can't be inlined at each reference site.
  std::unordered_map<const ast::node *, std::string> static_global_refs;
  /// Every `module_path_expr` the checker classified as *field access on a
  /// value* (`s.cases`, `self.value.inner`) rather than a module reference —
  /// see `checker::dotted_root_is_value` — mapped to the type after each
  /// segment: `[0]` is the root binding's type, `back()` the whole path's.
  /// `hir::lower_module_path` builds its projection chain from this and
  /// never re-decides what the path means; a `module_path_expr` absent from
  /// this map is a module reference.
  std::unordered_map<const ast::module_path_expr *, std::vector<type_id>>
      value_path_types;
  /// Every receiver of a field access or method call (`field_expr::object`)
  /// whose member was found only through `deref` — see `deref_step`. The
  /// steps run in order on the receiver's value before the member is
  /// reached: `h.x` lowers as `h.deref().x`. A receiver absent here is used
  /// as it is.
  std::unordered_map<const ast::expr *, std::vector<deref_step>>
      deref_adjustments;
  /// The same, for a value-rooted dotted path (`value_path_types`): for each
  /// segment index `i`, the steps applied to the value of the first `i`
  /// segments before segment `i` is selected. Empty where a segment needs
  /// none; a path absent here needs none at all.
  std::unordered_map<const ast::module_path_expr *,
                     std::vector<std::vector<deref_step>>>
      path_deref_adjustments;

  /// Lowering decisions; see `lowering_results`.
  lowering_results lowering;
  /// Ownership facts; see `ownership_results`.
  ownership_results ownership;
  /// Drop plans; see `drop_results`.
  drop_results drop;

  /// Whether a value of `id` copies rather than moves: `type_table::is_copy`,
  /// a type parameter bounded by `copy`, or a `copy` sum.
  [[nodiscard]] auto is_copy(type_id id) const -> bool {
    return types.is_copy(id) || ownership.copy_type_params.contains(id) ||
           ownership.copy_sum_types.contains(types.strip_refinement(id));
  }
};

} // namespace cinder::semantic
