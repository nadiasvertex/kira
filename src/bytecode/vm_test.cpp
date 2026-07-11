#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "src/bytecode/chunk.h"
#include "src/bytecode/opcodes.h"
#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/bytecode/vm.h"
#include "src/intrinsics.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;
namespace bc = kira::bytecode;

// ---------------------------------------------------------------------------
// Hand-assembly helpers mirroring `bytecode_compiler::function_compiler`'s
// own `emit_alloc_slots`/`emit_load_slot`/`emit_store_slot` (uniform-8-byte-
// slot convenience wrappers around `op_alloc`'s byte-size operand and
// `op_load_slot`/`op_store_slot`'s byte-offset+field-size operands) — every
// hand-assembled test below builds "N slots" heap blocks, so it uses these
// rather than encoding the wire format directly.
// ---------------------------------------------------------------------------

auto emit_alloc_slots(bc::chunk_writer &writer, uint8_t dst,
                      uint16_t slot_count) -> void {
  writer.emit_opcode(bc::opcode::op_alloc);
  writer.emit_u8(dst);
  writer.emit_u16(static_cast<uint16_t>(slot_count * 8));
}

auto emit_load_slot(bc::chunk_writer &writer, uint8_t dst, uint8_t ptr,
                    uint16_t slot_index) -> void {
  writer.emit_opcode(bc::opcode::op_load_slot);
  writer.emit_u8(dst);
  writer.emit_u8(ptr);
  writer.emit_u16(static_cast<uint16_t>(slot_index * 8));
  writer.emit_u8(8);
}

auto emit_store_slot(bc::chunk_writer &writer, uint8_t ptr, uint16_t slot_index,
                     uint8_t src) -> void {
  writer.emit_opcode(bc::opcode::op_store_slot);
  writer.emit_u8(ptr);
  writer.emit_u16(static_cast<uint16_t>(slot_index * 8));
  writer.emit_u8(src);
  writer.emit_u8(8);
}

auto emit_load_indexed(bc::chunk_writer &writer, uint8_t dst, uint8_t ptr,
                       uint8_t index_reg, uint8_t elem_size = 8) -> void {
  writer.emit_opcode(bc::opcode::op_load_indexed);
  writer.emit_u8(dst);
  writer.emit_u8(ptr);
  writer.emit_u8(index_reg);
  writer.emit_u8(elem_size);
}

auto emit_store_indexed(bc::chunk_writer &writer, uint8_t ptr,
                        uint8_t index_reg, uint8_t src, uint8_t elem_size = 8)
    -> void {
  writer.emit_opcode(bc::opcode::op_store_indexed);
  writer.emit_u8(ptr);
  writer.emit_u8(index_reg);
  writer.emit_u8(src);
  writer.emit_u8(elem_size);
}

auto emit_list_push(bc::chunk_writer &writer, uint8_t header, uint8_t value,
                    uint8_t elem_size = 8) -> void {
  writer.emit_opcode(bc::opcode::op_list_push);
  writer.emit_u8(header);
  writer.emit_u8(value);
  writer.emit_u8(elem_size);
}

auto test_add_returns_sum() -> void {
  // fn(a: i32, b: i32) -> i32 { return a + b }
  auto writer = bc::chunk_writer{};
  writer.emit_opcode(bc::opcode::op_add);
  writer.emit_u8(2);
  writer.emit_u8(0);
  writer.emit_u8(1);
  writer.emit_numeric_kind(bc::numeric_kind::i32);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(2);
  auto function = std::move(writer).finish("add", 2, 3);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  const auto args =
      std::array{bc::slot_value{int64_t{10}}, bc::slot_value{int64_t{32}}};
  auto result = bc::vm{module}.run(0, args);

  expect(result.has_value(), "expected add() to succeed, not panic");
  expect(result->has_value, "expected add() to produce a value");
  expect(result->value.i == 42, "expected 10 + 32 == 42");
}

auto test_checked_add_panics_on_overflow() -> void {
  // fn(a: i8, b: i8) -> i8 { return a + b }, called with 100 + 100.
  auto writer = bc::chunk_writer{};
  writer.emit_opcode(bc::opcode::op_add);
  writer.emit_u8(2);
  writer.emit_u8(0);
  writer.emit_u8(1);
  writer.emit_numeric_kind(bc::numeric_kind::i8);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(2);
  auto function = std::move(writer).finish("add_i8", 2, 3);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  const auto args =
      std::array{bc::slot_value{int64_t{100}}, bc::slot_value{int64_t{100}}};
  auto result = bc::vm{module}.run(0, args);

  expect(!result.has_value(), "expected int8 100+100 to overflow and panic");
  expect(result.error() == bc::panic_reason::integer_overflow,
         "expected the panic reason to be integer_overflow");
}

