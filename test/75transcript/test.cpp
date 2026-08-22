#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/domain/transcript_projection.hpp>
#include <aiforge/presentation/text.hpp>

namespace {

using namespace aiforge;

template <typename IdType>
auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

template <typename Payload>
auto event(const std::uint64_t sequence, Payload payload,
           std::string event_id = {}, std::string run_id = "run",
           std::optional<domain::InvocationId> invocation = std::nullopt)
    -> domain::RunEvent {
  if (event_id.empty()) event_id = "event-" + std::to_string(sequence);
  return domain::RunEvent{
      domain::EventMetadata{
          make_id<domain::EventId>(event_id), make_id<domain::RunId>(run_id),
          sequence, 1,
          domain::EventTimestamp{std::chrono::milliseconds{sequence}},
          std::nullopt, std::nullopt, std::move(invocation)},
      std::move(payload)};
}

auto started() -> domain::RunStarted {
  return {make_id<domain::SurfaceId>("surface"),
          make_id<domain::WorkspaceId>("chat"),
          make_id<domain::PermissionProfileId>("observe"), std::nullopt};
}

auto message(const std::string& id, const domain::Role role,
             std::string text) -> domain::Message {
  return {make_id<domain::MessageId>(id), role,
          {domain::TextBlock{std::move(text)}}, std::nullopt};
}

auto verification(const domain::InvocationId& invocation,
                  const domain::ArtifactId& artifact)
    -> domain::VerificationEvidence {
  return {make_id<domain::VerificationEvidenceId>("verification"),
          domain::VerificationKind::test,
          std::nullopt,
          domain::VerificationOutcome::passed,
          {make_id<domain::RepositoryId>("repository"),
           {"sha256", "aaaaaaaaaaaaaaaa", 0}},
          std::nullopt,
          std::nullopt,
          {"ctest", "3.28", "run_process", invocation},
          std::chrono::sys_time<std::chrono::milliseconds>{
              std::chrono::milliseconds{100}},
          "all tests passed",
          {{domain::VerificationOutputStream::standard_output, "passed", 6,
            false, std::nullopt}},
          {},
          {artifact}};
}

}  // namespace

TEST_CASE("transcript projection rejects invalid ordering transactionally",
          "[transcript][failure]") {
  domain::TranscriptProjection projection;
  const auto inference = make_id<domain::InferenceId>("inference");
  const auto assistant = make_id<domain::MessageId>("assistant");

  auto rejected = projection.apply(event(
      1, domain::AssistantContentDeltaAdded{
             assistant, inference, domain::TextBlock{"orphan"}}));
  REQUIRE_FALSE(rejected);
  REQUIRE(projection.last_sequence() == 0);

  REQUIRE(projection.apply(
      event(2, domain::UnknownEvent{"future.event"}, "future")));
  REQUIRE(projection.apply(event(5, started(), "start")));
  REQUIRE(projection.last_sequence() == 5);

  rejected = projection.apply(event(6, domain::UnknownEvent{"duplicate"},
                                    "start"));
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          domain::TranscriptProjectionErrorCode::duplicate_event);
  REQUIRE(projection.last_sequence() == 5);

  rejected = projection.apply(
      event(6, domain::RunCompleted{}, "other", "another-run"));
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          domain::TranscriptProjectionErrorCode::wrong_run);
  REQUIRE(projection.last_sequence() == 5);
}

