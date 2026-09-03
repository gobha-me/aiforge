#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <aiforge/backend/backend.hpp>
#include <aiforge/backend/provider_character_catalog.hpp>
#include <aiforge/credentials/credential.hpp>

namespace aiforge::adapters {

namespace detail {

[[nodiscard]] auto advance_venice_character_offset(int current,
                                                   std::size_t returned)
    -> std::expected<int, backend::ProviderCharacterError>;

} // namespace detail

struct VeniceBackendOptions {
  std::string base_url{"https://api.venice.ai/api/v1"};
  std::optional<std::chrono::milliseconds> connect_timeout;
  std::optional<std::chrono::milliseconds> read_timeout;
  std::optional<std::chrono::milliseconds> write_timeout;
  std::size_t pending_events{256};
};

class VeniceBackend final : public backend::Backend,
                            public backend::ModelContextProvider,
                            public backend::ProviderCharacterCatalogSource {
 public:
  explicit VeniceBackend(credentials::Secret credential,
                         VeniceBackendOptions options = {});
  ~VeniceBackend() override;

  VeniceBackend(const VeniceBackend&) = delete;
  auto operator=(const VeniceBackend&) -> VeniceBackend& = delete;
  VeniceBackend(VeniceBackend&&) noexcept;
  auto operator=(VeniceBackend&&) noexcept -> VeniceBackend&;

  [[nodiscard]] auto start(backend::BackendRequest request,
                           std::stop_token stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override;
  [[nodiscard]] auto lookup(const domain::ModelId& model_id,
                            std::stop_token stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override;
  [[nodiscard]] auto list(backend::ProviderCharacterLimits limits = {},
                          std::stop_token stop_token = {})
      -> std::expected<backend::ProviderCharacterCatalog,
                       backend::ProviderCharacterError> override;
  [[nodiscard]] auto lookup(const domain::ProviderCharacterId& id,
                            backend::ProviderCharacterLimits limits = {},
                            std::stop_token stop_token = {})
      -> std::expected<backend::ProviderCharacterSummary,
                       backend::ProviderCharacterError> override;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace aiforge::adapters
