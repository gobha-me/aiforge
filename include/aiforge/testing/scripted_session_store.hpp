#pragma once

#include <cstddef>
#include <expected>
#include <variant>
#include <vector>

#include <aiforge/storage/session_store.hpp>

namespace aiforge::testing {

struct CreateSessionCall {
  storage::SessionCreate session;
  auto operator==(const CreateSessionCall&) const -> bool = default;
};

struct OpenSessionCall {
  domain::SessionId session_id;
  auto operator==(const OpenSessionCall&) const -> bool = default;
};

struct ListSessionsCall {
  std::size_t limit{};
  auto operator==(const ListSessionsCall&) const -> bool = default;
};

struct AppendEventsCall {
  domain::SessionId session_id;
  std::vector<domain::RunEvent> events;
  auto operator==(const AppendEventsCall&) const -> bool = default;
};

struct ReplayEventsCall {
  domain::SessionId session_id;
  auto operator==(const ReplayEventsCall&) const -> bool = default;
};

using SessionStoreCall =
    std::variant<CreateSessionCall, OpenSessionCall, ListSessionsCall,
                 AppendEventsCall, ReplayEventsCall>;

struct VoidSessionStoreResult {
  auto operator==(const VoidSessionStoreResult&) const -> bool = default;
};

using SessionStoreOutcome =
    std::variant<VoidSessionStoreResult, storage::SessionInfo,
                 std::vector<storage::SessionInfo>,
                 std::vector<domain::RunEvent>, storage::SessionStoreError>;

struct SessionStoreExchange {
  SessionStoreCall expected_call;
  SessionStoreOutcome outcome;
  auto operator==(const SessionStoreExchange&) const -> bool = default;
};

class ScriptedSessionStore final : public storage::SessionStore {
 public:
  explicit ScriptedSessionStore(std::vector<SessionStoreExchange> exchanges);

  [[nodiscard]] auto create_session(
      storage::SessionCreate session, std::stop_token stop_token = {})
      -> std::expected<void, storage::SessionStoreError> override;
  [[nodiscard]] auto open_session(
      const domain::SessionId& session_id, std::stop_token stop_token = {})
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override;
  [[nodiscard]] auto list_sessions(
      std::size_t limit, std::stop_token stop_token = {})
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override;
  [[nodiscard]] auto append_events(
      const domain::SessionId& session_id,
      std::span<const domain::RunEvent> events,
      std::stop_token stop_token = {})
      -> std::expected<void, storage::SessionStoreError> override;
  [[nodiscard]] auto replay_events(
      const domain::SessionId& session_id, std::stop_token stop_token = {})
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override;

  [[nodiscard]] auto recorded_calls() const noexcept
      -> const std::vector<SessionStoreCall>&;
  [[nodiscard]] auto remaining_exchanges() const noexcept -> std::size_t;

 private:
  [[nodiscard]] auto next(SessionStoreCall call, std::stop_token stop_token)
      -> std::expected<SessionStoreOutcome, storage::SessionStoreError>;

  std::vector<SessionStoreExchange> m_exchanges;
  std::vector<SessionStoreCall> m_recorded_calls;
  std::size_t m_next_exchange{};
};

}  // namespace aiforge::testing
