// Failure matrix first: invalid identity/target/request and unsupported reads
// do no collection. Namespace/boot changes before or after collection fail.
// Duplicate/malformed/overflowing memory and uptime/offset fields fail without
// publishing raw input. Missing measurements produce partial unknown evidence;
// nonzero/unproven time offsets never become kernel uptime. Kernel memory is
// never relabeled as cgroup headroom; no service counters/host health invented.
// Byte exhaustion, cancellation, deadline and probe failures remain typed.
// Concurrent reads keep immutable identity and independent budgets. The real
// read-only factory has a bounded smoke, with unavailable platforms explicit.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <future>
#include <stdexcept>

#include "../../src/adapters/linux_ops_observation_source_internal.hpp"

namespace {
using namespace aiforge;
using Error = runtime::OpsObservationSourceError;
using File = adapters::LinuxOpsInputFile;
template <typename T> auto id(std::string value) -> T {
  return T::from(std::move(value)).value();
}
auto identity() -> domain::LinuxOpsIdentity {
  return {domain::LinuxExecutionScope::unknown,
          "12345678-1234-1234-1234-123456789abc", 42, 43};
}
class Probe final : public adapters::LinuxOpsProbe {
 public:
  domain::LinuxOpsIdentity binding{::identity()};
  std::atomic<unsigned> identities{}, reads{};
  std::atomic<unsigned> time_identities{};
  std::optional<adapters::LinuxOpsTimeIdentity> time{
      adapters::LinuxOpsTimeIdentity{50, 50}};
  unsigned change_time_at{};
  bool change_children{};
  unsigned change_at{};
  std::optional<Error> failure;
  bool throwing{}, expire{};
  std::stop_source* cancel{};
  std::array<std::optional<std::string>, 3> inputs{
      "123.75 999.25\n",
      "MemTotal: 1000 kB\nMemAvailable: 400 kB\nMemFree: 20 kB\n",
      "monotonic 0 0\nboottime 0 0\n"};
  auto identity(adapters::LinuxOpsReadBudget& budget)
      -> std::expected<domain::LinuxOpsIdentity, Error> override {
    const auto call = ++identities;
    if (auto used = budget.consume(37); !used)
      return std::unexpected(used.error());
    if (failure) return std::unexpected(*failure);
    auto result = binding;
    if (change_at != 0 && call >= change_at) ++result.mount_namespace;
    return result;
  }
  auto read(File file, adapters::LinuxOpsReadBudget& budget)
      -> std::expected<std::optional<std::string>, Error> override {
    ++reads;
    if (throwing)
      throw std::runtime_error("private raw diagnostic must not escape");
    if (expire) budget.deadline = std::chrono::steady_clock::time_point::min();
    if (cancel) cancel->request_stop();
    const auto& input = inputs.at(static_cast<std::size_t>(file));
    if (auto used = budget.consume(input ? input->size() : 0); !used)
      return std::unexpected(used.error());
    if (failure) return std::unexpected(*failure);
    return input;
  }
  auto time_identity(adapters::LinuxOpsReadBudget& budget)
      -> std::expected<std::optional<adapters::LinuxOpsTimeIdentity>,
                       Error> override {
    if (auto ready = budget.check(); !ready)
      return std::unexpected(ready.error());
    const auto call = ++time_identities;
    auto result = time;
    if (result && change_time_at != 0 && call >= change_time_at) {
      if (change_children)
        ++result->children;
      else
        ++result->current;
    }
    return result;
  }
};
struct Fixture {
  std::shared_ptr<Probe> probe{std::make_shared<Probe>()};
  std::shared_ptr<adapters::LinuxOpsObservationSource> source{
      adapters::LinuxOpsObservationSourceAccess::create(
          id<domain::OpsTargetId>("local"),
          id<domain::OpsConfigurationRevision>("revision"), probe)
          .value()};
  auto request() const -> domain::OpsObservationRequest {
    return {id<domain::OpsOwnerId>("owner"),
            id<domain::SessionId>("session"),
            id<domain::OpsRequestId>("read"),
            source->target_binding(),
            1,
            domain::OpsObservationOperation::linux_health,
            {},
            1,
            {}};
  }
};
auto health(const domain::OpsObservation& value)
    -> const domain::LinuxHealthObservation& {
  return std::get<domain::LinuxHealthObservation>(value.payload);
}
} // namespace

TEST_CASE("Linux health refuses invalid construction", "[linux-ops]") {
  auto probe = std::make_shared<Probe>();
  auto invalid = adapters::LinuxOpsObservationSourceAccess::create(
      id<domain::OpsTargetId>(std::string(1, static_cast<char>(0xff))),
      id<domain::OpsConfigurationRevision>("revision"), probe);
  REQUIRE_FALSE(invalid);
  REQUIRE(invalid.error() == Error::invalid_result);
  REQUIRE(probe->identities == 0);
  probe->binding.boot_id = "not a boot UUID";
  auto bad_binding = adapters::LinuxOpsObservationSourceAccess::create(
      id<domain::OpsTargetId>("local"),
      id<domain::OpsConfigurationRevision>("revision"), probe);
  REQUIRE_FALSE(bad_binding);
  REQUIRE(bad_binding.error() == Error::invalid_result);
  REQUIRE(probe->reads == 0);
}

