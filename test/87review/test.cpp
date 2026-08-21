#include <aiforge/repository/review_receipt.hpp>
#include <aiforge/runtime/review_gate.hpp>
#include <aiforge/testing/scripted_hosted_review_check.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace aiforge;

template <typename Id>
auto id(std::string value) -> Id {
  auto parsed = Id::from(std::move(value));
  REQUIRE(parsed);
  return std::move(*parsed);
}

auto digest(std::string value = "aaaaaaaaaaaaaaaa",
            const std::uint64_t bytes = 64) -> domain::ContentDigest {
  return {"sha256", std::move(value), bytes};
}

auto candidate(std::string fingerprint = "aaaaaaaaaaaaaaaa",
               std::string revision = "0123456789abcdef")
    -> domain::ReviewCandidate {
  return {{id<domain::RepositoryId>("repository"),
           digest(std::move(fingerprint), 0)},
          std::move(revision)};
}

auto verification_binding() -> domain::ReviewEvidenceBinding {
  return {id<domain::ReviewRequirementId>("tests"),
          domain::ReviewEvidenceKind::verification,
          "ctest",
          "3.28",
          id<domain::VerificationEvidenceId>("verification"),
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          digest("bbbbbbbbbbbbbbbb", 512),
          {{id<domain::ArtifactId>("test-report"),
            digest("cccccccccccccccc", 2048)}}};
}

auto scenario_binding() -> domain::ReviewEvidenceBinding {
  return {id<domain::ReviewRequirementId>("tui-scenario"),
          domain::ReviewEvidenceKind::scenario,
          "aiforge-scenario",
          "1",
          std::nullopt,
          std::string{"interactive-stream"},
          std::string{"corpus-v1"},
          std::string{"0123456789abcdef"},
          digest("1111111111111111", 1024),
          digest("2222222222222222", 128),
          digest("dddddddddddddddd", 4096),
          {{id<domain::ArtifactId>("scenario-trace"),
            digest("eeeeeeeeeeeeeeee", 4096)}}};
}

auto draft() -> domain::ReviewReceiptDraft {
  return {id<domain::ReviewReceiptId>("receipt"), candidate(),
          {verification_binding(), scenario_binding()}};
}

auto actor(std::string actor_id = "reviewer") -> domain::ReviewActor {
  return {std::move(actor_id), "Repository Reviewer"};
}

template <typename Payload>
auto event(const std::uint64_t sequence, Payload payload,
           std::string event_id = {}) -> domain::RunEvent {
  if (event_id.empty()) event_id = "event-" + std::to_string(sequence);
  return {{id<domain::EventId>(std::move(event_id)),
           id<domain::RunId>("review-run"), sequence, 1,
           domain::EventTimestamp{std::chrono::milliseconds{1000 + sequence}},
           std::nullopt, std::nullopt, std::nullopt},
          std::move(payload)};
}

auto approved_events() -> std::vector<domain::RunEvent> {
  const auto value = draft();
  return {
      event(1, domain::ReviewReceiptDrafted{value}),
      event(2, domain::ReviewRequested{value.receipt_id, actor()}),
      event(3,
            domain::ReviewVerdictRecorded{value.receipt_id,
                                          domain::ReviewVerdict::approved,
                                          actor()},
            "approval")};
}

auto policy(const bool hosted = false) -> runtime::ReviewAuthorizationPolicy {
  return {{{id<domain::ReviewRequirementId>("tests"),
            domain::ReviewEvidenceKind::verification, "ctest", "3.28",
            std::nullopt, std::nullopt},
           {id<domain::ReviewRequirementId>("tui-scenario"),
            domain::ReviewEvidenceKind::scenario, "aiforge-scenario", "1",
            std::string{"interactive-stream"},
            std::string{"corpus-v1"}}},
          false,
          {},
          hosted};
}

auto environment() -> runtime::ReviewGateEnvironment {
  const auto value = draft();
  return {value.candidate, value.evidence};
}

auto contains(const runtime::ReviewGateDecision& decision,
              const runtime::ReviewInvalidationTrigger trigger) -> bool {
  return std::ranges::find(decision.triggers, trigger) !=
         decision.triggers.end();
}

}  // namespace

