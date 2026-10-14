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

#include "src/comptime/fragments.h"
#include "src/comptime/value.h"
#include "src/parser/ast.h"
#include "src/parser/diagnostic.h"

namespace cinder::comptime {

/// A closed, tree-walking interpreter for the compile-time-evaluable subset
/// of Cinder used by `static` declarations (`static let`, `static assert`,
/// `static if`, `static for`, and `static def` calls).
///
/// Deliberately closed: no I/O, no filesystem, no environment access, and
/// no mutable state beyond its own binding environment — the language's
/// "two-effect model" (compile-time code may only emit diagnostics or emit
/// code, nothing else observable) means this evaluator must never grow an
/// escape hatch to native calls the way the tier-0 bytecode VM's
/// `op_call_intrinsic` does; that VM backs *runtime* execution and answers
/// a different question.
///
/// Supports literals, arithmetic/comparison/logical operators, unary
/// negation, `static let`/`static def` references (including forward
/// references, resolved lazily against a whole-session pending table),
/// list/struct literals, field/index access, pattern-destructuring `let`,
/// `if`/`return` control flow, and calls between `static def` functions.
/// Still no quote/splice values yet — that's a separate follow-up
/// milestone; see the compile-time evaluation design plan.
class evaluator {
public:
  // Seeds `locals_` with one root scope so a top-level `static assert`/
  // `static if` condition (or any other call into `evaluate`/`evaluate_
  // stmt` outside `call_function`, which otherwise always brackets its own
  // work in a `push_locals`/`pop_locals` pair) can safely reach a `match`/
  // `if` expression, `let`, or assignment — every one of those touches
  // `locals_.back()`, which is undefined behavior on an empty stack.
  evaluator(diagnostic_bag &diag, file_id_type file_id)
      : diag_(diag), file_id_(file_id), locals_(1) {}

  /// Updates which file newly reported diagnostics are attributed to. A
  /// single `evaluator` accumulates `static let` globals across an entire
  /// checking session (so cross-file references converge on one evaluated
  /// value regardless of which file's checking reaches them first — see
  /// the design plan's confluence requirement), but each file being
  /// checked needs its own diagnostics attributed to its own `file_id`.
  void set_file(file_id_type file_id) { file_id_ = file_id; }

  /// Evaluates `expr` against the currently-bound `static let` globals,
  /// resolving forward references to not-yet-checked `static let`s and
  /// `static def` calls lazily. Returns `value::make_error()` (already
  /// diagnosed) if `expr` uses a construct this evaluator doesn't support,
  /// or if evaluation hits a genuine runtime-style failure (e.g. division
  /// by zero, unbounded compile-time recursion).
  [[nodiscard]] auto evaluate(const ast::expr &expr) -> value;

  /// Evaluates `expr` exactly like `evaluate`, except a failure (an
  /// unresolved name, an unsupported construct, ...) is reported as
  /// `std::nullopt` instead of a diagnosed `value::make_error()` —
  /// diagnostics are suppressed for the duration of this call, the same way
  /// `try_eval_ordinary_call` suppresses them while speculatively folding a
  /// callee body. For a caller like `checker::resolve_static_if_branch`
  /// that only wants a real decision when the evaluator can actually reach
  /// one, and must otherwise fall back silently (not diagnose) to checking
  /// both branches — evaluating with plain `evaluate` would announce every
  /// such fallback as a spurious error even though "couldn't decide yet" is
  /// the ordinary, expected outcome for a condition that depends on
  /// something the evaluator has no value for at this point (an ordinary
  /// function-local `let`, say, as opposed to a `static let` global).
  [[nodiscard]] auto try_evaluate(const ast::expr &expr)
      -> std::optional<value> {
    const auto saved_suppressed = diagnostics_suppressed_;
    diagnostics_suppressed_ = true;
    auto result = evaluate(expr);
    diagnostics_suppressed_ = saved_suppressed;
    if (result.is_error()) {
      return std::nullopt;
    }
    return result;
  }

  /// Registers a `static let` binding's already-evaluated value so later
  /// `static` expressions in the same session can reference it by name.
  /// Re-binding the same name overwrites the previous value — callers are
  /// responsible for cycle/duplicate-definition diagnostics elsewhere
  /// (mirrors `checker`'s existing `statics_in_progress_` guard).
  void bind_global(const std::string &name, value v);

  /// Whether `name` already has a memoized value in `globals_`, so callers
  /// can avoid re-evaluating (and re-diagnosing) an initializer that a
  /// forward reference already resolved lazily.
  [[nodiscard]] auto has_global(const std::string &name) const -> bool {
    return globals_.contains(name);
  }

  /// The memoized value bound for `name`, or `nullptr` if `name` has no
  /// binding yet — callers that need the actual value (rather than just
  /// "is it bound") after a `has_global` check or their own `bind_global`
  /// call, e.g. `checker::resolve_ident` embedding a scalar `static let`'s
  /// value directly into referencing code.
  [[nodiscard]] auto global_value(const std::string &name) const
      -> const value * {
    const auto it = globals_.find(name);
    return it != globals_.end() ? &it->second : nullptr;
  }

  /// Registers a top-level `static let` for lazy, order-independent
  /// evaluation: if some other compile-time expression references `name`
  /// before `checker` reaches this declaration in file order, the
  /// evaluator evaluates `initializer` itself (attributing diagnostics to
  /// `owner_file`) instead of reporting "not a compile-time constant yet".
  /// Does not evaluate eagerly — evaluation only happens on first
  /// reference, and the result is memoized via `bind_global`.
  void register_pending_static(std::string name, const ast::expr &initializer,
                               file_id_type owner_file);

  /// Registers a top-level `static def` function so calls to `name`
  /// elsewhere in compile-time code can find and invoke it.
  void register_pending_function(std::string name, const ast::func_decl &decl);

  /// Whether `name` names a registered `static def` function — lets a
  /// caller (`checker::resolve_deriving_traits`) check before attempting a
  /// speculative compile-time call, rather than after: `evaluate` reports a
  /// real diagnostic on a failed call (not just a quiet `nullopt`), so
  /// probing via a would-be-error evaluate-and-discard is not an option in
  /// a session that legitimately never registered `std.derive` (e.g. a
  /// narrow test fixture set, or a future compile mode that omits it).
  [[nodiscard]] auto has_pending_function(const std::string &name) const
      -> bool {
    return pending_functions_.contains(name);
  }