auto test_checked_div_panics_on_divide_by_zero() -> void {
  auto writer = bc::chunk_writer{};
  writer.emit_opcode(bc::opcode::op_div);
  writer.emit_u8(2);
  writer.emit_u8(0);
  writer.emit_u8(1);
  writer.emit_numeric_kind(bc::numeric_kind::i32);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(2);
  auto function = std::move(writer).finish("div", 2, 3);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  const auto args =
      std::array{bc::slot_value{int64_t{10}}, bc::slot_value{int64_t{0}}};
  auto result = bc::vm{module}.run(0, args);

  expect(!result.has_value(), "expected division by zero to panic");
  expect(result.error() == bc::panic_reason::integer_divide_by_zero,
         "expected the panic reason to be integer_divide_by_zero");
}

auto test_wrapping_add_does_not_panic() -> void {
  // fn(a: u8, b: u8) -> u8 { return a +% b }, called with 200 +% 200.
  auto writer = bc::chunk_writer{};
  writer.emit_opcode(bc::opcode::op_add_wrap);
  writer.emit_u8(2);
  writer.emit_u8(0);
  writer.emit_u8(1);
  writer.emit_numeric_kind(bc::numeric_kind::u8);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(2);
  auto function = std::move(writer).finish("add_wrap_u8", 2, 3);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  const auto args =
      std::array{bc::slot_value{uint64_t{200}}, bc::slot_value{uint64_t{200}}};
  auto result = bc::vm{module}.run(0, args);

  expect(result.has_value(), "expected wrapping add never to panic");
  expect(result->value.u == 144,
         "expected (200 + 200) mod 256 == 144 for u8 wraparound");
}

auto test_saturating_add_clamps_to_max() -> void {
  auto writer = bc::chunk_writer{};
  writer.emit_opcode(bc::opcode::op_add_sat);
  writer.emit_u8(2);
  writer.emit_u8(0);
  writer.emit_u8(1);
  writer.emit_numeric_kind(bc::numeric_kind::u8);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(2);
  auto function = std::move(writer).finish("add_sat_u8", 2, 3);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  const auto args =
      std::array{bc::slot_value{uint64_t{200}}, bc::slot_value{uint64_t{200}}};
  auto result = bc::vm{module}.run(0, args);

  expect(result.has_value(), "expected saturating add never to panic");
  expect(result->value.u == 255,
         "expected 200 +| 200 to clamp to uint8::max (255)");
}

auto test_cast_sign_extends_negative_value() -> void {
  // fn() -> i64 { let x: i32 = -5; return x as i64 }
  auto writer = bc::chunk_writer{};
  const auto neg_five = writer.add_constant(bc::slot_value{int64_t{-5}});
  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(0);
  writer.emit_u16(neg_five);
  writer.emit_opcode(bc::opcode::op_cast);
  writer.emit_u8(1);
  writer.emit_u8(0);
  writer.emit_numeric_kind(bc::numeric_kind::i32);
  writer.emit_numeric_kind(bc::numeric_kind::i64);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(1);
  auto function = std::move(writer).finish("cast", 0, 2);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  auto result = bc::vm{module}.run(0, {});

  expect(result.has_value(), "expected the cast to succeed");
  expect(result->value.i == -5,
         "expected i32(-5) cast to i64 to sign-extend, staying -5");
}

auto test_while_loop_sums_one_to_n() -> void {
  // fn(n: i32) -> i32 {
  //   var sum = 0; var i = 1
  //   while i <= n { sum = sum + i; i = i + 1 }
  //   return sum
  // }
  // Registers: 0=n (param), 1=sum, 2=i, 3=cond, 4=one.
  auto writer = bc::chunk_writer{};
  const auto c_zero = writer.add_constant(bc::slot_value{int64_t{0}});
  const auto c_one = writer.add_constant(bc::slot_value{int64_t{1}});

  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(1);
  writer.emit_u16(c_zero);
  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(2);
  writer.emit_u16(c_one);
  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(4);
  writer.emit_u16(c_one);

  const auto loop_start = writer.current_offset();
  writer.emit_opcode(bc::opcode::op_le);
  writer.emit_u8(3);
  writer.emit_u8(2);
  writer.emit_u8(0);
  writer.emit_numeric_kind(bc::numeric_kind::i32);
  writer.emit_opcode(bc::opcode::op_jump_if_false);
  writer.emit_u8(3);
  const auto exit_placeholder = writer.emit_jump_placeholder();

  writer.emit_opcode(bc::opcode::op_add);
  writer.emit_u8(1);
  writer.emit_u8(1);
  writer.emit_u8(2);
  writer.emit_numeric_kind(bc::numeric_kind::i32);
  writer.emit_opcode(bc::opcode::op_add);
  writer.emit_u8(2);
  writer.emit_u8(2);
  writer.emit_u8(4);
  writer.emit_numeric_kind(bc::numeric_kind::i32);

  writer.emit_opcode(bc::opcode::op_jump);
  const auto after_operand = static_cast<int64_t>(writer.current_offset()) + 4;
  const auto back_offset =
      static_cast<int32_t>(static_cast<int64_t>(loop_start) - after_operand);
  writer.emit_i32(back_offset);

  writer.patch_jump_to_here(exit_placeholder);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(1);

  auto function = std::move(writer).finish("sum_to_n", 1, 5);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  const auto args = std::array{bc::slot_value{int64_t{10}}};
  auto result = bc::vm{module}.run(0, args);

  expect(result.has_value(), "expected the loop to complete without panicking");
  expect(result->value.i == 55, "expected sum(1..=10) == 55");
}

