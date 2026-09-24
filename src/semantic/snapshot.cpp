#include "src/semantic/snapshot.h"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kira::semantic {

namespace {

/// The spelling of every `ast::node_kind`, for the site column.
///
/// Deliberately a `switch` with no `default`: a new node kind must fail to
/// build here rather than render as a number in a golden file nobody
/// re-reads.
auto node_kind_name(ast::node_kind kind) -> std::string_view {
  switch (kind) {
  case ast::node_kind::error_node:
    return "error_node";
  case ast::node_kind::file_node:
    return "file";
  case ast::node_kind::module_decl:
    return "module_decl";
  case ast::node_kind::use_decl:
    return "use_decl";
  case ast::node_kind::type_decl:
    return "type_decl";
  case ast::node_kind::struct_type_def:
    return "struct_type_def";
  case ast::node_kind::sum_type_def:
    return "sum_type_def";
  case ast::node_kind::trait_decl:
    return "trait_decl";
  case ast::node_kind::signature_decl:
    return "signature_decl";
  case ast::node_kind::impl_decl:
    return "impl_decl";
  case ast::node_kind::extend_decl:
    return "extend_decl";
  case ast::node_kind::concept_decl:
    return "concept_decl";
  case ast::node_kind::func_decl:
    return "func_decl";
  case ast::node_kind::sub_module_decl:
    return "sub_module_decl";
  case ast::node_kind::dep_decl:
    return "dep_decl";
  case ast::node_kind::static_decl:
    return "static_decl";
  case ast::node_kind::associated_type_decl_node:
    return "associated_type_decl";
  case ast::node_kind::associated_type_def_node:
    return "associated_type_def";
  case ast::node_kind::splice_stmt:
    return "splice_stmt";
  case ast::node_kind::named_type:
    return "named_type";
  case ast::node_kind::bound_type:
    return "bound_type";
  case ast::node_kind::existential_type:
    return "existential_type";
  case ast::node_kind::tuple_type:
    return "tuple_type";
  case ast::node_kind::slice_type:
    return "slice_type";
  case ast::node_kind::array_type:
    return "array_type";
  case ast::node_kind::ref_type:
    return "ref_type";
  case ast::node_kind::ptr_type:
    return "ptr_type";
  case ast::node_kind::mut_type:
    return "mut_type";
  case ast::node_kind::fn_type:
    return "fn_type";
  case ast::node_kind::quote_type:
    return "quote_type";
  case ast::node_kind::splice_type:
    return "splice_type";
  case ast::node_kind::union_type:
    return "union_type";
  case ast::node_kind::refinement_type:
    return "refinement_type";
  case ast::node_kind::let_stmt:
    return "let_stmt";
  case ast::node_kind::var_stmt:
    return "var_stmt";
  case ast::node_kind::assign_stmt:
    return "assign_stmt";
  case ast::node_kind::expr_stmt:
    return "expr_stmt";
  case ast::node_kind::return_stmt:
    return "return_stmt";
  case ast::node_kind::break_stmt:
    return "break_stmt";
  case ast::node_kind::continue_stmt:
    return "continue_stmt";
  case ast::node_kind::if_stmt:
    return "if_stmt";
  case ast::node_kind::while_stmt:
    return "while_stmt";
  case ast::node_kind::for_stmt:
    return "for_stmt";
  case ast::node_kind::match_stmt:
    return "match_stmt";
  case ast::node_kind::crew_stmt:
    return "crew_stmt";
  case ast::node_kind::asm_stmt:
    return "asm_stmt";
  case ast::node_kind::ident_expr:
    return "ident";
  case ast::node_kind::literal_expr:
    return "literal";
  case ast::node_kind::binary_expr:
    return "binary";
  case ast::node_kind::unary_expr:
    return "unary";
  case ast::node_kind::postfix_expr:
    return "postfix";
  case ast::node_kind::call_expr:
    return "call";
  case ast::node_kind::index_expr:
    return "index";
  case ast::node_kind::field_expr:
    return "field";
  case ast::node_kind::cast_expr:
    return "cast";
  case ast::node_kind::try_expr:
    return "try";
  case ast::node_kind::tuple_expr:
    return "tuple";
  case ast::node_kind::array_expr:
    return "array";
  case ast::node_kind::struct_expr:
    return "struct_literal";
  case ast::node_kind::lambda_expr:
    return "lambda";
  case ast::node_kind::match_expr:
    return "match";
  case ast::node_kind::if_expr:
    return "if";
  case ast::node_kind::for_expr:
    return "for_expr";
  case ast::node_kind::await_expr:
    return "await";
  case ast::node_kind::async_expr:
    return "async";
  case ast::node_kind::yield_expr:
    return "yield";
  case ast::node_kind::par_expr:
    return "par";
  case ast::node_kind::race_expr:
    return "race";
  case ast::node_kind::crew_expr:
    return "crew";
  case ast::node_kind::on_expr:
    return "on";
  case ast::node_kind::block_expr:
    return "block";
  case ast::node_kind::quote_expr:
    return "quote";
  case ast::node_kind::splice_expr:
    return "splice";
  case ast::node_kind::static_expr:
    return "static";
  case ast::node_kind::module_path_expr:
    return "module_path";
  case ast::node_kind::group_expr:
    return "group";
  case ast::node_kind::where_expr:
    return "where";
  case ast::node_kind::interpolated_string_expr:
    return "interp_string";
  case ast::node_kind::wildcard_pattern:
    return "wildcard_pat";
  case ast::node_kind::literal_pattern:
    return "literal_pat";
  case ast::node_kind::binding_pattern:
    return "binding_pat";
  case ast::node_kind::constructor_pattern:
    return "ctor_pat";
  case ast::node_kind::tuple_pattern:
    return "tuple_pat";
  case ast::node_kind::struct_pattern:
    return "struct_pat";
  case ast::node_kind::array_pattern:
    return "array_pat";
  case ast::node_kind::range_pattern:
    return "range_pat";
  case ast::node_kind::option_pattern:
    return "option_pat";
  case ast::node_kind::result_pattern:
    return "result_pat";
  case ast::node_kind::ref_pattern:
    return "ref_pat";
  case ast::node_kind::or_pattern:
    return "or_pat";
  case ast::node_kind::group_pattern:
    return "group_pat";
  }
  return "?";
}

/// One rendered decision: the line itself, plus the key it sorts under.
///
/// The key is separate from the text because the text reads best with
/// ordinary numbers (`list.kira:118:9`) and sorts correctly only with padded
/// ones — sorting on the text alone would put line 100 before line 12.
struct entry {
  std::string key;
  std::string text;
};

class renderer {
public:
  renderer(const checked_types &checked, const source_manager &sources)
      : checked_(checked), sources_(sources) {}

