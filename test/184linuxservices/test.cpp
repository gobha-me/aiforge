// Failure matrix precedes smoke: invalid source/request, canonical path/Id,
// wrong property types, invocation substitution/restart, numeric bounds,
// output/capture/call limits, final proof, cancellation and concurrency.
#include "fixture.hpp"
#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <future>
#include <limits>

using namespace service_fixture;

TEST_CASE("Service source rejects invalid admission without opening a bus",
          "[linux-services]") {
  Fixture fixture;
  auto request = fixture.request();
  auto expected = Error::invalid_result;
  std::stop_source stop;
  SECTION("foreign immutable target") {
    request.target.configuration_revision =
        id<domain::OpsConfigurationRevision>("other");
    expected = Error::source_changed;
  }
  SECTION("invalid resource") {
    std::get<domain::LinuxServiceIdentity>(request.resource).unit_name =
        "--help";
  }
  SECTION("noncanonical explicit invocation") {
    std::get<domain::LinuxServiceIdentity>(request.resource).invocation_id =
        id<domain::OpsResourceUid>("not-an-invocation");
  }
  SECTION("zero explicit invocation") {
    std::get<domain::LinuxServiceIdentity>(request.resource).invocation_id =
        id<domain::OpsResourceUid>(std::string(32, '0'));
  }
  SECTION("logs require a selected invocation") {
    request.operation = domain::OpsObservationOperation::linux_service_logs;
  }
  SECTION("cancelled") {
    stop.request_stop();
    expected = Error::cancelled;
  }
  auto result = fixture.source->observe(request, stop.get_token());
  REQUIRE_FALSE(result);
  REQUIRE(result.error() == expected);
  REQUIRE(fixture.platform->state->opens == 0);
  REQUIRE(fixture.platform->state->sends == 0);
}
TEST_CASE("Procfs health never depends on the system bus", "[linux-services]") {
  Fixture fixture;
  fixture.platform->state->early_failure = Error::permission_denied;
  auto request = fixture.request(true);
  request.operation = domain::OpsObservationOperation::linux_health;
  auto observed = fixture.source->observe(request);
  REQUIRE(observed);
  REQUIRE(std::get<domain::LinuxHealthObservation>(observed->payload)
              .memory.has_value());
  REQUIRE(fixture.platform->state->opens == 0);
  REQUIRE(fixture.platform->state->sends == 0);
}
TEST_CASE(
    "Service canonical proof rejects substitution before other properties",
    "[linux-services]") {
  Fixture fixture;
  fixture.health();
  auto expected = Error::source_changed;
  unsigned calls{};
  SECTION("foreign object path") {
    fixture.platform->state->steps[4].reply =
        text("/foreign", DBUS_TYPE_OBJECT_PATH);
    calls = 5;
  }
  SECTION("wrong GetUnit type") {
    fixture.platform->state->steps[4].reply = text("/foreign");
    expected = Error::invalid_result;
    calls = 5;
  }
  SECTION("alias canonical Id") {
    fixture.platform->state->steps[5].reply = property("another.service");
    calls = 6;
  }
  auto observed = fixture.source->observe(fixture.request());
  REQUIRE_FALSE(observed);
  REQUIRE(observed.error() == expected);
  REQUIRE(fixture.platform->state->sends == calls);
}
TEST_CASE(
    "Service expected invocation rejects a foreign cycle before state reads",
    "[linux-services]") {
  Fixture fixture;
  fixture.health();
  auto request = fixture.request();
  std::get<domain::LinuxServiceIdentity>(request.resource).invocation_id =
      id<domain::OpsResourceUid>(std::string(32, '2'));
  auto observed = fixture.source->observe(request);
  REQUIRE_FALSE(observed);
  REQUIRE(observed.error() == Error::source_changed);
  REQUIRE(fixture.platform->state->sends == 7);
}
TEST_CASE("Service invocation shape and cycle changes never publish",
          "[linux-services]") {
  Unit unit;
  auto expected = Error::invalid_result;
  SECTION("short") {
    unit.before.resize(15);
  }
  SECTION("long") {
    unit.before.resize(17);
  }
  SECTION("all zero provided bytes") {
    unit.before.assign(16, 0);
  }
  SECTION("restart") {
    unit.after.assign(16, 2);
    expected = Error::source_changed;
  }
  SECTION("cycle disappears") {
    unit.after.clear();
    expected = Error::source_changed;
  }
  SECTION("cycle appears") {
    unit.before.clear();
    expected = Error::source_changed;
  }
  Fixture fixture;
  fixture.health(unit);
  auto observed = fixture.source->observe(fixture.request());
  REQUIRE_FALSE(observed);
  REQUIRE(observed.error() == expected);
}
TEST_CASE(
    "Service getters reject wrong variants and unknown required properties",
    "[linux-services]") {
  Fixture fixture;
  fixture.health();
  auto expected = Error::invalid_result;
  SECTION("status wrong scalar type") {
    fixture.platform->state->steps[10].reply =
        property_number(std::uint32_t{}, DBUS_TYPE_UINT32, "u");
  }
  SECTION("invocation string instead of bytes") {
    fixture.platform->state->steps[6].reply = property(std::string(32, '1'));
  }
  SECTION("extra top-level property field") {
    auto& message = fixture.platform->state->steps[7].reply;
    DBusMessageIter iterator;
    dbus_message_iter_init_append(message.get(), &iterator);
    append_text(iterator, "extra");
  }
  SECTION("unknown getter stays terminal unsupported") {
    Message error{dbus_message_new(DBUS_MESSAGE_TYPE_ERROR)};
    REQUIRE(error);
    REQUIRE(
        dbus_message_set_error_name(error.get(), DBUS_ERROR_UNKNOWN_PROPERTY));
    fixture.platform->state->steps[11].reply = std::move(error);
    expected = Error::unsupported;
  }
  auto observed = fixture.source->observe(fixture.request());
  REQUIRE_FALSE(observed);
  REQUIRE(observed.error() == expected);
}
TEST_CASE("Service numeric values cannot overflow neutral fields",
          "[linux-services]") {
  Unit unit;
  SECTION("negative status") {
    unit.status = -1;
  }
  SECTION("normal exit too large") {
    unit.code = CLD_EXITED;
    unit.status = 256;
  }
  SECTION("restart overflow") {
    unit.restarts =
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) +
        1;
  }
  Fixture fixture;
  fixture.health(unit);
  auto observed = fixture.source->observe(fixture.request());
  REQUIRE_FALSE(observed);
  REQUIRE(observed.error() == Error::invalid_result);
}
TEST_CASE("Service cancellation and final binding change return no evidence",
          "[linux-services]") {
  Fixture fixture;
  fixture.health();
  std::stop_source stop;
  auto expected = Error::source_changed;
  SECTION("final identity change") {
    fixture.platform->state->fail_final = true;
  }
  SECTION("cancellation after dispatch") {
    fixture.platform->state->stop = &stop;
    expected = Error::cancelled;
  }
  auto observed = fixture.source->observe(fixture.request(), stop.get_token());
  REQUIRE_FALSE(observed);
  REQUIRE(observed.error() == expected);
}
TEST_CASE("Original service deadline cannot restart during handshake reads or "
          "final proof",
          "[linux-services]") {
  Fixture fixture;
  fixture.health();
  std::size_t step{};
  SECTION("handshake") {
    step = 0;
  }
  SECTION("property read") {
    step = 8;
  }
  SECTION("final proof") {
    step = 15;
  }
  adapters::LinuxSystemdBudget budget{
      std::chrono::steady_clock::now() + std::chrono::seconds{5}, {}};
  fixture.platform->state->expire_budget = &budget;
  fixture.platform->state->expire_on_receive = step;
  auto observed = adapters::observe_linux_systemd_services(
      fixture.request(), *fixture.bus, budget,
      domain::EventTimestamp{std::chrono::milliseconds{1}});
  REQUIRE_FALSE(observed);
  REQUIRE(observed.error() == Error::timed_out);
  REQUIRE(fixture.platform->state->sends == step + 1);
}
TEST_CASE("Captured quota includes handshake unrelated messages and ignored "
          "list fields",
          "[linux-services]") {
  Fixture fixture;
  auto request = fixture.request();
  SECTION("handshake plus unrelated replies") {
    fixture.health();
    fixture.platform->state->unrelated = 6;
    request.limits.maximum_bytes = 2048;
  }
  SECTION("private list description is still charged") {
    Unit unit;
    unit.description.assign(4096, 'x');
    fixture.discovery({unit}, 1);
    request = fixture.request(true);
    request.limits.maximum_bytes = 4096;
  }
  auto observed = fixture.source->observe(request);
  REQUIRE_FALSE(observed);
  REQUIRE(observed.error() == Error::resource_exhausted);
  REQUIRE(fixture.platform->state->sends <= 5);
}
TEST_CASE("Discovery validates duplicate and foreign omitted tail rows",
          "[linux-services]") {
  Unit first;
  Unit second;
  second.name = "other.service";
  auto expected = Error::invalid_result;
  SECTION("duplicate beyond requested prefix") {
    second.name = first.name;
  }
  SECTION("foreign path beyond requested prefix") {
    second.path_override = "/foreign";
    expected = Error::source_changed;
  }
  Fixture fixture;
  fixture.discovery({first, second}, 1);
  auto request = fixture.request(true);
  request.limits.maximum_entries = 1;
  auto observed = fixture.source->observe(request);
  REQUIRE_FALSE(observed);
  REQUIRE(observed.error() == expected);
  REQUIRE(fixture.platform->state->sends == 5);
}
TEST_CASE("Discovery reports known omissions and retains exact invocation",
          "[linux-services]") {
  Unit first;
  Unit second;
  second.name = "other.service";
  Fixture fixture;
  fixture.discovery({first, second}, 1);
  auto request = fixture.request(true);
  request.limits.maximum_entries = 1;
  auto observed = fixture.source->observe(request);
  REQUIRE(observed);
  REQUIRE(observed->completeness ==
          domain::OpsObservationCompleteness::truncated);
  REQUIRE(observed->omitted_entries == 1);
  const auto& rows =
      std::get<domain::LinuxServicesObservation>(observed->payload).services;
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0].identity.invocation_id.has_value());
  REQUIRE(rows[0].identity.invocation_id->value() ==
          "01010101010101010101010101010101");
  REQUIRE_FALSE(rows[0].restart_count);
  REQUIRE_FALSE(rows[0].exit_status);
}
TEST_CASE("Discovery reserves final proof at exact call and capture boundaries",
          "[linux-services]") {
  Unit first;
  Unit second;
  second.name = "other.service";
  Fixture fixture;
  fixture.discovery({first, second}, 1);
  auto request = fixture.request(true);
  adapters::LinuxSystemdBudget measured{
      std::chrono::steady_clock::now() + std::chrono::seconds{5}, {}, 13};
  const auto initial = measured.remaining_captured_bytes;
  auto observed = adapters::observe_linux_systemd_services(
      request, *fixture.bus, measured,
      domain::EventTimestamp{std::chrono::milliseconds{1}});
  REQUIRE(observed);
  REQUIRE(measured.remaining_calls == 0);
  REQUIRE(observed->omitted_entries == 1);
  const auto consumed = initial - measured.remaining_captured_bytes;
  adapters::LinuxSystemdBudget exact{std::chrono::steady_clock::now() +
                                         std::chrono::seconds{5},
                                     {},
                                     13,
                                     consumed};
  auto repeated = adapters::observe_linux_systemd_services(
      request, *fixture.bus, exact,
      domain::EventTimestamp{std::chrono::milliseconds{1}});
  REQUIRE(repeated);
  REQUIRE(exact.remaining_captured_bytes == 0);
  adapters::LinuxSystemdBudget short_budget{std::chrono::steady_clock::now() +
                                                std::chrono::seconds{5},
                                            {},
                                            13,
                                            consumed - 1};
  auto rejected = adapters::observe_linux_systemd_services(
      request, *fixture.bus, short_budget,
      domain::EventTimestamp{std::chrono::milliseconds{1}});
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error() == Error::resource_exhausted);
}
TEST_CASE("Service signals unknown states and absent cycles stay truthful",
          "[linux-services]") {
  Unit unit;
  SECTION("signal is not an exit status") {
    unit.code = CLD_KILLED;
    unit.status = 9;
    unit.reason = "signal";
  }
  SECTION("unknown state") {
    unit.state = "new-systemd-state";
  }
  SECTION("inactive no invocation") {
    unit.state = "inactive";
    unit.before.clear();
    unit.after.clear();
  }
  Fixture fixture;
  fixture.health(unit);
  auto observed = fixture.source->observe(fixture.request());
  REQUIRE(observed);
  REQUIRE(observed->completeness ==
          domain::OpsObservationCompleteness::partial);
  const auto& service =
      std::get<domain::LinuxServiceObservation>(observed->payload);
  REQUIRE_FALSE(service.exit_status);
  REQUIRE_FALSE(service.identity.invocation_id);
}
TEST_CASE("Service source supports concurrent independent connections",
          "[linux-services]") {
  Fixture fixture;
  fixture.health();
  auto first = fixture.request();
  auto second = fixture.request();
  second.request_id = id<domain::OpsRequestId>("second");
  auto a = std::async(std::launch::async,
                      [&] { return fixture.source->observe(first); });
  auto b = std::async(std::launch::async,
                      [&] { return fixture.source->observe(second); });
  REQUIRE(a.get());
  REQUIRE(b.get());
  REQUIRE(fixture.platform->state->opens == 2);
}
TEST_CASE("Service smoke preserves selected identity and known zero values",
          "[linux-services]") {
  Unit unit;
  unit.code = CLD_EXITED;
  Fixture fixture;
  fixture.health(unit);
  auto request = fixture.request();
  std::get<domain::LinuxServiceIdentity>(request.resource).invocation_id =
      id<domain::OpsResourceUid>("01010101010101010101010101010101");
  auto observed = fixture.source->observe(request);
  REQUIRE(observed);
  REQUIRE(observed->completeness ==
          domain::OpsObservationCompleteness::complete);
  REQUIRE(observed->request == request);
  REQUIRE(domain::validate_recorded_ops_observation(*observed));
  const auto& service =
      std::get<domain::LinuxServiceObservation>(observed->payload);
  REQUIRE(service.identity ==
          std::get<domain::LinuxServiceIdentity>(request.resource));
  REQUIRE(service.exit_status == 0);
  REQUIRE(service.restart_count == 0);
}
TEST_CASE(
    "Only implemented health service and exact log operations are advertised",
    "[linux-services]") {
  const std::array expected{
      domain::OpsObservationOperation::linux_health,
      domain::OpsObservationOperation::linux_services,
      domain::OpsObservationOperation::linux_service_health,
      domain::OpsObservationOperation::linux_service_logs};
  REQUIRE(adapters::LinuxOpsObservationSource::supported_operations ==
          expected);
}
TEST_CASE("Empty loaded-service discovery is complete", "[linux-services]") {
  Fixture fixture;
  fixture.discovery({}, 0);
  auto observed = fixture.source->observe(fixture.request(true));
  REQUIRE(observed);
  REQUIRE(observed->completeness ==
          domain::OpsObservationCompleteness::complete);
  REQUIRE(observed->omitted_entries == 0);
  REQUIRE(std::get<domain::LinuxServicesObservation>(observed->payload)
              .services.empty());
}
