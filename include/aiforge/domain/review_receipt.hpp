#pragma once

#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/verification_evidence.hpp>

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
  auto operator==(const ReviewReceiptDraft&) const -> bool = default;
};

struct ReviewFinding {
  ReviewFindingId finding_id;
  std::string summary;
  std::optional<VerificationEvidenceId> verification_evidence_id;
  std::vector<ArtifactId> artifacts;
  auto operator==(const ReviewFinding&) const -> bool = default;
};

enum class ReviewVerdict {
  approved,
  changes_requested,
  rejected,
};

struct ReviewOverride {
  ReviewOverrideId override_id;
  ReviewReceiptId receipt_id;
  ReviewCandidate candidate;
  ReviewActor actor;
  std::string reason;
  auto operator==(const ReviewOverride&) const -> bool = default;
};

}  // namespace aiforge::domain
