// Tests for the check between a call's signature and the runtime symbol of an
// intrinsic (`src/hir/intrinsic_abi.h`). Signatures are built by hand with the
// classes the storage form gives each type.

#include <string>

#include "src/hir/intrinsic_abi.h"
#include "src/testing/test_assert.h"

namespace {

using cinder::hir::abi_signature;
using cinder::hir::abi_value;
using cinder::hir::check_intrinsic_signature;
using cinder::hir::pass_class;
using cinder::hir::runtime_intrinsic_id;
using cinder::testing::expect;

auto value(pass_class cls, size_t size) -> abi_value {
  return abi_value{.cls = cls, .size = size, .align = size >= 8 ? 8U : 1U};
}

auto id_of(const char *name) -> size_t {
  const auto id = runtime_intrinsic_id(name);
  expect(id.has_value(), "expected the intrinsic to be known");
  return *id;
}

auto mentions(const std::optional<std::string> &problem, const char *text)
    -> bool {
  return problem.has_value() && problem->find(text) != std::string::npos;
}

auto test_matching_signatures_pass() -> void {
  // rt_close(raw_fd) -> result[unit, io_errno]: both by address.
  const auto close = abi_signature{
      .result = value(pass_class::aggregate, 8),
      .params = {value(pass_class::aggregate, 8)}};
  expect(!check_intrinsic_signature(close, id_of("rt_close")).has_value(),
         "expected rt_close to match");

  // rt_str_len_scalars(str) -> usize: a register result.
  const auto length = abi_signature{.result = value(pass_class::scalar, 8),
                                    .params = {value(pass_class::descriptor, 16)}};
  expect(!check_intrinsic_signature(length, id_of("rt_str_len_scalars"))
              .has_value(),
         "expected rt_str_len_scalars to match");

  // rt_fmt_f64_fixed(float64, usize) -> str: scalars in, a descriptor out.
  const auto fixed = abi_signature{
      .result = value(pass_class::descriptor, 16),
      .params = {value(pass_class::scalar, 8), value(pass_class::scalar, 8)}};
  expect(!check_intrinsic_signature(fixed, id_of("rt_fmt_f64_fixed")).has_value(),
         "expected rt_fmt_f64_fixed to match");

  // A zero-sized parameter takes no operand and is not counted.
  const auto with_unit = abi_signature{
      .result = value(pass_class::aggregate, 8),
      .params = {value(pass_class::zero_sized, 0),
                 value(pass_class::aggregate, 8)}};
  expect(!check_intrinsic_signature(with_unit, id_of("rt_close")).has_value(),
         "expected a zero-sized parameter to be ignored");
}

auto test_result_class_must_agree_with_the_symbol() -> void {
  // rt_str_reverse builds its result in caller storage; a register result
  // would read an address as a number.
  const auto scalar_result = abi_signature{
      .result = value(pass_class::scalar, 8),
      .params = {value(pass_class::descriptor, 16)}};
  expect(mentions(check_intrinsic_signature(scalar_result,
                                            id_of("rt_str_reverse")),
                  "writes its result into caller storage"),
         "expected a register result for an `_into` symbol to be refused");

  // rt_str_eq returns a number; an aggregate result expects a destination.
  const auto aggregate_result = abi_signature{
      .result = value(pass_class::aggregate, 8),
      .params = {value(pass_class::descriptor, 16),
                 value(pass_class::descriptor, 16)}};
  expect(mentions(check_intrinsic_signature(aggregate_result,
                                            id_of("rt_str_eq")),
                  "returns its result in a register"),
         "expected a built result for a register symbol to be refused");

  // rt_str_len_scalars returns 64 bits; a 4-byte result does not fit.
  const auto narrow = abi_signature{
      .result = value(pass_class::scalar, 4),
      .params = {value(pass_class::descriptor, 16)}};
  expect(mentions(check_intrinsic_signature(narrow,
                                            id_of("rt_str_len_scalars")),
                  "does not fit the runtime symbol's return type"),
         "expected a scalar of the wrong width to be refused");
}

auto test_parameters_must_agree_with_the_symbol() -> void {
  // A descriptor where the symbol takes a number.
  const auto descriptor_for_number = abi_signature{
      .result = value(pass_class::descriptor, 16),
      .params = {value(pass_class::descriptor, 16),
                 value(pass_class::scalar, 8)}};
  expect(mentions(check_intrinsic_signature(descriptor_for_number,
                                            id_of("rt_fmt_f64_fixed")),
                  "parameter 1"),
         "expected a descriptor passed for a number to be refused");

  // A number where the symbol takes an address.
  const auto number_for_address = abi_signature{
      .result = value(pass_class::aggregate, 8),
      .params = {value(pass_class::scalar, 8)}};
  expect(mentions(check_intrinsic_signature(number_for_address,
                                            id_of("rt_close")),
                  "does not fit"),
         "expected a number passed for an address to be refused");

  // Too many and too few parameters.
  const auto too_many = abi_signature{
      .result = value(pass_class::aggregate, 8),
      .params = {value(pass_class::aggregate, 8),
                 value(pass_class::aggregate, 8)}};
  expect(mentions(check_intrinsic_signature(too_many, id_of("rt_close")),
                  "more parameters than its runtime symbol takes"),
         "expected an extra parameter to be refused");
  const auto too_few = abi_signature{.result = value(pass_class::aggregate, 8),
                                     .params = {}};
  expect(mentions(check_intrinsic_signature(too_few, id_of("rt_close")),
                  "declared with 0 parameters"),
         "expected a missing parameter to be refused");
}

auto test_inline_intrinsics_have_no_runtime_symbol() -> void {
  expect(!runtime_intrinsic_id("not_an_intrinsic").has_value(),
         "expected an unknown name to be refused");
}

} // namespace

auto main() -> int {
  test_matching_signatures_pass();
  test_result_class_must_agree_with_the_symbol();
  test_parameters_must_agree_with_the_symbol();
  test_inline_intrinsics_have_no_runtime_symbol();
  return 0;
}
