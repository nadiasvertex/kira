#include "src/bytecode/chunk.h"

#include <utility>

namespace kira::bytecode {

auto read_u16(const std::vector<uint8_t> &code, size_t offset) -> uint16_t {
  return static_cast<uint16_t>(static_cast<uint16_t>(code[offset]) |
                               static_cast<uint16_t>(code[offset + 1] << 8));
}

auto read_u32(const std::vector<uint8_t> &code, size_t offset) -> uint32_t {
  return static_cast<uint32_t>(code[offset]) |
         (static_cast<uint32_t>(code[offset + 1]) << 8) |
         (static_cast<uint32_t>(code[offset + 2]) << 16) |
         (static_cast<uint32_t>(code[offset + 3]) << 24);
}

auto read_i32(const std::vector<uint8_t> &code, size_t offset) -> int32_t {
  return static_cast<int32_t>(read_u32(code, offset));
}

auto chunk_writer::emit_opcode(opcode op) -> void {
  code_.push_back(static_cast<uint8_t>(op));
}

auto chunk_writer::emit_u8(uint8_t value) -> void { code_.push_back(value); }

auto chunk_writer::emit_u16(uint16_t value) -> void {
  code_.push_back(static_cast<uint8_t>(value & 0xFFU));
  code_.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
}

auto chunk_writer::emit_i32(int32_t value) -> void {
  const auto bits = static_cast<uint32_t>(value);
  code_.push_back(static_cast<uint8_t>(bits & 0xFFU));
  code_.push_back(static_cast<uint8_t>((bits >> 8) & 0xFFU));
  code_.push_back(static_cast<uint8_t>((bits >> 16) & 0xFFU));
  code_.push_back(static_cast<uint8_t>((bits >> 24) & 0xFFU));
}

auto chunk_writer::emit_numeric_kind(numeric_kind kind) -> void {
  emit_u8(static_cast<uint8_t>(kind));
}

auto chunk_writer::emit_jump_placeholder() -> size_t {
  const auto offset = code_.size();
  emit_i32(0);
  return offset;
}

auto chunk_writer::patch_jump_to_here(size_t placeholder_offset) -> void {
  const auto target = static_cast<int64_t>(code_.size());
  const auto after_operand = static_cast<int64_t>(placeholder_offset) + 4;
  const auto relative = static_cast<int32_t>(target - after_operand);
  const auto bits = static_cast<uint32_t>(relative);
  code_[placeholder_offset] = static_cast<uint8_t>(bits & 0xFFU);
  code_[placeholder_offset + 1] = static_cast<uint8_t>((bits >> 8) & 0xFFU);
  code_[placeholder_offset + 2] = static_cast<uint8_t>((bits >> 16) & 0xFFU);
  code_[placeholder_offset + 3] = static_cast<uint8_t>((bits >> 24) & 0xFFU);
}

auto chunk_writer::add_constant(slot_value value) -> uint16_t {
  // Not guarded against exceeding 65536 constants — see the doc comment on
  // `chunk_writer` in chunk.h; not a real concern for the scalar/control-
  // flow subset this increment covers.
  constants_.push_back(value);
  return static_cast<uint16_t>(constants_.size() - 1);
}

auto chunk_writer::add_string_constant(std::string text) -> uint16_t {
  string_constants_.push_back(std::move(text));
  return static_cast<uint16_t>(string_constants_.size() - 1);
}

auto chunk_writer::finish(std::string name, uint16_t param_count,
                          uint16_t register_count) && -> bytecode_function {
  return bytecode_function{.name = std::move(name),
                           .param_count = param_count,
                           .register_count = register_count,
                           .code = std::move(code_),
                           .constants = std::move(constants_),
                           .string_constants = std::move(string_constants_)};
}

} // namespace kira::bytecode
