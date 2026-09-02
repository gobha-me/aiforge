#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/domain/content.hpp>

namespace aiforge::domain {

// Mirrors the configuration layer's precedence sources without a domain
// dependency on `aiforge::config`.
enum class ProvenanceSource {
  command_line,
  environment,
  file,
  compiled_default,
};

enum class ProvenanceDisposition {
  selected,
  shadowed,
  rejected,
};

// A closed set keeps the persisted encoding strict. Free diagnostic text never
// enters an event.
enum class ProvenanceDiagnosticCode {
  invalid_registry,
  duplicate_key,
  duplicate_environment_binding,
  unknown_key,
  invalid_value,
  value_too_large,
  too_many_values,
  sensitive_value,
  duplicate_source_value,
  source_warning,
};

struct ProvenanceDecision {
  ProvenanceSource source{ProvenanceSource::compiled_default};
  ProvenanceDisposition disposition{ProvenanceDisposition::rejected};
  std::optional<ProvenanceDiagnosticCode> diagnostic_code;
  auto operator==(const ProvenanceDecision&) const -> bool = default;
};

struct ConfigurationProvenanceEntry {
  std::string key;
  // Engaged only when the key is not sensitive. A sensitive key records
  // presence and source; its resolved value never enters an event.
  std::optional<std::string> value;
  // True when a value resolved, whether or not that value is recordable.
  bool value_present{};
  std::optional<ProvenanceSource> source;
  bool sensitive{};
  std::vector<ProvenanceDecision> decisions;
  auto operator==(const ConfigurationProvenanceEntry&) const -> bool = default;
};

// Credential sources are owned by a later milestone. This is the minimal
// forward-compatible reference: where a credential came from, never any part of
// the credential itself.
enum class CredentialSourceKind {
  environment,
  configuration_file,
  unrecognized,
};

struct CredentialSourceReference {
  CredentialSourceKind kind{CredentialSourceKind::environment};
  // A non-secret locator such as an environment variable name. Credential
  // bytes, fragments, and lengths are forbidden.
  std::string identity;
  auto operator==(const CredentialSourceReference&) const -> bool = default;
};

struct RuntimeComponentVersion {
  std::string component;
  std::string version;
  auto operator==(const RuntimeComponentVersion&) const -> bool = default;
};

// Tool declarations carry no version, so declaration identity is the tool name
// with the effects and capability scopes it declared for the run.
struct ToolProvenanceEntry {
  std::string tool_name;
  std::vector<Effect> declared_effects;
  std::vector<CapabilityScope> capability_scopes;
  auto operator==(const ToolProvenanceEntry&) const -> bool = default;
};

// Effective request options distinguish provider defaults from values selected
// by durable configuration or the active interactive session. This is a
// deliberately closed domain enum rather than a configuration-layer type so
// persisted run truth cannot acquire unknown authority semantics silently.
enum class RequestOptionSource {
  provider_default,
  configuration,
  session_override,
};

struct EffectiveRequestOption {
  std::string key;
  // Provider defaults carry no asserted value. Configuration and session
  // overrides record the effective, non-secret value sent for the run.
  std::optional<std::string> value;
  RequestOptionSource source{RequestOptionSource::provider_default};
  auto operator==(const EffectiveRequestOption&) const -> bool = default;
};

struct RunProvenance {
  std::string aiforge_version;
  std::string backend_id;
  std::optional<std::string> backend_version;
  ModelId model_id;
  std::optional<CredentialSourceReference> credential_source;
  std::vector<ConfigurationProvenanceEntry> configuration;
  std::vector<RuntimeComponentVersion> components;
  // Owned by the run kernel, which fills it from the run's tool registry
  // snapshot. A caller submits this empty.
  std::vector<ToolProvenanceEntry> tools;
  std::vector<EffectiveRequestOption> effective_request_options{};
  auto operator==(const RunProvenance&) const -> bool = default;
};

struct RunProvenanceLimits {
  std::size_t maximum_configuration_entries{256};
  std::size_t maximum_decisions_per_entry{16};
  std::size_t maximum_key_bytes{128};
  std::size_t maximum_value_bytes{std::size_t{4} * 1024U};
  std::size_t maximum_identity_bytes{256};
  std::size_t maximum_components{64};
  std::size_t maximum_tools{256};
  std::size_t maximum_effects_per_tool{16};
  std::size_t maximum_scopes_per_tool{64};
  std::size_t maximum_request_options{64};
  std::size_t maximum_request_option_key_bytes{128};
  std::size_t maximum_request_option_value_bytes{std::size_t{4} * 1024U};
  std::size_t maximum_total_bytes{std::size_t{64} * 1024U};
  auto operator==(const RunProvenanceLimits&) const -> bool = default;
};

enum class RunProvenanceErrorCode {
  invalid_limits,
  invalid_identity,
  invalid_key,
  duplicate_key,
  sensitive_value_recorded,
  value_too_large,
  too_many_entries,
  invalid_credential_source,
  invalid_component,
  invalid_tool,
  duplicate_tool,
  invalid_request_option,
  resource_exhausted,
};

struct RunProvenanceError {
  RunProvenanceErrorCode code{RunProvenanceErrorCode::invalid_identity};
  // Names shape and key identity only. A resolved configuration value never
  // reaches a message.
  std::string message;
  auto operator==(const RunProvenanceError&) const -> bool = default;
};

[[nodiscard]] auto validate_run_provenance(const RunProvenance& provenance,
                                           RunProvenanceLimits limits = {})
    -> std::expected<void, RunProvenanceError>;

} // namespace aiforge::domain
