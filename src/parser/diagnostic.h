#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "source_location.h"

namespace kira {

// ==========================================================================
//  diagnostic severity levels.
//
//  We deliberately include "Help" and "Note" as first-class levels because
//  Kira's philosophy is that the compiler is a teacher. Errors should always
//  be accompanied by context that helps the user understand *why* something
//  is wrong and *how* to fix it.
// ==========================================================================
enum class diagnostic_level : uint8_t {
  /// A fatal error that prevents further analysis of the current construct.
  /// The parser will attempt error recovery, but the AST node will be
  /// marked as containing errors.
  error,

  /// A problem that doesn't prevent parsing from continuing but indicates
  /// something that will fail in a later compilation phase. For example,
  /// using a modifier in the wrong position.
  warning,

  /// Additional context attached to a preceding Error or Warning.
  /// Notes point at related locations ("this `{` was opened here",
  /// "previous definition was here", etc.).
  note,

  /// A concrete suggestion for how to fix the problem. This is the
  /// cooperative part — we don't just complain, we show the user what
  /// to do. Help messages may include suggested replacement text.
  help,
};

/// @brief Returns the stable textual label shown to users for `level`.
///
/// Renderer code and tests should use this helper instead of spelling the names
/// inline so severity wording stays consistent across compiler phases.
[[nodiscard]] constexpr auto
diagnostic_level_name(diagnostic_level level) noexcept -> std::string_view {
  switch (level) {
  case diagnostic_level::error:
    return "error";
  case diagnostic_level::warning:
    return "warning";
  case diagnostic_level::note:
    return "note";
  case diagnostic_level::help:
    return "help";
  }
  return "unknown";
}

/// @brief ANSI escape sequences used by the terminal diagnostic renderer.
///
/// They live in a dedicated namespace so formatting code can opt into color
/// without mixing escape literals into the higher-level rendering logic.
namespace ansi {
constexpr std::string_view reset = "\033[0m"; ///< Clear all active formatting.
constexpr std::string_view bold = "\033[1m";  ///< Emphasize severity headings.
constexpr std::string_view dim =
    "\033[2m"; ///< Reserved for lower-emphasis text.
constexpr std::string_view red = "\033[31m";     ///< Base error color.
constexpr std::string_view yellow = "\033[33m";  ///< Base warning color.
constexpr std::string_view blue = "\033[34m";    ///< Base note color.
constexpr std::string_view magenta = "\033[35m"; ///< Reserved accent color.
constexpr std::string_view cyan = "\033[36m";    ///< Reserved accent color.
constexpr std::string_view bold_red =
    "\033[1;31m"; ///< Error headline and underline color.
constexpr std::string_view bold_yellow =
    "\033[1;33m"; ///< Warning headline and underline color.
constexpr std::string_view bold_blue =
    "\033[1;34m"; ///< Note headline and gutter color.
constexpr std::string_view bold_magenta =
    "\033[1;35m"; ///< Reserved bold accent color.
constexpr std::string_view bold_cyan =
    "\033[1;36m"; ///< Reserved bold accent color.
constexpr std::string_view bold_green =
    "\033[1;32m"; ///< Help and fix-preview color.
} // namespace ansi

/// @brief Returns the preferred display color for a diagnostic severity.
///
/// Keeping the mapping here lets later renderers reuse the same severity-to-
/// color policy, or deliberately replace it in one place.
[[nodiscard]] constexpr auto
diagnostic_level_color(diagnostic_level level) noexcept -> std::string_view {
  switch (level) {
  case diagnostic_level::error:
    return ansi::bold_red;
  case diagnostic_level::warning:
    return ansi::bold_yellow;
  case diagnostic_level::note:
    return ansi::bold_blue;
  case diagnostic_level::help:
    return ansi::bold_green;
  }
  return ansi::reset;
}

// ==========================================================================
//  A label is a short annotation attached to a span of source code.
//  Diagnostics may carry multiple labels to highlight different parts
//  of the source that are relevant to the problem.
// ==========================================================================
struct diagnostic_label {
  source_span span;       ///< Source bytes to underline or point at.
  std::string message;    ///< Short explanation shown next to the underline.
  diagnostic_level level; ///< Controls how strongly the span is emphasized.

