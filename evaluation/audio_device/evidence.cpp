#include "evidence.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>

namespace aiforge::evaluation::audio_device {
namespace {

using Json = nlohmann::json;

constexpr std::array probe_ids{
    ProbeId::invalid_format_rejected,
    ProbeId::malformed_wav_rejected,
    ProbeId::oversized_wav_rejected,
    ProbeId::permission_denial_classified,
    ProbeId::device_loss_classified,
    ProbeId::playback_underrun_observed,
    ProbeId::capture_overrun_rejected,
    ProbeId::concurrent_operation_rejected,
    ProbeId::cancel_during_open,
    ProbeId::cancel_during_start,
    ProbeId::cancel_during_stream,
    ProbeId::cancel_during_stop,
    ProbeId::cancel_during_close,
    ProbeId::playback_owner_quiescent,
    ProbeId::capture_bound_enforced,
    ProbeId::partial_capture_not_published,
    ProbeId::late_callback_rejected,
    ProbeId::teardown_quiescent,
    ProbeId::runtime_backend_forced,
    ProbeId::physical_device_access_excluded,
    ProbeId::device_availability_behavior,
    ProbeId::playback_callback_lifecycle,
    ProbeId::capture_callback_lifecycle,
    ProbeId::controller_thread_cancellation,
    ProbeId::callback_quiescent_after_close,
};

constexpr std::array probe_names{
    std::string_view{"invalid_format_rejected"},
    std::string_view{"malformed_wav_rejected"},
    std::string_view{"oversized_wav_rejected"},
    std::string_view{"permission_denial_classified"},
    std::string_view{"device_loss_classified"},
    std::string_view{"playback_underrun_observed"},
    std::string_view{"capture_overrun_rejected"},
    std::string_view{"concurrent_operation_rejected"},
    std::string_view{"cancel_during_open"},
    std::string_view{"cancel_during_start"},
    std::string_view{"cancel_during_stream"},
    std::string_view{"cancel_during_stop"},
    std::string_view{"cancel_during_close"},
    std::string_view{"playback_owner_quiescent"},
    std::string_view{"capture_bound_enforced"},
    std::string_view{"partial_capture_not_published"},
    std::string_view{"late_callback_rejected"},
    std::string_view{"teardown_quiescent"},
    std::string_view{"runtime_backend_forced"},
    std::string_view{"physical_device_access_excluded"},
    std::string_view{"device_availability_behavior"},
    std::string_view{"playback_callback_lifecycle"},
    std::string_view{"capture_callback_lifecycle"},
    std::string_view{"controller_thread_cancellation"},
    std::string_view{"callback_quiescent_after_close"},
};

constexpr std::array directions{Direction::none, Direction::playback,
                                Direction::capture};
constexpr std::array direction_names{std::string_view{"none"},
                                     std::string_view{"playback"},
                                     std::string_view{"capture"}};

constexpr std::array probe_states{ProbeState::observed, ProbeState::unavailable,
                                  ProbeState::probe_error};
constexpr std::array probe_state_names{std::string_view{"observed"},
                                       std::string_view{"unavailable"},
                                       std::string_view{"probe_error"}};

constexpr std::array reason_codes{
    ReasonCode::none,
    ReasonCode::no_device,
    ReasonCode::permission_denied,
    ReasonCode::unsupported_format,
    ReasonCode::candidate_limitation,
    ReasonCode::contract_failed,
    ReasonCode::prerequisite_unavailable,
    ReasonCode::timeout,
    ReasonCode::signaled,
    ReasonCode::nonzero_exit,
    ReasonCode::malformed_protocol,
    ReasonCode::output_limit,
    ReasonCode::cleanup_failed,
    ReasonCode::internal_error,
};
constexpr std::array reason_names{
    std::string_view{"none"},
    std::string_view{"no_device"},
    std::string_view{"permission_denied"},
    std::string_view{"unsupported_format"},
    std::string_view{"candidate_limitation"},
    std::string_view{"contract_failed"},
    std::string_view{"prerequisite_unavailable"},
    std::string_view{"timeout"},
    std::string_view{"signaled"},
    std::string_view{"nonzero_exit"},
    std::string_view{"malformed_protocol"},
    std::string_view{"output_limit"},
    std::string_view{"cleanup_failed"},
    std::string_view{"internal_error"},
};

constexpr std::array candidate_ids{CandidateId::rtaudio,
                                   CandidateId::miniaudio};
constexpr std::array candidate_names{std::string_view{"rtaudio"},
                                     std::string_view{"miniaudio"}};
constexpr std::array dependency_sources{
    DependencySource::installed_package,
    DependencySource::controlled_source_fallback};
constexpr std::array dependency_source_names{
    std::string_view{"installed_package"},
    std::string_view{"controlled_source_fallback"}};
constexpr std::array linkages{Linkage::static_library};
constexpr std::array linkage_names{std::string_view{"static"}};
constexpr std::array runtime_backends{RuntimeBackend::dummy,
                                      RuntimeBackend::null_backend};
constexpr std::array runtime_backend_names{std::string_view{"dummy"},
                                           std::string_view{"null"}};

constexpr std::array contract_probe_keys{
    ProbeKey{ProbeId::invalid_format_rejected, Direction::none},
    ProbeKey{ProbeId::malformed_wav_rejected, Direction::playback},
    ProbeKey{ProbeId::oversized_wav_rejected, Direction::playback},
    ProbeKey{ProbeId::permission_denial_classified, Direction::capture},
    ProbeKey{ProbeId::device_loss_classified, Direction::playback},
    ProbeKey{ProbeId::device_loss_classified, Direction::capture},
    ProbeKey{ProbeId::playback_underrun_observed, Direction::playback},
    ProbeKey{ProbeId::capture_overrun_rejected, Direction::capture},
    ProbeKey{ProbeId::concurrent_operation_rejected, Direction::none},
    ProbeKey{ProbeId::cancel_during_open, Direction::playback},
    ProbeKey{ProbeId::cancel_during_open, Direction::capture},
    ProbeKey{ProbeId::cancel_during_start, Direction::playback},
    ProbeKey{ProbeId::cancel_during_start, Direction::capture},
    ProbeKey{ProbeId::cancel_during_stream, Direction::playback},
    ProbeKey{ProbeId::cancel_during_stream, Direction::capture},
    ProbeKey{ProbeId::cancel_during_stop, Direction::playback},
    ProbeKey{ProbeId::cancel_during_stop, Direction::capture},
    ProbeKey{ProbeId::cancel_during_close, Direction::playback},
    ProbeKey{ProbeId::cancel_during_close, Direction::capture},
    ProbeKey{ProbeId::playback_owner_quiescent, Direction::playback},
    ProbeKey{ProbeId::capture_bound_enforced, Direction::capture},
    ProbeKey{ProbeId::partial_capture_not_published, Direction::capture},
    ProbeKey{ProbeId::late_callback_rejected, Direction::none},
    ProbeKey{ProbeId::teardown_quiescent, Direction::none},
};

constexpr std::array candidate_probe_keys{
    ProbeKey{ProbeId::runtime_backend_forced, Direction::none},
    ProbeKey{ProbeId::physical_device_access_excluded, Direction::none},
    ProbeKey{ProbeId::device_availability_behavior, Direction::playback},
    ProbeKey{ProbeId::device_availability_behavior, Direction::capture},
    ProbeKey{ProbeId::playback_callback_lifecycle, Direction::playback},
    ProbeKey{ProbeId::capture_callback_lifecycle, Direction::capture},
    ProbeKey{ProbeId::controller_thread_cancellation, Direction::playback},
    ProbeKey{ProbeId::controller_thread_cancellation, Direction::capture},
    ProbeKey{ProbeId::callback_quiescent_after_close, Direction::playback},
    ProbeKey{ProbeId::callback_quiescent_after_close, Direction::capture},
};

static_assert(probe_ids.size() == probe_names.size());
static_assert(directions.size() == direction_names.size());
static_assert(probe_states.size() == probe_state_names.size());
static_assert(reason_codes.size() == reason_names.size());
static_assert(candidate_ids.size() == candidate_names.size());
static_assert(dependency_sources.size() == dependency_source_names.size());
static_assert(linkages.size() == linkage_names.size());
static_assert(runtime_backends.size() == runtime_backend_names.size());

class DuplicateJsonField final : public std::exception {
 public:
  [[nodiscard]] auto what() const noexcept -> const char* override {
    return "duplicate JSON field";
  }
};

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
  if (document.empty())
    return failure(EvidenceErrorCode::malformed_json,
                   "audio-device evidence JSON is empty");
  if (document.size() > maximum_bytes)
    return failure(EvidenceErrorCode::resource_exhausted,
                   "audio-device evidence JSON exceeds its byte limit");
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
        if (!object_fields.empty()) object_fields.pop_back();
      }
      return true;
    };
    return Json::parse(document.begin(), document.end(), callback, true, false);
  } catch (const DuplicateJsonField&) {
    return failure(EvidenceErrorCode::duplicate_field,
                   "audio-device evidence JSON contains a duplicate field");
  } catch (const Json::exception&) {
    return failure(EvidenceErrorCode::malformed_json,
                   "audio-device evidence JSON is malformed");
  } catch (const std::exception&) {
    return failure(
        EvidenceErrorCode::resource_exhausted,
        "audio-device evidence JSON could not be parsed within bounds");
  }
}

