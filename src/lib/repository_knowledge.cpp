#include <aiforge/repository/knowledge.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace aiforge::repository {
namespace {

using namespace domain;

[[nodiscard]] auto failure(
    const RepositoryKnowledgeErrorCode code, std::string message,
    std::optional<KnowledgeRecordId> record_id = std::nullopt)
    -> std::unexpected<RepositoryKnowledgeError> {
  return std::unexpected(
      RepositoryKnowledgeError{code, std::move(message), std::move(record_id)});
}

[[nodiscard]] auto valid_utf8_text(const std::string_view value,
                                   const bool allow_empty = false) -> bool {
  if (!allow_empty && value.empty()) return false;
  std::size_t index{};
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first == 0 || first == 0x7FU ||
        (first < 0x20U && first != '\t' && first != '\n' && first != '\r')) {
      return false;
    }
    std::size_t length{};
    std::uint32_t codepoint{};
    if (first <= 0x7FU) {
      length = 1;
      codepoint = first;
    } else if ((first & 0xE0U) == 0xC0U) {
      length = 2;
      codepoint = first & 0x1FU;
      if (codepoint < 2) return false;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      codepoint = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (length > value.size() - index) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if ((length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU) {
      return false;
    }
    index += length;
  }
  return true;
}

[[nodiscard]] auto bounded_text(const std::string_view value,
                                const std::size_t maximum,
                                const bool allow_empty = false) -> bool {
  return value.size() <= maximum && valid_utf8_text(value, allow_empty);
}

[[nodiscard]] auto valid_digest(const ContentDigest& digest,
                                const std::uint64_t maximum_bytes) -> bool {
  if (!bounded_text(digest.algorithm, 128) ||
      !bounded_text(digest.value, 512) || digest.byte_size > maximum_bytes) {
    return false;
  }
  return std::ranges::all_of(digest.algorithm,
                             [](const unsigned char value) {
                               return std::isalnum(value) != 0 ||
                                      value == '-' || value == '_' ||
                                      value == '.';
                             }) &&
         std::ranges::all_of(digest.value, [](const unsigned char value) {
           return std::isxdigit(value) != 0;
         });
}

[[nodiscard]] auto valid_snapshot(const RepositorySnapshotIdentity& snapshot,
                                  const std::uint64_t maximum_bytes) -> bool {
  return valid_digest(snapshot.fingerprint, maximum_bytes);
}

[[nodiscard]] auto valid_relative_path(const std::string& value,
                                       const std::size_t maximum) -> bool {
  if (!bounded_text(value, maximum)) return false;
  const std::filesystem::path path{value};
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return false;
  }
  for (const auto& part : path) {
    if (part == "." || part == ".." || part.empty()) return false;
  }
  return path.generic_string() == value;
}

