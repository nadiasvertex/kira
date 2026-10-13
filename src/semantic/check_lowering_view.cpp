#include "checker.h"
#include "src/parser/ast_traversal.h"

#include <cstdlib>
#include <print>

// The lowering view: the checker's records for the functions compile-time code
// runs on the VM, copied out of the session tables as each function is needed.
//
// Lowering reads a `checked_types`, which normally exists only once checking
// has finished. Compile-time code runs in the middle of checking, so the
// checker keeps a second `checked_types` (`session_.lowering_view_`) that
// holds just the functions the VM has been asked to run. Each record is copied
// with its types settled and refinements erased, exactly as
// `take_checked_types` does for the whole session, so lowering sees the same
// thing in both. The view's type table is the session's own, so the two never
// disagree about a `type_id`.
//
// Most records lowering needs and this projection does not copy make lowering
// fail closed ("no concrete checked type is available for this node"), and the
// call is interpreted instead. Not all: a callee's parameter types decide how
// an argument is passed, and without them lowering passes it another way
// without complaint. `CINDER_COMPTIME_VERIFY` (on in the driver tests)
// re-lowers every function the VM ran from the final results and reports any
// function that lowers differently (`comptime_vm::vm_backend::verify`).

namespace cinder::semantic {
namespace detail {

namespace {

/// Copies `from[key]` into `to[key]`, passing the copy through `fix` first.
template <typename Map, typename Key, typename Fix>
auto copy_entry(const Map &from, Map &to, const Key &key, Fix &&fix) -> bool {
  const auto found = from.find(key);
  if (found == from.end()) {
    return false;
  }
  auto value = found->second;
  fix(value);
  to.insert_or_assign(key, std::move(value));
  return true;
}

template <typename Map, typename Key>
auto copy_entry(const Map &from, Map &to, const Key &key) -> bool {
  return copy_entry(from, to, key, [](auto &) -> void {});
}

/// Records `index`'s reverse in `names`, keeping the first name a
/// declaration was indexed under.
auto index_names(
    const std::unordered_map<
        std::string, std::unordered_map<std::string, const ast::func_decl *>>
        &index,
    std::unordered_map<const ast::func_decl *,
                       std::pair<std::string, std::string>> &names) -> void {
  for (const auto &[module, functions] : index) {
    for (const auto &[name, decl] : functions) {
      names.emplace(decl, std::pair{module, name});
    }
  }
}

template <typename Set, typename Key>
auto copy_member(const Set &from, Set &to, const Key &key) -> void {
  if (from.contains(key)) {
    to.insert(key);
  }
}

} // namespace

/// The comptime program the VM backend talks to: a thin adapter over the
/// checker's own lowering-view functions.
class checker::vm_program final : public comptime_program {
public:
  explicit vm_program(checker &owner) : owner_(owner) {}

  [[nodiscard]] auto view() const -> const checked_types & override {
    return owner_.session_.lowering_view_;
  }

  [[nodiscard]] auto prepare(const ast::func_decl &fn)
      -> std::expected<void, std::string> override {
    return owner_.prepare_for_lowering(fn);
  }

  [[nodiscard]] auto prepare_expression(const ast::expr &expr)
      -> std::expected<void, std::string> override {
    return owner_.prepare_expression_for_lowering(expr);
  }

  [[nodiscard]] auto find_function(std::string_view owner_module,
                                   std::string_view hir_name)
      -> std::optional<comptime_function> override {
    return owner_.find_lowered_function(owner_module, hir_name);
  }

  [[nodiscard]] auto static_global(std::string_view name)
      -> const lowering_results::static_global_def * override {
    return owner_.project_static_global(name);
  }

  [[nodiscard]] auto host_call(std::string_view operation, const void *subject,
                               std::span<const comptime::value> args,
                               source_span span)
      -> std::expected<comptime::value, std::string> override {
    auto &fragments = owner_.comptime_eval_.fragments();
    auto built = std::expected<comptime::value, comptime::fragment_error>{};
    if (operation == "static" && subject != nullptr) {
      const auto found = owner_.cache_.static_binding_values_.find(
          static_cast<const ast::static_decl *>(subject));
      if (found == owner_.cache_.static_binding_values_.end() ||
          !found->second.has_value()) {
        return std::unexpected(std::string("the static has no value"));
      }
      return *found->second;
    }
    if (operation == "quote" && subject != nullptr) {
      built = fragments.quote(*static_cast<const ast::quote_expr *>(subject),
                              args);
    } else if (operation.starts_with("expr.")) {
      const auto spans = std::vector<source_span>(args.size(), span);
      built = fragments.build(operation.substr(5), args, spans, span);
    } else {
      return std::unexpected(
          std::format("`{}` is not a compile-time operation", operation));
    }
    if (!built.has_value()) {
      return std::unexpected(std::move(built.error().message));
    }
    return *std::move(built);
  }

