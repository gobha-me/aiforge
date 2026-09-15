// Failure matrix first: foreign peer PID/mount/user namespaces, zero peer PID,
// absent platform, expired/cancelled budget, failed connect and handshake,
// malformed replies/signatures/owner/UID/PID, wrong sender/serial, excessive
// unexpected messages, terminal connection failure, fixed operation arguments
// and changed final owner. No host bus/manager calls or service advertisement.
#include "../../src/adapters/linux_systemd_bus.hpp"
#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <functional>
#include <stdexcept>
#include <unistd.h>
#include <vector>

namespace {
using namespace aiforge;
using Error = runtime::OpsObservationSourceError;
using Message = adapters::LinuxSystemdMessage;
using Budget = adapters::LinuxSystemdBudget;
auto budget() -> Budget {
  return {std::chrono::steady_clock::now() + std::chrono::seconds{5}, {}};
}
auto identity() -> domain::LinuxOpsIdentity {
  return {domain::LinuxExecutionScope::unknown,
          "12345678-1234-1234-1234-123456789abc", 10, 11};
}
struct State {
  unsigned opens{}, verifies{}, sends{}, receives{};
  std::vector<std::string> destinations, paths, interfaces, members;
  std::optional<Error> open_error, verify_error, receive_error,
      connection_error;
  std::vector<std::vector<std::string>> arguments;
  int reply_fd{-1};
  bool zero_serial{}, bus_sender{};
  const char* error_name{};
  std::string owner{":1.42"}, final_owner{":1.42"};
  std::uint32_t uid{}, pid{1};
  bool wrong_sender{}, wrong_type{}, wrong_owner_type{}, throw_send{};
  unsigned ignored{}, fail_at{};
  std::stop_source* stop{};
};
class Wire final : public adapters::LinuxSystemdWire {
 public:
  explicit Wire(std::shared_ptr<State> state) : m_state(std::move(state)) {}
  auto send(DBusMessage& message)
      -> std::expected<std::uint32_t, Error> override {
    if (m_state->throw_send) throw std::runtime_error("private diagnostic");
    ++m_state->sends;
    m_state->destinations.emplace_back(dbus_message_get_destination(&message));
    m_state->paths.emplace_back(dbus_message_get_path(&message));
    m_state->interfaces.emplace_back(dbus_message_get_interface(&message));
    m_state->members.emplace_back(dbus_message_get_member(&message));
    REQUIRE_FALSE(dbus_message_get_auto_start(&message));
    REQUIRE_FALSE(dbus_message_get_allow_interactive_authorization(&message));
    std::vector<std::string> arguments;
    DBusMessageIter iterator;
    if (dbus_message_iter_init(&message, &iterator)) {
      do {
        if (dbus_message_iter_get_arg_type(&iterator) == DBUS_TYPE_STRING) {
          const char* argument{};
          dbus_message_iter_get_basic(&iterator, &argument);
          arguments.emplace_back(argument);
        } else {
          REQUIRE(dbus_message_iter_get_arg_type(&iterator) == DBUS_TYPE_ARRAY);
          DBusMessageIter array;
          dbus_message_iter_recurse(&iterator, &array);
          if (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_INVALID) {
            arguments.emplace_back("<empty array>");
          } else {
            REQUIRE(dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING);
            const char* argument{};
            dbus_message_iter_get_basic(&array, &argument);
            arguments.emplace_back(argument);
            REQUIRE_FALSE(dbus_message_iter_next(&array));
          }
        }
      } while (dbus_message_iter_next(&iterator));
    }
    m_state->arguments.push_back(std::move(arguments));
    m_serial = m_state->zero_serial ? 0 : m_state->sends;
    return m_serial;
  }
  auto receive(std::chrono::milliseconds wait)
      -> std::expected<Message, Error> override {
    REQUIRE(wait.count() >= 0);
    REQUIRE(wait <= std::chrono::milliseconds{25});
    ++m_state->receives;
    if (m_state->stop) m_state->stop->request_stop();
    if (m_state->receive_error) return std::unexpected(*m_state->receive_error);
    auto result = Message{dbus_message_new(
        m_state->error_name   ? DBUS_MESSAGE_TYPE_ERROR
        : m_state->wrong_type ? DBUS_MESSAGE_TYPE_METHOD_CALL
                              : DBUS_MESSAGE_TYPE_METHOD_RETURN)};
    REQUIRE(result);
    REQUIRE(dbus_message_set_reply_serial(
        result.get(), m_state->ignored ? m_serial + 1 : m_serial));
    if (m_state->ignored) --m_state->ignored;
    if (m_state->error_name)
      REQUIRE(dbus_message_set_error_name(result.get(), m_state->error_name));
    const char* sender = m_state->bus_sender ? "org.freedesktop.DBus"
                         : m_state->wrong_sender
                             ? ":1.99"
                             : m_state->destinations.back().c_str();
    REQUIRE(dbus_message_set_sender(result.get(), sender));
    const auto& member = m_state->members.back();
    if (member == "GetConnectionUnixUser" ||
        member == "GetConnectionUnixProcessID" ||
        (member == "GetNameOwner" && m_state->wrong_owner_type)) {
      auto value =
          member == "GetConnectionUnixUser" ? m_state->uid : m_state->pid;
      REQUIRE(dbus_message_append_args(result.get(), DBUS_TYPE_UINT32, &value,
                                       DBUS_TYPE_INVALID));
    } else {
      const char* value = member == "Hello"
                              ? ":1.23"
                              : (m_serial > 4 ? m_state->final_owner.c_str()
                                              : m_state->owner.c_str());
      REQUIRE(dbus_message_append_args(result.get(), DBUS_TYPE_STRING, &value,
                                       DBUS_TYPE_INVALID));
    }
    if (m_state->reply_fd >= 0)
      REQUIRE(dbus_message_append_args(result.get(), DBUS_TYPE_UNIX_FD,
                                       &m_state->reply_fd, DBUS_TYPE_INVALID));
    return result;
  }