  /// @brief Creates a source annotation attached to a diagnostic.
  ///
  /// Labels separate the primary narrative (`diagnostic::message`) from the
  /// precise spans that justify it, allowing later renderers and tooling to map
  /// problems back onto source code.
  ///
  /// @param s Span being highlighted.
  /// @param msg User-facing explanation of why that span matters.
  /// @param lvl Severity used for styling this label.
  diagnostic_label(source_span s, std::string msg,
                   diagnostic_level lvl = diagnostic_level::error)
      : span(s), message(std::move(msg)), level(lvl) {}
};

// ==========================================================================
//  A suggested edit — a concrete replacement the user can apply.
//  When we know how to fix the problem, we show it.
// ==========================================================================
struct suggested_fix {
  std::string description; ///< Human description of the intended edit.
  source_span span;        ///< Source range to replace; empty means insertion.
  std::string replacement; ///< Replacement text tools or users can apply.
};

// ==========================================================================
//  diagnostic — a single error, warning, note, or help message.
//
//  Diagnostics are designed to be *helpful*. Every error should ideally:
//    1. Say what went wrong in plain language (the message)
//    2. Point at exactly where it went wrong (labels)
//    3. Explain why it's wrong (notes)
//    4. Suggest how to fix it (help + suggested fixes)
//
//  The compiler is a teacher, not an adversary. We never just say
//  "syntax error" — we say what we expected, what we found, and what
//  the user probably meant to write.
// ==========================================================================
struct diagnostic {
  diagnostic_level level;   ///< Primary severity controlling user attention.
  std::string message;      ///< Main explanation of what went wrong or matters.
  file_id_type file_id = 0; ///< File that owns the primary diagnostic context.

  /// Primary and secondary labels pointing at source code.
  std::vector<diagnostic_label> labels;

  /// Child diagnostics — notes and help messages attached to this one.
  std::vector<diagnostic> children;

  /// Suggested fixes that tools/editors can auto-apply.
  std::vector<suggested_fix> fixes;

  // -- Builder-pattern methods for ergonomic construction --

  /// @brief Creates a diagnostic rooted in a specific file.
  ///
  /// Diagnostics stay as plain value objects so every compiler phase can build,
  /// enrich, and forward them before a renderer decides how to display them.
  ///
  /// @param lvl Severity of the diagnostic.
  /// @param msg Primary user-facing message.
  /// @param fid File owning the main context for the message.
  diagnostic(diagnostic_level lvl, std::string msg, file_id_type fid = 0)
      : level(lvl), message(std::move(msg)), file_id(fid) {}

  /// @brief Attaches a primary source label using the diagnostic's severity.
  ///
  /// Use this for the main blame site later phases should surface first.
  ///
  /// @param span Source range to highlight.
  /// @param msg Inline explanation shown beside the highlight.
  auto with_label(source_span span, std::string msg) & -> diagnostic & {
    labels.emplace_back(span, std::move(msg), level);
    return *this;
  }

  /// @brief Rvalue overload of `with_label` for fluent construction.
  auto with_label(source_span span, std::string msg) && -> diagnostic && {
    labels.emplace_back(span, std::move(msg), level);
    return std::move(*this);
  }

  /// @brief Attaches a secondary label for related supporting context.
  ///
  /// Secondary labels intentionally use note styling so users can distinguish
  /// "this is the problem" from "this code is involved".
  ///
  /// @param span Related source range.
  /// @param msg Explanation of that range's relationship to the issue.
  auto with_secondary_label(source_span span,
                            std::string msg) & -> diagnostic & {
    labels.emplace_back(span, std::move(msg), diagnostic_level::note);
    return *this;
  }

  /// @brief Rvalue overload of `with_secondary_label`.
  auto with_secondary_label(source_span span,
                            std::string msg) && -> diagnostic && {
    labels.emplace_back(span, std::move(msg), diagnostic_level::note);
    return std::move(*this);
  }

  /// @brief Adds a note child diagnostic with explanatory context.
  ///
  /// Notes are where later phases can explain invariant violations, prior
  /// declarations, or language rules without overloading the primary message.
  ///
  /// @param msg Additional explanatory text.
  auto with_note(std::string msg) & -> diagnostic & {
    children.emplace_back(diagnostic_level::note, std::move(msg), file_id);
    return *this;
  }

  /// @brief Rvalue overload of `with_note`.
  auto with_note(std::string msg) && -> diagnostic && {
    children.emplace_back(diagnostic_level::note, std::move(msg), file_id);
    return std::move(*this);
  }

