#pragma once

#include <aiforge/surfaces/admin_controller.hpp>

namespace aiforge::surfaces {
// Application chrome only; never dispatched as a source or policy action.
struct AdminToolbarVisibility {
  bool visible{};
};
using AdminCommand = std::variant<AdminAction, AdminToolbarVisibility>;
// Null means another command. A recognized but invalid /admin is always an
// error, including a control character immediately after its command name.
[[nodiscard]] auto parse_admin_command(std::string_view text) noexcept
    -> std::expected<std::optional<AdminCommand>, ManualOpsFailure>;
} // namespace aiforge::surfaces
