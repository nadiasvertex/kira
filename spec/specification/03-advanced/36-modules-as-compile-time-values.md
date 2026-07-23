# 36. Modules as Compile-Time Values

**Status:** Implemented

A module is a compile-time record of the types, functions, and constants it defines: it can be described by a `signature`, parameterized as a *functor*, and reflected on — all before code generation, with no runtime representation.

## Syntax

Grammar: `signature_decl`, the parameterized form of `sub_module_decl`, and the argumented form of `use_path` in `spec/kira-grammar.ebnf`.

```ebnf
signature_decl
    = [ visibility ] "signature" IDENT ":" NEWLINE
          INDENT { signature_item } DEDENT ;

signature_item
    = "type" IDENT [ ":" bound ] NEWLINE
    | [ visibility ] func_signature NEWLINE
    | [ visibility ] "static" IDENT ":" type_expr NEWLINE ;

sub_module_decl
    = [ visibility ] "module" IDENT [ type_params ] NEWLINE
    | [ visibility ] "module" IDENT [ type_params ] ":" NEWLINE
          INDENT { top_level_item } DEDENT ;

use_path
    = module_path [ "[" type_arg_list "]" ] "." use_selector
    | module_path [ "[" type_arg_list "]" ] ;
```

`signature` is a reserved keyword. A module parameter's signature bound uses the ordinary `type_param` `: bound` syntax (`module audited[DB: backend]`); `backend` resolves in the module/type namespace, where signatures, traits, and concepts all live. `use audited[postgres] as db` combines an argument list with the existing `as` alias; nested instantiation (`audited[cached[postgres]]`) parses because the argument list reuses `type_arg_list`.

## Semantics

### Signatures

A `signature` lists required members — types, function signatures, constants — without implementing them:

```kira
signature backend:
    type conn
    def connect(url: str) -> conn
    def query(c: &conn, sql: str) -> result[rows, db_error]
    def close(c: conn) -> unit
```

- A signature name is a new symbol kind (`signature_symbol`) in the module/type namespace. It resolves as a bound target but is never interned as a type.
- A signature body is checked once. An abstract `type conn` is an opaque type local to the signature (modeled on a type parameter) usable by later members.

**Structural satisfaction.** A module satisfies a signature — concept-style, with no `impl` — iff, after binding each abstract signature type to the module's same-named `pub` type:

- each required `type T` has a corresponding `pub type T`;
- each required `def f(params) -> ret` has a `pub def f` whose interned signature equals the required one after substituting each abstract type with its bound concrete type;
- each required `static C: T` has a `pub static C` of type `T` under the binding.

Deep parameter/return-type equality is checked (not just member existence, visibility, and arity) — the signature's types are resolved in the argument module's own resolve context, and each pair compared by interned `type_id`; an unknown/error type on either side never manufactures a false mismatch. The diagnostic on failure lists every missing or mismatched member, not just the first, each with expected-vs-found.

### Parameterized modules (functors)

```kira
module audited[DB: backend]:
    pub def query(c: &DB.conn, sql: str) -> result[rows, db_error]:
        audit_log(sql)
        DB.query(c, sql)
```

A `module` declaration with type parameters registers as a **functor**: visible by name (symbol kind `submodule_symbol`, flagged as a functor), its body not elaborated by ordinary per-module checking, and skipped by the module graph.

Instantiation, at `use audited[postgres] as db`:

```kira
use audited[postgres] as db

let rows = db.query(&conn, "select 1")   # audited, backed by postgres
```

1. Resolve the functor and each argument; check each module argument `satisfies` its signature bound.
2. **Applicative, memoized instantiation.** `audited[postgres]` names the same module everywhere it appears. Instantiation is a pure function of `(functor, canonical-arg-key)` — built from the functor's fully-qualified name plus each argument's canonical form — cached session-wide. Two files that both `use audited[postgres]` see one instantiated module and one `conn`.
3. If the key is already memoized, reuse the instantiated module. Otherwise, clone the functor body (distinct node identity per instantiation; no AST substitution of parameter references) and check it, with each module parameter bound as an **import alias** to its argument module in the clone's synthetic file:
   - value projections (`DB.query(...)`) resolve through the alias the same way an ordinary `use ... as` alias resolves;
   - type projections (`DB.conn`) resolve through an extension to `resolve_named_type`: a multi-segment type path whose head is an import alias to a session module rewrites to that module's absolute path and resolves there.
4. `def`, `type`, `static` (binding form), `impl`, `extend`, and `trait` members are all cloned. A non-binding `static` or an inline submodule falls back with a diagnostic. Types, traits, and statics register into the synthetic module before its functions, so return types, `bump` references, and trait bounds resolve.
5. Alias the result into the importing scope (`db`).

