#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// Pure, backend-agnostic UTF-8 string algorithms shared by both compiler
/// tiers: the bytecode VM (`src/bytecode/vm.cpp`) calls these directly on
/// `std::string_view`s decoded from its slot representation, and the LLVM
/// tier's C-ABI intrinsics (`src/runtime/string.cpp`) wrap them for
/// generated IR. Keeping the algorithms here (rather than duplicated in each
/// tier) is why the two backends can't drift apart. Backs `std.string`
/// (`spec/std-reference.md`).
///
/// Every function operates on UTF-8 bytes. Substring search is scalar-correct
/// without decoding because UTF-8 is self-synchronizing: a valid UTF-8 needle
/// can only match a valid UTF-8 haystack on scalar boundaries. Reported
/// offsets are therefore byte offsets that always land on a scalar boundary.
namespace cinder::runtime {

/// Byte-for-byte equality (length check + `memcmp`).
[[nodiscard]] auto str_equal(std::string_view a, std::string_view b) -> bool;

/// Lexicographic byte comparison: negative if `a` sorts before `b`, zero if
/// equal, positive if `a` sorts after `b` — `std.string`'s `cmp` (backing
/// `ord for str`) reads the sign only, same as `strcmp`/`memcmp`.
[[nodiscard]] auto str_compare(std::string_view a, std::string_view b) -> int;

/// First byte offset at or after `from` where `needle` occurs in `haystack`,
/// or `nullopt` if it does not. Two-Way string matching (Crochemore–Perrin):
/// O(n + m) worst case, O(1) extra space. An empty needle matches at
/// `min(from, haystack.size())`.
[[nodiscard]] auto str_find(std::string_view haystack, std::string_view needle,
                            size_t from) -> std::optional<size_t>;

/// Last byte offset where `needle` occurs in `haystack`, or `nullopt`. Linear
/// worst case (Two-Way over the byte-reversed inputs). An empty needle matches
/// at `haystack.size()`.
[[nodiscard]] auto str_rfind(std::string_view haystack, std::string_view needle)
    -> std::optional<size_t>;

/// Scalars emitted in reverse order (valid UTF-8 out). Reverses by scalar, not
/// by grapheme cluster; combining marks are not kept adjacent to their base.
[[nodiscard]] auto str_reverse(std::string_view s) -> std::string;

enum class trim_mode : uint8_t { both = 0, start = 1, end = 2 };

/// Removes leading and/or trailing scalars with the Unicode `White_Space`
/// property. Returns a sub-view into `s` (no allocation).
[[nodiscard]] auto str_trim(std::string_view s, trim_mode mode)
    -> std::string_view;

/// All non-overlapping occurrences of `from` replaced with `to`. An empty
/// `from` leaves `s` unchanged (no zero-width matches).
[[nodiscard]] auto str_replace(std::string_view s, std::string_view from,
                               std::string_view to) -> std::string;

/// The decoded Unicode scalar at byte offset `pos` in `s`, or U+FFFD if
/// `pos` doesn't start a valid UTF-8 sequence. Backs `for c in s` scalar
/// iteration over a `str` (`hir::hir_str_decode_scalar`) — called directly
/// by the bytecode VM; `cinder_rt_str_scalar_at` below is the raw-pointer
/// `extern "C"` wrapper generated IR calls instead.
[[nodiscard]] auto str_scalar_at(std::string_view s, size_t pos) -> uint32_t;

/// Bytes consumed decoding the scalar at byte offset `pos` in `s` — 1 if
/// `pos` doesn't start a valid UTF-8 sequence, so a `for`-loop cursor built
/// from this always makes forward progress. Companion to `str_scalar_at`
/// for the same lowering (`hir::hir_str_scalar_width`).
[[nodiscard]] auto str_scalar_width(std::string_view s, size_t pos) -> uint64_t;

extern "C" auto cinder_rt_str_scalar_at(const char *data, uint64_t len,
                                      uint64_t offset) -> uint32_t;
extern "C" auto cinder_rt_str_scalar_width(const char *data, uint64_t len,
                                         uint64_t offset) -> uint64_t;

} // namespace cinder::runtime
