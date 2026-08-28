#include <aiforge/repository/context_parcel.hpp>
#include <aiforge/repository/verification_evidence.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace {

using namespace aiforge;

template <typename Id> auto id(std::string value) -> Id {
  auto parsed = Id::from(std::move(value));
  REQUIRE(parsed);
  return std::move(*parsed);
}

auto digest(std::string value = "aaaaaaaaaaaaaaaa",
            const std::uint64_t bytes = 64) -> domain::ContentDigest {
  return {"test-sha256", std::move(value), bytes};
}

auto snapshot(std::string value = "aaaaaaaaaaaaaaaa")
    -> domain::RepositorySnapshotIdentity {
  return {id<domain::RepositoryId>("repository"), digest(std::move(value), 0)};
}

auto evidence(const domain::VerificationOutcome outcome =
                  domain::VerificationOutcome::passed)
    -> domain::VerificationEvidence {
  const auto invocation = id<domain::InvocationId>("verification-invocation");
  const auto artifact = id<domain::ArtifactId>("complete-output");
  return {id<domain::VerificationEvidenceId>("verification-1"),
          domain::VerificationKind::test,
          std::nullopt,
          outcome,
          snapshot(),
          std::nullopt,
          digest("bbbbbbbbbbbbbbbb", 128),
          {"ctest", "3.28", "run_process", invocation},
          std::chrono::sys_time<std::chrono::milliseconds>{
              std::chrono::milliseconds{100}},
          "29 tests passed",
          {{domain::VerificationOutputStream::standard_output, "29/29 passed",
            4096, true, artifact}},
          {{domain::VerificationDiagnosticSeverity::warning, "W1",
            "bounded warning", std::nullopt}},
          {id<domain::ArtifactId>("test-report")}};
}

auto environment(const domain::VerificationEvidence& value)
    -> repository::VerificationEvidenceEnvironment {
  return {value.source_snapshot,
          value.build_configuration,
          {id<domain::ArtifactId>("complete-output"),
           id<domain::ArtifactId>("test-report")},
          true};
}

} // namespace

TEST_CASE(
    "verification evidence rejects malformed identity provenance and output",
    "[verification][failure]") {
  auto value = evidence();
  value.kind = domain::VerificationKind::unknown;
  auto result = repository::validate_verification_evidence(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_evidence);

  value = evidence();
  value.kind = static_cast<domain::VerificationKind>(999);
  result = repository::validate_verification_evidence(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_evidence);

  value = evidence();
  value.outcome = static_cast<domain::VerificationOutcome>(999);
  result = repository::validate_verification_evidence(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_evidence);

  value = evidence();
  value.producer.tool_name = std::string{"bad\0tool", 8};
  result = repository::validate_verification_evidence(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_provenance);

  value = evidence();
  value.output.front().represented_bytes = 2;
  result = repository::validate_verification_evidence(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_output);

  value = evidence();
  value.output.push_back(value.output.front());
  result = repository::validate_verification_evidence(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_output);

  value = evidence();
  value.output.front().stream =
      static_cast<domain::VerificationOutputStream>(999);
  result = repository::validate_verification_evidence(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_output);

  value = evidence();
  value.output.front().complete_artifact_id.reset();
  result = repository::validate_verification_evidence(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_output);
}

TEST_CASE("verification diagnostics snapshots and artifacts fail closed",
          "[verification][diagnostic][artifact][failure]") {
  auto value = evidence();
  value.diagnostics.front().severity =
      domain::VerificationDiagnosticSeverity::unknown;
  auto result = repository::validate_verification_evidence(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_diagnostic);

  value = evidence();
  value.diagnostics.front().severity =
      static_cast<domain::VerificationDiagnosticSeverity>(999);
  result = repository::validate_verification_evidence(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_diagnostic);

  value = evidence();
  value.artifacts.push_back(value.artifacts.front());
  result = repository::validate_verification_evidence(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::VerificationEvidenceErrorCode::duplicate_artifact);

  value = evidence();
  value.artifacts.push_back(*value.output.front().complete_artifact_id);
  result = repository::validate_verification_evidence(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::VerificationEvidenceErrorCode::duplicate_artifact);

  value = evidence();
  value.kind = domain::VerificationKind::diff;
  result = repository::validate_verification_evidence(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_source);
}

