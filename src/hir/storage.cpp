#include "src/hir/storage.h"

#include <string>
#include <string_view>

namespace cinder::hir {

auto storage_class_name(storage_class cls) -> const char * {
  switch (cls) {
  case storage_class::frame_local:
    return "local";
  case storage_class::parameter:
    return "param";
  case storage_class::caller_result:
    return "result";
  case storage_class::outgoing_argument:
    return "outgoing";
  case storage_class::global:
    return "global";
  case storage_class::owner_field:
    return "owner_field";
  case storage_class::closure_environment:
    return "closure_env";
  case storage_class::coroutine_frame:
    return "coroutine";
  }
  return "?";
}

auto op_kind_name(op_kind kind) -> const char * {
  switch (kind) {
  case op_kind::compute:
    return "compute";
  case op_kind::init_empty:
    return "init_empty";
  case op_kind::copy_into:
    return "copy";
  case op_kind::move_into:
    return "move";
  case op_kind::publish_variant:
    return "publish";
  case op_kind::drop_in_place:
    return "drop";
  case op_kind::call_into:
    return "call";
  case op_kind::storage_dead:
    return "dead";
  case op_kind::return_value:
    return "return";
  case op_kind::if_begin:
    return "if";
  case op_kind::else_begin:
    return "else";
  case op_kind::if_end:
    return "end_if";
  case op_kind::loop_begin:
    return "loop";
  case op_kind::loop_end:
    return "end_loop";
  case op_kind::block_begin:
    return "block";
  case op_kind::block_end:
    return "end_block";
  case op_kind::exit:
    return "exit";
  }
  return "?";
}

auto storage_env::is_copy(semantic::type_id type) const -> bool {
  return checked.is_copy(type);
}

auto storage_env::drop_plan(semantic::type_id type) const
    -> const semantic::drop_plan * {
  const auto found = checked.drop.drop_plans.find(checked.types.strip_refinement(type));
  return found == checked.drop.drop_plans.end() ? nullptr : &found->second;
}

auto storage_env::needs_drop(semantic::type_id type) -> bool {
  const auto stripped = checked.types.strip_refinement(type);
  if (checked.drop.drop_plans.contains(stripped)) {
    return true;
  }
  if (checked.types.entry(stripped).kind == semantic::type_kind::fn_kind) {
    return true;
  }
  const auto layout = layouts.layout(stripped);
  if (!layout.has_value()) {
    return false;
  }
  const auto &found = **layout;
  switch (found.kind) {
  case runtime::value_layout_kind::tuple:
  case runtime::value_layout_kind::structure:
    for (const auto &member : found.members) {
      if (needs_drop(member.type)) {
        return true;
      }
    }
    return false;
  case runtime::value_layout_kind::array:
    return needs_drop(found.element);
  default:
    return false;
  }
}

auto storage_module::find(std::string_view function_name) const
    -> const storage_function * {
  for (const auto &function : functions) {
    if (function.name == function_name) {
      return &function;
    }
  }
  return nullptr;
}

namespace {

[[nodiscard]] auto render_place(const place &where) -> std::string {
  auto out = "s" + std::to_string(where.base);
  for (const auto &step : where.steps) {
    switch (step.kind) {
    case projection_kind::member:
      out += step.name.empty() ? "." + std::to_string(step.index)
                               : "." + step.name;
      break;
    case projection_kind::element:
      out += "[" + std::to_string(step.index) + "]";
      break;
    case projection_kind::payload:
      out += ".@" + step.name + "." + std::to_string(step.index);
      break;
    case projection_kind::deref:
      out += ".*";
      break;
    }
  }
  return out;
}

[[nodiscard]] auto render_operand(const operand &value) -> std::string {
  switch (value.kind) {
  case operand_kind::read:
    return (value.consume ? "move " : "") + render_place(value.where);
  case operand_kind::address_of:
    return "&" + render_place(value.where);
  case operand_kind::constant:
    return "#" + value.text;
  }
  return "?";
}

[[nodiscard]] auto render_compute(const storage_op &op) -> std::string {
  switch (op.computation) {
  case compute_kind::constant:
    return "const " + op.text;
  case compute_kind::copy:
    return "copy " + render_operand(op.operands.at(0));
  case compute_kind::binary:
    return std::string{"binary "} + std::string{ast::binary_op_name(op.binary)} +
           " " + render_operand(op.operands.at(0)) + ", " +
           render_operand(op.operands.at(1));
  case compute_kind::unary:
    return std::string{"unary "} + std::string{ast::unary_op_name(op.unary)} +
           " " + render_operand(op.operands.at(0));
  case compute_kind::cast:
    return "cast " + render_operand(op.operands.at(0));
  case compute_kind::pointer_offset:
    return "offset " + render_operand(op.operands.at(0)) + ", " +
           render_operand(op.operands.at(1)) + " * " + std::to_string(op.stride);
  }
  return "?";
}

[[nodiscard]] auto render_call(const storage_op &op) -> std::string {
  auto out = render_place(op.dest) + " = ";
  out += op.callee.is_direct() ? op.callee.name
                               : "(" + render_place(*op.callee.callable) + ")";
  out += "(";
  auto first = true;
  for (const auto &arg : op.args) {
    if (!first) {
      out += ", ";
    }
    first = false;
    if (arg.cls == pass_class::zero_sized) {
      out += "_";
    } else if (is_indirect(arg.cls)) {
      out += "move " + render_place(arg.slot);
    } else {
      out += render_operand(arg.value);
    }
  }
  return out + ")";
}

[[nodiscard]] auto render_op(const storage_op &op) -> std::string {
  switch (op.kind) {
  case op_kind::compute:
    return "compute " + render_place(op.dest) + " = " + render_compute(op);
  case op_kind::init_empty:
    return "init_empty " + render_place(op.dest);
  case op_kind::copy_into:
    return "copy " + render_place(op.dest) + " <- " + render_place(op.source);
  case op_kind::move_into:
    return "move " + render_place(op.dest) + " <- " + render_place(op.source);
  case op_kind::publish_variant:
    return "publish " + render_place(op.dest) + " @" + op.variant;
  case op_kind::drop_in_place:
    return "drop " + render_place(op.dest);
  case op_kind::call_into:
    return std::string{op.is_drop ? "call.drop " : "call "} + render_call(op);
  case op_kind::storage_dead:
    return "dead s" + std::to_string(op.slot);
  case op_kind::return_value:
    return op.value.has_value() ? "return " + render_operand(*op.value)
                                : std::string{"return"};
  case op_kind::if_begin:
    return "if " + render_operand(op.condition);
  case op_kind::else_begin:
    return "else";
  case op_kind::if_end:
    return "end_if";
  case op_kind::loop_begin:
    return "loop";
  case op_kind::loop_end:
    return "end_loop";
  case op_kind::block_begin:
    return "block";
  case op_kind::block_end:
    return "end_block";
  case op_kind::exit:
    return "exit " + std::to_string(op.depth);
  }
  return "?";
}

} // namespace

auto dump(const storage_function &function, const semantic::type_table &types)
    -> std::string {
  auto out = "fn " + function.name + " " + format_signature(function.signature) +
             "\n";
  for (const auto &slot : function.slots) {
    out += "  s" + std::to_string(slot.id) + " " +
           storage_class_name(slot.cls) + " " +
           (slot.name.empty() ? std::string{"_"} : slot.name) + ": " +
           types.display(slot.type) +
           (slot.initially_initialized ? " (initialized)" : "") + "\n";
  }
  for (size_t i = 0; i < function.ops.size(); ++i) {
    out += "  " + std::to_string(i) + ": " + render_op(function.ops[i]) + "\n";
  }
  return out;
}

auto dump(const storage_module &module, const semantic::type_table &types)
    -> std::string {
  auto out = "module " + module.name + "\n";
  for (const auto &function : module.functions) {
    out += dump(function, types);
  }
  return out;
}

} // namespace cinder::hir

