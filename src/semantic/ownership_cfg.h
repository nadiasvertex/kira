#pragma once

#include <cstdint>
#include <functional>
#include <limits>
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
};

/// Why a value cannot be moved out of a place (ch. 14, Moving out of places).
enum class move_block : std::uint8_t {
  element,  ///< `xs[i]`: the collection still owns it.
  deref,    ///< `*r`: it is behind a pointer or reference.
  borrowed, ///< The root only borrows its value (`self`, a reference).
  own_drop, ///< A field of a type with its own `drop`.
  fill,     ///< `[v; n]` duplicates `v`, which is not `copy`.
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

using event = std::variant<access_event, flow_event, use_event,
                           scope_exit_event, invalid_move_event>;

struct basic_block {
  std::vector<event> events;
  std::vector<block_id> successors;
};

struct function_cfg {
  std::vector<local_info> locals;
  std::vector<loan_info> loans;
  std::vector<basic_block> blocks; ///< `blocks[0]` is the entry.
};

/// Whether the bindings of `pattern`, matched against `subject`, own the
/// parts of the value they bind — so each may be dropped when its scope ends.
/// True when the subject is a whole local or a fresh value (a call result, a
/// constructor), which the match moves; false when it is a part of something
/// (`x.f`, `x[i]`, `*x`), a reference or view, `self`, or a global — those
/// still belong to their owner — or when the pattern's bindings overlap
/// (`pattern_bindings_are_disjoint`). `is_local` says whether a name is a
/// local of the function. `hir::lowerer` asks the same question, so both
/// sides agree on which bindings are dropped.
[[nodiscard]] auto owns_pattern_bindings(type_id subject_type,
                                         const ast::node &pattern,
                                         const checked_types &checked) -> bool;

/// Whether the single-name loop variable of `stmt` owns the element the loop
/// hands it, so it is dropped at the end of each iteration. True when the
/// loop calls `next` on an iterator or generator that yields values (not
/// references or views). A loop over a collection (`for x in xs`, through
/// `into_iterator`), an `option`, a range or a string borrows: the container
/// drops its own elements. `hir::lowerer` asks the same question.
[[nodiscard]] auto for_variable_owns(const ast::for_stmt &stmt,
                                     const checked_types &checked) -> bool;

/// The same question for a value of known type that no expression names,
/// such as a by-value destructuring parameter: true unless the pattern's
/// bindings overlap or the type is unknown, a reference or a view.
[[nodiscard]] auto
owns_pattern_bindings(const ast::expr &subject, const ast::pattern &pattern,
                      const checked_types &checked,
                      const std::function<bool(std::string_view)> &is_local)
    -> bool;

/// Builds the CFG of `decl`'s body, followed by one CFG per lambda or nested
/// function written inside it.
[[nodiscard]] auto build_function_cfgs(const ast::func_decl &decl,
                                       const checked_types &checked)
    -> std::vector<function_cfg>;

} // namespace cinder::semantic::ownership
