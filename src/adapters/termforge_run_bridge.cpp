#include <aiforge/adapters/termforge_run_bridge.hpp>
#include <string_view>
#include <variant>

namespace aiforge::adapters {
namespace {

constexpr std::string_view kWakeSource{"aiforge.runtime"};
constexpr std::string_view kWakeMessage{"events-ready"};

[[nodiscard]] auto is_wake(const termforge::Event& event) -> bool {
  const auto* error = std::get_if<termforge::ErrorEvent>(&event);
  return error != nullptr && error->severity == termforge::Severity::Info &&
         error->source == kWakeSource && error->message == kWakeMessage;
}

[[nodiscard]] auto is_escape(const termforge::Event& event) -> bool {
  const auto* key = std::get_if<termforge::KeyEvent>(&event);
  return key != nullptr && key->key == termforge::Key::Escape &&
         key->action == termforge::KeyAction::Press;
}

}  // namespace

auto TermForgeRunBridge::wake() noexcept -> void {
  try {
    if (m_live_wake_enabled) {
      m_app.post(termforge::ErrorEvent{termforge::Severity::Info,
                                       std::string{kWakeSource},
                                       std::string{kWakeMessage}});
    }
    if (m_wake_observer) m_wake_observer();
  } catch (...) {
    // App::post may allocate. Continuous-mode surfaces still call drain from
    // their tick path, so an allocation failure may delay but cannot corrupt a
    // run or permit a worker-thread widget mutation.
  }
}

auto TermForgeRunBridge::handle(const termforge::Event& event,
                                runtime::RunKernel& kernel)
    -> std::expected<std::vector<domain::RunEvent>, runtime::RunKernelError> {
  if (is_wake(event)) return kernel.drain();
  if (is_escape(event)) {
    const auto run_id = kernel.active_run_id();
    const auto inference_id = kernel.active_inference_id();
    if (run_id && inference_id) {
      auto cancelled = kernel.cancel(*run_id, *inference_id, "escape");
      if (!cancelled) return std::unexpected(std::move(cancelled.error()));
    }
  }
  return std::vector<domain::RunEvent>{};
}

auto TermForgeRunBridge::handle(const termforge::Event& event,
                                surfaces::ChatSession& session)
    -> std::expected<std::vector<domain::RunEvent>,
                     surfaces::ChatSessionError> {
  if (is_wake(event)) return session.drain();
  if (is_escape(event) && session.active()) {
    auto cancelled = session.cancel_active("escape");
    if (!cancelled) return std::unexpected(std::move(cancelled.error()));
  }
  return std::vector<domain::RunEvent>{};
}

}  // namespace aiforge::adapters
