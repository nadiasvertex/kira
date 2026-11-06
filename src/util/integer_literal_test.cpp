#include "src/util/integer_literal.h"

#include "src/testing/test_assert.h"

using cinder::testing::expect;
using cinder::util::format_wide_decimal;
using cinder::util::parse_integer_literal_wide;

namespace {

auto u128(uint64_t hi, uint64_t lo) -> unsigned __int128 {
  return (static_cast<unsigned __int128>(hi) << 64) | lo;
}

} // namespace

auto main() -> int {
  expect(format_wide_decimal(0) == "0", "format 0");
  expect(cinder::util::format_wide_signed_decimal(
             static_cast<__int128>(u128(uint64_t{1} << 63, 0))) ==
             "-170141183460469231731687303715884105728",
         "format int128 min");
  expect(cinder::util::format_wide_signed_decimal(-42) == "-42",
         "format negative");
  expect(format_wide_decimal(u128(~uint64_t{0}, ~uint64_t{0})) ==
             "340282366920938463463374607431768211455",
         "format uint128 max");
  expect(parse_integer_literal_wide("0") == u128(0, 0), "0");
  expect(parse_integer_literal_wide("1_000") == u128(0, 1000), "separators");
  expect(parse_integer_literal_wide("0xff") == u128(0, 255), "hex");
  expect(parse_integer_literal_wide("0o17") == u128(0, 15), "octal");
  expect(parse_integer_literal_wide("0b101") == u128(0, 5), "binary");
  // 2^64: one past what a 64-bit parser could hold.
  expect(parse_integer_literal_wide("18446744073709551616") == u128(1, 0),
         "2^64");
  // uint128's maximum, decimal and hex.
  expect(
      parse_integer_literal_wide("340282366920938463463374607431768211455") ==
          u128(~uint64_t{0}, ~uint64_t{0}),
      "uint128 max decimal");
  expect(
      parse_integer_literal_wide("0xffff_ffff_ffff_ffff_ffff_ffff_ffff_ffff") ==
          u128(~uint64_t{0}, ~uint64_t{0}),
      "uint128 max hex");
  expect(!parse_integer_literal_wide("340282366920938463463374607431768211456")
              .has_value(),
         "2^128 does not fit");
  expect(
      !parse_integer_literal_wide("0x1_0000_0000_0000_0000_0000_0000_0000_0000")
           .has_value(),
      "2^128 hex does not fit");
  expect(!parse_integer_literal_wide("12a").has_value(), "bad digit");
  expect(!parse_integer_literal_wide("0b2").has_value(), "digit past base");
  expect(!parse_integer_literal_wide("0x").has_value(), "no digits");
  return 0;
}
