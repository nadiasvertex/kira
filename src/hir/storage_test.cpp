#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/comptime_vm/backend.h"
#include "src/hir/abi.h"
#include "src/hir/lower.h"
#include "src/hir/nodes.h"
#include "src/hir/storage.h"
#include "src/hir/storage_lower.h"
#include "src/hir/storage_verify.h"
#include "src/parser/diagnostic.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/runtime/value_layout.h"
#include "src/semantic/analysis.h"
#include "src/semantic/check.h"
#include "src/semantic/checker_results.h"
#include "src/testing/stdlib_fixtures.h"
#include "src/testing/test_assert.h"
#include "src/testing/test_data.h"

namespace {

using cinder::testing::expect;
using cinder::testing::fail;
namespace hir = cinder::hir;
namespace runtime = cinder::runtime;
namespace semantic = cinder::semantic;

auto read_program(std::string_view name) -> std::string {
  static const auto dir =
      cinder::testing::find_test_data_dir("hir_storage_test");
  return cinder::testing::load_test_data_file(dir.string(), name);
}

/// A checked program lowered to HIR and then to storage form.
struct fixture {
  cinder::source_manager sources;
  cinder::diagnostic_bag diag{};
  cinder::testing::parsed_stdlib stdlib;
  cinder::ast::ptr<cinder::ast::file> ast_file;
  semantic::checked_types checked;
  hir::ptr<hir::hir_module> module;
  std::unique_ptr<runtime::value_layouts> layouts;
  std::unique_ptr<hir::storage_env> env;
  hir::storage_lowering lowering;

  [[nodiscard]] auto function(std::string_view name) const
      -> hir::storage_function {
    const auto *found = lowering.module.find(name);
    if (found == nullptr) {
      std::cerr << "missing function: " << name << "\n";
      fail("expected the function to lower to storage form");
    }
    return *found;
  }

  [[nodiscard]] auto hir_function(std::string_view name) const
      -> const hir::hir_function & {
    for (const auto &candidate : module->functions) {
      if (candidate->name == name) {
        return *candidate;
      }
    }
    fail("expected the function in the HIR module");
  }

  [[nodiscard]] auto refusal(std::string_view name) const
      -> const hir::storage_lower_error * {
    for (const auto &refused : lowering.refused) {
      if (refused.name == name) {
        return &refused.error;
      }
    }
    return nullptr;
  }

