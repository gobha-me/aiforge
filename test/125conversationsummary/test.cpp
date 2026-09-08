#include <aiforge/domain/conversation_summary.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace aiforge::domain;
using Code = ConversationSummaryErrorCode;
template <typename T> auto id(const std::string& text) -> T {
  return T::from(text).value();
}
auto sources(std::string prefix = "source", std::uint64_t first = 2)
    -> ConversationSummarySources {
  Message message{
      id<MessageId>(prefix + "-message"), Role::user, {TextBlock{"facts"}}, {}};
  const auto digest = normalized_conversation_message_digest(message).value();
  ConversationAdmittedEntry entry{
      id<EventId>(prefix + "-event"),
      first,
      id<ContextEntryId>(prefix + "-entry"),
      message.message_id,
      {id<ContextSourceId>(prefix + "-provenance"), {}, {}},
      1,
      5,
      ContextContentKind::conversation,
      digest};
  ConversationSummarySources result{1,
                                    1,
                                    id<SessionId>("session"),
                                    10,
                                    {{id<RunId>(prefix + "-run"),
                                      id<EventId>(prefix + "-terminal"),
                                      first + 2,
                                      {entry}}},
                                    {}};
  REQUIRE(seal_conversation_summary_sources(result));
  return result;
}
auto intent(ConversationSummarySources source = sources(),
            std::string name = "summary") -> ConversationSummaryIntent {
  ConversationSummaryIntent result{1,
                                   1,
                                   id<ConversationSummaryId>(name),
                                   std::move(source),
                                   id<RunId>(name + "-producer"),
                                   id<InferenceId>(name + "-inference"),
                                   id<ModelId>("model"),
                                   id<MessageId>(name + "-output"),
                                   "runtime-v1",
                                   {128, 16, 0},
                                   32,
                                   1024,
                                   {}};
  REQUIRE(seal_conversation_summary_intent(result));
  return result;
}
auto candidate(const ConversationSummaryIntent& source,
               std::uint64_t sequence = 11) -> ConversationSummaryCandidate {
  const auto prefix = std::string{source.summary_id.value()};
  ConversationSummaryCandidate result{1,
                                      source.sources.session_id,
                                      source.summary_id,
                                      1,
                                      *source.sources.source_digest,
                                      *source.intent_digest,
                                      id<EventId>(prefix + "-output-event"),
                                      sequence,
                                      id<EventId>(prefix + "-candidate-event"),
                                      sequence + 1,
                                      ConversationSummaryAuthor::model,
                                      {},
                                      "Important facts",
                                      {}};
  REQUIRE(seal_conversation_summary_candidate(result, source));
  return result;
}
auto activation(const ConversationSummaryIntent& source,
                const ConversationSummaryCandidate& text,
                std::uint64_t sequence = 13) -> ConversationSummaryActivation {
  std::vector<RunId> runs;
  for (const auto& group : source.sources.groups)
    runs.push_back(group.run_id);
  ConversationSummaryActivation result{
      1,
      source.sources.session_id,
      {text.summary_id, text.revision, *text.candidate_digest},
      *source.sources.source_digest,
      std::move(runs),
      source.sources.groups.front().entries.front().event_sequence,
      id<EventId>(std::string{text.summary_id.value()} + "-activation"),
      sequence,
      {}};
  REQUIRE(seal_conversation_summary_activation(result, text, source));
  return result;
}
} // namespace

TEST_CASE("summary sources reject missing unsupported or partial metadata",
          "[summary][failure]") {
  auto value = sources();
  SECTION("empty coverage") {
    value.groups.clear();
  }
  SECTION("future completion") {
    value.groups[0].terminal_sequence = 11;
  }
  SECTION("completion before content") {
    value.groups[0].terminal_sequence = 2;
  }
  SECTION("source without user classification") {
    value.groups[0].entries[0].kind = ContextContentKind::tool_result;
  }
  SECTION("event after completion") {
    value.groups[0].entries[0].event_sequence = 5;
  }
  SECTION("zero provider order") {
    value.groups[0].entries[0].order = 0;
  }
  SECTION("unknown kind") {
    value.groups[0].entries[0].kind = ContextContentKind::unknown;
  }
  SECTION("empty provenance") {
    value.groups[0].entries[0].provenance.source_location = "";
  }
  REQUIRE_FALSE(seal_conversation_summary_sources(value));
}

