#include <aiforge/runtime/conversation_summary_projection.hpp>
#include <aiforge/runtime/conversation_summary_sources.hpp>
#include <catch2/catch_test_macros.hpp>
#include <functional>

namespace {
using namespace aiforge;
template <typename T> auto id(const std::string& text) -> T {
  return T::from(text).value();
}
struct Fixture {
  domain::SessionEventLog log{id<domain::SessionId>("session")};
  auto append(std::string run, domain::RunEventPayload payload,
              std::uint32_t schema = 1) -> void {
    const auto sequence = log.last_sequence() + 1;
    REQUIRE(
        log.append({{id<domain::EventId>("event-" + std::to_string(sequence)),
                     id<domain::RunId>(run),
                     sequence,
                     schema,
                     domain::EventTimestamp{},
                     {},
                     {},
                     {}},
                    std::move(payload)}));
  }
  auto start(std::string run, domain::RunPurpose purpose) -> void {
    append(std::move(run),
           domain::RunStarted{id<domain::SurfaceId>("chat"),
                              id<domain::WorkspaceId>("chat"),
                              id<domain::PermissionProfileId>("observe"),
                              {},
                              {},
                              purpose,
                              {}},
           purpose == domain::RunPurpose::conversation ? 1 : 3);
  }
  auto intent() -> domain::ConversationSummaryIntent {
    start("source", domain::RunPurpose::conversation);
    append("source",
           domain::UserContentAdded{{id<domain::MessageId>("source-message"),
                                     domain::Role::user,
                                     {domain::TextBlock{"The door is blue."}},
                                     {}}});
    append("source", domain::RunCompleted{});
    auto prepared = runtime::prepare_conversation_summary_sources(
        {log, {id<domain::RunId>("source")}});
    REQUIRE(prepared);
    domain::ConversationSummaryIntent value{
        1,
        1,
        id<domain::ConversationSummaryId>("summary"),
        prepared->sources,
        id<domain::RunId>("producer"),
        id<domain::InferenceId>("inference"),
        id<domain::ModelId>("model"),
        id<domain::MessageId>("output"),
        "runtime-v1",
        {4096, 256, 0},
        512,
        1024,
        {}};
    REQUIRE(domain::seal_conversation_summary_intent(value));
    return value;
  }
  auto producer(const domain::ConversationSummaryIntent& intent,
                bool finish = true) -> void {
    start("producer", domain::RunPurpose::summary);
    append("producer",
           domain::ConversationSummaryGenerationIntentRecorded{intent});
    append("producer",
           domain::UserContentAdded{{id<domain::MessageId>("task"),
                                     domain::Role::user,
                                     {domain::TextBlock{"Summarize sources"}},
                                     {}}});
    append("producer", domain::RunCompletionRequested{});
    append("producer", domain::InferenceStarted{intent.producing_inference_id,
                                                intent.model_id});
    append("producer",
           domain::AssistantContentStarted{intent.output_message_id,
                                           intent.producing_inference_id});
    append("producer",
           domain::AssistantContentDeltaAdded{
               intent.output_message_id, intent.producing_inference_id,
               domain::TextBlock{"Door: blue."}});
    if (!finish) return;
    append("producer",
           domain::AssistantContentFinished{intent.output_message_id,
                                            intent.producing_inference_id});
    append("producer", domain::InferenceFinished{intent.producing_inference_id,
                                                 domain::FinishReason::stop});
    append("producer", domain::RunCompleted{});
  }
  auto publish(const domain::ConversationSummaryIntent& intent)
      -> domain::ConversationSummaryCandidate {
    auto draft = runtime::recover_conversation_summary_draft(log, intent);
    REQUIRE(draft);
    start("publication", domain::RunPurpose::control);
    const auto sequence = log.last_sequence() + 1;
    domain::ConversationSummaryCandidate candidate{
        1,
        log.session_id(),
        intent.summary_id,
        1,
        *intent.sources.source_digest,
        *intent.intent_digest,
        draft->output_event_id,
        draft->output_sequence,
        id<domain::EventId>("event-" + std::to_string(sequence)),
        sequence,
        domain::ConversationSummaryAuthor::model,
        {},
        draft->text,
        {}};
    REQUIRE(domain::seal_conversation_summary_candidate(candidate, intent));
    append("publication",
           domain::ConversationSummaryCandidatePublished{candidate});
    append("publication", domain::RunCompleted{});
    return candidate;
  }
  auto activate(const domain::ConversationSummaryIntent& intent,
                const domain::ConversationSummaryCandidate& candidate)
      -> domain::ConversationSummaryActivation {
    start("activation", domain::RunPurpose::control);
    const auto sequence = log.last_sequence() + 1;
    domain::ConversationSummaryActivation action{
        1,
        log.session_id(),
        {candidate.summary_id, candidate.revision, *candidate.candidate_digest},
        *intent.sources.source_digest,
        {id<domain::RunId>("source")},
        intent.sources.groups.front().entries.front().event_sequence,
        id<domain::EventId>("event-" + std::to_string(sequence)),
        sequence,
        {}};
    REQUIRE(domain::seal_conversation_summary_activation(action, candidate,
                                                         intent));
    append("activation", domain::ConversationSummaryActivated{0, action, {}});
    append("activation", domain::RunCompleted{});
    return action;
  }
};
auto changed(const domain::SessionEventLog& original,
             const std::function<void(domain::RunEvent&)>& change)
    -> domain::SessionEventLog {
  domain::SessionEventLog result{original.session_id()};
  for (auto event : original.events()) {
    change(event);
    REQUIRE(result.append(std::move(event)));
  }
  return result;
}
} // namespace

