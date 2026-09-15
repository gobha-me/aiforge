#pragma once

#include "../../src/adapters/linux_ops_observation_source_internal.hpp"
#include "../../src/adapters/linux_systemd_services.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace service_fixture {
using namespace aiforge;
using Error = runtime::OpsObservationSourceError;
using Message = adapters::LinuxSystemdMessage;
inline void require(bool value) {
  if (!value) throw std::runtime_error("fixture construction failed");
}
template <class Id> auto id(std::string value) -> Id {
  return Id::from(std::move(value)).value();
}
inline auto identity() -> domain::LinuxOpsIdentity {
  return {domain::LinuxExecutionScope::unknown,
          "12345678-1234-1234-1234-123456789abc", 40, 41};
}
inline auto message() -> Message {
  Message result{dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN)};
  require(result != nullptr);
  return result;
}
class Container {
 public:
  Container(DBusMessageIter& parent, int type, const char* signature)
      : m_parent(parent) {
    require(dbus_message_iter_open_container(&parent, type, signature,
                                             &value) != 0);
  }
  ~Container() {
    dbus_message_iter_abandon_container_if_open(&m_parent, &value);
  }
  void close() {
    require(dbus_message_iter_close_container(&m_parent, &value) != 0);
  }
  DBusMessageIter value = DBUS_MESSAGE_ITER_INIT_CLOSED;

