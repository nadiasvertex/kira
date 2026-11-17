#include "src/core/adapter.h"

#include <algorithm>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/core/adapter_builder.h"
#include "src/parser/ast_traversal.h"
#include "src/semantic/program_index.h"

namespace cinder::core {

namespace {

/// The bare target-type name an `impl`/`extend` block's methods are named
/// under (`Target::method`), the name HIR knows them by. Any other target
/// shape has no methods to lower, so it is not enumerated here.
///
/// A generic block (`extend[T] list[T]`) has no instance of its own to
/// lower, so lowering skips it whatever its target; its methods are
/// templates, named `list::push` after the target's bare name.
[[nodiscard]] auto simple_target_name(const ast::type_expr *for_type,
                                      bool generic_block)
    -> std::optional<std::string> {
  if (for_type == nullptr || for_type->kind != ast::node_kind::named_type) {
    return std::nullopt;
  }
  const auto &named = ast::cast<const ast::named_type>(*for_type);
  if (named.path.empty() || (!named.type_args.empty() && !generic_block)) {
    return std::nullopt;
  }
  return named.path.back();
}

[[nodiscard]] auto is_generic_template(const ast::func_decl &decl) -> bool {
  return !decl.type_params.empty() &&
         std::ranges::all_of(decl.type_params, [](const auto &param) -> bool {
           return !param.name.empty();
         });
}

/// What kind of generic template a planned function is.
enum class template_kind : std::uint8_t {
  none,
  /// Declared type or value parameters, its own or its block's.
  declared,
  /// An unannotated parameter the body leaves open (`open_param_templates`).
  implicit,
};

/// One function the module will hold, before its body is built.
struct planned_function {
  const ast::func_decl *decl = nullptr;
  std::string name;
  bool generated = false;
  template_kind generic = template_kind::none;
  /// The enclosing generic `impl`/`extend` block's parameters.
  const std::vector<ast::type_param> *block_params = nullptr;
};

/// An `intrinsic def` a module declares, before its signature is built.
struct planned_intrinsic {
  const ast::func_decl *decl = nullptr;
  std::string name;
  const std::vector<ast::type_param> *block_params = nullptr;
};

/// Everything one module will hold.
struct module_plan {
  std::string name;
  std::optional<file_id_type> file_id;
  std::vector<planned_function> functions;
  std::vector<const ast::static_decl *> statics;
  std::vector<planned_intrinsic> intrinsics;
};

/// Enumerates the functions of each module, its synthesized items, its inline
/// submodules and the functor instances, under the names and owner modules
/// HIR knows them by.
class module_planner {
public:
  explicit module_planner(const semantic::checked_types &checked)
      : checked_(checked) {}

  auto plan_items(const std::vector<ast::ptr<ast::node>> &items,
                  module_plan &plan) const -> void {
    auto &out = plan.functions;
    auto &statics = plan.statics;
    for (const auto &item : items) {
      if (item == nullptr) {
        continue;
      }
      switch (item->kind) {
      case ast::node_kind::static_decl: {
        const auto &decl = ast::cast<const ast::static_decl>(*item);
        if (checked_.lowering.static_global_defs.contains(&decl)) {
          statics.push_back(&decl);
        }
        break;
      }
      case ast::node_kind::impl_decl:
        plan_impl(ast::cast<const ast::impl_decl>(*item), false, plan);
        break;
      case ast::node_kind::extend_decl:
        plan_extend(ast::cast<const ast::extend_decl>(*item), plan);
        break;
      case ast::node_kind::func_decl: {
        const auto &decl = ast::cast<const ast::func_decl>(*item);
        if (decl.modifiers.is_intrinsic) {
          plan.intrinsics.push_back({.decl = &decl, .name = decl.name});
          break;
        }
        if (decl.modifiers.is_static) {
          // Compile-time only, generic or not: no runtime body to hold.
          break;
        }
        if (checked_.lowering.open_param_templates.contains(&decl)) {
          out.push_back({.decl = &decl,
                         .name = decl.name,
                         .generic = template_kind::implicit});
          break;
        }
        if (is_generic_template(decl)) {
          out.push_back({.decl = &decl,
                         .name = decl.name,
                         .generic = template_kind::declared});
          break;
        }
        out.push_back({.decl = &decl, .name = decl.name});
        break;
      }
      default:
        break;
      }
    }
  }

