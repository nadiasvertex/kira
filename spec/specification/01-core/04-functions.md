# 4. Functions

**Status:** Implemented

Covers `def`, parameter/return-type inference and its limits, named arguments and defaults, and functions as values.

## Syntax

`func_decl` in `spec/kira-grammar.ebnf`.

```kira
def add(a: int32, b: int32) -> int32:
    return a + b
```

The last expression of the body is the return value when no explicit `return` is reached (functions are expression-bodied like blocks generally are).

## Type inference

A parameter or return type may be omitted. When omitted, the compiler infers it **from the function's body only** — never from a call site — assigning each unannotated parameter the most general type its uses in the body allow.

```kira
def double(x):
    return x * 2        # x can be any number; double works for all of them
```

An unannotated parameter is not dynamically typed; it is resolved at compile time to the widest type the body permits (an implicit generic — the Intermediate chapter on generic functions formalizes this). Annotating narrows it to one concrete type:

```kira
def double(x: int32) -> int32:
    return x * 2        # now double is specifically for int32
```

Two rules bound inference:

1. **`pub` functions must annotate every parameter and the return type.** Diagnosed at the declaration (`public function \`{name}\` must annotate its parameters ...`, `src/semantic/check.cpp`). An exported function's signature is a contract for external callers, so it is written down rather than inferred.
2. **Inference never crosses a call.** A function's types are determined by its own body alone. When the body underdetermines a type, the compiler reports it and asks for an annotation rather than inferring from callers.

## Named arguments and defaults

A parameter may declare a default value; a call may pass any argument by name (`name: value`) regardless of position.

```kira
def greet(name: str, loud: bool = false) -> str:
    if loud:
        return "{name}!".to_uppercase()
    return "Hello, {name}"

greet("Alice")                  # uses default: loud = false
greet("Bob", loud: true)        # named argument
```

## Functions as values

A function name used as a value has type `fn(ParamTypes...) -> ReturnType` and may be passed, stored, and called like any other value.

```kira
def apply(f: fn(int32) -> int32, x: int32) -> int32:
    return f(x)

let result = apply(double, 5)   # result = 10
```

## See also

- [Lambdas](05-lambdas.md) — anonymous functions, usable anywhere a `fn(...)->...` value is expected.
- Generic Functions (Intermediate) — the Layer 2 formalization of unannotated-parameter inference as implicit generics.
