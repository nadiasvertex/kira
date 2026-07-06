#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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
  byte_offset start = 0; ///< First byte covered by the span.
  byte_offset end = 0;   ///< One-past-the-end byte covered by the span.

  /// @brief Returns whether this span covers no concrete source bytes.
  ///
  /// Empty spans are used both for zero-width insertion points and for
  /// synthesized placeholders introduced during recovery. Later phases should
  /// treat them as locations that can anchor diagnostics, but not as evidence
  /// that user-authored text existed there.
  [[nodiscard]] constexpr auto empty() const noexcept -> bool { return start == end; }

  /// @brief Returns the width of the covered source region in bytes.
  ///
  /// This stays in byte space so lexing and parsing can do constant-time span
  /// arithmetic without paying for Unicode-aware column tracking.
  [[nodiscard]] constexpr auto len() const noexcept -> uint32_t { return end - start; }

  /// @brief Returns a sentinel span for synthesized or unavailable locations.
  ///
  /// Downstream code should preserve dummy spans on synthetic nodes instead of
  /// inventing fake ranges, so diagnostics can distinguish "inserted by the
  /// compiler" from "came from the user's source".
  [[nodiscard]] static constexpr auto dummy() noexcept -> source_span { return source_span{.start=0, .end=0}; }

  /// @brief Returns a span covering both this range and `other`.
  ///
  /// Parsers and later tree-rewriting phases use this to give compound nodes a
  /// stable source envelope. Empty spans are ignored so recovery placeholders do
  /// not accidentally widen a real source range.
  ///
  /// @param other The additional source range to include.
  [[nodiscard]] constexpr auto merge(source_span other) const noexcept -> source_span {
    if (empty()) {
      return other;
}
    if (other.empty()) {
      return *this;
}
    return source_span{
        .start=start < other.start ? start : other.start,
        .end=end > other.end ? end : other.end,
    };
  }

  /// @brief Mutates this span so it encloses `other` as well.
  ///
  /// This is the in-place counterpart to `merge` for callers that build spans
  /// incrementally while consuming tokens.
  ///
  /// @param other The additional range to absorb.
  constexpr void extend_to(source_span other) noexcept { *this = merge(other); }

  constexpr auto operator==(const source_span &) const noexcept -> bool = default;
  constexpr auto operator<=>(const source_span &) const noexcept = default;
};

/// @brief Identifier for a source file within a compilation session.
///
/// We use a small integer index rather than carrying the filename string
/// everywhere. The SourceManager (or equivalent) maps these back to file
/// paths and source text when needed.
using file_id_type = uint16_t;

/// @brief A fully-qualified location in source code: file + span.
///
/// This is what diagnostics use to point at source code. It carries
/// enough information to produce a full error message with file name,
/// line number, column, and the underlined source snippet.
struct source_location {
  file_id_type file_id = 0; ///< Source file containing the span.
  source_span span;         ///< Byte range within that file.

  /// @brief Returns a sentinel location for synthesized or detached nodes.
  ///
  /// Later phases should keep dummy locations attached to synthetic work rather
  /// than projecting them onto an arbitrary file, which would make diagnostics
  /// misleading.
  [[nodiscard]] static constexpr auto dummy() noexcept -> source_location {
    return source_location{.file_id=0, .span=source_span::dummy()};
  }

  /// @brief Merges two locations from the same file.
  ///
  /// Compound declarations and diagnostics often need a single location that
  /// covers multiple child ranges. Cross-file merges are intentionally ignored,
  /// because forcing a multi-file construct into one location would hide the
  /// fact that it spans separate compilation units.
  ///
  /// @param other The other location to combine with this one.
  [[nodiscard]] constexpr auto
  merge(source_location other) const noexcept -> source_location {
    // Only merge locations from the same file; if they differ, prefer `this`.
    if (file_id != other.file_id) {
      return *this;
}
    return source_location{.file_id=file_id, .span=span.merge(other.span)};
  }

  constexpr auto operator==(const source_location &) const noexcept -> bool = default;
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
  /// @brief Stores a source file together with indexing data for diagnostics.
  ///
  /// The parser and renderer keep everything in byte offsets during normal
  /// operation. `source_file` is the boundary object that turns those offsets
  /// back into human-facing file, line, and snippet information when needed.
  ///
  /// @param id Stable compilation-session identifier for this file.
  /// @param name Display name used in diagnostics.
  /// @param source Owned source text for the file.
  source_file(file_id_type id, std::string name, std::string source)
      : id_(id), name_(std::move(name)), source_(std::move(source)) {
    build_line_index();
  }

  /// @brief Returns the stable identifier used by tokens and diagnostics.
  [[nodiscard]] auto id() const noexcept -> file_id_type { return id_; }
  /// @brief Returns the display name shown to the user.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }
  /// @brief Returns the full source buffer.
  ///
  /// Consumers should treat the returned view as read-only shared backing
  /// storage for token text and diagnostic rendering.
  [[nodiscard]] auto source() const noexcept -> std::string_view { return source_; }

