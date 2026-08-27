#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <aiforge/domain/events.hpp>
#include <aiforge/domain/project_backlog_projection.hpp>

namespace aiforge::storage {

enum class SessionStoreErrorCode {
  invalid_argument,
  not_found,
  already_exists,
  conflict,
  contention,
  corrupt,
  unsupported_version,
  resource_exhausted,
  permission_denied,
  io_failure,
  cancelled,
  internal_failure,
};

struct SessionStoreError {
  SessionStoreErrorCode code{SessionStoreErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  auto operator==(const SessionStoreError&) const -> bool = default;
};

struct SessionCreate {
  domain::SessionId session_id;
  domain::EventTimestamp created_at;
  auto operator==(const SessionCreate&) const -> bool = default;
};

struct SessionInfo {
  domain::SessionId session_id;
  domain::EventTimestamp created_at;
  domain::EventTimestamp last_activity_at;
  std::uint64_t last_sequence{};
  std::uint64_t run_count{};
  auto operator==(const SessionInfo&) const -> bool = default;
};

struct SessionStoreLimits {
  std::size_t maximum_batch_events{4096};
  std::size_t maximum_payload_bytes{8U * 1024U * 1024U};
  std::size_t maximum_replay_events{1000000};
  std::size_t maximum_replay_bytes{256U * 1024U * 1024U};
  std::chrono::milliseconds busy_timeout{250};
  auto operator==(const SessionStoreLimits&) const -> bool = default;
};

class SessionStore {
 public:
  virtual ~SessionStore() = default;

  [[nodiscard]] virtual auto create_session(
      SessionCreate session, std::stop_token stop_token = {})
      -> std::expected<void, SessionStoreError> = 0;
  [[nodiscard]] virtual auto open_session(
      const domain::SessionId& session_id, std::stop_token stop_token = {})
      -> std::expected<SessionInfo, SessionStoreError> = 0;
  [[nodiscard]] virtual auto list_sessions(
      std::size_t limit, std::stop_token stop_token = {})
      -> std::expected<std::vector<SessionInfo>, SessionStoreError> = 0;
  [[nodiscard]] virtual auto append_events(
      const domain::SessionId& session_id,
      std::span<const domain::RunEvent> events,
      std::stop_token stop_token = {})
      -> std::expected<void, SessionStoreError> = 0;
  [[nodiscard]] virtual auto replay_events(
      const domain::SessionId& session_id, std::stop_token stop_token = {})
      -> std::expected<std::vector<domain::RunEvent>, SessionStoreError> = 0;
  [[nodiscard]] virtual auto
  replay_project_backlog(const domain::RepositoryId &repository_id,
                         std::size_t maximum_sessions,
                         std::stop_token stop_token = {})
      -> std::expected<std::vector<domain::ProjectBacklogSessionEvents>,
                       SessionStoreError> {
    static_cast<void>(repository_id);
    static_cast<void>(maximum_sessions);
    static_cast<void>(stop_token);
    return std::unexpected(SessionStoreError{
        SessionStoreErrorCode::unsupported_version,
        "session store does not support project-backlog queries", false});
  }
};

}  // namespace aiforge::storage
