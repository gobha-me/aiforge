#include <aiforge/testing/scripted_session_store.hpp>

#include <string>
#include <utility>

namespace aiforge::testing {
namespace {

[[nodiscard]] auto error(const storage::SessionStoreErrorCode code,
                         std::string message)
    -> storage::SessionStoreError {
  return {code, std::move(message), false};
}

}  // namespace

ScriptedSessionStore::ScriptedSessionStore(
    std::vector<SessionStoreExchange> exchanges)
    : m_exchanges(std::move(exchanges)) {}

auto ScriptedSessionStore::next(SessionStoreCall call,
                                const std::stop_token stop_token)
    -> std::expected<SessionStoreOutcome, storage::SessionStoreError> {
  if (stop_token.stop_requested()) {
    return std::unexpected(error(storage::SessionStoreErrorCode::cancelled,
                                 "session-store operation cancelled"));
  }
  m_recorded_calls.push_back(call);
  if (m_next_exchange >= m_exchanges.size()) {
    return std::unexpected(error(storage::SessionStoreErrorCode::internal_failure,
                                 "scripted session store is exhausted"));
  }
  const auto& exchange = m_exchanges[m_next_exchange];
  if (exchange.expected_call != call) {
    return std::unexpected(error(storage::SessionStoreErrorCode::internal_failure,
                                 "session-store call did not match the script"));
  }
  ++m_next_exchange;
  if (const auto* failure =
          std::get_if<storage::SessionStoreError>(&exchange.outcome)) {
    return std::unexpected(*failure);
  }
  return exchange.outcome;
}

auto ScriptedSessionStore::create_session(storage::SessionCreate session,
                                          const std::stop_token stop_token)
    -> std::expected<void, storage::SessionStoreError> {
  auto result = next(CreateSessionCall{std::move(session)}, stop_token);
  if (!result) return std::unexpected(std::move(result.error()));
  if (!std::holds_alternative<VoidSessionStoreResult>(*result)) {
    return std::unexpected(error(storage::SessionStoreErrorCode::internal_failure,
                                 "scripted create outcome has the wrong type"));
  }
  return {};
}

auto ScriptedSessionStore::open_session(
    const domain::SessionId& session_id, const std::stop_token stop_token)
    -> std::expected<storage::SessionInfo, storage::SessionStoreError> {
  auto result = next(OpenSessionCall{session_id}, stop_token);
  if (!result) return std::unexpected(std::move(result.error()));
  if (const auto* info = std::get_if<storage::SessionInfo>(&*result)) return *info;
  return std::unexpected(error(storage::SessionStoreErrorCode::internal_failure,
                               "scripted open outcome has the wrong type"));
}

auto ScriptedSessionStore::list_sessions(const std::size_t limit,
                                         const std::stop_token stop_token)
    -> std::expected<std::vector<storage::SessionInfo>,
                     storage::SessionStoreError> {
  auto result = next(ListSessionsCall{limit}, stop_token);
  if (!result) return std::unexpected(std::move(result.error()));
  if (const auto* sessions =
          std::get_if<std::vector<storage::SessionInfo>>(&*result)) {
    return *sessions;
  }
  return std::unexpected(error(storage::SessionStoreErrorCode::internal_failure,
                               "scripted list outcome has the wrong type"));
}

auto ScriptedSessionStore::append_events(
    const domain::SessionId& session_id,
    const std::span<const domain::RunEvent> events,
    const std::stop_token stop_token)
    -> std::expected<void, storage::SessionStoreError> {
  auto result = next(
      AppendEventsCall{session_id, {events.begin(), events.end()}}, stop_token);
  if (!result) return std::unexpected(std::move(result.error()));
  if (!std::holds_alternative<VoidSessionStoreResult>(*result)) {
    return std::unexpected(error(storage::SessionStoreErrorCode::internal_failure,
                                 "scripted append outcome has the wrong type"));
  }
  return {};
}

auto ScriptedSessionStore::replay_events(
    const domain::SessionId& session_id, const std::stop_token stop_token)
    -> std::expected<std::vector<domain::RunEvent>,
                     storage::SessionStoreError> {
  auto result = next(ReplayEventsCall{session_id}, stop_token);
  if (!result) return std::unexpected(std::move(result.error()));
  if (const auto* events =
          std::get_if<std::vector<domain::RunEvent>>(&*result)) {
    return *events;
  }
  return std::unexpected(error(storage::SessionStoreErrorCode::internal_failure,
                               "scripted replay outcome has the wrong type"));
}

auto ScriptedSessionStore::replay_project_backlog(
    const domain::RepositoryId &repository_id,
    const std::size_t maximum_sessions, const std::stop_token stop_token)
    -> std::expected<std::vector<domain::ProjectBacklogSessionEvents>,
                     storage::SessionStoreError> {
  auto result = next(ReplayProjectBacklogCall{repository_id, maximum_sessions},
                     stop_token);
  if (!result)
    return std::unexpected(std::move(result.error()));
  if (const auto *histories =
          std::get_if<std::vector<domain::ProjectBacklogSessionEvents>>(
              &*result)) {
    return *histories;
  }
  return std::unexpected(
      error(storage::SessionStoreErrorCode::internal_failure,
            "scripted project-backlog replay outcome has the wrong type"));
}

auto ScriptedSessionStore::recorded_calls() const noexcept
    -> const std::vector<SessionStoreCall>& {
  return m_recorded_calls;
}

auto ScriptedSessionStore::remaining_exchanges() const noexcept -> std::size_t {
  return m_exchanges.size() - m_next_exchange;
}

}  // namespace aiforge::testing
