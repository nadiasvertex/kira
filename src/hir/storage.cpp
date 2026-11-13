#include "src/hir/storage.h"

#include <algorithm>
#include <string>
#include <string_view>

#include "src/parser/text_escape.h"

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

auto capture_kind_name(capture_kind kind) -> const char * {
  switch (kind) {
  case capture_kind::move:
    return "move";
  case capture_kind::copy:
    return "copy";
  case capture_kind::reference:
    return "ref";
  }
  return "?";
}

auto wait_mode_name(wait_mode mode) -> const char * {
  switch (mode) {
  case wait_mode::one:
    return "one";
  case wait_mode::all:
    return "all";
  case wait_mode::any:
    return "any";
  case wait_mode::crew:
    return "crew";
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
  case op_kind::replicate:
    return "replicate";
  case op_kind::publish_variant:
    return "publish";
  case op_kind::test_variant:
    return "test";
  case op_kind::drop_in_place:
    return "drop";
  case op_kind::forget:
    return "forget";
  case op_kind::zero_fill:
    return "zero";
  case op_kind::call_into:
    return "call";
  case op_kind::make_closure:
    return "closure";
  case op_kind::release_environment:
    return "release_env";
  case op_kind::make_generator:
    return "generator";
  case op_kind::generator_resume:
    return "resume";
  case op_kind::generator_take:
    return "take";
  case op_kind::coroutine_has_item:
    return "has_item";
  case op_kind::coroutine_dispatch:
    return "dispatch";
  case op_kind::coroutine_suspend:
    return "suspend";
  case op_kind::coroutine_await:
    return "await";
  case op_kind::task_block_on_group:
    return "block_on_group";
  case op_kind::coroutine_cancelled:
    return "cancelled";
  case op_kind::coroutine_finish:
    return "finish";
  case op_kind::storage_dead:
    return "dead";
  case op_kind::return_value:
    return "return";
  case op_kind::unreachable:
    return "unreachable";
  case op_kind::string_literal:
    return "string";
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

auto storage_env::pointee(semantic::type_id type) const
    -> std::optional<semantic::type_id> {
  const auto &entry = checked.types.entry(checked.types.strip_refinement(type));
  if (entry.kind == semantic::type_kind::ref_kind ||
      entry.kind == semantic::type_kind::ptr_kind) {
    return entry.result;
  }
  if (entry.kind == semantic::type_kind::builtin_generic_kind &&
      (entry.name == "cell" || entry.name == "cell_mut") &&
      entry.args.size() == 1) {
    return entry.args.front();
  }
  return std::nullopt;
}

auto storage_env::cell_reads_as_reference(semantic::type_id from,
                                          semantic::type_id to) const -> bool {
  const auto &source = checked.types.entry(checked.types.strip_refinement(from));
  const auto &target = checked.types.entry(checked.types.strip_refinement(to));
  if (source.kind != semantic::type_kind::builtin_generic_kind ||
      (source.name != "cell" && source.name != "cell_mut") ||
      source.args.size() != 1 || target.kind != semantic::type_kind::ref_kind) {
    return false;
  }
  if (target.is_mut && source.name != "cell_mut") {
    return false;
  }
  return checked.types.strip_refinement(source.args.front()) ==
         checked.types.strip_refinement(target.result);
}

auto storage_env::needs_drop(semantic::type_id type) -> bool {
  const auto stripped = checked.types.strip_refinement(type);
  if (const auto &entry = checked.types.entry(stripped);
      entry.kind == semantic::type_kind::builtin_generic_kind &&
      entry.name == "uninit") {
    return false; // Its slots promise no value, so it owns none.
  }
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

auto is_safe_tail_call(const storage_function &function, size_t index) -> bool {
  if (index >= function.ops.size() || function.coroutine_step ||
      function.static_init || function.signature.takes_environment ||
      function.signature.returns_through_address()) {
    return false;
  }
  const auto &call = function.ops[index];
  if (call.kind != op_kind::call_into || call.is_drop || !call.callee.is_direct() ||
      call.signature.takes_environment) {
    return false;
  }
  const auto plain = [](pass_class cls) {
    return cls == pass_class::scalar || cls == pass_class::zero_sized;
  };
  if (!plain(call.signature.result.cls) ||
      !std::ranges::all_of(call.signature.params,
                           [&](const abi_value &param) { return plain(param.cls); }) ||
      !std::ranges::all_of(call.args, [&](const call_argument &arg) {
        return plain(arg.cls) && arg.value.kind != operand_kind::address_of;
      })) {
    return false;
  }
  if (call.signature.result.cls != function.signature.result.cls) {
    return false;
  }
  for (auto at = index + 1; at < function.ops.size(); ++at) {
    const auto &op = function.ops[at];
    if (op.kind == op_kind::storage_dead) {
      continue;
    }
    if (op.kind != op_kind::return_value) {
      return false;
    }
    if (!op.value.has_value()) {
      return call.signature.result.cls == pass_class::zero_sized;
    }
    return op.value->kind == operand_kind::read && op.value->where.steps.empty() &&
           op.value->where.base == call.dest.base && call.dest.steps.empty();
  }
  return false;
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

auto program_function_name(std::string_view entry, std::string_view module,
                           std::string_view name) -> std::string {
  if (module == entry) {
    return std::string(name);
  }
  return std::string(module) + "::" + std::string(name);
}

auto program_callee_name(std::string_view entry, std::string_view module,
                         const call_target &callee) -> std::string {
  return program_function_name(
      entry, callee.owner_module.has_value() ? *callee.owner_module : module,
      callee.name);
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
    case projection_kind::length:
      out += ".len";
      break;
    case projection_kind::data:
      out += ".data";
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
  case compute_kind::checked_index:
    return "index " + render_operand(op.operands.at(0)) + " < " +
           render_operand(op.operands.at(1));
  case compute_kind::checked_end:
    return "bound " + render_operand(op.operands.at(0)) + " <= " +
           render_operand(op.operands.at(1));
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

/// `<size>/<align> [<kind> <source> @<offset>, ...]`, the state or environment
/// block a closure or generator fills.
[[nodiscard]] auto render_captures(const storage_op &op) -> std::string {
  auto out = std::to_string(op.env_size) + "/" + std::to_string(op.env_align) +
             " [";
  auto first = true;
  for (const auto &capture : op.captures) {
    out += first ? "" : ", ";
    first = false;
    out += std::string{capture_kind_name(capture.kind)} + " " +
           render_place(capture.source) + " @" + std::to_string(capture.offset);
  }
  return out + "]";
}

[[nodiscard]] auto render_op(const storage_op &op) -> std::string {
  switch (op.kind) {
  case op_kind::compute:
    return "compute " + render_place(op.dest) + " = " + render_compute(op);
  case op_kind::init_empty:
    return "init_empty " + render_place(op.dest);
  case op_kind::copy_into:
    return std::string{op.shallow ? "copy.shallow " : "copy "} +
           render_place(op.dest) + " <- " + render_place(op.source);
  case op_kind::move_into:
    return "move " + render_place(op.dest) + " <- " + render_place(op.source);
  case op_kind::replicate:
    return "replicate " + render_place(op.dest) + " <- " +
           render_place(op.source);
  case op_kind::publish_variant:
    return "publish " + render_place(op.dest) + " @" + op.variant;
  case op_kind::test_variant:
    return "test " + render_place(op.dest) + " = " + render_place(op.source) +
           " is @" + op.variant;
  case op_kind::drop_in_place:
    return "drop " + render_place(op.dest);
  case op_kind::forget:
    return "forget " + render_place(op.dest);
  case op_kind::zero_fill:
    return "zero " + render_place(op.dest);
  case op_kind::call_into:
    return std::string{op.is_drop     ? "call.drop "
                       : op.tail_call ? "call.tail "
                                      : "call "} +
           render_call(op);
  case op_kind::make_closure:
    return "closure " + render_place(op.dest) + " = " + op.entry + " glue " +
           (op.glue.empty() ? std::string{"-"} : op.glue) + " env " +
           render_captures(op);
  case op_kind::make_generator:
    return "generator " + render_place(op.dest) + " = " + op.entry + " state " +
           render_captures(op);
  case op_kind::generator_resume:
    return "resume " + render_place(op.dest) + " = " + render_place(op.source);
  case op_kind::generator_take:
    return "take " + render_place(op.dest) + " <- " + render_place(op.source);
  case op_kind::coroutine_has_item:
    return "has_item " + render_place(op.dest) + " = " +
           render_place(op.source);
  case op_kind::coroutine_dispatch:
    return "dispatch " + render_place(op.source) + " of " +
           std::to_string(op.depth);
  case op_kind::coroutine_suspend:
    return "suspend #" + std::to_string(op.depth) + " " +
           render_place(op.dest) + " <- " + render_place(op.source);
  case op_kind::coroutine_await:
    return std::string{"await"} +
           (op.waits == wait_mode::one
                ? std::string{}
                : std::string{" "} + wait_mode_name(op.waits)) +
           " #" + std::to_string(op.depth) + " " + render_place(op.dest) +
           " <- " + render_place(op.source);
  case op_kind::task_block_on_group:
    return std::string{"block_on_group "} + wait_mode_name(op.waits) + " " +
           render_place(op.source);
  case op_kind::coroutine_cancelled:
    return "cancelled " + render_place(op.dest) + " = " +
           render_place(op.source);
  case op_kind::coroutine_finish:
    return "finish " + render_place(op.source);
  case op_kind::release_environment:
    return "release_env";
  case op_kind::storage_dead:
    return "dead s" + std::to_string(op.slot);
  case op_kind::return_value:
    return op.value.has_value() ? "return " + render_operand(*op.value)
                                : std::string{"return"};
  case op_kind::unreachable:
    return "unreachable";
  case op_kind::string_literal:
    return "string " + render_place(op.dest) + " = " +
           encode_string_literal(op.text);
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
           (slot.initially_initialized ? " (initialized)" : "") +
           (slot.owns_nothing ? " (owns nothing)" : "") +
           (slot.cls == storage_class::closure_environment
                ? std::string{slot.env_indirect ? " env->@" : " env@"} +
                      std::to_string(slot.env_offset)
                : slot.cls == storage_class::coroutine_frame
                      ? " state@" + std::to_string(slot.env_offset)
                      : std::string{}) +
           "\n";
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

