#pragma once

#include <functional>
#include <memory>

#include <aiforge/cli/command_registry.hpp>
#include <aiforge/storage/artifact_store.hpp>
#include <aiforge/storage/session_store.hpp>

namespace aiforge::adapters {

class ProcessVideoCommand final : public cli::VideoCommand {
 public:
  using SessionStoreFactory =
      std::function<std::expected<std::shared_ptr<storage::SessionStore>,
                                  cli::CommandFailure>()>;
  using ArtifactStoreFactory =
      std::function<std::expected<std::shared_ptr<storage::ArtifactStore>,
                                  cli::CommandFailure>()>;

  ProcessVideoCommand();
  ProcessVideoCommand(SessionStoreFactory session_store_factory,
                      ArtifactStoreFactory artifact_store_factory);

  [[nodiscard]] auto show(ShowRequest request,
                          cli::CommandEnvironment& environment,
                          std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override;
  [[nodiscard]] auto export_artifact(ExportRequest request,
                                     cli::CommandEnvironment& environment,
                                     std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override;

 private:
  SessionStoreFactory m_session_store_factory;
  ArtifactStoreFactory m_artifact_store_factory;
};

} // namespace aiforge::adapters
