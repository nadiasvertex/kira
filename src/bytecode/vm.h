#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "src/bytecode/chunk.h"
#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"

namespace cinder::bytecode {

/// Result of a completed (non-panicking) call: `op_return_unit`-returning
/// functions produce `has_value == false`, matching how `op_call`'s doc
/// comment (opcodes.h) already describes `dst` as unused/untouched for
/// those.
struct vm_result {
  bool has_value = false;
  slot_value value{};
};

/// What a hosted run (`vm::run_hosted`) may do.
/// Performs one `op_host_call` for a hosted run: `site` says what, and
/// `args` are its operands, typed as `site.arg_types` say. The error
/// explains why the operation could not be done.
using host_fn = std::function<std::expected<slot_value, std::string>(
    const host_site &site, std::span<const slot_value> args)>;

struct run_limits {
  /// Instructions the run may execute before it fails with
  /// `step_budget_exhausted`; `nullopt` for no limit.
  std::optional<uint64_t> step_budget;
  /// Whether the program may call intrinsics that reach outside the VM — the
  /// file, console and platform-query ones. Refused ones fail with
  /// `effect_refused`.
  bool allow_effects = true;
  /// What performs `op_host_call`. Without one, the instruction fails: the
  /// operation exists only while compiling.
  const host_fn *host = nullptr;
};

/// One active call when a hosted run failed.
struct vm_frame_location {
  uint16_t function_index = 0;
  /// Offset of the instruction that was running (the call, for a caller).
  size_t offset = 0;
};

/// Why a hosted run failed, and where.
struct vm_failure {
  panic_reason reason = panic_reason::explicit_panic;
  /// The panic's own message (`panic("...")`'s text, what was refused), or
  /// the reason's description.
  std::string message;
  /// The calls active at the failure, innermost first.
  std::vector<vm_frame_location> frames;
};

/// The bytecode VM, the reference execution tier. Dispatches one
/// `bytecode_module`'s flat instruction stream via an ordinary `switch` over
/// `opcode`, with no computed-goto or threaded dispatch. Clarity takes
/// priority over dispatch speed in this tier.
///
/// Call frames are an explicit `std::vector<frame>` growing one entry per
/// nested `op_call`, not C++ call-stack recursion — this keeps Cinder's own
/// call-depth limit (and the `stack_overflow` panic it produces) independent
/// of how deep the host's own C++ stack happens to tolerate, and keeps the
/// dispatch loop as one flat `while` over whichever frame is current.
class vm {
public:
  explicit vm(const bytecode_module &module)
      : module_(module), globals_(module.global_count) {}

  /// Runs `module.functions[function_index]` to completion, with `args`
  /// copied into its entry frame's registers `[0, args.size())` — mirroring
  /// how `op_call` populates a callee frame from the caller's registers, so
  /// the outermost call is not a special case. Returns the panic reason on
  /// a checked-arithmetic violation, divide-by-zero, `op_panic`, or a
  /// call-depth limit violation.
  ///
  /// `globals_` (below) persists across separate `run` calls on the same
  /// `vm` instance — the driver relies on this to run `module.
  /// static_init_function` once, then call the program's real entry point
  /// with the globals it filled in still populated (see `interpret.cpp`).
  [[nodiscard]] auto run(uint16_t function_index,
                         std::span<const slot_value> args) const
      -> std::expected<vm_result, panic_reason>;

  /// Runs like `run`, for a host that must survive whatever the program
  /// does: no panic ends the process (`hosted_panic_scope`), the run stops
  /// once `limits` are exceeded, and a failure says where it happened.
  [[nodiscard]] auto run_hosted(uint16_t function_index,
                                std::span<const slot_value> args,
                                const run_limits &limits) const
      -> std::expected<vm_result, vm_failure>;

private:
  [[nodiscard]] auto execute(uint16_t function_index,
                             std::span<const slot_value> args,
                             const run_limits &limits) const
      -> std::expected<vm_result, vm_failure>;

  const bytecode_module &module_;
  /// The module-wide global-value table `op_load_global`/`op_store_global`
  /// index into — see `bytecode_module::global_count`'s doc comment.
  /// `mutable` because `run` is `const` (mirroring every other call's
  /// read-only view of `module_`) but a global write is, by construction,
  /// the one piece of state a `run` call is allowed to mutate for a later
  /// `run` call to observe.
  mutable std::vector<slot_value> globals_;
};

} // namespace cinder::bytecode