  /// Registers a top-level `type` declaration so `T.fields()`/
  /// `.field_count()`/`.name()` (`reflect.cpp`) can resolve `name` to its
  /// declaration independently of `checker`'s own module-scoped lookup —
  /// see `checker::register_comptime_globals`, the only caller.
  void register_pending_type(std::string name, const ast::type_decl &decl);

  /// Pushes the per-field concrete-type-name context for one
  /// `evaluate_derive_call` (`check.cpp`), so a `T.fields()` reflection
  /// evaluated inside that call's `derive_<trait>[...]()` body can report
  /// each field's *instantiated* type (`spec/todo.md` item 5) instead of the
  /// declaration's raw, un-substituted syntax. Keyed by the field's own
  /// `ast::struct_field*` (shared across every instantiation of a generic
  /// struct, since generic structs are never AST-cloned per instantiation)
  /// rather than stored globally, so it must be scoped to exactly the one
  /// `evaluate()` call it was computed for — see `pop_field_type_context`.
  /// A stack (not a single slot) because derivation can recurse: reflecting
  /// on a field may itself need to `ensure_derived_instance_impls` a nested
  /// type's own derived method while the outer context is still active.
  void push_field_type_context(
      std::unordered_map<const ast::struct_field *, std::string> names);

  /// Pops the context pushed by the matching `push_field_type_context` call.
  void pop_field_type_context();

  /// The concrete, instantiation-resolved type name for `field`, if the
  /// innermost active context (pushed by `push_field_type_context`) has one
  /// — `nullptr` when no context is active (e.g. `T.fields()` reflected
  /// outside a derive call) or `field` has no entry (its type failed to
  /// resolve). `try_eval_type_reflection_call` (`reflect.cpp`) falls back to
  /// syntactic rendering when this returns `nullptr`.
  [[nodiscard]] auto
  current_field_type_name(const ast::struct_field &field) const
      -> const std::string *;

  /// One `def`/`type` member a module exposes, for module reflection.
  struct module_member_info {
    std::string name;
    bool is_pub = false;
  };
  /// The reflectable surface of one module: its `def` and `type` members. An
  /// instantiated functor is registered exactly like an ordinary module (its
  /// synthetic name), so `M.functions()`/`M.types()` work uniformly on both.
  struct module_reflection_info {
    std::vector<module_member_info> functions;
    std::vector<module_member_info> types;
  };
  /// Registers a module's reflectable surface so `M.name()`/`M.functions()`/
  /// `M.types()`/`.function_count()`/`.type_count()` (`reflect.cpp`) resolve
  /// `M` to it. Called once per module by `checker::register_comptime_modules`.
  void register_pending_module(std::string name, module_reflection_info info);

  /// Session-wide trait/impl coherence facts, mirrored out of `checker`'s own
  /// `impl_trait_index_`/`validate_impl_coherence` (`check.cpp`) so
  /// `implements[T, Trait]()`/`T.traits()`/`Trait.requires()` (`reflect.cpp`)
  /// can answer without the evaluator ever reaching back into the checker.
  struct impl_coherence_info {
    /// `type_key_of`-keyed (declaration pointer for a user type, name for a
    /// builtin/compound-builtin type — see `check.cpp`'s `type_key_of`) ->
    /// every trait name that type has an impl for. Backs `T.traits()` and
    /// `implements[T, Trait]()`.
    std::unordered_map<std::string, std::vector<std::string>>
        traits_by_type_key;
    /// Trait name -> the supertrait names its own `requires` clause lists,
    /// in source order (`requires a + b` is legal — see `ast::trait_decl::
    /// requires_bound`'s `bound::terms`). Backs `Trait.requires()`.
    std::unordered_map<std::string, std::vector<std::string>> trait_requires;
  };
  /// Registers the coherence facts above. Called once, after
  /// `checker::validate_impl_coherence` has finished building its own
  /// tables and before any file's body (and therefore any `static`/comptime
  /// call) is checked — see `checker::register_comptime_coherence_info`.
  void register_impl_coherence_info(impl_coherence_info info);

  /// A node -> (sum type name, variant name) lookup, backed by `checker`'s
  /// own already-resolved `node_types_` (see `checker::resolve_variant_tag`,
  /// the only real implementation). The evaluator needs this because the
  /// parser drops the leading `@` from a variant constructor entirely —
  /// `@unix` and a plain identifier `unix` produce the exact same
  /// `ident_expr` node — so telling them apart requires the type checker's
  /// resolution, not anything recoverable from syntax alone.
  using variant_resolver_fn =
      std::function<std::optional<std::pair<std::string, std::string>>(
          const ast::node &)>;

  /// Installs the checker's variant-resolution callback (see
  /// `variant_resolver_fn`). Unset by default, in which case `eval_ident`/
  /// `eval_call` never attempt variant construction — every existing
  /// standalone use of `evaluator` (tests, anything not wired to a live
  /// `checker`) keeps working unchanged.
  void set_variant_resolver(variant_resolver_fn resolver) {
    variant_resolver_ = std::move(resolver);
  }

  /// What a dotted path `a.b.c` means, as the checker reads it for the
  /// evaluator — see `set_path_resolver`.
  struct dotted_path_reading {
    enum class kind : std::uint8_t {
      /// `a` is a binding in the evaluator's own frames: field access on it.
      local,
      /// The path names a module-level `static`, whose value is `base`;
      /// segments from `first_field` on are field access on it.
      static_value,
      /// The path names a module-level function, `function`.
      function,
      /// The path names nothing with a compile-time value; `reason` says
      /// why, or is empty when the cause was already reported.
      not_constant,
    };
    kind reading = kind::local;
    value base;
    size_t first_field = 1;
    std::string reason;
    const ast::func_decl *function = nullptr;
  };

  /// Reads a dotted path. `root_is_local` is whether the first segment is a
  /// binding in the evaluator's frames (a `static def` parameter, a `let` in
  /// its body, a `static for` binder) — the one input only the evaluator
  /// has. Everything else about what the path means is the checker's rule,
  /// so compile-time code and ordinary code can never disagree.
  using path_resolver_fn = std::function<dotted_path_reading(
      const ast::module_path_expr &path, bool root_is_local)>;

