#include "src/hir/storage_nrvo.h"

#include <set>
#include <string>
#include <utility>

namespace cinder::hir {

namespace {

/// Calls `visit` on every place `op` names.
template <typename Op, typename Visit>
auto for_each_place(Op &op, Visit &&visit) -> void {
  const auto touch = [&](auto &where) {
    if (where.base != k_invalid_storage) {
      visit(where);
    }
  };
  touch(op.dest);
  touch(op.source);
  for (auto &operand : op.operands) {
    touch(operand.where);
  }
  touch(op.condition.where);
  if (op.callee.callable.has_value()) {
    touch(*op.callee.callable);
  }
  for (auto &arg : op.args) {
    touch(arg.value.where);
    touch(arg.slot);
  }
  for (auto &capture : op.captures) {
    touch(capture.source);
  }
  if (op.value.has_value()) {
    touch(op.value->where);
  }
}

[[nodiscard]] auto mentions(const storage_op &op, storage_id slot) -> bool {
  auto found = op.kind == op_kind::storage_dead && op.slot == slot;
  for_each_place(op, [&](const place &where) {
    found = found || where.base == slot;
  });
  return found;
}

/// Why `op` stops `local` from living in the result storage; empty when it
/// does not. `return_move` is the move at a return, which goes away.
[[nodiscard]] auto unfit_use(const storage_op &op, storage_id local,
                             bool return_move) -> std::string {
  const auto address_taken = [&](const operand &value) {
    return value.where.base == local &&
           (value.kind == operand_kind::address_of || value.consume);
  };
  switch (op.kind) {
  case op_kind::compute:
    for (const auto &value : op.operands) {
      if (address_taken(value)) {
        return "its address is taken";
      }
    }
    return {};
  case op_kind::init_empty:
  case op_kind::publish_variant:
  case op_kind::drop_in_place:
  case op_kind::copy_into:
  case op_kind::replicate:
  case op_kind::test_variant:
  case op_kind::storage_dead:
    return {};
  case op_kind::move_into:
    if (op.source.base == local && !return_move) {
      return "it is moved out before the return";
    }
    return {};
  case op_kind::if_begin:
  case op_kind::check:
    return address_taken(op.condition) ? "its address is taken" : "";
  case op_kind::call_into:
    if (op.callee.callable.has_value() && op.callee.callable->base == local) {
      return "it is called";
    }
    for (const auto &arg : op.args) {
      if (arg.slot.base == local) {
        return "it is passed on";
      }
      // A synthesized drop looks at the value in place; any other call that
      // takes its address might keep it.
      if (address_taken(arg.value) &&
          !(op.is_drop && arg.value.kind == operand_kind::address_of)) {
        return "its address is taken";
      }
    }
    return {};
  default:
    return mentions(op, local) ? std::string{"an `"} + op_kind_name(op.kind) +
                                     "` operation uses it"
                               : std::string{};
  }
}

} // namespace

auto apply_nrvo(storage_function &function) -> std::vector<nrvo_decision> {
  auto decisions = std::vector<nrvo_decision>{};
  const auto result = function.result_slot;
  if (result == k_invalid_storage || function.coroutine_step) {
    return decisions;
  }
  auto &ops = function.ops;

  // What each return moves into the result storage.
  auto return_moves = std::set<size_t>{};
  auto locals = std::set<storage_id>{};
  auto every_return_moves_a_local = true;
  for (size_t i = 0; i < ops.size(); ++i) {
    if (ops[i].kind != op_kind::return_value) {
      continue;
    }
    auto at = i;
    while (at > 0 && ops[at - 1].kind == op_kind::storage_dead) {
      --at;
    }
    if (at > 0 && ops[at - 1].kind == op_kind::move_into &&
        ops[at - 1].dest.base == result && ops[at - 1].dest.steps.empty() &&
        ops[at - 1].source.steps.empty()) {
      return_moves.insert(at - 1);
      locals.insert(ops[at - 1].source.base);
    } else {
      every_return_moves_a_local = false;
    }
  }
  if (locals.empty()) {
    return decisions;
  }

  const auto local = *locals.begin();
  auto decision = nrvo_decision{.function = function.name,
                                .local = function.slots[local].name};
  const auto reject = [&](std::string why) {
    decision.reason = std::move(why);
    decisions.push_back(std::move(decision));
    return decisions;
  };
  if (!every_return_moves_a_local) {
    return reject("a return builds its value without this local");
  }
  if (locals.size() > 1) {
    return reject("the returns move different locals");
  }
  const auto &slot = function.slots[local];
  if (slot.cls != storage_class::frame_local) {
    return reject(std::string{"it is a "} + storage_class_name(slot.cls) +
                  " slot");
  }
  if (slot.type != function.slots[result].type) {
    return reject("its type is not the result's");
  }
  if (slot.initially_initialized) {
    return reject("it holds a value on entry");
  }

  // The first write must come outside every conditional and loop, so that
  // it comes before every return.
  auto depth = size_t{0};
  auto written = false;
  for (size_t i = 0; i < ops.size(); ++i) {
    const auto &op = ops[i];
    if (mentions(op, local)) {
      if (!written) {
        if (op.dest.base != local || op.kind == op_kind::storage_dead) {
          return reject("it is read before it is written");
        }
        if (depth != 0) {
          return reject("it is first written inside a conditional or a loop");
        }
        written = true;
      }
      if (auto why = unfit_use(op, local, return_moves.contains(i));
          !why.empty()) {
        return reject(std::move(why));
      }
    }
    switch (op.kind) {
    case op_kind::if_begin:
    case op_kind::loop_begin:
    case op_kind::block_begin:
      ++depth;
      break;
    case op_kind::if_end:
    case op_kind::loop_end:
    case op_kind::block_end:
      --depth;
      break;
    default:
      break;
    }
  }
  // Build the local in the result storage: its places move to the result
  // slot, the moves at the returns and the local's end disappear, and the
  // slot is deleted.
  auto kept = std::vector<storage_op>{};
  kept.reserve(ops.size());
  for (size_t i = 0; i < ops.size(); ++i) {
    auto &op = ops[i];
    if (return_moves.contains(i) ||
        (op.kind == op_kind::storage_dead && op.slot == local)) {
      continue;
    }
    for_each_place(op, [&](place &where) {
      if (where.base == local) {
        where.base = result;
      }
    });
    kept.push_back(std::move(op));
  }
  ops = std::move(kept);
  const auto renumber = [&](storage_id id) {
    return id > local ? id - 1 : id;
  };
  for (auto &op : ops) {
    for_each_place(op, [&](place &where) { where.base = renumber(where.base); });
    if (op.kind == op_kind::storage_dead) {
      op.slot = renumber(op.slot);
    }
  }
  function.slots.erase(function.slots.begin() + static_cast<std::ptrdiff_t>(local));
  for (auto &remaining : function.slots) {
    remaining.id = renumber(remaining.id);
    remaining.owner = renumber(remaining.owner);
  }
  function.result_slot = renumber(function.result_slot);
  for (auto &param : function.param_slots) {
    param = renumber(param);
  }
  decision.applied = true;
  decisions.push_back(std::move(decision));
  return decisions;
}

} // namespace cinder::hir
