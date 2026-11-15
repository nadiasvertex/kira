#include "driver/driver.h"
#include "src/testing/test_assert.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

// Each file in `src/testdata/known_gaps/` reproduces one open item of
// `spec/todo.md` and records, in `#` directives, the wrong behaviour the
// compiler shows today. This test passes while the gap is open and fails once
// the behaviour changes, so a fix has to move the program into the regular
// corpus.
//
//   # todo: <item number>                       required
//   # tier: vm | build | both                   optional, default both
//   # currently-error: <substring>              exactly one of these forms
//   # currently-accepted
//   # currently-result: <integer>               alone, or with currently-accepted
//   # currently-crash[: <signal number>]        the process dies from a signal
//   # want: <free text>                         required
//
// `currently-crash` means the VM tier's process (running the program), or the
// built executable on the build tier, is killed by a signal (any signal, or
// the one named). A crash seen where another behaviour is recorded is an
// ordinary "no longer behaves as recorded" failure.
//
// Every (file, tier) pair runs in its own child process: this binary runs
// itself again with `--child <file> <tier> <result file> <scratch dir>`, and
// the child writes what it observed to the result file. A fresh process gives
// each program a fresh compiler (nothing leaks between compilations) and
// makes a crash an observation instead of the end of the test. The children
// run a few at a time.

extern char **environ; // NOLINT

namespace {

namespace fs = std::filesystem;
using cinder::testing::expect;
using cinder::testing::fail;

struct temp_dir {
  fs::path path;

  ~temp_dir() {
    auto ec = std::error_code{};
    fs::remove_all(path, ec);
  }
};

auto make_temp_dir() -> temp_dir {
  static auto counter = std::atomic<unsigned>{0};
  auto base = fs::temp_directory_path() /
              std::format("cinder_known_gaps_{}_{}_{}", getpid(), counter++,
                          std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count());
  auto ec = std::error_code{};
  fs::create_directories(base, ec);
  expect(!ec, "expected to create a temporary directory");
  return temp_dir{.path = std::move(base)};
}

auto find_corpus_dir(std::string_view argv0) -> fs::path {
  auto candidates = std::vector<fs::path>{};
  if (const auto *srcdir = std::getenv("TEST_SRCDIR"); srcdir != nullptr) {
    if (const auto *workspace = std::getenv("TEST_WORKSPACE");
        workspace != nullptr && *workspace != '\0') {
      candidates.emplace_back(fs::path(srcdir) / workspace /
                              "src/testdata/known_gaps");
    }
    candidates.emplace_back(fs::path(srcdir) / "_main" /
                            "src/testdata/known_gaps");
  }
  if (!argv0.empty()) {
    candidates.emplace_back(fs::path(std::string(argv0) + ".runfiles") /
                            "_main" / "src/testdata/known_gaps");
  }
  candidates.emplace_back("src/testdata/known_gaps");
  for (const auto &candidate : candidates) {
    auto ec = std::error_code{};
    if (fs::is_directory(candidate, ec)) {
      return candidate;
    }
  }
  fail("could not locate the known_gaps corpus directory");
}

auto read_file(const fs::path &path) -> std::string {
  auto in = std::ifstream(path);
  auto text = std::stringstream{};
  text << in.rdbuf();
  return text.str();
}

/// Every value of the directive `marker` (at the start of a line), trimmed.
auto directive_values(std::string_view text, std::string_view marker)
    -> std::vector<std::string> {
  auto values = std::vector<std::string>{};
  auto rest = text;
  while (!rest.empty()) {
    const auto end = rest.find('\n');
    const auto line = rest.substr(0, end);
    if (line.starts_with(marker)) {
      auto value = std::string(line.substr(marker.size()));
      while (!value.empty() && value.front() == ' ') {
        value.erase(value.begin());
      }
      while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) {
        value.pop_back();
      }
      values.push_back(std::move(value));
    }
    rest = end == std::string_view::npos ? std::string_view{}
                                         : rest.substr(end + 1);
  }
  return values;
}

enum class tier { vm, build };

auto tier_name(tier which) -> std::string_view {
  return which == tier::vm ? "vm" : "build";
}

struct recorded_behavior {
  bool is_error = false;
  std::string error_substring;      // when is_error
  std::optional<int> result;        // when !is_error and pinned
  bool is_crash = false;
  std::optional<int> crash_signal;  // when is_crash and a signal is named
};