  /// Installs the checker's dotted-path reader (see `path_resolver_fn`).
  /// Unset, there is no module system to consult (a standalone evaluator,
  /// or the driver's pre-resolution `static if` fold), so no root can name
  /// a module and every path is field access on a name.
  void set_path_resolver(path_resolver_fn resolver) {
    path_resolver_ = std::move(resolver);
  }

  /// What a bare name that is not one of the evaluator's own frame locals
  /// means, as the checker reads it — see `set_name_resolver`.
  struct bare_name_reading {
    enum class kind : std::uint8_t {
      /// A module-level `static`, whose value is `base`.
      static_value,
      /// A module-level function, `function`.
      function,
      /// A module-level name with no compile-time value; `reason` says why,
      /// or is empty when the cause was already reported.
      not_constant,
      /// No module-level value of that name is visible here.
      not_found,
    };
    kind reading = kind::not_found;
    value base;
    const ast::func_decl *function = nullptr;
    std::string reason;
  };

  /// Reads a bare name that is not a frame local. The checker answers with
  /// the same module-scope lookup ordinary code gets (the module being
  /// checked, then its imports), so two modules that each declare
  /// `static limit` or `static def f` never meet.
  using name_resolver_fn =
      std::function<bare_name_reading(const std::string &name)>;

  /// Installs the checker's bare-name reader (see `name_resolver_fn`).
  /// Unset, bare names resolve against the evaluator's own session-wide
  /// tables (`register_pending_static`/`register_pending_function`/
  /// `bind_global`) — a standalone evaluator with no module system.
  void set_name_resolver(name_resolver_fn resolver) {
    name_resolver_ = std::move(resolver);
  }

  /// Runs `body` (a compile-time call into `fn`) in `fn`'s declaring
  /// module's context, so names inside it read the way they would at the
  /// declaration rather than at the call site — see `set_call_context`.
  using call_context_fn = std::function<value(
      const ast::func_decl &fn, const std::function<value()> &body)>;

  /// Installs the checker's call-context switch (see `call_context_fn`).
  /// Unset, a call runs in whatever context the caller had.
  void set_call_context(call_context_fn context) {
    call_context_ = std::move(context);
  }

  /// The method `name` a compile-time `receiver` value has, as the checker
  /// finds it for the receiver's type (`receiver.name(...)`). The checker
  /// reads that type from `receiver_expr`, the expression the value came
  /// from, since a compile-time value does not carry its exact type (an
  /// integer is just an integer). The error explains why there is no method
  /// to call: the type has none of that name, or its type is not known here.
  using method_resolver_fn =
      std::function<std::expected<const ast::func_decl *, std::string>(
          const ast::expr &receiver_expr, const value &receiver,
          const std::string &name)>;

  /// Installs the checker's method lookup (see `method_resolver_fn`). Unset,
  /// only the evaluator's own built-in methods (`str.len()`) are callable.
  void set_method_resolver(method_resolver_fn resolver) {
    method_resolver_ = std::move(resolver);
  }

  /// The type arguments the checker solved for the generic call `call`
  /// (`measure(box3 { side: 2 })` binds `T` to `box3`), as `type_value`s
  /// keyed by type-parameter name; `nullopt` when the checker recorded no
  /// solution for it. An implicit generic call names no type arguments for
  /// the evaluator to read, and an explicit one names them only by
  /// spelling, without the exact type the checker solved.
  using generic_call_resolver_fn =
      std::function<std::optional<std::vector<std::pair<std::string, value>>>(
          const ast::call_expr &call)>;

  /// Installs the checker's generic-call lookup (see
  /// `generic_call_resolver_fn`). Unset, an implicit generic call binds no
  /// type arguments.
  void set_generic_call_resolver(generic_call_resolver_fn resolver) {
    generic_call_resolver_ = std::move(resolver);
  }

  /// What happened when the VM was asked to run a compile-time call — see
  /// `set_vm_call`.
  struct vm_call_outcome {
    enum class kind : std::uint8_t {
      /// The VM cannot run this call; interpret it instead.
      not_run,
      /// The call ran; `result` is its value.
      returned,
      /// The call ran and failed; `message` says how and `notes` where.
      failed,
    };
    kind outcome = kind::not_run;
    value result;
    std::string message;
    std::vector<diagnostic> notes;
  };

  /// Runs a call to `fn` with `args` (already in parameter order, every
  /// parameter supplied) on the VM. `type_args` binds a generic `fn`'s type
  /// parameters, by name, to `type_value`s; the VM runs the instance for
  /// them.
  using vm_call_fn = std::function<vm_call_outcome(
      const ast::func_decl &fn, std::span<const value> args,
      std::span<const std::pair<std::string, value>> type_args)>;

  /// How compile-time calls use the VM (`set_vm_call`).
  enum class vm_mode : std::uint8_t {
    /// Every call is interpreted.
    off,
    /// A call the VM can run runs there; any other is interpreted.
    on,
    /// A call the VM can run runs both ways, and a difference between the
    /// two results is reported as an error: different values, or a failure
    /// on the VM only. The interpreter's result is used when it has one.
    /// For checking the move to the VM, not for ordinary builds.
    compare,
  };

  /// Installs the VM. Unset (or `vm_mode::off`), every call is interpreted.
  void set_vm_call(vm_call_fn call, vm_mode mode) {
    vm_call_ = std::move(call);
    vm_mode_ = mode;
  }

  /// Runs a whole compile-time expression on the VM, reading the bindings
  /// it uses from `local_value`. A `failed` outcome's `message` is the
  /// whole error.
  using vm_eval_fn = std::function<vm_call_outcome(const ast::expr &expr)>;

  /// Installs the VM for whole expressions (`evaluate`, `try_evaluate`).
  /// Unset, they are interpreted. `set_vm_call`'s mode applies.
  void set_vm_eval(vm_eval_fn eval) { vm_eval_ = std::move(eval); }

  /// The value a binding the evaluator holds has — a `static for` loop
  /// variable, a type parameter — or `nullptr` if it holds none of that
  /// name.
  [[nodiscard]] auto local_value(const std::string &name) -> const value * {
    return lookup_local(name);
  }

