#pragma once

#include <string_view>

namespace kira {

/// Single source of truth for the compiler's version.
///
/// Bump `k_version_minor` (reset `k_version_patch` to 0) when a feature
/// lands; bump `k_version_major` (reset the others to 0) on a breaking
/// language/CLI change. `k_version_patch` is otherwise unused today.
///
/// `MODULE.bazel`'s `module(version = ...)` field is a separate,
/// bzlmod-mandated string that can't read this header — keep it in sync by
/// hand whenever these constants change.
inline constexpr unsigned k_version_major = 0;
inline constexpr unsigned k_version_minor = 8;
inline constexpr unsigned k_version_patch = 0;

/// "MAJOR.MINOR.PATCH", kept in sync with the constants above by hand (no
/// constexpr formatting dependency needed for three small integers).
inline constexpr std::string_view k_version_string = "0.8.0";

/// ISO-8601 date this version was cut, updated by hand alongside the
/// version constants above. `__DATE__` can't serve this role: Bazel's
/// default toolchain redefines it to the literal `"redacted"` for
/// reproducible builds (confirmed empirically — this compiler binary
/// itself reports `__DATE__` as `"redacted"` under `bazelisk build`), so a
/// build-time macro can't recover a real date at all.
inline constexpr std::string_view k_release_date = "2026-07-18";

} // namespace kira
