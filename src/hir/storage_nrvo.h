#pragma once

#include <string>
#include <vector>

#include "src/hir/storage.h"

namespace cinder::hir {

/// What the pass decided for the local one function returns.
struct nrvo_decision {
  std::string function;
  /// The source name of the returned local, empty for a temporary.
  std::string local;
  /// Whether the local's storage became the function's result storage.
  bool applied = false;
  /// Why it did not, when it did not; empty otherwise.
  std::string reason;
};

/// Named return value optimization over a function in storage form.
///
/// When every `return` of a function whose result is built in the caller's
/// storage moves the same complete local into that storage, the local is
/// built there from the start: its slot is replaced by the result slot, the
/// moves at the returns are removed, and the local's slot is deleted. The
/// local qualifies when it is a frame local of exactly the result type that is
/// first written outside every conditional and loop, is never moved out of
/// except at the returns, has no address taken (a synthesized `drop` call
/// excepted), and is not captured. Anything else keeps the ordinary move,
/// which is not visible to the program.
///
/// Returns nothing when the function has no returned local to consider.
[[nodiscard]] auto apply_nrvo(storage_function &function)
    -> std::vector<nrvo_decision>;

} // namespace cinder::hir
