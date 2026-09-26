#include "src/hir/frame_budget.h"

#include <format>
#include <utility>

#include "src/hir/traversal.h"

namespace cinder::hir {

namespace {

/// Adds `node`'s buffers to `frame`, stopping at lambda bodies — those are
/// measured as frames of their own, queued on `lambdas`.
auto collect_buffers(hir_node &node, std::vector<frame_buffer> &frame,
                     std::vector<hir_lambda *> &lambdas) -> void {
  if (node.kind == hir_node_kind::hir_lambda) {
    lambdas.push_back(&dynamic_cast<hir_lambda &>(node));
    return;
  }
  if (node.kind == hir_node_kind::hir_stack_buffer) {
    const auto &buffer = dynamic_cast<const hir_stack_buffer &>(node);
    frame.push_back(
        frame_buffer{.span = buffer.span, .byte_size = buffer.byte_size});
  }
  for_each_child(node, [&](auto &slot) -> void {
    collect_buffers(*slot, frame, lambdas);
  });
}

auto check_frame(const std::string &name, source_span span, hir_node &body,
                 std::vector<frame_budget_violation> &out) -> void {
  auto buffers = std::vector<frame_buffer>{};
  auto lambdas = std::vector<hir_lambda *>{};
  collect_buffers(body, buffers, lambdas);

  auto total = uint64_t{0};
  for (const auto &buffer : buffers) {
    total += buffer.byte_size;
  }
  if (total > k_max_frame_stack_bytes) {
    out.push_back(frame_budget_violation{.frame_name = name,
                                         .frame_span = span,
                                         .total_bytes = total,
                                         .buffers = std::move(buffers)});
  }
  for (auto *lambda : lambdas) {
    if (lambda->body != nullptr) {
      check_frame(std::format("a lambda in {}", name), lambda->span,
                  *lambda->body, out);
    }
  }
}

} // namespace

auto find_frame_budget_violations(hir_module &module)
    -> std::vector<frame_budget_violation> {
  auto violations = std::vector<frame_budget_violation>{};
  for (auto &fn : module.functions) {
    if (fn->body != nullptr) {
      check_frame(std::format("`{}`", fn->name), fn->span, *fn->body,
                  violations);
    }
  }
  return violations;
}

} // namespace cinder::hir
