#include "check.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/semantic/module_index.h"
#include "src/semantic/types.h"

namespace kira::semantic {
namespace {

// ==========================================================================
//  Small helpers
// ==========================================================================

/// Bounded Levenshtein distance used for "did you mean" suggestions.
auto edit_distance(std::string_view a, std::string_view b) -> size_t {
  const auto rows = a.size() + 1;
  const auto cols = b.size() + 1;
  auto previous = std::vector<size_t>(cols);
  auto current = std::vector<size_t>(cols);
  for (size_t j = 0; j < cols; ++j) {
    previous[j] = j;
  }
  for (size_t i = 1; i < rows; ++i) {
    current[0] = i;
    for (size_t j = 1; j < cols; ++j) {
      const auto substitution =
          previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
      current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
    }
    std::swap(previous, current);
  }
  return previous[cols - 1];
}

auto best_suggestion(std::string_view name,
                     const std::vector<std::string> &candidates)
    -> std::optional<std::string> {
  auto best = std::optional<std::string>{};
  auto best_distance = size_t{3};
  for (const auto &candidate : candidates) {
    if (candidate == name || candidate.empty()) {
      continue;
    }
    const auto distance = edit_distance(name, candidate);
    if (distance < best_distance && distance < candidate.size()) {
      best_distance = distance;
      best = candidate;
    }
  }
  return best;
}

/// Parses an integer literal spelling (handles `_`, `0x`, `0o`, `0b`).
/// Returns nullopt when the value does not fit in 64 bits.
auto parse_integer_literal(std::string_view text) -> std::optional<uint64_t> {
  auto cleaned = std::string{};
  cleaned.reserve(text.size());
  for (const auto ch : text) {
    if (ch != '_') {
      cleaned.push_back(ch);
    }
  }
  auto base = 10;
  auto digits = std::string_view(cleaned);
  if (digits.size() > 2 && digits[0] == '0') {
    if (digits[1] == 'x' || digits[1] == 'X') {
      base = 16;
      digits.remove_prefix(2);
    } else if (digits[1] == 'o' || digits[1] == 'O') {
      base = 8;
      digits.remove_prefix(2);
    } else if (digits[1] == 'b' || digits[1] == 'B') {
      base = 2;
      digits.remove_prefix(2);
    }
  }
  auto value = uint64_t{0};
  const auto result =
      std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
  if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return value;
}

/// A variant-constructor expression (`@some(x)`) parses to an ident whose
/// span still covers the leading `@`; that lets us tell it apart from an
/// ordinary identifier of the same spelling.
auto is_variant_ident(const ast::ident_expr &ident) -> bool {
  return ident.span.len() > ident.name.size();
}

enum class binding_origin : uint8_t {
  let_binding,
  var_binding,
  parameter,
  pattern_binding,
  synthetic,
};

struct value_binding {
  type_id type = k_unknown_type;
  binding_origin origin = binding_origin::let_binding;
  source_span span;
};

struct fn_param_info {
  std::string name;
  type_id type = k_unknown_type;
  bool has_default = false;
  source_span span;
};

struct method_entry {
  const ast::func_decl *decl = nullptr;
  const module_members *owner = nullptr;
  const ast::trait_decl *from_trait = nullptr;
};

// ==========================================================================
//  checker — one instance per session run.
// ==========================================================================

class checker {
public:
  checker(const program_index &index, diagnostic_bag &diag,
          std::vector<bool> &file_has_errors)
      : index_(index), diag_(diag), file_has_errors_(file_has_errors) {}

  auto run(const std::vector<parsed_module> &inputs) -> void;

private:
  // --- session state ----------------------------------------------------
  const program_index &index_;
  diagnostic_bag &diag_;
  std::vector<bool> &file_has_errors_;
  type_table types_;

  // --- current file / module context -------------------------------------
  const module_members *module_ = nullptr;
  std::string module_name_;
  file_id_type file_id_ = 0;
  bool file_has_external_wildcard_ = false;

  // --- current function context -------------------------------------------
  std::vector<std::unordered_map<std::string, value_binding>> scopes_;
  std::vector<std::unordered_map<std::string, type_id>> type_params_;
  type_id self_type_ = k_unknown_type;
  type_id return_type_ = k_unknown_type;
  bool return_annotated_ = false;
  bool in_contract_ = false;
  std::unordered_set<std::string> reported_undefined_;

  // --- caches ---------------------------------------------------------------
  std::unordered_map<const ast::static_decl *, type_id> static_types_;
  std::unordered_set<const ast::static_decl *> statics_in_progress_;
  std::unordered_set<const ast::type_decl *> aliases_in_progress_;
  bool methods_built_ = false;
  std::unordered_map<const ast::type_decl *, std::vector<method_entry>>
      methods_;

  // ==========================================================================
  //  Diagnostics helpers
  // ==========================================================================

  auto mark_error() -> void {
    if (static_cast<size_t>(file_id_) < file_has_errors_.size()) {
      file_has_errors_[file_id_] = true;
    }
  }

  auto error(source_span span, std::string message, std::string label) -> void {
    auto diag = diagnostic(diagnostic_level::error, std::move(message), file_id_);
    diag.with_label(span, std::move(label));
    diag_.emit(std::move(diag));
    mark_error();
  }

  auto error_with_help(source_span span, std::string message, std::string label,
                       std::string help) -> void {
    auto diag = diagnostic(diagnostic_level::error, std::move(message), file_id_);
    diag.with_label(span, std::move(label));
    diag.with_help(std::move(help));
    diag_.emit(std::move(diag));
    mark_error();
  }

  auto type_mismatch(source_span span, type_id expected, type_id found,
                     std::string_view context) -> void {
    if (types_.compatible(expected, found)) {
      return;
    }
    auto diag = diagnostic(
        diagnostic_level::error,
        std::format("type mismatch: expected `{}`, found `{}`",
                    types_.display(expected), types_.display(found)),
        file_id_);
    diag.with_label(span, std::format("expected `{}` {}",
                                      types_.display(expected), context));
    if (types_.is_numeric(expected) && types_.is_numeric(found)) {
      diag.with_help(std::format(
          "Kira never converts numbers implicitly; write `{}(...)` to convert "
          "this value explicitly.",
          types_.display(expected)));
    }
    diag_.emit(std::move(diag));
    mark_error();
  }

  // ==========================================================================
  //  Scope management
  // ==========================================================================

  auto push_scope() -> void { scopes_.emplace_back(); }
  auto pop_scope() -> void { scopes_.pop_back(); }

  auto bind_value(std::string_view name, type_id type, binding_origin origin,
                  source_span span) -> void {
    if (name.empty() || scopes_.empty()) {
      return;
    }
    scopes_.back().insert_or_assign(std::string(name),
                                    value_binding{.type = type,
                                                  .origin = origin,
                                                  .span = span});
  }

  auto lookup_value(std::string_view name) -> const value_binding * {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
      if (const auto it = scope->find(std::string(name)); it != scope->end()) {
        return &it->second;
      }
    }
    return nullptr;
  }

  auto push_type_params(const std::vector<ast::type_param> &params) -> void {
    auto scope = std::unordered_map<std::string, type_id>{};
    for (const auto &param : params) {
      if (param.name.empty()) {
        continue;
      }
      scope.emplace(param.name, types_.type_param(param.name));
    }
    type_params_.push_back(std::move(scope));
  }