  [[nodiscard]] auto builtin(std::string_view name) -> semantic::type_id {
    return checked.types.builtin(name);
  }
};

auto build(const std::string &text) -> std::unique_ptr<fixture> {
  auto out = std::make_unique<fixture>();
  out->stdlib = cinder::testing::parse_stdlib(out->sources, out->diag);
  const auto file_id = out->sources.add_file("sample.cn", text);
  expect(file_id.has_value(), "expected fixture source to register");
  const auto *file = out->sources.get(*file_id);
  auto lexer = cinder::lexer(file->source(), file->id(), out->diag);
  auto tokens = lexer.tokenize();
  auto parser = cinder::parser(std::move(tokens), file->id(), out->diag);
  out->ast_file = parser.parse_file();
  expect(out->diag.error_count() == 0, "expected fixture to parse cleanly");

  auto file_has_errors =
      std::vector<bool>(static_cast<size_t>(*file_id) + 1, false);
  auto parsed_modules = out->stdlib.modules;
  parsed_modules.push_back(semantic::parsed_module{
      .file_id = *file_id, .ast_file = out->ast_file.get()});
  auto comptime = cinder::comptime_vm::vm_backend{};
  out->checked = semantic::check_program(parsed_modules, out->diag,
                                         file_has_errors, &comptime);
  if (out->diag.error_count() != 0) {
    std::cerr << cinder::diagnostic_renderer(out->sources, false)
                     .render_all(out->diag);
    fail("expected fixture to check cleanly");
  }
  auto lowered = hir::lower_module(*out->ast_file, "sample", out->checked);
  expect(lowered.has_value(), "expected the fixture to lower to HIR");
  out->module = std::move(*lowered);
  out->layouts = std::make_unique<runtime::value_layouts>(out->checked.types,
                                                          out->checked.shapes);
  out->env = std::make_unique<hir::storage_env>(
      hir::storage_env{out->checked, *out->layouts});
  out->lowering = hir::lower_to_storage(*out->module, *out->env);
  return out;
}

/// The operations of `function`, one string per operation, without indices.
auto op_lines(const fixture &f, const hir::storage_function &function)
    -> std::vector<std::string> {
  auto lines = std::vector<std::string>{};
  auto stream = std::istringstream(hir::dump(function, f.checked.types));
  auto line = std::string{};
  while (std::getline(stream, line)) {
    const auto colon = line.find(": ");
    if (line.starts_with("  ") && colon != std::string::npos && colon > 2 &&
        line.find_first_not_of("0123456789", 2) == colon) {
      lines.push_back(line.substr(colon + 2));
    }
  }
  return lines;
}

auto expect_ops(const fixture &f, std::string_view name,
                const std::vector<std::string> &expected) -> void {
  const auto actual = op_lines(f, f.function(name));
  if (actual != expected) {
    std::cerr << "storage form of `" << name << "` differs.\nexpected:\n";
    for (const auto &line : expected) {
      std::cerr << "  " << line << "\n";
    }
    std::cerr << "actual:\n";
    for (const auto &line : actual) {
      std::cerr << "  " << line << "\n";
    }
    fail("expected the storage form to match");
  }
}

/// Index of the `nth` operation of `kind`.
auto find_op(const hir::storage_function &function, hir::op_kind kind,
             size_t nth = 0) -> size_t {
  for (size_t i = 0; i < function.ops.size(); ++i) {
    if (function.ops[i].kind == kind && nth-- == 0) {
      return i;
    }
  }
  fail("expected the function to contain that operation");
}

auto expect_verifies(const fixture &f, const hir::storage_function &function,
                     std::string_view label,
                     const hir::verify_options &options = {}) -> void {
  const auto verdict = hir::verify(function, *f.env, &f.lowering.module, options);
  if (!verdict.has_value()) {
    std::cerr << label << ": unexpected "
              << hir::verify_error_kind_name(verdict.error().kind) << ": "
              << verdict.error().message << "\n";
    fail("expected the storage form to verify");
  }
}

auto expect_rejects(const fixture &f, const hir::storage_function &function,
                    hir::verify_error_kind kind, std::string_view label,
                    const hir::verify_options &options = {},
                    std::string_view message_has = {}) -> void {
  const auto verdict = hir::verify(function, *f.env, &f.lowering.module, options);
  if (verdict.has_value()) {
    std::cerr << label << ": the verifier accepted a corrupted form\n";
    fail("expected the verifier to reject the corrupted form");
  }
  if (!message_has.empty() &&
      verdict.error().message.find(message_has) == std::string::npos) {
    std::cerr << label << ": expected the message to contain `" << message_has
              << "` but got: " << verdict.error().message << "\n";
    fail("expected a different verifier message");
  }
  if (verdict.error().kind != kind) {
    std::cerr << label << ": expected " << hir::verify_error_kind_name(kind)
              << " but got " << hir::verify_error_kind_name(verdict.error().kind)
              << ": " << verdict.error().message << "\n";
    fail("expected a different verifier rule to fire");
  }
}

auto add_slot(hir::storage_function &function, hir::storage_class cls,
              semantic::type_id type) -> hir::storage_id {
  const auto id = static_cast<hir::storage_id>(function.slots.size());
  function.slots.push_back(hir::storage_slot{.id = id,
                                             .cls = cls,
                                             .type = type,
                                             .owner = id});
  return id;
}

auto place_at(hir::storage_id base, semantic::type_id type,
              std::vector<hir::projection> steps = {}) -> hir::place {
  return hir::place{.base = base, .steps = std::move(steps), .type = type};
}

// ==========================================================================
//  ABI classification
// ==========================================================================

auto test_abi_classes_follow_layout() -> void {
  auto f = build(read_program("forms.cn"));
  const auto pair = f->function("make_pair").slots[0].type;
  const auto classify = [&](semantic::type_id type) {
    const auto value = hir::classify_value(*f->layouts, type);
    if (!value.has_value()) {
      std::cerr << f->checked.types.display(type) << ": "
                << value.error().message << "\n";
    }
    expect(value.has_value(), "expected the type to classify");
    return *value;
  };
  expect(classify(f->builtin("int32")).cls == hir::pass_class::scalar,
         "expected int32 to be a scalar");
  expect(classify(f->builtin("int128")).cls == hir::pass_class::scalar,
         "expected int128 to be a scalar even though it is 16 bytes");
  expect(classify(f->builtin("unit")).cls == hir::pass_class::zero_sized,
         "expected unit to be zero-sized");
  expect(classify(f->builtin("str")).cls == hir::pass_class::descriptor,
         "expected str to be a descriptor");
  expect(classify(pair).cls == hir::pass_class::aggregate,
         "expected a struct to be an aggregate");
  expect(classify(pair).size == 8, "expected pair to be 8 bytes");
  const auto reference = f->function("first").slots[0].type;
  expect(classify(reference).cls == hir::pass_class::address,
         "expected &pair to be an address");
  const auto closure = f->function("apply").slots[1].type;
  expect(hir::classify_value(*f->layouts, f->checked.types.entry(closure).result)
                 .value()
                 .cls == hir::pass_class::aggregate,
         "expected a fn value to be an aggregate");
}

auto test_signature_operands_hide_the_result_address() -> void {
  auto f = build(read_program("forms.cn"));
  const auto &lowered = f->function("make_pair");
  expect(lowered.signature.returns_through_address(),
         "expected an aggregate result to be built in caller storage");
  const auto operands = lowered.signature.operands();
  expect(operands.size() == 2, "expected the result address and one argument");
  expect(operands[0].role == hir::abi_operand_role::result_address,
         "expected the result address first");
  expect(operands[1].role == hir::abi_operand_role::argument &&
             operands[1].source_index == 0 && !operands[1].by_address,
         "expected the scalar argument in a register");

  const auto direct = f->function("bind").signature.operands();
  expect(direct.size() == 1 && direct[0].source_index == 0,
         "expected a scalar result to need no hidden operand");

  // Zero-sized parameters occupy no operand but keep their source position.
  const auto unit = f->builtin("unit");
  const auto int32 = f->builtin("int32");
  const auto params = std::vector<semantic::type_id>{unit, int32};
  const auto signature = hir::classify_signature(*f->layouts, params, int32);
  expect(signature.has_value(), "expected the signature to classify");
  const auto skipped = signature->operands();
  expect(skipped.size() == 1 && skipped[0].source_index == 1,
         "expected the unit parameter to vanish and the next to keep index 1");
  expect(signature->params.size() == 2, "expected both source parameters");
}

auto test_signature_agreement_ignores_type_ids_but_not_shape() -> void {
  auto f = build(read_program("forms.cn"));
  const auto int32 = f->builtin("int32");
  const auto int64 = f->builtin("int64");
  auto narrow = std::vector<semantic::type_id>{int32};
  auto wide = std::vector<semantic::type_id>{int64};
  const auto a = hir::classify_signature(*f->layouts, narrow, int32).value();
  const auto b = hir::classify_signature(*f->layouts, wide, int32).value();
  expect(!a.agrees_with(b), "expected int32 and int64 parameters to disagree");
  expect(a.agrees_with(a), "expected a signature to agree with itself");

  const auto first = f->function("first").slots[0].type;
  const auto ref_params = std::vector<semantic::type_id>{first};
  const auto c = hir::classify_signature(*f->layouts, ref_params, int32).value();
  expect(c.agrees_with(c) && !a.agrees_with(c),
         "expected an address and a scalar to disagree");
}

auto test_function_value_and_function_share_a_signature() -> void {
  auto f = build(read_program("forms.cn"));
  const auto fn_ref = f->function("apply").slots[1].type;
  const auto fn_type = f->checked.types.entry(fn_ref).result;
  const auto indirect =
      hir::classify_fn_type(*f->layouts, f->checked.types, fn_type);
  expect(indirect.has_value(), "expected the fn type to classify");
  const auto &direct = f->function("make_pair").signature;
  expect(indirect->agrees_with(direct),
         "expected `fn(int32) -> pair` to match `make_pair`");
  expect(hir::classify_fn_type(*f->layouts, f->checked.types, f->builtin("int32"))
             .has_value() == false,
         "expected a non-function type to be refused");
}

auto test_synthesized_functions_classify_with_handle_results() -> void {
  auto f = build(read_program("forms.cn"));
  const auto signature =
      hir::classify_function(*f->layouts, f->hir_function("triple"));
  expect(signature.has_value(), "expected the generator to classify");
  expect(signature->result.cls == hir::pass_class::handle,
         "expected a generator to return a handle, not an aggregate");
  expect(!signature->returns_through_address(),
         "expected a handle result to travel in a register");
}

// ==========================================================================
//  Lowering
// ==========================================================================

auto test_constructor_is_built_in_the_result_bytes() -> void {
  auto f = build(read_program("forms.cn"));
  expect_ops(*f, "make_pair",
             {"compute s0.a = copy s1", "compute s0.b = binary + s1, #1",
              "return"});
  expect_ops(*f, "make_tuple",
             {"compute s0.0 = copy s1", "compute s0.1 = binary + s1, #1",
              "return"});
  expect_ops(*f, "make_array",
             {"compute s0[0] = copy s1", "compute s0[1] = copy s1",
              "compute s0[2] = const 0", "return"});
}

auto test_nested_call_result_is_forwarded_into_the_field() -> void {
  auto f = build(read_program("forms.cn"));
  expect_ops(*f, "make_line",
             {"call s0.from = make_pair(s1)", "compute s0.to.a = const 0",
              "compute s0.to.b = copy s1", "return"});
}

auto test_returned_call_receives_the_result_address() -> void {
  auto f = build(read_program("forms.cn"));
  expect_ops(*f, "forward", {"call s0 = make_pair(s1)", "return"});
  expect_ops(*f, "bind", {"call s1 = make_pair(s0)", "return s1.b"});
}

auto test_sum_is_published_after_its_payload() -> void {
  auto f = build(read_program("forms.cn"));
  expect_ops(*f, "make_rect",
             {"compute s0.@rect.0 = copy s1",
              "compute s0.@rect.1 = binary + s1, #1", "publish s0 @rect",
              "return"});
  expect_ops(*f, "make_empty", {"publish s0 @empty", "return"});
  expect_ops(*f, "make_some",
             {"compute s0.@some.0 = copy s1", "publish s0 @some", "return"});
  // A sum nested in a struct publishes in place, before later fields.
  expect_ops(*f, "make_framed",
             {"compute s0.kind.@circle.0 = copy s1",
              "publish s0.kind @circle", "compute s0.id = copy s1", "return"});
}

auto test_aggregate_arguments_use_outgoing_slots() -> void {
  auto f = build(read_program("forms.cn"));
  expect_ops(*f, "pass_fresh",
             {"call s2 = make_pair(s0)", "call s1 = take_pair(move s2, s0)",
              "dead s2", "return s1"});
  const auto lowered = f->function("pass_fresh");
  expect(lowered.slots[2].cls == hir::storage_class::outgoing_argument,
         "expected the aggregate argument in an outgoing slot");
  // A non-copy local is transferred by bytes into its own outgoing slot.
  expect_ops(*f, "pass_local",
             {"move s2 <- s0", "call s1 = take_pair(move s2, #1)", "dead s2",
              "return s1"});
}

auto test_copy_classification_chooses_copy_or_move() -> void {
  auto f = build(read_program("forms.cn"));
  expect_ops(*f, "pass_shape",
             {"copy s2 <- s0", "call s1 = use_shape(move s2)", "dead s2",
              "return s1"});
  expect(f->env->is_copy(f->function("pass_shape").slots[0].type),
         "expected a sum of copy payloads to be copy");
  expect(!f->env->is_copy(f->function("pass_local").slots[0].type),
         "expected a plain struct to move");
}

auto test_discarded_result_gets_temporary_storage() -> void {
  auto f = build(read_program("forms.cn"));
  expect_ops(*f, "discard",
             {"call s1 = make_pair(s0)", "dead s1", "return s0"});
}

auto test_replacement_is_built_apart_from_the_target() -> void {
  auto f = build(read_program("forms.cn"));
  expect_ops(*f, "reassign",
             {"call s2 = make_pair(s1)", "call s3 = make_pair(s2.b)",
              "move s2 <- s3", "dead s3", "move s0 <- s2", "return"});
  // The callee borrows the old value while the new one is built elsewhere.
  expect_ops(*f, "self_bump",
             {"call s2 = make_pair(s1)", "call s3 = bump(&s2)",
              "move s2 <- s3", "dead s3", "move s0 <- s2", "return"});
}

auto test_projection_through_a_reference_dereferences() -> void {
  auto f = build(read_program("forms.cn"));
  expect_ops(*f, "first", {"return s0.*.a"});
  expect_ops(*f, "bump",
             {"compute s0.a = binary + s1.*.a, #1",
              "compute s0.b = copy s1.*.b", "return"});
  expect_ops(*f, "borrow",
             {"call s1 = make_pair(s0)", "call s2 = first(&s1)", "return s2"});
}

auto test_indirect_call_goes_through_the_function_value() -> void {
  auto f = build(read_program("forms.cn"));
  expect_ops(*f, "apply", {"call s0 = (s1.*)(s2)", "return"});
  const auto lowered = f->function("apply");
  const auto &call = lowered.ops[find_op(lowered, hir::op_kind::call_into)];
  expect(!call.callee.is_direct(), "expected an indirect call");
  expect(call.signature.returns_through_address(),
         "expected the aggregate result to go through the result address");
}

auto test_scalar_and_unit_functions() -> void {
  auto f = build(read_program("forms.cn"));
  expect_ops(*f, "sum_scalars",
             {"compute s2 = binary + s0, s1",
              "compute s3 = binary * s2, #2", "return s3"});
  expect_ops(*f, "unit_result", {"call s1 = make_pair(s0)", "return"});
}

auto test_unsupported_constructs_are_refused_by_name() -> void {
  auto f = build(read_program("forms.cn"));
  const auto *matching = f->refusal("matching");
  expect(matching != nullptr, "expected `match` to be refused");
  expect(matching->kind ==
             hir::storage_lower_error_kind::unsupported_construct,
         "expected an unsupported-construct refusal");
  expect(matching->message.find("`match`") != std::string::npos,
         "expected the refusal to name the construct");
  const auto *shout = f->refusal("shout");
  expect(shout != nullptr && shout->message.find("str") != std::string::npos,
         "expected a string literal to be refused");
  expect(f->refusal("crewed") != nullptr, "expected a crew to be refused");
  expect(f->refusal("racing") == nullptr,
         "expected waiting on a set of tasks to lower");
  expect(f->refusal("triple") == nullptr,
         "expected a task that waits on nothing to lower");
  expect(f->lowering.module.find("matching") == nullptr,
         "expected a refused function to be absent, not half lowered");
}

auto test_every_lowered_function_verifies() -> void {
  auto f = build(read_program("forms.cn"));
  expect(!f->lowering.module.functions.empty(), "expected lowered functions");
  const auto verdict = hir::verify(f->lowering.module, *f->env);
  if (!verdict.has_value()) {
    std::cerr << verdict.error().function << " op " << verdict.error().op_index
              << ": " << verdict.error().message << "\n";
  }
  expect(verdict.has_value(), "expected every lowered function to verify");
}

auto test_dump_is_stable() -> void {
  auto f = build(read_program("forms.cn"));
  const auto first = hir::dump(f->lowering.module, f->checked.types);
  const auto again = hir::dump(f->lowering.module, f->checked.types);
  expect(first == again, "expected the dump to be deterministic");
  expect(first.find("fn make_pair (result@, scalar) -> aggregate") !=
             std::string::npos,
         "expected the signature in the dump");
  expect(first.find("s0 result _: pair") != std::string::npos,
         "expected the slots in the dump");
}

// ==========================================================================
//  Types that need a drop
// ==========================================================================

auto test_droppable_values_lower_and_verify() -> void {
  auto f = build(read_program("drops.cn"));
  expect(f->lowering.refused.empty(),
         "expected every function over droppable values to lower");
  const auto verdict = hir::verify(f->lowering.module, *f->env);
  if (!verdict.has_value()) {
    std::cerr << verdict.error().function << ": " << verdict.error().message
              << "\n";
  }
  expect(verdict.has_value(),
         "expected the drops to account for every droppable value");
}

auto test_scope_exit_drop_is_a_marked_call() -> void {
  auto f = build(read_program("drops.cn"));
  expect_ops(*f, "forget_noisy",
             {"call s1 = make_noisy(s0)", "compute s2 = copy s0",
              "call.drop s3 = noisy::drop(&s1)", "dead s3", "return s2"});
  // The value is dropped where it stands, after the value `return` carries
  // has been computed.
  expect_ops(*f, "read_through",
             {"call s1 = make_noisy(s0)", "call s2 = read_id(&s1)",
              "call.drop s3 = noisy::drop(&s1)", "dead s3", "return s2"});
  // A parameter the function owns is dropped by the function.
  expect_ops(*f, "take_noisy",
             {"compute s1 = const 0", "call.drop s2 = noisy::drop(&s0)",
              "dead s2", "return s1"});
  // A parameter passed on is moved into the callee, which drops it.
  expect_ops(*f, "pass_noisy",
             {"move s2 <- s0", "call s1 = take_noisy(move s2)", "dead s2",
              "return s1"});
  expect(!f->env->is_copy(f->function("pass_noisy").slots[0].type) &&
             f->env->needs_drop(f->function("pass_noisy").slots[0].type),
         "expected a type with a drop to move and to need a drop");
  // Only synthesized drops carry the mark.
  const auto lowered = f->function("read_through");
  expect(!lowered.ops[find_op(lowered, hir::op_kind::call_into, 0)].is_drop &&
             !lowered.ops[find_op(lowered, hir::op_kind::call_into, 1)].is_drop &&
             lowered.ops[find_op(lowered, hir::op_kind::call_into, 2)].is_drop,
         "expected exactly the scheduled drop to be marked");
}

auto test_discarded_temporary_is_dropped() -> void {
  auto f = build(read_program("drops.cn"));
  expect_ops(*f, "discard_noisy",
             {"call s1 = make_noisy(s0)", "call.drop s2 = noisy::drop(&s1)",
              "dead s2", "return s0"});
}

auto test_replacement_drops_the_old_value_first() -> void {
  auto f = build(read_program("drops.cn"));
  // The new value is built first, the old one dropped, then it is stored.
  expect_ops(*f, "replace_noisy",
             {"call s2 = make_noisy(s1)", "compute s4 = binary + s1, #1",
              "call s3 = make_noisy(s4)", "dead s4",
              "call.drop s5 = noisy::drop(&s2)", "dead s5", "move s2 <- s3",
              "move s0 <- s2", "return"});
  // Replacing one field drops only that field.
  expect_ops(*f, "replace_second",
             {"call s1 = make_holder(s0)", "compute s3 = binary + s0, #2",
              "call s2 = make_noisy(s3)", "dead s3",
              "call.drop s4 = noisy::drop(&s1.second)", "dead s4",
              "move s1.second <- s2", "compute s5 = copy s0",
              "call.drop s6 = noisy::drop(&s1.first)", "dead s6",
              "call.drop s7 = noisy::drop(&s1.second)", "dead s7",
              "return s5"});
}

auto test_fields_drop_in_order_and_moved_fields_are_skipped() -> void {
  auto f = build(read_program("drops.cn"));
  expect_ops(*f, "drop_holder",
             {"call s1 = make_holder(s0)", "compute s2 = copy s0",
              "call.drop s3 = noisy::drop(&s1.first)", "dead s3",
              "call.drop s4 = noisy::drop(&s1.second)", "dead s4",
              "return s2"});
  // `h.first` leaves by move, so only `h.second` is dropped.
  expect_ops(*f, "take_first",
             {"call s2 = make_holder(s1)", "move s3 <- s2.first",
              "call.drop s4 = noisy::drop(&s2.second)", "dead s4",
              "move s0 <- s3", "return"});
  // A parameter holds droppable fields it owns.
  expect_ops(*f, "pass_holder",
             {"compute s1 = const 0", "call.drop s2 = noisy::drop(&s0.first)",
              "dead s2", "call.drop s3 = noisy::drop(&s0.second)", "dead s3",
              "return s1"});
  // Built in an outgoing slot and moved into the callee, which drops it.
  expect_ops(*f, "forward_holder",
             {"call s2 = make_holder(s0)", "call s1 = pass_holder(move s2)",
              "dead s2", "return s1"});
  // A borrow neither drops nor moves.
  expect_ops(*f, "read_id", {"return s0.*.id"});
}

// ==========================================================================
//  Verifier: every rule must be able to fail
// ==========================================================================

auto test_verifier_rejects_incomplete_results() -> void {
  auto f = build(read_program("forms.cn"));
  auto no_field = f->function("make_pair");
  no_field.ops.erase(no_field.ops.begin());
  expect_rejects(*f, no_field, hir::verify_error_kind::bad_result,
                 "a field was never initialized");

  auto no_publish = f->function("make_rect");
  no_publish.ops.erase(no_publish.ops.begin() +
                       static_cast<std::ptrdiff_t>(
                           find_op(no_publish, hir::op_kind::publish_variant)));
  expect_rejects(*f, no_publish, hir::verify_error_kind::bad_result,
                 "a sum was never published");

  auto early = f->function("make_rect");
  const auto publish = find_op(early, hir::op_kind::publish_variant);
  early.ops[publish - 1].dest.steps[0].index = 0;
  early.ops[publish - 1].dest.steps[0].name = "circle";
  early.ops[publish - 1].dest.type = f->builtin("int32");
  expect_rejects(*f, early, hir::verify_error_kind::incomplete_variant,
                 "payload written for another variant");
}

auto test_verifier_rejects_missing_or_extra_returns() -> void {
  auto f = build(read_program("forms.cn"));
  auto open = f->function("forward");
  open.ops.pop_back();
  expect_rejects(*f, open, hir::verify_error_kind::missing_return,
                 "a function that never returns");

  auto trailing = f->function("forward");
  trailing.ops.push_back(trailing.ops.back());
  expect_rejects(*f, trailing, hir::verify_error_kind::code_after_return,
                 "an operation after the return");
}

auto test_verifier_rejects_reads_of_unfinished_values() -> void {
  auto f = build(read_program("forms.cn"));
  auto swapped = f->function("return_local");
  std::swap(swapped.ops[0], swapped.ops[1]);
  expect_rejects(*f, swapped, hir::verify_error_kind::uninitialized,
                 "a move before the call that builds its source");

  auto twice = f->function("return_local");
  const auto move = find_op(twice, hir::op_kind::move_into);
  twice.ops.insert(twice.ops.begin() + static_cast<std::ptrdiff_t>(move),
                   twice.ops[move]);
  expect_rejects(*f, twice, hir::verify_error_kind::uninitialized,
                 "a value moved twice");
}

auto test_verifier_rejects_use_after_dead() -> void {
  auto f = build(read_program("forms.cn"));
  auto dead = f->function("make_pair");
  auto end = hir::storage_op{.kind = hir::op_kind::storage_dead};
  end.slot = 1;
  dead.ops.insert(dead.ops.begin(), end);
  expect_rejects(*f, dead, hir::verify_error_kind::use_after_dead,
                 "a parameter read after its storage ended");

  auto twice = f->function("discard");
  const auto at = find_op(twice, hir::op_kind::storage_dead);
  twice.ops.insert(twice.ops.begin() + static_cast<std::ptrdiff_t>(at),
                   twice.ops[at]);
  expect_rejects(*f, twice, hir::verify_error_kind::use_after_dead,
                 "storage that ends twice");
}

auto test_verifier_checks_projection_bounds_and_types() -> void {
  auto f = build(read_program("forms.cn"));
  auto far = f->function("make_pair");
  far.ops[0].dest.steps[0].index = 7;
  expect_rejects(*f, far, hir::verify_error_kind::bad_place,
                 "a member past the end");

  auto renamed = f->function("make_pair");
  renamed.ops[0].dest.steps[0].name = "zzz";
  expect_rejects(*f, renamed, hir::verify_error_kind::bad_place,
                 "a member whose name disagrees with its index");

  auto retyped = f->function("make_pair");
  retyped.ops[0].dest.type = f->builtin("int64");
  expect_rejects(*f, retyped, hir::verify_error_kind::type_mismatch,
                 "a place typed other than where its projections arrive");

  auto element = f->function("make_array");
  element.ops[2].dest.steps[0].index = 3;
  expect_rejects(*f, element, hir::verify_error_kind::bad_place,
                 "an element past the end of an array");

  auto deref = f->function("make_pair");
  deref.ops[0].dest.steps.insert(deref.ops[0].dest.steps.begin(),
                                 hir::projection{.kind = hir::projection_kind::deref});
  expect_rejects(*f, deref, hir::verify_error_kind::bad_place,
                 "a dereference of a value that is not an address");
}

auto test_verifier_checks_transfer_rules() -> void {
  auto f = build(read_program("forms.cn"));
  auto copied = f->function("pass_local");
  copied.ops[find_op(copied, hir::op_kind::move_into)].kind =
      hir::op_kind::copy_into;
  expect_rejects(*f, copied, hir::verify_error_kind::not_copy,
                 "a copy of a type that moves");

  auto overlap = f->function("return_local");
  auto &move = overlap.ops[find_op(overlap, hir::op_kind::move_into)];
  move.dest = move.source;
  expect_rejects(*f, overlap, hir::verify_error_kind::overlapping_transfer,
                 "a transfer onto its own source");

  auto mismatched = f->function("return_local");
  auto &join = mismatched.ops[find_op(mismatched, hir::op_kind::move_into)];
  join.source.type = f->builtin("int32");
  expect_rejects(*f, join.source.type == f->builtin("int32")
                         ? mismatched
                         : mismatched,
                 hir::verify_error_kind::type_mismatch,
                 "a transfer between different types");
}

auto test_verifier_rejects_moves_out_of_references_and_sums() -> void {
  auto f = build(read_program("forms.cn"));
  const auto pair = f->function("make_pair").slots[0].type;

  // Move the pair a reference points at into a fresh local.
  auto through = f->function("first");
  const auto local = add_slot(through, hir::storage_class::frame_local, pair);
  auto steal = hir::storage_op{.kind = hir::op_kind::move_into};
  steal.dest = place_at(local, pair);
  steal.source = place_at(
      0, pair, {hir::projection{.kind = hir::projection_kind::deref}});
  through.ops.insert(through.ops.begin(), steal);
  expect_rejects(*f, through, hir::verify_error_kind::move_from_reference,
                 "a move out of borrowed bytes");

  // Take one payload out of a sum, then pass the sum on whole.
  auto partial = f->function("pass_shape");
  const auto taken = add_slot(partial, hir::storage_class::frame_local,
                              f->builtin("int32"));
  auto take = hir::storage_op{.kind = hir::op_kind::move_into};
  take.dest = place_at(taken, f->builtin("int32"));
  take.source = place_at(
      0, f->builtin("int32"),
      {hir::projection{.kind = hir::projection_kind::payload,
                       .index = 0,
                       .name = "rect"}});
  partial.ops.insert(partial.ops.begin(), take);
  expect_rejects(*f, partial, hir::verify_error_kind::uninitialized,
                 "a sum read whole after one payload moved out");
}

auto test_verifier_checks_call_signatures() -> void {
  auto f = build(read_program("forms.cn"));
  // The caller's view of `make_pair` disagrees with the callee's.
  auto skewed = f->function("forward");
  skewed.ops[find_op(skewed, hir::op_kind::call_into)].signature.params[0] =
      hir::classify_value(*f->layouts, f->builtin("int64")).value();
  expect_rejects(*f, skewed, hir::verify_error_kind::signature_mismatch,
                 "a call that passes a wider scalar than the callee takes");

  // A callee whose own signature changed is caught by the module check.
  auto module = f->lowering.module;
  for (auto &function : module.functions) {
    if (function.name == "make_pair") {
      function.signature.params[0].size = 8;
    }
  }
  const auto verdict = hir::verify(module, *f->env);
  expect(!verdict.has_value() &&
             verdict.error().kind == hir::verify_error_kind::signature_mismatch,
         "expected the module check to compare every call with its callee");

  auto arity = f->function("forward");
  arity.ops[find_op(arity, hir::op_kind::call_into)].args.clear();
  expect_rejects(*f, arity, hir::verify_error_kind::bad_argument,
                 "a call with a missing argument");

  auto indirect = f->function("apply");
  indirect.ops[find_op(indirect, hir::op_kind::call_into)]
      .signature.params[0] =
      hir::classify_value(*f->layouts, f->builtin("int64")).value();
  expect_rejects(*f, indirect, hir::verify_error_kind::signature_mismatch,
                 "an indirect call that disagrees with the function value");
}

auto test_verifier_checks_argument_consumption() -> void {
  auto f = build(read_program("forms.cn"));
  auto unused = f->function("pass_fresh");
  unused.ops.erase(unused.ops.begin() +
                   static_cast<std::ptrdiff_t>(
                       find_op(unused, hir::op_kind::call_into, 1)));
  expect_rejects(*f, unused, hir::verify_error_kind::bad_lifetime_end,
                 "an outgoing argument no call consumed");

  auto shared = f->function("pass_fresh");
  auto &call = shared.ops[find_op(shared, hir::op_kind::call_into, 1)];
  call.args[0].slot = place_at(add_slot(shared, hir::storage_class::frame_local, call.args[0].slot.type), call.args[0].slot.type);
  expect_rejects(*f, shared, hir::verify_error_kind::bad_argument,
                 "an argument that is not an outgoing slot");
}

auto test_verifier_rejects_a_result_that_aliases_a_borrow() -> void {
  auto f = build(read_program("forms.cn"));
  auto aliased = f->function("self_bump");
  auto &call = aliased.ops[find_op(aliased, hir::op_kind::call_into, 1)];
  call.dest = place_at(2, call.dest.type);
  expect_rejects(*f, aliased, hir::verify_error_kind::result_aliases_argument,
                 "x = make(&x) built in place");
}

auto test_verifier_rejects_double_construction_of_a_droppable() -> void {
  auto f = build(read_program("drops.cn"));
  auto twice = f->function("forget_noisy");
  twice.ops.insert(twice.ops.begin() + 1, twice.ops[0]);
  expect_rejects(*f, twice, hir::verify_error_kind::already_initialized,
                 "a value needing a drop built over a live one");
}

auto erase_op(hir::storage_function &function, size_t index) -> void {
  function.ops.erase(function.ops.begin() + static_cast<std::ptrdiff_t>(index));
}

auto test_verifier_reports_values_that_owe_a_drop() -> void {
  auto f = build(read_program("drops.cn"));

  // A local whose scope-exit drop is missing.
  auto forgotten = f->function("forget_noisy");
  erase_op(forgotten, find_op(forgotten, hir::op_kind::call_into, 1));
  expect_rejects(*f, forgotten, hir::verify_error_kind::leaked_value,
                 "a droppable local left at return");
  expect_verifies(*f, forgotten, "leaks are reported only on request",
                  hir::verify_options{.check_leaks = false});

  // A parameter the function owns.
  auto param = f->function("take_noisy");
  erase_op(param, find_op(param, hir::op_kind::call_into));
  expect_rejects(*f, param, hir::verify_error_kind::leaked_value,
                 "an owned parameter left at return");

  // Each droppable field owes its own drop.
  auto first_field = f->function("drop_holder");
  erase_op(first_field, find_op(first_field, hir::op_kind::call_into, 1));
  expect_rejects(*f, first_field, hir::verify_error_kind::leaked_value,
                 "the first field of a struct was not dropped");
  auto second_field = f->function("drop_holder");
  erase_op(second_field, find_op(second_field, hir::op_kind::call_into, 2));
  expect_rejects(*f, second_field, hir::verify_error_kind::leaked_value,
                 "the second field of a struct was not dropped");

  // What is left after a partial move still owes its drop; what moved does not.
  auto partial = f->function("take_first");
  expect_verifies(*f, partial, "a field moved out is not owed");
  erase_op(partial, find_op(partial, hir::op_kind::call_into, 1));
  expect_rejects(*f, partial, hir::verify_error_kind::leaked_value,
                 "the field that stayed behind was not dropped");

  // A drop of the whole place ends the value as well.
  auto in_place = f->function("forget_noisy");
  const auto call = find_op(in_place, hir::op_kind::call_into, 1);
  in_place.ops[call] = hir::storage_op{.kind = hir::op_kind::drop_in_place};
  in_place.ops[call].dest = place_at(1, in_place.slots[1].type);
  expect_verifies(*f, in_place, "dropped in place");
}

auto test_verifier_rejects_second_and_late_drops() -> void {
  auto f = build(read_program("drops.cn"));
  auto twice = f->function("forget_noisy");
  const auto call = find_op(twice, hir::op_kind::call_into, 1);
  twice.ops.insert(twice.ops.begin() + static_cast<std::ptrdiff_t>(call),
                   twice.ops[call]);
  expect_rejects(*f, twice, hir::verify_error_kind::double_drop,
                 "the same value dropped twice");

  // A borrow of the value after its drop ran.
  auto late = f->function("read_through");
  std::swap(late.ops[find_op(late, hir::op_kind::call_into, 1)],
            late.ops[find_op(late, hir::op_kind::call_into, 2)]);
  expect_rejects(*f, late, hir::verify_error_kind::use_after_drop,
                 "a borrow after the drop");

  // Replacement: skipping the old value's drop writes over a live value.
  auto skipped = f->function("replace_noisy");
  erase_op(skipped, find_op(skipped, hir::op_kind::call_into, 2));
  expect_rejects(*f, skipped, hir::verify_error_kind::already_initialized,
                 "a replacement written over a value that was not dropped");

  // Replacement: the old value moved out first needs no drop.
  auto field = f->function("replace_second");
  erase_op(field, find_op(field, hir::op_kind::call_into, 2));
  expect_rejects(*f, field, hir::verify_error_kind::already_initialized,
                 "a field replaced without dropping the old one");
}

auto test_verifier_checks_the_shape_of_a_drop_call() -> void {
  auto f = build(read_program("drops.cn"));
  const auto unit = f->builtin("unit");

  // A type with no drop of its own has nothing to call.
  auto no_own = f->function("drop_holder");
  const auto holder = f->function("drop_holder").slots[1].type;
  auto &first = no_own.ops[find_op(no_own, hir::op_kind::call_into, 1)];
  first.args[0].value.where = place_at(1, holder);
  expect_rejects(*f, no_own, hir::verify_error_kind::bad_operation,
                 "a drop call on a struct that has no drop of its own");

  // Bytes behind a reference belong to their owner.
  auto borrowed = f->function("read_id");
  auto drop = f->function("forget_noisy").ops[2];
  const auto noisy = f->function("forget_noisy").slots[1].type;
  drop.dest = place_at(add_slot(borrowed, hir::storage_class::frame_local, unit),
                       unit);
  drop.args[0].value.where =
      place_at(0, noisy, {hir::projection{.kind = hir::projection_kind::deref}});
  borrowed.ops.insert(borrowed.ops.begin(), drop);
  expect_rejects(*f, borrowed, hir::verify_error_kind::move_from_reference,
                 "a drop of a value behind a reference");

  // The call takes an address and nothing else.
  auto bare = f->function("forget_noisy");
  bare.ops[find_op(bare, hir::op_kind::call_into, 1)].args[0].value.kind =
      hir::operand_kind::constant;
  expect_rejects(*f, bare, hir::verify_error_kind::bad_argument,
                 "a drop call that is not given an address");
}

auto test_verifier_checks_slots_and_signature() -> void {
  auto f = build(read_program("forms.cn"));
  auto no_result = f->function("make_pair");
  no_result.result_slot = hir::k_invalid_storage;
  expect_rejects(*f, no_result, hir::verify_error_kind::bad_slot,
                 "an aggregate result with no result slot");

  auto missing_param = f->function("make_pair");
  missing_param.param_slots.clear();
  expect_rejects(*f, missing_param, hir::verify_error_kind::bad_slot,
                 "parameters without slots");

  auto renumbered = f->function("make_pair");
  renumbered.slots[1].id = 5;
  expect_rejects(*f, renumbered, hir::verify_error_kind::bad_slot,
                 "a slot whose id is not its index");

  auto direct_with_value = f->function("forward");
  direct_with_value.ops.back().value = hir::operand{};
  expect_rejects(*f, direct_with_value, hir::verify_error_kind::bad_result,
                 "a value returned alongside a result address");

  auto value_missing = f->function("bind");
  value_missing.ops.back().value.reset();
  expect_rejects(*f, value_missing, hir::verify_error_kind::bad_result,
                 "a direct result with no value");
}

// ==========================================================================
//  Control flow
// ==========================================================================

auto test_control_flow_lowers_and_verifies() -> void {
  auto f = build(read_program("flow.cn"));
  for (const auto &refused : f->lowering.refused) {
    std::cerr << "refused " << refused.name << ": " << refused.error.message
              << "\n";
  }
  expect(f->lowering.refused.empty(),
         "expected every control-flow function to lower");
  for (const auto &function : f->lowering.module.functions) {
    const auto verdict = hir::verify(function, *f->env, &f->lowering.module);
    if (!verdict.has_value()) {
      std::cerr << hir::dump(function, f->checked.types);
      std::cerr << function.name << " op " << verdict.error().op_index << " "
                << hir::verify_error_kind_name(verdict.error().kind) << ": "
                << verdict.error().message << "\n";
      fail("expected the function to verify");
    }
  }
}

auto test_if_branches_build_the_value_in_the_destination() -> void {
  auto f = build(read_program("flow.cn"));
  // Both sides write the `let` slot; the condition slot ends on each side.
  expect_ops(*f, "pick",
             {"compute s2 = binary > s0, #0", "if s2", "dead s2",
              "compute s1 = copy s0", "else", "dead s2",
              "compute s1 = binary - #0, s0", "end_if", "return s1"});
  // A constructor in each arm receives the caller's result bytes.
  expect_ops(*f, "pick_pair",
             {"compute s2 = binary > s1, #0", "if s2", "dead s2",
              "call s0 = make_pair(s1, #1)", "else", "dead s2",
              "call s0 = make_pair(#0, s1)", "end_if", "return"});
  // `elif` nests inside the `else` side.
  expect_ops(*f, "chain",
             {"compute s2 = binary > s0, #10", "if s2", "dead s2",
              "compute s1 = const 3", "else", "dead s2",
              "compute s3 = binary > s0, #5", "if s3", "dead s3",
              "compute s1 = const 2", "else", "dead s3",
              "compute s1 = const 1", "end_if", "end_if", "return s1"});
  // A path that returns leaves the join to the other path.
  expect_ops(*f, "early",
             {"compute s1 = binary > s0, #0", "if s1", "dead s1", "return #1",
              "else", "dead s1", "end_if", "return #2"});
  expect_ops(*f, "both_return",
             {"compute s1 = binary > s0, #0", "if s1", "dead s1", "return #1",
              "else", "dead s1", "return #2", "end_if"});
}

auto test_loops_test_run_and_repeat() -> void {
  auto f = build(read_program("flow.cn"));
  // The condition leaves the loop; the body is a block; the loop repeats.
  expect_ops(*f, "count",
             {"compute s1 = const 0", "loop", "compute s2 = binary < s1, s0",
              "if s2", "dead s2", "else", "dead s2", "exit 0", "end_if", "block",
              "compute s1 = binary + s1, #1", "end_block", "end_loop",
              "return s1"});
  // Storage the body declares ends before the loop repeats.
  expect_ops(*f, "count_with_locals",
             {"compute s1 = const 0", "compute s2 = const 0", "loop",
              "compute s3 = binary < s1, s0", "if s3", "dead s3", "else",
              "dead s3", "exit 0", "end_if", "block",
              "compute s4 = binary + s1, #1", "compute s2 = binary + s2, s4",
              "compute s1 = binary + s1, #1", "end_block", "dead s4",
              "end_loop", "return s2"});
  // `while true` has no condition to leave by; only `break` does.
  expect_ops(*f, "stops",
             {"compute s1 = const 0", "loop", "block",
              "compute s2 = binary >= s1, s0", "if s2", "dead s2", "exit 1",
              "else", "dead s2", "end_if", "compute s1 = binary + s1, #1",
              "end_block", "end_loop", "return s1"});
  // A loop with nothing that leaves it ends the function's path.
  expect_ops(*f, "returns_from_loop",
             {"compute s1 = const 0", "loop", "block",
              "compute s2 = binary == s1, s0", "if s2", "dead s2", "return s1",
              "else", "dead s2", "end_if", "compute s1 = binary + s1, #1",
              "end_block", "end_loop"});
}

auto test_break_and_continue_name_their_loop_by_depth() -> void {
  auto f = build(read_program("flow.cn"));
  // `continue` leaves the body block, so the step still runs.
  expect_ops(*f, "stepped",
             {"compute s1 = const 0", "compute s2 = const 0",
              "compute s3 = copy s0", "compute s4 = copy s2", "loop",
              "compute s5 = binary < s4, s3", "if s5", "dead s5", "else",
              "dead s5", "exit 0", "end_if", "block", "compute s6 = copy s4",
              "compute s7 = binary == s6, #2", "if s7", "dead s7", "exit 0",
              "else", "dead s7", "end_if", "compute s1 = binary + s1, s6",
              "end_block", "compute s4 = binary + s4, #1", "dead s6",
              "end_loop", "return s1"});
  // In nested loops `break` counts the inner body block and the inner loop.
  const auto inner = f->function("nested");
  auto depths = std::vector<size_t>{};
  for (const auto &op : inner.ops) {
    if (op.kind == hir::op_kind::exit) {
      depths.push_back(op.depth);
    }
  }
  expect((depths == std::vector<size_t>{0, 0, 1}),
         "expected exits for the outer test, the inner test, and the break");
  expect_ops(*f, "skips",
             {"compute s1 = const 0", "compute s2 = const 0", "loop",
              "compute s3 = binary < s1, s0", "if s3", "dead s3", "else",
              "dead s3", "exit 0", "end_if", "block",
              "compute s1 = binary + s1, #1", "compute s4 = binary == s1, #3",
              "if s4", "dead s4", "exit 0", "else", "dead s4", "end_if",
              "compute s2 = binary + s2, s1", "end_block", "end_loop",
              "return s2"});
}

auto test_logical_operators_do_not_run_the_right_side_eagerly() -> void {
  auto f = build(read_program("flow.cn"));
  // The division runs only when the left side did not decide the result.
  expect_ops(*f, "guarded",
             {"compute s1 = binary != s0, #0", "if s1",
              "compute s2 = binary / #10, s0", "compute s1 = binary > s2, #1",
              "dead s2", "end_if", "return s1"});
  // `or` runs its right side on the `else` path.
  expect_ops(*f, "either",
             {"compute s2 = copy s0", "if s2", "else", "compute s2 = copy s1",
              "end_if", "return s2"});
}

auto test_drops_inside_branches_and_loops() -> void {
  auto f = build(read_program("flow.cn"));
  // The drop runs on the path that owns the value, before it returns.
  expect_ops(*f, "drop_in_branch",
             {"compute s1 = binary > s0, #0", "if s1", "dead s1",
              "call s2 = make_noisy(s0)", "compute s3 = copy s2.id",
              "call.drop s4 = noisy::drop(&s2)", "dead s4", "return s3", "else",
              "dead s1", "end_if", "return #0"});
  // The value dropped each iteration is built again on the next one.
  expect_ops(*f, "drop_in_loop",
             {"compute s1 = const 0", "loop", "compute s2 = binary < s1, s0",
              "if s2", "dead s2", "else", "dead s2", "exit 0", "end_if", "block",
              "call s3 = make_noisy(s1)", "compute s1 = binary + s1, #1",
              "call.drop s4 = noisy::drop(&s3)", "dead s4", "end_block",
              "dead s3", "end_loop", "return s1"});
}

auto test_verifier_checks_joins_and_loops() -> void {
  auto f = build(read_program("flow.cn"));

  // A value that owes a drop on one path and not the other needs a flag.
  auto uneven = f->function("drop_in_branch");
  erase_op(uneven, find_op(uneven, hir::op_kind::return_value, 0));
  erase_op(uneven, find_op(uneven, hir::op_kind::storage_dead, 1));
  erase_op(uneven, find_op(uneven, hir::op_kind::call_into, 1));
  expect_rejects(*f, uneven, hir::verify_error_kind::join_mismatch,
                 "a droppable value dropped on one path only");

  // Storage that ends on one side while the other still holds a value.
  auto one_sided = f->function("pick");
  erase_op(one_sided, find_op(one_sided, hir::op_kind::storage_dead, 1));
  expect_rejects(*f, one_sided, hir::verify_error_kind::join_mismatch,
                 "a slot that ends on one side only");

  // A loop body that leaves a slot different at its end than at its start.
  auto carried = f->function("count_with_locals");
  erase_op(carried, find_op(carried, hir::op_kind::storage_dead, 2));
  expect_rejects(*f, carried, hir::verify_error_kind::join_mismatch,
                 "storage declared in a loop body that is never ended");

  // `exit` must name a loop or block that is open.
  auto too_deep = f->function("stops");
  too_deep.ops[find_op(too_deep, hir::op_kind::exit, 0)].depth = 5;
  expect_rejects(*f, too_deep, hir::verify_error_kind::bad_operation,
                 "an exit past the outermost loop");

  // A region left open, and one closed that was never opened.
  auto unclosed = f->function("early");
  erase_op(unclosed, find_op(unclosed, hir::op_kind::if_end, 0));
  expect_rejects(*f, unclosed, hir::verify_error_kind::bad_operation,
                 "an `if` that is never closed");
  auto stray = f->function("early");
  stray.ops.insert(stray.ops.begin(),
                   hir::storage_op{.kind = hir::op_kind::else_begin});
  expect_rejects(*f, stray, hir::verify_error_kind::bad_operation,
                 "an `else` outside any `if`");
  auto crossed = f->function("count");
  crossed.ops[find_op(crossed, hir::op_kind::block_end, 0)].kind =
      hir::op_kind::loop_end;
  expect_rejects(*f, crossed, hir::verify_error_kind::bad_operation,
                 "regions that close out of order");

  // After a return, only the markers that close a region may follow.
  auto after = f->function("both_return");
  after.ops.insert(after.ops.begin() + 4,
                   hir::storage_op{.kind = hir::op_kind::return_value});
  expect_rejects(*f, after, hir::verify_error_kind::code_after_return,
                 "an operation after a return inside a branch");

  // A path that can reach the end of the function must return.
  auto falls_off = f->function("early");
  erase_op(falls_off, find_op(falls_off, hir::op_kind::return_value, 1));
  expect_rejects(*f, falls_off, hir::verify_error_kind::missing_return,
                 "a function that can fall off its end after an `if`");
}

} // namespace


