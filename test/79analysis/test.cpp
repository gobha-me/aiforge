#include <aiforge/repository/language_analysis.hpp>
#include <aiforge/testing/scripted_language_analysis_source.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace aiforge;

template <typename Id> auto id(std::string value) -> Id {
  auto parsed = Id::from(std::move(value));
  REQUIRE(parsed);
  return std::move(*parsed);
}

auto digest(std::string value = "0123456789abcdef", std::uint64_t bytes = 64)
    -> domain::ContentDigest {
  return {"test-sha256", std::move(value), bytes};
}

auto snapshot(std::string value = "aaaaaaaaaaaaaaaa")
    -> domain::RepositorySnapshotIdentity {
  return {id<domain::RepositoryId>("repository-1"), digest(std::move(value))};
}

auto source(std::string path = "src/widget.cpp",
            std::string value = "bbbbbbbbbbbbbbbb",
            std::optional<domain::SourceByteRange> range = std::nullopt)
    -> domain::RepositorySourceIdentity {
  return {snapshot(), std::move(path), digest(std::move(value)), range};
}

auto producer(std::string version = "1") -> domain::KnowledgeProducer {
  return {"scripted-cpp-analyzer", std::move(version)};
}

auto target(repository::LanguageAnalysisFileKind file_kind =
                repository::LanguageAnalysisFileKind::source)
    -> repository::LanguageAnalysisTarget {
  return {source(), "c++", file_kind, digest("cccccccccccccccc", 24)};
}

auto capability_request() -> repository::LanguageAnalysisCapabilityRequest {
  return {target(), {}};
}

auto feature(const repository::LanguageAnalysisFeatureKind kind)
    -> repository::LanguageAnalysisFeature {
  return {kind, std::nullopt};
}

auto capabilities(const repository::LanguageAnalysisTarget& value = target())
    -> repository::LanguageAnalysisCapabilities {
  using Feature = repository::LanguageAnalysisFeatureKind;
  using Support = repository::LanguageAnalysisSupport;
  return {
      value,
      producer(),
      {{feature(Feature::symbols), Support::supported, ""},
       {feature(Feature::references), Support::supported, ""},
       {feature(Feature::relationships), Support::supported, ""},
       {feature(Feature::signatures), Support::supported, ""},
       {feature(Feature::diagnostics), Support::supported, ""}},
  };
}

auto entity(std::string name = "Widget::run",
            std::optional<domain::RepositorySourceIdentity> location = source(
                "src/widget.cpp", "bbbbbbbbbbbbbbbb",
                domain::SourceByteRange{0, 8})) -> domain::KnowledgeEntity {
  return {id<domain::KnowledgeEntityId>(name),
          domain::KnowledgeEntityKind::function, std::move(name),
          std::move(location)};
}

auto analysis_request(repository::LanguageAnalysisQuery query)
    -> repository::LanguageAnalysisRequest {
  return {target(), std::move(query), {}};
}

auto record_provenance(std::vector<domain::RepositorySourceIdentity> sources =
                           {source("src/widget.cpp", "bbbbbbbbbbbbbbbb",
                                   domain::SourceByteRange{0, 8})})
    -> domain::RepositoryKnowledgeProvenance {
  return {snapshot(),
          std::move(sources),
          producer(),
          std::chrono::sys_time<std::chrono::milliseconds>{
              std::chrono::milliseconds{100}},
          {},
          digest("cccccccccccccccc", 24),
          domain::KnowledgeDerivation::observed,
          domain::KnowledgeConfidence::high};
}

auto invalidation() -> domain::KnowledgeInvalidationRule {
  return {{domain::KnowledgeInvalidationTrigger::source_snapshot_changed,
           domain::KnowledgeInvalidationTrigger::source_digest_changed,
           domain::KnowledgeInvalidationTrigger::producer_version_changed,
           domain::KnowledgeInvalidationTrigger::build_configuration_changed}};
}