[[nodiscard]] auto add_checked(std::uint64_t& total, const std::uint64_t value)
    -> bool {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

[[nodiscard]] auto known_entity_kind(const KnowledgeEntityKind kind) noexcept
    -> bool {
  switch (kind) {
    case KnowledgeEntityKind::repository:
    case KnowledgeEntityKind::directory:
    case KnowledgeEntityKind::file:
    case KnowledgeEntityKind::source_range:
    case KnowledgeEntityKind::symbol:
    case KnowledgeEntityKind::type:
    case KnowledgeEntityKind::function:
    case KnowledgeEntityKind::method:
    case KnowledgeEntityKind::module:
    case KnowledgeEntityKind::package:
    case KnowledgeEntityKind::build_target:
    case KnowledgeEntityKind::test:
    case KnowledgeEntityKind::diagnostic:
    case KnowledgeEntityKind::configuration:
    case KnowledgeEntityKind::artifact: return true;
    case KnowledgeEntityKind::unknown: return false;
  }
  return false;
}

[[nodiscard]] auto known_relationship_kind(
    const KnowledgeRelationshipKind kind) noexcept -> bool {
  switch (kind) {
    case KnowledgeRelationshipKind::contains:
    case KnowledgeRelationshipKind::defines:
    case KnowledgeRelationshipKind::references:
    case KnowledgeRelationshipKind::calls:
    case KnowledgeRelationshipKind::imports:
    case KnowledgeRelationshipKind::includes:
    case KnowledgeRelationshipKind::inherits:
    case KnowledgeRelationshipKind::implements:
    case KnowledgeRelationshipKind::builds:
    case KnowledgeRelationshipKind::depends_on:
    case KnowledgeRelationshipKind::generates:
    case KnowledgeRelationshipKind::generated_from:
    case KnowledgeRelationshipKind::tests:
    case KnowledgeRelationshipKind::diagnoses:
    case KnowledgeRelationshipKind::changed_by: return true;
    case KnowledgeRelationshipKind::unknown: return false;
  }
  return false;
}

[[nodiscard]] auto known_diagnostic_severity(
    const KnowledgeDiagnosticSeverity severity) noexcept -> bool {
  switch (severity) {
    case KnowledgeDiagnosticSeverity::note:
    case KnowledgeDiagnosticSeverity::warning:
    case KnowledgeDiagnosticSeverity::error:
    case KnowledgeDiagnosticSeverity::fatal:
    case KnowledgeDiagnosticSeverity::unknown: return true;
  }
  return false;
}

[[nodiscard]] auto known_availability(
    const KnowledgeInputAvailability availability) noexcept -> bool {
  switch (availability) {
    case KnowledgeInputAvailability::available:
    case KnowledgeInputAvailability::unavailable:
    case KnowledgeInputAvailability::unknown: return true;
  }
  return false;
}

[[nodiscard]] auto known_derivation(const KnowledgeDerivation value) noexcept
    -> bool {
  return value == KnowledgeDerivation::observed ||
         value == KnowledgeDerivation::derived ||
         value == KnowledgeDerivation::inferred;
}

[[nodiscard]] auto known_confidence(const KnowledgeConfidence value) noexcept
    -> bool {
  return value == KnowledgeConfidence::certain ||
         value == KnowledgeConfidence::high ||
         value == KnowledgeConfidence::medium ||
         value == KnowledgeConfidence::low;
}

[[nodiscard]] auto known_freshness(const KnowledgeFreshness value) noexcept
    -> bool {
  switch (value) {
    case KnowledgeFreshness::current:
    case KnowledgeFreshness::possibly_stale:
    case KnowledgeFreshness::stale:
    case KnowledgeFreshness::unavailable: return true;
  }
  return false;
}

[[nodiscard]] auto known_trigger(
    const KnowledgeInvalidationTrigger trigger) noexcept -> bool {
  switch (trigger) {
    case KnowledgeInvalidationTrigger::source_snapshot_changed:
    case KnowledgeInvalidationTrigger::source_digest_changed:
    case KnowledgeInvalidationTrigger::dependency_changed:
    case KnowledgeInvalidationTrigger::producer_version_changed:
    case KnowledgeInvalidationTrigger::build_configuration_changed: return true;
    case KnowledgeInvalidationTrigger::unknown: return false;
  }
  return false;
}

[[nodiscard]] auto validate_source(const RepositorySourceIdentity& source,
                                   const RepositoryId& repository_id,
                                   const RepositoryKnowledgeLimits& limits,
                                   const KnowledgeRecordId& record_id)
    -> std::expected<void, RepositoryKnowledgeError> {
  if (source.snapshot.repository_id != repository_id ||
      !valid_snapshot(source.snapshot, limits.maximum_total_inline_bytes) ||
      !valid_relative_path(source.relative_path,
                           limits.maximum_metadata_bytes) ||
      !valid_digest(source.content_digest, limits.maximum_total_inline_bytes)) {
    return failure(RepositoryKnowledgeErrorCode::invalid_source,
                   "repository knowledge source identity is invalid",
                   record_id);
  }
  if (source.range && (source.range->begin >= source.range->end ||
                       source.range->end > source.content_digest.byte_size)) {
    return failure(RepositoryKnowledgeErrorCode::invalid_source,
                   "repository knowledge source range is invalid", record_id);
  }
  return {};
}

[[nodiscard]] auto source_key(const RepositorySourceIdentity& source)
    -> std::string {
  auto key = source.relative_path;
  key.push_back('\0');
  key.append(source.content_digest.algorithm);
  key.push_back('\0');
  key.append(source.content_digest.value);
  key.push_back('\0');
  if (source.range) {
    key.append(std::to_string(source.range->begin));
    key.push_back(':');
    key.append(std::to_string(source.range->end));
  }
  return key;
}

[[nodiscard]] auto entity_sources(const RepositoryKnowledgePayload& payload)
    -> std::vector<const RepositorySourceIdentity*> {
  return std::visit(
      [](const auto& value) {
        using Payload = std::decay_t<decltype(value)>;
        std::vector<const RepositorySourceIdentity*> sources;
        if constexpr (std::is_same_v<Payload, SymbolKnowledge>) {
          if (value.symbol.source) sources.push_back(&*value.symbol.source);
          for (const auto& declaration : value.declarations) {
            sources.push_back(&declaration);
          }
          if (value.definition) sources.push_back(&*value.definition);
        } else if constexpr (std::is_same_v<Payload, RelationshipKnowledge>) {
          if (value.source.source) sources.push_back(&*value.source.source);
          if (value.target.source) sources.push_back(&*value.target.source);
          if (value.evidence_source) sources.push_back(&*value.evidence_source);
        } else if constexpr (std::is_same_v<Payload, DiagnosticKnowledge>) {
          if (value.subject.source) sources.push_back(&*value.subject.source);
          if (value.source) sources.push_back(&*value.source);
        } else {
          if (value.subject.source) sources.push_back(&*value.subject.source);
        }
        return sources;
      },
      payload);
}

[[nodiscard]] auto entities(const RepositoryKnowledgePayload& payload)
    -> std::vector<const KnowledgeEntity*> {
  return std::visit(
      [](const auto& value) {
        using Payload = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Payload, RelationshipKnowledge>) {
          return std::vector<const KnowledgeEntity*>{&value.source,
                                                     &value.target};
        } else if constexpr (std::is_same_v<Payload, SymbolKnowledge>) {
          return std::vector<const KnowledgeEntity*>{&value.symbol};
        } else {
          return std::vector<const KnowledgeEntity*>{&value.subject};
        }
      },
      payload);
}

