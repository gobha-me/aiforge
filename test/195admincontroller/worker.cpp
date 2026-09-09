#include "fixture.hpp"
#include <limits>

using namespace admin_controller_test;
namespace {
class StalledFolder final : public runtime::LocalSourceGrantFactory {
 public:
  std::shared_ptr<Gate> gate{std::make_shared<Gate>()};
  auto guarantees_pinned_read_only_sources() const noexcept -> bool override {
    return true;
  }
  auto grant(const runtime::LocalFolderGrantRequest&, std::stop_token)
      -> std::expected<runtime::LocalFolderGrantResult,
                       LocalSourceError> override {
    gate->wait();
    return std::unexpected(LocalSourceError{});
  }
};
} // namespace
TEST_CASE("Exact Admin preparation binds while an unrelated shared job remains "
          "stalled",
          "[admin][controller]") {
  Fixture f;
  f.open();
  auto folder = std::make_shared<StalledFolder>();
  const Release release{folder->gate};
  auto sequence = f.worker->allocate_request_id();
  REQUIRE(sequence);
  const auto request = folder_grant_test::request(*sequence);
  REQUIRE(f.worker->submit(folder, request));
  REQUIRE(folder->gate->await());
  f.select();
  f.ready();
  CHECK(f.binding.calls == 1);
  CHECK(f.worker->occupied_slots() == 1);
  CHECK(f.catalog->state->observed == 0);
  REQUIRE(f.worker->cancel(request.token));
  folder->gate->release();
  REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
}

TEST_CASE("A foreign endpoint remains subject to the final native binding gate",
          "[admin][controller]") {
  Fixture f;
  f.open();
  auto other = runtime::OpsObservationBroker::create(f.worker);
  REQUIRE(other);
  auto foreign = (*other)->activate_session(f.session);
  REQUIRE(foreign);
  REQUIRE(f.controller->attach({f.session, f.manual, f.binding, *foreign}));
  f.select();
  REQUIRE(until([&] {
    static_cast<void>(f.controller->pump());
    return f.controller->inspect().problem.has_value();
  }));
  CHECK(f.binding.calls == 1);
  CHECK_FALSE(f.controller->inspect().active_target);
  CHECK_FALSE(f.manual.authority);
  CHECK(f.catalog->state->observed == 0);
}

TEST_CASE(
    "Pending B disables fresh and cached A reads until selection resolves",
    "[admin][controller]") {
  Fixture f;
  f.open();
  f.select();
  f.ready();
  f.capture(AdminReadServices{});
  const auto old = *f.controller->inspect().snapshots[1];
  f.catalog->state = std::make_shared<PreparationState>();
  f.catalog->state->gate = std::make_shared<Gate>();
  const Release release{f.catalog->state->gate};
  f.select("beta");
  REQUIRE(f.catalog->state->gate->await());
  const auto calls = f.manual.intents.size();
  REQUIRE_FALSE(f.controller->execute(AdminReadHealth{}));
  REQUIRE_FALSE(
      f.controller->execute(AdminReadNamedService{"fixture.service"}));
  REQUIRE_FALSE(f.controller->execute(
      AdminReadCachedService{f.session, old.observation_event_id, 1, 0}));
  CHECK(f.manual.intents.size() == calls);
  CHECK(f.controller->inspect().active_target ==
        old.observation.request.target);
  CHECK(f.controller->inspect().pending_target == id<OpsTargetId>("beta"));
  f.catalog->state->gate->release();
}

TEST_CASE("Manual pump refusal cancels pending preparation before binding",
          "[admin][controller]") {
  Fixture f;
  f.open();
  f.select();
  REQUIRE(until([&] { return f.catalog->state->prepared != 0; }));
  const auto token = f.catalog->state->request().token;
  REQUIRE(until([&] {
    auto state = f.worker->preparation_state(token);
    return state && state->ready;
  }));
  f.manual.pump_failure = ManualOpsFailure{ManualOpsErrorCode::unavailable};
  REQUIRE_FALSE(f.controller->pump());
  CHECK_FALSE(f.controller->inspect().pending_target);
  CHECK(f.binding.calls == 0);
  f.manual.pump_failure.reset();
  REQUIRE(until([&] {
    REQUIRE(f.controller->pump());
    return f.worker->occupied_slots() == 0;
  }));
  CHECK(f.binding.calls == 0);
  CHECK(f.catalog->calls == 1);
}

TEST_CASE("Exhausted shared request identities refuse Admin selection before "
          "factory creation",
          "[admin][controller]") {
  Fixture f;
  f.open();
  auto folder = std::make_shared<StalledFolder>();
  const Release release{folder->gate};
  const auto request =
      folder_grant_test::request(std::numeric_limits<std::uint64_t>::max());
  REQUIRE(f.worker->submit(folder, request));
  REQUIRE(folder->gate->await());
  auto selected =
      f.controller->execute(AdminSelectTarget{id<OpsTargetId>("alpha")});
  REQUIRE_FALSE(selected);
  CHECK(selected.error().code == ManualOpsErrorCode::resource_exhausted);
  CHECK(f.controller->inspect().fatal);
  CHECK(f.catalog->calls == 0);
  CHECK(f.catalog->state->prepared == 0);
  REQUIRE(f.worker->cancel(request.token));
  folder->gate->release();
  REQUIRE(until([&] { return f.worker->occupied_slots() == 0; }));
}
