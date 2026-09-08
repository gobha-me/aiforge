#include <aiforge/surfaces/context_control.hpp>
#include <limits>
#include <utility>

namespace aiforge::surfaces {
namespace {
template <class... T> struct Overloaded : T... {
  using T::operator()...;
};
auto rejected(std::string message) -> std::unexpected<ChatSessionError> {
  return std::unexpected(ChatSessionError{ChatSessionErrorCode::invalid_input,
                                          std::move(message), false});
}
} // namespace
ContextController::ContextController(ChatSession& session,
                                     std::string instance_identity)
    : m_session(session), m_instance_identity(std::move(instance_identity)) {
}
auto ContextController::producer_active() const noexcept -> bool {
  return m_producer.has_value() && m_session.active();
}
auto ContextController::closed() const noexcept -> bool {
  return m_closed;
}
auto ContextController::inspect(const ContextInspect& value)
    -> std::expected<ContextPayload, ChatSessionError> {
  if (value.limit == 0 || value.limit > context_maximum_page_size)
    return rejected("invalid inspection page size");
  auto context = m_session.inspect_conversation_context(value.draft);
  if (!context) return std::unexpected(context.error());
  auto catalog = m_session.summary_catalog();
  if (!catalog) return std::unexpected(catalog.error());
  return ContextInspected{std::move(*context), std::move(*catalog),
                          value.offset, value.limit};
}
auto ContextController::preview(const ContextPreview& value)
    -> std::expected<ContextPayload, ChatSessionError> {
  if (m_instance_identity.empty() || m_instance_identity.size() > 64 ||
      m_next_handle == std::numeric_limits<std::uint64_t>::max())
    return rejected("preview handle allocation unavailable");
  auto preview = m_session.preview_conversation_summary(
      value.candidate, value.replacements, value.draft);
  if (!preview) return std::unexpected(preview.error());
  m_review_handle = m_instance_identity + '-' + std::to_string(++m_next_handle);
  m_review = std::move(*preview);
  m_review_sequence = m_session.event_log().last_sequence();
  return ContextPreviewed{m_review_handle,
                          m_review->activation(),
                          m_review->context(),
                          m_review_sequence,
                          0,
                          context_maximum_page_size};
}
auto ContextController::preview_page(const ContextPreviewPage& value)
    -> std::expected<ContextPayload, ChatSessionError> {
  if (!m_review || value.handle != m_review_handle || value.limit == 0 ||
      value.limit > context_maximum_page_size)
    return rejected("preview page handle or bounds are invalid");
  if (m_review_sequence != m_session.event_log().last_sequence()) {
    m_review.reset();
    m_review_handle.clear();
    return rejected("preview session snapshot is stale");
  }
  return ContextPreviewed{m_review_handle,     m_review->activation(),
                          m_review->context(), m_review_sequence,
                          value.offset,        value.limit};
}
auto ContextController::apply(const ContextApply& value)
    -> std::expected<ContextPayload, ChatSessionError> {
  auto review = std::move(m_review);
  m_review.reset();
  const auto handle = std::exchange(m_review_handle, {});
  if (!review || value.handle != handle)
    return rejected("preview handle is absent, stale, or belongs to "
                    "another process");
  auto applied = m_session.apply_conversation_summary(*review, value.draft);
  if (!applied) return std::unexpected(applied.error());
  return std::move(*applied);
}
auto ContextController::dispatch(const ContextOperation& operation)
    -> std::expected<ContextPayload, ChatSessionError> {
  return std::visit(
      Overloaded{
          [&](const ContextInspect& value) { return inspect(value); },
          [&](const ContextPolicy& value)
              -> std::expected<ContextPayload, ChatSessionError> {
            auto changed = m_session.set_conversation_policy(
                value.expected_revision, value.mode, value.pins);
            if (!changed) return std::unexpected(changed.error());
            return ContextOk{};
          },
          [&](const ChatSummaryGenerate& value)
              -> std::expected<ContextPayload, ChatSessionError> {
            auto generated = m_session.generate_conversation_summary(value);
            if (!generated) return std::unexpected(generated.error());
            m_producer =
                ContextGenerated{generated->summary_id, generated->run_id};
            return *m_producer;
          },
          [&](const ContextPublish& value)
              -> std::expected<ContextPayload, ChatSessionError> {
            auto published =
                m_session.publish_conversation_summary(value.summary_id);
            if (!published) return std::unexpected(published.error());
            return std::move(*published);
          },
          [&](const ContextEdit& value)
              -> std::expected<ContextPayload, ChatSessionError> {
            auto edited = m_session.edit_conversation_summary(
                value.expected_sequence, value.parent, value.text);
            if (!edited) return std::unexpected(edited.error());
            return std::move(*edited);
          },
          [&](const ContextPreview& value) { return preview(value); },
          [&](const ContextPreviewPage& value) { return preview_page(value); },
          [&](const ContextApply& value) { return apply(value); },
          [&](const ContextDisable& value)
              -> std::expected<ContextPayload, ChatSessionError> {
            auto disabled = m_session.disable_conversation_summary(
                value.expected_revision, value.candidate,
                value.activation_event_id);
            if (!disabled) return std::unexpected(disabled.error());
            return ContextOk{};
          },
          [&](const ContextClose&)
              -> std::expected<ContextPayload, ChatSessionError> {
            auto cancelled = cancel_producer();
            if (!cancelled) return std::unexpected(cancelled.error());
            m_closed = true;
            return ContextOk{true};
          }},
      operation);
}
auto ContextController::execute(const ContextRequest& request) -> ContextReply {
  try {
    if (request.id == 0 || m_closed)
      return {request.id, m_session.event_log().last_sequence(),
              ContextFailure{
                  "invalid_request",
                  "context loop is closed or request identity is invalid",
                  false}};
    const bool inspect =
        std::holds_alternative<ContextInspect>(request.operation);
    const bool page =
        std::holds_alternative<ContextPreviewPage>(request.operation);
    const bool close = std::holds_alternative<ContextClose>(request.operation);
    const bool apply = std::holds_alternative<ContextApply>(request.operation);
    if (!inspect && !apply && !page) {
      m_review.reset();
      m_review_handle.clear();
    }
    if (m_session.active() && !inspect && !page && !close)
      return {request.id, m_session.event_log().last_sequence(),
              ContextFailure{
                  "busy",
                  "a run is active; wait for completion or close the loop",
                  false}};
    auto result = dispatch(request.operation);
    if (!result)
      return {request.id, m_session.event_log().last_sequence(),
              ContextFailure{"operation_failed", result.error().message,
                             result.error().effect_may_have_applied}};
    if (std::holds_alternative<ContextGenerated>(*result))
      m_producer_request_id = request.id;
    return {request.id, m_session.event_log().last_sequence(),
            std::move(*result)};
  } catch (...) {
    m_review.reset();
    m_review_handle.clear();
    return {request.id, m_session.event_log().last_sequence(),
            ContextFailure{"internal_failure",
                           "context operation failed internally", true}};
  }
}
auto ContextController::poll()
    -> std::expected<std::optional<ContextReply>, ChatSessionError> try {
  if (!m_producer) return std::nullopt;
  auto drained = m_session.drain();
  if (!drained) return std::unexpected(drained.error());
  if (m_session.active()) return std::nullopt;
  domain::RunProjection projection;
  for (const auto& event : m_session.event_log().events())
    if (event.metadata.run_id == m_producer->run_id) {
      auto applied = projection.apply(event);
      if (!applied) return rejected("summary result could not be projected");
    }
  auto producer = std::move(*m_producer);
  m_producer.reset();
  return ContextReply{m_producer_request_id,
                      m_session.event_log().last_sequence(),
                      ContextGenerationFinished{std::move(producer.summary_id),
                                                std::move(producer.run_id),
                                                projection.status()}};
} catch (...) {
  return rejected("summary result could not be drained");
}
auto ContextController::cancel_producer()
    -> std::expected<void, ChatSessionError> try {
  m_review.reset();
  m_review_handle.clear();
  if (producer_active()) return m_session.cancel_active("context input closed");
  return {};
} catch (...) {
  return rejected("summary producer could not be cancelled");
}
} // namespace aiforge::surfaces
