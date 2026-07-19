// ==========================================================================
//  Linear scan register allocation — unit tests.
//
//  These drive `allocate_registers` directly rather than through the
//  compiler, because the properties that matter are precisely the ones a
//  compiled-program test cannot isolate: whether two virtuals *share* a
//  physical is invisible to a program's result, right up until the sharing
//  is wrong and the answer silently changes. Asserting on the assignment
//  itself is what makes "reuse happened" and "reuse was refused" separately
//  checkable.
//
//  Instructions are a uniform 4 bytes here, so instruction `i` owns byte
//  offsets `[4i, 4i+4)` and a site at `4i + 1` belongs to it. The real
//  encoding is variable-width; the allocator only ever compares offsets
//  against `instruction_offsets`, so a uniform stride is a faithful stand-in.
// ==========================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "src/bytecode_compiler/register_alloc.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;
using namespace kira::bytecode_compiler;

/// Builds an input whose instruction stream is `count` uniform 4-byte
/// instructions.
[[nodiscard]] auto with_instructions(size_t count, uint32_t virtual_count)
    -> allocation_input {
  auto input = allocation_input{};
  input.virtual_count = virtual_count;
  for (auto index = size_t{0}; index < count; ++index) {
    input.instruction_offsets.push_back(index * 4);
  }
  return input;
}

/// The byte offset instruction `instruction` starts at. `loop_range` is
/// expressed in offsets, not indices, because that is what the compiler has
/// on hand while emitting.
[[nodiscard]] auto offset_of(size_t instruction) -> size_t {
  return instruction * 4;
}

/// Mentions `reg` inside instruction `instruction`.
auto mention(allocation_input &input, uint32_t reg, size_t instruction)
    -> void {
  input.sites.push_back(register_site{.code_offset = instruction * 4 + 1,
                                      .reg = virtual_reg{reg}});
}

[[nodiscard]] auto allocate_or_fail(const allocation_input &input)
    -> allocation_result {
  // Allocation is total since register operands widened to `u16` — there is
  // no failure left to unwrap. Kept as a named helper so the tests below read
  // the same as they did when it could fail.
  return allocate_registers(input);
}

/// Two virtuals whose ranges do not overlap share one physical — the whole
/// point of the pass.
auto test_disjoint_virtuals_share_a_physical() -> void {
  auto input = with_instructions(4, 2);
  mention(input, 0, 0);
  mention(input, 0, 1);
  mention(input, 1, 2);
  mention(input, 1, 3);

  const auto result = allocate_or_fail(input);
  expect(result.assignment[0] == result.assignment[1],
         "disjoint virtuals should share a physical register");
  expect(result.register_count == 1,
         "two disjoint virtuals should need exactly one physical register");
}

/// ...and two whose ranges do overlap must not.
auto test_overlapping_virtuals_get_distinct_physicals() -> void {
  auto input = with_instructions(4, 2);
  mention(input, 0, 0);
  mention(input, 0, 3);
  mention(input, 1, 1);
  mention(input, 1, 2);

  const auto result = allocate_or_fail(input);
  expect(result.assignment[0] != result.assignment[1],
         "overlapping virtuals must not share a physical register");
  expect(result.register_count == 2,
         "two overlapping virtuals need two physical registers");
}

/// A value defined before a loop and last *mentioned* early inside it is
/// still read on the next iteration. First/last-mention intervals say it is
/// dead from its last mention onward, which would let the allocator hand its
/// physical to something defined later in the body — clobbering it before
/// the back edge runs again.
auto test_value_live_across_a_back_edge_is_not_reused() -> void {
  auto input = with_instructions(6, 2);
  mention(input, 0, 0); // defined before the loop
  mention(input, 0, 2); // read early in the body, never mentioned again
  mention(input, 1, 3); // defined later in the same body
  input.loops.push_back(
      loop_range{.header = offset_of(1), .back_edge = offset_of(4)});

  const auto result = allocate_or_fail(input);
  expect(result.assignment[0] != result.assignment[1],
         "a value read on the next loop iteration must not have its register "
         "reused later in the body");
}

