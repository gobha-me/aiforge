#pragma once

#include <memory>

#include <aiforge/runtime/local_source.hpp>

namespace aiforge::runtime {

struct LocalFolderGrantToken {
  domain::SessionId session_id;
  std::uint64_t session_epoch{};
  std::uint64_t request_id{};
  auto operator==(const LocalFolderGrantToken&) const -> bool = default;
};
struct LocalFolderGrantRequest {
  LocalFolderGrantToken token;
  // Private ephemeral input only: never persisted or echoed in diagnostics.
  std::string absolute_path;
  std::uint64_t lease_generation{};
  domain::LocalSourceLimits limits;
};

class LocalSourceLease : public LocalSourceReader {
 public:
  [[nodiscard]] virtual auto root_identity() const noexcept
      -> const domain::LocalRootIdentity& = 0;
  [[nodiscard]] virtual auto session_id() const noexcept
      -> const domain::SessionId& = 0;
  [[nodiscard]] virtual auto lease_generation() const noexcept
      -> std::uint64_t = 0;
  // Revocation must only mark authority unavailable; never close or join here.
  virtual auto revoke() noexcept -> void = 0;
};
struct LocalFolderGrantResult {
  LocalFolderGrantToken token;
  domain::LocalRootIdentity root;
  std::shared_ptr<LocalSourceLease> lease;
};
class LocalSourceGrantFactory {
 public:
  virtual ~LocalSourceGrantFactory() = default;
  [[nodiscard]] virtual auto guarantees_pinned_read_only_sources()
      const noexcept -> bool {
    return false;
  }
  // Called on bounded background work. No tool or instruction authority.
  [[nodiscard]] virtual auto grant(const LocalFolderGrantRequest& request,
                                   std::stop_token stop = {})
      -> std::expected<LocalFolderGrantResult, domain::LocalSourceError> = 0;
};
[[nodiscard]] auto validate_local_folder_grant_request(
    const LocalFolderGrantRequest& request)
    -> std::expected<void, domain::LocalSourceError>;
[[nodiscard]] auto validate_local_folder_grant_result(
    const LocalFolderGrantRequest& request,
    const LocalFolderGrantResult& result)
    -> std::expected<void, domain::LocalSourceError>;

// Successful delivery transfers lease ownership to the caller. A future live
// lease registry must retire accepted leases off the UI thread; this port does
// not provide an unbounded cleanup queue or manage accepted lease lifetimes.
} // namespace aiforge::runtime
