#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/domain/event_log.hpp>
#include <aiforge/domain/run_projection.hpp>

namespace {

using namespace aiforge::domain;

template <typename IdType>
auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto metadata(const std::uint64_t sequence, std::string event = "event",
              std::string run = "run") -> EventMetadata {
  return EventMetadata{make_id<EventId>(std::move(event)),
                       make_id<RunId>(std::move(run)),
                       sequence,
                       1,
                       EventTimestamp{std::chrono::milliseconds{sequence}},
                       std::nullopt,
                       std::nullopt,
                       std::nullopt};
}

template <typename Payload>
auto event(const std::uint64_t sequence, Payload payload, std::string id = "event",
           std::string run = "run") -> RunEvent {
  return RunEvent{metadata(sequence, std::move(id), std::move(run)), std::move(payload)};
}

auto started() -> RunStarted {
  return RunStarted{make_id<SurfaceId>("surface"), make_id<WorkspaceId>("chat"),
                    make_id<PermissionProfileId>("observe"), std::nullopt};
}

auto provenance() -> RunProvenance {
  return RunProvenance{"0.10.0",
                       "venice",
                       std::nullopt,
                       make_id<ModelId>("model"),
                       CredentialSourceReference{
                           CredentialSourceKind::environment, "VENICE_API_KEY"},
                       {{"model",
                         "venice-model",
                         true,
                         ProvenanceSource::environment,
                         false,
                         {{ProvenanceSource::environment,
                           ProvenanceDisposition::selected, std::nullopt}}}},
                       {{"aiforge", "0.10.0"}},
                       {}};
}

}  // namespace

TEST_CASE("opaque IDs reject invalid input before it reaches an event", "[domain][failure]") {
  REQUIRE_FALSE(EventId::from(""));
  REQUIRE(EventId::from("").error() == IdError::empty);
  REQUIRE_FALSE(EventId::from(std::string(Id<EventIdTag>::max_size + 1, 'x')));
  REQUIRE(EventId::from("bad\nvalue").error() == IdError::control_character);
  REQUIRE(EventId::from("evt-01")->value() == "evt-01");
}

TEST_CASE("session event log rejects invalid envelopes and rewritten order",
          "[domain][failure]") {
  SessionEventLog log{make_id<SessionId>("session")};

  auto zero_sequence = event(0, UnknownEvent{"future"}, "zero");
  REQUIRE_FALSE(log.append(std::move(zero_sequence)));

  auto zero_schema = event(1, UnknownEvent{"future"}, "schema");
  zero_schema.metadata.schema_version = 0;
  REQUIRE_FALSE(log.append(std::move(zero_schema)));

  REQUIRE(log.append(event(1, started(), "one")));
  REQUIRE_FALSE(log.append(event(2, UnknownEvent{"future"}, "one")));
  REQUIRE_FALSE(log.append(event(1, UnknownEvent{"future"}, "two")));
  REQUIRE(log.events().size() == 1);
  REQUIRE(log.last_sequence() == 1);
}

TEST_CASE("run projection rejects events without a start and from another run",
          "[domain][failure]") {
  RunProjection projection;
  const auto inference = make_id<InferenceId>("inference");
  const auto message = make_id<MessageId>("message");

  REQUIRE_FALSE(projection.apply(event(
      1, AssistantContentDeltaAdded{message, inference, TextBlock{"orphan"}})));
  REQUIRE(projection.last_sequence() == 0);

  REQUIRE(projection.apply(event(1, UnknownEvent{"future.event"}, "unknown")));
  REQUIRE(projection.apply(event(2, started(), "start")));
  REQUIRE_FALSE(projection.apply(event(3, RunCompleted{}, "wrong", "other-run")));
  REQUIRE(projection.status() == RunStatus::running);
}

TEST_CASE("run projection requires persona provenance before run content",
          "[domain][persona][failure]") {
  const PersonaReference persona{
      make_id<PersonaId>("persona:reviewer"), "reviewer",
      "personas/reviewer.md", {"sha256", std::string(64, 'a'), 7}};
  const auto user = UserContentAdded{Message{
      make_id<MessageId>("user"), Role::user, {TextBlock{"prompt"}},
      std::nullopt}};

  RunProjection missing;
  REQUIRE(missing.apply(event(
      1, RunStarted{make_id<SurfaceId>("surface"),
                    make_id<WorkspaceId>("chat"),
                    make_id<PermissionProfileId>("observe"),
                    persona.persona_id},
      "start")));
  REQUIRE_FALSE(missing.apply(event(2, user, "user")));

  RunProjection ordered;
  REQUIRE(ordered.apply(event(
      1, RunStarted{make_id<SurfaceId>("surface"),
                    make_id<WorkspaceId>("chat"),
                    make_id<PermissionProfileId>("observe"),
                    persona.persona_id},
      "ordered-start")));
  REQUIRE(ordered.apply(event(
      2, PersonaSelectionRecorded{{PersonaSelectionAction::selected,
                                   PersonaSelectionSource::command_line,
                                   persona, std::nullopt}},
      "persona")));
  REQUIRE(ordered.apply(event(3, user, "ordered-user")));
}