  /// The builder quoted syntax comes from, shared with compile-time code
  /// the VM runs so both build the same syntax.
  [[nodiscard]] auto fragments() -> fragment_builder & { return fragments_; }

  /// Whether diagnostics are being suppressed (`try_evaluate`).
  [[nodiscard]] auto silent() const -> bool { return diagnostics_suppressed_; }

  /// Suppresses diagnostics (`on`) or not, returning whether they were, so
  /// a caller can restore that.
  auto set_silent(bool on) -> bool {
    return std::exchange(diagnostics_suppressed_, on);
  }

  /// `try_evaluate` for a guess: whether an ordinary `let`'s initializer
  /// happens to be a compile-time constant. Never runs on the VM, which
  /// would compile and run every such initializer.
  [[nodiscard]] auto probe(const ast::expr &expr) -> std::optional<value> {
    const auto saved = std::exchange(probing_, true);
    auto result = try_evaluate(expr);
    probing_ = saved;
    return result;
  }

  /// The type the current call's type parameter `name` is bound to, or
  /// `nullptr` when `name` is not a type parameter bound in this frame.
  /// Calls `fn` with no value arguments and `type_args` bound to its type
  /// parameters, as the call `fn[types]()` would.
  [[nodiscard]] auto
  call_with_types(const ast::func_decl &fn, source_span span,
                  std::vector<std::pair<std::string, value>> type_args)
      -> value {
    return call_function(fn, fn.name, {}, span, std::move(type_args));
  }
  [[nodiscard]] auto type_binding(const std::string &name) -> const value * {
    const auto *found = lookup_local(name);
    return found != nullptr && found->kind == value_kind::type_value ? found
                                                                     : nullptr;
  }

  /// The registered `static def` named `name`, if any — a session-wide
  /// lookup for a caller that synthesizes a call to a known library
  /// function (`checker::evaluate_derive_call`) and needs its declaration.
  [[nodiscard]] auto pending_function(const std::string &name) const
      -> const ast::func_decl * {
    const auto it = pending_functions_.find(name);
    return it != pending_functions_.end() ? it->second : nullptr;
  }

  /// Evaluates `iterable` in `static for` position to a `list` value.
  /// Supports list/array literals and integer ranges (`a..b`, `a..=b`);
  /// anything else reports "not yet supported" and returns an error.
  [[nodiscard]] auto evaluate_iterable(const ast::expr &iterable) -> value;

  /// Binds `pattern` against `v` into `scope`, reporting a diagnostic and
  /// returning `false` on a shape mismatch or unsupported pattern form.
  [[nodiscard]] auto bind_pattern(const ast::pattern &pattern, const value &v,
                                  std::unordered_map<std::string, value> &scope)
      -> bool;

  /// Pushes/pops one local-variable scope frame, used by callers (e.g.
  /// `static for` loop bodies) that need to bind loop variables around a
  /// sequence of statements evaluated through `evaluate_stmts`.
  void push_locals(std::unordered_map<std::string, value> scope);
  void pop_locals();

  /// Adds one binding into the current (innermost) locals frame, for a
  /// caller (`checker::check_stmt`'s `let_stmt` case) that discovers a
  /// comptime-evaluable value one statement at a time — e.g. `let n =
  /// T.name()` inside a bound generic instance — rather than having every
  /// binding for a scope ready up front the way `push_locals` expects.
  /// Overwrites any existing binding for `name` in that frame, matching
  /// `push_locals`' own "re-binding overwrites" behavior.
  void bind_local(const std::string &name, value v) {
    locals_.back().insert_or_assign(name, std::move(v));
  }

  /// Outcome of executing a statement sequence: whether a `return` was hit
  /// and, if so, its value, or whether a `break`/`continue` is unwinding to
  /// the innermost enclosing loop.
  struct exec_result {
    bool returned = false;
    bool errored = false;
    bool broke = false;
    bool continued = false;
    value result = value::make_unit();

    /// True when the rest of the enclosing statement sequence must not run.
    [[nodiscard]] auto interrupts() const -> bool {
      return returned || errored || broke || continued;
    }
  };

  /// Executes a statement/body-node sequence against the current locals
  /// scope, short-circuiting on `return` or on the first unsupported/
  /// erroring construct.
  [[nodiscard]] auto
  evaluate_stmts(const std::vector<ast::ptr<ast::node>> &body) -> exec_result;

  /// Moves out every fragment `fragments_` built so it outlives this
  /// evaluator — `checker::take_checked_types` calls this and stores the
  /// result in `checked_types`, since `hir::lower` reads spliced fragments
  /// (via `checked_types::spliced_fragments`) after the `checker`/
  /// `evaluator` that produced them has already been destroyed.
  [[nodiscard]] auto take_synthesized_fragments() -> ast::ptr_vec<ast::node> {
    return fragments_.take_fragments();
  }

