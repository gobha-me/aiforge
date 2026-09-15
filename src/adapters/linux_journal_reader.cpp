#include "linux_journal_reader.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include <aiforge/detail/utf8_text.hpp>

namespace aiforge::adapters {
namespace {
using Error = runtime::OpsObservationSourceError;
using Timestamp = domain::EventTimestamp;
auto fail(Error value) -> std::unexpected<Error> {
  return std::unexpected(value);
}
class Clock final : public LinuxJournalClock {
 public:
  [[nodiscard]] auto realtime() const -> Timestamp override {
    return std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
  }
};
auto hex_identity(std::string_view value) -> bool {
  return value.size() == 32 &&
         std::ranges::all_of(value,
                             [](char byte) {
                               return (byte >= '0' && byte <= '9') ||
                                      (byte >= 'a' && byte <= 'f');
                             }) &&
         std::ranges::any_of(value, [](char byte) { return byte != '0'; });
}
auto boot_text(const domain::LinuxOpsIdentity& identity) -> std::string {
  std::string result;
  result.reserve(32);
  for (char byte : identity.boot_id)
    if (byte != '-') result.push_back(byte);
  return result;
}
auto credential_assignment(std::string_view rest, std::string_view key)
    -> bool {
  while (true) {
    const auto found = rest.find(key);
    if (found == std::string_view::npos) return false;
    rest.remove_prefix(found + key.size());
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t' ||
                             rest.front() == '"' || rest.front() == '\''))
      rest.remove_prefix(1);
    if (!rest.empty() && (rest.front() == '=' || rest.front() == ':'))
      return true;
  }
}
auto known_secret(std::string_view text) -> bool {
  // Deliberately conservative heuristic, not general secret detection. No
  // external secret store or environment values are consulted.
  std::string lowered;
  lowered.reserve(text.size());
  for (char byte : text)
    lowered.push_back(byte >= 'A' && byte <= 'Z'
                          ? static_cast<char>(byte + ('a' - 'A'))
                          : byte);
  constexpr std::array schemes{"authorization:",
                               "authorization=",
                               "bearer ",
                               "basic ",
                               "-----begin private key",
                               "-----begin rsa private key",
                               "-----begin ec private key",
                               "-----begin openssh private key",
                               "-----begin encrypted private key"};
  for (const auto* marker : schemes)
    if (lowered.find(marker) != std::string::npos) return true;
  constexpr std::array keys{"password", "passwd", "api_key", "api-key",
                            "apikey",   "token",  "secret",  "credential"};
  return std::ranges::any_of(keys, [&](const auto* key) {
    return credential_assignment(lowered, key);
  });
}
struct Record {
  Timestamp timestamp;
  std::string message;
};
struct Field {
  std::string_view key, value;
};
auto parse_field(std::string_view field) -> std::expected<Field, Error> {
  const auto separator = field.find('=');
  if (separator == std::string_view::npos || separator == 0)
    return fail(Error::invalid_result);
  const auto key = field.substr(0, separator);
  if (!std::ranges::all_of(key, [](char byte) {
        return (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
               byte == '_';
      }))
    return fail(Error::invalid_result);
  return Field{key, field.substr(separator + 1)};
}
auto valid_message(std::string_view value) -> bool {
  return (value.empty() || detail::is_safe_utf8_text(value)) &&
         !std::ranges::any_of(
             value,
             [](unsigned char byte) { return byte < 32 && byte != '\n'; }) &&
         !known_secret(value);
}
struct FieldDecoder {
  const std::array<std::string_view, 3> expected;
  std::array<bool, 4> present{};
  auto accept(std::string_view input, Record& record)
      -> std::expected<void, Error> {
    auto field = parse_field(input);
    if (!field) return fail(field.error());
    constexpr std::array<std::string_view, 4> keys{
        "_BOOT_ID", "_SYSTEMD_UNIT", "_SYSTEMD_INVOCATION_ID", "MESSAGE"};
    const auto found = std::ranges::find(keys, field->key);
    if (found == keys.end()) return {};
    const auto slot = static_cast<std::size_t>(found - keys.begin());
    if (present[slot]) return fail(Error::invalid_result);
    present[slot] = true;
    if (slot < expected.size()) {
      if (field->value != expected[slot]) return fail(Error::source_changed);
    } else {
      if (!valid_message(field->value)) return fail(Error::invalid_result);
      record.message = field->value;
    }
    return {};
  }
};
auto read_record(LinuxJournalCursor& cursor,
                 const LinuxJournalSelection& selection,
                 LinuxSystemdBudget& budget) -> std::expected<Record, Error> {
  if (!selection.source.invocation_id) return fail(Error::invalid_result);
  auto micros = cursor.realtime_usec(budget);
  if (!micros) return fail(micros.error());
  constexpr auto maximum =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (*micros / 1000 > maximum) return fail(Error::invalid_result);
  Record result{Timestamp{std::chrono::milliseconds{
                    static_cast<std::int64_t>(*micros / 1000)}},
                {}};
  auto restarted = cursor.restart_fields(budget);
  if (!restarted) return fail(restarted.error());
  const auto boot = boot_text(selection.target);
  FieldDecoder decoder{{boot, selection.source.unit_name,
                        selection.source.invocation_id->value()}};
  for (std::size_t index = 0;; ++index) {
    if (auto ready = budget.check(); !ready) return fail(ready.error());
    auto field = cursor.next_field(budget);
    if (!field) return fail(field.error());
    if (!*field) break;
    if (index == 128 || (*field)->size() >= linux_journal_field_threshold)
      return fail(Error::resource_exhausted);
    if (auto charged = budget.consume_captured((*field)->size()); !charged)
      return fail(charged.error());
    if (auto accepted = decoder.accept(**field, result); !accepted)
      return fail(accepted.error());
  }
  if (!std::ranges::all_of(decoder.present, [](bool found) { return found; }))
    return fail(Error::invalid_result);
  return result;
}
auto valid_record_time(const Record& record, Timestamp completed,
                       std::chrono::seconds age) -> bool {
  return record.timestamp.time_since_epoch().count() >= 0 &&
         record.timestamp <= completed && completed - record.timestamp <= age;
}
auto append_record(domain::OpsObservation& result, const Record& record,
                   std::uint64_t& bytes) -> std::expected<bool, Error> {
  auto& lines = std::get<domain::OpsLogObservation>(result.payload).lines;
  const auto& limits = result.request.limits;
  const auto maximum =
      std::min(limits.maximum_entries, limits.maximum_log_lines);
  std::string_view remaining{record.message};
  while (true) {
    const auto separator = remaining.find('\n');
    const auto line = remaining.substr(0, separator);
    if (lines.size() == maximum ||
        line.size() > limits.maximum_log_bytes - bytes)
      return false;
    lines.push_back({record.timestamp, std::string{line}});
    auto valid = domain::validate_recorded_ops_observation(result);
    if (!valid || valid->evidence_bytes > limits.maximum_bytes) {
      lines.pop_back();
      if (!valid && valid.error().code !=
                        domain::OpsObservationErrorCode::resource_exhausted)
        return fail(Error::invalid_result);
      return false;
    }
    bytes += line.size();
    if (separator == std::string_view::npos) break;
    remaining.remove_prefix(separator + 1);
  }
  return true;
}
auto collect(const domain::OpsObservationRequest& request,
             LinuxJournalCursor& cursor, const LinuxJournalSelection& selection,
             LinuxSystemdBudget& budget, Timestamp started,
             const LinuxJournalClock& clock)
    -> std::expected<domain::OpsObservation, Error> {
  domain::OpsObservation result{
      request,
      started,
      clock.realtime(),
      domain::OpsObservationCompleteness::partial,
      std::nullopt,
      0,
      {},
      domain::OpsLogObservation{request.resource, {}}};
  auto& lines = std::get<domain::OpsLogObservation>(result.payload).lines;
  lines.reserve(std::min(request.limits.maximum_entries,
                         request.limits.maximum_log_lines));
  std::uint64_t bytes{};
  const auto scan_limit = std::min(request.limits.maximum_entries,
                                   request.limits.maximum_log_lines) +
                          1;
  for (std::size_t count = 0; count < scan_limit; ++count) {
    if (auto ready = budget.check(); !ready) return fail(ready.error());
    auto next = cursor.next(budget);
    if (!next) return fail(next.error());
    if (!*next) break;
    auto record = read_record(cursor, selection, budget);
    if (!record) return fail(record.error());
    result.completed_at = clock.realtime();
    if (!valid_record_time(*record, result.completed_at,
                           request.limits.maximum_log_age))
      return fail(Error::invalid_result);
    auto appended = append_record(result, *record, bytes);
    if (!appended) return fail(appended.error());
    if (!*appended || count + 1 == scan_limit) {
      result.completeness = domain::OpsObservationCompleteness::truncated;
      break;
    }
  }
  if (lines.empty()) return fail(Error::unavailable);
  return result;
}
} // namespace
auto linux_journal_clock() -> const LinuxJournalClock& {
  static const Clock clock;
  return clock;
}
auto validate_linux_journal_request(
    const domain::OpsObservationRequest& request)
    -> std::expected<void, Error> {
  if (request.operation != domain::OpsObservationOperation::linux_service_logs)
    return fail(Error::unsupported);
  if (!domain::validate_recorded_ops_request(request))
    return fail(Error::invalid_result);
  const auto* source =
      std::get_if<domain::LinuxServiceIdentity>(&request.resource);
  if (source == nullptr || !source->invocation_id ||
      !hex_identity(source->invocation_id->value()) ||
      !linux_systemd_unit_path(source->unit_name))
    return fail(Error::invalid_result);
  return {};
}
auto observe_linux_service_logs(const domain::OpsObservationRequest& request,
                                LinuxSystemdBus& bus,
                                LinuxJournalFactory& factory,
                                LinuxSystemdBudget& budget, Timestamp started,
                                const LinuxJournalClock& clock)
    -> std::expected<domain::OpsObservation, Error> {
  try {
    if (auto valid = validate_linux_journal_request(request); !valid)
      return fail(valid.error());
    const auto* target =
        std::get_if<domain::LinuxOpsIdentity>(&request.target.identity);
    if (target == nullptr) return fail(Error::invalid_result);
    const auto& source =
        std::get<domain::LinuxServiceIdentity>(request.resource);
    const auto duration =
        std::min(request.limits.timeout,
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     request.limits.maximum_log_age) /
                     2);
    budget.deadline =
        std::min(budget.deadline, std::chrono::steady_clock::now() + duration);
    budget.remaining_captured_bytes =
        std::min(budget.remaining_captured_bytes, request.limits.maximum_bytes);
    if (auto ready = budget.check(); !ready) return fail(ready.error());
    if (started.time_since_epoch().count() < 0)
      return fail(Error::invalid_result);
    const auto lower = started - request.limits.maximum_log_age + duration;
    const auto lower_ms = lower.time_since_epoch().count();
    if (lower_ms < 0 || static_cast<std::uint64_t>(lower_ms) >
                            std::numeric_limits<std::uint64_t>::max() / 1000)
      return fail(Error::invalid_result);
    LinuxJournalSelection selection{
        *target, source, static_cast<std::uint64_t>(lower_ms) * 1000};
    auto connection = bus.open(budget);
    if (!connection) return fail(connection.error());
    if (auto proof =
            verify_linux_systemd_service_identity(**connection, source, budget);
        !proof)
      return fail(proof.error());
    if (budget.remaining_calls < 6) return fail(Error::resource_exhausted);
    auto cursor = factory.open(selection, budget);
    if (!cursor) return fail(cursor.error());
    if (!*cursor) return fail(Error::internal_failure);
    auto result = collect(request, **cursor, selection, budget, started, clock);
    if (!result) return fail(result.error());
    // Release borrowed native mappings on this worker before final proof.
    cursor->reset();
    if (auto proof =
            verify_linux_systemd_service_identity(**connection, source, budget);
        !proof)
      return fail(proof.error());
    if (auto proof = (*connection)->verify(budget); !proof)
      return fail(proof.error());
    if (auto ready = budget.check(); !ready) return fail(ready.error());
    result->completed_at = clock.realtime();
    auto valid = domain::validate_recorded_ops_observation(*result);
    if (!valid || valid->evidence_bytes > request.limits.maximum_bytes)
      return fail(Error::invalid_result);
    return result;
  } catch (...) {
    return fail(Error::internal_failure);
  }
}
} // namespace aiforge::adapters
