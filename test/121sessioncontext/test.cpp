#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/session_context.hpp>
#include <aiforge/testing/scripted_session_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <stop_token>
#include <string>
#include <unistd.h>

namespace {
using namespace aiforge;
using Code = runtime::SessionContextErrorCode;
template <class Id> auto id(const std::string& text) -> Id {
  return Id::from(text).value();
}
auto mandatory() -> domain::ContextBuildInput {
  return {{4096, 7, 11},
          {{id<domain::ContextEntryId>("runtime"),
            domain::InstructionLayer::application_runtime,
            domain::InstructionOperation::add,
            {},
            domain::Message{id<domain::MessageId>("runtime-message"),
                            domain::Role::system,
                            {domain::TextBlock{"contract"}},
                            {}},
            {id<domain::ContextSourceId>("runtime-source"), {}, {}},
            0,
            1,
            10}},
          {{id<domain::ContextEntryId>("current"),
            domain::ContextContentKind::conversation,
            {id<domain::MessageId>("current-message"),
             domain::Role::user,
             {domain::TextBlock{"question"}},
             {}},
            {id<domain::ContextSourceId>("current-source"), {}, {}},
            1,
            10}}};
}
struct History {
  domain::SessionEventLog log{id<domain::SessionId>("session")};
  auto add(const std::string& run, domain::RunEventPayload payload) -> void {
    const auto sequence = log.last_sequence() + 1;
    const auto schema =
        std::holds_alternative<domain::RunStarted>(payload) ? 3U : 1U;
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
  auto start(const std::string& run,
             domain::RunPurpose purpose = domain::RunPurpose::conversation)
      -> void {
    add(run, domain::RunStarted{id<domain::SurfaceId>("chat"),
                                id<domain::WorkspaceId>("chat"),
                                id<domain::PermissionProfileId>("observe"),
                                {},
                                {},
                                purpose});
  }
  auto turn(const std::string& run, std::string text = "old question") -> void {
    start(run);
    add(run, domain::UserContentAdded{{id<domain::MessageId>(run + "-user"),
                                       domain::Role::user,
                                       {domain::TextBlock{std::move(text)}},
                                       {}}});
    // Failed turns preserve complete original user input as a whole group.
    add(run,
        domain::RunFailed{{domain::ErrorCode::unavailable, "offline", false}});
  }
  auto rolling(std::vector<domain::RunId> pins = {}) -> void {
    start("policy", domain::RunPurpose::control);
    add("policy",
        domain::ConversationPolicySet{
            0, {1, domain::ConversationMode::rolling, std::move(pins)}});
    add("policy", domain::RunCompleted{});
  }
};
auto request(const History& history, const domain::ContextBuildInput& input)
    -> runtime::SessionContextRequest {
  return {history.log, id<domain::ModelId>("model"), input, nullptr, {}, {},
          {}};
}
struct MemoryFixture {
  std::filesystem::path directory;
  std::unique_ptr<adapters::SqliteSessionStore> store;
  std::unique_ptr<runtime::MemoryController> controller;
  std::uint64_t suffix{};
  std::int64_t timestamp{};
  MemoryFixture() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "aiforge-session-context-XXXXXX")
                       .string();
    pattern.push_back('\0');
    const auto* path = ::mkdtemp(pattern.data());
    REQUIRE(path != nullptr);
    directory = path;
    auto opened =
        adapters::SqliteSessionStore::open(directory / "sessions.sqlite3");
    REQUIRE(opened);
    store = std::move(*opened);
    controller = std::make_unique<runtime::MemoryController>(
        *store, [this] { return ++suffix; },
        [this] {
          return domain::EventTimestamp{std::chrono::milliseconds{++timestamp}};
        });
    History source;
    source.start("memory-source");
    source.add(
        "memory-source",
        domain::UserContentAdded{{id<domain::MessageId>("memory-user"),
                                  domain::Role::user,
                                  {domain::TextBlock{"Prefer concise answers"}},
                                  {}}});
    source.add("memory-source", domain::InferenceStarted{
                                    id<domain::InferenceId>("memory-inference"),
                                    id<domain::ModelId>("model")});
    source.add(
        "memory-source",
        domain::ToolProposed{
            id<domain::InvocationId>("memory-call"),
            "propose_memory",
            {"application/json",
             R"({"scope":"global","kind":"user_preference","content":"Prefer concise answers","rationale":"Keep the user preference","evidence_excerpt":"Prefer concise answers"})"},
            {}});
    source.add("memory-source",
               domain::ToolResultRecorded{
                   id<domain::InvocationId>("memory-call"),
                   {domain::StructuredDataBlock{
                       "application/json",
                       R"({"scope":"global","status":"proposed"})"}},
                   {}});
    REQUIRE(store->create_session(
        {source.log.session_id(), domain::EventTimestamp{}}));
    REQUIRE(store->append_events(source.log.session_id(), source.log.events()));
    REQUIRE(controller->capture_committed(
                source.log.session_id(), source.log.events(),
                {domain::MemoryCaptureMode::automatic,
                 domain::MemoryCaptureMode::automatic,
                 domain::MemoryCaptureMode::automatic, 2048},
                {}, "test") == 1);
  }
  ~MemoryFixture() {
    controller.reset();
    store.reset();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  MemoryFixture(const MemoryFixture&) = delete;
  auto operator=(const MemoryFixture&) -> MemoryFixture& = delete;
};
} // namespace