TEST_CASE("review receipt validation rejects ambiguous or duplicated evidence",
          "[review][receipt][failure]") {
  auto value = draft();
  REQUIRE(repository::validate_review_receipt_draft(value));

  value.evidence.front().kind =
      static_cast<domain::ReviewEvidenceKind>(999);
  auto result = repository::validate_review_receipt_draft(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ReviewReceiptErrorCode::invalid_evidence);

  value = draft();
  value.evidence.front().scenario_id = "ambiguous";
  result = repository::validate_review_receipt_draft(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ReviewReceiptErrorCode::invalid_evidence);

  value = draft();
  value.evidence.back().requirement_id = value.evidence.front().requirement_id;
  result = repository::validate_review_receipt_draft(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ReviewReceiptErrorCode::duplicate_identity);

  value = draft();
  value.evidence.front().artifacts.push_back(
      value.evidence.front().artifacts.front());
  result = repository::validate_review_receipt_draft(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ReviewReceiptErrorCode::duplicate_identity);

  repository::ReviewReceiptLimits limits;
  limits.maximum_evidence = 1;
  result = repository::validate_review_receipt_draft(draft(), limits);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ReviewReceiptErrorCode::resource_exhausted);

  value = draft();
  value.evidence.back().scenario_application_revision = "another-revision";
  result = repository::validate_review_receipt_draft(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          repository::ReviewReceiptErrorCode::invalid_evidence);
}

TEST_CASE("review lifecycle projects findings verdicts revocation and conflict",
          "[review][projection][failure]") {
  const auto value = draft();
  repository::ReviewReceiptProjection projection;
  REQUIRE(projection.apply(event(1, domain::ReviewReceiptDrafted{value})));
  REQUIRE(projection.state() == repository::ReviewReceiptState::draft);
  auto invalid_actor = actor("reviewer\nforged");
  auto invalid_request = projection.apply(
      event(2, domain::ReviewRequested{value.receipt_id, invalid_actor},
            "invalid-request"));
  REQUIRE_FALSE(invalid_request);
  REQUIRE(projection.last_sequence() == 1);
  REQUIRE(projection.apply(
      event(2, domain::ReviewRequested{value.receipt_id, actor()})));
  REQUIRE(projection.state() ==
          repository::ReviewReceiptState::review_requested);

  const domain::ReviewFinding finding{
      id<domain::ReviewFindingId>("finding"), "A bounded defect",
      id<domain::VerificationEvidenceId>("verification"),
      {id<domain::ArtifactId>("test-report")}};
  REQUIRE(projection.apply(
      event(3, domain::ReviewFindingOpened{value.receipt_id, finding})));
  REQUIRE(projection.apply(event(
      4,
      domain::ReviewVerdictRecorded{value.receipt_id,
                                    domain::ReviewVerdict::approved, actor()},
      "approval")));
  REQUIRE(projection.state() == repository::ReviewReceiptState::findings_open);
  REQUIRE(projection.apply(event(
      5, domain::ReviewFindingResolved{value.receipt_id, finding.finding_id,
                                       actor(), std::string{"fixed"}})));
  REQUIRE(projection.state() == repository::ReviewReceiptState::approved);

  REQUIRE(projection.apply(event(
      6, domain::ReviewVerdictRevoked{value.receipt_id,
                                      id<domain::EventId>("approval"), actor(),
                                      "candidate requires another pass"})));
  REQUIRE(projection.state() == repository::ReviewReceiptState::revoked);
  REQUIRE(projection.apply(event(
      7,
      domain::ReviewVerdictRecorded{value.receipt_id,
                                    domain::ReviewVerdict::approved, actor()},
      "approval-two")));
  REQUIRE(projection.apply(event(
      8, domain::ReviewVerdictRecorded{
             value.receipt_id, domain::ReviewVerdict::changes_requested,
             actor("second-reviewer")},
      "conflict")));
  REQUIRE(projection.state() == repository::ReviewReceiptState::conflicted);

  auto duplicate_resolution = projection.apply(event(
      9, domain::ReviewFindingResolved{value.receipt_id, finding.finding_id,
                                       actor(), std::nullopt}));
  REQUIRE_FALSE(duplicate_resolution);
  REQUIRE(duplicate_resolution.error().code ==
          repository::ReviewReceiptErrorCode::unknown_finding);
  REQUIRE(projection.last_sequence() == 8);

  const auto rebuilt = repository::ReviewReceiptProjection::rebuild(
      std::vector<domain::RunEvent>{
          event(1, domain::ReviewReceiptDrafted{value}),
          event(2, domain::ReviewRequested{value.receipt_id, actor()}),
          event(3, domain::ReviewFindingOpened{value.receipt_id, finding})});
  REQUIRE(rebuilt);
  REQUIRE(rebuilt->state() == repository::ReviewReceiptState::findings_open);
}