auto symbol_record(
    std::string record_name = "symbol-widget-run",
    std::string entity_name = "Widget::run",
    std::optional<std::string> signature = "auto Widget::run() -> void")
    -> domain::RepositoryKnowledgeRecord {
  const auto location = source("src/widget.cpp", "bbbbbbbbbbbbbbbb",
                               domain::SourceByteRange{0, 8});
  return {id<domain::KnowledgeRecordId>(std::move(record_name)),
          1,
          domain::SymbolKnowledge{entity(std::move(entity_name), location),
                                  "c++",
                                  "Widget::run",
                                  std::move(signature),
                                  {location},
                                  location},
          record_provenance({location}),
          invalidation(),
          domain::KnowledgeFreshness::current};
}

auto relationship_record(std::string record_name = "reference-widget-run",
                         domain::KnowledgeRelationshipKind kind =
                             domain::KnowledgeRelationshipKind::references)
    -> domain::RepositoryKnowledgeRecord {
  const auto first = source("src/widget.cpp", "bbbbbbbbbbbbbbbb",
                            domain::SourceByteRange{0, 8});
  const auto second = source("src/main.cpp", "dddddddddddddddd",
                             domain::SourceByteRange{10, 18});
  return {id<domain::KnowledgeRecordId>(std::move(record_name)),
          1,
          domain::RelationshipKnowledge{entity("main-call", second), kind,
                                        entity("Widget::run", first), second},
          record_provenance({first, second}),
          invalidation(),
          domain::KnowledgeFreshness::current};
}

auto diagnostic_record() -> domain::RepositoryKnowledgeRecord {
  const auto location = source("src/widget.cpp", "bbbbbbbbbbbbbbbb",
                               domain::SourceByteRange{4, 8});
  return {id<domain::KnowledgeRecordId>("diagnostic-widget-run"),
          1,
          domain::DiagnosticKnowledge{
              entity("Widget::run", location),
              domain::KnowledgeDiagnosticSeverity::warning, "W001",
              "Widget::run is deprecated.", location},
          record_provenance({location}),
          invalidation(),
          domain::KnowledgeFreshness::current};
}

auto result(const repository::LanguageAnalysisRequest& request,
            std::vector<domain::RepositoryKnowledgeRecord> records = {})
    -> repository::LanguageAnalysisResult {
  return {request.target,
          producer(),
          repository::requested_language_analysis_feature(request.query),
          repository::LanguageAnalysisStatus::complete,
          std::move(records),
          {}};
}

auto analysis_error(const repository::LanguageAnalysisErrorCode code)
    -> repository::LanguageAnalysisError {
  return {code, "scripted failure", std::nullopt, std::nullopt,
          code == repository::LanguageAnalysisErrorCode::timed_out};
}

} // namespace

TEST_CASE(
    "language-analysis requests fail closed on malformed targets and limits",
    "[analysis][request][failure]") {
  auto request = capability_request();
  request.limits.timeout = std::chrono::milliseconds{0};
  REQUIRE_FALSE(repository::validate_language_analysis_request(request));

  request = capability_request();
  request.limits.maximum_capabilities = 4;
  REQUIRE_FALSE(repository::validate_language_analysis_request(request));

  request = capability_request();
  request.limits.maximum_notice_bytes = 0;
  REQUIRE_FALSE(repository::validate_language_analysis_request(request));

  request = capability_request();
  request.target.source.range = domain::SourceByteRange{0, 1};
  REQUIRE_FALSE(repository::validate_language_analysis_request(request));

  request = capability_request();
  request.target.language = std::string{"c++\0hidden", 10};
  REQUIRE_FALSE(repository::validate_language_analysis_request(request));

  request = capability_request();
  request.target.file_kind =
      static_cast<repository::LanguageAnalysisFileKind>(99);
  REQUIRE_FALSE(repository::validate_language_analysis_request(request));

  request = capability_request();
  request.target.source.relative_path = "../widget.cpp";
  REQUIRE_FALSE(repository::validate_language_analysis_request(request));

  request = capability_request();
  request.target.build_configuration->algorithm = "bad algorithm";
  REQUIRE_FALSE(repository::validate_language_analysis_request(request));
}