/// Nested loops: extending an interval to an inner back edge can push it
/// into an outer loop it did not previously overlap, which must then extend
/// it again. One pass would stop short.
auto test_nested_loops_extend_to_a_fixpoint() -> void {
  auto input = with_instructions(12, 2);
  mention(input, 0, 1);
  mention(input, 0, 3);
  mention(input, 1, 8);
  input.loops.push_back(
      loop_range{.header = offset_of(2), .back_edge = offset_of(5)});
  input.loops.push_back(
      loop_range{.header = offset_of(4), .back_edge = offset_of(9)});

  const auto result = allocate_or_fail(input);
  expect(result.assignment[0] != result.assignment[1],
         "an interval extended into an enclosing loop must be extended again "
         "to that loop's back edge");
}

/// `op_call` reads `argc` *consecutive* registers, so a group has to land on
/// a contiguous physical run.
auto test_argument_groups_are_contiguous() -> void {
  auto input = with_instructions(6, 4);
  mention(input, 0, 0);
  mention(input, 1, 1);
  mention(input, 2, 2);
  mention(input, 3, 3);
  input.groups.push_back(register_group{.first = virtual_reg{1}, .count = 3});

  const auto result = allocate_or_fail(input);
  expect(result.assignment[2] == result.assignment[1] + 1 &&
             result.assignment[3] == result.assignment[1] + 2,
         "a call's argument block must occupy consecutive physical registers");
}

/// A group stays live until its *last* member dies, so an unrelated value
/// spanning the block cannot be given a register inside it.
auto test_argument_group_blocks_reuse_until_the_call() -> void {
  auto input = with_instructions(8, 3);
  mention(input, 0, 1);
  mention(input, 1, 2);
  mention(input, 2, 5); // the call, reading the whole block
  input.groups.push_back(register_group{.first = virtual_reg{0}, .count = 3});

  const auto result = allocate_or_fail(input);
  expect(result.assignment[1] == result.assignment[0] + 1 &&
             result.assignment[2] == result.assignment[0] + 2,
         "every member of an argument group is allocated as one unit");
  expect(result.register_count == 3,
         "a three-register argument block needs three physical registers");
}

/// Parameters are identity-mapped: the calling convention has already
/// written them into `[0, param_count)` before the first instruction runs.
auto test_parameters_are_pinned_to_low_physicals() -> void {
  auto input = with_instructions(6, 3);
  input.pinned_prefix = 2;
  mention(input, 0, 5); // first *mentioned* late, but live from entry
  mention(input, 1, 5);
  mention(input, 2, 0);

  const auto result = allocate_or_fail(input);
  expect(result.assignment[0] == 0 && result.assignment[1] == 1,
         "parameters must be identity-mapped to the physicals the calling "
         "convention fills at entry");
  expect(result.assignment[2] == 2,
         "a temporary must not be given a register still holding a parameter");
}

/// The case that makes pinning load-bearing rather than emergent.
///
/// When every parameter is mentioned, identity mapping falls out of the sort
/// order for free — parameters sort first and the scan hands out the lowest
/// free register, so they land on 0, 1, 2... whether or not anything pins
/// them. That made the test above pass even with pinning disabled outright.
///
/// Skipping a parameter breaks that coincidence: an unmentioned parameter
/// never enters the scan, so parameter 1 is the first thing allocated and
/// the lowest-free-run search would hand it physical 0 — while the caller
/// wrote its argument into physical 1.
///
/// What that corrupts is not the parameter's own index (which is forced back
/// to its identity mapping regardless) but the *reservation*: physical 1 is
/// left looking free, so the next virtual is allocated on top of the live
/// parameter. The observable symptom is therefore two live virtuals sharing
/// one register, which is what this asserts.
auto test_pinning_reserves_an_unmentioned_parameters_successor() -> void {
  auto input = with_instructions(4, 3);
  input.pinned_prefix = 2;
  // Parameter 0 is never read. Parameter 1 and a temporary are both live
  // across the same span, so they must not collide.
  mention(input, 1, 0);
  mention(input, 1, 3);
  mention(input, 2, 0);
  mention(input, 2, 3);

  const auto result = allocate_or_fail(input);
  expect(result.assignment[1] == 1,
         "a parameter must keep the physical the caller wrote it into, even "
         "when an earlier parameter is never read");
  expect(result.assignment[1] != result.assignment[2],
         "a live parameter's register must be reserved against other "
         "virtuals, even when an earlier parameter is never read");
}