TEST_CASE("transcript rebuild equals deterministic incremental application",
          "[transcript]") {
  const auto inference = make_id<domain::InferenceId>("inference");
  const auto assistant = make_id<domain::MessageId>("assistant");
  const std::vector events{
      event(1, started()),
      event(2, domain::UserContentAdded{
                   message("user", domain::Role::user, "hello")}),
      event(3, domain::InferenceStarted{
                   inference, make_id<domain::ModelId>("model")}),
      event(4, domain::AssistantContentStarted{assistant, inference}),
      event(5, domain::AssistantContentDeltaAdded{
                   assistant, inference, domain::TextBlock{"**answer**"}}),
      event(6, domain::UsageRecorded{inference, {4, 2, 1, 0}}),
      event(7, domain::AssistantContentFinished{assistant, inference}),
      event(8, domain::InferenceFinished{inference,
                                         domain::FinishReason::stop}),
      event(9, domain::RunCompleted{}),
  };

  domain::TranscriptProjection incremental;
  for (const auto& value : events) REQUIRE(incremental.apply(value));
  const auto rebuilt = domain::TranscriptProjection::rebuild(events);
  REQUIRE(rebuilt);
  REQUIRE(rebuilt->items() == incremental.items());
  REQUIRE(rebuilt->usage() == incremental.usage());
  REQUIRE(rebuilt->status() == domain::RunStatus::completed);
  REQUIRE(rebuilt->last_sequence() == 9);

  REQUIRE(incremental.items().size() == 2);
  const auto& answer =
      std::get<domain::TranscriptMessage>(incremental.items().back());
  REQUIRE(answer.state == domain::TranscriptMessageState::complete);
  REQUIRE(answer.usage == domain::Usage{4, 2, 1, 0});
}

TEST_CASE("a provenance record replays without adding a transcript item",
          "[transcript][provenance]") {
  const domain::RunProvenance provenance{
      "0.10.0",
      "venice",
      std::nullopt,
      make_id<domain::ModelId>("model"),
      domain::CredentialSourceReference{
          domain::CredentialSourceKind::environment, "VENICE_API_KEY"},
      {{"model", std::string{"venice-model"}, true,
        domain::ProvenanceSource::environment, false,
        {{domain::ProvenanceSource::environment,
          domain::ProvenanceDisposition::selected, std::nullopt}}}},
      {{"aiforge", "0.10.0"}},
      {}};
  const std::vector events{
      event(1, started()),
      event(2, domain::RunProvenanceRecorded{provenance}),
      event(3, domain::UserContentAdded{
                   message("user", domain::Role::user, "hello")}),
      event(4, domain::RunCompleted{}),
  };

  domain::TranscriptProjection incremental;
  for (const auto& value : events) REQUIRE(incremental.apply(value));
  const auto rebuilt = domain::TranscriptProjection::rebuild(events);
  REQUIRE(rebuilt);
  REQUIRE(rebuilt->items() == incremental.items());
  // Provenance is run state, not a rendered turn.
  REQUIRE(incremental.items().size() == 1);
  REQUIRE(rebuilt->last_sequence() == 4);

  const auto session = domain::SessionTranscriptProjection::rebuild(events);
  REQUIRE(session);

  // The once-per-run rule is inherited from the run projection.
  auto duplicated = events;
  duplicated.insert(duplicated.begin() + 2,
                    event(3, domain::RunProvenanceRecorded{provenance},
                          "event-3b"));
  duplicated.back().metadata.sequence = 5;
  REQUIRE_FALSE(domain::TranscriptProjection::rebuild(duplicated));
}

TEST_CASE("session transcript composes sequential and interleaved runs",
          "[transcript][session]") {
  const std::vector events{
      event(1, started(), "first-start", "first-run"),
      event(2, started(), "second-start", "second-run"),
      event(3, domain::UnknownEvent{"future.event"}, "future", "first-run"),
      event(4,
            domain::UserContentAdded{
                message("first-user", domain::Role::user, "first")},
            "first-message", "first-run"),
      event(5,
            domain::UserContentAdded{
                message("second-user", domain::Role::user, "second")},
            "second-message", "second-run"),
      event(6, domain::RunCompleted{}, "first-complete", "first-run"),
      event(7, domain::RunCompleted{}, "second-complete", "second-run"),
  };

  domain::SessionTranscriptProjection incremental;
  for (const auto& value : events) REQUIRE(incremental.apply(value));
  const auto rebuilt = domain::SessionTranscriptProjection::rebuild(events);
  REQUIRE(rebuilt);
  REQUIRE(rebuilt->runs().size() == 2);
  REQUIRE(rebuilt->last_sequence() == 7);
  REQUIRE(rebuilt->runs()[0].run_id() ==
          make_id<domain::RunId>("first-run"));
  REQUIRE(rebuilt->runs()[1].run_id() ==
          make_id<domain::RunId>("second-run"));
  REQUIRE(rebuilt->runs()[0].items() == incremental.runs()[0].items());
  REQUIRE(rebuilt->runs()[1].items() == incremental.runs()[1].items());
}

