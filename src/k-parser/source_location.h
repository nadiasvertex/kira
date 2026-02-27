#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace kira {

/// @brief A byte offset into a source file.
///
/// We use a raw offset rather than line/column so that all position
/// arithmetic is O(1). Line and column are computed lazily when needed
/// for diagnostics.
using byte_offset = uint32_t;

/// @brief A half-open range [start, end) of byte offsets in a source file.
///
/// Spans are the fundamental unit of source location tracking. Every AST
/// node, every token, and every diagnostic carries a Span so we can always
/// point the user back to exactly the piece of source code we're talking
/// about.
struct source_span {
  byte_offset start = 0;
  byte_offset end = 0;

  /// Returns true if this span is empty (zero-length).
  [[nodiscard]] constexpr bool empty() const noexcept { return start == end; }

  /// Returns the length of this span in bytes.
  [[nodiscard]] constexpr uint32_t len() const noexcept { return end - start; }

  /// Returns a dummy/invalid span. Used as a sentinel for synthesized nodes
  /// that don't correspond to any real source text (e.g., error recovery
  /// insertions).
  [[nodiscard]] static constexpr source_span dummy() noexcept { return source_span{0, 0}; }

  /// Merge two spans into one that covers both. The result spans from
  /// the earliest start to the latest end. This is used when building
  /// AST nodes that cover multiple tokens.
  [[nodiscard]] constexpr source_span merge(source_span other) const noexcept {
    if (empty())
      return other;
    if (other.empty())
      return *this;
    return source_span{
        start < other.start ? start : other.start,
        end > other.end ? end : other.end,
    };
  }

  /// Extend this span to cover `other` as well (mutating version).
  constexpr void extend_to(source_span other) noexcept { *this = merge(other); }

  constexpr bool operator==(const source_span &) const noexcept = default;
  constexpr auto operator<=>(const Span &) const noexcept = default;
};

/// @brief Identifier for a source file within a compilation session.
///
/// We use a small integer index rather than carrying the filename string
/// everywhere. The SourceManager (or equivalent) maps these back to file
/// paths and source text when needed.
using FileId = uint16_t;

/// @brief A fully-qualified location in source code: file + span.
///
/// This is what diagnostics use to point at source code. It carries
/// enough information to produce a full error message with file name,
/// line number, column, and the underlined source snippet.
struct source_location {
  FileId file_id = 0;
  source_span span;

  [[nodiscard]] static constexpr source_location dummy() noexcept {
    return source_location{0, source_span::dummy()};
  }

  [[nodiscard]] constexpr source_location
  merge(source_location other) const noexcept {
    // Only merge locations from the same file; if they differ, prefer `this`.
    if (file_id != other.file_id)
      return *this;
    return source_location{file_id, span.merge(other.span)};
  }

  constexpr bool operator==(const source_location &) const noexcept = default;
};

/// @brief Resolved line and column information for display.
///
/// This is computed on-demand from a Span + source text. We don't store
/// this in every AST node because it's expensive to compute and rarely
/// needed — only when formatting diagnostics.
struct line_column {
  uint32_t line = 1;   ///< 1-based line number
  uint32_t column = 1; ///< 1-based column number (in bytes, not graphemes)
};

/// @brief A read-only view of a source file's contents, along with
/// precomputed line-start offsets for efficient line/column lookup.
///
/// The source_file does NOT own the source text — it borrows it. Make sure
/// the backing storage outlives the source_file.
class source_file {
public:
  source_file(FileId id, std::string name, std::string source)
      : id_(id), name_(std::move(name)), source_(std::move(source)) {
    build_line_index();
  }

  [[nodiscard]] FileId id() const noexcept { return id_; }
  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] std::string_view source() const noexcept { return source_; }

  /// Resolve a byte offset to a 1-based line and column.
  [[nodiscard]] line_column resolve(byte_offset offset) const noexcept {
    if (line_starts_.empty()) {
      return {1, 1};
    }

    // Binary search for the line containing this offset.
    // We want the last line_start that is <= offset.
    uint32_t lo = 0;
    uint32_t hi = static_cast<uint32_t>(line_starts_.size());
    while (lo + 1 < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (line_starts_[mid] <= offset) {
        lo = mid;
      } else {
        hi = mid;
      }
    }

    return line_column{
        .line = lo + 1,
        .column = static_cast<uint32_t>(offset - line_starts_[lo]) + 1,
    };
  }

  /// Extract the source text for a given span.
  [[nodiscard]] std::string_view text_at(source_span span) const noexcept {
    if (span.start >= source_.size())
      return {};
    auto actual_end = span.end <= source_.size()
                          ? span.end
                          : static_cast<byte_offset>(source_.size());
    return std::string_view(source_).substr(span.start,
                                            actual_end - span.start);
  }

  /// Extract the full source line that contains the given byte offset.
  /// Used to display the source context in diagnostic messages.
  [[nodiscard]] std::string_view line_at(byte_offset offset) const noexcept {
    auto lc = resolve(offset);
    uint32_t line_idx = lc.line - 1;
    if (line_idx >= line_starts_.size())
      return {};

    byte_offset line_start = line_starts_[line_idx];
    byte_offset line_end;
    if (line_idx + 1 < line_starts_.size()) {
      line_end = line_starts_[line_idx + 1];
    } else {
      line_end = static_cast<byte_offset>(source_.size());
    }

    // Strip trailing newline from the returned view.
    while (line_end > line_start &&
           (source_[line_end - 1] == '\n' || source_[line_end - 1] == '\r')) {
      --line_end;
    }

    return std::string_view(source_).substr(line_start, line_end - line_start);
  }

  /// Return the number of lines in the source file.
  [[nodiscard]] uint32_t line_count() const noexcept {
    return static_cast<uint32_t>(line_starts_.size());
  }

private:
  void build_line_index() {
    line_starts_.clear();
    line_starts_.push_back(0); // Line 1 starts at offset 0.

    for (byte_offset i = 0; i < static_cast<byte_offset>(source_.size()); ++i) {
      if (source_[i] == '\n') {
        line_starts_.push_back(i + 1);
      }
    }
  }

  FileId id_;
  std::string name_;
  std::string source_;
  std::vector<byte_offset> line_starts_;
};

} // namespace kira