TEST_CASE("Linux health refuses unsupported or foreign requests before IO",
          "[linux-ops]") {
  Fixture fixture;
  auto value = fixture.request();
  SECTION("unsupported services") {
    value.operation = domain::OpsObservationOperation::linux_services;
    auto result = fixture.source->observe(value);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == Error::unsupported);
  }
  SECTION("foreign target") {
    value.target.configuration_revision =
        id<domain::OpsConfigurationRevision>("foreign");
    auto result = fixture.source->observe(value);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == Error::source_changed);
  }
  SECTION("invalid limits") {
    value.limits.timeout = std::chrono::milliseconds{0};
    auto result = fixture.source->observe(value);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == Error::invalid_result);
  }
  REQUIRE(fixture.probe->identities == 1);
  REQUIRE(fixture.probe->reads == 0);
}

TEST_CASE("Linux health never publishes across namespace replacement",
          "[linux-ops]") {
  Fixture fixture;
  SECTION("before collection") {
    fixture.probe->change_at = 2;
  }
  SECTION("after collection") {
    fixture.probe->change_at = 3;
  }
  auto result = fixture.source->observe(fixture.request());
  REQUIRE_FALSE(result);
  REQUIRE(result.error() == Error::source_changed);
  REQUIRE(fixture.probe->reads == (fixture.probe->change_at == 2 ? 0 : 3));
}

TEST_CASE("Linux health rejects malformed supplied metrics", "[linux-ops]") {
  Fixture fixture;
  SECTION("duplicate total") {
    fixture.probe->inputs[1] =
        "MemTotal: 1 kB\nMemTotal: 2 kB\nMemAvailable: 1 kB\n";
  }
  SECTION("duplicate available") {
    fixture.probe->inputs[1] =
        "MemTotal: 2 kB\nMemAvailable: 1 kB\nMemAvailable: 1 kB\n";
  }
  SECTION("available exceeds total") {
    fixture.probe->inputs[1] = "MemTotal: 1 kB\nMemAvailable: 2 kB\n";
  }
  SECTION("wrong units") {
    fixture.probe->inputs[1] = "MemTotal: 1 MB\nMemAvailable: 1 kB\n";
  }
  SECTION("multiplication overflow") {
    fixture.probe->inputs[1] =
        "MemTotal: 18446744073709551615 kB\nMemAvailable: 1 kB\n";
  }
  SECTION("malformed row") {
    fixture.probe->inputs[1] = "MemTotal bad\n";
  }
  SECTION("oversized memory") {
    fixture.probe->inputs[1] = std::string(16384, 'x');
  }
  SECTION("negative uptime") {
    fixture.probe->inputs[0] = "-1.25 9.50\n";
  }
  SECTION("uptime overflow") {
    fixture.probe->inputs[0] = "18446744073709551616.0 0.0\n";
  }
  SECTION("uptime trailing data") {
    fixture.probe->inputs[0] = "1.0 1.0 garbage\n";
  }
  SECTION("duplicate offset") {
    fixture.probe->inputs[2] = "monotonic 0 0\nboottime 0 0\nboottime 0 0\n";
  }
  SECTION("offset nanoseconds overflow") {
    fixture.probe->inputs[2] = "monotonic 0 0\nboottime 0 1000000000\n";
  }
  SECTION("incomplete provided offsets") {
    fixture.probe->inputs[2] = "monotonic 0 0\n";
  }
  auto result = fixture.source->observe(fixture.request());
  REQUIRE_FALSE(result);
  REQUIRE(result.error() == Error::invalid_result);
}

TEST_CASE("Linux uptime requires zero offset proof", "[linux-ops]") {
  Fixture fixture;
  SECTION("missing proof") {
    fixture.probe->inputs[2].reset();
  }
  SECTION("positive offset") {
    fixture.probe->inputs[2] = "monotonic 0 0\nboottime 123 0\n";
  }
  SECTION("negative offset") {
    fixture.probe->inputs[2] = "monotonic 0 0\nboottime -123 0\n";
  }
  SECTION("nanosecond offset") {
    fixture.probe->inputs[2] = "monotonic 0 0\nboottime 0 1\n";
  }
  SECTION("current and child namespace differ") {
    fixture.probe->time->children = 51;
  }
  SECTION("time namespace proof unavailable") {
    fixture.probe->time.reset();
  }
  SECTION("current namespace changes during capture") {
    fixture.probe->change_time_at = 2;
  }
  SECTION("child namespace changes during capture") {
    fixture.probe->change_time_at = 2;
    fixture.probe->change_children = true;
  }
  auto result = fixture.source->observe(fixture.request());
  REQUIRE(result);
  REQUIRE_FALSE(health(*result).kernel_uptime_seconds);
  REQUIRE(health(*result).memory);
  REQUIRE(result->completeness == domain::OpsObservationCompleteness::partial);
  REQUIRE_FALSE(result->omitted_entries);
  REQUIRE(result->unsupported_entries == 0);
}