auto test_recursive_call_computes_factorial() -> void {
  // fn factorial(n: i32) -> i32 {
  //   if n <= 1 { return 1 }
  //   return n * factorial(n - 1)
  // }
  // Registers: 0=n (param), 1=cond, 2=one, 3=n_minus_1, 4=recursive_result,
  //            5=product.
  auto writer = bc::chunk_writer{};
  const auto c_one = writer.add_constant(bc::slot_value{int64_t{1}});

  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(2);
  writer.emit_u16(c_one);
  writer.emit_opcode(bc::opcode::op_le);
  writer.emit_u8(1);
  writer.emit_u8(0);
  writer.emit_u8(2);
  writer.emit_numeric_kind(bc::numeric_kind::i32);
  writer.emit_opcode(bc::opcode::op_jump_if_false);
  writer.emit_u8(1);
  const auto else_placeholder = writer.emit_jump_placeholder();

  // then: result = 1; jump to return.
  writer.emit_opcode(bc::opcode::op_move);
  writer.emit_u8(5);
  writer.emit_u8(2);
  writer.emit_opcode(bc::opcode::op_jump);
  const auto return_placeholder = writer.emit_jump_placeholder();

  // else: result = n * factorial(n - 1)
  writer.patch_jump_to_here(else_placeholder);
  writer.emit_opcode(bc::opcode::op_sub);
  writer.emit_u8(3);
  writer.emit_u8(0);
  writer.emit_u8(2);
  writer.emit_numeric_kind(bc::numeric_kind::i32);
  writer.emit_opcode(bc::opcode::op_call);
  writer.emit_u8(4);
  writer.emit_u16(0); // this function's own index — recursive self-call.
  writer.emit_u8(3);
  writer.emit_u8(1);
  writer.emit_opcode(bc::opcode::op_mul);
  writer.emit_u8(5);
  writer.emit_u8(0);
  writer.emit_u8(4);
  writer.emit_numeric_kind(bc::numeric_kind::i32);

  writer.patch_jump_to_here(return_placeholder);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(5);

  auto function = std::move(writer).finish("factorial", 1, 6);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  const auto vm = bc::vm{module};

  const auto args5 = std::array{bc::slot_value{int64_t{5}}};
  auto result5 = vm.run(0, args5);
  expect(result5.has_value(), "expected factorial(5) not to panic");
  expect(result5->value.i == 120, "expected factorial(5) == 120");

  const auto args0 = std::array{bc::slot_value{int64_t{0}}};
  auto result0 = vm.run(0, args0);
  expect(result0.has_value(), "expected factorial(0) not to panic");
  expect(result0->value.i == 1, "expected factorial(0) == 1");
}

auto test_unbounded_recursion_panics_with_stack_overflow() -> void {
  // fn rec(n: i32) -> i32 { return rec(n) } — never terminates on its own,
  // must be stopped by the VM's call-depth limit.
  auto writer = bc::chunk_writer{};
  writer.emit_opcode(bc::opcode::op_call);
  writer.emit_u8(1);
  writer.emit_u16(0);
  writer.emit_u8(0);
  writer.emit_u8(1);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(1);
  auto function = std::move(writer).finish("rec", 1, 2);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  const auto args = std::array{bc::slot_value{int64_t{0}}};
  auto result = bc::vm{module}.run(0, args);

  expect(!result.has_value(),
         "expected unbounded recursion to panic instead of exhausting memory");
  expect(result.error() == bc::panic_reason::stack_overflow,
         "expected the panic reason to be stack_overflow");
}

auto test_explicit_panic_opcode() -> void {
  auto writer = bc::chunk_writer{};
  writer.emit_opcode(bc::opcode::op_panic);
  auto function = std::move(writer).finish("boom", 0, 0);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  auto result = bc::vm{module}.run(0, {});

  expect(!result.has_value(), "expected op_panic to panic");
  expect(result.error() == bc::panic_reason::explicit_panic,
         "expected the panic reason to be explicit_panic");
}

