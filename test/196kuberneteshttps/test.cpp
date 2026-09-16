#include "fixture.hpp"
#include "kubernetes_credential_exclusion.hpp"
#include "kubernetes_https_control.hpp"
#include "kubernetes_https_read.hpp"
#include "kubernetes_tls_context.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <charconv>
#include <ctime>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <pthread.h>
#include <signal.h>
#include <thread>
#include <venice/detail/httplib_contract.hpp>

namespace {
using namespace aiforge;
using namespace fixture;
using namespace std::chrono_literals;
template <class T> auto id(std::string text) -> T {
  return T::from(std::move(text)).value();
}
auto rejects(const std::expected<domain::OpsObservation, Error>& result,
             Error expected) -> void {
  REQUIRE_FALSE(result);
  CHECK(result.error() == expected);
}
auto observed(Peer& peer, Operation operation = Operation::kubernetes_workloads,
              bool certificate = false)
    -> std::expected<domain::OpsObservation, Error> {
  const auto owner = source(configuration(peer.port(), certificate));
  return owner->observe(request(*owner, operation));
}
auto wait_received(const Peer& peer) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!peer.received() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(2ms);
  return peer.received();
}
auto recent_timestamp(std::chrono::seconds age = {}) -> std::string {
  const std::time_t value =
      std::time(nullptr) - static_cast<std::time_t>(age.count());
  std::tm utc{};
  REQUIRE(gmtime_r(&value, &utc) != nullptr);
  std::array<char, 32> output{};
  REQUIRE(std::strftime(output.data(), output.size(), "%Y-%m-%dT%H:%M:%SZ",
                        &utc) != 0);
  return output.data();
}
auto recent_timestamp_with(std::string_view suffix) -> std::string {
  auto result = recent_timestamp(1s);
  result.pop_back();
  result += suffix;
  return result;
}
auto log_request(const Source& owner) -> domain::OpsObservationRequest {
  auto result = request(owner, Operation::kubernetes_pod_logs);
  result.resource = domain::KubernetesPodIdentity{
      "selected", "pod-one", id<domain::OpsResourceUid>("pod-uid"),
      domain::KubernetesContainerIdentity{"main", "containerd://one"}};
  return result;
}
auto log_peer(std::string text, std::string final_pod = pod()) -> Peer {
  return Peer::sequence({response(pod()), text_response(std::move(text)),
                         response(std::move(final_pod))});
}
} // namespace

TEST_CASE("Kubernetes metadata ownership and admission never initialize TLS") {
  auto config = configuration(1);
  const domain::OpsTargetBinding binding{
      id<domain::OpsTargetId>("target"),
      id<domain::OpsConfigurationRevision>("revision"), config.identity()};
  const auto before = native_calls();
  auto owner = Source::create(binding, std::move(config));
  REQUIRE(owner);
  CHECK(native_calls() == before);
  CHECK((*owner)->target_binding() == binding);
  CHECK((*owner)->guarantees_bound_read_only_observations());
  auto req = request(**owner);
  SECTION("pre-cancel") {
    std::stop_source stop;
    stop.request_stop();
    rejects((*owner)->observe(req, stop.get_token()), Error::cancelled);
  }
  SECTION("limits before overflow") {
    req.limits.timeout = std::chrono::milliseconds::max();
    rejects((*owner)->observe(req), Error::resource_exhausted);
  }
  SECTION("foreign revision") {
    req.target.configuration_revision =
        id<domain::OpsConfigurationRevision>("other");
    rejects((*owner)->observe(req), Error::source_changed);
  }
  SECTION("foreign namespace") {
    std::get<domain::KubernetesOpsIdentity>(req.target.identity)
        .namespace_name = "other";
    rejects((*owner)->observe(req), Error::source_changed);
  }
  SECTION("wrong resource") {
    req.resource = domain::KubernetesPodIdentity{
        "selected", "pod", id<domain::OpsResourceUid>("uid"), {}};
    rejects((*owner)->observe(req), Error::invalid_result);
  }
  SECTION("known token cannot enter request identity") {
    req.request_id = id<domain::OpsRequestId>(std::string{token});
    rejects((*owner)->observe(req), Error::invalid_result);
  }
  CHECK(native_calls() == before);
}

