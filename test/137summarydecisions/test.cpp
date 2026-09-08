#include "../131summarykernel/fixture.hpp"
#include <aiforge/runtime/conversation_policy.hpp>
#include <aiforge/runtime/conversation_summary_context.hpp>

namespace {
using namespace summary_kernel_test;
auto version(const domain::ConversationSummaryCandidate& candidate)
    -> domain::ConversationSummaryVersion {
  REQUIRE(candidate.candidate_digest);
  return {candidate.summary_id, candidate.revision,
          *candidate.candidate_digest};
}
auto publish(Fixture& fixture) -> domain::ConversationSummaryCandidate {
  fixture.complete();
  auto candidate = fixture.kernel->publish_conversation_summary(
      {id<domain::RunId>("publish"), attributes(domain::RunPurpose::control),
       id<domain::ConversationSummaryId>("summary")});
  REQUIRE(candidate);
  return *candidate;
}
auto activation(Fixture& fixture,
                const domain::ConversationSummaryCandidate& candidate,
                std::string name = "apply")
    -> runtime::ConversationSummaryActivationChange {
  auto policy =
      runtime::recorded_conversation_policy(fixture.kernel->event_log());
  REQUIRE(policy);
  return {id<domain::RunId>(name),
          attributes(domain::RunPurpose::control),
          fixture.kernel->event_log().last_sequence(),
          policy->policy.revision,
          version(candidate),
          {}};
}
auto apply(Fixture& fixture,
           const domain::ConversationSummaryCandidate& candidate)
    -> domain::ConversationSummaryActivation {
  auto applied = fixture.kernel->activate_conversation_summary(
      activation(fixture, candidate));
  REQUIRE(applied);
  return *applied;
}
auto disable(Fixture& fixture,
             const domain::ConversationSummaryActivation& active)
    -> runtime::ConversationSummaryDisableChange {
  auto policy =
      runtime::recorded_conversation_policy(fixture.kernel->event_log());
  REQUIRE(policy);
  return {id<domain::RunId>("disable"),
          attributes(domain::RunPurpose::control),
          fixture.kernel->event_log().last_sequence(),
          policy->policy.revision,
          active.candidate,
          active.activation_event_id};
}
} // namespace

TEST_CASE("summary apply rejects stale review foreign candidates and invalid "
          "control without changes",
          "[summarydecisions][failure]") {
  Fixture f;
  const auto candidate = publish(f);
  const auto before = f.kernel->event_log().events();
  auto change = activation(f, candidate);
  SECTION("stale event snapshot") {
    --change.expected_sequence;
  }
  SECTION("stale policy revision") {
    ++change.expected_policy_revision;
  }
  SECTION("unpublished version") {
    ++change.candidate.revision;
  }
  SECTION("forged candidate digest") {
    change.candidate.candidate_digest.value[0] = 'x';
  }
  SECTION("ordinary run") {
    change.attributes.purpose = domain::RunPurpose::conversation;
  }
  SECTION("reused run") {
    change.run_id = id<domain::RunId>("publish");
  }
  SECTION("replacement absent from active set") {
    change.replaced_versions = {version(candidate)};
  }
  SECTION("too many replacements") {
    change.replaced_versions.assign(33, version(candidate));
  }
  REQUIRE_FALSE(f.kernel->activate_conversation_summary(change));
  CHECK(f.kernel->event_log().events() == before);
  CHECK(f.store.history == before);
  CHECK(f.backend.requests().size() == 1);
}

TEST_CASE("summary apply and disable append failures preserve the last durable "
          "policy",
          "[summarydecisions][storage]") {
  Fixture f;
  const auto candidate = publish(f);
  SECTION("failed initial apply") {
    const auto before = f.store.history;
    f.store.fail_append = true;
    CHECK_FALSE(
        f.kernel->activate_conversation_summary(activation(f, candidate)));
    CHECK(f.store.history == before);
    f.store.fail_append = false;
    f.reopen();
    auto state =
        runtime::recorded_conversation_summaries(f.kernel->event_log());
    REQUIRE(state);
    CHECK(state->active.empty());
    CHECK(state->policy_revision == 0);
  }
  SECTION("failed disable") {
    const auto active = apply(f, candidate);
    const auto before = f.store.history;
    f.store.fail_append = true;
    CHECK_FALSE(f.kernel->disable_conversation_summary(disable(f, active)));
    CHECK(f.store.history == before);
    f.store.fail_append = false;
    f.reopen();
    auto state =
        runtime::recorded_conversation_summaries(f.kernel->event_log());
    REQUIRE(state);
    REQUIRE(state->active.size() == 1);
    CHECK(state->active.front() == active);
    CHECK(state->policy_revision == 1);
  }
  CHECK(f.backend.requests().size() == 1);
}