auto test_alloc_and_slot_roundtrip_a_two_field_heap_block() -> void {
  // fn() -> i32 {
  //   let block = alloc(2 slots)
  //   store_slot(block, 0, 11)
  //   store_slot(block, 1, 31)
  //   return load_slot(block, 0) + load_slot(block, 1)
  // }
  auto writer = bc::chunk_writer{};
  const auto c11 = writer.add_constant(bc::slot_value{int64_t{11}});
  const auto c31 = writer.add_constant(bc::slot_value{int64_t{31}});

  emit_alloc_slots(writer, 0, 2);

  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(1);
  writer.emit_u16(c11);
  emit_store_slot(writer, 0, 0, 1);

  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(2);
  writer.emit_u16(c31);
  emit_store_slot(writer, 0, 1, 2);

  emit_load_slot(writer, 3, 0, 0);
  emit_load_slot(writer, 4, 0, 1);
  writer.emit_opcode(bc::opcode::op_add);
  writer.emit_u8(5);
  writer.emit_u8(3);
  writer.emit_u8(4);
  writer.emit_numeric_kind(bc::numeric_kind::i32);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(5);

  auto function = std::move(writer).finish("heap_roundtrip", 0, 6);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  auto result = bc::vm{module}.run(0, {});

  expect(result.has_value(), "expected the heap roundtrip not to panic");
  expect(result->value.i == 42, "expected 11 + 31 == 42 read back from slots");
}

auto test_packed_struct_fields_round_trip_without_clobbering_neighbors()
    -> void {
  // A packed `{ a: bool, b: int32, c: bool }`-shaped block: a@0 (1 byte),
  // b@1 (4 bytes), c@5 (1 byte), size 6 — verifies a narrow (1-byte)
  // `op_store_slot` doesn't clobber its neighbor's bytes the way a naive
  // "always write 8 bytes" store would, and that each field reads back at
  // its own width rather than the old uniform 8-byte-slot scheme.
  auto writer = bc::chunk_writer{};
  const auto c_true = writer.add_constant(bc::slot_value{uint64_t{1}});
  const auto c_mid = writer.add_constant(bc::slot_value{int64_t{0x11223344}});
  const auto c_false = writer.add_constant(bc::slot_value{uint64_t{0}});

  writer.emit_opcode(bc::opcode::op_alloc);
  writer.emit_u8(0);
  writer.emit_u16(6); // byte_size — packed layout, not slot count.

  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(1);
  writer.emit_u16(c_true);
  writer.emit_opcode(bc::opcode::op_store_slot);
  writer.emit_u8(0);
  writer.emit_u16(0);
  writer.emit_u8(1);
  writer.emit_u8(1); // field_size = 1 (bool)

  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(2);
  writer.emit_u16(c_mid);
  writer.emit_opcode(bc::opcode::op_store_slot);
  writer.emit_u8(0);
  writer.emit_u16(1);
  writer.emit_u8(2);
  writer.emit_u8(4); // field_size = 4 (int32)

  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(3);
  writer.emit_u16(c_false);
  writer.emit_opcode(bc::opcode::op_store_slot);
  writer.emit_u8(0);
  writer.emit_u16(5);
  writer.emit_u8(3);
  writer.emit_u8(1); // field_size = 1 (bool)

  writer.emit_opcode(bc::opcode::op_load_slot);
  writer.emit_u8(4);
  writer.emit_u8(0);
  writer.emit_u16(0);
  writer.emit_u8(1);
  writer.emit_opcode(bc::opcode::op_load_slot);
  writer.emit_u8(5);
  writer.emit_u8(0);
  writer.emit_u16(1);
  writer.emit_u8(4);
  writer.emit_opcode(bc::opcode::op_load_slot);
  writer.emit_u8(6);
  writer.emit_u8(0);
  writer.emit_u16(5);
  writer.emit_u8(1);

  // Pack a@1-bit / b / c into one return value: a*1000000 + b + c.
  const auto million = writer.add_constant(bc::slot_value{int64_t{1000000}});
  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(7);
  writer.emit_u16(million);
  writer.emit_opcode(bc::opcode::op_mul);
  writer.emit_u8(8);
  writer.emit_u8(4);
  writer.emit_u8(7);
  writer.emit_numeric_kind(bc::numeric_kind::i64);
  writer.emit_opcode(bc::opcode::op_add);
  writer.emit_u8(9);
  writer.emit_u8(8);
  writer.emit_u8(5);
  writer.emit_numeric_kind(bc::numeric_kind::i64);
  writer.emit_opcode(bc::opcode::op_add);
  writer.emit_u8(10);
  writer.emit_u8(9);
  writer.emit_u8(6);
  writer.emit_numeric_kind(bc::numeric_kind::i64);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(10);

  auto function = std::move(writer).finish("packed_struct", 0, 11);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  auto result = bc::vm{module}.run(0, {});

  expect(result.has_value(),
         "expected the packed-struct roundtrip not to panic");
  expect(result->value.i == 1000000 + 0x11223344 + 0,
         "expected each narrow field to round-trip without clobbering its "
         "neighbors");
}