  /// @brief Resolves a byte offset to a 1-based line and column pair.
  ///
  /// Line/column conversion is intentionally deferred until diagnostics need
  /// it. Parsing and AST construction should continue to traffic only in byte
  /// spans for speed and simplicity.
  ///
  /// @param offset Byte offset into `source()`.
  [[nodiscard]] auto resolve(byte_offset offset) const noexcept -> line_column {
    if (line_starts_.empty()) {
      return {.line=1, .column=1};
    }

    // Binary search for the line containing this offset.
    // We want the last line_start that is <= offset.
    uint32_t lo = 0;
    auto hi = static_cast<uint32_t>(line_starts_.size());
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

  /// @brief Returns the source slice covered by `span`.
  ///
  /// This clamps the end offset to the file length so recovery code can safely
  /// ask for text from partially invalid spans without crashing.
  ///
  /// @param span Byte range to project into the source buffer.
  [[nodiscard]] auto text_at(source_span span) const noexcept -> std::string_view {
    if (span.start >= source_.size()) {
      return {};
}
    // Clamp `actual_end` on both sides so a malformed span can't underflow
    // the resulting length. `source_` is an owned member (not a temporary),
    // so pointer arithmetic into it is safe; `substr` would instead build a
    // dangling view into a temporary `std::string`.
    auto actual_end = std::clamp(span.end, span.start,
                                 static_cast<byte_offset>(source_.size()));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return {source_.data() + span.start, actual_end - span.start};
  }

  /// @brief Returns the logical source line containing `offset`.
  ///
  /// Diagnostic rendering uses this to show a single line of user code without
  /// trailing newline characters. Later phases should prefer `text_at` for
  /// semantic slicing and reserve `line_at` for presentation.
  ///
  /// @param offset Byte offset whose enclosing line should be shown.
  [[nodiscard]] auto line_at(byte_offset offset) const noexcept -> std::string_view {
    auto lc = resolve(offset);
    uint32_t line_idx = lc.line - 1;
    if (line_idx >= line_starts_.size()) {
      return {};
}

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

    // `line_start <= line_end <= source_.size()` always holds here. `substr`
    // would build a dangling view into a temporary `std::string`, so index
    // into the owned `source_` buffer directly instead.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return {source_.data() + line_start, line_end - line_start};
  }

  /// @brief Returns the number of indexed logical lines in the file.
  [[nodiscard]] auto line_count() const noexcept -> uint32_t {
    return static_cast<uint32_t>(line_starts_.size());
  }

private:
  /// Builds the line-start index used by `resolve` and `line_at`.
  ///
  /// This one-time precomputation keeps later diagnostic lookups cheap and
  /// predictable, even when a file produces many parser errors.
  void build_line_index() {
    line_starts_.clear();
    line_starts_.push_back(0); // Line 1 starts at offset 0.

    for (byte_offset i = 0; i < static_cast<byte_offset>(source_.size()); ++i) {
      if (source_[i] == '\n') {
        line_starts_.push_back(i + 1);
      }
    }
  }

  file_id_type id_;                  ///< Stable session-local file identifier.
  std::string name_;                 ///< Display path or logical file name.
  std::string source_;               ///< Owned source buffer backing token text.
  std::vector<byte_offset> line_starts_; ///< Byte offset of each logical line start.
};

/// @brief Owns the source files participating in one compilation session.
///
/// This centralizes file-id assignment so tokens, AST nodes, and diagnostics can
/// all refer to source files by compact integers instead of copying path strings
/// around. Later phases should treat the ids as stable only within the owning
/// `source_manager` instance.
class source_manager {
public:
  /// @brief Adds a file and returns its newly assigned id.
  ///
  /// The returned id is stable for the lifetime of this manager and can be
  /// embedded in tokens, diagnostics, and semantic artifacts built from the
  /// file.
  ///
  /// @param name Display name for diagnostics.
  /// @param source Source text to own.
  [[nodiscard]] auto add_file(std::string name, std::string source)
      -> std::expected<file_id_type, std::string> {
    constexpr auto k_max_source_files =
        static_cast<size_t>(std::numeric_limits<file_id_type>::max()) + 1u;
    if (files_.size() >= k_max_source_files) {
      return std::unexpected{"too many source files for one compilation session"};
    }

    auto id = static_cast<file_id_type>(files_.size());
    files_.emplace_back(id, std::move(name), std::move(source));
    return id;
  }

  /// @brief Returns the file for `id`, or `nullptr` if the id is invalid.
  [[nodiscard]] auto get(file_id_type id) const noexcept -> const source_file * {
    if (static_cast<size_t>(id) >= files_.size()) {
      return nullptr;
    }
    return &files_[id];
  }

  /// @brief Returns a mutable file handle for `id`, or `nullptr` if missing.
  ///
  /// Mutation is rare and should generally be limited to infrastructure code;
  /// later analysis phases should usually consume `source_file` through the
  /// const overload.
  [[nodiscard]] auto get(file_id_type id) noexcept -> source_file * {
    if (static_cast<size_t>(id) >= files_.size()) {
      return nullptr;
    }
    return &files_[id];
  }

  /// @brief Returns the full file table in id order.
  ///
  /// Consumers should treat the returned vector as index-addressable storage
  /// whose positions correspond exactly to `file_id_type` values.
  [[nodiscard]] auto files() const noexcept -> const std::vector<source_file> & {
    return files_;
  }

private:
  std::vector<source_file> files_; ///< Compilation-session source files by id.
};

} // namespace kira