[[nodiscard]] auto validate_entity(const KnowledgeEntity& entity,
                                   const RepositoryId& repository_id,
                                   const RepositoryKnowledgeLimits& limits,
                                   const KnowledgeRecordId& record_id,
                                   const bool allow_unknown)
    -> std::expected<void, RepositoryKnowledgeError> {
  if ((!known_entity_kind(entity.kind) &&
       !(allow_unknown && entity.kind == KnowledgeEntityKind::unknown)) ||
      !bounded_text(entity.display_name, limits.maximum_metadata_bytes)) {
    return failure(RepositoryKnowledgeErrorCode::invalid_entity,
                   "repository knowledge entity is invalid", record_id);
  }
  if (entity.source) {
    return validate_source(*entity.source, repository_id, limits, record_id);
  }
  return {};
}

[[nodiscard]] auto payload_inline_bytes(
    const RepositoryKnowledgePayload& payload,
    const RepositoryKnowledgeLimits& limits, const KnowledgeRecordId& record_id)
    -> std::expected<std::uint64_t, RepositoryKnowledgeError> {
  return std::visit(
      [&](const auto& value)
          -> std::expected<std::uint64_t, RepositoryKnowledgeError> {
        using Payload = std::decay_t<decltype(value)>;
        std::uint64_t bytes{};
        const auto add = [&](const std::string_view text) {
          return add_checked(bytes, text.size());
        };
        if constexpr (std::is_same_v<Payload, SymbolKnowledge>) {
          if (!known_entity_kind(value.symbol.kind) ||
              !bounded_text(value.language, limits.maximum_metadata_bytes) ||
              (value.qualified_name &&
               !bounded_text(*value.qualified_name,
                             limits.maximum_metadata_bytes)) ||
              (value.signature &&
               !bounded_text(*value.signature, limits.maximum_summary_bytes)) ||
              value.declarations.size() >
                  limits.maximum_declarations_per_symbol ||
              (value.declarations.empty() && !value.definition &&
               !value.symbol.source)) {
            return failure(RepositoryKnowledgeErrorCode::invalid_record,
                           "symbol knowledge is invalid", record_id);
          }
          if (!add(value.language) ||
              (value.qualified_name && !add(*value.qualified_name)) ||
              (value.signature && !add(*value.signature))) {
            return failure(RepositoryKnowledgeErrorCode::overflow,
                           "symbol knowledge size overflowed", record_id);
          }
        } else if constexpr (std::is_same_v<Payload, RelationshipKnowledge>) {
          if (!known_relationship_kind(value.kind)) {
            return failure(RepositoryKnowledgeErrorCode::invalid_record,
                           "repository relationship kind is invalid",
                           record_id);
          }
        } else if constexpr (std::is_same_v<Payload, DiagnosticKnowledge>) {
          if (!known_diagnostic_severity(value.severity) ||
              !bounded_text(value.code, limits.maximum_metadata_bytes, true) ||
              !bounded_text(value.message, limits.maximum_summary_bytes)) {
            return failure(RepositoryKnowledgeErrorCode::invalid_record,
                           "diagnostic knowledge is invalid", record_id);
          }
          if (!add(value.code) || !add(value.message)) {
            return failure(RepositoryKnowledgeErrorCode::overflow,
                           "diagnostic knowledge size overflowed", record_id);
          }
        } else if constexpr (std::is_same_v<Payload,
                                            SemanticSummaryKnowledge>) {
          if (!bounded_text(value.purpose, limits.maximum_metadata_bytes) ||
              !bounded_text(value.summary, limits.maximum_summary_bytes)) {
            return failure(RepositoryKnowledgeErrorCode::invalid_record,
                           "semantic summary knowledge is invalid", record_id);
          }
          if (!add(value.purpose) || !add(value.summary)) {
            return failure(RepositoryKnowledgeErrorCode::overflow,
                           "semantic summary size overflowed", record_id);
          }
        } else {
          if (!bounded_text(value.type_name, limits.maximum_metadata_bytes)) {
            return failure(RepositoryKnowledgeErrorCode::invalid_record,
                           "unknown repository knowledge type is invalid",
                           record_id);
          }
          if (!add(value.type_name)) {
            return failure(RepositoryKnowledgeErrorCode::overflow,
                           "unknown repository knowledge size overflowed",
                           record_id);
          }
        }
        for (const auto* entity : entities(payload)) {
          if (!add(entity->display_name)) {
            return failure(RepositoryKnowledgeErrorCode::overflow,
                           "repository entity size overflowed", record_id);
          }
        }
        return bytes;
      },
      payload);
}

