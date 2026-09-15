#pragma once

#include <aiforge/domain/ops_target.hpp>
#include <expected>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>

namespace aiforge::adapters {

enum class StaticKubernetesSyntax { yaml, json };
enum class StaticKubernetesConfigFailure {
  invalid_input,
  unsupported,
  resource_exhausted,
  cancelled,
  internal_failure
};

struct StaticKubernetesTokenView {
  std::string_view token;
};
struct StaticKubernetesClientCertificateView {
  std::string_view certificate_chain_pem;
  std::string_view private_key_pem;
};
struct StaticKubernetesTlsView {
  std::string_view certificate_authorities_pem;
  std::variant<StaticKubernetesTokenView, StaticKubernetesClientCertificateView>
      authentication;
};

// Adapter-private credential custody. Never put configuration_bytes() into
// events, artifacts, errors, logs or neutral/fake launch captures. This parser
// establishes no authority and does not perform or authorize a child launch.
class StaticKubernetesConfig final {
 public:
  StaticKubernetesConfig(const StaticKubernetesConfig&) = delete;
  auto operator=(const StaticKubernetesConfig&)
      -> StaticKubernetesConfig& = delete;
  StaticKubernetesConfig(StaticKubernetesConfig&&) noexcept = default;
  auto operator=(StaticKubernetesConfig&&) noexcept
      -> StaticKubernetesConfig& = default;
  ~StaticKubernetesConfig() = default;

  [[nodiscard]] static auto parse(std::string_view bytes,
                                  StaticKubernetesSyntax syntax,
                                  std::string_view context,
                                  std::string_view namespace_name,
                                  std::stop_token stop = {}) noexcept
      -> std::expected<StaticKubernetesConfig, StaticKubernetesConfigFailure>;
  [[nodiscard]] auto identity() const noexcept
      -> const domain::KubernetesOpsIdentity& {
    return m_identity;
  }
  [[nodiscard]] auto configuration_bytes() const noexcept -> std::string_view {
    return m_configuration;
  }
  // Views expire when this owner is moved, replaced or destroyed. These are
  // structurally admitted bytes, not validated X.509/key material or authority.
  [[nodiscard]] auto tls_material() const& noexcept -> StaticKubernetesTlsView;
  [[nodiscard]] auto tls_material() const&& -> StaticKubernetesTlsView = delete;

 private:
  struct Material {
    std::string ca_pem;
    std::string token;
    std::string certificate_chain_pem;
    std::string private_key_pem;
  };
  StaticKubernetesConfig(domain::KubernetesOpsIdentity identity,
                         std::string configuration, Material material);
  domain::KubernetesOpsIdentity m_identity;
  std::string m_configuration;
  Material m_material;
};
} // namespace aiforge::adapters
