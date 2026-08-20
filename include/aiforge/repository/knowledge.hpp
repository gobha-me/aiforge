#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/repository_knowledge.hpp>

namespace aiforge::repository {

struct RepositoryKnowledgeLimits {
  std::size_t maximum_records{100000};
  std::size_t maximum_entities{200000};
  std::size_t maximum_sources_per_record{4096};
  std::size_t maximum_dependencies_per_record{4096};
  std::size_t maximum_declarations_per_symbol{4096};
  std::size_t maximum_metadata_bytes{4096};
  std::size_t maximum_summary_bytes{1024U * 1024U};
  std::uint64_t maximum_total_inline_bytes{256U * 1024U * 1024U};
  auto operator==(const RepositoryKnowledgeLimits&) const -> bool = default;
};

enum class RepositoryKnowledgeErrorCode {
  invalid_limits,
  invalid_graph,
  invalid_record,
  invalid_entity,
  invalid_source,
  invalid_provenance,
  invalid_invalidation_rule,
  invalid_freshness,
  duplicate_record,
  conflicting_entity,
  missing_dependency,
  cyclic_dependency,
  resource_exhausted,
  overflow,
  generation_conflict,
  revision_conflict,
  internal_failure,
};

struct RepositoryKnowledgeError {
  RepositoryKnowledgeErrorCode code{RepositoryKnowledgeErrorCode::internal_failure};
  std::string message;
  std::optional<domain::KnowledgeRecordId> record_id;
  auto operator==(const RepositoryKnowledgeError&) const -> bool = default;
};

struct RepositoryKnowledgeEstimate {
  std::size_t record_count{};
  std::size_t entity_count{};
  std::size_t relationship_count{};
  std::uint64_t inline_bytes{};
  auto operator==(const RepositoryKnowledgeEstimate&) const -> bool = default;
};

enum class KnowledgeInputAvailability {
  available,
  unavailable,
  unknown,
};

struct KnowledgeSourceObservation {
  std::string relative_path;
  std::optional<domain::ContentDigest> content_digest;
  KnowledgeInputAvailability availability{KnowledgeInputAvailability::unknown};
  auto operator==(const KnowledgeSourceObservation&) const -> bool = default;
};

struct KnowledgeDependencyObservation {
  domain::KnowledgeRecordId record_id;
  std::optional<std::uint64_t> revision;
  KnowledgeInputAvailability availability{KnowledgeInputAvailability::unknown};
  auto operator==(const KnowledgeDependencyObservation&) const -> bool = default;
};

struct RepositoryKnowledgeEnvironment {
  std::optional<domain::RepositorySnapshotIdentity> source_snapshot;
  std::vector<KnowledgeSourceObservation> sources;
  bool source_observation_complete{};
  std::vector<KnowledgeDependencyObservation> dependencies;
  bool dependency_observation_complete{};
  std::optional<domain::KnowledgeProducer> producer;
  std::optional<domain::ContentDigest> build_configuration;
  auto operator==(const RepositoryKnowledgeEnvironment&) const -> bool = default;
};

struct RepositoryKnowledgeAssessment {
  domain::KnowledgeRecordId record_id;
  domain::KnowledgeFreshness freshness{domain::KnowledgeFreshness::possibly_stale};
  std::vector<domain::KnowledgeInvalidationTrigger> affected_triggers;
  auto operator==(const RepositoryKnowledgeAssessment&) const -> bool = default;
};

struct RepositoryKnowledgeReplacement {
  std::optional<std::uint64_t> expected_revision;
  domain::RepositoryKnowledgeRecord record;
  auto operator==(const RepositoryKnowledgeReplacement&) const -> bool = default;
};

struct RepositoryKnowledgeUpdate {
  std::uint64_t expected_generation{};
  std::vector<RepositoryKnowledgeReplacement> replacements;
  std::vector<domain::KnowledgeRecordId> removals;
  auto operator==(const RepositoryKnowledgeUpdate&) const -> bool = default;
};

[[nodiscard]] auto validate_repository_knowledge_graph(
    const domain::RepositoryKnowledgeGraph& graph,
    const RepositoryKnowledgeLimits& limits = {})
    -> std::expected<RepositoryKnowledgeEstimate, RepositoryKnowledgeError>;

[[nodiscard]] auto assess_repository_knowledge_freshness(
    const domain::RepositoryKnowledgeRecord& record,
    const RepositoryKnowledgeEnvironment& environment,
    const RepositoryKnowledgeLimits& limits = {})
    -> std::expected<RepositoryKnowledgeAssessment, RepositoryKnowledgeError>;

[[nodiscard]] auto apply_repository_knowledge_update(
    const domain::RepositoryKnowledgeGraph& graph,
    const RepositoryKnowledgeUpdate& update,
    const RepositoryKnowledgeLimits& limits = {})
    -> std::expected<domain::RepositoryKnowledgeGraph,
                     RepositoryKnowledgeError>;

}  // namespace aiforge::repository
