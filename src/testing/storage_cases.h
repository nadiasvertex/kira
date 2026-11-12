#pragma once

// The value cases every backend that compiles the shared storage form must
// pass. A backend supplies a `case_backend` that compiles a fixture and runs
// its functions; the expected values are worked out by hand here (layout
// offsets, tags, narrow widths), not taken from another backend.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "src/bytecode/panic.h"
#include "src/runtime/allocator.h"
#include "src/testing/test_assert.h"

namespace cinder::testing::storage_cases {

/// Caller-owned result bytes, filled with a pattern that is not a valid value
/// so an unwritten part shows up.
struct result_bytes {
  alignas(16) std::array<std::byte, 64> bytes{};

  result_bytes() { bytes.fill(std::byte{0xAA}); }

  template <typename T> [[nodiscard]] auto at(size_t offset) const -> T {
    auto value = T{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
  }
};

/// A compiled fixture that can run its functions.
class case_runner {
public:
  virtual ~case_runner() = default;

  /// Runs a function that returns a scalar; the run must not panic. A value
  /// narrower than 64 bits comes back sign- or zero-extended by its type.
  [[nodiscard]] virtual auto scalar(std::string_view name,
                                    std::span<const int64_t> args)
      -> int64_t = 0;
  /// Runs a function that returns a scalar and reports the panic it ended in.
  [[nodiscard]] virtual auto panics(std::string_view name,
                                    std::span<const int64_t> args)
      -> std::optional<bytecode::panic_reason> = 0;
  /// Runs a function that builds its result through an address.
  [[nodiscard]] virtual auto into(std::string_view name,
                                  std::span<const int64_t> args)
      -> result_bytes = 0;
  /// Runs a scalar function whose one parameter travels by address, passing
  /// the address of storage the host owns.
  [[nodiscard]] virtual auto scalar_at(std::string_view name, void *argument)
      -> int64_t = 0;
  /// Whether `panics` can report an explicit `panic(...)`. The native runtime
  /// ends the process on one (`scheduler::raise_native_panic`), so only the
  /// VM hands it back to the host.
  [[nodiscard]] virtual auto reports_explicit_panic() const -> bool = 0;
};

/// Compiles fixtures: the file `name` under `src/testdata/<dir>/`.
class case_backend {
public:
  virtual ~case_backend() = default;
  [[nodiscard]] virtual auto open(std::string_view dir, std::string_view name)
      -> std::unique_ptr<case_runner> = 0;
};

inline auto expect_equal(int64_t actual, int64_t wanted, std::string_view what)
    -> void {
  if (actual != wanted) {
    std::cerr << what << ": expected " << wanted << ", got " << actual << "\n";
    fail("a computed value differs");
  }
}

// ==========================================================================
//  Scalars and control flow
// ==========================================================================

inline auto scalars_and_control_flow(case_backend &backend) -> void {
  auto p = backend.open("hir_storage_test", "flow.cn");
  struct row {
    std::string_view name;
    std::vector<int64_t> args;
    int64_t wanted;
  };
  const auto rows = std::vector<row>{
      {"early", {1}, 1},
      {"early", {-1}, 2},
      {"both_return", {0}, 2},
      {"pick", {7}, 7},
      {"pick", {-7}, 7},
      {"chain", {11}, 3},
      {"chain", {7}, 2},
      {"chain", {1}, 1},
      {"assign_in_branch", {4}, 1},
      {"assign_in_branch", {0}, 2},
      {"count", {5}, 5},
      {"count_with_locals", {4}, 10},
      {"stops", {3}, 3},
      {"skips", {5}, 12},
      {"returns_from_loop", {4}, 4},
      {"nested", {4}, 5},
      {"stepped", {5}, 8},
      // `and`/`or` evaluate their right side only when the left does not
      // decide the result: dividing by zero here must never run.
      {"guarded", {0}, 0},
      {"guarded", {5}, 1},
      {"guarded", {20}, 0},
      {"either", {0, 0}, 0},
      {"either", {0, 1}, 1},
      {"either", {1, 0}, 1},
  };
  for (const auto &entry : rows) {
    expect_equal(p->scalar(entry.name, entry.args), entry.wanted, entry.name);
  }
}

// ==========================================================================
//  Aggregate results built in caller storage
// ==========================================================================

inline auto aggregate_results_are_built_in_the_caller_storage(
    case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "values.cn");

