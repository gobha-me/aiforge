#include <aiforge/config/provenance.hpp>

#include <utility>

namespace aiforge::config {

// Every mapping below is an exhaustive switch without a default, so adding a
// configuration enumerator becomes a compile error rather than a silent
// mis-mapping of persisted provenance.

auto provenance_source(const ConfigSource source) -> domain::ProvenanceSource {
  switch (source) {
    case ConfigSource::command_line:
      return domain::ProvenanceSource::command_line;
    case ConfigSource::environment:
      return domain::ProvenanceSource::environment;
    case ConfigSource::file: return domain::ProvenanceSource::file;
    case ConfigSource::compiled_default:
      return domain::ProvenanceSource::compiled_default;
  }
  return domain::ProvenanceSource::compiled_default;
}

auto provenance_disposition(const CandidateDisposition disposition)
    -> domain::ProvenanceDisposition {
  switch (disposition) {
    case CandidateDisposition::selected:
      return domain::ProvenanceDisposition::selected;
    case CandidateDisposition::shadowed:
      return domain::ProvenanceDisposition::shadowed;
    case CandidateDisposition::rejected:
      return domain::ProvenanceDisposition::rejected;
  }
  return domain::ProvenanceDisposition::rejected;
}

auto provenance_diagnostic_code(const ConfigDiagnosticCode code)
    -> domain::ProvenanceDiagnosticCode {
  switch (code) {
    case ConfigDiagnosticCode::invalid_registry:
      return domain::ProvenanceDiagnosticCode::invalid_registry;
    case ConfigDiagnosticCode::duplicate_key:
      return domain::ProvenanceDiagnosticCode::duplicate_key;
    case ConfigDiagnosticCode::duplicate_environment_binding:
      return domain::ProvenanceDiagnosticCode::duplicate_environment_binding;
    case ConfigDiagnosticCode::unknown_key:
      return domain::ProvenanceDiagnosticCode::unknown_key;
    case ConfigDiagnosticCode::invalid_value:
      return domain::ProvenanceDiagnosticCode::invalid_value;
    case ConfigDiagnosticCode::value_too_large:
      return domain::ProvenanceDiagnosticCode::value_too_large;
    case ConfigDiagnosticCode::too_many_values:
      return domain::ProvenanceDiagnosticCode::too_many_values;
    case ConfigDiagnosticCode::sensitive_value:
      return domain::ProvenanceDiagnosticCode::sensitive_value;
    case ConfigDiagnosticCode::duplicate_source_value:
      return domain::ProvenanceDiagnosticCode::duplicate_source_value;
    case ConfigDiagnosticCode::source_warning:
      return domain::ProvenanceDiagnosticCode::source_warning;
  }
  return domain::ProvenanceDiagnosticCode::invalid_value;
}

auto configuration_provenance(const ResolvedConfig& resolved)
    -> std::vector<domain::ConfigurationProvenanceEntry> {
  std::vector<domain::ConfigurationProvenanceEntry> result;
  result.reserve(resolved.entries.size());
  for (const auto& entry : resolved.entries) {
    domain::ConfigurationProvenanceEntry mapped{};
    mapped.key = entry.key;
    mapped.value_present = entry.value.has_value();
    if (entry.value && !entry.sensitive &&
        !std::holds_alternative<ConfigTextMap>(*entry.value)) {
      mapped.value = format_config_value(*entry.value);
    }
    if (entry.source) mapped.source = provenance_source(*entry.source);
    mapped.sensitive = entry.sensitive;
    mapped.decisions.reserve(entry.decisions.size());
    for (const auto& decision : entry.decisions) {
      domain::ProvenanceDecision projected{};
      projected.source = provenance_source(decision.source);
      projected.disposition = provenance_disposition(decision.disposition);
      if (decision.diagnostic_code) {
        projected.diagnostic_code =
            provenance_diagnostic_code(*decision.diagnostic_code);
      }
      mapped.decisions.push_back(projected);
    }
    result.push_back(std::move(mapped));
  }
  return result;
}

} // namespace aiforge::config