**Type identity.** A `DB.conn` projection substitutes to the *same interned `type_id`* as the argument module's `conn`; memoization guarantees one module identity per instantiation key, so two `use audited[postgres]` imports share one `conn`.

**Ordering for `impl`/`extend`/coherence.** `build_method_table` and `validate_impl_coherence` run once, before per-file checking, so functor-body `impl`/`extend` members must be registered before them. A pre-pass walks every file's `use m[args]`, registers each instantiation's cloned members (including `impls`/`extends`) into the program index; the method-table and coherence build then see them; the cloned bodies are checked afterward, against the complete table. Each impl's target type is projected per instantiation, so two instantiations implementing a trait for their own local type never collide; the coherence key incorporates the instantiation.

**Cycle detection.** A functor whose instantiation (directly or transitively) requires instantiating itself is diagnosed.

**Codegen.** Each cloned `def` is recorded as a `functor_instance` (`{decl, owner_module}`). After the driver lowers the real source files, cloned members owning the same synthetic module are grouped into one standalone `hir_module`, appended to the module set — a materialized functor instantiation is an in-memory module with no source file, and ordinary cross-module dispatch links call sites to it on both the bytecode VM and LLVM/AOT backends. `type` members are compile-time only; a scalar `static` is inlined at each reference.

**Modules are never values at runtime, nor in the checker's expression language.** `let m = audited[postgres]` is out of scope; the only binding forms are `use ... as` and module parameters. Modules never enter `type_table`.

### Reflecting on a module

```kira
M.functions()   # list of function descriptors
M.types()       # list of type descriptors
M.name()        # the module's name, as str
M.function_count()
M.type_count()
```

Each function/type descriptor is a `{name, is_pub}` value; visibility is exposed as a field rather than pre-filtered (the evaluator has no caller-module context to apply "pub from outside, all from inside" itself), so callers filter on `is_pub` for outside-the-module use. Reflection calls evaluate only in the compile-time evaluator. A synthetic functor-instantiation module is reflected exactly like a hand-written one, by its synthetic name.

Reflection plus `static for` generates code from a module's members, replacing dynamic dispatch with a table baked into the binary:

```kira
static COMMANDS: map[str, command] = map(
    for f in cli_commands.functions() => (f.name(), make_command(f))
)
```

### Selecting a module at compile time

`static if` around a `use` gives zero-cost dependency injection:

```kira
static if BUILD.test:
    use fake_io  as io
else:
    use real_io  as io
```

Both branches must satisfy the same signature for code using `io` to remain unchanged across the swap.

**Pipeline-ordering rule.** The module graph and `use` resolution run before type checking and compile-time evaluation; a `static if` condition gating a `use` would otherwise make the module graph depend on evaluation. The condition is folded in a dedicated pre-resolution pass that evaluates the taken branch and drops the untaken one before the module graph is built, so the graph's evaluation-dependence stays a bounded pre-pass rather than general interleaving. The pass touches only a `static if` whose branch contains a `use`; every other `static if` keeps its ordinary check-time branch selection. The condition is evaluated with a standalone, resolution-free evaluator (no globals registered), so only literal expressions fold (`static if true:`) — the taken branch is folded recursively, so a nested import-gating `static if` inside it is handled too. `BUILD.*` build flags and resolution-dependent statics are named by the rule but have no evaluator support yet; referencing one in an import-gating condition produces a diagnostic explaining the restriction rather than being evaluated.

## Example

```kira
signature backend:
    type conn
    def connect(url: str) -> conn
    def query(c: &conn, sql: str) -> result[rows, db_error]
    def close(c: conn) -> unit

module audited[DB: backend]:
    pub def query(c: &DB.conn, sql: str) -> result[rows, db_error]:
        audit_log(sql)
        DB.query(c, sql)

use audited[postgres] as db

let rows = db.query(&conn, "select 1")   # audited, backed by postgres
```

## Implementation status

Fully landed. Two v1 limits remain, worth noting for anyone hitting them:

- Reflecting an instantiated functor through its `use ... as` alias (`db.functions()`) is not wired — reflect the functor's own name instead.
- Two submodules that share a leaf name collide in the reflection registry (last one registered wins); fully-qualified names are unaffected.
- Abstract pre-checking of a functor body against its signature bounds (producing errors with zero instantiations) is not implemented — errors surface per-instantiation, pointing at the functor body with a note chaining to the instantiation site.

## See also

- [Higher-Kinded Traits](37-higher-kinded-traits.md) — a related compile-time-only abstraction over constructors rather than modules; both share the "erased before codegen" property.