  auto pop_type_params() -> void { type_params_.pop_back(); }

  auto lookup_type_param(std::string_view name) -> std::optional<type_id> {
    for (auto scope = type_params_.rbegin(); scope != type_params_.rend();
         ++scope) {
      if (const auto it = scope->find(std::string(name)); it != scope->end()) {
        return it->second;
      }
    }
    return std::nullopt;
  }

  // ==========================================================================
  //  Module member lookup (across every file of the current module, plus
  //  imports and the prelude).
  // ==========================================================================

  auto find_session_module_of_path(const std::vector<std::string> &path) const
      -> const module_members * {
    // Longest module prefix registered in the session wins.
    const module_members *found = nullptr;
    for (size_t i = path.size(); i >= 1; --i) {
      const auto prefix = join_module_path_prefix(path, i);
      if (const auto *members = index_.find_module(prefix)) {
        found = members;
        break;
      }
      if (i == 1) {
        break;
      }
    }
    return found;
  }

  auto session_owns_path_root(const std::vector<std::string> &path) const
      -> bool {
    if (path.empty()) {
      return false;
    }
    const auto &root = path.front();
    for (const auto &[name, members] : index_.modules) {
      if (name == root || name.starts_with(root + ".")) {
        return true;
      }
    }
    return false;
  }

  auto imports_for_current_file() const -> const std::vector<import_binding> * {
    const auto it = index_.imports.find(file_id_);
    return it != index_.imports.end() ? &it->second : nullptr;
  }

  auto find_import(std::string_view name) const -> const import_binding * {
    const auto *imports = imports_for_current_file();
    if (imports == nullptr) {
      return nullptr;
    }
    for (const auto &binding : *imports) {
      if (!binding.is_wildcard && binding.local_name == name) {
        return &binding;
      }
    }
    return nullptr;
  }

  /// The module that an import binding pulls its member from, when the
  /// import stays within this compilation session.
  auto import_source_module(const import_binding &binding) const
      -> const module_members * {
    if (!session_owns_path_root(binding.path)) {
      return nullptr;
    }
    if (!binding.leaf_name.empty()) {
      return index_.find_module(join_strings(binding.path, "."));
    }
    // `use a.b.c` — `c` may itself be a module or a member of `a.b`.
    if (index_.find_module(join_strings(binding.path, ".")) != nullptr) {
      return nullptr; // imports a module; members are accessed via paths
    }
    if (binding.path.size() < 2) {
      return nullptr;
    }
    auto parent = binding.path;
    parent.pop_back();
    return index_.find_module(join_strings(parent, "."));
  }

  auto imported_member_name(const import_binding &binding) const
      -> std::string {
    return binding.leaf_name.empty() ? binding.path.back() : binding.leaf_name;
  }

  // ==========================================================================
  //  Type resolution
  //
  //  `quiet` suppresses diagnostics; it is used when resolving foreign
  //  declarations (callee signatures, alias expansion) whose own checking
  //  pass reports problems at the declaration site.
  // ==========================================================================

  struct resolve_ctx {
    const module_members *module = nullptr;
    const std::unordered_map<std::string, type_id> *param_bindings = nullptr;
    bool use_type_param_stack = false;
    bool quiet = false;
  };

  auto current_resolve_ctx() -> resolve_ctx {
    return resolve_ctx{.module = module_,
                       .param_bindings = nullptr,
                       .use_type_param_stack = true,
                       .quiet = false};
  }

  auto resolve_type(const ast::type_expr &type, const resolve_ctx &ctx)
      -> type_id {
    if (type.has_error) {
      return k_unknown_type;
    }

    switch (type.kind) {
    case ast::node_kind::named_type:
      return resolve_named_type(static_cast<const ast::named_type &>(type),
                                ctx);
    case ast::node_kind::tuple_type: {
      const auto &tuple = static_cast<const ast::tuple_type &>(type);
      auto elements = std::vector<type_id>{};
      elements.reserve(tuple.elements.size());
      for (const auto &element : tuple.elements) {
        elements.push_back(element != nullptr ? resolve_type(*element, ctx)
                                              : k_unknown_type);
      }
      return types_.tuple_of(std::move(elements));
    }
    case ast::node_kind::slice_type: {
      const auto &slice = static_cast<const ast::slice_type &>(type);
      const auto element = slice.element != nullptr
                               ? resolve_type(*slice.element, ctx)
                               : k_unknown_type;
      return types_.builtin_generic("slice", {element});
    }
    case ast::node_kind::array_type: {
      const auto &array = static_cast<const ast::array_type &>(type);
      const auto element = array.element != nullptr
                               ? resolve_type(*array.element, ctx)
                               : k_unknown_type;
      auto size = std::optional<uint64_t>{};
      if (array.size != nullptr &&
          array.size->kind == ast::node_kind::literal_expr) {
        const auto &lit = static_cast<const ast::literal_expr &>(*array.size);
        if (lit.lit_kind == token_kind::int_lit) {
          size = parse_integer_literal(lit.value);
        }
      }
      return types_.array_of(element, size);
    }
    case ast::node_kind::ref_type: {
      const auto &ref = static_cast<const ast::ref_type &>(type);
      const auto inner = ref.inner != nullptr ? resolve_type(*ref.inner, ctx)
                                              : k_unknown_type;
      return types_.ref_to(inner, ref.is_mut);
    }
    case ast::node_kind::ptr_type: {
      const auto &ptr = static_cast<const ast::ptr_type &>(type);
      const auto inner = ptr.inner != nullptr ? resolve_type(*ptr.inner, ctx)
                                              : k_unknown_type;
      return types_.ptr_to(inner, ptr.is_mut);
    }
    case ast::node_kind::fn_type: {
      const auto &fn = static_cast<const ast::fn_type &>(type);
      auto params = std::vector<type_id>{};
      params.reserve(fn.param_types.size());
      for (const auto &param : fn.param_types) {
        params.push_back(param != nullptr ? resolve_type(*param, ctx)
                                          : k_unknown_type);
      }
      const auto result = fn.return_type != nullptr
                              ? resolve_type(*fn.return_type, ctx)
                              : types_.builtin("unit");
      return types_.fn_of(std::move(params), result);
    }
    case ast::node_kind::refinement_type: {
      const auto &refinement = static_cast<const ast::refinement_type &>(type);
      return refinement.base != nullptr ? resolve_type(*refinement.base, ctx)
                                        : k_unknown_type;
    }
    case ast::node_kind::union_type:
    case ast::node_kind::bound_type:
    case ast::node_kind::quote_type:
      return k_unknown_type;
    default:
      return k_unknown_type;
    }
  }

  auto resolve_type_args(const ast::named_type &named, const resolve_ctx &ctx)
      -> std::vector<type_id> {
    auto args = std::vector<type_id>{};
    args.reserve(named.type_args.size());
    for (const auto &arg : named.type_args) {
      if (arg.value == nullptr) {
        args.push_back(k_unknown_type);
        continue;
      }
      // A type argument slot may hold a type or a compile-time value; only
      // type expressions resolve to types here.
      switch (arg.value->kind) {
      case ast::node_kind::named_type:
      case ast::node_kind::tuple_type:
      case ast::node_kind::slice_type:
      case ast::node_kind::array_type:
      case ast::node_kind::ref_type:
      case ast::node_kind::ptr_type:
      case ast::node_kind::fn_type:
      case ast::node_kind::quote_type:
      case ast::node_kind::union_type:
      case ast::node_kind::refinement_type:
      case ast::node_kind::bound_type:
        args.push_back(resolve_type(
            static_cast<const ast::type_expr &>(*arg.value), ctx));
        break;
      default:
        args.push_back(k_unknown_type);
        break;
      }
    }
    return args;
  }

