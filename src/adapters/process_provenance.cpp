#include <aiforge/adapters/process_provenance.hpp>

#include <utility>

#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/cli/command_registry.hpp>
#include <aiforge/config/provenance.hpp>

namespace aiforge::adapters {

auto process_run_provenance(
    const config::ResolvedConfig& resolved, domain::ModelId model_id,
    std::string backend_id,
    std::optional<domain::CredentialSourceReference> credential_source)
    -> domain::RunProvenance {
  auto version = cli::project_version();
  domain::RunProvenance provenance{version,
                                   std::move(backend_id),
                                   std::nullopt,
                                   std::move(model_id),
                                   std::move(credential_source),
                                   config::configuration_provenance(resolved),
                                   {},
                                   {}};
  provenance.components.push_back({"aiforge", std::move(version)});
  if (auto sqlite = sqlite_library_version(); !sqlite.empty()) {
    provenance.components.push_back({"sqlite3", std::move(sqlite)});
  }
  return provenance;
}

} // namespace aiforge::adapters
