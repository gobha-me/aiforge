#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aiforge::evaluation::audio_device {

inline constexpr std::string_view evidence_schema_id =
    "aiforge.audio-device-evaluation";
inline constexpr std::uint32_t evidence_schema_version = 1;
inline constexpr std::size_t maximum_child_report_bytes =
    std::size_t{16} * 1024;
inline constexpr std::size_t maximum_report_bytes = std::size_t{64} * 1024;
inline constexpr std::size_t maximum_platform_metadata_bytes = 128;
inline constexpr std::uint64_t maximum_observed_callbacks = 1'000'000;
inline constexpr std::uint64_t maximum_observed_frames =
    std::uint64_t{32} * 1024 * 1024;
inline constexpr std::uint64_t maximum_observed_xruns = 1'000'000;

enum class ProbeId {
  invalid_format_rejected,
  malformed_wav_rejected,
  oversized_wav_rejected,
  permission_denial_classified,
  device_loss_classified,
  playback_underrun_observed,
  capture_overrun_rejected,
  concurrent_operation_rejected,
  cancel_during_open,
  cancel_during_start,
  cancel_during_stream,
  cancel_during_stop,
  cancel_during_close,
  playback_owner_quiescent,
  capture_bound_enforced,
  partial_capture_not_published,
  late_callback_rejected,
  teardown_quiescent,
  runtime_backend_forced,
  physical_device_access_excluded,
  device_availability_behavior,
  playback_callback_lifecycle,
  capture_callback_lifecycle,
  controller_thread_cancellation,
  callback_quiescent_after_close,
};

enum class Direction { none, playback, capture };
enum class ProbeState { observed, unavailable, probe_error };

enum class ReasonCode {
  none,
  no_device,
  permission_denied,
  unsupported_format,
  candidate_limitation,
  contract_failed,
  prerequisite_unavailable,
  timeout,
  signaled,
  nonzero_exit,
  malformed_protocol,
  output_limit,
  cleanup_failed,
  internal_error,
};

enum class CandidateId { rtaudio, miniaudio };
enum class DependencySource { installed_package, controlled_source_fallback };
enum class Linkage { static_library };
enum class RuntimeBackend { dummy, null_backend };

struct ProbeKey {
  ProbeId probe_id{ProbeId::invalid_format_rejected};
  Direction direction{Direction::none};
  auto operator==(const ProbeKey&) const -> bool = default;
};

struct ProbeRecord {
  ProbeId probe_id{ProbeId::invalid_format_rejected};
  Direction direction{Direction::none};
  ProbeState state{ProbeState::probe_error};
  ReasonCode reason{ReasonCode::internal_error};
  std::uint64_t callbacks{};
  std::uint64_t frames{};
  std::uint64_t xruns{};
  bool cancellation_observed{};
  bool cleanup_complete{};
  auto operator==(const ProbeRecord&) const -> bool = default;
};

struct ContractReport {
  std::vector<ProbeRecord> probes;
  auto operator==(const ContractReport&) const -> bool = default;
};

struct CandidateReport {
  CandidateId candidate_id{CandidateId::rtaudio};
  std::string candidate_version;
  DependencySource dependency_source{
      DependencySource::controlled_source_fallback};
  Linkage linkage{Linkage::static_library};
  RuntimeBackend runtime_backend{RuntimeBackend::dummy};
  bool device_access{};
  bool codec_features{};
  std::vector<ProbeRecord> probes;
  auto operator==(const CandidateReport&) const -> bool = default;
};

struct EvidenceReport {
  std::string source_sha;
  std::string platform;
  std::string architecture;
  ContractReport contract;
  std::vector<CandidateReport> candidates;
  auto operator==(const EvidenceReport&) const -> bool = default;
};

enum class EvidenceErrorCode {
  malformed_json,
  duplicate_field,
  unknown_field,
  missing_field,
  invalid_schema,
  invalid_value,
  resource_exhausted,
};

struct EvidenceError {
  EvidenceErrorCode code{EvidenceErrorCode::invalid_value};
  std::string message;
  auto operator==(const EvidenceError&) const -> bool = default;
};

[[nodiscard]] auto required_contract_probe_keys() -> std::span<const ProbeKey>;
[[nodiscard]] auto required_candidate_probe_keys() -> std::span<const ProbeKey>;
[[nodiscard]] auto required_candidate_ids() -> std::span<const CandidateId>;

[[nodiscard]] auto probe_id_name(ProbeId value) -> std::string_view;
[[nodiscard]] auto direction_name(Direction value) -> std::string_view;
[[nodiscard]] auto probe_state_name(ProbeState value) -> std::string_view;
[[nodiscard]] auto reason_code_name(ReasonCode value) -> std::string_view;
[[nodiscard]] auto candidate_id_name(CandidateId value) -> std::string_view;
[[nodiscard]] auto dependency_source_name(DependencySource value)
    -> std::string_view;
[[nodiscard]] auto linkage_name(Linkage value) -> std::string_view;
[[nodiscard]] auto runtime_backend_name(RuntimeBackend value)
    -> std::string_view;
[[nodiscard]] auto candidate_version(CandidateId value) -> std::string_view;

[[nodiscard]] auto validate_probe_record(const ProbeRecord& value)
    -> std::expected<void, EvidenceError>;
[[nodiscard]] auto validate_contract_report(const ContractReport& value)
    -> std::expected<void, EvidenceError>;
[[nodiscard]] auto validate_candidate_report(const CandidateReport& value)
    -> std::expected<void, EvidenceError>;
[[nodiscard]] auto validate_report(const EvidenceReport& value)
    -> std::expected<void, EvidenceError>;

[[nodiscard]] auto serialize_contract_report(const ContractReport& value)
    -> std::expected<std::string, EvidenceError>;
[[nodiscard]] auto parse_contract_report(std::string_view document)
    -> std::expected<ContractReport, EvidenceError>;
[[nodiscard]] auto serialize_candidate_report(const CandidateReport& value)
    -> std::expected<std::string, EvidenceError>;
[[nodiscard]] auto parse_candidate_report(std::string_view document)
    -> std::expected<CandidateReport, EvidenceError>;
[[nodiscard]] auto serialize_report(const EvidenceReport& value)
    -> std::expected<std::string, EvidenceError>;
[[nodiscard]] auto parse_report(std::string_view document)
    -> std::expected<EvidenceReport, EvidenceError>;
[[nodiscard]] auto evidence_run_succeeded(const EvidenceReport& value)
    -> std::expected<bool, EvidenceError>;

} // namespace aiforge::evaluation::audio_device