[[nodiscard]] auto validate_record(const RepositoryKnowledgeRecord& record,
                                   const RepositoryId& repository_id,
                                   const RepositoryKnowledgeLimits& limits)
    -> std::expected<std::uint64_t, RepositoryKnowledgeError> {
  if (record.revision == 0 ||
      !valid_snapshot(record.provenance.source_snapshot,
                      limits.maximum_total_inline_bytes) ||
      record.provenance.source_snapshot.repository_id != repository_id ||
      !bounded_text(record.provenance.producer.name,
                    limits.maximum_metadata_bytes) ||
      !bounded_text(record.provenance.producer.version,
                    limits.maximum_metadata_bytes) ||
      record.provenance.created_at.time_since_epoch().count() < 0 ||
      record.provenance.sources.size() > limits.maximum_sources_per_record ||
      record.provenance.derivation_inputs.size() >
          limits.maximum_dependencies_per_record) {
    return failure(RepositoryKnowledgeErrorCode::invalid_provenance,
                   "repository knowledge provenance is invalid",
                   record.record_id);
  }
  if (!known_freshness(record.freshness)) {
    return failure(RepositoryKnowledgeErrorCode::invalid_freshness,
                   "repository knowledge freshness is invalid",
                   record.record_id);
  }

  const bool unknown_payload =
      std::holds_alternative<UnknownRepositoryKnowledge>(record.payload);
  if ((!unknown_payload && (!known_derivation(record.provenance.derivation) ||
                            !known_confidence(record.provenance.confidence))) ||
      (unknown_payload &&
       record.provenance.derivation != KnowledgeDerivation::unknown)) {
    return failure(RepositoryKnowledgeErrorCode::invalid_provenance,
                   "repository knowledge derivation is invalid",
                   record.record_id);
  }

  const bool derived =
      record.provenance.derivation == KnowledgeDerivation::derived ||
      record.provenance.derivation == KnowledgeDerivation::inferred;
  if ((!derived && !record.provenance.derivation_inputs.empty()) ||
      (derived && record.provenance.derivation_inputs.empty())) {
    return failure(RepositoryKnowledgeErrorCode::invalid_provenance,
                   "repository knowledge dependencies are inconsistent",
                   record.record_id);
  }

  std::set<KnowledgeRecordId> dependency_ids;
  for (const auto& dependency : record.provenance.derivation_inputs) {
    if (dependency.revision == 0 || dependency.record_id == record.record_id ||
        !dependency_ids.insert(dependency.record_id).second) {
      return failure(RepositoryKnowledgeErrorCode::invalid_provenance,
                     "repository knowledge dependency is invalid",
                     record.record_id);
    }
  }

  std::set<std::string> source_ids;
  for (const auto& source : record.provenance.sources) {
    auto valid =
        validate_source(source, repository_id, limits, record.record_id);
    if (!valid) return std::unexpected(valid.error());
    if (!same_source_state(source.snapshot,
                           record.provenance.source_snapshot) ||
        !source_ids.insert(source_key(source)).second) {
      return failure(RepositoryKnowledgeErrorCode::invalid_provenance,
                     "repository knowledge source provenance conflicts",
                     record.record_id);
    }
  }

  if (record.provenance.build_configuration &&
      !valid_digest(*record.provenance.build_configuration,
                    limits.maximum_total_inline_bytes)) {
    return failure(RepositoryKnowledgeErrorCode::invalid_provenance,
                   "repository build configuration identity is invalid",
                   record.record_id);
  }

  if (record.invalidation.triggers.empty()) {
    return failure(RepositoryKnowledgeErrorCode::invalid_invalidation_rule,
                   "repository knowledge invalidation rule is empty",
                   record.record_id);
  }
  std::set<KnowledgeInvalidationTrigger> triggers;
  for (const auto trigger : record.invalidation.triggers) {
    if (!known_trigger(trigger) || !triggers.insert(trigger).second) {
      return failure(RepositoryKnowledgeErrorCode::invalid_invalidation_rule,
                     "repository knowledge invalidation trigger is invalid",
                     record.record_id);
    }
  }
  if ((triggers.contains(KnowledgeInvalidationTrigger::source_digest_changed) &&
       record.provenance.sources.empty()) ||
      (triggers.contains(KnowledgeInvalidationTrigger::dependency_changed) &&
       record.provenance.derivation_inputs.empty()) ||
      (triggers.contains(
           KnowledgeInvalidationTrigger::build_configuration_changed) &&
       !record.provenance.build_configuration)) {
    return failure(RepositoryKnowledgeErrorCode::invalid_invalidation_rule,
                   "repository knowledge invalidation baseline is missing",
                   record.record_id);
  }

  for (const auto* entity : entities(record.payload)) {
    auto valid = validate_entity(*entity, repository_id, limits,
                                 record.record_id, unknown_payload);
    if (!valid) return std::unexpected(valid.error());
  }

  for (const auto* source : entity_sources(record.payload)) {
    auto valid =
        validate_source(*source, repository_id, limits, record.record_id);
    if (!valid) return std::unexpected(valid.error());
    if (!source_ids.contains(source_key(*source))) {
      return failure(RepositoryKnowledgeErrorCode::invalid_provenance,
                     "record source is absent from its provenance",
                     record.record_id);
    }
  }

  return payload_inline_bytes(record.payload, limits, record.record_id);
}

