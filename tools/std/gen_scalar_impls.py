#!/usr/bin/env python3
"""Generates src/std/traits.scalar.cn: the builtin scalars' impls of the core
traits (eq, ord, show, debug, add, sub, mul, div, rem, neg).

Why these exist (spec/inference-rewrite.md phase 9): a generic body is checked
once, against its bounds, and a bound is checked at the call against the type
passed. For `describe[T](x: T) where T: show` called at `int32` to be sound,
`int32` has to *be* a `show` -- with a real `show` method the instance can call
-- not merely pass for one. Every body here is the builtin operation itself,
so none of them can recurse: the operators on a builtin number are primitive
(`checker::require_operand_trait` and `wire_ord_dispatch` dispatch only for
types without one), and interpolating a builtin formats it directly.

The 128-bit types are left out for the reason `traits.hash.cn` gives: the
bytecode VM has no representation for them, and a reachable module compiles in
full. `str` is left out because `std.string` already gives it `eq` and `cmp`
with its own signatures; the checker treats it as satisfying those traits.

Regenerate with:  python3 tools/std/gen_scalar_impls.py > src/std/traits.scalar.cn
"""

SIGNED = ["int8", "int16", "int32", "int64", "isize"]
UNSIGNED = ["uint8", "uint16", "uint32", "uint64", "usize", "byte"]
FLOATS = ["float32", "float64"]
NUMERIC = SIGNED + UNSIGNED + FLOATS
ALL = ["bool", "char"] + NUMERIC

ARITH = [("add", "+"), ("sub", "-"), ("mul", "*"), ("div", "/"), ("rem", "%")]


def main() -> None:
    out = []
    emit = out.append
    emit("module std.traits")
    emit("")
    emit("# GENERATED FILE -- do not hand-edit.")
    emit("# Produced by tools/std/gen_scalar_impls.py; see its doc comment for")
    emit("# why these impls exist and why each body cannot recurse.")
    emit("")
    for t in ALL:
        emit(f"impl eq for {t}:")
        emit(f"    def eq(self, other: &{t}) -> bool:")
        emit(f"        return self == *other")
        emit("")
    for t in NUMERIC + ["char"]:
        emit(f"impl ord for {t}:")
        emit(f"    def cmp(self, other: &{t}) -> ordering:")
        emit(f"        if self < *other:")
        emit(f"            return @less")
        emit(f"        if *other < self:")
        emit(f"            return @greater")
        emit(f"        return @equal")
        emit("")
    for t in ALL:
        emit(f"impl show for {t}:")
        emit(f"    def show(self) -> str:")
        emit(f'        return "{{self}}"')
        emit("")
        emit(f"impl debug for {t}:")
        emit(f"    def debug(self) -> str:")
        emit(f'        return "{{self:?}}"')
        emit("")
    for trait, op in ARITH:
        for t in NUMERIC:
            emit(f"impl {trait} for {t}:")
            emit(f"    def {trait}(self, other: {t}) -> {t}:")
            emit(f"        return self {op} other")
            emit("")
    for t in SIGNED + FLOATS:
        emit(f"impl neg for {t}:")
        emit(f"    def neg(self) -> {t}:")
        emit(f"        return -self")
        emit("")
    print("\n".join(out).rstrip() + "")


if __name__ == "__main__":
    main()