  auto plan_synthesized(const std::string &module_name,
                        module_plan &plan) const -> void {
    auto &out = plan.functions;
    for (const auto &method : checked_.lowering.synthesized_trait_defaults) {
      if (method.owner_module == module_name && method.decl != nullptr) {
        // A generic impl's copy is a template over the impl's parameters,
        // as the impl's written methods are.
        out.push_back(
            {.decl = method.decl,
             .name = std::format("{}::{}", method.target_type_name,
                                 method.decl->name),
             .generic = method.block_type_params != nullptr
                            ? template_kind::declared
                            : template_kind::none,
             .block_params = method.block_type_params});
      }
    }
    for (const auto &instance : checked_.lowering.const_generic_instances) {
      if (instance.owner_module != module_name || instance.decl == nullptr ||
          checked_.lowering.comptime_only_functions.contains(instance.decl)) {
        continue;
      }
      out.push_back({.decl = instance.decl, .name = instance.decl->name});
    }
    for (const auto &splice : checked_.lowering.synthesized_item_splices) {
      if (splice.owner_module == module_name && splice.impl != nullptr) {
        plan_impl(*splice.impl, true, plan);
      }
    }
  }

private:
  /// The methods of an `impl`/`extend` block named `target`. A block with
  /// type parameters makes every method a template.
  auto plan_block(const std::vector<ast::ptr<ast::node>> &items,
                  const std::vector<ast::type_param> &block_params,
                  const std::string &target, bool generated,
                  module_plan &plan) const -> void {
    const auto generic_block = !block_params.empty();
    for (const auto &item : items) {
      if (item == nullptr || item->kind != ast::node_kind::func_decl) {
        continue;
      }
      const auto &decl = ast::cast<const ast::func_decl>(*item);
      const auto name = std::format("{}::{}", target, decl.name);
      if (decl.modifiers.is_intrinsic) {
        plan.intrinsics.push_back(
            {.decl = &decl,
             .name = name,
             .block_params = generic_block ? &block_params : nullptr});
        continue;
      }
      auto generic = template_kind::none;
      if (checked_.lowering.open_param_templates.contains(&decl)) {
        generic = template_kind::implicit;
      } else if (generic_block || !decl.type_params.empty()) {
        generic = template_kind::declared;
      }
      plan.functions.push_back(
          {.decl = &decl,
           .name = name,
           .generated = generated,
           .generic = generic,
           .block_params = generic_block ? &block_params : nullptr});
    }
  }

  auto plan_impl(const ast::impl_decl &impl, bool generated,
                 module_plan &plan) const -> void {
    const auto target =
        simple_target_name(impl.for_type.get(), !impl.type_params.empty());
    if (!target.has_value()) {
      return;
    }
    plan_block(impl.items, impl.type_params, *target, generated, plan);
  }

  auto plan_extend(const ast::extend_decl &extend, module_plan &plan) const
      -> void {
    const auto target = simple_target_name(extend.for_type.get(),
                                           !extend.type_params.empty());
    if (!target.has_value()) {
      return;
    }
    plan_block(extend.items, extend.type_params, *target, false, plan);
  }

  const semantic::checked_types &checked_;
};

class program_adapter {
public:
  program_adapter(semantic::checked_types &checked,
                  semantic::program_index index, bool contract_checks)
      : checked_(checked), planner_(checked), index_(std::move(index)),
        contract_checks_(contract_checks),
        lookup_([this](std::string_view module,
                       std::string_view name) -> const ast::func_decl * {
          const auto &table = functions_of(std::string(module));
          const auto found = table.find(std::string(name));
          return found == table.end() ? nullptr : found->second;
        }) {}
  program_adapter(const program_adapter &) = delete;
  auto operator=(const program_adapter &) -> program_adapter & = delete;
  program_adapter(program_adapter &&) = delete;
  auto operator=(program_adapter &&) -> program_adapter & = delete;
  ~program_adapter() = default;

