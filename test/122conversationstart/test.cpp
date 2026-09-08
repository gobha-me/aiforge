#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/conversation_context.hpp>
#include <aiforge/runtime/run_kernel.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace {
using namespace aiforge;
template <class Id> auto id(const std::string& text) -> Id {
  return Id::from(text).value();
}

class AnswerStream final : public backend::BackendStream {
 public:
  explicit AnswerStream(domain::MessageId message)
      : m_events{backend::ResponseStarted{"fake-response"},
                 backend::ContentDelta{std::move(message),
                                       domain::TextBlock{"answer"}},
                 backend::ResponseFinished{domain::FinishReason::stop}} {}
  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_index == m_events.size()) return std::nullopt;
    return m_events[m_index++];
  }

 private:
  std::vector<backend::BackendEvent> m_events;
  std::size_t m_index{};
};

class RecordingBackend final : public backend::Backend {
 public:
  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    requests.push_back(request);
    return std::make_unique<AnswerStream>(request.assistant_message_id);
  }
  std::vector<backend::BackendRequest> requests;
};

auto drain(runtime::RunKernel& kernel) -> void {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{3};
  while (kernel.active_inference_id() &&
         std::chrono::steady_clock::now() < deadline) {
    REQUIRE(kernel.drain());
    if (kernel.active_inference_id())
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE_FALSE(kernel.active_inference_id());
  REQUIRE_FALSE(kernel.active_run_id());
}

auto start_request(const domain::SessionEventLog& log,
                   const std::string& suffix = "next", bool admitted = true,
                   bool early_user = false) -> runtime::RunStart {
  domain::Message instruction{
      id<domain::MessageId>(suffix + "-runtime-message"),
      domain::Role::system,
      {domain::TextBlock{"Runtime contract"}},
      {}};
  domain::Message user{id<domain::MessageId>(suffix + "-user-message"),
                       domain::Role::user,
                       {domain::TextBlock{"New question"}},
                       {}};
  auto instruction_tokens = runtime::estimate_conversation_message(instruction);
  auto user_tokens = runtime::estimate_conversation_message(user);
  REQUIRE(instruction_tokens);
  REQUIRE(user_tokens);
  const domain::ContextCapacity capacity{4096, 100, 50};
  auto prepared =
      runtime::prepare_conversation_context({log,
                                             id<domain::ModelId>("model"),
                                             capacity,
                                             *instruction_tokens + *user_tokens,
                                             17,
                                             {},
                                             {}});
  REQUIRE(prepared);
  domain::ContextBuildInput input{capacity, {}, {}};
  input.instructions.push_back(
      {id<domain::ContextEntryId>(suffix + "-runtime-entry"),
       domain::InstructionLayer::application_runtime,
       domain::InstructionOperation::add,
       {},
       instruction,
       {id<domain::ContextSourceId>(suffix + "-runtime-source"), {}, {}},
       0,
       1,
       *instruction_tokens});
  for (const auto& group : prepared->selection.selected_groups)
    for (const auto& entry : group.entries)
      input.content.push_back(entry.content);
  const auto user_order =
      early_user ? 1 : 17 + prepared->selection.selected_entry_count;
  input.content.push_back(
      {id<domain::ContextEntryId>(suffix + "-user-entry"),
       domain::ContextContentKind::conversation,
       user,
       {id<domain::ContextSourceId>(suffix + "-user-source"), {}, {}},
       user_order,
       *user_tokens});
  auto context = runtime::ContextBuilder{}.build(std::move(input));
  REQUIRE(context);
  runtime::RunStart result{id<domain::RunId>(suffix + "-run"),
                           {id<domain::SurfaceId>("chat"),
                            id<domain::WorkspaceId>("chat"),
                            id<domain::PermissionProfileId>("observe"),
                            {}},
                           user,
                           {id<domain::InferenceId>(suffix + "-inference"),
                            id<domain::MessageId>(suffix + "-assistant"),
                            id<domain::ModelId>("model"),
                            std::move(*context),
                            {},
                            {}}};
  if (admitted)
    result.attributes.conversation_admission = std::move(prepared->admission);
  return result;
}

struct Fixture {
  RecordingBackend backend;
  runtime::RunKernel kernel{id<domain::SessionId>("session"), backend};
  explicit Fixture(bool source = true) {
    if (source) {
      REQUIRE(kernel.start(start_request(kernel.event_log(), "source", false)));
      drain(kernel);
      REQUIRE(backend.requests.size() == 1);
    }
  }
  auto reject(runtime::RunStart start) -> void {
    const auto events = kernel.event_log().events();
    const auto dispatched = backend.requests.size();
    const auto result = kernel.start(std::move(start));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == runtime::RunKernelErrorCode::invalid_start);
    CHECK(kernel.event_log().events() == events);
    CHECK(backend.requests.size() == dispatched);
    CHECK_FALSE(kernel.active_run_id());
    CHECK_FALSE(kernel.active_inference_id());
  }
  auto rolling() -> void {
    REQUIRE(kernel.record_conversation_policy(
        {id<domain::RunId>("policy-run"),
         {id<domain::SurfaceId>("chat"),
          id<domain::WorkspaceId>("chat"),
          id<domain::PermissionProfileId>("observe"),
          {},
          {},
          domain::RunPurpose::control},
         0,
         domain::ConversationMode::rolling,
         {}}));
  }
};
} // namespace

