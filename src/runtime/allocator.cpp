#include "src/runtime/allocator.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string_view>

#include "src/runtime/arena.h"

namespace kira::runtime {
namespace {

/// Rounds a byte count up to the 8-byte granularity every Kira heap block is
/// sized and aligned at (`spec/codegen-design.md` Decision 3's uniform
/// 8-byte slot). Doing it here rather than in each caller means a `realloc`
/// that grows a block by less than a slot is a no-op in `system` mode too,
/// not just in the arena.
[[nodiscard]] auto round_to_slot(uint64_t bytes) -> size_t {
  return static_cast<size_t>((bytes + 7) & ~uint64_t{7});
}

[[noreturn]] void out_of_memory(uint64_t bytes) {
  // No Kira-level failure channel exists for allocation yet (`rt_alloc`
  // returns a bare `*mut byte`, not a `result`), so this aborts rather than
  // handing generated code a null it has no way to check. Phrased like a
  // compiler diagnostic because it is the user who sees it.
  std::println(stderr,
               "kira: out of memory\n"
               "  could not allocate {} bytes\n"
               "  note: Kira's allocator has no failure channel yet, so an "
               "allocation that cannot be satisfied terminates the process",
               bytes);
  std::abort();
}

[[nodiscard]] auto resolve_mode() -> allocator_mode {
  const char *const requested = std::getenv("KIRA_ALLOCATOR");
  if (requested != nullptr && std::string_view{requested} == "arena") {
    return allocator_mode::arena;
  }
  return allocator_mode::system;
}

} // namespace

auto active_allocator_mode() -> allocator_mode {
  static const allocator_mode mode = resolve_mode();
  return mode;
}

} // namespace kira::runtime

extern "C" auto kira_heap_alloc(uint64_t bytes) -> void * {
  const auto size = kira::runtime::round_to_slot(bytes);
  if (size == 0) {
    return nullptr;
  }
  if (kira::runtime::active_allocator_mode() ==
      kira::runtime::allocator_mode::arena) {
    return kira::runtime::global_arena().allocate(size);
  }
  // `calloc` rather than `malloc` + `memset`: both backends materialize an
  // aggregate by allocating its slot block and storing only the fields that
  // have an initializer, so the zeroing is load-bearing, and `calloc` can
  // get it from fresh pages for free. Alignment: every platform's `calloc`
  // returns memory aligned for `max_align_t` (>= 8 everywhere Kira builds),
  // which is what the 8-byte slot invariant needs.
  void *const block = std::calloc(size, 1);
  if (block == nullptr) {
    kira::runtime::out_of_memory(bytes);
  }
  return block;
}

extern "C" auto kira_heap_realloc(void *ptr, uint64_t old_bytes,
                                  uint64_t new_bytes) -> void * {
  const auto old_size = kira::runtime::round_to_slot(old_bytes);
  const auto new_size = kira::runtime::round_to_slot(new_bytes);
  if (ptr == nullptr) {
    return kira_heap_alloc(new_bytes);
  }
  if (new_size == 0) {
    kira_heap_free(ptr, old_bytes);
    return nullptr;
  }
  if (new_size == old_size) {
    return ptr;
  }

  if (kira::runtime::active_allocator_mode() ==
      kira::runtime::allocator_mode::arena) {
    // The arena cannot grow a block in place and cannot reclaim the old one,
    // so every resize is a fresh allocation plus a copy. The new block is
    // already zeroed by `bump_arena::allocate`, so only the surviving prefix
    // needs copying.
    void *const grown = kira::runtime::global_arena().allocate(new_size);
    std::memcpy(grown, ptr, std::min(old_size, new_size));
    return grown;
  }

  void *const grown = std::realloc(ptr, new_size);
  if (grown == nullptr) {
    kira::runtime::out_of_memory(new_bytes);
  }
  if (new_size > old_size) {
    // `realloc` leaves the growth uninitialized; the zero-fill guarantee
    // `allocator.h` documents covers it, because a list that grows its
    // capacity must not expose stale bytes in the slots past its length.
    std::memset(static_cast<std::byte *>(grown) + old_size, 0, // NOLINT
                new_size - old_size);
  }
  return grown;
}

extern "C" void kira_heap_free(void *ptr, uint64_t bytes) {
  (void)bytes;
  if (ptr == nullptr) {
    return;
  }
  if (kira::runtime::active_allocator_mode() ==
      kira::runtime::allocator_mode::arena) {
    return; // The arena never reclaims; see `allocator_mode`.
  }
  std::free(ptr);
}

// --------------------------------------------------------------------------
//  Uniform-ABI intrinsic entry points (see allocator.h).
// --------------------------------------------------------------------------

namespace {

/// Reads the `usize` out of a `box_usize` (`src/std/intrinsics.kira`) — a
/// 1-slot struct whose single slot holds the count.
[[nodiscard]] auto unbox_usize(const uint64_t *box) -> uint64_t {
  return box == nullptr ? 0 : *box;
}

} // namespace

extern "C" auto kira_rt_alloc(uint64_t *bytes_box) -> uint64_t * {
  return static_cast<uint64_t *>(kira_heap_alloc(unbox_usize(bytes_box)));
}

extern "C" auto kira_rt_realloc(uint64_t *ptr, uint64_t *old_bytes_box,
                                uint64_t *new_bytes_box) -> uint64_t * {
  return static_cast<uint64_t *>(kira_heap_realloc(
      ptr, unbox_usize(old_bytes_box), unbox_usize(new_bytes_box)));
}

extern "C" auto kira_rt_free(uint64_t *ptr, uint64_t *bytes_box) -> uint64_t * {
  kira_heap_free(ptr, unbox_usize(bytes_box));
  return nullptr; // Kira `unit`; see allocator.h.
}
