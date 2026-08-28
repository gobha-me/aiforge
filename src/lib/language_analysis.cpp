#include <aiforge/repository/language_analysis.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace aiforge::repository {
namespace {

using namespace domain;

[[nodiscard]] auto failure(
    const LanguageAnalysisErrorCode code, std::string message,
    std::optional<LanguageAnalysisFeature> feature = std::nullopt,
    std::optional<KnowledgeRecordId> record_id = std::nullopt,
    const bool retryable = false) -> std::unexpected<LanguageAnalysisError> {
  return std::unexpected(
      LanguageAnalysisError{code, std::move(message), std::move(feature),
                            std::move(record_id), retryable});
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
  return std::ranges::all_of(digest.algorithm, [](const unsigned char value) {
    return std::isalnum(value) != 0 || value == '-' || value == '_';
  });
}

[[nodiscard]] auto valid_relative_path(const std::string& value,
                                       const std::size_t maximum) -> bool {
  if (!bounded_text(value, maximum)) return false;
  const std::filesystem::path path{value};
  if (path.is_absolute() || path.has_root_path() || path == "." ||
      path.lexically_normal() != path) {
    return false;
  }
  return std::ranges::none_of(path, [](const auto& component) {
    return component == ".." || component == "." || component.empty();
  });
}

[[nodiscard]] auto valid_source(const RepositorySourceIdentity& source,
                                const RepositoryKnowledgeLimits& limits,
                                const bool allow_range = true) -> bool {
  if (!valid_digest(source.snapshot.fingerprint,
                    limits.maximum_total_inline_bytes) ||
      !valid_relative_path(source.relative_path,
                           limits.maximum_metadata_bytes) ||
      !valid_digest(source.content_digest, limits.maximum_total_inline_bytes)) {
    return false;
  }
  if (!allow_range && source.range) return false;
  if (source.range && (source.range->begin >= source.range->end ||
                       source.range->end > source.content_digest.byte_size)) {
    return false;
  }
  return true;
}

[[nodiscard]] auto valid_knowledge_limits(
    const RepositoryKnowledgeLimits& limits) -> bool {
  return limits.maximum_records > 0 && limits.maximum_entities > 0 &&
         limits.maximum_sources_per_record > 0 &&
         limits.maximum_dependencies_per_record > 0 &&
         limits.maximum_declarations_per_symbol > 0 &&
         limits.maximum_metadata_bytes > 0 &&
         limits.maximum_summary_bytes > 0 &&
         limits.maximum_total_inline_bytes > 0 &&
         limits.maximum_summary_bytes <= limits.maximum_total_inline_bytes;
}

[[nodiscard]] auto valid_limits(const LanguageAnalysisLimits& limits) -> bool {
  return limits.maximum_capabilities >= 5 && limits.maximum_notices > 0 &&
         limits.maximum_notice_bytes > 0 && limits.timeout.count() > 0 &&
         valid_knowledge_limits(limits.knowledge);
}

[[nodiscard]] auto valid_file_kind(const LanguageAnalysisFileKind kind)
    -> bool {
  switch (kind) {
    case LanguageAnalysisFileKind::source:
    case LanguageAnalysisFileKind::generated:
    case LanguageAnalysisFileKind::vendor:
    case LanguageAnalysisFileKind::unknown: return true;
  }
  return false;
}

[[nodiscard]] auto valid_feature_kind(const LanguageAnalysisFeatureKind kind)
    -> bool {
  switch (kind) {
    case LanguageAnalysisFeatureKind::symbols:
    case LanguageAnalysisFeatureKind::references:
    case LanguageAnalysisFeatureKind::relationships:
    case LanguageAnalysisFeatureKind::signatures:
    case LanguageAnalysisFeatureKind::diagnostics:
    case LanguageAnalysisFeatureKind::unknown: return true;
  }
  return false;
}

[[nodiscard]] auto valid_feature(const LanguageAnalysisFeature& feature,
                                 const std::size_t maximum) -> bool {
  if (!valid_feature_kind(feature.kind)) return false;
  if (feature.kind == LanguageAnalysisFeatureKind::unknown) {
    return feature.extension_name &&
           bounded_text(*feature.extension_name, maximum);
  }
  return !feature.extension_name;
}

[[nodiscard]] auto feature_rank(const LanguageAnalysisFeatureKind kind)
    -> std::size_t {
  switch (kind) {
    case LanguageAnalysisFeatureKind::symbols: return 0;
    case LanguageAnalysisFeatureKind::references: return 1;
    case LanguageAnalysisFeatureKind::relationships: return 2;
    case LanguageAnalysisFeatureKind::signatures: return 3;
    case LanguageAnalysisFeatureKind::diagnostics: return 4;
    case LanguageAnalysisFeatureKind::unknown: return 5;
  }
  return 6;
}

[[nodiscard]] auto feature_less(const LanguageAnalysisFeature& left,
                                const LanguageAnalysisFeature& right) -> bool {
  const auto left_rank = feature_rank(left.kind);
  const auto right_rank = feature_rank(right.kind);
  if (left_rank != right_rank) return left_rank < right_rank;
  return left.extension_name.value_or("") < right.extension_name.value_or("");
}

[[nodiscard]] auto valid_support(const LanguageAnalysisSupport support)
    -> bool {
  switch (support) {
    case LanguageAnalysisSupport::supported:
    case LanguageAnalysisSupport::unsupported:
    case LanguageAnalysisSupport::unavailable:
    case LanguageAnalysisSupport::unknown: return true;
  }
  return false;
}

[[nodiscard]] auto valid_status(const LanguageAnalysisStatus status) -> bool {
  switch (status) {
    case LanguageAnalysisStatus::complete:
    case LanguageAnalysisStatus::partial:
    case LanguageAnalysisStatus::unsupported:
    case LanguageAnalysisStatus::unavailable: return true;
  }
  return false;
}

[[nodiscard]] auto valid_notice_kind(const LanguageAnalysisNoticeKind kind)
    -> bool {
  switch (kind) {
    case LanguageAnalysisNoticeKind::ambiguous:
    case LanguageAnalysisNoticeKind::incomplete:
    case LanguageAnalysisNoticeKind::generated_file:
    case LanguageAnalysisNoticeKind::vendor_file:
    case LanguageAnalysisNoticeKind::unknown: return true;
  }
  return false;
}

[[nodiscard]] auto valid_producer(const KnowledgeProducer& producer,
                                  const std::size_t maximum) -> bool {
  return bounded_text(producer.name, maximum) &&
         bounded_text(producer.version, maximum);
}

[[nodiscard]] auto valid_target(const LanguageAnalysisTarget& target,
                                const LanguageAnalysisLimits& limits) -> bool {
  return valid_source(target.source, limits.knowledge, false) &&
         bounded_text(target.language,
                      limits.knowledge.maximum_metadata_bytes) &&
         valid_file_kind(target.file_kind) &&
         (!target.build_configuration ||
          valid_digest(*target.build_configuration,
                       limits.knowledge.maximum_total_inline_bytes));
}

[[nodiscard]] auto valid_subject_kind(const KnowledgeEntityKind kind) -> bool {
  switch (kind) {
    case KnowledgeEntityKind::symbol:
    case KnowledgeEntityKind::type:
    case KnowledgeEntityKind::function:
    case KnowledgeEntityKind::method: return true;
    default: return false;
  }
}

[[nodiscard]] auto valid_subject(const KnowledgeEntity& subject,
                                 const LanguageAnalysisTarget& target,
                                 const LanguageAnalysisLimits& limits) -> bool {
  if (!valid_subject_kind(subject.kind) ||
      !bounded_text(subject.display_name,
                    limits.knowledge.maximum_metadata_bytes)) {
    return false;
  }
  return !subject.source ||
         (valid_source(*subject.source, limits.knowledge) &&
          same_source_state(subject.source->snapshot, target.source.snapshot));
}

[[nodiscard]] auto query_subject(const LanguageAnalysisQuery& query)
    -> const KnowledgeEntity* {
  if (const auto* value = std::get_if<SymbolReferencesQuery>(&query)) {
    return &value->subject;
  }
  if (const auto* value = std::get_if<SymbolRelationshipsQuery>(&query)) {
    return &value->subject;
  }
  if (const auto* value = std::get_if<SymbolSignatureQuery>(&query)) {
    return &value->subject;
  }
  return nullptr;
}

[[nodiscard]] auto known_feature_index(const LanguageAnalysisFeature& feature)
    -> std::optional<std::size_t> {
  if (feature.kind == LanguageAnalysisFeatureKind::unknown) return std::nullopt;
  return feature_rank(feature.kind);
}

[[nodiscard]] auto payload_matches_feature(
    const RepositoryKnowledgeRecord& record,
    const LanguageAnalysisFeatureKind feature) -> bool {
  switch (feature) {
    case LanguageAnalysisFeatureKind::symbols:
      return std::holds_alternative<SymbolKnowledge>(record.payload);
    case LanguageAnalysisFeatureKind::references:
      if (const auto* relationship =
              std::get_if<RelationshipKnowledge>(&record.payload)) {
        return relationship->kind == KnowledgeRelationshipKind::references;
      }
      return false;
    case LanguageAnalysisFeatureKind::relationships:
      return std::holds_alternative<RelationshipKnowledge>(record.payload);
    case LanguageAnalysisFeatureKind::signatures:
      if (const auto* symbol = std::get_if<SymbolKnowledge>(&record.payload)) {
        return symbol->signature && !symbol->signature->empty();
      }
      return false;
    case LanguageAnalysisFeatureKind::diagnostics:
      return std::holds_alternative<DiagnosticKnowledge>(record.payload);
    case LanguageAnalysisFeatureKind::unknown: return false;
  }
  return false;
}

[[nodiscard]] auto map_knowledge_error(const RepositoryKnowledgeError& error)
    -> std::unexpected<LanguageAnalysisError> {
  auto code = LanguageAnalysisErrorCode::malformed_response;
  if (error.code == RepositoryKnowledgeErrorCode::resource_exhausted ||
      error.code == RepositoryKnowledgeErrorCode::overflow) {
    code = LanguageAnalysisErrorCode::resource_exhausted;
  }
  return failure(code, "language analyzer returned invalid knowledge",
                 std::nullopt, error.record_id);
}

} // namespace