  auto type_name_candidates() -> std::vector<std::string> {
    auto candidates = std::vector<std::string>{
        "bool", "int32", "int64", "float64", "str",  "char",
        "unit", "list",  "option", "result", "usize", "byte",
    };
    if (module_ != nullptr) {
      for (const auto &[name, decl] : module_->types) {
        candidates.push_back(name);
      }
      for (const auto &[name, decl] : module_->traits) {
        candidates.push_back(name);
      }
      for (const auto &[name, decl] : module_->concepts) {
        candidates.push_back(name);
      }
    }
    for (const auto &scope : type_params_) {
      for (const auto &[name, id] : scope) {
        candidates.push_back(name);
      }
    }
    return candidates;
  }

  auto emit_undefined_type(const ast::named_type &named, std::string_view name)
      -> void {
    if (!reported_undefined_.insert(std::format("type:{}", name)).second) {
      return;
    }
    auto diag = diagnostic(diagnostic_level::error,
                           std::format("undefined type `{}`", name), file_id_);
    diag.with_label(named.span, "not found in this scope");
    if (const auto suggestion = best_suggestion(name, type_name_candidates())) {
      diag.with_help(std::format("did you mean `{}`?", *suggestion));
    } else {
      diag.with_help(std::format(
          "Declare `type {}` in module `{}`, or import it with `use`.", name,
          module_name_));
    }
    diag_.emit(std::move(diag));
    mark_error();
  }

  auto instantiate_user_type(const ast::type_decl &decl,
                             std::string_view owner_module,
                             const ast::named_type &named,
                             const resolve_ctx &ctx) -> type_id {
    const auto args = resolve_type_args(named, ctx);

    if (!ctx.quiet && !named.type_args.empty() &&
        named.type_args.size() != decl.type_params.size()) {
      auto diag = diagnostic(
          diagnostic_level::error,
          std::format("type `{}` expects {} type argument{}, found {}",
                      decl.name, decl.type_params.size(),
                      decl.type_params.size() == 1 ? "" : "s",
                      named.type_args.size()),
          file_id_);
      diag.with_label(named.span, "wrong number of type arguments");
      diag_.emit(std::move(diag));
      mark_error();
      return k_error_type;
    }

    return make_user_type(decl, owner_module, args);
  }

  /// Builds a user type id, expanding alias declarations to their target.
  auto make_user_type(const ast::type_decl &decl, std::string_view owner_module,
                      std::vector<type_id> args) -> type_id {
    const auto is_struct_or_sum =
        decl.definition != nullptr &&
        (decl.definition->kind == ast::node_kind::struct_type_def ||
         decl.definition->kind == ast::node_kind::sum_type_def);
    if (is_struct_or_sum || decl.definition == nullptr) {
      return types_.user_type(decl, owner_module, std::move(args));
    }

    // Alias (`type meters = float64`) or refinement — expand to the target.
    if (!aliases_in_progress_.insert(&decl).second) {
      return k_unknown_type; // alias cycle; reported by declaration checking
    }
    auto param_bindings = std::unordered_map<std::string, type_id>{};
    for (size_t i = 0; i < decl.type_params.size(); ++i) {
      param_bindings.emplace(decl.type_params[i].name,
                             i < args.size() ? args[i] : k_unknown_type);
    }
    const auto *owner = index_.find_module(owner_module);
    auto ctx = resolve_ctx{.module = owner != nullptr ? owner : module_,
                           .param_bindings = &param_bindings,
                           .use_type_param_stack = false,
                           .quiet = true};
    auto resolved = k_unknown_type;
    switch (decl.definition->kind) {
    case ast::node_kind::named_type:
    case ast::node_kind::tuple_type:
    case ast::node_kind::slice_type:
    case ast::node_kind::array_type:
    case ast::node_kind::ref_type:
    case ast::node_kind::ptr_type:
    case ast::node_kind::fn_type:
    case ast::node_kind::quote_type:
    case ast::node_kind::union_type:
    case ast::node_kind::refinement_type:
    case ast::node_kind::bound_type:
      resolved = resolve_type(
          static_cast<const ast::type_expr &>(*decl.definition), ctx);
      break;
    default:
      break;
    }
    aliases_in_progress_.erase(&decl);
    return resolved;
  }

  auto resolve_named_type(const ast::named_type &named, const resolve_ctx &ctx)
      -> type_id {
    if (named.path.empty()) {
      return k_unknown_type;
    }

    if (named.path.size() > 1) {
      // Multi-segment paths are validated by the qualified-path pass; here we
      // only recover the declaration when the path stays in this session.
      const auto *owner = find_session_module_of_path(named.path);
      if (owner == nullptr) {
        return k_unknown_type;
      }
      const auto member = named.path.back();
      if (const auto it = owner->types.find(member); it != owner->types.end()) {
        return instantiate_user_type(*it->second.decl, owner->module_name,
                                     named, ctx);
      }
      return k_unknown_type;
    }

    const auto &name = named.path.front();

    // Generic parameters in scope.
    if (ctx.param_bindings != nullptr) {
      if (const auto it = ctx.param_bindings->find(name);
          it != ctx.param_bindings->end()) {
        return it->second;
      }
    }
    if (ctx.use_type_param_stack) {
      if (const auto param = lookup_type_param(name)) {
        return *param;
      }
    }

    // `self` names the implementing type inside impls and traits.
    if (name == "self") {
      return self_type_;
    }

    // Builtins and prelude containers.
    if (named.type_args.empty() && is_builtin_scalar_name(name)) {
      return types_.builtin(name);
    }
    if (name == "array") {
      // `array[T, n]` through named-type syntax.
      auto args = resolve_type_args(named, ctx);
      return types_.array_of(args.empty() ? k_unknown_type : args.front(),
                             std::nullopt);
    }
    if (const auto arity = builtin_generic_arity(name)) {
      auto args = resolve_type_args(named, ctx);
      if (!ctx.quiet && !named.type_args.empty() &&
          (named.type_args.size() < arity->first ||
           named.type_args.size() > arity->second)) {
        error(named.span,
              std::format("type `{}` expects {} type argument{}, found {}",
                          name, arity->first, arity->first == 1 ? "" : "s",
                          named.type_args.size()),
              "wrong number of type arguments");
        return k_error_type;
      }
      return types_.builtin_generic(name, std::move(args));
    }
    if (name == "fn") {
      return k_unknown_type;
    }

    // Current module declarations (across all of the module's files).
    const auto *members =
        ctx.module != nullptr ? ctx.module : index_.find_module(module_name_);
    if (members != nullptr) {
      if (const auto it = members->types.find(name);
          it != members->types.end()) {
        return instantiate_user_type(*it->second.decl, members->module_name,
                                     named, ctx);
      }
      if (members->traits.contains(name) || members->concepts.contains(name)) {
        return k_unknown_type; // trait/concept used in a bound position
      }
    }

    // Imported names.
    if (const auto *binding = find_import(name)) {
      if (const auto *source = import_source_module(*binding)) {
        const auto member = imported_member_name(*binding);
        if (const auto it = source->types.find(member);
            it != source->types.end()) {
          return instantiate_user_type(*it->second.decl, source->module_name,
                                       named, ctx);
        }
      }
      return k_unknown_type; // external or non-type import; trust it
    }

    // Prelude traits used in bound positions (`T: show`).
    if (is_prelude_trait_name(name) || name == "send" || name == "share" ||
        name == "pool") {
      return k_unknown_type;
    }

    if (ctx.quiet || file_has_external_wildcard_) {
      return k_unknown_type;
    }

    emit_undefined_type(named, name);
    return k_error_type;
  }

