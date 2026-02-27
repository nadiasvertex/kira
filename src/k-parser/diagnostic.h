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
//  Diagnostic severity levels.
//
//  We deliberately include "Help" and "Note" as first-class levels because
//  Kira's philosophy is that the compiler is a teacher. Errors should always
//  be accompanied by context that helps the user understand *why* something
//  is wrong and *how* to fix it.
// ==========================================================================
enum class DiagnosticLevel : uint8_t {
  /// A fatal error that prevents further analysis of the current construct.
  /// The parser will attempt error recovery, but the AST node will be
  /// marked as containing errors.
  Error,

  /// A problem that doesn't prevent parsing from continuing but indicates
  /// something that will fail in a later compilation phase. For example,
  /// using a modifier in the wrong position.
  Warning,

  /// Additional context attached to a preceding Error or Warning.
  /// Notes point at related locations ("this `{` was opened here",
  /// "previous definition was here", etc.).
  Note,

  /// A concrete suggestion for how to fix the problem. This is the
  /// cooperative part — we don't just complain, we show the user what
  /// to do. Help messages may include suggested replacement text.
  Help,
};

/// Returns a human-readable label for a diagnostic level, suitable for
/// rendering in a terminal with ANSI colors.
[[nodiscard]] constexpr std::string_view diagnostic_level_name(
    DiagnosticLevel level) noexcept {
  switch (level) {
    case DiagnosticLevel::Error:   return "error";
    case DiagnosticLevel::Warning: return "warning";
    case DiagnosticLevel::Note:    return "note";
    case DiagnosticLevel::Help:    return "help";
  }
  return "unknown";
}

/// ANSI color codes for pretty-printing diagnostics.
namespace ansi {
  constexpr std::string_view Reset    = "\033[0m";
  constexpr std::string_view Bold     = "\033[1m";
  constexpr std::string_view Dim      = "\033[2m";
  constexpr std::string_view Red      = "\033[31m";
  constexpr std::string_view Yellow   = "\033[33m";
  constexpr std::string_view Blue     = "\033[34m";
  constexpr std::string_view Magenta  = "\033[35m";
  constexpr std::string_view Cyan     = "\033[36m";
  constexpr std::string_view BoldRed     = "\033[1;31m";
  constexpr std::string_view BoldYellow  = "\033[1;33m";
  constexpr std::string_view BoldBlue    = "\033[1;34m";
  constexpr std::string_view BoldMagenta = "\033[1;35m";
  constexpr std::string_view BoldCyan    = "\033[1;36m";
  constexpr std::string_view BoldGreen   = "\033[1;32m";
}  // namespace ansi

/// Returns the ANSI color for a diagnostic level.
[[nodiscard]] constexpr std::string_view diagnostic_level_color(
    DiagnosticLevel level) noexcept {
  switch (level) {
    case DiagnosticLevel::Error:   return ansi::BoldRed;
    case DiagnosticLevel::Warning: return ansi::BoldYellow;
    case DiagnosticLevel::Note:    return ansi::BoldBlue;
    case DiagnosticLevel::Help:    return ansi::BoldGreen;
  }
  return ansi::Reset;
}

// ==========================================================================
//  A label is a short annotation attached to a span of source code.
//  Diagnostics may carry multiple labels to highlight different parts
//  of the source that are relevant to the problem.
// ==========================================================================
struct DiagnosticLabel {
  Span span;
  std::string message;
  DiagnosticLevel level;  ///< Controls the underline color/style

  DiagnosticLabel(Span s, std::string msg,
                  DiagnosticLevel lvl = DiagnosticLevel::Error)
      : span(s), message(std::move(msg)), level(lvl) {}
};

// ==========================================================================
//  A suggested edit — a concrete replacement the user can apply.
//  When we know how to fix the problem, we show it.
// ==========================================================================
struct SuggestedFix {
  std::string description;  ///< e.g., "add a colon here"
  Span span;                ///< The span to replace (may be empty for insertions)
  std::string replacement;  ///< The text to insert/replace with
};