TEST_CASE("verification freshness tracks source configuration and artifacts",
          "[verification][freshness]") {
  const auto value = evidence();
  auto current =
      repository::assess_verification_evidence(value, environment(value));
  REQUIRE(current);
  REQUIRE(current->freshness == domain::EvidenceFreshness::current);
  REQUIRE(current->affected_triggers.empty());

  auto changed = environment(value);
  changed.source_snapshot = snapshot("cccccccccccccccc");
  auto stale = repository::assess_verification_evidence(value, changed);
  REQUIRE(stale);
  REQUIRE(stale->freshness == domain::EvidenceFreshness::stale);

  auto missing = environment(value);
  missing.available_artifacts.pop_back();
  auto unavailable = repository::assess_verification_evidence(value, missing);
  REQUIRE(unavailable);
  REQUIRE(unavailable->freshness == domain::EvidenceFreshness::unavailable);

  auto uncertain = environment(value);
  uncertain.build_configuration.reset();
  uncertain.artifact_observation_complete = false;
  uncertain.available_artifacts.clear();
  auto possibly_stale =
      repository::assess_verification_evidence(value, uncertain);
  REQUIRE(possibly_stale);
  REQUIRE(possibly_stale->freshness ==
          domain::EvidenceFreshness::possibly_stale);

  auto malformed = environment(value);
  malformed.source_snapshot->fingerprint.value = "not-hex";
  const auto invalid =
      repository::assess_verification_evidence(value, malformed);
  REQUIRE_FALSE(invalid);
  REQUIRE(invalid.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_source);
}

TEST_CASE("verification records become bounded untrusted context evidence",
          "[verification][context]") {
  const auto value = evidence();
  auto item = repository::make_verification_context_item(
      value, id<domain::EvidenceId>("context-verification"),
      domain::EvidenceFreshness::current, 32);
  REQUIRE(item);
  REQUIRE(std::holds_alternative<domain::VerificationEvidenceReference>(
      item->reference));
  REQUIRE(item->provenance.producing_invocation_id ==
          value.producer.invocation_id);

  const domain::ContextParcel parcel{
      id<domain::ContextParcelId>("verification-parcel"),
      "verify candidate change",
      domain::TaskPhase::verification,
      value.source_snapshot,
      {*item}};
  REQUIRE(repository::validate_context_parcel(parcel));

  const auto zero_tokens = repository::make_verification_context_item(
      value, id<domain::EvidenceId>("zero-token-verification"),
      domain::EvidenceFreshness::current, 0);
  REQUIRE_FALSE(zero_tokens);
  REQUIRE(zero_tokens.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_evidence);

  const auto unknown_freshness = repository::make_verification_context_item(
      value, id<domain::EvidenceId>("unknown-freshness-verification"),
      static_cast<domain::EvidenceFreshness>(999), 32);
  REQUIRE_FALSE(unknown_freshness);
  REQUIRE(unknown_freshness.error().code ==
          repository::VerificationEvidenceErrorCode::invalid_evidence);

  auto crowded = evidence();
  crowded.diagnostics.assign(1021,
                             {domain::VerificationDiagnosticSeverity::warning,
                              {},
                              "warning",
                              std::nullopt});
  const auto too_many_blocks = repository::make_verification_context_item(
      crowded, id<domain::EvidenceId>("crowded-verification"),
      domain::EvidenceFreshness::current, 32);
  REQUIRE_FALSE(too_many_blocks);
  REQUIRE(too_many_blocks.error().code ==
          repository::VerificationEvidenceErrorCode::resource_exhausted);

  auto unavailable = repository::make_verification_context_item(
      value, id<domain::EvidenceId>("unavailable-verification"),
      domain::EvidenceFreshness::unavailable, 32);
  REQUIRE(unavailable);
  REQUIRE(unavailable->content.empty());
  REQUIRE(unavailable->estimated_bytes == 0);
  REQUIRE(unavailable->estimated_tokens == 0);
}

TEST_CASE("terminal and partial verification outcomes remain representable",
          "[verification][outcome][smoke]") {
  for (const auto outcome : {domain::VerificationOutcome::passed,
                             domain::VerificationOutcome::failed,
                             domain::VerificationOutcome::partial,
                             domain::VerificationOutcome::cancelled,
                             domain::VerificationOutcome::timed_out,
                             domain::VerificationOutcome::unavailable}) {
    REQUIRE(repository::validate_verification_evidence(evidence(outcome)));
  }

  auto partial = evidence(domain::VerificationOutcome::partial);
  partial.output.front().complete_artifact_id.reset();
  REQUIRE(repository::validate_verification_evidence(partial));

  auto binary = evidence();
  binary.output.front().text.clear();
  binary.output.front().represented_bytes = 4096;
  binary.output.front().truncated = true;
  REQUIRE(repository::validate_verification_evidence(binary));

  auto value = evidence();
  value.output.front().represented_bytes =
      std::numeric_limits<std::uint64_t>::max();
  const auto projected = repository::make_verification_context_item(
      value, id<domain::EvidenceId>("overflow"),
      domain::EvidenceFreshness::current, 1);
  REQUIRE_FALSE(projected);
  REQUIRE(projected.error().code ==
          repository::VerificationEvidenceErrorCode::overflow);
}