template <std::size_t Size>
[[nodiscard]] auto validate_object_shape(
    const Json& value, const std::array<std::string_view, Size>& fields)
    -> std::expected<void, EvidenceError> {
  if (!value.is_object())
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device evidence value must be an object");
  for (const auto& [name, ignored] : value.items()) {
    static_cast<void>(ignored);
    if (std::ranges::find(fields, name) == fields.end())
      return failure(EvidenceErrorCode::unknown_field,
                     "audio-device evidence contains an unknown field");
  }
  for (const auto field : fields) {
    if (!value.contains(field))
      return failure(EvidenceErrorCode::missing_field,
                     "audio-device evidence is missing a required field");
  }
  return {};
}

[[nodiscard]] auto valid_schema(const Json& value) -> bool {
  return value.is_number_integer() && value == evidence_schema_version;
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

[[nodiscard]] auto valid_reason_for_state(const ProbeState state,
                                          const ReasonCode reason) -> bool {
  switch (reason) {
    case ReasonCode::none: return state == ProbeState::observed;
    case ReasonCode::no_device:
    case ReasonCode::permission_denied:
    case ReasonCode::unsupported_format:
    case ReasonCode::candidate_limitation:
    case ReasonCode::contract_failed:
    case ReasonCode::prerequisite_unavailable:
      return state == ProbeState::unavailable;
    case ReasonCode::timeout:
    case ReasonCode::signaled:
    case ReasonCode::nonzero_exit:
    case ReasonCode::malformed_protocol:
    case ReasonCode::output_limit:
    case ReasonCode::cleanup_failed:
    case ReasonCode::internal_error: return state == ProbeState::probe_error;
  }
  return false;
}

[[nodiscard]] auto cancellation_probe(const ProbeId probe_id) -> bool {
  return probe_id == ProbeId::cancel_during_open ||
         probe_id == ProbeId::cancel_during_start ||
         probe_id == ProbeId::cancel_during_stream ||
         probe_id == ProbeId::cancel_during_stop ||
         probe_id == ProbeId::cancel_during_close ||
         probe_id == ProbeId::controller_thread_cancellation;
}

[[nodiscard]] auto record_json(const ProbeRecord& value) -> Json {
  return {{"callbacks", value.callbacks},
          {"cancellation_observed", value.cancellation_observed},
          {"cleanup_complete", value.cleanup_complete},
          {"direction", direction_name(value.direction)},
          {"frames", value.frames},
          {"probe_id", probe_id_name(value.probe_id)},
          {"reason", reason_code_name(value.reason)},
          {"state", probe_state_name(value.state)},
          {"xruns", value.xruns}};
}

[[nodiscard]] auto parse_record_json(const Json& value)
    -> std::expected<ProbeRecord, EvidenceError> {
  constexpr std::array fields{
      std::string_view{"callbacks"},
      std::string_view{"cancellation_observed"},
      std::string_view{"cleanup_complete"},
      std::string_view{"direction"},
      std::string_view{"frames"},
      std::string_view{"probe_id"},
      std::string_view{"reason"},
      std::string_view{"state"},
      std::string_view{"xruns"},
  };
  auto shape = validate_object_shape(value, fields);
  if (!shape) return std::unexpected(std::move(shape.error()));
  const auto probe = parse_enum(value.at("probe_id"), probe_ids, probe_names);
  const auto direction =
      parse_enum(value.at("direction"), directions, direction_names);
  const auto state =
      parse_enum(value.at("state"), probe_states, probe_state_names);
  const auto reason =
      parse_enum(value.at("reason"), reason_codes, reason_names);
  if (!probe || !direction || !state || !reason ||
      !value.at("callbacks").is_number_unsigned() ||
      !value.at("frames").is_number_unsigned() ||
      !value.at("xruns").is_number_unsigned() ||
      !value.at("cancellation_observed").is_boolean() ||
      !value.at("cleanup_complete").is_boolean()) {
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device probe contains an invalid closed value");
  }
  ProbeRecord result{*probe,
                     *direction,
                     *state,
                     *reason,
                     value.at("callbacks").get<std::uint64_t>(),
                     value.at("frames").get<std::uint64_t>(),
                     value.at("xruns").get<std::uint64_t>(),
                     value.at("cancellation_observed").get<bool>(),
                     value.at("cleanup_complete").get<bool>()};
  auto valid = validate_probe_record(result);
  if (!valid) return std::unexpected(std::move(valid.error()));
  return result;
}

[[nodiscard]] auto probes_json(const std::vector<ProbeRecord>& probes) -> Json {
  auto rows = Json::array();
  for (const auto& probe : probes)
    rows.push_back(record_json(probe));
  return rows;
}

[[nodiscard]] auto parse_probes(const Json& value)
    -> std::expected<std::vector<ProbeRecord>, EvidenceError> {
  if (!value.is_array())
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device probes must be an array");
  std::vector<ProbeRecord> result;
  result.reserve(value.size());
  for (const auto& row : value) {
    auto parsed = parse_record_json(row);
    if (!parsed) return std::unexpected(std::move(parsed.error()));
    result.push_back(*parsed);
  }
  return result;
}

[[nodiscard]] auto candidate_json(const CandidateReport& value) -> Json {
  return {
      {"candidate_id", candidate_id_name(value.candidate_id)},
      {"candidate_version", value.candidate_version},
      {"codec_features", value.codec_features},
      {"dependency_source", dependency_source_name(value.dependency_source)},
      {"device_access", value.device_access},
      {"linkage", linkage_name(value.linkage)},
      {"probes", probes_json(value.probes)},
      {"runtime_backend", runtime_backend_name(value.runtime_backend)}};
}

[[nodiscard]] auto parse_candidate_json(const Json& value)
    -> std::expected<CandidateReport, EvidenceError> {
  constexpr std::array fields{
      std::string_view{"candidate_id"},   std::string_view{"candidate_version"},
      std::string_view{"codec_features"}, std::string_view{"dependency_source"},
      std::string_view{"device_access"},  std::string_view{"linkage"},
      std::string_view{"probes"},         std::string_view{"runtime_backend"},
  };
  auto shape = validate_object_shape(value, fields);
  if (!shape) return std::unexpected(std::move(shape.error()));
  const auto candidate =
      parse_enum(value.at("candidate_id"), candidate_ids, candidate_names);
  const auto source = parse_enum(value.at("dependency_source"),
                                 dependency_sources, dependency_source_names);
  const auto linkage = parse_enum(value.at("linkage"), linkages, linkage_names);
  const auto backend = parse_enum(value.at("runtime_backend"), runtime_backends,
                                  runtime_backend_names);
  if (!candidate || !source || !linkage || !backend ||
      !value.at("candidate_version").is_string() ||
      !value.at("device_access").is_boolean() ||
      !value.at("codec_features").is_boolean()) {
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device candidate contains an invalid closed value");
  }
  auto probes = parse_probes(value.at("probes"));
  if (!probes) return std::unexpected(std::move(probes.error()));
  CandidateReport result{
      *candidate,
      value.at("candidate_version").get<std::string>(),
      *source,
      *linkage,
      *backend,
      value.at("device_access").get<bool>(),
      value.at("codec_features").get<bool>(),
      std::move(*probes),
  };
  auto valid = validate_candidate_report(result);
  if (!valid) return std::unexpected(std::move(valid.error()));
  return result;
}

[[nodiscard]] auto append_newline(std::string value) -> std::string {
  value.push_back('\n');
  return value;
}

template <typename Report, typename Validator, typename Encoder>
[[nodiscard]] auto serialize_bounded(const Report& value,
                                     const std::size_t maximum_bytes,
                                     const Validator& validator,
                                     const Encoder& encoder)
    -> std::expected<std::string, EvidenceError> {
  auto valid = validator(value);
  if (!valid) return std::unexpected(std::move(valid.error()));
  try {
    auto document = append_newline(encoder(value).dump());
    if (document.size() > maximum_bytes)
      return failure(EvidenceErrorCode::resource_exhausted,
                     "audio-device evidence exceeds its byte limit");
    return document;
  } catch (const std::exception&) {
    return failure(EvidenceErrorCode::resource_exhausted,
                   "audio-device evidence could not be serialized");
  }
}

[[nodiscard]] auto validate_probe_order(
    const std::vector<ProbeRecord>& probes,
    const std::span<const ProbeKey> required)
    -> std::expected<void, EvidenceError> {
  if (probes.size() != required.size())
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device evidence probe set is incomplete");
  for (std::size_t index{}; index < required.size(); ++index) {
    if (ProbeKey{probes[index].probe_id, probes[index].direction} !=
        required[index]) {
      return failure(EvidenceErrorCode::invalid_value,
                     "audio-device evidence probe order is invalid");
    }
    auto valid = validate_probe_record(probes[index]);
    if (!valid) return std::unexpected(std::move(valid.error()));
  }
  return {};
}

} // namespace

