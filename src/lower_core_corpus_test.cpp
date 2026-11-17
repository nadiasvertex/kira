// Runs the codegen stress corpus (`codegen_corpus.h`) with each function
// lowered from the typed core where `hir::lower_core_function` supports it,
// and checks everything `codegen_stress_test` checks, on both tiers. Every
// other function keeps what the old lowerer made, so a module mixes both.
//
// It reports the share of functions lowered from the core and why the rest
// fell back, and fails when that share drops below `k_from_core_floor`.
//
// `CINDER_LOWER_CORE_FILTER` runs only the files whose name starts with it.
// `CINDER_LOWER_CORE_DIFF=1` prints each function whose core-lowered HIR
// differs from the old lowerer's, with both renderings.
// `CINDER_LOWER_CORE_DIFF=own` prints only the program's own functions.
// `CINDER_LOWER_CORE_LIST=1` prints each function that fell back, and why.

#include <cstdlib>
#include <format>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/codegen_corpus.h"
#include "src/core/adapter.h"
#include "src/hir/lower_core.h"
#include "src/hir/traversal.h"
#include "src/testing/test_assert.h"

namespace {

namespace hir = cinder::hir;
namespace corpus = cinder::testing::codegen_corpus;
using cinder::testing::expect;

/// The functions lowered from the core, counted once per corpus program for
/// the program's own modules and once in all for the standard library's.
constexpr auto k_from_core_floor = std::size_t{1681};

/// A structural rendering of a HIR function, with symbols numbered in the
/// order they appear, so two lowerings of one function can be compared.
class renderer {
public:
  renderer(std::string module, const cinder::semantic::type_table &types)
      : module_(std::move(module)), types_(types) {}

  auto function(const hir::hir_function &fn) -> std::string {
    auto out = std::format("fn {} ret={} owns={} (", fn.name, fn.return_type,
                           fn.owns_receiver);
    for (const auto &param : fn.params) {
      out += std::format("{}:{} ", symbol(param.symbol), param.type);
    }
    out += ")\n";
    node(*fn.body, 1, out);
    return out;
  }

private:
  auto symbol(hir::symbol_id id) -> std::size_t {
    return symbols_.try_emplace(id, symbols_.size()).first->second;
  }

