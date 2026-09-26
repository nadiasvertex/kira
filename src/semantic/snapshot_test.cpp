// Golden snapshots of every elaboration decision `check_program` makes.
//
// The safety net for `spec/inference-rewrite.md` phase 0. What it protects
// against is specific: a change to inference that keeps every existing test
// green while silently resolving one call, index, operator or literal
// conversion to a different target than before. Nothing else in the suite
// can see that — the other 28 targets assert diagnostics, computed values,
// or cross-tier agreement, none of which inspect *which* function a call
// resolved to out of several that would all run.
//
// Each corpus file below is checked in a full session (the whole injected
// stdlib plus that file), and the entire `checked_types` surface is rendered
// and compared byte for byte against a checked-in golden. Regenerate with:
//
//     CINDER_UPDATE_SNAPSHOTS=1 bazelisk test //src/semantic:snapshot_test
//
// and *read the diff* — a regeneration that was not read is the one way this
// harness can be worse than nothing.

#include <array>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "src/parser/diagnostic.h"
#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/snapshot.h"
#include "src/testing/stdlib_fixtures.h"
#include "src/testing/test_assert.h"
#include "src/testing/test_data.h"

namespace {

using cinder::testing::expect;
using cinder::testing::fail;

namespace fs = std::filesystem;

/// The fixtures to snapshot, and where each comes from.
///
/// All of them are checked in **one** session, together with one copy of the
/// injected stdlib, and rendered into one golden. That is not a convenience:
/// a session's snapshot is dominated by the stdlib's own ~24,000 decisions,
/// so a golden per fixture would check in that same stdlib once per fixture.
/// Sharing the session makes the marginal cost of adding a fixture its own
/// lines and nothing else.
///
/// The fixtures are hand-picked to span what the inference rewrite touches
/// rather than to cover the corpus: const-generic and type-generic
/// monomorphization, higher-kinded dispatch (the pattern fragment),
/// value-parameter and refinement solving (the dependent fragment), iterator
/// and comprehension desugaring, indexing through the `index`/`index_set`/
/// `index_mut`/`index_ref` traits, UFCS resolution, lambda inference, and
/// default-argument mapping.
struct snapshot_input {
  std::string_view corpus;   ///< Test-data directory name.
  std::string_view filename; ///< File within it.
};

constexpr auto k_inputs = std::array{
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "033_const_generic_monomorphization.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "034_generic_monomorphization.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "034_higher_kinded_trait_dispatch.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "037_user_iterator_for_loop.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "043_impl_on_generic_target.cn"},
    snapshot_input{.corpus = "codegen_stress", .filename = "045_ufcs.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "049_generic_impl_iterator_adapter.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "050_bound_driven_inference.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "052_element_assignment.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "054_static_dispatch_on_generic_types.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "057_lambda_solves_under_constructor.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "074_range_index_general_stride.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "078_impl_const_generic_value_param.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "082_default_parameter_values.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "087_list_cell_and_mutable_cell.cn"},
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "089_elaboration_snapshot_gaps.cn"},
    // An unannotated `[]` accumulator inside a generic body, instantiated at
    // two element types — the one shape where the template's elaboration and
    // its instances' must be told apart, and where telling them apart wrong
    // names a function nothing compiles.
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "091_unannotated_accumulator_in_generic.cn"},
    // Indexing an unannotated `[]`: the one dispatch site that read its
    // receiver unsettled, so the `index` impl was chosen against a variable.
    snapshot_input{.corpus = "codegen_stress",
                   .filename = "093_index_settles_its_receiver.cn"},
    snapshot_input{.corpus = "semantic_stress",
                   .filename = "003_collections_lambdas.cn"},
    snapshot_input{.corpus = "semantic_stress",
                   .filename = "027_dependent_and_refinement_types.cn"},
};

/// The single golden every fixture above shares.
constexpr auto k_golden_name = std::string_view("session");

auto update_requested() -> bool {
  const auto *flag = std::getenv("CINDER_UPDATE_SNAPSHOTS");
  return flag != nullptr && *flag != '\0' && std::string_view(flag) != "0";
}

