# 23. Contracts on Trait Methods

**Status:** Partial

`pre`/`post` conditions as part of a trait method's interface, and the behavioral-subtyping rule implementations are checked against. General contract syntax (contracts on any function, not just trait methods) is specified in [Contracts](../03-advanced/34-contracts.md); this chapter covers only the trait-method interaction.

## Syntax

```cinder
trait account:
    def withdraw(self, amount: int32) -> int32
        pre  amount > 0
        post return <= self.balance()
```

## Behavioral Subtyping

Every implementation is checked against the trait's declared contract under behavioral subtyping: an implementation may **weaken** a precondition (accept more) and **strengthen** a postcondition (promise more), never the reverse. This guarantees a caller relying on the trait's contract stays correct no matter which implementation runs. A default method's contract binds every override as well.

## Implementation status

`pre`/`post` contracts on ordinary function and method declarations are implemented and enforced: the checker checks each contract's condition as boolean (`in_contract_`/`in_postcondition_` state, `src/semantic/check_decls.cpp`), assumes preconditions as facts available to the body, and proves postconditions before return (`collect_function_facts` and the predicate-reasoning machinery in `src/semantic/check_reasoning.cpp`). See [Contracts](../03-advanced/34-contracts.md) for the general mechanism.

The behavioral-subtyping rule is not enforced. No checker path compares an `impl`'s contract with the trait declaration's contract; each declaration's contract is checked against its own body independently. An `impl` that tightens the trait's precondition (trait `pre n >= 0`, impl `pre n >= 10`) compiles, and a call that satisfies only the trait's precondition fails the impl's check at run time.

## See also

- [Traits](18-traits.md) — trait/`impl` mechanics.
- [Contracts](../03-advanced/34-contracts.md) — the general `pre`/`post` mechanism this chapter specializes.
