#pragma once

#include "src/hir/storage_program.h"

namespace cinder::driver {

/// The driver's program in storage form (`hir::storage_program`).
using storage_program = hir::storage_program;
using hir::prepare_storage_program;

/// Whether the driver runs and builds programs through the storage form, the
/// default, rather than the pointer-per-aggregate form (`CINDER_STORAGE_FORM=0`).
/// A whole program takes one form or the other (spec/value-semantics.md,
/// decision 6). The pointer form is removed by T-14.
[[nodiscard]] auto storage_form_enabled() -> bool;

} // namespace cinder::driver
