#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace kira {

// ==========================================================================
//  Shared intrinsic schema.
//
//  This is the single list of recognized `intrinsic def` names. It is the
//  one place that should ever need to change when an intrinsic is added or
//  renamed — the semantic checker validates `intrinsic def` declarations
//  against it, and (per spec/stdlib.md) the bytecode VM's dispatch table and
//  the LLVM codegen's `declare` list should read from it too, so the three
//  cannot drift apart.
//
//  See spec/stdlib.md for the full signature of each intrinsic; this table
//  only tracks names, since the signature itself is written and typechecked
//  as ordinary Cinder source at the `intrinsic def` site.
// ==========================================================================
inline constexpr std::array<std::string_view, 36> known_intrinsic_names = {{
    "rt_stdin",
    "rt_stdout",
    "rt_stderr",
    "rt_open",
    "rt_close",
    "rt_read",
    "rt_write",
    "rt_flush",
    // String-formatting intrinsics (`spec/string-formatting-design.md`):
    // back `std.fmt`'s `pad_str`/`pad_integral` and the builtin-only numeric
    // format styles (`d`/`x`/`X`/`o`/`b`/`e`/`E`/`f`/`g`/`G`/`c`). Every
    // scalar argument/return crosses at its own wire kind (see
    // `intrinsic_wire_kind` below) rather than being boxed in a heap
    // struct; only `str`/aggregate values (`raw_fd`/`io_errno` above, and
    // this group's own `find_result`) are opaque heap pointers.
    "rt_str_concat",
    "rt_str_len_scalars",
    "rt_str_repeat_char",
    "rt_str_truncate_scalars",
    "rt_fmt_radix_digits",
    "rt_fmt_f64_fixed",
    "rt_fmt_f64_sci",
    "rt_fmt_f64_general",
    "rt_fmt_char_from_codepoint",
    // `std.string` UTF-8 intrinsics (spec/std-reference.md). Substring search
    // is Two-Way (linear worst case) over bytes, scalar-correct by UTF-8
    // self-synchronization. The algorithms live in `src/runtime/
    // string_ops.h` and are shared verbatim with the bytecode VM's dispatch
    // table. Case mapping/folding is *not* here: `to_uppercase`/
    // `to_lowercase`/`fold_case` are pure Cinder over generated UCD tables
    // (`std.unicode`), not a native intrinsic — see 52-std-string.md's
    // Architecture section.
    "rt_str_eq",
    "rt_str_cmp",
    "rt_str_find",
    "rt_str_rfind",
    "rt_str_reverse",
    "rt_str_trim",
    "rt_str_replace",
    // `std.platform` runtime introspection intrinsics (spec/std-reference.md).
    // Every one is niladic and returns `result[T, io_errno]`.
    "rt_uname",
    "rt_gethostname",
    "rt_processor_name",
    "rt_libc_version",
    "rt_windows_version",
    "rt_macos_version",
    // Aborts the process after writing `msg` to stderr. The one intrinsic
    // that never returns: `option`/`result`'s `unwrap` need a failure path,
    // and terminating the process is inexpressible on the primitive set.
    "rt_panic",
    // Reinterprets a float's bit pattern as an equal-width unsigned integer
    // (`spec/todo.md` item 7). `as uint64`/`as uint32` on a float is a value
    // conversion (`compile_cast` emits `FPToUI`), not a reinterpret, so
    // `hash` on `float32`/`float64` needs a real bitcast primitive -- the
    // rare case that passes the minimal-intrinsics justification test
    // because it is genuinely inexpressible over the primitive set.
    "rt_bitcast_f64_to_u64",
    "rt_bitcast_f32_to_u32",
    // Raw heap allocation (`src/runtime/allocator.h`). These are what let a
    // collection own its own storage in Cinder source rather than relying on
    // the compiler's built-in `list[T]` -- the substrate `src/std/list.cn`
    // is rebuilt over. They pass the minimal-intrinsics justification test
    // (`spec/specification/04-stdlib/40-stdlib-overview.md`) on the strongest
    // possible grounds: obtaining memory is not expressible in terms of
    // anything else in the language.
    //
    // A `usize` argument crosses as a plain native `i64` (its wire kind);
    // a `*mut byte` is already pointer-shaped and crosses as itself.
    "rt_alloc",
    "rt_realloc",
    "rt_free",
}};

/// @brief Returns whether `name` is a recognized intrinsic.
[[nodiscard]] inline auto is_known_intrinsic(std::string_view name) noexcept
    -> bool {
  return std::ranges::any_of(
      known_intrinsic_names,
      [name](std::string_view known) { return known == name; });
}

/// @brief Returns `name`'s index into `known_intrinsic_names`, the same
/// index `op_call_intrinsic` (src/bytecode/opcodes.h) encodes as its
/// `intrinsic_id` operand and the VM's native dispatch table
/// (src/bytecode/vm.cpp) is ordered by.
[[nodiscard]] inline auto intrinsic_index_of(std::string_view name) noexcept
    -> std::optional<uint8_t> {
  for (size_t i = 0; i < known_intrinsic_names.size(); ++i) {
    if (known_intrinsic_names[i] == name) {
      return static_cast<uint8_t>(i);
    }
  }
  return std::nullopt;
}

