#pragma once

#include <aiforge/surfaces/admin_controller.hpp>
#include <string>

namespace aiforge::surfaces {
// Application chrome only; never dispatched as a source or policy action.
struct AdminToolbarVisibility {
  bool visible{};
};
// Presentation request resolved only against the currently displayed committed
// Pod-health proof. The controller continues to receive the same exact-source
// AdminEnableDisplayedLogs action used by buttons, menus and keys.
struct AdminEnableDisplayedContainerLogs {
  std::string container;
};
using AdminCommand = std::variant<AdminAction, AdminToolbarVisibility,
                                  AdminEnableDisplayedContainerLogs>;
// Null means another command. A recognized but invalid /admin is always an
// error, including a control character immediately after its command name.
[[nodiscard]] auto parse_admin_command(std::string_view text) noexcept
    -> std::expected<std::optional<AdminCommand>, ManualOpsFailure>;
} // namespace aiforge::surfaces