TEST_CASE("run projection rejects illegal inference and terminal ordering",
          "[domain][failure]") {
  RunProjection projection;
  const auto inference = make_id<InferenceId>("inference");
  const auto message = make_id<MessageId>("assistant");

  REQUIRE(projection.apply(event(1, started(), "e1")));
  REQUIRE_FALSE(projection.apply(event(
      2, AssistantContentStarted{message, inference}, "e2")));
  REQUIRE(projection.apply(event(
      2, InferenceStarted{inference, make_id<ModelId>("model")}, "e2")));
  REQUIRE(projection.apply(event(3, AssistantContentStarted{message, inference}, "e3")));
  REQUIRE_FALSE(projection.apply(event(
      4, InferenceFinished{inference, FinishReason::stop}, "e4")));
  REQUIRE(projection.apply(event(
      4, AssistantContentDeltaAdded{message, inference, TextBlock{"partial"}}, "e4")));
  REQUIRE(projection.apply(event(5, AssistantContentFinished{message, inference}, "e5")));
  REQUIRE(projection.apply(event(6, InferenceFinished{inference, FinishReason::stop}, "e6")));
  REQUIRE(projection.apply(event(7, RunCompleted{}, "e7")));
  REQUIRE_FALSE(projection.apply(event(8, RunCancelRequested{}, "e8")));
  REQUIRE(projection.apply(event(8, UnknownEvent{"future.after-terminal"}, "e8")));
}

TEST_CASE("run provenance is recorded once, after a start, on a live run",
          "[domain][failure][provenance]") {
  RunProjection projection;

  REQUIRE_FALSE(projection.apply(event(1, RunProvenanceRecorded{provenance()},
                                       "orphan")));
  REQUIRE(projection.last_sequence() == 0);
  REQUIRE_FALSE(projection.provenance());

  REQUIRE(projection.apply(event(1, started(), "e1")));
  REQUIRE(projection.apply(event(2, RunProvenanceRecorded{provenance()}, "e2")));
  REQUIRE(projection.provenance() == provenance());

  auto second = provenance();
  second.backend_id = "other";
  REQUIRE_FALSE(projection.apply(event(3, RunProvenanceRecorded{second}, "e3")));
  REQUIRE(projection.provenance() == provenance());
  REQUIRE(projection.last_sequence() == 2);

  REQUIRE(projection.apply(event(3, RunCompleted{}, "e3")));
  REQUIRE_FALSE(projection.apply(event(4, RunProvenanceRecorded{provenance()},
                                       "e4")));
}

TEST_CASE("run provenance validation refuses secrets and malformed identity",
          "[domain][failure][provenance]") {
  REQUIRE(validate_run_provenance(provenance()));

  RunProvenanceLimits zero_limits;
  zero_limits.maximum_key_bytes = 0;
  REQUIRE(validate_run_provenance(provenance(), zero_limits).error().code ==
          RunProvenanceErrorCode::invalid_limits);

  auto sensitive = provenance();
  sensitive.configuration.front().sensitive = true;
  REQUIRE(validate_run_provenance(sensitive).error().code ==
          RunProvenanceErrorCode::sensitive_value_recorded);
  // A sensitive key may still record that a value resolved, and from where.
  sensitive.configuration.front().value.reset();
  REQUIRE(validate_run_provenance(sensitive));
  REQUIRE(sensitive.configuration.front().value_present);

  auto duplicated = provenance();
  duplicated.configuration.push_back(duplicated.configuration.front());
  REQUIRE(validate_run_provenance(duplicated).error().code ==
          RunProvenanceErrorCode::duplicate_key);

  auto bad_key = provenance();
  bad_key.configuration.front().key = "model key";
  REQUIRE(validate_run_provenance(bad_key).error().code ==
          RunProvenanceErrorCode::invalid_key);
  bad_key.configuration.front().key.clear();
  REQUIRE(validate_run_provenance(bad_key).error().code ==
          RunProvenanceErrorCode::invalid_key);

  auto inconsistent = provenance();
  inconsistent.configuration.front().value_present = false;
  REQUIRE(validate_run_provenance(inconsistent).error().code ==
          RunProvenanceErrorCode::invalid_key);

  auto oversized = provenance();
  oversized.configuration.front().value = std::string(4097, 'x');
  REQUIRE(validate_run_provenance(oversized).error().code ==
          RunProvenanceErrorCode::value_too_large);

  auto many_decisions = provenance();
  many_decisions.configuration.front().decisions.assign(
      17, {ProvenanceSource::file, ProvenanceDisposition::shadowed,
           ProvenanceDiagnosticCode::duplicate_source_value});
  REQUIRE(validate_run_provenance(many_decisions).error().code ==
          RunProvenanceErrorCode::too_many_entries);

  auto many_entries = provenance();
  many_entries.configuration.clear();
  for (std::size_t index = 0; index <= 256; ++index) {
    many_entries.configuration.push_back(
        {"key-" + std::to_string(index), std::nullopt, false, std::nullopt,
         false, {}});
  }
  REQUIRE(validate_run_provenance(many_entries).error().code ==
          RunProvenanceErrorCode::too_many_entries);
}