TEST_CASE("summary source bounds and versions fail before publication",
          "[summary][failure]") {
  auto value = sources();
  SECTION("schema") {
    value.version = 2;
  }
  SECTION("estimator") {
    value.estimator_version = 2;
  }
  SECTION("groups") {
    value.groups.resize(summary_maximum_groups + 1, value.groups[0]);
  }
  SECTION("entries") {
    value.groups[0].entries.resize(summary_maximum_entries + 1,
                                   value.groups[0].entries[0]);
  }
  SECTION("message bytes") {
    value.groups[0].entries[0].message_digest.byte_size =
        summary_maximum_source_bytes + 1;
  }
  SECTION("bad digest") {
    value.groups[0].entries[0].message_digest.value = "invalid";
  }
  REQUIRE_FALSE(seal_conversation_summary_sources(value));
}

TEST_CASE("summary sources seal terminal identity and content coverage",
          "[summary][failure]") {
  auto value = sources();
  SECTION("terminal identity") {
    value.groups[0].terminal_event_id = id<EventId>("changed");
  }
  SECTION("terminal sequence") {
    ++value.groups[0].terminal_sequence;
  }
  SECTION("session") {
    value.session_id = id<SessionId>("foreign");
  }
  SECTION("snapshot") {
    ++value.snapshot_sequence;
  }
  SECTION("message digest") {
    value.groups[0].entries[0].message_digest.value.assign(64, 'a');
  }
  SECTION("estimate") {
    ++value.groups[0].entries[0].estimated_tokens;
  }
  REQUIRE_FALSE(validate_conversation_summary_sources(value));
}

TEST_CASE("summary source completion order is independent of provider order",
          "[summary]") {
  auto value = sources();
  value.groups[0].terminal_sequence = 8;
  for (std::uint64_t index = 0; index < 2; ++index) {
    auto entry = value.groups[0].entries[0];
    const auto suffix = std::to_string(index);
    entry.completed_event_id = id<EventId>("extra-event-" + suffix);
    entry.entry_id = id<ContextEntryId>("extra-entry-" + suffix);
    entry.message_id = id<MessageId>("extra-message-" + suffix);
    entry.provenance.source_id = id<ContextSourceId>("extra-source-" + suffix);
    entry.event_sequence = index == 0 ? 5 : 3;
    entry.order = index + 2;
    entry.kind = index == 0 ? ContextContentKind::conversation
                            : ContextContentKind::tool_result;
    value.groups[0].entries.push_back(std::move(entry));
  }
  REQUIRE(seal_conversation_summary_sources(value));
  CHECK(validate_conversation_summary_sources(value));
  SECTION("duplicate completion identity") {
    value.groups[0].entries[1].completed_event_id =
        value.groups[0].terminal_event_id;
    REQUIRE_FALSE(seal_conversation_summary_sources(value));
  }
  SECTION("duplicate completion sequence") {
    value.groups[0].entries[1].event_sequence = 8;
    REQUIRE_FALSE(seal_conversation_summary_sources(value));
  }
  SECTION("token overflow") {
    value.groups[0].entries[1].estimated_tokens =
        std::numeric_limits<std::uint64_t>::max();
    REQUIRE_FALSE(seal_conversation_summary_sources(value));
  }
}

TEST_CASE("summary intent validates source producer and exact capacity",
          "[summary][failure]") {
  auto value = intent();
  SECTION("stale source seal") {
    ++value.sources.snapshot_sequence;
  }
  SECTION("same source run") {
    value.producing_run_id = value.sources.groups[0].run_id;
  }
  SECTION("same output message") {
    value.output_message_id = value.sources.groups[0].entries[0].message_id;
  }
  SECTION("source cost omitted") {
    value.estimated_input_tokens = 4;
  }
  SECTION("input overflow") {
    value.capacity.reserved_output_tokens =
        std::numeric_limits<std::uint64_t>::max();
  }
  SECTION("capacity below fit") {
    value.capacity.context_window_tokens = 47;
  }
  SECTION("zero output") {
    value.capacity.reserved_output_tokens = 0;
  }
  SECTION("zero bytes") {
    value.maximum_output_bytes = 0;
  }
  SECTION("oversized output") {
    value.maximum_output_bytes = summary_maximum_text_bytes + 1;
  }
  SECTION("unsafe runtime version") {
    value.runtime_version = "runtime\nversion";
  }
  REQUIRE_FALSE(seal_conversation_summary_intent(value));
}

TEST_CASE("summary intent seals model inference and output bounds",
          "[summary][failure]") {
  auto value = intent();
  SECTION("model") {
    value.model_id = id<ModelId>("other");
  }
  SECTION("inference") {
    value.producing_inference_id = id<InferenceId>("other");
  }
  SECTION("runtime version") {
    value.runtime_version = "other";
  }
  SECTION("output identity") {
    value.output_message_id = id<MessageId>("other");
  }
  SECTION("output bound") {
    ++value.maximum_output_bytes;
  }
  REQUIRE_FALSE(validate_conversation_summary_intent(value));
}

