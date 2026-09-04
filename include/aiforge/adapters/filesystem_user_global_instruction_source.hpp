#pragma once

#include <filesystem>
#include <functional>

#include <aiforge/config/file_store.hpp>
#include <aiforge/instructions/editor.hpp>
#include <aiforge/instructions/source.hpp>

namespace aiforge::adapters {

[[nodiscard]] auto resolve_user_global_instruction_path(
    const config::ConfigPathEnvironment& environment)
    -> std::expected<std::filesystem::path,
                     instructions::UserGlobalInstructionError>;

[[nodiscard]] auto process_user_global_instruction_path()
    -> std::expected<std::filesystem::path,
                     instructions::UserGlobalInstructionError>;

enum class UserGlobalInstructionFilesystemCheckpointStage {
  temporary_synced,
  replacement_ready,
  rollback_ready,
  published,
  root_revalidation_ready,
};

using UserGlobalInstructionFilesystemCheckpoint = std::function<
    std::expected<void, instructions::UserGlobalInstructionEditorError>(
        UserGlobalInstructionFilesystemCheckpointStage)>;

class FilesystemUserGlobalInstructionSource final
    : public instructions::UserGlobalInstructionSource,
      public instructions::UserGlobalInstructionEditor {
 public:
  explicit FilesystemUserGlobalInstructionSource(
      std::filesystem::path path,
      UserGlobalInstructionFilesystemCheckpoint checkpoint = {})
      : m_path(std::move(path)), m_checkpoint(std::move(checkpoint)) {}

  [[nodiscard]] auto path() const noexcept -> const std::filesystem::path& {
    return m_path;
  }

  [[nodiscard]] auto load(instructions::UserGlobalInstructionLimits limits = {},
                          std::stop_token stop_token = {})
      -> std::expected<std::optional<domain::UserGlobalInstructionDocument>,
                       instructions::UserGlobalInstructionError> override;

  [[nodiscard]] auto write(instructions::UserGlobalInstructionWrite request,
                           std::stop_token stop_token = {})
      -> std::expected<instructions::UserGlobalInstructionWriteReceipt,
                       instructions::UserGlobalInstructionEditorError> override;

 private:
  std::filesystem::path m_path;
  UserGlobalInstructionFilesystemCheckpoint m_checkpoint;
};

} // namespace aiforge::adapters
