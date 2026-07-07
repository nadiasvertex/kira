#include "src/runtime/arena.h"

#include <cstring>

namespace kira::runtime {

auto bump_arena::allocate(size_t bytes) -> void * {
  // Round up to 8-byte alignment — every slot_value-sized field this arena
  // backs (str/list headers, tuple/struct/sum-payload/closure-env slots)
  // needs 8-byte alignment, and rounding the request up here means callers
  // never have to think about alignment themselves.
  const auto aligned = (bytes + 7) & ~size_t{7};
  if (aligned > remaining_) {
    const auto block_size = aligned > k_block_size ? aligned : k_block_size;
    blocks_.push_back(std::make_unique<std::byte[]>(block_size));
    current_ = blocks_.back().get();
    remaining_ = block_size;
    std::memset(current_, 0, block_size);
  }
  auto *result = current_;
  current_ += aligned;
  remaining_ -= aligned;
  return result;
}

auto global_arena() -> bump_arena & {
  static bump_arena arena;
  return arena;
}

} // namespace kira::runtime

extern "C" auto kira_rt_alloc(uint64_t bytes) -> void * {
  return kira::runtime::global_arena().allocate(bytes);
}