 private:
  std::shared_ptr<State> m_state;
  std::uint32_t m_serial{};
};
class Platform final : public adapters::LinuxSystemdPlatform {
 public:
  std::shared_ptr<State> state{std::make_shared<State>()};
  auto verify(Budget&) -> std::expected<void, Error> override {
    ++state->verifies;
    if (state->verify_error) return std::unexpected(*state->verify_error);
    return {};
  }
  auto open(Budget&)
      -> std::expected<std::unique_ptr<adapters::LinuxSystemdWire>,
                       Error> override {
    ++state->opens;
    if (state->open_error) return std::unexpected(*state->open_error);
    return std::make_unique<Wire>(state);
  }
  auto verify_connection(adapters::LinuxSystemdWire&, Budget&)
      -> std::expected<void, Error> override {
    if (state->connection_error)
      return std::unexpected(*state->connection_error);
    return {};
  }
};
struct Fixture {
  std::shared_ptr<Platform> platform{std::make_shared<Platform>()};
  std::shared_ptr<adapters::LinuxSystemdBus> bus{
      adapters::LinuxSystemdBusAccess::create(platform).value()};
};
} // namespace

TEST_CASE("Systemd peer namespace proof rejects ambiguity", "[linux-systemd]") {
  adapters::LinuxSystemdPeer peer{42, 101, 10, 11, 12};
  SECTION("missing PID") {
    peer.process_id = 0;
  }
  SECTION("wrong PID namespace") {
    ++peer.pid_namespace;
  }
  SECTION("wrong mount namespace") {
    ++peer.mount_namespace;
  }
  SECTION("wrong user namespace") {
    ++peer.user_namespace;
  }
  auto result = adapters::validate_linux_systemd_peer(identity(), 12, peer);
  REQUIRE_FALSE(result);
  REQUIRE(result.error() == Error::trust_failed);
}
TEST_CASE("Systemd refuses null platform and invalid budgets before connect",
          "[linux-systemd]") {
  REQUIRE_FALSE(adapters::LinuxSystemdBusAccess::create({}));
  Fixture fixture;
  auto value = budget();
  std::stop_source stop;
  SECTION("cancelled") {
    stop.request_stop();
    value.stop = stop.get_token();
  }
  SECTION("expired") {
    value.deadline = std::chrono::steady_clock::time_point::min();
  }
  SECTION("deadline widened") {
    value.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{6};
  }
  SECTION("call budget widened") {
    value.remaining_calls = 1025;
  }
  SECTION("call budget exhausted") {
    value.remaining_calls = 0;
  }
  REQUIRE_FALSE(fixture.bus->open(value));
  REQUIRE(fixture.platform->state->opens == 0);
  REQUIRE(fixture.platform->state->sends == 0);
}
TEST_CASE("Systemd preflight and connect failures issue no bus query",
          "[linux-systemd]") {
  Fixture fixture;
  auto value = budget();
  SECTION("binding changed") {
    fixture.platform->state->verify_error = Error::source_changed;
  }
  SECTION("peer proof denied after open before authentication") {
    fixture.platform->state->connection_error = Error::permission_denied;
  }
  SECTION("permission") {
    fixture.platform->state->open_error = Error::permission_denied;
  }
  REQUIRE_FALSE(fixture.bus->open(value));
  REQUIRE(fixture.platform->state->sends == 0);
  REQUIRE(fixture.platform->state->receives == 0);
}
TEST_CASE("Systemd handshake rejects malformed or foreign evidence",
          "[linux-systemd]") {
  Fixture fixture;
  auto value = budget();
  auto& state = *fixture.platform->state;
  SECTION("nonunique owner") {
    state.owner = "org.example.wrong";
  }
  SECTION("empty owner") {
    state.owner.clear();
  }
  SECTION("oversized owner") {
    state.owner = ":1." + std::string(256, 'a');
  }
  SECTION("zero send serial") {
    state.zero_serial = true;
  }
  SECTION("wrong owner type") {
    state.wrong_owner_type = true;
  }
  SECTION("nonroot manager") {
    state.uid = 1000;
  }
  SECTION("wrong manager PID") {
    state.pid = 42;
  }
  SECTION("wrong sender") {
    state.wrong_sender = true;
  }
  SECTION("wrong message type") {
    state.wrong_type = true;
  }
  SECTION("too many unrelated replies") {
    state.ignored = 20;
  }
  SECTION("receive error") {
    state.receive_error = Error::disconnected;
  }
  SECTION("unexpected exception") {
    state.throw_send = true;
  }
  REQUIRE_FALSE(fixture.bus->open(value));
  REQUIRE(state.sends <= 4);
}
TEST_CASE("Systemd cancellation after send never advances handshake",
          "[linux-systemd]") {
  Fixture fixture;
  auto value = budget();
  std::stop_source stop;
  value.stop = stop.get_token();
  fixture.platform->state->stop = &stop;
  auto result = fixture.bus->open(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error() == Error::cancelled);
  REQUIRE(fixture.platform->state->sends == 1);
}
TEST_CASE("Systemd fixed reads reject invalid resource names before dispatch",
          "[linux-systemd]") {
  Fixture fixture;
  auto value = budget();
  auto connected = fixture.bus->open(value);
  REQUIRE(connected);
  const auto before = fixture.platform->state->sends;
  for (const auto* unit :
       {"", "--help", "*.service", "../a.service", "ssh.socket"}) {
    auto result =
        (*connected)->read(adapters::LinuxSystemdRead::unit_id, unit, value);
    REQUIRE_FALSE(result);
  }
  REQUIRE(fixture.platform->state->sends == before);
}
TEST_CASE("Systemd final owner replacement prevents delivery",
          "[linux-systemd]") {
  Fixture fixture;
  auto value = budget();
  auto connected = fixture.bus->open(value);
  REQUIRE(connected);
  fixture.platform->state->final_owner = ":1.43";
  auto result = (*connected)->verify(value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error() == Error::source_changed);
  const auto sent = fixture.platform->state->sends;
  REQUIRE_FALSE(
      (*connected)
          ->read(adapters::LinuxSystemdRead::find_unit, "ssh.service", value));
  REQUIRE_FALSE((*connected)->verify(value));
  REQUIRE(fixture.platform->state->sends == sent);
}
TEST_CASE("Systemd handshake accepts dedicated bus UID and pins manager reads",
          "[linux-systemd]") {
  REQUIRE(adapters::validate_linux_systemd_peer(identity(), 12,
                                                {42, 101, 10, 11, 12}));
  Fixture fixture;
  REQUIRE(fixture.platform->state->opens == 0);
  auto value = budget();
  auto connected = fixture.bus->open(value);
  REQUIRE(connected);
  auto response =
      (*connected)
          ->read(adapters::LinuxSystemdRead::find_unit, "ssh.service", value);
  REQUIRE(response);
  REQUIRE(fixture.platform->state->destinations.back() == ":1.42");
  REQUIRE(fixture.platform->state->members.back() == "GetUnit");
  REQUIRE(fixture.platform->state->paths.back() == "/org/freedesktop/systemd1");
  REQUIRE((*connected)->verify(value));
}

