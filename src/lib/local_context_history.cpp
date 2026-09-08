#include <aiforge/runtime/run_kernel.hpp>

#include <set>
#include <utility>

namespace aiforge::runtime {
namespace {
class LocalAdmissionHistory final {
 public:
  explicit LocalAdmissionHistory(domain::SessionId session)
      : m_session(std::move(session)) {}

  auto consume(const domain::RunEvent& event) -> bool {
    if (const auto* started = std::get_if<domain::RunStarted>(&event.payload)) {
      if (m_started) return false;
      m_started = true;
      m_conversation = started->purpose == domain::RunPurpose::conversation;
      m_required = started->local_context_admission_required;
      return m_required ? m_conversation && event.metadata.schema_version == 5
                        : event.metadata.schema_version >= 1 &&
                              event.metadata.schema_version <= 4;
    }
    if (!m_started) return false;
    if (const auto* admitted =
            std::get_if<domain::LocalContextAdmitted>(&event.payload))
      return event.metadata.schema_version == 1 &&
             !event.metadata.invocation_id && accept_admission(*admitted);
    if (const auto* repository =
            std::get_if<domain::RepositoryContextAdmitted>(&event.payload)) {
      if (!m_pending) return true;
      if (m_repository_seen || event.metadata.schema_version != 1 ||
          event.metadata.invocation_id ||
          repository->inference_id != m_pending->inference_id ||
          !domain::validate_repository_context_admission(repository->admission))
        return false;
      m_repository_seen = true;
      return true;
    }
    if (const auto* inference =
            std::get_if<domain::InferenceStarted>(&event.payload))
      return accept_inference(inference->inference_id);
    if (const auto* unknown = std::get_if<domain::UnknownEvent>(&event.payload);
        unknown != nullptr &&
        unknown->type_name == "run.local_context_admitted")
      return false;
    return !m_pending;
  }

  [[nodiscard]] auto complete() const -> bool {
    return m_started && !m_pending && (!m_required || m_latest.has_value());
  }
  [[nodiscard]] auto latest() const
      -> const std::optional<domain::LocalContextAdmission>& {
    return m_latest;
  }

 private:
  auto accept_admission(const domain::LocalContextAdmitted& event) -> bool {
    if (!m_required || !m_conversation || m_pending ||
        (m_observed_inference && !m_latest) ||
        event.admission.session_id != m_session ||
        !domain::validate_local_context_admission(event.admission) ||
        (m_latest && !domain::local_context_admission_successor(
                         *m_latest, event.admission)))
      return false;
    m_pending = event;
    m_repository_seen = false;
    return true;
  }
  auto accept_inference(const domain::InferenceId& inference) -> bool {
    if (!m_inferences.insert(inference).second) return false;
    m_observed_inference = true;
    if (!m_pending) return !m_required && !m_latest;
    if (m_pending->inference_id != inference) return false;
    m_latest = std::move(m_pending->admission);
    m_pending.reset();
    m_repository_seen = false;
    return true;
  }
  domain::SessionId m_session;
  bool m_started{};
  bool m_conversation{};
  bool m_required{};
  bool m_observed_inference{};
  bool m_repository_seen{};
  std::set<domain::InferenceId> m_inferences;
  std::optional<domain::LocalContextAdmitted> m_pending;
  std::optional<domain::LocalContextAdmission> m_latest;
};
} // namespace

auto recorded_local_context_admission(const domain::SessionEventLog& event_log,
                                      const domain::RunId& run_id)
    -> std::expected<std::optional<domain::LocalContextAdmission>,
                     RunKernelError> {
  try {
    LocalAdmissionHistory history{event_log.session_id()};
    for (const auto& event : event_log.events()) {
      if (event.metadata.run_id == run_id && !history.consume(event))
        return std::unexpected(
            RunKernelError{RunKernelErrorCode::replay_rejected,
                           "local context admission history is inconsistent"});
    }
    if (!history.complete())
      return std::unexpected(
          RunKernelError{RunKernelErrorCode::replay_rejected,
                         "local context admission history is incomplete"});
    return history.latest();
  } catch (...) {
    return std::unexpected(RunKernelError{
        RunKernelErrorCode::internal_failure,
        "local context admission history could not be inspected"});
  }
}
} // namespace aiforge::runtime
