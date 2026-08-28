#include <aiforge/repository/context_parcel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
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

auto digest(std::string value = "0123456789abcdef", std::uint64_t bytes = 16)
    -> domain::ContentDigest {
  return {"test-sha256", std::move(value), bytes};
}

auto snapshot(std::string fingerprint = "aaaaaaaaaaaaaaaa")
    -> domain::RepositorySnapshotIdentity {
  return {id<domain::RepositoryId>("repository-1"),
          digest(std::move(fingerprint), 64)};
}

auto source(std::string path = "src/main.cpp")
    -> domain::RepositorySourceIdentity {
  return {snapshot(), std::move(path), digest("bbbbbbbbbbbbbbbb", 64),
          domain::SourceByteRange{0, 13}};
}

auto provenance(domain::EvidenceDerivation derivation =
                    domain::EvidenceDerivation::observed)
    -> domain::EvidenceProvenance {
  return {
      derivation,
      "filesystem",
      "1",
      std::chrono::sys_time<std::chrono::milliseconds>{
          std::chrono::milliseconds{100}},
      snapshot(),
      {},
      {},
      std::nullopt,
  };
}

auto exact_item(std::string evidence_id = "evidence-1")
    -> domain::ContextParcelItem {
  return {
      id<domain::EvidenceId>(std::move(evidence_id)),
      domain::ExactSourceEvidence{source()},
      domain::EvidenceFreshness::current,
      provenance(),
      {domain::TextBlock{"int main() {}"}},
      13,
      4,
  };
}

auto parcel(std::vector<domain::ContextParcelItem> items = {exact_item()})
    -> domain::ContextParcel {
  return {
      id<domain::ContextParcelId>("parcel-1"),
      "diagnose compiler failure",
      domain::TaskPhase::diagnosis,
      snapshot(),
      std::move(items),
  };
}

auto artifact_block(const domain::ArtifactId& artifact_id)
    -> domain::ArtifactReferenceBlock {
  return {artifact_id, std::string{"evidence"}};
}

} // namespace

TEST_CASE("context parcel limits and metadata fail before item validation",
          "[evidence][parcel][failure]") {
  auto value = parcel();
  auto limits = repository::ContextParcelLimits{};
  limits.maximum_items = 0;
  auto result = repository::validate_context_parcel(value, limits);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_limits);

  limits = {};
  limits.maximum_content_blocks_per_item = 0;
  result = repository::validate_context_parcel(value, limits);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_limits);

  value.purpose.clear();
  result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_parcel);

  value = parcel();
  value.purpose = std::string{"bad\0purpose", 11};
  result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_parcel);

  value = parcel();
  value.phase = domain::TaskPhase::unknown;
  result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_parcel);

  value = parcel({});
  result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_parcel);
}

TEST_CASE("context parcels reject malformed snapshot and source identities",
          "[evidence][parcel][source][failure]") {
  auto value = parcel();
  value.target_snapshot.fingerprint.algorithm = "bad algorithm";
  auto result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_source);

  value = parcel();
  std::get<domain::ExactSourceEvidence>(value.items.front().reference)
      .source.relative_path = "../escape.cpp";
  result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_source);

  value = parcel();
  auto& identity =
      std::get<domain::ExactSourceEvidence>(value.items.front().reference)
          .source;
  identity.content_digest.value = "not-hex";
  result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_source);
}

TEST_CASE("source ranges are nonempty half-open bounds within the content",
          "[evidence][parcel][range][failure]") {
  auto value = parcel();
  auto& range =
      *std::get<domain::ExactSourceEvidence>(value.items.front().reference)
           .source.range;
  range.end = range.begin;
  auto result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_range);

  value = parcel();
  auto& source_value =
      std::get<domain::ExactSourceEvidence>(value.items.front().reference)
          .source;
  source_value.range = domain::SourceByteRange{32, 65};
  result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_range);

  value = parcel();
  std::get<domain::ExactSourceEvidence>(value.items.front().reference)
      .source.range = domain::SourceByteRange{0, 12};
  result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_reference);
}

TEST_CASE("exact source evidence cannot claim non-source content",
          "[evidence][parcel][reference][failure]") {
  auto value = parcel();
  value.items.front().content = {
      domain::StructuredDataBlock{"text/plain", "int main() {}"}};
  value.items.front().estimated_bytes = 23;
  auto result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_reference);
}