  [[nodiscard]] auto run() -> std::string;

private:
  const checked_types &checked_;
  const source_manager &sources_;
  std::string out_;

  // ------------------------------------------------------------------
  //  Site and value rendering
  // ------------------------------------------------------------------

  /// The file `node` was checked in, or `nullptr` if it was recorded without
  /// one (see `checked_types::node_files`).
  [[nodiscard]] auto file_of(const ast::node *node) const
      -> const source_file * {
    if (node == nullptr) {
      return nullptr;
    }
    const auto found = checked_.node_files.find(node);
    if (found == checked_.node_files.end()) {
      return nullptr;
    }
    return sources_.get(found->second);
  }

  /// `std/list.kira:118:9`, or `?:<offset>` when the site has no known file.
  [[nodiscard]] auto site_text(source_span span, const source_file *file) const
      -> std::string {
    if (file == nullptr) {
      return std::format("?:{}", span.start);
    }
    const auto at = file->resolve(span.start);
    return std::format("{}:{}:{}", file->name(), at.line, at.column);
  }

  /// Sorts by file, then by byte range: a diff lands next to the code that
  /// moved. Padded so the order is numeric rather than lexicographic.
  [[nodiscard]] auto site_key(source_span span, const source_file *file) const
      -> std::string {
    return std::format("{}\x1f{:010}\x1f{:010}",
                       file == nullptr ? std::string_view("?") : file->name(),
                       span.start, span.end);
  }

