#include <aiforge/surfaces/chat_session.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace aiforge;

template <typename IdType>
auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

class Stream final : public backend::BackendStream {
 public:
  Stream(domain::MessageId message_id, std::string answer)
      : m_message_id(std::move(message_id)), m_answer(std::move(answer)) {}

  auto next(std::stop_token stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (stop_token.stop_requested()) {
      return backend::BackendEvent{backend::ResponseCancelled{"cancelled"}};
    }
    switch (m_step++) {
      case 0:
        return backend::BackendEvent{backend::ResponseStarted{"response"}};
      case 1:
        return backend::BackendEvent{
            backend::ContentDelta{m_message_id, domain::TextBlock{m_answer}}};
      case 2:
        return backend::BackendEvent{backend::UsageObserved{{2, 1, 0, 0}}};
      case 3:
        return backend::BackendEvent{
            backend::ResponseFinished{domain::FinishReason::stop}};
      default:
        return std::optional<backend::BackendEvent>{};
    }
  }

 private:
  domain::MessageId m_message_id;
  std::string m_answer;
  int m_step{};
};

class Backend final : public backend::Backend,
                      public backend::ModelContextProvider {
 public:
  auto lookup(const domain::ModelId& model_id, std::stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    return backend::ModelContextInfo{model_id, 100000, 4096};
  }

  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    requests.push_back(request);
    return std::make_unique<Stream>(
        request.assistant_message_id,
        "answer-" + std::to_string(requests.size()));
  }

  std::vector<backend::BackendRequest> requests;
};

class MemoryStore final : public storage::SessionStore {
 public:
  auto create_session(storage::SessionCreate session, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    if (sessions.contains(session.session_id)) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::already_exists, "exists", false});
    }
    sessions.emplace(
        session.session_id,
        storage::SessionInfo{session.session_id, session.created_at,
                             session.created_at, 0});
    return {};
  }

  auto open_session(const domain::SessionId& session_id, std::stop_token)
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override {
    const auto found = sessions.find(session_id);
    if (found == sessions.end()) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "missing", false});
    }
    return found->second;
  }

  auto list_sessions(std::size_t, std::stop_token)
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override {
    std::vector<storage::SessionInfo> result;
    for (const auto& [id, info] : sessions) {
      static_cast<void>(id);
      result.push_back(info);
    }
    return result;
  }

  auto append_events(const domain::SessionId& session_id,
                     std::span<const domain::RunEvent> events, std::stop_token)
      -> std::expected<void, storage::SessionStoreError> override {
    const auto found = sessions.find(session_id);
    if (found == sessions.end()) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "missing", false});
    }
    auto& history = histories[session_id];
    if (!history.empty() && !events.empty() &&
        events.front().metadata.sequence <= history.back().metadata.sequence) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::conflict, "sequence", false});
    }
    history.insert(history.end(), events.begin(), events.end());
    if (!events.empty()) {
      found->second.last_sequence = events.back().metadata.sequence;
      found->second.last_activity_at = events.back().metadata.timestamp;
    }
    return {};
  }

  auto replay_events(const domain::SessionId& session_id, std::stop_token)
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override {
    if (!sessions.contains(session_id)) {
      return std::unexpected(storage::SessionStoreError{
          storage::SessionStoreErrorCode::not_found, "missing", false});
    }
    return histories[session_id];
  }

  std::map<domain::SessionId, storage::SessionInfo> sessions;
  std::map<domain::SessionId, std::vector<domain::RunEvent>> histories;
};

auto drain_to_end(surfaces::ChatSession& session)
    -> std::vector<domain::RunEvent> {
  std::vector<domain::RunEvent> result;
  for (int attempt = 0; attempt < 200 && session.active(); ++attempt) {
    auto events = session.drain();
    REQUIRE(events);
    result.insert(result.end(), events->begin(), events->end());
    if (events->empty()) std::this_thread::sleep_for(2ms);
  }
  REQUIRE_FALSE(session.active());
  return result;
}

