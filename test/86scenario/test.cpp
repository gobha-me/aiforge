#include <unistd.h>

#include <aiforge/adapters/interactive_chat_app.hpp>
#include <aiforge/adapters/provider_character_picker_dialog.hpp>
#include <aiforge/backend/provider_character_catalog.hpp>
#include <aiforge/model/catalog.hpp>
#include <aiforge/runtime/ask_user_tool.hpp>
#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/testing/scripted_persona_editor.hpp>
#include <aiforge/testing/scripted_persona_source.hpp>
#include <aiforge/testing/scripted_tool_executor.hpp>
#include <aiforge/testing/tui_scenario.hpp>
#include <algorithm>
#include <array>
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
    current.context_window_tokens = 8192;
    model::CatalogEntry alternate{make_id<domain::ModelId>("alternate"),
                                  "text"};
    alternate.name = "Alternate model";
    alternate.context_window_tokens = 8192;
    model::CatalogEntry offline{make_id<domain::ModelId>("offline"), "text"};
    offline.name = "Offline model";
    offline.context_window_tokens = 8192;
    offline.offline = true;
    return model::CatalogSnapshot{
        std::chrono::sys_time<std::chrono::milliseconds>{123ms},
        {std::move(current), std::move(alternate), std::move(offline)}};
  }
};