  /// @brief Adds a help child diagnostic describing how to fix the issue.
  ///
  /// Prefer help text when the compiler can steer the user toward a likely
  /// repair without committing to an exact edit.
  ///
  /// @param msg Suggested next step for the user.
  auto with_help(std::string msg) & -> diagnostic & {
    children.emplace_back(diagnostic_level::help, std::move(msg), file_id);
    return *this;
  }

  /// @brief Rvalue overload of `with_help`.
  auto with_help(std::string msg) && -> diagnostic && {
    children.emplace_back(diagnostic_level::help, std::move(msg), file_id);
    return std::move(*this);
  }

  /// @brief Records a structured fix that tools may be able to apply.
  ///
  /// Unlike help text, fixes model a concrete edit and can be consumed by IDEs
  /// or command-line tooling.
  ///
  /// @param desc Description of the edit.
  /// @param span Replacement range.
  /// @param replacement Replacement text.
  auto with_fix(std::string desc, source_span span,
                std::string replacement) & -> diagnostic & {
    fixes.push_back(suggested_fix{.description = std::move(desc),
                                  .span = span,
                                  .replacement = std::move(replacement)});
    return *this;
  }

  /// @brief Rvalue overload of `with_fix`.
  auto with_fix(std::string desc, source_span span,
                std::string replacement) && -> diagnostic && {
    fixes.push_back(suggested_fix{.description = std::move(desc),
                                  .span = span,
                                  .replacement = std::move(replacement)});
    return std::move(*this);
  }

  /// @brief Returns whether this diagnostic counts against the error budget.
  [[nodiscard]] auto is_error() const noexcept -> bool {
    return level == diagnostic_level::error;
  }

  /// @brief Returns whether this diagnostic contributes to warning totals.
  [[nodiscard]] auto is_warning() const noexcept -> bool {
    return level == diagnostic_level::warning;
  }
};

// ==========================================================================
//  diagnostic_bag — collects diagnostics during a compilation phase.
//
//  This is the primary interface that the parser (and later phases) use
//  to report problems. It tracks error/warning counts and provides a
//  limit to prevent cascading error floods that confuse rather than help.
// ==========================================================================
class diagnostic_bag {
public:
  static constexpr uint32_t k_default_max_errors =
      50; ///< Default cascade cut-off.

  /// @brief Creates a diagnostic collector with an optional error budget.
  ///
  /// Most compiler phases share one bag so users see a single ordered stream of
  /// problems instead of phase-local fragments.
  ///
  /// @param max_errors Maximum number of primary errors before throttling.
  explicit diagnostic_bag(uint32_t max_errors = k_default_max_errors)
      : max_errors_(max_errors) {}

  // -- Reporting --

  /// @brief Adds a diagnostic and updates severity counters.
  ///
  /// The bag enforces a max-error budget so the compiler stays useful after an
  /// early syntax disaster instead of overwhelming the user with follow-on
  /// noise.
  ///
  /// @param diag Diagnostic to retain.
  void emit(const diagnostic& diag) {
    if (diag.is_error()) {
      ++error_count_;
      if (error_count_ > max_errors_ && !cascade_reported_) {
        cascade_reported_ = true;
        diagnostics_.emplace_back(
            diagnostic_level::error,
            std::format("too many errors ({}); stopping here to avoid "
                        "overwhelming you — fix the issues above first "
                        "and the rest will likely resolve",
                        error_count_),
            diag.file_id);
        return;
      }
      if (error_count_ > max_errors_) {
        return; // Silently drop — we already told the user.
      }
    }
    if (diag.is_warning()) {
      ++warning_count_;
    }
    diagnostics_.push_back(std::move(diag));
  }

  /// @brief Convenience helper for emitting a single-span error.
  ///
  /// This is aimed at parser and lexer call sites that have one obvious blame
  /// span and do not need to build a richer `diagnostic` manually.
  ///
  /// @param file_id File containing the problem.
  /// @param message Primary error message.
  /// @param span Source range to label.
  /// @param label_msg Inline label text; defaults to `here`.
  void error(file_id_type file_id, std::string message, source_span span,
             std::string label_msg = "") {
    auto diag =
        diagnostic(diagnostic_level::error, std::move(message), file_id);
    if (!label_msg.empty()) {
      diag.with_label(span, std::move(label_msg));
    } else {
      diag.with_label(span, "here");
    }
    emit(std::move(diag));
  }

