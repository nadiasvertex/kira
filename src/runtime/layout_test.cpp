#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "src/parser/diagnostic.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/runtime/layout.h"
#include "src/semantic/analysis.h"
#include "src/semantic/check.h"
#include "src/semantic/types.h"
#include "src/testing/test_assert.h"

namespace {

using kira::testing::expect;
namespace runtime = kira::runtime;
namespace semantic = kira::semantic;

// Mirrors src/bytecode_compiler/compile_test.cpp's own fixture helper: lex
// -> parse -> check one source string, then hand back the checked types so
// this test can look up the type_id for a declared struct/sum type by name.
struct checked_fixture {
  kira::source_manager sources;
  kira::diagnostic_bag diag{};
  kira::ast::ptr<kira::ast::file> ast_file;
  semantic::checked_types checked;
};

auto check_fixture(const std::string &text) -> checked_fixture {
  auto fixture = checked_fixture{};
  const auto file_id = fixture.sources.add_file("sample.kira", text);
  expect(file_id.has_value(), "expected fixture source to register");

  const auto *file = fixture.sources.get(*file_id);
  expect(file != nullptr, "expected registered fixture source");

  auto lexer = kira::lexer(file->source(), file->id(), fixture.diag);
  auto tokens = lexer.tokenize();
  auto parser = kira::parser(std::move(tokens), file->id(), fixture.diag);
  fixture.ast_file = parser.parse_file();
  expect(fixture.diag.error_count() == 0, "expected fixture to parse cleanly");

  auto file_has_errors =
      std::vector<bool>(static_cast<size_t>(*file_id) + 1, false);
  const auto parsed_modules = std::vector<semantic::parsed_module>{
      semantic::parsed_module{.file_id = *file_id,
                              .ast_file = fixture.ast_file.get()},
  };
  fixture.checked =
      semantic::check_program(parsed_modules, fixture.diag, file_has_errors);
  expect(fixture.diag.error_count() == 0, "expected fixture to check cleanly");
  return fixture;
}

/// Finds the `type_id` for a user type named `name` by scanning every
/// `hir_local_ref`-free path this test needs: the checker's own type
/// declarations are reachable through `ast_file`'s items, resolved through
/// `type_table::user_type` — simplest here is to re-resolve the name the
/// same way a struct-literal expression's own type ends up recorded, via
/// `node_types` on the file's first `def`'s body. Instead, since every
/// fixture below declares its type before a `def main` that constructs it,
/// look up `main`'s recorded expression type directly.
auto struct_or_sum_type_of_main_return(const checked_fixture &fixture)
    -> semantic::type_id {
  for (const auto &item : fixture.ast_file->items) {
    if (item->kind != kira::ast::node_kind::func_decl) {
      continue;
    }
    const auto &fn = dynamic_cast<const kira::ast::func_decl &>(*item);
    if (fn.name != "main" || fn.return_type == nullptr) {
      continue;
    }
    const auto found = fixture.checked.node_types.find(fn.return_type.get());
    expect(found != fixture.checked.node_types.end(),
           "expected main's declared return type to be recorded");
    return found->second;
  }
  kira::testing::fail("expected fixture to declare def main");
}

auto test_struct_field_slots_match_declaration_order() -> void {
  auto fixture = check_fixture(
      "module sample\n"
      "type point = { pub x: int32, pub y: int32, pub z: int32 }\n"
      "def main() -> point:\n"
      "    return { x: 1, y: 2, z: 3 }\n");
  const auto id = struct_or_sum_type_of_main_return(fixture);
  const auto names = runtime::struct_field_names(fixture.checked.types, id);
  expect(names.size() == 3, "expected three struct fields");
  expect(names[0] == "x" && names[1] == "y" && names[2] == "z",
         "expected struct fields in declaration order");
  expect(runtime::struct_field_slot(fixture.checked.types, id, "y") == 1,
         "expected field y at slot 1");
  expect(!runtime::struct_field_slot(fixture.checked.types, id, "nope")
              .has_value(),
         "expected an unknown field name to resolve to nullopt");
}

auto test_sum_variant_tags_and_payload_arity() -> void {
  auto fixture = check_fixture("module sample\n"
                               "type shape = @circle(float64) | "
                               "@rectangle(float64, float64) | @empty\n"
                               "def main() -> shape:\n"
                               "    return @circle(1.0)\n");
  const auto id = struct_or_sum_type_of_main_return(fixture);
  const auto names = runtime::sum_variant_names(fixture.checked.types, id);
  expect(names.size() == 3, "expected three variants");
  expect(names[0] == "circle" && names[1] == "rectangle" && names[2] == "empty",
         "expected variants in declaration order");
  expect(runtime::sum_variant_tag(fixture.checked.types, id, "rectangle") == 1,
         "expected rectangle to have tag 1");
  expect(runtime::sum_variant_payload_slots(fixture.checked.types, id,
                                            "circle") == 1,
         "expected circle to have one payload slot");
  expect(runtime::sum_variant_payload_slots(fixture.checked.types, id,
                                            "rectangle") == 2,
         "expected rectangle to have two payload slots");
  expect(runtime::sum_variant_payload_slots(fixture.checked.types, id,
                                            "empty") == 0,
         "expected a unit variant to have zero payload slots");
  expect(runtime::sum_max_payload_slots(fixture.checked.types, id) == 2,
         "expected the widest variant (rectangle) to size the payload area");
}

auto test_list_reserve_slot_grows_and_preserves_existing_elements() -> void {
  uint64_t header[3] = {0, 0, 0};

  auto *slot0 = runtime::list_reserve_slot(header);
  expect(header[0] == 1, "expected len to become 1 after the first reserve");
  expect(header[1] == 4, "expected the first growth to start at capacity 4");
  *slot0 = 10;

  auto *slot1 = runtime::list_reserve_slot(header);
  auto *slot2 = runtime::list_reserve_slot(header);
  auto *slot3 = runtime::list_reserve_slot(header);
  *slot1 = 20;
  *slot2 = 30;
  *slot3 = 40;
  expect(header[0] == 4, "expected len == 4 after four reserves");
  expect(header[1] == 4, "expected cap to still be 4 (exactly filled)");

  // A fifth reserve exceeds capacity 4, forcing growth to 8 and copying
  // every existing element across.
  auto *slot4 = runtime::list_reserve_slot(header);
  *slot4 = 50;
  expect(header[0] == 5, "expected len == 5 after the growth-triggering push");
  expect(header[1] == 8, "expected capacity to double to 8");
  auto *data = reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(header[2]));
  expect(data[0] == 10 && data[1] == 20 && data[2] == 30 && data[3] == 40 &&
             data[4] == 50,
         "expected every element to survive the growth copy in order");
}

} // namespace

auto main() -> int {
  try {
    test_struct_field_slots_match_declaration_order();
    test_sum_variant_tags_and_payload_arity();
    test_list_reserve_slot_grows_and_preserves_existing_elements();
  } catch (const std::exception &ex) {
    std::cerr << "layout_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