  /// Attempts to evaluate a whole call to `fn` (a registered `static def`)
  /// on behalf of an *ordinary*, non-`static` call site — see todo item 8
  /// (a call to a `static def` from ordinary code never lowers). This runs
  /// exactly the same interpretation `eval_call` already gives a call
  /// reached from `static assert`/`static if`/another `static def`'s body,
  /// but silently: `checker` calls this speculatively, once per ordinary
  /// call site of a comptime-only generic instance, and most such call
  /// sites are not actually compile-time evaluable (an argument may be an
  /// ordinary runtime value). Unlike `evaluate`, which always reports a
  /// diagnostic on failure, this suppresses every diagnostic `evaluate`/
  /// `call_function` would have emitted along the way and returns
  /// `std::nullopt` instead, leaving the call site free to fall back to an
  /// ordinary (unfolded) function call with no visible side effect.
  /// `type_args` binds `fn`'s own type parameters (`T` in `is_bool[T]()`) to
  /// the concrete types this call's monomorphization solved them to — the
  /// same `{name, type_value}` shape `checker::check_function` pushes for a
  /// comptime-only instance while it is being *checked* (see that function's
  /// `type_param_locals`). That binding lives only for the duration of the
  /// check and is gone by the time an unrelated, later ordinary call site
  /// asks to fold the same instance, so this call has to re-supply it:
  /// without `type_args`, `T.name()`/`T.kind()` inside `fn`'s body have
  /// nothing to resolve against and every such call reports "`T` does not
  /// name a known type here" rather than folding.
  [[nodiscard]] auto try_eval_ordinary_call(
      const ast::func_decl &fn, const ast::call_expr &call,
      std::vector<std::pair<std::string, value>> type_args = {})
      -> std::optional<value>;

private:
  /// `evaluate` without its tracing.
  [[nodiscard]] auto evaluate_unwrapped(const ast::expr &expr) -> value;
  [[nodiscard]] auto eval_binary(const ast::binary_expr &bin) -> value;
  [[nodiscard]] auto eval_unary(const ast::unary_expr &un) -> value;
  [[nodiscard]] auto eval_literal(const ast::literal_expr &lit) -> value;
  [[nodiscard]] auto eval_ident(const ast::ident_expr &ident) -> value;
  [[nodiscard]] auto eval_array(const ast::array_expr &arr) -> value;
  [[nodiscard]] auto eval_struct(const ast::struct_expr &st) -> value;
  [[nodiscard]] auto eval_field(const ast::field_expr &fld) -> value;
  [[nodiscard]] auto eval_index(const ast::index_expr &idx) -> value;
  [[nodiscard]] auto eval_call(const ast::call_expr &call) -> value;
  [[nodiscard]] auto eval_tuple(const ast::tuple_expr &tup) -> value;
  [[nodiscard]] auto eval_module_path(const ast::module_path_expr &path)
      -> value;
  [[nodiscard]] auto eval_quote(const ast::quote_expr &quote) -> value;
  [[nodiscard]] auto
  eval_interpolated_string(const ast::interpolated_string_expr &interp)
      -> value;
  /// `match subject: ...` used in expression position (e.g. `return match
  /// ...:`), as opposed to `evaluate_stmt`'s `match_stmt` case for one used
  /// as a bare statement. Shares that case's arm-selection logic but always
  /// produces a `value` — a compact `=> expr` arm evaluates directly, a
  /// block-form arm runs via `evaluate_block_value` (so a `return` or a
  /// bare tail expression both work) and reports an error only if the
  /// block falls through with neither, since an expression position always
  /// needs a value.
  [[nodiscard]] auto eval_match(const ast::match_expr &match) -> value;
  /// `if cond: ... elif ...: ... else: ...` used in expression position
  /// (e.g. `return if ...: ... else: ...`) — the expression-position
  /// counterpart of `evaluate_stmt`'s `if_stmt` case, mirroring
  /// `eval_match`'s block-body handling.
  [[nodiscard]] auto eval_if(const ast::if_expr &if_expr_node) -> value;

  /// `<expr> as <type>` — numeric/bool/char scalar conversion. `target_type`
  /// is resolved the same way a generic type argument is (`resolve_generic_
  /// type_arg`'s bound-local-then-builtin-name path), so a `static def[T]`
  /// body can write `v as T` with `T` bound to whatever scalar the caller
  /// instantiated it at. Anything else (a compound/user type, or a name that
  /// doesn't resolve at all) reports a specific "cast to '<name>' is not
  /// supported" diagnostic rather than the generic unsupported-expression
  /// fallback.
  [[nodiscard]] auto eval_cast(const ast::cast_expr &cast) -> value;

  /// Recognizes `expr.lit(...)`/`expr.ident(...)` — the AST-builder
  /// intrinsics that construct a new `expr` quote-value programmatically
  /// (as opposed to capturing existing syntax via a backtick quote). `expr`
  /// here is a contextual pseudo-namespace, not a real bound value (nothing
  /// ever binds the name `expr` itself); `field` must be a direct call
  /// whose callee is a `field_expr` with an `ident_expr` object literally
  /// spelled `expr`. Returns `nullopt` if `call` doesn't match this shape
  /// at all, so `eval_call` can fall through to its ordinary `static def`
  /// dispatch.
  [[nodiscard]] auto try_eval_expr_builder_call(const ast::call_expr &call)
      -> std::optional<value>;

  /// Recognizes `T.fields()`/`T.field_count()`/`T.name()` — compile-time
  /// reflection over a registered `type` declaration's own syntax (design
  /// plan section 4; implemented in `reflect.cpp`). `T` must be an
  /// `ident_expr` naming an entry in `pending_types_`. Returns `nullopt`
  /// if `call` doesn't match this shape at all (including "names a type,
  /// but not one of the three recognized calls"), so `eval_call` can fall
  /// through to its ordinary dispatch.
  [[nodiscard]] auto try_eval_type_reflection_call(const ast::call_expr &call)
      -> std::optional<value>;

  /// Recognizes `M.name()`/`M.functions()`/`M.types()`/`.function_count()`/
  /// `.type_count()` — compile-time reflection over a registered module's
  /// surface (`register_pending_module`). `M` must be an `ident_expr` naming
  /// an entry in `pending_modules_`. Returns `nullopt` if `call` doesn't match
  /// this shape at all, so `eval_call` can fall through to its ordinary
  /// dispatch (and to type reflection, which is tried first).
  [[nodiscard]] auto try_eval_module_reflection_call(const ast::call_expr &call)
      -> std::optional<value>;

  /// Recognizes `name[A]`/`name[A, B]`/...`(...)` — a compile-time generic
  /// call to a `static def` function with one or more type parameters (e.g.
  /// `derive_show[point]()`, `is_same[A, B]()`). `name` must be in
  /// `pending_functions_` with at least as many type parameters as type
  /// arguments given; each type argument must resolve via
  /// `resolve_generic_type_arg` (a real declared type, a known builtin
  /// scalar name, or — for a nested generic call forwarding its own type
  /// parameter — an already-bound `type_value` local). Returns `nullopt` if
  /// the shape doesn't match at all, so `eval_call` can fall through to its
  /// ordinary direct-call dispatch (which itself will reject an
  /// `index_expr`/nested-`call_expr` callee).
  [[nodiscard]] auto try_eval_comptime_generic_call(const ast::call_expr &call)
      -> std::optional<value>;