class ScenarioProviderCharacterSource final
    : public backend::ProviderCharacterCatalogSource {
 public:
  explicit ScenarioProviderCharacterSource(const bool drift = false)
      : m_drift(drift) {}

  auto list(backend::ProviderCharacterLimits, std::stop_token)
      -> std::expected<backend::ProviderCharacterCatalog,
                       backend::ProviderCharacterError> override {
    ++m_list_calls;
    auto alan = summary("alan-watts", "model");
    alan.name = "Alan Watts";
    alan.description = "Philosophical conversation";
    alan.tags = {"philosophy", "featured"};
    auto incompatible = summary("other-model", "alternate");
    auto offline = summary("offline-guide", "offline");
    backend::ProviderCharacterSummary missing{
        make_id<domain::ProviderCharacterId>("missing-model")};
    return backend::ProviderCharacterCatalog{
        {std::move(alan), std::move(incompatible), std::move(offline),
         std::move(missing)},
        "scenario.characters"};
  }

  auto lookup(const domain::ProviderCharacterId& id,
              backend::ProviderCharacterLimits, std::stop_token)
      -> std::expected<backend::ProviderCharacterSummary,
                       backend::ProviderCharacterError> override {
    ++m_lookup_calls;
    if (id.value() != "alan-watts") {
      return std::unexpected(backend::ProviderCharacterError{
          backend::ProviderCharacterErrorCode::not_found,
          "character disappeared", false, 404});
    }
    return summary("alan-watts", m_drift ? "alternate" : "model");
  }

  [[nodiscard]] auto list_calls() const noexcept -> std::size_t {
    return m_list_calls;
  }
  [[nodiscard]] auto lookup_calls() const noexcept -> std::size_t {
    return m_lookup_calls;
  }

 private:
  [[nodiscard]] static auto summary(const std::string& id,
                                    const std::string& model_id)
      -> backend::ProviderCharacterSummary {
    backend::ProviderCharacterSummary result{
        make_id<domain::ProviderCharacterId>(id)};
    result.model_id = make_id<domain::ModelId>(model_id);
    return result;
  }

  bool m_drift{};
  std::size_t m_list_calls{};
  std::size_t m_lookup_calls{};
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
    const auto reasoning = make_id<domain::SessionId>("reasoning-session");
    const auto reasoning_inference =
        make_id<domain::InferenceId>("reasoning-inference");
    const auto reasoning_message =
        make_id<domain::MessageId>("reasoning-message");
    m_sessions.emplace(reasoning, storage::SessionInfo{
                                      reasoning, domain::EventTimestamp{600ms},
                                      domain::EventTimestamp{4000ms}, 8, 1});
    m_histories[reasoning] = {
        run_event(
            1, "reasoning-start", "reasoning-run",
            domain::RunStarted{surface, workspace, profile, std::nullopt}),
        run_event(2, "reasoning-inference", "reasoning-run",
                  domain::InferenceStarted{reasoning_inference, model}),
        run_event(3, "reasoning-message", "reasoning-run",
                  domain::AssistantContentStarted{reasoning_message,
                                                  reasoning_inference}),
        run_event(4, "reasoning-delta", "reasoning-run",
                  domain::ReasoningMetadataAdded{
                      reasoning_inference,
                      std::string{"literal **reasoning**"},
                      {{"application/private", "never-render-this"}}}),
        run_event(5, "reasoning-answer", "reasoning-run",
                  domain::AssistantContentDeltaAdded{
                      reasoning_message, reasoning_inference,
                      domain::TextBlock{"durable answer"}}),
        run_event(6, "reasoning-content-finished", "reasoning-run",
                  domain::AssistantContentFinished{reasoning_message,
                                                   reasoning_inference}),
        run_event(7, "reasoning-finished", "reasoning-run",
                  domain::InferenceFinished{reasoning_inference,
                                            domain::FinishReason::stop}),
        run_event(8, "reasoning-completed", "reasoning-run",
                  domain::RunCompleted{}),
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
    ++m_append_calls;
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

  [[nodiscard]] auto append_calls() const noexcept -> std::size_t {
    return m_append_calls;
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
  std::size_t m_append_calls{};
  std::map<domain::SessionId, storage::SessionInfo> m_sessions;
  std::map<domain::SessionId, std::vector<domain::RunEvent>> m_histories;
};

class GatedBackendState final {
 public:
  auto set_web_search_support(std::optional<bool> support) -> void {
    std::lock_guard lock{m_mutex};
    m_web_search_support = support;
  }

  auto model_capabilities() -> backend::ModelCapabilityMap {
    std::lock_guard lock{m_mutex};
    backend::ModelCapabilityMap result;
    if (m_web_search_support) {
      result.emplace("web-search", m_web_search_support);
    }
    return result;
  }

  auto initialize(const backend::BackendRequest& request) -> bool {
    std::lock_guard lock{m_mutex};
    ++m_initialize_calls;
    if (m_initialized) return false;
    m_request = request;
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

  auto initialize_calls() -> std::size_t {
    std::lock_guard lock{m_mutex};
    return m_initialize_calls;
  }

  auto captured_request() -> std::optional<backend::BackendRequest> {
    std::lock_guard lock{m_mutex};
    return m_request;
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
  std::size_t m_initialize_calls{};
  std::optional<backend::BackendRequest> m_request;
  bool m_initialized{};
  bool m_ended{};
  bool m_cancelled{};
  bool m_cancel_end_waiting{};
  bool m_cancel_end_released{};
  std::optional<bool> m_web_search_support;
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
                                     pricing_observation(),
                                     m_state->model_capabilities()};
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

class QuestionBackendState final {
 public:
  QuestionBackendState() {
    const auto invocation = make_id<domain::InvocationId>("ask-call");
    m_events = {
        backend::BackendEvent{backend::ResponseStarted{"question-response"}},
        backend::BackendEvent{backend::ToolCallDelta{
            invocation, "ask_user",
            R"({"questions":[{"id":"format","prompt":"Choose output","kind":"one","required":true,"minimum_selections":1,"maximum_selections":1,"options":[{"id":"short","label":"Short","recommended":true},{"id":"long","label":"Long"}]}]})"}},
        backend::BackendEvent{
            backend::ResponseFinished{domain::FinishReason::tool_call}},
        std::nullopt,
        backend::BackendEvent{backend::ResponseStarted{"answer-response"}},
        backend::BackendEvent{backend::ContentDelta{
            make_id<domain::MessageId>("continuation-assistant"),
            domain::TextBlock{"continued answer"}}},
        backend::BackendEvent{
            backend::ResponseFinished{domain::FinishReason::stop}},
        std::nullopt,
    };
  }

  auto start(backend::BackendRequest request)
      -> std::expected<std::size_t, backend::BackendError> {
    std::lock_guard lock{m_mutex};
    if (m_requests.size() >= 2) {
      return std::unexpected(backend::BackendError{
          backend::BackendErrorKind::script_exhausted,
          "question backend was started too many times", false, std::nullopt});
    }
    if (m_requests.empty()) {
      if (request.tools.size() != 1 ||
          request.tools.front().name != "ask_user") {
        m_valid = false;
      }
    } else {
      m_events[5] = backend::BackendEvent{backend::ContentDelta{
          request.assistant_message_id, domain::TextBlock{"continued answer"}}};
      if (request.tools != m_requests.front().tools ||
          !std::ranges::any_of(request.context.entries, [](const auto& entry) {
            return entry.kind == domain::ContextEntryKind::tool_result;
          })) {
        m_valid = false;
      }
    }
    const auto offset = m_requests.size() * 4U;
    m_requests.push_back(std::move(request));
    m_condition.notify_all();
    return offset;
  }

  auto next(const std::size_t offset, std::size_t& local_index,
            std::stop_token token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> {
    std::unique_lock lock{m_mutex};
    const auto index = offset + local_index;
    ++m_waiting_calls;
    m_condition.notify_all();
    if (!m_condition.wait(lock, token, [&] { return m_released > index; })) {
      return backend::BackendEvent{backend::ResponseCancelled{"cancelled"}};
    }
    auto result = m_events.at(index);
    ++local_index;
    if (!result) ++m_ended_streams;
    m_condition.notify_all();
    return result;
  }

  auto release(const std::string_view description, const bool wait_for_wake)
      -> std::expected<void, std::string> {
    static const std::array expected{
        std::string_view{"question-started"},
        std::string_view{"question-call"},
        std::string_view{"question-finished"},
        std::string_view{"question-end"},
        std::string_view{"continuation-started"},
        std::string_view{"continuation-delta"},
        std::string_view{"continuation-finished"},
        std::string_view{"continuation-end"},
    };
    std::unique_lock lock{m_mutex};
    if (!m_condition.wait_for(lock, 1s,
                              [&] { return m_waiting_calls > m_released; })) {
      return std::unexpected("question backend did not reach its boundary");
    }
    if (m_released >= expected.size() || description != expected[m_released]) {
      return std::unexpected("question backend descriptor mismatch");
    }
    ++m_released;
    const auto released = m_released;
    m_condition.notify_all();
    const bool ended = released == 4 || released == 8;
    const auto expected_ended = released == 4 ? 1U : 2U;
    if (!m_condition.wait_for(lock, 1s, [&] {
          return ended ? m_ended_streams >= expected_ended
                       : m_waiting_calls >= released + 1U;
        })) {
      return std::unexpected("question backend did not acknowledge its step");
    }
    if (wait_for_wake &&
        !m_condition.wait_for(lock, 1s, [&] { return m_wakes >= released; })) {
      return std::unexpected("question backend update was not posted");
    }
    return {};
  }

  auto observe_wake() -> void {
    std::lock_guard lock{m_mutex};
    ++m_wakes;
    m_condition.notify_all();
  }

  [[nodiscard]] auto semantic_state() -> std::string {
    std::lock_guard lock{m_mutex};
    return "requests=" + std::to_string(m_requests.size()) +
           (m_valid ? "|valid" : "|invalid");
  }

 private:
  std::mutex m_mutex;
  std::condition_variable_any m_condition;
  std::vector<std::optional<backend::BackendEvent>> m_events;
  std::vector<backend::BackendRequest> m_requests;
  std::size_t m_waiting_calls{};
  std::size_t m_released{};
  std::size_t m_wakes{};
  std::size_t m_ended_streams{};
  bool m_valid{true};
};

class QuestionStream final : public backend::BackendStream {
 public:
  QuestionStream(std::shared_ptr<QuestionBackendState> state,
                 const std::size_t offset)
      : m_state(std::move(state)), m_offset(offset) {}

  auto next(std::stop_token token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    return m_state->next(m_offset, m_index, token);
  }

 private:
  std::shared_ptr<QuestionBackendState> m_state;
  std::size_t m_offset{};
  std::size_t m_index{};
};

class QuestionBackend final : public backend::Backend,
                              public backend::ModelContextProvider {
 public:
  explicit QuestionBackend(std::shared_ptr<QuestionBackendState> state)
      : m_state(std::move(state)) {}

  auto lookup(const domain::ModelId& model_id, std::stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    return backend::ModelContextInfo{
        model_id, 8192, 1024, pricing_observation(),
        backend::ModelCapabilityMap{{"tools", true}}};
  }

  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    auto offset = m_state->start(std::move(request));
    if (!offset) return std::unexpected(std::move(offset.error()));
    return std::make_unique<QuestionStream>(m_state, *offset);
  }

 private:
  std::shared_ptr<QuestionBackendState> m_state;
};

class ApprovalBackendState final {
 public:
  ApprovalBackendState() {
    const auto invocation = make_id<domain::InvocationId>("approval-call");
    m_events = {
        backend::BackendEvent{backend::ResponseStarted{"approval-response"}},
        backend::BackendEvent{
            backend::ToolCallDelta{invocation, "read_repository_file",
                                   R"({"relative_path":"README.md"})"}},
        backend::BackendEvent{
            backend::ResponseFinished{domain::FinishReason::tool_call}},
        std::nullopt,
        backend::BackendEvent{
            backend::ResponseStarted{"approval-continuation-response"}},
        backend::BackendEvent{backend::ContentDelta{
            make_id<domain::MessageId>("approval-continuation-assistant"),
            domain::TextBlock{"continued after approval"}}},
        backend::BackendEvent{
            backend::ResponseFinished{domain::FinishReason::stop}},
        std::nullopt,
    };
  }

  auto start(backend::BackendRequest request)
      -> std::expected<std::size_t, backend::BackendError> {
    std::lock_guard lock{m_mutex};
    if (m_requests.size() >= 2) {
      return std::unexpected(backend::BackendError{
          backend::BackendErrorKind::script_exhausted,
          "approval backend was started too many times", false, std::nullopt});
    }
    if (request.tools.size() != 1 ||
        request.tools.front().name != "read_repository_file") {
      m_valid = false;
    }
    if (!m_requests.empty()) {
      m_events[5] = backend::BackendEvent{
          backend::ContentDelta{request.assistant_message_id,
                                domain::TextBlock{"continued after approval"}}};
      if (request.tools != m_requests.front().tools ||
          !std::ranges::any_of(request.context.entries, [](const auto& entry) {
            return entry.kind == domain::ContextEntryKind::tool_result;
          })) {
        m_valid = false;
      }
    }
    const auto offset = m_requests.size() * 4U;
    m_requests.push_back(std::move(request));
    m_condition.notify_all();
    return offset;
  }

  auto next(const std::size_t offset, std::size_t& local_index,
            std::stop_token token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> {
    std::unique_lock lock{m_mutex};
    const auto index = offset + local_index;
    ++m_waiting_calls;
    m_condition.notify_all();
    if (!m_condition.wait(lock, token, [&] { return m_released > index; })) {
      return backend::BackendEvent{backend::ResponseCancelled{"cancelled"}};
    }
    auto result = m_events.at(index);
    ++local_index;
    if (!result) ++m_ended_streams;
    m_condition.notify_all();
    return result;
  }

  auto release(const std::string_view description, const bool wait_for_wake)
      -> std::expected<void, std::string> {
    static const std::array expected{
        std::string_view{"approval-started"},
        std::string_view{"approval-call"},
        std::string_view{"approval-finished"},
        std::string_view{"approval-end"},
        std::string_view{"continuation-started"},
        std::string_view{"continuation-delta"},
        std::string_view{"continuation-finished"},
        std::string_view{"continuation-end"},
    };
    std::unique_lock lock{m_mutex};
    if (!m_condition.wait_for(lock, 1s,
                              [&] { return m_waiting_calls > m_released; })) {
      return std::unexpected("approval backend did not reach boundary " +
                             std::string{description} +
                             " (released=" + std::to_string(m_released) +
                             ", waiting=" + std::to_string(m_waiting_calls) +
                             ", requests=" + std::to_string(m_requests.size()) +
                             ", wakes=" + std::to_string(m_wakes) + ")");
    }
    if (m_released >= expected.size() || description != expected[m_released]) {
      return std::unexpected("approval backend descriptor mismatch");
    }
    ++m_released;
    const auto released = m_released;
    m_condition.notify_all();
    const bool ended = released == 4 || released == 8;
    const auto expected_ended = released == 4 ? 1U : 2U;
    if (!m_condition.wait_for(lock, 1s, [&] {
          return ended ? m_ended_streams >= expected_ended
                       : m_waiting_calls >= released + 1U;
        })) {
      return std::unexpected(
          "approval backend did not acknowledge " + std::string{description} +
          " (released=" + std::to_string(m_released) +
          ", waiting=" + std::to_string(m_waiting_calls) +
          ", ended=" + std::to_string(m_ended_streams) + ")");
    }
    if (wait_for_wake &&
        !m_condition.wait_for(lock, 1s, [&] { return m_wakes >= released; })) {
      return std::unexpected("approval backend update was not posted");
    }
    return {};
  }

  auto observe_wake() -> void {
    std::lock_guard lock{m_mutex};
    ++m_wakes;
    m_condition.notify_all();
  }

  [[nodiscard]] auto semantic_state() -> std::string {
    std::lock_guard lock{m_mutex};
    return "requests=" + std::to_string(m_requests.size()) +
           (m_valid ? "|valid" : "|invalid");
  }

 private:
  std::mutex m_mutex;
  std::condition_variable_any m_condition;
  std::vector<std::optional<backend::BackendEvent>> m_events;
  std::vector<backend::BackendRequest> m_requests;
  std::size_t m_waiting_calls{};
  std::size_t m_released{};
  std::size_t m_wakes{};
  std::size_t m_ended_streams{};
  bool m_valid{true};
};

class ApprovalStream final : public backend::BackendStream {
 public:
  ApprovalStream(std::shared_ptr<ApprovalBackendState> state,
                 const std::size_t offset)
      : m_state(std::move(state)), m_offset(offset) {}

  auto next(std::stop_token token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    return m_state->next(m_offset, m_index, token);
  }

 private:
  std::shared_ptr<ApprovalBackendState> m_state;
  std::size_t m_offset{};
  std::size_t m_index{};
};

class ApprovalBackend final : public backend::Backend,
                              public backend::ModelContextProvider {
 public:
  explicit ApprovalBackend(std::shared_ptr<ApprovalBackendState> state)
      : m_state(std::move(state)) {}

  auto lookup(const domain::ModelId& model_id, std::stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    return backend::ModelContextInfo{
        model_id, 8192, 1024, pricing_observation(),
        backend::ModelCapabilityMap{{"tools", true}}};
  }

  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    auto offset = m_state->start(std::move(request));
    if (!offset) return std::unexpected(std::move(offset.error()));
    return std::make_unique<ApprovalStream>(m_state, *offset);
  }

 private:
  std::shared_ptr<ApprovalBackendState> m_state;
};

class ApprovalToolExecutor final : public runtime::ToolExecutor {
 public:
  ApprovalToolExecutor(domain::StructuredDataBlock expected_arguments,
                       domain::CapabilityScope requested_scope,
                       std::vector<testing::ScriptedToolExchange> exchanges)
      : m_expected_arguments(std::move(expected_arguments)),
        m_requested_scope(std::move(requested_scope)),
        m_script(std::move(exchanges)) {}

  [[nodiscard]] auto validate(const domain::StructuredDataBlock& arguments)
      const -> std::expected<runtime::ValidatedToolArguments,
                             runtime::ToolExecutionError> override {
    if (arguments != m_expected_arguments) {
      return std::unexpected(runtime::ToolExecutionError{
          runtime::ToolExecutionErrorCode::invalid_arguments,
          "approval scenario received unexpected arguments", false});
    }
    return runtime::ValidatedToolArguments{
        arguments, {m_requested_scope}, {domain::Effect::read}};
  }

  [[nodiscard]] auto start(runtime::ToolInvocation invocation,
                           std::stop_token stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    return m_script.start(std::move(invocation), stop_token);
  }

  [[nodiscard]] auto recorded_invocations() const noexcept
      -> const std::vector<runtime::ToolInvocation>& {
    return m_script.recorded_invocations();
  }

  [[nodiscard]] auto remaining_exchanges() const noexcept -> std::size_t {
    return m_script.remaining_exchanges();
  }

 private:
  domain::StructuredDataBlock m_expected_arguments;
  domain::CapabilityScope m_requested_scope;
  testing::ScriptedToolExecutor m_script;
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

auto question_scenario(const bool interrupt = false) -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = interrupt ? "interactive-ask-user-interrupt"
                                : "interactive-ask-user-answer";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 14, 1000, 280};
  value.backend_script = {"question-started", "question-call",
                          "question-finished", "question-end"};
  if (!interrupt) {
    value.backend_script.insert(value.backend_script.end(),
                                {"continuation-started", "continuation-delta",
                                 "continuation-finished", "continuation-end"});
  }
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
  };
  if (interrupt) {
    value.steps.push_back({6, testing::TuiScenarioPost{termforge::KeyEvent{
                                  termforge::Key::Char, U'c', true, false,
                                  false, termforge::KeyAction::Press}}});
    value.steps.push_back(
        {8, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}});
    value.steps.push_back({8, enter});
  } else {
    value.steps.push_back({6, enter});
    for (std::uint64_t frame = 7; frame < 11; ++frame) {
      value.steps.push_back(
          {frame,
           testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}});
    }
    value.steps.push_back(
        {13, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}});
    value.steps.push_back({13, enter});
  }
  value.limits.maximum_frames = 48;
  return value;
}

