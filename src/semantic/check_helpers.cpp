#include "check_helpers.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Small helpers
// ==========================================================================

/// A variant-constructor expression (`@some(x)`) parses to an ident whose
/// span still covers the leading `@`; that lets us tell it apart from an
/// ordinary identifier of the same spelling.
auto is_variant_ident(const ast::ident_expr &ident) -> bool {
  return ident.span.len() > ident.name.size();
}

/// The key `k_builtin_methods` files a receiver under, or empty when the
/// receiver is not a type builtin methods are answered for. `slice_mut`
/// shares `slice`'s entries — the two differ in mutability, not in what
/// inherent methods they carry.
auto builtin_method_owner(const type_entry &object) -> std::string_view {
  if (object.kind == type_kind::builtin_kind && object.name == "str") {
    return "str";
  }
  // A fixed `array[T, N]` is its own data block, so `as_ptr`/`as_mut_ptr`
  // are meaningful on it exactly as they are on a `list`/`slice` -- it is
  // the one indexable shape that is not `builtin_generic_kind`.
  if (object.kind == type_kind::array_kind) {
    return "array";
  }
  if (object.kind != type_kind::builtin_generic_kind) {
    return {};
  }
  if (object.name == "slice" || object.name == "slice_mut") {
    return "slice";
  }
  if (object.name == "cell" || object.name == "cell_mut") {
    // Unlike `slice`/`slice_mut`, `cell` and `cell_mut` do *not* share one
    // bucket: `set` is only ever valid on a `cell_mut` receiver, and folding
    // both into one owner would offer it on a read-only `cell` too.
    return object.name;
  }
  if (object.name == "option" || object.name == "result" ||
      object.name == "generator" || object.name == "task" ||
      object.name == "crew" ||
      object.name == "uninit") {
    return object.name;
  }
  return {};
}

} // namespace cinder::semantic::detail