TEST_CASE(
    "Session preparation rejects bad mandatory input before memory access") {
  History history;
  auto input = mandatory();
  testing::ScriptedSessionStore store{{}};
  runtime::MemoryController memory{store, [] { return 1; },
                                   [] { return domain::EventTimestamp{}; }};
  auto value = request(history, input);
  value.memory_controller = &memory;
  SECTION("no runtime instruction") {
    input.instructions.clear();
  }
  SECTION("required input capacity") {
    input.capacity.context_window_tokens = 30;
  }
  SECTION("overflow") {
    input.instructions.front().estimated_tokens =
        std::numeric_limits<std::uint64_t>::max();
  }
  SECTION("history cannot be supplied as mandatory") {
    input.content.front().message.role = domain::Role::assistant;
  }
  SECTION("no current user") {
    input.content.clear();
  }
  SECTION("ambiguous required order") {
    auto evidence = input.content.front();
    evidence.entry_id = id<domain::ContextEntryId>("evidence");
    evidence.message.message_id = id<domain::MessageId>("evidence-message");
    evidence.message.role = domain::Role::evidence;
    evidence.kind = domain::ContextContentKind::evidence;
    input.content.push_back(std::move(evidence));
  }
  const auto before = input;
  REQUIRE_FALSE(runtime::prepare_session_context(value));
  CHECK(store.recorded_calls().empty());
  CHECK(input == before);
  CHECK(history.log.events().empty());
}

TEST_CASE("Session preparation rejects limits and cancellation without memory "
          "access") {
  History history;
  auto input = mandatory();
  auto value = request(history, input);
  SECTION("input count") {
    value.selection_limits.maximum_entries = 1;
  }
  SECTION("input bytes") {
    value.history_limits.maximum_content_bytes = 1;
  }
  SECTION("input items") {
    value.history_limits.maximum_content_items = 0;
  }
  REQUIRE_FALSE(runtime::prepare_session_context(value));
  std::stop_source stop;
  stop.request_stop();
  auto result = runtime::prepare_session_context(request(history, input),
                                                 stop.get_token());
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::cancelled);
}

TEST_CASE("Session preparation reserves full history and mandatory pins before "
          "memory") {
  History history;
  history.turn("old", std::string(100, 'x'));
  auto input = mandatory();
  input.capacity.context_window_tokens = 80;
  testing::ScriptedSessionStore store{{}};
  runtime::MemoryController memory{store, [] { return 1; },
                                   [] { return domain::EventTimestamp{}; }};
  SECTION("full history must fit") {
  }
  SECTION("pins must fit") {
    history.rolling({id<domain::RunId>("old")});
  }
  SECTION("missing pins fail") {
    history.rolling({id<domain::RunId>("missing")});
  }
  auto value = request(history, input);
  value.memory_controller = &memory;
  REQUIRE_FALSE(runtime::prepare_session_context(value));
  CHECK(store.recorded_calls().empty());
}