TEST_CASE("session transcript rejects cross-run envelope failures transactionally",
          "[transcript][session][failure]") {
  domain::SessionTranscriptProjection projection;
  REQUIRE(projection.apply(event(1, started(), "start", "first-run")));
  REQUIRE(projection.apply(
      event(3, domain::RunCompleted{}, "complete", "first-run")));

  auto rejected = projection.apply(
      event(4, started(), "start", "second-run"));
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          domain::TranscriptProjectionErrorCode::duplicate_event);
  REQUIRE(projection.last_sequence() == 3);
  REQUIRE(projection.runs().size() == 1);

  rejected = projection.apply(
      event(2, started(), "second-start", "second-run"));
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          domain::TranscriptProjectionErrorCode::non_monotonic_sequence);
  REQUIRE(projection.last_sequence() == 3);
  REQUIRE(projection.runs().size() == 1);

  rejected = projection.apply(event(
      4,
      domain::UserContentAdded{
          message("orphan", domain::Role::user, "missing start")},
      "orphan-message", "second-run"));
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          domain::TranscriptProjectionErrorCode::invalid_transition);
  REQUIRE(projection.last_sequence() == 3);
  REQUIRE(projection.runs().size() == 1);
}

TEST_CASE("unfinished and cancelled assistant content remains visible",
          "[transcript][failure][cancel]") {
  domain::TranscriptProjection projection;
  const auto inference = make_id<domain::InferenceId>("inference");
  const auto assistant = make_id<domain::MessageId>("assistant");
  REQUIRE(projection.apply(event(1, started())));
  REQUIRE(projection.apply(event(
      2, domain::InferenceStarted{inference, make_id<domain::ModelId>("model")})));
  REQUIRE(projection.apply(
      event(3, domain::AssistantContentStarted{assistant, inference})));
  REQUIRE(projection.apply(event(
      4, domain::AssistantContentDeltaAdded{
             assistant, inference, domain::TextBlock{"partial"}})));

  auto& streaming =
      std::get<domain::TranscriptMessage>(projection.items().back());
  REQUIRE(streaming.state == domain::TranscriptMessageState::streaming);

  REQUIRE(projection.apply(event(
      5, domain::InferenceCancelled{inference, std::string{"escape"}})));
  REQUIRE(projection.apply(
      event(6, domain::RunCancelled{std::string{"escape"}})));
  REQUIRE(projection.items().size() == 1);
  const auto& cancelled =
      std::get<domain::TranscriptMessage>(projection.items().back());
  REQUIRE(cancelled.state == domain::TranscriptMessageState::cancelled);
  REQUIRE(std::get<domain::TextBlock>(cancelled.content.front()).text ==
          "partial");
}

TEST_CASE("tool summaries enforce one ordered terminal lifecycle",
          "[transcript][tool][failure]") {
  domain::TranscriptProjection projection;
  const auto invocation = make_id<domain::InvocationId>("invocation");
  const domain::CapabilityScope scope{domain::Effect::read, "root", "/repo"};
  REQUIRE(projection.apply(event(1, started())));
  REQUIRE(projection.apply(event(
      2,
      domain::ToolProposed{invocation, "read", {"application/json", "{}"},
                           {domain::Effect::read}},
      {}, "run", invocation)));
  REQUIRE(projection.apply(event(
      3, domain::ToolPolicyDecided{invocation,
                                   domain::PolicyDecision::require_approval,
                                   {scope}, std::nullopt},
      {}, "run", invocation)));
  REQUIRE(projection.apply(event(
      4, domain::ToolApprovalRequested{invocation, {scope}}, {}, "run",
      invocation)));
  REQUIRE(projection.apply(event(
      5, domain::ToolApprovalDecided{invocation,
                                     domain::ApprovalDecision::approved,
                                     {scope}},
      {}, "run", invocation)));
  REQUIRE(projection.apply(
      event(6, domain::ToolStarted{invocation}, {}, "run", invocation)));
  REQUIRE(projection.apply(event(
      7,
      domain::ToolProgressed{invocation, {domain::TextBlock{"working"}}},
      {}, "run", invocation)));
  REQUIRE(projection.apply(event(
      8,
      domain::ToolResultRecorded{invocation, {domain::TextBlock{"done"}}},
      {}, "run", invocation)));

  const auto& summary =
      std::get<domain::TranscriptToolSummary>(projection.items().back());
  REQUIRE(summary.state == domain::TranscriptToolState::complete);
  REQUIRE(summary.progress.size() == 1);
  REQUIRE(summary.result.size() == 1);

  const auto duplicate = projection.apply(event(
      9,
      domain::ToolResultRecorded{invocation, {domain::TextBlock{"again"}}},
      {}, "run", invocation));
  REQUIRE_FALSE(duplicate);
  REQUIRE(projection.last_sequence() == 8);
  REQUIRE(std::get<domain::TranscriptToolSummary>(projection.items().back()) ==
          summary);
}

