#include <unistd.h>

#include <aiforge/adapters/interactive_chat_app.hpp>
#include <aiforge/model/catalog.hpp>
#include <aiforge/testing/tui_scenario.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <termforge/widgets/choice_wizard_dialog.hpp>
#include <thread>
#include <utility>
#include <vector>

using namespace aiforge;

namespace {

using namespace std::chrono_literals;

template <typename IdType> auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

template <typename Payload>
auto run_event(const std::uint64_t sequence, std::string event_id,
               std::string run_id, Payload payload) -> domain::RunEvent {
  return {{make_id<domain::EventId>(event_id), make_id<domain::RunId>(run_id),
           sequence, 1,
           domain::EventTimestamp{std::chrono::milliseconds{sequence}},
           std::nullopt, std::nullopt, std::nullopt},
          std::move(payload)};
}

auto reported_cost() -> domain::ReportedCost {
  return domain::ReportedCost::create(
             {domain::MonetaryAmount::create(
                  "USD", domain::DecimalAmount::from("0").value())
                  .value(),
              domain::MonetaryAmount::create(
                  "venice.diem",
                  domain::DecimalAmount::from("0.0645375").value())
                  .value()})
      .value();
}

auto pricing_observation() -> domain::PricingObservation {
  domain::TextPricing pricing;
  pricing.base.input =
      domain::PriceRate{domain::DecimalAmount::from("1").value(),
                        domain::DecimalAmount::from("1").value()};
  pricing.base.output =
      domain::PriceRate{domain::DecimalAmount::from("2").value(),
                        domain::DecimalAmount::from("2").value()};
  pricing.base.cache_input =
      domain::PriceRate{domain::DecimalAmount::from("0.5").value(),
                        domain::DecimalAmount::from("0.5").value()};
  return domain::make_pricing_observation(
             make_id<domain::ModelId>("model"), "test.models", std::nullopt,
             domain::EventTimestamp{123ms}, domain::PricingCatalogOrigin::live,
             std::move(pricing))
      .value();
}

auto normalized_screen(const termforge::Screen& screen) -> std::string {
  std::ostringstream normalized;
  std::vector<std::pair<int, int>> reverse_cells;
  normalized << screen.cols() << 'x' << screen.rows() << ':';
  for (int row{}; row < screen.rows(); ++row) {
    if (row != 0) normalized << '\n';
    std::string text;
    for (int column{}; column < screen.cols(); ++column) {
      const auto cell = screen.text_at(column, row);
      text += cell.empty() ? " " : std::string{cell};
      if (termforge::any(screen.at(column, row).attrs &
                         termforge::Attr::Reverse)) {
        reverse_cells.emplace_back(column, row);
      }
    }
    while (!text.empty() && text.back() == ' ')
      text.pop_back();
    normalized << text;
  }
  if (!reverse_cells.empty()) {
    normalized << "\n@reverse=";
    for (std::size_t index{}; index < reverse_cells.size(); ++index) {
      if (index != 0) normalized << ';';
      normalized << reverse_cells[index].first << ','
                 << reverse_cells[index].second;
    }
  }
  return std::move(normalized).str();
}

struct ProbeState {
  std::string events;
  std::string frame;
  int ticks{};
  int columns{};
  int rows{};
};

class ScenarioProbe final : public termforge::App {
 public:
  ScenarioProbe(std::shared_ptr<ProbeState> state, termforge::ByteSink* output)
      : m_state(std::move(state)), m_output(output) {
    if (::pipe(m_pipe) != 0 ||
        !terminal().set_io(termforge::TerminalIo{m_pipe[0], -1})) {
      if (m_pipe[0] >= 0) ::close(m_pipe[0]);
      if (m_pipe[1] >= 0) ::close(m_pipe[1]);
      m_pipe[0] = -1;
      m_pipe[1] = -1;
    }
    set_frame_ms(10);
    set_tick_hz(100);
    set_max_tick_dt(std::chrono::duration<double>::zero());
  }

  ~ScenarioProbe() override {
    for (const int fd : m_pipe) {
      if (fd >= 0) ::close(fd);
    }
  }

  [[nodiscard]] auto ready() const noexcept -> bool { return m_pipe[0] >= 0; }

  [[nodiscard]] auto configure(const termforge::Capabilities& capabilities)
      -> std::expected<void, std::string> {
    auto configured = terminal().set_capabilities(capabilities);
    if (!configured) return std::unexpected(configured.error().message);
    return {};
  }

  auto on_start() -> void override {
    if (m_output != nullptr) driver().set_output(m_output);
  }

  auto on_event(const termforge::Event& event) -> void override {
    if (const auto* key = std::get_if<termforge::KeyEvent>(&event)) {
      if (key->action == termforge::KeyAction::Press &&
          key->key == termforge::Key::Char) {
        m_state->events += static_cast<char>(key->ch);
        if (key->ch == U'q') quit();
      }
      return;
    }
    if (const auto* resized = std::get_if<termforge::ResizeEvent>(&event)) {
      m_state->columns = resized->cols;
      m_state->rows = resized->rows;
      m_state->events += 'R';
      return;
    }
    if (const auto* error = std::get_if<termforge::ErrorEvent>(&event)) {
      if (error->source == "fake.backend") m_state->events += 'B';
      if (error->source == "fake.tool") m_state->events += 'T';
    }
  }

  auto on_tick(std::chrono::duration<double>) -> void override {
    ++m_state->ticks;
  }

  auto on_render(termforge::Screen& screen) -> void override {
    screen.clear();
    screen.write_text(0, 0, m_state->events, {}, {});
    m_state->frame =
        normalized_screen(screen) + "#tick=" + std::to_string(m_state->ticks);
  }

 private:
  std::shared_ptr<ProbeState> m_state;
  termforge::ByteSink* m_output{};
  int m_pipe[2]{-1, -1};
};

class Pipe final {
 public:
  Pipe() { m_ok = ::pipe(m_fds) == 0; }
  ~Pipe() {
    for (const int fd : m_fds) {
      if (fd >= 0) ::close(fd);
    }
  }
  Pipe(const Pipe&) = delete;
  auto operator=(const Pipe&) -> Pipe& = delete;
  [[nodiscard]] auto ok() const noexcept -> bool { return m_ok; }
  [[nodiscard]] auto read_fd() const noexcept -> int { return m_fds[0]; }

