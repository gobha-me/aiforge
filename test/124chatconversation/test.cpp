#include <aiforge/runtime/conversation_history.hpp>
#include <aiforge/surfaces/chat_session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace {
using namespace aiforge;
using namespace std::chrono_literals;
template <class Id> auto id(const std::string& text) -> Id {
  return Id::from(text).value();
}

class Stream final : public backend::BackendStream {
 public:
  explicit Stream(domain::MessageId message) : m_message(std::move(message)) {}
  auto next(std::stop_token stop)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (stop.stop_requested())
      return backend::BackendEvent{backend::ResponseCancelled{"cancelled"}};
    switch (m_step++) {
      case 0:
        return backend::BackendEvent{backend::ResponseStarted{"response"}};
      case 1:
        return backend::BackendEvent{
            backend::ContentDelta{m_message, domain::TextBlock{"answer"}}};
      case 2:
        return backend::BackendEvent{
            backend::ResponseFinished{domain::FinishReason::stop}};
      default: return std::optional<backend::BackendEvent>{};
    }
  }

 private:
  domain::MessageId m_message;
  unsigned m_step{};
};

class Backend final : public backend::Backend,
                      public backend::ModelContextProvider {
 public:
  auto lookup(const domain::ModelId& model, std::stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    return backend::ModelContextInfo{model, window, 16};
  }
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    auto stream = std::make_unique<Stream>(request.assistant_message_id);
    std::lock_guard lock{m_mutex};
    m_requests.push_back(std::move(request));
    return stream;
  }
  auto requests() -> std::vector<backend::BackendRequest> {
    std::lock_guard lock{m_mutex};
    return m_requests;
  }
  std::uint64_t window{8192};

 private:
  std::mutex m_mutex;
  std::vector<backend::BackendRequest> m_requests;
};

class Store final : public storage::SessionStore {
 public:
  auto create_session(storage::SessionCreate value, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    info = storage::SessionInfo{value.session_id, value.created_at,
                                value.created_at, 0};
    return {};
  }
  auto open_session(const domain::SessionId&, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    return info.value();
  }
  auto list_sessions(std::size_t, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    return std::vector{info.value()};
  }
  auto append_events(const domain::SessionId&,
                     std::span<const domain::RunEvent> events, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    ++append_attempts;
    if (fail_append)
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::contention, "append refused", true});
    history.insert(history.end(), events.begin(), events.end());
    if (!events.empty()) {
      info->last_sequence = events.back().metadata.sequence;
      info->last_activity_at = events.back().metadata.timestamp;
    }
    return {};
  }
  auto replay_events(const domain::SessionId&, std::stop_token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    return history;
  }
  std::optional<storage::SessionInfo> info;
  std::vector<domain::RunEvent> history;
  bool fail_append{};
  std::size_t append_attempts{};
};

auto open(Backend& backend, Store* store = nullptr, std::stop_token stop = {})
    -> std::unique_ptr<surfaces::ChatSession> {
  surfaces::ChatSessionDependencies dependencies;
  dependencies.identity_suffix_source = [next = std::uint64_t{}]() mutable {
    return ++next;
  };
  auto session = surfaces::ChatSession::open(
      {id<domain::ModelId>("model"),
       store != nullptr ? surfaces::ChatSessionOpen::Mode::create
                        : surfaces::ChatSessionOpen::Mode::ephemeral,
       {}},
      backend, backend, store, nullptr, stop, {1024 * 1024, 16},
      std::move(dependencies));
  REQUIRE(session);
  return std::move(*session);
}
auto finish(surfaces::ChatSession& session) -> void {
  for (unsigned attempt = 0; attempt < 1000 && session.active(); ++attempt) {
    auto events = session.drain();
    REQUIRE(events);
    if (events->empty()) std::this_thread::sleep_for(1ms);
  }
  REQUIRE_FALSE(session.active());
}
auto admission(const surfaces::ChatSubmission& submission)
    -> domain::ConversationAdmission {
  for (const auto& event : submission.committed_events) {
    if (const auto* start = std::get_if<domain::RunStarted>(&event.payload)) {
      REQUIRE(start->conversation_admission);
      return *start->conversation_admission;
    }
  }
  FAIL("submission has no RunStarted event");
  std::terminate();
}
} // namespace

TEST_CASE("Chat policy rejects stale malformed and ineligible decisions "
          "without mutation") {
  Backend backend;
  auto session = open(backend);
  auto revision = std::uint64_t{};
  auto mode = domain::ConversationMode::rolling;
  std::vector<domain::RunId> pins;
  SECTION("stale revision") {
    revision = 1;
  }
  SECTION("unknown mode") {
    mode = static_cast<domain::ConversationMode>(255);
  }
  SECTION("missing pin") {
    pins.push_back(id<domain::RunId>("missing"));
  }
  SECTION("duplicate pin") {
    pins.assign(2, id<domain::RunId>("missing"));
  }
  const auto before = session->event_log().events();
  const auto result = session->set_conversation_policy(revision, mode, pins);
  REQUIRE_FALSE(result);
  CHECK_FALSE(result.error().effect_may_have_applied);
  CHECK(session->event_log().events() == before);
  REQUIRE(session->conversation_policy());
  CHECK(session->conversation_policy()->policy.revision == 0);
  CHECK(backend.requests().empty());
}

