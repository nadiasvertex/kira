#include <cstdlib>
#include <exception>
#include <iostream>

#include "src/bytecode/chunk.h"
#include "src/bytecode/opcodes.h"
#include "src/bytecode/value.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;
namespace bc = kira::bytecode;

auto test_numeric_kind_of_maps_every_scalar() -> void {
  auto types = kira::semantic::type_table{};

  expect(bc::numeric_kind_of(types, types.builtin("bool")) ==
             bc::numeric_kind::boolean,
         "expected bool to map to numeric_kind::boolean");
  expect(bc::numeric_kind_of(types, types.builtin("char")) ==
             bc::numeric_kind::character,
         "expected char to map to numeric_kind::character");
  expect(bc::numeric_kind_of(types, types.builtin("int8")) ==
             bc::numeric_kind::i8,
         "expected int8 to map to numeric_kind::i8");
  expect(bc::numeric_kind_of(types, types.builtin("int32")) ==
             bc::numeric_kind::i32,
         "expected int32 to map to numeric_kind::i32");
  expect(bc::numeric_kind_of(types, types.builtin("int64")) ==
             bc::numeric_kind::i64,
         "expected int64 to map to numeric_kind::i64");
  expect(bc::numeric_kind_of(types, types.builtin("isize")) ==
             bc::numeric_kind::i64,
         "expected isize to alias numeric_kind::i64, same as int64");
  expect(bc::numeric_kind_of(types, types.builtin("uint32")) ==
             bc::numeric_kind::u32,
         "expected uint32 to map to numeric_kind::u32");
  expect(bc::numeric_kind_of(types, types.builtin("usize")) ==
             bc::numeric_kind::u64,
         "expected usize to alias numeric_kind::u64, same as uint64");
  expect(bc::numeric_kind_of(types, types.builtin("byte")) ==
             bc::numeric_kind::u8,
         "expected byte to alias numeric_kind::u8, same as uint8");
  expect(bc::numeric_kind_of(types, types.builtin("float32")) ==
             bc::numeric_kind::f32,
         "expected float32 to map to numeric_kind::f32");
  expect(bc::numeric_kind_of(types, types.builtin("float64")) ==
             bc::numeric_kind::f64,
         "expected float64 to map to numeric_kind::f64");
}

auto test_numeric_kind_of_rejects_unsupported_widths_and_non_scalars() -> void {
  auto types = kira::semantic::type_table{};

  expect(!bc::numeric_kind_of(types, types.builtin("int128")).has_value(),
         "expected int128 to be unsupported in this increment");
  expect(!bc::numeric_kind_of(types, types.builtin("uint128")).has_value(),
         "expected uint128 to be unsupported in this increment");
  expect(!bc::numeric_kind_of(types, types.builtin("float128")).has_value(),
         "expected float128 to be unsupported in this increment");
  expect(!bc::numeric_kind_of(types, types.builtin("str")).has_value(),
         "expected str (not a scalar numeric type) to have no numeric_kind");

  const auto list_of_int32 =
      types.builtin_generic("list", {types.builtin("int32")});
  expect(!bc::numeric_kind_of(types, list_of_int32).has_value(),
         "expected a builtin_generic_kind type to have no numeric_kind");
}

auto test_writer_encodes_load_const_and_move() -> void {
  auto writer = bc::chunk_writer{};
  const auto const_index = writer.add_constant(bc::slot_value{int64_t{42}});
  writer.emit_opcode(bc::opcode::op_load_const);
  writer.emit_u8(2); // dst register
  writer.emit_u16(const_index);
  writer.emit_opcode(bc::opcode::op_move);
  writer.emit_u8(3); // dst register
  writer.emit_u8(2); // src register

  auto function = std::move(writer).finish("f", 2, 5);

  expect(function.constants.size() == 1, "expected exactly one constant");
  expect(function.constants[0].i == 42, "expected the constant to be 42");
  expect(function.code.size() == 4 + 3,
         "expected op_load_const(4 bytes) + op_move(3 bytes)");
  expect(function.code[0] == static_cast<uint8_t>(bc::opcode::op_load_const),
         "expected the first opcode to be op_load_const");
  expect(function.code[1] == 2, "expected the dst register operand to be 2");
  expect(bc::read_u16(function.code, 2) == const_index,
         "expected the const_index operand to round-trip");
  expect(function.code[4] == static_cast<uint8_t>(bc::opcode::op_move),
         "expected the second opcode to be op_move");
  expect(function.code[5] == 3, "expected op_move's dst register to be 3");
  expect(function.code[6] == 2, "expected op_move's src register to be 2");
  expect(function.param_count == 2, "expected param_count to survive finish");
  expect(function.register_count == 5,
         "expected register_count to survive finish");
}