TEST_CASE("summary candidate rejects invalid text scope and output provenance",
          "[summary][failure]") {
  const auto source = intent();
  auto value = candidate(source);
  SECTION("foreign session") {
    value.session_id = id<SessionId>("foreign");
  }
  SECTION("wrong summary") {
    value.summary_id = id<ConversationSummaryId>("other");
  }
  SECTION("output before source snapshot") {
    value.output_sequence = 10;
  }
  SECTION("creation before output") {
    value.created_sequence = 11;
  }
  SECTION("same event identity") {
    value.created_event_id = value.output_event_id;
  }
  SECTION("source output identity") {
    value.output_event_id = source.sources.groups[0].terminal_event_id;
  }
  SECTION("source creation identity") {
    value.created_event_id =
        source.sources.groups[0].entries[0].completed_event_id;
  }
  SECTION("empty text") {
    value.text.clear();
  }
  SECTION("oversized text") {
    value.text.assign(source.maximum_output_bytes + 1, 'x');
  }
  SECTION("unsafe control") {
    value.text = "facts\x1b[0m";
  }
  SECTION("malformed UTF8") {
    value.text = std::string(1, static_cast<char>(0xff));
  }
  SECTION("unknown author") {
    value.author = static_cast<ConversationSummaryAuthor>(99);
  }
  REQUIRE_FALSE(seal_conversation_summary_candidate(value, source));
}

TEST_CASE(
    "summary edits retain generation provenance and exact previous version",
    "[summary]") {
  const auto source = intent();
  const auto original = candidate(source);
  auto edited = original;
  edited.revision = 2;
  edited.author = ConversationSummaryAuthor::user_edit;
  edited.edited_from = ConversationSummaryVersion{
      original.summary_id, original.revision, *original.candidate_digest};
  edited.created_event_id = id<EventId>("edit-event");
  edited.created_sequence = 14;
  edited.text = "Reviewed facts 雪";
  REQUIRE_FALSE(seal_conversation_summary_candidate(edited, source));
  REQUIRE(seal_conversation_summary_candidate(edited, source, &original));
  CHECK(validate_conversation_summary_candidate(edited, source, &original));
  CHECK(edited.output_event_id == original.output_event_id);
  SECTION("stale parent digest") {
    edited.edited_from->candidate_digest.value.assign(64, 'a');
    REQUIRE_FALSE(
        seal_conversation_summary_candidate(edited, source, &original));
  }
  SECTION("revision jump") {
    edited.revision = 3;
    REQUIRE_FALSE(
        seal_conversation_summary_candidate(edited, source, &original));
  }
  SECTION("changed original output") {
    edited.output_event_id = id<EventId>("other-output");
    REQUIRE_FALSE(
        seal_conversation_summary_candidate(edited, source, &original));
  }
  SECTION("changed sealed parent") {
    auto changed = original;
    changed.text = "unsealed";
    REQUIRE_FALSE(
        seal_conversation_summary_candidate(edited, source, &changed));
  }
  SECTION("text changed after sealing") {
    edited.text += " more";
    REQUIRE_FALSE(
        validate_conversation_summary_candidate(edited, source, &original));
  }
}

TEST_CASE("summary activation binds exact candidate coverage and chronology",
          "[summary][failure]") {
  const auto source = intent();
  const auto text = candidate(source);
  auto value = activation(source, text);
  SECTION("substituted source") {
    value.covered_run_ids[0] = id<RunId>("foreign-run");
  }
  SECTION("duplicated source") {
    value.covered_run_ids.push_back(value.covered_run_ids[0]);
  }
  SECTION("candidate revision") {
    ++value.candidate.revision;
  }
  SECTION("anchor") {
    ++value.source_anchor_sequence;
  }
  SECTION("creation sequence") {
    value.activation_sequence = text.created_sequence;
  }
  SECTION("source activation identity") {
    value.activation_event_id = source.sources.groups[0].terminal_event_id;
  }
  SECTION("foreign scope") {
    value.session_id = id<SessionId>("foreign");
  }
  REQUIRE_FALSE(seal_conversation_summary_activation(value, text, source));
}

