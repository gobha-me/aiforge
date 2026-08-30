#include <aiforge/adapters/process_image.hpp>

#include <algorithm>
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

#include <aiforge/adapters/filesystem_artifact_store.hpp>
#include <aiforge/adapters/image_backend.hpp>
#include <aiforge/adapters/process_credentials.hpp>
#include <aiforge/adapters/process_model_catalog.hpp>
#include <aiforge/adapters/process_provenance.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/adapters/termforge_image_renderer.hpp>
#include <aiforge/adapters/venice_image_generator.hpp>
#include <aiforge/config/config.hpp>
#include <aiforge/config/file_store.hpp>
#include <aiforge/model/catalog.hpp>
#include <aiforge/surfaces/image.hpp>
#include <termforge/core/input.hpp>
#include <termforge/core/terminal.hpp>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aiforge::adapters {
namespace {

constexpr std::size_t maximum_artifact_bytes = 32U * 1024U * 1024U;

[[nodiscard]] auto failure(const cli::CommandFailureKind kind,
                           std::string message)
    -> std::unexpected<cli::CommandFailure> {
  return std::unexpected(cli::CommandFailure{kind, std::move(message)});
}

auto warning(std::ostream& error, const std::string_view message) -> bool {
  try {
    error << "aiforge: warning: " << message << '\n';
    return static_cast<bool>(error);
  } catch (...) {
    return false;
  }
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
  if (!environment) {
    return failure(cli::CommandFailureKind::runtime,
                   "configuration environment could not be read");
  }
  layers.push_back(std::move(*environment));
  auto path = config::process_config_path();
  if (!path) {
    if (!warning(error, path.error().message)) {
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
    }
  } else {
    auto file = config::JsonConfigFileStore{*path}.load(registry);
    if (file) {
      layers.push_back(std::move(*file));
    } else if (!warning(error, file.error().message)) {
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
    }
  }
  auto resolved = config::resolve_config(registry, layers);
  if (!resolved) {
    return failure(cli::CommandFailureKind::runtime,
                   "configuration could not be resolved");
  }
  for (const auto& diagnostic : resolved->diagnostics) {
    if (!warning(error, diagnostic.message)) {
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
    }
  }
  return std::move(*resolved);
}

[[nodiscard]] auto requested_media_type(
    const std::optional<std::string>& format) -> std::optional<std::string> {
  if (!format || *format == "auto") return std::nullopt;
  if (*format == "png") return "image/png";
  if (*format == "jpeg") return "image/jpeg";
  if (*format == "webp") return "image/webp";
  return std::nullopt;
}

[[nodiscard]] auto select_image_model(model::CatalogService& catalog,
                                      const std::string& requested,
                                      const std::stop_token stop_token)
    -> std::expected<domain::ModelId, cli::CommandFailure> {
  auto model_id = domain::ModelId::from(requested);
  if (!model_id) {
    return failure(cli::CommandFailureKind::usage, "model ID is invalid");
  }
  auto snapshot = catalog.snapshot(stop_token);
  if (!snapshot) {
    return failure(snapshot.error().code == model::CatalogErrorCode::cancelled
                       ? cli::CommandFailureKind::cancelled
                       : cli::CommandFailureKind::runtime,
                   snapshot.error().message);
  }
  if (model::find_model(snapshot->get(), *model_id, "image") != nullptr) {
    return std::move(*model_id);
  }
  auto suggestions =
      model::suggest_models(snapshot->get(), requested, 3, "image");
  std::string message = "unknown image model '" + requested + "'";
  if (!suggestions.empty()) {
    message += "; did you mean ";
    for (std::size_t index{}; index < suggestions.size(); ++index) {
      if (index != 0) {
        message += index + 1 == suggestions.size() ? " or " : ", ";
      }
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

[[nodiscard]] auto open_stores(
    const FilesystemArtifactStoreOpenMode artifact_mode =
        FilesystemArtifactStoreOpenMode::create)
    -> std::expected<Stores, cli::CommandFailure> {
  auto path = process_session_store_path();
  if (!path) {
    return failure(cli::CommandFailureKind::runtime,
                   "session storage path could not be resolved");
  }
  auto sessions = artifact_mode == FilesystemArtifactStoreOpenMode::create
                      ? SqliteSessionStore::open(*path)
                      : SqliteSessionStore::open_existing_read_only(*path);
  if (!sessions) {
    return failure(cli::CommandFailureKind::runtime,
                   "session storage could not be opened");
  }
  auto artifacts =
      FilesystemArtifactStore::open(path->parent_path() / "artifacts",
                                    {maximum_artifact_bytes}, artifact_mode);
  if (!artifacts) {
    return failure(cli::CommandFailureKind::runtime,
                   "artifact storage could not be opened");
  }
  return Stores{std::move(*sessions), std::move(*artifacts)};
}

[[nodiscard]] auto image_artifacts(
    const std::span<const domain::RunEvent> events)
    -> std::vector<domain::ArtifactMetadata> {
  std::vector<domain::ArtifactMetadata> result;
  for (const auto& event : events) {
    const auto* created = std::get_if<domain::ArtifactCreated>(&event.payload);
    if (created != nullptr && created->artifact.producing_inference_id &&
        (created->artifact.media_type == "image/png" ||
         created->artifact.media_type == "image/jpeg" ||
         created->artifact.media_type == "image/webp")) {
      result.push_back(created->artifact);
    }
  }
  return result;
}

#ifndef _WIN32
class Descriptor final {
 public:
  explicit Descriptor(const int descriptor) : m_descriptor(descriptor) {}
  ~Descriptor() {
    if (m_descriptor >= 0) static_cast<void>(::close(m_descriptor));
  }
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  [[nodiscard]] auto get() const noexcept -> int { return m_descriptor; }

 private:
  int m_descriptor;
};
#endif

[[nodiscard]] auto export_artifact(const storage::ArtifactRead& artifact,
                                   const std::filesystem::path& path,
                                   const std::stop_token stop_token)
    -> std::expected<void, cli::CommandFailure> {
  if (path.empty() || path.filename().empty()) {
    return failure(cli::CommandFailureKind::usage,
                   "output path must name a file");
  }
#ifdef _WIN32
  static_cast<void>(artifact);
  static_cast<void>(stop_token);
  return failure(cli::CommandFailureKind::runtime,
                 "artifact export is unavailable on Windows");
#else
  Descriptor descriptor{
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
             S_IRUSR | S_IWUSR)};
  if (descriptor.get() < 0) {
    return failure(errno == EEXIST ? cli::CommandFailureKind::usage
                                   : cli::CommandFailureKind::runtime,
                   errno == EEXIST ? "output path already exists"
                                   : "output file could not be created");
  }
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

[[nodiscard]] auto render_terminal_viewer(const storage::ArtifactRead& artifact,
                                          cli::CommandEnvironment& environment)
    -> std::expected<void, std::string> {
  if (!environment.input_is_terminal || !environment.output_is_terminal ||
      environment.input_descriptor < 0 || environment.output_descriptor < 0) {
    return {};
  }
  try {
    termforge::Terminal terminal;
    if (auto configured = terminal.set_io(
            {environment.input_descriptor, environment.output_descriptor});
        !configured) {
      return std::unexpected("terminal image I/O could not be configured");
    }
    if (auto raw = terminal.enter_raw(); !raw) {
      return std::unexpected("terminal image input could not be initialized");
    }
    auto capabilities = terminal.query_capabilities();
    if (!capabilities) {
      return std::unexpected("terminal image capabilities could not be read");
    }
    auto driver = terminal.select_driver(*capabilities);
    if (auto initialized = driver->init(); !initialized) {
      return std::unexpected("terminal image driver could not be initialized");
    }

    int columns = 80;
    int rows = 24;
#ifndef _WIN32
    struct winsize size{};
    if (::ioctl(environment.output_descriptor, TIOCGWINSZ, &size) == 0) {
      if (size.ws_col > 0) columns = size.ws_col;
      if (size.ws_row > 0) rows = size.ws_row;
    }
#endif
    columns = std::clamp(columns, 1, 120);
    rows = std::clamp(rows - 2, 1, 40);
    if (artifact.metadata.width && artifact.metadata.height) {
      const auto proportional =
          static_cast<std::uint64_t>(*artifact.metadata.height) *
          static_cast<std::uint64_t>(columns) /
          (static_cast<std::uint64_t>(*artifact.metadata.width) * 2U);
      rows = std::clamp(static_cast<int>(std::min<std::uint64_t>(
                            proportional, static_cast<std::uint64_t>(rows))),
                        1, rows);
    }

    terminal.enter_screen();
    struct ScreenGuard final {
      termforge::TerminalDriver& driver;
      termforge::Terminal& terminal;
      ~ScreenGuard() {
        driver.shutdown();
        terminal.leave_screen();
      }
    } guard{*driver, terminal};
    driver->draw_text(0, 0, "Generated image - press any key to return",
                      {0xE0, 0xE0, 0xF0}, {0x0A, 0x0A, 0x14},
                      termforge::Attr::Bold);
    auto rendered = render_image_artifact(artifact, *driver,
                                          termforge::Rect{0, 1, columns, rows});
    if (!rendered) return std::unexpected(rendered.error().message);

    termforge::Input input;
    terminal.set_read_timeout(1);
    while (!environment.stop_token.stop_requested()) {
      char buffer[256];
      for (;;) {
        const auto count = terminal.read_input(buffer, sizeof(buffer));
        if (count <= 0) break;
        input.feed(std::string_view{buffer, static_cast<std::size_t>(count)});
      }
      input.flush();
      for (auto& record : input.poll_replies()) {
        if (auto* reply = std::get_if<termforge::TerminalReply>(&record)) {
          driver->consume_reply(*reply);
        }
      }
      for (const auto& event : input.poll()) {
        if (std::holds_alternative<termforge::KeyEvent>(event)) return {};
      }
    }
    return std::unexpected("terminal image display was cancelled");
  } catch (...) {
    return std::unexpected("terminal image display failed internally");
  }
}

[[nodiscard]] auto present_artifact(
    FilesystemArtifactStore& store, const domain::SessionId& session_id,
    const domain::ArtifactMetadata& metadata,
    const std::optional<std::string>& output_path,
    cli::CommandEnvironment& environment, std::ostream& output,
    std::ostream& error) -> std::expected<void, cli::CommandFailure> {
  auto artifact =
      store.get(metadata, maximum_artifact_bytes, environment.stop_token);
  if (!artifact) {
    return failure(artifact.error().code ==
                           storage::ArtifactStoreErrorCode::cancelled
                       ? cli::CommandFailureKind::cancelled
                       : cli::CommandFailureKind::runtime,
                   artifact.error().message);
  }
  auto validated = validate_image_artifact(*artifact);
  if (!validated) {
    return failure(cli::CommandFailureKind::runtime, validated.error().message);
  }
  if (output_path) {
    auto exported =
        export_artifact(*artifact, *output_path, environment.stop_token);
    if (!exported) return std::unexpected(std::move(exported.error()));
  }
  auto rendered = render_terminal_viewer(*artifact, environment);
  if (!rendered && environment.stop_token.stop_requested()) {
    return failure(cli::CommandFailureKind::cancelled,
                   "terminal image display was cancelled");
  }
  if (!rendered && !warning(error, rendered.error())) {
    return failure(cli::CommandFailureKind::runtime,
                   "diagnostic output failed");
  }
  try {
    output << "session=" << session_id.value()
           << " artifact=" << metadata.artifact_id.value()
           << " media=" << metadata.media_type;
    if (metadata.width && metadata.height) {
      output << " dimensions=" << *metadata.width << 'x' << *metadata.height;
    }
    output << " bytes=" << metadata.byte_size << " digest=" << metadata.digest;
    output << '\n';
    if (!output) {
      return failure(cli::CommandFailureKind::runtime, "image output failed");
    }
  } catch (...) {
    return failure(cli::CommandFailureKind::runtime, "image output failed");
  }
  return {};
}

[[nodiscard]] auto command_failure(const surfaces::ImageError& error)
    -> cli::CommandFailure {
  switch (error.code) {
    case surfaces::ImageErrorCode::invalid_input:
      return {cli::CommandFailureKind::usage, error.message};
    case surfaces::ImageErrorCode::cancelled:
      return {cli::CommandFailureKind::cancelled, error.message};
    case surfaces::ImageErrorCode::context_failed:
    case surfaces::ImageErrorCode::run_failed:
    case surfaces::ImageErrorCode::internal_failure:
      return {cli::CommandFailureKind::runtime, error.message};
  }
  return {cli::CommandFailureKind::runtime, "image command failed internally"};
}

} // namespace

auto ProcessImageCommand::generate(GenerateRequest request,
                                   cli::CommandEnvironment& environment,
                                   std::ostream& output, std::ostream& error)
    -> std::expected<void, cli::CommandFailure> {
  try {
    auto resolved = load_process_config(error, request.model);
    if (!resolved) return std::unexpected(std::move(resolved.error()));
    auto catalog = ProcessModelCatalog::create();
    if (!catalog) {
      return failure(cli::CommandFailureKind::runtime, catalog.error().message);
    }
    auto model_id = select_image_model((*catalog)->service(), request.model,
                                       environment.stop_token);
    if (!model_id) return std::unexpected(std::move(model_id.error()));
    auto credential = resolve_process_credential(error);
    if (!credential) return std::unexpected(std::move(credential.error()));
    if (!credential->credential) {
      return failure(cli::CommandFailureKind::runtime,
                     "Venice credential is not configured; run 'aiforge login' "
                     "or set VENICE_API_KEY");
    }
    auto stores = open_stores();
    if (!stores) return std::unexpected(std::move(stores.error()));
    auto resolved_credential = std::move(*credential->credential);
    const auto credential_source = resolved_credential.source;
    VeniceImageGenerator generator{std::move(resolved_credential.secret)};
    ImageBackend backend{generator,
                         *stores->artifacts,
                         {requested_media_type(request.format), 1024U * 1024U,
                          maximum_artifact_bytes}};
    auto provenance = process_run_provenance(*resolved, *model_id, "venice",
                                             credential_source);
    surfaces::ImageSurface surface{backend, *stores->sessions};
    auto generated =
        surface.generate({std::move(request.prompt), std::move(*model_id),
                          std::move(provenance)},
                         environment.stop_token);
    if (!generated) {
      return std::unexpected(command_failure(generated.error()));
    }
    return present_artifact(*stores->artifacts, generated->session_id,
                            generated->artifact, request.output_path,
                            environment, output, error);
  } catch (...) {
    return failure(cli::CommandFailureKind::runtime,
                   "image generation command failed internally");
  }
}

auto ProcessImageCommand::show(ShowRequest request,
                               cli::CommandEnvironment& environment,
                               std::ostream& output, std::ostream& error)
    -> std::expected<void, cli::CommandFailure> {
  try {
    auto stores = open_stores(FilesystemArtifactStoreOpenMode::existing);
    if (!stores) return std::unexpected(std::move(stores.error()));
    auto events = stores->sessions->replay_events(request.session_id,
                                                  environment.stop_token);
    if (!events) {
      return failure(
          events.error().code == storage::SessionStoreErrorCode::cancelled
              ? cli::CommandFailureKind::cancelled
              : cli::CommandFailureKind::runtime,
          events.error().code == storage::SessionStoreErrorCode::not_found
              ? "image session was not found"
              : "image session could not be replayed");
    }
    auto artifacts = image_artifacts(*events);
    auto selected = artifacts.end();
    if (request.artifact_id) {
      selected = std::ranges::find(artifacts, *request.artifact_id,
                                   &domain::ArtifactMetadata::artifact_id);
    } else if (!artifacts.empty()) {
      selected = std::prev(artifacts.end());
    }
    if (selected == artifacts.end()) {
      return failure(cli::CommandFailureKind::runtime,
                     request.artifact_id
                         ? "image artifact is not present in that session"
                         : "session has no generated image artifacts");
    }
    return present_artifact(*stores->artifacts, request.session_id, *selected,
                            request.output_path, environment, output, error);
  } catch (...) {
    return failure(cli::CommandFailureKind::runtime,
                   "image show command failed internally");
  }
}

} // namespace aiforge::adapters
