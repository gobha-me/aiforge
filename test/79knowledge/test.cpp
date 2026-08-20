#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <aiforge/repository/knowledge.hpp>

namespace {

namespace domain = aiforge::domain;
namespace repository = aiforge::repository;

template <typename Id>
auto id(std::string value) -> Id {
  auto parsed = Id::from(std::move(value));
  REQUIRE(parsed);
  return std::move(*parsed);
}

auto digest(std::string value = "0123456789abcdef",
            const std::uint64_t bytes = 64) -> domain::ContentDigest {
  return {"test-sha256", std::move(value), bytes};
}

auto repository_id() -> domain::RepositoryId {
  return id<domain::RepositoryId>("repository-1");
}

auto snapshot(std::string fingerprint = "aaaaaaaaaaaaaaaa")
    -> domain::RepositorySnapshotIdentity {
  return {repository_id(), digest(std::move(fingerprint), 64)};
}

auto source(std::string path = "src/main.cpp",
            std::string content = "bbbbbbbbbbbbbbbb")
    -> domain::RepositorySourceIdentity {
  return {snapshot(), std::move(path), digest(std::move(content), 32),
          domain::SourceByteRange{0, 16}};
}

auto entity(std::string name = "function-1",
            domain::KnowledgeEntityKind kind =
                domain::KnowledgeEntityKind::function,
            std::string path = "src/main.cpp") -> domain::KnowledgeEntity {
  return {id<domain::KnowledgeEntityId>(std::move(name)), kind, "Widget::run",
          source(std::move(path))};
}

auto producer(std::string version = "1") -> domain::KnowledgeProducer {
  return {"scripted-analyzer", std::move(version)};
}

auto symbol_record(std::string record_name = "symbol-record",
                   const std::uint64_t revision = 1)
    -> domain::RepositoryKnowledgeRecord {
  const auto symbol_source = source();
  return {
      id<domain::KnowledgeRecordId>(std::move(record_name)),
      revision,
      domain::SymbolKnowledge{
          entity(), "cpp", std::string{"Widget::run"},
          std::string{"auto Widget::run() -> void"}, {symbol_source},
          symbol_source},
      {snapshot(),
       {symbol_source},
       producer(),
       std::chrono::sys_time<std::chrono::milliseconds>{
           std::chrono::milliseconds{100}},
       {},
       digest("cccccccccccccccc", 24),
       domain::KnowledgeDerivation::observed,
       domain::KnowledgeConfidence::certain},
      {{domain::KnowledgeInvalidationTrigger::source_snapshot_changed,
        domain::KnowledgeInvalidationTrigger::source_digest_changed,
        domain::KnowledgeInvalidationTrigger::producer_version_changed,
        domain::KnowledgeInvalidationTrigger::build_configuration_changed}},
      domain::KnowledgeFreshness::current};
}

auto summary_record(std::string record_name = "summary-record",
                    std::string dependency_name = "symbol-record",
                    const std::uint64_t dependency_revision = 1,
                    const std::uint64_t revision = 1)
    -> domain::RepositoryKnowledgeRecord {
  const auto summary_source = source();
  return {
      id<domain::KnowledgeRecordId>(std::move(record_name)),
      revision,
      domain::SemanticSummaryKnowledge{
          entity(), "dispatch behavior",
          "Validates the request and dispatches it to the selected backend.",
          std::nullopt},
      {snapshot(),
       {summary_source},
       producer(),
       std::chrono::sys_time<std::chrono::milliseconds>{
           std::chrono::milliseconds{200}},
       {{id<domain::KnowledgeRecordId>(std::move(dependency_name)),
         dependency_revision}},
       digest("cccccccccccccccc", 24),
       domain::KnowledgeDerivation::derived,
       domain::KnowledgeConfidence::high},
      {{domain::KnowledgeInvalidationTrigger::source_snapshot_changed,
        domain::KnowledgeInvalidationTrigger::source_digest_changed,
        domain::KnowledgeInvalidationTrigger::dependency_changed,
        domain::KnowledgeInvalidationTrigger::producer_version_changed,
        domain::KnowledgeInvalidationTrigger::build_configuration_changed}},
      domain::KnowledgeFreshness::current};
}

auto graph(std::vector<domain::RepositoryKnowledgeRecord> records = {
               symbol_record(), summary_record()})
    -> domain::RepositoryKnowledgeGraph {
  return {repository_id(), records.empty() ? 0U : 1U, std::move(records)};
}

auto current_environment(const bool include_dependency = true)
    -> repository::RepositoryKnowledgeEnvironment {
  auto value = repository::RepositoryKnowledgeEnvironment{
      snapshot(),
      {{"src/main.cpp", digest("bbbbbbbbbbbbbbbb", 32),
        repository::KnowledgeInputAvailability::available}},
      true,
      {},
      true,
      producer(),
      digest("cccccccccccccccc", 24)};
  if (include_dependency) {
    value.dependencies.push_back(
        {id<domain::KnowledgeRecordId>("symbol-record"), 1,
         repository::KnowledgeInputAvailability::available});
  }
  return value;
}

auto error_code(const domain::RepositoryKnowledgeGraph& value)
    -> repository::RepositoryKnowledgeErrorCode {
  const auto result = repository::validate_repository_knowledge_graph(value);
  REQUIRE_FALSE(result);
  return result.error().code;
}

}  // namespace

