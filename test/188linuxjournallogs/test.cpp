#include "fixture.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

using namespace journal_fixture;

TEST_CASE(
    "Journal admission and initial service proof precede all journal reads",
    "[ops][journal]") {
  Fixture f;
  const auto mode = GENERATE(0, 1, 2, 3, 4, 5, 6);
  if (mode == 0) f.request.limits.maximum_log_age = std::chrono::seconds{601};
  if (mode == 1) f.request.limits.maximum_log_lines = 201;
  if (mode == 2) f.request.limits.maximum_log_bytes = 32769;
  if (mode == 3)
    std::get<domain::LinuxServiceIdentity>(f.request.resource)
        .invocation_id.reset();
  if (mode == 4) f.factory.state->stop.request_stop();
  if (mode == 5) f.service.platform->state->early_failure = Error::unavailable;
  if (mode == 6)
    f.service.platform->state->steps[6].reply =
        service_fixture::invocation(std::vector<unsigned char>(16, 2));
  CHECK_FALSE(f.run());
  CHECK(f.factory.state->opens == 0);
  CHECK(f.factory.state->calls == 0);
  if (mode < 5) CHECK(f.service.platform->state->opens == 0);
}

TEST_CASE(
    "Journal rejects malformed returned fields and exact identity mismatch",
    "[ops][journal]") {
  Fixture f;
  auto& fields = f.factory.state->entries[0].fields;
  const auto mode = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  if (mode == 0) fields.pop_back();
  if (mode == 1) fields.push_back(fields.front());
  if (mode == 2) fields.push_back("MESSAGE=duplicate");
  if (mode == 3) fields[0] = "_BOOT_ID=foreign";
  if (mode == 4) fields[1] = "_SYSTEMD_UNIT=other.service";
  if (mode == 5) fields[2] = "INVOCATION_ID=01010101010101010101010101010101";
  if (mode == 6) fields[3] = std::string{"MESSAGE=bad\0binary", 18};
  if (mode == 7) fields[3] = "MESSAGE=\x1b[31mred";
  if (mode == 8) fields[3] = "MESSAGE=\xff";
  if (mode == 9) fields.push_back("bad_field=ignored");
  if (mode == 10) fields[3] = "MESSAGE=carriage\rreturn";
  CHECK_FALSE(f.run());
  CHECK(f.factory.state->closes == 1);
}

TEST_CASE(
    "Journal excludes recognizable credential text without a secret store",
    "[ops][journal]") {
  Fixture f;
  const auto text = GENERATE(
      "Authorization: Bearer fixture-token", "Bearer fake",
      "Basic ZmFrZTpmYWtl", "password = fake", "API_KEY: fake",
      "{\"token\":\"fake\"}", "-----BEGIN PRIVATE KEY-----",
      "-----BEGIN RSA PRIVATE KEY-----", "-----BEGIN OPENSSH PRIVATE KEY-----");
  f.factory.state->entries[0].fields[3] = std::string{"MESSAGE="} + text;
  const auto result = f.run();
  REQUIRE_FALSE(result);
  CHECK(result.error() == Error::invalid_result);
}

TEST_CASE(
    "Journal empty and permission-limited visibility never proves completeness",
    "[ops][journal]") {
  Fixture f;
  f.factory.state->entries.clear();
  auto result = f.run();
  REQUIRE_FALSE(result);
  CHECK(result.error() == Error::unavailable);
  CHECK(f.factory.state->closes == 1);
}

TEST_CASE("Journal field threshold and field-count ceilings reject before "
          "retained evidence",
          "[ops][journal]") {
  Fixture f;
  const auto mode = GENERATE(0, 1, 2, 3);
  auto& fields = f.factory.state->entries[0].fields;
  if (mode == 0) fields[3] = "MESSAGE=" + std::string(32769, 'x');
  if (mode == 1) fields.push_back("IGNORED=" + std::string(32769, 'x'));
  if (mode == 2) fields.push_back("IGNORED=" + std::string(40000, 'x'));
  if (mode == 3)
    for (unsigned n = 0; n < 125; ++n)
      fields.push_back("IGNORED=x");
  auto result = f.run();
  REQUIRE_FALSE(result);
  CHECK(result.error() == Error::resource_exhausted);
}