struct known_gap {
  std::string file;
  std::string todo;
  std::vector<tier> tiers;
  recorded_behavior recorded;
  std::string want;
};

auto parse_integer(const std::string &text, const std::string &file,
                   std::string_view marker) -> int {
  try {
    size_t used = 0;
    const auto value = std::stoi(text, &used);
    expect(used == text.size(), "trailing text");
    return value;
  } catch (const std::exception &) {
    fail(std::format("{}: `{}` needs an integer, got `{}`", file, marker,
                     text));
  }
}

auto parse_gap(const fs::path &path) -> known_gap {
  const auto file = path.filename().string();
  const auto text = read_file(path);
  auto gap = known_gap{.file = file};

  const auto single = [&](std::string_view marker) -> std::optional<std::string> {
    const auto values = directive_values(text, marker);
    expect(values.size() <= 1,
           std::format("{}: duplicate `{}` directive", file, marker));
    if (values.empty()) {
      return std::nullopt;
    }
    return values.front();
  };

  const auto todo = single("# todo:");
  expect(todo.has_value() && !todo->empty(),
         std::format("{}: missing `# todo:` directive", file));
  gap.todo = *todo;

  const auto want = single("# want:");
  expect(want.has_value() && !want->empty(),
         std::format("{}: missing `# want:` directive", file));
  gap.want = *want;

  const auto tier_text = single("# tier:").value_or("both");
  if (tier_text == "vm") {
    gap.tiers = {tier::vm};
  } else if (tier_text == "build") {
    gap.tiers = {tier::build};
  } else if (tier_text == "both") {
    gap.tiers = {tier::vm, tier::build};
  } else {
    fail(std::format("{}: `# tier:` must be vm, build, or both, got `{}`",
                     file, tier_text));
  }

  const auto error = single("# currently-error:");
  const auto accepted = directive_values(text, "# currently-accepted");
  expect(accepted.size() <= 1,
         std::format("{}: duplicate `# currently-accepted` directive", file));
  const auto result = single("# currently-result:");
  const auto crash = directive_values(text, "# currently-crash");
  expect(crash.size() <= 1,
         std::format("{}: duplicate `# currently-crash` directive", file));

  expect(!(error.has_value() && (!accepted.empty() || result.has_value())),
         std::format("{}: `# currently-error:` excludes the other `currently` "
                     "directives",
                     file));
  expect(crash.empty() ||
             !(error.has_value() || !accepted.empty() || result.has_value()),
         std::format("{}: `# currently-crash` excludes the other `currently` "
                     "directives",
                     file));
  expect(error.has_value() || !accepted.empty() || result.has_value() ||
             !crash.empty(),
         std::format("{}: missing a `currently` directive "
                     "(`# currently-error:`, `# currently-accepted`, "
                     "`# currently-result:`, or `# currently-crash`)",
                     file));
  if (!crash.empty()) {
    gap.recorded.is_crash = true;
    auto number = crash.front();
    if (!number.empty()) {
      expect(number.front() == ':',
             std::format("{}: unexpected text after `# currently-crash`",
                         file));
      number.erase(number.begin());
      while (!number.empty() && number.front() == ' ') {
        number.erase(number.begin());
      }
      gap.recorded.crash_signal =
          parse_integer(number, file, "# currently-crash:");
    }
  }
  if (error.has_value()) {
    expect(!error->empty(),
           std::format("{}: `# currently-error:` needs a substring", file));
    gap.recorded.is_error = true;
    gap.recorded.error_substring = *error;
  } else if (result.has_value()) {
    gap.recorded.result = parse_integer(*result, file, "# currently-result:");
  }
  return gap;
}

struct observed_behavior {
  bool is_error = false;
  std::string diagnostics;  // when is_error
  std::optional<int> result; // when accepted and it ran
  std::string detail;       // why `result` is missing, if it is
  std::optional<int> crash_signal; // set when the process died from a signal
};

auto describe(const observed_behavior &observed) -> std::string {
  if (observed.crash_signal.has_value()) {
    return std::format("a crash (signal {})", *observed.crash_signal);
  }
  if (observed.is_error) {
    return std::format("an error: {}", observed.diagnostics);
  }
  if (observed.result.has_value()) {
    return std::format("accepted, result {}", *observed.result);
  }
  return std::format("accepted, no result ({})", observed.detail);
}

