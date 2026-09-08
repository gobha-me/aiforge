#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <variant>

#include <aiforge/runtime/local_context_controller.hpp>
#include <aiforge/runtime/local_source_grant.hpp>
#include <aiforge/runtime/repository_context_controller.hpp>

namespace aiforge::runtime {

using LocalSourceWorkRequest =
    std::variant<LocalListRequest, LocalPreviewRequest, LocalRevalidateRequest>;
using LocalSourceWorkResult =
    std::variant<LocalListResult, LocalPreviewResult, LocalReadResult>;
struct LocalSourceWorkCompletion {
  LocalSourceRequestToken token;
  std::expected<LocalSourceWorkResult, domain::LocalSourceError> result;
};
struct LocalFolderGrantCompletion {
  LocalFolderGrantToken token;
  std::expected<LocalFolderGrantResult, domain::LocalSourceError> result;
};
struct LocalContextWorkToken {
  domain::SessionId session_id;
  std::uint64_t session_epoch{};
  std::uint64_t request_id{};
  std::uint64_t selection_revision{};
  auto operator==(const LocalContextWorkToken&) const -> bool = default;
};
struct LocalContextWorkRequest {
  LocalContextWorkToken token;
  // New preparation stays unsealed. Revalidation restores the original
  // sealed membership, including omissions, without selecting again.
  std::variant<LocalContextRequest, domain::LocalContextAdmission> operation;
};
struct LocalContextWorkCompletion {
  LocalContextWorkToken token;
  std::expected<PreparedLocalContext, domain::LocalContextError> result;
};

struct RepositoryContextWorkToken {
  domain::SessionId session_id;
  std::uint64_t session_epoch{};
  std::uint64_t request_id{};
  std::uint64_t selection_revision{};
  auto operator==(const RepositoryContextWorkToken&) const -> bool = default;
};
struct RepositoryContextWorkRequest {
  RepositoryContextWorkToken token;
  std::variant<RepositoryContextRequest, domain::RepositoryContextAdmission>
      operation;
};
struct RepositoryContextWorkCompletion {
  RepositoryContextWorkToken token;
  std::expected<PreparedRepositoryContext, domain::RepositoryContextError>
      result;
};

enum class LocalSourceWorkerErrorCode {
  invalid_request,
  busy,
  stale_request,
  internal_failure
};
struct LocalSourceWorkerError {
  LocalSourceWorkerErrorCode code{LocalSourceWorkerErrorCode::invalid_request};
  std::string message;
};

// Application-owned, single owner-thread API. Keep one controller across
// session switches so abandoned reads cannot escape its application-wide
// capacity. Workers own their port, immutable request and private completion
// state only.
class LocalSourceWorker final {
 public:
  static constexpr std::size_t maximum_capacity{8};
  [[nodiscard]] static auto create(std::size_t capacity = 2)
      -> std::expected<std::unique_ptr<LocalSourceWorker>,
                       LocalSourceWorkerError>;
  ~LocalSourceWorker();
  LocalSourceWorker(const LocalSourceWorker&) = delete;
  auto operator=(const LocalSourceWorker&) -> LocalSourceWorker& = delete;
  LocalSourceWorker(LocalSourceWorker&&) = delete;
  auto operator=(LocalSourceWorker&&) -> LocalSourceWorker& = delete;

  // Successful submissions require strictly increasing request IDs across this
  // controller, including sessions. This bounds stale-token tracking to one
  // high-water mark. Failed validation/busy does not consume the request ID;
  // an admitted slot consumes it even if thread creation subsequently fails.
  // The reader, not a constructed token, establishes live filesystem authority.
  // The same owning reader may receive concurrent calls up to this capacity.
  [[nodiscard]] auto submit(const std::shared_ptr<LocalSourceReader>& reader,
                            LocalSourceWorkRequest request)
      -> std::expected<void, LocalSourceWorkerError>;
  [[nodiscard]] auto submit(
      const std::shared_ptr<LocalSourceGrantFactory>& factory,
      LocalFolderGrantRequest request)
      -> std::expected<void, LocalSourceWorkerError>;
  // Concurrent slots may use one controller; its owned resolver must support
  // concurrent read-only lookups. No lease authority enters the completion.
  [[nodiscard]] auto submit(
      const std::shared_ptr<LocalContextController>& controller,
      LocalContextWorkRequest request)
      -> std::expected<void, LocalSourceWorkerError>;
  // Borrowed controllers are refused. The owning source graph must support
  // concurrent read-only calls; it is released on the producer before
  // retirement.
  [[nodiscard]] auto submit(
      const std::shared_ptr<RepositoryContextController>& controller,
      RepositoryContextWorkRequest request)
      -> std::expected<void, LocalSourceWorkerError>;
  [[nodiscard]] auto poll(const RepositoryContextWorkToken& token)
      -> std::expected<std::optional<RepositoryContextWorkCompletion>,
                       LocalSourceWorkerError>;
  [[nodiscard]] auto cancel(const RepositoryContextWorkToken& token)
      -> std::expected<void, LocalSourceWorkerError>;
  // Null means still working. A completion is moved out exactly once; every
  // lookup compares the entire token, including root/lease/selection revision.
  [[nodiscard]] auto poll(const LocalSourceRequestToken& token)
      -> std::expected<std::optional<LocalSourceWorkCompletion>,
                       LocalSourceWorkerError>;
  [[nodiscard]] auto cancel(const LocalSourceRequestToken& token)
      -> std::expected<void, LocalSourceWorkerError>;
  [[nodiscard]] auto poll(const LocalFolderGrantToken& token)
      -> std::expected<std::optional<LocalFolderGrantCompletion>,
                       LocalSourceWorkerError>;
  [[nodiscard]] auto cancel(const LocalFolderGrantToken& token)
      -> std::expected<void, LocalSourceWorkerError>;
  [[nodiscard]] auto poll(const LocalContextWorkToken& token)
      -> std::expected<std::optional<LocalContextWorkCompletion>,
                       LocalSourceWorkerError>;
  [[nodiscard]] auto cancel(const LocalContextWorkToken& token)
      -> std::expected<void, LocalSourceWorkerError>;
  [[nodiscard]] auto invalidate_session(const domain::SessionId& session_id)
      -> std::expected<void, LocalSourceWorkerError>;
  [[nodiscard]] auto occupied_slots() const -> std::size_t;
  // Bounded readiness metadata only; leaves every completion owned by its slot.
  [[nodiscard]] auto ready_results() const -> std::size_t;
  [[nodiscard]] auto ready_result(const RepositoryContextWorkToken& token) const
      -> bool;

 private:
  struct Impl;
  explicit LocalSourceWorker(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

// Cancellation/destruction discard delivery immediately, without joining or
// calling reader stop callbacks on the owner thread. Each slot has at most one
// reader and one cancellation relay (at most 2 * capacity activities); it
// remains occupied until both finish. No replacements bypass retired slots.
// Completed unconsumed values also occupy a slot. Cancelling cannot interrupt a
// stalled OS call or arbitrary stop callback. Teardown releases controller
// state; detached activities retain only their bounded owning job state until
// finished.
// Grant results remain producer-owned until claimed or discarded. Discarded
// leases are destroyed on that producer, outside the job mutex, before slot
// retirement. Successful polling transfers lease cleanup to the caller's live
// registry; it is not managed by this worker. A consumed grant slot can remain
// briefly occupied until its producer exits. Submission borrows the caller's
// shared pointer until both activities start, then retains its own copy. The
// caller remains responsible for destruction of its own port/temporary owners.

} // namespace aiforge::runtime
