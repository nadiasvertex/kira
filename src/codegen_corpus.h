#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "src/hir/nodes.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/checker_results.h"

/// The codegen stress corpus runner (`codegen_stress_test`): each program is
/// checked, lowered to HIR and to the storage form, verified, and run on the
/// bytecode VM and the LLVM JIT, as lowered and again with small calls
/// inlined. `main`'s answer is checked against the file's `# expect:`,
/// `# expect-panic:` or `# expect-text:`, and its heap use against
/// `# max-live-blocks:` and `# max-allocations:`.
namespace cinder::testing::codegen_corpus {

/// What a hook sees of one program once it is lowered to HIR, before small
/// calls are inlined.
struct post_lower_context {
  hir::ptr_vec<hir::hir_module> &modules;
  semantic::checked_types &checked;
  std::span<const semantic::parsed_module> parsed;
  const std::vector<bool> &file_has_errors;
  /// The corpus file, and the HIR name of its module.
  file_id_type file_id;
  std::string module_name;
  /// Whether this is the run with small calls inlined.
  bool inline_calls = false;
};

using post_lower_hook = std::function<void(post_lower_context &)>;

struct verdict {
  bool passed = false;
  std::string status;
  /// The LLVM tier, tried once the VM tier passes.
  bool llvm_passed = false;
  std::string llvm_status = "not run (the VM tier failed)";
};

/// Runs one corpus file on both tiers.
[[nodiscard]] auto survey(const std::filesystem::path &path, bool inline_calls,
                          const post_lower_hook &hook) -> verdict;

struct corpus_options {
  /// The test's name, for its summary line.
  std::string name;
  /// Which files to run, by file name; every file when empty.
  std::function<bool(const std::string &)> include;
  post_lower_hook hook;
};

/// Runs every included corpus file as lowered and inlined, on both tiers,
/// and fails the test (`cinder::testing::fail`) when any does not pass.
/// Returns the number of files run. `CINDER_CORPUS_SURVEY=1` prints every
/// file's status.
auto run_corpus(const corpus_options &options) -> std::size_t;

} // namespace cinder::testing::codegen_corpus
