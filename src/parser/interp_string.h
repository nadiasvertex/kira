#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kira {

/// @brief One run produced by splitting a string literal's inner content
/// (the text between the quotes, escapes/braces not yet decoded) into
/// literal-text and interpolation runs, per
/// `spec/string-formatting-design.md`.
///
/// Offsets are byte offsets into the `content` view passed to
/// `scan_interpolated_content`, i.e. relative to the string's first
/// character after the opening `"` — callers add the token's own base
/// offset to get absolute file positions.
struct interp_run {
  bool is_literal = true;

  /// Valid when `is_literal`: the run's raw text, with doubled `{{`/`}}`
  /// already collapsed to a single literal brace, but backslash escapes
  /// (`\n`, `\"`, ...) still undecoded — callers should still run this
  /// through `decode_string_body`.
  std::string literal_text;

  /// Valid when `!is_literal`: half-open `[expr_start, expr_end)` range of
  /// the embedded expression's raw source text.
  size_t expr_start = 0;
  size_t expr_end = 0;

  bool self_doc = false; ///< Whether a trailing `=` was written.

  bool has_spec = false; ///< Whether a `:format_spec` was written.
  /// Valid when `has_spec`: half-open `[spec_start, spec_end)` range of the
  /// format-spec text (after the `:`, before the closing `}`).
  size_t spec_start = 0;
  size_t spec_end = 0;
};

/// @brief Splits a string literal's inner content into literal and
/// interpolation runs.
///
/// This is a pure scan with no diagnostics of its own: it tracks combined
/// nesting depth over `{`/`(`/`[` (so a named-argument `:` inside
/// `f(x: 1)`, or a nested struct literal's own `{...}` fields, are never
/// mistaken for the interpolation's own `=`/`:` separators) and skips over
/// nested `"..."`/`'...'` literals (honoring backslash escapes) while
/// looking for each interpolation's boundaries. Returns `std::nullopt` if a
/// `{` interpolation is never closed with a matching `}` before `content`
/// ends — callers should fall back to a generic "unterminated string
/// interpolation" diagnostic in that case (the lexer already emits one when
/// scanning the enclosing string, so this is a secondary/defensive check).
[[nodiscard]] auto scan_interpolated_content(std::string_view content)
    -> std::optional<std::vector<interp_run>>;

/// @brief Returns whether `content` contains at least one unescaped,
/// unsuffixed `{` — i.e. whether it needs interpolation splitting at all.
/// Lets callers cheaply keep the zero-cost `literal_expr` path for ordinary
/// strings without running the full scanner.
[[nodiscard]] auto has_interpolation(std::string_view content) -> bool;

/// @brief Finds the `}` matching the `{` at `text[open_pos]`, honoring the
/// same combined `{`/`(`/`[` nesting and quoted-literal skipping
/// `scan_interpolated_content` uses — for locating a dynamic `{expr}`
/// width/precision inside a format-spec's own text. Returns `std::nullopt`
/// if no matching `}` is found before `text` ends.
[[nodiscard]] auto find_matching_brace(std::string_view text, size_t open_pos)
    -> std::optional<size_t>;

} // namespace kira
