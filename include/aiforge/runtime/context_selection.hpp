#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/context.hpp>
#include <aiforge/domain/repository_evidence.hpp>
#include <aiforge/repository/context_parcel.hpp>

namespace aiforge::runtime {

enum class ContextBudgetClass {
  conversation,
  summary,
  tool_result,
  repository_evidence,
  attachment,
  unknown,
};

enum class ContextRepresentation {
  direct,
  exact,
  derived,
  excerpt,
  summary,
  artifact_reference,
  unknown,
};

struct ContextSelectionCandidate {
  domain::ContextContentInput content;
  ContextBudgetClass budget_class{ContextBudgetClass::conversation};
  ContextRepresentation representation{ContextRepresentation::direct};
  domain::EvidenceFreshness freshness{domain::EvidenceFreshness::current};
  bool required{};
  // Lower ranks are more relevant. Stable order and entry ID break ties.
  std::uint64_t relevance_rank{};
  std::optional<std::string> alternative_group;
  auto operator==(const ContextSelectionCandidate&) const -> bool = default;
};

struct RepositoryEvidenceSelection {
  domain::EvidenceId evidence_id;
  domain::ContextEntryId entry_id;
  domain::MessageId message_id;
  domain::ContextSourceId source_id;
  ContextBudgetClass budget_class{ContextBudgetClass::repository_evidence};
  ContextRepresentation representation{ContextRepresentation::exact};
  bool required{};
  std::uint64_t relevance_rank{};
  std::uint64_t order{};
  std::optional<std::string> alternative_group;
  auto operator==(const RepositoryEvidenceSelection&) const -> bool = default;
};

struct ContextParcelSelection {
  domain::ContextParcel parcel;
  // Exactly one binding is required for every parcel item.
  std::vector<RepositoryEvidenceSelection> items;
  auto operator==(const ContextParcelSelection&) const -> bool = default;
};

struct ContextClassBudgets {
  // Set values are hard ceilings for admitted candidates in that class.
  // Required candidates that exceed a ceiling fail the selection.
  std::optional<std::uint64_t> conversation_tokens;
  std::optional<std::uint64_t> summary_tokens;
  std::optional<std::uint64_t> tool_result_tokens;
  std::optional<std::uint64_t> repository_evidence_tokens;
  std::optional<std::uint64_t> attachment_tokens;
  auto operator==(const ContextClassBudgets&) const -> bool = default;
};

struct ContextSelectionLimits {
  std::size_t maximum_candidates{4096};
  std::size_t maximum_parcels{1024};
  std::size_t maximum_alternative_group_bytes{128};
  auto operator==(const ContextSelectionLimits&) const -> bool = default;
};

struct ContextSelectionRequest {
  domain::TaskPhase phase{domain::TaskPhase::orientation};
  domain::ContextCapacity capacity;
  std::vector<domain::InstructionInput> instructions;
  // Candidate content remains disposable working-set input. Callers mark only
  // content that must enter this inference as required.
  std::vector<ContextSelectionCandidate> candidates;
  std::vector<ContextParcelSelection> parcels;
  ContextClassBudgets budgets;
  repository::ContextParcelLimits parcel_limits;
  ContextSelectionLimits selection_limits;
  auto operator==(const ContextSelectionRequest&) const -> bool = default;
};

enum class ContextSelectionDecision {
  admitted,
  omitted_budget,
  omitted_class_budget,
  omitted_stale,
  omitted_unavailable,
  omitted_unsupported,
  omitted_alternative,
};

struct ContextSelectionDecisionRecord {
  domain::ContextEntryId entry_id;
  std::optional<domain::EvidenceId> evidence_id;
  ContextSelectionDecision decision{ContextSelectionDecision::admitted};
  std::uint64_t estimated_tokens{};
  auto operator==(const ContextSelectionDecisionRecord&) const -> bool = default;
};

struct ContextClassUsage {
  std::uint64_t conversation_tokens{};
  std::uint64_t summary_tokens{};
  std::uint64_t tool_result_tokens{};
  std::uint64_t repository_evidence_tokens{};
  std::uint64_t attachment_tokens{};
  auto operator==(const ContextClassUsage&) const -> bool = default;
};

struct ContextSelectionResult {
  domain::ConstructedContext context;
  std::vector<ContextSelectionDecisionRecord> decisions;
  ContextClassUsage usage;
  auto operator==(const ContextSelectionResult&) const -> bool = default;
};

enum class ContextSelectionErrorCode {
  invalid_phase,
  invalid_candidate,
  invalid_parcel,
  duplicate_candidate,
  conflicting_candidate,
  required_candidate_unavailable,
  resource_exhausted,
  token_overflow,
  mandatory_capacity_exceeded,
  context_build_failed,
  internal_failure,
};

struct ContextSelectionError {
  ContextSelectionErrorCode code{ContextSelectionErrorCode::invalid_candidate};
  std::string message;
  std::optional<domain::ContextEntryId> entry_id;
  std::optional<domain::EvidenceId> evidence_id;
  auto operator==(const ContextSelectionError&) const -> bool = default;
};

}  // namespace aiforge::runtime