// ==========================================================================
//  Diagnostic — a single error, warning, note, or help message.
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
struct Diagnostic {
  DiagnosticLevel level;
  std::string message;
  FileId file_id = 0;

  /// Primary and secondary labels pointing at source code.
  std::vector<DiagnosticLabel> labels;

  /// Child diagnostics — notes and help messages attached to this one.
  std::vector<Diagnostic> children;

  /// Suggested fixes that tools/editors can auto-apply.
  std::vector<SuggestedFix> fixes;

  // -- Builder-pattern methods for ergonomic construction --

  Diagnostic(DiagnosticLevel lvl, std::string msg, FileId fid = 0)
      : level(lvl), message(std::move(msg)), file_id(fid) {}

  /// Add a primary label (points at the main error site).
  Diagnostic& with_label(Span span, std::string msg) & {
    labels.emplace_back(span, std::move(msg), level);
    return *this;
  }

  Diagnostic&& with_label(Span span, std::string msg) && {
    labels.emplace_back(span, std::move(msg), level);
    return std::move(*this);
  }

  /// Add a secondary label (points at related code).
  Diagnostic& with_secondary_label(Span span, std::string msg) & {
    labels.emplace_back(span, std::move(msg), DiagnosticLevel::Note);
    return *this;
  }

  Diagnostic&& with_secondary_label(Span span, std::string msg) && {
    labels.emplace_back(span, std::move(msg), DiagnosticLevel::Note);
    return std::move(*this);
  }

  /// Add a note — extra context about why this is an error.
  Diagnostic& with_note(std::string msg) & {
    children.emplace_back(DiagnosticLevel::Note, std::move(msg), file_id);
    return *this;
  }

  Diagnostic&& with_note(std::string msg) && {
    children.emplace_back(DiagnosticLevel::Note, std::move(msg), file_id);
    return std::move(*this);
  }

  /// Add a help message — a suggestion for how to fix the problem.
  Diagnostic& with_help(std::string msg) & {
    children.emplace_back(DiagnosticLevel::Help, std::move(msg), file_id);
    return *this;
  }

  Diagnostic&& with_help(std::string msg) && {
    children.emplace_back(DiagnosticLevel::Help, std::move(msg), file_id);
    return std::move(*this);
  }

  /// Add a suggested fix (machine-applicable).
  Diagnostic& with_fix(std::string desc, Span span,
                        std::string replacement) & {
    fixes.push_back(
        SuggestedFix{std::move(desc), span, std::move(replacement)});
    return *this;
  }

  Diagnostic&& with_fix(std::string desc, Span span,
                         std::string replacement) && {
    fixes.push_back(
        SuggestedFix{std::move(desc), span, std::move(replacement)});
    return std::move(*this);
  }

  [[nodiscard]] bool is_error() const noexcept {
    return level == DiagnosticLevel::Error;
  }

  [[nodiscard]] bool is_warning() const noexcept {
    return level == DiagnosticLevel::Warning;
  }
};

// ==========================================================================
//  DiagnosticBag — collects diagnostics during a compilation phase.
//
//  This is the primary interface that the parser (and later phases) use
//  to report problems. It tracks error/warning counts and provides a
//  limit to prevent cascading error floods that confuse rather than help.
// ==========================================================================
class DiagnosticBag {
 public:
  static constexpr uint32_t kDefaultMaxErrors = 50;

  explicit DiagnosticBag(uint32_t max_errors = kDefaultMaxErrors)
      : max_errors_(max_errors) {}

  // -- Reporting --

  /// Add a diagnostic to the bag.
  void emit(Diagnostic diag) {
    if (diag.is_error()) {
      ++error_count_;
      if (error_count_ > max_errors_ && !cascade_reported_) {
        cascade_reported_ = true;
        diagnostics_.push_back(Diagnostic(
            DiagnosticLevel::Error,
            std::format("too many errors ({}); stopping here to avoid "
                        "overwhelming you — fix the issues above first "
                        "and the rest will likely resolve",
                        error_count_),
            diag.file_id));
        return;
      }
      if (error_count_ > max_errors_) {
        return;  // Silently drop — we already told the user.
      }
    }
    if (diag.is_warning()) {
      ++warning_count_;
    }
    diagnostics_.push_back(std::move(diag));
  }