[[nodiscard]] auto validate_limits(const RepositoryKnowledgeLimits& limits)
    -> bool {
  return limits.maximum_records > 0 && limits.maximum_entities > 0 &&
         limits.maximum_sources_per_record > 0 &&
         limits.maximum_dependencies_per_record > 0 &&
         limits.maximum_declarations_per_symbol > 0 &&
         limits.maximum_metadata_bytes > 0 &&
         limits.maximum_summary_bytes > 0 &&
         limits.maximum_total_inline_bytes > 0 &&
         limits.maximum_summary_bytes <= limits.maximum_total_inline_bytes;
}

[[nodiscard]] auto has_dependency_cycle(
    const std::map<KnowledgeRecordId, const RepositoryKnowledgeRecord*>&
        records) -> bool {
  enum class VisitState { visiting, visited };
  std::map<KnowledgeRecordId, VisitState> states;
  const auto visit = [&](const auto& self,
                         const RepositoryKnowledgeRecord& record) -> bool {
    const auto state = states.find(record.record_id);
    if (state != states.end()) {
      return state->second == VisitState::visiting;
    }
    states.emplace(record.record_id, VisitState::visiting);
    for (const auto& dependency : record.provenance.derivation_inputs) {
      const auto found = records.find(dependency.record_id);
      if (found != records.end() && self(self, *found->second)) return true;
    }
    states[record.record_id] = VisitState::visited;
    return false;
  };
  return std::ranges::any_of(
      records, [&](const auto& entry) { return visit(visit, *entry.second); });
}

[[nodiscard]] auto validate_graph_impl(const RepositoryKnowledgeGraph& graph,
                                       const RepositoryKnowledgeLimits& limits)
    -> std::expected<RepositoryKnowledgeEstimate, RepositoryKnowledgeError> {
  if (!validate_limits(limits)) {
    return failure(RepositoryKnowledgeErrorCode::invalid_limits,
                   "repository knowledge limits are invalid");
  }
  if (!graph.records.empty() && graph.generation == 0) {
    return failure(RepositoryKnowledgeErrorCode::invalid_graph,
                   "repository knowledge generation is invalid");
  }
  if (graph.records.size() > limits.maximum_records) {
    return failure(RepositoryKnowledgeErrorCode::resource_exhausted,
                   "repository knowledge record limit exceeded");
  }

  RepositoryKnowledgeEstimate estimate;
  estimate.record_count = graph.records.size();
  std::map<KnowledgeRecordId, const RepositoryKnowledgeRecord*> records;
  struct EntityState {
    KnowledgeEntity entity;
    KnowledgeFreshness freshness;
  };
  std::map<KnowledgeEntityId, EntityState> known_entities;
  for (const auto& record : graph.records) {
    if (!records.emplace(record.record_id, &record).second) {
      return failure(RepositoryKnowledgeErrorCode::duplicate_record,
                     "repository knowledge record identity is duplicated",
                     record.record_id);
    }
    auto bytes = validate_record(record, graph.repository_id, limits);
    if (!bytes) return std::unexpected(bytes.error());
    if (!add_checked(estimate.inline_bytes, *bytes)) {
      return failure(RepositoryKnowledgeErrorCode::overflow,
                     "repository knowledge inline size overflowed",
                     record.record_id);
    }
    if (estimate.inline_bytes > limits.maximum_total_inline_bytes) {
      return failure(RepositoryKnowledgeErrorCode::resource_exhausted,
                     "repository knowledge inline limit exceeded",
                     record.record_id);
    }
    if (std::holds_alternative<RelationshipKnowledge>(record.payload)) {
      ++estimate.relationship_count;
    }
    for (const auto* entity : entities(record.payload)) {
      const auto [found, inserted] = known_entities.emplace(
          entity->entity_id, EntityState{*entity, record.freshness});
      if (!inserted && found->second.entity != *entity &&
          found->second.freshness == KnowledgeFreshness::current &&
          record.freshness == KnowledgeFreshness::current) {
        return failure(RepositoryKnowledgeErrorCode::conflicting_entity,
                       "repository knowledge entity identity conflicts",
                       record.record_id);
      }
      if (!inserted && record.freshness == KnowledgeFreshness::current) {
        found->second = EntityState{*entity, record.freshness};
      }
    }
  }
  estimate.entity_count = known_entities.size();
  if (estimate.entity_count > limits.maximum_entities) {
    return failure(RepositoryKnowledgeErrorCode::resource_exhausted,
                   "repository knowledge entity limit exceeded");
  }

  for (const auto& record : graph.records) {
    for (const auto& dependency : record.provenance.derivation_inputs) {
      const auto found = records.find(dependency.record_id);
      if (found == records.end()) {
        if (record.freshness != KnowledgeFreshness::unavailable) {
          return failure(RepositoryKnowledgeErrorCode::missing_dependency,
                         "repository knowledge dependency is unavailable",
                         record.record_id);
        }
      } else if (found->second->revision != dependency.revision &&
                 record.freshness == KnowledgeFreshness::current) {
        return failure(RepositoryKnowledgeErrorCode::invalid_freshness,
                       "current knowledge references an obsolete dependency",
                       record.record_id);
      }
    }
  }
  if (has_dependency_cycle(records)) {
    return failure(RepositoryKnowledgeErrorCode::cyclic_dependency,
                   "repository knowledge derivation contains a cycle");
  }
  return estimate;
}