  /// Follows ref types to the value type they lend.
  auto strip_refs(type_id id) -> type_id {
    const auto *entry = &types_.entry(id);
    while (entry->kind == type_kind::ref_kind) {
      id = entry->result;
      entry = &types_.entry(id);
    }
    return id;
  }

  // ==========================================================================
  //  Struct / sum member queries with generic substitution
  // ==========================================================================

  auto param_bindings_for_instance(const type_entry &instance)
      -> std::unordered_map<std::string, type_id> {
    auto bindings = std::unordered_map<std::string, type_id>{};
    if (instance.decl == nullptr) {
      return bindings;
    }
    const auto &params = instance.decl->type_params;
    for (size_t i = 0; i < params.size(); ++i) {
      bindings.emplace(params[i].name, i < instance.args.size()
                                           ? instance.args[i]
                                           : k_unknown_type);
    }
    return bindings;
  }

  auto member_resolve_ctx(const type_entry &instance,
                          const std::unordered_map<std::string, type_id>
                              &bindings) -> resolve_ctx {
    return resolve_ctx{.module = index_.find_module(instance.module_name),
                       .param_bindings = &bindings,
                       .use_type_param_stack = false,
                       .quiet = true};
  }

  auto struct_fields_of(const type_entry &instance)
      -> const std::vector<ast::struct_field> * {
    if (instance.kind != type_kind::struct_kind || instance.decl == nullptr ||
        instance.decl->definition == nullptr ||
        instance.decl->definition->kind != ast::node_kind::struct_type_def) {
      return nullptr;
    }
    return &static_cast<const ast::struct_type_def &>(
                *instance.decl->definition)
                .body.fields;
  }

  auto struct_field_type(const type_entry &instance, std::string_view name)
      -> std::optional<type_id> {
    const auto *fields = struct_fields_of(instance);
    if (fields == nullptr) {
      return std::nullopt;
    }
    for (const auto &field : *fields) {
      if (field.name == name) {
        if (field.type == nullptr) {
          return k_unknown_type;
        }
        const auto bindings = param_bindings_for_instance(instance);
        return resolve_type(*field.type, member_resolve_ctx(instance, bindings));
      }
    }
    return std::nullopt;
  }

  auto sum_variants_of(const type_entry &instance)
      -> const std::vector<ast::sum_variant> * {
    if (instance.kind != type_kind::sum_kind || instance.decl == nullptr ||
        instance.decl->definition == nullptr ||
        instance.decl->definition->kind != ast::node_kind::sum_type_def) {
      return nullptr;
    }
    return &static_cast<const ast::sum_type_def &>(*instance.decl->definition)
                .body.variants;
  }

  auto find_variant(const type_entry &instance, std::string_view name)
      -> const ast::sum_variant * {
    const auto *variants = sum_variants_of(instance);
    if (variants == nullptr) {
      return nullptr;
    }
    for (const auto &variant : *variants) {
      if (variant.name == name) {
        return &variant;
      }
    }
    return nullptr;
  }

  auto variant_payload_types(const type_entry &instance,
                             const ast::sum_variant &variant)
      -> std::vector<type_id> {
    auto payload = std::vector<type_id>{};
    payload.reserve(variant.payload_types.size());
    const auto bindings = param_bindings_for_instance(instance);
    const auto ctx = member_resolve_ctx(instance, bindings);
    for (const auto &type : variant.payload_types) {
      payload.push_back(type != nullptr ? resolve_type(*type, ctx)
                                        : k_unknown_type);
    }
    return payload;
  }

  // ==========================================================================
  //  Function signatures
  // ==========================================================================

  auto param_name_of(const ast::Param &param) -> std::string {
    if (param.pattern != nullptr &&
        param.pattern->kind == ast::node_kind::binding_pattern) {
      return static_cast<const ast::binding_pattern &>(*param.pattern).name;
    }
    return {};
  }

  auto signature_params(const ast::func_decl &decl,
                        const module_members *owner, bool skip_self)
      -> std::vector<fn_param_info> {
    auto param_bindings = std::unordered_map<std::string, type_id>{};
    for (const auto &type_param : decl.type_params) {
      if (!type_param.name.empty()) {
        param_bindings.emplace(type_param.name,
                               types_.type_param(type_param.name));
      }
    }
    const auto ctx = resolve_ctx{.module = owner,
                                 .param_bindings = &param_bindings,
                                 .use_type_param_stack = false,
                                 .quiet = true};

    auto params = std::vector<fn_param_info>{};
    params.reserve(decl.params.size());
    for (size_t i = 0; i < decl.params.size(); ++i) {
      const auto &param = decl.params[i];
      auto name = param_name_of(param);
      if (i == 0 && skip_self && name == "self") {
        continue;
      }
      params.push_back(fn_param_info{
          .name = name,
          .type = param.type_annotation != nullptr
                      ? resolve_type(*param.type_annotation, ctx)
                      : k_unknown_type,
          .has_default = param.default_value != nullptr,
          .span = param.span,
      });
    }
    return params;
  }

  auto signature_return_type(const ast::func_decl &decl,
                             const module_members *owner) -> type_id {
    if (decl.return_type == nullptr) {
      return k_unknown_type;
    }
    auto param_bindings = std::unordered_map<std::string, type_id>{};
    for (const auto &type_param : decl.type_params) {
      if (!type_param.name.empty()) {
        param_bindings.emplace(type_param.name,
                               types_.type_param(type_param.name));
      }
    }
    const auto ctx = resolve_ctx{.module = owner,
                                 .param_bindings = &param_bindings,
                                 .use_type_param_stack = false,
                                 .quiet = true};
    return resolve_type(*decl.return_type, ctx);
  }

  auto fn_type_of(const ast::func_decl &decl, const module_members *owner)
      -> type_id {
    auto params = std::vector<type_id>{};
    for (const auto &param : signature_params(decl, owner, false)) {
      params.push_back(param.type);
    }
    auto result = signature_return_type(decl, owner);
    if (result == k_unknown_type && decl.return_type == nullptr &&
        decl.body_expr == nullptr && decl.body_stmts.empty()) {
      result = types_.builtin("unit");
    }
    return types_.fn_of(std::move(params), result);
  }

  // ==========================================================================
  //  Call checking
  // ==========================================================================

