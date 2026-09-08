#pragma once
#include <aiforge/runtime/conversation_context.hpp>
#include <aiforge/runtime/conversation_summary_context.hpp>
#include <aiforge/runtime/conversation_summary_sources.hpp>
#include <aiforge/runtime/session_context.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>

namespace summary_context_test {
using namespace aiforge;
template <typename Id> auto id(const std::string& text) -> Id {
  return Id::from(text).value();
}
struct Summary {
  domain::ConversationSummaryIntent intent;
  domain::ConversationSummaryCandidate candidate;
};
struct Fixture {
  domain::SessionEventLog log{id<domain::SessionId>("session")};
  std::uint64_t revision{};
  auto add(const std::string& run, domain::RunEventPayload payload) -> void {
    const auto seq = log.last_sequence() + 1;
    REQUIRE(log.append(
        {{id<domain::EventId>("event-" + std::to_string(seq)),
          id<domain::RunId>(run),
          seq,
          std::holds_alternative<domain::RunStarted>(payload) ? 3U : 1U,
          {},
          {},
          {},
          {}},
         std::move(payload)}));
  }
  auto start(const std::string& run, domain::RunPurpose purpose) -> void {
    add(run, domain::RunStarted{id<domain::SurfaceId>("chat"),
                                id<domain::WorkspaceId>("chat"),
                                id<domain::PermissionProfileId>("observe"),
                                {},
                                {},
                                purpose});
  }
  auto source(const std::string& name, std::string text = "Original facts.")
      -> void {
    start(name, domain::RunPurpose::conversation);
    add(name, domain::UserContentAdded{{id<domain::MessageId>(name + "-user"),
                                        domain::Role::user,
                                        {domain::TextBlock{std::move(text)}},
                                        {}}});
    add(name, domain::RunCompleted{});
  }
  auto summary(const std::string& name, std::vector<domain::RunId> runs,
               std::string text = "Reviewed facts.") -> Summary {
    auto sources =
        runtime::prepare_conversation_summary_sources({log, std::move(runs)});
    REQUIRE(sources);
    domain::ConversationSummaryIntent intent{
        1,
        1,
        id<domain::ConversationSummaryId>(name),
        std::move(sources->sources),
        id<domain::RunId>(name + "-producer"),
        id<domain::InferenceId>(name + "-inference"),
        id<domain::ModelId>("model"),
        id<domain::MessageId>(name + "-output"),
        "test",
        {100000, 1000, 0},
        sources->estimated_tokens + 512,
        4096,
        {}};
    REQUIRE(domain::seal_conversation_summary_intent(intent));
    const auto run = name + "-producer";
    start(run, domain::RunPurpose::summary);
    add(run, domain::ConversationSummaryGenerationIntentRecorded{intent});
    add(run, domain::UserContentAdded{{id<domain::MessageId>(name + "-task"),
                                       domain::Role::user,
                                       {domain::TextBlock{"Prepare handoff."}},
                                       {}}});
    add(run, domain::RunCompletionRequested{});
    add(run, domain::InferenceStarted{intent.producing_inference_id,
                                      intent.model_id});
    add(run, domain::AssistantContentStarted{intent.output_message_id,
                                             intent.producing_inference_id});
    add(run, domain::AssistantContentDeltaAdded{
                 intent.output_message_id, intent.producing_inference_id,
                 domain::TextBlock{std::move(text)}});
    add(run, domain::AssistantContentFinished{intent.output_message_id,
                                              intent.producing_inference_id});
    add(run, domain::InferenceFinished{intent.producing_inference_id,
                                       domain::FinishReason::stop});
    add(run, domain::RunCompleted{});
    auto draft = runtime::recover_conversation_summary_draft(log, intent);
    REQUIRE(draft);
    const auto publication = name + "-publication";
    start(publication, domain::RunPurpose::control);
    const auto seq = log.last_sequence() + 1;
    domain::ConversationSummaryCandidate candidate{
        1,
        log.session_id(),
        intent.summary_id,
        1,
        *intent.sources.source_digest,
        *intent.intent_digest,
        draft->output_event_id,
        draft->output_sequence,
        id<domain::EventId>("event-" + std::to_string(seq)),
        seq,
        domain::ConversationSummaryAuthor::model,
        {},
        draft->text,
        {}};
    REQUIRE(domain::seal_conversation_summary_candidate(candidate, intent));
    add(publication, domain::ConversationSummaryCandidatePublished{candidate});
    add(publication, domain::RunCompleted{});
    return {std::move(intent), std::move(candidate)};
  }
  auto activate(const Summary& summary)
      -> domain::ConversationSummaryActivation {
    const auto run = "activation-" + std::to_string(log.last_sequence());
    start(run, domain::RunPurpose::control);
    const auto seq = log.last_sequence() + 1;
    std::vector<domain::RunId> coverage;
    for (const auto& group : summary.intent.sources.groups)
      coverage.push_back(group.run_id);
    domain::ConversationSummaryActivation value{
        1,
        log.session_id(),
        {summary.candidate.summary_id, summary.candidate.revision,
         *summary.candidate.candidate_digest},
        summary.candidate.source_digest,
        std::move(coverage),
        summary.intent.sources.groups.front().entries.front().event_sequence,
        id<domain::EventId>("event-" + std::to_string(seq)),
        seq,
        {}};
    REQUIRE(domain::seal_conversation_summary_activation(
        value, summary.candidate, summary.intent));
    add(run, domain::ConversationSummaryActivated{revision++, value, {}});
    add(run, domain::RunCompleted{});
    return value;
  }
  auto disable(const domain::ConversationSummaryActivation& activation)
      -> void {
    const auto run = "disable-" + std::to_string(log.last_sequence());
    start(run, domain::RunPurpose::control);
    add(run,
        domain::ConversationSummaryDisabled{revision++, activation.candidate,
                                            activation.activation_event_id});
    add(run, domain::RunCompleted{});
  }
  auto policy(domain::ConversationMode mode,
              std::vector<domain::RunId> pins = {}) -> void {
    const auto run = "policy-" + std::to_string(log.last_sequence());
    start(run, domain::RunPurpose::control);
    const auto previous = revision++;
    add(run, domain::ConversationPolicySet{previous,
                                           {revision, mode, std::move(pins)}});
    add(run, domain::RunCompleted{});
  }
  auto request(std::uint64_t first_order = 1) const
      -> runtime::ConversationContextRequest {
    return {
        log, id<domain::ModelId>("model"), {100000, 16, 7}, 10, first_order, {},
        {}};
  }
};
inline auto mandatory() -> domain::ContextBuildInput {
  return {{100000, 16, 7},
          {{id<domain::ContextEntryId>("runtime-entry"),
            domain::InstructionLayer::application_runtime,
            domain::InstructionOperation::add,
            {},
            domain::Message{id<domain::MessageId>("runtime-message"),
                            domain::Role::system,
                            {domain::TextBlock{"Runtime contract."}},
                            {}},
            {id<domain::ContextSourceId>("runtime-source"), {}, {}},
            0,
            1,
            10}},
          {{id<domain::ContextEntryId>("user-entry"),
            domain::ContextContentKind::conversation,
            {id<domain::MessageId>("current-user"),
             domain::Role::user,
             {domain::TextBlock{"Next task."}},
             {}},
            {id<domain::ContextSourceId>("current-source"), {}, {}},
            1,
            10}}};
}
} // namespace summary_context_test
