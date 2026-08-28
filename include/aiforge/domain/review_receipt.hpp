#pragma once

#include <aiforge/domain/verification_evidence.hpp>

#include <optional>
#include <string>
#include <vector>

namespace aiforge::domain {

enum class ReviewEvidenceKind {
  verification,
  scenario,
};

struct ReviewActor {
  std::string actor_id;
  std::string display_name;
  auto operator==(const ReviewActor&) const -> bool = default;
};

struct ReviewParticipantProvenance {
  ReviewActor actor;
  std::optional<RunId> run_id;
  std::optional<std::string> backend_id;
  std::optional<std::string> backend_version;
  std::optional<ModelId> model_id;
  std::optional<std::string> model_version;
  auto operator==(const ReviewParticipantProvenance&) const -> bool = default;
};

struct ReviewCandidate {
  RepositorySnapshotIdentity snapshot;
  std::string revision;
  auto operator==(const ReviewCandidate&) const -> bool = default;
};

struct ReviewArtifactDigest {
  ArtifactId artifact_id;
  ContentDigest digest;
  auto operator==(const ReviewArtifactDigest&) const -> bool = default;
};

struct ReviewEvidenceBinding {
  ReviewRequirementId requirement_id;
  ReviewEvidenceKind kind{ReviewEvidenceKind::verification};
  std::string producer_name;
  std::string producer_version;
  std::optional<VerificationEvidenceId> verification_evidence_id;
  std::optional<std::string> scenario_id;
  std::optional<std::string> scenario_corpus_version;
  std::optional<std::string> scenario_application_revision;
  std::optional<ContentDigest> scenario_fake_script_digest;
  std::optional<ContentDigest> scenario_terminal_capabilities_digest;
  ContentDigest result_digest;
  std::vector<ReviewArtifactDigest> artifacts;
  auto operator==(const ReviewEvidenceBinding&) const -> bool = default;
};

struct ReviewReceiptDraft {
  ReviewReceiptId receipt_id;
  ReviewCandidate candidate;
  std::vector<ReviewEvidenceBinding> evidence;
  std::optional<ReviewParticipantProvenance> author;
  auto operator==(const ReviewReceiptDraft&) const -> bool = default;
};

enum class ReviewFindingSeverity {
  low,
  medium,
  high,
  critical,
};

struct ReviewFinding {
  ReviewFindingId finding_id;
  std::string summary;
  std::optional<VerificationEvidenceId> verification_evidence_id;
  std::vector<ArtifactId> artifacts;
  ReviewFindingSeverity severity{ReviewFindingSeverity::medium};
  std::optional<RepositorySourceIdentity> source;
  std::vector<VerificationEvidenceId> reproduction_evidence_ids;
  auto operator==(const ReviewFinding&) const -> bool = default;
};

enum class ReviewVerdict {
  approved,
  changes_requested,
  rejected,
};

struct ReviewChildResult {
  ReviewReceiptId receipt_id;
  ReviewCandidate candidate;
  ReviewParticipantProvenance reviewer;
  std::vector<ReviewFinding> findings;
  ReviewVerdict verdict{ReviewVerdict::rejected};
  auto operator==(const ReviewChildResult&) const -> bool = default;
};

struct ReviewOverride {
  ReviewOverrideId override_id;
  ReviewReceiptId receipt_id;
  ReviewCandidate candidate;
  ReviewActor actor;
  std::string reason;
  auto operator==(const ReviewOverride&) const -> bool = default;
};

} // namespace aiforge::domain
