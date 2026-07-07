#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace kira::runtime {

/// Bump/arena allocator: hands out zeroed, 8-byte-aligned memory from
/// growable blocks and never frees anything. This is the explicit
/// placeholder `spec/codegen-design.md` Decision 3 calls for — real memory
/// management (refcounting, ownership-based drop, or a tracing GC) is
/// blocked on Kira's still-undesigned ownership/borrow model, so this is
/// deliberately the simplest thing that could work, not a performance
/// strategy. Every heap-backed Kira value (`str`, `list[T]`, tuple/struct/
/// sum-type payloads, closure environments) is allocated from here by both
/// the bytecode VM and `llvm_codegen`-compiled code, which is exactly what
/// keeps their in-memory layouts byte-for-byte identical per Decision 3.
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

/// The single arena every heap allocation in a `kira run`/`kira build`
/// process draws from. A function-local static singleton rather than a
/// context object threaded through the VM/codegen call paths: both tiers
/// already assume a single-threaded, single-arena-per-process model (no
/// concurrency story exists yet, per `spec/llm-compiler-roadmap.md`'s "What
/// Not To Assume Yet"), and `kira_rt_alloc` below needs a plain C-ABI
/// symbol `llvm_codegen`-compiled IR can call with no context argument.
[[nodiscard]] auto global_arena() -> bump_arena &;

} // namespace kira::runtime

/// C-ABI entry point `llvm_codegen`-compiled IR (JIT and AOT alike) calls to
/// allocate heap memory from `global_arena()`. The bytecode VM calls
/// `kira::runtime::global_arena().allocate(...)` directly (a plain C++ call,
/// no ABI boundary to cross) — this `extern "C"` wrapper exists solely for
/// the LLVM tier, the same reason `kira_codegen_panic`
/// (`src/llvm_codegen/runtime.h`) is `extern "C"`.
extern "C" auto kira_rt_alloc(uint64_t bytes) -> void *;
