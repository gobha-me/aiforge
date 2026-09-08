#include <catch2/catch_test_macros.hpp>

#include <aiforge/runtime/conversation_selection.hpp>

#include <limits>
#include <stop_token>
#include <string>
#include <utility>

namespace {

using namespace aiforge;
using Code = runtime::ConversationSelectionErrorCode;
using Decision = runtime::ConversationSelectionDecision;

template <typename Id> auto id(const std::string& text) -> Id {
  return Id::from(text).value();
}

auto entry(std::uint64_t order, domain::Role role, std::uint64_t tokens = 5)
    -> runtime::ConversationHistoryEntry {
  const auto suffix = std::to_string(order);
  return {{id<domain::ContextEntryId>("entry-" + suffix),
           domain::ContextContentKind::conversation,
           {id<domain::MessageId>("message-" + suffix),
            role,
            {domain::TextBlock{"text"}},
            std::nullopt},
           {id<domain::ContextSourceId>("source-" + suffix), std::nullopt,
            std::nullopt},
           order,
           tokens},
          id<domain::EventId>("event-" + suffix),
          order};
}

auto group(std::uint64_t number, std::uint64_t tokens = 10)
    -> runtime::ConversationHistoryGroup {
  return {
      id<domain::RunId>("run-" + std::to_string(number)),
      {entry(number * 10, domain::Role::user, tokens / 2),
       entry(number * 10 + 1, domain::Role::assistant, tokens - tokens / 2)}};
}

auto request(std::uint64_t available = 20)
    -> runtime::ConversationSelectionRequest {
  runtime::ConversationSelectionRequest value;
  value.mode = runtime::ConversationSelectionMode::rolling;
  value.capacity = {available + 15, 5, 3};
  value.mandatory_input_tokens = 7;
  value.groups = {group(1), group(2), group(3)};
  return value;
}

auto error(const runtime::ConversationSelectionRequest& value) -> Code {
  const auto selected = runtime::select_conversation(value);
  REQUIRE_FALSE(selected);
  return selected.error().code;
}

auto tool_group() -> runtime::ConversationHistoryGroup {
  auto value = group(1);
  auto& calls = value.entries[1].content.message.tool_calls;
  calls = {{id<domain::InvocationId>("tool-one"),
            "read",
            {"application/json", "{}"}},
           {id<domain::InvocationId>("tool-two"),
            "read",
            {"application/json", "{}"}}};
  for (std::uint64_t index = 0; index < 2; ++index) {
    auto result = entry(12 + index, domain::Role::tool);
    result.content.kind = domain::ContextContentKind::tool_result;
    result.content.message.invocation_id =
        id<domain::InvocationId>(index == 0 ? "tool-one" : "tool-two");
    value.entries.push_back(std::move(result));
  }
  value.entries.push_back(entry(14, domain::Role::assistant));
  return value;
}

} // namespace

TEST_CASE("conversation selection rejects invalid modes and mandatory capacity",
          "[conversationselection][failure]") {
  auto value = request();
  value.mode = static_cast<runtime::ConversationSelectionMode>(99);
  REQUIRE(error(value) == Code::invalid_mode);
  value = request();
  value.capacity.context_window_tokens = 0;
  REQUIRE(error(value) == Code::invalid_capacity);
  value = request();
  value.mandatory_input_tokens = 100;
  REQUIRE(error(value) == Code::mandatory_capacity_exceeded);
  value = request();
  value.mandatory_input_tokens = std::numeric_limits<std::uint64_t>::max();
  REQUIRE(error(value) == Code::token_overflow);
  value = request();
  value.capacity.reserved_output_tokens =
      std::numeric_limits<std::uint64_t>::max();
  REQUIRE(error(value) == Code::token_overflow);
}

TEST_CASE("conversation selection detects overflow even in omitted history",
          "[conversationselection][failure]") {
  auto value = request();
  value.groups[0].entries[0].content.estimated_tokens =
      std::numeric_limits<std::uint64_t>::max();
  REQUIRE(error(value) == Code::token_overflow);
  value = request();
  value.groups[0].entries[0].content.estimated_tokens =
      std::numeric_limits<std::uint64_t>::max() - 5;
  REQUIRE(error(value) == Code::token_overflow);
}

