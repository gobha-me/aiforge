#include "fixture.hpp"
#include <functional>

namespace {
using namespace summary_kernel_test;
using Mutation = std::function<void(runtime::RunStart&)>;
} // namespace

TEST_CASE("summary kernel rejects missing foreign mismatched and tampered "
          "intent before dispatch",
          "[summarykernel][failure]") {
  const std::vector<Mutation> mutations{
      [](auto& start) { start.summary_intent.reset(); },
      [](auto& start) {
        start.attributes.purpose = domain::RunPurpose::conversation;
      },
      [](auto& start) {
        start.summary_intent->sources.session_id =
            id<domain::SessionId>("foreign");
      },
      [](auto& start) {
        start.summary_intent->producing_run_id = id<domain::RunId>("wrong");
      },
      [](auto& start) {
        start.summary_intent->producing_inference_id =
            id<domain::InferenceId>("wrong");
      },
      [](auto& start) {
        start.summary_intent->model_id = id<domain::ModelId>("wrong");
      },
      [](auto& start) {
        start.summary_intent->output_message_id =
            id<domain::MessageId>("wrong");
      },
      [](auto& start) {
        start.request.context.entries.back().message.content = {
            domain::TextBlock{"injected"}};
      },
      [](auto& start) {
        start.user_message.content = {domain::TextBlock{"composer draft"}};
      }};
  for (const auto& mutate : mutations) {
    Fixture fixture;
    auto request = fixture.request();
    mutate(request);
    fixture.reject(std::move(request));
  }
}

TEST_CASE("summary kernel rejects added tools imports memory continuation "
          "state and output cap mismatch",
          "[summarykernel][failure]") {
  const std::vector<Mutation> mutations{
      [](auto& start) {
        start.request.tools.push_back({"read",
                                       "Read",
                                       {"application/schema+json", "{}"},
                                       {domain::Effect::read},
                                       {}});
      },
      [](auto& start) {
        start.imported_artifacts.push_back({id<domain::ArtifactId>("artifact"),
                                            "text/plain",
                                            1,
                                            "digest",
                                            {},
                                            {},
                                            {}});
      },
      [](auto& start) {
        start.attributes.memory_selection.emplace();
        REQUIRE(
            domain::seal_memory_selection(*start.attributes.memory_selection));
      },
      [](auto& start) {
        start.request.assistant_continuation_state.push_back(
            {id<domain::MessageId>("old-assistant"), "old reasoning", {}});
      },
      [](auto& start) { start.request.options.max_output_tokens.reset(); },
      [](auto& start) { start.request.options.max_output_tokens = 0; },
      [](auto& start) { start.request.options.max_output_tokens = 999; }};
  for (const auto& mutate : mutations) {
    Fixture fixture;
    auto request = fixture.request();
    mutate(request);
    fixture.reject(std::move(request));
  }
}

TEST_CASE("summary kernel preserves history and performs no dispatch when "
          "atomic start append fails",
          "[summarykernel][storage]") {
  Fixture fixture;
  const auto request = fixture.request();
  fixture.store.fail_append = true;
  fixture.reject(request);
  CHECK(fixture.store.append_attempts == 1);
  CHECK(fixture.store.batches.empty());
  CHECK_FALSE(fixture.kernel->active_run_id());
}

TEST_CASE("summary intent is durably recorded in the initial transaction "
          "before provider dispatch",
          "[summarykernel][atomic]") {
  Fixture fixture;
  auto request = fixture.request();
  request.attributes.workspace_id = id<domain::WorkspaceId>("code");
  REQUIRE(fixture.kernel->start(request));
  fixture.drain();
  REQUIRE(fixture.backend.requests().size() == 1);
  CHECK(fixture.backend.intent_was_durable());
  REQUIRE_FALSE(fixture.store.batches.empty());
  const auto& batch = fixture.store.batches.front();
  auto index = [&](auto predicate) {
    const auto found = std::ranges::find_if(batch, predicate);
    REQUIRE(found != batch.end());
    return static_cast<std::size_t>(found - batch.begin());
  };
  const auto started = index([](const auto& event) {
    return std::holds_alternative<domain::RunStarted>(event.payload);
  });
  const auto intent = index([](const auto& event) {
    return std::holds_alternative<
        domain::ConversationSummaryGenerationIntentRecorded>(event.payload);
  });
  const auto user = index([](const auto& event) {
    return std::holds_alternative<domain::UserContentAdded>(event.payload);
  });
  const auto inference = index([](const auto& event) {
    return std::holds_alternative<domain::InferenceStarted>(event.payload);
  });
  CHECK(started < intent);
  CHECK(intent < user);
  CHECK(user < inference);
  CHECK(std::get<domain::ConversationSummaryGenerationIntentRecorded>(
            batch[intent].payload)
            .intent == request.summary_intent);
  const auto before = fixture.kernel->event_log().events();
  fixture.reopen();
  CHECK(fixture.kernel->event_log().events() == before);
  CHECK(fixture.backend.requests().size() == 1);
}

TEST_CASE("summary kernel will not execute a second inference after an "
          "unexpected tool response",
          "[summarykernel][continuation]") {
  Fixture fixture;
  fixture.backend.tools = true;
  const auto start = fixture.request();
  REQUIRE(fixture.kernel->start(start));
  fixture.drain();
  auto next = start.request;
  next.inference_id = id<domain::InferenceId>("second-inference");
  next.assistant_message_id = id<domain::MessageId>("second-output");
  const auto before = fixture.kernel->event_log().events();
  CHECK_FALSE(fixture.kernel->continue_run(start.run_id, next));
  CHECK(fixture.kernel->event_log().events() == before);
  CHECK(fixture.backend.requests().size() == 1);
}

TEST_CASE("summary start rejects a prospective catalog exceeding its event "
          "bound before append",
          "[summarykernel][bounds]") {
  Fixture fixture;
  while (fixture.store.history.size() < 65536)
    fixture.store.seed(domain::UnknownEvent{"metadata.observation"});
  fixture.reopen();
  const auto state =
      runtime::recorded_conversation_summaries(fixture.kernel->event_log());
  REQUIRE(state);
  const auto request = fixture.request();
  fixture.reject(request);
  CHECK(fixture.store.append_attempts == 0);
  CHECK(fixture.store.batches.empty());
}