auto question_cancel_scenario() -> testing::TuiScenario {
  auto value = question_scenario();
  value.scenario_id = "interactive-ask-user-cancel";
  value.steps.at(6).action = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Escape, 0, false, false, false,
                          termforge::KeyAction::Press}};
  return value;
}

enum class ApprovalScenarioAction {
  allow_once,
  deny,
  dismiss,
  interrupt,
};

auto approval_scenario(const ApprovalScenarioAction action)
    -> testing::TuiScenario {
  testing::TuiScenario value;
  switch (action) {
    case ApprovalScenarioAction::allow_once:
      value.scenario_id = "interactive-tool-approval-allow-once";
      break;
    case ApprovalScenarioAction::deny:
      value.scenario_id = "interactive-tool-approval-default-deny";
      break;
    case ApprovalScenarioAction::dismiss:
      value.scenario_id = "interactive-tool-approval-dismiss";
      break;
    case ApprovalScenarioAction::interrupt:
      value.scenario_id = "interactive-tool-approval-interrupt";
      break;
  }
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 24, 1000, 480};
  value.backend_script = {"approval-started", "approval-call",
                          "approval-finished", "approval-end"};
  if (action != ApprovalScenarioAction::interrupt) {
    value.backend_script.insert(value.backend_script.end(),
                                {"continuation-started", "continuation-delta",
                                 "continuation-finished", "continuation-end"});
  }
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{
              "/tools profile repository-read"}}},
      {0, enter},
      {2, testing::TuiScenarioPost{termforge::PasteEvent{"read file"}}},
      {2, enter},
      {3, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {4, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {5, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {6, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {8, testing::TuiScenarioResize{{12, 4, 120, 80}}},
      {9, testing::TuiScenarioResize{{100, 24, 1000, 480}}},
  };
  switch (action) {
    case ApprovalScenarioAction::allow_once:
      value.steps.push_back({11, testing::TuiScenarioPost{termforge::KeyEvent{
                                     termforge::Key::Down, 0, false, false,
                                     false, termforge::KeyAction::Press}}});
      value.steps.push_back({11, enter});
      break;
    case ApprovalScenarioAction::deny:
      value.steps.push_back({11, enter});
      break;
    case ApprovalScenarioAction::dismiss:
      value.steps.push_back({11, testing::TuiScenarioPost{termforge::KeyEvent{
                                     termforge::Key::Escape, 0, false, false,
                                     false, termforge::KeyAction::Press}}});
      break;
    case ApprovalScenarioAction::interrupt:
      value.steps.push_back({11, testing::TuiScenarioPost{termforge::KeyEvent{
                                     termforge::Key::Char, U'c', true, false,
                                     false, termforge::KeyAction::Press}}});
      break;
  }
  if (action == ApprovalScenarioAction::interrupt) {
    value.steps.push_back(
        {14, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}});
    value.steps.push_back({14, enter});
  } else {
    for (std::uint64_t frame = 13; frame < 17; ++frame) {
      value.steps.push_back(
          {frame,
           testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}});
    }
    value.steps.push_back(
        {20, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}});
    value.steps.push_back({20, enter});
  }
  value.limits.maximum_frames = 48;
  return value;
}

