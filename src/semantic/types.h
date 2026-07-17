#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/linear_poly.h"

namespace kira::semantic {

// ==========================================================================
//  Type model
//
//  Types are interned into a `type_table` so equality is id equality.
//  `unknown` deliberately unifies with everything: it is produced wherever
//  the checker cannot know a type (unannotated parameters, external modules,
//  unresolved generics) and it silences downstream diagnostics so one gap in
//  knowledge never causes cascading errors.
// ==========================================================================

/// Interned index into a `type_table`; equality is id equality.
using type_id = uint32_t;

/// The always-present "not known" type; compatible with every other type.
inline constexpr type_id k_unknown_type = 0;
/// The always-present type produced after an error has already been
/// reported for an expression, so its type doesn't need to be guessed.
inline constexpr type_id k_error_type = 1;

/// Which shape of type a `type_entry` describes; determines which of its
/// fields are meaningful.
enum class type_kind : uint8_t {
  unknown_kind,         ///< Not known; compatible with every type.
  error_kind,           ///< Produced after an error was already reported.
  builtin_kind,         ///< Scalar builtin such as `int32`, `str`, `bool`.
  builtin_generic_kind, ///< Prelude container such as `list[T]`, `option[T]`.
  tuple_kind,           ///< `(A, B, C)`.
  array_kind,           ///< `array[T, n]`.
  fn_kind,              ///< `fn(A, B) -> R`.
  ref_kind,             ///< `&T` / `&mut T`.
  ptr_kind,             ///< `*T` / `*mut T`.
  struct_kind,          ///< User struct type, possibly instantiated.
  sum_kind,             ///< User sum type, possibly instantiated.
  opaque_kind,          ///< User alias/opaque type declaration.
  existential_kind,     ///< `some Trait[Args] + Other` on a function's
                        ///< return type; see the doc comment below.
  type_param_kind,      ///< In-scope generic parameter such as `T`.
  ctor_ref_kind,        ///< An *unapplied* nominal type constructor — what
                        ///< `option` denotes as an argument to `monad[option]`
                        ///< or in `impl monad for option`, before any type
                        ///< arguments are applied. Only nominal constructors
                        ///< (prelude generics and user generic declarations)
                        ///< inhabit higher kinds; see `ctor_ref`.
  param_app_kind,       ///< An application whose head is itself an in-scope
                        ///< higher-kinded type parameter: the `F[A]` in
                        ///< `trait functor[F[_]]`'s method signatures. Kept
                        ///< abstract — the checker treats it like a type
                        ///< parameter (compatible with everything) until
                        ///< substitution re-resolves it to a concrete
                        ///< application; see `param_app`.
  type_var_kind,        ///< Fresh inference variable; see `fresh_type_var`.
  const_value_kind,     ///< A literal compile-time value used as a const
                        ///< generic argument, e.g. the `3` in `vec[T, 3]`.
  symbolic_value_kind,  ///< A compile-time value that isn't closed: a
                        ///< canonical linear polynomial over value
                        ///< parameters, e.g. the `n + 1` in `vec[T, n + 1]`.
  const_variant_kind,   ///< A sum-type variant used as a compile-time value
                        ///< argument, e.g. the `open` in `connection[open]`.
  refinement_kind,      ///< A base type constrained by a `where` predicate;
                        ///< see the doc comment below.
};

/// The three `type_kind`s that describe a compile-time *value* occupying a
/// type-argument slot, rather than a type: a closed integer, an open linear
/// polynomial, and a sum-type variant. Grouped because every value slot is
/// compared, displayed, and substituted the same way regardless of which of
/// the three it happens to be (`spec/dependent-types-design.md` §2).
[[nodiscard]] constexpr auto is_value_kind(type_kind kind) -> bool {
  return kind == type_kind::const_value_kind ||
         kind == type_kind::symbolic_value_kind ||
         kind == type_kind::const_variant_kind;
}

/// One trait requirement in an existential type's bound list
/// (`some Trait[Args] + Other`), resolved from source: the trait's bare name
/// (looked up session-wide via `find_trait_anywhere`, not stored as a
/// `type_id` since traits aren't themselves interned into `type_table`) plus
/// its resolved generic arguments.
struct bound_trait_ref {
  std::string trait_name;
  std::vector<type_id> trait_args;
};

/// The interned data behind one `type_id`. Which fields are meaningful
/// depends on `kind`: e.g. `args` holds tuple elements for `tuple_kind` but
/// generic arguments for `builtin_generic_kind`/`struct_kind`/`sum_kind`, and
/// `result` holds the referenced/pointed-to/array-element type for
/// `ref_kind`/`ptr_kind`/`array_kind` but the return type for `fn_kind`.
/// `existential_kind` reuses `result` too, for its concrete backing type
/// (`k_unknown_type` until `check_function` finishes checking the declaring
/// function's body and backfills it via `mutable_entry`; see
/// `checker::resolve_existential_type`/`check_function`, `check.cpp`) and
/// uses the dedicated `existential_bound` field for its trait requirements
/// — a value of this kind is a *nominal* fiction, minted fresh (never
/// structurally interned) once per `some Trait[Args]` written in source, so
/// two occurrences with identical bounds are still distinct types, exactly
/// like Rust's `-> impl Trait`. Lowering unwraps every `existential_kind`
/// type back to its `result` before HIR ever sees it (`hir::lowerer::
/// resolve_opaque`) — opacity is purely a checker-level view enforced by
/// restricting which operations `infer_method_call` allows on a receiver of
/// this kind, not a distinct runtime representation.
struct type_entry {
  type_kind kind = type_kind::unknown_kind;
  std::string name;                     ///< Builtin/user/parameter name.
  std::string module_name;              ///< Owning module for user types.
  const ast::type_decl *decl = nullptr; ///< Declaration for user types.
  std::vector<type_id> args;            ///< Element/parameter/generic args.
  type_id result = k_unknown_type;      ///< fn result or ref/ptr/array inner.
  bool is_mut = false;                  ///< Mutability for ref/ptr types.
  std::optional<uint64_t> array_size;   ///< Array length when statically known.
  /// The *kind* of a type parameter or constructor, measured as an arity —
  /// kinds are arities and nothing more (no currying, no kind polymorphism).
  /// For `type_param_kind`: 0 for an ordinary `T`, n for a declared
  /// `F[_, ...]` constructor parameter. For `ctor_ref_kind`: the number of
  /// type arguments the referenced constructor takes. 0 everywhere else.
  size_t ctor_arity = 0;
  std::vector<bound_trait_ref> existential_bound; ///< `existential_kind` only.
  /// The value of a `const_value_kind` / `symbolic_value_kind` slot, always
  /// in canonical form — constant for the former, open for the latter. One
  /// field rather than two so every consumer (compatibility, display,
  /// substitution, the solver) reads a value slot exactly one way regardless
  /// of whether it happened to close.
  linear_poly value;
  /// The `where` predicate of a `refinement_kind`, an expression over `self`
  /// (the value being constrained) and the declaration's value parameters.
  /// Borrowed from the AST — never owned; refinements are checker-level
  /// facts, so nothing outlives the session that produced them.
  const ast::expr *predicate = nullptr;
};

/// Owns every `type_entry` produced while checking one session, interning
/// structurally-identical types (same kind, name/decl, and arguments) to the
/// same `type_id` so type equality is just id equality.
class type_table {
public:
  /// Seeds the table with `k_unknown_type` and `k_error_type` at their fixed
  /// ids, plus `bool` (see `bool_type`).
  type_table();

