#pragma once

#include <expected>
#include <string>

#include "driver.h"

namespace kira::driver {

/// When `cfg.test_mode` is set, scans the user portion of `cfg.sources`
/// (everything before `cfg.stdlib_boundary`, or all of it if unset) for
/// inline submodules literally named `tests` and, unless a user source
/// already declares `main`, appends a synthesized runner source to
/// `cfg.sources` that calls `std.test.run_suites` over every discovered
/// suite. See spec/specification/04-stdlib/testing/61-std-test.md's
/// Discovery section for the exact classification rules.
///
/// Does its own throwaway parse of the current `cfg.sources` to inspect
/// their syntax; a parse failure there is left for the real compile to
/// report normally — this pass silently does nothing (returns success) on a
/// parse error, or when nothing matches, or when a user source already
/// declares `main`.
///
/// @param cfg Command-line configuration; `cfg.sources` is appended to in
/// place when a runner is synthesized.
[[nodiscard]] auto discover_and_inject_test_runner(cli_config &cfg)
    -> std::expected<void, std::string>;

} // namespace kira::driver
