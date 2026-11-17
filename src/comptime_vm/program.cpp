#include "src/comptime_vm/program.h"

#include <cstdlib>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "src/bytecode_compiler/storage_compile.h"
#include "src/core/adapter.h"
#include "src/core/verify.h"
#include "src/hir/lower.h"
#include "src/hir/lower_core.h"
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

/// How compile-time code is lowered: able to work on quoted syntax.
const auto k_compile_time =
    hir::lowering_options{.contract_checks = true, .compile_time = true};

[[nodiscard]] auto key_of(std::string_view module, std::string_view name)
    -> std::string {
  return std::format("{}::{}", module, name);
}

/// How the adapter is told to build compile-time code.
const auto k_adapt_compile_time =
    core::adapt_options{.contract_checks = true, .compile_time = true};

/// `CINDER_COMPTIME_CORE_LOG=<file>` appends one line per function,
/// expression and static built for compile-time code from the core: `ok` or
/// `fail`, what it is, and for a failure why. It is how the core path's
/// coverage is counted across a test run.
auto log_core(std::string_view outcome, std::string_view what,
              std::string_view name, std::string_view why = {}) -> void {
  static const auto *const path = std::getenv("CINDER_COMPTIME_CORE_LOG");
  if (path == nullptr) {
    return;
  }
  auto out = std::ofstream(path, std::ios::app);
  out << outcome << '\t' << what << '\t' << name << '\t' << why << '\n';
}

[[nodiscard]] auto describe(const core::gap &gap) -> std::string {
  return std::format("the core adapter has no form for it: {} — {} (at bytes "
                     "{}..{})",
                     gap.construct, gap.detail, gap.span.start, gap.span.end);
}

[[nodiscard]] auto describe(const hir::unsupported &why) -> std::string {
  return std::format("the core lowering does not support {} (at bytes {}..{})",
                     why.kind, why.span.start, why.span.end);
}

/// What the core path keeps while it builds one program: the bare names it
/// looked up, and, when the core is verified, the core it built.
struct core_build {
  comptime_lowering mode;
  core::function_lookup lookup;
  /// The core program, by module name.
  std::map<std::string, core::module> modules;
  /// The names of the `intrinsic def`s already added to `modules`.
  std::unordered_set<std::string> intrinsics;

  auto module_named(const std::string &name) -> core::module & {
    auto &module = modules[name];
    module.name = name;
    return module;
  }
};

[[nodiscard]] auto make_lookup(semantic::comptime_program &program,
                               lookup_memo *memo) -> core::function_lookup {
  return [&program, memo](std::string_view module,
                          std::string_view name) -> const ast::func_decl * {
    const auto found = program.find_function(module, name);
    const auto *decl = found.has_value() ? found->decl : nullptr;
    // A call named by name alone (one the checker ran at compile time, or an
    // `intrinsic def` in the same module) has no callee record: the callee's
    // signature reaches the view once it is prepared, which assembling does
    // next anyway. One that cannot be prepared is reported when lowered.
    if (decl != nullptr) {
      static_cast<void>(program.prepare(*decl));
    }
    if (memo != nullptr) {
      memo->insert_or_assign(key_of(module, name), decl);
    }
    return decl;
  };
}

/// `decl`, named `name` in `module`, built by the adapter and lowered from
/// the core. The error says why not.
[[nodiscard]] auto lower_through_core(semantic::comptime_program &program,
                                      core_build &build,
                                      const ast::func_decl &decl,
                                      const std::string &module,
                                      const std::string &name)
    -> std::expected<hir::ptr<hir::hir_function>, std::string> {
  const auto key = key_of(module, name);
  auto built = core::adapt_function(decl, name, module, program.records(),
                                    build.lookup, k_adapt_compile_time);
  if (!built.has_value()) {
    log_core("fail", "function", key,
             std::format("{}\t{}\t{}", built.error().construct,
                         built.error().detail, built.error().span.start));
    return std::unexpected(describe(built.error()));
  }
  auto lowered =
      hir::lower_core_comptime_function(**built, module, program.view());
  if (!lowered.has_value()) {
    log_core("fail", "function", key, "lowering: " + lowered.error().kind);
    return std::unexpected(describe(lowered.error()));
  }
  log_core("ok", "function", key);
  if (build.mode.verify_core) {
    build.module_named(module).functions.push_back(std::move(*built));
  }
  return std::move(*lowered);
}