  /// Recognizes `implements[T, Trait]()` — whether `T` has an impl of the
  /// kind-`*` trait named by `Trait` (a query against the coherence facts
  /// `register_impl_coherence_info` installed). Both arguments must be
  /// plain type-name identifiers, resolved the same way
  /// `try_eval_comptime_generic_call`'s type arguments are; `Trait` is
  /// looked up by name only (traits aren't registered the way types are, so
  /// there is no declaration to resolve it against — the coherence table is
  /// itself keyed by trait *name*, which is all a kind-`*` trait check
  /// needs). Returns `nullopt` if the shape doesn't match `implements[..](
  /// )` at all.
  [[nodiscard]] auto try_eval_implements_call(const ast::call_expr &call)
      -> std::optional<value>;

  /// Recognizes `T.traits()` (every trait name `T` has an impl for) and
  /// `Trait.requires()` (the trait's own `requires`-supertrait name, if
  /// any, as a 0-or-1-element list) — the two coherence-table-backed
  /// meta-queries (`spec/specification/04-stdlib/type-traits/60-meta-
  /// queries.md`). `object` must be a plain identifier; `T.traits()` first
  /// tries `resolve_generic_type_arg`'s type-key (so it works for both a
  /// user type and a builtin/bound type parameter), falling back to
  /// treating the identifier itself as a trait name for `.requires()`
  /// (traits have no separate registration table to check membership
  /// against first). Returns `nullopt` if the shape doesn't match at all.
  [[nodiscard]] auto try_eval_trait_reflection_call(const ast::call_expr &call)
      -> std::optional<value>;

  /// Resolves `ident` to a registered `type` declaration for reflection/
  /// generic-call purposes: first checks whether `ident` names a local
  /// bound to a `type_value` (a generic parameter received by the enclosing
  /// `static def`, e.g. `T`), then falls back to `pending_types_`-by-
  /// literal-name (an ordinary top-level type name, e.g. `point`). Returns
  /// `nullptr` if neither resolves — in particular, for a `type_value` local
  /// bound to a builtin/compound type (no declaration), and for a plain
  /// builtin type name written literally; callers that only need the
  /// *name* (`T.name()`, `T.kind()`'s scalar case, generic-call type-
  /// argument binding) should use `resolve_generic_type_arg` instead, which
  /// covers those cases too.
  [[nodiscard]] auto resolve_type_reference(const ast::ident_expr &ident)
      -> const ast::type_decl *;

  /// Resolves a type argument written as a plain identifier — `T` forwarded
  /// from an enclosing generic parameter, a real declared type's name, or a
  /// literal builtin scalar name (`int32`, `bool`, ...) — to a `type_value`
  /// (`comptime::value`) carrying its name and, when there is one, its
  /// declaration. This is the one place that closes the gap `resolve_type_
  /// reference` deliberately leaves open: a builtin scalar has no `type_
  /// decl`, but still has a name every predicate (`T.name()`, `T.kind()`'s
  /// scalar classification, `is_integer[T]`'s own `T.name()` dispatch)
  /// needs to see. Returns `nullopt` if `ident` names none of the above.
  [[nodiscard]] auto resolve_generic_type_arg(const ast::ident_expr &ident)
      -> std::optional<value>;

  /// Unwraps the two AST shapes an explicit multi-type-argument call can
  /// take — `name[A]` (an `index_expr` callee, one argument) and
  /// `name[A, B, ...]` (a `call_expr` callee whose own `args` are the type
  /// arguments — see the parser's `parse_postfix`, which can't yet tell
  /// `values[0]` from `zeros[8]` and produces a plain `call_expr` for any
  /// bracket group that isn't exactly one unnamed argument) — mirroring
  /// `checker::explicit_generic_callee` (`check.cpp`) on the evaluator
  /// side. Appends each type-argument expression (in order) to `args_out`
  /// and returns the real base callee expression, or `nullptr` if `callee`
  /// is neither shape (a named bracket argument counts as neither, since
  /// that spells something else entirely at this position).
  [[nodiscard]] auto
  unwrap_explicit_generic_callee(const ast::expr &callee,
                                 std::vector<const ast::expr *> &args_out)
      -> const ast::expr *;

  /// Asks `variant_resolver_` (if installed) whether `node` is a resolved
  /// variant constructor, then finds the matching `sum_variant` inside
  /// `pending_types_[type_name]` — the `type` declaration must have been
  /// registered via `register_pending_type` for this to succeed, mirroring
  /// the same "must be compile-time visible" requirement
  /// `resolve_type_reference` already imposes for reflection targets. Returns
  /// `nullopt` if no resolver is installed, `node` doesn't resolve to a
  /// sum-typed variant, or the resolved type was never registered. The
  /// builtin `option`/`result` have no declaration and are taken as the
  /// checker resolved them. Answers the (type name, variant tag) pair.
  [[nodiscard]] auto resolve_variant(const ast::node &node)
      -> std::optional<std::pair<std::string, std::string>>;

  /// Last-resort fallback for `eval_ident` when `resolve_variant` comes back
  /// empty *and* `name` isn't bound as a local/global/pending static/pending
  /// function either: searches every `pending_types_` entry for a sum type
  /// declaring a variant named `name` and, if exactly one type has one,
  /// treats the identifier as that variant constructor.
  ///
  /// `resolve_variant`'s checker-backed path answers precisely from
  /// `node_types_`, but that table is only populated once the checker has
  /// actually type-checked the expression's enclosing function body — and a
  /// type-generic `static def`'s *template* is checked exactly once, on
  /// whatever turn the whole-session file loop reaches its declaring file.
  /// A call folded from an *earlier-processed* file (e.g. a user's own
  /// source, checked before the injected stdlib prelude that follows it —
  /// see `driver::inject_stdlib_prelude`) into a not-yet-checked template
  /// whose body constructs a bare `@variant` (`return @signed_integer`,
  /// `std.traits.category`'s `type_category`) finds no entry there at all.
  /// Every `type` declaration, in contrast, is registered into
  /// `pending_types_` order-independently, in a pre-pass over every input
  /// file before any body is checked (`checker::register_comptime_globals`),
  /// so resolving straight from the sum type's own variant list sidesteps
  /// the ordering dependency entirely. Ambiguous only in principle (two
  /// unrelated sum types sharing a variant spelling): whichever match is
  /// found first wins, which is harmless here since every consumer of the
  /// resulting `value` (`bind_pattern`'s `constructor_pattern` case) matches
  /// by variant tag alone, never by owning type identity. The builtin
  /// `option`/`result` variants (`some`, `none`, `ok`, `err`) answer without
  /// a declaration.
  [[nodiscard]] auto resolve_variant_by_name(const std::string &name)
      -> std::optional<std::pair<std::string, std::string>>;

