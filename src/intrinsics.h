#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace cinder {

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
//  only tracks names and the C-ABI shape of each native symbol, since the
//  signature itself is written and typechecked as ordinary Cinder source at
//  the `intrinsic def` site.
// ==========================================================================

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
//  builds each `cinder_rt_*` declaration and call site by hand) does not
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

/// One recognized intrinsic: its name and the C-ABI shape of its native
/// `cinder_rt_*` symbol. Only the first `arity` entries of `params` are
/// meaningful.
struct intrinsic_signature {
  std::string_view name;
  intrinsic_wire_kind result;
  std::array<intrinsic_wire_kind, 3> params;
  uint8_t arity;
};

[[nodiscard]] consteval auto
intrinsic(std::string_view name, intrinsic_wire_kind result,
          std::initializer_list<intrinsic_wire_kind> params)
    -> intrinsic_signature {
  auto signature =
      intrinsic_signature{.name = name,
                          .result = result,
                          .params = {},
                          .arity = static_cast<uint8_t>(params.size())};
  std::ranges::copy(params, signature.params.begin());
  return signature;
}

/// Short names for the rows of `known_intrinsics`.
namespace wire {
inline constexpr auto ptr = intrinsic_wire_kind::ptr;
inline constexpr auto i32 = intrinsic_wire_kind::i32;
inline constexpr auto i64 = intrinsic_wire_kind::i64;
inline constexpr auto f32 = intrinsic_wire_kind::f32;
inline constexpr auto f64 = intrinsic_wire_kind::f64;
} // namespace wire