TEST_CASE(
    "kernel rejects admission source and request mismatches before dispatch",
    "[conversation][kernel][failure]") {
  Fixture fixture;
  auto start = start_request(fixture.kernel.event_log());
  REQUIRE(start.attributes.conversation_admission->groups.size() == 1);
  REQUIRE(start.request.context.entries.size() == 4);
  SECTION("missing admitted source") {
    start.request.context.entries.erase(start.request.context.entries.begin() +
                                        1);
  }
  SECTION("changed historical message") {
    start.request.context.entries[1].message.content = {
        domain::TextBlock{"changed"}};
  }
  SECTION("changed historical estimate") {
    ++start.request.context.entries[1].estimated_tokens;
  }
  SECTION("changed historical provenance") {
    start.request.context.entries[1].provenance.source_location = "replaced";
  }
  SECTION("changed target model") {
    start.request.model_id = id<domain::ModelId>("other-model");
  }
  SECTION("changed capacity") {
    ++start.request.context.capacity.context_window_tokens;
  }
  SECTION("changed snapshot") {
    ++start.attributes.conversation_admission->source_snapshot_sequence;
    REQUIRE(domain::seal_conversation_admission(
        *start.attributes.conversation_admission));
  }
  SECTION("changed mandatory token total") {
    ++start.request.context.entries.front().estimated_tokens;
  }
  SECTION("changed current user") {
    start.request.context.entries.back().message.content = {
        domain::TextBlock{"another question"}};
  }
  SECTION("missing current user") {
    start.request.context.entries.pop_back();
  }
  SECTION("extra conversation entry") {
    auto extra = start.request.context.entries[1];
    extra.entry_id = id<domain::ContextEntryId>("extra-history");
    start.request.context.entries.push_back(std::move(extra));
  }
  SECTION("extra tool result") {
    auto extra = start.request.context.entries[1];
    extra.entry_id = id<domain::ContextEntryId>("extra-tool");
    extra.kind = domain::ContextEntryKind::tool_result;
    extra.message.role = domain::Role::tool;
    extra.message.invocation_id = id<domain::InvocationId>("unadmitted-tool");
    start.request.context.entries.push_back(std::move(extra));
  }
  fixture.reject(std::move(start));
}

