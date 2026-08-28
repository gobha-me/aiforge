#pragma once

#include <aiforge/backend/backend.hpp>
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

namespace aiforge::adapters {

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
};

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
