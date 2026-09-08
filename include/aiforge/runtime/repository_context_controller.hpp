#pragma once

#include <chrono>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <aiforge/domain/repository_context.hpp>
#include <aiforge/repository/exact_source_edit.hpp>
#include <aiforge/repository/project_instruction_source.hpp>
#include <aiforge/runtime/context_selection.hpp>

namespace aiforge::runtime {

// Adapter port: all methods must share one pinned physical root lease, perform
// read-only observation/discovery, and reject untracked or nonregular evidence.
// A stable new snapshot is allowed; replacing the pinned root is not.
class RepositoryContextSource {
 public:
  virtual ~RepositoryContextSource() = default;
  [[nodiscard]] virtual auto identity() const noexcept -> std::string_view = 0;
  [[nodiscard]] virtual auto guarantees_pinned_read_only_sources()
      const noexcept -> bool {
    return false;
  }
  [[nodiscard]] virtual auto observe(
      repository::RepositorySnapshotLimits limits, std::stop_token stop = {})
      -> std::expected<domain::RepositorySnapshot,
                       repository::RepositorySnapshotError> = 0;
  [[nodiscard]] virtual auto discover(
      repository::ProjectInstructionRequest request, std::stop_token stop = {})
      -> std::expected<domain::ProjectInstructionDiscovery,
                       repository::ProjectInstructionError> = 0;
  [[nodiscard]] virtual auto read(repository::ExactSourceReadRequest request,
                                  std::stop_token stop = {})
      -> std::expected<repository::ExactSourceReadResult,
                       repository::ExactSourceEditError> = 0;
};

struct RepositoryContextLimits {
  std::size_t maximum_evidence_files{64};
  std::uint64_t maximum_evidence_file_bytes{std::uint64_t{256} * 1024};
  std::uint64_t maximum_evidence_total_bytes{std::uint64_t{2} * 1024 * 1024};
  std::chrono::milliseconds timeout{std::chrono::seconds{30}};
  repository::RepositorySnapshotLimits snapshot;
  repository::ProjectInstructionLimits instructions;
  auto operator==(const RepositoryContextLimits&) const -> bool = default;
};

struct RepositoryContextRequest {
  // Empty or "." denotes root; other paths must already be normalized.
  std::string target_subtree;
  std::uint64_t selection_revision{1};
  std::vector<std::string> evidence_paths;
  auto operator==(const RepositoryContextRequest&) const -> bool = default;
};

struct PreparedRepositoryContext {
  domain::RepositorySnapshot snapshot;
  domain::ProjectInstructionDiscovery discovery;
  std::vector<domain::InstructionInput> instructions;
  ContextParcelSelection evidence;
  // Unsealed until final selection has supplied capacity and decisions.
  domain::RepositoryContextAdmission admission;
  auto operator==(const PreparedRepositoryContext&) const -> bool = default;
};

// Bounded value preflight only; performs no source observation.
[[nodiscard]] auto validate_repository_context_request(
    const RepositoryContextRequest& request,
    const RepositoryContextLimits& limits = {})
    -> std::expected<void, domain::RepositoryContextError>;

class RepositoryContextController final {
 public:
  // The supplied source must transitively own every adapter/lease it uses.
  // Production's owning pinned-source factory provides this complete graph.
  [[nodiscard]] static auto create_owned(
      std::shared_ptr<RepositoryContextSource> source,
      domain::RepositoryRootIdentity root, RepositoryContextLimits limits = {})
      -> std::expected<std::shared_ptr<RepositoryContextController>,
                       domain::RepositoryContextError>;
  [[nodiscard]] auto owns_source() const noexcept -> bool {
    return m_owned_source != nullptr;
  }
  RepositoryContextController(RepositoryContextSource& source,
                              domain::RepositoryRootIdentity root,
                              RepositoryContextLimits limits = {});
  [[nodiscard]] auto prepare(RepositoryContextRequest request,
                             std::stop_token stop = {}) const
      -> std::expected<PreparedRepositoryContext,
                       domain::RepositoryContextError>;
  // Reconstruct the original exact inputs, allowing only unrelated snapshot
  // drift. Restores its capacity/decisions and seals the successor admission.
  [[nodiscard]] auto revalidate(
      const domain::RepositoryContextAdmission& original,
      std::stop_token stop = {}) const
      -> std::expected<PreparedRepositoryContext,
                       domain::RepositoryContextError>;

 private:
  std::shared_ptr<RepositoryContextSource> m_owned_source;
  RepositoryContextSource& m_source;
  domain::RepositoryRootIdentity m_root;
  std::string m_binding;
  RepositoryContextLimits m_limits;
};

[[nodiscard]] auto finalize_repository_context_admission(
    const domain::RepositoryContextAdmission& prepared,
    const ContextSelectionResult& selection)
    -> std::expected<domain::RepositoryContextAdmission,
                     domain::RepositoryContextError>;

[[nodiscard]] auto finalize_repository_context_admission(
    const PreparedRepositoryContext& prepared,
    const ContextSelectionResult& selection)
    -> std::expected<domain::RepositoryContextAdmission,
                     domain::RepositoryContextError>;

} // namespace aiforge::runtime
