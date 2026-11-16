#include "src/core/intrinsics.h"

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace cinder::core {
namespace {

using semantic::type_entry;
using semantic::type_id;
using semantic::type_kind;
using semantic::type_table;

/// A message when the shape is wrong; nothing when it is right.
using verdict = std::optional<std::string>;

/// A container operand: the container, and how the operand held it.
struct container_operand {
  type_id type = semantic::k_unknown_type;
  bool borrowed = false;
  bool borrowed_mut = false;
};

/// One call being checked.
struct call_shape {
  std::string_view name;
  const type_table &types;
  std::span<const type_id> params;
  type_id result;
  std::span<const type_id> generics;

  [[nodiscard]] auto entry(type_id id) const -> const type_entry & { return types.entry(id); }
  [[nodiscard]] auto show(type_id id) const -> std::string { return types.display(id); }

  [[nodiscard]] auto is_builtin(type_id id, std::string_view n) const -> bool {
    const auto &e = entry(id);
    return e.kind == type_kind::builtin_kind && e.name == n;
  }
  [[nodiscard]] auto is_generic(type_id id, std::string_view n) const -> bool {
    const auto &e = entry(id);
    return e.kind == type_kind::builtin_generic_kind && e.name == n;
  }
  [[nodiscard]] auto is_generic(type_id id, std::string_view n, std::size_t arity) const -> bool {
    return is_generic(id, n) && entry(id).args.size() == arity;
  }
  [[nodiscard]] auto is_raw_pointer(type_id id) const -> bool {
    return entry(id).kind == type_kind::ptr_kind;
  }
  [[nodiscard]] auto is_ref(type_id id, bool is_mut) const -> bool {
    const auto &e = entry(id);
    return e.kind == type_kind::ref_kind && e.is_mut == is_mut;
  }
  [[nodiscard]] auto is_integer(type_id id) const -> bool {
    return entry(id).kind == type_kind::builtin_kind && types.is_integer(id);
  }
  [[nodiscard]] auto is_usize(type_id id) const -> bool {
    return id != semantic::k_unknown_type && id == types.usize_type();
  }
  [[nodiscard]] auto is_task(type_id id) const -> bool {
    return is_generic(id, "task") && !entry(id).args.empty();
  }
  [[nodiscard]] auto is_unit(type_id id) const -> bool { return is_builtin(id, "unit"); }

  [[nodiscard]] auto message(std::string_view what) const -> std::string {
    return std::format("`{}`: {}", name, what);
  }

  /// Exactly `n` operands.
  [[nodiscard]] auto arity(std::size_t n) const -> verdict {
    if (params.size() != n) {
      return message(std::format("takes {} operand{}, found {}", n, n == 1 ? "" : "s",
                                 params.size()));
    }
    return std::nullopt;
  }
  /// Exactly `n` generic arguments.
  [[nodiscard]] auto generic_count(std::size_t n) const -> verdict {
    if (generics.size() != n) {
      return message(std::format("takes {} generic argument{}, found {}", n, n == 1 ? "" : "s",
                                 generics.size()));
    }
    return std::nullopt;
  }
  [[nodiscard]] auto result_is(type_id expected) const -> verdict {
    if (result != expected) {
      return message(
          std::format("the result is `{}`, expected `{}`", show(result), show(expected)));
    }
    return std::nullopt;
  }
  [[nodiscard]] auto operand_is(std::size_t k, type_id expected) const -> verdict {
    if (params[k] != expected) {
      return message(std::format("operand {} is `{}`, expected `{}`", k, show(params[k]),
                                 show(expected)));
    }
    return std::nullopt;
  }
  [[nodiscard]] auto operand_usize(std::size_t k) const -> verdict {
    if (!is_usize(params[k])) {
      return message(std::format("operand {} is `{}`, expected `usize`", k, show(params[k])));
    }
    return std::nullopt;
  }
  [[nodiscard]] auto operand_str(std::size_t k) const -> verdict {
    if (!is_builtin(params[k], "str")) {
      return message(std::format("operand {} is `{}`, expected `str`", k, show(params[k])));
    }
    return std::nullopt;
  }
  [[nodiscard]] auto operand_task(std::size_t k) const -> verdict {
    if (!is_task(params[k])) {
      return message(std::format("operand {} is `{}`, expected a task", k, show(params[k])));
    }
    return std::nullopt;
  }

  /// The element of an array, a list or a slice view.
  [[nodiscard]] auto element_of(type_id id) const -> std::optional<type_id> {
    const auto &e = entry(id);
    if (e.kind == type_kind::array_kind) {
      return e.result;
    }
    if ((is_generic(id, "list") || is_generic(id, "slice") || is_generic(id, "slice_mut")) &&
        e.args.size() == 1) {
      return e.args[0];
    }
    return std::nullopt;
  }

  /// Operand `k` as a builtin container, by value or lent. Only a `slice` is
  /// passed by value; every other container is lent.
  [[nodiscard]] auto container(std::size_t k, container_operand &out) const -> verdict {
    auto id = params[k];
    const auto &outer = entry(id);
    if (outer.kind == type_kind::ref_kind) {
      out = {.type = outer.result, .borrowed = true, .borrowed_mut = outer.is_mut};
    } else {
      out = {.type = id, .borrowed = false, .borrowed_mut = false};
    }
    const bool sequence =
        element_of(out.type).has_value() || is_builtin(out.type, "str");
    if (!sequence) {
      return message(std::format("operand {} is `{}`, which is not an array, list, `str` or slice",
                                 k, show(params[k])));
    }
    if (!out.borrowed && !is_generic(out.type, "slice")) {
      return message(std::format("operand {} is `{}`, which must be lent (`&`) unless it is a "
                                 "`slice`",
                                 k, show(params[k])));
    }
    return std::nullopt;
  }

  /// `T` of the single generic argument must equal `expected`.
  [[nodiscard]] auto generic_is(type_id expected) const -> verdict {
    if (generics[0] != expected) {
      return message(std::format("the generic argument is `{}`, expected `{}`",
                                 show(generics[0]), show(expected)));
    }
    return std::nullopt;
  }
};

// ---------------------------------------------------------------------------
//  Raw pointers
// ---------------------------------------------------------------------------

auto op_ptr_offset(const call_shape &c) -> verdict {
  if (auto e = c.arity(2)) {
    return e;
  }
  if (!c.is_raw_pointer(c.params[0])) {
    return c.message(std::format("operand 0 is `{}`, expected a raw pointer", c.show(c.params[0])));
  }
  if (auto e = c.operand_usize(1)) {
    return e;
  }
  if (auto e = c.result_is(c.params[0])) {
    return e;
  }
  if (auto e = c.generic_count(1)) {
    return e;
  }
  return c.generic_is(c.entry(c.params[0]).result);
}

auto op_ptr_cast(const call_shape &c) -> verdict {
  if (auto e = c.arity(1)) {
    return e;
  }
  if (!c.is_raw_pointer(c.params[0]) && c.entry(c.params[0]).kind != type_kind::ref_kind) {
    return c.message(std::format("operand 0 is `{}`, expected a raw pointer or a reference",
                                 c.show(c.params[0])));
  }
  if (!c.is_raw_pointer(c.result)) {
    return c.message(std::format("the result is `{}`, expected a raw pointer", c.show(c.result)));
  }
  if (auto e = c.generic_count(1)) {
    return e;
  }
  return c.generic_is(c.entry(c.result).result);
}

auto op_slice_from_raw_parts(const call_shape &c) -> verdict {
  if (auto e = c.arity(2)) {
    return e;
  }
  if (!c.is_raw_pointer(c.params[0])) {
    return c.message(std::format("operand 0 is `{}`, expected a raw pointer", c.show(c.params[0])));
  }
  if (auto e = c.operand_usize(1)) {
    return e;
  }
  const auto &p = c.entry(c.params[0]);
  const auto want = p.is_mut ? "slice_mut" : "slice";
  if (!c.is_generic(c.result, want, 1) || c.entry(c.result).args[0] != p.result) {
    return c.message(std::format("the result is `{}`, expected `{}[{}]`", c.show(c.result), want,
                                 c.show(p.result)));
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
//  Containers
// ---------------------------------------------------------------------------

auto op_range_view(const call_shape &c) -> verdict {
  if (auto e = c.arity(3)) {
    return e;
  }
  auto box = container_operand{};
  if (auto e = c.container(0, box)) {
    return e;
  }
  for (const std::size_t k : {std::size_t{1}, std::size_t{2}}) {
    if (!c.is_integer(c.params[k])) {
      return c.message(
          std::format("operand {} is `{}`, expected an integer bound", k, c.show(c.params[k])));
    }
  }
  if (c.is_builtin(box.type, "str")) {
    return c.result_is(box.type);
  }
  const bool is_mut_view = c.is_generic(c.result, "slice_mut", 1);
  if (!is_mut_view && !c.is_generic(c.result, "slice", 1)) {
    return c.message(
        std::format("the result is `{}`, expected a `slice` or `slice_mut`", c.show(c.result)));
  }
  if (c.entry(c.result).args[0] != *c.element_of(box.type)) {
    return c.message(std::format("the result is `{}`, but the container's element is `{}`",
                                 c.show(c.result), c.show(*c.element_of(box.type))));
  }
  if (is_mut_view && !box.borrowed_mut) {
    return c.message("a `slice_mut` result needs the container lent with `&mut`");
  }
  return std::nullopt;
}

auto op_len(const call_shape &c) -> verdict {
  if (auto e = c.arity(1)) {
    return e;
  }
  auto box = container_operand{};
  if (auto e = c.container(0, box)) {
    return e;
  }
  return c.result_is(c.types.usize_type());
}

auto op_container_data(const call_shape &c) -> verdict {
  if (auto e = c.arity(1)) {
    return e;
  }
  auto box = container_operand{};
  // A stack buffer lent with `&` or `&mut` is also a container here.
  const auto &outer = c.entry(c.params[0]);
  const bool buffer = outer.kind == type_kind::ref_kind && c.is_generic(outer.result, "uninit", 2);
  if (buffer) {
    box = {.type = outer.result, .borrowed = true, .borrowed_mut = outer.is_mut};
  } else if (auto e = c.container(0, box)) {
    return e;
  }
  if (!c.is_raw_pointer(c.result)) {
    return c.message(std::format("the result is `{}`, expected a raw pointer", c.show(c.result)));
  }
  const auto &r = c.entry(c.result);
  const auto element = buffer ? std::optional<type_id>{c.entry(box.type).args[0]}
                              : c.element_of(box.type);
  if (element && r.result != *element) {
    return c.message(std::format("the result is `{}`, but the container's element is `{}`",
                                 c.show(c.result), c.show(*element)));
  }
  if (r.is_mut && !box.borrowed_mut) {
    return c.message("a `*mut` result needs the container lent with `&mut`");
  }
  return std::nullopt;
}

auto op_as_bytes(const call_shape &c) -> verdict {
  if (auto e = c.arity(1)) {
    return e;
  }
  if (auto e = c.operand_str(0)) {
    return e;
  }
  if (!c.is_generic(c.result, "slice", 1) ||
      !(c.is_builtin(c.entry(c.result).args[0], "uint8") ||
        c.is_builtin(c.entry(c.result).args[0], "byte"))) {
    return c.message(std::format("the result is `{}`, expected `slice[uint8]`", c.show(c.result)));
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
//  Generators and cells
// ---------------------------------------------------------------------------

auto op_generator_next(const call_shape &c) -> verdict {
  if (auto e = c.arity(1)) {
    return e;
  }
  const auto &p = c.entry(c.params[0]);
  if (!c.is_ref(c.params[0], true) || !c.is_generic(p.result, "generator", 1)) {
    return c.message(
        std::format("operand 0 is `{}`, expected `&mut generator[T]`", c.show(c.params[0])));
  }
  const auto item = c.entry(p.result).args[0];
  if (!c.is_generic(c.result, "option", 1) || c.entry(c.result).args[0] != item) {
    return c.message(
        std::format("the result is `{}`, expected `option[{}]`", c.show(c.result), c.show(item)));
  }
  return std::nullopt;
}

auto op_cell_get(const call_shape &c) -> verdict {
  if (auto e = c.arity(1)) {
    return e;
  }
  if (!c.is_generic(c.params[0], "cell", 1) && !c.is_generic(c.params[0], "cell_mut", 1)) {
    return c.message(
        std::format("operand 0 is `{}`, expected `cell[T]` or `cell_mut[T]`", c.show(c.params[0])));
  }
  return c.result_is(c.entry(c.params[0]).args[0]);
}

auto op_cell_set(const call_shape &c) -> verdict {
  if (auto e = c.arity(2)) {
    return e;
  }
  if (!c.is_generic(c.params[0], "cell_mut", 1)) {
    return c.message(std::format("operand 0 is `{}`, expected `cell_mut[T]`", c.show(c.params[0])));
  }
  if (auto e = c.operand_is(1, c.entry(c.params[0]).args[0])) {
    return e;
  }
  if (!c.is_unit(c.result)) {
    return c.message(std::format("the result is `{}`, expected `unit`", c.show(c.result)));
  }
  return std::nullopt;
}

/// `cell[T]` to `&T`, `cell_mut[T]` to `&mut T` (`from_cell`), or back.
auto cell_conversion(const call_shape &c, std::string_view cell_name, bool ref_mut, bool from_cell)
    -> verdict {
  if (auto e = c.arity(1)) {
    return e;
  }
  const auto from = c.params[0];
  const auto to = c.result;
  const auto cell_type = from_cell ? from : to;
  const auto ref_type = from_cell ? to : from;
  if (!c.is_generic(cell_type, cell_name, 1)) {
    return c.message(std::format("expected `{}[T]` as the {}, found `{}`", cell_name,
                                from_cell ? "operand" : "result", c.show(cell_type)));
  }
  if (!c.is_ref(ref_type, ref_mut)) {
    return c.message(std::format("expected `&{}T` as the {}, found `{}`", ref_mut ? "mut " : "",
                                from_cell ? "result" : "operand", c.show(ref_type)));
  }
  const auto element = c.entry(cell_type).args[0];
  if (c.entry(ref_type).result != element) {
    return c.message(std::format("the cell holds `{}` but the reference is to `{}`",
                                 c.show(element), c.show(c.entry(ref_type).result)));
  }
  if (auto e = c.generic_count(1)) {
    return e;
  }
  return c.generic_is(element);
}

auto op_cell_to_ref(const call_shape &c) -> verdict { return cell_conversion(c, "cell", false, true); }
auto op_cell_mut_to_ref_mut(const call_shape &c) -> verdict {
  return cell_conversion(c, "cell_mut", true, true);
}
auto op_ref_to_cell(const call_shape &c) -> verdict {
  return cell_conversion(c, "cell", false, false);
}
auto op_ref_mut_to_cell_mut(const call_shape &c) -> verdict {
  return cell_conversion(c, "cell_mut", true, false);
}

// ---------------------------------------------------------------------------
//  Buffers and layout queries
// ---------------------------------------------------------------------------

auto op_uninit_len(const call_shape &c) -> verdict {
  if (auto e = c.arity(0)) {
    return e;
  }
  if (auto e = c.generic_count(1)) {
    return e;
  }
  if (!c.is_generic(c.generics[0], "uninit", 2)) {
    return c.message(std::format("the generic argument is `{}`, expected an `uninit[T, N]` buffer",
                                 c.show(c.generics[0])));
  }
  return c.result_is(c.types.usize_type());
}

auto op_stack_buffer(const call_shape &c) -> verdict {
  if (auto e = c.arity(0)) {
    return e;
  }
  if (auto e = c.generic_count(1)) {
    return e;
  }
  if (!c.is_generic(c.generics[0], "uninit", 2)) {
    return c.message(std::format("the generic argument is `{}`, expected an `uninit[T, N]` buffer",
                                 c.show(c.generics[0])));
  }
  return c.result_is(c.generics[0]);
}

auto op_size_of(const call_shape &c) -> verdict {
  if (auto e = c.arity(0)) {
    return e;
  }
  if (auto e = c.generic_count(1)) {
    return e;
  }
  return c.result_is(c.types.usize_type());
}

auto op_needs_drop(const call_shape &c) -> verdict {
  if (auto e = c.arity(0)) {
    return e;
  }
  if (auto e = c.generic_count(1)) {
    return e;
  }
  return c.result_is(c.types.bool_type());
}

// ---------------------------------------------------------------------------
//  Strings
// ---------------------------------------------------------------------------

auto op_rt_str_concat(const call_shape &c) -> verdict {
  if (auto e = c.arity(2)) {
    return e;
  }
  for (const std::size_t k : {std::size_t{0}, std::size_t{1}}) {
    if (auto e = c.operand_str(k)) {
      return e;
    }
  }
  return c.result_is(c.params[0]);
}

auto op_str_decode_scalar(const call_shape &c) -> verdict {
  if (auto e = c.arity(2)) {
    return e;
  }
  if (auto e = c.operand_str(0)) {
    return e;
  }
  if (auto e = c.operand_usize(1)) {
    return e;
  }
  return c.result_is(c.types.char_type());
}

auto op_str_scalar_width(const call_shape &c) -> verdict {
  if (auto e = c.arity(2)) {
    return e;
  }
  if (auto e = c.operand_str(0)) {
    return e;
  }
  if (auto e = c.operand_usize(1)) {
    return e;
  }
  return c.result_is(c.types.usize_type());
}

// ---------------------------------------------------------------------------
//  Tasks and crews
// ---------------------------------------------------------------------------

auto op_task_block(const call_shape &c) -> verdict {
  if (auto e = c.arity(1)) {
    return e;
  }
  const auto &f = c.entry(c.params[0]);
  if (f.kind != type_kind::fn_kind || !f.args.empty()) {
    return c.message(
        std::format("operand 0 is `{}`, expected a `fn() -> T`", c.show(c.params[0])));
  }
  if (!c.is_task(c.result) || !task_awaits(c.types, c.result, f.result)) {
    return c.message(std::format("the result is `{}`, expected a task that awaits `{}`",
                                 c.show(c.result), c.show(f.result)));
  }
  return std::nullopt;
}

auto op_on_context(const call_shape &c) -> verdict {
  if (auto e = c.arity(1)) {
    return e;
  }
  if (auto e = c.operand_task(0)) {
    return e;
  }
  return c.result_is(c.params[0]);
}

auto op_await_par(const call_shape &c) -> verdict {
  if (c.params.empty()) {
    return c.message("takes at least one task");
  }
  for (std::size_t k = 0; k < c.params.size(); ++k) {
    if (auto e = c.operand_task(k)) {
      return e;
    }
  }
  const auto &r = c.entry(c.result);
  if (r.kind != type_kind::tuple_kind || r.args.size() != c.params.size()) {
    return c.message(std::format("the result is `{}`, expected a tuple of {} awaited types",
                                 c.show(c.result), c.params.size()));
  }
  for (std::size_t k = 0; k < c.params.size(); ++k) {
    if (!task_awaits(c.types, c.params[k], r.args[k])) {
      return c.message(std::format("element {} of the result is `{}`, which is not what `{}` awaits",
                                   k, c.show(r.args[k]), c.show(c.params[k])));
    }
  }
  return std::nullopt;
}

auto op_await_race(const call_shape &c) -> verdict {
  if (c.params.empty()) {
    return c.message("takes at least one task");
  }
  for (std::size_t k = 0; k < c.params.size(); ++k) {
    if (auto e = c.operand_task(k)) {
      return e;
    }
    if (!task_awaits(c.types, c.params[k], c.result)) {
      return c.message(std::format("operand {} is `{}`, which does not await the result `{}`", k,
                                   c.show(c.params[k]), c.show(c.result)));
    }
  }
  return std::nullopt;
}

auto op_crew_new(const call_shape &c) -> verdict {
  if (auto e = c.arity(0)) {
    return e;
  }
  if (!c.is_generic(c.result, "crew", 0)) {
    return c.message(std::format("the result is `{}`, expected `crew`", c.show(c.result)));
  }
  return std::nullopt;
}

auto op_crew_end(const call_shape &c) -> verdict {
  if (auto e = c.arity(1)) {
    return e;
  }
  if (!c.is_generic(c.params[0], "crew", 0)) {
    return c.message(std::format("operand 0 is `{}`, expected `crew`", c.show(c.params[0])));
  }
  if (!c.is_unit(c.result)) {
    return c.message(std::format("the result is `{}`, expected `unit`", c.show(c.result)));
  }
  return std::nullopt;
}

/// Operand `k` is `&task`.
auto ref_to_task(const call_shape &c, std::size_t k) -> verdict {
  if (!c.is_ref(c.params[k], false) || !c.is_task(c.entry(c.params[k]).result)) {
    return c.message(std::format("operand {} is `{}`, expected `&task`", k, c.show(c.params[k])));
  }
  return std::nullopt;
}

auto op_task_spawn(const call_shape &c) -> verdict {
  if (auto e = c.arity(1)) {
    return e;
  }
  if (auto e = ref_to_task(c, 0)) {
    return e;
  }
  if (!c.is_unit(c.result)) {
    return c.message(std::format("the result is `{}`, expected `unit`", c.show(c.result)));
  }
  return std::nullopt;
}

auto op_task_get(const call_shape &c) -> verdict {
  if (auto e = c.arity(1)) {
    return e;
  }
  if (auto e = ref_to_task(c, 0)) {
    return e;
  }
  if (!task_awaits(c.types, c.entry(c.params[0]).result, c.result)) {
    return c.message(std::format("the result is `{}`, which is not what `{}` awaits",
                                 c.show(c.result), c.show(c.entry(c.params[0]).result)));
  }
  return std::nullopt;
}

auto ref_to_crew(const call_shape &c, std::size_t k) -> verdict {
  if (!c.is_ref(c.params[k], false) || !c.is_generic(c.entry(c.params[k]).result, "crew", 0)) {
    return c.message(std::format("operand {} is `{}`, expected `&crew`", k, c.show(c.params[k])));
  }
  return std::nullopt;
}

auto op_crew_spawn(const call_shape &c) -> verdict {
  if (auto e = c.arity(2)) {
    return e;
  }
  if (auto e = ref_to_crew(c, 0)) {
    return e;
  }
  return c.operand_task(1);
}

auto op_crew_errors(const call_shape &c) -> verdict {
  if (auto e = c.arity(1)) {
    return e;
  }
  if (auto e = ref_to_crew(c, 0)) {
    return e;
  }
  // The standard library's `list[E]` is a struct of module `std.list`; the
  // builtin generic form is accepted too.
  const auto &r = c.entry(c.result);
  const bool std_list = r.kind == type_kind::struct_kind && r.name == "list" &&
                        r.module_name == "std.list" && r.args.size() == 1;
  if (!c.is_generic(c.result, "list", 1) && !std_list) {
    return c.message(std::format("the result is `{}`, expected a `list[E]`", c.show(c.result)));
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
//  The table
// ---------------------------------------------------------------------------

struct operation {
  std::string_view name;
  verdict (*check)(const call_shape &);
};

constexpr auto k_operations = std::to_array<operation>({
    {"ptr_offset", op_ptr_offset},
    {"range_view", op_range_view},
    // `xs[a..=b]`: the same view with `b` included. Kept apart from
    // `range_view(xs, a, b + 1)`, which overflows when `b` is the largest
    // value of its type.
    {"range_view_inclusive", op_range_view},
    {"len", op_len},
    {"container_data", op_container_data},
    {"as_bytes", op_as_bytes},
    {"generator_next", op_generator_next},
    {"cell_get", op_cell_get},
    {"cell_set", op_cell_set},
    {"cell_to_ref", op_cell_to_ref},
    {"cell_mut_to_ref_mut", op_cell_mut_to_ref_mut},
    {"ref_to_cell", op_ref_to_cell},
    {"ref_mut_to_cell_mut", op_ref_mut_to_cell_mut},
    {"uninit_len", op_uninit_len},
    {"stack_buffer", op_stack_buffer},
    {"ptr_cast", op_ptr_cast},
    {"slice_from_raw_parts", op_slice_from_raw_parts},
    {"size_of", op_size_of},
    {"align_of", op_size_of},
    {"needs_drop", op_needs_drop},
    {"rt_str_concat", op_rt_str_concat},
    {"str_decode_scalar", op_str_decode_scalar},
    {"str_scalar_width", op_str_scalar_width},
    {"task_block", op_task_block},
    {"on_cpu", op_on_context},
    {"on_io", op_on_context},
    {"await_par", op_await_par},
    {"await_race", op_await_race},
    {"crew_new", op_crew_new},
    {"crew_end", op_crew_end},
    {"task_spawn", op_task_spawn},
    {"crew_spawn", op_crew_spawn},
    {"task_get", op_task_get},
    {"crew_errors", op_crew_errors},
});

[[nodiscard]] auto find_operation(std::string_view name) -> const operation * {
  for (const auto &op : k_operations) {
    if (op.name == name) {
      return &op;
    }
  }
  return nullptr;
}

} // namespace

auto is_compiler_operation(std::string_view name) -> bool {
  return find_operation(name) != nullptr;
}

auto check_intrinsic(std::string_view name, type_id signature,
                     std::span<const type_id> generic_args, const semantic::checked_types &checked)
    -> std::optional<std::string> {
  const auto *op = find_operation(name);
  if (op == nullptr) {
    return std::format("`{}` is not a compiler operation", name);
  }
  const auto &sig = checked.types.entry(signature);
  if (sig.kind != type_kind::fn_kind) {
    return std::nullopt;
  }
  return op->check(call_shape{.name = name,
                              .types = checked.types,
                              .params = sig.args,
                              .result = sig.result,
                              .generics = generic_args});
}

auto task_awaits(const type_table &types, type_id task, type_id awaited) -> bool {
  const auto &t = types.entry(task);
  if (t.kind != type_kind::builtin_generic_kind || t.name != "task" || t.args.empty()) {
    return false;
  }
  const auto &err = types.entry(t.args.size() >= 2 ? t.args[1] : semantic::k_unknown_type);
  const bool fallible = t.args.size() >= 2 && !(err.kind == type_kind::builtin_kind && err.name == "never");
  if (!fallible) {
    return awaited == t.args[0];
  }
  const auto &r = types.entry(awaited);
  return r.kind == type_kind::builtin_generic_kind && r.name == "result" && r.args.size() == 2 &&
         r.args[0] == t.args[0] && r.args[1] == t.args[1];
}

} // namespace cinder::core
