# 37. Higher-Kinded Traits

**Status:** Implemented

A trait may be parameterized over a type constructor rather than a concrete type — `trait functor[F[_]]` — enabling abstractions like `functor`/`monad` written once over any matching constructor.

## Syntax

Type parameter lists (`type_params`, `type_param` in `spec/kira-grammar.ebnf`) admit a higher-kinded form: `F[_]` (arity 1), `F[_, _]` (arity 2), and so on — one underscore per constructor argument. `is_higher_kinded` and the underscore count are recorded on the AST's `type_param`.

## Semantics

### Kinds are arities

A type parameter's kind is `*` (ordinary, ground type) or an n-ary constructor. There is no currying, no kind polymorphism, and no kind inference across declarations — the kind is exactly what the declaration syntax states. Builtin generics carry their true arities (`option` 1, `result` 2, `list` 1, `array` 2, ...).

### Type constructors and applications

Two additional shapes exist in the type table alongside ordinary types:

- an unapplied nominal constructor — what `option` denotes as an argument to `monad[option]` or in `impl monad for option`;
- an application whose head is a type parameter — `F[A]`, `M[B]` — holding the head parameter and argument type ids.

Substituting a concrete constructor for a parameter normalizes the application to the same interned type as if it had been written concretely: substituting `F := option` into `F[A]` produces *the* `option[A]` id, identical to one written directly in source, preserving the id-equality invariant that comparisons downstream rely on.

**Kind checking.** Applying a kind-`*` name, applying an n-ary constructor to the wrong number of arguments, or using a higher-kinded parameter bare in type position are all diagnosed. A bare generic name is accepted as a constructor argument only where the target parameter is itself higher-kinded (kind-directed resolution).

**Only nominal constructors inhabit higher kinds.** The argument for `F[_]` must be a named declaration of matching arity — a user struct/sum/opaque generic or a prelude generic. No lambdas at the type level, no partially-applied constructors, no generic aliases as constructor arguments.

### Trait declarations over constructor parameters

```kira
trait functor[F[_]]:
    def map[A, B](fa: F[A], f: fn(A) -> B) -> F[B]

trait monad[M[_]] requires functor[M]:
    def pure[A](a: A) -> M[A]
    def bind[A, B](ma: M[A], f: fn(A) -> M[B]) -> M[B]
```

Method signatures may apply the trait's constructor parameter; method-local generic parameters (`[A, B]`) interact with it normally. Trait-default bodies type-check under an abstract `F`, using only operations the trait's own surface and `requires` bounds justify. `requires functor[M]` where `M` is higher-kinded makes `functor[M]`'s methods available wherever `monad[M]` is in scope, via the same implied-bounds mechanism kind-`*` traits use. Associated types inside a higher-kinded trait are not supported.

### Impls for constructors

```kira
impl monad for option:
    def pure[A](a: A) -> option[A]: @some(a)
    def bind[A, B](ma: option[A], f: fn(A) -> option[B]) -> option[B]:
        match ma:
            @some(a) => f(a)
            @none    => @none
```

The impl target resolves to an unapplied constructor; its arity must match the trait parameter's kind (`impl functor for result` is diagnosed — `result` is 2-ary, `functor` requires 1-ary). Coherence uses the same string-keyed, one-impl-per-(trait, declaration) scheme as kind-`*` impls, keyed on the constructor's declaration; the orphan rule is unchanged (own the trait or the constructor). Each impl method is checked against the trait signature under the constructor substitution — `F[A]` becomes `option[A]` and must equal the impl's written `option[A]` by id. Trait defaults clone per impl, as for ordinary traits.

### Inference and monomorphization

Unification for a higher-kinded parameter is **rigid pattern-matching only**: `F[A] ~ option[int32]` solves `F = option, A = int32` by matching the outermost nominal constructor; `F[A] ~ int32` fails; two flexible constructors never unify with each other's applications. This is a deliberate restriction — full higher-order unification is undecidable in general and unnecessary for the traits this feature targets.

Calling `bind(ma, f)` with `ma: option[int32]` against `bind[A, B](ma: M[A], f: fn(A) -> M[B])` solves `M = option, A = int32` from the outermost constructor of `ma`'s type, then `B` flows from `f` as usual. Method syntax (`ma.bind(f)`) resolves through the constructor's impl the same way ordinary method lookup works. A generic function may itself take a higher-kinded, bound-checked parameter (`def lift[F[_]: functor, A](...)`).

**Erasure.** By HIR, every constructor parameter is resolved to a concrete constructor and every application to a concrete type — an HK trait method call monomorphizes per call site (e.g. `option::pure$int32`), and backends, layout, and both runtimes are unaffected. This is a semantic-analysis-only feature (plus module-metadata plumbing for separate compilation).

## Example

```kira
trait functor[F[_]]:
    def map[A, B](fa: F[A], f: fn(A) -> B) -> F[B]

trait monad[M[_]] requires functor[M]:
    def pure[A](a: A) -> M[A]
    def bind[A, B](ma: M[A], f: fn(A) -> M[B]) -> M[B]

impl monad for option:
    def pure[A](a: A) -> option[A]: @some(a)
    def bind[A, B](ma: option[A], f: fn(A) -> option[B]) -> option[B]:
        match ma:
            @some(a) => f(a)
            @none    => @none
```

## Implementation status

Implemented end to end: kind-as-arity checking, constructor/application type-table representation with substitution normalization, trait and impl checking over constructors, rigid inference, and per-call monomorphization all land, and both the bytecode VM and LLVM/AOT backends run the example above unchanged. Regression coverage includes `check_test` acceptance and five kind-error cases, `src/testdata/semantic_stress/028_higher_kinded_traits.kira`, `src/testdata/codegen_stress/034_higher_kinded_trait_dispatch.kira` (VM/JIT parity), and parser tests for `F[_, _]`.

Deferred, not v1: higher-kinded parameters in `concept` definitions, `some Trait` existentials over higher-kinded traits, and `box[trait]` objects for higher-kinded traits (object safety for HKTs is out of scope).

## See also

- [Modules as Compile-Time Values](36-modules-as-compile-time-values.md) — a sibling compile-time-only abstraction; both erase before code generation.
