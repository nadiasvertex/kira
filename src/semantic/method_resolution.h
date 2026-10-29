#pragma once

#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "src/parser/ast.h"
#include "src/semantic/program_index.h"
#include "src/semantic/type_table.h"

namespace cinder::semantic::detail {

/// What each spelling of a type parameter refers to while resolving source
/// written in those spellings — a signature, a body, a bound. A scope, not a
/// solution: it answers a question about names, which is why it is keyed by
/// them. A solution is a `param_subst`; `checker::bindings_by_name` turns one
/// into the other where an instance body is about to be resolved under it.
using type_scope = std::unordered_map<std::string, type_id>;

struct method_entry {
  const ast::func_decl *decl = nullptr; ///< The method's declaration.
  const module_members *owner =
      nullptr; ///< Module the method should resolve types in.
  const ast::trait_decl *from_trait =
      nullptr; ///< Owning trait, if this is a default method.
  bool is_extension =
      false; ///< Whether this method came from an `extend` block.
  /// The trait this method implements, if any — set for both a trait's
  /// default body (redundant with `from_trait->name`) and an impl's own
  /// override of a required/provided trait method; empty for an `extend`
  /// block or any other inherent method. Lets a caller identify a specific,
  /// known-consuming trait method (`into_iterator::into_iter`) without a
  /// general by-value-`self` convention to key off — see `receiver_mode`
  /// in `ownership_cfg.cpp`.
  std::string trait_name;
  /// File the method was written in. Every per-call instance of this method
  /// is re-checked, and name resolution reads imports out of the file being
  /// checked — so an instance checked under the *caller's* file cannot see
  /// anything the method's own module imported. That made a generic method
  /// unable to call an imported function at all (`undefined name`, reported
  /// against a line in the caller's file), which is why a generic stdlib
  /// module could not depend on another one.
  file_id_type file_id = 0;
  /// Bindings that are fixed for this method independent of any call — a
  /// higher-kinded trait default monomorphized for one impl carries its
  /// trait's constructor parameter here (`F := option`), so re-checking a
  /// per-call instance of it resolves `F[A]` the same way its own check did.
  type_scope fixed_type_params;
  /// Type parameters of the `impl` or `extend` block this method was declared
  /// in, or null for a trait default. Needed because a block's *own* generic
  /// parameters (`impl[T] get_it[T] for holder[T]`, `extend[T] holder[T]`)
  /// scope the method's signature and body without appearing on the method's
  /// `type_params` — so the method looks non-generic while being anything but.
  /// A non-null pointer to an *empty* vector is meaningful: it marks a method
  /// of a non-generic block over a possibly-generic target, which still needs
  /// per-receiver instances (see `impl_needs_instance`).
  const std::vector<ast::type_param> *block_type_params = nullptr;
  /// The block's target as a *pattern*, with the block's own type parameters
  /// left as `type_param` types (`holder[T]`). Rigid-unifying this against the
  /// concrete receiver is what solves those parameters at a call site; see
  /// `impl_generic_bindings`.
  type_id impl_target_pattern = k_unknown_type;
};

/// An extra condition on a method lookup, past the method's name — see
/// `method_resolver::resolve`. Empty means "any method with that name".
using method_filter = std::function<bool(const method_entry &)>;

// ==========================================================================
//  Method resolution — what `receiver.name` refers to
//
//  Every method a program declares is filed in one of three tables, by what
//  the impl or `extend` block targets. `method_resolver` answers one
//  question over them: given a receiver type and a method name, which
//  method is meant, and which kind of method is it. It reads the tables and
//  the type table and nothing else.
//
//  Two things a lookup can need are not here, because they change checker
//  state: building the tables (`checker::build_method_table`) and deriving
//  the methods of a generic `deriving` type for one instantiation
//  (`checker::ensure_derived_instance_impls`). The checker runs the first
//  before it asks, and runs the second and asks again when the answer is
//  `none` — see `checker::resolve_method`.
// ==========================================================================

/// The tables a lookup reads. Methods are kept in deques, and the deques
/// have to stay deques: a generic `deriving` type derives lazily, per
/// concrete instantiation, so a lookup can append to a bucket while a caller
/// still holds the `const method_entry *` an earlier lookup returned. A
/// vector would reallocate and leave that pointer dangling; a deque never
/// invalidates references to existing elements on `push_back`. Same
/// reasoning, and the same bug class, as `type_table::entries_`.
struct method_tables {
  /// Impl, trait-default, and `extend` methods of a user type, keyed by its
  /// declaration.
  std::unordered_map<const ast::type_decl *, std::deque<method_entry>> declared;
  /// `extend`-block methods on a builtin type (`str`), keyed by the
  /// builtin's type-entry name since builtins have no `type_decl`.
  std::unordered_map<std::string, std::deque<method_entry>> extend_by_builtin;
  /// Impl-block methods whose target is a *prelude constructor* (`impl monad
  /// for option`), keyed by the constructor's name, for the same reason.
  /// Consulted for a call on any instantiation of the constructor
  /// (`option[int32].bind(...)`) and for type-qualified associated calls
  /// (`option.pure(...)`).
  std::unordered_map<std::string, std::deque<method_entry>> impl_by_builtin;
};

/// Where a resolved method came from. The origin is what a caller's
/// instantiation ladder dispatches on: a declared method may belong to a
/// generic impl, a builtin's inherent method has no declaration at all.
enum class method_origin : uint8_t {
  none,             ///< No method of this name reaches the receiver.
  declared,         ///< An impl, trait default, or `extend` on a user type.
  builtin_inherent, ///< Hard-coded in the compiler (`k_builtin_methods`).
  builtin_impl,     ///< An impl on a prelude constructor (`option`).
  builtin_extend,   ///< An `extend` block on a builtin (`str`).
};

struct method_resolution {
  method_origin origin = method_origin::none;
  /// The declaration found. Null for `none`. For `builtin_inherent` it is
  /// the impl or `extend` method of the same name when there is one, and
  /// otherwise null: the compiler's own methods have no declaration.
  const method_entry *method = nullptr;

