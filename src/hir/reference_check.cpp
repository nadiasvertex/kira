#include "src/hir/reference_check.h"

#include <format>
#include <map>
#include <ranges>
#include <string>
#include <utility>

#include "src/hir/traversal.h"

namespace cinder::hir {
namespace {

using semantic::type_id;
using semantic::type_kind;

class checker {
public:
  checker(const ptr_vec<hir_module> &modules, const semantic::type_table &types)
      : types_(types) {
    for (const auto &module : modules) {
      for (const auto &function : module->functions) {
        functions_.emplace(std::pair{module->module_name, function->name},
                           function.get());
      }
    }
  }

  auto run(const ptr_vec<hir_module> &modules)
      -> std::vector<reference_violation> {
    for (const auto &module : modules) {
      module_ = module->module_name;
      file_id_ = module->file_id;
      for (const auto &function : module->functions) {
        function_ = function.get();
        for (const auto &param : function->params) {
          if (param.name == "self" && !is_ref(param.type) &&
              types_.is_heap_represented(param.type)) {
            report(function->span, std::format("`self` is typed `{}`, not a "
                                               "reference",
                                               types_.display(param.type)));
          }
        }
        symbol_types_.clear();
        return_type_ = function->is_generator ? semantic::k_unknown_type
                                              : function->return_type;
        if (function->body != nullptr) {
          collect_symbol_types(*function->body);
          walk(*function->body);
        }
      }
    }
    return std::move(out_);
  }

private:
  const semantic::type_table &types_;
  std::map<std::pair<std::string, std::string>, const hir_function *>
      functions_;
  std::string module_;
  std::optional<file_id_type> file_id_;
  const hir_function *function_ = nullptr;
  /// What a `return` here must produce: the enclosing function's, or the
  /// innermost lambda's. Unknown for a generator, whose `return` carries no
  /// value of the declared type.
  type_id return_type_ = semantic::k_unknown_type;
  std::vector<reference_violation> out_;
  /// Each local's type, as its references spell it.
  std::map<symbol_id, type_id> symbol_types_;

  auto collect_symbol_types(const hir_node &node) -> void {
    if (node.kind == hir_node_kind::hir_local_ref) {
      const auto &ref = dynamic_cast<const hir_local_ref &>(node);
      if (!types_.is_unknown(ref.type)) {
        symbol_types_.emplace(ref.symbol, ref.type);
      }
    }
    for_each_child(const_cast<hir_node &>(node), [this](auto &child) -> void {
      collect_symbol_types(*child);
    });
  }

  [[nodiscard]] auto is_ref(type_id type) const -> bool {
    return !types_.is_unknown(type) &&
           types_.entry(type).kind == type_kind::ref_kind;
  }

  /// A reference, or a `cell` view, which is the address of one element.
  [[nodiscard]] auto is_ref_like(type_id type) const -> bool {
    if (is_ref(type)) {
      return true;
    }
    if (types_.is_unknown(type)) {
      return false;
    }
    const auto &entry = types_.entry(type);
    return entry.kind == type_kind::builtin_generic_kind &&
           (entry.name == "cell" || entry.name == "cell_mut");
  }

  auto report(source_span span, std::string what) -> void {
    out_.push_back(reference_violation{
        .module = module_,
        .function = function_ != nullptr ? function_->name : std::string{},
        .file_id = file_id_,
        .span = span,
        .what = std::move(what)});
  }

  auto projection(const hir_expr &object, std::string_view what) -> void {
    if (is_ref(object.type)) {
      report(object.span, std::format("{} through `{}` with no explicit deref",
                                      what, types_.display(object.type)));
    }
  }

  /// A value flowing into a destination of type `want`.
  auto flows(const hir_expr &value, type_id want, std::string_view where)
      -> void {
    if (types_.is_unknown(want) || types_.is_unknown(value.type)) {
      return;
    }
    if (is_ref_like(want) != is_ref_like(value.type)) {
      report(value.span,
             std::format("{}: `{}` flows where `{}` is expected", where,
                         types_.display(value.type), types_.display(want)));
    }
  }

  [[nodiscard]] auto callee_of(const hir_call &call) const
      -> const hir_function * {
    if (call.callee == nullptr ||
        call.callee->kind != hir_node_kind::hir_local_ref) {
      return nullptr;
    }
    const auto &ref = dynamic_cast<const hir_local_ref &>(*call.callee);
    const auto found = functions_.find(
        std::pair{ref.owner_module.value_or(module_), ref.name});
    return found != functions_.end() ? found->second : nullptr;
  }