/// Checks every fixture in one stdlib session and renders the result.
auto render_session() -> std::string {
  auto sources = cinder::source_manager{};
  auto diag = cinder::diagnostic_bag{};
  auto file_has_errors = std::vector<bool>{};
  auto ast_files = std::vector<cinder::ast::ptr<cinder::ast::file>>{};
  auto parsed_modules = std::vector<cinder::semantic::parsed_module>{};

  auto texts = std::vector<std::pair<std::string, std::string>>{};
  for (auto &source : cinder::testing::load_stdlib_sources()) {
    texts.emplace_back(std::move(source.path), std::move(source.text));
  }
  for (const auto &input : k_inputs) {
    const auto corpus_dir = cinder::testing::find_test_data_dir(input.corpus);
    texts.emplace_back(std::string(input.filename),
                       cinder::testing::load_test_data_file(corpus_dir.string(),
                                                          input.filename));
  }

  for (const auto &[path, text] : texts) {
    const auto file_id = sources.add_file(path, text);
    expect(file_id.has_value(), "expected snapshot source to register");
    if (file_has_errors.size() <= static_cast<size_t>(*file_id)) {
      file_has_errors.resize(static_cast<size_t>(*file_id) + 1, false);
    }
    const auto *file = sources.get(*file_id);
    expect(file != nullptr, "expected registered snapshot source");

    auto lex = cinder::lexer(file->source(), file->id(), diag);
    auto parse = cinder::parser(lex.tokenize(), file->id(), diag);
    auto ast_file = parse.parse_file();
    parsed_modules.push_back(cinder::semantic::parsed_module{
        .file_id = *file_id, .ast_file = ast_file.get()});
    ast_files.push_back(std::move(ast_file));
  }

  if (diag.error_count() != 0) {
    std::cerr << cinder::diagnostic_renderer(sources, false).render_all(diag);
    fail("expected the snapshot session to parse cleanly");
  }

  const auto checked =
      cinder::semantic::validate_semantics(parsed_modules, diag, file_has_errors);
  if (diag.error_count() != 0) {
    std::cerr << cinder::diagnostic_renderer(sources, false).render_all(diag);
    fail("expected the snapshot session to check cleanly");
  }

  return cinder::semantic::render_snapshot(checked, sources);
}

/// Where the goldens live in the source tree. `TEST_SRCDIR` points at a
/// read-only runfiles tree, so regeneration needs `BUILD_WORKSPACE_DIRECTORY`
/// (set by `bazel run`) or the working directory.
auto golden_write_path(std::string_view filename) -> fs::path {
  auto root = fs::path{};
  if (const auto *workspace = std::getenv("BUILD_WORKSPACE_DIRECTORY");
      workspace != nullptr && *workspace != '\0') {
    root = fs::path(workspace);
  }
  return root / "src/testdata/inference_snapshot" /
         (std::string(filename) + ".snapshot");
}

auto golden_read_path(std::string_view filename) -> fs::path {
  const auto dir = cinder::testing::find_test_data_dir("inference_snapshot");
  return dir / (std::string(filename) + ".snapshot");
}

/// The first line that differs, with a little context — a byte-count-only
/// failure on a multi-megabyte file is unactionable.
auto first_difference(std::string_view expected, std::string_view found)
    -> std::string {
  auto expected_stream = std::istringstream(std::string(expected));
  auto found_stream = std::istringstream(std::string(found));
  auto expected_line = std::string{};
  auto found_line = std::string{};
  auto line_number = 1;
  while (true) {
    const auto has_expected =
        static_cast<bool>(std::getline(expected_stream, expected_line));
    const auto has_found =
        static_cast<bool>(std::getline(found_stream, found_line));
    if (!has_expected && !has_found) {
      return "(no line differs; the files differ only in trailing bytes)";
    }
    if (!has_expected) {
      return std::format("line {}: golden ends, snapshot has `{}`", line_number,
                         found_line);
    }
    if (!has_found) {
      return std::format("line {}: snapshot ends, golden has `{}`", line_number,
                         expected_line);
    }
    if (expected_line != found_line) {
      return std::format("line {}:\n  golden:   {}\n  snapshot: {}",
                         line_number, expected_line, found_line);
    }
    ++line_number;
  }
}

auto test_snapshot_matches() -> void {
  const auto rendered = render_session();

  if (update_requested()) {
    const auto path = golden_write_path(k_golden_name);
    auto ec = std::error_code{};
    fs::create_directories(path.parent_path(), ec);
    auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
    expect(out.is_open(), "expected to open the golden file for regeneration");
    out << rendered;
    out.close();
    std::cerr << "wrote " << path.string() << "\n";
    return;
  }

  const auto path = golden_read_path(k_golden_name);
  auto in = std::ifstream(path, std::ios::binary);
  expect(in.is_open(), "expected a checked-in golden snapshot; regenerate with "
                       "CINDER_UPDATE_SNAPSHOTS=1");
  const auto golden = std::string(std::istreambuf_iterator<char>(in),
                                  std::istreambuf_iterator<char>());

  if (golden != rendered) {
    std::cerr << "elaboration snapshot drift\n"
              << first_difference(golden, rendered) << "\n"
              << "golden " << golden.size() << " bytes, snapshot "
              << rendered.size() << " bytes\n";
    fail("an elaboration decision changed; read the diff, then regenerate "
         "with CINDER_UPDATE_SNAPSHOTS=1 if the change is intended");
  }
}

} // namespace

auto main() -> int {
  test_snapshot_matches();
  std::cout << "snapshot_test passed\n";
  return 0;
}
