#pragma once

#include "check.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "src/comptime/eval.h"
#include "src/intrinsics.h"
#include "src/parser/ast_clone.h"
#include "src/parser/text_escape.h"
#include "src/semantic/binding_walk.h"
#include "src/semantic/infer/blame.h"
#include "src/semantic/infer/infer_ctxt.h"
#include "src/semantic/infer/obligations.h"
#include "src/semantic/infer/rigid_match.h"
#include "src/semantic/infer/unify.h"
#include "src/semantic/module_index.h"
#include "src/semantic/predicates.h"
#include "src/semantic/reason.h"
#include "src/semantic/resolution.h"
#include "src/semantic/types.h"

namespace cinder::semantic::detail {

auto is_variant_ident(const ast::ident_expr &ident) -> bool;

[[nodiscard]] auto builtin_method_owner(const type_entry &object)
    -> std::string_view;

} // namespace cinder::semantic::detail