auto test_writer_encodes_register_arithmetic() -> void {
  // reg[0] = reg[1] + reg[2] (int32)
  auto writer = bc::chunk_writer{};
  writer.emit_opcode(bc::opcode::op_add);
  writer.emit_u8(0); // dst
  writer.emit_u8(1); // lhs
  writer.emit_u8(2); // rhs
  writer.emit_numeric_kind(bc::numeric_kind::i32);
  auto function = std::move(writer).finish("f", 0, 0);

  expect(function.code.size() == 5,
         "expected opcode + dst + lhs + rhs + numeric_kind");
  expect(function.code[1] == 0, "expected dst register 0");
  expect(function.code[2] == 1, "expected lhs register 1");
  expect(function.code[3] == 2, "expected rhs register 2");
  expect(function.code[4] == static_cast<uint8_t>(bc::numeric_kind::i32),
         "expected the numeric_kind byte to round-trip");
}

auto test_writer_patches_forward_jump() -> void {
  // Compiles the shape of `if reg[0]: op_move <skipped>  <target>` — a
  // forward jump over one instruction, the same pattern `if`/`while`
  // lowering will use, now testing a condition register instead of a
  // stack pop.
  auto writer = bc::chunk_writer{};
  writer.emit_opcode(bc::opcode::op_jump_if_false);
  writer.emit_u8(0); // condition register
  const auto placeholder = writer.emit_jump_placeholder();
  writer.emit_opcode(bc::opcode::op_move); // the "then" body, skipped if
  writer.emit_u8(1);                       // false
  writer.emit_u8(1);
  writer.patch_jump_to_here(placeholder);
  writer.emit_opcode(bc::opcode::op_move); // what the jump lands on
  writer.emit_u8(2);
  writer.emit_u8(2);

  auto function = std::move(writer).finish("f", 0, 0);

  // Layout: [0]=op_jump_if_false [1]=cond reg [2..6)=i32 offset
  //         [6]=op_move [7][8]=regs   [9]=op_move [10][11]=regs
  expect(function.code.size() == 12, "expected a 12-byte function body");
  const auto offset = bc::read_i32(function.code, 2);
  // Relative to the byte after the 4-byte operand (offset 6); the target
  // (offset 9, the second op_move) is 3 bytes further along.
  expect(offset == 3,
         "expected the patched jump offset to point past the skipped "
         "op_move instruction");
}

auto test_writer_encodes_backward_jump() -> void {
  // Compiles the shape of `while reg[0]: body` — a backward jump to the
  // loop condition check. Unlike the forward-jump case, the target
  // (loop_start) is already known when the jump is emitted, so this needs
  // no placeholder/patch round trip: the compiler computes the relative
  // offset immediately and emits it directly with `emit_i32`.
  auto writer = bc::chunk_writer{};
  const auto loop_start = writer.current_offset();
  writer.emit_opcode(bc::opcode::op_not_bool); // stand-in condition check
  writer.emit_u8(0);
  writer.emit_u8(0);
  writer.emit_opcode(bc::opcode::op_jump);
  const auto operand_offset = writer.current_offset();
  const auto after_operand = static_cast<int64_t>(operand_offset) + 4;
  const auto relative =
      static_cast<int32_t>(static_cast<int64_t>(loop_start) - after_operand);
  writer.emit_i32(relative);

  auto function = std::move(writer).finish("f", 0, 0);

  expect(function.code.size() == 8,
         "expected op_not_bool(3 bytes) + op_jump + a 4-byte operand");
  expect(bc::read_i32(function.code, 4) < 0,
         "expected a backward jump's offset to be negative");
  expect(bc::read_i32(function.code, 4) == relative,
         "expected the encoded backward-jump offset to round-trip exactly");
}

auto test_writer_encodes_call() -> void {
  // reg[0] = call function_index=7 with args starting at reg[1], argc=2
  auto writer = bc::chunk_writer{};
  writer.emit_opcode(bc::opcode::op_call);
  writer.emit_u8(0);  // dst
  writer.emit_u16(7); // function_index
  writer.emit_u8(1);  // first_arg_reg
  writer.emit_u8(2);  // argc
  auto function = std::move(writer).finish("f", 0, 0);

  expect(function.code.size() == 6,
         "expected opcode + dst + u16 function_index + first_arg_reg + argc");
  expect(function.code[1] == 0, "expected dst register 0");
  expect(bc::read_u16(function.code, 2) == 7,
         "expected the function_index operand to round-trip");
  expect(function.code[4] == 1, "expected first_arg_reg to be 1");
  expect(function.code[5] == 2, "expected argc to be 2");
}

} // namespace

auto main() -> int {
  try {
    test_numeric_kind_of_maps_every_scalar();
    test_numeric_kind_of_rejects_unsupported_widths_and_non_scalars();
    test_writer_encodes_load_const_and_move();
    test_writer_encodes_register_arithmetic();
    test_writer_patches_forward_jump();
    test_writer_encodes_backward_jump();
    test_writer_encodes_call();
  } catch (const std::exception &ex) {
    std::cerr << "chunk_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
