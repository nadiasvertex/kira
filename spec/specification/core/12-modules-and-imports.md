# 12. Modules and Imports

**Status:** Implemented, with a naming discrepancy from the old tutorial noted below

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

**The real keyword set is `pub` / `internal` / `super` / `priv`** (`ast::visibility` in `src/parser/ast.h`; `spec/kira-grammar.ebnf`'s `visibility` production). This differs from the older tutorial prose, which described only `pub`/`module`/`file` — those `module` and `file` keywords do not exist in the grammar or lexer (`src/parser/token.h` has no such visibility keyword tokens). The correspondence to that older, informal terminology is approximate, not exact:

```
pub       visible to any importer, anywhere
internal  visible within the current package/compilation unit — this is the
          default when no visibility modifier is written (roughly what the
          old tutorial called "module" visibility)
super     visible only to the parent module scope (no equivalent in the old
          tutorial's two-level pub/module/file model)
priv      visible only within the immediately enclosing scope/file (roughly
          what the old tutorial called "file" visibility)
```

`token_to_visibility` maps an absent modifier to `visibility::def`, which `module_index.cpp`'s `visibility_name` reports as `"internal"` — confirming `internal` is the true default, not a separate case from writing `internal` explicitly.

```kira
module my_app.geometry

pub type point = { pub x: float64, pub y: float64 }

pub def distance(a: point, b: point) -> float64:
    let dx = a.x - b.x
    let dy = a.y - b.y
    sqrt(dx*dx + dy*dy)

priv def scratch_helper() -> float64:    # file-private
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