  /// The functions `module_name` declares, as the checker's module table
  /// holds them: the program index's members, and for a functor instance
  /// the clones `functor_instances` records under it.
  [[nodiscard]] auto functions_of(const std::string &module_name)
      -> const function_table & {
    auto [it, inserted] = function_tables_.try_emplace(module_name);
    if (!inserted) {
      return it->second;
    }
    if (const auto *members = index_.find_module(module_name)) {
      for (const auto &[name, ref] : members->functions) {
        it->second.emplace(name, ref.decl);
      }
    }
    for (const auto &instance : checked_.lowering.functor_instances) {
      if (instance.owner_module == module_name && instance.decl != nullptr &&
          instance.impl_target.empty()) {
        it->second.insert_or_assign(instance.decl->name, instance.decl);
      }
    }
    return it->second;
  }

  /// The type ids of a template's parameters: its block's, then its own.
  [[nodiscard]] auto declared_params(
      const ast::func_decl &decl,
      const std::vector<ast::type_param> *block_params) -> std::vector<type_id> {
    auto params = std::vector<type_id>{};
    const auto add = [&](const std::vector<ast::type_param> &list) {
      for (const auto &param : list) {
        if (!param.name.empty()) {
          params.push_back(checked_.types.type_param(
              param.name, param.higher_kinded_arity, &param));
        }
      }
    };
    if (block_params != nullptr) {
      add(*block_params);
    }
    add(decl.type_params);
    return params;
  }

  /// Records each planned template's parameters, so every body built later
  /// knows a call to a template when it meets one.
  auto register_templates(const module_plan &plan) -> void {
    for (const auto &planned : plan.functions) {
      switch (planned.generic) {
      case template_kind::none:
        break;
      case template_kind::declared:
        templates_.insert_or_assign(
            planned.decl, declared_params(*planned.decl, planned.block_params));
        break;
      case template_kind::implicit:
        templates_.insert_or_assign(planned.decl, std::vector<type_id>{});
        break;
      }
    }
  }

  auto add_plan(module_plan plan) -> void {
    register_templates(plan);
    plans_.push_back(std::move(plan));
  }

  auto report(const std::string &module, const std::string &function,
              const failure &why, bool in_template,
              gap::item_kind item = gap::item_kind::function) -> void {
    auto construct = why.construct;
    // A template records only the decisions that hold for every instance;
    // the rest it leaves to them (`defer_to_instances`), so a record a
    // template body lacks is one of those.
    if (in_template && construct.starts_with("missing record: ") &&
        !construct.ends_with("(deferred to instances)")) {
      construct += " (deferred to instances)";
    }
    result_.gaps.push_back(gap{.module = module,
                               .function = function,
                               .span = why.span,
                               .construct = std::move(construct),
                               .detail = why.detail,
                               .feature = why.feature,
                               .in_template = in_template,
                               .item = item});
  }

  auto build_module(const module_plan &plan) -> void {
    auto built = make_node<module>();
    built->name = plan.name;
    built->file_id = plan.file_id;
    for (const auto *decl : plan.statics) {
      const auto &def = checked_.lowering.static_global_defs.at(decl);
      auto value = build_static_value(checked_, def.value, decl->span);
      if (!value.has_value()) {
        report(built->name, def.name, value.error(), false,
               gap::item_kind::static_global);
        continue;
      }
      built->statics.push_back(static_global{.name = def.name,
                                             .module = built->name,
                                             .type = (*value)->type,
                                             .value = std::move(*value)});
    }
    for (const auto &planned : plan.intrinsics) {
      auto params = declared_params(*planned.decl, planned.block_params);
      const auto generic = !params.empty();
      auto signature =
          build_intrinsic_signature(checked_, *planned.decl, std::move(params));
      if (!signature.has_value()) {
        report(built->name, planned.name, signature.error(), false,
               gap::item_kind::intrinsic);
        continue;
      }
      built->intrinsics.push_back(intrinsic_decl{
          .name = planned.name, .signature = *signature, .generic = generic});
    }
    for (const auto &planned : plan.functions) {
      const auto in_template = planned.generic != template_kind::none;
      if (planned.generic == template_kind::implicit) {
        result_.gaps.push_back(gap{
            .module = built->name,
            .function = planned.name,
            .span = planned.decl->span,
            .construct = "implicit generic template",
            .detail = "its unannotated parameters are inference variables "
                      "(rule T1), and no type parameter stands for them",
            .in_template = true,
            .item = gap::item_kind::function});
        continue;
      }
      auto params = in_template ? templates_.at(planned.decl)
                                : std::vector<type_id>{};
      auto function = build_function(
          checked_, lookup_, templates_, *planned.decl, planned.name,
          built->name, std::move(params),
          adapt_options{.contract_checks = contract_checks_,
                        .compile_time = false});
      if (!function.has_value()) {
        report(built->name, planned.name, function.error(), in_template);
        continue;
      }
      (*function)->generated = planned.generated;
      built->functions.push_back(std::move(*function));
    }
    result_.modules.push_back(std::move(built));
  }