TEST_CASE("parcel item identities and estimates are bounded and checked",
          "[evidence][parcel][budget][failure]") {
  auto duplicate = exact_item();
  auto result = repository::validate_context_parcel(
      parcel({exact_item(), std::move(duplicate)}));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::duplicate_item);

  auto value = parcel();
  value.items.front().estimated_bytes = 1;
  result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_item);

  value = parcel({exact_item("one"), exact_item("two")});
  value.items[0].estimated_bytes = std::numeric_limits<std::uint64_t>::max();
  value.items[1].estimated_bytes = 13;
  repository::ContextParcelLimits limits;
  limits.maximum_item_bytes = std::numeric_limits<std::uint64_t>::max();
  limits.maximum_total_bytes = std::numeric_limits<std::uint64_t>::max();
  result = repository::validate_context_parcel(value, limits);
  REQUIRE_FALSE(result);
  INFO(result.error().message);
  REQUIRE(result.error().code == repository::ContextParcelErrorCode::overflow);

  value = parcel();
  limits = {};
  limits.maximum_total_tokens = 3;
  result = repository::validate_context_parcel(value, limits);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::resource_exhausted);
}

TEST_CASE("freshness claims agree with source snapshot provenance",
          "[evidence][parcel][freshness][failure]") {
  auto value = parcel();
  value.items.front().provenance.source_snapshot = snapshot("cccccccccccccccc");
  auto result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::conflicting_provenance);

  value = parcel();
  value.items.front().freshness = domain::EvidenceFreshness::stale;
  REQUIRE(repository::validate_context_parcel(value));

  value = parcel();
  auto old = snapshot("cccccccccccccccc");
  std::get<domain::ExactSourceEvidence>(value.items.front().reference)
      .source.snapshot = old;
  value.items.front().provenance.source_snapshot = old;
  result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_freshness);

  value.items.front().freshness = domain::EvidenceFreshness::stale;
  REQUIRE(repository::validate_context_parcel(value));

  value = parcel();
  value.items.front().provenance.source_snapshot->repository_id =
      id<domain::RepositoryId>("repository-2");
  std::get<domain::ExactSourceEvidence>(value.items.front().reference)
      .source.snapshot = *value.items.front().provenance.source_snapshot;
  result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_source);
}

TEST_CASE("unavailable evidence carries identity without model content",
          "[evidence][parcel][unavailable]") {
  auto value = parcel();
  auto& item = value.items.front();
  item.freshness = domain::EvidenceFreshness::unavailable;
  item.content.clear();
  item.estimated_bytes = 0;
  item.estimated_tokens = 0;
  REQUIRE(repository::validate_context_parcel(value));

  item.content = {domain::TextBlock{"stale bytes"}};
  auto result = repository::validate_context_parcel(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_freshness);
}

TEST_CASE("artifact references must match the typed evidence reference",
          "[evidence][parcel][artifact][failure]") {
  const auto artifact = id<domain::ArtifactId>("diagnostic-artifact");
  auto item = exact_item();
  item.reference = domain::DiagnosticEvidence{artifact, source()};
  item.content = {artifact_block(artifact)};
  item.estimated_bytes = 4096;
  item.estimated_tokens = 64;
  REQUIRE(repository::validate_context_parcel(parcel({item})));

  item.content = {artifact_block(id<domain::ArtifactId>("wrong-artifact"))};
  auto result = repository::validate_context_parcel(parcel({item}));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_reference);

  item.content = {artifact_block(artifact), artifact_block(artifact)};
  result = repository::validate_context_parcel(parcel({item}));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_reference);
}

TEST_CASE("known evidence cannot omit repository snapshot provenance",
          "[evidence][parcel][provenance][failure]") {
  const auto artifact = id<domain::ArtifactId>("diagnostic-artifact");
  auto item = exact_item();
  item.reference = domain::DiagnosticEvidence{artifact, std::nullopt};
  item.provenance.source_snapshot.reset();
  item.freshness = domain::EvidenceFreshness::possibly_stale;
  item.content = {artifact_block(artifact)};
  item.estimated_bytes = 4096;
  item.estimated_tokens = 64;
  auto result = repository::validate_context_parcel(parcel({item}));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_provenance);
}