TEST_CASE("Session preparation preserves memory selection errors") {
  History history;
  auto input = mandatory();
  testing::ScriptedSessionStore store{{}};
  runtime::MemoryController memory{store, [] { return 1; },
                                   [] { return domain::EventTimestamp{}; }};
  auto value = request(history, input);
  value.memory_controller = &memory;
  auto result = runtime::prepare_session_context(value);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::memory_failed);
  CHECK_FALSE(store.recorded_calls().empty());
}

TEST_CASE(
    "Tool declaration accounting is versioned bounded and byte conservative") {
  std::vector<backend::ToolDeclaration> tools{
      {"lookup", "Résumé", {"application/json", "{}"}, {}, {}}};
  const auto expected = 32 + tools.front().name.size() +
                        tools.front().description.size() +
                        tools.front().input_schema.media_type.size() + 2;
  CHECK(runtime::estimate_session_tool_declarations(tools) == expected);
  CHECK(runtime::estimate_session_tool_declarations({}) == 0);
  SECTION("registered schema media type") {
    tools.front().input_schema.media_type = "application/schema+json";
    CHECK(runtime::estimate_session_tool_declarations(tools) == expected + 7);
  }
  SECTION("unsupported version") {
    REQUIRE_FALSE(runtime::estimate_session_tool_declarations(tools, 2));
  }
  SECTION("duplicate names") {
    tools.push_back(tools.front());
    REQUIRE_FALSE(runtime::estimate_session_tool_declarations(tools));
  }
  SECTION("control characters") {
    tools.front().name = "bad\nname";
    REQUIRE_FALSE(runtime::estimate_session_tool_declarations(tools));
  }
  SECTION("unsupported schema") {
    tools.front().input_schema.media_type = "unknown";
    REQUIRE_FALSE(runtime::estimate_session_tool_declarations(tools));
  }
  SECTION("empty schema") {
    tools.front().input_schema.data.clear();
    REQUIRE_FALSE(runtime::estimate_session_tool_declarations(tools));
  }
  SECTION("byte boundary") {
    runtime::ConversationHistoryLimits limits;
    limits.maximum_content_bytes = expected - 32;
    CHECK(runtime::estimate_session_tool_declarations(tools, 1, limits) ==
          expected);
    --limits.maximum_content_bytes;
    REQUIRE_FALSE(
        runtime::estimate_session_tool_declarations(tools, 1, limits));
  }
  SECTION("item boundary") {
    runtime::ConversationHistoryLimits limits;
    limits.maximum_content_items = 0;
    REQUIRE_FALSE(
        runtime::estimate_session_tool_declarations(tools, 1, limits));
  }
  SECTION("cancellation") {
    std::stop_source stop;
    stop.request_stop();
    REQUIRE_FALSE(runtime::estimate_session_tool_declarations(
        tools, 1, {}, stop.get_token()));
  }
}

TEST_CASE("Empty history seals both admissions and counts external input "
          "exactly once") {
  History history;
  auto input = mandatory();
  input.capacity.context_window_tokens = 38;
  auto result = runtime::prepare_session_context(request(history, input));
  REQUIRE(result);
  CHECK(result->conversation_admission.mandatory_input_tokens == 20);
  CHECK(result->conversation_admission.groups.empty());
  CHECK(result->memory_selection.available_tokens == 0);
  CHECK(domain::validate_memory_selection(result->memory_selection));
  CHECK(
      domain::validate_conversation_admission(result->conversation_admission));
  auto built = runtime::ContextBuilder{}.build(result->input);
  REQUIRE(built);
  CHECK(built->estimated_input_tokens == 31);
}