TEST_CASE("summary projection rejects unavailable snapshots and partial intent "
          "transactions",
          "[summary][failure]") {
  Fixture f;
  const auto intent = f.intent();
  f.producer(intent);
  CHECK_FALSE(runtime::recorded_conversation_summaries(
      f.log, f.log.last_sequence() + 1));
  CHECK_FALSE(runtime::recorded_conversation_summaries(f.log, 5));
  auto corrupt = changed(f.log, [](auto& event) {
    if (auto* value =
            std::get_if<domain::ConversationSummaryGenerationIntentRecorded>(
                &event.payload)) {
      value->intent.producing_run_id = id<domain::RunId>("forged");
      REQUIRE(domain::seal_conversation_summary_intent(value->intent));
    }
  });
  CHECK_FALSE(runtime::recorded_conversation_summaries(corrupt));
}

TEST_CASE(
    "summary intent requires the exact startup user and completion sequence",
    "[summary][failure]") {
  Fixture f;
  const auto intent = f.intent();
  f.producer(intent);
  int fault{};
  SECTION("missing user") {
    fault = 1;
  }
  SECTION("missing completion") {
    fault = 2;
  }
  SECTION("duplicate user") {
    fault = 3;
  }
  SECTION("reordered completion") {
    fault = 4;
  }
  auto corrupt = changed(f.log, [&](auto& event) {
    if (event.metadata.run_id != intent.producing_run_id) return;
    if (event.metadata.sequence == 6 && (fault == 1 || fault == 4))
      event.payload = domain::RunCompletionRequested{};
    if (event.metadata.sequence == 7 && fault == 2)
      event.payload = domain::UnknownEvent{"metadata.future"};
    if (event.metadata.sequence == 7 && (fault == 3 || fault == 4))
      event.payload =
          domain::UserContentAdded{{id<domain::MessageId>("duplicate"),
                                    domain::Role::user,
                                    {domain::TextBlock{"task"}},
                                    {}}};
  });
  CHECK_FALSE(runtime::recorded_conversation_summaries(corrupt));
}