// ==========================================================================
//  Raw memory
// ==========================================================================

auto find_offset(const hir::storage_function &function) -> size_t {
  for (size_t i = 0; i < function.ops.size(); ++i) {
    if (function.ops[i].kind == hir::op_kind::compute &&
        function.ops[i].computation == hir::compute_kind::pointer_offset) {
      return i;
    }
  }
  fail("expected the function to advance a raw pointer");
}

auto drop_calls(const hir::storage_function &function) -> size_t {
  auto count = size_t{0};
  for (const auto &op : function.ops) {
    count += op.kind == hir::op_kind::call_into && op.is_drop ? 1U : 0U;
  }
  return count;
}

auto test_raw_index_is_a_pointer_offset_then_a_dereference() -> void {
  auto f = build(read_program("raw.cn"));
  const auto load = f->function("load");
  const auto &offset = load.ops[find_offset(load)];
  // `pair` is `{int32 @0, int64 @8}`: 16 bytes.
  expect(offset.stride == 16, "expected the stride to be the element's size");
  expect(offset.operands.size() == 2, "expected a pointer and an index");
  // A struct moves unless it is declared otherwise: the read takes the value
  // out of the slot into the result.
  const auto &read = load.ops[find_op(load, hir::op_kind::move_into)];
  expect(read.source.steps.size() == 1 &&
             read.source.steps[0].kind == hir::projection_kind::deref,
         "expected the slot to be read through the advanced pointer");
  expect(read.source.base == offset.dest.base,
         "expected the read to use the pointer the offset computed");
  expect_verifies(*f, load, "load");
  expect_verifies(*f, f->function("store"), "store");
  expect_verifies(*f, f->function("second_field"), "second_field");

  const auto text = hir::dump(f->lowering.module, f->checked.types);
  expect(text.find("offset") != std::string::npos &&
             text.find("* 16") != std::string::npos,
         "expected the offset and its stride in the dump");
}

