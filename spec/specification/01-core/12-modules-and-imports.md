# 12. Modules and Imports

**Status:** Implemented

Covers `use`, visibility, and re-exporting with `pub use`. This is the basic module chapter; module-spanning-files and project structure are covered in the Intermediate module-system chapter.

## `use`

`use_decl`, `use_path`, `use_selector` in `spec/kira-grammar.ebnf`.

```kira
use my_app.geometry.point
use my_app.geometry.{ point, shape }   # multiple at once
use my_app.geometry.point as pt        # rename
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

## See also

- Module System In Depth (Intermediate) — files spanning a module, project structure, `project.kira`.
- [Programs and `main`](13-programs-and-main.md) — the entry module the compiler starts from.
