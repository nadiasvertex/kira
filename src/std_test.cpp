#include "cli.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

[[noreturn]] auto fail(const std::string &message) -> void {
  std::cerr << "std_test failed: " << message << '\n';
  std::exit(1);
}

auto expect(bool condition, const std::string &message) -> void {
  if (!condition) {
    fail(message);
  }
}

struct temp_dir {
  fs::path path;

  ~temp_dir() {
    auto ec = std::error_code{};
    fs::remove_all(path, ec);
  }
};

auto make_temp_dir() -> temp_dir {
  auto base =
      fs::temp_directory_path() /
      std::format("kira_std_test_{}",
                  std::chrono::steady_clock::now().time_since_epoch().count());
  auto ec = std::error_code{};
  fs::create_directories(base, ec);
  expect(!ec, std::format("expected to create temporary directory: {}",
                          ec.message()));
  return temp_dir{.path = std::move(base)};
}

auto candidate_corpus_dirs(std::string_view argv0) -> std::vector<fs::path> {
  auto candidates = std::vector<fs::path>{};

  if (const auto *srcdir = std::getenv("TEST_SRCDIR"); srcdir != nullptr) {
    if (const auto *workspace = std::getenv("TEST_WORKSPACE");
        workspace != nullptr && *workspace != '\0') {
      candidates.emplace_back(fs::path(srcdir) / workspace /
                              "src/testdata/std_test");
    }
    candidates.emplace_back(fs::path(srcdir) / "_main" /
                            "src/testdata/std_test");
  }

  if (!argv0.empty()) {
    candidates.emplace_back(fs::path(std::string(argv0) + ".runfiles") /
                            "_main" / "src/testdata/std_test");
  }

  candidates.emplace_back("src/testdata/std_test");
  return candidates;
}

auto find_corpus_dir(std::string_view argv0) -> fs::path {
  for (const auto &candidate : candidate_corpus_dirs(argv0)) {
    auto ec = std::error_code{};
    if (fs::is_directory(candidate, ec)) {
      return candidate;
    }
  }

  fail("could not locate std_test corpus directory");
  std::abort();
}

/// One `<name>.kira` sample paired with the `<name>.expected` stdout text
/// `println`/`print` are expected to produce when `main` runs to completion.
struct sample {
  std::string name;
  fs::path source_path;
  std::string expected_stdout;
};

auto list_samples(const fs::path &corpus_dir) -> std::vector<sample> {
  auto samples = std::vector<sample>{};
  for (const auto &entry : fs::directory_iterator(corpus_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".kira") {
      continue;
    }
    const auto expected_path = entry.path().parent_path() /
                               (entry.path().stem().string() + ".expected");
    expect(fs::exists(expected_path),
           std::format("expected `{}` to have a matching `{}`",
                       entry.path().string(), expected_path.string()));

    auto expected_stream = std::ifstream(expected_path, std::ios::binary);
    auto expected_content = std::ostringstream{};
    expected_content << expected_stream.rdbuf();

    samples.push_back(sample{.name = entry.path().stem().string(),
                             .source_path = entry.path(),
                             .expected_stdout = expected_content.str()});
  }

  std::ranges::sort(samples, {}, &sample::name);
  return samples;
}

/// Runs `source_path`'s `main` via the tier-0 bytecode VM
/// (`driver::compile_sources` with `cfg.run = true`) with real process
/// stdout (fd 1) redirected to a temp file for the duration of the call —
/// `println`/`print` write through `rt_write`'s real `::write(1, ...)`
/// syscall (`src/bytecode/vm.cpp`), not anything `compile_report` captures,
/// so this is the only way to observe what a sample actually printed.
auto run_sample_capturing_stdout(const sample &sample, const fs::path &tmp_dir)
    -> std::string {
  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {sample.source_path.string()},
      .metadata_dir = (tmp_dir / "meta").string(),
      .show_help = false,
      .run = true,
      .run_function = "main",
  };
  kira::driver::inject_stdlib_prelude(cfg);

  const auto capture_path = tmp_dir / (sample.name + ".stdout");
  const auto capture_fd =
      ::open(capture_path.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  expect(capture_fd >= 0, std::format("expected to open `{}` for capturing "
                                      "stdout",
                                      capture_path.string()));

  const auto saved_stdout_fd = ::dup(STDOUT_FILENO);
  expect(saved_stdout_fd >= 0, "expected to save the real stdout fd");
  ::fflush(stdout);
  expect(::dup2(capture_fd, STDOUT_FILENO) >= 0,
         "expected to redirect stdout to the capture file");
  ::close(capture_fd);

  auto report = kira::driver::compile_sources(cfg, false);

  ::fflush(stdout);
  ::dup2(saved_stdout_fd, STDOUT_FILENO);
  ::close(saved_stdout_fd);

  expect(report.has_value(),
         std::format("[{}] expected compile_sources to return a report",
                     sample.name));
  expect(report->error_count == 0,
         std::format("[{}] expected the sample to compile cleanly: {}",
                     sample.name, report->diagnostics));
  expect(report->run.has_value(),
         std::format("[{}] expected a run outcome to be recorded",
                     sample.name));
  expect(report->run->succeeded,
         std::format("[{}] expected `main` to run successfully: {}",
                     sample.name, report->run->message));

  auto capture_stream = std::ifstream(capture_path, std::ios::binary);
  auto captured = std::ostringstream{};
  captured << capture_stream.rdbuf();
  return captured.str();
}

} // namespace

auto main(int argc, char *argv[]) -> int {
  try {
    auto corpus_dir = find_corpus_dir(argc > 0 ? std::string_view(*argv)
                                               : std::string_view{});
    auto samples = list_samples(corpus_dir);

    expect(!samples.empty(), "expected std_test corpus to contain samples");

    for (const auto &sample : samples) {
      auto dir = make_temp_dir();
      const auto actual_stdout = run_sample_capturing_stdout(sample, dir.path);
      expect(actual_stdout == sample.expected_stdout,
             std::format("[{}] expected stdout:\n{}\n---\ngot:\n{}",
                         sample.name, sample.expected_stdout, actual_stdout));
    }

    std::cout << "std_test: " << samples.size() << " sample(s) passed\n";
  } catch (const std::exception &ex) {
    std::cerr << "std_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }

  return 0;
}
