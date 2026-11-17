#pragma once

#include <string>

#include "src/parser/source_location.h"

/// The types HIR lowering shares with its callers. Lowering itself, from the
/// typed core, is `lower_core.h`.
namespace cinder::hir {

/// Why drop emission (`drop_emission.h`) could not make a drop the schedule
/// asked for, and where.
struct lowering_error {
  source_span span;
  std::string message;
  /// A feature the compiler does not build yet, which the user can avoid.
  /// Otherwise the error is a compiler bug.
  bool feature = false;
};

/// Knobs the driver hands lowering, as opposed to facts it reads out of the
/// checker.
struct lowering_options {
  /// Whether a contract the checker could not discharge statically becomes a
  /// runtime `hir_contract_check`. False is the spec's release elision
  /// ("Release builds may elide runtime contract checks with an explicit
  /// flag — doing so is the programmer's assertion that all contracts hold by
  /// other means", spec/cinder-reference.md), reached via
  /// `--no-contract-checks`. It is deliberately not the default: a contract
  /// that is only *believed* silently is worth less than one that is checked.
  bool contract_checks = true;
  /// Whether the code runs while compiling, on the compile-time VM. Only
  /// then can it work on quoted syntax (`hir_host_call`), which has no
  /// run-time form.
  bool compile_time = false;
};

} // namespace cinder::hir