/// The reified static `def`, owned by `module`, built by the adapter and
/// lowered from the core.
[[nodiscard]] auto
static_through_core(semantic::comptime_program &program, core_build &build,
                    const semantic::lowering_results::static_global_def &def,
                    const std::string &module)
    -> std::expected<hir::hir_static_global, std::string> {
  auto built = core::adapt_static(def, module, program.records(), {});
  if (!built.has_value()) {
    log_core("fail", "static", def.name, built.error().construct);
    return std::unexpected(describe(built.error()));
  }
  auto lowered = hir::lower_core_static(*built);
  if (!lowered.has_value()) {
    log_core("fail", "static", def.name, "lowering: " + lowered.error().kind);
    return std::unexpected(describe(lowered.error()));
  }
  log_core("ok", "static", def.name);
  if (build.mode.verify_core) {
    build.module_named(module).statics.push_back(std::move(*built));
  }
  return std::move(*lowered);
}

/// Adds the `intrinsic def` lowered code calls as `name` in `module`, if it
/// is one, to the core program the verifier checks (rule C5 looks calls to
/// one up there).
auto add_intrinsic(semantic::comptime_program &program, core_build &build,
                   const std::string &module, const std::string &name)
    -> void {
  if (!build.mode.verify_core || build.intrinsics.contains(name)) {
    return;
  }
  const auto found = program.find_function(module, name);
  if (!found.has_value() || found->decl == nullptr ||
      !found->decl->modifiers.is_intrinsic) {
    return;
  }
  build.intrinsics.insert(name);
  if (auto made = core::adapt_intrinsic(*found->decl, name, program.records());
      made.has_value()) {
    build.module_named(module).intrinsics.push_back(std::move(*made));
  }
}

/// The verifier's findings on the core `build` holds, as one message, or
/// nothing when it is well formed.
[[nodiscard]] auto verify_core(const core_build &build,
                               const semantic::checked_types &checked)
    -> std::optional<std::string> {
  auto program = std::vector<const core::module *>{};
  for (const auto &[name, module] : build.modules) {
    program.push_back(&module);
  }
  const auto violations = core::verify(program, checked);
  if (violations.empty()) {
    return std::nullopt;
  }
  auto message = std::string("the core built for compile-time code is not "
                             "well formed:");
  for (const auto &v : violations) {
    message += std::format(" [{}] {}.{}: {} (at bytes {}..{});", v.rule,
                           v.module, v.function, v.message, v.span.start,
                           v.span.end);
    log_core("fail", "verify", key_of(v.module, v.function), v.rule);
  }
  return message;
}

} // namespace

auto comptime_lowering::from_environment() -> comptime_lowering {
  const auto *mode = std::getenv("CINDER_LOWER_FROM_CORE");
  const auto from_core = mode != nullptr && std::string_view(mode) == "only";
  return comptime_lowering{
      .from_core = from_core,
      .verify_core = from_core && std::getenv("CINDER_CORE_VERIFY") != nullptr};
}

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
    case hir::hir_node_kind::hir_host_call:
      out += " " + static_cast<const hir::hir_host_call &>(node).operation;
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

namespace {

/// Lowers every function `entry` reaches, transitively, and compiles them
/// with it into one module. `entry` is already lowered, named `entry_name`
/// in `entry_module`; `entry_decl` is its declaration, if it has one.
[[nodiscard]] auto
assemble(semantic::comptime_program &program, const std::string &entry_module,
         const std::string &entry_name, const ast::func_decl *entry_decl,
         hir::ptr<hir::hir_function> entry, core_build *build)
    -> std::expected<compiled_program, semantic::comptime_failure> {
  auto result = compiled_program{};
  for (const auto &param : entry->params) {
    result.param_types.push_back(param.type);
  }
  result.return_type = entry->return_type;
  if (entry_decl != nullptr) {
    result.functions.emplace(key_of(entry_module, entry_name), entry_decl);
    result.fingerprints.emplace(entry_decl, hir_fingerprint(*entry));
    result.names.emplace(entry_decl,
                         lowered_name{.module = entry_module, .name = entry_name});
  }
  // Lowered functions grouped by module, the entry's module first: the
  // bytecode compiler names the entry module's functions bare and every
  // other module's `module::name`, which is how lowered calls name them.
  auto module_order = std::vector<std::string>{entry_module};
  auto by_module = std::map<std::string, hir::ptr_vec<hir::hir_function>>{};
  // Functions a lowered body names but that could not be made ready, with
  // why — reported only if compiling then finds a call to one.
  auto skipped = std::map<std::string, std::string>{};
  auto queued =
      std::unordered_set<std::string>{key_of(entry_module, entry_name)};
  auto pending = std::deque<semantic::comptime_function>{};

  // Every reified `static let` the lowered code reads, built once before the
  // call runs (`bytecode_module::static_initializers`). Its value can call a
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
      if (ref.name.empty()) {
        return;
      }
      const auto &module = ref.owner_module.value_or(module_name);
      if (build != nullptr) {
        add_intrinsic(program, *build, module, ref.name);
      }
      if (is_known_intrinsic(ref.name)) {
        return;
      }
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
    auto lowered = std::expected<hir::hir_static_global, std::string>{};
    if (build != nullptr) {
      const auto &owners = program.view().lowering.static_global_owners;
      const auto owner = owners.find(name);
      lowered = static_through_core(
          program, *build, *def,
          owner != owners.end() ? owner->second : entry_module);
    } else if (auto old =
                   hir::lower_static_global(*def, program.view(), source_span{});
               old.has_value()) {
      lowered = std::move(*old);
    } else {
      lowered = std::unexpected(old.error().message);
    }
    if (!lowered.has_value()) {
      global_failure = lowered.error();
      return;
    }
    enqueue_calls(*lowered->value, entry_module);
    globals.push_back(std::move(*lowered));
  };

