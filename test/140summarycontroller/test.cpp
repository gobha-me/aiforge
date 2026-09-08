#include "../131summarykernel/fixture.hpp"
#include "../135summarycontext/fixture.hpp"
#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/summary_controller.hpp>
#include <aiforge/testing/scripted_session_store.hpp>
#include <stop_token>

namespace {
using namespace aiforge;
using summary_kernel_test::id;
struct Fixture : summary_kernel_test::Fixture {
  std::optional<summary_context_test::Summary> summary;
  explicit Fixture(bool pin = false) {
    summary_context_test::Fixture source;
    source.source("old", std::string(2000, 'x'));
    summary = source.summary("summary", {id<domain::RunId>("old")});
    source.policy(domain::ConversationMode::rolling,
                  pin ? std::vector{id<domain::RunId>("old")}
                      : std::vector<domain::RunId>{});
    store.history = source.log.events();
    reopen();
  }
  auto change() const -> runtime::ConversationSummaryActivationChange {
    const auto state =
        runtime::recorded_conversation_summaries(kernel->event_log());
    REQUIRE(state);
    return {id<domain::RunId>("activate"),
            summary_kernel_test::attributes(domain::RunPurpose::control),
            kernel->event_log().last_sequence(),
            state->policy_revision,
            {summary->candidate.summary_id, summary->candidate.revision,
             *summary->candidate.candidate_digest},
            {}};
  }
};
auto request(const domain::ContextBuildInput& input)
    -> runtime::SummaryContextRequest {
  return {id<domain::ModelId>("model"), input};
}
} // namespace

TEST_CASE(
    "summary controller budget failure preserves the usable policy and draft",
    "[summarycontroller][failure]") {
  Fixture f;
  runtime::SummaryController controller{*f.kernel};
  auto input = summary_context_test::mandatory();
  input.capacity.context_window_tokens = 50;
  const auto before = f.kernel->event_log().events();
  const auto draft = input;
  const auto result = controller.preview(f.change(), request(input));
  REQUIRE_FALSE(result);
  CHECK(f.kernel->event_log().events() == before);
  CHECK(f.store.history == before);
  CHECK(f.store.append_attempts == 0);
  CHECK(f.backend.requests().empty());
  CHECK(input == draft);
}

TEST_CASE("summary controller apply rejects changed reviewed request inputs",
          "[summarycontroller][stale]") {
  Fixture f;
  runtime::SummaryController controller{*f.kernel};
  auto input = summary_context_test::mandatory();
  const auto preview = controller.preview(f.change(), request(input));
  REQUIRE(preview);
  auto fresh = request(input);
  SECTION("model") {
    fresh.model_id = id<domain::ModelId>("other");
  }
  SECTION("capacity") {
    input.capacity.context_window_tokens++;
  }
  SECTION("draft") {
    input.content.back().message.content = {domain::TextBlock{"Changed draft"}};
  }
  SECTION("estimate") {
    input.content.back().estimated_tokens++;
  }
  SECTION("history limits") {
    fresh.history_limits.maximum_events--;
  }
  SECTION("selection limits") {
    fresh.selection_limits.maximum_groups--;
  }
  const auto before = f.kernel->event_log().events();
  const auto result = controller.apply(*preview, fresh);
  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        runtime::SummaryControllerErrorCode::stale_review);
  CHECK(f.kernel->event_log().events() == before);
  CHECK(f.store.append_attempts == 0);
  CHECK(f.backend.requests().empty());
}

TEST_CASE("summary controller rejects reviews after edits or policy changes",
          "[summarycontroller][stale]") {
  Fixture f;
  runtime::SummaryController controller{*f.kernel};
  const auto input = summary_context_test::mandatory();
  const auto change = f.change();
  const auto preview = controller.preview(change, request(input));
  REQUIRE(preview);
  SECTION("candidate edited") {
    REQUIRE(controller.edit({id<domain::RunId>("edit"), change.attributes,
                             change.expected_sequence, change.candidate,
                             "New reviewed facts."}));
  }
  SECTION("policy changed") {
    REQUIRE(f.kernel->record_conversation_policy(
        {id<domain::RunId>("policy-change"),
         change.attributes,
         change.expected_policy_revision,
         domain::ConversationMode::full,
         {}}));
  }
  const auto before = f.kernel->event_log().events();
  const auto attempts = f.store.append_attempts;
  const auto result = controller.apply(*preview, request(input));
  REQUIRE_FALSE(result);
  CHECK(result.error().code ==
        runtime::SummaryControllerErrorCode::stale_review);
  CHECK(f.kernel->event_log().events() == before);
  CHECK(f.store.append_attempts == attempts);
  CHECK(f.backend.requests().empty());
}

TEST_CASE("summary controller activation append failure leaves policy intact",
          "[summarycontroller][failure]") {
  Fixture f;
  runtime::SummaryController controller{*f.kernel};
  const auto input = summary_context_test::mandatory();
  const auto preview = controller.preview(f.change(), request(input));
  REQUIRE(preview);
  const auto before = f.kernel->event_log().events();
  f.store.fail_append = true;
  const auto result = controller.apply(*preview, request(input));
  REQUIRE_FALSE(result);
  CHECK(f.kernel->event_log().events() == before);
  CHECK(f.store.history == before);
  CHECK(f.store.append_attempts == 1);
  CHECK(f.backend.requests().empty());
}

