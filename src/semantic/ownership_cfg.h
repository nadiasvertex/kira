#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/semantic/checker_results.h"

/// The control-flow graph the ownership checker (`check_graph`) runs its
/// dataflow over. One `function_cfg` is built per function, lambda or task
/// body, from the typed core (`core::ownership_graphs`), and records — in
/// evaluation order — every event that matters to ownership: each access to
/// a local (read, move, borrow, write, end of scope) and each flow of
/// borrows from one holder to another.
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
inline constexpr auto k_no_param = std::numeric_limits<std::uint32_t>::max();

/// What a local stands for — used to choose how a diagnostic describes the
/// value that still holds a borrow.
enum class local_role : std::uint8_t {
  binding,      ///< A parameter or `let`/`var`/pattern binding the user named.
  loop_source,  ///< What a `for` loop iterates; held for the whole loop.
  call_temp,    ///< What a call or aggregate holds while it is evaluated.
  join_temp,    ///< The value an `if`/`match`/block expression produces.
  subject_temp, ///< A `match`/`if let`/destructuring subject.
  return_slot,  ///< The value the function returns.
  parameter_storage, ///< Caller storage a parameter or capture reaches: its
                     ///< tokens are rooted here, and it is live after the
                     ///< body exits, so a borrow stored into it outlives the
                     ///< body's locals.
  spawned_tasks, ///< What tasks started with `spawn()` outside a `crew`
                 ///< borrow: they may still run after this body returns.
  field_path,   ///< A field of an owning local reached through fields only
                ///< (`j.output`, `j.a.b`), tracked so a partial move of it
                ///< is known. Its `parent` is the path one field shorter.
  crew_tasks,   ///< What the tasks spawned into a `crew` borrow; held until
                ///< the crew ends.
  temporary,    ///< A value that is not a place, stored so it can be
                ///< borrowed (`&make()`, `make().len()`). It is dropped at
                ///< the end of its statement (ch. 17, When drops run).
};

