# 23. Contracts on Trait Methods

**Status:** Partial

`pre`/`post` conditions as part of a trait method's interface, and the behavioral-subtyping rule implementations are checked against. General contract syntax (contracts on any function, not just trait methods) is specified in [Contracts](../advanced/34-contracts.md); this chapter covers only the trait-method interaction.

## Syntax

```kira
trait account:
    def withdraw(self, amount: int32) -> int32
        pre  amount > 0
        post return <= self.balance()
```

## Behavioral Subtyping

Every implementation is checked against the trait's declared contract under behavioral subtyping: an implementation may **weaken** a precondition (accept more) and **strengthen** a postcondition (promise more), never the reverse. This guarantees a caller relying on the trait's contract stays correct no matter which implementation runs. A default method's contract binds every override as well.

## Implementation status

`pre`/`post` contracts on ordinary function and method declarations are implemented and enforced: `src/semantic/check.cpp` checks each contract's condition as boolean (`in_contract_`/`in_postcondition_` state, `check.cpp:3350` and `check.cpp:13462`), assumes preconditions as facts available to the body, and proves postconditions before return (`collect_function_facts` and the predicate-reasoning machinery around `check.cpp:3300`–`3360`). See [Contracts](../advanced/34-contracts.md) for the general mechanism.

No evidence was found of a dedicated cross-check comparing an `impl`'s contract against the trait declaration's contract under the weaken-pre/strengthen-post rule specifically — no `check.cpp` code path was found that reads a trait method's contract and an implementing method's contract together and validates one implies the other. Each declaration's own contract is checked against its own body independently. Treat the behavioral-subtyping rule stated above as the target design; whether the compiler currently rejects an override that violates it (e.g. a `withdraw` override that tightens the precondition) is unconfirmed.

## See also

- [Traits](18-traits.md) — trait/`impl` mechanics.
- [Contracts](../advanced/34-contracts.md) — the general `pre`/`post` mechanism this chapter specializes.
