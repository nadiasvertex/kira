#include "check.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/intrinsics.h"
#include "src/parser/ast_clone.h"
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
      current[j] =
          std::min({previous[j] + 1, current[j - 1] + 1, substitution});
    }
    std::swap(previous, current);
  }
  return previous[cols - 1];
}

/// Picks the closest name in `candidates` to `name` for a "did you mean"
/// hint, rejecting matches whose edit distance is not meaningfully smaller
/// than the candidate's own length (avoids suggesting unrelated short names).
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
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
  // std::from_chars requires a raw begin/end pointer pair.
  const char *digits_end = digits.data() + digits.size();
  const auto result = std::from_chars(digits.data(), digits_end, value, base);
  if (result.ec != std::errc{} || result.ptr != digits_end) {
    return std::nullopt;
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,bugprone-suspicious-stringview-data-usage)
  return value;
}

/// A variant-constructor expression (`@some(x)`) parses to an ident whose
/// span still covers the leading `@`; that lets us tell it apart from an
/// ordinary identifier of the same spelling.
auto is_variant_ident(const ast::ident_expr &ident) -> bool {
  return ident.span.len() > ident.name.size();
}

/// How a value binding entered scope, used to decide whether it may be
/// reassigned and to word the "declared here" note on an immutability error.
enum class binding_origin : uint8_t {
  let_binding,     ///< `let` — immutable.
  var_binding,     ///< `var` — mutable.
  parameter,       ///< Function or lambda parameter.
  pattern_binding, ///< Bound by a pattern (match arm, `if let`, `for`, ...).
  mut_binding,     ///< A pattern binding written with a leading `mut`
                   ///< (`let mut x`, `mut self`) — mutable like `var`.
  synthetic,       ///< Introduced by the checker itself (e.g. a `crew`/`cancel`
                   ///< name).
};

/// One name bound in the current lexical scope stack, with enough
/// information to type future references and to diagnose illegal mutation.
struct value_binding {
  type_id type = k_unknown_type;
  binding_origin origin = binding_origin::let_binding;
  source_span
      span; ///< Where the binding was introduced, for "declared here" notes.
};

/// A normalized function/method parameter, used by call-argument checking
/// regardless of whether the callee came from a `func_decl`, a trait method,
/// or a `fn(...)`-typed value.
struct fn_param_info {
  std::string name;
  type_id type = k_unknown_type;
  bool has_default = false;
  source_span span;
};

/// One method available on a type, from an inherent/trait `impl` block
/// (`from_trait == nullptr`), a trait's default body reached through an
/// impl that didn't override it, or an `extend` block (`is_extension`).
/// Inherent/impl methods win over trait defaults, which win over
/// extensions — see `find_method`.
struct method_entry {
  const ast::func_decl *decl = nullptr; ///< The method's declaration.
  const module_members *owner =
      nullptr; ///< Module the method should resolve types in.
  const ast::trait_decl *from_trait =
      nullptr; ///< Owning trait, if this is a default method.
  bool is_extension =
      false; ///< Whether this method came from an `extend` block.
};

// ==========================================================================
//  Local parameter-usage inference
//
//  Per spec/kira-reference.md, an unannotated parameter gets "the most
//  general type the body allows" — `def double(x): return x * 2` must stay
//  callable with every numeric type, not collapse to whichever type a
//  literal happens to default to. So a bare literal is deliberately never
//  treated as a concrete anchor here: `infer_arithmetic` already treats an
//  unknown/type-parameter operand as fully permissive with no diagnostic
//  (see `check.cpp`'s `infer_arithmetic`), which is exactly the generic
//  behavior the reference describes, so leaving such a parameter at
//  `k_unknown_type` is correct, not merely a conservative fallback.
//
//  A parameter is pinned to one concrete type only when the body leaves no
//  other possibility: unifying with an already-concretely-typed sibling
//  parameter (Kira never converts numbers implicitly, so `x + y` with
//  `y: int32` forces `x: int32`), or with a same-module callee's own
//  concretely-annotated parameter. Operators still link operand variables
//  together (both sides of `+` must end up the same type), which is a
//  structural fact independent of any one anchor. This pass never emits
//  diagnostics — a parameter with no anchor, or a genuinely conflicting
//  one, simply stays `k_unknown_type`, identical to today's behavior.
//
//  This is deliberately narrower than whole-program Hindley-Milner
//  inference (see spec/llm-compiler-roadmap.md phase 4): there is no
//  cross-function call-site-driven propagation, and — because the checker
//  checks each function body exactly once rather than re-checking it per
//  call-site instantiation — an argument whose type is genuinely
//  incompatible with an unconstrained parameter's body (e.g. calling
//  `double` with a type that has no `mul` impl) is not yet caught here;
//  that requires per-instantiation re-checking, which is a substantially
//  larger feature left as future work rather than approximated.
// ==========================================================================
class param_usage_inferrer {
public:
  param_usage_inferrer(type_table &types, const module_members *owner)
      : types_(types), owner_(owner) {}

  /// Seeds `name` with a type (concrete or a fresh variable) in the local
  /// inference environment.
  auto seed(std::string_view name, type_id type) -> void {
    if (!name.empty()) {
      env_.insert_or_assign(std::string(name), type);
    }
  }

  /// Resolves `name`'s best-known type after walking the body: a concrete
  /// type if usage constrained it, or `k_unknown_type` if nothing did.
  [[nodiscard]] auto resolved(std::string_view name) const -> type_id {
    const auto it = env_.find(std::string(name));
    if (it == env_.end()) {
      return k_unknown_type;
    }
    const auto root = find(it->second);
    return types_.entry(root).kind == type_kind::type_var_kind ? k_unknown_type
                                                               : root;
  }

  /// Seeds the function's own declared return type, if any, so `return`
  /// statements (and a compact expression body's implicit return) can anchor
  /// against it — the same "no other type this could be" reasoning as an
  /// annotated sibling parameter, just applied to the signature's return
  /// type instead of another parameter.
  auto set_return_type(type_id type) -> void { return_type_ = type; }

  /// Walks a function body (either a single expression or a statement
  /// list), collecting constraints as a side effect on the seeded
  /// environment.
  auto walk_body(const ast::expr *body_expr,
                 const std::vector<ast::ptr<ast::node>> &body_stmts) -> void {
    if (body_expr != nullptr) {
      // A compact expression body's value *is* the function's return value.
      unify(walk_expr(*body_expr), return_type_);
      return;
    }
    for (const auto &stmt : body_stmts) {
      if (stmt != nullptr) {
        walk_node(*stmt);
      }
    }
  }

private:
  type_table &types_;
  const module_members *owner_;
  type_id return_type_ = k_unknown_type;
  std::unordered_map<std::string, type_id> env_;
  std::unordered_map<type_id, type_id> subst_;

  /// Chases a variable's binding chain to its current root.
  [[nodiscard]] auto find(type_id id) const -> type_id {
    auto current = id;
    while (true) {
      const auto it = subst_.find(current);
      if (it == subst_.end() || it->second == current) {
        return current;
      }
      current = it->second;
    }
  }

  /// Unifies `a` and `b`: binds whichever side is still an unresolved
  /// variable to the other. Two already-concrete sides are left as-is —
  /// this pass never diagnoses, so a genuine conflict is simply not
  /// recorded (the parameter falls back to `k_unknown_type`, exactly as it
  /// would today). Never binds to `k_unknown_type`/`k_error_type`, since
  /// those carry no information to propagate.
  auto unify(type_id a, type_id b) -> void {
    if (a == k_unknown_type || b == k_unknown_type || a == k_error_type ||
        b == k_error_type) {
      return;
    }
    const auto ra = find(a);
    const auto rb = find(b);
    if (ra == rb) {
      return;
    }
    if (types_.entry(ra).kind == type_kind::type_var_kind) {
      subst_.insert_or_assign(ra, rb);
    } else if (types_.entry(rb).kind == type_kind::type_var_kind) {
      subst_.insert_or_assign(rb, ra);
    }
  }

  /// Resolves a simple, non-generic named-type annotation (a builtin
  /// scalar spelled directly, e.g. `int32`) without depending on the full
  /// `resolve_type`/module-context machinery this pass deliberately stays
  /// independent of; `nullopt` for anything else (generics, user types,
  /// refs, tuples, ...).
  [[nodiscard]] auto
  simple_builtin_annotation(const ast::type_expr *annotation) const
      -> std::optional<type_id> {
    if (annotation == nullptr ||
        annotation->kind != ast::node_kind::named_type) {
      return std::nullopt;
    }
    const auto &named = dynamic_cast<const ast::named_type &>(*annotation);
    if (named.path.size() != 1 || !named.type_args.empty() ||
        !is_builtin_scalar_name(named.path.front())) {
      return std::nullopt;
    }
    return types_.builtin(named.path.front());
  }

  static auto is_self_param(const ast::param &param) -> bool {
    return param.pattern != nullptr &&
           param.pattern->kind == ast::node_kind::binding_pattern &&
           dynamic_cast<const ast::binding_pattern &>(*param.pattern).name ==
               "self";
  }

  auto bind_pattern_name(const ast::pattern *pattern, type_id type) -> void {
    if (pattern != nullptr &&
        pattern->kind == ast::node_kind::binding_pattern) {
      seed(dynamic_cast<const ast::binding_pattern &>(*pattern).name, type);
    }
  }

  auto walk_unary(const ast::unary_expr &unary) -> type_id {
    const auto operand =
        unary.operand != nullptr ? walk_expr(*unary.operand) : k_unknown_type;
    if (unary.op == ast::unary_op::logical_not) {
      unify(operand, types_.builtin("bool"));
      return types_.builtin("bool");
    }
    if (unary.op == ast::unary_op::neg || unary.op == ast::unary_op::bit_not) {
      return operand;
    }
    return k_unknown_type;
  }

  auto walk_binary(const ast::binary_expr &binary) -> type_id {
    const auto lhs =
        binary.lhs != nullptr ? walk_expr(*binary.lhs) : k_unknown_type;
    const auto rhs =
        binary.rhs != nullptr ? walk_expr(*binary.rhs) : k_unknown_type;
    switch (binary.op) {
    case ast::binary_op::logical_and:
    case ast::binary_op::logical_or:
      unify(lhs, types_.builtin("bool"));
      unify(rhs, types_.builtin("bool"));
      return types_.builtin("bool");
    case ast::binary_op::eq_eq:
    case ast::binary_op::bang_eq:
    case ast::binary_op::lt:
    case ast::binary_op::lt_eq:
    case ast::binary_op::gt:
    case ast::binary_op::gt_eq:
      unify(lhs, rhs);
      return types_.builtin("bool");
    case ast::binary_op::add:
    case ast::binary_op::sub:
    case ast::binary_op::mul:
    case ast::binary_op::div:
    case ast::binary_op::mod:
    case ast::binary_op::add_wrap:
    case ast::binary_op::sub_wrap:
    case ast::binary_op::mul_wrap:
    case ast::binary_op::add_sat:
    case ast::binary_op::sub_sat:
    case ast::binary_op::mul_sat:
    case ast::binary_op::bit_and:
    case ast::binary_op::bit_or:
    case ast::binary_op::bit_xor:
    case ast::binary_op::shl:
    case ast::binary_op::shr: {
      unify(lhs, rhs);
      const auto left_root = find(lhs);
      const auto left_is_open =
          left_root == k_unknown_type ||
          types_.entry(left_root).kind == type_kind::type_var_kind;
      return left_is_open ? find(rhs) : left_root;
    }
    default:
      return k_unknown_type;
    }
  }

  auto walk_call_args_loosely(const ast::call_expr &call) -> void {
    for (const auto &arg : call.args) {
      if (arg.value != nullptr) {
        walk_expr(*arg.value);
      }
    }
  }

  auto walk_call(const ast::call_expr &call) -> type_id {
    if (call.callee == nullptr) {
      walk_call_args_loosely(call);
      return k_unknown_type;
    }
    if (call.callee->kind != ast::node_kind::ident_expr || owner_ == nullptr) {
      // A method call (`x.foo()`) or any other non-plain-name callee: we
      // can't anchor from an unresolved method's parameter types, but the
      // callee subtree (e.g. the receiver `x`, or `(a + b)` in
      // `(a + b).foo()`) may still contain param usage worth visiting.
      walk_expr(*call.callee);
      walk_call_args_loosely(call);
      return k_unknown_type;
    }
    const auto &name = dynamic_cast<const ast::ident_expr &>(*call.callee).name;
    const auto it = owner_->functions.find(name);
    if (it == owner_->functions.end() || it->second.decl == nullptr) {
      walk_call_args_loosely(call);
      return k_unknown_type;
    }
    const auto &callee = *it->second.decl;
    for (size_t i = 0; i < callee.params.size() && i < call.args.size(); ++i) {
      if (call.args[i].name.has_value() ||
          (i == 0 && is_self_param(callee.params[i]))) {
        if (call.args[i].value != nullptr) {
          walk_expr(*call.args[i].value);
        }
        continue;
      }
      const auto found =
          simple_builtin_annotation(callee.params[i].type_annotation.get());
      if (call.args[i].value == nullptr) {
        continue;
      }
      const auto arg_type = walk_expr(*call.args[i].value);
      if (found.has_value()) {
        unify(arg_type, *found);
      }
    }
    if (const auto found =
            simple_builtin_annotation(callee.return_type.get())) {
      return *found;
    }
    return k_unknown_type;
  }

  /// Finds `field_name` in a same-module struct type's field list, or
  /// `nullptr` if `owner_` is unset, the type isn't a same-module struct, or
  /// the field name isn't declared on it.
  [[nodiscard]] auto struct_field_decl(const ast::expr *type_name,
                                       std::string_view field_name) const
      -> const ast::struct_field * {
    if (owner_ == nullptr || type_name == nullptr ||
        type_name->kind != ast::node_kind::ident_expr) {
      return nullptr;
    }
    const auto &name = dynamic_cast<const ast::ident_expr &>(*type_name).name;
    const auto it = owner_->types.find(name);
    if (it == owner_->types.end() || it->second.decl == nullptr ||
        it->second.decl->definition == nullptr ||
        it->second.decl->definition->kind != ast::node_kind::struct_type_def) {
      return nullptr;
    }
    const auto &body =
        dynamic_cast<const ast::struct_type_def &>(*it->second.decl->definition)
            .body;
    for (const auto &field : body.fields) {
      if (field.name == field_name) {
        return &field;
      }
    }
    return nullptr;
  }

  /// A struct literal's fields are exactly like a callee's annotated
  /// parameters (see `walk_call`): a field declared with a concrete builtin
  /// scalar type anchors whatever unannotated parameter is assigned to it,
  /// by name (`Point { x: p }`) or by shorthand (`Point { x }`, meaning
  /// `x: x`).
  auto walk_struct_literal(const ast::struct_expr &literal) -> void {
    for (const auto &field : literal.fields) {
      const auto *decl_field =
          struct_field_decl(literal.type_name.get(), field.name);
      const auto found = decl_field != nullptr
                             ? simple_builtin_annotation(decl_field->type.get())
                             : std::nullopt;
      if (field.value != nullptr) {
        const auto value_type = walk_expr(*field.value);
        if (found.has_value()) {
          unify(value_type, *found);
        }
        continue;
      }
      // Shorthand `{ name }`: the value is the local variable `name` itself.
      if (found.has_value()) {
        if (const auto it = env_.find(field.name); it != env_.end()) {
          unify(it->second, *found);
        }
      }
    }
  }

  auto walk_if_branches(const std::vector<ast::if_branch> &branches,
                        const std::vector<ast::ptr<ast::node>> &else_body)
      -> void {
    for (const auto &branch : branches) {
      if (branch.condition != nullptr) {
        unify(walk_expr(*branch.condition), types_.builtin("bool"));
      }
      for (const auto &stmt : branch.body) {
        if (stmt != nullptr) {
          walk_node(*stmt);
        }
      }
    }
    for (const auto &stmt : else_body) {
      if (stmt != nullptr) {
        walk_node(*stmt);
      }
    }
  }

  auto walk_match_arms(const ast::expr *subject,
                       const std::vector<ast::match_arm> &arms) -> void {
    if (subject != nullptr) {
      walk_expr(*subject);
    }
    for (const auto &arm : arms) {
      if (arm.guard != nullptr) {
        walk_expr(*arm.guard);
      }
      if (arm.body_expr != nullptr) {
        walk_expr(*arm.body_expr);
      }
      for (const auto &stmt : arm.body_stmts) {
        if (stmt != nullptr) {
          walk_node(*stmt);
        }
      }
    }
  }

  /// Best-known type of `expr` for constraint purposes: a concrete type, a
  /// still-open variable, or `k_unknown_type` if this pass doesn't
  /// recognize the shape (walked for side effects only, in that case).
  auto walk_expr(const ast::expr &expr) -> type_id {
    switch (expr.kind) {
    case ast::node_kind::ident_expr: {
      const auto &ident = dynamic_cast<const ast::ident_expr &>(expr);
      const auto it = env_.find(ident.name);
      return it != env_.end() ? find(it->second) : k_unknown_type;
    }
    case ast::node_kind::literal_expr:
      // A bare literal is never a concrete anchor — see the class comment.
      // It is inherently polymorphic over whatever numeric/other type its
      // context requires, so unifying a parameter against one would wrongly
      // narrow a genuinely general parameter (`double(x): return x * 2`
      // must stay callable with every numeric type).
      return k_unknown_type;
    case ast::node_kind::group_expr:
      return walk_expr(*dynamic_cast<const ast::group_expr &>(expr).inner);
    case ast::node_kind::try_expr:
      return walk_expr(*dynamic_cast<const ast::try_expr &>(expr).operand);
    case ast::node_kind::unary_expr:
      return walk_unary(dynamic_cast<const ast::unary_expr &>(expr));
    case ast::node_kind::binary_expr:
      return walk_binary(dynamic_cast<const ast::binary_expr &>(expr));
    case ast::node_kind::call_expr:
      return walk_call(dynamic_cast<const ast::call_expr &>(expr));
    case ast::node_kind::tuple_expr:
      for (const auto &element :
           dynamic_cast<const ast::tuple_expr &>(expr).elements) {
        if (element != nullptr) {
          walk_expr(*element);
        }
      }
      return k_unknown_type;
    case ast::node_kind::field_expr: {
      const auto &field = dynamic_cast<const ast::field_expr &>(expr);
      if (field.object != nullptr) {
        walk_expr(*field.object);
      }
      return k_unknown_type;
    }
    case ast::node_kind::struct_expr:
      walk_struct_literal(dynamic_cast<const ast::struct_expr &>(expr));
      return k_unknown_type;
    case ast::node_kind::index_expr: {
      const auto &index = dynamic_cast<const ast::index_expr &>(expr);
      if (index.object != nullptr) {
        walk_expr(*index.object);
      }
      if (index.index != nullptr) {
        walk_expr(*index.index);
      }
      return k_unknown_type;
    }
    case ast::node_kind::block_expr:
      walk_body(nullptr, dynamic_cast<const ast::block_expr &>(expr).stmts);
      return k_unknown_type;
    case ast::node_kind::if_expr: {
      const auto &node = dynamic_cast<const ast::if_expr &>(expr);
      walk_if_branches(node.branches, node.else_body);
      return k_unknown_type;
    }
    case ast::node_kind::match_expr: {
      const auto &node = dynamic_cast<const ast::match_expr &>(expr);
      walk_match_arms(node.subject.get(), node.arms);
      return k_unknown_type;
    }
    default:
      return k_unknown_type;
    }
  }

  auto walk_node(const ast::node &node) -> void {
    switch (node.kind) {
    case ast::node_kind::let_stmt: {
      const auto &stmt = dynamic_cast<const ast::let_stmt &>(node);
      const auto found = stmt.initializer != nullptr
                             ? walk_expr(*stmt.initializer)
                             : k_unknown_type;
      // An explicit local annotation (`let y: int32 = x`) is the same kind
      // of fact as an annotated sibling parameter: there's no other type the
      // initializer could be, so it anchors whatever unresolved usage it
      // came from.
      if (const auto annotated =
              simple_builtin_annotation(stmt.type_annotation.get());
          annotated.has_value()) {
        unify(found, *annotated);
        bind_pattern_name(stmt.pattern.get(), *annotated);
      } else {
        bind_pattern_name(stmt.pattern.get(), found);
      }
      return;
    }
    case ast::node_kind::var_stmt: {
      const auto &stmt = dynamic_cast<const ast::var_stmt &>(node);
      const auto found = stmt.initializer != nullptr
                             ? walk_expr(*stmt.initializer)
                             : k_unknown_type;
      if (const auto annotated =
              simple_builtin_annotation(stmt.type_annotation.get());
          annotated.has_value()) {
        unify(found, *annotated);
        seed(stmt.name, *annotated);
      } else {
        seed(stmt.name, found);
      }
      return;
    }
    case ast::node_kind::assign_stmt: {
      const auto &stmt = dynamic_cast<const ast::assign_stmt &>(node);
      const auto rhs =
          stmt.value != nullptr ? walk_expr(*stmt.value) : k_unknown_type;
      if (stmt.target != nullptr &&
          stmt.target->kind == ast::node_kind::ident_expr) {
        const auto &ident = dynamic_cast<const ast::ident_expr &>(*stmt.target);
        if (const auto it = env_.find(ident.name); it != env_.end()) {
          unify(it->second, rhs);
        }
      } else if (stmt.target != nullptr) {
        walk_expr(*stmt.target);
      }
      return;
    }
    case ast::node_kind::expr_stmt: {
      const auto &stmt = dynamic_cast<const ast::expr_stmt &>(node);
      if (stmt.expr != nullptr) {
        walk_expr(*stmt.expr);
      }
      return;
    }
    case ast::node_kind::return_stmt: {
      const auto &stmt = dynamic_cast<const ast::return_stmt &>(node);
      if (stmt.value != nullptr) {
        unify(walk_expr(*stmt.value), return_type_);
      }
      return;
    }
    case ast::node_kind::if_stmt: {
      const auto &stmt = dynamic_cast<const ast::if_stmt &>(node);
      walk_if_branches(stmt.branches, stmt.else_body);
      return;
    }
    case ast::node_kind::while_stmt: {
      const auto &stmt = dynamic_cast<const ast::while_stmt &>(node);
      if (stmt.condition != nullptr) {
        unify(walk_expr(*stmt.condition), types_.builtin("bool"));
      }
      if (stmt.let_expr != nullptr) {
        walk_expr(*stmt.let_expr);
      }
      for (const auto &body_stmt : stmt.body) {
        if (body_stmt != nullptr) {
          walk_node(*body_stmt);
        }
      }
      return;
    }
    case ast::node_kind::for_stmt: {
      const auto &stmt = dynamic_cast<const ast::for_stmt &>(node);
      if (stmt.iterable != nullptr) {
        walk_expr(*stmt.iterable);
      }
      if (stmt.guard != nullptr) {
        walk_expr(*stmt.guard);
      }
      for (const auto &body_stmt : stmt.body) {
        if (body_stmt != nullptr) {
          walk_node(*body_stmt);
        }
      }
      return;
    }
    case ast::node_kind::match_stmt: {
      const auto &stmt = dynamic_cast<const ast::match_stmt &>(node);
      walk_match_arms(stmt.subject.get(), stmt.arms);
      return;
    }
    default:
      if (const auto *as_expr = dynamic_cast<const ast::expr *>(&node)) {
        walk_expr(*as_expr);
      }
      return;
    }
  }
};

// ==========================================================================
//  checker — one instance per session run.
// ==========================================================================

/// Runs name resolution and type checking over a whole session. One
/// instance is used for the entire `check_program` call: `types_` and the
/// caches persist across files so cross-file lookups and impl coherence
/// work, while per-file/per-function state is saved and restored around
/// each declaration.
class checker {
public:
  checker(const program_index &index, diagnostic_bag &diag,
          std::vector<bool> &file_has_errors)
      : index_(index), diag_(diag), file_has_errors_(file_has_errors) {}

  /// Entry point: validates impl coherence session-wide, then checks every
  /// input file in turn.
  auto run(const std::vector<parsed_module> &inputs) -> void;

