#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/runtime/ops_observation_history.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <limits>

namespace {
using namespace aiforge;
using namespace aiforge::domain;
template <class T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
auto target(std::string name = "cluster") -> OpsTargetBinding {
  return {id<OpsTargetId>(std::move(name)),
          id<OpsConfigurationRevision>("config-r1"),
          KubernetesOpsIdentity{
              "production", "payments", {"10.0.0.2", 6443}, "sha256:fixture"}};
}
struct History {
  SessionEventLog log{id<SessionId>("session")};
  std::uint64_t event{};
  auto push(const RunId& run, RunEventPayload payload, std::uint32_t schema = 1,
            std::optional<InvocationId> invocation = {}) -> void {
    const auto sequence = ++event;
    REQUIRE(log.append({{id<EventId>("event-" + std::to_string(sequence)),
                         run,
                         sequence,
                         schema,
                         EventTimestamp{std::chrono::milliseconds{sequence}},
                         {},
                         {},
                         std::move(invocation)},
                        std::move(payload)}));
  }
  auto select(std::string run_name, OpsTargetBinding selected,
              std::uint64_t generation) -> void {
    const auto run = id<RunId>(std::move(run_name));
    push(run,
         RunStarted{id<SurfaceId>("admin"),
                    id<WorkspaceId>("ops"),
                    id<PermissionProfileId>("observe"),
                    {},
                    {},
                    RunPurpose::control},
         3);
    push(run, OpsTargetSelected{std::move(selected), generation});
    push(run, RunCompleted{});
  }
};
} // namespace

TEST_CASE("Target selection projection retains exact identity and next same ID",
          "[ops][target-selection]") {
  History history;
  const auto first = target();
  history.select("selection-1", first, 1);
  auto second = first;
  second.configuration_revision = id<OpsConfigurationRevision>("config-r2");
  history.select("selection-2", second, 2);

  const auto projected = runtime::recorded_ops_observations(history.log);
  REQUIRE(projected);
  REQUIRE(projected->latest_selection);
  CHECK(projected->latest_selection->run_id == id<RunId>("selection-2"));
  CHECK(projected->latest_selection->target == second);
  CHECK(projected->latest_selection->selection_generation == 2);
  CHECK(projected->maximum_selection_generation == 2);
  CHECK(projected->invocations.empty());
}

TEST_CASE("Target selection projection rejects malformed control ordering",
          "[ops][target-selection]") {
  History history;
  const auto run = id<RunId>("selection");
  history.push(run,
               RunStarted{id<SurfaceId>("admin"),
                          id<WorkspaceId>("ops"),
                          id<PermissionProfileId>("observe"),
                          {},
                          {},
                          RunPurpose::control},
               3);
  history.push(run, OpsTargetSelected{target(), 1});
  history.push(run, RunCompleted{});
  auto events = history.log.events();
  SECTION("missing completion") {
    events.pop_back();
  }
  SECTION("selection before start") {
    std::swap(events[0].payload, events[1].payload);
    std::swap(events[0].metadata.schema_version,
              events[1].metadata.schema_version);
  }
  SECTION("ordinary content in selection run") {
    events.insert(events.begin() + 2, events.front());
    events[2].payload = RunAwaitingInput{id<QuestionId>("question")};
    events[2].metadata.schema_version = 1;
    for (std::size_t index = 0; index < events.size(); ++index) {
      events[index].metadata.sequence = index + 1;
      events[index].metadata.event_id =
          id<EventId>("mixed-" + std::to_string(index));
    }
  }
  SECTION("unknown content before selection") {
    events.insert(events.begin() + 1, events.front());
    events[1].payload = UnknownEvent{"future.selection"};
    events[1].metadata.schema_version = 9;
    for (std::size_t index = 0; index < events.size(); ++index) {
      events[index].metadata.sequence = index + 1;
      events[index].metadata.event_id =
          id<EventId>("unknown-before-" + std::to_string(index));
    }
  }
  SECTION("unknown content after selection") {
    events.insert(events.begin() + 2, events.front());
    events[2].payload = UnknownEvent{"future.selection"};
    events[2].metadata.schema_version = 9;
    for (std::size_t index = 0; index < events.size(); ++index) {
      events[index].metadata.sequence = index + 1;
      events[index].metadata.event_id =
          id<EventId>("unknown-after-" + std::to_string(index));
    }
  }
  SECTION("duplicate selection in one run") {
    events.insert(events.begin() + 2, events[1]);
    for (std::size_t index = 0; index < events.size(); ++index) {
      events[index].metadata.sequence = index + 1;
      events[index].metadata.event_id =
          id<EventId>("duplicate-" + std::to_string(index));
    }
  }
  SECTION("invocation metadata") {
    events[1].metadata.invocation_id = id<InvocationId>("forged");
  }
  SECTION("start cause metadata") {
    events[0].metadata.caused_by_event_id = id<EventId>("forged-cause");
  }
  SECTION("start parent metadata") {
    events[0].metadata.parent_run_id = id<RunId>("forged-parent");
  }
  SECTION("start invocation metadata") {
    events[0].metadata.invocation_id = id<InvocationId>("forged");
  }
  SECTION("selection cause metadata") {
    events[1].metadata.caused_by_event_id = id<EventId>("forged-cause");
  }
  SECTION("selection parent metadata") {
    events[1].metadata.parent_run_id = id<RunId>("forged-parent");
  }
  SECTION("completion cause metadata") {
    events[2].metadata.caused_by_event_id = id<EventId>("forged-cause");
  }
  SECTION("completion parent metadata") {
    events[2].metadata.parent_run_id = id<RunId>("forged-parent");
  }
  SECTION("completion invocation metadata") {
    events[2].metadata.invocation_id = id<InvocationId>("forged");
  }
  SECTION("conversation start") {
    std::get<RunStarted>(events[0].payload).purpose = RunPurpose::conversation;
    events[0].metadata.schema_version = 1;
  }
  SECTION("invalid target") {
    std::get<OpsTargetSelected>(events[1].payload).target.identity =
        KubernetesOpsIdentity{
            "", "payments", {"10.0.0.2", 6443}, "sha256:fixture"};
  }
  SessionEventLog malformed{id<SessionId>("session")};
  for (auto& event : events)
    REQUIRE(malformed.append(std::move(event)));
  REQUIRE_FALSE(runtime::recorded_ops_observations(malformed));
}