TEST_CASE("cancelled approval accepts one terminal tool error",
          "[transcript][tool][failure]") {
  domain::TranscriptProjection projection;
  const auto invocation = make_id<domain::InvocationId>("invocation");
  REQUIRE(projection.apply(event(1, started())));
  REQUIRE(projection.apply(event(
      2,
      domain::ToolProposed{invocation, "read", {"application/json", "{}"},
                           {domain::Effect::read}},
      {}, "run", invocation)));
  REQUIRE(projection.apply(event(
      3,
      domain::ToolPolicyDecided{invocation,
                                domain::PolicyDecision::require_approval, {},
                                std::nullopt},
      {}, "run", invocation)));
  REQUIRE(projection.apply(event(
      4, domain::ToolApprovalRequested{invocation, {}}, {}, "run",
      invocation)));
  REQUIRE(projection.apply(event(
      5,
      domain::ToolApprovalDecided{invocation,
                                  domain::ApprovalDecision::cancelled, {}},
      {}, "run", invocation)));
  const domain::DomainError cancelled{domain::ErrorCode::cancelled,
                                      "tool approval cancelled", false};
  REQUIRE(projection.apply(event(
      6, domain::ToolErrored{invocation, cancelled}, {}, "run", invocation)));
  const auto& summary =
      std::get<domain::TranscriptToolSummary>(projection.items().back());
  REQUIRE(summary.state == domain::TranscriptToolState::cancelled);
  REQUIRE(summary.error == cancelled);
  REQUIRE_FALSE(projection.apply(event(
      7, domain::ToolErrored{invocation, cancelled}, {}, "run", invocation)));
}

TEST_CASE("questions and artifacts reject unknown or invalid references",
          "[transcript][question][artifact][failure]") {
  domain::TranscriptProjection projection;
  const auto question = make_id<domain::QuestionId>("question");
  const auto artifact = make_id<domain::ArtifactId>("artifact");
  const auto user = make_id<domain::MessageId>("user");
  REQUIRE(projection.apply(event(1, started())));
  REQUIRE(projection.apply(event(
      2, domain::UserContentAdded{message("user", domain::Role::user, "hi")})));
  REQUIRE(projection.apply(event(
      3, domain::QuestionRequested{domain::QuestionDefinition{
             question, "Choose", domain::QuestionSelection::one,
             {{"yes", "Yes", std::nullopt}}, true, 1, 1,
             domain::QuestionOtherInput{"Other", std::nullopt, 4096}}})));

  auto invalid = projection.apply(event(
      4, domain::QuestionAnswered{
             domain::QuestionAnswer{question, {"missing"}, std::nullopt}},
      "bad-answer"));
  REQUIRE_FALSE(invalid);
  REQUIRE(projection.last_sequence() == 3);
  REQUIRE(projection.apply(event(
      4, domain::QuestionAnswered{
             domain::QuestionAnswer{question, {"yes"}, std::nullopt}},
      "answer")));

  invalid = projection.apply(event(
      5, domain::ArtifactReferenced{artifact, user}, "early-reference"));
  REQUIRE_FALSE(invalid);
  REQUIRE(invalid.error().code ==
          domain::TranscriptProjectionErrorCode::unknown_artifact);
  REQUIRE(projection.last_sequence() == 4);

  const domain::ArtifactMetadata metadata{artifact, "text/plain", 4,
                                          "sha256:abcd", std::nullopt,
                                          std::nullopt, std::nullopt};
  REQUIRE(projection.apply(
      event(5, domain::ArtifactCreated{metadata}, "artifact-created")));
  REQUIRE(projection.apply(event(
      6, domain::ArtifactReferenced{artifact, user}, "artifact-reference")));
  REQUIRE(std::holds_alternative<domain::TranscriptArtifactReference>(
      projection.items().back()));
  REQUIRE(std::get<domain::TranscriptMessage>(projection.items().front())
              .artifacts == std::vector{metadata});
}

