#pragma once
#include <aiforge/adapters/process_admin.hpp>
#include <aiforge/config/config.hpp>
#include <aiforge/runtime/ops_source_preparation.hpp>
#include <aiforge/storage/session_store.hpp>

namespace aiforge::adapters::admin_detail {
template <class T> using Result = std::expected<T, cli::CommandFailure>;
// Private application construction seam. No executable user/model callback.
class Dependencies {
 public:
  virtual ~Dependencies() = default;
  [[nodiscard]] virtual auto load_catalog()
      -> Result<config::OpsTargetsConfig> = 0;
  [[nodiscard]] virtual auto open_store()
      -> Result<std::unique_ptr<storage::SessionStore>> = 0;
  [[nodiscard]] virtual auto factory(
      runtime::OpsSourcePreparationIdentity identity)
      -> Result<std::shared_ptr<runtime::OpsSourcePreparationFactory>> = 0;
  [[nodiscard]] virtual auto instance_identity() -> Result<std::string> = 0;
};
[[nodiscard]] auto execute(const cli::AdminCommand::Request& request,
                           Dependencies& dependencies, std::stop_token stop,
                           std::ostream& output) -> Result<void>;
} // namespace aiforge::adapters::admin_detail