  /// @brief Convenience helper for emitting a single-span warning.
  ///
  /// @param file_id File containing the warning.
  /// @param message Primary warning message.
  /// @param span Source range to label.
  /// @param label_msg Inline label text; defaults to `here`.
  void warning(file_id_type file_id, std::string message, source_span span,
               std::string label_msg = "") {
    auto diag =
        diagnostic(diagnostic_level::warning, std::move(message), file_id);
    if (!label_msg.empty()) {
      diag.with_label(span, std::move(label_msg));
    } else {
      diag.with_label(span, "here");
    }
    emit(std::move(diag));
  }

  // -- Queries --

  /// @brief Returns whether any error-level diagnostics have been observed.
  [[nodiscard]] auto has_errors() const noexcept -> bool {
    return error_count_ > 0;
  }

  /// @brief Returns the number of retained or budgeted errors.
  [[nodiscard]] auto error_count() const noexcept -> uint32_t {
    return error_count_;
  }

  /// @brief Returns the number of warning diagnostics seen so far.
  [[nodiscard]] auto warning_count() const noexcept -> uint32_t {
    return warning_count_;
  }

  /// @brief Returns whether the bag has moved past its configured error budget.
  [[nodiscard]] auto at_error_limit() const noexcept -> bool {
    return error_count_ > max_errors_;
  }

  /// @brief Returns the stored diagnostics in emission order.
  [[nodiscard]] auto diagnostics() const noexcept
      -> const std::vector<diagnostic> & {
    return diagnostics_;
  }

  /// @brief Returns mutable access for infrastructure that needs to reorder or
  /// inspect.
  [[nodiscard]] auto diagnostics() noexcept -> std::vector<diagnostic> & {
    return diagnostics_;
  }

  /// @brief Moves all stored diagnostics out and resets the bag state.
  ///
  /// This is useful when a pipeline stage wants to hand off accumulated issues
  /// without preserving previous counters.
  [[nodiscard]] auto take() -> std::vector<diagnostic> {
    auto result = std::move(diagnostics_);
    diagnostics_.clear();
    error_count_ = 0;
    warning_count_ = 0;
    cascade_reported_ = false;
    return result;
  }

  /// @brief Clears diagnostics and resets all counters.
  void clear() {
    diagnostics_.clear();
    error_count_ = 0;
    warning_count_ = 0;
    cascade_reported_ = false;
  }

private:
  std::vector<diagnostic>
      diagnostics_;               ///< Emitted diagnostics in user-facing order.
  uint32_t error_count_ = 0;      ///< Total errors observed, even past the cap.
  uint32_t warning_count_ = 0;    ///< Total warnings observed.
  uint32_t max_errors_;           ///< Error budget before throttling begins.
  bool cascade_reported_ = false; ///< Whether the max-error notice was emitted.
};

// ==========================================================================
//  diagnostic_renderer — formats diagnostics as pretty-printed terminal
//  output with source context, underlines, and ANSI colors.
//
//  The output style is inspired by Rust's error messages: clear, helpful,
//  and visually structured so the user's eye is drawn to the important
//  parts. The philosophy: if the user has to read an error message more
//  than once to understand it, the error message has failed.
// ==========================================================================
class diagnostic_renderer {
public:
  /// @brief Creates a renderer bound to the source files used by diagnostics.
  ///
  /// Rendering is intentionally separate from collection so frontends can swap
  /// in non-terminal presentations while reusing the same `diagnostic` model.
  ///
  /// @param sources Source table used to resolve spans.
  /// @param use_color Whether ANSI styling should be emitted.
  explicit diagnostic_renderer(const source_manager &sources,
                               bool use_color = true)
      : sources_(sources), use_color_(use_color) {}

  /// @brief Renders one diagnostic and all of its attachments to a string.
  ///
  /// The output is terminal-oriented text; callers producing structured editor
  /// diagnostics should consume the underlying `diagnostic` object directly.
  ///
  /// @param diag Diagnostic to render.
  [[nodiscard]] auto render(const diagnostic &diag) const -> std::string {
    std::string out;
    render_diagnostic(out, diag, /*indent=*/0);
    return out;
  }

