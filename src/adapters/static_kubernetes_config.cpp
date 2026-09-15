#include "static_kubernetes_config_parser.hpp"

#include <aiforge/detail/sha256.hpp>
#include <aiforge/detail/utf8_text.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <new>
#include <span>
#include <utility>

namespace aiforge::adapters::static_kubernetes_detail {
namespace {
auto base64_digit(char value) -> unsigned {
  if (value >= 'A' && value <= 'Z') return static_cast<unsigned>(value - 'A');
  if (value >= 'a' && value <= 'z')
    return static_cast<unsigned>(value - 'a') + 26U;
  if (value >= '0' && value <= '9')
    return static_cast<unsigned>(value - '0') + 52U;
  if (value == '+') return 62;
  if (value == '/') return 63;
  reject();
}
auto pem_body(std::string_view body) -> void {
  require(!body.empty());
  std::string compact;
  compact.reserve(body.size());
  while (!body.empty()) {
    const auto end = body.find('\n');
    require(end != std::string_view::npos);
    auto line = body.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    require(!line.empty() && line.size() <= 76);
    compact += line;
    body.remove_prefix(end + 1);
  }
  const auto decoded = decode_base64(compact);
  require(!decoded.empty());
}
auto single_pem(std::string_view& value, std::string_view label) -> void {
  const auto begin = std::string{"-----BEGIN "} + std::string{label} + "-----";
  const auto end = std::string{"-----END "} + std::string{label} + "-----";
  require(value.starts_with(begin));
  value.remove_prefix(begin.size());
  if (value.starts_with('\r')) value.remove_prefix(1);
  require(value.starts_with('\n'));
  value.remove_prefix(1);
  const auto marker = value.find(end);
  require(marker != std::string_view::npos);
  pem_body(value.substr(0, marker));
  value.remove_prefix(marker + end.size());
  if (value.starts_with('\r')) value.remove_prefix(1);
  if (value.starts_with('\n'))
    value.remove_prefix(1);
  else
    require(value.empty());
}
template <class T> auto unique_names(const std::vector<T>& entries) -> void {
  for (auto current = entries.begin(); current != entries.end(); ++current)
    require(std::find_if(entries.begin(), current, [&](const auto& other) {
              return other.name == current->name;
            }) == current);
}
auto validate_users(const std::vector<User>& users) -> void {
  for (const auto& user : users) {
    if (!user.token.empty()) {
      require(user.cert_data.empty() && user.key_data.empty());
      require(std::ranges::none_of(
          user.token, [](unsigned char c) { return c <= 32 || c == 127; }));
    } else {
      require(!user.cert_data.empty() && !user.key_data.empty());
      validate_pem(decode_base64(user.cert_data), false);
      validate_pem(decode_base64(user.key_data), true);
    }
  }
}
struct Selection {
  std::string_view context;
  std::string_view namespace_name;
};
auto cluster_identity(const Cluster& cluster, Selection selection,
                      std::string_view ca) -> domain::KubernetesOpsIdentity {
  validate_pem(ca, false);
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{ca.data(), ca.size()}));
  domain::KubernetesOpsIdentity identity{
      std::string{selection.context}, std::string{selection.namespace_name},
      parse_endpoint(cluster.server), "sha256:" + hash.finish()};
  require(domain::validate_kubernetes_ops_identity(identity).has_value());
  return identity;
}
auto validate_document(const Document& document,
                       std::string_view selected_context,
                       std::string_view selected_namespace,
                       std::stop_token stop) -> void {
  unique_names(document.clusters);
  unique_names(document.users);
  unique_names(document.contexts);
  for (const auto& cluster : document.clusters) {
    require(!stop.stop_requested(), Failure::cancelled);
    // Actual document/owner values; no fabricated target IDs or grants.
    (void)cluster_identity(cluster, {selected_context, selected_namespace},
                           decode_base64(cluster.ca_data));
  }
  validate_users(document.users);
  for (const auto& context : document.contexts) {
    require(std::ranges::find(document.clusters, context.cluster,
                              &Cluster::name) != document.clusters.end());
    require(std::ranges::find(document.users, context.user, &User::name) !=
            document.users.end());
    if (!context.namespace_name.empty()) {
      const auto& cluster = *std::ranges::find(document.clusters,
                                               context.cluster, &Cluster::name);
      (void)cluster_identity(cluster, {context.name, context.namespace_name},
                             decode_base64(cluster.ca_data));
    }
  }
}
auto emit(Writer& writer, const Cluster& cluster, const User& user,
          const domain::KubernetesOpsIdentity& identity) -> void {
  writer.raw("{\"apiVersion\":\"v1\",\"kind\":\"Config\",\"clusters\":[{"
             "\"name\":\"selected-cluster\",\"cluster\":{\"server\":");
  writer.quoted(cluster.server);
  writer.raw(",\"certificate-authority-data\":");
  writer.quoted(cluster.ca_data);
  writer.raw(R"(}}],"users":[{"name":"selected-user","user":{)");
  if (!user.token.empty()) {
    writer.raw("\"token\":");
    writer.quoted(user.token);
  } else {
    writer.raw("\"client-certificate-data\":");
    writer.quoted(user.cert_data);
    writer.raw(",\"client-key-data\":");
    writer.quoted(user.key_data);
  }
  writer.raw(R"(}}],"contexts":[{"name":)");
  writer.quoted(identity.context_name);
  writer.raw(
      R"(,"context":{"cluster":"selected-cluster","user":"selected-user","namespace":)");
  writer.quoted(identity.namespace_name);
  writer.raw("}}],\"current-context\":");
  writer.quoted(identity.context_name);
  writer.raw("}");
}
} // namespace