auto test_verifier_checks_the_operands_of_an_offset() -> void {
  auto f = build(read_program("raw.cn"));
  const auto int32 = f->builtin("int32");

  // The result of an offset is a pointer.
  auto narrowed = f->function("load");
  const auto at = find_offset(narrowed);
  narrowed.slots[narrowed.ops[at].dest.base].type = int32;
  narrowed.ops[at].dest.type = int32;
  expect_rejects(*f, narrowed, hir::verify_error_kind::type_mismatch,
                 "an offset that does not give a pointer");

  // So is its base.
  auto numeric = f->function("load");
  numeric.ops[find_offset(numeric)].operands[0].type = int32;
  expect_rejects(*f, numeric, hir::verify_error_kind::type_mismatch,
                 "an offset of a number");

  // It takes exactly a pointer and an index.
  auto bare = f->function("load");
  bare.ops[find_offset(bare)].operands.pop_back();
  expect_rejects(*f, bare, hir::verify_error_kind::bad_operation,
                 "an offset without an index");
}

auto test_raw_reads_own_the_value_and_raw_writes_drop_nothing() -> void {
  auto f = build(read_program("raw.cn"));
  // The write over a slot runs no drop; the value read out is dropped once by
  // its new owner.
  expect(drop_calls(f->function("overwrite")) == 0,
         "expected a raw write not to drop the slot's old value");
  expect(drop_calls(f->function("take_and_drop")) == 1,
         "expected the value read out of a slot to be dropped once");
  expect_verifies(*f, f->function("overwrite"), "overwrite");
  const auto taken = f->function("take_and_drop");
  expect_verifies(*f, taken, "take_and_drop");
  // `noisy` is not `copy`: the read moves the value out of the raw slot, which
  // the verifier accepts only behind a raw pointer.
  const auto &move = taken.ops[find_op(taken, hir::op_kind::move_into)];
  expect(move.source.steps.size() == 1 &&
             move.source.steps[0].kind == hir::projection_kind::deref,
         "expected the value to be moved out through the pointer");

  // The same read through an ordinary reference is still refused (see
  // `test_verifier_rejects_moves_out_of_references_and_sums`).
}