  auto check_call_args_against(const ast::call_expr &call,
                               const std::vector<fn_param_info> &params,
                               std::string_view callee_name,
                               source_location decl_location) -> void {
    auto param_used = std::vector<bool>(params.size(), false);
    auto next_positional = size_t{0};
    auto seen_named = false;

    for (const auto &arg : call.args) {
      const fn_param_info *target = nullptr;
      if (arg.name.has_value()) {
        seen_named = true;
        for (size_t i = 0; i < params.size(); ++i) {
          if (params[i].name == *arg.name) {
            if (param_used[i]) {
              error(arg.span,
                    std::format("argument `{}` is provided more than once",
                                *arg.name),
                    "duplicate argument");
            }
            param_used[i] = true;
            target = &params[i];
            break;
          }
        }
        if (target == nullptr) {
          auto diag = diagnostic(
              diagnostic_level::error,
              std::format("unknown named argument `{}` in call to `{}`",
                          *arg.name, callee_name),
              file_id_);
          diag.with_label(arg.span, "no parameter has this name");
          auto names = std::vector<std::string>{};
          for (const auto &param : params) {
            names.push_back(param.name);
          }
          if (const auto suggestion = best_suggestion(*arg.name, names)) {
            diag.with_help(std::format("did you mean `{}:`?", *suggestion));
          }
          diag_.emit(std::move(diag));
          mark_error();
        }
      } else {
        if (seen_named) {
          error(arg.span,
                "positional arguments may not follow named arguments",
                "positional argument after a named one");
        }
        while (next_positional < params.size() &&
               param_used[next_positional]) {
          ++next_positional;
        }
        if (next_positional < params.size()) {
          param_used[next_positional] = true;
          target = &params[next_positional];
          ++next_positional;
        } else {
          error(arg.span,
                std::format("too many arguments to `{}`: expected {}, found {}",
                            callee_name, params.size(), call.args.size()),
                "unexpected extra argument");
          if (arg.value != nullptr) {
            infer_expr(*arg.value, k_unknown_type);
          }
          continue;
        }
      }

      if (arg.value != nullptr) {
        const auto expected = target != nullptr ? target->type : k_unknown_type;
        const auto found = infer_expr(*arg.value, expected);
        if (target != nullptr) {
          type_mismatch(arg.value->span, expected, found, "for this argument");
        }
      }
    }

    for (size_t i = 0; i < params.size(); ++i) {
      if (param_used[i] || params[i].has_default) {
        continue;
      }
      auto diag = diagnostic(
          diagnostic_level::error,
          std::format("missing argument `{}` in call to `{}`",
                      params[i].name.empty() ? std::format("#{}", i + 1)
                                             : params[i].name,
                      callee_name),
          file_id_);
      diag.with_label(call.span, "call is missing a required argument");
      if (decl_location.span.len() > 0) {
        diag.children.push_back(
            diagnostic(diagnostic_level::note,
                       std::format("`{}` is declared here", callee_name),
                       decl_location.file_id)
                .with_label(decl_location.span, "declaration"));
      }
      diag_.emit(std::move(diag));
      mark_error();
    }
  }

  auto check_call_against_decl(const ast::call_expr &call,
                               const ast::func_decl &decl,
                               const module_members *owner,
                               file_id_type decl_file, bool skip_self)
      -> type_id {
    if (in_contract_ && !decl.modifiers.is_pure) {
      error_with_help(
          call.span,
          std::format("contract conditions may only call pure functions, and "
                      "`{}` is not declared `pure`",
                      decl.name),
          "impure call in a contract condition",
          std::format("Mark `{}` as `pure def` if it has no side effects.",
                      decl.name));
    }
    const auto params = signature_params(decl, owner, skip_self);
    check_call_args_against(call, params, decl.name,
                            source_location{.file_id = decl_file,
                                            .span = decl.span});
    return signature_return_type(decl, owner);
  }

  auto infer_call_args_loosely(const ast::call_expr &call) -> void {
    for (const auto &arg : call.args) {
      if (arg.value != nullptr) {
        infer_expr(*arg.value, k_unknown_type);
      }
    }
  }

  auto check_variant_construction(const ast::call_expr &call,
                                  const type_entry &instance,
                                  const ast::sum_variant &variant,
                                  type_id instance_id) -> type_id {
    const auto payload = variant_payload_types(instance, variant);
    if (call.args.size() != payload.size()) {
      error(call.span,
            std::format("variant `@{}` of `{}` takes {} value{}, found {}",
                        variant.name, instance.name, payload.size(),
                        payload.size() == 1 ? "" : "s", call.args.size()),
            "wrong number of constructor values");
      infer_call_args_loosely(call);
      return instance_id;
    }
    auto inferred_args = std::vector<type_id>{};
    for (size_t i = 0; i < call.args.size(); ++i) {
      const auto expected = payload[i];
      if (call.args[i].value == nullptr) {
        inferred_args.push_back(k_unknown_type);
        continue;
      }
      const auto found = infer_expr(*call.args[i].value, expected);
      type_mismatch(call.args[i].value->span, expected, found,
                    "for this constructor value");
      inferred_args.push_back(found);
    }

    // Instantiate a generic sum when a payload slot names a bare parameter.
    if (instance.decl != nullptr && !instance.decl->type_params.empty() &&
        instance.args.empty()) {
      auto args = std::vector<type_id>{};
      for (const auto &param : instance.decl->type_params) {
        auto bound = k_unknown_type;
        for (size_t i = 0; i < variant.payload_types.size(); ++i) {
          const auto *payload_type = variant.payload_types[i].get();
          if (payload_type != nullptr &&
              payload_type->kind == ast::node_kind::named_type) {
            const auto &named =
                static_cast<const ast::named_type &>(*payload_type);
            if (named.path.size() == 1 && named.path.front() == param.name &&
                i < inferred_args.size()) {
              bound = inferred_args[i];
              break;
            }
          }
        }
        args.push_back(bound);
      }
      return make_user_type(*instance.decl, instance.module_name,
                            std::move(args));
    }
    return instance_id;
  }

  /// Handles `@some(x)` / `@ok(x)` / `@err(e)` / `@none` (and their bare
  /// spellings) against an optional expected option/result instance.
  auto check_prelude_variant(std::string_view name, const ast::call_expr *call,
                             source_span span, type_id expected) -> type_id {
    const auto &expected_entry = types_.entry(expected);
    const auto expected_is =
        [&](std::string_view generic) {
          return expected_entry.kind == type_kind::builtin_generic_kind &&
                 expected_entry.name == generic;
        };

    auto payload_expected = k_unknown_type;
    if ((name == "some" && expected_is("option")) ||
        (name == "ok" && expected_is("result"))) {
      payload_expected = expected_entry.args.empty() ? k_unknown_type
                                                     : expected_entry.args[0];
    } else if (name == "err" && expected_is("result")) {
      payload_expected = expected_entry.args.size() > 1
                             ? expected_entry.args[1]
                             : k_unknown_type;
    }

    auto payload_found = k_unknown_type;
    if (call != nullptr) {
      if (name == "none" && !call->args.empty()) {
        error(span, "variant `@none` carries no value",
              "unexpected constructor value");
      }
      if (call->args.size() > 1) {
        error(span,
              std::format("variant `@{}` takes one value, found {}", name,
                          call->args.size()),
              "wrong number of constructor values");
      }
      for (const auto &arg : call->args) {
        if (arg.value != nullptr) {
          payload_found = infer_expr(*arg.value, payload_expected);
          if (payload_expected != k_unknown_type) {
            type_mismatch(arg.value->span, payload_expected, payload_found,
                          "for this constructor value");
          }
        }
      }
    }

    if (expected_is("option") && (name == "some" || name == "none")) {
      return expected;
    }
    if (expected_is("result") && (name == "ok" || name == "err")) {
      return expected;
    }
    if (name == "some" || name == "none") {
      return types_.builtin_generic("option", {payload_found});
    }
    return types_.builtin_generic(
        "result", name == "ok"
                      ? std::vector<type_id>{payload_found, k_unknown_type}
                      : std::vector<type_id>{k_unknown_type, payload_found});
  }

