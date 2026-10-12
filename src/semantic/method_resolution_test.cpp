#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "src/parser/ast.h"
#include "src/semantic/method_resolution.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::semantic::type_entry;
using cinder::semantic::type_id;
using cinder::semantic::type_table;
using cinder::semantic::detail::method_entry;
using cinder::semantic::detail::method_origin;
using cinder::semantic::detail::method_resolver;
using cinder::semantic::detail::method_tables;
using cinder::testing::expect;
namespace ast = cinder::ast;

auto func(std::string name) -> std::unique_ptr<ast::func_decl> {
  auto decl = std::make_unique<ast::func_decl>();
  decl->name = std::move(name);
  return decl;
}

auto is_len(const type_entry &entry, std::string_view name) -> bool {
  return entry.name == "str" && name == "len";
}

/// A type table, a generic `boxed` declaration, and the declarations the
/// tests file methods under. The owners are kept here because method
/// entries point at them.
struct fixture {
  type_table types;
  method_tables tables;
  ast::type_decl boxed_decl;
  ast::trait_decl trait;
  ast::type_param param;
  std::unique_ptr<ast::func_decl> get_i32 = func("get");
  std::unique_ptr<ast::func_decl> get_i64 = func("get");
  std::unique_ptr<ast::func_decl> get_any = func("get");
  std::unique_ptr<ast::func_decl> get_default = func("get");
  std::unique_ptr<ast::func_decl> only_extend = func("only_extend");
  std::unique_ptr<ast::func_decl> pure = func("pure");
  std::unique_ptr<ast::func_decl> shout = func("shout");
  std::unique_ptr<ast::func_decl> len = func("len");

  type_id int32 = types.builtin("int32");
  type_id int64 = types.builtin("int64");
  type_id str = types.builtin("str");
  type_id boxed_i32 = types.user_type(boxed_decl, "m", {int32});
  type_id boxed_i64 = types.user_type(boxed_decl, "m", {int64});
  type_id t = types.type_param("T", 0, &param);
  type_id boxed_t = types.user_type(boxed_decl, "m", {t});

  auto resolver() const -> method_resolver {
    return method_resolver{types, tables, &is_len};
  }

  static auto entry_for(const ast::func_decl &decl, type_id pattern)
      -> method_entry {
    auto entry = method_entry{};
    entry.decl = &decl;
    entry.impl_target_pattern = pattern;
    return entry;
  }
};

auto test_exact_instantiation_wins_over_generic_impl() -> void {
  auto f = fixture{};
  auto &bucket = f.tables.declared[&f.boxed_decl];
  // The generic impl is filed first, so only the priority rule can pick the
  // specific one.
  bucket.push_back(fixture::entry_for(*f.get_any, f.boxed_t));
  bucket.push_back(fixture::entry_for(*f.get_i32, f.boxed_i32));
  bucket.push_back(fixture::entry_for(*f.get_i64, f.boxed_i64));

  const auto resolver = f.resolver();
  const auto narrow = resolver.resolve(f.boxed_i32, "get");
  expect(narrow.origin == method_origin::declared,
         "boxed[int32].get resolves to a declared method");
  expect(narrow.method->decl == f.get_i32.get(),
         "boxed[int32].get picks the int32 impl over the generic one");
  const auto wide = resolver.resolve(f.boxed_i64, "get");
  expect(wide.method->decl == f.get_i64.get(),
         "boxed[int64].get picks the int64 impl, not the int32 one");
}

auto test_generic_impl_answers_when_nothing_is_exact() -> void {
  auto f = fixture{};
  f.tables.declared[&f.boxed_decl].push_back(
      fixture::entry_for(*f.get_any, f.boxed_t));
  const auto found = f.resolver().resolve(f.boxed_i64, "get");
  expect(found.method != nullptr && found.method->decl == f.get_any.get(),
         "an impl over boxed[T] answers for boxed[int64]");
}

auto test_a_target_for_another_instantiation_does_not_apply() -> void {
  auto f = fixture{};
  f.tables.declared[&f.boxed_decl].push_back(
      fixture::entry_for(*f.get_i32, f.boxed_i32));
  const auto found = f.resolver().resolve(f.boxed_i64, "get");
  expect(!found.found(),
         "an impl for boxed[int32] does not answer for boxed[int64]");
}

