#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/semantic/types.h"

/// The control-flow graph the ownership checker (`check_ownership`) runs its
/// dataflow over. One `function_cfg` is built per function or lambda body,
/// straight from the AST, and records — in evaluation order — every event
/// that matters to ownership: each access to a local (read, move, borrow,
/// write, end of scope) and each flow of borrows from one holder to another.
///
/// The model is deliberately small:
///
///   * A **loan** is one borrow of a local: an explicit `&`/`&mut`, the
///     implicit autoref of an argument or receiver, a `slice`/`cell` view
///     made by indexing, or a closure's `&`/`&mut` capture.
///   * A **holder** is any local — a user binding or a compiler temporary —
///     whose value carries loans. `flow_event`s say which loans a holder
///     receives, directly or by copying another holder's.
///   * A loan is **live** wherever some holder carrying it is live (will be
///     used again). No lifetime annotations are needed: a loan lasts exactly
///     as long as the values that carry it.
///
/// Every access is then checked against the live loans of the local it
/// touches, with one conflict table (`check_ownership`), and moves are
/// tracked by a forward "maybe moved" pass over the same graph.
namespace cinder::semantic::ownership {

using local_id = std::uint32_t;
using loan_id = std::uint32_t;
using block_id = std::uint32_t;

inline constexpr auto k_no_loan = std::numeric_limits<loan_id>::max();
inline constexpr auto k_no_local = std::numeric_limits<local_id>::max();

/// What a local stands for — used to choose how a diagnostic describes the
/// value that still holds a borrow.
enum class local_role : std::uint8_t {
  binding,      ///< A parameter or `let`/`var`/pattern binding the user named.
  loop_source,  ///< What a `for` loop iterates; held for the whole loop.
  call_temp,    ///< What a call or aggregate holds while it is evaluated.
  join_temp,    ///< The value an `if`/`match`/block expression produces.
  subject_temp, ///< A `match`/`if let`/destructuring subject.
  return_slot,  ///< The value the function returns.
  field_path,   ///< A field of an owning local reached through fields only
                ///< (`j.output`, `j.a.b`), tracked so a partial move of it
                ///< is known. Its `parent` is the path one field shorter.
  temporary,    ///< A value that is not a place, stored so it can be
                ///< borrowed (`&make()`, `make().len()`). It is dropped at
                ///< the end of its statement (ch. 17, When drops run).
};

struct local_info {
  std::string name; ///< The user's spelling; for a temporary, a description.
  local_role role = local_role::binding;
  type_id type = k_unknown_type;
  source_span span = source_span::dummy();
  /// Moving it transfers ownership, so a later use is a use-after-move.
  /// False for `copy` types (ch. 14) and anything whose type is unknown.
  bool movable = false;
  /// Owns its value, so a field may be moved out of it: a `let`/`var`
  /// binding, a parameter other than `self`, or a pattern binding that owns
  /// what it binds. A binding that only borrows its value (`self`, a
  /// reference, a loop variable over a collection) does not.
  bool owns = false;
  /// For a `field_path` local, the local it is a field of; `k_no_local`
  /// otherwise.
  local_id parent = k_no_local;
  /// The `field_path` locals one field below this one.
  std::vector<local_id> children;
  /// Bound whole — by a plain `let`/`var name`, or a parameter that is a
  /// single name — so it owns its value outright. A pattern binding may
  /// instead alias part of its subject (partial moves are not tracked), so
  /// dropping it at scope exit could drop storage something else still owns.
  bool whole = false;
  /// The declaration that binds it — a `let`/`var` binding pattern, a `var`
  /// statement, or a single-name parameter's pattern — where lowering can
  /// set its drop flag (`hir::compute_drop_schedule`) and find the local
  /// again (`hir::lowerer` declares the same node). A pattern binding is
  /// declared by its pattern, shorthand field or aliased group
  /// (`pattern_binding::key`), a `<for iterator>` by the loop's `iterable`,
  /// a `<subject>` by its pattern. Null for a temporary or a `field_path`
  /// local.
  const void *node = nullptr;
};

/// How a loan was made — used only to word diagnostics.
enum class loan_origin : std::uint8_t {
  borrow,   ///< An explicit `&`/`&mut`, or an argument's implicit autoref.
  receiver, ///< A method receiver's implicit autoref.
  view,     ///< A `slice`/`cell` view made by indexing.
  capture,  ///< A closure's `&`/`&mut` capture-list entry.
};

struct loan_info {
  local_id root = 0;
  bool is_mut = false;
  /// A two-phase receiver reservation: shared while the call's arguments are
  /// evaluated, but a real `&mut` of the receiver — so it conflicts as a
  /// shared loan and reports as a mutable one.
  bool reserved_mut = false;
  loan_origin origin = loan_origin::borrow;
  source_span span = source_span::dummy();
};

enum class access_kind : std::uint8_t {
  read,          ///< Reads or copies the value; ownership stays put.
  move,          ///< Transfers ownership out of the local.
  borrow_shared, ///< Makes a `&` loan of it.
  borrow_mut,    ///< Makes a `&mut` loan of it.
  write_part,    ///< Writes into it: a field, element, or compound assignment.
  write_whole,   ///< Replaces its whole value (`x = ...`), reinitializing it.
  storage_dead,  ///< Its scope ends; nothing may still borrow it.
};

/// An access to `local`. `exempt` is a loan this access is allowed to
/// coexist with — the reservation a two-phase receiver's activation turns
/// into a real `&mut`.
struct access_event {
  local_id local = 0;
  access_kind kind = access_kind::read;
  source_span span = source_span::dummy();
  loan_id exempt = k_no_loan;
  /// The access touches only part of `local` (a field, element, or what it
  /// points at). It conflicts with `local`'s loans as usual, but a move does
  /// not move `local` itself, and only a move of `local` itself makes it a
  /// use after move. A part reached through fields alone is also accessed as
  /// its own `field_path` local.
  bool projected = false;
  /// For a move, the expression whose evaluation moves the value (the
  /// place expression, a struct literal's shorthand field, or the lambda
  /// that captures it): where lowering clears a drop flag.
  const void *node = nullptr;
};

/// Why a value cannot be moved out of a place (ch. 14, Moving out of places).
enum class move_block : std::uint8_t {
  element,  ///< `xs[i]`: the collection still owns it.
  deref,    ///< `*r`: it is behind a pointer or reference.
  borrowed, ///< The root only borrows its value (`self`, a reference).
  own_drop, ///< A field of a type with its own `drop`.
  fill,     ///< `[v; n]` duplicates `v`, which is not `copy`.
  iterated, ///< `for x in r`: `into_iter` would consume what `r` borrows.
  overlapping, ///< A non-`copy` name bound by a pattern whose bindings overlap.
};

/// A by-value use of a non-`copy` place the rules forbid moving out of.
/// `place` spells the place up to the part that blocks the move; `owner` is
/// the root name, or for `own_drop` the type that implements `drop`.
struct invalid_move_event {
  source_span span = source_span::dummy();
  move_block reason = move_block::element;
  std::string place;
  std::string owner;
};

/// `dest` receives `loans`, plus every loan the `sources` hold right now.
/// With `replace`, whatever `dest` held before is discarded (a whole new
/// value); without it, the loans are added (a field or element store).
struct flow_event {
  local_id dest = 0;
  std::vector<loan_id> loans;
  std::vector<local_id> sources;
  bool replace = false;
};

/// A use of a temporary holder with no access of its own — the point a call
/// runs, or a loop asks its source for the next element. Keeps the holder,
/// and so its loans, live up to here.
struct use_event {
  local_id local = 0;
};

/// A point where scopes end, just before their locals' `storage_dead`
/// accesses — what drop scheduling (`hir::compute_drop_schedule`) reads.
/// `key` names the exit the way `hir::lowerer` does: the address of the
/// statement vector whose scope closes normally, or of the
/// `return`/`break`/`continue` that leaves early. `groups` holds, per scope
/// ended (innermost first), its `whole` locals that own storage, in reverse
/// declaration order. The checker ignores it.
struct scope_exit_event {
  const void *key = nullptr;
  std::vector<std::vector<local_id>> groups;
};

/// An assignment `x = v` or `x.f = v` about to overwrite its target, after
/// `v` has been evaluated — what drop scheduling reads to decide whether the
/// old value must be dropped first. `local` is the target's local or
/// `field_path` local, or `k_no_local` for a place reached through a borrow
/// (`self.f`, `r.f`, `*r`), which always holds a value. `key` is the
/// `ast::assign_stmt`. The checker ignores it.
struct assign_event {
  const void *key = nullptr;
  local_id local = k_no_local;
};

using event = std::variant<access_event, flow_event, use_event,
                           scope_exit_event, invalid_move_event, assign_event>;

struct basic_block {
  std::vector<event> events;
  std::vector<block_id> successors;
};

struct function_cfg {
  /// The top-level patterns whose bindings own the parts they bind
  /// (`owns_pattern_bindings`), and those whose arm owns the whole subject
  /// (`arm_owns_subject`) — what `hir::lowerer` reads.
  std::set<const void *> owning_patterns;
  std::set<const void *> owning_subjects;
  /// Where an owning pattern leaves a droppable part unbound: its `_`
  /// patterns, and its struct patterns whose `..` skips a droppable field.
  std::set<const void *> leftover_drops;
  /// Where each temporary ends: the expression whose value was stored in
  /// one (borrowed while not a place, read a field from, or computed and
  /// discarded), mapped to the node of the statement or full expression it
  /// ends with. Null when it ends with no statement of this body.
  /// `hir::make_references_explicit` drops it there, finding both through
  /// `hir_node::origin`.
  std::map<const void *, const void *> temporary_ends;
  /// The top-level owning patterns whose subject is still owned by the path
  /// where the pattern misses (`let else`, `if let`, `while let`).
  std::set<const void *> unmatched_drops;
  /// The `for` statements and comprehension clauses whose loop variables own
  /// the elements they bind (`for_variable_owns`, `clause_variable_owns`).
  std::set<const void *> owning_loops;
  /// The keys (a loop's `iterable` member) of loops that hold an iterator
  /// they must drop (`loop_handle_type`).
  std::set<const void *> loop_handles;
  /// The calls whose receiver is consumed by `into_iter`.
  std::set<const void *> consuming_calls;
  std::vector<local_info> locals;
  std::vector<loan_info> loans;
  std::vector<basic_block> blocks; ///< `blocks[0]` is the entry.
};

/// The key of the exit a `generator def` takes when it is dropped before
/// its body first runs. A generator dropped while suspended at a `yield`
/// exits there instead, keyed by the `ast::yield_expr`.
[[nodiscard]] auto generator_start(const ast::func_decl &decl) -> const void *;

/// Builds the CFG of `decl`'s body, followed by one CFG per lambda or nested
/// function written inside it.
[[nodiscard]] auto build_function_cfgs(const ast::func_decl &decl,
                                       const checked_types &checked)
    -> std::vector<function_cfg>;

} // namespace cinder::semantic::ownership