TEST_CASE("Target selection generations reject duplicate stale and skipped",
          "[ops][target-selection]") {
  History history;
  history.select("selection-1", target(), 1);
  auto generation = std::uint64_t{2};
  SECTION("duplicate") {
    generation = 1;
  }
  SECTION("stale") {
    generation = 0;
  }
  SECTION("skipped") {
    generation = 3;
  }
  history.select("selection-2", target("other"), generation);
  REQUIRE_FALSE(runtime::recorded_ops_observations(history.log));
}

TEST_CASE("Exhausted target generation cannot advance",
          "[ops][target-selection]") {
  History history;
  history.select("selection-max", target(),
                 std::numeric_limits<std::uint64_t>::max());
  history.select("selection-after-max", target("other"), 1);
  REQUIRE_FALSE(runtime::recorded_ops_observations(history.log));
}

TEST_CASE("Interrupted unrelated work does not convert historical selection",
          "[ops][target-selection]") {
  History history;
  const auto selected = target();
  history.select("selection-1", selected, 1);
  const auto interrupted = id<RunId>("interrupted-chat");
  history.push(interrupted,
               RunStarted{id<SurfaceId>("chat"),
                          id<WorkspaceId>("chat"),
                          id<PermissionProfileId>("chat"),
                          {}},
               1);
  const auto projected = runtime::recorded_ops_observations(history.log);
  REQUIRE(projected);
  REQUIRE(projected->latest_selection);
  CHECK(projected->latest_selection->target == selected);
  CHECK(projected->latest_selection->selection_generation == 1);
}

TEST_CASE("Unrelated future events remain skippable before selection",
          "[ops][target-selection][compatibility]") {
  History history;
  history.push(id<RunId>("future-run"), UnknownEvent{"future.event"}, 9);
  const auto selected = target();
  history.select("selection-1", selected, 1);

  const auto projected = runtime::recorded_ops_observations(history.log);
  REQUIRE(projected);
  REQUIRE(projected->latest_selection);
  CHECK(projected->latest_selection->target == selected);
  CHECK(projected->latest_selection->selection_generation == 1);
}

TEST_CASE("SQLite round trip preserves exact target selection payload",
          "[ops][target-selection][sqlite]") {
  auto pattern =
      (std::filesystem::temp_directory_path() / "aiforge-ops-selection-XXXXXX")
          .string();
  REQUIRE(::mkdtemp(pattern.data()) != nullptr);
  const std::filesystem::path directory{pattern};
  auto opened = adapters::SqliteSessionStore::open(directory / "sessions.db");
  REQUIRE(opened);
  auto& store = **opened;
  const auto session = id<SessionId>("session");
  REQUIRE(store.create_session({session, {}}));
  History history;
  history.select("selection-1", target(), 1);
  REQUIRE(store.append_events(session, history.log.events()));
  auto replayed = store.replay_events(session);
  REQUIRE(replayed);
  CHECK(*replayed == history.log.events());
  opened->reset();
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  CHECK_FALSE(error);
}