TEST_CASE("summary draft never treats failed partial or truncated output as a "
          "candidate",
          "[summary][failure]") {
  Fixture f;
  const auto intent = f.intent();
  f.producer(intent);
  int fault{};
  SECTION("length") {
    fault = 1;
  }
  SECTION("refusal") {
    fault = 2;
  }
  SECTION("tool call") {
    fault = 3;
  }
  SECTION("wrong inference") {
    fault = 4;
  }
  SECTION("oversized") {
    fault = 5;
  }
  SECTION("cancelled") {
    fault = 6;
  }
  auto corrupt = changed(f.log, [&](auto& event) {
    if (auto* value = std::get_if<domain::InferenceFinished>(&event.payload)) {
      if (fault == 1) value->reason = domain::FinishReason::length;
      if (fault == 2) value->reason = domain::FinishReason::content_filter;
      if (fault == 3) value->reason = domain::FinishReason::tool_call;
    }
    if (auto* value =
            std::get_if<domain::AssistantContentFinished>(&event.payload);
        value != nullptr && fault == 4)
      value->inference_id = id<domain::InferenceId>("wrong");
    if (auto* value =
            std::get_if<domain::AssistantContentDeltaAdded>(&event.payload);
        value != nullptr && fault == 5)
      value->delta = domain::TextBlock{std::string(1025, 'x')};
    if (fault == 6 &&
        std::holds_alternative<domain::RunCompleted>(event.payload) &&
        event.metadata.run_id == intent.producing_run_id)
      event.payload = domain::RunCancelled{};
  });
  CHECK_FALSE(runtime::recover_conversation_summary_draft(corrupt, intent));
  CHECK_FALSE(runtime::recover_conversation_summary_draft(f.log, intent, 10));
}

TEST_CASE("summary publication rejects forged text creation metadata and "
          "partial transactions",
          "[summary][failure]") {
  Fixture f;
  const auto intent = f.intent();
  f.producer(intent);
  const auto candidate = f.publish(intent);
  CHECK_FALSE(runtime::recorded_conversation_summaries(
      f.log, candidate.created_sequence));
  auto corrupt = changed(f.log, [&](auto& event) {
    if (auto* value =
            std::get_if<domain::ConversationSummaryCandidatePublished>(
                &event.payload)) {
      SECTION("resealed forged text") {
        value->candidate.text = "Door: red.";
      }
      SECTION("forged creation") {
        value->candidate.created_event_id = id<domain::EventId>("invented");
      }
      REQUIRE(domain::seal_conversation_summary_candidate(value->candidate,
                                                          intent));
    }
  });
  CHECK_FALSE(runtime::recorded_conversation_summaries(corrupt));
}

TEST_CASE("summary replay refuses future control and output schemas",
          "[summary][failure]") {
  Fixture f;
  const auto intent = f.intent();
  f.producer(intent);
  f.publish(intent);
  auto corrupt = changed(f.log, [](auto& event) {
    if (std::holds_alternative<domain::ConversationSummaryCandidatePublished>(
            event.payload)) {
      event.payload = domain::UnknownEvent{
          "session.conversation_summary_candidate_published"};
      event.metadata.schema_version = 99;
    }
  });
  CHECK_FALSE(runtime::recorded_conversation_summaries(corrupt));
  corrupt = changed(f.log, [](auto& event) {
    if (std::holds_alternative<domain::AssistantContentFinished>(
            event.payload)) {
      event.payload = domain::UnknownEvent{"content.assistant_finished"};
      event.metadata.schema_version = 99;
    }
  });
  CHECK_FALSE(runtime::recorded_conversation_summaries(corrupt));
}

