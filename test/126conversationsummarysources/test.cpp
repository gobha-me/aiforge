#include <catch2/catch_test_macros.hpp>

#include <aiforge/runtime/conversation_summary_sources.hpp>

#include <chrono>
#include <functional>
#include <stop_token>
#include <string>
#include <utility>

namespace {
using namespace aiforge;
using Code = runtime::ConversationSummarySourceErrorCode;

template <typename Id> auto id(const std::string& text) -> Id {
  return Id::from(text).value();
}

struct History {
  domain::SessionEventLog log{id<domain::SessionId>("session")};
  auto add(const std::string& run, domain::RunEventPayload payload,
           std::optional<domain::RunId> parent = {}) -> void {
    const auto seq = log.last_sequence() + 1;
    REQUIRE(log.append({{id<domain::EventId>("event-" + std::to_string(seq)),
                         id<domain::RunId>(run),
                         seq,
                         1,
                         domain::EventTimestamp{std::chrono::milliseconds{1}},
                         {},
                         std::move(parent),
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
    add(run, domain::UserContentAdded{{id<domain::MessageId>(run + "-user"),
                                       domain::Role::user,
                                       {domain::TextBlock{"question"}},
                                       {}}});
  }
  auto assistant_start(const std::string& run) -> void {
    add(run, domain::AssistantContentStarted{
                 id<domain::MessageId>(run + "-assistant"),
                 id<domain::InferenceId>(run + "-inference")});
  }
  auto assistant_finish(const std::string& run) -> void {
    add(run, domain::AssistantContentFinished{
                 id<domain::MessageId>(run + "-assistant"),
                 id<domain::InferenceId>(run + "-inference")});
  }
  auto answer(const std::string& run) -> void {
    assistant_start(run);
    add(run, domain::AssistantContentDeltaAdded{
                 id<domain::MessageId>(run + "-assistant"),
                 id<domain::InferenceId>(run + "-inference"),
                 domain::TextBlock{"answer ☃"}});
    assistant_finish(run);
    add(run, domain::RunCompleted{});
  }
  auto complete(const std::string& run,
                domain::RunPurpose purpose = domain::RunPurpose::conversation)
      -> void {
    start(run, purpose);
    answer(run);
  }
  auto tools() -> void {
    start("tools");
    assistant_start("tools");
    add("tools", domain::ToolProposed{id<domain::InvocationId>("call"),
                                      "read",
                                      {"application/json", "{}"},
                                      {}});
    // A validation error can precede assistant completion in durable order.
    add("tools",
        domain::ToolErrored{id<domain::InvocationId>("call"),
                            {domain::ErrorCode::backend, "invalid", false},
                            id<domain::MessageId>("tool-result")});
    assistant_finish("tools");
    add("tools", domain::RunCompleted{});
  }
};

auto prepare(const History& history,
             std::vector<domain::RunId> runs = {id<domain::RunId>("run")}) {
  return runtime::prepare_conversation_summary_sources(
      {history.log, std::move(runs)});
}

auto changed(const domain::SessionEventLog& original,
             const std::function<void(domain::RunEvent&)>& edit)
    -> domain::SessionEventLog {
  domain::SessionEventLog result{original.session_id()};
  for (auto event : original.events()) {
    edit(event);
    REQUIRE(result.append(std::move(event)));
  }
  return result;
}
} // namespace

TEST_CASE(
    "summary sources reject empty duplicate missing and unavailable requests",
    "[summarysources][failure]") {
  History history;
  history.complete("run");
  CHECK_FALSE(prepare(history, {}));
  CHECK_FALSE(
      prepare(history, {id<domain::RunId>("run"), id<domain::RunId>("run")}));
  CHECK_FALSE(prepare(history, {id<domain::RunId>("missing")}));
  for (const auto snapshot :
       {std::uint64_t{0}, history.log.last_sequence() + 1, std::uint64_t{2}}) {
    CHECK_FALSE(runtime::prepare_conversation_summary_sources(
        {history.log, {id<domain::RunId>("run")}, snapshot}));
  }
  const auto gapped =
      changed(history.log, [](auto& event) { event.metadata.sequence *= 2; });
  CHECK_FALSE(runtime::prepare_conversation_summary_sources(
      {gapped, {id<domain::RunId>("run")}, 3}));
}

TEST_CASE("summary sources reject live child control summary and invalid "
          "purpose runs",
          "[summarysources][failure]") {
  History live;
  live.start("run");
  CHECK_FALSE(prepare(live));
  for (const auto purpose :
       {domain::RunPurpose::control, domain::RunPurpose::summary,
        static_cast<domain::RunPurpose>(255)}) {
    History history;
    history.complete("run", purpose);
    CHECK_FALSE(prepare(history));
  }
  History child;
  child.complete("run");
  child.add("run", domain::ChildRunCreated{id<domain::RunId>("run"), {}});
  CHECK_FALSE(prepare(child));
  History parent;
  parent.complete("run");
  parent.add("run", domain::UnknownEvent{"metadata"},
             id<domain::RunId>("parent"));
  CHECK_FALSE(prepare(parent));
}

TEST_CASE("summary sources reject opaque consumed schemas but ignore unrelated "
          "payloads",
          "[summarysources][failure]") {
  History history;
  history.complete("run");
  for (const auto name : {"run.completed", "content.user_added",
                          "tool.result_recorded", "run.child_created"}) {
    const auto opaque = changed(history.log, [&](auto& event) {
      if (std::holds_alternative<domain::RunCompleted>(event.payload))
        event.payload = domain::UnknownEvent{name};
    });
    CHECK_FALSE(runtime::prepare_conversation_summary_sources(
        {opaque, {id<domain::RunId>("run")}}));
  }
  history.add("run", domain::UnknownEvent{"future.observation"});
  history.add("unrelated", domain::UnknownEvent{
                               "run.completed",
                               {"application/json", std::string(10000, 'x')}});
  auto limits = runtime::ConversationHistoryLimits{};
  limits.maximum_content_bytes = 1024;
  REQUIRE(runtime::prepare_conversation_summary_sources(
      {history.log, {id<domain::RunId>("run")}, {}, limits}));
}

TEST_CASE("summary source bounds and cancellation produce no partial result",
          "[summarysources][failure]") {
  History history;
  history.complete("run");
  const auto before = history.log.events();
  std::stop_source stop;
  stop.request_stop();
  auto cancelled = runtime::prepare_conversation_summary_sources(
      {history.log, {id<domain::RunId>("run")}}, stop.get_token());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error().code == Code::cancelled);
  for (unsigned bound = 0; bound < 5; ++bound) {
    auto limits = runtime::ConversationHistoryLimits{};
    if (bound == 0) limits.maximum_events = 1;
    if (bound == 1) limits.maximum_runs = 0;
    if (bound == 2) limits.maximum_content_bytes = 1;
    if (bound == 3) limits.maximum_source_references = 0;
    if (bound == 4) limits.maximum_content_items = 0;
    const auto result = runtime::prepare_conversation_summary_sources(
        {history.log, {id<domain::RunId>("run")}, {}, limits});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Code::resource_exhausted);
  }
  std::vector<domain::RunId> excessive;
  for (std::size_t n = 0; n <= domain::summary_maximum_groups; ++n)
    excessive.push_back(id<domain::RunId>("run-" + std::to_string(n)));
  const auto result = prepare(history, std::move(excessive));
  REQUIRE_FALSE(result);
  CHECK(result.error().code == Code::resource_exhausted);
  CHECK(history.log.events() == before);
}

TEST_CASE("summary recovery rejects modified seals partial coverage and "
          "substituted terminals",
          "[summarysources][failure]") {
  History history;
  history.complete("run");
  const auto prepared = prepare(history);
  REQUIRE(prepared);
  auto sources = prepared->sources;
  sources.groups.front().entries.front().estimated_tokens++;
  CHECK_FALSE(
      runtime::resolve_conversation_summary_sources(history.log, sources));
  REQUIRE(domain::seal_conversation_summary_sources(sources));
  CHECK_FALSE(
      runtime::resolve_conversation_summary_sources(history.log, sources));
  sources = prepared->sources;
  sources.groups.front().entries.pop_back();
  REQUIRE(domain::seal_conversation_summary_sources(sources));
  CHECK_FALSE(
      runtime::resolve_conversation_summary_sources(history.log, sources));
  sources = prepared->sources;
  sources.groups.front().terminal_event_id = id<domain::EventId>("substitute");
  REQUIRE(domain::seal_conversation_summary_sources(sources));
  CHECK_FALSE(
      runtime::resolve_conversation_summary_sources(history.log, sources));
  sources = prepared->sources;
  sources.session_id = id<domain::SessionId>("foreign");
  REQUIRE(domain::seal_conversation_summary_sources(sources));
  const auto foreign =
      runtime::resolve_conversation_summary_sources(history.log, sources);
  REQUIRE_FALSE(foreign);
  CHECK(foreign.error().code == Code::foreign_scope);
}

TEST_CASE("summary recovery checks actual source text and cannot import "
          "missing groups",
          "[summarysources][failure]") {
  History history;
  history.complete("run");
  const auto prepared = prepare(history);
  REQUIRE(prepared);
  const auto altered = changed(history.log, [](auto& event) {
    if (auto* user = std::get_if<domain::UserContentAdded>(&event.payload))
      user->message.content = {domain::TextBlock{"altered"}};
  });
  CHECK_FALSE(runtime::resolve_conversation_summary_sources(altered,
                                                            prepared->sources));
  domain::SessionEventLog missing{history.log.session_id()};
  CHECK_FALSE(runtime::resolve_conversation_summary_sources(missing,
                                                            prepared->sources));
  std::stop_source stop;
  stop.request_stop();
  const auto cancelled = runtime::resolve_conversation_summary_sources(
      history.log, prepared->sources, {}, stop.get_token());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error().code == Code::cancelled);
}

TEST_CASE("summary sources preserve complete tool exchanges and true "
          "completion references",
          "[summarysources][tools]") {
  History history;
  history.tools();
  const auto prepared = prepare(history, {id<domain::RunId>("tools")});
  REQUIRE(prepared);
  REQUIRE(prepared->content.size() == 3);
  CHECK(prepared->content[0].message.role == domain::Role::user);
  CHECK(prepared->content[1].message.tool_calls.size() == 1);
  CHECK(prepared->content[2].message.role == domain::Role::tool);
  const auto& entries = prepared->sources.groups.front().entries;
  CHECK(entries[2].event_sequence < entries[1].event_sequence);
  CHECK(entries[2].completed_event_id == id<domain::EventId>("event-5"));
  CHECK(prepared->sources.groups.front().terminal_sequence ==
        history.log.last_sequence());
  const auto resolved = runtime::resolve_conversation_summary_sources(
      history.log, prepared->sources);
  REQUIRE(resolved);
  CHECK(*resolved == *prepared);
}

TEST_CASE("summary sources canonicalize interleaved groups and recover only "
          "the saved prefix",
          "[summarysources][snapshot]") {
  History history;
  history.start("a");
  history.start("b");
  history.answer("b");
  history.answer("a");
  const auto prepared =
      prepare(history, {id<domain::RunId>("b"), id<domain::RunId>("a")});
  REQUIRE(prepared);
  REQUIRE(prepared->sources.groups.size() == 2);
  CHECK(prepared->sources.groups[0].run_id == id<domain::RunId>("a"));
  CHECK(prepared->sources.groups[1].run_id == id<domain::RunId>("b"));
  std::uint64_t tokens{};
  for (std::size_t n = 0; n < prepared->content.size(); ++n) {
    CHECK(prepared->content[n].order == n + 1);
    tokens += prepared->content[n].estimated_tokens;
  }
  CHECK(prepared->estimated_tokens == tokens);
  history.add("a", domain::ChildRunCreated{id<domain::RunId>("a"), {}});
  history.complete("new", domain::RunPurpose::summary);
  const auto resolved = runtime::resolve_conversation_summary_sources(
      history.log, prepared->sources);
  REQUIRE(resolved);
  CHECK(*resolved == *prepared);
  CHECK_FALSE(prepare(history, {id<domain::RunId>("a")}));
}

TEST_CASE(
    "failed and cancelled summary sources retain their atomic user input only",
    "[summarysources][terminal]") {
  History history;
  history.start("failed");
  history.assistant_start("failed");
  history.add("failed",
              domain::RunFailed{{domain::ErrorCode::backend, "failed", false}});
  history.start("cancelled");
  history.assistant_start("cancelled");
  history.add("cancelled", domain::RunCancelled{});
  const auto prepared = prepare(
      history, {id<domain::RunId>("failed"), id<domain::RunId>("cancelled")});
  REQUIRE(prepared);
  REQUIRE(prepared->sources.groups.size() == 2);
  REQUIRE(prepared->content.size() == 2);
  for (const auto& group : prepared->sources.groups)
    CHECK(group.entries.size() == 1);
  for (const auto& content : prepared->content)
    CHECK(content.message.role == domain::Role::user);
}