  auto node(const hir::hir_node &n, int depth, std::string &out,
            bool callee = false) -> void {
    using k = hir::hir_node_kind;
    // A cast that changes nothing, or only a raw pointer's mutability, is
    // the same operation either way.
    if (n.kind == k::hir_cast) {
      const auto &operand = *dynamic_cast<const hir::hir_cast &>(n).operand;
      const auto is_ptr = [&](cinder::semantic::type_id t) {
        return types_.entry(t).kind == cinder::semantic::type_kind::ptr_kind;
      };
      if (operand.type == n.type || (is_ptr(operand.type) && is_ptr(n.type))) {
        node(operand, depth, out, callee);
        return;
      }
    }
    out += std::string(static_cast<std::size_t>(depth) * 2, ' ');
    // A callee's own type is not read for a direct call.
    out += callee ? std::format("k{}", static_cast<int>(n.kind))
                  : std::format("k{} t{}", static_cast<int>(n.kind), n.type);
    if (n.continues_statement) {
      out += " cont";
    }
    switch (n.kind) {
    case k::hir_literal:
      out += " " + dynamic_cast<const hir::hir_literal &>(n).value;
      break;
    case k::hir_local_ref: {
      const auto &ref = dynamic_cast<const hir::hir_local_ref &>(n);
      out += std::format(" s{} {}", symbol(ref.symbol), ref.name);
      if (ref.owner_module.has_value() && *ref.owner_module != module_) {
        out += " @" + *ref.owner_module;
      }
      break;
    }
    case k::hir_binary:
      out += std::format(" op{}", static_cast<int>(
                                      dynamic_cast<const hir::hir_binary &>(n).op));
      break;
    case k::hir_unary:
      out += std::format(" op{}", static_cast<int>(
                                      dynamic_cast<const hir::hir_unary &>(n).op));
      break;
    case k::hir_call: {
      const auto &call = dynamic_cast<const hir::hir_call &>(n);
      out += std::format(" tail={} consumes={} by_value={} drop={}",
                         call.is_tail_call, call.consumes_receiver,
                         call.receiver_by_value, call.is_drop);
      break;
    }
    case k::hir_field:
      out += " ." + dynamic_cast<const hir::hir_field &>(n).field_name;
      break;
    case k::hir_tuple_index:
      out += std::format(" .{}", dynamic_cast<const hir::hir_tuple_index &>(n).index);
      break;
    case k::hir_variant_init:
      out += " @" + dynamic_cast<const hir::hir_variant_init &>(n).variant_name;
      break;
    case k::hir_block:
      out += std::format(" exit{}", static_cast<int>(
                                        dynamic_cast<const hir::hir_block &>(n).exit));
      break;
    case k::hir_let:
      out += std::format(" s{}", symbol(dynamic_cast<const hir::hir_let &>(n).symbol));
      break;
    case k::hir_assign:
      out += std::format(" op{}", static_cast<int>(
                                      dynamic_cast<const hir::hir_assign &>(n).op));
      break;
    case k::hir_yield: {
      const auto &yield = dynamic_cast<const hir::hir_yield &>(n);
      out += std::format(" awaits={} all={}", yield.awaits, yield.awaits_all);
      break;
    }
    case k::hir_contract_check: {
      const auto &check = dynamic_cast<const hir::hir_contract_check &>(n);
      out += std::format(" clause{} \"{}\"", static_cast<int>(check.kind),
                         check.message);
      break;
    }
    case k::hir_lambda: {
      const auto &closure = dynamic_cast<const hir::hir_lambda &>(n);
      if (!closure.captures.has_value()) {
        out += " implicit";
        break;
      }
      for (const auto &capture : *closure.captures) {
        out += std::format(" s{}{}", symbol(capture.symbol),
                           cinder::ast::is_reference_capture(capture.mode)
                               ? "&"
                               : "");
      }
      break;
    }
    case k::hir_struct_init:
      for (const auto &field : dynamic_cast<const hir::hir_struct_init &>(n).fields) {
        out += " " + field.name;
      }
      break;
    default:
      break;
    }
    out += "\n";
    const auto *call = dynamic_cast<const hir::hir_call *>(&n);
    hir::for_each_child(const_cast<hir::hir_node &>(n), [&](auto &child) {
      node(*child, depth + 1, out,
           call != nullptr &&
               static_cast<const void *>(child.get()) ==
                   static_cast<const void *>(call->callee.get()));
    });
  }

  std::string module_;
  const cinder::semantic::type_table &types_;
  std::unordered_map<hir::symbol_id, std::size_t> symbols_;
};

struct tally {
  std::size_t from_core = 0;
  std::size_t total = 0;
  std::map<std::string, std::size_t> fallbacks;
};

struct coverage {
  tally program;
  tally stdlib;
  std::set<std::string> stdlib_seen;
  std::size_t differing = 0;
};

auto print_tally(const char *what, const tally &t) -> void {
  std::cout << std::format("  {}: {} / {} functions lowered from the core\n",
                           what, t.from_core, t.total);
  auto ordered = std::vector<std::pair<std::size_t, std::string>>{};
  for (const auto &[reason, count] : t.fallbacks) {
    ordered.emplace_back(count, reason);
  }
  std::ranges::sort(ordered, std::greater{});
  for (const auto &[count, reason] : ordered) {
    std::cout << std::format("    {:6}  {}\n", count, reason);
  }
}

} // namespace