auto test_narrow_element_array_indexed_load_store_round_trip() -> void {
  // A 5-element `array[int16, 5]`-shaped block (2 bytes/element, 10 bytes
  // total) — verifies `op_load_indexed`/`op_store_indexed`'s generalized
  // `elem_size` operand keeps elements tightly packed at their own natural
  // width instead of the old hardcoded 8-byte stride.
  auto w = bc::chunk_writer{};
  w.emit_opcode(bc::opcode::op_alloc);
  w.emit_u8(0);
  w.emit_u16(10); // 5 elements * 2 bytes.

  const auto c1 = w.add_constant(bc::slot_value{int64_t{111}});
  const auto c2 = w.add_constant(bc::slot_value{int64_t{222}});
  w.emit_opcode(bc::opcode::op_load_const);
  w.emit_u8(1);
  w.emit_u16(c1);
  const auto zero_idx = w.add_constant(bc::slot_value{uint64_t{0}});
  w.emit_opcode(bc::opcode::op_load_const);
  w.emit_u8(2);
  w.emit_u16(zero_idx);
  emit_store_indexed(w, 0, 2, 1, 2); // block[0] = 111, elem_size 2.

  w.emit_opcode(bc::opcode::op_load_const);
  w.emit_u8(3);
  w.emit_u16(c2);
  const auto four_idx = w.add_constant(bc::slot_value{uint64_t{4}});
  w.emit_opcode(bc::opcode::op_load_const);
  w.emit_u8(4);
  w.emit_u16(four_idx);
  emit_store_indexed(w, 0, 4, 3, 2); // block[4] = 222, elem_size 2.

  emit_load_indexed(w, 5, 0, 2, 2);
  emit_load_indexed(w, 6, 0, 4, 2);
  w.emit_opcode(bc::opcode::op_add);
  w.emit_u8(7);
  w.emit_u8(5);
  w.emit_u8(6);
  w.emit_numeric_kind(bc::numeric_kind::i64);
  w.emit_opcode(bc::opcode::op_return_value);
  w.emit_u8(7);

  auto function = std::move(w).finish("narrow_array", 0, 8);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  auto result = bc::vm{module}.run(0, {});

  expect(result.has_value(),
         "expected the narrow-element array test not to panic");
  expect(result->value.i == 333,
         "expected both 2-byte elements to round-trip at index 0 and 4 "
         "(byte offsets 0 and 8) without overlapping");
}

auto test_list_push_narrow_element_grows_and_stays_tightly_packed() -> void {
  // A `list[int16]`-shaped push sequence (3-slot header, `op_list_push`'s
  // generalized `elem_size` operand) — verifies pushed 2-byte elements
  // don't waste the old hardcoded 8-bytes/element `data` block stride.
  auto w = bc::chunk_writer{};
  emit_alloc_slots(w, 0, 3); // list header: { len; cap; data }.

  const auto c10 = w.add_constant(bc::slot_value{int64_t{10}});
  const auto c20 = w.add_constant(bc::slot_value{int64_t{20}});
  w.emit_opcode(bc::opcode::op_load_const);
  w.emit_u8(1);
  w.emit_u16(c10);
  emit_list_push(w, 0, 1, 2);
  w.emit_opcode(bc::opcode::op_load_const);
  w.emit_u8(2);
  w.emit_u16(c20);
  emit_list_push(w, 0, 2, 2);

  emit_load_slot(w, 3, 0, 2); // data pointer (slot 2 of the header).
  const auto zero_idx = w.add_constant(bc::slot_value{uint64_t{0}});
  w.emit_opcode(bc::opcode::op_load_const);
  w.emit_u8(5);
  w.emit_u16(zero_idx);
  emit_load_indexed(w, 6, 3, 5, 2);
  const auto one_idx = w.add_constant(bc::slot_value{uint64_t{1}});
  w.emit_opcode(bc::opcode::op_load_const);
  w.emit_u8(7);
  w.emit_u16(one_idx);
  emit_load_indexed(w, 8, 3, 7, 2);
  w.emit_opcode(bc::opcode::op_add);
  w.emit_u8(9);
  w.emit_u8(6);
  w.emit_u8(8);
  w.emit_numeric_kind(bc::numeric_kind::i64);
  w.emit_opcode(bc::opcode::op_return_value);
  w.emit_u8(9);

  auto function = std::move(w).finish("narrow_list_push", 0, 10);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  auto result = bc::vm{module}.run(0, {});

  expect(result.has_value(), "expected the narrow list_push test not to panic");
  expect(result->value.i == 30,
         "expected both pushed 2-byte elements to read back correctly at "
         "their tightly-packed offsets");
}