TEST_CASE("summary controller refuses bounded source and input failures before "
          "copying",
          "[summarycontroller][failure]") {
  Fixture f;
  runtime::SummaryController controller{*f.kernel};
  auto input = summary_context_test::mandatory();
  auto current = request(input);
  SECTION("event bound") {
    current.history_limits.maximum_events = 1;
  }
  SECTION("byte bound") {
    current.history_limits.maximum_content_bytes = 1;
  }
  SECTION("entry bound") {
    current.selection_limits.maximum_entries = 1;
  }
  const auto before = f.kernel->event_log().events();
  CHECK_FALSE(controller.preview(f.change(), current));
  CHECK(f.kernel->event_log().events() == before);
  CHECK(f.store.append_attempts == 0);
  CHECK(f.backend.requests().empty());
}

TEST_CASE("summary controller preview is cancelled without persistence",
          "[summarycontroller][failure]") {
  Fixture f;
  runtime::SummaryController controller{*f.kernel};
  const auto input = summary_context_test::mandatory();
  std::stop_source stop;
  stop.request_stop();
  const auto result =
      controller.preview(f.change(), request(input), stop.get_token());
  REQUIRE_FALSE(result);
  CHECK(result.error().code == runtime::SummaryControllerErrorCode::cancelled);
  CHECK(f.store.append_attempts == 0);
  CHECK(f.backend.requests().empty());
}

TEST_CASE(
    "summary controller review preserves pins and commits only activation",
    "[summarycontroller][success]") {
  Fixture f{true};
  runtime::SummaryController controller{*f.kernel};
  const auto input = summary_context_test::mandatory();
  const auto before = f.kernel->event_log().events();
  const auto preview = controller.preview(f.change(), request(input));
  REQUIRE(preview);
  CHECK(f.kernel->event_log().events() == before);
  CHECK(f.store.append_attempts == 0);
  CHECK(f.backend.requests().empty());
  const auto& context = preview->context();
  REQUIRE(context.conversation_admission.summaries.size() == 1);
  REQUIRE(context.conversation_admission.groups.size() == 1);
  CHECK(context.conversation_admission.groups.front().pinned);
  CHECK(context.conversation_admission.mandatory_input_tokens ==
        20 + context.conversation_admission.summaries.front().estimated_tokens);
  REQUIRE(context.input.content.size() == 3);
  CHECK(context.input.content.back().message == input.content.back().message);
  const auto applied = controller.apply(*preview, request(input));
  REQUIRE(applied);
  CHECK(*applied == preview->activation());
  CHECK(f.kernel->event_log().events().size() == before.size() + 3);
  CHECK(f.store.append_attempts == 1);
  CHECK(f.backend.requests().empty());
  CHECK_FALSE(controller.apply(*preview, request(input)));
  CHECK(f.store.append_attempts == 1);
}

TEST_CASE(
    "summary controller forces read-only memory selection on preview and apply",
    "[summarycontroller][memory]") {
  Fixture f;
  runtime::SummaryController controller{*f.kernel};
  const testing::SessionStoreExchange lookup{
      testing::FindMemoryJournalCall{domain::MemoryOwner::global()},
      std::optional<storage::SessionInfo>{}};
  testing::ScriptedSessionStore memory_store{{lookup, lookup}};
  std::uint64_t identities{};
  runtime::MemoryController memory{memory_store, [&] { return ++identities; },
                                   [] { return domain::EventTimestamp{}; }};
  const auto input = summary_context_test::mandatory();
  auto current = request(input);
  current.memory_controller = &memory;
  CHECK_FALSE(current.memory.read_only);
  const auto preview = controller.preview(f.change(), current);
  REQUIRE(preview);
  CHECK(memory_store.recorded_calls().size() == 1);
  CHECK(identities == 0);
  CHECK(f.store.append_attempts == 0);
  REQUIRE(controller.apply(*preview, current));
  CHECK(memory_store.recorded_calls().size() == 2);
  CHECK(identities == 0);
  CHECK(f.backend.requests().empty());
  CHECK_FALSE(current.memory.read_only);
}

TEST_CASE("summary context preview refuses forged prospective suffixes",
          "[summarycontroller][failure]") {
  Fixture f;
  auto prospective = f.kernel->preview_conversation_summary(f.change());
  REQUIRE(prospective);
  auto suffix = prospective->events;
  SECTION("partial") {
    suffix.pop_back();
  }
  SECTION("sequence gap") {
    suffix[1].metadata.sequence++;
  }
  SECTION("unknown schema") {
    suffix[1].metadata.schema_version = 99;
  }
  SECTION("duplicate identity") {
    suffix[1].metadata.event_id = suffix[0].metadata.event_id;
  }
  SECTION("conversation start") {
    std::get<domain::RunStarted>(suffix[0].payload).purpose =
        domain::RunPurpose::conversation;
  }
  SECTION("foreign control run") {
    suffix[2].metadata.run_id = id<domain::RunId>("foreign");
  }
  const auto before = f.kernel->event_log().events();
  const auto input = summary_context_test::mandatory();
  const auto result = runtime::preview_session_context_after_summary_activation(
      {f.kernel->event_log(),
       id<domain::ModelId>("model"),
       input,
       nullptr,
       {},
       {},
       {}},
      suffix);
  CHECK_FALSE(result);
  CHECK(f.kernel->event_log().events() == before);
  CHECK(f.store.append_attempts == 0);
  CHECK(f.backend.requests().empty());
}
