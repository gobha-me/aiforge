#pragma once

#include <optional>
#include <string>

#include <aiforge/config/config.hpp>
#include <aiforge/domain/provenance.hpp>

namespace aiforge::adapters {

// Builds the provenance record both process surfaces attach to their runs. The
// tool section is left empty because the run kernel owns it.
//
// A credential source is a non-secret locator such as an environment variable
// name. Credential bytes never reach this function.
[[nodiscard]] auto process_run_provenance(
    const config::ResolvedConfig& resolved, domain::ModelId model_id,
    std::string backend_id,
    std::optional<domain::CredentialSourceReference> credential_source)
    -> domain::RunProvenance;

}  // namespace aiforge::adapters