auto test_load_str_const_produces_len_and_data_slots() -> void {
  // fn() -> u64 { return len("hi!") } — reads the heap `str` value's own
  // length slot (slot 0) directly, since increment 2 hasn't wired up a
  // surface-syntax `.len()` yet.
  auto writer = bc::chunk_writer{};
  const auto idx = writer.add_string_constant("hi!");
  writer.emit_opcode(bc::opcode::op_load_str_const);
  writer.emit_u8(0);
  writer.emit_u16(idx);
  emit_load_slot(writer, 1, 0, 0);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(1);

  auto function = std::move(writer).finish("str_len", 0, 2);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  auto result = bc::vm{module}.run(0, {});

  expect(result.has_value(), "expected loading a string constant not to panic");
  expect(result->value.u == 3, "expected \"hi!\" to report length 3");
}

auto test_return_unit_produces_no_value() -> void {
  auto writer = bc::chunk_writer{};
  writer.emit_opcode(bc::opcode::op_return_unit);
  auto function = std::move(writer).finish("unit_fn", 0, 0);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  auto result = bc::vm{module}.run(0, {});

  expect(result.has_value(), "expected op_return_unit not to panic");
  expect(!result->has_value,
         "expected op_return_unit's result to carry no value");
}

// ---------------------------------------------------------------------------
// Intrinsics (op_call_intrinsic, src/intrinsics.h) — see vm.cpp's own doc
// comment on the "Intrinsics" section for the heap conventions these tests
// hand-assemble against (a struct is N slots in field-declaration order; a
// `str`/slice value is a 2-slot `{ len; data_ptr }` header).
// ---------------------------------------------------------------------------

auto intrinsic_id(std::string_view name) -> uint8_t {
  const auto id = kira::intrinsic_index_of(name);
  expect(id.has_value(), "expected a recognized intrinsic name");
  return id.value();
}

auto test_intrinsic_rt_stdout_returns_fd_one() -> void {
  // fn() -> int64 { let fd = rt_stdout(); return fd.value }
  auto writer = bc::chunk_writer{};
  writer.emit_opcode(bc::opcode::op_call_intrinsic);
  writer.emit_u8(0); // dst
  writer.emit_u8(intrinsic_id("rt_stdout"));
  writer.emit_u8(0); // first_arg_reg (unused — argc is 0)
  writer.emit_u8(0); // argc
  emit_load_slot(writer, 1, 0, 0);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(1);
  auto function = std::move(writer).finish("stdout_fd", 0, 2);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  auto result = bc::vm{module}.run(0, {});
  expect(result.has_value(), "expected rt_stdout() to succeed");
  expect(result->value.i == 1, "expected rt_stdout()'s raw_fd.value to be 1");
}

