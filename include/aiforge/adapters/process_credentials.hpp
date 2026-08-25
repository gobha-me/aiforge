#pragma once

#include <expected>
#include <iosfwd>

#include <aiforge/cli/command_registry.hpp>
#include <aiforge/credentials/credential.hpp>

namespace aiforge::adapters {

// Resolves process-owned credential sources and emits only bounded, non-secret
// stored-source warnings to the supplied diagnostic stream.
[[nodiscard]] auto resolve_process_credential(std::ostream& diagnostics)
    -> std::expected<credentials::CredentialResolution, cli::CommandFailure>;

}  // namespace aiforge::adapters
