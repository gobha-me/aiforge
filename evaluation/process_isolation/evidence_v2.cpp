#include "evidence_v2.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>

namespace aiforge::evaluation::process_isolation::v2 {
namespace {

using Json = nlohmann::json;

constexpr std::array probe_ids{
    ProbeId::cgroup_v2_delegation,
    ProbeId::cgroup_required_controllers,
    ProbeId::cgroup_atomic_child_placement,
    ProbeId::cgroup_self_migration_denial,
    ProbeId::cgroup_whole_tree_enumeration,
    ProbeId::cgroup_kill,
    ProbeId::cgroup_populated_zero,
    ProbeId::cgroup_setsid_containment,
    ProbeId::cgroup_double_fork_containment,
    ProbeId::cgroup_daemon_containment,
    ProbeId::cgroup_clone_fork_fanout,
    ProbeId::cgroup_leader_exit_containment,
    ProbeId::cgroup_cancellation_cleanup,
    ProbeId::cgroup_cpu_limit_enforcement,
    ProbeId::cgroup_memory_limit_termination,
    ProbeId::cgroup_pids_limit_enforcement,
    ProbeId::landlock_read_confinement,
    ProbeId::landlock_write_confinement,
    ProbeId::landlock_execute_confinement,
    ProbeId::seccomp_internet_socket_family_denial,
    ProbeId::seccomp_unix_socket_denial,
    ProbeId::private_root_construction,
    ProbeId::private_mount_propagation,
    ProbeId::descriptor_relative_launch,
    ProbeId::staged_input_identity,
    ProbeId::staged_output_identity,
    ProbeId::combined_setup_order,
    ProbeId::private_root_combined_setup_order,
    ProbeId::partial_setup_cleanup,
};

constexpr std::array probe_names{
    std::string_view{"cgroup_v2_delegation"},
    std::string_view{"cgroup_required_controllers"},
    std::string_view{"cgroup_atomic_child_placement"},
    std::string_view{"cgroup_self_migration_denial"},
    std::string_view{"cgroup_whole_tree_enumeration"},
    std::string_view{"cgroup_kill"},
    std::string_view{"cgroup_populated_zero"},
    std::string_view{"cgroup_setsid_containment"},
    std::string_view{"cgroup_double_fork_containment"},
    std::string_view{"cgroup_daemon_containment"},
    std::string_view{"cgroup_clone_fork_fanout"},
    std::string_view{"cgroup_leader_exit_containment"},
    std::string_view{"cgroup_cancellation_cleanup"},
    std::string_view{"cgroup_cpu_limit_enforcement"},
    std::string_view{"cgroup_memory_limit_termination"},
    std::string_view{"cgroup_pids_limit_enforcement"},
    std::string_view{"landlock_read_confinement"},
    std::string_view{"landlock_write_confinement"},
    std::string_view{"landlock_execute_confinement"},
    std::string_view{"seccomp_internet_socket_family_denial"},
    std::string_view{"seccomp_unix_socket_denial"},
    std::string_view{"private_root_construction"},
    std::string_view{"private_mount_propagation"},
    std::string_view{"descriptor_relative_launch"},
    std::string_view{"staged_input_identity"},
    std::string_view{"staged_output_identity"},
    std::string_view{"combined_setup_order"},
    std::string_view{"private_root_combined_setup_order"},
    std::string_view{"partial_setup_cleanup"},
};

constexpr std::array states{ProbeState::enforced, ProbeState::unavailable,
                            ProbeState::probe_error};
constexpr std::array state_names{std::string_view{"enforced"},
                                 std::string_view{"unavailable"},
                                 std::string_view{"probe_error"}};

constexpr std::array reason_codes{
    ReasonCode::none,
    ReasonCode::unsupported_kernel,
    ReasonCode::unsupported_architecture,
    ReasonCode::permission_denied,
    ReasonCode::mechanism_absent,
    ReasonCode::missing_delegation,
    ReasonCode::missing_controller,
    ReasonCode::enforcement_failed,
    ReasonCode::prerequisite_unavailable,
    ReasonCode::unsupported_combination,
    ReasonCode::timeout,
    ReasonCode::cancelled,
    ReasonCode::limit_not_triggered,
    ReasonCode::pid_reuse,
    ReasonCode::setup_race,
    ReasonCode::signaled,
    ReasonCode::nonzero_exit,
    ReasonCode::malformed_protocol,
    ReasonCode::output_limit,
    ReasonCode::cleanup_failed,
    ReasonCode::internal_error,
};

constexpr std::array reason_names{
    std::string_view{"none"},
    std::string_view{"unsupported_kernel"},
    std::string_view{"unsupported_architecture"},
    std::string_view{"permission_denied"},
    std::string_view{"mechanism_absent"},
    std::string_view{"missing_delegation"},
    std::string_view{"missing_controller"},
    std::string_view{"enforcement_failed"},
    std::string_view{"prerequisite_unavailable"},
    std::string_view{"unsupported_combination"},
    std::string_view{"timeout"},
    std::string_view{"cancelled"},
    std::string_view{"limit_not_triggered"},
    std::string_view{"pid_reuse"},
    std::string_view{"setup_race"},
    std::string_view{"signaled"},
    std::string_view{"nonzero_exit"},
    std::string_view{"malformed_protocol"},
    std::string_view{"output_limit"},
    std::string_view{"cleanup_failed"},
    std::string_view{"internal_error"},
};

static_assert(probe_ids.size() == probe_names.size());
static_assert(states.size() == state_names.size());
static_assert(reason_codes.size() == reason_names.size());

class DuplicateJsonField final : public std::exception {};

[[nodiscard]] auto failure(const EvidenceErrorCode code, std::string message)
    -> std::unexpected<EvidenceError> {
  return std::unexpected(EvidenceError{code, std::move(message)});
}

template <typename Enum, std::size_t Size>
[[nodiscard]] auto enum_name(const Enum value,
                             const std::array<Enum, Size>& values,
                             const std::array<std::string_view, Size>& names)
    -> std::string_view {
  const auto found = std::ranges::find(values, value);
  if (found == values.end()) return {};
  return names[static_cast<std::size_t>(found - values.begin())];
}

template <typename Enum, std::size_t Size>
[[nodiscard]] auto parse_enum(const Json& value,
                              const std::array<Enum, Size>& values,
                              const std::array<std::string_view, Size>& names)
    -> std::optional<Enum> {
  if (!value.is_string()) return std::nullopt;
  const auto& text = value.get_ref<const std::string&>();
  const auto found = std::ranges::find(names, text);
  if (found == names.end()) return std::nullopt;
  return values[static_cast<std::size_t>(found - names.begin())];
}

[[nodiscard]] auto parse_json(const std::string_view document,
                              const std::size_t maximum_bytes)
    -> std::expected<Json, EvidenceError> {
  if (document.empty()) {
    return failure(EvidenceErrorCode::malformed_json, "evidence JSON is empty");
  }
  if (document.size() > maximum_bytes) {
    return failure(EvidenceErrorCode::resource_exhausted,
                   "evidence JSON exceeds its byte limit");
  }
  try {
    std::vector<std::unordered_set<std::string>> object_fields;
    const auto callback = [&object_fields](const int,
                                           const Json::parse_event_t event,
                                           Json& parsed) {
      if (event == Json::parse_event_t::object_start) {
        object_fields.emplace_back();
      } else if (event == Json::parse_event_t::key) {
        if (object_fields.empty() ||
            !object_fields.back().insert(parsed.get<std::string>()).second) {
          throw DuplicateJsonField{};
        }
      } else if (event == Json::parse_event_t::object_end) {
        object_fields.pop_back();
      }
      return true;
    };
    return Json::parse(document.begin(), document.end(), callback, true, false);
  } catch (const DuplicateJsonField&) {
    return failure(EvidenceErrorCode::duplicate_field,
                   "evidence JSON contains a duplicate field");
  } catch (const Json::exception&) {
    return failure(EvidenceErrorCode::malformed_json,
                   "evidence JSON is malformed");
  } catch (...) {
    return failure(EvidenceErrorCode::resource_exhausted,
                   "evidence JSON could not be parsed within bounds");
  }
}

template <std::size_t Size>
[[nodiscard]] auto validate_object_shape(
    const Json& value, const std::array<std::string_view, Size>& fields)
    -> std::expected<void, EvidenceError> {
  if (!value.is_object()) {
    return failure(EvidenceErrorCode::invalid_value,
                   "evidence JSON value must be an object");
  }
  for (const auto& [name, ignored] : value.items()) {
    static_cast<void>(ignored);
    if (std::ranges::find(fields, name) == fields.end()) {
      return failure(EvidenceErrorCode::unknown_field,
                     "evidence JSON contains an unknown field");
    }
  }
  for (const auto field : fields) {
    if (!value.contains(field)) {
      return failure(EvidenceErrorCode::missing_field,
                     "evidence JSON is missing a required field");
    }
  }
  return {};
}

[[nodiscard]] auto valid_source_sha(const std::string_view value) -> bool {
  return value.size() == 40 &&
         std::ranges::all_of(value, [](const unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] auto valid_platform_metadata(const std::string_view value)
    -> bool {
  return !value.empty() && value.size() <= maximum_platform_metadata_bytes &&
         std::ranges::all_of(value, [](const unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '.' ||
                  character == '_' || character == '-' || character == '+';
         });
}

[[nodiscard]] auto valid_reason(const ProbeState state, const ReasonCode reason)
    -> bool {
  switch (reason) {
    case ReasonCode::none: return state == ProbeState::enforced;
    case ReasonCode::unsupported_kernel:
    case ReasonCode::unsupported_architecture:
    case ReasonCode::permission_denied:
    case ReasonCode::mechanism_absent:
    case ReasonCode::missing_delegation:
    case ReasonCode::missing_controller:
    case ReasonCode::enforcement_failed:
    case ReasonCode::prerequisite_unavailable:
    case ReasonCode::unsupported_combination:
    case ReasonCode::limit_not_triggered:
      return state == ProbeState::unavailable;
    case ReasonCode::timeout:
    case ReasonCode::cancelled:
    case ReasonCode::pid_reuse:
    case ReasonCode::setup_race:
    case ReasonCode::signaled:
    case ReasonCode::nonzero_exit:
    case ReasonCode::malformed_protocol:
    case ReasonCode::output_limit:
    case ReasonCode::cleanup_failed:
    case ReasonCode::internal_error: return state == ProbeState::probe_error;
  }
  return false;
}

[[nodiscard]] auto record_json(const ProbeRecord& value,
                               const bool include_schema) -> Json {
  Json result{{"probe_id", probe_id_name(value.probe_id)},
              {"reason", reason_code_name(value.reason)},
              {"state", probe_state_name(value.state)}};
  if (include_schema) result["schema_version"] = evidence_schema_version;
  return result;
}

[[nodiscard]] auto parse_record_json(const Json& value,
                                     const bool includes_schema)
    -> std::expected<ProbeRecord, EvidenceError> {
  constexpr std::array child_fields{
      std::string_view{"probe_id"}, std::string_view{"reason"},
      std::string_view{"schema_version"}, std::string_view{"state"}};
  constexpr std::array report_fields{std::string_view{"probe_id"},
                                     std::string_view{"reason"},
                                     std::string_view{"state"}};
  auto shape = includes_schema ? validate_object_shape(value, child_fields)
                               : validate_object_shape(value, report_fields);
  if (!shape) return std::unexpected(std::move(shape.error()));
  if (includes_schema &&
      (!value.at("schema_version").is_number_integer() ||
       value.at("schema_version") != evidence_schema_version)) {
    return failure(EvidenceErrorCode::invalid_schema,
                   "child evidence schema version is unsupported");
  }
  const auto probe = parse_enum(value.at("probe_id"), probe_ids, probe_names);
  const auto state = parse_enum(value.at("state"), states, state_names);
  const auto reason =
      parse_enum(value.at("reason"), reason_codes, reason_names);
  if (!probe || !state || !reason) {
    return failure(EvidenceErrorCode::invalid_value,
                   "child evidence contains an invalid closed value");
  }
  ProbeRecord result{*probe, *state, *reason};
  auto valid = validate_child_record(result);
  if (!valid) return std::unexpected(std::move(valid.error()));
  return result;
}

[[nodiscard]] auto report_json(const EvidenceReport& value) -> Json {
  auto rows = Json::array();
  for (const auto& probe : value.probes)
    rows.push_back(record_json(probe, false));
  return {{"architecture", value.architecture},
          {"kernel", value.kernel},
          {"platform", value.platform},
          {"probes", std::move(rows)},
          {"schema_version", evidence_schema_version},
          {"source_sha", value.source_sha}};
}

} // namespace

auto required_probe_ids() -> std::span<const ProbeId> {
  return probe_ids;
}

auto probe_id_name(const ProbeId value) -> std::string_view {
  return enum_name(value, probe_ids, probe_names);
}

auto reason_code_name(const ReasonCode value) -> std::string_view {
  return enum_name(value, reason_codes, reason_names);
}

auto validate_child_record(const ProbeRecord& value)
    -> std::expected<void, EvidenceError> {
  if (probe_id_name(value.probe_id).empty() ||
      probe_state_name(value.state).empty() ||
      reason_code_name(value.reason).empty() ||
      !valid_reason(value.state, value.reason)) {
    return failure(EvidenceErrorCode::invalid_value,
                   "child evidence contains an invalid closed value");
  }
  return {};
}

auto serialize_child_record(const ProbeRecord& value)
    -> std::expected<std::string, EvidenceError> {
  auto valid = validate_child_record(value);
  if (!valid) return std::unexpected(std::move(valid.error()));
  try {
    auto document = record_json(value, true).dump();
    document.push_back('\n');
    if (document.size() > maximum_child_record_bytes) {
      return failure(EvidenceErrorCode::resource_exhausted,
                     "child evidence JSON exceeds its byte limit");
    }
    return document;
  } catch (...) {
    return failure(EvidenceErrorCode::resource_exhausted,
                   "child evidence JSON could not be serialized");
  }
}

auto parse_child_record(const std::string_view document)
    -> std::expected<ProbeRecord, EvidenceError> {
  auto parsed = parse_json(document, maximum_child_record_bytes);
  if (!parsed) return std::unexpected(std::move(parsed.error()));
  return parse_record_json(*parsed, true);
}

auto validate_report(const EvidenceReport& value)
    -> std::expected<void, EvidenceError> {
  if (!valid_source_sha(value.source_sha) || value.platform != "linux" ||
      !valid_platform_metadata(value.kernel) ||
      !valid_platform_metadata(value.architecture)) {
    return failure(EvidenceErrorCode::invalid_value,
                   "evidence report provenance is invalid");
  }
  if (value.probes.size() != probe_ids.size()) {
    return failure(EvidenceErrorCode::invalid_value,
                   "evidence report is incomplete");
  }
  for (std::size_t index{}; index < probe_ids.size(); ++index) {
    if (value.probes[index].probe_id != probe_ids[index]) {
      return failure(EvidenceErrorCode::invalid_value,
                     "evidence report probe order is invalid");
    }
    auto valid = validate_child_record(value.probes[index]);
    if (!valid) return std::unexpected(std::move(valid.error()));
  }
  return {};
}

auto serialize_report(const EvidenceReport& value)
    -> std::expected<std::string, EvidenceError> {
  auto valid = validate_report(value);
  if (!valid) return std::unexpected(std::move(valid.error()));
  try {
    auto document = report_json(value).dump();
    document.push_back('\n');
    if (document.size() > maximum_report_bytes) {
      return failure(EvidenceErrorCode::resource_exhausted,
                     "evidence report JSON exceeds its byte limit");
    }
    return document;
  } catch (...) {
    return failure(EvidenceErrorCode::resource_exhausted,
                   "evidence report JSON could not be serialized");
  }
}

auto parse_report(const std::string_view document)
    -> std::expected<EvidenceReport, EvidenceError> {
  auto parsed = parse_json(document, maximum_report_bytes);
  if (!parsed) return std::unexpected(std::move(parsed.error()));
  constexpr std::array fields{
      std::string_view{"architecture"},   std::string_view{"kernel"},
      std::string_view{"platform"},       std::string_view{"probes"},
      std::string_view{"schema_version"}, std::string_view{"source_sha"}};
  auto shape = validate_object_shape(*parsed, fields);
  if (!shape) return std::unexpected(std::move(shape.error()));
  if (!parsed->at("schema_version").is_number_integer() ||
      parsed->at("schema_version") != evidence_schema_version) {
    return failure(EvidenceErrorCode::invalid_schema,
                   "evidence report schema version is unsupported");
  }
  if (!parsed->at("source_sha").is_string() ||
      !parsed->at("platform").is_string() ||
      !parsed->at("kernel").is_string() ||
      !parsed->at("architecture").is_string() ||
      !parsed->at("probes").is_array()) {
    return failure(EvidenceErrorCode::invalid_value,
                   "evidence report field types are invalid");
  }
  try {
    EvidenceReport result{parsed->at("source_sha").get<std::string>(),
                          parsed->at("platform").get<std::string>(),
                          parsed->at("kernel").get<std::string>(),
                          parsed->at("architecture").get<std::string>(),
                          {}};
    const auto& rows = parsed->at("probes");
    if (rows.size() != probe_ids.size()) {
      return failure(EvidenceErrorCode::invalid_value,
                     "evidence report is incomplete");
    }
    result.probes.reserve(rows.size());
    for (const auto& row : rows) {
      auto parsed_record = parse_record_json(row, false);
      if (!parsed_record)
        return std::unexpected(std::move(parsed_record.error()));
      result.probes.push_back(*parsed_record);
    }
    auto valid = validate_report(result);
    if (!valid) return std::unexpected(std::move(valid.error()));
    return result;
  } catch (const Json::exception&) {
    return failure(EvidenceErrorCode::invalid_value,
                   "evidence report field types are invalid");
  } catch (...) {
    return failure(EvidenceErrorCode::resource_exhausted,
                   "evidence report could not be decoded within bounds");
  }
}

auto evidence_run_succeeded(const EvidenceReport& value)
    -> std::expected<bool, EvidenceError> {
  auto valid = validate_report(value);
  if (!valid) return std::unexpected(std::move(valid.error()));
  return std::ranges::none_of(value.probes, [](const ProbeRecord& record) {
    return record.state == ProbeState::probe_error;
  });
}

} // namespace aiforge::evaluation::process_isolation::v2
