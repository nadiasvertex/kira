#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/comptime_vm/backend.h"
#include "src/parser/diagnostic.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/runtime/result_out.h"
#include "src/runtime/value_layout.h"
#include "src/semantic/aggregate_shape.h"
#include "src/semantic/check.h"
#include "src/semantic/checker_results.h"
#include "src/testing/test_assert.h"
#include "src/testing/test_data.h"

namespace {

using cinder::testing::expect;
namespace ast = cinder::ast;
namespace runtime = cinder::runtime;
namespace semantic = cinder::semantic;

// ==========================================================================
//  Hand-built types. These check the layout rules against sizes worked out by
//  hand, with no dependence on the checker.
// ==========================================================================

struct world {
  semantic::type_table types;
  semantic::aggregate_shapes shapes;
  std::deque<ast::type_decl> decls;

  auto scalar(std::string_view name) -> semantic::type_id {
    return types.builtin(name);
  }

  // `array[element, count]`. Arrays intern by their length slot, not by the
  // integer, so each count needs its own constant to be a distinct type.
  auto array(semantic::type_id element, std::optional<uint64_t> count)
      -> semantic::type_id {
    if (!count.has_value()) {
      return types.array_of(element, std::nullopt);
    }
    return types.array_of(element, count,
                          types.const_value(scalar("usize"), *count));
  }

  auto
  make_struct(std::string name,
              std::vector<std::pair<std::string, semantic::type_id>> fields,
              bool packed = false) -> semantic::type_id {
    auto &decl = decls.emplace_back();
    decl.name = std::move(name);
    decl.modifiers.is_packed = packed;
    decl.definition = std::make_unique<ast::struct_type_def>();
    const auto id = types.user_type(decl, "test", {});
    auto shape = semantic::aggregate_shape{.packed = packed};
    for (auto &[field_name, type] : fields) {
      shape.fields.push_back(
          semantic::field_shape{.name = std::move(field_name), .type = type});
    }
    shapes.emplace(id, std::move(shape));
    return id;
  }

  auto
  make_sum(std::string name,
           std::vector<std::pair<std::string, std::vector<semantic::type_id>>>
               variants) -> semantic::type_id {
    auto &decl = decls.emplace_back();
    decl.name = std::move(name);
    decl.definition = std::make_unique<ast::sum_type_def>();
    const auto id = types.user_type(decl, "test", {});
    auto shape = semantic::aggregate_shape{};
    for (auto &[variant_name, payload] : variants) {
      shape.variants.push_back(semantic::variant_shape{
          .name = std::move(variant_name), .payload = std::move(payload)});
    }
    shapes.emplace(id, std::move(shape));
    return id;
  }

  // An `option[payload]` or `result[ok, err]`, with the shape the checker
  // would record for it.
  auto make_option(semantic::type_id payload) -> semantic::type_id {
    const auto id = types.builtin_generic("option", {payload});
    shapes.emplace(
        id, semantic::aggregate_shape{
                .variants = {semantic::variant_shape{.name = "some",
                                                     .payload = {payload}},
                             semantic::variant_shape{.name = "none"}}});
    return id;
  }

