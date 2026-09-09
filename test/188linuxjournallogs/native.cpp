#include "fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cerrno>
#include <systemd/sd-journal.h>

namespace {
struct NativeState {
  unsigned calls{}, closes{};
  int flags{};
  std::size_t threshold{};
  std::uint64_t lower{};
  std::vector<std::string> matches;
  std::optional<unsigned> fail_at;
  int failure{-EACCES};
  journal_fixture::Entry entry;
  unsigned rows{};
  std::size_t field{};
  auto step() -> int {
    ++calls;
    return fail_at == calls ? failure : 0;
  }
};
NativeState native;
} // namespace
// Linker wrapping confines these tests to this process-owned protocol fixture.
// There is no forwarding symbol or host journal access in these wrappers.
extern "C" {
int __wrap_sd_journal_open(sd_journal** journal, int flags) {
  native.flags = flags;
  if (const auto result = native.step(); result < 0) return result;
  *journal = reinterpret_cast<sd_journal*>(&native);
  return 0;
}
void __wrap_sd_journal_close(sd_journal*) {
  ++native.closes;
}
int __wrap_sd_journal_set_data_threshold(sd_journal*, std::size_t threshold) {
  native.threshold = threshold;
  return native.step();
}
int __wrap_sd_journal_add_match(sd_journal*, const void* data,
                                std::size_t size) {
  native.matches.emplace_back(static_cast<const char*>(data), size);
  return native.step();
}
int __wrap_sd_journal_seek_realtime_usec(sd_journal*, std::uint64_t value) {
  native.lower = value;
  return native.step();
}
int __wrap_sd_journal_next(sd_journal*) {
  if (const auto result = native.step(); result < 0) return result;
  return native.rows++ == 0 ? 1 : 0;
}
int __wrap_sd_journal_get_realtime_usec(sd_journal*, std::uint64_t* value) {
  if (const auto result = native.step(); result < 0) return result;
  *value = native.entry.timestamp;
  return 0;
}
void __wrap_sd_journal_restart_data(sd_journal*) {
  native.field = 0;
}
int __wrap_sd_journal_enumerate_data(sd_journal*, const void** data,
                                     std::size_t* size) {
  if (const auto result = native.step(); result < 0) return result;
  if (native.field == native.entry.fields.size()) return 0;
  const auto& value = native.entry.fields[native.field++];
  *data = value.data();
  *size = value.size();
  return 1;
}
}

TEST_CASE("Native journal transport uses only fixed local system selection",
          "[ops][journal][native]") {
  native = {};
  journal_fixture::Fixture f;
  auto factory = aiforge::adapters::make_linux_journal_factory();
  REQUIRE(factory);
  CHECK(native.calls == 0);
  auto result = aiforge::adapters::observe_linux_service_logs(
      f.request, *f.service.bus, **factory, f.budget, f.clock.value, f.clock);
  REQUIRE(result);
  CHECK(native.flags == (SD_JOURNAL_LOCAL_ONLY | SD_JOURNAL_SYSTEM));
  CHECK(native.threshold == aiforge::adapters::linux_journal_field_threshold);
  CHECK(native.lower == 405000000);
  CHECK(native.matches ==
        std::vector<std::string>{
            "_BOOT_ID=12345678123412341234123456789abc",
            "_SYSTEMD_UNIT=ssh.service",
            "_SYSTEMD_INVOCATION_ID=01010101010101010101010101010101"});
  CHECK(native.closes == 1);
}

TEST_CASE("Native journal failed calls return fixed failures and close their "
          "owner handle",
          "[ops][journal][native]") {
  native = {};
  native.fail_at = GENERATE(1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U);
  native.failure = GENERATE(-EACCES, -ENOMEM, -EBADMSG);
  journal_fixture::Fixture f;
  auto factory = aiforge::adapters::make_linux_journal_factory();
  REQUIRE(factory);
  auto result = aiforge::adapters::observe_linux_service_logs(
      f.request, *f.service.bus, **factory, f.budget, f.clock.value, f.clock);
  REQUIRE_FALSE(result);
  using Error = aiforge::runtime::OpsObservationSourceError;
  CHECK(result.error() == (native.failure == -EACCES ? Error::unavailable
                           : native.failure == -ENOMEM
                               ? Error::resource_exhausted
                               : Error::invalid_result));
  CHECK(native.closes == (*native.fail_at == 1 ? 0 : 1));
}

TEST_CASE("Native journal stop is checked before any library open",
          "[ops][journal][native]") {
  native = {};
  journal_fixture::Fixture f;
  auto factory = aiforge::adapters::make_linux_journal_factory();
  REQUIRE(factory);
  f.factory.state->stop.request_stop();
  auto result = aiforge::adapters::observe_linux_service_logs(
      f.request, *f.service.bus, **factory, f.budget, f.clock.value, f.clock);
  REQUIRE_FALSE(result);
  CHECK(native.calls == 0);
  CHECK(native.closes == 0);
}