  auto pair = p->into("make_pair", std::vector<int64_t>{5});
  expect_equal(pair.at<int32_t>(0), 5, "make_pair.a");
  expect_equal(pair.at<int32_t>(4), 6, "make_pair.b");

  // The nested pairs are built where they sit inside the line.
  auto line = p->into("make_line", std::vector<int64_t>{5});
  expect_equal(line.at<int32_t>(0), 5, "line.from.a");
  expect_equal(line.at<int32_t>(4), 6, "line.from.b");
  expect_equal(line.at<int32_t>(8), 0, "line.to.a");
  expect_equal(line.at<int32_t>(12), 5, "line.to.b");

  auto forwarded = p->into("forward", std::vector<int64_t>{7});
  expect_equal(forwarded.at<int32_t>(0), 7, "forward.a");
  expect_equal(forwarded.at<int32_t>(4), 8, "forward.b");

  auto tuple = p->into("make_tuple", std::vector<int64_t>{3});
  expect_equal(tuple.at<int32_t>(0), 3, "tuple.0");
  expect_equal(tuple.at<int32_t>(4), 4, "tuple.1");

  auto array = p->into("make_array", std::vector<int64_t>{9});
  expect_equal(array.at<int32_t>(0), 9, "array[0]");
  expect_equal(array.at<int32_t>(4), 9, "array[1]");
  expect_equal(array.at<int32_t>(8), 0, "array[2]");

  // A sum: the tag, then the active payload at the payload offset.
  auto rect = p->into("make_rect", std::vector<int64_t>{4});
  expect_equal(rect.at<int64_t>(0), 1, "rect tag");
  expect_equal(rect.at<int32_t>(8), 4, "rect.0");
  expect_equal(rect.at<int32_t>(12), 5, "rect.1");
  auto empty = p->into("make_empty", std::vector<int64_t>{});
  expect_equal(empty.at<int64_t>(0), 2, "empty tag");

  // A sum nested in a struct, with trailing fields after its padding.
  auto framed = p->into("make_framed", std::vector<int64_t>{3});
  expect_equal(framed.at<int64_t>(0), 0, "framed.kind tag");
  expect_equal(framed.at<int32_t>(8), 3, "framed.kind payload");
  expect_equal(framed.at<int32_t>(16), 3, "framed.id");

  // Larger than a register image, built and forwarded through two calls.
  auto wide = p->into("forward_wide", std::vector<int64_t>{100});
  for (int64_t part = 0; part < 4; ++part) {
    expect_equal(wide.at<int64_t>(static_cast<size_t>(part) * 8), 100 + part,
                 "wide member");
  }

  // Members narrower than a word are stored at their own widths.
  auto flags = p->into("make_flags", std::vector<int64_t>{42});
  expect_equal(flags.at<uint8_t>(0), 1, "flags.lit");
  expect_equal(flags.at<int8_t>(1), -5, "flags.small");
  expect_equal(flags.at<int16_t>(2), -300, "flags.mid");
  expect_equal(flags.at<int32_t>(4), 42, "flags.id");
}

// ==========================================================================
//  Locals, assignment, and borrows
// ==========================================================================

inline auto locals_assignment_and_borrows(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "values.cn");
  expect_equal(p->scalar("bind", std::vector<int64_t>{3}), 4, "bind");
  expect_equal(p->scalar("borrow", std::vector<int64_t>{9}), 9, "borrow");
  expect_equal(p->scalar("build_line_sum", std::vector<int64_t>{5}),
               5 + 6 + 0 + 5, "build_line_sum");
  // `p.a = 40` through `&mut p`, then reading both fields: 40 + (n + 1).
  expect_equal(p->scalar("mutate", std::vector<int64_t>{2}), 43, "mutate");

  // The replacement is built first, then moved over the old value.
  auto reassigned = p->into("reassign", std::vector<int64_t>{5});
  expect_equal(reassigned.at<int32_t>(0), 6, "reassign.a");
  expect_equal(reassigned.at<int32_t>(4), 7, "reassign.b");

  auto returned = p->into("return_local", std::vector<int64_t>{8});
  expect_equal(returned.at<int32_t>(0), 8, "return_local.a");
  expect_equal(returned.at<int32_t>(4), 9, "return_local.b");

  // `p = bump(&p)`: the new value is built apart from the borrowed one.
  auto bumped = p->into("self_bump", std::vector<int64_t>{2});
  expect_equal(bumped.at<int32_t>(0), 3, "self_bump.a");
  expect_equal(bumped.at<int32_t>(4), 3, "self_bump.b");
}