TEST_CASE("symbol-focused requests require a valid same-snapshot subject",
          "[analysis][request][failure]") {
  auto bad_kind = entity();
  bad_kind.kind = domain::KnowledgeEntityKind::repository;
  auto request = analysis_request(repository::SymbolReferencesQuery{bad_kind});
  REQUIRE_FALSE(repository::validate_language_analysis_request(request));

  auto stale = entity();
  stale.source->snapshot = snapshot("eeeeeeeeeeeeeeee");
  request = analysis_request(repository::SymbolSignatureQuery{stale});
  const auto stale_result =
      repository::validate_language_analysis_request(request);
  REQUIRE_FALSE(stale_result);
  REQUIRE(stale_result.error().code ==
          repository::LanguageAnalysisErrorCode::invalid_request);

  request = analysis_request(
      repository::SymbolRelationshipsQuery{entity("external", std::nullopt)});
  REQUIRE(repository::validate_language_analysis_request(request));
}

TEST_CASE("capability discovery is complete ordered and forward compatible",
          "[analysis][capabilities]") {
  const auto request = capability_request();
  auto response = capabilities();
  response.capabilities.push_back(
      {{repository::LanguageAnalysisFeatureKind::unknown,
        "vendor.rename-safe-locations"},
       repository::LanguageAnalysisSupport::supported,
       ""});
  REQUIRE(
      repository::validate_language_analysis_capabilities(request, response));

  auto missing = capabilities();
  missing.capabilities.pop_back();
  REQUIRE_FALSE(
      repository::validate_language_analysis_capabilities(request, missing));

  auto unordered = capabilities();
  std::swap(unordered.capabilities[0], unordered.capabilities[1]);
  REQUIRE_FALSE(
      repository::validate_language_analysis_capabilities(request, unordered));

  auto unexplained = capabilities();
  unexplained.capabilities.front().support =
      repository::LanguageAnalysisSupport::unsupported;
  REQUIRE_FALSE(repository::validate_language_analysis_capabilities(
      request, unexplained));
  unexplained.capabilities.front().detail = "language is not installed";
  REQUIRE(repository::validate_language_analysis_capabilities(request,
                                                              unexplained));
}

TEST_CASE("capability results retain exact source and build configuration",
          "[analysis][capabilities][failure]") {
  const auto request = capability_request();
  auto response = capabilities();
  response.target.source.content_digest = digest("eeeeeeeeeeeeeeee");
  auto validated =
      repository::validate_language_analysis_capabilities(request, response);
  REQUIRE_FALSE(validated);
  REQUIRE(validated.error().code ==
          repository::LanguageAnalysisErrorCode::stale_snapshot);

  response = capabilities();
  response.target.build_configuration = digest("ffffffffffffffff", 24);
  validated =
      repository::validate_language_analysis_capabilities(request, response);
  REQUIRE_FALSE(validated);
  REQUIRE(validated.error().code ==
          repository::LanguageAnalysisErrorCode::build_configuration_mismatch);

  response = capabilities();
  response.producer.name.clear();
  REQUIRE_FALSE(
      repository::validate_language_analysis_capabilities(request, response));

  response = capabilities();
  response.capabilities.front().feature = {
      repository::LanguageAnalysisFeatureKind::unknown, std::nullopt};
  REQUIRE_FALSE(
      repository::validate_language_analysis_capabilities(request, response));

  response = capabilities();
  response.capabilities.front().support =
      static_cast<repository::LanguageAnalysisSupport>(99);
  REQUIRE_FALSE(
      repository::validate_language_analysis_capabilities(request, response));
}