TEST_CASE("merge gate invalidates changed candidates evidence and policy",
          "[review][gate][failure]") {
  const auto projection =
      repository::ReviewReceiptProjection::rebuild(approved_events());
  REQUIRE(projection);
  runtime::ReviewMergeGate gate;

  auto current = environment();
  auto decision = gate.evaluate(*projection, policy(), current);
  REQUIRE(decision);
  REQUIRE(decision->authorization);
  REQUIRE(decision->state == runtime::ReviewGateState::authorized);
  REQUIRE(decision->authorization->source() ==
          runtime::ReviewAuthorizationSource::receipt);
  REQUIRE(decision->authorization->candidate() == current.current_candidate);

  current.current_candidate = candidate("ffffffffffffffff");
  decision = gate.evaluate(*projection, policy(), current);
  REQUIRE(decision);
  REQUIRE_FALSE(decision->authorization);
  REQUIRE(decision->state == runtime::ReviewGateState::invalidated);
  REQUIRE(contains(*decision,
                   runtime::ReviewInvalidationTrigger::candidate_changed));

  current = environment();
  current.current_candidate.revision = "fedcba9876543210";
  decision = gate.evaluate(*projection, policy(), current);
  REQUIRE(decision);
  REQUIRE_FALSE(decision->authorization);
  REQUIRE(contains(*decision,
                   runtime::ReviewInvalidationTrigger::candidate_changed));

  current = environment();
  current.current_evidence.front().result_digest =
      digest("9999999999999999", 512);
  decision = gate.evaluate(*projection, policy(), current);
  REQUIRE(decision);
  REQUIRE_FALSE(decision->authorization);
  REQUIRE(contains(*decision,
                   runtime::ReviewInvalidationTrigger::evidence_changed));

  current = environment();
  current.current_evidence.front().artifacts.clear();
  decision = gate.evaluate(*projection, policy(), current);
  REQUIRE(decision);
  REQUIRE_FALSE(decision->authorization);
  REQUIRE(contains(
      *decision,
      runtime::ReviewInvalidationTrigger::artifact_missing_or_changed));

  current = environment();
  current.current_evidence.back().scenario_fake_script_digest =
      digest("3333333333333333", 1024);
  decision = gate.evaluate(*projection, policy(), current);
  REQUIRE(decision);
  REQUIRE_FALSE(decision->authorization);
  REQUIRE(contains(*decision,
                   runtime::ReviewInvalidationTrigger::evidence_changed));

  auto stale_policy = policy();
  stale_policy.required_evidence.back().producer_version = "2";
  decision = gate.evaluate(*projection, stale_policy, environment());
  REQUIRE(decision);
  REQUIRE_FALSE(decision->authorization);
  REQUIRE(contains(
      *decision,
      runtime::ReviewInvalidationTrigger::verifier_version_changed));

  stale_policy = policy();
  stale_policy.required_evidence.back().scenario_corpus_version = "corpus-v2";
  decision = gate.evaluate(*projection, stale_policy, environment());
  REQUIRE(decision);
  REQUIRE_FALSE(decision->authorization);
  REQUIRE(contains(
      *decision,
      runtime::ReviewInvalidationTrigger::scenario_version_changed));
}

