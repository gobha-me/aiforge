#include "static_kubernetes_config_parser.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <type_traits>

namespace {
using namespace aiforge;
using Config = adapters::StaticKubernetesConfig;
using Syntax = adapters::StaticKubernetesSyntax;
using Failure = adapters::StaticKubernetesConfigFailure;
namespace cfg = adapters::static_kubernetes_detail;
using Json = nlohmann::json;
// Deliberately synthetic PEM envelopes: structural parser fixtures, not valid
// certificates or private keys. No actual cluster or authentication is used.
constexpr auto ca = "LS0tLS1CRUdJTiBDRVJUSUZJQ0FURS0tLS0tCkFRSUQKLS0tLS1FTkQgQ0"
                    "VSVElGSUNBVEUtLS0tLQo=";
constexpr auto key = "LS0tLS1CRUdJTiBQUklWQVRFIEtFWS0tLS0tCkJBVUcKLS0tLS1FTkQgU"
                     "FJJVkFURSBLRVktLS0tLQo=";
auto document() -> Json {
  return Json{
      {"apiVersion", "v1"},
      {"kind", "Config"},
      {"clusters", Json::array({{{"name", "cluster-one"},
                                 {"cluster",
                                  {{"server", "https://fixture.invalid:6443"},
                                   {"certificate-authority-data", ca}}}}})},
      {"users", Json::array({{{"name", "user-one"},
                              {"user", {{"token", "synthetic-token"}}}}})},
      {"contexts", Json::array({{{"name", "context-one"},
                                 {"context",
                                  {{"cluster", "cluster-one"},
                                   {"user", "user-one"},
                                   {"namespace", "old-default"}}}}})},
      {"current-context", "context-one"},
      {"preferences", Json::object()}};
}
auto yaml_document(std::string_view token = "synthetic-token") -> std::string {
  return "apiVersion: v1\nkind: Config\nclusters:\n- name: cluster-one\n  "
         "cluster:\n    server: https://fixture.invalid:6443\n    "
         "certificate-authority-data: " +
         std::string{ca} + "\nusers:\n- name: user-one\n  user:\n    token: " +
         std::string{token} +
         "\ncontexts:\n- name: context-one\n  context:\n    cluster: "
         "cluster-one\n    user: user-one\n    namespace: "
         "old-default\ncurrent-context: context-one\n";
}
auto parse(std::string_view text, Syntax syntax = Syntax::json,
           std::string_view context = "context-one",
           std::string_view ns = "selected") {
  return Config::parse(text, syntax, context, ns);
}
auto refused(const Json& value) -> void {
  const auto text = value.dump();
  CHECK_FALSE(parse(text));
  CHECK_FALSE(parse(text, Syntax::yaml));
}
auto rejects(const std::function<void()>& operation, Failure failure) -> void {
  try {
    operation();
    FAIL("expected fixed parser rejection");
  } catch (const cfg::Rejected& error) {
    CHECK(error.failure == failure);
  }
}
} // namespace

TEST_CASE("static kubeconfig rejects missing and duplicate schema fields") {
  const std::array required{"/apiVersion",
                            "/kind",
                            "/clusters",
                            "/users",
                            "/contexts",
                            "/clusters/0/name",
                            "/clusters/0/cluster",
                            "/clusters/0/cluster/server",
                            "/clusters/0/cluster/certificate-authority-data",
                            "/users/0/name",
                            "/users/0/user",
                            "/users/0/user/token",
                            "/contexts/0/name",
                            "/contexts/0/context",
                            "/contexts/0/context/cluster",
                            "/contexts/0/context/user"};
  for (const auto path : required) {
    DYNAMIC_SECTION("missing " << path) {
      auto value = document();
      const Json::json_pointer pointer{path};
      value.at(pointer.parent_pointer()).erase(pointer.back());
      refused(value);
    }
  }
  for (const auto list : {"clusters", "users", "contexts"}) {
    auto value = document();
    value[list].push_back(value[list][0]);
    refused(value);
  }
  auto duplicate = document().dump();
  duplicate.insert(1, "\"apiVersion\":\"v1\",");
  CHECK_FALSE(parse(duplicate));
  CHECK_FALSE(parse(duplicate, Syntax::yaml));
  auto escaped = document().dump();
  escaped.insert(1, "\"api\\u0056ersion\":\"v1\",");
  CHECK_FALSE(parse(escaped));
  CHECK_FALSE(parse(escaped, Syntax::yaml));
  for (const auto key_name :
       {"name", "server", "token", "cluster", "namespace"}) {
    auto text = document().dump();
    const auto needle = "\"" + std::string{key_name} + "\":";
    const auto offset = text.find(needle);
    REQUIRE(offset != std::string::npos);
    text.insert(offset, needle + "\"synthetic-duplicate\",");
    CHECK_FALSE(parse(text));
    CHECK_FALSE(parse(text, Syntax::yaml));
  }
}

