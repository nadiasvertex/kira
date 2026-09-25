#include "src/bytecode_compiler/register_alloc.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <queue>
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

/// Which physical registers are free, answering "lowest start of a free run
/// of `count` registers" in O(log n).
///
/// A segment tree over `[0, capacity)`: each node records the longest free
/// run touching its left edge (`prefix`), its right edge (`suffix`), and
/// anywhere inside it (`best`). The leftmost run long enough is found by
/// descending — left child first, then a run straddling the midpoint, then
/// the right child. A linear scan of the register file per value made
/// allocation quadratic in the number of values live at once: 65536 of them
/// took minutes.
///
/// Registers at or above `capacity` are all free, so the tree starts small
/// and doubles as the frame grows. A function that only ever holds a few
/// values live pays for a few dozen nodes, not all 65536 registers.
class free_register_map {
public:
  free_register_map() { grow_to(1); }

  /// The lowest physical starting a free run of `count` registers. A run may
  /// begin inside the tree and continue past `capacity` into registers never
  /// yet used, so this always has an answer; whether it fits in the frame is
  /// the caller's check.
  [[nodiscard]] auto lowest_free_run(uint32_t count) const -> uint32_t {
    if (best_[1] < count) {
      return capacity_ - suffix_[1];
    }
    auto node = size_t{1};
    auto low = uint32_t{0};
    auto length = capacity_;
    while (node < capacity_) {
      const auto half = length / 2;
      const auto left = node * 2;
      const auto right = left + 1;
      if (best_[left] >= count) {
        node = left;
      } else if (suffix_[left] + prefix_[right] >= count) {
        return low + half - suffix_[left];
      } else {
        node = right;
        low += half;
      }
      length = half;
    }
    return low;
  }

  auto set_busy(uint32_t first, uint32_t count, bool busy) -> void {
    grow_to(first + count);
    for (auto physical = first; physical < first + count; ++physical) {
      auto node = capacity_ + physical;
      const auto free = busy ? uint32_t{0} : uint32_t{1};
      prefix_[node] = free;
      suffix_[node] = free;
      best_[node] = free;
      for (node /= 2; node >= 1; node /= 2) {
        combine(node);
      }
    }
  }

private:
  /// The number of registers node `node` covers.
  [[nodiscard]] auto span_of(size_t node) const -> uint32_t {
    return capacity_ >> (std::bit_width(node) - 1);
  }

  auto combine(size_t node) -> void {
    const auto left = node * 2;
    const auto right = left + 1;
    const auto half = span_of(left);
    prefix_[node] = prefix_[left] == half ? half + prefix_[right]
                                           : prefix_[left];
    suffix_[node] = suffix_[right] == half ? half + suffix_[left]
                                            : suffix_[right];
    best_[node] = std::max({best_[left], best_[right],
                            suffix_[left] + prefix_[right]});
  }

  /// Doubles the tree until it covers `[0, limit)`, carrying every leaf's
  /// state across and treating the new registers as free.
  auto grow_to(uint32_t limit) -> void {
    if (limit <= capacity_ && !best_.empty()) {
      return;
    }
    auto capacity = std::max(capacity_, uint32_t{64});
    while (capacity < limit) {
      capacity *= 2;
    }
    auto leaves = std::vector<uint32_t>(capacity, 1);
    for (auto physical = uint32_t{0}; physical < capacity_; ++physical) {
      leaves[physical] = best_[capacity_ + physical];
    }
    capacity_ = capacity;
    prefix_.assign(size_t{2} * capacity_, 0);
    suffix_.assign(size_t{2} * capacity_, 0);
    best_.assign(size_t{2} * capacity_, 0);
    for (auto physical = uint32_t{0}; physical < capacity_; ++physical) {
      prefix_[capacity_ + physical] = leaves[physical];
      suffix_[capacity_ + physical] = leaves[physical];
      best_[capacity_ + physical] = leaves[physical];
    }
    for (auto node = size_t{capacity_} - 1; node >= 1; --node) {
      combine(node);
    }
  }

  uint32_t capacity_ = 0;
  std::vector<uint32_t> prefix_;
  std::vector<uint32_t> suffix_;
  std::vector<uint32_t> best_;
};

} // namespace

auto allocate_registers(const allocation_input &input)
    -> std::expected<allocation_result, register_file_exhausted> {
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

  auto assignment = std::vector<uint16_t>(input.virtual_count, 0);
  auto free_registers = free_register_map{};
  // Ordered by end point, soonest first, so expiring is a pop per group
  // rather than a pass over everything live.
  const auto ends_later = [](const active_group &lhs,
                             const active_group &rhs) -> bool {
    return lhs.end > rhs.end;
  };
  auto active = std::priority_queue<active_group, std::vector<active_group>,
                                    decltype(ends_later)>{ends_later};
  auto highest = uint32_t{0};

  const auto assign = [&](const scan_group &group, uint32_t physical) -> void {
    for (auto offset = uint32_t{0}; offset < group.count; ++offset) {
      const auto id = group.first + offset;
      if (id < input.virtual_count) {
        assignment[id] = static_cast<uint16_t>(physical + offset);
      }
    }
    free_registers.set_busy(physical, group.count, true);
    active.push(active_group{
        .physical = physical, .count = group.count, .end = group.range.end});
    highest = std::max(highest, physical + group.count);
  };

  for (const auto index : order) {
    const auto &group = groups[index];

    // Expire. A group whose interval *ends* exactly where this one starts is
    // deliberately not expired: within a single instruction the VM reads its
    // source operands and writes its destination, and letting those share a
    // physical would make the write clobber a source still to be read.
    while (!active.empty() && active.top().end < group.range.start) {
      free_registers.set_busy(active.top().physical, active.top().count,
                              false);
      active.pop();
    }

    const auto pinned = group.first < input.pinned_prefix && group.count == 1;
    // A group needs its registers contiguous (call opcodes read `argc`
    // consecutive registers), so a long-lived value in the middle of an
    // otherwise free run can push it higher. If the lowest run that fits
    // runs off the end of the frame, the frame is out of room.
    const auto physical =
        pinned ? group.first : free_registers.lowest_free_run(group.count);
    if (physical + group.count > k_physical_register_count) {
      return std::unexpected(register_file_exhausted{});
    }
    assign(group, physical);
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
      assignment[id] = static_cast<uint16_t>(id);
    }
  }

  return allocation_result{.assignment = std::move(assignment),
                           .register_count = highest};
}

} // namespace kira::bytecode_compiler