TEST_CASE("trusted overrides are loud candidate-bound and revocable",
          "[review][override][failure]") {
  const auto value = draft();
  std::vector events{
      event(1, domain::ReviewReceiptDrafted{value}),
      event(2, domain::ReviewRequested{value.receipt_id, actor()}),
      event(3, domain::ReviewOverrideRecorded{domain::ReviewOverride{
                   id<domain::ReviewOverrideId>("override"), value.receipt_id,
                   value.candidate, actor("maintainer"),
                   "Emergency release after explicit inspection"}})};
  auto projection = repository::ReviewReceiptProjection::rebuild(events);
  REQUIRE(projection);

  auto override_policy = policy();
  override_policy.allow_human_override = true;
  override_policy.trusted_override_actor_ids = {"maintainer"};
  runtime::ReviewMergeGate gate;
  auto decision = gate.evaluate(*projection, override_policy, environment());
  REQUIRE(decision);
  REQUIRE(decision->authorization);
  REQUIRE(decision->state == runtime::ReviewGateState::overridden);
  REQUIRE(decision->authorization->source() ==
          runtime::ReviewAuthorizationSource::human_override);
  REQUIRE(decision->explanation.find("explicit trusted human override") !=
          std::string::npos);
  const auto override_digest = decision->authorization->decision_digest();

  auto changed_override_events = events;
  auto& changed_override = std::get<domain::ReviewOverrideRecorded>(
      changed_override_events.back().payload);
  changed_override.override.reason = "Different explicit rationale";
  const auto changed_override_projection =
      repository::ReviewReceiptProjection::rebuild(changed_override_events);
  REQUIRE(changed_override_projection);
  const auto changed_override_decision = gate.evaluate(
      *changed_override_projection, override_policy, environment());
  REQUIRE(changed_override_decision);
  REQUIRE(changed_override_decision->authorization);
  REQUIRE(changed_override_decision->authorization->decision_digest() !=
          override_digest);

  override_policy.trusted_override_actor_ids = {"someone-else"};
  decision = gate.evaluate(*projection, override_policy, environment());
  REQUIRE(decision);
  REQUIRE_FALSE(decision->authorization);
  REQUIRE(contains(*decision,
                   runtime::ReviewInvalidationTrigger::override_untrusted));

  events.push_back(event(
      4, domain::ReviewOverrideRevoked{value.receipt_id,
                                       id<domain::ReviewOverrideId>("override"),
                                       actor("maintainer"), "risk restored"}));
  projection = repository::ReviewReceiptProjection::rebuild(events);
  REQUIRE(projection);
  override_policy.trusted_override_actor_ids = {"maintainer"};
  decision = gate.evaluate(*projection, override_policy, environment());
  REQUIRE(decision);
  REQUIRE_FALSE(decision->authorization);
}

TEST_CASE("required hosted checks confirm the exact candidate and decision",
          "[review][hosted][failure]") {
  const auto projection =
      repository::ReviewReceiptProjection::rebuild(approved_events());
  REQUIRE(projection);
  runtime::ReviewMergeGate gate;
  const auto local = gate.evaluate(*projection, policy(), environment());
  REQUIRE(local);
  REQUIRE(local->authorization);

  auto hosted_policy = policy(true);
  const auto missing = gate.evaluate(*projection, hosted_policy, environment());
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().code ==
          runtime::ReviewGateErrorCode::hosted_check_failure);

  runtime::HostedReviewCheckUpdate expected{
      draft().receipt_id, draft().candidate,
      runtime::HostedReviewCheckState::success,
      local->authorization->decision_digest(),
      "review receipt approved for the exact candidate"};
  runtime::HostedReviewCheckConfirmation confirmation{
      draft().candidate, runtime::HostedReviewCheckState::success,
      local->authorization->decision_digest()};
  testing::ScriptedHostedReviewCheck fake{{{expected, confirmation}}};
  auto decision = gate.evaluate(*projection, hosted_policy, environment(), &fake);
  REQUIRE(decision);
  REQUIRE(decision->authorization);
  REQUIRE(decision->hosted_check == confirmation);
  REQUIRE(fake.recorded_updates() ==
          std::vector<runtime::HostedReviewCheckUpdate>{expected});
  REQUIRE(fake.remaining_exchanges() == 0);

  auto mismatch = confirmation;
  mismatch.candidate = candidate("ffffffffffffffff");
  testing::ScriptedHostedReviewCheck mismatched{{{expected, mismatch}}};
  decision = gate.evaluate(*projection, hosted_policy, environment(),
                           &mismatched);
  REQUIRE_FALSE(decision);
  REQUIRE(decision.error().code ==
          runtime::ReviewGateErrorCode::hosted_check_failure);

  testing::ScriptedHostedReviewCheck unavailable{{{
      expected,
      runtime::HostedReviewCheckError{
          runtime::HostedReviewCheckErrorCode::unavailable, "offline", true}}}};
  decision = gate.evaluate(*projection, hosted_policy, environment(),
                           &unavailable);
  REQUIRE_FALSE(decision);
  REQUIRE(decision.error().retryable);
}