TEST_CASE(
    "summary replacement requires explicit exact supersession and is immutable",
    "[summary]") {
  const auto first_intent = intent();
  const auto first_text = candidate(first_intent);
  std::vector<ConversationSummaryActivation> active{
      activation(first_intent, first_text)};
  const auto second_intent = intent(sources(), "replacement");
  const auto second_text = candidate(second_intent, 20);
  const auto next = activation(second_intent, second_text, 22);
  const auto before = active;
  const auto overlap =
      validate_conversation_summary_replacement(active, {}, next);
  REQUIRE_FALSE(overlap);
  CHECK(overlap.error().code == Code::overlap);
  CHECK(active == before);
  std::vector<ConversationSummaryVersion> replaced{active[0].candidate};
  CHECK(validate_conversation_summary_replacement(active, replaced, next));
  CHECK(active == before);
  SECTION("stale replacement reference") {
    ++replaced[0].revision;
    REQUIRE_FALSE(
        validate_conversation_summary_replacement(active, replaced, next));
  }
  SECTION("duplicate replacement") {
    replaced.push_back(replaced[0]);
    REQUIRE_FALSE(
        validate_conversation_summary_replacement(active, replaced, next));
  }
  SECTION("unknown replacement") {
    replaced[0].summary_id = id<ConversationSummaryId>("missing");
    REQUIRE_FALSE(
        validate_conversation_summary_replacement(active, replaced, next));
  }
  SECTION("bounded active set") {
    active.resize(summary_maximum_active + 1, active[0]);
    const auto result =
        validate_conversation_summary_replacement(active, replaced, next);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::resource_exhausted);
  }
  SECTION("disjoint original runs coexist") {
    const auto other_intent =
        intent(sources("other-source", 7), "other-summary");
    const auto other_text = candidate(other_intent, 20);
    const auto other = activation(other_intent, other_text, 22);
    CHECK(validate_conversation_summary_replacement(active, {}, other));
  }
}

TEST_CASE("summary seals are deterministic and failed resealing preserves "
          "prior state",
          "[summary]") {
  auto source = intent();
  source.capacity.context_window_tokens = 48;
  REQUIRE(seal_conversation_summary_intent(source));
  const auto original = source;
  REQUIRE(seal_conversation_summary_intent(source));
  CHECK(source == original);
  source.maximum_output_bytes = 0;
  const auto before = source.intent_digest;
  REQUIRE_FALSE(seal_conversation_summary_intent(source));
  CHECK(source.intent_digest == before);
}

TEST_CASE("summary sources bound aggregate content and manifest metadata",
          "[summary][failure]") {
  auto value = sources();
  auto entry = value.groups.front().entries.front();
  std::size_t count{};
  SECTION("aggregate source bytes") {
    count = 2;
    entry.message_digest.byte_size = summary_maximum_source_bytes / 2 + 1;
  }
  SECTION("aggregate manifest bytes") {
    count = 512;
    entry.provenance.source_location =
        std::string(conversation_maximum_provenance_bytes, 'x');
  }
  value.groups.front().entries.clear();
  value.groups.front().terminal_sequence = count + 2;
  value.snapshot_sequence = count + 2;
  for (std::size_t index = 0; index < count; ++index) {
    const auto suffix = std::to_string(index);
    auto next = entry;
    next.completed_event_id = id<EventId>("bounded-event-" + suffix);
    next.entry_id = id<ContextEntryId>("bounded-entry-" + suffix);
    next.message_id = id<MessageId>("bounded-message-" + suffix);
    next.provenance.source_id = id<ContextSourceId>("bounded-source-" + suffix);
    next.event_sequence = index + 2;
    next.order = index + 1;
    value.groups.front().entries.push_back(std::move(next));
  }
  const auto before = value.source_digest;
  const auto result = seal_conversation_summary_sources(value);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::resource_exhausted);
  CHECK(value.source_digest == before);
}

TEST_CASE("edited summary activation requires its exact immutable parent",
          "[summary]") {
  const auto source = intent();
  const auto original = candidate(source);
  auto edited = original;
  edited.revision = 2;
  edited.author = ConversationSummaryAuthor::user_edit;
  edited.edited_from = ConversationSummaryVersion{
      original.summary_id, original.revision, *original.candidate_digest};
  edited.created_event_id = id<EventId>("edited-candidate");
  edited.created_sequence = 14;
  edited.text = "Reviewed facts";
  REQUIRE(seal_conversation_summary_candidate(edited, source, &original));
  auto selected = activation(source, original);
  selected.candidate = {edited.summary_id, edited.revision,
                        *edited.candidate_digest};
  selected.activation_event_id = id<EventId>("edited-activation");
  selected.activation_sequence = 15;
  const auto before = selected.activation_digest;
  REQUIRE_FALSE(seal_conversation_summary_activation(selected, edited, source));
  CHECK(selected.activation_digest == before);
  REQUIRE(seal_conversation_summary_activation(selected, edited, source,
                                               &original));
  CHECK(validate_conversation_summary_activation(selected));
  auto changed = original;
  changed.text = "Unsealed change";
  REQUIRE_FALSE(
      seal_conversation_summary_activation(selected, edited, source, &changed));
}