[[nodiscard]] auto worse_freshness(const KnowledgeFreshness left,
                                   const KnowledgeFreshness right) noexcept
    -> KnowledgeFreshness {
  const auto rank = [](const KnowledgeFreshness value) {
    switch (value) {
      case KnowledgeFreshness::current: return 0;
      case KnowledgeFreshness::possibly_stale: return 1;
      case KnowledgeFreshness::stale: return 2;
      case KnowledgeFreshness::unavailable: return 3;
    }
    return 3;
  };
  return rank(left) >= rank(right) ? left : right;
}

[[nodiscard]] auto validate_environment(
    const RepositoryKnowledgeEnvironment& environment,
    const RepositoryKnowledgeLimits& limits, const KnowledgeRecordId& record_id)
    -> std::expected<void, RepositoryKnowledgeError> {
  if (environment.sources.size() > limits.maximum_sources_per_record ||
      environment.dependencies.size() >
          limits.maximum_dependencies_per_record) {
    return failure(RepositoryKnowledgeErrorCode::resource_exhausted,
                   "repository knowledge environment exceeds its limits",
                   record_id);
  }
  if (environment.source_snapshot &&
      !valid_snapshot(*environment.source_snapshot,
                      limits.maximum_total_inline_bytes)) {
    return failure(RepositoryKnowledgeErrorCode::invalid_source,
                   "repository knowledge environment snapshot is invalid",
                   record_id);
  }
  std::set<std::string> paths;
  for (const auto& source : environment.sources) {
    if (!valid_relative_path(source.relative_path,
                             limits.maximum_metadata_bytes) ||
        !paths.insert(source.relative_path).second ||
        !known_availability(source.availability) ||
        (source.availability == KnowledgeInputAvailability::available) !=
            source.content_digest.has_value() ||
        (source.content_digest &&
         !valid_digest(*source.content_digest,
                       limits.maximum_total_inline_bytes))) {
      return failure(RepositoryKnowledgeErrorCode::invalid_source,
                     "repository source observation is invalid", record_id);
    }
  }
  std::set<KnowledgeRecordId> dependencies;
  for (const auto& dependency : environment.dependencies) {
    if (!dependencies.insert(dependency.record_id).second ||
        !known_availability(dependency.availability) ||
        (dependency.availability == KnowledgeInputAvailability::available &&
         (!dependency.revision || *dependency.revision == 0)) ||
        (dependency.availability != KnowledgeInputAvailability::available &&
         dependency.revision)) {
      return failure(RepositoryKnowledgeErrorCode::invalid_provenance,
                     "repository dependency observation is invalid", record_id);
    }
  }
  if (environment.producer && (!bounded_text(environment.producer->name,
                                             limits.maximum_metadata_bytes) ||
                               !bounded_text(environment.producer->version,
                                             limits.maximum_metadata_bytes))) {
    return failure(RepositoryKnowledgeErrorCode::invalid_provenance,
                   "repository producer observation is invalid", record_id);
  }
  if (environment.build_configuration &&
      !valid_digest(*environment.build_configuration,
                    limits.maximum_total_inline_bytes)) {
    return failure(RepositoryKnowledgeErrorCode::invalid_provenance,
                   "repository build configuration observation is invalid",
                   record_id);
  }
  return {};
}