auto requested_language_analysis_feature(
    const LanguageAnalysisQuery& query) noexcept -> LanguageAnalysisFeature {
  switch (query.index()) {
    case 0: return {LanguageAnalysisFeatureKind::symbols, std::nullopt};
    case 1: return {LanguageAnalysisFeatureKind::references, std::nullopt};
    case 2: return {LanguageAnalysisFeatureKind::relationships, std::nullopt};
    case 3: return {LanguageAnalysisFeatureKind::signatures, std::nullopt};
    case 4: return {LanguageAnalysisFeatureKind::diagnostics, std::nullopt};
    default: return {LanguageAnalysisFeatureKind::unknown, "invalid.query"};
  }
}

auto validate_language_analysis_request(
    const LanguageAnalysisCapabilityRequest& request)
    -> std::expected<void, LanguageAnalysisError> {
  try {
    if (!valid_limits(request.limits) ||
        !valid_target(request.target, request.limits)) {
      return failure(LanguageAnalysisErrorCode::invalid_request,
                     "invalid language-analysis capability request");
    }
    return {};
  } catch (...) {
    return failure(LanguageAnalysisErrorCode::internal_failure,
                   "language-analysis request validation failed internally");
  }
}

auto validate_language_analysis_request(const LanguageAnalysisRequest& request)
    -> std::expected<void, LanguageAnalysisError> {
  try {
    if (request.query.valueless_by_exception() ||
        !valid_limits(request.limits) ||
        !valid_target(request.target, request.limits)) {
      return failure(LanguageAnalysisErrorCode::invalid_request,
                     "invalid language-analysis request");
    }
    const auto* subject = query_subject(request.query);
    if (subject && !valid_subject(*subject, request.target, request.limits)) {
      return failure(LanguageAnalysisErrorCode::invalid_request,
                     "language-analysis subject is invalid",
                     requested_language_analysis_feature(request.query));
    }
    return {};
  } catch (...) {
    return failure(LanguageAnalysisErrorCode::internal_failure,
                   "language-analysis request validation failed internally");
  }
}

