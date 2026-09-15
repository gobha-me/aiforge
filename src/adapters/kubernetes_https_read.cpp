#include "kubernetes_https_read.hpp"
#include "kubernetes_credential_exclusion.hpp"
#include "kubernetes_https_control.hpp"
#include "kubernetes_observation_projection.hpp"
#include "kubernetes_tls_context.hpp"
#include <aiforge/detail/utf8_text.hpp>
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
auto log_path(const domain::OpsObservationRequest& request,
              const domain::KubernetesPodIdentity& pod,
              const domain::KubernetesContainerIdentity& container)
    -> std::string {
  auto result =
      "/api/v1/namespaces/" + encoded(pod.namespace_name) + "/pods/" +
      encoded(pod.name) + "/log?container=" + encoded(container.name) +
      "&timestamps=true&tailLines=" +
      std::to_string(request.limits.maximum_log_lines) + "&sinceSeconds=" +
      std::to_string(request.limits.maximum_log_age.count()) +
      "&limitBytes=" + std::to_string(request.limits.maximum_log_bytes);
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
auto text_type(std::string_view input) -> bool {
  const auto value = lower(input);
  return value == "text/plain" || value == "text/plain; charset=utf-8" ||
         value == "text/plain;charset=utf-8";
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
  Capture(const KubernetesHttpsControl& control, std::uint64_t maximum,
          std::uint64_t maximum_body, bool json)
      : m_control(control), m_remaining(maximum),
        m_body_remaining(maximum_body), m_json(json) {}
  auto header(const httplib::Response& response) -> bool {
    if (m_header_seen) return refuse(Error::invalid_result);
    m_header_seen = true;
    if (!charge(response.version.size()) || !charge(3) ||
        !charge(response.reason.size()) || !headers(response.headers))
      return false;
    if (const auto error = status_error(response.status)) return refuse(*error);
    if (response.get_header_value_count("Content-Type") != 1 ||
        !(m_json ? json_type(response.get_header_value("Content-Type"))
                 : text_type(response.get_header_value("Content-Type"))))
      return refuse(Error::invalid_result);
    if (!content_framing(response)) return false;
    return true;
  }
  auto body(const char* data, std::size_t length, std::uint64_t offset,
            std::uint64_t) -> bool {
    if (!m_header_seen || offset != m_body.size())
      return refuse(Error::invalid_result);
    if (length > m_body_remaining || !charge(length))
      return refuse(Error::resource_exhausted);
    m_body_remaining -= length;
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
      if (length > m_remaining || length > m_body_remaining)
        return refuse(Error::resource_exhausted);
    }
    return true;
  }
  const KubernetesHttpsControl& m_control;
  std::uint64_t m_remaining;
  std::uint64_t m_body_remaining;
  bool m_json;
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
  Capture capture{control, request.limits.maximum_bytes,
                  request.limits.maximum_bytes, true};
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

auto digits(std::string_view value) -> std::optional<unsigned> {
  if (value.empty()) return {};
  unsigned result{};
  for (const char byte : value) {
    if (byte < '0' || byte > '9') return {};
    result = (result * 10U) + static_cast<unsigned>(byte - '0');
  }
  return result;
}
struct LogDateTime {
  unsigned year;
  unsigned month;
  unsigned day;
  unsigned hour;
  unsigned minute;
  unsigned second;
};
auto log_date_time(std::string_view value) -> std::optional<LogDateTime> {
  if (value.size() < 20 || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':')
    return {};
  const auto year = digits(value.substr(0, 4));
  const auto month = digits(value.substr(5, 2));
  const auto day = digits(value.substr(8, 2));
  const auto hour = digits(value.substr(11, 2));
  const auto minute = digits(value.substr(14, 2));
  const auto second = digits(value.substr(17, 2));
  if (!year || !month || !day || !hour || !minute || !second || *hour > 23 ||
      *minute > 59 || *second > 59)
    return {};
  return LogDateTime{*year, *month, *day, *hour, *minute, *second};
}
auto log_milliseconds(std::string_view value, std::size_t& offset)
    -> std::optional<unsigned> {
  if (offset >= value.size() || value[offset] != '.') return 0;
  const auto begin = ++offset;
  while (offset < value.size() && value[offset] >= '0' && value[offset] <= '9')
    ++offset;
  const auto fraction = value.substr(begin, offset - begin);
  if (fraction.empty() || fraction.size() > 9) return {};
  unsigned result{};
  for (std::size_t index{}; index < 3; ++index)
    result = result * 10U + (index < fraction.size()
                                 ? static_cast<unsigned>(fraction[index] - '0')
                                 : 0U);
  return result;
}
auto log_zone(std::string_view value, std::size_t offset)
    -> std::optional<std::chrono::minutes> {
  if (offset == value.size() - 1 && value[offset] == 'Z')
    return std::chrono::minutes{};
  if (value.size() - offset != 6 ||
      (value[offset] != '+' && value[offset] != '-') ||
      value[offset + 3] != ':')
    return {};
  const auto hours = digits(value.substr(offset + 1, 2));
  const auto minutes = digits(value.substr(offset + 4, 2));
  if (!hours || !minutes || *hours > 23 || *minutes > 59) return {};
  const auto total = static_cast<int>((*hours * 60U) + *minutes);
  return std::chrono::minutes{value[offset] == '-' ? -total : total};
}
auto log_timestamp(std::string_view value)
    -> std::optional<domain::EventTimestamp> {
  const auto fields = log_date_time(value);
  if (!fields) return {};
  const std::chrono::year_month_day date{
      std::chrono::year{static_cast<int>(fields->year)},
      std::chrono::month{fields->month}, std::chrono::day{fields->day}};
  if (!date.ok()) return {};
  std::size_t offset{19};
  const auto millis = log_milliseconds(value, offset);
  const auto zone = log_zone(value, offset);
  if (!millis || !zone) return {};
  const auto result = std::chrono::sys_days{date} +
                      std::chrono::hours{fields->hour} +
                      std::chrono::minutes{fields->minute} +
                      std::chrono::seconds{fields->second} +
                      std::chrono::milliseconds{*millis} - *zone;
  if (result.time_since_epoch().count() < 0) return {};
  return domain::EventTimestamp{
      std::chrono::duration_cast<std::chrono::milliseconds>(
          result.time_since_epoch())};
}
auto parse_log_lines(std::string_view bytes,
                     const domain::OpsObservationRequest& request,
                     std::stop_token stop)
    -> std::expected<std::vector<domain::OpsLogLine>, Error> {
  std::vector<domain::OpsLogLine> result;
  std::uint64_t text_bytes{};
  while (!bytes.empty()) {
    if (stop.stop_requested()) return std::unexpected(Error::cancelled);
    const auto newline = bytes.find('\n');
    auto line = bytes.substr(0, newline);
    bytes = newline == std::string_view::npos ? std::string_view{}
                                              : bytes.substr(newline + 1);
    if (line.ends_with('\r') ||
        result.size() == request.limits.maximum_log_lines)
      return std::unexpected(result.size() == request.limits.maximum_log_lines
                                 ? Error::resource_exhausted
                                 : Error::invalid_result);
    const auto split = line.find(' ');
    if (split == std::string_view::npos)
      return std::unexpected(Error::invalid_result);
    const auto timestamp = log_timestamp(line.substr(0, split));
    const auto text = line.substr(split + 1);
    if (!timestamp || !detail::is_safe_utf8_text(text) ||
        std::ranges::any_of(
            text,
            [](unsigned char byte) { return byte < 32 || byte == 127; }) ||
        text.size() > request.limits.maximum_log_bytes - text_bytes)
      return std::unexpected(Error::invalid_result);
    text_bytes += text.size();
    result.push_back({*timestamp, std::string{text}});
  }
  return result;
}
auto collect_log_bytes(httplib::SSLClient& client,
                       const StaticKubernetesConfig& configuration,
                       const domain::OpsObservationRequest& request,
                       const domain::KubernetesPodIdentity& pod,
                       const domain::KubernetesContainerIdentity& container,
                       const KubernetesHttpsControl& control)
    -> std::expected<std::vector<domain::OpsLogLine>, Error> {
  if (const auto error = control.failure()) return std::unexpected(*error);
  configure(client, control, request.limits.maximum_log_bytes);
  Capture capture{control, request.limits.maximum_bytes,
                  request.limits.maximum_log_bytes, false};
  httplib::Request wire;
  wire.method = "GET";
  wire.path = log_path(request, pod, container);
  wire.headers.emplace("Accept", "text/plain");
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
  if (!capture.headers(response.trailers))
    return std::unexpected(capture.failure().value_or(Error::internal_failure));
  return parse_log_lines(capture.text(), request, control.token());
}
auto read_log_bytes(const StaticKubernetesConfig& configuration,
                    const domain::OpsObservationRequest& request,
                    const domain::KubernetesPodIdentity& pod,
                    const domain::KubernetesContainerIdentity& container,
                    Clock::time_point deadline, std::stop_token stop)
    -> std::expected<std::vector<domain::OpsLogLine>, Error> {
  if (stop.stop_requested()) return std::unexpected(Error::cancelled);
  if (Clock::now() >= deadline) return std::unexpected(Error::timed_out);
  auto result = [&]() -> std::expected<std::vector<domain::OpsLogLine>, Error> {
    KubernetesSigPipeBlock signal;
    if (!signal.valid()) return std::unexpected(Error::unavailable);
    const auto& endpoint = configuration.identity().endpoint;
    httplib::SSLClient client{endpoint.host, endpoint.port};
    KubernetesHttpsControl control{deadline, stop};
    auto prepared =
        prepare_kubernetes_tls(client, configuration.tls_material(), control);
    if (!prepared) return std::unexpected(prepared.error());
    control.start(client);
    return collect_log_bytes(client, configuration, request, pod, container,
                             control);
  }();
  if (stop.stop_requested()) return std::unexpected(Error::cancelled);
  if (Clock::now() >= deadline) return std::unexpected(Error::timed_out);
  return result;
}
auto attests_log_source(const domain::KubernetesPodObservation& pod,
                        const domain::KubernetesPodIdentity& expected) -> bool {
  if (pod.identity.namespace_name != expected.namespace_name ||
      pod.identity.name != expected.name || pod.identity.uid != expected.uid ||
      !expected.container)
    return false;
  const auto found =
      std::ranges::find(pod.containers, expected.container->name,
                        &domain::KubernetesContainerObservation::name);
  return found != pod.containers.end() &&
         found->runtime_identity == expected.container->runtime_identity;
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

auto read_kubernetes_log_observation(
    const StaticKubernetesConfig& configuration,
    const domain::OpsObservationRequest& request, Clock::time_point deadline,
    domain::EventTimestamp started_at, std::stop_token stop,
    const KubernetesLogReadTestHooks* test_hooks) noexcept
    -> std::expected<domain::OpsObservation, Error> {
#if defined(__linux__)
  try {
    const auto* expected =
        std::get_if<domain::KubernetesPodIdentity>(&request.resource);
    if (request.operation !=
            domain::OpsObservationOperation::kubernetes_pod_logs ||
        expected == nullptr || !expected->container)
      return std::unexpected(Error::invalid_result);
    const auto& container = *expected->container;
    auto health_request = request;
    health_request.operation =
        domain::OpsObservationOperation::kubernetes_pod_health;
    std::get<domain::KubernetesPodIdentity>(health_request.resource)
        .container.reset();
    auto before = read_kubernetes_observation(configuration, health_request,
                                              deadline, started_at, stop);
    if (!before) return std::unexpected(before.error());
    const auto& before_pod =
        std::get<domain::KubernetesPodObservation>(before->payload);
    if (!attests_log_source(before_pod, *expected))
      return std::unexpected(Error::source_changed);
    auto lines = read_log_bytes(configuration, request, *expected, container,
                                deadline, stop);
    if (!lines) return std::unexpected(lines.error());
    const auto post_started = domain::EventTimestamp{
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())};
    auto after = read_kubernetes_observation(configuration, health_request,
                                             deadline, post_started, stop);
    if (!after) return std::unexpected(after.error());
    const auto& after_pod =
        std::get<domain::KubernetesPodObservation>(after->payload);
    if (!attests_log_source(after_pod, *expected))
      return std::unexpected(Error::source_changed);
    domain::OpsObservation result{
        request,
        started_at,
        after->completed_at,
        domain::OpsObservationCompleteness::complete,
        0,
        0,
        {},
        domain::OpsLogObservation{request.resource, std::move(*lines)}};
    const auto exclusion =
        KubernetesCredentialExclusion{configuration.tls_material(), stop};
    if (stop.stop_requested()) return std::unexpected(Error::cancelled);
    if (test_hooks != nullptr && test_hooks->before_credential_exclusion)
      test_hooks->before_credential_exclusion();
    const bool excluded = exclusion.contains(result);
    if (stop.stop_requested()) return std::unexpected(Error::cancelled);
    if (Clock::now() >= deadline) return std::unexpected(Error::timed_out);
    if (excluded) return std::unexpected(Error::invalid_result);
    const auto checked = domain::validate_recorded_ops_observation(result);
    if (!checked) {
      if (checked.error().code ==
          domain::OpsObservationErrorCode::resource_exhausted)
        return std::unexpected(Error::resource_exhausted);
      return std::unexpected(Error::invalid_result);
    }
    if (stop.stop_requested()) return std::unexpected(Error::cancelled);
    if (Clock::now() >= deadline) return std::unexpected(Error::timed_out);
    return result;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::resource_exhausted);
  } catch (...) {
    return std::unexpected(Error::internal_failure);
  }
#else
  static_cast<void>(configuration);
  static_cast<void>(request);
  static_cast<void>(deadline);
  static_cast<void>(started_at);
  static_cast<void>(stop);
  return std::unexpected(Error::unsupported);
#endif
}
} // namespace aiforge::adapters
