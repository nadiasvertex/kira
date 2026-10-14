#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "src/comptime/value.h"
#include "src/parser/ast.h"
#include "src/parser/source_location.h"

namespace cinder::comptime {

/// Why building quoted syntax failed, and where.
struct fragment_error {
  source_span span;
  std::string message;
};

/// Builds quoted syntax for compile-time code: the value a quote evaluates
/// to, and the `expr.*` builders (`expr.lit`, `expr.field`, ...). Every
/// operation takes values already computed — a quote's splices, a builder's
/// arguments — which the compile-time VM passes in through host calls. Owns
/// every node it makes for the rest of the session.
class fragment_builder {
public:
  /// The splices nested in `quote` (`splice_expr`/`splice_type` nodes) that
  /// evaluating it resolves, in the order `quote` takes their values. Empty
  /// when it resolves none: then its splices, if any, stay syntax, and are
  /// resolved where the fragment is itself spliced.
  ///
  /// Narrow on purpose: only the shapes a generated `impl` needs (an
  /// `impl`, its methods, `return`, binary operators and types) are looked
  /// into. A splice anywhere else is left for the splice site.
  [[nodiscard]] static auto eager_splices(const ast::quote_expr &quote)
      -> std::vector<const ast::node *>;

  /// The value `quote` evaluates to, given the values of its
  /// `eager_splices` in order: its body, with each of those splices
  /// replaced by its value's syntax. Internal `let`/`var` bindings of an
  /// `expr` or `stmt` quote are renamed first (`rename_internal_bindings`),
  /// so splicing it cannot capture a name at the splice site.
  [[nodiscard]] auto quote(const ast::quote_expr &quote,
                           std::span<const value> splices)
      -> std::expected<value, fragment_error>;

  /// Whether `name` is an `expr.*` builder (`lit`, `ident`, ...).
  [[nodiscard]] static auto is_builder(std::string_view name) -> bool;

  /// `expr.<name>(args...)`: builds a new fragment from `args`. `span` is
  /// the call's and `arg_spans` its arguments', for errors.
  [[nodiscard]] auto build(std::string_view name, std::span<const value> args,
                           std::span<const source_span> arg_spans,
                           source_span span)
      -> std::expected<value, fragment_error>;

  /// Moves out every node built so far, so it outlives the builder —
  /// `checked_types::synthesized_fragments` keeps them for lowering.
  [[nodiscard]] auto take_fragments() -> ast::ptr_vec<ast::node>;

private:
  /// Deep-clones an expression fragment so it can become an owned child of
  /// a new node. Only the shapes the builders produce or a simple quoted
  /// expression holds are supported; `nullptr` for anything else.
  [[nodiscard]] auto clone_expr(const ast::node &node) -> ast::ptr<ast::expr>;

  /// Deep-clones a pattern built by `expr.ctor_pattern`; `nullptr` for any
  /// other shape.
  [[nodiscard]] auto clone_pattern(const ast::node &node)
      -> ast::ptr<ast::pattern>;

  /// Deep-clones one interpolation segment; `nullopt` if its value cannot be
  /// cloned. A dynamic width or precision is dropped.
  [[nodiscard]] auto clone_segment(const ast::interp_segment &segment)
      -> std::optional<ast::interp_segment>;

  /// Appends the segments `fragment` contributes to a string being built by
  /// `expr.interp_concat`: a string literal's text, an interpolated
  /// string's own segments, or one value segment for anything else.
  [[nodiscard]] auto append_segments(const ast::node &fragment,
                                     source_span span,
                                     std::vector<ast::interp_segment> &out)
      -> std::expected<void, fragment_error>;

  /// Clones `node`, replacing each eager splice with the next of `splices`
  /// (`next` counts how many are used).
  [[nodiscard]] auto materialize(const ast::node &node,
                                 std::span<const value> splices, size_t &next)
      -> std::expected<ast::ptr<ast::node>, fragment_error>;

  /// Keeps `node` for the session and returns it as a value of `kind`.
  [[nodiscard]] auto keep(ast::ptr<ast::node> node, value_kind kind) -> value;

  ast::ptr_vec<ast::node> fragments_;
  /// Feeds `rename_internal_bindings`' fresh names — session-wide, so two
  /// quotes spliced into one scope never mint the same name.
  std::uint64_t hygiene_next_id_ = 0;
  /// Quote bodies already renamed, so a quote evaluated more than once (one
  /// inside a loop) is not renamed again.
  std::unordered_set<const ast::node *> hygiene_renamed_;
};

} // namespace cinder::comptime
