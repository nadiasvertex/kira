#include "src/bytecode/panic.h"

#include <string>

namespace kira::bytecode {

auto panic_reason_message(panic_reason reason) noexcept -> std::string_view {
  switch (reason) {
  case panic_reason::integer_overflow:
    return "integer overflow";
  case panic_reason::integer_divide_by_zero:
    return "divide by zero";
  case panic_reason::explicit_panic:
    return "explicit panic";
  case panic_reason::stack_overflow:
    return "stack overflow";
  case panic_reason::index_out_of_bounds:
    return "index out of bounds";
  }
  return "unknown panic";
}

panic_error::panic_error(panic_reason reason)
    : std::runtime_error(std::string(panic_reason_message(reason))),
      reason_(reason) {}

} // namespace kira::bytecode
