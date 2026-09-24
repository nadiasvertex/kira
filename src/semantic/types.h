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
  /// `spec/inference-rewrite.md` phase 7 gets a real unifier onto these
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
  /// interned types transitively carry a view (`checked_types::
  /// view_bearing_types`). Ids are dense and never reused, so a snapshot of
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
///
/// `defaults_by_param[i]` is that parameter's declared default-value
/// expression (null when it has none), and `param_names[i]` its spelling.
/// Both are copied from the callee's signature at the point the call is
/// checked because lowering has no route back to the declaration a call
/// resolved against for every call form — and an omitted argument is lowered
/// by lowering the callee's default expression right here at the call site
/// (see `lower_call`), which needs exactly these two pieces.
struct call_argument_mapping {
  std::vector<const ast::expr *> args_by_param;
  std::vector<const ast::expr *> defaults_by_param;
  std::vector<std::string> param_names;
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
  /// convention to key off — see `move_checker::receiver_is_moved`.
  std::string trait_name;
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

/// What it takes to drop one value of a struct/sum type — see
/// `checker::resolve_drop_plans` (`check.cpp`). `own_drop` is this type's own
/// `impl drop`, if it has one; `droppable_fields` are the struct fields (in
/// declaration order) that are themselves droppable, whether or not this
/// type has an `own_drop` of its own — the spec's implicit field-wise rule
/// applies regardless. Sum types never populate `droppable_fields` (payload
/// recursion is not implemented — a sum type only drops via an explicit
/// `impl drop` on the sum type itself). A `type_id` absent from
/// `checked_types::drop_plans` is not droppable at all; that absence is the
/// scope-exit drop pass's only "should I even look at this local" test.
struct drop_plan {
  std::optional<resolved_callee> own_drop;
  std::vector<std::pair<std::string, type_id>> droppable_fields;
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
  /// because the collection is not consumed by iterating it
  /// (`spec/list-migration-design.md` phase 2).
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

/// Type ids for `std.fmt`'s runtime-support types (`src/std/fmt.kira`),
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

/// Which of the two layout questions a `checked_types::layout_queries` entry
/// asks about its operand type.
enum class layout_query_kind : uint8_t {
  size_of,  ///< `size_of[T]()` — the bytes one `T` occupies.
  align_of, ///< `align_of[T]()` — the byte boundary a `T` must start on.
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
  /// Every struct/sum type found droppable — see `drop_plan`'s doc comment.
  /// Populated once, after the main per-function walk, by
  /// `checker::resolve_drop_plans` (`check.cpp`) over every type interned in
  /// `types`. Consumed by `hir::compute_drop_schedule`/`hir::lowerer` to
  /// build scope-exit drop calls (`spec/todo.md` item 6).
  std::unordered_map<type_id, drop_plan> drop_plans;
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

  /// The return type the checker inferred from the body of each function
  /// declared without one. Lowering reads it where it would read the
  /// annotation.
  std::unordered_map<const ast::func_decl *, type_id> inferred_return_types;
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
  /// One top-level `static let` reified as real backing data — an
  /// array/list/tuple whose elements are all scalar (integer/floating/
  /// boolean), which `checker::materialize_const_literal` cannot inline the
  /// way it does a single scalar (there's no one `ast::literal_expr` shape
  /// for a whole array). `hir::lower_module_items` looks a module's own
  /// `static_decl` items up here (by declaration pointer) and lowers a
  /// matching entry into an `hir_static_global`; `name` is unique across the
  /// whole program, so every backend can key one flat global table by it
  /// regardless of which module a reference lives in — mirroring how the
  /// existing cross-module function table already works.
  struct static_global_def {
    std::string name;
    type_id type;
    std::vector<comptime::value> elements;
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
  /// Every `ident_expr`/`module_path_expr` that resolved to a reified static
  /// global (see `static_global_defs`), mapped to that global's `name` —
  /// `hir::lower_ident`/`lower_module_path` look this up before falling back
  /// to `hir_local_ref`, mirroring `static_const_values` but for the
  /// aggregate case that can't be inlined at each reference site.
  std::unordered_map<const ast::node *, std::string> static_global_refs;
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
  /// Every interned `type_id` that transitively carries a *view*
  /// (`slice[T]`/`mut slice[T]`, directly or inside a struct field, sum
  /// variant payload, tuple element, reference, or generic argument) — see
  /// `checker::type_contains_view`, which computes this over the whole table
  /// once checking finishes. The borrow checker (`check_borrows`) reads it to
  /// decide whether a value keeps a borrow of its source collection alive past
  /// the statement that made it (a call returning a `window[T]` that stores a
  /// `slice` still borrows the sliced collection); it cannot compute this
  /// itself because resolving a struct's field types needs the generic
  /// substitution only the checker has. `str` is deliberately *excluded* even
  /// though the language models it as a view: the implementation treats `str`
  /// as an owned value everywhere, and tracking it as a borrow would reject
  /// pervasive valid code — matching `is_view_type`, which also lists only
  /// `slice`/`slice_mut`.
  std::unordered_set<type_id> view_bearing_types;
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
