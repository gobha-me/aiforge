#pragma once

#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <aiforge/repository/review_receipt.hpp>

namespace aiforge::runtime {

struct ReviewPolicyRequirement {
  domain::ReviewRequirementId requirement_id;
  domain::ReviewEvidenceKind kind{domain::ReviewEvidenceKind::verification};
  std::string producer_name;
  std::string producer_version;
  std::optional<std::string> scenario_id;
  std::optional<std::string> scenario_corpus_version;
  auto operator==(const ReviewPolicyRequirement&) const -> bool = default;
};

struct ReviewAuthorizationPolicy {
  std::vector<ReviewPolicyRequirement> required_evidence;
  bool allow_human_override{};
  std::vector<std::string> trusted_override_actor_ids;
  bool require_hosted_check{};
  auto operator==(const ReviewAuthorizationPolicy&) const -> bool = default;
};

struct ReviewGateEnvironment {
  domain::ReviewCandidate current_candidate;
  std::vector<domain::ReviewEvidenceBinding> current_evidence;
  auto operator==(const ReviewGateEnvironment&) const -> bool = default;
};

enum class ReviewInvalidationTrigger {
  candidate_changed,
  requirement_missing,
  evidence_changed,
  verifier_version_changed,
  scenario_version_changed,
  artifact_missing_or_changed,
  findings_open,
  verdict_missing,
  verdict_conflicted,
  verdict_not_approved,
  approval_revoked,
  override_untrusted,
};

enum class ReviewAuthorizationSource {
  receipt,
  human_override,
};

enum class ReviewGateState {
  denied,
  invalidated,
  authorized,
  overridden,
};

enum class HostedReviewCheckState {
  success,
  failure,
};

struct HostedReviewCheckUpdate {
  domain::ReviewReceiptId receipt_id;
  domain::ReviewCandidate candidate;
  HostedReviewCheckState state{HostedReviewCheckState::failure};
  domain::ContentDigest decision_digest;
  std::string summary;
  auto operator==(const HostedReviewCheckUpdate&) const -> bool = default;
};

struct HostedReviewCheckConfirmation {
  domain::ReviewCandidate candidate;
  HostedReviewCheckState state{HostedReviewCheckState::failure};
  domain::ContentDigest decision_digest;
  auto operator==(const HostedReviewCheckConfirmation&) const -> bool = default;
};

enum class HostedReviewCheckErrorCode {
  unavailable,
  rejected,
  mismatched_confirmation,
  internal_failure,
};

struct HostedReviewCheckError {
  HostedReviewCheckErrorCode code{HostedReviewCheckErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  auto operator==(const HostedReviewCheckError&) const -> bool = default;
};

class HostedReviewCheckPort {
 public:
  virtual ~HostedReviewCheckPort() = default;

  [[nodiscard]] virtual auto publish(const HostedReviewCheckUpdate& update)
      -> std::expected<HostedReviewCheckConfirmation,
                       HostedReviewCheckError> = 0;
};

class MergeAuthorization final {
 public:
  [[nodiscard]] auto receipt_id() const noexcept
      -> const domain::ReviewReceiptId& {
    return m_receipt_id;
  }
  [[nodiscard]] auto candidate() const noexcept
      -> const domain::ReviewCandidate& {
    return m_candidate;
  }
  [[nodiscard]] auto source() const noexcept -> ReviewAuthorizationSource {
    return m_source;
  }
  [[nodiscard]] auto decision_digest() const noexcept
      -> const domain::ContentDigest& {
    return m_decision_digest;
  }

  auto operator==(const MergeAuthorization&) const -> bool = default;

 private:
  friend class ReviewMergeGate;
  MergeAuthorization(domain::ReviewReceiptId receipt_id,
                     domain::ReviewCandidate candidate,
                     ReviewAuthorizationSource source,
                     domain::ContentDigest decision_digest)
      : m_receipt_id(std::move(receipt_id)), m_candidate(std::move(candidate)),
        m_source(source), m_decision_digest(std::move(decision_digest)) {}

  domain::ReviewReceiptId m_receipt_id;
  domain::ReviewCandidate m_candidate;
  ReviewAuthorizationSource m_source{ReviewAuthorizationSource::receipt};
  domain::ContentDigest m_decision_digest;
};

struct ReviewGateDecision {
  ReviewGateState state{ReviewGateState::denied};
  std::optional<MergeAuthorization> authorization;
  std::vector<ReviewInvalidationTrigger> triggers;
  std::optional<HostedReviewCheckConfirmation> hosted_check;
  std::string explanation;
};

enum class ReviewGateErrorCode {
  invalid_policy,
  invalid_environment,
  invalid_projection,
  hosted_check_failure,
  internal_failure,
};

struct ReviewGateError {
  ReviewGateErrorCode code{ReviewGateErrorCode::internal_failure};
  std::string message;
  bool retryable{};
  auto operator==(const ReviewGateError&) const -> bool = default;
};

class ReviewMergeGate final {
 public:
  [[nodiscard]] auto evaluate(
      const repository::ReviewReceiptProjection& receipt,
      const ReviewAuthorizationPolicy& policy,
      const ReviewGateEnvironment& environment,
      HostedReviewCheckPort* hosted_check = nullptr) const
      -> std::expected<ReviewGateDecision, ReviewGateError>;
};

} // namespace aiforge::runtime