auto tool_profile_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-tool-profile-session-local";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 18, 1000, 360};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  const auto down = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Down, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/tools off"}}},
      {0, enter},
      {2, testing::TuiScenarioPost{termforge::PasteEvent{
              "/tools profile essentials"}}},
      {2, enter},
      {4, testing::TuiScenarioPost{termforge::PasteEvent{"/tools"}}},
      {4, enter},
      {5, testing::TuiScenarioResize{{8, 3, 80, 60}}},
      {6, testing::TuiScenarioResize{{100, 18, 1000, 360}}},
      {7, down},
      {7, down},
      {8, enter},
      {10, testing::TuiScenarioPost{termforge::PasteEvent{
               "/session resume target-session"}}},
      {10, enter},
      {12, testing::TuiScenarioPost{termforge::PasteEvent{"/tools"}}},
      {12, enter},
      {14, enter},
      {16, testing::TuiScenarioPost{termforge::PasteEvent{"/tools off"}}},
      {16, enter},
      {18, testing::TuiScenarioPost{termforge::PasteEvent{"/session new"}}},
      {18, enter},
      {20, testing::TuiScenarioPost{termforge::PasteEvent{"/tools"}}},
      {20, enter},
      {22, enter},
      {24, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}},
      {24, enter},
  };
  value.limits.maximum_frames = 48;
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

auto request_settings_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-request-settings-cancel-resize";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {160, 28, 1600, 560};
  const auto key = [](const termforge::Key selected) {
    return testing::TuiScenarioPost{termforge::KeyEvent{
        selected, 0, false, false, false, termforge::KeyAction::Press}};
  };
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/settings"}}},
      {0, key(termforge::Key::Enter)},
      {1, testing::TuiScenarioResize{{8, 3, 80, 60}}},
      {2, testing::TuiScenarioResize{{160, 28, 1600, 560}}},
      {3, key(termforge::Key::Enter)},
      {4, testing::TuiScenarioResize{{160, 28, 1600, 560}}},
      {5, testing::TuiScenarioResize{{16, 5, 160, 100}}},
      {6, testing::TuiScenarioResize{{160, 28, 1600, 560}}},
      {7, key(termforge::Key::Escape)},
      {8, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Char, U'd', true, false, false,
              termforge::KeyAction::Press}}},
  };
  value.limits.maximum_frames = 24;
  return value;
}

auto persona_manager_cancel_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-persona-manager-cancel-resize";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {120, 20, 1200, 400};
  const auto key = [](const termforge::Key selected) {
    return testing::TuiScenarioPost{termforge::KeyEvent{
        selected, 0, false, false, false, termforge::KeyAction::Press}};
  };
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/persona manage"}}},
      {0, key(termforge::Key::Enter)},
      {1, key(termforge::Key::Enter)},
      {2, testing::TuiScenarioResize{{8, 3, 80, 60}}},
      {3, testing::TuiScenarioResize{{120, 20, 1200, 400}}},
      {4, key(termforge::Key::Escape)},
      {5, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Char, U'd', true, false, false,
              termforge::KeyAction::Press}}},
  };
  value.limits.maximum_frames = 16;
  return value;
}

auto request_settings_apply_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-request-settings-session-apply";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 18, 1000, 360};
  const auto key = [](const termforge::Key selected) {
    return testing::TuiScenarioPost{termforge::KeyEvent{
        selected, 0, false, false, false, termforge::KeyAction::Press}};
  };
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/settings"}}},
      {0, key(termforge::Key::Enter)},
      {1, key(termforge::Key::Down)},
      {2, key(termforge::Key::Enter)},
      {3, key(termforge::Key::Down)},
      {4, key(termforge::Key::Enter)},
      {5, key(termforge::Key::Enter)},
      {6, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Char, U'd', true, false, false,
              termforge::KeyAction::Press}}},
  };
  value.limits.maximum_frames = 24;
  return value;
}

auto request_settings_session_switch_scenario() -> testing::TuiScenario {
  auto value = request_settings_apply_scenario();
  value.scenario_id = "interactive-request-settings-session-switch";
  const auto key = [](const termforge::Key selected) {
    return testing::TuiScenarioPost{termforge::KeyEvent{
        selected, 0, false, false, false, termforge::KeyAction::Press}};
  };
  value.steps.pop_back();
  value.steps.insert(
      value.steps.end(),
      {{6, testing::TuiScenarioPost{termforge::PasteEvent{"/session new"}}},
       {6, key(termforge::Key::Enter)},
       {7, testing::TuiScenarioPost{termforge::PasteEvent{"/settings"}}},
       {7, key(termforge::Key::Enter)},
       {8, key(termforge::Key::Down)},
       {9, key(termforge::Key::Enter)},
       {10, key(termforge::Key::Escape)},
       {11, testing::TuiScenarioPost{
                termforge::KeyEvent{termforge::Key::Char, U'd', true, false,
                                    false, termforge::KeyAction::Press}}}});
  value.limits.maximum_frames = 40;
  return value;
}

auto request_settings_save_scenario() -> testing::TuiScenario {
  auto value = request_settings_apply_scenario();
  value.scenario_id = "interactive-request-settings-save-default";
  value.steps.insert(value.steps.begin() + 6,
                     {5, testing::TuiScenarioPost{termforge::KeyEvent{
                             termforge::Key::Down, 0, false, false, false,
                             termforge::KeyAction::Press}}});
  value.steps.back().after_frame = 7;
  return value;
}

auto request_web_search_save_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-web-search-save-default";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 18, 1000, 360};
  const auto key = [](const termforge::Key selected) {
    return testing::TuiScenarioPost{termforge::KeyEvent{
        selected, 0, false, false, false, termforge::KeyAction::Press}};
  };
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/settings"}}},
      {0, key(termforge::Key::Enter)},
      {1, key(termforge::Key::Enter)},
      {2, key(termforge::Key::Down)},
      {3, key(termforge::Key::Down)},
      {4, key(termforge::Key::Enter)},
      {5, key(termforge::Key::Down)},
      {6, key(termforge::Key::Enter)},
      {7, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Char, U'd', true, false, false,
              termforge::KeyAction::Press}}},
  };
  value.limits.maximum_frames = 24;
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