  [[nodiscard]] auto found() const -> bool {
    return origin != method_origin::none;
  }
};

class method_resolver {
public:
  /// Whether the compiler itself defines `name` on this builtin receiver.
  /// A plain function: the table it reads belongs to the checker.
  using builtin_probe = bool (*)(const type_entry &, std::string_view);

  method_resolver(const type_table &types, const method_tables &tables,
                  builtin_probe has_inherent)
      : types_(&types), tables_(&tables), has_inherent_(has_inherent) {}

  /// Resolves `name` on `receiver`.
  ///
  /// A receiver with a declaration is searched in `tables.declared`.
  /// Inherent and impl-provided methods take priority over trait defaults,
  /// which take priority over `extend`-block methods; among the first, an
  /// impl whose target names this exact instantiation beats one that covers
  /// it generically. `receiver` being unknown keeps the declaration-level
  /// behavior, for callers that have only a `type_entry`'s declaration.
  ///
  /// A receiver without one (a builtin) answers, in order: the compiler's
  /// own methods, an impl on its constructor, an `extend` block. A name the
  /// compiler defines is reported as `builtin_inherent`, since that is what
  /// a call dispatches to.
  ///
  /// `accepts`, when set, narrows the search past the name: a type may
  /// carry several impls of the same parameterized trait (`index[usize]`
  /// and `index[range[usize]]`), each contributing a method of the same
  /// name, and only the caller knows which one this use site means. It
  /// applies to declared methods only.
  [[nodiscard]] auto resolve(type_id receiver, std::string_view name,
                             const method_filter &accepts = {}) const
      -> method_resolution;

  /// Like `resolve`, for a caller that has only a `type_entry` and, perhaps,
  /// the instantiation it came from (`k_unknown_type` when it does not).
  [[nodiscard]] auto resolve(const type_entry &entry, type_id instance,
                             std::string_view name,
                             const method_filter &accepts = {}) const
      -> method_resolution;

  /// The impl method `name` of a prelude constructor, looked up by the
  /// constructor's spelling alone (`option.pure(...)`), where no
  /// `type_entry` exists to resolve against.
  [[nodiscard]] auto constructor_method(std::string_view constructor,
                                        std::string_view name) const
      -> const method_entry *;

  /// Every method name `entry` declares through an impl or `extend`, for the
  /// "did you mean" suggestion on an unknown method. Unsorted, with repeats.
  [[nodiscard]] auto declared_names(const type_entry &entry) const
      -> std::vector<std::string>;

  /// Whether `pattern` — an impl's target, with the block's own parameters
  /// left as parameters — can stand for `concrete`.
  ///
  /// `impl get_it for boxed[int32]` and `impl get_it for boxed[int64]` share
  /// a declaration, which is all method lookup used to consult, so a
  /// `boxed[int64]` receiver could resolve to the `int32` impl's body and be
  /// compiled under the wrong types.
  [[nodiscard]] auto target_pattern_matches(type_id pattern,
                                            type_id concrete) const -> bool;

  /// Whether `id` mentions a type parameter anywhere inside it — i.e.
  /// whether it is a *pattern* standing for many concrete types
  /// (`holder[T]`) rather than one (`holder[int32]`).
  [[nodiscard]] auto mentions_param(type_id id) const -> bool;

private:
  [[nodiscard]] auto resolve_declared(const type_entry &entry, type_id instance,
                                      std::string_view name,
                                      const method_filter &accepts) const
      -> const method_entry *;
  [[nodiscard]] auto resolve_builtin_impl(const type_entry &entry,
                                          std::string_view name) const
      -> const method_entry *;
  [[nodiscard]] auto resolve_builtin_extend(const type_entry &entry,
                                            std::string_view name) const
      -> const method_entry *;

  const type_table *types_;
  const method_tables *tables_;
  builtin_probe has_inherent_;
};

} // namespace cinder::semantic::detail