TEST_CASE("knowledge graphs reject invalid limits, generations, and records",
          "[knowledge][failure]") {
  auto limits = repository::RepositoryKnowledgeLimits{};
  limits.maximum_records = 0;
  auto result = repository::validate_repository_knowledge_graph(graph(), limits);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::RepositoryKnowledgeErrorCode::invalid_limits);

  auto value = graph();
  value.generation = 0;
  REQUIRE(error_code(value) ==
          repository::RepositoryKnowledgeErrorCode::invalid_graph);

  value = graph();
  value.records.front().revision = 0;
  REQUIRE(error_code(value) ==
          repository::RepositoryKnowledgeErrorCode::invalid_provenance);

  value = graph();
  value.records.front().freshness =
      static_cast<domain::KnowledgeFreshness>(99);
  REQUIRE(error_code(value) ==
          repository::RepositoryKnowledgeErrorCode::invalid_freshness);

  value = graph();
  std::get<domain::SymbolKnowledge>(value.records.front().payload).language =
      std::string{"bad\0language", 12};
  REQUIRE(error_code(value) ==
          repository::RepositoryKnowledgeErrorCode::invalid_record);

  value = graph();
  std::get<domain::SemanticSummaryKnowledge>(value.records.back().payload)
      .summary = std::string(1024U * 1024U + 1U, 'x');
  REQUIRE(error_code(value) ==
          repository::RepositoryKnowledgeErrorCode::invalid_record);
}

TEST_CASE("knowledge provenance and invalidation baselines fail closed",
          "[knowledge][provenance][failure]") {
  auto value = graph();
  value.records.front().provenance.sources.front().relative_path = "../escape.cpp";
  REQUIRE(error_code(value) ==
          repository::RepositoryKnowledgeErrorCode::invalid_source);

  value = graph();
  value.records.front().provenance.sources.clear();
  REQUIRE(error_code(value) ==
          repository::RepositoryKnowledgeErrorCode::invalid_invalidation_rule);

  value = graph();
  value.records.back().provenance.derivation_inputs.clear();
  REQUIRE(error_code(value) ==
          repository::RepositoryKnowledgeErrorCode::invalid_provenance);

  value = graph();
  value.records.front().payload = domain::DiagnosticKnowledge{
      entity(), static_cast<domain::KnowledgeDiagnosticSeverity>(99), "",
      "bad severity", source()};
  REQUIRE(error_code(value) ==
          repository::RepositoryKnowledgeErrorCode::invalid_record);

  value = graph();
  value.records.front().invalidation.triggers.push_back(
      domain::KnowledgeInvalidationTrigger::source_snapshot_changed);
  REQUIRE(error_code(value) ==
          repository::RepositoryKnowledgeErrorCode::invalid_invalidation_rule);

  value = graph();
  auto& definition = *std::get<domain::SymbolKnowledge>(
                           value.records.front().payload)
                           .definition;
  definition.content_digest = digest("dddddddddddddddd", 32);
  REQUIRE(error_code(value) ==
          repository::RepositoryKnowledgeErrorCode::invalid_provenance);
}

TEST_CASE("record and entity identities cannot conflict",
          "[knowledge][identity][failure]") {
  auto duplicate = symbol_record();
  REQUIRE(error_code(graph({symbol_record(), duplicate})) ==
          repository::RepositoryKnowledgeErrorCode::duplicate_record);

  auto conflicting = summary_record("other-summary");
  std::get<domain::SemanticSummaryKnowledge>(conflicting.payload)
      .subject.display_name = "Different symbol";
  REQUIRE(error_code(graph({symbol_record(), conflicting})) ==
          repository::RepositoryKnowledgeErrorCode::conflicting_entity);
}

