#pragma once

#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <utility>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>

#include "src/core/nodes.h"
#include "src/hir/drop_emission.h"
#include "src/hir/drop_schedule.h"
#include "src/hir/lower_core.h"
#include "src/hir/nodes.h"

/// Internal to `lower_core.h`: the lowerer of one core function, one method
/// per core node kind. Split across `lower_core.cpp` (functions and the
/// substitution), `lower_core_expr.cpp` (expressions, places, calls) and
/// `lower_core_stmt.cpp` (statements and blocks).
namespace cinder::hir {

template <typename T> using core_result = std::expected<T, unsupported>;

/// The name of a core node kind, as an `unsupported` reports it.
[[nodiscard]] auto core_kind_name(core::node_kind kind) -> std::string;

class core_lowerer {
public:
  core_lowerer(const semantic::checked_types &checked,
               const lowering_options &options, std::string_view core_module,
               std::string_view hir_module)
      : checked_(checked), options_(options), core_module_(core_module),
        hir_module_(hir_module),
        drops_(checked,
               drop_emitter::environment{
                   .mint = [this]() -> symbol_id { return mint(); },
                   .whole_local = [this](const void *node)
                       -> std::optional<symbol_id> {
                     return whole_local(node);
                   },
                   .function_symbol = [this](std::string_view name)
                       -> symbol_id { return callee_symbol(name); }}) {}

  [[nodiscard]] auto function(const core::function &source)
      -> core_result<ptr<hir_function>>;

