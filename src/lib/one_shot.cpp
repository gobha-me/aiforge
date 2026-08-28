#include <aiforge/domain/usage_ledger.hpp>
#include <aiforge/presentation/text.hpp>
#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/persona.hpp>
#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/surfaces/one_shot.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <ostream>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "conversation_context.hpp"

namespace aiforge::surfaces {
namespace {

[[nodiscard]] auto one_shot_error(const OneShotErrorCode code,
                                  std::string message)
    -> std::unexpected<OneShotError> {
  return std::unexpected(OneShotError{code, std::move(message)});
}

[[nodiscard]] auto valid_utf8(const std::string_view value) -> bool {
  std::size_t index{};
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first == 0) return false;
    std::size_t length{};
    std::uint32_t codepoint{};
    if (first <= 0x7FU) {
      length = 1;
      codepoint = first;
    } else if ((first & 0xE0U) == 0xC0U) {
      length = 2;
      codepoint = first & 0x1FU;
      if (codepoint < 2) return false;
    } else if ((first & 0xF0U) == 0xE0U) {
      length = 3;
      codepoint = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (length > value.size() - index) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    if ((length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU) {
      return false;
    }
    index += length;
  }
  return true;
}

[[nodiscard]] auto sanitized(const std::string_view value)
    -> std::expected<std::string, OneShotError> {
  auto result = presentation::sanitize_untrusted_text(value);
  if (!result) {
    return one_shot_error(OneShotErrorCode::internal_failure,
                          result.error().message);
  }
  return std::move(*result);
}

[[nodiscard]] auto sanitized_inline(const std::string_view value)
    -> std::expected<std::string, OneShotError> {
  auto result = sanitized(value);
  if (!result) return result;
  std::ranges::replace(*result, '\n', ' ');
  std::ranges::replace(*result, '\t', ' ');
  return result;
}

auto write(std::ostream& stream, const std::string_view value) -> bool {
  try {
    stream << value;
    return static_cast<bool>(stream);
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto estimate_line(
    const std::vector<domain::SessionCostEstimate>& estimates) -> std::string {
  std::string result{"estimate:"};
  for (const auto& estimate : estimates) {
    result +=
        " " + std::string{domain::cost_estimate_unit_name(estimate.unit)} + "=";
    if (estimate.subtotal) {
      result += estimate.subtotal->amount().to_string();
    } else {
      result += "unavailable";
    }
    if (estimate.estimated_inferences != estimate.total_inferences ||
        !estimate.unavailable.empty() || estimate.aggregation_failure) {
      result += "[" + std::to_string(estimate.estimated_inferences) + "/" +
                std::to_string(estimate.total_inferences);
      for (const auto& failure : estimate.unavailable) {
        result +=
            ";" +
            std::string{domain::cost_estimate_reason_name(failure.reason)} +
            "=" + std::to_string(failure.count);
      }
      if (estimate.aggregation_failure) {
        result += ";" + std::string{domain::cost_estimate_reason_name(
                            *estimate.aggregation_failure)};
      }
      result += "]";
    }
  }
  result += " (catalog-derived)";
  result.push_back('\n');
  return result;
}

[[nodiscard]] auto spend_line(const domain::SessionSpendSummary& spend)
    -> std::string {
  auto result = std::string{"spend: USD="};
  if (!spend.accounted) {
    result += "unavailable cap=" + spend.ceiling.amount().to_string();
  } else {
    result += spend.accounted->amount().to_string() +
              " cap=" + spend.ceiling.amount().to_string() +
              " remaining=" + spend.remaining->amount().to_string() +
              (spend.reached ? " reached" : " open");
  }
  result += " (provider-reported or catalog-derived)\n";
  return result;
}

template <typename IdType>
[[nodiscard]] auto make_id(const std::string_view prefix,
                           const std::uint64_t suffix)
    -> std::expected<IdType, OneShotError> {
  auto id = IdType::from(std::string{prefix} + '-' + std::to_string(suffix));
  if (!id) {
    return one_shot_error(OneShotErrorCode::internal_failure,
                          "could not create one-shot identity");
  }
  return std::move(*id);
}

[[nodiscard]] auto next_suffix() -> std::uint64_t {
  static std::atomic<std::uint64_t> sequence{};
  const auto count = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto tick = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return tick ^ count;
}

struct SpendState {
  domain::UsageLedgerProjection ledger;
  domain::SessionSpendCeilingProjection ceiling;
};

[[nodiscard]] auto rebuild_spend_ceiling(const domain::SessionEventLog& log)
    -> std::expected<domain::SessionSpendCeilingProjection, OneShotError> {
  domain::SessionSpendCeilingProjection ceiling;
  for (const auto& event : log.events()) {
    if (!ceiling.apply(event)) {
      return one_shot_error(OneShotErrorCode::run_failed,
                            "session spend ceiling history is invalid");
    }
  }
  return ceiling;
}

[[nodiscard]] auto rebuild_spend_state(const domain::SessionEventLog& log)
    -> std::expected<SpendState, OneShotError> {
  SpendState state;
  for (const auto& event : log.events()) {
    if (!state.ledger.apply(event) || !state.ceiling.apply(event)) {
      return one_shot_error(OneShotErrorCode::run_failed,
                            "session spend history is invalid");
    }
  }
  return state;
}

[[nodiscard]] auto apply_requested_spend_ceiling(
    runtime::RunKernel& kernel,
    const std::optional<domain::SessionSpendCeiling>& requested)
    -> std::expected<void, OneShotError> {
  auto ceiling = rebuild_spend_ceiling(kernel.event_log());
  if (!ceiling) return std::unexpected(std::move(ceiling.error()));
  if (!requested) return {};
  if (ceiling->ceiling()) {
    const auto ordering =
        domain::compare(requested->amount(), ceiling->ceiling()->amount());
    if (ordering == std::strong_ordering::greater) {
      return one_shot_error(OneShotErrorCode::invalid_input,
                            "session spend ceiling cannot be widened");
    }
    if (ordering == std::strong_ordering::equal) return {};
  }
  const auto suffix = next_suffix();
  auto run_id = make_id<domain::RunId>("spend-policy", suffix);
  auto surface_id = make_id<domain::SurfaceId>("session-policy", suffix);
  auto workspace_id = make_id<domain::WorkspaceId>("chat", suffix);
  auto permission_id = make_id<domain::PermissionProfileId>("observe", suffix);
  if (!run_id || !surface_id || !workspace_id || !permission_id) {
    return one_shot_error(OneShotErrorCode::internal_failure,
                          "session spend identity generation failed");
  }
  auto recorded = kernel.record_session_spend_ceiling(
      {*run_id,
       {*surface_id, *workspace_id, *permission_id, std::nullopt},
       *requested,
       domain::SessionSpendCeilingSource::command_line});
  if (!recorded) {
    return one_shot_error(OneShotErrorCode::run_failed,
                          recorded.error().message);
  }
  return {};
}

class Wake final : public runtime::RunWakeSink {
 public:
  auto wake() noexcept -> void override {
    {
      std::lock_guard lock(m_mutex);
      ++m_generation;
    }
    m_ready.notify_all();
  }

  auto wait(const std::size_t observed, const std::stop_token stop_token)
      -> void {
    std::unique_lock lock(m_mutex);
    static_cast<void>(
        m_ready.wait_for(lock, stop_token, std::chrono::milliseconds{250},
                         [&] { return m_generation != observed; }));
  }

  [[nodiscard]] auto generation() -> std::size_t {
    std::lock_guard lock(m_mutex);
    return m_generation;
  }

 private:
  std::mutex m_mutex;
  std::condition_variable_any m_ready;
  std::size_t m_generation{};
};

struct ResolvedPersona {
  std::optional<domain::PersonaDocument> document;
  std::optional<domain::PersonaSelection> selection;
};

[[nodiscard]] auto resolve_persona(persona::PersonaSource* source,
                                   const persona::PersonaDirective& directive,
                                   const domain::SessionEventLog& event_log,
                                   const std::stop_token stop_token)
    -> std::expected<ResolvedPersona, OneShotError> {
  if ((directive.kind == persona::PersonaDirectiveKind::select) !=
          directive.name.has_value() ||
      directive.source == domain::PersonaSelectionSource::unknown) {
    return one_shot_error(OneShotErrorCode::invalid_input,
                          "persona selection is invalid");
  }
  auto latest = runtime::latest_persona_selection(event_log);
  if (!latest) {
    return one_shot_error(OneShotErrorCode::run_failed,
                          "persona history is invalid");
  }
  std::optional<domain::PersonaReference> previous;
  if (*latest &&
      (*latest)->action == domain::PersonaSelectionAction::selected) {
    previous = (*latest)->persona;
  }
  if (directive.kind == persona::PersonaDirectiveKind::disable) {
    return ResolvedPersona{
        std::nullopt,
        domain::PersonaSelection{domain::PersonaSelectionAction::disabled,
                                 directive.source, std::nullopt, previous}};
  }
  if (directive.kind == persona::PersonaDirectiveKind::inherit) {
    if (!*latest) return ResolvedPersona{};
    if ((*latest)->action == domain::PersonaSelectionAction::disabled) {
      return ResolvedPersona{
          std::nullopt,
          domain::PersonaSelection{domain::PersonaSelectionAction::disabled,
                                   domain::PersonaSelectionSource::resumed,
                                   std::nullopt, std::nullopt}};
    }
  }
  const auto name = directive.kind == persona::PersonaDirectiveKind::select
                        ? *directive.name
                        : previous->name;
  if (source == nullptr) {
    return one_shot_error(OneShotErrorCode::context_failed,
                          "persona source is unavailable");
  }
  auto loaded = source->load(name, {}, stop_token);
  if (!loaded) {
    return one_shot_error(
        loaded.error().code == persona::PersonaErrorCode::cancelled
            ? OneShotErrorCode::cancelled
        : loaded.error().code == persona::PersonaErrorCode::invalid_name
            ? OneShotErrorCode::invalid_input
            : OneShotErrorCode::context_failed,
        loaded.error().message);
  }
  if (!domain::validate_persona_document(*loaded)) {
    return one_shot_error(OneShotErrorCode::context_failed,
                          "persona document is invalid");
  }
  if (directive.kind == persona::PersonaDirectiveKind::inherit &&
      loaded->reference != *previous) {
    return one_shot_error(OneShotErrorCode::context_failed,
                          "persona changed since the session was recorded; "
                          "explicitly select it or disable it");
  }
  const auto selection_source =
      directive.kind == persona::PersonaDirectiveKind::inherit
          ? domain::PersonaSelectionSource::resumed
          : directive.source;
  auto reference = loaded->reference;
  return ResolvedPersona{
      std::move(*loaded),
      domain::PersonaSelection{domain::PersonaSelectionAction::selected,
                               selection_source, std::move(reference),
                               std::move(previous)}};
}

[[nodiscard]] auto render_events(const std::vector<domain::RunEvent>& events,
                                 std::ostream& output, std::ostream& error,
                                 std::optional<domain::DomainError>& run_error)
    -> std::expected<void, OneShotError> {
  for (const auto& event : events) {
    if (const auto* delta =
            std::get_if<domain::AssistantContentDeltaAdded>(&event.payload)) {
      if (const auto* text = std::get_if<domain::TextBlock>(&delta->delta)) {
        auto clean = sanitized(text->text);
        if (!clean) return std::unexpected(std::move(clean.error()));
        if (!write(output, *clean)) {
          return one_shot_error(OneShotErrorCode::output_failed,
                                "completion output failed");
        }
      } else if (const auto* citation =
                     std::get_if<domain::CitationBlock>(&delta->delta)) {
        auto uri = sanitized_inline(citation->uri);
        if (!uri) return std::unexpected(std::move(uri.error()));
        std::string line = "citation: " + *uri;
        if (citation->title) {
          auto title = sanitized_inline(*citation->title);
          if (!title) return std::unexpected(std::move(title.error()));
          line += " (" + *title + ')';
        }
        line.push_back('\n');
        if (!write(error, line)) {
          return one_shot_error(OneShotErrorCode::output_failed,
                                "diagnostic output failed");
        }
      } else {
        return one_shot_error(OneShotErrorCode::run_failed,
                              "backend produced unsupported one-shot content");
      }
    } else if (const auto* failed =
                   std::get_if<domain::RunFailed>(&event.payload)) {
      run_error = failed->error;
    }
  }
  return {};
}

} // namespace

OneShotSurface::OneShotSurface(backend::Backend& backend,
                               backend::ModelContextProvider& model_context,
                               OneShotLimits limits,
                               persona::PersonaSource* persona_source,
                               OneShotDependencies dependencies)
    : m_backend(backend), m_model_context(model_context),
      m_persona_source(persona_source), m_limits(limits),
      m_dependencies(std::move(dependencies)) {
}

OneShotSurface::OneShotSurface(backend::Backend& backend,
                               backend::ModelContextProvider& model_context,
                               storage::SessionStore& session_store,
                               OneShotLimits limits,
                               persona::PersonaSource* persona_source,
                               OneShotDependencies dependencies)
    : m_backend(backend), m_model_context(model_context),
      m_session_store(&session_store), m_persona_source(persona_source),
      m_limits(limits), m_dependencies(std::move(dependencies)) {
}

auto OneShotSurface::run(OneShotRequest request, std::ostream& output,
                         std::ostream& error, const std::stop_token stop_token)
    -> std::expected<OneShotResult, OneShotError> {
  try {
    if (m_limits.maximum_input_bytes == 0 ||
        m_limits.preferred_output_tokens == 0) {
      return one_shot_error(OneShotErrorCode::internal_failure,
                            "one-shot limits are invalid");
    }
    if (request.prompt.empty() || !valid_utf8(request.prompt)) {
      return one_shot_error(OneShotErrorCode::invalid_input,
                            "prompt must be nonempty UTF-8 text without NUL");
    }
    const auto evidence_size =
        request.stdin_evidence ? request.stdin_evidence->size() : 0;
    if (request.prompt.size() > m_limits.maximum_input_bytes ||
        evidence_size > m_limits.maximum_input_bytes - request.prompt.size()) {
      return one_shot_error(OneShotErrorCode::input_too_large,
                            "one-shot input exceeds the configured limit");
    }
    if (request.stdin_evidence &&
        (!valid_utf8(*request.stdin_evidence) ||
         request.stdin_evidence->find('\0') != std::string::npos)) {
      return one_shot_error(
          OneShotErrorCode::invalid_input,
          "standard input must be UTF-8 text without binary NUL bytes");
    }
    if (stop_token.stop_requested()) {
      return one_shot_error(OneShotErrorCode::cancelled, "request cancelled");
    }

    auto model = m_model_context.lookup(request.model_id, stop_token);
    if (!model) {
      if (model.error().kind == backend::BackendErrorKind::cancelled ||
          stop_token.stop_requested()) {
        return one_shot_error(OneShotErrorCode::cancelled, "request cancelled");
      }
      return one_shot_error(OneShotErrorCode::model_lookup_failed,
                            "model context lookup failed");
    }
    if (model->model_id != request.model_id ||
        model->context_window_tokens == 0) {
      return one_shot_error(OneShotErrorCode::model_lookup_failed,
                            "model context metadata is invalid");
    }
    auto output_tokens = m_limits.preferred_output_tokens;
    if (model->maximum_output_tokens) {
      output_tokens = std::min(output_tokens, *model->maximum_output_tokens);
    }
    if (output_tokens == 0 || output_tokens >= model->context_window_tokens) {
      return one_shot_error(OneShotErrorCode::context_failed,
                            "model context capacity is too small");
    }

    const auto suffix = next_suffix();
    const bool durable =
        request.session_mode != OneShotRequest::SessionMode::ephemeral &&
        m_session_store != nullptr;
    const bool exact_resume =
        request.session_mode == OneShotRequest::SessionMode::resume;
    if (exact_resume != request.session_id.has_value()) {
      return one_shot_error(OneShotErrorCode::invalid_input,
                            "session selection is invalid");
    }
    if (request.session_mode != OneShotRequest::SessionMode::create &&
        request.session_mode != OneShotRequest::SessionMode::ephemeral &&
        m_session_store == nullptr) {
      return one_shot_error(OneShotErrorCode::run_failed,
                            "durable session storage is unavailable");
    }

    auto generated_session_id = make_id<domain::SessionId>("session", suffix);
    if (!generated_session_id) {
      return one_shot_error(OneShotErrorCode::internal_failure,
                            "could not create one-shot session identity");
    }
    auto session_id = request.session_id.value_or(*generated_session_id);
    if (request.session_mode == OneShotRequest::SessionMode::continue_latest) {
      auto sessions = m_session_store->list_sessions(1, stop_token);
      if (!sessions) {
        return one_shot_error(stop_token.stop_requested()
                                  ? OneShotErrorCode::cancelled
                                  : OneShotErrorCode::run_failed,
                              stop_token.stop_requested()
                                  ? "request cancelled"
                                  : "recent sessions could not be listed");
      }
      if (sessions->empty()) {
        return one_shot_error(OneShotErrorCode::invalid_input,
                              "there is no durable session to continue");
      }
      session_id = sessions->front().session_id;
    }

    Wake wake;
    std::unique_ptr<runtime::RunKernel> kernel;
    if (durable) {
      const auto mode =
          request.session_mode == OneShotRequest::SessionMode::create
              ? runtime::DurableSessionMode::create
              : runtime::DurableSessionMode::resume;
      auto opened = runtime::RunKernel::open_durable(
          {session_id, mode,
           std::chrono::floor<std::chrono::milliseconds>(
               std::chrono::system_clock::now())},
          *m_session_store, m_backend, &wake, runtime::TimestampSource{},
          runtime::RunKernelLimits{}, m_dependencies.tools,
          m_dependencies.tool_policy);
      if (!opened) {
        return one_shot_error(stop_token.stop_requested()
                                  ? OneShotErrorCode::cancelled
                                  : OneShotErrorCode::run_failed,
                              stop_token.stop_requested()
                                  ? "request cancelled"
                                  : "durable session could not be opened");
      }
      kernel = std::move(*opened);
    } else {
      kernel = std::make_unique<runtime::RunKernel>(
          session_id, m_backend, &wake, runtime::TimestampSource{},
          runtime::RunKernelLimits{}, m_dependencies.tools,
          m_dependencies.tool_policy);
    }

    if (auto applied = apply_requested_spend_ceiling(
            *kernel, request.session_spend_ceiling);
        !applied) {
      return std::unexpected(std::move(applied.error()));
    }
    auto opening_ceiling = rebuild_spend_ceiling(kernel->event_log());
    if (!opening_ceiling) {
      return std::unexpected(std::move(opening_ceiling.error()));
    }
    if (opening_ceiling->ceiling()) {
      auto opening_spend = rebuild_spend_state(kernel->event_log());
      if (!opening_spend) {
        return std::unexpected(std::move(opening_spend.error()));
      }
      const auto spend = domain::summarize_session_spend(
          opening_spend->ledger.records(), *opening_spend->ceiling.ceiling());
      if (!spend.accounted) {
        return one_shot_error(OneShotErrorCode::spend_accounting_unavailable,
                              "session spend accounting is unavailable; "
                              "refusing another inference");
      }
      if (spend.reached) {
        return one_shot_error(OneShotErrorCode::spend_ceiling_reached,
                              "session spend ceiling reached (USD " +
                                  spend.accounted->amount().to_string() +
                                  " of " + spend.ceiling.amount().to_string() +
                                  ")");
      }
    }

    auto resolved_persona = resolve_persona(m_persona_source, request.persona,
                                            kernel->event_log(), stop_token);
    if (!resolved_persona) {
      return std::unexpected(std::move(resolved_persona.error()));
    }

    auto run_id = make_id<domain::RunId>("run", suffix);
    auto inference_id = make_id<domain::InferenceId>("inference", suffix);
    auto user_message_id = make_id<domain::MessageId>("user", suffix);
    auto assistant_message_id = make_id<domain::MessageId>("assistant", suffix);
    auto runtime_message_id = make_id<domain::MessageId>("runtime", suffix);
    auto runtime_entry_id =
        make_id<domain::ContextEntryId>("runtime-entry", suffix);
    auto user_entry_id = make_id<domain::ContextEntryId>("user-entry", suffix);
    auto runtime_source_id =
        make_id<domain::ContextSourceId>("runtime-source", suffix);
    auto user_source_id =
        make_id<domain::ContextSourceId>("user-source", suffix);
    auto surface_id = make_id<domain::SurfaceId>("one-shot", suffix);
    auto workspace_id = make_id<domain::WorkspaceId>("chat", suffix);
    auto permission_id =
        make_id<domain::PermissionProfileId>("observe", suffix);
    if (!run_id || !inference_id || !user_message_id || !assistant_message_id ||
        !runtime_message_id || !runtime_entry_id || !user_entry_id ||
        !runtime_source_id || !user_source_id || !surface_id || !workspace_id ||
        !permission_id) {
      return one_shot_error(OneShotErrorCode::internal_failure,
                            "could not create one-shot identities");
    }

    domain::Message user_message{*user_message_id,
                                 domain::Role::user,
                                 {domain::TextBlock{request.prompt}},
                                 std::nullopt};
    auto history = detail::replayed_conversation(kernel->event_log(), suffix);
    if (!history) {
      return one_shot_error(OneShotErrorCode::run_failed,
                            std::move(history.error()));
    }
    auto content = std::move(*history);
    if (m_dependencies.memory_controller != nullptr) {
      std::uint64_t mandatory = detail::runtime_contract.size() +
                                request.prompt.size() + evidence_size;
      for (const auto& item : content)
        mandatory += item.estimated_tokens;
      if (resolved_persona->document) {
        mandatory += resolved_persona->document->text.size();
      }
      const auto maximum_input = model->context_window_tokens - output_tokens;
      const auto available = mandatory < maximum_input
                                 ? maximum_input - mandatory
                                 : std::uint64_t{};
      auto memory = runtime::select_memory_context(
          *m_dependencies.memory_controller,
          {m_dependencies.repository_id,
           m_dependencies.memory_settings.context_tokens, available});
      if (!memory) {
        return one_shot_error(OneShotErrorCode::context_failed,
                              memory.error().message);
      }
      for (auto& item : *memory) {
        item.order = static_cast<std::uint64_t>(content.size()) + 1;
        content.push_back(std::move(item));
      }
    }
    const auto user_order = static_cast<std::uint64_t>(content.size()) + 1;
    content.push_back(
        {*user_entry_id,
         domain::ContextContentKind::conversation,
         user_message,
         {*user_source_id, std::string{"command-line"}, std::nullopt},
         user_order,
         request.prompt.size()});

    domain::ContextBuildInput build_input{
        {model->context_window_tokens, output_tokens, 0},
        {{*runtime_entry_id,
          domain::InstructionLayer::application_runtime,
          domain::InstructionOperation::add,
          std::nullopt,
          domain::Message{
              *runtime_message_id,
              domain::Role::system,
              {domain::TextBlock{std::string{detail::runtime_contract}}},
              std::nullopt},
          {*runtime_source_id, std::string{"aiforge:runtime"}, std::nullopt},
          0,
          1,
          detail::runtime_contract.size()}},
        std::move(content)};

    if (resolved_persona->document) {
      auto persona_instruction = runtime::persona_instruction_input(
          *resolved_persona->document, resolved_persona->document->text.size());
      if (!persona_instruction) {
        return one_shot_error(OneShotErrorCode::context_failed,
                              persona_instruction.error().message);
      }
      build_input.instructions.push_back(std::move(*persona_instruction));
    }

    if (request.stdin_evidence && !request.stdin_evidence->empty()) {
      auto evidence_message_id =
          make_id<domain::MessageId>("stdin-message", suffix);
      auto evidence_entry_id =
          make_id<domain::ContextEntryId>("stdin-entry", suffix);
      auto evidence_source_id =
          make_id<domain::ContextSourceId>("stdin-source", suffix);
      if (!evidence_message_id || !evidence_entry_id || !evidence_source_id) {
        return one_shot_error(OneShotErrorCode::internal_failure,
                              "could not create stdin evidence identity");
      }
      build_input.content.push_back(
          {*evidence_entry_id,
           domain::ContextContentKind::evidence,
           {*evidence_message_id,
            domain::Role::evidence,
            {domain::TextBlock{std::move(*request.stdin_evidence)}},
            std::nullopt},
           {*evidence_source_id, std::string{"stdin"}, std::nullopt},
           user_order + 1,
           evidence_size});
    }

    auto context = runtime::ContextBuilder{}.build(std::move(build_input));
    if (!context) {
      return one_shot_error(OneShotErrorCode::context_failed,
                            "one-shot input exceeds model context capacity");
    }

    backend::BackendRequest backend_request{
        *inference_id,
        *assistant_message_id,
        request.model_id,
        std::move(*context),
        {},
        {std::nullopt, output_tokens, std::nullopt, {}}};
    auto started = kernel->start(
        {*run_id,
         {*surface_id, *workspace_id, *permission_id,
          resolved_persona->document
              ? std::optional<domain::PersonaId>{resolved_persona->document
                                                     ->reference.persona_id}
              : std::nullopt},
         std::move(user_message),
         std::move(backend_request),
         std::move(request.provenance),
         std::move(resolved_persona->selection),
         model->pricing_observation});
    if (!started) {
      return one_shot_error(OneShotErrorCode::run_failed,
                            "one-shot run could not start");
    }

    auto session_line = "session: " + std::string{session_id.value()};
    if (!durable) session_line += " (ephemeral)";
    session_line.push_back('\n');
    if (!write(error, session_line)) {
      static_cast<void>(
          kernel->cancel(*run_id, *inference_id, "diagnostic output failure"));
      return one_shot_error(OneShotErrorCode::output_failed,
                            "diagnostic output failed");
    }

    bool cancellation_sent{};
    std::optional<domain::DomainError> run_error;
    auto generation = wake.generation();
    while (kernel->active_run_id()) {
      if (stop_token.stop_requested() && !cancellation_sent) {
        const auto active_run = kernel->active_run_id();
        const auto active_inference = kernel->active_inference_id();
        if (active_run && active_inference) {
          auto cancelled =
              kernel->cancel(*active_run, *active_inference, "interrupt");
          if (!cancelled) {
            return one_shot_error(OneShotErrorCode::run_failed,
                                  "one-shot cancellation failed");
          }
          cancellation_sent = true;
        }
      }
      auto events = kernel->drain();
      if (!events) {
        return one_shot_error(OneShotErrorCode::run_failed,
                              "one-shot run failed internally");
      }
      auto rendered = render_events(*events, output, error, run_error);
      if (!rendered) {
        if (const auto active_run = kernel->active_run_id()) {
          if (const auto active_inference = kernel->active_inference_id()) {
            static_cast<void>(kernel->cancel(*active_run, *active_inference,
                                             "output failure"));
          }
        }
        return std::unexpected(std::move(rendered.error()));
      }
      if (kernel->active_run_id() && events->empty()) {
        wake.wait(generation, stop_token);
      }
      generation = wake.generation();
    }

    if (m_dependencies.memory_controller != nullptr) {
      auto captured = m_dependencies.memory_controller->capture_committed(
          kernel->event_log().session_id(), kernel->event_log().events(),
          m_dependencies.memory_settings, m_dependencies.repository_id,
          m_dependencies.runtime_version);
      if (!captured) {
        return one_shot_error(OneShotErrorCode::run_failed,
                              captured.error().message);
      }
    }

    const auto* projection = kernel->projection(*run_id);
    if (projection == nullptr) {
      return one_shot_error(OneShotErrorCode::run_failed,
                            "one-shot run has no projection");
    }
    if (projection->status() == domain::RunStatus::cancelled) {
      return one_shot_error(OneShotErrorCode::cancelled, "request cancelled");
    }
    if (projection->status() != domain::RunStatus::completed) {
      return one_shot_error(OneShotErrorCode::run_failed,
                            run_error ? run_error->message
                                      : "one-shot run failed");
    }

    const auto& usage = projection->usage();
    const auto usage_line =
        "usage: input=" + std::to_string(usage.input_tokens) +
        " output=" + std::to_string(usage.output_tokens) +
        " cached=" + std::to_string(usage.cached_input_tokens) +
        " reasoning=" + std::to_string(usage.reasoning_tokens) + '\n';
    if (!write(error, usage_line)) {
      return one_shot_error(OneShotErrorCode::output_failed,
                            "diagnostic output failed");
    }
    const auto& reported_cost = projection->reported_cost();
    std::string cost_line{"cost:"};
    if (reported_cost) {
      for (const auto& amount : reported_cost->amounts()) {
        cost_line += " " + std::string{amount.unit()} + "=" +
                     amount.amount().to_string();
      }
      cost_line += " (provider-reported)";
    } else {
      cost_line += " unavailable";
    }
    cost_line.push_back('\n');
    if (!write(error, cost_line)) {
      return one_shot_error(OneShotErrorCode::output_failed,
                            "diagnostic output failed");
    }
    domain::UsageLedgerProjection run_ledger;
    for (const auto& event : kernel->event_log().events()) {
      if (event.metadata.run_id != *run_id) continue;
      auto applied = run_ledger.apply(event);
      if (!applied) {
        return one_shot_error(OneShotErrorCode::run_failed,
                              "one-shot cost estimate replay failed");
      }
    }
    std::vector<domain::SessionCostEstimate> estimates;
    estimates.reserve(2);
    estimates.push_back(domain::summarize_cost_estimates(
        run_ledger.records(), domain::CostEstimateUnit::usd));
    estimates.push_back(domain::summarize_cost_estimates(
        run_ledger.records(), domain::CostEstimateUnit::venice_diem));
    if (!write(error, estimate_line(estimates))) {
      return one_shot_error(OneShotErrorCode::output_failed,
                            "diagnostic output failed");
    }
    std::optional<domain::SessionSpendSummary> spend;
    auto final_ceiling = rebuild_spend_ceiling(kernel->event_log());
    if (!final_ceiling) {
      return std::unexpected(std::move(final_ceiling.error()));
    }
    if (final_ceiling->ceiling()) {
      auto final_spend_state = rebuild_spend_state(kernel->event_log());
      if (!final_spend_state) {
        return std::unexpected(std::move(final_spend_state.error()));
      }
      spend = domain::summarize_session_spend(
          final_spend_state->ledger.records(),
          *final_spend_state->ceiling.ceiling());
      if (!write(error, spend_line(*spend))) {
        return one_shot_error(OneShotErrorCode::output_failed,
                              "diagnostic output failed");
      }
    }
    return OneShotResult{usage,      reported_cost, std::move(estimates),
                         session_id, durable,       std::move(spend)};
  } catch (...) {
    return one_shot_error(OneShotErrorCode::internal_failure,
                          "one-shot execution failed internally");
  }
}

} // namespace aiforge::surfaces
