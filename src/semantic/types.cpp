#include "types.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <utility>

#include "src/semantic/module_index.h"

namespace kira::semantic {
namespace {

/// Every scalar name recognized as a builtin type (numeric family, `bool`,
/// `str`, quote-value types, and the two built-in execution contexts).
constexpr std::array<std::string_view, 28> k_builtin_scalar_names = {
    "bool",     "char",   "str",      "unit",      "never",   "byte",
    "int8",     "int16",  "int32",    "int64",     "int128",  "uint8",
    "uint16",   "uint32", "uint64",   "uint128",   "float32", "float64",
    "float128", "isize",  "usize",    "ordering",  "io",      "cpu",
    "expr",     "stmt",   "def_expr", "type_expr",
};

/// One prelude container name and its accepted generic-argument count
/// range (inclusive).
struct generic_arity_entry {
  std::string_view name;
  size_t min_args;
  size_t max_args;
};

/// Prelude container names and their allowed generic-argument arities.
constexpr std::array<generic_arity_entry, 13> k_builtin_generic_arities = {{
    {.name = "list", .min_args = 1, .max_args = 1},
    {.name = "option", .min_args = 1, .max_args = 1},
    {.name = "result", .min_args = 2, .max_args = 2},
    {.name = "box", .min_args = 1, .max_args = 1},
    {.name = "slice", .min_args = 1, .max_args = 1},
    {.name = "slice_mut", .min_args = 1, .max_args = 1},
    {.name = "shared", .min_args = 0, .max_args = 1},
    {.name = "task", .min_args = 1, .max_args = 3},
    {.name = "channel", .min_args = 1, .max_args = 1},
    {.name = "watch", .min_args = 1, .max_args = 1},
    {.name = "mutex", .min_args = 1, .max_args = 1},
    {.name = "atomic", .min_args = 1, .max_args = 1},
    // The concrete type backing a `generator def`'s `some iterator[T]`
    // return value — see `check_function`'s generator handling in
    // `src/semantic/check.cpp`. `T` is the generator's item type.
    {.name = "generator", .min_args = 1, .max_args = 1},
}};

/// Appends a bracketed, comma-separated list of `args` ids to `key`, for
/// building a unique intern key that distinguishes generic instantiations.
auto append_args_key(std::string &key, const std::vector<type_id> &args)
    -> void {
  key += '[';
  for (const auto arg : args) {
    key += std::to_string(arg);
    key += ',';
  }
  key += ']';
}

} // namespace

/// Reserves ids 0 and 1 for `k_unknown_type` and `k_error_type`, then
/// pre-interns `bool`/`usize`/`char` (see `bool_type`'s doc comment for why).
type_table::type_table() {
  entries_.push_back(
      type_entry{.kind = type_kind::unknown_kind, .name = "<unknown>"});
  entries_.push_back(
      type_entry{.kind = type_kind::error_kind, .name = "<error>"});
  [[maybe_unused]] const auto bool_id = builtin("bool");
  [[maybe_unused]] const auto usize_id = builtin("usize");
  [[maybe_unused]] const auto char_id = builtin("char");
}

/// Structural interning keyed on `key`: identical keys always produce the
/// same id, so callers build `key` to encode everything that should make
/// two types distinct (kind tag, name/declaration identity, arguments).
auto type_table::intern(std::string key, type_entry entry) -> type_id {
  if (const auto it = interned_.find(key); it != interned_.end()) {
    return it->second;
  }
  const auto id = static_cast<type_id>(entries_.size());
  entries_.push_back(std::move(entry));
  interned_.emplace(std::move(key), id);
  return id;
}

/// Interns using name alone as the key — every `int32` is the same type.
auto type_table::builtin(std::string_view name) -> type_id {
  return intern(
      std::format("b:{}", name),
      type_entry{.kind = type_kind::builtin_kind, .name = std::string(name)});
}

/// Interns using name plus argument ids as the key, so `list[int32]` and
/// `list[str]` are distinct but any two `list[int32]`s are the same type.
auto type_table::builtin_generic(std::string_view name,
                                 std::vector<type_id> args) -> type_id {
  auto key = std::format("g:{}", name);
  append_args_key(key, args);
  return intern(std::move(key),
                type_entry{.kind = type_kind::builtin_generic_kind,
                           .name = std::string(name),
                           .args = std::move(args)});
}

/// Interns using element ids as the key, so tuple identity is purely
/// structural (element types and arity), independent of source location.
auto type_table::tuple_of(std::vector<type_id> elements) -> type_id {
  auto key = std::string("t:");
  append_args_key(key, elements);
  return intern(std::move(key), type_entry{.kind = type_kind::tuple_kind,
                                           .name = "tuple",
                                           .args = std::move(elements)});
}

/// Interns on the element id plus the *length value slot* — not on `size`,
/// which is only a mirror of a closed length: two arrays with the same
/// element and the same symbolic length (`array[T, n]` in two places) must be
/// the same type, and two with different symbolic lengths (`array[T, n]` vs
/// `array[T, m]`) must not be.
auto type_table::array_of(type_id element, std::optional<uint64_t> size,
                          type_id length) -> type_id {
  auto key = std::format("a:{}:{}", element, length);
  return intern(std::move(key), type_entry{.kind = type_kind::array_kind,
                                           .name = "array",
                                           .args = {length},
                                           .result = element,
                                           .array_size = size});
}

/// Interns using parameter ids and the result id as the key.
auto type_table::fn_of(std::vector<type_id> params, type_id result) -> type_id {
  auto key = std::string("f:");
  append_args_key(key, params);
  key += std::format("->{}", result);
  return intern(std::move(key), type_entry{.kind = type_kind::fn_kind,
                                           .name = "fn",
                                           .args = std::move(params),
                                           .result = result});
}

/// Interns using the inner id and mutability as the key.
auto type_table::ref_to(type_id inner, bool is_mut) -> type_id {
  return intern(std::format("r:{}:{}", inner, is_mut),
                type_entry{.kind = type_kind::ref_kind,
                           .name = "ref",
                           .result = inner,
                           .is_mut = is_mut});
}

/// Interns using the inner id and mutability as the key.
auto type_table::ptr_to(type_id inner, bool is_mut) -> type_id {
  return intern(std::format("p:{}:{}", inner, is_mut),
                type_entry{.kind = type_kind::ptr_kind,
                           .name = "ptr",
                           .result = inner,
                           .is_mut = is_mut});
}

/// Interns using the declaration's address and argument ids as the key, so
/// two instantiations of the same generic `type` declaration with the same
/// arguments are the same type, while distinct declarations with the same
/// name (in different modules) never collide. The concrete `type_kind` is
/// inferred from the declaration's body (struct, sum, or opaque/alias).
auto type_table::user_type(const ast::type_decl &decl,
                           std::string_view module_name,
                           std::vector<type_id> args) -> type_id {
  auto kind = type_kind::opaque_kind;
  if (decl.definition != nullptr) {
    if (decl.definition->kind == ast::node_kind::struct_type_def) {
      kind = type_kind::struct_kind;
    } else if (decl.definition->kind == ast::node_kind::sum_type_def) {
      kind = type_kind::sum_kind;
    }
  }
  auto key = std::format("u:{}", static_cast<const void *>(&decl));
  append_args_key(key, args);
  return intern(std::move(key),
                type_entry{.kind = kind,
                           .name = decl.name,
                           .module_name = std::string(module_name),
                           .decl = &decl,
                           .args = std::move(args)});
}

/// Interns using name plus kind (arity) as the key, so same-named type
/// parameters of the same kind collapse to one id while `T` and a
/// (pathological) constructor parameter `T[_]` stay distinct. Arity 0 keeps
/// the historical bare key so ordinary parameters intern exactly as before.
auto type_table::type_param(std::string_view name, size_t arity) -> type_id {
  auto key = arity == 0 ? std::format("v:{}", name)
                        : std::format("v:{}/{}", name, arity);
  return intern(std::move(key), type_entry{.kind = type_kind::type_param_kind,
                                           .name = std::string(name),
                                           .ctor_arity = arity});
}

/// Interns on the declaration's address (user constructors) or the builtin
/// name (prelude constructors), so every unapplied reference to one
/// constructor is one id — and distinct from every *applied* instantiation
/// of it, which interns under the applied keys above.
auto type_table::ctor_ref(std::string_view name, std::string_view module_name,
                          const ast::type_decl *decl, size_t arity) -> type_id {
  auto key = decl != nullptr
                 ? std::format("k:{}", static_cast<const void *>(decl))
                 : std::format("k:{}", name);
  return intern(std::move(key),
                type_entry{.kind = type_kind::ctor_ref_kind,
                           .name = std::string(name),
                           .module_name = std::string(module_name),
                           .decl = decl,
                           .ctor_arity = arity});
}

/// Interns on the head parameter's id plus the argument ids, so `F[A]`
/// written twice is one type while `F[A]` and `F[B]` are two.
auto type_table::param_app(type_id head, std::vector<type_id> args) -> type_id {
  auto key = std::format("app:{}", head);
  append_args_key(key, args);
  return intern(std::move(key), type_entry{.kind = type_kind::param_app_kind,
                                           .name = entry(head).name,
                                           .args = std::move(args),
                                           .result = head});
}

/// Interns using the underlying type id and the value as the key, so two
/// const generic arguments are the same type iff they carry the same
/// literal value of the same underlying type.
auto type_table::const_value(type_id underlying, uint64_t value) -> type_id {
  return intern(
      std::format("c:{}:{}", underlying, value),
      type_entry{.kind = type_kind::const_value_kind,
                 .name = std::to_string(value),
                 .result = underlying,
                 .value = poly_constant(static_cast<int64_t>(value))});
}

/// Interns on the polynomial's canonical key, which is what makes `m + n` and
/// `n + m` one type rather than two. A closed polynomial is redirected to
/// `const_value` so no value has two representations — see the header.
auto type_table::symbolic_value(type_id underlying, linear_poly value)
    -> type_id {
  if (value.is_constant()) {
    return const_value(underlying, static_cast<uint64_t>(value.constant));
  }
  auto key = std::format("s:{}:{}", underlying, value.key());
  auto name = value.display();
  return intern(std::move(key),
                type_entry{.kind = type_kind::symbolic_value_kind,
                           .name = std::move(name),
                           .result = underlying,
                           .value = std::move(value)});
}

/// Interns on the sum declaration's address plus the variant name, so two
/// same-named variants of different sum types never collide.
auto type_table::const_variant(const ast::type_decl &sum_decl,
                               std::string_view module_name,
                               std::string_view variant_name) -> type_id {
  return intern(std::format("cv:{}:{}", static_cast<const void *>(&sum_decl),
                            variant_name),
                type_entry{.kind = type_kind::const_variant_kind,
                           .name = std::string(variant_name),
                           .module_name = std::string(module_name),
                           .decl = &sum_decl});
}

/// Declaration-nominal when `decl` is given (keyed on the declaration plus its
/// arguments), occurrence-nominal otherwise (keyed on the predicate node) —
/// see the header for why refinements are named to the user but structural to
/// the solver.
auto type_table::refinement_of(const ast::type_decl *decl,
                               std::string_view display_name,
                               std::string_view module_name, type_id base,
                               std::vector<type_id> args,
                               const ast::expr *predicate) -> type_id {
  auto key = decl != nullptr
                 ? std::format("w:{}", static_cast<const void *>(decl))
                 : std::format("wi:{}", static_cast<const void *>(predicate));
  append_args_key(key, args);
  return intern(std::move(key), type_entry{
                                    .kind = type_kind::refinement_kind,
                                    .name = std::string(display_name),
                                    .module_name = std::string(module_name),
                                    .decl = decl,
                                    .args = std::move(args),
                                    .result = base,
                                    .predicate = predicate,
                                });
}

/// Always pushes a new entry rather than consulting `interned_`, so every
/// call yields a distinct, never-shared `type_id`.
auto type_table::fresh_type_var() -> type_id {
  const auto id = static_cast<type_id>(entries_.size());
  entries_.push_back(type_entry{.kind = type_kind::type_var_kind,
                                .name = std::format("?{}", id)});
  return id;
}

/// Always pushes a new entry rather than consulting `interned_` — see
/// `type_entry`'s doc comment on why existential types are nominal, not
/// structural.
auto type_table::fresh_existential(std::string display_name,
                                   std::vector<bound_trait_ref> bound)
    -> type_id {
  const auto id = static_cast<type_id>(entries_.size());
  entries_.push_back(type_entry{.kind = type_kind::existential_kind,
                                .name = std::move(display_name),
                                .existential_bound = std::move(bound)});
  return id;
}

/// Bounds-checks `id`, falling back to the `unknown` entry rather than
/// indexing out of range.
auto type_table::entry(type_id id) const -> const type_entry & {
  if (static_cast<size_t>(id) >= entries_.size()) {
    return entries_[k_unknown_type];
  }
  return entries_[id];
}

/// See the header: no bounds fallback, `id` must already be valid.
auto type_table::mutable_entry(type_id id) -> type_entry & {
  return entries_[id];
}

auto type_table::bool_type() const -> type_id {
  const auto it = interned_.find("b:bool");
  return it != interned_.end() ? it->second : k_unknown_type;
}

auto type_table::usize_type() const -> type_id {
  const auto it = interned_.find("b:usize");
  return it != interned_.end() ? it->second : k_unknown_type;
}

auto type_table::char_type() const -> type_id {
  const auto it = interned_.find("b:char");
  return it != interned_.end() ? it->second : k_unknown_type;
}

/// Recursively renders `id` into Kira's own type syntax, matching the
/// spellings used in error messages (`_` for `unknown`, `list[int32]`,
/// `fn(int32) -> str`, `&mut T`, ...).
auto type_table::display(type_id id) const -> std::string {
  const auto &item = entry(id);
  switch (item.kind) {
  case type_kind::unknown_kind:
  case type_kind::type_var_kind:
    return "_";
  case type_kind::error_kind:
    return "<error>";
  case type_kind::builtin_kind:
  case type_kind::type_param_kind:
  case type_kind::ctor_ref_kind:
  case type_kind::existential_kind:
    return item.name;
  case type_kind::param_app_kind:
  case type_kind::builtin_generic_kind:
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::opaque_kind: {
    if (item.args.empty()) {
      return item.name;
    }
    auto out = item.name + "[";
    for (size_t i = 0; i < item.args.size(); ++i) {
      if (i != 0) {
        out += ", ";
      }
      out += display(item.args[i]);
    }
    out += "]";
    return out;
  }
  case type_kind::tuple_kind: {
    auto out = std::string("(");
    for (size_t i = 0; i < item.args.size(); ++i) {
      if (i != 0) {
        out += ", ";
      }
      out += display(item.args[i]);
    }
    out += ")";
    return out;
  }
  case type_kind::array_kind:
    return std::format("array[{}, {}]", display(item.result),
                       item.args.empty() ? std::string("_")
                                         : display(item.args.front()));
  case type_kind::fn_kind: {
    auto out = std::string("fn(");
    for (size_t i = 0; i < item.args.size(); ++i) {
      if (i != 0) {
        out += ", ";
      }
      out += display(item.args[i]);
    }
    out += std::format(") -> {}", display(item.result));
    return out;
  }
  case type_kind::ref_kind:
    return std::format("&{}{}", item.is_mut ? "mut " : "",
                       display(item.result));
  case type_kind::ptr_kind:
    return std::format("*{}{}", item.is_mut ? "mut " : "",
                       display(item.result));
  case type_kind::const_value_kind:
  case type_kind::symbolic_value_kind:
  case type_kind::const_variant_kind:
  case type_kind::refinement_kind:
    return item.name;
  }
  return "<?>";
}

/// See the header for semantics.
auto type_table::strip_refinement(type_id id) const -> type_id {
  const auto *item = &entry(id);
  while (item->kind == type_kind::refinement_kind) {
    id = item->result;
    item = &entry(id);
  }
  return id;
}

/// Rebuilds the type bottom-up, re-interning as it goes, so the result is a
/// first-class id and not a special "erased" view. Types with no arguments to
/// rewrite (the overwhelming majority) are returned unchanged without
/// interning anything.
auto type_table::erase_refinements(type_id id) -> type_id {
  const auto stripped = strip_refinement(id);

  // Copied, not referenced: interning below can push new entries, and while
  // `std::deque` keeps existing references valid, the recursion also reads
  // fields after those calls — a copy makes that independent of the table's
  // storage guarantees rather than reliant on them.
  const auto item = entry(stripped);

  auto args = std::vector<type_id>{};
  args.reserve(item.args.size());
  auto changed = false;
  for (const auto arg : item.args) {
    const auto erased = erase_refinements(arg);
    changed = changed || erased != arg;
    args.push_back(erased);
  }
  const auto result = item.result != k_unknown_type
                          ? erase_refinements(item.result)
                          : item.result;
  changed = changed || result != item.result;
  if (!changed) {
    return stripped;
  }

  switch (item.kind) {
  case type_kind::builtin_generic_kind:
    return builtin_generic(item.name, std::move(args));
  case type_kind::tuple_kind:
    return tuple_of(std::move(args));
  case type_kind::array_kind:
    return array_of(result, item.array_size,
                    args.empty() ? k_unknown_type : args.front());
  case type_kind::fn_kind:
    return fn_of(std::move(args), result);
  case type_kind::ref_kind:
    return ref_to(result, item.is_mut);
  case type_kind::ptr_kind:
    return ptr_to(result, item.is_mut);
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::opaque_kind:
    if (item.decl == nullptr) {
      return stripped;
    }
    return user_type(*item.decl, item.module_name, std::move(args));
  default:
    // Scalars, value slots, type parameters, existentials: nothing inside to
    // rewrite, so `changed` can't have been set and this is unreachable in
    // practice — but returning the stripped id is the right answer anyway.
    return stripped;
  }
}

/// Linear scan: layout asks this once per struct field of a user type, and
/// the table is small. See the header for why a name is all there is to go on.
auto type_table::refinement_base_named(std::string_view name) const
    -> std::optional<type_id> {
  auto found = std::optional<type_id>{};
  for (const auto &item : entries_) {
    if (item.kind != type_kind::refinement_kind || item.decl == nullptr ||
        item.decl->name != name) {
      continue;
    }
    const auto base = strip_refinement(item.result);
    if (found.has_value() && *found != base) {
      return std::nullopt; // ambiguous; see the header
    }
    found = base;
  }
  return found;
}

/// See the header for semantics.
auto type_table::is_unknown(type_id id) const -> bool {
  const auto kind = entry(id).kind;
  // `param_app_kind` counts as unknown for the same reason a bare type
  // parameter does: `F[A]` under an abstract `F` can become anything at
  // instantiation time, so complaining about it would be a false positive.
  return kind == type_kind::unknown_kind || kind == type_kind::error_kind ||
         kind == type_kind::type_param_kind ||
         kind == type_kind::param_app_kind || kind == type_kind::type_var_kind;
}

// Every scalar-family predicate below strips refinements first: a `positive`
// *is* an `int32` for every purpose except discharging its own predicate, so
// it must be numeric, comparable, and formattable exactly like one.

/// See the header for semantics.
auto type_table::is_boolean(type_id id) const -> bool {
  const auto &item = entry(strip_refinement(id));
  return item.kind == type_kind::builtin_kind && item.name == "bool";
}

/// See the header for semantics.
auto type_table::is_integer(type_id id) const -> bool {
  const auto &item = entry(strip_refinement(id));
  if (item.kind != type_kind::builtin_kind) {
    return false;
  }
  return item.name.starts_with("int") || item.name.starts_with("uint") ||
         item.name == "byte" || item.name == "isize" || item.name == "usize";
}

/// See the header for semantics.
auto type_table::is_float(type_id id) const -> bool {
  const auto &item = entry(strip_refinement(id));
  return item.kind == type_kind::builtin_kind && item.name.starts_with("float");
}

/// See the header for semantics.
auto type_table::is_numeric(type_id id) const -> bool {
  return is_integer(id) || is_float(id);
}

/// See the header for semantics.
auto type_table::is_unit(type_id id) const -> bool {
  const auto &item = entry(strip_refinement(id));
  return item.kind == type_kind::builtin_kind && item.name == "unit";
}

/// Whether the unknowns of a value slot range over an unsigned type, which is
/// the fact that refutes `n + 1 = 0`. Read off the slot's underlying type
/// (`result`), so `n: usize` constrains and `n: int32` doesn't.
auto type_table::value_vars_non_negative(const type_entry &slot) const -> bool {
  const auto &underlying = entry(slot.result);
  return underlying.kind == type_kind::builtin_kind &&
         (underlying.name.starts_with("uint") || underlying.name == "usize" ||
          underlying.name == "byte");
}

/// Compatibility of two *value* slots, per `spec/dependent-types-design.md`
/// §2.2: satisfiability of the equation between them, not identity. Identical
/// ids were already handled by `compatible`'s first line, so reaching here
/// means the two slots differ syntactically and the question is whether they
/// could still denote the same value.
auto type_table::values_compatible(type_id expected, type_id found) const
    -> bool {
  const auto &expected_entry = entry(expected);
  const auto &found_entry = entry(found);

  // A variant value is an identity, not a quantity: distinct ids are distinct
  // states, which is exactly what makes `send(c: connection[open])` reject a
  // `connection[closed]`.
  if (expected_entry.kind == type_kind::const_variant_kind ||
      found_entry.kind == type_kind::const_variant_kind) {
    return false;
  }

  return equation_satisfiable(expected_entry.value, found_entry.value,
                              value_vars_non_negative(expected_entry) ||
                                  value_vars_non_negative(found_entry));
}

/// Structural compatibility: identical ids always match; `unknown`/`error`/
/// type-parameter types match anything; `never` (from `panic` or an early
/// return) matches any expected type; a reference on either side compares
/// against its target type; and same-kind types compare their
/// declaration/name identity plus recursively-compatible arguments.
auto type_table::compatible(type_id expected, type_id found) const -> bool {
  if (expected == found || is_unknown(expected) || is_unknown(found)) {
    return true;
  }

  // Refinements compare as their base in *both* directions. Widening
  // (`positive` where `int32` is wanted) is genuinely free. Narrowing
  // (`int32` where `positive` is wanted) is structurally fine too — what it
  // owes is a *proof*, not a different shape, and `checker::check_narrowing`
  // is where that debt is collected. See the header.
  if (entry(expected).kind == type_kind::refinement_kind ||
      entry(found).kind == type_kind::refinement_kind) {
    return compatible(strip_refinement(expected), strip_refinement(found));
  }

  const auto &expected_entry = entry(expected);
  const auto &found_entry = entry(found);

  if (is_value_kind(expected_entry.kind) && is_value_kind(found_entry.kind)) {
    return values_compatible(expected, found);
  }

  // `never` (panic, propagating return) is acceptable anywhere.
  if (found_entry.kind == type_kind::builtin_kind &&
      found_entry.name == "never") {
    return true;
  }

  if (expected_entry.kind != found_entry.kind) {
    // A `&T` argument position accepts a lent `T`; the borrow checker owns
    // the deeper rules, so typing treats the reference as its target here.
    if (expected_entry.kind == type_kind::ref_kind) {
      return compatible(expected_entry.result, found);
    }
    if (found_entry.kind == type_kind::ref_kind) {
      return compatible(expected, found_entry.result);
    }
    return false;
  }

  switch (expected_entry.kind) {
  case type_kind::builtin_generic_kind:
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::opaque_kind: {
    const auto same_declaration = expected_entry.decl != nullptr
                                      ? expected_entry.decl == found_entry.decl
                                      : expected_entry.name == found_entry.name;
    if (!same_declaration ||
        expected_entry.args.size() != found_entry.args.size()) {
      return false;
    }
    for (size_t i = 0; i < expected_entry.args.size(); ++i) {
      if (!compatible(expected_entry.args[i], found_entry.args[i])) {
        return false;
      }
    }
    return true;
  }
  case type_kind::tuple_kind:
  case type_kind::fn_kind: {
    if (expected_entry.args.size() != found_entry.args.size() ||
        !compatible(expected_entry.result, found_entry.result)) {
      return false;
    }
    for (size_t i = 0; i < expected_entry.args.size(); ++i) {
      if (!compatible(expected_entry.args[i], found_entry.args[i])) {
        return false;
      }
    }
    return true;
  }
  case type_kind::array_kind:
    // The length is an ordinary value slot, so `array[T, n + 1]` against
    // `array[T, 0]` is refuted by the same arithmetic that refutes it for a
    // user `vec[T, n + 1]` — a written-out length and a const generic are the
    // same mechanism.
    return compatible(expected_entry.result, found_entry.result) &&
           (expected_entry.args.empty() || found_entry.args.empty() ||
            compatible(expected_entry.args.front(), found_entry.args.front()));
  case type_kind::ref_kind:
  case type_kind::ptr_kind:
    return compatible(expected_entry.result, found_entry.result) &&
           (found_entry.is_mut || !expected_entry.is_mut);
  default:
    return false;
  }
}

/// Linear search over `k_builtin_scalar_names`.
auto is_builtin_scalar_name(std::string_view name) -> bool {
  return std::ranges::any_of(
      k_builtin_scalar_names,
      [name](std::string_view candidate) -> bool { return candidate == name; });
}

auto tuple_index_of(std::string_view name) -> std::optional<std::size_t> {
  if (name.empty() || !std::ranges::all_of(name, [](char c) -> bool {
        return c >= '0' && c <= '9';
      })) {
    return std::nullopt;
  }
  auto value = std::size_t{0};
  const auto result =
      std::from_chars(name.data(), name.data() + name.size(), value);
  if (result.ec != std::errc{} || result.ptr != name.data() + name.size()) {
    return std::nullopt;
  }
  return value;
}

/// Linear search over `k_builtin_generic_arities`.
auto builtin_generic_arity(std::string_view name)
    -> std::optional<std::pair<size_t, size_t>> {
  for (const auto &candidate : k_builtin_generic_arities) {
    if (candidate.name == name) {
      return std::pair{candidate.min_args, candidate.max_args};
    }
  }
  return std::nullopt;
}

/// 128-bit integer types have no representable max in `uint64_t` and are
/// intentionally left unhandled (returning `nullopt`), so literal-fit
/// checking simply does not fire for them yet.
auto integer_max_value(std::string_view name) -> std::optional<uint64_t> {
  if (name == "int8") {
    return 127u;
  }
  if (name == "int16") {
    return 32767u;
  }
  if (name == "int32") {
    return 2147483647u;
  }
  if (name == "int64" || name == "isize") {
    return 9223372036854775807u;
  }
  if (name == "uint8" || name == "byte") {
    return 255u;
  }
  if (name == "uint16") {
    return 65535u;
  }
  if (name == "uint32") {
    return 4294967295u;
  }
  if (name == "uint64" || name == "usize") {
    return 18446744073709551615u;
  }
  return std::nullopt;
}

// ==========================================================================
//  Program index construction
// ==========================================================================

namespace {

/// Expands one `use` declaration into the local name bindings it
/// introduces: the trailing path segment for a bare `use a.b.c`, one
/// binding per selected/renamed item for `use a.b.{c, d as e}`, or a single
/// wildcard marker for `use a.b.*`.
auto record_use_bindings(const ast::use_decl &decl, file_id_type file_id,
                         program_index &index) -> void {
  auto &bindings = index.imports[file_id];

  if (!decl.instantiation_args.empty()) {
    // A functor instantiation (`use audited[postgres] as db`) is not an
    // ordinary import: the alias binds to the *instantiated* module, which the
    // checker materializes and registers. Skip it here so no binding pointing
    // at the bare functor is created (`check_functor_instantiation`).
    return;
  }

  if (!decl.selector.has_value()) {
    if (decl.path.empty()) {
      return;
    }
    bindings.push_back(import_binding{
        .local_name = decl.path.back(),
        .path = decl.path,
        .leaf_name = {},
        .is_wildcard = false,
        .span = decl.span,
    });
    return;
  }

  if (decl.selector->kind == ast::use_selector_kind::wildcard) {
    bindings.push_back(import_binding{
        .local_name = {},
        .path = decl.path,
        .leaf_name = {},
        .is_wildcard = true,
        .span = decl.selector->span,
    });
    return;
  }

  for (const auto &item : decl.selector->items) {
    bindings.push_back(import_binding{
        .local_name = item.alias.value_or(item.name),
        .path = decl.path,
        .leaf_name = item.name,
        .is_wildcard = false,
        .span = item.span,
    });
  }
}

/// Classifies one module-scope item into `members`' matching table (types,
/// traits, concepts, functions, static bindings, impls, extends), also
/// indexing a sum type's variants by name so a bare `@variant` can be found
/// later without knowing which sum type it belongs to.
auto record_module_item(const ast::node &item, file_id_type file_id,
                        module_members &members) -> void {
  switch (item.kind) {
  case ast::node_kind::type_decl: {
    const auto &decl = dynamic_cast<const ast::type_decl &>(item);
    if (decl.name.empty()) {
      return;
    }
    members.types.emplace(decl.name,
                          type_decl_ref{.decl = &decl, .file_id = file_id});
    if (decl.definition != nullptr &&
        decl.definition->kind == ast::node_kind::sum_type_def) {
      const auto &sum =
          dynamic_cast<const ast::sum_type_def &>(*decl.definition);
      for (const auto &variant : sum.body.variants) {
        if (!variant.name.empty()) {
          members.variants.emplace(
              variant.name,
              variant_ref{.sum_decl = &decl, .variant = &variant});
        }
      }
    }
    return;
  }
  case ast::node_kind::trait_decl: {
    const auto &decl = dynamic_cast<const ast::trait_decl &>(item);
    if (!decl.name.empty()) {
      members.traits.emplace(decl.name,
                             trait_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::concept_decl: {
    const auto &decl = dynamic_cast<const ast::concept_decl &>(item);
    if (!decl.name.empty()) {
      members.concepts.emplace(
          decl.name, concept_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::signature_decl: {
    const auto &decl = dynamic_cast<const ast::signature_decl &>(item);
    if (!decl.name.empty()) {
      members.signatures.emplace(
          decl.name, signature_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::func_decl: {
    const auto &decl = dynamic_cast<const ast::func_decl &>(item);
    if (!decl.name.empty()) {
      members.functions.emplace(
          decl.name, func_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::static_decl: {
    const auto &decl = dynamic_cast<const ast::static_decl &>(item);
    if (decl.decl_kind == ast::static_decl_kind::binding &&
        !decl.name.empty()) {
      members.statics.emplace(
          decl.name, static_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::impl_decl: {
    const auto &decl = dynamic_cast<const ast::impl_decl &>(item);
    members.impls.push_back(impl_ref{
        .decl = &decl, .module_name = members.module_name, .file_id = file_id});
    return;
  }
  case ast::node_kind::extend_decl: {
    const auto &decl = dynamic_cast<const ast::extend_decl &>(item);
    members.extends.push_back(extend_ref{
        .decl = &decl, .module_name = members.module_name, .file_id = file_id});
    return;
  }
  default:
    return;
  }
}

/// Recursively indexes `items` into `index`: `use` declarations become
/// import bindings, inline submodules recurse under their qualified name,
/// and everything else is classified via `record_module_item`.
auto index_items(const std::vector<ast::ptr<ast::node>> &items,
                 std::string_view module_name, file_id_type file_id,
                 program_index &index) -> void {
  auto &members = index.modules[std::string(module_name)];
  members.module_name = std::string(module_name);

  for (const auto &item : items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (item->kind == ast::node_kind::use_decl) {
      record_use_bindings(dynamic_cast<const ast::use_decl &>(*item), file_id,
                          index);
      continue;
    }
    if (item->kind == ast::node_kind::sub_module_decl) {
      const auto &decl = dynamic_cast<const ast::sub_module_decl &>(*item);
      if (decl.is_functor()) {
        // A parameterized module is a functor: recorded on its parent so
        // instantiation can find it, but *not* indexed as a module of its own
        // (its body is elaborated per instantiation, not once as written).
        if (!decl.name.empty()) {
          members.functors.emplace(
              decl.name, functor_decl_ref{.decl = &decl, .file_id = file_id});
        }
        continue;
      }
      if (!decl.items.empty()) {
        index_items(decl.items, append_module_name(module_name, decl.name),
                    file_id, index);
      }
      continue;
    }
    record_module_item(*item, file_id, members);
  }
}

} // namespace

/// Exact-match lookup in the `modules` map.
auto program_index::find_module(std::string_view module_name) const
    -> const module_members * {
  const auto it = modules.find(std::string(module_name));
  return it != modules.end() ? &it->second : nullptr;
}

/// Skips files without a valid module declaration, then indexes the rest
/// via `index_items`.
auto build_program_index(const std::vector<parsed_module> &inputs)
    -> program_index {
  auto index = program_index{};

  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty()) {
      continue;
    }
    const auto module_name =
        join_strings(input.ast_file->module_decl->path, ".");
    index_items(input.ast_file->items, module_name, input.file_id, index);
  }

  return index;
}

} // namespace kira::semantic