/// A file's `module` path, as the corpus runner names a library module.
auto module_name_of(const cinder::ast::file &file) -> std::string {
  auto name = std::string{};
  if (file.module_decl != nullptr) {
    for (const auto &segment : file.module_decl->path) {
      name += name.empty() ? segment : "." + segment;
    }
  }
  return name;
}

/// `CINDER_LOWER_FROM_CORE=only`: replaces every module the old lowerer
/// made with the one `hir::lower_core_program` makes, in the same order, and
/// fails when a module differs in which functions or statics it holds, in
/// their order, or could not be made.
auto replace_with_core_program(corpus::post_lower_context &context,
                               const cinder::core::adapted_program &adapted,
                               coverage &seen, bool list) -> void {
  auto program = std::vector<const cinder::core::module *>{};
  for (const auto &module : adapted.modules) {
    program.push_back(module.get());
  }
  auto input = hir::core_lowering_input{.modules = program,
                                        .file_module_names = {}};
  for (const auto &parsed : context.parsed) {
    input.file_module_names.emplace(
        parsed.file_id, parsed.file_id == context.file_id
                            ? context.module_name
                            : module_name_of(*parsed.ast_file));
  }
  auto lowered =
      hir::lower_core_program(input, adapted.gaps, context.checked, {});
  for (const auto &failure : lowered.failures) {
    std::cout << std::format("only: module `{}` not lowered: {}\n",
                             failure.module, failure.message);
  }
  expect(lowered.failures.empty(),
         std::format("expected every module to lower from the core alone ({} "
                     "failure(s))",
                     lowered.failures.size()));
  // Several files may hold one module: each is lowered on its own, in order.
  auto by_name = std::multimap<std::string, hir::ptr<hir::hir_module>>{};
  for (auto &module : lowered.modules) {
    auto name = module->module_name;
    by_name.emplace(std::move(name), std::move(module));
  }
  const auto names_of = [](const hir::hir_module &module) {
    auto out = std::vector<std::string>{};
    for (const auto &function : module.functions) {
      out.push_back(function->name);
    }
    for (const auto &global : module.statics) {
      out.push_back("static " + global.name);
    }
    return out;
  };
  if (std::getenv("CINDER_LOWER_CORE_MODULES") != nullptr) {
    for (const auto &module : context.modules) {
      std::cout << std::format("old {} {}\n", module->module_name,
                               names_of(*module).size() > 0 ? names_of(*module).front() : "");
    }
    for (const auto &[name, module] : by_name) {
      std::cout << std::format("core {} {}\n", name,
                               names_of(*module).size() > 0 ? names_of(*module).front() : "");
    }
  }
  for (auto &module : context.modules) {
    const auto found = by_name.lower_bound(module->module_name);
    expect(found != by_name.end() && found->first == module->module_name,
           std::format("expected the core to make the module `{}`",
                       module->module_name));
    const auto before = names_of(*module);
    const auto after = names_of(*found->second);
    if (before != after) {
      auto text = std::string{};
      for (const auto &name : before) {
        text += " " + name;
      }
      text += "\n  core:";
      for (const auto &name : after) {
        text += " " + name;
      }
      cinder::testing::fail(std::format(
          "module `{}` holds other functions from the core\n  old:{}",
          module->module_name, text));
    }
    found->second->file_id = module->file_id;
    module = std::move(found->second);
    by_name.erase(found);
  }
  // Modules the old path does not lower here (functor instantiations).
  for (auto &[name, module] : by_name) {
    context.modules.push_back(std::move(module));
  }
  if (context.inline_calls) {
    return;
  }
  for (const auto &record : lowered.records) {
    const auto own = record.module == context.module_name ||
                     record.module.starts_with(context.module_name + ".");
    if (!own && !seen.stdlib_seen.insert(record.module + "::" + record.function)
                     .second) {
      continue;
    }
    auto &t = own ? seen.program : seen.stdlib;
    ++t.total;
    if (record.from_core) {
      ++t.from_core;
    } else {
      ++t.fallbacks[record.fallback];
      if (list) {
        std::cout << std::format("fallback {}::{}: {}\n", record.module,
                                 record.function, record.fallback);
      }
    }
  }
}