TEST_CASE("Chat policy cancellation and failed storage preserve the previous "
          "snapshot") {
  Backend backend;
  Store store;
  std::stop_source stop;
  auto session = open(backend, &store, stop.get_token());
  auto first =
      session->set_conversation_policy(0, domain::ConversationMode::rolling);
  REQUIRE(first);
  REQUIRE(first->size() == 3);
  REQUIRE(std::get<domain::RunStarted>(first->front().payload).purpose ==
          domain::RunPurpose::control);
  const auto previous = session->conversation_policy();
  REQUIRE(previous);
  const auto history = store.history;
  SECTION("cancelled request") {
    stop.request_stop();
  }
  SECTION("failed append") {
    store.fail_append = true;
  }
  auto changed =
      session->set_conversation_policy(1, domain::ConversationMode::full);
  REQUIRE_FALSE(changed);
  CHECK_FALSE(changed.error().effect_may_have_applied);
  CHECK(session->conversation_policy() == previous);
  CHECK(store.history == history);
  CHECK(session->event_log().events() == history);
  CHECK(backend.requests().empty());
}

TEST_CASE("Chat full history refuses tiny capacity and rolling admits the next "
          "turn") {
  Backend backend;
  auto session = open(backend);
  auto first = session->submit(std::string(3000, 'x'));
  REQUIRE(first);
  finish(*session);
  const auto requests = backend.requests();
  REQUIRE(requests.size() == 1);
  backend.window =
      requests.front().context.entries.front().estimated_tokens + 16 + 6 + 10;
  REQUIRE(session->select_model(id<domain::ModelId>("tiny")));
  const auto before = session->event_log().events();
  auto rejected = session->submit("second");
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code ==
        surfaces::ChatSessionErrorCode::context_failed);
  CHECK(session->event_log().events() == before);
  CHECK(backend.requests().size() == 1);
  auto changed =
      session->set_conversation_policy(0, domain::ConversationMode::rolling);
  REQUIRE(changed);
  CHECK(changed->size() == 3);
  auto second = session->submit("second");
  REQUIRE(second);
  const auto selected = admission(*second);
  CHECK(selected.policy_revision == 1);
  CHECK(selected.mode == domain::ConversationMode::rolling);
  CHECK(selected.groups.empty());
  CHECK(selected.omitted_group_count == 1);
  finish(*session);
  REQUIRE(backend.requests().size() == 2);
  CHECK(backend.requests().back().context.entries.size() == 2);
  REQUIRE(session->set_conversation_policy(1, domain::ConversationMode::full));
  CHECK_FALSE(session->submit("third"));
  CHECK(backend.requests().size() == 2);
}

TEST_CASE("Chat pins remain mandatory and cannot silently disappear") {
  Backend backend;
  auto session = open(backend);
  auto first = session->submit(std::string(2000, 'x'));
  REQUIRE(first);
  finish(*session);
  REQUIRE(session->submit("newer"));
  finish(*session);
  REQUIRE(session->set_conversation_policy(0, domain::ConversationMode::rolling,
                                           {first->run_id}));
  auto groups =
      runtime::reconstruct_conversation_history({session->event_log()});
  REQUIRE(groups);
  REQUIRE(groups->size() == 2);
  std::uint64_t pin_tokens{};
  for (const auto& entry : groups->front().entries)
    pin_tokens += entry.content.estimated_tokens;
  backend.window =
      backend.requests().front().context.entries.front().estimated_tokens + 16 +
      4 + pin_tokens;
  REQUIRE(session->select_model(id<domain::ModelId>("pin-capacity")));
  auto pinned = session->submit("next");
  REQUIRE(pinned);
  const auto selected = admission(*pinned);
  REQUIRE(selected.groups.size() == 1);
  CHECK(selected.groups.front().run_id == first->run_id);
  CHECK(selected.groups.front().pinned);
  finish(*session);
  --backend.window;
  REQUIRE(session->select_model(id<domain::ModelId>("too-small")));
  const auto before = session->event_log().events();
  REQUIRE_FALSE(session->submit("next"));
  CHECK(session->event_log().events() == before);
  CHECK(backend.requests().size() == 3);
}

TEST_CASE("Chat policy changes affect subsequent turns while active admission "
          "stays fixed") {
  Backend backend;
  auto session = open(backend);
  auto running = session->submit("first");
  REQUIRE(running);
  REQUIRE(session->active());
  const auto active_admission = admission(*running);
  REQUIRE(active_admission.policy_revision == 0);
  REQUIRE(
      session->set_conversation_policy(0, domain::ConversationMode::rolling));
  REQUIRE(session->active());
  CHECK(session->conversation_policy()->policy.revision == 1);
  auto invalid_pin = session->set_conversation_policy(
      1, domain::ConversationMode::rolling, {running->run_id});
  REQUIRE_FALSE(invalid_pin);
  CHECK(session->conversation_policy()->policy.revision == 1);
  const auto& started = std::get<domain::RunStarted>(
      session->event_log().events().front().payload);
  REQUIRE(started.conversation_admission);
  CHECK(*started.conversation_admission == active_admission);
  finish(*session);
  CHECK(backend.requests().size() == 1);
  auto next = session->submit("next");
  REQUIRE(next);
  CHECK(admission(*next).policy_revision == 1);
  finish(*session);
  CHECK(backend.requests().size() == 2);
}
