#include <aiforge/adapters/process_audio.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <aiforge/adapters/audio_backend.hpp>
#include <aiforge/adapters/filesystem_artifact_store.hpp>
#include <aiforge/adapters/process_credentials.hpp>
#include <aiforge/adapters/process_model_catalog.hpp>
#include <aiforge/adapters/process_provenance.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/adapters/venice_audio_service.hpp>
#include <aiforge/audio/wav.hpp>
#include <aiforge/config/config.hpp>
#include <aiforge/config/file_store.hpp>
#include <aiforge/model/catalog.hpp>
#include <aiforge/surfaces/audio.hpp>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aiforge::adapters {
namespace {

constexpr std::size_t maximum_audio_bytes = 32U * 1024U * 1024U;

[[nodiscard]] auto failure(cli::CommandFailureKind kind, std::string message)
    -> std::unexpected<cli::CommandFailure> {
  return std::unexpected(cli::CommandFailure{kind, std::move(message)});
}

auto warning(std::ostream& error, const std::string_view message) -> bool {
  try {
    error << "aiforge: " << message << '\n';
    return static_cast<bool>(error);
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto valid_language(const std::optional<std::string>& language)
    -> bool {
  if (!language) return true;
  if (language->empty() || language->size() > 64 || language->front() == '-' ||
      language->back() == '-') {
    return false;
  }
  bool hyphen{};
  for (const unsigned char byte : *language) {
    if (byte == '-') {
      if (hyphen) return false;
      hyphen = true;
    } else {
      if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9'))) {
        return false;
      }
      hyphen = false;
    }
  }
  return true;
}

[[nodiscard]] auto load_process_config(std::ostream& error,
                                       const std::string& requested_model)
    -> std::expected<config::ResolvedConfig, cli::CommandFailure> {
  const auto& registry = config::builtin_config_registry();
  std::vector<config::ConfigLayer> layers;
  layers.push_back(config::ConfigLayer{
      config::ConfigSource::command_line,
      {{"model", config::ConfigValue{requested_model}, std::nullopt}},
      {}});
  auto environment = config::environment_config_layer(registry);
  if (!environment)
    return failure(cli::CommandFailureKind::runtime,
                   "configuration environment could not be read");
  layers.push_back(std::move(*environment));
  auto path = config::process_config_path();
  if (!path) {
    if (!warning(error, "warning: " + path.error().message))
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
  } else {
    auto file = config::JsonConfigFileStore{*path}.load(registry);
    if (file) {
      layers.push_back(std::move(*file));
    } else if (!warning(error, "warning: " + file.error().message)) {
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
    }
  }
  auto resolved = config::resolve_config(registry, layers);
  if (!resolved)
    return failure(cli::CommandFailureKind::runtime,
                   "configuration could not be resolved");
  for (const auto& diagnostic : resolved->diagnostics) {
    if (!warning(error, "warning: " + diagnostic.message))
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
  }
  return std::move(*resolved);
}

[[nodiscard]] auto select_model(model::CatalogService& catalog,
                                const std::string& requested,
                                const std::string_view type,
                                const std::stop_token stop_token)
    -> std::expected<domain::ModelId, cli::CommandFailure> {
  auto model_id = domain::ModelId::from(requested);
  if (!model_id)
    return failure(cli::CommandFailureKind::usage, "model ID is invalid");
  auto snapshot = catalog.snapshot(stop_token);
  if (!snapshot) {
    return failure(snapshot.error().code == model::CatalogErrorCode::cancelled
                       ? cli::CommandFailureKind::cancelled
                       : cli::CommandFailureKind::runtime,
                   snapshot.error().message);
  }
  if (model::find_model(snapshot->get(), *model_id, type) != nullptr)
    return std::move(*model_id);
  auto suggestions = model::suggest_models(snapshot->get(), requested, 3, type);
  std::string message =
      "unknown " + std::string{type} + " model '" + requested + "'";
  if (!suggestions.empty()) {
    message += "; did you mean ";
    for (std::size_t index{}; index < suggestions.size(); ++index) {
      if (index != 0)
        message += index + 1 == suggestions.size() ? " or " : ", ";
      message += "'" + suggestions[index] + "'";
    }
    message.push_back('?');
  }
  return failure(cli::CommandFailureKind::usage, std::move(message));
}

struct Stores {
  std::unique_ptr<SqliteSessionStore> sessions;
  std::unique_ptr<FilesystemArtifactStore> artifacts;
};

[[nodiscard]] auto open_stores(FilesystemArtifactStoreOpenMode mode =
                                   FilesystemArtifactStoreOpenMode::create)
    -> std::expected<Stores, cli::CommandFailure> {
  auto path = process_session_store_path();
  if (!path)
    return failure(cli::CommandFailureKind::runtime,
                   "session storage path could not be resolved");
  auto sessions = mode == FilesystemArtifactStoreOpenMode::create
                      ? SqliteSessionStore::open(*path)
                      : SqliteSessionStore::open_existing_read_only(*path);
  if (!sessions)
    return failure(cli::CommandFailureKind::runtime,
                   "session storage could not be opened");
  auto artifacts = FilesystemArtifactStore::open(
      path->parent_path() / "artifacts", {maximum_audio_bytes}, mode);
  if (!artifacts)
    return failure(cli::CommandFailureKind::runtime,
                   "artifact storage could not be opened");
  return Stores{std::move(*sessions), std::move(*artifacts)};
}

#ifndef _WIN32
class Descriptor final {
 public:
  explicit Descriptor(int value) : m_value(value) {}
  ~Descriptor() {
    if (m_value >= 0) static_cast<void>(::close(m_value));
  }
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  [[nodiscard]] auto get() const noexcept -> int { return m_value; }

 private:
  int m_value{-1};
};
#endif

[[nodiscard]] auto read_input(const std::filesystem::path& path,
                              const std::stop_token stop_token)
    -> std::expected<std::vector<std::byte>, cli::CommandFailure> {
#ifdef _WIN32
  static_cast<void>(path);
  static_cast<void>(stop_token);
  return failure(cli::CommandFailureKind::runtime,
                 "secure audio input is unavailable on this platform");
#else
  Descriptor descriptor{
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (descriptor.get() < 0)
    return failure(errno == ELOOP ? cli::CommandFailureKind::usage
                                  : cli::CommandFailureKind::runtime,
                   errno == ELOOP ? "audio input must not be a symbolic link"
                                  : "audio input could not be opened");
  struct stat before{};
  if (::fstat(descriptor.get(), &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_size < 0 ||
      static_cast<std::uint64_t>(before.st_size) > maximum_audio_bytes) {
    return failure(cli::CommandFailureKind::usage,
                   "audio input must be a bounded regular file");
  }
  std::vector<std::byte> content(static_cast<std::size_t>(before.st_size));
  std::size_t offset{};
  while (offset < content.size()) {
    if (stop_token.stop_requested())
      return failure(cli::CommandFailureKind::cancelled,
                     "audio input read cancelled");
    const auto count = ::read(descriptor.get(), content.data() + offset,
                              content.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      return failure(cli::CommandFailureKind::runtime,
                     "audio input could not be read");
    }
    if (count == 0)
      return failure(cli::CommandFailureKind::runtime,
                     "audio input changed while being read");
    offset += static_cast<std::size_t>(count);
  }
  std::byte extra{};
  if (::read(descriptor.get(), &extra, 1) != 0)
    return failure(cli::CommandFailureKind::runtime,
                   "audio input changed while being read");
  struct stat after{};
  if (::fstat(descriptor.get(), &after) != 0 || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino || before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) {
    return failure(cli::CommandFailureKind::runtime,
                   "audio input changed while being read");
  }
  return content;
#endif
}

[[nodiscard]] auto export_bytes(const storage::ArtifactRead& artifact,
                                const std::filesystem::path& path,
                                const std::stop_token stop_token)
    -> std::expected<void, cli::CommandFailure> {
#ifdef _WIN32
  static_cast<void>(artifact);
  static_cast<void>(path);
  static_cast<void>(stop_token);
  return failure(cli::CommandFailureKind::runtime,
                 "secure audio export is unavailable on this platform");
#else
  Descriptor descriptor{
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
             S_IRUSR | S_IWUSR)};
  if (descriptor.get() < 0)
    return failure(errno == EEXIST ? cli::CommandFailureKind::usage
                                   : cli::CommandFailureKind::runtime,
                   errno == EEXIST ? "output path already exists"
                                   : "output file could not be created");
  std::size_t offset{};
  while (offset < artifact.content.size()) {
    if (stop_token.stop_requested()) {
      static_cast<void>(::unlink(path.c_str()));
      return failure(cli::CommandFailureKind::cancelled, "export cancelled");
    }
    const auto count =
        ::write(descriptor.get(), artifact.content.data() + offset,
                artifact.content.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      static_cast<void>(::unlink(path.c_str()));
      return failure(cli::CommandFailureKind::runtime,
                     "output file could not be written");
    }
    offset += static_cast<std::size_t>(count);
  }
  if (::fsync(descriptor.get()) != 0) {
    static_cast<void>(::unlink(path.c_str()));
    return failure(cli::CommandFailureKind::runtime,
                   "output file could not be synchronized");
  }
  return {};
#endif
}

[[nodiscard]] auto audio_artifacts(const std::vector<domain::RunEvent>& events)
    -> std::vector<domain::ArtifactMetadata> {
  std::vector<domain::ArtifactMetadata> result;
  for (const auto& event : events) {
    const auto* created = std::get_if<domain::ArtifactCreated>(&event.payload);
    if (created != nullptr && created->artifact.media_type == "audio/wav")
      result.push_back(created->artifact);
  }
  return result;
}

[[nodiscard]] auto read_validated(FilesystemArtifactStore& store,
                                  const domain::ArtifactMetadata& metadata,
                                  const std::stop_token stop_token)
    -> std::expected<storage::ArtifactRead, cli::CommandFailure> {
  auto read = store.get(metadata, maximum_audio_bytes, stop_token);
  if (!read)
    return failure(read.error().code ==
                           storage::ArtifactStoreErrorCode::cancelled
                       ? cli::CommandFailureKind::cancelled
                       : cli::CommandFailureKind::runtime,
                   read.error().message);
  if (read->metadata != metadata || metadata.media_type != "audio/wav" ||
      !audio::validate_pcm_wav(read->content,
                               {.maximum_bytes = maximum_audio_bytes})) {
    return failure(cli::CommandFailureKind::runtime,
                   "audio artifact is not valid bounded PCM WAV");
  }
  return std::move(*read);
}

[[nodiscard]] auto command_failure(const surfaces::AudioError& error)
    -> cli::CommandFailure {
  switch (error.code) {
    case surfaces::AudioErrorCode::invalid_input:
      return {cli::CommandFailureKind::usage, error.message};
    case surfaces::AudioErrorCode::cancelled:
      return {cli::CommandFailureKind::cancelled, error.message};
    case surfaces::AudioErrorCode::context_failed:
    case surfaces::AudioErrorCode::run_failed:
    case surfaces::AudioErrorCode::internal_failure:
      return {cli::CommandFailureKind::runtime, error.message};
  }
  return {cli::CommandFailureKind::runtime, "audio command failed internally"};
}

[[nodiscard]] auto next_input_id() -> domain::ArtifactId {
  static std::atomic<std::uint64_t> sequence{};
  auto id = domain::ArtifactId::from(
      "audio-input-" +
      std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) + 1));
  return std::move(*id);
}

} // namespace