  /// Interns a scalar builtin type such as `int32` or `str`.
  [[nodiscard]] auto builtin(std::string_view name) -> type_id;
  /// Interns a prelude container instantiation such as `list[T]`.
  [[nodiscard]] auto builtin_generic(std::string_view name,
                                     std::vector<type_id> args) -> type_id;
  /// Interns a tuple type `(A, B, C)` from its element types.
  [[nodiscard]] auto tuple_of(std::vector<type_id> elements) -> type_id;
  /// Interns `array[T, n]`. `length` is the *value slot* holding the length
  /// — a `const_value_kind`/`symbolic_value_kind`/`type_param_kind` id, or
  /// `k_unknown_type` when the length wasn't written or fell outside the
  /// reasoning fragment. `size` mirrors `length` as a plain integer when (and
  /// only when) the length is a closed constant, because layout and both
  /// backends need a byte count, not a proof term; it is `nullopt` for a
  /// symbolic length, which is exactly the case a backend must never see (see
  /// `spec/dependent-types-design.md` §3.1 — lowering rejects a runtime type
  /// whose length is still open).
  [[nodiscard]] auto array_of(type_id element, std::optional<uint64_t> size,
                              type_id length = k_unknown_type) -> type_id;
  /// Interns a function type `fn(params...) -> result`.
  [[nodiscard]] auto fn_of(std::vector<type_id> params, type_id result)
      -> type_id;
  /// Interns a reference type `&T` (or `&mut T` when `is_mut`).
  [[nodiscard]] auto ref_to(type_id inner, bool is_mut) -> type_id;
  /// Interns a raw pointer type `*T` (or `*mut T` when `is_mut`).
  [[nodiscard]] auto ptr_to(type_id inner, bool is_mut) -> type_id;
  /// Interns an instantiation of a user `type` declaration (struct, sum, or
  /// alias/opaque) with the given generic `args`.
  [[nodiscard]] auto user_type(const ast::type_decl &decl,
                               std::string_view module_name,
                               std::vector<type_id> args) -> type_id;
  /// Interns an in-scope generic type/value parameter, identified by name
  /// and kind. `arity` is 0 for an ordinary parameter and n for a declared
  /// n-argument constructor parameter (`F[_]` is 1) — the same name declared
  /// at two different kinds interns to two distinct ids, since the kind is
  /// part of what the parameter *is*.
  [[nodiscard]] auto type_param(std::string_view name, size_t arity = 0)
      -> type_id;
  /// Interns an *unapplied* nominal type constructor of the given arity —
  /// `option` as a value of kind `[_]`, passable where a higher-kinded
  /// parameter is expected. `decl` is the user declaration behind it, or
  /// `nullptr` for a prelude generic (`option`, `list`, `result`, ...).
  /// Keyed on the declaration (or builtin name) alone, so every reference to
  /// the same constructor is one id.
  [[nodiscard]] auto ctor_ref(std::string_view name,
                              std::string_view module_name,
                              const ast::type_decl *decl, size_t arity)
      -> type_id;
  /// Interns an application of an in-scope higher-kinded *parameter* to
  /// argument types — the `F[A]` in a `trait functor[F[_]]` method
  /// signature. `head` must be a `type_param_kind` id with matching arity;
  /// the head is stored in `result` and the applied arguments in `args`.
  /// Deliberately abstract: substituting a concrete constructor for the head
  /// never rewrites one of these — the checker re-resolves the written type
  /// under the substitution instead, so `F[A]` with `F := option` interns as
  /// the ordinary `option[A]` id and id-equality holds.
  [[nodiscard]] auto param_app(type_id head, std::vector<type_id> args)
      -> type_id;
  /// Interns a literal compile-time value used as a const generic argument
  /// (e.g. the `3` in `vec[T, 3]`), keyed on `underlying` (the value's
  /// scalar type, e.g. `usize`) and `value` so `vec[T, 3]` and `vec[T, 5]`
  /// are distinct types while two instantiations with the same literal
  /// collapse to one id.
  [[nodiscard]] auto const_value(type_id underlying, uint64_t value) -> type_id;
  /// Interns an *open* compile-time value — a canonical linear polynomial
  /// over in-scope value parameters, e.g. the `n + 1` in `vec[T, n + 1]`.
  /// Keyed on the polynomial's canonical form, so `m + n` and `n + m` intern
  /// to the same id and the table's id-equality invariant survives symbolic
  /// arithmetic. A polynomial that turns out to be closed is *not*
  /// representable here — it degrades to `const_value` instead, so every
  /// value has exactly one representation (`spec/dependent-types-design.md`
  /// §2.1).
  [[nodiscard]] auto symbolic_value(type_id underlying, linear_poly value)
      -> type_id;
  /// Interns a sum-type variant used as a compile-time value argument (the
  /// `open` in `connection[open]`), keyed on the sum declaration and the
  /// variant name so distinct states are distinct types. Variants compare by
  /// identity and nothing else — which is all §State Machines in Types needs,
  /// and is total.
  [[nodiscard]] auto const_variant(const ast::type_decl &sum_decl,
                                   std::string_view module_name,
                                   std::string_view variant_name) -> type_id;
  /// Interns a refinement type — a `base` constrained by a `where`
  /// `predicate` over `self`. Identity is *declaration-nominal*: keyed on
  /// `decl` (plus its resolved `args`, so `index[3]` and `index[5]` differ),
  /// which makes two identically-predicated declarations distinct types to
  /// the user while the solver still sees straight through both to
  /// `(base, predicate)`. An inline refinement (`x: int32 where self > 0`,
  /// with no declaration to name it) passes `decl == nullptr` and is keyed on
  /// its predicate node instead — one type per written occurrence.
  [[nodiscard]] auto refinement_of(const ast::type_decl *decl,
                                   std::string_view display_name,
                                   std::string_view module_name, type_id base,
                                   std::vector<type_id> args,
                                   const ast::expr *predicate) -> type_id;
  /// Mints a fresh, globally-unique inference variable for local parameter
  /// inference (see `src/semantic/check.cpp`'s unification engine). Unlike
  /// every other `type_table` constructor, this never structurally interns —
  /// each call returns a distinct id, since two variables must stay
  /// independently solvable even if they happen to arise identically. A
  /// variable left unsolved after inference behaves exactly like
  /// `k_unknown_type` everywhere (`is_unknown`, `compatible`, `display`), so
  /// a var that escapes unresolved is harmless rather than a false positive.
  [[nodiscard]] auto fresh_type_var() -> type_id;
  /// Mints a fresh, never-interned `existential_kind` type for one
  /// `some Trait[Args] + Other` written in source — like `fresh_type_var`,
  /// always pushes a new entry rather than consulting `interned_`, since two
  /// syntactically-identical existential types at different declarations
  /// must stay distinct (see `type_entry`'s doc comment). `result` (the
  /// eventual concrete backing type) starts as `k_unknown_type`; the caller
  /// backfills it later via `mutable_entry` once it's known.
  [[nodiscard]] auto fresh_existential(std::string display_name,
                                       std::vector<bound_trait_ref> bound)
      -> type_id;