  auto plan_submodules(const std::vector<ast::ptr<ast::node>> &items,
                       const std::string &parent,
                       std::optional<file_id_type> file_id) -> void {
    for (const auto &item : items) {
      if (item == nullptr || item->kind != ast::node_kind::sub_module_decl) {
        continue;
      }
      const auto &decl = ast::cast<const ast::sub_module_decl>(*item);
      if (decl.is_functor() || decl.items.empty()) {
        continue;
      }
      auto plan = module_plan{};
      plan.name =
          parent.empty() ? decl.name : std::format("{}.{}", parent, decl.name);
      plan.file_id = file_id;
      planner_.plan_items(decl.items, plan);
      planner_.plan_synthesized(plan.name, plan);
      const auto name = plan.name;
      add_plan(std::move(plan));
      plan_submodules(decl.items, name, file_id);
    }
  }

  auto plan_file(const semantic::parsed_module &input) -> void {
    const auto &file = *input.ast_file;
    auto plan = module_plan{};
    if (file.module_decl != nullptr) {
      for (const auto &segment : file.module_decl->path) {
        plan.name += plan.name.empty() ? segment : "." + segment;
      }
    }
    plan.file_id = input.file_id;
    planner_.plan_items(file.items, plan);
    planner_.plan_synthesized(plan.name, plan);
    const auto name = plan.name;
    add_plan(std::move(plan));
    plan_submodules(file.items, name, input.file_id);
  }

  auto plan_functors() -> void {
    auto order = std::vector<std::string>{};
    auto grouped = std::unordered_map<std::string, module_plan>{};
    for (const auto &instance : checked_.lowering.functor_instances) {
      if (instance.decl == nullptr) {
        continue;
      }
      auto [it, inserted] = grouped.try_emplace(instance.owner_module);
      if (inserted) {
        order.push_back(instance.owner_module);
        it->second.name = instance.owner_module;
      }
      it->second.functions.push_back(
          {.decl = instance.decl,
           .name = instance.impl_target.empty()
                       ? instance.decl->name
                       : std::format("{}::{}", instance.impl_target,
                                     instance.decl->name)});
    }
    for (const auto &name : order) {
      add_plan(std::move(grouped.at(name)));
    }
  }

  auto build_all() -> void {
    for (const auto &plan : plans_) {
      build_module(plan);
    }
    // Each instance points at its template, when the template was built.
    auto built_templates = std::unordered_map<const ast::func_decl *,
                                              const function *>{};
    for (const auto &m : result_.modules) {
      for (const auto &f : m->functions) {
        if (templates_.contains(f->decl)) {
          built_templates.emplace(f->decl, f.get());
        }
      }
    }
    for (auto &m : result_.modules) {
      for (auto &f : m->functions) {
        const auto tmpl = checked_.ownership.instance_templates.find(f->decl);
        if (tmpl == checked_.ownership.instance_templates.end()) {
          continue;
        }
        if (const auto found = built_templates.find(tmpl->second);
            found != built_templates.end()) {
          f->instance_of = found->second;
        }
      }
    }
  }