TEST_CASE(
    "analysis status represents absence and partial results without errors",
    "[analysis][availability]") {
  const auto request = analysis_request(repository::DocumentSymbolsQuery{});
  auto response = result(request);
  response.status = repository::LanguageAnalysisStatus::unsupported;
  REQUIRE(repository::validate_language_analysis_result(request, response));

  response.records = {symbol_record()};
  REQUIRE_FALSE(
      repository::validate_language_analysis_result(request, response));

  response = result(request);
  response.status = repository::LanguageAnalysisStatus::unavailable;
  response.notices = {{repository::LanguageAnalysisNoticeKind::incomplete,
                       std::nullopt,
                       "analyzer is temporarily unavailable",
                       {}}};
  REQUIRE(repository::validate_language_analysis_result(request, response));

  response = result(request, {symbol_record()});
  response.status = repository::LanguageAnalysisStatus::partial;
  REQUIRE_FALSE(
      repository::validate_language_analysis_result(request, response));
  response.notices = {{repository::LanguageAnalysisNoticeKind::incomplete,
                       std::nullopt,
                       "one translation unit timed out",
                       {}}};
  REQUIRE(repository::validate_language_analysis_result(request, response));

  response.status = static_cast<repository::LanguageAnalysisStatus>(99);
  REQUIRE_FALSE(
      repository::validate_language_analysis_result(request, response));
}

TEST_CASE("each known query admits only its neutral knowledge payload",
          "[analysis][payload]") {
  auto request = analysis_request(repository::DocumentSymbolsQuery{});
  REQUIRE(repository::validate_language_analysis_result(
      request, result(request, {symbol_record()})));

  request = analysis_request(repository::SymbolSignatureQuery{entity()});
  REQUIRE(repository::validate_language_analysis_result(
      request, result(request, {symbol_record()})));

  auto missing_signature = symbol_record();
  std::get<domain::SymbolKnowledge>(missing_signature.payload)
      .signature.reset();
  REQUIRE_FALSE(repository::validate_language_analysis_result(
      request, result(request, {missing_signature})));

  request = analysis_request(repository::SymbolReferencesQuery{entity()});
  REQUIRE(repository::validate_language_analysis_result(
      request, result(request, {relationship_record()})));
  REQUIRE_FALSE(repository::validate_language_analysis_result(
      request,
      result(request,
             {relationship_record("call-widget-run",
                                  domain::KnowledgeRelationshipKind::calls)})));

  request = analysis_request(repository::SymbolRelationshipsQuery{entity()});
  REQUIRE(repository::validate_language_analysis_result(
      request,
      result(request,
             {relationship_record("call-widget-run",
                                  domain::KnowledgeRelationshipKind::calls)})));

  request = analysis_request(repository::DocumentDiagnosticsQuery{});
  REQUIRE(repository::validate_language_analysis_result(
      request, result(request, {diagnostic_record()})));
}

TEST_CASE("analysis records cannot drift from request provenance",
          "[analysis][provenance][failure]") {
  const auto request = analysis_request(repository::DocumentSymbolsQuery{});
  auto response = result(request, {symbol_record()});
  response.target.source.snapshot = snapshot("eeeeeeeeeeeeeeee");
  auto validated =
      repository::validate_language_analysis_result(request, response);
  REQUIRE_FALSE(validated);
  REQUIRE(validated.error().code ==
          repository::LanguageAnalysisErrorCode::stale_snapshot);

  response = result(request, {symbol_record()});
  response.records.front().provenance.source_snapshot =
      snapshot("eeeeeeeeeeeeeeee");
  validated = repository::validate_language_analysis_result(request, response);
  REQUIRE_FALSE(validated);
  REQUIRE(validated.error().code ==
          repository::LanguageAnalysisErrorCode::stale_snapshot);

  response = result(request, {symbol_record()});
  response.records.front().provenance.build_configuration =
      digest("ffffffffffffffff", 24);
  validated = repository::validate_language_analysis_result(request, response);
  REQUIRE_FALSE(validated);
  REQUIRE(validated.error().code ==
          repository::LanguageAnalysisErrorCode::build_configuration_mismatch);

  response = result(request, {symbol_record()});
  response.records.front().provenance.producer = producer("2");
  validated = repository::validate_language_analysis_result(request, response);
  REQUIRE_FALSE(validated);
  REQUIRE(validated.error().code ==
          repository::LanguageAnalysisErrorCode::malformed_response);
}