  enqueue_calls(*entry->body, entry_module);
  by_module[entry_module].push_back(std::move(entry));
  while (!pending.empty()) {
    const auto fn = std::move(pending.front());
    pending.pop_front();
    if (fn.decl == nullptr || fn.decl->modifiers.is_intrinsic ||
        fn.decl == entry_decl) {
      continue;
    }
    const auto key = key_of(fn.owner_module, fn.hir_name);
    if (auto ready = program.prepare(*fn.decl); !ready.has_value()) {
      skipped.emplace(key, ready.error());
      continue;
    }
    auto lowered = std::expected<hir::ptr<hir::hir_function>, std::string>{};
    if (build != nullptr) {
      lowered = lower_through_core(program, *build, *fn.decl, fn.owner_module,
                                   fn.hir_name);
    } else if (auto old =
                   hir::lower_function(*fn.decl, program.view(), k_compile_time);
               old.has_value()) {
      lowered = std::move(*old);
    } else {
      lowered = std::unexpected(std::format("{} (at bytes {}..{})",
                                            old.error().message,
                                            old.error().span.start,
                                            old.error().span.end));
    }
    if (!lowered.has_value()) {
      skipped.emplace(key, lowered.error());
      continue;
    }
    auto &function = **lowered;
    function.name = fn.hir_name;
    result.functions.emplace(key, fn.decl);
    result.fingerprints.emplace(fn.decl, hir_fingerprint(function));
    result.names.emplace(fn.decl, lowered_name{.module = fn.owner_module,
                                               .name = fn.hir_name});
    enqueue_calls(*function.body, fn.owner_module);
    if (!by_module.contains(fn.owner_module)) {
      module_order.push_back(fn.owner_module);
    }
    by_module[fn.owner_module].push_back(std::move(*lowered));
  }

  if (global_failure.has_value()) {
    return unsupported(*global_failure);
  }
  if (build != nullptr && build->mode.verify_core) {
    if (auto broken = verify_core(*build, program.view())) {
      return unsupported(std::move(*broken));
    }
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
  // The same storage form the driver runs (spec/value-semantics.md): values
  // are inline bytes, and the compiler runs the bytecode built from it.
  auto storage =
      hir::prepare_storage_program(module_pointers, program.records());
  if (!storage.has_value()) {
    auto message = storage.error();
    for (const auto &[name, why] : skipped) {
      message += std::format("; `{}` could not be prepared: {}", name, why);
    }
    return unsupported(std::move(message));
  }
  auto compiled = bytecode_compiler::compile_storage_module(
      std::span<const hir::storage_module *const>((*storage)->modules),
      *(*storage)->env);
  if (!compiled.has_value()) {
    auto message = compiled.error().message;
    for (const auto &[name, why] : skipped) {
      message += std::format("; `{}` could not be prepared: {}", name, why);
    }
    return unsupported(std::move(message));
  }
  result.module = std::move(*compiled);
  result.storage = std::move(*storage);
  result.lowered = std::move(modules);
  auto found_entry = false;
  for (std::size_t i = 0; i < result.module.functions.size(); ++i) {
    if (result.module.functions[i].name == entry_name) {
      result.entry_index = static_cast<std::uint16_t>(i);
      found_entry = true;
      break;
    }
  }
  if (!found_entry) {
    return unsupported(std::format("`{}` was not compiled", entry_name));
  }
  return result;
}

} // namespace