  [[nodiscard]] auto ty(type_id id) const -> std::string {
    return checked_.types.display(id);
  }

  [[nodiscard]] static auto decl_name(const ast::func_decl *decl)
      -> std::string {
    return decl == nullptr ? std::string("-") : decl->name;
  }

  /// A resolved call target, in the shape lowering will mangle it into:
  /// `owner.module::target::name`, plus the trait it went through and
  /// whether a receiver is passed.
  [[nodiscard]] static auto callee_text(const resolved_callee &callee)
      -> std::string {
    auto text = std::string{};
    if (!callee.owner_module.empty()) {
      text += callee.owner_module;
      text += "::";
    }
    if (!callee.impl_target_type.empty()) {
      text += callee.impl_target_type;
      text += "::";
    }
    text += decl_name(callee.decl);
    if (!callee.trait_name.empty()) {
      text += std::format(" trait={}", callee.trait_name);
    }
    if (callee.receiver != nullptr) {
      text += " recv";
    }
    return text;
  }

  [[nodiscard]] auto loop_dispatch_text(const iterator_loop_dispatch &d) const
      -> std::string {
    auto text = std::format("next={}::{} elem={}", d.impl_target_type,
                            decl_name(d.decl), ty(d.element_type));
    if (!d.owner_module.empty()) {
      text += std::format(" owner={}", d.owner_module);
    }
    if (d.adapter_decl != nullptr) {
      text += std::format(" adapter={}::{} -> {}", d.adapter_impl_target_type,
                          decl_name(d.adapter_decl), ty(d.adapter_result_type));
    }
    return text;
  }

  [[nodiscard]] auto comprehension_text(const comprehension_dispatch &d) const
      -> std::string {
    return std::format("list={} new={} push={}", ty(d.list_type),
                       callee_text(d.new_callee), callee_text(d.push_callee));
  }

  // ------------------------------------------------------------------
  //  Section emission
  // ------------------------------------------------------------------

  auto emit(std::string_view name, std::vector<entry> entries) -> void {
    std::ranges::sort(entries, {}, &entry::key);
    out_ += std::format("\n## {} ({})\n", name, entries.size());
    for (const auto &item : entries) {
      out_ += item.text;
      out_ += '\n';
    }
  }

  /// A map or set keyed by an `ast::node` subclass: the file comes from
  /// `node_files`, so these are the readable ones.
  template <typename Map, typename Fn>
  auto node_section(std::string_view name, const Map &map, Fn render) -> void {
    auto entries = std::vector<entry>{};
    entries.reserve(map.size());
    for (const auto &item : map) {
      const auto *key = key_of(item);
      const auto *node = static_cast<const ast::node *>(key);
      const auto *file = file_of(node);
      auto text = std::format("{} [{}] {}", site_text(node->span, file),
                              node_kind_name(node->kind), render(item));
      entries.push_back(entry{.key = site_key(node->span, file) + text,
                              .text = std::move(text)});
    }
    emit(name, std::move(entries));
  }

  /// A map keyed by something that carries a `span` but is not an
  /// `ast::node` — a struct pattern's field clause, a struct literal's field
  /// initializer, a contract clause. There is no node to look a file up by,
  /// so these render as `?:<offset>`; they are few, and the payload names
  /// the field.
  template <typename Map, typename Fn>
  auto span_section(std::string_view name, const Map &map, Fn render) -> void {
    auto entries = std::vector<entry>{};
    entries.reserve(map.size());
    for (const auto &item : map) {
      const auto *key = key_of(item);
      auto text =
          std::format("{} {}", site_text(key->span, nullptr), render(item));
      entries.push_back(entry{.key = site_key(key->span, nullptr) + text,
                              .text = std::move(text)});
    }
    emit(name, std::move(entries));
  }

