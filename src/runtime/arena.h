#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace cinder::runtime {

/// Bump/arena allocator: hands out zeroed, 8-byte-aligned memory from
/// growable blocks and never frees anything. It backs the `arena` allocator
/// mode (see `allocator.h`), which is not the default. Every heap-backed
/// Cinder value (`str`, `list[T]`, tuple/struct/sum-type payloads, closure
/// environments) is allocated the same way by both the bytecode VM and
/// `llvm_codegen`-compiled code, so their in-memory layouts match.
class bump_arena {
public:
  bump_arena() = default;
  bump_arena(const bump_arena &) = delete;
  auto operator=(const bump_arena &) -> bump_arena & = delete;

  /// Returns `bytes` of zeroed, 8-byte-aligned memory that lives for the
  /// remainder of the process — never reclaimed, never moved.
  [[nodiscard]] auto allocate(size_t bytes) -> void *;

private:
  static constexpr size_t k_block_size = 1 << 20; // 1 MiB.

  std::vector<std::unique_ptr<std::byte[]>> blocks_;
  std::byte *current_ = nullptr;
  size_t remaining_ = 0;
};

/// The single arena every heap allocation in a `cinder run`/`cinder build`
/// process draws from. A function-local static singleton rather than a
/// context object threaded through the VM/codegen call paths: both tiers
/// already assume a single-threaded, single-arena-per-process model (no
/// concurrency story exists yet, per `spec/llm-compiler-roadmap.md`'s "What
/// Not To Assume Yet"), and `cinder_heap_alloc` (`allocator.h`) needs a plain
/// C-ABI symbol `llvm_codegen`-compiled IR can call with no context argument.
///
/// This is no longer the only allocation strategy: it is one of the two
/// `cinder::runtime::allocator_mode`s, selected by `CINDER_ALLOCATOR=arena`.
/// The C-ABI entry points every allocation actually goes through live in
/// `allocator.h`.
[[nodiscard]] auto global_arena() -> bump_arena &;

} // namespace cinder::runtime
