#pragma once

#include <cstddef>
#include <cstdint>

namespace cinder::runtime::coroutine {

// ==========================================================================
//  Coroutine object layout shared by `bytecode::vm`, `bytecode_compiler` and
//  `llvm_codegen`. Every suspendable function (today only `generator def`)
//  is a fixed block of `k_slot_count` 8-byte slots, allocated like any other
//  heap value (see `layout.h`):
//
//    { step_function; state_ptr; resume_index; status }
//
//  `step_function` is the function index (VM) or function pointer (LLVM) of
//  the compiled state machine; `state_ptr` points at the flat block holding
//  locals that live across suspensions (null when there are none);
//  `resume_index` selects which suspension point the step function resumes
//  after (0 at first entry). The step function is called with
//  `(state_ptr, resume_index, object)`.
//
//  `status` is the one word every consumer of a coroutine shares. A
//  generator's `.next()`, a task's poll, and a drop all read and write it,
//  so the values below are the protocol rather than a per-feature detail.
// ==========================================================================

inline constexpr size_t k_slot_count = 4;

/// Slot indices within a coroutine object.
inline constexpr size_t k_step_function_slot = 0;
inline constexpr size_t k_state_ptr_slot = 1;
inline constexpr size_t k_resume_index_slot = 2;
inline constexpr size_t k_status_slot = 3;

/// Number of arguments the step function takes.
inline constexpr size_t k_step_arg_count = 3;

/// Values of the `status` slot.
enum class status : uint64_t {
  /// Not finished: the next resume continues the body.
  suspended = 0,
  /// The body returned; resuming again does nothing.
  done = 1,
  /// A cancel was requested and the body is being resumed only to observe it
  /// and unwind (`hir_generator_cancelled` tests for this value).
  cancel_requested = 2,
};

[[nodiscard]] constexpr auto raw(status s) -> uint64_t {
  return static_cast<uint64_t>(s);
}

} // namespace cinder::runtime::coroutine