auto text_messages(const backend::BackendRequest& request,
                   const domain::Role role) -> std::vector<std::string> {
  std::vector<std::string> result;
  for (const auto& entry : request.context.entries) {
    if (entry.message.role != role) continue;
    for (const auto& block : entry.message.content) {
      if (const auto* text = std::get_if<domain::TextBlock>(&block)) {
        result.push_back(text->text);
      }
    }
  }
  return result;
}

}  // namespace

TEST_CASE("interactive submission validates before creating durable facts",
          "[chat][failure]") {
  Backend backend;
  auto session = surfaces::ChatSession::open(
      {make_id<domain::ModelId>("model"),
       surfaces::ChatSessionOpen::Mode::ephemeral, std::nullopt},
      backend, backend);
  REQUIRE(session);

  REQUIRE_FALSE((*session)->submit(""));
  REQUIRE_FALSE((*session)->submit("bad\x1btext"));
  REQUIRE((*session)->event_log().events().empty());
  REQUIRE(backend.requests.empty());

  auto limited = surfaces::ChatSession::open(
      {make_id<domain::ModelId>("model"),
       surfaces::ChatSessionOpen::Mode::ephemeral, std::nullopt},
      backend, backend, nullptr, nullptr, {}, {3, 128});
  REQUIRE(limited);
  const auto oversized = (*limited)->submit("four");
  REQUIRE_FALSE(oversized);
  REQUIRE(oversized.error().code ==
          surfaces::ChatSessionErrorCode::input_too_large);
}

TEST_CASE("interactive turns stream and reuse completed conversation context",
          "[chat]") {
  Backend backend;
  auto session = surfaces::ChatSession::open(
      {make_id<domain::ModelId>("model"),
       surfaces::ChatSessionOpen::Mode::ephemeral, std::nullopt},
      backend, backend);
  REQUIRE(session);

  auto first = (*session)->submit("first\nline");
  REQUIRE(first);
  REQUIRE(std::ranges::any_of(first->committed_events, [](const auto& event) {
    return std::holds_alternative<domain::UserContentAdded>(event.payload);
  }));
  REQUIRE((*session)->submitted_prompts() ==
          std::vector<std::string>{"first\nline"});
  const auto first_events = drain_to_end(**session);
  REQUIRE(std::ranges::any_of(first_events, [](const auto& event) {
    return std::holds_alternative<domain::RunCompleted>(event.payload);
  }));

  auto second = (*session)->submit("second");
  REQUIRE(second);
  drain_to_end(**session);
  REQUIRE(backend.requests.size() == 2);
  REQUIRE(text_messages(backend.requests[1], domain::Role::user) ==
          std::vector<std::string>{"first\nline", "second"});
  REQUIRE(text_messages(backend.requests[1], domain::Role::assistant) ==
          std::vector<std::string>{"answer-1"});
  REQUIRE((*session)->submitted_prompts() ==
          std::vector<std::string>{"first\nline", "second"});
}

TEST_CASE("durable interactive resume rebuilds history without inference",
          "[chat][storage]") {
  Backend backend;
  MemoryStore store;
  const auto id = make_id<domain::SessionId>("saved");
  {
    auto session = surfaces::ChatSession::open(
        {make_id<domain::ModelId>("model"),
         surfaces::ChatSessionOpen::Mode::create, id},
        backend, backend, &store);
    REQUIRE_FALSE(session);  // create mode never accepts a caller-supplied ID
  }

  auto created = surfaces::ChatSession::open(
      {make_id<domain::ModelId>("model"),
       surfaces::ChatSessionOpen::Mode::create, std::nullopt},
      backend, backend, &store);
  REQUIRE(created);
  const auto created_id = (*created)->session_id();
  REQUIRE((*created)->submit("persisted"));
  drain_to_end(**created);
  created->reset();
  const auto requests_before = backend.requests.size();

  auto resumed = surfaces::ChatSession::open(
      {make_id<domain::ModelId>("model"),
       surfaces::ChatSessionOpen::Mode::resume, created_id},
      backend, backend, &store);
  REQUIRE(resumed);
  REQUIRE((*resumed)->submitted_prompts() ==
          std::vector<std::string>{"persisted"});
  REQUIRE(backend.requests.size() == requests_before);
}