TEST_CASE("Kubernetes Pod logs require identity before and after one bounded "
          "exact-container read") {
  auto peer = log_peer(recent_timestamp() + " first line\n" +
                       recent_timestamp() + " second line\n");
  const auto owner = source(configuration(peer.port()));
  const auto req = log_request(*owner);
  const auto result = owner->observe(req);
  REQUIRE(result);
  const auto& logs = std::get<domain::OpsLogObservation>(result->payload);
  REQUIRE(logs.source == req.resource);
  REQUIRE(logs.lines.size() == 2);
  CHECK(logs.lines[0].text == "first line");
  CHECK(logs.lines[1].text == "second line");
  const auto lines = peer.request_lines();
  REQUIRE(lines.size() == 3);
  CHECK(lines[0] == "GET /api/v1/namespaces/selected/pods/pod-one HTTP/1.1");
  CHECK(lines[1] ==
        "GET /api/v1/namespaces/selected/pods/pod-one/log?container=main&"
        "timestamps=true&tailLines=200&sinceSeconds=600&limitBytes=32768 "
        "HTTP/1.1");
  CHECK(lines[2] == lines[0]);
}

TEST_CASE("Kubernetes Pod log source shape is rejected before native IO") {
  auto config = configuration(1);
  const auto owner = source(configuration(1));
  auto req = log_request(*owner);
  std::get<domain::KubernetesPodIdentity>(req.resource).container.reset();
  const auto before = native_calls();
  const auto result = adapters::read_kubernetes_log_observation(
      config, req, std::chrono::steady_clock::now() + 1s,
      domain::EventTimestamp{1ms}, {});
  rejects(result, Error::invalid_result);
  CHECK(native_calls() == before);
}

TEST_CASE("Kubernetes Pod log timestamps retain bounded RFC3339 forms") {
  const std::array valid{recent_timestamp(), recent_timestamp_with(".1Z"),
                         recent_timestamp_with(".123Z"),
                         recent_timestamp_with(".123456789Z"),
                         recent_timestamp_with("+00:00")};
  for (const auto& timestamp : valid) {
    auto peer = log_peer(timestamp + " retained\n");
    const auto owner = source(configuration(peer.port()));
    const auto result = owner->observe(log_request(*owner));
    REQUIRE(result);
    const auto& lines =
        std::get<domain::OpsLogObservation>(result->payload).lines;
    REQUIRE(lines.size() == 1);
    CHECK(lines.front().text == "retained");
  }

  const std::array malformed{
      recent_timestamp_with(".Z"), recent_timestamp_with(".1234567890Z"),
      recent_timestamp_with("+24:00"), recent_timestamp_with("+00:60"),
      recent_timestamp_with("")};
  for (const auto& timestamp : malformed) {
    auto peer = log_peer(timestamp + " discarded\n");
    const auto owner = source(configuration(peer.port()));
    rejects(owner->observe(log_request(*owner)), Error::invalid_result);
    CHECK(peer.accepted() == 2);
  }
}

TEST_CASE("Kubernetes Pod logs are discarded when exact identity changes") {
  auto changed = nlohmann::json::parse(pod());
  SECTION("Pod UID") {
    changed["metadata"]["uid"] = "replacement";
  }
  SECTION("container runtime") {
    changed["status"]["containerStatuses"][0]["containerID"] =
        "containerd://replacement";
  }
  auto peer = log_peer(recent_timestamp() + " private text\n", changed.dump());
  const auto owner = source(configuration(peer.port()));
  rejects(owner->observe(log_request(*owner)), Error::source_changed);
  CHECK(peer.accepted() == 3);
}

TEST_CASE("Kubernetes Pod logs are never fetched before exact identity is "
          "attested") {
  auto changed = nlohmann::json::parse(pod());
  SECTION("Pod UID") {
    changed["metadata"]["uid"] = "replacement";
  }
  SECTION("container runtime") {
    changed["status"]["containerStatuses"][0]["containerID"] =
        "containerd://replacement";
  }
  auto peer = Peer::sequence(
      {response(changed.dump()),
       text_response(recent_timestamp() + " must-not-be-fetched\n"),
       response(pod())});
  const auto owner = source(configuration(peer.port()));
  rejects(owner->observe(log_request(*owner)), Error::source_changed);
  CHECK(peer.accepted() == 1);
}

