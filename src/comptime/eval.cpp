#include "src/comptime/eval.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "src/parser/ast_traversal.h"

namespace cinder::comptime {

void evaluator::push_locals(std::unordered_map<std::string, value> scope) {
  locals_.push_back(std::move(scope));
  lazy_locals_.emplace_back();
}

void evaluator::pop_locals() {
  locals_.pop_back();
  lazy_locals_.pop_back();
}

void evaluator::bind_local(const std::string &name, value v) {
  force_lazy_readers(name);
  lazy_locals_.back().erase(name);
  locals_.back().insert_or_assign(name, std::move(v));
}

void evaluator::bind_lazy_local(const std::string &name,
                                const ast::expr &initializer) {
  force_lazy_readers(name);
  locals_.back().erase(name);
  lazy_locals_.back().insert_or_assign(
      name, lazy_local{.initializer = &initializer, .running = false});
}

auto evaluator::lookup_local(const std::string &name) -> const value * {
  for (auto frame = locals_.size(); frame-- > 0;) {
    if (const auto found = locals_[frame].find(name);
        found != locals_[frame].end()) {
      return &found->second;
    }
    if (lazy_locals_[frame].contains(name)) {
      return force_lazy_local(frame, name);
    }
  }
  return nullptr;
}

auto evaluator::force_lazy_local(size_t frame, const std::string &name)
    -> const value * {
  auto &entry = lazy_locals_[frame].at(name);
  if (entry.running) {
    return nullptr;
  }
  entry.running = true;
  const auto &initializer = *entry.initializer;
  const auto saved_silent = set_silent(true);
  auto result = evaluate(initializer);
  set_silent(saved_silent);
  lazy_locals_[frame].erase(name);
  if (result.is_error()) {
    return nullptr;
  }
  return &locals_[frame]
              .insert_or_assign(name, std::move(result))
              .first->second;
}

void evaluator::force_lazy_readers(const std::string &name) {
  auto readers = std::vector<std::pair<size_t, std::string>>{};
  for (size_t frame = 0; frame < lazy_locals_.size(); ++frame) {
    for (const auto &[lazy_name, entry] : lazy_locals_[frame]) {
      auto reads = false;
      ast::walk(*entry.initializer, [&](const ast::node &node) -> bool {
        if (const auto *ident = ast::as<const ast::ident_expr>(&node)) {
          reads = reads || ident->name == name;
        } else if (const auto *path =
                       ast::as<const ast::module_path_expr>(&node)) {
          reads = reads ||
                  (!path->segments.empty() && path->segments.front() == name);
        }
        return !reads;
      });
      if (reads) {
        readers.emplace_back(frame, lazy_name);
      }
    }
  }
  for (const auto &[frame, reader] : readers) {
    if (lazy_locals_[frame].contains(reader)) {
      static_cast<void>(force_lazy_local(frame, reader));
    }
  }
}

auto evaluator::report(source_span span, std::string message,
                       std::vector<diagnostic> notes) -> value {
  if (diagnostics_suppressed_) {
    return value::make_error();
  }
  auto error = diagnostic(diagnostic_level::error, std::move(message), file_id_)
                   .with_label(span, "evaluated at compile time here");
  for (auto &note : notes) {
    error.children.push_back(std::move(note));
  }
  diag_.emit(std::move(error));
  return value::make_error();
}

auto evaluator::use_vm_outcome(vm_call_outcome outcome, std::string_view what,
                               source_span span) -> value {
  switch (outcome.outcome) {
  case vm_call_outcome::kind::returned:
    return std::move(outcome.result);
  case vm_call_outcome::kind::failed:
    return report(span, std::move(outcome.message), std::move(outcome.notes));
  case vm_call_outcome::kind::not_run:
    break;
  }
  return report(span, outcome.message.empty()
                          ? std::format("{} cannot run at compile time", what)
                          : std::format("{} cannot run at compile time: {}",
                                        what, outcome.message));
}

auto evaluator::evaluate(const ast::expr &expr) -> value {
  if (expr.has_error) {
    return value::make_error();
  }
  if (!vm_eval_) {
    return report(expr.span, "this expression cannot run at compile time: "
                             "no compile-time backend is installed");
  }
  return use_vm_outcome(vm_eval_(expr), "this expression", expr.span);
}

auto evaluator::call_function(
    const ast::func_decl &fn, std::span<const value> args, source_span span,
    std::span<const std::pair<std::string, value>> type_args) -> value {
  if (!vm_call_) {
    return report(span, std::format("the call to `{}` cannot run at compile "
                                    "time: no compile-time backend is "
                                    "installed",
                                    fn.name));
  }
  auto outcome = vm_call_(fn, args, type_args);
  if (outcome.outcome == vm_call_outcome::kind::failed) {
    outcome.message = std::format("compile-time call to `{}` failed: {}",
                                  fn.name, outcome.message);
  }
  return use_vm_outcome(std::move(outcome),
                        std::format("the call to `{}`", fn.name), span);
}

auto evaluator::try_eval_ordinary_call(
    const ast::func_decl &fn, const ast::call_expr &call,
    std::vector<std::pair<std::string, value>> type_args)
    -> std::optional<value> {
  const auto saved = set_silent(true);
  auto args = std::vector<value>{};
  args.reserve(call.args.size());
  auto ok = call.args.size() == fn.params.size();
  for (const auto &arg : call.args) {
    if (!ok || arg.name.has_value() || arg.value == nullptr) {
      ok = false;
      break;
    }
    auto evaluated = evaluate(*arg.value);
    if (evaluated.is_error()) {
      ok = false;
      break;
    }
    args.push_back(std::move(evaluated));
  }
  auto result =
      ok ? call_function(fn, args, call.span, type_args) : value::make_error();
  set_silent(saved);
  if (result.is_error()) {
    return std::nullopt;
  }
  return result;
}

auto evaluator::evaluate_iterable(const ast::expr &iterable) -> value {
  if (const auto *bin = ast::as<const ast::binary_expr>(&iterable);
      bin != nullptr && (bin->op == ast::binary_op::range ||
                         bin->op == ast::binary_op::range_inclusive)) {
    if (bin->lhs == nullptr || bin->rhs == nullptr) {
      return value::make_error();
    }
    auto lo = evaluate(*bin->lhs);
    if (lo.is_error()) {
      return lo;
    }
    auto hi = evaluate(*bin->rhs);
    if (hi.is_error()) {
      return hi;
    }
    if (lo.kind != value_kind::integer || hi.kind != value_kind::integer) {
      return report(iterable.span, "a compile-time range requires integer "
                                   "bounds");
    }
    const auto end = bin->op == ast::binary_op::range_inclusive ? hi.integer + 1
                                                                : hi.integer;
    auto elements = std::vector<value>{};
    for (auto i = lo.integer; i < end; ++i) {
      elements.push_back(value::make_int(i));
    }
    return value::make_list(std::move(elements));
  }
  auto evaluated = evaluate(iterable);
  if (evaluated.is_error()) {
    return evaluated;
  }
  if (evaluated.kind != value_kind::list) {
    return report(iterable.span, "a compile-time `for` requires a list "
                                 "or range value to iterate over");
  }
  return evaluated;
}

auto evaluator::bind_pattern(const ast::pattern &pattern, const value &v,
                             std::unordered_map<std::string, value> &scope)
    -> bool {
  switch (pattern.kind) {
  case ast::node_kind::wildcard_pattern:
    return true;
  case ast::node_kind::binding_pattern:
    scope.insert_or_assign(ast::cast<const ast::binding_pattern>(pattern).name,
                           v);
    return true;
  case ast::node_kind::group_pattern: {
    const auto &group = ast::cast<const ast::group_pattern>(pattern);
    if (group.inner == nullptr || !bind_pattern(*group.inner, v, scope)) {
      return false;
    }
    if (group.alias.has_value()) {
      scope.insert_or_assign(*group.alias, v);
    }
    return true;
  }
  case ast::node_kind::tuple_pattern: {
    const auto &tup = ast::cast<const ast::tuple_pattern>(pattern);
    if (v.kind != value_kind::list ||
        v.elements.size() != tup.elements.size()) {
      report(pattern.span, "this tuple pattern does not match the shape of "
                           "the value it takes apart");
      return false;
    }
    for (size_t i = 0; i < tup.elements.size(); ++i) {
      if (tup.elements[i] == nullptr ||
          !bind_pattern(*tup.elements[i], v.elements[i], scope)) {
        return false;
      }
    }
    return true;
  }
  case ast::node_kind::struct_pattern: {
    const auto &st = ast::cast<const ast::struct_pattern>(pattern);
    if (v.kind != value_kind::struct_instance) {
      report(pattern.span, "this struct pattern requires a struct value");
      return false;
    }
    for (const auto &field : st.fields) {
      if (field.is_rest) {
        continue;
      }
      const auto it = v.fields.find(field.name);
      if (it == v.fields.end()) {
        report(field.span,
               std::format("this struct value has no field `{}`", field.name));
        return false;
      }
      if (field.pattern != nullptr) {
        if (!bind_pattern(*field.pattern, it->second, scope)) {
          return false;
        }
      } else {
        scope.insert_or_assign(field.name, it->second);
      }
    }
    return true;
  }
  default:
    report(pattern.span, "a `static for` loop variable must be a name, `_`, "
                         "or a tuple or struct pattern that always matches");
    return false;
  }
}

} // namespace cinder::comptime
