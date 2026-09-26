#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace cinder::bytecode {

/// Closed set of reasons the VM (`vm.h`) can panic. There is no
/// arbitrary/user-supplied message here — `op_panic`'s original design
/// (opcodes.h) carried a `message_const_index` into the constant pool, but
/// `slot_value` (value.h) is a bare numeric union with no string
/// representation, and heap types (`str` included) are explicitly deferred
/// to `spec/codegen-design.md` increment 6. Rather than inventing a
/// string-table side channel just for diagnostics ahead of that increment,
/// every panic this tier can raise is one of this fixed set, and the VM
/// renders a fixed human-readable message per reason (`panic_reason_message`
/// below) — revisit once increment 6 gives this tier real strings.
enum class panic_reason : uint8_t {
  integer_overflow,
  integer_divide_by_zero,
  explicit_panic,
  stack_overflow,
  index_out_of_bounds,
  // The three contract kinds get three reasons rather than one shared
  // "contract violated": which side of the contract broke is the first thing
  // the reader needs to know (a failed `pre` blames the caller, a failed
  // `post` blames the callee, a failed `invariant` blames whoever last
  // touched the value), and with no string channel in this tier the reason
  // *is* the whole message.
  precondition_violated,
  postcondition_violated,
  invariant_violated,
};

[[nodiscard]] auto panic_reason_message(panic_reason reason) noexcept
    -> std::string_view;

/// Exit status of a program killed by a panic, whichever tier ran it and
/// whichever panic it was — the compiler-emitted kind (`raise_panic` below,
/// `cinder_codegen_panic` in the AOT runtime) and the Cinder-level kind
/// (`std.panic.panic` -> `cinder_rt_panic`, `src/runtime/io.cpp`).
///
/// 101 rather than `abort`'s 134: it is the convention several other
/// languages already use for "the program itself signaled a panic", it
/// stays distinguishable from an ordinary `exit(1)`, and it does not dump
/// core on every out-of-range index. The two runtime files that cannot
/// depend on this header repeat the number with a comment pointing here.
inline constexpr int k_panic_exit_code = 101;

/// Whether `reason` terminates the process outright rather than unwinding to
/// the embedding host.
///
/// An out-of-range index is the one panic no program can do anything useful
/// about. Every other reason here is arguably a condition a host might want
/// to observe and report on its own terms; a bounds violation means the
/// program's own idea of a container's extent was wrong, and there is no
/// recovery that does not amount to guessing. Treating it as fatal is also
/// what makes every container agree: `list[T]`'s bounds check is ordinary
/// Cinder calling `std.panic.panic` (`src/std/list.cn`), which has always
/// aborted, while `array[T, N]`/`slice[T]`/`str` are checked by a
/// compiler-emitted opcode that used to unwind — so the same mistake
/// behaved differently depending on which container it was made against.
[[nodiscard]] constexpr auto is_fatal(panic_reason reason) noexcept -> bool {
  return reason == panic_reason::index_out_of_bounds;
}

/// Raises `reason`: terminates for a fatal one (`is_fatal`), and otherwise
/// throws `panic_error` for `vm::run` to convert into `std::expected`'s
/// error case.
///
/// Every path that can carry a *compiler-chosen* reason goes through here
/// rather than throwing directly, so "which panics are fatal" is one
/// predicate and not a decision repeated at each raise site.
[[noreturn]] auto raise_panic(panic_reason reason) -> void;

/// Thrown from deep inside nested `op_call` frames to unwind every pending
/// call frame back to `vm::run`'s boundary in one step, where it is caught
/// and converted into `std::expected`'s error case. This is exactly the
/// "truly exceptional, non-local failure" `spec/CONVENTIONS.md` carves out
/// exceptions for: a panic must abandon an arbitrary, dynamically-sized
/// stack of frames, which `std::expected` would otherwise require every
/// intermediate opcode handler and `op_call` site to manually check for and
/// re-propagate.
class panic_error : public std::runtime_error {
public:
  explicit panic_error(panic_reason reason);

  [[nodiscard]] auto reason() const noexcept -> panic_reason { return reason_; }

private:
  panic_reason reason_;
};

} // namespace cinder::bytecode
