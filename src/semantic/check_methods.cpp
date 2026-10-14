#include "checker.h"
#include "src/parser/ast_traversal.h"

namespace cinder::semantic::detail {

/// Extracts the trailing name of an impl's trait-type path (e.g. `show`
/// from `impl show for point:`), or empty for an inherent impl with no
/// trait.
auto checker::trait_name_of_impl(const ast::impl_decl &impl) -> std::string {
  if (impl.trait_type == nullptr ||
      impl.trait_type->kind != ast::node_kind::named_type) {
    return {};
  }
  const auto &named = ast::cast<const ast::named_type>(*impl.trait_type);
  return named.path.empty() ? std::string{} : named.path.back();
}

/// A stable string key identifying a type for impl-coherence bookkeeping:
/// the declaration's address for user types, or the type's display name
/// otherwise.
///
/// Deliberately *declaration*-level: every instantiation of a generic type
/// shares one key. That is what `type_has_trait` needs — asking whether
/// `holder[int32]` implements `show` has to find an `impl[T] show for
/// holder[T]` — so the key cannot carry type arguments. Whether two impls
/// sharing a key actually *conflict* is a separate, finer question, and
/// `impl_targets_overlap` answers it.
auto checker::type_key_of(const type_entry &entry) -> std::string {
  if (entry.decl != nullptr) {
    return std::format("u:{}", static_cast<const void *>(entry.decl));
  }
  return std::format("n:{}", entry.name);
}

/// Whether two impls of the same trait, on targets sharing a coherence key
/// (so: on the same type declaration), actually apply to a common concrete
/// type — the thing the reference forbids ("no two can apply to the same
/// concrete type").
///
/// Two *distinct fully-concrete* instantiations never do: `impl get_it for
/// boxed[int32]` and `impl get_it for boxed[int64]` are implementations for
/// two different types that happen to come from one declaration, and the
/// reference blesses exactly this ("a specific `impl show for point` and a
/// blanket `impl[T: show] show for list[T]` coexist"). Keying coherence at
/// the declaration level alone used to reject them.
///
/// Anything else is treated as overlapping. That is deliberately
/// conservative: a generic target could in principle be proven disjoint
/// from another by unification, but a blanket impl and a concrete one for
/// the same declaration usually *do* overlap, and over-reporting a real
/// ambiguity is the safe direction for a guarantee the standard library
/// leans on.
auto checker::impl_targets_overlap(type_id first, type_id second) -> bool {
  if (first == second) {
    return true;
  }
  const auto resolver = method_lookup();
  return resolver.mentions_param(first) || resolver.mentions_param(second);
}

/// Finds a trait declaration named `name` in the current module or
/// reachable through a non-wildcard import.
auto checker::find_session_trait(std::string_view name)
    -> std::optional<trait_decl_ref> {
  if (file_.module_ != nullptr) {
    if (const auto it = file_.module_->traits.find(std::string(name));
        it != file_.module_->traits.end()) {
      return it->second;
    }
  }
  if (const auto *binding = find_import(name)) {
    if (const auto *source = import_source_module(*binding)) {
      if (const auto it = source->traits.find(imported_member_name(*binding));
          it != source->traits.end()) {
        return it->second;
      }
    }
  }
  for (const auto *source : wildcard_import_sources()) {
    if (const auto it = source->traits.find(std::string(name));
        it != source->traits.end()) {
      return it->second;
    }
  }
  return find_prelude_trait(name);
}

/// Finds a trait declaration named `name` in the auto-imported prelude —
/// module `prelude` itself, or one of the stdlib modules it re-exports
/// (currently `std.traits`, home of `from`/`drop`) — without requiring an
/// explicit `use`. Every real invocation of the compiler injects
/// `prelude.cn`/`std/traits.cn` into the session (`compile_sources`,
/// `src/driver/driver.cpp`); a file that writes `no_prelude` opts out.
/// Absent from a session that never included those files (most unit
/// tests), this is simply inert.
auto checker::find_prelude_trait(std::string_view name)
    -> std::optional<trait_decl_ref> {
  if (file_.file_no_prelude_) {
    return std::nullopt;
  }
  for (const auto module_name : k_prelude_reexport_modules) {
    if (const auto *source = index_.find_module(module_name)) {
      if (const auto it = source->traits.find(std::string(name));
          it != source->traits.end()) {
        return it->second;
      }
    }
  }
  return std::nullopt;
}

/// Finds a `concept` declaration named `name` in the auto-imported
/// prelude — `std.traits`'s category concepts (`numeric`, `integer`, ...)
/// are bounds a body needs as routinely as `ord`, so they are reachable the
/// same way. Mirrors `find_prelude_trait`.
auto checker::find_prelude_concept(std::string_view name)
    -> std::optional<concept_decl_ref> {
  if (file_.file_no_prelude_) {
    return std::nullopt;
  }
  for (const auto module_name : k_prelude_reexport_modules) {
    if (const auto *source = index_.find_module(module_name)) {
      if (const auto it = source->concepts.find(std::string(name));
          it != source->concepts.end()) {
        return it->second;
      }
    }
  }
  return std::nullopt;
}

/// Finds a `type` declaration named `name` in the auto-imported prelude.
/// Mirrors `find_prelude_trait`, one map over instead of `traits`, and
/// returns the declaring module because a user type's identity includes it
/// — interning it under the *current* module would mint a second `ordering`
/// distinct from the one `cmp` returns.
auto checker::find_prelude_type(std::string_view name)
    -> std::optional<std::pair<const ast::type_decl *, std::string>> {
  if (file_.file_no_prelude_) {
    return std::nullopt;
  }
  for (const auto module_name : k_prelude_reexport_modules) {
    if (const auto *source = index_.find_module(module_name)) {
      if (const auto it = source->types.find(std::string(name));
          it != source->types.end()) {
        return std::pair{it->second.decl, source->module_name};
      }
    }
  }
  return std::nullopt;
}

auto checker::find_prelude_variant(std::string_view name)
    -> std::optional<prelude_variant_ref> {
  if (file_.file_no_prelude_) {
    return std::nullopt;
  }
  for (const auto module_name : k_prelude_reexport_modules) {
    if (const auto *source = index_.find_module(module_name)) {
      if (const auto it = source->variants.find(std::string(name));
          it != source->variants.end()) {
        return prelude_variant_ref{.sum_decl = it->second.sum_decl,
                                   .variant = it->second.variant,
                                   .module_name = source->module_name};
      }
    }
  }
  return std::nullopt;
}

/// Finds a free function named `name` in the auto-imported prelude —
/// currently just `std.console` (home of `println`/`print`/`eprintln`/
/// `eprint`) — without requiring an explicit `use`. Mirrors
/// `find_prelude_trait` exactly, one module list for functions instead of
/// traits, except it also returns the declaring module (`fn_type_of`/
/// `check_call_against_decl` need it as the function's "owner", the same
/// way the `find_import` call branch a few lines below passes its own
/// resolved `source` module through); see `find_prelude_trait`'s doc
/// comment for the auto-injection mechanism (`inject_stdlib_prelude`,
/// `src/driver/driver.cpp`) this depends on.
auto checker::find_prelude_function(std::string_view name)
    -> std::optional<std::pair<func_decl_ref, const module_members *>> {
  if (file_.file_no_prelude_) {
    return std::nullopt;
  }
  // `std.iter` joins `std.console` because the iteration entry points are
  // prelude-level vocabulary in the same way `println` is: §5.2's families
  // (`iter`, `iter_mut`, `into_iter`) and `collect` are meant to be written
  // unqualified, and under UFCS `xs.iter().collect()` is the spelling the
  // design assumes throughout. Traits from `std.iter` were already reachable
  // via `find_prelude_trait`; its free functions were not, so `collect` was
  // an undefined name in every module that did not import it by hand.
  // `std.algo` joins them for the same reason: the whole catalog is free
  // functions reached by UFCS, so `xs.iter().map(f).filter(p)` only reads
  // that way if `map` and `filter` resolve unqualified.
  // `std.traits` joins them for a narrower reason: its `ord_cmp`/`ord_then`/
  // `ord_equal` (and, for `deriving hash`, `hash_seed`/`hash_combine`/
  // `hash_value`/`hash_tag`) are the combinators a *generated* body calls
  // (`src/std/deriving.cn`), and that body is spliced into the user's own
  // module, where those names have to resolve without an import the user
  // never wrote.
  static constexpr std::array<std::string_view, 4> k_prelude_function_modules =
      {"std.console", "std.iter", "std.algo", "std.traits"};
  for (const auto module_name : k_prelude_function_modules) {
    if (const auto *source = index_.find_module(module_name)) {
      if (const auto it = source->functions.find(std::string(name));
          it != source->functions.end()) {
        return std::pair{it->second, source};
      }
    }
  }
  return std::nullopt;
}

/// The declaration a trait name refers to from inside `home`: that module
/// first, then anywhere else in the session. Null for an empty name or a
/// trait the session does not declare.
auto checker::resolve_trait_decl_from(const std::string &trait_name,
                                      const module_members *home)
    -> const ast::trait_decl * {
  if (trait_name.empty()) {
    return nullptr;
  }
  if (home != nullptr) {
    if (const auto it = home->traits.find(trait_name);
        it != home->traits.end()) {
      return it->second.decl;
    }
  }
  for (const auto &[module_name, other] : index_.modules) {
    if (const auto it = other.traits.find(trait_name);
        it != other.traits.end()) {
      return it->second.decl;
    }
  }
  return nullptr;
}

/// The arity of the trait's constructor parameter when `impl`'s trait
/// abstracts over a type constructor (`trait monad[M[_]]`) — the fact
/// that makes a *bare* generic name legal as the impl's `for` target
/// (`impl monad for option` names the constructor, not an instantiation).
/// `nullopt` for ordinary traits and trait-less inherent impls. `home` is
/// the impl's own module, searched first; the trait may live anywhere in
/// the session.
auto checker::impl_expected_ctor_arity(const ast::impl_decl &impl,
                                       const module_members *home)
    -> std::optional<size_t> {
  const auto *trait_decl =
      resolve_trait_decl_from(trait_name_of_impl(impl), home);
  if (trait_decl == nullptr || trait_decl->type_params.empty()) {
    return std::nullopt;
  }
  const auto arity = trait_decl->type_params.front().higher_kinded_arity;
  return arity > 0 ? std::optional<size_t>{arity} : std::nullopt;
}

/// Resolves the concrete type an impl block targets (its `for` clause),
/// against the impl's own generic parameters. For an impl of a
/// higher-kinded trait the target resolves to the unapplied constructor
/// (`ctor_ref_kind`) rather than an instantiation.
/// `impl_trait_arguments_key` for a session-wide `impl_ref`, resolved in
/// the impl's *own* module with its block parameters rigid — the same
/// context `resolve_impl_target` builds, so the two halves of a coherence
/// key are read the same way.
auto checker::resolve_impl_trait_key(const impl_ref &impl,
                                     std::string_view trait_name)
    -> std::string {
  auto param_bindings = type_scope{};
  for (const auto &param : impl.decl->type_params) {
    if (!param.name.empty()) {
      param_bindings.emplace(
          param.name, session_.types_.type_param(
                          param.name, param.higher_kinded_arity, &param));
    }
  }
  const auto ctx = resolve_ctx{.module = index_.find_module(impl.module_name),
                               .param_bindings = &param_bindings,
                               .use_type_param_stack = false,
                               .quiet = true};
  return impl_trait_arguments_key(*impl.decl, trait_name, ctx);
}

/// The single trait argument an impl supplies, as a type rather than as a
/// key fragment — `usize` for `impl index[usize] for list[T]`, and
/// `k_unknown_type` for a trait taking no arguments (or taking more than
/// the one `impl_trait_arguments_key` keys on). Resolved in exactly the
/// context `resolve_impl_trait_key` uses, so a block parameter comes back
/// as a rigid `type_param` and `impl_targets_overlap` can see it for what
/// it is.
auto checker::resolve_impl_trait_argument(const impl_ref &impl) -> type_id {
  if (impl.decl->trait_type == nullptr ||
      impl.decl->trait_type->kind != ast::node_kind::named_type) {
    return k_unknown_type;
  }
  const auto &named = ast::cast<const ast::named_type>(*impl.decl->trait_type);
  if (named.type_args.size() != 1) {
    return k_unknown_type;
  }
  const auto *argument =
      ast::as<const ast::type_expr>(named.type_args.front().value.get());
  if (argument == nullptr) {
    return k_unknown_type;
  }
  auto param_bindings = type_scope{};
  for (const auto &param : impl.decl->type_params) {
    if (!param.name.empty()) {
      param_bindings.emplace(
          param.name, session_.types_.type_param(
                          param.name, param.higher_kinded_arity, &param));
    }
  }
  const auto ctx = resolve_ctx{.module = index_.find_module(impl.module_name),
                               .param_bindings = &param_bindings,
                               .use_type_param_stack = false,
                               .quiet = true};
  return resolve_type(*argument, ctx);
}

auto checker::resolve_impl_target(const impl_ref &impl) -> type_id {
  if (impl.decl->for_type == nullptr) {
    return k_unknown_type;
  }
  auto param_bindings = type_scope{};
  for (const auto &param : impl.decl->type_params) {
    if (!param.name.empty()) {
      param_bindings.emplace(
          param.name, session_.types_.type_param(
                          param.name, param.higher_kinded_arity, &param));
    }
  }
  const auto *home = index_.find_module(impl.module_name);
  const auto ctx = resolve_ctx{.module = home,
                               .param_bindings = &param_bindings,
                               .use_type_param_stack = false,
                               .quiet = true,
                               .expected_ctor_arity =
                                   impl_expected_ctor_arity(*impl.decl, home)};
  return resolve_type(*impl.decl->for_type, ctx);
}

/// Resolves the type an extend block targets (its `extend` clause) as a
/// *pattern*: a parameterized block's own parameters resolve to rigid
/// `type_param` stand-ins, so `extend[T] holder[T]` yields `holder[T]` for
/// later unification against a concrete receiver, exactly as
/// `resolve_impl_target` does for `impl`.
auto checker::resolve_extend_target(const extend_ref &ext) -> type_id {
  if (ext.decl->for_type == nullptr) {
    return k_unknown_type;
  }
  auto param_bindings = type_scope{};
  for (const auto &param : ext.decl->type_params) {
    if (!param.name.empty()) {
      param_bindings.emplace(
          param.name, session_.types_.type_param(
                          param.name, param.higher_kinded_arity, &param));
    }
  }
  const auto ctx = resolve_ctx{.module = index_.find_module(ext.module_name),
                               .param_bindings = &param_bindings,
                               .use_type_param_stack = false,
                               .quiet = true};
  return resolve_type(*ext.decl->for_type, ctx);
}

/// Builds `cache_.method_tables_.declared` once per session (idempotent via
/// `cache_.methods_built_`): for every impl block in every module, records its
/// methods against the impl's target type declaration, plus — for a trait impl
/// — the target trait's default-bodied methods, so calling a non-overridden
/// default method through the impl works without duplicating its body per type.
/// Also records every `extend` block's methods, against the target's
/// `type_decl` when it names a user type or against the target's builtin
/// name (`cache_.method_tables_.extend_by_builtin`) otherwise.
///
/// Trait-default monomorphization is deferred to a second pass (below,
/// `deferred_defaults`) rather than run inline as each impl is visited:
/// `monomorphize_trait_default` type-checks a clone of the default body,
/// and that check can itself call `find_method`/`build_method_table`
/// reentrantly (e.g. a default `read_to_end` calling `list[byte].push`).
/// `cache_.methods_built_` is already `true` by then, so a reentrant call would
/// see whatever subset of
/// `cache_.method_tables_.declared`/`cache_.method_tables_.extend_by_builtin`
/// this loop happened to have populated so far — order-dependent on
/// `index_.modules`'s (unordered) iteration order, i.e. on which module
/// the compile's entry point happens to be (spec/todo.md item 18). Doing
/// every extend/impl registration for every module first, then
/// monomorphizing defaults only once that's complete, means a reentrant
/// lookup during monomorphization always sees the fully built table.
auto checker::build_method_table() -> void {
  if (cache_.methods_built_) {
    return;
  }
  cache_.methods_built_ = true;

  auto deferred_defaults = std::vector<deferred_trait_default>{};

  for (const auto &[module_name, members] : index_.modules) {
    for (const auto &ext : members.extends) {
      const auto target = strip_refs(resolve_extend_target(ext));
      const auto &target_entry = session_.types_.entry(target);
      auto extend_methods = std::deque<method_entry>{};
      for (const auto &item : ext.decl->items) {
        if (item == nullptr || item->has_error ||
            item->kind != ast::node_kind::func_decl) {
          continue;
        }
        extend_methods.push_back(method_entry{
            .decl = ast::as<const ast::func_decl>(item.get()),
            .owner = &members,
            .from_trait = nullptr,
            .is_extension = true,
            // An `extend` block is an inherent impl, and gets the same
            // per-receiver instance discipline: without these two an
            // `extend` on a generic type type-checked and then compiled
            // nothing at all (spec/todo.md items 7 and 11).
            .file_id = ext.file_id,
            .block_type_params = &ext.decl->type_params,
            .impl_target_pattern = target,
        });
      }
      if (extend_methods.empty()) {
        continue;
      }
      if (target_entry.decl != nullptr) {
        auto &methods = cache_.method_tables_.declared[target_entry.decl];
        methods.insert(methods.end(), extend_methods.begin(),
                       extend_methods.end());
      } else if (!target_entry.name.empty()) {
        auto &methods =
            cache_.method_tables_.extend_by_builtin[target_entry.name];
        methods.insert(methods.end(), extend_methods.begin(),
                       extend_methods.end());
      }
    }

    for (const auto &impl : members.impls) {
      register_impl_methods(members, impl, deferred_defaults);
    }
  }

  monomorphize_trait_defaults(deferred_defaults);
}

/// Registers one `impl` block's methods in the method table: its own methods
/// and, for a trait impl, the trait's methods it does not override. A
/// default-bodied trait method is queued on `deferred_defaults` rather than
/// monomorphized here — see `build_method_table`.
auto checker::register_impl_methods(
    const module_members &members, const impl_ref &impl,
    std::vector<deferred_trait_default> &deferred_defaults) -> void {
  const auto target = strip_refs(resolve_impl_target(impl));
  const auto &target_entry = session_.types_.entry(target);
  // An impl of a higher-kinded trait for a *prelude* constructor
  // (`impl monad for option`) has no `type_decl` to key
  // `cache_.method_tables_.declared` by; its methods go in the
  // constructor-name-keyed table instead.
  const auto is_builtin_ctor_target =
      target_entry.kind == type_kind::ctor_ref_kind &&
      target_entry.decl == nullptr;
  // An impl for an *applied* prelude generic (`impl from_iter[int32]
  // for list[int32]`). Like the bare-constructor case it has no
  // `type_decl` to key `cache_.method_tables_.declared` by, and it used to fall
  // into the `continue` below: the impl was parsed and coherence-checked, then
  // silently never registered, so its members did not exist as far as
  // every lookup was concerned. `collect`'s collector impls are all
  // this shape, which is why `list[int32].from_iter(...)` reported no
  // such static member however it was written.
  const auto is_builtin_applied_target =
      target_entry.kind == type_kind::builtin_generic_kind &&
      target_entry.decl == nullptr && !target_entry.name.empty();
  // An impl for a builtin *scalar* (`impl zero for int32`). The third
  // instance of the same shape: no `type_decl`, so it fell into the
  // `continue` and was parsed, coherence-checked, and then silently
  // never registered. `int32` and friends are where the numeric
  // identity traits have to land, since nothing else can name a zero.
  const auto is_builtin_scalar_target =
      target_entry.kind == type_kind::builtin_kind &&
      target_entry.decl == nullptr && !target_entry.name.empty();
  if (target_entry.decl == nullptr && !is_builtin_ctor_target &&
      !is_builtin_applied_target && !is_builtin_scalar_target) {
    return;
  }
  auto &methods = (is_builtin_ctor_target || is_builtin_applied_target ||
                   is_builtin_scalar_target)
                      ? cache_.method_tables_.impl_by_builtin[target_entry.name]
                      : cache_.method_tables_.declared[target_entry.decl];
  auto overridden_names = std::unordered_set<std::string>{};
  // Owned copy of `target_entry.name`, taken up front: `target_entry`
  // is a reference into `type_table`'s backing store
  // (`type_table::entries_`, a `std::vector`), and
  // `monomorphize_trait_default` below calls `check_function` on each
  // cloned default body, which can intern new types and reallocate
  // that vector — silently invalidating `target_entry` (and any
  // `string_view` still pointing into it) for every default *after*
  // the first in this same impl. Reading through an owned string
  // instead of `target_entry.name` keeps every call in this loop
  // using a name that's still valid no matter how much interning the
  // previous call triggered.
  const auto target_type_name = std::string(target_entry.name);

  // Trait methods with default bodies are callable through the impl;
  // also recorded on the impl's own overridden methods below, so a
  // caller can identify a method by the trait it implements.
  const auto trait_name = trait_name_of_impl(*impl.decl);

  for (const auto &item : impl.decl->items) {
    if (item == nullptr || item->has_error ||
        item->kind != ast::node_kind::func_decl) {
      continue;
    }
    const auto *decl = ast::as<const ast::func_decl>(item.get());
    overridden_names.insert(decl->name);
    methods.push_back(method_entry{
        .decl = decl,
        .owner = &members,
        .from_trait = nullptr,
        .is_extension = false,
        .trait_name = trait_name,
        .file_id = impl.file_id,
        .block_type_params = &impl.decl->type_params,
        .impl_target_pattern = target,
    });
  }

  if (trait_name.empty()) {
    return;
  }
  const auto *trait_module = &members;
  const ast::trait_decl *trait_decl = nullptr;
  auto trait_file_id = file_id_type{0};
  if (const auto it = members.traits.find(trait_name);
      it != members.traits.end()) {
    trait_decl = it->second.decl;
    trait_file_id = it->second.file_id;
  } else {
    for (const auto &[other_name, other] : index_.modules) {
      if (const auto other_it = other.traits.find(trait_name);
          other_it != other.traits.end()) {
        trait_decl = other_it->second.decl;
        trait_file_id = other_it->second.file_id;
        trait_module = &other;
        break;
      }
    }
  }
  if (trait_decl == nullptr) {
    return;
  }
  for (const auto &item : trait_decl->items) {
    if (item == nullptr || item->kind != ast::node_kind::func_decl) {
      continue;
    }
    const auto *decl = ast::as<const ast::func_decl>(item.get());
    const auto first_param = decl->params.empty()
                                 ? std::string{}
                                 : param_name_of(decl->params.front());
    const auto action =
        classify_trait_item(*decl, first_param, overridden_names);
    if (action == trait_item_action::overridden) {
      continue;
    }
    if (action == trait_item_action::register_as_declared) {
      methods.push_back(method_entry{
          .decl = decl,
          .owner = trait_module,
          .from_trait = trait_decl,
          .trait_name = trait_name,
          .file_id = trait_file_id,
      });
      continue;
    }
    deferred_defaults.push_back(deferred_trait_default{
        .decl = decl,
        .target = target,
        .target_type_name = target_type_name,
        .trait_decl = trait_decl,
        .trait_module = trait_module,
        .trait_file_id = trait_file_id,
        .methods = &methods,
    });
  }
}

auto checker::monomorphize_trait_defaults(
    const std::vector<deferred_trait_default> &deferred_defaults) -> void {
  for (const auto &deferred : deferred_defaults) {
    monomorphize_trait_default(*deferred.decl, deferred.target,
                               deferred.target_type_name, deferred.trait_decl,
                               deferred.trait_module, deferred.trait_file_id,
                               *deferred.methods);
  }
}

/// Adds an `impl` a splice or a `deriving` clause produced to the program.
/// Splices run on the VM once the method table and coherence records for
/// the written `impl`s exist, so the new one is added to both directly.
/// Compile-time code that ran before it was added did not see it.
auto checker::add_synthesized_impl(impl_ref impl) -> void {
  build_method_table();
  auto &members = index_.modules[impl.module_name];
  members.impls.push_back(std::move(impl));
  const auto &added = members.impls.back();
  auto deferred_defaults = std::vector<deferred_trait_default>{};
  register_impl_methods(members, added, deferred_defaults);
  monomorphize_trait_defaults(deferred_defaults);
  record_impl_coherence(added);
}

/// Clones `decl` (an unwritten trait-default method) and type-checks the
/// clone with `function_.self_type_` bound to `target` — the concrete impl
/// target, e.g. `file` — instead of the trait's own abstract `self` type param
/// (`check_trait_decl` checks the trait's own copy exactly once, generic
/// over `self`; that copy's `session_.node_types_` entries can never resolve a
/// concrete call like `self.write(buf)` against a real declaration). On
/// success, registers the clone as an ordinary `method_entry` (so
/// `find_method`/`record_instance_method_callee` treat it exactly like a
/// real impl-provided method — see `infer_method_call`) and records it in
/// `session_.synthesized_trait_defaults_` so `hir::lower_module` lowers it into
/// a real `hir_function`. On failure (a construct `ast::clone_func_decl`
/// doesn't support — see its doc comment), reports a diagnostic rather
/// than silently leaving the method uncallable through this impl.
auto checker::monomorphize_trait_default(
    const ast::func_decl &decl, type_id target,
    std::string_view target_type_name, const ast::trait_decl *trait_decl,
    const module_members *trait_module, file_id_type trait_file_id,
    std::deque<method_entry> &methods) -> void {
  auto cloned = ast::clone_func_decl(decl);
  if (!cloned.has_value()) {
    auto diag = diagnostic(
        diagnostic_level::error,
        std::format("cannot make `{}` callable on `{}`: its default body "
                    "in `{}` uses a construct trait-default monomorphization "
                    "doesn't support yet",
                    decl.name, target_type_name, trait_module->module_name),
        trait_file_id);
    diag.with_label(cloned.error().span, cloned.error().message);
    emit_diag(diag);
    return;
  }

  const auto *raw = cloned->get();
  session_.synthesized_decls_.push_back(std::move(*cloned));

  const auto saved_module = file_.module_;
  const auto saved_file_id = file_.file_id_;
  const auto saved_self = function_.self_type_;
  file_.module_ = trait_module;
  file_.file_id_ = trait_file_id;
  function_.self_type_ = target;
  auto bound_trait_params =
      trait_default_bindings(session_.types_, trait_decl, target);
  const auto pushed_trait_params = !bound_trait_params.empty();
  if (pushed_trait_params) {
    function_.type_params_.push_back(bound_trait_params);
  }
  check_function(*raw, /*at_module_scope=*/false);
  if (pushed_trait_params) {
    pop_type_params();
  }
  file_.module_ = saved_module;
  file_.file_id_ = saved_file_id;
  function_.self_type_ = saved_self;

  methods.push_back(method_entry{
      .decl = raw,
      .owner = trait_module,
      .from_trait = nullptr,
      .trait_name = trait_decl != nullptr ? trait_decl->name : std::string{},
      .file_id = trait_file_id,
      .fixed_type_params = std::move(bound_trait_params),
  });
  session_.synthesized_trait_defaults_.push_back(
      synthesized_method{.decl = raw,
                         .target_type_name = std::string(target_type_name),
                         .owner_module = trait_module->module_name});
}

auto checker::method_lookup() -> method_resolver {
  return method_resolver{session_.types_, cache_.method_tables_,
                         &has_builtin_inherent_method};
}

/// The receiver is `instance`, whose own `type_id` is `id`; `id` is used to
/// pick between impls on different instantiations of one generic
/// declaration, and passing `k_unknown_type` keeps the declaration-level
/// behavior for callers that have only a `type_entry` in hand.
auto checker::resolve_method(const type_entry &instance, type_id id,
                             std::string_view name,
                             const method_filter &accepts)
    -> method_resolution {
  build_method_table();
  if (const auto found = method_lookup().resolve(instance, id, name, accepts);
      found.found()) {
    return found;
  }
  // Nothing provides `name` for this receiver. If the receiver is a
  // concrete instantiation of a generic `deriving` type, this is the point
  // at which that instantiation's methods get derived — lazily, because no
  // earlier phase can know which instantiations exist. Ordering the two
  // this way is what gives a hand-written `impl` priority over a derived
  // one for free: a type that already had the method never reaches the
  // derivation at all.
  if (!ensure_derived_instance_impls(id, name)) {
    return {};
  }
  return method_lookup().resolve(session_.types_.entry(id), id, name, accepts);
}

auto checker::resolve_method(type_id receiver, std::string_view name,
                             const method_filter &accepts)
    -> method_resolution {
  // A copy: deriving can intern types, and the table may move its entries.
  const auto entry = session_.types_.entry(receiver);
  return resolve_method(entry, receiver, name, accepts);
}

auto checker::find_method(const type_entry &instance, std::string_view name,
                          type_id instance_id, const method_filter &accepts)
    -> const method_entry * {
  const auto found = resolve_method(instance, instance_id, name, accepts);
  return found.origin == method_origin::declared ? found.method : nullptr;
}

/// Whether `name` is one of `k_builtin_methods`' entries for `object`.
/// Asked by name rather than through `builtin_method_result`, whose answer
/// for an element-typed method is the element — an open leaf, which
/// `is_unknown` cannot tell from "no such method".
auto checker::has_builtin_inherent_method(const type_entry &object,
                                          std::string_view name) -> bool {
  const auto owner = builtin_method_owner(object);
  return !owner.empty() && std::ranges::any_of(k_builtin_methods,
                                               [&](const auto &method) -> bool {
                                                 return method.owner == owner &&
                                                        method.name == name;
                                               });
}

/// Rigid pattern unification (higher-kinded traits, Strategy #3): matches
/// `pattern` — a declared type mentioning the callee's own type parameters
/// — against the `concrete` type an argument actually has, binding each
/// parameter it meets to the corresponding concrete piece. Rigid means the
/// *shape* must already match: only the outermost nominal constructor is
/// consulted, two different constructors never unify, and nothing is
/// invented — a failed match simply binds nothing (argument checking has
/// its own diagnostics). First binding wins; later occurrences are left to
/// ordinary compatibility checking.
///
/// `allow_override` is for the one caller (`solve_from_bounds`) whose
/// `concrete` is not a heuristic but ground truth: a `where I: iterator[T]`
/// bound names T's one true value (I's actual element type), not merely a
/// type some argument's shape happens to suggest. A prior argument-derived
/// guess for the same name is replaced rather than left to silently win —
/// see the `&int32`-vs-`int32` `max_by`/`min_by` bug this was added for.
/// Solves `pattern`'s type parameters against `concrete`, adding what it
/// learns to `bindings`.
///
/// The structural walk this used to
/// be is gone, and the work is done by the one unifier
/// (`infer/rigid_match.h`). Every call site keeps the same
/// name-keyed-map interface, so this step swaps the algorithm and nothing
/// else — which the phase 0 golden is what verifies.
///
/// Two of the old walk's three defects are already gone with the swap:
/// an array's length now solves, and `&T` no longer matches `&mut T`.
/// Neither changed a single recorded decision, which is what the phase 0
/// golden was for.
///
/// The third — a reference meeting its target — is still switched on, and
/// is not a defect that can simply be turned off: `cli_test`, `std_test`
/// and `ownership_check_test` all depend on it, because the checker relies on
/// this matcher to absorb the auto-borrow at call sites that nothing else
/// performs. Removing it means giving that job to an explicit coercion
/// step, which is its own piece of work.
auto checker::match_params(type_id pattern, type_id concrete,
                           param_subst &bindings, bool allow_override) -> void {
  // An open leaf is an answer, not a gap: `iterator[U]` against the
  // `iterator[?a]` a literal-element list's iterator implements solves
  // `U := ?a`, which settles when the literal does. `is_unknown` counts a
  // leaf as unknown, so it is ruled out by kind here.
  if (pattern == concrete ||
      (session_.types_.is_unknown(concrete) &&
       session_.types_.entry(concrete).kind != type_kind::type_var_kind)) {
    return;
  }
  const auto matched = infer::match_pattern(session_.types_, pattern, concrete);
  // The failure is deliberately dropped: nearly every caller matches
  // speculatively (does this impl's target fit? what does the expected
  // type suggest?) and a miss there is an answer, not an error. The one
  // place a miss is the user's mistake — a generic call's arguments —
  // matches again and reports through blame (`report_argument_conflicts`).
  for (const auto &[param, solved] : matched.bindings) {
    if (allow_override) {
      bindings.insert_or_assign(param, solved);
    } else {
      bindings.try_emplace(param, solved);
    }
  }
}

/// Whether `id` mentions something still abstract — an unknown, a type
/// parameter, a parameter application — anywhere in its structure.
/// `is_unknown` answers the same question shallowly; this recurses, so
/// `option[B]` and `fn(int32) -> option[B]` count as abstract too.
///
/// `leaves_are_answers`: a metavariable counts as concrete. For a caller
/// comparing two candidate answers, `option[?a]` (the `?a` a later
/// statement will say) is an answer and `option[U]` is not.
auto checker::mentions_abstract_type(type_id id, bool leaves_are_answers)
    -> bool {
  if (leaves_are_answers &&
      session_.types_.entry(id).kind == type_kind::type_var_kind) {
    return false;
  }
  if (session_.types_.is_unknown(id)) {
    return true;
  }
  const auto entry =
      session_.types_.entry(id); // copy: callers may intern after
  for (const auto arg : entry.args) {
    if (mentions_abstract_type(arg, leaves_are_answers)) {
      return true;
    }
  }
  // `result` only means "component type" for these kinds; elsewhere it is
  // an unused default (`k_unknown_type`), which must not read as abstract.
  if (entry.kind == type_kind::fn_kind || entry.kind == type_kind::ref_kind ||
      entry.kind == type_kind::ptr_kind ||
      entry.kind == type_kind::array_kind) {
    return mentions_abstract_type(entry.result, leaves_are_answers);
  }
  return false;
}

/// Whether `id` is, or contains, an in-scope generic parameter — `J`,
/// `option[J]`, `fn(J) -> bool`.
///
/// Deliberately separate from `mentions_abstract_type`, which does not
/// look at `type_param_kind` at all and so reads a bare `J` as concrete.
/// Widening that one would change what every one of its callers means by
/// "abstract"; this asks the narrower question its callers actually need:
/// is this type still written in parameters, i.e. are we looking at a
/// template rather than at something instantiable?
/// Whether `id` is written in a template's parameters at all — a type
/// parameter, an application of one, or an open value (`n + 1`) — and so
/// means something different in each instance.
auto checker::mentions_template_param(type_id id) -> bool {
  const auto entry =
      session_.types_.entry(id); // copy: callers may intern after
  if (entry.kind == type_kind::type_param_kind ||
      entry.kind == type_kind::param_app_kind ||
      entry.kind == type_kind::symbolic_value_kind) {
    return true;
  }
  for (const auto arg : entry.args) {
    if (mentions_template_param(arg)) {
      return true;
    }
  }
  if (entry.kind == type_kind::fn_kind || entry.kind == type_kind::ref_kind ||
      entry.kind == type_kind::ptr_kind ||
      entry.kind == type_kind::array_kind ||
      entry.kind == type_kind::refinement_kind) {
    return entry.result != k_unknown_type &&
           mentions_template_param(entry.result);
  }
  return false;
}

auto checker::mentions_type_param(type_id id) -> bool {
  const auto entry =
      session_.types_.entry(id); // copy: callers may intern after
  if (entry.kind == type_kind::type_param_kind ||
      entry.kind == type_kind::param_app_kind) {
    return true;
  }
  for (const auto arg : entry.args) {
    if (mentions_type_param(arg)) {
      return true;
    }
  }
  if (entry.kind == type_kind::fn_kind || entry.kind == type_kind::ref_kind ||
      entry.kind == type_kind::ptr_kind ||
      entry.kind == type_kind::array_kind) {
    return mentions_type_param(entry.result);
  }
  return false;
}

/// Whether `id` has an `unknown` anywhere inside it, not only at the top.
/// `session_.types_.is_unknown` answers for the outermost type alone, which
/// reads a half-resolved `slice[_]` as resolved — it is not, and a caller that
/// stops there hands lowering a type with no element in it.
auto checker::mentions_unknown(type_id id) -> bool {
  const auto entry =
      session_.types_.entry(id); // copy: callers may intern after
  if (session_.types_.is_unknown(id)) {
    return true;
  }
  for (const auto arg : entry.args) {
    if (mentions_unknown(arg)) {
      return true;
    }
  }
  if (entry.kind == type_kind::fn_kind || entry.kind == type_kind::ref_kind ||
      entry.kind == type_kind::ptr_kind ||
      entry.kind == type_kind::array_kind) {
    return mentions_unknown(entry.result);
  }
  return false;
}

/// Rewrites `id` with every solved type parameter replaced by its
/// binding, re-interning bottom-up — so `M[B]` under `{M := option,
/// B := str}` comes back as *the* `option[str]` id, indistinguishable
/// from one written in source (the id-equality invariant substitution
/// must preserve). Unsolved parameters and unrepresentable corners pass
/// through unchanged, degrading to today's loosely-typed behavior rather
/// than erring.
auto checker::substitute_solved(type_id id, const param_subst &bindings)
    -> type_id {
  if (bindings.empty()) {
    return id;
  }
  // Keyed by the parameter's own id, so a parameter is replaced wherever it
  // stands — a bare `T`, or a constructor parameter `F` written unapplied.
  if (const auto it = bindings.find(id); it != bindings.end()) {
    return it->second;
  }
  const auto item = session_.types_.entry(id); // copy: interning below can push
  if (item.kind == type_kind::symbolic_value_kind) {
    // A value parameter lives inside a polynomial, not as a slot of its
    // own, so its binding is applied to each variable it keys.
    auto values = std::unordered_map<std::string, linear_poly>{};
    for (const auto &term : item.value.terms) {
      const auto param = poly_var_identity(term.var);
      const auto it =
          param.has_value() ? bindings.find(*param) : bindings.end();
      if (it == bindings.end()) {
        continue;
      }
      if (const auto poly = slot_poly(session_.types_, it->second)) {
        values.emplace(term.var, *poly);
      }
    }
    return values.empty()
               ? id
               : session_.types_.symbolic_value(
                     item.result, poly_substitute(item.value, values));
  }
  if (item.kind == type_kind::param_app_kind) {
    const auto it = bindings.find(item.result);
    if (it == bindings.end()) {
      return id;
    }
    const auto ctor = session_.types_.entry(it->second);
    if (ctor.kind != type_kind::ctor_ref_kind ||
        ctor.ctor_arity != item.args.size()) {
      return id;
    }
    auto args = std::vector<type_id>{};
    args.reserve(item.args.size());
    for (const auto arg : item.args) {
      args.push_back(substitute_solved(arg, bindings));
    }
    return ctor.decl != nullptr
               ? session_.types_.user_type(*ctor.decl, ctor.module_name,
                                           std::move(args))
               : session_.types_.builtin_generic(ctor.name, std::move(args));
  }

  auto args = std::vector<type_id>{};
  args.reserve(item.args.size());
  auto changed = false;
  for (const auto arg : item.args) {
    const auto rewritten = substitute_solved(arg, bindings);
    changed = changed || rewritten != arg;
    args.push_back(rewritten);
  }
  const auto result = item.result != k_unknown_type
                          ? substitute_solved(item.result, bindings)
                          : item.result;
  changed = changed || result != item.result;
  if (!changed) {
    return id;
  }
  switch (item.kind) {
  case type_kind::builtin_generic_kind:
    return session_.types_.builtin_generic(item.name, std::move(args));
  case type_kind::tuple_kind:
    return session_.types_.tuple_of(std::move(args));
  case type_kind::fn_kind:
    return session_.types_.fn_of(std::move(args), result);
  case type_kind::ref_kind:
    return session_.types_.ref_to(result, item.is_mut);
  case type_kind::ptr_kind:
    return session_.types_.ptr_to(result, item.is_mut);
  case type_kind::array_kind:
    return args.empty() ? session_.types_.array_of(result, item.array_size,
                                                   k_unknown_type)
                        : array_with_length(result, args.front());
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::opaque_kind:
    return item.decl != nullptr
               ? session_.types_.user_type(*item.decl, item.module_name,
                                           std::move(args))
               : id;
  default:
    return id;
  }
}

/// Whether `instance` implements `trait_name`, via either a `deriving`
/// clause or a recorded `impl` (session-wide coherence index).
auto checker::type_has_trait(const type_entry &instance,
                             std::string_view trait_name) -> bool {
  build_method_table();
  if (instance.decl != nullptr) {
    for (const auto &derived : instance.decl->deriving) {
      if (derived == trait_name) {
        return true;
      }
    }
  }
  return impl_trait_index_.contains(
      std::format("{}:{}", trait_name, type_key_of(instance)));
}

/// The concrete trait arguments the `impl` of `trait_name` for `concrete`
/// supplies: asked about `counter` and `iterator`, an
/// `impl iterator[int32] for counter` answers `[int32]`.
///
/// Searched over every module's impls rather than read out of
/// `impl_trait_index_`, which is keyed at *declaration* level — one entry
/// per generic type rather than per instantiation — and records no
/// arguments at all. That is enough for `type_has_trait`'s "does some impl
/// exist", and not enough for "what does that impl say `T` is".
auto checker::trait_args_of_impl_for(type_id concrete,
                                     std::string_view trait_name)
    -> std::optional<std::vector<type_id>> {
  for (const auto &[module_name, members] : index_.modules) {
    for (const auto &impl : members.impls) {
      if (impl.decl->has_error || impl.decl->trait_type == nullptr ||
          impl.decl->trait_type->kind != ast::node_kind::named_type ||
          trait_name_of_impl(*impl.decl) != trait_name) {
        continue;
      }
      const auto target = strip_refs(resolve_impl_target(impl));
      if (session_.types_.is_unknown(target)) {
        continue;
      }
      // Rigid unification alone does not mean the impl applies —
      // `match_params` reports what *would* make the pattern match without
      // insisting that it does. Substituting the solution back and
      // requiring the original settles it, so `impl ... for holder[T]` is
      // accepted for `holder[int32]` and rejected for `boxed[int32]`.
      auto impl_bindings = param_subst{};
      match_params(target, concrete, impl_bindings);
      if (substitute_solved(target, impl_bindings) != concrete) {
        continue;
      }

      auto param_bindings = type_scope{};
      for (const auto &param : impl.decl->type_params) {
        if (!param.name.empty()) {
          param_bindings.emplace(
              param.name, session_.types_.type_param(
                              param.name, param.higher_kinded_arity, &param));
        }
      }
      const auto ctx = resolve_ctx{.module = &members,
                                   .param_bindings = &param_bindings,
                                   .use_type_param_stack = false,
                                   .quiet = true};
      const auto &named =
          ast::cast<const ast::named_type>(*impl.decl->trait_type);
      auto args = std::vector<type_id>{};
      for (const auto &arg : named.type_args) {
        const auto *arg_type = ast::as<const ast::type_expr>(arg.value.get());
        args.push_back(
            arg_type != nullptr
                ? substitute_solved(resolve_type(*arg_type, ctx), impl_bindings)
                : k_unknown_type);
      }
      return args;
    }
  }
  return std::nullopt;
}

/// Solves type parameters that a bound relates to an already-solved one.
///
/// `def map[I, T, U](it: I, f: fn(T) -> U) where I: iterator[T]` is the
/// shape every adapter in `spec/collections-algorithms-design.md` §5.3 is
/// built from, and the arguments alone cannot solve it: `it` answers `I`,
/// the lambda is what `T` is needed *for*, and nothing else mentions `T`.
/// The bound is the missing link — `I := counter` plus
/// `impl iterator[int32] for counter` gives `T := int32`.
///
/// Bounds are otherwise advisory in this compiler (they are not enforced at
/// the call site), and this does not change that: an unsatisfied bound
/// still says nothing here, it merely fails to contribute a binding.
/// Existing bindings are never overwritten, so a bound can turn an
/// unsolvable call into a solved one and never re-answer a call the
/// arguments already settled.
auto checker::solve_from_bounds(const ast::func_decl &decl,
                                const module_members *owner,
                                param_subst &bindings) -> void {
  auto pattern_params =
      generic_param_bindings(decl, /*enclosing_block_params=*/nullptr);
  const auto pattern_ctx = resolve_ctx{.module = owner,
                                       .param_bindings = &pattern_params,
                                       .use_type_param_stack = false,
                                       .quiet = true};

  const auto apply = [&](std::string_view subject_name,
                         const ast::type_expr &bound_expr) -> void {
    const auto subject = pattern_params.find(std::string(subject_name));
    if (subject == pattern_params.end()) {
      return;
    }
    const auto solved = bindings.find(subject->second);
    if (solved == bindings.end()) {
      return;
    }
    // A `+`-joined list contributes from each term independently; a bare
    // `Trait[Args]` is the one-term case of the same thing. Only a term
    // with arguments (`iterator[T]`) has anything to solve.
    auto terms = std::vector<const ast::named_type *>{};
    const auto add_term = [&](const ast::type_expr *term) -> void {
      if (term == nullptr || term->kind != ast::node_kind::named_type) {
        return;
      }
      const auto &named = ast::cast<const ast::named_type>(*term);
      if (!named.path.empty() && !named.type_args.empty()) {
        terms.push_back(&named);
      }
    };
    if (bound_expr.kind == ast::node_kind::bound_type) {
      for (const auto &term :
           ast::cast<const ast::bound_type>(bound_expr).value.terms) {
        add_term(term.type.get());
      }
    } else {
      add_term(&bound_expr);
    }
    if (terms.empty()) {
      return;
    }
    // The impl that answers the bound is chosen by the subject's head, so
    // a head still open (the result of a call waiting on its receiver) is
    // owed its answer now.
    if (solved->second != k_unknown_type) {
      solved->second = demand_shape(solved->second);
    }
    // Only the head has to be known: `list_iter[?a]` answers
    // `iterator[&?a]`, and the leaf travels on into `T`.
    if (session_.types_.is_unknown(solved->second) ||
        mentions_abstract_type(solved->second,
                               /*leaves_are_answers=*/true)) {
      return;
    }

    for (const auto *term : terms) {
      const auto &named = *term;
      auto concrete_args =
          trait_args_of_impl_for(strip_refs(solved->second), named.path.back());
      if (!concrete_args.has_value()) {
        continue;
      }
      for (size_t i = 0;
           i < named.type_args.size() && i < concrete_args->size(); ++i) {
        const auto *arg_type =
            ast::as<const ast::type_expr>(named.type_args[i].value.get());
        if (arg_type == nullptr) {
          continue;
        }
        const auto resolved_arg = resolve_type(*arg_type, pattern_ctx);
        match_params(resolved_arg, (*concrete_args)[i], bindings,
                     /*allow_override=*/true);
      }
    }
  };

  for (const auto &param : decl.type_params) {
    if (!param.is_value_param && param.bound_or_type != nullptr) {
      apply(param.name, *param.bound_or_type);
    }
  }
  for (const auto &constraint : decl.where_constraints) {
    if (constraint.subject == nullptr || constraint.bound_or_type == nullptr ||
        constraint.subject->kind != ast::node_kind::named_type) {
      continue;
    }
    const auto &subject = ast::cast<const ast::named_type>(*constraint.subject);
    if (!subject.path.empty()) {
      apply(subject.path.back(), *constraint.bound_or_type);
    }
  }
}

/// Result types of methods derived through `deriving` or prelude traits.
auto checker::derived_method_result(const type_entry &instance,
                                    std::string_view name)
    -> std::optional<type_id> {
  const auto has = [&](std::string_view trait_name) -> auto {
    return type_has_trait(instance, trait_name);
  };
  if (name == "show" && has("show")) {
    return session_.types_.builtin("str");
  }
  if (name == "debug" && has("debug")) {
    return session_.types_.builtin("str");
  }
  if (name == "hash" && has("hash")) {
    return session_.types_.builtin("uint64");
  }
  if (name == "cmp" && has("ord")) {
    // `ordering` is a real sum type in `std.traits`, not a builtin scalar,
    // so it has to be looked up rather than named. A session without the
    // standard library has no `ordering` at all; `k_unknown_type` is the
    // honest answer there, and unifies with whatever the caller expects
    // rather than inventing a second type with the same spelling.
    if (const auto found =
            find_type_decl_by_path({"std", "traits", "ordering"})) {
      return session_.types_.user_type(*found->first, found->second, {});
    }
    return k_unknown_type;
  }
  if (name == "eq" && has("eq")) {
    return session_.types_.builtin("bool");
  }
  return std::nullopt;
}

// ==========================================================================
//  Member lookup through `deref`
// ==========================================================================

/// Whether `type` itself provides `name` — a field, a method (declared,
/// trait, `extend`, or derived), or for a builtin its own method. Only
/// when it does not is `deref` consulted (spec ch. 18: the receiver's own
/// member wins). A type parameter or an existential is reported as having
/// it: bounds decide what those offer, and `deref` never guesses past
/// them.
auto checker::type_has_member(type_id type, std::string_view name) -> bool {
  const auto &entry = session_.types_.entry(type);
  switch (entry.kind) {
  case type_kind::struct_kind:
    return struct_field_type(entry, name).has_value() ||
           find_method(entry, name, type) != nullptr ||
           derived_method_result(entry, name).has_value();
  case type_kind::sum_kind:
  case type_kind::opaque_kind:
    return find_method(entry, name, type) != nullptr ||
           derived_method_result(entry, name).has_value();
  case type_kind::tuple_kind:
    return tuple_index_of(name).has_value();
  case type_kind::builtin_kind:
  case type_kind::builtin_generic_kind:
    return resolve_method(entry, type, name).found();
  case type_kind::fn_kind:
    return resolve_method(entry, type, name).origin ==
           method_origin::builtin_extend;
  default:
    return true;
  }
}

/// `type`'s `std.traits.deref` method, when `type` is a struct, sum, or
/// opaque type that implements it.
auto checker::deref_method_of(type_id type) -> const method_entry * {
  const auto &entry = session_.types_.entry(type);
  if (entry.kind != type_kind::struct_kind &&
      entry.kind != type_kind::sum_kind &&
      entry.kind != type_kind::opaque_kind) {
    return nullptr;
  }
  return find_method(entry, "deref", type,
                     [](const method_entry &method) -> bool {
                       return method.trait_name == "deref";
                     });
}

/// Resolves one implicit `receiver.deref()` on a receiver of type
/// `handle`, through the same route a written `handle.deref()` takes, so
/// a generic impl (`impl[T] deref for shared[T]`) gets its per-receiver
/// instance. The call is built only to drive that route; what it resolved
/// to is moved into the returned step, and nothing keyed by the probe is
/// left behind for lowering to find.
///
/// Inside a template, a handle that is still written in the template's
/// parameters has no instance to name: only the result type is worked
/// out, and each instance resolves its own step (`defer_deref_steps`).
auto checker::resolve_deref_step(const ast::expr &receiver, type_id handle,
                                 source_span span)
    -> std::optional<deref_step> {
  const auto *method = deref_method_of(handle);
  if (method == nullptr) {
    return std::nullopt;
  }
  const auto &entry = session_.types_.entry(handle);
  const auto callee_type = fn_type_of(*method->decl, method->owner);
  if (function_.current_template_ != nullptr &&
      mentions_template_param(handle)) {
    auto bindings = param_subst{};
    match_params(method->impl_target_pattern, handle, bindings);
    return deref_step{.callee = {},
                      .callee_type = callee_type,
                      .result = substitute_solved(
                          signature_return_type(*method->decl, method->owner,
                                                method->block_type_params),
                          bindings)};
  }
  auto probe = std::make_unique<ast::call_expr>();
  probe->span = span;
  const auto &call = *probe;
  session_.deref_probe_calls_.push_back(std::move(probe));
  auto result = k_unknown_type;
  if (const auto instantiated = check_impl_generic_method_call(
          call, *method, entry, receiver, handle)) {
    result = *instantiated;
  } else {
    record_instance_method_callee(call, *method, entry.name, receiver);
    result = signature_return_type(*method->decl, method->owner);
  }
  session_.call_argument_mappings_.erase(&call);
  const auto found = session_.resolved_callees_.find(&call);
  if (found == session_.resolved_callees_.end() || result == k_error_type) {
    if (found != session_.resolved_callees_.end()) {
      session_.resolved_callees_.erase(found);
    }
    return std::nullopt;
  }
  auto callee = found->second;
  callee.receiver = nullptr;
  session_.resolved_callees_.erase(found);
  return deref_step{.callee = std::move(callee),
                    .callee_type = callee_type,
                    .result = result};
}

/// The `deref` steps that take a receiver of type `object` to one that
/// has `name`, and the type they reach — or `nullopt` when `object` has
/// `name` itself, or no chain of `deref` impls reaches a type that does.
///
/// Never a guess: a receiver whose type still has open leaves is not
/// looked through (spec ch. 18), so a call waiting on its receiver is not
/// resolved against a target the receiver may turn out not to have.
auto checker::deref_chain(const ast::expr &receiver, type_id object,
                          std::string_view name, source_span span)
    -> std::optional<std::pair<std::vector<deref_step>, type_id>> {
  // Deep enough for a handle to a handle; a cycle of `deref` impls stops
  // here rather than looping.
  static constexpr size_t k_max_deref_depth = 8;
  auto current = strip_refs(settle(object));
  auto steps = std::vector<deref_step>{};
  while (steps.size() < k_max_deref_depth) {
    if (mentions_type_var(current) || type_has_member(current, name)) {
      break;
    }
    auto step = resolve_deref_step(receiver, current, span);
    if (!step.has_value()) {
      break;
    }
    current = strip_refs(settle(step->result));
    steps.push_back(std::move(*step));
  }
  if (steps.empty() || mentions_type_var(current) ||
      !type_has_member(current, name)) {
    return std::nullopt;
  }
  return std::pair{std::move(steps), current};
}

/// Stores `steps` for `site`, where lowering and the ownership checker
/// read them.
auto checker::record_deref_steps(const deref_site &site,
                                 std::vector<deref_step> steps) -> void {
  if (site.path != nullptr) {
    auto &segments = session_.path_deref_adjustments_[site.path];
    if (segments.size() < site.path->segments.size()) {
      segments.resize(site.path->segments.size());
    }
    segments[site.segment] = std::move(steps);
    return;
  }
  session_.deref_adjustments_[site.receiver] = std::move(steps);
}

/// The receiver a deref chain at `site` is resolved against: the
/// `field_expr`'s object, or the path itself for a dotted path (whose
/// prefix has no node of its own; the probe only needs one for spans).
auto checker::deref_site_receiver(const deref_site &site) -> const ast::expr & {
  if (site.receiver != nullptr) {
    return *site.receiver;
  }
  return *site.path;
}

/// Looks `name` up through `deref` from a receiver of type `object` at
/// `site`, recording the steps when it is found there. Returns the type
/// the member is found on (stripped of references), or `nullopt` when
/// `object` has `name` itself or `deref` does not reach it.
///
/// In a template the steps are each instance's to resolve: the instance
/// is substituted from the template's records, which do not include
/// these (a step names a concrete `deref` instance), so the decision is
/// replayed against the instance's own receiver type.
auto checker::lookup_through_deref(const deref_site &site, type_id object,
                                   std::string_view name, source_span span)
    -> std::optional<type_id> {
  if (!site.active()) {
    return std::nullopt;
  }
  auto chain = deref_chain(deref_site_receiver(site), object, name, span);
  if (function_.current_template_ != nullptr) {
    defer_deref_steps(site, object, std::string(name), span);
  }
  if (!chain.has_value()) {
    return std::nullopt;
  }
  const auto target = chain->second;
  if (function_.current_template_ == nullptr) {
    record_deref_steps(site, std::move(chain->first));
  }
  return target;
}

/// `lookup_through_deref`'s template half: each instance resolves the
/// steps against its own clone of the site.
auto checker::defer_deref_steps(const deref_site &site, type_id object,
                                std::string name, source_span span) -> void {
  const auto *anchor = site.path != nullptr
                           ? static_cast<const ast::node *>(site.path)
                           : static_cast<const ast::node *>(site.receiver);
  defer_to_instances(
      *anchor,
      [this, site, object, name = std::move(name),
       span](instance_subst &subst) -> void {
        auto clone = deref_site{.receiver = clone_of(subst, site.receiver),
                                .path = clone_of(subst, site.path),
                                .segment = site.segment};
        const auto concrete = substitute_type(object, subst);
        auto chain =
            deref_chain(deref_site_receiver(clone), concrete, name, span);
        if (chain.has_value()) {
          record_deref_steps(clone, std::move(chain->first));
        }
      });
}

/// The type `*operand` reads when `operand`'s type `handle` implements
/// `deref`: the target of the `&target` that `deref()` returns. Records
/// the one implicit `deref()` call as `operand`'s adjustment, which
/// lowering and the ownership checker read the same way they do for a
/// member found through `deref`. `nullopt` when `handle` does not
/// implement `deref` or is not yet known.
auto checker::deref_operator_target(const ast::expr &operand, type_id handle,
                                    source_span span)
    -> std::optional<type_id> {
  handle = settle(handle);
  if (mentions_type_var(handle) || deref_method_of(handle) == nullptr) {
    return std::nullopt;
  }
  auto step = resolve_deref_step(operand, handle, span);
  if (function_.current_template_ != nullptr) {
    defer_to_instances(
        operand,
        [this, operand = &operand, handle,
         span](instance_subst &subst) -> void {
          const auto *clone = clone_of(subst, operand);
          const auto concrete = strip_refs(substitute_type(handle, subst));
          if (auto instance = resolve_deref_step(*clone, concrete, span)) {
            session_.deref_adjustments_[clone] = {std::move(*instance)};
          }
        });
  }
  if (!step.has_value()) {
    return std::nullopt;
  }
  const auto result = settle(step->result);
  if (function_.current_template_ == nullptr) {
    session_.deref_adjustments_[&operand] = {std::move(*step)};
  }
  const auto &result_entry = session_.types_.entry(result);
  return result_entry.kind == type_kind::ref_kind ? result_entry.result
                                                  : result;
}

/// Whether a value whose type has head `entry` is certainly not something
/// `*` reads through: not a reference, pointer, cell, or `deref` handle.
/// Heads still open, and type parameters, are left to later checks.
auto checker::cannot_be_dereferenced(const type_entry &entry) -> bool {
  switch (entry.kind) {
  case type_kind::builtin_kind:
  case type_kind::builtin_generic_kind:
  case type_kind::tuple_kind:
  case type_kind::array_kind:
  case type_kind::fn_kind:
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::opaque_kind:
    return true;
  default:
    return false;
  }
}

/// Reports `*x` where `x` is neither a reference, a raw pointer, a cell,
/// nor a type that implements `deref`.
auto checker::report_deref_of_non_reference(const ast::unary_expr &unary,
                                            type_id type) -> void {
  const auto shown = session_.types_.display(type);
  auto diag =
      diagnostic(diagnostic_level::error,
                 std::format("cannot dereference a value of type `{}`", shown),
                 file_.file_id_);
  diag.with_label(unary.span, std::format("`{}` is not a reference", shown));
  diag.with_note(
      "`*` reads the value a reference (`&T`, `&mut T`), a raw pointer, or "
      "a handle that implements `deref` points to");
  diag.with_help(std::format(
      "A value of type `{}` is not a reference to anything; use it without "
      "`*`. To make a type usable with `*`, implement `deref` for it.",
      shown));
  emit_diag(diag);
  mark_error();
}

/// A note naming the `deref` target that was searched as well, for a
/// member lookup that failed on a handle type. `nullopt` when `type`
/// does not implement `deref`.
auto checker::deref_search_note(type_id type) -> std::optional<std::string> {
  const auto stripped = strip_refs(settle(type));
  if (mentions_type_var(stripped)) {
    return std::nullopt;
  }
  const auto *method = deref_method_of(stripped);
  if (method == nullptr) {
    return std::nullopt;
  }
  auto bindings = param_subst{};
  match_params(method->impl_target_pattern, stripped, bindings);
  const auto target = strip_refs(
      substitute_solved(signature_return_type(*method->decl, method->owner,
                                              method->block_type_params),
                        bindings));
  return std::format("`{}` implements `deref`, so its target `{}` was "
                     "searched as well",
                     session_.types_.display(stripped),
                     session_.types_.display(target));
}

/// Whether the place `target` is reached through an implicit `deref()`
/// (`config.port`, `h.inner.x`), which only lends a shared reference.
/// Returns the receiver's handle type when it is.
auto checker::place_through_deref(const ast::expr &target)
    -> std::optional<type_id> {
  switch (target.kind) {
  case ast::node_kind::field_expr: {
    const auto &field = ast::cast<const ast::field_expr>(target);
    if (field.object == nullptr) {
      return std::nullopt;
    }
    if (session_.deref_adjustments_.contains(field.object.get())) {
      const auto found = session_.node_types_.find(field.object.get());
      return strip_refs(settle(found != session_.node_types_.end()
                                   ? found->second
                                   : k_unknown_type));
    }
    return place_through_deref(*field.object);
  }
  case ast::node_kind::index_expr: {
    const auto &index = ast::cast<const ast::index_expr>(target);
    return index.object != nullptr ? place_through_deref(*index.object)
                                   : std::nullopt;
  }
  case ast::node_kind::group_expr: {
    const auto &group = ast::cast<const ast::group_expr>(target);
    return group.inner != nullptr ? place_through_deref(*group.inner)
                                  : std::nullopt;
  }
  case ast::node_kind::unary_expr: {
    // `*h` on a `deref` handle (`deref_operator_target`).
    const auto &unary = ast::cast<const ast::unary_expr>(target);
    if (unary.op != ast::unary_op::deref || unary.operand == nullptr) {
      return std::nullopt;
    }
    if (session_.deref_adjustments_.contains(unary.operand.get())) {
      const auto found = session_.node_types_.find(unary.operand.get());
      return strip_refs(settle(found != session_.node_types_.end()
                                   ? found->second
                                   : k_unknown_type));
    }
    return place_through_deref(*unary.operand);
  }
  case ast::node_kind::module_path_expr: {
    const auto &path = ast::cast<const ast::module_path_expr>(target);
    const auto found = session_.path_deref_adjustments_.find(&path);
    const auto types = session_.value_path_types_.find(&path);
    if (found == session_.path_deref_adjustments_.end() ||
        types == session_.value_path_types_.end()) {
      return std::nullopt;
    }
    for (size_t i = 0; i < found->second.size(); ++i) {
      if (!found->second[i].empty() && i > 0 && i - 1 < types->second.size()) {
        return strip_refs(settle(types->second[i - 1]));
      }
    }
    return std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

/// Reports a write through `deref` (`config.port = 1`, `&mut config.port`)
/// — `deref` lends only a shared reference, and there is no `deref_mut`.
auto checker::report_write_through_deref(source_span span, type_id handle,
                                         std::string_view what) -> void {
  const auto shown = session_.types_.display(handle);
  auto diag =
      diagnostic(diagnostic_level::error,
                 std::format("cannot {} a value reached through `{}`'s `deref`",
                             what, shown),
                 file_.file_id_);
  diag.with_label(span, "reached through `deref`, which is read-only");
  diag.with_note(std::format(
      "this place is part of the value `{}` lends through `deref()`, which "
      "returns a shared reference",
      shown));
  diag.with_help(
      "There is no `deref_mut`: what a handle like `shared[T]` refers to "
      "may be seen by every other handle, so it is read-only through any "
      "of them. To change a shared value, share a `mutex[T]` and change "
      "it while holding the lock, or build a new value and replace the "
      "handle.");
  emit_diag(diag);
  mark_error();
}

/// Whether `method`, found on the `deref` target of a receiver of type
/// `handle`, may be called through the shared reference `deref()` lends.
/// A `mut self` method would write through it, and `into_iter` would
/// move out of it; both are reported here.
auto checker::method_callable_through_deref(const ast::call_expr &call,
                                            const ast::field_expr &field,
                                            const method_entry &method,
                                            type_id handle) -> bool {
  if (method.decl->params.empty() ||
      param_name_of(method.decl->params.front()) != "self") {
    return true;
  }
  const auto *binding = ast::as<const ast::binding_pattern>(
      method.decl->params.front().pattern.get());
  const auto mutates = binding != nullptr && binding->is_mut;
  const auto consumes = method.trait_name == "into_iterator";
  if (!mutates && !consumes) {
    return true;
  }
  const auto shown = session_.types_.display(handle);
  auto diag = diagnostic(diagnostic_level::error,
                         std::format("cannot call `{}` through `{}`'s `deref`",
                                     field.field_name, shown),
                         file_.file_id_);
  diag.with_label(
      call.span,
      mutates ? std::format("`{}` takes `mut self`", field.field_name)
              : std::format("`{}` consumes its receiver", field.field_name));
  diag.with_note(std::format(
      "`{}` has no method `{}` of its own, so it was looked up on the "
      "target of `deref()`, which returns a shared reference",
      shown, field.field_name));
  diag.with_help(
      mutates ? std::string(
                    "There is no `deref_mut`: what a handle like `shared[T]` "
                    "refers to may be seen by every other handle, so it is "
                    "read-only through any of them. To change a shared value, "
                    "share a `mutex[T]` and change it while holding the lock.")
              : std::string("A value reached through `deref` is only "
                            "borrowed, so it cannot be moved out. Iterate a "
                            "borrow of it instead."));
  emit_diag(diag);
  mark_error();
  return false;
}

/// The type of one element of `T.fields()`: a compile-time descriptor of
/// one struct field, built by `comptime::make_field_descriptor`.
auto checker::field_info_type() -> type_id {
  return session_.types_.builtin("field_info");
}

/// The type of `info.name` on a `field_info`. `type_of` is a type
/// fragment, which has no checked type of its own.
auto checker::field_info_member_type(std::string_view name, source_span span)
    -> type_id {
  if (name == "name" || name == "type_name") {
    return session_.types_.builtin("str");
  }
  if (name == "is_data_member") {
    return session_.types_.builtin("bool");
  }
  if (name == "type_of") {
    return k_unknown_type;
  }
  error_with_help(
      span, std::format("no field `{}` on a `field_info`", name),
      "unknown field",
      "A `field_info` from `T.fields()` has the fields `name`, `type_name`, "
      "`type_of`, and `is_data_member`.");
  return k_error_type;
}

/// The type a `static for` binds its loop variable to: `field_info` over
/// `T.fields()` (a type or a type parameter), else the iterable's element
/// type.
auto checker::static_for_element_type(const ast::expr &iterable) -> type_id {
  const auto type = infer_expr(iterable, k_unknown_type);
  if (const auto element = list_element_type(type); element != k_unknown_type) {
    return element;
  }
  // Without the standard library, `T.fields()` has no `list[field_info]`
  // type, and its loop variable is the builtin descriptor.
  if (iterable.kind == ast::node_kind::call_expr) {
    const auto &call = ast::cast<const ast::call_expr>(iterable);
    const auto *callee = ast::as<const ast::field_expr>(call.callee.get());
    if (callee != nullptr && callee->field_name == "fields" &&
        callee->object != nullptr &&
        callee->object->kind == ast::node_kind::ident_expr) {
      const auto &name = ast::cast<const ast::ident_expr>(*callee->object).name;
      if (lookup_type_param(name).has_value() || find_type_decl_by_name(name)) {
        return field_info_type();
      }
    }
  }
  if (const auto element = list_element_type(type); element != k_unknown_type) {
    return element;
  }
  return element_type_of(type, iterable.span);
}

/// Shared field-access typing for `expr.name`, whether it arrived as a
/// `field_expr` or as a value-rooted dotted path. `site` names the
/// receiver, so a field found only through `deref` can record the steps
/// that reach it; without one, `deref` is not consulted.
auto checker::field_access_type(type_id object, std::string_view name,
                                source_span span, const deref_site &site)
    -> type_id {
  // A field is chosen by what kind of value this is, so a head still open
  // (the result of a call waiting on its receiver) is owed its answer now.
  const auto stripped = strip_refs(demand_shape(object));
  if (const auto target = lookup_through_deref(site, stripped, name, span)) {
    return field_access_type(*target, name, span);
  }
  const auto &entry = session_.types_.entry(stripped);
  if (stripped == field_info_type()) {
    return field_info_member_type(name, span);
  }

  switch (entry.kind) {
  case type_kind::struct_kind: {
    if (const auto field_type = struct_field_type(entry, name)) {
      return *field_type;
    }
    if (const auto *method = find_method(entry, name, stripped)) {
      return fn_type_of(*method->decl, method->owner);
    }
    auto diag = diagnostic(
        diagnostic_level::error,
        std::format("no field `{}` on struct `{}`", name, entry.name),
        file_.file_id_);
    diag.with_label(span, "unknown field");
    if (const auto fields = struct_field_names(entry); !fields.empty()) {
      diag.with_note(std::format("`{}` has the fields {}", entry.name, fields));
    }
    if (const auto searched = deref_search_note(stripped)) {
      diag.with_note(*searched);
    }
    emit_diag(diag);
    mark_error();
    return k_error_type;
  }
  case type_kind::sum_kind: {
    if (const auto *method = find_method(entry, name, stripped)) {
      return fn_type_of(*method->decl, method->owner);
    }
    if (derived_method_result(entry, name)) {
      return k_unknown_type;
    }
    error_with_help(
        span, std::format("no field `{}` on sum type `{}`", name, entry.name),
        "sum types have no fields",
        "Inspect a sum-type value with `match` to reach the data inside "
        "its variants.");
    return k_error_type;
  }
  case type_kind::existential_kind: {
    // Plain (non-call) field/method-value access — `infer_method_call`'s
    // own `existential_kind` case handles the call-syntax form
    // (`x.method(...)`) with the same restriction; this covers
    // `x.method` used as a value and any attempt to reach a field. Both
    // are rejected outright rather than falling through to a silent
    // `k_unknown_type` (the `default` case's builtin-method fallback)
    // that would otherwise cascade into a confusing, undiagnosed lowering
    // failure later — the whole point of existential opacity is that
    // only the bound traits' methods (and only by calling them) are
    // reachable.
    error_with_help(
        span,
        std::format("no field `{}` on the existential type `{}`", name,
                    entry.name),
        "existential types have no accessible fields",
        std::format("`{}` only exposes {} — call one of its methods "
                    "instead.",
                    entry.name, existential_bound_names(entry)));
    return k_error_type;
  }
  case type_kind::tuple_kind: {
    // `t.0` — a tuple's elements are selected by position, and the parser
    // hands the position through as the field name's text (see its
    // `int_lit` case in `parse_postfix_suffix`).
    const auto shown = session_.types_.display(stripped);
    const auto index = tuple_index_of(name);
    if (!index.has_value()) {
      error_with_help(
          span, std::format("no field `{}` on tuple `{}`", name, shown),
          "a tuple's elements have positions, not names",
          std::format("Select an element by position instead, like `.0` "
                      "through `.{}`.",
                      entry.args.empty() ? 0 : entry.args.size() - 1));
      return k_error_type;
    }
    if (*index >= entry.args.size()) {
      error_with_help(
          span,
          std::format("tuple `{}` has no element at position {}", shown,
                      *index),
          std::format("this tuple has {} element(s)", entry.args.size()),
          entry.args.empty()
              ? std::string("This tuple has no elements to select.")
              : std::format("Its positions run from `.0` to `.{}`.",
                            entry.args.size() - 1));
      return k_error_type;
    }
    return entry.args[*index];
  }
  case type_kind::fn_kind: {
    // `helper.x` where `helper` is a function. Used to fall through to
    // the builtin arm, which knows no methods on `fn` types and returned
    // `unknown` without a word, so the mistake type-checked.
    if (const auto found = resolve_method(entry, stripped, name);
        found.origin == method_origin::builtin_extend) {
      return fn_type_of(*found.method->decl, found.method->owner);
    }
    error_with_help(
        span,
        std::format("no field `{}` on a function of type `{}`", name,
                    session_.types_.display(stripped)),
        "a function has no fields",
        std::format("To read `{}` from what the function returns, call it "
                    "first: `f().{}`.",
                    name, name));
    return k_error_type;
  }
  default: {
    const auto builtin_result = builtin_method_result(entry, name);
    if (!has_builtin_inherent_method(entry, name)) {
      if (const auto found = resolve_method(entry, stripped, name);
          found.origin == method_origin::builtin_extend) {
        return fn_type_of(*found.method->decl, found.method->owner);
      }
      if (const auto reported =
              report_unknown_builtin_field(name, span, entry, object)) {
        return *reported;
      }
    }
    return builtin_result;
  }
  }
}

/// Reports plain (non-call) field access `object.name` naming nothing on a
/// *builtin* receiver, once every lookup — builtin inherent method,
/// `extend` — has failed.
///
/// This case used to fall through silently, returning `k_unknown_type`
/// with no diagnostic at all: `s.nonsense` on a `slice[T]`/`option[T]`/
/// `cell[T]` type-checked, and `k_unknown_type` unified with whatever the
/// caller expected, so the mistake only surfaced later at lowering as
/// "no concrete checked type is available for this node ... this is a gap
/// in the compiler, not a mistake in your code" — a confidently wrong
/// diagnostic for a misspelled field. `field_access_type`'s `struct_kind`
/// and `sum_kind` arms have always reported "no field `x` on ..." here;
/// the builtin-generic default arm did not.
///
/// Returns `nullopt` rather than reporting when the receiver's method set
/// is not actually known (see `report_unknown_builtin_method`'s doc
/// comment for why that matters).
auto checker::report_unknown_builtin_field(std::string_view name,
                                           source_span span,
                                           const type_entry &entry,
                                           type_id object)
    -> std::optional<type_id> {
  if (entry.kind != type_kind::builtin_kind &&
      entry.kind != type_kind::builtin_generic_kind) {
    return std::nullopt;
  }

  build_method_table();
  auto candidates = builtin_method_names(entry);
  for (auto &declared : method_lookup().declared_names(entry)) {
    candidates.push_back(std::move(declared));
  }
  std::ranges::sort(candidates);
  candidates.erase(std::ranges::unique(candidates).begin(), candidates.end());

  const auto display = session_.types_.display(object);
  auto diag = diagnostic(
      diagnostic_level::error,
      std::format("no field `{}` on type `{}`", name, display), file_.file_id_);
  diag.with_label(span, "unknown field");
  if (const auto suggestion = best_suggestion(name, candidates)) {
    diag.with_help(std::format("did you mean `{}`?", *suggestion));
  } else if (!candidates.empty()) {
    auto listed = std::string{};
    for (const auto &candidate : candidates) {
      if (!listed.empty()) {
        listed += ", ";
      }
      listed += std::format("`{}`", candidate);
    }
    diag.with_note(
        std::format("`{}` provides the methods {}", display, listed));
    diag.with_help(std::format(
        "`{}` has no field named `{}` — did you mean to call one of its "
        "methods instead?",
        display, name));
  } else {
    diag.with_help(std::format("`{}` has no fields or methods.", display));
  }
  emit_diag(diag);
  mark_error();
  return k_error_type;
}

/// Types a plain (non-call) field access `object.name`.
auto checker::infer_field(const ast::field_expr &field) -> type_id {
  if (field.object == nullptr) {
    return k_unknown_type;
  }
  if (const auto type_constant = infer_type_name_member(field)) {
    return *type_constant;
  }
  const auto object = infer_expr(*field.object, k_unknown_type);
  return field_access_type(object, field.field_name, field.span,
                           deref_site{.receiver = field.object.get()});
}

/// The one rule for what `a.b.c` means — the "Dotted Names" section of
/// `spec/specification/01-core/12-modules-and-imports.md`: field access
/// when `a` is a binding in an enclosing lexical scope (including one a
/// capture list puts out of reach: that is still the local, and deserves
/// the capture diagnostic, not a module lookup) or `self` in a method;
/// a module reference when `a` is the module's own root or `std`; field
/// access on a module-level value when `a` names a `def`/`static` of this
/// module or an imported one; a module reference otherwise. A local always
/// wins, so no library body can be broken by whatever module names a user
/// program declares, and a module-level value can never meet a visible
/// module of the same name (`validate_module_name_conflicts`), so the
/// order of the last two readings only matters for the root and `std`.
///
/// `root_is_local` is whether `a` is a lexical binding where the path is
/// read. The checker passes its own scopes (`dotted_root_is_value`); the
/// compile-time evaluator, which runs `static def` bodies with a frame
/// stack of its own, passes its frames — the rest of the rule is this
/// function either way.
auto checker::classify_dotted_root(std::string_view root, bool root_is_local)
    -> dotted_root {
  if (root_is_local) {
    return dotted_root::local_value;
  }
  if (root_visible_without_import(visibility_module(), root)) {
    return dotted_root::module;
  }
  if (find_module_scope_value(root).has_value()) {
    return dotted_root::module_value;
  }
  return dotted_root::module;
}

/// `classify_dotted_root` against the checker's own scopes. Every site
/// that has to tell the two readings apart asks this, and
/// `infer_value_rooted_path` records the answer for lowering, so no later
/// pass decides again.
auto checker::dotted_root_is_value(std::string_view root) -> bool {
  const auto is_local =
      find_value_in_scopes(root).binding != nullptr ||
      (root == "self" && function_.self_type_ != k_unknown_type);
  return classify_dotted_root(root, is_local) != dotted_root::module;
}

auto checker::infer_module_path(const ast::module_path_expr &path) -> type_id {
  if (path.segments.empty()) {
    return k_unknown_type;
  }
  const auto &root = path.segments.front();
  if (dotted_root_is_value(root)) {
    return infer_value_rooted_path(path);
  }
  if (path.segments.size() == 2 && is_builtin_scalar_name(root)) {
    const auto type = session_.types_.builtin(root);
    const auto &member = path.segments.back();
    if ((member == "max" || member == "min") &&
        session_.types_.is_numeric(type)) {
      return type;
    }
    return k_unknown_type;
  }
  if (!validate_module_path(path)) {
    return k_error_type;
  }

  // A module-qualified `static` binding (`main.inner.limit`, or
  // `r.limit` after `use p.r`). Resolving it here is what gives the
  // reference a concrete type *and* a literal for lowering to embed —
  // without both, the path typed as unknown and the program died in
  // codegen with "only a two-segment `value.field` path is lowered".
  if (const auto *owner = find_static_owner_of_path(path.segments)) {
    if (const auto it = owner->statics.find(path.segments.back());
        it != owner->statics.end()) {
      const auto type = static_binding_type(*it->second.decl, owner);
      record_static_const_reference(path, *it->second.decl, type,
                                    owner->module_name);
      return type;
    }
  }
  // A module-qualified function named as a plain value rather than called
  // (`app.geometry.tests.test_area` passed to `case(...)`). The bare-name
  // spelling records this in `resolve_ident`; recording it here is what
  // lets `hir::lower_module_path` build the same owner-carrying reference
  // instead of failing as an unsupported long path.
  if (const auto *owner = find_fn_owner_of_path(path.segments)) {
    if (const auto it = owner->functions.find(path.segments.back());
        it != owner->functions.end()) {
      record_fn_value_reference(path, *it->second.decl, owner->module_name);
      return fn_type_of(*it->second.decl, owner);
    }
  }
  return infer_module_static_projection(path);
}

auto checker::find_module_static_prefix(
    const std::vector<std::string> &segments) const
    -> std::optional<module_static_prefix> {
  for (auto end = segments.size(); end >= 2; --end) {
    const auto prefix = std::vector<std::string>(
        segments.begin(), segments.begin() + static_cast<std::ptrdiff_t>(end));
    const auto *owner = find_static_owner_of_path(prefix);
    if (owner == nullptr) {
      continue;
    }
    if (const auto it = owner->statics.find(prefix.back());
        it != owner->statics.end()) {
      return module_static_prefix{
          .decl = it->second.decl, .owner = owner, .first_field = end};
    }
  }
  return std::nullopt;
}

/// `geo.origin.y`: a module path to a `static`, then field access on it
/// (spec "Dotted Names", rule 3).
auto checker::infer_module_static_projection(const ast::module_path_expr &path)
    -> type_id {
  const auto found = find_module_static_prefix(path.segments);
  if (!found.has_value() || found->first_field == path.segments.size()) {
    return k_unknown_type;
  }
  auto type = static_binding_type(*found->decl, found->owner);
  for (auto i = found->first_field; i < path.segments.size(); ++i) {
    type = field_access_type(type, path.segments[i], path.span);
  }
  fold_static_field_path(path, *found->decl, found->owner, found->first_field,
                         type);
  return type;
}

/// The module and file declaring `fn`, when it is a module-level
/// function or a method of an `impl`/`extend` block; `nullopt` for
/// anything else (a synthesized declaration), which then runs in its
/// caller's context.
auto checker::function_owner(const ast::func_decl &fn)
    -> std::optional<std::pair<const module_members *, file_id_type>> {
  // Rescanned when the index has grown: an instantiated functor adds a
  // module mid-session.
  if (cache_.function_owners_modules_ != index_.modules.size()) {
    cache_.function_owners_modules_ = index_.modules.size();
    for (const auto &[module_name, members] : index_.modules) {
      for (const auto &[fn_name, ref] : members.functions) {
        cache_.function_owners_.emplace(ref.decl,
                                        std::pair{&members, ref.file_id});
      }
      // Methods too: a compile-time method call (`s.trim()`) runs its body
      // where it was written, so `std.string`'s own `rt_str_*` resolve.
      const auto add_methods = [&](const auto &items, file_id_type file) {
        for (const auto &item : items) {
          if (const auto *method = ast::as<const ast::func_decl>(item.get())) {
            cache_.function_owners_.emplace(method, std::pair{&members, file});
          }
        }
      };
      for (const auto &ref : members.impls) {
        add_methods(ref.decl->items, ref.file_id);
      }
      for (const auto &ref : members.extends) {
        add_methods(ref.decl->items, ref.file_id);
      }
    }
  }
  const auto it = cache_.function_owners_.find(&fn);
  if (it == cache_.function_owners_.end()) {
    return std::nullopt;
  }
  return it->second;
}

/// A module-level `static`'s compile-time value for the evaluator,
/// evaluated per declaration in its declaring module
/// (`ensure_static_binding_evaluated`). The error is the diagnostic to
/// report, or empty when the cause was already reported; `cycle` is the
/// message for a read of `decl` while its own initializer is running.
auto checker::comptime_static_value(const ast::static_decl &decl,
                                    const module_members *owner,
                                    std::string cycle)
    -> std::expected<comptime::value, std::string> {
  if (cache_.static_bindings_evaluating_.contains(&decl)) {
    return std::unexpected(std::move(cycle));
  }
  if (const auto *value = ensure_static_binding_evaluated(decl, owner)) {
    return *value;
  }
  return std::unexpected(std::string{});
}

/// The compile-time evaluator's method lookup
/// (`comptime::evaluator::method_resolver_fn`): the method ordinary code
/// would call on `receiver_expr`. A compile-time value does not carry the
/// exact type it was checked at (an integer is just an integer), so the
/// type comes from the receiver expression as the checker typed it. A
/// `str` value needs no expression type: it has only one type.
auto checker::find_comptime_method(const ast::expr &receiver_expr,
                                   const comptime::value &receiver,
                                   const std::string &name)
    -> std::expected<const ast::func_decl *, std::string> {
  auto receiver_type = k_unknown_type;
  if (receiver.kind == comptime::value_kind::string) {
    receiver_type = session_.types_.builtin("str");
  } else if (const auto it = session_.node_types_.find(&receiver_expr);
             it != session_.node_types_.end()) {
    receiver_type = strip_refs(settle(it->second));
  }
  receiver_type = substitute_comptime_type_params(receiver_type);
  if (mentions_template_param(receiver_type)) {
    return std::unexpected(std::format(
        "the receiver has the generic type `{}`, and compile-time code "
        "cannot yet tell which type it stands for",
        session_.types_.display(receiver_type)));
  }
  if (session_.types_.is_unknown(receiver_type) ||
      receiver_type == k_error_type) {
    return std::unexpected(std::string{
        "the receiver's type is not known where this compile-time code was "
        "checked"});
  }
  const auto *method = resolve_method(receiver_type, name).method;
  if (method == nullptr) {
    return std::unexpected(std::format("`{}` has no method of that name",
                                       session_.types_.display(receiver_type)));
  }
  return method->decl;
}

/// The compile-time evaluator's generic-call lookup
/// (`comptime::evaluator::generic_call_resolver_fn`): the type arguments
/// the checker solved for `call`. A call written inside a generic body
/// solves to that body's own parameters (`inner(x)` with `x: T`), which
/// are read through the types the running call bound them to.
auto checker::comptime_call_type_args(const ast::call_expr &call)
    -> std::optional<std::vector<std::pair<std::string, comptime::value>>> {
  const auto it = session_.comptime_call_type_slots_.find(&call);
  if (it == session_.comptime_call_type_slots_.end()) {
    return std::nullopt;
  }
  auto values = std::vector<std::pair<std::string, comptime::value>>{};
  for (const auto &[name, solved] : it->second) {
    const auto type = substitute_comptime_type_params(settle(solved));
    const auto &entry = session_.types_.entry(type);
    values.emplace_back(
        name, comptime::value::make_type_value(entry.name, entry.decl, type));
  }
  return values;
}

/// `type` with each type parameter the running compile-time call has
/// bound (`comptime::evaluator::type_binding`) replaced by its binding.
/// Compile-time code runs a generic `static def`'s template body, whose
/// expressions the checker typed with the parameters themselves.
auto checker::substitute_comptime_type_params(type_id type) -> type_id {
  auto bindings = param_subst{};
  const auto collect = [&](this const auto &self, type_id id) -> void {
    const auto entry =
        session_.types_.entry(id); // copy: callers may intern after
    if (entry.kind == type_kind::type_param_kind) {
      const auto *bound = comptime_eval_.type_binding(entry.name);
      if (bound != nullptr && bound->checked_type != k_unknown_type &&
          bound->checked_type != id) {
        bindings.emplace(id, bound->checked_type);
      }
      return;
    }
    for (const auto arg : entry.args) {
      self(arg);
    }
    if (entry.result != k_unknown_type &&
        (entry.kind == type_kind::fn_kind ||
         entry.kind == type_kind::ref_kind ||
         entry.kind == type_kind::ptr_kind ||
         entry.kind == type_kind::array_kind)) {
      self(entry.result);
    }
  };
  collect(type);
  return substitute_solved(type, bindings);
}

/// The compile-time evaluator's view of a bare name that is none of its
/// own frame locals (`comptime::evaluator::name_resolver_fn`): the same
/// module-scope lookup `resolve_ident` does for ordinary code — this
/// module's `def`s and `static`s, then imported ones — in whatever module
/// the evaluator is running in (`in_module_context`).
auto checker::read_bare_name_for_comptime(const std::string &name)
    -> comptime::evaluator::bare_name_reading {
  using reading = comptime::evaluator::bare_name_reading;
  const auto found = find_module_scope_value(name);
  if (!found.has_value()) {
    // A prelude function (`binary_search`, `partition_point`, ...) resolves
    // unqualified in ordinary calls, so it does here too.
    if (const auto prelude = find_prelude_function(name)) {
      return reading{.reading = reading::kind::function,
                     .base = {},
                     .function = prelude->first.decl,
                     .reason = {}};
    }
    return reading{};
  }
  if (found->function != nullptr) {
    return reading{.reading = reading::kind::function,
                   .base = {},
                   .function = found->function,
                   .reason = {}};
  }
  auto value = comptime_static_value(
      *found->static_binding, found->owner,
      std::format("`{}` is part of a compile-time evaluation cycle (it "
                  "references itself, directly or indirectly, while being "
                  "evaluated)",
                  name));
  if (!value.has_value()) {
    return reading{.reading = reading::kind::not_constant,
                   .base = {},
                   .function = nullptr,
                   .reason = std::move(value.error())};
  }
  return reading{.reading = reading::kind::static_value,
                 .base = std::move(*value),
                 .function = nullptr,
                 .reason = {}};
}

/// The compile-time evaluator's view of a dotted path
/// (`comptime::evaluator::path_resolver_fn`): the same
/// `classify_dotted_root` ordinary code gets, with the evaluator's own
/// frames standing in for lexical scope, and module-level `static`s
/// evaluated through `comptime_static_value` rather than looked up by
/// bare name.
auto checker::read_dotted_path_for_comptime(const ast::module_path_expr &path,
                                            bool root_is_local)
    -> comptime::evaluator::dotted_path_reading {
  using reading = comptime::evaluator::dotted_path_reading;
  const auto &root = path.segments.front();
  const auto text = join_strings(path.segments, ".");
  const auto static_value = [&](const ast::static_decl &decl,
                                const module_members *owner,
                                size_t first_field) -> reading {
    auto value = comptime_static_value(
        decl, owner,
        std::format("`{}` is part of a compile-time evaluation cycle: "
                    "`{}` reads it while it is being evaluated",
                    decl.name, text));
    if (!value.has_value()) {
      return reading{.reading = reading::kind::not_constant,
                     .base = {},
                     .first_field = 0,
                     .reason = std::move(value.error())};
    }
    return reading{.reading = reading::kind::static_value,
                   .base = std::move(*value),
                   .first_field = first_field,
                   .reason = {}};
  };
  switch (classify_dotted_root(root, root_is_local)) {
  case dotted_root::local_value:
    return reading{};
  case dotted_root::module_value: {
    const auto value = find_module_scope_value(root);
    if (value->static_binding != nullptr) {
      return static_value(*value->static_binding, value->owner, 1);
    }
    return reading{.reading = reading::kind::not_constant,
                   .base = {},
                   .first_field = 0,
                   .reason = std::format("`{}` is a function, which has no "
                                         "fields to read in `{}`",
                                         root, text)};
  }
  case dotted_root::module:
    break;
  }
  if (const auto *owner = find_fn_owner_of_path(path.segments)) {
    return reading{.reading = reading::kind::function,
                   .base = {},
                   .first_field = 0,
                   .reason = {},
                   .function = owner->functions.at(path.segments.back()).decl};
  }
  if (const auto found = find_module_static_prefix(path.segments)) {
    return static_value(*found->decl, found->owner, found->first_field);
  }
  return reading{.reading = reading::kind::not_constant,
                 .base = {},
                 .first_field = 0,
                 .reason = std::format("`{}` is not a compile-time constant: "
                                       "`{}` is not a binding here, so the "
                                       "path is read as a module path, and "
                                       "it names no `static` binding",
                                       text, root)};
}

/// `a.b.c` where `a` is a binding in scope, or a module-level `def`/
/// `static`: a chain of field accesses. Records each prefix's type in
/// `session_.value_path_types_` — lowering builds the projection chain from
/// exactly these, whatever the chain's length.
auto checker::infer_value_rooted_path(const ast::module_path_expr &path)
    -> type_id {
  const auto &root = path.segments.front();
  auto base = function_.self_type_;
  auto module_value = std::optional<module_value_ref>{};
  if (const auto *binding = lookup_value(root)) {
    if (binding->generic_lambda != nullptr) {
      // No member of a closure could say which copy is meant.
      return generic_lambda_value(
          {.node = &path, .name = root, .span = path.span}, *binding,
          k_unknown_type);
    }
    base = binding->type;
  } else if (const auto *barrier = capture_barrier_blocking(root)) {
    emit_capture_not_listed(path.span, root, *barrier);
    return k_error_type;
  } else if (root != "self" || function_.self_type_ == k_unknown_type) {
    module_value = find_module_scope_value(root);
    if (!module_value.has_value()) {
      return k_unknown_type;
    }
    base = module_value->function != nullptr
               ? fn_type_of(*module_value->function, module_value->owner)
               : static_binding_type(*module_value->static_binding,
                                     module_value->owner);
  }
  auto segment_types = std::vector<type_id>{base};
  for (size_t i = 1; i < path.segments.size(); ++i) {
    base = field_access_type(base, path.segments[i], path.span,
                             deref_site{.path = &path, .segment = i});
    segment_types.push_back(base);
  }
  session_.value_path_types_.insert_or_assign(&path, std::move(segment_types));
  if (module_value.has_value() && module_value->static_binding != nullptr) {
    fold_static_field_path(path, *module_value->static_binding,
                           module_value->owner, 1, base);
  }
  return base;
}

/// `origin.x` (or `geo.origin.x`) where `origin` is a `static`: its value
/// is known at compile time, so a scalar field is embedded as a literal,
/// exactly as a bare scalar `static` is (`record_static_const_reference`).
/// `first_field` is the index of the first field segment. A projection
/// that doesn't end in a scalar is left to lowering, which reports it.
auto checker::fold_static_field_path(const ast::module_path_expr &path,
                                     const ast::static_decl &decl,
                                     const module_members *owner,
                                     size_t first_field, type_id type) -> void {
  const auto *value = ensure_static_binding_evaluated(decl, owner);
  for (size_t i = first_field; value != nullptr && i < path.segments.size();
       ++i) {
    if (value->kind != comptime::value_kind::struct_instance) {
      return;
    }
    const auto it = value->fields.find(path.segments[i]);
    value = it != value->fields.end() ? &it->second : nullptr;
  }
  if (value == nullptr) {
    return;
  }
  if (const auto *lit = materialize_const_literal(*value, path.span, type)) {
    session_.static_const_values_[&path] = lit;
  }
}

/// The module whose "Visible Modules" set applies to the code being
/// checked — see `session_.functor_owner_module_`.
auto checker::visibility_module() const -> const std::string & {
  return session_.functor_owner_module_.empty()
             ? file_.module_name_
             : session_.functor_owner_module_;
}

/// Validates a module-rooted dotted path against the module graph, once
/// per node. Returns false after reporting it.
auto checker::validate_module_path(const ast::module_path_expr &path) -> bool {
  if (session_.session_index_ == nullptr ||
      session_.semantic_index_ == nullptr ||
      !session_.validated_module_paths_.insert(&path).second) {
    return true;
  }
  const auto aliases = session_.file_module_aliases_.find(file_.file_id_);
  return validate_module_reference(
      path, visibility_module(), file_.file_id_, *session_.session_index_,
      *session_.semantic_index_,
      aliases != session_.file_module_aliases_.end() ? &aliases->second
                                                     : nullptr,
      diag_, file_has_errors_);
}

/// Whether an identifier in callee/index position names a function or type
/// rather than a runtime value (so `name[T]` is generic instantiation).
auto checker::ident_names_callable_decl(const ast::ident_expr &ident) -> bool {
  if (lookup_value(ident.name) != nullptr) {
    return false;
  }
  if (file_.module_ != nullptr &&
      (file_.module_->functions.contains(ident.name) ||
       file_.module_->types.contains(ident.name) ||
       file_.module_->traits.contains(ident.name) ||
       file_.module_->concepts.contains(ident.name))) {
    return true;
  }
  return is_prelude_value_name(ident.name) ||
         is_builtin_scalar_name(ident.name) ||
         builtin_generic_arity(ident.name).has_value() ||
         find_import(ident.name) != nullptr;
}

/// Tries to prove `i < len(v)` for an indexing expression, and records the
/// answer. This is the payoff the whole feature is for: an index the
/// compiler can *prove* in bounds needs no runtime bounds check, so a
/// `safe_get(v: array[T, n], i: index[n])` compiles to a bare load.
///
/// A *refuted* index (`v[9]` into an `array[T, 4]`) is an error — the code
/// cannot run correctly. An index that is merely unproven is silent: the
/// bounds check stays, which is the status quo and perfectly safe. Only the
/// deliberate act of writing a dependent signature buys the elision, and
/// failing to buy it costs nothing.
auto checker::check_index_in_bounds(const ast::index_expr &index,
                                    type_id object, type_id key) -> void {
  if (index.index == nullptr || index.object == nullptr ||
      !session_.types_.is_integer(key)) {
    return;
  }
  const auto &entry = session_.types_.entry(object);
  if (entry.kind != type_kind::array_kind || entry.args.empty()) {
    return;
  }
  const auto length = slot_poly(session_.types_, entry.args.front());
  const auto position = solver_poly(*index.index, predicate_subst{});
  if (!length.has_value() || !position.has_value()) {
    return;
  }

  // `i < len(v)`, as `len(v) - i - 1 >= 0`.
  auto poly = poly_sub(poly_sub(*length, *position), poly_constant(1));
  const auto subst = predicate_subst{};
  const auto goal = goal_form{fact_set{constraint{
      .poly = std::move(poly),
      .rel = relation::ge,
      .label = std::format("{} < {}", describe(*index.index, subst),
                           length->display()),
  }}};

  switch (solve(function_.facts_, goal)) {
  case proof_result::proved:
    session_.proven_in_bounds_.insert(&index);
    return;
  case proof_result::refuted: {
    auto diag =
        diagnostic(diagnostic_level::error,
                   std::format("index out of bounds: `{}` is never true",
                               goal_label(goal, "the index bound")),
                   file_.file_id_);
    diag.with_label(
        index.index->span,
        std::format("indexing a `{}`", session_.types_.display(object)));
    if (const auto known = known_facts_note(goal)) {
      diag.children.emplace_back(diagnostic_level::note, *known,
                                 file_.file_id_);
    }
    diag.with_help(
        "This access is out of range on every execution that reaches it — "
        "it is not a check the compiler failed to prove, but one it "
        "disproved.");
    emit_diag(diag);
    mark_error();
    return;
  }
  case proof_result::unknown:
    // The bounds check stays. Nothing to say: not every index needs to be
    // proved, and nagging about the ones that aren't would make the feature
    // a tax on code that never asked for it.
    return;
  }
}

/// Finds a trait by name reachable from the current module (own
/// declarations or imports), then locates which module actually owns it
/// (needed because `find_session_trait` doesn't track the owning module
/// for an imported trait).
auto checker::find_trait_anywhere(std::string_view name) -> std::optional<
    std::pair<const ast::trait_decl *, const module_members *>> {
  if (const auto found = find_session_trait(name)) {
    const auto *owner = file_.module_;
    // The trait may live in another module of the session.
    for (const auto &[module_name, members] : index_.modules) {
      if (const auto it = members.traits.find(std::string(name));
          it != members.traits.end() && it->second.decl == found->decl) {
        owner = &members;
        break;
      }
    }
    return std::pair{found->decl, owner};
  }
  return std::nullopt;
}

/// Whether `trait` declares a method item named `name` — used to check
/// whether a method call on an existential-typed receiver is within its
/// bound (`infer_method_call`'s `existential_kind` case). Supertrait
/// requirements aren't walked; a method only reachable through a
/// supertrait isn't yet considered part of the bound.
auto checker::trait_declares_method(const ast::trait_decl &trait,
                                    std::string_view name) -> bool {
  for (const auto &item : trait.items) {
    if (item != nullptr && item->kind == ast::node_kind::func_decl &&
        ast::cast<const ast::func_decl>(*item).name == name) {
      return true;
    }
  }
  return false;
}

} // namespace cinder::semantic::detail