  /// Looks up the data behind `id`; returns the `unknown` entry for an
  /// out-of-range id.
  [[nodiscard]] auto entry(type_id id) const -> const type_entry &;
  /// Mutable access to an already-minted entry, for backfilling a field that
  /// wasn't known at mint time (currently only `existential_kind`'s `result`
  /// — see `type_entry`'s doc comment). `id` must already be a valid,
  /// in-range id; unlike `entry`, this does not fall back to `unknown`.
  [[nodiscard]] auto mutable_entry(type_id id) -> type_entry &;
  /// Renders `id` as the user-facing type spelling used in diagnostics
  /// (e.g. `list[int32]`, `fn(int32) -> str`).
  [[nodiscard]] auto display(type_id id) const -> std::string;

  /// The interned id for the builtin `bool` type. Unlike every other
  /// builtin scalar (interned lazily on first use via `builtin`), `bool`
  /// is pre-interned unconditionally by the constructor, so this is a
  /// `const` lookup — no interning, no mutation. Exists for callers that
  /// hold a `const type_table&` and need a definite `bool` type_id for a
  /// type the checked program may never have happened to reference itself
  /// (lowering synthesizing a comparison for a desugared `for` loop, e.g.).
  [[nodiscard]] auto bool_type() const -> type_id;
  /// The interned id for the builtin `usize` type, pre-interned for the
  /// same reason and by the same mechanism as `bool_type` — used for a
  /// desugared indexed-container `for` loop's index/length values.
  [[nodiscard]] auto usize_type() const -> type_id;
  /// The interned id for the builtin `char` type, pre-interned for the
  /// same reason and by the same mechanism as `bool_type` — used as the
  /// element type of a desugared `for` loop over a `str`.
  [[nodiscard]] auto char_type() const -> type_id;

