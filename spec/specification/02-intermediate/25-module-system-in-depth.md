# 25. The Module System in Depth

**Status:** Partial

Modules spanning files, and project structure (`project.kira`, search paths, dependencies as `static` data).

## Modules Span Files

A module is not a single file, and modules do not nest. Any number of files may declare the same module; together they form it, and `module`-visible names are shared across all of them. Two modules whose paths share a prefix — `my_app.geometry` and `my_app.geometry.shapes` — are unrelated: the shared prefix is a shared folder, not a parent–child relationship. Neither can see the other's non-`pub` names.

To split a large module across files, give each file the same `module` line:

```kira
# my_app/geometry/core.kira
module my_app.geometry

pub type point = { pub x: float64, pub y: float64 }
```

```kira
# my_app/geometry/distance.kira
module my_app.geometry

pub def distance(a: point, b: point) -> float64:    # sees point directly
    ...
```

## Project Structure

A project's root is `project.kira`, ordinary Kira evaluated at compile time — no separate manifest language. Search paths and dependencies are `static` data:

```kira
module my_app

static search_path: list[str] = ["src", "vendor"]

static deps: list[dependency] = [
    dependency { name: "geometry", path: "vendor/geometry", version: "1.2.0" },
]
```

`dependency` is a type the build system provides. Because the manifest is Kira, dependency lists can be computed — assembled with `for`, branched on with `static if`, or factored into helper functions — in the same language as the rest of the program.

Dependencies are resolved at compile time, and a `project.lock` file records the resolved versions. What a package exposes to its dependents is determined by the `pub` surface of its modules, discovered through compile-time reflection — no separate list of exported modules to maintain.

## Implementation status

- Modules spanning files is implemented: `src/semantic/module_index.cpp` builds the cross-file module graph and `detect_duplicate_module_paths` (invoked from `src/semantic/analysis.cpp`) validates module-path conflicts across files, so multiple files sharing one `module` line joining into one module is real, checked behavior.
- `project.kira` as an evaluated manifest, `search_path`/`deps` as `static` data read by the build system, and `project.lock` are **not implemented**. No reference to a project manifest, search path, or dependency-resolution mechanism was found in `src/driver/cli.cpp` or elsewhere in the driver — the CLI takes source file paths directly (`bazelisk run //src:kira -- path/to/module.kira`), with no project-root discovery step. This subsection is design-only.

## See also

- [Traits](18-traits.md) — `pub` surface and visibility referenced above follow the same rules used for trait/type visibility.