  [[nodiscard]] auto
  call_function(const ast::func_decl &fn, const std::string &name,
                std::vector<value> args, source_span span,
                std::vector<std::pair<std::string, value>> type_args = {})
      -> value;

  /// `call_function`'s interpreted path: runs `fn`'s body in this evaluator.
  [[nodiscard]] auto
  interpret_function(const ast::func_decl &fn, const std::string &name,
                     std::vector<value> args, source_span span,
                     std::vector<std::pair<std::string, value>> type_args)
      -> value;

  /// A lambda expression: captures the visible locals of the current frame
  /// (or only those its explicit capture list names).
  [[nodiscard]] auto eval_lambda(const ast::lambda_expr &lambda) -> value;

  /// Calls a `value_kind::lambda` value with `args`, in a fresh frame
  /// holding its captures and parameters.
  [[nodiscard]] auto call_lambda(const value &callee, std::vector<value> args,
                                 source_span span) -> value;

  /// `xs.push(x)` on a local `var` list: appends in place. `nullopt` for any
  /// other call, which is then an ordinary method call.
  [[nodiscard]] auto eval_local_push(const ast::call_expr &call,
                                     const ast::field_expr &method)
      -> std::optional<value>;

  /// `receiver.name(args...)`: a built-in method on the value, a closure
  /// stored in a struct field of that name, or the method the checker finds
  /// for the receiver's type, called with the receiver as `self`.
  [[nodiscard]] auto eval_method_call(const ast::call_expr &call,
                                      const ast::field_expr &method,
                                      const value &receiver) -> value;

  /// Runs an `intrinsic def` the evaluator implements natively (the
  /// `rt_str_*` primitives `std.string` is built from), with the same
  /// `src/runtime/string_ops.h` code both backends use.
  [[nodiscard]] auto call_intrinsic(const ast::func_decl &fn,
                                    std::span<const value> args,
                                    source_span span) -> value;

  [[nodiscard]] auto evaluate_stmt(const ast::node &node) -> exec_result;

  /// Runs a `while`/`while let` loop. Each iteration's body gets its own
  /// locals frame, so a `let` inside it does not outlive the iteration.
  [[nodiscard]] auto evaluate_while(const ast::while_stmt &loop) -> exec_result;

  /// Runs a `for` loop over a compile-time range or list.
  [[nodiscard]] auto evaluate_for(const ast::for_stmt &loop) -> exec_result;

  /// Unrolls a `static for` (inline or block form) nested in a `static def`
  /// body. For the inline form, each yielded value is appended to `yields`
  /// when it is non-null, so a tail `static for ... => e` can become the
  /// list it evaluates to.
  [[nodiscard]] auto evaluate_static_for(const ast::static_decl &decl,
                                         std::vector<value> *yields)
      -> exec_result;

  /// Runs one loop iteration's body and folds its `break`/`continue` into
  /// the loop. Returns the result to propagate out of the loop, or nullopt
  /// to keep looping; sets `stop` when a `break` ends the loop normally.
  [[nodiscard]] auto run_loop_body(const std::vector<ast::ptr<ast::node>> &body,
                                   std::unordered_map<std::string, value> scope,
                                   bool &stop) -> std::optional<exec_result>;

  /// Counts one loop iteration against `k_max_loop_iterations`, reporting
  /// at `span` once the budget is exhausted.
  [[nodiscard]] auto count_loop_iteration(std::size_t &iterations,
                                          source_span span) -> bool;

  /// Applies a binary operator to two already-evaluated operands. Shared by
  /// `eval_binary` and compound assignment (`x += 1`).
  [[nodiscard]] auto apply_binary(ast::binary_op op, const value &lhs,
                                  const value &rhs, source_span span) -> value;

  /// Executes one statement/body-node known to sit in *tail position* of a
  /// block being evaluated for its value (`evaluate_block_value`'s last
  /// non-error item), producing that block's value even when it never
  /// explicitly `return`s — mirroring `checker::node_provides_function_
  /// value`'s "a tail expression/nested if/nested match implicitly provides
  /// the enclosing block's value" rule on the evaluator side. An
  /// `expr_stmt` becomes the value directly; a tail `if_stmt`/`match_stmt`/
  /// `static if` recurses into whichever branch or arm was taken via
  /// `evaluate_block_value`; anything else (in particular `return_stmt`)
  /// fall through to the ordinary `evaluate_stmt`.
  [[nodiscard]] auto evaluate_tail(const ast::node &node) -> exec_result;

  /// Executes `body` as a value-producing block: every statement but the
  /// last runs through `evaluate_stmt` exactly like `evaluate_stmts`, and
  /// the last (skipping trailing `nullptr`/already-errored items) runs
  /// through `evaluate_tail` so a bare tail expression — or a nested `if`/
  /// `match`/`static if` whose own arms end in one — becomes the block's
  /// value without requiring an explicit `return`. Used wherever a block is
  /// reached in expression position: `eval_if`/`eval_match`'s block-form
  /// arms, `evaluate_tail`'s own recursive cases, and (via `call_function`)
  /// a `static def` body itself.
  [[nodiscard]] auto
  evaluate_block_value(const std::vector<ast::ptr<ast::node>> &body)
      -> exec_result;

  /// Looks up `name` in the local-scope stack (innermost first), down to
  /// the current call's own frames — a `static def` body never sees its
  /// caller's locals.
  [[nodiscard]] auto lookup_local(const std::string &name) -> const value *;

  /// Reads a bare name that is not a frame local: through `name_resolver_`
  /// when the checker installed one, otherwise against the evaluator's own
  /// session-wide tables. Reports nothing, except what evaluating a
  /// not-yet-evaluated `static` initializer reports itself.
  [[nodiscard]] auto read_module_name(const std::string &name, source_span span)
      -> bare_name_reading;

  /// Resolves a bare name (a frame local, then `read_module_name`),
  /// reporting "not a compile-time constant" if none match. Also reads the
  /// root of a dotted path the checker classified as a local
  /// (`eval_module_path`).
  [[nodiscard]] auto resolve_name(const std::string &name, source_span span)
      -> value;