auto test_priority_is_impl_then_trait_default_then_extension() -> void {
  auto f = fixture{};
  auto &bucket = f.tables.declared[&f.boxed_decl];
  auto extension = fixture::entry_for(*f.only_extend, f.boxed_t);
  extension.is_extension = true;
  auto extension_get = fixture::entry_for(*f.get_any, f.boxed_t);
  extension_get.is_extension = true;
  auto trait_default = fixture::entry_for(*f.get_default, f.boxed_t);
  trait_default.from_trait = &f.trait;
  bucket.push_back(extension_get);
  bucket.push_back(trait_default);

  const auto resolver = f.resolver();
  expect(resolver.resolve(f.boxed_i32, "get").method->decl ==
             f.get_default.get(),
         "a trait default beats an extension method");

  bucket.push_back(fixture::entry_for(*f.get_i32, f.boxed_i32));
  expect(resolver.resolve(f.boxed_i32, "get").method->decl == f.get_i32.get(),
         "an impl's own method beats a trait default");

  bucket.push_back(extension);
  expect(resolver.resolve(f.boxed_i32, "only_extend").method->decl ==
             f.only_extend.get(),
         "an extension method answers when nothing else has the name");
}

auto test_filter_narrows_past_the_name() -> void {
  auto f = fixture{};
  auto &bucket = f.tables.declared[&f.boxed_decl];
  bucket.push_back(fixture::entry_for(*f.get_i32, f.boxed_i32));
  bucket.push_back(fixture::entry_for(*f.get_i64, f.boxed_i32));
  const auto wanted = f.get_i64.get();
  const auto found = f.resolver().resolve(
      f.boxed_i32, "get",
      [wanted](const method_entry &m) -> bool { return m.decl == wanted; });
  expect(found.method != nullptr && found.method->decl == wanted,
         "the filter picks the second of two methods with one name");
  const auto none = f.resolver().resolve(
      f.boxed_i32, "get", [](const method_entry &) -> bool { return false; });
  expect(!none.found(), "a filter that rejects everything finds nothing");
}

auto test_builtin_origins() -> void {
  auto f = fixture{};
  f.tables.impl_by_builtin["str"].push_back(
      fixture::entry_for(*f.pure, f.str));
  f.tables.extend_by_builtin["str"].push_back(
      fixture::entry_for(*f.shout, f.str));
  f.tables.impl_by_builtin["str"].push_back(
      fixture::entry_for(*f.len, f.str));
  const auto resolver = f.resolver();

  const auto inherent = resolver.resolve(f.str, "len");
  expect(inherent.origin == method_origin::builtin_inherent,
         "a name the compiler defines is reported as inherent");
  expect(inherent.method != nullptr && inherent.method->decl->name == "len",
         "an inherent name still carries the impl method of that name");

  const auto impl = resolver.resolve(f.str, "pure");
  expect(impl.origin == method_origin::builtin_impl &&
             impl.method->decl == f.pure.get(),
         "an impl on a builtin is reported as builtin_impl");
  const auto extend = resolver.resolve(f.str, "shout");
  expect(extend.origin == method_origin::builtin_extend &&
             extend.method->decl == f.shout.get(),
         "an extend block on a builtin is reported as builtin_extend");
  expect(!resolver.resolve(f.str, "missing").found(),
         "an unknown name on a builtin finds nothing");
  expect(!resolver.resolve(f.int32, "pure").found(),
         "a method filed under str is not found on int32");
}

auto test_constructor_method_and_names() -> void {
  auto f = fixture{};
  f.tables.impl_by_builtin["option"].push_back(
      fixture::entry_for(*f.pure, f.str));
  f.tables.declared[&f.boxed_decl].push_back(
      fixture::entry_for(*f.get_i32, f.boxed_i32));
  f.tables.extend_by_builtin["str"].push_back(
      fixture::entry_for(*f.shout, f.str));
  const auto resolver = f.resolver();

  expect(resolver.constructor_method("option", "pure") == &f.tables
                                                              .impl_by_builtin
                                                                  ["option"]
                                                              .front(),
         "constructor_method finds an impl by the constructor's spelling");
  expect(resolver.constructor_method("option", "other") == nullptr,
         "constructor_method returns null for an unknown name");

  const auto declared =
      resolver.declared_names(f.types.entry(f.boxed_i32));
  expect(declared.size() == 1 && declared.front() == "get",
         "declared_names lists a user type's methods");
  const auto builtin = resolver.declared_names(f.types.entry(f.str));
  expect(builtin.size() == 1 && builtin.front() == "shout",
         "declared_names lists a builtin's extend methods");
}

auto test_mentions_param() -> void {
  auto f = fixture{};
  const auto resolver = f.resolver();
  expect(resolver.mentions_param(f.boxed_t),
         "boxed[T] mentions a type parameter");
  expect(!resolver.mentions_param(f.boxed_i32),
         "boxed[int32] mentions none");
}

} // namespace

auto main() -> int {
  test_exact_instantiation_wins_over_generic_impl();
  test_generic_impl_answers_when_nothing_is_exact();
  test_a_target_for_another_instantiation_does_not_apply();
  test_priority_is_impl_then_trait_default_then_extension();
  test_filter_narrows_past_the_name();
  test_builtin_origins();
  test_constructor_method_and_names();
  test_mentions_param();
  return 0;
}