  [[nodiscard]] auto file_of(const ast::func_decl &fn)
      -> std::optional<file_id_type> override {
    if (const auto owner = owner_.function_owner(fn); owner.has_value()) {
      return owner->second;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto struct_fields(type_id id)
      -> std::optional<std::vector<std::pair<std::string, type_id>>> override {
    const auto entry = owner_.session_.types_.entry(id); // copy: may intern
    const auto *fields = owner_.struct_fields_of(entry);
    if (fields == nullptr) {
      return std::nullopt;
    }
    auto result = std::vector<std::pair<std::string, type_id>>{};
    for (const auto &field : *fields) {
      const auto type = owner_.struct_field_type(entry, field.name);
      if (!type.has_value()) {
        return std::nullopt;
      }
      result.emplace_back(field.name, owner_.lowering_type(*type));
    }
    return result;
  }

  [[nodiscard]] auto variant_payload(type_id id, std::string_view variant)
      -> std::optional<std::vector<type_id>> override {
    const auto entry = owner_.session_.types_.entry(id); // copy: may intern
    if (entry.kind == type_kind::builtin_generic_kind) {
      if (entry.name == "option" && entry.args.size() == 1) {
        if (variant == "some") {
          return std::vector<type_id>{entry.args[0]};
        }
        if (variant == "none") {
          return std::vector<type_id>{};
        }
      }
      if (entry.name == "result" && entry.args.size() == 2) {
        if (variant == "ok") {
          return std::vector<type_id>{entry.args[0]};
        }
        if (variant == "err") {
          return std::vector<type_id>{entry.args[1]};
        }
      }
      return std::nullopt;
    }
    const auto *found = owner_.find_variant(entry, variant);
    if (found == nullptr) {
      return std::nullopt;
    }
    auto payload = owner_.variant_payload_types(entry, *found);
    for (auto &type : payload) {
      type = owner_.lowering_type(type);
    }
    return payload;
  }

private:
  checker &owner_;
};

checker::~checker() = default;

auto checker::set_comptime_backend(comptime_backend *backend,
                                   comptime::evaluator::vm_mode mode) -> void {
  comptime_backend_ = backend;
  if (backend == nullptr) {
    comptime_eval_.set_vm_call({}, comptime::evaluator::vm_mode::off);
    return;
  }
  if (vm_program_ == nullptr) {
    vm_program_ = std::make_unique<vm_program>(*this);
  }
  comptime_eval_.set_vm_call(
      [this](const ast::func_decl &fn, std::span<const comptime::value> args,
             std::span<const std::pair<std::string, comptime::value>> type_args)
          -> comptime::evaluator::vm_call_outcome {
        return run_comptime_call_on_vm(fn, args, type_args);
      },
      mode);
  comptime_eval_.set_vm_eval(
      [this](const ast::expr &expr) -> comptime::evaluator::vm_call_outcome {
        return run_comptime_expr_on_vm(expr);
      });
}

auto checker::comptime_instance(
    const ast::func_decl &fn,
    std::span<const std::pair<std::string, comptime::value>> type_args)
    -> const ast::func_decl * {
  // Named the way `find_or_check_generic_instance` names it: the template's
  // name and one `$<type>` per type parameter, in declaration order.
  auto name = fn.name;
  for (const auto &param : fn.type_params) {
    const auto bound =
        std::ranges::find_if(type_args, [&](const auto &arg) -> bool {
          return arg.first == param.name;
        });
    if (param.is_value_param || bound == type_args.end() ||
        bound->second.kind != comptime::value_kind::type_value ||
        bound->second.checked_type == k_unknown_type) {
      return nullptr;
    }
    name += "$" + mangle_type_for_instance(settle(bound->second.checked_type));
  }
  const auto found = session_.hk_instance_cache_.find(
      std::format("{}#{}", static_cast<const void *>(&fn), name));
  return found != session_.hk_instance_cache_.end() ? found->second : nullptr;
}

auto checker::run_comptime_call_on_vm(
    const ast::func_decl &template_fn, std::span<const comptime::value> args,
    std::span<const std::pair<std::string, comptime::value>> type_args)
    -> comptime::evaluator::vm_call_outcome {
  using outcome = comptime::evaluator::vm_call_outcome;
  if (!bodies_phase_ || comptime_backend_ == nullptr) {
    return outcome{.outcome = outcome::kind::not_run};
  }
  const auto *instance = type_args.empty()
                             ? &template_fn
                             : comptime_instance(template_fn, type_args);
  if (instance == nullptr) {
    return outcome{.outcome = outcome::kind::not_run};
  }
  const auto &fn = *instance;
  if (instances_.queued_template(fn) != nullptr) {
    make_instance_now(fn);
  }
  refresh_function_index();
  const auto named = lowering_view_state_.names_by_decl.find(&fn);
  if (named == lowering_view_state_.names_by_decl.end()) {
    return outcome{.outcome = outcome::kind::not_run};
  }
  // Copied: running the call can grow the index, which rebuilds it.
  const auto callee = comptime_function{.decl = &fn,
                                        .owner_module = named->second.first,
                                        .hir_name = named->second.second};
  auto result = comptime_backend_->call(*vm_program_, callee, args);
  // `CINDER_COMPTIME_TRACE` lists each call and whether the VM ran it — for
  // finding what keeps a call in the interpreter.
  static const auto trace = std::getenv("CINDER_COMPTIME_TRACE") != nullptr;
  if (trace) {
    std::println(stderr, "comptime vm: `{}`: {}", callee.hir_name,
                 result.has_value() ? std::string("ran")
                 : result.error().reason == comptime_failure::kind::unsupported
                     ? "not run: " + result.error().message
                     : "failed: " + result.error().message);
  }
  if (result.has_value()) {
    return outcome{.outcome = outcome::kind::returned,
                   .result = *std::move(result)};
  }
  if (result.error().reason == comptime_failure::kind::unsupported) {
    return outcome{.outcome = outcome::kind::not_run,
                   .message = std::move(result.error().message)};
  }
  return outcome{.outcome = outcome::kind::failed,
                 .result = {},
                 .message = std::move(result.error().message),
                 .notes = vm_failure_notes(result.error())};
}

auto checker::vm_failure_notes(const comptime_failure &failure)
    -> std::vector<diagnostic> {
  auto notes = std::vector<diagnostic>{};
  auto first = true;
  for (const auto &frame : failure.frames) {
    if (!frame.file.has_value() || frame.span.empty()) {
      continue;
    }
    notes.push_back(
        diagnostic(diagnostic_level::note,
                   first
                       ? std::format("it failed here, in `{}`", frame.function)
                       : std::format("called from `{}`", frame.function),
                   *frame.file)
            .with_label(frame.span, first ? "failed here" : "called here"));
    first = false;
  }
  return notes;
}

auto checker::run_comptime_expr_on_vm(const ast::expr &expr)
    -> comptime::evaluator::vm_call_outcome {
  using outcome = comptime::evaluator::vm_call_outcome;
  if (!bodies_phase_ || comptime_backend_ == nullptr ||
      file_.module_ == nullptr) {
    return outcome{.outcome = outcome::kind::not_run};
  }
  // The bindings the expression reads that only the evaluator has a value
  // for — a `static for` loop variable, a compile-time `let` — become its
  // parameters. A binding it has none for is known only when the program
  // runs, so the expression cannot run now.
  auto request = comptime_expression{.expr = &expr,
                                     .owner_module = file_.module_->module_name,
                                     .file = file_.file_id_,
                                     .captures = {}};
  auto args = std::vector<comptime::value>{};
  auto seen = std::unordered_set<std::string>{};
  auto blocked = std::optional<std::string>{};
  // `at_type` is the binding's type as the expression reads it.
  const auto read = [&](const std::string &name, type_id at_type) -> void {
    if (blocked.has_value() || !seen.insert(name).second) {
      return;
    }
    if (const auto *held = comptime_eval_.local_value(name)) {
      // A type parameter is a type, not a value; lowering reads the types
      // it names from the records.
      if (held->kind == comptime::value_kind::type_value) {
        return;
      }
      const auto type = lowering_type(at_type);
      if (session_.types_.is_unknown(type)) {
        blocked = std::format("the type of `{}` is not known", name);
        return;
      }
      request.captures.emplace_back(name, type);
      args.push_back(*held);
      return;
    }
    if (find_value_in_scopes(name).binding != nullptr) {
      blocked = std::format("`{}` is known only when the program runs", name);
    }
  };
  ast::walk(expr, [&](const ast::node &node) -> bool {
    switch (node.kind) {
    case ast::node_kind::quote_expr:
      return false;
    case ast::node_kind::ident_expr: {
      const auto found = session_.node_types_.find(&node);
      read(ast::cast<const ast::ident_expr>(node).name,
           found == session_.node_types_.end() ? k_unknown_type
                                               : found->second);
      break;
    }
    case ast::node_kind::module_path_expr: {
      // A value-rooted path (`f.name`) records each segment's type, its
      // root's first.
      const auto &path = ast::cast<const ast::module_path_expr>(node);
      const auto found = session_.value_path_types_.find(&path);
      if (!path.segments.empty() && found != session_.value_path_types_.end() &&
          !found->second.empty()) {
        read(path.segments.front(), found->second.front());
      }
      break;
    }
    default:
      break;
    }
    return true;
  });
  static const auto trace = std::getenv("CINDER_COMPTIME_TRACE") != nullptr;
  if (blocked.has_value()) {
    if (trace) {
      std::println(stderr, "comptime vm: expression at {}: not run: {}",
                   expr.span.start, *blocked);
    }
    return outcome{.outcome = outcome::kind::not_run};
  }
  auto result = comptime_backend_->evaluate(*vm_program_, request, args);
  if (trace) {
    for (const auto &[name, type] : request.captures) {
      std::println(stderr, "comptime vm: expression at {} reads `{}`: {}",
                   expr.span.start, name, session_.types_.display(type));
    }
    std::println(stderr, "comptime vm: expression at {}: {}", expr.span.start,
                 result.has_value() ? std::string("ran")
                 : result.error().reason == comptime_failure::kind::unsupported
                     ? "not run: " + result.error().message
                     : "failed: " + result.error().message);
  }
  if (result.has_value()) {
    return outcome{.outcome = outcome::kind::returned,
                   .result = *std::move(result)};
  }
  if (result.error().reason == comptime_failure::kind::unsupported) {
    return outcome{.outcome = outcome::kind::not_run,
                   .message = std::move(result.error().message)};
  }
  // An expression that is one call fails as that call.
  auto what = std::string("compile-time evaluation");
  if (const auto *call = ast::as<const ast::call_expr>(&expr);
      call != nullptr && call->callee != nullptr) {
    const auto &callee = *call->callee;
    auto name = std::string{};
    if (const auto *ident = ast::as<const ast::ident_expr>(&callee)) {
      name = ident->name;
    } else if (const auto *field = ast::as<const ast::field_expr>(&callee)) {
      name = field->field_name;
    } else if (const auto *path =
                   ast::as<const ast::module_path_expr>(&callee);
               path != nullptr && !path->segments.empty()) {
      name = path->segments.back();
    }
    if (!name.empty()) {
      what = std::format("compile-time call to `{}`", name);
    }
  }
  return outcome{.outcome = outcome::kind::failed,
                 .result = {},
                 .message = std::format("{} failed: {}", what,
                                        result.error().message),
                 .notes = vm_failure_notes(result.error())};
}

auto checker::lowering_type(type_id id) -> type_id {
  return session_.types_.erase_refinements(settle(id));
}

auto checker::view_type(type_id id) -> type_id {
  const auto settled = lowering_type(id);
  if (lowering_view_state_.problem.has_value()) {
    return settled;
  }
  // Open types say the function is not finished: a leaf nothing has solved
  // yet, or a type parameter of a template. Lowering would read either as a
  // concrete type and get it wrong, so the function is refused instead.
  auto seen = std::unordered_set<type_id>{};
  auto pending = std::vector<type_id>{settled};
  while (!pending.empty()) {
    const auto current = pending.back();
    pending.pop_back();
    if (!seen.insert(current).second) {
      continue;
    }
    const auto &entry = session_.types_.entry(current);
    switch (entry.kind) {
    case type_kind::type_var_kind:
      lowering_view_state_.problem = std::format(
          "the type `{}` is not settled yet", session_.types_.display(settled));
      return settled;
    case type_kind::type_param_kind:
    case type_kind::param_app_kind:
    case type_kind::ctor_ref_kind:
    case type_kind::symbolic_value_kind:
      lowering_view_state_.problem = std::format(
          "the type `{}` is generic", session_.types_.display(settled));
      return settled;
    default:
      break;
    }
    for (const auto arg : entry.args) {
      pending.push_back(arg);
    }
    pending.push_back(entry.result);
  }
  return settled;
}

auto checker::prepare_for_lowering(const ast::func_decl &decl)
    -> std::expected<void, std::string> {
  if (lowering_view_state_.prepared.contains(&decl)) {
    return {};
  }
  if (decl.has_error) {
    return std::unexpected(std::string("its declaration has an error"));
  }
  if (decl.modifiers.is_intrinsic) {
    // Nothing to lower: the backends implement an intrinsic by name.
    lowering_view_state_.prepared.insert(&decl);
    return {};
  }
  const auto is_instance = std::ranges::any_of(
      session_.const_generic_instances_,
      [&decl](const auto &instance) -> bool { return instance.decl == &decl; });
  if (!decl.type_params.empty() && !is_instance) {
    return std::unexpected(std::string("it is a generic template"));
  }
  if (open_param_decls_.contains(&decl) && open_param_decls_.at(&decl)) {
    return std::unexpected(
        std::string("it is generic over its unannotated parameters"));
  }
  // A body the checker has not reached has no records at all. Its return
  // type is recorded on entry to `check_function`, so that is the sign.
  const auto checked =
      decl.return_type != nullptr
          ? session_.node_types_.contains(decl.return_type.get())
          : inferred_returns_.contains(&decl);
  if (!checked) {
    check_item_for_comptime(decl);
  }
  const auto checked_now =
      decl.return_type != nullptr
          ? session_.node_types_.contains(decl.return_type.get())
          : inferred_returns_.contains(&decl);
  if (!checked_now) {
    return std::unexpected(std::string("its body has not been checked yet"));
  }
  default_leaves_of(decl);
  // Checking an item early can run compile-time code, which can prepare
  // another function in the middle of this one: each keeps its own attempt.
  auto &state = lowering_view_state_;
  const auto saved_problem = std::exchange(state.problem, std::nullopt);
  const auto saved_nodes = std::exchange(state.attempt_nodes, {});
  const auto saved_signatures = std::exchange(state.attempt_signatures, {});
  const auto restore = [&]() -> void {
    state.problem = saved_problem;
    state.attempt_nodes = saved_nodes;
    state.attempt_signatures = saved_signatures;
  };
  refresh_lowering_view_tables();
  project_signature(decl);
  project_subtree(decl);
  for (const auto &contract : decl.contracts) {
    copy_member(session_.elided_contracts_,
                session_.lowering_view_.lowering.elided_contracts, &contract);
  }
  if (lowering_view_state_.problem.has_value()) {
    auto problem = *std::move(lowering_view_state_.problem);
    lowering_view_state_.problem.reset();
    // Forget what this attempt visited, so a retry once the open types have
    // settled copies them again. The records already copied stay until then:
    // nothing lowers this function before a later `prepare` succeeds.
    for (const auto *visited : lowering_view_state_.attempt_nodes) {
      lowering_view_state_.visited.erase(visited);
    }
    for (const auto *signature : lowering_view_state_.attempt_signatures) {
      lowering_view_state_.signatures.erase(signature);
    }
    restore();
    return std::unexpected(std::move(problem));
  }
  lowering_view_state_.prepared.insert(&decl);
  restore();
  return {};
}

auto checker::prepare_expression_for_lowering(const ast::expr &expr)
    -> std::expected<void, std::string> {
  default_leaves_of(expr);
  auto &state = lowering_view_state_;
  const auto saved_problem = std::exchange(state.problem, std::nullopt);
  const auto saved_nodes = std::exchange(state.attempt_nodes, {});
  const auto saved_signatures = std::exchange(state.attempt_signatures, {});
  const auto restore = [&]() -> void {
    state.problem = saved_problem;
    state.attempt_nodes = saved_nodes;
    state.attempt_signatures = saved_signatures;
  };
  refresh_lowering_view_tables();
  project_subtree(expr);
  if (state.problem.has_value()) {
    auto problem = *std::move(state.problem);
    for (const auto *visited : state.attempt_nodes) {
      state.visited.erase(visited);
    }
    for (const auto *signature : state.attempt_signatures) {
      state.signatures.erase(signature);
    }
    restore();
    return std::unexpected(std::move(problem));
  }
  restore();
  return {};
}

auto checker::project_signature(const ast::func_decl &decl) -> void {
  if (lowering_view_state_.signatures.contains(&decl)) {
    return;
  }
  // A callee's signature records are written when its item is checked, which
  // file order may not have reached yet.
  const auto checked = [&] -> bool {
    return decl.return_type != nullptr
               ? session_.node_types_.contains(decl.return_type.get())
               : inferred_returns_.contains(&decl);
  };
  if (!checked()) {
    check_item_for_comptime(decl);
  }
  // A generic instance a call named is made at the end of the file; its
  // parameters have no records until then.
  if (instances_.queued_template(decl) != nullptr) {
    make_instance_now(decl);
  }
  lowering_view_state_.signatures.insert(&decl);
  lowering_view_state_.attempt_signatures.push_back(&decl);
  // A callee's signature may be generic — a template whose instance the call
  // names — and lowering reads it as it is. Only the body being prepared
  // has to be concrete, so nothing here makes it unready.
  const auto saved_problem =
      std::exchange(lowering_view_state_.problem, std::nullopt);
  auto &view = session_.lowering_view_;
  for (const auto &param : decl.params) {
    if (param.pattern != nullptr) {
      project_subtree(*param.pattern);
    }
    if (param.type_annotation != nullptr) {
      project_subtree(*param.type_annotation);
    }
  }
  if (decl.return_type != nullptr) {
    project_subtree(*decl.return_type);
  }
  copy_entry(inferred_returns_, view.inferred_return_types, &decl,
             [this](type_id &type) -> void { type = view_type(type); });
  copy_entry(implicit_signatures_, view.ownership.implicit_signatures, &decl,
             [this](implicit_signature &signature) -> void {
               for (auto &param : signature.params) {
                 param = lowering_type(param);
               }
               signature.result = lowering_type(signature.result);
             });
  copy_entry(instance_templates_, view.ownership.instance_templates, &decl);
  lowering_view_state_.problem = saved_problem;
}

auto checker::project_callee(const resolved_callee &callee) -> void {
  if (callee.decl != nullptr) {
    project_signature(*callee.decl);
  }
}

auto checker::project_subtree(const ast::node &root) -> void {
  ast::walk(root, [this](const ast::node &node) -> bool {
    if (!lowering_view_state_.visited.insert(&node).second) {
      return false;
    }
    lowering_view_state_.attempt_nodes.push_back(&node);
    project_node(node);
    switch (node.kind) {
    case ast::node_kind::quote_expr:
      // Quoted syntax is data, not code this function runs — except the
      // splices evaluating it resolves.
      for (const auto *splice : comptime::fragment_builder::eager_splices(
               ast::cast<const ast::quote_expr>(node))) {
        const auto *operand =
            splice->kind == ast::node_kind::splice_expr
                ? ast::cast<const ast::splice_expr>(*splice).operand.get()
                : ast::cast<const ast::splice_type>(*splice).operand.get();
        if (operand != nullptr) {
          project_subtree(*operand);
        }
      }
      return false;
    case ast::node_kind::static_decl: {
      const auto *decl = ast::as<const ast::static_decl>(&node);
      if (decl == nullptr) {
        return true;
      }
      switch (decl->decl_kind) {
      case ast::static_decl_kind::conditional_compilation: {
        // Only the branch the checker took is ever lowered; the other may
        // not even type-check for this instance.
        const auto taken = session_.static_if_taken_branch_.find(decl);
        if (taken == session_.static_if_taken_branch_.end()) {
          return true;
        }
        for (const auto &item :
             taken->second ? decl->if_body : decl->else_body) {
          if (item != nullptr) {
            project_subtree(*item);
          }
        }
        return false;
      }
      case ast::static_decl_kind::assertion:
      case ast::static_decl_kind::binding:
        // Compile-time only: nothing here is lowered.
        return false;
      default:
        return true;
      }
    }
    default:
      return true;
    }
  });
}

auto checker::project_node(const ast::node &node) -> void {
  auto &view = session_.lowering_view_;
  auto &lowered = view.lowering;
  const auto fix_type = [this](type_id &type) -> void {
    type = view_type(type);
  };
  const auto fix_steps = [this](std::vector<deref_step> &steps) -> void {
    for (auto &step : steps) {
      step.callee_type = view_type(step.callee_type);
      step.result = view_type(step.result);
      project_callee(step.callee);
    }
  };
  const auto fix_dispatch = [this](comprehension_dispatch &dispatch) -> void {
    dispatch.list_type = view_type(dispatch.list_type);
    project_callee(dispatch.new_callee);
    project_callee(dispatch.push_callee);
  };
  const auto fix_iterator = [this](iterator_loop_dispatch &dispatch) -> void {
    dispatch.element_type = view_type(dispatch.element_type);
    dispatch.adapter_result_type = view_type(dispatch.adapter_result_type);
    if (dispatch.decl != nullptr) {
      project_signature(*dispatch.decl);
    }
    if (dispatch.adapter_decl != nullptr) {
      project_signature(*dispatch.adapter_decl);
    }
  };
  const auto fix_callee = [this](resolved_callee &callee) -> void {
    project_callee(callee);
  };

  copy_entry(session_.node_types_, view.node_types, &node, fix_type);
  copy_entry(session_.node_files_, view.node_files, &node);
  copy_entry(session_.resolved_fn_values_, lowered.resolved_fn_values, &node,
             fix_callee);
  copy_entry(session_.comprehension_iterator_dispatches_,
             view.comprehension_iterator_dispatches, &node, fix_iterator);
  copy_entry(session_.static_global_refs_, view.static_global_refs, &node);
  copy_entry(session_.unreadable_static_refs_, lowered.unreadable_static_refs,
             &node);
  copy_entry(session_.static_fragment_refs_, lowered.static_fragment_refs,
             &node);
  copy_entry(generic_lambda_refs_, view.ownership.generic_lambda_refs,
             static_cast<const void *>(&node));
  if (const auto found = session_.spliced_fragments_.find(&node);
      found != session_.spliced_fragments_.end()) {
    view.spliced_fragments.insert_or_assign(&node, found->second);
    if (found->second != nullptr) {
      project_subtree(*found->second);
    }
  }
  if (const auto found = session_.static_const_values_.find(&node);
      found != session_.static_const_values_.end()) {
    lowered.static_const_values.insert_or_assign(&node, found->second);
    if (found->second != nullptr) {
      project_subtree(*found->second);
    }
  }
  if (const auto found = session_.static_struct_values_.find(&node);
      found != session_.static_struct_values_.end()) {
    lowered.static_struct_values.insert_or_assign(&node, found->second);
    if (found->second != nullptr) {
      project_subtree(*found->second);
    }
  }

  switch (node.kind) {
  case ast::node_kind::call_expr: {
    const auto *call = ast::as<const ast::call_expr>(&node);
    if (call == nullptr) {
      break;
    }
    if (const auto found = session_.call_argument_mappings_.find(call);
        found != session_.call_argument_mappings_.end()) {
      view.call_argument_mappings.insert_or_assign(call, found->second);
      // An omitted argument lowers its default here, at the call site.
      for (const auto *default_value : found->second.defaults_by_param) {
        if (default_value != nullptr) {
          project_subtree(*default_value);
        }
      }
    }
    copy_entry(session_.resolved_callees_, view.resolved_callees, call,
               fix_callee);
    copy_entry(session_.type_param_reflections_, lowered.type_param_reflections,
               call);
    copy_entry(session_.layout_queries_, lowered.layout_queries, call,
               [this](layout_query &query) -> void {
                 query.operand = view_type(query.operand);
               });
    copy_entry(session_.ptr_casts_, lowered.ptr_casts, call, fix_type);
    copy_member(session_.slice_from_raw_parts_calls_,
                lowered.slice_from_raw_parts_calls, call);
    copy_entry(session_.stack_buffers_, lowered.stack_buffers, call,
               [this](stack_buffer_request &request) -> void {
                 request.element = view_type(request.element);
               });
    if (const auto found = session_.reflection_folds_.find(call);
        found != session_.reflection_folds_.end()) {
      auto value = found->second;
      settle_reified(value);
      lowered.reflection_folds.insert_or_assign(call, std::move(value));
    }
    if (const auto found = session_.folded_comptime_calls_.find(call);
        found != session_.folded_comptime_calls_.end()) {
      lowered.folded_comptime_calls.insert_or_assign(call, found->second);
      if (found->second != nullptr) {
        project_subtree(*found->second);
      }
    }
    break;
  }
  case ast::node_kind::binary_expr: {
    const auto *binary = ast::as<const ast::binary_expr>(&node);
    if (binary == nullptr) {
      break;
    }
    copy_entry(session_.operator_dispatches_, lowered.operator_dispatches,
               binary, fix_callee);
    copy_entry(session_.ord_dispatch_result_types_,
               lowered.ord_dispatch_result_types, binary, fix_type);
    break;
  }
  case ast::node_kind::index_expr: {
    const auto *index = ast::as<const ast::index_expr>(&node);
    if (index == nullptr) {
      break;
    }
    copy_entry(session_.index_dispatches_, lowered.index_dispatches, index,
               fix_callee);
    copy_entry(session_.index_set_dispatches_, lowered.index_set_dispatches,
               index, fix_callee);
    copy_entry(session_.index_mut_dispatches_, lowered.index_mut_dispatches,
               index, fix_callee);
    copy_entry(session_.index_ref_dispatches_, lowered.index_ref_dispatches,
               index, fix_callee);
    copy_member(session_.proven_in_bounds_, lowered.proven_in_bounds, index);
    break;
  }
  case ast::node_kind::array_expr: {
    const auto *array = ast::as<const ast::array_expr>(&node);
    if (array == nullptr) {
      break;
    }
    copy_entry(session_.array_literal_conversions_,
               lowered.array_literal_conversions, array,
               [this](array_literal_conversion &conversion) -> void {
                 conversion.array_type = view_type(conversion.array_type);
                 project_callee(conversion.callee);
               });
    copy_entry(session_.runtime_fill_dispatches_,
               lowered.runtime_fill_dispatches, array, fix_dispatch);
    break;
  }
  case ast::node_kind::for_expr: {
    const auto *for_expr = ast::as<const ast::for_expr>(&node);
    if (for_expr != nullptr) {
      copy_entry(session_.comprehension_dispatches_,
                 lowered.comprehension_dispatches, for_expr, fix_dispatch);
    }
    break;
  }
  case ast::node_kind::for_stmt: {
    const auto *loop = ast::as<const ast::for_stmt>(&node);
    if (loop != nullptr) {
      copy_entry(session_.for_iterator_dispatches_,
                 view.for_iterator_dispatches, loop, fix_iterator);
    }
    break;
  }
  case ast::node_kind::try_expr: {
    const auto *try_expr = ast::as<const ast::try_expr>(&node);
    if (try_expr == nullptr) {
      break;
    }
    copy_entry(session_.try_conversions_, lowered.try_conversions, try_expr,
               fix_callee);
    copy_entry(session_.try_conversion_types_, lowered.try_conversion_types,
               try_expr, fix_type);
    break;
  }
  case ast::node_kind::module_path_expr: {
    const auto *path = ast::as<const ast::module_path_expr>(&node);
    if (path == nullptr) {
      break;
    }
    copy_entry(session_.value_path_types_, view.value_path_types, path,
               [this](std::vector<type_id> &types) -> void {
                 for (auto &type : types) {
                   type = view_type(type);
                 }
               });
    copy_entry(session_.path_deref_adjustments_, view.path_deref_adjustments,
               path, [&](std::vector<std::vector<deref_step>> &segments) {
                 for (auto &steps : segments) {
                   fix_steps(steps);
                 }
               });
    break;
  }
  case ast::node_kind::struct_expr: {
    const auto *literal = ast::as<const ast::struct_expr>(&node);
    if (literal == nullptr) {
      break;
    }
    for (const auto &field : literal->fields) {
      copy_entry(session_.struct_literal_field_types_,
                 lowered.struct_literal_field_types, &field, fix_type);
      copy_entry(session_.struct_literal_field_expected_,
                 lowered.struct_literal_field_expected, &field, fix_type);
      copy_entry(generic_lambda_refs_, view.ownership.generic_lambda_refs,
                 static_cast<const void *>(&field));
    }
    break;
  }
  case ast::node_kind::struct_pattern: {
    const auto *pattern = ast::as<const ast::struct_pattern>(&node);
    if (pattern == nullptr) {
      break;
    }
    for (const auto &field : pattern->fields) {
      copy_entry(session_.struct_pattern_field_types_,
                 lowered.struct_pattern_field_types, &field, fix_type);
    }
    break;
  }
  case ast::node_kind::lambda_expr: {
    const auto *lambda = ast::as<const ast::lambda_expr>(&node);
    if (lambda == nullptr) {
      break;
    }
    if (const auto found = generic_lambda_instances_.find(lambda);
        found != generic_lambda_instances_.end()) {
      view.ownership.generic_lambda_instances.insert_or_assign(lambda,
                                                               found->second);
      for (const auto *copy : found->second) {
        if (copy != nullptr) {
          copy_entry(generic_lambda_uses_, view.ownership.generic_lambda_uses,
                     copy);
          project_subtree(*copy);
        }
      }
    }
    break;
  }
  case ast::node_kind::static_decl: {
    const auto *decl = ast::as<const ast::static_decl>(&node);
    if (decl == nullptr) {
      break;
    }
    copy_entry(session_.static_if_taken_branch_, lowered.static_if_taken_branch,
               decl);
    copy_entry(cache_.static_for_dispatches_, lowered.static_for_dispatches,
               decl, fix_dispatch);
    if (const auto found = cache_.static_for_elements_.find(decl);
        found != cache_.static_for_elements_.end()) {
      lowered.static_for_elements.insert_or_assign(decl, found->second);
      for (const auto *element : found->second) {
        if (element != nullptr) {
          project_subtree(*element);
        }
      }
    }
    if (const auto found = cache_.static_for_iterations_.find(decl);
        found != cache_.static_for_iterations_.end()) {
      lowered.static_for_iterations.insert_or_assign(decl, found->second);
      for (const auto &iteration : found->second) {
        for (const auto &[name, value] : iteration.bindings) {
          if (value != nullptr) {
            project_subtree(*value);
          }
        }
        for (const auto &[read, folded] : iteration.folded) {
          if (folded != nullptr) {
            project_subtree(*folded);
          }
        }
      }
    }
    break;
  }
  default:
    break;
  }

  if (const auto *expr = ast::as<const ast::expr>(&node); expr != nullptr) {
    copy_entry(session_.deref_adjustments_, view.deref_adjustments, expr,
               fix_steps);
    copy_entry(session_.interp_dispatches_, lowered.interp_dispatches, expr,
               [this](interp_dispatch &dispatch) -> void {
                 dispatch.value_type = view_type(dispatch.value_type);
                 if (dispatch.decl != nullptr) {
                   project_signature(*dispatch.decl);
                 }
               });
  }
}

auto checker::check_item_for_comptime(const ast::func_decl &decl) -> void {
  // The item holding `decl`, with the module and file it belongs to.
  const ast::node *item = nullptr;
  const module_members *owner = nullptr;
  auto file = file_id_type{0};
  const auto holds = [&decl](const std::vector<ast::ptr<ast::node>> &items) {
    return std::ranges::any_of(items, [&decl](const auto &member) -> bool {
      return member.get() == &decl;
    });
  };
  for (const auto &[module_name, members] : index_.modules) {
    if (const auto found = members.functions.find(decl.name);
        found != members.functions.end() && found->second.decl == &decl) {
      item = &decl;
      owner = &members;
      file = found->second.file_id;
      break;
    }
    for (const auto &ref : members.impls) {
      if (ref.decl != nullptr && holds(ref.decl->items)) {
        item = ref.decl;
        owner = index_.find_module(ref.module_name);
        file = ref.file_id;
      }
    }
    for (const auto &ref : members.extends) {
      if (ref.decl != nullptr && holds(ref.decl->items)) {
        item = ref.decl;
        owner = index_.find_module(ref.module_name);
        file = ref.file_id;
      }
    }
    if (item != nullptr) {
      break;
    }
  }
  const auto source = input_files_.find(file);
  if (item == nullptr || owner == nullptr || source == input_files_.end() ||
      items_checked_early_.contains(item) ||
      (static_cast<size_t>(file) < file_has_errors_.size() &&
       file_has_errors_[file])) {
    return;
  }
  // Marked first, so a compile-time call back into this item while it is
  // being checked does not start a second check of it.
  items_checked_early_.insert(item);
  const auto saved_item = std::exchange(checking_item_early_, item);
  const auto saved_no_prelude = file_.file_no_prelude_;
  const auto saved_wildcard = file_.file_has_external_wildcard_;
  auto module_level = body_context{};
  swap_body_context(module_level);
  [[maybe_unused]] const auto done =
      in_module_context(*owner, file, [&] -> bool {
        file_.file_no_prelude_ = source->second->no_prelude;
        compute_external_wildcard();
        push_scope();
        check_item(*item, /*at_module_scope=*/true);
        pop_scope();
        return true;
      });
  swap_body_context(module_level);
  file_.file_no_prelude_ = saved_no_prelude;
  file_.file_has_external_wildcard_ = saved_wildcard;
  checking_item_early_ = saved_item;
}

auto checker::default_leaves_of(const ast::node &root) -> void {
  auto nodes = std::unordered_set<const ast::node *>{};
  ast::walk(root, [&](const ast::node &node) -> bool {
    nodes.insert(&node);
    return node.kind != ast::node_kind::quote_expr;
  });
  // The leaves `root`'s records still hold, by representative.
  const auto open_leaves = [&] -> std::unordered_set<type_id> {
    auto leaves = std::unordered_set<type_id>{};
    auto seen = std::unordered_set<type_id>{};
    const auto collect = [&](this const auto &self, type_id id) -> void {
      if (!seen.insert(id).second) {
        return;
      }
      if (leaf_ctxt_.is_meta(id)) {
        leaves.insert(leaf_ctxt_.shallow_resolve(id));
        return;
      }
      const auto &entry = session_.types_.entry(id);
      for (const auto arg : entry.args) {
        self(arg);
      }
      self(entry.result);
    };
    for (const auto *node : nodes) {
      if (const auto found = session_.node_types_.find(node);
          found != session_.node_types_.end()) {
        collect(settle(found->second));
      }
    }
    return leaves;
  };
  const auto saved_file = file_.file_id_;
  const auto *saved_module = file_.module_;
  const auto *saved_template = function_.current_template_;
  // Defaulting a leaf can let a deferred literal be wired, and wiring one
  // can solve a receiver a deferred method call waits on — so the two
  // repeat until neither has anything left to do in `root`.
  for (auto round = 0; round < 8; ++round) {
    const auto leaves = open_leaves();
    // Runs the queue's other obligations to a fixpoint as well, which is
    // what finishes a method call whose receiver is now known. A literal
    // that does not fit its default is reported at file end, like every
    // other literal, by `flush_leaf_values`.
    (void)leaf_queue_.flush([&](type_id leaf) -> bool {
      return leaves.contains(leaf_ctxt_.shallow_resolve(leaf));
    });
    auto wired = false;
    // By index: wiring one literal can defer another.
    for (std::size_t i = 0; i < pending_leaf_literals_.size(); ++i) {
      if (pending_leaf_literals_[i].skip ||
          !nodes.contains(pending_leaf_literals_[i].literal)) {
        continue;
      }
      pending_leaf_literals_[i].skip = true;
      const auto leaf = pending_leaf_literals_[i];
      settle_leaf_literal(leaf);
      wired = true;
    }
    if (!wired) {
      break;
    }
  }
  file_.file_id_ = saved_file;
  file_.module_ = saved_module;
  function_.current_template_ = saved_template;
}

auto checker::refresh_lowering_view_tables() -> void {
  auto &view = session_.lowering_view_;
  auto &lowered = view.lowering;
  // The instance lists are short and only ever grow; lowering scans them
  // to recognize an instance, so they are copied whole.
  lowered.const_generic_instances = session_.const_generic_instances_;
  lowered.synthesized_trait_defaults = session_.synthesized_trait_defaults_;
  lowered.comptime_only_functions = session_.comptime_only_functions_;
  lowered.static_global_owners = session_.static_global_owners_;
  lowered.open_param_templates.clear();
  for (const auto &[decl, open] : open_param_decls_) {
    if (open) {
      lowered.open_param_templates.insert(decl);
    }
  }
  view.ownership.copy_type_params = copy_type_params_;
  if (lowering_view_state_.derived_type_count == session_.types_.count()) {
    return;
  }
  // Computing these interns types, so loop until the table stops growing.
  while (lowering_view_state_.derived_type_count != session_.types_.count()) {
    const auto first = lowering_view_state_.derived_type_count;
    // Drop plans for the types interned since last time, so compile-time
    // code drops what it owns exactly as the same code does at run time.
    resolve_drop_plans_from(first);
    // A drop call is built from its plan, not from a record on a node, and
    // lowering reads the `drop` method's parameters to pass the receiver by
    // reference — so each plan's method is projected with the plan.
    auto drop_methods = std::vector<resolved_callee>{};
    for (std::size_t raw = 0; raw < session_.types_.count(); ++raw) {
      const auto id = static_cast<type_id>(raw);
      if (view.drop.drop_plans.contains(id)) {
        continue;
      }
      if (const auto plan = session_.drop_plans_.find(id);
          plan != session_.drop_plans_.end()) {
        if (plan->second.own_drop.has_value()) {
          drop_methods.push_back(*plan->second.own_drop);
        }
        view.drop.drop_plans.emplace(id, plan->second);
        continue;
      }
      auto scratch = std::unordered_set<type_id>{};
      if (session_.types_.entry(id).kind == type_kind::fn_kind &&
          !mentions_type_var(id, scratch)) {
        view.drop.drop_plans.emplace(
            id, drop_plan{.closure_glue = session_.closure_glue_type_});
      }
    }
    // Whether a type's `drop` is ready to lower says nothing about the
    // function being prepared; that one is lowered only if a call needs it.
    const auto saved_problem =
        std::exchange(lowering_view_state_.problem, std::nullopt);
    for (const auto &method : drop_methods) {
      project_callee(method);
    }
    lowering_view_state_.problem = saved_problem;
    lowering_view_state_.derived_type_count = session_.types_.count();
    lowered.fmt_types = resolve_fmt_runtime_types();
    const auto count = session_.types_.count();
    view.ownership.borrow_bearing_types = compute_borrow_bearing_types(count);
    view.ownership.own_borrow_bearing_types =
        compute_borrow_bearing_types(count, /*params_bear=*/false);
    view.ownership.copy_sum_types.clear();
    for (std::size_t raw = 0; raw < count; ++raw) {
      const auto id = static_cast<type_id>(raw);
      if (is_sum_like(id) && value_is_copy(id)) {
        view.ownership.copy_sum_types.insert(id);
      }
    }
  }
}

auto checker::refresh_function_index() -> void {
  auto &state = lowering_view_state_;
  const auto sizes = std::array<std::size_t, 5>{
      index_.modules.size(), session_.const_generic_instances_.size(),
      session_.synthesized_trait_defaults_.size(),
      functor_.functor_instance_decls_.size(),
      session_.synthesized_item_splices_.size()};
  if (sizes == state.indexed_sizes) {
    return;
  }
  state.indexed_sizes = sizes;
  auto &index = state.functions_by_name;
  index.clear();
  state.names_by_decl.clear();
  // Mirrors how `hir::lower_module` names what it lowers, so a name a
  // lowered call uses finds the declaration lowering would have compiled.
  const auto target_name =
      [](const ast::type_expr *type) -> std::optional<std::string> {
    const auto *named =
        ast::as_kind<const ast::named_type>(type, ast::node_kind::named_type);
    if (named == nullptr || named->path.empty() || !named->type_args.empty()) {
      return std::nullopt;
    }
    return named->path.back();
  };
  const auto add_methods =
      [&](std::unordered_map<std::string, const ast::func_decl *> &into,
          const std::vector<ast::ptr<ast::node>> &items,
          const std::string &target) -> void {
    for (const auto &item : items) {
      if (const auto *method = ast::as<const ast::func_decl>(item.get())) {
        into.emplace(std::format("{}::{}", target, method->name), method);
      }
    }
  };
  for (const auto &[module_name, members] : index_.modules) {
    auto &into = index[module_name];
    for (const auto &[name, ref] : members.functions) {
      if (ref.decl != nullptr) {
        into.emplace(name, ref.decl);
      }
    }
    for (const auto &ref : members.impls) {
      if (ref.decl == nullptr || !ref.decl->type_params.empty()) {
        continue;
      }
      if (const auto target = target_name(ref.decl->for_type.get())) {
        add_methods(index[ref.module_name], ref.decl->items, *target);
      }
    }
    for (const auto &ref : members.extends) {
      if (ref.decl == nullptr) {
        continue;
      }
      if (const auto target = target_name(ref.decl->for_type.get())) {
        add_methods(index[ref.module_name], ref.decl->items, *target);
      }
    }
  }
  for (const auto &synthesized : session_.synthesized_trait_defaults_) {
    if (synthesized.decl != nullptr) {
      index[synthesized.owner_module].emplace(
          std::format("{}::{}", synthesized.target_type_name,
                      synthesized.decl->name),
          synthesized.decl);
    }
  }
  for (const auto &instance : session_.const_generic_instances_) {
    if (instance.decl != nullptr) {
      index[instance.owner_module].emplace(instance.decl->name, instance.decl);
    }
  }
  for (const auto &instance : functor_.functor_instance_decls_) {
    if (instance.decl == nullptr) {
      continue;
    }
    index[instance.owner_module].emplace(
        instance.impl_target.empty()
            ? instance.decl->name
            : std::format("{}::{}", instance.impl_target, instance.decl->name),
        instance.decl);
  }
  for (const auto &splice : session_.synthesized_item_splices_) {
    if (splice.impl == nullptr) {
      continue;
    }
    if (const auto target = target_name(splice.impl->for_type.get())) {
      add_methods(index[splice.owner_module], splice.impl->items, *target);
    }
  }
  index_names(index, state.names_by_decl);
}

auto checker::project_static_global(std::string_view name)
    -> const lowering_results::static_global_def * {
  auto &defs = session_.lowering_view_.lowering.static_global_defs;
  for (const auto &[decl, def] : defs) {
    if (def.name == name) {
      return &def;
    }
  }
  for (const auto &[decl, def] : session_.static_global_defs_) {
    if (def.name != name) {
      continue;
    }
    auto copy = def;
    copy.type = lowering_type(copy.type);
    settle_reified(copy.value);
    return &defs.insert_or_assign(decl, std::move(copy)).first->second;
  }
  return nullptr;
}

auto checker::settle_reified(reified_value &value) -> void {
  value.type = lowering_type(value.type);
  if (value.conversion != nullptr) {
    project_subtree(*value.conversion);
  }
  for (auto &part : value.parts) {
    settle_reified(part);
  }
}

auto checker::find_lowered_function(std::string_view module,
                                    std::string_view name)
    -> std::optional<comptime_function> {
  refresh_function_index();
  const auto in_module =
      lowering_view_state_.functions_by_name.find(std::string(module));
  const auto found =
      in_module == lowering_view_state_.functions_by_name.end()
          ? std::unordered_map<std::string, const ast::func_decl *>::iterator{}
          : in_module->second.find(std::string(name));
  if (in_module == lowering_view_state_.functions_by_name.end() ||
      found == in_module->second.end()) {
    // A generic instance a call has named but the end-of-file flush has not
    // made yet: make it now.
    const ast::func_decl *named = nullptr;
    for (const auto &[key, instance] : session_.hk_instance_cache_) {
      if (instance == nullptr || instance->name != name) {
        continue;
      }
      const auto owner = function_owner(*instance);
      if (owner.has_value() && owner->first->module_name == module) {
        named = instance;
        break;
      }
    }
    if (named == nullptr) {
      return std::nullopt;
    }
    // After the loop: making the instance can request others, which grows
    // the cache being iterated.
    make_instance_now(*named);
    return comptime_function{.decl = named,
                             .owner_module = std::string(module),
                             .hir_name = std::string(name)};
  }
  return comptime_function{.decl = found->second,
                           .owner_module = std::string(module),
                           .hir_name = std::string(name)};
}

} // namespace detail
} // namespace cinder::semantic