/// Argument count for each intrinsic's native C-ABI symbol (`kira_rt_*`,
/// `src/runtime/io.h`), indexed the same as `known_intrinsic_names`. Read
/// from here rather than duplicated so the two backends' declared arities
/// can't drift out of sync with each other or with `io.h`'s actual
/// signatures.
inline constexpr std::array<uint8_t, 36> known_intrinsic_arities = {{
    0, // rt_stdin
    0, // rt_stdout
    0, // rt_stderr
    2, // rt_open
    1, // rt_close
    2, // rt_read
    2, // rt_write
    1, // rt_flush
    2, // rt_str_concat
    1, // rt_str_len_scalars
    2, // rt_str_repeat_char
    2, // rt_str_truncate_scalars
    3, // rt_fmt_radix_digits
    2, // rt_fmt_f64_fixed
    3, // rt_fmt_f64_sci
    2, // rt_fmt_f64_general
    1, // rt_fmt_char_from_codepoint
    2, // rt_str_eq         (a, b)
    2, // rt_str_cmp        (a, b)
    3, // rt_str_find       (haystack, needle, from)
    2, // rt_str_rfind      (haystack, needle)
    1, // rt_str_reverse    (s)
    2, // rt_str_trim       (s, mode)
    3, // rt_str_replace    (s, from, to)
    0, // rt_uname
    0, // rt_gethostname
    0, // rt_processor_name
    0, // rt_libc_version
    0, // rt_windows_version
    0, // rt_macos_version
    1, // rt_panic          (msg)
    1, // rt_bitcast_f64_to_u64 (v)
    1, // rt_bitcast_f32_to_u32 (v)
    1, // rt_alloc          (bytes)
    3, // rt_realloc        (ptr, old_bytes, new_bytes)
    2, // rt_free           (ptr, bytes)
}};

// ==========================================================================
//  Wire-level ABI kind of each intrinsic's parameters and return value.
//
//  Every intrinsic argument/return used to be an opaque heap pointer,
//  including scalars, which were boxed in a single-field struct (`box_u64`
//  etc.) purely so they'd fit that uniform "everything is `ptr`" shape. That
//  cost a heap allocation per scalar crossing and an unbox on the other
//  side for no reason a primitive value needs paying: `kind::i32`/`i64`/
//  `f32`/`f64` below cross as themselves — a plain native scalar — the same
//  way an ordinary (non-intrinsic) Cinder function call already does.
//
//  `bool`/`uint8` widen to `i32`: not for range, but because a sub-32-bit
//  integer argument's calling-convention lowering depends on a `zeroext`/
//  `signext` attribute that this table's caller (`llvm_codegen`, which
//  builds each `kira_rt_*` declaration and call site by hand) does not
//  attempt to reproduce byte-for-byte against what Clang attaches to the
//  real definition in `src/runtime/`. `i32`/`i64`/`f32`/`f64` need no such
//  attribute on any target Cinder builds for, so widening the two sub-32-bit
//  cases sidesteps the question entirely rather than risking getting it
//  subtly wrong.
// ==========================================================================
enum class intrinsic_wire_kind : uint8_t {
  ptr, ///< An opaque heap pointer — `src/runtime/layout.h`'s convention.
  i32, ///< A native 32-bit integer; also stands for a widened `bool`/`uint8`.
  i64, ///< A native 64-bit integer; also stands for `usize`.
  f32, ///< A native IEEE-754 32-bit float, bit-preserving (no promotion).
  f64, ///< A native IEEE-754 64-bit float.
};

/// Each intrinsic's return kind, indexed the same as `known_intrinsic_names`.
inline constexpr std::array<intrinsic_wire_kind, 36>
    known_intrinsic_return_kinds = {{
        intrinsic_wire_kind::ptr, // rt_stdin
        intrinsic_wire_kind::ptr, // rt_stdout
        intrinsic_wire_kind::ptr, // rt_stderr
        intrinsic_wire_kind::ptr, // rt_open
        intrinsic_wire_kind::ptr, // rt_close
        intrinsic_wire_kind::ptr, // rt_read
        intrinsic_wire_kind::ptr, // rt_write
        intrinsic_wire_kind::ptr, // rt_flush
        intrinsic_wire_kind::ptr, // rt_str_concat
        intrinsic_wire_kind::i64, // rt_str_len_scalars -> usize
        intrinsic_wire_kind::ptr, // rt_str_repeat_char
        intrinsic_wire_kind::ptr, // rt_str_truncate_scalars
        intrinsic_wire_kind::ptr, // rt_fmt_radix_digits
        intrinsic_wire_kind::ptr, // rt_fmt_f64_fixed
        intrinsic_wire_kind::ptr, // rt_fmt_f64_sci
        intrinsic_wire_kind::ptr, // rt_fmt_f64_general
        intrinsic_wire_kind::ptr, // rt_fmt_char_from_codepoint
        intrinsic_wire_kind::i32, // rt_str_eq -> bool
        intrinsic_wire_kind::i32, // rt_str_cmp -> int32
        intrinsic_wire_kind::ptr, // rt_str_find
        intrinsic_wire_kind::ptr, // rt_str_rfind
        intrinsic_wire_kind::ptr, // rt_str_reverse
        intrinsic_wire_kind::ptr, // rt_str_trim
        intrinsic_wire_kind::ptr, // rt_str_replace
        intrinsic_wire_kind::ptr, // rt_uname
        intrinsic_wire_kind::ptr, // rt_gethostname
        intrinsic_wire_kind::ptr, // rt_processor_name
        intrinsic_wire_kind::ptr, // rt_libc_version
        intrinsic_wire_kind::ptr, // rt_windows_version
        intrinsic_wire_kind::ptr, // rt_macos_version
        intrinsic_wire_kind::ptr, // rt_panic (never returns)
        intrinsic_wire_kind::i64, // rt_bitcast_f64_to_u64 -> uint64
        intrinsic_wire_kind::i32, // rt_bitcast_f32_to_u32 -> uint32
        intrinsic_wire_kind::ptr, // rt_alloc
        intrinsic_wire_kind::ptr, // rt_realloc
        intrinsic_wire_kind::ptr, // rt_free (unused `unit` placeholder)
    }};