 private:
  int m_fds[2]{-1, -1};
  bool m_ok{};
};

class NoEditor final : public surfaces::DraftEditor {
 public:
  auto edit(std::string_view, std::stop_token)
      -> std::expected<std::string, surfaces::DraftEditorError> override {
    return std::unexpected(surfaces::DraftEditorError{
        surfaces::DraftEditorErrorCode::not_configured,
        "editor is unavailable in a scenario"});
  }
};

class ScenarioCatalogSource final : public model::CatalogSource {
 public:
  auto fetch(std::stop_token)
      -> std::expected<model::CatalogSnapshot, model::CatalogError> override {
    model::CatalogEntry current{make_id<domain::ModelId>("model"), "text"};
    current.name = "Current model";
    model::CatalogEntry alternate{make_id<domain::ModelId>("alternate"),
                                  "text"};
    alternate.name = "Alternate model";
    return model::CatalogSnapshot{
        std::chrono::sys_time<std::chrono::milliseconds>{123ms},
        {std::move(current), std::move(alternate)}};
  }
};

class SessionScenarioStore final : public storage::SessionStore {
 public:
  explicit SessionScenarioStore(const bool fail_listing = false)
      : m_fail_listing(fail_listing) {
    const auto target = make_id<domain::SessionId>("target-session");
    m_sessions.emplace(
        target, storage::SessionInfo{target, domain::EventTimestamp{500ms},
                                     domain::EventTimestamp{3000ms}, 11, 7});
    const auto surface = make_id<domain::SurfaceId>("interactive");
    const auto workspace = make_id<domain::WorkspaceId>("chat");
    const auto profile = make_id<domain::PermissionProfileId>("observe");
    const auto first_inference =
        make_id<domain::InferenceId>("failed-inference");
    const auto second_inference =
        make_id<domain::InferenceId>("cancelled-inference");
    const auto model = make_id<domain::ModelId>("model");
    m_histories[target] = {
        run_event(
            1, "target-start-1", "target-run-1",
            domain::RunStarted{surface, workspace, profile, std::nullopt}),
        run_event(2, "target-inference-1", "target-run-1",
                  domain::InferenceStarted{first_inference, model}),
        run_event(3, "target-usage-1", "target-run-1",
                  domain::UsageRecorded{first_inference, {5, 3, 2, 1}}),
        run_event(
            4, "target-cost-1", "target-run-1",
            domain::InferenceCostRecorded{first_inference, reported_cost()}),
        run_event(5, "target-inference-failed", "target-run-1",
                  domain::InferenceFailed{
                      first_inference,
                      {domain::ErrorCode::backend, "failed", false}}),
        run_event(
            6, "target-run-failed", "target-run-1",
            domain::RunFailed{{domain::ErrorCode::backend, "failed", false}}),
        run_event(
            7, "target-start-2", "target-run-2",
            domain::RunStarted{surface, workspace, profile, std::nullopt}),
        run_event(8, "target-inference-2", "target-run-2",
                  domain::InferenceStarted{second_inference, model}),
        run_event(9, "target-usage-2", "target-run-2",
                  domain::UsageRecorded{second_inference, {7, 4, 0, 0}}),
        run_event(10, "target-inference-cancelled", "target-run-2",
                  domain::InferenceCancelled{second_inference, "cancelled"}),
        run_event(11, "target-run-cancelled", "target-run-2",
                  domain::RunCancelled{"cancelled"}),
    };
    const auto overflow = make_id<domain::SessionId>("usage-overflow-session");
    m_sessions.emplace(
        overflow, storage::SessionInfo{overflow, domain::EventTimestamp{450ms},
                                       domain::EventTimestamp{2750ms}, 10, 2});
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto overflow_first = make_id<domain::InferenceId>("overflow-first");
    const auto overflow_second =
        make_id<domain::InferenceId>("overflow-second");
    m_histories[overflow] = {
        run_event(
            1, "overflow-start-1", "overflow-run-1",
            domain::RunStarted{surface, workspace, profile, std::nullopt}),
        run_event(2, "overflow-inference-1", "overflow-run-1",
                  domain::InferenceStarted{overflow_first, model}),
        run_event(3, "overflow-usage-1", "overflow-run-1",
                  domain::UsageRecorded{overflow_first, {maximum, 0, 0, 0}}),
        run_event(4, "overflow-finish-1", "overflow-run-1",
                  domain::InferenceFinished{overflow_first,
                                            domain::FinishReason::stop}),
        run_event(5, "overflow-complete-1", "overflow-run-1",
                  domain::RunCompleted{}),
        run_event(
            6, "overflow-start-2", "overflow-run-2",
            domain::RunStarted{surface, workspace, profile, std::nullopt}),
        run_event(7, "overflow-inference-2", "overflow-run-2",
                  domain::InferenceStarted{overflow_second, model}),
        run_event(8, "overflow-usage-2", "overflow-run-2",
                  domain::UsageRecorded{overflow_second, {1, 0, 0, 0}}),
        run_event(9, "overflow-finish-2", "overflow-run-2",
                  domain::InferenceFinished{overflow_second,
                                            domain::FinishReason::stop}),
        run_event(10, "overflow-complete-2", "overflow-run-2",
                  domain::RunCompleted{}),
    };
    const auto corrupt = make_id<domain::SessionId>("corrupt-session");
    m_sessions.emplace(
        corrupt, storage::SessionInfo{corrupt, domain::EventTimestamp{400ms},
                                      domain::EventTimestamp{2500ms}, 1, 1});
    m_histories[corrupt] = {
        domain::RunEvent{{make_id<domain::EventId>("corrupt-event"),
                          make_id<domain::RunId>("corrupt-run"), 1, 1,
                          domain::EventTimestamp{2500ms}, std::nullopt,
                          std::nullopt, std::nullopt},
                         domain::RunCompleted{}}};
  }

  auto create_session(storage::SessionCreate session, std::stop_token token)
      -> std::expected<void, storage::SessionStoreError> override {
    if (token.stop_requested()) return std::unexpected(cancelled());
    if (m_sessions.contains(session.session_id)) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::already_exists, "session exists",
          false});
    }
    const auto timestamp = domain::EventTimestamp{std::chrono::milliseconds{
        1000 * static_cast<std::int64_t>(m_sessions.size() + 1)}};
    m_sessions.emplace(
        session.session_id,
        storage::SessionInfo{session.session_id, timestamp, timestamp, 0, 0});
    return {};
  }

  auto open_session(const domain::SessionId& session_id, std::stop_token token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    if (token.stop_requested()) return std::unexpected(cancelled());
    const auto found = m_sessions.find(session_id);
    if (found == m_sessions.end()) {
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::not_found,
                                     "session disappeared", false});
    }
    return found->second;
  }

  auto list_sessions(const std::size_t limit, std::stop_token token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    if (token.stop_requested()) return std::unexpected(cancelled());
    if (m_fail_listing) {
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::contention,
                                     "session catalog is contended", true});
    }
    std::vector<storage::SessionInfo> result;
    for (const auto& [id, info] : m_sessions) {
      static_cast<void>(id);
      result.push_back(info);
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
      if (left.last_activity_at != right.last_activity_at) {
        return left.last_activity_at > right.last_activity_at;
      }
      return left.session_id < right.session_id;
    });
    if (result.size() > limit) {
      result.erase(result.begin() + static_cast<std::ptrdiff_t>(limit),
                   result.end());
    }
    return result;
  }

  auto append_events(const domain::SessionId& session_id,
                     const std::span<const domain::RunEvent> events,
                     std::stop_token token)
      -> std::expected<void, storage::SessionStoreError> override {
    if (token.stop_requested()) return std::unexpected(cancelled());
    const auto found = m_sessions.find(session_id);
    if (found == m_sessions.end()) {
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::not_found,
                                     "session disappeared", false});
    }
    auto& history = m_histories[session_id];
    history.insert(history.end(), events.begin(), events.end());
    if (!history.empty()) {
      found->second.last_sequence = history.back().metadata.sequence;
      found->second.last_activity_at = history.back().metadata.timestamp;
      std::set<domain::RunId> runs;
      for (const auto& event : history)
        runs.insert(event.metadata.run_id);
      found->second.run_count = runs.size();
    }
    return {};
  }

  auto replay_events(const domain::SessionId& session_id, std::stop_token token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    if (token.stop_requested()) return std::unexpected(cancelled());
    if (!m_sessions.contains(session_id)) {
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::not_found,
                                     "session disappeared", false});
    }
    return m_histories[session_id];
  }

 private:
  [[nodiscard]] static auto cancelled() -> storage::SessionStoreError {
    return {storage::SessionStoreErrorCode::cancelled,
            "session operation cancelled", false};
  }

  bool m_fail_listing{};
  std::map<domain::SessionId, storage::SessionInfo> m_sessions;
  std::map<domain::SessionId, std::vector<domain::RunEvent>> m_histories;
};

class GatedBackendState final {
 public:
  auto initialize(const backend::BackendRequest& request) -> bool {
    std::lock_guard lock{m_mutex};
    if (m_initialized) return false;
    m_steps = {
        backend::BackendEvent{backend::ResponseStarted{"response"}},
        backend::BackendEvent{backend::ContentDelta{
            request.assistant_message_id, domain::TextBlock{"hello"}}},
        backend::BackendEvent{backend::UsageObserved{{3, 2, 1, 1}}},
        backend::BackendEvent{backend::CostObserved{reported_cost()}},
        backend::BackendEvent{
            backend::ResponseFinished{domain::FinishReason::stop}},
        std::nullopt,
    };
    m_initialized = true;
    m_condition.notify_all();
    return true;
  }