TEST_CASE("Systemd peer and current binding changes retire the connection",
          "[linux-systemd]") {
  Fixture fixture;
  auto value = budget();
  auto connected = fixture.bus->open(value);
  REQUIRE(connected);
  auto& state = *fixture.platform->state;
  const auto sent = state.sends;
  SECTION("current binding") {
    state.verify_error = Error::source_changed;
  }
  SECTION("connected peer") {
    state.connection_error = Error::trust_failed;
  }
  REQUIRE_FALSE(
      (*connected)
          ->read(adapters::LinuxSystemdRead::find_unit, "ssh.service", value));
  state.verify_error.reset();
  state.connection_error.reset();
  REQUIRE_FALSE(
      (*connected)
          ->read(adapters::LinuxSystemdRead::find_unit, "ssh.service", value));
  REQUIRE(state.sends == sent);
}
TEST_CASE("Systemd rejects FD-bearing evidence and never retries",
          "[linux-systemd]") {
  Fixture fixture;
  auto value = budget();
  auto connected = fixture.bus->open(value);
  REQUIRE(connected);
  int descriptors[2]{};
  REQUIRE(::pipe(descriptors) == 0);
  struct ClosePipe {
    int* descriptors;
    ~ClosePipe() {
      ::close(descriptors[0]);
      ::close(descriptors[1]);
    }
  } close{descriptors};
  fixture.platform->state->reply_fd = descriptors[0];
  auto result =
      (*connected)
          ->read(adapters::LinuxSystemdRead::find_unit, "ssh.service", value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error() == Error::invalid_result);
  const auto sent = fixture.platform->state->sends;
  fixture.platform->state->reply_fd = -1;
  REQUIRE_FALSE((*connected)->verify(value));
  REQUIRE(fixture.platform->state->sends == sent);
}
TEST_CASE("Systemd read request table contains only fixed manager methods",
          "[linux-systemd]") {
  using Read = adapters::LinuxSystemdRead;
  Fixture fixture;
  auto value = budget();
  auto connected = fixture.bus->open(value);
  REQUIRE(connected);
  auto& state = *fixture.platform->state;
  REQUIRE((*connected)->read(Read::list_services, "", value));
  REQUIRE(state.members.back() == "ListUnitsByPatterns");
  REQUIRE(state.paths.back() == "/org/freedesktop/systemd1");
  REQUIRE(state.arguments.back() ==
          std::vector<std::string>{"<empty array>", "*.service"});
  const auto sent = state.sends;
  REQUIRE_FALSE((*connected)->read(Read::list_services, "ssh.service", value));
  REQUIRE_FALSE(
      (*connected)->read(static_cast<Read>(999), "ssh.service", value));
  REQUIRE(state.sends == sent);
  const std::pair<Read, const char*> properties[] = {
      {Read::unit_id, "Id"},
      {Read::active_state, "ActiveState"},
      {Read::invocation_id, "InvocationID"},
      {Read::service_result, "Result"},
      {Read::execution_code, "ExecMainCode"},
      {Read::execution_status, "ExecMainStatus"},
      {Read::restart_count, "NRestarts"}};
  for (const auto& [operation, name] : properties) {
    REQUIRE((*connected)->read(operation, "a-b@c_d.service", value));
    REQUIRE(state.destinations.back() == ":1.42");
    REQUIRE(state.members.back() == "Get");
    REQUIRE(state.interfaces.back() == "org.freedesktop.DBus.Properties");
    REQUIRE(state.paths.back() ==
            "/org/freedesktop/systemd1/unit/a_2db_40c_5fd_2eservice");
    const auto* interface = operation == Read::unit_id ||
                                    operation == Read::active_state ||
                                    operation == Read::invocation_id
                                ? "org.freedesktop.systemd1.Unit"
                                : "org.freedesktop.systemd1.Service";
    REQUIRE(state.arguments.back() ==
            std::vector<std::string>{interface, name});
  }
}