auto chat_factory(
    const bool with_catalog = false, const bool with_persistence = false,
    const bool stale_preview = false, const bool persistence_failure = false,
    std::optional<config::ConfigSource> shadow_source = std::nullopt,
    const bool report_persist_calls = false, const bool with_personas = false,
    const bool with_characters = false, const bool character_drift = false,
    const bool report_character_run = false)
    -> testing::TuiScenarioTargetFactory {
  return [with_catalog, with_persistence, stale_preview, persistence_failure,
          shadow_source, report_persist_calls, with_personas, with_characters,
          character_drift, report_character_run](
             const testing::TuiScenarioPass pass, termforge::ByteSink* output)
             -> std::expected<testing::TuiScenarioTarget,
                              testing::TuiScenarioError> {
    auto pipe = std::make_shared<Pipe>();
    if (!pipe->ok()) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          "chat scenario pipe setup failed"});
    }
    auto backend_state = std::make_shared<GatedBackendState>();
    if (stale_preview) backend_state->set_web_search_support(true);
    auto backend = std::make_shared<GatedBackend>(backend_state);
    auto editor = std::make_shared<NoEditor>();
    auto catalog_source = std::make_shared<ScenarioCatalogSource>();
    auto catalog = std::make_shared<model::CatalogService>(*catalog_source);
    auto characters =
        std::make_shared<ScenarioProviderCharacterSource>(character_drift);
    auto frame = std::make_shared<std::string>();
    auto suffix = std::make_shared<std::uint64_t>();
    auto persist_calls = std::make_shared<std::size_t>();
    auto personas = std::make_shared<testing::ScriptedPersonaSource>(
        std::vector<testing::PersonaListOutcome>{
            std::vector<domain::PersonaSummary>{}},
        std::vector<testing::PersonaLoadExchange>{});
    auto persona_editor = std::make_shared<testing::ScriptedPersonaEditor>();
    adapters::InteractiveChatAppOptions options;
    options.live_wake_enabled = pass == testing::TuiScenarioPass::record;
    options.poll_worker_updates = false;
    if (with_catalog || with_characters) options.model_catalog = catalog.get();
    if (with_characters) options.provider_character_catalog = characters.get();
    if (with_persistence) {
      options.preview_request_setting =
          [backend_state, stale_preview,
           shadow_source](const adapters::VeniceRequestSettingSave& save)
          -> std::expected<adapters::VenicePreparedPersistedSettings,
                           std::string> {
        adapters::VeniceConfiguredRequestSettings configured;
        if (save.web_search) {
          if (shadow_source) {
            configured.web_search = adapters::VeniceWebSearchSetting::off;
            configured.web_search_source = shadow_source;
          } else {
            configured.web_search = *save.web_search;
            if (*save.web_search != adapters::VeniceWebSearchSetting::inherit) {
              configured.web_search_source = config::ConfigSource::file;
            }
          }
        } else if (save.system_prompt) {
          if (shadow_source) {
            configured.system_prompt =
                adapters::VeniceSystemPromptSetting::exclude;
            configured.system_prompt_source = shadow_source;
          } else {
            configured.system_prompt = *save.system_prompt;
            if (*save.system_prompt !=
                adapters::VeniceSystemPromptSetting::inherit) {
              configured.system_prompt_source = config::ConfigSource::file;
            }
          }
        } else {
          return std::unexpected("scenario save is empty");
        }
        if (stale_preview) backend_state->set_web_search_support(false);
        domain::ConfigurationProvenanceEntry provenance;
        provenance.key = save.web_search ? "venice.web_search"
                                         : "venice.include_system_prompt";
        return adapters::VenicePreparedPersistedSettings{std::move(configured),
                                                         std::move(provenance)};
      };
      options.persist_request_setting =
          [persistence_failure,
           persist_calls](const adapters::VeniceRequestSettingSave&)
          -> std::expected<void, std::string> {
        ++*persist_calls;
        if (persistence_failure)
          return std::unexpected("scenario persistence failed");
        return {};
      };
    }
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
    if (with_personas) {
      options.session_dependencies.persona_source = personas.get();
      options.session_dependencies.persona_editor = persona_editor.get();
    }
    const auto model_id = make_id<domain::ModelId>("model");
    surfaces::ChatSessionOpen open{
        model_id, surfaces::ChatSessionOpen::Mode::ephemeral, std::nullopt};
    if (report_character_run) {
      open.provenance = domain::RunProvenance{"test-revision",
                                              "scenario",
                                              std::nullopt,
                                              model_id,
                                              std::nullopt,
                                              {},
                                              {{"aiforge", "test-revision"}},
                                              {},
                                              {}};
    }
    auto app = adapters::make_interactive_chat_app(*backend, *backend, nullptr,
                                                   std::move(open), *editor, {},
                                                   std::move(options));
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
        [raw, backend, editor, catalog_source, catalog, characters, personas,
         persona_editor] {
          static_cast<void>(backend);
          static_cast<void>(editor);
          static_cast<void>(catalog_source);
          static_cast<void>(catalog);
          static_cast<void>(characters);
          static_cast<void>(personas);
          static_cast<void>(persona_editor);
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
        [raw, backend, editor, catalog_source, catalog, characters,
         persist_calls, report_persist_calls, personas, persona_editor,
         with_personas, with_characters, backend_state, report_character_run] {
          static_cast<void>(backend);
          static_cast<void>(editor);
          static_cast<void>(catalog_source);
          static_cast<void>(catalog);
          static_cast<void>(personas);
          static_cast<void>(persona_editor);
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
          if (report_persist_calls) state << "|persist=" << *persist_calls;
          if (with_personas) {
            state << "|persona-creates="
                  << persona_editor->recorded_creates().size()
                  << "|persona-replaces="
                  << persona_editor->recorded_replaces().size();
          }
          if (with_characters) {
            state << "|character-lists=" << characters->list_calls()
                  << "|character-lookups=" << characters->lookup_calls();
          }
          if (report_character_run) {
            const auto request = backend_state->captured_request();
            state << "|request-extensions="
                  << (request ? request->options.extensions.size() : 0U)
                  << "|request-tools="
                  << (request ? request->tools.size() : 0U);
            if (request) {
              const auto character = request->options.extensions.find(
                  std::string{adapters::venice_character_slug_extension});
              if (character != request->options.extensions.end()) {
                state << "|request-character=" << character->second.media_type
                      << ':' << character->second.data;
              }
            }
            for (const auto& event : raw->events()) {
              if (const auto* started =
                      std::get_if<domain::RunStarted>(&event.payload)) {
                state << "|run-axes=" << started->surface_id.value() << ','
                      << started->workspace_id.value() << ','
                      << started->permission_profile_id.value() << ",persona="
                      << (started->persona_id
                              ? std::string{started->persona_id->value()}
                              : std::string{"none"});
              }
              if (const auto* recorded =
                      std::get_if<domain::RunProvenanceRecorded>(
                          &event.payload)) {
                state << "|provenance-tools="
                      << recorded->provenance.tools.size();
                const auto character = std::ranges::find(
                    recorded->provenance.effective_request_options,
                    adapters::venice_character_slug_extension,
                    &domain::EffectiveRequestOption::key);
                if (character !=
                    recorded->provenance.effective_request_options.end()) {
                  state
                      << "|provenance-character="
                      << character->value.value_or("missing") << ':'
                      << (character->source ==
                                  domain::RequestOptionSource::session_override
                              ? "session_override"
                              : "unexpected_source");
                }
              }
            }
          }
          return std::move(state).str();
        }};
  };
}

auto question_factory() -> testing::TuiScenarioTargetFactory {
  return [](const testing::TuiScenarioPass pass, termforge::ByteSink* output)
             -> std::expected<testing::TuiScenarioTarget,
                              testing::TuiScenarioError> {
    auto pipe = std::make_shared<Pipe>();
    if (!pipe->ok()) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          "question scenario pipe setup failed"});
    }
    auto state = std::make_shared<QuestionBackendState>();
    auto backend = std::make_shared<QuestionBackend>(state);
    auto editor = std::make_shared<NoEditor>();
    auto frame = std::make_shared<std::string>();
    auto suffix = std::make_shared<std::uint64_t>();
    runtime::ToolRegistry registry;
    if (auto registered = runtime::register_ask_user_tool(registry, true);
        !registered) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          registered.error().message});
    }
    auto tools = registry.snapshot();
    if (!tools) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          tools.error().message});
    }
    adapters::InteractiveChatAppOptions options;
    options.live_wake_enabled = pass == testing::TuiScenarioPass::record;
    options.poll_worker_updates = false;
    options.wake_observer = [state] { state->observe_wake(); };
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
    options.session_dependencies.tools = std::move(*tools);
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
        [raw, backend, editor] {
          static_cast<void>(backend);
          static_cast<void>(editor);
          return raw->run();
        },
        [state, pass](const std::string_view step) {
          return state->release(step, pass == testing::TuiScenarioPass::record);
        },
        [](std::string_view) -> std::expected<void, std::string> {
          return std::unexpected("question scenario has no tool script");
        },
        [frame] { return *frame; },
        [raw, state, backend, editor] {
          static_cast<void>(backend);
          static_cast<void>(editor);
          auto terminal = std::string{"active"};
          for (const auto& event : raw->events()) {
            if (std::holds_alternative<domain::RunCompleted>(event.payload)) {
              terminal = "completed";
            } else if (std::holds_alternative<domain::RunCancelled>(
                           event.payload)) {
              terminal = "cancelled";
            }
          }
          return state->semantic_state() + "|" + terminal + "|" +
                 std::string{raw->status_text()};
        }};
  };
}

