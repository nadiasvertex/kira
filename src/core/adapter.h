#pragma once

#include <span>
#include <string>
#include <vector>

#include "src/core/nodes.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/checker_results.h"

/// Builds the typed core from the AST and the checker's side tables
/// (`spec/unified-checker-plan.md`, phase 2).
///
/// The adapter exists while the checker still writes side tables. It makes no
/// decision of its own: every type, callee, conversion and dispatch comes from
/// a record in `checked_types`. When a record it needs is absent, or a
/// construct has no core form yet, it reports a `gap` and leaves that function
/// out. Phase 12 deletes it, when the checker emits the core directly.
namespace cinder::core {

/// A function the adapter could not build.
struct gap {
  std::string module;
  std::string function;
  source_span span;
  /// What stopped it: an AST node kind such as "crew_expr", or
  /// "missing record: <table>" when the checker left no decision.
  std::string construct;
  std::string detail;
  /// The function is a generic template.
  bool in_template = false;
};

struct adapted_program {
  std::vector<ptr<module>> modules;
  std::vector<gap> gaps;
};

/// Builds one core module per module the session lowers: each file's module,
/// its inline submodules, and each functor instantiation. A module holds the
/// functions HIR lowering would lower, plus generic templates.
///
/// `checked` is not const because the adapter interns the reference and
/// function types it makes explicit. It adds no other kind of type.
[[nodiscard]] auto
adapt_program(std::span<const semantic::parsed_module> inputs,
              const std::vector<bool> &file_has_errors,
              semantic::checked_types &checked) -> adapted_program;

} // namespace cinder::core
