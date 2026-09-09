#pragma once
#include "../159foldergrant/fixture.hpp"
#include <aiforge/adapters/interactive_chat_app.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/surfaces/admin_controller.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <random>
#include <source_location>
#include <stdexcept>

namespace chat_admin_test {
using namespace aiforge;
using namespace aiforge::domain;
using namespace std::chrono_literals;
using folder_grant_test::Gate;
using folder_grant_test::Release;
using folder_grant_test::until;
template <class T> auto id(std::string value) -> T {
  auto parsed = T::from(std::move(value));
  REQUIRE(parsed);
  return std::move(*parsed);
}
struct Directory {
  std::filesystem::path path{
      std::filesystem::temp_directory_path() /
      ("aiforge-chat-admin-" + std::to_string(std::random_device{}()))};
  Directory() {
    if (!std::filesystem::create_directory(path))
      throw std::runtime_error("test directory was not exclusively created");
    std::error_code error;
    std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    if (error) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      throw std::runtime_error("test directory permissions failed");
    }
  }
  ~Directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
// Refusal is injected at the storage port, but all accepted transactions and
// replay go through the production SQLite codec/projections.
class Store final : public storage::SessionStore {
 public:
  std::unique_ptr<adapters::SqliteSessionStore> sqlite;
  bool refuse_observation{}, refuse_replay{};
  unsigned refused{};
  explicit Store(const std::filesystem::path& path) {
    auto opened = adapters::SqliteSessionStore::open(path / "sessions.sqlite");
    REQUIRE(opened);
    sqlite = std::move(*opened);
    REQUIRE(sqlite->create_session({id<SessionId>("session"), {}}, {}));
  }
  auto create_session(storage::SessionCreate value, std::stop_token stop)
      -> std::expected<void, storage::SessionStoreError> override {
    return sqlite->create_session(std::move(value), stop);
  }
  auto create_session_with_events(storage::SessionCreate value,
                                  std::span<const RunEvent> events,
                                  std::stop_token stop)
      -> std::expected<void, storage::SessionStoreError> override {
    return sqlite->create_session_with_events(std::move(value), events, stop);
  }
  auto open_session(const SessionId& session, std::stop_token stop)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    return sqlite->open_session(session, stop);
  }
  auto list_sessions(std::size_t maximum, std::stop_token stop)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    return sqlite->list_sessions(maximum, stop);
  }
  auto append_events(const SessionId& session, std::span<const RunEvent> events,
                     std::stop_token stop)
      -> std::expected<void, storage::SessionStoreError> override {
    if (refuse_observation &&
        std::ranges::any_of(events, [](const auto& event) {
          return std::holds_alternative<OpsObservationRecorded>(event.payload);
        })) {
      ++refused;
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::io_failure,
                                     "fixed append refusal", false});
    }
    return sqlite->append_events(session, events, stop);
  }
  auto replay_events(const SessionId& session, std::stop_token stop)
      -> std::expected<std::vector<RunEvent>,
                       storage::SessionStoreError> override {
    if (refuse_replay && session != id<SessionId>("session"))
      return std::unexpected(
          storage::SessionStoreError{storage::SessionStoreErrorCode::io_failure,
                                     "fixed replay refusal", false});
    return sqlite->replay_events(session, stop);
  }
};
struct SourceState {
  std::atomic<unsigned> preparations{}, observations{}, destroyed{}, owner_io{};
  const std::thread::id owner{std::this_thread::get_id()};
  std::shared_ptr<Gate> preparation_gate, observation_gate;
  bool preparation_failure{}, observation_failure{};
};
class Source final : public runtime::OpsObservationSource {
 public:
  Source(runtime::OpsSourcePreparationIdentity identity,
         std::shared_ptr<SourceState> state)
      : m_binding{std::move(identity.target_id),
                  std::move(identity.configuration_revision),
                  LinuxOpsIdentity{LinuxExecutionScope::container,
                                   "12345678-1234-1234-1234-123456789abc", 42,
                                   43}},
        m_state(std::move(state)) {}
  ~Source() override { ++m_state->destroyed; }
  auto guarantees_bound_read_only_observations() const noexcept
      -> bool override {
    return true;
  }
  auto target_binding() const noexcept -> const OpsTargetBinding& override {
    return m_binding;
  }
  auto observe(const OpsObservationRequest& request, std::stop_token)
      -> std::expected<OpsObservation,
                       runtime::OpsObservationSourceError> override {
    ++m_state->observations;
    if (std::this_thread::get_id() == m_state->owner) ++m_state->owner_io;
    if (m_state->observation_gate) m_state->observation_gate->wait();
    if (m_state->observation_failure)
      return std::unexpected(
          runtime::OpsObservationSourceError::permission_denied);
    OpsObservation result{
        request,
        EventTimestamp{1000ms},
        EventTimestamp{1001ms},
        OpsObservationCompleteness::partial,
        {},
        0,
        {},
        LinuxHealthObservation{
            OpsHealthState::unknown, 12,
            LinuxMemoryObservation{OpsMemoryScope::kernel, 1024, 512}}};
    if (request.operation == OpsObservationOperation::linux_services)
      result.payload =
          LinuxServicesObservation{{{{"example.service", {}},
                                     OpsServiceState::failed,
                                     OpsObservationReason::failed_exit,
                                     7,
                                     3}}};
    else if (request.operation == OpsObservationOperation::linux_service_health)
      result.payload = LinuxServiceObservation{
          std::get<LinuxServiceIdentity>(request.resource),
          OpsServiceState::active, OpsObservationReason::none, 0, 2};
    return result;
  }