TEST_CASE("verification evidence replays after its terminal invocation",
          "[transcript][verification]") {
  domain::TranscriptProjection projection;
  const auto invocation = make_id<domain::InvocationId>("verification-call");
  const auto artifact = make_id<domain::ArtifactId>("verification-output");
  const auto recorded = verification(invocation, artifact);
  const std::vector events{
      event(1, started()),
      event(2,
            domain::ToolProposed{invocation, "run_process",
                                 {"application/json", "{}"}, {}},
            {}, "run", invocation),
      event(3,
            domain::ToolPolicyDecided{invocation,
                                      domain::PolicyDecision::allow, {},
                                      std::nullopt},
            {}, "run", invocation),
      event(4, domain::ToolStarted{invocation}, {}, "run", invocation),
      event(5,
            domain::ArtifactCreated{{artifact, "text/plain", 6,
                                     "sha256:passed", invocation,
                                     std::nullopt, std::nullopt}},
            "artifact", "run", invocation),
      event(6,
            domain::ToolResultRecorded{invocation,
                                       {domain::TextBlock{"passed"}}},
            {}, "run", invocation),
      event(7, domain::VerificationEvidenceRecorded{recorded}, {}, "run",
            invocation),
  };
  for (const auto& value : events) REQUIRE(projection.apply(value));

  REQUIRE(std::holds_alternative<domain::TranscriptVerificationSummary>(
      projection.items().back()));
  REQUIRE(std::get<domain::TranscriptVerificationSummary>(
              projection.items().back())
              .evidence == recorded);
  const auto replayed = domain::TranscriptProjection::rebuild(events);
  REQUIRE(replayed);
  REQUIRE(replayed->items() == projection.items());

  const auto duplicate = projection.apply(event(
      8, domain::VerificationEvidenceRecorded{recorded}, {}, "run",
      invocation));
  REQUIRE_FALSE(duplicate);
  REQUIRE(projection.last_sequence() == 7);
}

TEST_CASE("verification evidence rejects live invocations and unknown artifacts",
          "[transcript][verification][failure]") {
  domain::TranscriptProjection projection;
  const auto invocation = make_id<domain::InvocationId>("verification-call");
  const auto artifact = make_id<domain::ArtifactId>("missing-output");
  REQUIRE(projection.apply(event(1, started())));
  REQUIRE(projection.apply(event(
      2,
      domain::ToolProposed{invocation, "run_process",
                           {"application/json", "{}"}, {}},
      {}, "run", invocation)));
  REQUIRE(projection.apply(event(
      3, domain::ToolPolicyDecided{invocation, domain::PolicyDecision::allow,
                                   {}, std::nullopt},
      {}, "run", invocation)));
  REQUIRE(projection.apply(
      event(4, domain::ToolStarted{invocation}, {}, "run", invocation)));
  auto rejected = projection.apply(event(
      5, domain::VerificationEvidenceRecorded{verification(invocation, artifact)},
      {}, "run", invocation));
  REQUIRE_FALSE(rejected);
  REQUIRE(projection.last_sequence() == 4);

  REQUIRE(projection.apply(event(
      5, domain::ToolResultRecorded{invocation, {domain::TextBlock{"passed"}}},
      "result", "run", invocation)));
  rejected = projection.apply(event(
      6, domain::VerificationEvidenceRecorded{verification(invocation, artifact)},
      "verification", "run", invocation));
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          domain::TranscriptProjectionErrorCode::unknown_artifact);
}