TEST_CASE("derivation cycles fail while semantic relationship cycles are valid",
          "[knowledge][graph][failure]") {
  auto first = summary_record("first", "second");
  auto second = summary_record("second", "first");
  REQUIRE(error_code(graph({first, second})) ==
          repository::RepositoryKnowledgeErrorCode::cyclic_dependency);

  const auto first_source = source("src/first.cpp", "1111111111111111");
  const auto second_source = source("src/second.cpp", "2222222222222222");
  const auto first_entity = domain::KnowledgeEntity{
      id<domain::KnowledgeEntityId>("first-function"),
      domain::KnowledgeEntityKind::function, "first", first_source};
  const auto second_entity = domain::KnowledgeEntity{
      id<domain::KnowledgeEntityId>("second-function"),
      domain::KnowledgeEntityKind::function, "second", second_source};
  const auto relationship = [&](std::string name,
                                domain::KnowledgeEntity from,
                                domain::KnowledgeEntity to) {
    return domain::RepositoryKnowledgeRecord{
        id<domain::KnowledgeRecordId>(std::move(name)),
        1,
        domain::RelationshipKnowledge{std::move(from),
                                      domain::KnowledgeRelationshipKind::calls,
                                      std::move(to), std::nullopt},
        {snapshot(),
         {first_source, second_source},
         producer(),
         std::chrono::sys_time<std::chrono::milliseconds>{
             std::chrono::milliseconds{100}},
         {},
         std::nullopt,
         domain::KnowledgeDerivation::observed,
         domain::KnowledgeConfidence::certain},
        {{domain::KnowledgeInvalidationTrigger::source_snapshot_changed,
          domain::KnowledgeInvalidationTrigger::source_digest_changed}},
        domain::KnowledgeFreshness::current};
  };
  const auto result = repository::validate_repository_knowledge_graph(graph(
      {relationship("first-calls-second", first_entity, second_entity),
       relationship("second-calls-first", second_entity, first_entity)}));
  REQUIRE(result);
  REQUIRE(result->relationship_count == 2);
  REQUIRE(result->entity_count == 2);
}

TEST_CASE("missing dependencies require explicit unavailable freshness",
          "[knowledge][dependency][failure]") {
  auto missing = summary_record();
  REQUIRE(error_code(graph({missing})) ==
          repository::RepositoryKnowledgeErrorCode::missing_dependency);

  missing.freshness = domain::KnowledgeFreshness::unavailable;
  REQUIRE(repository::validate_repository_knowledge_graph(graph({missing})));

  auto stale_revision = summary_record();
  stale_revision.provenance.derivation_inputs.front().revision = 2;
  REQUIRE(error_code(graph({symbol_record(), stale_revision})) ==
          repository::RepositoryKnowledgeErrorCode::invalid_freshness);
  stale_revision.freshness = domain::KnowledgeFreshness::stale;
  REQUIRE(repository::validate_repository_knowledge_graph(
      graph({symbol_record(), stale_revision})));
}

TEST_CASE("freshness assessment distinguishes current, uncertain, stale, and unavailable",
          "[knowledge][freshness]") {
  const auto record = summary_record();
  auto environment = current_environment();
  auto result = repository::assess_repository_knowledge_freshness(
      record, environment);
  REQUIRE(result);
  REQUIRE(result->freshness == domain::KnowledgeFreshness::current);
  REQUIRE(result->affected_triggers.empty());

  environment.sources.clear();
  environment.source_observation_complete = false;
  result = repository::assess_repository_knowledge_freshness(record,
                                                              environment);
  REQUIRE(result);
  REQUIRE(result->freshness == domain::KnowledgeFreshness::possibly_stale);

  environment = current_environment();
  environment.sources.front().content_digest =
      digest("dddddddddddddddd", 32);
  result = repository::assess_repository_knowledge_freshness(record,
                                                              environment);
  REQUIRE(result);
  REQUIRE(result->freshness == domain::KnowledgeFreshness::stale);

  environment = current_environment();
  environment.dependencies.front().availability =
      repository::KnowledgeInputAvailability::unavailable;
  environment.dependencies.front().revision.reset();
  result = repository::assess_repository_knowledge_freshness(record,
                                                              environment);
  REQUIRE(result);
  REQUIRE(result->freshness == domain::KnowledgeFreshness::unavailable);

  environment = current_environment();
  environment.producer = producer("2");
  environment.build_configuration = digest("eeeeeeeeeeeeeeee", 24);
  result = repository::assess_repository_knowledge_freshness(record,
                                                              environment);
  REQUIRE(result);
  REQUIRE(result->freshness == domain::KnowledgeFreshness::stale);
  REQUIRE(std::ranges::find(
              result->affected_triggers,
              domain::KnowledgeInvalidationTrigger::producer_version_changed) !=
          result->affected_triggers.end());
  REQUIRE(std::ranges::find(
              result->affected_triggers,
              domain::KnowledgeInvalidationTrigger::build_configuration_changed) !=
          result->affected_triggers.end());
}