TEST_CASE("Systemd runtime library minimum rejects older loaded versions",
          "[linux-systemd]") {
  REQUIRE_FALSE(adapters::validate_linux_systemd_version(1, 16, 1));
  REQUIRE_FALSE(adapters::validate_linux_systemd_version(1, 14, 20));
  REQUIRE_FALSE(adapters::validate_linux_systemd_version(0, 99, 99));
  REQUIRE_FALSE(adapters::validate_linux_systemd_version(1, 16, -1));
  REQUIRE(adapters::validate_linux_systemd_version(1, 16, 2));
  REQUIRE(adapters::validate_linux_systemd_version(1, 16, 4));
}
TEST_CASE("Systemd accepts only error routing replies from verified bus",
          "[linux-systemd]") {
  Fixture fixture;
  auto value = budget();
  auto connected = fixture.bus->open(value);
  REQUIRE(connected);
  auto& state = *fixture.platform->state;
  state.bus_sender = true;
  Error expected = Error::trust_failed;
  SECTION("owner vanished") {
    state.error_name = DBUS_ERROR_NAME_HAS_NO_OWNER;
    expected = Error::unavailable;
  }
  SECTION("bus policy denied") {
    state.error_name = DBUS_ERROR_ACCESS_DENIED;
    expected = Error::permission_denied;
  }
  SECTION("bus cannot provide manager payload") {
  }
  auto result =
      (*connected)
          ->read(adapters::LinuxSystemdRead::find_unit, "ssh.service", value);
  REQUIRE_FALSE(result);
  REQUIRE(result.error() == expected);
  const auto sent = state.sends;
  REQUIRE_FALSE((*connected)->verify(value));
  REQUIRE(state.sends == sent);
}