auto required_contract_probe_keys() -> std::span<const ProbeKey> {
  return contract_probe_keys;
}

auto required_candidate_probe_keys() -> std::span<const ProbeKey> {
  return candidate_probe_keys;
}

auto required_candidate_ids() -> std::span<const CandidateId> {
  return candidate_ids;
}

auto probe_id_name(const ProbeId value) -> std::string_view {
  return enum_name(value, probe_ids, probe_names);
}

auto direction_name(const Direction value) -> std::string_view {
  return enum_name(value, directions, direction_names);
}

auto probe_state_name(const ProbeState value) -> std::string_view {
  return enum_name(value, probe_states, probe_state_names);
}

auto reason_code_name(const ReasonCode value) -> std::string_view {
  return enum_name(value, reason_codes, reason_names);
}

auto candidate_id_name(const CandidateId value) -> std::string_view {
  return enum_name(value, candidate_ids, candidate_names);
}

auto dependency_source_name(const DependencySource value) -> std::string_view {
  return enum_name(value, dependency_sources, dependency_source_names);
}

auto linkage_name(const Linkage value) -> std::string_view {
  return enum_name(value, linkages, linkage_names);
}

auto runtime_backend_name(const RuntimeBackend value) -> std::string_view {
  return enum_name(value, runtime_backends, runtime_backend_names);
}

