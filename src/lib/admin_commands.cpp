#include <aiforge/detail/admin_input.hpp>
#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/surfaces/admin_commands.hpp>

namespace aiforge::surfaces {
namespace {
using Result = std::expected<std::optional<AdminCommand>, ManualOpsFailure>;
auto invalid() -> std::unexpected<ManualOpsFailure> {
  return std::unexpected(ManualOpsFailure{ManualOpsErrorCode::invalid_input});
}
auto word(std::string_view& text) -> std::string_view {
  const auto start = text.find_first_not_of(" \t");
  if (start == std::string_view::npos) {
    text = {};
    return {};
  }
  text.remove_prefix(start);
  const auto end = text.find_first_of(" \t");
  const auto value = text.substr(0, end);
  text.remove_prefix(value.size());
  return value;
}
auto without_argument(std::string_view action) -> Result {
  if (action.empty() || action == "targets")
    return AdminCommand{AdminAction{AdminInspect{}}};
  if (action == "health") return AdminCommand{AdminAction{AdminReadHealth{}}};
  if (action == "services")
    return AdminCommand{AdminAction{AdminReadServices{}}};
  if (action == "cancel") return AdminCommand{AdminAction{AdminCancel{}}};
  if (action == "close") return AdminCommand{AdminAction{AdminCloseView{}}};
  return invalid();
}
auto parse(std::string_view text) -> Result {
  if (!text.starts_with("/admin")) return std::nullopt;
  if (text.size() > 6 && text[6] != ' ' && text[6] != '\t') {
    const auto byte = static_cast<unsigned char>(text[6]);
    if (byte < 32 || byte == 127) return invalid();
    return std::nullopt;
  }
  if (text.size() > 1024 || !detail::is_safe_utf8_text(text) ||
      text.find_first_of("\r\n") != std::string_view::npos)
    return invalid();
  text.remove_prefix(6);
  const auto action = word(text);
  const auto argument = word(text);
  if (!word(text).empty()) return invalid();
  if (argument.empty()) return without_argument(action);
  if (action == "toolbar" && (argument == "show" || argument == "hide"))
    return AdminCommand{AdminToolbarVisibility{argument == "show"}};
  if (action == "service" && detail::valid_admin_service(argument))
    return AdminCommand{
        AdminAction{AdminReadNamedService{std::string{argument}}}};
  if (action == "select" && detail::valid_admin_target(argument)) {
    auto target = domain::OpsTargetId::from(std::string{argument});
    if (!target) return invalid();
    return AdminCommand{AdminAction{AdminSelectTarget{std::move(*target)}}};
  }
  return invalid();
}
} // namespace
auto parse_admin_command(std::string_view text) noexcept -> Result {
  try {
    return parse(text);
  } catch (...) {
    return std::unexpected(
        ManualOpsFailure{ManualOpsErrorCode::internal_failure});
  }
}
} // namespace aiforge::surfaces
