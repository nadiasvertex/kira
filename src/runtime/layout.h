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

} // namespace kira::runtime