  /// Convenience: emit an error.
  void error(FileId file_id, std::string message, Span span,
             std::string label_msg = "") {
    auto diag = Diagnostic(DiagnosticLevel::Error, std::move(message), file_id);
    if (!label_msg.empty()) {
      diag.with_label(span, std::move(label_msg));
    } else {
      diag.with_label(span, "here");
    }
    emit(std::move(diag));
  }

  /// Convenience: emit a warning.
  void warning(FileId file_id, std::string message, Span span,
               std::string label_msg = "") {
    auto diag =
        Diagnostic(DiagnosticLevel::Warning, std::move(message), file_id);
    if (!label_msg.empty()) {
      diag.with_label(span, std::move(label_msg));
    } else {
      diag.with_label(span, "here");
    }
    emit(std::move(diag));
  }

  // -- Queries --

  [[nodiscard]] bool has_errors() const noexcept { return error_count_ > 0; }

  [[nodiscard]] uint32_t error_count() const noexcept { return error_count_; }

  [[nodiscard]] uint32_t warning_count() const noexcept {
    return warning_count_;
  }

  [[nodiscard]] bool at_error_limit() const noexcept {
    return error_count_ > max_errors_;
  }

  [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept {
    return diagnostics_;
  }

  [[nodiscard]] std::vector<Diagnostic>& diagnostics() noexcept {
    return diagnostics_;
  }

  /// Take ownership of all diagnostics (empties this bag).
  [[nodiscard]] std::vector<Diagnostic> take() {
    auto result = std::move(diagnostics_);
    diagnostics_.clear();
    error_count_ = 0;
    warning_count_ = 0;
    cascade_reported_ = false;
    return result;
  }

  /// Clear all diagnostics.
  void clear() {
    diagnostics_.clear();
    error_count_ = 0;
    warning_count_ = 0;
    cascade_reported_ = false;
  }

 private:
  std::vector<Diagnostic> diagnostics_;
  uint32_t error_count_ = 0;
  uint32_t warning_count_ = 0;
  uint32_t max_errors_;
  bool cascade_reported_ = false;
};

// ==========================================================================
//  DiagnosticRenderer — formats diagnostics as pretty-printed terminal
//  output with source context, underlines, and ANSI colors.
//
//  The output style is inspired by Rust's error messages: clear, helpful,
//  and visually structured so the user's eye is drawn to the important
//  parts. The philosophy: if the user has to read an error message more
//  than once to understand it, the error message has failed.
// ==========================================================================
class DiagnosticRenderer {
 public:
  explicit DiagnosticRenderer(const SourceFile& file, bool use_color = true)
      : file_(file), use_color_(use_color) {}

  /// Render a single diagnostic to a string.
  [[nodiscard]] std::string render(const Diagnostic& diag) const {
    std::string out;
    render_diagnostic(out, diag, /*indent=*/0);
    return out;
  }

  /// Render all diagnostics in a bag.
  [[nodiscard]] std::string render_all(const DiagnosticBag& bag) const {
    std::string out;
    for (const auto& diag : bag.diagnostics()) {
      render_diagnostic(out, diag, /*indent=*/0);
      out += '\n';
    }
    return out;
  }

 private:
  void render_diagnostic(std::string& out, const Diagnostic& diag,
                          int indent) const {
    // Header line: "error: message"
    std::string prefix(static_cast<size_t>(indent * 2), ' ');

    if (use_color_) {
      out += prefix;
      out += diagnostic_level_color(diag.level);
      out += diagnostic_level_name(diag.level);
      out += ansi::Reset;
      out += ansi::Bold;
      out += ": ";
      out += diag.message;
      out += ansi::Reset;
      out += '\n';
    } else {
      out += prefix;
      out += diagnostic_level_name(diag.level);
      out += ": ";
      out += diag.message;
      out += '\n';
    }

    // Render labels with source context.
    for (const auto& label : diag.labels) {
      render_label(out, label, prefix);
    }

    // Render suggested fixes.
    for (const auto& fix : diag.fixes) {
      render_fix(out, fix, prefix);
    }

    // Render child diagnostics (notes, help).
    for (const auto& child : diag.children) {
      render_diagnostic(out, child, indent + 1);
    }
  }