[[nodiscard]] auto assess_impl(
    const RepositoryKnowledgeRecord& record,
    const RepositoryKnowledgeEnvironment& environment,
    const RepositoryKnowledgeLimits& limits)
    -> std::expected<RepositoryKnowledgeAssessment, RepositoryKnowledgeError> {
  if (!validate_limits(limits)) {
    return failure(RepositoryKnowledgeErrorCode::invalid_limits,
                   "repository knowledge limits are invalid");
  }
  auto valid_record = validate_record(
      record, record.provenance.source_snapshot.repository_id, limits);
  if (!valid_record) return std::unexpected(valid_record.error());
  auto valid_environment =
      validate_environment(environment, limits, record.record_id);
  if (!valid_environment) return std::unexpected(valid_environment.error());

  RepositoryKnowledgeAssessment result{
      record.record_id, KnowledgeFreshness::current, {}};
  const auto affect = [&](const KnowledgeInvalidationTrigger trigger,
                          const KnowledgeFreshness freshness) {
    result.freshness = worse_freshness(result.freshness, freshness);
    if (!std::ranges::contains(result.affected_triggers, trigger)) {
      result.affected_triggers.push_back(trigger);
    }
  };
  for (const auto trigger : record.invalidation.triggers) {
    switch (trigger) {
      case KnowledgeInvalidationTrigger::source_snapshot_changed:
        if (!environment.source_snapshot) {
          affect(trigger, KnowledgeFreshness::possibly_stale);
        } else if (environment.source_snapshot->repository_id !=
                   record.provenance.source_snapshot.repository_id) {
          affect(trigger, KnowledgeFreshness::unavailable);
        } else if (!same_source_state(*environment.source_snapshot,
                                      record.provenance.source_snapshot)) {
          affect(trigger, KnowledgeFreshness::stale);
        }
        break;
      case KnowledgeInvalidationTrigger::source_digest_changed:
        for (const auto& baseline : record.provenance.sources) {
          const auto found =
              std::ranges::find(environment.sources, baseline.relative_path,
                                &KnowledgeSourceObservation::relative_path);
          if (found == environment.sources.end()) {
            affect(trigger, environment.source_observation_complete
                                ? KnowledgeFreshness::unavailable
                                : KnowledgeFreshness::possibly_stale);
          } else if (found->availability ==
                     KnowledgeInputAvailability::unavailable) {
            affect(trigger, KnowledgeFreshness::unavailable);
          } else if (found->availability ==
                     KnowledgeInputAvailability::unknown) {
            affect(trigger, KnowledgeFreshness::possibly_stale);
          } else if (*found->content_digest != baseline.content_digest) {
            affect(trigger, KnowledgeFreshness::stale);
          }
        }
        break;
      case KnowledgeInvalidationTrigger::dependency_changed:
        for (const auto& baseline : record.provenance.derivation_inputs) {
          const auto found =
              std::ranges::find(environment.dependencies, baseline.record_id,
                                &KnowledgeDependencyObservation::record_id);
          if (found == environment.dependencies.end()) {
            affect(trigger, environment.dependency_observation_complete
                                ? KnowledgeFreshness::unavailable
                                : KnowledgeFreshness::possibly_stale);
          } else if (found->availability ==
                     KnowledgeInputAvailability::unavailable) {
            affect(trigger, KnowledgeFreshness::unavailable);
          } else if (found->availability ==
                     KnowledgeInputAvailability::unknown) {
            affect(trigger, KnowledgeFreshness::possibly_stale);
          } else if (*found->revision != baseline.revision) {
            affect(trigger, KnowledgeFreshness::stale);
          }
        }
        break;
      case KnowledgeInvalidationTrigger::producer_version_changed:
        if (!environment.producer) {
          affect(trigger, KnowledgeFreshness::possibly_stale);
        } else if (*environment.producer != record.provenance.producer) {
          affect(trigger, KnowledgeFreshness::stale);
        }
        break;
      case KnowledgeInvalidationTrigger::build_configuration_changed:
        if (!environment.build_configuration) {
          affect(trigger, KnowledgeFreshness::possibly_stale);
        } else if (*environment.build_configuration !=
                   *record.provenance.build_configuration) {
          affect(trigger, KnowledgeFreshness::stale);
        }
        break;
      case KnowledgeInvalidationTrigger::unknown:
        return failure(RepositoryKnowledgeErrorCode::invalid_invalidation_rule,
                       "repository knowledge invalidation trigger is unknown",
                       record.record_id);
    }
  }
  return result;
}