  auto walk(const hir_node &node) -> void {
    switch (node.kind) {
    case hir_node_kind::hir_field:
      projection(*dynamic_cast<const hir_field &>(node).object, "field access");
      break;
    case hir_node_kind::hir_tuple_index:
      projection(*dynamic_cast<const hir_tuple_index &>(node).object,
                 "tuple projection");
      break;
    case hir_node_kind::hir_variant_payload:
      projection(*dynamic_cast<const hir_variant_payload &>(node).object,
                 "payload projection");
      break;
    case hir_node_kind::hir_index:
      projection(*dynamic_cast<const hir_index &>(node).object, "indexing");
      break;
    case hir_node_kind::hir_container_data:
      projection(*dynamic_cast<const hir_container_data &>(node).object,
                 "a container's data pointer");
      break;
    case hir_node_kind::hir_container_len:
      projection(*dynamic_cast<const hir_container_len &>(node).object,
                 "a container's length");
      break;
    case hir_node_kind::hir_generator_next:
      projection(*dynamic_cast<const hir_generator_next &>(node).object,
                 "a generator step");
      break;
    case hir_node_kind::hir_generator_cancel:
      projection(*dynamic_cast<const hir_generator_cancel &>(node).object,
                 "a generator drop");
      break;
    case hir_node_kind::hir_closure_drop:
      projection(*dynamic_cast<const hir_closure_drop &>(node).object,
                 "a closure drop");
      break;
    case hir_node_kind::hir_str_decode_scalar:
      projection(*dynamic_cast<const hir_str_decode_scalar &>(node).object,
                 "a string decode");
      break;
    case hir_node_kind::hir_str_scalar_width:
      projection(*dynamic_cast<const hir_str_scalar_width &>(node).object,
                 "a string decode");
      break;
    case hir_node_kind::hir_cell_set:
      projection(*dynamic_cast<const hir_cell_set &>(node).cell,
                 "a cell write");
      break;
    case hir_node_kind::hir_match:
      projection(*dynamic_cast<const hir_match &>(node).subject,
                 "a match subject");
      break;
    case hir_node_kind::hir_while_let:
      projection(*dynamic_cast<const hir_while_let &>(node).subject,
                 "a `while let` subject");
      break;
    case hir_node_kind::hir_let_else:
      projection(*dynamic_cast<const hir_let_else &>(node).initializer,
                 "a `let ... else` subject");
      break;
    case hir_node_kind::hir_call: {
      const auto &call = dynamic_cast<const hir_call &>(node);
      if (call.callee != nullptr && call.target == nullptr) {
        projection(*call.callee, "a call");
      }
      if (const auto *callee = callee_of(call);
          callee != nullptr && callee->params.size() == call.args.size()) {
        for (std::size_t i = 0; i < call.args.size(); ++i) {
          if (call.args[i] != nullptr) {
            flows(*call.args[i], callee->params[i].type,
                  std::format("argument {} of `{}`", i, callee->name));
          }
        }
      }
      break;
    }
    case hir_node_kind::hir_return: {
      const auto &ret = dynamic_cast<const hir_return &>(node);
      if (ret.value != nullptr && function_ != nullptr) {
        flows(*ret.value, return_type_, "returned value");
      }
      break;
    }
    case hir_node_kind::hir_lambda: {
      // A `return` in a lambda body leaves the lambda, so it answers to the
      // lambda's result type, not the enclosing function's.
      const auto saved = std::exchange(
          return_type_, dynamic_cast<const hir_lambda &>(node).return_type);
      for_each_child(const_cast<hir_node &>(node),
                     [this](auto &child) -> void { walk(*child); });
      return_type_ = saved;
      return;
    }
    case hir_node_kind::hir_let: {
      const auto &let = dynamic_cast<const hir_let &>(node);
      if (const auto found = symbol_types_.find(let.symbol);
          found != symbol_types_.end() && let.initializer != nullptr) {
        flows(*let.initializer, found->second,
              std::format("initializer of `{}`", let.name));
      }
      break;
    }
    case hir_node_kind::hir_assign: {
      const auto &assign = dynamic_cast<const hir_assign &>(node);
      if (assign.target != nullptr && assign.value != nullptr) {
        flows(*assign.value, assign.target->type, "assigned value");
      }
      break;
    }
    default:
      break;
    }
    // The traversal helper hands out mutable slots; nothing here writes.
    for_each_child(const_cast<hir_node &>(node),
                   [this](auto &child) -> void { walk(*child); });
  }
};

/// Whether `type` is a type parameter itself, so whether it is a reference
/// is not known until an instance says what it is.
auto is_bare_param(const semantic::type_table &types, type_id type) -> bool {
  const auto kind = types.entry(type).kind;
  return kind == type_kind::type_param_kind ||
         kind == type_kind::param_app_kind;
}

[[nodiscard]] auto is_place(const hir_expr &expr) -> bool {
  switch (expr.kind) {
  case hir_node_kind::hir_local_ref:
  case hir_node_kind::hir_field:
  case hir_node_kind::hir_index:
  case hir_node_kind::hir_tuple_index:
  case hir_node_kind::hir_variant_payload:
    return true;
  case hir_node_kind::hir_unary:
    return dynamic_cast<const hir_unary &>(expr).op == ast::unary_op::deref;
  default:
    return false;
  }
}

[[nodiscard]] auto is_jump(const hir_node &node) -> bool {
  return node.kind == hir_node_kind::hir_return ||
         node.kind == hir_node_kind::hir_break ||
         node.kind == hir_node_kind::hir_continue;
}

class rewriter {
public:
  rewriter(const semantic::checked_types &checked,
           const std::function<symbol_id()> &mint,
           const drop_temporary_fn &drop_temporary,
           const temporary_end_fn &end_of)
      : checked_(checked), types_(checked.types), mint_(mint),
        drop_temporary_(drop_temporary), end_of_(end_of) {}

  auto run(hir_function &function) -> void {
    returns_.push_back(function.is_generator ? semantic::k_unknown_type
                                             : function.return_type);
    if (function.body != nullptr) {
      visit(*function.body);
      remove_empty_exits(*function.body);
    }
    returns_.pop_back();
  }

private:
  const semantic::checked_types &checked_;
  const semantic::type_table &types_;
  const std::function<symbol_id()> &mint_;
  const drop_temporary_fn &drop_temporary_;
  const temporary_end_fn &end_of_;
  std::vector<type_id> returns_;
  /// The temporaries made in one enclosing statement or full expression.
  struct frame {
    std::vector<temporary> temps;
    /// Every temporary gets a live flag (a `while let` subject's).
    bool flag_all = false;
    /// The AST node of the statement or full expression the frame is, which
    /// the ownership checker names as a temporary's end. Null for a frame
    /// with no source construct.
    const void *origin = nullptr;
    /// A frame whose temporaries are never dropped: the receiver a call
    /// consumes.
    bool discard = false;
  };
  /// The enclosing frames, innermost last. Each frame's temporaries are
  /// dropped when it closes.
  std::vector<frame> frames_;

