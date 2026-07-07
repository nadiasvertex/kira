#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <vector>

#include "src/bytecode/chunk.h"
#include "src/bytecode/opcodes.h"
#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/bytecode/vm.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;
namespace bc = kira::bytecode;

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
    test_return_unit_produces_no_value();
  } catch (const std::exception &ex) {
    std::cerr << "vm_test failed: unhandled exception: " << ex.what() << '\n';
    std::exit(1);
  }
  return 0;
}
