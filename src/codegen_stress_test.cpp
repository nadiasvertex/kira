// Runs every codegen stress corpus program through the driver's pipeline on
// both tiers: each is checked (types and ownership), lowered to HIR and to the
// storage form, verified, compiled for the bytecode VM and for the LLVM JIT,
// and run, then lowered again with small calls inlined and run again.
//
// `main`'s answer is checked on both tiers against the file's directive:
// `# expect: <integer>` for a scalar result, `# expect-panic: <reason>` for a
// panic, and `# expect-text: <rendering>` for a `str`, `list[T]`, tuple, or
// fixed array, rendered from its inline bytes. A `main` returning `unit`
// needs none. Agreement between the tiers is not the check: both read the
// same layouts, so they fail together (CLAUDE.md).
//
// `main` must free every heap block it allocates: `# max-live-blocks:
// <integer>` allows a file to leave some, and `# max-allocations: <integer>`
// bounds the blocks it allocates at all, on any thread. Both are checked on
// each tier and counted from after the statics are built.
//
// `CINDER_CORPUS_SURVEY=1` prints every file's status, and
// `CINDER_CORPUS_FILTER` runs only the files whose name contains it.

#include <cstdlib>
#include <string>

#include "src/codegen_corpus.h"

auto main() -> int {
  const auto *filter = std::getenv("CINDER_CORPUS_FILTER");
  auto options = cinder::testing::codegen_corpus::corpus_options{
      .name = "codegen_stress_test", .include = {}, .hook = {}};
  if (filter != nullptr && *filter != '\0') {
    options.include = [filter](const std::string &name) -> bool {
      return name.contains(filter);
    };
  }
  cinder::testing::codegen_corpus::run_corpus(options);
  return 0;
}
