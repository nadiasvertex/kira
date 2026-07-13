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
//  as ordinary Kira source at the `intrinsic def` site.
// ==========================================================================
inline constexpr std::array<std::string_view, 23> known_intrinsic_names = {{
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
    // scalar argument/return is boxed in a single-field struct (`box_u64`
    // etc., `src/std/fmt.kira`) so it stays heap-representable, matching
    // this table's existing "every intrinsic value is `ptr`" convention
    // (`raw_fd`/`io_errno` already do the same for the eight I/O intrinsics
    // above) rather than generalizing the ABI.
    "rt_str_concat",
    "rt_str_len_scalars",
    "rt_str_repeat_char",
    "rt_str_truncate_scalars",
    "rt_fmt_radix_digits",
    "rt_fmt_f64_fixed",
    "rt_fmt_f64_sci",
    "rt_fmt_f64_general",
    "rt_fmt_char_from_codepoint",
    // `std.platform` runtime introspection intrinsics (spec/std-platform.md).
    // Every one is niladic and returns `result[T, io_errno]`.
    "rt_uname",
    "rt_gethostname",
    "rt_processor_name",
    "rt_libc_version",
    "rt_windows_version",
    "rt_macos_version",
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
/// `src/runtime/io.h`), indexed the same as `known_intrinsic_names`. Every
/// intrinsic argument and return value is an opaque heap pointer (see
/// `io.h`'s doc comment), so this arity is all `llvm_codegen` needs to
/// declare each `kira_rt_*` function's LLVM signature — read from here
/// rather than duplicated so the two backends' declared arities can't drift
/// out of sync with each other or with `io.h`'s actual signatures.
inline constexpr std::array<uint8_t, 23> known_intrinsic_arities = {{
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
    0, // rt_uname
    0, // rt_gethostname
    0, // rt_processor_name
    0, // rt_libc_version
    0, // rt_windows_version
    0, // rt_macos_version
}};

} // namespace kira