 private:
  const OpsTargetBinding m_binding;
  std::shared_ptr<SourceState> m_state;
};
class Factory final : public runtime::OpsSourcePreparationFactory {
 public:
  Factory(runtime::OpsSourcePreparationIdentity identity,
          std::shared_ptr<SourceState> state)
      : m_identity(std::move(identity)), m_state(std::move(state)) {}
  auto guarantees_owned_read_only_preparation() const noexcept
      -> bool override {
    return true;
  }
  auto preparation_identity() const noexcept
      -> const runtime::OpsSourcePreparationIdentity& override {
    return m_identity;
  }
  auto prepare(const runtime::OpsSourcePreparationRequest&, std::stop_token)
      -> std::expected<runtime::PreparedOpsSource,
                       runtime::OpsObservationSourceError> override {
    ++m_state->preparations;
    if (std::this_thread::get_id() == m_state->owner) ++m_state->owner_io;
    if (m_state->preparation_gate) m_state->preparation_gate->wait();
    if (m_state->preparation_failure)
      return std::unexpected(runtime::OpsObservationSourceError::unavailable);
    return runtime::PreparedOpsSource{
        std::make_shared<Source>(m_identity, m_state)};
  }

 private:
  const runtime::OpsSourcePreparationIdentity m_identity;
  std::shared_ptr<SourceState> m_state;
};
class Catalog final : public surfaces::AdminSourceCatalog {
 public:
  const std::vector<surfaces::AdminTargetChoice> choices{
      {id<OpsTargetId>("alpha"), "Alpha Linux", OpsTargetKind::linux_local},
      {id<OpsTargetId>("beta"), "Beta Linux", OpsTargetKind::linux_local}};
  std::shared_ptr<SourceState> alpha{std::make_shared<SourceState>()};
  std::shared_ptr<SourceState> beta{std::make_shared<SourceState>()};
  unsigned factories{};
  auto guarantees_owned_metadata() const noexcept -> bool override {
    return true;
  }
  auto targets() const noexcept
      -> std::span<const surfaces::AdminTargetChoice> override {
    return choices;
  }
  auto factory(OpsTargetId target, OpsConfigurationRevision revision)
      -> std::expected<std::shared_ptr<runtime::OpsSourcePreparationFactory>,
                       runtime::OpsObservationSourceError> override {
    ++factories;
    if (target != choices[0].id && target != choices[1].id)
      return std::unexpected(runtime::OpsObservationSourceError::unavailable);
    auto state = target == choices[0].id ? alpha : beta;
    return std::make_shared<Factory>(
        runtime::OpsSourcePreparationIdentity{
            std::move(target), std::move(revision), OpsTargetKind::linux_local},
        std::move(state));
  }
  auto release() -> void {
    for (const auto& state : {alpha, beta}) {
      if (state->preparation_gate) state->preparation_gate->release();
      if (state->observation_gate) state->observation_gate->release();
    }
  }
};
class Models final : public backend::ModelContextProvider {
 public:
  unsigned calls{};
  bool unavailable{}, supports_tools{true};
  auto lookup(const ModelId& model, std::stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    ++calls;
    if (unavailable)
      return std::unexpected(
          backend::BackendError{backend::BackendErrorKind::unavailable,
                                "fixed model refusal",
                                false,
                                {}});
    return backend::ModelContextInfo{
        model, 32768, 256, {}, {{"tools", supports_tools}}};
  }
};
struct BackendState {
  std::atomic<unsigned> starts{}, nexts{};
  std::shared_ptr<Gate> gate{std::make_shared<Gate>()};
};
class HeldStream final : public backend::BackendStream {
 public:
  explicit HeldStream(std::shared_ptr<BackendState> state)
      : m_state(std::move(state)) {}
  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    const auto call = ++m_state->nexts;
    if (call == 1)
      return backend::BackendEvent{backend::ResponseStarted{"fixture"}};
    if (call == 2) {
      m_state->gate->wait();
      return backend::BackendEvent{
          backend::ResponseFinished{FinishReason::stop}};
    }
    return std::nullopt;
  }

 private:
  std::shared_ptr<BackendState> m_state;
};
class Backend final : public backend::Backend {
 public:
  std::shared_ptr<BackendState> state{std::make_shared<BackendState>()};
  bool unavailable{true};
  auto start(backend::BackendRequest, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    ++state->starts;
    if (unavailable)
      return std::unexpected(
          backend::BackendError{backend::BackendErrorKind::unavailable,
                                "fixed backend refusal",
                                false,
                                {}});
    return std::make_unique<HeldStream>(state);
  }
};
class Editor final : public surfaces::DraftEditor {
 public:
  auto edit(std::string_view text, std::stop_token)
      -> std::expected<std::string, surfaces::DraftEditorError> override {
    return std::string{text};
  }
};
inline auto key(termforge::Key value, char32_t character = 0, bool ctrl = false)
    -> termforge::Event {
  return termforge::KeyEvent{value, character, ctrl,
                             false, false,     termforge::KeyAction::Press};
}
inline auto rendered(adapters::InteractiveChatApp& app, int cols = 120,
                     int rows = 32) -> std::string {
  termforge::Screen screen{cols, rows};
  app.on_render(screen);
  if (auto* overlay = app.top_overlay()) overlay->draw(screen);
  std::string text;
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x)
      text += screen.text_at(x, y);
    text += '\n';
  }
  return text;
}
template <class T> auto count(std::span<const RunEvent> events) -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(events, [](const auto& event) {
        return std::holds_alternative<T>(event.payload);
      }));
}
struct Fixture {
  Directory directory;
  Store store{directory.path};
  Models models;
  Backend backend;
  Editor editor;
  std::shared_ptr<Catalog> catalog{std::make_shared<Catalog>()};
  std::shared_ptr<std::atomic<unsigned>> wakes{
      std::make_shared<std::atomic<unsigned>>()};
  std::unique_ptr<adapters::InteractiveChatApp> app;
  std::shared_ptr<runtime::LocalSourceGrantFactory> local_factory;
  unsigned initial_lookups{};
  std::size_t previous_command_bytes{};
  bool wake_only{};
  ~Fixture() {
    catalog->release();
    backend.state->gate->release();
    app.reset();
  }
  auto open(runtime::ApprovalMode mode = runtime::ApprovalMode::allow_all,
            bool polling = false, bool with_catalog = true,
            bool profile_off = false) -> void {
    adapters::InteractiveChatAppOptions options;
    options.live_wake_enabled = false;
    options.local_source_factory = local_factory;
    options.poll_worker_updates = polling;
    options.wake_observer = [state = wakes] { ++*state; };
    if (with_catalog) options.admin_sources = catalog;
    auto identity = std::make_shared<std::uint64_t>(1000);
    options.session_dependencies.identity_suffix_source = [identity] {
      return ++*identity;
    };
    runtime::ApplicationLaunchContextConfiguration configuration;
    configuration.approval_mode = mode;
    auto launch = runtime::make_application_launch_context(configuration);
    REQUIRE(launch);
    runtime::ToolRegistry registry;
    options.session_dependencies.tools = registry.snapshot().value();
    auto policy = runtime::make_tool_launch_policy(
        options.session_dependencies.tools,
        {id<PermissionProfileId>("observe"), *launch, {}});
    REQUIRE(policy);
    options.session_dependencies.tool_policy = *policy;
    options.session_dependencies.permission_profile_id =
        id<PermissionProfileId>("observe");
    if (profile_off)
      options.session_dependencies.model_tool_profile_maximums.emplace(
          id<ModelId>("model"), id<ToolProfileId>("off"));
    app = adapters::make_interactive_chat_app(
        backend, models, &store,
        {id<ModelId>("model"), surfaces::ChatSessionOpen::Mode::resume,
         id<SessionId>("session"),
         RunProvenance{
             "test", "fake", {}, id<ModelId>("model"), {}, {}, {}, {}}},
        editor, {}, std::move(options));
    INFO(app->setup_error().message);
    REQUIRE(app->ready());
    static_cast<void>(rendered(*app));
    app->on_start();
    initial_lookups = models.calls;
  }
  auto command(std::string text) -> void {
    REQUIRE_FALSE(app->modal());
    app->dispatch_event(key(termforge::Key::End));
    for (std::size_t i{}; i < previous_command_bytes; ++i)
      app->dispatch_event(key(termforge::Key::Backspace));
    previous_command_bytes = text.size();
    app->dispatch_event(termforge::PasteEvent{std::move(text)});
    app->dispatch_event(key(termforge::Key::Enter));
    static_cast<void>(rendered(*app));
  }
  auto click(std::string_view label) -> bool {
    termforge::Screen screen{120, 32};
    app->on_render(screen);
    if (auto* overlay = app->top_overlay()) overlay->draw(screen);
    for (int row{}; row < 32; ++row)
      for (int col{}; col < 120; ++col) {
        std::string text;
        for (std::size_t i{};
             i < label.size() && col + static_cast<int>(i) < 120; ++i)
          text += screen.text_at(col + static_cast<int>(i), row);
        if (text == label) {
          app->dispatch_event(termforge::MouseEvent{col, row, 0, true});
          return true;
        }
      }
    return false;
  }
  auto press(char32_t character) -> void {
    app->dispatch_event(key(termforge::Key::Char, character));
    static_cast<void>(rendered(*app));
  }
  auto step() -> void {
    if (wake_only)
      app->dispatch_event(termforge::ErrorEvent{
          termforge::Severity::Info, "aiforge.runtime", "events-ready"});
    else
      app->on_tick(1ms);
    static_cast<void>(rendered(*app));
  }
  template <class Predicate>
  auto wait(Predicate predicate, std::string_view label = "condition",
            std::source_location caller = std::source_location::current())
      -> void {
    const bool finished = until([&] {
      step();
      return predicate();
    });
    INFO("wait " << label << " at " << caller.file_name() << ':'
                 << caller.line());
    INFO("status=" << app->status_text() << " modal=" << app->modal());
    INFO("alpha prepared/observed/destroyed="
         << catalog->alpha->preparations.load() << '/'
         << catalog->alpha->observations.load() << '/'
         << catalog->alpha->destroyed.load());
    INFO("beta prepared/observed/destroyed="
         << catalog->beta->preparations.load() << '/'
         << catalog->beta->observations.load() << '/'
         << catalog->beta->destroyed.load());
    INFO("observations/completed/failed="
         << count<OpsObservationRecorded>(app->events()) << '/'
         << count<RunCompleted>(app->events()) << '/'
         << count<RunFailed>(app->events()));
    INFO("visible busy=" << (rendered(*app).find(
                                 "Session or source worker is busy") !=
                             std::string::npos));
    REQUIRE(finished);
  }
  auto wait_for_gate(const std::shared_ptr<Gate>& gate) -> void {
    wait([&] {
      const std::lock_guard lock{gate->mutex};
      return gate->entered;
    });
  }
  auto close() -> void {
    for (unsigned i{}; i < 2 && app->modal(); ++i)
      app->dispatch_event(key(termforge::Key::Escape));
    REQUIRE_FALSE(app->modal());
  }
  auto select(std::string name = "alpha") -> void {
    command("/admin select " + name);
    wait([&] {
      return rendered(*app).find("Active target: " + name) != std::string::npos;
    });
  }
  auto history(std::string name = "session") -> std::vector<RunEvent> {
    auto events =
        store.sqlite->replay_events(id<SessionId>(std::move(name)), {});
    REQUIRE(events);
    return std::move(*events);
  }
  auto completed(std::size_t number) -> void {
    wait(
        [&] {
          return count<OpsObservationRecorded>(app->events()) == number &&
                 count<RunCompleted>(app->events()) == number;
        },
        "completed observations " + std::to_string(number));
  }
  auto no_model() const -> void {
    CHECK(backend.state->starts.load() == 0);
    CHECK(models.calls == initial_lookups);
    CHECK(catalog->alpha->owner_io.load() == 0);
    CHECK(catalog->beta->owner_io.load() == 0);
  }
};
} // namespace chat_admin_test