  auto find_module_variant(std::string_view name) -> std::optional<
      std::pair<const ast::type_decl *, const ast::sum_variant *>> {
    if (module_ == nullptr) {
      return std::nullopt;
    }
    if (const auto it = module_->variants.find(std::string(name));
        it != module_->variants.end()) {
      return std::pair{it->second.sum_decl, it->second.variant};
    }
    // Variants of imported sum types.
    const auto *imports = imports_for_current_file();
    if (imports != nullptr) {
      for (const auto &binding : *imports) {
        if (binding.is_wildcard) {
          continue;
        }
        if (const auto *source = import_source_module(binding)) {
          const auto member = imported_member_name(binding);
          if (const auto it = source->types.find(member);
              it != source->types.end()) {
            const auto member_it = source->variants.find(std::string(name));
            if (member_it != source->variants.end() &&
                member_it->second.sum_decl == it->second.decl) {
              return std::pair{member_it->second.sum_decl,
                               member_it->second.variant};
            }
          }
        }
      }
    }
    return std::nullopt;
  }

  auto resolve_variant_value(std::string_view name, const ast::call_expr *call,
                             source_span span, type_id expected)
      -> std::optional<type_id> {
    if (name == "some" || name == "none" || name == "ok" || name == "err") {
      return check_prelude_variant(name, call, span, expected);
    }

    // Prefer a variant of the expected sum type, then module-level sums.
    const auto expected_entry = types_.entry(strip_refs(expected));
    if (expected_entry.kind == type_kind::sum_kind) {
      if (const auto *variant = find_variant(expected_entry, name)) {
        if (call != nullptr) {
          return check_variant_construction(*call, expected_entry, *variant,
                                            strip_refs(expected));
        }
        if (!variant->payload_types.empty()) {
          error(span,
                std::format("variant `@{}` of `{}` carries {} value{} and "
                            "must be constructed with `@{}(...)`",
                            name, expected_entry.name,
                            variant->payload_types.size(),
                            variant->payload_types.size() == 1 ? "" : "s",
                            name),
                "missing constructor values");
        }
        return strip_refs(expected);
      }
    }

    if (const auto found = find_module_variant(name)) {
      const auto *decl = found->first;
      const auto instance_id =
          make_user_type(*decl, module_name_, {});
      const auto &instance = types_.entry(instance_id);
      if (call != nullptr) {
        return check_variant_construction(*call, instance, *found->second,
                                          instance_id);
      }
      if (!found->second->payload_types.empty()) {
        error(span,
              std::format("variant `@{}` of `{}` carries {} value{} and must "
                          "be constructed with `@{}(...)`",
                          name, decl->name, found->second->payload_types.size(),
                          found->second->payload_types.size() == 1 ? "" : "s",
                          name),
              "missing constructor values");
      }
      return instance_id;
    }

    return std::nullopt;
  }

  // ==========================================================================
  //  Identifier resolution (value namespace)
  // ==========================================================================

  auto is_type_like_name(std::string_view name) -> bool {
    if (is_builtin_scalar_name(name) || builtin_generic_arity(name) ||
        name == "array" || is_prelude_trait_name(name)) {
      return true;
    }
    if (module_ != nullptr &&
        (module_->types.contains(std::string(name)) ||
         module_->traits.contains(std::string(name)) ||
         module_->concepts.contains(std::string(name)))) {
      return true;
    }
    return false;
  }

  auto value_name_candidates() -> std::vector<std::string> {
    auto candidates = std::vector<std::string>{};
    for (const auto &scope : scopes_) {
      for (const auto &[name, binding] : scope) {
        candidates.push_back(name);
      }
    }
    if (module_ != nullptr) {
      for (const auto &[name, decl] : module_->functions) {
        candidates.push_back(name);
      }
      for (const auto &[name, decl] : module_->statics) {
        candidates.push_back(name);
      }
      for (const auto &[name, decl] : module_->variants) {
        candidates.push_back(name);
      }
    }
    for (const auto prelude :
         {"println", "print", "panic", "assert", "size_of", "args", "env",
          "min", "max"}) {
      candidates.emplace_back(prelude);
    }
    return candidates;
  }

  auto emit_undefined_name(source_span span, std::string_view name) -> void {
    if (!reported_undefined_.insert(std::format("value:{}", name)).second) {
      return;
    }
    auto diag = diagnostic(diagnostic_level::error,
                           std::format("undefined name `{}`", name), file_id_);
    diag.with_label(span, "not found in this scope");
    if (const auto suggestion = best_suggestion(name, value_name_candidates())) {
      diag.with_help(std::format("did you mean `{}`?", *suggestion));
    } else {
      diag.with_help(std::format(
          "Define `{}` before using it, or import it with `use`.", name));
    }
    diag_.emit(std::move(diag));
    mark_error();
  }

  auto is_prelude_value_name(std::string_view name) -> bool {
    return name == "println" || name == "print" || name == "panic" ||
           name == "assert" || name == "size_of" || name == "args" ||
           name == "env" || name == "min" || name == "max" ||
           name == "break" || name == "continue" || name == "cancel" ||
           name == "pool" || name == "io" || name == "cpu" ||
           name == "channel" || name == "watch" || name == "shared" ||
           name == "unwrap" || name == "expr";
  }

  auto resolve_ident(const ast::ident_expr &ident, type_id expected)
      -> type_id {
    const auto &name = ident.name;
    if (name.empty() || ident.has_error) {
      return k_unknown_type;
    }

    if (is_variant_ident(ident)) {
      if (const auto variant =
              resolve_variant_value(name, nullptr, ident.span, expected)) {
        return *variant;
      }
      emit_undefined_variant(ident.span, name, expected);
      return k_error_type;
    }

    if (const auto *binding = lookup_value(name)) {
      return binding->type;
    }
    if (name == "self" && self_type_ != k_unknown_type) {
      return self_type_;
    }

    if (module_ != nullptr) {
      if (const auto it = module_->functions.find(name);
          it != module_->functions.end()) {
        return fn_type_of(*it->second.decl, module_);
      }
      if (const auto it = module_->statics.find(name);
          it != module_->statics.end()) {
        return static_binding_type(*it->second.decl, module_);
      }
    }

    if (const auto *binding = find_import(name)) {
      if (const auto *source = import_source_module(*binding)) {
        const auto member = imported_member_name(*binding);
        if (const auto it = source->functions.find(member);
            it != source->functions.end()) {
          return fn_type_of(*it->second.decl, source);
        }
        if (const auto it = source->statics.find(member);
            it != source->statics.end()) {
          return static_binding_type(*it->second.decl, source);
        }
      }
      return k_unknown_type;
    }

    if (is_prelude_value_name(name) || is_type_like_name(name)) {
      return k_unknown_type;
    }

    // Bare variant spelling without `@` (tolerated for resilience).
    if (const auto variant =
            resolve_variant_value(name, nullptr, ident.span, expected)) {
      return *variant;
    }

    if (file_has_external_wildcard_) {
      return k_unknown_type;
    }

    emit_undefined_name(ident.span, name);
    return k_error_type;
  }

