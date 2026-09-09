#include "linux_systemd_bus.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace aiforge::adapters {
namespace {
using Error = runtime::OpsObservationSourceError;
using Clock = std::chrono::steady_clock;
constexpr const char* bus_name = "org.freedesktop.DBus";
constexpr const char* bus_path = "/org/freedesktop/DBus";
constexpr const char* manager_name = "org.freedesktop.systemd1";
constexpr const char* manager_path = "/org/freedesktop/systemd1";
constexpr const char* manager_interface = "org.freedesktop.systemd1.Manager";
constexpr const char* properties_interface = "org.freedesktop.DBus.Properties";
auto fail(Error error) -> std::unexpected<Error> {
  return std::unexpected(error);
}
auto unique_name(const char* value) -> bool {
  if (value == nullptr) return false;
  const std::string_view text{value};
  return text.size() > 1 && text.size() <= 255 && text.front() == ':' &&
         dbus_validate_bus_name(value, nullptr) != 0;
}
auto string_reply(DBusMessage& message) -> std::expected<std::string, Error> {
  if (dbus_message_has_signature(&message, "s") == 0)
    return fail(Error::invalid_result);
  DBusMessageIter iterator;
  dbus_message_iter_init(&message, &iterator);
  const char* value{};
  dbus_message_iter_get_basic(&iterator, static_cast<void*>(&value));
  if (value == nullptr || std::string_view{value}.size() > 255)
    return fail(Error::invalid_result);
  return std::string{value};
}
auto integer_reply(DBusMessage& message)
    -> std::expected<std::uint32_t, Error> {
  if (dbus_message_has_signature(&message, "u") == 0)
    return fail(Error::invalid_result);
  DBusMessageIter iterator;
  dbus_message_iter_init(&message, &iterator);
  dbus_uint32_t value{};
  dbus_message_iter_get_basic(&iterator, static_cast<void*>(&value));
  return value;
}
auto append_strings(DBusMessage& message, const char* first,
                    const char* second = nullptr)
    -> std::expected<void, Error> {
  DBusMessageIter iterator;
  dbus_message_iter_init_append(&message, &iterator);
  if (dbus_message_iter_append_basic(&iterator, DBUS_TYPE_STRING,
                                     static_cast<const void*>(&first)) == 0 ||
      (second != nullptr &&
       dbus_message_iter_append_basic(&iterator, DBUS_TYPE_STRING,
                                      static_cast<const void*>(&second)) == 0))
    return fail(Error::resource_exhausted);
  return {};
}
auto error_reply(DBusMessage& message) -> Error {
  const auto* name = dbus_message_get_error_name(&message);
  if (name == nullptr) return Error::invalid_result;
  const std::string_view value{name};
  if (value == DBUS_ERROR_ACCESS_DENIED || value == DBUS_ERROR_AUTH_FAILED)
    return Error::permission_denied;
  if (value == DBUS_ERROR_NO_REPLY || value == DBUS_ERROR_TIMEOUT)
    return Error::timed_out;
  if (value == DBUS_ERROR_DISCONNECTED) return Error::disconnected;
  if (value == DBUS_ERROR_NO_MEMORY || value == DBUS_ERROR_LIMITS_EXCEEDED)
    return Error::resource_exhausted;
  if (value == DBUS_ERROR_NAME_HAS_NO_OWNER ||
      value == DBUS_ERROR_SERVICE_UNKNOWN)
    return Error::unavailable;
  return Error::invalid_result;
}
auto valid_unit(std::string_view unit) -> bool {
  if (unit.size() <= 8 || unit.size() > 255 || !unit.ends_with(".service") ||
      unit.front() == '-')
    return false;
  return std::ranges::all_of(unit, [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-' ||
           value == '.' || value == '@' || value == ':';
  });
}
auto unit_path(std::string_view unit) -> std::string {
  constexpr std::string_view digits{"0123456789abcdef"};
  std::string result{std::string{manager_path} + "/unit/"};
  for (const auto character : unit) {
    const auto byte = static_cast<unsigned char>(character);
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9')) {
      result.push_back(character);
    } else {
      result.push_back('_');
      result.push_back(digits[byte >> 4]);
      result.push_back(digits[byte & 15]);
    }
  }
  return result;
}
struct Property {
  const char* interface;
  const char* name;
};
auto property(LinuxSystemdRead operation) -> std::optional<Property> {
  constexpr const char* unit = "org.freedesktop.systemd1.Unit";
  constexpr const char* service = "org.freedesktop.systemd1.Service";
  switch (operation) {
    case LinuxSystemdRead::unit_id: return Property{unit, "Id"};
    case LinuxSystemdRead::active_state: return Property{unit, "ActiveState"};
    case LinuxSystemdRead::invocation_id: return Property{unit, "InvocationID"};
    case LinuxSystemdRead::service_result: return Property{service, "Result"};
    case LinuxSystemdRead::execution_code:
      return Property{service, "ExecMainCode"};
    case LinuxSystemdRead::execution_status:
      return Property{service, "ExecMainStatus"};
    case LinuxSystemdRead::restart_count: return Property{service, "NRestarts"};
    case LinuxSystemdRead::list_services:
    case LinuxSystemdRead::find_unit: return std::nullopt;
  }
  return std::nullopt;
}
class ArrayWriter {
 public:
  explicit ArrayWriter(DBusMessageIter& parent) : m_parent(parent) {}
  ~ArrayWriter() {
    // Valid after failed open/close and successful close, per the public API.
    dbus_message_iter_abandon_container_if_open(&m_parent, &m_array);
  }
  ArrayWriter(const ArrayWriter&) = delete;
  auto operator=(const ArrayWriter&) -> ArrayWriter& = delete;
  [[nodiscard]] auto append(const char* value) -> std::expected<void, Error> {
    if (dbus_message_iter_open_container(&m_parent, DBUS_TYPE_ARRAY, "s",
                                         &m_array) == 0)
      return fail(Error::resource_exhausted);
    if (value != nullptr &&
        dbus_message_iter_append_basic(&m_array, DBUS_TYPE_STRING,
                                       static_cast<const void*>(&value)) == 0)
      return fail(Error::resource_exhausted);
    if (dbus_message_iter_close_container(&m_parent, &m_array) == 0)
      return fail(Error::resource_exhausted);
    return {};
  }