auto test_constant_array_index_is_an_element_projection() -> void {
  auto f = build(read_program("raw.cn"));
  const auto third = f->function("third");
  const auto &ret = third.ops[find_op(third, hir::op_kind::return_value)];
  expect(ret.value.has_value() && ret.value->where.steps.size() == 1 &&
             ret.value->where.steps[0].kind == hir::projection_kind::element &&
             ret.value->where.steps[0].index == 2,
         "expected element 2 of the array parameter");
  expect_verifies(*f, third, "third");
}

auto closure_op(const hir::storage_function &function) -> const hir::storage_op & {
  return function.ops[find_op(function, hir::op_kind::make_closure)];
}

auto test_closure_is_built_with_its_captures_and_glue() -> void {
  auto f = build(read_program("closures.cn"));
  const auto add = f->function("add_captured");
  const auto &made = closure_op(add);
  expect(made.entry == "add_captured$closure0" &&
             made.glue == "add_captured$closure0$drop",
         "expected the closure to name its body and its drop glue");
  expect(made.env_size == 8 && made.env_align == 8,
         "expected an environment for one int64");
  expect(made.captures.size() == 1 &&
             made.captures[0].kind == hir::capture_kind::copy &&
             made.captures[0].offset == 0,
         "expected `n` captured by copy at offset 0");
  expect_verifies(*f, add, "add_captured");

  // The body takes the environment first and sees `n` at its offset.
  const auto body = f->function("add_captured$closure0");
  expect(body.signature.takes_environment,
         "expected the closure body to take the environment");
  auto captured = size_t{0};
  for (const auto &slot : body.slots) {
    if (slot.cls == hir::storage_class::closure_environment) {
      ++captured;
      expect(slot.env_offset == 0 && !slot.env_indirect,
             "expected `n` inline at offset 0");
    }
  }
  expect(captured == 1, "expected one environment slot in the body");
  expect_verifies(*f, body, "add_captured$closure0");

  // The glue frees the block it is given.
  const auto glue = f->function("add_captured$closure0$drop");
  const auto &release = glue.ops[find_op(glue, hir::op_kind::release_environment)];
  expect(release.env_size == 8, "expected the glue to free the whole block");
  expect_verifies(*f, glue, "add_captured$closure0$drop");
}

auto test_aggregate_captures_sit_at_their_own_alignment() -> void {
  auto f = build(read_program("closures.cn"));
  // `pair` is `{int32 @0, int64 @8}`: 16 bytes, aligned to 8.
  const auto pair_fn = f->function("captured_pair");
  const auto &made = closure_op(pair_fn);
  expect(made.env_size == 16 && made.captures.size() == 1 &&
             made.captures[0].offset == 0,
         "expected the whole pair copied into the environment");
  expect_verifies(*f, f->function("captured_pair"), "captured_pair");
  expect_verifies(*f, f->function("captured_pair$closure0"),
                  "captured_pair$closure0");
}

auto test_moved_captures_are_dropped_by_the_glue() -> void {
  auto f = build(read_program("closures.cn"));
  const auto owner = f->function("own");
  const auto &made = closure_op(owner);
  expect(made.captures.size() == 1 &&
             made.captures[0].kind == hir::capture_kind::move,
         "expected `n` moved into the closure");
  // The enclosing function no longer owes a drop for `n`; the glue pays it.
  expect(drop_calls(owner) == 0,
         "expected the moved value not to be dropped by its old owner");
  expect(drop_calls(f->function("own$closure0$drop")) == 1,
         "expected the glue to drop the one owned capture");
  expect_verifies(*f, owner, "own");
  expect_verifies(*f, f->function("own$closure0$drop"), "own$closure0$drop");
  // The closure is dropped by whoever ends it: the callee that was handed it.
  const auto callee = f->function("call_it");
  expect(callee.ops[find_op(callee, hir::op_kind::drop_in_place)].dest.base ==
             callee.param_slots[0],
         "expected `call_it` to drop the function value it owns");
  const auto &call = callee.ops[find_op(callee, hir::op_kind::call_into)];
  expect(!call.callee.is_direct(), "expected a call through the function value");
}

auto test_reference_captures_are_reached_through_an_address() -> void {
  auto f = build(read_program("closures.cn"));
  const auto outer = f->function("by_reference");
  const auto &made = closure_op(outer);
  expect(made.captures.size() == 1 &&
             made.captures[0].kind == hir::capture_kind::reference &&
             made.env_size == 8,
         "expected the environment to hold one address");
  expect_verifies(*f, outer, "by_reference");
  const auto body = f->function("by_reference$closure0");
  auto indirect = size_t{0};
  for (const auto &slot : body.slots) {
    indirect += slot.cls == hir::storage_class::closure_environment &&
                        slot.env_indirect
                    ? 1U
                    : 0U;
  }
  expect(indirect == 1, "expected `total` reached through the stored address");
  expect_verifies(*f, body, "by_reference$closure0");
}

auto test_verifier_checks_closures() -> void {
  auto f = build(read_program("closures.cn"));

  auto outside = f->function("add_captured");
  outside.ops[find_op(outside, hir::op_kind::make_closure)].captures[0].offset = 16;
  expect_rejects(*f, outside, hir::verify_error_kind::bad_operation,
                 "a capture outside the environment");

  auto misaligned = f->function("add_captured");
  misaligned.ops[find_op(misaligned, hir::op_kind::make_closure)]
      .captures[0]
      .offset = 4;
  expect_rejects(*f, misaligned, hir::verify_error_kind::bad_operation,
                 "a capture that is not at its alignment");

  auto wrong_body = f->function("add_captured");
  wrong_body.ops[find_op(wrong_body, hir::op_kind::make_closure)].entry = "call_it";
  expect_rejects(*f, wrong_body, hir::verify_error_kind::signature_mismatch,
                 "an entry that is not a closure body");

  auto wrong_glue = f->function("add_captured");
  wrong_glue.ops[find_op(wrong_glue, hir::op_kind::make_closure)].glue =
      "add_captured$closure0";
  expect_rejects(*f, wrong_glue, hir::verify_error_kind::signature_mismatch,
                 "drop glue that takes arguments");

  auto empty = f->function("add_captured");
  empty.ops[find_op(empty, hir::op_kind::make_closure)].env_size = 0;
  expect_rejects(*f, empty, hir::verify_error_kind::bad_operation,
                 "captures stored in an empty environment");

  // A function that was not given an environment has none to release.
  auto stray = f->function("call_it");
  stray.ops.insert(stray.ops.begin(), hir::storage_op{
                                          .kind = hir::op_kind::release_environment,
                                          .env_size = 8});
  expect_rejects(*f, stray, hir::verify_error_kind::bad_operation,
                 "a release in a function with no environment");

  // The closure keeps ownership of what it captured: the body cannot move it.
  auto taken = f->function("own$closure0");
  auto changed = false;
  for (auto &op : taken.ops) {
    for (auto &value : op.operands) {
      if (value.kind == hir::operand_kind::read && value.where.base != 0 &&
          taken.slots[value.where.base].cls ==
              hir::storage_class::closure_environment) {
        value.consume = true;
        changed = true;
      }
    }
  }
  expect(changed, "expected the body to read a captured value");
  expect_rejects(*f, taken, hir::verify_error_kind::move_from_reference,
                 "a body that takes a captured value");
}