auto candidate_version(const CandidateId value) -> std::string_view {
  switch (value) {
    case CandidateId::rtaudio: return "6.0.1";
    case CandidateId::miniaudio: return "0.11.25";
  }
  return {};
}

auto validate_probe_record(const ProbeRecord& value)
    -> std::expected<void, EvidenceError> {
  if (probe_id_name(value.probe_id).empty() ||
      direction_name(value.direction).empty() ||
      probe_state_name(value.state).empty() ||
      reason_code_name(value.reason).empty() ||
      !valid_reason_for_state(value.state, value.reason)) {
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device probe contains invalid state");
  }
  if (value.callbacks > maximum_observed_callbacks ||
      value.frames > maximum_observed_frames ||
      value.xruns > maximum_observed_xruns) {
    return failure(EvidenceErrorCode::resource_exhausted,
                   "audio-device probe counters exceed their limits");
  }
  if (value.cancellation_observed && !cancellation_probe(value.probe_id))
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device cancellation evidence is misplaced");
  if (value.state == ProbeState::observed &&
      cancellation_probe(value.probe_id) && !value.cancellation_observed)
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device cancellation observation is missing");
  if (!value.cleanup_complete && (value.state != ProbeState::probe_error ||
                                  value.reason != ReasonCode::cleanup_failed)) {
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device cleanup state is inconsistent");
  }
  if (value.reason == ReasonCode::cleanup_failed && value.cleanup_complete)
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device cleanup failure is inconsistent");
  return {};
}

