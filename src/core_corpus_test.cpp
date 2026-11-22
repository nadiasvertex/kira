#include "driver/driver.h"
#include "src/testing/test_assert.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

// Builds the typed core (`src/core/adapter.h`) for every program of
// `src/testdata/codegen_stress/`, with the stdlib prelude, and runs the core
// verifier over it (`spec/unified-checker-plan.md`, phase 2).
//
// Results are aggregated across programs and de-duplicated by (module,
// function), so a stdlib function counts once. The test prints how many
// functions were built, a histogram of the gaps that stopped the rest, and
// the verifier's violations by rule. It fails when fewer than
// `k_built_floor` functions are built, or on a violation that is not listed
// in `src/testdata/core_known_violations.txt`. Each line of that file is
// `<rule> <module> <function>  # <reason>`; `-` stands for an empty function
// name (a static). A module or function that ends in `*` matches every name
// that starts with what comes before it, so `forget*` covers each instance of
// `forget` and `*` covers every name.
//
// Each program compiles in its own child process (`--child <file> <result
// file> <scratch dir>`), as in `known_gaps_test`, so a crash in the adapter or
// the verifier is an observation instead of the end of the test.

extern char **environ; // NOLINT

namespace {

namespace fs = std::filesystem;
using cinder::testing::expect;
using cinder::testing::fail;

/// Raise this as the adapter covers more of the corpus.
constexpr auto k_built_floor = std::size_t{2301};

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
              std::format("cinder_core_corpus_{}_{}_{}", getpid(), counter++,
                          std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count());
  auto ec = std::error_code{};
  fs::create_directories(base, ec);
  expect(!ec, "expected to create a temporary directory");
  return temp_dir{.path = std::move(base)};
}

auto find_dir(std::string_view argv0, std::string_view relative) -> fs::path {
  auto candidates = std::vector<fs::path>{};
  if (const auto *srcdir = std::getenv("TEST_SRCDIR"); srcdir != nullptr) {
    if (const auto *workspace = std::getenv("TEST_WORKSPACE");
        workspace != nullptr && *workspace != '\0') {
      candidates.emplace_back(fs::path(srcdir) / workspace / relative);
    }
    candidates.emplace_back(fs::path(srcdir) / "_main" / relative);
  }
  if (!argv0.empty()) {
    candidates.emplace_back(fs::path(std::string(argv0) + ".runfiles") /
                            "_main" / relative);
  }
  candidates.emplace_back(relative);
  for (const auto &candidate : candidates) {
    auto ec = std::error_code{};
    if (fs::exists(candidate, ec)) {
      return candidate;
    }
  }
  fail(std::format("could not locate {}", relative));
}

auto read_file(const fs::path &path) -> std::string {
  auto in = std::ifstream(path);
  auto text = std::stringstream{};
  text << in.rdbuf();
  return text.str();
}

/// Tabs and newlines would break the observation file's line format.
auto flatten(std::string text) -> std::string {
  std::ranges::replace(text, '\t', ' ');
  std::ranges::replace(text, '\n', ' ');
  return text;
}

auto split_tabs(std::string_view line) -> std::vector<std::string> {
  auto parts = std::vector<std::string>{};
  while (true) {
    const auto tab = line.find('\t');
    parts.emplace_back(line.substr(0, tab));
    if (tab == std::string_view::npos) {
      return parts;
    }
    line = line.substr(tab + 1);
  }
}

// The observation file holds one record per line, fields separated by tabs:
//   built <module> <function>
//   gap <module> <function> <construct> <detail>
//   violation <rule> <module> <function> <message>
//   error <diagnostics>
auto run_child(const std::vector<std::string> &args) -> int {
  expect(args.size() == 3, "usage: --child <file> <result> <scratch>");
  auto cfg = cinder::driver::cli_config{
      .program_name = "cinder",
      .sources = {args[0]},
      .metadata_dir = (fs::path(args[2]) / "meta").string(),
      .show_help = false,
  };
  cfg.core_check = true;
  cinder::driver::inject_stdlib_prelude(cfg);
  auto out = std::ofstream(args[1]);
  const auto report = cinder::driver::compile_sources(cfg, false);
  if (!report.has_value()) {
    out << "error\t" << flatten(report.error()) << '\n';
    return 0;
  }
  if (report->error_count != 0 || !report->core.has_value()) {
    out << "error\t" << flatten(report->diagnostics) << '\n';
    return 0;
  }
  for (const auto &[module, function] : report->core->built) {
    out << "built\t" << module << '\t' << function << '\n';
  }
  for (const auto &[module, function] : report->core->templates) {
    out << "template\t" << module << '\t' << function << '\n';
  }
  for (const auto &gap : report->core->gaps) {
    out << (gap.in_template ? "template_gap\t" : "gap\t") << gap.module
        << '\t' << gap.function << '\t' << flatten(gap.construct) << '\t'
        << flatten(gap.detail) << '\n';
  }
  for (const auto &v : report->core->violations) {
    out << "violation\t" << v.rule << '\t' << v.module << '\t'
        << (v.function.empty() ? std::string{"-"} : v.function) << '\t'
        << flatten(v.message) << '\n';
  }
  return 0;
}

