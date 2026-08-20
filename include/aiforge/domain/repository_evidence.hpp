#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <aiforge/domain/content.hpp>
#include <aiforge/domain/repository.hpp>

namespace aiforge::domain {

enum class TaskPhase {
  orientation,
  diagnosis,
  editing,
  verification,
  review,
  unknown,
};

enum class EvidenceFreshness {
  current,
  possibly_stale,
  stale,
  unavailable,
};

enum class EvidenceDerivation {
  observed,
  derived,
  inferred,
  unknown,
};

struct ExactSourceEvidence {
  RepositorySourceIdentity source;
  auto operator==(const ExactSourceEvidence&) const -> bool = default;
};

struct DiagnosticEvidence {
  ArtifactId artifact_id;
  std::optional<RepositorySourceIdentity> source;
  auto operator==(const DiagnosticEvidence&) const -> bool = default;
};

struct DiffEvidence {
  RepositorySnapshotIdentity base_snapshot;
  RepositorySnapshotIdentity target_snapshot;
  ArtifactId artifact_id;
  auto operator==(const DiffEvidence&) const -> bool = default;
};

struct ToolResultEvidence {
  InvocationId invocation_id;
  std::optional<ArtifactId> artifact_id;
  auto operator==(const ToolResultEvidence&) const -> bool = default;
};

struct DerivedRecordEvidence {
  std::string record_type;
  std::string record_id;
  std::optional<ArtifactId> artifact_id;
  auto operator==(const DerivedRecordEvidence&) const -> bool = default;
};

struct UnknownRepositoryEvidence {
  std::string type_name;
  std::optional<ArtifactId> artifact_id;
  auto operator==(const UnknownRepositoryEvidence&) const -> bool = default;
};

using RepositoryEvidenceReference =
    std::variant<ExactSourceEvidence, DiagnosticEvidence, DiffEvidence,
                 ToolResultEvidence, DerivedRecordEvidence,
                 UnknownRepositoryEvidence>;

struct EvidenceProvenance {
  EvidenceDerivation derivation{EvidenceDerivation::observed};
  std::string producer;
  std::string producer_version;
  std::chrono::sys_time<std::chrono::milliseconds> created_at;
  std::optional<RepositorySnapshotIdentity> source_snapshot;
  std::vector<EventId> source_event_ids;
  std::vector<EvidenceId> derivation_inputs;
  std::optional<InvocationId> producing_invocation_id;
  auto operator==(const EvidenceProvenance&) const -> bool = default;
};

struct ContextParcelItem {
  EvidenceId evidence_id;
  RepositoryEvidenceReference reference;
  EvidenceFreshness freshness{EvidenceFreshness::current};
  EvidenceProvenance provenance;
  std::vector<ContentBlock> content;
  std::uint64_t estimated_bytes{};
  std::uint64_t estimated_tokens{};
  auto operator==(const ContextParcelItem&) const -> bool = default;
};

struct ContextParcel {
  ContextParcelId parcel_id;
  // Runtime-owned, bounded metadata describing why this evidence was selected.
  std::string purpose;
  TaskPhase phase{TaskPhase::orientation};
  RepositorySnapshotIdentity target_snapshot;
  std::vector<ContextParcelItem> items;
  auto operator==(const ContextParcel&) const -> bool = default;
};

}  // namespace aiforge::domain