auto approval_factory() -> testing::TuiScenarioTargetFactory {
  return [](const testing::TuiScenarioPass pass, termforge::ByteSink* output)
             -> std::expected<testing::TuiScenarioTarget,
                              testing::TuiScenarioError> {
    auto pipe = std::make_shared<Pipe>();
    if (!pipe->ok()) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          "approval scenario pipe setup failed"});
    }
    auto state = std::make_shared<ApprovalBackendState>();
    auto backend = std::make_shared<ApprovalBackend>(state);
    auto editor = std::make_shared<NoEditor>();
    auto frame = std::make_shared<std::string>();
    auto suffix = std::make_shared<std::uint64_t>();
    const auto invocation = make_id<domain::InvocationId>("approval-call");
    const domain::CapabilityScope declared_scope{
        domain::Effect::read, "filesystem.root", "/work/repository"};
    const domain::CapabilityScope requested_scope{
        domain::Effect::read, "filesystem.root", "/work/repository/README.md"};
    const runtime::ToolExecutionLimits limits{4096, 8, 1s};
    const domain::StructuredDataBlock arguments{
        "application/json", R"({"relative_path":"README.md"})"};
    auto executor = std::make_shared<ApprovalToolExecutor>(
        arguments, requested_scope,
        std::vector<testing::ScriptedToolExchange>{
            {runtime::ToolInvocation{
                 invocation,
                 std::nullopt,
                 "read_repository_file",
                 runtime::ValidatedToolArguments{
                     arguments, {requested_scope}, {domain::Effect::read}},
                 {requested_scope},
                 limits},
             testing::ToolStreamScript{
                 {runtime::ToolExecutionEvent{runtime::ToolResult{
                      {domain::TextBlock{"repository contents"}}}},
                  testing::ToolEndOfStream{}}}}});
    runtime::ToolRegistry registry;
    auto registered = registry.register_tool(
        backend::ToolDeclaration{
            "read_repository_file",
            "Read one repository file",
            {"application/schema+json", R"({"type":"object"})"},
            {domain::Effect::read},
            {declared_scope}},
        executor, limits,
        runtime::ToolExecutorContract{"scenario.repository-read", "1"});
    if (!registered) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          registered.error().message});
    }
    auto tools = registry.snapshot();
    if (!tools) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          tools.error().message});
    }
    const auto permission_profile =
        make_id<domain::PermissionProfileId>("approval-scenario");
    auto policy = runtime::make_tool_launch_policy(
        *tools, {permission_profile,
                 runtime::RestrictionLevel::medium,
                 runtime::ApprovalMode::prompt,
                 {}});
    if (!policy) {
      return std::unexpected(testing::TuiScenarioError{
          testing::TuiScenarioErrorCode::target_failure,
          policy.error().message});
    }

    adapters::InteractiveChatAppOptions options;
    options.live_wake_enabled = pass == testing::TuiScenarioPass::record;
    options.poll_worker_updates = false;
    options.wake_observer = [state] { state->observe_wake(); };
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
    options.session_dependencies.tools = *tools;
    options.session_dependencies.tool_policy = std::move(*policy);
    options.session_dependencies.permission_profile_id = permission_profile;
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
        [raw, backend, editor] {
          static_cast<void>(backend);
          static_cast<void>(editor);
          return raw->run();
        },
        [state, pass](const std::string_view step) {
          return state->release(step, pass == testing::TuiScenarioPass::record);
        },
        [](std::string_view) -> std::expected<void, std::string> {
          return std::unexpected("approval scenario has no tool script");
        },
        [frame] { return *frame; },
        [raw, state, backend, editor, executor] {
          static_cast<void>(backend);
          static_cast<void>(editor);
          auto terminal = std::string{"active"};
          auto decision = std::string{"none"};
          bool started{};
          for (const auto& event : raw->events()) {
            if (const auto* approved =
                    std::get_if<domain::ToolApprovalDecided>(&event.payload)) {
              switch (approved->decision) {
                case domain::ApprovalDecision::approved:
                  decision = "approved";
                  break;
                case domain::ApprovalDecision::denied:
                  decision = "denied";
                  break;
                case domain::ApprovalDecision::cancelled:
                  decision = "cancelled";
                  break;
              }
              if (approved->lifetime !=
                      domain::ApprovalGrantLifetime::invocation ||
                  (approved->decision == domain::ApprovalDecision::approved &&
                   approved->granted_scopes !=
                       std::vector<domain::CapabilityScope>{
                           {domain::Effect::read, "filesystem.root",
                            "/work/repository/README.md"}}) ||
                  (approved->decision != domain::ApprovalDecision::approved &&
                   !approved->granted_scopes.empty())) {
                decision += "-invalid";
              }
            }
            if (std::holds_alternative<domain::ToolStarted>(event.payload)) {
              started = true;
            }
            if (std::holds_alternative<domain::RunCompleted>(event.payload)) {
              terminal = "completed";
            } else if (std::holds_alternative<domain::RunCancelled>(
                           event.payload)) {
              terminal = "cancelled";
            }
          }
          return state->semantic_state() + "|decision=" + decision +
                 "|started=" + (started ? "yes" : "no") + "|executions=" +
                 std::to_string(executor->recorded_invocations().size()) +
                 "|remaining=" +
                 std::to_string(executor->remaining_exchanges()) + "|" +
                 terminal + "|" + std::string{raw->status_text()};
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

auto provider_character_picker_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-provider-character-picker";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 28, 1000, 560};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  const auto clear = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Char, U'c', true, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/character"}}},
      {0, enter},
      {1, testing::TuiScenarioResize{{8, 3, 80, 60}}},
      {2, testing::TuiScenarioResize{{100, 28, 1000, 560}}},
      {3, testing::TuiScenarioPost{termforge::PasteEvent{"alan"}}},
      {4, enter},
      {5, testing::TuiScenarioPost{termforge::PasteEvent{"/model model"}}},
      {5, enter},
      {6, testing::TuiScenarioPost{termforge::PasteEvent{"/model alternate"}}},
      {6, enter},
      {7, clear},
      {8, testing::TuiScenarioPost{termforge::PasteEvent{"/character off"}}},
      {8, enter},
      {9, testing::TuiScenarioPost{termforge::PasteEvent{"/model alternate"}}},
      {9, enter},
      {10, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}},
      {10, enter},
  };
  value.limits.maximum_frames = 40;
  return value;
}

auto provider_character_disabled_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-provider-character-disabled-choice";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 28, 1000, 560};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/character"}}},
      {0, enter},
      {1, testing::TuiScenarioPost{termforge::PasteEvent{"missing-model"}}},
      {2, enter},
      {3, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Escape, 0, false, false, false,
              termforge::KeyAction::Press}}},
      {4, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Char, U'd', true, false, false,
              termforge::KeyAction::Press}}},
  };
  value.limits.maximum_frames = 24;
  return value;
}

auto provider_character_unsafe_paste_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-provider-character-unsafe-paste";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 28, 1000, 560};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{"/character"}}},
      {0, enter},
      {1, testing::TuiScenarioPost{termforge::PasteEvent{"bad\x1b"}}},
      {2, enter},
      {3, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Char, U'd', true, false, false,
              termforge::KeyAction::Press}}},
  };
  value.limits.maximum_frames = 20;
  return value;
}

auto provider_character_drift_scenario() -> testing::TuiScenario {
  auto value = provider_character_picker_scenario();
  value.scenario_id = "interactive-provider-character-model-drift";
  value.steps.resize(6);
  value.steps.push_back({5, testing::TuiScenarioPost{termforge::KeyEvent{
                                termforge::Key::Char, U'd', true, false, false,
                                termforge::KeyAction::Press}}});
  value.limits.maximum_frames = 24;
  return value;
}

auto provider_character_session_switch_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-provider-character-session-switch";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 12, 1000, 240};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{
              "/character set alan-watts"}}},
      {0, enter},
      {1, testing::TuiScenarioPost{termforge::PasteEvent{"/session new"}}},
      {1, enter},
      {2, testing::TuiScenarioPost{termforge::PasteEvent{"/model alternate"}}},
      {2, enter},
      {3, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}},
      {3, enter},
  };
  value.limits.maximum_frames = 24;
  return value;
}

auto provider_character_next_inference_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-provider-character-next-inference";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 12, 1000, 240};
  value.backend_script = {"response-started",  "delta:hello", "usage", "cost",
                          "response-finished", "end"};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{
              "/character set alan-watts"}}},
      {0, enter},
      {1, testing::TuiScenarioPost{termforge::PasteEvent{"question"}}},
      {1, enter},
      {2, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {3, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {4, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {5, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {6, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {7, testing::TuiScenarioRelease{testing::TuiScenarioProducer::backend}},
      {8, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}},
      {8, enter},
  };
  value.limits.maximum_frames = 28;
  return value;
}

auto provider_character_disappearance_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-provider-character-disappearance";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {100, 12, 1000, 240};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{
              "/character set disappeared"}}},
      {0, enter},
      {1, testing::TuiScenarioPost{termforge::KeyEvent{
              termforge::Key::Char, U'c', true, false, false,
              termforge::KeyAction::Press}}},
      {2, testing::TuiScenarioPost{termforge::PasteEvent{"/model alternate"}}},
      {2, enter},
      {3, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}},
      {3, enter},
  };
  value.limits.maximum_frames = 24;
  return value;
}

auto reasoning_visibility_scenario() -> testing::TuiScenario {
  testing::TuiScenario value;
  value.scenario_id = "interactive-reasoning-visibility";
  value.corpus_version = "1";
  value.application_revision = "test-revision";
  value.initial_size = {120, 10, 1200, 200};
  const auto enter = testing::TuiScenarioPost{
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}};
  value.steps = {
      {0, testing::TuiScenarioPost{termforge::PasteEvent{
              "/session resume reasoning-session"}}},
      {0, enter},
      {1, testing::TuiScenarioPost{termforge::PasteEvent{"/reasoning show"}}},
      {1, enter},
      {2, testing::TuiScenarioResize{{18, 4, 180, 80}}},
      {3, testing::TuiScenarioPost{termforge::PasteEvent{
              "/session resume target-session"}}},
      {3, enter},
      {4, testing::TuiScenarioPost{termforge::PasteEvent{
              "/session resume reasoning-session"}}},
      {4, enter},
      {5, testing::TuiScenarioPost{termforge::PasteEvent{"/reasoning hide"}}},
      {5, enter},
      {6, testing::TuiScenarioPost{termforge::PasteEvent{"/quit"}}},
      {6, enter},
  };
  value.limits.maximum_frames = 32;
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

