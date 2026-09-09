#include "linux_systemd_bus.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace aiforge::adapters {
namespace {
using Error = runtime::OpsObservationSourceError;
auto fail(Error value) -> std::unexpected<Error> {
  return std::unexpected(value);
}
class Capture {
 public:
  explicit Capture(LinuxSystemdBudget& budget) : m_budget(budget) {}
  auto text(const char* value) -> std::expected<void, Error> {
    if (value == nullptr) return {};
    const std::string_view text{value};
    if (text.size() > std::size_t{64} * 1024)
      return fail(Error::resource_exhausted);
    if (auto envelope = m_budget.consume_captured(16); !envelope)
      return envelope;
    return m_budget.consume_captured(text.size());
  }
  auto body(DBusMessageIter iterator) -> std::expected<void, Error> {
    std::array<DBusMessageIter, 9> levels{};
    levels[0] = iterator;
    std::size_t depth{};
    while (true) {
      auto& current = levels[depth];
      if (dbus_message_iter_get_arg_type(&current) == DBUS_TYPE_INVALID) {
        if (depth == 0) return {};
        --depth;
        continue;
      }
      if (++m_nodes > 8192) return fail(Error::resource_exhausted);
      if (auto envelope = m_budget.consume_captured(16); !envelope)
        return envelope;
      auto child = field(current);
      if (!child) return fail(child.error());
      dbus_message_iter_next(&current);
      if (*child) {
        if (depth + 1 == levels.size()) return fail(Error::resource_exhausted);
        levels[++depth] = **child;
      }
    }
  }

 private:
  auto field(DBusMessageIter& iterator)
      -> std::expected<std::optional<DBusMessageIter>, Error> {
    switch (dbus_message_iter_get_arg_type(&iterator)) {
      case DBUS_TYPE_STRING:
      case DBUS_TYPE_OBJECT_PATH:
      case DBUS_TYPE_SIGNATURE: {
        const char* value{};
        dbus_message_iter_get_basic(&iterator, static_cast<void*>(&value));
        if (auto charged = text(value); !charged) return fail(charged.error());
        return std::nullopt;
      }
      case DBUS_TYPE_ARRAY:
      case DBUS_TYPE_VARIANT:
      case DBUS_TYPE_STRUCT:
      case DBUS_TYPE_DICT_ENTRY: {
        DBusMessageIter child;
        dbus_message_iter_recurse(&iterator, &child);
        return child;
      }
      case DBUS_TYPE_BYTE:
      case DBUS_TYPE_BOOLEAN:
      case DBUS_TYPE_INT16:
      case DBUS_TYPE_UINT16:
      case DBUS_TYPE_INT32:
      case DBUS_TYPE_UINT32:
      case DBUS_TYPE_INT64:
      case DBUS_TYPE_UINT64:
      case DBUS_TYPE_DOUBLE: return std::nullopt;
      default: return fail(Error::invalid_result);
    }
  }
  LinuxSystemdBudget& m_budget;
  std::size_t m_nodes{};
};
} // namespace
auto charge_linux_systemd_message(DBusMessage& message,
                                  LinuxSystemdBudget& budget)
    -> std::expected<void, Error> {
  if (auto envelope = budget.consume_captured(64); !envelope) return envelope;
  Capture capture{budget};
  const std::array headers{dbus_message_get_path(&message),
                           dbus_message_get_interface(&message),
                           dbus_message_get_member(&message),
                           dbus_message_get_sender(&message),
                           dbus_message_get_destination(&message),
                           dbus_message_get_error_name(&message),
                           dbus_message_get_signature(&message)};
  for (const auto* value : headers) {
    if (auto charged = capture.text(value); !charged) return charged;
  }
  DBusMessageIter iterator;
  dbus_message_iter_init(&message, &iterator);
  return capture.body(iterator);
}
} // namespace aiforge::adapters
