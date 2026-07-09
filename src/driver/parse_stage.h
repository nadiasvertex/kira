#pragma once

#include <filesystem>
#include <vector>

#include "driver.h"
#include "src/parser/ast.h"
#include "src/parser/diagnostic.h"
#include "src/parser/source_location.h"

namespace kira::driver {

/// Parsed source file plus the bookkeeping needed by later driver passes.
struct parsed_input {
  std::filesystem::path source_path;
  file_id_type file_id = 0;
  ast::ptr<ast::file> ast_file;
};

/// Reads, lexes, and parses every source in `cfg.sources` into `sources`.
///
/// Each file that fails to load (I/O error, duplicate/invalid file id) is
/// reported via `diagnostics` and otherwise skipped rather than aborting the
/// whole session — later driver phases only see the files that made it into
/// the returned vector. `file_has_errors` is grown to cover every allocated
/// file id and marked `true` for files where lexing/parsing produced at
/// least one diagnostic, so semantic analysis and metadata emission can skip
/// them.
///
/// @param cfg Command-line configuration naming the source files to parse.
/// @param sources Source manager that owns loaded file text and ids.
/// @param session_diagnostics Diagnostic bag accumulating lexer/parser
/// errors.
/// @param file_has_errors Per-file-id error flags, grown as files are added.
/// @param diagnostics Plain-text driver diagnostics buffer for I/O failures.
[[nodiscard]] auto parse_sources(const cli_config &cfg, source_manager &sources,
                                 diagnostic_bag &session_diagnostics,
                                 std::vector<bool> &file_has_errors,
                                 std::string &diagnostics)
    -> std::vector<parsed_input>;

} // namespace kira::driver
