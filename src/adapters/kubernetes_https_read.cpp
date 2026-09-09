#include "kubernetes_https_read.hpp"
#include "kubernetes_credential_exclusion.hpp"
#include "kubernetes_https_control.hpp"
#include "kubernetes_observation_projection.hpp"
#include "kubernetes_tls_context.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <venice/detail/httplib_contract.hpp>

namespace aiforge::adapters {
namespace {
using Error = runtime::OpsObservationSourceError;
using Clock = std::chrono::steady_clock;
#if defined(__linux__)
auto encoded(std::string_view input) -> std::string {
  constexpr std::string_view digits{"0123456789ABCDEF"};
  std::string result;
  result.reserve(input.size());
  for (const unsigned char byte : input) {
    if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
        byte == '_' || byte == '~') {
      result.push_back(static_cast<char>(byte));
    } else {
      result.push_back('%');
      result.push_back(digits[byte >> 4U]);
      result.push_back(digits[byte & 15U]);
    }
  }
  return result;
}
auto selector_value(std::string_view input) -> std::string {
  std::string result;
  for (const char byte : input) {
    if (byte == '\\' || byte == ',' || byte == '=') result.push_back('\\');
    result.push_back(byte);
  }
  return result;
}
auto path(const domain::OpsObservationRequest& request) -> std::string {
  const auto& identity =
      std::get<domain::KubernetesOpsIdentity>(request.target.identity);
  auto result = "/api/v1/namespaces/" + encoded(identity.namespace_name);
  if (request.operation ==
      domain::OpsObservationOperation::kubernetes_pod_health)
    return result + "/pods/" +
           encoded(
               std::get<domain::KubernetesPodIdentity>(request.resource).name);
  result +=
      request.operation == domain::OpsObservationOperation::kubernetes_workloads
          ? "/pods?limit="
          : "/events?limit=";
  result += std::to_string(request.limits.maximum_entries);
  // Core/v1 events attest Pod identity. An optional selected container remains
  // request context, consistent with the domain's Pod-scoped event contract.
  if (const auto* pod =
          std::get_if<domain::KubernetesPodIdentity>(&request.resource)) {
    const auto selector =
        "involvedObject.kind=Pod,involvedObject.namespace=" +
        selector_value(pod->namespace_name) +
        ",involvedObject.name=" + selector_value(pod->name) +
        ",involvedObject.uid=" + selector_value(pod->uid.value());
    result += "&fieldSelector=" + encoded(selector);
  }
  return result;
}
auto lower(std::string_view input) -> std::string {
  std::string result{input};
  for (auto& byte : result)
    if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte - 'A' + 'a');
  return result;
}
auto json_type(std::string_view input) -> bool {
  const auto value = lower(input);
  return value == "application/json" ||
         value == "application/json; charset=utf-8" ||
         value == "application/json;charset=utf-8";
}
auto status_error(int status) -> std::optional<Error> {
  if (status == 200) return {};
  if (status == 401) return Error::authentication_failed;
  if (status == 403) return Error::permission_denied;
  if (status == 404) return Error::source_changed;
  if (status >= 300 && status < 400) return Error::trust_failed;
  if (status == 429 || (status >= 500 && status <= 599))
    return Error::unavailable;
  return Error::invalid_result;
}
auto transport_error(httplib::Error error) -> Error {
  switch (error) {
    case httplib::Error::SSLServerVerification:
    case httplib::Error::SSLServerHostnameVerification:
      return Error::trust_failed;
    case httplib::Error::SSLLoadingCerts: return Error::authentication_failed;
    case httplib::Error::ConnectionTimeout:
    case httplib::Error::Timeout: return Error::timed_out;
    case httplib::Error::ResourceExhaustion:
    case httplib::Error::ExceedMaxPayloadSize: return Error::resource_exhausted;
    case httplib::Error::Connection:
    case httplib::Error::SSLConnection:
    case httplib::Error::Read:
    case httplib::Error::Write: return Error::disconnected;
    default: return Error::invalid_result;
  }
}
class Capture final {
 public:
  Capture(const KubernetesHttpsControl& control, std::uint64_t maximum)
      : m_control(control), m_remaining(maximum) {}
  auto header(const httplib::Response& response) -> bool {
    if (m_header_seen) return refuse(Error::invalid_result);
    m_header_seen = true;
    if (!charge(response.version.size()) || !charge(3) ||
        !charge(response.reason.size()) || !headers(response.headers))
      return false;
    if (const auto error = status_error(response.status)) return refuse(*error);
    if (response.get_header_value_count("Content-Type") != 1 ||
        !json_type(response.get_header_value("Content-Type")))
      return refuse(Error::invalid_result);
    if (!content_framing(response)) return false;
    return true;
  }
  auto body(const char* data, std::size_t length, std::uint64_t offset,
            std::uint64_t) -> bool {
    if (!m_header_seen || offset != m_body.size())
      return refuse(Error::invalid_result);
    if (!charge(length)) return false;
    m_body.append(data, length);
    return true;
  }
  auto headers(const httplib::Headers& fields) -> bool {
    return std::ranges::all_of(fields, [this](const auto& field) {
      return charge(field.first.size()) && charge(field.second.size());
    });
  }
  [[nodiscard]] auto failure() const noexcept -> std::optional<Error> {
    return m_failure;
  }
  [[nodiscard]] auto text() const noexcept -> std::string_view {
    return m_body;
  }

