#pragma once

#include <cstdint>
#include <string_view>

namespace kira::driver {

/// Default entry-point function name executed by default (or via
/// `--compile`).
inline constexpr std::string_view k_default_run_function = "main";

/// Optimization level requested via `-O0`/`-O1`/`-O2`/`-O3`. Kept as its own
/// small, LLVM-header-free enum (rather than reusing
/// `llvm_codegen::optimization_level` directly) so `cli_config`/`parse_args`
/// (`driver.h`/`cli.cpp`) don't have to pull in LLVM's headers just to store
/// which level the user asked for — `build_hir_module` (`aot.cpp`, which
/// already includes `src/llvm_codegen/codegen.h`) is the one place that
/// converts this to `llvm_codegen::optimization_level`, and the two enums'
/// values are kept numerically identical (0-3) specifically to make that
/// conversion a plain `static_cast`.
enum class optimization_level : uint8_t { o0 = 0, o1 = 1, o2 = 2, o3 = 3 };

/// `--run` (the tier-0 bytecode VM) never touches LLVM at all, so `-O` only
/// affects `--compile`/`-c` output — `o0` is the default there too, matching
/// every existing `kira build` invocation's behavior before `-O` existed.
inline constexpr optimization_level k_default_optimization_level =
    optimization_level::o0;

} // namespace kira::driver
