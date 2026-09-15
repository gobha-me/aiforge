#include "linux_systemd_services.hpp"

#include <algorithm>
#include <array>
#include <csignal>
#include <limits>
#include <set>
#include <utility>

#include <aiforge/detail/utf8_text.hpp>

namespace aiforge::adapters {
namespace {
using Error = runtime::OpsObservationSourceError;
using Read = LinuxSystemdRead;
auto fail(Error error) -> std::unexpected<Error> {
  return std::unexpected(error);
}
auto timestamp() -> domain::EventTimestamp {
  return domain::EventTimestamp{
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())};
}
struct PropertyValue {
  LinuxSystemdMessage message;
  DBusMessageIter value{};
};
auto property(LinuxSystemdConnection& connection, Read operation,
              std::string_view unit, int expected, LinuxSystemdBudget& budget)
    -> std::expected<PropertyValue, Error> {
  auto reply = connection.read(operation, unit, budget);
  if (!reply) return fail(reply.error());
  if (dbus_message_has_signature(reply->get(), "v") == 0)
    return fail(Error::invalid_result);
  PropertyValue result{std::move(*reply), {}};
  DBusMessageIter outer;
  dbus_message_iter_init(result.message.get(), &outer);
  dbus_message_iter_recurse(&outer, &result.value);
  if (dbus_message_iter_get_arg_type(&result.value) != expected)
    return fail(Error::invalid_result);
  return result;
}
auto text_property(LinuxSystemdConnection& connection, Read operation,
                   std::string_view unit, std::size_t maximum,
                   LinuxSystemdBudget& budget)
    -> std::expected<std::string, Error> {
  auto reply = property(connection, operation, unit, DBUS_TYPE_STRING, budget);
  if (!reply) return fail(reply.error());
  const char* value{};
  dbus_message_iter_get_basic(&reply->value, static_cast<void*>(&value));
  if (value == nullptr) return fail(Error::invalid_result);
  const std::string_view text{value};
  if (text.empty() || text.size() > maximum || !detail::is_safe_utf8_text(text))
    return fail(Error::invalid_result);
  return std::string{text};
}
template <class Number>
auto number_property(LinuxSystemdConnection& connection, Read operation,
                     std::string_view unit, int type,
                     LinuxSystemdBudget& budget)
    -> std::expected<Number, Error> {
  auto reply = property(connection, operation, unit, type, budget);
  if (!reply) return fail(reply.error());
  Number value{};
  dbus_message_iter_get_basic(&reply->value, static_cast<void*>(&value));
  return value;
}
auto canonical_unit(LinuxSystemdConnection& connection, std::string_view unit,
                    LinuxSystemdBudget& budget) -> std::expected<void, Error> {
  auto expected = linux_systemd_unit_path(unit);
  if (!expected) return fail(expected.error());
  auto found = connection.read(Read::find_unit, unit, budget);
  if (!found) return fail(found.error());
  if (dbus_message_has_signature(found->get(), "o") == 0)
    return fail(Error::invalid_result);
  DBusMessageIter iterator;
  dbus_message_iter_init(found->get(), &iterator);
  const char* path{};
  dbus_message_iter_get_basic(&iterator, static_cast<void*>(&path));
  if (path == nullptr || std::string_view{path} != *expected)
    return fail(Error::source_changed);
  auto id = text_property(connection, Read::unit_id, unit, 255, budget);
  if (!id) return fail(id.error());
  if (*id != unit) return fail(Error::source_changed);
  return {};
}
auto invocation(LinuxSystemdConnection& connection, std::string_view unit,
                LinuxSystemdBudget& budget)
    -> std::expected<std::optional<domain::OpsResourceUid>, Error> {
  auto reply =
      property(connection, Read::invocation_id, unit, DBUS_TYPE_ARRAY, budget);
  if (!reply) return fail(reply.error());
  if (dbus_message_iter_get_element_type(&reply->value) != DBUS_TYPE_BYTE)
    return fail(Error::invalid_result);
  DBusMessageIter iterator;
  dbus_message_iter_recurse(&reply->value, &iterator);
  constexpr std::string_view digits{"0123456789abcdef"};
  std::string encoded;
  encoded.reserve(32);
  bool nonzero{};
  while (dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_INVALID) {
    if (encoded.size() == 32) return fail(Error::invalid_result);
    unsigned char byte{};
    dbus_message_iter_get_basic(&iterator, static_cast<void*>(&byte));
    nonzero = nonzero || byte != 0;
    encoded.push_back(digits[byte >> 4]);
    encoded.push_back(digits[byte & 15]);
    if (dbus_message_iter_next(&iterator) == 0) break;
  }
  if (encoded.empty()) return std::nullopt;
  if (encoded.size() != 32 || !nonzero) return fail(Error::invalid_result);
  auto result = domain::OpsResourceUid::from(std::move(encoded));
  if (!result) return fail(Error::invalid_result);
  return std::optional{std::move(*result)};
}
auto service_state(std::string_view value) -> domain::OpsServiceState {
  using State = domain::OpsServiceState;
  if (value == "active") return State::active;
  if (value == "inactive") return State::inactive;
  if (value == "activating") return State::activating;
  if (value == "deactivating") return State::deactivating;
  if (value == "failed") return State::failed;
  return State::unknown;
}
auto service_reason(std::string_view value) -> domain::OpsObservationReason {
  using Reason = domain::OpsObservationReason;
  if (value == "success") return Reason::none;
  if (value == "exit-code") return Reason::failed_exit;
  if (value == "oom-kill") return Reason::out_of_memory;
  return Reason::unknown;
}
struct ServiceValue {
  domain::LinuxServiceObservation value;
  bool partial{};
};
auto service_details(ServiceValue& result, LinuxSystemdConnection& connection,
                     std::string_view unit, LinuxSystemdBudget& budget)
    -> std::expected<void, Error> {
  auto reason =
      text_property(connection, Read::service_result, unit, 64, budget);
  if (!reason) return fail(reason.error());
  auto code = number_property<dbus_int32_t>(connection, Read::execution_code,
                                            unit, DBUS_TYPE_INT32, budget);
  if (!code) return fail(code.error());
  auto status = number_property<dbus_int32_t>(
      connection, Read::execution_status, unit, DBUS_TYPE_INT32, budget);
  if (!status) return fail(status.error());
  auto restarts = number_property<dbus_uint32_t>(
      connection, Read::restart_count, unit, DBUS_TYPE_UINT32, budget);
  if (!restarts) return fail(restarts.error());
  if (*status < 0 || *restarts > static_cast<dbus_uint32_t>(
                                     std::numeric_limits<std::int32_t>::max()))
    return fail(Error::invalid_result);
  result.value.reason = service_reason(*reason);
  result.partial = result.partial ||
                   result.value.reason == domain::OpsObservationReason::unknown;
  if (*code == CLD_EXITED) {
    if (*status > 255) return fail(Error::invalid_result);
    result.value.exit_status = *status;
  } else if (*code != 0 && *code != CLD_KILLED && *code != CLD_DUMPED) {
    result.partial = true;
  }
  result.value.restart_count = *restarts;
  return {};
}
auto service(LinuxSystemdConnection& connection, std::string_view unit,
             bool detailed, LinuxSystemdBudget& budget,
             const std::optional<domain::OpsResourceUid>& expected = {})
    -> std::expected<ServiceValue, Error> {
  if (auto canonical = canonical_unit(connection, unit, budget); !canonical)
    return fail(canonical.error());
  auto before = invocation(connection, unit, budget);
  if (!before) return fail(before.error());
  if (expected && *before != expected) return fail(Error::source_changed);
  auto state = text_property(connection, Read::active_state, unit, 64, budget);
  if (!state) return fail(state.error());
  ServiceValue result{{{std::string{unit}, *before},
                       service_state(*state),
                       domain::OpsObservationReason::unknown,
                       {},
                       {}},
                      false};
  result.partial =
      result.value.state == domain::OpsServiceState::unknown ||
      (result.value.state == domain::OpsServiceState::active && !*before);
  if (detailed) {
    if (auto details = service_details(result, connection, unit, budget);
        !details)
      return fail(details.error());
  }
  auto after = invocation(connection, unit, budget);
  if (!after) return fail(after.error());
  if (*before != *after) return fail(Error::source_changed);
  // Without an observed cycle, only the named unit's sampled state is useful;
  // avoid attributing process/counter details to an unproved runtime identity.
  if (detailed && !*before) {
    result.value.reason = domain::OpsObservationReason::unknown;
    result.value.exit_status.reset();
    result.value.restart_count.reset();
    result.partial = true;
  }
  return result;
}
struct ListRows {
  std::vector<std::string_view> names;
  std::uint64_t total{};
};
auto list_rows(DBusMessage& reply, std::size_t admitted,
               LinuxSystemdBudget& budget) -> std::expected<ListRows, Error> {
  if (dbus_message_has_signature(&reply, "a(ssssssouso)") == 0)
    return fail(Error::invalid_result);
  DBusMessageIter outer;
  dbus_message_iter_init(&reply, &outer);
  DBusMessageIter rows;
  dbus_message_iter_recurse(&outer, &rows);
  ListRows result;
  result.names.reserve(admitted);
  std::set<std::string_view> names;
  while (dbus_message_iter_get_arg_type(&rows) != DBUS_TYPE_INVALID) {
    if (auto ready = budget.check(); !ready) return fail(ready.error());
    if (++result.total > 4096) return fail(Error::resource_exhausted);
    DBusMessageIter field;
    dbus_message_iter_recurse(&rows, &field);
    const char* name{};
    dbus_message_iter_get_basic(&field, static_cast<void*>(&name));
    if (name == nullptr) return fail(Error::invalid_result);
    const std::string_view unit{name};
    auto path = linux_systemd_unit_path(unit);
    if (!path || !names.insert(unit).second) return fail(Error::invalid_result);
    for (unsigned index = 0; index < 6; ++index)
      dbus_message_iter_next(&field);
    const char* observed_path{};
    dbus_message_iter_get_basic(&field, static_cast<void*>(&observed_path));
    if (observed_path == nullptr || std::string_view{observed_path} != *path)
      return fail(Error::source_changed);
    if (result.names.size() < admitted) result.names.push_back(unit);
    if (dbus_message_iter_next(&rows) == 0) break;
  }
  return result;
}
auto validate_result(const domain::OpsObservation& result)
    -> std::expected<void, Error> {
  auto valid = domain::validate_recorded_ops_observation(result);
  if (!valid)
    return fail(valid.error().code ==
                        domain::OpsObservationErrorCode::resource_exhausted
                    ? Error::resource_exhausted
                    : Error::invalid_result);
  return {};
}
auto discover(const domain::OpsObservationRequest& request,
              LinuxSystemdConnection& connection, LinuxSystemdBudget& budget,
              domain::EventTimestamp started)
    -> std::expected<domain::OpsObservation, Error> {
  auto reply = connection.read(Read::list_services, {}, budget);
  if (!reply) return fail(reply.error());
  // Three calls finish owner proof. No detail query may spend that reserve.
  if (budget.remaining_calls < 3) return fail(Error::resource_exhausted);
  const auto admitted =
      std::min(request.limits.maximum_entries,
               static_cast<std::size_t>((budget.remaining_calls - 3) / 5));
  auto rows = list_rows(**reply, admitted, budget);
  if (!rows) return fail(rows.error());
  domain::OpsObservation result{
      request,     started,
      timestamp(), domain::OpsObservationCompleteness::complete,
      0,           0,
      {},          domain::LinuxServicesObservation{}};
  auto& output =
      std::get<domain::LinuxServicesObservation>(result.payload).services;
  output.reserve(rows->names.size());
  for (const auto unit : rows->names) {
    auto row = service(connection, unit, false, budget);
    if (!row) return fail(row.error());
    output.push_back(std::move(row->value));
    const auto previous_unsupported = result.unsupported_entries;
    result.unsupported_entries += row->partial ? 1 : 0;
    result.completeness = result.unsupported_entries != 0
                              ? domain::OpsObservationCompleteness::partial
                              : domain::OpsObservationCompleteness::complete;
    if (auto valid = validate_result(result); !valid) {
      if (valid.error() != Error::resource_exhausted)
        return fail(valid.error());
      output.pop_back();
      result.unsupported_entries = previous_unsupported;
      break;
    }
  }
  result.omitted_entries = rows->total - output.size();
  result.completeness = *result.omitted_entries != 0
                            ? domain::OpsObservationCompleteness::truncated
                        : result.unsupported_entries != 0
                            ? domain::OpsObservationCompleteness::partial
                            : domain::OpsObservationCompleteness::complete;
  return result;
}
auto inspect(const domain::OpsObservationRequest& request,
             LinuxSystemdConnection& connection, LinuxSystemdBudget& budget,
             domain::EventTimestamp started)
    -> std::expected<domain::OpsObservation, Error> {
  const auto* selected =
      std::get_if<domain::LinuxServiceIdentity>(&request.resource);
  if (selected == nullptr) return fail(Error::invalid_result);
  auto observed = service(connection, selected->unit_name, true, budget,
                          selected->invocation_id);
  if (!observed) return fail(observed.error());
  if (selected->invocation_id &&
      observed->value.identity.invocation_id != selected->invocation_id)
    return fail(Error::source_changed);
  observed->value.identity = *selected;
  return domain::OpsObservation{
      request,
      started,
      timestamp(),
      observed->partial ? domain::OpsObservationCompleteness::partial
                        : domain::OpsObservationCompleteness::complete,
      0,
      observed->partial ? 1U : 0U,
      {},
      std::move(observed->value)};
}
} // namespace
auto verify_linux_systemd_service_identity(
    LinuxSystemdConnection& connection,
    const domain::LinuxServiceIdentity& selected, LinuxSystemdBudget& budget)
    -> std::expected<void, Error> {
  try {
    if (!selected.invocation_id) return fail(Error::invalid_result);
    if (auto canonical = canonical_unit(connection, selected.unit_name, budget);
        !canonical)
      return fail(canonical.error());
    auto current = invocation(connection, selected.unit_name, budget);
    if (!current) return fail(current.error());
    if (*current != selected.invocation_id) return fail(Error::source_changed);
    return {};
  } catch (...) {
    return fail(Error::internal_failure);
  }
}
auto validate_linux_systemd_service_request(
    const domain::OpsObservationRequest& request)
    -> std::expected<void, Error> {
  if (request.operation != domain::OpsObservationOperation::linux_services &&
      request.operation !=
          domain::OpsObservationOperation::linux_service_health)
    return fail(Error::unsupported);
  if (!domain::validate_recorded_ops_request(request))
    return fail(Error::invalid_result);
  const auto* selected =
      std::get_if<domain::LinuxServiceIdentity>(&request.resource);
  if (selected == nullptr || !selected->invocation_id) return {};
  const auto& identity = selected->invocation_id->value();
  if (identity.size() != 32 ||
      !std::ranges::all_of(identity,
                           [](char byte) {
                             return (byte >= '0' && byte <= '9') ||
                                    (byte >= 'a' && byte <= 'f');
                           }) ||
      std::ranges::all_of(identity, [](char byte) { return byte == '0'; }))
    return fail(Error::invalid_result);
  return {};
}
auto observe_linux_systemd_services(
    const domain::OpsObservationRequest& request, LinuxSystemdBus& bus,
    LinuxSystemdBudget& budget, domain::EventTimestamp started_at)
    -> std::expected<domain::OpsObservation, Error> {
  try {
    if (auto request_valid = validate_linux_systemd_service_request(request);
        !request_valid)
      return fail(request_valid.error());
    budget.remaining_captured_bytes =
        std::min(budget.remaining_captured_bytes, request.limits.maximum_bytes);
    budget.deadline =
        std::min(budget.deadline,
                 std::chrono::steady_clock::now() + request.limits.timeout);
    auto connection = bus.open(budget);
    if (!connection) return fail(connection.error());
    auto result =
        request.operation == domain::OpsObservationOperation::linux_services
            ? discover(request, **connection, budget, started_at)
            : inspect(request, **connection, budget, started_at);
    if (!result) return fail(result.error());
    if (auto proof = (*connection)->verify(budget); !proof)
      return fail(proof.error());
    if (auto ready = budget.check(); !ready) return fail(ready.error());
    result->completed_at = timestamp();
    if (auto valid = validate_result(*result); !valid)
      return fail(valid.error());
    return result;
  } catch (...) {
    return fail(Error::internal_failure);
  }
}
} // namespace aiforge::adapters
