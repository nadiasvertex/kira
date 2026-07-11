#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace kira {

/// Decodes a `char_lit` token's raw text (quotes and all, e.g. `'a'`,
/// `'\n'`, `'\u{1F600}'`) into the Unicode scalar value it names.
[[nodiscard]] auto decode_char_literal(std::string_view text)
    -> std::optional<uint32_t>;

/// Decodes a `string_lit` token's raw text (opening/closing quotes and all)
/// into the UTF-8 byte content the runtime `str` value should hold. Source
/// bytes that aren't part of an escape sequence are copied through
/// unchanged (multi-byte UTF-8 sequences already in the source need no
/// re-decoding, unlike `decode_char_literal`, which has to produce a single
/// scalar value rather than a byte range) — only `\...` escapes need
/// translating, mirroring the same escape set `decode_char_literal` accepts.
/// String interpolation (`"...{expr}..."`) is not handled here: nothing
/// downstream of the lexer currently splits interpolation into separate
/// expressions, so a `{`/`}` in the source is treated as a literal character,
/// same as the lexer's own `scan_string` currently does.
[[nodiscard]] auto decode_string_literal(std::string_view text)
    -> std::optional<std::string>;

/// Decodes escape sequences in a raw, unquoted slice of `string_lit` source
/// text — the same escape handling `decode_string_literal` applies to a
/// whole token's inner content, but usable on just one literal-text segment
/// of a string split by interpolation
/// (`spec/string-formatting-design.md`), since a `{...}` region there is
/// never itself passed through this function.
[[nodiscard]] auto decode_string_body(std::string_view unquoted_text)
    -> std::optional<std::string>;

} // namespace kira