  auto next(std::stop_token stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> {
    std::unique_lock lock{m_mutex};
    if (m_cancelled) {
      m_cancel_end_waiting = true;
      m_condition.notify_all();
      static_cast<void>(m_condition.wait_for(
          lock, 1s, [&] { return m_cancel_end_released; }));
      m_ended = true;
      m_condition.notify_all();
      return std::nullopt;
    }
    ++m_waiting_calls;
    m_condition.notify_all();
    if (!m_condition.wait(lock, stop_token,
                          [&] { return m_released > m_consumed; })) {
      m_cancelled = true;
      m_condition.notify_all();
      return backend::BackendEvent{backend::ResponseCancelled{"cancelled"}};
    }
    auto result = m_steps[m_consumed++];
    if (!result) m_ended = true;
    m_condition.notify_all();
    return result;
  }

  auto release(const std::string_view description)
      -> std::expected<void, std::string> {
    static const std::vector<std::string_view> expected{
        "response-started",  "delta:hello", "usage", "cost",
        "response-finished", "end"};
    std::unique_lock lock{m_mutex};
    if (!m_condition.wait_for(lock, 1s, [&] {
          return m_initialized && m_waiting_calls >= m_released + 1;
        })) {
      return std::unexpected("backend did not reach the scripted boundary");
    }
    if (m_released >= expected.size() || description != expected[m_released]) {
      return std::unexpected("backend script descriptor mismatch");
    }
    ++m_released;
    m_condition.notify_all();
    const auto released = m_released;
    const bool acknowledged = m_condition.wait_for(lock, 1s, [&] {
      return released == expected.size() ? m_ended
                                         : m_waiting_calls >= released + 1;
    });
    if (!acknowledged) {
      return std::unexpected("backend did not acknowledge the scripted step");
    }
    return {};
  }

  auto observe_wake() -> void {
    std::lock_guard lock{m_mutex};
    ++m_observed_wakes;
    m_condition.notify_all();
  }

  auto wait_for_wake(const std::size_t count)
      -> std::expected<void, std::string> {
    std::unique_lock lock{m_mutex};
    if (!m_condition.wait_for(lock, 1s,
                              [&] { return m_observed_wakes >= count; })) {
      return std::unexpected("backend update was not posted to the UI loop");
    }
    return {};
  }

  auto release_cancel_end() -> std::expected<void, std::string> {
    std::unique_lock lock{m_mutex};
    if (!m_condition.wait_for(lock, 1s, [&] { return m_cancel_end_waiting; })) {
      return std::unexpected("cancelled backend did not request stream end");
    }
    m_cancel_end_released = true;
    m_condition.notify_all();
    if (!m_condition.wait_for(lock, 1s, [&] { return m_ended; })) {
      return std::unexpected("cancelled backend did not end its stream");
    }
    return {};
  }

 private:
  std::mutex m_mutex;
  std::condition_variable_any m_condition;
  std::vector<std::optional<backend::BackendEvent>> m_steps;
  std::size_t m_waiting_calls{};
  std::size_t m_released{};
  std::size_t m_consumed{};
  std::size_t m_observed_wakes{};
  bool m_initialized{};
  bool m_ended{};
  bool m_cancelled{};
  bool m_cancel_end_waiting{};
  bool m_cancel_end_released{};
};

class GatedStream final : public backend::BackendStream {
 public:
  explicit GatedStream(std::shared_ptr<GatedBackendState> state)
      : m_state(std::move(state)) {}

  auto next(std::stop_token stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    return m_state->next(stop_token);
  }

 private:
  std::shared_ptr<GatedBackendState> m_state;
};

class GatedBackend final : public backend::Backend,
                           public backend::ModelContextProvider {
 public:
  explicit GatedBackend(std::shared_ptr<GatedBackendState> state)
      : m_state(std::move(state)) {}

  auto lookup(const domain::ModelId& model_id, std::stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    return backend::ModelContextInfo{model_id, 8192, 1024,
                                     pricing_observation()};
  }

  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    if (!m_state->initialize(request)) {
      return std::unexpected(backend::BackendError{
          backend::BackendErrorKind::script_exhausted,
          "scenario backend was started more than once", false, std::nullopt});
    }
    return std::make_unique<GatedStream>(m_state);
  }

 private:
  std::shared_ptr<GatedBackendState> m_state;
};

struct ModalState {
  std::string result{"pending"};
  int columns{};
  int rows{};
};

class ModalProbe final : public termforge::App {
 public:
  ModalProbe(std::shared_ptr<ModalState> state, termforge::ByteSink* output)
      : m_state(std::move(state)), m_output(output) {
    if (::pipe(m_pipe) != 0 ||
        !terminal().set_io(termforge::TerminalIo{m_pipe[0], -1})) {
      if (m_pipe[0] >= 0) ::close(m_pipe[0]);
      if (m_pipe[1] >= 0) ::close(m_pipe[1]);
      m_pipe[0] = -1;
      m_pipe[1] = -1;
    }
    set_frame_ms(10);
    termforge::ChoiceWizardPage first;
    first.title = "First";
    first.text = "Choose first";
    first.mode = termforge::ChoiceMode::Single;
    first.choices = {{"One", "first option"}};
    first.selected_indices = {0};
    first.minimum_selected = 1;
    first.maximum_selected = 1;
    termforge::ChoiceWizardPage second;
    second.title = "Second";
    second.text = "Choose second";
    second.mode = termforge::ChoiceMode::Single;
    second.choices = {{"Two", "second option"}};
    second.selected_indices = {0};
    second.minimum_selected = 1;
    second.maximum_selected = 1;
    REQUIRE(m_dialog.set_pages({std::move(first), std::move(second)}));
    m_dialog.on_result(
        [this](std::optional<termforge::ChoiceWizardResult> result) {
          m_state->result = result ? "submitted" : "cancelled";
          pop_overlay();
          quit();
        });
  }

  ~ModalProbe() override {
    for (const int fd : m_pipe) {
      if (fd >= 0) ::close(fd);
    }
  }

  [[nodiscard]] auto ready() const noexcept -> bool { return m_pipe[0] >= 0; }

  [[nodiscard]] auto configure(const termforge::Capabilities& capabilities)
      -> std::expected<void, std::string> {
    auto configured = terminal().set_capabilities(capabilities);
    if (!configured) return std::unexpected(configured.error().message);
    return {};
  }

  auto on_start() -> void override {
    if (m_output != nullptr) driver().set_output(m_output);
    push_overlay(m_dialog, {.backdrop = termforge::Backdrop::Fill});
  }

  auto on_event(const termforge::Event& event) -> void override {
    if (const auto* resized = std::get_if<termforge::ResizeEvent>(&event)) {
      m_state->columns = resized->cols;
      m_state->rows = resized->rows;
    }
  }

  auto on_render(termforge::Screen& screen) -> void override { screen.clear(); }

  [[nodiscard]] auto frame_state() -> std::string {
    return std::to_string(m_dialog.current_page()) + ":" +
           std::to_string(screen().cols()) + "x" +
           std::to_string(screen().rows()) + ":" + m_state->result;
  }

 private:
  std::shared_ptr<ModalState> m_state;
  termforge::ByteSink* m_output{};
  termforge::ChoiceWizardDialog m_dialog;
  int m_pipe[2]{-1, -1};
};

auto key(const char32_t value) -> testing::TuiScenarioPost {
  return {termforge::KeyEvent{termforge::Key::Char, value, false, false, false,
                              termforge::KeyAction::Press}};
}

auto scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-stream-tool-resize";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {20, 4, 160, 80};
  value.backend_script = {"delta:hello"};
  value.tool_script = {"result:ok"};
  value.steps = {
      {0, key(U'a')},
      {1, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {2, testing::TuiScenarioResize{{12, 3, 120, 60}}},
      {3, testing::TuiScenarioRelease{testing::TuiScenarioProducer::tool}},
      {4, key(U'q')},
  };
  return value;
}

