#pragma once

#include <aiforge/adapters/venice_generation_options.hpp>
#include <aiforge/backend/backend.hpp>
#include <aiforge/backend/provider_character_catalog.hpp>
#include <aiforge/cli/command_registry.hpp>
#include <aiforge/domain/events.hpp>
#include <aiforge/model/catalog.hpp>
#include <aiforge/storage/session_store.hpp>
#include <aiforge/surfaces/chat_session.hpp>
#include <aiforge/surfaces/draft_editor.hpp>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <termforge/core/app.hpp>
#include <termforge/core/byte_sink.hpp>
#include <variant>

namespace aiforge::adapters {

struct VeniceRequestSettingSave {
  std::optional<VeniceWebSearchSetting> web_search;
  std::optional<VeniceSystemPromptSetting> system_prompt;
};

struct VenicePreparedPersistedSettings {
  VeniceConfiguredRequestSettings configured;
  domain::ConfigurationProvenanceEntry configuration_provenance;
};

using PreviewVeniceRequestSetting = std::function<
    auto(const VeniceRequestSettingSave&)
        ->std::expected<VenicePreparedPersistedSettings, std::string>>;
using PersistVeniceRequestSetting = std::function<
    auto(const VeniceRequestSettingSave&)->std::expected<void, std::string>>;

using ToolProfileMaximumSubject =
    std::variant<domain::ModelId, domain::PersonaId>;

struct ToolProfileMaximumSave {
  ToolProfileMaximumSubject subject;
  std::optional<domain::ToolProfileId> maximum_profile_id;
};

struct ToolProfileMaximumPersistError {
  std::string message;
  bool effect_may_have_applied{};
};

using PersistToolProfileMaximum =
    std::function<auto(const ToolProfileMaximumSave&)
                      ->std::expected<void, ToolProfileMaximumPersistError>>;

struct UserGlobalInstructionEnablePersistError {
  std::string message;
  bool effect_may_have_applied{};
};

struct UserGlobalInstructionEnablePreview {
  bool effective_enabled{true};
  domain::ConfigurationProvenanceEntry configuration_provenance;
};

using PreviewUserGlobalInstructionEnabled = std::function<
    auto(bool)->std::expected<UserGlobalInstructionEnablePreview, std::string>>;
using PersistUserGlobalInstructionEnabled = std::function<
    auto(bool)->std::expected<void, UserGlobalInstructionEnablePersistError>>;

struct InteractiveChatAppOptions {
  surfaces::ChatSessionDependencies session_dependencies{};
  termforge::ByteSink* rendered_output{};
  std::function<void(const termforge::Screen&)> rendered_frame;
  // Runs on the worker thread after its update is queued and any enabled live
  // event-ready marker has been posted. It must not touch the app or widgets.
  std::function<void()> wake_observer;
  bool live_wake_enabled{true};
  bool poll_worker_updates{true};
  model::CatalogService* model_catalog{};
  backend::ProviderCharacterCatalogSource* provider_character_catalog{};
  VeniceConfiguredRequestSettings configured_request_settings;
  PreviewVeniceRequestSetting preview_request_setting;
  PersistVeniceRequestSetting persist_request_setting;
  PersistToolProfileMaximum persist_tool_profile_maximum;
  std::string user_global_instruction_path;
  bool user_global_instructions_enabled{};
  PreviewUserGlobalInstructionEnabled preview_user_global_instruction_enabled;
  PersistUserGlobalInstructionEnabled persist_user_global_instruction_enabled;
};

struct InteractiveModelPickerAppOptions {
  termforge::ByteSink* rendered_output{};
  std::function<void(const termforge::Screen&)> rendered_frame;
};

class InteractiveModelPickerApp : public termforge::App {
 public:
  ~InteractiveModelPickerApp() override = default;

  [[nodiscard]] virtual auto selected_model() const
      -> std::optional<domain::ModelId> = 0;
  [[nodiscard]] virtual auto cancelled() const noexcept -> bool = 0;
  [[nodiscard]] virtual auto status_text() const noexcept
      -> std::string_view = 0;
  [[nodiscard]] virtual auto configure_terminal_for_scenario(
      termforge::TerminalIo io, const termforge::Capabilities& capabilities)
      -> std::expected<void, std::string> = 0;
};

[[nodiscard]] auto validate_interactive_model_selection(
    const model::CatalogSnapshot& snapshot, const domain::ModelId& selected)
    -> std::expected<void, std::string>;

[[nodiscard]] auto make_interactive_model_picker_app(
    const model::CatalogSnapshot& snapshot, std::stop_token stop_token = {},
    InteractiveModelPickerAppOptions options = {})
    -> std::unique_ptr<InteractiveModelPickerApp>;

class InteractiveChatApp : public termforge::App {
 public:
  ~InteractiveChatApp() override = default;

  [[nodiscard]] virtual auto ready() const noexcept -> bool = 0;
  [[nodiscard]] virtual auto setup_error() const -> cli::CommandFailure = 0;
  [[nodiscard]] virtual auto pending_edit() const noexcept -> bool = 0;
  virtual auto perform_edit() -> void = 0;
  [[nodiscard]] virtual auto failure_state() const
      -> std::optional<cli::CommandFailure> = 0;
  [[nodiscard]] virtual auto events() const noexcept
      -> std::span<const domain::RunEvent> = 0;
  [[nodiscard]] virtual auto status_text() const noexcept
      -> std::string_view = 0;
  [[nodiscard]] virtual auto configure_terminal_for_scenario(
      termforge::TerminalIo io, const termforge::Capabilities& capabilities)
      -> std::expected<void, std::string> = 0;
};

[[nodiscard]] auto make_interactive_chat_app(
    backend::Backend& backend, backend::ModelContextProvider& model_context,
    storage::SessionStore* session_store, surfaces::ChatSessionOpen open,
    surfaces::DraftEditor& editor, std::stop_token stop_token = {},
    InteractiveChatAppOptions options = {})
    -> std::unique_ptr<InteractiveChatApp>;

} // namespace aiforge::adapters