auto validate_material_size(std::array<std::string_view, 4> values) -> void {
  std::size_t total{};
  for (const auto value : values) {
    require(value.size() <= material_limit - total,
            Failure::resource_exhausted);
    total += value.size();
  }
}

auto decode_base64(std::string_view value) -> std::string {
  require(!value.empty() && value.size() % 4 == 0);
  require(value.size() <= scalar_limit, Failure::resource_exhausted);
  std::string output;
  output.reserve((value.size() / 4) * 3);
  for (std::size_t index{}; index < value.size(); index += 4) {
    const auto a = base64_digit(value[index]);
    const auto b = base64_digit(value[index + 1]);
    const bool pad2 = value[index + 2] == '=';
    const bool pad3 = value[index + 3] == '=';
    require(!pad2 || pad3);
    require(!(pad2 || pad3) || index + 4 == value.size());
    const auto c = pad2 ? 0U : base64_digit(value[index + 2]);
    const auto d = pad3 ? 0U : base64_digit(value[index + 3]);
    require(!pad2 || (b & 15U) == 0);
    require(!pad3 || (c & 3U) == 0);
    output.push_back(static_cast<char>((a << 2U) | (b >> 4U)));
    if (!pad2) output.push_back(static_cast<char>((b << 4U) | (c >> 2U)));
    if (!pad3) output.push_back(static_cast<char>((c << 6U) | d));
  }
  return output;
}
auto validate_pem(std::string_view value, bool private_key) -> void {
  require(value.size() <= scalar_limit && !value.empty());
  if (private_key) {
    constexpr std::array labels{"PRIVATE KEY", "RSA PRIVATE KEY",
                                "EC PRIVATE KEY"};
    for (const auto label : labels) {
      if (value.starts_with(std::string{"-----BEGIN "} + label + "-----")) {
        single_pem(value, label);
        require(value.empty());
        return;
      }
    }
    reject();
  }
  std::size_t blocks{};
  while (!value.empty()) {
    require(++blocks <= 32, Failure::resource_exhausted);
    single_pem(value, "CERTIFICATE");
  }
}
auto parse_endpoint(std::string_view value) -> domain::OpsHttpsEndpoint {
  require(value.starts_with("https://"));
  value.remove_prefix(8);
  if (value.ends_with('/')) value.remove_suffix(1);
  require(!value.empty() &&
          value.find_first_of("/@?#%\\") == std::string_view::npos);
  std::string_view host;
  std::string_view port;
  if (value.starts_with('[')) {
    const auto end = value.find(']');
    require(end != std::string_view::npos);
    host = value.substr(1, end - 1);
    require(host.find(':') != std::string_view::npos);
    value.remove_prefix(end + 1);
    if (!value.empty()) {
      require(value.starts_with(':'));
      port = value.substr(1);
      require(!port.empty());
    }
  } else {
    const auto colon = value.find(':');
    host = value.substr(0, colon);
    if (colon != std::string_view::npos) {
      port = value.substr(colon + 1);
      require(!port.empty());
    }
  }
  std::uint16_t number = 443;
  if (!port.empty()) {
    const auto parsed =
        std::from_chars(port.data(), port.data() + port.size(), number);
    require(parsed.ec == std::errc{} &&
            parsed.ptr == port.data() + port.size() && number != 0);
  }
  return {std::string{host}, number};
}
auto Writer::raw(std::string_view value) -> void {
  require(value.size() <= output_limit - m_size, Failure::resource_exhausted);
  m_size += value.size();
  if (m_output != nullptr) m_output->append(value);
}
auto Writer::quoted(std::string_view value) -> void {
  raw("\"");
  for (const char character : value) {
    switch (character) {
      case '"': raw("\\\""); break;
      case '\\': raw("\\\\"); break;
      default:
        require(static_cast<unsigned char>(character) >= 32);
        raw(std::string_view{&character, 1});
        break;
    }
  }
  raw("\"");
}
} // namespace aiforge::adapters::static_kubernetes_detail