TEST_CASE("static kubeconfig rejects unknown fields at every mapping including "
          "unselected") {
  for (const auto path :
       {"", "/preferences", "/clusters/0", "/clusters/0/cluster", "/users/0",
        "/users/0/user", "/contexts/0", "/contexts/0/context"}) {
    auto value = document();
    value[Json::json_pointer{path}]["synthetic-secret-key"] =
        "synthetic-secret-value";
    refused(value);
  }
  for (const auto field :
       {"exec", "auth-provider", "tokenFile", "username", "password", "as",
        "as-groups", "extensions", "client-certificate", "client-key"}) {
    auto value = document();
    auto other = value["users"][0];
    other["name"] = "unselected-user";
    other["user"][field] = "synthetic-secret";
    value["users"].push_back(other);
    refused(value);
  }
  for (const auto field :
       {"certificate-authority", "proxy-url", "tls-server-name", "extensions",
        "disable-compression"}) {
    auto value = document();
    auto other = value["clusters"][0];
    other["name"] = "unselected-cluster";
    other["cluster"][field] = "synthetic-secret";
    value["clusters"].push_back(other);
    refused(value);
  }
}

TEST_CASE(
    "static kubeconfig preserves types and requires supported credentials") {
  const std::array paths{"/apiVersion",
                         "/kind",
                         "/clusters/0/name",
                         "/clusters/0/cluster/server",
                         "/clusters/0/cluster/certificate-authority-data",
                         "/users/0/name",
                         "/users/0/user/token",
                         "/contexts/0/name",
                         "/contexts/0/context/cluster",
                         "/contexts/0/context/user",
                         "/contexts/0/context/namespace",
                         "/current-context"};
  for (const auto path : paths) {
    for (const auto& wrong : {Json(nullptr), Json(true), Json(false), Json(1),
                              Json(1.5), Json::array(), Json::object()}) {
      auto value = document();
      value[Json::json_pointer{path}] = wrong;
      refused(value);
    }
  }
  for (const auto path :
       {"/clusters", "/users", "/contexts", "/preferences", "/clusters/0",
        "/users/0", "/contexts/0", "/clusters/0/cluster", "/users/0/user",
        "/contexts/0/context"}) {
    auto value = document();
    value[Json::json_pointer{path}] = "wrong-container";
    refused(value);
  }
  for (const auto& wrong : {Json(nullptr), Json(true), Json("false"), Json(0),
                            Json::array(), Json::object()}) {
    auto value = document();
    value["clusters"][0]["cluster"]["insecure-skip-tls-verify"] = wrong;
    refused(value);
  }
  for (const auto& auth :
       {Json{{"token", ""}}, Json{{"token", "has space"}},
        Json{{"token", "synthetic"},
             {"client-certificate-data", ca},
             {"client-key-data", key}},
        Json{{"client-certificate-data", ca}}, Json{{"client-key-data", key}},
        Json{{"client-certificate-data", ""}, {"client-key-data", key}}}) {
    auto value = document();
    value["users"][0]["user"] = auth;
    refused(value);
  }
}

TEST_CASE("static kubeconfig rejects unresolved selection and invalid endpoint "
          "identity") {
  const auto value = document().dump();
  for (const auto context : {"", "absent", "bad\ncontext"})
    CHECK_FALSE(parse(value, Syntax::json, context));
  for (const auto ns : {"", "default/other", "UPPER", "-bad", "bad-", "a.b"})
    CHECK_FALSE(parse(value, Syntax::json, "context-one", ns));
  for (const auto field : {"cluster", "user"}) {
    auto changed = document();
    changed["contexts"][0]["context"][field] = "absent";
    refused(changed);
  }
  for (const auto server :
       {"http://fixture.invalid", "https://user@fixture.invalid",
        "https://fixture.invalid/path", "https://fixture.invalid?x=1",
        "https://fixture.invalid#x", "https://fixture.invalid:0",
        "https://fixture.invalid:65536",
        "https://fixture.invalid:", "https://fixture.invalid:+443",
        "https://fixture.invalid//", "https://Fixture.invalid",
        "https://999.1.2.3", "https://[::1", "https://[::1]extra",
        "https://::1", "https://[127.0.0.1]", "https://[fe80::1%eth0]"}) {
    auto changed = document();
    changed["clusters"][0]["cluster"]["server"] = server;
    refused(changed);
  }
  for (const auto server : {"https://fixture.invalid",
                            "https://fixture.invalid/", "https://127.0.0.1:443",
                            "https://[::1]:6443", "https://[2001:db8::1]"}) {
    auto changed = document();
    changed["clusters"][0]["cluster"]["server"] = server;
    CHECK(parse(changed.dump()));
  }
}

