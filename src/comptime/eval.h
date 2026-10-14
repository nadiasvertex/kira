#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/comptime/fragments.h"
#include "src/comptime/value.h"
#include "src/parser/ast.h"
#include "src/parser/diagnostic.h"

namespace cinder::comptime {

/// Runs compile-time code for the checker: `static let` initializers,
/// `static assert` and `static if` conditions, `static for` iterables and
/// guards, splice operands, and calls to `static def` functions.
///
/// Nothing here interprets syntax. The checker installs the bytecode VM
/// (`set_vm_call`, `set_vm_eval`), which lowers each expression or call to
/// HIR and runs it. What this class keeps is what the VM reads but does not
/// own: the bindings compile-time code may read (a `static for` loop
/// variable, a type parameter, an ordinary `let` evaluated on demand), the
/// syntax quotes build (`fragments`), and whether failures are reported or
/// silent.
class evaluator {
public:
  // `locals_` starts with one root frame, so a binding made outside any
  // pushed frame has somewhere to go.
  evaluator(diagnostic_bag &diag, file_id_type file_id)
      : diag_(diag), file_id_(file_id), locals_(1), lazy_locals_(1) {}

  /// Sets which file newly reported diagnostics are attributed to.
  void set_file(file_id_type file_id) { file_id_ = file_id; }

  /// Runs `expr` on the VM. Returns `value::make_error()`, already
  /// reported, when it cannot run or fails.
  [[nodiscard]] auto evaluate(const ast::expr &expr) -> value;

  /// `evaluate`, silently: `nullopt` instead of a reported error. For a
  /// caller that only wants a value when one is available now, such as a
  /// `static if` whose condition may read a binding known only when the
  /// program runs.
  [[nodiscard]] auto try_evaluate(const ast::expr &expr)
      -> std::optional<value> {
    const auto saved = set_silent(true);
    auto result = evaluate(expr);
    set_silent(saved);
    if (result.is_error()) {
      return std::nullopt;
    }
    return result;
  }

  /// What happened when the VM was asked to run something.
  struct vm_call_outcome {
    enum class kind : std::uint8_t {
      /// The VM cannot run it; `message` says why, when known.
      not_run,
      /// It ran; `result` is its value.
      returned,
      /// It ran and failed; `message` says how and `notes` where.
      failed,
    };
    kind outcome = kind::not_run;
    value result;
    std::string message;
    std::vector<diagnostic> notes;
  };

  /// Runs a call to `fn` with `args` (in parameter order, every parameter
  /// supplied) on the VM. `type_args` binds a generic `fn`'s type
  /// parameters, by name, to `type_value`s; the VM runs the instance for
  /// them.
  using vm_call_fn = std::function<vm_call_outcome(
      const ast::func_decl &fn, std::span<const value> args,
      std::span<const std::pair<std::string, value>> type_args)>;

  /// Runs a whole expression on the VM, reading the bindings it uses from
  /// `local_value`. A `failed` outcome's `message` is the whole error.
  using vm_eval_fn = std::function<vm_call_outcome(const ast::expr &expr)>;

  /// Installs the VM. Unset, compile-time code is reported as unable to
  /// run.
  void set_vm_call(vm_call_fn call) { vm_call_ = std::move(call); }
  void set_vm_eval(vm_eval_fn eval) { vm_eval_ = std::move(eval); }

  /// The value a binding held here has — a `static for` loop variable, a
  /// type parameter, an evaluated `let` — or `nullptr` if none of that name
  /// is visible.
  [[nodiscard]] auto local_value(const std::string &name) -> const value * {
    return lookup_local(name);
  }

  /// The type the current call's type parameter `name` is bound to, or
  /// `nullptr` when `name` is not a type parameter bound in this frame.
  [[nodiscard]] auto type_binding(const std::string &name) -> const value * {
    const auto *found = lookup_local(name);
    return found != nullptr && found->kind == value_kind::type_value ? found
                                                                     : nullptr;
  }

  /// The builder quoted syntax comes from, shared with compile-time code
  /// the VM runs.
  [[nodiscard]] auto fragments() -> fragment_builder & { return fragments_; }

  /// Moves out every fragment `fragments_` built, so it outlives this
  /// evaluator: lowering reads spliced fragments after checking ends.
  [[nodiscard]] auto take_synthesized_fragments() -> ast::ptr_vec<ast::node> {
    return fragments_.take_fragments();
  }

  /// Whether failures are silent (`try_evaluate`).
  [[nodiscard]] auto silent() const -> bool { return diagnostics_suppressed_; }

