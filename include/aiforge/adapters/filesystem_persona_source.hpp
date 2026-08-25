#pragma once

#include <filesystem>

#include <aiforge/config/file_store.hpp>
#include <aiforge/persona/source.hpp>

namespace aiforge::adapters {

[[nodiscard]] auto resolve_persona_root(
    const config::ConfigPathEnvironment& environment)
    -> std::expected<std::filesystem::path, persona::PersonaError>;

[[nodiscard]] auto process_persona_root()
    -> std::expected<std::filesystem::path, persona::PersonaError>;

class FilesystemPersonaSource final : public persona::PersonaSource {
 public:
  explicit FilesystemPersonaSource(std::filesystem::path root)
      : m_root(std::move(root)) {}

  [[nodiscard]] auto root() const noexcept -> const std::filesystem::path& {
    return m_root;
  }
  [[nodiscard]] auto list(persona::PersonaLimits limits = {},
                          std::stop_token stop_token = {})
      -> std::expected<std::vector<domain::PersonaSummary>,
                       persona::PersonaError> override;
  [[nodiscard]] auto load(std::string name,
                          persona::PersonaLimits limits = {},
                          std::stop_token stop_token = {})
      -> std::expected<domain::PersonaDocument,
                       persona::PersonaError> override;

 private:
  std::filesystem::path m_root;
};

}  // namespace aiforge::adapters