TEST_CASE("Journal source errors cancellation and expiry discard the complete "
          "attempt",
          "[ops][journal]") {
  const auto stage = GENERATE(1U, 2U, 3U, 4U, 5U, 8U, 9U);
  const auto mode = GENERATE(0, 1, 2);
  Fixture f;
  if (mode == 0) f.factory.state->fail_at = stage;
  if (mode == 1) f.factory.state->expire_at = stage;
  if (mode == 2) f.factory.state->stop_at = stage;
  const auto result = f.run();
  REQUIRE_FALSE(result);
  CHECK(result.error() == (mode == 0   ? Error::invalid_result
                           : mode == 1 ? Error::timed_out
                                       : Error::cancelled));
  CHECK(f.factory.state->closes == (stage == 1 ? 0 : 1));
}

TEST_CASE("Journal final unit invocation and manager proof cannot publish an "
          "earlier prefix",
          "[ops][journal]") {
  Fixture f;
  const auto step = GENERATE(7U, 8U, 9U, 10U);
  auto& value = f.service.platform->state->steps[step];
  if (step == 7)
    value.reply = service_fixture::text("/foreign", DBUS_TYPE_OBJECT_PATH);
  if (step == 8) value.reply = service_fixture::property("other.service");
  if (step == 9)
    value.reply =
        service_fixture::invocation(std::vector<unsigned char>(16, 2));
  if (step == 10) value.reply = service_fixture::text(":1.43");
  CHECK_FALSE(f.run());
  CHECK(f.factory.state->closes == 1);
}

TEST_CASE("Journal capture and final-call reserves are cumulative",
          "[ops][journal]") {
  SECTION("proof reserve prevents opening") {
    Fixture f;
    f.budget.remaining_calls = 12;
    auto result = f.run();
    REQUIRE_FALSE(result);
    CHECK(result.error() == Error::resource_exhausted);
    CHECK(f.factory.state->opens == 0);
  }
  SECTION("captured budget includes handshake and fields") {
    Fixture measured;
    const auto before = measured.budget.remaining_captured_bytes;
    REQUIRE(measured.run());
    const auto consumed = before - measured.budget.remaining_captured_bytes;
    Fixture exact;
    exact.budget.remaining_captured_bytes = consumed;
    REQUIRE(exact.run());
    CHECK(exact.budget.remaining_captured_bytes == 0);
    Fixture short_budget;
    short_budget.budget.remaining_captured_bytes = consumed - 1;
    CHECK_FALSE(short_budget.run());
  }
  SECTION("an already expired original deadline cannot be refreshed") {
    Fixture f;
    f.budget.deadline = std::chrono::steady_clock::time_point::min();
    CHECK_FALSE(f.run());
    CHECK(f.service.platform->state->opens == 0);
  }
}

TEST_CASE("Journal actual timestamps reject skew and final window crossing",
          "[ops][journal]") {
  Fixture f;
  const auto mode = GENERATE(0, 1, 2, 3);
  if (mode == 0) f.factory.state->entries[0].timestamp += 1000;
  if (mode == 1) f.factory.state->entries[0].timestamp -= 601000000;
  if (mode == 2)
    f.factory.state->entries[0].timestamp =
        std::numeric_limits<std::uint64_t>::max();
  if (mode == 3) {
    f.clock.jump_at = 3;
    f.clock.jump = std::chrono::milliseconds{601000};
  }
  CHECK_FALSE(f.run());
}

TEST_CASE(
    "Journal validates lookahead and returns only a bounded window prefix",
    "[ops][journal]") {
  SECTION("bounded prefix has unknown omissions") {
    Fixture f;
    f.request.limits.maximum_log_lines = 2;
    f.factory.state->entries.assign(3, Entry{});
    f.factory.state->entries[0].fields[3] = "MESSAGE=first";
    f.factory.state->entries[1].fields[3] = "MESSAGE=second";
    f.factory.state->entries[2].fields[3] = "MESSAGE=third";
    auto result = f.run();
    REQUIRE(result);
    const auto& lines =
        std::get<domain::OpsLogObservation>(result->payload).lines;
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text == "first");
    CHECK(lines[1].text == "second");
    CHECK(result->completeness ==
          domain::OpsObservationCompleteness::truncated);
    CHECK_FALSE(result->omitted_entries);
  }
  SECTION("malformed lookahead refuses retained prefix") {
    Fixture f;
    f.request.limits.maximum_log_lines = 1;
    f.factory.state->entries.assign(2, Entry{});
    f.factory.state->entries[1].fields[3] = "MESSAGE=Bearer fake";
    CHECK_FALSE(f.run());
  }
  SECTION("exact byte bound and one fewer") {
    Fixture f;
    f.factory.state->entries[0].fields[3] =
        "MESSAGE=" + std::string(32768, 'x');
    REQUIRE(f.run());
    Fixture smaller;
    smaller.request.limits.maximum_log_bytes = 32767;
    smaller.factory.state->entries[0].fields[3] =
        "MESSAGE=" + std::string(32768, 'x');
    CHECK_FALSE(smaller.run());
  }
}

