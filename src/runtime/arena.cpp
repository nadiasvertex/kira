#include "src/runtime/arena.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace cinder::runtime {

auto bump_arena::allocate(size_t bytes) -> void * { return allocate(bytes, 8); }

auto bump_arena::allocate(size_t bytes, size_t align) -> void * {
  // Round the size up to 8 bytes — every slot_value-sized field this arena
  // backs (str/list headers, tuple/struct/sum-payload/closure-env slots)
  // needs 8-byte alignment, and rounding the request up here means callers
  // never have to think about alignment themselves. A stricter `align` also
  // moves the start of the block, below.
  align = std::max<size_t>(align, 8);
  const auto aligned = (bytes + 7) & ~size_t{7};
  const auto lock = std::scoped_lock(mutex_);
  const auto padding = [&]() -> size_t {
    const auto address = reinterpret_cast<uintptr_t>(current_); // NOLINT
    return (align - (address & (align - 1))) & (align - 1);
  };
  if (current_ == nullptr || padding() + aligned > remaining_) {
    // A fresh block starts at whatever `new[]` returns, which is not
    // necessarily `align`-aligned, so leave room to pad it.
    const auto needed = aligned + align;
    const auto block_size = std::max(needed, k_block_size);
    blocks_.push_back(std::make_unique<std::byte[]>(block_size));
    current_ = blocks_.back().get();
    remaining_ = block_size;
    std::memset(current_, 0, block_size);
  }
  const auto skip = padding();
  auto *result = current_ + skip;
  current_ += skip + aligned;
  remaining_ -= skip + aligned;
  return result;
}

auto global_arena() -> bump_arena & {
  static bump_arena arena;
  return arena;
}

} // namespace cinder::runtime
