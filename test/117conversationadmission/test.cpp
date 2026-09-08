#include <aiforge/domain/conversation_admission.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <utility>

namespace {
using namespace aiforge::domain;
using Code = ConversationAdmissionErrorCode;
template <typename T> auto id(const std::string& text) -> T {
  return T::from(text).value();
}
auto message() -> Message {
  return {
      id<MessageId>("message"), Role::user, {TextBlock{"story"}}, std::nullopt};
}
auto admission() -> ConversationAdmission {
  const auto text = message();
  const auto digest = normalized_conversation_message_digest(text).value();
  ConversationAdmission value{.session_id = id<SessionId>("session"),
                              .model_id = id<ModelId>("model"),
                              .policy_event_id = {},
                              .capacity = {},
                              .groups = {},
                              .omitted_groups_digest = {},
                              .admission_digest = {}};
  value.source_snapshot_sequence = 10;
  value.capacity = {100, 10, 5};
  value.mandatory_input_tokens = 15;
  value.groups = {
      {id<RunId>("run"),
       {{id<EventId>("event"),
         2,
         id<ContextEntryId>("entry"),
         text.message_id,
         {id<ContextSourceId>("source"), "session:source", "source-digest"},
         1,
         20,
         ContextContentKind::conversation,
         digest}},
       false}};
  return value;
}
auto sealing_error(ConversationAdmission value) -> Code {
  const auto sealed = seal_conversation_admission(value);
  REQUIRE_FALSE(sealed);
  return sealed.error().code;
}
} // namespace

TEST_CASE("conversation policy rejects malformed bounded state",
          "[conversationadmission][failure]") {
  ConversationPolicy policy;
  REQUIRE_FALSE(validate_conversation_policy(policy));
  policy.revision = 1;
  policy.mode = static_cast<ConversationMode>(99);
  REQUIRE_FALSE(validate_conversation_policy(policy));
  policy.mode = ConversationMode::rolling;
  policy.pinned_run_ids = {id<RunId>("run"), id<RunId>("run")};
  REQUIRE_FALSE(validate_conversation_policy(policy));
  policy.pinned_run_ids.assign(conversation_maximum_pins + 1, id<RunId>("run"));
  const auto limited = validate_conversation_policy(policy);
  REQUIRE_FALSE(limited);
  CHECK(limited.error().code == Code::resource_exhausted);
}

TEST_CASE("admission rejects unsupported schema and estimator",
          "[conversationadmission][failure]") {
  auto value = admission();
  SECTION("schema") {
    value.version = 99;
  }
  SECTION("estimator") {
    value.estimator_version = 2;
  }
  CHECK(sealing_error(value) == Code::unsupported_version);
}

TEST_CASE("admission requires coherent policy references",
          "[conversationadmission][failure]") {
  auto value = admission();
  SECTION("rolling has no policy") {
    value.mode = ConversationMode::rolling;
  }
  SECTION("revision without event") {
    value.policy_revision = 1;
  }
  SECTION("event without revision") {
    value.policy_event_id = id<EventId>("policy");
  }
  SECTION("invalid mode") {
    value.mode = static_cast<ConversationMode>(55);
  }
  CHECK(sealing_error(value) == Code::invalid_policy);
}

TEST_CASE("admission rejects missing and changed seals",
          "[conversationadmission][failure]") {
  auto value = admission();
  REQUIRE_FALSE(validate_conversation_admission(value));
  REQUIRE(seal_conversation_admission(value));
  SECTION("session") {
    value.session_id = id<SessionId>("other");
  }
  SECTION("model") {
    value.model_id = id<ModelId>("other");
  }
  SECTION("snapshot") {
    ++value.source_snapshot_sequence;
  }
  SECTION("capacity") {
    ++value.capacity.context_window_tokens;
  }
  SECTION("output") {
    ++value.capacity.reserved_output_tokens;
  }
  SECTION("external input") {
    ++value.capacity.reserved_input_tokens;
  }
  SECTION("mandatory inputs") {
    ++value.mandatory_input_tokens;
  }
  SECTION("run") {
    value.groups[0].run_id = id<RunId>("other");
  }
  SECTION("event") {
    value.groups[0].entries[0].completed_event_id = id<EventId>("other");
  }
  SECTION("entry") {
    value.groups[0].entries[0].entry_id = id<ContextEntryId>("other");
  }
  SECTION("message") {
    value.groups[0].entries[0].message_id = id<MessageId>("other");
  }
  SECTION("location") {
    value.groups[0].entries[0].provenance.source_location = "other";
  }
  SECTION("source digest") {
    value.groups[0].entries[0].provenance.digest = "other";
  }
  SECTION("source identity") {
    value.groups[0].entries[0].provenance.source_id =
        id<ContextSourceId>("other");
  }
  SECTION("sequence") {
    ++value.groups[0].entries[0].event_sequence;
  }
  SECTION("order") {
    ++value.groups[0].entries[0].order;
  }
  SECTION("estimate") {
    ++value.groups[0].entries[0].estimated_tokens;
  }
  SECTION("message bytes") {
    ++value.groups[0].entries[0].message_digest.byte_size;
  }
  SECTION("message digest") {
    value.groups[0].entries[0].message_digest.value.assign(64, 'a');
  }
  SECTION("seal bytes") {
    ++value.admission_digest->byte_size;
  }
  SECTION("seal digest") {
    value.admission_digest->value.assign(64, 'b');
  }
  const auto validated = validate_conversation_admission(value);
  REQUIRE_FALSE(validated);
  CHECK(validated.error().code == Code::invalid_digest);
}