TEST_CASE("conversation selection rejects duplicate stable identities",
          "[conversationselection][failure]") {
  auto value = request();
  SECTION("runs") {
    value.groups[1].run_id = value.groups[0].run_id;
  }
  SECTION("events") {
    value.groups[1].entries[0].completed_event_id =
        value.groups[0].entries[0].completed_event_id;
  }
  SECTION("messages") {
    value.groups[1].entries[0].content.message.message_id =
        value.groups[0].entries[0].content.message.message_id;
  }
  SECTION("entries") {
    value.groups[1].entries[0].content.entry_id =
        value.groups[0].entries[0].content.entry_id;
  }
  SECTION("sources") {
    value.groups[1].entries[0].content.provenance.source_id =
        value.groups[0].entries[0].content.provenance.source_id;
  }
  REQUIRE(error(value) == Code::duplicate_identity);
}

TEST_CASE("conversation selection rejects nonchronological sources",
          "[conversationselection][failure]") {
  auto value = request();
  SECTION("zero event sequence") {
    value.groups[0].entries[0].event_sequence = 0;
  }
  SECTION("duplicate event sequence") {
    value.groups[1].entries[0].event_sequence = 11;
  }
  SECTION("source predates its user input") {
    value.groups[0].entries[1].event_sequence = 9;
  }
  SECTION("zero content order") {
    value.groups[0].entries[0].content.order = 0;
  }
  SECTION("duplicate content order") {
    value.groups[1].entries[0].content.order = 11;
  }
  SECTION("reordered groups") {
    std::swap(value.groups[0], value.groups[1]);
  }
  REQUIRE(error(value) == Code::invalid_chronology);
}

TEST_CASE("conversation selection validates discarded content and roles",
          "[conversationselection][failure]") {
  auto value = request();
  auto& first = value.groups[0].entries[0].content;
  SECTION("empty group") {
    value.groups[0].entries.clear();
  }
  SECTION("zero estimate") {
    first.estimated_tokens = 0;
  }
  SECTION("empty location") {
    first.provenance.source_location = "";
  }
  SECTION("empty digest") {
    first.provenance.digest = "";
  }
  SECTION("unknown content") {
    first.message.content = {domain::UnknownContentBlock{"future"}};
  }
  SECTION("instructions") {
    first.message.role = domain::Role::system;
  }
  SECTION("evidence") {
    first.kind = domain::ContextContentKind::evidence;
  }
  SECTION("empty message") {
    first.message.content.clear();
  }
  SECTION("second user") {
    value.groups[0].entries[1].content.message.role = domain::Role::user;
  }
  REQUIRE(error(value) == Code::invalid_group);
}

TEST_CASE("conversation selection enforces aggregate work and payload bounds",
          "[conversationselection][failure]") {
  auto value = request();
  SECTION("groups") {
    value.limits.maximum_groups = 2;
  }
  SECTION("entries") {
    value.limits.maximum_entries = 5;
  }
  SECTION("source references") {
    value.limits.maximum_source_references = 5;
  }
  SECTION("content items") {
    value.limits.maximum_content_items = 5;
  }
  SECTION("content bytes") {
    value.limits.maximum_content_bytes = 23;
  }
  SECTION("provenance bytes") {
    value.limits.maximum_content_bytes = 24;
    value.groups[0].entries[0].content.provenance.digest = "digest";
  }
  SECTION("pin count") {
    value.pinned_run_ids = {value.groups[0].run_id};
    value.limits.maximum_pins = 0;
  }
  REQUIRE(error(value) == Code::resource_exhausted);
}

TEST_CASE("conversation selection treats pins as mandatory known unique groups",
          "[conversationselection][failure]") {
  auto value = request();
  value.pinned_run_ids = {id<domain::RunId>("missing")};
  REQUIRE(error(value) == Code::invalid_pin);
  value.pinned_run_ids = {value.groups[0].run_id, value.groups[0].run_id};
  REQUIRE(error(value) == Code::invalid_pin);
  value = request(9);
  value.pinned_run_ids = {value.groups[0].run_id};
  REQUIRE(error(value) == Code::mandatory_capacity_exceeded);
}

TEST_CASE("full history never silently switches to rolling",
          "[conversationselection][failure]") {
  auto value = request();
  value.mode = runtime::ConversationSelectionMode::full;
  REQUIRE(error(value) == Code::history_capacity_exceeded);
  value.capacity.context_window_tokens += 10;
  const auto selected = runtime::select_conversation(value);
  REQUIRE(selected);
  CHECK(selected->selected_groups == value.groups);
  CHECK(selected->decisions[0].decision == Decision::admitted_full);
  CHECK(selected->omitted_group_count == 0);
}

