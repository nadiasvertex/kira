#pragma once

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/comptime/value.h"
#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/semantic/checker_results.h"

namespace cinder::semantic {

// ==========================================================================
//  Compile-time execution on the VM
//
//  Compile-time code runs in the middle of checking. The checker cannot call
//  HIR lowering or the VM directly (both depend on it), so it talks to them
//  through the two interfaces below: the checker implements
//  `comptime_program`, which hands out checked functions ready to lower, and
//  `//src/comptime_vm` implements `comptime_backend`, which lowers, compiles
//  and runs them.
// ==========================================================================

/// One function compile-time code reaches, named the way HIR lowering names
/// it: `owner_module` is the module that declares it, and `hir_name` its name
/// in that module — `f`, `point::show` for a method, `get$3` for an instance.
struct comptime_function {
  const ast::func_decl *decl = nullptr;
  std::string owner_module;
  std::string hir_name;
};

/// A compile-time expression run as a function of its own: a `static
/// assert` condition, a `static let` initializer, a splice operand. It is
/// written in `owner_module`, so the names it calls resolve there, and it
/// reads each of `captures` (name and type) from where it is written — a
/// `static for` loop variable, say — as a parameter, in order.
struct comptime_expression {
  const ast::expr *expr = nullptr;
  std::string owner_module;
  std::optional<file_id_type> file;
  std::vector<std::pair<std::string, type_id>> captures;
};

/// The checker's side of a compile-time call: the checked functions it can
/// make ready for lowering.
class comptime_program {
public:
  comptime_program() = default;
  comptime_program(const comptime_program &) = delete;
  auto operator=(const comptime_program &) -> comptime_program & = delete;
  comptime_program(comptime_program &&) = delete;
  auto operator=(comptime_program &&) -> comptime_program & = delete;
  virtual ~comptime_program() = default;

  /// The records lowering reads, for every function `prepare` has made
  /// ready. Types are settled and refinements erased, as after checking.
  [[nodiscard]] virtual auto view() const -> const checked_types & = 0;

  /// Makes `fn` ready to lower: checked, with every type it records settled,
  /// and its records copied into `view()`. The error says why it is not
  /// ready (a generic template, a body not checked yet, a type still open).
  [[nodiscard]] virtual auto prepare(const ast::func_decl &fn)
      -> std::expected<void, std::string> = 0;

  /// Makes the expression `expr` ready to lower, as `prepare` does a
  /// function's body.
  [[nodiscard]] virtual auto prepare_expression(const ast::expr &expr)
      -> std::expected<void, std::string> = 0;

  /// The function lowered code calls as `hir_name` in `owner_module`, or
  /// `nullopt` if no checked function has that name.
  [[nodiscard]] virtual auto find_function(std::string_view owner_module,
                                           std::string_view hir_name)
      -> std::optional<comptime_function> = 0;

  /// The reified `static let` global lowered code reads as `name`, with the
  /// records building its value copied into `view()`; null if none has that
  /// name.
  [[nodiscard]] virtual auto static_global(std::string_view name)
      -> const lowering_results::static_global_def * = 0;

  /// Performs `operation` for compile-time code (`hir_host_call`): builds
  /// the syntax a quote (`subject`) or an `expr.*` builder evaluates to,
  /// from `args`. The error says why it could not.
  [[nodiscard]] virtual auto host_call(std::string_view operation,
                                       const void *subject,
                                       std::span<const comptime::value> args,
                                       source_span span)
      -> std::expected<comptime::value, std::string> = 0;

  /// The file `fn` is declared in, for an error inside it.
  [[nodiscard]] virtual auto file_of(const ast::func_decl &fn)
      -> std::optional<file_id_type> = 0;

  /// The fields of struct type `id` in declaration order, each with its
  /// type in this instance (`T` substituted); `nullopt` if `id` is not a
  /// struct. Types are settled, as in `view()`.
  [[nodiscard]] virtual auto struct_fields(type_id id)
      -> std::optional<std::vector<std::pair<std::string, type_id>>> = 0;

  /// The payload types of variant `variant` of sum type `id` (a user sum,
  /// `option` or `result`), in order; `nullopt` if it has no such variant.
  [[nodiscard]] virtual auto variant_payload(type_id id,
                                             std::string_view variant)
      -> std::optional<std::vector<type_id>> = 0;
};

/// Why a compile-time call did not produce a value.
struct comptime_failure {
  enum class kind : std::uint8_t {
    /// The VM cannot run this call (a construct lowering or the bytecode
    /// compiler does not support, a value with no VM form). Nothing was
    /// reported; the caller may run the call another way.
    unsupported,
    /// The call ran and failed: a panic, an overflow, or running out of its
    /// step budget. `message` explains it and `frames` say where.
    failed,
  };
  /// One active call when the failure happened, innermost first.
  struct frame {
    std::string function;
    std::optional<file_id_type> file;
    source_span span;
  };
  kind reason = kind::unsupported;
  std::string message;
  std::vector<frame> frames;
};

/// Runs compile-time calls on the VM.
class comptime_backend {
public:
  comptime_backend() = default;
  comptime_backend(const comptime_backend &) = delete;
  auto operator=(const comptime_backend &) -> comptime_backend & = delete;
  comptime_backend(comptime_backend &&) = delete;
  auto operator=(comptime_backend &&) -> comptime_backend & = delete;
  virtual ~comptime_backend() = default;

  /// Calls `fn` with `args`, which are already in `fn`'s parameter order,
  /// and returns its result as a compile-time value.
  [[nodiscard]] virtual auto call(comptime_program &program,
                                  const comptime_function &fn,
                                  std::span<const comptime::value> args)
      -> std::expected<comptime::value, comptime_failure> = 0;

  /// Runs `expr`, passing `args` for its captures in order, and returns its
  /// value.
  [[nodiscard]] virtual auto evaluate(comptime_program &program,
                                      const comptime_expression &expr,
                                      std::span<const comptime::value> args)
      -> std::expected<comptime::value, comptime_failure> = 0;
};

} // namespace cinder::semantic