/// Each intrinsic's parameter kinds, indexed the same as
/// `known_intrinsic_names`; only the first `known_intrinsic_arities[i]`
/// entries of each row are meaningful, the rest are unused filler.
inline constexpr std::array<std::array<intrinsic_wire_kind, 3>, 36>
    known_intrinsic_param_kinds = {{
        {},                                                   // rt_stdin
        {},                                                   // rt_stdout
        {},                                                   // rt_stderr
        {intrinsic_wire_kind::ptr, intrinsic_wire_kind::ptr}, // rt_open
        {intrinsic_wire_kind::ptr},                           // rt_close
        {intrinsic_wire_kind::ptr, intrinsic_wire_kind::ptr}, // rt_read
        {intrinsic_wire_kind::ptr, intrinsic_wire_kind::ptr}, // rt_write
        {intrinsic_wire_kind::ptr},                           // rt_flush
        {intrinsic_wire_kind::ptr, intrinsic_wire_kind::ptr}, // rt_str_concat
        {intrinsic_wire_kind::ptr}, // rt_str_len_scalars
        {intrinsic_wire_kind::i32,
         intrinsic_wire_kind::i64}, // rt_str_repeat_char (codepoint, n)
        {intrinsic_wire_kind::ptr,
         intrinsic_wire_kind::i64}, // rt_str_truncate_scalars (s, n)
        {intrinsic_wire_kind::i64, intrinsic_wire_kind::i32,
         intrinsic_wire_kind::i32}, // rt_fmt_radix_digits (value, radix,
                                    // uppercase)
        {intrinsic_wire_kind::f64,
         intrinsic_wire_kind::i64}, // rt_fmt_f64_fixed (value, precision)
        {intrinsic_wire_kind::f64, intrinsic_wire_kind::i64,
         intrinsic_wire_kind::i32}, // rt_fmt_f64_sci (value, precision,
                                    // uppercase)
        {intrinsic_wire_kind::f64,
         intrinsic_wire_kind::i64}, // rt_fmt_f64_general (value, precision)
        {intrinsic_wire_kind::i32}, // rt_fmt_char_from_codepoint (cp)
        {intrinsic_wire_kind::ptr, intrinsic_wire_kind::ptr}, // rt_str_eq
        {intrinsic_wire_kind::ptr, intrinsic_wire_kind::ptr}, // rt_str_cmp
        {intrinsic_wire_kind::ptr, intrinsic_wire_kind::ptr,
         intrinsic_wire_kind::i64}, // rt_str_find (haystack, needle, from)
        {intrinsic_wire_kind::ptr, intrinsic_wire_kind::ptr}, // rt_str_rfind
        {intrinsic_wire_kind::ptr},                           // rt_str_reverse
        {intrinsic_wire_kind::ptr,
         intrinsic_wire_kind::i32}, // rt_str_trim (s, mode)
        {intrinsic_wire_kind::ptr, intrinsic_wire_kind::ptr,
         intrinsic_wire_kind::ptr}, // rt_str_replace
        {},                         // rt_uname
        {},                         // rt_gethostname
        {},                         // rt_processor_name
        {},                         // rt_libc_version
        {},                         // rt_windows_version
        {},                         // rt_macos_version
        {intrinsic_wire_kind::ptr}, // rt_panic (msg)
        {intrinsic_wire_kind::f64}, // rt_bitcast_f64_to_u64 (v)
        {intrinsic_wire_kind::f32}, // rt_bitcast_f32_to_u32 (v)
        {intrinsic_wire_kind::i64}, // rt_alloc (bytes)
        {intrinsic_wire_kind::ptr, intrinsic_wire_kind::i64,
         intrinsic_wire_kind::i64}, // rt_realloc (ptr, old_bytes, new_bytes)
        {intrinsic_wire_kind::ptr,
         intrinsic_wire_kind::i64}, // rt_free (ptr, bytes)
    }};

} // namespace kira