  /// Follows a `refinement_kind` to the type it refines, transitively (a
  /// refinement of a refinement is legal), and returns `id` unchanged for
  /// every other kind. A refinement *is* its base at runtime — the predicate
  /// is a compile-time fact, not a representation — so every question about
  /// representation, arithmetic, or layout asks it of the base. The one thing
  /// that must **not** strip is a narrowing coercion, which is precisely the
  /// place the predicate has to be discharged (`checker::check_narrowing`).
  [[nodiscard]] auto strip_refinement(type_id id) const -> type_id;

  /// Rewrites `id` with every refinement inside it replaced by its base,
  /// however deeply nested — `option[index[n]]` becomes `option[usize]`,
  /// `array[positive, 4]` becomes `array[int32, 4]`.
  ///
  /// `strip_refinement` peels only the outermost layer, which is all the
  /// *checker* ever needs (an obligation is always about one value). Lowering
  /// needs more: a refinement is a compile-time fact with no runtime
  /// existence, and a backend that met one nested inside a container would
  /// have to invent a layout for something that has none. So the checker runs
  /// every type it hands downstream through this, and HIR, the VM, and LLVM
  /// never encounter a `refinement_kind` at all.
  ///
  /// Interning, so the result is an ordinary type id like any other.
  [[nodiscard]] auto erase_refinements(type_id id) -> type_id;

