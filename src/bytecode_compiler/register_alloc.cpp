#include "src/bytecode_compiler/register_alloc.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <vector>

namespace kira::bytecode_compiler {

namespace {

/// A live range over instruction indices, closed at both ends. `live` is
/// false for a virtual that was allocated but never actually mentioned in
/// the code buffer — it needs an assignment for completeness but occupies no
/// physical register.
struct live_interval {
  size_t start = 0;
  size_t end = 0;
  bool live = false;

  auto observe(size_t index) -> void {
    if (!live) {
      start = index;
      end = index;
      live = true;
      return;
    }
    start = std::min(start, index);
    end = std::max(end, index);
  }
};

/// One allocation unit: a single virtual, or a contiguous argument run that
/// must be allocated as one block.
struct scan_group {
  uint32_t first = 0;
  uint32_t count = 1;
  live_interval range;
};

/// A group currently holding physical registers `[physical, physical+count)`.
struct active_group {
  uint32_t physical = 0;
  uint32_t count = 1;
  size_t end = 0;
};

/// The instruction a byte offset falls inside: the last instruction whose
/// opcode byte is at or before it.
[[nodiscard]] auto instruction_index_of(const std::vector<size_t> &offsets,
                                        size_t code_offset) -> size_t {
  const auto after = std::ranges::upper_bound(offsets, code_offset);
  if (after == offsets.begin()) {
    return 0;
  }
  return static_cast<size_t>(std::distance(offsets.begin(), after) - 1);
}

/// Extends every group overlapping a loop out to that loop's back edge.
///
/// Without this the allocator would reuse a physical that is still read on
/// the next iteration: a virtual defined before the loop and last *mentioned*
/// early in the body has a first/last-mention interval that ends mid-loop,
/// which says "dead" about a value the back edge is about to read again.
///
/// Run to a fixpoint because loops nest: extending an interval out to an
/// inner loop's back edge can push it into an outer loop it did not
/// previously overlap, which must then extend it again.
auto extend_across_loops(std::vector<scan_group> &groups,
                         const std::vector<loop_range> &loops,
                         const std::vector<size_t> &instruction_offsets)
    -> void {
  if (loops.empty()) {
    return;
  }
  auto ranges = std::vector<std::pair<size_t, size_t>>{};
  ranges.reserve(loops.size());
  for (const auto &loop : loops) {
    ranges.emplace_back(
        instruction_index_of(instruction_offsets, loop.header),
        instruction_index_of(instruction_offsets, loop.back_edge));
  }
  for (size_t pass = 0; pass <= loops.size(); ++pass) {
    auto changed = false;
    for (auto &group : groups) {
      if (!group.range.live) {
        continue;
      }
      for (const auto &[header, back_edge] : ranges) {
        const auto overlaps =
            group.range.start <= back_edge && group.range.end >= header;
        if (overlaps && group.range.end < back_edge) {
          group.range.end = back_edge;
          changed = true;
        }
      }
    }
    if (!changed) {
      return;
    }
  }
}

/// The lowest physical register starting a free run of `count` registers, or
/// `nullopt` when no such run exists.
[[nodiscard]] auto find_free_run(const std::vector<bool> &busy, uint32_t count)
    -> std::optional<uint32_t> {
  auto run = uint32_t{0};
  for (auto index = uint32_t{0}; index < k_physical_register_count; ++index) {
    if (busy[index]) {
      run = 0;
      continue;
    }
    ++run;
    if (run == count) {
      return index + 1 - count;
    }
  }
  return std::nullopt;
}

} // namespace

auto allocate_registers(const allocation_input &input)
    -> std::expected<allocation_result, allocation_error> {
  if (input.pinned_prefix > k_physical_register_count) {
    return std::unexpected(allocation_error{
        .message = std::format(
            "takes more parameters ({}) than the {} registers a frame can "
            "address, so there is nowhere to put them at entry",
            input.pinned_prefix, k_physical_register_count)});
  }

  // ----------------------------------------------------------------------
  //  Live intervals, per virtual, from first and last mention.
  // ----------------------------------------------------------------------
  auto intervals = std::vector<live_interval>(input.virtual_count);
  for (const auto &site : input.sites) {
    if (site.reg.id >= input.virtual_count) {
      continue;
    }
    intervals[site.reg.id].observe(
        instruction_index_of(input.instruction_offsets, site.code_offset));
  }

  const auto last_instruction = input.instruction_offsets.empty()
                                    ? size_t{0}
                                    : input.instruction_offsets.size() - 1;

  // A parameter is live from entry whether or not it is mentioned early —
  // the calling convention has already written it by the time the first
  // instruction runs.
  for (auto id = uint32_t{0}; id < input.pinned_prefix; ++id) {
    if (intervals[id].live) {
      intervals[id].start = 0;
    }
  }

  // `op_addr_local` hands out the address of a frame register, and nothing
  // here can bound how long that address is held — it may be stored into a
  // struct and read back through much later. Last mention of the *register*
  // says nothing about the lifetime of a pointer to it, so these stay live
  // to the end of the function.
  for (const auto reg : input.address_taken) {
    if (reg.id < input.virtual_count && intervals[reg.id].live) {
      intervals[reg.id].end = last_instruction;
    }
  }

  // ----------------------------------------------------------------------
  //  Grouping: contiguous argument runs allocate as one unit.
  // ----------------------------------------------------------------------
  auto group_of = std::vector<uint32_t>(input.virtual_count, UINT32_MAX);
  auto groups = std::vector<scan_group>{};
  for (const auto &group : input.groups) {
    const auto index = static_cast<uint32_t>(groups.size());
    auto entry = scan_group{.first = group.first.id, .count = group.count};
    for (auto offset = uint32_t{0}; offset < group.count; ++offset) {
      const auto id = group.first.id + offset;
      if (id >= input.virtual_count || group_of[id] != UINT32_MAX) {
        continue;
      }
      group_of[id] = index;
      if (intervals[id].live) {
        entry.range.observe(intervals[id].start);
        entry.range.observe(intervals[id].end);
      }
    }
    groups.push_back(entry);
  }
  for (auto id = uint32_t{0}; id < input.virtual_count; ++id) {
    if (group_of[id] != UINT32_MAX) {
      continue;
    }
    group_of[id] = static_cast<uint32_t>(groups.size());
    groups.push_back(
        scan_group{.first = id, .count = 1, .range = intervals[id]});
  }

  extend_across_loops(groups, input.loops, input.instruction_offsets);

  // ----------------------------------------------------------------------
  //  The scan itself: groups in order of interval start, expiring actives
  //  whose intervals have ended, reusing whatever that frees.
  // ----------------------------------------------------------------------
  auto order = std::vector<uint32_t>{};
  order.reserve(groups.size());
  for (auto index = uint32_t{0}; index < groups.size(); ++index) {
    if (groups[index].range.live) {
      order.push_back(index);
    }
  }
  std::ranges::sort(order, [&](uint32_t lhs, uint32_t rhs) -> bool {
    if (groups[lhs].range.start != groups[rhs].range.start) {
      return groups[lhs].range.start < groups[rhs].range.start;
    }
    // At equal start, pinned virtuals go first so they claim the low
    // physicals the calling convention requires them to occupy.
    return groups[lhs].first < groups[rhs].first;
  });

  auto assignment = std::vector<uint8_t>(input.virtual_count, 0);
  auto busy = std::vector<bool>(k_physical_register_count, false);
  auto active = std::vector<active_group>{};
  auto highest = uint32_t{0};

  const auto assign = [&](const scan_group &group, uint32_t physical) -> void {
    for (auto offset = uint32_t{0}; offset < group.count; ++offset) {
      const auto id = group.first + offset;
      if (id < input.virtual_count) {
        assignment[id] = static_cast<uint8_t>(physical + offset);
      }
      busy[physical + offset] = true;
    }
    active.push_back(active_group{
        .physical = physical, .count = group.count, .end = group.range.end});
    highest = std::max(highest, physical + group.count);
  };

  for (const auto index : order) {
    const auto &group = groups[index];

    // Expire. A group whose interval *ends* exactly where this one starts is
    // deliberately not expired: within a single instruction the VM reads its
    // source operands and writes its destination, and letting those share a
    // physical would make the write clobber a source still to be read.
    auto surviving = std::vector<active_group>{};
    surviving.reserve(active.size());
    for (const auto &entry : active) {
      if (entry.end >= group.range.start) {
        surviving.push_back(entry);
        continue;
      }
      for (auto offset = uint32_t{0}; offset < entry.count; ++offset) {
        busy[entry.physical + offset] = false;
      }
    }
    active = std::move(surviving);

    const auto pinned = group.first < input.pinned_prefix && group.count == 1;
    const auto physical = pinned ? std::optional<uint32_t>{group.first}
                                 : find_free_run(busy, group.count);
    if (!physical.has_value()) {
      return std::unexpected(allocation_error{
          .message = std::format(
              "needs more than {} values alive at the same time, which this "
              "bytecode format's u8 register operands cannot address. Register "
              "reuse already recycles each value's register as soon as it "
              "dies, "
              "so this is a function holding genuinely that many live values "
              "at "
              "once — splitting it into smaller functions is what shortens "
              "their lifetimes",
              k_physical_register_count)});
    }
    assign(group, *physical);
  }

  // The frame must be large enough for the calling convention to write every
  // parameter at entry, even if some are never read.
  highest = std::max(highest, input.pinned_prefix);

  // A parameter the body never mentions never entered the scan, so it has no
  // assignment yet. Give it its identity mapping for consistency — but only
  // that case: overwriting a *live* parameter's assignment here would paper
  // over the scan getting it wrong, since the scan is also what reserves the
  // physical against other virtuals claiming it.
  for (auto id = uint32_t{0}; id < input.pinned_prefix; ++id) {
    if (!intervals[id].live) {
      assignment[id] = static_cast<uint8_t>(id);
    }
  }

  return allocation_result{.assignment = std::move(assignment),
                           .register_count = static_cast<uint16_t>(highest)};
}

} // namespace kira::bytecode_compiler