TEST_CASE("Kubernetes Pod log bytes fail closed on malformed or credential "
          "bearing content") {
  const std::array values{
      std::pair{std::string{"missing timestamp\n"}, Error::invalid_result},
      std::pair{std::string{"2000-01-01T00:00:00Z too old\n"},
                Error::invalid_result},
      std::pair{recent_timestamp() + " " + std::string{token} + "\n",
                Error::invalid_result},
      std::pair{recent_timestamp() + " -----BEGIN PRIVATE KEY-----\n",
                Error::invalid_result},
      std::pair{recent_timestamp() + " bad\tcontrol\n", Error::invalid_result}};
  for (const auto& [text, error] : values) {
    auto peer = log_peer(text);
    const auto owner = source(configuration(peer.port()));
    rejects(owner->observe(log_request(*owner)), error);
  }
}

TEST_CASE("Kubernetes Pod log HTTP metadata and framing fail closed") {
  const auto line = recent_timestamp() + " private text\n";
  SECTION("wrong content type") {
    auto peer =
        Peer::sequence({response(pod()), response(line), response(pod())});
    const auto owner = source(configuration(peer.port()));
    rejects(owner->observe(log_request(*owner)), Error::invalid_result);
    CHECK(peer.accepted() == 2);
  }
  SECTION("duplicate content type") {
    auto peer = Peer::sequence(
        {response(pod()), text_response(line, "Content-Type: text/html\r\n"),
         response(pod())});
    const auto owner = source(configuration(peer.port()));
    rejects(owner->observe(log_request(*owner)), Error::invalid_result);
    CHECK(peer.accepted() == 2);
  }
  SECTION("compression") {
    auto peer = Peer::sequence(
        {response(pod()), text_response(line, "Content-Encoding: gzip\r\n"),
         response(pod())});
    const auto owner = source(configuration(peer.port()));
    rejects(owner->observe(log_request(*owner)), Error::invalid_result);
    CHECK(peer.accepted() == 2);
  }
  SECTION("ambiguous framing") {
    auto peer = Peer::sequence(
        {response(pod()), text_response(line, "Transfer-Encoding: chunked\r\n"),
         response(pod())});
    const auto owner = source(configuration(peer.port()));
    CHECK_FALSE(owner->observe(log_request(*owner)));
    CHECK(peer.accepted() == 2);
  }
}

TEST_CASE("Kubernetes Pod log HTTP errors and redirects never fetch a third "
          "response") {
  const std::array cases{std::pair{401, Error::authentication_failed},
                         std::pair{403, Error::permission_denied},
                         std::pair{404, Error::source_changed},
                         std::pair{429, Error::unavailable},
                         std::pair{503, Error::unavailable}};
  for (const auto& [status, error] : cases) {
    auto peer = Peer::sequence(
        {response(pod()), text_response("private-provider-error", {}, status),
         response(pod())});
    const auto owner = source(configuration(peer.port()));
    rejects(owner->observe(log_request(*owner)), error);
    CHECK(peer.accepted() == 2);
  }
  SECTION("redirect") {
    Peer destination{{response("unexpected")}};
    auto peer =
        Peer::sequence({response(pod()),
                        text_response("",
                                      "Location: https://127.0.0.1:" +
                                          std::to_string(destination.port()) +
                                          "/unexpected\r\n",
                                      302),
                        response(pod())});
    const auto owner = source(configuration(peer.port()));
    rejects(owner->observe(log_request(*owner)), Error::trust_failed);
    CHECK(peer.accepted() == 2);
    CHECK(destination.accepted() == 0);
  }
}

TEST_CASE("Kubernetes Pod log byte and line ceilings reject excess") {
  SECTION("body bytes") {
    auto peer = log_peer(recent_timestamp() + " too-large\n");
    const auto owner = source(configuration(peer.port()));
    auto req = log_request(*owner);
    req.limits.maximum_log_bytes = 8;
    rejects(owner->observe(req), Error::resource_exhausted);
  }
  SECTION("lines") {
    auto peer =
        log_peer(recent_timestamp() + " one\n" + recent_timestamp() + " two\n");
    const auto owner = source(configuration(peer.port()));
    auto req = log_request(*owner);
    req.limits.maximum_log_lines = 1;
    rejects(owner->observe(req), Error::resource_exhausted);
  }
}

