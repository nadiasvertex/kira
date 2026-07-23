# 19. Generics and Inference

**Status:** Implemented

Generic function bounds (`[T: show]`, `+` for multiple bounds) and the full type-argument inference order: sources, propagation, explicit arguments, bracket disambiguation, and static dispatch through a solved type parameter.

## Bounds

```kira
def print_item[T: show](item: T) -> unit:
    println(item.show())

def largest[T: ord](items: &list[T]) -> option[usize]:
    if items.is_empty(): return @none
    var best = 0
    for i in 1..items.len():
        if items[i] > items[best]: best = i
    return @some(best)    # return the index — a borrow could not escape this call
```

`[T: show]` means "`T` is any type that implements `show`." Multiple bounds use `+`:

```kira
def inspect[T: show + eq](a: T, b: T) -> unit:
    if a == b:
        println("equal: {a.show()}")
    else:
        println("{a.show()} != {b.show()}")
```

## Inference Order

For each type parameter, the checker tries these sources in order and stops at the first that answers. A parameter already bound by an earlier source is never revisited by a later one.

1. **An explicit type argument.** `zeros[8]()` says `n` is 8, and that settles it. `f[T](x: T)` called as `f[int64](5i32)` uses `int64` even though the argument alone would have suggested `int32`, and a genuine conflict (argument incompatible with the explicit argument) is reported as a type mismatch.
2. **`unify_rigid` against the argument types.** `print_item(3)` solves `T := int32` because `item: T` was given an `int32`.
3. **An enclosing instance's fixed bindings**, when checking one generic body from inside another.
4. **The expected type at the call site**, as a fallback only. A parameter that appears only in the return type has nothing in the arguments to solve it:

```kira
let names: list[str] = words.iter().collect()   # C is list[str], from the annotation
return counts.iter().collect()                  # C is the declared result type
```

### The expected type is a fallback, never an override

Arguments always win. An annotation that disagrees with the arguments is reported as a mismatch rather than quietly changing what the call means:

```kira
let x: int64 = take(5i32)   # error: expected int64, found int32
                            # T was solved to int32 from the argument
```

This is deliberate, not incidental: `k_unknown_type` unifies with everything (see [Traits](18-traits.md) and the type-checking overview), so the checker has no way to distinguish a hint it is confident in from a hint standing in for "not known yet." If a hint could override an argument-derived binding, `let x: int64 = take(5i32)` would silently re-solve `T := int64` instead of reporting the mismatch, and any unknown-typed context could quietly rewrite a well-understood call. Fallback-only means adding the expected-type source can only turn errors into successes, never successes into different successes — annotations are still checked against what inference found, because inference runs first and independently.

Where no expected type exists, there is nothing to infer from:

```kira
let xs = words.iter().collect()   # error: cannot tell which collection to build
                                  # help: annotate the binding, or write
                                  #       .collect[list[str]]()
```

### Where the expected type reaches

The expected type propagates to every position where the checker knows one: annotated `let`/`var` initializers, call arguments, `return` expressions, assignment right-hand sides, struct literal fields, collection elements, match arm results (joined against the other arms), and a lambda body with a declared result type. An unannotated `let` has no expected type and inference fails there, correctly.

## Explicit Type Arguments

Writing type arguments explicitly uses the same square brackets as the declaration, on both free functions and methods:

```kira
let xs = words.iter().collect[list[str]]()
let ys = zeros[8]()
```

A type argument may itself be a generic type — `list[str]` above is a single argument, not two.

## Bracket Disambiguation

Brackets carry three meanings: indexing, a const-generic value argument, and a type argument. The rule, in order:

1. If the base resolves in the value namespace to a local binding, parameter, or static, the brackets are **indexing**. A value shadowing a generic name means the user meant the value.
2. Otherwise, if the base resolves in the type namespace or names a generic callable, the brackets are a **compile-time argument list**.
3. Within a compile-time argument list, each argument's meaning is fixed by the *declaration's* parameter at that position, never by the argument's own syntax: a `[n: usize]` parameter takes a value argument, a bare `[T]` parameter takes a type argument. `f[list[int32]]` and `f[3]` are not distinguished by inspecting the brackets — the declaration of `f` says which is expected, and a mismatch is a diagnosable error, not an ambiguity.
4. Anything else is an error, not a fallback to indexing.

Types and values are kept in separate namespaces, so rule 1 is a single lookup. This is why type arguments use `[]` rather than `<>`: the `[]`/indexing collision is decidable by name lookup in one step, while `<>` collides with the comparison operators (`a < b, c > (d)` is simultaneously a valid expression and a valid instantiation) in a way no namespace lookup resolves, requiring unbounded lookahead or a disambiguating token (`::<>` in one language, the `template` keyword in another).

## Static Dispatch Through a Type Parameter

A trait's `static` member can be called through a type parameter once that parameter is solved:

```kira
pub trait from_iter[T]:
    static def from_iter[I: iterator[T]](it: I) -> self

pub def collect[I, C](self: I) -> C:
    return C.from_iter(self)
```

`C` is solved first (by the ordinary inference order above), and `C.from_iter` then resolves to the static member of the `impl` for the type `C` was solved to. Resolution substitutes the active binding for the type parameter and resolves the member against the resulting concrete type; inside an uninstantiated generic template, no binding exists yet, so the path resolves to `k_unknown_type` and is checked only once instantiated. `from_iter` needs no object safety — dispatch through a type parameter is always static and monomorphic, never boxed.

A bound like `C: from_iter` on such a function is not itself enforced at the call site: an unsuitable `C` fails where its body calls the missing static member, inside the instantiated generic, rather than at the point where `C` was chosen. The checker attaches an instantiation-site note pointing back at the call that solved `C`, and at what solved it, so the failure is still actionable even though it surfaces one level removed from the user's code.

## Implementation status

All of the above is implemented in `src/semantic/check.cpp`, sharing the `solve_generic_params` / `find_or_check_generic_instance` substrate:

- Expected-type solving: `solve_from_expected_type` (`check.cpp:5182`), invoked from both the free-function and receiver-call paths (`check.cpp:5248`, `check.cpp:5445`).
- Explicit type arguments on method calls: `method_explicit_generic_args` (`check.cpp:8516`), consumed at `check.cpp:9036` — brackets on a method call are honored, not discarded.
- Static dispatch through a type parameter: `type_param_slots_` (`check.cpp:1107` and its use sites) holds the active per-instantiation binding used to resolve a qualified path whose base names a type parameter.

## See also

- [Traits](18-traits.md) — the bounds generic parameters are checked against.
- [Trait Objects](24-trait-objects.md) — the dynamic alternative when a type set is open rather than solved at compile time.