  auto make_result(semantic::type_id ok, semantic::type_id err)
      -> semantic::type_id {
    const auto id = types.builtin_generic("result", {ok, err});
    shapes.emplace(
        id, semantic::aggregate_shape{
                .variants = {
                    semantic::variant_shape{.name = "ok", .payload = {ok}},
                    semantic::variant_shape{.name = "err", .payload = {err}}}});
    return id;
  }
};

auto expect_size_align(runtime::value_layouts &layouts, semantic::type_id id,
                       size_t size, size_t align, std::string_view what)
    -> void {
  const auto found = layouts.layout(id);
  expect(found.has_value(),
         std::string(what) + ": expected a layout, got " +
             (found.has_value() ? "" : found.error().message));
  expect((*found)->size == size, std::string(what) + ": expected size " +
                                     std::to_string(size) + ", got " +
                                     std::to_string((*found)->size));
  expect((*found)->align == align, std::string(what) + ": expected alignment " +
                                       std::to_string(align) + ", got " +
                                       std::to_string((*found)->align));
}

auto expect_failure(runtime::value_layouts &layouts, semantic::type_id id,
                    runtime::layout_error_kind kind, std::string_view what)
    -> runtime::layout_error {
  const auto found = layouts.layout(id);
  expect(!found.has_value(), std::string(what) + ": expected a failure");
  expect(found.error().kind == kind,
         std::string(what) + ": wrong failure kind: " + found.error().message);
  return found.error();
}

auto contains(const std::string &text, std::string_view needle) -> bool {
  return text.find(needle) != std::string::npos;
}

auto test_scalar_layouts() -> void {
  auto w = world{};
  auto layouts = runtime::value_layouts{w.types, w.shapes};
  expect_size_align(layouts, w.scalar("int8"), 1, 1, "int8");
  expect_size_align(layouts, w.scalar("int16"), 2, 2, "int16");
  expect_size_align(layouts, w.scalar("int32"), 4, 4, "int32");
  expect_size_align(layouts, w.scalar("float64"), 8, 8, "float64");
  expect_size_align(layouts, w.scalar("int128"), 16, 16, "int128");
  expect_size_align(layouts, w.scalar("unit"), 0, 1, "unit");
  expect_size_align(layouts, w.scalar("str"), 16, 8, "str descriptor");
  const auto failure =
      expect_failure(layouts, w.scalar("float128"),
                     runtime::layout_error_kind::unsupported, "float128");
  expect(contains(failure.message, "float128"),
         "expected the failure to name the type");
}

auto test_address_descriptor_and_handle_layouts() -> void {
  auto w = world{};
  auto layouts = runtime::value_layouts{w.types, w.shapes};
  const auto i32 = w.scalar("int32");
  expect_size_align(layouts, w.types.ref_to(i32, false), 8, 8, "&int32");
  expect_size_align(layouts, w.types.ptr_to(i32, true), 8, 8, "*mut int32");
  expect_size_align(layouts, w.types.builtin_generic("slice", {i32}), 16, 8,
                    "slice");
  expect_size_align(layouts, w.types.builtin_generic("slice_mut", {i32}), 16, 8,
                    "slice_mut");
  expect_size_align(layouts, w.types.builtin_generic("cell", {i32}), 8, 8,
                    "cell");
  expect_size_align(layouts, w.types.builtin_generic("generator", {i32}), 8, 8,
                    "generator");
  expect_size_align(layouts, w.types.builtin_generic("task", {i32}), 8, 8,
                    "task");
  expect_size_align(layouts, w.types.fn_of({i32}, i32), 24, 8, "fn value");
  const auto slice = layouts.layout(w.types.builtin_generic("slice", {i32}));
  expect((*slice)->kind == runtime::value_layout_kind::descriptor,
         "expected a slice to be a descriptor");
}

auto test_reserved_builtins_fail_closed() -> void {
  auto w = world{};
  auto layouts = runtime::value_layouts{w.types, w.shapes};
  const auto i32 = w.scalar("int32");
  for (const auto *name : {"box", "channel", "watch", "mutex", "atomic"}) {
    const auto failure =
        expect_failure(layouts, w.types.builtin_generic(name, {i32}),
                       runtime::layout_error_kind::unsupported, name);
    expect(contains(failure.message, name), "expected the failure to name it");
  }
  // A struct whose shape the checker never recorded is unresolved, not empty.
  auto &decl = w.decls.emplace_back();
  decl.name = "ghost";
  decl.definition = std::make_unique<ast::struct_type_def>();
  expect_failure(layouts, w.types.user_type(decl, "test", {}),
                 runtime::layout_error_kind::unresolved,
                 "struct with no shape");
  expect_failure(layouts, semantic::k_unknown_type,
                 runtime::layout_error_kind::unresolved, "unknown type");
}

auto test_struct_padding_and_nesting() -> void {
  auto w = world{};
  auto layouts = runtime::value_layouts{w.types, w.shapes};
  const auto i8 = w.scalar("int8");
  const auto i32 = w.scalar("int32");
  const auto i64 = w.scalar("int64");

  const auto padded =
      w.make_struct("padded", {{"a", i8}, {"b", i32}, {"c", i8}});
  expect_size_align(layouts, padded, 12, 4, "padded struct");
  const auto padded_layout = *layouts.layout(padded);
  expect(padded_layout->find_field("a")->offset == 0, "a at 0");
  expect(padded_layout->find_field("b")->offset == 4, "b at 4");
  expect(padded_layout->find_field("c")->offset == 8, "c at 8");
  expect(padded_layout->find_field("missing") == nullptr,
         "expected an unknown field to be absent");

  const auto inner = w.make_struct("inner", {{"a", i64}, {"b", i8}});
  expect_size_align(layouts, inner, 16, 8, "inner struct");
  const auto outer = w.make_struct("outer", {{"x", i8}, {"in", inner}});
  expect_size_align(layouts, outer, 24, 8, "nested struct");
  expect((*layouts.layout(outer))->find_field("in")->offset == 8,
         "expected the nested struct at its own alignment");

  const auto empty = w.make_struct("empty", {});
  expect_size_align(layouts, empty, 0, 1, "field-less struct");
}

auto test_packed_struct_has_no_padding() -> void {
  auto w = world{};
  auto layouts = runtime::value_layouts{w.types, w.shapes};
  const auto packed = w.make_struct("packed",
                                    {{"a", w.scalar("bool")},
                                     {"b", w.scalar("int32")},
                                     {"c", w.scalar("bool")}},
                                    /*packed=*/true);
  expect_size_align(layouts, packed, 6, 1, "packed struct");
  const auto layout = *layouts.layout(packed);
  expect(layout->packed, "expected the packed flag on the layout");
  expect(layout->find_field("b")->offset == 1, "packed b at 1");
  expect(layout->find_field("c")->offset == 5, "packed c at 5");
  // A padded struct holding a packed one keeps the packed one's alignment of 1.
  const auto holder =
      w.make_struct("holder", {{"tag", w.scalar("int8")}, {"inner", packed}});
  expect_size_align(layouts, holder, 7, 1, "struct holding a packed struct");
}

auto test_tuple_and_int128_alignment() -> void {
  auto w = world{};
  auto layouts = runtime::value_layouts{w.types, w.shapes};
  const auto pair = w.types.tuple_of({w.scalar("int8"), w.scalar("int128")});
  expect_size_align(layouts, pair, 32, 16, "(int8, int128)");
  const auto layout = *layouts.layout(pair);
  expect(layout->members.size() == 2, "expected two tuple members");
  expect(layout->members[0].offset == 0 && layout->members[1].offset == 16,
         "expected the int128 at offset 16");
  expect_size_align(layouts, w.types.tuple_of({}), 0, 1, "()");
}

auto test_array_stride_and_zero_sized_arrays() -> void {
  auto w = world{};
  auto layouts = runtime::value_layouts{w.types, w.shapes};
  const auto i32 = w.scalar("int32");
  const auto five = w.array(i32, 5);
  expect_size_align(layouts, five, 20, 4, "array[int32, 5]");
  const auto five_layout = *layouts.layout(five);
  expect(five_layout->stride == 4 && five_layout->count == 5,
         "expected stride 4 and count 5");

  const auto inner = w.make_struct(
      "inner", {{"a", w.scalar("int64")}, {"b", w.scalar("int8")}});
  const auto structs = w.array(inner, 3);
  expect_size_align(layouts, structs, 48, 8, "array of 16-byte structs");
  expect((*layouts.layout(structs))->stride == 16, "expected stride 16");

  expect_size_align(layouts, w.array(w.scalar("int128"), 3), 48, 16,
                    "array of int128");
  expect_size_align(layouts, w.array(i32, 0), 0, 4, "array[int32, 0]");
  const auto units = w.array(w.scalar("unit"), 1000);
  expect_size_align(layouts, units, 0, 1, "array[unit, 1000]");
  expect((*layouts.layout(units))->stride == 0 &&
             (*layouts.layout(units))->count == 1000,
         "expected a zero stride that still records the count");
  expect_failure(layouts, w.array(i32, std::nullopt),
                 runtime::layout_error_kind::unresolved, "unsized array");
}

auto test_sum_layouts() -> void {
  auto w = world{};
  auto layouts = runtime::value_layouts{w.types, w.shapes};
  const auto i8 = w.scalar("int8");
  const auto i32 = w.scalar("int32");

  // Only unit variants: the tag alone.
  expect_size_align(layouts, w.make_sum("flags", {{"on", {}}, {"off", {}}}), 8,
                    8, "payload-free sum");

  expect_size_align(layouts, w.make_option(i32), 16, 8, "option[int32]");
  expect_size_align(layouts, w.make_option(i8), 16, 8, "option[int8]");

  // `rect` is a tuple of two int32 (8 bytes, align 4) placed after the tag.
  const auto shape = w.make_sum(
      "shape",
      {{"circle", {w.scalar("float64")}}, {"rect", {i32, i32}}, {"empty", {}}});
  expect_size_align(layouts, shape, 16, 8, "shape");
  const auto shape_layout = *layouts.layout(shape);
  expect(shape_layout->payload_offset == 8, "expected the payload at 8");
  expect(shape_layout->find_variant("rect")->tag == 1, "rect has tag 1");
  expect(shape_layout->find_variant("rect")->payload[1].offset == 12,
         "expected the second rect payload at 12");
  expect(shape_layout->find_variant("empty")->payload.empty(),
         "expected a unit variant to have no payload");
  expect(shape_layout->find_variant("nope") == nullptr, "no such variant");

  // A 128-bit payload pushes the payload area to 16 and the whole to 16-align.
  const auto wide =
      w.make_sum("wide", {{"big", {w.scalar("int128")}}, {"small", {i8}}});
  expect_size_align(layouts, wide, 32, 16, "sum with an int128 payload");
  expect((*layouts.layout(wide))->payload_offset == 16,
         "expected the int128 payload area at 16");
  expect((*layouts.layout(wide))->find_variant("small")->payload[0].offset ==
             16,
         "expected every variant's payload at the payload offset");

  expect_size_align(layouts, w.make_result(i32, w.scalar("str")), 24, 8,
                    "result[int32, str]");
  // A payload that is itself a sum nests inline.
  expect_size_align(layouts, w.make_option(w.make_option(i32)), 24, 8,
                    "option[option[int32]]");
}

auto test_refinement_lays_out_as_its_base() -> void {
  auto w = world{};
  auto layouts = runtime::value_layouts{w.types, w.shapes};
  const auto i16 = w.scalar("int16");
  expect_size_align(layouts, w.types.strip_refinement(i16), 2, 2, "int16");
}

auto test_inline_cycles_are_rejected_with_a_path() -> void {
  auto w = world{};
  auto layouts = runtime::value_layouts{w.types, w.shapes};

  // Self-containing struct: built in two steps because the field names the
  // type being declared.
  auto &decl = w.decls.emplace_back();
  decl.name = "direct";
  decl.definition = std::make_unique<ast::struct_type_def>();
  const auto direct = w.types.user_type(decl, "test", {});
  w.shapes.emplace(direct, semantic::aggregate_shape{
                               .fields = {{.name = "inner", .type = direct}}});
  const auto direct_error = expect_failure(
      layouts, direct, runtime::layout_error_kind::infinite_size, "direct");
  expect(contains(direct_error.message, "`direct` contains itself") &&
             contains(direct_error.message, "`direct.inner` is `direct`"),
         "expected the field path in the message: " + direct_error.message);

  // Mutual recursion names every field on the cycle.
  auto &ping_decl = w.decls.emplace_back();
  ping_decl.name = "ping";
  ping_decl.definition = std::make_unique<ast::struct_type_def>();
  auto &pong_decl = w.decls.emplace_back();
  pong_decl.name = "pong";
  pong_decl.definition = std::make_unique<ast::struct_type_def>();
  const auto ping = w.types.user_type(ping_decl, "test", {});
  const auto pong = w.types.user_type(pong_decl, "test", {});
  w.shapes.emplace(ping, semantic::aggregate_shape{
                             .fields = {{.name = "next", .type = pong}}});
  w.shapes.emplace(pong, semantic::aggregate_shape{
                             .fields = {{.name = "back", .type = ping}}});
  const auto ping_error = expect_failure(
      layouts, ping, runtime::layout_error_kind::infinite_size, "ping");
  expect(contains(ping_error.message, "`ping.next` is `pong`") &&
             contains(ping_error.message, "`pong.back` is `ping`"),
         "expected both fields on the cycle: " + ping_error.message);
  expect_failure(layouts, pong, runtime::layout_error_kind::infinite_size,
                 "pong");

  // A type that merely contains a cyclic one fails too, and the message still
  // names the cycle.
  const auto holder = w.make_struct("holder", {{"p", ping}});
  const auto holder_error = expect_failure(
      layouts, holder, runtime::layout_error_kind::infinite_size, "holder");
  expect(contains(holder_error.message, "`ping.next`"),
         "expected the cycle in the message: " + holder_error.message);

  // The same cycle through an array and through an option payload.
  auto &tree_decl = w.decls.emplace_back();
  tree_decl.name = "tree";
  tree_decl.definition = std::make_unique<ast::struct_type_def>();
  const auto tree = w.types.user_type(tree_decl, "test", {});
  const auto kids = w.array(tree, 2);
  const auto maybe = w.make_option(tree);
  w.shapes.emplace(tree, semantic::aggregate_shape{
                             .fields = {{.name = "kids", .type = kids},
                                        {.name = "parent", .type = maybe}}});
  const auto tree_error = expect_failure(
      layouts, tree, runtime::layout_error_kind::infinite_size, "tree");
  expect(contains(tree_error.message, "`tree.kids`"),
         "expected the array field in the message: " + tree_error.message);

  // Asking again gives the same answer from the cache.
  expect(layouts.layout(direct).error().message == direct_error.message,
         "expected a cached failure to repeat its message");
}

auto test_cycle_through_an_address_is_finite() -> void {
  auto w = world{};
  auto layouts = runtime::value_layouts{w.types, w.shapes};
  auto &decl = w.decls.emplace_back();
  decl.name = "node";
  decl.definition = std::make_unique<ast::struct_type_def>();
  const auto node = w.types.user_type(decl, "test", {});
  w.shapes.emplace(
      node,
      semantic::aggregate_shape{
          .fields = {{.name = "value", .type = w.scalar("int32")},
                     {.name = "next", .type = w.types.ptr_to(node, true)},
                     {.name = "prev", .type = w.types.ref_to(node, false)}}});
  expect_size_align(layouts, node, 24, 8, "node with pointer links");
}

auto test_oversized_layouts_are_rejected() -> void {
  auto w = world{};
  auto layouts = runtime::value_layouts{w.types, w.shapes};
  const auto i64 = w.scalar("int64");
  // 2^20 * 2^20 * 2^20 * 8 bytes overflows any real address space.
  const auto big =
      w.array(w.array(w.array(i64, uint64_t{1} << 20), uint64_t{1} << 20),
              uint64_t{1} << 20);
  const auto failure = expect_failure(
      layouts, big, runtime::layout_error_kind::too_large, "oversized array");
  expect(contains(failure.message, "larger than the supported maximum"),
         "expected the limit in the message: " + failure.message);
  // A count that does not fit in size_t arithmetic is rejected, not wrapped.
  expect_failure(layouts, w.array(i64, UINT64_MAX),
                 runtime::layout_error_kind::too_large, "huge count");
  // But a large zero-sized array is fine.
  expect_size_align(layouts, w.array(w.scalar("unit"), UINT64_MAX >> 1), 0, 1,
                    "huge array of unit");
}

// ==========================================================================
//  Types the checker produced. These check that generic substitution,
//  qualified names, and nesting reach the layout, which the hand-built types
//  above do not exercise.
// ==========================================================================

auto read_program(std::string_view name) -> std::string {
  static const auto dir =
      cinder::testing::find_test_data_dir("value_layout_test");
  return cinder::testing::load_test_data_file(dir.string(), name);
}

struct checked_fixture {
  cinder::source_manager sources;
  cinder::diagnostic_bag diag{};
  cinder::ast::ptr<cinder::ast::file> ast_file;
  semantic::checked_types checked;
};

auto check_fixture(const std::string &text) -> checked_fixture {
  auto fixture = checked_fixture{};
  const auto file_id = fixture.sources.add_file("sample.cn", text);
  expect(file_id.has_value(), "expected fixture source to register");
  const auto *file = fixture.sources.get(*file_id);
  expect(file != nullptr, "expected registered fixture source");
  auto lexer = cinder::lexer(file->source(), file->id(), fixture.diag);
  auto tokens = lexer.tokenize();
  auto parser = cinder::parser(std::move(tokens), file->id(), fixture.diag);
  fixture.ast_file = parser.parse_file();
  expect(fixture.diag.error_count() == 0, "expected fixture to parse cleanly");
  auto file_has_errors =
      std::vector<bool>(static_cast<size_t>(*file_id) + 1, false);
  const auto parsed_modules = std::vector<semantic::parsed_module>{
      semantic::parsed_module{.file_id = *file_id,
                              .ast_file = fixture.ast_file.get()},
  };
  auto comptime = cinder::comptime_vm::vm_backend{};
  fixture.checked = semantic::check_program(parsed_modules, fixture.diag,
                                            file_has_errors, &comptime);
  expect(fixture.diag.error_count() == 0, "expected fixture to check cleanly");
  return fixture;
}

/// The interned type whose source spelling is `spelling`, e.g.
/// `pair[int8, int64]`.
auto find_type(const checked_fixture &fixture, std::string_view spelling)
    -> semantic::type_id {
  const auto &types = fixture.checked.types;
  for (size_t raw = 0; raw < types.count(); ++raw) {
    const auto id = static_cast<semantic::type_id>(raw);
    if (types.display(id) == spelling) {
      return id;
    }
  }
  cinder::testing::fail("expected the fixture to intern " +
                        std::string(spelling));
}

auto test_checked_generic_nesting() -> void {
  auto fixture = check_fixture(read_program("generic_nested_layout.cn"));
  auto layouts =
      runtime::value_layouts{fixture.checked.types, fixture.checked.shapes};

  // pair[int8, int64]: first at 0, second at 8. A syntactic walk of the
  // declaration sees two type parameters and cannot say that.
  const auto pair = find_type(fixture, "pair[int8, int64]");
  expect_size_align(layouts, pair, 16, 8, "pair[int8, int64]");
  expect((*layouts.layout(pair))->find_field("first")->offset == 0, "first");
  expect((*layouts.layout(pair))->find_field("second")->offset == 8, "second");

  // wrapper: tag@0, inner@8 (ends 24), items@24 (ends 30), wide@32 (ends 48).
  const auto wrapper = find_type(fixture, "wrapper");
  expect_size_align(layouts, wrapper, 48, 16, "wrapper");
  const auto layout = *layouts.layout(wrapper);
  expect(layout->find_field("tag")->offset == 0, "tag at 0");
  expect(layout->find_field("inner")->offset == 8, "inner at 8");
  expect(layout->find_field("inner")->size == 16, "inner is 16 bytes inline");
  expect(layout->find_field("items")->offset == 24, "items at 24");
  expect(layout->find_field("items")->size == 6, "items is 3 * int16");
  expect(layout->find_field("wide")->offset == 32, "wide at 32");

  // option[pair]: the tag, then the 16-byte payload.
  expect_size_align(layouts, find_type(fixture, "option[pair[int8, int64]]"),
                    24, 8, "option[pair[int8, int64]]");
}

auto test_checked_sum_layouts() -> void {
  auto fixture = check_fixture(read_program("sum_layouts.cn"));
  auto layouts =
      runtime::value_layouts{fixture.checked.types, fixture.checked.shapes};
  expect_size_align(layouts, find_type(fixture, "shape"), 16, 8, "shape");
  const auto shape = *layouts.layout(find_type(fixture, "shape"));
  expect(shape->find_variant("circle")->tag == 0, "circle tag");
  expect(shape->find_variant("rect")->tag == 1, "rect tag");
  expect(shape->find_variant("rect")->payload[1].offset == 12, "rect.1 at 12");
  expect(shape->find_variant("empty")->tag == 2, "empty tag");
  expect_size_align(layouts, find_type(fixture, "wide"), 32, 16, "wide");
  expect_size_align(layouts, find_type(fixture, "flags"), 8, 8, "flags");
  expect_size_align(layouts, find_type(fixture, "result[int32, str]"), 24, 8,
                    "result[int32, str]");
}

/// The shapes the runtime writes through `src/runtime/result_out.h` must be the
/// layouts the compiler computes for the matching declarations
/// (`runtime_results.cn` mirrors `src/std/intrinsics.cn`). The runtime cannot
/// ask the compiler at run time, so this test is what ties them together.
auto test_runtime_result_shapes_match_the_compiler() -> void {
  namespace out = runtime::out;
  auto fixture = check_fixture(read_program("runtime_results.cn"));
  auto layouts =
      runtime::value_layouts{fixture.checked.types, fixture.checked.shapes};

  expect(out::k_result_tag_size == runtime::k_sum_tag_size,
         "expected the runtime to use the compiler's tag size");
  expect(out::k_descriptor_size == runtime::k_descriptor_size &&
             out::k_descriptor_data_offset == runtime::k_descriptor_data_offset,
         "expected the runtime to use the compiler's descriptor layout");

  expect_size_align(layouts, find_type(fixture, "raw_fd"), out::k_raw_fd_size,
                    8, "raw_fd");
  expect_size_align(layouts, find_type(fixture, "io_errno"),
                    out::k_io_errno_size, 4, "io_errno");
  const auto find = *layouts.layout(find_type(fixture, "find_result"));
  expect(find->size == out::k_find_result_size &&
             find->find_field("found")->offset == 0 &&
             find->find_field("found")->size == 1 &&
             find->find_field("pos")->offset == out::k_find_pos_offset,
         "expected find_result to be a flag byte then a word at 8");

  // Each `result[T, io_errno]`: the payload size the runtime assumes, and
  // that both variants place their payload at the same offset.
  const auto expect_result = [&](std::string_view spelling,
                                 size_t payload_size) {
    const auto name = std::string(spelling);
    const auto layout = *layouts.layout(find_type(fixture, spelling));
    expect(layout->size == out::result_size(payload_size) && layout->align == 8,
           name + " has size " + std::to_string(layout->size) + ", expected " +
               std::to_string(out::result_size(payload_size)));
    expect(layout->tag_offset == 0 &&
               layout->payload_offset == out::k_result_payload_offset,
           name + " places its payload at the end of the tag");
    const auto *ok = layout->find_variant("ok");
    const auto *err = layout->find_variant("err");
    expect(ok != nullptr && err != nullptr, name + " has ok and err");
    expect(ok->tag == static_cast<int64_t>(out::k_result_ok_tag) &&
               err->tag == static_cast<int64_t>(out::k_result_err_tag),
           name + " numbers ok 0 and err 1");
    expect(ok->payload.size() == 1 && ok->payload[0].size == payload_size &&
               ok->payload[0].offset == out::k_result_payload_offset,
           name + " ok payload");
    expect(err->payload.size() == 1 &&
               err->payload[0].size == out::k_io_errno_size &&
               err->payload[0].offset == out::k_result_payload_offset,
           name + " err payload");
  };
  expect_result("result[raw_fd, io_errno]", out::k_raw_fd_size);
  expect_result("result[usize, io_errno]", 8);
  expect_result("result[raw_pipe, io_errno]", 16);
  expect_result("result[str, io_errno]", out::k_descriptor_size);
  expect_result("result[uname_raw, io_errno]", 5 * out::k_descriptor_size);
  expect_result("result[libc_version, io_errno]", 2 * out::k_descriptor_size);
  expect_result("result[winver_raw, io_errno]", 32);
  // `unit` takes no bytes, but the error payload still sets the size.
  expect_result("result[unit, io_errno]", 0);
  expect(out::k_result_unit_size == out::result_size(0) &&
             out::k_result_word_size == out::result_size(8) &&
             out::k_result_str_size == out::result_size(16),
         "expected the named result sizes to follow result_size");

  // `winver_raw`: four words of 4 bytes, then the string at 16.
  const auto winver = *layouts.layout(find_type(fixture, "winver_raw"));
  expect(winver->find_field("major")->offset == 0 &&
             winver->find_field("minor")->offset == 4 &&
             winver->find_field("build")->offset == 8 &&
             winver->find_field("platform_id")->offset == 12 &&
             winver->find_field("csd_version")->offset == 16,
         "expected winver_raw fields at 0, 4, 8, 12 and 16");
  const auto pipe = *layouts.layout(find_type(fixture, "raw_pipe"));
  expect(pipe->find_field("read")->offset == 0 &&
             pipe->find_field("write")->offset == 8,
         "expected raw_pipe ends at 0 and 8");
}

auto test_checked_packed_and_zero_sized() -> void {
  auto fixture = check_fixture(read_program("packed_and_zero_sized.cn"));
  auto layouts =
      runtime::value_layouts{fixture.checked.types, fixture.checked.shapes};
  expect_size_align(layouts, find_type(fixture, "mixed"), 6, 1, "mixed");
  expect_size_align(layouts, find_type(fixture, "marker"), 4, 4, "marker");
  const auto marker = *layouts.layout(find_type(fixture, "marker"));
  expect(marker->find_field("nothing")->size == 0 &&
             marker->find_field("n")->offset == 0,
         "expected the unit field to take no space");
  // holder: m@0 (6, align 1), k@8 (4, align 4), rest@12 (0) -> 12.
  const auto holder = find_type(fixture, "holder");
  expect_size_align(layouts, holder, 12, 4, "holder");
  expect((*layouts.layout(holder))->find_field("k")->offset == 8, "k at 8");
}

auto test_checked_pointer_cycle_is_finite() -> void {
  auto fixture = check_fixture(read_program("pointer_cycle_is_finite.cn"));
  auto layouts =
      runtime::value_layouts{fixture.checked.types, fixture.checked.shapes};
  expect_size_align(layouts, find_type(fixture, "node"), 16, 8, "node");
  // slot: head is option[*mut node] (tag + pointer = 16), count follows.
  expect_size_align(layouts, find_type(fixture, "slot"), 24, 8, "slot");
}

auto test_checked_inline_cycles() -> void {
  auto fixture = check_fixture(read_program("inline_cycle.cn"));
  auto layouts =
      runtime::value_layouts{fixture.checked.types, fixture.checked.shapes};
  const auto ping =
      expect_failure(layouts, find_type(fixture, "ping"),
                     runtime::layout_error_kind::infinite_size, "ping");
  expect(contains(ping.message, "`ping.next` is `pong`") &&
             contains(ping.message, "`pong.back` is `ping`"),
         "expected both fields: " + ping.message);
  const auto direct =
      expect_failure(layouts, find_type(fixture, "direct"),
                     runtime::layout_error_kind::infinite_size, "direct");
  expect(contains(direct.message, "`direct.inner` is `direct`"),
         "expected the field: " + direct.message);
  const auto through = expect_failure(
      layouts, find_type(fixture, "through_option"),
      runtime::layout_error_kind::infinite_size, "through_option");
  expect(contains(through.message, "`through_option.rest`") &&
             contains(through.message, "some.0"),
         "expected the option payload on the path: " + through.message);
}

} // namespace

auto main() -> int {
  test_scalar_layouts();
  test_address_descriptor_and_handle_layouts();
  test_reserved_builtins_fail_closed();
  test_struct_padding_and_nesting();
  test_packed_struct_has_no_padding();
  test_tuple_and_int128_alignment();
  test_array_stride_and_zero_sized_arrays();
  test_sum_layouts();
  test_refinement_lays_out_as_its_base();
  test_inline_cycles_are_rejected_with_a_path();
  test_cycle_through_an_address_is_finite();
  test_oversized_layouts_are_rejected();
  test_checked_generic_nesting();
  test_checked_sum_layouts();
  test_runtime_result_shapes_match_the_compiler();
  test_checked_packed_and_zero_sized();
  test_checked_pointer_cycle_is_finite();
  test_checked_inline_cycles();
  return 0;
}
