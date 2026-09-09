#include "kubernetes_tls_context.hpp"
#include <limits>
#include <memory>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <vector>
#include <venice/detail/httplib_contract.hpp>

namespace aiforge::adapters {
namespace {
using Error = runtime::OpsObservationSourceError;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
auto reject_password(char*, int, int, void*) -> int {
  return 0;
}
auto bio(std::string_view value) -> Bio {
  if (value.empty() ||
      value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return {nullptr, BIO_free};
  return {BIO_new_mem_buf(value.data(), static_cast<int>(value.size())),
          BIO_free};
}
auto skip_space(std::string_view& value) -> void {
  const auto first = value.find_first_not_of(" \t\r\n");
  value = first == std::string_view::npos ? std::string_view{}
                                          : value.substr(first);
}
auto certificates(std::string_view input, const KubernetesHttpsControl& control)
    -> std::expected<std::vector<Certificate>, Error> {
  constexpr std::string_view begin{"-----BEGIN CERTIFICATE-----"};
  constexpr std::string_view end{"-----END CERTIFICATE-----"};
  std::vector<Certificate> result;
  while (true) {
    if (const auto error = control.failure()) return std::unexpected(*error);
    skip_space(input);
    if (input.empty()) break;
    if (result.size() == 32 || !input.starts_with(begin))
      return std::unexpected(Error::trust_failed);
    const auto offset = input.find(end, begin.size());
    if (offset == std::string_view::npos)
      return std::unexpected(Error::trust_failed);
    const auto length = offset + end.size();
    auto buffer = bio(input.substr(0, length));
    if (!buffer) return std::unexpected(Error::resource_exhausted);
    Certificate certificate{
        PEM_read_bio_X509(buffer.get(), nullptr, reject_password, nullptr),
        X509_free};
    if (!certificate || BIO_ctrl_pending(buffer.get()) != 0)
      return std::unexpected(Error::trust_failed);
    result.push_back(std::move(certificate));
    input.remove_prefix(length);
  }
  if (result.empty()) return std::unexpected(Error::trust_failed);
  return result;
}
auto exhausted_key_bio(BIO* buffer) -> bool {
  const char* remaining{};
  const auto size = BIO_get_mem_data(buffer, &remaining);
  if (size < 0 || (size != 0 && remaining == nullptr)) return false;
  if (size == 0) return true;
  auto tail = std::string_view{remaining, static_cast<std::size_t>(size)};
  skip_space(tail);
  return tail.empty();
}
auto client_identity(SSL_CTX* context,
                     StaticKubernetesClientCertificateView material,
                     const KubernetesHttpsControl& control)
    -> std::expected<void, Error> {
  auto chain = certificates(material.certificate_chain_pem, control);
  if (!chain) return std::unexpected(chain.error());
  auto buffer = bio(material.private_key_pem);
  if (!buffer) return std::unexpected(Error::resource_exhausted);
  Key key{
      PEM_read_bio_PrivateKey(buffer.get(), nullptr, reject_password, nullptr),
      EVP_PKEY_free};
  if (!key || !exhausted_key_bio(buffer.get()) ||
      SSL_CTX_use_certificate(context, chain->front().get()) != 1 ||
      SSL_CTX_use_PrivateKey(context, key.get()) != 1 ||
      SSL_CTX_check_private_key(context) != 1)
    return std::unexpected(Error::authentication_failed);
  for (std::size_t i = 1; i < chain->size(); ++i) {
    if (const auto error = control.failure()) return std::unexpected(*error);
    auto* child = (*chain)[i - 1].get();
    auto* issuer = (*chain)[i].get();
    Key issuer_key{X509_get_pubkey(issuer), EVP_PKEY_free};
    if (!issuer_key ||
        X509_NAME_cmp(X509_get_issuer_name(child),
                      X509_get_subject_name(issuer)) != 0 ||
        X509_verify(child, issuer_key.get()) != 1 ||
        SSL_CTX_add1_chain_cert(context, issuer) != 1)
      return std::unexpected(Error::authentication_failed);
  }
  return {};
}
} // namespace
auto prepare_kubernetes_tls(httplib::SSLClient& client,
                            StaticKubernetesTlsView material,
                            const KubernetesHttpsControl& control)
    -> std::expected<void, Error> {
  if (const auto error = control.failure()) return std::unexpected(*error);
  auto* context = static_cast<SSL_CTX*>(client.tls_context());
  if (context == nullptr) return std::unexpected(Error::unavailable);
  auto authorities =
      certificates(material.certificate_authorities_pem, control);
  if (!authorities) return std::unexpected(authorities.error());
  std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store{
      X509_STORE_new(), X509_STORE_free};
  if (!store) return std::unexpected(Error::resource_exhausted);
  for (const auto& authority : *authorities) {
    if (const auto error = control.failure()) return std::unexpected(*error);
    if (X509_STORE_add_cert(store.get(), authority.get()) != 1)
      return std::unexpected(Error::trust_failed);
  }
  client.enable_system_ca(false);
  client.set_ca_cert_store(store.release());
  client.enable_server_certificate_verification(true);
  client.enable_server_hostname_verification(true);
  if (const auto* certificate =
          std::get_if<StaticKubernetesClientCertificateView>(
              &material.authentication))
    return client_identity(context, *certificate, control);
  return {};
}
} // namespace aiforge::adapters