  /// A jump that leaves frames: it drops their temporaries, the innermost
  /// frame's first, under their live flags.
  struct exit_point {
    /// The placeholder lowering left before the jump, or null for a
    /// `return` with a value and no placeholder, whose value carries the
    /// drops instead (`{let r = value; drops; r}`).
    hir_block *placeholder = nullptr;
    hir_return *ret = nullptr;
    /// The first frame the jump leaves: 0 for a `return`, the innermost
    /// loop's for a `break` or `continue`.
    std::size_t boundary = 0;
    /// How many frames enclose it that have not closed yet.
    std::size_t depth = 0;
    ptr_vec<hir_node> drops;
  };
  std::vector<exit_point> exits_;
  /// The frame count at each enclosing loop's body, innermost last.
  std::vector<std::size_t> loop_bases_;
  /// Set by a placeholder until the jump it comes before is visited.
  bool placeholder_pending_ = false;
  /// Statements to insert after the one being visited: the drops of a
  /// `while let` subject's temporaries left alive when the loop ends.
  ptr_vec<hir_node> after_statement_;

  [[nodiscard]] auto needs_drop(type_id type) const -> bool {
    return drop_temporary_ != nullptr && !types_.is_unknown(type) &&
           checked_.drop_plans.contains(type);
  }

  /// Notes `symbol`, a local holding the value of `value` (a temporary), to
  /// be dropped when the frame the ownership checker ends it with closes.
  auto note_temporary(symbol_id symbol, type_id type, hir_block *creation,
                      const hir_expr &value) -> void {
    if (!needs_drop(type) || frames_.empty()) {
      return;
    }
    frames_[end_frame(value)].temps.push_back(temporary{.symbol = symbol,
                                                        .type = type,
                                                        .moved_paths = {},
                                                        .creation = creation});
  }

  /// The frame a temporary holding `value` is dropped with: the one the
  /// ownership checker ends it with. A value lowering synthesized has no
  /// record and ends with the innermost frame.
  [[nodiscard]] auto end_frame(const hir_expr &value) const -> std::size_t {
    const auto innermost = frames_.size() - 1;
    if (frames_.back().discard || value.origin == nullptr ||
        end_of_ == nullptr) {
      return innermost;
    }
    if (const auto end = end_of_(value.origin); end.has_value()) {
      for (auto i = frames_.size(); i > 0; --i) {
        if (frames_[i - 1].origin == *end) {
          return i - 1;
        }
      }
    }
    return innermost;
  }

  [[nodiscard]] auto find_temporary(symbol_id symbol) -> temporary * {
    for (auto &frame : frames_) {
      for (auto &temp : frame.temps) {
        if (temp.symbol == symbol) {
          return &temp;
        }
      }
    }
    return nullptr;
  }

  /// The temporary a place made of fields of `*{let t = ...; &t}` is part
  /// of, with the field names down to `expr`.
  auto temporary_root(const hir_expr &expr, std::vector<std::string> &path)
      -> temporary * {
    if (expr.kind == hir_node_kind::hir_field) {
      const auto &field = dynamic_cast<const hir_field &>(expr);
      auto *temp = field.object != nullptr ? temporary_root(*field.object, path)
                                           : nullptr;
      if (temp != nullptr) {
        path.push_back(field.field_name);
      }
      return temp;
    }
    if (expr.kind != hir_node_kind::hir_unary) {
      return nullptr;
    }
    const auto &deref = dynamic_cast<const hir_unary &>(expr);
    if (deref.op != ast::unary_op::deref || deref.operand == nullptr ||
        deref.operand->kind != hir_node_kind::hir_block) {
      return nullptr;
    }
    const auto &block = dynamic_cast<const hir_block &>(*deref.operand);
    if (block.stmts.empty() ||
        block.stmts.front()->kind != hir_node_kind::hir_let) {
      return nullptr;
    }
    return find_temporary(
        dynamic_cast<const hir_let &>(*block.stmts.front()).symbol);
  }

  /// The drop of one temporary, its receiver made a reference like any
  /// other call's.
  auto drop_one(const temporary &temp) -> ptr_vec<hir_node> {
    auto drops = ptr_vec<hir_node>{};
    drop_temporary_(temp, drops);
    frames_.emplace_back();
    for (auto &drop : drops) {
      visit(*drop);
    }
    frames_.pop_back();
    return drops;
  }

  [[nodiscard]] auto bool_literal(source_span span, bool value) const
      -> ptr<hir_expr> {
    return ptr<hir_expr>(hir::make<hir_literal>(span, types_.bool_type(),
                                                value ? token_kind::kw_true
                                                      : token_kind::kw_false,
                                                value ? "true" : "false"));
  }

  [[nodiscard]] auto live_ref(const temporary &temp, source_span span) const
      -> ptr<hir_expr> {
    return ptr<hir_expr>(hir::make<hir_local_ref>(
        span, types_.bool_type(), temp.live, std::string("<live>")));
  }

  /// Appends the drops of `temps` to `out`, last made first: under its live
  /// flag, which it clears, for a temporary that has one.
  auto drop_all(std::vector<temporary> &temps, ptr_vec<hir_node> &out) -> void {
    for (auto &temp : std::views::reverse(temps)) {
      auto drops = drop_one(temp);
      if (temp.live == k_invalid_symbol_id) {
        for (auto &drop : drops) {
          out.push_back(std::move(drop));
        }
        continue;
      }
      out.push_back(guarded(temp, std::move(drops)));
    }
  }

  /// `if live: live = false; drops`.
  auto guarded(const temporary &temp, ptr_vec<hir_node> drops)
      -> ptr<hir_node> {
    const auto span = source_span::dummy();
    auto body = ptr_vec<hir_node>{};
    body.push_back(ptr<hir_node>(hir::make<hir_assign>(
        span, ast::assign_op::assign, live_ref(temp, span),
        bool_literal(span, false))));
    for (auto &drop : drops) {
      body.push_back(std::move(drop));
    }
    auto branches = std::vector<hir_if_branch>{};
    branches.push_back(
        hir_if_branch{.condition = live_ref(temp, span),
                      .body = hir::make<hir_block>(
                          span, semantic::k_unknown_type, std::move(body))});
    return ptr<hir_node>(hir::make<hir_expr_stmt>(
        span, ptr<hir_expr>(hir::make<hir_if>(span, semantic::k_unknown_type,
                                              std::move(branches), nullptr))));
  }

