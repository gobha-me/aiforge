#pragma once
#include "kubernetes_ops_observation_source.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace fixture {
using Source = aiforge::adapters::KubernetesOpsObservationSource;
using Error = aiforge::runtime::OpsObservationSourceError;
using Operation = aiforge::domain::OpsObservationOperation;
struct Credentials {
  std::string server_ca;
  std::string server_cert;
  std::string server_key;
  std::string wrong_host_cert;
  std::string expired_cert;
  std::string unrelated_ca;
  std::string client_ca;
  std::string client_chain;
  std::string client_leaf;
  std::string client_key;
  std::string wrong_client_key;
};
[[nodiscard]] auto credentials() -> const Credentials&;
inline constexpr std::string_view token{"fixture-token-not-real-196"};
struct PeerOptions {
  std::string response;
  bool wrong_host{};
  bool expired{};
  bool require_client{};
  bool stall_handshake{};
  bool stall_body{};
  unsigned connections{1};
};
class Peer final {
 public:
  explicit Peer(PeerOptions options);
  ~Peer();
  Peer(const Peer&) = delete;
  auto operator=(const Peer&) -> Peer& = delete;
  [[nodiscard]] auto port() const noexcept -> std::uint16_t;
  [[nodiscard]] auto accepted() const noexcept -> unsigned;
  [[nodiscard]] auto received() const noexcept -> bool;
  [[nodiscard]] auto authorized() const noexcept -> bool;
  [[nodiscard]] auto request_line() const -> std::string;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
[[nodiscard]] auto response(std::string body, std::string headers = {},
                            int status = 200) -> std::string;
[[nodiscard]] auto pod() -> std::string;
[[nodiscard]] auto list(bool events = false) -> std::string;
[[nodiscard]] auto configuration(std::uint16_t port, bool certificate = false,
                                 std::string ca = {}, std::string chain = {},
                                 std::string key = {},
                                 std::string host = "127.0.0.1")
    -> aiforge::adapters::StaticKubernetesConfig;
[[nodiscard]] auto source(
    aiforge::adapters::StaticKubernetesConfig configuration)
    -> std::shared_ptr<Source>;
[[nodiscard]] auto request(
    const Source& source, Operation operation = Operation::kubernetes_workloads)
    -> aiforge::domain::OpsObservationRequest;
// Test-owned interposition at SSL_CTX_new models a retained native TLS stall.
// Releasing/destructing this guard wakes the call, including failed assertions.
class TlsPause final {
 public:
  TlsPause();
  ~TlsPause();
  TlsPause(const TlsPause&) = delete;
  auto operator=(const TlsPause&) -> TlsPause& = delete;
  [[nodiscard]] auto entered() const noexcept -> bool;
  auto release() noexcept -> void;

 private:
  struct Impl;
  std::shared_ptr<Impl> m_impl;
  friend auto pause_native_tls() -> void;
};
auto fail_next_thread_start(bool enabled) noexcept -> void;
[[nodiscard]] auto native_calls() noexcept -> unsigned;
} // namespace fixture
