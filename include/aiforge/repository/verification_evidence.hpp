#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/repository_evidence.hpp>

namespace aiforge::repository {

struct VerificationEvidenceLimits {
  std::size_t maximum_summary_bytes{16U * 1024U};
  std::size_t maximum_output_excerpts{16};
  std::size_t maximum_excerpt_bytes{64U * 1024U};
  std::uint64_t maximum_total_inline_bytes{256U * 1024U};
  std::size_t maximum_diagnostics{4096};
  std::size_t maximum_diagnostic_bytes{16U * 1024U};
  std::size_t maximum_artifacts{64};
  std::size_t maximum_metadata_bytes{4096};
  auto operator==(const VerificationEvidenceLimits&) const -> bool = default;
};

enum class VerificationEvidenceErrorCode {
  invalid_limits,
  invalid_evidence,
  invalid_source,
  invalid_provenance,
  invalid_output,
  invalid_diagnostic,
  duplicate_artifact,
  resource_exhausted,
  overflow,
  internal_failure,
};

struct VerificationEvidenceError {
  VerificationEvidenceErrorCode code{
      VerificationEvidenceErrorCode::internal_failure};
  std::string message;
  std::optional<domain::VerificationEvidenceId> evidence_id;
  auto operator==(const VerificationEvidenceError&) const -> bool = default;
};

enum class VerificationInvalidationTrigger {
  source_snapshot_changed,
  build_configuration_changed,
  artifact_unavailable,
};

struct VerificationEvidenceEnvironment {
  std::optional<domain::RepositorySnapshotIdentity> source_snapshot;
  std::optional<domain::ContentDigest> build_configuration;
  std::vector<domain::ArtifactId> available_artifacts;
  bool artifact_observation_complete{};
  auto operator==(const VerificationEvidenceEnvironment&) const -> bool = default;
};

struct VerificationEvidenceAssessment {
  domain::EvidenceFreshness freshness{domain::EvidenceFreshness::possibly_stale};
  std::vector<VerificationInvalidationTrigger> affected_triggers;
  auto operator==(const VerificationEvidenceAssessment&) const -> bool = default;
};

[[nodiscard]] auto validate_verification_evidence(
    const domain::VerificationEvidence& evidence,
    const VerificationEvidenceLimits& limits = {})
    -> std::expected<void, VerificationEvidenceError>;

[[nodiscard]] auto assess_verification_evidence(
    const domain::VerificationEvidence& evidence,
    const VerificationEvidenceEnvironment& environment,
    const VerificationEvidenceLimits& limits = {})
    -> std::expected<VerificationEvidenceAssessment,
                     VerificationEvidenceError>;

[[nodiscard]] auto make_verification_context_item(
    const domain::VerificationEvidence& evidence,
    domain::EvidenceId context_evidence_id,
    domain::EvidenceFreshness freshness,
    std::uint64_t estimated_tokens,
    const VerificationEvidenceLimits& limits = {})
    -> std::expected<domain::ContextParcelItem, VerificationEvidenceError>;

}  // namespace aiforge::repository