/// A frame has to be big enough for the calling convention to write every
/// parameter, even one the body never reads.
auto test_frame_covers_unread_parameters() -> void {
  auto input = with_instructions(2, 3);
  input.pinned_prefix = 3;
  mention(input, 0, 0);

  const auto result = allocate_or_fail(input);
  expect(result.register_count == 3,
         "register_count must cover every parameter, read or not");
}

/// `op_addr_local` hands out a pointer to a frame register. Nothing bounds
/// how long that pointer is held, so the register cannot be recycled at its
/// last mention.
auto test_address_taken_registers_live_to_the_end() -> void {
  auto input = with_instructions(6, 2);
  mention(input, 0, 0);
  mention(input, 0, 1);
  mention(input, 1, 3);
  input.address_taken.push_back(virtual_reg{0});

  const auto result = allocate_or_fail(input);
  expect(result.assignment[0] != result.assignment[1],
         "a register whose address escaped must not be reused later in the "
         "function");
}

/// The headline result: virtual count is no longer the thing that matters.
auto test_many_short_lived_virtuals_collapse() -> void {
  constexpr auto k_count = uint32_t{1000};
  auto input = with_instructions(k_count, k_count);
  for (auto index = uint32_t{0}; index < k_count; ++index) {
    mention(input, index, index);
  }

  const auto result = allocate_or_fail(input);
  expect(result.register_count == 1,
         "a thousand virtuals that never overlap should collapse onto one "
         "physical register");
}

/// The case that used to be a hard compile error, and is now ordinary.
///
/// With a `u8` register operand, 300 simultaneously live values had nowhere
/// to go: the frame could not address a 257th register, there was no spill
/// slot to spill to, and the user's only recourse was splitting the function
/// by hand. Widening the operand to `u16` makes this unremarkable — it needs
/// 300 registers and gets exactly 300, none shared, since every one of them
/// really is live at the same time.
auto test_many_simultaneously_live_values_all_get_distinct_physicals() -> void {
  constexpr auto k_count = uint32_t{300};
  auto input = with_instructions(2, k_count);
  for (auto index = uint32_t{0}; index < k_count; ++index) {
    mention(input, index, 0);
    mention(input, index, 1);
  }

  const auto result = allocate_or_fail(input);
  expect(result.register_count == k_count,
         "300 values live at the same instant need 300 distinct physicals");

  auto seen = std::vector<bool>(k_count, false);
  for (auto index = uint32_t{0}; index < k_count; ++index) {
    const auto physical = result.assignment[index];
    expect(physical < k_count && !seen[physical],
           "every simultaneously live value must get its own physical "
           "register — sharing one would clobber a value still in use");
    seen[physical] = true;
  }
}

/// The same 300 virtuals, laid out so only a few are ever live at once, do
/// fit — the case that previously failed to compile purely on virtual count.
auto test_many_virtuals_fit_when_not_simultaneously_live() -> void {
  constexpr auto k_count = uint32_t{300};
  auto input = with_instructions(k_count + 1, k_count);
  for (auto index = uint32_t{0}; index < k_count; ++index) {
    mention(input, index, index);
    mention(input, index, index + 1);
  }

  const auto result = allocate_or_fail(input);
  expect(result.register_count <= 2,
         "300 virtuals with overlapping-by-one ranges need only two physicals");
}

} // namespace

auto main() -> int {
  test_disjoint_virtuals_share_a_physical();
  test_overlapping_virtuals_get_distinct_physicals();
  test_value_live_across_a_back_edge_is_not_reused();
  test_nested_loops_extend_to_a_fixpoint();
  test_argument_groups_are_contiguous();
  test_argument_group_blocks_reuse_until_the_call();
  test_parameters_are_pinned_to_low_physicals();
  test_pinning_reserves_an_unmentioned_parameters_successor();
  test_frame_covers_unread_parameters();
  test_address_taken_registers_live_to_the_end();
  test_many_short_lived_virtuals_collapse();
  test_many_simultaneously_live_values_all_get_distinct_physicals();
  test_many_virtuals_fit_when_not_simultaneously_live();
  return 0;
}