  void render_label(std::string& out, const DiagnosticLabel& label,
                    const std::string& prefix) const {
    if (label.span.empty() && label.span.start == 0) {
      return;  // Dummy span — nothing to render.
    }

    auto start_lc = file_.resolve(label.span.start);
    auto end_lc = file_.resolve(
        label.span.end > label.span.start ? label.span.end - 1
                                          : label.span.start);

    // Location line: "  --> file.kira:10:5"
    std::string arrow_color =
        use_color_ ? std::string(ansi::BoldBlue) : "";
    std::string reset = use_color_ ? std::string(ansi::Reset) : "";

    out += prefix;
    out += "  ";
    out += arrow_color;
    out += "-->";
    out += reset;
    out += " ";
    out += file_.name();
    out += ":";
    out += std::to_string(start_lc.line);
    out += ":";
    out += std::to_string(start_lc.column);
    out += '\n';

    // Source line(s) with underline.
    // For simplicity we render single-line labels with a caret underline.
    // Multi-line labels get a vertical bar in the margin.
    std::string line_color =
        use_color_ ? std::string(ansi::BoldBlue) : "";
    std::string underline_color =
        use_color_ ? std::string(diagnostic_level_color(label.level)) : "";

    uint32_t gutter_width = count_digits(
        std::max(start_lc.line, end_lc.line));
    gutter_width = std::max(gutter_width, 3u);  // Minimum gutter width.

    if (start_lc.line == end_lc.line) {
      // Single-line label.
      auto source_line = file_.line_at(label.span.start);

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
      if (caret_len == 0) caret_len = 1;

      // Clamp to line length.
      if (caret_start > source_line.size()) {
        caret_start = static_cast<uint32_t>(source_line.size());
      }
      if (caret_start + caret_len > source_line.size()) {
        caret_len =
            static_cast<uint32_t>(source_line.size()) - caret_start;
        if (caret_len == 0) caret_len = 1;
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
      auto first_line = file_.line_at(label.span.start);
      auto last_line = file_.line_at(
          label.span.end > 0 ? label.span.end - 1 : label.span.end);

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

  void render_fix(std::string& out, const SuggestedFix& fix,
                  const std::string& prefix) const {
    std::string help_color =
        use_color_ ? std::string(ansi::BoldGreen) : "";
    std::string reset = use_color_ ? std::string(ansi::Reset) : "";

    out += prefix;
    out += "  ";
    out += help_color;
    out += "fix";
    out += reset;
    out += ": ";
    out += fix.description;
    out += '\n';

    if (!fix.replacement.empty()) {
      // Show what the code would look like after the fix.
      if (!fix.span.empty()) {
        auto lc = file_.resolve(fix.span.start);
        out += prefix;
        out += "    ";
        out += help_color;
        out += std::to_string(lc.line);
        out += " | ";
        out += reset;

        // Reconstruct the line with the fix applied.
        auto source_line = file_.line_at(fix.span.start);
        auto line_start_offset =
            fix.span.start - (lc.column - 1);

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

  [[nodiscard]] static std::string pad_left(std::string s, uint32_t width) {
    if (s.size() >= width) return s;
    return std::string(width - s.size(), ' ') + s;
  }

  [[nodiscard]] static uint32_t count_digits(uint32_t n) {
    if (n == 0) return 1;
    uint32_t digits = 0;
    while (n > 0) {
      ++digits;
      n /= 10;
    }
    return digits;
  }

  const SourceFile& file_;
  bool use_color_;
};

}  // namespace kira