auto factory(const bool diverge_on_replay = false,
             const bool mismatch_capabilities = false)
    -> testing::TuiScenarioTargetFactory {
  return [=](const testing::TuiScenarioPass pass, termforge::ByteSink* output)
             -> std::expected<testing::TuiScenarioTarget,
                              testing::TuiScenarioError> {
    auto state = std::make_shared<ProbeState>();
    auto app = std::make_unique<ScenarioProbe>(state, output);
    auto* raw = app.get();
    if (!raw->ready()) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          "probe pipe setup failed"});
    }
    const auto live = pass == testing::TuiScenarioPass::record;
    return testing::TuiScenarioTarget{
        std::move(app),
        [raw, pass,
         mismatch_capabilities](const termforge::Capabilities& requested)
            -> std::expected<void, std::string> {
          auto selected = requested;
          if (pass == testing::TuiScenarioPass::replay &&
              mismatch_capabilities) {
            selected.truecolor = !selected.truecolor;
            selected.color_levels = selected.truecolor ? 24 : 0;
          }
          return raw->configure(selected);
        },
        [raw] { return raw->run(); },
        [raw, live](
            const std::string_view step) -> std::expected<void, std::string> {
          if (step != "delta:hello") {
            return std::unexpected("unexpected backend step");
          }
          if (live) {
            raw->post(termforge::ErrorEvent{termforge::Severity::Info,
                                            "fake.backend", "hello"});
          }
          return {};
        },
        [raw, live](
            const std::string_view step) -> std::expected<void, std::string> {
          if (step != "result:ok") {
            return std::unexpected("unexpected tool step");
          }
          if (live) {
            raw->post(termforge::ErrorEvent{termforge::Severity::Info,
                                            "fake.tool", "ok"});
          }
          return {};
        },
        [state, pass, diverge_on_replay] {
          auto result = state->frame;
          if (pass == testing::TuiScenarioPass::replay && diverge_on_replay) {
            result += "-changed";
          }
          return result;
        },
        [state] {
          return state->events + ":" + std::to_string(state->columns) + "x" +
                 std::to_string(state->rows) +
                 ":ticks=" + std::to_string(state->ticks);
        }};
  };
}

auto chat_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-chat-stream";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {240, 10, 2400, 200};
  value.backend_script = {"response-started",  "delta:hello", "usage", "cost",
                          "response-finished", "end"};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"question"}}},
      {0, enter},
      {1, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {2, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {3, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {4, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {5, testing::TuiScenarioResize{{32, 6, 320, 120}}},
      {5, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {6, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {7, testing::TuiScenarioResize{{120, 12, 1200, 240}}},
      {8, testing::TuiScenarioPost{termforge::PasteEvent{"/usage"}}},
      {8, enter},
      {9, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}},
      {9, enter},
  };
  value.limits.maximum_frames = 32;
  return value;
}

auto chat_cancellation_scenario(std::string scenario_id,
                                termforge::KeyEvent cancellation)
    -> testing::TuiScenario {
  auto value = chat_scenario();
  value.scenario_id = std::move(scenario_id);
  value.backend_script = {"response-started", "delta:hello", "cancelled",
                          "cancel-end"};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"question"}}},
      {0, enter},
      {1, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {2, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {3, testing::TuiScenarioPost{cancellation}},
      {4, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {5, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {6, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}},
      {6, enter},
  };
  return value;
}

auto idle_control_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-idle-control-keys";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {120, 8, 1200, 160};
  const auto control = [](const char32_t ch) {
    return testing::TuiScenarioPost{
        termforge::KeyEvent{termforge::Key::Char, ch, true, false, false,
                            termforge::KeyAction::Press}};
  };
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"single-line draft"}}},
      {1, control(U'c')},
      {2, testing::TuiScenarioResize{{48, 5, 480, 100}}},
      {3, testing::TuiScenarioPost{termforge::PasteEvent{
              "alpha\nbeta\ngamma\ndelta\nepsilon"}}},
      {4, control(U'c')},
      {5, testing::TuiScenarioResize{{120, 8, 1200, 160}}},
      {6, control(U'c')},
      {7, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Escape, 0, false, false, false,
              termforge::KeyAction::Press}}},
      {8, testing::TuiScenarioPost{termforge::PasteEvent{"after escape"}}},
      {9, control(U'c')},
      {10, control(U'd')},
  };
  value.limits.maximum_frames = 32;
  return value;
}

auto help_control_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-help-control-keys";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 8, 1000, 160};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/help"}}},
      {0, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Enter, 0, false, false, false,
              termforge::KeyAction::Press}}},
      {1, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Escape, 0, false, false, false,
              termforge::KeyAction::Press}}},
      {2, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Char, U'd', true, false, false,
              termforge::KeyAction::Press}}},
  };
  value.limits.maximum_frames = 16;
  return value;
}

auto empty_usage_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-empty-usage";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 14, 1000, 280};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/usage"}}},
      {0, enter},
      {1, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}},
      {1, enter},
  };
  value.limits.maximum_frames = 12;
  return value;
}

auto empty_plan_tasks_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-empty-plan-tasks";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 12, 1000, 240};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  const auto escape = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Escape, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/plan"}}},
      {0, enter},
      {1, escape},
      {2, testing::TuiScenarioPost{termforge::PasteEvent{"/tasks"}}},
      {2, enter},
      {3, testing::TuiScenarioResize{{24, 5, 240, 100}}},
      {4, escape},
      {5, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Char, U'd', true, false, false,
              termforge::KeyAction::Press}}},
  };
  value.limits.maximum_frames = 20;
  return value;
}

auto active_control_d_scenario() -> testing::TuiScenario {
  auto value = chat_scenario();
  value.scenario_id = "interactive-active-control-d";
  value.initial_size = {120, 8, 1200, 160};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  const auto control_d = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Char, U'd', true, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"question"}}},
      {0, enter},
      {1, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {2, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {3, control_d},
      {4, testing::TuiScenarioResize{{48, 5, 480, 100}}},
      {5, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {6, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {7, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {8, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {10, control_d},
  };
  value.limits.maximum_frames = 32;
  return value;
}

auto cursor_lifecycle_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-visible-composer-cursor";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {8, 4, 80, 80};
  value.backend_script = {"response-started",  "delta:hello", "usage", "cost",
                          "response-finished", "end"};
  const auto key = [](const termforge::Key value) {
    return testing::TuiScenarioPost{termforge::KeyEvent{
        value, 0, false, false, false, termforge::KeyAction::Press}};
  };
  const auto control = [](const char32_t ch) {
    return testing::TuiScenarioPost{
        termforge::KeyEvent{termforge::Key::Char, ch, true, false, false,
                            termforge::KeyAction::Press}};
  };
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"abcd"}}},
      {1, key(termforge::Key::Left)},
      {2, testing::TuiScenarioResize{{4, 4, 40, 80}}},
      {3, key(termforge::Key::Right)},
      {4, control(U'c')},
      {5, testing::TuiScenarioResize{{1, 1, 10, 20}}},
      {6, testing::TuiScenarioPost{termforge::PasteEvent{"tiny"}}},
      {7, testing::TuiScenarioResize{{8, 2, 80, 40}}},
      {8, control(U'c')},
      {9, testing::TuiScenarioResize{{20, 6, 200, 120}}},
      {10, testing::TuiScenarioPost{termforge::PasteEvent{"/help"}}},
      {10, key(termforge::Key::Enter)},
      {11, key(termforge::Key::Escape)},
      {12, testing::TuiScenarioPost{termforge::PasteEvent{"prompt"}}},
      {12, key(termforge::Key::Enter)},
      {13, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {14, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {15, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {16, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {17, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {18, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {19, key(termforge::Key::Up)},
      {20, testing::TuiScenarioPost{termforge::MouseEvent{2, 4, 0, true}}},
      {21, control(U'e')},
  };
  value.limits.maximum_frames = 48;
  return value;
}

auto cursor_modal_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-composer-cursor-modal-focus";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {20, 6, 200, 120};
  const auto key = [](const termforge::Key value) {
    return testing::TuiScenarioPost{termforge::KeyEvent{
        value, 0, false, false, false, termforge::KeyAction::Press}};
  };
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/model"}}},
      {0, key(termforge::Key::Enter)},
      {1, key(termforge::Key::Escape)},
      {2, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Char, U'd', true, false, false,
              termforge::KeyAction::Press}}},
  };
  value.limits.maximum_frames = 16;
  return value;
}

auto startup_model_scenario(const bool cancel) -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = cancel ? "interactive-startup-model-cancel"
                             : "interactive-startup-model-select-resize";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 28, 1000, 560};
  const auto key = [](const termforge::Key selected) {
    return testing::TuiScenarioPost{termforge::KeyEvent{
        selected, 0, false, false, false, termforge::KeyAction::Press}};
  };
  if (cancel) {
    value.steps = {{0, key(termforge::Key::Escape)}};
  } else {
    value.steps = {
        {0, testing::TuiScenarioResize{{8, 3, 80, 60}}},
        {1, testing::TuiScenarioResize{{50, 12, 500, 240}}},
        {2, testing::TuiScenarioResize{{100, 28, 1000, 560}}},
    };
    for (const char character : std::string_view{"cap:tools=false"}) {
      value.steps.push_back(
          {3, testing::TuiScenarioPost{termforge::KeyEvent{
                  termforge::Key::Char, static_cast<char32_t>(character), false,
                  false, false, termforge::KeyAction::Press}}});
    }
    value.steps.push_back({4, key(termforge::Key::Enter)});
  }
  value.limits.maximum_frames = 16;
  return value;
}

