#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

#include <aiforge/repository/knowledge.hpp>

namespace aiforge::repository {

enum class LanguageAnalysisFeatureKind {
  symbols,
  references,
  relationships,
  signatures,
  diagnostics,
  unknown,
};

struct LanguageAnalysisFeature {
  LanguageAnalysisFeatureKind kind{LanguageAnalysisFeatureKind::unknown};
  std::optional<std::string> extension_name;
  auto operator==(const LanguageAnalysisFeature&) const -> bool = default;
};

enum class LanguageAnalysisFileKind {
  source,
  generated,
  vendor,
  unknown,
};

struct LanguageAnalysisLimits {
  std::size_t maximum_capabilities{64};
  std::size_t maximum_notices{1024};
  std::size_t maximum_notice_bytes{4096};
  std::chrono::milliseconds timeout{30000};
  RepositoryKnowledgeLimits knowledge;
  auto operator==(const LanguageAnalysisLimits&) const -> bool = default;
};

struct LanguageAnalysisTarget {
  domain::RepositorySourceIdentity source;
  std::string language;
  LanguageAnalysisFileKind file_kind{LanguageAnalysisFileKind::unknown};
  std::optional<domain::ContentDigest> build_configuration;
  auto operator==(const LanguageAnalysisTarget&) const -> bool = default;
};

struct LanguageAnalysisCapabilityRequest {
  LanguageAnalysisTarget target;
  LanguageAnalysisLimits limits;
  auto operator==(const LanguageAnalysisCapabilityRequest&) const
      -> bool = default;
};

enum class LanguageAnalysisSupport {
  supported,
  unsupported,
  unavailable,
  unknown,
};

struct LanguageAnalysisCapability {
  LanguageAnalysisFeature feature;
  LanguageAnalysisSupport support{LanguageAnalysisSupport::unknown};
  std::string detail;
  auto operator==(const LanguageAnalysisCapability&) const -> bool = default;
};

struct LanguageAnalysisCapabilities {
  LanguageAnalysisTarget target;
  domain::KnowledgeProducer producer;
  std::vector<LanguageAnalysisCapability> capabilities;
  auto operator==(const LanguageAnalysisCapabilities&) const -> bool = default;
};

struct DocumentSymbolsQuery {
  auto operator==(const DocumentSymbolsQuery&) const -> bool = default;
};

struct SymbolReferencesQuery {
  domain::KnowledgeEntity subject;
  auto operator==(const SymbolReferencesQuery&) const -> bool = default;
};

struct SymbolRelationshipsQuery {
  domain::KnowledgeEntity subject;
  auto operator==(const SymbolRelationshipsQuery&) const -> bool = default;
};

struct SymbolSignatureQuery {
  domain::KnowledgeEntity subject;
  auto operator==(const SymbolSignatureQuery&) const -> bool = default;
};

struct DocumentDiagnosticsQuery {
  auto operator==(const DocumentDiagnosticsQuery&) const -> bool = default;
};

using LanguageAnalysisQuery =
    std::variant<DocumentSymbolsQuery, SymbolReferencesQuery,
                 SymbolRelationshipsQuery, SymbolSignatureQuery,
                 DocumentDiagnosticsQuery>;

struct LanguageAnalysisRequest {
  LanguageAnalysisTarget target;
  LanguageAnalysisQuery query;
  LanguageAnalysisLimits limits;
  auto operator==(const LanguageAnalysisRequest&) const -> bool = default;
};

enum class LanguageAnalysisStatus {
  complete,
  partial,
  unsupported,
  unavailable,
};

enum class LanguageAnalysisNoticeKind {
  ambiguous,
  incomplete,
  generated_file,
  vendor_file,
  unknown,
};

struct LanguageAnalysisNotice {
  LanguageAnalysisNoticeKind kind{LanguageAnalysisNoticeKind::unknown};
  std::optional<std::string> type_name;
  std::string message;
  std::vector<domain::KnowledgeRecordId> related_records;
  auto operator==(const LanguageAnalysisNotice&) const -> bool = default;
};

struct LanguageAnalysisResult {
  LanguageAnalysisTarget target;
  domain::KnowledgeProducer producer;
  LanguageAnalysisFeature feature;
  LanguageAnalysisStatus status{LanguageAnalysisStatus::unavailable};
  std::vector<domain::RepositoryKnowledgeRecord> records;
  std::vector<LanguageAnalysisNotice> notices;
  auto operator==(const LanguageAnalysisResult&) const -> bool = default;
};

enum class LanguageAnalysisErrorCode {
  invalid_request,
  invalid_result,
  stale_snapshot,
  build_configuration_mismatch,
  malformed_response,
  resource_exhausted,
  timed_out,
  cancelled,
  analyzer_failure,
  internal_failure,
};

struct LanguageAnalysisError {
  LanguageAnalysisErrorCode code{LanguageAnalysisErrorCode::internal_failure};
  std::string message;
  std::optional<LanguageAnalysisFeature> feature;
  std::optional<domain::KnowledgeRecordId> record_id;
  bool retryable{};
  auto operator==(const LanguageAnalysisError&) const -> bool = default;
};

class LanguageAnalysisSource {
 public:
  virtual ~LanguageAnalysisSource() = default;

  [[nodiscard]] virtual auto discover(LanguageAnalysisCapabilityRequest request,
                                      std::stop_token stop_token = {})
      -> std::expected<LanguageAnalysisCapabilities, LanguageAnalysisError> = 0;

  [[nodiscard]] virtual auto analyze(LanguageAnalysisRequest request,
                                     std::stop_token stop_token = {})
      -> std::expected<LanguageAnalysisResult, LanguageAnalysisError> = 0;
};

[[nodiscard]] auto requested_language_analysis_feature(
    const LanguageAnalysisQuery& query) noexcept -> LanguageAnalysisFeature;

[[nodiscard]] auto validate_language_analysis_request(
    const LanguageAnalysisCapabilityRequest& request)
    -> std::expected<void, LanguageAnalysisError>;

[[nodiscard]] auto validate_language_analysis_request(
    const LanguageAnalysisRequest& request)
    -> std::expected<void, LanguageAnalysisError>;

[[nodiscard]] auto validate_language_analysis_capabilities(
    const LanguageAnalysisCapabilityRequest& request,
    const LanguageAnalysisCapabilities& capabilities)
    -> std::expected<void, LanguageAnalysisError>;

[[nodiscard]] auto validate_language_analysis_result(
    const LanguageAnalysisRequest& request,
    const LanguageAnalysisResult& result)
    -> std::expected<void, LanguageAnalysisError>;

} // namespace aiforge::repository