  /// Sections with no source site at all — keyed by type, by name, or not
  /// keyed at all. Sorted by their own text.
  auto text_section(std::string_view name, std::vector<std::string> lines)
      -> void {
    auto entries = std::vector<entry>{};
    entries.reserve(lines.size());
    for (auto &line : lines) {
      entries.push_back(entry{.key = line, .text = std::move(line)});
    }
    emit(name, std::move(entries));
  }

  /// `map` iteration yields a pair for a map and a bare key for a set; both
  /// spellings need the key.
  template <typename Pair>
  [[nodiscard]] static auto key_of(const Pair &item) -> decltype(item.first) {
    return item.first;
  }
  template <typename Key>
  [[nodiscard]] static auto key_of(Key *const &item) -> Key * {
    return item;
  }
};

auto renderer::run() -> std::string {
  out_ = std::format("# inference snapshot\n# {} interned types\n",
                     checked_.types.count());

  node_section("node_types", checked_.node_types,
               [this](const auto &item) { return ty(item.second); });

  span_section("struct_pattern_field_types",
               checked_.struct_pattern_field_types, [this](const auto &item) {
                 return std::format("{} {}", item.first->name, ty(item.second));
               });
  span_section("struct_literal_field_types",
               checked_.struct_literal_field_types, [this](const auto &item) {
                 return std::format("{} {}", item.first->name, ty(item.second));
               });

  node_section("call_argument_mappings", checked_.call_argument_mappings,
               [](const auto &item) {
                 const auto &mapping = item.second;
                 auto text = std::string{};
                 for (size_t i = 0; i < mapping.param_names.size(); ++i) {
                   if (i != 0) {
                     text += ", ";
                   }
                   text += mapping.param_names[i];
                   const auto *arg = i < mapping.args_by_param.size()
                                         ? mapping.args_by_param[i]
                                         : nullptr;
                   const auto *fallback = i < mapping.defaults_by_param.size()
                                              ? mapping.defaults_by_param[i]
                                              : nullptr;
                   text += arg != nullptr        ? "=arg"
                           : fallback != nullptr ? "=default"
                                                 : "=omitted";
                 }
                 return std::format("({})", text);
               });

  const auto callee = [](const auto &item) { return callee_text(item.second); };
  node_section("resolved_callees", checked_.resolved_callees, callee);
  node_section("resolved_fn_values", checked_.resolved_fn_values, callee);
  node_section("operator_dispatches", checked_.operator_dispatches, callee);
  node_section("ord_dispatch_result_types", checked_.ord_dispatch_result_types,
               [this](const auto &item) { return ty(item.second); });
  node_section("index_dispatches", checked_.index_dispatches, callee);
  node_section("index_set_dispatches", checked_.index_set_dispatches, callee);
  node_section("index_mut_dispatches", checked_.index_mut_dispatches, callee);
  node_section("index_ref_dispatches", checked_.index_ref_dispatches, callee);
  node_section("try_conversions", checked_.try_conversions, callee);
  node_section("try_conversion_types", checked_.try_conversion_types,
               [this](const auto &item) { return ty(item.second); });

  node_section("array_literal_conversions", checked_.array_literal_conversions,
               [this](const auto &item) {
                 return std::format("{} from {}",
                                    callee_text(item.second.callee),
                                    ty(item.second.array_type));
               });

  node_section("interp_dispatches", checked_.interp_dispatches,
               [this](const auto &item) {
                 const auto &d = item.second;
                 const auto kind = [&]() -> std::string_view {
                   switch (d.kind) {
                   case interp_dispatch::kind_t::builtin_show:
                     return "show";
                   case interp_dispatch::kind_t::builtin_debug:
                     return "debug";
                   case interp_dispatch::kind_t::builtin_radix:
                     return "radix";
                   case interp_dispatch::kind_t::builtin_float:
                     return "float";
                   case interp_dispatch::kind_t::builtin_char:
                     return "char";
                   case interp_dispatch::kind_t::trait_method:
                     return "trait_method";
                   }
                   return "?";
                 }();
                 auto text = std::format("{} spec='{}' value={}", kind,
                                         d.type_char == 0 ? ' ' : d.type_char,
                                         ty(d.value_type));
                 if (d.decl != nullptr) {
                   text += std::format(" -> {}::{}::{}", d.owner_module,
                                       d.impl_target_type, decl_name(d.decl));
                 }
                 return text;
               });

  node_section("type_param_reflections", checked_.type_param_reflections,
               [](const auto &item) { return item.second.type_name; });

  node_section(
      "for_iterator_dispatches", checked_.for_iterator_dispatches,
      [this](const auto &item) { return loop_dispatch_text(item.second); });
  node_section(
      "comprehension_iterator_dispatches",
      checked_.comprehension_iterator_dispatches,
      [this](const auto &item) { return loop_dispatch_text(item.second); });
  node_section(
      "comprehension_dispatches", checked_.comprehension_dispatches,
      [this](const auto &item) { return comprehension_text(item.second); });
  node_section(
      "runtime_fill_dispatches", checked_.runtime_fill_dispatches,
      [this](const auto &item) { return comprehension_text(item.second); });

  node_section(
      "layout_queries", checked_.layout_queries, [this](const auto &item) {
        return std::format(
            "{} of {}",
            item.second.kind == layout_query_kind::size_of ? "size" : "align",
            ty(item.second.operand));
      });
  node_section("ptr_casts", checked_.ptr_casts,
               [this](const auto &item) { return ty(item.second); });
  node_section("slice_from_raw_parts_calls",
               checked_.slice_from_raw_parts_calls,
               [](const auto &) { return std::string{}; });
  node_section("stack_buffers", checked_.stack_buffers,
               [this](const auto &item) {
                 return std::format("{} x {}", ty(item.second.element),
                                    item.second.count);
               });
  node_section("proven_in_bounds", checked_.proven_in_bounds,
               [](const auto &) { return std::string{}; });

  node_section("static_const_values", checked_.static_const_values,
               [](const auto &item) {
                 return item.second == nullptr ? std::string("-")
                                               : item.second->value;
               });
  node_section("folded_comptime_calls", checked_.folded_comptime_calls,
               [](const auto &item) {
                 return item.second == nullptr ? std::string("-")
                                               : item.second->value;
               });
  node_section("static_global_refs", checked_.static_global_refs,
               [](const auto &item) { return item.second; });
  node_section("static_if_taken_branch", checked_.static_if_taken_branch,
               [](const auto &item) {
                 return std::string(item.second ? "then" : "else");
               });
  node_section("static_global_defs", checked_.static_global_defs,
               [this](const auto &item) {
                 return std::format("{} : {} [{} element(s)]", item.second.name,
                                    ty(item.second.type),
                                    item.second.elements.size());
               });

  node_section("spliced_fragments", checked_.spliced_fragments,
               [](const auto &item) {
                 return item.second == nullptr
                            ? std::string("-")
                            : std::string(node_kind_name(item.second->kind));
               });

  // Sections with no source site of their own.
  auto drops = std::vector<std::string>{};
  drops.reserve(checked_.drop_plans.size());
  for (const auto &[type, plan] : checked_.drop_plans) {
    auto fields = std::string{};
    for (const auto &[name, field_type] : plan.droppable_fields) {
      if (!fields.empty()) {
        fields += ", ";
      }
      fields += std::format("{}: {}", name, ty(field_type));
    }
    drops.push_back(std::format(
        "{} own={} fields=[{}]", ty(type),
        plan.own_drop.has_value() ? callee_text(*plan.own_drop) : "-", fields));
  }
  text_section("drop_plans", std::move(drops));

  auto views = std::vector<std::string>{};
  views.reserve(checked_.view_bearing_types.size());
  for (const auto type : checked_.view_bearing_types) {
    views.push_back(ty(type));
  }
  text_section("view_bearing_types", std::move(views));

  auto owners = std::vector<std::string>{};
  owners.reserve(checked_.static_global_owners.size());
  for (const auto &[name, owner] : checked_.static_global_owners) {
    owners.push_back(std::format("{} -> {}", name, owner));
  }
  text_section("static_global_owners", std::move(owners));

  auto comptime_only = std::vector<std::string>{};
  comptime_only.reserve(checked_.comptime_only_functions.size());
  for (const auto *decl : checked_.comptime_only_functions) {
    comptime_only.push_back(decl_name(decl));
  }
  text_section("comptime_only_functions", std::move(comptime_only));

  auto defaults = std::vector<std::string>{};
  defaults.reserve(checked_.synthesized_trait_defaults.size());
  for (const auto &method : checked_.synthesized_trait_defaults) {
    defaults.push_back(std::format("{}::{}::{}", method.owner_module,
                                   method.target_type_name,
                                   decl_name(method.decl)));
  }
  text_section("synthesized_trait_defaults", std::move(defaults));

  auto instances = std::vector<std::string>{};
  instances.reserve(checked_.const_generic_instances.size());
  for (const auto &instance : checked_.const_generic_instances) {
    instances.push_back(
        std::format("{}::{}", instance.owner_module, decl_name(instance.decl)));
  }
  text_section("const_generic_instances", std::move(instances));

  auto functors = std::vector<std::string>{};
  functors.reserve(checked_.functor_instances.size());
  for (const auto &instance : checked_.functor_instances) {
    functors.push_back(std::format("{}::{}::{}", instance.owner_module,
                                   instance.impl_target,
                                   decl_name(instance.decl)));
  }
  text_section("functor_instances", std::move(functors));

  auto splices = std::vector<std::string>{};
  splices.reserve(checked_.synthesized_item_splices.size());
  for (const auto &splice : checked_.synthesized_item_splices) {
    splices.push_back(
        std::format("{} impl@{}", splice.owner_module,
                    splice.impl == nullptr ? 0 : splice.impl->span.start));
  }
  text_section("synthesized_item_splices", std::move(splices));

  auto synthesized = std::vector<std::string>{};
  synthesized.reserve(checked_.synthesized_decls.size());
  for (const auto &decl : checked_.synthesized_decls) {
    synthesized.push_back(decl_name(decl.get()));
  }
  text_section("synthesized_decls", std::move(synthesized));

  // The remaining synthesized arenas have no stable identity of their own —
  // they are storage keeping cloned AST alive, reached through the maps
  // above. Their sizes still move when monomorphization changes, so the
  // counts are worth watching even though the contents are not addressable.
  out_ += std::format("\n## arena sizes\n"
                      "synthesized_functor_nodes {}\n"
                      "synthesized_types {}\n"
                      "synthesized_fragments {}\n"
                      "synthesized_const_literals {}\n"
                      "elided_contracts {}\n",
                      checked_.synthesized_functor_nodes.size(),
                      checked_.synthesized_types.size(),
                      checked_.synthesized_fragments.size(),
                      checked_.synthesized_const_literals.size(),
                      checked_.elided_contracts.size());

  const auto &fmt = checked_.fmt_types;
  out_ += std::format(
      "\n## fmt_runtime_types\n"
      "format_spec {}\nalign_mode {}\nsign_mode {}\n"
      "str {}\nint64 {}\nuint64 {}\n"
      "uint32 {}\nuint8 {}\nfloat64 {}\noption_align_mode {}\n"
      "option_usize {}\n",
      ty(fmt.format_spec), ty(fmt.align_mode), ty(fmt.sign_mode),
      ty(fmt.str_type), ty(fmt.int64_type),
      ty(fmt.uint64_type), ty(fmt.uint32_type), ty(fmt.uint8_type),
      ty(fmt.float64_type), ty(fmt.option_align_mode), ty(fmt.option_usize));

  return std::move(out_);
}

} // namespace

auto render_snapshot(const checked_types &checked,
                     const source_manager &sources) -> std::string {
  return renderer(checked, sources).run();
}

} // namespace kira::semantic
