#pragma once

#include <cstdint>

namespace cinder::runtime {

/// Which allocation strategy every Cinder heap allocation in this process
/// draws from. Both are complete implementations of the same three
/// operations, selectable at startup so a change to the allocator can be
/// A/B'd against the historical behavior without recompiling:
///
/// - `system`: `calloc`/`realloc`/`free`. Memory is genuinely reclaimed, so
///   `rt_free`/`rt_realloc` mean what they say. This is the default.
/// - `arena`: the original process-lifetime bump arena (`bump_arena`,
///   `arena.h`). `free` is a no-op and `realloc` always copies into a fresh
///   block, so a program's peak footprint is its total allocation. Kept
///   because it is the one configuration in which no allocation is ever
///   reused, which makes a use-after-free in generated code impossible and
///   therefore isolates an allocator bug from a codegen bug.
///
/// Selected by the `CINDER_ALLOCATOR` environment variable (`system` /
/// `arena`), read once on first allocation. An unrecognized value is the
/// default rather than an error: this is a diagnostic knob, and failing a
/// user's program over a typo in it would be worse than quietly running
/// correctly.
enum class allocator_mode : uint8_t {
  system,
  arena,
};

/// The mode in force for this process. Resolved once, on first call.
[[nodiscard]] auto active_allocator_mode() -> allocator_mode;

} // namespace cinder::runtime

/// C-ABI heap entry points. Both backends bottom out here: the bytecode VM
/// calls them as plain C++ (no ABI boundary), and `llvm_codegen`-compiled IR
/// (JIT and AOT alike) calls the same symbols, which is what keeps the two
/// tiers' heap layouts byte-for-byte identical per `spec/codegen-design.md`
/// Decision 3.
///
/// Every block these hand back is **zero-filled and 8-byte aligned**, in
/// both modes and including the grown tail of a `cinder_heap_realloc`. Both
/// backends rely on that: an aggregate is materialized by allocating its
/// slot block and then storing only the fields that have an initializer, so
/// a non-zero byte in a slot nobody wrote would be observable.
///
/// `old_bytes` is passed to realloc/free even though the system mode does
/// not need it. It costs the caller nothing — every Cinder caller is a
/// collection that already tracks its own capacity — and it is what lets a
/// mode that does not record block sizes out-of-band (the arena, and any
/// future pooled/sized allocator) implement the same three operations.
extern "C" {
/// Returns `bytes` of zeroed, 8-byte-aligned memory, or `nullptr` when
/// `bytes` is 0. Never returns null for a non-zero request: allocation
/// failure aborts, because there is no Cinder-level way to signal it yet.
auto cinder_heap_alloc(uint64_t bytes) -> void *;

/// Grows or shrinks `ptr` (which must have come from `cinder_heap_alloc`/
/// `cinder_heap_realloc` with size `old_bytes`, or be null) to `new_bytes`,
/// preserving the first `min(old_bytes, new_bytes)` bytes and zeroing any
/// growth. A null `ptr` behaves as `cinder_heap_alloc(new_bytes)`; a
/// `new_bytes` of 0 frees and returns `nullptr`.
auto cinder_heap_realloc(void *ptr, uint64_t old_bytes, uint64_t new_bytes)
    -> void *;

/// Releases a block obtained from the two above. A null `ptr` is a no-op.
/// In `arena` mode this does nothing at all — deliberately, see
/// `allocator_mode`.
void cinder_heap_free(void *ptr, uint64_t bytes);
}

/// The `rt_alloc`/`rt_realloc`/`rt_free` intrinsics (`src/intrinsics.h`,
/// declared as Cinder in `src/std/intrinsics.cn`), exposing the three
/// entry points above to Cinder source so a collection can own its own
/// storage instead of relying on the compiler's built-in `list[T]`.
///
/// These are separate symbols from the `cinder_heap_*` trio above because they
/// speak the intrinsic ABI (`src/intrinsics.h`'s `intrinsic_wire_kind`)
/// rather than the native `(i64) -> ptr` shape generated IR calls for its
/// own allocations — in practice identical here, since a `usize` argument's
/// wire kind is already a native `uint64_t` and a `*mut byte` is already
/// pointer-shaped. `cinder_rt_free` returns a pointer it never uses (always
/// null, standing for Cinder's `unit`) purely to share that one ABI.
extern "C" {
auto cinder_rt_alloc(uint64_t bytes) -> uint64_t *;
auto cinder_rt_realloc(uint64_t *ptr, uint64_t old_bytes, uint64_t new_bytes)
    -> uint64_t *;
auto cinder_rt_free(uint64_t *ptr, uint64_t bytes) -> uint64_t *;
}