 private:
  DBusMessageIter& m_parent;
};
inline void append_text(DBusMessageIter& iterator, const std::string& text,
                        int type = DBUS_TYPE_STRING) {
  const auto* value = text.c_str();
  require(dbus_message_iter_append_basic(&iterator, type, &value) != 0);
}
template <class Number>
void append_number(DBusMessageIter& iterator, Number value, int type) {
  require(dbus_message_iter_append_basic(&iterator, type, &value) != 0);
}
inline auto text(const std::string& text, int type = DBUS_TYPE_STRING)
    -> Message {
  auto result = message();
  DBusMessageIter iterator;
  dbus_message_iter_init_append(result.get(), &iterator);
  append_text(iterator, text, type);
  return result;
}
inline auto number(std::uint32_t value) -> Message {
  auto result = message();
  DBusMessageIter iterator;
  dbus_message_iter_init_append(result.get(), &iterator);
  append_number(iterator, value, DBUS_TYPE_UINT32);
  return result;
}
inline auto property(const std::string& value) -> Message {
  auto result = message();
  DBusMessageIter outer;
  dbus_message_iter_init_append(result.get(), &outer);
  Container inner{outer, DBUS_TYPE_VARIANT, "s"};
  append_text(inner.value, value);
  inner.close();
  return result;
}
template <class Number>
auto property_number(Number value, int type, const char* signature) -> Message {
  auto result = message();
  DBusMessageIter outer;
  dbus_message_iter_init_append(result.get(), &outer);
  Container inner{outer, DBUS_TYPE_VARIANT, signature};
  append_number(inner.value, value, type);
  inner.close();
  return result;
}
inline auto invocation(const std::vector<unsigned char>& value) -> Message {
  auto result = message();
  DBusMessageIter outer;
  dbus_message_iter_init_append(result.get(), &outer);
  Container inner{outer, DBUS_TYPE_VARIANT, "ay"};
  Container array{inner.value, DBUS_TYPE_ARRAY, "y"};
  for (const auto byte : value)
    append_number(array.value, byte, DBUS_TYPE_BYTE);
  array.close();
  inner.close();
  return result;
}
struct Unit {
  std::string name{"ssh.service"};
  std::string state{"active"};
  std::string reason{"success"};
  std::vector<unsigned char> before = std::vector<unsigned char>(16, 1);
  std::vector<unsigned char> after = std::vector<unsigned char>(16, 1);
  std::int32_t code{}, status{};
  std::uint32_t restarts{};
  std::string description{"private description must not be captured"};
  std::string path_override{};
};
inline auto list(const std::vector<Unit>& units) -> Message {
  auto result = message();
  DBusMessageIter outer;
  dbus_message_iter_init_append(result.get(), &outer);
  Container rows{outer, DBUS_TYPE_ARRAY, "(ssssssouso)"};
  for (const auto& unit : units) {
    Container row{rows.value, DBUS_TYPE_STRUCT, nullptr};
    for (const auto& value : std::vector<std::string>{
             unit.name, unit.description, "loaded", unit.state, "running", ""})
      append_text(row.value, value);
    auto path = unit.path_override.empty()
                    ? adapters::linux_systemd_unit_path(unit.name).value()
                    : unit.path_override;
    append_text(row.value, path, DBUS_TYPE_OBJECT_PATH);
    append_number(row.value, std::uint32_t{}, DBUS_TYPE_UINT32);
    append_text(row.value, "");
    append_text(row.value, "/", DBUS_TYPE_OBJECT_PATH);
    row.close();
  }
  rows.close();
  return result;
}
struct Step {
  std::string member, property;
  Message reply;
};
struct Call {
  std::string member, property;
};
struct State {
  std::vector<Step> steps;
  std::mutex mutex;
  std::vector<Call> calls;
  std::atomic<unsigned> opens{}, sends{}, receives{}, verifies{};
  std::optional<Error> early_failure{};
  bool fail_final{};
  std::atomic<bool> final{};
  unsigned unrelated{};
  std::string unrelated_text = std::string(400, 'x');
  std::stop_source* stop{};
  adapters::LinuxSystemdBudget* expire_budget{};
  std::size_t expire_on_receive{};
};
class Wire final : public adapters::LinuxSystemdWire {
 public:
  explicit Wire(std::shared_ptr<State> state)
      : m_state(std::move(state)), m_unrelated(m_state->unrelated) {}
  auto send(DBusMessage& request)
      -> std::expected<std::uint32_t, Error> override {
    ++m_state->sends;
    const std::string member{dbus_message_get_member(&request)};
    std::string property;
    if (member == "Get") {
      DBusMessageIter iterator;
      dbus_message_iter_init(&request, &iterator);
      dbus_message_iter_next(&iterator);
      const char* value{};
      dbus_message_iter_get_basic(&iterator, &value);
      property = value;
    }
    {
      std::lock_guard lock{m_state->mutex};
      m_state->calls.push_back({member, property});
    }
    if (m_index >= m_state->steps.size())
      return std::unexpected(Error::invalid_result);
    const auto& step = m_state->steps[m_index];
    if (step.member != member || step.property != property)
      return std::unexpected(Error::invalid_result);
    if (member == "GetNameOwner" && m_index > 4) m_state->final = true;
    return static_cast<std::uint32_t>(m_index + 1);
  }
  auto receive(std::chrono::milliseconds)
      -> std::expected<Message, Error> override {
    ++m_state->receives;
    if (m_state->stop) m_state->stop->request_stop();
    if (m_state->expire_budget && m_index == m_state->expire_on_receive)
      m_state->expire_budget->deadline =
          std::chrono::steady_clock::time_point::min();
    if (m_index >= m_state->steps.size())
      return std::unexpected(Error::invalid_result);
    const auto& step = m_state->steps[m_index];
    Message result;
    const bool unrelated = m_unrelated != 0;
    if (unrelated) {
      --m_unrelated;
      result = text(m_state->unrelated_text);
    } else
      result.reset(dbus_message_copy(step.reply.get()));
    require(result != nullptr);
    require(dbus_message_set_reply_serial(
                result.get(), static_cast<std::uint32_t>(
                                  unrelated ? 9999 : m_index + 1)) != 0);
    const auto* sender = step.member == "Hello" ||
                                 step.member.starts_with("GetConnection") ||
                                 step.member == "GetNameOwner"
                             ? "org.freedesktop.DBus"
                             : ":1.42";
    require(dbus_message_set_sender(result.get(), sender) != 0);
    if (!unrelated) ++m_index;
    return result;
  }