auto validate_contract_report(const ContractReport& value)
    -> std::expected<void, EvidenceError> {
  return validate_probe_order(value.probes, contract_probe_keys);
}

auto validate_candidate_report(const CandidateReport& value)
    -> std::expected<void, EvidenceError> {
  if (candidate_id_name(value.candidate_id).empty() ||
      value.candidate_version != candidate_version(value.candidate_id) ||
      dependency_source_name(value.dependency_source).empty() ||
      linkage_name(value.linkage).empty() ||
      runtime_backend_name(value.runtime_backend).empty() ||
      value.device_access || value.codec_features) {
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device candidate metadata is invalid");
  }
  const auto expected_backend = value.candidate_id == CandidateId::rtaudio
                                    ? RuntimeBackend::dummy
                                    : RuntimeBackend::null_backend;
  if (value.runtime_backend != expected_backend)
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device candidate backend identity is invalid");
  return validate_probe_order(value.probes, candidate_probe_keys);
}

auto validate_report(const EvidenceReport& value)
    -> std::expected<void, EvidenceError> {
  if (!valid_source_sha(value.source_sha) || value.platform != "linux" ||
      !valid_platform_metadata(value.architecture)) {
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device evidence provenance is invalid");
  }
  auto contract = validate_contract_report(value.contract);
  if (!contract) return std::unexpected(std::move(contract.error()));
  if (value.candidates.size() != candidate_ids.size())
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device candidate set is incomplete");
  for (std::size_t index{}; index < candidate_ids.size(); ++index) {
    if (value.candidates[index].candidate_id != candidate_ids[index])
      return failure(EvidenceErrorCode::invalid_value,
                     "audio-device candidate order is invalid");
    auto valid = validate_candidate_report(value.candidates[index]);
    if (!valid) return std::unexpected(std::move(valid.error()));
  }
  return {};
}