auto session_factory(const bool durable, const bool fail_listing = false,
                     const bool report_mutations = false)
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
        [raw, backend, editor, store, backend_state, report_mutations] {
          static_cast<void>(backend);
          static_cast<void>(editor);
          static_cast<void>(store);
          auto state = std::string{raw->status_text()};
          if (report_mutations) {
            state += "|appends=" + std::to_string(store->append_calls());
            state += "|inferences=" +
                     std::to_string(backend_state->initialize_calls());
          }
          return state;
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

TEST_CASE("interactive ask_user answers continue the same run",
          "[scenario][chat][questions]") {
  const auto result =
      testing::run_tui_scenario(question_scenario(), question_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state.find("requests=2|valid|completed") !=
          std::string::npos);
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("Choose output") != std::string::npos;
      }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("continued answer") != std::string::npos;
      }));
}

TEST_CASE("interactive ask_user Ctrl+C cancels instead of quitting silently",
          "[scenario][chat][questions][cancellation]") {
  const auto result =
      testing::run_tui_scenario(question_scenario(true), question_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state.find("requests=1|valid|cancelled") !=
          std::string::npos);
}

TEST_CASE("interactive tool approval shows exact authority and allows once",
          "[scenario][chat][tools][approval]") {
  const auto result = testing::run_tui_scenario(
      approval_scenario(ApprovalScenarioAction::allow_once),
      approval_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state.find(
              "requests=2|valid|decision=approved|started=yes|executions=1|"
              "remaining=0|completed") != std::string::npos);
  for (const auto expected :
       {"Approve tool action", "Tool: read_repository_file", "Effects: read",
        "filesystem.root", "/work/repository/README.md", "Deny",
        "Allow once"}) {
    CAPTURE(expected);
    CHECK(result->recorded.wire_output.find(expected) != std::string::npos);
  }
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames,
      [](const std::string& frame) { return frame.starts_with("12x4:"); }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("continued after approval") != std::string::npos;
      }));
}

TEST_CASE("interactive tool approval defaults to deny and dismissal continues",
          "[scenario][chat][tools][approval][cancellation]") {
  const std::vector<std::pair<ApprovalScenarioAction, std::string_view>> cases{
      {ApprovalScenarioAction::deny, "decision=denied"},
      {ApprovalScenarioAction::dismiss, "decision=cancelled"},
  };
  for (const auto& [action, expected] : cases) {
    const auto result = testing::run_tui_scenario(approval_scenario(action),
                                                  approval_factory());
    INFO((result ? std::string{} : result.error().message));
    REQUIRE(result);
    REQUIRE(result->recorded == result->replayed);
    REQUIRE(result->recorded.semantic_state.find("requests=2|valid|") !=
            std::string::npos);
    REQUIRE(result->recorded.semantic_state.find(expected) !=
            std::string::npos);
    REQUIRE(result->recorded.semantic_state.find(
                "started=no|executions=0|remaining=1|completed") !=
            std::string::npos);
    REQUIRE(std::ranges::any_of(
        result->recorded.normalized_frames, [](const std::string& frame) {
          return frame.find("continued after approval") != std::string::npos;
        }));
  }
}

TEST_CASE("interactive tool approval Ctrl+C cancels the active run",
          "[scenario][chat][tools][approval][cancellation]") {
  const auto result = testing::run_tui_scenario(
      approval_scenario(ApprovalScenarioAction::interrupt), approval_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state.find(
              "requests=1|valid|decision=none|started=no|executions=0|"
              "remaining=1|cancelled") != std::string::npos);
}

TEST_CASE("interactive tool profile commands remain session local",
          "[scenario][chat][tools][profiles]") {
  const auto result =
      testing::run_tui_scenario(tool_profile_scenario(), session_factory(true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  const auto picker_open = std::ranges::find_if(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("Choose a Chat tool profile") != std::string::npos;
      });
  REQUIRE(picker_open != result->recorded.normalized_frames.end());
  const auto selected_off = std::ranges::find_if(
      std::next(picker_open), result->recorded.normalized_frames.end(),
      [](const std::string& frame) {
        return frame.find("Selected Off") != std::string::npos;
      });
  REQUIRE(selected_off != result->recorded.normalized_frames.end());
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames,
      [](const std::string& frame) { return frame.starts_with("8x3:"); }));
  const auto resumed_session = std::ranges::find_if(
      std::next(selected_off), result->recorded.normalized_frames.end(),
      [](const std::string& frame) {
        return frame.find("target-session") != std::string::npos;
      });
  REQUIRE(resumed_session != result->recorded.normalized_frames.end());
  const auto resumed_profile = std::ranges::find_if(
      std::next(resumed_session), result->recorded.normalized_frames.end(),
      [](const std::string& frame) {
        return frame.find("Selected Essentials") != std::string::npos;
      });
  REQUIRE(resumed_profile != result->recorded.normalized_frames.end());
  const auto selected_off_again = std::ranges::find_if(
      std::next(resumed_profile), result->recorded.normalized_frames.end(),
      [](const std::string& frame) {
        return frame.find("Selected Off") != std::string::npos;
      });
  REQUIRE(selected_off_again != result->recorded.normalized_frames.end());
  const auto new_session = std::ranges::find_if(
      std::next(selected_off_again), result->recorded.normalized_frames.end(),
      [](const std::string& frame) {
        return frame.find("session session-3") != std::string::npos;
      });
  REQUIRE(new_session != result->recorded.normalized_frames.end());
  REQUIRE(std::ranges::any_of(
      std::next(new_session), result->recorded.normalized_frames.end(),
      [](const std::string& frame) {
        return frame.find("Selected Essentials") != std::string::npos;
      }));
}

TEST_CASE("interactive ask_user dismissal records a result and continues",
          "[scenario][chat][questions][cancellation]") {
  const auto result =
      testing::run_tui_scenario(question_cancel_scenario(), question_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state.find("requests=2|valid|completed") !=
          std::string::npos);
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("Question cancelled; continuing run") !=
               std::string::npos;
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

TEST_CASE("request settings panel cancels atomically across tiny resizes",
          "[scenario][chat][settings][failure]") {
  const auto result =
      testing::run_tui_scenario(request_settings_scenario(), chat_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state == "Request settings unchanged");
  REQUIRE(result->recorded.wire_output.find("Request settings") !=
          std::string::npos);
  REQUIRE(result->recorded.wire_output.find("Selected-model support") !=
          std::string::npos);
  REQUIRE(result->recorded.wire_output.find("Takes effect: next inference") !=
          std::string::npos);
  REQUIRE(result->recorded.wire_output.find("Effective winner:") !=
          std::string::npos);
  REQUIRE(result->recorded.wire_output.find("(provider default)") !=
          std::string::npos);
}

TEST_CASE("persona manager cancels without writes across tiny resizes",
          "[scenario][chat][persona][failure]") {
  const auto result = testing::run_tui_scenario(
      persona_manager_cancel_scenario(),
      chat_factory(false, false, false, false, std::nullopt, false, true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state ==
          "Persona creation cancelled|persona-creates=0|"
          "persona-replaces=0");
  REQUIRE(result->recorded.wire_output.find("Manage personas") !=
          std::string::npos);
  REQUIRE(result->recorded.wire_output.find("Create new") != std::string::npos);
  REQUIRE(result->recorded.wire_output.find("Create persona") !=
          std::string::npos);
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames,
      [](const std::string& frame) { return frame.starts_with("8x3:"); }));
}

TEST_CASE("request settings panel applies a transient next-inference value",
          "[scenario][chat][settings]") {
  const auto result = testing::run_tui_scenario(
      request_settings_apply_scenario(), chat_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state ==
          "Session request override updated; applies next inference");
  REQUIRE(result->recorded.wire_output.find("Venice system prompt") !=
          std::string::npos);
}

TEST_CASE("request setting session overrides clear on session switch",
          "[scenario][chat][settings][session]") {
  const auto result = testing::run_tui_scenario(
      request_settings_session_switch_scenario(), chat_factory());
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state == "Request settings unchanged");
  const auto last_settings_panel =
      result->recorded.wire_output.rfind("Venice system prompt");
  REQUIRE(last_settings_panel != std::string::npos);
  REQUIRE(result->recorded.wire_output.find("Session override: none",
                                            last_settings_panel) !=
          std::string::npos);
}