struct process_status {
  bool spawned = false;
  std::optional<int> exit_code;
  std::optional<int> signal;
};

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
  const auto rc =
      posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
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

struct observation {
  std::vector<std::vector<std::string>> records;
  std::string failure; ///< Non-empty when the child gave no observation.
};

auto observe_in_child(const std::string &self, const fs::path &source)
    -> observation {
  const auto dir = make_temp_dir();
  const auto result_file = dir.path / "observation";
  const auto status = run_process(
      {self, "--child", source.string(), result_file.string(),
       dir.path.string()},
      dir.path / "child.out");
  if (!status.spawned) {
    return {.failure = "could not start a child process"};
  }
  if (status.signal.has_value()) {
    return {.failure = std::format("the child crashed (signal {})",
                                   *status.signal)};
  }
  if (status.exit_code != 0) {
    return {.failure = std::format("the child exited with status {}: {}",
                                   status.exit_code.value_or(-1),
                                   read_file(dir.path / "child.out"))};
  }
  auto seen = observation{};
  auto in = std::ifstream(result_file);
  for (auto line = std::string{}; std::getline(in, line);) {
    if (!line.empty()) {
      seen.records.push_back(split_tabs(line));
    }
  }
  return seen;
}

using violation_key = std::tuple<std::string, std::string, std::string>;

/// Whether `name` matches an allow-list word: equal to it, or, when the
/// word ends in `*`, starting with the part before the `*`.
auto matches_word(std::string_view word, std::string_view name) -> bool {
  if (word.ends_with('*')) {
    return name.starts_with(word.substr(0, word.size() - 1));
  }
  return word == name;
}

auto is_known(const std::vector<violation_key> &known,
              const violation_key &key) -> bool {
  return std::ranges::any_of(known, [&](const violation_key &entry) {
    return std::get<0>(entry) == std::get<0>(key) &&
           matches_word(std::get<1>(entry), std::get<1>(key)) &&
           matches_word(std::get<2>(entry), std::get<2>(key));
  });
}

auto read_known_violations(const fs::path &file)
    -> std::vector<violation_key> {
  auto known = std::vector<violation_key>{};
  auto in = std::ifstream(file);
  auto number = 0;
  for (auto line = std::string{}; std::getline(in, line);) {
    ++number;
    const auto hash = line.find('#');
    const auto body = line.substr(0, hash);
    auto words = std::istringstream(body);
    auto rule = std::string{};
    auto module = std::string{};
    auto function = std::string{};
    if (!(words >> rule)) {
      continue;
    }
    expect(static_cast<bool>(words >> module >> function),
           std::format("core_known_violations.txt:{}: expected `<rule> "
                       "<module> <function>`",
                       number));
    expect(hash != std::string::npos &&
               line.find_first_not_of(" \t", hash + 1) != std::string::npos,
           std::format("core_known_violations.txt:{}: each entry needs a `#` "
                       "reason",
                       number));
    known.emplace_back(rule, module, function);
  }
  return known;
}

} // namespace