auto describe(const recorded_behavior &recorded) -> std::string {
  if (recorded.is_crash) {
    return recorded.crash_signal.has_value()
               ? std::format("a crash (signal {})", *recorded.crash_signal)
               : std::string{"a crash"};
  }
  if (recorded.is_error) {
    return std::format("an error containing `{}`", recorded.error_substring);
  }
  if (recorded.result.has_value()) {
    return std::format("accepted, result {}", *recorded.result);
  }
  return "accepted";
}

struct process_status {
  bool spawned = false;
  std::optional<int> exit_code;
  std::optional<int> signal;
};

/// Runs `argv` with stdout and stderr sent to `output` and waits for it.
auto run_process(const std::vector<std::string> &args, const fs::path &output)
    -> process_status {
  auto actions = posix_spawn_file_actions_t{};
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 1, output.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0600);
  posix_spawn_file_actions_adddup2(&actions, 1, 2);
  auto argv = std::vector<char *>{};
  for (const auto &arg : args) {
    argv.push_back(const_cast<char *>(arg.c_str())); // NOLINT
  }
  argv.push_back(nullptr);
  auto pid = pid_t{};
  const auto rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(),
                              environ);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) {
    return {};
  }
  auto status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return {.spawned = true};
    }
  }
  if (WIFSIGNALED(status)) {
    return {.spawned = true, .signal = WTERMSIG(status)};
  }
  if (WIFEXITED(status)) {
    return {.spawned = true, .exit_code = WEXITSTATUS(status)};
  }
  return {.spawned = true};
}

/// Runs the built executable and turns how it ended into an observation.
auto run_executable(const fs::path &exe, const fs::path &scratch)
    -> observed_behavior {
  const auto status = run_process({exe.string()}, scratch / "program.out");
  if (status.signal.has_value()) {
    return {.crash_signal = status.signal};
  }
  if (!status.exit_code.has_value()) {
    return {.detail = "the built executable did not exit normally"};
  }
  return {.result = *status.exit_code};
}

auto observe(const fs::path &source, tier which, const fs::path &scratch)
    -> observed_behavior {
  auto cfg = cinder::driver::cli_config{
      .program_name = "cinder",
      .sources = {source.string()},
      .metadata_dir = (scratch / "meta").string(),
      .show_help = false,
  };
  const auto exe = scratch / "program";
  if (which == tier::vm) {
    cfg.run = true;
    cfg.run_function = "main";
  } else {
    cfg.build = true;
    cfg.build_function = "main";
    cfg.build_output = exe.string();
  }
  cinder::driver::inject_stdlib_prelude(cfg);

  auto report = cinder::driver::compile_sources(cfg, false);
  if (!report.has_value()) {
    return {.is_error = true, .diagnostics = report.error()};
  }
  if (report->error_count != 0) {
    return {.is_error = true, .diagnostics = report->diagnostics};
  }
  if (which == tier::vm) {
    if (!report->run.has_value() || !report->run->succeeded) {
      return {.is_error = true,
              .diagnostics = report->run.has_value()
                                 ? report->run->message
                                 : std::string{"no run outcome recorded"}};
    }
    return {.result = report->run->exit_code & 0xff};
  }
  if (!report->build.has_value() || !report->build->succeeded) {
    return {.is_error = true,
            .diagnostics = report->build.has_value()
                               ? report->build->message
                               : std::string{"no build outcome recorded"}};
  }
  return run_executable(exe, scratch);
}

auto matches(const recorded_behavior &recorded,
             const observed_behavior &observed) -> bool {
  if (recorded.is_crash) {
    return observed.crash_signal.has_value() &&
           (!recorded.crash_signal.has_value() ||
            *recorded.crash_signal == *observed.crash_signal);
  }
  if (observed.crash_signal.has_value()) {
    return false;
  }
  if (recorded.is_error) {
    return observed.is_error &&
           observed.diagnostics.find(recorded.error_substring) !=
               std::string::npos;
  }
  if (observed.is_error) {
    return false;
  }
  if (recorded.result.has_value()) {
    return observed.result.has_value() &&
           *observed.result == (*recorded.result & 0xff);
  }
  return true;
}

// The result file: the first line is `error`, `result <n>`, `crash <signal>`, or `none`; the
// rest is the diagnostics (for `error`) or the detail (for `none`).
void write_observation(const fs::path &file, const observed_behavior &seen) {
  auto out = std::ofstream(file);
  if (seen.crash_signal.has_value()) {
    out << "crash " << *seen.crash_signal << '\n';
  } else if (seen.is_error) {
    out << "error\n" << seen.diagnostics;
  } else if (seen.result.has_value()) {
    out << "result " << *seen.result << '\n';
  } else {
    out << "none\n" << seen.detail;
  }
}