  [[nodiscard]] auto take() -> adapted_program { return std::move(result_); }

private:
  semantic::checked_types &checked_;
  module_planner planner_;
  semantic::program_index index_;
  bool contract_checks_ = true;
  std::unordered_map<std::string, function_table> function_tables_;
  function_lookup lookup_;
  std::vector<module_plan> plans_;
  template_table templates_;
  adapted_program result_;
};

} // namespace

auto adapt_program(std::span<const semantic::parsed_module> inputs,
                   const std::vector<bool> &file_has_errors,
                   semantic::checked_types &checked,
                   const adapt_options &options) -> adapted_program {
  auto adapter = program_adapter(
      checked,
      semantic::build_program_index(std::vector<semantic::parsed_module>(
          inputs.begin(), inputs.end())),
      options.contract_checks);
  for (const auto &input : inputs) {
    if (input.ast_file == nullptr ||
        (static_cast<size_t>(input.file_id) < file_has_errors.size() &&
         file_has_errors[input.file_id])) {
      continue;
    }
    adapter.plan_file(input);
  }
  adapter.plan_functors();
  adapter.build_all();
  return adapter.take();
}

namespace {

[[nodiscard]] auto gap_of(const failure &why, const std::string &module,
                          const std::string &item, gap::item_kind kind) -> gap {
  return gap{.module = module,
             .function = item,
             .span = why.span,
             .construct = why.construct,
             .detail = why.detail,
             .feature = why.feature,
             .in_template = false,
             .item = kind};
}

} // namespace

auto adapt_function(const ast::func_decl &decl, std::string name,
                    std::string module, semantic::checked_types &checked,
                    const function_lookup &functions,
                    const adapt_options &options)
    -> std::expected<ptr<function>, gap> {
  // The same per-construct code as `adapt_program`: a body that is not
  // generic, so no template table is needed.
  const auto no_templates = template_table{};
  auto built = build_function(checked, functions, no_templates, decl, name,
                              module, {}, options);
  if (!built.has_value()) {
    return std::unexpected(
        gap_of(built.error(), module, name, gap::item_kind::function));
  }
  return std::move(*built);
}

auto adapt_expression(const ast::expr &expr,
                      std::span<const expression_capture> captures,
                      std::string name, std::string module,
                      semantic::checked_types &checked,
                      const function_lookup &functions)
    -> std::expected<ptr<function>, gap> {
  auto built = build_expression_function(checked, functions, expr, captures,
                                         name, module);
  if (!built.has_value()) {
    return std::unexpected(
        gap_of(built.error(), module, name, gap::item_kind::function));
  }
  return std::move(*built);
}

auto adapt_static(const semantic::lowering_results::static_global_def &def,
                  std::string module, semantic::checked_types &checked,
                  source_span span) -> std::expected<static_global, gap> {
  auto value = build_static_value(checked, def.value, span);
  if (!value.has_value()) {
    return std::unexpected(
        gap_of(value.error(), module, def.name, gap::item_kind::static_global));
  }
  const auto type = (*value)->type;
  return static_global{.name = def.name,
                       .module = std::move(module),
                       .type = type,
                       .value = std::move(*value)};
}

auto adapt_intrinsic(const ast::func_decl &decl, std::string name,
                     semantic::checked_types &checked)
    -> std::expected<intrinsic_decl, gap> {
  // Its own parameters only: an `intrinsic def` in a generic block is
  // declared with the block's too, which the declaration alone does not
  // name.
  auto params = std::vector<type_id>{};
  for (const auto &param : decl.type_params) {
    if (!param.name.empty()) {
      params.push_back(checked.types.type_param(
          param.name, param.higher_kinded_arity, &param));
    }
  }
  const auto generic = !params.empty();
  auto signature = build_intrinsic_signature(checked, decl, std::move(params));
  if (!signature.has_value()) {
    return std::unexpected(
        gap_of(signature.error(), "", name, gap::item_kind::intrinsic));
  }
  return intrinsic_decl{
      .name = std::move(name), .signature = *signature, .generic = generic};
}

} // namespace cinder::core
