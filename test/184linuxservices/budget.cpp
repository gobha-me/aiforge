// Failure matrix: deadline does not restart in transport construction; capture
// quotas count handshake and unrelated messages, reject oversized/deep data and
// retire a connection before further dispatch. No host bus or provider calls.
#include "../../src/adapters/linux_systemd_bus.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
using namespace aiforge;
using Error = runtime::OpsObservationSourceError;
auto target() -> domain::OpsTargetBinding {
  return {domain::OpsTargetId::from("local").value(),
          domain::OpsConfigurationRevision::from("revision").value(),
          domain::LinuxOpsIdentity{domain::LinuxExecutionScope::unknown,
                                   "12345678-1234-1234-1234-123456789abc", 40,
                                   41}};
}
} // namespace
TEST_CASE("Systemd factory preserves the original stopped or expired budget",
          "[linux-services]") {
  std::stop_source stop;
  adapters::LinuxSystemdBudget budget{std::chrono::steady_clock::now() +
                                          std::chrono::seconds{1},
                                      stop.get_token()};
  auto expected = Error::cancelled;
  SECTION("cancelled before factory pinning") {
    stop.request_stop();
  }
  SECTION("original deadline expired before factory pinning") {
    budget.deadline = std::chrono::steady_clock::time_point::min();
    expected = Error::timed_out;
  }
  auto source = adapters::LinuxSystemdBus::create(target(), budget);
  REQUIRE_FALSE(source);
  REQUIRE(source.error() == expected);
}
namespace {
auto message(std::string_view value) -> adapters::LinuxSystemdMessage {
  adapters::LinuxSystemdMessage result{
      dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN)};
  REQUIRE(result);
  const std::string owned{value};
  const auto* text = owned.c_str();
  REQUIRE(dbus_message_append_args(result.get(), DBUS_TYPE_STRING, &text,
                                   DBUS_TYPE_INVALID));
  return result;
}
auto budget() -> adapters::LinuxSystemdBudget {
  return {std::chrono::steady_clock::now() + std::chrono::seconds{5}, {}};
}
} // namespace
TEST_CASE("Systemd capture budget is cumulative and refuses oversized strings",
          "[linux-services]") {
  auto reply = message(std::string(200, 'x'));
  auto measured = budget();
  const auto initial = measured.remaining_captured_bytes;
  REQUIRE(adapters::charge_linux_systemd_message(*reply, measured));
  const auto used = initial - measured.remaining_captured_bytes;
  auto narrowed = budget();
  narrowed.remaining_captured_bytes = used * 2 - 1;
  REQUIRE(adapters::charge_linux_systemd_message(*reply, narrowed));
  auto exhausted = adapters::charge_linux_systemd_message(*reply, narrowed);
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error() == Error::resource_exhausted);
  auto oversized = message(std::string(65537, 'x'));
  auto maximum = budget();
  auto rejected = adapters::charge_linux_systemd_message(*oversized, maximum);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error() == Error::resource_exhausted);
}
TEST_CASE("Systemd capture bounds nested variants and array visits",
          "[linux-services]") {
  adapters::LinuxSystemdMessage reply{
      dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN)};
  REQUIRE(reply);
  DBusMessageIter outer;
  dbus_message_iter_init_append(reply.get(), &outer);
  SECTION("variant nesting") {
    std::array<DBusMessageIter, 12> levels{};
    levels[0] = outer;
    for (std::size_t index = 0; index < 10; ++index)
      REQUIRE(dbus_message_iter_open_container(
          &levels[index], DBUS_TYPE_VARIANT, index == 9 ? "s" : "v",
          &levels[index + 1]));
    const char* value = "bounded";
    REQUIRE(
        dbus_message_iter_append_basic(&levels[10], DBUS_TYPE_STRING, &value));
    for (std::size_t index = 10; index != 0; --index)
      REQUIRE(dbus_message_iter_close_container(&levels[index - 1],
                                                &levels[index]));
  }
  SECTION("many small fields") {
    DBusMessageIter array;
    REQUIRE(
        dbus_message_iter_open_container(&outer, DBUS_TYPE_ARRAY, "y", &array));
    const unsigned char value{};
    bool appended = true;
    for (unsigned index = 0; index < 8200; ++index)
      appended = appended && dbus_message_iter_append_basic(
                                 &array, DBUS_TYPE_BYTE, &value) != 0;
    REQUIRE(appended);
    REQUIRE(dbus_message_iter_close_container(&outer, &array));
  }
  auto value = budget();
  auto rejected = adapters::charge_linux_systemd_message(*reply, value);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error() == Error::resource_exhausted);
}