TEST_CASE("question identities are scoped to their tool invocation",
          "[transcript][question]") {
  domain::TranscriptProjection projection;
  const auto question = make_id<domain::QuestionId>("question");
  const auto first = make_id<domain::InvocationId>("first-call");
  const auto second = make_id<domain::InvocationId>("second-call");
  const domain::QuestionDefinition definition{
      question, "Choose", domain::QuestionSelection::one,
      {{"yes", "Yes", std::nullopt}}, true, 1, 1, std::nullopt};
  REQUIRE(projection.apply(event(1, started())));
  REQUIRE(projection.apply(event(2, domain::QuestionRequested{definition},
                                 {}, "run", first)));
  REQUIRE_FALSE(projection.apply(event(
      3, domain::QuestionAnswered{{question, {"yes"}, std::nullopt}}, {},
      "run", second)));
  REQUIRE(projection.last_sequence() == 2);
  REQUIRE(projection.apply(event(
      3, domain::QuestionAnswered{{question, {"yes"}, std::nullopt}}, {},
      "run", first)));
  REQUIRE(projection.apply(event(4, domain::QuestionRequested{definition},
                                 {}, "run", second)));
  REQUIRE(projection.apply(event(
      5, domain::QuestionCancelled{question, "cancelled"}, {}, "run",
      second)));

  REQUIRE(projection.items().size() == 2);
  REQUIRE(std::get<domain::TranscriptQuestionSummary>(projection.items()[0])
              .invocation_id == first);
  REQUIRE(std::get<domain::TranscriptQuestionSummary>(projection.items()[1])
              .invocation_id == second);
}

TEST_CASE("untrusted text sanitization is deterministic and UTF-8 safe",
          "[presentation][failure]") {
  const std::string input =
      std::string{"safe\r\n\x1b[31mred\x1b[0m\x01"} +
      std::string{"\xC0\xAF", 2} + "\tend";
  const auto clean = presentation::sanitize_untrusted_text(input);
  REQUIRE(clean);
  REQUIRE(clean->find('\x1b') == std::string::npos);
  REQUIRE(clean->find('\x01') == std::string::npos);
  REQUIRE(*clean == "safe\nred\xEF\xBF\xBD\xEF\xBF\xBD\tend");
}

TEST_CASE("Markdown-lite produces semantic spans without dropping literals",
          "[presentation][markdown]") {
  const auto document = presentation::tokenize_markdown_lite(
      "# Heading\r\n- **bold** and *italic*\n``code ` inside``\n```cpp\n"
      "value();\n```\nunterminated **marker");
  REQUIRE(document);
  REQUIRE(document->size() == 5);
  REQUIRE(presentation::has_semantic(
      document->front().front().semantic,
      presentation::TextSemantic::heading));
  REQUIRE(document->at(1).front().text == "\xE2\x80\xA2 ");
  REQUIRE(presentation::has_semantic(
      document->at(1).at(1).semantic,
      presentation::TextSemantic::strong));
  REQUIRE(presentation::has_semantic(
      document->at(2).front().semantic, presentation::TextSemantic::code));
  REQUIRE(presentation::has_semantic(
      document->at(3).front().semantic, presentation::TextSemantic::code));
  const auto plain = presentation::flatten(*document);
  REQUIRE(plain);
  REQUIRE(plain->find("unterminated **marker") != std::string::npos);

  const auto unfenced = presentation::tokenize_markdown_lite("```\nplain");
  REQUIRE(unfenced);
  const auto unfenced_plain = presentation::flatten(*unfenced);
  REQUIRE(unfenced_plain);
  REQUIRE(*unfenced_plain == "```\nplain");
}

TEST_CASE("Markdown-lite handles a bounded ten-megabyte line",
          "[presentation][performance]") {
  const std::string large(10U * 1024U * 1024U, 'x');
  const auto document = presentation::tokenize_markdown_lite(large);
  REQUIRE(document);
  REQUIRE(document->size() == 1);
  REQUIRE(document->front().size() == 1);
  REQUIRE(document->front().front().text.size() == large.size());

  const auto rejected = presentation::tokenize_markdown_lite(large, 1024);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code == presentation::TextErrorCode::input_too_large);
}
