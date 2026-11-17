#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
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
  /// `detail` is the message the user sees: the program uses a feature the
  /// compiler does not build yet. Otherwise the gap is the compiler's own,
  /// and the user sees `construct` and `detail` as an internal failure.
  bool feature = false;
  /// The function is a generic template.
  bool in_template = false;
  /// What was left out: a function, a static's value, or an `intrinsic
  /// def`'s signature (which lowering does not need).
  enum class item_kind : std::uint8_t { function, static_global, intrinsic };
  item_kind item = item_kind::function;
};

/// How the program will be compiled, where that changes the core.
struct adapt_options {
  /// Whether contracts and type invariants are checked at run time. Without
  /// them the core has no `contract_check`, and none of the `let return` and
  /// `let self` bindings the checks read.
  bool contract_checks = true;
  /// The code runs only on the compile-time VM (the on-demand entry points
  /// below). Then a quote, a fragment builder and a read of a `static`'s
  /// syntax become `host_call`s, and the function is marked
  /// `function::compile_time`; without it they are gaps, since run-time
  /// code cannot build syntax.
  bool compile_time = false;
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
              semantic::checked_types &checked,
              const adapt_options &options = {}) -> adapted_program;

/// The function `module` declares under the bare name `name`, or null: the
/// table a bare call `f(x)` and string interpolation's `std.fmt` calls are
/// found in (`program_index` members, plus a functor instance's clones).
/// `adapt_program` builds it from the program; compile-time code asks the
/// checker for it.
using function_lookup = std::function<const ast::func_decl *(
    std::string_view module, std::string_view name)>;

/// Builds one function on demand, as `adapt_program` builds each function of
/// a module: `decl` as `name` in `module`. For compile-time code, which the
/// checker hands out one function at a time while it is still checking
/// (`semantic::comptime_program`); `options.compile_time` marks it so.
/// The function must not be a generic template (a const-generic instance,
/// which keeps its template's parameter list, is not one).
[[nodiscard]] auto adapt_function(const ast::func_decl &decl, std::string name,
                                  std::string module,
                                  semantic::checked_types &checked,
                                  const function_lookup &functions,
                                  const adapt_options &options = {})
    -> std::expected<ptr<function>, gap>;

/// A name a compile-time expression reads from where it is written (a
/// `static for` loop variable, say), passed in as a parameter.
struct expression_capture {
  std::string name;
  type_id type = k_unknown_type;
};

/// Builds a compile-time expression (a `static assert` condition, a `static
/// let` initializer, a splice operand) as the whole body of a function of
/// its own, named `name` in `module`, as `hir::lower_comptime_expression`
/// lowers it: each of `captures` is a by-value parameter in order, and the
/// body's result is the expression. The function has no declaration, is
/// `compile_time`, and drops nothing; every read of a reified static in it
/// is a fresh value.
[[nodiscard]] auto adapt_expression(const ast::expr &expr,
                                    std::span<const expression_capture> captures,
                                    std::string name, std::string module,
                                    semantic::checked_types &checked,
                                    const function_lookup &functions)
    -> std::expected<ptr<function>, gap>;

/// Builds one reified `static let`'s value, as `adapt_program` builds each
/// static of a module (and `hir::lower_static_global` lowers it), owned by
/// `module`.
[[nodiscard]] auto
adapt_static(const semantic::lowering_results::static_global_def &def,
             std::string module, semantic::checked_types &checked,
             source_span span) -> std::expected<static_global, gap>;

/// Builds the declared signature of one `intrinsic def`, as `adapt_program`
/// builds each a module declares, for a program the verifier checks.
[[nodiscard]] auto adapt_intrinsic(const ast::func_decl &decl, std::string name,
                                   semantic::checked_types &checked)
    -> std::expected<intrinsic_decl, gap>;

} // namespace cinder::core