auto main() -> int {
  const auto *prefix = std::getenv("CINDER_LOWER_CORE_FILTER");
  const auto *mode = std::getenv("CINDER_LOWER_FROM_CORE");
  const auto only = mode != nullptr && std::string_view(mode) == "only";
  const auto diff = std::getenv("CINDER_LOWER_CORE_DIFF") != nullptr;
  const auto list = std::getenv("CINDER_LOWER_CORE_LIST") != nullptr;
  const auto own_only =
      diff && std::string_view(std::getenv("CINDER_LOWER_CORE_DIFF")) == "own";
  auto seen = coverage{};
  auto options = corpus::corpus_options{
      .name = "lower_core_corpus_test", .include = {}, .hook = {}};
  if (prefix != nullptr && *prefix != '\0') {
    options.include = [prefix](const std::string &name) -> bool {
      return name.starts_with(prefix);
    };
  }
  options.hook = [&](corpus::post_lower_context &context) -> void {
    auto adapted = cinder::core::adapt_program(
        context.parsed, context.file_has_errors, context.checked);
    if (only) {
      replace_with_core_program(context, adapted, seen, list);
      return;
    }
    auto program = std::vector<const cinder::core::module *>{};
    for (const auto &module : adapted.modules) {
      program.push_back(module.get());
    }
    auto input = hir::core_lowering_input{.modules = program,
                                          .file_module_names = {}};
    input.file_module_names.emplace(context.file_id, context.module_name);
    const auto on_replace = [&](const hir::hir_module &module,
                                const hir::hir_function &old_lowered,
                                const hir::hir_function &made) -> void {
      if (!diff || context.inline_calls) {
        return;
      }
      // `CINDER_LOWER_CORE_DIFF=own`: the program's own functions only.
      if (own_only && module.module_name != context.module_name &&
          !module.module_name.starts_with(context.module_name + ".")) {
        return;
      }
      const auto before = renderer{module.module_name, context.checked.types}.function(
          old_lowered);
      const auto after = renderer{module.module_name, context.checked.types}.function(made);
      if (before != after) {
        ++seen.differing;
        std::cout << std::format("--- {} (old)\n{}+++ (core)\n{}\n",
                                 old_lowered.name, before, after);
      }
    };
    const auto records = hir::substitute_core_functions(
        context.modules, input, context.checked, {}, on_replace);
    if (context.inline_calls) {
      return;
    }
    for (const auto &record : records) {
      const auto own = record.module == context.module_name ||
                       record.module.starts_with(context.module_name + ".");
      if (!own && !seen.stdlib_seen
                       .insert(record.module + "::" + record.function)
                       .second) {
        continue;
      }
      auto &t = own ? seen.program : seen.stdlib;
      ++t.total;
      if (record.from_core) {
        ++t.from_core;
      } else {
        ++t.fallbacks[record.fallback];
        if (list) {
          std::cout << std::format("fallback {}::{}: {}\n", record.module,
                                   record.function, record.fallback);
        }
      }
    }
  };
  corpus::run_corpus(options);
  std::cout << std::format("lower_core_corpus_test coverage{}:\n",
                           only ? " (modules from the core alone)" : "");
  print_tally("corpus programs", seen.program);
  print_tally("standard library", seen.stdlib);
  if (diff) {
    std::cout << std::format("  {} function(s) differ structurally\n",
                             seen.differing);
  }
  const auto from_core = seen.program.from_core + seen.stdlib.from_core;
  if (options.include == nullptr) {
    expect(from_core >= k_from_core_floor,
           std::format("expected at least {} functions lowered from the core, "
                       "found {}",
                       k_from_core_floor, from_core));
  }
  return 0;
}
