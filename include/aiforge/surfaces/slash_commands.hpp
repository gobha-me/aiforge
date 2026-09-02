#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aiforge::surfaces {

struct SlashCommandContext {
  bool run_active{};
  bool editor_available{true};
  std::stop_token stop_token;
  auto operator==(const SlashCommandContext&) const -> bool = default;
};

enum class SlashCommandAction {
  show_help,
  quit,
  clear_view,
  edit_draft,
  list_sessions,
  resume_session,
  new_session,
  list_personas,
  select_persona,
  disable_persona,
  choose_model,
  manage_request_settings,
  show_usage,
  show_plan,
  show_tasks,
  manage_memory,
};

struct SlashCommandResult {
  SlashCommandAction action{SlashCommandAction::show_help};
  std::optional<std::string> subject;
  auto operator==(const SlashCommandResult&) const -> bool = default;
};

enum class SlashCommandErrorCode {
  invalid_input,
  input_too_large,
  unknown_command,
  unavailable_command,
  invalid_arguments,
  cancelled,
  handler_failure,
  internal_failure,
};

struct SlashCommandError {
  SlashCommandErrorCode code{SlashCommandErrorCode::internal_failure};
  std::string message;
  auto operator==(const SlashCommandError&) const -> bool = default;
};

using SlashCommandAvailability = auto (*)(const SlashCommandContext&) -> bool;
using SlashCommandHandler = auto (*)(std::string_view,
                                     const SlashCommandContext&)
    -> std::expected<SlashCommandResult, SlashCommandError>;

struct SlashCommandSpec {
  std::string id;
  std::string name;
  std::string arguments;
  std::string help;
  SlashCommandAvailability available{};
  SlashCommandHandler handler{};
};

struct SlashCommandDescription {
  std::string id;
  std::string name;
  std::string arguments;
  std::string help;
  bool available{};
  auto operator==(const SlashCommandDescription&) const -> bool = default;
};

enum class SlashCommandRegistryErrorCode {
  invalid_id,
  invalid_name,
  invalid_metadata,
  duplicate_id,
  duplicate_name,
  missing_availability,
  missing_handler,
  internal_failure,
};

struct SlashCommandRegistryError {
  SlashCommandRegistryErrorCode code{
      SlashCommandRegistryErrorCode::internal_failure};
  std::string message;
  std::string command_id;
  auto operator==(const SlashCommandRegistryError&) const -> bool = default;
};

struct SlashCommandLimits {
  std::size_t maximum_input_bytes{64U * 1024U};
  std::size_t maximum_name_bytes{64U};
  auto operator==(const SlashCommandLimits&) const -> bool = default;
};

class SlashCommandRegistry final {
 public:
  [[nodiscard]] static auto create(std::vector<SlashCommandSpec> commands)
      -> std::expected<SlashCommandRegistry, SlashCommandRegistryError>;

  [[nodiscard]] auto dispatch(std::string_view input,
                              const SlashCommandContext& context = {},
                              SlashCommandLimits limits = {}) const noexcept
      -> std::expected<std::optional<SlashCommandResult>, SlashCommandError>;

  [[nodiscard]] auto describe(
      std::optional<std::string_view> name = std::nullopt,
      const SlashCommandContext& context = {}) const noexcept
      -> std::expected<std::vector<SlashCommandDescription>, SlashCommandError>;

  [[nodiscard]] auto complete(std::string_view prefix,
                              const SlashCommandContext& context = {},
                              SlashCommandLimits limits = {}) const noexcept
      -> std::expected<std::vector<std::string>, SlashCommandError>;

  [[nodiscard]] auto commands() const noexcept
      -> std::span<const SlashCommandSpec> {
    return m_commands;
  }

 private:
  explicit SlashCommandRegistry(std::vector<SlashCommandSpec> commands)
      : m_commands(std::move(commands)) {}

  std::vector<SlashCommandSpec> m_commands;
};

[[nodiscard]] auto builtin_slash_command_registry()
    -> const SlashCommandRegistry&;

} // namespace aiforge::surfaces
