#pragma once

#include <aiforge/domain/events.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace aiforge::repository {

struct ReviewReceiptLimits {
  std::size_t maximum_evidence{128};
  std::size_t maximum_artifacts_per_evidence{128};
  std::size_t maximum_findings{4096};
  std::size_t maximum_finding_artifacts{64};
  std::size_t maximum_text_bytes{16U * 1024U};
  std::size_t maximum_metadata_bytes{4096};
  auto operator==(const ReviewReceiptLimits&) const -> bool = default;
};

enum class ReviewReceiptErrorCode {
  invalid_limits,
  invalid_receipt,
  invalid_candidate,
  invalid_evidence,
  invalid_actor,
  invalid_finding,
  duplicate_identity,
  wrong_receipt,
  invalid_transition,
  unknown_finding,
  unknown_verdict,
  unknown_override,
  invalid_envelope,
  non_monotonic_sequence,
  duplicate_event,
  resource_exhausted,
  internal_failure,
};

struct ReviewReceiptError {
  ReviewReceiptErrorCode code{ReviewReceiptErrorCode::internal_failure};
  std::string message;
  std::optional<domain::ReviewReceiptId> receipt_id;
  auto operator==(const ReviewReceiptError&) const -> bool = default;
};

enum class ReviewReceiptState {
  not_started,
  draft,
  review_requested,
  findings_open,
  approved,
  changes_requested,
  rejected,
  revoked,
  conflicted,
};

struct ProjectedReviewFinding {
  domain::ReviewFinding finding;
  bool open{true};
  auto operator==(const ProjectedReviewFinding&) const -> bool = default;
};

struct ProjectedReviewVerdict {
  domain::EventId event_id;
  domain::ReviewVerdict verdict{domain::ReviewVerdict::rejected};
  domain::ReviewActor reviewer;
  std::optional<domain::ReviewParticipantProvenance> reviewer_provenance;
  bool active{true};
  auto operator==(const ProjectedReviewVerdict&) const -> bool = default;
};

struct ProjectedReviewOverride {
  domain::ReviewOverride value;
  bool active{true};
  auto operator==(const ProjectedReviewOverride&) const -> bool = default;
};

[[nodiscard]] auto validate_review_receipt_draft(
    const domain::ReviewReceiptDraft& draft,
    const ReviewReceiptLimits& limits = {})
    -> std::expected<void, ReviewReceiptError>;

[[nodiscard]] auto validate_review_candidate(
    const domain::ReviewCandidate& candidate,
    const ReviewReceiptLimits& limits = {})
    -> std::expected<void, ReviewReceiptError>;

[[nodiscard]] auto validate_review_actor(const domain::ReviewActor& actor,
                                         const ReviewReceiptLimits& limits = {})
    -> std::expected<void, ReviewReceiptError>;

[[nodiscard]] auto validate_review_participant_provenance(
    const domain::ReviewParticipantProvenance& participant,
    const ReviewReceiptLimits& limits = {})
    -> std::expected<void, ReviewReceiptError>;

[[nodiscard]] auto validate_review_finding(
    const domain::ReviewFinding& finding,
    const ReviewReceiptLimits& limits = {})
    -> std::expected<void, ReviewReceiptError>;

[[nodiscard]] auto validate_review_child_result(
    const domain::ReviewChildResult& result,
    const domain::ReviewReceiptDraft& draft,
    std::span<const domain::EvidenceId> returned_evidence,
    std::span<const domain::ArtifactId> returned_artifacts,
    const ReviewReceiptLimits& limits = {})
    -> std::expected<void, ReviewReceiptError>;

[[nodiscard]] auto validate_review_override(
    const domain::ReviewOverride& value, const ReviewReceiptLimits& limits = {})
    -> std::expected<void, ReviewReceiptError>;

[[nodiscard]] auto validate_review_evidence_binding(
    const domain::ReviewEvidenceBinding& binding,
    const ReviewReceiptLimits& limits = {})
    -> std::expected<void, ReviewReceiptError>;

class ReviewReceiptProjection final {
 public:
  [[nodiscard]] auto apply(const domain::RunEvent& event,
                           const ReviewReceiptLimits& limits = {})
      -> std::expected<void, ReviewReceiptError>;

  [[nodiscard]] static auto rebuild(std::span<const domain::RunEvent> events,
                                    const ReviewReceiptLimits& limits = {})
      -> std::expected<ReviewReceiptProjection, ReviewReceiptError>;

  [[nodiscard]] auto receipt_id() const noexcept
      -> const std::optional<domain::ReviewReceiptId>& {
    return m_receipt_id;
  }
  [[nodiscard]] auto draft() const noexcept
      -> const std::optional<domain::ReviewReceiptDraft>& {
    return m_draft;
  }
  [[nodiscard]] auto state() const noexcept -> ReviewReceiptState;
  [[nodiscard]] auto findings() const noexcept
      -> const std::vector<ProjectedReviewFinding>& {
    return m_findings;
  }
  [[nodiscard]] auto verdicts() const noexcept
      -> const std::vector<ProjectedReviewVerdict>& {
    return m_verdicts;
  }
  [[nodiscard]] auto overrides() const noexcept
      -> const std::vector<ProjectedReviewOverride>& {
    return m_overrides;
  }
  [[nodiscard]] auto last_sequence() const noexcept -> std::uint64_t {
    return m_last_sequence;
  }

 private:
  [[nodiscard]] auto apply_in_place(const domain::RunEvent& event,
                                    const ReviewReceiptLimits& limits)
      -> std::expected<void, ReviewReceiptError>;

  std::optional<domain::ReviewReceiptId> m_receipt_id;
  std::optional<domain::ReviewReceiptDraft> m_draft;
  bool m_review_requested{};
  bool m_approval_revoked{};
  std::vector<ProjectedReviewFinding> m_findings;
  std::vector<ProjectedReviewVerdict> m_verdicts;
  std::vector<ProjectedReviewOverride> m_overrides;
  std::set<domain::EventId> m_event_ids;
  std::uint64_t m_last_sequence{};
};

} // namespace aiforge::repository
