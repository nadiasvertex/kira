#pragma once

#include <span>
#include <string>
#include <vector>

#include "src/core/nodes.h"
#include "src/parser/source_location.h"
#include "src/semantic/checker_results.h"

/// The core verifier: a type checker for the typed core with no inference
/// (`spec/unified-checker-plan.md`, "Core verifier").
///
/// The verifier trusts nothing the elaborator decided. It reads the core, the
/// type table, the member types in `checked_types::shapes` (and `open_shapes`
/// for a type that mentions a generic parameter), and
/// `checked_types::is_copy`, and recomputes every fact below from them. Type
/// equality is equality of `type_id`. A violation is an internal compiler
/// error, never a user diagnostic.
///
/// Every rule has an id. A violation carries the id of the rule it breaks, and
/// each rule has a passing and a failing test in `verify_test.cpp`.
///
/// "Diverges" below is the definition in `nodes.h` (`expr`). `unit` is the
/// builtin `unit` type.
///
/// Types
///   T1  Every type in a node is settled: not `unknown`, `error` or an
///       inference variable, at any depth.
///   T2  No type is a refinement, an existential, or an unapplied constructor
///       at any depth. The elaborator erases the first two.
///   T3  A type parameter, a parameter application, or a symbolic value may
///       appear only inside a function that lists it in `generic_params`.
///
/// Places
///   P1  A `local` root names a symbol in scope at that point, and its type is
///       the type the symbol was declared with. A `global` root names a static
///       of the program with that type. A `temporary` root has a value, and
///       its type is the value's type.
///   P2  Each step is valid for the type before it and gives the type it says:
///         field           a struct; `index` and `name` agree with the shape
///         tuple_element   a tuple; `index` is in range
///         variant_payload a sum, `option` or `result`; `name` is a variant
///                         and `index` one of its payloads
///         deref           a `&T`, `&mut T`, `*T` or `*mut T`
///         element         an array, a slice, `str`, a raw pointer or an
///                         `uninit` buffer; `subscript` is an integer
///                         expression
///   P3  A `variant_payload` step appears only in a pattern binding's path,
///       under a `variant` test of the same variant at the same position.
///   P4  Writability is computed from the root and the path alone:
///         root   a `mut` local, a by-value parameter, a writable binding, a
///                `copy`, `move` or `borrow_mut` capture, and a temporary are
///                writable; any other local is an immutable binding; a global
///                is a static value
///         deref  of `&mut T` or `*mut T` makes the place writable whatever
///                came before; of `&T` or `*T` makes it behind a shared ref
///         element of a mutable slice, a `*mut T` or an `uninit` buffer is
///                writable; of a shared slice, a `*T` or `str` is behind a
///                shared ref
///         any other step keeps what came before
///
/// Access
///   A1  A `use` has the type of its place. Its mode is `copy` exactly when
///       that type is `copy`.
///   A2  A moved place has no `deref` of a reference and no `element` step of
///       an array, a slice or `str` in its path, and is not rooted at a
///       global, an `alias` binding or a borrowed capture. A `deref` or an
///       `element` of a raw pointer starts over: what the pointer points at
///       is storage of its own, so only the steps after it count. An
///       `element` of an `uninit` buffer may be moved. Both are the machine
///       layer's read. This holds for a `use` and for a `move` pattern
///       binding, whose place is the subject's path followed by its own. A
///       `move` capture obeys it too.
///   A3  A `borrow` of a place of type `T` has type `&T`, or `&mut T` when
///       `is_mut`.
///   A4  A mutable borrow's place is writable (P4).
///   A5  `two_phase` is set only on an inserted mutable borrow that is an
///       argument of a call.
///
/// Calls
///   C1  `signature` is a `fn` type. There is one argument per parameter, each
///       of exactly the parameter's type, and the call has the result type.
///   C2  A `function` callee has a declaration. When the program holds the
///       core function for that declaration, the callee's signature and
///       receiver mode are that function's.
///   C3  A receiver mode other than `none` fixes the first parameter's type as
///       `receiver_mode` documents; `by_value` needs a `copy` type.
///   C4  A `value` callee has a value whose type is the signature or a `&`
///       of it, and no declaration.
///   C5  An `intrinsic` callee has a name and no declaration or value. The
///       name is an operation of the compiler's own (`intrinsics.h`), and the
///       signature and `generic_args` have that operation's shape; or it is
///       an `intrinsic def` of the program, and the signature is the declared
///       one (for a generic declaration, has as many parameters).
///   C6  `generic_args` are settled (T1, T2).
///   C7  A `function_ref` has a `function` callee and the type of its
///       signature.
///   C8  A callee whose template the program holds has as many `generic_args`
///       as the template has parameters. The checker does not record an
///       instance's arguments yet, so every call to an instance breaks this
///       until phase 5 of the plan; `core_known_violations.txt` lists it.
///
/// Expressions
///   E1  A literal's type fits its token: an integer or float type for a
///       number, `bool`, `str`, `char`, or `unit`.
///   E2  A `primitive` has one operator. Its operands are builtin scalars or
///       raw pointers, never a struct, sum, tuple, array, `str` or view.
///       Arithmetic and bitwise operators take two operands of the result
///       type; a shift takes an integer of the result type and an integer; a
///       comparison takes two operands of one type and gives `bool`; `and`,
///       `or` and `not` take and give `bool`; negation takes the result type.
///   E3  A `cast` converts between builtin scalars, between raw pointers
///       and `usize`, or from one raw pointer type to another.
///   E4  A tuple, array, struct or variant is built from one element per
///       member, each of the member's type. A tuple's, an array's and a
///       variant's are in member order. A struct's `field_indices` name each
///       field of the shape once, `field_names` agree with them, and element
///       `i` has the type of field `field_indices[i]`. An array with `repeat`
///       has one element and `repeat` equal to its length.
///   E5  A block has the type of its result, or `unit` without one, unless it
///       diverges. A `let` is in scope for the statements after it and for the
///       result, and nowhere else.
///   E6  An `if` condition is `bool`. With an `else`, each branch that does
///       not diverge has the `if`'s type. Without one, the `if` and its branch
///       are `unit`.
///   E7  Each `match` test has the type of the part it tests, starting from
///       the subject's type. A `range` test has at least one bound. An arm has at least one alternative. Every
///       alternative of an arm binds the same symbols, with the same type,
///       mode and `writable`.
///   E8  A binding's path is valid from the subject's place (P2, P3) and its
///       type is the type of the part. `copy` needs a `copy` type and a part
///       A2 would let move; `move` obeys A2; a writable `alias` needs the
///       part to be writable (P4).
///   E9  A guard is `bool`. Each arm body that does not diverge has the
///       `match`'s type.
///   E10 A lambda's type is `fn(params) -> return_type`, and its body has
///       `return_type` unless it diverges. Each capture's place is valid in
///       the enclosing scope and has the capture's type. A `copy` capture
///       needs a `copy` type; `borrow_mut` needs a writable place. Inside the
///       body a local root names only the lambda's parameters, captures and
///       own bindings.
///   E11 `await` takes a task and has its result type.
///
/// Statements
///   S1  A `let`'s value, when present, has the `let`'s type.
///   S2  An assignment's value has the target's type. The target is writable
///       (P4), or it is a whole local whose `let` had no value.
///   S3  A loop condition is `bool`. `break` and `continue` appear only inside
///       a loop of the same function or lambda.
///   S4  `return` takes a value of the enclosing function's `body_type`, or of
///       the enclosing lambda's `return_type`; it takes none only when that
///       type is `unit`. `yield` appears only in a generator and takes its
///       `yield_type`.
///   S5  A contract check's condition is `bool`.
///
/// Functions
///   F1  Every symbol is introduced once in a function: by a parameter, a
///       `let`, a capture, or the bindings of one match arm.
///   F2  The body has `body_type` unless it diverges.
///   F3  The receiver mode fits the first parameter (C3). A function without
///       generic parameters mentions none (T3).
///   F4  A static's value has the static's type and is built only from
///       literals, aggregate and variant construction, and calls.
///
/// Not checked yet: that a generic call's arguments meet the callee's bounds
/// (the satisfaction query arrives in phase 6), and that a `match` is
/// exhaustive.
namespace cinder::core {

struct violation {
  std::string rule;     ///< The id of the broken rule, such as "P2".
  std::string module;
  std::string function; ///< Empty for a static.
  source_span span;
  std::string message;
};

/// Checks every function and static of `program` against the rules above.
/// Returns the violations found, in program order; empty when the program is
/// well formed.
[[nodiscard]] auto verify(std::span<const module *const> program,
                          const semantic::checked_types &checked)
    -> std::vector<violation>;

} // namespace cinder::core