 private:
  auto refuse(Error error) noexcept -> bool {
    m_failure = error;
    return false;
  }
  auto charge(std::size_t size) -> bool {
    if (const auto error = m_control.failure()) return refuse(*error);
    if (size > m_remaining) return refuse(Error::resource_exhausted);
    m_remaining -= size;
    return true;
  }
  auto content_framing(const httplib::Response& response) -> bool {
    const auto coding = response.get_header_value_count("Content-Encoding");
    if (coding > 1 || (coding == 1 && lower(response.get_header_value(
                                          "Content-Encoding")) != "identity"))
      return refuse(Error::invalid_result);
    const auto lengths = response.get_header_value_count("Content-Length");
    const auto transfer = response.get_header_value_count("Transfer-Encoding");
    if (lengths > 1 || transfer > 1 || (lengths != 0 && transfer != 0) ||
        (transfer == 1 &&
         lower(response.get_header_value("Transfer-Encoding")) != "chunked"))
      return refuse(Error::invalid_result);
    if (lengths == 1) {
      const auto value = response.get_header_value("Content-Length");
      std::uint64_t length{};
      const auto* end = std::to_address(value.cend());
      const auto parsed = std::from_chars(value.data(), end, length);
      if (parsed.ec != std::errc{} || parsed.ptr != end)
        return refuse(Error::invalid_result);
      if (length > m_remaining) return refuse(Error::resource_exhausted);
    }
    return true;
  }
  const KubernetesHttpsControl& m_control;
  std::uint64_t m_remaining;
  bool m_header_seen{};
  std::optional<Error> m_failure;
  std::string m_body;
};
auto configure(httplib::SSLClient& client,
               const KubernetesHttpsControl& control, std::uint64_t maximum)
    -> void {
  const auto remaining =
      std::max(std::chrono::milliseconds{1}, control.remaining());
  client.set_connection_timeout(remaining);
  client.set_read_timeout(remaining);
  client.set_write_timeout(remaining);
  client.set_max_timeout(remaining);
  client.set_payload_max_length(static_cast<std::size_t>(maximum));
  client.set_follow_location(false);
  client.set_keep_alive(false);
  client.set_decompress(false);
  client.set_path_encode(false);
  client.set_proxy("", 0);
}
auto collect(httplib::SSLClient& client,
             const StaticKubernetesConfig& configuration,
             const domain::OpsObservationRequest& request,
             const KubernetesHttpsControl& control,
             domain::EventTimestamp started)
    -> std::expected<domain::OpsObservation, Error> {
  if (const auto error = control.failure()) return std::unexpected(*error);
  configure(client, control, request.limits.maximum_bytes);
  Capture capture{control, request.limits.maximum_bytes};
  httplib::Request wire;
  wire.method = "GET";
  wire.path = path(request);
  wire.headers.emplace("Accept", "application/json");
  wire.headers.emplace("Accept-Encoding", "identity");
  const auto material = configuration.tls_material();
  if (const auto* token =
          std::get_if<StaticKubernetesTokenView>(&material.authentication))
    wire.headers.emplace("Authorization",
                         "Bearer " + std::string{token->token});
  wire.response_handler = [&capture](const auto& response) {
    return capture.header(response);
  };
  wire.content_receiver = [&capture](const char* data, std::size_t length,
                                     std::uint64_t offset,
                                     std::uint64_t total) {
    return capture.body(data, length, offset, total);
  };
  httplib::Response response;
  auto error = httplib::Error::Success;
  const bool sent = client.send(wire, response, error);
  if (const auto stopped = control.failure()) return std::unexpected(*stopped);
  if (const auto failed = capture.failure()) return std::unexpected(*failed);
  if (!sent) return std::unexpected(transport_error(error));
  // The dependency may discard undeclared/prohibited trailers. Only retained
  // normalized fields are accounted here; this is not total-wire accounting.
  if (!capture.headers(response.trailers))
    return std::unexpected(capture.failure().value_or(Error::internal_failure));
  const auto completed = domain::EventTimestamp{
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())};
  auto observation = project_kubernetes_observation(
      request, started, completed, capture.text(), control.token());
  if (const auto stopped = control.failure()) return std::unexpected(*stopped);
  if (!observation) return observation;
  const bool excluded =
      KubernetesCredentialExclusion{material, control.token()}.contains(
          *observation);
  if (const auto stopped = control.failure()) return std::unexpected(*stopped);
  if (excluded) return std::unexpected(Error::invalid_result);
  observation->completed_at = domain::EventTimestamp{
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())};
  const auto validated =
      domain::validate_recorded_ops_observation(*observation);
  if (const auto stopped = control.failure()) return std::unexpected(*stopped);
  if (!validated) {
    if (validated.error().code ==
        domain::OpsObservationErrorCode::internal_failure)
      return std::unexpected(Error::internal_failure);
    if (validated.error().code ==
        domain::OpsObservationErrorCode::resource_exhausted)
      return std::unexpected(Error::resource_exhausted);
    return std::unexpected(Error::invalid_result);
  }
  return observation;
}
#endif
} // namespace
auto read_kubernetes_observation(const StaticKubernetesConfig& configuration,
                                 const domain::OpsObservationRequest& request,
                                 Clock::time_point deadline,
                                 domain::EventTimestamp started_at,
                                 std::stop_token stop) noexcept
    -> std::expected<domain::OpsObservation, Error> {
#if defined(__linux__)
  try {
    if (stop.stop_requested()) return std::unexpected(Error::cancelled);
    if (Clock::now() >= deadline) return std::unexpected(Error::timed_out);
    auto result = [&]() -> std::expected<domain::OpsObservation, Error> {
      KubernetesSigPipeBlock signal;
      if (!signal.valid()) return std::unexpected(Error::unavailable);
      const auto& endpoint = configuration.identity().endpoint;
      httplib::SSLClient client{endpoint.host, endpoint.port};
      KubernetesHttpsControl control{deadline, stop};
      auto prepared =
          prepare_kubernetes_tls(client, configuration.tls_material(), control);
      if (!prepared) return std::unexpected(prepared.error());
      control.start(client);
      return collect(client, configuration, request, control, started_at);
    }();
    // Teardown may retain the source slot too. Recheck after watcher/client
    // cleanup rather than publish an observation that expired during cleanup.
    if (stop.stop_requested()) return std::unexpected(Error::cancelled);
    if (Clock::now() >= deadline) return std::unexpected(Error::timed_out);
    return result;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::resource_exhausted);
  } catch (...) {
    return std::unexpected(Error::internal_failure);
  }
#else
  (void)configuration;
  (void)request;
  (void)deadline;
  (void)started_at;
  (void)stop;
  return std::unexpected(Error::unsupported);
#endif
}
} // namespace aiforge::adapters
