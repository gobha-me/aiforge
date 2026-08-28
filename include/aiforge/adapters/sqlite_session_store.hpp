#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <aiforge/storage/session_store.hpp>

namespace aiforge::adapters {

// The SQLite runtime this build is linked against, for run provenance. The
// SQLite headers stay inside this adapter.
[[nodiscard]] auto sqlite_library_version() -> std::string;

struct SessionStorePathEnvironment {
  std::optional<std::filesystem::path> xdg_state_home;
  std::optional<std::filesystem::path> home;
};

[[nodiscard]] auto resolve_session_store_path(
    const SessionStorePathEnvironment& environment)
    -> std::expected<std::filesystem::path, storage::SessionStoreError>;

[[nodiscard]] auto process_session_store_path()
    -> std::expected<std::filesystem::path, storage::SessionStoreError>;

class SqliteSessionStore final : public storage::SessionStore {
 public:
  [[nodiscard]] static auto open(std::filesystem::path path,
                                 storage::SessionStoreLimits limits = {})
      -> std::expected<std::unique_ptr<SqliteSessionStore>,
                       storage::SessionStoreError>;

  ~SqliteSessionStore() override;
  SqliteSessionStore(const SqliteSessionStore&) = delete;
  auto operator=(const SqliteSessionStore&) -> SqliteSessionStore& = delete;
  SqliteSessionStore(SqliteSessionStore&&) = delete;
  auto operator=(SqliteSessionStore&&) -> SqliteSessionStore& = delete;

  [[nodiscard]] auto path() const noexcept -> const std::filesystem::path&;

  [[nodiscard]] auto create_session(storage::SessionCreate session,
                                    std::stop_token stop_token = {})
      -> std::expected<void, storage::SessionStoreError> override;
  [[nodiscard]] auto open_session(const domain::SessionId& session_id,
                                  std::stop_token stop_token = {})
      -> std::expected<storage::SessionInfo,
                       storage::SessionStoreError> override;
  [[nodiscard]] auto list_sessions(std::size_t limit,
                                   std::stop_token stop_token = {})
      -> std::expected<std::vector<storage::SessionInfo>,
                       storage::SessionStoreError> override;
  [[nodiscard]] auto append_events(const domain::SessionId& session_id,
                                   std::span<const domain::RunEvent> events,
                                   std::stop_token stop_token = {})
      -> std::expected<void, storage::SessionStoreError> override;
  [[nodiscard]] auto replay_events(const domain::SessionId& session_id,
                                   std::stop_token stop_token = {})
      -> std::expected<std::vector<domain::RunEvent>,
                       storage::SessionStoreError> override;
  [[nodiscard]] auto replay_project_backlog(
      const domain::RepositoryId& repository_id, std::size_t maximum_sessions,
      std::stop_token stop_token = {})
      -> std::expected<std::vector<domain::ProjectBacklogSessionEvents>,
                       storage::SessionStoreError> override;

 private:
  struct Impl;
  explicit SqliteSessionStore(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> m_impl;
};

} // namespace aiforge::adapters
