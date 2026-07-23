# 31. Compile-Time Execution

**Status:** Implemented

Compile-time evaluation of `static` declarations, `static if`/`static assert`/`static for`, compile-time reflection, `pure` functions, and quoting/splicing.

There is no separate template or macro language. Compile-time code is ordinary Kira — the same functions, loops, and pattern matching — evaluated by an interpreter (`src/comptime/eval.*`) rather than compiled to machine code. See [Compile-Time Semantics](32-compile-time-semantics.md) for the execution model this section's constructs run inside.

## `static` bindings

Grammar: `static_decl` in `spec/kira-grammar.ebnf`.

```kira
static PI:    float64          = 3.141592653589793
static LIMIT: int32            = 1_000_000
static TABLE: array[int32, 256] = build_lookup_table()
```

- A `static` binding's initializer is evaluated by the compile-time interpreter. Evaluation failure (an expression that is not closed, or that depends on runtime input) is a compile error.
- `build_lookup_table` above is an ordinary function; nothing marks it `static` — the binding forces evaluation of whatever it calls.
- The result is reified: frozen, immutable data baked into the binary. See [Compile-Time Semantics § Compile-Time Memory](32-compile-time-semantics.md#compile-time-memory).
- A `static` item may also appear inside a `trait`/`impl` body as a required or provided constant (`check.cpp`'s trait-item handling for `static_decl`).

## `static if`

```kira
static IS_64BIT: bool = size_of[usize]() == 8

static if IS_64BIT:
    type word = uint64
else:
    type word = uint32
```

- The condition is evaluated at compile time; it must be closed.
- The branch not taken is **not compiled or type-checked** — it is discarded before name resolution sees it (`static_decl_kind::conditional_compilation`, `check.cpp:15119`).
- `static if` is valid at module scope (selecting between top-level items) and inside a function body (selecting between statements).

## `static assert`

```kira
static assert size_of[T]() <= 64, "T must fit in a cache line"
```

- If the condition is closed and false, this is a compile error at the assertion's location, with the optional message attached.
- If the condition depends on runtime input, it lowers to a runtime panic instead (`static_decl_kind::assertion`, `check.cpp:15100`).
- The optional `, "message"` form matches contract condition messages; see [Contracts](34-contracts.md).

## `static for`

Two forms: an inline expression-yielding form and a block-statement form.

```kira
def field_names[T]() -> list[str]:
    static for field in T.fields() => field.name
```

```kira
static for field in T.fields():
    println(field.name)
```

- The iterable must be a compile-time value (typically the result of reflection, see below). `static for` unrolls: one code path is generated per iteration, each with `field` bound to that iteration's concrete value.
- An optional guard filters iterations: `static for x in xs if pred(x) => ...`.
- `static_decl_kind::for_inline` and `static_decl_kind::for_block` are the two AST forms (`ast.h:1836`); both are handled in `check.cpp:15145`–`15211`.
- This is the mechanism `deriving` is built from: reflect over a type's fields, unroll, and (typically combined with quoting) emit one code fragment per field.

## Compile-time reflection

```kira
T.fields()          # list of field descriptors
T.field_count()     # number of fields (compile-time integer)
T.name()            # name of the type as str
```

- Reflection is implemented in `src/comptime/reflect.cpp` as a read-only traversal of the type's already-parsed declaration syntax — it does not consult the type checker's `type_table`.
- Because Kira monomorphizes generics, the type `T` a reflective call queries is always concrete by the time it is evaluated, so the result is an ordinary compile-time constant.
- Reflection may only invoke `pure` functions and is itself referentially transparent — see [Compile-Time Semantics § Reflection](32-compile-time-semantics.md#reflection) for why this matters for contracts.

## `pure` functions

`pure` asserts a function is referentially transparent — same inputs, same output, no side effects — and the compiler verifies the claim.

```kira
pure def clamp(x: float64, lo: float64, hi: float64) -> float64:
    if x < lo: return lo
    if x > hi: return hi
    return x
```

- `pure` and `static` are independent modifiers (two separate axes — see [Compile-Time Semantics](32-compile-time-semantics.md#two-axes-phase-and-effect)). A function may be `pure` only, `static` only, or `static pure`.
- Only calls to `pure` functions are legal inside `pre`/`post`/`invariant` conditions (`check.cpp:4430` rejects a non-pure call `in_contract_`). See [Contracts](34-contracts.md).
- A `pure` lambda uses `pure` before the arrow: `let square = pure x => x * x`.

```kira
static pure def align_up(n: usize, align: usize) -> usize:
    (n + align - 1) & ~(align - 1)

type aligned_buf[n: usize] = array[byte, align_up(n, 16)]
```

## Quoting and splicing

A backtick captures syntax as a compile-time value; `~` inserts a compile-time value's syntax at the splice site.

```kira
static let increment: expr = `(x + 1)`

def apply(x: int32) -> int32:
    return ~increment    # equivalent to: return x + 1
```

Quote value types (`ast::quote_fragment_kind`):

| Type | Captures |
|---|---|
| `expr` | a single expression |
| `stmt` | a single statement |
| `def_expr` | a definition (function, type, impl, etc.) |
| `type_expr` | a type expression |

All are compile-time only; none has a runtime representation.

Building expressions programmatically, via `expr.lit`, `expr.field`, `expr.ty`, and friends:

```kira
static def make_adder(n: int32) -> expr:
    `(x + ~(expr.lit(n)))`

static let add5 = make_adder(5)

def apply(x: int32) -> int32:
    return ~add5    # equivalent to: return x + 5
```

Generating definitions — combined with `static for` and reflection, this is how `deriving` works internally:

```kira
static def derive_show[T]() -> def_expr:
    let fields = T.fields()
    let parts  = for f in fields =>
        `(~(expr.lit(f.name)) + ": " + self.(~(expr.field(f))).show())`
    let body   = parts.join(` + ", "`)
    `impl show for ~(expr.ty[T]()):
        def show(self) -> str:
            ~body`

~derive_show[point]()    # emits the impl at this point in the module
```

### Hygiene

Quotes are hygienic by default: a plain (non-destructuring) `let`/`var` binding introduced *inside* a quoted fragment is renamed to a fresh, globally-unique synthetic name before splicing, so it cannot capture or be captured by a same-named binding at the splice site (`src/comptime/hygiene.{h,cpp}`, `rename_internal_bindings`).

- Deliberately narrow: only plain-name `let`/`var` bindings are renamed. Destructuring patterns, `for`/`while let`/`match` pattern bindings, lambda parameters, and struct-literal shorthand fields are left unrenamed — a free reference to such a name resolves against the splice site, not the quote's own definition site.
- A nested quote or splice found inside a fragment is left untouched; hygiene does not recurse into nested quotes.
- Use `expr.capture("name")` to deliberately capture a name from the surrounding context, opting out of hygiene for that one reference.

## Implementation status

Implemented end to end:

- `static` bindings, `static if`, `static assert`, `static for` (both forms) — `static_decl` and its four `static_decl_kind` variants, handled in `check.cpp` (`check_static_decl`, `~15074`–`15230`) and by the evaluator (`src/comptime/eval.cpp`).
- Compile-time reflection (`T.fields()`, `T.field_count()`, `T.name()`) — `src/comptime/reflect.cpp`.
- `pure` verification for functions and lambdas — enforced in `check.cpp` and required inside contract conditions.
- Quoting/splicing with all four fragment kinds (`expr`, `stmt`, `def_expr`, `type_expr`), reification at splice sites, and hygienic renaming of internal bindings — `src/comptime/hygiene.{h,cpp}`, `src/comptime/eval.cpp`, and the `quote_expr`/`splice_expr`/`splice_stmt`/`splice_type` node kinds in `check.cpp`.

Documented gap: resolving a quote's free references against its own definition site (the other half of full hygiene, as opposed to the splice site) is explicitly not implemented — `hygiene.h`'s own comment notes this is "still-unimplemented."

## See also

- [Compile-Time Semantics](32-compile-time-semantics.md) — the phase/effect model and evaluation/reasoning split these constructs run inside.
- [Dependent and Refinement Types](33-dependent-and-refinement-types.md) — uses `pure` functions as uninterpreted atoms in the reasoning solver.
- [Contracts](34-contracts.md) — `pure` requirement on `pre`/`post`/`invariant`.