TEST_CASE("static kubeconfig rejects malformed base64 and PEM envelopes "
          "without diagnostics") {
  for (const auto value :
       {"", "A", "AAA", "====", "A===", "AA=A", "AA==AAAA",
        "AB==", "AAB=", "AAA-", " AAA", "AAAA\n", "AA==", "AQID"}) {
    auto changed = document();
    changed["clusters"][0]["cluster"]["certificate-authority-data"] = value;
    refused(changed);
  }
  for (const auto pem :
       {"", "synthetic-secret",
        "-----BEGIN CERTIFICATE-----\nAQID\n-----END PRIVATE KEY-----\n",
        "-----BEGIN CERTIFICATE-----\nAB==\n-----END CERTIFICATE-----\n",
        "-----BEGIN CERTIFICATE-----\nAQID-----END CERTIFICATE-----",
        "-----BEGIN CERTIFICATE-----\nAQID\n-----END "
        "CERTIFICATE-----\ntrailing"}) {
    rejects([&] { cfg::validate_pem(pem, false); }, Failure::invalid_input);
  }
  CHECK(cfg::decode_base64("AA==") == std::string(1, '\0'));
  CHECK(cfg::decode_base64("AQI=") == std::string("\1\2", 2));
  CHECK(cfg::decode_base64("AQID") == std::string("\1\2\3", 3));
  const auto certificate = cfg::decode_base64(ca);
  std::string bundle;
  for (unsigned index{}; index < 32; ++index)
    bundle += certificate;
  CHECK_NOTHROW(cfg::validate_pem(bundle, false));
  rejects([&] { cfg::validate_pem(bundle + certificate, false); },
          Failure::resource_exhausted);
  CHECK_NOTHROW(cfg::validate_pem(cfg::decode_base64(key), true));
  rejects([&] { cfg::validate_pem(certificate, true); },
          Failure::invalid_input);
  rejects(
      [&] {
        cfg::validate_pem(cfg::decode_base64(key) + cfg::decode_base64(key),
                          true);
      },
      Failure::invalid_input);
  const auto failure =
      parse("{\"synthetic-secret\": invalid-synthetic-secret}");
  REQUIRE_FALSE(failure);
  CHECK(failure.error() == Failure::unsupported);
  const auto malformed = parse("{\"apiVersion\": invalid-synthetic-secret}");
  REQUIRE_FALSE(malformed);
  CHECK(malformed.error() == Failure::invalid_input);
}

TEST_CASE("static kubeconfig YAML rejects alias expansion tags ambiguity and "
          "multiple documents") {
  const auto base = yaml_document();
  for (const auto token : {"&anchor synthetic",
                           "*anchor",
                           "null",
                           "~",
                           "true",
                           "false",
                           "yes",
                           "NO",
                           "on",
                           "off",
                           "y",
                           "n",
                           "123",
                           "-2",
                           ".2",
                           "0x12",
                           "2026-09-09",
                           "1e100",
                           ".nan",
                           "!!int 1",
                           "!custom synthetic",
                           "!!null synthetic",
                           "!!bool invalid",
                           "[synthetic]",
                           "{synthetic: value}"}) {
    CHECK_FALSE(parse(yaml_document(token), Syntax::yaml));
  }
  for (const auto token :
       {"\"true\"", "'123'", "!!str false", "!!str 123", "'2026-09-09'"})
    CHECK(parse(yaml_document(token), Syntax::yaml));
  for (const auto prefix : {"--- &root\n", "--- !custom\n",
                            "? [a,b]\n: value\n", "<<: {kind: Config}\n"})
    CHECK_FALSE(parse(std::string{prefix} + base, Syntax::yaml));
  CHECK_FALSE(parse(base + "\n---\n{}", Syntax::yaml));
  CHECK_FALSE(parse(base + "\n---\n[", Syntax::yaml));
  CHECK_FALSE(parse(base + "\n\"api\\u0056ersion\": v1\n", Syntax::yaml));
  CHECK_FALSE(parse(base, Syntax::json));
  CHECK_FALSE(parse("", Syntax::yaml));
  CHECK_FALSE(parse("# only a comment\n", Syntax::yaml));
}

