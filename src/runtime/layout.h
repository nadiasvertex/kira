#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/semantic/types.h"

namespace kira::runtime {

// ==========================================================================
//  Heap value layouts shared by `bytecode_compiler`/`bytecode::vm` and
//  `llvm_codegen` (spec/codegen-design.md Decision 3). Every non-scalar
//  Kira value is a single pointer into `bump_arena` memory (`arena.h`) to a
//  flat block of 8-byte slots, uniform regardless of each slot's own type —
//  a nested aggregate/heap value is itself just a pointer, so it fits in
//  one slot the same as any scalar. This trades some memory for not having
//  to hand-roll per-field alignment/packing logic in two backends for a
//  representation `CLAUDE.md` explicitly deprioritizes performance for:
//
//  - `str`:                  { u64 len; u8* data; }               (2 slots)
//  - `list[T]`:              { u64 len; u64 cap; T* data; }       (3 slots)
//  - fixed `array[T, N]`, tuple, struct:
//                            { slot_0; slot_1; ...; slot_{N-1}; } (N slots)
//  - sum type:               { i64 tag; <payload slots...>; }     (1 + max
//                            payload-slot-count across all variants)
//  - closure/`fn` value:     { u64 fn_slot; u64 env_ptr; }        (2 slots)
//
//  This header only answers "what slot index does field/variant X live
//  at" from a struct/sum type's *declaration* (field/variant name and
//  order, payload arity) — it deliberately does not resolve field *types*
//  (that machinery lives in `semantic::check.cpp`'s private
//  `struct_field_type`/`variant_payload_types`, which need generic
//  substitution this pass doesn't: every `hir_expr` this codegen touches
//  already carries its own fully-resolved `type_id`, so field/variant
//  *order* is all either backend needs to compute a slot index).
// ==========================================================================

/// Ordered field names of a struct-kind `id`, or empty if `id` is not a
/// struct (or has no resolvable definition).
[[nodiscard]] auto struct_field_names(const semantic::type_table &types,
                                      semantic::type_id id)
    -> std::vector<std::string_view>;

/// The slot index of field `name` within struct-kind `id`, or `nullopt` if
/// `id` is not a struct or has no such field.
[[nodiscard]] auto struct_field_slot(const semantic::type_table &types,
                                     semantic::type_id id,
                                     std::string_view name)
    -> std::optional<size_t>;

/// Ordered variant names of a sum-kind `id`, or empty if `id` is not a sum
/// type (or has no resolvable definition).
[[nodiscard]] auto sum_variant_names(const semantic::type_table &types,
                                     semantic::type_id id)
    -> std::vector<std::string_view>;

/// The 0-based tag of variant `name` within sum-kind `id`, or `nullopt` if
/// `id` is not a sum type or has no such variant.
[[nodiscard]] auto sum_variant_tag(const semantic::type_table &types,
                                   semantic::type_id id, std::string_view name)
    -> std::optional<int64_t>;

/// The payload slot count (arity) of variant `name` within sum-kind `id`,
/// or `nullopt` if `id` is not a sum type or has no such variant.
[[nodiscard]] auto sum_variant_payload_slots(const semantic::type_table &types,
                                             semantic::type_id id,
                                             std::string_view name)
    -> std::optional<size_t>;

/// The widest payload slot count across every variant of sum-kind `id`
/// (0 if `id` is not a sum type, or every variant is a unit variant) —
/// the payload-area size a `hir_variant_init`/allocation site must
/// reserve regardless of which variant it happens to construct, since one
/// sum-type value's heap block must be able to hold any of its variants.
[[nodiscard]] auto sum_max_payload_slots(const semantic::type_table &types,
                                         semantic::type_id id) -> size_t;

// ==========================================================================
//  Byte-precise layout — size/alignment/offset, as opposed to the ordinal
//  slot-index API above. Added to generalize `array[byte,N]`'s pre-existing
//  tight-packing special case into the general rule (every `array[T,N]` is
//  N contiguous elements of T's own natural size), to give struct field
//  layout real alignment/padding (or, with the `packed` modifier, none at
//  all) instead of the uniform "every field costs 8 bytes" scheme above, and
//  (below) the same treatment for tuples. Sum-type payloads deliberately
//  stay on the ordinal/uniform scheme (`sum_*` above) — not in scope for
//  this pass.
// ==========================================================================

/// A type's natural (or, for a `packed` struct, packed) size and alignment
/// in bytes.
struct layout_info {
  size_t size_bytes = 0;
  size_t align_bytes = 1;
};

/// The size/alignment `id` occupies as a field/element of a contiguous
/// block: builtin scalars get their natural width (`bool`/`byte`/`int8`/
/// `uint8` = 1, `int16`/`uint16` = 2, `int32`/`uint32`/`char` = 4,
/// `int64`/`uint64`/`isize`/`usize`/`float64` = 8, `float32` = 4, `unit` =
/// size 0 align 1); every heap-referenced kind (`str`, prelude generics,
/// tuple, array, struct, sum, `fn`/closure, `ref`/`ptr`) is pointer-sized/
/// aligned (8/8) here — this doesn't change what a value looks like on its
/// own, only what it costs as one field/element of another aggregate.
/// `nullopt` only for a genuinely unrepresentable scalar (`int128`/
/// `uint128`/`float128` — mirrors `bytecode::numeric_kind_of`'s existing
/// `nullopt` gap for the same three types) or an unresolvable type.
[[nodiscard]] auto layout_of(const semantic::type_table &types,
                             semantic::type_id id)
    -> std::optional<layout_info>;

/// Whether struct-kind `id`'s declaration carries the `packed` modifier
/// (`ast::type_modifiers::is_packed`). `false` for a non-struct `id`.
/// The `N` of a `uninit[T, N]`, or `nullopt` when the slot count is not a
/// definite compile-time constant (an un-instantiated generic template).
/// `N` is a const-generic *value* argument, so it lives in the entry's
/// second type argument as a `const_value_kind` rather than in
/// `array_size` the way a fixed `array[T, N]`'s length does — which is the
/// only reason both backends need a named helper for it instead of reading
/// the field directly.
[[nodiscard]] auto uninit_slot_count(const semantic::type_table &types,
                                     const semantic::type_entry &entry)
    -> std::optional<uint64_t>;

[[nodiscard]] auto is_struct_packed(const semantic::type_table &types,
                                    semantic::type_id id) -> bool;

/// The whole-struct size/alignment of struct-kind `id`: fields placed in
/// declaration order, each rounded up to its own alignment before being
/// placed (no rounding at all, back-to-back, if `is_struct_packed`), final
/// size rounded up to the widest field's alignment (packed: no rounding,
/// align 1). `{0, 1}` for a non-struct or field-less `id`.
[[nodiscard]] auto struct_layout(const semantic::type_table &types,
                                 semantic::type_id id) -> layout_info;

/// The byte offset of field `name` within struct-kind `id`, computed by the
/// same padded/packed algorithm as `struct_layout`. `nullopt` if `id` is not
/// a struct, has no such field, or a field's own layout is unrepresentable
/// (see `layout_of`).
[[nodiscard]] auto struct_field_offset(const semantic::type_table &types,
                                       semantic::type_id id,
                                       std::string_view name)
    -> std::optional<size_t>;

/// The whole-tuple size/alignment of tuple-kind `id`: elements placed in
/// declaration order, each rounded up to its own alignment before being
/// placed, final size rounded up to the widest element's alignment — the
/// same padded algorithm as `struct_layout`, just walking `type_entry.args`
/// (a tuple's element types are already fully-resolved `type_id`s, unlike a
/// struct field's declared `ast::type_expr`, so no `field_layout`/generic-
/// substitution machinery is needed here). `{0, 1}` for a non-tuple or
/// element-less `id`.
[[nodiscard]] auto tuple_layout(const semantic::type_table &types,
                                semantic::type_id id) -> layout_info;

/// The byte offset of element `index` within tuple-kind `id`, computed by
/// the same algorithm as `tuple_layout`. `nullopt` if `id` is not a tuple,
/// `index` is out of range, or an element's own layout is unrepresentable
/// (see `layout_of`).
[[nodiscard]] auto tuple_element_offset(const semantic::type_table &types,
                                        semantic::type_id id, size_t index)
    -> std::optional<size_t>;

/// Grows (if necessary) the `list[T]` value whose 3-slot header
/// `{ u64 len; u64 cap; T* data; }` (this file's top comment) begins at
/// `header` so it has room for one more `elem_size`-byte element (1/2/4/8 —
/// `runtime::layout_of(element_type).size_bytes`, generalizing what used to
/// be a hardcoded `sizeof(uint64_t)` element width so a `list[bool]`/
/// `list[int16]` doesn't waste 8 bytes per element the same way an array's
/// own element storage no longer does), bumps `len`, and returns a pointer
/// to the newly-reserved element's address (`data + old_len * elem_size`)
/// — the caller stores its own `elem_size`-byte payload there directly.
/// Growth allocates a fresh, larger `data` block via `global_arena()`
/// (copying every existing element across, `old_len * elem_size` bytes)
/// when the list is already at capacity — starting capacity 4, doubling
/// thereafter, the same "simplest thing that works" placeholder growth
/// strategy `bump_arena` itself already uses. Returning an address for the
/// caller to store through (rather than taking the value as a parameter
/// here) mirrors how every other heap write in this codebase —
/// `str`/tuple/struct/sum-payload construction — is a plain store at a
/// computed address, never a value-passing runtime call: it means this
/// function doesn't need to know or care whether the pushed value is a
/// scalar bit pattern or a heap pointer, only how many bytes it is. Returns
/// `void*`, not `uint64_t*`, since the reserved address is not generally
/// 8-byte-aligned once `elem_size` can be 1/2/4. A plain C++ function so
/// `bytecode::vm` can call it directly with no ABI boundary to cross
/// (mirrors `global_arena()`'s own doc comment in `arena.h`);
/// `kira_rt_list_reserve_slot` below is the `extern "C"` wrapper generated
/// IR calls instead.
[[nodiscard]] auto list_reserve_slot(uint64_t *header, size_t elem_size)
    -> void *;

/// `extern "C"` wrapper around `list_reserve_slot` for generated IR to call
/// by name (`src/llvm_codegen/codegen.h`'s `kListReserveSlotSymbolName`) —
/// mirrors `kira_rt_alloc` (`arena.h`)'s JIT/AOT-boundary rationale exactly.
extern "C" auto kira_rt_list_reserve_slot(uint64_t *header, uint64_t elem_size)
    -> void *;

} // namespace kira::runtime