  /// The base type of the refinement *declared* under `name`, if exactly one
  /// is — `int32` for a `type positive = int32 where self > 0`.
  ///
  /// Exists for `runtime/layout.cpp`, which resolves a struct field's type
  /// from its declared AST name and has no module index to look a user type
  /// up in. Without this, a field typed `positive` would fall through to
  /// layout's "some named type, therefore heap-referenced" default and be
  /// given 8 bytes instead of the 4 its `int32` base actually occupies —
  /// harmless for an ordinary struct (writer and reader agree), but wrong for
  /// a `packed` one, whose whole purpose is an exact byte layout.
  ///
  /// `nullopt` when no refinement is declared under `name`, and also when
  /// *several* are (two modules may each declare a `positive` over different
  /// bases): a name alone cannot disambiguate them, and guessing a layout is
  /// worse than declining to.
  [[nodiscard]] auto refinement_base_named(std::string_view name) const
      -> std::optional<type_id>;

  /// Whether `id` is `unknown`, `error`, or a type parameter — anything the
  /// checker treats as "don't know, don't complain."
  [[nodiscard]] auto is_unknown(type_id id) const -> bool;
  /// Whether `id` is the builtin `bool`.
  [[nodiscard]] auto is_boolean(type_id id) const -> bool;
  /// Whether `id` is a builtin signed/unsigned integer type (including
  /// `byte`, `isize`, `usize`).
  [[nodiscard]] auto is_integer(type_id id) const -> bool;
  /// Whether `id` is a builtin `float32`/`float64`/`float128`.
  [[nodiscard]] auto is_float(type_id id) const -> bool;
  /// Whether `id` is an integer or float builtin.
  [[nodiscard]] auto is_numeric(type_id id) const -> bool;
  /// Whether `id` is the builtin `unit`.
  [[nodiscard]] auto is_unit(type_id id) const -> bool;

  /// Whether a value of `found` is acceptable where `expected` is required.
  /// Unknown and error types are compatible with everything by design.
  ///
  /// This is *structural* compatibility only. Refinements are compared by
  /// their base — a `positive` is structurally an `int32` and vice versa —
  /// because the predicate is not a shape question: narrowing an `int32` to a
  /// `positive` is a **proof obligation**, raised and discharged by the
  /// checker (`check_narrowing`), not a type mismatch to report here. Were
  /// this to return `false` for the narrowing direction, every such site would
  /// report "type mismatch" and the solver would never get to prove anything.
  ///
  /// Value slots (`vec[T, n + 1]` against `vec[T, 3]`) compare by asking
  /// whether the equation between their polynomials is *satisfiable*, not
  /// whether they are identical — see `equation_satisfiable`. So a value slot
  /// that could still be solved stays compatible, exactly as an unsolved `T`
  /// does, while one that provably cannot (`n + 1 = 0` for `n: usize`) is
  /// rejected.
  [[nodiscard]] auto compatible(type_id expected, type_id found) const -> bool;

private:
  /// Returns the existing id for `key` if already interned, otherwise
  /// stores `entry` and returns its new id.
  [[nodiscard]] auto intern(std::string key, type_entry entry) -> type_id;
  /// Whether two value slots could denote the same value — see `compatible`.
  [[nodiscard]] auto values_compatible(type_id expected, type_id found) const
      -> bool;
  /// Whether a value slot's unknowns are unsigned (so `>= 0`).
  [[nodiscard]] auto value_vars_non_negative(const type_entry &slot) const
      -> bool;

