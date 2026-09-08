#pragma once

#include <aiforge/domain/local_context.hpp>
#include <aiforge/runtime/context_selection.hpp>
#include <aiforge/runtime/local_source.hpp>
#include <memory>

namespace aiforge::runtime {

struct LocalContextGrant {
  domain::SessionId session_id;
  domain::LocalRootIdentity root;
  std::uint64_t lease_generation{};
  std::shared_ptr<LocalSourceReader> reader;
};
class LocalContextGrantResolver {
 public:
  virtual ~LocalContextGrantResolver() = default;
  // Read-only lookup of CURRENT explicit grants. Never opens or grants a root.
  // Implementation owns any dependencies; stop is cooperative, not a promise
  // to interrupt a blocked call. Revoked/foreign-session grants return absent.
  [[nodiscard]] virtual auto resolve(const domain::SessionId& session,
                                     const domain::LocalRootIdentity& root,
                                     std::stop_token stop = {})
      -> std::expected<std::optional<LocalContextGrant>,
                       domain::LocalSourceError> = 0;
};
struct LocalContextRequest {
  domain::SessionId session_id;
  std::uint64_t selection_revision{};
  std::vector<domain::LocalSourceIdentity> sources;
  std::uint64_t first_order{1};
};
struct PreparedLocalContext {
  std::vector<ContextSelectionCandidate> candidates;
  // New preparation is unsealed until final combined selection. Recovery
  // retains the original sealed admission; only admitted candidates enter its
  // reconstructed request, without running selection again.
  domain::LocalContextAdmission admission;
};
class LocalContextController final {
 public:
  explicit LocalContextController(
      std::shared_ptr<LocalContextGrantResolver> grants,
      domain::LocalSourceLimits limits = {});
  [[nodiscard]] auto prepare(LocalContextRequest request,
                             std::stop_token stop = {}) const
      -> std::expected<PreparedLocalContext, domain::LocalContextError>;
  [[nodiscard]] auto revalidate(const domain::LocalContextAdmission& original,
                                std::stop_token stop = {}) const
      -> std::expected<PreparedLocalContext, domain::LocalContextError>;

 private:
  std::shared_ptr<LocalContextGrantResolver> m_grants;
  domain::LocalSourceLimits m_limits;
};

// Does not run a selector or read files. Use the result of the ONE combined
// optional selection with repository evidence after session preparation.
[[nodiscard]] auto finalize_local_context_admission(
    const PreparedLocalContext& prepared,
    const ContextSelectionResult& selection)
    -> std::expected<domain::LocalContextAdmission, domain::LocalContextError>;

} // namespace aiforge::runtime
