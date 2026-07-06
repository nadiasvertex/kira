#pragma once

// ==========================================================================
//  Kira Language — Shared Test Assertion Helpers
//
//  Small fail()/expect() primitives shared by the hand-rolled test binaries
//  under src/k-parser and src/semantic, so each one doesn't reimplement its
//  own copy.
// ==========================================================================

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace kira::testing {

[[noreturn]] inline auto fail(std::string_view message) -> void {
  std::cerr << "test failed: " << message << '\n';
  std::exit(1);
}

inline auto expect(bool condition, std::string_view message) -> void {
  if (!condition) {
    fail(message);
  }
}

} // namespace kira::testing