auto startup_model_factory(
    const model::CatalogOrigin origin = model::CatalogOrigin::live)
    -> testing::TuiScenarioTargetFactory {
  return [origin](testing::TuiScenarioPass, termforge::ByteSink* output)
             -> std::expected<testing::TuiScenarioTarget,
                              testing::TuiScenarioError> {
    auto pipe = std::make_shared<Pipe>();
    if (!pipe->ok()) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          "startup model scenario pipe setup failed"});
    }
    model::CatalogEntry first{make_id<domain::ModelId>("model"), "text"};
    first.name = "Current model";
    first.context_window_tokens = 8192;
    first.maximum_output_tokens = 1024;
    first.capabilities.push_back({model::Capability::tool_calling, true});
    model::CatalogEntry alternate{make_id<domain::ModelId>("alternate"),
                                  "text"};
    alternate.name = "Alternate model";
    alternate.context_window_tokens = 8192;
    alternate.maximum_output_tokens = 2048;
    alternate.capabilities.push_back({model::Capability::tool_calling, false});
    model::CatalogSnapshot snapshot{
        std::chrono::sys_time<std::chrono::milliseconds>{123ms},
        {std::move(first), std::move(alternate)}};
    snapshot.origin = origin;
    if (origin == model::CatalogOrigin::stale_cache) {
      snapshot.warnings.push_back("using stale model catalog");
    }
    auto frame = std::make_shared<std::string>();
    adapters::InteractiveModelPickerAppOptions options;
    options.rendered_output = output;
    options.rendered_frame = [frame](const termforge::Screen& screen) {
      *frame = normalized_screen(screen);
    };
    auto app = adapters::make_interactive_model_picker_app(snapshot, {},
                                                           std::move(options));
    auto* raw = app.get();
    return testing::TuiScenarioTarget{
        std::move(app),
        [raw, pipe](const termforge::Capabilities& capabilities) {
          return raw->configure_terminal_for_scenario(
              termforge::TerminalIo{pipe->read_fd(), -1}, capabilities);
        },
        [raw] { return raw->run(); },
        [](std::string_view) -> std::expected<void, std::string> {
          return std::unexpected("startup model scenario has no backend");
        },
        [](std::string_view) -> std::expected<void, std::string> {
          return std::unexpected("startup model scenario has no tool");
        },
        [frame] { return *frame; },
        [raw] {
          if (const auto selected = raw->selected_model()) {
            return "selected:" + std::string{selected->value()};
          }
          return raw->cancelled() ? std::string{"cancelled"}
                                  : std::string{raw->status_text()};
        }};
  };
}

auto chat_factory(const bool with_catalog = false)
    -> testing::TuiScenarioTargetFactory {
  return [with_catalog](const testing::TuiScenarioPass pass,
                        termforge::ByteSink* output)
             -> std::expected<testing::TuiScenarioTarget,
                              testing::TuiScenarioError> {
    auto pipe = std::make_shared<Pipe>();
    if (!pipe->ok()) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          "chat scenario pipe setup failed"});
    }
    auto backend_state = std::make_shared<GatedBackendState>();
    auto backend = std::make_shared<GatedBackend>(backend_state);
    auto editor = std::make_shared<NoEditor>();
    auto catalog_source = std::make_shared<ScenarioCatalogSource>();
    auto catalog = std::make_shared<model::CatalogService>(*catalog_source);
    auto frame = std::make_shared<std::string>();
    auto suffix = std::make_shared<std::uint64_t>();
    adapters::InteractiveChatAppOptions options;
    options.live_wake_enabled = pass == testing::TuiScenarioPass::record;
    options.poll_worker_updates = false;
    if (with_catalog) options.model_catalog = catalog.get();
    options.wake_observer = [backend_state] { backend_state->observe_wake(); };
    options.rendered_output = output;
    options.rendered_frame = [frame](const termforge::Screen& screen) {
      *frame = normalized_screen(screen);
    };
    options.session_dependencies.identity_suffix_source = [suffix] {
      return ++*suffix;
    };
    options.session_dependencies.timestamp_source = [] {
      return domain::EventTimestamp{123ms};
    };
    auto app = adapters::make_interactive_chat_app(
        *backend, *backend, nullptr,
        {make_id<domain::ModelId>("model"),
         surfaces::ChatSessionOpen::Mode::ephemeral, std::nullopt},
        *editor, {}, std::move(options));
    if (!app->ready()) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          app->setup_error().message});
    }
    auto* raw = app.get();
    return testing::TuiScenarioTarget{
        std::move(app),
        [raw, pipe](const termforge::Capabilities& capabilities) {
          return raw->configure_terminal_for_scenario(
              termforge::TerminalIo{pipe->read_fd(), -1}, capabilities);
        },
        [raw, backend, editor, catalog_source, catalog] {
          static_cast<void>(backend);
          static_cast<void>(editor);
          static_cast<void>(catalog_source);
          static_cast<void>(catalog);
          return raw->run();
        },
        [backend_state](const std::string_view step) {
          if (step == "cancelled") return backend_state->wait_for_wake(3);
          if (step == "cancel-end") {
            auto ended = backend_state->release_cancel_end();
            if (!ended) return ended;
            return backend_state->wait_for_wake(4);
          }
          auto released = backend_state->release(step);
          if (!released) return released;
          static const std::vector<std::string_view> expected{
              "response-started",  "delta:hello", "usage", "cost",
              "response-finished", "end"};
          const auto found = std::ranges::find(expected, step);
          if (found == expected.end()) {
            return std::expected<void, std::string>{
                std::unexpected("unknown backend wake boundary")};
          }
          return backend_state->wait_for_wake(
              static_cast<std::size_t>(std::distance(expected.begin(), found)) +
              1);
        },
        [](std::string_view) -> std::expected<void, std::string> {
          return std::unexpected("chat scenario has no tool script");
        },
        [frame] { return *frame; },
        [raw, backend, editor, catalog_source, catalog] {
          static_cast<void>(backend);
          static_cast<void>(editor);
          static_cast<void>(catalog_source);
          static_cast<void>(catalog);
          std::ostringstream state;
          state << raw->status_text();
          if (raw->pending_edit()) state << "|pending-edit";
          for (const auto& event : raw->events()) {
            state << '|' << event.metadata.sequence << ':'
                  << event.payload.index();
            if (std::holds_alternative<domain::RunCancelled>(event.payload)) {
              state << ":cancelled";
            }
          }
          return std::move(state).str();
        }};
  };
}