[[nodiscard]] auto apply_update_impl(const RepositoryKnowledgeGraph& graph,
                                     const RepositoryKnowledgeUpdate& update,
                                     const RepositoryKnowledgeLimits& limits)
    -> std::expected<RepositoryKnowledgeGraph, RepositoryKnowledgeError> {
  auto valid = validate_graph_impl(graph, limits);
  if (!valid) return std::unexpected(valid.error());
  if (update.expected_generation != graph.generation) {
    return failure(RepositoryKnowledgeErrorCode::generation_conflict,
                   "repository knowledge generation changed");
  }
  if (update.replacements.empty() && update.removals.empty()) {
    return failure(RepositoryKnowledgeErrorCode::invalid_graph,
                   "repository knowledge update is empty");
  }
  if (update.replacements.size() > limits.maximum_records ||
      update.removals.size() > limits.maximum_records) {
    return failure(RepositoryKnowledgeErrorCode::resource_exhausted,
                   "repository knowledge update exceeds its record limit");
  }
  if (graph.generation == std::numeric_limits<std::uint64_t>::max()) {
    return failure(RepositoryKnowledgeErrorCode::overflow,
                   "repository knowledge generation overflowed");
  }

  std::set<KnowledgeRecordId> operations;
  for (const auto& replacement : update.replacements) {
    if (!operations.insert(replacement.record.record_id).second) {
      return failure(RepositoryKnowledgeErrorCode::duplicate_record,
                     "repository knowledge update repeats a record",
                     replacement.record.record_id);
    }
  }
  for (const auto& removal : update.removals) {
    if (!operations.insert(removal).second) {
      return failure(RepositoryKnowledgeErrorCode::duplicate_record,
                     "repository knowledge update repeats a record", removal);
    }
  }

  auto result = graph;
  std::set<KnowledgeRecordId> changed;
  std::set<KnowledgeRecordId> removed;
  for (const auto& replacement : update.replacements) {
    const auto found =
        std::ranges::find(result.records, replacement.record.record_id,
                          &RepositoryKnowledgeRecord::record_id);
    if (!replacement.expected_revision) {
      if (found != result.records.end() || replacement.record.revision != 1) {
        return failure(RepositoryKnowledgeErrorCode::revision_conflict,
                       "repository knowledge insertion revision conflicts",
                       replacement.record.record_id);
      }
      result.records.push_back(replacement.record);
      changed.insert(replacement.record.record_id);
      continue;
    }
    if (found == result.records.end() ||
        found->revision != *replacement.expected_revision ||
        found->revision == std::numeric_limits<std::uint64_t>::max() ||
        replacement.record.revision != found->revision + 1) {
      return failure(RepositoryKnowledgeErrorCode::revision_conflict,
                     "repository knowledge replacement revision conflicts",
                     replacement.record.record_id);
    }
    *found = replacement.record;
    changed.insert(replacement.record.record_id);
  }

  for (const auto& removal : update.removals) {
    const auto found = std::ranges::find(result.records, removal,
                                         &RepositoryKnowledgeRecord::record_id);
    if (found == result.records.end()) {
      return failure(RepositoryKnowledgeErrorCode::revision_conflict,
                     "repository knowledge removal target is missing", removal);
    }
    result.records.erase(found);
    changed.insert(removal);
    removed.insert(removal);
  }

  std::set<KnowledgeRecordId> invalidated;
  bool progress = true;
  while (progress) {
    progress = false;
    for (auto& record : result.records) {
      if (operations.contains(record.record_id) ||
          invalidated.contains(record.record_id)) {
        continue;
      }
      auto next_freshness = record.freshness;
      bool depends_on_change = false;
      for (const auto& dependency : record.provenance.derivation_inputs) {
        if (!changed.contains(dependency.record_id)) continue;
        depends_on_change = true;
        next_freshness = worse_freshness(next_freshness,
                                         removed.contains(dependency.record_id)
                                             ? KnowledgeFreshness::unavailable
                                             : KnowledgeFreshness::stale);
      }
      if (!depends_on_change) continue;
      if (record.revision == std::numeric_limits<std::uint64_t>::max()) {
        return failure(RepositoryKnowledgeErrorCode::overflow,
                       "repository knowledge revision overflowed",
                       record.record_id);
      }
      ++record.revision;
      record.freshness = next_freshness;
      invalidated.insert(record.record_id);
      changed.insert(record.record_id);
      progress = true;
    }
  }

  ++result.generation;
  std::ranges::sort(result.records, {}, &RepositoryKnowledgeRecord::record_id);
  auto result_valid = validate_graph_impl(result, limits);
  if (!result_valid) return std::unexpected(result_valid.error());
  return result;
}

} // namespace

auto validate_repository_knowledge_graph(
    const domain::RepositoryKnowledgeGraph& graph,
    const RepositoryKnowledgeLimits& limits)
    -> std::expected<RepositoryKnowledgeEstimate, RepositoryKnowledgeError> {
  try {
    return validate_graph_impl(graph, limits);
  } catch (...) {
    return failure(RepositoryKnowledgeErrorCode::internal_failure,
                   "repository knowledge validation failed internally");
  }
}

auto assess_repository_knowledge_freshness(
    const domain::RepositoryKnowledgeRecord& record,
    const RepositoryKnowledgeEnvironment& environment,
    const RepositoryKnowledgeLimits& limits)
    -> std::expected<RepositoryKnowledgeAssessment, RepositoryKnowledgeError> {
  try {
    return assess_impl(record, environment, limits);
  } catch (...) {
    return failure(RepositoryKnowledgeErrorCode::internal_failure,
                   "repository knowledge assessment failed internally",
                   record.record_id);
  }
}

auto apply_repository_knowledge_update(
    const domain::RepositoryKnowledgeGraph& graph,
    const RepositoryKnowledgeUpdate& update,
    const RepositoryKnowledgeLimits& limits)
    -> std::expected<domain::RepositoryKnowledgeGraph,
                     RepositoryKnowledgeError> {
  try {
    return apply_update_impl(graph, update, limits);
  } catch (...) {
    return failure(RepositoryKnowledgeErrorCode::internal_failure,
                   "repository knowledge update failed internally");
  }
}

} // namespace aiforge::repository