TEST_CASE("freshness observations reject duplicates and malformed availability",
          "[knowledge][freshness][failure]") {
  auto environment = current_environment();
  environment.sources.push_back(environment.sources.front());
  auto result = repository::assess_repository_knowledge_freshness(
      summary_record(), environment);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::RepositoryKnowledgeErrorCode::invalid_source);

  environment = current_environment();
  environment.dependencies.front().availability =
      repository::KnowledgeInputAvailability::unavailable;
  result = repository::assess_repository_knowledge_freshness(summary_record(),
                                                              environment);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::RepositoryKnowledgeErrorCode::invalid_provenance);

  environment = current_environment();
  environment.sources.front().availability =
      static_cast<repository::KnowledgeInputAvailability>(99);
  environment.sources.front().content_digest.reset();
  result = repository::assess_repository_knowledge_freshness(summary_record(),
                                                              environment);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::RepositoryKnowledgeErrorCode::invalid_source);
}

TEST_CASE("atomic updates reject races and invalidate dependent records",
          "[knowledge][update]") {
  const auto original = graph();
  auto replacement = symbol_record("symbol-record", 2);
  std::get<domain::SymbolKnowledge>(replacement.payload).signature =
      "auto Widget::run(int attempts) -> void";
  auto update = repository::RepositoryKnowledgeUpdate{
      1, {{1, replacement}}, {}};
  auto result = repository::apply_repository_knowledge_update(original, update);
  REQUIRE(result);
  REQUIRE(result->generation == 2);
  REQUIRE(result->records.front().record_id.value() == "summary-record");
  REQUIRE(result->records.front().revision == 2);
  REQUIRE(result->records.front().freshness ==
          domain::KnowledgeFreshness::stale);

  const auto raced =
      repository::apply_repository_knowledge_update(*result, update);
  REQUIRE_FALSE(raced);
  REQUIRE(raced.error().code ==
          repository::RepositoryKnowledgeErrorCode::generation_conflict);
  REQUIRE(original == graph());

  const auto removed = repository::apply_repository_knowledge_update(
      *result,
      repository::RepositoryKnowledgeUpdate{
          2, {}, {id<domain::KnowledgeRecordId>("symbol-record")}});
  REQUIRE(removed);
  REQUIRE(removed->records.size() == 1);
  REQUIRE(removed->records.front().revision == 3);
  REQUIRE(removed->records.front().freshness ==
          domain::KnowledgeFreshness::unavailable);

  const auto emptied = repository::apply_repository_knowledge_update(
      graph({symbol_record()}),
      repository::RepositoryKnowledgeUpdate{
          1, {}, {id<domain::KnowledgeRecordId>("symbol-record")}});
  REQUIRE(emptied);
  REQUIRE(emptied->generation == 2);
  REQUIRE(emptied->records.empty());
}

TEST_CASE("a batch can rebuild a record and all of its dependents atomically",
          "[knowledge][update][rebuild]") {
  auto replacement = symbol_record("symbol-record", 2);
  auto rebuilt = summary_record("summary-record", "symbol-record", 2, 2);
  const auto result = repository::apply_repository_knowledge_update(
      graph(), repository::RepositoryKnowledgeUpdate{
                   1, {{1, replacement}, {1, rebuilt}}, {}});
  REQUIRE(result);
  REQUIRE(result->generation == 2);
  REQUIRE(std::ranges::all_of(result->records, [](const auto& record) {
    return record.revision == 2 &&
           record.freshness == domain::KnowledgeFreshness::current;
  }));
}