 private:
  std::shared_ptr<State> m_state;
  std::size_t m_index{};
  unsigned m_unrelated{};
};
class Platform final : public adapters::LinuxSystemdPlatform {
 public:
  std::shared_ptr<State> state = std::make_shared<State>();
  auto verify(adapters::LinuxSystemdBudget&)
      -> std::expected<void, Error> override {
    ++state->verifies;
    if (state->early_failure) return std::unexpected(*state->early_failure);
    if (state->fail_final && state->final)
      return std::unexpected(Error::source_changed);
    return {};
  }
  auto open(adapters::LinuxSystemdBudget&)
      -> std::expected<std::unique_ptr<adapters::LinuxSystemdWire>,
                       Error> override {
    ++state->opens;
    return std::make_unique<Wire>(state);
  }
  auto verify_connection(adapters::LinuxSystemdWire&,
                         adapters::LinuxSystemdBudget&)
      -> std::expected<void, Error> override {
    return {};
  }
};
class Probe final : public adapters::LinuxOpsProbe {
 public:
  auto identity(adapters::LinuxOpsReadBudget& budget)
      -> std::expected<domain::LinuxOpsIdentity, Error> override {
    if (auto used = budget.consume(37); !used)
      return std::unexpected(used.error());
    return service_fixture::identity();
  }
  auto read(adapters::LinuxOpsInputFile file, adapters::LinuxOpsReadBudget&)
      -> std::expected<std::optional<std::string>, Error> override {
    if (file == adapters::LinuxOpsInputFile::memory)
      return std::optional<std::string>{
          "MemTotal: 1000 kB\nMemAvailable: 400 kB\n"};
    if (file == adapters::LinuxOpsInputFile::uptime)
      return std::optional<std::string>{"12.00 20.00\n"};
    return std::optional<std::string>{"monotonic 0 0\nboottime 0 0\n"};
  }
  auto time_identity(adapters::LinuxOpsReadBudget&)
      -> std::expected<std::optional<adapters::LinuxOpsTimeIdentity>,
                       Error> override {
    return std::optional{adapters::LinuxOpsTimeIdentity{50, 50}};
  }
};
struct Fixture {
  std::shared_ptr<Platform> platform = std::make_shared<Platform>();
  std::shared_ptr<adapters::LinuxSystemdBus> bus =
      adapters::LinuxSystemdBusAccess::create(platform).value();
  std::shared_ptr<adapters::LinuxOpsObservationSource> source =
      adapters::LinuxOpsObservationSourceAccess::create(
          id<domain::OpsTargetId>("local"),
          id<domain::OpsConfigurationRevision>("revision"),
          std::make_shared<Probe>(), bus)
          .value();
  Fixture() { require(dbus_threads_init_default() != 0); }
  auto request(bool discovery = false) const -> domain::OpsObservationRequest {
    return {id<domain::OpsOwnerId>("owner"),
            id<domain::SessionId>("session"),
            id<domain::OpsRequestId>("read"),
            source->target_binding(),
            1,
            discovery ? domain::OpsObservationOperation::linux_services
                      : domain::OpsObservationOperation::linux_service_health,
            discovery
                ? domain::OpsResourceIdentity{std::monostate{}}
                : domain::OpsResourceIdentity{domain::LinuxServiceIdentity{
                      "ssh.service", {}}},
            1,
            {}};
  }
  void handshake() {
    auto& steps = platform->state->steps;
    steps.push_back({"Hello", "", text(":1.23")});
    proof();
  }
  void proof() {
    auto& steps = platform->state->steps;
    steps.push_back({"GetNameOwner", "", text(":1.42")});
    steps.push_back({"GetConnectionUnixUser", "", number(0)});
    steps.push_back({"GetConnectionUnixProcessID", "", number(1)});
  }
  void service(const Unit& unit, bool detailed = true) {
    auto& steps = platform->state->steps;
    steps.push_back({"GetUnit", "",
                     text(adapters::linux_systemd_unit_path(unit.name).value(),
                          DBUS_TYPE_OBJECT_PATH)});
    steps.push_back({"Get", "Id", property(unit.name)});
    steps.push_back({"Get", "InvocationID", invocation(unit.before)});
    steps.push_back({"Get", "ActiveState", property(unit.state)});
    if (detailed) {
      steps.push_back({"Get", "Result", property(unit.reason)});
      steps.push_back({"Get", "ExecMainCode",
                       property_number(unit.code, DBUS_TYPE_INT32, "i")});
      steps.push_back({"Get", "ExecMainStatus",
                       property_number(unit.status, DBUS_TYPE_INT32, "i")});
      steps.push_back({"Get", "NRestarts",
                       property_number(unit.restarts, DBUS_TYPE_UINT32, "u")});
    }
    steps.push_back({"Get", "InvocationID", invocation(unit.after)});
  }
  void health(const Unit& unit = {}) {
    handshake();
    service(unit);
    proof();
  }
  void discovery(const std::vector<Unit>& units, std::size_t retained) {
    handshake();
    platform->state->steps.push_back({"ListUnitsByPatterns", "", list(units)});
    for (std::size_t index = 0; index < std::min(units.size(), retained);
         ++index)
      service(units[index], false);
    proof();
  }
};
} // namespace service_fixture