TEST_CASE("malformed and excessive analyzer output is bounded",
          "[analysis][bounds][failure]") {
  auto request = analysis_request(repository::DocumentSymbolsQuery{});
  auto response = result(request, {symbol_record(), symbol_record()});
  auto validated =
      repository::validate_language_analysis_result(request, response);
  REQUIRE_FALSE(validated);
  REQUIRE(validated.error().code ==
          repository::LanguageAnalysisErrorCode::malformed_response);

  response =
      result(request, {symbol_record(),
                       symbol_record("symbol-widget-stop", "Widget::stop")});
  request.limits.knowledge.maximum_records = 1;
  validated = repository::validate_language_analysis_result(request, response);
  REQUIRE_FALSE(validated);
  REQUIRE(validated.error().code ==
          repository::LanguageAnalysisErrorCode::resource_exhausted);

  request = analysis_request(repository::DocumentSymbolsQuery{});
  response = result(request);
  response.notices = {{repository::LanguageAnalysisNoticeKind::unknown,
                       std::nullopt,
                       "future analyzer notice",
                       {}}};
  REQUIRE_FALSE(
      repository::validate_language_analysis_result(request, response));
  response.notices.front().type_name = "vendor.future-notice";
  REQUIRE(repository::validate_language_analysis_result(request, response));

  response.notices.front().kind =
      static_cast<repository::LanguageAnalysisNoticeKind>(99);
  REQUIRE_FALSE(
      repository::validate_language_analysis_result(request, response));

  response.notices.front().kind =
      repository::LanguageAnalysisNoticeKind::unknown;
  response.notices.front().message = std::string(4097, 'x');
  REQUIRE_FALSE(
      repository::validate_language_analysis_result(request, response));
}

TEST_CASE("ambiguous symbols remain explicit instead of selecting a winner",
          "[analysis][ambiguity]") {
  const auto request = analysis_request(repository::DocumentSymbolsQuery{});
  auto first = symbol_record();
  auto second =
      symbol_record("symbol-widget-run-overload", "Widget::run-overload");
  auto response = result(request, {first, second});
  response.notices = {{repository::LanguageAnalysisNoticeKind::ambiguous,
                       std::nullopt,
                       "multiple symbols match Widget::run",
                       {first.record_id, second.record_id}}};
  REQUIRE(repository::validate_language_analysis_result(request, response));

  response.notices.front().related_records.pop_back();
  REQUIRE_FALSE(
      repository::validate_language_analysis_result(request, response));

  response.notices.front().related_records = {first.record_id, first.record_id};
  REQUIRE_FALSE(
      repository::validate_language_analysis_result(request, response));
}

TEST_CASE("generated vendor and unknown files remain explicit valid targets",
          "[analysis][files]") {
  for (const auto& [kind, notice] :
       {std::pair{repository::LanguageAnalysisFileKind::generated,
                  repository::LanguageAnalysisNoticeKind::generated_file},
        std::pair{repository::LanguageAnalysisFileKind::vendor,
                  repository::LanguageAnalysisNoticeKind::vendor_file},
        std::pair{repository::LanguageAnalysisFileKind::unknown,
                  repository::LanguageAnalysisNoticeKind::incomplete}}) {
    auto request = analysis_request(repository::DocumentDiagnosticsQuery{});
    request.target.file_kind = kind;
    auto response = result(request);
    response.notices = {
        {notice, std::nullopt, "file classification retained by analyzer", {}}};
    REQUIRE(repository::validate_language_analysis_result(request, response));
  }
}

