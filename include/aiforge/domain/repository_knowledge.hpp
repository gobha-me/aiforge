#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <aiforge/domain/repository.hpp>

namespace aiforge::domain {

enum class KnowledgeEntityKind {
  repository,
  directory,
  file,
  source_range,
  symbol,
  type,
  function,
  method,
  module,
  package,
  build_target,
  test,
  diagnostic,
  configuration,
  artifact,
  unknown,
};

struct KnowledgeEntity {
  KnowledgeEntityId entity_id;
  KnowledgeEntityKind kind{KnowledgeEntityKind::unknown};
  std::string display_name;
  std::optional<RepositorySourceIdentity> source;
  auto operator==(const KnowledgeEntity&) const -> bool = default;
};

struct SymbolKnowledge {
  KnowledgeEntity symbol;
  std::string language;
  std::optional<std::string> qualified_name;
  std::optional<std::string> signature;
  std::vector<RepositorySourceIdentity> declarations;
  std::optional<RepositorySourceIdentity> definition;
  auto operator==(const SymbolKnowledge&) const -> bool = default;
};

enum class KnowledgeRelationshipKind {
  contains,
  defines,
  references,
  calls,
  imports,
  includes,
  inherits,
  implements,
  builds,
  depends_on,
  generates,
  generated_from,
  tests,
  diagnoses,
  changed_by,
  unknown,
};

struct RelationshipKnowledge {
  KnowledgeEntity source;
  KnowledgeRelationshipKind kind{KnowledgeRelationshipKind::unknown};
  KnowledgeEntity target;
  std::optional<RepositorySourceIdentity> evidence_source;
  auto operator==(const RelationshipKnowledge&) const -> bool = default;
};

enum class KnowledgeDiagnosticSeverity {
  note,
  warning,
  error,
  fatal,
  unknown,
};

struct DiagnosticKnowledge {
  KnowledgeEntity subject;
  KnowledgeDiagnosticSeverity severity{KnowledgeDiagnosticSeverity::unknown};
  std::string code;
  std::string message;
  std::optional<RepositorySourceIdentity> source;
  auto operator==(const DiagnosticKnowledge&) const -> bool = default;
};

struct SemanticSummaryKnowledge {
  KnowledgeEntity subject;
  std::string purpose;
  std::string summary;
  std::optional<ArtifactId> artifact_id;
  auto operator==(const SemanticSummaryKnowledge&) const -> bool = default;
};

struct UnknownRepositoryKnowledge {
  KnowledgeEntity subject;
  std::string type_name;
  std::optional<ArtifactId> artifact_id;
  auto operator==(const UnknownRepositoryKnowledge&) const -> bool = default;
};

using RepositoryKnowledgePayload =
    std::variant<SymbolKnowledge, RelationshipKnowledge, DiagnosticKnowledge,
                 SemanticSummaryKnowledge, UnknownRepositoryKnowledge>;

enum class KnowledgeDerivation {
  observed,
  derived,
  inferred,
  unknown,
};

enum class KnowledgeConfidence {
  certain,
  high,
  medium,
  low,
  unknown,
};

enum class KnowledgeFreshness {
  current,
  possibly_stale,
  stale,
  unavailable,
};

struct KnowledgeProducer {
  std::string name;
  std::string version;
  auto operator==(const KnowledgeProducer&) const -> bool = default;
};

struct KnowledgeRecordReference {
  KnowledgeRecordId record_id;
  std::uint64_t revision{};
  auto operator==(const KnowledgeRecordReference&) const -> bool = default;
};

enum class KnowledgeInvalidationTrigger {
  source_snapshot_changed,
  source_digest_changed,
  dependency_changed,
  producer_version_changed,
  build_configuration_changed,
  unknown,
};

struct KnowledgeInvalidationRule {
  std::vector<KnowledgeInvalidationTrigger> triggers;
  auto operator==(const KnowledgeInvalidationRule&) const -> bool = default;
};

struct RepositoryKnowledgeProvenance {
  RepositorySnapshotIdentity source_snapshot;
  std::vector<RepositorySourceIdentity> sources;
  KnowledgeProducer producer;
  std::chrono::sys_time<std::chrono::milliseconds> created_at;
  std::vector<KnowledgeRecordReference> derivation_inputs;
  std::optional<ContentDigest> build_configuration;
  KnowledgeDerivation derivation{KnowledgeDerivation::unknown};
  KnowledgeConfidence confidence{KnowledgeConfidence::unknown};
  auto operator==(const RepositoryKnowledgeProvenance&) const -> bool = default;
};

struct RepositoryKnowledgeRecord {
  KnowledgeRecordId record_id;
  std::uint64_t revision{};
  RepositoryKnowledgePayload payload;
  RepositoryKnowledgeProvenance provenance;
  KnowledgeInvalidationRule invalidation;
  KnowledgeFreshness freshness{KnowledgeFreshness::current};
  auto operator==(const RepositoryKnowledgeRecord&) const -> bool = default;
};

struct RepositoryKnowledgeGraph {
  RepositoryId repository_id;
  std::uint64_t generation{};
  std::vector<RepositoryKnowledgeRecord> records;
  auto operator==(const RepositoryKnowledgeGraph&) const -> bool = default;
};

}  // namespace aiforge::domain
