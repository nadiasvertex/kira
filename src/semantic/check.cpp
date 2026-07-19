#include "check.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "src/comptime/eval.h"
#include "src/intrinsics.h"
#include "src/parser/ast_clone.h"
#include "src/semantic/module_index.h"
#include "src/semantic/reason.h"
#include "src/semantic/types.h"

namespace kira::semantic {
namespace {

// ==========================================================================
//  Small helpers
// ==========================================================================

/// How an operator is spelled in source — used to echo a predicate back to
/// the user in a proof diagnostic, so ``cannot prove `i < n` `` quotes the
/// condition the way they would have written it.
auto binary_op_spelling(ast::binary_op op) -> std::string_view {
  switch (op) {
  case ast::binary_op::eq_eq:
    return "==";
  case ast::binary_op::bang_eq:
    return "!=";
  case ast::binary_op::lt:
    return "<";
  case ast::binary_op::lt_eq:
    return "<=";
  case ast::binary_op::gt:
    return ">";
  case ast::binary_op::gt_eq:
    return ">=";
  case ast::binary_op::add:
    return "+";
  case ast::binary_op::sub:
    return "-";
  case ast::binary_op::mul:
    return "*";
  case ast::binary_op::div:
    return "/";
  case ast::binary_op::mod:
    return "%";
  case ast::binary_op::logical_and:
    return "and";
  case ast::binary_op::logical_or:
    return "or";
  default:
    return "?";
  }
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
/// What a builtin inherent method returns, relative to its receiver.
///
/// An enum rather than a `type_id` because these are described in one static
/// table (`k_builtin_methods`) built before any `type_table` exists, and
/// several of them depend on the receiver's element type — `list[T]::pop`
/// returns `option[T]`, not one fixed type.
enum class builtin_result_shape : uint8_t {
  usize_result,      ///< `usize`.
  bool_result,       ///< `bool`.
  unit_result,       ///< `unit`.
  element,           ///< The receiver's element type, `T`.
  option_of_element, ///< `option[T]`.
  list_of_element,   ///< `list[T]`.
  byte_slice,        ///< `slice[byte]`.
};

/// One builtin inherent method: the receiver type constructor it belongs to,
/// its name, and what it evaluates to.
struct builtin_method_signature {
  std::string_view owner;
  std::string_view name;
  builtin_result_shape result;
};

/// Every inherent method the compiler answers for a builtin receiver.
///
/// This is deliberately one table rather than a chain of `if` comparisons,
/// because two separate questions have to be answered from it and they must
/// never disagree: *what does `xs.pop()` evaluate to* (`builtin_method_result`)
/// and *what names does a `list` actually have* (`builtin_method_names`, which
/// feeds the "did you mean" suggestion on an unknown method). When those were
/// allowed to be two lists, a name added to one but not the other produced a
/// method that worked but could never be suggested, or a suggestion for a
/// method that did not exist.
///
/// Only methods with real lowering belong here, and an entry *shadows* any
/// `extend` method of the same name: this table is consulted first, and the
/// `extend` lookup runs only when it answers nothing. `str`'s `contains`,
/// `starts_with`, `split`, ... are genuine `extend str` methods in
/// `std.string`, and are found by `find_extend_method_for_builtin` precisely
/// *because* they are absent here — adding them would shadow that module.
///
/// The direction of travel is toward nothing: `option`/`result`'s methods
/// moved out to `std.option`/`std.result` as ordinary Kira, which is what
/// removing their entries here made possible. Prefer writing a method in the
/// stdlib over adding a row plus a matching interception in
/// `hir::lower_call` (spec/collections-algorithms-design.md).
inline constexpr auto k_builtin_methods =
    std::to_array<builtin_method_signature>({
        {"list", "len", builtin_result_shape::usize_result},
        {"list", "push", builtin_result_shape::unit_result},
        {"slice", "len", builtin_result_shape::usize_result},
        {"str", "len", builtin_result_shape::usize_result},
        {"str", "as_bytes", builtin_result_shape::byte_slice},
        {"generator", "next", builtin_result_shape::option_of_element},
    });

/// The key `k_builtin_methods` files a receiver under, or empty when the
/// receiver is not a type builtin methods are answered for. `slice_mut`
/// shares `slice`'s entries — the two differ in mutability, not in what
/// inherent methods they carry.
[[nodiscard]] inline auto builtin_method_owner(const type_entry &object)
    -> std::string_view {
  if (object.kind == type_kind::builtin_kind && object.name == "str") {
    return "str";
  }
  if (object.kind != type_kind::builtin_generic_kind) {
    return {};
  }
  if (object.name == "slice" || object.name == "slice_mut") {
    return "slice";
  }
  if (object.name == "list" || object.name == "option" ||
      object.name == "result" || object.name == "generator") {
    return object.name;
  }
  return {};
}

struct method_entry {
  const ast::func_decl *decl = nullptr; ///< The method's declaration.
  const module_members *owner =
      nullptr; ///< Module the method should resolve types in.
  const ast::trait_decl *from_trait =
      nullptr; ///< Owning trait, if this is a default method.
  bool is_extension =
      false; ///< Whether this method came from an `extend` block.
  /// Bindings that are fixed for this method independent of any call — a
  /// higher-kinded trait default monomorphized for one impl carries its
  /// trait's constructor parameter here (`F := option`), so re-checking a
  /// per-call instance of it resolves `F[A]` the same way its own check did.
  std::unordered_map<std::string, type_id> fixed_type_params;
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
    // An error node is not what its `kind` claims to be. Recovery placeholders
    // borrow a benign kind — `ast::error_expr` reports itself as an
    // `ident_expr` — and mark themselves with `has_error` instead of adding a
    // kind of their own, so the tree stays structurally walkable after a parse
    // failure. The price is that `kind` alone no longer licenses a
    // `dynamic_cast`, and every consumer must check `has_error` first, exactly
    // as `checker::infer_expr_impl` does. Skipping this check is not a missed
    // optimization, it is a `std::bad_cast` — and `if let` plants an
    // `error_expr` in `branch.condition` on *every* parse, error or not, so
    // this is reachable from correct code.
    if (expr.has_error) {
      return k_unknown_type;
    }
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
    // Same reason as `walk_expr`: an `ast::error_stmt` reports itself as an
    // `expr_stmt` and is not one.
    if (node.has_error) {
      return;
    }
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
  checker(program_index &index, diagnostic_bag &diag,
          std::vector<bool> &file_has_errors)
      : index_(index), diag_(diag), file_has_errors_(file_has_errors),
        comptime_eval_(diag, 0) {
    comptime_eval_.set_variant_resolver(
        [this](const ast::node &node) { return resolve_variant_tag(node); });
  }

  /// Entry point: validates impl coherence session-wide, then checks every
  /// input file in turn.
  auto run(const std::vector<parsed_module> &inputs) -> void;

  /// Hands the session's interned types and per-expression type map to the
  /// caller, once `run` has finished. Moves both out of the checker, so call
  /// this at most once, after `run` returns.
  /// Everything the checker synthesized that must outlive it: the compile-
  /// time evaluator's quoted fragments, plus the desugared body of every
  /// `try_from`. Both are pointed at by `spliced_fragments`, which lowering
  /// reads long after the checker is gone.
  auto take_synthesized_fragments() -> ast::ptr_vec<ast::node> {
    auto fragments = comptime_eval_.take_synthesized_fragments();
    for (auto &fragment : proof_fragments_) {
      fragments.push_back(std::move(fragment));
    }
    proof_fragments_.clear();
    return fragments;
  }

  auto take_checked_types() -> checked_types {
    const auto fmt_types = resolve_fmt_runtime_types();
    // Refinements have done their work by now — every obligation is raised
    // and discharged during checking, and a predicate has no runtime
    // existence. Erase them here, once, at the boundary, so no downstream
    // pass (HIR, layout, either backend) ever has to know they existed. This
    // is the same trick `resolve_opaque` plays for existential types, applied
    // one layer earlier because a refinement can hide *inside* a type
    // (`option[index[n]]`) where an unwrap at the use site wouldn't reach it.
    for (auto &[node, type] : node_types_) {
      type = types_.erase_refinements(type);
    }
    for (auto &[field, type] : struct_pattern_field_types_) {
      type = types_.erase_refinements(type);
    }
    for (auto &[field, type] : struct_literal_field_types_) {
      type = types_.erase_refinements(type);
    }
    for (auto &[node, dispatch] : interp_dispatches_) {
      dispatch.value_type = types_.erase_refinements(dispatch.value_type);
    }
    return checked_types{
        .types = std::move(types_),
        .node_types = std::move(node_types_),
        .struct_pattern_field_types = std::move(struct_pattern_field_types_),
        .struct_literal_field_types = std::move(struct_literal_field_types_),
        .call_argument_mappings = std::move(call_argument_mappings_),
        .resolved_callees = std::move(resolved_callees_),
        .operator_dispatches = std::move(operator_dispatches_),
        .interp_dispatches = std::move(interp_dispatches_),
        .for_iterator_dispatches = std::move(for_iterator_dispatches_),
        .fmt_types = fmt_types,
        .synthesized_decls = std::move(synthesized_decls_),
        .synthesized_trait_defaults = std::move(synthesized_trait_defaults_),
        .const_generic_instances = std::move(const_generic_instances_),
        .synthesized_functor_nodes = std::move(synthetic_nodes_),
        .functor_instances = std::move(functor_instance_decls_),
        .synthesized_types = std::move(synthesized_types_),
        .spliced_fragments = std::move(spliced_fragments_),
        .synthesized_fragments = take_synthesized_fragments(),
        .synthesized_item_splices = std::move(synthesized_item_splices_),
        .synthesized_const_literals = std::move(synthesized_const_literals_),
        .static_const_values = std::move(static_const_values_),
        .proven_in_bounds = std::move(proven_in_bounds_),
        .elided_contracts = std::move(elided_contracts_)};
  }

  /// Resolves `std.fmt`'s runtime-support types (`format_spec`, `align_mode`,
  /// `sign_mode`, and the `box_*` scalar wrappers) into `type_id`s for
  /// `hir::lower` — see `fmt_runtime_types`'s doc comment for why this can't
  /// just be a lookup lowering does itself. A no-op (all-zero) result if
  /// `std.fmt` isn't part of this session at all.
  auto resolve_fmt_runtime_types() -> fmt_runtime_types {
    auto result = fmt_runtime_types{};
    const auto *fmt_module = index_.find_module("std.fmt");
    if (fmt_module == nullptr) {
      return result;
    }
    const auto resolve = [&](std::string_view name) -> type_id {
      const auto it = fmt_module->types.find(std::string(name));
      if (it == fmt_module->types.end() || it->second.decl == nullptr) {
        return 0;
      }
      return types_.user_type(*it->second.decl, "std.fmt", {});
    };
    result.format_spec = resolve("format_spec");
    result.align_mode = resolve("align_mode");
    result.sign_mode = resolve("sign_mode");
    result.box_u64 = resolve("box_u64");
    result.box_u32 = resolve("box_u32");
    result.box_u8 = resolve("box_u8");
    result.box_usize = resolve("box_usize");
    result.box_bool = resolve("box_bool");
    result.box_f64 = resolve("box_f64");
    result.str_type = types_.builtin("str");
    result.int64_type = types_.builtin("int64");
    result.uint64_type = types_.builtin("uint64");
    result.uint32_type = types_.builtin("uint32");
    result.uint8_type = types_.builtin("uint8");
    result.float64_type = types_.builtin("float64");
    if (result.align_mode != 0) {
      result.option_align_mode =
          types_.builtin_generic("option", {result.align_mode});
    }
    result.option_usize = types_.builtin_generic("option", {usize_type()});
    return result;
  }

  /// The interned id for the builtin `usize` type — a thin wrapper so
  /// `resolve_fmt_runtime_types` reads the same way regardless of whether
  /// `usize` happens to already be interned (`type_table::builtin` interns
  /// lazily either way, so this is just a readable spelling).
  auto usize_type() -> type_id { return types_.builtin("usize"); }

private:
  // --- session state ----------------------------------------------------
  // Not `const`: `resolve_item_splices` (run before `build_method_table`/
  // `validate_impl_coherence`) injects splice-synthesized impls directly
  // into `index_.modules[...].impls`, so both of those functions — which
  // only ever iterate `index_.modules` — pick the synthesized impls up for
  // free, with no separate bookkeeping of their own.
  program_index &index_;
  diagnostic_bag &diag_;
  /// Every diagnostic already emitted, keyed by level, file, message, and the
  /// spans it points at — see `emit_diag`, which is why this exists.
  std::unordered_set<std::string> emitted_diagnostics_;
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
  /// The expected type each call site was inferred under, recorded on the way
  /// into `infer_call` — the *fourth* and last source of a generic solution
  /// (`spec/generic-inference-design.md` §2). Keyed by call rather than held
  /// in a single member because argument checking runs first and infers
  /// nested calls of its own, which would clobber one.
  ///
  /// Only ever a fallback: `solve_generic_params` consults it after explicit
  /// arguments and after unification against the arguments, and `unify_rigid`
  /// declines to overwrite a name already bound. So a hint can turn an
  /// unsolvable call into a solved one and can never re-solve a call the
  /// arguments already answered — which is what keeps `k_unknown_type`'s
  /// unify-with-everything rule from letting an unknown context quietly
  /// retype a well-understood call.
  std::unordered_map<const ast::call_expr *, type_id> call_expected_types_;
  /// Per call site, the type parameters `solve_from_expected_type` answered
  /// rather than the arguments. Read only to build the second note of §5.4:
  /// once a parameter can be solved from context, an error inside the
  /// instance has to say *which* context, or the binding appears to come
  /// from nowhere the user wrote.
  std::unordered_map<const ast::node *, std::vector<std::string>>
      expected_solved_params_;
  /// The chain of generic instantiations currently being checked, innermost
  /// last. `emit_diag` appends one note per frame, so a diagnostic raised
  /// inside a monomorphized body points back at the call that asked for that
  /// body — without which the user, who wrote an annotation and never named
  /// the failing member at all, has nothing actionable to act on (§5.4).
  struct instantiation_frame {
    source_span call_span;      ///< The call that demanded this instance.
    file_id_type call_file = 0; ///< File that call was written in.
    std::string instance_name;  ///< e.g. `collect$list_iter_int32_$int32`.
    /// One line per parameter solved from the expected type rather than from
    /// an argument.
    std::vector<std::string> context_solutions;
  };
  std::vector<instantiation_frame> instantiation_sites_;
  /// Every module-qualified or type-qualified call resolved by
  /// `infer_qualified_call`. Handed to the caller via `take_checked_types`.
  std::unordered_map<const ast::call_expr *, resolved_callee> resolved_callees_;
  /// Every arithmetic operator resolved against a user operand's overload
  /// trait method, recorded by `require_operand_trait`. Handed to the
  /// caller via `take_checked_types`.
  std::unordered_map<const ast::binary_expr *, resolved_callee>
      operator_dispatches_;
  /// Per-(target type, trait name) associated-type bindings captured while
  /// checking that trait's impl (`check_impl_decl`) — the only place
  /// `self_assoc_types_` is ever populated. A method resolved by
  /// `find_method` from *outside* its own impl (an operator overload's
  /// dispatch, say) has no `self_assoc_types_` context of its own by the
  /// time it's looked up, since that map is scoped to the impl body's own
  /// checking and restored afterward — so a return type mentioning
  /// `self.output` would otherwise resolve to `unknown`. Consulted by
  /// `resolve_operator_return_type`, which temporarily reinstates the
  /// right impl's bindings before resolving such a method's return type.
  std::unordered_map<
      type_id,
      std::unordered_map<std::string, std::unordered_map<std::string, type_id>>>
      impl_assoc_types_;
  /// Every interpolation segment's resolved rendering dispatch — see
  /// `interp_dispatch`'s doc comment in types.h. Populated by
  /// `check_interpolated_string`. Handed to the caller via
  /// `take_checked_types`.
  std::unordered_map<const ast::expr *, interp_dispatch> interp_dispatches_;
  /// Every `for` loop over a user `std.iter.iterator[T]` — see
  /// `iterator_loop_dispatch` in types.h. Populated by `check_body_node`'s
  /// `for_stmt` case, handed to the caller via `take_checked_types`.
  std::unordered_map<const ast::for_stmt *, iterator_loop_dispatch>
      for_iterator_dispatches_;
  /// Owns every trait-default method clone `build_method_table` synthesizes
  /// (see `synthesized_method` in types.h) — lifetime must outlive
  /// `checked_types`, so both are moved out together in
  /// `take_checked_types`.
  ast::ptr_vec<ast::func_decl> synthesized_decls_;
  std::vector<synthesized_method> synthesized_trait_defaults_;
  /// Every const-generic instance `instantiate_generic_function` produced (see
  /// `const_generic_instance` in types.h). The clones themselves are owned by
  /// `synthesized_decls_`, the same way trait-default clones are.
  std::vector<const_generic_instance> const_generic_instances_;
  /// The constants the const-generic instance currently being checked was
  /// instantiated with (`n` -> the `const_value` type slot, and `n` -> the
  /// plain integer). Empty while checking anything else, including the
  /// template these instances come from. `push_type_params` reads the first
  /// (so `index[n]` resolves to `index[3]` inside the instance, which is what
  /// gives a `try_from` real parameters to compare against), and
  /// `resolve_ident` reads the second (so a *value* use of `n` lowers to the
  /// literal `3` — there is no runtime parameter to load it from).
  std::unordered_map<std::string, type_id> const_param_slots_;
  /// While checking a monomorphized copy of a generic *method* (higher-
  /// kinded trait impls — `instantiate_hk_method`): each type parameter
  /// name bound to the concrete type the call solved for it. The type
  /// analog of `const_param_slots_`, consulted by `push_type_params`.
  std::unordered_map<std::string, type_id> type_param_slots_;
  /// One monomorphized instance per (declaration, solution) — so a call made
  /// a thousand times with `n == 3` compiles one `get$3` and not a thousand.
  /// Shared by every instantiation path (free function, method, higher-kinded
  /// method); see `find_or_check_generic_instance`, which also explains why
  /// the entry is inserted *before* the clone's body is checked. Values point
  /// into `synthesized_decls_`.
  std::unordered_map<std::string, const ast::func_decl *> hk_instance_cache_;
  std::unordered_map<std::string, int64_t> const_param_values_;
  /// Nesting depth of `instantiate_generic_function`, bounding a template that
  /// instantiates itself at an ever-changing constant (`f[n]` calling
  /// `f[n + 1]`) — which would otherwise never stop.
  size_t instantiation_depth_ = 0;
  /// Whether the function being checked is a const-generic *template* — its
  /// value parameters are still symbolic, and it will only ever reach a
  /// backend through the instances `instantiate_generic_function` makes of it.
  /// Constructs that need a real constant (`try_from` on `index[n]`) neither
  /// report nor lower here; the instances handle both.
  bool in_const_generic_template_ = false;
  /// Whether the function being checked is a type-generic *template* — its
  /// type parameters are still abstract `T`s (nothing bound them in
  /// `type_param_slots_`), so a call it makes to another type-generic function
  /// can't be monomorphized yet. The template reaches a backend only through
  /// the instances `instantiate_generic_function` makes of it; those clear this
  /// flag (their parameters are bound) and monomorphize such calls for real.
  bool in_type_generic_template_ = false;
  /// Owns every `named_type` synthesized by `reinterpret_as_named_type`
  /// (see its doc comment) for the whole session — stable-address (a
  /// `vector<unique_ptr<T>>` only moves the pointer, never the pointee),
  /// same pattern as `synthesized_decls_` above.
  ast::ptr_vec<ast::type_expr> synthesized_types_;
  /// Splice-node -> resolved-fragment mapping — see `checked_types::
  /// spliced_fragments`'s doc comment in types.h. Populated by the
  /// `splice_expr` case in `infer_expr_impl` and the `splice_stmt` case in
  /// `check_body_node`. Handed to the caller via `take_checked_types`.
  std::unordered_map<const ast::node *, const ast::node *> spliced_fragments_;
  /// Every item-level splice resolved to an injected `impl` block — see
  /// `synthesized_item_splice` in types.h. Populated by
  /// `resolve_item_splices`, moved out via `take_checked_types` for
  /// `hir::lower_module`.
  std::vector<synthesized_item_splice> synthesized_item_splices_;
  /// Top-level (item-position) `splice_stmt` node -> the `impl_decl` it
  /// resolved to, so `check_file`'s item loop knows to `check_item` the
  /// resolved impl instead of routing the splice node itself through
  /// `check_body_node` (which only understands statement/expression
  /// fragments) — see `resolve_item_splices`.
  std::unordered_map<const ast::node *, const ast::impl_decl *>
      item_splice_impls_;
  /// `type_decl` node -> every `impl_decl` `resolve_deriving_traits`
  /// spliced in for it (one per real-derivation-backed trait named in
  /// `deriving`, e.g. `show` *and* `eq`), so `check_file`'s item loop (and
  /// its `sub_module_decl` counterpart) know to *also* `check_item` each
  /// derived impl, in addition to (not instead of, unlike
  /// `item_splice_impls_`) checking the `type_decl` itself normally — there
  /// is no literal splice AST node for `deriving` sugar to swap in for.
  std::unordered_map<const ast::node *, std::vector<const ast::impl_decl *>>
      derived_trait_impls_;
  /// Owns every literal node synthesized by `materialize_const_literal` —
  /// see `checked_types::synthesized_const_literals`'s doc comment.
  ast::ptr_vec<ast::literal_expr> synthesized_const_literals_;
  /// See `checked_types::static_const_values`'s doc comment. Populated by
  /// `resolve_ident`.
  std::unordered_map<const ast::node *, const ast::literal_expr *>
      static_const_values_;
  /// See `checked_types::proven_in_bounds`'s doc comment. Populated by
  /// `check_index_in_bounds`.
  std::unordered_set<const ast::index_expr *> proven_in_bounds_;
  /// Owns the desugared body of every `try_from` call — see
  /// `build_try_from_fragment`. Handed to `checked_types::
  /// synthesized_fragments` at the end of checking, since `spliced_fragments`
  /// points into these and `hir::lower` runs after the checker is gone.
  ast::ptr_vec<ast::node> proof_fragments_;
  /// See `checked_types::elided_contracts`'s doc comment. Populated by
  /// `check_call_preconditions`.
  std::unordered_set<const ast::contract_clause *> elided_contracts_;
  /// Numbers the compiler-introduced binding each `try_from` desugaring needs,
  /// so two proofs in one function never collide.
  size_t proof_temporaries_ = 0;

  // --- current file / module context -------------------------------------
  const module_members *module_ = nullptr;
  std::string module_name_;
  file_id_type file_id_ = 0;
  bool file_has_external_wildcard_ = false;
  bool file_no_prelude_ = false;

  // --- functor instantiation (modules as compile-time values) -------------
  /// AST nodes the checker synthesizes for functor instantiations (cloned
  /// functor-body items), owned here so they outlive the check.
  std::vector<ast::ptr<ast::node>> synthetic_nodes_;
  /// Memoized instantiations: canonical instantiation key → the sanitized
  /// module name its members were registered under. Guarantees one module
  /// identity per (functor, arguments) tuple, session-wide (applicative
  /// functor semantics, `spec/module-values-design.md` §4).
  std::unordered_map<std::string, std::string> functor_instances_;
  /// One entry per `def` cloned into a materialized functor instantiation,
  /// each tagged with its synthetic module name — moved into
  /// `checked_types::functor_instances` for `hir::lower_functor_modules`.
  std::vector<functor_instance> functor_instance_decls_;
  /// One module parameter of a functor bound to its argument module during
  /// instantiation (`DB` → `main.postgres`).
  struct functor_arg_binding {
    std::string param_name;
    std::string arg_module_name;
  };
  /// A materialized functor instantiation whose cloned body has been
  /// *registered* (into `index_`, so its `impl`/`extend` members join the
  /// method table and coherence check) but not yet *checked*. Body checking is
  /// deferred until after `build_method_table`/`validate_impl_coherence` so a
  /// method call inside the body resolves against a complete method table.
  struct pending_functor_body {
    std::string synth_name;
    std::string owner_module; ///< Module the functor was declared in.
    file_id_type functor_file = 0;
    std::vector<functor_arg_binding> module_bindings;
    std::vector<const ast::type_decl *> types;
    std::vector<const ast::static_decl *> statics;
    std::vector<const ast::func_decl *> funcs;
    std::vector<const ast::impl_decl *> impls;
    std::vector<const ast::extend_decl *> extends;
    std::vector<const ast::trait_decl *> traits;
  };
  std::vector<pending_functor_body> pending_functor_bodies_;

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
  /// Whether the contract condition currently being checked is a `post` —
  /// the only place `return` (the value the function returns, not the
  /// statement) may be named. See `resolve_ident`.
  bool in_postcondition_ = false;
  /// Whether the function body currently being checked belongs to a
  /// `generator def`; when true, `yield` is legal and its operand must
  /// match `generator_item_type_` (see `infer_yield`/`check_function`).
  bool in_generator_ = false;
  type_id generator_item_type_ = k_unknown_type;
  /// Whether the function body currently being checked declares a general
  /// `some Trait[Args]` existential return type (never true together with
  /// `in_generator_` — a `generator def`'s `some iterator[T]` stays on its
  /// own dedicated path, see `check_function`). While true, every
  /// return-point's inferred type is joined into `existential_underlying_`
  /// instead of being checked against `return_type_` directly, since
  /// `return_type_` here is the fresh opaque id itself, not a real shape any
  /// concrete expression could match.
  bool checking_existential_return_ = false;
  type_id existential_underlying_ = k_unknown_type;
  std::unordered_set<std::string> reported_undefined_;
  /// Everything known about the values in scope at the point currently being
  /// checked — value-parameter domains, refined parameters' predicates, array
  /// lengths, preconditions, and (inside a branch) path conditions. Rebuilt
  /// per function by `collect_function_facts`, extended and truncated
  /// lexically by `fact_scope`. This is the left-hand side of every proof the
  /// compiler attempts; see `src/semantic/reason.h`.
  fact_set facts_;

  // --- caches ---------------------------------------------------------------
  std::unordered_map<const ast::static_decl *, type_id> static_types_;
  std::unordered_set<const ast::static_decl *> statics_in_progress_;
  /// Compile-time evaluator backing `static let`/`static assert`/
  /// `static if`. One instance for the whole session (parallel to
  /// `static_types_`/`types_`) so `static let` bindings evaluated while
  /// checking one file stay visible by name to `static` expressions in
  /// later files — see the compile-time evaluation design plan's
  /// confluence requirement.
  comptime::evaluator comptime_eval_;
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
  /// Impl-block methods whose target is a *prelude constructor* (`impl
  /// monad for option`), keyed by the constructor's name — the prelude
  /// generics have no `type_decl` to key `methods_` by, exactly like
  /// `extend_methods_by_builtin_` above. Consulted for method calls on any
  /// instantiation of the constructor (`option[int32].bind(...)`) and for
  /// type-qualified associated calls (`option.pure(...)`).
  std::unordered_map<std::string, std::vector<method_entry>>
      impl_methods_by_builtin_;
  /// Memoizes `resolve_existential_type`'s minted `existential_kind` id per
  /// AST node, so the many independent `resolve_type` calls that can all
  /// reach the same `some Trait[Args]` node (a function's own return-type
  /// resolution in `check_function`, plus every external caller's view of
  /// it via `signature_return_type`) agree on one id instead of each
  /// minting a fresh, structurally-identical-but-distinct existential type.
  std::unordered_map<const ast::existential_type *, type_id> existential_types_;

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

  /// Emits `diag`, unless the same complaint about the same source has
  /// already been made.
  ///
  /// One piece of source can be checked several times: a const-generic
  /// template's body is checked once as itself and again inside every
  /// instance made of it (`instantiate_generic_function`), and a trait-default
  /// body once per implementing type (`monomorphize_trait_default`). A
  /// mistake in that source is *one* mistake — reporting it once per copy
  /// would tell the user, three times over, about a line they wrote once.
  /// Anything an instance discovers that the template could not (a contract
  /// refuted at `n == 3`, an index provably out of bounds) reads differently
  /// and still comes through.
  auto emit_diag(const diagnostic &diag) -> void {
    auto key = std::format("{}|{}|{}", static_cast<int>(diag.level),
                           diag.file_id, diag.message);
    for (const auto &label : diag.labels) {
      key += std::format("|{}:{}", label.span.start, label.span.end);
    }
    if (!emitted_diagnostics_.insert(std::move(key)).second) {
      return;
    }
    diag_.emit(with_instantiation_notes(diag));
  }

  /// Appends the §5.4 instantiation-site notes to an error raised while
  /// checking a monomorphized body.
  ///
  /// A generic instance is compiled because some call asked for it, and the
  /// user wrote that call — not the template. Without these notes a failure
  /// inside `collect`'s body reads as an error in the standard library at a
  /// line the user has never seen, and the type the parameter was solved to
  /// appears to come from nowhere at all. Innermost frame first, so the
  /// nearest call is the one read first.
  auto with_instantiation_notes(const diagnostic &diag) -> diagnostic {
    if (instantiation_sites_.empty() || diag.level != diagnostic_level::error) {
      return diag;
    }
    auto annotated = diag;
    for (const auto &frame : std::views::reverse(instantiation_sites_)) {
      annotated.children.push_back(
          diagnostic(diagnostic_level::note,
                     std::format("instantiated from here, as `{}`",
                                 frame.instance_name),
                     frame.call_file)
              .with_label(frame.call_span, "this call needs that copy"));
      for (const auto &solution : frame.context_solutions) {
        annotated.children.push_back(
            diagnostic(diagnostic_level::note, solution, frame.call_file));
      }
    }
    return annotated;
  }

  /// Emits a single-label error at `span` in the current file.
  auto error(source_span span, const std::string &message,
             const std::string &label) -> void {
    auto diag = diagnostic(diagnostic_level::error, message, file_id_);
    diag.with_label(span, label);
    emit_diag(diag);
    mark_error();
  }

  /// Emits a single-label error with an attached help suggestion.
  auto error_with_help(source_span span, const std::string &message,
                       const std::string &label, const std::string &help)
      -> void {
    auto diag = diagnostic(diagnostic_level::error, message, file_id_);
    diag.with_label(span, label);
    diag.with_help(help);
    emit_diag(diag);
    mark_error();
  }

  /// Explains, in the user's own arithmetic, *why* two types with the same
  /// shape still don't match — because somewhere inside them a compile-time
  /// value slot cannot line up. Without this, calling `head` on an empty
  /// `vec` reports a bare "expected `vec[T, n + 1]`, found `vec[int32, 0]`"
  /// and leaves the reader to work out that the lengths are the problem, and
  /// then that no `n: usize` could ever close the gap. Returns `nullopt` when
  /// the mismatch isn't about a value slot, which is the common case.
  auto value_slot_conflict(type_id expected, type_id found)
      -> std::optional<std::string> {
    const auto &expected_entry = types_.entry(expected);
    const auto &found_entry = types_.entry(found);

    if (is_value_kind(expected_entry.kind) && is_value_kind(found_entry.kind)) {
      if (types_.compatible(expected, found)) {
        return std::nullopt;
      }
      if (expected_entry.kind == type_kind::const_variant_kind ||
          found_entry.kind == type_kind::const_variant_kind) {
        return std::format(
            "This value is in state `{}`, but state `{}` is required here — "
            "and a state is a fact about the value, not a conversion you can "
            "perform. Route the value through whatever operation produces "
            "state `{}`.",
            found_entry.name, expected_entry.name, expected_entry.name);
      }
      auto unknowns = std::vector<std::string>{};
      for (const auto &term : expected_entry.value.terms) {
        unknowns.push_back(term.var);
      }
      for (const auto &term : found_entry.value.terms) {
        unknowns.push_back(term.var);
      }
      if (unknowns.empty()) {
        return std::nullopt; // two constants; the displayed types say it all
      }
      std::ranges::sort(unknowns);
      unknowns.erase(std::ranges::unique(unknowns).begin(), unknowns.end());
      const auto domain = types_.display(is_value_kind(expected_entry.kind) &&
                                                 expected_entry.result != 0
                                             ? expected_entry.result
                                             : found_entry.result);
      return std::format(
          "`{}` can never equal `{}` for `{}: {}` — there is no value of `{}` "
          "that would make these lengths agree.",
          expected_entry.name, found_entry.name, join_strings(unknowns, "`, `"),
          domain, join_strings(unknowns, "`, `"));
    }

    if (expected_entry.kind != found_entry.kind) {
      return std::nullopt;
    }
    for (size_t i = 0;
         i < expected_entry.args.size() && i < found_entry.args.size(); ++i) {
      if (auto conflict = value_slot_conflict(expected_entry.args[i],
                                              found_entry.args[i])) {
        return conflict;
      }
    }
    if (expected_entry.result != found_entry.result) {
      return value_slot_conflict(expected_entry.result, found_entry.result);
    }
    return std::nullopt;
  }

  /// Emits a "type mismatch" error if `found` is not `compatible` with
  /// `expected`; a no-op otherwise. Adds a conversion hint when both sides
  /// are numeric, since Kira never converts numbers implicitly.
  ///
  /// `value`, where a caller has the expression to hand, is what makes this
  /// the single chokepoint for *coercion* rather than just for shape: a value
  /// flowing into a refined type owes a proof, and this is every place a
  /// value flows into a declared type (argument, initializer, assignment,
  /// field, element, return). Passing `nullptr` simply forgoes that check —
  /// used where there is no expression, such as a pattern's implied type.
  auto type_mismatch(source_span span, type_id expected, type_id found,
                     std::string_view context, const ast::expr *value = nullptr)
      -> void {
    if (types_.compatible(expected, found)) {
      check_narrowing(expected, found, value, span, context);
      return;
    }
    auto diag =
        diagnostic(diagnostic_level::error,
                   std::format("type mismatch: expected `{}`, found `{}`",
                               types_.display(expected), types_.display(found)),
                   file_id_);
    diag.with_label(span, std::format("expected `{}` {}",
                                      types_.display(expected), context));
    if (auto conflict = value_slot_conflict(expected, found)) {
      diag.with_help(*conflict);
    } else if (types_.is_numeric(expected) && types_.is_numeric(found)) {
      diag.with_help(std::format(
          "Kira never converts numbers implicitly; write `{}(...)` to convert "
          "this value explicitly.",
          types_.display(expected)));
    }
    emit_diag(diag);
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
  /// a `type_param` type — except a value parameter the const-generic
  /// instance currently being checked has a constant for, which interns as
  /// that constant instead (`const_param_slots_`). That one substitution is
  /// the whole of monomorphization on the checker's side: with `n` bound to
  /// `3` rather than to itself, every type the body mentions resolves the way
  /// it would have if the programmer had written `3` there, and the code that
  /// needs a real number (`index[n]`'s bound, `try_from`'s check) finds one.
  auto push_type_params(const std::vector<ast::type_param> &params) -> void {
    auto scope = std::unordered_map<std::string, type_id>{};
    for (const auto &param : params) {
      if (param.name.empty()) {
        continue;
      }
      if (const auto bound = const_param_slots_.find(param.name);
          param.is_value_param && bound != const_param_slots_.end()) {
        scope.emplace(param.name, bound->second);
        continue;
      }
      // The type-parameter analog of `const_param_slots_`: while checking a
      // monomorphized copy of a generic method (`instantiate_hk_method`),
      // each of its *type* parameters is bound to the concrete type the call
      // solved for it instead of an abstract `type_param`.
      if (const auto bound = type_param_slots_.find(param.name);
          !param.is_value_param && bound != type_param_slots_.end()) {
        scope.emplace(param.name, bound->second);
        continue;
      }
      scope.emplace(param.name,
                    types_.type_param(param.name, param.higher_kinded_arity));
    }
    type_params_.push_back(std::move(scope));
  }

  /// Closes the innermost generic-parameter scope.
  auto pop_type_params() -> void { type_params_.pop_back(); }

  /// Binds each compile-time *value* parameter (`n: usize`) as an ordinary
  /// value in the current scope. A value parameter has two lives: it is a
  /// type argument to `resolve_type` (so `vec[T, n]` means something) and a
  /// plain value to `infer_expr` (so `self < n` and `for i in 0..n` mean
  /// something). This is the second one.
  auto bind_value_params(const std::vector<ast::type_param> &params) -> void {
    for (const auto &param : params) {
      if (!param.is_value_param || param.name.empty()) {
        continue;
      }
      const auto type =
          param.bound_or_type != nullptr
              ? resolve_type(*param.bound_or_type, current_resolve_ctx())
              : k_unknown_type;
      bind_value(param.name, type, binding_origin::parameter, param.span);
    }
  }

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

  /// The session-owned modules whose members a wildcard import
  /// (`use a.b.*`) in the current file makes available for unqualified
  /// lookup. Recomputed per call because `file_id_` swaps temporarily while
  /// checking a callee declared in another file. External-rooted wildcards
  /// never resolve here — they only suppress undefined-name errors
  /// (`file_has_external_wildcard_`).
  [[nodiscard]] auto wildcard_import_sources() const
      -> std::vector<const module_members *> {
    auto sources = std::vector<const module_members *>{};
    const auto *imports = imports_for_current_file();
    if (imports == nullptr) {
      return sources;
    }
    for (const auto &binding : *imports) {
      if (!binding.is_wildcard) {
        continue;
      }
      if (const auto *source =
              index_.find_module(join_strings(binding.path, "."))) {
        sources.push_back(source);
      }
    }
    return sources;
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
    /// Whether `some Trait[Args]` (`existential_type`) may resolve to a real
    /// opaque type here. Only set for a function's own return-type position
    /// (`check_function`'s `decl.return_type` resolution and
    /// `signature_return_type`'s external view of the same node) — every
    /// other position (parameters, `let`/`var` annotations, struct fields,
    /// `where` bounds) rejects it with a targeted diagnostic instead, since
    /// there is no monomorphization to give it meaning anywhere else (see
    /// the existential-type-system plan's scope notes).
    bool existential_allowed = false;
    /// When set, a *bare* generic name (`option`, a user generic struct/sum)
    /// may resolve to an unapplied constructor (`ctor_ref_kind`) of this
    /// arity — kind-directed resolution, only enabled where a higher-kinded
    /// slot is being filled: a trait argument for an `F[_]` parameter, or an
    /// `impl <hk-trait> for <ctor>` target. Everywhere else a bare generic
    /// name keeps its historical meaning (an instantiation with unknown
    /// arguments), so ordinary code is untouched.
    std::optional<size_t> expected_ctor_arity;
    /// Set alongside `existential_allowed` when resolving a `generator
    /// def`'s own return type. `some iterator[T]` there is pure syntactic
    /// sugar `check_function` substitutes for the concrete `generator[T]`
    /// (see its doc comment) — it never actually needs `iterator` to be a
    /// real, in-scope trait declaration (unlike the general existential
    /// mechanism), so `resolve_existential_type` skips trait-existence
    /// validation here. Without this, every generator fixture across the
    /// codebase (many written standalone, without `std.iter` in scope —
    /// generator semantics predate the general existential system and
    /// were never meant to depend on it) would need to import `std.iter`
    /// purely to satisfy a validation generator lowering doesn't use.
    bool is_generator_return = false;
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
      const auto length = array.size != nullptr
                              ? resolve_length_arg(*array.size, ctx)
                              : k_unknown_type;
      return array_with_length(element, length);
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
      // An *inline* refinement (`x: int32 where self > 0`) — one written with
      // no declaration to name it. A refinement written as a `type`
      // declaration never reaches here; it is minted, nominally, by
      // `make_refinement_type`.
      const auto &refinement = dynamic_cast<const ast::refinement_type &>(type);
      const auto base = refinement.base != nullptr
                            ? resolve_type(*refinement.base, ctx)
                            : k_unknown_type;
      const auto *predicate =
          dynamic_cast<const ast::expr *>(refinement.predicate.get());
      if (predicate == nullptr || predicate->has_error) {
        return base;
      }
      return types_.refinement_of(
          nullptr, std::format("{} where ...", types_.display(base)),
          module_name_, base, {}, predicate);
    }
    case ast::node_kind::quote_type: {
      const auto &quote = dynamic_cast<const ast::quote_type &>(type);
      switch (quote.quote_kind) {
      case ast::quote_fragment_kind::expr:
        return types_.builtin("expr");
      case ast::quote_fragment_kind::stmt:
        return types_.builtin("stmt");
      case ast::quote_fragment_kind::def_expr:
        return types_.builtin("def_expr");
      case ast::quote_fragment_kind::type_expr:
        return types_.builtin("type_expr");
      case ast::quote_fragment_kind::none:
        return k_unknown_type;
      }
    }
    case ast::node_kind::splice_type: {
      const auto &splice = dynamic_cast<const ast::splice_type &>(type);
      return resolve_splice_type(splice, ctx);
    }
    case ast::node_kind::existential_type:
      return resolve_existential_type(
          dynamic_cast<const ast::existential_type &>(type), ctx);
    default:
      return k_unknown_type;
    }
  }

  /// Resolves `some Bound` (`existential_type`) to a fresh, nominal
  /// `existential_kind` type — one per AST node, memoized in
  /// `existential_types_` so every caller resolving the same node (a
  /// function's own return-type resolution plus every external call site's
  /// `signature_return_type` view of it) agrees on the same id. Only legal
  /// in a function's return-type position (`ctx.existential_allowed`);
  /// anywhere else this reports "not allowed here" and returns
  /// `k_unknown_type`, unless `ctx.quiet` (an external, best-effort view
  /// that shouldn't double-report a diagnostic the declaration's own check
  /// already reports).
  ///
  /// `ctx.is_generator_return` takes a completely separate path: a
  /// `generator def`'s `some iterator[T]` must resolve straight to the
  /// concrete `generator[T]` for *every* caller (this function's own body
  /// check and every external call site's `signature_return_type` view
  /// alike, since both call through here on the same AST node) — never the
  /// general opaque wrapper, which only `check_function`'s own local
  /// `return_type_` could see, not a caller resolving a call's type
  /// elsewhere. Generator semantics predate, and stay fully independent of,
  /// the general existential mechanism below; this is exactly the old,
  /// pre-generalization behavior, preserved verbatim under this flag.
  auto resolve_existential_type(const ast::existential_type &ety,
                                const resolve_ctx &ctx) -> type_id {
    if (!ctx.existential_allowed) {
      if (!ctx.quiet) {
        error_with_help(
            ety.span,
            "`some Trait[Args]` is only supported as a function's return "
            "type",
            "existential type not allowed here",
            "`some Trait` names an opaque type chosen by whatever function "
            "returns it; write a concrete type (or a generic type "
            "parameter) here instead.");
      }
      return k_unknown_type;
    }
    if (ctx.is_generator_return) {
      if (ety.value.terms.size() != 1 || ety.value.terms[0].type == nullptr ||
          ety.value.terms[0].type->kind != ast::node_kind::named_type) {
        return k_unknown_type;
      }
      const auto &named =
          dynamic_cast<const ast::named_type &>(*ety.value.terms[0].type);
      if (named.path.size() != 1 || named.path.front() != "iterator" ||
          named.type_args.size() != 1) {
        return k_unknown_type;
      }
      auto args = resolve_type_args(named, ctx);
      const auto item = args.empty() ? k_unknown_type : args.front();
      return types_.builtin_generic("generator", {item});
    }
    if (const auto found = existential_types_.find(&ety);
        found != existential_types_.end()) {
      return found->second;
    }
    auto bound = std::vector<bound_trait_ref>{};
    auto display_name = std::string("some ");
    for (const auto &term : ety.value.terms) {
      if (term.type == nullptr ||
          term.type->kind != ast::node_kind::named_type) {
        continue;
      }
      const auto &named = dynamic_cast<const ast::named_type &>(*term.type);
      if (named.path.empty()) {
        continue;
      }
      const auto &trait_name = named.path.back();
      if (!ctx.quiet && !find_trait_anywhere(trait_name).has_value()) {
        error_with_help(
            term.span, std::format("`{}` is not a known trait", trait_name),
            "unknown trait in existential bound",
            "`some Trait` requires a real `trait` declaration in scope.");
      }
      auto args = resolve_type_args(named, ctx);
      if (!bound.empty()) {
        display_name += " + ";
      }
      display_name += trait_name;
      if (!args.empty()) {
        display_name += "[";
        for (size_t i = 0; i < args.size(); ++i) {
          if (i != 0) {
            display_name += ", ";
          }
          display_name += types_.display(args[i]);
        }
        display_name += "]";
      }
      bound.push_back(bound_trait_ref{.trait_name = trait_name,
                                      .trait_args = std::move(args)});
    }
    const auto id =
        types_.fresh_existential(std::move(display_name), std::move(bound));
    existential_types_.emplace(&ety, id);
    return id;
  }

  /// A quoted `` `(int32)` `` or `` `(std.foo.bar)` `` classifies as an
  /// `expr` fragment at parse time (`parser::classify_and_parse_quote_
  /// fragment` has no annotation context to know it's meant as a type — see
  /// its doc comment) — the disambiguation is genuinely deferred to this
  /// layer. Here, in type position, a bare name or dotted path is
  /// unambiguous: it can only sensibly mean a named type, so reinterpret it
  /// as one rather than rejecting the splice. Anything structurally richer
  /// (a call, a binary expression, ...) isn't reinterpreted — return
  /// `nullptr` and let the caller report the mismatch.
  auto reinterpret_as_named_type(const ast::node &fragment)
      -> const ast::type_expr * {
    auto path = std::vector<std::string>{};
    if (const auto *ident = dynamic_cast<const ast::ident_expr *>(&fragment)) {
      path.push_back(ident->name);
    } else if (const auto *module_path =
                   dynamic_cast<const ast::module_path_expr *>(&fragment)) {
      path = module_path->segments;
    } else {
      return nullptr;
    }
    auto named = ast::make<ast::named_type>();
    named->span = fragment.span;
    named->path = std::move(path);
    const auto *raw = named.get();
    synthesized_types_.push_back(std::move(named));
    return raw;
  }

  /// Type-position splice: `~(expr)`. Evaluates the operand at compile time
  /// (mirroring `infer_expr`'s `splice_expr` case) and, if it resolves to a
  /// `type_expr_fragment` quote value, resolves the wrapped `ast::type_expr`
  /// in `splice.operand`'s place. No hygiene yet (M5): the wrapped type is
  /// resolved against `ctx`, the splice site's own resolve context, not the
  /// quote site's.
  auto resolve_splice_type(const ast::splice_type &splice,
                           const resolve_ctx &ctx) -> type_id {
    if (splice.operand == nullptr) {
      return k_unknown_type;
    }
    const auto operand_type = infer_expr(*splice.operand, k_unknown_type);
    require_quote_value(operand_type, splice.operand->span,
                        "a type-position splice");
    const auto fragment_value = comptime_eval_.evaluate(*splice.operand);
    if (fragment_value.is_error()) {
      return k_error_type;
    }
    if (fragment_value.kind == comptime::value_kind::type_expr_fragment &&
        fragment_value.fragment != nullptr) {
      const auto *fragment_type =
          dynamic_cast<const ast::type_expr *>(fragment_value.fragment);
      if (fragment_type != nullptr) {
        return resolve_type(*fragment_type, ctx);
      }
    }
    if (fragment_value.kind == comptime::value_kind::expr_fragment &&
        fragment_value.fragment != nullptr) {
      if (const auto *reinterpreted =
              reinterpret_as_named_type(*fragment_value.fragment)) {
        return resolve_type(*reinterpreted, ctx);
      }
    }
    error_with_help(
        splice.operand->span,
        std::format("a type-position splice must resolve to a quoted "
                    "type, found {}",
                    quote_fragment_kind_name(fragment_value.kind)),
        "this splice is used in type position",
        "Only a value quoted as `` `(...)` `` with `type_expr` content "
        "can be spliced into a type position.");
    return k_error_type;
  }

  // ==========================================================================
  //  Compile-time value slots (`spec/dependent-types-design.md` §2)
  //
  //  A type-argument slot may hold a *value* rather than a type: the `3` in
  //  `vec[T, 3]`, the `n + 1` in `vec[T, n + 1]`, the `open` in
  //  `connection[open]`. Closed values fold; open ones become canonical
  //  linear polynomials; variants stay identities. Everything else leaves the
  //  fragment and resolves to `unknown`, which unifies with everything and
  //  diagnoses nothing — the standing policy for "I don't know".
  // ==========================================================================

  /// Looks up the value slot an in-scope name denotes — a value
  /// type-parameter (`n`), or whatever a caller already substituted for it.
  auto lookup_value_binding(std::string_view name, const resolve_ctx &ctx)
      -> std::optional<type_id> {
    if (ctx.param_bindings != nullptr) {
      if (const auto it = ctx.param_bindings->find(std::string(name));
          it != ctx.param_bindings->end()) {
        return it->second;
      }
    }
    if (ctx.use_type_param_stack) {
      if (const auto param = lookup_type_param(name)) {
        return *param;
      }
    }
    return std::nullopt;
  }

  /// The polynomial a bare name contributes. An unsubstituted value parameter
  /// is the unknown `n` itself; a substituted one contributes whatever the
  /// caller passed (a constant, or another polynomial — which is what makes
  /// `concat`'s `vec[T, m + n]` compose when `m` is itself `k + 1`).
  auto name_poly(std::string_view name, const resolve_ctx &ctx)
      -> std::optional<linear_poly> {
    const auto slot = lookup_value_binding(name, ctx);
    if (!slot.has_value()) {
      return std::nullopt;
    }
    const auto &entry = types_.entry(*slot);
    switch (entry.kind) {
    case type_kind::type_param_kind:
      return poly_variable(std::string(name));
    case type_kind::const_value_kind:
    case type_kind::symbolic_value_kind:
      return entry.value;
    default:
      return std::nullopt;
    }
  }

  /// Builds the canonical polynomial denoted by a compile-time value
  /// expression, or `nullopt` when the expression falls outside the linear
  /// fragment (`m * n`, `n / 2`, a call, a field access — see the design doc
  /// §2.1 and §4.4). A bare name arrives here as a `named_type` rather than
  /// an `ident_expr`, because `vec[T, n]`'s `n` parses as a type before the
  /// parser has any way to know the slot wants a value; both spellings are
  /// accepted.
  auto value_poly(const ast::node &node, const resolve_ctx &ctx)
      -> std::optional<linear_poly> {
    switch (node.kind) {
    case ast::node_kind::literal_expr: {
      const auto &lit = dynamic_cast<const ast::literal_expr &>(node);
      if (lit.lit_kind != token_kind::int_lit) {
        return std::nullopt;
      }
      const auto value = parse_integer_literal(lit.value);
      if (!value.has_value()) {
        return std::nullopt;
      }
      return poly_constant(static_cast<int64_t>(*value));
    }
    case ast::node_kind::ident_expr:
      return name_poly(dynamic_cast<const ast::ident_expr &>(node).name, ctx);
    case ast::node_kind::named_type: {
      const auto &named = dynamic_cast<const ast::named_type &>(node);
      if (named.path.size() != 1 || !named.type_args.empty()) {
        return std::nullopt;
      }
      return name_poly(named.path.front(), ctx);
    }
    case ast::node_kind::unary_expr: {
      const auto &unary = dynamic_cast<const ast::unary_expr &>(node);
      if (unary.op != ast::unary_op::neg || unary.operand == nullptr) {
        return std::nullopt;
      }
      const auto operand = value_poly(*unary.operand, ctx);
      return operand.has_value() ? std::optional{poly_negate(*operand)}
                                 : std::nullopt;
    }
    case ast::node_kind::binary_expr: {
      const auto &binary = dynamic_cast<const ast::binary_expr &>(node);
      if (binary.lhs == nullptr || binary.rhs == nullptr) {
        return std::nullopt;
      }
      const auto left = value_poly(*binary.lhs, ctx);
      const auto right = value_poly(*binary.rhs, ctx);
      if (!left.has_value() || !right.has_value()) {
        return std::nullopt;
      }
      switch (binary.op) {
      case ast::binary_op::add:
        return poly_add(*left, *right);
      case ast::binary_op::sub:
        return poly_sub(*left, *right);
      case ast::binary_op::mul:
        // Linear means linear: one side has to be closed, or the product
        // isn't a polynomial we can decide anything about.
        if (left->is_constant()) {
          return poly_scale(*right, left->constant);
        }
        if (right->is_constant()) {
          return poly_scale(*left, right->constant);
        }
        return std::nullopt;
      default:
        return std::nullopt;
      }
    }
    default:
      return std::nullopt;
    }
  }

  /// The bare name a value-argument node spells, if it is one — the `open` in
  /// `connection[open]` (a `named_type`, since it looks like a type) or in
  /// `connection[@open]` (an `ident_expr`, since `@` makes it unambiguously a
  /// variant). Both spellings mean the same state.
  auto value_arg_name(const ast::node &node) -> std::optional<std::string> {
    if (node.kind == ast::node_kind::ident_expr) {
      return dynamic_cast<const ast::ident_expr &>(node).name;
    }
    if (node.kind == ast::node_kind::named_type) {
      const auto &named = dynamic_cast<const ast::named_type &>(node);
      if (named.path.size() == 1 && named.type_args.empty()) {
        return named.path.front();
      }
    }
    return std::nullopt;
  }

  /// Resolves one argument written for a declared *value* parameter
  /// (`n: usize`, `S: conn_state`). A sum-typed parameter takes a variant
  /// name; an integer-typed one takes an expression in the linear fragment.
  auto resolve_value_arg(const ast::node &node, const ast::type_param &param,
                         const resolve_ctx &ctx, source_span span) -> type_id {
    const auto declared =
        param.bound_or_type != nullptr
            ? resolve_type(*param.bound_or_type, quiet_ctx(ctx))
            : types_.usize_type();

    const auto &declared_entry = types_.entry(declared);
    if (declared_entry.kind == type_kind::sum_kind &&
        declared_entry.decl != nullptr) {
      const auto name = value_arg_name(node);
      if (!name.has_value()) {
        return k_unknown_type;
      }
      const auto &sum = dynamic_cast<const ast::sum_type_def &>(
          *declared_entry.decl->definition);
      for (const auto &variant : sum.body.variants) {
        if (variant.name == *name) {
          return types_.const_variant(*declared_entry.decl,
                                      declared_entry.module_name, *name);
        }
      }
      if (!ctx.quiet) {
        auto names = std::vector<std::string>{};
        for (const auto &variant : sum.body.variants) {
          names.push_back(variant.name);
        }
        auto diag = diagnostic(diagnostic_level::error,
                               std::format("`{}` is not a state of `{}`", *name,
                                           declared_entry.name),
                               file_id_);
        diag.with_label(span, "not a variant of this parameter's type");
        diag.with_help(std::format(
            "The `{}` parameter of this type ranges over `{}`, whose states "
            "are {}.",
            param.name, declared_entry.name, join_strings(names, ", ")));
        emit_diag(diag);
        mark_error();
      }
      return k_error_type;
    }

    const auto poly = value_poly(node, ctx);
    if (!poly.has_value()) {
      return k_unknown_type;
    }
    return types_.symbolic_value(declared, *poly);
  }

  /// A copy of `ctx` that reports nothing — used when resolving a type purely
  /// to inspect it (a value parameter's declared type, say), where any
  /// diagnostic would be a duplicate of one the declaration's own check emits.
  auto quiet_ctx(const resolve_ctx &ctx) -> resolve_ctx {
    auto quiet = ctx;
    quiet.quiet = true;
    return quiet;
  }

  /// The length slot of an `array[T, n]`: the same value grammar as any const
  /// generic argument, over `usize` (an array length has no other type).
  auto resolve_length_arg(const ast::node &node, const resolve_ctx &ctx)
      -> type_id {
    const auto poly = value_poly(node, ctx);
    return poly.has_value() ? types_.symbolic_value(types_.usize_type(), *poly)
                            : k_unknown_type;
  }

  /// The closed length of a value slot, when it has one — what layout and
  /// both backends need. `nullopt` for a symbolic length, which is a length
  /// no runtime representation can have.
  auto closed_length(type_id length) -> std::optional<uint64_t> {
    const auto &entry = types_.entry(length);
    if (entry.kind != type_kind::const_value_kind || entry.value.constant < 0) {
      return std::nullopt;
    }
    return static_cast<uint64_t>(entry.value.constant);
  }

  /// Interns `array[T, <length>]` from a resolved length value slot, keeping
  /// the plain integer size in sync for the backends.
  auto array_with_length(type_id element, type_id length) -> type_id {
    return types_.array_of(element, closed_length(length), length);
  }

  /// Resolves each generic argument slot of a named type. A slot declared as
  /// a *value* parameter routes through `resolve_value_arg` (see the section
  /// comment above). A slot declared as — or, absent `decl_params`, simply
  /// looking like — a type resolves structurally.
  ///
  /// `decl_params` is the target declaration's own type parameters, which is
  /// what tells a value slot from a type slot; it may be shorter than
  /// `named`'s argument list (arity mismatches are diagnosed by the caller).
  /// Without it — a prelude container, whose parameters are all types — the
  /// old positional heuristics still apply, so a bare integer literal or an
  /// in-scope value parameter still lands as a value.
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

      const auto *param = decl_params != nullptr && i < decl_params->size()
                              ? &(*decl_params)[i]
                              : nullptr;
      if (param != nullptr && param->is_value_param) {
        args.push_back(resolve_value_arg(*arg.value, *param, ctx, arg.span));
        continue;
      }

      // A type argument slot may hold a type or a compile-time value; only
      // type expressions resolve to types here.
      switch (arg.value->kind) {
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
      case ast::node_kind::named_type:
        // A bare name resolves through the ordinary type path, which already
        // consults `param_bindings` and the type-parameter stack — so a slot
        // a caller has substituted a value into comes back as that value.
        args.push_back(resolve_type(
            dynamic_cast<const ast::named_type &>(*arg.value), ctx));
        break;
      case ast::node_kind::ident_expr: {
        const auto &ident = dynamic_cast<const ast::ident_expr &>(*arg.value);
        const auto slot = lookup_value_binding(ident.name, ctx);
        args.push_back(slot.value_or(k_unknown_type));
        break;
      }
      case ast::node_kind::literal_expr:
      case ast::node_kind::unary_expr:
      case ast::node_kind::binary_expr:
        // An unlabelled value slot (a prelude container, or a declaration
        // whose parameter list we couldn't consult): the value's own type
        // isn't declared anywhere, so it takes the `usize` the language uses
        // for every unannotated compile-time count.
        args.push_back(resolve_length_arg(*arg.value, ctx));
        break;
      default:
        args.push_back(k_unknown_type);
        break;
      }
    }
    return args;
  }

  // ==========================================================================
  //  Reasoning — building facts and goals from source
  //
  //  The bridge between the AST and `src/semantic/reason.h`. A predicate the
  //  user wrote (`self < n`, `x > 0`, `i < v.len()`) becomes a `goal_form`
  //  when it must be *proved* and a `fact_set` when it may be *assumed* —
  //  the same translation either way, differing only in what happens to the
  //  parts that fall outside the fragment: an unrepresentable goal cannot be
  //  proved, while an unrepresentable fact is simply forgotten.
  //
  //  Both translations carry a substitution, because a predicate is always
  //  written about `self` and the declaring type's value parameters, and must
  //  be re-stated about the actual value and the actual arguments before it
  //  means anything at a use site: `index[n]`'s `self < n`, applied to the
  //  argument `i` at a call in a function with its own `n`, is the goal
  //  `i < n`.
  // ==========================================================================

  /// Re-states a predicate about a concrete value. `values` maps each name
  /// the predicate is written in terms of (`self`, and the declaration's
  /// value parameters) to the polynomial that stands in for it here;
  /// `spellings` maps the same names to their source spelling, purely so the
  /// diagnostic can echo the goal back in the user's own words.
  struct predicate_subst {
    std::unordered_map<std::string, linear_poly> values;
    std::unordered_map<std::string, std::string> spellings;
  };

  /// What a call site has pinned each of the callee's value parameters to.
  using value_bindings = std::unordered_map<std::string, linear_poly>;

  /// One explicit compile-time argument, as the parser managed to write it
  /// down.
  ///
  /// The brackets of `zeros[8]()` sit in expression position, where the
  /// parser cannot know a type was meant, so they arrive as an `expr` that
  /// `explicit_type_argument` maps back to a type when the declaration says
  /// the parameter is one. The brackets of `b.take[int64](5)` sit in a
  /// method-name suffix, which the grammar spells as a `type_arg_list`
  /// outright, so those arrive already parsed as a `type_expr`. Exactly one
  /// of the two is set; a value parameter can only be answered by the first,
  /// since a type never folds to a constant.
  struct explicit_generic_arg {
    const ast::expr *value = nullptr;     ///< Written in expression position.
    const ast::type_expr *type = nullptr; ///< Written in type position.
    source_span span;                     ///< Span of the argument itself.
  };

  /// A call's explicit compile-time arguments, in written order. Empty when
  /// the call names its callee bare and every parameter has to be read off
  /// the arguments instead.
  using explicit_generic_args = std::vector<explicit_generic_arg>;

  /// One call argument matched to the parameter it fills, held until
  /// `check_call_args_against` decides the order to infer them in.
  struct pending_argument {
    const ast::expr *value = nullptr;
    const fn_param_info *target = nullptr;
  };

  /// What `check_call_args_against` needs to solve a generic callee's type
  /// parameters partway through checking its arguments, so a deferred lambda
  /// can be checked against a substituted parameter type. Absent for a call
  /// to a non-generic callee, which has nothing to substitute.
  struct generic_call_context {
    const ast::func_decl *decl = nullptr;
    const module_members *owner = nullptr;
    const explicit_generic_args *explicit_args = nullptr;
    const ast::expr *ufcs_receiver = nullptr;
    /// Bindings the caller already solved and is not re-deriving here. A UFCS
    /// call solves its first parameter against the receiver before the
    /// written arguments are looked at (`check_ufcs_call`), and that binding
    /// is exactly the one a bound needs as its subject — `it.map(f)` knows
    /// `I` from `it` and needs `where I: iterator[T]` to get `T`.
    const std::unordered_map<std::string, type_id> *seed = nullptr;
  };

  /// A narrowing obligation raised by one call argument, held back until the
  /// whole argument list has been seen — see `check_call_args_against`.
  struct deferred_narrowing {
    type_id expected = k_unknown_type;
    type_id found = k_unknown_type;
    const ast::expr *value = nullptr;
    source_span span;
  };

  /// The atom key standing for an expression the solver cannot look inside —
  /// a field access, or a call to a `pure` function. Uninterpreted: `f(x)`
  /// equals `f(x)` and nothing else is known about it, so the key must be
  /// canonical (two spellings of the same argument must produce one key) and
  /// injective (two different arguments must not).
  auto atom_key(const ast::expr &expr, const predicate_subst &subst)
      -> std::optional<std::string> {
    switch (expr.kind) {
    case ast::node_kind::ident_expr: {
      const auto &ident = dynamic_cast<const ast::ident_expr &>(expr);
      if (const auto it = subst.values.find(ident.name);
          it != subst.values.end()) {
        return it->second.display();
      }
      return ident.name;
    }
    case ast::node_kind::field_expr: {
      const auto &field = dynamic_cast<const ast::field_expr &>(expr);
      if (field.object == nullptr) {
        return std::nullopt;
      }
      const auto object = atom_key(*field.object, subst);
      return object.has_value() ? std::optional{std::format("{}.{}", *object,
                                                            field.field_name)}
                                : std::nullopt;
    }
    case ast::node_kind::module_path_expr: {
      // `p.value` and `self.value` parse as *paths*, not field accesses —
      // `a.b` is ambiguous between a projection and a module member until
      // names are resolved, and the parser does not resolve names. So the
      // shape a predicate about a struct field actually arrives in is this
      // one, and treating it as an atom keyed on the written path is both
      // correct and exactly what makes an `invariant self.value > 0`
      // discharge a `positive` at `p.value`: the fact and the goal key alike.
      const auto &path = dynamic_cast<const ast::module_path_expr &>(expr);
      if (path.segments.empty()) {
        return std::nullopt;
      }
      auto segments = path.segments;
      if (const auto it = subst.values.find(segments.front());
          it != subst.values.end()) {
        segments.front() = it->second.display();
      }
      return join_strings(segments, ".");
    }
    case ast::node_kind::call_expr: {
      // A method call `v.len()` keys as `len(v)`, a free call `f(x)` as
      // `f(x)` — so a fact stated one way discharges a goal stated the other.
      const auto &call = dynamic_cast<const ast::call_expr &>(expr);
      if (call.callee == nullptr) {
        return std::nullopt;
      }
      auto name = std::string{};
      auto arguments = std::vector<std::string>{};
      if (call.callee->kind == ast::node_kind::field_expr) {
        const auto &field = dynamic_cast<const ast::field_expr &>(*call.callee);
        if (field.object == nullptr) {
          return std::nullopt;
        }
        const auto receiver = atom_key(*field.object, subst);
        if (!receiver.has_value()) {
          return std::nullopt;
        }
        name = field.field_name;
        arguments.push_back(*receiver);
      } else if (call.callee->kind == ast::node_kind::ident_expr) {
        name = dynamic_cast<const ast::ident_expr &>(*call.callee).name;
      } else {
        return std::nullopt;
      }
      for (const auto &arg : call.args) {
        if (arg.value == nullptr) {
          return std::nullopt;
        }
        const auto key = atom_key(*arg.value, subst);
        if (!key.has_value()) {
          return std::nullopt;
        }
        arguments.push_back(*key);
      }
      return std::format("{}({})", name, join_strings(arguments, ","));
    }
    default:
      return std::nullopt;
    }
  }

  /// Translates a value expression into a solver polynomial, treating what it
  /// cannot decompose as an opaque atom. `nullopt` means the expression left
  /// the fragment entirely (`m * n`, `n / 2`, an index, a cast) — see
  /// `spec/dependent-types-design.md` §4.4.
  auto solver_poly(const ast::expr &expr, const predicate_subst &subst)
      -> std::optional<linear_poly> {
    switch (expr.kind) {
    case ast::node_kind::literal_expr: {
      const auto &lit = dynamic_cast<const ast::literal_expr &>(expr);
      if (lit.lit_kind != token_kind::int_lit) {
        return std::nullopt;
      }
      const auto value = parse_integer_literal(lit.value);
      return value.has_value()
                 ? std::optional{poly_constant(static_cast<int64_t>(*value))}
                 : std::nullopt;
    }
    case ast::node_kind::ident_expr: {
      const auto &ident = dynamic_cast<const ast::ident_expr &>(expr);
      if (const auto it = subst.values.find(ident.name);
          it != subst.values.end()) {
        return it->second;
      }
      return poly_variable(ident.name);
    }
    case ast::node_kind::unary_expr: {
      const auto &unary = dynamic_cast<const ast::unary_expr &>(expr);
      if (unary.op != ast::unary_op::neg || unary.operand == nullptr) {
        return std::nullopt;
      }
      const auto operand = solver_poly(*unary.operand, subst);
      return operand.has_value() ? std::optional{poly_negate(*operand)}
                                 : std::nullopt;
    }
    case ast::node_kind::binary_expr: {
      const auto &binary = dynamic_cast<const ast::binary_expr &>(expr);
      if (binary.lhs == nullptr || binary.rhs == nullptr) {
        return std::nullopt;
      }
      const auto left = solver_poly(*binary.lhs, subst);
      const auto right = solver_poly(*binary.rhs, subst);
      if (!left.has_value() || !right.has_value()) {
        return std::nullopt;
      }
      switch (binary.op) {
      case ast::binary_op::add:
        return poly_add(*left, *right);
      case ast::binary_op::sub:
        return poly_sub(*left, *right);
      case ast::binary_op::mul:
        if (left->is_constant()) {
          return poly_scale(*right, left->constant);
        }
        if (right->is_constant()) {
          return poly_scale(*left, right->constant);
        }
        return std::nullopt;
      default:
        return std::nullopt;
      }
    }
    case ast::node_kind::field_expr:
    case ast::node_kind::module_path_expr:
    case ast::node_kind::call_expr: {
      const auto key = atom_key(expr, subst);
      return key.has_value() ? std::optional{poly_variable(*key)}
                             : std::nullopt;
    }
    default:
      return std::nullopt;
    }
  }

  /// The comparison a relational operator makes, once `negated` has been
  /// folded in — `not (a < b)` is `a >= b`, so negation is a rewrite of the
  /// operator, never a wrapper around the constraint.
  static auto negate_comparison(ast::binary_op op) -> ast::binary_op {
    switch (op) {
    case ast::binary_op::eq_eq:
      return ast::binary_op::bang_eq;
    case ast::binary_op::bang_eq:
      return ast::binary_op::eq_eq;
    case ast::binary_op::lt:
      return ast::binary_op::gt_eq;
    case ast::binary_op::lt_eq:
      return ast::binary_op::gt;
    case ast::binary_op::gt:
      return ast::binary_op::lt_eq;
    case ast::binary_op::gt_eq:
      return ast::binary_op::lt;
    default:
      return op;
    }
  }

  /// Builds `lhs OP rhs` as a constraint `poly REL 0`. `<` and `<=` have no
  /// relation of their own — they are the same statement about the negated
  /// polynomial — and `<`/`>` shift the constant by one because the fragment
  /// is over integers, where `a > b` *is* `a >= b + 1`.
  auto comparison_constraint(ast::binary_op op, const linear_poly &lhs,
                             const linear_poly &rhs, std::string label)
      -> std::optional<constraint> {
    const auto difference = poly_sub(lhs, rhs);
    auto shifted = [&](int64_t by) -> linear_poly {
      auto poly = difference;
      poly.constant += by;
      return poly;
    };
    switch (op) {
    case ast::binary_op::eq_eq:
      return constraint{
          .poly = difference, .rel = relation::eq, .label = std::move(label)};
    case ast::binary_op::bang_eq:
      return constraint{
          .poly = difference, .rel = relation::ne, .label = std::move(label)};
    case ast::binary_op::gt_eq:
      return constraint{
          .poly = difference, .rel = relation::ge, .label = std::move(label)};
    case ast::binary_op::gt:
      return constraint{
          .poly = shifted(-1), .rel = relation::ge, .label = std::move(label)};
    case ast::binary_op::lt_eq:
      return constraint{.poly = poly_negate(difference),
                        .rel = relation::ge,
                        .label = std::move(label)};
    case ast::binary_op::lt:
      return constraint{.poly = poly_negate(shifted(1)),
                        .rel = relation::ge,
                        .label = std::move(label)};
    default:
      return std::nullopt;
    }
  }

  /// Renders a predicate back into source-like text with the substitution
  /// applied, so a diagnostic about `index[n]`'s `self < n` can say
  /// ``cannot prove `i < n` `` rather than quoting a predicate the user never
  /// wrote about a `self` that isn't there.
  auto describe(const ast::expr &expr, const predicate_subst &subst)
      -> std::string {
    switch (expr.kind) {
    case ast::node_kind::literal_expr:
      return std::string(dynamic_cast<const ast::literal_expr &>(expr).value);
    case ast::node_kind::ident_expr: {
      const auto &ident = dynamic_cast<const ast::ident_expr &>(expr);
      if (const auto it = subst.spellings.find(ident.name);
          it != subst.spellings.end()) {
        return it->second;
      }
      return ident.name;
    }
    case ast::node_kind::field_expr: {
      const auto &field = dynamic_cast<const ast::field_expr &>(expr);
      return field.object != nullptr
                 ? std::format("{}.{}", describe(*field.object, subst),
                               field.field_name)
                 : field.field_name;
    }
    case ast::node_kind::module_path_expr: {
      const auto &path = dynamic_cast<const ast::module_path_expr &>(expr);
      auto segments = path.segments;
      if (!segments.empty()) {
        if (const auto it = subst.spellings.find(segments.front());
            it != subst.spellings.end()) {
          segments.front() = it->second;
        }
      }
      return join_strings(segments, ".");
    }
    case ast::node_kind::call_expr: {
      const auto &call = dynamic_cast<const ast::call_expr &>(expr);
      auto arguments = std::vector<std::string>{};
      for (const auto &arg : call.args) {
        arguments.push_back(arg.value != nullptr ? describe(*arg.value, subst)
                                                 : "_");
      }
      const auto callee =
          call.callee != nullptr ? describe(*call.callee, subst) : "_";
      return std::format("{}({})", callee, join_strings(arguments, ", "));
    }
    case ast::node_kind::unary_expr: {
      const auto &unary = dynamic_cast<const ast::unary_expr &>(expr);
      const auto operand =
          unary.operand != nullptr ? describe(*unary.operand, subst) : "_";
      switch (unary.op) {
      case ast::unary_op::neg:
        return std::format("-{}", operand);
      case ast::unary_op::logical_not:
        return std::format("not {}", operand);
      default:
        return operand;
      }
    }
    case ast::node_kind::binary_expr: {
      const auto &binary = dynamic_cast<const ast::binary_expr &>(expr);
      const auto left =
          binary.lhs != nullptr ? describe(*binary.lhs, subst) : "_";
      const auto right =
          binary.rhs != nullptr ? describe(*binary.rhs, subst) : "_";
      return std::format("{} {} {}", left, binary_op_spelling(binary.op),
                         right);
    }
    default:
      return "...";
    }
  }

  /// Translates a boolean predicate into a goal in disjunctive normal form.
  /// An empty result means "not expressible", which upstream reads as "not
  /// provable" — the graceful path, never a silent acceptance.
  auto goal_from(const ast::expr &expr, const predicate_subst &subst,
                 bool negated) -> goal_form {
    switch (expr.kind) {
    case ast::node_kind::unary_expr: {
      const auto &unary = dynamic_cast<const ast::unary_expr &>(expr);
      if (unary.op == ast::unary_op::logical_not && unary.operand != nullptr) {
        return goal_from(*unary.operand, subst, !negated);
      }
      break;
    }
    case ast::node_kind::binary_expr: {
      const auto &binary = dynamic_cast<const ast::binary_expr &>(expr);
      if (binary.lhs == nullptr || binary.rhs == nullptr) {
        break;
      }
      // De Morgan: negation swaps the connective, so one traversal handles
      // both polarities.
      const auto conjunctive =
          (binary.op == ast::binary_op::logical_and) != negated;
      if (binary.op == ast::binary_op::logical_and ||
          binary.op == ast::binary_op::logical_or) {
        const auto left = goal_from(*binary.lhs, subst, negated);
        const auto right = goal_from(*binary.rhs, subst, negated);
        if (!conjunctive) {
          // Disjunction: either alternative will do, so an inexpressible side
          // just drops out and the other still stands on its own.
          auto combined = left;
          combined.insert(combined.end(), right.begin(), right.end());
          return combined;
        }
        // Conjunction: every conjunct must be proved, so an inexpressible
        // side sinks the whole goal.
        if (left.empty() || right.empty()) {
          return {};
        }
        auto combined = goal_form{};
        for (const auto &left_alt : left) {
          for (const auto &right_alt : right) {
            auto merged = left_alt;
            merged.insert(merged.end(), right_alt.begin(), right_alt.end());
            combined.push_back(std::move(merged));
          }
        }
        return combined;
      }

      const auto left = solver_poly(*binary.lhs, subst);
      const auto right = solver_poly(*binary.rhs, subst);
      if (!left.has_value() || !right.has_value()) {
        return {};
      }
      const auto op = negated ? negate_comparison(binary.op) : binary.op;
      auto built =
          comparison_constraint(op, *left, *right, describe(expr, subst));
      if (!built.has_value()) {
        return {};
      }
      return goal_form{fact_set{std::move(*built)}};
    }
    default:
      break;
    }

    // Any other boolean term — a call to a `pure` predicate function, say —
    // enters as `atom == 1`. Deliberately the weakest useful rule: an
    // identical fact discharges it, and nothing else does.
    const auto atom = atom_key(expr, subst);
    if (!atom.has_value()) {
      return {};
    }
    auto poly = poly_variable(*atom);
    poly.constant -= 1;
    return goal_form{fact_set{constraint{
        .poly = std::move(poly),
        .rel = negated ? relation::ne : relation::eq,
        .label = describe(expr, subst),
    }}};
  }

  /// Translates a boolean predicate into facts that may be *assumed*. Unlike
  /// a goal this is a plain conjunction: a disjunctive fact carries no
  /// information the solver's fragment can use, so it is dropped — sound,
  /// because forgetting a fact can only make the compiler prove less.
  auto facts_from(const ast::expr &expr, const predicate_subst &subst,
                  bool negated) -> fact_set {
    const auto goal = goal_from(expr, subst, negated);
    return goal.size() == 1 ? goal.front() : fact_set{};
  }

  /// The polynomial standing for a resolved value slot (`3`, `n + 1`, an
  /// unsubstituted value parameter). This is how a refinement's arguments
  /// reach its predicate: `index[n + 1]`'s `self < n` must become
  /// `self < n + 1`.
  auto slot_poly(type_id slot) -> std::optional<linear_poly> {
    const auto &entry = types_.entry(slot);
    switch (entry.kind) {
    case type_kind::const_value_kind:
    case type_kind::symbolic_value_kind:
      return entry.value;
    case type_kind::type_param_kind:
      return poly_variable(entry.name);
    default:
      return std::nullopt;
    }
  }

  /// The substitution under which a refinement's predicate speaks about
  /// `value` (a concrete expression, rendered as `spelling`): `self` becomes
  /// the value, and each of the declaration's value parameters becomes the
  /// argument supplied for it. `nullopt` when an argument falls outside the
  /// fragment, which makes the whole predicate unusable rather than
  /// half-substituted.
  auto refinement_subst(const type_entry &refinement, const linear_poly &value,
                        std::string spelling, const value_bindings *solved)
      -> std::optional<predicate_subst> {
    auto subst = predicate_subst{};
    subst.values.emplace("self", value);
    subst.spellings.emplace("self", std::move(spelling));

    if (refinement.decl == nullptr) {
      return subst;
    }
    for (size_t i = 0; i < refinement.decl->type_params.size(); ++i) {
      const auto &param = refinement.decl->type_params[i];
      if (!param.is_value_param || i >= refinement.args.size()) {
        continue;
      }
      auto poly = slot_poly(refinement.args[i]);
      if (!poly.has_value()) {
        return std::nullopt;
      }
      // At a call site, the callee's own value parameters have been pinned by
      // the other arguments: `index[n]`'s `n` is the `4` that `v: array[T, 4]`
      // just supplied. Substituting here is what turns an obligation stated in
      // the callee's terms into one the caller can actually be held to.
      if (solved != nullptr) {
        poly = poly_substitute(*poly, *solved);
      }
      subst.spellings.emplace(param.name, poly->display());
      subst.values.emplace(param.name, *poly);
    }
    return subst;
  }

  /// Builds the goal that narrowing `value` to `refined` owes: the
  /// refinement's predicate, re-stated about this value. Walks a refinement
  /// of a refinement down to the base, conjoining every predicate on the way.
  auto narrowing_goal(type_id refined, const ast::expr &value,
                      const value_bindings *solved) -> goal_form {
    const auto subst_value = solver_poly(value, predicate_subst{});
    if (!subst_value.has_value()) {
      return {};
    }
    const auto spelling = describe(value, predicate_subst{});

    auto goal = goal_form{};
    for (auto current = refined;
         types_.entry(current).kind == type_kind::refinement_kind;
         current = types_.entry(current).result) {
      const auto &entry = types_.entry(current);
      if (entry.predicate == nullptr) {
        continue;
      }
      const auto subst =
          refinement_subst(entry, *subst_value, spelling, solved);
      if (!subst.has_value()) {
        return {};
      }
      const auto part = goal_from(*entry.predicate, *subst, false);
      if (part.empty()) {
        return {};
      }
      if (goal.empty()) {
        goal = part;
        continue;
      }
      auto combined = goal_form{};
      for (const auto &left : goal) {
        for (const auto &right : part) {
          auto merged = left;
          merged.insert(merged.end(), right.begin(), right.end());
          combined.push_back(std::move(merged));
        }
      }
      goal = std::move(combined);
    }
    return goal;
  }

  /// The facts currently in scope, rendered for a diagnostic. Only the ones
  /// that actually mention something the goal mentions are worth showing —
  /// a reader who wants to know why `i < n` didn't follow is not helped by
  /// being told what is known about `k`.
  auto known_facts_note(const goal_form &goal) -> std::optional<std::string> {
    auto mentioned = std::unordered_set<std::string>{};
    for (const auto &alternative : goal) {
      for (const auto &item : alternative) {
        for (const auto &term : item.poly.terms) {
          mentioned.insert(term.var);
        }
      }
    }
    if (mentioned.empty()) {
      // The goal is arithmetic on constants (`9 < 4`). There is nothing to
      // know about it, and saying "nothing is known" would read as a failure
      // of the compiler rather than of the code.
      return std::nullopt;
    }
    auto shown = std::vector<std::string>{};
    for (const auto &fact : facts_) {
      if (fact.label.empty()) {
        continue;
      }
      const auto relevant = std::ranges::any_of(
          fact.poly.terms, [&](const poly_term &term) -> bool {
            return mentioned.contains(term.var);
          });
      if (relevant) {
        shown.push_back(std::format("`{}`", fact.label));
      }
    }
    if (shown.empty()) {
      return "nothing is known about the values in this condition here";
    }
    return std::format("known here: {}", join_strings(shown, ", "));
  }

  /// The one goal that every alternative shares, for a diagnostic's headline
  /// — a goal is almost always a single constraint, and when it isn't, its
  /// first alternative is still the honest thing to lead with.
  ///
  /// `fallback` is what to say when the goal is *empty*, which is not an
  /// internal failure but a real and expected outcome: the predicate fell
  /// outside the reasoning fragment (a float bound, `n * n`), so there is no
  /// constraint to name and the compiler must quote the source instead. A
  /// placeholder like "the refinement's predicate" would read as the compiler
  /// having lost track of something, when in fact it knows exactly which
  /// predicate it is and exactly why it can't decide it.
  auto goal_label(const goal_form &goal, std::string_view fallback)
      -> std::string {
    for (const auto &alternative : goal) {
      for (const auto &item : alternative) {
        if (!item.label.empty()) {
          return item.label;
        }
      }
    }
    return std::string(fallback);
  }

  /// The source text of a refinement's predicate, restated about `value` —
  /// the honest thing to quote when the predicate is real but undecidable.
  auto predicate_text(type_id refined, const ast::expr &value) -> std::string {
    const auto &entry = types_.entry(refined);
    if (entry.predicate == nullptr) {
      return std::format("the predicate of `{}`", types_.display(refined));
    }
    auto subst = predicate_subst{};
    subst.spellings.emplace("self", describe(value, predicate_subst{}));
    return describe(*entry.predicate, subst);
  }

  /// Spells a refinement the way it reads *at this site*, with the callee's
  /// value parameters replaced by what the call site pinned them to — so the
  /// diagnostic says "expected `index[4]`" rather than "expected `index[n]`"
  /// and quotes an `n` the reader cannot see from where they are standing.
  auto display_refined(type_id refined, const value_bindings *solved)
      -> std::string {
    const auto &entry = types_.entry(refined);
    if (solved == nullptr || entry.decl == nullptr || entry.args.empty()) {
      return types_.display(refined);
    }
    auto arguments = std::vector<std::string>{};
    for (const auto arg : entry.args) {
      const auto poly = slot_poly(arg);
      arguments.push_back(poly.has_value()
                              ? poly_substitute(*poly, *solved).display()
                              : types_.display(arg));
    }
    return std::format("{}[{}]", entry.decl->name,
                       join_strings(arguments, ", "));
  }

  /// Discharges the obligation a *narrowing* incurs: using a value where a
  /// more refined type is required. Widening is free and never lands here
  /// (`compatible` accepts both directions structurally — see `types.h`);
  /// this is the sole place the predicate is actually made to hold.
  ///
  /// Proved: silent. Refuted: an error, because the code is wrong on every
  /// path that reaches it. Unproven: the graceful path — a diagnostic naming
  /// the goal, the facts, and both routes out.
  auto check_narrowing(type_id expected, type_id found, const ast::expr *value,
                       source_span span, std::string_view context,
                       const value_bindings *solved = nullptr) -> void {
    if (value == nullptr ||
        types_.entry(expected).kind != type_kind::refinement_kind) {
      return;
    }
    // Already at least as refined as required: no debt. `found` being the
    // same refinement (or a refinement *of* it) carries the predicate along
    // with it, which is the whole point of the type surviving assignment.
    if (refines(found, expected)) {
      return;
    }
    // A value whose type isn't even the right shape has a type error already
    // reported against it; adding an unprovable-predicate error on top would
    // be noise about a value that was never going to work.
    if (types_.is_unknown(types_.strip_refinement(found))) {
      return;
    }

    const auto goal = narrowing_goal(expected, *value, solved);
    const auto outcome =
        goal.empty() ? proof_result::unknown : solve(facts_, goal);
    if (outcome == proof_result::proved) {
      return;
    }

    const auto refined_name = display_refined(expected, solved);
    const auto base_name = types_.display(types_.strip_refinement(expected));
    const auto goal_text = goal_label(goal, predicate_text(expected, *value));
    const auto message =
        outcome == proof_result::refuted
            ? std::format("`{}` is never true here, so this value can never "
                          "be a `{}`",
                          goal_text, refined_name)
            : std::format("cannot prove `{}` {}", goal_text, context);

    auto diag = diagnostic(diagnostic_level::error, message, file_id_);
    diag.with_label(span, std::format("expected `{}`, found `{}`", refined_name,
                                      types_.display(found)));
    if (const auto known = known_facts_note(goal)) {
      diag.children.emplace_back(diagnostic_level::note, *known, file_id_);
    }
    if (outcome == proof_result::refuted) {
      diag.with_help(std::format(
          "This isn't a gap in the compiler's reasoning — the facts in scope "
          "rule the predicate out. Either the value or the `{}` bound is "
          "wrong.",
          refined_name));
    } else {
      diag.with_help(std::format(
          "Either constrain the value upstream — take it as a `{}` in the "
          "first place, or add a `pre` that says so — or prove it here:\n"
          "    if let @some(proof) = {}.try_from(<value>):\n"
          "        ...\n"
          "which turns the `{}` into a `{}` by checking the predicate at "
          "runtime.",
          refined_name, refined_name, base_name, refined_name));
    }
    emit_diag(diag);
    mark_error();
  }

  /// Solves a callee's value parameters against a concrete argument type, by
  /// walking the two types in parallel and reading off every value slot the
  /// argument determines: matching `array[T, n]` against `array[int32, 4]`
  /// binds `n := 4`; matching `vec[T, n + 1]` against `vec[int32, 3]` binds
  /// `n := 2`.
  ///
  /// This is what makes a dependent signature usable from outside itself. The
  /// callee's `index[n]` means nothing at a call site until `n` is known, and
  /// this is where it becomes known — from the *other* argument, which is
  /// exactly how `safe_get(v, i)` ties the index to the array.
  auto solve_value_params(type_id param, type_id argument, value_bindings &out)
      -> void {
    // Identical types determine nothing new, and stopping here is also what
    // keeps a recursive type (whose argument list can point back at itself)
    // from walking forever.
    if (param == argument) {
      return;
    }
    const auto &param_entry = types_.entry(param);
    const auto &argument_entry = types_.entry(argument);

    if (is_value_kind(param_entry.kind) ||
        param_entry.kind == type_kind::type_param_kind) {
      const auto pattern = slot_poly(param);
      const auto value = slot_poly(argument);
      if (!pattern.has_value() || !value.has_value()) {
        return;
      }
      if (auto solved = solve_for_unknown(*pattern, *value)) {
        // First binding wins: a second, conflicting one means the call is
        // already ill-typed (`compatible` will have said so), and guessing
        // between them would only add a confusing second diagnostic.
        out.emplace(std::move(solved->first), std::move(solved->second));
      }
      return;
    }

    if (param_entry.kind != argument_entry.kind) {
      // A `&T` parameter taking a lent `T` (and the reverse) is the one shape
      // difference that still carries the same structure underneath.
      if (param_entry.kind == type_kind::ref_kind) {
        solve_value_params(param_entry.result, argument, out);
      } else if (argument_entry.kind == type_kind::ref_kind) {
        solve_value_params(param, argument_entry.result, out);
      }
      return;
    }

    for (size_t i = 0;
         i < param_entry.args.size() && i < argument_entry.args.size(); ++i) {
      solve_value_params(param_entry.args[i], argument_entry.args[i], out);
    }
    if (param_entry.result != k_unknown_type &&
        argument_entry.result != k_unknown_type) {
      solve_value_params(param_entry.result, argument_entry.result, out);
    }
  }

  /// Restores `facts_` to its length on entry when it goes out of scope, so a
  /// branch's path conditions and a block's local refinements are visible
  /// exactly within the region that dominates them and nowhere else.
  class fact_scope {
  public:
    explicit fact_scope(fact_set &facts)
        : facts_(&facts), depth_(facts.size()) {}
    fact_scope(const fact_scope &) = delete;
    fact_scope(fact_scope &&) = delete;
    auto operator=(const fact_scope &) -> fact_scope & = delete;
    auto operator=(fact_scope &&) -> fact_scope & = delete;
    ~fact_scope() { facts_->resize(depth_); }

  private:
    fact_set *facts_;
    size_t depth_;
  };

  /// Everything the *type* of a binding says about its value, added to the
  /// facts in scope: a refined type contributes its predicate (with `self`
  /// bound to the name), and an array contributes its length
  /// (`len(v) = n`) — which is what lets an index into it be proved in bounds
  /// against a matching `index[n]`.
  auto assume_binding(std::string_view name, type_id type) -> void {
    const auto value = poly_variable(std::string(name));

    const auto bare = strip_refs(type);
    for (auto current = bare;
         types_.entry(current).kind == type_kind::refinement_kind;
         current = types_.entry(current).result) {
      const auto &entry = types_.entry(current);
      if (entry.predicate == nullptr) {
        continue;
      }
      const auto subst =
          refinement_subst(entry, value, std::string(name), nullptr);
      if (!subst.has_value()) {
        continue;
      }
      for (auto &fact : facts_from(*entry.predicate, *subst, false)) {
        facts_.push_back(std::move(fact));
      }
    }

    const auto &entry = types_.entry(types_.strip_refinement(bare));
    if (entry.kind == type_kind::array_kind && !entry.args.empty()) {
      if (const auto length = slot_poly(entry.args.front())) {
        auto poly =
            poly_sub(poly_variable(std::format("len({})", name)), *length);
        facts_.push_back(constraint{
            .poly = std::move(poly),
            .rel = relation::eq,
            .label = std::format("len({}) == {}", name, length->display()),
        });
      }
    }

    // A struct's `invariant` holds of *every* value of that type, everywhere —
    // that is what makes it an invariant rather than a precondition. So it
    // enters the environment for any binding of the type, which is what lets
    // a `positive_int` whose `invariant self.value > 0` satisfy a `positive`
    // with no further proof (`kira-reference.md` §Contracts).
    if (entry.decl != nullptr && entry.decl->invariant != nullptr &&
        !entry.decl->invariant->has_error) {
      auto subst = predicate_subst{};
      subst.values.emplace("self", value);
      subst.spellings.emplace("self", std::string(name));
      for (auto &fact : facts_from(*entry.decl->invariant, subst, false)) {
        facts_.push_back(std::move(fact));
      }
    }
  }

  // ------------------------------------------------------------------------
  //  Flow-sensitive narrowing (`spec/dependent-types-design.md` §5.1, item 6)
  //
  //  Inside `if x > 0:`, the fact `x > 0` holds — so passing `x` where a
  //  `positive` is expected discharges statically, with no `try_from` and no
  //  annotation. The `else` gets the negation. Facts are scoped exactly to
  //  the region the condition dominates, and any assignment to a name drops
  //  everything known about it.
  //
  //  This is ergonomics, not expressiveness: the language is already complete
  //  without it, because `try_from` covers everything. It is deliberately the
  //  last thing built, and deliberately does not try to be clever — nothing
  //  narrows through a function call, a closure, or an `&mut`.
  // ------------------------------------------------------------------------

  /// Assumes a condition (or its negation) for the region it dominates.
  auto assume_condition(const ast::expr *condition, bool negated) -> void {
    // An `if let` plants an `error_expr` in `condition` on every parse (see
    // `param_usage_inferrer::walk_expr`); it says nothing about any value.
    if (condition == nullptr || condition->has_error) {
      return;
    }
    for (auto &fact : facts_from(*condition, predicate_subst{}, negated)) {
      facts_.push_back(std::move(fact));
    }
  }

  /// Whether a solver atom is *about* `name` — the name itself (`x`), a
  /// projection of it (`x.len`), or a call over it (`len(x)`, `f(x, y)`).
  /// Used to decide what an assignment invalidates, so it errs toward
  /// forgetting too much rather than too little: a fact wrongly kept is
  /// unsound, a fact wrongly dropped merely costs a proof.
  static auto atom_mentions(std::string_view atom, std::string_view name)
      -> bool {
    for (size_t at = atom.find(name); at != std::string_view::npos;
         at = atom.find(name, at + 1)) {
      const auto before = at == 0 ? ' ' : atom[at - 1];
      const auto after =
          at + name.size() >= atom.size() ? ' ' : atom[at + name.size()];
      const auto is_name_char = [](char c) -> bool {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
      };
      if (!is_name_char(before) && !is_name_char(after)) {
        return true;
      }
    }
    return false;
  }

  /// Forgets everything known about `name`. Called when a value is assigned
  /// to, or lent mutably — at which point every fact that mentioned it
  /// describes a value that no longer exists.
  auto invalidate_facts(std::string_view name) -> void {
    if (name.empty()) {
      return;
    }
    std::erase_if(facts_, [&](const constraint &fact) -> bool {
      return std::ranges::any_of(fact.poly.terms,
                                 [&](const poly_term &term) -> bool {
                                   return atom_mentions(term.var, name);
                                 });
    });
  }

  /// Builds the facts that hold on entry to a function body, in the order the
  /// design doc lays out (§5.1): value-parameter domains, then each
  /// parameter's type, then the preconditions. A `pre` is an *obligation* at
  /// every call site and a *fact* inside the callee — that duality is the
  /// whole of contract reasoning, and this is the half of it that assumes.
  auto collect_function_facts(const ast::func_decl &decl) -> void {
    for (const auto &param : decl.type_params) {
      if (!param.is_value_param || param.name.empty()) {
        continue;
      }
      const auto declared =
          param.bound_or_type != nullptr
              ? resolve_type(*param.bound_or_type, current_resolve_ctx())
              : types_.usize_type();
      const auto &entry = types_.entry(declared);
      const auto unsigned_domain =
          entry.kind == type_kind::builtin_kind &&
          (entry.name.starts_with("uint") || entry.name == "usize" ||
           entry.name == "byte");
      if (unsigned_domain) {
        facts_.push_back(constraint{
            .poly = poly_variable(param.name),
            .rel = relation::ge,
            .label = std::format("{} >= 0", param.name),
        });
      }
    }

    for (const auto &param : decl.params) {
      if (param.pattern == nullptr ||
          param.pattern->kind != ast::node_kind::binding_pattern) {
        continue;
      }
      const auto &binding =
          dynamic_cast<const ast::binding_pattern &>(*param.pattern);
      if (const auto *found = lookup_value(binding.name)) {
        assume_binding(binding.name, found->type);
      }
    }

    // Each `pre` is now weighed against everything already known — the
    // parameters' own types (a refinement or a `usize` domain is a fact
    // about every value that could ever be passed) plus the preconditions
    // ahead of it, which are checked before it at run time and so hold by
    // the time it is evaluated. A `pre` that follows from those needs no
    // runtime check: it cannot fail on any call, from anywhere, for the same
    // reason `p: positive` makes `pre p > 0` say nothing new. That — and not
    // "some call site happened to satisfy it" — is what makes an elision
    // sound for a check the callee performs once on entry, on behalf of
    // callers it may never see (`checked_types::elided_contracts`).
    for (const auto &contract : decl.contracts) {
      if (!contract.is_pre || contract.condition == nullptr ||
          contract.condition->has_error) {
        continue;
      }
      if (const auto goal =
              goal_from(*contract.condition, predicate_subst{}, false);
          !goal.empty() && solve(facts_, goal) == proof_result::proved) {
        elided_contracts_.insert(&contract);
      }
      for (auto &fact :
           facts_from(*contract.condition, predicate_subst{}, false)) {
        facts_.push_back(std::move(fact));
      }
    }
  }

  /// Whether `found` already carries at least the predicate `expected`
  /// demands — the same refinement, or one layered on top of it.
  auto refines(type_id found, type_id expected) -> bool {
    for (auto current = found;
         types_.entry(current).kind == type_kind::refinement_kind;
         current = types_.entry(current).result) {
      if (current == expected) {
        return true;
      }
    }
    return false;
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
    emit_diag(diag);
    mark_error();
  }

  /// Checks generic-argument arity against `decl.type_params` (when
  /// arguments were written at all — omitting them entirely is allowed and
  /// leaves parameters `unknown`), then builds the instantiated type.
  auto instantiate_user_type(const ast::type_decl &decl,
                             std::string_view owner_module,
                             const ast::named_type &named,
                             const resolve_ctx &ctx) -> type_id {
    // Kind-directed: a bare generic user type filling a higher-kinded slot
    // names the constructor itself, exactly as a bare prelude generic does.
    if (named.type_args.empty() && ctx.expected_ctor_arity.has_value() &&
        !decl.type_params.empty()) {
      return check_ctor_against_expected(
          types_.ctor_ref(decl.name, owner_module, &decl,
                          decl.type_params.size()),
          decl.name, decl.type_params.size(), named.span, ctx);
    }
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
      emit_diag(diag);
      mark_error();
      return k_error_type;
    }

    return make_user_type(decl, owner_module, args);
  }

  /// The parameter substitution a declaration's own body is resolved under:
  /// each declared parameter bound to the argument supplied for it (or left
  /// `unknown` when the caller wrote no arguments at all, which is legal).
  auto bindings_for_decl(const ast::type_decl &decl,
                         const std::vector<type_id> &args)
      -> std::unordered_map<std::string, type_id> {
    auto bindings = std::unordered_map<std::string, type_id>{};
    for (size_t i = 0; i < decl.type_params.size(); ++i) {
      bindings.emplace(decl.type_params[i].name,
                       i < args.size() ? args[i] : k_unknown_type);
    }
    return bindings;
  }

  /// Mints the nominal type of a refinement *declaration*
  /// (`type index[n: usize] = usize where self < n`). Declaration-nominal, so
  /// `index[n]` is spelled `index[n]` in every diagnostic and stays distinct
  /// from any other refinement of `usize` — while the predicate rides along
  /// on the type entry for the solver, which sees straight through the name
  /// (`spec/dependent-types-design.md` §3).
  auto make_refinement_type(const ast::type_decl &decl,
                            std::string_view owner_module,
                            std::vector<type_id> args) -> type_id {
    const auto &refinement =
        dynamic_cast<const ast::refinement_type &>(*decl.definition);

    auto bindings = bindings_for_decl(decl, args);
    const auto *owner = index_.find_module(owner_module);
    const auto ctx = resolve_ctx{.module = owner != nullptr ? owner : module_,
                                 .param_bindings = &bindings,
                                 .use_type_param_stack = false,
                                 .quiet = true};
    const auto base = refinement.base != nullptr
                          ? resolve_type(*refinement.base, ctx)
                          : k_unknown_type;

    auto display_name = decl.name;
    if (!args.empty()) {
      display_name += '[';
      for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
          display_name += ", ";
        }
        display_name += types_.display(args[i]);
      }
      display_name += ']';
    }

    const auto *predicate =
        dynamic_cast<const ast::expr *>(refinement.predicate.get());
    return types_.refinement_of(
        &decl, display_name, owner_module, base, std::move(args),
        predicate != nullptr && !predicate->has_error ? predicate : nullptr);
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

    // A refinement declaration is a type in its own right, not an alias for
    // its base — that is the whole of Phase 2.
    if (decl.definition->kind == ast::node_kind::refinement_type) {
      if (!aliases_in_progress_.insert(&decl).second) {
        return k_unknown_type; // cycle; reported by declaration checking
      }
      const auto result = make_refinement_type(decl, owner_module, args);
      aliases_in_progress_.erase(&decl);
      return result;
    }

    // Alias (`type meters = float64`) — expand to the target.
    if (!aliases_in_progress_.insert(&decl).second) {
      return k_unknown_type; // alias cycle; reported by declaration checking
    }
    auto param_bindings = bindings_for_decl(decl, args);
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

  /// `ctx` with any constructor expectation cleared, for resolving the
  /// *arguments* of an application — an argument position is an ordinary
  /// type position even when the application itself fills a higher-kinded
  /// slot.
  auto argument_ctx(const resolve_ctx &ctx) -> resolve_ctx {
    auto inner = ctx;
    inner.expected_ctor_arity = std::nullopt;
    return inner;
  }

  /// Checks a resolved constructor (or constructor-kinded parameter) against
  /// the arity the surrounding higher-kinded slot expects. Returns the
  /// constructor's own id on a match; otherwise explains the kind mismatch —
  /// what was required, what was found, and how to bridge the gap — and
  /// returns `k_error_type`.
  auto check_ctor_against_expected(type_id head, std::string_view name,
                                   size_t arity, source_span span,
                                   const resolve_ctx &ctx) -> type_id {
    const auto expected = *ctx.expected_ctor_arity;
    if (arity == expected) {
      return head;
    }
    if (!ctx.quiet) {
      const auto placeholders = [](size_t n) -> std::string {
        auto out = std::string{"[_"};
        for (size_t i = 1; i < n; ++i) {
          out += ", _";
        }
        return out + "]";
      };
      error_with_help(
          span,
          std::format("`{}` takes {} type argument{}, but this position "
                      "requires a {}-argument type constructor (`{}`)",
                      name, arity, arity == 1 ? "" : "s", expected,
                      placeholders(expected)),
          "type constructor of the wrong kind",
          std::format("A higher-kinded parameter is satisfied only by a "
                      "constructor of exactly matching arity. Wrap `{}` in a "
                      "dedicated {}-parameter struct or sum type and use "
                      "that wrapper here instead.",
                      name, expected));
    }
    return ctx.quiet ? k_unknown_type : k_error_type;
  }

  /// Finishes resolving a name that resolved to an in-scope generic
  /// parameter (possibly higher-kinded) or to a constructor substituted for
  /// one (trait-default monomorphization binding `F := option`). This is
  /// where every kind rule is enforced: a kind-`*` parameter takes no
  /// arguments, a constructor parameter used bare is not a type, and an
  /// application must supply exactly the declared arity. Applying a
  /// *concrete* constructor re-enters the ordinary interning path, so
  /// `F[A]` under `F := option` yields the same `type_id` ordinary
  /// `option[A]` code gets — the id-equality invariant substitution must
  /// preserve.
  auto apply_resolved_head(type_id head, const ast::named_type &named,
                           const resolve_ctx &ctx) -> type_id {
    // Copied, not referenced: interning below can push new entries.
    const auto entry = types_.entry(head);

    if (entry.kind == type_kind::ctor_ref_kind) {
      if (named.type_args.empty()) {
        if (ctx.expected_ctor_arity.has_value()) {
          return check_ctor_against_expected(head, entry.name, entry.ctor_arity,
                                             named.span, ctx);
        }
        if (!ctx.quiet) {
          error_with_help(
              named.span,
              std::format("`{}` is a type constructor, not a type", entry.name),
              "unapplied type constructor in type position",
              std::format("Apply it to {} type argument{} — e.g. "
                          "`{}[...]` — to name a concrete type.",
                          entry.ctor_arity, entry.ctor_arity == 1 ? "" : "s",
                          entry.name));
        }
        return ctx.quiet ? k_unknown_type : k_error_type;
      }
      if (named.type_args.size() != entry.ctor_arity) {
        if (!ctx.quiet) {
          error(named.span,
                std::format("type `{}` expects {} type argument{}, found {}",
                            entry.name, entry.ctor_arity,
                            entry.ctor_arity == 1 ? "" : "s",
                            named.type_args.size()),
                "wrong number of type arguments");
        }
        return ctx.quiet ? k_unknown_type : k_error_type;
      }
      if (entry.decl != nullptr) {
        return instantiate_user_type(*entry.decl, entry.module_name, named,
                                     argument_ctx(ctx));
      }
      return types_.builtin_generic(
          entry.name, resolve_type_args(named, argument_ctx(ctx)));
    }

    if (entry.kind != type_kind::type_param_kind) {
      // A value-parameter slot or an already-substituted concrete type —
      // nothing kind-shaped to check here.
      return head;
    }

    const auto arity = entry.ctor_arity;
    if (named.type_args.empty()) {
      if (arity == 0) {
        return head;
      }
      if (ctx.expected_ctor_arity.has_value()) {
        return check_ctor_against_expected(head, entry.name, arity, named.span,
                                           ctx);
      }
      if (!ctx.quiet) {
        error_with_help(
            named.span,
            std::format("`{}` is a {}-argument type constructor, not a type",
                        entry.name, arity),
            "higher-kinded parameter used as a type",
            std::format("`{}` was declared as `{}[{}]`, so it names a type "
                        "only once applied — write `{}[...]` with {} type "
                        "argument{}.",
                        entry.name, entry.name, arity == 1 ? "_" : "_, _",
                        entry.name, arity, arity == 1 ? "" : "s"));
      }
      return ctx.quiet ? k_unknown_type : k_error_type;
    }

    if (arity == 0) {
      if (!ctx.quiet) {
        error_with_help(
            named.span,
            std::format("type parameter `{}` does not take type arguments",
                        entry.name),
            "kind mismatch: applying an ordinary type parameter",
            std::format("`{}` stands for a complete type. If it should stand "
                        "for a type *constructor* like `option`, declare it "
                        "higher-kinded: `{}[_]`.",
                        entry.name, entry.name));
      }
      return ctx.quiet ? k_unknown_type : k_error_type;
    }
    if (named.type_args.size() != arity) {
      if (!ctx.quiet) {
        error(named.span,
              std::format("`{}` expects {} type argument{}, found {}",
                          entry.name, arity, arity == 1 ? "" : "s",
                          named.type_args.size()),
              "wrong number of type arguments");
      }
      return ctx.quiet ? k_unknown_type : k_error_type;
    }
    return types_.param_app(head, resolve_type_args(named, argument_ctx(ctx)));
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
      // A leading import alias (`use pkg.db as db` → `db.conn`): rewrite the
      // alias to the aliased module's absolute path and resolve there. This is
      // what makes a functor's `DB.conn` projection resolve, where `DB` is the
      // module parameter bound to the argument module as an import alias.
      if (const auto *binding = find_import(named.path.front());
          binding != nullptr && binding->leaf_name.empty()) {
        // The module the alias' head names. A whole-module import — a functor
        // parameter `DB` bound to `postgres`, or a plain `use pkg.sub` — names
        // a module directly (its `path`); a renamed member import falls back
        // to the member's owning module. `import_source_module` deliberately
        // returns null for a whole-module import (its members are reached by
        // path), so it cannot be the only source consulted here.
        const module_members *aliased =
            index_.find_module(join_strings(binding->path, "."));
        if (aliased == nullptr) {
          aliased = import_source_module(*binding);
        }
        if (aliased != nullptr) {
          auto absolute = split_module_name(aliased->module_name);
          absolute.insert(absolute.end(), named.path.begin() + 1,
                          named.path.end());
          if (const auto *owner = find_session_module_of_path(absolute)) {
            const auto &member = absolute.back();
            if (const auto it = owner->types.find(member);
                it != owner->types.end()) {
              return instantiate_user_type(*it->second.decl, owner->module_name,
                                           named, ctx);
            }
          }
        }
      }
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

    // Generic parameters in scope. `apply_resolved_head` owns every kind
    // rule: bare kind-`*` params pass through, `F[A]` becomes a parameter
    // application (or, under a concrete substitution, the real applied
    // type), and misuse gets a kind diagnostic.
    if (ctx.param_bindings != nullptr) {
      if (const auto it = ctx.param_bindings->find(name);
          it != ctx.param_bindings->end()) {
        return apply_resolved_head(it->second, named, ctx);
      }
    }
    if (ctx.use_type_param_stack) {
      if (const auto param = lookup_type_param(name)) {
        return apply_resolved_head(*param, named, ctx);
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
      // `array[T, n]` through named-type syntax — the element type in the
      // first slot, the length value in the second.
      const auto *element_node = named.type_args.empty()
                                     ? nullptr
                                     : dynamic_cast<const ast::type_expr *>(
                                           named.type_args.front().value.get());
      const auto element = element_node != nullptr
                               ? resolve_type(*element_node, ctx)
                               : k_unknown_type;
      const auto length =
          named.type_args.size() < 2 || named.type_args[1].value == nullptr
              ? k_unknown_type
              : resolve_length_arg(*named.type_args[1].value, ctx);
      return array_with_length(element, length);
    }
    if (const auto arity = builtin_generic_arity(name)) {
      // Kind-directed: where a higher-kinded slot is being filled, a bare
      // prelude generic names the *constructor* itself (`monad[option]`,
      // `impl monad for option`), not an instantiation.
      if (named.type_args.empty() && ctx.expected_ctor_arity.has_value()) {
        return check_ctor_against_expected(
            types_.ctor_ref(name, "", nullptr, arity->first), name,
            arity->first, named.span, ctx);
      }
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

    // Names re-exported by a session-owned wildcard import (`use a.b.*`).
    for (const auto *source : wildcard_import_sources()) {
      if (const auto it = source->types.find(std::string(name));
          it != source->types.end()) {
        return instantiate_user_type(*it->second.decl, source->module_name,
                                     named, ctx);
      }
      if (source->traits.contains(std::string(name)) ||
          source->concepts.contains(std::string(name))) {
        return k_unknown_type; // trait/concept used in a bound position
      }
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

  /// The type a value *acts* as once every compile-time-only wrapper is
  /// peeled away: the target behind a reference, and the base behind a
  /// refinement. What an operator sees, in other words. The one thing that
  /// must never see through a refinement is a narrowing coercion, which is
  /// the obligation itself.
  auto base_shape(type_id id) -> type_id {
    return types_.strip_refinement(strip_refs(types_.strip_refinement(id)));
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

  /// The rigid `type_param` stand-ins a declaration's signature resolves
  /// against: its own parameters, plus — when it is a method — those of the
  /// `impl` or `extend` block enclosing it, which are equally in scope over the
  /// signature and are the only thing distinguishing `impl[T] ... for
  /// holder[T]` from an impl for one concrete instantiation.
  auto generic_param_bindings(
      const ast::func_decl &decl,
      const std::vector<ast::type_param> *enclosing_block_params)
      -> std::unordered_map<std::string, type_id> {
    auto bindings = std::unordered_map<std::string, type_id>{};
    const auto seed = [&](const ast::type_param &type_param) -> void {
      if (!type_param.name.empty()) {
        bindings.emplace(
            type_param.name,
            types_.type_param(type_param.name, type_param.higher_kinded_arity));
      }
    };
    if (enclosing_block_params != nullptr) {
      for (const auto &type_param : *enclosing_block_params) {
        seed(type_param);
      }
    }
    // Order between the two is immaterial: a method parameter shadowing an
    // impl one of the same name resolves to a `type_param` stand-in that is
    // interned identically either way. What matters is only that both sets
    // are present.
    for (const auto &type_param : decl.type_params) {
      seed(type_param);
    }
    return bindings;
  }

  /// Normalizes a function declaration's parameter list into
  /// `fn_param_info`s with their types resolved against the function's own
  /// generic parameters. `skip_self` drops a leading `self` parameter,
  /// since call-argument checking never expects the caller to pass it.
  ///
  /// `enclosing_block_params` seeds the enclosing `impl`/`extend` block's
  /// parameters as well. A method of `impl[T] get_it[T] for holder[T]`
  /// mentions `T` in its signature without declaring it — the parameter
  /// belongs to the block — so resolving the signature against
  /// `decl.type_params` alone leaves `T` unresolvable, and the method reads as
  /// if it had no generic content at all.
  auto signature_params(
      const ast::func_decl &decl, const module_members *owner, bool skip_self,
      const std::vector<ast::type_param> *enclosing_block_params = nullptr)
      -> std::vector<fn_param_info> {
    auto param_bindings = generic_param_bindings(decl, enclosing_block_params);
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
  auto signature_return_type(
      const ast::func_decl &decl, const module_members *owner,
      const std::vector<ast::type_param> *enclosing_block_params = nullptr)
      -> type_id {
    if (decl.return_type == nullptr) {
      return k_unknown_type;
    }
    auto param_bindings = generic_param_bindings(decl, enclosing_block_params);
    const auto ctx =
        resolve_ctx{.module = owner,
                    .param_bindings = &param_bindings,
                    .use_type_param_stack = false,
                    .quiet = true,
                    .existential_allowed = true,
                    .is_generator_return = decl.modifiers.is_generator};
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
  /// `solved_out`, when given, receives the callee's value parameters as the
  /// arguments determined them (`solve_value_params`) — the same bindings the
  /// refinement obligations below are discharged under, handed back so
  /// `check_call_against_decl` can monomorphize the callee with them.
  auto check_call_args_against(const ast::call_expr &call,
                               const std::vector<fn_param_info> &params,
                               std::string_view callee_name,
                               source_location decl_location,
                               value_bindings *solved_out = nullptr,
                               const generic_call_context *generic = nullptr)
      -> void {
    auto param_used = std::vector<bool>(params.size(), false);
    auto args_by_param = std::vector<const ast::expr *>(params.size(), nullptr);
    auto next_positional = size_t{0};
    auto seen_named = false;
    // See the deferral comment in the argument loop below.
    auto solved_values = value_bindings{};
    auto deferred = std::vector<deferred_narrowing>{};
    auto pending = std::vector<pending_argument>{};

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
          emit_diag(diag);
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
        pending.push_back(
            pending_argument{.value = arg.value.get(), .target = target});
      }
    }

    // `call_argument_mappings_` is published before the arguments are
    // checked, not after, because the generic solving between the two passes
    // below reads it back to find which argument reached which parameter.
    call_argument_mappings_[&call] =
        call_argument_mapping{.args_by_param = args_by_param};

    const auto check_argument =
        [&](const pending_argument &item,
            const std::unordered_map<std::string, type_id> &bindings) -> void {
      const auto expected = item.target != nullptr
                                ? substitute_solved(item.target->type, bindings)
                                : k_unknown_type;
      const auto found = infer_expr(*item.value, expected);
      if (item.target == nullptr) {
        return;
      }
      // Structure is decided now; a *proof* has to wait. A refined
      // parameter's predicate is written in the callee's value parameters
      // (`index[n]`'s `n`), and which arguments pin those down isn't known
      // until every argument has been seen — `safe_get(v, i)` learns `n`
      // from `v`, which for a differently-ordered signature could just as
      // easily come after `i`. So the shape check runs in place and the
      // obligation is collected for the second pass below.
      type_mismatch(item.value->span, expected, found, "for this argument");
      solve_value_params(expected, found, solved_values);
      if (types_.entry(expected).kind == type_kind::refinement_kind) {
        deferred.push_back(deferred_narrowing{
            .expected = expected,
            .found = found,
            .value = item.value,
            .span = item.value->span,
        });
      }
    };

    // Lambda arguments go last, and against a *substituted* parameter type.
    //
    // A lambda is the one argument whose type is decided by what is expected
    // of it rather than by what it contains: `x => x * 2` passed to an
    // `f: fn(T) -> U` has no type of its own until `T` is known. Checked in
    // declaration order it would be inferred against the literal `fn(T) -> U`
    // and bind its parameter to the type *parameter* `T`, which then reaches
    // lowering as an abstract type and fails there rather than here ("type
    // `T` has no scalar bytecode representation"). Every other argument
    // determines its own type, so running those first — and solving the call's
    // generic parameters from them plus the declared bounds — is what makes
    // `T` known by the time the lambda needs it.
    //
    // Each argument is still inferred exactly once. This reorders when the
    // lambdas are visited, it does not visit them twice: `infer_expr` mints
    // symbols and declares locals, and running it twice over one lambda would
    // register its parameters two times over.
    auto no_bindings = std::unordered_map<std::string, type_id>{};
    for (const auto &item : pending) {
      if (item.value->kind != ast::node_kind::lambda_expr) {
        check_argument(item, no_bindings);
      }
    }
    auto bindings = std::unordered_map<std::string, type_id>{};
    if (generic != nullptr) {
      bindings = preliminary_type_bindings(call, params, *generic);
    }
    for (const auto &item : pending) {
      if (item.value->kind == ast::node_kind::lambda_expr) {
        check_argument(item, bindings);
      }
    }

    for (const auto &obligation : deferred) {
      check_narrowing(obligation.expected, obligation.found, obligation.value,
                      obligation.span, "for this argument", &solved_values);
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
      emit_diag(diag);
      mark_error();
    }

    if (solved_out != nullptr) {
      *solved_out = std::move(solved_values);
    }
  }

  /// Checks a call against a known `func_decl`: enforces the
  /// contract-purity rule when inside a contract condition, checks its
  /// arguments via `check_call_args_against`, and returns its return type.
  ///
  /// `explicit_args` are the compile-time arguments the call gave in brackets
  /// (`zeros[8]()`), empty for the ordinary bare-callee call.
  auto check_call_against_decl(const ast::call_expr &call,
                               const ast::func_decl &decl,
                               const module_members *owner,
                               file_id_type decl_file, bool skip_self,
                               const explicit_generic_args &explicit_args = {})
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
    auto solved = value_bindings{};
    const auto generic = generic_call_context{.decl = &decl,
                                              .owner = owner,
                                              .explicit_args = &explicit_args,
                                              .ufcs_receiver = nullptr};
    check_call_args_against(
        call, params, decl.name,
        source_location{.file_id = decl_file, .span = decl.span}, &solved,
        is_generic_template(decl) ? &generic : nullptr);
    check_call_preconditions(call, decl, params);
    // Not from inside a template: a call there is written in terms of
    // parameters that are still symbols (`get(v, i)` inside a function generic
    // over `n` passes an `array[int32, n]`; `wrap(x)` inside one generic over
    // `T` passes an abstract `T`), so there is nothing concrete to instantiate
    // the callee with — and no need for one. The template is never lowered;
    // its *instances* are, and this same call, re-checked in each of them,
    // instantiates the callee for that instance's arguments.
    //
    // Value parameters and type parameters take the same road, together: a
    // template mixing them (`[n: usize, T]`) is one solution with both kinds
    // of binding in it, not a third case.
    if (!in_const_generic_template_ && !in_type_generic_template_ &&
        is_generic_template(decl) && is_free_function(decl, owner)) {
      if (const auto result = instantiate_generic_function(
              call, decl, owner, decl_file, solved, params, explicit_args)) {
        return *result;
      }
    }
    return signature_return_type(decl, owner);
  }

  // ==========================================================================
  //  Const-generic monomorphization
  //  (`spec/dependent-types-design.md` §3.1, `spec/dependent-types-
  //   implementation-plan.md` phase 1)
  //
  //  A value parameter is a compile-time argument with no runtime existence:
  //  `def get[n: usize](v: array[int32, n], i: index[n])` is passed a `v` and
  //  an `i`, never an `n`. Everything in the body that *mentions* `n` — the
  //  array's length, the refinement's bound, the check an `index[n].try_from`
  //  compiles into — therefore has nothing to compile against while `n` is
  //  still a symbol.
  //
  //  So the template is never lowered. Instead, each call site whose arguments
  //  pin `n` down to a constant (`solve_value_params`, already run to check
  //  the arguments) gets an *instance*: the declaration cloned, re-checked
  //  with `n` bound to that constant, and named `get$3`. Instances are shared
  //  per constant tuple, so a call in a loop compiles one of them.
  // ==========================================================================

  /// Whether `decl` is generic over compile-time *values* only — the case
  /// that monomorphizes here — generic over compile-time *values*
  /// (`[n: usize]`), over *types* (`[T]`), or over both at once
  /// (`[n: usize, T]`). All three are one case: a template with no compiled
  /// form of its own, whose every parameter one call pins down to a constant
  /// or a concrete type, yielding an instance that is what actually gets
  /// lowered.
  [[nodiscard]] static auto is_generic_template(const ast::func_decl &decl)
      -> bool {
    return !decl.type_params.empty() &&
           std::ranges::all_of(decl.type_params, [](const auto &param) -> bool {
             return !param.name.empty();
           });
  }

  /// Whether `decl` is a module-level free function of `owner` — the shape
  /// instances are lowered as by their bare name (`hir::lower_module`). A
  /// *method* is instantiated too, but through the receiver-call path, which
  /// prefixes its impl target into the instance name (`vec::at$3`).
  [[nodiscard]] auto is_free_function(const ast::func_decl &decl,
                                      const module_members *owner) const
      -> bool {
    if (owner == nullptr) {
      return false;
    }
    const auto it = owner->functions.find(decl.name);
    return it != owner->functions.end() && it->second.decl == &decl;
  }

  // ------------------------------------------------------------------
  //  One solution, one instance
  //
  //  Value parameters and type parameters are two answers to the same
  //  question — what does this call pin the template's parameters down to —
  //  and the checker already had two nearly identical machines for them.
  //  They are one machine below. A parameter's answer is a constant or a
  //  concrete type; either way it becomes a binding the instance's body is
  //  re-checked under (`const_param_slots_` / `type_param_slots_`, which
  //  `push_type_params` consults side by side) and a segment of the
  //  instance's name. A template mixing the two (`[n: usize, T]`) is then
  //  not a special case at all — it is just a solution with both kinds of
  //  binding in it.
  // ------------------------------------------------------------------

  /// What one call pins a template's parameters down to: a constant per value
  /// parameter, a concrete type per type parameter, and the name the instance
  /// carrying that solution is compiled under.
  struct generic_solution {
    /// Value parameters, as the interned `const_value` types
    /// `push_type_params` binds them to.
    std::unordered_map<std::string, type_id> const_slots;
    /// Value parameters, as the raw numbers `lower_ident` embeds.
    std::unordered_map<std::string, int64_t> values;
    /// Type parameters, as the concrete types they solved to.
    std::unordered_map<std::string, type_id> type_slots;
    /// Every parameter's answer, in declaration order, as a symbol-safe
    /// suffix — `$3`, `$int32`, `$3$int32` for a mixed template.
    std::string suffix;
  };

  /// One parameter's answer, appended to `solution` in declaration order.
  auto bind_generic_constant(generic_solution &solution,
                             const ast::type_param &param, type_id underlying,
                             int64_t constant) -> void {
    solution.const_slots.emplace(
        param.name,
        types_.const_value(underlying, static_cast<uint64_t>(constant)));
    solution.values.emplace(param.name, constant);
    // A negative constant spells its minus as `m` (`shift$m1`), keeping the
    // name to characters every backend's symbol table accepts.
    solution.suffix += constant < 0 ? std::format("$m{}", -constant)
                                    : std::format("${}", constant);
  }

  auto bind_generic_type(generic_solution &solution,
                         const ast::type_param &param, type_id solved) -> void {
    solution.type_slots.emplace(param.name, solved);
    solution.suffix += std::format("${}", mangle_type_for_instance(solved));
  }

  /// How to describe, in a diagnostic, what a copy of `decl` is made *per* —
  /// read off the template's own parameters so a mixed template says both.
  [[nodiscard]] static auto instance_cardinality(const ast::func_decl &decl)
      -> std::string_view {
    const auto has_value =
        std::ranges::any_of(decl.type_params, [](const auto &param) -> bool {
          return param.is_value_param;
        });
    const auto has_type =
        std::ranges::any_of(decl.type_params, [](const auto &param) -> bool {
          return !param.is_value_param;
        });
    if (has_value && has_type) {
      return "one per combination of constant and concrete type";
    }
    return has_value ? "one per constant" : "one per concrete type";
  }

  /// The instance for one (template, solution) pair — reused if it already
  /// exists, cloned and checked under the solution's bindings if it doesn't.
  /// This is the single place an instance comes into being, for free
  /// functions and impl/extend methods alike: to lowering all of them are the
  /// same thing, a checked clone named for its instantiation, lowered once in
  /// its owning module while the template itself is skipped.
  ///
  /// The cache entry is published *before* the clone's body is checked, and
  /// that ordering is what makes an ordinarily recursive generic function
  /// work: the recursive call inside the body finds the instance it is
  /// currently inside, instead of instantiating a second copy of it forever.
  /// A body that recurses at a *different* argument each time (`f[n]` calling
  /// `f[n + 1]`) has no such fixed point, and is caught by the depth bound
  /// instead.
  ///
  /// `decl_file` is `nullopt` for a method, whose `method_entry` records no
  /// declaring file; an instance-check diagnostic then points at the
  /// instantiating call, which is at least actionable. `fixed_type_params`
  /// carries a trait default's own bindings (`F := option`), which scope the
  /// whole instance check.
  ///
  /// `self_type` is the impl/extend target for a `self`-taking method, whose
  /// body names `self` without ever annotating it — `check_function` reads
  /// that type out of `self_type_`, so an instance checked with it unset
  /// would find no type for `self` at all. `k_unknown_type` for a free
  /// function, which has no receiver to bind.
  /// `site` is the syntax that asked for this instance. It is a `node` rather
  /// than a `call_expr` because not every instantiation comes from a written
  /// call: `for x in adapter:` desugars to `adapter.next()`, and the `next`
  /// it needs a compiled copy of is reached from the `for` statement itself
  /// (`try_resolve_iterator`). Only the span and the map identity are used,
  /// both of which every node has.
  auto find_or_check_generic_instance(
      const ast::node &call, const ast::func_decl &decl,
      const module_members *owner, std::optional<file_id_type> decl_file,
      const generic_solution &solution, const std::string &name,
      const std::unordered_map<std::string, type_id> *fixed_type_params,
      type_id self_type = k_unknown_type) -> const ast::func_decl * {
    const auto key =
        std::format("{}#{}", static_cast<const void *>(&decl), name);
    if (const auto found = hk_instance_cache_.find(key);
        found != hk_instance_cache_.end()) {
      return found->second;
    }

    static constexpr size_t k_max_instantiation_depth = 32;
    if (instantiation_depth_ >= k_max_instantiation_depth) {
      error_with_help(
          call.span,
          std::format("`{}` instantiates itself endlessly", decl.name),
          "generic instantiation is too deep",
          std::format("Each call with a new compile-time argument compiles a "
                      "new copy of `{}`, so a body that calls itself at a "
                      "*different* argument every time (`{}[n]` calling "
                      "`{}[n + 1]`, say) never bottoms out. Recurse at an "
                      "argument that eventually repeats, or make the "
                      "recursion an ordinary runtime one over a parameter you "
                      "don't change.",
                      decl.name, decl.name, decl.name));
      return nullptr;
    }

    auto cloned = ast::clone_func_decl(decl);
    if (!cloned.has_value()) {
      auto diag =
          diagnostic(diagnostic_level::error,
                     std::format("cannot compile `{}` for this call: its body "
                                 "uses a construct generic instantiation "
                                 "doesn't support yet",
                                 decl.name),
                     file_id_);
      diag.with_label(call.span,
                      std::format("this call needs a compiled copy of `{}`, {}",
                                  decl.name, instance_cardinality(decl)));
      diag.children.push_back(
          diagnostic(diagnostic_level::note, cloned.error().message,
                     decl_file.value_or(file_id_))
              .with_label(cloned.error().span, "unsupported here"));
      emit_diag(diag);
      mark_error();
      return nullptr;
    }

    (*cloned)->name = name;
    const auto *instance = cloned->get();
    synthesized_decls_.push_back(std::move(*cloned));
    hk_instance_cache_.emplace(key, instance);

    const auto saved_module = module_;
    const auto saved_file_id = file_id_;
    auto saved_const_slots = std::move(const_param_slots_);
    auto saved_values = std::move(const_param_values_);
    auto saved_type_slots = std::move(type_param_slots_);
    const auto saved_contract = in_contract_;
    const auto saved_postcondition = in_postcondition_;
    const auto saved_self_type = self_type_;
    module_ = owner;
    if (!types_.is_unknown(self_type)) {
      self_type_ = self_type;
    }
    if (decl_file.has_value()) {
      file_id_ = *decl_file;
    }
    // Both kinds of binding go in together: `push_type_params` consults the
    // two maps side by side, so a mixed template's `n` interns as its
    // constant and its `T` as its concrete type in the same scope.
    const_param_slots_ = solution.const_slots;
    const_param_values_ = solution.values;
    type_param_slots_ = solution.type_slots;
    in_contract_ = false;
    in_postcondition_ = false;
    if (fixed_type_params != nullptr && !fixed_type_params->empty()) {
      type_params_.push_back(*fixed_type_params);
    }
    // Recorded against the *caller's* file, captured before `file_id_` is
    // swapped to the declaring one above — the whole point of the note is to
    // name a line the user actually wrote.
    auto frame = instantiation_frame{.call_span = call.span,
                                     .call_file = saved_file_id,
                                     .instance_name = name};
    if (const auto solved = expected_solved_params_.find(&call);
        solved != expected_solved_params_.end()) {
      frame.context_solutions = solved->second;
    }
    instantiation_sites_.push_back(std::move(frame));
    ++instantiation_depth_;
    check_function(*instance, /*at_module_scope=*/false);
    --instantiation_depth_;
    instantiation_sites_.pop_back();
    if (fixed_type_params != nullptr && !fixed_type_params->empty()) {
      pop_type_params();
    }
    self_type_ = saved_self_type;
    in_postcondition_ = saved_postcondition;
    in_contract_ = saved_contract;
    type_param_slots_ = std::move(saved_type_slots);
    const_param_values_ = std::move(saved_values);
    const_param_slots_ = std::move(saved_const_slots);
    file_id_ = saved_file_id;
    module_ = saved_module;

    const_generic_instances_.push_back(const_generic_instance{
        .decl = instance, .owner_module = owner->module_name});
    return instance;
  }

  /// Reads the explicit compile-time arguments out of a call's callee, and
  /// hands back the identifier the callee really names.
  ///
  /// The parser can't tell `values[0]` from `zeros[8]` and doesn't try
  /// (`parse_postfix`): a single unnamed bracket argument becomes an
  /// `index_expr`, several become a `call_expr`. Both shapes mean explicit
  /// instantiation when the base turns out to name a generic declaration,
  /// which only the checker knows — so both are unwrapped here.
  auto explicit_generic_callee(const ast::expr &callee,
                               explicit_generic_args &args_out)
      -> const ast::expr * {
    if (callee.kind == ast::node_kind::index_expr) {
      const auto &index = dynamic_cast<const ast::index_expr &>(callee);
      if (index.object == nullptr || index.index == nullptr) {
        return nullptr;
      }
      args_out.push_back(explicit_generic_arg{.value = index.index.get(),
                                              .span = index.index->span});
      return index.object.get();
    }
    if (callee.kind == ast::node_kind::call_expr) {
      const auto &applied = dynamic_cast<const ast::call_expr &>(callee);
      if (applied.callee == nullptr) {
        return nullptr;
      }
      for (const auto &arg : applied.args) {
        if (arg.value == nullptr || arg.name.has_value()) {
          return nullptr; // a named bracket argument isn't this shape
        }
        args_out.push_back(explicit_generic_arg{.value = arg.value.get(),
                                                .span = arg.value->span});
      }
      return applied.callee.get();
    }
    return nullptr;
  }

  /// Resolves one explicit *type* argument, written as an expression because
  /// that is all the parser could know it might be. Routed back through
  /// `resolve_type` on a synthesized `named_type` rather than resolved by
  /// hand, so an explicit argument reaches exactly the same builtins, user
  /// types, imports, and in-scope parameters an annotation would.
  auto explicit_type_argument(const explicit_generic_arg &arg)
      -> std::optional<type_id> {
    // Already a type on the way in (`b.take[int64](5)`): nothing to
    // reinterpret, just resolve it.
    if (arg.type != nullptr) {
      const auto resolved = resolve_type(*arg.type, current_resolve_ctx());
      return types_.is_unknown(resolved) || resolved == k_error_type
                 ? std::nullopt
                 : std::optional{resolved};
    }
    if (arg.value == nullptr) {
      return std::nullopt;
    }
    auto named = expr_as_named_type(*arg.value);
    if (named == nullptr) {
      return std::nullopt;
    }
    const auto &type = *named;
    synthesized_types_.push_back(std::move(named));
    const auto resolved = resolve_type(type, current_resolve_ctx());
    return types_.is_unknown(resolved) || resolved == k_error_type
               ? std::nullopt
               : std::optional{resolved};
  }

  /// Rewrites an expression that was *meant* as a type into the `named_type`
  /// the parser would have built had it been in type position — `int32` from
  /// an `ident_expr`, `std.list` from a `module_path_expr`, and
  /// `list[int32]` / `map[str, int32]` from the index and application shapes
  /// `parse_postfix` gives one and several bracket arguments.
  ///
  /// The grammar already sanctions all of these: `type_arg` admits any
  /// `type_expr` and `named_type` admits a nested `"[" type_arg_list "]"`, so
  /// `list[int32]` is one well-formed type argument by the grammar's own
  /// rules. Only the checker's expression-to-type mapping was narrower, which
  /// is what made nested generic type arguments a defect rather than a
  /// missing feature (§3.2).
  ///
  /// Hands back ownership rather than a borrowed pointer, so a nested result
  /// can be parked straight into its parent's `type_args` and the caller
  /// decides what keeps the outermost node alive.
  auto expr_as_named_type(const ast::expr &arg) -> ast::ptr<ast::named_type> {
    auto named = ast::make<ast::named_type>();
    named->span = arg.span;

    // Reads one bracket argument into `named`'s own type arguments, failing
    // the whole rewrite if it isn't itself expressible as a type.
    const auto take_arg = [&](const ast::expr &inner) -> bool {
      auto nested = expr_as_named_type(inner);
      if (nested == nullptr) {
        return false;
      }
      auto slot = ast::type_arg{};
      slot.span = inner.span;
      slot.value = std::move(nested);
      named->type_args.push_back(std::move(slot));
      return true;
    };

    switch (arg.kind) {
    case ast::node_kind::ident_expr:
      named->path = {dynamic_cast<const ast::ident_expr &>(arg).name};
      break;
    case ast::node_kind::module_path_expr:
      named->path = dynamic_cast<const ast::module_path_expr &>(arg).segments;
      break;
    case ast::node_kind::index_expr: {
      // `list[int32]` — one bracket argument, which the parser spells as an
      // index because in expression position that is all it could be.
      const auto &index = dynamic_cast<const ast::index_expr &>(arg);
      if (index.object == nullptr || index.index == nullptr) {
        return nullptr;
      }
      auto base = expr_as_named_type(*index.object);
      if (base == nullptr || !base->type_args.empty()) {
        return nullptr;
      }
      named->path = base->path;
      if (!take_arg(*index.index)) {
        return nullptr;
      }
      break;
    }
    case ast::node_kind::call_expr: {
      // `map[str, int32]` — several bracket arguments, which the parser
      // spells as an application instead.
      const auto &applied = dynamic_cast<const ast::call_expr &>(arg);
      if (applied.callee == nullptr) {
        return nullptr;
      }
      auto base = expr_as_named_type(*applied.callee);
      if (base == nullptr || !base->type_args.empty()) {
        return nullptr;
      }
      named->path = base->path;
      for (const auto &inner : applied.args) {
        if (inner.value == nullptr || inner.name.has_value() ||
            !take_arg(*inner.value)) {
          return nullptr;
        }
      }
      break;
    }
    default:
      return nullptr;
    }
    return named->path.empty() ? nullptr : std::move(named);
  }

  /// Folds one explicit *value* argument down to the constant it denotes.
  /// `solver_poly` does the folding, under the constants the *enclosing*
  /// instance was compiled with — which is what lets a template forward its
  /// own value parameter (`f[n + 1]()` inside an instance where `n` is 3
  /// instantiates `f[4]`).
  auto explicit_value_argument(const ast::expr &arg) -> std::optional<int64_t> {
    auto subst = predicate_subst{};
    for (const auto &[name, value] : const_param_values_) {
      subst.values.emplace(name, poly_constant(value));
    }
    const auto poly = solver_poly(arg, subst);
    return poly.has_value() && poly->is_constant()
               ? std::optional{poly->constant}
               : std::nullopt;
  }

  /// The type a value parameter's constants live in — `[n: usize]`'s `usize`.
  /// `nullopt` when it isn't a number, which is diagnosed by the caller: a
  /// value parameter of a sum type (`[S: conn_state]`, the state machines of
  /// `spec/dependent-types-design.md` §State Machines in Types) is a
  /// compile-time value too, but not one there is a constant to name an
  /// instance after or to compile a body against.
  auto value_param_underlying(const ast::type_param &param,
                              const module_members *owner)
      -> std::optional<type_id> {
    const auto underlying =
        param.bound_or_type != nullptr
            ? resolve_type(*param.bound_or_type,
                           resolve_ctx{.module = owner,
                                       .param_bindings = nullptr,
                                       .use_type_param_stack = false,
                                       .quiet = true})
            : types_.usize_type();
    return types_.is_integer(underlying) ? std::optional{underlying}
                                         : std::nullopt;
  }

  /// Solves every one of `decl`'s compile-time parameters for one call.
  ///
  /// There are three places an answer can come from, tried in this order
  /// because that is the order of decreasing explicitness:
  ///
  ///   1. an explicit argument (`zeros[8]()`), which says so outright;
  ///   2. `unify_rigid`'s solution against the argument types, which pins a
  ///      type parameter (`x: T` given an `int32`) and also a value parameter
  ///      mentioned in a type (`v: array[int32, n]` given an `array[int32,
  ///      3]`, whose `n` solves to a `const_value`);
  ///   3. `solve_value_params`' polynomial solution, already computed while
  ///      checking the arguments, for a value parameter a refinement or
  ///      length constrains.
  ///
  /// `nullopt` when some parameter is left unanswered by all three — the one
  /// way monomorphization fails that is the *caller's* to fix, so it is
  /// diagnosed here, naming the parameter and how to pin it down.
  auto
  solve_generic_params(const ast::call_expr &call, const ast::func_decl &decl,
                       const module_members *owner,
                       const value_bindings &solved,
                       std::unordered_map<std::string, type_id> &type_bindings,
                       const explicit_generic_args &explicit_args)
      -> std::optional<generic_solution> {
    if (explicit_args.size() > decl.type_params.size()) {
      error_with_help(
          call.span,
          std::format("`{}` takes {} compile-time argument(s), and this call "
                      "gives {}",
                      decl.name, decl.type_params.size(), explicit_args.size()),
          "too many arguments in brackets",
          std::format("The brackets after `{}` name its compile-time "
                      "parameters, in declaration order. Drop the extra "
                      "argument(s), or leave the brackets off entirely and "
                      "let the call's arguments determine them.",
                      decl.name));
      return std::nullopt;
    }

    auto solution = generic_solution{};
    for (size_t i = 0; i < decl.type_params.size(); ++i) {
      const auto &param = decl.type_params[i];
      const auto *explicit_arg =
          i < explicit_args.size() ? &explicit_args[i] : nullptr;

      if (param.is_value_param) {
        const auto underlying = value_param_underlying(param, owner);
        if (!underlying.has_value()) {
          report_non_numeric_value_param(call, decl, param);
          return std::nullopt;
        }
        if (explicit_arg != nullptr) {
          // Written in type position (`.m[...]`), where the grammar parses a
          // `type_arg_list` — a type never folds to a constant, so this
          // parameter simply cannot be answered in that spelling.
          const auto constant =
              explicit_arg->value != nullptr
                  ? explicit_value_argument(*explicit_arg->value)
                  : std::optional<int64_t>{};
          if (!constant.has_value()) {
            error_with_help(
                explicit_arg->span,
                std::format("`{}`'s compile-time argument `{}` is not a "
                            "constant",
                            decl.name, param.name),
                "this has to be a definite number here",
                std::format("`{}` is compiled once per constant `{}`, so the "
                            "argument in brackets has to fold to a number at "
                            "compile time — a literal, or arithmetic over "
                            "literals and value parameters already fixed.",
                            decl.name, param.name));
            return std::nullopt;
          }
          bind_generic_constant(solution, param, *underlying, *constant);
          continue;
        }
        // A value parameter mentioned in a parameter's *type* is solved by
        // unification like any other, and arrives as an interned constant.
        if (const auto found = type_bindings.find(param.name);
            found != type_bindings.end()) {
          const auto &entry = types_.entry(found->second);
          if (entry.kind == type_kind::const_value_kind) {
            bind_generic_constant(solution, param, *underlying,
                                  entry.value.constant);
            continue;
          }
        }
        if (const auto found = solved.find(param.name);
            found != solved.end() && found->second.is_constant()) {
          bind_generic_constant(solution, param, *underlying,
                                found->second.constant);
          continue;
        }
        report_unsolved_value_param(call, decl, param);
        return std::nullopt;
      }

      if (explicit_arg != nullptr) {
        const auto resolved = explicit_type_argument(*explicit_arg);
        if (!resolved.has_value()) {
          error_with_help(
              explicit_arg->span,
              std::format("`{}`'s compile-time argument `{}` is not a type "
                          "this call can name",
                          decl.name, param.name),
              "expected a type here",
              std::format("`{}`'s `{}` is a type parameter, so the argument "
                          "in brackets has to name a type that is in scope.",
                          decl.name, param.name));
          return std::nullopt;
        }
        // The brackets win over the arguments — they are the more explicit
        // of the two — but only after saying so when the two disagree. The
        // binding is written back so the caller's own substitution into the
        // declared return type sees the type the call was *actually*
        // instantiated at, rather than the one unification happened to find.
        if (const auto found = type_bindings.find(param.name);
            found != type_bindings.end() && found->second != *resolved &&
            !types_.is_unknown(found->second) &&
            !mentions_abstract_type(found->second)) {
          error_with_help(
              explicit_arg->span,
              std::format("`{}`'s `{}` is given as `{}` in brackets, but the "
                          "arguments make it `{}`",
                          decl.name, param.name, types_.display(*resolved),
                          types_.display(found->second)),
              std::format("this says `{}` is `{}`", param.name,
                          types_.display(*resolved)),
              std::format("A compile-time argument in brackets pins `{}` "
                          "down, so the arguments have to agree with it. "
                          "Either pass arguments that are `{}`, or drop the "
                          "brackets and let the arguments decide.",
                          param.name, types_.display(*resolved)));
          return std::nullopt;
        }
        type_bindings[param.name] = *resolved;
        bind_generic_type(solution, param, *resolved);
        continue;
      }
      const auto found = type_bindings.find(param.name);
      if (found == type_bindings.end() || types_.is_unknown(found->second) ||
          mentions_abstract_type(found->second)) {
        report_unsolved_type_param(call, decl, param);
        return std::nullopt;
      }
      bind_generic_type(solution, param, found->second);
    }
    return solution;
  }

  /// Solves the type parameters `decl`'s *arguments* determine, by unifying
  /// each declared parameter type against the type the argument was inferred
  /// to have — an `x: T` parameter given an `int32` argument solves `T :=
  /// int32`, and a `v: array[int32, n]` parameter given an `array[int32, 3]`
  /// solves `n := 3`. Shared by the free-function and receiver-call paths,
  /// which differ only in which parameters they hand it.
  auto
  solve_from_argument_types(const ast::call_expr &call,
                            const std::vector<fn_param_info> &params,
                            std::unordered_map<std::string, type_id> &bindings,
                            const ast::expr *ufcs_receiver = nullptr) -> void {
    const auto mapping = call_argument_mappings_.find(&call);
    if (mapping == call_argument_mappings_.end()) {
      return;
    }
    const auto &args_by_param = mapping->second.args_by_param;
    for (size_t i = 0; i < params.size(); ++i) {
      const auto *argument = ufcs_argument_for(i, args_by_param, ufcs_receiver);
      if (argument == nullptr) {
        continue;
      }
      if (const auto found = node_types_.find(argument);
          found != node_types_.end()) {
        unify_rigid(params[i].type, found->second, bindings);
      }
    }
  }

  /// The bindings available *partway* through checking a call's arguments —
  /// everything the brackets, the already-checked arguments, and the declared
  /// bounds can answer between them. Used only to substitute into a deferred
  /// lambda's expected type; the authoritative solution is still
  /// `solve_generic_params`, which runs once every argument has been seen and
  /// is the only one that reports an unsolved parameter.
  ///
  /// Quiet by construction: nothing here diagnoses. A parameter this cannot
  /// answer simply stays abstract, and the lambda is checked against what is
  /// known — which is exactly what happened for every call before deferral
  /// existed.
  auto preliminary_type_bindings(const ast::call_expr &call,
                                 const std::vector<fn_param_info> &params,
                                 const generic_call_context &generic)
      -> std::unordered_map<std::string, type_id> {
    auto bindings = generic.seed != nullptr
                        ? *generic.seed
                        : std::unordered_map<std::string, type_id>{};
    const auto &decl = *generic.decl;
    if (generic.explicit_args != nullptr) {
      for (size_t i = 0;
           i < generic.explicit_args->size() && i < decl.type_params.size();
           ++i) {
        const auto &param = decl.type_params[i];
        if (param.is_value_param || param.name.empty()) {
          continue;
        }
        if (const auto resolved =
                explicit_type_argument((*generic.explicit_args)[i])) {
          bindings.emplace(param.name, *resolved);
        }
      }
    }
    solve_from_argument_types(call, params, bindings, generic.ufcs_receiver);
    solve_from_bounds(decl, generic.owner, bindings);
    return bindings;
  }

  /// The argument expression supplied for declared parameter `i`.
  ///
  /// A UFCS call (`recv.f(a)` reaching `f(recv, a)`) writes one fewer
  /// argument than `f` declares parameters: the receiver fills parameter 0
  /// and lives outside `args_by_param`, which is built against the *written*
  /// arguments only. Everything that walks parameters against arguments —
  /// generic solving, precondition substitution — has to shift by one here,
  /// or it reads each argument against the wrong parameter. `nullptr` when
  /// no argument reaches this parameter (a default, or past the end).
  [[nodiscard]] static auto
  ufcs_argument_for(size_t index,
                    const std::vector<const ast::expr *> &args_by_param,
                    const ast::expr *ufcs_receiver) -> const ast::expr * {
    if (ufcs_receiver == nullptr) {
      return index < args_by_param.size() ? args_by_param[index] : nullptr;
    }
    if (index == 0) {
      return ufcs_receiver;
    }
    return index - 1 < args_by_param.size() ? args_by_param[index - 1]
                                            : nullptr;
  }

  /// Solves whatever the arguments left open from the type the call site
  /// *expects* — the last of the four sources, and the only one that can
  /// reach a parameter mentioned nowhere but the return type (`def
  /// collect[I, C](self: I) -> C`).
  ///
  /// Deliberately last, and deliberately non-overriding: `unify_rigid`
  /// declines to rebind a name, so "arguments win over context" falls out of
  /// call order rather than needing a rule of its own. That is what makes
  /// this safe to add without auditing existing call sites — it can turn an
  /// unsolved call into a solved one, never a solved call into a differently
  /// solved one, so `let x: int64 = take(5i32)` still reports the mismatch it
  /// reports today instead of silently re-solving `T := int64`.
  ///
  /// See `spec/generic-inference-design.md` §2.
  auto
  solve_from_expected_type(const ast::call_expr &call,
                           const ast::func_decl &decl,
                           const module_members *owner,
                           std::unordered_map<std::string, type_id> &bindings,
                           const explicit_generic_args &explicit_args) -> void {
    if (decl.return_type == nullptr) {
      return;
    }
    const auto found = call_expected_types_.find(&call);
    if (found == call_expected_types_.end()) {
      return;
    }
    const auto expected = types_.strip_refinement(found->second);
    if (types_.is_unknown(expected) || expected == k_error_type) {
      return;
    }
    // Resolved in the *template's* own terms — `signature_return_type` binds
    // each type parameter to its abstract `type_param`, which is exactly the
    // form `unify_rigid` knows how to solve, so `-> C` comes back as a
    // bindable `C` rather than as `unknown`.
    //
    // Solved aside and merged in, rather than unified straight into
    // `bindings`, so a parameter the brackets already answer never picks up
    // a competing answer from context. Without that, `solve_generic_params`
    // would see a hint-derived binding where it expects an argument-derived
    // one and report a conflict the user's arguments never had — the hint is
    // a fallback, and a fallback that can raise an error is not one.
    auto from_expected = std::unordered_map<std::string, type_id>{};
    unify_rigid(signature_return_type(decl, owner), expected, from_expected);
    for (size_t i = 0; i < decl.type_params.size(); ++i) {
      const auto &name = decl.type_params[i].name;
      if (name.empty() || i < explicit_args.size() || bindings.contains(name)) {
        continue;
      }
      if (const auto solved = from_expected.find(name);
          solved != from_expected.end()) {
        bindings.emplace(name, solved->second);
        expected_solved_params_[&call].push_back(
            std::format("`{}` was solved to `{}` from the type expected here",
                        name, types_.display(solved->second)));
      }
    }
  }

  /// Monomorphizes `decl` for this call, returning the call's result type
  /// under the instantiation (`-> array[int32, n]` becomes `array[int32, 3]`,
  /// `-> T` becomes `int32`) — or `nullopt` when the call cannot be
  /// monomorphized, having said why.
  ///
  /// The instance is registered as this call's `resolved_callee`, so lowering
  /// emits a call to `get$3` with no idea that a template was ever involved.
  auto instantiate_generic_function(
      const ast::call_expr &call, const ast::func_decl &decl,
      const module_members *owner, file_id_type decl_file,
      const value_bindings &solved, const std::vector<fn_param_info> &params,
      const explicit_generic_args &explicit_args,
      const ast::expr *ufcs_receiver = nullptr) -> std::optional<type_id> {
    auto type_bindings = std::unordered_map<std::string, type_id>{};
    solve_from_argument_types(call, params, type_bindings, ufcs_receiver);
    // Between the arguments and the expected type, because a bound is solved
    // *from* an argument-derived binding (`I` answers `T` via
    // `where I: iterator[T]`) and should still lose to an explicit annotation
    // the way every argument-derived binding does. `def sum[I, T](it: I)`
    // has no argument mentioning `T` at all, so without this the terminal of
    // every adapter chain is unsolvable.
    solve_from_bounds(decl, owner, type_bindings);
    solve_from_expected_type(call, decl, owner, type_bindings, explicit_args);

    const auto solution = solve_generic_params(call, decl, owner, solved,
                                               type_bindings, explicit_args);
    if (!solution.has_value()) {
      return std::nullopt;
    }

    const auto *instance = find_or_check_generic_instance(
        call, decl, owner, decl_file, *solution, decl.name + solution->suffix,
        /*fixed_type_params=*/nullptr);
    if (instance == nullptr) {
      return std::nullopt;
    }

    // The instance's signature, resolved under the same substitution its body
    // was checked with — this is what makes the *caller* see concrete types
    // (`array[int32, 3]`, not `array[int32, n]`) for the callee it names and
    // the value the call produces.
    auto bindings = solution->const_slots;
    bindings.insert(solution->type_slots.begin(), solution->type_slots.end());
    const auto ctx = resolve_ctx{.module = owner,
                                 .param_bindings = &bindings,
                                 .use_type_param_stack = false,
                                 .quiet = true,
                                 .existential_allowed = true};
    auto param_types = std::vector<type_id>{};
    for (const auto &param : decl.params) {
      param_types.push_back(param.type_annotation != nullptr
                                ? resolve_type(*param.type_annotation, ctx)
                                : k_unknown_type);
    }
    const auto result = decl.return_type != nullptr
                            ? resolve_type(*decl.return_type, ctx)
                            : types_.builtin("unit");
    if (call.callee != nullptr) {
      record_expr_type(*call.callee,
                       types_.fn_of(std::move(param_types), result));
    }
    // Deliberately last: a module-qualified call (`app.lib.at(v, i)`) already
    // recorded the *template* as its callee on the way in
    // (`infer_qualified_call`), and lowering must emit a call to the instance,
    // which is the only one of the two that exists as a function.
    resolved_callees_[&call] =
        resolved_callee{.decl = instance,
                        .owner_module = owner->module_name,
                        .impl_target_type = "",
                        .receiver = ufcs_receiver};
    return record_expr_type(call, result);
  }

  /// Explains a call whose value parameter no argument determines — the one
  /// way monomorphization fails that is the *caller's* to fix.
  auto report_unsolved_value_param(const ast::call_expr &call,
                                   const ast::func_decl &decl,
                                   const ast::type_param &param) -> void {
    error_with_help(
        call.span,
        std::format("cannot tell what `{}` is in this call to `{}`", param.name,
                    decl.name),
        std::format("`{}` is a compile-time value, and this call doesn't fix "
                    "it",
                    param.name),
        std::format(
            "`{}` is compiled once per constant `{}`, so `{}` has to be a "
            "definite number here. Either give it outright in brackets "
            "(`{}[8](...)`), or pass an argument whose type mentions it with "
            "a known length (an `array[T, {}]`, for instance).",
            decl.name, param.name, param.name, decl.name, param.name));
  }

  /// The same explanation for a type parameter the arguments leave open.
  auto report_unsolved_type_param(const ast::call_expr &call,
                                  const ast::func_decl &decl,
                                  const ast::type_param &param) -> void {
    error_with_help(
        call.span,
        std::format("cannot tell what `{}` is in this call to `{}`", param.name,
                    decl.name),
        std::format("`{}` is a type parameter this call leaves unsolved",
                    param.name),
        std::format("`{}` is compiled once per concrete type, so every type "
                    "parameter has to be pinned down. Either give it outright "
                    "in brackets (`{}[int32](...)`), or annotate the value "
                    "that should determine `{}` — an annotated `let`, a "
                    "declared return type, or any other context with a known "
                    "type is used to solve a parameter the arguments leave "
                    "open.",
                    decl.name, decl.name, param.name));
  }

  /// A value parameter that isn't a number has no constant to name an
  /// instance after and none to compile the body against.
  auto report_non_numeric_value_param(const ast::call_expr &call,
                                      const ast::func_decl &decl,
                                      const ast::type_param &param) -> void {
    const auto underlying =
        param.bound_or_type != nullptr
            ? resolve_type(*param.bound_or_type,
                           resolve_ctx{.module = module_,
                                       .param_bindings = nullptr,
                                       .use_type_param_stack = false,
                                       .quiet = true})
            : types_.usize_type();
    error_with_help(
        call.span,
        std::format("`{}` cannot be compiled: it is generic over the "
                    "compile-time value `{}`, which is a `{}` rather than "
                    "a number",
                    decl.name, param.name, types_.display(underlying)),
        "no compiled copy can be made for this call",
        std::format("A function generic over a value is compiled once per "
                    "*constant* it is called with, and only integer values "
                    "have constants to compile for so far. Write the "
                    "state out (`{}[<state>]`) in the signature instead of "
                    "abstracting over `{}`.",
                    types_.display(underlying), param.name));
  }

  // ==========================================================================
  //  Higher-kinded-trait method monomorphization
  //  (`spec/higher-kinded-traits-implementation-plan.md` phase 5)
  //
  //  A method an `impl monad for option` provides is generic over its own
  //  type parameters (`def pure[A](a: A) -> option[A]`), and — like every
  //  function generic over *types* — has no compiled form of its own
  //  (`hir::lower_function` refuses type-generic declarations). So each call
  //  whose receiver/arguments pin every parameter down to a concrete type
  //  gets an instance: the declaration cloned, re-checked under that
  //  substitution (`type_param_slots_`), named `option::pure$int32`, and
  //  registered exactly like a const-generic instance so lowering and both
  //  backends see only fully-applied types (Strategy #4: HKTs erase at
  //  monomorphization).
  // ==========================================================================

  /// Renders `id` in symbol-safe characters for an instance name —
  /// `option[int32]` becomes `option_int32_`. Purely a naming aid; identity
  /// is the cache key built from the solved `type_id`s themselves.
  auto mangle_type_for_instance(type_id id) -> std::string {
    auto out = std::string{};
    for (const auto c : types_.display(id)) {
      out += std::isalnum(static_cast<unsigned char>(c)) != 0 ? c : '_';
    }
    return out;
  }

  /// Monomorphizes an impl/extend-provided generic method for one call's
  /// solved bindings, returning the checked instance — reused per (method,
  /// solution) — or `nullptr` when a parameter is unsolved or the body can't
  /// be cloned (both diagnosed here).
  ///
  /// This is `instantiate_generic_function` with a receiver: the same
  /// solution, the same instance builder, the same registry. It differs only
  /// in the instance's name, which carries the impl target
  /// (`option::pure$int32`, `vec::at$3`) because that is how `hir::lower_impl_
  /// associated_functions` names an impl member, and in taking a
  /// `method_entry` — which records no declaring file, and may carry a trait
  /// default's own fixed bindings.
  ///
  /// A value parameter is solved here exactly as a type parameter is: `n` in
  /// a `def at[n: usize](self, i: index[n])` is unified out of the receiver's
  /// own type and arrives as an interned constant, which
  /// `solve_generic_params` reads back.
  ///
  /// `bindings` is taken by mutable reference because the expected-type
  /// solution (§2) is folded into it here: the callers substitute the same
  /// map into the declared return type afterward, and a `C` solved only from
  /// context has to reach that substitution too or the call would come back
  /// typed as the abstract parameter.
  /// `extra_fixed` carries bindings the *impl block* supplies, for a method
  /// that is generic on both counts: `static def from_iter[I]` inside
  /// `impl[T] from_iter[T] for list[T]` declares `I` and inherits `T`.
  /// `solve_generic_params` only ever walks the declaration's own
  /// `type_params`, so it solves `I` from the arguments and never learns `T`
  /// exists — the instance then fails to check with ``undefined type `T` ``.
  /// The impl's parameters have to arrive the way
  /// `check_impl_generic_method_call` delivers them: as scoped fixed
  /// bindings, not as solution slots.
  auto instantiate_hk_method(
      const ast::call_expr &call, const method_entry &method,
      std::string_view target_type_name,
      std::unordered_map<std::string, type_id> &bindings,
      const explicit_generic_args &explicit_args = {},
      const value_bindings &solved = {}, type_id self_type = k_unknown_type,
      const std::unordered_map<std::string, type_id> *extra_fixed = nullptr)
      -> const ast::func_decl * {
    const auto &decl = *method.decl;
    solve_from_expected_type(call, decl, method.owner, bindings, explicit_args);
    const auto solution = solve_generic_params(call, decl, method.owner, solved,
                                               bindings, explicit_args);
    if (!solution.has_value()) {
      return nullptr;
    }
    const auto name =
        std::format("{}::{}{}", target_type_name, decl.name, solution->suffix);
    auto scoped_params = method.fixed_type_params;
    if (extra_fixed != nullptr) {
      scoped_params.insert(extra_fixed->begin(), extra_fixed->end());
    }
    return find_or_check_generic_instance(call, decl, method.owner,
                                          /*decl_file=*/std::nullopt, *solution,
                                          name, &scoped_params, self_type);
  }

  /// Holds a callee's `pre` conditions to account at the call site.
  ///
  /// A precondition has two faces, and they are the whole of contract
  /// reasoning: inside the callee it is a *fact* it may assume
  /// (`collect_function_facts`), and at every call site it is an *obligation*
  /// the caller must meet. This is the obligation half.
  ///
  /// Refuted: a compile error, because the call is wrong on every execution
  /// that reaches it, and the spec says so outright ("When a condition is
  /// statically knowable, violation is a compile error"). Proved or unproven:
  /// silence. Neither one *elides* anything, and that is deliberate — the
  /// runtime check lives at the callee's entry, where it stands in for every
  /// call site at once, so a proof about this one call says nothing about the
  /// next one. The elision a callee-entry check can honour is the one
  /// `collect_function_facts` computes instead: a `pre` that follows from the
  /// parameters' own types, and so cannot fail on any call at all.
  auto check_call_preconditions(const ast::call_expr &call,
                                const ast::func_decl &decl,
                                const std::vector<fn_param_info> &params,
                                const ast::expr *ufcs_receiver = nullptr)
      -> void {
    if (decl.contracts.empty()) {
      return;
    }
    const auto mapping = call_argument_mappings_.find(&call);
    if (mapping == call_argument_mappings_.end()) {
      return;
    }

    // Restate the precondition in the caller's terms: each parameter becomes
    // the argument actually supplied for it. A parameter left to its default,
    // or one whose argument falls outside the fragment, simply doesn't enter
    // the substitution — the condition then mentions a name nothing is known
    // about, and comes back `unknown`, which is exactly right.
    auto subst = predicate_subst{};
    for (size_t i = 0; i < params.size(); ++i) {
      const auto *argument =
          ufcs_argument_for(i, mapping->second.args_by_param, ufcs_receiver);
      if (argument == nullptr || params[i].name.empty()) {
        continue;
      }
      if (const auto poly = solver_poly(*argument, predicate_subst{})) {
        subst.values.emplace(params[i].name, *poly);
        subst.spellings.emplace(params[i].name,
                                describe(*argument, predicate_subst{}));
      }
    }

    for (const auto &contract : decl.contracts) {
      if (!contract.is_pre || contract.condition == nullptr ||
          contract.condition->has_error) {
        continue;
      }
      const auto goal = goal_from(*contract.condition, subst, false);
      if (goal.empty()) {
        continue;
      }
      switch (solve(facts_, goal)) {
      case proof_result::proved:
        // Nothing to do: this call is safe, and the callee's own entry check
        // (which every *other* call site also relies on) stays put.
        break;
      case proof_result::refuted: {
        auto diag = diagnostic(
            diagnostic_level::error,
            std::format("precondition `{}` is never true at this call",
                        goal_label(goal, "the precondition")),
            file_id_);
        diag.with_label(call.span,
                        std::format("calling `{}` here always violates its "
                                    "contract",
                                    decl.name));
        if (const auto known = known_facts_note(goal)) {
          diag.children.emplace_back(diagnostic_level::note, *known, file_id_);
        }
        diag.with_help(
            contract.message.has_value()
                ? std::format("`{}` documents this as: {}", decl.name,
                              *contract.message)
                : std::format("`{}` requires this to hold on entry. The "
                              "compiler can see that it doesn't — this is not "
                              "a check it failed to prove, but one it "
                              "disproved.",
                              decl.name));
        emit_diag(diag);
        mark_error();
        break;
      }
      case proof_result::unknown:
        // The check stays, and runs. This is what a contract is for.
        break;
      }
    }
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
                          "for this constructor value", arg.value.get());
          }
        }
      }
    }

    // When the expected slot is itself still abstract (a callee's unsolved
    // type parameter — `option[B]` from a generic signature) but the payload
    // is concrete, answer with the concrete instantiation: echoing the
    // abstract expectation back would tell rigid inference (`unify_rigid`)
    // nothing, leaving `B` unsolvable at a call that plainly determines it.
    if (expected_is("option") && (name == "some" || name == "none")) {
      if (name == "some" && !expected_entry.args.empty() &&
          types_.is_unknown(expected_entry.args[0]) &&
          !types_.is_unknown(payload_found)) {
        return types_.builtin_generic("option", {payload_found});
      }
      return expected;
    }
    if (expected_is("result") && (name == "ok" || name == "err")) {
      if (!types_.is_unknown(payload_found) &&
          expected_entry.args.size() == 2) {
        const auto slot = name == "ok" ? size_t{0} : size_t{1};
        if (types_.is_unknown(expected_entry.args[slot])) {
          auto args = std::vector<type_id>{expected_entry.args[0],
                                           expected_entry.args[1]};
          args[slot] = payload_found;
          return types_.builtin_generic("result", std::move(args));
        }
      }
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
    // Variants of sum types re-exported by a session-owned wildcard import
    // (`use a.b.*` brings every type along, so every variant comes too).
    for (const auto *source : wildcard_import_sources()) {
      if (const auto it = source->variants.find(std::string(name));
          it != source->variants.end()) {
        return std::pair{it->second.sum_decl, it->second.variant};
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
    emit_diag(diag);
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

    // `return` inside a contract condition (the parser only ever produces an
    // identifier spelled `return` there) names the value the function
    // returns. It exists in a `post`, where the function has produced one,
    // and nowhere else.
    if (name == "return" && in_contract_) {
      if (!in_postcondition_) {
        error_with_help(
            ident.span,
            "`return` names the value the function returns, so it may only "
            "appear in a `post` condition",
            "there is no returned value here",
            "A `pre` condition runs on entry, before the function has produced "
            "anything, and a type `invariant` describes a value that has no "
            "call in progress at all. Move this to a `post` condition if it "
            "describes the result.");
        return k_error_type;
      }
      if (types_.is_unit(return_type_)) {
        error_with_help(
            ident.span,
            "`return` names the value the function returns, and this function "
            "returns `unit`",
            "no meaningful value to constrain",
            "A `unit`-returning function has only one possible result, so a "
            "`post` condition about it can never say anything. Constrain a "
            "parameter instead, or give the function a return type.");
        return k_error_type;
      }
      return return_type_;
    }

    if (const auto *binding = lookup_value(name)) {
      if (binding->origin == binding_origin::parameter) {
        record_const_param_reference(ident, binding->type);
      }
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
        const auto type = static_binding_type(*it->second.decl, module_);
        record_static_const_reference(ident, *it->second.decl, type);
        return type;
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
          const auto type = static_binding_type(*it->second.decl, source);
          record_static_const_reference(ident, *it->second.decl, type);
          return type;
        }
      }
      return k_unknown_type;
    }

    // Values re-exported by a session-owned wildcard import (`use a.b.*`).
    for (const auto *source : wildcard_import_sources()) {
      if (const auto it = source->functions.find(std::string(name));
          it != source->functions.end()) {
        return fn_type_of(*it->second.decl, source);
      }
      if (const auto it = source->statics.find(std::string(name));
          it != source->statics.end()) {
        const auto type = static_binding_type(*it->second.decl, source);
        record_static_const_reference(ident, *it->second.decl, type);
        return type;
      }
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
    emit_diag(diag);
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

  /// Ensures `decl`'s compile-time value is evaluated and bound in
  /// `comptime_eval_`, then returns it — or `nullptr` if there's no
  /// initializer, it already failed to check, or evaluation itself
  /// failed. Skips re-evaluating (and potentially re-diagnosing) an
  /// initializer a forward reference already resolved, exactly like
  /// `check_static_decl`'s own `binding` case (which is in fact one of
  /// this helper's two callers, refactored to share it) — `resolve_ident`
  /// is the other, since a name's *first* reference in file-checking
  /// order might be an ordinary expression far from the `static let`'s
  /// own declaration, and every reference needs the same confluent,
  /// order-independent evaluation the rest of this compile-time subsystem
  /// already guarantees.
  auto ensure_static_binding_evaluated(const ast::static_decl &decl)
      -> const comptime::value * {
    if (decl.initializer == nullptr || decl.initializer->has_error ||
        decl.name.empty()) {
      return nullptr;
    }
    if (!comptime_eval_.has_global(decl.name)) {
      const auto evaluated = comptime_eval_.evaluate(*decl.initializer);
      if (evaluated.is_error()) {
        return nullptr;
      }
      comptime_eval_.bind_global(decl.name, evaluated);
    }
    return comptime_eval_.global_value(decl.name);
  }

  /// If `decl` (the `static let` a name just resolved to) evaluates to a
  /// materializable scalar, records `ident -> literal` in
  /// `static_const_values_` so `hir::lower_ident` can embed the value
  /// directly instead of emitting an unresolvable `hir_local_ref` — see
  /// `ensure_static_binding_evaluated`/`materialize_const_literal`'s doc
  /// comments for why evaluation must happen here (not just at `decl`'s
  /// own declaration site) and what "materializable" means. `type` is
  /// `ident`'s already-resolved checked type (`static_binding_type`),
  /// passed in rather than recomputed so the synthesized literal can be
  /// given the exact same type in `node_types_` — lowering requires every
  /// node it visits, including one this pass synthesizes, to already have
  /// a recorded checked type.
  auto record_static_const_reference(const ast::ident_expr &ident,
                                     const ast::static_decl &decl, type_id type)
      -> void {
    const auto *value = ensure_static_binding_evaluated(decl);
    if (value == nullptr) {
      return;
    }
    if (const auto *lit = materialize_const_literal(*value, ident.span, type)) {
      static_const_values_[&ident] = lit;
    }
  }

  /// Records a *value* use of a value parameter (`return n`, `for i in 0..n`)
  /// inside a const-generic instance as the constant that instance was made
  /// for, the same way `record_static_const_reference` records a scalar
  /// `static let`: `hir::lower_ident` finds the literal in
  /// `static_const_values_` and emits it instead of loading a local that
  /// doesn't exist. It can't exist — a value parameter is passed at compile
  /// time, so nothing at runtime holds `n`. In the template (where `n` is
  /// still symbolic) `const_param_values_` is empty and this does nothing,
  /// which is correct: the template is never lowered.
  auto record_const_param_reference(const ast::ident_expr &ident, type_id type)
      -> void {
    const auto it = const_param_values_.find(ident.name);
    if (it == const_param_values_.end()) {
      return;
    }
    auto lit = ast::make<ast::literal_expr>();
    lit->span = ident.span;
    lit->lit_kind = token_kind::int_lit;
    lit->value = std::to_string(it->second);
    const auto *raw = lit.get();
    synthesized_const_literals_.push_back(std::move(lit));
    record_expr_type(*raw, type);
    static_const_values_[&ident] = raw;
  }

  /// Synthesizes an `ast::literal_expr` embedding `value`'s exact
  /// compile-time content — with `span` and `type` (recorded into
  /// `node_types_`, exactly like every other checked expression) taken
  /// from the reference site being replaced, since the synthesized node
  /// itself was never independently type-checked. Owned session-wide in
  /// `synthesized_const_literals_` (same stable-address
  /// `vector<unique_ptr<T>>` pattern as `synthesized_decls_`/
  /// `synthesized_types_`) — lets a scalar `static let` reference lower
  /// as a real literal instead of an unresolvable `hir_local_ref` (see
  /// `resolve_ident`). Deliberately narrow, mirroring `comptime::
  /// try_eval_expr_builder_call`'s own `expr.lit` scope exactly: only
  /// integer/floating/boolean values materialize; anything else (a
  /// string, list, struct, ...) returns `nullptr`, leaving that reference
  /// to fall back to the pre-existing (unresolvable-at-runtime) behavior
  /// rather than risk mis-encoding something like a string's escape
  /// sequences into a synthesized literal's raw spelling.
  auto materialize_const_literal(const comptime::value &value, source_span span,
                                 type_id type) -> const ast::literal_expr * {
    auto lit = ast::make<ast::literal_expr>();
    lit->span = span;
    switch (value.kind) {
    case comptime::value_kind::integer:
      lit->lit_kind = token_kind::int_lit;
      lit->value = std::to_string(value.integer);
      break;
    case comptime::value_kind::floating:
      lit->lit_kind = token_kind::float_lit;
      lit->value = std::to_string(value.floating);
      break;
    case comptime::value_kind::boolean:
      lit->lit_kind =
          value.boolean ? token_kind::kw_true : token_kind::kw_false;
      lit->value = value.boolean ? "true" : "false";
      break;
    default:
      return nullptr;
    }
    const auto *raw = lit.get();
    synthesized_const_literals_.push_back(std::move(lit));
    record_expr_type(*raw, type);
    return raw;
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
    emit_diag(diag);
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

  /// Resolves `method`'s return type the way it would have resolved *inside
  /// its own impl* — reinstating that impl's `self.output`-style associated
  /// type bindings (`impl_assoc_types_`, keyed by `(target, trait_name)`)
  /// around an ordinary `signature_return_type` call. Without this, a method
  /// looked up from outside its impl (an operator overload's dispatch) would
  /// see `self_assoc_types_` as whatever it happened to be left at — empty,
  /// most of the time — and `self.output` would resolve to `unknown`.
  auto resolve_operator_return_type(type_id target, std::string_view trait_name,
                                    const method_entry &method) -> type_id {
    const auto target_it = impl_assoc_types_.find(target);
    if (target_it == impl_assoc_types_.end()) {
      return signature_return_type(*method.decl, method.owner);
    }
    const auto trait_it = target_it->second.find(std::string(trait_name));
    if (trait_it == target_it->second.end()) {
      return signature_return_type(*method.decl, method.owner);
    }
    const auto saved_assoc = self_assoc_types_;
    self_assoc_types_ = trait_it->second;
    const auto result = signature_return_type(*method.decl, method.owner);
    self_assoc_types_ = saved_assoc;
    return result;
  }

  /// For a user struct/sum/opaque operand, requires it to implement
  /// `trait_name` (reporting a missing-impl error with an `impl`/`deriving`
  /// hint otherwise); returns `unknown` for anything else. When `binary` is
  /// an arithmetic operator being dispatched to a real overload method (see
  /// `operator_trait_for`), also records an `operator_dispatches_` entry so
  /// `hir::lower_binary` emits a call to that method instead of a raw
  /// numeric opcode, and returns the method's real (impl-specific) return
  /// type rather than `unknown`. `wire_dispatch` is true for `==`/`!=` too
  /// (dispatches to `eq`), but false for ordering comparisons (`<`/`<=`/
  /// `>`/`>=`), which always yield `bool` regardless of this function's
  /// return value and have no overload-method dispatch of their own yet.
  auto require_operand_trait(const ast::binary_expr &binary, type_id operand,
                             std::string_view trait_name,
                             std::string_view op_name, bool wire_dispatch)
      -> type_id {
    const auto target = strip_refs(operand);
    const auto entry = types_.entry(target);
    if (entry.kind == type_kind::struct_kind ||
        entry.kind == type_kind::sum_kind ||
        entry.kind == type_kind::opaque_kind) {
      if (type_has_trait(entry, trait_name)) {
        if (wire_dispatch && binary.lhs != nullptr) {
          if (const auto *method = find_method(entry, trait_name);
              method != nullptr && !method->decl->params.empty() &&
              param_name_of(method->decl->params.front()) == "self") {
            operator_dispatches_[&binary] =
                resolved_callee{.decl = method->decl,
                                .owner_module = method->owner->module_name,
                                .impl_target_type = entry.name,
                                .receiver = binary.lhs.get()};
            return resolve_operator_return_type(target, trait_name, *method);
          }
        }
        return k_unknown_type; // operator impl decides the result type
      }
      error_with_help(
          binary.span,
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
    // Operands participate as their *base* type: a `positive` is an `int32`
    // to `+`, and `p + 1` is an `int32`, not a `positive` — arithmetic does
    // not carry a predicate through it, and pretending otherwise would be
    // unsound (`p + 1` overflows to a negative). Widening is free; this is
    // where it is free (`spec/dependent-types-design.md` §3.1).
    const auto numeric_expected = types_.is_numeric(strip_refs(expected))
                                      ? base_shape(expected)
                                      : k_unknown_type;
    const auto lhs = binary.lhs != nullptr
                         ? base_shape(infer_expr(*binary.lhs, numeric_expected))
                         : k_unknown_type;
    const auto rhs_expected = types_.is_numeric(lhs) ? lhs : numeric_expected;
    const auto rhs = binary.rhs != nullptr
                         ? base_shape(infer_expr(*binary.rhs, rhs_expected))
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
        return require_operand_trait(binary, lhs, trait_name, op_name,
                                     /*wire_dispatch=*/true);
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
      emit_diag(diag);
      mark_error();
      return k_error_type;
    }
    return lhs;
  }

  /// Types `==`/`!=` (`is_equality`) or `<`/`<=`/`>`/`>=`: operands must be
  /// mutually compatible types, and (for a user struct/sum operand) must
  /// implement `eq`/`ord`. Always yields `bool`. For `==`/`!=` against a
  /// struct/sum/opaque operand, also wires a real `eq` method dispatch
  /// (mirrors arithmetic operators); `ord` has no dispatch of its own yet
  /// (`<`/`<=`/`>`/`>=` would need to translate `cmp()`'s `ordering` result,
  /// not just call through).
  auto infer_comparison(const ast::binary_expr &binary, bool is_equality)
      -> type_id {
    // Comparison, like arithmetic, sees a refined value as its base — an
    // `index[n]` compares against a `usize` without ceremony, and the trait
    // lookup below (`ord`/`eq`) is asked of `usize`, which is what actually
    // implements it.
    const auto lhs = binary.lhs != nullptr
                         ? base_shape(infer_expr(*binary.lhs, k_unknown_type))
                         : k_unknown_type;
    const auto rhs = binary.rhs != nullptr
                         ? base_shape(infer_expr(*binary.rhs, lhs))
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
    require_operand_trait(binary, lhs, trait_name,
                          ast::binary_op_name(binary.op),
                          /*wire_dispatch=*/is_equality);
    if (is_equality) {
      wire_str_equality_dispatch(binary, lhs);
    }
    return bool_type;
  }

  /// Wires `==`/`!=` between two `str` operands to a real call of
  /// `std.string`'s `str::eq` extend method (`rt_str_eq`-backed) — `str`
  /// has no scalar bytecode/LLVM representation for `==` to compare
  /// directly (see `std/string.kira`'s own `eq` doc comment: "Use this
  /// instead of `==`, which has no `str` codegen yet"). Recorded through
  /// the same `operator_dispatches_` map arithmetic operators use;
  /// `hir::lower_binary` negates the call's result for `!=` itself (it
  /// already has `bin.op`, so no extra bookkeeping is needed here). A
  /// no-op for anything but two `str` operands — every other builtin
  /// scalar already has real `==`/`!=` codegen.
  auto wire_str_equality_dispatch(const ast::binary_expr &binary, type_id lhs)
      -> void {
    const auto entry = types_.entry(strip_refs(lhs));
    if (entry.kind != type_kind::builtin_kind || entry.name != "str" ||
        binary.lhs == nullptr) {
      return;
    }
    const auto *method = find_extend_method_for_builtin(entry, "eq");
    if (method == nullptr) {
      return;
    }
    operator_dispatches_[&binary] =
        resolved_callee{.decl = method->decl,
                        .owner_module = method->owner->module_name,
                        .impl_target_type = entry.name,
                        .receiver = binary.lhs.get()};
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

  /// Reports an error unless `found` names one of the four quote-value
  /// types (`expr`/`stmt`/`def_expr`/`type_expr`) — used at a splice site,
  /// where the operand must be quoted syntax, not an ordinary value like
  /// `~(42)`. `unknown`/`error` are accepted so one upstream gap doesn't
  /// cascade into a second, redundant diagnostic here.
  /// Human-readable name for a compile-time quote-fragment kind, used in
  /// splice-position mismatch diagnostics (e.g. splicing a quoted statement
  /// where only a quoted expression is usable).
  static auto quote_fragment_kind_name(comptime::value_kind kind)
      -> std::string_view {
    switch (kind) {
    case comptime::value_kind::expr_fragment:
      return "a quoted expression";
    case comptime::value_kind::stmt_fragment:
      return "a quoted statement";
    case comptime::value_kind::def_expr_fragment:
      return "a quoted definition";
    case comptime::value_kind::type_expr_fragment:
      return "a quoted type";
    default:
      return "a non-quote value";
    }
  }

  auto require_quote_value(type_id found, source_span span,
                           std::string_view context) -> void {
    if (types_.is_unknown(found)) {
      return;
    }
    const auto &entry = types_.entry(found);
    if (entry.kind == type_kind::builtin_kind &&
        (entry.name == "expr" || entry.name == "stmt" ||
         entry.name == "def_expr" || entry.name == "type_expr")) {
      return;
    }
    error_with_help(
        span,
        std::format("{} must be a quoted fragment (`expr`, `stmt`, or "
                    "`def_expr`), found `{}`",
                    context, types_.display(found)),
        "expected quoted syntax here",
        "`~` splices previously-captured syntax (from a backtick quote "
        "`` `(...)` ``) back into code; it does not evaluate an arbitrary "
        "compile-time value.");
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
      // Lending a value mutably hands someone else the right to change it, so
      // nothing known about it survives the loan. The design doc is explicit
      // that narrowing does not flow through an `&mut`
      // (`spec/dependent-types-design.md` §5.1); this is where that is
      // enforced, rather than by trying to track what the callee does.
      if (unary.operand != nullptr) {
        if (const auto *root = assignment_root_ident(*unary.operand)) {
          invalidate_facts(root->name);
        }
      }
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
          return types_.ref_to(types_.builtin_generic("slice_mut", {element}),
                               true);
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
  /// One recorded trait impl, keyed by `type_key_of` (declaration-level, so
  /// `type_has_trait` finds an impl through any instantiation). `target` is
  /// the impl's actual target type, which is what conflict detection needs
  /// and the key deliberately drops.
  struct recorded_impl {
    source_location location;
    type_id target = k_unknown_type;
  };
  std::unordered_map<std::string, recorded_impl> impl_trait_index_;

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
  /// otherwise.
  ///
  /// Deliberately *declaration*-level: every instantiation of a generic type
  /// shares one key. That is what `type_has_trait` needs — asking whether
  /// `holder[int32]` implements `show` has to find an `impl[T] show for
  /// holder[T]` — so the key cannot carry type arguments. Whether two impls
  /// sharing a key actually *conflict* is a separate, finer question, and
  /// `impl_targets_overlap` answers it.
  auto type_key_of(const type_entry &entry) -> std::string {
    if (entry.decl != nullptr) {
      return std::format("u:{}", static_cast<const void *>(entry.decl));
    }
    return std::format("n:{}", entry.name);
  }

  /// Whether `id` mentions a type parameter anywhere inside it — i.e. whether
  /// it is a *pattern* standing for many concrete types (`holder[T]`) rather
  /// than one (`holder[int32]`).
  auto type_mentions_param(type_id id) -> bool {
    const auto entry = types_.entry(id);
    if (entry.kind == type_kind::type_param_kind ||
        entry.kind == type_kind::param_app_kind) {
      return true;
    }
    return std::ranges::any_of(entry.args, [&](type_id arg) -> bool {
      return type_mentions_param(arg);
    });
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
  auto impl_targets_overlap(type_id first, type_id second) -> bool {
    if (first == second) {
      return true;
    }
    return type_mentions_param(first) || type_mentions_param(second);
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
  /// `prelude.kira`/`std/traits.kira` into the session (`compile_sources`,
  /// `src/driver/driver.cpp`); a file that writes `no_prelude` opts out.
  /// Absent from a session that never included those files (most unit
  /// tests), this is simply inert.
  auto find_prelude_trait(std::string_view name)
      -> std::optional<trait_decl_ref> {
    if (file_no_prelude_) {
      return std::nullopt;
    }
    static constexpr std::array<std::string_view, 3>
        k_prelude_reexport_modules = {"prelude", "std.traits", "std.iter"};
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
  auto find_prelude_function(std::string_view name)
      -> std::optional<std::pair<func_decl_ref, const module_members *>> {
    if (file_no_prelude_) {
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
    static constexpr std::array<std::string_view, 3>
        k_prelude_function_modules = {"std.console", "std.iter", "std.algo"};
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

  /// The arity of the trait's constructor parameter when `impl`'s trait
  /// abstracts over a type constructor (`trait monad[M[_]]`) — the fact
  /// that makes a *bare* generic name legal as the impl's `for` target
  /// (`impl monad for option` names the constructor, not an instantiation).
  /// `nullopt` for ordinary traits and trait-less inherent impls. `home` is
  /// the impl's own module, searched first; the trait may live anywhere in
  /// the session.
  auto impl_expected_ctor_arity(const ast::impl_decl &impl,
                                const module_members *home)
      -> std::optional<size_t> {
    const auto trait_name = trait_name_of_impl(impl);
    if (trait_name.empty()) {
      return std::nullopt;
    }
    const ast::trait_decl *trait_decl = nullptr;
    if (home != nullptr) {
      if (const auto it = home->traits.find(trait_name);
          it != home->traits.end()) {
        trait_decl = it->second.decl;
      }
    }
    if (trait_decl == nullptr) {
      for (const auto &[module_name, other] : index_.modules) {
        if (const auto it = other.traits.find(trait_name);
            it != other.traits.end()) {
          trait_decl = it->second.decl;
          break;
        }
      }
    }
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
  auto resolve_impl_target(const impl_ref &impl) -> type_id {
    if (impl.decl->for_type == nullptr) {
      return k_unknown_type;
    }
    auto param_bindings = std::unordered_map<std::string, type_id>{};
    for (const auto &param : impl.decl->type_params) {
      if (!param.name.empty()) {
        param_bindings.emplace(
            param.name,
            types_.type_param(param.name, param.higher_kinded_arity));
      }
    }
    const auto *home = index_.find_module(impl.module_name);
    const auto ctx = resolve_ctx{
        .module = home,
        .param_bindings = &param_bindings,
        .use_type_param_stack = false,
        .quiet = true,
        .expected_ctor_arity = impl_expected_ctor_arity(*impl.decl, home)};
    return resolve_type(*impl.decl->for_type, ctx);
  }

  /// Resolves the type an extend block targets (its `extend` clause) as a
  /// *pattern*: a parameterized block's own parameters resolve to rigid
  /// `type_param` stand-ins, so `extend[T] holder[T]` yields `holder[T]` for
  /// later unification against a concrete receiver, exactly as
  /// `resolve_impl_target` does for `impl`.
  auto resolve_extend_target(const extend_ref &ext) -> type_id {
    if (ext.decl->for_type == nullptr) {
      return k_unknown_type;
    }
    auto param_bindings = std::unordered_map<std::string, type_id>{};
    for (const auto &param : ext.decl->type_params) {
      if (!param.name.empty()) {
        param_bindings.emplace(
            param.name,
            types_.type_param(param.name, param.higher_kinded_arity));
      }
    }
    const auto ctx = resolve_ctx{.module = index_.find_module(ext.module_name),
                                 .param_bindings = &param_bindings,
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
              // An `extend` block is an inherent impl, and gets the same
              // per-receiver instance discipline: without these two an
              // `extend` on a generic type type-checked and then compiled
              // nothing at all (spec/todo.md items 7 and 11).
              .block_type_params = &ext.decl->type_params,
              .impl_target_pattern = target,
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
        // An impl of a higher-kinded trait for a *prelude* constructor
        // (`impl monad for option`) has no `type_decl` to key `methods_`
        // by; its methods go in the constructor-name-keyed table instead.
        const auto is_builtin_ctor_target =
            target_entry.kind == type_kind::ctor_ref_kind &&
            target_entry.decl == nullptr;
        // An impl for an *applied* prelude generic (`impl from_iter[int32]
        // for list[int32]`). Like the bare-constructor case it has no
        // `type_decl` to key `methods_` by, and it used to fall into the
        // `continue` below: the impl was parsed and coherence-checked, then
        // silently never registered, so its members did not exist as far as
        // every lookup was concerned. `collect`'s collector impls are all
        // this shape, which is why `list[int32].from_iter(...)` reported no
        // such static member however it was written.
        const auto is_builtin_applied_target =
            target_entry.kind == type_kind::builtin_generic_kind &&
            target_entry.decl == nullptr && !target_entry.name.empty();
        if (target_entry.decl == nullptr && !is_builtin_ctor_target &&
            !is_builtin_applied_target) {
          continue;
        }
        auto &methods = (is_builtin_ctor_target || is_builtin_applied_target)
                            ? impl_methods_by_builtin_[target_entry.name]
                            : methods_[target_entry.decl];
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
              .block_type_params = &impl.decl->type_params,
              .impl_target_pattern = target,
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
          monomorphize_trait_default(*decl, target, target_type_name,
                                     trait_decl, trait_module, trait_file_id,
                                     methods);
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
                                  const ast::trait_decl *trait_decl,
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
      emit_diag(diag);
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
    // A higher-kinded trait's default body is written against the trait's
    // constructor parameter (`F[A]`, `M[B]`); checking it for a concrete
    // impl binds that parameter to the impl's target constructor, so every
    // `F[A]` in the clone re-resolves to the real applied type
    // (`apply_resolved_head` — `option[A]`, the same interned id ordinary
    // code gets).
    auto bound_trait_params = std::unordered_map<std::string, type_id>{};
    if (trait_decl != nullptr && !trait_decl->type_params.empty() &&
        trait_decl->type_params.front().higher_kinded_arity > 0 &&
        types_.entry(target).kind == type_kind::ctor_ref_kind) {
      bound_trait_params.emplace(trait_decl->type_params.front().name, target);
    }
    const auto pushed_trait_params = !bound_trait_params.empty();
    if (pushed_trait_params) {
      type_params_.push_back(bound_trait_params);
    }
    check_function(*raw, /*at_module_scope=*/false);
    if (pushed_trait_params) {
      pop_type_params();
    }
    module_ = saved_module;
    file_id_ = saved_file_id;
    self_type_ = saved_self;

    methods.push_back(method_entry{
        .decl = raw,
        .owner = trait_module,
        .from_trait = nullptr,
        .fixed_type_params = std::move(bound_trait_params),
    });
    synthesized_trait_defaults_.push_back(
        synthesized_method{.decl = raw,
                           .target_type_name = std::string(target_type_name),
                           .owner_module = trait_module->module_name});
  }

  /// Looks up a method by name on a user-type instance. An inherent or
  /// impl-provided method always wins over a trait default with the same
  /// name; the default is only used as a fallback.
  /// Whether an impl's target *pattern* (`holder[T]`, `boxed[int64]`) applies
  /// to a concrete receiver type. A type parameter in the pattern matches
  /// anything in that position; everything else must match nominally and
  /// argument-for-argument.
  ///
  /// This is what tells `impl get_it for boxed[int32]` from `impl get_it for
  /// boxed[int64]`. Both are recorded against the one `boxed` declaration,
  /// which is all method lookup used to consult — so a `boxed[int64]`
  /// receiver could resolve to the `int32` impl's body and be compiled under
  /// the wrong types.
  auto target_pattern_matches(type_id pattern, type_id concrete) -> bool {
    if (pattern == concrete || types_.is_unknown(pattern)) {
      return true;
    }
    const auto pattern_entry = types_.entry(pattern);
    if (pattern_entry.kind == type_kind::type_param_kind ||
        pattern_entry.kind == type_kind::param_app_kind ||
        pattern_entry.kind == type_kind::ctor_ref_kind) {
      return true;
    }
    const auto concrete_entry = types_.entry(concrete);
    if (pattern_entry.decl != concrete_entry.decl) {
      return false;
    }
    if (pattern_entry.decl == nullptr &&
        pattern_entry.name != concrete_entry.name) {
      return false;
    }
    if (pattern_entry.args.size() != concrete_entry.args.size()) {
      return false;
    }
    for (size_t i = 0; i < pattern_entry.args.size(); ++i) {
      if (!target_pattern_matches(pattern_entry.args[i],
                                  concrete_entry.args[i])) {
        return false;
      }
    }
    return true;
  }

  /// `instance_id` is the receiver's own `type_id`, used to pick between
  /// impls on different instantiations of one generic declaration; passing
  /// `k_unknown_type` (the default) keeps the historical declaration-level
  /// behavior for callers that have only a `type_entry` in hand.
  auto find_method(const type_entry &instance, std::string_view name,
                   type_id instance_id = k_unknown_type)
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
    // Within the first group, an impl whose target names this exact
    // instantiation beats one that merely covers it generically.
    const method_entry *generic_match = nullptr;
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
      const auto applies =
          types_.is_unknown(instance_id) ||
          target_pattern_matches(method.impl_target_pattern, instance_id);
      if (!applies) {
        continue;
      }
      if (method.from_trait == nullptr) {
        if (!type_mentions_param(method.impl_target_pattern)) {
          return &method;
        }
        if (generic_match == nullptr) {
          generic_match = &method;
        }
        continue;
      }
      if (trait_fallback == nullptr) {
        trait_fallback = &method;
      }
    }
    if (generic_match != nullptr) {
      return generic_match;
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

  /// Looks up an impl-provided method by name on a *prelude constructor*
  /// (`option`, `list`, ...) — the methods `impl monad for option` adds,
  /// keyed by constructor name via `impl_methods_by_builtin_`. An
  /// impl-written method wins over a monomorphized trait default, same
  /// priority rule as `find_method`.
  auto find_builtin_impl_method(std::string_view type_name,
                                std::string_view name) -> const method_entry * {
    build_method_table();
    const auto it = impl_methods_by_builtin_.find(std::string(type_name));
    if (it == impl_methods_by_builtin_.end()) {
      return nullptr;
    }
    const method_entry *trait_fallback = nullptr;
    for (const auto &method : it->second) {
      if (method.decl->name != name) {
        continue;
      }
      if (method.from_trait == nullptr) {
        return &method;
      }
      if (trait_fallback == nullptr) {
        trait_fallback = &method;
      }
    }
    return trait_fallback;
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
  auto unify_rigid(type_id pattern, type_id concrete,
                   std::unordered_map<std::string, type_id> &bindings) -> void {
    if (pattern == concrete || types_.is_unknown(concrete)) {
      return;
    }
    // Copies, not references: recursion can intern (nothing here does today,
    // but the entries are cheap and the deque discipline is easy to lose).
    const auto pattern_entry = types_.entry(pattern);
    if (pattern_entry.kind == type_kind::type_param_kind &&
        pattern_entry.ctor_arity == 0) {
      bindings.try_emplace(pattern_entry.name, concrete);
      return;
    }
    const auto concrete_entry = types_.entry(concrete);
    if (pattern_entry.kind == type_kind::param_app_kind) {
      // `F[A]` against `option[int32]`: the head solves from the outermost
      // nominal constructor, the arguments recurse positionally.
      const auto &head = types_.entry(pattern_entry.result);
      const auto arg_count_matches =
          concrete_entry.args.size() == pattern_entry.args.size();
      if (arg_count_matches &&
          (concrete_entry.kind == type_kind::builtin_generic_kind ||
           concrete_entry.kind == type_kind::struct_kind ||
           concrete_entry.kind == type_kind::sum_kind ||
           concrete_entry.kind == type_kind::opaque_kind)) {
        bindings.try_emplace(
            head.name,
            types_.ctor_ref(concrete_entry.name, concrete_entry.module_name,
                            concrete_entry.decl, concrete_entry.args.size()));
        for (size_t i = 0; i < pattern_entry.args.size(); ++i) {
          unify_rigid(pattern_entry.args[i], concrete_entry.args[i], bindings);
        }
      }
      return;
    }
    if (pattern_entry.kind != concrete_entry.kind) {
      // One structural allowance mirrors `compatible`: a reference pattern
      // meets its target, and vice versa.
      if (pattern_entry.kind == type_kind::ref_kind) {
        unify_rigid(pattern_entry.result, concrete, bindings);
      } else if (concrete_entry.kind == type_kind::ref_kind) {
        unify_rigid(pattern, concrete_entry.result, bindings);
      }
      return;
    }
    switch (pattern_entry.kind) {
    case type_kind::builtin_generic_kind:
    case type_kind::struct_kind:
    case type_kind::sum_kind:
    case type_kind::opaque_kind:
    case type_kind::tuple_kind:
    case type_kind::fn_kind: {
      if (pattern_entry.args.size() == concrete_entry.args.size()) {
        for (size_t i = 0; i < pattern_entry.args.size(); ++i) {
          unify_rigid(pattern_entry.args[i], concrete_entry.args[i], bindings);
        }
      }
      unify_rigid(pattern_entry.result, concrete_entry.result, bindings);
      return;
    }
    case type_kind::ref_kind:
    case type_kind::ptr_kind:
    case type_kind::array_kind:
      unify_rigid(pattern_entry.result, concrete_entry.result, bindings);
      return;
    default:
      return;
    }
  }

  /// Whether `id` mentions something still abstract — an unknown, a type
  /// parameter, a parameter application — anywhere in its structure.
  /// `is_unknown` answers the same question shallowly; this recurses, so
  /// `option[B]` and `fn(int32) -> option[B]` count as abstract too.
  auto mentions_abstract_type(type_id id) -> bool {
    if (types_.is_unknown(id)) {
      return true;
    }
    const auto entry = types_.entry(id); // copy: callers may intern after
    for (const auto arg : entry.args) {
      if (mentions_abstract_type(arg)) {
        return true;
      }
    }
    // `result` only means "component type" for these kinds; elsewhere it is
    // an unused default (`k_unknown_type`), which must not read as abstract.
    if (entry.kind == type_kind::fn_kind || entry.kind == type_kind::ref_kind ||
        entry.kind == type_kind::ptr_kind ||
        entry.kind == type_kind::array_kind) {
      return mentions_abstract_type(entry.result);
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
  auto
  substitute_solved(type_id id,
                    const std::unordered_map<std::string, type_id> &bindings)
      -> type_id {
    if (bindings.empty()) {
      return id;
    }
    const auto item = types_.entry(id); // copy: interning below can push
    if (item.kind == type_kind::type_param_kind && item.ctor_arity == 0) {
      const auto it = bindings.find(item.name);
      return it != bindings.end() ? it->second : id;
    }
    if (item.kind == type_kind::param_app_kind) {
      const auto head = types_.entry(item.result);
      const auto it = bindings.find(head.name);
      if (it == bindings.end()) {
        return id;
      }
      const auto ctor = types_.entry(it->second);
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
                 ? types_.user_type(*ctor.decl, ctor.module_name,
                                    std::move(args))
                 : types_.builtin_generic(ctor.name, std::move(args));
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
      return types_.builtin_generic(item.name, std::move(args));
    case type_kind::tuple_kind:
      return types_.tuple_of(std::move(args));
    case type_kind::fn_kind:
      return types_.fn_of(std::move(args), result);
    case type_kind::ref_kind:
      return types_.ref_to(result, item.is_mut);
    case type_kind::ptr_kind:
      return types_.ptr_to(result, item.is_mut);
    case type_kind::array_kind:
      return types_.array_of(result, item.array_size,
                             args.empty() ? k_unknown_type : args.front());
    case type_kind::struct_kind:
    case type_kind::sum_kind:
    case type_kind::opaque_kind:
      return item.decl != nullptr
                 ? types_.user_type(*item.decl, item.module_name,
                                    std::move(args))
                 : id;
    default:
      return id;
    }
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

  /// The concrete trait arguments the `impl` of `trait_name` for `concrete`
  /// supplies: asked about `counter` and `iterator`, an
  /// `impl iterator[int32] for counter` answers `[int32]`.
  ///
  /// Searched over every module's impls rather than read out of
  /// `impl_trait_index_`, which is keyed at *declaration* level — one entry
  /// per generic type rather than per instantiation — and records no
  /// arguments at all. That is enough for `type_has_trait`'s "does some impl
  /// exist", and not enough for "what does that impl say `T` is".
  auto trait_args_of_impl_for(type_id concrete, std::string_view trait_name)
      -> std::optional<std::vector<type_id>> {
    for (const auto &[module_name, members] : index_.modules) {
      for (const auto &impl : members.impls) {
        if (impl.decl->has_error || impl.decl->trait_type == nullptr ||
            impl.decl->trait_type->kind != ast::node_kind::named_type ||
            trait_name_of_impl(*impl.decl) != trait_name) {
          continue;
        }
        const auto target = strip_refs(resolve_impl_target(impl));
        if (types_.is_unknown(target)) {
          continue;
        }
        // Rigid unification alone does not mean the impl applies —
        // `unify_rigid` reports what *would* make the pattern match without
        // insisting that it does. Substituting the solution back and
        // requiring the original settles it, so `impl ... for holder[T]` is
        // accepted for `holder[int32]` and rejected for `boxed[int32]`.
        auto impl_bindings = std::unordered_map<std::string, type_id>{};
        unify_rigid(target, concrete, impl_bindings);
        if (substitute_solved(target, impl_bindings) != concrete) {
          continue;
        }

        auto param_bindings = std::unordered_map<std::string, type_id>{};
        for (const auto &param : impl.decl->type_params) {
          if (!param.name.empty()) {
            param_bindings.emplace(
                param.name,
                types_.type_param(param.name, param.higher_kinded_arity));
          }
        }
        const auto ctx = resolve_ctx{.module = &members,
                                     .param_bindings = &param_bindings,
                                     .use_type_param_stack = false,
                                     .quiet = true};
        const auto &named =
            dynamic_cast<const ast::named_type &>(*impl.decl->trait_type);
        auto args = std::vector<type_id>{};
        for (const auto &arg : named.type_args) {
          const auto *arg_type =
              dynamic_cast<const ast::type_expr *>(arg.value.get());
          args.push_back(arg_type != nullptr
                             ? substitute_solved(resolve_type(*arg_type, ctx),
                                                 impl_bindings)
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
  auto solve_from_bounds(const ast::func_decl &decl,
                         const module_members *owner,
                         std::unordered_map<std::string, type_id> &bindings)
      -> void {
    auto pattern_params =
        generic_param_bindings(decl, /*enclosing_block_params=*/nullptr);
    const auto pattern_ctx = resolve_ctx{.module = owner,
                                         .param_bindings = &pattern_params,
                                         .use_type_param_stack = false,
                                         .quiet = true};

    const auto apply = [&](std::string_view subject_name,
                           const ast::type_expr &bound_expr) -> void {
      const auto solved = bindings.find(std::string(subject_name));
      if (solved == bindings.end() || types_.is_unknown(solved->second) ||
          mentions_abstract_type(solved->second)) {
        return;
      }
      // A `+`-joined list contributes from each term independently; a bare
      // `Trait[Args]` is the one-term case of the same thing.
      auto terms = std::vector<const ast::type_expr *>{};
      if (bound_expr.kind == ast::node_kind::bound_type) {
        for (const auto &term :
             dynamic_cast<const ast::bound_type &>(bound_expr).value.terms) {
          if (term.type != nullptr) {
            terms.push_back(term.type.get());
          }
        }
      } else {
        terms.push_back(&bound_expr);
      }

      for (const auto *term : terms) {
        if (term->kind != ast::node_kind::named_type) {
          continue;
        }
        const auto &named = dynamic_cast<const ast::named_type &>(*term);
        if (named.path.empty() || named.type_args.empty()) {
          continue;
        }
        const auto concrete_args = trait_args_of_impl_for(
            strip_refs(solved->second), named.path.back());
        if (!concrete_args.has_value()) {
          continue;
        }
        for (size_t i = 0;
             i < named.type_args.size() && i < concrete_args->size(); ++i) {
          const auto *arg_type = dynamic_cast<const ast::type_expr *>(
              named.type_args[i].value.get());
          if (arg_type == nullptr) {
            continue;
          }
          unify_rigid(resolve_type(*arg_type, pattern_ctx), (*concrete_args)[i],
                      bindings);
        }
      }
    };

    for (const auto &param : decl.type_params) {
      if (!param.is_value_param && param.bound_or_type != nullptr) {
        apply(param.name, *param.bound_or_type);
      }
    }
    for (const auto &constraint : decl.where_constraints) {
      if (constraint.subject == nullptr ||
          constraint.bound_or_type == nullptr ||
          constraint.subject->kind != ast::node_kind::named_type) {
        continue;
      }
      const auto &subject =
          dynamic_cast<const ast::named_type &>(*constraint.subject);
      if (!subject.path.empty()) {
        apply(subject.path.back(), *constraint.bound_or_type);
      }
    }
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
    if (name == "debug" && has("debug")) {
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
                    "for this argument", call.args[i].value.get());
    }
    return fn_entry.result;
  }

  /// Hard-codes the result types of the handful of prelude methods the
  /// checker knows about for `list[T]`, `str`, `option[T]`, and `result[T, E]`
  /// — a stopgap until the standard library itself is represented as
  /// checkable declarations. Returns `unknown` for any other object kind or
  /// unrecognized method name, which is silently accepted rather than
  /// reported as an error.
  /// The type `object.name(...)` evaluates to for a builtin inherent
  /// method, or `k_unknown_type` when `name` is not one — which is what lets
  /// the caller fall through to `extend`/`impl` methods and UFCS.
  auto builtin_method_result(const type_entry &object, std::string_view name)
      -> type_id {
    const auto owner = builtin_method_owner(object);
    if (owner.empty()) {
      return k_unknown_type;
    }
    const auto element = object.args.empty() ? k_unknown_type : object.args[0];
    for (const auto &method : k_builtin_methods) {
      if (method.owner != owner || method.name != name) {
        continue;
      }
      switch (method.result) {
      case builtin_result_shape::usize_result:
        return types_.builtin("usize");
      case builtin_result_shape::bool_result:
        return types_.builtin("bool");
      case builtin_result_shape::unit_result:
        return types_.builtin("unit");
      case builtin_result_shape::element:
        return element;
      case builtin_result_shape::option_of_element:
        return types_.builtin_generic("option", {element});
      case builtin_result_shape::list_of_element:
        return types_.builtin_generic("list", {element});
      case builtin_result_shape::byte_slice:
        return types_.builtin_generic("slice", {types_.builtin("byte")});
      }
    }
    return k_unknown_type;
  }

  /// Reports `receiver.name(...)` naming no method that exists on a *builtin*
  /// receiver, once every lookup — builtin inherent, `impl`, `extend`, UFCS —
  /// has failed.
  ///
  /// This case used to fall through silently, returning `k_unknown_type` with
  /// no diagnostic at all: `nums.fliter(...)` on a `list[int32]` compiled, and
  /// whatever it "produced" unified with everything downstream because
  /// `k_unknown_type` deliberately does. A user-defined receiver has always
  /// reported "no method `x` on type `y`" here; a builtin one said nothing,
  /// which is exactly backwards — `list`, `str` and `option` are the types a
  /// beginner meets first.
  ///
  /// Returns `nullopt` rather than reporting when the receiver is a type
  /// whose method set is not actually known. `k_unknown_type` is the whole
  /// mechanism by which one gap in inference avoids cascading into unrelated
  /// errors, and a type parameter's methods depend on bounds resolved
  /// elsewhere — reporting on either would turn a silent gap into a wrong
  /// error, which is worse than the gap.
  [[nodiscard]] auto report_unknown_builtin_method(const ast::call_expr &call,
                                                   const ast::field_expr &field,
                                                   const type_entry &entry,
                                                   type_id object)
      -> std::optional<type_id> {
    // Restricting to these two kinds is what keeps an unresolved receiver
    // silent: `k_unknown_type` is `unknown_kind`, and a type parameter is
    // `type_param_kind`, so neither reaches the report below.
    if (entry.kind != type_kind::builtin_kind &&
        entry.kind != type_kind::builtin_generic_kind) {
      return std::nullopt;
    }

    build_method_table();
    auto candidates = builtin_method_names(entry);
    if (const auto found = extend_methods_by_builtin_.find(entry.name);
        found != extend_methods_by_builtin_.end()) {
      for (const auto &method : found->second) {
        candidates.push_back(method.decl->name);
      }
    }
    std::ranges::sort(candidates);
    candidates.erase(std::ranges::unique(candidates).begin(), candidates.end());

    const auto display = types_.display(object);
    auto diag = diagnostic(
        diagnostic_level::error,
        std::format("no method `{}` on type `{}`", field.field_name, display),
        file_id_);
    diag.with_label(field.span, "method not found");
    if (const auto suggestion = best_suggestion(field.field_name, candidates)) {
      diag.with_help(std::format("did you mean `{}`?", *suggestion));
    } else if (!candidates.empty()) {
      auto listed = std::string{};
      for (const auto &name : candidates) {
        if (!listed.empty()) {
          listed += ", ";
        }
        listed += std::format("`{}`", name);
      }
      diag.with_note(std::format("`{}` provides {}", display, listed));
      diag.with_help(std::format(
          "A builtin type's methods come from the compiler itself and from "
          "`extend {}:` blocks in the standard library. To add one of your "
          "own, write a free function whose first parameter is `{}` — it is "
          "then callable as a method.",
          entry.name, display));
    } else {
      diag.with_help(std::format(
          "`{}` has no methods. A free function whose first parameter is `{}` "
          "is callable as a method on one.",
          display, display));
    }
    emit_diag(diag);
    mark_error();
    infer_call_args_loosely(call);
    return k_error_type;
  }

  /// Every inherent method name a builtin receiver answers to, for the "did
  /// you mean" suggestion on an unknown method.
  [[nodiscard]] static auto builtin_method_names(const type_entry &object)
      -> std::vector<std::string> {
    const auto owner = builtin_method_owner(object);
    auto names = std::vector<std::string>{};
    if (owner.empty()) {
      return names;
    }
    for (const auto &method : k_builtin_methods) {
      if (method.owner == owner) {
        names.emplace_back(method.name);
      }
    }
    return names;
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

  /// Types a call to a `self`-taking method that is a *generic template*
  /// (`def at[n: usize](self, v: array[int32, n], i: index[n])`), resolving
  /// it to a monomorphized instance the way `check_receiver_call` does for a
  /// receiver-style one.
  ///
  /// Without this the call would resolve to the template — which has no
  /// compiled form, so the backends would look for a `holder::at` that was
  /// never lowered. The parameters are solved from the argument types alone:
  /// `self` is the receiver and carries no compile-time arguments of the
  /// method's own, so it is skipped here exactly as it is in the ordinary
  /// non-generic path.
  ///
  /// Returns `nullopt` when the method isn't a template, or when the
  /// enclosing declaration is itself one — inside a template the arguments
  /// are still symbols and there is nothing concrete to instantiate against.
  auto check_generic_instance_method_call(
      const ast::call_expr &call, const method_entry &method,
      std::string_view target_type_name, const ast::expr &receiver,
      type_id receiver_type, const explicit_generic_args &explicit_args = {})
      -> std::optional<type_id> {
    if (!is_generic_template(*method.decl) || in_const_generic_template_ ||
        in_type_generic_template_) {
      return std::nullopt;
    }
    const auto params = signature_params(*method.decl, method.owner,
                                         /*skip_self=*/true);
    // Both solvers run, because they see different things: `unify_rigid`
    // below reads a type parameter straight off an argument's type, while
    // `solve_value_params` (via `solved`) is what reaches *inside* a type to
    // pin a value parameter — an `array[int32, n]` parameter given an
    // `array[int32, 3]` argument solves `n := 3` there and nowhere else.
    auto solved = value_bindings{};
    check_call_args_against(
        call, params, method.decl->name,
        source_location{.file_id = file_id_, .span = method.decl->span},
        &solved);
    check_call_preconditions(call, *method.decl, params);

    auto bindings = std::unordered_map<std::string, type_id>{};
    solve_from_argument_types(call, params, bindings);

    const auto *instance =
        instantiate_hk_method(call, method, target_type_name, bindings,
                              explicit_args, solved, receiver_type);
    if (instance == nullptr) {
      return std::nullopt;
    }
    resolved_callees_[&call] =
        resolved_callee{.decl = instance,
                        .owner_module = method.owner->module_name,
                        .impl_target_type = "",
                        .receiver = &receiver};
    return substitute_solved(signature_return_type(*method.decl, method.owner),
                             bindings);
  }

  /// Whether calling `method` on a receiver of type `receiver` needs a
  /// monomorphized instance of its own, rather than resolving to the single
  /// compiled `Target::method` an ordinary impl member gets.
  ///
  /// Two shapes need one, and both are invisible on the method's own
  /// declaration:
  ///
  ///   - `impl[T] get_it[T] for holder[T]` — the impl is generic, so the
  ///     method's signature and body are written in terms of `T` and mean
  ///     something different for every receiver.
  ///   - `impl get_it[int32] for holder[int32]` — the impl is *not* generic,
  ///     but its target is an instantiation, so `holder[int32]` and
  ///     `holder[str]` may each have an impl and both would claim the name
  ///     `holder::get`.
  ///
  /// In both cases there is no one function to compile, which is why
  /// `hir::lower_impl_associated_functions` historically emitted nothing at
  /// all for either (design §2.1's G0).
  auto impl_needs_instance(const method_entry &method,
                           const type_entry &receiver) -> bool {
    return method.block_type_params != nullptr &&
           (!method.block_type_params->empty() || !receiver.args.empty());
  }

  /// Types a call to a method whose *impl block* — not the method itself — is
  /// what carries generic content (`impl_needs_instance`), resolving it to an
  /// instance compiled for this one receiver type.
  ///
  /// The impl's parameters are solved by rigid-unifying its target pattern
  /// (`holder[T]`) against the concrete receiver (`holder[int32]`), giving
  /// `T := int32`; the signature is then restated under that solution, so
  /// `def get(self) -> T` correctly reports `int32` instead of accepting
  /// anything at all (design §2's G1 — the silent-acceptance bug).
  ///
  /// The instance is keyed on the *receiver's* type arguments rather than the
  /// solved impl parameters, because those are what distinguish two impls for
  /// two instantiations of the same target: a non-generic
  /// `impl get_it[int32] for holder[int32]` solves nothing and would otherwise
  /// collide with its `holder[str]` sibling under one shared name.
  ///
  /// Returns `nullopt` when no instance is needed, or when the enclosing
  /// declaration is itself a template — inside one the receiver is still
  /// written in type parameters and there is nothing concrete to compile for.
  auto check_impl_generic_method_call(const ast::call_expr &call,
                                      const method_entry &method,
                                      const type_entry &receiver_entry,
                                      const ast::expr &receiver,
                                      type_id receiver_type)
      -> std::optional<type_id> {
    if (!impl_needs_instance(method, receiver_entry) ||
        in_const_generic_template_ || in_type_generic_template_) {
      return std::nullopt;
    }

    auto bindings = std::unordered_map<std::string, type_id>{};
    unify_rigid(method.impl_target_pattern, receiver_type, bindings);
    for (const auto &type_param : *method.block_type_params) {
      if (type_param.name.empty() || bindings.contains(type_param.name)) {
        continue;
      }
      // The receiver didn't pin one of the impl's parameters — an impl whose
      // parameter appears nowhere in its own target type. Nothing about the
      // call site can determine it, so there is no instance to compile.
      error_with_help(
          call.span,
          std::format("cannot tell which `{}` this call to `{}` means",
                      type_param.name, method.decl->name),
          std::format("`{}` is not determined by the receiver's type",
                      type_param.name),
          std::format(
              "`{}` is declared on the `impl` block, and the compiler solves "
              "it by matching the impl's target type against the receiver. "
              "Since `{}` doesn't appear in that target type, no receiver can "
              "pin it down. Move `{}` onto `{}` itself, so it can be solved "
              "from the arguments or given explicitly.",
              type_param.name, type_param.name, type_param.name,
              method.decl->name));
      return k_error_type;
    }

    // The impl's parameters go in as *fixed* bindings rather than as the
    // solution's `type_slots`, because `push_type_params` — which is what
    // consults `type_slots` — only ever walks the declaration's own
    // `type_params`, and `T` is not one of them. `fixed_type_params` is
    // pushed as a scope outright, which is how a monomorphized trait default
    // already carries its `F := option`, and is the mechanism that makes the
    // *body* of `def get(self) -> T` resolve `T` while it is being checked.
    auto scoped_params = method.fixed_type_params;
    scoped_params.insert(bindings.begin(), bindings.end());

    auto solution = generic_solution{};
    solution.suffix =
        std::format("${}", mangle_type_for_instance(receiver_type));

    auto params = signature_params(*method.decl, method.owner,
                                   /*skip_self=*/true, method.block_type_params);
    for (auto &param : params) {
      param.type = substitute_solved(param.type, bindings);
    }
    check_call_args_against(
        call, params, method.decl->name,
        source_location{.file_id = file_id_, .span = method.decl->span});
    check_call_preconditions(call, *method.decl, params);

    const auto name = std::format("{}::{}{}", receiver_entry.name,
                                  method.decl->name, solution.suffix);
    const auto *instance = find_or_check_generic_instance(
        call, *method.decl, method.owner, /*decl_file=*/std::nullopt, solution,
        name, &scoped_params, receiver_type);
    if (instance == nullptr) {
      return std::nullopt;
    }
    resolved_callees_[&call] =
        resolved_callee{.decl = instance,
                        .owner_module = method.owner->module_name,
                        .impl_target_type = "",
                        .receiver = &receiver};
    return substitute_solved(
        signature_return_type(*method.decl, method.owner, method.block_type_params),
        bindings);
  }

  /// The static-dispatch sibling of `check_impl_generic_method_call`: a
  /// `static def` reached through a *generic* impl block
  /// (`impl[T] maker[T] for crate[T]`), where the parameter to be solved
  /// belongs to the impl and not to the method.
  ///
  /// Such a method has no `type_params` of its own, so it used to take
  /// `resolve_type_param_static`'s non-generic path, which names the callee
  /// `crate[int32]::make` and stops. Nothing had instantiated the impl for
  /// `int32`, so no function ever bore that name: the call type-checked and
  /// then failed in lowering with "could not be resolved to a function in
  /// this compiled module". Type-checking a call and compiling nothing for
  /// it is exactly the split the receiver-taking sibling exists to prevent.
  ///
  /// The instance discipline is that sibling's, unchanged. The only
  /// difference is where the type solving the impl comes from: a qualified
  /// path (`crate[int32].make(...)`) or a solved type parameter
  /// (`C.make(...)`), rather than a receiver expression.
  ///
  /// `bindings` receives the impl's solution so the caller can restate the
  /// return type under it — `-> crate[T]` has to report `crate[int32]`.
  /// Returns `nullptr` when no instance could be built, leaving the caller
  /// on its existing path.
  auto check_impl_generic_static_call(
      const ast::call_expr &call, const method_entry &method, type_id target,
      std::unordered_map<std::string, type_id> &bindings)
      -> const ast::func_decl * {
    unify_rigid(method.impl_target_pattern, target, bindings);
    for (const auto &type_param : *method.block_type_params) {
      if (type_param.name.empty() || bindings.contains(type_param.name)) {
        continue;
      }
      // An impl parameter its own target type never mentions. No call site
      // can pin it, so there is nothing concrete to compile.
      return nullptr;
    }

    auto scoped_params = method.fixed_type_params;
    scoped_params.insert(bindings.begin(), bindings.end());

    auto solution = generic_solution{};
    solution.suffix = std::format("${}", mangle_type_for_instance(target));
    // Owned before `find_or_check_generic_instance` runs: checking the
    // instance body interns types, and this name is read after that.
    const auto target_name = std::string(types_.entry(target).name);
    const auto name = std::format("{}::{}{}", target_name, method.decl->name,
                                  solution.suffix);
    return find_or_check_generic_instance(call, *method.decl, method.owner,
                                          /*decl_file=*/std::nullopt, solution,
                                          name, &scoped_params, target);
  }

  /// Whether `method` is *receiver-style* for an `instance` of some
  /// constructor: declared without `self`, but with a first parameter that
  /// is an application of the very constructor the receiver instantiates —
  /// the shape a higher-kinded trait's methods take (`def bind[A, B](ma:
  /// M[A], ...)` under `impl monad for option` has first parameter
  /// `option[A]`). Method-call syntax passes the receiver as that first
  /// argument. A non-`self` method whose first parameter is anything else
  /// (an associated function like `from(errno)`) keeps its historical
  /// call-shape, where the written arguments fill every parameter.
  auto receiver_fills_first_param(const method_entry &method,
                                  const type_entry &instance) -> bool {
    if (method.decl->params.empty() ||
        param_name_of(method.decl->params.front()) == "self") {
      return false;
    }
    const auto params = signature_params(*method.decl, method.owner, false);
    if (params.empty()) {
      return false;
    }
    const auto &first = types_.entry(strip_refs(params.front().type));
    if (first.decl != nullptr || instance.decl != nullptr) {
      return first.decl != nullptr && first.decl == instance.decl;
    }
    return first.kind == type_kind::builtin_generic_kind &&
           instance.kind == type_kind::builtin_generic_kind &&
           first.name == instance.name;
  }

  /// Checks a method-syntax call resolved to a receiver-style method (see
  /// `receiver_fills_first_param`): the receiver fills the first declared
  /// parameter, the written arguments fill the rest. The callee's type
  /// parameters are solved by rigid pattern unification — the receiver
  /// first (`option[int32]` against `M[A]` gives `M = option, A = int32`),
  /// then each checked argument — and the declared return type is restated
  /// under that solution, so `ma.bind(f)` has a concrete type instead of an
  /// opaque generic one.
  auto check_receiver_call(const ast::call_expr &call,
                           const method_entry &method,
                           std::string_view target_type_name,
                           const ast::expr &receiver, type_id receiver_type,
                           const explicit_generic_args &explicit_args = {})
      -> type_id {
    const auto params = signature_params(*method.decl, method.owner, false);
    if (params.empty()) {
      infer_call_args_loosely(call);
      return signature_return_type(*method.decl, method.owner);
    }

    auto bindings = std::unordered_map<std::string, type_id>{};
    unify_rigid(params.front().type, receiver_type, bindings);
    type_mismatch(receiver.span,
                  substitute_solved(params.front().type, bindings),
                  receiver_type, "for this method's receiver");

    auto rest = std::vector<fn_param_info>(params.begin() + 1, params.end());
    for (auto &param : rest) {
      param.type = substitute_solved(param.type, bindings);
    }
    check_call_args_against(
        call, rest, method.decl->name,
        source_location{.file_id = file_id_, .span = method.decl->span});

    // Later arguments pin parameters the receiver didn't (`bind`'s `B`
    // arrives with the function argument): read each argument's recorded
    // type back and unify it too, then state the return type under the
    // full solution.
    if (const auto mapping = call_argument_mappings_.find(&call);
        mapping != call_argument_mappings_.end()) {
      const auto &args_by_param = mapping->second.args_by_param;
      for (size_t i = 0; i < rest.size() && i < args_by_param.size(); ++i) {
        if (args_by_param[i] == nullptr) {
          continue;
        }
        if (const auto found = node_types_.find(args_by_param[i]);
            found != node_types_.end()) {
          unify_rigid(rest[i].type, found->second, bindings);
        }
      }
    }

    // A generic method has no compiled form of its own — resolve the call
    // against a monomorphized instance (`option::bind$int32$str`) so both
    // backends see only concrete types. A non-generic method resolves
    // directly, exactly like a `self`-taking method does.
    if (!method.decl->type_params.empty()) {
      if (const auto *instance = instantiate_hk_method(
              call, method, target_type_name, bindings, explicit_args)) {
        resolved_callees_[&call] =
            resolved_callee{.decl = instance,
                            .owner_module = method.owner->module_name,
                            .impl_target_type = "",
                            .receiver = &receiver};
      }
    } else {
      resolved_callees_[&call] =
          resolved_callee{.decl = method.decl,
                          .owner_module = method.owner->module_name,
                          .impl_target_type = std::string(target_type_name),
                          .receiver = &receiver};
    }
    return substitute_solved(signature_return_type(*method.decl, method.owner),
                             bindings);
  }

  /// Types a method-call expression `object.method(args...)`: resolves
  /// `object`'s type, then tries (in order) a user-declared method, a
  /// `deriving`/prelude-trait-derived method, a callable struct field, or a
  /// builtin-type method — reporting an unknown-method error only for
  /// struct/sum/opaque objects, since builtin methods are best-effort.
  /// The compile-time arguments a method call wrote after the method name
  /// (`b.take[int64](5)`). Unlike the expression-position brackets of
  /// `zeros[8]()`, these are parsed as types outright — the grammar spells
  /// the suffix as a `type_arg_list` — so they need no reinterpretation.
  auto method_explicit_generic_args(const ast::field_expr &field)
      -> explicit_generic_args {
    auto args = explicit_generic_args{};
    args.reserve(field.generic_args.size());
    for (const auto &arg : field.generic_args) {
      if (arg != nullptr) {
        args.push_back(
            explicit_generic_arg{.type = arg.get(), .span = arg->span});
      }
    }
    return args;
  }

  /// Rejects brackets on a method that has no compile-time parameters to
  /// fill (§3.3). The shape can't mean indexing — that would index the
  /// call's *result*, which is written `m(...)[i]` — so there is nothing
  /// sensible to fall back to, and silently ignoring it is the bug this
  /// whole section exists to fix.
  auto check_method_accepts_generic_args(
      const ast::field_expr &field, const ast::func_decl &decl,
      const explicit_generic_args &explicit_args) -> bool {
    if (explicit_args.empty() || !decl.type_params.empty()) {
      return true;
    }
    error_with_help(
        field.span,
        std::format("`{}` takes no compile-time parameters, and this call "
                    "gives it {} in brackets",
                    decl.name, explicit_args.size()),
        "no compile-time parameters to fill",
        std::format(
            "`{}` is declared without a `[...]` parameter list, so there is "
            "nothing for these brackets to name. Drop them. (To index what "
            "the call returns, put the brackets after it: `.{}( ... )[i]`.)",
            decl.name, decl.name));
    return false;
  }

  // ==========================================================================
  //  UFCS — free functions called with method syntax
  //  (`spec/collections-algorithms-design.md` §3)
  //
  //  `recv.f(a...)` falls back to `f(recv, a...)` when no method `f` exists.
  //  This is the *last* arm of `infer_method_call`'s lookup ladder, always,
  //  which is the property that makes it safe to ship: a free function can
  //  never shadow a method, so adding one to the standard library can never
  //  silently change the meaning of code that already compiles.
  //
  //  Nothing new is needed downstream. Lowering already emits a receiver as
  //  a prepended first argument for any `resolved_callee` carrying one (see
  //  `resolved_callee::receiver`), and an empty `impl_target_type` already
  //  means "call this by its bare name" — which is exactly a free function.
  //  So a UFCS call lowers as the plain call it desugars to, and both
  //  backends never learn the feature exists.
  // ==========================================================================

  /// One free function that `recv.f(...)` might mean.
  struct ufcs_candidate {
    const ast::func_decl *decl = nullptr;
    const module_members *owner = nullptr;
    file_id_type file_id = 0;
  };

  /// Whether `decl` may be reached by method syntax at all (§3.3): it takes
  /// at least one parameter — there must be something for the receiver to
  /// fill — and that parameter is not `self`, which would make it a method
  /// already reached by the arms above.
  ///
  /// The design also requires `pub`. That is enforced only for functions
  /// from *other* modules, which is where the concern it answers lives
  /// (§R6, candidate-pool pollution from imports). Inside the declaring
  /// module a private `def helper(x: int32)` is already callable as
  /// `helper(x)`, and refusing `x.helper()` there would be an asymmetry with
  /// no safety story behind it.
  [[nodiscard]] auto is_ufcs_eligible(const ast::func_decl &decl,
                                      bool same_module) -> bool {
    if (decl.params.empty() || param_name_of(decl.params.front()) == "self") {
      return false;
    }
    return same_module || decl.visibility == ast::visibility::pub;
  }

  /// Every UFCS-eligible free function named `name` visible here, in the
  /// same order `infer_call` resolves a bare `name(...)`: this module, then
  /// explicit imports, then wildcard imports, then the prelude. Duplicates
  /// are collapsed by declaration identity, so a function reachable through
  /// two different imports is one candidate, not an ambiguity.
  [[nodiscard]] auto collect_ufcs_candidates(const std::string &name)
      -> std::vector<ufcs_candidate> {
    auto found = std::vector<ufcs_candidate>{};
    const auto add = [&](const func_decl_ref &fn, const module_members *owner,
                         bool same_module) -> void {
      if (fn.decl == nullptr || owner == nullptr ||
          !is_ufcs_eligible(*fn.decl, same_module)) {
        return;
      }
      if (std::ranges::any_of(found, [&](const ufcs_candidate &seen) -> bool {
            return seen.decl == fn.decl;
          })) {
        return;
      }
      found.push_back(ufcs_candidate{
          .decl = fn.decl, .owner = owner, .file_id = fn.file_id});
    };

    if (module_ != nullptr) {
      if (const auto it = module_->functions.find(name);
          it != module_->functions.end()) {
        add(it->second, module_, /*same_module=*/true);
      }
    }
    if (const auto *import = find_import(name)) {
      if (const auto *source = import_source_module(*import)) {
        if (const auto it =
                source->functions.find(imported_member_name(*import));
            it != source->functions.end()) {
          add(it->second, source, /*same_module=*/false);
        }
      }
    }
    for (const auto *source : wildcard_import_sources()) {
      if (const auto it = source->functions.find(name);
          it != source->functions.end()) {
        add(it->second, source, /*same_module=*/false);
      }
    }
    if (const auto prelude = find_prelude_function(name)) {
      add(prelude->first, prelude->second, /*same_module=*/false);
    }
    return found;
  }

  /// How a receiver of type `receiver` sits against a first parameter
  /// declared `param` (§3.2).
  enum class receiver_fit : uint8_t {
    fits,         ///< Callable as written.
    needs_mut,    ///< Shape matches, but `&mut T` wants a mutable receiver.
    does_not_fit, ///< Different type entirely; not a candidate.
  };

  /// Whether the receiver expression names something that may be mutated —
  /// a `var`/`mut` binding, or a value already held behind a `&mut`. Used
  /// only to decide between `fits` and `needs_mut`, never to *permit* a
  /// mutation: the ordinary mutability checks still run on the call.
  [[nodiscard]] auto receiver_is_mutable_lvalue(const ast::expr &receiver,
                                                type_id receiver_type) -> bool {
    const auto &entry = types_.entry(receiver_type);
    if (entry.kind == type_kind::ref_kind) {
      return entry.is_mut;
    }
    const auto *root = assignment_root_ident(receiver);
    if (root == nullptr) {
      return false;
    }
    const auto *binding = lookup_value(root->name);
    if (binding == nullptr) {
      return false;
    }
    return binding->origin == binding_origin::var_binding ||
           binding->origin == binding_origin::mut_binding;
  }

  /// The §3.2 adaptation table, first match wins. `param` is the callee's
  /// declared first parameter type; `receiver` is the receiver's own type,
  /// references and all.
  [[nodiscard]] auto classify_receiver_fit(type_id param, type_id receiver,
                                           bool receiver_is_mutable)
      -> receiver_fit {
    const auto &param_entry = types_.entry(param);
    const auto bare_param = strip_refs(param);
    const auto bare_receiver = strip_refs(receiver);

    // An unconstrained generic first parameter (`def sum[I, T](it: I)`)
    // accepts anything — unification binds it to whatever arrived.
    if (types_.entry(bare_param).kind == type_kind::type_param_kind) {
      return receiver_fit::fits;
    }
    if (!types_.compatible(bare_param, bare_receiver)) {
      return receiver_fit::does_not_fit;
    }
    if (param_entry.kind == type_kind::ref_kind && param_entry.is_mut) {
      // `&mut T` against a non-`mut` binding is a hard error, never a
      // silently skipped candidate — otherwise `v.iter_mut()` on a `let`
      // collapses into a baffling "no method `iter_mut`".
      return receiver_is_mutable ? receiver_fit::fits : receiver_fit::needs_mut;
    }
    return receiver_fit::fits;
  }

  /// Renders a candidate the way the ambiguity and no-fit diagnostics name
  /// it: fully qualified, with the signature the user has to satisfy.
  [[nodiscard]] auto describe_ufcs_candidate(const ufcs_candidate &candidate)
      -> std::string {
    auto rendered = std::string{};
    for (const auto &param : signature_params(*candidate.decl, candidate.owner,
                                              /*skip_self=*/false)) {
      if (!rendered.empty()) {
        rendered += ", ";
      }
      rendered += std::format("{}: {}", param.name, types_.display(param.type));
    }
    return std::format("{}.{}({})", candidate.owner->module_name,
                       candidate.decl->name, rendered);
  }

  /// The candidate's qualified name alone, for the "call it this way
  /// instead" half of the ambiguity help.
  [[nodiscard]] static auto ufcs_candidate_path(const ufcs_candidate &candidate)
      -> std::string {
    return std::format("{}.{}", candidate.owner->module_name,
                       candidate.decl->name);
  }

  /// Checks `recv.f(a...)` resolved to the free function `candidate`, as the
  /// call `f(recv, a...)` it desugars to: the receiver fills the first
  /// declared parameter, the written arguments fill the rest, and the
  /// callee's type parameters are solved from the receiver first and the
  /// arguments after — the same discipline `check_receiver_call` applies to
  /// a method, on a declaration that simply lives outside any `impl`.
  auto check_ufcs_call(const ast::call_expr &call, const ast::field_expr &field,
                       const ufcs_candidate &candidate, type_id receiver_type)
      -> type_id {
    const auto &decl = *candidate.decl;
    const auto params = signature_params(decl, candidate.owner,
                                         /*skip_self=*/false);
    record_expr_type(field, fn_type_of(decl, candidate.owner));

    auto bindings = std::unordered_map<std::string, type_id>{};
    // Solved through the references on both sides: a `&T` parameter taking
    // an auto-ref'd `T` receiver still has to bind `T`, and comparing the
    // decorated types would bind nothing.
    unify_rigid(strip_refs(params.front().type), strip_refs(receiver_type),
                bindings);

    auto rest = std::vector<fn_param_info>(params.begin() + 1, params.end());
    for (auto &param : rest) {
      param.type = substitute_solved(param.type, bindings);
    }
    auto solved = value_bindings{};
    const auto generic = generic_call_context{.decl = &decl,
                                              .owner = candidate.owner,
                                              .explicit_args = nullptr,
                                              .ufcs_receiver = nullptr,
                                              .seed = &bindings};
    check_call_args_against(
        call, rest, decl.name,
        source_location{.file_id = candidate.file_id, .span = decl.span},
        &solved, is_generic_template(decl) ? &generic : nullptr);
    check_call_preconditions(call, decl, params, field.object.get());

    if (!in_const_generic_template_ && !in_type_generic_template_ &&
        is_generic_template(decl) && is_free_function(decl, candidate.owner)) {
      if (const auto result = instantiate_generic_function(
              call, decl, candidate.owner, candidate.file_id, solved, params,
              /*explicit_args=*/{}, field.object.get())) {
        return *result;
      }
    }

    resolved_callees_[&call] =
        resolved_callee{.decl = &decl,
                        .owner_module = candidate.owner->module_name,
                        .impl_target_type = "",
                        .receiver = field.object.get()};
    return substitute_solved(signature_return_type(decl, candidate.owner),
                             bindings);
  }

  /// The fourth and last arm of the method-call ladder. Returns `nullopt`
  /// when no free function of this name is visible at all, leaving the
  /// caller to report its own not-found error; otherwise this call is UFCS's
  /// to answer, successfully or with a diagnostic.
  auto try_ufcs_call(const ast::call_expr &call, const ast::field_expr &field,
                     type_id receiver_type) -> std::optional<type_id> {
    if (field.object == nullptr) {
      return std::nullopt;
    }
    const auto candidates = collect_ufcs_candidates(field.field_name);
    if (candidates.empty()) {
      return std::nullopt;
    }

    const auto mutable_receiver =
        receiver_is_mutable_lvalue(*field.object, receiver_type);
    auto viable = std::vector<ufcs_candidate>{};
    auto wants_mut = std::vector<ufcs_candidate>{};
    for (const auto &candidate : candidates) {
      const auto params = signature_params(*candidate.decl, candidate.owner,
                                           /*skip_self=*/false);
      if (params.empty()) {
        continue;
      }
      switch (classify_receiver_fit(params.front().type, receiver_type,
                                    mutable_receiver)) {
      case receiver_fit::fits:
        viable.push_back(candidate);
        break;
      case receiver_fit::needs_mut:
        wants_mut.push_back(candidate);
        break;
      case receiver_fit::does_not_fit:
        break;
      }
    }

    if (viable.size() == 1) {
      return check_ufcs_call(call, field, viable.front(), receiver_type);
    }

    if (viable.size() > 1) {
      auto diag = diagnostic(
          diagnostic_level::error,
          std::format("`{}` is ambiguous here — {} visible functions accept "
                      "`{}` as their first argument",
                      field.field_name, viable.size(),
                      types_.display(receiver_type)),
          file_id_);
      diag.with_label(field.span, "ambiguous call");
      for (const auto &candidate : viable) {
        diag.with_note(describe_ufcs_candidate(candidate));
      }
      diag.with_help(std::format(
          "There is no most-specific winner to pick between these; say which "
          "one you mean by calling it qualified, with the receiver as its "
          "first argument: `{}({}{})`.",
          ufcs_candidate_path(viable.front()),
          describe(*field.object, predicate_subst{}),
          call.args.empty() ? "" : ", ..."));
      emit_diag(diag);
      mark_error();
      infer_call_args_loosely(call);
      return k_error_type;
    }

    if (!wants_mut.empty()) {
      const auto &candidate = wants_mut.front();
      error_with_help(
          field.span,
          std::format("`{}` needs to mutate its receiver, and `{}` is not a "
                      "mutable binding",
                      field.field_name,
                      describe(*field.object, predicate_subst{})),
          "receiver is immutable",
          std::format("`{}` takes its first argument by `&mut`. Bind the "
                      "receiver with `var` (or `let mut`) so it can be "
                      "mutated in place.",
                      describe_ufcs_candidate(candidate)));
      infer_call_args_loosely(call);
      return k_error_type;
    }

    // The name exists but nothing accepts this receiver — the highest-value
    // message of the three, because this is what a forgotten `.iter()`
    // looks like.
    auto diag =
        diagnostic(diagnostic_level::error,
                   std::format("`{}` cannot be called on `{}`",
                               field.field_name, types_.display(receiver_type)),
                   file_id_);
    diag.with_label(field.span,
                    std::format("no `{}` accepts `{}` as its first argument",
                                field.field_name,
                                types_.display(receiver_type)));
    // Naming each candidate's *first parameter type* is the whole value of
    // this message: the mismatch is always between that and the receiver,
    // and spelling both out is what turns "no method" into "you forgot an
    // `.iter()`" without having to guess which fix applies.
    auto wanted = std::string{};
    for (const auto &candidate : candidates) {
      const auto params = signature_params(*candidate.decl, candidate.owner,
                                           /*skip_self=*/false);
      diag.with_note(std::format(
          "`{}` is visible here, and wants `{}` first",
          describe_ufcs_candidate(candidate),
          params.empty() ? "nothing" : types_.display(params.front().type)));
      if (wanted.empty() && !params.empty()) {
        wanted = types_.display(params.front().type);
      }
    }
    diag.with_help(std::format(
        "A free function is callable as a method only when its first "
        "parameter accepts the receiver. Here that parameter is `{}` and the "
        "receiver is `{}` — either produce a `{}` to call this on (a "
        "`list[T]` becomes an iterator with `.iter()`), or call `{}` "
        "qualified with an argument that fits.",
        wanted, types_.display(receiver_type), wanted, field.field_name));
    emit_diag(diag);
    mark_error();
    infer_call_args_loosely(call);
    return k_error_type;
  }

  auto infer_method_call(const ast::call_expr &call,
                         const ast::field_expr &field) -> type_id {
    if (field.object == nullptr) {
      infer_call_args_loosely(call);
      return k_unknown_type;
    }

    // `expr.lit(...)`/`expr.ident(...)` — the AST-builder intrinsics that
    // construct a new `expr` quote value programmatically. `expr` here is
    // a contextual pseudo-namespace (see `is_prelude_value_name`), not a
    // real bound value, so it must be intercepted before ordinary object
    // typing/method lookup ever tries to resolve it. Actual construction
    // happens at evaluation time (`evaluator::try_eval_expr_builder_call`);
    // this just gives the call site a concrete type to check against.
    if (field.object->kind == ast::node_kind::ident_expr &&
        dynamic_cast<const ast::ident_expr &>(*field.object).name == "expr") {
      infer_call_args_loosely(call);
      static constexpr std::array<std::string_view, 6> k_expr_builder_names = {
          "lit", "ident", "field", "interp_concat", "debug", "binary"};
      if (std::ranges::find(k_expr_builder_names, field.field_name) ==
          k_expr_builder_names.end()) {
        error_with_help(
            field.span,
            std::format("`expr.{}` is not a recognized AST-builder "
                        "intrinsic",
                        field.field_name),
            "unknown AST-builder call",
            "Only `expr.lit(value)`, `expr.ident(name)`, "
            "`expr.field(object, name)`, `expr.interp_concat(a, b)`, "
            "`expr.debug(value)`, and `expr.binary(op, lhs, rhs)` construct "
            "a new `expr` quote value programmatically.");
        return k_error_type;
      }
      return types_.builtin("expr");
    }

    // `T.fields()`/`T.field_count()`/`T.name()` — compile-time reflection
    // over a struct type's own declaration (design plan section 4). Only
    // intercepted when both the method name matches one of the three
    // recognized reflection calls *and* the object actually names a real
    // type or an in-scope generic type parameter (the latter for a
    // `static def derive_show[T]()`-shaped body calling `T.fields()` — M7)
    // reachable here — a bare `ident_expr` whose name happens to collide
    // with an unrelated value or qualified-call target must still fall
    // through to the ordinary handling below untouched.
    if (field.object->kind == ast::node_kind::ident_expr &&
        (field.field_name == "fields" || field.field_name == "field_count" ||
         field.field_name == "name")) {
      const auto &type_name =
          dynamic_cast<const ast::ident_expr &>(*field.object).name;
      if (lookup_type_param(type_name).has_value()) {
        infer_call_args_loosely(call);
        if (field.field_name == "name") {
          return types_.builtin("str");
        }
        if (field.field_name == "field_count") {
          return types_.builtin("int32");
        }
        return k_unknown_type;
      }
      if (const auto found = find_type_decl_by_name(type_name)) {
        infer_call_args_loosely(call);
        if (field.field_name == "name") {
          return types_.builtin("str");
        }
        if (field.field_name == "field_count") {
          return types_.builtin("int32");
        }
        // `fields()` returns a compile-time list of per-field descriptors
        // — no builtin "list of X" type exists to check this against yet,
        // so it deliberately checks as `k_unknown_type` (same "don't
        // cascade a gap in the type system into a real error" role it
        // plays everywhere else) rather than inventing new type-system
        // machinery for this one narrow milestone. Real per-field values
        // (`comptime::value` `struct_instance`s with `name`/`type`
        // string fields) exist at evaluation time regardless — see
        // `comptime::try_eval_type_reflection_call` (`reflect.cpp`).
        return k_unknown_type;
      }
    }

    // `M.name()`/`M.functions()`/`M.types()`/`.function_count()`/
    // `.type_count()` — compile-time reflection over a module's surface
    // (design §5). Intercepted only when the object names a module in scope
    // and is not a type (type reflection above already claimed the type case),
    // so `comptime::try_eval_module_reflection_call` sees a well-typed call.
    if (field.object->kind == ast::node_kind::ident_expr &&
        (field.field_name == "name" || field.field_name == "functions" ||
         field.field_name == "types" || field.field_name == "function_count" ||
         field.field_name == "type_count")) {
      const auto &mod_name =
          dynamic_cast<const ast::ident_expr &>(*field.object).name;
      if (names_reflectable_module(mod_name)) {
        infer_call_args_loosely(call);
        if (field.field_name == "name") {
          return types_.builtin("str");
        }
        if (field.field_name == "function_count" ||
            field.field_name == "type_count") {
          return types_.builtin("int32");
        }
        // `functions()`/`types()` return a compile-time list of descriptors —
        // no "list of X" type to check against, so `k_unknown` (same
        // non-cascading role as type reflection's `fields()`).
        return k_unknown_type;
      }
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
    // `b.take[int64](5)`: the grammar's `"." IDENT "[" type_arg_list "]"`
    // suffix, which the parser has always built and the checker used to
    // throw away (§3 — a silently wrong answer, not a missing feature).
    const auto explicit_args = method_explicit_generic_args(field);

    switch (entry.kind) {
    case type_kind::struct_kind:
    case type_kind::sum_kind:
    case type_kind::opaque_kind: {
      if (const auto *method = find_method(entry, field.field_name, object)) {
        record_expr_type(field, fn_type_of(*method->decl, method->owner));
        if (!check_method_accepts_generic_args(field, *method->decl,
                                               explicit_args)) {
          infer_call_args_loosely(call);
          return k_error_type;
        }
        if (receiver_fills_first_param(*method, entry)) {
          return check_receiver_call(call, *method, entry.name, *field.object,
                                     object, explicit_args);
        }
        if (const auto instantiated = check_generic_instance_method_call(
                call, *method, entry.name, *field.object, object,
                explicit_args)) {
          return *instantiated;
        }
        // Tried after the method's own generics: a method that is a template
        // itself is handled above even when its impl is generic too, since
        // that path already solves from the arguments and the receiver.
        if (const auto instantiated = check_impl_generic_method_call(
                call, *method, entry, *field.object, object)) {
          return *instantiated;
        }
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
      // Last arm of the ladder: a free function called with method syntax
      // (§3). Reached only after every method lookup above has failed, so a
      // free function can never shadow a method.
      if (const auto ufcs = try_ufcs_call(call, field, object)) {
        return *ufcs;
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
      emit_diag(diag);
      mark_error();
      infer_call_args_loosely(call);
      return k_error_type;
    }
    case type_kind::existential_kind: {
      // Only the bound traits' own methods are callable on an existential
      // receiver — the concrete underlying type (`entry.result`) is a
      // checker-internal detail, not something the caller may otherwise
      // touch. Dispatch itself is ordinary static `find_method` against the
      // underlying type once the name is confirmed to belong to the bound.
      const auto in_bound = std::ranges::any_of(
          entry.existential_bound,
          [&](const bound_trait_ref &required) -> bool {
            const auto trait = find_trait_anywhere(required.trait_name);
            return trait.has_value() &&
                   trait_declares_method(*trait->first, field.field_name);
          });
      if (!in_bound) {
        error_with_help(
            field.span,
            std::format("no method `{}` on the existential type `{}`",
                        field.field_name, entry.name),
            "method not in bound",
            std::format("`{}` only exposes {}.", entry.name,
                        existential_bound_names(entry)));
        infer_call_args_loosely(call);
        return k_error_type;
      }
      const auto &underlying = types_.entry(entry.result);
      if (const auto *method = find_method(underlying, field.field_name)) {
        record_expr_type(field, fn_type_of(*method->decl, method->owner));
        record_instance_method_callee(call, *method, underlying.name,
                                      *field.object);
        const auto result = check_call_against_decl(
            call, *method->decl, method->owner, file_id_, /*skip_self=*/true);
        // A method that hands back the receiver's own concrete type (e.g. a
        // builder-style trait method returning `Self`) stays opaque to the
        // caller, exactly like the value that produced it.
        return types_.compatible(entry.result, result) ? object : result;
      }
      error_with_help(
          field.span,
          std::format("`{}` provides `{}` but no concrete implementation "
                      "was found",
                      entry.name, field.field_name),
          "method not implemented",
          "This is likely a trait default that hasn't been given a body — "
          "check the `impl` providing this trait for the underlying type.");
      infer_call_args_loosely(call);
      return k_error_type;
    }
    default: {
      // Builtin inherent methods take priority; an impl on the builtin's
      // constructor (`impl monad for option`) or an `extend` block fills in
      // only when the name isn't one of the hardcoded builtin methods.
      const auto builtin_result =
          builtin_method_result(entry, field.field_name);
      if (types_.is_unknown(builtin_result)) {
        if (const auto *method =
                find_builtin_impl_method(entry.name, field.field_name)) {
          record_expr_type(field, fn_type_of(*method->decl, method->owner));
          if (!check_method_accepts_generic_args(field, *method->decl,
                                                 explicit_args)) {
            infer_call_args_loosely(call);
            return k_error_type;
          }
          if (receiver_fills_first_param(*method, entry)) {
            return check_receiver_call(call, *method, entry.name, *field.object,
                                       object, explicit_args);
          }
          record_instance_method_callee(call, *method, entry.name,
                                        *field.object);
          return check_call_against_decl(call, *method->decl, method->owner,
                                         file_id_, /*skip_self=*/true);
        }
        if (const auto *method =
                find_extend_method_for_builtin(entry, field.field_name)) {
          record_expr_type(field, fn_type_of(*method->decl, method->owner));
          // Same instantiation ladder the user-type path above runs. Without
          // it an `extend[T] option[T]:` block type-checked and then compiled
          // nothing, because the two arms below name one shared function for
          // every element type and none was ever built.
          if (const auto instantiated = check_generic_instance_method_call(
                  call, *method, entry.name, *field.object, object,
                  explicit_args)) {
            return *instantiated;
          }
          if (const auto instantiated = check_impl_generic_method_call(
                  call, *method, entry, *field.object, object)) {
            return *instantiated;
          }
          record_instance_method_callee(call, *method, entry.name,
                                        *field.object);
          return check_call_against_decl(call, *method->decl, method->owner,
                                         file_id_, /*skip_self=*/true);
        }
        // Same last arm as the struct/sum/opaque case above, and the one
        // that matters most for `std.algo`: the whole catalog is free
        // functions over a builtin `list`/`slice` receiver. Inside the
        // `is_unknown` guard, so a real builtin method still wins.
        if (const auto ufcs = try_ufcs_call(call, field, object)) {
          return *ufcs;
        }
        if (const auto reported =
                report_unknown_builtin_method(call, field, entry, object)) {
          return *reported;
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
  /// Resolves `C.member(...)` once `C` has been substituted for the concrete
  /// `target` this instance solved it to (§5.2/§5.3).
  ///
  /// The instance name comes from the solved type's own interned name, so
  /// `C.from_iter` inside `collect[list_iter[int32], list[int32]]` names
  /// `list[int32]::from_iter`. That keeps lowering's naming scheme untouched:
  /// it sees an ordinary impl member on a concrete type and never learns a
  /// type parameter was involved.
  ///
  /// `nullopt` when `target` has no such static member, leaving the caller to
  /// fall through to its remaining cases (and, ultimately, to an ordinary
  /// unknown-member diagnostic carrying the §5.4 instantiation note).
  /// The concrete type a static call's receiver names, when that receiver is
  /// written as a generic application (`list[int32].from_iter(...)`).
  ///
  /// The receiver reaches the checker as an *expression*: `gen[int32]`
  /// parses as an index into something named `gen`, and `map[str, int32]` as
  /// a call of `map`, because in expression position that is all the parser
  /// could know. `expr_as_named_type` already rebuilds exactly these shapes,
  /// so the receiver is reinterpreted by the same route an explicit type
  /// argument takes and reaches the same builtins, user types, and imports.
  ///
  /// Resolution is deliberately `quiet`, and a head naming a value declines
  /// outright: this runs speculatively on every `<expr>.method(...)` whose
  /// object is an index or a call, and the overwhelming majority of those
  /// are real indexing and real calls. Reporting an undefined *type* for
  /// `values[0].method()` would be nonsense, so anything unresolved simply
  /// leaves the caller on its existing path, diagnostics included.
  auto resolve_applied_type_receiver(const ast::expr &object)
      -> std::optional<type_id> {
    auto named = expr_as_named_type(object);
    if (named == nullptr || named->type_args.empty()) {
      return std::nullopt;
    }
    // A head that names a value is indexing or invocation, whatever it might
    // also spell as a type. Same test the `ident_expr` case at the top of
    // `infer_qualified_call` applies.
    if (named->path.size() == 1 &&
        (lookup_value(named->path.front()) != nullptr ||
         named->path.front() == "self")) {
      return std::nullopt;
    }

    const auto &type = *named;
    synthesized_types_.push_back(std::move(named));
    auto ctx = current_resolve_ctx();
    ctx.quiet = true;
    const auto resolved = resolve_type(type, ctx);
    return types_.is_unknown(resolved) || resolved == k_error_type
               ? std::nullopt
               : std::optional{resolved};
  }

  auto resolve_type_param_static(const ast::call_expr &call,
                                 const ast::field_expr &field, type_id target,
                                 const std::string &fn_name)
      -> std::optional<type_id> {
    const auto &entry = types_.entry(target);
    const auto *method = find_method(entry, fn_name);
    if (method == nullptr && entry.decl == nullptr && !entry.name.empty()) {
      // A builtin target (`list[int32]`) has no `type_decl` for `find_method`
      // to key on; its impl members live in the constructor-name-keyed table.
      method = find_builtin_impl_method(entry.name, fn_name);
    }
    if (method == nullptr) {
      return std::nullopt;
    }
    // A `self`-taking method needs a receiver, and a qualified path gives it
    // none — that is an ordinary method call written wrongly, not this.
    if (!method->decl->params.empty() &&
        param_name_of(method->decl->params.front()) == "self") {
      return std::nullopt;
    }

    record_expr_type(field, fn_type_of(*method->decl, method->owner));
    const auto target_name = types_.display(target);
    const auto params = signature_params(*method->decl, method->owner,
                                         /*skip_self=*/false);
    auto solved = value_bindings{};
    check_call_args_against(
        call, params, method->decl->name,
        source_location{.file_id = file_id_, .span = method->decl->span},
        &solved);
    check_call_preconditions(call, *method->decl, params);

    auto bindings = std::unordered_map<std::string, type_id>{};
    solve_from_argument_types(call, params, bindings);

    // A `static def` carried by a generic impl block. Checked before the
    // method's own `type_params` are consulted, because the parameter that
    // makes this call generic is the *impl*'s and the method usually
    // declares none of its own — which is precisely why the branch below
    // used to claim it and name a function nobody compiled.
    // Restricted to methods declaring no parameters of their own: when a
    // method has its own (`static def from_iter[I]`), those still have to be
    // solved from the arguments, which is `instantiate_hk_method`'s job
    // below. Claiming such a call here would build an instance with the
    // impl's parameters bound and the method's left open, and the body would
    // then fail to lower on the first use of one.
    if (method->decl->type_params.empty() &&
        impl_needs_instance(*method, entry) && !in_const_generic_template_ &&
        !in_type_generic_template_) {
      if (const auto *instance =
              check_impl_generic_static_call(call, *method, target, bindings)) {
        resolved_callees_[&call] =
            resolved_callee{.decl = instance,
                            .owner_module = method->owner->module_name,
                            .impl_target_type = ""};
        return substitute_solved(signature_return_type(*method->decl,
                                                       method->owner,
                                                       method->block_type_params),
                                 bindings);
      }
    }

    if (method->decl->type_params.empty()) {
      resolved_callees_[&call] =
          resolved_callee{.decl = method->decl,
                          .owner_module = method->owner->module_name,
                          .impl_target_type = target_name};
      return substitute_solved(
          signature_return_type(*method->decl, method->owner), bindings);
    }
    // A method generic on both counts: its own parameters *and* the impl
    // block's. The branch above deliberately declines these, because `I` still
    // has to be solved from the arguments; but declining it must not also drop
    // `T`. Unifying the impl's target pattern against the concrete target
    // recovers it — `list[T]` against `list[int32]` gives `T := int32` — and
    // it then rides into the instance as a fixed binding, so the body resolves
    // `T` and the return type restates as `list[int32]` rather than `list[T]`.
    auto impl_bindings = std::unordered_map<std::string, type_id>{};
    if (method->block_type_params != nullptr && !in_const_generic_template_ &&
        !in_type_generic_template_) {
      unify_rigid(method->impl_target_pattern, target, impl_bindings);
      bindings.insert(impl_bindings.begin(), impl_bindings.end());
    }
    const auto *instance =
        instantiate_hk_method(call, *method, target_name, bindings,
                              /*explicit_args=*/{}, solved,
                              /*self_type=*/k_unknown_type, &impl_bindings);
    if (instance == nullptr) {
      return k_error_type;
    }
    resolved_callees_[&call] =
        resolved_callee{.decl = instance,
                        .owner_module = method->owner->module_name,
                        .impl_target_type = ""};
    return substitute_solved(
        signature_return_type(*method->decl, method->owner), bindings);
  }

  /// Explains a `C.member(...)` whose solved type has no such static member.
  ///
  /// This is the *common* failure of an inferred, unbounded generic like
  /// `collect` (§6): `C` is decoration-bounded at most, so an unsuitable one
  /// isn't caught at the call site and surfaces here instead. The message
  /// therefore has to carry its own context — which parameter, what it was
  /// solved to — because the instantiation notes supply the rest.
  auto report_missing_type_param_static(const ast::field_expr &field,
                                        const std::string &param_name,
                                        type_id target,
                                        const std::string &fn_name) -> void {
    error_with_help(
        field.span,
        std::format("no static `{}` on type `{}`", fn_name,
                    types_.display(target)),
        std::format("`{}` was solved to `{}`, which provides no `{}`",
                    param_name, types_.display(target), fn_name),
        std::format("`{}` here stands for whatever type this instance solved "
                    "it to, and the call needs that type to provide a static "
                    "`{}`. Either add one in an `extend {}:` block, or use a "
                    "type for `{}` that already has one.",
                    param_name, fn_name, types_.display(target), param_name));
  }

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
    } else if (object.kind == ast::node_kind::index_expr ||
               object.kind == ast::node_kind::call_expr) {
      // A static call on a generic *application*: `list[int32].from_iter(it)`,
      // `crate[int32].make(9)`. Resolved to the concrete target and then
      // dispatched by exactly the machinery a solved type parameter uses, so
      // `list[int32].from_iter` and a `C.from_iter` that solved `C` to
      // `list[int32]` name the same instance.
      const auto target = resolve_applied_type_receiver(object);
      if (!target.has_value()) {
        return std::nullopt;
      }
      return resolve_type_param_static(call, field, *target, fn_name);
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

      // A single-segment root naming a prelude constructor
      // (`option.pure(...)`) — an associated function provided by an impl
      // of a higher-kinded trait for that constructor. The callee's type
      // parameters solve from the checked arguments by rigid pattern
      // unification, so the call's type is the concrete instantiation
      // (`option.pure(1)` is an `option[int32]`).
      if (builtin_generic_arity(root.front()).has_value()) {
        if (const auto *method =
                find_builtin_impl_method(root.front(), fn_name)) {
          const auto has_self =
              !method->decl->params.empty() &&
              param_name_of(method->decl->params.front()) == "self";
          if (!has_self) {
            record_expr_type(field, fn_type_of(*method->decl, method->owner));
            const auto params =
                signature_params(*method->decl, method->owner, false);
            check_call_args_against(
                call, params, method->decl->name,
                source_location{.file_id = file_id_,
                                .span = method->decl->span});
            auto bindings = std::unordered_map<std::string, type_id>{};
            if (const auto mapping = call_argument_mappings_.find(&call);
                mapping != call_argument_mappings_.end()) {
              const auto &args_by_param = mapping->second.args_by_param;
              for (size_t i = 0; i < params.size() && i < args_by_param.size();
                   ++i) {
                if (args_by_param[i] == nullptr) {
                  continue;
                }
                if (const auto found = node_types_.find(args_by_param[i]);
                    found != node_types_.end()) {
                  unify_rigid(params[i].type, found->second, bindings);
                }
              }
            }
            // Same instance discipline as `check_receiver_call`: a generic
            // associated function resolves to its monomorphized copy.
            if (!method->decl->type_params.empty()) {
              if (const auto *instance = instantiate_hk_method(
                      call, *method, root.front(), bindings)) {
                resolved_callees_[&call] =
                    resolved_callee{.decl = instance,
                                    .owner_module = method->owner->module_name,
                                    .impl_target_type = ""};
              }
            } else {
              resolved_callees_[&call] =
                  resolved_callee{.decl = method->decl,
                                  .owner_module = method->owner->module_name,
                                  .impl_target_type = root.front()};
            }
            return substitute_solved(
                signature_return_type(*method->decl, method->owner), bindings);
          }
        }
      }

      // A single-segment root naming a *type parameter* (`C.from_iter(it)`
      // inside `def collect[I, C](self: I) -> C`). The parameter is
      // substituted for whatever this instance solved it to, and the member
      // is then resolved against that concrete type — dispatch is static and
      // monomorphic, so nothing here needs the trait to be object-safe
      // (§5.2).
      if (const auto bound = lookup_type_param(root.front())) {
        // Still a template: `C` is an abstract parameter, nothing is bound,
        // and there is no type to look a member up on. The call types as
        // `unknown` and is checked for real in each instance — the same
        // discipline `find_or_check_generic_instance` applies to the rest of
        // a generic body.
        if (mentions_abstract_type(*bound) || types_.is_unknown(*bound)) {
          infer_call_args_loosely(call);
          return k_unknown_type;
        }
        if (const auto result =
                resolve_type_param_static(call, field, *bound, fn_name)) {
          return result;
        }
        // `C` is bound, so this path is definitely a type-parameter static
        // dispatch — it just doesn't resolve. Diagnosed here rather than
        // left to fall through, which would report the parameter itself as
        // an undefined name and say nothing about the type it stands for.
        report_missing_type_param_static(field, root.front(), *bound, fn_name);
        infer_call_args_loosely(call);
        return k_error_type;
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
  /// a real declaration in the auto-imported prelude (`find_prelude_function`
  /// — `println`/`print`/`eprintln`/`eprint`), a handful of remaining
  /// prelude forms with their own hard-coded signature (`panic`, `assert`,
  /// ...), a builtin conversion call, a bare variant spelling, or an
  /// undefined-name error.
  /// A function reachable from the current module by bare name, together with
  /// where it came from — the module that owns it (its own or the one an
  /// import points at) and the file it was declared in.
  struct callable_lookup {
    const ast::func_decl *decl = nullptr;
    const module_members *owner = nullptr;
    file_id_type file_id{};
  };

  /// Finds the function `name` refers to here: a declaration of the current
  /// module, or one an import brings in under that name.
  auto find_callable_decl(std::string_view name)
      -> std::optional<callable_lookup> {
    if (module_ != nullptr) {
      if (const auto it = module_->functions.find(std::string(name));
          it != module_->functions.end()) {
        return callable_lookup{.decl = it->second.decl,
                               .owner = module_,
                               .file_id = it->second.file_id};
      }
    }
    if (const auto *import = find_import(name)) {
      if (const auto *source = import_source_module(*import)) {
        const auto member = imported_member_name(*import);
        if (const auto it = source->functions.find(member);
            it != source->functions.end()) {
          return callable_lookup{.decl = it->second.decl,
                                 .owner = source,
                                 .file_id = it->second.file_id};
        }
      }
    }
    return std::nullopt;
  }

  /// Recognizes an explicitly instantiated call — `zeros[8]()`,
  /// `pair[int32, str](a, b)` — where the brackets give the callee's
  /// compile-time arguments outright instead of leaving them to be read off
  /// the call's arguments.
  ///
  /// This is the answer for a parameter no argument *could* determine: a
  /// `def zeros[n: usize]() -> array[int32, n]` mentions `n` only in its
  /// return type, so nothing about `zeros()` says which `n` is meant, and
  /// only the call site can say. Falls through (`nullopt`) whenever the shape
  /// isn't this — the same brackets spell ordinary indexing, and the base
  /// naming a generic function is the only thing that tells the two apart.
  auto infer_explicit_generic_call(const ast::call_expr &call)
      -> std::optional<type_id> {
    auto explicit_args = explicit_generic_args{};
    const auto *base = explicit_generic_callee(*call.callee, explicit_args);
    if (base == nullptr || explicit_args.empty() ||
        base->kind != ast::node_kind::ident_expr) {
      return std::nullopt;
    }
    // A local value shadowing the name means the brackets are indexing it,
    // not instantiating anything.
    const auto &name = dynamic_cast<const ast::ident_expr &>(*base).name;
    if (lookup_value(name) != nullptr) {
      return std::nullopt;
    }
    const auto found = find_callable_decl(name);
    if (!found.has_value() || !is_generic_template(*found->decl)) {
      return std::nullopt;
    }
    record_expr_type(*base, k_unknown_type);
    return check_call_against_decl(call, *found->decl, found->owner,
                                   found->file_id, /*skip_self=*/false,
                                   explicit_args);
  }

  /// Recognizes `name[T](...)` — a compile-time generic call to a top-level
  /// `static def` function (e.g. `derive_show[point]()`), distinguished
  /// from ordinary indexing-then-calling by `ident_names_callable_decl`
  /// (mirrors `infer_index`'s own "generic instantiation, not indexing"
  /// branch). Types the call as the function's declared return type via the
  /// ordinary `check_call_against_decl` machinery (`T` isn't substituted
  /// into the signature — this milestone's only caller, `derive_show`,
  /// never mentions its type parameter in its own signature, only inside
  /// its body via reflection) so argument-count/type checking stays real.
  /// Returns `nullopt` when the shape doesn't match at all, so `infer_call`
  /// falls through to its ordinary dispatch.
  auto infer_comptime_generic_call(const ast::call_expr &call,
                                   const ast::index_expr &index)
      -> std::optional<type_id> {
    if (index.object == nullptr || index.index == nullptr ||
        index.object->kind != ast::node_kind::ident_expr) {
      return std::nullopt;
    }
    const auto &callee_ident =
        dynamic_cast<const ast::ident_expr &>(*index.object);
    const auto found = find_callable_decl(callee_ident.name);
    if (!found.has_value() || !found->decl->modifiers.is_static ||
        found->decl->type_params.empty()) {
      return std::nullopt;
    }
    const auto *decl = found->decl;
    const auto *owner = found->owner;
    const auto decl_file = found->file_id;
    // The type argument must name either an in-scope generic parameter (a
    // nested compile-time generic call forwarding its own type parameter)
    // or a real declared type — anything else falls through to ordinary
    // indexing so a genuine mistake still gets an ordinary diagnostic
    // rather than a confusing one from this narrow comptime-only path.
    if (index.index->kind != ast::node_kind::ident_expr) {
      return std::nullopt;
    }
    const auto &type_arg_name =
        dynamic_cast<const ast::ident_expr &>(*index.index).name;
    if (!lookup_type_param(type_arg_name).has_value() &&
        !find_type_decl_by_name(type_arg_name).has_value()) {
      return std::nullopt;
    }
    return check_call_against_decl(call, *decl, owner, decl_file,
                                   /*skip_self=*/false);
  }

  auto infer_call(const ast::call_expr &call, type_id expected) -> type_id {
    // Before anything else, including argument checking: every
    // instantiation path below reaches back for this by call node.
    if (!types_.is_unknown(expected) && expected != k_error_type) {
      call_expected_types_[&call] = types_.strip_refinement(expected);
    }
    if (call.callee == nullptr) {
      infer_call_args_loosely(call);
      return k_unknown_type;
    }

    if (const auto proof = infer_refinement_try_from(call)) {
      return *proof;
    }

    if (call.callee->kind == ast::node_kind::field_expr) {
      return infer_method_call(
          call, dynamic_cast<const ast::field_expr &>(*call.callee));
    }

    if (call.callee->kind == ast::node_kind::index_expr) {
      if (const auto result = infer_comptime_generic_call(
              call, dynamic_cast<const ast::index_expr &>(*call.callee))) {
        return *result;
      }
    }

    // `zeros[8]()` / `pair[int32, str](a, b)`: the brackets carry the
    // callee's compile-time arguments. Tried after the `static def` form
    // above, which is the narrower of the two and claims its own shape.
    if (call.callee->kind == ast::node_kind::index_expr ||
        call.callee->kind == ast::node_kind::call_expr) {
      if (const auto result = infer_explicit_generic_call(call)) {
        return *result;
      }
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
          // Same persistence as the prelude branch below: lowering needs
          // the owning module to rebuild this cross-module call.
          resolved_callees_[&call] =
              resolved_callee{.decl = it->second.decl,
                              .owner_module = source->module_name,
                              .impl_target_type = ""};
          return check_call_against_decl(call, *it->second.decl, source,
                                         it->second.file_id,
                                         /*skip_self=*/false);
        }
        if (const auto it = source->types.find(member);
            it != source->types.end()) {
          infer_call_args_loosely(call);
          return k_unknown_type;
        }
        infer_call_args_loosely(call);
        return k_unknown_type;
      }
      // The import binds a *module* rather than a member — `use std.iter`
      // makes `iter` name the module, and `import_source_module` says so by
      // returning null. A module is not callable, so this name is not this
      // branch's to answer, and falling through lets prelude and builtin
      // resolution have it.
      //
      // Returning `k_unknown_type` here instead made a module import shadow
      // every same-named function: a file that wrote `use std.iter` lost the
      // `iter` free function to the module binding, type-checked anyway
      // because unknown unifies with everything, and then failed in lowering
      // with "no concrete checked type is available for this node" — the
      // silent-acceptance shape the unknown type exists to avoid, not cause.
    }

    // Functions re-exported by a session-owned wildcard import
    // (`use a.b.*`) — same resolution and persistence as the explicit
    // import branch above.
    for (const auto *source : wildcard_import_sources()) {
      if (const auto it = source->functions.find(name);
          it != source->functions.end()) {
        record_expr_type(ident, fn_type_of(*it->second.decl, source));
        resolved_callees_[&call] =
            resolved_callee{.decl = it->second.decl,
                            .owner_module = source->module_name,
                            .impl_target_type = ""};
        return check_call_against_decl(call, *it->second.decl, source,
                                       it->second.file_id,
                                       /*skip_self=*/false);
      }
    }

    // Prelude functions — `println`/`print`/`eprintln`/`eprint` resolve
    // here to their real `std.console` declarations (auto-injected into
    // every session, `inject_stdlib_prelude`), exactly like the module-
    // local-function branch a few lines above, just against a module found
    // through prelude reachability instead of `module_`/an explicit `use`.
    if (const auto found = find_prelude_function(name)) {
      const auto &[fn, owner] = *found;
      record_expr_type(ident, fn_type_of(*fn.decl, owner));
      resolved_callees_[&call] =
          resolved_callee{.decl = fn.decl,
                          .owner_module = owner->module_name,
                          .impl_target_type = ""};
      return check_call_against_decl(call, *fn.decl, owner, fn.file_id,
                                     /*skip_self=*/false);
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
      emit_diag(diag);
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
  auto check_index_in_bounds(const ast::index_expr &index, type_id object,
                             type_id key) -> void {
    if (index.index == nullptr || index.object == nullptr ||
        !types_.is_integer(key)) {
      return;
    }
    const auto &entry = types_.entry(object);
    if (entry.kind != type_kind::array_kind || entry.args.empty()) {
      return;
    }
    const auto length = slot_poly(entry.args.front());
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

    switch (solve(facts_, goal)) {
    case proof_result::proved:
      proven_in_bounds_.insert(&index);
      return;
    case proof_result::refuted: {
      auto diag =
          diagnostic(diagnostic_level::error,
                     std::format("index out of bounds: `{}` is never true",
                                 goal_label(goal, "the index bound")),
                     file_id_);
      diag.with_label(index.index->span,
                      std::format("indexing a `{}`", types_.display(object)));
      if (const auto known = known_facts_note(goal)) {
        diag.children.emplace_back(diagnostic_level::note, *known, file_id_);
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

  // ==========================================================================
  //  Typed runtime proofs — `try_from`
  //  (`spec/dependent-types-design.md` §7)
  //
  //  The escape hatch that lets the reasoning fragment stay small without
  //  making the language incomplete. Everything the solver cannot prove, a
  //  `try_from` can *check*:
  //
  //      if let @some(i) = index[n].try_from(raw):
  //          v[i]        # `i` is an `index[n]` — the unwrap *is* the proof
  //
  //  No flow analysis is needed to see that: `i` simply has the refined type,
  //  because that is what the intrinsic returns.
  // ==========================================================================

  /// Recognizes `<refinement>.try_from(value)` — either `positive.try_from(x)`
  /// or, for a parameterized refinement, `index[n].try_from(x)` — and returns
  /// the refinement type it names.
  auto try_from_target(const ast::call_expr &call) -> std::optional<type_id> {
    if (call.callee == nullptr ||
        call.callee->kind != ast::node_kind::field_expr) {
      return std::nullopt;
    }
    const auto &field = dynamic_cast<const ast::field_expr &>(*call.callee);
    if (field.field_name != "try_from" || field.object == nullptr) {
      return std::nullopt;
    }

    // `positive` — a bare name. `index[n]` — parsed as an *index* expression,
    // since at parse time there is no telling a generic instantiation from a
    // subscript.
    const auto *name_node = field.object.get();
    const ast::index_expr *instantiation = nullptr;
    if (name_node->kind == ast::node_kind::index_expr) {
      instantiation = &dynamic_cast<const ast::index_expr &>(*name_node);
      name_node = instantiation->object.get();
    }
    if (name_node == nullptr || name_node->kind != ast::node_kind::ident_expr) {
      return std::nullopt;
    }
    const auto &name = dynamic_cast<const ast::ident_expr &>(*name_node).name;

    const auto found = find_type_decl_by_name(name);
    if (!found.has_value() || found->first == nullptr ||
        found->first->definition == nullptr ||
        found->first->definition->kind != ast::node_kind::refinement_type) {
      return std::nullopt;
    }

    // Rebuild the instantiation's arguments the same way a type annotation
    // would, so `index[n]` here and `index[n]` in a signature are one type.
    auto args = std::vector<type_id>{};
    if (instantiation != nullptr && instantiation->index != nullptr) {
      const auto &params = found->first->type_params;
      if (!params.empty()) {
        args.push_back(resolve_value_arg(*instantiation->index, params.front(),
                                         current_resolve_ctx(),
                                         instantiation->index->span));
      }
    }
    return make_user_type(*found->first, found->second, std::move(args));
  }

  /// Types and desugars a `try_from` call.
  ///
  /// Types it as `option[refined]`, and rewrites it into ordinary Kira that
  /// any backend already knows how to lower — no new HIR node, no new opcode,
  /// no intrinsic:
  ///
  ///     block:
  ///         let <tmp> = <value>
  ///         if <predicate with self := tmp>:
  ///             @some(<tmp>)
  ///         else:
  ///             @none
  ///
  /// The binding is what makes this honest: `<value>` is evaluated exactly
  /// once, even though it is *mentioned* twice, so a `try_from` of an
  /// effectful expression behaves the way the reader expects. The rewrite is
  /// recorded in `spliced_fragments`, which `hir::lower_expr` already consults
  /// to redirect a node to the syntax it was rewritten to.
  auto infer_refinement_try_from(const ast::call_expr &call)
      -> std::optional<type_id> {
    const auto refined = try_from_target(call);
    if (!refined.has_value()) {
      return std::nullopt;
    }
    const auto &entry = types_.entry(*refined);
    const auto base = entry.result;
    const auto result_type = types_.builtin_generic("option", {*refined});

    if (call.args.size() != 1 || call.args.front().value == nullptr) {
      error_with_help(
          call.span,
          std::format("`{}.try_from` takes exactly one argument",
                      types_.display(*refined)),
          "wrong number of arguments",
          std::format("Pass the `{}` you want to check, and match on the "
                      "`option[{}]` it returns.",
                      types_.display(base), types_.display(*refined)));
      return result_type;
    }

    const auto &value = *call.args.front().value;
    const auto found = infer_expr(value, base);
    type_mismatch(value.span, base, found, "for this value to be checked");

    if (entry.predicate == nullptr) {
      // The predicate failed to parse or check; its own declaration already
      // said so. Type the call anyway so nothing cascades.
      return result_type;
    }

    auto fragment = build_try_from_fragment(call, value, entry, base, *refined);
    if (fragment != nullptr) {
      spliced_fragments_[&call] = fragment;
    }
    return record_expr_type(call, result_type);
  }

  /// Builds (and type-checks) the rewritten body described by
  /// `infer_refinement_try_from`, returning the synthesized `block_expr` — or
  /// `nullptr` if the predicate or the value uses syntax the cloner doesn't
  /// cover, in which case the call still types correctly and only its lowering
  /// is refused, with a diagnostic.
  auto build_try_from_fragment(const ast::call_expr &call,
                               const ast::expr &value, const type_entry &entry,
                               type_id base, type_id refined)
      -> const ast::node * {
    auto predicate = ast::clone_expr(*entry.predicate);
    if (!predicate.has_value()) {
      error_with_help(
          call.span,
          std::format("`{}.try_from` cannot be compiled here: {}",
                      types_.display(refined), predicate.error().message),
          "this proof cannot be lowered",
          "A refinement's predicate has to be an ordinary expression the "
          "compiler can re-emit as the runtime check this proof compiles "
          "into. Simplify the predicate, or check the value with a hand-"
          "written `if` instead.");
      return nullptr;
    }

    const auto parameters = refinement_constants(entry);
    if (!parameters.has_value()) {
      if (in_const_generic_template_) {
        // `index[n].try_from(raw)` inside a function generic over `n`. There
        // is no check to build *here* — `n` is a symbol — but there will be
        // one in each instance the template is compiled into, where `n` is a
        // number (`instantiate_generic_function`). The template itself is never
        // lowered, so leaving this call without a fragment costs nothing.
        return nullptr;
      }
      error_with_help(
          call.span,
          std::format("`{}.try_from` needs its parameters to be known here",
                      types_.display(refined)),
          "this refinement is still generic",
          std::format(
              "The check this compiles into compares the value against the "
              "refinement's parameters, so they have to be real values at "
              "this point — `{}.try_from(...)` with a literal, not one "
              "written in terms of a value parameter the caller supplies.",
              types_.display(types_.strip_refinement(refined))));
      return nullptr;
    }

    // A name no source file can collide with, since `#` is not an identifier
    // character — this binding is the compiler's, not the user's, and must
    // never shadow something they wrote.
    const auto temp_name = std::format("#proof{}", proof_temporaries_++);
    specialize_predicate(*predicate, temp_name, *parameters);

    auto binding = ast::make<ast::binding_pattern>();
    binding->span = call.span;
    binding->name = temp_name;

    // The value is *not* cloned. A placeholder stands in for it, redirected
    // through `spliced_fragments` to the expression the user actually wrote —
    // which the checker has already typed, node for node. Cloning would mean
    // re-inferring the copy (re-reporting any diagnostic inside it) and would
    // limit `try_from` to whatever `ast::clone_expr` happens to cover. This
    // way the argument may be any expression at all, and it is evaluated
    // exactly once, where the user wrote it.
    auto placeholder = make_ident(temp_name, value.span, /*is_variant=*/false);
    const auto *placeholder_node = placeholder.get();
    spliced_fragments_[placeholder_node] = &value;
    record_expr_type(*placeholder_node, base);

    auto bind = ast::make<ast::let_stmt>();
    bind->span = call.span;
    bind->pattern = std::move(binding);
    bind->initializer = std::move(placeholder);

    auto some_call = make_variant_call("some", temp_name, call.span);
    auto none_value = make_variant_call("none", {}, call.span);
    const auto *some_expr = some_call.get();
    const auto *none_expr = none_value.get();
    const auto *predicate_expr = predicate->get();

    auto branch = ast::if_branch{};
    branch.span = call.span;
    branch.condition = std::move(*predicate);
    branch.body.push_back(wrap_in_stmt(std::move(some_call)));

    auto conditional = ast::make<ast::if_expr>();
    conditional->span = call.span;
    conditional->branches.push_back(std::move(branch));
    conditional->else_body.push_back(wrap_in_stmt(std::move(none_value)));
    const auto *conditional_expr = conditional.get();

    auto block = ast::make<ast::block_expr>();
    block->span = call.span;
    const auto *bound_pattern = bind->pattern.get();
    block->stmts.push_back(std::move(bind));
    block->stmts.push_back(wrap_in_stmt(std::move(conditional)));

    // Type the fragment the way the checker types anything else, so every
    // synthesized node lands in `node_types` and lowering can read it back.
    // The binding is introduced twice, deliberately: as the *base* type while
    // the predicate is checked (the predicate asks a question about a plain
    // `int32`), then as the *refined* type inside the `@some`, where the
    // branch we are in is itself the proof that the predicate holds. That
    // second binding is the entire mechanism — it is why unwrapping the
    // option hands back a value that simply *has* the refined type, with no
    // further obligation and no flow analysis.
    push_scope();
    bind_value(temp_name, base, binding_origin::let_binding, call.span);
    record_expr_type(*bound_pattern, base);
    require_bool(*predicate_expr, "a refinement predicate");
    bind_value(temp_name, refined, binding_origin::let_binding, call.span);
    const auto result_type = types_.builtin_generic("option", {refined});
    infer_expr(*some_expr, result_type);
    infer_expr(*none_expr, result_type);
    record_expr_type(*conditional_expr, result_type);
    record_expr_type(*block, result_type);
    pop_scope();

    const auto *fragment = block.get();
    proof_fragments_.push_back(std::move(block));
    return fragment;
  }

  /// Rewrites a cloned predicate so it speaks about *this* proof: `self`
  /// becomes the local the value was bound to, and each of the refinement's
  /// value parameters becomes the constant this instantiation supplied.
  ///
  /// Both substitutions are needed, and for the same reason. The predicate of
  /// `index[n]` is `self < n`; the check generated for `index[8].try_from(x)`
  /// has to be `#proof0 < 8`. Neither `self` nor `n` exists at runtime — one
  /// is the abstract value, the other a compile-time parameter — so both must
  /// be gone before the predicate can be compiled as ordinary code.
  ///
  /// Takes the owning pointer, not a reference, because replacing `n` with
  /// `8` swaps one node for another rather than editing it in place.
  auto specialize_predicate(
      ast::ptr<ast::expr> &slot, const std::string &value_binding,
      const std::unordered_map<std::string, int64_t> &parameters) -> void {
    if (slot == nullptr) {
      return;
    }
    switch (slot->kind) {
    case ast::node_kind::ident_expr: {
      auto &ident = dynamic_cast<ast::ident_expr &>(*slot);
      if (ident.name == "self") {
        ident.name = value_binding;
        return;
      }
      if (const auto it = parameters.find(ident.name); it != parameters.end()) {
        auto literal = ast::make<ast::literal_expr>();
        literal->span = ident.span;
        literal->lit_kind = token_kind::int_lit;
        literal->value = std::to_string(it->second);
        slot = std::move(literal);
      }
      return;
    }
    case ast::node_kind::binary_expr: {
      auto &binary = dynamic_cast<ast::binary_expr &>(*slot);
      specialize_predicate(binary.lhs, value_binding, parameters);
      specialize_predicate(binary.rhs, value_binding, parameters);
      return;
    }
    case ast::node_kind::unary_expr: {
      auto &unary = dynamic_cast<ast::unary_expr &>(*slot);
      specialize_predicate(unary.operand, value_binding, parameters);
      return;
    }
    case ast::node_kind::field_expr: {
      auto &field = dynamic_cast<ast::field_expr &>(*slot);
      specialize_predicate(field.object, value_binding, parameters);
      return;
    }
    case ast::node_kind::module_path_expr: {
      // `self.value` is a path, not a field access — see `atom_key`.
      auto &path = dynamic_cast<ast::module_path_expr &>(*slot);
      if (!path.segments.empty() && path.segments.front() == "self") {
        path.segments.front() = value_binding;
      }
      return;
    }
    case ast::node_kind::index_expr: {
      auto &index = dynamic_cast<ast::index_expr &>(*slot);
      specialize_predicate(index.object, value_binding, parameters);
      specialize_predicate(index.index, value_binding, parameters);
      return;
    }
    case ast::node_kind::call_expr: {
      auto &call = dynamic_cast<ast::call_expr &>(*slot);
      specialize_predicate(call.callee, value_binding, parameters);
      for (auto &arg : call.args) {
        specialize_predicate(arg.value, value_binding, parameters);
      }
      return;
    }
    default:
      return;
    }
  }

  /// The constant each of a refinement's value parameters was instantiated
  /// with at this use. `nullopt` when one of them is still open (an `index[n]`
  /// inside a function generic over `n`) — which means no runtime check can be
  /// generated *here*, because the value it would compare against does not
  /// exist until the enclosing template is monomorphized. The instances
  /// `instantiate_generic_function` makes of it are where the check does get
  /// built, with `n` a real number.
  auto refinement_constants(const type_entry &entry)
      -> std::optional<std::unordered_map<std::string, int64_t>> {
    auto constants = std::unordered_map<std::string, int64_t>{};
    if (entry.decl == nullptr) {
      return constants;
    }
    for (size_t i = 0; i < entry.decl->type_params.size(); ++i) {
      const auto &param = entry.decl->type_params[i];
      if (!param.is_value_param || i >= entry.args.size()) {
        continue;
      }
      const auto &slot = types_.entry(entry.args[i]);
      if (slot.kind != type_kind::const_value_kind) {
        return std::nullopt;
      }
      constants.emplace(param.name, slot.value.constant);
    }
    return constants;
  }

  /// A synthesized identifier.
  ///
  /// The span is not decoration. `is_variant_ident` tells `@some` from `some`
  /// by the span being one byte *wider* than the name — the `@` — since the
  /// parser lowers both to a plain `ident_expr`. A synthesized name therefore
  /// has to carry a span of exactly its own width, or the checker reads it as
  /// a variant and hunts for a sum type that declares it. `is_variant` sizes
  /// the span accordingly, which is how `@some` and `#proof0` are built by the
  /// same function without one being mistaken for the other.
  auto make_ident(std::string_view name, source_span span, bool is_variant)
      -> ast::ptr<ast::ident_expr> {
    auto ident = ast::make<ast::ident_expr>();
    ident->name = std::string(name);
    ident->span = source_span{
        .start = span.start,
        .end = span.start + static_cast<uint32_t>(name.size()) +
               (is_variant ? 1U : 0U),
    };
    return ident;
  }

  /// `@some(<binding>)`, or a bare `@none` when `binding` is empty.
  auto make_variant_call(std::string_view variant, std::string_view binding,
                         source_span span) -> ast::ptr<ast::expr> {
    auto name = make_ident(variant, span, /*is_variant=*/true);
    if (binding.empty()) {
      return name;
    }

    auto argument = make_ident(binding, span, /*is_variant=*/false);
    const auto argument_span = argument->span;

    auto call = ast::make<ast::call_expr>();
    call->span = span;
    call->callee = std::move(name);
    call->args.push_back(ast::call_arg{.span = argument_span,
                                       .name = std::nullopt,
                                       .value = std::move(argument)});
    return call;
  }

  /// Wraps an expression as a statement, the shape every block body wants.
  auto wrap_in_stmt(ast::ptr<ast::expr> value) -> ast::ptr<ast::node> {
    auto stmt = ast::make<ast::expr_stmt>();
    stmt->span = value->span;
    stmt->expr = std::move(value);
    return stmt;
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
      // `size_of[T]()` inside a concept/generic bound) or a real declared
      // type (e.g. `point` in a compile-time generic call like
      // `derive_show[point]()`) is a type argument, not a value reference —
      // skip ordinary name resolution so it doesn't spuriously report
      // "undefined name".
      const auto is_type_argument =
          index.index != nullptr &&
          index.index->kind == ast::node_kind::ident_expr &&
          (lookup_type_param(
               dynamic_cast<const ast::ident_expr &>(*index.index).name)
               .has_value() ||
           find_type_decl_by_name(
               dynamic_cast<const ast::ident_expr &>(*index.index).name)
               .has_value());
      if (index.index != nullptr && !is_type_argument) {
        infer_expr(*index.index, k_unknown_type);
      }
      return k_unknown_type;
    }

    const auto object = base_shape(infer_expr(*index.object, k_unknown_type));
    const auto &entry = types_.entry(object);
    const auto key =
        index.index != nullptr
            ? strip_refs(infer_expr(*index.index, types_.builtin("usize")))
            : k_unknown_type;
    check_index_in_bounds(index, object, key);
    const auto key_is_range =
        types_.entry(key).kind == type_kind::builtin_generic_kind &&
        types_.entry(key).name == "range";

    const auto key_span =
        index.index != nullptr ? index.index->span : index.span;
    const auto require_integer_key = [&] -> void {
      if (!key_is_range && !types_.is_unknown(key) && !types_.is_integer(key)) {
        error(key_span,
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
        require_integer_key();
        // A range slice of a `str` is a `str`; a single scalar index yields
        // the `byte` at that position in the UTF-8 backing bytes (mirroring
        // `slice[byte]`, which the `builtin_generic_kind` arm below already
        // element-indexes to `byte`).
        return key_is_range ? object : types_.builtin("byte");
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
    for (const auto *source : wildcard_import_sources()) {
      if (const auto it = source->types.find(std::string(name));
          it != source->types.end()) {
        return std::pair{it->second.decl, source->module_name};
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
        emit_diag(diag);
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
                      "for this field", field.value.get());
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

    // An inferred expectation that still mentions an unsolved type parameter
    // is withheld from the body. Propagating it makes the body infer *as*
    // that parameter — a `p => if ...: @some(x) else: @none` checked against
    // `option[U]` comes back `option[U]`, not `option[int32]` — and a body
    // type that mentions `U` tells the call-site solver nothing, so `U` is
    // then reported unsolved on a call that plainly determines it. Checked
    // against no expectation the body answers concretely and `U` falls out.
    //
    // Only for an expectation the *call site* supplied: a `return_type` the
    // user wrote is theirs to be checked against, abstract or not.
    const auto body_expectation =
        lambda.return_type == nullptr && mentions_abstract_type(declared_result)
            ? k_unknown_type
            : declared_result;

    auto body_result = k_unknown_type;
    if (lambda.body_expr != nullptr) {
      body_result = infer_expr(*lambda.body_expr, body_expectation);
      if (body_expectation != k_unknown_type) {
        type_mismatch(lambda.body_expr->span, body_expectation, body_result,
                      "as the lambda result");
      }
    } else {
      body_result = check_body_nodes(lambda.body_stmts, body_expectation);
    }
    pop_scope();

    auto result =
        declared_result != k_unknown_type ? declared_result : body_result;
    // An expected result that still mentions an unsolved type parameter
    // (the `M[B]` in `bind`'s function argument) yields to a concrete body
    // type: reporting `option[B]` back would tell rigid inference nothing
    // at a call site that plainly determines `B`.
    if (result != body_result && body_result != k_unknown_type &&
        mentions_abstract_type(result) &&
        !mentions_abstract_type(body_result)) {
      result = body_result;
    }
    return types_.fn_of(std::move(param_types), result);
  }

  /// Checks an `if`/`elif` branch's condition (requiring `bool`) or, for an
  /// `if let` branch, infers the scrutinee and checks the pattern against it.
  /// Both callers run this with the branch's own scope already open, so the
  /// names an `if let` pattern binds are visible in that branch's body and
  /// nowhere else — matching how lowering scopes them (`lower_if_let_chain`,
  /// `hir/lower.cpp`) and how `while let` already behaves.
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
      push_scope();
      check_if_branch_header(branch);
      // Inside a branch, its condition holds — and so does the refutation of
      // every branch before it, since reaching this one means those failed.
      auto assumed = fact_scope(facts_);
      for (const auto &earlier : expr.branches) {
        if (&earlier == &branch) {
          break;
        }
        assume_condition(earlier.condition.get(), /*negated=*/true);
      }
      assume_condition(branch.condition.get(), /*negated=*/false);
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
      // Reaching the `else` means every branch condition was false.
      auto refuted = fact_scope(facts_);
      for (const auto &branch : expr.branches) {
        assume_condition(branch.condition.get(), /*negated=*/true);
      }
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
      auto patterns = std::vector<const ast::pattern *>{};
      patterns.reserve(clause.patterns.size());
      for (const auto &pattern : clause.patterns) {
        patterns.push_back(dynamic_cast<const ast::pattern *>(pattern.get()));
      }
      check_loop_head_patterns(patterns, element);
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

  /// Types `yield expr` — only legal inside a `generator def` body. Checks
  /// the yielded value against the enclosing generator's item type
  /// (`generator_item_type_`), the same way a `return` statement checks
  /// against `return_type_`. A `yield` expression itself always has type
  /// `unit` — it's a statement-shaped suspension point, not a
  /// value-producing expression, even though it's grammatically an `expr`
  /// so it can appear as an expression-statement without extra plumbing.
  auto infer_yield(const ast::yield_expr &expr) -> type_id {
    if (!in_generator_) {
      error(expr.span, "`yield` outside a `generator def` body",
            "not inside a generator");
      if (expr.value != nullptr) {
        infer_expr(*expr.value, k_unknown_type);
      }
      return types_.builtin("unit");
    }
    const auto found = expr.value != nullptr
                           ? infer_expr(*expr.value, generator_item_type_)
                           : k_unknown_type;
    if (!types_.is_unknown(generator_item_type_)) {
      type_mismatch(expr.span, generator_item_type_, found,
                    "as the yielded value");
    }
    return types_.builtin("unit");
  }

  /// Returns the element type yielded by iterating `iterable` (array/list/
  /// slice/range/option element, or `char` for `str`), used by `for`
  /// loops/comprehensions. Reports a not-iterable error for a known
  /// non-iterable scalar type.
  /// Resolves a user type's `std.iter.iterator[T]` conformance for a `for`
  /// loop: finds a `next(mut self) -> option[T]` method on `iterable` and,
  /// when present, returns the dispatch a `for x in it: ...` loop needs to
  /// desugar into `while let @some(x) = it.next(): ...` (`element_type` is the
  /// `T`, the rest is what `hir::lower_iterator_loop` rebuilds the `it.next()`
  /// call from). `nullopt` for any type that isn't shaped like an iterator —
  /// including the builtin iterables (`list`/`slice`/`range`/...), which have
  /// their own dedicated loop shapes and never reach this path.
  /// `site`, when given, is the syntax whose iteration needs a *compiled*
  /// `next` — the `for` statement. An adapter declared
  /// `impl[I, T, U] iterator[U] for map_iter[I, T, U]` has no single compiled
  /// `next`, exactly as a written `it.next()` call would not
  /// (`check_impl_generic_method_call`); the difference is only that nothing
  /// in the source names the call, so the instance has to be requested from
  /// here or lowering finds no such function. Passing `nullptr` answers the
  /// element type without compiling anything, which is what `element_type_of`
  /// wants — it is asked about types in places that are not loops at all.
  auto try_resolve_iterator(type_id iterable, const ast::node *site = nullptr)
      -> std::optional<iterator_loop_dispatch> {
    const auto stripped = strip_refs(iterable);
    const auto &entry = types_.entry(stripped);
    if (entry.decl == nullptr) {
      return std::nullopt;
    }
    const auto *method = find_method(entry, "next");
    if (method == nullptr || method->decl->params.empty() ||
        param_name_of(method->decl->params.front()) != "self") {
      return std::nullopt;
    }

    // Solved unconditionally, not just when an instance is wanted: an impl
    // parameter reaching the *element* type (`-> option[U]`) leaves the
    // return type written in `U` until the receiver answers it, and an
    // unsubstituted `U` is what `hir::lower_iterator_loop` rejects as "the
    // iterator's item type did not resolve to a concrete type".
    auto bindings = std::unordered_map<std::string, type_id>{};
    if (method->block_type_params != nullptr) {
      unify_rigid(method->impl_target_pattern, stripped, bindings);
    }
    const auto ret = substitute_solved(
        signature_return_type(*method->decl, method->owner, method->block_type_params),
        bindings);
    const auto &ret_entry = types_.entry(strip_refs(ret));
    if (ret_entry.kind != type_kind::builtin_generic_kind ||
        ret_entry.name != "option" || ret_entry.args.empty()) {
      return std::nullopt;
    }

    auto dispatch =
        iterator_loop_dispatch{.decl = method->decl,
                               .owner_module = method->owner->module_name,
                               .impl_target_type = entry.name,
                               .element_type = ret_entry.args.front()};

    if (site == nullptr || !impl_needs_instance(*method, entry) ||
        in_const_generic_template_ || in_type_generic_template_) {
      return dispatch;
    }

    // Keyed on the receiver, and named after it, for the reason
    // `check_impl_generic_method_call` documents: two impls on two
    // instantiations of one target both claim `next`, and only the
    // receiver's arguments tell them apart.
    auto scoped_params = method->fixed_type_params;
    scoped_params.insert(bindings.begin(), bindings.end());
    auto solution = generic_solution{};
    solution.suffix = std::format("${}", mangle_type_for_instance(stripped));
    const auto name = std::format("{}::{}{}", entry.name, method->decl->name,
                                  solution.suffix);
    const auto *instance = find_or_check_generic_instance(
        *site, *method->decl, method->owner, /*decl_file=*/std::nullopt,
        solution, name, &scoped_params, stripped);
    if (instance == nullptr) {
      return dispatch;
    }
    // The instance's name already carries the receiver, so it is a plain
    // function name rather than a `Type::method` pair to be joined later.
    dispatch.decl = instance;
    dispatch.impl_target_type = "";
    return dispatch;
  }

  auto element_type_of(type_id iterable, source_span span) -> type_id {
    const auto stripped = strip_refs(iterable);
    const auto &entry = types_.entry(stripped);
    switch (entry.kind) {
    case type_kind::array_kind:
      return entry.result;
    case type_kind::builtin_generic_kind:
      if (entry.name == "list" || entry.name == "slice" ||
          entry.name == "slice_mut" || entry.name == "range" ||
          entry.name == "option" || entry.name == "generator") {
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
      // A user type that implements `std.iter.iterator[T]` iterates to `T`
      // (via `it.next() -> option[T]`); anything else has no element type.
      if (const auto iter = try_resolve_iterator(stripped)) {
        return iter->element_type;
      }
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
    // A refined `expected` is an *obligation*, not a shape hint. Inference
    // gets the base type, so an expression only ever comes back refined by
    // actually being one (a `positive` parameter read back, a `try_from`
    // unwrapped) — never by having been asked for one. Without this,
    // `needs_positive(0 - 3)` would infer its own argument *as* a `positive`
    // and the narrowing check would find nothing left to prove.
    return record_expr_type(
        expr, infer_expr_impl(expr, types_.strip_refinement(expected)));
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
    case ast::node_kind::yield_expr:
      return infer_yield(dynamic_cast<const ast::yield_expr &>(expr));
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
    case ast::node_kind::quote_expr: {
      const auto &quote = dynamic_cast<const ast::quote_expr &>(expr);
      switch (quote.fragment_kind) {
      case ast::quote_fragment_kind::stmt:
        return types_.builtin("stmt");
      case ast::quote_fragment_kind::def_expr:
        return types_.builtin("def_expr");
      case ast::quote_fragment_kind::type_expr:
        return types_.builtin("type_expr");
      case ast::quote_fragment_kind::expr:
        // A bare name or dotted path (`` `(int32)` ``, `` `(std.foo.bar)` ``)
        // is lexically ambiguous between an expression and a type — the
        // parser classifies it as `expr` since it has no annotation context
        // (see `parser::classify_and_parse_quote_fragment`'s doc comment).
        // When the surrounding context expects `type_expr` and the content
        // reinterprets as a named type, honor that instead of reporting a
        // spurious mismatch — this is the "deferred to the semantic layer"
        // disambiguation the parser punted on.
        if (!types_.is_unknown(expected) &&
            types_.entry(expected).kind == type_kind::builtin_kind &&
            types_.entry(expected).name == "type_expr" &&
            quote.parsed_body != nullptr &&
            reinterpret_as_named_type(*quote.parsed_body) != nullptr) {
          return types_.builtin("type_expr");
        }
        return types_.builtin("expr");
      case ast::quote_fragment_kind::none:
        // `none` means the parser couldn't classify the quoted content
        // (already diagnosed at parse time, or genuinely empty) — fall back
        // to `expr` rather than `unknown` so a downstream annotation
        // mismatch still gets a concrete, if possibly wrong, type to report
        // against instead of silently unifying with everything.
        return types_.builtin("expr");
      }
      return types_.builtin("expr");
    }
    case ast::node_kind::splice_expr: {
      const auto &splice = dynamic_cast<const ast::splice_expr &>(expr);
      if (splice.operand == nullptr) {
        return k_unknown_type;
      }
      const auto operand_type = infer_expr(*splice.operand, k_unknown_type);
      require_quote_value(operand_type, splice.operand->span,
                          "a spliced expression");
      // Reification: evaluate the operand at compile time and, if it
      // resolves to a quoted expression, graft that fragment in as this
      // splice's real content — type-checking it here (so `hir::lower` has
      // a `node_types_` entry to look up) and recording the splice ->
      // fragment mapping (`spliced_fragments_`) so lowering can find it.
      // No hygiene pass yet (M5): the fragment is checked directly against
      // whatever scope is active at the splice site.
      const auto fragment_value = comptime_eval_.evaluate(*splice.operand);
      if (fragment_value.is_error()) {
        return k_error_type;
      }
      if (fragment_value.kind != comptime::value_kind::expr_fragment ||
          fragment_value.fragment == nullptr) {
        error_with_help(
            splice.operand->span,
            std::format("a spliced expression must resolve to a quoted "
                        "expression, found {}",
                        quote_fragment_kind_name(fragment_value.kind)),
            "this splice is used in expression position",
            "Only a value quoted as `` `(...)` `` with expression content "
            "can be spliced here; a quoted statement or definition belongs "
            "in statement or item position instead.");
        return k_error_type;
      }
      const auto *fragment_expr =
          dynamic_cast<const ast::expr *>(fragment_value.fragment);
      if (fragment_expr == nullptr) {
        return k_error_type;
      }
      const auto fragment_type = infer_expr(*fragment_expr, expected);
      spliced_fragments_[&expr] = fragment_expr;
      return fragment_type;
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
    case ast::node_kind::interpolated_string_expr:
      return check_interpolated_string(
          dynamic_cast<const ast::interpolated_string_expr &>(expr));
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

  /// Types an interpolated string literal (`"{expr :spec}"`,
  /// `spec/string-formatting-design.md`): infers each embedded expression's
  /// type, resolves the format spec's type char to a required capability
  /// (a builtin primitive or a trait), checks the `.precision`-on-integer-
  /// style and dynamic-width/precision-must-be-`usize` rules, and records
  /// how `hir::lower` should render each segment in `interp_dispatches_`.
  /// The whole node always types as `str`.
  auto check_interpolated_string(const ast::interpolated_string_expr &node)
      -> type_id {
    const auto usize_type = types_.builtin("usize");

    const auto check_dynamic_size =
        [&](const std::variant<std::monostate, size_t, ast::ptr<ast::expr>>
                &slot) -> void {
      const auto *dynamic_expr = std::get_if<ast::ptr<ast::expr>>(&slot);
      if (dynamic_expr == nullptr || *dynamic_expr == nullptr) {
        return;
      }
      const auto found = infer_expr(**dynamic_expr, usize_type);
      if (!types_.is_unknown(found) && !types_.compatible(usize_type, found)) {
        error((*dynamic_expr)->span,
              std::format("format width must be `usize`, found `{}`",
                          types_.display(found)),
              "expected usize here");
      }
    };

    for (const auto &seg : node.segments) {
      if (seg.is_literal || seg.value == nullptr) {
        continue;
      }
      const auto value_type =
          strip_refs(infer_expr(*seg.value, k_unknown_type));

      if (seg.has_spec) {
        check_dynamic_size(seg.spec.width);
        check_dynamic_size(seg.spec.precision);
      }

      if (types_.is_unknown(value_type)) {
        continue;
      }

      const char type_char = (seg.has_spec && seg.spec.type_char.has_value())
                                 ? *seg.spec.type_char
                                 : '\0';

      const bool has_precision =
          seg.has_spec &&
          !std::holds_alternative<std::monostate>(seg.spec.precision);
      const bool is_integer_style = type_char == 'd' || type_char == 'x' ||
                                    type_char == 'X' || type_char == 'o' ||
                                    type_char == 'b';
      if (has_precision && is_integer_style) {
        error_with_help(
            seg.spec.span,
            std::format("`.precision` is not allowed with `{}`", type_char),
            "precision is only meaningful for `s`, `?`, `f`, `e`/`E`, `g`/`G`",
            "for integer styles, use width and zero-padding instead, e.g. "
            "\"{value :04x}\"");
      }

      const auto &value_entry = types_.entry(value_type);
      const bool is_builtin_int = types_.is_integer(value_type);
      const bool is_builtin_float = types_.is_float(value_type);
      const bool is_builtin_str = value_entry.kind == type_kind::builtin_kind &&
                                  value_entry.name == "str";
      const bool is_builtin_bool = types_.is_boolean(value_type);
      const bool is_builtin_char =
          value_entry.kind == type_kind::builtin_kind &&
          value_entry.name == "char";
      const bool is_any_builtin = is_builtin_int || is_builtin_float ||
                                  is_builtin_str || is_builtin_bool ||
                                  is_builtin_char;

      auto dispatch = interp_dispatch{};
      dispatch.type_char = type_char;
      dispatch.value_type = value_type;

      const auto try_trait_dispatch =
          [&](std::string_view trait_name,
              interp_dispatch::kind_t builtin_kind_fallback,
              bool builtin_allowed) -> bool {
        if (builtin_allowed) {
          dispatch.kind = builtin_kind_fallback;
          return true;
        }
        if (!type_has_trait(value_entry, trait_name)) {
          return false;
        }
        dispatch.kind = interp_dispatch::kind_t::trait_method;
        dispatch.impl_target_type = value_entry.name;
        if (const auto *method = find_method(value_entry, trait_name);
            method != nullptr) {
          dispatch.decl = method->decl;
          dispatch.owner_module = method->owner->module_name;
        }
        return true;
      };

      // What `value_type` actually supports — used only for the diagnostic
      // below when the requested style isn't one of them, so the message
      // never has to guess: builtins get a fixed description per category,
      // a user type gets a list of the capability traits it really
      // implements (empty if none, meaning only `impl show for T` etc. can
      // fix this).
      auto describe_supported = [&]() -> std::string {
        if (is_builtin_int) {
          return "the default/`s` style (`show`), `?` (`debug`), `d`, "
                 "`x`/`X`, `o`, `b`, and `c`";
        }
        if (is_builtin_float) {
          return "the default/`s` style (`show`), `?` (`debug`), `e`/`E`, "
                 "`f`, and `g`/`G`";
        }
        if (is_builtin_char) {
          return "the default/`s` style (`show`), `?` (`debug`), and `c`";
        }
        if (is_builtin_str || is_builtin_bool) {
          return "the default/`s` style (`show`) and `?` (`debug`)";
        }
        auto styles = std::vector<std::string>{};
        for (const auto &[trait_name, label] :
             {std::pair{"show", "the default/`s` style (`show`)"},
              std::pair{"debug", "`?` (`debug`)"},
              std::pair{"hex", "`x`/`X` (`hex`)"},
              std::pair{"octal", "`o` (`octal`)"},
              std::pair{"binary", "`b` (`binary`)"}}) {
          if (type_has_trait(value_entry, trait_name)) {
            styles.emplace_back(label);
          }
        }
        if (styles.empty()) {
          return "no format styles yet";
        }
        auto out = std::string{};
        for (size_t i = 0; i < styles.size(); ++i) {
          if (i > 0) {
            out += ", ";
          }
          out += styles[i];
        }
        return out;
      };

      auto ok = false;
      std::string trait_hint;
      switch (type_char) {
      case '\0':
      case 's':
        ok = try_trait_dispatch("show", interp_dispatch::kind_t::builtin_show,
                                is_any_builtin);
        trait_hint = "show";
        break;
      case '?':
        ok = try_trait_dispatch("debug", interp_dispatch::kind_t::builtin_debug,
                                is_any_builtin);
        trait_hint = "debug";
        break;
      case 'x':
      case 'X':
        ok = try_trait_dispatch("hex", interp_dispatch::kind_t::builtin_radix,
                                is_builtin_int);
        trait_hint = "hex";
        break;
      case 'o':
        ok = try_trait_dispatch("octal", interp_dispatch::kind_t::builtin_radix,
                                is_builtin_int);
        trait_hint = "octal";
        break;
      case 'b':
        ok = try_trait_dispatch(
            "binary", interp_dispatch::kind_t::builtin_radix, is_builtin_int);
        trait_hint = "binary";
        break;
      case 'd':
        ok = is_builtin_int;
        dispatch.kind = interp_dispatch::kind_t::builtin_radix;
        break;
      case 'e':
      case 'E':
      case 'f':
      case 'g':
      case 'G':
        ok = is_builtin_float;
        dispatch.kind = interp_dispatch::kind_t::builtin_float;
        break;
      case 'c':
        ok = is_builtin_int;
        dispatch.kind = interp_dispatch::kind_t::builtin_char;
        break;
      default:
        break;
      }

      if (!ok) {
        error_with_help(
            seg.value->span,
            std::format("`{}` does not support {} formatting",
                        types_.display(value_type),
                        type_char == '\0' ? "the requested"
                                          : std::string(1, type_char)),
            std::format("`{}` does not implement a required trait",
                        types_.display(value_type)),
            std::format("`{}` supports {} — implement `{} for {}` yourself "
                        "if you want this style to mean something specific",
                        types_.display(value_type), describe_supported(),
                        trait_hint.empty() ? "show" : trait_hint,
                        types_.display(value_type)));
        continue;
      }

      interp_dispatches_[seg.value.get()] = dispatch;
    }

    return types_.builtin("str");
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
        } else if (const auto folded =
                       explicit_value_argument(*array.fill_count);
                   folded.has_value() && *folded >= 0) {
          // Inside a monomorphized instance the count may be a value
          // parameter (`[0; n]` in a `def zeros[n: usize]`), which is a
          // definite number here even though it isn't spelled as a literal —
          // fold it, so the array gets a known length and the backends have
          // an allocation size. In the *template* there is no constant to
          // fold to and this correctly finds none.
          count = static_cast<uint64_t>(*folded);
        }
      }
      if (expected_is_list) {
        return types_.builtin_generic("list", {element});
      }
      const auto length = count.has_value()
                              ? types_.const_value(types_.usize_type(), *count)
                              : k_unknown_type;
      return array_with_length(element, length);
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
        type_mismatch(item->span, element, found, "for this element",
                      item.get());
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
      return array_with_length(
          element,
          types_.const_value(types_.usize_type(), array.elements.size()));
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
  /// Binds the loop variables of a `for` head — statement or comprehension
  /// clause — against `element`, the type of one thing the iterable yields.
  ///
  /// One pattern matches the element whole (`for (k, v) in pairs`); several
  /// are its positional components (`for k, v in pairs`), which is the same
  /// destructuring written without parentheses. The multi-name form used to
  /// bind every name to `k_unknown_type` — "could be anything" — so a body
  /// that used a loop variable at the wrong type was accepted in silence and
  /// then lowered against a type nobody had checked. Splitting an element
  /// that is not a tuple, or into the wrong number of pieces, is a mistake
  /// worth naming rather than papering over.
  auto
  check_loop_head_patterns(const std::vector<const ast::pattern *> &patterns,
                           type_id element) -> void {
    if (patterns.empty()) {
      return;
    }
    if (patterns.size() == 1) {
      if (patterns.front() != nullptr) {
        check_pattern(*patterns.front(), element);
      }
      return;
    }

    const auto element_entry = types_.entry(element);
    const auto is_tuple = element_entry.kind == type_kind::tuple_kind;
    // Point at the loop variables. The enclosing statement's span runs to
    // the end of the body, so using it would underline the loop's last line
    // rather than the names actually at fault.
    auto vars_span = source_span{};
    for (const auto *pattern : patterns) {
      if (pattern == nullptr) {
        continue;
      }
      vars_span = vars_span.empty() ? pattern->span
                                    : source_span{.start = vars_span.start,
                                                  .end = pattern->span.end};
    }

    if (is_tuple && element_entry.args.size() != patterns.size()) {
      error_with_help(
          vars_span,
          std::format(
              "this loop binds {} names, but each element of `{}` has {}",
              patterns.size(), types_.display(element),
              element_entry.args.size()),
          "the number of loop variables must match the element's arity",
          "write one name per component, or bind the whole element with a "
          "single name and index it");
    } else if (!is_tuple && !types_.is_unknown(element)) {
      error_with_help(
          vars_span,
          std::format("this loop binds {} names, but `{}` is not a tuple",
                      patterns.size(), types_.display(element)),
          "only a tuple element can be split across several loop variables",
          "use a single loop variable here");
    }

    for (size_t i = 0; i < patterns.size(); ++i) {
      if (patterns[i] == nullptr) {
        continue;
      }
      const auto component = is_tuple && i < element_entry.args.size()
                                 ? element_entry.args[i]
                                 : k_unknown_type;
      check_pattern(*patterns[i], component);
    }
  }

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
      // A binding takes the subject *unstripped*, unlike every structural
      // pattern below. Stripping exists so that destructuring can see
      // through a reference — matching a `&point` against `point { x, y }`
      // has to reach the fields — but a binding does not destructure
      // anything, it names the value it was handed. When that value really
      // is a reference, stripping invented a different type for it:
      // `@some(p)` on an `option[&mut int32]` bound `p` as `int32`, so `*p`
      // had nothing to dereference and reached lowering with no concrete
      // type at all. That is the shape every `iter_mut` yields.
      record_expr_type(pattern, subject);
      bind_value(binding.name, subject,
                 binding.is_mut ? binding_origin::mut_binding
                                : binding_origin::pattern_binding,
                 binding.span);
      // The name a pattern binds carries its type's facts, exactly as a `let`
      // does. This is what makes `try_from` work: after
      // `@some(i) => ...`, `i` has type `index[8]`, and it is *this* line that
      // turns that type into the usable fact `i < 8`. Mutable bindings are
      // skipped for the same reason a `var` is — see `assume_binding`.
      if (!binding.is_mut) {
        assume_binding(binding.name, subject);
      }
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
        emit_diag(diag);
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
    emit_diag(diag);
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

    // Everything known about the assigned value described the *old* one. Drop
    // it before checking the new value, so nothing proved about `x` before an
    // `x = read()` can be used after it.
    if (stmt.target != nullptr) {
      if (const auto *root = assignment_root_ident(*stmt.target)) {
        invalidate_facts(root->name);
      }
    }

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
            emit_diag(diag);
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
                      "from the assignment target", stmt.value.get());
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
                        "from the annotation", stmt.initializer.get());
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
          // An immutable binding's type is a permanent fact about it: a `let
          // i: index[n]` is an `i < n` for the rest of the block. (A `var` is
          // deliberately *not* assumed — see `check_assignment`, which has to
          // invalidate what assignment breaks.)
          if (!binding.is_mut) {
            assume_binding(binding.name, binding_type);
          }
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
                        "from the annotation", stmt.initializer.get());
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
      if (in_generator_) {
        // A generator's only output channel is `yield`; `return` just marks
        // exhaustion (translates to the iterator's `next()` returning
        // `none` forever after), so a value here would have nowhere to go.
        if (stmt.value != nullptr) {
          error_with_help(
              stmt.span,
              "`return <value>` is not allowed inside a `generator def`",
              "generators have no second return channel",
              "Use `yield` to hand values back to the caller, and a bare "
              "`return` to end the generator early.");
        }
        return types_.builtin("never");
      }
      if (stmt.value != nullptr) {
        const auto expected_for_value =
            checking_existential_return_ ? k_unknown_type : return_type_;
        const auto found = infer_expr(*stmt.value, expected_for_value);
        if (checking_existential_return_) {
          existential_underlying_ =
              join_branch_type(existential_underlying_, found, stmt.value->span,
                               "existential return");
        } else if (return_annotated_ &&
                   !types_.compatible(return_type_, found)) {
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
          emit_diag(diag);
          mark_error();
        } else if (return_annotated_) {
          // Structurally fine — but returning into a refined return type is
          // still a narrowing, and owes the same proof an argument would.
          check_narrowing(return_type_, found, stmt.value.get(),
                          stmt.value->span, "for this returned value");
        }
      } else if (!checking_existential_return_ && return_annotated_ &&
                 !types_.is_unit(return_type_) &&
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
        push_scope();
        check_if_branch_header(branch);
        // Path conditions, exactly as in `infer_if_expr` — see the section
        // comment on `assume_condition`.
        auto assumed = fact_scope(facts_);
        for (const auto &earlier : stmt.branches) {
          if (&earlier == &branch) {
            break;
          }
          assume_condition(earlier.condition.get(), /*negated=*/true);
        }
        assume_condition(branch.condition.get(), /*negated=*/false);
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
        auto refuted = fact_scope(facts_);
        for (const auto &branch : stmt.branches) {
          assume_condition(branch.condition.get(), /*negated=*/true);
        }
        const auto else_type = check_body_nodes(stmt.else_body, expected_tail);
        result = join_branch_type(result, else_type,
                                  stmt.else_body.back() != nullptr
                                      ? stmt.else_body.back()->span
                                      : stmt.span,
                                  "`if`");
      } else if (types_.entry(result).name == "never") {
        // No `else`: the untaken path always exists and is implicitly
        // `unit` — an `if` whose only branch(es) purely diverge (`return`/
        // `panic`/...) must not make the whole construct register as
        // `never` itself, or a later statement in the same block becomes
        // unreachable in the type system's eyes even though control
        // genuinely falls through when the condition is false. Concretely:
        // `if cond: return x` followed by more statements — `join_branch_type`
        // adopts the branch's `never` because `result` started as
        // `k_unknown_type` (see its "found is `never`" special case), which
        // is correct when there *is* an `else` (every path is spoken for)
        // but wrong here.
        result = unit;
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
        // Record the `next`-method dispatch so lowering can desugar a
        // user-iterator loop into `while let @some(x) = it.next(): ...`.
        if (auto iter = try_resolve_iterator(iterable, &stmt)) {
          for_iterator_dispatches_[&stmt] = std::move(*iter);
        }
      }
      push_scope();
      {
        auto patterns = std::vector<const ast::pattern *>{};
        patterns.reserve(stmt.patterns.size());
        for (const auto &pattern : stmt.patterns) {
          patterns.push_back(pattern.get());
        }
        check_loop_head_patterns(patterns, element);
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
      if (stmt.expr == nullptr) {
        return unit;
      }
      const auto operand_type = infer_expr(*stmt.expr, k_unknown_type);
      require_quote_value(operand_type, stmt.expr->span, "a spliced statement");
      // Reification, mirroring `splice_expr` above: a quoted statement
      // fragment is checked and grafted in directly; a quoted expression
      // fragment is accepted too (splicing `` `(f())` `` in statement
      // position is just an expression-statement, same as writing `f()`
      // there directly).
      const auto fragment_value = comptime_eval_.evaluate(*stmt.expr);
      if (fragment_value.is_error()) {
        return k_error_type;
      }
      if (fragment_value.kind == comptime::value_kind::stmt_fragment &&
          fragment_value.fragment != nullptr) {
        const auto fragment_type =
            check_body_node(*fragment_value.fragment, expected_tail);
        spliced_fragments_[&node] = fragment_value.fragment;
        return fragment_type;
      }
      if (fragment_value.kind == comptime::value_kind::expr_fragment &&
          fragment_value.fragment != nullptr) {
        const auto *fragment_expr =
            dynamic_cast<const ast::expr *>(fragment_value.fragment);
        if (fragment_expr != nullptr) {
          const auto fragment_type = infer_expr(*fragment_expr, expected_tail);
          spliced_fragments_[&node] = fragment_expr;
          return fragment_type;
        }
      }
      error_with_help(
          stmt.expr->span,
          std::format("a spliced statement must resolve to a quoted "
                      "statement or expression, found {}",
                      quote_fragment_kind_name(fragment_value.kind)),
          "this splice is used in statement position",
          "Only a value quoted with expression or statement content can be "
          "spliced here; a quoted definition belongs in item position "
          "instead.");
      return k_error_type;
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
      emit_diag(diag);
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

  /// True when a block's final statement guarantees the enclosing
  /// function's result is produced on every control-flow path through it —
  /// by `return`ing, or by every branch of a tail `if`/`match` doing so.
  ///
  /// Deliberately permissive: any statement kind this doesn't recognize
  /// counts as providing a value, because nested branch/arm tails are
  /// already join-checked against the expected type (`join_branch_type`,
  /// `check_match`) and diagnosed there. This predicate exists to close
  /// the holes typing alone can't see: a tail `if` with no `else` (its
  /// statement typing seeds the branch join with `expected_tail`, so the
  /// missing fall-through path never surfaces as a mismatch), and the same
  /// shape nested inside branch or arm tails.
  static auto
  block_provides_function_value(const std::vector<ast::ptr<ast::node>> &stmts)
      -> bool {
    for (const auto &item : stmts | std::views::reverse) {
      if (item != nullptr) {
        return node_provides_function_value(*item);
      }
    }
    // An empty (or fully recovered-away) block: stay permissive — the
    // parser has already complained about whatever went missing.
    return true;
  }

  /// True for a tail `while true:` — the one unit-typed statement control
  /// never falls past, since Kira has no `break`: the loop can only be
  /// left by `return`ing out of the enclosing function.
  static auto is_while_true(const ast::node &node) -> bool {
    const auto *stmt = dynamic_cast<const ast::while_stmt *>(&node);
    if (stmt == nullptr) {
      return false;
    }
    const auto *condition =
        dynamic_cast<const ast::literal_expr *>(stmt->condition.get());
    return condition != nullptr && condition->lit_kind == token_kind::kw_true;
  }

  /// See `block_provides_function_value` — the per-statement half of the
  /// recursion.
  static auto node_provides_function_value(const ast::node &node) -> bool {
    switch (node.kind) {
    case ast::node_kind::return_stmt:
      return true;
    case ast::node_kind::if_stmt: {
      const auto &stmt = dynamic_cast<const ast::if_stmt &>(node);
      return !stmt.else_body.empty() &&
             std::ranges::all_of(stmt.branches,
                                 [](const ast::if_branch &branch) -> bool {
                                   return block_provides_function_value(
                                       branch.body);
                                 }) &&
             block_provides_function_value(stmt.else_body);
    }
    case ast::node_kind::match_stmt: {
      const auto &stmt = dynamic_cast<const ast::match_stmt &>(node);
      // Compact `=> expr` arms always produce a value; recovered arms have
      // already been diagnosed.
      return std::ranges::all_of(
          stmt.arms, [](const ast::match_arm &arm) -> bool {
            return arm.body_expr != nullptr || arm.has_error ||
                   block_provides_function_value(arm.body_stmts);
          });
    }
    default:
      return true;
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
    // A value parameter this function has no constant for is still a symbol
    // here, which makes this the *template* — see the member's doc comment.
    const auto saved_template = in_const_generic_template_;
    in_const_generic_template_ = std::ranges::any_of(
        decl.type_params, [this](const auto &param) -> bool {
          return param.is_value_param && !param.name.empty() &&
                 !const_param_slots_.contains(param.name);
        });
    // The type-parameter analog: a type parameter with no binding in
    // `type_param_slots_` is still abstract, which makes this the template.
    const auto saved_type_template = in_type_generic_template_;
    in_type_generic_template_ = std::ranges::any_of(
        decl.type_params, [this](const auto &param) -> bool {
          return !param.is_value_param && !param.name.empty() &&
                 !type_param_slots_.contains(param.name);
        });
    auto saved_scopes = std::move(scopes_);
    scopes_.clear();
    push_scope();
    auto saved_reported = std::move(reported_undefined_);
    reported_undefined_.clear();
    // A function proves things only from what *its own* signature and body
    // establish; a sibling's facts are not in scope here any more than its
    // locals are.
    auto saved_facts = std::move(facts_);
    facts_.clear();

    bind_value_params(decl.type_params);

    const auto &inferred_types = param_types_for(decl, module_);
    for (size_t i = 0; i < decl.params.size(); ++i) {
      const auto &param = decl.params[i];
      auto type = k_unknown_type;
      if (param.type_annotation != nullptr) {
        // `current_resolve_ctx()` leaves `existential_allowed` at its
        // default `false`, so a `some Trait[Args]` annotation here is
        // rejected by `resolve_existential_type` itself with a clear
        // "not allowed here" diagnostic — parameters get no dedicated
        // message of their own.
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
    auto return_ctx = current_resolve_ctx();
    return_ctx.existential_allowed = true;
    return_ctx.is_generator_return = decl.modifiers.is_generator;
    return_type_ = return_annotated_
                       ? resolve_type(*decl.return_type, return_ctx)
                       : k_unknown_type;
    if (decl.return_type != nullptr) {
      // Same rationale as the parameter loop above: a declared return type
      // is resolved from a `type_expr`, not an expression, so it needs its
      // own persistence hook for lowering to read.
      record_expr_type(*decl.return_type, return_type_);
    }

    const auto saved_in_generator = in_generator_;
    const auto saved_generator_item = generator_item_type_;
    in_generator_ = decl.modifiers.is_generator;
    generator_item_type_ = k_unknown_type;
    if (in_generator_) {
      // `return_ctx.is_generator_return` (set above) already made
      // `resolve_type` resolve `some iterator[T]` straight to the concrete
      // `generator[T]` — see `resolve_existential_type`'s doc comment —
      // so `return_type_` here is never the general opaque wrapper.
      const auto &return_entry = types_.entry(return_type_);
      const auto is_generator_type =
          return_annotated_ &&
          return_entry.kind == type_kind::builtin_generic_kind &&
          return_entry.name == "generator" && !return_entry.args.empty();
      if (is_generator_type) {
        generator_item_type_ = return_entry.args.front();
      } else {
        error_with_help(
            decl.span,
            std::format("generator `{}` must declare a `some iterator[T]` "
                        "return type",
                        decl.name),
            "missing or invalid generator return type",
            "Write `-> some iterator[T]`, naming the type of value each "
            "`yield` produces.");
      }
      if (decl.body_expr != nullptr) {
        error_with_help(
            decl.span,
            std::format("generator `{}` cannot use a compact expression body",
                        decl.name),
            "expression-bodied generators aren't supported",
            "Give the generator an indented block body and `yield` values "
            "from inside it.");
      }
    }

    // A general `some Trait[Args]` existential return type (not the
    // generator sugar above): the body's concrete return points are
    // inferred and joined via `join_branch_type` into
    // `existential_underlying_` rather than checked against `return_type_`
    // directly, since `return_type_` is the fresh opaque id itself — no
    // concrete expression could ever match it.
    const auto saved_checking_existential = checking_existential_return_;
    const auto saved_existential_underlying = existential_underlying_;
    checking_existential_return_ = false;
    existential_underlying_ = k_unknown_type;
    const auto existential_return_id = return_type_;
    if (!in_generator_ && return_annotated_ &&
        types_.entry(return_type_).kind == type_kind::existential_kind) {
      checking_existential_return_ = true;
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
      // A generator has no single exit a `post` could describe. Its body runs
      // in steps and hands values out through `yield`; `return` there means
      // "exhausted", not "here is the result", and the value the *call*
      // produces is the generator object itself, which the body never names.
      // A `pre` still means exactly what it always meant — a condition on the
      // arguments, checked once before the body starts — so only the `post`
      // face is refused, and it is refused here, where the declaration is,
      // rather than later where only the lowering is visible.
      if (in_generator_ && !contract.is_pre) {
        error_with_help(
            contract.span,
            std::format("a `post` condition cannot be written on the "
                        "`generator def` `{}`",
                        decl.name),
            "a generator has no single result to describe",
            "A generator's body runs in steps and produces its values with "
            "`yield`; `return` inside it means the generator is exhausted, "
            "not that it is handing back a result. Constrain the arguments "
            "with a `pre` condition, or check each produced value in the "
            "body before yielding it.");
        continue;
      }
      if (contract.condition != nullptr && !contract.condition->has_error) {
        in_postcondition_ = !contract.is_pre;
        require_bool(*contract.condition, "a contract condition");
      }
    }
    in_postcondition_ = false;
    in_contract_ = false;

    // Only now, with every parameter bound and every contract checked, is
    // there anything to assume — `assume_binding` reads the parameters' types
    // back out of the scope the loop above filled in.
    collect_function_facts(decl);

    if (in_generator_) {
      // A generator body's tail statement isn't a return value the way an
      // ordinary function's is — its only output channel is `yield`
      // (checked in place via `infer_yield`, which reads
      // `generator_item_type_` directly) — so there's nothing to compare
      // against `return_type_` here.
      if (decl.body_expr == nullptr) {
        check_body_nodes(decl.body_stmts, k_unknown_type);
      }
    } else if (decl.body_expr != nullptr) {
      const auto expected_for_body =
          checking_existential_return_
              ? k_unknown_type
              : (return_annotated_ ? return_type_ : k_unknown_type);
      const auto found = infer_expr(*decl.body_expr, expected_for_body);
      if (checking_existential_return_) {
        existential_underlying_ =
            join_branch_type(existential_underlying_, found,
                             decl.body_expr->span, "existential return");
      } else if (return_annotated_) {
        type_mismatch(decl.body_expr->span, return_type_, found,
                      "as the function result", decl.body_expr.get());
      }
    } else if (!decl.body_stmts.empty()) {
      const auto expected_for_body =
          checking_existential_return_
              ? k_unknown_type
              : (return_annotated_ ? return_type_ : k_unknown_type);
      const auto tail = check_body_nodes(decl.body_stmts, expected_for_body);
      if (checking_existential_return_) {
        if (!types_.is_unit(tail) && !types_.is_unknown(tail)) {
          existential_underlying_ = join_branch_type(
              existential_underlying_, tail,
              decl.body_stmts.back() != nullptr ? decl.body_stmts.back()->span
                                                : decl.span,
              "existential return");
        }
      } else if (return_annotated_ && !types_.is_unit(return_type_) &&
                 !types_.is_unknown(return_type_) && !types_.is_unknown(tail) &&
                 tail != k_error_type) {
        const auto tail_span = decl.body_stmts.back() != nullptr
                                   ? decl.body_stmts.back()->span
                                   : decl.span;
        if (!types_.is_unit(tail) && !types_.compatible(return_type_, tail)) {
          error(tail_span,
                std::format("function `{}` returns `{}`, but its final "
                            "expression has type `{}`",
                            decl.name, types_.display(return_type_),
                            types_.display(tail)),
                "mismatched final expression");
        } else if (types_.is_unit(tail)
                       ? (decl.body_stmts.back() == nullptr ||
                          !is_while_true(*decl.body_stmts.back()))
                       : !block_provides_function_value(decl.body_stmts)) {
          // The body type-checked, but control can still fall off the end
          // without a value: either the tail statement produces `unit`
          // outright (e.g. the body ends in a `let` or a non-`while true`
          // loop), or a tail `if`/`match` leaves some path — typically a
          // missing `else` — that returns nothing.
          auto diag = diagnostic(
              diagnostic_level::error,
              std::format("function `{}` is declared to return `{}`, but "
                          "not every path through its body returns a value",
                          decl.name, types_.display(return_type_)),
              file_id_);
          diag.with_label(tail_span,
                          "control can reach the end of the function from "
                          "here without a value");
          if (decl.return_type != nullptr) {
            diag.with_label(decl.return_type->span,
                            std::format("`{}` promised here",
                                        types_.display(return_type_)));
          }
          diag.with_help(std::format(
              "Make every path produce a `{}`: add a `return`, cover the "
              "missing `else` or `match` arm, or make the final statement "
              "an expression of that type. If the function has nothing to "
              "return, change the return type to `unit`.",
              types_.display(return_type_)));
          emit_diag(diag);
          mark_error();
        }
      }
    }

    if (checking_existential_return_) {
      if (types_.is_unknown(existential_underlying_)) {
        error_with_help(
            decl.span,
            std::format("could not infer a concrete return type for `{}`",
                        decl.name),
            "existential return type has no concrete value",
            "An existential return type still needs a real value flowing "
            "out of the function on every path — return or produce a "
            "concrete value, not just `panic`/an infinite loop.");
      } else {
        auto &existential_entry = types_.mutable_entry(existential_return_id);
        if (types_.is_unknown(existential_entry.result)) {
          existential_entry.result = existential_underlying_;
          const auto &underlying_entry = types_.entry(existential_underlying_);
          for (const auto &required : existential_entry.existential_bound) {
            if (!type_has_trait(underlying_entry, required.trait_name)) {
              error_with_help(
                  decl.return_type->span,
                  std::format("`{}` does not implement `{}`",
                              types_.display(existential_underlying_),
                              required.trait_name),
                  "existential bound not satisfied",
                  std::format(
                      "Add `impl {} for {}: ...` providing the trait's "
                      "methods, or return a type that already implements "
                      "it.",
                      required.trait_name,
                      types_.display(existential_underlying_)));
            }
          }
        }
      }
    }

    checking_existential_return_ = saved_checking_existential;
    existential_underlying_ = saved_existential_underlying;
    in_generator_ = saved_in_generator;
    generator_item_type_ = saved_generator_item;
    return_type_ = saved_return;
    return_annotated_ = saved_annotated;
    reported_undefined_ = std::move(saved_reported);
    facts_ = std::move(saved_facts);
    scopes_ = std::move(saved_scopes);
    in_const_generic_template_ = saved_template;
    in_type_generic_template_ = saved_type_template;
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
          derived != "hash" && derived != "debug") {
        error_with_help(
            decl.span,
            std::format("cannot derive `{}` for `{}`", derived, decl.name),
            "not a derivable trait",
            "The derivable traits are `eq`, `ord`, `show`, `hash`, and "
            "`debug`; implement other traits with an explicit `impl` "
            "block.");
      }
    }

    if (decl.modifiers.is_packed && decl.definition != nullptr &&
        decl.definition->kind != ast::node_kind::struct_type_def) {
      const auto *kind_name = [&]() -> const char * {
        switch (decl.definition->kind) {
        case ast::node_kind::sum_type_def:
          return "a sum type";
        case ast::node_kind::refinement_type:
          return "a refinement type";
        default:
          return "a type alias";
        }
      }();
      error_with_help(
          decl.span,
          std::format("`packed` only applies to struct types; `{}` is {}",
                      decl.name, kind_name),
          "has no field layout to pack",
          "Remove `packed` here — only a struct-shaped `type` declaration "
          "has fields whose byte layout `packed` controls.");
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
            // The declaration's value parameters are ordinary values inside
            // its predicate — `index[n]`'s `self < n` reads `n` as a `usize`,
            // not as a type. Same rule as a `def`'s value parameters inside
            // its body (`check_function`).
            bind_value_params(decl.type_params);
            // A refinement predicate is a contract on a value, and lives
            // under the same purity rule as `pre`/`post`: it must be a
            // question about the value, not an action taken on the way past.
            in_contract_ = true;
            require_bool(*predicate, "a refinement predicate");
            in_contract_ = false;
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

  /// Whether `trait` declares a method item named `name` — used to check
  /// whether a method call on an existential-typed receiver is within its
  /// bound (`infer_method_call`'s `existential_kind` case). Supertrait
  /// requirements aren't walked; a method only reachable through a
  /// supertrait isn't yet considered part of the bound.
  auto trait_declares_method(const ast::trait_decl &trait,
                             std::string_view name) -> bool {
    for (const auto &item : trait.items) {
      if (item != nullptr && item->kind == ast::node_kind::func_decl &&
          dynamic_cast<const ast::func_decl &>(*item).name == name) {
        return true;
      }
    }
    return false;
  }

  /// Comma-separated `` `Trait[Args]` `` list for an existential type's
  /// bound, used in "only exposes ..." diagnostics.
  auto existential_bound_names(const type_entry &entry) -> std::string {
    auto out = std::string{};
    for (const auto &required : entry.existential_bound) {
      if (!out.empty()) {
        out += ", ";
      }
      out += std::format("`{}`", required.trait_name);
    }
    return out;
  }

  /// Checks an `impl` block: resolves its target type and (if present) its
  /// trait, validating that a name written before `for` from a type
  /// declaration rather than a trait is flagged; when the trait is known,
  /// checks member completeness/extras via `check_impl_members`; then checks
  /// every member declaration (methods, associated types) with `self_type_`
  /// bound to the impl's target.
  auto check_impl_decl(const ast::impl_decl &decl) -> void {
    push_type_params(decl.type_params);

    // The trait resolves first: its parameter's *kind* directs how the
    // `for` target is read — `impl monad for option` names the constructor
    // `option`, while `impl show for option` would name an instantiation.
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
              std::format("Write `extend {}:` to add inherent methods to a "
                          "type instead.",
                          trait_name));
        }
      }
    }

    auto target_ctx = current_resolve_ctx();
    const auto *hk_param =
        trait_decl != nullptr && !trait_decl->type_params.empty() &&
                trait_decl->type_params.front().higher_kinded_arity > 0
            ? &trait_decl->type_params.front()
            : nullptr;
    if (hk_param != nullptr) {
      target_ctx.expected_ctor_arity = hk_param->higher_kinded_arity;
    }
    const auto target =
        decl.for_type != nullptr
            ? strip_refs(resolve_type(*decl.for_type, target_ctx))
            : k_unknown_type;
    const auto &target_entry = types_.entry(target);

    // A higher-kinded trait is only implementable *for a constructor*: a
    // plain type has the wrong kind. (A wrong-arity constructor was already
    // diagnosed during resolution — `check_ctor_against_expected`.)
    if (hk_param != nullptr && decl.for_type != nullptr &&
        !types_.is_unknown(target) &&
        target_entry.kind != type_kind::ctor_ref_kind) {
      error_with_help(
          decl.for_type->span,
          std::format("trait `{}` abstracts over a type constructor, but "
                      "`{}` is a plain type",
                      trait_name, types_.display(target)),
          "impl target has the wrong kind",
          std::format("`{}` declares its parameter as `{}[{}]`, so it is "
                      "implemented for a name that still *takes* type "
                      "arguments — `option` or `list`, say — not for an "
                      "already-complete type.",
                      trait_name, hk_param->name,
                      hk_param->higher_kinded_arity == 1 ? "_" : "_, _"));
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

    // Persist this impl's associated-type bindings under (target, trait) so
    // a method call resolved from outside the impl (an operator overload's
    // dispatch — see `resolve_operator_return_type`) can still resolve a
    // `self.output`-shaped return type correctly.
    if (!trait_name.empty() && !types_.is_unknown(target)) {
      impl_assoc_types_[target][trait_name] = self_assoc_types_;
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
    // A parameterized block's parameters scope its target type *and* every
    // member, so they go on the stack before the target resolves — otherwise
    // `extend[T] holder[T]` cannot even name `T` in its own `extend` clause.
    push_type_params(decl.type_params);
    const auto target =
        decl.for_type != nullptr
            ? strip_refs(resolve_type(*decl.for_type, current_resolve_ctx()))
            : k_unknown_type;

    // `extend gen:` where `gen` is generic: the target names a constructor
    // that still takes arguments, so no member can say what `self`'s fields
    // hold. This used to type-check and then die in lowering on the first use
    // of a field ("no concrete checked type is available for this node"),
    // which told the user nothing about the actual mistake.
    const auto &target_entry = types_.entry(target);
    const auto target_is_unapplied_ctor =
        target_entry.kind == type_kind::ctor_ref_kind ||
        (target_entry.decl != nullptr &&
         !target_entry.decl->type_params.empty() && target_entry.args.empty());
    if (decl.type_params.empty() && decl.for_type != nullptr &&
        target_is_unapplied_ctor) {
      const auto name = std::string(target_entry.name);
      error_with_help(
          decl.for_type->span,
          std::format("`{}` still takes type arguments, so `extend {}:` does "
                      "not name a type to extend",
                      name, name),
          "extend target is a type constructor, not a type",
          std::format(
              "Give the block its own parameter and pass it along — "
              "`extend[T] {}[T]:` — to add methods to every `{}`, or name one "
              "instantiation, as in `extend {}[int32]:`.",
              name, name, name));
    }

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
    pop_type_params();
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

    const auto target_name = target.decl != nullptr ? target.decl->name
                             : !target.name.empty()
                                 ? target.name
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
        emit_diag(diag);
        mark_error();
      }
    }

    // `requires` obligations: implementing this trait demands the others.
    // Constructor targets (`impl monad for option`) participate too — their
    // coherence records are keyed by constructor name, which is exactly what
    // `type_has_trait` consults.
    if (trait.requires_bound.has_value() &&
        (target.decl != nullptr || target.kind == type_kind::ctor_ref_kind)) {
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
        param_scope.emplace(
            param.name,
            types_.type_param(param.name, param.higher_kinded_arity));
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

  /// Checks a `signature` declaration for well-formedness: each abstract
  /// `type` member is bound as an opaque type parameter (so later members may
  /// reference it, e.g. `def connect(url: str) -> conn`), then every `def`
  /// member's signature and every required `static`'s type is resolved. A
  /// signature is never a type and never enters `type_table`; it only
  /// describes the shape a module must have to satisfy it
  /// (`spec/module-values-design.md` §3).
  auto check_signature_decl(const ast::signature_decl &decl) -> void {
    auto param_scope = std::unordered_map<std::string, type_id>{};
    for (const auto &item : decl.items) {
      if (item == nullptr || item->has_error ||
          item->kind != ast::node_kind::associated_type_decl_node) {
        continue;
      }
      const auto &assoc =
          dynamic_cast<const ast::associated_type_decl_node &>(*item);
      if (!assoc.value.name.empty()) {
        param_scope.emplace(assoc.value.name,
                            types_.type_param(assoc.value.name));
      }
    }
    type_params_.push_back(std::move(param_scope));

    for (const auto &item : decl.items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      if (item->kind == ast::node_kind::func_decl) {
        check_function(dynamic_cast<const ast::func_decl &>(*item),
                       /*at_module_scope=*/false);
      } else if (item->kind == ast::node_kind::static_decl) {
        const auto &stat = dynamic_cast<const ast::static_decl &>(*item);
        if (stat.type_annotation != nullptr) {
          resolve_type(*stat.type_annotation, current_resolve_ctx());
        }
      }
    }

    pop_type_params();
  }

  /// Resolves the `signature` a module-parameter bound names, or `nullptr` if
  /// the bound is not a single-signature bound. `DB: backend` stores its bound
  /// as a `bound_type` wrapping one term whose `named_type` path names the
  /// signature.
  [[nodiscard]] auto
  resolve_bound_signature(const ast::type_expr &bound_expr) const
      -> const ast::signature_decl * {
    const ast::named_type *named = nullptr;
    if (const auto *bt = dynamic_cast<const ast::bound_type *>(&bound_expr)) {
      if (bt->value.terms.size() == 1 && bt->value.terms.front().type) {
        named = dynamic_cast<const ast::named_type *>(
            bt->value.terms.front().type.get());
      }
    } else if (const auto *nt =
                   dynamic_cast<const ast::named_type *>(&bound_expr)) {
      named = nt;
    }
    if (named == nullptr || named->path.empty()) {
      return nullptr;
    }
    const auto &name = named->path.back();
    // A signature named in a bound is looked up in the current module first,
    // then anywhere in the session (imports make it visible unqualified).
    if (module_ != nullptr) {
      if (const auto it = module_->signatures.find(name);
          it != module_->signatures.end()) {
        return it->second.decl;
      }
    }
    for (const auto &[mod_name, members] : index_.modules) {
      if (const auto it = members.signatures.find(name);
          it != members.signatures.end()) {
        return it->second.decl;
      }
    }
    return nullptr;
  }

  /// A resolved functor: its declaration/file and the module that owns it.
  struct resolved_functor {
    const functor_decl_ref *ref = nullptr;
    std::string owner_module; ///< Qualified name of the owning module.
  };

  /// Resolves the functor a `use m[args]` path names, or `nullopt` if no
  /// parameterized module by that name is in scope.
  [[nodiscard]] auto resolve_functor(const std::vector<std::string> &path) const
      -> std::optional<resolved_functor> {
    if (path.empty()) {
      return std::nullopt;
    }
    const auto &name = path.back();
    if (path.size() >= 2) {
      const auto owner_path =
          std::vector<std::string>(path.begin(), path.end() - 1);
      if (const auto *owner = find_session_module_of_path(owner_path)) {
        if (const auto it = owner->functors.find(name);
            it != owner->functors.end()) {
          return resolved_functor{.ref = &it->second,
                                  .owner_module = owner->module_name};
        }
      }
    }
    if (module_ != nullptr) {
      if (const auto it = module_->functors.find(name);
          it != module_->functors.end()) {
        return resolved_functor{.ref = &it->second,
                                .owner_module = module_->module_name};
      }
    }
    for (const auto &[mod_name, members] : index_.modules) {
      if (const auto it = members.functors.find(name);
          it != members.functors.end()) {
        return resolved_functor{.ref = &it->second,
                                .owner_module = members.module_name};
      }
    }
    return std::nullopt;
  }

  /// Resolves a module named by a functor argument (a `named_type` path),
  /// trying the path as written, then relative to the current module.
  [[nodiscard]] auto resolve_arg_module(const ast::named_type &nt) const
      -> const module_members * {
    if (nt.path.empty()) {
      return nullptr;
    }
    const auto joined = join_strings(nt.path, ".");
    if (const auto *m = index_.find_module(joined)) {
      return m;
    }
    if (!module_name_.empty()) {
      if (const auto *m =
              index_.find_module(append_module_name(module_name_, joined))) {
        return m;
      }
    }
    // An imported module: `use pkg.postgres` binds `postgres` to that module.
    if (const auto *binding = find_import(joined)) {
      if (const auto *m =
              index_.find_module(join_strings(binding->path, "."))) {
        return m;
      }
    }
    return nullptr;
  }

  /// Whether two resolved types are compatible for structural satisfaction:
  /// equal by interned id, or one is `k_unknown`/`k_error` so we don't cascade
  /// a knowledge gap into a spurious mismatch.
  [[nodiscard]] auto types_satisfy(type_id want, type_id got) const -> bool {
    if (want == k_unknown_type || got == k_unknown_type ||
        want == k_error_type || got == k_error_type) {
      return true;
    }
    return want == got;
  }

  /// Structural signature satisfaction: whether `mod` provides every member
  /// `sig` requires, each `pub`. Collects *every* failure into `failures`
  /// (concept-quality diagnostics list them all) and returns whether the set
  /// is empty. Checks member existence, visibility, function arity, and — under
  /// the abstract-type binding — deep parameter/return-type equality
  /// (`spec/module-values-design.md` §3). Each abstract signature type
  /// (`type conn`) binds to the module's concrete same-named type simply by
  /// resolving the signature's types in the module's own resolve context, where
  /// `conn` names the module's `pub type conn`.
  [[nodiscard]] auto
  module_satisfies_signature(const module_members &mod,
                             const ast::signature_decl &sig,
                             std::vector<std::string> &failures) -> bool {
    // Resolve a signature type expression against the argument module, quietly
    // (the module and signature were already checked at their own sites, so no
    // fresh diagnostics belong here). Abstract types resolve to the module's
    // concrete same-named types by virtue of the module context.
    const auto resolve_in_module = [&](const ast::type_expr &t) -> type_id {
      return resolve_type(t, resolve_ctx{.module = &mod,
                                         .param_bindings = nullptr,
                                         .use_type_param_stack = false,
                                         .quiet = true});
    };
    for (const auto &item : sig.items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      switch (item->kind) {
      case ast::node_kind::associated_type_decl_node: {
        const auto &assoc =
            dynamic_cast<const ast::associated_type_decl_node &>(*item);
        const auto it = mod.types.find(assoc.value.name);
        if (it == mod.types.end()) {
          failures.push_back(
              std::format("missing type `{}`", assoc.value.name));
        } else if (it->second.decl->visibility != ast::visibility::pub) {
          failures.push_back(std::format("type `{}` is provided but not `pub`",
                                         assoc.value.name));
        }
        break;
      }
      case ast::node_kind::func_decl: {
        const auto &fn = dynamic_cast<const ast::func_decl &>(*item);
        const auto it = mod.functions.find(fn.name);
        if (it == mod.functions.end()) {
          failures.push_back(std::format("missing function `{}`", fn.name));
        } else if (it->second.decl->visibility != ast::visibility::pub) {
          failures.push_back(
              std::format("function `{}` is provided but not `pub`", fn.name));
        } else if (it->second.decl->params.size() != fn.params.size()) {
          failures.push_back(std::format(
              "function `{}` takes {} parameter(s), but the signature requires "
              "{}",
              fn.name, it->second.decl->params.size(), fn.params.size()));
        } else {
          // Same arity and `pub`: check each parameter and the return type
          // match under the abstract-type binding.
          const auto &have = *it->second.decl;
          for (size_t p = 0; p < fn.params.size(); ++p) {
            if (fn.params[p].type_annotation == nullptr ||
                have.params[p].type_annotation == nullptr) {
              continue;
            }
            const auto want = resolve_in_module(*fn.params[p].type_annotation);
            const auto got = resolve_in_module(*have.params[p].type_annotation);
            if (!types_satisfy(want, got)) {
              failures.push_back(std::format(
                  "function `{}` parameter {} has type `{}`, but the signature "
                  "requires `{}`",
                  fn.name, p + 1, types_.display(got), types_.display(want)));
            }
          }
          if (fn.return_type != nullptr && have.return_type != nullptr) {
            const auto want = resolve_in_module(*fn.return_type);
            const auto got = resolve_in_module(*have.return_type);
            if (!types_satisfy(want, got)) {
              failures.push_back(std::format(
                  "function `{}` returns `{}`, but the signature requires `{}`",
                  fn.name, types_.display(got), types_.display(want)));
            }
          }
        }
        break;
      }
      case ast::node_kind::static_decl: {
        const auto &stat = dynamic_cast<const ast::static_decl &>(*item);
        const auto it = mod.statics.find(stat.name);
        if (it == mod.statics.end()) {
          failures.push_back(
              std::format("missing constant `static {}`", stat.name));
        } else if (it->second.decl->visibility != ast::visibility::pub) {
          failures.push_back(std::format(
              "constant `{}` is provided but not `pub`", stat.name));
        } else if (stat.type_annotation != nullptr &&
                   it->second.decl->type_annotation != nullptr) {
          const auto want = resolve_in_module(*stat.type_annotation);
          const auto got = resolve_in_module(*it->second.decl->type_annotation);
          if (!types_satisfy(want, got)) {
            failures.push_back(std::format(
                "constant `{}` has type `{}`, but the signature requires `{}`",
                stat.name, types_.display(got), types_.display(want)));
          }
        }
        break;
      }
      default:
        break;
      }
    }
    return failures.empty();
  }

  /// Validates a functor instantiation `use m[args]`: resolves the functor,
  /// checks argument arity, and — for each module parameter bounded by a
  /// signature — checks its argument module `satisfies` the bound with
  /// concept-quality diagnostics. Materializing the instantiated module (so
  /// its members become usable through the alias) is the next phase; until
  /// then a satisfied instantiation reports that its elaboration is pending.
  auto check_functor_instantiation(const ast::use_decl &decl) -> void {
    const auto functor = resolve_functor(decl.path);
    if (!functor.has_value()) {
      auto diag = diagnostic(
          diagnostic_level::error,
          std::format("no parameterized module named `{}` is in scope",
                      join_strings(decl.path, ".")),
          file_id_);
      diag.with_label(decl.span, "instantiated here");
      diag.with_help(
          "`use path[args]` instantiates a parameterized module (a `module "
          "name[...]:` declaration). Check the name and that the module is "
          "imported.");
      emit_diag(diag);
      mark_file_error();
      return;
    }

    const auto &fdecl = *functor->ref->decl;
    if (decl.instantiation_args.size() != fdecl.type_params.size()) {
      auto diag = diagnostic(
          diagnostic_level::error,
          std::format("parameterized module `{}` takes {} argument(s), but {} "
                      "were supplied",
                      fdecl.name, fdecl.type_params.size(),
                      decl.instantiation_args.size()),
          file_id_);
      diag.with_label(decl.span, "in this instantiation");
      emit_diag(diag);
      mark_file_error();
      return;
    }

    // Resolve each module parameter's argument and check satisfaction. Each
    // binding maps the parameter name to the argument module, which becomes an
    // import alias inside the instantiated body.
    std::vector<functor_arg_binding> module_bindings;
    std::vector<std::string> arg_key_parts;
    bool all_ok = true;
    for (size_t i = 0; i < fdecl.type_params.size(); ++i) {
      const auto &param = fdecl.type_params[i];
      const auto &arg = decl.instantiation_args[i];
      if (param.bound_or_type == nullptr) {
        arg_key_parts.emplace_back("?");
        continue; // an unbounded type parameter: no signature obligation
      }
      // `DB: backend` is syntactically indistinguishable from a value
      // parameter (`n: usize`) — the parser cannot tell a signature bound from
      // a value type. It is a module parameter exactly when its bound names a
      // signature, so resolution (not `is_value_param`) is what decides.
      const auto *sig = resolve_bound_signature(*param.bound_or_type);
      if (sig == nullptr) {
        arg_key_parts.emplace_back("?");
        continue; // an ordinary value/type/trait/concept parameter
      }
      const auto *arg_named =
          arg.value != nullptr
              ? dynamic_cast<const ast::named_type *>(arg.value.get())
              : nullptr;
      const module_members *arg_mod =
          arg_named != nullptr ? resolve_arg_module(*arg_named) : nullptr;
      if (arg_mod == nullptr) {
        auto diag = diagnostic(
            diagnostic_level::error,
            std::format("argument for module parameter `{}` must be a module "
                        "satisfying signature `{}`",
                        param.name, sig->name),
            file_id_);
        diag.with_label(arg.span, "not a module");
        emit_diag(diag);
        all_ok = false;
        arg_key_parts.emplace_back("?");
        continue;
      }
      std::vector<std::string> failures;
      if (!module_satisfies_signature(*arg_mod, *sig, failures)) {
        auto diag = diagnostic(
            diagnostic_level::error,
            std::format("module `{}` does not satisfy signature `{}`",
                        arg_mod->module_name, sig->name),
            file_id_);
        diag.with_label(arg.span, "used here");
        for (const auto &failure : failures) {
          diag.children.push_back(
              diagnostic(diagnostic_level::note, failure, file_id_));
        }
        diag.with_help(std::format(
            "A module satisfies `{}` structurally: it must provide each "
            "required `type`, `def`, and `static` as a `pub` member. Add or "
            "expose the members listed above.",
            sig->name));
        emit_diag(diag);
        all_ok = false;
        arg_key_parts.emplace_back("?");
        continue;
      }
      module_bindings.push_back(
          {.param_name = param.name, .arg_module_name = arg_mod->module_name});
      arg_key_parts.push_back(arg_mod->module_name);
    }

    if (!all_ok) {
      mark_file_error();
      return;
    }

    // Canonical instantiation key: functor's fully-qualified name plus each
    // argument's canonical module name. Memoized session-wide so two imports
    // of `audited[postgres]` share one module identity (applicative functor).
    const auto instantiation_key = std::format(
        "{}[{}]", append_module_name(functor->owner_module, fdecl.name),
        join_strings(arg_key_parts, ","));

    const auto alias_name = functor_alias_name(decl, fdecl.name);

    auto synth_it = functor_instances_.find(instantiation_key);
    if (synth_it == functor_instances_.end()) {
      const auto synth_name = materialize_functor(
          fdecl, *functor, module_bindings, instantiation_key, decl.span);
      if (!synth_name.has_value()) {
        // A body construct beyond v1 materialization; a diagnostic was already
        // emitted by `materialize_functor`.
        mark_file_error();
        return;
      }
      synth_it =
          functor_instances_.emplace(instantiation_key, *synth_name).first;
    }

    // Bind the alias in the importing file to the instantiated module so
    // `db.query(...)` resolves to its members.
    index_.imports[file_id_].push_back(import_binding{
        .local_name = alias_name,
        .path = split_module_name(synth_it->second),
        .leaf_name = {},
        .is_wildcard = false,
        .span = decl.span,
    });
  }

  /// The local name a functor instantiation is bound to: the `as` alias if
  /// present, else the functor's own name.
  [[nodiscard]] static auto functor_alias_name(const ast::use_decl &decl,
                                               const std::string &functor_name)
      -> std::string {
    if (decl.selector.has_value() &&
        decl.selector->kind == ast::use_selector_kind::single &&
        !decl.selector->items.empty() &&
        decl.selector->items.front().alias.has_value()) {
      return *decl.selector->items.front().alias;
    }
    return functor_name;
  }

  /// Turns an instantiation key into a dotted-path-free module name usable as
  /// a `program_index` key and in `import_binding::path` (which is split/
  /// joined on `.`). Non-identifier characters become `_`.
  [[nodiscard]] static auto sanitize_module_name(std::string_view key)
      -> std::string {
    std::string out;
    out.reserve(key.size());
    for (const char c : key) {
      out.push_back((std::isalnum(static_cast<unsigned char>(c)) != 0) ? c
                                                                       : '_');
    }
    return out;
  }

  /// The bare target-type name of an `impl`/`extend` block's `for` type
  /// (`impl show for handle` → `handle`), used both as the coherence/method-
  /// table key and as the `functor_instance::impl_target` HIR lowering name.
  [[nodiscard]] static auto impl_target_name(const ast::type_expr *for_type)
      -> std::string {
    if (const auto *named = dynamic_cast<const ast::named_type *>(for_type);
        named != nullptr && !named->path.empty()) {
      return named->path.back();
    }
    return {};
  }

  /// Materializes a functor instantiation: clones each supported member of the
  /// body (giving the instance distinct node identity for per-instantiation
  /// type checking) and registers them under a fresh synthetic module so its
  /// `impl`/`extend` members join the method table and coherence check. Body
  /// *checking* is deferred (a `pending_functor_body` is recorded) until after
  /// `build_method_table`/`validate_impl_coherence`. Returns the synthetic
  /// module name, or `nullopt` if the body uses a construct materialization
  /// does not support (a diagnostic is emitted). See
  /// `spec/module-values-design.md` §4.
  [[nodiscard]] auto
  materialize_functor(const ast::sub_module_decl &fdecl,
                      const resolved_functor &functor,
                      const std::vector<functor_arg_binding> &module_bindings,
                      const std::string &instantiation_key,
                      source_span use_span) -> std::optional<std::string> {
    auto synth_name = sanitize_module_name(instantiation_key);

    // Clone each member of the functor body. Materialization covers `def`,
    // `type`, `static` (binding), `impl`, `extend`, and `trait` members;
    // anything else (a non-binding `static` form, an inline submodule) falls
    // back with a clear diagnostic rather than a partial module. A clone gives
    // each instantiation distinct node identity so `node_types_` and
    // diagnostics stay per-instantiation, while projections through a module
    // parameter (`DB.conn`, `DB.query(...)`) resolve via the import aliases
    // bound at check time.
    const auto reject_member = [&](source_span span, std::string_view what) {
      auto diag =
          diagnostic(diagnostic_level::error,
                     std::format("instantiating `{}` is not supported yet: {}",
                                 fdecl.name, what),
                     file_id_);
      diag.with_label(use_span, "in this instantiation");
      diag.with_label(span, "unsupported functor-body member");
      diag.with_help(
          "materialization of a parameterized module supports `def`, `type`, "
          "`static` (binding), `impl`, `extend`, and `trait` members.");
      emit_diag(diag);
    };

    std::vector<const ast::func_decl *> cloned_funcs;
    std::vector<const ast::type_decl *> cloned_types;
    std::vector<const ast::static_decl *> cloned_statics;
    std::vector<const ast::impl_decl *> cloned_impls;
    std::vector<const ast::extend_decl *> cloned_extends;
    std::vector<const ast::trait_decl *> cloned_traits;
    for (const auto &item : fdecl.items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      switch (item->kind) {
      case ast::node_kind::use_decl:
      case ast::node_kind::module_decl:
        continue;
      case ast::node_kind::func_decl: {
        const auto &fn = dynamic_cast<const ast::func_decl &>(*item);
        auto cloned = ast::clone_func_decl(fn);
        if (!cloned.has_value()) {
          reject_member(fn.span, cloned.error().message);
          return std::nullopt;
        }
        auto *raw = cloned->get();
        synthetic_nodes_.push_back(ast::ptr<ast::node>(std::move(*cloned)));
        cloned_funcs.push_back(raw);
        break;
      }
      case ast::node_kind::type_decl: {
        const auto &td = dynamic_cast<const ast::type_decl &>(*item);
        auto cloned = ast::clone_type_decl(td);
        if (!cloned.has_value()) {
          reject_member(td.span, cloned.error().message);
          return std::nullopt;
        }
        auto *raw = cloned->get();
        synthetic_nodes_.push_back(ast::ptr<ast::node>(std::move(*cloned)));
        cloned_types.push_back(raw);
        break;
      }
      case ast::node_kind::static_decl: {
        const auto &sd = dynamic_cast<const ast::static_decl &>(*item);
        if (sd.decl_kind != ast::static_decl_kind::binding) {
          reject_member(sd.span,
                        "its body contains a non-binding `static` form");
          return std::nullopt;
        }
        auto cloned = ast::clone_static_decl(sd);
        if (!cloned.has_value()) {
          reject_member(sd.span, cloned.error().message);
          return std::nullopt;
        }
        auto *raw = cloned->get();
        synthetic_nodes_.push_back(ast::ptr<ast::node>(std::move(*cloned)));
        cloned_statics.push_back(raw);
        break;
      }
      case ast::node_kind::impl_decl: {
        const auto &id = dynamic_cast<const ast::impl_decl &>(*item);
        auto cloned = ast::clone_impl_decl(id);
        if (!cloned.has_value()) {
          reject_member(id.span, cloned.error().message);
          return std::nullopt;
        }
        auto *raw = cloned->get();
        synthetic_nodes_.push_back(ast::ptr<ast::node>(std::move(*cloned)));
        cloned_impls.push_back(raw);
        break;
      }
      case ast::node_kind::extend_decl: {
        const auto &ed = dynamic_cast<const ast::extend_decl &>(*item);
        auto cloned = ast::clone_extend_decl(ed);
        if (!cloned.has_value()) {
          reject_member(ed.span, cloned.error().message);
          return std::nullopt;
        }
        auto *raw = cloned->get();
        synthetic_nodes_.push_back(ast::ptr<ast::node>(std::move(*cloned)));
        cloned_extends.push_back(raw);
        break;
      }
      case ast::node_kind::trait_decl: {
        const auto &tr = dynamic_cast<const ast::trait_decl &>(*item);
        auto cloned = ast::clone_trait_decl(tr);
        if (!cloned.has_value()) {
          reject_member(tr.span, cloned.error().message);
          return std::nullopt;
        }
        auto *raw = cloned->get();
        synthetic_nodes_.push_back(ast::ptr<ast::node>(std::move(*cloned)));
        cloned_traits.push_back(raw);
        break;
      }
      default:
        reject_member(item->span,
                      "its body contains a member that is not a `def`, "
                      "`type`, `static`, `impl`, `extend`, or `trait`");
        return std::nullopt;
      }
    }

    // Register the synthetic module and its members in the program index.
    // Types, traits, and statics are registered before functions so a
    // function's return type (`-> row`), a static reference, or a trait bound
    // resolves against them.
    auto &synth = index_.modules[synth_name];
    synth.module_name = synth_name;
    for (const auto *tr : cloned_traits) {
      synth.traits.insert_or_assign(
          tr->name,
          trait_decl_ref{.decl = tr, .file_id = functor.ref->file_id});
    }
    for (const auto *td : cloned_types) {
      synth.types.insert_or_assign(
          td->name, type_decl_ref{.decl = td, .file_id = functor.ref->file_id});
      if (td->definition != nullptr &&
          td->definition->kind == ast::node_kind::sum_type_def) {
        const auto &sum =
            dynamic_cast<const ast::sum_type_def &>(*td->definition);
        for (const auto &variant : sum.body.variants) {
          if (!variant.name.empty()) {
            synth.variants.insert_or_assign(
                variant.name, variant_ref{.sum_decl = td, .variant = &variant});
          }
        }
      }
    }
    for (const auto *sd : cloned_statics) {
      synth.statics.insert_or_assign(
          sd->name,
          static_decl_ref{.decl = sd, .file_id = functor.ref->file_id});
    }
    for (const auto *fn : cloned_funcs) {
      synth.functions.insert_or_assign(
          fn->name, func_decl_ref{.decl = fn, .file_id = functor.ref->file_id});
      // Record the clone for HIR lowering: `hir::lower_functor_modules` builds
      // a standalone `hir_module` named `synth_name` from every clone tagged
      // with it, so a `db.f(...)` call site's cross-module dispatch finds it.
      // Only `def`s lower to runtime code; `type` members are compile-time and
      // a scalar `static` is inlined at each reference (`static_const_values`).
      functor_instance_decls_.push_back(
          functor_instance{.decl = fn, .owner_module = synth_name});
    }
    // Register `impl`/`extend` blocks so `build_method_table` and
    // `validate_impl_coherence` (which run right after this pass) see them, and
    // record each method as an `impl_target::method` HIR instance so it lowers
    // to runtime code within the synthetic module.
    for (const auto *id : cloned_impls) {
      synth.impls.push_back(impl_ref{.decl = id,
                                     .module_name = synth_name,
                                     .file_id = functor.ref->file_id});
      const auto target = impl_target_name(id->for_type.get());
      for (const auto &member : id->items) {
        if (member != nullptr && member->kind == ast::node_kind::func_decl) {
          functor_instance_decls_.push_back(functor_instance{
              .decl = &dynamic_cast<const ast::func_decl &>(*member),
              .owner_module = synth_name,
              .impl_target = target});
        }
      }
    }
    for (const auto *ed : cloned_extends) {
      synth.extends.push_back(extend_ref{.decl = ed,
                                         .module_name = synth_name,
                                         .file_id = functor.ref->file_id});
      const auto target = impl_target_name(ed->for_type.get());
      for (const auto &member : ed->items) {
        if (member != nullptr && member->kind == ast::node_kind::func_decl) {
          functor_instance_decls_.push_back(functor_instance{
              .decl = &dynamic_cast<const ast::func_decl &>(*member),
              .owner_module = synth_name,
              .impl_target = target});
        }
      }
    }

    // Defer body checking until after the method table and coherence build:
    // a method call inside the body must resolve against a complete table.
    pending_functor_bodies_.push_back(
        pending_functor_body{.synth_name = synth_name,
                             .owner_module = functor.owner_module,
                             .functor_file = functor.ref->file_id,
                             .module_bindings = module_bindings,
                             .types = std::move(cloned_types),
                             .statics = std::move(cloned_statics),
                             .funcs = std::move(cloned_funcs),
                             .impls = std::move(cloned_impls),
                             .extends = std::move(cloned_extends),
                             .traits = std::move(cloned_traits)});
    return synth_name;
  }

  /// Checks every deferred functor-instantiation body (recorded by
  /// `materialize_functor`), after the method table and coherence build. Each
  /// module parameter is temporarily bound as an import alias to its argument
  /// module in the functor's own file so the cloned bodies' `DB.conn` /
  /// `DB.query(...)` projections resolve; the binding is restored afterward.
  auto check_pending_functor_bodies() -> void {
    const auto *saved_module = module_;
    const auto saved_module_name = module_name_;
    const auto saved_file_id = file_id_;
    for (const auto &pending : pending_functor_bodies_) {
      const auto synth_it = index_.modules.find(pending.synth_name);
      if (synth_it == index_.modules.end()) {
        continue;
      }
      auto *synth = &synth_it->second;
      auto &functor_imports = index_.imports[pending.functor_file];
      const auto saved_import_count = functor_imports.size();
      // Give the cloned body its original lexical scope: a `use owner.*`
      // wildcard so a name declared alongside the functor (a sibling `trait`,
      // `type`, ...) resolves while checking the instantiation in its own
      // synthetic module. Then bind each module parameter as an import alias
      // to its argument module so `DB.conn` / `DB.query(...)` projections
      // resolve. Both are restored afterward.
      if (!pending.owner_module.empty()) {
        functor_imports.push_back(
            import_binding{.local_name = {},
                           .path = split_module_name(pending.owner_module),
                           .leaf_name = {},
                           .is_wildcard = true,
                           .span = source_span::dummy()});
      }
      for (const auto &binding : pending.module_bindings) {
        functor_imports.push_back(import_binding{
            .local_name = binding.param_name,
            .path = split_module_name(binding.arg_module_name),
            .leaf_name = {},
            .is_wildcard = false,
            .span = source_span::dummy(),
        });
      }

      module_ = synth;
      module_name_ = pending.synth_name;
      file_id_ = pending.functor_file;
      for (const auto *tr : pending.traits) {
        check_trait_decl(*tr);
      }
      for (const auto *td : pending.types) {
        check_type_decl(*td);
      }
      for (const auto *sd : pending.statics) {
        check_static_decl(*sd);
      }
      for (const auto *fn : pending.funcs) {
        check_function(*fn, /*at_module_scope=*/true);
      }
      for (const auto *id : pending.impls) {
        check_impl_decl(*id);
      }
      for (const auto *ed : pending.extends) {
        check_extend_decl(*ed);
      }

      functor_imports.resize(saved_import_count);
    }
    module_ = saved_module;
    module_name_ = saved_module_name;
    file_id_ = saved_file_id;
  }

  /// Marks the file currently being checked as containing an error, guarding
  /// the index bound.
  auto mark_file_error() -> void {
    if (static_cast<size_t>(file_id_) < file_has_errors_.size()) {
      file_has_errors_[file_id_] = true;
    }
  }

  /// Checks a `static` declaration in whichever of its five forms it takes
  /// (binding, assertion, conditional compilation, or either `static for`
  /// variant), dispatching on `decl_kind`.
  /// Type-checks a `static` declaration, then — for `binding`/`assertion`/
  /// `conditional_compilation` — actually evaluates it via `comptime_eval_`
  /// (the M2 milestone of the compile-time evaluation subsystem). Only the
  /// scalar-arithmetic subset `comptime::evaluator` currently supports
  /// evaluates for real; anything wider (a name it doesn't recognize, a
  /// forward reference to a `static let` not yet checked in file order, a
  /// call) reports its own "not yet supported"/"not a compile-time
  /// constant yet" diagnostic from within the evaluator and this function
  /// falls back to the old type-check-only behavior for that node so a
  /// missing evaluator feature degrades to a diagnostic rather than a
  /// crash. `static for` still only type-checks its body once against the
  /// element type — iteration is a later milestone.
  /// The `checker`-side half of `comptime::evaluator::variant_resolver_fn`
  /// (installed on `comptime_eval_` in the constructor): given a node the
  /// evaluator is about to evaluate, reports whether `infer_expr` already
  /// resolved it as a sum-type variant constructor (bare `@unix` parses to
  /// a plain `ident_expr` named `"unix"`, and `@other(s)` to a `call_expr`
  /// whose callee is an `ident_expr` named `"other"` — the parser drops the
  /// `@` entirely, so this is the only place that distinguishes a variant
  /// reference from an ordinary identifier/call). Requires `node` to already
  /// have an entry in `node_types_`, which every caller of
  /// `comptime_eval_.evaluate(...)` on a full expression tree (`static if`/
  /// `static assert` conditions, `static let` initializers) already
  /// guarantees by calling `infer_expr`/`require_bool` first.
  auto resolve_variant_tag(const ast::node &node)
      -> std::optional<std::pair<std::string, std::string>> {
    const auto it = node_types_.find(&node);
    if (it == node_types_.end()) {
      return std::nullopt;
    }
    const auto &entry = types_.entry(strip_refs(it->second));
    if (entry.kind != type_kind::sum_kind) {
      return std::nullopt;
    }
    if (const auto *ident = dynamic_cast<const ast::ident_expr *>(&node)) {
      return std::make_pair(entry.name, ident->name);
    }
    if (const auto *call = dynamic_cast<const ast::call_expr *>(&node)) {
      if (const auto *callee =
              dynamic_cast<const ast::ident_expr *>(call->callee.get())) {
        return std::make_pair(entry.name, callee->name);
      }
    }
    return std::nullopt;
  }

  auto check_static_decl(const ast::static_decl &decl) -> void {
    comptime_eval_.set_file(file_id_);
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
                        "from the annotation", decl.initializer.get());
        }
      }
      static_types_.insert_or_assign(
          &decl, decl.type_annotation != nullptr ? declared : found);
      // A forward reference from an earlier-checked `static let` (or an
      // ordinary expression that reached this name first — see
      // `resolve_ident`) may already have resolved this binding lazily;
      // `ensure_static_binding_evaluated` skips re-evaluating (and
      // potentially re-diagnosing) it in that case.
      ensure_static_binding_evaluated(decl);
      return;
    }
    case ast::static_decl_kind::assertion:
      if (decl.assert_condition != nullptr &&
          !decl.assert_condition->has_error) {
        require_bool(*decl.assert_condition, "a `static assert` condition");
        const auto evaluated = comptime_eval_.evaluate(*decl.assert_condition);
        if (!evaluated.is_error() &&
            evaluated.kind == comptime::value_kind::boolean &&
            !evaluated.is_true()) {
          auto diag = diagnostic(diagnostic_level::error,
                                 decl.assert_message.has_value()
                                     ? *decl.assert_message
                                     : "`static assert` condition was false",
                                 file_id_)
                          .with_label(decl.assert_condition->span,
                                      "this condition evaluated to `false`");
          emit_diag(diag);
        }
      }
      return;
    case ast::static_decl_kind::conditional_compilation: {
      auto condition_value = std::optional<comptime::value>{};
      if (decl.if_condition != nullptr && !decl.if_condition->has_error) {
        require_bool(*decl.if_condition, "a `static if` condition");
        auto evaluated = comptime_eval_.evaluate(*decl.if_condition);
        if (!evaluated.is_error() &&
            evaluated.kind == comptime::value_kind::boolean) {
          condition_value = std::move(evaluated);
        }
      }
      // When the condition evaluated to a real boolean, only the taken
      // branch is checked — this is real branch selection, not just
      // type-checking both sides. When evaluation couldn't determine a
      // value (unsupported construct, forward reference, ...), fall back
      // to checking both branches so users still get diagnostics for
      // whichever branch has real problems.
      if (condition_value.has_value()) {
        check_body_nodes(condition_value->is_true() ? decl.if_body
                                                    : decl.else_body,
                         k_unknown_type);
      } else {
        check_body_nodes(decl.if_body, k_unknown_type);
        check_body_nodes(decl.else_body, k_unknown_type);
      }
      return;
    }
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

      // Real compile-time iteration, separate from the type-check pass
      // above (which only checks the body/yield once against the element
      // type). Only a single loop pattern is supported for now — multiple
      // patterns (tuple-unpacking `for a, b in ...`) still only
      // type-check, matching the pre-existing fallback behavior. Also
      // skipped entirely while any generic type-parameter scope is active
      // (e.g. this `static for` sits inside a `static def derive_show[T]()`
      // body): the iterable may depend on `T`, which has no bound value
      // until an actual compile-time generic call (`derive_show[point]()`)
      // provides one — evaluating it now, during ordinary declaration
      // checking, would spuriously fail every time regardless of whether
      // the function is ever called. Real iteration for that case instead
      // happens inside `evaluator::evaluate_stmt`'s own `for_block`/
      // `for_inline` handling, run once per call with `T` actually bound.
      const auto inside_generic_scope =
          std::ranges::any_of(type_params_, [](const auto &scope) -> bool {
            return !scope.empty();
          });
      if (!inside_generic_scope && decl.for_iterable != nullptr &&
          !decl.for_iterable->has_error && decl.for_patterns.size() == 1 &&
          decl.for_patterns[0] != nullptr) {
        auto list_value = comptime_eval_.evaluate_iterable(*decl.for_iterable);
        if (!list_value.is_error()) {
          for (const auto &element_value : list_value.elements) {
            auto scope = std::unordered_map<std::string, comptime::value>{};
            if (!comptime_eval_.bind_pattern(*decl.for_patterns[0],
                                             element_value, scope)) {
              break;
            }
            comptime_eval_.push_locals(std::move(scope));
            if (decl.for_guard != nullptr) {
              auto guard = comptime_eval_.evaluate(*decl.for_guard);
              if (guard.is_error() ||
                  guard.kind != comptime::value_kind::boolean) {
                comptime_eval_.pop_locals();
                break;
              }
              if (!guard.boolean) {
                comptime_eval_.pop_locals();
                continue;
              }
            }
            if (decl.decl_kind == ast::static_decl_kind::for_inline) {
              if (decl.for_yield != nullptr) {
                // Evaluated for its compile-time diagnostics/effects; the
                // resulting sequence has no consumer yet — reifying it
                // into generated code is splice reification (M4).
                (void)comptime_eval_.evaluate(*decl.for_yield);
              }
            } else {
              const auto exec = comptime_eval_.evaluate_stmts(decl.for_body);
              if (exec.errored) {
                comptime_eval_.pop_locals();
                break;
              }
            }
            comptime_eval_.pop_locals();
          }
        }
      }
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
    case ast::node_kind::signature_decl:
      check_signature_decl(dynamic_cast<const ast::signature_decl &>(item));
      return;
    case ast::node_kind::static_decl:
      check_static_decl(dynamic_cast<const ast::static_decl &>(item));
      return;
    case ast::node_kind::sub_module_decl: {
      const auto &decl = dynamic_cast<const ast::sub_module_decl &>(item);
      if (decl.is_functor()) {
        // A functor's body is not elaborated as written — it is type-checked
        // per instantiation, after substituting the module arguments (see
        // `spec/module-values-design.md` §4). Checking it here, with the
        // parameters unbound, would report spurious `DB.conn`-style errors.
        return;
      }
      if (decl.items.empty()) {
        return;
      }
      const auto saved_module_name = module_name_;
      const auto *saved_module = module_;
      module_name_ = append_module_name(module_name_, decl.name);
      module_ = index_.find_module(module_name_);
      push_scope();
      for (const auto &child : decl.items) {
        if (child == nullptr || child->has_error) {
          continue;
        }
        if (const auto it = item_splice_impls_.find(child.get());
            it != item_splice_impls_.end()) {
          check_item(*it->second, /*at_module_scope=*/true);
          continue;
        }
        if (const auto *expr = dynamic_cast<const ast::expr *>(child.get());
            expr != nullptr ||
            dynamic_cast<const ast::stmt *>(child.get()) != nullptr) {
          check_body_node(*child, k_unknown_type);
        } else {
          check_item(*child, /*at_module_scope=*/true);
        }
        if (const auto it = derived_trait_impls_.find(child.get());
            it != derived_trait_impls_.end()) {
          for (const auto *impl : it->second) {
            check_item(*impl, /*at_module_scope=*/true);
          }
        }
      }
      pop_scope();
      module_name_ = saved_module_name;
      module_ = saved_module;
      return;
    }
    case ast::node_kind::use_decl:
    // Functor instantiations (`use m[args]`) are materialized in a pre-pass
    // (`discover_functor_instantiations`) before the method-table/coherence
    // build, not here — see `run_impl`. An ordinary `use`, a `dep`, and a
    // `module` header all need no per-item checking.
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
        const auto [it, inserted] = impl_trait_index_.emplace(
            key, recorded_impl{.location = location, .target = target});
        if (inserted) {
          continue;
        }
        if (!impl_targets_overlap(it->second.target, target)) {
          // Two impls for two different instantiations of one generic
          // declaration. Legal, and the key stays pointing at the first —
          // `type_has_trait` only asks whether *some* impl exists.
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
                                   trait_name,
                                   types_.display(it->second.target)),
                       it->second.location.file_id)
                .with_label(it->second.location.span,
                            "previous implementation"));
        duplicate.with_help(
            "A program contains at most one implementation of a trait for a "
            "type; remove or merge one of these.");
        emit_diag(duplicate);
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
      } else if (item_splice_impls_.contains(item.get())) {
        // Resolved to an injected `impl` block by `resolve_item_splices` —
        // an item, not a top-level script statement.
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
      emit_diag(diag);
      mark_error();
    }

    push_scope();
    for (const auto &item : file.items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      if (const auto it = item_splice_impls_.find(item.get());
          it != item_splice_impls_.end()) {
        // Already resolved by `resolve_item_splices` (before method-table/
        // coherence build) — check the injected impl itself, in place of
        // the splice node. Do not fall through to `check_body_node` below:
        // `splice_stmt` derives from `stmt`, so it would otherwise match
        // that branch and re-evaluate the operand a second time.
        check_item(*it->second, /*at_module_scope=*/true);
        continue;
      }
      if (dynamic_cast<const ast::stmt *>(item.get()) != nullptr ||
          dynamic_cast<const ast::expr *>(item.get()) != nullptr) {
        check_body_node(*item, k_unknown_type);
      } else {
        check_item(*item, /*at_module_scope=*/true);
      }
      if (const auto it = derived_trait_impls_.find(item.get());
          it != derived_trait_impls_.end()) {
        for (const auto *impl : it->second) {
          check_item(*impl, /*at_module_scope=*/true);
        }
      }
    }
    pop_scope();
  }

  /// Recursively registers every top-level `static let` binding and
  /// `static def` function found in `items` (descending into inline
  /// submodules) with `comptime_eval_`, so cross-file/forward references
  /// resolve lazily regardless of file-checking order — see the design
  /// plan's confluence requirement.
  auto register_comptime_globals(const std::vector<ast::ptr<ast::node>> &items,
                                 file_id_type owner_file) -> void {
    for (const auto &item : items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      if (item->kind == ast::node_kind::static_decl) {
        const auto &decl = dynamic_cast<const ast::static_decl &>(*item);
        if (decl.decl_kind == ast::static_decl_kind::binding &&
            decl.initializer != nullptr && !decl.name.empty()) {
          comptime_eval_.register_pending_static(decl.name, *decl.initializer,
                                                 owner_file);
        }
      } else if (item->kind == ast::node_kind::func_decl) {
        const auto &fn = dynamic_cast<const ast::func_decl &>(*item);
        if (fn.modifiers.is_static && !fn.name.empty()) {
          comptime_eval_.register_pending_function(fn.name, fn);
        }
      } else if (item->kind == ast::node_kind::type_decl) {
        // Every `type` declaration is registered, unconditionally (Kira
        // types have no "static" modifier to gate on — they're always
        // compile-time-known) — gives `comptime::evaluator::eval_call`'s
        // reflection intrinsics (`T.fields()`/etc., `reflect.cpp`) their
        // own session-wide name -> declaration table, the same
        // "confluent, order-independent" shape `register_pending_static`/
        // `register_pending_function` already use for `static let`/
        // `static def`.
        const auto &decl = dynamic_cast<const ast::type_decl &>(*item);
        if (!decl.name.empty()) {
          comptime_eval_.register_pending_type(decl.name, decl);
        }
      } else if (item->kind == ast::node_kind::sub_module_decl) {
        const auto &sub = dynamic_cast<const ast::sub_module_decl &>(*item);
        register_comptime_globals(sub.items, owner_file);
      }
    }
  }

  /// Whether `name` reflects a module — matching a module's fully-qualified
  /// name or its leaf segment, the two keys `register_comptime_modules`
  /// registers. Lets `infer_field_call` type `M.functions()` and hand it to
  /// compile-time evaluation instead of rejecting `M` as an undefined value.
  [[nodiscard]] auto names_reflectable_module(std::string_view name) const
      -> bool {
    return std::ranges::any_of(index_.modules, [&](const auto &entry) {
      return entry.first == name ||
             split_module_name(entry.first).back() == name;
    });
  }

  /// Registers every module's reflectable surface (its `def` and `type`
  /// members, each with visibility) with the compile-time evaluator so
  /// `M.functions()`/`M.types()`/`M.name()` reflection resolves. Iterates the
  /// finished `program_index`, so an instantiated functor's synthetic module
  /// is reflected exactly like a hand-written one (design §5).
  auto register_comptime_modules() -> void {
    for (const auto &[module_name, members] : index_.modules) {
      auto info = comptime::evaluator::module_reflection_info{};
      info.functions.reserve(members.functions.size());
      for (const auto &[name, ref] : members.functions) {
        info.functions.push_back(comptime::evaluator::module_member_info{
            .name = name,
            .is_pub = ref.decl != nullptr &&
                      ref.decl->visibility == ast::visibility::pub});
      }
      info.types.reserve(members.types.size());
      for (const auto &[name, ref] : members.types) {
        info.types.push_back(comptime::evaluator::module_member_info{
            .name = name,
            .is_pub = ref.decl != nullptr &&
                      ref.decl->visibility == ast::visibility::pub});
      }
      // Register under the fully-qualified name and, for ergonomic
      // reflection, the leaf name (`sample.math` is reachable as `math` when
      // a reflection reference names it unqualified). Full name registered
      // last so it wins any leaf/full collision.
      const auto leaf = split_module_name(module_name).back();
      if (leaf != module_name) {
        comptime_eval_.register_pending_module(leaf, info);
      }
      comptime_eval_.register_pending_module(module_name, std::move(info));
    }
  }

  /// Recursively resolves every item-level splice (`~expr` used directly
  /// among `items`, as opposed to inside a function body) into an injected
  /// `impl` block, descending into inline submodules the same way
  /// `register_comptime_globals` does. Must run after that pass (so
  /// `static def`/`static let` globals it references are registered) but
  /// before `build_method_table`/`validate_impl_coherence` — see `index_`'s
  /// doc comment for why running here, rather than during ordinary
  /// per-file checking, is what makes the injected impl's methods visible
  /// to coherence checking and to every other file's method lookups
  /// regardless of file-checking order.
  ///
  /// Only a `def_expr` fragment wrapping a single `impl_decl` is supported
  /// — item-level splice producing a `type`/`trait`/`def` is not
  /// implemented yet, and reports a clear diagnostic rather than silently
  /// doing nothing. Evaluates each splice's operand exactly once: the
  /// result (found or not) is recorded in `item_splice_impls_` so
  /// `check_file`'s item loop never falls through to re-evaluating it via
  /// `check_body_node`.
  /// Every `deriving`-able trait with a real `static def derive_<name>[T]()`
  /// in `std.derive` (`src/std/deriving.kira`) — the traits M7 actually
  /// re-derives for real, as opposed to leaving on the old, type-only
  /// `derived_method_result` path. `ord`/`hash` are deliberately not here:
  /// `ord`'s `cmp` return type (`ordering`) has no real variants or runtime
  /// representation anywhere in the language yet, and no builtin scalar
  /// implements `.hash()` or has a hash-combining primitive to fold field
  /// hashes with — both would need new language-level infrastructure, not
  /// just new derive logic, so they stay on the type-check-only fallback
  /// (`derived_method_result` still lists all five names for that purpose).
  static constexpr std::array<std::string_view, 3> k_real_derive_traits = {
      "show", "eq", "debug"};

  /// Splices `~derive_<trait>[TypeName]()` in behind the scenes, once per
  /// entry in `k_real_derive_traits` present in `decl.deriving`, for a
  /// concrete (non-generic), struct-shaped `type ... deriving <trait>` —
  /// M7's replacement for the old hardcoded `derived_method_result` type-
  /// only rule (each derived trait stays listed there too, purely for the
  /// `type_has_trait` bookkeeping every `deriving`d trait needs; the *real*
  /// runtime method now comes from here for `show`/`eq`/`debug`). Reuses
  /// `resolve_item_splices`'s own tail exactly (evaluate, extract the
  /// wrapped `impl_decl`, push into `index_.modules[...].impls`) via a
  /// synthesized call — there's no literal `splice_stmt` AST node for this
  /// sugar to key off, so nothing is recorded in `item_splice_impls_`
  /// (nothing will ever look one up for a plain `type_decl` item). A
  /// generic type or a non-struct (sum/refinement) definition is left
  /// entirely on the old type-only path unchanged: every `derive_<trait>
  /// [T]()` body only supports a concrete, struct-shaped `T` (`T.fields()`
  /// — see `reflect.cpp`), and the point of this milestone is a real,
  /// working derivation, not a diagnostic regression for the shapes it
  /// doesn't cover yet.
  auto resolve_deriving_traits(const ast::type_decl &decl,
                               file_id_type owner_file) -> void {
    if (decl.name.empty() || !decl.type_params.empty() ||
        decl.definition == nullptr ||
        decl.definition->kind != ast::node_kind::struct_type_def) {
      return;
    }
    for (const auto trait_name : k_real_derive_traits) {
      if (!std::ranges::contains(decl.deriving, std::string(trait_name))) {
        continue;
      }
      auto callee_ident = ast::make<ast::ident_expr>();
      callee_ident->span = decl.span;
      callee_ident->name = std::format("derive_{}", trait_name);
      auto type_ident = ast::make<ast::ident_expr>();
      type_ident->span = decl.span;
      type_ident->name = decl.name;
      auto index = ast::make<ast::index_expr>();
      index->span = decl.span;
      index->object = std::move(callee_ident);
      index->index = std::move(type_ident);
      auto call = ast::make<ast::call_expr>();
      call->span = decl.span;
      call->callee = std::move(index);

      const auto fragment_value = comptime_eval_.evaluate(*call);
      if (fragment_value.is_error() ||
          fragment_value.kind != comptime::value_kind::def_expr_fragment ||
          fragment_value.fragment == nullptr) {
        continue;
      }
      const auto *impl =
          dynamic_cast<const ast::impl_decl *>(fragment_value.fragment);
      if (impl == nullptr) {
        continue;
      }
      index_.modules[module_name_].impls.push_back(impl_ref{
          .decl = impl, .module_name = module_name_, .file_id = owner_file});
      synthesized_item_splices_.push_back(
          synthesized_item_splice{.impl = impl, .owner_module = module_name_});
      derived_trait_impls_[&decl].push_back(impl);
    }
  }

  /// Walks a file's items (recursing into inline submodules with the module
  /// context switched, exactly as `resolve_item_splices` does) and materializes
  /// every functor instantiation `use m[args]`, so its members — including any
  /// `impl`/`extend` — are registered before the method-table/coherence build.
  auto
  discover_functor_instantiations(const std::vector<ast::ptr<ast::node>> &items)
      -> void {
    for (const auto &item : items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      if (item->kind == ast::node_kind::use_decl) {
        const auto &use = dynamic_cast<const ast::use_decl &>(*item);
        if (!use.instantiation_args.empty()) {
          check_functor_instantiation(use);
        }
        continue;
      }
      if (item->kind == ast::node_kind::sub_module_decl) {
        const auto &sub = dynamic_cast<const ast::sub_module_decl &>(*item);
        if (sub.items.empty() || sub.is_functor()) {
          continue; // a functor declaration itself is materialized per `use`
        }
        const auto saved_module_name = module_name_;
        const auto *saved_module = module_;
        module_name_ = append_module_name(module_name_, sub.name);
        module_ = index_.find_module(module_name_);
        push_scope();
        discover_functor_instantiations(sub.items);
        pop_scope();
        module_name_ = saved_module_name;
        module_ = saved_module;
      }
    }
  }

  auto resolve_item_splices(const std::vector<ast::ptr<ast::node>> &items,
                            file_id_type owner_file) -> void {
    for (const auto &item : items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      if (item->kind == ast::node_kind::type_decl) {
        resolve_deriving_traits(dynamic_cast<const ast::type_decl &>(*item),
                                owner_file);
        continue;
      }
      if (item->kind == ast::node_kind::sub_module_decl) {
        const auto &sub = dynamic_cast<const ast::sub_module_decl &>(*item);
        if (sub.items.empty()) {
          continue;
        }
        const auto saved_module_name = module_name_;
        const auto *saved_module = module_;
        module_name_ = append_module_name(module_name_, sub.name);
        module_ = index_.find_module(module_name_);
        push_scope();
        resolve_item_splices(sub.items, owner_file);
        pop_scope();
        module_name_ = saved_module_name;
        module_ = saved_module;
        continue;
      }
      if (item->kind != ast::node_kind::splice_stmt) {
        continue;
      }
      const auto &splice = dynamic_cast<const ast::splice_stmt &>(*item);
      if (splice.expr == nullptr) {
        continue;
      }
      const auto operand_type = infer_expr(*splice.expr, k_unknown_type);
      require_quote_value(operand_type, splice.expr->span,
                          "an item-level splice");
      const auto fragment_value = comptime_eval_.evaluate(*splice.expr);
      if (fragment_value.is_error()) {
        continue;
      }
      if (fragment_value.kind != comptime::value_kind::def_expr_fragment ||
          fragment_value.fragment == nullptr) {
        error_with_help(
            splice.expr->span,
            std::format("an item-level splice must resolve to a quoted "
                        "definition (`def_expr`), found {}",
                        quote_fragment_kind_name(fragment_value.kind)),
            "this splice is used in item position",
            "Only a value quoted with definition content (e.g. "
            "`` `(impl show for point: ...)` ``) can be spliced among a "
            "module's top-level items; a quoted expression or statement "
            "belongs inside a function body instead.");
        continue;
      }
      const auto *impl =
          dynamic_cast<const ast::impl_decl *>(fragment_value.fragment);
      if (impl == nullptr) {
        error_with_help(
            splice.expr->span,
            "an item-level splice currently only supports injecting an "
            "`impl` block",
            "this quoted definition is not an `impl`",
            "Splicing a quoted `type`/`trait`/`def` at item position is "
            "not supported yet; wrap the generated code in an `impl` "
            "block instead.");
        continue;
      }
      index_.modules[module_name_].impls.push_back(impl_ref{
          .decl = impl, .module_name = module_name_, .file_id = owner_file});
      item_splice_impls_[item.get()] = impl;
      synthesized_item_splices_.push_back(
          synthesized_item_splice{.impl = impl, .owner_module = module_name_});
    }
  }

public:
  /// Runs impl coherence once for the whole session, then checks every
  /// input file that has a valid module declaration and no already-recorded
  /// error, setting up the current-file/current-module context before each.
  auto run_impl(const std::vector<parsed_module> &inputs) -> void {
    // Eagerly, before anything below can take a `const type_entry &`
    // reference into `type_table` and hold it across an expression: `find_
    // method`'s own lazy call to this (guarded by `methods_built_`, so it
    // only actually runs once) interns a type for every cloned trait-
    // default body it monomorphizes, which can reallocate `type_table`'s
    // backing storage. A caller like `infer_method_call` that resolved
    // `entry` just before calling `find_method` — the very first call
    // anywhere in the session, if this weren't forced early — would have
    // that reference silently dangle for the rest of its own scope once
    // `find_method` returns (see `target_type_name` ending up empty
    // instead of a real struct name, for exactly this reason).
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
      register_comptime_globals(input.ast_file->items, input.file_id);
    }

    // Resolve item-level splices before the method table/coherence build
    // below, so any injected impl participates exactly like one written
    // directly in source — see `resolve_item_splices`'s doc comment.
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
      compute_external_wildcard();
      push_scope();
      resolve_item_splices(input.ast_file->items, input.file_id);
      pop_scope();
    }

    // Materialize every functor instantiation (`use m[args]`) before the
    // method table and coherence build below, so any `impl`/`extend` a functor
    // body declares participates in both — exactly like an item-level splice.
    // Bodies are only *registered* here; they are *checked* by
    // `check_pending_functor_bodies` after the table is complete.
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
      compute_external_wildcard();
      push_scope();
      discover_functor_instantiations(input.ast_file->items);
      pop_scope();
    }

    // Register every module's reflectable surface (now including materialized
    // functor instantiations) so compile-time `M.functions()`/`M.types()`/
    // `M.name()` reflection resolves — see `register_comptime_modules`.
    register_comptime_modules();

    build_method_table();
    validate_impl_coherence();
    check_pending_functor_bodies();

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
  // Not `const`: `checker::resolve_item_splices` mutates this in place to
  // graft splice-synthesized impls in before method-table/coherence build —
  // see `checker::index_`'s doc comment.
  auto index = build_program_index(inputs);
  auto session_checker = checker(index, diag, file_has_errors);
  session_checker.run(inputs);
  return session_checker.take_checked_types();
}

} // namespace kira::semantic