TEST_CASE("admission binds expected session",
          "[conversationadmission][failure]") {
  auto value = admission();
  REQUIRE(seal_conversation_admission(value));
  const auto validated =
      validate_conversation_admission(value, id<SessionId>("foreign"));
  REQUIRE_FALSE(validated);
  CHECK(validated.error().code == Code::foreign_scope);
  CHECK(validate_conversation_admission(value, value.session_id));
}

TEST_CASE("admission rejects invalid sources and classifications",
          "[conversationadmission][failure]") {
  auto value = admission();
  auto& entry = value.groups[0].entries[0];
  SECTION("zero sequence") {
    entry.event_sequence = 0;
  }
  SECTION("future source") {
    entry.event_sequence = 11;
  }
  SECTION("zero order") {
    entry.order = 0;
  }
  SECTION("zero estimate") {
    entry.estimated_tokens = 0;
  }
  SECTION("unknown kind") {
    entry.kind = ContextContentKind::unknown;
  }
  SECTION("evidence kind") {
    entry.kind = ContextContentKind::evidence;
  }
  SECTION("first source tool result") {
    entry.kind = ContextContentKind::tool_result;
  }
  SECTION("empty provenance") {
    entry.provenance.source_location = "";
  }
  SECTION("oversized provenance") {
    entry.provenance.digest =
        std::string(conversation_maximum_provenance_bytes + 1, 'x');
  }
  SECTION("empty group") {
    value.groups[0].entries.clear();
  }
  SECTION("pin without policy") {
    value.groups[0].pinned = true;
  }
  CHECK(sealing_error(value) == Code::invalid_admission);
}

TEST_CASE("admission accounting rejects overflow and capacity failure",
          "[conversationadmission][failure]") {
  auto value = admission();
  SECTION("mandatory overflow") {
    value.mandatory_input_tokens = std::numeric_limits<std::uint64_t>::max();
    CHECK(sealing_error(value) == Code::token_overflow);
  }
  SECTION("output overflow") {
    value.capacity.reserved_output_tokens =
        std::numeric_limits<std::uint64_t>::max();
    CHECK(sealing_error(value) == Code::token_overflow);
  }
  SECTION("zero capacity") {
    value.capacity.context_window_tokens = 0;
    CHECK(sealing_error(value) == Code::capacity_exceeded);
  }
  SECTION("below exact fit") {
    value.capacity.context_window_tokens = 49;
    CHECK(sealing_error(value) == Code::capacity_exceeded);
  }
}

TEST_CASE("admission limits collections before traversal",
          "[conversationadmission][failure]") {
  auto value = admission();
  SECTION("groups") {
    value.groups.resize(conversation_maximum_groups + 1, value.groups[0]);
  }
  SECTION("entries") {
    value.groups[0].entries.resize(conversation_maximum_entries + 1,
                                   value.groups[0].entries[0]);
  }
  CHECK(sealing_error(value) == Code::resource_exhausted);
}

TEST_CASE("admission bounds aggregate manifest encoding",
          "[conversationadmission][failure]") {
  auto value = admission();
  value.source_snapshot_sequence = 2000;
  value.capacity.context_window_tokens = 100000;
  const auto prototype = value.groups.front().entries.front();
  value.groups.front().entries.clear();
  for (std::uint64_t index = 0; index < 1024; ++index) {
    auto entry = prototype;
    const auto suffix = std::to_string(index);
    entry.completed_event_id = id<EventId>("event-" + suffix);
    entry.entry_id = id<ContextEntryId>("entry-" + suffix);
    entry.message_id = id<MessageId>("message-" + suffix);
    entry.provenance.source_id = id<ContextSourceId>("source-" + suffix);
    entry.provenance.source_location =
        std::string(conversation_maximum_provenance_bytes, 'x');
    entry.provenance.digest =
        std::string(conversation_maximum_provenance_bytes, 'y');
    entry.event_sequence = index + 2;
    entry.order = index + 1;
    value.groups.front().entries.push_back(std::move(entry));
  }
  CHECK(sealing_error(value) == Code::resource_exhausted);
}

