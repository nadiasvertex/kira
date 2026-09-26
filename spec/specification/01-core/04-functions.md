# 4. Functions

**Status:** Implemented

Covers `def`, parameter/return-type inference and its limits, named arguments and defaults, and functions as values.

## Syntax

`func_decl` in `spec/cinder-grammar.ebnf`.

```cinder
def add(a: int32, b: int32) -> int32:
    return a + b
```

The last expression of the body is the return value when no explicit `return` is reached (functions are expression-bodied like blocks generally are).

## Type inference

A parameter or return type may be omitted. When omitted, the compiler infers it **from the function's body only** — never from a call site — assigning each unannotated parameter the most general type its uses in the body allow.

```cinder
def double(x):
    return x * 2        # x can be any number; double works for all of them
```

An unannotated parameter is not dynamically typed; it is resolved at compile time to the widest type the body permits (an implicit generic — the Intermediate chapter on generic functions formalizes this). Annotating narrows it to one concrete type:

```cinder
def double(x: int32) -> int32:
    return x * 2        # now double is specifically for int32
```

Two rules bound inference:

1. **`pub` functions must annotate every parameter and the return type.** Diagnosed at the declaration (`public function \`{name}\` must annotate its parameters ...`, `src/semantic/check.cpp`). An exported function's signature is a contract for external callers, so it is written down rather than inferred.
2. **Inference never crosses a call.** A function's types are determined by its own body alone. When the body underdetermines a type, the compiler reports it and asks for an annotation rather than inferring from callers.

## Expression-bodied functions

When the whole body is a single expression, it can follow the `:` directly on the same line instead of an indented block. There is no separate syntax for this — `func_body` in `spec/cinder-grammar.ebnf` allows either an inline expression or an indented block after the colon, and the two are interchangeable everywhere a function body is expected.

```cinder
def add(a: int32, b: int32) -> int32: a + b

def square(x: int32) -> int32: x * x
```

This is the same rule that makes `add` from the opening example equivalent to:

```cinder
def add(a: int32, b: int32) -> int32:
    return a + b
```

Both `return a + b` and the bare expression `a + b` produce the same value; the inline form just skips the `return` and the indented block when the body is one expression. If the expression needs more than one line, use the block form instead.

## Named arguments and defaults

A parameter may declare a default value; a call may pass any argument by name (`name: value`) regardless of position.

```cinder
def greet(name: str, loud: bool = false) -> str:
  return "{name}!".to_uppercase() if loud else "Hello, {name}"

greet("Alice")                  # uses default: loud = false
greet("Bob", loud: true)        # named argument
```

A default value is evaluated **at the call site**, once per call that omits the argument — `def push(xs: list[int32] = [])` hands each such call its own fresh list, never one shared between them. It follows that a default may only name things the call site can also see (a literal, a `static let`, a function to call); it may not refer to another of the function's parameters, since those are values of a call that has not happened yet.

## Functions as values

A function name used as a value has type `fn(ParamTypes...) -> ReturnType` and may be passed, stored, and called like any other value.

```cinder
def apply(f: fn(int32) -> int32, x: int32) -> int32:
    return f(x)

let result = apply(double, 5)   # result = 10
```

## See also

- [Lambdas](05-lambdas.md) — anonymous functions, usable anywhere a `fn(...)->...` value is expected.
- Generic Functions (Intermediate) — the Layer 2 formalization of unannotated-parameter inference as implicit generics.
