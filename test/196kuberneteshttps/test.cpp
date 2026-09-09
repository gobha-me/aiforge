#include "fixture.hpp"
#include "kubernetes_credential_exclusion.hpp"
#include "kubernetes_https_control.hpp"
#include "kubernetes_https_read.hpp"
#include "kubernetes_tls_context.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <charconv>
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
  SECTION("log support stays closed") {
    req.operation = Operation::kubernetes_pod_logs;
    req.resource = domain::KubernetesPodIdentity{
        "selected", "pod", id<domain::OpsResourceUid>("uid"),
        domain::KubernetesContainerIdentity{"main", "containerd://one"}};
    rejects((*owner)->observe(req), Error::unsupported);
  }
  SECTION("known token cannot enter request identity") {
    req.request_id = id<domain::OpsRequestId>(std::string{token});
    rejects((*owner)->observe(req), Error::invalid_result);
  }
  CHECK(native_calls() == before);
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
