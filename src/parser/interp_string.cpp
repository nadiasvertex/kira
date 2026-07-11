#include "src/parser/interp_string.h"

namespace kira {

auto has_interpolation(std::string_view content) -> bool {
  for (size_t i = 0; i < content.size(); ++i) {
    if (content[i] == '\\') {
      ++i; // Skip the escaped character; it can't itself start a run.
      continue;
    }
    if (content[i] == '{') {
      if (i + 1 < content.size() && content[i + 1] == '{') {
        ++i; // Doubled `{{` stays literal.
        continue;
      }
      return true;
    }
  }
  return false;
}

namespace {

/// Skips a quoted literal (`"`- or `'`-delimited) starting at `content[i]`
/// (the opening quote), honoring backslash escapes, and returns the index
/// just past the closing quote — or `std::nullopt` if `content` ends first.
[[nodiscard]] auto skip_quoted(std::string_view content, size_t i)
    -> std::optional<size_t> {
  const char quote = content[i];
  ++i;
  while (i < content.size()) {
    if (content[i] == '\\') {
      i += 2;
      continue;
    }
    if (content[i] == quote) {
      return i + 1;
    }
    ++i;
  }
  return std::nullopt;
}

} // namespace

auto scan_interpolated_content(std::string_view content)
    -> std::optional<std::vector<interp_run>> {
  auto runs = std::vector<interp_run>{};
  auto literal_buf = std::string{};
  size_t i = 0;

  while (i < content.size()) {
    const char c = content[i];

    if (c == '\\') {
      literal_buf.push_back(c);
      if (i + 1 < content.size()) {
        literal_buf.push_back(content[i + 1]);
      }
      i += 2;
      continue;
    }

    if (c == '{') {
      if (i + 1 < content.size() && content[i + 1] == '{') {
        literal_buf.push_back('{');
        i += 2;
        continue;
      }

      if (!literal_buf.empty()) {
        runs.push_back(interp_run{.is_literal = true,
                                  .literal_text = std::move(literal_buf)});
        literal_buf.clear();
      }

      auto run = interp_run{.is_literal = false};
      run.expr_start = i + 1;

      size_t depth = 0;
      size_t j = run.expr_start;
      bool self_doc_seen = false;
      bool spec_seen = false;
      bool closed = false;

      while (j < content.size()) {
        const char ch = content[j];

        if (ch == '\\') {
          j += 2;
          continue;
        }
        if (ch == '"' || ch == '\'') {
          const auto after = skip_quoted(content, j);
          if (!after.has_value()) {
            return std::nullopt;
          }
          j = *after;
          continue;
        }
        if (ch == '{' || ch == '(' || ch == '[') {
          ++depth;
          ++j;
          continue;
        }
        if (ch == ')' || ch == ']') {
          if (depth > 0) {
            --depth;
          }
          ++j;
          continue;
        }
        if (ch == '}') {
          if (depth > 0) {
            --depth;
            ++j;
            continue;
          }
          if (spec_seen) {
            run.spec_end = j;
          } else if (!self_doc_seen) {
            run.expr_end = j;
          }
          i = j + 1;
          closed = true;
          break;
        }
        if (depth == 0 && ch == '=' && !spec_seen) {
          const bool is_eq_eq = j + 1 < content.size() && content[j + 1] == '=';
          if (!is_eq_eq && !self_doc_seen) {
            run.expr_end = j;
            self_doc_seen = true;
            ++j;
            continue;
          }
        }
        if (depth == 0 && ch == ':' && !spec_seen) {
          if (!self_doc_seen) {
            run.expr_end = j;
          }
          spec_seen = true;
          run.spec_start = j + 1;
          ++j;
          continue;
        }
        ++j;
      }

      if (!closed) {
        return std::nullopt;
      }

      run.self_doc = self_doc_seen;
      run.has_spec = spec_seen;
      runs.push_back(std::move(run));
      continue;
    }

    literal_buf.push_back(c);
    ++i;
  }

  if (!literal_buf.empty()) {
    runs.push_back(
        interp_run{.is_literal = true, .literal_text = std::move(literal_buf)});
  }

  return runs;
}

auto find_matching_brace(std::string_view text, size_t open_pos)
    -> std::optional<size_t> {
  size_t depth = 0;
  size_t j = open_pos + 1;
  while (j < text.size()) {
    const char ch = text[j];
    if (ch == '\\') {
      j += 2;
      continue;
    }
    if (ch == '"' || ch == '\'') {
      const auto after = skip_quoted(text, j);
      if (!after.has_value()) {
        return std::nullopt;
      }
      j = *after;
      continue;
    }
    if (ch == '{' || ch == '(' || ch == '[') {
      ++depth;
      ++j;
      continue;
    }
    if (ch == ')' || ch == ']') {
      if (depth > 0) {
        --depth;
      }
      ++j;
      continue;
    }
    if (ch == '}') {
      if (depth > 0) {
        --depth;
        ++j;
        continue;
      }
      return j;
    }
    ++j;
  }
  return std::nullopt;
}

} // namespace kira