  auto emit_undefined_variant(source_span span, std::string_view name,
                              type_id expected) -> void {
    if (!reported_undefined_.insert(std::format("variant:{}", name)).second) {
      return;
    }
    auto diag = diagnostic(
        diagnostic_level::error,
        std::format("unknown variant `@{}`", name), file_id_);
    diag.with_label(span, "no sum type in scope declares this variant");
    const auto expected_entry = types_.entry(strip_refs(expected));
    if (const auto *variants = sum_variants_of(expected_entry)) {
      auto names = std::string{};
      for (const auto &variant : *variants) {
        if (!names.empty()) {
          names += ", ";
        }
        names += std::format("`@{}`", variant.name);
      }
      diag.with_note(std::format("`{}` declares the variants {}",
                                 expected_entry.name, names));
    }
    diag_.emit(std::move(diag));
    mark_error();
  }

  auto static_binding_type(const ast::static_decl &decl,
                           const module_members *owner) -> type_id {
    if (const auto it = static_types_.find(&decl); it != static_types_.end()) {
      return it->second;
    }
    if (!statics_in_progress_.insert(&decl).second) {
      return k_unknown_type; // initializer cycle
    }
    auto type = k_unknown_type;
    if (decl.type_annotation != nullptr) {
      type = resolve_type(*decl.type_annotation,
                          resolve_ctx{.module = owner,
                                      .param_bindings = nullptr,
                                      .use_type_param_stack = false,
                                      .quiet = true});
    }
    statics_in_progress_.erase(&decl);
    static_types_.emplace(&decl, type);
    return type;
  }

  // ==========================================================================
  //  Literals
  // ==========================================================================

  auto check_integer_fit(const ast::literal_expr &lit, type_id target) -> void {
    const auto &entry = types_.entry(target);
    if (entry.kind != type_kind::builtin_kind) {
      return;
    }
    const auto max_value = integer_max_value(entry.name);
    if (!max_value.has_value()) {
      return;
    }
    const auto value = parse_integer_literal(lit.value);
    if (value.has_value() && *value <= *max_value) {
      return;
    }
    auto diag = diagnostic(
        diagnostic_level::error,
        std::format("integer literal `{}` does not fit in `{}`", lit.value,
                    entry.name),
        file_id_);
    diag.with_label(lit.span, std::format("too large for `{}`", entry.name));
    diag.with_note(
        std::format("the largest `{}` value is {}", entry.name, *max_value));
    diag.with_help("Use a wider integer type, or reduce the value.");
    diag_.emit(std::move(diag));
    mark_error();
  }

  auto infer_literal(const ast::literal_expr &lit, type_id expected)
      -> type_id {
    switch (lit.lit_kind) {
    case token_kind::int_lit: {
      const auto stripped = strip_refs(expected);
      if (types_.is_integer(stripped)) {
        check_integer_fit(lit, stripped);
        return stripped;
      }
      if (types_.is_float(stripped)) {
        return stripped;
      }
      const auto fallback = types_.builtin("int32");
      check_integer_fit(lit, fallback);
      return fallback;
    }
    case token_kind::float_lit: {
      const auto stripped = strip_refs(expected);
      if (types_.is_float(stripped)) {
        return stripped;
      }
      return types_.builtin("float64");
    }
    case token_kind::string_lit:
      return types_.builtin("str");
    case token_kind::char_lit:
      return types_.builtin("char");
    case token_kind::kw_true:
    case token_kind::kw_false:
      return types_.builtin("bool");
    case token_kind::kw_unit:
      return types_.builtin("unit");
    default:
      return k_unknown_type;
    }
  }

  // ==========================================================================
  //  Binary and unary operators
  // ==========================================================================

  auto operator_trait_for(ast::binary_op op) -> std::string_view {
    switch (op) {
    case ast::binary_op::Add:
    case ast::binary_op::AddWrap:
    case ast::binary_op::AddSat:
      return "add";
    case ast::binary_op::Sub:
    case ast::binary_op::SubWrap:
    case ast::binary_op::SubSat:
      return "sub";
    case ast::binary_op::Mul:
    case ast::binary_op::MulWrap:
    case ast::binary_op::MulSat:
      return "mul";
    case ast::binary_op::Div:
      return "div";
    case ast::binary_op::Mod:
      return "rem";
    default:
      return {};
    }
  }

  auto require_operand_trait(source_span span, type_id operand,
                             std::string_view trait_name,
                             std::string_view op_name) -> type_id {
    const auto entry = types_.entry(strip_refs(operand));
    if (entry.kind == type_kind::struct_kind ||
        entry.kind == type_kind::sum_kind ||
        entry.kind == type_kind::opaque_kind) {
      if (type_has_trait(entry, trait_name)) {
        return k_unknown_type; // operator impl decides the result type
      }
      error_with_help(
          span,
          std::format("operator `{}` is not defined for type `{}`", op_name,
                      entry.name),
          std::format("`{}` does not implement the `{}` trait", entry.name,
                      trait_name),
          std::format("Add `impl {} for {}` (or `deriving {}` where "
                      "applicable) to support this operator.",
                      trait_name, entry.name, trait_name));
      return k_error_type;
    }
    return k_unknown_type;
  }

  auto infer_arithmetic(const ast::binary_expr &binary, type_id expected)
      -> type_id {
    const auto numeric_expected =
        types_.is_numeric(strip_refs(expected)) ? expected : k_unknown_type;
    const auto lhs = binary.lhs != nullptr
                         ? strip_refs(infer_expr(*binary.lhs, numeric_expected))
                         : k_unknown_type;
    const auto rhs_expected = types_.is_numeric(lhs) ? lhs : numeric_expected;
    const auto rhs = binary.rhs != nullptr
                         ? strip_refs(infer_expr(*binary.rhs, rhs_expected))
                         : k_unknown_type;
    const auto op_name = ast::binary_op_name(binary.op);

    if (types_.is_unknown(lhs) || types_.is_unknown(rhs)) {
      return types_.is_numeric(lhs)   ? lhs
             : types_.is_numeric(rhs) ? rhs
                                      : k_unknown_type;
    }

    const auto trait_name = operator_trait_for(binary.op);
    if (!types_.is_numeric(lhs)) {
      if (!trait_name.empty()) {
        return require_operand_trait(binary.lhs->span, lhs, trait_name,
                                     op_name);
      }
      error(binary.span,
            std::format("operator `{}` requires numeric operands, found `{}`",
                        op_name, types_.display(lhs)),
            "not a numeric value");
      return k_error_type;
    }
    if (!types_.is_numeric(rhs)) {
      error(binary.rhs->span,
            std::format("operator `{}` requires numeric operands, found `{}`",
                        op_name, types_.display(rhs)),
            "not a numeric value");
      return k_error_type;
    }
    if (lhs != rhs) {
      auto diag = diagnostic(
          diagnostic_level::error,
          std::format("mismatched numeric types `{}` and `{}` in `{}` "
                      "expression",
                      types_.display(lhs), types_.display(rhs), op_name),
          file_id_);
      diag.with_label(binary.span, "operands must have the same numeric type");
      diag.with_help(std::format(
          "Kira never converts numbers implicitly; convert one side "
          "explicitly, e.g. `{}(...)`.",
          types_.display(lhs)));
      diag_.emit(std::move(diag));
      mark_error();
      return k_error_type;
    }
    return lhs;
  }

