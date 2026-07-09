#include "parse_stage.h"

#include <utility>

#include "src/parser/lexer.h"
#include "src/parser/parser.h"
#include "src/util/path.h"
#include "src/util/str.h"

using kira::lexer;
using kira::util::append_error;
using kira::util::normalize_path;
using kira::util::read_source_file;

namespace kira::driver {

auto parse_sources(const cli_config &cfg, source_manager &sources,
                   diagnostic_bag &session_diagnostics,
                   std::vector<bool> &file_has_errors, std::string &diagnostics)
    -> std::vector<parsed_input> {
  auto parsed_inputs = std::vector<parsed_input>{};

  for (const auto &source_arg : cfg.sources) {
    const auto source_path = std::filesystem::path(source_arg);
    auto source_text = read_source_file(source_path);
    if (!source_text) {
      append_error(diagnostics, source_text.error());
      continue;
    }

    auto file_id =
        sources.add_file(normalize_path(source_path), std::move(*source_text));
    if (!file_id) {
      append_error(diagnostics, file_id.error());
      continue;
    }

    if (file_has_errors.size() <= static_cast<size_t>(*file_id)) {
      file_has_errors.resize(static_cast<size_t>(*file_id) + 1, false);
    }

    const auto *file = sources.get(*file_id);
    if (file == nullptr) {
      append_error(diagnostics, "internal error: missing source file");
      continue;
    }

    auto errors_before = session_diagnostics.error_count();
    auto tokenizer = lexer(file->source(), file->id(), session_diagnostics);
    auto tokens = tokenizer.tokenize();
    auto parser_instance =
        parser(std::move(tokens), file->id(), session_diagnostics);
    auto ast_file = parser_instance.parse_file();

    if (session_diagnostics.error_count() > errors_before) {
      file_has_errors[*file_id] = true;
    }

    parsed_inputs.push_back(parsed_input{
        .source_path = source_path,
        .file_id = *file_id,
        .ast_file = std::move(ast_file),
    });
  }

  return parsed_inputs;
}

} // namespace kira::driver
