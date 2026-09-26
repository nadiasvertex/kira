#pragma once

#include <string>

#include "src/parser/source_location.h"
#include "src/semantic/types.h"

namespace cinder::semantic {

/// Renders every elaboration decision in `checked` as deterministic text.
///
/// This is the safety net for the inference rewrite
/// (`spec/inference-rewrite.md` phase 0), and it exists because the test suite
/// cannot see the thing the rewrite is most likely to break. `check_program`
/// answers two questions at once: what type each expression has, and — far
/// larger — which function every call, operator, index, literal conversion,
/// loop and comprehension actually resolved to. The second answer is tens of
/// thousands of decisions wide, it is what lowering consumes, and almost none
/// of it is asserted anywhere. A refactor that silently re-resolved one call to
/// a different instance would keep all 28 test targets green.
///
/// So: snapshot the whole surface, diff it. Every map in `checked_types` is
/// rendered, not a chosen subset — a subset would quietly stop covering
/// whatever the next change happens to touch. Types are rendered by their
/// display spelling and declarations by name rather than by `type_id` or
/// pointer, so the output is stable across runs and readable in a diff.
///
/// Ordering is by source site (file, then byte range), so a diff lands next
/// to the code that moved. A key the checker recorded without a file — see
/// `checked_types::node_files` — renders its file as `?` and still sorts
/// stably by offset.
[[nodiscard]] auto render_snapshot(const checked_types &checked,
                                   const source_manager &sources)
    -> std::string;

} // namespace cinder::semantic