TEST_CASE(
    "summary edits require explicit exact supersession of overlapping coverage",
    "[summarydecisions][replacement]") {
  Fixture f;
  const auto original = publish(f);
  const auto first = apply(f, original);
  auto edited = f.kernel->edit_conversation_summary(
      {id<domain::RunId>("edit"), attributes(domain::RunPurpose::control),
       f.kernel->event_log().last_sequence(), version(original),
       "Reviewed task: one uncertainty remains."});
  REQUIRE(edited);
  const auto before = f.store.history;
  CHECK_FALSE(f.kernel->activate_conversation_summary(
      activation(f, original, "stale-apply")));
  CHECK_FALSE(f.kernel->activate_conversation_summary(
      activation(f, *edited, "overlap")));
  CHECK(f.store.history == before);
  auto change = activation(f, *edited, "replace");
  change.replaced_versions = {first.candidate};
  const auto replaced = f.kernel->activate_conversation_summary(change);
  REQUIRE(replaced);
  auto state = runtime::recorded_conversation_summaries(f.kernel->event_log());
  REQUIRE(state);
  REQUIRE(state->active.size() == 1);
  CHECK(state->active.front() == *replaced);
  CHECK(state->candidates.size() == 2);
  CHECK(state->policy_revision == 2);
}

TEST_CASE("summary disable requires the exact current activation identity",
          "[summarydecisions][failure]") {
  Fixture f;
  const auto candidate = publish(f);
  const auto active = apply(f, candidate);
  auto change = disable(f, active);
  const auto before = f.store.history;
  SECTION("foreign activation") {
    change.activation_event_id = id<domain::EventId>("other");
  }
  SECTION("foreign candidate") {
    ++change.candidate.revision;
  }
  SECTION("stale revision") {
    --change.expected_policy_revision;
  }
  SECTION("stale preview") {
    --change.expected_sequence;
  }
  CHECK_FALSE(f.kernel->disable_conversation_summary(change));
  CHECK(f.store.history == before);
}

TEST_CASE("reactivation cannot restore a disabled unresolved summary admission",
          "[summarydecisions][recovery]") {
  Fixture f;
  const auto candidate = publish(f);
  REQUIRE(f.kernel->record_conversation_policy(
      {id<domain::RunId>("rolling"),
       attributes(domain::RunPurpose::control),
       0,
       domain::ConversationMode::rolling,
       {}}));
  const auto first = apply(f, candidate);
  const auto before_disable = f.kernel->event_log().last_sequence();
  const auto saved =
      runtime::prepare_conversation_summary_context(f.kernel->event_log());
  REQUIRE(saved);
  REQUIRE(f.kernel->disable_conversation_summary(disable(f, first)));
  const auto second = f.kernel->activate_conversation_summary(
      activation(f, candidate, "reactivate"));
  REQUIRE(second);
  CHECK(second->activation_event_id != first.activation_event_id);
  CHECK_FALSE(runtime::recover_conversation_summary_context(
      f.kernel->event_log(), saved->summaries, before_disable));
  CHECK(runtime::recover_conversation_summary_context(
      f.kernel->event_log(), saved->summaries, before_disable, {}, {}, false));
  CHECK(f.backend.requests().size() == 1);
}