auto ProcessAudioCommand::synthesize(SynthesizeRequest request,
                                     cli::CommandEnvironment& environment,
                                     std::ostream& output, std::ostream& error)
    -> std::expected<void, cli::CommandFailure> {
  try {
    auto voice = domain::VoiceId::from(std::move(request.voice));
    if (!voice || !valid_language(request.language))
      return failure(cli::CommandFailureKind::usage,
                     "voice or language is invalid");
    auto resolved = load_process_config(error, request.model);
    if (!resolved) return std::unexpected(std::move(resolved.error()));
    auto catalog = ProcessModelCatalog::create();
    if (!catalog)
      return failure(cli::CommandFailureKind::runtime, catalog.error().message);
    auto model_id = select_model((*catalog)->service(), request.model, "tts",
                                 environment.stop_token);
    if (!model_id) return std::unexpected(std::move(model_id.error()));
    auto credential = resolve_process_credential(error);
    if (!credential) return std::unexpected(std::move(credential.error()));
    if (!credential->credential)
      return failure(cli::CommandFailureKind::runtime,
                     "Venice credential is not configured; run 'aiforge login' "
                     "or set VENICE_API_KEY");
    auto stores = open_stores();
    if (!stores) return std::unexpected(std::move(stores.error()));
    auto resolved_credential = std::move(*credential->credential);
    const auto credential_source = resolved_credential.source;
    VeniceAudioService service{std::move(resolved_credential.secret)};
    SpeechBackend backend{service, *stores->artifacts, std::move(*voice),
                          std::move(request.language)};
    auto provenance = process_run_provenance(*resolved, *model_id, "venice",
                                             credential_source);
    surfaces::SpeechSurface surface{backend, *stores->sessions};
    auto result = surface.synthesize(
        {std::move(request.text), std::move(*model_id), std::move(provenance)},
        environment.stop_token);
    if (!result) return std::unexpected(command_failure(result.error()));
    auto artifact = read_validated(*stores->artifacts, result->artifact,
                                   environment.stop_token);
    if (!artifact) return std::unexpected(std::move(artifact.error()));
    if (request.output_path) {
      auto exported =
          export_bytes(*artifact, *request.output_path, environment.stop_token);
      if (!exported) return std::unexpected(std::move(exported.error()));
    }
    output << "session=" << result->session_id.value()
           << " artifact=" << result->artifact.artifact_id.value()
           << " media=audio/wav bytes=" << result->artifact.byte_size
           << " digest=" << result->artifact.digest << '\n';
    if (!output)
      return failure(cli::CommandFailureKind::runtime, "audio output failed");
    return {};
  } catch (...) {
    return failure(cli::CommandFailureKind::runtime,
                   "speech synthesis command failed internally");
  }
}