TEST_CASE("scripted analyzer records exact discovery and analysis exchanges",
          "[analysis][fake]") {
  const auto discovery = capability_request();
  const auto request = analysis_request(repository::DocumentSymbolsQuery{});
  const auto discovered = capabilities();
  const auto analyzed = result(request, {symbol_record()});
  testing::ScriptedLanguageAnalysisSource fake({{discovery, discovered}},
                                               {{request, analyzed}});

  const auto discovery_result = fake.discover(discovery);
  REQUIRE(discovery_result == discovered);
  const auto analysis_result = fake.analyze(request);
  REQUIRE(analysis_result == analyzed);
  REQUIRE(fake.recorded_capability_requests() == std::vector{discovery});
  REQUIRE(fake.recorded_analysis_requests() == std::vector{request});
  REQUIRE(fake.remaining_capability_exchanges() == 0);
  REQUIRE(fake.remaining_analysis_exchanges() == 0);
}

TEST_CASE("scripted analyzer exposes timeout crash mismatch exhaustion and "
          "cancellation",
          "[analysis][fake][failure]") {
  const auto discovery = capability_request();
  const auto request = analysis_request(repository::DocumentSymbolsQuery{});
  testing::ScriptedLanguageAnalysisSource failures(
      {{discovery,
        analysis_error(repository::LanguageAnalysisErrorCode::timed_out)}},
      {{request,
        analysis_error(
            repository::LanguageAnalysisErrorCode::analyzer_failure)}});
  auto discovered = failures.discover(discovery);
  REQUIRE_FALSE(discovered);
  REQUIRE(discovered.error().code ==
          repository::LanguageAnalysisErrorCode::timed_out);
  auto analyzed = failures.analyze(request);
  REQUIRE_FALSE(analyzed);
  REQUIRE(analyzed.error().code ==
          repository::LanguageAnalysisErrorCode::analyzer_failure);

  discovered = failures.discover(discovery);
  REQUIRE_FALSE(discovered);
  REQUIRE(discovered.error().code ==
          repository::LanguageAnalysisErrorCode::internal_failure);

  testing::ScriptedLanguageAnalysisSource mismatch(
      {{discovery, capabilities()}}, {});
  auto changed = discovery;
  changed.target.language = "c";
  discovered = mismatch.discover(changed);
  REQUIRE_FALSE(discovered);
  REQUIRE(mismatch.remaining_capability_exchanges() == 1);

  testing::ScriptedLanguageAnalysisSource cancelled(
      {{discovery, capabilities()}}, {{request, result(request)}});
  std::stop_source stop;
  stop.request_stop();
  discovered = cancelled.discover(discovery, stop.get_token());
  REQUIRE_FALSE(discovered);
  REQUIRE(discovered.error().code ==
          repository::LanguageAnalysisErrorCode::cancelled);
  analyzed = cancelled.analyze(request, stop.get_token());
  REQUIRE_FALSE(analyzed);
  REQUIRE(analyzed.error().code ==
          repository::LanguageAnalysisErrorCode::cancelled);
  REQUIRE(cancelled.recorded_capability_requests().empty());
  REQUIRE(cancelled.recorded_analysis_requests().empty());
}

TEST_CASE("analysis feature mapping is stable", "[analysis][smoke]") {
  REQUIRE(repository::requested_language_analysis_feature(
              repository::DocumentSymbolsQuery{}) ==
          feature(repository::LanguageAnalysisFeatureKind::symbols));
  REQUIRE(repository::requested_language_analysis_feature(
              repository::SymbolReferencesQuery{entity()}) ==
          feature(repository::LanguageAnalysisFeatureKind::references));
  REQUIRE(repository::requested_language_analysis_feature(
              repository::SymbolRelationshipsQuery{entity()}) ==
          feature(repository::LanguageAnalysisFeatureKind::relationships));
  REQUIRE(repository::requested_language_analysis_feature(
              repository::SymbolSignatureQuery{entity()}) ==
          feature(repository::LanguageAnalysisFeatureKind::signatures));
  REQUIRE(repository::requested_language_analysis_feature(
              repository::DocumentDiagnosticsQuery{}) ==
          feature(repository::LanguageAnalysisFeatureKind::diagnostics));
}
