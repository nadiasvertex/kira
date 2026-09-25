# 12. Modules and Imports

**Status:** Implemented — see Implementation status

Covers `use`, visibility, and re-exporting with `pub use`. This is the basic module chapter; module-spanning-files and project structure are covered in the Intermediate module-system chapter.

## `use`

`use_decl`, `use_path`, `use_selector` in `spec/kira-grammar.ebnf`.

```kira
use my_app.geometry.point
use my_app.geometry.{ point, shape }   # multiple at once
use my_app.geometry.point as pt        # rename
use my_app as app                      # rename a root module
```

A `use_selector` may also be `*` for a wildcard import of everything a module exports.

## Visibility

The keyword set is `pub` / `module` / `file` (`ast::visibility` in `src/parser/ast.h`; `spec/kira-grammar.ebnf`'s `visibility` production):

```
pub     visible to any importer, anywhere
module  visible anywhere within the current module (declaring module and
        its descendants) — this is the default when no visibility modifier
        is written
file    visible only within the immediately enclosing file
```

`token_to_visibility` maps an absent modifier to `visibility::def`, which `module_index.cpp`'s `visibility_name` reports as `"module"` — confirming `module` is the true default, not a separate case from writing `module` explicitly.

`module` visibility reuses the same keyword spelling as the `module` keyword that introduces a submodule declaration (`sub_module_decl = [visibility] "module" IDENT ...`). The parser resolves the two uses with one token of lookahead: a `module` token immediately followed by an identifier is always the submodule-declaration keyword itself (a bare identifier never starts a declaration on its own), so `module module inner:` declares a submodule named `inner` with explicit `module` visibility, while a plain `module inner:` declares it with the (identical) default visibility.

```kira
module my_app.geometry

pub type point = { pub x: float64, pub y: float64 }

pub def distance(a: point, b: point) -> float64:
    let dx = a.x - b.x
    let dy = a.y - b.y
    sqrt(dx*dx + dy*dy)

file def scratch_helper() -> float64:    # file-private
    ...
```

## Re-exporting

`use_decl` itself takes an optional leading `visibility`; `pub use` brings a name in from another module and re-exports it as part of the current module's own public surface — a facade gathering names from several places into one import point.

```kira
module my_app

pub use my_app.geometry.{ point, shape }
pub use my_app.transform.rotate
```

Importers of `my_app` now see `point`, `shape`, and `rotate` directly. The standard library prelude itself is built this way: several `std.*` modules are re-exported into every file implicitly (`k_prelude_reexport_modules` in `src/semantic/check.cpp`).

## Visible Modules

A module path is only ever resolved against the modules **visible** in the current scope. From inside module `a.b`, the visible modules are:

- `a.b`'s declared child modules — each `sub_module_decl` in `a.b`, either bodiless (`module c`) or inline (`module c:` with a block);
- `a.b`'s ancestors, reached through its own root (`a.x`) or `super`;
- every module brought in by a `use` in the file, under its imported (or `as`-renamed) name;
- `std`, which every file sees as if it began with an implicit `use std`.

No other module is visible. A shared path prefix alone creates no relationship: a module `a.b.c` that `a.b` never declares or imports is not visible from `a.b`, and neither is another root module the file does not `use`. Naming one is an unresolved path, with help pointing at the missing `use`.

## Dotted Names

`a.b` is written the same way whether `a` is a value (field access) or a module (a qualified path). The meaning is decided by resolving the first segment:

1. **Local bindings shadow modules.** If the first segment names a binding in an enclosing lexical scope — a `let`/`var`, a parameter, `self`, a pattern binding in a `match` arm, `if let`, `while let`, or `for`, or a `static for` binder — the path is field access on that binding, even when a visible module has the same name. Shadowing is silent: code never has to know which module names exist elsewhere, so a user module named `s` cannot break a library function with a loop variable `s`. The same holds inside compile-time code: a `static def` parameter or a `let` in its body shadows a module exactly as it would at run time.
2. **Nothing else shares a name with a visible module.** No other name in a module's scope — a `def`, a `static` binding, a `type`, `trait`, `concept`, or `signature`, or a name brought in by `use` — may share its name with a module visible there: one the module *declares* as a child, or one the file *imports*. Two imports may not bind one name if either is a module. This is a compile-time error reported once, where the clash was introduced — at the declaration for a declared child, at the `use` for an import — never at the individual uses:

   ```kira
   module a.b

   module c              # declares child module a.b.c

   static c: int32 = 3   # error: `c` names both a static binding and the
                         #        module `a.b.c` visible in `a.b`
   ```

   The fix is to rename one of the two, or to import the module under another name (`use x.c as c_mod`, or `use x as x_mod` for a root module). A module that exists in the program but that this module neither declares nor imports is no conflict. The module's own root and `std` are not checked: a path starting with either always means the module, so `module main` may declare `def main`.
3. **Module-level values.** If the first segment is not the module's own root or `std` and names a `def` or `static` of the module, or one the file imports, the path is field access on that value: `origin.x` reads a field of `static let origin`. Rule 2 guarantees such a name is never also a visible module.
4. **Otherwise** the first segment is looked up among the visible modules, and the rest of the path is resolved inside that module. When the path reaches a `static` binding before it ends, the remaining segments are field access on the static's value: `geo.origin.x`.

Because every rule is evaluated against a finite, locally-declared set — the enclosing lexical scopes, the module's own declarations, and the file's imports — whether a dotted name is ambiguous is decided entirely by the file it appears in.

## Implementation status

- `use`, visibility, and re-exporting are implemented.
- **Visible Modules** and **Dotted Names** are implemented. The checker alone decides what a dotted name means (`checker::classify_dotted_root`, `src/semantic/check.cpp`) and records value-rooted paths for lowering (`checked_types::value_path_types`); module-rooted ones are validated through `validate_module_reference` (`src/semantic/resolution.cpp`), and both path validation and the checker's own lookups apply the one visibility rule, `root_visible_without_import`. The compile-time evaluator asks the checker through `comptime::evaluator::set_path_resolver`, supplying only whether the root is one of its own frame locals. Rule 2 is `validate_module_name_conflicts`.
- A field read through a module-level `static` (rules 3 and 4) lowers when the value read is a scalar, which is embedded as a constant. A non-scalar part of a struct-valued `static` (`origin` itself, or `seg.a` where `a` is a struct) has no runtime storage yet; only arrays and lists of scalars are reified as globals.

## See also

- Module System In Depth (Intermediate) — files spanning a module, project structure, `project.kira`.
- [Programs and `main`](13-programs-and-main.md) — the entry module the compiler starts from.
