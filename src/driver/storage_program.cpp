#include "storage_program.h"

#include <cstdlib>
#include <string_view>


namespace cinder::driver {

auto storage_form_enabled() -> bool {
  // The storage form is the default; `CINDER_STORAGE_FORM=0` selects the
  // legacy pointer form while it remains.
  const auto *flag = std::getenv("CINDER_STORAGE_FORM");
  return flag == nullptr || std::string_view{flag} != "0";
}

} // namespace cinder::driver
