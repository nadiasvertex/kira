#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "src/testing/test_assert.h"

namespace kira::testing {

namespace fs = std::filesystem;

auto candidate_test_data_dirs(std::string_view test_name)
    -> std::vector<fs::path> {
  auto candidates = std::vector<fs::path>{};

  if (const auto *srcdir = std::getenv("TEST_SRCDIR"); srcdir != nullptr) {
    if (const auto *workspace = std::getenv("TEST_WORKSPACE");
        workspace != nullptr && *workspace != '\0') {
      candidates.emplace_back(fs::path(srcdir) / workspace / "src/testdata" /
                              test_name);
    }
    candidates.emplace_back(fs::path(srcdir) / "_main" / "src/testdata" /
                            test_name);
  }

  candidates.emplace_back(fs::path("src/testdata") / test_name);
  return candidates;
}

auto find_test_data_dir(std::string_view test_name) -> fs::path {
  for (const auto &candidate : candidate_test_data_dirs(test_name)) {
    auto ec = std::error_code{};
    if (fs::is_directory(candidate, ec)) {
      return candidate;
    }
  }

  fail(std::string("could not locate ") + std::string(test_name) +
       " test data directory");
  std::abort();
}

auto load_test_data_file(std::string_view test_dir, std::string_view filename)
    -> std::string {
  const auto path = fs::path(test_dir) / filename;
  auto file = std::ifstream(path);
  if (!file.is_open()) {
    fail(std::string("could not open test data file: ") + path.string());
  }

  auto content = std::string{};
  auto line = std::string{};
  while (std::getline(file, line)) {
    content += line;
    content += '\n';
  }

  return content;
}

} // namespace kira::testing
