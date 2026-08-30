#pragma once

#include <filesystem>
#include <memory>

#include <aiforge/storage/artifact_store.hpp>

namespace aiforge::adapters {

struct FilesystemArtifactStoreLimits {
  std::size_t maximum_artifact_bytes{32U * 1024U * 1024U};
  auto operator==(const FilesystemArtifactStoreLimits&) const -> bool = default;
};

enum class FilesystemArtifactStoreOpenMode {
  create,
  existing,
};

class FilesystemArtifactStore final : public storage::ArtifactStore {
 public:
  [[nodiscard]] static auto open(std::filesystem::path root,
                                 FilesystemArtifactStoreLimits limits = {},
                                 FilesystemArtifactStoreOpenMode mode =
                                     FilesystemArtifactStoreOpenMode::create)
      -> std::expected<std::unique_ptr<FilesystemArtifactStore>,
                       storage::ArtifactStoreError>;
  ~FilesystemArtifactStore() override;

  FilesystemArtifactStore(const FilesystemArtifactStore&) = delete;
  auto operator=(const FilesystemArtifactStore&)
      -> FilesystemArtifactStore& = delete;

  [[nodiscard]] auto put(storage::ArtifactWrite write,
                         std::span<const std::byte> content,
                         std::stop_token stop_token = {})
      -> std::expected<domain::ArtifactMetadata,
                       storage::ArtifactStoreError> override;
  [[nodiscard]] auto get(const domain::ArtifactMetadata& metadata,
                         std::size_t maximum_bytes,
                         std::stop_token stop_token = {})
      -> std::expected<storage::ArtifactRead,
                       storage::ArtifactStoreError> override;
  [[nodiscard]] auto root() const noexcept -> const std::filesystem::path&;

 private:
  FilesystemArtifactStore(std::filesystem::path root,
                          FilesystemArtifactStoreLimits limits);
  std::filesystem::path m_root;
  FilesystemArtifactStoreLimits m_limits;
};

} // namespace aiforge::adapters