TEST_CASE("admission rejects duplicate identities and logical order",
          "[conversationadmission][failure]") {
  auto value = admission();
  auto second = value.groups[0].entries[0];
  second.completed_event_id = id<EventId>("event-two");
  second.event_sequence = 3;
  second.entry_id = id<ContextEntryId>("entry-two");
  second.message_id = id<MessageId>("message-two");
  second.provenance.source_id = id<ContextSourceId>("source-two");
  second.order = 2;
  SECTION("event") {
    second.completed_event_id = value.groups[0].entries[0].completed_event_id;
  }
  SECTION("sequence") {
    second.event_sequence = 2;
  }
  SECTION("entry") {
    second.entry_id = value.groups[0].entries[0].entry_id;
  }
  SECTION("message") {
    second.message_id = value.groups[0].entries[0].message_id;
  }
  SECTION("source") {
    second.provenance.source_id =
        value.groups[0].entries[0].provenance.source_id;
  }
  SECTION("order") {
    second.order = 1;
  }
  SECTION("predates user") {
    second.event_sequence = 1;
  }
  value.groups[0].entries.push_back(std::move(second));
  CHECK(sealing_error(value) == Code::invalid_admission);
}

TEST_CASE("admission validates bounded omission summaries",
          "[conversationadmission][failure]") {
  auto value = admission();
  value.mode = ConversationMode::rolling;
  value.policy_event_id = id<EventId>("policy");
  value.policy_revision = 1;
  value.omitted_group_count = 1;
  const auto digest = value.groups[0].entries[0].message_digest;
  SECTION("missing summary") {
    CHECK(sealing_error(value) == Code::invalid_admission);
  }
  SECTION("zero count with summary") {
    value.omitted_group_count = 0;
    value.omitted_groups_digest = digest;
    CHECK(sealing_error(value) == Code::invalid_admission);
  }
  SECTION("count exceeds snapshot") {
    value.omitted_group_count = 11;
    value.omitted_groups_digest = digest;
    CHECK(sealing_error(value) == Code::invalid_admission);
  }
  SECTION("full mode cannot omit") {
    value.mode = ConversationMode::full;
    value.omitted_groups_digest = digest;
    CHECK(sealing_error(value) == Code::invalid_admission);
  }
  SECTION("corrupt summary") {
    value.omitted_groups_digest = ContentDigest{"sha256", "bad", 1};
    CHECK(sealing_error(value) == Code::invalid_digest);
  }
  SECTION("sealed count changes") {
    value.omitted_groups_digest = digest;
    REQUIRE(seal_conversation_admission(value));
    ++value.omitted_group_count;
    REQUIRE_FALSE(validate_conversation_admission(value));
  }
}

TEST_CASE("unsupported messages fail before digest publication",
          "[conversationadmission][failure]") {
  auto value = message();
  SECTION("unknown") {
    value.content = {UnknownContentBlock{"future"}};
  }
  SECTION("unresolved artifact") {
    value.content = {
        ArtifactReferenceBlock{id<ArtifactId>("artifact"), std::nullopt}};
  }
  SECTION("system role") {
    value.role = Role::system;
  }
  SECTION("orphan tool") {
    value.role = Role::tool;
  }
  SECTION("user invocation") {
    value.invocation_id = id<InvocationId>("tool");
  }
  SECTION("empty") {
    value.content.clear();
  }
  REQUIRE_FALSE(normalized_conversation_message_digest(value));
  REQUIRE_FALSE(normalized_conversation_message_digest(message(), 2));
}

TEST_CASE("message digests bound payload and zero-byte work",
          "[conversationadmission][failure]") {
  auto value = message();
  SECTION("bytes") {
    value.content = {
        TextBlock{std::string(conversation_maximum_message_bytes, 'x')}};
  }
  SECTION("items") {
    value.content.assign(conversation_maximum_message_items + 1, TextBlock{""});
  }
  const auto result = normalized_conversation_message_digest(value);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::resource_exhausted);
}

