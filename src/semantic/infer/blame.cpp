#include "src/semantic/infer/blame.h"

#include <algorithm>
#include <format>
#include <utility>

namespace cinder::semantic::infer {

namespace {

/// How the cause wants a side described, falling back to its spelling.
///
/// A type's spelling is often not the explanation. `usize` says nothing about
/// *why* a length had to be one; "the length of `xs`" does. The cause carries
/// that phrasing because it was recorded where the constraint was created,
/// which is the only place that knew it.
auto describe(const type_table &table, type_id id, const std::string &desc)
    -> std::string {
  if (desc.empty()) {
    return std::format("`{}`", table.display(id));
  }
  return std::format("{} (`{}`)", desc, table.display(id));
}

/// The context stack, innermost first, as note lines.
auto context_notes(infer_ctxt &ctx, cause_id why) -> std::vector<blame_note> {
  auto notes = std::vector<blame_note>{};
  const auto chain = ctx.cause_chain(why);
  // The innermost frame is the primary site; the rest are the story of how
  // the checker got there.
  for (size_t i = 1; i < chain.size(); ++i) {
    const auto &frame = ctx.cause_at(chain[i]);
    if (frame.reason.empty()) {
      continue;
    }
    notes.push_back(
        blame_note{.where = frame.where,
                   .message = std::format("while checking {}", frame.reason)});
  }
  return notes;
}

} // namespace

constraint_graph::constraint_graph(infer_ctxt &ctx) : ctx_(&ctx) {}

auto constraint_graph::record(type_id left, type_id right, cause_id why)
    -> edge_id {
  const auto id = static_cast<edge_id>(edges_.size());
  edges_.push_back(constraint_edge{.left = left, .right = right, .why = why});
  return id;
}

auto constraint_graph::size() const -> size_t { return edges_.size(); }

auto constraint_graph::at(edge_id id) const -> const constraint_edge & {
  return edges_[id];
}

auto constraint_graph::demands_on(type_id var) -> std::vector<demand> {
  const auto root = ctx_->find(var);
  auto found = std::vector<demand>{};
  for (size_t i = 0; i < edges_.size(); ++i) {
    const auto &edge = edges_[i];
    // Which side *is* the variable is decided before zonking: once it is
    // solved, both sides zonk to the same thing and the question has no
    // answer. `find` is the right test because a merge does not change which
    // constraint mentioned which variable.
    const auto left_is = ctx_->find(edge.left) == root;
    const auto right_is = ctx_->find(edge.right) == root;
    if (left_is == right_is) {
      // Either it mentions neither side, or it is `var ~ var`, which asks
      // nothing of it.
      continue;
    }
    const auto other = left_is ? edge.right : edge.left;
    found.push_back(
        demand{.edge = static_cast<edge_id>(i), .required = ctx_->zonk(other)});
  }
  return found;
}

auto constraint_graph::outliers(type_id var) -> std::vector<demand> {
  const auto all = demands_on(var);
  if (all.size() < 2) {
    return {};
  }

  auto counts = std::vector<std::pair<type_id, size_t>>{};
  for (const auto &one : all) {
    const auto at = std::ranges::find_if(
        counts, [&](const auto &entry) { return entry.first == one.required; });
    if (at == counts.end()) {
      counts.emplace_back(one.required, 1);
    } else {
      ++at->second;
    }
  }
  if (counts.size() < 2) {
    return {}; // Everyone agrees. There is nothing to blame.
  }

  const auto most = std::ranges::max_element(
      counts, {}, [](const auto &entry) { return entry.second; });
  const auto majority = most->second;
  const auto tied = std::ranges::count_if(
      counts, [&](const auto &entry) { return entry.second == majority; });
  if (tied > 1) {
    // An even split. See the header: no winner is invented here.
    return {};
  }

  auto odd = std::vector<demand>{};
  for (const auto &one : all) {
    if (one.required != most->first) {
      odd.push_back(one);
    }
  }
  return odd;
}

auto explain_conflict(constraint_graph &graph, infer_ctxt &ctx,
                      const type_table &table, type_id var)
    -> std::optional<blame_report> {
  const auto all = graph.demands_on(var);
  if (all.size() < 2) {
    return std::nullopt;
  }
  const auto disagrees = std::ranges::any_of(all, [&](const auto &one) {
    return one.required != all.front().required;
  });
  if (!disagrees) {
    return std::nullopt;
  }

  const auto odd = graph.outliers(var);
  if (odd.empty()) {
    // A tie. Both sides are shown and neither is called the mistake, because
    // the constraints genuinely do not say which one is.
    const auto &first = graph.at(all.front().edge);
    const auto &second = graph.at(all.back().edge);
    const auto &first_why = ctx.cause_at(first.why);
    const auto &second_why = ctx.cause_at(second.why);

    auto report = blame_report{
        .primary = second_why.where,
        .headline = std::format(
            "{} and {} disagree, and nothing here says which is meant",
            describe(table, all.front().required, first_why.found_desc),
            describe(table, all.back().required, second_why.found_desc)),
        .label =
            std::format("this one is {}", describe(table, all.back().required,
                                                   second_why.found_desc)),
        .help = std::format(
            "Both are equally supported, so the compiler will not guess. "
            "Make them agree, or annotate {} to say which was intended.",
            second_why.reason.empty() ? "the binding" : second_why.reason),
        .notes = {}};
    report.notes.push_back(
        blame_note{.where = first_why.where,
                   .message = std::format("this one is {}",
                                          describe(table, all.front().required,
                                                   first_why.found_desc))});
    for (auto &note : context_notes(ctx, second.why)) {
      report.notes.push_back(std::move(note));
    }
    return report;
  }

  // There is a majority. It is the evidence, and the minority is the
  // mistake — so the caret goes on the outlier and the agreeing sites are
  // shown as the reason it is one.
  const auto &blamed_edge = graph.at(odd.front().edge);
  const auto &blamed_why = ctx.cause_at(blamed_edge.why);
  const auto expected = std::ranges::find_if(all, [&](const auto &one) {
                          return one.required != odd.front().required;
                        })->required;
  const auto agreeing = all.size() - odd.size();

  auto report = blame_report{
      .primary = blamed_why.where,
      .headline = std::format(
          "expected {}, found {}",
          describe(table, expected, blamed_why.expected_desc),
          describe(table, odd.front().required, blamed_why.found_desc)),
      .label =
          blamed_why.reason.empty()
              ? std::format("this is `{}`", table.display(odd.front().required))
              : std::format("{} is `{}`", blamed_why.reason,
                            table.display(odd.front().required)),
      .help = std::format(
          "{} other{} here require{} `{}`, so this is the one that does not "
          "fit. Change it to `{}`, or change the others if `{}` is what was "
          "meant.",
          agreeing, agreeing == 1 ? "" : "s", agreeing == 1 ? "s" : "",
          table.display(expected), table.display(expected),
          table.display(odd.front().required)),
      .notes = {}};

  for (const auto &one : all) {
    if (one.required != expected) {
      continue;
    }
    const auto &agree_why = ctx.cause_at(graph.at(one.edge).why);
    report.notes.push_back(blame_note{
        .where = agree_why.where,
        .message = std::format("this requires `{}`", table.display(expected))});
  }
  for (auto &note : context_notes(ctx, blamed_edge.why)) {
    report.notes.push_back(std::move(note));
  }
  return report;
}

auto explain_stall(const stalled_variable &stalled) -> blame_report {
  const auto named =
      stalled.origin.empty() ? std::string{"this type"} : stalled.origin;

  auto report = blame_report{
      .primary = stalled.where,
      .headline = std::format("nothing so far determines {}", named),
      .label = std::format("{} is still open here", named),
      .help = {},
      .notes = {}};

  // The two stalls are different mistakes and get different advice. With
  // facts in scope, the user wrote constraints that were not enough and
  // needs to know which; with none, nothing in the program mentions the
  // variable at all and an annotation is genuinely the only answer.
  if (stalled.facts.empty()) {
    report.help = std::format(
        "Nothing in this program says what {} is. Annotate it where it is "
        "introduced, or use it somewhere that would pin it down.",
        named);
  } else {
    report.help = std::format(
        "What is known about {} is listed below, and none of it is enough to "
        "choose. Add a constraint that distinguishes the remaining "
        "candidates, or annotate {} directly.",
        named, named);
    for (const auto &fact : stalled.facts) {
      report.notes.push_back(blame_note{
          .where = stalled.where, .message = std::format("known: {}", fact)});
    }
  }

  for (const auto &goal : stalled.blocked_goals) {
    report.notes.push_back(
        blame_note{.where = stalled.where,
                   .message = std::format("waiting on it: {}", goal)});
  }
  return report;
}

} // namespace cinder::semantic::infer