auto test_intrinsic_rt_write_and_rt_read_round_trip_through_a_pipe() -> void {
  // fn(write_fd, read_fd, len, write_ptr, read_ptr) -> i64 {
  //   let w = rt_write({value: write_fd}, {len; write_ptr})   // result[usize,
  //   io_errno] let r = rt_read({value: read_fd}, {len; read_ptr})       //
  //   result[usize, io_errno] return w.tag * 1000000 + r.tag * 10000 +
  //   r.payload
  // }
  // Encodes both results' tags plus the byte count into one return value so
  // this test can confirm *both* intrinsics returned `@ok(...)` (tag 0) —
  // not just that some payload came back — without needing branchy
  // unwrap-or-panic bytecode; the real `@ok`/`@err` unwrap path is exercised
  // through real Kira `match` syntax in
  // src/bytecode_compiler/compile_test.cpp instead.
  int fds[2] = {-1, -1};
  expect(::pipe(fds) == 0, "expected pipe() to succeed");

  char write_buf[2] = {'h', 'i'};
  char read_buf[4] = {0, 0, 0, 0};

  auto writer = bc::chunk_writer{};
  // r5 = { value: r0 } — write_fd as a raw_fd struct.
  emit_alloc_slots(writer, 5, 1);
  emit_store_slot(writer, 5, 0, 0);
  // r6 = { len: r2, data_ptr: r3 } — the write buffer's slice.
  emit_alloc_slots(writer, 6, 2);
  emit_store_slot(writer, 6, 0, 2);
  emit_store_slot(writer, 6, 1, 3);
  // r7 = rt_write(r5, r6)
  writer.emit_opcode(bc::opcode::op_call_intrinsic);
  writer.emit_u8(7);
  writer.emit_u8(intrinsic_id("rt_write"));
  writer.emit_u8(5);
  writer.emit_u8(2);
  // r8 = { value: r1 } — read_fd as a raw_fd struct.
  emit_alloc_slots(writer, 8, 1);
  emit_store_slot(writer, 8, 0, 1);
  // r9 = { len: r2, data_ptr: r4 } — the read buffer's slice.
  emit_alloc_slots(writer, 9, 2);
  emit_store_slot(writer, 9, 0, 2);
  emit_store_slot(writer, 9, 1, 4);
  // r10 = rt_read(r8, r9)
  writer.emit_opcode(bc::opcode::op_call_intrinsic);
  writer.emit_u8(10);
  writer.emit_u8(intrinsic_id("rt_read"));
  writer.emit_u8(8);
  writer.emit_u8(2);
  // r11 = w.tag (r7[0]), r12 = r.tag (r10[0]), r13 = r.payload (r10[1])
  emit_load_slot(writer, 11, 7, 0);
  emit_load_slot(writer, 12, 10, 0);
  emit_load_slot(writer, 13, 10, 1);
  // r14 = 1000000, r15 = 10000
  const auto million_idx =
      writer.add_constant(bc::slot_value{int64_t{1000000}});
  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(14);
  writer.emit_u16(million_idx);
  const auto ten_k_idx = writer.add_constant(bc::slot_value{int64_t{10000}});
  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(15);
  writer.emit_u16(ten_k_idx);
  // r16 = w.tag * 1000000, r17 = r.tag * 10000
  writer.emit_opcode(bc::opcode::op_mul);
  writer.emit_u8(16);
  writer.emit_u8(11);
  writer.emit_u8(14);
  writer.emit_numeric_kind(bc::numeric_kind::i64);
  writer.emit_opcode(bc::opcode::op_mul);
  writer.emit_u8(17);
  writer.emit_u8(12);
  writer.emit_u8(15);
  writer.emit_numeric_kind(bc::numeric_kind::i64);
  // r18 = r16 + r17, r19 = r18 + r.payload
  writer.emit_opcode(bc::opcode::op_add);
  writer.emit_u8(18);
  writer.emit_u8(16);
  writer.emit_u8(17);
  writer.emit_numeric_kind(bc::numeric_kind::i64);
  writer.emit_opcode(bc::opcode::op_add);
  writer.emit_u8(19);
  writer.emit_u8(18);
  writer.emit_u8(13);
  writer.emit_numeric_kind(bc::numeric_kind::i64);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(19);
  auto function = std::move(writer).finish("pipe_roundtrip", 5, 20);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  const auto args = std::array{
      bc::slot_value{int64_t{fds[1]}}, // r0: write fd
      bc::slot_value{int64_t{fds[0]}}, // r1: read fd
      bc::slot_value{uint64_t{2}},     // r2: length
      bc::slot_value{static_cast<uint64_t>(
          reinterpret_cast<uintptr_t>(&write_buf[0]))}, // r3
      bc::slot_value{static_cast<uint64_t>(
          reinterpret_cast<uintptr_t>(&read_buf[0]))}, // r4
  };
  auto result = bc::vm{module}.run(0, args);

  ::close(fds[0]);
  ::close(fds[1]);

  expect(result.has_value(), "expected the rt_write/rt_read round trip to "
                             "succeed against a real pipe");
  expect(result->value.i == 2,
         "expected both rt_write() and rt_read() to return `@ok(...)` (tag "
         "0) and rt_read() to read exactly 2 bytes — encoded as "
         "write_tag*1e6 + read_tag*1e4 + read_payload");
  expect(read_buf[0] == 'h' && read_buf[1] == 'i',
         "expected the bytes rt_write() wrote to be the bytes rt_read() "
         "read back");
}

auto test_intrinsic_rt_close_returns_ok_on_a_valid_fd() -> void {
  const int duped = ::dup(0);
  expect(duped >= 0, "expected dup(0) to produce a closable fd");

  auto writer = bc::chunk_writer{};
  emit_alloc_slots(writer, 1, 1);
  emit_store_slot(writer, 1, 0, 0);
  writer.emit_opcode(bc::opcode::op_call_intrinsic);
  writer.emit_u8(2);
  writer.emit_u8(intrinsic_id("rt_close"));
  writer.emit_u8(1);
  writer.emit_u8(1);
  // dst(3) = close_result.tag
  emit_load_slot(writer, 3, 2, 0);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(3);
  auto function = std::move(writer).finish("close_fd", 1, 4);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  const auto args = std::array{bc::slot_value{int64_t{duped}}};
  auto result = bc::vm{module}.run(0, args);
  expect(result.has_value(), "expected rt_close() not to panic");
  expect(result->value.i == 0,
         "expected rt_close() on a valid fd to return `@ok(...)` (tag 0)");
}