TEST_CASE("Linux missing measurements remain partial without invented health",
          "[linux-ops]") {
  Fixture fixture;
  SECTION("missing available does not use free") {
    fixture.probe->inputs[1] = "MemTotal: 1000 kB\nMemFree: 50 kB\n";
  }
  SECTION("missing memory") {
    fixture.probe->inputs[1].reset();
  }
  SECTION("all missing") {
    fixture.probe->inputs[0].reset();
    fixture.probe->inputs[1].reset();
    fixture.probe->inputs[2].reset();
  }
  auto result = fixture.source->observe(fixture.request());
  REQUIRE(result);
  REQUIRE_FALSE(health(*result).memory);
  REQUIRE(health(*result).health == domain::OpsHealthState::unknown);
  REQUIRE_FALSE(health(*result).active_services);
  REQUIRE_FALSE(health(*result).failed_services);
  REQUIRE(result->completeness == domain::OpsObservationCompleteness::partial);
  REQUIRE_FALSE(result->omitted_entries);
}

TEST_CASE("Linux collection honors cancellation time and byte bounds",
          "[linux-ops]") {
  Fixture fixture;
  auto value = fixture.request();
  std::stop_source stop;
  Error expected = Error::cancelled;
  SECTION("cancel before read") {
    stop.request_stop();
  }
  SECTION("cancel during read") {
    fixture.probe->cancel = &stop;
  }
  SECTION("deadline") {
    fixture.probe->expire = true;
    expected = Error::timed_out;
  }
  SECTION("private bytes") {
    value.limits.maximum_bytes = 1;
    expected = Error::resource_exhausted;
  }
  SECTION("exception") {
    fixture.probe->throwing = true;
    expected = Error::internal_failure;
  }
  SECTION("source unavailable") {
    fixture.probe->failure = Error::unavailable;
    expected = Error::unavailable;
  }
  auto result = fixture.source->observe(value, stop.get_token());
  REQUIRE_FALSE(result);
  REQUIRE(result.error() == expected);
}

TEST_CASE("Linux health supports concurrent immutable captures",
          "[linux-ops]") {
  Fixture fixture;
  const auto binding = fixture.source->target_binding();
  auto first = std::async(std::launch::async, [&] {
    return fixture.source->observe(fixture.request());
  });
  auto second = std::async(std::launch::async, [&] {
    return fixture.source->observe(fixture.request());
  });
  REQUIRE(first.get());
  REQUIRE(second.get());
  REQUIRE(fixture.source->target_binding() == binding);
  REQUIRE(fixture.probe->reads == 6);
}

TEST_CASE("Linux real factory reads only scoped kernel health", "[linux-ops]") {
  auto source = adapters::LinuxOpsObservationSource::create(
      id<domain::OpsTargetId>("real-local"),
      id<domain::OpsConfigurationRevision>("test-revision"));
#if defined(__linux__)
  REQUIRE(source);
  auto request = Fixture{}.request();
  request.target = (*source)->target_binding();
  auto result = (*source)->observe(request);
  REQUIRE(result);
  const auto& local =
      std::get<domain::LinuxOpsIdentity>(request.target.identity);
  REQUIRE(local.scope != domain::LinuxExecutionScope::host);
  REQUIRE(local.pid_namespace != 0);
  REQUIRE(local.mount_namespace != 0);
  REQUIRE(health(*result).health == domain::OpsHealthState::unknown);
  if (health(*result).memory)
    REQUIRE(health(*result).memory->scope == domain::OpsMemoryScope::kernel);
#else
  REQUIRE_FALSE(source);
  REQUIRE(source.error() == Error::unsupported);
#endif
}

TEST_CASE(
    "Linux health returns timestamped kernel facts without service claims",
    "[linux-ops]") {
  Fixture fixture;
  auto result = fixture.source->observe(fixture.request());
  REQUIRE(result);
  REQUIRE(result->request == fixture.request());
  REQUIRE(result->completed_at >= result->started_at);
  REQUIRE(result->completeness == domain::OpsObservationCompleteness::complete);
  REQUIRE(health(*result).kernel_uptime_seconds == 123);
  REQUIRE(health(*result).memory->scope == domain::OpsMemoryScope::kernel);
  REQUIRE(health(*result).memory->total_bytes == 1024000);
  REQUIRE(health(*result).memory->available_bytes == 409600);
  REQUIRE(health(*result).health == domain::OpsHealthState::unknown);
  REQUIRE_FALSE(health(*result).active_services);
  REQUIRE_FALSE(health(*result).failed_services);
  REQUIRE(fixture.source->supported_operations.size() == 1);
}