  /// Hands the session's interned types and per-expression type map to the
  /// caller, once `run` has finished. Moves both out of the checker, so call
  /// this at most once, after `run` returns.
  auto take_checked_types() -> checked_types {
    return checked_types{
        .types = std::move(types_),
        .node_types = std::move(node_types_),
        .struct_pattern_field_types = std::move(struct_pattern_field_types_),
        .struct_literal_field_types = std::move(struct_literal_field_types_),
        .call_argument_mappings = std::move(call_argument_mappings_),
        .resolved_callees = std::move(resolved_callees_),
        .synthesized_decls = std::move(synthesized_decls_),
        .synthesized_trait_defaults = std::move(synthesized_trait_defaults_)};
  }

private:
  // --- session state ----------------------------------------------------
  const program_index &index_;
  diagnostic_bag &diag_;
  std::vector<bool> &file_has_errors_;
  type_table types_;
  /// Resolved type of every expression node visited by `infer_expr`
  /// (recorded there — see its wrapper — plus every pattern node visited by
  /// `check_pattern`). Several direct-`resolve_ident`-shaped bypasses also
  /// record here manually, since each resolves an ident's type from a
  /// declaration or binding directly rather than through `infer_expr`:
  /// `check_assignment`'s two branches (an assignment target resolved from
  /// an existing scope binding, and one resolved because no such binding
  /// exists), and `infer_call`'s three branches for a plain-identifier
  /// callee resolved as a real function reference (a scope binding, a
  /// same-module function, or an imported function — the remaining
  /// callee-name branches there are prelude magic forms like `println`/
  /// `panic` with no real declared type to record). Handed to the caller
  /// via `take_checked_types`.
  std::unordered_map<const ast::node *, type_id> node_types_;
  /// Resolved type of every non-rest `ast::field_pattern` visited by
  /// `check_pattern`'s `struct_pattern` case — needed for shorthand fields
  /// (`{x}`), which have no sub-pattern node of their own to key `node_types_`
  /// against. Handed to the caller via `take_checked_types`.
  std::unordered_map<const ast::field_pattern *, type_id>
      struct_pattern_field_types_;
  /// Resolved type of every non-`error` `ast::struct_field_init` shorthand
  /// field (`{x}`, `field.value == nullptr`) visited by
  /// `check_struct_literal` — these read an in-scope value directly rather
  /// than through `infer_expr`, so (like `struct_pattern_field_types_`)
  /// they need their own map. Handed to the caller via `take_checked_types`.
  std::unordered_map<const ast::struct_field_init *, type_id>
      struct_literal_field_types_;
  /// Per-call-site argument-to-parameter mapping, recorded in
  /// `check_call_args_against` — the only place a call is matched against
  /// a real declared parameter list (see `call_argument_mapping`'s doc
  /// comment in types.h for why a bare `fn(...)`-typed callee never gets
  /// an entry here). Handed to the caller via `take_checked_types`.
  std::unordered_map<const ast::call_expr *, call_argument_mapping>
      call_argument_mappings_;
  /// Every module-qualified or type-qualified call resolved by
  /// `infer_qualified_call`. Handed to the caller via `take_checked_types`.
  std::unordered_map<const ast::call_expr *, resolved_callee> resolved_callees_;
  /// Owns every trait-default method clone `build_method_table` synthesizes
  /// (see `synthesized_method` in types.h) — lifetime must outlive
  /// `checked_types`, so both are moved out together in
  /// `take_checked_types`.
  ast::ptr_vec<ast::func_decl> synthesized_decls_;
  std::vector<synthesized_method> synthesized_trait_defaults_;

  // --- current file / module context -------------------------------------
  const module_members *module_ = nullptr;
  std::string module_name_;
  file_id_type file_id_ = 0;
  bool file_has_external_wildcard_ = false;
  bool file_no_prelude_ = false;

  // --- current function context -------------------------------------------
  std::vector<std::unordered_map<std::string, value_binding>> scopes_;
  std::vector<std::unordered_map<std::string, type_id>> type_params_;
  type_id self_type_ = k_unknown_type;
  /// Associated-type names in scope for `self.<name>` references, valid
  /// while checking a trait's or impl's own members (see `check_trait_decl`
  /// and `check_impl_decl`).
  std::unordered_map<std::string, type_id> self_assoc_types_;
  type_id return_type_ = k_unknown_type;
  bool return_annotated_ = false;
  bool in_contract_ = false;
  std::unordered_set<std::string> reported_undefined_;

  // --- caches ---------------------------------------------------------------
  std::unordered_map<const ast::static_decl *, type_id> static_types_;
  std::unordered_set<const ast::static_decl *> statics_in_progress_;
  std::unordered_set<const ast::type_decl *> aliases_in_progress_;
  /// Cache of `param_types_for`'s result, so `check_function`'s own body
  /// check and `signature_params`'s call-site view agree on the same
  /// inferred types instead of independently guessing. Indices align 1:1
  /// with `decl.params`.
  std::unordered_map<const ast::func_decl *, std::vector<type_id>>
      inferred_param_types_;
  std::unordered_set<const ast::func_decl *> param_inference_in_progress_;
  bool methods_built_ = false;
  std::unordered_map<const ast::type_decl *, std::vector<method_entry>>
      methods_;
  /// Extend-block methods on a builtin type (e.g. `str`), keyed by the
  /// builtin's type-entry name since builtins have no `type_decl` to key
  /// `methods_` by.
  std::unordered_map<std::string, std::vector<method_entry>>
      extend_methods_by_builtin_;

  // ==========================================================================
  //  Diagnostics helpers
  // ==========================================================================

  /// Marks the file currently being checked (`file_id_`) as containing an
  /// error, so later driver phases skip emitting metadata for it.
  auto mark_error() -> void {
    if (static_cast<size_t>(file_id_) < file_has_errors_.size()) {
      file_has_errors_[file_id_] = true;
    }
  }

  /// Emits a single-label error at `span` in the current file.
  auto error(source_span span, const std::string &message,
             const std::string &label) -> void {
    auto diag = diagnostic(diagnostic_level::error, message, file_id_);
    diag.with_label(span, label);
    diag_.emit(diag);
    mark_error();
  }

  /// Emits a single-label error with an attached help suggestion.
  auto error_with_help(source_span span, const std::string &message,
                       const std::string &label, const std::string &help)
      -> void {
    auto diag = diagnostic(diagnostic_level::error, message, file_id_);
    diag.with_label(span, label);
    diag.with_help(help);
    diag_.emit(diag);
    mark_error();
  }

