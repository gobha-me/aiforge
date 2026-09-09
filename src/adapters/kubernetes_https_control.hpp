#pragma once
#include <aiforge/runtime/ops_observation_source.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#if defined(__linux__)
#include <signal.h>
#endif
namespace httplib {
class SSLClient;
}
namespace aiforge::adapters {
// Thread-scoped signal protection; no process disposition changes. Its lifetime
// must include the client destructor and the watcher (which inherits the mask).
class KubernetesSigPipeBlock final {
 public:
  KubernetesSigPipeBlock() noexcept;
  ~KubernetesSigPipeBlock();
  KubernetesSigPipeBlock(const KubernetesSigPipeBlock&) = delete;
  auto operator=(const KubernetesSigPipeBlock&)
      -> KubernetesSigPipeBlock& = delete;
  [[nodiscard]] auto valid() const noexcept -> bool { return m_valid; }

 private:
#if defined(__linux__)
  sigset_t m_saved{};
  sigset_t m_only{};
  bool m_engaged{};
  bool m_preserve_pending{};
#endif
  bool m_valid{};
};

class KubernetesHttpsControl final {
 public:
  using Clock = std::chrono::steady_clock;
  KubernetesHttpsControl(Clock::time_point deadline, std::stop_token external);
  ~KubernetesHttpsControl();
  KubernetesHttpsControl(const KubernetesHttpsControl&) = delete;
  auto operator=(const KubernetesHttpsControl&)
      -> KubernetesHttpsControl& = delete;
  // Start only once, after client creation. Caller destroys this before client.
  auto start(httplib::SSLClient& client) -> void;
  [[nodiscard]] auto failure() const noexcept
      -> std::optional<runtime::OpsObservationSourceError>;
  [[nodiscard]] auto remaining() const noexcept -> std::chrono::milliseconds;
  [[nodiscard]] auto token() const noexcept -> std::stop_token;

 private:
  struct Notify {
    KubernetesHttpsControl* owner;
    auto operator()() const noexcept -> void;
  };
  auto watch(httplib::SSLClient& client) noexcept -> void;
  const Clock::time_point m_deadline;
  const std::stop_token m_external;
  std::stop_source m_projection;
  std::atomic<bool> m_finished{};
  std::atomic<bool> m_failed{};
  std::mutex m_mutex;
  std::condition_variable m_ready;
  std::stop_callback<Notify> m_callback;
  std::thread m_thread;
};
} // namespace aiforge::adapters
