#pragma once

#include <vector>

#include "src/core/adapter.h"
#include "src/core/nodes.h"
#include "src/parser/diagnostic.h"
#include "src/semantic/checker_results.h"
#include "src/semantic/ownership_cfg.h"

/// The ownership graph, built from the typed core (ch. 14, ch. 15, ch. 17;
/// `spec/unified-checker-plan.md`, phase 7).
///
/// One `function_cfg` is built per core function and per lambda or task body
/// in it. Places are the core's places: a loan is rooted at the deepest
/// field path of the place it borrows (`p.a.b`, stopping at a `deref`, an
/// element or a variant payload), so borrows of different fields of one
/// local do not conflict. A `self` parameter is a reference: moving a part
/// that is not `copy` out of it is rejected, in every method, generated or
/// written. The ownership checker (`semantic::ownership`) runs its dataflow
/// over these graphs, and drop scheduling (`hir::compute_drop_schedule`)
/// reads the same graphs.
namespace cinder::core {

/// The graphs of `fn`: its body first, then one per lambda or task body
/// written in it.
[[nodiscard]] auto ownership_graphs(const function &fn,
                                    const semantic::checked_types &checked)
    -> std::vector<semantic::ownership::function_cfg>;

/// Checks every function of `program` the ownership rules apply to: each
/// template and ordinary function once, and generated impls (a `deriving`
/// clause, an item splice) as well. Modules whose file id is at or past
/// `skip_from_fileid` are skipped, as are files that already have errors. A
/// file that gets an error here is marked in `file_has_errors`.
auto check_ownership(const adapted_program &program,
                     const semantic::checked_types &checked,
                     diagnostic_bag &diag, std::vector<bool> &file_has_errors,
                     unsigned skip_from_fileid) -> void;

} // namespace cinder::core