  // ---- expressions (lower_core_expr.cpp) ---------------------------------
  [[nodiscard]] auto expr(const core::expr &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto literal(const core::literal &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto use(const core::use &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto borrow(const core::borrow &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto call(const core::call &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto intrinsic_call(const core::call &source)
      -> core_result<ptr<hir_expr>>;
  /// A container operation's receiver as the old lowerer reads it.
  [[nodiscard]] auto receiver_operand(const core::expr &arg)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto primitive(const core::primitive &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto cast(const core::cast &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto aggregate(const core::aggregate_init &source)
      -> core_result<ptr<hir_expr>>;
  [[nodiscard]] auto variant(const core::variant_init &source)
      -> core_result<ptr<hir_expr>>;
  /// An `if`, typed `type` in HIR (see `hir_type_in`).
  [[nodiscard]] auto if_expr(const core::if_expr &source, type_id type)
      -> core_result<ptr<hir_if>>;
  /// The place `source` names, as the chain of projections HIR reads and
  /// writes through.
  [[nodiscard]] auto place(const core::place &source, source_span span)
      -> core_result<ptr<hir_expr>>;
  /// The callee reference of a call to a declared function.
  [[nodiscard]] auto function_callee(const core::callee &target,
                                     source_span span) -> ptr<hir_expr>;

  // ---- lambdas (lower_core_lambda.cpp) -----------------------------------
  [[nodiscard]] auto lambda(const core::lambda &source)
      -> core_result<ptr<hir_expr>>;
  /// The drop glue of `closure`, lowered from `source`: the drops of what
  /// its environment owns (`drop_schedule::owned_captures`), or null.
  [[nodiscard]] auto drop_glue(const core::lambda &source,
                               const hir_lambda &closure)
      -> core_result<ptr<hir_lambda>>;

  // ---- matches (lower_core_match.cpp) ------------------------------------
  using place_fn = drop_emitter::place_fn;
  /// A `match`, `if let`, `or else` or `?` (or a `for` over an `option`) as
  /// the `hir_match` the old lowerer builds, typed `type`.
  [[nodiscard]] auto match(const core::match_expr &source, type_id type)
      -> core_result<ptr<hir_match>>;
  /// The subject as the source wrote it: the place without the `deref`s
  /// the elaboration added, read with its origin and drop flags.
  [[nodiscard]] auto subject_value(const core::match_expr &source)
      -> core_result<ptr<hir_expr>>;
  /// The type of the subject as the source wrote it.
  [[nodiscard]] auto subject_type(const core::match_expr &source) const
      -> type_id;
  /// The HIR pattern for `test`, the part `part` names. The bindings of
  /// `alternative` at each part become `hir_let`s in `pending`, with the
  /// drops of what a `_` or `..` leaves, in the old lowerer's order.
  /// One step of the path from a match subject to the part a pattern
  /// tests, as a binding's `path` spells it.
  struct pattern_step {
    core::projection::step kind;
    std::size_t index = 0;
    std::string name;
  };
  [[nodiscard]] auto pattern(const core::pattern_test &test,
                             const place_fn &part,
                             std::vector<pattern_step> &path,
                             const core::pattern_alternative &alternative,
                             std::size_t &next_binding, bool own_bindings,
                             ptr_vec<hir_node> &pending)
      -> core_result<ptr<hir_pattern>>;
  /// `pattern` from the top of `alternative`, checking every binding was
  /// placed.
  [[nodiscard]] auto alternative_pattern(
      const core::pattern_alternative &alternative, const place_fn &subject,
      bool own_bindings, ptr_vec<hir_node> &pending)
      -> core_result<ptr<hir_pattern>>;
  /// The HIR arms of `arm`: one, or one per `|` alternative when they bind
  /// names. `before_body` are statements the body starts with.
  auto arm_into(const core::match_expr &source, const core::match_arm &arm,
                const place_fn &subject, type_id body_type,
                std::vector<hir_match_arm> &out) -> core_result<void>;
  /// A destructuring `let` or `let ... else`: the statements it lowers to,
  /// then the rest of the block (the first arm's body) in the same scope.
  /// Returns that block, whose statements the caller lowers next.
  auto let_pattern_into(const core::match_expr &source,
                        ptr_vec<hir_node> &out)
      -> core_result<const core::block *>;
  /// A `while let`, or a `for` loop's test of `next`: the `while_loop`
  /// holding only `source`.
  auto while_let_into(const core::match_expr &source, ptr_vec<hir_node> &out)
      -> core_result<void>;
  /// A `for` loop's element binding (`match_form::for_element`) and the
  /// loop body, appended to `out`.
  auto for_element_into(const core::match_expr &source,
                        ptr_vec<hir_node> &out) -> core_result<void>;
  /// The loop variables of a `for` from `element`, then the body of `arm`.
  /// `prefix` is the path from the match subject to the element; a single
  /// plain loop variable owns what it binds only when `own_plain`.
  auto loop_body_into(const core::match_arm &arm, const place_fn &element,
                      const core::pattern_test &head,
                      std::vector<pattern_step> prefix, bool own_plain,
                      ptr_vec<hir_node> &out) -> core_result<void>;

  // ---- statements (lower_core_stmt.cpp) ----------------------------------
  /// `source` as a HIR block of type `type`: a result becomes the trailing
  /// expression statement.
  [[nodiscard]] auto block(const core::block &source, type_id type)
      -> core_result<ptr<hir_block>>;
  /// `block` in a scope the caller opened (the function's body, whose
  /// parameters' scope is the function's).
  [[nodiscard]] auto block_in_scope(const core::block &source, type_id type)
      -> core_result<ptr<hir_block>>;
  /// The statements of `source` and of the blocks a pattern `let` carries
  /// on in, appended to `stmts`; a result becomes the last statement.
  auto statements_into(const core::block &source, type_id type,
                       ptr_vec<hir_node> &stmts) -> core_result<void>;
  /// A `for` loop's block (the loop state, then the loop), appended to
  /// `out` with the drop of the iterator it holds after it.
  auto for_block_into(const core::block &source, ptr_vec<hir_node> &out)
      -> core_result<void>;
  /// Appends the HIR statements for `source`. `tail_type` is set for the
  /// last statement of a block, whose `if` is the block's tail.
  auto stmt_into(const core::stmt &source, ptr_vec<hir_node> &out,
                 std::optional<type_id> tail_type) -> core_result<void>;
  auto let_into(const core::let &source, ptr_vec<hir_node> &out)
      -> core_result<void>;
  auto assign_into(const core::assign &source, ptr_vec<hir_node> &out)
      -> core_result<void>;
  auto expr_stmt_into(const core::expr_stmt &source, ptr_vec<hir_node> &out,
                      std::optional<type_id> tail_type) -> core_result<void>;
  auto while_into(const core::while_loop &source, ptr_vec<hir_node> &out)
      -> core_result<void>;
  auto jump_into(const core::jump &source, ptr_vec<hir_node> &out)
      -> core_result<void>;

  // ---- drops (lower_core_stmt.cpp) ---------------------------------------
  auto push_scope() -> void { scopes_.emplace_back(); }
  auto pop_scope() -> void { scopes_.pop_back(); }
  /// Records `symbol` as the local the binding `key` declares whole, which
  /// is how a scheduled drop names it (`pending_drop::node`).
  auto own(const void *key, symbol_id symbol) -> void {
    scopes_.back().emplace_back(key, symbol);
  }
  [[nodiscard]] auto whole_local(const void *key) const
      -> std::optional<symbol_id>;
  /// The symbol calls name the function `name` by: one per name.
  [[nodiscard]] auto callee_symbol(std::string_view name) -> symbol_id;
  /// `drop_emitter`'s error as this lowering's: the schedule asked for a
  /// drop it cannot make here.
  [[nodiscard]] static auto refuse_drop(const lowering_error &error)
      -> std::unexpected<unsupported> {
    return refuse(std::format("drop: {}", error.message), error.span);
  }

  // ---- contracts (lower_core_contract.cpp) --------------------------------
  auto contract_into(const core::contract_check &source,
                     ptr_vec<hir_node> &out) -> core_result<void>;
  /// The block the elaboration makes of a `return` that checks
  /// postconditions or ends crews (`let return = v; <post checks>; <crew
  /// ends>; return return`), if `node` (a statement or a block) is one.
  [[nodiscard]] static auto exit_block(const core::node *node)
      -> const core::block *;
  /// Lowers `exit` as the old lowerer lowers the `return` it stands for:
  /// the value, held in a temporary across the crew ends and the drops,
  /// then bound to `return` for the checks. `tail` is the value already
  /// lowered, for a function body's tail.
  auto exit_into(const core::block &exit, ptr_vec<hir_node> &out,
                 ptr<hir_expr> tail = nullptr) -> core_result<void>;
  /// `return value`, through the postconditions `posts` when there are
  /// any: `let return = value` (the core's `bound`, or a fresh symbol),
  /// the checks, `return return`. A task yields the value instead.
  auto return_through_posts(
      source_span span, ptr<hir_expr> value,
      std::optional<symbol_id> bound,
      const std::vector<const core::contract_check *> &posts,
      ptr_vec<hir_node> &out) -> core_result<void>;
  /// The statements of `source` to lower in order: for the function's
  /// body, a trailing exit with a value or trailing postcondition checks
  /// are left for `finish_contracts` (recorded in `tail_exit_` and
  /// `tail_posts_`).
  auto tail_contracts(const core::block &source) -> std::size_t;
  /// After the body scope's drops, as the old lowerer does: the tail value
  /// returns through the postconditions, or a body that runs off its end
  /// checks them last.
  auto finish_contracts(ptr_vec<hir_node> &stmts, source_span span)
      -> core_result<void>;
  /// The block checking a type's invariant after a field write (`let self
  /// = &object; <invariant check>`), if `value` is one.
  [[nodiscard]] static auto invariant_block(const core::expr &value)
      -> const core::block *;

  // ---- coroutines (lower_core_task.cpp) ----------------------------------
  /// `if <cancelled> { cancel awaited; drops; <scheduled drops of key>;
  /// return }`: what a coroutine resumed only to be dropped runs where it
  /// was suspended (`key`, a drop schedule key) or before its body.
  [[nodiscard]] auto cancel_check(const void *key, source_span span,
                                  ptr_vec<hir_expr> awaited = {},
                                  ptr_vec<hir_node> drops = {})
      -> core_result<ptr<hir_node>>;

  // ---- helpers -----------------------------------------------------------
  /// The type table, for the few types lowering interns that checking had
  /// no occasion to (a runtime call's `unit`), as the old lowerer does.
  [[nodiscard]] auto interning_types() const -> semantic::type_table & {
    return const_cast<semantic::type_table &>(checked_.types);
  }
  /// `type` as HIR holds it: an opaque type is the type it stands for.
  [[nodiscard]] auto hir_type(type_id type) const -> type_id;
  /// The type of a block or `if` in statement position: a `unit` one is
  /// untyped in HIR, as the old lowerer leaves it.
  [[nodiscard]] auto hir_type_in_statement(type_id type) const -> type_id;
  [[nodiscard]] auto mint() -> symbol_id { return next_symbol_++; }
  /// A fresh HIR symbol for the core local `symbol`, named `name`.
  auto declare(symbol_id symbol, const std::string &name) -> symbol_id;
  [[nodiscard]] static auto refuse(std::string kind, source_span span)
      -> std::unexpected<unsupported> {
    return std::unexpected(unsupported{.kind = std::move(kind), .span = span});
  }
  [[nodiscard]] static auto refuse(const core::node &node)
      -> std::unexpected<unsupported> {
    return refuse(core_kind_name(node.kind), node.span);
  }

private:
  const semantic::checked_types &checked_;
  const lowering_options &options_;
  std::string core_module_;
  std::string hir_module_;
  /// Each core local's HIR symbol.
  std::unordered_map<symbol_id, symbol_id> symbols_;
  /// Each local's name, for the references to it (diagnostics only).
  std::unordered_map<symbol_id, std::string> names_;
  /// The schedule key of each core local a `let` declares
  /// (`let::binding_origin`).
  std::unordered_map<symbol_id, const void *> binding_keys_;
  /// The core locals that stand for a part of a match subject (a binding
  /// the elaboration made up): a place rooted at one reads the part.
  std::unordered_map<symbol_id, place_fn> aliases_;
  /// The value of the `let` of a `for` loop's iterator being lowered: its
  /// adapter call consumes its receiver with no ownership record.
  const core::expr *for_adapter_ = nullptr;
  /// The function's body, whose tail `return` and trailing postcondition
  /// checks follow its scope's drops, as in the old lowerer.
  const core::block *function_body_ = nullptr;
  /// Set while lowering `function_body_` when it ends in an exit block with
  /// a value (`exit_block`) or postcondition checks: lowered after the
  /// scope's drops.
  const core::block *tail_exit_ = nullptr;
  std::vector<const core::contract_check *> tail_posts_;
  /// Lowering an `async def`'s body: a `return` hands over its value with a
  /// `yield` and an `await` suspends.
  bool in_task_body_ = false;
  /// One symbol per function a call names, as the old lowerer mints them.
  std::unordered_map<std::string, symbol_id> callee_symbols_;
  symbol_id next_symbol_ = 1;
  /// The bindings each open scope declares whole, by their schedule key.
  std::vector<std::vector<std::pair<const void *, symbol_id>>> scopes_;
  drop_emitter drops_;
};

} // namespace cinder::hir
