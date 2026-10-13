#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/parser/ast.h"
#include "src/parser/ast_clone.h"
#include "src/parser/source_location.h"
#include "src/semantic/method_resolution.h"
#include "src/semantic/types.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Instance queue — when each generic instance gets made
//
//  A call that names a generic instance clones the template's declaration
//  and queues it here. This file decides, per instance, *when* and *how* it
//  is made: checked by walking its clone, or made by substituting its
//  template's records; immediately, after its template has been checked, or
//  at the end of the session. It owns the queues and the template state
//  those decisions read (which templates are checked, which failed).
//
//  What making an instance involves — the checker context it runs under,
//  the body check, the substitution — belongs to the checker and is reached
//  through `instance_hooks`. The queue never reads checker state.
// ==========================================================================

/// One link in the chain of generic instantiations being checked. A
/// diagnostic raised inside a monomorphized body points back at the call that
/// asked for that body.
struct instantiation_frame {
  source_span call_span;      ///< The call that demanded this instance.
  file_id_type call_file = 0; ///< File that call was written in.
  std::string instance_name;  ///< e.g. `collect$list_iter_int32_$int32`.
  /// One line per parameter solved from the expected type rather than from
  /// an argument.
  std::vector<std::string> context_solutions;
  /// The frame is a use of the generic lambda `instance_name`, which checks
  /// its own copy of the lambda (`instantiate_generic_lambda`).
  bool lambda_use = false;
};

/// What one call pins a template's parameters down to: a constant per value
/// parameter, a concrete type per type parameter, and the name suffix the
/// instance carrying that solution is compiled under.
struct generic_solution {
  /// Value parameters, as the interned `const_value` types
  /// `push_type_params` binds them to.
  type_scope const_slots;
  /// Value parameters, as the raw numbers `lower_ident` embeds.
  std::unordered_map<std::string, int64_t> values;
  /// Type parameters, as the concrete types they solved to.
  type_scope type_slots;
  /// Every parameter's answer, in declaration order, as a symbol-safe
  /// suffix — `$3`, `$int32`, `$3$int32` for a mixed template.
  std::string suffix;
  /// Whether every bound the callee declares holds for this solution. When
  /// one does not, the call still has a result, but no instance is made.
  bool bounds_hold = true;
};

/// An instance that has been cloned and named but not yet made.
///
/// Everything the deferred half reads out of the checker is captured here at
/// *request* time, because by the time the queue runs the walk has moved on.
/// The one that matters most is `enclosing_type_params`: an instance body is
/// checked with the requester's type-parameter scopes in place, and
/// re-deriving them later is not possible.
struct pending_instance {
  const ast::func_decl *instance = nullptr;
  /// The template it was cloned from.
  const ast::func_decl *tmpl = nullptr;
  const module_members *owner = nullptr;
  std::optional<file_id_type> decl_file;
  generic_solution solution;
  std::optional<type_scope> fixed_type_params;
  type_id self_type = k_unknown_type;
  const std::vector<ast::type_param> *block_type_params = nullptr;
  /// The whole chain of frames, not just this one: the "instantiated from
  /// here" note walks it, and a deferred body has no enclosing stack left to
  /// read it off.
  std::vector<instantiation_frame> sites;
  /// This instance's own depth in that chain, so `f[n]` requesting `f[n+1]`
  /// still bottoms out once the recursion is a worklist.
  size_t depth = 0;
  std::vector<type_scope> enclosing_type_params;
  std::shared_ptr<const ast::clone_map> clone_pairs;
  /// An instance of a function with unannotated parameters, checked against
  /// its call's concrete types rather than substituted: its template is a
  /// probe, not a check.
  bool open_params = false;
};

/// What the queue asks of the checker to make one instance. Each call runs
/// under the context captured in the `pending_instance` it is given.
class instance_hooks {
public:
  virtual ~instance_hooks() = default;

  /// Whether `item` is checked by walking its clone rather than substituted
  /// from its template.
  virtual auto walks(const pending_instance &item) -> bool = 0;
  /// Whether every answer in `item` is a concrete type. An instance asked for
  /// at a type nothing solved is the requester's mistake, and is not made.
  virtual auto answers_are_concrete(const pending_instance &item) -> bool = 0;
  /// Checks the instance's own clone body.
  virtual auto check_clone(const pending_instance &item) -> void = 0;
  /// Checks the template's body, in the instance's context. Only asked of a
  /// template with no parameters of its own that was never checked.
  virtual auto check_template(const pending_instance &item) -> void = 0;
  /// Makes the instance's records from its template's.
  virtual auto substitute(pending_instance &item) -> void = 0;
  /// Registers a made instance for lowering.
  virtual auto record(const pending_instance &item) -> void = 0;
  /// Runs after an instance made by walking its clone has been recorded.
  virtual auto walked(const pending_instance &item) -> void = 0;
};

class instance_queue {
public:
  /// Queues an instance to be walked at the next `flush_walks`.
  auto request(pending_instance item) -> void;
  /// Makes `item` now, as a walk or a substitution, whichever it is.
  auto make_now(pending_instance &item, instance_hooks &hooks) -> void;

  /// A template's body has been checked. Every instance waiting on it is
  /// moved to the substitution queue. `failed` records that the template
  /// reported an error, so its instances are never made.
  auto template_checked(const ast::func_decl &tmpl, bool failed) -> void;

  [[nodiscard]] auto is_checked(const ast::func_decl &tmpl) const -> bool;
  [[nodiscard]] auto is_failed(const ast::func_decl &tmpl) const -> bool;

  [[nodiscard]] auto has_walks() const -> bool { return !walks_.empty(); }
  [[nodiscard]] auto has_substitutions() const -> bool {
    return !substitutions_.empty();
  }

  /// Makes every queued instance, including the ones those instances ask for.
  /// An instance that is substituted rather than walked moves to the
  /// substitution queue.
  auto flush_walks(instance_hooks &hooks) -> void;
  /// Makes every queued substitution whose template is checked; one whose
  /// template is not waits for it.
  auto flush_substitutions(instance_hooks &hooks) -> void;
  /// At the end of the session: instances whose template was never checked in
  /// its own right.
  auto flush_unchecked(instance_hooks &hooks) -> void;

private:
  auto make_by_substitution(pending_instance &item, instance_hooks &hooks)
      -> void;

  /// Instances cloned but not yet made, in request order.
  std::vector<pending_instance> walks_;
  /// Instances made by substitution, held until the end of the flush that
  /// requested them: their templates' records have to be settled first.
  std::vector<pending_instance> substitutions_;
  /// Instances whose template has not been checked yet, by template.
  std::unordered_map<const ast::func_decl *, std::vector<pending_instance>>
      waiting_;
  /// Every checked body, not only generic templates.
  std::unordered_set<const ast::func_decl *> checked_;
  /// Templates whose own check reported an error. Their instances are never
  /// made: an instance can only repeat what the template already said.
  std::unordered_set<const ast::func_decl *> failed_;
};

} // namespace cinder::semantic::detail