TEST_CASE(
    "summary activation and disabling bind exact policy and active version",
    "[summary][failure]") {
  Fixture f;
  const auto intent = f.intent();
  f.producer(intent);
  const auto candidate = f.publish(intent);
  const auto active = f.activate(intent, candidate);
  auto corrupt = changed(f.log, [](auto& event) {
    if (auto* value =
            std::get_if<domain::ConversationSummaryActivated>(&event.payload))
      value->previous_policy_revision = 1;
  });
  CHECK_FALSE(runtime::recorded_conversation_summaries(corrupt));
  f.start("disable", domain::RunPurpose::control);
  f.append("disable",
           domain::ConversationSummaryDisabled{
               1, active.candidate, id<domain::EventId>("wrong-activation")});
  f.append("disable", domain::RunCompleted{});
  CHECK_FALSE(runtime::recorded_conversation_summaries(f.log));
}

TEST_CASE(
    "durable output recovers unchanged across an intervening control append",
    "[summary][recovery]") {
  Fixture f;
  const auto intent = f.intent();
  f.producer(intent);
  const auto before =
      runtime::recover_conversation_summary_draft(f.log, intent);
  REQUIRE(before);
  f.start("policy", domain::RunPurpose::control);
  f.append("policy", domain::ConversationPolicySet{
                         0, {1, domain::ConversationMode::rolling, {}}});
  f.append("policy", domain::RunCompleted{});
  const auto after = runtime::recover_conversation_summary_draft(f.log, intent);
  REQUIRE(after);
  CHECK(*before == *after);
  const auto candidate = f.publish(intent);
  CHECK(candidate.created_sequence > before->output_sequence);
  const auto state = runtime::recorded_conversation_summaries(f.log);
  REQUIRE(state);
  REQUIRE(state->candidates.size() == 1);
  CHECK(state->active.empty());
  CHECK(state->policy_revision == 1);
  CHECK(state->candidates.front() == candidate);
}

TEST_CASE("summary generated candidate activates then disables without erasing "
          "history",
          "[summary][smoke]") {
  Fixture f;
  const auto intent = f.intent();
  f.producer(intent);
  const auto candidate = f.publish(intent);
  const auto active = f.activate(intent, candidate);
  const auto activated = runtime::recorded_conversation_summaries(f.log);
  REQUIRE(activated);
  REQUIRE(activated->active.size() == 1);
  CHECK(activated->active.front() == active);
  CHECK(activated->policy_revision == 1);
  f.start("disable", domain::RunPurpose::control);
  f.append("disable", domain::ConversationSummaryDisabled{
                          1, active.candidate, active.activation_event_id});
  f.append("disable", domain::RunCompleted{});
  const auto disabled = runtime::recorded_conversation_summaries(f.log);
  REQUIRE(disabled);
  CHECK(disabled->active.empty());
  CHECK(disabled->policy_revision == 2);
  CHECK(disabled->intents == activated->intents);
  CHECK(disabled->candidates == activated->candidates);
}

TEST_CASE(
    "publishing an edit makes earlier summary candidates stale for activation",
    "[summary][failure]") {
  Fixture f;
  const auto intent = f.intent();
  f.producer(intent);
  const auto original = f.publish(intent);
  f.start("edit", domain::RunPurpose::control);
  auto edited = original;
  edited.revision = 2;
  edited.author = domain::ConversationSummaryAuthor::user_edit;
  edited.edited_from = domain::ConversationSummaryVersion{
      original.summary_id, original.revision, *original.candidate_digest};
  edited.created_sequence = f.log.last_sequence() + 1;
  edited.created_event_id =
      id<domain::EventId>("event-" + std::to_string(edited.created_sequence));
  edited.text = "The door was blue; repainting is unfinished.";
  REQUIRE(
      domain::seal_conversation_summary_candidate(edited, intent, &original));
  f.append("edit", domain::ConversationSummaryCandidatePublished{edited});
  f.append("edit", domain::RunCompleted{});
  REQUIRE(runtime::recorded_conversation_summaries(f.log));
  f.activate(intent, original);
  CHECK_FALSE(runtime::recorded_conversation_summaries(f.log));
}