  // A `std::deque`, not `std::vector`: `entry()` returns `const type_entry &`
  // that callers routinely hold across further calls into this table (e.g.
  // `infer_method_call` holding its own `entry` across `find_method`/
  // `fn_type_of`, which can themselves intern new types). A `std::vector`
  // reallocates its backing storage on `push_back`, silently invalidating
  // every such reference the moment capacity is exceeded — a real,
  // previously-hit bug (a struct's own name reading back empty mid-
  // expression). `std::deque` never invalidates references to existing
  // elements on `push_back`, only iterators (which nothing here holds
  // across a mutation), while keeping the same O(1) indexed access
  // `entry()`/`intern()` rely on — a direct, representation-only fix for
  // the whole class of bug at once, rather than auditing every call site
  // that takes a `const type_entry &`.
  std::deque<type_entry> entries_;
  std::unordered_map<std::string, type_id> interned_;
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
struct call_argument_mapping {
  std::vector<const ast::expr *> args_by_param;
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
/// see `checker::instantiate_const_generic` (`check.cpp`), the only place
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
};

/// Type ids for `std.fmt`'s runtime-support types (`src/std/fmt.kira`),
/// resolved once after checking finishes and handed to `hir::lower` so it
/// can build `format_spec`/box-type struct literals for interpolation
/// lowering without a name-based lookup of its own — `type_table` only
/// supports looking a user type up via its declaring `ast::type_decl`, which
/// lowering has no route to independent of an AST node it's already
/// visiting (see `interp_dispatch`'s use in `hir::lower`). Left as
/// `k_unknown_type` (0) if `std.fmt` wasn't part of the session — lowering
/// only reads these when a `interp_dispatch` entry exists, and one can only
/// exist if a source file had string interpolation, which the driver never
/// allows without also injecting `std.fmt` (`inject_stdlib_prelude`).
struct fmt_runtime_types {
  type_id format_spec = 0;
  type_id align_mode = 0;
  type_id sign_mode = 0;
  type_id box_u64 = 0;
  type_id box_u32 = 0;
  type_id box_u8 = 0;
  type_id box_usize = 0;
  type_id box_bool = 0;
  type_id box_f64 = 0;
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

struct checked_types {
  type_table types;
  std::unordered_map<const ast::node *, type_id> node_types;
  std::unordered_map<const ast::field_pattern *, type_id>
      struct_pattern_field_types;
  std::unordered_map<const ast::struct_field_init *, type_id>
      struct_literal_field_types;
  std::unordered_map<const ast::call_expr *, call_argument_mapping>
      call_argument_mappings;
  /// Every call resolved by `infer_qualified_call` — see `resolved_callee`'s
  /// doc comment. A node absent here was either resolved some other way
  /// (a plain same-module/imported bare-name call, a method call) or never
  /// resolved at all.
  std::unordered_map<const ast::call_expr *, resolved_callee> resolved_callees;
  /// Every interpolation segment's resolved rendering dispatch — see
  /// `interp_dispatch`'s doc comment. Keyed by the segment's `value`
  /// expression pointer (`ast::interp_segment::value.get()`).
  std::unordered_map<const ast::expr *, interp_dispatch> interp_dispatches;
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
  /// Every `splice_expr`/`splice_stmt` node resolved (via compile-time
  /// evaluation of its operand) to the exact AST fragment it should be
  /// lowered as, keyed by the splice node itself. `hir::lower` looks a
  /// splice node up here and lowers the resolved fragment in its place —
  /// see `checker::infer_expr`'s `splice_expr` case and
  /// `checker::check_body_node`'s `splice_stmt` case. Absent entries mean
  /// the splice never resolved to usable syntax (already diagnosed).
  std::unordered_map<const ast::node *, const ast::node *> spliced_fragments;
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

/// Whether `name` is a builtin scalar type (`int32`, `str`, `bool`, ...).
[[nodiscard]] auto is_builtin_scalar_name(std::string_view name) -> bool;

/// Expected generic-argument arity for a prelude container name, as an
/// inclusive [min, max] pair; `nullopt` when the name is not a prelude
/// container.
[[nodiscard]] auto builtin_generic_arity(std::string_view name)
    -> std::optional<std::pair<size_t, size_t>>;

/// Inclusive unsigned upper bound of a builtin integer type when it fits in
/// 64 bits; `nullopt` for non-integers and for the 128-bit types.
[[nodiscard]] auto integer_max_value(std::string_view name)
    -> std::optional<uint64_t>;

// ==========================================================================
//  Program index — session-wide declaration lookup tables.
// ==========================================================================

/// A `type` declaration plus the file it was declared in.
struct type_decl_ref {
  const ast::type_decl *decl = nullptr;
  file_id_type file_id = 0;
};

/// A `trait` declaration plus the file it was declared in.
struct trait_decl_ref {
  const ast::trait_decl *decl = nullptr;
  file_id_type file_id = 0;
};

/// A `concept` declaration plus the file it was declared in.
struct concept_decl_ref {
  const ast::concept_decl *decl = nullptr;
  file_id_type file_id = 0;
};

/// A `signature` declaration plus the file it was declared in.
struct signature_decl_ref {
  const ast::signature_decl *decl = nullptr;
  file_id_type file_id = 0;
};

/// A parameterized module (functor) declaration plus the file it was declared
/// in. Kept distinct from ordinary submodules because a functor's body is not
/// elaborated as a module in its own right — it is instantiated per argument
/// tuple by a `use m[args]` import.
struct functor_decl_ref {
  const ast::sub_module_decl *decl = nullptr;
  file_id_type file_id = 0;
};

/// A `def` declaration plus the file it was declared in.
struct func_decl_ref {
  const ast::func_decl *decl = nullptr;
  file_id_type file_id = 0;
};

/// A `static` binding declaration plus the file it was declared in.
struct static_decl_ref {
  const ast::static_decl *decl = nullptr;
  file_id_type file_id = 0;
};

/// One sum-type variant, plus the `type` declaration it belongs to — lets a
/// bare variant name (`@some`) be looked up without first knowing its sum
/// type.
struct variant_ref {
  const ast::type_decl *sum_decl = nullptr;
  const ast::sum_variant *variant = nullptr;
};

/// One `impl` block, plus the module and file it was declared in.
struct impl_ref {
  const ast::impl_decl *decl = nullptr;
  std::string module_name;
  file_id_type file_id = 0;
};

/// One `extend` block, plus the module and file it was declared in.
struct extend_ref {
  const ast::extend_decl *decl = nullptr;
  std::string module_name;
  file_id_type file_id = 0;
};

/// One name a `use` declaration binds into a file's local scope — either the
/// final path segment (`use a.b.c` binds `c`), a selected/renamed item
/// (`use a.b.{c as d}`), or a wildcard marker (`use a.b.*`).
struct import_binding {
  std::string local_name;        ///< Name the import introduces locally.
  std::vector<std::string> path; ///< Source module path of the import.
  std::string leaf_name;         ///< Imported member name; empty for modules.
  bool is_wildcard = false;      ///< Whether this is a `use a.b.*` wildcard.
  source_span span;              ///< Location of the imported item/selector.
};

/// Every declaration directly owned by one module, aggregated across every
/// file that contributes to it (a module may span several files).
struct module_members {
  std::string module_name;
  std::unordered_map<std::string, type_decl_ref> types;
  std::unordered_map<std::string, trait_decl_ref> traits;
  std::unordered_map<std::string, concept_decl_ref> concepts;
  std::unordered_map<std::string, signature_decl_ref> signatures;
  std::unordered_map<std::string, functor_decl_ref> functors;
  std::unordered_map<std::string, func_decl_ref> functions;
  std::unordered_map<std::string, static_decl_ref> statics;
  std::unordered_map<std::string, variant_ref> variants;
  std::vector<impl_ref> impls;
  std::vector<extend_ref> extends;
};

/// Session-wide index of every module's declarations and every file's
/// imports, used by the checker to resolve names and types without
/// re-walking the AST for each lookup.
struct program_index {
  std::unordered_map<std::string, module_members> modules;
  std::unordered_map<file_id_type, std::vector<import_binding>> imports;

  /// Looks up a module by its fully-qualified dotted name, or `nullptr` if
  /// no file in the session declares it.
  [[nodiscard]] auto find_module(std::string_view module_name) const
      -> const module_members *;
};

/// Builds a `program_index` by walking every input file's items, recording
/// each module's direct declarations and each file's `use` bindings
/// (recursing into inline submodules under their qualified name).
[[nodiscard]] auto build_program_index(const std::vector<parsed_module> &inputs)
    -> program_index;

} // namespace kira::semantic
