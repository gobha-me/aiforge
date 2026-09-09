#include "process_admin_internal.hpp"
#include <aiforge/adapters/linux_ops_source_preparation.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/config/file_store.hpp>
#include <array>
#include <random>

namespace aiforge::adapters {
namespace {
using admin_detail::Result;
auto failure(std::string message) -> std::unexpected<cli::CommandFailure> {
  return std::unexpected(cli::CommandFailure{cli::CommandFailureKind::runtime,
                                             std::move(message)});
}
class ProcessDependencies final : public admin_detail::Dependencies {
 public:
  auto load_catalog() -> Result<config::OpsTargetsConfig> override {
    auto path = config::process_config_path();
    if (!path) return failure("Admin configuration path is unavailable");
    const auto& registry = config::builtin_config_registry();
    config::JsonConfigFileStore store{*path};
    auto file = store.load(registry);
    if (!file) return failure("Admin configuration could not be read");
    const std::array layers{std::move(*file)};
    auto resolved = config::resolve_config(registry, layers);
    if (!resolved) return failure("Admin configuration is invalid");
    auto catalog = config::resolve_ops_targets(*resolved);
    if (!catalog) return failure("Admin targets configuration is invalid");
    return std::move(*catalog);
  }
  auto open_store() -> Result<std::unique_ptr<storage::SessionStore>> override {
    auto path = process_session_store_path();
    if (!path) return failure("Admin session storage path is unavailable");
    auto store = SqliteSessionStore::open(*path);
    if (!store) return failure("Admin session storage could not be opened");
    return std::move(*store);
  }
  auto factory(runtime::OpsSourcePreparationIdentity identity) -> Result<
      std::shared_ptr<runtime::OpsSourcePreparationFactory>> override {
    auto prepared = LinuxOpsSourcePreparationFactory::create(
        std::move(identity.target_id),
        std::move(identity.configuration_revision));
    if (!prepared) return failure("Admin source preparation is unavailable");
    return std::move(*prepared);
  }
  auto instance_identity() -> Result<std::string> override {
    std::random_device random;
    std::string value{"admin"};
    for (unsigned i = 0; i < 4; ++i)
      value += '-' + std::to_string(random());
    return value;
  }
};
} // namespace

auto ProcessAdminCommand::execute(Request request,
                                  cli::CommandEnvironment& environment,
                                  std::ostream& output,
                                  std::ostream& diagnostics) -> Result<void> {
  static_cast<void>(diagnostics);
  try {
    ProcessDependencies dependencies;
    return admin_detail::execute(request, dependencies, environment.stop_token,
                                 output);
  } catch (...) {
    // Allocation failure must not require another allocation to report failure.
    return std::unexpected(
        cli::CommandFailure{cli::CommandFailureKind::runtime, {}});
  }
}
} // namespace aiforge::adapters