struct local_info {
  std::string name{}; ///< The user's spelling; for a temporary, a description.
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
  std::vector<local_id> children{};
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
  /// Bound with `let`, so nothing may change it in place: no `&mut` of it
  /// and no `mut self` call on it, unless the change lands behind a
  /// reference or pointer it holds (ch. 3).
  bool immutable = false;
  /// Immutable because a `match`, `if let`, `while let` or `for` pattern
  /// binds it, rather than a `let`.
  bool by_pattern = false;
  /// A `parameter_storage` local that stands for a capture of the enclosing
  /// function's value rather than for a parameter: worded so in diagnostics.
  bool capture = false;
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
  /// A `capture` loan a lambda took by the borrow-or-move rule (ch. 16): no
  /// capture list, or a bare `name` entry, rather than `&name`/`&mut name`.
  /// When the closure escapes, the capture moves instead.
  bool implicit = false;
  /// A `capture` loan only: the closure it was taken for, by its
  /// `ast::lambda_expr` (`lambda::closure_origin`).
  const void *closure = nullptr;
  source_span span = source_span::dummy();
  /// A `capture` loan only: the name of the closure the capture was made
  /// for, when a `let` binds it. A value that later carries the loan (the
  /// result of a call the closure was passed to) is not the closure, so a
  /// conflict names the closure by this.
  std::string carrier{};
  /// A token for parameter `param` (`function_cfg::parameters`): what the
  /// caller's argument carries. Its `root` is the parameter's
  /// `parameter_storage` local. An `identity` token is the reference the
  /// argument is (the caller's place it points at); any other token is the
  /// borrows that place, or a by-value argument, holds. Function summaries
  /// are read off which tokens reach the result and each parameter's
  /// storage (`summary_of`).
  std::uint32_t param = k_no_param;
  bool identity = false;
  /// Made by borrowing a place reached through two or more references
  /// (`&self.src[i]` with `src: &list[T]`): it borrows what the parameter's
  /// referent points at, not the referent itself.
  bool deep = false;
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
  element,       ///< `xs[i]`: the collection still owns it.
  deref,         ///< `*r`: it is behind a pointer or reference.
  through_deref, ///< `h.x` found through `deref()`: only lent by the handle.
  borrowed,      ///< The root only borrows its value (`self`, a reference).
  own_drop,      ///< A field of a type with its own `drop`.
  fill,          ///< `[v; n]` duplicates `v`, which is not `copy`.
  overlapping, ///< A non-`copy` name bound by a pattern whose bindings overlap.
  captured,    ///< A closure's capture: the closure keeps it between calls.
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
/// value); without it, the loans are added (a field or element store). A
/// value read from behind a parameter's reference (`p.x` with `p: &T`) is
/// stored where that reference points, so it is not the reference: it does
/// not carry the `identity` token of the `strip` storage.
struct flow_event {
  local_id dest = 0;
  std::vector<loan_id> loans;
  std::vector<local_id> sources;
  bool replace = false;
  local_id strip = k_no_local;
};

/// A store into the storage a value points at, through references: `*r =
/// v`, `r.f = v` with `r` a reference, or what a callee stores through an
/// argument (`function_summary::stores`). The targets are found from the
/// loans the `through` holders carry: the roots of their `&mut` loans, then
/// the roots of those roots' `&mut` loans, `depth` times (one level per
/// reference the written place dereferences), or with `transitive` as far
/// as they reach. Every holder and target receives `loans` plus what the
/// `sources` carry (`strip` as for a flow), except loans rooted at a
/// target: a store never makes storage borrow itself.
struct store_event {
  std::vector<local_id> through;
  std::vector<loan_id> loans;
  std::vector<local_id> sources;
  local_id strip = k_no_local;
  std::uint32_t depth = 0;
  bool transitive = false;
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

/// What a `static_write_event` does to the static.
enum class static_write : std::uint8_t {
  assign,     ///< `xs = v`, `xs[i] = v`, `p.x += 1`.
  borrow_mut, ///< `&mut xs`.
  receiver,   ///< `xs.push(4)`: a `mut self` or `&mut` receiver.
  move,       ///< Moving a non-`copy` part out of it (`xs[0]`).
};

/// A write, mutable borrow, or move through a `static`. Statics are frozen
/// data that every function and task shares, so each of these is an error.
/// `name` is the static; `method` is the called method for `receiver`.
struct static_write_event {
  source_span span = source_span::dummy();
  static_write kind = static_write::assign;
  std::string name;
  std::string method;
};

/// A `&mut` borrow of, or a `mut self` call on, a binding made with `let`
/// (ch. 3). Assigning to one is the checker's to reject; these two ways of
/// changing it in place are seen here. `kind` is `borrow_mut` or
/// `receiver`; `method` is the called method for `receiver`; `declared` is
/// where the binding was made.
struct let_write_event {
  source_span span = source_span::dummy();
  static_write kind = static_write::borrow_mut;
  std::string name;
  std::string method;
  source_span declared = source_span::dummy();
  /// A `match`, `if let`, `while let` or `for` pattern bound it.
  bool by_pattern = false;
};

/// A write, `&mut`, or `mut self` call that would change something behind a
/// read-only reference: a `&T`, a `*T`, or a shared view (ch. 14). `place`
/// is the place as written; `through` is the read-only type it is reached
/// through; `method` is the called method for `receiver`.
struct shared_write_event {
  source_span span = source_span::dummy();
  static_write kind = static_write::assign;
  std::string place;
  std::string method;
  std::string through;
};

/// A write, `&mut`, or `mut self` call on `name` inside an `async:` or
/// `on(...)` block that only borrows it from the enclosing function
/// (`task_capture`). `method` is the called method for `receiver`.
struct task_capture_write_event {
  source_span span = source_span::dummy();
  static_write kind = static_write::assign;
  std::string name;
  std::string method;
};

using event =
    std::variant<access_event, flow_event, store_event, use_event,
                 scope_exit_event,
                 invalid_move_event, assign_event, static_write_event,
                 let_write_event, shared_write_event, task_capture_write_event>;

struct basic_block {
  std::vector<event> events;
  std::vector<block_id> successors;
};

/// A captured value a closure's environment owns: moved in when the lambda
/// is created, so the closure's drop glue drops it.
struct owned_capture {
  std::string name;
  type_id type = k_unknown_type;
  /// Unused: null.
  const void *node = nullptr;
};

/// A value an `async:` or `on(...)` block captures from the enclosing
/// function. The block runs as a task of its own, built as a function whose
/// parameters are these captures. A `moved` capture is one the body
/// consumes: it moves into the task, which owns it as a local of its own
/// graph declared by `node` (its `core::capture`), and drops it there. Any
/// other capture is lent to the task: the task value holds a shared loan of
/// it until it is dropped or awaited.
struct task_capture {
  std::string name;
  type_id type = k_unknown_type;
  bool moved = false;
  const void *node = nullptr;
  /// The task block.
  source_span span = source_span::dummy();
};

/// What a body is given through one parameter or capture: the local that
/// binds it, the `parameter_storage` local its tokens are rooted at, and the
/// tokens. A parameter whose type carries no borrow has no storage.
struct parameter_tokens {
  local_id local = k_no_local;
  local_id storage = k_no_local;
  loan_id contents = k_no_loan;
  loan_id identity = k_no_loan;
  /// The parameter is a reference: a borrow of its referent is a borrow of
  /// the caller's place, not of the parameter's own storage.
  bool by_reference = false;
};

/// Which part of an argument a summary entry carries: the borrows the
/// argument holds (`contents`), or the place a reference argument points at
/// (`referent`), or both.
inline constexpr std::uint8_t k_contents = 1;
inline constexpr std::uint8_t k_referent = 2;

/// A borrow a callee may store into storage it reaches through parameter
/// `into`: the parts of parameter `from`'s argument.
struct stored_borrow {
  std::uint32_t into = 0;
  std::uint32_t from = 0;
  std::uint8_t parts = 0;
  auto operator==(const stored_borrow &) const -> bool = default;
};

/// What a call does with the borrows one argument carries.
struct summary_param {
  /// The parts of the argument the result may carry.
  std::uint8_t result = 0;
  /// The parts a task the callee starts with `spawn()` outside a `crew` may
  /// hold: they may be used until the program ends.
  std::uint8_t escapes = 0;
  /// The parameter is a reference, so the argument has a `referent` part.
  bool by_reference = false;
  auto operator==(const summary_param &) const -> bool = default;
};

/// What a call does with the borrows its arguments carry
/// (`spec/unified-checker-plan.md`, Ownership on the core): which parts of
/// each argument may reach the result, and which parts the callee may store
/// into storage another argument reaches. Inferred from the body
/// (`check_graph`); for a call with no body to infer from, derived from the
/// signature.
struct function_summary {
  std::vector<summary_param> params;
  std::vector<stored_borrow> stores;
  auto operator==(const function_summary &) const -> bool = default;
};

/// The summary of each function with a body, by its declaration (a
/// template's, for an instance).
using summary_table =
    std::unordered_map<const ast::func_decl *, function_summary>;

struct function_cfg {
  /// The parameters, in order, receiver first; empty for a lambda or task.
  std::vector<parameter_tokens> parameters;
  /// The captures of a lambda or task body, which are given to it as a
  /// parameter is.
  std::vector<parameter_tokens> captures;
  block_id exit = 0;
  local_id return_slot = k_no_local;
  /// The `spawned_tasks` holder, or `k_no_local`.
  local_id spawned = k_no_local;
  /// A function's own graph, not a lambda's or task block's: what it does
  /// with its parameters is its summary.
  bool is_function = false;
  /// The function is a generator or async function: its frame holds every
  /// argument, so each part of each argument may reach the result.
  bool coroutine = false;
  /// Where a lambda's captures move because the closure escapes (ch. 16):
  /// the lambda's span, for the note on a later use of a moved value.
  std::vector<source_span> escaping_lambdas;
  /// The `match` arms (by `match_arm::origin`) whose bindings overlap, so
  /// the arm owns the whole subject as a `<subject>` local.
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
  /// The calls whose receiver is consumed by `into_iter`.
  std::set<const void *> consuming_calls;
  /// The droppable values each lambda moves into its environment, in
  /// capture order, keyed by the `ast::lambda_expr`.
  std::map<const void *, std::vector<owned_capture>> owned_captures;
  /// What each `async:` or `on(...)` block captures, in first-reference
  /// order, keyed by the block's expression.
  std::map<const void *, std::vector<task_capture>> task_captures;
  std::vector<local_info> locals;
  std::vector<loan_info> loans;
  std::vector<basic_block> blocks; ///< `blocks[0]` is the entry.
};

} // namespace cinder::semantic::ownership