auto ProcessAudioCommand::transcribe(TranscribeRequest request,
                                     cli::CommandEnvironment& environment,
                                     std::ostream& output, std::ostream& error)
    -> std::expected<void, cli::CommandFailure> {
  try {
    if (!valid_language(request.language))
      return failure(cli::CommandFailureKind::usage, "language is invalid");
    auto input = read_input(request.input_path, environment.stop_token);
    if (!input) return std::unexpected(std::move(input.error()));
    if (!audio::validate_pcm_wav(*input,
                                 {.maximum_bytes = maximum_audio_bytes})) {
      return failure(cli::CommandFailureKind::usage,
                     "audio input is not valid bounded PCM WAV");
    }
    auto resolved = load_process_config(error, request.model);
    if (!resolved) return std::unexpected(std::move(resolved.error()));
    auto catalog = ProcessModelCatalog::create();
    if (!catalog)
      return failure(cli::CommandFailureKind::runtime, catalog.error().message);
    auto model_id = select_model((*catalog)->service(), request.model, "asr",
                                 environment.stop_token);
    if (!model_id) return std::unexpected(std::move(model_id.error()));
    auto credential = resolve_process_credential(error);
    if (!credential) return std::unexpected(std::move(credential.error()));
    if (!credential->credential)
      return failure(cli::CommandFailureKind::runtime,
                     "Venice credential is not configured; run 'aiforge login' "
                     "or set VENICE_API_KEY");
    auto stores = open_stores();
    if (!stores) return std::unexpected(std::move(stores.error()));
    auto stored =
        stores->artifacts->put({next_input_id(), "audio/wav", std::nullopt},
                               *input, environment.stop_token);
    if (!stored)
      return failure(stored.error().code ==
                             storage::ArtifactStoreErrorCode::cancelled
                         ? cli::CommandFailureKind::cancelled
                         : cli::CommandFailureKind::runtime,
                     "transcription input could not be stored");
    auto resolved_credential = std::move(*credential->credential);
    const auto credential_source = resolved_credential.source;
    VeniceAudioService service{std::move(resolved_credential.secret)};
    TranscriptionBackend backend{service, *stores->artifacts, *stored,
                                 std::move(request.language)};
    auto provenance = process_run_provenance(*resolved, *model_id, "venice",
                                             credential_source);
    surfaces::TranscriptionSurface surface{backend, *stores->sessions};
    auto result = surface.transcribe(
        {*stored, std::move(*model_id), std::move(provenance)},
        environment.stop_token);
    if (!result) return std::unexpected(command_failure(result.error()));
    output << result->text;
    if (result->text.empty() || result->text.back() != '\n') output << '\n';
    if (!output)
      return failure(cli::CommandFailureKind::runtime,
                     "transcription output failed");
    if (!warning(error,
                 "session=" + std::string{result->session_id.value()} +
                     " artifact=" +
                     std::string{result->input_artifact.artifact_id.value()})) {
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
    }
    return {};
  } catch (...) {
    return failure(cli::CommandFailureKind::runtime,
                   "audio transcription command failed internally");
  }
}

