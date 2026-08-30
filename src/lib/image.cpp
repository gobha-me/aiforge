#include <aiforge/surfaces/image.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include <aiforge/runtime/context_builder.hpp>
#include <aiforge/runtime/run_kernel.hpp>

namespace aiforge::surfaces {
namespace {

constexpr std::string_view runtime_instruction =
    "Generate one image for the user prompt. Do not invoke tools.";
constexpr std::size_t maximum_prompt_bytes = 1024U * 1024U;

[[nodiscard]] auto image_error(const ImageErrorCode code, std::string message)
    -> std::unexpected<ImageError> {
  return std::unexpected(ImageError{code, std::move(message)});
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
    -> std::expected<IdType, ImageError> {
  auto value = IdType::from(std::string{prefix} + '-' + std::to_string(suffix));
  if (!value) {
    return image_error(ImageErrorCode::internal_failure,
                       "could not create image run identity");
  }
  return std::move(*value);
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

} // namespace

ImageSurface::ImageSurface(backend::Backend& backend,
                           storage::SessionStore& session_store)
    : m_backend(backend), m_session_store(session_store) {
}

auto ImageSurface::generate(ImageRequest request,
                            const std::stop_token stop_token)
    -> std::expected<ImageResult, ImageError> {
  try {
    if (request.prompt.empty() ||
        request.prompt.size() > maximum_prompt_bytes ||
        !valid_utf8(request.prompt)) {
      return image_error(
          ImageErrorCode::invalid_input,
          "prompt must be nonempty UTF-8 text within the configured limit");
    }
    if (stop_token.stop_requested()) {
      return image_error(ImageErrorCode::cancelled, "image request cancelled");
    }

    const auto suffix = next_suffix();
    auto session_id = make_id<domain::SessionId>("image-session", suffix);
    auto run_id = make_id<domain::RunId>("image-run", suffix);
    auto inference_id = make_id<domain::InferenceId>("image-inference", suffix);
    auto user_message_id = make_id<domain::MessageId>("image-user", suffix);
    auto assistant_message_id =
        make_id<domain::MessageId>("image-assistant", suffix);
    auto runtime_message_id =
        make_id<domain::MessageId>("image-runtime", suffix);
    auto runtime_entry_id =
        make_id<domain::ContextEntryId>("image-runtime-entry", suffix);
    auto user_entry_id =
        make_id<domain::ContextEntryId>("image-user-entry", suffix);
    auto runtime_source_id =
        make_id<domain::ContextSourceId>("image-runtime-source", suffix);
    auto user_source_id =
        make_id<domain::ContextSourceId>("image-user-source", suffix);
    auto surface_id = make_id<domain::SurfaceId>("image", suffix);
    auto workspace_id = make_id<domain::WorkspaceId>("media", suffix);
    auto permission_id =
        make_id<domain::PermissionProfileId>("observe", suffix);
    if (!session_id || !run_id || !inference_id || !user_message_id ||
        !assistant_message_id || !runtime_message_id || !runtime_entry_id ||
        !user_entry_id || !runtime_source_id || !user_source_id ||
        !surface_id || !workspace_id || !permission_id) {
      return image_error(ImageErrorCode::internal_failure,
                         "could not create image run identities");
    }

    domain::Message user_message{*user_message_id,
                                 domain::Role::user,
                                 {domain::TextBlock{request.prompt}},
                                 std::nullopt};
    const auto prompt_tokens =
        static_cast<std::uint64_t>(request.prompt.size());
    const auto runtime_tokens =
        static_cast<std::uint64_t>(runtime_instruction.size());
    if (prompt_tokens >
        std::numeric_limits<std::uint64_t>::max() - runtime_tokens - 1) {
      return image_error(ImageErrorCode::context_failed,
                         "image prompt capacity overflowed");
    }
    domain::ContextBuildInput build_input{
        {prompt_tokens + runtime_tokens + 1, 1, 0},
        {{*runtime_entry_id,
          domain::InstructionLayer::application_runtime,
          domain::InstructionOperation::add,
          std::nullopt,
          domain::Message{*runtime_message_id,
                          domain::Role::system,
                          {domain::TextBlock{std::string{runtime_instruction}}},
                          std::nullopt},
          {*runtime_source_id, std::string{"aiforge:image"}, std::nullopt},
          0,
          1,
          runtime_tokens}},
        {{*user_entry_id,
          domain::ContextContentKind::conversation,
          user_message,
          {*user_source_id, std::string{"command-line"}, std::nullopt},
          1,
          prompt_tokens}}};
    auto context = runtime::ContextBuilder{}.build(std::move(build_input));
    if (!context) {
      return image_error(ImageErrorCode::context_failed,
                         "image prompt context could not be built");
    }

    Wake wake;
    auto opened = runtime::RunKernel::open_durable(
        {*session_id, runtime::DurableSessionMode::create,
         std::chrono::floor<std::chrono::milliseconds>(
             std::chrono::system_clock::now())},
        m_session_store, m_backend, &wake);
    if (!opened) {
      return image_error(ImageErrorCode::run_failed,
                         "durable image session could not be opened");
    }
    auto& kernel = **opened;
    auto started = kernel.start(
        {*run_id,
         {*surface_id, *workspace_id, *permission_id, std::nullopt},
         std::move(user_message),
         {*inference_id,
          *assistant_message_id,
          std::move(request.model_id),
          std::move(*context),
          {},
          {}},
         std::move(request.provenance)});
    if (!started) {
      return image_error(ImageErrorCode::run_failed,
                         "image run could not start");
    }

    std::optional<domain::ArtifactMetadata> artifact;
    std::optional<domain::DomainError> run_failure;
    bool cancellation_sent{};
    auto generation = wake.generation();
    while (kernel.active_run_id()) {
      if (stop_token.stop_requested() && !cancellation_sent) {
        auto cancelled = kernel.cancel(*run_id, *inference_id, "interrupt");
        if (!cancelled) {
          return image_error(ImageErrorCode::run_failed,
                             "image cancellation failed");
        }
        cancellation_sent = true;
      }
      auto events = kernel.drain();
      if (!events) {
        return image_error(ImageErrorCode::run_failed,
                           "image run failed internally");
      }
      for (const auto& event : *events) {
        if (const auto* created =
                std::get_if<domain::ArtifactCreated>(&event.payload)) {
          if (artifact) {
            return image_error(ImageErrorCode::run_failed,
                               "image run created multiple artifacts");
          }
          artifact = created->artifact;
        } else if (const auto* failed =
                       std::get_if<domain::RunFailed>(&event.payload)) {
          run_failure = failed->error;
        }
      }
      if (kernel.active_run_id() && events->empty()) {
        wake.wait(generation, stop_token);
      }
      generation = wake.generation();
    }

    const auto* projection = kernel.projection(*run_id);
    if (projection == nullptr) {
      return image_error(ImageErrorCode::run_failed,
                         "image run has no projection");
    }
    if (projection->status() == domain::RunStatus::cancelled) {
      return image_error(ImageErrorCode::cancelled, "image request cancelled");
    }
    if (projection->status() != domain::RunStatus::completed || !artifact) {
      return image_error(ImageErrorCode::run_failed,
                         run_failure ? run_failure->message
                                     : "image run did not produce an artifact");
    }
    return ImageResult{std::move(*session_id), std::move(*run_id),
                       std::move(*artifact)};
  } catch (...) {
    return image_error(ImageErrorCode::internal_failure,
                       "image generation failed internally");
  }
}

} // namespace aiforge::surfaces