// ==========================================================================
//  Arguments that travel by address
// ==========================================================================

inline auto aggregate_arguments_travel_by_address(case_backend &backend)
    -> void {
  auto p = backend.open("bytecode_storage_test", "values.cn");
  expect_equal(p->scalar("pass_fresh", std::vector<int64_t>{10}), 20,
               "pass_fresh");
  expect_equal(p->scalar("pass_wide", std::vector<int64_t>{50}), 50 + 53,
               "pass_wide");

  // A host passes a parameter as the address of storage it owns.
  struct alignas(8) pair_bytes {
    int32_t a;
    int32_t b;
  };
  auto argument = pair_bytes{.a = 3, .b = 4};
  expect_equal(p->scalar_at("pass_local", &argument), 4, "pass_local");
}

// ==========================================================================
//  Frames, recursion, and the heap
// ==========================================================================

inline auto deep_recursion_keeps_frame_storage_stable(case_backend &backend)
    -> void {
  auto p = backend.open("bytecode_storage_test", "values.cn");
  // Each level reads the value its callee built in its own frame.
  auto deep = p->into("deep", std::vector<int64_t>{3000});
  expect_equal(deep.at<int32_t>(0), 3000, "deep.a");
  expect_equal(deep.at<int32_t>(4), 1 + 2 * 3000, "deep.b");
}

inline auto values_are_not_allocated_on_the_heap(case_backend &backend)
    -> void {
  auto p = backend.open("bytecode_storage_test", "values.cn");
  const auto before = runtime::heap_allocation_count();
  static_cast<void>(p->into("make_line", std::vector<int64_t>{5}));
  static_cast<void>(p->into("make_framed", std::vector<int64_t>{5}));
  static_cast<void>(p->into("deep", std::vector<int64_t>{200}));
  expect_equal(p->scalar("pass_fresh", std::vector<int64_t>{2}), 4,
               "pass_fresh");
  expect_equal(static_cast<int64_t>(runtime::heap_allocation_count() - before),
               0, "heap allocations");
}

inline auto narrow_signed_values_are_extended_when_read(case_backend &backend)
    -> void {
  auto p = backend.open("bytecode_storage_test", "values.cn");
  expect_equal(p->scalar("small_flags", std::vector<int64_t>{1}), -305,
               "small_flags");
  expect_equal(p->scalar("neg_literal", std::vector<int64_t>{}), -128,
               "neg_literal");
  expect_equal(p->scalar("narrow_compare", std::vector<int64_t>{7}), 1,
               "narrow_compare");
  // Returned whole, without a cast that would widen it first.
  expect_equal(p->scalar("mid_of", std::vector<int64_t>{7}), -300, "mid_of");
}

// ==========================================================================
//  Packed values
// ==========================================================================

inline auto packed_values_are_accessed_without_alignment(case_backend &backend)
    -> void {
  auto p = backend.open("bytecode_storage_test", "packed.cn");

  // `header` is 7 bytes with no padding: `len` starts at byte 3.
  auto header = p->into("make_header", std::vector<int64_t>{5});
  expect_equal(header.at<uint16_t>(0), 0x1234, "header.magic");
  expect_equal(header.at<uint8_t>(2), 0xAB, "header.flags");
  expect_equal(header.at<uint32_t>(3), 5, "header.len");

  // A packed struct inside a packed struct is not realigned either.
  auto framed = p->into("make_framed", std::vector<int64_t>{9});
  expect_equal(framed.at<uint8_t>(0), 7, "framed.kind");
  expect_equal(framed.at<uint16_t>(1), 0x1234, "framed.head.magic");
  expect_equal(framed.at<uint8_t>(3), 0xAB, "framed.head.flags");
  expect_equal(framed.at<uint32_t>(4), 9, "framed.head.len");

  expect_equal(p->scalar("total", std::vector<int64_t>{5}), 0x1234 + 0xAB + 5,
               "total");
  expect_equal(p->scalar("framed_len", std::vector<int64_t>{9}), 9 + 7,
               "framed_len");
  // Written and read back through a reference to the packed struct.
  expect_equal(p->scalar("via_reference", std::vector<int64_t>{5}),
               99 * 1000 + 0x1234, "via_reference");

  // A host may place a packed parameter at any address: here its `len`
  // field starts at an odd one.
  alignas(8) auto raw = std::array<std::byte, 16>{};
  const auto magic = uint16_t{0x1234};
  const auto len = uint32_t{0x01020304};
  std::memcpy(raw.data() + 2, &magic, sizeof(magic));
  std::memcpy(raw.data() + 5, &len, sizeof(len));
  expect_equal(p->scalar_at("read_len", raw.data() + 2), 0x01020304,
               "read_len");
}

