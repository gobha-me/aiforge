#pragma once

#include <filesystem>
#include <memory>

#include <aiforge/runtime/local_source_grant.hpp>

namespace aiforge::adapters {

class LocalFileSource;

// Explicit session read/browse grant. Does not register model tool authority.
// The absolute path remains adapter-owned. Linux physical identity is scoped
// to the current boot and mount namespace; regrant after either changes cannot
// silently recover an older physical binding.
// Synchronous filesystem IO: the application must schedule this factory on
// bounded owned background work, just like list/preview/revalidate.
[[nodiscard]] auto grant_local_folder(domain::SessionId session_id,
                                      std::filesystem::path absolute_root,
                                      std::uint64_t lease_generation,
                                      domain::LocalSourceLimits limits = {},
                                      std::stop_token stop = {})
    -> std::expected<std::shared_ptr<LocalFileSource>,
                     domain::LocalSourceError>;

// All operation state and descriptors are owned. Callers retain shared lease
// ownership for outstanding operations. revoke() is immediate and never joins
// workers; cancellation is checked between calls, not inside stalled OS calls.
class LocalFileSource final : public runtime::LocalSourceLease {
 public:
  ~LocalFileSource() override;
  LocalFileSource(const LocalFileSource&) = delete;
  auto operator=(const LocalFileSource&) -> LocalFileSource& = delete;

  [[nodiscard]] auto root_identity() const noexcept
      -> const domain::LocalRootIdentity& override;
  [[nodiscard]] auto session_id() const noexcept
      -> const domain::SessionId& override;
  [[nodiscard]] auto lease_generation() const noexcept
      -> std::uint64_t override;
  auto revoke() noexcept -> void override;
  [[nodiscard]] auto guarantees_pinned_read_only_sources() const noexcept
      -> bool override;
  [[nodiscard]] auto list(runtime::LocalListRequest request,
                          std::stop_token stop = {})
      -> std::expected<runtime::LocalListResult,
                       domain::LocalSourceError> override;
  [[nodiscard]] auto preview(runtime::LocalPreviewRequest request,
                             std::stop_token stop = {})
      -> std::expected<runtime::LocalPreviewResult,
                       domain::LocalSourceError> override;
  [[nodiscard]] auto revalidate(runtime::LocalRevalidateRequest request,
                                std::stop_token stop = {})
      -> std::expected<runtime::LocalReadResult,
                       domain::LocalSourceError> override;

 private:
  struct Impl;
  explicit LocalFileSource(std::unique_ptr<Impl> implementation);
  std::unique_ptr<Impl> m_impl;
  friend auto grant_local_folder(domain::SessionId, std::filesystem::path,
                                 std::uint64_t, domain::LocalSourceLimits,
                                 std::stop_token)
      -> std::expected<std::shared_ptr<LocalFileSource>,
                       domain::LocalSourceError>;
};

class LocalFileGrantFactory final : public runtime::LocalSourceGrantFactory {
 public:
  [[nodiscard]] auto guarantees_pinned_read_only_sources() const noexcept
      -> bool override {
    return true;
  }
  [[nodiscard]] auto grant(const runtime::LocalFolderGrantRequest& request,
                           std::stop_token stop = {})
      -> std::expected<runtime::LocalFolderGrantResult,
                       domain::LocalSourceError> override;
};

} // namespace aiforge::adapters