  /// `callee` as a dotted path when it is a call's `a.f`/`a.b.f` shape — a
  /// field access on a name or path — so the checker can read it
  /// (`path_resolver_`); `nullopt` for any other callee.
  [[nodiscard]] static auto qualified_callee_path(const ast::expr &callee)
      -> std::optional<ast::module_path_expr>;

  /// `callee` as the `receiver.method` of a method call, or `nullptr` when
  /// it is not a field access or the checker reads it as a dotted path to a
  /// module-level function.
  [[nodiscard]] auto method_callee(const ast::expr &callee)
      -> const ast::field_expr *;

  /// The function a callee name `name` calls: a frame-local closure, else a
  /// module-level function (or a `static` bound to a closure). `nullptr` if
  /// it names neither.
  [[nodiscard]] auto resolve_callee(const std::string &name, source_span span)
      -> const ast::func_decl *;

  /// Resolves a not-yet-bound global by evaluating its pending `static
  /// let` initializer, with cycle detection.
  [[nodiscard]] auto resolve_pending_static(const std::string &name,
                                            source_span span) -> const value *;

  /// Reports an evaluation-time diagnostic and returns the error sentinel,
  /// so call sites can `return report(...)` in one expression.
  auto report(source_span span, std::string message) -> value;

  diagnostic_bag &diag_;
  file_id_type file_id_;
  std::unordered_map<std::string, value> globals_;

  struct pending_static {
    const ast::expr *initializer = nullptr;
    file_id_type owner_file{};
  };
  std::unordered_map<std::string, pending_static> pending_statics_;
  std::unordered_set<std::string> statics_in_progress_;
  std::unordered_map<std::string, const ast::func_decl *> pending_functions_;
  /// Every registered `type` declaration, by name — see
  /// `register_pending_type` and `try_eval_type_reflection_call`.
  std::unordered_map<std::string, const ast::type_decl *> pending_types_;
  /// Every registered module's reflectable surface, by name — see
  /// `register_pending_module` and `try_eval_module_reflection_call`.
  std::unordered_map<std::string, module_reflection_info> pending_modules_;
  /// See `register_impl_coherence_info`; empty (both maps) until `checker`
  /// installs it, in which case `implements[..]()`/`T.traits()`/`Trait.
  /// requires()` simply find nothing rather than crashing.
  impl_coherence_info coherence_info_;

  /// See `set_variant_resolver`; unset (empty `std::function`) until
  /// `checker` installs it.
  variant_resolver_fn variant_resolver_;

  /// See `set_path_resolver`; unset until `checker` installs it.
  path_resolver_fn path_resolver_;

  /// See `set_name_resolver`; unset until `checker` installs it.
  name_resolver_fn name_resolver_;

  /// See `set_call_context`; unset until `checker` installs it.
  call_context_fn call_context_;

  /// See `set_method_resolver`; unset until `checker` installs it.
  method_resolver_fn method_resolver_;

  /// See `set_generic_call_resolver`; unset until `checker` installs it.
  generic_call_resolver_fn generic_call_resolver_;

  /// See `set_vm_call`.
  vm_call_fn vm_call_;
  vm_mode vm_mode_ = vm_mode::off;

  /// Runs `fn` on the VM if it can (`vm_call_`), returning `nullopt` when it
  /// cannot and the call must be interpreted. In `vm_mode::compare`,
  /// `interpret` runs it the other way too and the two are compared.
  [[nodiscard]] auto
  call_on_vm(const ast::func_decl &fn, const std::string &name,
             std::span<const value> args,
             std::span<const std::pair<std::string, value>> type_args,
             source_span span, const std::function<value()> &interpret)
      -> std::optional<value>;

  /// Uses what the VM did: its value, or its failure reported at `span`;
  /// `nullopt` when it did not run. In `vm_mode::compare`, `interpret`
  /// runs the same thing in the interpreter and a difference is reported,
  /// naming `what` ran.
  [[nodiscard]] auto use_vm_outcome(vm_call_outcome outcome,
                                    std::string_view what, source_span span,
                                    const std::function<value()> &interpret)
      -> std::optional<value>;

  /// See `set_vm_eval`.
  vm_eval_fn vm_eval_;
  /// While set, `evaluate` does not use `vm_eval_` (see `probe`).
  bool probing_ = false;

  /// Reports a failed VM call at `span`, with `notes` saying where in the
  /// callee it failed.
  auto report_vm_failure(source_span span, std::string message,
                         std::vector<diagnostic> notes) -> value;

  std::vector<std::unordered_map<std::string, value>> locals_;
  /// Index of the innermost call's first frame in `locals_` — the floor
  /// `lookup_local` and assignment search down to.
  size_t frame_base_ = 0;
  /// The `def` whose body is running (null at module scope); a lambda
  /// records it so its body later resolves names in that `def`'s module.
  const ast::func_decl *current_function_ = nullptr;
  /// One compile-time call in progress: where it was written and what it
  /// called.
  struct call_site {
    file_id_type file{};
    source_span span;
    std::string callee;
  };
  /// The calls in progress, outermost first. An error inside a file the
  /// evaluation did not start in gets a note at the innermost of these
  /// written in the file it did start in — the user's own call.
  std::vector<call_site> call_sites_;
  int call_depth_ = 0;
  /// Nesting of `evaluate` calls, so tracing names only the outermost.
  int eval_depth_ = 0;
  /// Iterations one compile-time loop may run before evaluation gives up,
  /// so a loop that never ends reports an error instead of hanging the
  /// compiler.
  static constexpr std::size_t k_max_loop_iterations = 1'000'000;

  /// Builds every fragment quotes and the `expr.*` builders evaluate to,
  /// and owns them (`take_synthesized_fragments`).
  fragment_builder fragments_;

  /// While set, `report` returns the error sentinel without emitting into
  /// `diag_` — see `try_eval_ordinary_call`, the only setter.
  bool diagnostics_suppressed_ = false;

  /// See `push_field_type_context`/`current_field_type_name`.
  std::vector<std::unordered_map<const ast::struct_field *, std::string>>
      field_type_contexts_;
};

} // namespace cinder::comptime