  auto infer_comparison(const ast::binary_expr &binary, bool is_equality)
      -> type_id {
    const auto lhs = binary.lhs != nullptr
                         ? strip_refs(infer_expr(*binary.lhs, k_unknown_type))
                         : k_unknown_type;
    const auto rhs = binary.rhs != nullptr
                         ? strip_refs(infer_expr(*binary.rhs, lhs))
                         : k_unknown_type;
    const auto bool_type = types_.builtin("bool");
    if (types_.is_unknown(lhs) || types_.is_unknown(rhs)) {
      return bool_type;
    }
    if (!types_.compatible(lhs, rhs) && !types_.compatible(rhs, lhs)) {
      error(binary.span,
            std::format("cannot compare `{}` with `{}`", types_.display(lhs),
                        types_.display(rhs)),
            "operands have different types");
      return bool_type;
    }
    const auto trait_name = is_equality ? "eq" : "ord";
    require_operand_trait(binary.span, lhs, trait_name,
                          ast::binary_op_name(binary.op));
    return bool_type;
  }

  auto require_bool(const ast::expr &expr, std::string_view context) -> void {
    const auto found = strip_refs(infer_expr(expr, types_.builtin("bool")));
    if (!types_.is_unknown(found) && !types_.is_boolean(found)) {
      error_with_help(
          expr.span,
          std::format("{} must be `bool`, found `{}`", context,
                      types_.display(found)),
          "expected `bool` here",
          "Kira has no truthiness; write an explicit comparison such as "
          "`x != 0` or `!list.is_empty()`.");
    }
  }

  auto infer_binary(const ast::binary_expr &binary, type_id expected)
      -> type_id {
    switch (binary.op) {
    case ast::binary_op::Add:
    case ast::binary_op::Sub:
    case ast::binary_op::Mul:
    case ast::binary_op::Div:
    case ast::binary_op::Mod:
    case ast::binary_op::AddWrap:
    case ast::binary_op::SubWrap:
    case ast::binary_op::MulWrap:
    case ast::binary_op::AddSat:
    case ast::binary_op::SubSat:
    case ast::binary_op::MulSat:
      return infer_arithmetic(binary, expected);

    case ast::binary_op::EqEq:
    case ast::binary_op::BangEq:
      return infer_comparison(binary, true);
    case ast::binary_op::Lt:
    case ast::binary_op::LtEq:
    case ast::binary_op::Gt:
    case ast::binary_op::GtEq:
      return infer_comparison(binary, false);

    case ast::binary_op::And:
    case ast::binary_op::Or:
      if (binary.lhs != nullptr) {
        require_bool(*binary.lhs, "the left operand of a logical operator");
      }
      if (binary.rhs != nullptr) {
        require_bool(*binary.rhs, "the right operand of a logical operator");
      }
      return types_.builtin("bool");

    case ast::binary_op::Shl:
    case ast::binary_op::Shr:
    case ast::binary_op::BitAnd:
    case ast::binary_op::BitXor: {
      const auto lhs = binary.lhs != nullptr
                           ? strip_refs(infer_expr(*binary.lhs, expected))
                           : k_unknown_type;
      const auto rhs = binary.rhs != nullptr
                           ? strip_refs(infer_expr(*binary.rhs, lhs))
                           : k_unknown_type;
      for (const auto operand : {lhs, rhs}) {
        if (!types_.is_unknown(operand) && !types_.is_integer(operand)) {
          error(binary.span,
                std::format("operator `{}` requires integer operands, found "
                            "`{}`",
                            ast::binary_op_name(binary.op),
                            types_.display(operand)),
                "not an integer value");
          return k_error_type;
        }
      }
      return types_.is_integer(lhs) ? lhs : rhs;
    }

    case ast::binary_op::In:
    case ast::binary_op::NotIn: {
      if (binary.lhs != nullptr) {
        infer_expr(*binary.lhs, k_unknown_type);
      }
      if (binary.rhs != nullptr) {
        infer_expr(*binary.rhs, k_unknown_type);
      }
      return types_.builtin("bool");
    }

    case ast::binary_op::Range:
    case ast::binary_op::RangeInclusive: {
      const auto lhs = binary.lhs != nullptr
                           ? strip_refs(infer_expr(*binary.lhs, k_unknown_type))
                           : k_unknown_type;
      const auto rhs = binary.rhs != nullptr
                           ? strip_refs(infer_expr(*binary.rhs, lhs))
                           : k_unknown_type;
      const auto element = types_.is_integer(lhs) ? lhs
                           : types_.is_integer(rhs)
                               ? rhs
                               : k_unknown_type;
      return types_.builtin_generic("range", {element});
    }

    case ast::binary_op::Pipe:
    case ast::binary_op::BitOr: {
      const auto lhs = binary.lhs != nullptr
                           ? strip_refs(infer_expr(*binary.lhs, expected))
                           : k_unknown_type;
      const auto rhs = binary.rhs != nullptr
                           ? strip_refs(infer_expr(*binary.rhs, lhs))
                           : k_unknown_type;
      if (types_.is_integer(lhs) && types_.is_integer(rhs)) {
        return lhs;
      }
      return k_unknown_type; // sum declarations, execution graphs, patterns
    }
    }
    return k_unknown_type;
  }

  auto infer_unary(const ast::unary_expr &unary, type_id expected) -> type_id {
    const auto operand =
        unary.operand != nullptr
            ? infer_expr(*unary.operand,
                         unary.op == ast::unary_op::Neg ? expected
                                                        : k_unknown_type)
            : k_unknown_type;
    const auto stripped = strip_refs(operand);

    switch (unary.op) {
    case ast::unary_op::Neg:
      if (!types_.is_unknown(stripped) && !types_.is_numeric(stripped)) {
        error(unary.span,
              std::format("unary `-` requires a numeric operand, found `{}`",
                          types_.display(stripped)),
              "not a numeric value");
        return k_error_type;
      }
      return stripped;
    case ast::unary_op::Not:
      if (!types_.is_unknown(stripped) && !types_.is_boolean(stripped)) {
        error(unary.span,
              std::format("`not` requires a `bool` operand, found `{}`",
                          types_.display(stripped)),
              "expected `bool` here");
      }
      return types_.builtin("bool");
    case ast::unary_op::BitNot:
      if (!types_.is_unknown(stripped) && !types_.is_integer(stripped)) {
        error(unary.span,
              std::format("`~` requires an integer operand, found `{}`",
                          types_.display(stripped)),
              "not an integer value");
      }
      return stripped;
    case ast::unary_op::Deref: {
      const auto &entry = types_.entry(operand);
      if (entry.kind == type_kind::ptr_kind ||
          entry.kind == type_kind::ref_kind) {
        return entry.result;
      }
      return k_unknown_type;
    }
    case ast::unary_op::AddrOf:
      return types_.ref_to(stripped, false);
    case ast::unary_op::AddrOfMut:
      return types_.ref_to(stripped, true);
    }
    return k_unknown_type;
  }

  // Continued in part 3c: calls, fields, indexing, struct literals.