TEST_CASE("conversation selection rejects split or unmatched tool exchanges",
          "[conversationselection][failure]") {
  auto value = request();
  value.groups = {tool_group()};
  SECTION("missing result") {
    value.groups[0].entries.erase(value.groups[0].entries.begin() + 2);
  }
  SECTION("orphan result") {
    value.groups[0].entries[2].content.message.invocation_id =
        id<domain::InvocationId>("missing");
  }
  SECTION("duplicate invocation") {
    value.groups[0].entries[1].content.message.tool_calls[1].invocation_id =
        id<domain::InvocationId>("tool-one");
    REQUIRE(error(value) == Code::duplicate_identity);
    return;
  }
  SECTION("missing final response") {
    value.groups[0].entries.pop_back();
  }
  SECTION("user tool call") {
    value.groups[0].entries[0].content.message.tool_calls =
        value.groups[0].entries[1].content.message.tool_calls;
  }
  SECTION("tool result masquerades as conversation") {
    value.groups[0].entries[2].content.kind =
        domain::ContextContentKind::conversation;
  }
  REQUIRE(error(value) == Code::invalid_group);
}

TEST_CASE("conversation selection cancellation leaves immutable input intact",
          "[conversationselection][failure]") {
  const auto value = request();
  const auto original = value;
  std::stop_source stop;
  stop.request_stop();
  const auto selected = runtime::select_conversation(value, stop.get_token());
  REQUIRE_FALSE(selected);
  CHECK(selected.error().code == Code::cancelled);
  CHECK(value == original);
}

TEST_CASE("omitted tool groups cannot hide malformed call metadata",
          "[conversationselection][failure]") {
  auto value = request(10);
  value.groups = {tool_group(), group(2)};
  auto& call = value.groups[0].entries[1].content.message.tool_calls[0];
  SECTION("non JSON arguments") {
    call.arguments.media_type = "text/plain";
  }
  SECTION("missing arguments") {
    call.arguments.data.clear();
  }
  SECTION("control bearing name") {
    call.tool_name = "read\nfile";
  }
  SECTION("oversized name") {
    call.tool_name = std::string(129, 'a');
  }
  REQUIRE(error(value) == Code::invalid_group);
}

TEST_CASE("multiple sequential tool exchanges remain one atomic historical run",
          "[conversationselection]") {
  auto value = request(35);
  auto history = tool_group();
  history.entries.back().content.message.tool_calls = {
      {id<domain::InvocationId>("third-tool"),
       "read",
       {"application/json", "{}"}}};
  auto third_result = entry(15, domain::Role::tool);
  third_result.content.kind = domain::ContextContentKind::tool_result;
  third_result.content.message.invocation_id =
      id<domain::InvocationId>("third-tool");
  history.entries.push_back(std::move(third_result));
  history.entries.push_back(entry(16, domain::Role::assistant));
  value.groups = {std::move(history)};
  const auto selected = runtime::select_conversation(value);
  REQUIRE(selected);
  CHECK(selected->selected_groups == value.groups);
  CHECK(selected->selected_entry_count == 7);
  --value.capacity.context_window_tokens;
  const auto omitted = runtime::select_conversation(value);
  REQUIRE(omitted);
  CHECK(omitted->selected_groups.empty());
  CHECK(omitted->omitted_entry_count == 7);
  value.pinned_run_ids = {value.groups[0].run_id};
  CHECK(error(value) == Code::mandatory_capacity_exceeded);
}

TEST_CASE(
    "rolling history keeps a newest contiguous suffix with honest accounting",
    "[conversationselection]") {
  const auto value = request();
  const auto selected = runtime::select_conversation(value);
  REQUIRE(selected);
  REQUIRE(selected->selected_groups.size() == 2);
  CHECK(selected->selected_groups[0] == value.groups[1]);
  CHECK(selected->selected_groups[1] == value.groups[2]);
  CHECK(selected->decisions[0].decision == Decision::omitted_capacity);
  CHECK(selected->selected_entry_count == 4);
  CHECK(selected->omitted_entry_count == 2);
  CHECK(selected->omitted_group_count == 1);
  CHECK(selected->available_history_tokens == 20);
  CHECK(selected->selected_history_tokens == 20);
  CHECK(selected->omitted_history_tokens == 10);
  CHECK(selected->estimated_input_tokens == 30);
}