auto validate_language_analysis_capabilities(
    const LanguageAnalysisCapabilityRequest& request,
    const LanguageAnalysisCapabilities& capabilities)
    -> std::expected<void, LanguageAnalysisError> {
  try {
    if (auto valid = validate_language_analysis_request(request); !valid) {
      return std::unexpected(valid.error());
    }
    if (!same_source_state(capabilities.target.source.snapshot,
                           request.target.source.snapshot) ||
        capabilities.target.source != request.target.source) {
      return failure(LanguageAnalysisErrorCode::stale_snapshot,
                     "language-analysis capability result is for stale source");
    }
    if (capabilities.target.build_configuration !=
        request.target.build_configuration) {
      return failure(LanguageAnalysisErrorCode::build_configuration_mismatch,
                     "language-analysis capability result used another build "
                     "configuration");
    }
    if (capabilities.target != request.target ||
        !valid_producer(capabilities.producer,
                        request.limits.knowledge.maximum_metadata_bytes) ||
        capabilities.capabilities.size() >
            request.limits.maximum_capabilities) {
      return failure(LanguageAnalysisErrorCode::invalid_result,
                     "invalid language-analysis capability result");
    }

    std::array<bool, 5> known{};
    std::optional<LanguageAnalysisFeature> previous;
    for (const auto& capability : capabilities.capabilities) {
      if (!valid_feature(capability.feature,
                         request.limits.knowledge.maximum_metadata_bytes) ||
          !valid_support(capability.support) ||
          !bounded_text(capability.detail, request.limits.maximum_notice_bytes,
                        true) ||
          (capability.support != LanguageAnalysisSupport::supported &&
           capability.detail.empty())) {
        return failure(LanguageAnalysisErrorCode::invalid_result,
                       "invalid language-analysis capability entry",
                       capability.feature);
      }
      if (previous && !feature_less(*previous, capability.feature)) {
        return failure(
            LanguageAnalysisErrorCode::invalid_result,
            "language-analysis capabilities are duplicated or unordered",
            capability.feature);
      }
      previous = capability.feature;
      if (const auto index = known_feature_index(capability.feature)) {
        known[*index] = true;
      }
    }
    if (!std::ranges::all_of(known, [](const bool value) { return value; })) {
      return failure(
          LanguageAnalysisErrorCode::invalid_result,
          "language-analysis capability result omitted a known feature");
    }
    return {};
  } catch (...) {
    return failure(LanguageAnalysisErrorCode::internal_failure,
                   "language-analysis capability validation failed internally");
  }
}