  /// Emits a "type mismatch" error if `found` is not `compatible` with
  /// `expected`; a no-op otherwise. Adds a conversion hint when both sides
  /// are numeric, since Kira never converts numbers implicitly.
  auto type_mismatch(source_span span, type_id expected, type_id found,
                     std::string_view context) -> void {
    if (types_.compatible(expected, found)) {
      return;
    }
    auto diag =
        diagnostic(diagnostic_level::error,
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
    diag_.emit(diag);
    mark_error();
  }

  // ==========================================================================
  //  Scope management
  // ==========================================================================

  /// Opens a new innermost lexical scope for value bindings.
  auto push_scope() -> void { scopes_.emplace_back(); }
  /// Closes the innermost lexical scope.
  auto pop_scope() -> void { scopes_.pop_back(); }

  /// Introduces or shadows `name` in the innermost scope. A no-op for an
  /// empty name (e.g. `_`) or when no scope is open.
  auto bind_value(std::string_view name, type_id type, binding_origin origin,
                  source_span span) -> void {
    if (name.empty() || scopes_.empty()) {
      return;
    }
    scopes_.back().insert_or_assign(
        std::string(name),
        value_binding{.type = type, .origin = origin, .span = span});
  }

  /// Looks up `name` from the innermost scope outward, returning the first
  /// (most-shadowing) match, or `nullptr` if unbound.
  auto lookup_value(std::string_view name) -> const value_binding * {
    for (auto &scope : std::views::reverse(scopes_)) {
      if (const auto it = scope.find(std::string(name)); it != scope.end()) {
        return &it->second;
      }
    }
    return nullptr;
  }

  /// Opens a new generic-parameter scope, interning each named parameter as
  /// a `type_param` type.
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

  /// Closes the innermost generic-parameter scope.
  auto pop_type_params() -> void { type_params_.pop_back(); }

  /// Looks up an in-scope generic parameter by name, searching from the
  /// innermost scope outward.
  auto lookup_type_param(std::string_view name) -> std::optional<type_id> {
    for (auto &type_param : std::views::reverse(type_params_)) {
      if (const auto it = type_param.find(std::string(name));
          it != type_param.end()) {
        return it->second;
      }
    }
    return std::nullopt;
  }

  // ==========================================================================
  //  Module member lookup (across every file of the current module, plus
  //  imports and the prelude).
  // ==========================================================================

  /// Finds the session module that owns the longest matching prefix of
  /// `path`, used to resolve a multi-segment named-type path like
  /// `pkg.mod.Thing` to the module `pkg.mod` and member `Thing`.
  [[nodiscard]] auto
  find_session_module_of_path(const std::vector<std::string> &path) const
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

  /// Whether `path`'s first segment names (or prefixes) a module declared
  /// somewhere in this session — used to tell a session-owned reference
  /// apart from a genuinely external one this checker can't see into.
  [[nodiscard]] auto
  session_owns_path_root(const std::vector<std::string> &path) const -> bool {
    if (path.empty()) {
      return false;
    }
    const auto &root = path.front();
    return std::ranges::any_of(
        index_.modules, [&root](const auto &entry) -> bool {
          const auto &name = entry.first;
          return name == root || name.starts_with(root + ".");
        });
  }

  /// Returns the `use` bindings recorded for the file currently being
  /// checked, or `nullptr` if it has none.
  [[nodiscard]] auto imports_for_current_file() const
      -> const std::vector<import_binding> * {
    const auto it = index_.imports.find(file_id_);
    return it != index_.imports.end() ? &it->second : nullptr;
  }

  /// Finds the non-wildcard import binding that introduces `name` locally
  /// in the current file.
  [[nodiscard]] auto find_import(std::string_view name) const
      -> const import_binding * {
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
  [[nodiscard]] auto import_source_module(const import_binding &binding) const
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

  /// Returns the local member name an import binds — the selector's item
  /// name if present, otherwise the trailing path segment.
  [[nodiscard]] auto imported_member_name(const import_binding &binding) const
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

  /// Ambient state for resolving a type expression: which module's members
  /// are in scope, an optional substitution for generic parameters (used
  /// when instantiating a generic declaration), and whether to also consult
  /// the live `type_params_` scope stack (only true for types resolved in
  /// the context of the function/type/impl currently being checked).
  struct resolve_ctx {
    const module_members *module = nullptr;
    const std::unordered_map<std::string, type_id> *param_bindings = nullptr;
    bool use_type_param_stack = false;
    bool quiet = false;
  };

  /// The resolve context for types written in the body of the
  /// function/type/impl currently being checked: current module plus the
  /// live generic-parameter scope stack.
  auto current_resolve_ctx() -> resolve_ctx {
    return resolve_ctx{.module = module_,
                       .param_bindings = nullptr,
                       .use_type_param_stack = true,
                       .quiet = false};
  }

  /// Recursively resolves a type-position AST node to an interned `type_id`,
  /// dispatching on its concrete kind. `named_type` is the only case with
  /// real resolution work (see `resolve_named_type`); the rest just resolve
  /// their component types structurally.
  auto resolve_type(const ast::type_expr &type, const resolve_ctx &ctx)
      -> type_id {
    if (type.has_error) {
      return k_unknown_type;
    }

    switch (type.kind) {
    case ast::node_kind::named_type:
      return resolve_named_type(dynamic_cast<const ast::named_type &>(type),
                                ctx);
    case ast::node_kind::tuple_type: {
      const auto &tuple = dynamic_cast<const ast::tuple_type &>(type);
      auto elements = std::vector<type_id>{};
      elements.reserve(tuple.elements.size());
      for (const auto &element : tuple.elements) {
        elements.push_back(element != nullptr ? resolve_type(*element, ctx)
                                              : k_unknown_type);
      }
      return types_.tuple_of(std::move(elements));
    }
    case ast::node_kind::slice_type: {
      const auto &slice = dynamic_cast<const ast::slice_type &>(type);
      const auto element = slice.element != nullptr
                               ? resolve_type(*slice.element, ctx)
                               : k_unknown_type;
      return types_.builtin_generic("slice", {element});
    }
    case ast::node_kind::array_type: {
      const auto &array = dynamic_cast<const ast::array_type &>(type);
      const auto element = array.element != nullptr
                               ? resolve_type(*array.element, ctx)
                               : k_unknown_type;
      auto size = std::optional<uint64_t>{};
      if (array.size != nullptr &&
          array.size->kind == ast::node_kind::literal_expr) {
        const auto &lit = dynamic_cast<const ast::literal_expr &>(*array.size);
        if (lit.lit_kind == token_kind::int_lit) {
          size = parse_integer_literal(lit.value);
        }
      }
      return types_.array_of(element, size);
    }
    case ast::node_kind::ref_type: {
      const auto &ref = dynamic_cast<const ast::ref_type &>(type);
      const auto inner =
          ref.inner != nullptr ? resolve_type(*ref.inner, ctx) : k_unknown_type;
      return types_.ref_to(inner, ref.is_mut);
    }
    case ast::node_kind::ptr_type: {
      const auto &ptr = dynamic_cast<const ast::ptr_type &>(type);
      const auto inner =
          ptr.inner != nullptr ? resolve_type(*ptr.inner, ctx) : k_unknown_type;
      return types_.ptr_to(inner, ptr.is_mut);
    }
    case ast::node_kind::fn_type: {
      const auto &fn = dynamic_cast<const ast::fn_type &>(type);
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
      const auto &refinement = dynamic_cast<const ast::refinement_type &>(type);
      return refinement.base != nullptr ? resolve_type(*refinement.base, ctx)
                                        : k_unknown_type;
    }
    default:
      return k_unknown_type;
    }
  }

  /// Resolves each generic argument slot of a named type. A slot holding a
  /// type expression resolves to that type; a slot holding a bare integer
  /// literal (a const generic argument, e.g. the `3` in `vec[T, 3]`) interns
  /// as a `const_value` so distinct literals produce distinct types; a slot
  /// holding a bare identifier that names an in-scope value type-parameter
  /// (e.g. `n` inside `def head[T, n: usize](v: vec[T, n])`) resolves to
  /// that parameter, keeping the slot fully generic. Any other compile-time
  /// value expression (arithmetic such as `m + n`, calls, ...) is not
  /// evaluated and resolves to `unknown` — see `spec/dependent-types-
  /// design.md` for why this is deliberately not a constraint solver yet.
  /// `decl_params`, when given, is the target declaration's own type
  /// parameters, used only to look up a literal argument's declared
  /// underlying scalar type by position; it may be shorter than `named`'s
  /// argument list (arity mismatches are diagnosed by the caller).
  auto
  resolve_type_args(const ast::named_type &named, const resolve_ctx &ctx,
                    const std::vector<ast::type_param> *decl_params = nullptr)
      -> std::vector<type_id> {
    auto args = std::vector<type_id>{};
    args.reserve(named.type_args.size());
    for (size_t i = 0; i < named.type_args.size(); ++i) {
      const auto &arg = named.type_args[i];
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
            dynamic_cast<const ast::type_expr &>(*arg.value), ctx));
        break;
      case ast::node_kind::literal_expr: {
        const auto &lit = dynamic_cast<const ast::literal_expr &>(*arg.value);
        const auto value = lit.lit_kind == token_kind::int_lit
                               ? parse_integer_literal(lit.value)
                               : std::nullopt;
        if (!value.has_value()) {
          args.push_back(k_unknown_type);
          break;
        }
        auto underlying = types_.usize_type();
        if (decl_params != nullptr && i < decl_params->size() &&
            (*decl_params)[i].is_value_param &&
            (*decl_params)[i].bound_or_type != nullptr) {
          underlying = resolve_type(*(*decl_params)[i].bound_or_type, ctx);
        }
        args.push_back(types_.const_value(underlying, *value));
        break;
      }
      case ast::node_kind::ident_expr: {
        const auto &ident = dynamic_cast<const ast::ident_expr &>(*arg.value);
        if (ctx.param_bindings != nullptr) {
          if (const auto it = ctx.param_bindings->find(ident.name);
              it != ctx.param_bindings->end()) {
            args.push_back(it->second);
            break;
          }
        }
        if (ctx.use_type_param_stack) {
          if (const auto param = lookup_type_param(ident.name)) {
            args.push_back(*param);
            break;
          }
        }
        args.push_back(k_unknown_type);
        break;
      }
      default:
        args.push_back(k_unknown_type);
        break;
      }
    }
    return args;
  }

  /// Builds the candidate name list for "did you mean" suggestions on an
  /// undefined type: common builtins plus every type/trait/concept in the
  /// current module and every in-scope generic parameter.
  auto type_name_candidates() -> std::vector<std::string> {
    auto candidates = std::vector<std::string>{
        "bool", "int32", "int64",  "float64", "str",   "char",
        "unit", "list",  "option", "result",  "usize", "byte",
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

  /// Emits "undefined type" once per distinct name per file (via
  /// `reported_undefined_`), with a suggestion or a declare/import hint.
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
    diag_.emit(diag);
    mark_error();
  }

  /// Checks generic-argument arity against `decl.type_params` (when
  /// arguments were written at all — omitting them entirely is allowed and
  /// leaves parameters `unknown`), then builds the instantiated type.
  auto instantiate_user_type(const ast::type_decl &decl,
                             std::string_view owner_module,
                             const ast::named_type &named,
                             const resolve_ctx &ctx) -> type_id {
    const auto args = resolve_type_args(named, ctx, &decl.type_params);

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
      diag_.emit(diag);
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
          dynamic_cast<const ast::type_expr &>(*decl.definition), ctx);
      break;
    default:
      break;
    }
    aliases_in_progress_.erase(&decl);
    return resolved;
  }

  /// Resolves a named-type reference through, in order: a session-owned
  /// multi-segment path; an in-scope generic parameter or substitution;
  /// `self`; a builtin scalar or prelude container; the current module's
  /// declarations; an imported name; a prelude trait used in bound position;
  /// or (failing all of that) reports an undefined-type error, unless
  /// suppressed by `ctx.quiet` or an external wildcard import that might
  /// plausibly supply the name.
  auto resolve_named_type(const ast::named_type &named, const resolve_ctx &ctx)
      -> type_id {
    if (named.path.empty()) {
      return k_unknown_type;
    }

    if (named.path.size() == 2 && named.path.front() == "self") {
      // `self.output` — an associated-type reference, not a module path.
      if (const auto it = self_assoc_types_.find(named.path.back());
          it != self_assoc_types_.end()) {
        return it->second;
      }
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

    // Prelude traits used in bound positions (`T: show`, `T: drop`).
    if (find_prelude_trait(name).has_value() || name == "send" ||
        name == "share" || name == "pool") {
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

  /// Maps an instantiated user type's declared generic-parameter names to
  /// the concrete argument ids it was instantiated with, for substituting
  /// into field/variant-payload type expressions.
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

  /// Builds the resolve context for a struct field or sum variant payload
  /// type: the instance's owning module, plus its generic-parameter
  /// substitution.
  auto
  member_resolve_ctx(const type_entry &instance,
                     const std::unordered_map<std::string, type_id> &bindings)
      -> resolve_ctx {
    return resolve_ctx{.module = index_.find_module(instance.module_name),
                       .param_bindings = &bindings,
                       .use_type_param_stack = false,
                       .quiet = true};
  }

  /// Returns the field list of a struct-kind instance, or `nullptr` if
  /// `instance` is not a struct.
  auto struct_fields_of(const type_entry &instance)
      -> const std::vector<ast::struct_field> * {
    if (instance.kind != type_kind::struct_kind || instance.decl == nullptr ||
        instance.decl->definition == nullptr ||
        instance.decl->definition->kind != ast::node_kind::struct_type_def) {
      return nullptr;
    }
    return &dynamic_cast<const ast::struct_type_def &>(
                *instance.decl->definition)
                .body.fields;
  }

  /// Looks up a field by name on a struct instance, resolving its declared
  /// type with the instance's generic substitution applied. `nullopt` when
  /// the field does not exist (distinct from `k_unknown_type`, which means
  /// the field exists but has no annotation).
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
        return resolve_type(*field.type,
                            member_resolve_ctx(instance, bindings));
      }
    }
    return std::nullopt;
  }

  /// Returns the variant list of a sum-kind instance, or `nullptr` if
  /// `instance` is not a sum type.
  auto sum_variants_of(const type_entry &instance)
      -> const std::vector<ast::sum_variant> * {
    if (instance.kind != type_kind::sum_kind || instance.decl == nullptr ||
        instance.decl->definition == nullptr ||
        instance.decl->definition->kind != ast::node_kind::sum_type_def) {
      return nullptr;
    }
    return &dynamic_cast<const ast::sum_type_def &>(*instance.decl->definition)
                .body.variants;
  }

  /// Looks up a variant by name on a sum-type instance.
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

  /// Resolves a variant's payload types with the instance's generic
  /// substitution applied.
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

  /// Returns a parameter's bound name if it is a simple identifier
  /// pattern, or an empty string for a destructuring parameter pattern.
  auto param_name_of(const ast::param &param) -> std::string {
    if (param.pattern != nullptr &&
        param.pattern->kind == ast::node_kind::binding_pattern) {
      return dynamic_cast<const ast::binding_pattern &>(*param.pattern).name;
    }
    return {};
  }

  /// Returns `decl`'s parameter types with unannotated, non-`self`
  /// parameters filled in from local body-usage inference where possible
  /// (`param_usage_inferrer` above); indices align 1:1 with `decl.params`.
  /// Annotated parameters and `self` are untouched by this cache — callers
  /// still resolve those the way they always have. Generic functions,
  /// `pub` functions (which must annotate everything already), and a
  /// function already being inferred higher up the call stack (recursion
  /// guard) skip inference and simply report `k_unknown_type`, identical
  /// to today's behavior.
  auto param_types_for(const ast::func_decl &decl, const module_members *owner)
      -> const std::vector<type_id> & {
    if (const auto it = inferred_param_types_.find(&decl);
        it != inferred_param_types_.end()) {
      return it->second;
    }
    auto &stored = inferred_param_types_
                       .emplace(&decl, std::vector<type_id>(decl.params.size(),
                                                            k_unknown_type))
                       .first->second;

    const auto can_infer = decl.visibility != ast::visibility::pub &&
                           decl.type_params.empty() &&
                           param_inference_in_progress_.insert(&decl).second;
    if (!can_infer) {
      return stored;
    }

    auto inferrer = param_usage_inferrer(types_, owner);
    auto seeded_names = std::vector<std::string>(decl.params.size());
    const auto ctx = resolve_ctx{.module = owner,
                                 .param_bindings = nullptr,
                                 .use_type_param_stack = false,
                                 .quiet = true};
    // A declared return type is the same kind of fact as an annotated
    // sibling parameter: `def f(x) -> int32: return x` has no other type
    // `x` could be, so `return` (and a compact expression body's implicit
    // return) anchor against it.
    if (decl.return_type != nullptr) {
      inferrer.set_return_type(resolve_type(*decl.return_type, ctx));
    }
    for (size_t i = 0; i < decl.params.size(); ++i) {
      const auto &param = decl.params[i];
      const auto name = param_name_of(param);
      if (name.empty() || (i == 0 && name == "self")) {
        continue;
      }
      seeded_names[i] = name;
      // Annotated siblings are seeded with their real type so a mixed
      // signature still constrains its unannotated parameters (e.g.
      // `def f(x, y: int32): return x + y` infers `x: int32`).
      inferrer.seed(name, param.type_annotation != nullptr
                              ? resolve_type(*param.type_annotation, ctx)
                              : types_.fresh_type_var());
    }
    inferrer.walk_body(decl.body_expr.get(), decl.body_stmts);
    for (size_t i = 0; i < decl.params.size(); ++i) {
      if (decl.params[i].type_annotation == nullptr &&
          !seeded_names[i].empty()) {
        stored[i] = inferrer.resolved(seeded_names[i]);
      }
    }
    param_inference_in_progress_.erase(&decl);
    return stored;
  }

  /// Normalizes a function declaration's parameter list into
  /// `fn_param_info`s with their types resolved against the function's own
  /// generic parameters. `skip_self` drops a leading `self` parameter,
  /// since call-argument checking never expects the caller to pass it.
  auto signature_params(const ast::func_decl &decl, const module_members *owner,
                        bool skip_self) -> std::vector<fn_param_info> {
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

    const auto &inferred = param_types_for(decl, owner);
    auto params = std::vector<fn_param_info>{};
    params.reserve(decl.params.size());
    for (size_t i = 0; i < decl.params.size(); ++i) {
      const auto &param = decl.params[i];
      auto name = param_name_of(param);
      if (i == 0 && skip_self && name == "self") {
        continue;
      }
      auto type = k_unknown_type;
      if (param.type_annotation != nullptr) {
        type = resolve_type(*param.type_annotation, ctx);
      } else if (i < inferred.size()) {
        type = inferred[i];
      }
      params.push_back(fn_param_info{
          .name = name,
          .type = type,
          .has_default = param.default_value != nullptr,
          .span = param.span,
      });
    }
    return params;
  }

  /// Resolves a function's declared return type against its own generic
  /// parameters; `unknown` for an unannotated return type (inferred from the
  /// body, never guessed from this signature alone).
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

  /// Builds the `fn(...)` value type of a function declaration, used when
  /// a function name is referenced as a value (passed as an argument,
  /// assigned to a binding) rather than called directly.
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

  /// Matches a call's arguments against a parameter list: positional
  /// arguments fill the next unused parameter in order, named arguments
  /// bind by name (reporting unknown/duplicate names), and each argument's
  /// inferred type is checked against its target parameter's type. Reports
  /// too many arguments, and any required parameter left unfilled. Also
  /// records the resulting argument-to-parameter assignment into
  /// `call_argument_mappings_` (see `call_argument_mapping`'s doc comment
  /// in types.h), since this is the only place that assignment is known.
  auto check_call_args_against(const ast::call_expr &call,
                               const std::vector<fn_param_info> &params,
                               std::string_view callee_name,
                               source_location decl_location) -> void {
    auto param_used = std::vector<bool>(params.size(), false);
    auto args_by_param = std::vector<const ast::expr *>(params.size(), nullptr);
    auto next_positional = size_t{0};
    auto seen_named = false;

    for (const auto &arg : call.args) {
      const fn_param_info *target = nullptr;
      if (arg.name.has_value()) {
        const auto &arg_name = *arg.name;
        seen_named = true;
        for (size_t i = 0; i < params.size(); ++i) {
          if (params[i].name == arg_name) {
            if (param_used[i]) {
              error(arg.span,
                    std::format("argument `{}` is provided more than once",
                                arg_name),
                    "duplicate argument");
            }
            param_used[i] = true;
            args_by_param[i] = arg.value.get();
            target = &params[i];
            break;
          }
        }
        if (target == nullptr) {
          auto diag = diagnostic(
              diagnostic_level::error,
              std::format("unknown named argument `{}` in call to `{}`",
                          arg_name, callee_name),
              file_id_);
          diag.with_label(arg.span, "no parameter has this name");
          auto names = std::vector<std::string>{};
          for (const auto &param : params) {
            names.push_back(param.name);
          }
          if (const auto suggestion = best_suggestion(arg_name, names)) {
            diag.with_help(std::format("did you mean `{}:`?", *suggestion));
          }
          diag_.emit(diag);
          mark_error();
        }
      } else {
        if (seen_named) {
          error(arg.span, "positional arguments may not follow named arguments",
                "positional argument after a named one");
        }
        while (next_positional < params.size() && param_used[next_positional]) {
          ++next_positional;
        }
        if (next_positional < params.size()) {
          param_used[next_positional] = true;
          args_by_param[next_positional] = arg.value.get();
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
      diag_.emit(diag);
      mark_error();
    }

    call_argument_mappings_[&call] =
        call_argument_mapping{.args_by_param = std::move(args_by_param)};
  }

  /// Checks a call against a known `func_decl`: enforces the
  /// contract-purity rule when inside a contract condition, checks its
  /// arguments via `check_call_args_against`, and returns its return type.
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
    check_call_args_against(
        call, params, decl.name,
        source_location{.file_id = decl_file, .span = decl.span});
    return signature_return_type(decl, owner);
  }

  /// Infers each argument's type with no expectation, for a call whose
  /// callee could not be resolved — keeps names inside the arguments from
  /// going unchecked even though the call itself can't be validated.
  auto infer_call_args_loosely(const ast::call_expr &call) -> void {
    for (const auto &arg : call.args) {
      if (arg.value != nullptr) {
        infer_expr(*arg.value, k_unknown_type);
      }
    }
  }

  /// Checks a sum-type variant constructor call's arguments against the
  /// variant's declared payload types, and — if the sum type is generic and
  /// wasn't already instantiated — infers its type arguments from payload
  /// slots that name a bare type parameter directly.
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
                dynamic_cast<const ast::named_type &>(*payload_type);
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
    const auto expected_is = [&](std::string_view generic) -> auto {
      return expected_entry.kind == type_kind::builtin_generic_kind &&
             expected_entry.name == generic;
    };

    auto payload_expected = k_unknown_type;
    if ((name == "some" && expected_is("option")) ||
        (name == "ok" && expected_is("result"))) {
      payload_expected =
          expected_entry.args.empty() ? k_unknown_type : expected_entry.args[0];
    } else if (name == "err" && expected_is("result")) {
      payload_expected = expected_entry.args.size() > 1 ? expected_entry.args[1]
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

  /// Finds a sum-type variant named `name` declared by the current module,
  /// or reachable through a non-wildcard import of its owning type.
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

  /// Resolves a variant name (`@some`/`@ok`/`@err`/`@none`, or a
  /// user sum-type variant) to its constructed type. Prefers a variant of
  /// the expected sum type when one matches, then falls back to any
  /// module-visible sum type declaring that variant name. `call` is
  /// non-null when the variant is being constructed with arguments
  /// (`@some(x)`) rather than referenced bare (`@none`). Returns `nullopt`
  /// when no sum type declares this variant.
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
                std::format(
                    "variant `@{}` of `{}` carries {} value{} and "
                    "must be constructed with `@{}(...)`",
                    name, expected_entry.name, variant->payload_types.size(),
                    variant->payload_types.size() == 1 ? "" : "s", name),
                "missing constructor values");
        }
        return strip_refs(expected);
      }
    }

    if (const auto found = find_module_variant(name)) {
      const auto *decl = found->first;
      const auto instance_id = make_user_type(*decl, module_name_, {});
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

  /// Whether `name` is a builtin, prelude container/trait, or a
  /// type/trait/concept the current module declares — used to avoid
  /// reporting an "undefined name" error for a name that is really a type
  /// used where the checker didn't expect one.
  auto is_type_like_name(std::string_view name) -> bool {
    if (is_builtin_scalar_name(name) || builtin_generic_arity(name) ||
        name == "array" || find_prelude_trait(name).has_value()) {
      return true;
    }
    if (module_ != nullptr && (module_->types.contains(std::string(name)) ||
                               module_->traits.contains(std::string(name)) ||
                               module_->concepts.contains(std::string(name)))) {
      return true;
    }
    return false;
  }

  /// Builds the candidate name list for "did you mean" suggestions on an
  /// undefined value name: every in-scope binding, plus the current
  /// module's functions, statics, and sum-type variants, plus common
  /// prelude functions.
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
    for (const auto prelude : {"println", "print", "panic", "assert", "size_of",
                               "args", "env", "min", "max"}) {
      candidates.emplace_back(prelude);
    }
    return candidates;
  }

  /// Emits "undefined name" once per distinct name per file, with a
  /// suggestion or a define/import hint.
  auto emit_undefined_name(source_span span, std::string_view name) -> void {
    if (!reported_undefined_.insert(std::format("value:{}", name)).second) {
      return;
    }
    auto diag = diagnostic(diagnostic_level::error,
                           std::format("undefined name `{}`", name), file_id_);
    diag.with_label(span, "not found in this scope");
    if (const auto suggestion =
            best_suggestion(name, value_name_candidates())) {
      diag.with_help(std::format("did you mean `{}`?", *suggestion));
    } else {
      diag.with_help(std::format(
          "Define `{}` before using it, or import it with `use`.", name));
    }
    diag_.emit(diag);
    mark_error();
  }

  /// Names the checker treats as always resolvable without deeper lookup:
  /// prelude functions, loop control keywords, execution-context names, and
  /// concurrency primitives whose full typing is out of scope for now.
  auto is_prelude_value_name(std::string_view name) -> bool {
    return name == "println" || name == "print" || name == "panic" ||
           name == "assert" || name == "size_of" || name == "args" ||
           name == "env" || name == "min" || name == "max" || name == "break" ||
           name == "continue" || name == "cancel" || name == "pool" ||
           name == "io" || name == "cpu" || name == "channel" ||
           name == "watch" || name == "shared" || name == "expr";
  }

  /// Resolves a value-position identifier through, in order: a variant
  /// constructor (if `@`-prefixed); a local binding or `self`; the current
  /// module's functions/statics; an imported name; a prelude/type-like name;
  /// a bare (un-`@`-prefixed) variant spelling; or reports undefined-name,
  /// unless suppressed by an external wildcard import.
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

  /// Emits "unknown variant" once per distinct name per file, noting the
  /// expected sum type's actual variants when one is known.
  auto emit_undefined_variant(source_span span, std::string_view name,
                              type_id expected) -> void {
    if (!reported_undefined_.insert(std::format("variant:{}", name)).second) {
      return;
    }
    auto diag =
        diagnostic(diagnostic_level::error,
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
    diag_.emit(diag);
    mark_error();
  }

  /// Resolves and caches a `static` binding's declared type, guarding
  /// against an initializer cycle (`static a = b` / `static b = a`) by
  /// returning `unknown` if the declaration is already being resolved.
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

  /// Reports an error when an integer literal's value does not fit in its
  /// target builtin type's range (a no-op for non-integer or non-builtin
  /// targets, or when the literal's value could not be parsed).
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
    auto diag =
        diagnostic(diagnostic_level::error,
                   std::format("integer literal `{}` does not fit in `{}`",
                               lit.value, entry.name),
                   file_id_);
    diag.with_label(lit.span, std::format("too large for `{}`", entry.name));
    diag.with_note(
        std::format("the largest `{}` value is {}", entry.name, *max_value));
    diag.with_help("Use a wider integer type, or reduce the value.");
    diag_.emit(diag);
    mark_error();
  }

  /// Types a literal expression. An integer or float literal adopts the
  /// expected numeric type when one is given (checking integer fit),
  /// otherwise defaults to `int32`/`float64` per the language's literal
  /// defaulting rule.
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

  /// Maps an arithmetic operator to the trait that overloads it on a
  /// user struct/sum type (`add`, `sub`, `mul`, `div`, `rem`); an empty
  /// string for operators with no overload trait.
  auto operator_trait_for(ast::binary_op op) -> std::string_view {
    switch (op) {
    case ast::binary_op::add:
    case ast::binary_op::add_wrap:
    case ast::binary_op::add_sat:
      return "add";
    case ast::binary_op::sub:
    case ast::binary_op::sub_wrap:
    case ast::binary_op::sub_sat:
      return "sub";
    case ast::binary_op::mul:
    case ast::binary_op::mul_wrap:
    case ast::binary_op::mul_sat:
      return "mul";
    case ast::binary_op::div:
      return "div";
    case ast::binary_op::mod:
      return "rem";
    default:
      return {};
    }
  }

  /// For a user struct/sum/opaque operand, requires it to implement
  /// `trait_name` (reporting a missing-impl error with an `impl`/`deriving`
  /// hint otherwise); returns `unknown` for anything else, since the result
  /// type of an overloaded operator is decided by its impl, not guessed here.
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

  /// Types an arithmetic operator (`+`, `-`, `*`, `/`, `%` and their
  /// wrapping/saturating forms): both operands must be the same numeric
  /// type, or (for non-numeric operands) must implement the operator's
  /// overload trait. Reports a mismatched-numeric-types error rather than
  /// converting either side, since Kira never converts numbers implicitly.
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
      diag_.emit(diag);
      mark_error();
      return k_error_type;
    }
    return lhs;
  }

  /// Types `==`/`!=` (`is_equality`) or `<`/`<=`/`>`/`>=`: operands must be
  /// mutually compatible types, and (for a user struct/sum operand) must
  /// implement `eq`/`ord`. Always yields `bool`.
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

  /// Infers `expr` expecting `bool` and reports an error if it isn't —
  /// Kira has no truthiness, so every condition must be an explicit `bool`.
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

  /// Dispatches a binary expression to the appropriate typing rule for its
  /// operator family: arithmetic, equality/ordering comparison, logical
  /// (`and`/`or`, requiring `bool` operands), bitwise/shift (requiring
  /// integer operands), membership (`in`/`not in`), ranges, or `|`
  /// (bitwise-or on integers; otherwise untyped, since `|` also appears in
  /// sum-type/pipeline/pattern syntax this checker does not model).
  auto infer_binary(const ast::binary_expr &binary, type_id expected)
      -> type_id {
    switch (binary.op) {
    case ast::binary_op::add:
    case ast::binary_op::sub:
    case ast::binary_op::mul:
    case ast::binary_op::div:
    case ast::binary_op::mod:
    case ast::binary_op::add_wrap:
    case ast::binary_op::sub_wrap:
    case ast::binary_op::mul_wrap:
    case ast::binary_op::add_sat:
    case ast::binary_op::sub_sat:
    case ast::binary_op::mul_sat:
      return infer_arithmetic(binary, expected);

    case ast::binary_op::eq_eq:
    case ast::binary_op::bang_eq:
      return infer_comparison(binary, true);
    case ast::binary_op::lt:
    case ast::binary_op::lt_eq:
    case ast::binary_op::gt:
    case ast::binary_op::gt_eq:
      return infer_comparison(binary, false);

    case ast::binary_op::logical_and:
    case ast::binary_op::logical_or:
      if (binary.lhs != nullptr) {
        require_bool(*binary.lhs, "the left operand of a logical operator");
      }
      if (binary.rhs != nullptr) {
        require_bool(*binary.rhs, "the right operand of a logical operator");
      }
      return types_.builtin("bool");

    case ast::binary_op::shl:
    case ast::binary_op::shr:
    case ast::binary_op::bit_and:
    case ast::binary_op::bit_xor: {
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

    case ast::binary_op::in:
    case ast::binary_op::not_in: {
      if (binary.lhs != nullptr) {
        infer_expr(*binary.lhs, k_unknown_type);
      }
      if (binary.rhs != nullptr) {
        infer_expr(*binary.rhs, k_unknown_type);
      }
      return types_.builtin("bool");
    }

    case ast::binary_op::range:
    case ast::binary_op::range_inclusive: {
      const auto lhs = binary.lhs != nullptr
                           ? strip_refs(infer_expr(*binary.lhs, k_unknown_type))
                           : k_unknown_type;
      const auto rhs = binary.rhs != nullptr
                           ? strip_refs(infer_expr(*binary.rhs, lhs))
                           : k_unknown_type;
      const auto element = types_.is_integer(lhs)   ? lhs
                           : types_.is_integer(rhs) ? rhs
                                                    : k_unknown_type;
      return types_.builtin_generic("range", {element});
    }

    case ast::binary_op::pipe:
    case ast::binary_op::bit_or: {
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

  /// Types a unary operator: `-` requires (and preserves) a numeric
  /// operand, `not` requires and yields `bool`, `~` requires (and preserves)
  /// an integer operand, `*` (deref) unwraps a pointer/reference, and
  /// `&`/`&mut` wrap the operand in a reference type.
  auto infer_unary(const ast::unary_expr &unary, type_id expected) -> type_id {
    const auto operand =
        unary.operand != nullptr
            ? infer_expr(*unary.operand, unary.op == ast::unary_op::neg
                                             ? expected
                                             : k_unknown_type)
            : k_unknown_type;
    const auto stripped = strip_refs(operand);

    switch (unary.op) {
    case ast::unary_op::neg:
      if (!types_.is_unknown(stripped) && !types_.is_numeric(stripped)) {
        error(unary.span,
              std::format("unary `-` requires a numeric operand, found `{}`",
                          types_.display(stripped)),
              "not a numeric value");
        return k_error_type;
      }
      return stripped;
    case ast::unary_op::logical_not:
      if (!types_.is_unknown(stripped) && !types_.is_boolean(stripped)) {
        error(unary.span,
              std::format("`not` requires a `bool` operand, found `{}`",
                          types_.display(stripped)),
              "expected `bool` here");
      }
      return types_.builtin("bool");
    case ast::unary_op::bit_not:
      if (!types_.is_unknown(stripped) && !types_.is_integer(stripped)) {
        error(unary.span,
              std::format("`~` requires an integer operand, found `{}`",
                          types_.display(stripped)),
              "not an integer value");
      }
      return stripped;
    case ast::unary_op::deref: {
      const auto &entry = types_.entry(operand);
      if (entry.kind == type_kind::ptr_kind ||
          entry.kind == type_kind::ref_kind) {
        return entry.result;
      }
      return k_unknown_type;
    }
    case ast::unary_op::addr_of:
      return types_.ref_to(stripped, false);
    case ast::unary_op::addr_of_mut: {
      // `&mut container[a..b]` — `infer_index`'s range-slice result is
      // always the immutable `slice[T]` (the same range-index expression
      // written bare, or under a plain `&`, should stay immutable) — but
      // wrapped directly in `&mut`, the intent is a mutable sub-view, so
      // upgrade it to `slice_mut[T]` here rather than teaching `infer_index`
      // about an enclosing-expression context it otherwise has no reason to
      // know about.
      if (unary.operand != nullptr &&
          unary.operand->kind == ast::node_kind::index_expr) {
        const auto &stripped_entry = types_.entry(stripped);
        if (stripped_entry.kind == type_kind::builtin_generic_kind &&
            stripped_entry.name == "slice") {
          const auto element = stripped_entry.args.empty()
                                   ? k_unknown_type
                                   : stripped_entry.args[0];
          return types_.ref_to(
              types_.builtin_generic("slice_mut", {element}), true);
        }
      }
      return types_.ref_to(stripped, true);
    }
    }
    return k_unknown_type;
  }

  // ==========================================================================
  //  Impl / method tables
  // ==========================================================================

  /// Maps `"{trait_name}:{type_key}"` to the location of the first impl
  /// seen for that (trait, type) pair, session-wide — built by
  /// `validate_impl_coherence` and consulted by `type_has_trait`.
  std::unordered_map<std::string, source_location> impl_trait_index_;

  /// Extracts the trailing name of an impl's trait-type path (e.g. `show`
  /// from `impl show for point:`), or empty for an inherent impl with no
  /// trait.
  auto trait_name_of_impl(const ast::impl_decl &impl) -> std::string {
    if (impl.trait_type == nullptr ||
        impl.trait_type->kind != ast::node_kind::named_type) {
      return {};
    }
    const auto &named = dynamic_cast<const ast::named_type &>(*impl.trait_type);
    return named.path.empty() ? std::string{} : named.path.back();
  }

  /// A stable string key identifying a type for impl-coherence bookkeeping:
  /// the declaration's address for user types, or the type's display name
  /// otherwise (so two distinct instantiations of the same generic
  /// declaration share one coherence key, matching Kira's "one impl per
  /// (trait, type)" rule at the declaration level).
  auto type_key_of(const type_entry &entry) -> std::string {
    if (entry.decl != nullptr) {
      return std::format("u:{}", static_cast<const void *>(entry.decl));
    }
    return std::format("n:{}", entry.name);
  }

  /// Finds a trait declaration named `name` in the current module or
  /// reachable through a non-wildcard import.
  auto find_session_trait(std::string_view name)
      -> std::optional<trait_decl_ref> {
    if (module_ != nullptr) {
      if (const auto it = module_->traits.find(std::string(name));
          it != module_->traits.end()) {
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
    return find_prelude_trait(name);
  }

  /// Finds a trait declaration named `name` in the auto-imported prelude —
  /// module `prelude` itself, or one of the stdlib modules it re-exports
  /// (currently `std.traits`, home of `from`/`drop`) — without requiring an
  /// explicit `use`. Every real invocation of the compiler injects
  /// `prelude.kira`/`std/traits.kira` into the session (`compile_sources`,
  /// `src/driver/driver.cpp`); a file that writes `no_prelude` opts out.
  /// Absent from a session that never included those files (most unit
  /// tests), this is simply inert.
  auto find_prelude_trait(std::string_view name)
      -> std::optional<trait_decl_ref> {
    if (file_no_prelude_) {
      return std::nullopt;
    }
    static constexpr std::array<std::string_view, 2>
        k_prelude_reexport_modules = {"prelude", "std.traits"};
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

  /// Resolves the concrete type an impl block targets (its `for` clause),
  /// against the impl's own generic parameters.
  auto resolve_impl_target(const impl_ref &impl) -> type_id {
    if (impl.decl->for_type == nullptr) {
      return k_unknown_type;
    }
    auto param_bindings = std::unordered_map<std::string, type_id>{};
    for (const auto &param : impl.decl->type_params) {
      if (!param.name.empty()) {
        param_bindings.emplace(param.name, types_.type_param(param.name));
      }
    }
    const auto ctx = resolve_ctx{.module = index_.find_module(impl.module_name),
                                 .param_bindings = &param_bindings,
                                 .use_type_param_stack = false,
                                 .quiet = true};
    return resolve_type(*impl.decl->for_type, ctx);
  }

  /// Resolves the concrete type an extend block targets (its `extend`
  /// clause). Extend blocks carry no generic parameters of their own.
  auto resolve_extend_target(const extend_ref &ext) -> type_id {
    if (ext.decl->for_type == nullptr) {
      return k_unknown_type;
    }
    const auto ctx = resolve_ctx{.module = index_.find_module(ext.module_name),
                                 .param_bindings = nullptr,
                                 .use_type_param_stack = false,
                                 .quiet = true};
    return resolve_type(*ext.decl->for_type, ctx);
  }

  /// Builds `methods_` once per session (idempotent via `methods_built_`):
  /// for every impl block in every module, records its methods against the
  /// impl's target type declaration, plus — for a trait impl — the target
  /// trait's default-bodied methods, so calling a non-overridden default
  /// method through the impl works without duplicating its body per type.
  /// Also records every `extend` block's methods, against the target's
  /// `type_decl` when it names a user type or against the target's builtin
  /// name (`extend_methods_by_builtin_`) otherwise.
  auto build_method_table() -> void {
    if (methods_built_) {
      return;
    }
    methods_built_ = true;

    for (const auto &[module_name, members] : index_.modules) {
      for (const auto &ext : members.extends) {
        const auto target = strip_refs(resolve_extend_target(ext));
        const auto &target_entry = types_.entry(target);
        auto extend_methods = std::vector<method_entry>{};
        for (const auto &item : ext.decl->items) {
          if (item == nullptr || item->has_error ||
              item->kind != ast::node_kind::func_decl) {
            continue;
          }
          extend_methods.push_back(method_entry{
              .decl = dynamic_cast<const ast::func_decl *>(item.get()),
              .owner = &members,
              .from_trait = nullptr,
              .is_extension = true,
          });
        }
        if (extend_methods.empty()) {
          continue;
        }
        if (target_entry.decl != nullptr) {
          auto &methods = methods_[target_entry.decl];
          methods.insert(methods.end(), extend_methods.begin(),
                         extend_methods.end());
        } else if (!target_entry.name.empty()) {
          auto &methods = extend_methods_by_builtin_[target_entry.name];
          methods.insert(methods.end(), extend_methods.begin(),
                         extend_methods.end());
        }
      }

      for (const auto &impl : members.impls) {
        const auto target = strip_refs(resolve_impl_target(impl));
        const auto &target_entry = types_.entry(target);
        if (target_entry.decl == nullptr) {
          continue;
        }
        auto &methods = methods_[target_entry.decl];
        auto overridden_names = std::unordered_set<std::string>{};

        for (const auto &item : impl.decl->items) {
          if (item == nullptr || item->has_error ||
              item->kind != ast::node_kind::func_decl) {
            continue;
          }
          const auto *decl = dynamic_cast<const ast::func_decl *>(item.get());
          overridden_names.insert(decl->name);
          methods.push_back(method_entry{
              .decl = decl,
              .owner = &members,
              .from_trait = nullptr,
          });
        }

        // Trait methods with default bodies are callable through the impl.
        const auto trait_name = trait_name_of_impl(*impl.decl);
        if (trait_name.empty()) {
          continue;
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
          continue;
        }
        for (const auto &item : trait_decl->items) {
          if (item == nullptr || item->kind != ast::node_kind::func_decl) {
            continue;
          }
          const auto *decl = dynamic_cast<const ast::func_decl *>(item.get());
          if (overridden_names.contains(decl->name)) {
            continue;
          }
          const auto has_self = !decl->params.empty() &&
                                param_name_of(decl->params.front()) == "self";
          const auto has_body =
              decl->body_expr != nullptr || !decl->body_stmts.empty();
          if (!has_self || !has_body) {
            // No `self` receiver (an associated-function-shaped trait
            // requirement) or no default body (a required method the impl
            // must itself provide, already validated by
            // `check_impl_members`) — nothing to monomorphize.
            methods.push_back(method_entry{
                .decl = decl,
                .owner = trait_module,
                .from_trait = trait_decl,
            });
            continue;
          }
          monomorphize_trait_default(*decl, target, target_entry.name,
                                     trait_module, trait_file_id, methods);
        }
      }
    }
  }

  /// Clones `decl` (an unwritten trait-default method) and type-checks the
  /// clone with `self_type_` bound to `target` — the concrete impl target,
  /// e.g. `file` — instead of the trait's own abstract `self` type param
  /// (`check_trait_decl` checks the trait's own copy exactly once, generic
  /// over `self`; that copy's `node_types_` entries can never resolve a
  /// concrete call like `self.write(buf)` against a real declaration). On
  /// success, registers the clone as an ordinary `method_entry` (so
  /// `find_method`/`record_instance_method_callee` treat it exactly like a
  /// real impl-provided method — see `infer_method_call`) and records it in
  /// `synthesized_trait_defaults_` so `hir::lower_module` lowers it into a
  /// real `hir_function`. On failure (a construct `ast::clone_func_decl`
  /// doesn't support — see its doc comment), reports a diagnostic rather
  /// than silently leaving the method uncallable through this impl.
  auto monomorphize_trait_default(const ast::func_decl &decl, type_id target,
                                  std::string_view target_type_name,
                                  const module_members *trait_module,
                                  file_id_type trait_file_id,
                                  std::vector<method_entry> &methods) -> void {
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
      diag_.emit(diag);
      return;
    }

    const auto *raw = cloned->get();
    synthesized_decls_.push_back(std::move(*cloned));

    const auto saved_module = module_;
    const auto saved_file_id = file_id_;
    const auto saved_self = self_type_;
    module_ = trait_module;
    file_id_ = trait_file_id;
    self_type_ = target;
    check_function(*raw, /*at_module_scope=*/false);
    module_ = saved_module;
    file_id_ = saved_file_id;
    self_type_ = saved_self;

    methods.push_back(method_entry{
        .decl = raw,
        .owner = trait_module,
        .from_trait = nullptr,
    });
    synthesized_trait_defaults_.push_back(
        synthesized_method{.decl = raw,
                          .target_type_name = std::string(target_type_name),
                          .owner_module = trait_module->module_name});
  }

  /// Looks up a method by name on a user-type instance. An inherent or
  /// impl-provided method always wins over a trait default with the same
  /// name; the default is only used as a fallback.
  auto find_method(const type_entry &instance, std::string_view name)
      -> const method_entry * {
    build_method_table();
    if (instance.decl == nullptr) {
      return nullptr;
    }
    const auto it = methods_.find(instance.decl);
    if (it == methods_.end()) {
      return nullptr;
    }
    // Inherent/impl-provided methods take priority over trait defaults,
    // which take priority over `extend`-block methods (the least specific).
    const method_entry *trait_fallback = nullptr;
    const method_entry *extension_fallback = nullptr;
    for (const auto &method : it->second) {
      if (method.decl->name != name) {
        continue;
      }
      if (method.is_extension) {
        if (extension_fallback == nullptr) {
          extension_fallback = &method;
        }
        continue;
      }
      if (method.from_trait == nullptr) {
        return &method;
      }
      if (trait_fallback == nullptr) {
        trait_fallback = &method;
      }
    }
    return trait_fallback != nullptr ? trait_fallback : extension_fallback;
  }

  /// Looks up an `extend`-block method by name on a builtin instance (one
  /// with no `type_decl`, e.g. `str`), keyed by the builtin's type-entry
  /// name via `extend_methods_by_builtin_`.
  auto find_extend_method_for_builtin(const type_entry &instance,
                                      std::string_view name)
      -> const method_entry * {
    build_method_table();
    const auto it = extend_methods_by_builtin_.find(instance.name);
    if (it == extend_methods_by_builtin_.end()) {
      return nullptr;
    }
    for (const auto &method : it->second) {
      if (method.decl->name == name) {
        return &method;
      }
    }
    return nullptr;
  }

  /// Whether `instance` implements `trait_name`, via either a `deriving`
  /// clause or a recorded `impl` (session-wide coherence index).
  auto type_has_trait(const type_entry &instance, std::string_view trait_name)
      -> bool {
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

  /// Result types of methods derived through `deriving` or prelude traits.
  auto derived_method_result(const type_entry &instance, std::string_view name)
      -> std::optional<type_id> {
    const auto has = [&](std::string_view trait_name) -> auto {
      return type_has_trait(instance, trait_name);
    };
    if (name == "show" && has("show")) {
      return types_.builtin("str");
    }
    if (name == "hash" && has("hash")) {
      return types_.builtin("uint64");
    }
    if (name == "cmp" && has("ord")) {
      return types_.builtin("ordering");
    }
    if (name == "eq" && has("eq")) {
      return types_.builtin("bool");
    }
    return std::nullopt;
  }

  // ==========================================================================
  //  Calls
  // ==========================================================================

  /// Checks a call against a `fn(...)`-typed callable value: arity must
  /// match exactly (no named arguments or defaults for a bare fn type), and
  /// each argument's type is checked positionally.
  auto check_call_against_fn_type(const ast::call_expr &call,
                                  const type_entry &fn_entry,
                                  std::string_view callee_name) -> type_id {
    if (call.args.size() != fn_entry.args.size()) {
      error(call.span,
            std::format("wrong number of arguments to `{}`: expected {}, "
                        "found {}",
                        callee_name, fn_entry.args.size(), call.args.size()),
            "argument count does not match the function type");
      infer_call_args_loosely(call);
      return fn_entry.result;
    }
    for (size_t i = 0; i < call.args.size(); ++i) {
      if (call.args[i].value == nullptr) {
        continue;
      }
      const auto expected = fn_entry.args[i];
      const auto found = infer_expr(*call.args[i].value, expected);
      type_mismatch(call.args[i].value->span, expected, found,
                    "for this argument");
    }
    return fn_entry.result;
  }

  /// Hard-codes the result types of the handful of prelude methods the
  /// checker knows about for `list[T]`, `str`, `option[T]`, and `result[T, E]`
  /// — a stopgap until the standard library itself is represented as
  /// checkable declarations. Returns `unknown` for any other object kind or
  /// unrecognized method name, which is silently accepted rather than
  /// reported as an error.
  auto builtin_method_result(const type_entry &object, std::string_view name)
      -> type_id {
    const auto element = object.args.empty() ? k_unknown_type : object.args[0];
    if (object.kind == type_kind::builtin_generic_kind &&
        object.name == "list") {
      if (name == "len") {
        return types_.builtin("usize");
      }
      if (name == "is_empty" || name == "contains" || name == "any" ||
          name == "all") {
        return types_.builtin("bool");
      }
      if (name == "push" || name == "sort" || name == "clear") {
        return types_.builtin("unit");
      }
      if (name == "pop" || name == "find" || name == "first" ||
          name == "last") {
        return types_.builtin_generic("option", {element});
      }
      if (name == "filter") {
        return types_.builtin_generic("list", {element});
      }
      return k_unknown_type;
    }
    if (object.kind == type_kind::builtin_generic_kind &&
        (object.name == "slice" || object.name == "slice_mut")) {
      if (name == "len") {
        return types_.builtin("usize");
      }
      if (name == "is_empty") {
        return types_.builtin("bool");
      }
      return k_unknown_type;
    }
    if (object.kind == type_kind::builtin_kind && object.name == "str") {
      if (name == "len") {
        return types_.builtin("usize");
      }
      if (name == "contains" || name == "starts_with" || name == "ends_with" ||
          name == "is_empty") {
        return types_.builtin("bool");
      }
      if (name == "to_uppercase" || name == "to_lowercase" ||
          name == "replace" || name == "trim" || name == "reversed") {
        return types_.builtin("str");
      }
      if (name == "split") {
        return types_.builtin_generic("list", {types_.builtin("str")});
      }
      if (name == "as_bytes") {
        return types_.builtin_generic("slice", {types_.builtin("byte")});
      }
      return k_unknown_type;
    }
    if (object.kind == type_kind::builtin_generic_kind &&
        object.name == "option") {
      if (name == "unwrap" || name == "unwrap_or") {
        return element;
      }
      if (name == "is_some" || name == "is_none") {
        return types_.builtin("bool");
      }
      return k_unknown_type;
    }
    if (object.kind == type_kind::builtin_generic_kind &&
        object.name == "result") {
      if (name == "unwrap") {
        return element;
      }
      if (name == "is_ok" || name == "is_err") {
        return types_.builtin("bool");
      }
      return k_unknown_type;
    }
    return k_unknown_type;
  }

  /// Builds the comma-separated, sorted, deduplicated method-name list
  /// used in the "provides the methods ..." note on an unknown-method error.
  auto available_method_names(const type_entry &instance) -> std::string {
    build_method_table();
    auto names = std::vector<std::string>{};
    if (instance.decl != nullptr) {
      if (const auto it = methods_.find(instance.decl); it != methods_.end()) {
        for (const auto &method : it->second) {
          names.push_back(method.decl->name);
        }
      }
    }
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());
    auto out = std::string{};
    for (const auto &name : names) {
      if (!out.empty()) {
        out += ", ";
      }
      out += std::format("`{}`", name);
    }
    return out;
  }

  /// Records a `resolved_callees_` entry for a genuine instance-method call
  /// (`object.method(args...)`, `method` a real `self`-taking declaration
  /// found via `find_method`/`find_extend_method_for_builtin`) so
  /// `hir::lower_call` can rebuild the call site directly instead of
  /// mishandling `field` as plain field access — see `resolved_callee`'s
  /// doc comment (`semantic/types.h`) for why `receiver` matters. A method
  /// with no `self` parameter (an associated function reached through
  /// `find_method`, e.g. a type-constant lookup) has no receiver to record
  /// and is left alone; `infer_qualified_call` is what actually resolves
  /// those call shapes.
  auto record_instance_method_callee(const ast::call_expr &call,
                                     const method_entry &method,
                                     std::string_view target_type_name,
                                     const ast::expr &receiver) -> void {
    if (method.decl->params.empty() ||
        param_name_of(method.decl->params.front()) != "self") {
      return;
    }
    resolved_callees_[&call] =
        resolved_callee{.decl = method.decl,
                        .owner_module = method.owner->module_name,
                        .impl_target_type = std::string(target_type_name),
                        .receiver = &receiver};
  }

  /// Types a method-call expression `object.method(args...)`: resolves
  /// `object`'s type, then tries (in order) a user-declared method, a
  /// `deriving`/prelude-trait-derived method, a callable struct field, or a
  /// builtin-type method — reporting an unknown-method error only for
  /// struct/sum/opaque objects, since builtin methods are best-effort.
  auto infer_method_call(const ast::call_expr &call,
                         const ast::field_expr &field) -> type_id {
    if (field.object == nullptr) {
      infer_call_args_loosely(call);
      return k_unknown_type;
    }

    // `int32.max`-style access on a type name never reaches method lookup.
    if (const auto type_constant = infer_type_name_member(field)) {
      infer_call_args_loosely(call);
      return *type_constant;
    }

    // A module- or type-qualified call (`std.io.open(...)`,
    // `console.println(...)`, `io_error.from(...)`) — see
    // `infer_qualified_call`'s doc comment for why this is checked here
    // rather than dispatched on `call.callee`'s own kind.
    if (const auto qualified = infer_qualified_call(call, field)) {
      return *qualified;
    }

    const auto object = strip_refs(infer_expr(*field.object, k_unknown_type));
    const auto &entry = types_.entry(object);

    switch (entry.kind) {
    case type_kind::struct_kind:
    case type_kind::sum_kind:
    case type_kind::opaque_kind: {
      if (const auto *method = find_method(entry, field.field_name)) {
        record_expr_type(field, fn_type_of(*method->decl, method->owner));
        record_instance_method_callee(call, *method, entry.name, *field.object);
        return check_call_against_decl(call, *method->decl, method->owner,
                                       file_id_, /*skip_self=*/true);
      }
      if (const auto derived = derived_method_result(entry, field.field_name)) {
        infer_call_args_loosely(call);
        return *derived;
      }
      if (const auto field_type = struct_field_type(entry, field.field_name)) {
        const auto &field_entry = types_.entry(*field_type);
        if (field_entry.kind == type_kind::fn_kind) {
          return check_call_against_fn_type(call, field_entry,
                                            field.field_name);
        }
        infer_call_args_loosely(call);
        return k_unknown_type;
      }
      auto diag = diagnostic(diagnostic_level::error,
                             std::format("no method `{}` on type `{}`",
                                         field.field_name, entry.name),
                             file_id_);
      diag.with_label(field.span, "method not found");
      if (const auto methods = available_method_names(entry);
          !methods.empty()) {
        diag.with_note(
            std::format("`{}` provides the methods {}", entry.name, methods));
      } else {
        diag.with_help(std::format(
            "Add the method in an `impl {}:` block, or implement a trait "
            "that provides it.",
            entry.name));
      }
      diag_.emit(diag);
      mark_error();
      infer_call_args_loosely(call);
      return k_error_type;
    }
    default: {
      // Builtin inherent methods take priority; an `extend` block fills in
      // only when the name isn't one of the hardcoded builtin methods.
      const auto builtin_result =
          builtin_method_result(entry, field.field_name);
      if (types_.is_unknown(builtin_result)) {
        if (const auto *method =
                find_extend_method_for_builtin(entry, field.field_name)) {
          record_expr_type(field, fn_type_of(*method->decl, method->owner));
          record_instance_method_callee(call, *method, entry.name,
                                        *field.object);
          return check_call_against_decl(call, *method->decl, method->owner,
                                         file_id_, /*skip_self=*/true);
        }
      }
      infer_call_args_loosely(call);
      return builtin_result;
    }
    }
  }

  /// Types a constructor-style conversion call `target_name(value)` (e.g.
  /// `float64(n)`), Kira's replacement for a cast operator. Reports a
  /// no-conversion-exists error when the source is a known non-numeric,
  /// non-boolean type and the target is numeric.
  auto check_conversion_call(const ast::call_expr &call,
                             std::string_view target_name) -> type_id {
    const auto target = types_.builtin(target_name);
    if (call.args.size() != 1) {
      error(call.span,
            std::format("conversion to `{}` takes exactly one value, found {}",
                        target_name, call.args.size()),
            "wrong number of arguments");
      infer_call_args_loosely(call);
      return target;
    }
    const auto &arg = call.args.front();
    if (arg.value == nullptr) {
      return target;
    }
    const auto found = strip_refs(infer_expr(*arg.value, k_unknown_type));
    if (types_.is_numeric(target) && !types_.is_unknown(found) &&
        !types_.is_numeric(found) && !types_.is_boolean(found)) {
      error_with_help(
          arg.value->span,
          std::format("no conversion from `{}` to `{}`", types_.display(found),
                      target_name),
          "cannot convert this value",
          std::format("A conversion exists when `{}` provides a `from` for "
                      "the source type; the numeric types convert between "
                      "one another.",
                      target_name));
    }
    return target;
  }

  /// Resolves a call whose callee is `object.fn_name(...)` against a real
  /// declaration when `object` names a module or a type rather than a
  /// value — a fully module-qualified free function (`std.io.open(...)`,
  /// `object` a multi-segment `module_path_expr`), a free function reached
  /// through a whole-module `use` import (`console.println(...)`, `object`
  /// a single-segment `ident_expr` naming an imported module), or a
  /// type-qualified associated function (`io_error.from(...)`, `object` a
  /// single-segment `ident_expr` naming a type) — dispatched like
  /// `find_method` but with no receiver, so a match whose first parameter is
  /// `self` is rejected (that's a genuine instance-method call, handled the
  /// existing way by the rest of `infer_method_call`). Returns `nullopt`
  /// when `object` is value-rooted or doesn't resolve to any of these,
  /// leaving the caller to fall back to today's method-call/unknown
  /// handling. On a match, records a `resolved_callee` so `hir::lower_call`
  /// can rebuild this exact call site without redoing this resolution.
  ///
  /// Called from `infer_method_call` rather than dispatched on
  /// `call.callee`'s own kind in `infer_call`, because the parser never
  /// produces a bare `module_path_expr` callee for a call — a dotted name
  /// immediately followed by `(` always parses as `field_expr` (see
  /// `parser::parse_ident_or_path_expr`), with `object` holding whatever
  /// dotted prefix, if any, preceded the call-adjacent final segment.
  auto infer_qualified_call(const ast::call_expr &call,
                            const ast::field_expr &field)
      -> std::optional<type_id> {
    const auto &object = *field.object;
    const auto fn_name = field.field_name;
    auto root = std::vector<std::string>{};
    if (object.kind == ast::node_kind::ident_expr) {
      const auto &ident = dynamic_cast<const ast::ident_expr &>(object);
      if (lookup_value(ident.name) != nullptr ||
          (ident.name == "self" && self_type_ != k_unknown_type)) {
        return std::nullopt; // a real value, not a module/type reference
      }
      root = {ident.name};
    } else if (object.kind == ast::node_kind::module_path_expr) {
      const auto &path = dynamic_cast<const ast::module_path_expr &>(object);
      if (path.segments.empty()) {
        return std::nullopt;
      }
      if (lookup_value(path.segments.front()) != nullptr ||
          (path.segments.front() == "self" && self_type_ != k_unknown_type)) {
        return std::nullopt; // a value-rooted field chain, not a module path
      }
      root = path.segments;
    } else {
      return std::nullopt;
    }

    const auto resolve_against =
        [&](const ast::func_decl &decl, const module_members *owner,
            file_id_type decl_file, std::string impl_target_type) -> type_id {
      record_expr_type(field, fn_type_of(decl, owner));
      resolved_callees_[&call] =
          resolved_callee{.decl = &decl,
                          .owner_module = owner->module_name,
                          .impl_target_type = std::move(impl_target_type)};
      return check_call_against_decl(call, decl, owner, decl_file,
                                     /*skip_self=*/false);
    };

    // Fully module-qualified: `std.io.open(...)`.
    if (const auto *owner = find_session_module_of_path(root)) {
      if (const auto it = owner->functions.find(fn_name);
          it != owner->functions.end()) {
        return resolve_against(*it->second.decl, owner, it->second.file_id, "");
      }
    }

    if (root.size() == 1) {
      // A single-segment root naming an imported module (`use std.console`,
      // then `console.println(...)`).
      if (const auto *binding = find_import(root.front());
          binding != nullptr && binding->leaf_name.empty() &&
          session_owns_path_root(binding->path)) {
        if (const auto *owner =
                index_.find_module(join_strings(binding->path, "."))) {
          if (const auto it = owner->functions.find(fn_name);
              it != owner->functions.end()) {
            return resolve_against(*it->second.decl, owner, it->second.file_id,
                                   "");
          }
        }
      }

      // A single-segment root naming a type (`io_error.from(...)`) —
      // resolve an associated (non-`self`) function the same way a method
      // call resolves an instance method.
      if (const auto found_type = find_type_decl_by_name(root.front())) {
        const auto type =
            make_user_type(*found_type->first, found_type->second, {});
        const auto &entry = types_.entry(type);
        if (const auto *method = find_method(entry, fn_name)) {
          const auto has_self =
              !method->decl->params.empty() &&
              method->decl->params.front().pattern != nullptr &&
              method->decl->params.front().pattern->kind ==
                  ast::node_kind::binding_pattern &&
              dynamic_cast<const ast::binding_pattern &>(
                  *method->decl->params.front().pattern)
                      .name == "self";
          if (!has_self) {
            return resolve_against(*method->decl, method->owner, file_id_,
                                   found_type->first->name);
          }
        }
      }
    }

    return std::nullopt;
  }

  /// Types a call expression by dispatching on its callee's shape: a method
  /// call (field-expr callee) goes to `infer_method_call`; a computed
  /// callable value is checked against its `fn(...)` type; a variant-ident
  /// callee constructs that variant; otherwise the identifier is resolved in
  /// order through local bindings, the current module's functions, imports,
  /// prelude functions (each with its own hard-coded signature), a builtin
  /// conversion call, a bare variant spelling, or an undefined-name error.
  auto infer_call(const ast::call_expr &call, type_id expected) -> type_id {
    if (call.callee == nullptr) {
      infer_call_args_loosely(call);
      return k_unknown_type;
    }

    if (call.callee->kind == ast::node_kind::field_expr) {
      return infer_method_call(
          call, dynamic_cast<const ast::field_expr &>(*call.callee));
    }

    if (call.callee->kind != ast::node_kind::ident_expr) {
      const auto callee = infer_expr(*call.callee, k_unknown_type);
      const auto &entry = types_.entry(strip_refs(callee));
      if (entry.kind == type_kind::fn_kind) {
        return check_call_against_fn_type(call, entry, "this function value");
      }
      infer_call_args_loosely(call);
      return k_unknown_type;
    }

    const auto &ident = dynamic_cast<const ast::ident_expr &>(*call.callee);
    const auto &name = ident.name;

    if (is_variant_ident(ident)) {
      if (const auto variant =
              resolve_variant_value(name, &call, call.span, expected)) {
        return *variant;
      }
      emit_undefined_variant(ident.span, name, expected);
      infer_call_args_loosely(call);
      return k_error_type;
    }

    if (const auto *binding = lookup_value(name)) {
      const auto &entry = types_.entry(strip_refs(binding->type));
      if (entry.kind == type_kind::fn_kind) {
        // The callee ident is resolved straight from the scope binding
        // here, not through `infer_expr`, so (like the other direct-
        // resolution bypasses this chokepoint doc comment lists) it needs
        // its own persistence hook.
        record_expr_type(ident, binding->type);
        return check_call_against_fn_type(call, entry, name);
      }
      if (types_.is_unknown(binding->type)) {
        infer_call_args_loosely(call);
        return k_unknown_type;
      }
      error_with_help(
          call.span, std::format("`{}` is not callable", name),
          std::format("this has type `{}`", types_.display(binding->type)),
          "Only functions, lambdas, and callable values can be called.");
      infer_call_args_loosely(call);
      return k_error_type;
    }

    if (module_ != nullptr) {
      if (const auto it = module_->functions.find(name);
          it != module_->functions.end()) {
        record_expr_type(ident, fn_type_of(*it->second.decl, module_));
        return check_call_against_decl(call, *it->second.decl, module_,
                                       it->second.file_id,
                                       /*skip_self=*/false);
      }
    }

    if (const auto *import = find_import(name)) {
      if (const auto *source = import_source_module(*import)) {
        const auto member = imported_member_name(*import);
        if (const auto it = source->functions.find(member);
            it != source->functions.end()) {
          record_expr_type(ident, fn_type_of(*it->second.decl, source));
          return check_call_against_decl(call, *it->second.decl, source,
                                         it->second.file_id,
                                         /*skip_self=*/false);
        }
        if (const auto it = source->types.find(member);
            it != source->types.end()) {
          infer_call_args_loosely(call);
          return k_unknown_type;
        }
      }
      infer_call_args_loosely(call);
      return k_unknown_type;
    }

    // Prelude functions. `println`/`print` are lowered directly to the
    // `rt_write`/`rt_stdout` intrinsics by `hir::lower_call` (see its doc
    // comment) rather than resolved against a real declaration here, so —
    // unlike every other branch in this function — the callee `ident` is
    // deliberately left unrecorded in `node_types_`: that absence is what
    // lowering keys off of to recognize the magic form.
    if (name == "println" || name == "print") {
      infer_call_args_loosely(call);
      return types_.builtin("unit");
    }
    if (name == "panic") {
      infer_call_args_loosely(call);
      return types_.builtin("never");
    }
    if (name == "assert") {
      if (!call.args.empty() && call.args.front().value != nullptr) {
        require_bool(*call.args.front().value, "the `assert` condition");
      }
      for (size_t i = 1; i < call.args.size(); ++i) {
        if (call.args[i].value != nullptr) {
          infer_expr(*call.args[i].value, k_unknown_type);
        }
      }
      return types_.builtin("unit");
    }
    if (name == "size_of") {
      infer_call_args_loosely(call);
      return types_.builtin("usize");
    }
    if (name == "args") {
      infer_call_args_loosely(call);
      return types_.builtin_generic("list", {types_.builtin("str")});
    }
    if (name == "env") {
      infer_call_args_loosely(call);
      return types_.builtin_generic("option", {types_.builtin("str")});
    }
    if (name == "min" || name == "max") {
      auto first = k_unknown_type;
      for (const auto &arg : call.args) {
        if (arg.value != nullptr) {
          const auto found = infer_expr(*arg.value, first);
          if (first == k_unknown_type) {
            first = found;
          }
        }
      }
      return first;
    }

    if (is_builtin_scalar_name(name)) {
      return check_conversion_call(call, name);
    }

    if (const auto variant =
            resolve_variant_value(name, &call, call.span, expected)) {
      return *variant;
    }

    if (is_prelude_value_name(name) || is_type_like_name(name) ||
        builtin_generic_arity(name).has_value()) {
      infer_call_args_loosely(call);
      return k_unknown_type;
    }

    if (file_has_external_wildcard_) {
      infer_call_args_loosely(call);
      return k_unknown_type;
    }

    emit_undefined_name(ident.span, name);
    infer_call_args_loosely(call);
    return k_error_type;
  }

  // ==========================================================================
  //  Field access and indexing
  // ==========================================================================

  /// `int32.max` / `int32.min` — constants on a numeric type name.
  auto infer_type_name_member(const ast::field_expr &field)
      -> std::optional<type_id> {
    if (field.object == nullptr ||
        field.object->kind != ast::node_kind::ident_expr) {
      return std::nullopt;
    }
    const auto &ident = dynamic_cast<const ast::ident_expr &>(*field.object);
    if (lookup_value(ident.name) != nullptr ||
        !is_builtin_scalar_name(ident.name)) {
      return std::nullopt;
    }
    const auto type = types_.builtin(ident.name);
    if ((field.field_name == "max" || field.field_name == "min") &&
        types_.is_numeric(type)) {
      return type;
    }
    return k_unknown_type; // other type-level members (reflection, try_from)
  }

  /// Builds the comma-separated field-name list used in the "has the
  /// fields ..." note on an unknown-field error.
  auto struct_field_names(const type_entry &instance) -> std::string {
    auto out = std::string{};
    if (const auto *fields = struct_fields_of(instance)) {
      for (const auto &field : *fields) {
        if (!out.empty()) {
          out += ", ";
        }
        out += std::format("`{}`", field.name);
      }
    }
    return out;
  }

  /// Shared field-access typing for `expr.name`, whether it arrived as a
  /// `field_expr` or as a value-rooted dotted path.
  auto field_access_type(type_id object, std::string_view name,
                         source_span span) -> type_id {
    const auto stripped = strip_refs(object);
    const auto &entry = types_.entry(stripped);

    switch (entry.kind) {
    case type_kind::struct_kind: {
      if (const auto field_type = struct_field_type(entry, name)) {
        return *field_type;
      }
      if (const auto *method = find_method(entry, name)) {
        return fn_type_of(*method->decl, method->owner);
      }
      auto diag = diagnostic(
          diagnostic_level::error,
          std::format("no field `{}` on struct `{}`", name, entry.name),
          file_id_);
      diag.with_label(span, "unknown field");
      if (const auto fields = struct_field_names(entry); !fields.empty()) {
        diag.with_note(
            std::format("`{}` has the fields {}", entry.name, fields));
      }
      diag_.emit(diag);
      mark_error();
      return k_error_type;
    }
    case type_kind::sum_kind: {
      if (const auto *method = find_method(entry, name)) {
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
    default: {
      const auto builtin_result = builtin_method_result(entry, name);
      if (types_.is_unknown(builtin_result)) {
        if (const auto *method = find_extend_method_for_builtin(entry, name)) {
          return fn_type_of(*method->decl, method->owner);
        }
      }
      return builtin_result;
    }
    }
  }

  /// Types a plain (non-call) field access `object.name`.
  auto infer_field(const ast::field_expr &field) -> type_id {
    if (field.object == nullptr) {
      return k_unknown_type;
    }
    if (const auto type_constant = infer_type_name_member(field)) {
      return *type_constant;
    }
    const auto object = infer_expr(*field.object, k_unknown_type);
    return field_access_type(object, field.field_name, field.span);
  }

  /// A dotted path whose first segment names an in-scope value is a chain of
  /// field accesses, not a module reference.
  auto infer_module_path(const ast::module_path_expr &path) -> type_id {
    if (path.segments.empty()) {
      return k_unknown_type;
    }
    const auto &root = path.segments.front();

    auto base = k_unknown_type;
    auto value_rooted = false;
    if (const auto *binding = lookup_value(root)) {
      base = binding->type;
      value_rooted = true;
    } else if (root == "self" && self_type_ != k_unknown_type) {
      base = self_type_;
      value_rooted = true;
    } else if (path.segments.size() == 2 && is_builtin_scalar_name(root)) {
      const auto type = types_.builtin(root);
      const auto &member = path.segments.back();
      if ((member == "max" || member == "min") && types_.is_numeric(type)) {
        return type;
      }
      return k_unknown_type;
    }

    if (!value_rooted) {
      return k_unknown_type; // module references are validated elsewhere
    }
    for (size_t i = 1; i < path.segments.size(); ++i) {
      base = field_access_type(base, path.segments[i], path.span);
    }
    return base;
  }

  /// Whether an identifier in callee/index position names a function or type
  /// rather than a runtime value (so `name[T]` is generic instantiation).
  auto ident_names_callable_decl(const ast::ident_expr &ident) -> bool {
    if (lookup_value(ident.name) != nullptr) {
      return false;
    }
    if (module_ != nullptr && (module_->functions.contains(ident.name) ||
                               module_->types.contains(ident.name) ||
                               module_->traits.contains(ident.name) ||
                               module_->concepts.contains(ident.name))) {
      return true;
    }
    return is_prelude_value_name(ident.name) ||
           is_builtin_scalar_name(ident.name) ||
           builtin_generic_arity(ident.name).has_value() ||
           find_import(ident.name) != nullptr;
  }

  /// Types an indexing expression `object[key]`. Distinguishes generic
  /// instantiation (`size_of[usize]`) from real indexing via
  /// `ident_names_callable_decl`, then, for `array`/`list`/`slice`/`str`,
  /// requires an integer key (except for a range key, which produces a
  /// slice/substring rather than a single element).
  auto infer_index(const ast::index_expr &index) -> type_id {
    if (index.object == nullptr) {
      return k_unknown_type;
    }

    // `size_of[usize]` / `index[n]` — generic instantiation, not indexing.
    if (index.object->kind == ast::node_kind::ident_expr &&
        ident_names_callable_decl(
            dynamic_cast<const ast::ident_expr &>(*index.object))) {
      // A bare identifier naming an in-scope generic parameter (e.g. `T` in
      // `size_of[T]()` inside a concept/generic bound) is a type argument,
      // not a value reference — skip ordinary name resolution so it doesn't
      // spuriously report "undefined name".
      const auto is_type_param_arg =
          index.index != nullptr &&
          index.index->kind == ast::node_kind::ident_expr &&
          lookup_type_param(
              dynamic_cast<const ast::ident_expr &>(*index.index).name)
              .has_value();
      if (index.index != nullptr && !is_type_param_arg) {
        infer_expr(*index.index, k_unknown_type);
      }
      return k_unknown_type;
    }

    const auto object = strip_refs(infer_expr(*index.object, k_unknown_type));
    const auto &entry = types_.entry(object);
    const auto key =
        index.index != nullptr
            ? strip_refs(infer_expr(*index.index, types_.builtin("usize")))
            : k_unknown_type;
    const auto key_is_range =
        types_.entry(key).kind == type_kind::builtin_generic_kind &&
        types_.entry(key).name == "range";

    const auto require_integer_key = [&] -> void {
      if (!key_is_range && !types_.is_unknown(key) && !types_.is_integer(key)) {
        error(index.index != nullptr ? index.index->span : index.span,
              std::format("index must be an integer type, found `{}`",
                          types_.display(key)),
              "expected an integer index");
      }
    };

    switch (entry.kind) {
    case type_kind::array_kind:
      require_integer_key();
      return key_is_range ? types_.builtin_generic("slice", {entry.result})
                          : entry.result;
    case type_kind::builtin_generic_kind: {
      if (entry.name == "list" || entry.name == "slice" ||
          entry.name == "slice_mut") {
        require_integer_key();
        const auto element =
            entry.args.empty() ? k_unknown_type : entry.args[0];
        // A range-sliced `slice_mut` stays a `slice_mut` (a sub-view of a
        // mutable slice is still mutable) — `list`/`slice` range-slice to a
        // plain (immutable) `slice`, same as before.
        if (key_is_range) {
          return types_.builtin_generic(
              entry.name == "slice_mut" ? "slice_mut" : "slice", {element});
        }
        return element;
      }
      return k_unknown_type;
    }
    case type_kind::builtin_kind: {
      if (entry.name == "str") {
        return key_is_range ? object : k_unknown_type;
      }
      if (types_.is_numeric(object) || types_.is_boolean(object)) {
        error(index.span,
              std::format("cannot index a value of type `{}`",
                          types_.display(object)),
              "not an indexable value");
        return k_error_type;
      }
      return k_unknown_type;
    }
    default:
      return k_unknown_type;
    }
  }

  // ==========================================================================
  //  Struct literals
  // ==========================================================================

  /// Finds a `type` declaration by name in the current module or
  /// reachable through a non-wildcard import; returns the declaration paired
  /// with its owning module's name.
  auto find_type_decl_by_name(std::string_view name)
      -> std::optional<std::pair<const ast::type_decl *, std::string>> {
    if (module_ != nullptr) {
      if (const auto it = module_->types.find(std::string(name));
          it != module_->types.end()) {
        return std::pair{it->second.decl, module_->module_name};
      }
    }
    if (const auto *binding = find_import(name)) {
      if (const auto *source = import_source_module(*binding)) {
        if (const auto it = source->types.find(imported_member_name(*binding));
            it != source->types.end()) {
          return std::pair{it->second.decl, source->module_name};
        }
      }
    }
    return std::nullopt;
  }

  /// Types a struct literal `Type { field: value, ... }` (or bare `{ ... }`
  /// against an expected struct type). Reports duplicate/unknown/missing
  /// fields and checks each field value's type; when the target is a
  /// generic struct instantiated with no explicit arguments, infers them
  /// from field values whose declared type names a bare type parameter.
  auto check_struct_literal(const ast::struct_expr &expr, type_id expected)
      -> type_id {
    auto target = k_unknown_type;

    if (expr.type_name != nullptr) {
      if (expr.type_name->kind == ast::node_kind::ident_expr) {
        const auto &ident =
            dynamic_cast<const ast::ident_expr &>(*expr.type_name);
        if (const auto found = find_type_decl_by_name(ident.name)) {
          target = make_user_type(*found->first, found->second, {});
        } else if (!file_has_external_wildcard_ &&
                   !is_builtin_scalar_name(ident.name) &&
                   !builtin_generic_arity(ident.name).has_value() &&
                   find_import(ident.name) == nullptr) {
          if (reported_undefined_.insert(std::format("type:{}", ident.name))
                  .second) {
            error_with_help(
                expr.type_name->span,
                std::format("undefined type `{}`", ident.name),
                "no struct type with this name is in scope",
                std::format("Declare `type {} = {{ ... }}` or import it with "
                            "`use`.",
                            ident.name));
          }
          target = k_error_type;
        }
      }
      // Qualified struct heads are validated by the qualified-path pass.
    } else if (types_.entry(strip_refs(expected)).kind ==
               type_kind::struct_kind) {
      target = strip_refs(expected);
    }

    const auto &entry = types_.entry(target);
    const auto *fields = struct_fields_of(entry);
    if (fields == nullptr) {
      for (const auto &field : expr.fields) {
        if (field.value != nullptr) {
          infer_expr(*field.value, k_unknown_type);
        }
      }
      return target == k_error_type ? k_error_type : target;
    }

    const auto bindings = param_bindings_for_instance(entry);
    const auto member_ctx = member_resolve_ctx(entry, bindings);

    auto initialized = std::unordered_set<std::string>{};
    auto inferred_by_field = std::unordered_map<std::string, type_id>{};

    for (const auto &field : expr.fields) {
      if (!initialized.insert(field.name).second) {
        error(
            field.span,
            std::format("field `{}` is initialized more than once", field.name),
            "duplicate field");
        continue;
      }
      const ast::struct_field *declared = nullptr;
      for (const auto &candidate : *fields) {
        if (candidate.name == field.name) {
          declared = &candidate;
          break;
        }
      }
      if (declared == nullptr) {
        auto diag = diagnostic(
            diagnostic_level::error,
            std::format("unknown field `{}` in struct literal for `{}`",
                        field.name, entry.name),
            file_id_);
        diag.with_label(field.span, "no such field");
        if (const auto names = struct_field_names(entry); !names.empty()) {
          diag.with_note(
              std::format("`{}` has the fields {}", entry.name, names));
        }
        diag_.emit(diag);
        mark_error();
        if (field.value != nullptr) {
          infer_expr(*field.value, k_unknown_type);
        }
        continue;
      }
      const auto field_expected =
          declared->type != nullptr ? resolve_type(*declared->type, member_ctx)
                                    : k_unknown_type;
      auto found = k_unknown_type;
      if (field.value != nullptr) {
        found = infer_expr(*field.value, field_expected);
        type_mismatch(field.value->span, field_expected, found,
                      "for this field");
      } else if (const auto *binding = lookup_value(field.name)) {
        // Shorthand `{name}` binds the in-scope value of the same name.
        found = binding->type;
        type_mismatch(field.span, field_expected, found, "for this field");
      }
      // Recorded unconditionally (even when neither branch above ran, in
      // which case `found` is still `k_unknown_type`) so lowering's lookup
      // is a plain `.find` — see `struct_literal_field_types_`'s doc
      // comment.
      struct_literal_field_types_[&field] = found;
      inferred_by_field.emplace(field.name, found);
    }

    auto missing = std::string{};
    for (const auto &field : *fields) {
      if (!initialized.contains(field.name)) {
        if (!missing.empty()) {
          missing += ", ";
        }
        missing += std::format("`{}`", field.name);
      }
    }
    if (!missing.empty()) {
      error_with_help(
          expr.span,
          std::format("missing field{} {} in struct literal for `{}`",
                      missing.find(',') != std::string::npos ? "s" : "",
                      missing, entry.name),
          "struct literal is incomplete",
          "Every struct field must be initialized; Kira structs have no "
          "default field values.");
    }

    // Instantiate generic structs from fields whose declared type is a bare
    // type parameter.
    if (entry.decl != nullptr && !entry.decl->type_params.empty() &&
        entry.args.empty()) {
      auto args = std::vector<type_id>{};
      for (const auto &param : entry.decl->type_params) {
        auto bound = k_unknown_type;
        for (const auto &field : *fields) {
          if (field.type != nullptr &&
              field.type->kind == ast::node_kind::named_type) {
            const auto &named =
                dynamic_cast<const ast::named_type &>(*field.type);
            if (named.path.size() == 1 && named.path.front() == param.name) {
              if (const auto it = inferred_by_field.find(field.name);
                  it != inferred_by_field.end()) {
                bound = it->second;
                break;
              }
            }
          }
        }
        args.push_back(bound);
      }
      return make_user_type(*entry.decl, entry.module_name, std::move(args));
    }

    return target;
  }

  // ==========================================================================
  //  Blocks and body values
  // ==========================================================================

  /// Checks a list of body nodes in a fresh scope and returns the value type
  /// of the final expression statement (`unit` otherwise).
  auto check_body_nodes(const std::vector<ast::ptr<ast::node>> &items,
                        type_id expected_tail) -> type_id {
    push_scope();
    auto last = types_.builtin("unit");
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i] == nullptr) {
        continue;
      }
      const auto is_last = i + 1 == items.size();
      last =
          check_body_node(*items[i], is_last ? expected_tail : k_unknown_type);
      if (!is_last) {
        last = types_.builtin("unit");
      }
    }
    pop_scope();
    return last;
  }

  /// Types a lambda expression. Parameter types come from an explicit
  /// annotation, or (failing that) from the corresponding slot of an
  /// expected `fn(...)` type; the declared/expected return type is checked
  /// against the body's inferred type. Always yields a concrete `fn(...)`
  /// type — Kira monomorphizes closures, so there is no separate closure
  /// type distinct from the function-value type it's assigned/passed as.
  auto infer_lambda(const ast::lambda_expr &lambda, type_id expected)
      -> type_id {
    const auto &expected_entry = types_.entry(strip_refs(expected));
    const auto expected_is_fn = expected_entry.kind == type_kind::fn_kind;

    push_scope();
    auto param_types = std::vector<type_id>{};
    for (size_t i = 0; i < lambda.params.size(); ++i) {
      const auto &param = lambda.params[i];
      auto type = k_unknown_type;
      if (param.type_annotation != nullptr) {
        type = resolve_type(*param.type_annotation, current_resolve_ctx());
      } else if (expected_is_fn && i < expected_entry.args.size()) {
        type = expected_entry.args[i];
      }
      param_types.push_back(type);
      if (param.pattern != nullptr &&
          param.pattern->kind == ast::node_kind::binding_pattern) {
        const auto &binding =
            dynamic_cast<const ast::binding_pattern &>(*param.pattern);
        bind_value(binding.name, type, binding_origin::parameter, param.span);
      } else if (param.pattern != nullptr) {
        check_pattern(dynamic_cast<const ast::pattern &>(*param.pattern), type);
      }
    }

    auto declared_result = k_unknown_type;
    if (lambda.return_type != nullptr) {
      declared_result =
          resolve_type(*lambda.return_type, current_resolve_ctx());
    } else if (expected_is_fn) {
      declared_result = expected_entry.result;
    }

    auto body_result = k_unknown_type;
    if (lambda.body_expr != nullptr) {
      body_result = infer_expr(*lambda.body_expr, declared_result);
      if (declared_result != k_unknown_type) {
        type_mismatch(lambda.body_expr->span, declared_result, body_result,
                      "as the lambda result");
      }
    } else {
      body_result = check_body_nodes(lambda.body_stmts, declared_result);
    }
    pop_scope();

    const auto result =
        declared_result != k_unknown_type ? declared_result : body_result;
    return types_.fn_of(std::move(param_types), result);
  }

  /// Checks an `if`/`elif` branch's condition (requiring `bool`) or, for an
  /// `if let` branch, infers the scrutinee and checks the pattern against it.
  auto check_if_branch_header(const ast::if_branch &branch) -> void {
    if (branch.condition != nullptr) {
      require_bool(*branch.condition, "an `if` condition");
    }
    if (branch.let_expr != nullptr && branch.let_pattern != nullptr) {
      const auto subject = infer_expr(*branch.let_expr, k_unknown_type);
      check_pattern(dynamic_cast<const ast::pattern &>(*branch.let_pattern),
                    strip_refs(subject));
    } else if (branch.let_expr != nullptr) {
      infer_expr(*branch.let_expr, k_unknown_type);
    }
  }

  /// Joins branch value types, diagnosing genuinely conflicting branches.
  auto join_branch_type(type_id current, type_id found, source_span span,
                        std::string_view construct) -> type_id {
    if (types_.is_unknown(current) || types_.entry(found).name == "never") {
      return types_.is_unknown(current) ? found : current;
    }
    if (types_.is_unknown(found)) {
      return current;
    }
    if (!types_.compatible(current, found) &&
        !types_.compatible(found, current)) {
      error_with_help(
          span,
          std::format("{} branches have mismatched types: `{}` vs `{}`",
                      construct, types_.display(current),
                      types_.display(found)),
          std::format("this branch produces `{}`", types_.display(found)),
          "Every branch of a value-producing conditional must produce the "
          "same type.");
      return current;
    }
    return current;
  }

  /// Types an `if`/`elif`/`else` expression: each branch's tail value is
  /// joined via `join_branch_type`, reporting a mismatch if any two
  /// branches produce genuinely incompatible types. An `if` with no `else`
  /// still yields the joined branch type, since exhaustiveness for boolean
  /// conditions is not enforced the way `match` exhaustiveness is.
  auto infer_if_expr(const ast::if_expr &expr, type_id expected) -> type_id {
    auto result = expected;
    for (const auto &branch : expr.branches) {
      check_if_branch_header(branch);
      push_scope();
      if (branch.let_pattern != nullptr && branch.let_expr == nullptr) {
        check_pattern(dynamic_cast<const ast::pattern &>(*branch.let_pattern),
                      k_unknown_type);
      }
      const auto branch_type = check_body_nodes(branch.body, expected);
      pop_scope();
      result =
          join_branch_type(result, branch_type,
                           branch.body.empty() || branch.body.back() == nullptr
                               ? branch.span
                               : branch.body.back()->span,
                           "`if`");
    }
    if (!expr.else_body.empty()) {
      const auto else_type = check_body_nodes(expr.else_body, expected);
      result = join_branch_type(result, else_type,
                                expr.else_body.back() != nullptr
                                    ? expr.else_body.back()->span
                                    : expr.span,
                                "`if`");
    }
    return result;
  }

  /// Types a `for ... => yield` comprehension expression: each clause's
  /// iterable is unwrapped to its element type via `element_type_of` and
  /// bound to that clause's pattern(s), the optional guard must be `bool`,
  /// and the result is always `list[yield_type]`.
  auto infer_for_expr(const ast::for_expr &expr) -> type_id {
    push_scope();
    for (const auto &clause : expr.clauses) {
      auto element = k_unknown_type;
      if (clause.iterable != nullptr) {
        const auto iterable = infer_expr(*clause.iterable, k_unknown_type);
        element = element_type_of(iterable, clause.iterable->span);
      }
      // Multiple patterns per clause destructure tuple elements positionally.
      if (clause.patterns.size() == 1) {
        if (clause.patterns.front() != nullptr) {
          check_pattern(
              dynamic_cast<const ast::pattern &>(*clause.patterns.front()),
              element);
        }
      } else {
        for (const auto &pattern : clause.patterns) {
          if (pattern != nullptr) {
            check_pattern(dynamic_cast<const ast::pattern &>(*pattern),
                          k_unknown_type);
          }
        }
      }
    }
    if (expr.guard != nullptr) {
      require_bool(*expr.guard, "a `for` guard");
    }
    auto yield_type = k_unknown_type;
    if (expr.yield_expr != nullptr) {
      yield_type = infer_expr(*expr.yield_expr, k_unknown_type);
    }
    pop_scope();
    return types_.builtin_generic("list", {yield_type});
  }

  /// Types `expr?`: the operand must be `result`/`option`, and (when the
  /// enclosing function's return type is known) that return type must also
  /// be `result`/`option` for the early-return side of `?` to make sense.
  /// Yields the wrapped success type.
  auto infer_try(const ast::try_expr &expr) -> type_id {
    if (expr.operand == nullptr) {
      return k_unknown_type;
    }
    const auto operand = strip_refs(infer_expr(*expr.operand, k_unknown_type));
    const auto &entry = types_.entry(operand);

    const auto is_wrapper = entry.kind == type_kind::builtin_generic_kind &&
                            (entry.name == "result" || entry.name == "option");
    if (!is_wrapper && !types_.is_unknown(operand)) {
      error_with_help(
          expr.span,
          std::format("`?` requires a `result` or `option` value, found `{}`",
                      types_.display(operand)),
          "cannot propagate from this value",
          "`?` unwraps `@ok`/`@some` and returns `@err`/`@none` early; only "
          "`result` and `option` values support it.");
      return k_error_type;
    }

    if (return_annotated_) {
      const auto &return_entry = types_.entry(strip_refs(return_type_));
      const auto returns_wrapper =
          return_entry.kind == type_kind::builtin_generic_kind &&
          (return_entry.name == "result" || return_entry.name == "option");
      if (!returns_wrapper && !types_.is_unknown(return_type_)) {
        error_with_help(
            expr.span,
            std::format("cannot use `?` in a function that returns `{}`",
                        types_.display(return_type_)),
            "`?` needs a `result` or `option` return type to propagate into",
            "Change the function to return `result[T, E]` (or `option[T]`), "
            "or handle the failure here with `match`.");
      }
    }

    return is_wrapper && !entry.args.empty() ? entry.args[0] : k_unknown_type;
  }

  /// Types `await expr` (or bare `await yield`, which yields `unit`).
  /// Unwraps a `task[T, E, C]` operand to `result[T, E]` (or plain `T` when
  /// the task cannot fail); any other operand type passes through
  /// unchanged, since full task/context typing is not yet modeled.
  auto infer_await(const ast::await_expr &expr) -> type_id {
    if (expr.operand == nullptr) {
      return types_.builtin("unit"); // `await yield`
    }
    const auto operand = strip_refs(infer_expr(*expr.operand, k_unknown_type));
    const auto &entry = types_.entry(operand);
    if (entry.kind == type_kind::builtin_generic_kind && entry.name == "task") {
      if (entry.args.size() >= 2 && !types_.is_unknown(entry.args[1])) {
        return types_.builtin_generic("result", {entry.args[0], entry.args[1]});
      }
      return entry.args.empty() ? k_unknown_type : entry.args[0];
    }
    return operand; // awaiting a non-task is validated once tasks are typed
  }

  /// Returns the element type yielded by iterating `iterable` (array/list/
  /// slice/range/option element, or `char` for `str`), used by `for`
  /// loops/comprehensions. Reports a not-iterable error for a known
  /// non-iterable scalar type.
  auto element_type_of(type_id iterable, source_span span) -> type_id {
    const auto stripped = strip_refs(iterable);
    const auto &entry = types_.entry(stripped);
    switch (entry.kind) {
    case type_kind::array_kind:
      return entry.result;
    case type_kind::builtin_generic_kind:
      if (entry.name == "list" || entry.name == "slice" ||
          entry.name == "slice_mut" || entry.name == "range" ||
          entry.name == "option") {
        return entry.args.empty() ? k_unknown_type : entry.args[0];
      }
      return k_unknown_type;
    case type_kind::builtin_kind:
      if (entry.name == "str") {
        return types_.builtin("char");
      }
      if (types_.is_numeric(stripped) || types_.is_boolean(stripped)) {
        error_with_help(
            span,
            std::format("cannot iterate over a value of type `{}`",
                        types_.display(stripped)),
            "not an iterable value",
            "Iterate over a collection or a range such as `0..10`.");
        return k_error_type;
      }
      return k_unknown_type;
    default:
      return k_unknown_type;
    }
  }

  // ==========================================================================
  //  Expression dispatch
  // ==========================================================================

  /// Records `type` as `node`'s resolved type in `node_types_` and returns
  /// it unchanged, so a call site can wrap its result in one expression.
  auto record_expr_type(const ast::node &node, type_id type) -> type_id {
    node_types_[&node] = type;
    return type;
  }

  /// The central expression-typing dispatcher: infers `expr`'s type given an
  /// `expected` type (used to disambiguate literals, propagate generic
  /// arguments into a lambda, and check tail-expression compatibility),
  /// dispatching to the dedicated `infer_*`/`check_*` function for its
  /// concrete node kind. Thin wrapper around `infer_expr_impl` that records
  /// every visited expression's resolved type into `node_types_`, so this is
  /// the one place that needs to — every `infer_*` helper is reached only
  /// through here (recursively, for nested expressions too).
  auto infer_expr(const ast::expr &expr, type_id expected) -> type_id {
    return record_expr_type(expr, infer_expr_impl(expr, expected));
  }

  auto infer_expr_impl(const ast::expr &expr, type_id expected) -> type_id {
    if (expr.has_error) {
      return k_unknown_type;
    }

    switch (expr.kind) {
    case ast::node_kind::ident_expr:
      return resolve_ident(dynamic_cast<const ast::ident_expr &>(expr),
                           expected);
    case ast::node_kind::literal_expr:
      return infer_literal(dynamic_cast<const ast::literal_expr &>(expr),
                           expected);
    case ast::node_kind::binary_expr:
      return infer_binary(dynamic_cast<const ast::binary_expr &>(expr),
                          expected);
    case ast::node_kind::unary_expr:
      return infer_unary(dynamic_cast<const ast::unary_expr &>(expr), expected);
    case ast::node_kind::call_expr:
      return infer_call(dynamic_cast<const ast::call_expr &>(expr), expected);
    case ast::node_kind::index_expr:
      return infer_index(dynamic_cast<const ast::index_expr &>(expr));
    case ast::node_kind::field_expr:
      return infer_field(dynamic_cast<const ast::field_expr &>(expr));
    case ast::node_kind::cast_expr: {
      const auto &cast = dynamic_cast<const ast::cast_expr &>(expr);
      if (cast.operand != nullptr) {
        infer_expr(*cast.operand, k_unknown_type);
      }
      return cast.target_type != nullptr
                 ? resolve_type(*cast.target_type, current_resolve_ctx())
                 : k_unknown_type;
    }
    case ast::node_kind::try_expr:
      return infer_try(dynamic_cast<const ast::try_expr &>(expr));
    case ast::node_kind::tuple_expr: {
      const auto &tuple = dynamic_cast<const ast::tuple_expr &>(expr);
      const auto &expected_entry = types_.entry(strip_refs(expected));
      auto elements = std::vector<type_id>{};
      elements.reserve(tuple.elements.size());
      for (size_t i = 0; i < tuple.elements.size(); ++i) {
        const auto element_expected =
            expected_entry.kind == type_kind::tuple_kind &&
                    i < expected_entry.args.size()
                ? expected_entry.args[i]
                : k_unknown_type;
        elements.push_back(
            tuple.elements[i] != nullptr
                ? infer_expr(*tuple.elements[i], element_expected)
                : k_unknown_type);
      }
      return types_.tuple_of(std::move(elements));
    }
    case ast::node_kind::array_expr:
      return infer_array(dynamic_cast<const ast::array_expr &>(expr), expected);
    case ast::node_kind::struct_expr:
      return check_struct_literal(dynamic_cast<const ast::struct_expr &>(expr),
                                  expected);
    case ast::node_kind::lambda_expr:
      return infer_lambda(dynamic_cast<const ast::lambda_expr &>(expr),
                          expected);
    case ast::node_kind::match_expr: {
      const auto &match = dynamic_cast<const ast::match_expr &>(expr);
      return check_match(match.subject.get(), match.arms, expected, match.span);
    }
    case ast::node_kind::if_expr:
      return infer_if_expr(dynamic_cast<const ast::if_expr &>(expr), expected);
    case ast::node_kind::for_expr:
      return infer_for_expr(dynamic_cast<const ast::for_expr &>(expr));
    case ast::node_kind::await_expr:
      return infer_await(dynamic_cast<const ast::await_expr &>(expr));
    case ast::node_kind::async_expr:
      check_body_nodes(dynamic_cast<const ast::async_expr &>(expr).body,
                       k_unknown_type);
      return k_unknown_type;
    case ast::node_kind::par_expr: {
      const auto &par = dynamic_cast<const ast::par_expr &>(expr);
      auto elements = std::vector<type_id>{};
      for (const auto &branch : par.branches) {
        elements.push_back(branch != nullptr
                               ? infer_expr(*branch, k_unknown_type)
                               : k_unknown_type);
      }
      return types_.tuple_of(std::move(elements));
    }
    case ast::node_kind::race_expr: {
      const auto &race = dynamic_cast<const ast::race_expr &>(expr);
      auto result = k_unknown_type;
      for (const auto &branch : race.branches) {
        if (branch != nullptr) {
          result = infer_expr(*branch, k_unknown_type);
        }
      }
      return result;
    }
    case ast::node_kind::crew_expr: {
      const auto &crew = dynamic_cast<const ast::crew_expr &>(expr);
      push_scope();
      bind_value(crew.name, k_unknown_type, binding_origin::synthetic,
                 crew.span);
      check_body_nodes(crew.body, k_unknown_type);
      pop_scope();
      return k_unknown_type;
    }
    case ast::node_kind::on_expr: {
      const auto &on = dynamic_cast<const ast::on_expr &>(expr);
      if (on.sender != nullptr) {
        infer_expr(*on.sender, k_unknown_type);
      }
      return check_body_nodes(on.body, expected);
    }
    case ast::node_kind::block_expr:
      return check_body_nodes(dynamic_cast<const ast::block_expr &>(expr).stmts,
                              expected);
    case ast::node_kind::quote_expr:
      return types_.builtin("expr");
    case ast::node_kind::splice_expr: {
      const auto &splice = dynamic_cast<const ast::splice_expr &>(expr);
      if (splice.operand != nullptr) {
        infer_expr(*splice.operand, k_unknown_type);
      }
      return k_unknown_type;
    }
    case ast::node_kind::static_expr: {
      const auto &inner = dynamic_cast<const ast::static_expr &>(expr);
      return inner.operand != nullptr ? infer_expr(*inner.operand, expected)
                                      : k_unknown_type;
    }
    case ast::node_kind::module_path_expr:
      return infer_module_path(
          dynamic_cast<const ast::module_path_expr &>(expr));
    case ast::node_kind::group_expr: {
      const auto &group = dynamic_cast<const ast::group_expr &>(expr);
      return group.inner != nullptr ? infer_expr(*group.inner, expected)
                                    : k_unknown_type;
    }
    case ast::node_kind::where_expr: {
      const auto &where = dynamic_cast<const ast::where_expr &>(expr);
      push_scope();
      for (const auto &binding : where.bindings) {
        auto type = k_unknown_type;
        if (binding.value != nullptr) {
          type = infer_expr(*binding.value, k_unknown_type);
        }
        bind_value(binding.name, type, binding_origin::let_binding,
                   binding.span);
      }
      const auto result = where.inner != nullptr
                              ? infer_expr(*where.inner, expected)
                              : k_unknown_type;
      pop_scope();
      return result;
    }
    default:
      return k_unknown_type;
    }
  }

  /// Types an array literal, either the explicit-list form `[a, b, c]` or
  /// the fill form `[val; count]`. The expected type (an `array[T, n]` or a
  /// `list[T]`/`slice[T]`) determines the element type and result container;
  /// with no useful expectation, an explicit list defaults to `list[T]` with
  /// `T` inferred from its first element, checking the rest against it.
  auto infer_array(const ast::array_expr &array, type_id expected) -> type_id {
    const auto &expected_entry = types_.entry(strip_refs(expected));
    auto element_expected = k_unknown_type;
    auto expected_is_array = false;
    auto expected_is_list = false;
    if (expected_entry.kind == type_kind::array_kind) {
      element_expected = expected_entry.result;
      expected_is_array = true;
    } else if (expected_entry.kind == type_kind::builtin_generic_kind &&
               (expected_entry.name == "list" ||
                expected_entry.name == "slice")) {
      element_expected =
          expected_entry.args.empty() ? k_unknown_type : expected_entry.args[0];
      expected_is_list = true;
    }

    if (array.fill_value != nullptr) {
      const auto element = infer_expr(*array.fill_value, element_expected);
      auto count = std::optional<uint64_t>{};
      if (array.fill_count != nullptr) {
        infer_expr(*array.fill_count, types_.builtin("usize"));
        if (array.fill_count->kind == ast::node_kind::literal_expr) {
          const auto &lit =
              dynamic_cast<const ast::literal_expr &>(*array.fill_count);
          if (lit.lit_kind == token_kind::int_lit) {
            count = parse_integer_literal(lit.value);
          }
        }
      }
      return expected_is_list ? types_.builtin_generic("list", {element})
                              : types_.array_of(element, count);
    }

    auto element = element_expected;
    for (const auto &item : array.elements) {
      if (item == nullptr) {
        continue;
      }
      const auto found = infer_expr(*item, element);
      if (types_.is_unknown(element)) {
        element = found;
      } else {
        type_mismatch(item->span, element, found, "for this element");
      }
    }

    if (expected_is_array) {
      if (expected_entry.array_size.has_value() &&
          *expected_entry.array_size != array.elements.size()) {
        error(array.span,
              std::format("array literal has {} element{}, but `{}` holds "
                          "exactly {}",
                          array.elements.size(),
                          array.elements.size() == 1 ? "" : "s",
                          types_.display(strip_refs(expected)),
                          *expected_entry.array_size),
              "wrong number of elements");
      }
      return types_.array_of(element, array.elements.size());
    }
    return types_.builtin_generic("list", {element});
  }

  // ==========================================================================
  //  Patterns
  // ==========================================================================

  /// Checks a pattern against the type it is matching (`subject`), binding
  /// any names it introduces and reporting shape mismatches (wrong tuple
  /// arity, unknown struct field, wrong variant argument count) and literal
  /// type mismatches. Recurses into every subpattern regardless of whether
  /// `subject` is fully known, so bindings are always produced even when the
  /// checker can't fully verify the pattern's shape.
  auto check_pattern(const ast::pattern &pattern, type_id subject) -> void {
    if (pattern.has_error) {
      return;
    }
    const auto stripped = strip_refs(subject);
    const auto &entry = types_.entry(stripped);
    // Every pattern kind below matches against `stripped` — recording it
    // uniformly here (rather than in each branch) is what lets a later
    // lowering pass ask "what type does this pattern node match?" for any
    // pattern, including ones nested inside tuple/constructor/struct
    // destructuring, without re-deriving it from the AST a second time.
    record_expr_type(pattern, stripped);

    switch (pattern.kind) {
    case ast::node_kind::wildcard_pattern:
      return;
    case ast::node_kind::binding_pattern: {
      const auto &binding = dynamic_cast<const ast::binding_pattern &>(pattern);
      bind_value(binding.name, stripped,
                 binding.is_mut ? binding_origin::mut_binding
                                : binding_origin::pattern_binding,
                 binding.span);
      return;
    }
    case ast::node_kind::literal_pattern: {
      const auto &literal = dynamic_cast<const ast::literal_pattern &>(pattern);
      auto lit = ast::literal_expr{};
      lit.lit_kind = literal.lit_kind;
      lit.value = literal.value;
      lit.span = literal.span;
      const auto found = infer_literal(lit, stripped);
      type_mismatch(literal.span, stripped, found, "from the match subject");
      return;
    }
    case ast::node_kind::tuple_pattern: {
      const auto &tuple = dynamic_cast<const ast::tuple_pattern &>(pattern);
      if (entry.kind == type_kind::tuple_kind &&
          entry.args.size() != tuple.elements.size()) {
        error(tuple.span,
              std::format("tuple pattern has {} element{}, but `{}` has {}",
                          tuple.elements.size(),
                          tuple.elements.size() == 1 ? "" : "s",
                          types_.display(stripped), entry.args.size()),
              "pattern shape does not match the value");
      }
      for (size_t i = 0; i < tuple.elements.size(); ++i) {
        if (tuple.elements[i] != nullptr) {
          const auto element =
              entry.kind == type_kind::tuple_kind && i < entry.args.size()
                  ? entry.args[i]
                  : k_unknown_type;
          check_pattern(*tuple.elements[i], element);
        }
      }
      return;
    }
    case ast::node_kind::constructor_pattern: {
      const auto &ctor =
          dynamic_cast<const ast::constructor_pattern &>(pattern);
      check_constructor_pattern(ctor, stripped);
      return;
    }
    case ast::node_kind::option_pattern: {
      const auto &option = dynamic_cast<const ast::option_pattern &>(pattern);
      auto inner = k_unknown_type;
      if (entry.kind == type_kind::builtin_generic_kind &&
          entry.name == "option" && !entry.args.empty()) {
        inner = entry.args[0];
      }
      if (option.inner != nullptr) {
        check_pattern(*option.inner, inner);
      }
      return;
    }
    case ast::node_kind::result_pattern: {
      const auto &result = dynamic_cast<const ast::result_pattern &>(pattern);
      auto inner = k_unknown_type;
      if (entry.kind == type_kind::builtin_generic_kind &&
          entry.name == "result") {
        const auto slot =
            result.result_kind == ast::option_result_kind::err ? 1u : 0u;
        if (slot < entry.args.size()) {
          inner = entry.args[slot];
        }
      }
      if (result.inner != nullptr) {
        check_pattern(*result.inner, inner);
      }
      return;
    }
    case ast::node_kind::struct_pattern: {
      const auto &struct_pattern =
          dynamic_cast<const ast::struct_pattern &>(pattern);
      for (const auto &field : struct_pattern.fields) {
        if (field.is_rest) {
          continue;
        }
        auto field_type = k_unknown_type;
        if (entry.kind == type_kind::struct_kind) {
          if (const auto found = struct_field_type(entry, field.name)) {
            field_type = *found;
          } else {
            error(field.span,
                  std::format("no field `{}` on struct `{}`", field.name,
                              entry.name),
                  "unknown field in pattern");
          }
        }
        // A shorthand field (`{x}`, field.pattern == nullptr) has no
        // sub-pattern node to key `node_types_` against, so it's recorded
        // here instead — see `struct_pattern_field_types_`'s doc comment.
        struct_pattern_field_types_[&field] = field_type;
        if (field.pattern != nullptr) {
          check_pattern(*field.pattern, field_type);
        } else if (!field.name.empty()) {
          bind_value(field.name, field_type, binding_origin::pattern_binding,
                     field.span);
        }
      }
      return;
    }
    case ast::node_kind::array_pattern: {
      const auto &array = dynamic_cast<const ast::array_pattern &>(pattern);
      const auto element = element_type_of(stripped, array.span);
      for (const auto &item : array.elements) {
        if (item != nullptr) {
          check_pattern(*item, element);
        }
      }
      return;
    }
    case ast::node_kind::range_pattern: {
      const auto &range = dynamic_cast<const ast::range_pattern &>(pattern);
      if (range.start != nullptr) {
        const auto found = infer_expr(*range.start, stripped);
        type_mismatch(range.start->span, stripped, found,
                      "from the match subject");
      }
      if (range.end != nullptr) {
        const auto found = infer_expr(*range.end, stripped);
        type_mismatch(range.end->span, stripped, found,
                      "from the match subject");
      }
      return;
    }
    case ast::node_kind::ref_pattern: {
      const auto &ref = dynamic_cast<const ast::ref_pattern &>(pattern);
      if (ref.inner != nullptr) {
        check_pattern(*ref.inner, stripped);
      }
      return;
    }
    case ast::node_kind::or_pattern: {
      const auto &alternatives = dynamic_cast<const ast::or_pattern &>(pattern);
      for (const auto &alternative : alternatives.alternatives) {
        if (alternative != nullptr) {
          check_pattern(*alternative, stripped);
        }
      }
      return;
    }
    case ast::node_kind::group_pattern: {
      const auto &group = dynamic_cast<const ast::group_pattern &>(pattern);
      if (group.inner != nullptr) {
        check_pattern(*group.inner, stripped);
      }
      if (group.alias.has_value()) {
        bind_value(*group.alias, stripped, binding_origin::pattern_binding,
                   group.span);
      }
      return;
    }
    default:
      return;
    }
  }

  /// Checks a `@variant(...)` pattern against the sum type it should match:
  /// resolves prelude option/result variants specially, otherwise looks up
  /// the variant on `subject`'s sum type and checks the destructured
  /// sub-patterns against its declared payload types, reporting an unknown
  /// variant or wrong destructuring arity.
  auto check_constructor_pattern(const ast::constructor_pattern &ctor,
                                 type_id subject) -> void {
    const auto &entry = types_.entry(subject);

    // Prelude option/result variants in pattern position.
    if (ctor.name == "some" || ctor.name == "none" || ctor.name == "ok" ||
        ctor.name == "err") {
      auto inner = k_unknown_type;
      if (entry.kind == type_kind::builtin_generic_kind) {
        if (entry.name == "option" && !entry.args.empty()) {
          inner = entry.args[0];
        } else if (entry.name == "result") {
          const auto slot = ctor.name == "err" ? 1u : 0u;
          if (slot < entry.args.size()) {
            inner = entry.args[slot];
          }
        }
      }
      for (const auto &arg : ctor.args) {
        if (arg != nullptr) {
          check_pattern(*arg, inner);
        }
      }
      return;
    }

    if (entry.kind == type_kind::sum_kind) {
      const auto *variant = find_variant(entry, ctor.name);
      if (variant == nullptr) {
        auto diag =
            diagnostic(diagnostic_level::error,
                       std::format("unknown variant `@{}` for sum type `{}`",
                                   ctor.name, entry.name),
                       file_id_);
        diag.with_label(ctor.span, "no such variant");
        if (const auto *variants = sum_variants_of(entry)) {
          auto names = std::string{};
          for (const auto &candidate : *variants) {
            if (!names.empty()) {
              names += ", ";
            }
            names += std::format("`@{}`", candidate.name);
          }
          diag.with_note(
              std::format("`{}` declares the variants {}", entry.name, names));
        }
        diag_.emit(diag);
        mark_error();
        for (const auto &arg : ctor.args) {
          if (arg != nullptr) {
            check_pattern(*arg, k_unknown_type);
          }
        }
        return;
      }

      const auto payload = variant_payload_types(entry, *variant);
      if (ctor.args.size() != payload.size()) {
        error(ctor.span,
              std::format("variant `@{}` carries {} value{}, but the pattern "
                          "destructures {}",
                          ctor.name, payload.size(),
                          payload.size() == 1 ? "" : "s", ctor.args.size()),
              "pattern shape does not match the variant");
      }
      for (size_t i = 0; i < ctor.args.size(); ++i) {
        if (ctor.args[i] != nullptr) {
          check_pattern(*ctor.args[i],
                        i < payload.size() ? payload[i] : k_unknown_type);
        }
      }
      return;
    }

    // Unknown subject — still resolve nested bindings.
    for (const auto &arg : ctor.args) {
      if (arg != nullptr) {
        check_pattern(*arg, k_unknown_type);
      }
    }
  }

  // ==========================================================================
  //  Match checking and exhaustiveness
  // ==========================================================================

  /// Whether `pattern` matches every possible value of its subject's type
  /// (a wildcard, a plain binding, or an or-pattern with an irrefutable
  /// alternative) — such an arm guarantees exhaustiveness regardless of
  /// what other arms cover.
  auto pattern_is_irrefutable(const ast::pattern &pattern) -> bool {
    switch (pattern.kind) {
    case ast::node_kind::wildcard_pattern:
    case ast::node_kind::binding_pattern:
      return true;
    case ast::node_kind::group_pattern: {
      const auto &group = dynamic_cast<const ast::group_pattern &>(pattern);
      return group.inner != nullptr && pattern_is_irrefutable(*group.inner);
    }
    case ast::node_kind::or_pattern: {
      const auto &alternatives = dynamic_cast<const ast::or_pattern &>(pattern);
      return std::ranges::any_of(alternatives.alternatives,
                                 [this](const auto &alternative) -> bool {
                                   return alternative != nullptr &&
                                          pattern_is_irrefutable(*alternative);
                                 });
    }
    default:
      return false;
    }
  }

  /// Records the variant/literal names `pattern` covers (a constructor's
  /// variant name, an option/result kind, a boolean literal), recursing
  /// into or-pattern alternatives and group-pattern inner patterns.
  auto collect_covered_names(const ast::pattern &pattern,
                             std::unordered_set<std::string> &covered) -> void {
    switch (pattern.kind) {
    case ast::node_kind::constructor_pattern:
      covered.insert(
          dynamic_cast<const ast::constructor_pattern &>(pattern).name);
      return;
    case ast::node_kind::option_pattern:
      covered.insert(
          dynamic_cast<const ast::option_pattern &>(pattern).option_kind ==
                  ast::option_result_kind::some
              ? "some"
              : "none");
      return;
    case ast::node_kind::result_pattern:
      covered.insert(
          dynamic_cast<const ast::result_pattern &>(pattern).result_kind ==
                  ast::option_result_kind::err
              ? "err"
              : "ok");
      return;
    case ast::node_kind::literal_pattern: {
      const auto &literal = dynamic_cast<const ast::literal_pattern &>(pattern);
      if (literal.lit_kind == token_kind::kw_true) {
        covered.insert("true");
      } else if (literal.lit_kind == token_kind::kw_false) {
        covered.insert("false");
      }
      return;
    }
    case ast::node_kind::or_pattern: {
      const auto &alternatives = dynamic_cast<const ast::or_pattern &>(pattern);
      for (const auto &alternative : alternatives.alternatives) {
        if (alternative != nullptr) {
          collect_covered_names(*alternative, covered);
        }
      }
      return;
    }
    case ast::node_kind::group_pattern: {
      const auto &group = dynamic_cast<const ast::group_pattern &>(pattern);
      if (group.inner != nullptr) {
        collect_covered_names(*group.inner, covered);
      }
      return;
    }
    default:
      return;
    }
  }

  /// Reports a non-exhaustive-match error listing the specific missing
  /// variants/values, unless some unguarded arm is irrefutable. Only proves
  /// exhaustiveness for sum types, `option`, `result`, and `bool`; any other
  /// subject type (including `unknown`) is trusted rather than checked.
  auto check_match_exhaustiveness(type_id subject, source_span span,
                                  const std::vector<ast::match_arm> &arms)
      -> void {
    auto covered = std::unordered_set<std::string>{};
    for (const auto &arm : arms) {
      if (arm.pattern == nullptr || arm.has_error) {
        continue;
      }
      const auto &pattern = dynamic_cast<const ast::pattern &>(*arm.pattern);
      if (arm.guard != nullptr) {
        continue; // guarded arms never guarantee coverage
      }
      if (pattern_is_irrefutable(pattern)) {
        return; // an unconditional arm covers everything
      }
      collect_covered_names(pattern, covered);
    }

    const auto stripped = strip_refs(subject);
    const auto &entry = types_.entry(stripped);

    auto required = std::vector<std::string>{};
    if (entry.kind == type_kind::sum_kind) {
      if (const auto *variants = sum_variants_of(entry)) {
        for (const auto &variant : *variants) {
          required.push_back(variant.name);
        }
      }
    } else if (entry.kind == type_kind::builtin_generic_kind &&
               entry.name == "option") {
      required = {"some", "none"};
    } else if (entry.kind == type_kind::builtin_generic_kind &&
               entry.name == "result") {
      required = {"ok", "err"};
    } else if (types_.is_boolean(stripped)) {
      required = {"true", "false"};
    } else if (entry.kind == type_kind::builtin_kind) {
      error_with_help(
          span,
          std::format("non-exhaustive match on `{}`", types_.display(stripped)),
          "these arms may not cover every value",
          "Add a final `_ => ...` arm to handle the remaining values.");
      return;
    } else {
      return; // unknown or user types without variants — nothing to prove
    }

    auto missing = std::string{};
    for (const auto &name : required) {
      if (!covered.contains(name)) {
        if (!missing.empty()) {
          missing += ", ";
        }
        missing += (types_.is_boolean(stripped) ? std::format("`{}`", name)
                                                : std::format("`@{}`", name));
      }
    }
    if (missing.empty()) {
      return;
    }
    auto diag =
        diagnostic(diagnostic_level::error,
                   std::format("non-exhaustive match on `{}`: {} not covered",
                               types_.display(stripped), missing),
                   file_id_);
    diag.with_label(span, "missing match arms");
    diag.with_help(std::format(
        "Handle {} explicitly, or add a final `_ => ...` arm.", missing));
    diag_.emit(diag);
    mark_error();
  }

  /// Types a `match` expression or statement: infers the subject (if any),
  /// checks each arm's pattern against it, requires a `bool` guard, joins
  /// arm result types via `join_branch_type`, and finally checks
  /// exhaustiveness. Shared by `match_expr` and `match_stmt` handling.
  auto check_match(const ast::expr *subject,
                   const std::vector<ast::match_arm> &arms, type_id expected,
                   source_span span) -> type_id {
    auto subject_type = k_unknown_type;
    if (subject != nullptr) {
      subject_type = strip_refs(infer_expr(*subject, k_unknown_type));
    }

    auto result = expected;
    for (const auto &arm : arms) {
      push_scope();
      if (arm.pattern != nullptr) {
        check_pattern(dynamic_cast<const ast::pattern &>(*arm.pattern),
                      subject_type);
      }
      if (arm.guard != nullptr) {
        require_bool(*arm.guard, "a match guard");
      }
      type_id arm_type;
      source_span body_span;
      if (arm.body_expr != nullptr) {
        arm_type = infer_expr(*arm.body_expr, expected);
        body_span = arm.body_expr->span;
      } else {
        arm_type = check_body_nodes(arm.body_stmts, expected);
        body_span = arm.span;
      }
      result = join_branch_type(result, arm_type, body_span, "`match`");
      pop_scope();
    }

    if (!arms.empty()) {
      check_match_exhaustiveness(
          subject_type, subject != nullptr ? subject->span : span, arms);
    }
    return result;
  }

  // ==========================================================================
  //  Statements
  // ==========================================================================

  /// Human-readable phrase for a binding's origin, used in the "declared
  /// here" note on an immutable-assignment error (e.g. "bound with `let`").
  auto binding_origin_description(binding_origin origin) -> std::string_view {
    switch (origin) {
    case binding_origin::let_binding:
      return "bound with `let`";
    case binding_origin::pattern_binding:
      return "bound by a pattern";
    case binding_origin::parameter:
      return "a parameter";
    case binding_origin::var_binding:
      return "declared with `var`";
    case binding_origin::mut_binding:
      return "declared with `mut`";
    case binding_origin::synthetic:
      return "introduced here";
    }
    return "bound here";
  }

  /// Walks through field/index/group access to find the identifier at the
  /// root of an assignment target (e.g. `a` in `a.b[0].c = x`), so mutation
  /// through a `let`-bound root can be flagged even when the assignment
  /// itself targets a nested field.
  auto assignment_root_ident(const ast::expr &target)
      -> const ast::ident_expr * {
    const auto *current = &target;
    while (true) {
      switch (current->kind) {
      case ast::node_kind::ident_expr:
        return dynamic_cast<const ast::ident_expr *>(current);
      case ast::node_kind::field_expr: {
        const auto &field = dynamic_cast<const ast::field_expr &>(*current);
        if (field.object == nullptr) {
          return nullptr;
        }
        current = field.object.get();
        break;
      }
      case ast::node_kind::index_expr: {
        const auto &index = dynamic_cast<const ast::index_expr &>(*current);
        if (index.object == nullptr) {
          return nullptr;
        }
        current = index.object.get();
        break;
      }
      case ast::node_kind::group_expr: {
        const auto &group = dynamic_cast<const ast::group_expr &>(*current);
        if (group.inner == nullptr) {
          return nullptr;
        }
        current = group.inner.get();
        break;
      }
      default:
        return nullptr;
      }
    }
  }

  /// Checks an assignment statement: a plain identifier target must not be
  /// `let`/pattern-bound; a computed target (field/index chain) is checked
  /// for mutation through a `let`-bound root; a compound-assignment
  /// operator (`+=` etc.) requires a numeric target; and the assigned
  /// value's type is checked against the target's type.
  auto check_assignment(const ast::assign_stmt &stmt) -> void {
    auto target_type = k_unknown_type;

    if (stmt.target != nullptr) {
      if (stmt.target->kind == ast::node_kind::ident_expr) {
        const auto &ident = dynamic_cast<const ast::ident_expr &>(*stmt.target);
        if (const auto *binding = lookup_value(ident.name)) {
          target_type = binding->type;
          // Unlike the "not found" branch below, this path resolves the
          // target straight from the scope binding rather than through
          // `infer_expr`, so it needs its own persistence hook — see
          // `node_types_`'s doc comment.
          record_expr_type(ident, target_type);
          if (binding->origin == binding_origin::let_binding ||
              binding->origin == binding_origin::pattern_binding) {
            auto diag = diagnostic(
                diagnostic_level::error,
                std::format("cannot assign to immutable binding `{}`",
                            ident.name),
                file_id_);
            diag.with_label(ident.span, "this binding is immutable");
            diag.children.push_back(
                diagnostic(
                    diagnostic_level::note,
                    std::format("`{}` is {}", ident.name,
                                binding_origin_description(binding->origin)),
                    file_id_)
                    .with_label(binding->span, "binding declared here"));
            diag.with_help(std::format(
                "Declare it with `var {}` to allow reassignment, or shadow "
                "it with a new `let`.",
                ident.name));
            diag_.emit(diag);
            mark_error();
          }
        } else {
          target_type =
              record_expr_type(ident, resolve_ident(ident, k_unknown_type));
        }
      } else {
        target_type = infer_expr(*stmt.target, k_unknown_type);
        auto root_name = std::string{};
        if (const auto *root = assignment_root_ident(*stmt.target)) {
          root_name = root->name;
        } else if (stmt.target->kind == ast::node_kind::module_path_expr) {
          const auto &path =
              dynamic_cast<const ast::module_path_expr &>(*stmt.target);
          if (!path.segments.empty()) {
            root_name = path.segments.front();
          }
        }
        if (!root_name.empty()) {
          const auto *binding = lookup_value(root_name);
          if (binding != nullptr &&
              binding->origin == binding_origin::let_binding &&
              types_.entry(binding->type).kind != type_kind::ref_kind) {
            error_with_help(
                stmt.target->span,
                std::format("cannot mutate `{}` — it is bound with `let`",
                            root_name),
                "mutation through an immutable binding",
                std::format("Declare `{}` with `var` to allow mutation.",
                            root_name));
          }
        }
      }
    }

    const auto stripped = strip_refs(target_type);
    if (stmt.op != ast::assign_op::assign && !types_.is_unknown(stripped) &&
        !types_.is_numeric(stripped)) {
      error(stmt.span,
            std::format("compound assignment requires a numeric target, "
                        "found `{}`",
                        types_.display(stripped)),
            "not a numeric value");
    }

    if (stmt.value != nullptr) {
      const auto found = infer_expr(*stmt.value, stripped);
      if (stmt.op == ast::assign_op::assign) {
        type_mismatch(stmt.value->span, stripped, found,
                      "from the assignment target");
      }
    }
  }

  /// Checks one node in statement position. Returns the node's value type so
  /// blocks can surface their tail expression; bindings extend the innermost
  /// scope.
  /// Checks one statement-position AST node and returns its value type
  /// (used as the tail-expression type when it is the last node of a
  /// block). Handles every statement kind directly; a bare expression or a
  /// nested declaration in statement position falls through to
  /// `infer_expr`/`check_item` respectively.
  auto check_body_node(const ast::node &node, type_id expected_tail)
      -> type_id {
    if (node.has_error) {
      return k_unknown_type;
    }
    const auto unit = types_.builtin("unit");

    switch (node.kind) {
    case ast::node_kind::let_stmt: {
      const auto &stmt = dynamic_cast<const ast::let_stmt &>(node);
      auto declared = k_unknown_type;
      if (stmt.type_annotation != nullptr) {
        declared = resolve_type(*stmt.type_annotation, current_resolve_ctx());
      }
      auto found = k_unknown_type;
      if (stmt.initializer != nullptr) {
        found = infer_expr(*stmt.initializer, declared);
        if (stmt.type_annotation != nullptr) {
          type_mismatch(stmt.initializer->span, declared, found,
                        "from the annotation");
        }
      }
      const auto binding_type =
          stmt.type_annotation != nullptr ? declared : found;
      if (!stmt.else_body.empty()) {
        check_body_nodes(stmt.else_body, k_unknown_type);
      }
      if (stmt.pattern != nullptr) {
        if (stmt.pattern->kind == ast::node_kind::binding_pattern) {
          const auto &binding =
              dynamic_cast<const ast::binding_pattern &>(*stmt.pattern);
          // Bypasses check_pattern (which records this itself at its own
          // call site), so record explicitly: a later pass (the move
          // checker) needs every plain `let` binding's type keyed by its
          // pattern node, the same way a destructured binding's is.
          record_expr_type(*stmt.pattern, binding_type);
          bind_value(binding.name, binding_type,
                     binding.is_mut ? binding_origin::mut_binding
                                    : binding_origin::let_binding,
                     binding.span);
        } else {
          check_pattern(*stmt.pattern, strip_refs(binding_type));
        }
      }
      return unit;
    }

    case ast::node_kind::var_stmt: {
      const auto &stmt = dynamic_cast<const ast::var_stmt &>(node);
      auto declared = k_unknown_type;
      if (stmt.type_annotation != nullptr) {
        declared = resolve_type(*stmt.type_annotation, current_resolve_ctx());
      }
      auto found = k_unknown_type;
      if (stmt.initializer != nullptr) {
        found = infer_expr(*stmt.initializer, declared);
        if (stmt.type_annotation != nullptr) {
          type_mismatch(stmt.initializer->span, declared, found,
                        "from the annotation");
        }
      }
      const auto binding_type =
          stmt.type_annotation != nullptr ? declared : found;
      // `var` has no pattern node to key against, so key on the statement
      // itself — a later pass (the move checker) needs this binding's type
      // and node_types is keyed generically by `const ast::node *`.
      record_expr_type(stmt, binding_type);
      bind_value(stmt.name, binding_type, binding_origin::var_binding,
                 stmt.span);
      return unit;
    }

    case ast::node_kind::assign_stmt:
      check_assignment(dynamic_cast<const ast::assign_stmt &>(node));
      return unit;

    case ast::node_kind::expr_stmt: {
      const auto &stmt = dynamic_cast<const ast::expr_stmt &>(node);
      return stmt.expr != nullptr ? infer_expr(*stmt.expr, expected_tail)
                                  : unit;
    }

    case ast::node_kind::return_stmt: {
      const auto &stmt = dynamic_cast<const ast::return_stmt &>(node);
      if (stmt.value != nullptr) {
        const auto found = infer_expr(*stmt.value, return_type_);
        if (return_annotated_ && !types_.compatible(return_type_, found)) {
          auto diag = diagnostic(
              diagnostic_level::error,
              std::format("return type mismatch: expected `{}`, found `{}`",
                          types_.display(return_type_), types_.display(found)),
              file_id_);
          diag.with_label(stmt.value->span, std::format("this returns `{}`",
                                                        types_.display(found)));
          diag.with_note(
              std::format("the enclosing function is declared to return `{}`",
                          types_.display(return_type_)));
          diag_.emit(diag);
          mark_error();
        }
      } else if (return_annotated_ && !types_.is_unit(return_type_) &&
                 !types_.is_unknown(return_type_)) {
        error_with_help(
            stmt.span,
            std::format("bare `return` in a function that returns `{}`",
                        types_.display(return_type_)),
            "missing return value",
            std::format("Return a `{}` value, or change the return type to "
                        "`unit`.",
                        types_.display(return_type_)));
      }
      return types_.builtin("never");
    }

    case ast::node_kind::if_stmt: {
      // Mirrors `infer_if_expr` (used for `if` in ordinary expression
      // position, e.g. `return if cond: a else: b`) rather than always
      // typing to `unit`: an `if` used as a block's tail statement is
      // exactly as value-producing as `match` already is below — join each
      // branch's tail type against `expected_tail` the same way. When
      // `expected_tail` is `k_unknown_type` (this `if` is a genuine
      // mid-body statement, not a tail — see `check_body_nodes`, which
      // only ever passes a real `expected_tail` to the *last* item), this
      // still ends up `unit` exactly like before.
      const auto &stmt = dynamic_cast<const ast::if_stmt &>(node);
      auto result = expected_tail;
      for (const auto &branch : stmt.branches) {
        check_if_branch_header(branch);
        push_scope();
        if (branch.let_pattern != nullptr && branch.let_expr == nullptr) {
          check_pattern(dynamic_cast<const ast::pattern &>(*branch.let_pattern),
                        k_unknown_type);
        }
        const auto branch_type = check_body_nodes(branch.body, expected_tail);
        pop_scope();
        result = join_branch_type(result, branch_type,
                                  branch.body.empty() ||
                                          branch.body.back() == nullptr
                                      ? branch.span
                                      : branch.body.back()->span,
                                  "`if`");
      }
      if (!stmt.else_body.empty()) {
        const auto else_type = check_body_nodes(stmt.else_body, expected_tail);
        result = join_branch_type(result, else_type,
                                  stmt.else_body.back() != nullptr
                                      ? stmt.else_body.back()->span
                                      : stmt.span,
                                  "`if`");
      }
      return result != k_unknown_type ? result : unit;
    }

    case ast::node_kind::while_stmt: {
      const auto &stmt = dynamic_cast<const ast::while_stmt &>(node);
      if (stmt.condition != nullptr) {
        require_bool(*stmt.condition, "a `while` condition");
      }
      push_scope();
      if (stmt.let_expr != nullptr && stmt.let_pattern != nullptr) {
        const auto subject = infer_expr(*stmt.let_expr, k_unknown_type);
        check_pattern(*stmt.let_pattern, strip_refs(subject));
      }
      check_body_nodes(stmt.body, k_unknown_type);
      pop_scope();
      return unit;
    }

    case ast::node_kind::for_stmt: {
      const auto &stmt = dynamic_cast<const ast::for_stmt &>(node);
      auto element = k_unknown_type;
      if (stmt.iterable != nullptr) {
        const auto iterable = infer_expr(*stmt.iterable, k_unknown_type);
        element = element_type_of(iterable, stmt.iterable->span);
      }
      push_scope();
      if (stmt.patterns.size() == 1) {
        if (stmt.patterns.front() != nullptr) {
          check_pattern(*stmt.patterns.front(), element);
        }
      } else {
        for (const auto &pattern : stmt.patterns) {
          if (pattern != nullptr) {
            check_pattern(*pattern, k_unknown_type);
          }
        }
      }
      if (stmt.guard != nullptr) {
        require_bool(*stmt.guard, "a `for` guard");
      }
      check_body_nodes(stmt.body, k_unknown_type);
      pop_scope();
      return unit;
    }

    case ast::node_kind::match_stmt: {
      const auto &stmt = dynamic_cast<const ast::match_stmt &>(node);
      check_match(stmt.subject.get(), stmt.arms, expected_tail, stmt.span);
      return expected_tail != k_unknown_type ? expected_tail : unit;
    }

    case ast::node_kind::crew_stmt: {
      const auto &stmt = dynamic_cast<const ast::crew_stmt &>(node);
      push_scope();
      bind_value(stmt.name, k_unknown_type, binding_origin::synthetic,
                 stmt.span);
      check_body_nodes(stmt.body, k_unknown_type);
      pop_scope();
      return unit;
    }

    case ast::node_kind::splice_stmt: {
      const auto &stmt = dynamic_cast<const ast::splice_stmt &>(node);
      if (stmt.expr != nullptr) {
        infer_expr(*stmt.expr, k_unknown_type);
      }
      return unit;
    }

    case ast::node_kind::asm_stmt:
    case ast::node_kind::use_decl:
    case ast::node_kind::dep_decl:
      return unit;

    default:
      // Nested declarations and bare expressions in statement position.
      if (const auto *expr = dynamic_cast<const ast::expr *>(&node)) {
        return infer_expr(*expr, expected_tail);
      }
      check_item(node, /*at_module_scope=*/false);
      return unit;
    }
  }

  // ==========================================================================
  //  Declarations
  // ==========================================================================

  /// Checks an `intrinsic def` declaration: its name must be one the
  /// compiler's backends actually implement (see `src/intrinsics.h` and
  /// `spec/stdlib.md`), it must sit at module scope rather than inside a
  /// trait/impl, and — since there is no body to infer from — every
  /// parameter and the return type must be explicitly annotated.
  auto check_intrinsic_decl(const ast::func_decl &decl, bool at_module_scope)
      -> void {
    if (!at_module_scope) {
      error_with_help(
          decl.span,
          std::format("`intrinsic def {}` must be a module-level declaration",
                      decl.name),
          "intrinsic declarations cannot be a trait or impl member",
          "Move this declaration to module scope. A trait or impl method "
          "can call a module-level intrinsic from its body instead.");
    }

    if (!is_known_intrinsic(decl.name)) {
      auto candidates = std::vector<std::string>{};
      candidates.reserve(known_intrinsic_names.size());
      for (const auto known : known_intrinsic_names) {
        candidates.emplace_back(known);
      }
      auto diag = diagnostic(
          diagnostic_level::error,
          std::format("`{}` is not a recognized intrinsic", decl.name),
          file_id_);
      diag.with_label(decl.span, "no backend implements this name");
      if (const auto suggestion = best_suggestion(decl.name, candidates)) {
        diag.with_help(std::format("did you mean `{}`?", *suggestion));
      } else {
        diag.with_help(
            "see spec/stdlib.md for the list of recognized intrinsics");
      }
      diag_.emit(diag);
      mark_error();
    }

    for (const auto &param : decl.params) {
      if (param.type_annotation == nullptr) {
        error_with_help(
            param.span,
            std::format("intrinsic `{}` must annotate every parameter",
                        decl.name),
            "missing type annotation",
            "An intrinsic has no body for the compiler to infer types "
            "from — every parameter needs an explicit type.");
      }
    }
    if (decl.return_type == nullptr) {
      error_with_help(
          decl.span,
          std::format("intrinsic `{}` must annotate its return type",
                      decl.name),
          "missing return type",
          "An intrinsic has no body for the compiler to infer a return "
          "type from — add `-> Type`.");
    }
  }

  /// Checks a function/method declaration end to end: enforces the
  /// `pub`-must-annotate rule (`at_module_scope`), binds type parameters
  /// and value parameters (including a leading `self` typed from
  /// `self_type_`) into a fresh scope stack, checks contract conditions
  /// under purity enforcement, and checks the body against the declared or
  /// inferred return type. Saves and restores all per-function state
  /// (scopes, return type, undefined-name dedup) so nested/sibling checks
  /// don't see stale state.
  auto check_function(const ast::func_decl &decl, bool at_module_scope)
      -> void {
    if (decl.has_error) {
      return;
    }

    if (at_module_scope && decl.visibility == ast::visibility::pub) {
      auto unannotated = false;
      for (size_t i = 0; i < decl.params.size(); ++i) {
        if (decl.params[i].type_annotation == nullptr &&
            !(i == 0 && param_name_of(decl.params[i]) == "self")) {
          unannotated = true;
        }
      }
      if (unannotated || decl.return_type == nullptr) {
        error_with_help(
            decl.span,
            std::format("public function `{}` must annotate its parameters "
                        "and return type",
                        decl.name),
            "exported signature is incomplete",
            "An exported function is a contract other code depends on; "
            "inference is for code inside your module.");
      }
    }

    if (decl.modifiers.is_intrinsic) {
      check_intrinsic_decl(decl, at_module_scope);
    }

    push_type_params(decl.type_params);
    auto saved_scopes = std::move(scopes_);
    scopes_.clear();
    push_scope();
    auto saved_reported = std::move(reported_undefined_);
    reported_undefined_.clear();

    // Compile-time value parameters (`n: usize`) are ordinary values inside
    // the body.
    for (const auto &type_param : decl.type_params) {
      if (type_param.is_value_param && !type_param.name.empty()) {
        const auto type =
            type_param.bound_or_type != nullptr
                ? resolve_type(*type_param.bound_or_type, current_resolve_ctx())
                : k_unknown_type;
        bind_value(type_param.name, type, binding_origin::parameter,
                   type_param.span);
      }
    }

    const auto &inferred_types = param_types_for(decl, module_);
    for (size_t i = 0; i < decl.params.size(); ++i) {
      const auto &param = decl.params[i];
      auto type = k_unknown_type;
      if (param.type_annotation != nullptr) {
        type = resolve_type(*param.type_annotation, current_resolve_ctx());
      } else if (i == 0 && param_name_of(param) == "self") {
        type = self_type_;
      } else if (i < inferred_types.size()) {
        type = inferred_types[i];
      }
      if (param.pattern != nullptr) {
        // Parameters aren't expressions, so `infer_expr`'s chokepoint never
        // sees them; record the resolved type here so a later lowering pass
        // (spec/typed-ir-design.md) can read a parameter's declared type
        // without re-running `resolve_type`'s module-context machinery.
        record_expr_type(*param.pattern, type);
        if (param.pattern->kind == ast::node_kind::binding_pattern) {
          const auto &binding =
              dynamic_cast<const ast::binding_pattern &>(*param.pattern);
          bind_value(binding.name, type, binding_origin::parameter, param.span);
        } else {
          check_pattern(*param.pattern, strip_refs(type));
        }
      }
      if (param.default_value != nullptr) {
        const auto found = infer_expr(*param.default_value, type);
        type_mismatch(param.default_value->span, type, found,
                      "for this default value");
      }
    }

    const auto saved_return = return_type_;
    const auto saved_annotated = return_annotated_;
    return_annotated_ = decl.return_type != nullptr;
    return_type_ = return_annotated_
                       ? resolve_type(*decl.return_type, current_resolve_ctx())
                       : k_unknown_type;
    if (decl.return_type != nullptr) {
      // Same rationale as the parameter loop above: a declared return type
      // is resolved from a `type_expr`, not an expression, so it needs its
      // own persistence hook for lowering to read.
      record_expr_type(*decl.return_type, return_type_);
    }

    for (const auto &constraint : decl.where_constraints) {
      if (constraint.subject != nullptr) {
        resolve_type(*constraint.subject, current_resolve_ctx());
      }
      if (constraint.bound_or_type != nullptr) {
        resolve_type(*constraint.bound_or_type, current_resolve_ctx());
      }
    }

    in_contract_ = true;
    for (const auto &contract : decl.contracts) {
      if (contract.condition != nullptr && !contract.condition->has_error) {
        require_bool(*contract.condition, "a contract condition");
      }
    }
    in_contract_ = false;

    if (decl.body_expr != nullptr) {
      const auto found = infer_expr(
          *decl.body_expr, return_annotated_ ? return_type_ : k_unknown_type);
      if (return_annotated_) {
        type_mismatch(decl.body_expr->span, return_type_, found,
                      "as the function result");
      }
    } else if (!decl.body_stmts.empty()) {
      const auto tail = check_body_nodes(
          decl.body_stmts, return_annotated_ ? return_type_ : k_unknown_type);
      if (return_annotated_ && !types_.is_unit(return_type_) &&
          !types_.is_unknown(return_type_) && !types_.is_unknown(tail) &&
          !types_.is_unit(tail) && !types_.compatible(return_type_, tail)) {
        error(decl.body_stmts.back() != nullptr ? decl.body_stmts.back()->span
                                                : decl.span,
              std::format("function `{}` returns `{}`, but its final "
                          "expression has type `{}`",
                          decl.name, types_.display(return_type_),
                          types_.display(tail)),
              "mismatched final expression");
      }
    }

    return_type_ = saved_return;
    return_annotated_ = saved_annotated;
    reported_undefined_ = std::move(saved_reported);
    scopes_ = std::move(saved_scopes);
    pop_type_params();
  }

  /// Checks a `type` declaration: validates its `deriving` clause against
  /// the derivable-trait allowlist, checks for duplicate struct fields or
  /// sum variants, resolves every field/variant-payload/alias type, and (for
  /// an `invariant` clause) checks it as a `bool` contract with `self`
  /// bound to the type being declared.
  auto check_type_decl(const ast::type_decl &decl) -> void {
    push_type_params(decl.type_params);

    for (const auto &derived : decl.deriving) {
      if (derived != "eq" && derived != "ord" && derived != "show" &&
          derived != "hash") {
        error_with_help(
            decl.span,
            std::format("cannot derive `{}` for `{}`", derived, decl.name),
            "not a derivable trait",
            "The derivable traits are `eq`, `ord`, `show`, and `hash`; "
            "implement other traits with an explicit `impl` block.");
      }
    }

    if (decl.definition != nullptr) {
      switch (decl.definition->kind) {
      case ast::node_kind::struct_type_def: {
        const auto &body =
            dynamic_cast<const ast::struct_type_def &>(*decl.definition).body;
        auto seen = std::unordered_set<std::string>{};
        for (const auto &field : body.fields) {
          if (!field.name.empty() && !seen.insert(field.name).second) {
            error(field.span,
                  std::format("duplicate field `{}` in struct `{}`", field.name,
                              decl.name),
                  "field declared more than once");
          }
          if (field.type != nullptr) {
            resolve_type(*field.type, current_resolve_ctx());
          }
        }
        break;
      }
      case ast::node_kind::sum_type_def: {
        const auto &body =
            dynamic_cast<const ast::sum_type_def &>(*decl.definition).body;
        auto seen = std::unordered_set<std::string>{};
        for (const auto &variant : body.variants) {
          if (!variant.name.empty() && !seen.insert(variant.name).second) {
            error(variant.span,
                  std::format("duplicate variant `@{}` in sum type `{}`",
                              variant.name, decl.name),
                  "variant declared more than once");
          }
          for (const auto &payload : variant.payload_types) {
            if (payload != nullptr) {
              resolve_type(*payload, current_resolve_ctx());
            }
          }
        }
        break;
      }
      case ast::node_kind::refinement_type: {
        const auto &refinement =
            dynamic_cast<const ast::refinement_type &>(*decl.definition);
        const auto base =
            refinement.base != nullptr
                ? resolve_type(*refinement.base, current_resolve_ctx())
                : k_unknown_type;
        if (refinement.predicate != nullptr &&
            !refinement.predicate->has_error) {
          if (const auto *predicate =
                  dynamic_cast<const ast::expr *>(refinement.predicate.get())) {
            push_scope();
            const auto saved_self = self_type_;
            self_type_ = base;
            bind_value("self", base, binding_origin::parameter, decl.span);
            require_bool(*predicate, "a refinement predicate");
            self_type_ = saved_self;
            pop_scope();
          }
        }
        break;
      }
      default:
        if (const auto *type =
                dynamic_cast<const ast::type_expr *>(decl.definition.get())) {
          resolve_type(*type, current_resolve_ctx());
        }
        break;
      }
    }

    if (decl.invariant != nullptr && !decl.invariant->has_error) {
      push_scope();
      const auto saved_self = self_type_;
      self_type_ = make_user_type(decl, module_name_, {});
      bind_value("self", self_type_, binding_origin::parameter, decl.span);
      in_contract_ = true;
      require_bool(*decl.invariant, "a type invariant");
      in_contract_ = false;
      self_type_ = saved_self;
      pop_scope();
    }

    pop_type_params();
  }

  /// Finds a trait by name reachable from the current module (own
  /// declarations or imports), then locates which module actually owns it
  /// (needed because `find_session_trait` doesn't track the owning module
  /// for an imported trait).
  auto find_trait_anywhere(std::string_view name) -> std::optional<
      std::pair<const ast::trait_decl *, const module_members *>> {
    if (const auto found = find_session_trait(name)) {
      const auto *owner = module_;
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

  /// Checks an `impl` block: resolves its target type and (if present) its
  /// trait, validating that a name written before `for` from a type
  /// declaration rather than a trait is flagged; when the trait is known,
  /// checks member completeness/extras via `check_impl_members`; then checks
  /// every member declaration (methods, associated types) with `self_type_`
  /// bound to the impl's target.
  auto check_impl_decl(const ast::impl_decl &decl) -> void {
    push_type_params(decl.type_params);

    const auto target =
        decl.for_type != nullptr
            ? strip_refs(resolve_type(*decl.for_type, current_resolve_ctx()))
            : k_unknown_type;
    const auto &target_entry = types_.entry(target);

    const ast::trait_decl *trait_decl = nullptr;
    auto trait_name = std::string{};
    if (decl.trait_type != nullptr) {
      resolve_type(*decl.trait_type, current_resolve_ctx());
      if (decl.trait_type->kind == ast::node_kind::named_type) {
        const auto &named =
            dynamic_cast<const ast::named_type &>(*decl.trait_type);
        if (!named.path.empty()) {
          trait_name = named.path.back();
        }
      }
      if (!trait_name.empty()) {
        if (const auto found = find_trait_anywhere(trait_name)) {
          trait_decl = found->first;
        } else if (module_ != nullptr && module_->types.contains(trait_name)) {
          error_with_help(
              decl.trait_type->span,
              std::format("`{}` is a type, not a trait", trait_name),
              "impl requires a trait before `for`",
              std::format("Write `impl {}:` to add inherent methods to a "
                          "type instead.",
                          trait_name));
        }
      }
    }

    if (trait_decl != nullptr) {
      check_impl_members(decl, *trait_decl, trait_name, target_entry);
    }

    for (const auto &constraint : decl.where_constraints) {
      if (constraint.subject != nullptr) {
        resolve_type(*constraint.subject, current_resolve_ctx());
      }
      if (constraint.bound_or_type != nullptr) {
        resolve_type(*constraint.bound_or_type, current_resolve_ctx());
      }
    }

    const auto saved_self = self_type_;
    const auto saved_assoc = self_assoc_types_;
    self_type_ = target;
    self_assoc_types_.clear();

    // Populate `self.<name>` associated-type references (impl-defined first,
    // falling back to the trait's default) before checking any method body,
    // since a method may reference an associated type defined later in the
    // same impl.
    if (trait_decl != nullptr) {
      for (const auto &item : trait_decl->items) {
        if (item == nullptr ||
            item->kind != ast::node_kind::associated_type_decl_node) {
          continue;
        }
        const auto &assoc =
            dynamic_cast<const ast::associated_type_decl_node &>(*item);
        if (assoc.value.default_type != nullptr) {
          self_assoc_types_.emplace(
              assoc.value.name,
              resolve_type(*assoc.value.default_type, current_resolve_ctx()));
        }
      }
    }
    for (const auto &item : decl.items) {
      if (item == nullptr || item->has_error ||
          item->kind != ast::node_kind::associated_type_def_node) {
        continue;
      }
      const auto &assoc =
          dynamic_cast<const ast::associated_type_def_node &>(*item);
      const auto resolved =
          assoc.value.type != nullptr
              ? resolve_type(*assoc.value.type, current_resolve_ctx())
              : k_unknown_type;
      self_assoc_types_.insert_or_assign(assoc.value.name, resolved);
    }

    for (const auto &item : decl.items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      if (item->kind == ast::node_kind::func_decl) {
        check_function(dynamic_cast<const ast::func_decl &>(*item),
                       /*at_module_scope=*/false);
      } else if (item->kind == ast::node_kind::associated_type_def_node) {
        // Already resolved above.
      } else {
        check_item(*item, /*at_module_scope=*/false);
      }
    }
    self_type_ = saved_self;
    self_assoc_types_ = saved_assoc;
    pop_type_params();
  }

  /// Checks an `extend` block: resolves its target type and checks each
  /// method with `self_type_` bound to it. Unlike `check_impl_decl`, there
  /// is no trait to satisfy, so there is no coherence, completeness, or
  /// `requires` checking — `extend` makes no conformance claim, so it needs
  /// none of that bookkeeping.
  auto check_extend_decl(const ast::extend_decl &decl) -> void {
    const auto target =
        decl.for_type != nullptr
            ? strip_refs(resolve_type(*decl.for_type, current_resolve_ctx()))
            : k_unknown_type;

    const auto saved_self = self_type_;
    self_type_ = target;
    for (const auto &item : decl.items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      if (item->kind == ast::node_kind::func_decl) {
        check_function(dynamic_cast<const ast::func_decl &>(*item),
                       /*at_module_scope=*/false);
      }
    }
    self_type_ = saved_self;
  }

  /// Validates an impl's members against its trait's requirements: every
  /// non-default trait method must be implemented, every associated type
  /// without a default must be defined, every impl method must actually be
  /// a trait member with a matching parameter count, and every trait
  /// `requires` obligation must already be satisfied by the target type.
  auto check_impl_members(const ast::impl_decl &impl,
                          const ast::trait_decl &trait,
                          std::string_view trait_name, const type_entry &target)
      -> void {
    auto impl_methods =
        std::unordered_map<std::string, const ast::func_decl *>{};
    auto impl_assoc_types = std::unordered_set<std::string>{};
    for (const auto &item : impl.items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      if (item->kind == ast::node_kind::func_decl) {
        const auto &fn = dynamic_cast<const ast::func_decl &>(*item);
        impl_methods.emplace(fn.name, &fn);
      } else if (item->kind == ast::node_kind::associated_type_def_node) {
        impl_assoc_types.insert(
            dynamic_cast<const ast::associated_type_def_node &>(*item)
                .value.name);
      }
    }

    auto trait_methods =
        std::unordered_map<std::string, const ast::func_decl *>{};
    auto missing_methods = std::string{};
    auto missing_assoc = std::string{};
    for (const auto &item : trait.items) {
      if (item == nullptr) {
        continue;
      }
      if (item->kind == ast::node_kind::func_decl) {
        const auto &fn = dynamic_cast<const ast::func_decl &>(*item);
        trait_methods.emplace(fn.name, &fn);
        const auto has_default =
            fn.body_expr != nullptr || !fn.body_stmts.empty();
        if (!has_default && !impl_methods.contains(fn.name)) {
          if (!missing_methods.empty()) {
            missing_methods += ", ";
          }
          missing_methods += std::format("`{}`", fn.name);
        }
      } else if (item->kind == ast::node_kind::associated_type_decl_node) {
        const auto &assoc =
            dynamic_cast<const ast::associated_type_decl_node &>(*item);
        if (assoc.value.default_type == nullptr &&
            !impl_assoc_types.contains(assoc.value.name)) {
          if (!missing_assoc.empty()) {
            missing_assoc += ", ";
          }
          missing_assoc += std::format("`{}`", assoc.value.name);
        }
      }
    }

    const auto target_name = target.decl != nullptr
                                 ? target.decl->name
                                 : types_.display(k_unknown_type);

    if (!missing_methods.empty()) {
      error_with_help(
          impl.span,
          std::format("implementation of trait `{}` for `{}` is missing "
                      "method{} {}",
                      trait_name, target_name,
                      missing_methods.find(',') != std::string::npos ? "s" : "",
                      missing_methods),
          "incomplete trait implementation",
          "Every trait method without a default body must be implemented.");
    }
    if (!missing_assoc.empty()) {
      error_with_help(
          impl.span,
          std::format("implementation of trait `{}` for `{}` is missing "
                      "associated type{} {}",
                      trait_name, target_name,
                      missing_assoc.find(',') != std::string::npos ? "s" : "",
                      missing_assoc),
          "incomplete trait implementation",
          "Define the associated type with `type name = ...` inside the "
          "impl.");
    }

    for (const auto &[name, fn] : impl_methods) {
      const auto it = trait_methods.find(name);
      if (it == trait_methods.end()) {
        error_with_help(
            fn->span,
            std::format("`{}` is not a member of trait `{}`", name, trait_name),
            "unexpected method in trait implementation",
            std::format("Move `{}` into an inherent `impl {}:` block, or add "
                        "it to the trait.",
                        name, target_name));
        continue;
      }
      if (fn->params.size() != it->second->params.size()) {
        auto diag = diagnostic(
            diagnostic_level::error,
            std::format("method `{}` takes {} parameter{}, but trait `{}` "
                        "declares it with {}",
                        name, fn->params.size(),
                        fn->params.size() == 1 ? "" : "s", trait_name,
                        it->second->params.size()),
            file_id_);
        diag.with_label(fn->span, "signature differs from the trait");
        diag_.emit(diag);
        mark_error();
      }
    }

    // `requires` obligations: implementing this trait demands the others.
    if (trait.requires_bound.has_value() && target.decl != nullptr) {
      for (const auto &term : trait.requires_bound->terms) {
        if (term.type == nullptr ||
            term.type->kind != ast::node_kind::named_type) {
          continue;
        }
        const auto &named = dynamic_cast<const ast::named_type &>(*term.type);
        if (named.path.empty()) {
          continue;
        }
        const auto &required = named.path.back();
        if (type_has_trait(target, required)) {
          continue;
        }
        error_with_help(
            impl.span,
            std::format("trait `{}` requires `{}`, but `{}` does not "
                        "implement it",
                        trait_name, required, target_name),
            "unsatisfied trait requirement",
            std::format("Add `impl {} for {}` (or derive it) before "
                        "implementing `{}`.",
                        required, target_name, trait_name));
      }
    }
  }

  /// Checks a `trait` declaration: binds its type parameters and a
  /// self-referential `self` type parameter, resolves its `requires` bound
  /// and associated-type defaults, and checks each method declaration
  /// (including any default body) as an ordinary function.
  auto check_trait_decl(const ast::trait_decl &decl) -> void {
    push_type_params(decl.type_params);
    const auto saved_self = self_type_;
    const auto saved_assoc = self_assoc_types_;
    self_type_ = types_.type_param("self");
    self_assoc_types_.clear();

    if (decl.requires_bound.has_value()) {
      for (const auto &term : decl.requires_bound->terms) {
        if (term.type != nullptr) {
          resolve_type(*term.type, current_resolve_ctx());
        }
      }
    }

    // Populate `self.<name>` associated-type references before checking any
    // method signature, since a method may reference an associated type
    // declared later in the same trait body.
    for (const auto &item : decl.items) {
      if (item == nullptr || item->has_error ||
          item->kind != ast::node_kind::associated_type_decl_node) {
        continue;
      }
      const auto &assoc =
          dynamic_cast<const ast::associated_type_decl_node &>(*item);
      const auto resolved =
          assoc.value.default_type != nullptr
              ? resolve_type(*assoc.value.default_type, current_resolve_ctx())
              : k_unknown_type;
      self_assoc_types_.emplace(assoc.value.name, resolved);
    }

    for (const auto &item : decl.items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      if (item->kind == ast::node_kind::func_decl) {
        check_function(dynamic_cast<const ast::func_decl &>(*item),
                       /*at_module_scope=*/false);
      }
    }

    self_type_ = saved_self;
    self_assoc_types_ = saved_assoc;
    pop_type_params();
  }

  /// Checks a `concept` declaration: binds its parameters as generic type
  /// parameters, then resolves each constraint's subject and (whether it's
  /// a trait/concept bound or a compile-time value expression) its
  /// bound-or-expression payload.
  auto check_concept_decl(const ast::concept_decl &decl) -> void {
    auto param_scope = std::unordered_map<std::string, type_id>{};
    for (const auto &param : decl.params) {
      if (!param.name.empty()) {
        param_scope.emplace(param.name, types_.type_param(param.name));
      }
    }
    type_params_.push_back(std::move(param_scope));

    for (const auto &constraint : decl.constraints) {
      if (constraint.subject != nullptr) {
        resolve_type(*constraint.subject, current_resolve_ctx());
      }
      if (constraint.bound_or_expr != nullptr &&
          !constraint.bound_or_expr->has_error) {
        if (const auto *type = dynamic_cast<const ast::type_expr *>(
                constraint.bound_or_expr.get())) {
          resolve_type(*type, current_resolve_ctx());
        } else if (const auto *expr = dynamic_cast<const ast::expr *>(
                       constraint.bound_or_expr.get())) {
          push_scope();
          infer_expr(*expr, k_unknown_type);
          pop_scope();
        }
      }
    }

    pop_type_params();
  }

  /// Checks a `static` declaration in whichever of its five forms it takes
  /// (binding, assertion, conditional compilation, or either `static for`
  /// variant), dispatching on `decl_kind`.
  auto check_static_decl(const ast::static_decl &decl) -> void {
    switch (decl.decl_kind) {
    case ast::static_decl_kind::binding: {
      auto declared = k_unknown_type;
      if (decl.type_annotation != nullptr) {
        declared = resolve_type(*decl.type_annotation, current_resolve_ctx());
      }
      auto found = k_unknown_type;
      if (decl.initializer != nullptr) {
        found = infer_expr(*decl.initializer, declared);
        if (decl.type_annotation != nullptr) {
          type_mismatch(decl.initializer->span, declared, found,
                        "from the annotation");
        }
      }
      static_types_.insert_or_assign(
          &decl, decl.type_annotation != nullptr ? declared : found);
      return;
    }
    case ast::static_decl_kind::assertion:
      if (decl.assert_condition != nullptr &&
          !decl.assert_condition->has_error) {
        require_bool(*decl.assert_condition, "a `static assert` condition");
      }
      return;
    case ast::static_decl_kind::conditional_compilation:
      if (decl.if_condition != nullptr && !decl.if_condition->has_error) {
        require_bool(*decl.if_condition, "a `static if` condition");
      }
      check_body_nodes(decl.if_body, k_unknown_type);
      check_body_nodes(decl.else_body, k_unknown_type);
      return;
    case ast::static_decl_kind::for_inline:
    case ast::static_decl_kind::for_block: {
      push_scope();
      auto element = k_unknown_type;
      if (decl.for_iterable != nullptr) {
        const auto iterable = infer_expr(*decl.for_iterable, k_unknown_type);
        element = element_type_of(iterable, decl.for_iterable->span);
      }
      for (const auto &pattern : decl.for_patterns) {
        if (pattern != nullptr) {
          check_pattern(*pattern, decl.for_patterns.size() == 1
                                      ? element
                                      : k_unknown_type);
        }
      }
      if (decl.for_guard != nullptr) {
        require_bool(*decl.for_guard, "a `static for` guard");
      }
      if (decl.for_yield != nullptr) {
        infer_expr(*decl.for_yield, k_unknown_type);
      }
      check_body_nodes(decl.for_body, k_unknown_type);
      pop_scope();
      return;
    }
    }
  }

  /// Checks one module/trait/impl-scope item, dispatching to the
  /// appropriate check_* function for its declaration kind; a nested inline
  /// submodule recurses with the module context switched to the submodule,
  /// and anything else (a bare statement reached via unusual nesting) falls
  /// through to `check_body_node`.
  auto check_item(const ast::node &item, bool at_module_scope) -> void {
    if (item.has_error) {
      return;
    }
    switch (item.kind) {
    case ast::node_kind::func_decl:
      check_function(dynamic_cast<const ast::func_decl &>(item),
                     at_module_scope);
      return;
    case ast::node_kind::type_decl:
      check_type_decl(dynamic_cast<const ast::type_decl &>(item));
      return;
    case ast::node_kind::trait_decl:
      check_trait_decl(dynamic_cast<const ast::trait_decl &>(item));
      return;
    case ast::node_kind::impl_decl:
      check_impl_decl(dynamic_cast<const ast::impl_decl &>(item));
      return;
    case ast::node_kind::extend_decl:
      check_extend_decl(dynamic_cast<const ast::extend_decl &>(item));
      return;
    case ast::node_kind::concept_decl:
      check_concept_decl(dynamic_cast<const ast::concept_decl &>(item));
      return;
    case ast::node_kind::static_decl:
      check_static_decl(dynamic_cast<const ast::static_decl &>(item));
      return;
    case ast::node_kind::sub_module_decl: {
      const auto &decl = dynamic_cast<const ast::sub_module_decl &>(item);
      if (decl.items.empty()) {
        return;
      }
      const auto saved_module_name = module_name_;
      const auto *saved_module = module_;
      module_name_ = append_module_name(module_name_, decl.name);
      module_ = index_.find_module(module_name_);
      push_scope();
      for (const auto &child : decl.items) {
        if (child != nullptr && !child->has_error) {
          if (const auto *expr = dynamic_cast<const ast::expr *>(child.get());
              expr != nullptr ||
              dynamic_cast<const ast::stmt *>(child.get()) != nullptr) {
            check_body_node(*child, k_unknown_type);
          } else {
            check_item(*child, /*at_module_scope=*/true);
          }
        }
      }
      pop_scope();
      module_name_ = saved_module_name;
      module_ = saved_module;
      return;
    }
    case ast::node_kind::use_decl:
    case ast::node_kind::dep_decl:
    case ast::node_kind::module_decl:
      return;
    default:
      check_body_node(item, k_unknown_type);
      return;
    }
  }

  // ==========================================================================
  //  Impl coherence (session-wide)
  // ==========================================================================

  /// Session-wide coherence check: for every trait impl in every module,
  /// records the (trait, target-type) pair in `impl_trait_index_` and
  /// reports a duplicate-implementation error (with both locations) if the
  /// pair was already recorded — enforcing Kira's "at most one impl per
  /// (trait, type)" rule. Inherent impls (no trait) are skipped since they
  /// cannot conflict by construction.
  auto validate_impl_coherence() -> void {
    for (const auto &[module_name, members] : index_.modules) {
      for (const auto &impl : members.impls) {
        if (impl.decl->has_error) {
          continue;
        }
        const auto trait_name = trait_name_of_impl(*impl.decl);
        if (trait_name.empty()) {
          continue; // inherent impls never conflict by construction
        }
        const auto target = strip_refs(resolve_impl_target(impl));
        const auto &target_entry = types_.entry(target);
        if (types_.is_unknown(target)) {
          continue;
        }
        const auto key =
            std::format("{}:{}", trait_name, type_key_of(target_entry));
        const auto location =
            source_location{.file_id = impl.file_id, .span = impl.decl->span};
        const auto [it, inserted] = impl_trait_index_.emplace(key, location);
        if (inserted) {
          continue;
        }
        auto duplicate = diagnostic(
            diagnostic_level::error,
            std::format("duplicate implementation of trait `{}` for `{}`",
                        trait_name, types_.display(target)),
            impl.file_id);
        duplicate.with_label(impl.decl->span, "conflicting implementation");
        duplicate.children.push_back(
            diagnostic(diagnostic_level::note,
                       std::format("`{}` was first implemented for `{}` here",
                                   trait_name, types_.display(target)),
                       it->second.file_id)
                .with_label(it->second.span, "previous implementation"));
        duplicate.with_help(
            "A program contains at most one implementation of a trait for a "
            "type; remove or merge one of these.");
        diag_.emit(duplicate);
        if (static_cast<size_t>(impl.file_id) < file_has_errors_.size()) {
          file_has_errors_[impl.file_id] = true;
        }
      }
    }
  }

  // ==========================================================================
  //  Per-file entry
  // ==========================================================================

  /// Recomputes `file_has_external_wildcard_` for the file currently being
  /// checked: true when it has a `use pkg.*` wildcard import whose root the
  /// session does not own, meaning some undefined-looking name might
  /// actually come from that external module.
  auto compute_external_wildcard() -> void {
    file_has_external_wildcard_ = false;
    const auto *imports = imports_for_current_file();
    if (imports == nullptr) {
      return;
    }
    for (const auto &binding : *imports) {
      if (binding.is_wildcard && !session_owns_path_root(binding.path)) {
        file_has_external_wildcard_ = true;
        return;
      }
    }
  }

  /// Checks one file's items end to end: enforces the "`main` xor top-level
  /// statements" rule, recomputes the external-wildcard flag, and checks
  /// every top-level item (statements via `check_body_node`, declarations
  /// via `check_item`) in a fresh top-level scope.
  auto check_file(const ast::file &file) -> void {
    compute_external_wildcard();
    reported_undefined_.clear();

    // A file may declare `main` or use top-level statements, not both.
    const ast::func_decl *main_decl = nullptr;
    const ast::node *first_statement = nullptr;
    for (const auto &item : file.items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      if (item->kind == ast::node_kind::func_decl) {
        const auto &fn = dynamic_cast<const ast::func_decl &>(*item);
        if (fn.name == "main") {
          main_decl = &fn;
        }
      } else if (first_statement == nullptr &&
                 (dynamic_cast<const ast::stmt *>(item.get()) != nullptr ||
                  dynamic_cast<const ast::expr *>(item.get()) != nullptr)) {
        first_statement = item.get();
      }
    }
    if (main_decl != nullptr && first_statement != nullptr) {
      auto diag = diagnostic(
          diagnostic_level::error,
          "a file may declare `main` explicitly or use top-level statements, "
          "but not both",
          file_id_);
      diag.with_label(first_statement->span,
                      "top-level statement in a file that declares `main`");
      diag.children.push_back(
          diagnostic(diagnostic_level::note, "`main` is declared here",
                     file_id_)
              .with_label(main_decl->span, "explicit `main`"));
      diag.with_help("Move these statements into `main`, or delete the "
                     "explicit `main` and keep the script form.");
      diag_.emit(diag);
      mark_error();
    }

    push_scope();
    for (const auto &item : file.items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      if (dynamic_cast<const ast::stmt *>(item.get()) != nullptr ||
          dynamic_cast<const ast::expr *>(item.get()) != nullptr) {
        check_body_node(*item, k_unknown_type);
      } else {
        check_item(*item, /*at_module_scope=*/true);
      }
    }
    pop_scope();
  }

public:
  /// Runs impl coherence once for the whole session, then checks every
  /// input file that has a valid module declaration and no already-recorded
  /// error, setting up the current-file/current-module context before each.
  auto run_impl(const std::vector<parsed_module> &inputs) -> void {
    validate_impl_coherence();

    for (const auto &input : inputs) {
      if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
          input.ast_file->module_decl->has_error ||
          input.ast_file->module_decl->path.empty()) {
        continue;
      }
      if (static_cast<size_t>(input.file_id) < file_has_errors_.size() &&
          file_has_errors_[input.file_id]) {
        continue;
      }
      file_id_ = input.file_id;
      module_name_ = join_strings(input.ast_file->module_decl->path, ".");
      module_ = index_.find_module(module_name_);
      file_no_prelude_ = input.ast_file->no_prelude;
      check_file(*input.ast_file);
    }
  }
};

/// Out-of-line definition of the public entry point declared on `checker`;
/// forwards to `run_impl`.
auto checker::run(const std::vector<parsed_module> &inputs) -> void {
  run_impl(inputs);
}

} // namespace

/// Builds the session-wide `program_index` once, then runs one `checker`
/// instance over every input file.
auto check_program(const std::vector<parsed_module> &inputs,
                   diagnostic_bag &diag, std::vector<bool> &file_has_errors)
    -> checked_types {
  const auto index = build_program_index(inputs);
  auto session_checker = checker(index, diag, file_has_errors);
  session_checker.run(inputs);
  return session_checker.take_checked_types();
}

} // namespace kira::semantic
