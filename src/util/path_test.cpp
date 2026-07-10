#include <exception>
#include <filesystem>
#include <iostream>

#include "src/testing/test_assert.h"
#include "src/util/path.h"

namespace {

using kira::testing::expect;

/// `resolve_self_executable` locates this very test binary — used by
/// `find_stdlib_source_file` (`src/driver/driver.cpp`) and
/// `find_bazel_archive` (`src/driver/aot.cpp`) to find sibling support
/// files for a `just package` install, independent of cwd or `argv[0]`.
auto test_resolve_self_executable_finds_running_binary() -> void {
  const auto resolved = kira::util::resolve_self_executable();
  expect(resolved.has_value(),
         "expected resolve_self_executable to locate this platform's "
         "running test binary");

  auto ec = std::error_code{};
  expect(std::filesystem::exists(resolved.value(), ec),
         "expected the resolved self-executable path to exist on disk");
  expect(resolved.value().is_absolute(),
         "expected the resolved self-executable path to be absolute");
}

} // namespace

auto main() -> int {
  try {
    test_resolve_self_executable_finds_running_binary();
  } catch (const std::exception &ex) {
    std::cerr << "path_test failed: unhandled exception: " << ex.what() << '\n';
    std::exit(1);
  }
  return 0;
}