auto session_success_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-session-list-resume-new";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {180, 12, 1800, 240};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  const auto escape = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Escape, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/session list"}}},
      {0, enter},
      {1, escape},
      {2, testing::TuiScenarioPost{termforge::PasteEvent{
              "/session resume session-1"}}},
      {2, enter},
      {3, testing::TuiScenarioPost{termforge::PasteEvent{
              "/session resume target-session"}}},
      {3, enter},
      {4, testing::TuiScenarioPost{termforge::PasteEvent{"/session new"}}},
      {4, enter},
      {5, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}},
      {5, enter},
  };
  value.limits.maximum_frames = 24;
  return value;
}

auto session_failure_scenario(std::string scenario_id, std::string command)
    -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = std::move(scenario_id);
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 8, 1000, 160};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{std::move(command)}}},
      {0, enter},
      {2, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Char, U'd', true, false, false,
              termforge::KeyAction::Press}}},
  };
  value.limits.maximum_frames = 16;
  return value;
}

auto ephemeral_session_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-ephemeral-session-actions";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 9, 1000, 180};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  const auto escape = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Escape, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/session"}}},
      {0, enter},
      {1, escape},
      {2, testing::TuiScenarioPost{termforge::PasteEvent{"/session new"}}},
      {2, enter},
      {3, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}},
      {3, enter},
  };
  value.limits.maximum_frames = 20;
  return value;
}

auto session_factory(const bool durable, const bool fail_listing = false)
    -> testing::TuiScenarioTargetFactory {
  return [=](testing::TuiScenarioPass, termforge::ByteSink* output)
             -> std::expected<testing::TuiScenarioTarget,
                              testing::TuiScenarioError> {
    auto pipe = std::make_shared<Pipe>();
    if (!pipe->ok()) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          "session scenario pipe setup failed"});
    }
    auto backend_state = std::make_shared<GatedBackendState>();
    auto backend = std::make_shared<GatedBackend>(backend_state);
    auto editor = std::make_shared<NoEditor>();
    auto store = std::make_shared<SessionScenarioStore>(fail_listing);
    auto frame = std::make_shared<std::string>();
    auto suffix = std::make_shared<std::uint64_t>();
    adapters::InteractiveChatAppOptions options;
    options.live_wake_enabled = false;
    options.poll_worker_updates = false;
    options.rendered_output = output;
    options.rendered_frame = [frame](const termforge::Screen& screen) {
      *frame = normalized_screen(screen);
    };
    options.session_dependencies.identity_suffix_source = [suffix] {
      return ++*suffix;
    };
    options.session_dependencies.timestamp_source = [] {
      return domain::EventTimestamp{123ms};
    };
    auto app = adapters::make_interactive_chat_app(
        *backend, *backend, durable ? store.get() : nullptr,
        {make_id<domain::ModelId>("model"),
         durable ? surfaces::ChatSessionOpen::Mode::create
                 : surfaces::ChatSessionOpen::Mode::ephemeral,
         std::nullopt},
        *editor, {}, std::move(options));
    if (!app->ready()) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          app->setup_error().message});
    }
    auto* raw = app.get();
    return testing::TuiScenarioTarget{
        std::move(app),
        [raw, pipe](const termforge::Capabilities& capabilities) {
          return raw->configure_terminal_for_scenario(
              termforge::TerminalIo{pipe->read_fd(), -1}, capabilities);
        },
        [raw, backend, editor, store] {
          static_cast<void>(backend);
          static_cast<void>(editor);
          static_cast<void>(store);
          return raw->run();
        },
        [](std::string_view) -> std::expected<void, std::string> {
          return std::unexpected("session scenario has no backend script");
        },
        [](std::string_view) -> std::expected<void, std::string> {
          return std::unexpected("session scenario has no tool script");
        },
        [frame] { return *frame; },
        [raw, backend, editor, store] {
          static_cast<void>(backend);
          static_cast<void>(editor);
          static_cast<void>(store);
          return std::string{raw->status_text()};
        }};
  };
}

auto modal_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "ask-user-modal-resize";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {40, 12, 400, 240};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, enter},
      {1, testing::TuiScenarioResize{{8, 3, 80, 60}}},
      {2, testing::TuiScenarioResize{{40, 12, 400, 240}}},
      {3, enter},
  };
  value.limits.maximum_frames = 16;
  return value;
}

auto modal_factory() -> testing::TuiScenarioTargetFactory {
  return [](testing::TuiScenarioPass, termforge::ByteSink* output)
             -> std::expected<testing::TuiScenarioTarget,
                              testing::TuiScenarioError> {
    auto state = std::make_shared<ModalState>();
    auto app = std::make_unique<ModalProbe>(state, output);
    auto* raw = app.get();
    if (!raw->ready()) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          "modal scenario pipe setup failed"});
    }
    return testing::TuiScenarioTarget{
        std::move(app),
        [raw](const termforge::Capabilities& capabilities) {
          return raw->configure(capabilities);
        },
        [raw] { return raw->run(); },
        [](std::string_view) -> std::expected<void, std::string> {
          return std::unexpected("modal scenario has no backend script");
        },
        [](std::string_view) -> std::expected<void, std::string> {
          return std::unexpected("modal scenario has no tool script");
        },
        [raw] { return raw->frame_state(); },
        [state] {
          return state->result + ":" + std::to_string(state->columns) + "x" +
                 std::to_string(state->rows);
        }};
  };
}

} // namespace

TEST_CASE(
    "TUI scenarios reject malformed specifications before creating an app",
    "[scenario][failure]") {
  auto invalid = scenario();
  invalid.schema_version = 2;
  int calls{};
  auto never = [&calls](testing::TuiScenarioPass, termforge::ByteSink*)
      -> std::expected<testing::TuiScenarioTarget, testing::TuiScenarioError> {
    ++calls;
    return std::unexpected(testing::TuiScenarioError{
        testing::TuiScenarioErrorCode::target_failure, "unexpected"});
  };
  const auto rejected = testing::run_tui_scenario(invalid, never);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          testing::TuiScenarioErrorCode::invalid_scenario);
  REQUIRE(calls == 0);

  invalid = scenario();
  invalid.steps.erase(invalid.steps.begin() + 1);
  const auto mismatched = testing::run_tui_scenario(invalid, never);
  REQUIRE_FALSE(mismatched);
  REQUIRE(mismatched.error().code ==
          testing::TuiScenarioErrorCode::script_mismatch);
  REQUIRE(calls == 0);

  invalid = scenario();
  std::swap(invalid.steps[0], invalid.steps[1]);
  const auto unordered = testing::run_tui_scenario(invalid, never);
  REQUIRE_FALSE(unordered);
  REQUIRE(unordered.error().code ==
          testing::TuiScenarioErrorCode::invalid_scenario);
  REQUIRE(calls == 0);

  invalid = scenario();
  invalid.limits.maximum_script_bytes = 4;
  const auto oversized = testing::run_tui_scenario(invalid, never);
  REQUIRE_FALSE(oversized);
  REQUIRE(oversized.error().code ==
          testing::TuiScenarioErrorCode::resource_limit);
  REQUIRE(calls == 0);

  invalid = scenario();
  invalid.limits.maximum_trace_bytes = 1;
  const auto bounded_trace = testing::run_tui_scenario(invalid, factory());
  REQUIRE_FALSE(bounded_trace);
  REQUIRE(bounded_trace.error().code ==
          testing::TuiScenarioErrorCode::resource_limit);
}