TEST_CASE("rolling never backfills older small groups after a capacity gap",
          "[conversationselection]") {
  auto value = request(15);
  value.groups[1] = group(2, 20);
  const auto selected = runtime::select_conversation(value);
  REQUIRE(selected);
  REQUIRE(selected->selected_groups.size() == 1);
  CHECK(selected->selected_groups[0] == value.groups[2]);
  CHECK(selected->decisions[0].decision == Decision::omitted_older);
  CHECK(selected->decisions[1].decision == Decision::omitted_capacity);
  value.groups[2] = group(3, 20);
  const auto none = runtime::select_conversation(value);
  REQUIRE(none);
  CHECK(none->selected_groups.empty());
}

TEST_CASE("older pins survive gaps without double charging recent pins",
          "[conversationselection]") {
  auto value = request(20);
  value.pinned_run_ids = {value.groups[0].run_id, value.groups[2].run_id};
  const auto selected = runtime::select_conversation(value);
  REQUIRE(selected);
  REQUIRE(selected->selected_groups.size() == 2);
  CHECK(selected->selected_groups[0] == value.groups[0]);
  CHECK(selected->selected_groups[1] == value.groups[2]);
  CHECK(selected->selected_history_tokens == 20);
  CHECK(selected->decisions[0].decision == Decision::admitted_pin);
  CHECK(selected->decisions[2].decision == Decision::admitted_pin);
}

TEST_CASE("complete multi-tool groups fit or leave as a whole",
          "[conversationselection]") {
  auto value = request(25);
  value.groups = {tool_group()};
  auto selected = runtime::select_conversation(value);
  REQUIRE(selected);
  CHECK(selected->selected_groups == value.groups);
  value.capacity.context_window_tokens -= 1;
  selected = runtime::select_conversation(value);
  REQUIRE(selected);
  CHECK(selected->selected_groups.empty());
  CHECK(selected->omitted_entry_count == 5);
}

TEST_CASE(
    "UTF8 content is preserved and supplied estimates are not retokenized",
    "[conversationselection]") {
  auto value = request(10);
  value.groups = {group(1)};
  value.groups[0].entries[0].content.message.content = {
      domain::TextBlock{"雪の物語 🐈"}};
  const auto selected = runtime::select_conversation(value);
  REQUIRE(selected);
  CHECK(selected->selected_groups == value.groups);
  CHECK(selected->selected_history_tokens == 10);
}

TEST_CASE("empty history and retained failed user input have exact boundaries",
          "[conversationselection]") {
  auto value = request(0);
  value.groups.clear();
  value.limits = {0, 0, 0, 0, 0, 0};
  const auto empty = runtime::select_conversation(value);
  REQUIRE(empty);
  CHECK(empty->estimated_input_tokens == 10);
  value = request(5);
  value.groups = {group(1)};
  value.groups[0].entries.pop_back();
  const auto failed_user = runtime::select_conversation(value);
  REQUIRE(failed_user);
  CHECK(failed_user->selected_groups == value.groups);
}

TEST_CASE("early tool errors retain their true source while following the call",
          "[conversationselection]") {
  auto value = request(25);
  value.groups = {tool_group()};
  // Rejected proposals can terminate before the assistant's completion event.
  // Provider order remains assistant, tools, answer.
  value.groups[0].entries[1].event_sequence = 13;
  value.groups[0].entries[2].event_sequence = 11;
  value.groups[0].entries[3].event_sequence = 12;
  const auto selected = runtime::select_conversation(value);
  REQUIRE(selected);
  CHECK(selected->selected_groups == value.groups);
  --value.capacity.context_window_tokens;
  const auto omitted = runtime::select_conversation(value);
  REQUIRE(omitted);
  CHECK(omitted->selected_groups.empty());
}

TEST_CASE("completed interleaved runs retain group and flattened message order",
          "[conversationselection]") {
  auto value = request(20);
  value.groups.pop_back();
  value.groups[0].entries[0].event_sequence = 1;
  value.groups[0].entries[1].event_sequence = 10;
  value.groups[1].entries[0].event_sequence = 2;
  value.groups[1].entries[1].event_sequence = 9;
  const auto selected = runtime::select_conversation(value);
  REQUIRE(selected);
  CHECK(selected->selected_groups == value.groups);
}
