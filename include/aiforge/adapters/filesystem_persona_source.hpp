#pragma once

#include <filesystem>
#include <functional>

#include <aiforge/config/file_store.hpp>
#include <aiforge/persona/editor.hpp>
#include <aiforge/persona/source.hpp>

namespace aiforge::adapters {

[[nodiscard]] auto resolve_persona_root(
    const config::ConfigPathEnvironment& environment)
    -> std::expected<std::filesystem::path, persona::PersonaError>;

[[nodiscard]] auto process_persona_root()
    -> std::expected<std::filesystem::path, persona::PersonaError>;

enum class PersonaFilesystemCheckpointStage {
  temporary_synced,
  replacement_ready,
  rollback_ready,
  published,
};

using PersonaFilesystemCheckpoint =
    std::function<std::expected<void, persona::PersonaEditorError>(
        PersonaFilesystemCheckpointStage)>;

class FilesystemPersonaSource final : public persona::PersonaSource,
                                      public persona::PersonaEditor {
 public:
  explicit FilesystemPersonaSource(std::filesystem::path root,
                                   PersonaFilesystemCheckpoint checkpoint = {})
      : m_root(std::move(root)), m_checkpoint(std::move(checkpoint)) {}

  [[nodiscard]] auto root() const noexcept -> const std::filesystem::path& {
    return m_root;
  }
  [[nodiscard]] auto list(persona::PersonaLimits limits = {},
                          std::stop_token stop_token = {})
      -> std::expected<std::vector<domain::PersonaSummary>,
                       persona::PersonaError> override;
  [[nodiscard]] auto load(std::string name, persona::PersonaLimits limits = {},
                          std::stop_token stop_token = {})
      -> std::expected<domain::PersonaDocument, persona::PersonaError> override;
  [[nodiscard]] auto create(persona::PersonaCreate request,
                            std::stop_token stop_token = {})
      -> std::expected<persona::PersonaWriteReceipt,
                       persona::PersonaEditorError> override;
  [[nodiscard]] auto replace(persona::PersonaReplace request,
                             std::stop_token stop_token = {})
      -> std::expected<persona::PersonaWriteReceipt,
                       persona::PersonaEditorError> override;

 private:
  std::filesystem::path m_root;
  PersonaFilesystemCheckpoint m_checkpoint;
};

} // namespace aiforge::adapters