TEST_CASE(
    "Journal smoke preserves empty multiline and nonmonotonic source order",
    "[ops][journal]") {
  Fixture f;
  f.factory.state->entries.assign(3, Entry{});
  f.factory.state->entries[0].fields[3] = "MESSAGE=one\n\ntwo\n";
  f.factory.state->entries[1].fields[3] = "MESSAGE=";
  f.factory.state->entries[2].fields[3] = "MESSAGE=last";
  f.factory.state->entries[2].timestamp -= 1000;
  auto result = f.run();
  REQUIRE(result);
  CHECK(result->completeness == domain::OpsObservationCompleteness::partial);
  CHECK_FALSE(result->omitted_entries);
  const auto& lines =
      std::get<domain::OpsLogObservation>(result->payload).lines;
  REQUIRE(lines.size() == 6);
  CHECK(lines[0].text == "one");
  CHECK(lines[1].text.empty());
  CHECK(lines[2].text == "two");
  CHECK(lines[3].text.empty());
  CHECK(lines[4].text.empty());
  CHECK(lines[5].text == "last");
  CHECK(lines[5].timestamp < lines[4].timestamp);
  REQUIRE(f.factory.state->selection);
  CHECK(f.factory.state->selection->lower_realtime_usec == 405000000);
  CHECK(f.factory.state->closes == 1);
}

TEST_CASE(
    "Journal source preserves immutable target admission and health separation",
    "[ops][journal]") {
  Fixture f;
  auto factory = std::make_shared<Factory>();
  auto source = adapters::LinuxOpsObservationSourceAccess::create(
      service_fixture::id<domain::OpsTargetId>("local"),
      service_fixture::id<domain::OpsConfigurationRevision>("revision"),
      std::make_shared<service_fixture::Probe>(), f.service.bus, factory);
  REQUIRE(source);
  SECTION("foreign target has no journal or bus IO") {
    auto request = f.request;
    request.target.target_id =
        service_fixture::id<domain::OpsTargetId>("other");
    CHECK_FALSE((*source)->observe(request));
    CHECK(factory->state->opens == 0);
    CHECK(f.service.platform->state->opens == 0);
  }
  SECTION("health remains independent of journal and bus availability") {
    auto request = f.request;
    request.operation = domain::OpsObservationOperation::linux_health;
    request.resource = std::monostate{};
    const auto result = (*source)->observe(request);
    REQUIRE(result);
    CHECK(factory->state->opens == 0);
    CHECK(f.service.platform->state->opens == 0);
  }
}

TEST_CASE("Journal exact line ceiling and narrowed entry ceiling retain "
          "truthful bounds",
          "[ops][journal]") {
  SECTION("exact 200 lines remain partial without invented omissions") {
    Fixture f;
    f.factory.state->entries.assign(200, Entry{});
    auto result = f.run();
    REQUIRE(result);
    CHECK(std::get<domain::OpsLogObservation>(result->payload).lines.size() ==
          200);
    CHECK(result->completeness == domain::OpsObservationCompleteness::partial);
    CHECK_FALSE(result->omitted_entries);
  }
  SECTION("entry ceiling narrows multiline output") {
    Fixture f;
    f.request.limits.maximum_entries = 1;
    f.factory.state->entries[0].fields[3] = "MESSAGE=first\nsecond";
    auto result = f.run();
    REQUIRE(result);
    CHECK(std::get<domain::OpsLogObservation>(result->payload).lines.size() ==
          1);
    CHECK(result->completeness ==
          domain::OpsObservationCompleteness::truncated);
  }
  SECTION("open failure is unavailable with no cursor") {
    Fixture f;
    f.factory.state->fail_at = 1;
    f.factory.state->failure = Error::unavailable;
    const auto result = f.run();
    REQUIRE_FALSE(result);
    CHECK(result.error() == Error::unavailable);
    CHECK(f.factory.state->closes == 0);
  }
}
