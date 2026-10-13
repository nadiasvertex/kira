#include "src/comptime_vm/program.h"

#include <deque>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <unordered_set>
#include <utility>

#include "src/bytecode_compiler/compile.h"
#include "src/hir/lower.h"
#include "src/hir/traversal.h"
#include "src/intrinsics.h"

namespace cinder::comptime_vm {

namespace {

[[nodiscard]] auto unsupported(std::string message)
    -> std::unexpected<semantic::comptime_failure> {
  return std::unexpected(semantic::comptime_failure{
      .reason = semantic::comptime_failure::kind::unsupported,
      .message = std::move(message),
      .frames = {}});
}

[[nodiscard]] auto key_of(std::string_view module, std::string_view name)
    -> std::string {
  return std::format("{}::{}", module, name);
}

} // namespace

auto hir_fingerprint(hir::hir_function &fn) -> std::string {
  auto out = std::string{};
  for (const auto &param : fn.params) {
    out += std::format("param {} {};", param.name, param.type);
  }
  out += std::format("returns {};", fn.return_type);
  hir::walk(*fn.body, [&out](hir::hir_node &node) -> void {
    out += std::format("{}", static_cast<int>(node.kind));
    if (const auto *expr = dynamic_cast<const hir::hir_expr *>(&node)) {
      out += std::format(":{}", expr->type);
    }
    switch (node.kind) {
    case hir::hir_node_kind::hir_literal:
      out += " " + static_cast<const hir::hir_literal &>(node).value;
      break;
    case hir::hir_node_kind::hir_local_ref: {
      const auto &ref = static_cast<const hir::hir_local_ref &>(node);
      out += std::format(" {}@{}", ref.name, ref.owner_module.value_or(""));
      break;
    }
    case hir::hir_node_kind::hir_global_ref:
      out += " " + static_cast<const hir::hir_global_ref &>(node).name;
      break;
    case hir::hir_node_kind::hir_field:
      out += " ." + static_cast<const hir::hir_field &>(node).field_name;
      break;
    case hir::hir_node_kind::hir_binary:
      out += std::format(
          " op{}",
          static_cast<int>(static_cast<const hir::hir_binary &>(node).op));
      break;
    case hir::hir_node_kind::hir_unary:
      out += std::format(
          " op{}",
          static_cast<int>(static_cast<const hir::hir_unary &>(node).op));
      break;
    case hir::hir_node_kind::hir_call: {
      const auto &call = static_cast<const hir::hir_call &>(node);
      out += std::format(" ->{}{}",
                         call.target != nullptr ? call.target->name : "",
                         call.is_tail_call ? " tail" : "");
      break;
    }
    default:
      break;
    }
    out += ";";
  });
  return out;
}

auto compile_program(semantic::comptime_program &program,
                     const semantic::comptime_function &entry)
    -> std::expected<compiled_program, semantic::comptime_failure> {
  auto result = compiled_program{};
  // Lowered functions grouped by module, the entry's module first: the
  // bytecode compiler names the entry module's functions bare and every
  // other module's `module::name`, which is how lowered calls name them.
  auto module_order = std::vector<std::string>{entry.owner_module};
  auto by_module = std::map<std::string, hir::ptr_vec<hir::hir_function>>{};
  // Functions a lowered body names but that could not be made ready, with
  // why — reported only if compiling then finds a call to one.
  auto skipped = std::map<std::string, std::string>{};
  auto queued = std::unordered_set<std::string>{
      key_of(entry.owner_module, entry.hir_name)};
  auto pending = std::deque<semantic::comptime_function>{entry};
  const auto is_entry = [&entry](const semantic::comptime_function &fn) {
    return fn.decl == entry.decl;
  };

  // Every reified `static let` the lowered code reads, built once before the
  // call runs (`bytecode_module::static_init_function`). Its value can call a
  // `from_array` constructor, which is lowered like any other function.
  auto globals = std::vector<hir::hir_static_global>{};
  auto global_names = std::unordered_set<std::string>{};
  auto global_failure = std::optional<std::string>{};
  std::function<void(const std::string &)> add_global;
  // Every name lowered code refers to that is not one of its own locals is
  // a function in its module or the one it names. A local happens to be
  // looked up too; it finds nothing, or a function nothing calls.
  const auto enqueue_calls = [&](hir::hir_node &root,
                                 const std::string &module_name) -> void {
    hir::walk(root, [&](hir::hir_node &node) -> void {
      if (node.kind == hir::hir_node_kind::hir_global_ref) {
        add_global(static_cast<const hir::hir_global_ref &>(node).name);
        return;
      }
      if (node.kind != hir::hir_node_kind::hir_local_ref) {
        return;
      }
      const auto &ref = static_cast<const hir::hir_local_ref &>(node);
      if (ref.name.empty() || is_known_intrinsic(ref.name)) {
        return;
      }
      const auto &module = ref.owner_module.value_or(module_name);
      if (!queued.insert(key_of(module, ref.name)).second) {
        return;
      }
      if (auto callee = program.find_function(module, ref.name)) {
        pending.push_back(std::move(*callee));
      }
    });
  };
  add_global = [&](const std::string &name) -> void {
    if (!global_names.insert(name).second) {
      return;
    }
    const auto *def = program.static_global(name);
    if (def == nullptr) {
      global_failure = std::format("the static `{}` has no value", name);
      return;
    }
    auto lowered =
        hir::lower_static_global(*def, program.view(), source_span{});
    if (!lowered.has_value()) {
      global_failure = lowered.error().message;
      return;
    }
    enqueue_calls(*lowered->value, entry.owner_module);
    globals.push_back(std::move(*lowered));
  };

  while (!pending.empty()) {
    const auto fn = std::move(pending.front());
    pending.pop_front();
    if (fn.decl == nullptr || fn.decl->modifiers.is_intrinsic) {
      continue;
    }
    const auto key = key_of(fn.owner_module, fn.hir_name);
    if (auto ready = program.prepare(*fn.decl); !ready.has_value()) {
      if (is_entry(fn)) {
        return unsupported(std::format("`{}` cannot run on the VM: {}",
                                       fn.hir_name, ready.error()));
      }
      skipped.emplace(key, ready.error());
      continue;
    }
    auto lowered = hir::lower_function(*fn.decl, program.view());
    if (!lowered.has_value()) {
      if (is_entry(fn)) {
        return unsupported(
            std::format("`{}` cannot be lowered: {} (at bytes {}..{})",
                        fn.hir_name, lowered.error().message,
                        lowered.error().span.start, lowered.error().span.end));
      }
      skipped.emplace(key, std::format("{} (at bytes {}..{})",
                                       lowered.error().message,
                                       lowered.error().span.start,
                                       lowered.error().span.end));
      continue;
    }
    auto &function = **lowered;
    function.name = fn.hir_name;
    if (is_entry(fn)) {
      for (const auto &param : function.params) {
        result.param_types.push_back(param.type);
      }
      result.return_type = function.return_type;
    }
    result.functions.emplace(key, fn.decl);
    result.fingerprints.emplace(fn.decl, hir_fingerprint(function));
    enqueue_calls(*function.body, fn.owner_module);
    if (!by_module.contains(fn.owner_module)) {
      module_order.push_back(fn.owner_module);
    }
    by_module[fn.owner_module].push_back(std::move(*lowered));
  }

  if (global_failure.has_value()) {
    return unsupported(*global_failure);
  }
  auto modules = hir::ptr_vec<hir::hir_module>{};
  auto seen_modules = std::unordered_set<std::string>{};
  for (const auto &name : module_order) {
    if (!seen_modules.insert(name).second) {
      continue;
    }
    auto found = by_module.find(name);
    auto functions = found != by_module.end()
                         ? std::move(found->second)
                         : hir::ptr_vec<hir::hir_function>{};
    modules.push_back(
        hir::make<hir::hir_module>(source_span{}, name, std::move(functions)));
  }
  modules.front()->statics = std::move(globals);
  auto module_pointers = std::vector<const hir::hir_module *>{};
  for (const auto &module : modules) {
    module_pointers.push_back(module.get());
  }
  auto compiled =
      bytecode_compiler::compile_module(module_pointers, program.view().types);
  if (!compiled.has_value()) {
    auto message = compiled.error().message;
    for (const auto &[name, why] : skipped) {
      message += std::format("; `{}` could not be prepared: {}", name, why);
    }
    return unsupported(std::move(message));
  }
  result.module = std::move(*compiled);
  auto found_entry = false;
  for (std::size_t i = 0; i < result.module.functions.size(); ++i) {
    if (result.module.functions[i].name == entry.hir_name) {
      result.entry_index = static_cast<std::uint16_t>(i);
      found_entry = true;
      break;
    }
  }
  if (!found_entry) {
    return unsupported(std::format("`{}` was not compiled", entry.hir_name));
  }
  return result;
}

} // namespace cinder::comptime_vm
