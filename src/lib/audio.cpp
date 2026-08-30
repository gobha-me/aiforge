#include <aiforge/surfaces/audio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/run_kernel.hpp>

namespace aiforge::surfaces {
namespace {

constexpr std::string_view speech_instruction =
    "Synthesize one PCM WAV speech artifact. Do not invoke tools.";
constexpr std::string_view transcription_instruction =
    "Transcribe the referenced PCM WAV artifact as text. Do not invoke tools.";
constexpr std::size_t maximum_text_bytes = 1024U * 1024U;

[[nodiscard]] auto failure(AudioErrorCode code, std::string message)
    -> std::unexpected<AudioError> {
  return std::unexpected(AudioError{code, std::move(message)});
}

[[nodiscard]] auto safe_text(const std::string_view value) -> bool {
  if (value.empty() || value.size() > maximum_text_bytes) return false;
  std::size_t index{};
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if ((first < 0x20U && first != '\n' && first != '\t') || first == 0x7FU)
      return false;
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

[[nodiscard]] auto next_suffix() -> std::uint64_t {
  static std::atomic<std::uint64_t> sequence{};
  const auto count = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto tick = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return tick ^ count;
}

template <typename IdType>
[[nodiscard]] auto make_id(const std::string_view prefix,
                           const std::uint64_t suffix)
    -> std::expected<IdType, AudioError> {
  auto id = IdType::from(std::string{prefix} + '-' + std::to_string(suffix));
  if (!id)
    return failure(AudioErrorCode::internal_failure,
                   "could not create audio run identity");
  return std::move(*id);
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

  [[nodiscard]] auto generation() const -> std::size_t {
    std::lock_guard lock(m_mutex);
    return m_generation;
  }

 private:
  mutable std::mutex m_mutex;
  std::condition_variable_any m_ready;
  std::size_t m_generation{};
};

struct RunIds {
  domain::SessionId session;
  domain::RunId run;
  domain::InferenceId inference;
  domain::MessageId user_message;
  domain::MessageId assistant_message;
  domain::MessageId runtime_message;
  domain::ContextEntryId runtime_entry;
  domain::ContextEntryId user_entry;
  domain::ContextSourceId runtime_source;
  domain::ContextSourceId user_source;
  domain::SurfaceId surface;
  domain::WorkspaceId workspace;
  domain::PermissionProfileId permission;
};

[[nodiscard]] auto run_ids(const std::string_view kind,
                           const std::uint64_t suffix)
    -> std::expected<RunIds, AudioError> {
  auto session = make_id<domain::SessionId>("audio-session", suffix);
  auto run = make_id<domain::RunId>("audio-run", suffix);
  auto inference = make_id<domain::InferenceId>("audio-inference", suffix);
  auto user = make_id<domain::MessageId>("audio-user", suffix);
  auto assistant = make_id<domain::MessageId>("audio-assistant", suffix);
  auto runtime_message = make_id<domain::MessageId>("audio-runtime", suffix);
  auto runtime_entry =
      make_id<domain::ContextEntryId>("audio-runtime-entry", suffix);
  auto user_entry = make_id<domain::ContextEntryId>("audio-user-entry", suffix);
  auto runtime_source =
      make_id<domain::ContextSourceId>("audio-runtime-source", suffix);
  auto user_source =
      make_id<domain::ContextSourceId>("audio-user-source", suffix);
  auto surface = make_id<domain::SurfaceId>(kind, suffix);
  auto workspace = make_id<domain::WorkspaceId>("media", suffix);
  auto permission = make_id<domain::PermissionProfileId>("observe", suffix);
  if (!session || !run || !inference || !user || !assistant ||
      !runtime_message || !runtime_entry || !user_entry || !runtime_source ||
      !user_source || !surface || !workspace || !permission) {
    return failure(AudioErrorCode::internal_failure,
                   "could not create audio run identities");
  }
  return RunIds{std::move(*session),        std::move(*run),
                std::move(*inference),      std::move(*user),
                std::move(*assistant),      std::move(*runtime_message),
                std::move(*runtime_entry),  std::move(*user_entry),
                std::move(*runtime_source), std::move(*user_source),
                std::move(*surface),        std::move(*workspace),
                std::move(*permission)};
}

[[nodiscard]] auto build_context(const RunIds& ids,
                                 const std::string_view instruction,
                                 const domain::Message& user_message,
                                 const std::uint64_t user_bytes)
    -> std::expected<domain::ConstructedContext, AudioError> {
  const auto runtime_bytes = static_cast<std::uint64_t>(instruction.size());
  if (user_bytes >
      std::numeric_limits<std::uint64_t>::max() - runtime_bytes - 1U) {
    return failure(AudioErrorCode::context_failed,
                   "audio context capacity overflowed");
  }
  domain::ContextBuildInput input{
      {user_bytes + runtime_bytes + 1U, 1, 0},
      {{ids.runtime_entry,
        domain::InstructionLayer::application_runtime,
        domain::InstructionOperation::add,
        std::nullopt,
        domain::Message{ids.runtime_message,
                        domain::Role::system,
                        {domain::TextBlock{std::string{instruction}}},
                        std::nullopt},
        {ids.runtime_source, "aiforge:audio", std::nullopt},
        0,
        1,
        runtime_bytes}},
      {{ids.user_entry,
        domain::ContextContentKind::conversation,
        user_message,
        {ids.user_source, "command-line", std::nullopt},
        1,
        user_bytes}}};
  auto context = runtime::ContextBuilder{}.build(std::move(input));
  if (!context)
    return failure(AudioErrorCode::context_failed,
                   "audio context could not be built");
  return std::move(*context);
}

struct RunObservation {
  std::optional<domain::ArtifactMetadata> artifact;
  std::string text;
};

[[nodiscard]] auto run_audio(backend::Backend& backend,
                             storage::SessionStore& store, RunIds ids,
                             domain::Message user_message,
                             domain::ConstructedContext context,
                             domain::ModelId model_id,
                             std::optional<domain::RunProvenance> provenance,
                             std::vector<domain::ArtifactMetadata> imported,
                             const std::stop_token stop_token)
    -> std::expected<RunObservation, AudioError> {
  Wake wake;
  auto opened = runtime::RunKernel::open_durable(
      {ids.session, runtime::DurableSessionMode::create,
       std::chrono::floor<std::chrono::milliseconds>(
           std::chrono::system_clock::now())},
      store, backend, &wake);
  if (!opened)
    return failure(AudioErrorCode::run_failed,
                   "durable audio session could not be opened");
  auto& kernel = **opened;
  auto started =
      kernel.start({ids.run,
                    {ids.surface, ids.workspace, ids.permission, std::nullopt},
                    std::move(user_message),
                    {ids.inference,
                     ids.assistant_message,
                     std::move(model_id),
                     std::move(context),
                     {},
                     {}},
                    std::move(provenance),
                    std::nullopt,
                    std::nullopt,
                    std::move(imported)});
  if (!started)
    return failure(AudioErrorCode::run_failed, "audio run could not start");

  RunObservation observation;
  std::optional<domain::DomainError> run_failure;
  bool cancellation_sent{};
  auto generation = wake.generation();
  while (kernel.active_run_id()) {
    if (stop_token.stop_requested() && !cancellation_sent) {
      auto cancelled = kernel.cancel(ids.run, ids.inference, "interrupt");
      if (!cancelled)
        return failure(AudioErrorCode::run_failed, "audio cancellation failed");
      cancellation_sent = true;
    }
    auto events = kernel.drain();
    if (!events)
      return failure(AudioErrorCode::run_failed, "audio run failed internally");
    for (const auto& event : *events) {
      if (const auto* created =
              std::get_if<domain::ArtifactCreated>(&event.payload)) {
        if (observation.artifact)
          return failure(AudioErrorCode::run_failed,
                         "audio run created multiple output artifacts");
        observation.artifact = created->artifact;
      } else if (const auto* delta =
                     std::get_if<domain::AssistantContentDeltaAdded>(
                         &event.payload)) {
        if (const auto* text = std::get_if<domain::TextBlock>(&delta->delta))
          observation.text += text->text;
      } else if (const auto* failed =
                     std::get_if<domain::RunFailed>(&event.payload)) {
        run_failure = failed->error;
      }
    }
    if (kernel.active_run_id() && events->empty())
      wake.wait(generation, stop_token);
    generation = wake.generation();
  }
  const auto* projection = kernel.projection(ids.run);
  if (projection == nullptr)
    return failure(AudioErrorCode::run_failed, "audio run has no projection");
  if (projection->status() == domain::RunStatus::cancelled)
    return failure(AudioErrorCode::cancelled, "audio request cancelled");
  if (projection->status() != domain::RunStatus::completed)
    return failure(AudioErrorCode::run_failed,
                   run_failure ? run_failure->message
                               : "audio run did not complete");
  return observation;
}

} // namespace

SpeechSurface::SpeechSurface(backend::Backend& backend,
                             storage::SessionStore& session_store)
    : m_backend(backend), m_session_store(session_store) {
}

auto SpeechSurface::synthesize(SpeechRequest request,
                               const std::stop_token stop_token)
    -> std::expected<SpeechResult, AudioError> {
  try {
    if (!safe_text(request.text))
      return failure(AudioErrorCode::invalid_input,
                     "speech text must be bounded control-safe text");
    if (stop_token.stop_requested())
      return failure(AudioErrorCode::cancelled, "speech synthesis cancelled");
    auto ids = run_ids("speech", next_suffix());
    if (!ids) return std::unexpected(std::move(ids.error()));
    domain::Message user{ids->user_message,
                         domain::Role::user,
                         {domain::TextBlock{request.text}},
                         std::nullopt};
    auto context =
        build_context(*ids, speech_instruction, user, request.text.size());
    if (!context) return std::unexpected(std::move(context.error()));
    const auto session_id = ids->session;
    const auto run_id = ids->run;
    auto observed =
        run_audio(m_backend, m_session_store, std::move(*ids), std::move(user),
                  std::move(*context), std::move(request.model_id),
                  std::move(request.provenance), {}, stop_token);
    if (!observed) return std::unexpected(std::move(observed.error()));
    if (!observed->artifact || !observed->text.empty())
      return failure(AudioErrorCode::run_failed,
                     "speech run did not produce exactly one artifact");
    return SpeechResult{session_id, run_id, std::move(*observed->artifact)};
  } catch (...) {
    return failure(AudioErrorCode::internal_failure,
                   "speech synthesis failed internally");
  }
}

TranscriptionSurface::TranscriptionSurface(backend::Backend& backend,
                                           storage::SessionStore& session_store)
    : m_backend(backend), m_session_store(session_store) {
}

auto TranscriptionSurface::transcribe(TranscriptionRequest request,
                                      const std::stop_token stop_token)
    -> std::expected<TranscriptionResult, AudioError> {
  try {
    if (request.input_artifact.media_type != "audio/wav" ||
        request.input_artifact.byte_size == 0 ||
        request.input_artifact.producing_invocation_id ||
        request.input_artifact.producing_inference_id ||
        request.input_artifact.width || request.input_artifact.height) {
      return failure(AudioErrorCode::invalid_input,
                     "transcription input artifact is invalid");
    }
    if (stop_token.stop_requested())
      return failure(AudioErrorCode::cancelled,
                     "audio transcription cancelled");
    auto ids = run_ids("transcription", next_suffix());
    if (!ids) return std::unexpected(std::move(ids.error()));
    domain::Message user{
        ids->user_message,
        domain::Role::user,
        {domain::ArtifactReferenceBlock{request.input_artifact.artifact_id,
                                        std::string{"audio to transcribe"}}},
        std::nullopt};
    auto context =
        build_context(*ids, transcription_instruction, user,
                      request.input_artifact.artifact_id.value().size() + 19U);
    if (!context) return std::unexpected(std::move(context.error()));
    const auto session_id = ids->session;
    const auto run_id = ids->run;
    const auto input_artifact = request.input_artifact;
    auto observed = run_audio(
        m_backend, m_session_store, std::move(*ids), std::move(user),
        std::move(*context), std::move(request.model_id),
        std::move(request.provenance), {request.input_artifact}, stop_token);
    if (!observed) return std::unexpected(std::move(observed.error()));
    if (observed->artifact || !safe_text(observed->text))
      return failure(AudioErrorCode::run_failed,
                     "transcription run did not produce valid text");
    return TranscriptionResult{session_id, run_id, input_artifact,
                               std::move(observed->text)};
  } catch (...) {
    return failure(AudioErrorCode::internal_failure,
                   "audio transcription failed internally");
  }
}

} // namespace aiforge::surfaces
