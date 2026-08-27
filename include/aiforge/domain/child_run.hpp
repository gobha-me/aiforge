#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/content.hpp>
#include <aiforge/domain/plan.hpp>
#include <aiforge/domain/repository_evidence.hpp>

namespace aiforge::domain {

struct ChildRunBudget {
  std::uint32_t maximum_inferences{1};
  std::uint32_t maximum_tool_invocations{1};
  std::uint64_t maximum_input_tokens{1};
  std::uint64_t maximum_output_tokens{1};
  std::chrono::milliseconds timeout{std::chrono::seconds{30}};
  auto operator==(const ChildRunBudget&) const -> bool = default;
};

struct ChildRunContextBinding {
  ContextParcelId parcel_id;
  RepositorySnapshotIdentity target_snapshot;
  std::vector<EvidenceId> evidence_ids;
  std::uint64_t represented_bytes{};
  std::uint64_t estimated_tokens{};
  auto operator==(const ChildRunContextBinding&) const -> bool = default;
};

struct ChildRunDescriptor {
  RunId parent_run_id;
  PlanId plan_id;
  PlanRevisionId revision_id;
  PlanTaskId task_id;
  ChildRunContextBinding context;
  ChildRunBudget budget;
  std::vector<Effect> effects;
  std::vector<CapabilityScope> capability_scopes;
  std::uint32_t attempt{1};
  auto operator==(const ChildRunDescriptor&) const -> bool = default;
};

enum class SessionTaskOutcome {
  completed,
  failed,
  cancelled,
  timed_out,
  budget_exhausted,
  unavailable,
};

struct ChildRunConsumption {
  std::uint32_t inference_count{};
  std::uint32_t tool_invocation_count{};
  Usage usage;
  auto operator==(const ChildRunConsumption&) const -> bool = default;
};

struct SessionTaskResult {
  PlanId plan_id;
  PlanRevisionId revision_id;
  PlanTaskId task_id;
  RunId child_run_id;
  SessionTaskOutcome outcome{SessionTaskOutcome::failed};
  ChildRunConsumption consumption;
  std::vector<EvidenceId> evidence_ids;
  std::vector<ArtifactId> artifact_ids;
  std::optional<DomainError> error;
  auto operator==(const SessionTaskResult&) const -> bool = default;
};

enum class ChildRunContractErrorCode {
  invalid_descriptor,
  invalid_context,
  invalid_budget,
  invalid_capabilities,
  invalid_result,
  resource_exhausted,
};

struct ChildRunContractError {
  ChildRunContractErrorCode code{ChildRunContractErrorCode::invalid_descriptor};
  std::string message;
  auto operator==(const ChildRunContractError&) const -> bool = default;
};

[[nodiscard]] auto
validate_child_run_descriptor(const ChildRunDescriptor& descriptor)
    -> std::expected<void, ChildRunContractError>;

[[nodiscard]] auto validate_session_task_result(const SessionTaskResult& result,
                                                const ChildRunBudget& budget)
    -> std::expected<void, ChildRunContractError>;

}  // namespace aiforge::domain