  /// @brief Renders every diagnostic in `bag` into one terminal-formatted blob.
  ///
  /// @param bag Collection of diagnostics to display in order.
  [[nodiscard]] auto render_all(const diagnostic_bag &bag) const
      -> std::string {
    std::string out;
    for (const auto &diag : bag.diagnostics()) {
      render_diagnostic(out, diag, /*indent=*/0);
      out += '\n';
    }
    return out;
  }

private:
  /// Renders a diagnostic recursively, indenting notes and help children.
  void render_diagnostic(std::string &out, const diagnostic &diag,
                         int indent) const {
    // Header line: "error: message"
    std::string prefix(static_cast<size_t>(indent * 2), ' ');

    if (use_color_) {
      out += prefix;
      out += diagnostic_level_color(diag.level);
      out += diagnostic_level_name(diag.level);
      out += ansi::reset;
      out += ansi::bold;
      out += ": ";
      out += diag.message;
      out += ansi::reset;
      out += '\n';
    } else {
      out += prefix;
      out += diagnostic_level_name(diag.level);
      out += ": ";
      out += diag.message;
      out += '\n';
    }

    const auto *file = sources_.get(diag.file_id);

    // Render labels with source context.
    for (const auto &label : diag.labels) {
      render_label(out, label, prefix, file);
    }

    // Render suggested fixes.
    for (const auto &fix : diag.fixes) {
      render_fix(out, fix, prefix, file);
    }

    // Render child diagnostics (notes, help).
    for (const auto &child : diag.children) {
      render_diagnostic(out, child, indent + 1);
    }
  }

  /// Renders a labeled source span with file and line context when available.
  void render_label(std::string &out, const diagnostic_label &label,
                    const std::string &prefix, const source_file *file) const {
    if (label.span.empty() && label.span.start == 0) {
      return; // Dummy span — nothing to render.
    }

    std::string arrow_color = use_color_ ? std::string(ansi::bold_blue) : "";
    std::string reset = use_color_ ? std::string(ansi::reset) : "";

    if (file == nullptr) {
      out += prefix;
      out += "  ";
      out += arrow_color;
      out += "-->";
      out += reset;
      out += " <unknown file>";
      if (!label.message.empty()) {
        out += ": ";
        out += label.message;
      }
      out += '\n';
      return;
    }

    auto start_lc = file->resolve(label.span.start);
    auto end_lc =
        file->resolve(label.span.end > label.span.start ? label.span.end - 1
                                                        : label.span.start);

    // Location line: "  --> file.kira:10:5"
    out += prefix;
    out += "  ";
    out += arrow_color;
    out += "-->";
    out += reset;
    out += " ";
    out += file->name();
    out += ":";
    out += std::to_string(start_lc.line);
    out += ":";
    out += std::to_string(start_lc.column);
    out += '\n';

    // Source line(s) with underline.
    // For simplicity we render single-line labels with a caret underline.
    // Multi-line labels get a vertical bar in the margin.
    std::string line_color = use_color_ ? std::string(ansi::bold_blue) : "";
    std::string underline_color =
        use_color_ ? std::string(diagnostic_level_color(label.level)) : "";

    uint32_t gutter_width = count_digits(std::max(start_lc.line, end_lc.line));
    gutter_width = std::max(gutter_width, 3u); // Minimum gutter width.

    if (start_lc.line == end_lc.line) {
      // Single-line label.
      auto source_line = file->line_at(label.span.start);

      // Gutter + pipe.
      out += prefix;
      out += "  ";
      out += line_color;
      out += pad_left(std::to_string(start_lc.line), gutter_width);
      out += " | ";
      out += reset;
      out += source_line;
      out += '\n';

      // Underline.
      uint32_t caret_start = start_lc.column - 1;
      uint32_t caret_len = label.span.len();
      if (caret_len == 0) {
        caret_len = 1;
      }

      // Clamp to line length.
      if (caret_start > source_line.size()) {
        caret_start = static_cast<uint32_t>(source_line.size());
      }
      if (caret_start + caret_len > source_line.size()) {
        caret_len = static_cast<uint32_t>(source_line.size()) - caret_start;
        if (caret_len == 0) {
          caret_len = 1;
        }
      }

      out += prefix;
      out += "  ";
      out += line_color;
      out += std::string(gutter_width, ' ');
      out += " | ";
      out += underline_color;
      out += std::string(caret_start, ' ');
      out += std::string(caret_len, '^');
      if (!label.message.empty()) {
        out += ' ';
        out += label.message;
      }
      out += reset;
      out += '\n';
    } else {
      // Multi-line span — show first and last lines with a vertical
      // connector in between.
      auto first_line = file->line_at(label.span.start);
      auto last_line = file->line_at(label.span.end > 0 ? label.span.end - 1
                                                        : label.span.end);

      // First line.
      out += prefix;
      out += "  ";
      out += line_color;
      out += pad_left(std::to_string(start_lc.line), gutter_width);
      out += " | ";
      out += reset;
      out += underline_color;
      out += first_line;
      out += reset;
      out += '\n';

      // Ellipsis if there's more than one line between.
      if (end_lc.line - start_lc.line > 1) {
        out += prefix;
        out += "  ";
        out += line_color;
        out += std::string(gutter_width, '.');
        out += " | ";
        out += reset;
        out += '\n';
      }

      // Last line.
      if (end_lc.line != start_lc.line) {
        out += prefix;
        out += "  ";
        out += line_color;
        out += pad_left(std::to_string(end_lc.line), gutter_width);
        out += " | ";
        out += reset;
        out += underline_color;
        out += last_line;
        out += reset;
        out += '\n';
      }

      // Underline on last line.
      uint32_t caret_end = end_lc.column;
      out += prefix;
      out += "  ";
      out += line_color;
      out += std::string(gutter_width, ' ');
      out += " | ";
      out += underline_color;
      out += std::string(caret_end, '^');
      if (!label.message.empty()) {
        out += ' ';
        out += label.message;
      }
      out += reset;
      out += '\n';
    }
  }