auto serialize_contract_report(const ContractReport& value)
    -> std::expected<std::string, EvidenceError> {
  return serialize_bounded(
      value, maximum_child_report_bytes, validate_contract_report,
      [](const ContractReport& report) {
        return Json{{"kind", "contract"},
                    {"probes", probes_json(report.probes)},
                    {"schema_id", evidence_schema_id},
                    {"schema_version", evidence_schema_version}};
      });
}

auto parse_contract_report(const std::string_view document)
    -> std::expected<ContractReport, EvidenceError> {
  auto parsed = parse_json(document, maximum_child_report_bytes);
  if (!parsed) return std::unexpected(std::move(parsed.error()));
  constexpr std::array fields{
      std::string_view{"kind"}, std::string_view{"probes"},
      std::string_view{"schema_id"}, std::string_view{"schema_version"}};
  auto shape = validate_object_shape(*parsed, fields);
  if (!shape) return std::unexpected(std::move(shape.error()));
  if (parsed->at("schema_id") != evidence_schema_id ||
      !valid_schema(parsed->at("schema_version")))
    return failure(EvidenceErrorCode::invalid_schema,
                   "audio-device child schema is unsupported");
  if (!parsed->at("kind").is_string() || parsed->at("kind") != "contract")
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device child kind is invalid");
  auto probes = parse_probes(parsed->at("probes"));
  if (!probes) return std::unexpected(std::move(probes.error()));
  ContractReport result{std::move(*probes)};
  auto valid = validate_contract_report(result);
  if (!valid) return std::unexpected(std::move(valid.error()));
  return result;
}

auto serialize_candidate_report(const CandidateReport& value)
    -> std::expected<std::string, EvidenceError> {
  return serialize_bounded(value, maximum_child_report_bytes,
                           validate_candidate_report,
                           [](const CandidateReport& report) {
                             auto result = candidate_json(report);
                             result["kind"] = "candidate";
                             result["schema_id"] = evidence_schema_id;
                             result["schema_version"] = evidence_schema_version;
                             return result;
                           });
}

auto parse_candidate_report(const std::string_view document)
    -> std::expected<CandidateReport, EvidenceError> {
  auto parsed = parse_json(document, maximum_child_report_bytes);
  if (!parsed) return std::unexpected(std::move(parsed.error()));
  constexpr std::array fields{
      std::string_view{"candidate_id"},
      std::string_view{"candidate_version"},
      std::string_view{"codec_features"},
      std::string_view{"dependency_source"},
      std::string_view{"device_access"},
      std::string_view{"kind"},
      std::string_view{"linkage"},
      std::string_view{"probes"},
      std::string_view{"runtime_backend"},
      std::string_view{"schema_id"},
      std::string_view{"schema_version"},
  };
  auto shape = validate_object_shape(*parsed, fields);
  if (!shape) return std::unexpected(std::move(shape.error()));
  if (parsed->at("schema_id") != evidence_schema_id ||
      !valid_schema(parsed->at("schema_version")))
    return failure(EvidenceErrorCode::invalid_schema,
                   "audio-device child schema is unsupported");
  if (!parsed->at("kind").is_string() || parsed->at("kind") != "candidate")
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device child kind is invalid");
  parsed->erase("kind");
  parsed->erase("schema_id");
  parsed->erase("schema_version");
  return parse_candidate_json(*parsed);
}

