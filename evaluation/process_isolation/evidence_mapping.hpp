#pragma once

#include "evidence.hpp"
#include "evidence_v2.hpp"
#include "evidence_v3.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace aiforge::evaluation::process_isolation::mapping {

enum class EvidenceLevel { low, medium, high };

enum class AssessmentReason {
  none,
  missing_evidence,
  malformed_evidence,
  stale_evidence,
  conflicting_evidence,
  unproven_conjunct,
  unavailable_conjunct,
  indeterminate_evidence,
};

struct LevelAssessment {
  EvidenceLevel level{EvidenceLevel::low};
  bool complete{};
  AssessmentReason reason{AssessmentReason::missing_evidence};
  std::string_view conjunct;
  std::string_view evidence_reason;
  auto operator==(const LevelAssessment&) const -> bool = default;
};

struct EvidenceAssessment {
  std::array<LevelAssessment, 3> levels;
  auto operator==(const EvidenceAssessment&) const -> bool = default;
};

struct ExpectedEvidenceIdentity {
  std::string_view source_sha;
  std::string_view kernel;
  std::string_view architecture;
};

// Reviews retained engineering evidence for the ADR 0018 conjunctions. The
// expected identity must be supplied independently from the reports. This
// result is non-authoritative and must never be reused as launch availability.
[[nodiscard]] auto assess_linux_evidence(
    ExpectedEvidenceIdentity expected_identity,
    std::optional<std::string_view> schema_v1_document,
    std::optional<std::string_view> schema_v2_document,
    std::optional<std::string_view> schema_v3_document) -> EvidenceAssessment;

[[nodiscard]] auto evidence_level_name(EvidenceLevel value) -> std::string_view;
[[nodiscard]] auto assessment_reason_name(AssessmentReason value)
    -> std::string_view;
[[nodiscard]] auto required_v3_probe_ids(EvidenceLevel value)
    -> std::span<const v3::ProbeId>;

} // namespace aiforge::evaluation::process_isolation::mapping