 private:
  DBusMessageIter& m_parent;
  DBusMessageIter m_array = DBUS_MESSAGE_ITER_INIT_CLOSED;
};
auto list_arguments(DBusMessage& message) -> std::expected<void, Error> {
  DBusMessageIter outer;
  dbus_message_iter_init_append(&message, &outer);
  ArrayWriter writer{outer};
  if (auto states = writer.append(nullptr); !states) return states;
  return writer.append("*.service");
}
auto read_message(const std::string& owner, LinuxSystemdRead operation,
                  std::string_view unit)
    -> std::expected<LinuxSystemdMessage, Error> {
  const auto selected = property(operation);
  if (operation != LinuxSystemdRead::list_services &&
      operation != LinuxSystemdRead::find_unit && !selected)
    return fail(Error::unsupported);
  if (operation == LinuxSystemdRead::list_services ? !unit.empty()
                                                   : !valid_unit(unit))
    return fail(Error::invalid_result);
  const auto path = selected ? unit_path(unit) : std::string{manager_path};
  const auto* interface = selected ? properties_interface : manager_interface;
  const auto* member = selected ? "Get"
                       : operation == LinuxSystemdRead::find_unit
                           ? "GetUnit"
                           : "ListUnitsByPatterns";
  LinuxSystemdMessage message{dbus_message_new_method_call(
      owner.c_str(), path.c_str(), interface, member)};
  if (!message) return fail(Error::resource_exhausted);
  std::expected<void, Error> appended;
  if (selected) {
    appended = append_strings(*message, selected->interface, selected->name);
  } else if (operation == LinuxSystemdRead::find_unit) {
    const std::string name{unit};
    appended = append_strings(*message, name.c_str());
  } else {
    appended = list_arguments(*message);
  }
  if (!appended) return fail(appended.error());
  return message;
}
auto validate_reply(DBusMessage& reply, std::uint32_t serial,
                    const std::string& destination)
    -> std::expected<bool, Error> {
  if (dbus_message_contains_unix_fds(&reply) != 0)
    return fail(Error::invalid_result);
  if (dbus_message_get_reply_serial(&reply) != serial) return false;
  const auto* sender = dbus_message_get_sender(&reply);
  const auto type = dbus_message_get_type(&reply);
  if (sender == nullptr) return fail(Error::trust_failed);
  const std::string_view origin{sender};
  // The verified bus can reject routing to a vanished or denied unique owner.
  // It cannot supply a successful manager payload through this exception.
  if (origin != destination &&
      (type != DBUS_MESSAGE_TYPE_ERROR || origin != bus_name))
    return fail(Error::trust_failed);
  if (type == DBUS_MESSAGE_TYPE_ERROR) return fail(error_reply(reply));
  if (type != DBUS_MESSAGE_TYPE_METHOD_RETURN)
    return fail(Error::invalid_result);
  return true;
}
} // namespace
void LinuxSystemdMessageDeleter::operator()(
    DBusMessage* message) const noexcept {
  if (message != nullptr) dbus_message_unref(message);
}
auto LinuxSystemdBudget::check() const -> std::expected<void, Error> {
  if (stop.stop_requested()) return fail(Error::cancelled);
  const auto now = Clock::now();
  if (now >= deadline) return fail(Error::timed_out);
  if (deadline - now > std::chrono::seconds{5} || remaining_calls > 1024)
    return fail(Error::invalid_result);
  if (remaining_calls == 0) return fail(Error::resource_exhausted);
  return {};
}
auto validate_linux_systemd_version(int major, int minor, int micro)
    -> std::expected<void, Error> {
  if (major < 0 || minor < 0 || micro < 0 || major < 1 ||
      (major == 1 && (minor < 16 || (minor == 16 && micro < 2))))
    return fail(Error::unsupported);
  return {};
}
auto validate_linux_systemd_peer(const domain::LinuxOpsIdentity& binding,
                                 std::uint64_t user_namespace,
                                 const LinuxSystemdPeer& peer)
    -> std::expected<void, Error> {
  if (peer.process_id == 0 || binding.pid_namespace == 0 ||
      binding.mount_namespace == 0 || user_namespace == 0 ||
      peer.pid_namespace != binding.pid_namespace ||
      peer.mount_namespace != binding.mount_namespace ||
      peer.user_namespace != user_namespace)
    return fail(Error::trust_failed);
  return {};
}
struct LinuxSystemdConnection::Impl {
  std::shared_ptr<LinuxSystemdPlatform> platform;
  std::unique_ptr<LinuxSystemdWire> wire;
  std::string owner;
  bool failed{};
  auto receive_reply(std::uint32_t serial, const std::string& destination,
                     LinuxSystemdBudget& budget) const
      -> std::expected<LinuxSystemdMessage, Error> {
    unsigned unrelated{};
    while (true) {
      if (budget.stop.stop_requested()) return fail(Error::cancelled);
      const auto remaining = budget.deadline - Clock::now();
      if (remaining <= Clock::duration::zero()) return fail(Error::timed_out);
      const auto wait = std::min(
          std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
          std::chrono::milliseconds{25});
      auto received = wire->receive(wait);
      if (!received) return fail(received.error());
      if (budget.stop.stop_requested()) return fail(Error::cancelled);
      if (Clock::now() >= budget.deadline) return fail(Error::timed_out);
      if (!*received) continue;
      auto accepted = validate_reply(**received, serial, destination);
      if (!accepted) return fail(accepted.error());
      if (*accepted) return std::move(*received);
      if (++unrelated > 8) return fail(Error::resource_exhausted);
    }
  }
  auto exchange(LinuxSystemdMessage message, LinuxSystemdBudget& budget)
      -> std::expected<LinuxSystemdMessage, Error> {
    if (failed) return fail(Error::disconnected);
    // Any transport failure permanently retires this connection. Invalid
    // requests are rejected before this point and send nothing.
    failed = true;
    if (auto ready = budget.check(); !ready) return fail(ready.error());
    if (!message) return fail(Error::resource_exhausted);
    dbus_message_set_auto_start(message.get(), 0);
    dbus_message_set_allow_interactive_authorization(message.get(), 0);
    const std::string destination{dbus_message_get_destination(message.get())};
    auto serial = wire->send(*message);
    if (!serial) return fail(serial.error());
    if (*serial == 0) return fail(Error::invalid_result);
    --budget.remaining_calls;
    auto result = receive_reply(*serial, destination, budget);
    if (result) failed = false;
    return result;
  }
  auto bus_call(const char* member, const char* argument,
                LinuxSystemdBudget& budget)
      -> std::expected<LinuxSystemdMessage, Error> {
    LinuxSystemdMessage message{
        dbus_message_new_method_call(bus_name, bus_path, bus_name, member)};
    if (!message) return fail(Error::resource_exhausted);
    if (argument != nullptr) {
      if (auto appended = append_strings(*message, argument); !appended)
        return fail(appended.error());
    }
    return exchange(std::move(message), budget);
  }
  auto check_owner(LinuxSystemdBudget& budget, bool initial)
      -> std::expected<void, Error> {
    auto named = bus_call("GetNameOwner", manager_name, budget);
    if (!named) return fail(named.error());
    auto name = string_reply(**named);
    if (!name || !unique_name(name->c_str()))
      return fail(Error::invalid_result);
    if (!initial && *name != owner) return fail(Error::source_changed);
    auto user = bus_call("GetConnectionUnixUser", name->c_str(), budget);
    if (!user) return fail(user.error());
    auto uid = integer_reply(**user);
    if (!uid || *uid != 0) return fail(Error::trust_failed);
    auto process =
        bus_call("GetConnectionUnixProcessID", name->c_str(), budget);
    if (!process) return fail(process.error());
    auto pid = integer_reply(**process);
    if (!pid || *pid != 1) return fail(Error::trust_failed);
    owner = std::move(*name);
    if (auto verified = platform->verify(budget); !verified) return verified;
    return platform->verify_connection(*wire, budget);
  }
};
LinuxSystemdConnection::LinuxSystemdConnection(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl)) {
}
LinuxSystemdConnection::~LinuxSystemdConnection() = default;
auto LinuxSystemdConnection::read(LinuxSystemdRead operation,
                                  std::string_view unit,
                                  LinuxSystemdBudget& budget)
    -> std::expected<LinuxSystemdMessage, Error> {
  try {
    if (m_impl->failed) return fail(Error::disconnected);
    if (auto ready = budget.check(); !ready) return fail(ready.error());
    auto message = read_message(m_impl->owner, operation, unit);
    if (!message) return fail(message.error());
    if (auto verified = m_impl->platform->verify(budget); !verified) {
      m_impl->failed = true;
      return fail(verified.error());
    }
    if (auto verified =
            m_impl->platform->verify_connection(*m_impl->wire, budget);
        !verified) {
      m_impl->failed = true;
      return fail(verified.error());
    }
    return m_impl->exchange(std::move(*message), budget);
  } catch (...) {
    m_impl->failed = true;
    return fail(Error::internal_failure);
  }
}
auto LinuxSystemdConnection::verify(LinuxSystemdBudget& budget)
    -> std::expected<void, Error> {
  try {
    auto verified = m_impl->check_owner(budget, false);
    if (!verified) m_impl->failed = true;
    return verified;
  } catch (...) {
    m_impl->failed = true;
    return fail(Error::internal_failure);
  }
}
LinuxSystemdBus::LinuxSystemdBus(std::shared_ptr<LinuxSystemdPlatform> platform)
    : m_platform(std::move(platform)) {
}
auto LinuxSystemdBusAccess::create(
    std::shared_ptr<LinuxSystemdPlatform> platform)
    -> std::expected<std::shared_ptr<LinuxSystemdBus>, Error> {
  try {
    if (!platform) return fail(Error::unavailable);
    return std::shared_ptr<LinuxSystemdBus>{
        new LinuxSystemdBus{std::move(platform)}};
  } catch (...) {
    return fail(Error::internal_failure);
  }
}
auto LinuxSystemdBus::open(LinuxSystemdBudget& budget) const
    -> std::expected<std::unique_ptr<LinuxSystemdConnection>, Error> {
  try {
    if (auto ready = budget.check(); !ready) return fail(ready.error());
    if (auto verified = m_platform->verify(budget); !verified)
      return fail(verified.error());
    auto wire = m_platform->open(budget);
    if (!wire) return fail(wire.error());
    auto impl = std::make_unique<LinuxSystemdConnection::Impl>();
    impl->platform = m_platform;
    impl->wire = std::move(*wire);
    if (!impl->wire) return fail(Error::unavailable);
    if (auto verified = m_platform->verify_connection(*impl->wire, budget);
        !verified)
      return fail(verified.error());
    auto hello = impl->bus_call("Hello", nullptr, budget);
    if (!hello) return fail(hello.error());
    auto name = string_reply(**hello);
    if (!name || !unique_name(name->c_str()))
      return fail(Error::invalid_result);
    if (auto checked = impl->check_owner(budget, true); !checked)
      return fail(checked.error());
    return std::unique_ptr<LinuxSystemdConnection>{
        new LinuxSystemdConnection{std::move(impl)}};
  } catch (...) {
    return fail(Error::internal_failure);
  }
}
} // namespace aiforge::adapters