  /// Gives `temp` a live flag: declared false in `lets`, which run before
  /// the frame, and set right after the temporary is stored.
  auto give_flag(temporary &temp, ptr_vec<hir_node> &lets) -> void {
    if (temp.creation == nullptr || temp.creation->stmts.empty()) {
      return;
    }
    const auto span = source_span::dummy();
    temp.live = mint_();
    lets.push_back(ptr<hir_node>(hir::make<hir_let>(
        span, temp.live, std::string("<live>"), bool_literal(span, false),
        /*mut=*/true)));
    auto &stmts = temp.creation->stmts;
    stmts.insert(stmts.begin() + 1,
                 ptr<hir_node>(hir::make<hir_assign>(
                     span, ast::assign_op::assign, live_ref(temp, span),
                     bool_literal(span, true))));
  }

  /// A closed frame's temporaries, and the declarations of their live flags,
  /// which must run before the frame does.
  struct closed_frame {
    std::vector<temporary> temps;
    ptr_vec<hir_node> flag_lets;
  };

  /// Closes the innermost frame. Every jump inside it that leaves it drops
  /// its temporaries, which then get live flags; a jump that leaves no
  /// further frames gets its drops in place.
  auto close_frame() -> closed_frame {
    auto closing = std::move(frames_.back());
    frames_.pop_back();
    const auto index = frames_.size();
    auto out = closed_frame{.temps = std::move(closing.temps), .flag_lets = {}};
    const auto leaves = [index](const exit_point &e) -> bool {
      return e.depth > index && e.boundary <= index;
    };
    if (!out.temps.empty() &&
        (closing.flag_all || std::ranges::any_of(exits_, leaves))) {
      for (auto &temp : out.temps) {
        give_flag(temp, out.flag_lets);
      }
      for (auto &e : exits_) {
        if (leaves(e)) {
          drop_all(out.temps, e.drops);
        }
      }
    }
    for (auto &e : exits_) {
      e.depth = std::min(e.depth, index);
    }
    // A jump whose last frame closed has all of its drops.
    for (auto &e : exits_) {
      if (e.boundary >= index && e.depth <= index) {
        place_exit(e);
      }
    }
    std::erase_if(exits_, [index](const exit_point &e) {
      return e.boundary >= index && e.depth <= index;
    });
    return out;
  }

  /// Puts an exit's drops where its jump runs them.
  auto place_exit(exit_point &e) -> void {
    if (e.drops.empty()) {
      return;
    }
    if (e.placeholder != nullptr) {
      for (auto &drop : e.drops) {
        e.placeholder->stmts.push_back(std::move(drop));
      }
      return;
    }
    if (e.ret == nullptr || e.ret->value == nullptr) {
      return;
    }
    auto &slot = e.ret->value;
    const auto span = slot->span;
    const auto type = slot->type;
    const auto symbol = mint_();
    auto stmts = ptr_vec<hir_node>{};
    stmts.push_back(ptr<hir_node>(hir::make<hir_let>(
        span, symbol, std::string("<returned>"), std::move(slot))));
    for (auto &drop : e.drops) {
      stmts.push_back(std::move(drop));
    }
    stmts.push_back(ptr<hir_node>(hir::make<hir_expr_stmt>(
        span, ptr<hir_expr>(hir::make<hir_local_ref>(
                  span, type, symbol, std::string("<returned>"))))));
    slot = ptr<hir_expr>(hir::make<hir_block>(span, type, std::move(stmts)));
  }

  /// Registers a jump: one with a placeholder, or a `return` whose value
  /// carries its drops.
  auto note_jump(hir_block *placeholder, hir_return *ret, jump_exit kind)
      -> void {
    const auto boundary = kind == jump_exit::loop && !loop_bases_.empty()
                              ? loop_bases_.back()
                              : std::size_t{0};
    exits_.push_back(exit_point{.placeholder = placeholder,
                                .ret = ret,
                                .boundary = boundary,
                                .depth = frames_.size(),
                                .drops = {}});
  }

  /// Removes the placeholders no temporary needed.
  static auto remove_empty_exits(hir_node &node) -> void {
    if (node.kind == hir_node_kind::hir_block) {
      std::erase_if(
          dynamic_cast<hir_block &>(node).stmts,
          [](const ptr<hir_node> &stmt) -> bool {
            if (stmt->kind != hir_node_kind::hir_expr_stmt) {
              return false;
            }
            const auto &expr = dynamic_cast<const hir_expr_stmt &>(*stmt).expr;
            if (expr == nullptr || expr->kind != hir_node_kind::hir_block) {
              return false;
            }
            const auto &block = dynamic_cast<const hir_block &>(*expr);
            return block.exit != jump_exit::none && block.stmts.empty();
          });
    }
    for_each_child(node,
                   [](auto &child) -> void { remove_empty_exits(*child); });
  }

  /// Evaluates `slot` as a full expression: the temporaries made inside it
  /// are dropped right after it, before its value is used.
  auto full_expression(ptr<hir_expr> &slot) -> void {
    if (slot == nullptr) {
      return;
    }
    frames_.push_back(frame{.temps = {},
                            .flag_all = false,
                            .origin = slot->origin,
                            .discard = false});
    visit(*slot);
    close_into(slot);
  }

