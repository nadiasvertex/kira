#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "src/hir/nodes.h"
#include "src/semantic/types.h"

namespace kira::driver {

/// Outcome of building `cli_config::build_function` into a standalone
/// executable when `cli_config::build` is set. Like `run_outcome`, this is a
/// distinct, best-effort step layered on top of HIR lowering — the same
/// scalar/control-flow subset restriction applies, now via
/// `src/llvm_codegen` rather than `src/bytecode_compiler`.
struct build_outcome {
  bool succeeded = false; ///< True when a standalone executable was produced.
  std::string message;    ///< The output executable's path on success; the
                          ///< reason the build didn't happen otherwise.
};

} // namespace kira::driver

namespace kira::driver {

/// Locates one `alwayslink = True` cc_library's archive under `bazel_package`
/// (e.g. `src/llvm_codegen`) named `library_name` (e.g. `aot_runtime`), so
/// `--compile` can hand it to the system linker.
[[nodiscard]] auto find_bazel_archive(std::string_view program_name,
                                      std::string_view bazel_package,
                                      std::string_view library_name)
    -> std::optional<std::filesystem::path>;

/// Compiles `hir_module` to a native object file via `src/llvm_codegen`,
/// then links it against Kira's AOT runtime support library into a
/// standalone executable at `output_path`.
[[nodiscard]] auto build_hir_module(const hir::hir_module &hir_module,
                                    const semantic::type_table &types,
                                    std::string_view function_name,
                                    const std::filesystem::path &output_path,
                                    std::string_view program_name)
    -> build_outcome;

} // namespace kira::driver