TEST_CASE("kernel validates physical history order and current user position",
          "[conversation][kernel][ordering][failure]") {
  Fixture fixture;
  auto start = start_request(fixture.kernel.event_log());
  SECTION("physical history permutation preserves sealed order fields") {
    std::swap(start.request.context.entries[1],
              start.request.context.entries[2]);
  }
  SECTION("ContextBuilder sorts early current user before admitted history") {
    start = start_request(fixture.kernel.event_log(), "next", true, true);
    REQUIRE(start.request.context.entries[1].message == start.user_message);
    REQUIRE(start.request.context.entries[2].order == 17);
  }
  SECTION("nonhistory evidence cannot split the admitted exchange") {
    auto evidence = start.request.context.entries.front();
    evidence.entry_id = id<domain::ContextEntryId>("extra-evidence");
    evidence.kind = domain::ContextEntryKind::evidence;
    evidence.instruction_layer.reset();
    evidence.message.role = domain::Role::evidence;
    start.request.context.entries.insert(
        start.request.context.entries.begin() + 2, std::move(evidence));
  }
  SECTION(
      "current user order must follow history despite its physical position") {
    start.request.context.entries.back().order = 1;
  }
  fixture.reject(std::move(start));
}

TEST_CASE("kernel requires a manifest after explicit conversation policy",
          "[conversation][kernel][policy][failure]") {
  Fixture fixture;
  SECTION("rolling policy") {
    fixture.rolling();
  }
  SECTION("restored full policy remains explicit") {
    fixture.rolling();
    REQUIRE(fixture.kernel.record_conversation_policy(
        {id<domain::RunId>("full-policy-run"),
         {id<domain::SurfaceId>("chat"),
          id<domain::WorkspaceId>("chat"),
          id<domain::PermissionProfileId>("observe"),
          {},
          {},
          domain::RunPurpose::control},
         1,
         domain::ConversationMode::full,
         {}}));
  }
  fixture.reject(start_request(fixture.kernel.event_log(), "next", false));
}

TEST_CASE("kernel rejects an admission prepared before a policy append",
          "[conversation][kernel][snapshot][failure]") {
  Fixture fixture;
  auto stale = start_request(fixture.kernel.event_log());
  fixture.rolling();
  fixture.reject(std::move(stale));
}

TEST_CASE("explicit empty admission and absent legacy admission stay distinct",
          "[conversation][kernel][compatibility]") {
  Fixture fixture{false};
  bool admitted = true;
  SECTION("sealed explicit empty") {
  }
  SECTION("absent legacy") {
    admitted = false;
  }
  auto start = start_request(fixture.kernel.event_log(), "next", admitted);
  const auto expected = start.request;
  REQUIRE(fixture.kernel.start(start));
  drain(fixture.kernel);
  REQUIRE(fixture.backend.requests.size() == 1);
  CHECK(fixture.backend.requests.front() == expected);
  const auto* recorded = std::get_if<domain::RunStarted>(
      &fixture.kernel.event_log().events().front().payload);
  REQUIRE(recorded != nullptr);
  CHECK(recorded->conversation_admission.has_value() == admitted);
  if (admitted) {
    CHECK(recorded->conversation_admission->groups.empty());
    CHECK(recorded->conversation_admission->admission_digest.has_value());
    CHECK(fixture.kernel.event_log().events().front().metadata.schema_version ==
          3);
  } else {
    CHECK(fixture.kernel.event_log().events().front().metadata.schema_version ==
          1);
  }
}

TEST_CASE(
    "kernel dispatches the complete admitted history and records its seal",
    "[conversation][kernel][roundtrip]") {
  Fixture fixture;
  SECTION("implicit full") {
  }
  SECTION("explicit rolling") {
    fixture.rolling();
  }
  auto start = start_request(fixture.kernel.event_log());
  const auto expected = start.request;
  const auto admission = start.attributes.conversation_admission;
  const auto previous_sequence = fixture.kernel.event_log().last_sequence();
  REQUIRE(fixture.kernel.start(start));
  drain(fixture.kernel);
  REQUIRE(fixture.backend.requests.size() == 2);
  CHECK(fixture.backend.requests.back() == expected);
  const auto& started = fixture.kernel.event_log().events()[previous_sequence];
  REQUIRE(started.metadata.schema_version == 3);
  REQUIRE(std::holds_alternative<domain::RunStarted>(started.payload));
  CHECK(std::get<domain::RunStarted>(started.payload).conversation_admission ==
        admission);
}