TEST_CASE("message digests reject malformed tool fields",
          "[conversationadmission][failure]") {
  auto value = message();
  value.role = Role::assistant;
  value.tool_calls = {
      {id<InvocationId>("tool"), "read", {"application/json", "{}"}}};
  SECTION("control name") {
    value.tool_calls[0].tool_name = "read\nfile";
  }
  SECTION("long name") {
    value.tool_calls[0].tool_name.assign(129, 'a');
  }
  SECTION("empty name") {
    value.tool_calls[0].tool_name.clear();
  }
  SECTION("media type") {
    value.tool_calls[0].arguments.media_type = "text/plain";
  }
  SECTION("empty arguments") {
    value.tool_calls[0].arguments.data.clear();
  }
  SECTION("duplicate call") {
    value.tool_calls.push_back(value.tool_calls[0]);
  }
  const auto result = normalized_conversation_message_digest(value);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::invalid_message);
}

TEST_CASE("message digest includes framing and optional presence",
          "[conversationadmission]") {
  auto value = message();
  const auto original = normalized_conversation_message_digest(value);
  REQUIRE(original);
  SECTION("text") {
    value.content = {TextBlock{"stories"}};
  }
  SECTION("identity") {
    value.message_id = id<MessageId>("other");
  }
  SECTION("role") {
    value.role = Role::assistant;
  }
  SECTION("block framing") {
    value.content = {TextBlock{"st"}, TextBlock{"ory"}};
  }
  SECTION("structured kind") {
    value.content = {StructuredDataBlock{"text", "story"}};
  }
  SECTION("citation") {
    value.content = {CitationBlock{"story", std::nullopt}};
  }
  const auto changed = normalized_conversation_message_digest(value);
  REQUIRE(changed);
  CHECK(*original != *changed);
  value.content = {CitationBlock{"source", std::nullopt}};
  const auto absent = normalized_conversation_message_digest(value);
  value.content = {CitationBlock{"source", ""}};
  CHECK(absent != normalized_conversation_message_digest(value));
}

TEST_CASE("tool message digest binds exact calls", "[conversationadmission]") {
  auto value = message();
  value.role = Role::assistant;
  value.tool_calls = {
      {id<InvocationId>("tool"), "read", {"application/json", "{}"}}};
  const auto original = normalized_conversation_message_digest(value);
  REQUIRE(original);
  SECTION("name") {
    value.tool_calls[0].tool_name = "write";
  }
  SECTION("arguments") {
    value.tool_calls[0].arguments.data = "[]";
  }
  SECTION("invocation") {
    value.tool_calls[0].invocation_id = id<InvocationId>("other");
  }
  const auto changed = normalized_conversation_message_digest(value);
  REQUIRE(changed);
  CHECK(*original != *changed);
}

TEST_CASE("admission seals explicit empty input and preserves failed seals",
          "[conversationadmission]") {
  auto value = admission();
  value.groups.clear();
  value.source_snapshot_sequence = 0;
  REQUIRE(seal_conversation_admission(value));
  REQUIRE(validate_conversation_admission(value));
  const auto previous = value.admission_digest;
  value.version = 9;
  REQUIRE_FALSE(seal_conversation_admission(value));
  CHECK(value.admission_digest == previous);
}

TEST_CASE("admission preserves early tool errors in provider order",
          "[conversationadmission]") {
  auto value = admission();
  for (std::uint64_t index = 0; index < 2; ++index) {
    auto entry = value.groups[0].entries[0];
    const auto suffix = std::to_string(index);
    entry.completed_event_id = id<EventId>("event-extra-" + suffix);
    entry.event_sequence = index == 0 ? 5 : 3;
    entry.entry_id = id<ContextEntryId>("entry-extra-" + suffix);
    entry.message_id = id<MessageId>("message-extra-" + suffix);
    entry.provenance.source_id = id<ContextSourceId>("source-extra-" + suffix);
    entry.order = index + 2;
    entry.kind = index == 0 ? ContextContentKind::conversation
                            : ContextContentKind::tool_result;
    value.groups[0].entries.push_back(std::move(entry));
  }
  value.capacity.context_window_tokens = 90;
  REQUIRE(seal_conversation_admission(value));
  CHECK(validate_conversation_admission(value));
  CHECK(value.groups[0].entries[2].event_sequence == 3);
}

TEST_CASE("exact fit admission and pinned policy retain identities",
          "[conversationadmission]") {
  auto value = admission();
  value.capacity.context_window_tokens = 50;
  value.policy_event_id = id<EventId>("policy");
  value.policy_revision = 3;
  value.mode = ConversationMode::rolling;
  value.groups[0].pinned = true;
  REQUIRE(validate_conversation_policy(
      {3, ConversationMode::rolling, {value.groups[0].run_id}}));
  REQUIRE(seal_conversation_admission(value));
  CHECK(validate_conversation_admission(value));
  const auto sealed = value;
  REQUIRE(seal_conversation_admission(value));
  CHECK(value == sealed);
}