TEST_CASE("run provenance validation bounds identity, credentials, and tools",
          "[domain][failure][provenance]") {
  auto empty_version = provenance();
  empty_version.aiforge_version.clear();
  REQUIRE(validate_run_provenance(empty_version).error().code ==
          RunProvenanceErrorCode::invalid_identity);

  auto control_backend = provenance();
  control_backend.backend_id = "ven\nice";
  REQUIRE(validate_run_provenance(control_backend).error().code ==
          RunProvenanceErrorCode::invalid_identity);

  // A leaked credential is far likelier than a variable name to carry padding
  // or whitespace, so the locator charset excludes both.
  auto secret_shaped = provenance();
  secret_shaped.credential_source->identity = "sk-abc123==";
  REQUIRE(validate_run_provenance(secret_shaped).error().code ==
          RunProvenanceErrorCode::invalid_credential_source);
  secret_shaped.credential_source->identity = "VENICE API KEY";
  REQUIRE(validate_run_provenance(secret_shaped).error().code ==
          RunProvenanceErrorCode::invalid_credential_source);
  secret_shaped.credential_source->identity.clear();
  REQUIRE(validate_run_provenance(secret_shaped).error().code ==
          RunProvenanceErrorCode::invalid_credential_source);

  auto bad_component = provenance();
  bad_component.components.front().version = "1.0 (dev)";
  REQUIRE(validate_run_provenance(bad_component).error().code ==
          RunProvenanceErrorCode::invalid_component);

  auto duplicate_tool = provenance();
  duplicate_tool.tools = {{"read", {Effect::read}, {}},
                          {"read", {Effect::read}, {}}};
  REQUIRE(validate_run_provenance(duplicate_tool).error().code ==
          RunProvenanceErrorCode::duplicate_tool);

  auto empty_tool = provenance();
  empty_tool.tools = {{"", {}, {}}};
  REQUIRE(validate_run_provenance(empty_tool).error().code ==
          RunProvenanceErrorCode::invalid_tool);

  auto wide_tool = provenance();
  wide_tool.tools = {{"read", std::vector<Effect>(17, Effect::read), {}}};
  REQUIRE(validate_run_provenance(wide_tool).error().code ==
          RunProvenanceErrorCode::invalid_tool);

  auto exhausted = provenance();
  exhausted.configuration.clear();
  for (std::size_t index = 0; index < 32; ++index) {
    exhausted.configuration.push_back({"key-" + std::to_string(index),
                                       std::string(4000, 'x'), true,
                                       std::nullopt, false, {}});
  }
  REQUIRE(validate_run_provenance(exhausted).error().code ==
          RunProvenanceErrorCode::resource_exhausted);
}

