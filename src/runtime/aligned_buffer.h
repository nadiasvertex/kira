#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>

namespace cinder::runtime {

/// The alignment of frame storage. It covers every scalar (`int128` is the
/// widest at 16); a layout that needs more must ask `aligned_buffer` for more.
inline constexpr size_t k_frame_storage_alignment = 16;

/// Releases a block that `aligned_buffer` allocated with a stricter-than-default
/// alignment; it must be told the alignment, since the matching `operator
/// delete` takes it.
struct aligned_bytes_deleter {
  size_t align = k_frame_storage_alignment;
  auto operator()(std::byte *raw) const -> void {
    ::operator delete(raw, std::align_val_t{align});
  }
};

/// A zero-filled, owned block of bytes that starts on a multiple of its
/// alignment. It backs frame storage — locals and `uninit` buffers whose
/// addresses are taken — which must be aligned for whatever layout is placed in
/// it. `std::vector<std::byte>` promises only the default `operator new`
/// alignment, which is not a contract about 128-bit values.
///
/// The address of the first byte never changes for the life of the object, and
/// moving the object keeps it, so pointers into a frame survive the frame
/// vector reallocating. These blocks are frame storage, not Cinder heap
/// blocks: they do not count toward `heap_allocation_count`.
class aligned_buffer {
public:
  aligned_buffer() = default;

  /// `bytes` zeroed bytes starting on a multiple of `align` (a power of two).
  /// A zero-byte buffer owns nothing and `data()` is null.
  explicit aligned_buffer(size_t bytes,
                          size_t align = k_frame_storage_alignment)
      : size_(bytes) {
    if (bytes == 0) {
      return;
    }
    auto *raw = static_cast<std::byte *>(
        ::operator new(bytes, std::align_val_t{align}));
    std::memset(raw, 0, bytes);
    data_ = std::unique_ptr<std::byte, aligned_bytes_deleter>(
        raw, aligned_bytes_deleter{align});
  }

  [[nodiscard]] auto data() -> std::byte * { return data_.get(); }
  [[nodiscard]] auto data() const -> const std::byte * { return data_.get(); }
  [[nodiscard]] auto size() const -> size_t { return size_; }

private:
  std::unique_ptr<std::byte, aligned_bytes_deleter> data_;
  size_t size_ = 0;
};

} // namespace cinder::runtime