TEST_CASE(
    "TUI scenarios record and replay semantic input resize and fake steps",
    "[scenario]") {
  const auto result = testing::run_tui_scenario(scenario(), factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state.find("aBRTq") != std::string::npos);
  REQUIRE(result->recorded.semantic_state.find("12x3") != std::string::npos);
  REQUIRE(result->fake_script_digest.starts_with("fnv1a64:"));
  REQUIRE(result->trace_digest.starts_with("fnv1a64:"));
  REQUIRE(result->corpus_version == "1");
  REQUIRE_FALSE(result->trace.empty());
  REQUIRE_FALSE(result->recorded.normalized_frames.empty());
}

TEST_CASE("interactive chat records and replays a gated backend stream",
          "[scenario][chat][stream]") {
  const auto result =
      testing::run_tui_scenario(chat_scenario(), chat_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state.starts_with(
      "Usage summary is derived from session events"));
  REQUIRE(std::ranges::any_of(result->recorded.normalized_frames,
                              [](const std::string& frame) {
                                return frame.find("hello") != std::string::npos;
                              }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("usage 3 in/2 out/1 cached/1 reasoning") !=
                   std::string::npos &&
               frame.find("reported 0 USD + 0.0645375 venice.diem") !=
                   std::string::npos &&
               frame.find("estimated 0.0000065 USD 0.0000065 venice.diem") !=
                   std::string::npos;
      }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("Session usage, cost, and spend ceiling") !=
                   std::string::npos &&
               frame.find("Input tokens: 3") != std::string::npos &&
               frame.find("Cached input tokens: 1") != std::string::npos &&
               frame.find("Reasoning tokens: 1") != std::string::npos &&
               frame.find("1 completed") != std::string::npos &&
               frame.find(
                   "Reported cost: 0 USD + 0.0645375 venice.diem (1 of 1") !=
                   std::string::npos &&
               frame.find("Catalog estimate (USD): 0.0000065 USD") !=
                   std::string::npos;
      }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.starts_with("32x6:") &&
               frame.find("usage 3 in/2 out") == std::string::npos;
      }));
}

TEST_CASE("interactive chat cancellation replay preserves cross-thread order",
          "[scenario][chat][cancellation]") {
  const std::vector<std::pair<std::string, termforge::KeyEvent>> cases{
      {"interactive-chat-escape-cancellation",
       {termforge::Key::Escape, 0, false, false, false,
        termforge::KeyAction::Press}},
      {"interactive-chat-control-c-cancellation",
       {termforge::Key::Char, U'c', true, false, false,
        termforge::KeyAction::Press}},
  };
  for (const auto& [scenario_id, cancellation] : cases) {
    const auto result = testing::run_tui_scenario(
        chat_cancellation_scenario(scenario_id, cancellation), chat_factory());
    INFO((result ? std::string{} : result.error().message));
    REQUIRE(result);
    REQUIRE(result->recorded == result->replayed);
    REQUIRE(result->recorded.semantic_state.find(":cancelled") !=
            std::string::npos);
    REQUIRE(std::ranges::any_of(
        result->recorded.normalized_frames, [](const std::string& frame) {
          return frame.find("cancelled") != std::string::npos;
        }));
  }
}

TEST_CASE("idle control keys clear drafts without durable history and exit",
          "[scenario][chat][controls]") {
  const auto result =
      testing::run_tui_scenario(idle_control_scenario(), chat_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state == "Draft cleared");
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("Draft cleared") != std::string::npos;
      }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("Draft is already empty") != std::string::npos;
      }));
  const auto multiline = std::ranges::find_if(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.starts_with("48x5:") &&
               frame.find("epsilon") != std::string::npos;
      });
  REQUIRE(multiline != result->recorded.normalized_frames.end());
  REQUIRE(std::ranges::any_of(
      std::next(multiline), result->recorded.normalized_frames.end(),
      [](const std::string& frame) {
        return frame.starts_with("48x5:") &&
               frame.find("Ctrl+C clear") != std::string::npos &&
               frame.find("alpha") == std::string::npos &&
               frame.find("epsilon") == std::string::npos;
      }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("after escape") != std::string::npos;
      }));
}

TEST_CASE("help Escape closes the panel and idle Ctrl+D exits cleanly",
          "[scenario][chat][controls][help]") {
  const auto result =
      testing::run_tui_scenario(help_control_scenario(), chat_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state == "Ready");
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("Slash command help") != std::string::npos &&
               frame.find("Esc closes") != std::string::npos;
      }));
}

TEST_CASE("interactive usage keeps absent cost explicit", "[scenario][usage]") {
  const auto result =
      testing::run_tui_scenario(empty_usage_scenario(), chat_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("Input tokens: 0") != std::string::npos &&
               frame.find("Inferences: 0 total") != std::string::npos &&
               frame.find("Reported cost: unavailable (no inferences)") !=
                   std::string::npos;
      }));
}

TEST_CASE("interactive plan and task views remain bounded without state",
          "[scenario][plan][tasks]") {
  const auto result =
      testing::run_tui_scenario(empty_plan_tasks_scenario(), chat_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("This session has no plan") != std::string::npos;
      }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("Active session tasks") != std::string::npos &&
               frame.find("Project backlog") != std::string::npos;
      }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames,
      [](const std::string& frame) { return frame.starts_with("24x5:"); }));
}

TEST_CASE("active Ctrl+D neither exits nor cancels the run",
          "[scenario][chat][controls][cancellation]") {
  const auto result =
      testing::run_tui_scenario(active_control_d_scenario(), chat_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state.starts_with("Ready"));
  REQUIRE(result->recorded.semantic_state.find(":cancelled") ==
          std::string::npos);
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("Ctrl+D is unavailable while a run is active") !=
               std::string::npos;
      }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.starts_with("48x5:") &&
               frame.find("Esc/Ctrl+C cancel | Ctrl+D") != std::string::npos;
      }));
}

TEST_CASE("interactive composer cursor survives fallback lifecycle boundaries",
          "[scenario][chat][cursor][fallback]") {
  const auto result =
      testing::run_tui_scenario(cursor_lifecycle_scenario(), chat_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state.find("|pending-edit") !=
          std::string::npos);

  const auto has_frame = [&](const auto& predicate) {
    return std::ranges::any_of(result->recorded.normalized_frames, predicate);
  };
  REQUIRE(has_frame([](const std::string& frame) {
    return frame.starts_with("8x4:") &&
           frame.find("abcd") != std::string::npos &&
           frame.find("@reverse=4,2") != std::string::npos;
  }));
  REQUIRE(has_frame([](const std::string& frame) {
    return frame.starts_with("8x4:") &&
           frame.find("@reverse=3,2") != std::string::npos;
  }));
  REQUIRE(has_frame([](const std::string& frame) {
    return frame.starts_with("4x4:") &&
           frame.find("@reverse=0,2") != std::string::npos;
  }));
  REQUIRE(has_frame([](const std::string& frame) {
    return frame.starts_with("1x1:") &&
           frame.find("@reverse=0,0") != std::string::npos;
  }));
  REQUIRE(has_frame([](const std::string& frame) {
    return frame.starts_with("8x2:tiny") &&
           frame.find("@reverse=4,0") != std::string::npos;
  }));
  REQUIRE(has_frame([](const std::string& frame) {
    return frame.find("Slash command help") != std::string::npos &&
           frame.find("@reverse=0,4") != std::string::npos;
  }));
  REQUIRE(has_frame([](const std::string& frame) {
    return frame.find("Running") != std::string::npos &&
           frame.find("@reverse=") == std::string::npos;
  }));
  REQUIRE(has_frame([](const std::string& frame) {
    return frame.find("prompt") != std::string::npos &&
           frame.find("@reverse=6,4") != std::string::npos;
  }));
  REQUIRE(has_frame([](const std::string& frame) {
    return frame.find("prompt") != std::string::npos &&
           frame.find("@reverse=2,4") != std::string::npos;
  }));
  REQUIRE(result->recorded.wire_output.find("\033[7m") != std::string::npos);
  REQUIRE(result->recorded.wire_output.find("\033[0m") != std::string::npos);
}