TEST_CASE("source-changing replacements retain stale derived interpretations",
          "[knowledge][update][freshness]") {
  auto replacement = symbol_record("symbol-record", 2);
  const auto next_snapshot = snapshot("dddddddddddddddd");
  auto next_source = source("src/main.cpp", "eeeeeeeeeeeeeeee");
  next_source.snapshot = next_snapshot;
  auto& symbol = std::get<domain::SymbolKnowledge>(replacement.payload);
  symbol.symbol.source = next_source;
  symbol.declarations = {next_source};
  symbol.definition = next_source;
  replacement.provenance.source_snapshot = next_snapshot;
  replacement.provenance.sources = {next_source};

  const auto result = repository::apply_repository_knowledge_update(
      graph(), repository::RepositoryKnowledgeUpdate{
                   1, {{1, replacement}}, {}});
  REQUIRE(result);
  const auto summary = std::ranges::find(
      result->records, id<domain::KnowledgeRecordId>("summary-record"),
      &domain::RepositoryKnowledgeRecord::record_id);
  REQUIRE(summary != result->records.end());
  REQUIRE(summary->freshness == domain::KnowledgeFreshness::stale);
  REQUIRE(repository::validate_repository_knowledge_graph(*result));
}

TEST_CASE("failed updates are atomic and revisions are exact",
          "[knowledge][update][failure]") {
  const auto original = graph();
  auto invalid = symbol_record("symbol-record", 3);
  auto result = repository::apply_repository_knowledge_update(
      original,
      repository::RepositoryKnowledgeUpdate{1, {{1, invalid}}, {}});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::RepositoryKnowledgeErrorCode::revision_conflict);
  REQUIRE(original == graph());

  auto inserted = symbol_record("new-symbol", 2);
  result = repository::apply_repository_knowledge_update(
      original,
      repository::RepositoryKnowledgeUpdate{1, {{std::nullopt, inserted}}, {}});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::RepositoryKnowledgeErrorCode::revision_conflict);

  result = repository::apply_repository_knowledge_update(
      original, repository::RepositoryKnowledgeUpdate{1, {}, {}});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::RepositoryKnowledgeErrorCode::invalid_graph);
}

TEST_CASE("unknown future records remain opaque and bounded",
          "[knowledge][unknown]") {
  auto unknown = domain::RepositoryKnowledgeRecord{
      id<domain::KnowledgeRecordId>("future-record"),
      1,
      domain::UnknownRepositoryKnowledge{
          {id<domain::KnowledgeEntityId>("future-entity"),
           domain::KnowledgeEntityKind::unknown, "future entity", std::nullopt},
          "future.semantic-record", std::nullopt},
      {snapshot(),
       {},
       producer(),
       std::chrono::sys_time<std::chrono::milliseconds>{
           std::chrono::milliseconds{300}},
       {},
       std::nullopt,
       domain::KnowledgeDerivation::unknown,
       domain::KnowledgeConfidence::unknown},
      {{domain::KnowledgeInvalidationTrigger::source_snapshot_changed}},
      domain::KnowledgeFreshness::current};
  auto result = repository::validate_repository_knowledge_graph(graph({unknown}));
  REQUIRE(result);
  REQUIRE(result->record_count == 1);

  std::get<domain::UnknownRepositoryKnowledge>(unknown.payload)
      .type_name.clear();
  REQUIRE(error_code(graph({unknown})) ==
          repository::RepositoryKnowledgeErrorCode::invalid_record);
}

TEST_CASE("mixed knowledge reports deterministic bounded estimates",
          "[knowledge][smoke]") {
  auto diagnostic = symbol_record("diagnostic-record");
  diagnostic.payload = domain::DiagnosticKnowledge{
      entity(), domain::KnowledgeDiagnosticSeverity::warning, "W001",
      "The selected symbol is deprecated.", source()};
  diagnostic.provenance.derivation = domain::KnowledgeDerivation::observed;
  diagnostic.provenance.derivation_inputs.clear();

  const auto result = repository::validate_repository_knowledge_graph(
      graph({symbol_record(), summary_record(), diagnostic}));
  REQUIRE(result);
  REQUIRE(result->record_count == 3);
  REQUIRE(result->entity_count == 1);
  REQUIRE(result->relationship_count == 0);
  REQUIRE(result->inline_bytes > 0);
}
