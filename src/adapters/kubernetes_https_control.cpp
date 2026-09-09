#include "kubernetes_https_control.hpp"
#include <cerrno>
#include <venice/detail/httplib_contract.hpp>
#if defined(__linux__)
#include <pthread.h>
#endif

namespace aiforge::adapters {
KubernetesSigPipeBlock::KubernetesSigPipeBlock() noexcept {
#if defined(__linux__)
  if (sigemptyset(&m_only) != 0 || sigaddset(&m_only, SIGPIPE) != 0 ||
      pthread_sigmask(SIG_BLOCK, &m_only, &m_saved) != 0)
    return;
  m_engaged = true;
  sigset_t pending{};
  if (sigpending(&pending) != 0) return;
  m_preserve_pending = sigismember(&m_saved, SIGPIPE) == 1 ||
                       sigismember(&pending, SIGPIPE) == 1;
  m_valid = true;
#endif
}
KubernetesSigPipeBlock::~KubernetesSigPipeBlock() {
#if defined(__linux__)
  if (!m_engaged) return;
  if (m_valid && !m_preserve_pending) {
    const timespec zero{};
    while (sigtimedwait(&m_only, nullptr, &zero) >= 0 || errno == EINTR) {
    }
  }
  (void)pthread_sigmask(SIG_SETMASK, &m_saved, nullptr);
#endif
}
KubernetesHttpsControl::KubernetesHttpsControl(Clock::time_point deadline,
                                               std::stop_token external)
    : m_deadline(deadline), m_external(std::move(external)),
      m_callback(m_external, Notify{this}) {
}
KubernetesHttpsControl::~KubernetesHttpsControl() {
  m_finished.store(true);
  m_ready.notify_all();
  if (m_thread.joinable()) m_thread.join();
}
auto KubernetesHttpsControl::Notify::operator()() const noexcept -> void {
  owner->m_ready.notify_all();
}
auto KubernetesHttpsControl::start(httplib::SSLClient& client) -> void {
  m_thread = std::thread{[this, &client] { watch(client); }};
}
auto KubernetesHttpsControl::failure() const noexcept
    -> std::optional<runtime::OpsObservationSourceError> {
  using Error = runtime::OpsObservationSourceError;
  if (m_external.stop_requested()) return Error::cancelled;
  if (Clock::now() >= m_deadline) return Error::timed_out;
  if (m_failed.load()) return Error::internal_failure;
  return {};
}
auto KubernetesHttpsControl::remaining() const noexcept
    -> std::chrono::milliseconds {
  // Native waits have millisecond granularity. Round up only that physical
  // allowance; failure() and publication keep the original absolute deadline.
  return std::chrono::ceil<std::chrono::milliseconds>(m_deadline -
                                                      Clock::now());
}
auto KubernetesHttpsControl::token() const noexcept -> std::stop_token {
  return m_projection.get_token();
}
auto KubernetesHttpsControl::watch(httplib::SSLClient& client) noexcept
    -> void {
  try {
    while (!m_finished.load()) {
      if (failure()) {
        (void)m_projection.request_stop();
        // No control mutex is held here. stop may itself wait on synchronous
        // connect/DNS; only this retained worker-owned watcher can block there.
        client.stop();
      }
      std::unique_lock lock{m_mutex};
      m_ready.wait_for(lock, std::chrono::milliseconds{2},
                       [this] { return m_finished.load(); });
    }
  } catch (...) {
    m_failed.store(true);
    (void)m_projection.request_stop();
  }
}
} // namespace aiforge::adapters
