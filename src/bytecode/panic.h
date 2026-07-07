#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace kira::bytecode {

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
};

[[nodiscard]] auto panic_reason_message(panic_reason reason) noexcept
    -> std::string_view;

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

} // namespace kira::bytecode
