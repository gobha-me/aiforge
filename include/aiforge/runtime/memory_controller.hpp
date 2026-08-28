#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <aiforge/config/config.hpp>
#include <aiforge/domain/context.hpp>
#include <aiforge/domain/memory_projection.hpp>
#include <aiforge/runtime/memory_tool.hpp>
#include <aiforge/storage/session_store.hpp>

namespace aiforge::runtime {

struct MemorySettings {
  domain::MemoryCaptureMode global_capture{domain::MemoryCaptureMode::off};
  domain::MemoryCaptureMode project_capture{domain::MemoryCaptureMode::review};
  std::uint64_t context_tokens{2048};
  auto operator==(const MemorySettings&) const -> bool = default;
};

enum class MemoryControllerErrorCode {
  invalid_configuration,
  unavailable,
  invalid_source,
  invalid_transition,
  stale_state,
  storage_failure,
  cancelled,
  internal_failure,
};

struct MemoryControllerError {
  MemoryControllerErrorCode code{MemoryControllerErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  auto operator==(const MemoryControllerError&) const -> bool = default;
};

struct MemoryRecordView {
  domain::ProjectedMemoryRecord projected;
  bool source_available{};
  auto operator==(const MemoryRecordView&) const -> bool = default;
};

struct MemoryProposalView {
  domain::ProjectedMemoryProposal projected;
  bool source_available{};
  auto operator==(const MemoryProposalView&) const -> bool = default;
};

struct MemoryState {
  domain::MemoryScope scope{domain::MemoryScope::global};
  std::optional<domain::RepositoryId> repository_id;
  std::vector<MemoryProposalView> proposals;
  std::vector<MemoryRecordView> records;
};

struct MemoryMutationTarget {
  domain::MemoryScope scope{domain::MemoryScope::global};
  std::optional<domain::RepositoryId> repository_id;
};

struct MemoryAcceptRequest {
  MemoryMutationTarget target;
  domain::MemoryProposalId proposal_id;
  domain::EventId expected_proposal_event_id;
  std::optional<std::string> edited_content;
  std::optional<domain::MemoryRecordId> replacement_record_id;
  std::optional<domain::EventId> expected_record_event_id;
};

struct MemoryRejectRequest {
  MemoryMutationTarget target;
  domain::MemoryProposalId proposal_id;
  domain::EventId expected_proposal_event_id;
  std::string reason;
};

struct MemoryExpireRequest {
  MemoryMutationTarget target;
  domain::MemoryRecordId record_id;
  domain::EventId expected_record_event_id;
  std::string reason;
};

struct MemoryContextRequest {
  std::optional<domain::RepositoryId> repository_id;
  std::uint64_t maximum_tokens{2048};
  std::uint64_t available_tokens{};
};

using MemoryIdentitySuffixSource = std::function<std::uint64_t()>;
using MemoryTimestampSource = std::function<domain::EventTimestamp()>;

class MemoryController;

[[nodiscard]] auto resolve_memory_settings(const config::ResolvedConfig& config)
    -> std::expected<MemorySettings, MemoryControllerError>;
[[nodiscard]] auto select_memory_context(MemoryController& controller,
                                         MemoryContextRequest request)
    -> std::expected<std::vector<domain::ContextContentInput>,
                     MemoryControllerError>;

class MemoryController final {
 public:
  MemoryController(storage::SessionStore& store,
                   MemoryIdentitySuffixSource identity_suffix_source,
                   MemoryTimestampSource timestamp_source,
                   std::stop_token stop_token = {},
                   domain::MemoryLimits limits = {});

  [[nodiscard]] auto capture_committed(
      const domain::SessionId& source_session_id,
      std::span<const domain::RunEvent> source_events,
      const MemorySettings& settings,
      std::optional<domain::RepositoryId> repository_id,
      std::string runtime_version)
      -> std::expected<std::size_t, MemoryControllerError>;
  [[nodiscard]] auto inspect(MemoryMutationTarget target)
      -> std::expected<MemoryState, MemoryControllerError>;
  [[nodiscard]] auto current_for_context(
      std::optional<domain::RepositoryId> repository_id)
      -> std::expected<std::vector<MemoryRecordView>, MemoryControllerError>;
  [[nodiscard]] auto accept(MemoryAcceptRequest request)
      -> std::expected<void, MemoryControllerError>;
  [[nodiscard]] auto reject(MemoryRejectRequest request)
      -> std::expected<void, MemoryControllerError>;
  [[nodiscard]] auto expire(MemoryExpireRequest request)
      -> std::expected<void, MemoryControllerError>;

 private:
  struct Journal;
  [[nodiscard]] auto open(MemoryMutationTarget target)
      -> std::expected<Journal, MemoryControllerError>;
  [[nodiscard]] auto append(Journal& journal, const domain::RunId& run_id,
                            std::optional<domain::InvocationId> invocation_id,
                            std::vector<domain::RunEventPayload> payloads)
      -> std::expected<void, MemoryControllerError>;
  [[nodiscard]] auto append_capture(Journal& journal,
                                    const domain::RunId& run_id,
                                    const domain::InvocationId& invocation_id,
                                    const domain::MemoryProposal& proposal,
                                    domain::MemoryPolicyAction action,
                                    std::string reason)
      -> std::expected<void, MemoryControllerError>;

  storage::SessionStore& m_store;
  MemoryIdentitySuffixSource m_identity_suffix_source;
  MemoryTimestampSource m_timestamp_source;
  std::stop_token m_stop_token;
  domain::MemoryLimits m_limits;
};

} // namespace aiforge::runtime