TEST_CASE("derived provenance has stable unique non-self inputs",
          "[evidence][parcel][provenance][failure]") {
  auto base = exact_item("source-evidence");
  auto derived = exact_item("derived-evidence");
  const auto artifact = id<domain::ArtifactId>("derived-artifact");
  derived.reference =
      domain::DerivedRecordEvidence{"symbol-summary", "record-1", artifact};
  derived.provenance = provenance(domain::EvidenceDerivation::derived);
  derived.provenance.derivation_inputs = {base.evidence_id};
  derived.content = {artifact_block(artifact)};
  derived.estimated_bytes = 1024;
  derived.estimated_tokens = 32;
  REQUIRE(repository::validate_context_parcel(parcel({base, derived})));

  derived.provenance.derivation_inputs = {derived.evidence_id};
  auto result = repository::validate_context_parcel(parcel({base, derived}));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_provenance);

  derived.provenance.derivation_inputs = {base.evidence_id, base.evidence_id};
  result = repository::validate_context_parcel(parcel({base, derived}));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_provenance);

  derived.provenance.derivation_inputs.clear();
  result = repository::validate_context_parcel(parcel({base, derived}));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_provenance);
}

TEST_CASE("unknown evidence kinds remain opaque and bounded",
          "[evidence][parcel][unknown]") {
  auto item = exact_item();
  item.reference =
      domain::UnknownRepositoryEvidence{"future.semantic-record", std::nullopt};
  item.provenance.derivation = domain::EvidenceDerivation::unknown;
  REQUIRE(repository::validate_context_parcel(parcel({item})));

  std::get<domain::UnknownRepositoryEvidence>(item.reference).type_name.clear();
  auto result = repository::validate_context_parcel(parcel({item}));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::invalid_reference);
}

TEST_CASE("mixed context parcels report deterministic aggregate estimates",
          "[evidence][parcel][smoke]") {
  auto exact = exact_item("exact");

  const auto diagnostic_artifact = id<domain::ArtifactId>("diagnostic");
  auto diagnostic = exact_item("diagnostic-item");
  diagnostic.reference =
      domain::DiagnosticEvidence{diagnostic_artifact, source("src/lib.cpp")};
  diagnostic.content = {artifact_block(diagnostic_artifact)};
  diagnostic.estimated_bytes = 20;
  diagnostic.estimated_tokens = 5;

  const auto diff_artifact = id<domain::ArtifactId>("diff");
  auto diff = exact_item("diff-item");
  diff.reference = domain::DiffEvidence{snapshot("dddddddddddddddd"),
                                        snapshot(), diff_artifact};
  diff.content = {artifact_block(diff_artifact)};
  diff.estimated_bytes = 30;
  diff.estimated_tokens = 6;

  auto tool = exact_item("tool-item");
  const auto invocation = id<domain::InvocationId>("invocation-1");
  tool.reference = domain::ToolResultEvidence{invocation, std::nullopt};
  tool.provenance.producing_invocation_id = invocation;
  tool.content = {domain::StructuredDataBlock{"text/plain", "passed"}};
  tool.estimated_bytes = 16;
  tool.estimated_tokens = 3;

  auto value = parcel({exact, diagnostic, diff, tool});
  auto result = repository::validate_context_parcel(value);
  REQUIRE(result);
  REQUIRE(result->item_count == 4);
  REQUIRE(result->represented_bytes == 79);
  REQUIRE(result->estimated_tokens == 18);
  REQUIRE(result->inline_bytes > 0);

  auto observed = domain::snapshot_identity(domain::RepositorySnapshot{
      {value.target_snapshot.repository_id, "/work/repository"},
      std::nullopt,
      {},
      value.target_snapshot.fingerprint,
      std::chrono::sys_time<std::chrono::milliseconds>{
          std::chrono::milliseconds{100}},
  });
  REQUIRE(domain::same_source_state(observed, value.target_snapshot));
}

TEST_CASE("parcel budgets accept exact boundaries and reject excess",
          "[evidence][parcel][budget]") {
  auto item = exact_item();
  item.estimated_bytes = 64;
  repository::ContextParcelLimits limits;
  limits.maximum_items = 1;
  limits.maximum_item_bytes = 64;
  limits.maximum_total_bytes = 64;
  limits.maximum_total_tokens = 4;
  auto result = repository::validate_context_parcel(parcel({item}), limits);
  REQUIRE(result);
  REQUIRE(result->represented_bytes == 64);
  REQUIRE(result->estimated_tokens == 4);

  item.estimated_bytes = 65;
  result = repository::validate_context_parcel(parcel({item}), limits);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::resource_exhausted);

  item = exact_item();
  item.content.push_back(domain::TextBlock{"more"});
  item.estimated_bytes = 17;
  limits = {};
  limits.maximum_content_blocks_per_item = 1;
  result = repository::validate_context_parcel(parcel({item}), limits);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ContextParcelErrorCode::resource_exhausted);
}