/// Every runtime-called intrinsic, in the order `op_call_intrinsic` encodes
/// as its `intrinsic_id` and the VM's native table (`src/bytecode/vm.cpp`)
/// follows. Adding one means adding a row here and a function there.
inline constexpr std::array<intrinsic_signature, 51> known_intrinsics = {{
    intrinsic("rt_stdin", wire::ptr, {}),
    intrinsic("rt_stdout", wire::ptr, {}),
    intrinsic("rt_stderr", wire::ptr, {}),
    intrinsic("rt_open", wire::ptr, {wire::ptr, wire::ptr}),
    intrinsic("rt_close", wire::ptr, {wire::ptr}),
    intrinsic("rt_read", wire::ptr, {wire::ptr, wire::ptr}),
    intrinsic("rt_write", wire::ptr, {wire::ptr, wire::ptr}),
    intrinsic("rt_flush", wire::ptr, {wire::ptr}),
    // String-formatting intrinsics (`spec/string-formatting-design.md`):
    // back `std.fmt`'s `pad_str`/`pad_integral` and the builtin-only numeric
    // format styles (`d`/`x`/`X`/`o`/`b`/`e`/`E`/`f`/`g`/`G`/`c`). Every
    // scalar argument/return crosses at its own wire kind (see
    // `intrinsic_wire_kind` below) rather than being boxed in a heap
    // struct; only `str`/aggregate values (`raw_fd`/`io_errno` above, and
    // this group's own `find_result`) are opaque heap pointers.
    intrinsic("rt_str_concat", wire::ptr, {wire::ptr, wire::ptr}),
    intrinsic("rt_str_len_scalars", wire::i64, {wire::ptr}),
    intrinsic("rt_str_repeat_char", wire::ptr, {wire::i32, wire::i64}),
    intrinsic("rt_str_truncate_scalars", wire::ptr, {wire::ptr, wire::i64}),
    intrinsic("rt_fmt_radix_digits", wire::ptr,
              {wire::i64, wire::i32, wire::i32}),
    intrinsic("rt_fmt_f64_fixed", wire::ptr, {wire::f64, wire::i64}),
    intrinsic("rt_fmt_f64_sci", wire::ptr, {wire::f64, wire::i64, wire::i32}),
    intrinsic("rt_fmt_f64_general", wire::ptr, {wire::f64, wire::i64}),
    intrinsic("rt_fmt_char_from_codepoint", wire::ptr, {wire::i32}),
    // `std.string` UTF-8 intrinsics (spec/std-reference.md). Substring search
    // is Two-Way (linear worst case) over bytes, scalar-correct by UTF-8
    // self-synchronization. The algorithms live in `src/runtime/
    // string_ops.h` and are shared verbatim with the bytecode VM's dispatch
    // table. Case mapping/folding is *not* here: `to_uppercase`/
    // `to_lowercase`/`fold_case` are pure Cinder over generated UCD tables
    // (`std.unicode`), not a native intrinsic — see 52-std-string.md's
    // Architecture section.
    intrinsic("rt_str_eq", wire::i32,
              {wire::ptr, wire::ptr}), // rt_str_eq         (a, b),
    intrinsic("rt_str_cmp", wire::i32,
              {wire::ptr, wire::ptr}), // rt_str_cmp        (a, b),
    intrinsic("rt_str_find", wire::ptr,
              {wire::ptr, wire::ptr,
               wire::i64}), // rt_str_find       (haystack, needle, from),
    intrinsic("rt_str_rfind", wire::ptr,
              {wire::ptr, wire::ptr}), // rt_str_rfind      (haystack, needle),
    intrinsic("rt_str_reverse", wire::ptr, {wire::ptr}), // rt_str_reverse (s),
    intrinsic("rt_str_trim", wire::ptr,
              {wire::ptr, wire::i32}), // rt_str_trim       (s, mode),
    intrinsic(
        "rt_str_replace", wire::ptr,
        {wire::ptr, wire::ptr, wire::ptr}), // rt_str_replace    (s, from, to),
    // `std.platform` runtime introspection intrinsics (spec/std-reference.md).
    // Every one is niladic and returns `result[T, io_errno]`.
    intrinsic("rt_uname", wire::ptr, {}),
    intrinsic("rt_gethostname", wire::ptr, {}),
    intrinsic("rt_processor_name", wire::ptr, {}),
    intrinsic("rt_libc_version", wire::ptr, {}),
    intrinsic("rt_windows_version", wire::ptr, {}),
    intrinsic("rt_macos_version", wire::ptr, {}),
    // Aborts the process after writing `msg` to stderr. The one intrinsic
    // that never returns: `option`/`result`'s `unwrap` need a failure path,
    // and terminating the process is inexpressible on the primitive set.
    intrinsic("rt_panic", wire::ptr, {wire::ptr}), // rt_panic          (msg),
    // Reinterprets a float's bit pattern as an equal-width unsigned integer
    // (`spec/todo.md` item 7). `as uint64`/`as uint32` on a float is a value
    // conversion (`compile_cast` emits `FPToUI`), not a reinterpret, so
    // `hash` on `float32`/`float64` needs a real bitcast primitive -- the
    // rare case that passes the minimal-intrinsics justification test
    // because it is genuinely inexpressible over the primitive set.
    intrinsic("rt_bitcast_f64_to_u64", wire::i64,
              {wire::f64}), // rt_bitcast_f64_to_u64 (v),
    intrinsic("rt_bitcast_f32_to_u32", wire::i32,
              {wire::f32}), // rt_bitcast_f32_to_u32 (v),
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
    intrinsic("rt_alloc", wire::ptr, {wire::i64}), // rt_alloc          (bytes),
    intrinsic("rt_realloc", wire::ptr,
              {wire::ptr, wire::i64,
               wire::i64}), // rt_realloc        (ptr, old_bytes, new_bytes),
    intrinsic("rt_free", wire::ptr,
              {wire::ptr, wire::i64}), // rt_free           (ptr, bytes),
    // The task scheduler (`src/runtime/scheduler.h`). A task crosses as the
    // pointer to its coroutine object. Compiled code reaches these from the
    // await lowering; none is called by name from Cinder source yet.
    intrinsic("rt_task_block_on", wire::ptr,
              {wire::ptr, wire::i64}), // rt_task_block_on  (task, pool),
    intrinsic("rt_task_await_poll", wire::i32,
              {wire::ptr, wire::ptr}), // rt_task_await_poll (self, awaited),
    intrinsic("rt_task_spawn", wire::ptr,
              {wire::ptr, wire::i64}), // rt_task_spawn     (task, pool),
    intrinsic("rt_task_cancel", wire::i32,
              {wire::ptr}), // rt_task_cancel    (task),
    intrinsic("rt_task_is_finished", wire::i32,
              {wire::ptr}), // rt_task_is_finished (task),
    intrinsic("rt_task_await_any_poll", wire::i32,
              {wire::ptr, wire::ptr,
               wire::i64}), // rt_task_await_any_poll (self, tasks, count),
    intrinsic("rt_task_block_on_any", wire::ptr,
              {wire::ptr, wire::i64}), // rt_task_block_on_any (tasks, count),
    intrinsic("rt_task_await_all_poll", wire::i32,
              {wire::ptr, wire::ptr,
               wire::i64}), // rt_task_await_all_poll (self, tasks, count),
    intrinsic("rt_task_block_on_all", wire::ptr,
              {wire::ptr, wire::i64}), // rt_task_block_on_all (tasks, count),
    intrinsic("rt_task_drop", wire::i32,
              {wire::ptr}), // rt_task_drop      (task),
    intrinsic("rt_crew_new", wire::ptr, {}), // rt_crew_new (),
    intrinsic("rt_crew_spawn", wire::ptr,
              {wire::ptr, wire::ptr}), // rt_crew_spawn (crew, task),
    intrinsic("rt_crew_join_poll", wire::i32,
              {wire::ptr, wire::ptr}), // rt_crew_join_poll (self, crew),
    intrinsic("rt_crew_join", wire::ptr, {wire::ptr}), // rt_crew_join (crew),
    intrinsic("rt_crew_free", wire::ptr, {wire::ptr}), // rt_crew_free (crew),
}};

