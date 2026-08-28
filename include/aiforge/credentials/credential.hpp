#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <aiforge/domain/provenance.hpp>

namespace aiforge::credentials {

inline constexpr std::size_t maximum_credential_bytes = 64U * 1024U;

enum class CredentialErrorCode {
  missing_home,
  invalid_base_path,
  invalid_value,
  path_escape,
  not_regular,
  insecure_permissions,
  too_large,
  lock_failed,
  read_failed,
  write_failed,
  sync_failed,
  rename_failed,
};

struct CredentialError {
  CredentialErrorCode code{CredentialErrorCode::read_failed};
  std::filesystem::path path;
  std::string message;
};

class Secret final {
 public:
  Secret(const Secret&) = delete;
  auto operator=(const Secret&) -> Secret& = delete;
  Secret(Secret&& other) noexcept;
  auto operator=(Secret&& other) noexcept -> Secret&;
  ~Secret();

  [[nodiscard]] auto view() const noexcept -> std::string_view;
  [[nodiscard]] auto release() && -> std::string;

 private:
  explicit Secret(std::string value);
  auto clear() noexcept -> void;

  std::string m_value;

  friend auto make_secret(std::string value)
      -> std::expected<Secret, CredentialError>;
};

[[nodiscard]] auto make_secret(std::string value)
    -> std::expected<Secret, CredentialError>;

struct CredentialPathEnvironment {
  std::optional<std::filesystem::path> xdg_config_home;
  std::optional<std::filesystem::path> home;
};

[[nodiscard]] auto resolve_credential_path(
    const CredentialPathEnvironment& environment)
    -> std::expected<std::filesystem::path, CredentialError>;
[[nodiscard]] auto process_credential_path()
    -> std::expected<std::filesystem::path, CredentialError>;

class CredentialStore {
 public:
  virtual ~CredentialStore() = default;

  [[nodiscard]] virtual auto load()
      -> std::expected<std::optional<Secret>, CredentialError> = 0;
  [[nodiscard]] virtual auto store(const Secret& credential)
      -> std::expected<void, CredentialError> = 0;
};

class FileCredentialStore final : public CredentialStore {
 public:
  explicit FileCredentialStore(std::filesystem::path path);

  [[nodiscard]] auto path() const noexcept -> const std::filesystem::path&;
  [[nodiscard]] auto load()
      -> std::expected<std::optional<Secret>, CredentialError> override;
  [[nodiscard]] auto store(const Secret& credential)
      -> std::expected<void, CredentialError> override;

 private:
  std::filesystem::path m_path;
};

struct ResolvedCredential {
  Secret secret;
  domain::CredentialSourceReference source;
};

struct CredentialResolution {
  std::optional<ResolvedCredential> credential;
  std::vector<std::string> warnings;
};

// A present environment value is authoritative. Stored-source failures are
// returned as warnings with no credential so interactive discovery can remain
// available without weakening source validation.
[[nodiscard]] auto resolve_credential(
    std::optional<std::string> environment_value, CredentialStore& store)
    -> std::expected<CredentialResolution, CredentialError>;

} // namespace aiforge::credentials
