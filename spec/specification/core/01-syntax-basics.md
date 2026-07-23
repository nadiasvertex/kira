# 1. Syntax Basics

**Status:** Implemented

Covers indentation-delimited blocks, comments and doc comments, and the `module` declaration line.

## Indentation and blocks

Kira has no semicolons and no braces for blocks. A block is introduced by `:` at the end of a line and one level of indentation; it ends when indentation returns to the enclosing level. The lexer synthesizes `INDENT`/`DEDENT`/`NEWLINE` tokens from indentation directly (see `INDENT`, `DEDENT` in `spec/kira-grammar.ebnf`); newlines inside balanced brackets `()`, `[]`, `{}` are suppressed, so an expression may wrap across lines there without a continuation marker.

```kira
def greet(name: str) -> str:
    let message = "Hello, {name}!"
    return message
```

All keywords are lowercase.

## Comments

An ordinary comment begins with `#` and runs to end of line (`COMMENT` in the grammar). It is discarded by the lexer and carries no semantic meaning.

```kira
# This is a comment
let x = 42    # so is this
```

## Doc comments

A comment beginning `#:` is a documentation comment. Written alone on one or more consecutive lines immediately above a declaration, it attaches to that declaration; the compiler retains the text for tooling to surface. Consecutive `#:` lines join into a single docstring. `#:` following code on the same line is an ordinary comment, not a doc comment.

Doc comments attach to functions, types, traits, `impl`/`extend` methods, struct fields, and the `module` line itself.

```kira
#: Adds two integers and returns the sum.
#: Overflow wraps in machine mode.
def add(a: int32, b: int32) -> int32:
    a + b
```

## The `module` declaration

Every file's first non-comment line is a `module_decl`: the keyword `module` followed by a dotted `module_path` (`module_path = IDENT { "." IDENT }`).

```kira
module my_app.utils
```

A module is one or more files sharing the same declared path; several files with the same `module` line together form one module. In a module path, every segment but the last names a folder; the final segment is the module's own name. `my_app.utils` is the module `utils`, found in folder `my_app/`. The file's own filename is not part of the module path and does not need to match it.

## See also

- [Modules and Imports](12-modules-and-imports.md) for visibility and `use`.
- `spec/kira-grammar.ebnf`, productions `file`, `module_decl`, `module_path`.
