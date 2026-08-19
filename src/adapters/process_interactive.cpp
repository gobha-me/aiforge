#include <aiforge/adapters/process_draft_editor.hpp>
#include <aiforge/adapters/process_interactive.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/adapters/termforge_run_bridge.hpp>
#include <aiforge/adapters/transcript_view.hpp>
#include <aiforge/adapters/venice_backend.hpp>
#include <aiforge/config/config.hpp>
#include <aiforge/config/file_store.hpp>
#include <aiforge/surfaces/chat_session.hpp>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <termforge/core/app.hpp>
#include <termforge/widgets/composer.hpp>
#include <termforge/widgets/focus_ring.hpp>
#include <utility>
#include <variant>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto failure(const cli::CommandFailureKind kind,
                           std::string message)
    -> std::unexpected<cli::CommandFailure> {
  return std::unexpected(cli::CommandFailure{kind, std::move(message)});
}

auto warning(std::ostream& stream, const std::string_view message) -> bool {
  try {
    stream << "aiforge: warning: " << message << '\n';
    return static_cast<bool>(stream);
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto load_config(std::ostream& diagnostics)
    -> std::expected<config::ResolvedConfig, cli::CommandFailure> {
  const auto& registry = config::builtin_config_registry();
  std::vector<config::ConfigLayer> layers;
  auto environment = config::environment_config_layer(registry);
  if (!environment) {
    return failure(cli::CommandFailureKind::runtime,
                   "configuration environment could not be read");
  }
  layers.push_back(std::move(*environment));
  auto path = config::process_config_path();
  if (!path) {
    if (!warning(diagnostics, path.error().message)) {
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
    }
  } else {
    auto file = config::JsonConfigFileStore{*path}.load(registry);
    if (file) {
      layers.push_back(std::move(*file));
    } else if (!warning(diagnostics, file.error().message)) {
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
    }
  }
  auto resolved = config::resolve_config(registry, layers);
  if (!resolved) {
    return failure(cli::CommandFailureKind::runtime,
                   "configuration could not be resolved");
  }
  for (const auto& diagnostic : resolved->diagnostics) {
    if (!warning(diagnostics, diagnostic.message)) {
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
    }
  }
  return std::move(*resolved);
}

[[nodiscard]] auto configured_model(const config::ResolvedConfig& resolved)
    -> std::expected<domain::ModelId, cli::CommandFailure> {
  const auto* entry = resolved.find("model");
  if (entry == nullptr || !entry->value) {
    return failure(
        cli::CommandFailureKind::runtime,
        "model is not configured; set AIFORGE_MODEL or config model");
  }
  const auto* text = std::get_if<std::string>(&*entry->value);
  if (text == nullptr || text->empty()) {
    return failure(cli::CommandFailureKind::runtime,
                   "configured model is invalid");
  }
  auto model = domain::ModelId::from(*text);
  if (!model) {
    return failure(cli::CommandFailureKind::runtime,
                   "configured model is invalid");
  }
  return std::move(*model);
}

[[nodiscard]] auto session_error(const surfaces::ChatSessionError& value)
    -> cli::CommandFailure {
  switch (value.code) {
    case surfaces::ChatSessionErrorCode::invalid_input:
    case surfaces::ChatSessionErrorCode::input_too_large:
      return {cli::CommandFailureKind::usage, value.message};
    case surfaces::ChatSessionErrorCode::cancelled:
      return {cli::CommandFailureKind::cancelled, value.message};
    default:
      return {cli::CommandFailureKind::runtime, value.message};
  }
}

class ChatApp final : public termforge::App {
 public:
  ChatApp(backend::Backend& backend,
          backend::ModelContextProvider& model_context,
          storage::SessionStore* session_store, surfaces::ChatSessionOpen open,
          surfaces::DraftEditor& editor, const std::stop_token stop_token)
      : m_bridge(*this), m_editor(editor), m_stop_token(stop_token) {
    set_frame_ms(33);
    m_composer.set_max_height(8);
    m_focus.add(&m_composer);
    auto session =
        surfaces::ChatSession::open(std::move(open), backend, model_context,
                                    session_store, &m_bridge, stop_token);
    if (!session) {
      m_setup_error = session_error(session.error());
      return;
    }
    m_session = std::move(*session);
    auto rebuilt = m_transcript.rebuild(m_session->event_log().events());
    if (!rebuilt) {
      m_setup_error =
          cli::CommandFailure{cli::CommandFailureKind::runtime,
                              "interactive transcript replay failed"};
      m_session.reset();
      return;
    }
    sync_history();
    m_status = "Ready";
  }

  [[nodiscard]] auto ready() const noexcept -> bool {
    return m_session != nullptr && !m_setup_error.has_value();
  }

  [[nodiscard]] auto setup_error() const -> cli::CommandFailure {
    return m_setup_error.value_or(cli::CommandFailure{
        cli::CommandFailureKind::runtime, "interactive session setup failed"});
  }

  [[nodiscard]] auto pending_edit() const noexcept -> bool {
    return m_pending_edit;
  }

  auto perform_edit() -> void {
    m_pending_edit = false;
    const auto original = m_composer.text();
    auto edited = m_editor.edit(original, m_stop_token);
    if (!edited) {
      m_status = edited.error().message;
      return;
    }
    m_composer.set_text(std::move(*edited));
    m_status = "Draft updated by editor";
  }

  [[nodiscard]] auto failure_state() const
      -> std::optional<cli::CommandFailure> {
    return m_failure;
  }

  auto on_event(const termforge::Event& event) -> void override {
    if (!m_session) return;
    auto bridged = m_bridge.handle(event, *m_session);
    if (!bridged) {
      fail(session_error(bridged.error()));
      return;
    }
    if (!apply_events(*bridged)) return;

    if (const auto* mouse = std::get_if<termforge::MouseEvent>(&event)) {
      if (mouse->pressed && !m_session->active()) {
        static_cast<void>(m_focus.focus_at(mouse->x, mouse->y));
      }
      static_cast<void>(m_transcript.on_event(event));
      if (!m_session->active()) {
        route_mouse(*mouse, {&m_composer});
      }
      return;
    }

    if (const auto* key = std::get_if<termforge::KeyEvent>(&event)) {
      if (key->action != termforge::KeyAction::Press) return;
      if (key->key == termforge::Key::Escape && m_session->active()) return;
      if (key->ctrl && key->key == termforge::Key::Char && key->ch == U'c') {
        if (m_session->active()) {
          auto cancelled = m_session->cancel_active("interrupt");
          if (!cancelled) fail(session_error(cancelled.error()));
        } else {
          quit();
        }
        return;
      }
      if (!m_session->active() && key->ctrl &&
          key->key == termforge::Key::Char && key->ch == U'e') {
        request_edit();
        return;
      }
      if (!m_session->active() && key->ctrl &&
          key->key == termforge::Key::Char && key->ch == U'h') {
        m_history_enabled = !m_history_enabled;
        sync_history();
        m_status = m_history_enabled ? "Prompt recall enabled"
                                     : "Prompt recall disabled";
        return;
      }
      if (!m_session->active() && key->ctrl &&
          key->key == termforge::Key::Char && key->ch == U'k') {
        m_history_cutoff = m_session->submitted_prompts().size();
        sync_history();
        m_status = "Prompt recall cleared; durable events retained";
        return;
      }
      if (key->key == termforge::Key::PageUp) {
        m_transcript.widget().scroll(-10);
        return;
      }
      if (key->key == termforge::Key::PageDown) {
        m_transcript.widget().scroll(10);
        return;
      }
    }

    if (!m_session->active() && m_focus.handle_key(event)) return;
    if (const auto* key = std::get_if<termforge::KeyEvent>(&event);
        key != nullptr && key->action == termforge::KeyAction::Press &&
        key->key == termforge::Key::Enter && !m_session->active()) {
      submit();
      return;
    }
    termforge::App::on_event(event);
  }

  auto on_tick(std::chrono::duration<double>) -> void override {
    if (!m_session) return;
    auto drained = m_session->drain();
    if (!drained) {
      fail(session_error(drained.error()));
      return;
    }
    if (!apply_events(*drained)) return;
    if (m_stop_token.stop_requested()) {
      if (m_session->active()) {
        auto cancelled = m_session->cancel_active("interrupt");
        if (!cancelled) fail(session_error(cancelled.error()));
      } else {
        quit();
      }
    }
  }

  auto on_render(termforge::Screen& screen) -> void override {
    screen.clear();
    const int columns = screen.cols();
    const int rows = screen.rows();
    if (columns <= 0 || rows <= 0 || !m_session) return;

    std::string header =
        "AIForge  session " + std::string{m_session->session_id().value()};
    if (!m_session->durable()) header += " (ephemeral)";
    screen.write_text(0, 0, header, termforge::theme::kFg,
                      termforge::Rgb{0x20, 0x20, 0x40});

    const int usable = std::max(0, rows - 2);
    const int composer_rows =
        std::min(usable, m_composer.preferred_height(columns));
    const int transcript_rows = usable - composer_rows;
    m_transcript.set_geometry({0, 1, columns, transcript_rows});
    m_composer.set_geometry({0, 1 + transcript_rows, columns, composer_rows});
    m_transcript.draw(screen);
    m_composer.draw(screen);

    std::string footer =
        m_session->active()
            ? "Running — Esc cancels"
            : "Enter submit | Shift/Alt+Enter newline | Ctrl+E or /edit editor";
    if (!m_status.empty()) footer += " | " + m_status;
    screen.write_text(0, rows - 1, footer, termforge::theme::kDim,
                      termforge::theme::kBg);
  }

 private:
  auto request_edit() -> void {
    m_pending_edit = true;
    quit();
  }

  auto submit() -> void {
    const std::string draft = m_composer.text();
    if (draft.empty()) {
      m_status = "Draft is empty";
      return;
    }
    if (draft == "/edit") {
      request_edit();
      return;
    }
    auto submitted = m_session->submit(draft);
    if (!submitted) {
      m_status = submitted.error().message;
      return;
    }
    if (!apply_events(submitted->committed_events)) return;
    m_composer.clear();
    sync_history();
    m_transcript.widget().scroll_to_bottom();
    m_status = "Running";
  }

  auto sync_history() -> void {
    if (!m_session) return;
    m_composer.clear_history();
    if (m_history_enabled) {
      auto prompts = m_session->submitted_prompts();
      const auto begin = std::min(m_history_cutoff, prompts.size());
      for (std::size_t index = begin; index < prompts.size(); ++index) {
        m_composer.push_history(std::move(prompts[index]));
      }
    }
  }

  [[nodiscard]] auto apply_events(const std::vector<domain::RunEvent>& events)
      -> bool {
    for (const auto& event : events) {
      auto applied = m_transcript.apply(event);
      if (!applied) {
        fail({cli::CommandFailureKind::runtime,
              "interactive transcript update failed"});
        return false;
      }
      if (std::holds_alternative<domain::RunCompleted>(event.payload)) {
        m_status = "Ready";
      } else if (std::holds_alternative<domain::RunCancelled>(event.payload)) {
        m_status = "Cancelled";
      } else if (const auto* failed =
                     std::get_if<domain::RunFailed>(&event.payload)) {
        m_status = failed->error.message;
      }
    }
    return true;
  }

  auto fail(cli::CommandFailure value) -> void {
    if (!m_failure) m_failure = std::move(value);
    quit();
  }

  TermForgeRunBridge m_bridge;
  surfaces::DraftEditor& m_editor;
  std::stop_token m_stop_token;
  TranscriptView m_transcript;
  termforge::Composer m_composer;
  termforge::FocusRing m_focus;
  std::unique_ptr<surfaces::ChatSession> m_session;
  std::optional<cli::CommandFailure> m_setup_error;
  std::optional<cli::CommandFailure> m_failure;
  std::string m_status;
  bool m_pending_edit{};
  bool m_history_enabled{true};
  std::size_t m_history_cutoff{};
};

}  // namespace

auto ProcessInteractiveCommand::execute(Request request,
                                        cli::CommandEnvironment& environment,
                                        std::ostream& output,
                                        std::ostream& diagnostics)
    -> std::expected<void, cli::CommandFailure> {
  try {
    static_cast<void>(output);
    if (!environment.input_is_terminal || !environment.output_is_terminal) {
      return failure(cli::CommandFailureKind::usage,
                     "interactive chat requires terminal input and output");
    }
    auto resolved = load_config(diagnostics);
    if (!resolved) return std::unexpected(std::move(resolved.error()));
    auto model = configured_model(*resolved);
    if (!model) return std::unexpected(std::move(model.error()));

    const char* raw_key = std::getenv("VENICE_API_KEY");
    if (raw_key == nullptr || *raw_key == '\0') {
      return failure(cli::CommandFailureKind::runtime,
                     "VENICE_API_KEY is not configured");
    }
    const std::string api_key{raw_key};
    if (api_key.size() > 64U * 1024U) {
      return failure(cli::CommandFailureKind::runtime,
                     "VENICE_API_KEY is invalid");
    }

    VeniceBackendOptions options;
    options.api_key = api_key;
    VeniceBackend backend{std::move(options)};
    ProcessDraftEditor editor;
    const auto mode = [&] {
      switch (request.session_mode) {
        case SessionMode::create:
          return surfaces::ChatSessionOpen::Mode::create;
        case SessionMode::resume:
          return surfaces::ChatSessionOpen::Mode::resume;
        case SessionMode::continue_latest:
          return surfaces::ChatSessionOpen::Mode::continue_latest;
        case SessionMode::ephemeral:
          return surfaces::ChatSessionOpen::Mode::ephemeral;
      }
      return surfaces::ChatSessionOpen::Mode::create;
    }();
    surfaces::ChatSessionOpen open{std::move(*model), mode,
                                   std::move(request.session_id)};

    std::unique_ptr<SqliteSessionStore> store;
    if (mode != surfaces::ChatSessionOpen::Mode::ephemeral) {
      auto path = process_session_store_path();
      if (!path) {
        return failure(cli::CommandFailureKind::runtime,
                       "session storage path could not be resolved");
      }
      auto opened = SqliteSessionStore::open(*path);
      if (!opened) {
        return failure(cli::CommandFailureKind::runtime,
                       "session storage could not be opened");
      }
      store = std::move(*opened);
    }

    ChatApp app{backend,         backend, store.get(),
                std::move(open), editor,  environment.stop_token};
    if (!app.ready()) return std::unexpected(app.setup_error());
    for (;;) {
      const int result = app.run();
      if (app.failure_state()) {
        return std::unexpected(*app.failure_state());
      }
      if (!app.pending_edit()) {
        if (result != 0) {
          return failure(cli::CommandFailureKind::runtime,
                         "interactive terminal setup failed");
        }
        return {};
      }
      app.perform_edit();
      if (environment.stop_token.stop_requested()) {
        return failure(cli::CommandFailureKind::cancelled,
                       "interactive chat cancelled");
      }
    }
  } catch (...) {
    return failure(cli::CommandFailureKind::runtime,
                   "interactive chat failed internally");
  }
}

}  // namespace aiforge::adapters