TEST_CASE(
    "static kubeconfig JSON is strict and decoded unsafe text never survives") {
  const auto base = document().dump();
  for (const auto suffix : {"{}", "//comment", "/*comment*/", "\n---\n", "]"})
    CHECK_FALSE(parse(base + suffix));
  for (const auto text :
       {"{", "null", "[]", "true", "1", "\"string\"",
        "{\"apiVersion\":\"\\ud800\"}", "{\"apiVersion\":\"\\udc00\"}"})
    CHECK_FALSE(parse(text));
  for (const auto token :
       {std::string{"nul\0secret", 10}, std::string{"line\nsecret"},
        std::string{"tab\tsecret"}, std::string{"escape\x1b"},
        std::string{"bidi\xe2\x80\xae"}, std::string{"bad\xff"}}) {
    if (token.ends_with('\xff')) {
      auto raw = yaml_document();
      raw += token;
      CHECK_FALSE(parse(raw, Syntax::yaml));
    } else {
      auto changed = document();
      changed["users"][0]["user"]["token"] = token;
      refused(changed);
    }
  }
  CHECK_FALSE(parse(base, static_cast<Syntax>(99)));
  // JSON numeric overflow must never become a coerced scalar or an internal
  // failure that preserves a foreign parser diagnostic.
  const auto overflow = parse("{\"apiVersion\":1e999999}");
  REQUIRE_FALSE(overflow);
  CHECK(overflow.error() == Failure::invalid_input);
}

TEST_CASE(
    "static kubeconfig byte and named entry limits fail before publishing") {
  const auto base = document().dump();
  auto padded = base + std::string(cfg::input_limit - base.size(), ' ');
  CHECK(parse(padded));
  padded += ' ';
  const auto oversized = parse(padded);
  REQUIRE_FALSE(oversized);
  CHECK(oversized.error() == Failure::resource_exhausted);
  auto value = document();
  value["users"][0]["user"]["token"] = std::string(cfg::token_limit, 'x');
  CHECK(parse(value.dump()));
  value["users"][0]["user"]["token"] = std::string(cfg::token_limit + 1, 'x');
  refused(value);
  value = document();
  value["contexts"][0]["name"] = std::string(256, 'x');
  CHECK(parse(value.dump(), Syntax::json, std::string(256, 'x')));
  value["contexts"][0]["name"] = std::string(257, 'x');
  CHECK_FALSE(parse(value.dump(), Syntax::json, std::string(257, 'x')));
  CHECK(parse(base, Syntax::json, "context-one", std::string(63, 'a')));
  CHECK_FALSE(parse(base, Syntax::json, "context-one", std::string(64, 'a')));
  for (const auto list : {"clusters", "users", "contexts"}) {
    value = document();
    for (std::size_t index = 1; index < cfg::entry_limit; ++index) {
      auto entry = value[list][0];
      entry["name"] = "unused-" + std::to_string(index);
      value[list].push_back(entry);
    }
    CHECK(parse(value.dump()));
    auto entry = value[list][0];
    entry["name"] = "over-limit";
    value[list].push_back(entry);
    refused(value);
  }
}

TEST_CASE("static kubeconfig defensive budgets include cancellation and "
          "checked output escaping") {
  cfg::Budget events;
  for (std::size_t index{}; index < cfg::event_limit; ++index)
    events.event();
  rejects([&] { events.event(); }, Failure::resource_exhausted);
  cfg::Budget depth;
  for (std::size_t index{}; index < cfg::depth_limit; ++index)
    depth.enter();
  rejects([&] { depth.enter(); }, Failure::resource_exhausted);
  for (std::size_t index{}; index < cfg::depth_limit; ++index)
    depth.leave();
  rejects([&] { depth.leave(); }, Failure::invalid_input);
  cfg::Budget scalar;
  const std::string full(cfg::scalar_limit, 'x');
  scalar.scalar(full);
  rejects([&] { scalar.scalar(full + 'x'); }, Failure::resource_exhausted);
  cfg::Budget aggregate;
  aggregate.scalar(full);
  aggregate.scalar(full);
  rejects([&] { aggregate.scalar("x"); }, Failure::resource_exhausted);
  cfg::Writer output;
  output.raw(std::string(cfg::output_limit, 'x'));
  CHECK(output.size() == cfg::output_limit);
  rejects([&] { output.raw("x"); }, Failure::resource_exhausted);
  cfg::Writer escaped;
  escaped.quoted(std::string((cfg::output_limit - 2) / 2, '"'));
  CHECK(escaped.size() == cfg::output_limit);
  rejects([&] { escaped.quoted("x"); }, Failure::resource_exhausted);
  std::stop_source stop;
  cfg::Sink sink{stop.get_token()};
  sink.document_start();
  sink.mapping_start();
  stop.request_stop();
  rejects([&] { sink.key("apiVersion"); }, Failure::cancelled);
  const auto cancelled =
      Config::parse(document().dump(), Syntax::json, "context-one", "selected",
                    stop.get_token());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error() == Failure::cancelled);
}