static_assert(std::ranges::none_of(known_intrinsics,
                                   [](const intrinsic_signature &row) {
                                     return row.name.empty();
                                   }),
              "an intrinsic row is missing: the array is larger than its "
              "initializer list");

/// The columns of `known_intrinsics`, for the code that wants one of them
/// indexed by intrinsic id.
inline constexpr auto known_intrinsic_names = [] {
  auto names = std::array<std::string_view, known_intrinsics.size()>{};
  std::ranges::transform(known_intrinsics, names.begin(),
                         &intrinsic_signature::name);
  return names;
}();

inline constexpr auto known_intrinsic_arities = [] {
  auto arities = std::array<uint8_t, known_intrinsics.size()>{};
  std::ranges::transform(known_intrinsics, arities.begin(),
                         &intrinsic_signature::arity);
  return arities;
}();

inline constexpr auto known_intrinsic_return_kinds = [] {
  auto kinds = std::array<intrinsic_wire_kind, known_intrinsics.size()>{};
  std::ranges::transform(known_intrinsics, kinds.begin(),
                         &intrinsic_signature::result);
  return kinds;
}();

inline constexpr auto known_intrinsic_param_kinds = [] {
  auto kinds =
      std::array<std::array<intrinsic_wire_kind, 3>, known_intrinsics.size()>{};
  std::ranges::transform(known_intrinsics, kinds.begin(),
                         &intrinsic_signature::params);
  return kinds;
}();

// ==========================================================================
//  Inline intrinsics.
//
//  An inline intrinsic is an `intrinsic def` that neither backend reaches
//  through a runtime call: the VM compiles it to a dedicated opcode and the
//  LLVM tier to a single instruction. It therefore has no `cinder_rt_*`
//  symbol, no VM native-table slot, and no entry in the tables above —
//  `intrinsic_index_of` deliberately does not find it. Both backends check
//  `inline_intrinsic_of` before `intrinsic_index_of` at a call site.
//
//  The atomics are here because a runtime call would be the wrong shape for
//  them: the operation is one instruction, and the memory ordering belongs
//  to that instruction.
// ==========================================================================
enum class inline_intrinsic : uint8_t {
  atomic_fetch_add, ///< `rt_atomic_fetch_add(p: *mut usize, n: usize) -> usize`
  atomic_fetch_sub, ///< `rt_atomic_fetch_sub(p: *mut usize, n: usize) -> usize`
};

/// @brief Returns which inline intrinsic `name` is, if any.
[[nodiscard]] inline auto inline_intrinsic_of(std::string_view name) noexcept
    -> std::optional<inline_intrinsic> {
  if (name == "rt_atomic_fetch_add") {
    return inline_intrinsic::atomic_fetch_add;
  }
  if (name == "rt_atomic_fetch_sub") {
    return inline_intrinsic::atomic_fetch_sub;
  }
  return std::nullopt;
}

/// @brief Returns whether `name` is a recognized intrinsic, runtime-called
/// or inline.
[[nodiscard]] inline auto is_known_intrinsic(std::string_view name) noexcept
    -> bool {
  return inline_intrinsic_of(name).has_value() ||
         std::ranges::any_of(
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

} // namespace cinder