// ==========================================================================
//  Patterns: match, while let, let else
// ==========================================================================

auto test_match_tests_the_tag_and_binds_payloads() -> void {
  auto f = build(read_program("patterns.cn"));
  // Each arm tests its variant into a flag, branches on it, and binds its
  // payload from the sum's own bytes. The last arm needs no test.
  expect_ops(*f, "area",
             {"test s3 = s0 is @circle", "if s3", "dead s3",
              "compute s2 = const true", "else", "dead s3",
              "compute s2 = const false", "end_if", "if s2", "dead s2",
              "compute s4 = copy s0.@circle.0", "compute s1 = copy s4", "else",
              "dead s2", "test s6 = s0 is @rect", "if s6", "dead s6",
              "compute s5 = const true", "else", "dead s6",
              "compute s5 = const false", "end_if", "if s5", "dead s5",
              "compute s7 = copy s0.@rect.0", "compute s8 = copy s0.@rect.1",
              "compute s1 = binary + s7, s8", "else", "dead s5",
              "compute s1 = const 7", "end_if", "end_if", "return s1"});
  expect_verifies(*f, f->function("area"), "area");
  const auto tests = [&](std::string_view name) {
    auto count = size_t{0};
    for (const auto &op : f->function(name).ops) {
      count += op.kind == hir::op_kind::test_variant ? 1U : 0U;
    }
    return count;
  };
  expect(tests("area") == 2, "expected the last arm to need no variant test");
  expect(tests("either") == 2,
         "expected each alternative of an or-pattern to be tested");
}

auto test_statement_match_writes_through_its_arms() -> void {
  auto f = build(read_program("patterns.cn"));
  expect_ops(*f, "partial",
             {"compute s1 = const 100", "test s3 = s0 is @circle", "if s3",
              "dead s3", "compute s2 = const true", "else", "dead s3",
              "compute s2 = const false", "end_if", "if s2", "dead s2",
              "compute s4 = copy s0.@circle.0",
              "compute s1 = binary + s1, s4", "else", "dead s2",
              "test s6 = s0 is @empty", "if s6", "dead s6",
              "compute s5 = const true", "else", "dead s6",
              "compute s5 = const false", "end_if", "if s5", "dead s5",
              "compute s1 = binary + s1, #1", "else", "dead s5",
              "compute s1 = binary + s1, #2", "end_if", "end_if", "return s1"});
  expect_verifies(*f, f->function("partial"), "partial");
}

auto test_literal_patterns_compare_after_the_tag() -> void {
  auto f = build(read_program("patterns.cn"));
  // `@some(0)` reads the payload only inside the branch that saw `@some`.
  const auto lines = op_lines(*f, f->function("literal"));
  const auto head = std::vector<std::string>(lines.begin(), lines.begin() + 5);
  expect(head == std::vector<std::string>{"test s3 = s0 is @some", "if s3",
                                          "dead s3",
                                          "compute s4 = binary == s0.@some.0, #0",
                                          "if s4"},
         "expected the comparison to sit inside the branch on the tag");
  expect_verifies(*f, f->function("literal"), "literal");
}

auto test_aggregate_payload_moves_out_of_the_sum() -> void {
  auto f = build(read_program("patterns.cn"));
  expect_ops(*f, "unbox",
             {"test s3 = s0 is @one", "if s3", "dead s3",
              "compute s2 = const true", "else", "dead s3",
              "compute s2 = const false", "end_if", "if s2", "dead s2",
              "move s4 <- s0.@one.0", "compute s1 = copy s4.a", "else",
              "dead s2", "compute s1 = unary - #1", "end_if", "return s1"});
  expect_verifies(*f, f->function("unbox"), "unbox");
}

auto test_while_let_and_let_else_lower() -> void {
  auto f = build(read_program("patterns.cn"));
  // The subject is rebuilt on every iteration, the loop leaves when the
  // pattern fails, and the subject's storage ends before the loop repeats.
  expect_ops(*f, "drain",
             {"compute s1 = copy s0", "compute s2 = const 0", "loop",
              "compute s4 = binary > s1, #0", "call s3 = make_option(s4, s1)",
              "dead s4", "test s6 = s3 is @some", "if s6", "dead s6",
              "compute s5 = const true", "else", "dead s6",
              "compute s5 = const false", "end_if", "if s5", "dead s5", "else",
              "dead s5", "exit 0", "end_if", "block",
              "compute s7 = copy s3.@some.0", "compute s2 = binary + s2, s7",
              "compute s1 = binary - s1, #1", "end_block", "dead s7", "dead s3",
              "end_loop", "return s2"});
  // The else block leaves, so the code after the statement runs only when the
  // pattern held and reads the payload unconditionally.
  expect_ops(*f, "first_or",
             {"call s2 = make_option(s0, s1)", "test s4 = s2 is @some", "if s4",
              "dead s4", "compute s3 = const true", "else", "dead s4",
              "compute s3 = const false", "end_if", "if s3", "dead s3", "else",
              "dead s3", "compute s5 = unary - #5", "return s5", "end_if",
              "compute s6 = copy s2.@some.0", "return s6"});
  expect_verifies(*f, f->function("drain"), "drain");
  expect_verifies(*f, f->function("first_or"), "first_or");
}

auto test_patterns_not_lowered_yet_are_refused_by_name() -> void {
  auto f = build(read_program("patterns.cn"));
  const auto *guarded = f->refusal("guarded");
  expect(guarded != nullptr &&
             guarded->message.find("guard") != std::string::npos,
         "expected a `match` arm guard to be refused by name");
  const auto *owned = f->refusal("owned_subject");
  expect(owned != nullptr && owned->message.find("drop") != std::string::npos,
         "expected a pattern test on a value that needs a drop to be refused");
}

auto test_verifier_checks_variant_tests() -> void {
  auto f = build(read_program("patterns.cn"));
  const auto shape = f->function("area").slots[0].type;
  const auto flag = f->function("area").slots[3].type;

  auto unknown = f->function("area");
  unknown.ops[find_op(unknown, hir::op_kind::test_variant)].variant = "oval";
  expect_rejects(*f, unknown, hir::verify_error_kind::bad_place,
                 "a test for a variant the sum does not have");

  auto scalar = f->function("area");
  scalar.ops[find_op(scalar, hir::op_kind::test_variant)].source =
      place_at(3, flag);
  expect_rejects(*f, scalar, hir::verify_error_kind::type_mismatch,
                 "a variant test on a value that is not a sum");

  auto wide = f->function("area");
  wide.ops[find_op(wide, hir::op_kind::test_variant)].dest = place_at(0, shape);
  expect_rejects(*f, wide, hir::verify_error_kind::type_mismatch,
                 "a variant test written to an aggregate");

  // A sum that nothing has built has no tag to read.
  auto unbuilt = f->function("area");
  const auto fresh = add_slot(unbuilt, hir::storage_class::frame_local, shape);
  unbuilt.ops[find_op(unbuilt, hir::op_kind::test_variant)].source =
      place_at(fresh, shape);
  expect_rejects(*f, unbuilt, hir::verify_error_kind::uninitialized,
                 "a variant test on an unbuilt sum");

  // The binding reads a payload the sum no longer has once it is moved out.
  auto taken = f->function("unbox");
  const auto &move = taken.ops[find_op(taken, hir::op_kind::move_into)];
  auto again = move;
  taken.ops.insert(taken.ops.begin() +
                       static_cast<std::ptrdiff_t>(
                           find_op(taken, hir::op_kind::move_into)) + 1,
                   again);
  expect_rejects(*f, taken, hir::verify_error_kind::uninitialized,
                 "a payload moved out twice");
}

// ==========================================================================
//  Generators
// ==========================================================================

auto test_verifier_checks_task_groups() -> void {
  auto f = build(read_program("tasks.cn"));

  // A group wait names a tuple of tasks, not one task.
  auto one_as_group = f->function("sum_doubles$step");
  one_as_group.ops[find_op(one_as_group, hir::op_kind::coroutine_await)].waits =
      hir::wait_mode::all;
  expect_rejects(*f, one_as_group, hir::verify_error_kind::type_mismatch,
                 "a group wait on one task", {}, "needs a tuple of tasks");

  // A wait on one task does not name a tuple.
  auto group_as_one = f->function("together$step");
  group_as_one.ops[find_op(group_as_one, hir::op_kind::coroutine_await)].waits =
      hir::wait_mode::one;
  expect_rejects(*f, group_as_one, hir::verify_error_kind::type_mismatch,
                 "a one-task wait on a group", {}, "an await needs a task");

  // A step suspends instead of blocking the thread.
  auto blocks_in_step = f->function("together$step");
  const auto group = blocks_in_step.ops[find_op(blocks_in_step, hir::op_kind::coroutine_await)]
                         .source;
  blocks_in_step.ops.insert(
      blocks_in_step.ops.begin() +
          static_cast<std::ptrdiff_t>(find_op(blocks_in_step, hir::op_kind::coroutine_await)),
      hir::storage_op{.kind = hir::op_kind::task_block_on_group,
                      .source = group,
                      .waits = hir::wait_mode::all});
  expect_rejects(*f, blocks_in_step, hir::verify_error_kind::bad_operation,
                 "a step that blocks the thread");

  // Blocking waits for all of a group or for any of it.
  auto block_one = f->function("main");
  block_one.ops[find_op(block_one, hir::op_kind::task_block_on_group)].waits =
      hir::wait_mode::one;
  expect_rejects(*f, block_one, hir::verify_error_kind::bad_operation,
                 "a block on a group of one");

  auto block_int = f->function("main");
  block_int.ops[find_op(block_int, hir::op_kind::task_block_on_group)].source =
      place_at(0, block_int.slots[0].type);
  expect_rejects(*f, block_int, hir::verify_error_kind::type_mismatch,
                 "a block on something that is not a group", {},
                 "needs a tuple of tasks");

  // The cancel exit must drop every member of the group.
  auto leaked = f->function("together$step");
  leaked.ops.erase(leaked.ops.begin() +
                   static_cast<std::ptrdiff_t>(
                       find_op(leaked, hir::op_kind::drop_in_place)));
  expect_rejects(*f, leaked, hir::verify_error_kind::leaked_value,
                 "a cancelled wait that leaves a member of the group behind");
}

auto test_generator_constructor_builds_the_handle() -> void {
  auto f = build(read_program("generators.cn"));
  // The constructor is an ordinary function: it copies `n` into the state
  // block and returns the handle.
  expect_ops(*f, "count",
             {"generator s1 = count$step state 48/8 [copy s0 @0]",
              "return move s1"});
  expect_verifies(*f, f->function("count"), "count");
  const auto &made =
      f->function("count").ops[find_op(f->function("count"), hir::op_kind::make_generator)];
  expect(made.entry == "count$step" && made.env_size == 48 && made.env_align == 8,
         "expected the generator to name its step and size its state block");
}

auto test_generator_step_keeps_every_local_in_the_state_block() -> void {
  auto f = build(read_program("generators.cn"));
  const auto step = f->function("count$step");
  expect(step.coroutine_step && step.signature.takes_environment,
         "expected the step to take the state block");
  expect(step.slots[0].cls == hir::storage_class::parameter &&
             step.slots[1].cls == hir::storage_class::parameter,
         "expected the resume index and the coroutine to be parameters");
  auto offsets = std::vector<size_t>{};
  for (const auto &slot : step.slots) {
    if (slot.cls == hir::storage_class::coroutine_frame) {
      offsets.push_back(slot.env_offset);
    }
  }
  expect(offsets.size() == 6, "expected the parameter and every local in the block");
  expect(offsets.front() == 0, "expected `n` first, where the constructor copies it");
  expect_ops(*f, "count$step",
             {"dispatch s0 of 1", "cancelled s3 = s1", "if s3", "dead s3",
              "finish s1", "return #1", "else", "dead s3", "end_if",
              "compute s4 = const 0", "loop", "compute s5 = binary < s4, s2",
              "if s5", "dead s5", "else", "dead s5", "exit 0", "end_if",
              "block", "compute s6 = copy s4", "suspend #1 s1 <- s6", "dead s6",
              "cancelled s7 = s1", "if s7", "dead s7", "finish s1", "return #1",
              "else", "dead s7", "end_if", "compute s4 = binary + s4, #1",
              "end_block", "end_loop", "finish s1", "return #1"});
  expect_verifies(*f, step, "count$step");
}