auto main(int argc, char *argv[]) -> int {
  try {
    if (argc > 1 && std::string_view(argv[1]) == "--child") {
      return run_child(std::vector<std::string>(argv + 2, argv + argc));
    }
    const auto self = std::string(argv[0]);
    const auto argv0 = argc > 0 ? std::string_view(*argv) : std::string_view{};
    const auto corpus_dir = find_dir(argv0, "src/testdata/codegen_stress");
    const auto known = read_known_violations(
        find_dir(argv0, "src/testdata/core_known_violations.txt"));

    auto files = std::vector<fs::path>{};
    for (const auto &entry : fs::directory_iterator(corpus_dir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".cn") {
        files.push_back(entry.path());
      }
    }
    std::ranges::sort(files);
    expect(!files.empty(), "the codegen_stress corpus is empty");

    auto seen = std::vector<observation>(files.size());
    auto next = std::atomic<size_t>{0};
    const auto workers =
        std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 2, 8);
    auto threads = std::vector<std::thread>{};
    for (size_t i = 0; i < workers; ++i) {
      threads.emplace_back([&] {
        for (auto index = next++; index < files.size(); index = next++) {
          seen[index] = observe_in_child(self, files[index]);
        }
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }

    using function_key = std::pair<std::string, std::string>;
    auto built = std::set<function_key>{};
    auto templates = std::set<function_key>{};
    auto template_gaps = std::set<function_key>{};
    // (module, function) -> construct, first seen.
    auto gaps = std::map<function_key, std::pair<std::string, std::string>>{};
    // (rule, module, function) -> first message and program.
    auto violations =
        std::map<violation_key, std::pair<std::string, std::string>>{};
    auto failures = std::vector<std::string>{};
    auto rejected = 0;
    for (size_t i = 0; i < files.size(); ++i) {
      const auto name = files[i].filename().string();
      if (!seen[i].failure.empty()) {
        failures.push_back(std::format("{}: {}", name, seen[i].failure));
        continue;
      }
      for (const auto &record : seen[i].records) {
        const auto &kind = record.front();
        if (kind == "built" && record.size() >= 3) {
          built.emplace(record[1], record[2]);
        } else if (kind == "template" && record.size() >= 3) {
          templates.emplace(record[1], record[2]);
        } else if ((kind == "gap" || kind == "template_gap") &&
                   record.size() >= 5) {
          gaps.try_emplace({record[1], record[2]}, record[3], record[4]);
          if (kind == "template_gap") {
            template_gaps.emplace(record[1], record[2]);
          }
        } else if (kind == "violation" && record.size() >= 5) {
          violations.try_emplace({record[1], record[2], record[3]},
                                 record[4], name);
        } else if (kind == "error") {
          ++rejected;
        }
      }
    }
    // A function built in one program and gapped in another counts as built.
    for (const auto &key : built) {
      gaps.erase(key);
    }

    auto histogram = std::map<std::string, std::size_t>{};
    for (const auto &[key, gap] : gaps) {
      ++histogram[gap.first];
    }
    auto ordered = std::vector<std::pair<std::size_t, std::string>>{};
    for (const auto &[construct, count] : histogram) {
      ordered.emplace_back(count, construct);
    }
    std::ranges::sort(ordered, std::greater{});

    std::cout << std::format("core_corpus_test: {} program(s), {} rejected "
                             "by the checker\n",
                             files.size(), rejected);
    auto templates_gapped = std::size_t{0};
    for (const auto &key : template_gaps) {
      templates_gapped += gaps.contains(key) ? 1 : 0;
    }
    std::cout << std::format("functions built: {} / {}\n", built.size(),
                             built.size() + gaps.size());
    std::cout << std::format(
        "  templates: {} / {}\n  instances and other functions: {} / {}\n",
        templates.size(), templates.size() + templates_gapped,
        built.size() - templates.size(),
        built.size() - templates.size() + gaps.size() - templates_gapped);
    std::cout << "gaps by construct:\n";
    for (const auto &[count, construct] : ordered) {
      std::cout << std::format("  {:5}  {}\n", count, construct);
    }
    auto by_rule = std::map<std::string, std::size_t>{};
    for (const auto &[key, v] : violations) {
      ++by_rule[std::get<0>(key)];
    }
    std::cout << "violations by rule:\n";
    for (const auto &[rule, count] : by_rule) {
      std::cout << std::format("  {:5}  {}\n", count, rule);
    }
    auto unknown = std::vector<std::string>{};
    for (const auto &[key, v] : violations) {
      const auto &[rule, module, function] = key;
      const auto line = std::format("  {} {} {}: {} ({})", rule, module,
                                    function, v.first, v.second);
      std::cout << line << '\n';
      if (!is_known(known, key)) {
        unknown.push_back(line);
      }
    }
    if (const auto *dump = std::getenv("CORE_CORPUS_GAPS"); dump != nullptr) {
      for (const auto &[key, gap] : gaps) {
        std::cout << std::format("  gap {} {}: {}: {}\n", key.first,
                                 key.second, gap.first, gap.second);
      }
    }

    for (const auto &failure : failures) {
      std::cerr << "  " << failure << '\n';
    }
    expect(failures.empty(),
           std::format("{} program(s) gave no observation (see above)",
                       failures.size()));
    if (!unknown.empty()) {
      auto text = std::string{};
      for (const auto &line : unknown) {
        text += line + '\n';
      }
      fail(std::format("{} core violation(s) not listed in "
                       "src/testdata/core_known_violations.txt:\n{}",
                       unknown.size(), text));
    }
    expect(built.size() >= k_built_floor,
           std::format("only {} function(s) built; the floor is {}",
                       built.size(), k_built_floor));
  } catch (const std::exception &ex) {
    std::cerr << "core_corpus_test failed: unhandled exception: " << ex.what()
              << '\n';
    std::exit(1);
  }
  return 0;
}