TEST_CASE("Kubernetes Pod log byte and line ceilings accept exact bounds") {
  SECTION("body bytes") {
    const auto body = recent_timestamp() + " exact\n";
    auto peer = log_peer(body);
    const auto owner = source(configuration(peer.port()));
    auto req = log_request(*owner);
    req.limits.maximum_log_bytes = body.size();
    REQUIRE(owner->observe(req));
  }
  SECTION("lines") {
    auto peer =
        log_peer(recent_timestamp() + " one\n" + recent_timestamp() + " two\n");
    const auto owner = source(configuration(peer.port()));
    auto req = log_request(*owner);
    req.limits.maximum_log_lines = 2;
    REQUIRE(owner->observe(req));
  }
}

TEST_CASE("Kubernetes Pod log cancellation covers the middle response") {
  auto peer = Peer::sequence({response(pod()),
                              text_response(recent_timestamp() + " hidden\n"),
                              response(pod())},
                             2);
  const auto owner = source(configuration(peer.port()));
  const auto req = log_request(*owner);
  std::optional<std::expected<domain::OpsObservation, Error>> result;
  std::jthread operation{
      [&](std::stop_token stop) { result.emplace(owner->observe(req, stop)); }};
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (peer.accepted() < 2 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(2ms);
  REQUIRE(peer.accepted() == 2);
  operation.request_stop();
  operation.join();
  REQUIRE(result);
  rejects(*result, Error::cancelled);
  CHECK(peer.accepted() == 2);
}

TEST_CASE("Kubernetes Pod log deadline covers the complete three-read path") {
  auto peer = Peer::sequence({response(pod()),
                              text_response(recent_timestamp() + " hidden\n"),
                              response(pod())},
                             2);
  const auto owner = source(configuration(peer.port()));
  auto req = log_request(*owner);
  req.limits.timeout = 80ms;
  rejects(owner->observe(req), Error::timed_out);
  CHECK(peer.accepted() == 2);
}

TEST_CASE("Kubernetes Pod log cancellation covers the final identity read") {
  auto peer = Peer::sequence({response(pod()),
                              text_response(recent_timestamp() + " hidden\n"),
                              response(pod())},
                             3);
  const auto owner = source(configuration(peer.port()));
  const auto req = log_request(*owner);
  std::optional<std::expected<domain::OpsObservation, Error>> result;
  std::jthread operation{
      [&](std::stop_token stop) { result.emplace(owner->observe(req, stop)); }};
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (peer.accepted() < 3 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(2ms);
  REQUIRE(peer.accepted() == 3);
  operation.request_stop();
  operation.join();
  REQUIRE(result);
  rejects(*result, Error::cancelled);
}

TEST_CASE("Kubernetes Pod log cancellation during final credential exclusion "
          "never publishes text") {
  auto peer = log_peer(recent_timestamp() + " private text\n");
  const auto owner = source(configuration(peer.port()));
  const auto req = log_request(*owner);
  auto config = configuration(peer.port());
  std::stop_source stop;
  adapters::KubernetesLogReadTestHooks hooks{[&] { stop.request_stop(); }};
  const auto started = domain::EventTimestamp{
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())};
  const auto result = adapters::read_kubernetes_log_observation(
      config, req, std::chrono::steady_clock::now() + 2s, started,
      stop.get_token(), &hooks);
  rejects(result, Error::cancelled);
  CHECK(peer.accepted() == 3);
}

TEST_CASE("Kubernetes Pod log deadline covers the final identity read") {
  auto peer = Peer::sequence({response(pod()),
                              text_response(recent_timestamp() + " hidden\n"),
                              response(pod())},
                             3);
  const auto owner = source(configuration(peer.port()));
  auto req = log_request(*owner);
  req.limits.timeout = 80ms;
  rejects(owner->observe(req), Error::timed_out);
  CHECK(peer.accepted() == 3);
}

TEST_CASE("Kubernetes factory refuses foreign and credential-bearing "
          "identities without TLS") {
  auto config = configuration(1);
  domain::OpsTargetBinding binding{
      id<domain::OpsTargetId>("target"),
      id<domain::OpsConfigurationRevision>("revision"), config.identity()};
  SECTION("foreign") {
    std::get<domain::KubernetesOpsIdentity>(binding.identity).context_name =
        "other";
  }
  SECTION("token") {
    binding.target_id = id<domain::OpsTargetId>(std::string{token});
  }
  const auto before = native_calls();
  CHECK_FALSE(Source::create(binding, std::move(config)));
  CHECK(native_calls() == before);
}

TEST_CASE(
    "Structurally admitted invalid TLS material fails only in the worker") {
  Peer peer{{response(list())}};
  const auto owner = source(configuration(
      peer.port(), false,
      "-----BEGIN CERTIFICATE-----\nQQ==\n-----END CERTIFICATE-----\n"));
  const auto before = native_calls();
  CHECK(owner->target_binding().target_id.value() == "target");
  CHECK(native_calls() == before);
  rejects(owner->observe(request(*owner)), Error::trust_failed);
  CHECK(peer.accepted() == 0);
}

TEST_CASE("Kubernetes TLS refuses untrusted wrong-host expired and invalid "
          "client identities") {
  SECTION("unrelated root") {
    Peer peer{{response(list())}};
    const auto owner =
        source(configuration(peer.port(), false, credentials().unrelated_ca));
    rejects(owner->observe(request(*owner)), Error::trust_failed);
    CHECK_FALSE(peer.received());
  }
  SECTION("host verification") {
    Peer peer{{response(list()), true}};
    rejects(observed(peer), Error::trust_failed);
    CHECK_FALSE(peer.received());
  }
  SECTION("expired") {
    Peer peer{{response(list()), false, true}};
    rejects(observed(peer), Error::trust_failed);
    CHECK_FALSE(peer.received());
  }
  SECTION("bad CA block after valid") {
    Peer peer{{response(list())}};
    const auto owner = source(configuration(
        peer.port(), false,
        credentials().server_ca +
            "-----BEGIN CERTIFICATE-----\nQQ==\n-----END CERTIFICATE-----\n"));
    rejects(owner->observe(request(*owner)), Error::trust_failed);
    CHECK(peer.accepted() == 0);
  }
  SECTION("client mismatched key") {
    Peer peer{{response(list()), false, false, true}};
    const auto owner = source(configuration(peer.port(), true, {}, {},
                                            credentials().wrong_client_key));
    rejects(owner->observe(request(*owner)), Error::authentication_failed);
    CHECK(peer.accepted() == 0);
  }
  SECTION("missing client intermediate") {
    Peer peer{{response(list()), false, false, true}};
    const auto owner =
        source(configuration(peer.port(), true, {}, credentials().client_leaf));
    CHECK_FALSE(owner->observe(request(*owner)));
    CHECK_FALSE(peer.received());
  }
  SECTION("unrelated appended client issuer") {
    Peer peer{{response(list()), false, false, true}};
    const auto owner = source(
        configuration(peer.port(), true, {},
                      credentials().client_leaf + credentials().server_ca));
    rejects(owner->observe(request(*owner)), Error::authentication_failed);
    CHECK(peer.accepted() == 0);
  }
}

TEST_CASE("Kubernetes HTTP refusal never publishes an error body") {
  const std::array cases{std::pair{401, Error::authentication_failed},
                         std::pair{403, Error::permission_denied},
                         std::pair{404, Error::source_changed},
                         std::pair{429, Error::unavailable},
                         std::pair{503, Error::unavailable}};
  for (const auto& [status, error] : cases) {
    Peer peer{{response("private-provider-error", {}, status)}};
    rejects(observed(peer), error);
  }
  SECTION("redirect") {
    Peer destination{{response(list())}};
    Peer peer{
        {response("",
                  "Location: https://127.0.0.1:" +
                      std::to_string(destination.port()) + "/unexpected\r\n",
                  302)}};
    rejects(observed(peer), Error::trust_failed);
    CHECK(destination.accepted() == 0);
  }
  SECTION("duplicate content type") {
    Peer peer{{response(list(), "Content-Type: text/plain\r\n")}};
    rejects(observed(peer), Error::invalid_result);
  }
  SECTION("compression") {
    Peer peer{{response(list(), "Content-Encoding: gzip\r\n")}};
    rejects(observed(peer), Error::invalid_result);
  }
  SECTION("ambiguous framing") {
    Peer peer{{response(list(), "Transfer-Encoding: chunked\r\n")}};
    CHECK_FALSE(observed(peer));
  }
}

TEST_CASE(
    "Kubernetes cumulative normalized response budget has an exact boundary") {
  const auto body = list();
  const auto header_length =
      std::string{"HTTP/1.1"}.size() + 3 + std::string{"Result"}.size() +
      std::string{"Content-Typeapplication/jsonContent-LengthConnectionclose"}
          .size() +
      std::to_string(body.size()).size();
  for (const bool over : {false, true}) {
    Peer peer{{response(body)}};
    const auto owner = source(configuration(peer.port()));
    auto req = request(*owner);
    req.limits.maximum_bytes = body.size() + header_length - (over ? 1 : 0);
    const auto result = owner->observe(req);
    if (over)
      rejects(result, Error::resource_exhausted);
    else
      REQUIRE(result);
  }
}

TEST_CASE("Kubernetes chunked capture validates tails and retained trailers") {
  const auto body = list();
  std::array<char, 32> length{};
  const auto encoded = std::to_chars(
      length.data(), length.data() + length.size(), body.size(), 16);
  const auto chunk =
      std::string{length.data(), encoded.ptr} + "\r\n" + body + "\r\n";
  SECTION("complete chunked") {
    Peer peer{{"HTTP/1.1 200 OK\r\nContent-Type: "
               "application/json\r\nTransfer-Encoding: chunked\r\nConnection: "
               "close\r\n\r\n" +
               chunk + "0\r\n\r\n"}};
    REQUIRE(observed(peer));
  }
  SECTION("malformed after valid body") {
    Peer peer{{"HTTP/1.1 200 OK\r\nContent-Type: "
               "application/json\r\nTransfer-Encoding: chunked\r\n\r\n" +
               chunk + "INVALID\r\n"}};
    CHECK_FALSE(observed(peer));
  }
  SECTION("retained trailer pushes captured budget over") {
    Peer peer{{"HTTP/1.1 200 OK\r\nContent-Type: "
               "application/json\r\nTransfer-Encoding: chunked\r\nTrailer: "
               "X-Fixture\r\n\r\n" +
               chunk + "0\r\nX-Fixture: " + std::string(512, 'x') +
               "\r\n\r\n"}};
    const auto owner = source(configuration(peer.port()));
    auto req = request(*owner);
    req.limits.maximum_bytes = body.size() + 200;
    rejects(owner->observe(req), Error::resource_exhausted);
  }
}

TEST_CASE("Kubernetes projection rejects changed identity and reflected known "
          "credentials") {
  auto value = nlohmann::json::parse(pod());
  SECTION("wrong UID") {
    value["metadata"]["uid"] = "other";
  }
  SECTION("wrong namespace") {
    value["metadata"]["namespace"] = "other";
  }
  SECTION("wrong API group") {
    value["apiVersion"] = "apps/v1";
  }
  SECTION("reflected token in version") {
    value["metadata"]["resourceVersion"] = token;
  }
  SECTION("decoded token reflection") {
    value["status"]["containerStatuses"][0]["containerID"] =
        "runtime://" + std::string{token};
  }
  Peer peer{{response(value.dump())}};
  CHECK_FALSE(observed(peer, Operation::kubernetes_pod_health));
}

TEST_CASE(
    "Kubernetes cancellation and one deadline include stalled TLS and body") {
  SECTION("handshake timeout") {
    Peer peer{{response(list()), false, false, false, true}};
    const auto owner = source(configuration(peer.port()));
    auto req = request(*owner);
    req.limits.timeout = 50ms;
    rejects(owner->observe(req), Error::timed_out);
  }
  SECTION("body timeout") {
    Peer peer{{response(list()), false, false, false, false, true}};
    const auto owner = source(configuration(peer.port()));
    auto req = request(*owner);
    req.limits.timeout = 80ms;
    rejects(owner->observe(req), Error::timed_out);
  }
  SECTION("nonjoining stop request and caller signal preservation") {
    struct RestoreDisposition {
      struct sigaction saved{};
      bool active{};
      ~RestoreDisposition() {
        if (active) (void)sigaction(SIGPIPE, &saved, nullptr);
      }
    } disposition;
    REQUIRE(sigaction(SIGPIPE, nullptr, &disposition.saved) == 0);
    struct sigaction defaults{};
    defaults.sa_handler = SIG_DFL;
    REQUIRE(sigemptyset(&defaults.sa_mask) == 0);
    REQUIRE(sigaction(SIGPIPE, &defaults, nullptr) == 0);
    disposition.active = true;
    struct sigaction before{};
    REQUIRE(sigaction(SIGPIPE, nullptr, &before) == 0);
    Peer peer{{response(list()), false, false, false, false, true}};
    const auto owner = source(configuration(peer.port()));
    auto req = request(*owner);
    std::optional<std::expected<domain::OpsObservation, Error>> result;
    std::jthread operation{[&](std::stop_token stop) {
      result.emplace(owner->observe(req, stop));
    }};
    REQUIRE(wait_received(peer));
    const auto began = std::chrono::steady_clock::now();
    operation.request_stop();
    CHECK(std::chrono::steady_clock::now() - began < 100ms);
    operation.join();
    REQUIRE(result);
    rejects(*result, Error::cancelled);
    struct sigaction after{};
    REQUIRE(sigaction(SIGPIPE, nullptr, &after) == 0);
    CHECK(after.sa_handler == before.sa_handler);
  }
}

TEST_CASE("Kubernetes SIGPIPE scope preserves a caller-owned pending signal") {
  sigset_t only{}, saved{};
  sigemptyset(&only);
  sigaddset(&only, SIGPIPE);
  REQUIRE(pthread_sigmask(SIG_BLOCK, &only, &saved) == 0);
  struct Restore {
    sigset_t mask;
    sigset_t signal;
    ~Restore() {
      const timespec zero{};
      (void)sigtimedwait(&signal, nullptr, &zero);
      (void)pthread_sigmask(SIG_SETMASK, &mask, nullptr);
    }
  } restore{saved, only};
  REQUIRE(pthread_kill(pthread_self(), SIGPIPE) == 0);
  {
    adapters::KubernetesSigPipeBlock block;
    REQUIRE(block.valid());
  }
  sigset_t pending{};
  REQUIRE(sigpending(&pending) == 0);
  CHECK(sigismember(&pending, SIGPIPE) == 1);
}

TEST_CASE("Kubernetes fixed GET smoke uses checked TLS full client chain and "
          "exact selectors") {
  SECTION("inventory bearer") {
    Peer peer{{response(list())}};
    const auto result = observed(peer);
    REQUIRE(result);
    CHECK(peer.authorized());
    CHECK(peer.standard_headers());
    CHECK(peer.request_line() ==
          "GET /api/v1/namespaces/selected/pods?limit=256 HTTP/1.1");
    CHECK(result->source_version == "list-rv");
  }
  SECTION("full client chain") {
    Peer peer{{response(pod()), false, false, true}};
    const auto result = observed(peer, Operation::kubernetes_pod_health, true);
    REQUIRE(result);
    CHECK(peer.authorized());
    CHECK(peer.request_line() ==
          "GET /api/v1/namespaces/selected/pods/pod-one HTTP/1.1");
  }
  SECTION("namespace events") {
    Peer peer{{response(list(true))}};
    const auto result = observed(peer, Operation::kubernetes_events);
    REQUIRE(result);
    CHECK(std::get<domain::KubernetesEventsObservation>(result->payload)
              .events.empty());
    CHECK(peer.request_line() ==
          "GET /api/v1/namespaces/selected/events?limit=256 HTTP/1.1");
  }
  SECTION("exact UID is one selector value") {
    Peer peer{{response(list(true))}};
    const auto owner = source(configuration(peer.port()));
    auto req = request(*owner, Operation::kubernetes_events);
    req.resource = domain::KubernetesPodIdentity{
        "selected", "pod-one", id<domain::OpsResourceUid>("opaque,a=b\\c"), {}};
    REQUIRE(owner->observe(req));
    CHECK(peer.request_line() ==
          "GET "
          "/api/v1/namespaces/selected/"
          "events?limit=256&fieldSelector=involvedObject.kind%3DPod%"
          "2CinvolvedObject.namespace%3Dselected%2CinvolvedObject.name%3Dpod-"
          "one%2CinvolvedObject.uid%3Dopaque%5C%2Ca%5C%3Db%5C%5Cc HTTP/1.1");
  }
}

TEST_CASE("Concurrent Kubernetes reads retain independent TLS clients on one "
          "immutable source") {
  Peer peer{{response(list()), false, false, false, false, false, 2}};
  const auto owner = source(configuration(peer.port()));
  const auto req = request(*owner);
  std::optional<std::expected<domain::OpsObservation, Error>> first;
  std::optional<std::expected<domain::OpsObservation, Error>> second;
  const auto before = native_calls();
  std::jthread one{
      [&](std::stop_token stop) { first.emplace(owner->observe(req, stop)); }};
  std::jthread two{
      [&](std::stop_token stop) { second.emplace(owner->observe(req, stop)); }};
  one.join();
  two.join();
  REQUIRE(first);
  REQUIRE(second);
  REQUIRE(*first);
  REQUIRE(*second);
  CHECK(peer.accepted() == 2);
  CHECK(native_calls() >= before + 4);
  CHECK((*first)->request == req);
  CHECK((*second)->request == req);
}

TEST_CASE("The original Kubernetes deadline refuses before any new TLS stage") {
  auto config = configuration(1);
  const auto owner = source(configuration(1));
  const auto req = request(*owner);
  const auto before = native_calls();
  const auto result = adapters::read_kubernetes_observation(
      config, req, std::chrono::steady_clock::now() - 1ms,
      domain::EventTimestamp{1ms}, {});
  rejects(result, Error::timed_out);
  CHECK(native_calls() == before);
}

TEST_CASE(
    "The native Kubernetes key decoder refuses unexpected BIO remainder") {
  const auto& values = credentials();
  const auto key = values.client_key + "unexpected-private-tail";
  adapters::StaticKubernetesTlsView material{
      values.server_ca, adapters::StaticKubernetesClientCertificateView{
                            values.client_chain, key}};
  adapters::KubernetesSigPipeBlock signal;
  REQUIRE(signal.valid());
  httplib::SSLClient client{"127.0.0.1", 1};
  adapters::KubernetesHttpsControl control{
      std::chrono::steady_clock::now() + 1s, {}};
  const auto result =
      adapters::prepare_kubernetes_tls(client, material, control);
  REQUIRE_FALSE(result);
  CHECK(result.error() == Error::authentication_failed);
}

TEST_CASE("Kubernetes native parser header counts and unframed bodies keep "
          "their bounds") {
  SECTION("header count exact and one over") {
    for (const unsigned count : {97U, 98U}) {
      std::string headers;
      for (unsigned i = 0; i < count; ++i)
        headers += "X-Fixture: value\r\n";
      Peer peer{{response(list(), headers)}};
      const auto result = observed(peer);
      if (count == 97)
        REQUIRE(result);
      else
        CHECK_FALSE(result);
    }
  }
  SECTION("one over canonical header line") {
    Peer peer{
        {response(list(), "X-Fixture: " + std::string(8192, 'x') + "\r\n")}};
    CHECK_FALSE(observed(peer));
  }
  SECTION("EOF body is bounded without Content-Length") {
    Peer peer{{"HTTP/1.1 200 OK\r\nContent-Type: "
               "application/json\r\nConnection: close\r\n\r\n" +
               list()}};
    REQUIRE(observed(peer));
  }
  SECTION("wrong content type") {
    Peer peer{{"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: "
               "close\r\n\r\n" +
               list()}};
    rejects(observed(peer), Error::invalid_result);
  }
}

TEST_CASE("Kubernetes continued inventory stays partial and ignored credential "
          "text is dropped") {
  auto value = nlohmann::json::parse(list());
  value["metadata"]["continue"] = "opaque-next-page";
  value["items"][0]["status"]["message"] = token;
  Peer peer{{response(value.dump())}};
  const auto result = observed(peer);
  REQUIRE(result);
  CHECK(result->completeness == domain::OpsObservationCompleteness::partial);
  CHECK_FALSE(result->omitted_entries);
  CHECK(peer.accepted() == 1);
}
