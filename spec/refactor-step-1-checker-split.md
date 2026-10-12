# Refactor step 1: split the checker into files

This ticket splits `src/semantic/check.cpp` (about 30k lines, one class) into
a private header and 16 source files. No logic changes. The class declaration
and its member data move to `semantic/checker.h`. Method definitions move out
of the class body into files that follow the existing section banners, with a
few misplaced groups moved to where they belong. The public API in `check.h`
does not change. The work lands as a series of small commits, each of which
builds warning-free and passes `just test`. This is step 1 of
`spec/refactor-plan.md`.

Line numbers below are as of commit `dfeb304` and are approximate. They will
drift as other work lands, so the section names are the reliable reference.

## Mechanics

The checker stays one class for now. Only where its code lives changes.

- **Namespace.** The file-local anonymous namespace becomes
  `cinder::semantic::detail`, because several files now share its contents.
- **Private header.** `semantic/checker.h` holds the helper structs
  (`value_binding`, `fn_param_info`, `builtin_method_signature`,
  `method_entry`, `deref_site`), the class declaration, and all member data.
  It goes in the Bazel target's `srcs`, not `hdrs`, so nothing outside
  `semantic/` can include it.
- **Small helpers.** The free functions in the "Small helpers" section
  (lines 40–400) move to `semantic/check_helpers.h` and `.cpp`.
- **Out-of-line definitions.** Each inline member function becomes a
  declaration in the header and an `auto checker::name(...) -> T`
  definition in its file. The following rules apply:
  - Default arguments stay on the declaration only.
  - `[[nodiscard]]` stays on the declaration.
  - `const` qualifiers must match on both.
- **Templates.** The 10 member templates stay defined in the header,
  unless only one file uses a template. In that case it is defined in that
  file and declared in the header.
- **Doc comments.** Doc comments stay with the definitions. In the header,
  each block of declarations sits under a banner that names its source file,
  so the header doubles as a table of contents.
- **Header order.** The header keeps the existing state comments (session,
  file, functor, function, caches) as they are. Step 2 turns those into
  types.

A script or clang-based tool should do the move, not hand editing. That
reduces the risk of dropping a default argument or changing a signature.

## File split

Each file below lists the sections it takes from `check.cpp`. Sizes are
rough.

| File | Sections from `check.cpp` | Approx. lines |
|---|---|---|
| `check.cpp` | Constructor, `run`, `check_program`, and "Per-file entry" (29424–30273) without the comptime registration functions | 900 |
| `check_diagnostics.cpp` | "Diagnostics helpers" (1414–1992), plus `mark_file_error` | 650 |
| `check_scopes.cpp` | "Scope management" (1992–2237), "Module member lookup" (2237–2505), "Identifier resolution" (11673–12492) | 1330 |
| `check_type_resolution.cpp` | "Type resolution" (2505–2564), "Compile-time value slots" (3736–4044), "Struct / sum member queries" (5710–6124), "Function signatures" (6124–6405) | 1060 |
| `check_bounds.cpp` | "Facts about type parameters" with its "Domains" and "`static if` narrowing" parts (2564–3736), "Bounds at the call site" (10505–11193), and the bound-method group from "Declarations" (`check_bound_method_call` through `check_bound_arithmetic`, 26327–26886) | 2420 |
| `check_reasoning.cpp` | "Reasoning" with "Flow-sensitive narrowing" (4044–5710), and "Typed runtime proofs — `try_from`" (20211–21302) | 2760 |
| `check_generic_calls.cpp` | "Const-generic monomorphization" through "One solution, one instance", "Leaf unknowns", and "Generic lambdas" (6829–8943) | 2110 |
| `check_instances.cpp` | "Instances by substitution" (8943–10505), "Higher-kinded-trait method monomorphization" (11193–11673) | 2040 |
| `check_operators.cpp` | "Literals" (12492–12634), "Binary and unary operators" (12634–13818) | 1330 |
| `check_methods.cpp` | "Impl / method tables" (13818–15296), "Member lookup through `deref`" (18890–20211), plus `trait_declares_method` and `find_trait_anywhere` | 2850 |
| `check_calls.cpp` | "Call checking" (6405–6829), "Calls" (15296–16287) | 1410 |
| `check_ufcs.cpp` | "UFCS" (16287–18851) | 2560 |
| `check_expr.cpp` | "Field access and indexing" (18851–18890), "Struct literals" (21302–21673), "Blocks and body values" (21673–22756), "Expression dispatch" (22756–24092) | 2830 |
| `check_patterns.cpp` | "Patterns" (24092–24474), "Match checking and exhaustiveness" (24474–24676) | 580 |
| `check_stmt.cpp` | "Statements" (24676–25426) | 750 |
| `check_decls.cpp` | "Declarations": functions, intrinsics, and type declarations (25426–26281), then impls, extends, traits, concepts, signatures, and functors (26916–28348); `check_item` (29237–29326); and "Impl coherence (session-wide)" (29326–29424) | 2400 |
| `check_comptime.cpp` | The comptime part of "Declarations": `inside_unbound_generic_scope` through `check_static_decl` (28442–29237), plus `register_comptime_globals`, `register_comptime_modules`, and `register_comptime_coherence_info` from "Per-file entry" | 1100 |

The header, `checker.h`, will be about 2000 lines: about 700 of member data
and the rest declarations. Step 2 makes it smaller.

### Groups that move away from their current section

A few groups sit in the wrong section today. They move as follows:

- The bound-method functions in "Declarations" check calls, not
  declarations. They go to `check_bounds.cpp`.
- `trait_declares_method` and `find_trait_anywhere` are lookups. They go to
  `check_methods.cpp`.
- The `static for` / `static if` / `static` declaration code and the comptime
  registration functions go to `check_comptime.cpp`. This file is the one
  place the checker talks to the compile-time evaluator, which is where step 6
  of the plan (compile-time code on the VM) will make its changes.

Where a function in one section is only a helper for another section, it
moves with its caller. These cases come up during the move and are not listed
here.

## Order of commits

1. Rename the anonymous namespace to `detail` and move the small helpers to
   `check_helpers.{h,cpp}`.
2. Move the class declaration and member data into `checker.h`, keeping all
   method bodies inline. `check.cpp` includes it.
3. Move one file's worth of methods out of line per commit, in the table's
   order. After each commit the build is warning-free and `just test`
   passes.
4. When the class body holds only declarations, remove the inline section
   banners from `checker.h` and replace them with per-file banners.

## Verification

The change does not alter behavior, so the existing tests are the check:

- `just test` passes after every commit.
- `snapshot_test` and `check_test` pass unchanged. They cover diagnostic
  text and ordering, which are the most likely to shift if an overload or
  default argument changes during the move.
- The build is warning-free under the project flags, including `-Wshadow`
  and `-Wconversion`, which catch some signature mismatches.
- A full rebuild of `//src/semantic/...` completes. Build time for a
  one-line edit in a single file goes down noticeably compared with today.

## Out of scope

This ticket does not change logic, rename methods, merge duplicate code,
change which state a method can reach, or touch `check.h` or
`checked_types`. Those are steps 2 through 4 of the plan.