auto test_next_resumes_then_takes_or_publishes_none() -> void {
  auto f = build(read_program("generators.cn"));
  expect_ops(*f, "sum_to",
             {"call s1 = count(s0)", "compute s2 = const 0", "loop",
              "resume s4 = s1", "compute s5 = binary == s4, #0", "if s5",
              "dead s5", "take s3.@some.0 <- s1", "publish s3 @some", "else",
              "dead s5", "publish s3 @none", "end_if", "dead s4",
              "test s7 = s3 is @some", "if s7", "dead s7",
              "compute s6 = const true", "else", "dead s7",
              "compute s6 = const false", "end_if", "if s6", "dead s6", "else",
              "dead s6", "exit 0", "end_if", "block",
              "compute s8 = copy s3.@some.0", "compute s2 = binary + s2, s8",
              "end_block", "dead s8", "dead s3", "end_loop",
              "compute s9 = copy s2", "drop s1", "return s9"});
  expect_verifies(*f, f->function("sum_to"), "sum_to");
  // Two generators in one function each get their own drop.
  auto drops = size_t{0};
  for (const auto &op : f->function("both").ops) {
    drops += op.kind == hir::op_kind::drop_in_place ? 1U : 0U;
  }
  expect(drops == 2, "expected each generator to be dropped once");
  expect_verifies(*f, f->function("both"), "both");
}

auto test_generators_not_lowered_yet_are_refused_by_name() -> void {
  auto f = build(read_program("generators.cn"));
  // Yielding such an item is fine: the suspension hands it over. Reading it
  // out needs the `option` that holds it to be dropped, which is not modeled.
  const auto *taken = f->refusal("take_owned");
  expect(taken != nullptr && taken->message.find("drop") != std::string::npos,
         "expected reading a droppable item to be refused");
  expect_verifies(*f, f->function("owned$step"), "owned$step");
}

auto test_verifier_checks_generators() -> void {
  auto f = build(read_program("generators.cn"));
  const auto step_index = [&](const hir::storage_function &fn, hir::op_kind kind,
                              size_t nth = 0) { return find_op(fn, kind, nth); };

  auto wrong_entry = f->function("count");
  wrong_entry.ops[step_index(wrong_entry, hir::op_kind::make_generator)].entry = "sum_to";
  expect_rejects(*f, wrong_entry, hir::verify_error_kind::signature_mismatch,
                 "an entry that is not a step function");

  auto outside = f->function("count");
  outside.ops[step_index(outside, hir::op_kind::make_generator)].captures[0].offset = 48;
  expect_rejects(*f, outside, hir::verify_error_kind::bad_operation,
                 "a capture outside the state block");

  // Step-only operations belong in a step.
  auto stray = f->function("sum_to");
  stray.ops.insert(stray.ops.begin(),
                   hir::storage_op{.kind = hir::op_kind::coroutine_finish,
                                   .source = place_at(1, stray.slots[1].type)});
  expect_rejects(*f, stray, hir::verify_error_kind::bad_operation,
                 "a step-only operation in an ordinary function");

  // A suspension point is placed once.
  auto twice = f->function("count$step");
  const auto at = step_index(twice, hir::op_kind::coroutine_suspend);
  auto again = twice.ops[at];
  twice.ops.insert(twice.ops.begin() + static_cast<std::ptrdiff_t>(at) + 1, again);
  expect_rejects(*f, twice, hir::verify_error_kind::bad_operation,
                 "a suspension point placed twice");

  // The item is consumed by the suspension.
  auto reread = f->function("count$step");
  const auto suspend = step_index(reread, hir::op_kind::coroutine_suspend);
  auto copy = hir::storage_op{.kind = hir::op_kind::compute,
                              .computation = hir::compute_kind::copy,
                              .dest = reread.ops[suspend].source};
  copy.operands.push_back(hir::operand{.kind = hir::operand_kind::read,
                                       .where = reread.ops[suspend].source,
                                       .type = reread.ops[suspend].source.type});
  reread.ops.insert(reread.ops.begin() + static_cast<std::ptrdiff_t>(suspend) + 1,
                    copy);
  expect_rejects(*f, reread, hir::verify_error_kind::uninitialized,
                 "an item read after it was handed over");

  // `take` must produce the type the generator yields, from a generator.
  auto mistyped = f->function("sum_to");
  mistyped.ops[step_index(mistyped, hir::op_kind::generator_take)].dest =
      place_at(5, mistyped.slots[5].type);
  expect_rejects(*f, mistyped, hir::verify_error_kind::type_mismatch,
                 "a take into a place of another type");

  auto not_a_generator = f->function("sum_to");
  not_a_generator.ops[step_index(not_a_generator, hir::op_kind::generator_take)]
      .source = place_at(2, not_a_generator.slots[2].type);
  expect_rejects(*f, not_a_generator, hir::verify_error_kind::type_mismatch,
                 "a take from a value that is not a generator");
}

auto test_groups_wait_for_every_task_or_the_first() -> void {
  auto f = build(read_program("tasks.cn"));
  expect(f->lowering.refused.empty(), "expected every task function to lower");
  // The branch tasks are started, then moved into one tuple that the runtime
  // reads in place. The step waits on the tuple as a whole; the cancel exit
  // drops its members, and so does the read after the wait.
  expect_ops(*f, "together$step",
             {"dispatch s0 of 2", "cancelled s3 = s1", "if s3", "dead s3",
              "finish s1", "return #1", "else", "dead s3", "end_if",
              "call s4 = double(s2)", "compute s6 = binary + s2, #1",
              "call s5 = double(s6)", "dead s6", "call s7 = rt_task_spawn(s4, #0)",
              "dead s7", "call s8 = rt_task_spawn(s5, #0)", "dead s8",
              "move s9.0 <- s4", "move s9.1 <- s5", "await all #1 s1 <- s9",
              "cancelled s10 = s1", "if s10", "dead s10", "drop s9.0",
              "drop s9.1", "finish s1", "return #1", "else", "dead s10",
              "end_if", "take s11.0 <- s9.0", "take s11.1 <- s9.1",
              "drop s9.0", "drop s9.1", "move s12 <- s11",
              "compute s13 = copy s12.0", "compute s14 = copy s12.1",
              "compute s15 = binary + s13, s14", "suspend #2 s1 <- s15",
              "dead s15", "finish s1", "return #1"});
  expect_verifies(*f, f->function("together$step"), "together$step");

  const auto &race = f->function("first_wins$step");
  const auto &any = race.ops[find_op(race, hir::op_kind::coroutine_await)];
  expect(any.waits == hir::wait_mode::any && any.depth == 1,
         "expected a race to wait for any one task");
  expect_verifies(*f, race, "first_wins$step");

  // The entry `async def main` is not a task, so its waits block the thread.
  const auto &entry = f->function("main");
  expect(entry.ops[find_op(entry, hir::op_kind::task_block_on_group, 0)].waits ==
                 hir::wait_mode::all &&
             entry.ops[find_op(entry, hir::op_kind::task_block_on_group, 1)]
                     .waits == hir::wait_mode::any,
         "expected `par` to wait for all and `race` for any");
  expect_verifies(*f, entry, "main");
}

auto test_task_constructor_and_step_hand_over_the_result() -> void {
  auto f = build(read_program("tasks.cn"));
  expect(f->lowering.refused.empty(), "expected every task function to lower");
  // The constructor is the generator's: it copies `x` into the state block
  // and returns the handle, which is a `task`.
  expect_ops(*f, "double",
             {"generator s1 = double$step state 24/8 [copy s0 @0]",
              "return move s1"});
  expect_verifies(*f, f->function("double"), "double");
  // The body hands its result over once and finishes.
  expect_ops(*f, "double$step",
             {"dispatch s0 of 1", "cancelled s3 = s1", "if s3", "dead s3",
              "finish s1", "return #1", "else", "dead s3", "end_if",
              "compute s4 = binary * s2, #2", "suspend #1 s1 <- s4", "dead s4",
              "finish s1", "return #1"});
  expect_verifies(*f, f->function("double$step"), "double$step");
  expect_verifies(*f, f->function("make_pair$step"), "make_pair$step");
}

auto test_await_is_a_resume_point_that_may_return_pending() -> void {
  auto f = build(read_program("tasks.cn"));
  // The awaited task is a local of the state block. After the wait the step
  // checks for a cancel, which drops the awaited task first; past it the
  // result is moved out and the task dropped.
  expect_ops(*f, "sum_doubles$step",
             {"dispatch s0 of 2", "cancelled s3 = s1", "if s3", "dead s3",
              "finish s1", "return #1", "else", "dead s3", "end_if",
              "compute s4 = const 0", "compute s5 = const 0", "loop",
              "compute s6 = binary < s5, s2", "if s6", "dead s6", "else",
              "dead s6", "exit 0", "end_if", "block", "call s7 = double(s5)",
              "await #1 s1 <- s7", "cancelled s8 = s1", "if s8", "dead s8",
              "drop s7", "finish s1", "return #1", "else", "dead s8", "end_if",
              "take s9 <- s7", "drop s7", "compute s4 = binary + s4, s9",
              "compute s5 = binary + s5, #1", "end_block", "dead s9", "dead s7",
              "end_loop", "compute s10 = copy s4", "suspend #2 s1 <- s10",
              "dead s10", "finish s1", "return #1"});
  expect_verifies(*f, f->function("sum_doubles$step"), "sum_doubles$step");
  const auto &step = f->function("sum_doubles$step");
  const auto &wait = step.ops[find_op(step, hir::op_kind::coroutine_await)];
  expect(wait.depth == 1 && wait.dest.base == 1,
         "expected the await to be the first resume point, in the task itself");
  // The awaited task lives in the state block, so it is there after a resume.
  expect(step.slots[wait.source.base].cls == hir::storage_class::coroutine_frame,
         "expected the awaited task to be held in the state block");
}

auto test_plain_functions_block_on_the_task() -> void {
  auto f = build(read_program("tasks.cn"));
  // No suspension to make: the thread waits, then the result is moved out.
  expect_ops(*f, "run_double",
             {"call s1 = double(s0)", "call s3 = rt_task_block_on(s1, #0)",
              "take s2 <- s1", "dead s3", "drop s1", "return s2"});
  expect_verifies(*f, f->function("run_double"), "run_double");
  // Spawning looks at the handle without taking it, so it can be read after.
  expect_ops(*f, "run_spawned",
             {"call s1 = double(s0)", "call s2 = rt_task_spawn(s1, #0)",
              "dead s2", "call s4 = rt_task_block_on(s1, #0)", "take s3 <- s1",
              "dead s4", "drop s1", "return s3"});
  expect_verifies(*f, f->function("run_spawned"), "run_spawned");
  // The entry `async def main` is an ordinary function that awaits this way.
  expect_ops(
      *f, "main",
      {"call s1 = double(#4)", "call s2 = double(#5)",
       "call s3 = rt_task_spawn(s1, #0)", "dead s3",
       "call s4 = rt_task_spawn(s2, #0)", "dead s4", "move s5.0 <- s1",
       "move s5.1 <- s2", "block_on_group all s5", "init_empty s6", "dead s6",
       "call s8 = rt_task_block_on(s5.0, #0)", "take s7.0 <- s5.0",
       "call s9 = rt_task_block_on(s5.1, #0)", "take s7.1 <- s5.1", "dead s9",
       "dead s8", "drop s5.0", "drop s5.1", "move s0 <- s7",
       "compute s10 = copy s0.0", "compute s11 = copy s0.1",
       "call s13 = double(#6)", "call s14 = double(#7)",
       "call s15 = rt_task_spawn(s13, #0)", "dead s15",
       "call s16 = rt_task_spawn(s14, #0)", "dead s16", "move s17.0 <- s13",
       "move s17.1 <- s14", "block_on_group any s17", "init_empty s18",
       "dead s18", "call s20 = rt_task_is_finished(s17.0)", "if s20", "dead s20",
       "call s21 = rt_task_block_on(s17.0, #0)", "take s19 <- s17.0", "dead s21",
       "else", "dead s20", "call s22 = rt_task_block_on(s17.1, #0)",
       "take s19 <- s17.1", "dead s22", "end_if", "call s23 = rt_task_cancel(s17.0)",
       "dead s23", "call s24 = rt_task_cancel(s17.1)", "dead s24",
       "drop s17.0", "drop s17.1", "compute s12 = copy s19",
       "compute s26 = binary + s10, s11", "compute s25 = binary + s26, s12",
       "dead s26", "return s25"});
  expect_verifies(*f, f->function("main"), "main");
  expect_verifies(*f, f->function("run_pair"), "run_pair");
}