  /// Closes the innermost frame around `slot`: `{let r = slot; drops; r}`.
  auto close_into(ptr<hir_expr> &slot) -> void {
    auto closed = close_frame();
    auto &temps = closed.temps;
    if (temps.empty()) {
      return;
    }
    const auto span = slot->span;
    const auto type = slot->type;
    auto stmts = std::move(closed.flag_lets);
    if (!types_.is_unknown(type) && !types_.is_unit(type)) {
      const auto symbol = mint_();
      stmts.push_back(ptr<hir_node>(hir::make<hir_let>(
          span, symbol, std::string("<full expression>"), std::move(slot))));
      drop_all(temps, stmts);
      stmts.push_back(ptr<hir_node>(hir::make<hir_expr_stmt>(
          span, ptr<hir_expr>(hir::make<hir_local_ref>(
                    span, type, symbol, std::string("<full expression>"))))));
    } else {
      stmts.push_back(
          ptr<hir_node>(hir::make<hir_expr_stmt>(span, std::move(slot))));
      drop_all(temps, stmts);
    }
    slot = ptr<hir_expr>(hir::make<hir_block>(span, type, std::move(stmts)));
  }

  /// A block's statements, one source statement at a time: the
  /// temporaries a statement makes are dropped after it, or before it when
  /// it leaves the block (a `return`, `break` or `continue`, whose own value
  /// is a full expression) or is the block's value. A value computed and
  /// discarded is itself a temporary.
  auto visit_block(hir_block &block, bool statements_only) -> void {
    auto &stmts = block.stmts;
    auto out = ptr_vec<hir_node>{};
    std::size_t i = 0;
    while (i < stmts.size()) {
      auto end = i + 1;
      while (end < stmts.size() && stmts[end]->continues_statement) {
        ++end;
      }
      frames_.push_back(frame{.temps = {},
                              .flag_all = false,
                              .origin = stmts[i]->origin,
                              .discard = false});
      auto value_tail = false;
      auto after = std::vector<ptr_vec<hir_node>>(end - i);
      auto before = ptr_vec<hir_node>(end - i);
      for (auto k = i; k < end; ++k) {
        auto &stmt = stmts[k];
        const auto last = k + 1 == stmts.size();
        if (stmt->kind == hir_node_kind::hir_expr_stmt) {
          auto &expr = dynamic_cast<hir_expr_stmt &>(*stmt).expr;
          const auto has_value = expr != nullptr &&
                                 !types_.is_unknown(expr->type) &&
                                 !types_.is_unit(expr->type);
          if (last && has_value &&
              !(statements_only || types_.is_unit(block.type))) {
            // The block's value: its temporaries end with it.
            full_expression(expr);
            value_tail = true;
            continue;
          }
          visit(*stmt);
          if (expr != nullptr && has_value && !is_place(*expr) &&
              needs_drop(expr->type)) {
            const auto symbol = mint_();
            const auto type = expr->type;
            const auto span = stmt->span;
            const auto &value = *expr;
            auto store = ptr_vec<hir_node>{};
            store.push_back(ptr<hir_node>(hir::make<hir_let>(
                span, symbol, std::string("<discarded>"), std::move(expr))));
            auto block = hir::make<hir_block>(span, semantic::k_unknown_type,
                                              std::move(store));
            auto *creation = block.get();
            stmt = ptr<hir_node>(hir::make<hir_expr_stmt>(
                span, ptr<hir_expr>(std::move(block))));
            note_temporary(symbol, type, creation, value);
          }
          after[k - i] = std::move(after_statement_);
          after_statement_.clear();
          continue;
        }
        if (!placeholder_pending_ &&
            (stmt->kind == hir_node_kind::hir_break ||
             stmt->kind == hir_node_kind::hir_continue ||
             (stmt->kind == hir_node_kind::hir_return &&
              dynamic_cast<const hir_return &>(*stmt).value == nullptr))) {
          // A jump lowering made without a placeholder of its own.
          auto placeholder = hir::make<hir_block>(
              stmt->span, semantic::k_unknown_type, ptr_vec<hir_node>{});
          placeholder->exit = stmt->kind == hir_node_kind::hir_return
                                  ? jump_exit::function
                                  : jump_exit::loop;
          note_jump(placeholder.get(), nullptr, placeholder->exit);
          before[k - i] = ptr<hir_node>(hir::make<hir_expr_stmt>(
              stmt->span, ptr<hir_expr>(std::move(placeholder))));
        }
        visit(*stmt);
        after[k - i] = std::move(after_statement_);
        after_statement_.clear();
      }
      auto closed = close_frame();
      auto &temps = closed.temps;
      for (auto &let : closed.flag_lets) {
        out.push_back(std::move(let));
      }
      // A statement that ends in a jump never reaches its end: the jump
      // drops its temporaries.
      const auto jumps = is_jump(*stmts[end - 1]);
      const auto before_last = !temps.empty() && value_tail;
      for (auto k = i; k < end; ++k) {
        if (before_last && k + 1 == end) {
          drop_all(temps, out);
        }
        if (before[k - i] != nullptr) {
          out.push_back(std::move(before[k - i]));
        }
        out.push_back(std::move(stmts[k]));
        for (auto &extra : after[k - i]) {
          out.push_back(std::move(extra));
        }
      }
      if (!temps.empty() && !before_last && !jumps) {
        drop_all(temps, out);
      }
      i = end;
    }
    stmts = std::move(out);
  }

  [[nodiscard]] auto is_ref(type_id type) const -> bool {
    return !types_.is_unknown(type) &&
           types_.entry(type).kind == type_kind::ref_kind;
  }

  [[nodiscard]] auto is_cell(type_id type) const -> bool {
    if (types_.is_unknown(type)) {
      return false;
    }
    const auto &entry = types_.entry(type);
    return entry.kind == type_kind::builtin_generic_kind &&
           (entry.name == "cell" || entry.name == "cell_mut");
  }

  /// `expr` read through every reference it is: `*expr`, `**expr`, ...
  auto deref(ptr<hir_expr> expr) -> ptr<hir_expr> {
    while (expr != nullptr && is_ref(expr->type)) {
      const auto referent = types_.entry(expr->type).result;
      const auto span = expr->span;
      expr = ptr<hir_expr>(hir::make<hir_unary>(
          span, referent, ast::unary_op::deref, std::move(expr)));
    }
    return expr;
  }

