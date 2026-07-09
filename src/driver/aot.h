#pragma once

#include <string>

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