auto read_observation(const fs::path &file) -> std::optional<observed_behavior> {
  const auto text = read_file(file);
  const auto eol = text.find('\n');
  const auto head = text.substr(0, eol);
  const auto rest =
      eol == std::string::npos ? std::string{} : text.substr(eol + 1);
  if (head == "error") {
    return observed_behavior{.is_error = true, .diagnostics = rest};
  }
  if (head.starts_with("crash ")) {
    return observed_behavior{.crash_signal = std::stoi(head.substr(6))};
  }
  if (head.starts_with("result ")) {
    return observed_behavior{.result = std::stoi(head.substr(7))};
  }
  if (head == "none") {
    return observed_behavior{.detail = rest};
  }
  return std::nullopt;
}

/// Observes one (file, tier) pair in a fresh child process.
auto observe_in_child(const std::string &self, const fs::path &source,
                      tier which) -> observed_behavior {
  const auto dir = make_temp_dir();
  const auto result_file = dir.path / "observation";
  const auto status =
      run_process({self, "--child", source.string(),
                   std::string(tier_name(which)), result_file.string(),
                   dir.path.string()},
                  dir.path / "child.out");
  if (!status.spawned) {
    fail(std::format("could not start a child process for {}", source.string()));
  }
  if (status.signal.has_value()) {
    return {.crash_signal = status.signal};
  }
  if (status.exit_code == 0) {
    if (auto seen = read_observation(result_file); seen.has_value()) {
      return *seen;
    }
  }
  return {.is_error = true,
          .diagnostics = std::format("the child process exited with status {} "
                                     "without an observation: {}",
                                     status.exit_code.value_or(-1),
                                     read_file(dir.path / "child.out"))};
}

auto run_child(const std::vector<std::string> &args) -> int {
  expect(args.size() == 4, "usage: --child <file> <tier> <result> <scratch>");
  const auto which = args[1] == "vm" ? tier::vm : tier::build;
  const auto seen = observe(args[0], which, args[3]);
  write_observation(args[2], seen);
  return 0;
}

} // namespace

auto main(int argc, char *argv[]) -> int {
  try {
    if (argc > 1 && std::string_view(argv[1]) == "--child") {
      return run_child(std::vector<std::string>(argv + 2, argv + argc));
    }
    const auto self = std::string(argv[0]);
    const auto corpus_dir = find_corpus_dir(
        argc > 0 ? std::string_view(*argv) : std::string_view{});
    auto files = std::vector<fs::path>{};
    for (const auto &entry : fs::directory_iterator(corpus_dir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".cn") {
        files.push_back(entry.path());
      }
    }
    std::ranges::sort(files);
    expect(!files.empty(), "the known_gaps corpus is empty");

    struct job {
      known_gap gap;
      fs::path path;
      tier which;
      observed_behavior observed;
    };
    auto jobs = std::vector<job>{};
    for (const auto &path : files) {
      auto gap = parse_gap(path);
      for (const auto which : gap.tiers) {
        jobs.push_back({.gap = gap, .path = path, .which = which, .observed = {}});
      }
    }

    // Parallel children; each thread only spawns and waits.
    auto next = std::atomic<size_t>{0};
    const auto workers = std::clamp<size_t>(
        std::thread::hardware_concurrency() / 2, 2, 8);
    auto threads = std::vector<std::thread>{};
    for (size_t i = 0; i < workers; ++i) {
      threads.emplace_back([&] {
        for (auto index = next++; index < jobs.size(); index = next++) {
          jobs[index].observed =
              observe_in_child(self, jobs[index].path, jobs[index].which);
        }
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }

    for (const auto &j : jobs) {
      if (!matches(j.gap.recorded, j.observed)) {
        fail(std::format(
            "known gap {} ({}, tier {}) no longer behaves as recorded: "
            "recorded {}, observed {}. If todo item {} is fixed, move this "
            "program to the regular corpus with the behaviour in `# want:` "
            "(\"{}\") and delete it here.",
            j.gap.todo, j.gap.file, tier_name(j.which),
            describe(j.gap.recorded), describe(j.observed), j.gap.todo,
            j.gap.want));
      }
    }
    std::cout << "known_gaps_test: " << files.size()
              << " known gap(s) still behave as recorded\n";
  } catch (const std::exception &ex) {
    std::cerr << "known_gaps_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