// ==========================================================================
//  Drops
// ==========================================================================

inline auto drops_run_with_the_value_they_end(case_backend &backend) -> void {
  auto p = backend.open("bytecode_storage_test", "drops.cn");
  // `drop` divides by `self.id`, so a drop that runs with an id of zero
  // panics: that shows it ran, and ran on the right bytes.
  struct row {
    std::string_view name;
    int64_t arg;
    int64_t wanted;
  };
  const auto fine = std::vector<row>{
      {"scoped", 5, 1},     {"discarded", 5, 2}, {"replaced", 5, 3},
      {"moved_away", 5, 4}, {"receive", 5, 5},   {"in_loop", 1, 1},
  };
  for (const auto &entry : fine) {
    expect_equal(p->scalar(entry.name, std::vector<int64_t>{entry.arg}),
                 entry.wanted, entry.name);
  }
  for (const auto name :
       {"scoped", "discarded", "replaced", "moved_away", "receive"}) {
    const auto failure = p->panics(name, std::vector<int64_t>{0});
    expect(failure.has_value() &&
               *failure == bytecode::panic_reason::integer_divide_by_zero,
           "expected the value's drop to run and panic");
  }
  // Only the second iteration's value has an id of zero.
  const auto looped = p->panics("in_loop", std::vector<int64_t>{2});
  expect(looped.has_value() &&
             *looped == bytecode::panic_reason::integer_divide_by_zero,
         "expected a drop in each iteration of the loop");
}

/// A call that returns `never` ends its path, and a trailing `match` or `if`
/// is the function's value. The message is an empty `str` the host owns.
inline auto never_calls_and_tail_values(case_backend &backend) -> void {
  auto p = backend.open("hir_storage_test", "flow.cn");
  alignas(8) auto empty = std::array<uint64_t, 2>{0, 0};
  const auto message = static_cast<int64_t>(reinterpret_cast<uintptr_t>(&empty));
  expect_equal(p->scalar("guard", std::vector<int64_t>{4, message}), 8,
               "guard(4)");
  expect_equal(p->scalar("arm_dies", std::vector<int64_t>{3, message}), 3,
               "arm_dies(3)");
  expect_equal(p->scalar("tail_dies", std::vector<int64_t>{5, message}), 5,
               "tail_dies(5)");
  if (p->reports_explicit_panic()) {
    expect(p->panics("guard", std::vector<int64_t>{-1, message}).has_value(),
           "expected the never call to panic");
    expect(p->panics("arm_dies", std::vector<int64_t>{0, message}).has_value(),
           "expected the diverging arm to panic");
    expect(p->panics("tail_dies", std::vector<int64_t>{0, message}).has_value(),
           "expected the trailing never call to panic");
  }
  expect_equal(p->scalar("tail_match", std::vector<int64_t>{0}), 10,
               "tail_match(0)");
  expect_equal(p->scalar("tail_match", std::vector<int64_t>{1}), 20,
               "tail_match(1)");
  expect_equal(p->scalar("tail_match", std::vector<int64_t>{7}), 30,
               "tail_match(7)");
  const auto positive = p->into("tail_if", std::vector<int64_t>{6});
  expect_equal(positive.at<int32_t>(0), 6, "tail_if(6).a");
  expect_equal(positive.at<int32_t>(4), 1, "tail_if(6).b");
  const auto negative = p->into("tail_if", std::vector<int64_t>{-2});
  expect_equal(negative.at<int32_t>(0), 0, "tail_if(-2).a");
  expect_equal(negative.at<int32_t>(4), -2, "tail_if(-2).b");
}

/// Runs every case.
inline auto run_all(case_backend &backend) -> void {
  scalars_and_control_flow(backend);
  aggregate_results_are_built_in_the_caller_storage(backend);
  locals_assignment_and_borrows(backend);
  aggregate_arguments_travel_by_address(backend);
  deep_recursion_keeps_frame_storage_stable(backend);
  values_are_not_allocated_on_the_heap(backend);
  narrow_signed_values_are_extended_when_read(backend);
  packed_values_are_accessed_without_alignment(backend);
  drops_run_with_the_value_they_end(backend);
  never_calls_and_tail_values(backend);
}

} // namespace cinder::testing::storage_cases
