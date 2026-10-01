#pragma once

#include <functional>
#include <string>
#include <vector>

#include "src/hir/nodes.h"
#include "src/semantic/types.h"

namespace cinder::hir {

/// One place where lowered HIR uses a reference as the value it refers to,
/// or a value where a reference is expected, without an explicit
/// `hir_unary` deref or address-of between them.
struct reference_violation {
  std::string module;
  std::string function;
  source_span span = source_span::dummy();
  std::string what;
};

/// Finds every implicit reference conversion in `modules`: a projection
/// (`.f`, `[i]`, a payload) whose object is a reference, a direct call
/// whose argument and parameter disagree on being a reference, a `self`
/// parameter that is a heap-represented value rather than a reference (a
/// scalar `self` is passed by value), and a `return` or assignment whose
/// value and destination disagree.
///
/// Every `&`/`&mut` a program writes or implies must be explicit in HIR
/// before references to heap-represented values can become the address of
/// the place they borrow (`spec/todo.md` item 20): the backends then
/// compile an address-of and a deref as real operations, and an implicit
/// conversion would silently be the wrong one.
[[nodiscard]] auto find_implicit_references(
    const ptr_vec<hir_module> &modules, const semantic::type_table &types)
    -> std::vector<reference_violation>;

/// A local holding a temporary value that must be dropped: what a
/// statement stored so it could borrow it, read a field of, or discard it.
/// `moved_paths` are fields moved out of it, which it no longer drops.
struct temporary {
  symbol_id symbol = k_invalid_symbol_id;
  type_id type = k_unknown_type;
  std::vector<std::vector<std::string>> moved_paths;
  /// The block whose first statement stores the temporary, where its live
  /// flag is set right after.
  hir_block *creation = nullptr;
  /// The temporary's live flag, when a jump can leave it alive or it is a
  /// `while let` subject's: set once it is stored, cleared once dropped.
  symbol_id live = k_invalid_symbol_id;
};

/// Appends the drop of one temporary to a statement list.
using drop_temporary_fn =
    std::function<void(const temporary &, ptr_vec<hir_node> &)>;

/// Makes every implicit reference conversion in `function` explicit, the
/// ones `find_implicit_references` reports: a projection through a
/// reference gets a `hir_unary` deref, and an argument, returned value or
/// assigned value whose reference-ness differs from its destination gets a
/// deref or an address-of. A value that is not a place is first stored in a
/// fresh local (`mint`), so the address-of has something to point at.
///
/// Parameter types come from each call's `target` declaration, or from the
/// callee's `fn` type for a call through a function value. A parameter whose
/// type is a bare type parameter is left alone: whether it is a reference
/// depends on the instance.
///
/// It also drops temporaries (ch. 17, When drops run): a value that is not
/// a place, stored so it can be borrowed or a field read from it, or
/// computed and discarded, is dropped with `drop_temporary` at the end of
/// its statement. A condition, a match guard, the right operand of
/// `and`/`or`, a returned value and a block's value are full expressions of
/// their own, whose temporaries are dropped as soon as they are evaluated.
/// A `while let` subject's temporaries last one iteration. A `return`,
/// `break`, `continue` or `?` that leaves a statement early drops the
/// temporaries it has made so far, under run-time live flags, in the
/// placeholder lowering leaves before the jump (`hir_block::exit`). The
/// ownership checker ends the same temporaries at the same points
/// (`semantic::ownership`), so nothing still borrows one when it drops.
auto make_references_explicit(hir_function &function,
                              const semantic::checked_types &checked,
                              const std::function<symbol_id()> &mint,
                              const drop_temporary_fn &drop_temporary) -> void;

} // namespace cinder::hir
