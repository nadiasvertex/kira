# 24. Trait Objects

**Status:** Partial

`box[trait]`, object-safety rules, and the `some trait` opaque-return-type comparison.

## Opaque Return Types: `some trait`

A function that returns a concrete type implementing a trait, but whose exact type is unnameable or unimportant, uses `some trait` as its return type. This is zero-cost static polymorphism: the compiler monomorphizes at each call site.

```kira
def make_shape() -> some drawable:
    return @circle(10.0)

def make_iter() -> some iterator[int32]:
    return range(0, 100).filter(x => x % 2 == 0).map(x => x * x)
```

The caller sees only the trait interface:

```kira
let s = make_shape()
s.draw()                    # works — s is known to implement drawable
# s.radius                  # error — concrete type is hidden
```

Because each call site receives a distinct monomorphized type, `some trait` values cannot be placed in a homogeneous collection without erasing them. Use a sum type (`@circle | @square`) for a closed, inspectable set of variants; use `box[trait]` when the set is open; use `some trait` when the concrete type is static but inconvenient to name.

## Trait Objects: `box[trait]`

Generic bounds resolve at compile time, so `[T: show]` is monomorphized and costs nothing. But some programs must hold values whose concrete types are not known together at compile time — a `list` of user-defined shapes, handlers registered by plugins loaded at run time. That is open-world polymorphism, and it cannot be monomorphized. For it, `box[trait]` is a **trait object**: an owned, heap-allocated value whose concrete type is erased behind the trait, its methods dispatched through a vtable.

```kira
trait drawable:
    def area(self) -> float64
# disk and square are structs that implement drawable

var shapes: list[box[drawable]] = []
shapes.push(box(disk { radius: 1.0 }))
shapes.push(box(square { side: 2.0 }))

let total = shapes.map(s => s.area()).sum()   # each call dispatches through the vtable
```

`box[fn(A) -> B]` is the special case where the trait is a callable (see [Closures and Capture](16-closures-and-capture.md)). Because coherence guarantees one implementation per type, a trait object's vtable is never ambiguous.

### Object Safety

A trait can be used as `box[...]` only when it is *object-safe*:

- Every method dispatches on `&self` or `&mut self`.
- No method takes type parameters of its own.
- No method takes or returns `self` by value (the concrete size is erased).

`box[drawable]` and `box[show]` work; `box[add]`, whose method takes `other: self` and returns `self.output`, does not. Add `send`/`share` as extra bounds (`box[drawable + send]`) to move a trait object across tasks.

### `some trait` vs. `box[trait]`

| Feature | `some trait` | `box[trait]` |
|---|---|---|
| Cost | Zero — monomorphized | Heap allocation + vtable |
| Use case | One concrete type, just unnameable | Open set of types, runtime choice |
| Can store in `list` | No — each call site has a different type | Yes — uniform type |
| Escapes a function | Yes | Yes |

Reach for `box` only at a genuinely open boundary. To merely *accept* any type implementing a trait, use a generic bound — monomorphized and free; `box` is for *storing or returning* values whose types are not known together.

## Implementation status

- `some trait` (general existential return types) **is implemented**: `semantic::type_kind::existential_kind` (`src/semantic/types.h`/`.cpp`), minted via `type_table::fresh_existential` and resolved by `resolve_existential_type`; method calls on an opaque-typed receiver are checked against the bound traits' declared methods and dispatched statically against the underlying concrete type (`infer_method_call`'s `existential_kind` case in `check.cpp`). This works for any function, not only `generator def`.
- `box[trait]` **is not implemented**. `box` exists only as a bare unary type constructor (`{.name = "box", .min_args = 1, .max_args = 1}` in `src/semantic/types.cpp`) with no object-safety checking, no vtable construction, and no lowering: no reference to `"box"` was found in `src/hir/`, `src/bytecode/`, or `src/llvm_codegen/`. No trait-object/vtable dynamic dispatch exists anywhere in the compiler. Neither the object-safety rules nor the `some trait`/`box[trait]` comparison table above can currently be exercised end-to-end — `box[trait]` is design-only content, same as `box[fn(A) -> B]` in [Closures and Capture](16-closures-and-capture.md).

## See also

- [Traits](18-traits.md) — the trait mechanism this builds on.
- [Closures and Capture](16-closures-and-capture.md) — `box[fn(A) -> B]` as the callable special case.
- [Generics and Inference](19-generics-and-inference.md) — the monomorphized alternative when the type set is closed/known.