auto validate_language_analysis_result(const LanguageAnalysisRequest& request,
                                       const LanguageAnalysisResult& result)
    -> std::expected<void, LanguageAnalysisError> {
  try {
    if (auto valid = validate_language_analysis_request(request); !valid) {
      return std::unexpected(valid.error());
    }
    const auto requested = requested_language_analysis_feature(request.query);
    if (!same_source_state(result.target.source.snapshot,
                           request.target.source.snapshot) ||
        result.target.source != request.target.source) {
      return failure(LanguageAnalysisErrorCode::stale_snapshot,
                     "language-analysis result is for stale source", requested);
    }
    if (result.target.build_configuration !=
        request.target.build_configuration) {
      return failure(
          LanguageAnalysisErrorCode::build_configuration_mismatch,
          "language-analysis result used another build configuration",
          requested);
    }
    if (result.target != request.target || result.feature != requested ||
        !valid_status(result.status) ||
        !valid_producer(result.producer,
                        request.limits.knowledge.maximum_metadata_bytes) ||
        result.notices.size() > request.limits.maximum_notices) {
      return failure(LanguageAnalysisErrorCode::invalid_result,
                     "invalid language-analysis result", requested);
    }
    if ((result.status == LanguageAnalysisStatus::unsupported ||
         result.status == LanguageAnalysisStatus::unavailable) &&
        !result.records.empty()) {
      return failure(LanguageAnalysisErrorCode::invalid_result,
                     "unavailable analysis cannot contain records", requested);
    }
    if (result.status == LanguageAnalysisStatus::partial &&
        result.notices.empty()) {
      return failure(LanguageAnalysisErrorCode::invalid_result,
                     "partial analysis requires a notice", requested);
    }

    for (const auto& record : result.records) {
      if (!same_source_state(record.provenance.source_snapshot,
                             request.target.source.snapshot)) {
        return failure(LanguageAnalysisErrorCode::stale_snapshot,
                       "analysis record has stale snapshot provenance",
                       requested, record.record_id);
      }
      if (record.provenance.build_configuration !=
          request.target.build_configuration) {
        return failure(LanguageAnalysisErrorCode::build_configuration_mismatch,
                       "analysis record used another build configuration",
                       requested, record.record_id);
      }
    }

    RepositoryKnowledgeGraph graph{request.target.source.snapshot.repository_id,
                                   result.records.empty() ? 0U : 1U,
                                   result.records};
    const auto graph_result =
        validate_repository_knowledge_graph(graph, request.limits.knowledge);
    if (!graph_result) return map_knowledge_error(graph_result.error());

    std::set<KnowledgeRecordId> record_ids;
    for (const auto& record : result.records) {
      record_ids.insert(record.record_id);
      if (record.provenance.producer != result.producer ||
          record.freshness != KnowledgeFreshness::current ||
          !payload_matches_feature(record, requested.kind)) {
        return failure(LanguageAnalysisErrorCode::malformed_response,
                       "analysis record does not match its requested feature",
                       requested, record.record_id);
      }
    }

    for (const auto& notice : result.notices) {
      if (!valid_notice_kind(notice.kind) ||
          !bounded_text(notice.message, request.limits.maximum_notice_bytes) ||
          notice.related_records.size() >
              request.limits.knowledge.maximum_records ||
          (notice.kind == LanguageAnalysisNoticeKind::unknown
               ? !notice.type_name ||
                     !bounded_text(
                         *notice.type_name,
                         request.limits.knowledge.maximum_metadata_bytes)
               : notice.type_name.has_value())) {
        return failure(LanguageAnalysisErrorCode::invalid_result,
                       "invalid language-analysis notice", requested);
      }
      std::set<KnowledgeRecordId> related;
      for (const auto& record_id : notice.related_records) {
        if (!record_ids.contains(record_id) ||
            !related.insert(record_id).second) {
          return failure(
              LanguageAnalysisErrorCode::invalid_result,
              "language-analysis notice references an invalid record",
              requested, record_id);
        }
      }
      if (notice.kind == LanguageAnalysisNoticeKind::ambiguous &&
          related.size() < 2) {
        return failure(LanguageAnalysisErrorCode::invalid_result,
                       "ambiguous analysis requires multiple records",
                       requested);
      }
    }
    return {};
  } catch (...) {
    return failure(LanguageAnalysisErrorCode::internal_failure,
                   "language-analysis result validation failed internally");
  }
}

} // namespace aiforge::repository