namespace aiforge::adapters {
namespace cfg = static_kubernetes_detail;
StaticKubernetesConfig::StaticKubernetesConfig(
    domain::KubernetesOpsIdentity identity, std::string configuration,
    Material material)
    : m_identity(std::move(identity)),
      m_configuration(std::move(configuration)),
      m_material(std::move(material)) {
}
auto StaticKubernetesConfig::tls_material() const& noexcept
    -> StaticKubernetesTlsView {
  if (!m_material.token.empty())
    return {m_material.ca_pem, StaticKubernetesTokenView{m_material.token}};
  return {m_material.ca_pem,
          StaticKubernetesClientCertificateView{
              m_material.certificate_chain_pem, m_material.private_key_pem}};
}
auto StaticKubernetesConfig::parse(std::string_view bytes,
                                   StaticKubernetesSyntax syntax,
                                   std::string_view context,
                                   std::string_view namespace_name,
                                   std::stop_token stop) noexcept
    -> std::expected<StaticKubernetesConfig, StaticKubernetesConfigFailure> {
  try {
    cfg::require(!stop.stop_requested(), cfg::Failure::cancelled);
    cfg::require(bytes.size() <= cfg::input_limit,
                 cfg::Failure::resource_exhausted);
    cfg::require(detail::is_safe_utf8_text(bytes) &&
                 cfg::safe_text(context, 256) &&
                 cfg::safe_text(namespace_name, 63));
    cfg::Sink sink{stop};
    switch (syntax) {
      case StaticKubernetesSyntax::yaml: cfg::parse_yaml(bytes, sink); break;
      case StaticKubernetesSyntax::json: cfg::parse_json(bytes, sink); break;
      default: cfg::reject();
    }
    const auto document = sink.finish();
    cfg::validate_document(document, context, namespace_name, stop);
    const auto selected =
        std::ranges::find(document.contexts, context, &cfg::Context::name);
    cfg::require(selected != document.contexts.end());
    const auto& cluster = *std::ranges::find(
        document.clusters, selected->cluster, &cfg::Cluster::name);
    const auto& user =
        *std::ranges::find(document.users, selected->user, &cfg::User::name);
    auto ca_pem = cfg::decode_base64(cluster.ca_data);
    auto identity =
        cfg::cluster_identity(cluster, {context, namespace_name}, ca_pem);
    auto certificate = user.cert_data.empty()
                           ? std::string{}
                           : cfg::decode_base64(user.cert_data);
    auto key = user.key_data.empty() ? std::string{}
                                     : cfg::decode_base64(user.key_data);
    cfg::validate_material_size({ca_pem, user.token, certificate, key});
    cfg::Writer measure;
    cfg::emit(measure, cluster, user, identity);
    std::string output;
    output.reserve(measure.size());
    cfg::Writer writer{&output};
    cfg::emit(writer, cluster, user, identity);
    cfg::require(!stop.stop_requested(), cfg::Failure::cancelled);
    return StaticKubernetesConfig{std::move(identity), std::move(output),
                                  Material{std::move(ca_pem), user.token,
                                           std::move(certificate),
                                           std::move(key)}};
  } catch (const cfg::Rejected& error) {
    return std::unexpected(error.failure);
  } catch (const std::bad_alloc&) {
    return std::unexpected(cfg::Failure::resource_exhausted);
  } catch (...) {
    return std::unexpected(cfg::Failure::internal_failure);
  }
}
} // namespace aiforge::adapters