TEST_CASE("Session accounting respects instruction replacement and required "
          "evidence order") {
  History history;
  history.turn("old");
  auto input = mandatory();
  auto original = input.instructions.front();
  original.entry_id = id<domain::ContextEntryId>("old-global");
  original.layer = domain::InstructionLayer::user_global;
  original.message->message_id = id<domain::MessageId>("old-global-message");
  original.estimated_tokens = 10000;
  auto replacement = original;
  replacement.entry_id = id<domain::ContextEntryId>("new-global");
  replacement.message->message_id = id<domain::MessageId>("new-global-message");
  replacement.operation = domain::InstructionOperation::replace;
  replacement.target_entry_id = original.entry_id;
  replacement.order = 2;
  replacement.estimated_tokens = 5;
  input.instructions.push_back(std::move(original));
  input.instructions.push_back(std::move(replacement));
  auto evidence = input.content.front();
  evidence.entry_id = id<domain::ContextEntryId>("stdin");
  evidence.message.message_id = id<domain::MessageId>("stdin-message");
  evidence.message.role = domain::Role::evidence;
  evidence.kind = domain::ContextContentKind::evidence;
  evidence.order = 2;
  input.content.insert(input.content.begin(), std::move(evidence));
  // Original superseded estimate exceeds capacity; only the active five count.
  auto result = runtime::prepare_session_context(request(history, input));
  REQUIRE(result);
  CHECK(result->conversation_admission.mandatory_input_tokens == 35);
  REQUIRE(result->input.content.size() == 3);
  CHECK(result->input.content[1].entry_id ==
        id<domain::ContextEntryId>("current"));
  CHECK(result->input.content[2].entry_id ==
        id<domain::ContextEntryId>("stdin"));
  CHECK(result->input.content[2].order == 3);
}

TEST_CASE(
    "Scoped memory has priority over unpinned recent history in rolling mode") {
  MemoryFixture memory;
  auto selected = runtime::select_memory_context_with_provenance(
      *memory.controller, {{}, {}, 2048, 4096});
  REQUIRE(selected);
  REQUIRE(selected->content.size() == 1);
  const auto memory_tokens = selected->content.front().estimated_tokens;
  History history;
  history.turn("pinned");
  history.turn("middle");
  history.turn("newest");
  auto groups =
      runtime::reconstruct_conversation_history({history.log, {}, 1, {}});
  REQUIRE(groups);
  const auto group_tokens =
      groups->front().entries.front().content.estimated_tokens;
  REQUIRE(memory_tokens > group_tokens);
  auto input = mandatory();
  input.capacity.context_window_tokens = 38 + memory_tokens + group_tokens * 2;
  auto value = request(history, input);
  value.memory_controller = memory.controller.get();
  value.memory.available_tokens = std::numeric_limits<std::uint64_t>::max();
  SECTION("full reserves every history group before memory") {
    auto result = runtime::prepare_session_context(value);
    REQUIRE(result);
    CHECK(result->conversation_admission.groups.size() == 3);
    CHECK(result->memory_selection.entries.empty());
    CHECK(result->memory_selection.available_tokens ==
          memory_tokens - group_tokens);
  }
  SECTION("rolling preserves pin and gives memory priority") {
    history.rolling({id<domain::RunId>("pinned")});
    auto result = runtime::prepare_session_context(value);
    REQUIRE(result);
    REQUIRE(result->memory_selection.entries.size() == 1);
    REQUIRE(result->conversation_admission.groups.size() == 2);
    CHECK(result->conversation_admission.groups.front().run_id ==
          id<domain::RunId>("pinned"));
    CHECK(result->conversation_admission.groups.back().run_id ==
          id<domain::RunId>("newest"));
    CHECK(result->conversation_admission.omitted_group_count == 1);
    CHECK(result->conversation_admission.mandatory_input_tokens ==
          20 + memory_tokens);
    CHECK(result->memory_selection.entries.front().order == 1);
    CHECK(result->conversation_admission.groups.front().entries.front().order ==
          2);
    CHECK(result->input.content.back().order == 4);
    CHECK(result->input.content.front().kind ==
          domain::ContextContentKind::evidence);
    CHECK(domain::validate_memory_selection(result->memory_selection));
    CHECK(domain::validate_conversation_admission(
        result->conversation_admission));
    auto built = runtime::ContextBuilder{}.build(result->input);
    REQUIRE(built);
    CHECK(domain::memory_selection_matches_context(result->memory_selection,
                                                   *built));
  }
}