auto ProcessAudioCommand::export_artifact(ExportRequest request,
                                          cli::CommandEnvironment& environment,
                                          std::ostream& output, std::ostream&)
    -> std::expected<void, cli::CommandFailure> {
  try {
    auto stores = open_stores(FilesystemArtifactStoreOpenMode::existing);
    if (!stores) return std::unexpected(std::move(stores.error()));
    auto events = stores->sessions->replay_events(request.session_id,
                                                  environment.stop_token);
    if (!events)
      return failure(
          events.error().code == storage::SessionStoreErrorCode::cancelled
              ? cli::CommandFailureKind::cancelled
              : cli::CommandFailureKind::runtime,
          events.error().code == storage::SessionStoreErrorCode::not_found
              ? "audio session was not found"
              : "audio session could not be replayed");
    auto artifacts = audio_artifacts(*events);
    auto selected = artifacts.end();
    if (request.artifact_id) {
      selected = std::ranges::find(artifacts, *request.artifact_id,
                                   &domain::ArtifactMetadata::artifact_id);
    } else if (!artifacts.empty()) {
      selected = std::prev(artifacts.end());
    }
    if (selected == artifacts.end())
      return failure(cli::CommandFailureKind::runtime,
                     request.artifact_id
                         ? "audio artifact is not present in that session"
                         : "session has no PCM WAV artifacts");
    auto artifact =
        read_validated(*stores->artifacts, *selected, environment.stop_token);
    if (!artifact) return std::unexpected(std::move(artifact.error()));
    auto exported =
        export_bytes(*artifact, request.output_path, environment.stop_token);
    if (!exported) return std::unexpected(std::move(exported.error()));
    output << "session=" << request.session_id.value()
           << " artifact=" << selected->artifact_id.value()
           << " output=" << request.output_path << '\n';
    if (!output)
      return failure(cli::CommandFailureKind::runtime,
                     "audio export output failed");
    return {};
  } catch (...) {
    return failure(cli::CommandFailureKind::runtime,
                   "audio export command failed internally");
  }
}

} // namespace aiforge::adapters