TEST_CASE("request settings panel distinguishes a persisted default",
          "[scenario][chat][settings][config]") {
  const auto result = testing::run_tui_scenario(
      request_settings_save_scenario(), chat_factory(false, true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state ==
          "Saved request default; applies next inference");
}

TEST_CASE("request setting save revalidates stale model support before write",
          "[scenario][chat][settings][config][failure]") {
  const auto result = testing::run_tui_scenario(
      request_web_search_save_scenario(),
      chat_factory(false, true, true, false, std::nullopt, true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state ==
          "selected model does not confirm required capability "
          "'web-search'|persist=0");
}

TEST_CASE("request setting persistence failure leaves the live session intact",
          "[scenario][chat][settings][config][failure]") {
  const auto result = testing::run_tui_scenario(
      request_settings_save_scenario(),
      chat_factory(false, true, false, true, std::nullopt, true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state ==
          "scenario persistence failed|persist=1");
}

TEST_CASE("saved request setting reports a higher-precedence active winner",
          "[scenario][chat][settings][config]") {
  const auto result = testing::run_tui_scenario(
      request_settings_save_scenario(),
      chat_factory(false, true, false, false,
                   config::ConfigSource::environment));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state ==
          "Saved request default; active environment value still wins next "
          "inference");
}

TEST_CASE("provider character picker is searchable bounded and model-safe",
          "[scenario][chat][character]") {
  const auto result =
      testing::run_tui_scenario(provider_character_picker_scenario(),
                                chat_factory(false, false, false, false,
                                             std::nullopt, false, false, true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state ==
          "Selected model alternate|character-lists=1|character-lookups=3");
  REQUIRE(result->recorded.wire_output.find("Select provider character") !=
          std::string::npos);
  REQUIRE(result->recorded.wire_output.find("Alan Watts") != std::string::npos);
  REQUIRE(result->recorded.wire_output.find("requires model alternate") !=
          std::string::npos);
  REQUIRE(result->recorded.wire_output.find("required text model is offline") !=
          std::string::npos);
  REQUIRE(result->recorded.wire_output.find(
              "character has no model metadata") != std::string::npos);
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames,
      [](const std::string& frame) { return frame.starts_with("8x3:"); }));
}

TEST_CASE("provider character typed filter enforces its exact byte boundary",
          "[scenario][chat][character][filter][failure]") {
  const auto current_model = make_id<domain::ModelId>("model");
  backend::ProviderCharacterSummary provider_first{
      make_id<domain::ProviderCharacterId>("provider-first")};
  provider_first.name = std::string(4095, 'a') + 'y';
  provider_first.model_id = current_model;
  backend::ProviderCharacterSummary boundary{
      make_id<domain::ProviderCharacterId>("boundary")};
  boundary.name = std::string(4095, 'a') + 'x';
  boundary.model_id = current_model;
  const backend::ProviderCharacterCatalog characters{{provider_first, boundary},
                                                     "scenario.characters"};
  model::CatalogEntry model_entry{current_model, "text"};
  model_entry.context_window_tokens = 8192;
  const model::CatalogSnapshot models{
      std::chrono::sys_time<std::chrono::milliseconds>{123ms},
      {std::move(model_entry)}};

  adapters::ProviderCharacterPickerDialog order_dialog;
  std::optional<adapters::ProviderCharacterPickerResult> ordered_selection;
  order_dialog.set_characters(characters, models, current_model);
  order_dialog.on_result(
      [&ordered_selection](
          std::optional<adapters::ProviderCharacterPickerResult> result) {
        ordered_selection = std::move(result);
      });
  REQUIRE(order_dialog.on_event(
      termforge::KeyEvent{termforge::Key::Down, 0, false, false, false,
                          termforge::KeyAction::Press}));
  REQUIRE(order_dialog.on_event(
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}));
  REQUIRE(ordered_selection);
  REQUIRE(ordered_selection->selection);
  REQUIRE(ordered_selection->selection->id == provider_first.id);

  adapters::ProviderCharacterPickerDialog dialog;
  std::optional<adapters::ProviderCharacterPickerResult> selected;
  dialog.set_characters(characters, models, current_model);
  dialog.on_result(
      [&selected](
          std::optional<adapters::ProviderCharacterPickerResult> result) {
        selected = std::move(result);
      });
  const auto typed = [&dialog](const char32_t character) {
    return dialog.on_event(termforge::KeyEvent{termforge::Key::Char, character,
                                               false, false, false,
                                               termforge::KeyAction::Press});
  };
  bool accepted = true;
  for (std::size_t index{}; index < 4095; ++index)
    accepted = typed(U'a') && accepted;
  REQUIRE(accepted);
  REQUIRE(typed(U'x')); // Exactly 4096 bytes selects only `boundary`.
  REQUIRE(typed(U'z')); // The over-bound character must not change the filter.
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Enter, 0, false,
                                              false, false,
                                              termforge::KeyAction::Press}));
  REQUIRE(selected);
  REQUIRE(selected->selection);
  REQUIRE(selected->selection->id == boundary.id);
}

TEST_CASE("disabled provider character choices cannot mutate the session",
          "[scenario][chat][character][failure]") {
  const auto result =
      testing::run_tui_scenario(provider_character_disabled_scenario(),
                                chat_factory(false, false, false, false,
                                             std::nullopt, false, false, true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state ==
          "Provider character selection cancelled|character-lists=1|"
          "character-lookups=0");
}

TEST_CASE("provider character filter rejects unsafe paste without mutation",
          "[scenario][chat][character][failure]") {
  const auto result =
      testing::run_tui_scenario(provider_character_unsafe_paste_scenario(),
                                chat_factory(false, false, false, false,
                                             std::nullopt, false, false, true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state ==
          "Provider character disabled; applies next inference|"
          "character-lists=1|character-lookups=0");
}

TEST_CASE("provider character picker revalidates model drift before mutation",
          "[scenario][chat][character][failure]") {
  const auto result = testing::run_tui_scenario(
      provider_character_drift_scenario(),
      chat_factory(false, false, false, false, std::nullopt, false, false, true,
                   true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state ==
          "Provider character model changed; reopen /character|"
          "character-lists=1|character-lookups=1");
}

TEST_CASE("provider character selection clears on a new session",
          "[scenario][chat][character][session]") {
  const auto result =
      testing::run_tui_scenario(provider_character_session_switch_scenario(),
                                chat_factory(false, false, false, false,
                                             std::nullopt, false, false, true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state ==
          "Selected model alternate|character-lists=0|character-lookups=1");
}

TEST_CASE("provider character reaches the next request and provenance",
          "[scenario][chat][character][provenance]") {
  const auto result = testing::run_tui_scenario(
      provider_character_next_inference_scenario(),
      chat_factory(false, false, false, false, std::nullopt, false, false, true,
                   false, true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state.find(
              "|request-extensions=1|request-tools=0|request-character="
              "application/json:\"alan-watts\"") != std::string::npos);
  REQUIRE(result->recorded.semantic_state.find(
              "|run-axes=interactive-2,chat-2,observe-2,persona=none") !=
          std::string::npos);
  REQUIRE(result->recorded.semantic_state.find("|provenance-tools=0|") !=
          std::string::npos);
  REQUIRE(result->recorded.semantic_state.find(
              "|provenance-character=alan-watts:session_override") !=
          std::string::npos);
}

TEST_CASE("direct provider character set is atomic when lookup disappears",
          "[scenario][chat][character][failure]") {
  const auto result =
      testing::run_tui_scenario(provider_character_disappearance_scenario(),
                                chat_factory(false, false, false, false,
                                             std::nullopt, false, false, true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state ==
          "Selected model alternate|character-lists=0|character-lookups=1");
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

TEST_CASE(
    "interactive reasoning visibility replays and survives session switches",
    "[scenario][chat][reasoning][session]") {
  const auto result = testing::run_tui_scenario(
      reasoning_visibility_scenario(), session_factory(true, false, true));
  INFO((result ? std::string{} : result.error().message));
  REQUIRE(result);
  REQUIRE(result->recorded == result->replayed);
  REQUIRE(result->recorded.semantic_state ==
          "Reasoning text hidden|appends=0|inferences=0");
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("Reasoning — hidden (21 bytes)") !=
                   std::string::npos &&
               frame.find("literal **reasoning**") == std::string::npos;
      }));
  REQUIRE(std::ranges::any_of(
      result->recorded.normalized_frames, [](const std::string& frame) {
        return frame.find("literal **reasoning**") != std::string::npos;
      }));
  for (const auto& frame : result->recorded.normalized_frames) {
    REQUIRE(frame.find("never-render-this") == std::string::npos);
  }
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
