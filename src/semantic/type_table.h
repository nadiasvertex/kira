#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/comptime/value.h"
#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/linear_poly.h"

namespace cinder::semantic {

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
  /// For `type_param_kind`: whether this is a declared *value* parameter
  /// (`n: usize`). A value parameter is never a value slot by itself — a slot
  /// that mentions it is a `symbolic_value_kind` over its variable — so this
  /// is what lets a bare one met in type position be turned into that slot.
  bool is_value_param = false;
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
  /// — a `const_value_kind`/`symbolic_value_kind` id, or
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
  /// Interns an in-scope generic type/value parameter.
  ///
  /// `arity` is 0 for an ordinary parameter and n for a declared n-argument
  /// constructor parameter (`F[_]` is 1) — the same name declared at two
  /// different kinds interns to two distinct ids, since the kind is part of
  /// what the parameter *is*.
  ///
  /// `decl` is the `[T]` that declared it, and when given it is what the
  /// parameter is keyed on. Without it, parameters are keyed by name alone
  /// and the `T` of one declaration is the *same id* as the `T` of every
  /// other — which is not a nuisance but a soundness hole: matching a
  /// callee's `list[T]` against a caller's `list[slice[T]]` then asks for
  /// `T := slice[T]`, an infinite type, and any matcher with an occurs check
  /// must either refuse it or be wrong. Scoping is how
  /// `infer::match_pattern` puts the real unifier onto these
  /// matches at all.
  ///
  /// Passing `nullptr` is for parameters with no declaration node to name
  /// them — `self` inside a trait, an associated type — which keep the old
  /// by-name identity because there is nothing else to key them on.
  [[nodiscard]] auto type_param(std::string_view name, size_t arity = 0,
                                const ast::type_param *decl = nullptr)
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
  /// The number of interned entries, so a caller can enumerate every live
  /// `type_id` as `[0, count())`. Used by `check_program` to precompute which
  /// interned types can carry a borrow (`checked_types::
  /// borrow_bearing_types`). Ids are dense and never reused, so a snapshot of
  /// this taken before an enumeration stays a valid upper bound even if the
  /// walk interns further types (`std::deque` keeps existing entries stable).
  [[nodiscard]] auto count() const -> std::size_t;
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

  /// Whether a value of type `id` *is* a pointer at run time.
  ///
  /// The heap-backed set `src/runtime/layout.h` describes: `str`, the
  /// prelude generics (`list`, `option`, `result`, ...), tuple, fixed
  /// array, struct, sum, and `fn`/closure. Everything else — the numbers,
  /// `bool`, `char`, `unit` — lives inline at its natural width.
  ///
  /// One definition, because two subsystems ask it and a disagreement
  /// between them is not a cosmetic drift: `llvm_codegen`'s `is_heap_type`
  /// decides whether a value is an opaque `ptr`, and the rule just below
  /// decides whether typing may ignore a `&`. A backend that thought
  /// `&int32` was a passthrough while typing thought it was an address is
  /// exactly the silent arithmetic-on-a-pointer bug (`spec/todo.md` 21).
  [[nodiscard]] auto is_heap_represented(type_id id) const -> bool;

  /// Whether a `&` to `referent` has the same runtime representation as the
  /// value itself, so typing may treat the two alike.
  ///
  /// True for everything `is_heap_represented` covers — `&xs` on a `list`
  /// costs no instruction, which is why `xs.len()` through a `&list[T]`
  /// parameter is the same code as through an owned one, the transparency
  /// the borrowing chapter describes. A number is the other case: `&n`
  /// *materializes* an address, and reading the number back out is a load,
  /// spelled `*n` (`codegen_stress/042`).
  ///
  /// Typing has to know the difference. Treating a `&int32` as an `int32`
  /// anyway is not a harmless shorthand — nothing emits the load, and the
  /// address is used as the number.
  ///
  /// Also true for a still-abstract referent (`&T` in a generic body, or an
  /// open leaf), which is the one place the two predicates differ. It is
  /// not *known* to need a load, and refusing it would break every generic
  /// that borrows; the instantiated copy is checked again with a real type,
  /// which is where the question can be answered.
  [[nodiscard]] auto reference_is_transparent(type_id referent) const -> bool;

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
  /// Whether `id` is a view — `slice[T]`/`mut slice[T]` or `cell[T]`/
  /// `mut cell[T]`, the borrowing values allowed to outlive a call. `str`
  /// is not one here: the implementation treats it as an owned value.
  [[nodiscard]] auto is_view(type_id id) const -> bool;
  /// Whether `id` is a mutable view (`mut slice[T]` / `mut cell[T]`).
  [[nodiscard]] auto is_mut_view(type_id id) const -> bool;
  /// Whether a value of `id` copies rather than moves (ch. 14, Copy and
  /// move): a builtin scalar (including `str`), `&T`, a raw pointer, or a
  /// read-only view. A type parameter is never `copy` here; whether its
  /// `copy` bound makes it one is the caller's to ask
  /// (`checked_types::copy_type_params`).
  [[nodiscard]] auto is_copy(type_id id) const -> bool;

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

/// Whether `name` is a builtin scalar type (`int32`, `str`, `bool`, ...).
[[nodiscard]] auto is_builtin_scalar_name(std::string_view name) -> bool;

/// The tuple element position `name` denotes, for the positional field
/// access `t.0` — or `nullopt` if `name` is not a plain decimal position.
///
/// The parser carries a tuple index through as a `field_expr`'s name text
/// (see its `int_lit` case in `parse_postfix_suffix`), so both the checker
/// and lowering need to recover the number from it, and must agree on
/// exactly which spellings count. Only plain decimal digits do: `t.0x1`,
/// `t.1_000`, and a suffixed `t.0u8` are all rejected rather than being
/// quietly reinterpreted, since a tuple position is a syntactic offset
/// rather than a numeric value.
[[nodiscard]] auto tuple_index_of(std::string_view name)
    -> std::optional<std::size_t>;

/// Expected generic-argument arity for a prelude container name, as an
/// inclusive [min, max] pair; `nullopt` when the name is not a prelude
/// container.
[[nodiscard]] auto builtin_generic_arity(std::string_view name)
    -> std::optional<std::pair<size_t, size_t>>;

/// Inclusive unsigned upper bound of a builtin integer type when it fits in
/// 64 bits; `nullopt` for non-integers and for the 128-bit types.
[[nodiscard]] auto integer_max_value(std::string_view name)
    -> std::optional<uint64_t>;

/// True for the signed builtin integer type names (`int8`..`int64`,
/// `isize`); false for unsigned integer names and non-integer names.
[[nodiscard]] auto is_signed_integer_name(std::string_view name) -> bool;


} // namespace cinder::semantic
