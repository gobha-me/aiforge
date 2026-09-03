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

// The readable authority identity remains name, effects, and scopes. A
// registration digest additionally binds the exact declaration, limits, and
// versioned executor contract when durable recovery may be required.
struct ToolProvenanceEntry {
  std::string tool_name;
  std::vector<Effect> declared_effects;
  std::vector<CapabilityScope> capability_scopes;
  // Present for newly recorded versioned registrations. Legacy completed runs
  // may omit it; a pending durable run may not resume without it.
  std::optional<std::string> registration_digest{};
  auto operator==(const ToolProvenanceEntry&) const -> bool = default;
};

// Named profile references explain which independently selected and associated
// upper bounds produced a run's exact kernel-owned tool snapshot. Associations
// are maxima only; authority and declarations remain separate runtime facts.
struct ToolProfileProvenance {
  ToolProfileId selected_profile_id;
  std::optional<ToolProfileId> model_maximum_profile_id;
  std::optional<ToolProfileId> persona_maximum_profile_id;
  auto operator==(const ToolProfileProvenance&) const -> bool = default;
};

// Launch policy is durable authority metadata, not presentation state. These
// closed values intentionally mirror the user-visible launch controls without
// depending on the runtime policy implementation.
enum class ToolRestrictionLevel {
  high,
  medium,
  low,
  none,
};

enum class ToolApprovalMode {
  prompt,
  automatic,
  allow_all,
};

// The identity versions the interpretation of the closed fields. Tool
// registration provenance separately binds the exact per-run declarations;
// this record binds the application-lifetime ceiling that constrained them.
struct ToolPolicyProvenance {
  std::string identity;
  PermissionProfileId permission_profile_id;
  ToolRestrictionLevel restriction_level{ToolRestrictionLevel::high};
  ToolApprovalMode approval_mode{ToolApprovalMode::prompt};
  std::vector<Effect> effect_ceiling;
  std::vector<CapabilityScope> capability_ceiling;
  std::vector<std::string> automatically_eligible_tools;
  auto operator==(const ToolPolicyProvenance&) const -> bool = default;
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
  // Tool registration and launch-policy provenance are owned by the run
  // kernel. A caller submits both empty.
  std::vector<ToolProvenanceEntry> tools;
  std::vector<EffectiveRequestOption> effective_request_options{};
  std::optional<ToolProfileProvenance> tool_profile{};
  std::optional<ToolPolicyProvenance> tool_policy{};
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
  std::size_t maximum_policy_effects{16};
  std::size_t maximum_policy_scopes{1024};
  std::size_t maximum_automatic_tools{256};
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
  invalid_tool_profile,
  invalid_tool_policy,
};

struct RunProvenanceError {
  RunProvenanceErrorCode code{RunProvenanceErrorCode::invalid_identity};
  // Names shape and key identity only. A resolved configuration value never
  // reaches a message.
  std::string message;
  auto operator==(const RunProvenanceError&) const -> bool = default;
};

[[nodiscard]] auto validate_tool_policy_provenance(
    const ToolPolicyProvenance& provenance, RunProvenanceLimits limits = {})
    -> std::expected<void, RunProvenanceError>;

[[nodiscard]] auto validate_run_provenance(const RunProvenance& provenance,
                                           RunProvenanceLimits limits = {})
    -> std::expected<void, RunProvenanceError>;

} // namespace aiforge::domain