TEST_CASE("summary decisions preserve mode pins original history and full-mode "
          "dormant records",
          "[summarydecisions][smoke]") {
  Fixture f;
  const auto candidate = publish(f);
  REQUIRE(f.kernel->record_conversation_policy(
      {id<domain::RunId>("rolling"),
       attributes(domain::RunPurpose::control),
       0,
       domain::ConversationMode::rolling,
       {id<domain::RunId>("source-run")}}));
  const auto active = apply(f, candidate);
  auto policy = runtime::recorded_conversation_policy(f.kernel->event_log());
  REQUIRE(policy);
  CHECK(policy->policy.mode == domain::ConversationMode::rolling);
  CHECK(policy->policy.pinned_run_ids ==
        std::vector{id<domain::RunId>("source-run")});
  REQUIRE(f.kernel->record_conversation_policy(
      {id<domain::RunId>("full"),
       attributes(domain::RunPurpose::control),
       policy->policy.revision,
       domain::ConversationMode::full,
       {id<domain::RunId>("source-run")}}));
  const auto restored =
      runtime::recorded_conversation_summaries(f.kernel->event_log());
  REQUIRE(restored);
  REQUIRE(restored->active.size() == 1);
  CHECK(restored->active.front() == active);
  REQUIRE(f.kernel->disable_conversation_summary(disable(f, active)));
  const auto history =
      runtime::reconstruct_conversation_history({f.kernel->event_log()});
  REQUIRE(history);
  REQUIRE(history->size() == 1);
  CHECK(history->front().run_id == id<domain::RunId>("source-run"));
}

TEST_CASE("summary activation preview is an inert proposal with the eventual "
          "real identity",
          "[summarydecisions][preview]") {
  Fixture f;
  const auto candidate = publish(f);
  const auto change = activation(f, candidate);
  const auto before = f.store.history;
  const auto preview = f.kernel->preview_conversation_summary(change);
  REQUIRE(preview);
  REQUIRE(preview->events.size() == 3);
  CHECK(f.store.history == before);
  CHECK(f.kernel->event_log().events() == before);
  CHECK(f.backend.requests().size() == 1);
  const auto state =
      runtime::recorded_conversation_summaries(f.kernel->event_log());
  REQUIRE(state);
  CHECK(state->active.empty());
  const auto applied = f.kernel->activate_conversation_summary(change);
  REQUIRE(applied);
  CHECK(*applied == preview->activation);
}

TEST_CASE(
    "prospective summary replay rejects malformed or foreign control suffixes",
    "[summarydecisions][preview][failure]") {
  Fixture f;
  const auto candidate = publish(f);
  auto proposed =
      f.kernel->preview_conversation_summary(activation(f, candidate));
  REQUIRE(proposed);
  auto& events = proposed->events;
  SECTION("truncated transaction") {
    events.pop_back();
  }
  SECTION("extra event") {
    events.push_back(events.back());
  }
  SECTION("reordered transaction") {
    std::swap(events[0], events[1]);
  }
  SECTION("unknown schema") {
    events[1].metadata.schema_version = 2;
  }
  SECTION("foreign run") {
    events[2].metadata.run_id = id<domain::RunId>("foreign");
  }
  SECTION("reused event") {
    events[2].metadata.event_id = events[0].metadata.event_id;
  }
  SECTION("durable event identity") {
    events[2].metadata.event_id = f.store.history.front().metadata.event_id;
  }
  SECTION("sequence gap") {
    ++events[2].metadata.sequence;
  }
  SECTION("invocation metadata") {
    events[1].metadata.invocation_id = id<domain::InvocationId>("foreign");
  }
  SECTION("causal metadata") {
    events[1].metadata.caused_by_event_id =
        f.store.history.front().metadata.event_id;
  }
  SECTION("parent metadata") {
    events[1].metadata.parent_run_id = id<domain::RunId>("parent");
  }
  SECTION("wrong revision") {
    ++std::get<domain::ConversationSummaryActivated>(events[1].payload)
          .previous_policy_revision;
  }
  SECTION("too many replacements") {
    auto& action =
        std::get<domain::ConversationSummaryActivated>(events[1].payload);
    action.replaced_versions.assign(33, action.activation.candidate);
  }
  const auto before = f.store.history;
  CHECK_FALSE(runtime::preview_conversation_summary_transition(
      f.kernel->event_log(), events));
  CHECK(f.store.history == before);
  CHECK(f.backend.requests().size() == 1);
}