TEST_CASE("usage aggregation detects overflow without partially applying the event",
          "[domain][failure]") {
  RunProjection projection;
  const auto inference = make_id<InferenceId>("inference");

  REQUIRE(projection.apply(event(1, started(), "e1")));
  REQUIRE(projection.apply(event(
      2, InferenceStarted{inference, make_id<ModelId>("model")}, "e2")));
  REQUIRE(projection.apply(event(
      3, UsageRecorded{inference,
                       Usage{std::numeric_limits<std::uint64_t>::max(), 1, 2, 3}},
      "e3")));
  const auto failed = projection.apply(event(
      4, UsageRecorded{inference, Usage{1, 0, 0, 0}}, "e4"));
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().code == ProjectionErrorCode::usage_overflow);
  REQUIRE(projection.last_sequence() == 3);
  REQUIRE(projection.usage().input_tokens == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("cancelled inference keeps partial assistant evidence", "[domain]") {
  RunProjection projection;
  const auto inference = make_id<InferenceId>("inference");
  const auto message = make_id<MessageId>("assistant");

  REQUIRE(projection.apply(event(1, started(), "e1")));
  REQUIRE(projection.apply(event(
      2, UserContentAdded{Message{make_id<MessageId>("user"), Role::user,
                                  {TextBlock{"hello"}}, std::nullopt}},
      "e2")));
  REQUIRE(projection.apply(event(
      3, InferenceStarted{inference, make_id<ModelId>("model")}, "e3")));
  REQUIRE(projection.apply(event(4, AssistantContentStarted{message, inference}, "e4")));
  REQUIRE(projection.apply(event(
      5, AssistantContentDeltaAdded{message, inference, TextBlock{"partial"}}, "e5")));
  REQUIRE(projection.apply(event(
      6, InferenceCancelled{inference, std::string{"user request"}}, "e6")));
  REQUIRE(projection.messages().back().complete);
  REQUIRE(projection.messages().back().content ==
          std::vector<ContentBlock>{TextBlock{"partial"}});
}

TEST_CASE("all north-star event families have typed payloads", "[domain]") {
  const auto inference = make_id<InferenceId>("inference");
  const auto invocation = make_id<InvocationId>("invocation");
  const auto question = make_id<QuestionId>("question");
  const auto artifact = make_id<ArtifactId>("artifact");
  const auto view = make_id<ViewId>("view");
  const auto message = make_id<MessageId>("message");
  const DomainError error{ErrorCode::backend, "redacted", true};
  const CapabilityScope scope{Effect::read, "root", "/workspace"};
  const QuestionDefinition definition{question, "Choose", QuestionSelection::one,
                                      {{"yes", "Yes", std::nullopt}}, true, 1,
                                      1,
                                      QuestionOtherInput{"Other", std::nullopt,
                                                         4096}};
  const ArtifactMetadata artifact_metadata{artifact, "text/plain", 3, "sha256:abc",
                                           invocation, std::nullopt, std::nullopt};

  const std::vector<RunEventPayload> payloads{
      started(),
      RunProvenanceRecorded{provenance()},
      RunAwaitingInput{question},
      RunResumed{question},
      RunCompletionRequested{},
      RunCompleted{},
      RunFailed{error},
      RunCancelRequested{std::nullopt},
      RunCancelled{std::nullopt},
      UserContentAdded{Message{message, Role::user, {TextBlock{"text"}}, std::nullopt}},
      AssistantContentStarted{message, inference},
      AssistantContentDeltaAdded{message, inference, TextBlock{"delta"}},
      AssistantContentFinished{message, inference},
      InferenceStarted{inference, make_id<ModelId>("model")},
      ReasoningMetadataAdded{inference, std::nullopt, {{"visibility", "summary"}}},
      UsageRecorded{inference, Usage{1, 2, 3, 4}},
      InferenceFinished{inference, FinishReason::stop},
      InferenceFailed{inference, error},
      InferenceCancelled{inference, std::nullopt},
      ToolProposed{invocation, "read", {"application/json", "{}"}, {Effect::read}},
      ToolPolicyDecided{invocation, PolicyDecision::allow, {scope}, std::nullopt},
      ToolApprovalRequested{invocation, {scope}},
      ToolApprovalDecided{invocation, ApprovalDecision::approved, {scope}},
      ToolStarted{invocation},
      ToolProgressed{invocation, {TextBlock{"working"}}},
      ToolResultRecorded{invocation, {TextBlock{"done"}}},
      ToolErrored{invocation, error},
      QuestionRequested{definition},
      QuestionAnswered{QuestionAnswer{question, {"yes"}, std::nullopt}},
      QuestionCancelled{question, std::nullopt},
      ArtifactCreated{artifact_metadata},
      ArtifactReferenced{artifact, message},
      ArtifactDisplayed{artifact, view, "right-pane"},
      ArtifactRemovedFromView{artifact, view},
      ChildRunCreated{make_id<RunId>("child")},
      InterRunMessageSent{make_id<RunId>("target"), {TextBlock{"message"}}},
      UnknownEvent{"future.event"},
  };

  REQUIRE(payloads.size() == 37);
  REQUIRE(std::holds_alternative<RunStarted>(payloads.front()));
  REQUIRE(std::holds_alternative<UnknownEvent>(payloads.back()));
}