auto compile_program(semantic::comptime_program &program,
                     const semantic::comptime_function &entry,
                     const comptime_lowering &mode, lookup_memo *memo)
    -> std::expected<compiled_program, semantic::comptime_failure> {
  if (entry.decl == nullptr) {
    return unsupported("the called function has no declaration");
  }
  if (auto ready = program.prepare(*entry.decl); !ready.has_value()) {
    return unsupported(std::format("`{}` cannot run on the VM: {}",
                                   entry.hir_name, ready.error()));
  }
  if (mode.from_core) {
    auto build = core_build{.mode = mode,
                            .lookup = make_lookup(program, memo),
                            .modules = {},
                            .intrinsics = {}};
    auto lowered = lower_through_core(program, build, *entry.decl,
                                      entry.owner_module, entry.hir_name);
    if (!lowered.has_value()) {
      return unsupported(std::format("`{}` cannot be lowered: {}",
                                     entry.hir_name, lowered.error()));
    }
    return assemble(program, entry.owner_module, entry.hir_name, entry.decl,
                    std::move(*lowered), &build);
  }
  auto lowered =
      hir::lower_function(*entry.decl, program.view(), k_compile_time);
  if (!lowered.has_value()) {
    return unsupported(
        std::format("`{}` cannot be lowered: {} (at bytes {}..{})",
                    entry.hir_name, lowered.error().message,
                    lowered.error().span.start, lowered.error().span.end));
  }
  (*lowered)->name = entry.hir_name;
  return assemble(program, entry.owner_module, entry.hir_name, entry.decl,
                  std::move(*lowered), nullptr);
}

auto compile_expression(semantic::comptime_program &program,
                        const semantic::comptime_expression &expr,
                        const comptime_lowering &mode, lookup_memo *memo)
    -> std::expected<compiled_program, semantic::comptime_failure> {
  if (expr.expr == nullptr) {
    return unsupported("there is no expression");
  }
  if (auto ready = program.prepare_expression(*expr.expr); !ready.has_value()) {
    return unsupported(
        std::format("the expression cannot run on the VM: {}", ready.error()));
  }
  if (mode.from_core) {
    auto build = core_build{.mode = mode,
                            .lookup = make_lookup(program, memo),
                            .modules = {},
                            .intrinsics = {}};
    auto captures = std::vector<core::expression_capture>{};
    captures.reserve(expr.captures.size());
    for (const auto &[name, type] : expr.captures) {
      captures.push_back(core::expression_capture{.name = name, .type = type});
    }
    const auto name = std::string(k_expression_function);
    const auto key = key_of(expr.owner_module, name);
    auto built = core::adapt_expression(*expr.expr, captures, name,
                                        expr.owner_module, program.records(),
                                        build.lookup);
    if (!built.has_value()) {
      log_core("fail", "expression", key,
               std::format("{}\t{}\t{}", built.error().construct,
                           built.error().detail, built.error().span.start));
      return unsupported(std::format("the expression cannot be lowered: {}",
                                     describe(built.error())));
    }
    auto lowered = hir::lower_core_comptime_function(
        **built, expr.owner_module, program.view());
    if (!lowered.has_value()) {
      log_core("fail", "expression", key, "lowering: " + lowered.error().kind);
      return unsupported(std::format("the expression cannot be lowered: {}",
                                     describe(lowered.error())));
    }
    log_core("ok", "expression", key);
    if (mode.verify_core) {
      build.module_named(expr.owner_module).functions.push_back(
          std::move(*built));
    }
    return assemble(program, expr.owner_module, name, nullptr,
                    std::move(*lowered), &build);
  }
  auto captures = std::vector<hir::comptime_capture>{};
  captures.reserve(expr.captures.size());
  for (const auto &[name, type] : expr.captures) {
    captures.push_back(hir::comptime_capture{.name = name, .type = type});
  }
  auto lowered = hir::lower_comptime_expression(
      *expr.expr, captures, std::string(k_expression_function), program.view());
  if (!lowered.has_value()) {
    return unsupported(
        std::format("the expression cannot be lowered: {} (at bytes {}..{})",
                    lowered.error().message, lowered.error().span.start,
                    lowered.error().span.end));
  }
  return assemble(program, expr.owner_module,
                  std::string(k_expression_function), nullptr,
                  std::move(*lowered), nullptr);
}

auto lower_from_core_again(const ast::func_decl &decl, const lowered_name &name,
                           semantic::checked_types &checked,
                           const lookup_memo &memo)
    -> std::expected<hir::ptr<hir::hir_function>, std::string> {
  const auto lookup = core::function_lookup(
      [&memo](std::string_view module,
              std::string_view bare) -> const ast::func_decl * {
        const auto found = memo.find(key_of(module, bare));
        return found == memo.end() ? nullptr : found->second;
      });
  auto built = core::adapt_function(decl, name.name, name.module, checked,
                                    lookup, k_adapt_compile_time);
  if (!built.has_value()) {
    return std::unexpected(describe(built.error()));
  }
  auto lowered =
      hir::lower_core_comptime_function(**built, name.module, checked);
  if (!lowered.has_value()) {
    return std::unexpected(describe(lowered.error()));
  }
  return std::move(*lowered);
}

} // namespace cinder::comptime_vm