TEST_CASE("interactive composer yields cursor focus to modal overlays",
          "[scenario][chat][cursor][modal]") {
  const auto result =
      testing::run_tui_scenario(cursor_modal_scenario(), chat_factory(true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state == "Model selection cancelled");
  const auto modal_frame = std::ranges::find_if(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("@reverse=") == std::string::npos;
      });
  REQUIRE(modal_frame != result->recorded.normalized_frames.begin());
  REQUIRE(modal_frame != result->recorded.normalized_frames.end());
  REQUIRE(std::prev(modal_frame)->find("@reverse=0,4") != std::string::npos);
  REQUIRE(std::ranges::any_of(
      std::next(modal_frame), result->recorded.normalized_frames.end(),
      [](const std::string& frame) {
        return frame.find("@reverse=0,4") != std::string::npos;
      }));
}

TEST_CASE("startup model selection is keyboard-only and resize deterministic",
          "[scenario][models][startup]") {
  for (const auto origin :
       {model::CatalogOrigin::live, model::CatalogOrigin::fresh_cache,
        model::CatalogOrigin::stale_cache}) {
    const auto result = testing::run_tui_scenario(
        startup_model_scenario(false), startup_model_factory(origin));
    INFO((result ? std::string{} : result.error().message));
    REQUIRE(result);
    REQUIRE(result->recorded == result->replayed);
    REQUIRE(result->recorded.semantic_state == "selected:alternate");
    REQUIRE(result->recorded.wire_output.find("Select model") !=
            std::string::npos);
    const auto expected = origin == model::CatalogOrigin::live
                              ? "Catalog: live"
                              : (origin == model::CatalogOrigin::fresh_cache
                                     ? "Catalog: fresh cache"
                                     : "Catalog: stale cache");
    REQUIRE(result->recorded.wire_output.find("tools false") !=
            std::string::npos);
    REQUIRE(result->recorded.wire_output.find(expected) != std::string::npos);
  }
}

TEST_CASE("startup model cancellation creates no session or backend surface",
          "[scenario][models][startup][cancel]") {
  const auto result = testing::run_tui_scenario(startup_model_scenario(true),
                                                startup_model_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state == "cancelled");
}

TEST_CASE("interactive session actions list resume and create atomically",
          "[scenario][session]") {
  const auto result = testing::run_tui_scenario(session_success_scenario(),
                                                session_factory(true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state == "Started session session-3");
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("target-session") != std::string::npos &&
               frame.find("runs 7") != std::string::npos;
      }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("Session is already current") != std::string::npos;
      }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("AIForge  session target-session") !=
                   std::string::npos &&
               frame.find("usage 12 in/7 out/2 cached/1 reasoning") !=
                   std::string::npos &&
               frame.find("reported 0 USD + 0.0645375 venice.diem (partial)") !=
                   std::string::npos;
      }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("AIForge  session session-3") != std::string::npos;
      }));
}

TEST_CASE("failed session operations preserve the current interactive app",
          "[scenario][session][failure]") {
  struct FailureCase {
    std::string scenario_id;
    std::string command;
    bool fail_listing{};
    std::string status;
  };
  const std::vector<FailureCase> cases{
      {"interactive-session-missing-failure", "/session resume missing", false,
       "durable session could not be opened"},
      {"interactive-session-replay-failure", "/session resume corrupt-session",
       false, "durable session could not be opened"},
      {"interactive-session-usage-overflow",
       "/session resume usage-overflow-session", false,
       "Interactive usage replay failed: usage ledger total overflow"},
      {"interactive-session-list-failure", "/session list", true,
       "Sessions could not be listed: session catalog is contended"},
  };
  for (const auto& failure : cases) {
    const auto result = testing::run_tui_scenario(
        session_failure_scenario(failure.scenario_id, failure.command),
        session_factory(true, failure.fail_listing));
    INFO((result ? std::string{} : result.error().message));
    REQUIRE(result);
    REQUIRE(result->recorded == result->replayed);
    REQUIRE(result->recorded.semantic_state == failure.status);
    REQUIRE(std::ranges::any_of(
        result->recorded.normalized_frames, [](const std::string& frame) {
          return frame.find("AIForge  session session-1") != std::string::npos;
        }));
  }
}

TEST_CASE("ephemeral session actions explain and preserve ephemerality",
          "[scenario][session][ephemeral]") {
  const auto result = testing::run_tui_scenario(ephemeral_session_scenario(),
                                                session_factory(false));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state == "Started session session-2");
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("Current session is ephemeral") != std::string::npos;
      }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("AIForge  session session-2 (ephemeral)") !=
               std::string::npos;
      }));
}

TEST_CASE("modal wizard replay survives tiny-terminal resize",
          "[scenario][modal][resize]") {
  const auto result =
      testing::run_tui_scenario(modal_scenario(), modal_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state.starts_with("submitted"));
  REQUIRE(std::ranges::any_of(result->recorded.normalized_frames,
                              [](const std::string& frame) {
                                return frame.find("8x3") != std::string::npos;
                              }));
}

TEST_CASE(
    "TUI scenario replay rejects malformed traces and capability mismatch",
    "[scenario][failure][trace]") {
  const auto recorded = testing::run_tui_scenario(scenario(), factory());
  INFO((recorded ? std::string{} : recorded.error().message));
  REQUIRE(recorded);
  auto truncated = recorded->trace;
  truncated.resize(truncated.size() / 2);
  const auto malformed =
      testing::replay_tui_scenario(scenario(), truncated, factory());
  REQUIRE_FALSE(malformed);
  REQUIRE(malformed.error().code ==
          testing::TuiScenarioErrorCode::trace_failure);

  const auto incompatible = testing::replay_tui_scenario(
      scenario(), recorded->trace, factory(false, true));
  REQUIRE_FALSE(incompatible);
  REQUIRE(incompatible.error().code ==
          testing::TuiScenarioErrorCode::trace_failure);

  auto changed_corpus = scenario();
  changed_corpus.corpus_version = "2";
  const auto stale =
      testing::replay_tui_scenario(changed_corpus, *recorded, factory());
  REQUIRE_FALSE(stale);
  REQUIRE(stale.error().code ==
          testing::TuiScenarioErrorCode::provenance_mismatch);

  auto tampered = *recorded;
  tampered.trace += "x";
  const auto invalid_identity =
      testing::replay_tui_scenario(scenario(), tampered, factory());
  REQUIRE_FALSE(invalid_identity);
  REQUIRE(invalid_identity.error().code ==
          testing::TuiScenarioErrorCode::trace_failure);
}

TEST_CASE("TUI scenarios fail on replay divergence and frame exhaustion",
          "[scenario][failure]") {
  const auto diverged =
      testing::run_tui_scenario(scenario(), factory(true, false));
  INFO((diverged ? std::string{} : diverged.error().message));
  REQUIRE_FALSE(diverged);
  REQUIRE(diverged.error().code ==
          testing::TuiScenarioErrorCode::replay_diverged);

  auto unbounded = scenario();
  unbounded.steps.pop_back();
  unbounded.limits.maximum_frames = 6;
  const auto exhausted = testing::run_tui_scenario(unbounded, factory());
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().code ==
          testing::TuiScenarioErrorCode::resource_limit);

  auto mismatched_factory = factory();
  auto mismatched_target = [mismatched_factory](testing::TuiScenarioPass pass,
                                                termforge::ByteSink* output)
      -> std::expected<testing::TuiScenarioTarget, testing::TuiScenarioError> {
    auto target = mismatched_factory(pass, output);
    if (!target) return target;
    target->release_backend_step =
        [](std::string_view) -> std::expected<void, std::string> {
      return std::unexpected("descriptor mismatch");
    };
    return target;
  };
  const auto mismatched =
      testing::run_tui_scenario(scenario(), mismatched_target);
  REQUIRE_FALSE(mismatched);
  REQUIRE(mismatched.error().code ==
          testing::TuiScenarioErrorCode::target_failure);
}