  /// `&expr` typed `want`; a value that is not a place is stored first.
  auto borrow(ptr<hir_expr> expr, type_id want) -> ptr<hir_expr> {
    const auto span = expr->span;
    const auto op = types_.entry(want).is_mut ? ast::unary_op::addr_of_mut
                                              : ast::unary_op::addr_of;
    if (is_place(*expr)) {
      // A part of a temporary that is borrowed stays in it.
      auto path = std::vector<std::string>{};
      if (auto *temp = temporary_root(*expr, path); temp != nullptr) {
        temp->moved_paths.clear();
      }
      return ptr<hir_expr>(
          hir::make<hir_unary>(span, want, op, std::move(expr)));
    }
    const auto symbol = mint_();
    const auto value_type = expr->type;
    const auto &value = *expr;
    auto stmts = ptr_vec<hir_node>{};
    stmts.push_back(ptr<hir_node>(hir::make<hir_let>(
        span, symbol, std::string("<borrowed>"), std::move(expr))));
    auto place = ptr<hir_expr>(hir::make<hir_local_ref>(
        span, value_type, symbol, std::string("<borrowed>")));
    stmts.push_back(ptr<hir_node>(hir::make<hir_expr_stmt>(
        span, ptr<hir_expr>(
                  hir::make<hir_unary>(span, want, op, std::move(place))))));
    auto block = hir::make<hir_block>(span, want, std::move(stmts));
    note_temporary(symbol, value_type, block.get(), value);
    return ptr<hir_expr>(std::move(block));
  }

  /// `slot` made to agree with a destination of type `want` on being a
  /// reference.
  auto coerce(ptr<hir_expr> &slot, type_id want) -> void {
    // Whether a value is a reference is its type's head; only a bare type
    // parameter leaves that open.
    if (slot == nullptr || types_.is_unknown(want) ||
        types_.is_unknown(slot->type) || is_bare_param(types_, want) ||
        is_bare_param(types_, slot->type)) {
      return;
    }
    // A `cell[T]` is the address of one element — what `&xs[i]` lowers to
    // through `index_ref` — so it already is the reference a `&T` wants.
    const auto want_ref = is_ref(want) || is_cell(want);
    const auto have_ref = is_ref(slot->type) || is_cell(slot->type);
    if (want_ref && !have_ref) {
      slot = borrow(std::move(slot), want);
    } else if (!want_ref && have_ref) {
      slot = deref(std::move(slot));
    }
  }

  /// Dereferences a pattern subject that is a reference, and retypes the
  /// subject symbol's uses (and the patterns' subject types) inside `node`.
  auto match_through(hir_node &node, ptr<hir_expr> &subject, symbol_id symbol)
      -> void {
    if (subject == nullptr || !is_ref(subject->type)) {
      return;
    }
    const auto old_type = subject->type;
    subject = deref(std::move(subject));
    retype(node, symbol, old_type, subject->type, subject.get());
  }

  auto retype(hir_node &node, symbol_id symbol, type_id from, type_id to,
              const hir_expr *skip) -> void {
    if (&node == skip) {
      return;
    }
    if (node.kind == hir_node_kind::hir_local_ref) {
      auto &ref = dynamic_cast<hir_local_ref &>(node);
      if (ref.symbol == symbol && ref.type == from) {
        ref.type = to;
      }
    }
    if (auto *pattern = dynamic_cast<hir_pattern *>(&node);
        pattern != nullptr && pattern->subject_type == from) {
      pattern->subject_type = to;
    }
    for_each_child(node, [&](auto &child) -> void {
      retype(*child, symbol, from, to, skip);
    });
  }

  /// Stores `slot`, a value that is not a place and must be dropped, in a
  /// temporary, and reads it in place: `*{let t = slot; &t}`.
  auto read_through_temporary(ptr<hir_expr> &slot) -> void {
    if (slot == nullptr || is_place(*slot) || !needs_drop(slot->type) ||
        frames_.empty()) {
      return;
    }
    const auto type = slot->type;
    const auto ref = const_cast<semantic::type_table &>(types_).ref_to(
        type, /*is_mut=*/false);
    slot = deref(borrow(std::move(slot), ref));
  }

  auto project(ptr<hir_expr> &object) -> void {
    if (object != nullptr && is_ref(object->type)) {
      object = deref(std::move(object));
    }
  }

  [[nodiscard]] auto param_types(const hir_call &call) const
      -> std::vector<type_id> {
    auto out = std::vector<type_id>{};
    if (call.target != nullptr) {
      for (const auto &param : call.target->params) {
        const auto found = checked_.node_types.find(param.pattern.get());
        auto type = found != checked_.node_types.end()
                        ? found->second
                        : semantic::k_unknown_type;
        // A runtime intrinsic receives a heap value's own pointer where its
        // signature says `&T`: the runtime reads and writes through the
        // block, never the caller's slot.
        if (call.target->modifiers.is_intrinsic && is_ref(type) &&
            types_.is_heap_represented(types_.entry(type).result)) {
          type = types_.entry(type).result;
        }
        out.push_back(type);
      }
      return out;
    }
    if (call.callee == nullptr || types_.is_unknown(call.callee->type)) {
      return out;
    }
    auto callee_type = call.callee->type;
    while (is_ref(callee_type)) {
      callee_type = types_.entry(callee_type).result;
    }
    const auto &entry = types_.entry(callee_type);
    if (entry.kind == type_kind::fn_kind) {
      out.assign(entry.args.begin(), entry.args.end());
    }
    return out;
  }