  /// Renders a suggested source edit preview below a diagnostic.
  void render_fix(std::string &out, const suggested_fix &fix,
                  const std::string &prefix, const source_file *file) const {
    std::string help_color = use_color_ ? std::string(ansi::bold_green) : "";
    std::string reset = use_color_ ? std::string(ansi::reset) : "";

    out += prefix;
    out += "  ";
    out += help_color;
    out += "fix";
    out += reset;
    out += ": ";
    out += fix.description;
    out += '\n';

    if (!fix.replacement.empty()) {
      if (file == nullptr) {
        out += prefix;
        out += "    ";
        out += help_color;
        out += fix.replacement;
        out += reset;
        out += '\n';
        return;
      }

      // Show what the code would look like after the fix.
      if (!fix.span.empty()) {
        auto lc = file->resolve(fix.span.start);
        out += prefix;
        out += "    ";
        out += help_color;
        out += std::to_string(lc.line);
        out += " | ";
        out += reset;

        // Reconstruct the line with the fix applied.
        auto source_line = file->line_at(fix.span.start);
        auto line_start_offset = fix.span.start - (lc.column - 1);

        // Characters before the replacement.
        uint32_t pre_len = fix.span.start - line_start_offset;
        if (pre_len <= source_line.size()) {
          out += source_line.substr(0, pre_len);
        }

        // The replacement text (highlighted).
        out += help_color;
        out += fix.replacement;
        out += reset;

        // Characters after the replacement.
        uint32_t post_start = fix.span.end - line_start_offset;
        if (post_start < source_line.size()) {
          out += source_line.substr(post_start);
        }
        out += '\n';
      } else {
        // Insertion (empty span) — just show the text.
        out += prefix;
        out += "    ";
        out += help_color;
        out += fix.replacement;
        out += reset;
        out += '\n';
      }
    }
  }

  /// Left-pads line-number strings so diagnostic gutters align cleanly.
  [[nodiscard]] static auto pad_left(std::string s, uint32_t width)
      -> std::string {
    if (s.size() >= width) {
      return s;
    }
    return std::string(width - s.size(), ' ') + s;
  }

  /// Counts decimal digits for source-line gutter sizing.
  [[nodiscard]] static auto count_digits(uint32_t n) -> uint32_t {
    if (n == 0) {
      return 1;
    }
    uint32_t digits = 0;
    while (n > 0) {
      ++digits;
      n /= 10;
    }
    return digits;
  }

  const source_manager
      &sources_;   ///< Source table used for file/snippet lookup.
  bool use_color_; ///< Whether ANSI escape sequences are emitted.
};

} // namespace kira
