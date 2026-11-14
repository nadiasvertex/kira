#!/usr/bin/env python3
"""Generates src/std/traits.structural.cn: `eq` for the structural types --
tuples, `array[T, n]`, `option[T]` and `result[T, E]`.

Two values of one of these types are equal when their elements are, compared
in order with each element type's own `==`. That is all an impl here says, and
the impl's bounds (`A: eq`, ...) are what make the element types part of the
rule: `(int32, str)` is `eq`, and `(int32, fn() -> int32)` is not, which the
checker reports at the `==` rather than inside these bodies
(`checker::require_structural_operator`).

There is no `ord`: the specification defines no order on these types.

The language has no variadic generics, so each tuple length has its own impl,
up to 12 elements. A longer tuple is reported as having no `==`.

Regenerate with:
  python3 tools/std/gen_structural_impls.py > src/std/traits.structural.cn
"""

MAX_TUPLE = 12


def main() -> None:
    out = []
    emit = out.append
    emit("module std.traits")
    emit("")
    emit("# GENERATED FILE -- do not hand-edit.")
    emit("# Produced by tools/std/gen_structural_impls.py; see its doc comment")
    emit("# for the rule these impls state.")
    emit("")
    emit("impl[T: eq] eq for option[T]:")
    emit("    def eq(self, other: &option[T]) -> bool:")
    emit("        return match self:")
    emit("            @some(x) => match other:")
    emit("                @some(y) => x == y")
    emit("                @none => false")
    emit("            @none => match other:")
    emit("                @some(_) => false")
    emit("                @none => true")
    emit("")
    emit("impl[T: eq, E: eq] eq for result[T, E]:")
    emit("    def eq(self, other: &result[T, E]) -> bool:")
    emit("        return match self:")
    emit("            @ok(x) => match other:")
    emit("                @ok(y) => x == y")
    emit("                @err(_) => false")
    emit("            @err(x) => match other:")
    emit("                @ok(_) => false")
    emit("                @err(y) => x == y")
    emit("")
    emit("impl[T: eq, n: usize] eq for array[T, n]:")
    emit("    def eq(self, other: &array[T, n]) -> bool:")
    emit("        var i: usize = 0")
    emit("        while i < n:")
    emit("            if self[i] != other[i]:")
    emit("                return false")
    emit("            i = i + 1")
    emit("        return true")
    for arity in range(2, MAX_TUPLE + 1):
        names = [f"T{i}" for i in range(arity)]
        params = ", ".join(f"{name}: eq" for name in names)
        shape = "(" + ", ".join(names) + ")"
        compare = " and ".join(
            f"self.{i} == other.{i}" for i in range(arity))
        emit("")
        emit(f"impl[{params}] eq for {shape}:")
        emit(f"    def eq(self, other: &{shape}) -> bool:")
        emit(f"        return {compare}")
    print("\n".join(out))


if __name__ == "__main__":
    main()