  /// Makes failures silent (`on`) or not, returning whether they were, so a
  /// caller can restore that.
  auto set_silent(bool on) -> bool {
    return std::exchange(diagnostics_suppressed_, on);
  }

  /// Pushes or pops one frame of bindings, such as one `static for`
  /// iteration's loop variables.
  void push_locals(std::unordered_map<std::string, value> scope);
  void pop_locals();

  /// Binds `name` in the innermost frame, replacing any binding of that
  /// name there.
  void bind_local(const std::string &name, value v);

  /// Binds an ordinary immutable `let` in the current frame without
  /// evaluating it. The first compile-time expression that reads `name`
  /// evaluates `initializer`, silently; if that fails, `name` has no
  /// compile-time value, as for any binding known only at run time.
  /// `initializer` must outlive the frame.
  void bind_lazy_local(const std::string &name, const ast::expr &initializer);

  /// Calls `fn` with no value arguments and `type_args` bound to its type
  /// parameters, as the call `fn[types]()` would.
  [[nodiscard]] auto
  call_with_types(const ast::func_decl &fn, source_span span,
                  std::vector<std::pair<std::string, value>> type_args)
      -> value {
    return call_function(fn, {}, span, type_args);
  }

  /// Runs `call`, a call to the `static def` `fn` from ordinary code,
  /// silently: its arguments and then the call. `nullopt` when an argument
  /// is not a compile-time value or the call cannot run, so the caller can
  /// leave it as an ordinary call. `type_args` binds `fn`'s type
  /// parameters to the types this call solved them to.
  [[nodiscard]] auto try_eval_ordinary_call(
      const ast::func_decl &fn, const ast::call_expr &call,
      std::vector<std::pair<std::string, value>> type_args = {})
      -> std::optional<value>;

  /// The values a `static for` iterates over: an integer range (`a..b`,
  /// `a..=b`) or a list.
  [[nodiscard]] auto evaluate_iterable(const ast::expr &iterable) -> value;

  /// Binds a `static for` loop pattern against `v` into `scope`: a name, a
  /// wildcard, or a tuple or struct pattern taking `v` apart. Reports and
  /// returns `false` when `v` does not have the pattern's shape.
  [[nodiscard]] auto bind_pattern(const ast::pattern &pattern, const value &v,
                                  std::unordered_map<std::string, value> &scope)
      -> bool;

private:
  /// Runs `fn` on the VM, reporting at `span` when it cannot run or fails.
  [[nodiscard]] auto
  call_function(const ast::func_decl &fn, std::span<const value> args,
                source_span span,
                std::span<const std::pair<std::string, value>> type_args)
      -> value;

  /// The value `outcome` holds, or the error it reports at `span`. `what`
  /// names what was run.
  [[nodiscard]] auto use_vm_outcome(vm_call_outcome outcome,
                                    std::string_view what, source_span span)
      -> value;

  /// Looks up `name` in the frames, innermost first, evaluating a lazy
  /// `let` on the way.
  [[nodiscard]] auto lookup_local(const std::string &name) -> const value *;

  /// Reports an error at `span` unless silent, and returns the error
  /// sentinel.
  auto report(source_span span, std::string message,
              std::vector<diagnostic> notes = {}) -> value;

  diagnostic_bag &diag_;
  file_id_type file_id_;

  vm_call_fn vm_call_;
  vm_eval_fn vm_eval_;

  std::vector<std::unordered_map<std::string, value>> locals_;
  /// A `let` bound by `bind_lazy_local` and not yet read.
  struct lazy_local {
    const ast::expr *initializer = nullptr;
    /// Set while its initializer runs, so a cycle reads no value.
    bool running = false;
  };
  /// One map per frame of `locals_`, in step with it. A name is in at most
  /// one of a frame's two maps.
  std::vector<std::unordered_map<std::string, lazy_local>> lazy_locals_;
  /// Evaluates the lazy `let` `name` in frame `frame` and moves it into
  /// `locals_`, returning its value or `nullptr` if it has none.
  auto force_lazy_local(size_t frame, const std::string &name)
      -> const value *;
  /// Before `name` is rebound, evaluates every visible lazy `let` whose
  /// initializer reads it, so each sees the binding it was written against.
  void force_lazy_readers(const std::string &name);

  /// Builds every fragment quotes and the `expr.*` builders evaluate to,
  /// and owns them (`take_synthesized_fragments`).
  fragment_builder fragments_;

  /// While set, `report` emits nothing.
  bool diagnostics_suppressed_ = false;
};

} // namespace cinder::comptime