TEST_CASE(
    "static kubeconfig identity helper preserves existing binding rules") {
  const auto result = parse(document().dump());
  REQUIRE(result);
  const auto identity = result->identity();
  domain::OpsTargetBinding binding{
      *domain::OpsTargetId::from("target"),
      *domain::OpsConfigurationRevision::from("revision"), identity};
  CHECK(domain::validate_ops_target_binding(binding));
  for (unsigned mutation{}; mutation != 5; ++mutation) {
    auto invalid = identity;
    switch (mutation) {
      case 0: invalid.context_name.clear(); break;
      case 1: invalid.namespace_name = "Invalid"; break;
      case 2: invalid.endpoint.host = "not/a/host"; break;
      case 3: invalid.endpoint.port = 0; break;
      default: invalid.trust_identity.clear(); break;
    }
    binding.identity = invalid;
    CHECK_FALSE(domain::validate_kubernetes_ops_identity(invalid));
    CHECK_FALSE(domain::validate_ops_target_binding(binding));
  }
  binding.identity = identity;
  binding.target_id = *domain::OpsTargetId::from(std::string(1, '\xff'));
  CHECK(domain::validate_kubernetes_ops_identity(identity));
  CHECK_FALSE(domain::validate_ops_target_binding(binding));
}

TEST_CASE("static kubeconfig selects explicitly and keeps only private "
          "admitted credentials") {
  static_assert(!std::is_copy_constructible_v<Config> &&
                std::is_nothrow_move_constructible_v<Config>);
  auto value = document();
  auto other = value["users"][0];
  other["name"] = "unselected-user";
  other["user"]["token"] = "unselected-synthetic-secret";
  value["users"].push_back(other);
  value["current-context"] = "unselected-context";
  value["clusters"][0]["cluster"]["insecure-skip-tls-verify"] = false;
  const auto json = parse(value.dump());
  REQUIRE(json);
  CHECK(json->identity().namespace_name == "selected");
  CHECK(json->identity().context_name == "context-one");
  CHECK(json->identity().endpoint.host == "fixture.invalid");
  CHECK(json->identity().endpoint.port == 6443);
  CHECK(json->identity().trust_identity.starts_with("sha256:"));
  CHECK(json->identity().trust_identity.size() == 71);
  CHECK(json->identity().trust_identity ==
        "sha256:"
        "1ce4f4d40b3fa446e425ce65a2eb1ecb7d9e9859412b8f005e4813a9c6a03f91");
  CHECK(json->configuration_bytes().find("unselected") ==
        std::string_view::npos);
  CHECK(json->configuration_bytes().find("old-default") ==
        std::string_view::npos);
  const auto yaml = parse(yaml_document(), Syntax::yaml);
  REQUIRE(yaml);
  CHECK(json->identity() == yaml->identity());
  CHECK(json->configuration_bytes() == yaml->configuration_bytes());
  const auto generated = parse(json->configuration_bytes());
  REQUIRE(generated);
  CHECK(generated->identity() == json->identity());
  value["contexts"][0]["context"].erase("namespace");
  value["users"][0]["user"] = {{"client-certificate-data", ca},
                               {"client-key-data", key}};
  const auto certificate = parse(value.dump());
  REQUIRE(certificate);
  CHECK(certificate->identity() == json->identity());
  CHECK(certificate->configuration_bytes().find("client-key-data") !=
        std::string_view::npos);
  CHECK(parse(certificate->configuration_bytes()));
  value["users"][0]["user"] = {{"token", R"(synthetic-"\token)"}};
  const auto escaped = parse(value.dump());
  REQUIRE(escaped);
  const auto private_document = Json::parse(escaped->configuration_bytes());
  CHECK(private_document["users"][0]["user"]["token"] ==
        R"(synthetic-"\token)");
  CHECK(escaped->identity().trust_identity == json->identity().trust_identity);
  CHECK(parse(escaped->configuration_bytes()));
}
