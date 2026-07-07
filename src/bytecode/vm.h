#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include "src/bytecode/chunk.h"
#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"

namespace kira::bytecode {

/// Result of a completed (non-panicking) call: `op_return_unit`-returning
/// functions produce `has_value == false`, matching how `op_call`'s doc
/// comment (opcodes.h) already describes `dst` as unused/untouched for
/// those.
struct vm_result {
  bool has_value = false;
  slot_value value{};
};

/// Tier-0 bytecode interpreter (`spec/codegen-design.md` Decision 1,
/// increment 2). Dispatches one `bytecode_module`'s flat instruction stream
/// via an ordinary `switch` over `opcode` — no computed-goto/threaded
/// dispatch trick, per `CLAUDE.md`'s stated priority of clarity over
/// dispatch-speed cleverness for this tier (real hot code is Decision 4's
/// job, not this one's).
///
/// Call frames are an explicit `std::vector<frame>` growing one entry per
/// nested `op_call`, not C++ call-stack recursion — this keeps Kira's own
/// call-depth limit (and the `stack_overflow` panic it produces) independent
/// of how deep the host's own C++ stack happens to tolerate, and keeps the
/// dispatch loop as one flat `while` over whichever frame is current.
class vm {
public:
  explicit vm(const bytecode_module &module) : module_(module) {}

  /// Runs `module.functions[function_index]` to completion, with `args`
  /// copied into its entry frame's registers `[0, args.size())` — mirroring
  /// how `op_call` populates a callee frame from the caller's registers, so
  /// the outermost call is not a special case. Returns the panic reason on
  /// a checked-arithmetic violation, divide-by-zero, `op_panic`, or a
  /// call-depth limit violation.
  [[nodiscard]] auto run(uint16_t function_index,
                         std::span<const slot_value> args) const
      -> std::expected<vm_result, panic_reason>;

private:
  const bytecode_module &module_;
};

} // namespace kira::bytecode
