#pragma once

#include <functional>
#include <set>
#include <unordered_map>
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

/// What building one function's graphs reads and records beyond the core:
/// the summaries of the functions it calls, and what it finds.
struct graph_context {
  /// The inferred summaries (`semantic::ownership::function_summary`); a
  /// call to a function without one uses the summary its signature allows.
  const semantic::ownership::summary_table *summaries = nullptr;
  /// The functions whose inferred summaries the graphs read.
  std::set<const ast::func_decl *> consulted;
  /// Every lambda built, by its `closure_origin`.
  std::unordered_map<const void *, const lambda *> lambdas;
  /// Called with a callee before its summary is read, so the fixed point
  /// can infer callees before their callers.
  std::function<void(const ast::func_decl *)> ensure;
};

/// The graphs of `fn`: its body first, then one per lambda or task body
/// written in it. Calls use the summaries `context` holds; without one,
/// each call uses the summary its callee's signature allows.
[[nodiscard]] auto ownership_graphs(const function &fn,
                                    const semantic::checked_types &checked,
                                    graph_context *context = nullptr)
    -> std::vector<semantic::ownership::function_cfg>;

/// Checks every function of `program` the ownership rules apply to: each
/// template and ordinary function once, and generated impls (a `deriving`
/// clause, an item splice) as well. Modules whose file id is at or past
/// `skip_from_fileid` are skipped, as are files that already have errors. A
/// file that gets an error here is marked in `file_has_errors`.
///
/// First each function's summary is inferred, to a fixed point over the
/// whole program (skipped modules included, since their functions are
/// called). Then a closure found to escape has its borrow-or-move captures
/// changed to moves in `program`, which lowering reads (ch. 16). The
/// summaries are returned, keyed by declaration, for the module metadata.
auto check_ownership(adapted_program &program,
                     const semantic::checked_types &checked,
                     diagnostic_bag &diag, std::vector<bool> &file_has_errors,
                     unsigned skip_from_fileid)
    -> semantic::ownership::summary_table;

} // namespace cinder::core
