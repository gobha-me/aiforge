#include <aiforge/bootstrap.hpp>

namespace aiforge {

auto bootstrap_status() noexcept -> std::string_view {
  return "AIForge core is available; interactive and network adapters are not "
         "enabled yet.";
}

} // namespace aiforge
