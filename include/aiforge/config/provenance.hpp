#pragma once

#include <vector>

#include <aiforge/config/config.hpp>
#include <aiforge/domain/provenance.hpp>

namespace aiforge::config {

// Projects a resolved configuration into neutral domain provenance so it can be
// recorded as a run event. A sensitive key contributes presence, source, and
// its decision trail; its resolved value is never copied. Free diagnostic text
// is deliberately dropped: the per-entry decision codes carry the same facts
// without carrying a message that could quote a value.
[[nodiscard]] auto configuration_provenance(const ResolvedConfig& resolved)
    -> std::vector<domain::ConfigurationProvenanceEntry>;

[[nodiscard]] auto provenance_source(ConfigSource source)
    -> domain::ProvenanceSource;
[[nodiscard]] auto provenance_disposition(CandidateDisposition disposition)
    -> domain::ProvenanceDisposition;
[[nodiscard]] auto provenance_diagnostic_code(ConfigDiagnosticCode code)
    -> domain::ProvenanceDiagnosticCode;

} // namespace aiforge::config