auto test_dropping_a_task_drops_its_unread_result() -> void {
  auto f = build(read_program("task_results.cn"));
  expect(f->lowering.refused.size() == 1,
         "expected only the fallible task's handle to be refused");
  // The handle is dropped without a read. If it has finished and still holds
  // its result, the result is taken out and dropped before the handle goes.
  expect_ops(*f, "unread_noisy",
             {"call s1 = make_noisy(s0)", "compute s2 = copy s0",
              "call s3 = rt_task_is_finished(s1)", "if s3", "dead s3",
              "has_item s4 = s1", "if s4", "dead s4", "take s5 <- s1",
              "call.drop s6 = noisy::drop(&s5)", "dead s6", "dead s5", "else",
              "dead s4", "end_if", "else", "dead s3", "end_if", "drop s1",
              "return s2"});
  expect_verifies(*f, f->function("unread_noisy"), "unread_noisy");
  // A reader moves the result out and drops it itself; the drop of the handle
  // that follows still carries the check, which finds no item at run time.
  expect_verifies(*f, f->function("get_noisy"), "get_noisy");
  expect_verifies(*f, f->function("read_noisy$step"), "read_noisy$step");
  expect_verifies(*f, f->function("make_noisy$step"), "make_noisy$step");

  // Each droppable field and tuple member is dropped, in order.
  expect_ops(*f, "unread_holder",
             {"call s1 = make_holder(s0)", "compute s2 = copy s0",
              "call s3 = rt_task_is_finished(s1)", "if s3", "dead s3",
              "has_item s4 = s1", "if s4", "dead s4", "take s5 <- s1",
              "call.drop s6 = noisy::drop(&s5.first)", "dead s6",
              "call.drop s7 = noisy::drop(&s5.second)", "dead s7", "dead s5",
              "else", "dead s4", "end_if", "else", "dead s3", "end_if",
              "drop s1", "return s2"});
  expect_ops(*f, "unread_both",
             {"call s1 = make_both(s0)", "compute s2 = copy s0",
              "call s3 = rt_task_is_finished(s1)", "if s3", "dead s3",
              "has_item s4 = s1", "if s4", "dead s4", "take s5 <- s1",
              "call.drop s6 = noisy::drop(&s5.0)", "dead s6",
              "call.drop s7 = noisy::drop(&s5.1)", "dead s7", "dead s5",
              "else", "dead s4", "end_if", "else", "dead s3", "end_if",
              "drop s1", "return s2"});
  expect_verifies(*f, f->function("unread_holder"), "unread_holder");
  expect_verifies(*f, f->function("unread_both"), "unread_both");

  // A result whose drop is not modeled is refused by name, never leaked.
  const auto *fallible = f->refusal("unread_fallible");
  expect(fallible != nullptr &&
             fallible->message.find("needs a drop") != std::string::npos,
         "expected the drop of a fallible task result to be refused");

  // The result is taken out of the task, so it owes its own drop.
  auto leaked = f->function("unread_noisy");
  const auto dropped = find_op(leaked, hir::op_kind::call_into, 2);
  leaked.ops.erase(leaked.ops.begin() + static_cast<std::ptrdiff_t>(dropped));
  expect_rejects(*f, leaked, hir::verify_error_kind::leaked_value,
                 "an unread result that is taken and never dropped");

  // The item test reads a coroutine and writes a flag.
  auto not_a_handle = f->function("unread_noisy");
  auto &test = not_a_handle.ops[find_op(not_a_handle,
                                        hir::op_kind::coroutine_has_item)];
  test.source = place_at(0, not_a_handle.slots[0].type);
  expect_rejects(*f, not_a_handle, hir::verify_error_kind::type_mismatch,
                 "an item test of something that is not a coroutine");
  auto not_a_flag = f->function("unread_noisy");
  auto &flagged = not_a_flag.ops[find_op(not_a_flag,
                                         hir::op_kind::coroutine_has_item)];
  flagged.dest = place_at(0, not_a_flag.slots[0].type);
  expect_rejects(*f, not_a_flag, hir::verify_error_kind::type_mismatch,
                 "an item test written to something that is not a flag");
}

auto test_verifier_checks_tasks() -> void {
  auto f = build(read_program("tasks.cn"));

  // An await belongs in a step.
  auto stray = f->function("run_double");
  stray.ops.insert(stray.ops.begin(),
                   hir::storage_op{.kind = hir::op_kind::coroutine_await,
                                   .dest = place_at(1, stray.slots[1].type),
                                   .source = place_at(1, stray.slots[1].type),
                                   .depth = 1});
  expect_rejects(*f, stray, hir::verify_error_kind::bad_operation,
                 "an await in an ordinary function");

  // A resume point is placed once, and the dispatch must name it.
  auto twice = f->function("sum_doubles$step");
  const auto at = find_op(twice, hir::op_kind::coroutine_await);
  auto again = twice.ops[at];
  twice.ops.insert(twice.ops.begin() + static_cast<std::ptrdiff_t>(at) + 1, again);
  expect_rejects(*f, twice, hir::verify_error_kind::bad_operation,
                 "an await point placed twice");

  auto unnamed = f->function("sum_doubles$step");
  unnamed.ops[find_op(unnamed, hir::op_kind::coroutine_await)].depth = 3;
  expect_rejects(*f, unnamed, hir::verify_error_kind::bad_operation,
                 "an await point the dispatch does not name");

  // What is awaited, and what awaits, are tasks.
  auto not_a_task = f->function("sum_doubles$step");
  not_a_task.ops[find_op(not_a_task, hir::op_kind::coroutine_await)].source =
      place_at(2, not_a_task.slots[2].type);
  expect_rejects(*f, not_a_task, hir::verify_error_kind::type_mismatch,
                 "an await of a value that is not a task");

  auto no_self = f->function("sum_doubles$step");
  no_self.ops[find_op(no_self, hir::op_kind::coroutine_await)].dest =
      place_at(2, no_self.slots[2].type);
  expect_rejects(*f, no_self, hir::verify_error_kind::type_mismatch,
                 "an await by something that is not a task");

  // A task is not resumed by hand: only the scheduler runs its step.
  auto resumed = f->function("run_double");
  auto &take = resumed.ops[find_op(resumed, hir::op_kind::generator_take)];
  take.kind = hir::op_kind::generator_resume;
  take.dest = place_at(3, resumed.slots[3].type);
  expect_rejects(*f, resumed, hir::verify_error_kind::type_mismatch,
                 "a task resumed like a generator", {}, "resume needs a generator");

  // A generator is not awaited: only tasks are, and only the scheduler wakes
  // the waiter.
  auto awaits_generator = f->function("with_generator$step");
  auto &wait = awaits_generator.ops[find_op(awaits_generator, hir::op_kind::coroutine_await)];
  wait.source = awaits_generator.ops[find_op(awaits_generator, hir::op_kind::call_into)].dest;
  expect_rejects(*f, awaits_generator, hir::verify_error_kind::type_mismatch,
                 "an await of a generator", {}, "an await needs a task");

  // The result read is the type the task returns.
  auto mistyped = f->function("run_pair");
  mistyped.ops[find_op(mistyped, hir::op_kind::generator_take)].dest =
      place_at(0, mistyped.slots[0].type);
  expect_rejects(*f, mistyped, hir::verify_error_kind::type_mismatch,
                 "a task result read as another type");

  // A task that yields the wrong type of result.
  auto wrong_result = f->function("make_pair$step");
  wrong_result.ops[find_op(wrong_result, hir::op_kind::coroutine_suspend)].source =
      place_at(2, wrong_result.slots[2].type);
  expect_rejects(*f, wrong_result, hir::verify_error_kind::type_mismatch,
                 "a task that yields a value of another type");

  // The awaited task must not be left behind when the wait is cancelled.
  auto leaked = f->function("sum_doubles$step");
  const auto cancel_drop = find_op(leaked, hir::op_kind::drop_in_place);
  leaked.ops.erase(leaked.ops.begin() + static_cast<std::ptrdiff_t>(cancel_drop));
  expect_rejects(*f, leaked, hir::verify_error_kind::leaked_value,
                 "a cancelled wait that leaves the awaited task behind");
}

auto main() -> int {
  try {
    test_groups_wait_for_every_task_or_the_first();
    test_verifier_checks_task_groups();
    test_task_constructor_and_step_hand_over_the_result();
    test_await_is_a_resume_point_that_may_return_pending();
    test_plain_functions_block_on_the_task();
    test_dropping_a_task_drops_its_unread_result();
    test_verifier_checks_tasks();
    test_generator_constructor_builds_the_handle();
    test_generator_step_keeps_every_local_in_the_state_block();
    test_next_resumes_then_takes_or_publishes_none();
    test_generators_not_lowered_yet_are_refused_by_name();
    test_verifier_checks_generators();
    test_closure_is_built_with_its_captures_and_glue();
    test_aggregate_captures_sit_at_their_own_alignment();
    test_moved_captures_are_dropped_by_the_glue();
    test_reference_captures_are_reached_through_an_address();
    test_verifier_checks_closures();
    test_match_tests_the_tag_and_binds_payloads();
    test_statement_match_writes_through_its_arms();
    test_literal_patterns_compare_after_the_tag();
    test_aggregate_payload_moves_out_of_the_sum();
    test_while_let_and_let_else_lower();
    test_patterns_not_lowered_yet_are_refused_by_name();
    test_verifier_checks_variant_tests();
    test_abi_classes_follow_layout();
    test_signature_operands_hide_the_result_address();
    test_signature_agreement_ignores_type_ids_but_not_shape();
    test_function_value_and_function_share_a_signature();
    test_synthesized_functions_classify_with_handle_results();
    test_constructor_is_built_in_the_result_bytes();
    test_nested_call_result_is_forwarded_into_the_field();
    test_returned_call_receives_the_result_address();
    test_sum_is_published_after_its_payload();
    test_aggregate_arguments_use_outgoing_slots();
    test_copy_classification_chooses_copy_or_move();
    test_discarded_result_gets_temporary_storage();
    test_replacement_is_built_apart_from_the_target();
    test_projection_through_a_reference_dereferences();
    test_indirect_call_goes_through_the_function_value();
    test_scalar_and_unit_functions();
    test_unsupported_constructs_are_refused_by_name();
    test_every_lowered_function_verifies();
    test_dump_is_stable();
    test_droppable_values_lower_and_verify();
    test_scope_exit_drop_is_a_marked_call();
    test_discarded_temporary_is_dropped();
    test_replacement_drops_the_old_value_first();
    test_fields_drop_in_order_and_moved_fields_are_skipped();
    test_verifier_rejects_incomplete_results();
    test_verifier_rejects_missing_or_extra_returns();
    test_verifier_rejects_reads_of_unfinished_values();
    test_verifier_rejects_use_after_dead();
    test_verifier_checks_projection_bounds_and_types();
    test_verifier_checks_transfer_rules();
    test_verifier_rejects_moves_out_of_references_and_sums();
    test_verifier_checks_call_signatures();
    test_verifier_checks_argument_consumption();
    test_verifier_rejects_a_result_that_aliases_a_borrow();
    test_verifier_rejects_double_construction_of_a_droppable();
    test_verifier_reports_values_that_owe_a_drop();
    test_verifier_rejects_second_and_late_drops();
    test_verifier_checks_the_shape_of_a_drop_call();
    test_verifier_checks_slots_and_signature();
    test_control_flow_lowers_and_verifies();
    test_if_branches_build_the_value_in_the_destination();
    test_loops_test_run_and_repeat();
    test_break_and_continue_name_their_loop_by_depth();
    test_logical_operators_do_not_run_the_right_side_eagerly();
    test_drops_inside_branches_and_loops();
    test_verifier_checks_joins_and_loops();
    test_raw_index_is_a_pointer_offset_then_a_dereference();
    test_verifier_checks_the_operands_of_an_offset();
    test_raw_reads_own_the_value_and_raw_writes_drop_nothing();
    test_constant_array_index_is_an_element_projection();
  } catch (const std::exception &e) {
    cinder::testing::fail(e.what());
  }
  return 0;
}