auto serialize_report(const EvidenceReport& value)
    -> std::expected<std::string, EvidenceError> {
  return serialize_bounded(
      value, maximum_report_bytes, validate_report,
      [](const EvidenceReport& report) {
        auto candidates = Json::array();
        for (const auto& candidate : report.candidates)
          candidates.push_back(candidate_json(candidate));
        return Json{{"architecture", report.architecture},
                    {"candidates", std::move(candidates)},
                    {"contract_probes", probes_json(report.contract.probes)},
                    {"platform", report.platform},
                    {"schema_id", evidence_schema_id},
                    {"schema_version", evidence_schema_version},
                    {"source_sha", report.source_sha}};
      });
}

auto parse_report(const std::string_view document)
    -> std::expected<EvidenceReport, EvidenceError> {
  auto parsed = parse_json(document, maximum_report_bytes);
  if (!parsed) return std::unexpected(std::move(parsed.error()));
  constexpr std::array fields{
      std::string_view{"architecture"},    std::string_view{"candidates"},
      std::string_view{"contract_probes"}, std::string_view{"platform"},
      std::string_view{"schema_id"},       std::string_view{"schema_version"},
      std::string_view{"source_sha"},
  };
  auto shape = validate_object_shape(*parsed, fields);
  if (!shape) return std::unexpected(std::move(shape.error()));
  if (parsed->at("schema_id") != evidence_schema_id ||
      !valid_schema(parsed->at("schema_version")))
    return failure(EvidenceErrorCode::invalid_schema,
                   "audio-device report schema is unsupported");
  if (!parsed->at("source_sha").is_string() ||
      !parsed->at("platform").is_string() ||
      !parsed->at("architecture").is_string() ||
      !parsed->at("candidates").is_array())
    return failure(EvidenceErrorCode::invalid_value,
                   "audio-device report field types are invalid");
  auto contract_probes = parse_probes(parsed->at("contract_probes"));
  if (!contract_probes)
    return std::unexpected(std::move(contract_probes.error()));
  std::vector<CandidateReport> candidates;
  candidates.reserve(parsed->at("candidates").size());
  for (const auto& row : parsed->at("candidates")) {
    auto candidate = parse_candidate_json(row);
    if (!candidate) return std::unexpected(std::move(candidate.error()));
    candidates.push_back(std::move(*candidate));
  }
  EvidenceReport result{parsed->at("source_sha").get<std::string>(),
                        parsed->at("platform").get<std::string>(),
                        parsed->at("architecture").get<std::string>(),
                        ContractReport{std::move(*contract_probes)},
                        std::move(candidates)};
  auto valid = validate_report(result);
  if (!valid) return std::unexpected(std::move(valid.error()));
  return result;
}

auto evidence_run_succeeded(const EvidenceReport& value)
    -> std::expected<bool, EvidenceError> {
  auto valid = validate_report(value);
  if (!valid) return std::unexpected(std::move(valid.error()));
  const auto failed = [](const ProbeRecord& row) {
    return row.state == ProbeState::probe_error;
  };
  if (std::ranges::any_of(value.contract.probes, [](const ProbeRecord& row) {
        return row.state != ProbeState::observed;
      }))
    return false;
  for (const auto& candidate : value.candidates) {
    const auto required_observed = [&](const ProbeId probe_id) {
      const auto row =
          std::ranges::find(candidate.probes, probe_id, &ProbeRecord::probe_id);
      return row != candidate.probes.end() &&
             row->state == ProbeState::observed;
    };
    if (!required_observed(ProbeId::runtime_backend_forced) ||
        !required_observed(ProbeId::physical_device_access_excluded))
      return false;
  }
  return std::ranges::none_of(value.candidates, [&](const auto& candidate) {
    return std::ranges::any_of(candidate.probes, failed);
  });
}

} // namespace aiforge::evaluation::audio_device