auto test_intrinsic_rt_open_returns_err_on_a_missing_file() -> void {
  // fn(path_len, path_ptr, read, write, append, create, truncate) -> i64 {
  //   let r = rt_open({len; path_ptr}, {read; write; append; create;
  //                    truncate})           // result[raw_fd, io_errno]
  //   return r.tag * 1000000 + r.payload.code
  // }
  const std::string path = "/definitely/does/not/exist/kira-vm-test.kira";

  auto writer = bc::chunk_writer{};
  // r7 = { len: r0, data_ptr: r1 } — the path string.
  emit_alloc_slots(writer, 7, 2);
  emit_store_slot(writer, 7, 0, 0);
  emit_store_slot(writer, 7, 1, 1);
  // r8 = { read: r2, write: r3, append: r4, create: r5, truncate: r6 }
  emit_alloc_slots(writer, 8, 5);
  for (uint16_t i = 0; i < 5; ++i) {
    emit_store_slot(writer, 8, i, static_cast<uint8_t>(2 + i));
  }
  // r9 = rt_open(r7, r8)
  writer.emit_opcode(bc::opcode::op_call_intrinsic);
  writer.emit_u8(9);
  writer.emit_u8(intrinsic_id("rt_open"));
  writer.emit_u8(7);
  writer.emit_u8(2);
  // r10 = r9.tag, r11 = r9.payload (an `io_errno` struct pointer for @err)
  emit_load_slot(writer, 10, 9, 0);
  emit_load_slot(writer, 11, 9, 1);
  // r12 = r11.code
  emit_load_slot(writer, 12, 11, 0);
  // r13 = 1000000, r14 = r10 * r13, r15 = r14 + r12
  const auto million_idx =
      writer.add_constant(bc::slot_value{int64_t{1000000}});
  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(13);
  writer.emit_u16(million_idx);
  writer.emit_opcode(bc::opcode::op_mul);
  writer.emit_u8(14);
  writer.emit_u8(10);
  writer.emit_u8(13);
  writer.emit_numeric_kind(bc::numeric_kind::i64);
  writer.emit_opcode(bc::opcode::op_add);
  writer.emit_u8(15);
  writer.emit_u8(14);
  writer.emit_u8(12);
  writer.emit_numeric_kind(bc::numeric_kind::i64);
  writer.emit_opcode(bc::opcode::op_return_value);
  writer.emit_u8(15);
  auto function = std::move(writer).finish("open_missing", 7, 16);

  auto module = bc::bytecode_module{.module_name = "m", .functions = {}};
  module.functions.push_back(std::move(function));

  const auto args = std::array{
      bc::slot_value{uint64_t{path.size()}},
      bc::slot_value{
          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(path.data()))},
      bc::slot_value{uint64_t{1}}, // read
      bc::slot_value{uint64_t{0}}, // write
      bc::slot_value{uint64_t{0}}, // append
      bc::slot_value{uint64_t{0}}, // create
      bc::slot_value{uint64_t{0}}, // truncate
  };
  auto result = bc::vm{module}.run(0, args);

  expect(result.has_value(),
         "expected rt_open() on a missing file not to panic, only to "
         "return `@err(...)`");
  expect(result->value.i == 1000000 + ENOENT,
         "expected rt_open() to return `@err({ code: ENOENT })` (tag 1) for "
         "a nonexistent, non-`create` path");
}

} // namespace

auto main() -> int {
  try {
    test_add_returns_sum();
    test_checked_add_panics_on_overflow();
    test_checked_div_panics_on_divide_by_zero();
    test_wrapping_add_does_not_panic();
    test_saturating_add_clamps_to_max();
    test_cast_sign_extends_negative_value();
    test_while_loop_sums_one_to_n();
    test_recursive_call_computes_factorial();
    test_unbounded_recursion_panics_with_stack_overflow();
    test_explicit_panic_opcode();
    test_alloc_and_slot_roundtrip_a_two_field_heap_block();
    test_packed_struct_fields_round_trip_without_clobbering_neighbors();
    test_narrow_element_array_indexed_load_store_round_trip();
    test_list_push_narrow_element_grows_and_stays_tightly_packed();
    test_load_str_const_produces_len_and_data_slots();
    test_return_unit_produces_no_value();
    test_intrinsic_rt_stdout_returns_fd_one();
    test_intrinsic_rt_write_and_rt_read_round_trip_through_a_pipe();
    test_intrinsic_rt_close_returns_ok_on_a_valid_fd();
    test_intrinsic_rt_open_returns_err_on_a_missing_file();
  } catch (const std::exception &ex) {
    std::cerr << "vm_test failed: unhandled exception: " << ex.what() << '\n';
    std::exit(1);
  }
  return 0;
}