  auto visit(hir_node &node) -> void {
    if (node.kind == hir_node_kind::hir_lambda) {
      auto &lambda = dynamic_cast<hir_lambda &>(node);
      returns_.push_back(lambda.return_type);
      // A `return` in the lambda leaves the lambda, not the frames and
      // loops around it.
      auto outer = std::move(frames_);
      auto outer_exits = std::move(exits_);
      auto outer_loops = std::move(loop_bases_);
      frames_.clear();
      exits_.clear();
      loop_bases_.clear();
      if (lambda.body != nullptr) {
        visit(*lambda.body);
      }
      frames_ = std::move(outer);
      exits_ = std::move(outer_exits);
      loop_bases_ = std::move(outer_loops);
      returns_.pop_back();
      if (lambda.drop_glue != nullptr) {
        visit(*lambda.drop_glue);
      }
      return;
    }
    switch (node.kind) {
    case hir_node_kind::hir_block: {
      auto &block = dynamic_cast<hir_block &>(node);
      if (block.exit != jump_exit::none) {
        note_jump(&block, nullptr, block.exit);
        placeholder_pending_ = true;
        return;
      }
      visit_block(block, false);
      return;
    }
    case hir_node_kind::hir_break:
    case hir_node_kind::hir_continue:
      placeholder_pending_ = false;
      return;
    case hir_node_kind::hir_if: {
      // A condition is a full expression: what it makes is dropped before
      // a branch runs.
      auto &branch_if = dynamic_cast<hir_if &>(node);
      for (auto &branch : branch_if.branches) {
        full_expression(branch.condition);
        if (branch.body != nullptr) {
          visit(*branch.body);
        }
      }
      if (branch_if.else_body != nullptr) {
        visit(*branch_if.else_body);
      }
      return;
    }
    case hir_node_kind::hir_while: {
      auto &loop = dynamic_cast<hir_while &>(node);
      full_expression(loop.condition);
      loop_bases_.push_back(frames_.size());
      if (loop.body != nullptr) {
        visit_block(*loop.body, true);
      }
      if (loop.step != nullptr) {
        visit_block(*loop.step, true);
      }
      loop_bases_.pop_back();
      return;
    }
    case hir_node_kind::hir_binary: {
      auto &binary = dynamic_cast<hir_binary &>(node);
      if (binary.op == ast::binary_op::logical_and ||
          binary.op == ast::binary_op::logical_or) {
        // The right operand runs only sometimes.
        if (binary.lhs != nullptr) {
          visit(*binary.lhs);
        }
        full_expression(binary.rhs);
        return;
      }
      break;
    }
    case hir_node_kind::hir_return:
    case hir_node_kind::hir_yield: {
      auto &value = node.kind == hir_node_kind::hir_return
                        ? dynamic_cast<hir_return &>(node).value
                        : dynamic_cast<hir_yield &>(node).value;
      frames_.push_back(
          frame{.temps = {},
                .flag_all = false,
                .origin = value != nullptr ? value->origin : nullptr,
                .discard = false});
      if (value != nullptr) {
        visit(*value);
        if (node.kind == hir_node_kind::hir_return && !returns_.empty()) {
          coerce(value, returns_.back());
        }
        close_into(value);
      } else {
        frames_.pop_back();
      }
      if (node.kind == hir_node_kind::hir_return) {
        // Without a placeholder before it, the returned value carries the
        // drops of what the `return` leaves alive.
        if (!placeholder_pending_ && value != nullptr) {
          note_jump(nullptr, &dynamic_cast<hir_return &>(node),
                    jump_exit::function);
        }
        placeholder_pending_ = false;
      }
      return;
    }
    default:
      break;
    }
    // A `match`/`while let`/`let ... else` on a reference matches what it
    // refers to: the subject is dereferenced once, and every use of the
    // subject's own symbol (the patterns' projections) sees the value.
    switch (node.kind) {
    case hir_node_kind::hir_match: {
      auto &match = dynamic_cast<hir_match &>(node);
      match_through(node, match.subject, match.subject_symbol);
      if (match.subject != nullptr) {
        visit(*match.subject);
      }
      for (auto &arm : match.arms) {
        if (arm.pattern != nullptr) {
          visit(*arm.pattern);
        }
        full_expression(arm.guard);
        if (arm.body != nullptr) {
          visit(*arm.body);
        }
      }
      return;
    }
    case hir_node_kind::hir_while_let: {
      auto &loop = dynamic_cast<hir_while_let &>(node);
      const auto *subject_origin =
          loop.subject != nullptr ? loop.subject->origin : nullptr;
      match_through(node, loop.subject, loop.subject_symbol);
      // The subject's temporaries last one iteration: they are dropped
      // after the body, when the pattern fails, and by a jump out of the
      // body, each under its live flag.
      loop_bases_.push_back(frames_.size());
      frames_.push_back(frame{.temps = {},
                              .flag_all = true,
                              .origin = subject_origin,
                              .discard = false});
      if (loop.subject != nullptr) {
        visit(*loop.subject);
      }
      if (loop.pattern != nullptr) {
        visit(*loop.pattern);
      }
      if (loop.body != nullptr) {
        visit_block(*loop.body, true);
      }
      auto closed = close_frame();
      loop_bases_.pop_back();
      if (!closed.temps.empty() && loop.subject != nullptr) {
        const auto span = loop.subject->span;
        const auto type = loop.subject->type;
        auto stmts = std::move(closed.flag_lets);
        stmts.push_back(ptr<hir_node>(
            hir::make<hir_expr_stmt>(span, std::move(loop.subject))));
        loop.subject =
            ptr<hir_expr>(hir::make<hir_block>(span, type, std::move(stmts)));
        if (loop.body != nullptr) {
          drop_all(closed.temps, loop.body->stmts);
        }
        drop_all(closed.temps, after_statement_);
      }
      return;
    }
    case hir_node_kind::hir_let_else: {
      auto &let = dynamic_cast<hir_let_else &>(node);
      match_through(node, let.initializer, let.subject_symbol);
      break;
    }
    default:
      break;
    }
    for_each_child(node, [this](auto &child) -> void { visit(*child); });
    switch (node.kind) {
    case hir_node_kind::hir_field: {
      auto &field = dynamic_cast<hir_field &>(node);
      project(field.object);
      read_through_temporary(field.object);
      // A field of a temporary that is not `copy`, used by value, is moved
      // out of it; the temporary drops only the rest. A borrow of it
      // (`borrow`) puts it back.
      auto path = std::vector<std::string>{};
      if (auto *temp = temporary_root(field, path); temp != nullptr) {
        temp->moved_paths.clear();
        if (!checked_.is_copy(field.type)) {
          temp->moved_paths.push_back(std::move(path));
        }
      }
      return;
    }
    case hir_node_kind::hir_unary: {
      auto &unary = dynamic_cast<hir_unary &>(node);
      if ((unary.op == ast::unary_op::addr_of ||
           unary.op == ast::unary_op::addr_of_mut) &&
          unary.operand != nullptr) {
        auto path = std::vector<std::string>{};
        if (auto *temp = temporary_root(*unary.operand, path);
            temp != nullptr) {
          temp->moved_paths.clear();
        }
        read_through_temporary(unary.operand);
      }
      return;
    }
    case hir_node_kind::hir_container_data:
      project(dynamic_cast<hir_container_data &>(node).object);
      return;
    case hir_node_kind::hir_container_len:
      project(dynamic_cast<hir_container_len &>(node).object);
      return;
    case hir_node_kind::hir_generator_next:
      project(dynamic_cast<hir_generator_next &>(node).object);
      return;
    case hir_node_kind::hir_generator_cancel:
      project(dynamic_cast<hir_generator_cancel &>(node).object);
      return;
    case hir_node_kind::hir_closure_drop:
      project(dynamic_cast<hir_closure_drop &>(node).object);
      return;
    case hir_node_kind::hir_str_decode_scalar:
      project(dynamic_cast<hir_str_decode_scalar &>(node).object);
      return;
    case hir_node_kind::hir_str_scalar_width:
      project(dynamic_cast<hir_str_scalar_width &>(node).object);
      return;
    case hir_node_kind::hir_cell_set:
      project(dynamic_cast<hir_cell_set &>(node).cell);
      return;
    case hir_node_kind::hir_tuple_index:
      project(dynamic_cast<hir_tuple_index &>(node).object);
      return;
    case hir_node_kind::hir_variant_payload:
      project(dynamic_cast<hir_variant_payload &>(node).object);
      return;
    case hir_node_kind::hir_index:
      project(dynamic_cast<hir_index &>(node).object);
      return;
    case hir_node_kind::hir_call: {
      auto &call = dynamic_cast<hir_call &>(node);
      // Calling through a `&fn(...)` calls the function value it refers to.
      if (call.target == nullptr) {
        project(call.callee);
      }
      const auto params = param_types(call);
      if (params.size() == call.args.size()) {
        for (std::size_t i = 0; i < params.size(); ++i) {
          if (i == 0 && call.consumes_receiver) {
            // The callee owns the receiver now; a temporary holding it is
            // not dropped here.
            frames_.push_back(frame{.temps = {},
                                    .flag_all = false,
                                    .origin = nullptr,
                                    .discard = true});
            coerce(call.args[i], params[i]);
            frames_.pop_back();
            continue;
          }
          coerce(call.args[i], params[i]);
        }
      }
      return;
    }
    case hir_node_kind::hir_struct_init:
      for (auto &field : dynamic_cast<hir_struct_init &>(node).fields) {
        coerce(field.value, field.declared);
      }
      return;
    case hir_node_kind::hir_tuple: {
      auto &tuple = dynamic_cast<hir_tuple &>(node);
      if (!types_.is_unknown(tuple.type) &&
          types_.entry(tuple.type).kind == type_kind::tuple_kind) {
        const auto elements = types_.entry(tuple.type).args;
        if (elements.size() == tuple.elements.size()) {
          for (std::size_t i = 0; i < elements.size(); ++i) {
            coerce(tuple.elements[i], elements[i]);
          }
        }
      }
      return;
    }
    case hir_node_kind::hir_array_init: {
      auto &array = dynamic_cast<hir_array_init &>(node);
      if (!types_.is_unknown(array.type)) {
        const auto &entry = types_.entry(array.type);
        const auto element = entry.kind == type_kind::array_kind
                                 ? entry.result
                                 : semantic::k_unknown_type;
        for (auto &value : array.elements) {
          coerce(value, element);
        }
        coerce(array.fill_value, element);
      }
      return;
    }
    case hir_node_kind::hir_variant_init: {
      // `option` and `result` spell their payload types in their own type
      // arguments; a user sum's payloads are not recorded here yet.
      auto &init = dynamic_cast<hir_variant_init &>(node);
      if (types_.is_unknown(init.type) || init.args.size() != 1) {
        return;
      }
      const auto &entry = types_.entry(init.type);
      if (entry.kind != type_kind::builtin_generic_kind) {
        return;
      }
      if (entry.name == "option" && init.variant_name == "some" &&
          !entry.args.empty()) {
        coerce(init.args[0], entry.args[0]);
      } else if (entry.name == "result" && entry.args.size() == 2) {
        coerce(init.args[0],
               init.variant_name == "ok" ? entry.args[0] : entry.args[1]);
      }
      return;
    }
    case hir_node_kind::hir_assign: {
      auto &assign = dynamic_cast<hir_assign &>(node);
      if (assign.target != nullptr) {
        coerce(assign.value, assign.target->type);
      }
      return;
    }
    default:
      return;
    }
  }
};

} // namespace

auto make_references_explicit(hir_function &function,
                              const semantic::checked_types &checked,
                              const std::function<symbol_id()> &mint,
                              const drop_temporary_fn &drop_temporary,
                              const temporary_end_fn &end_of) -> void {
  rewriter(checked, mint, drop_temporary, end_of).run(function);
}

auto find_implicit_references(const ptr_vec<hir_module> &modules,
                              const semantic::type_table &types)
    -> std::vector<reference_violation> {
  return checker(modules, types).run(modules);
}

} // namespace cinder::hir
