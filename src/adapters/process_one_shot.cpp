#include <aiforge/adapters/process_one_shot.hpp>

#include <array>
#include <cstdlib>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/adapters/process_provenance.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/adapters/venice_backend.hpp>
#include <aiforge/config/config.hpp>
#include <aiforge/config/file_store.hpp>
#include <aiforge/surfaces/one_shot.hpp>

namespace aiforge::adapters {
namespace {

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

[[nodiscard]] auto load_process_config(std::ostream& error)
    -> std::expected<config::ResolvedConfig, cli::CommandFailure> {
  const auto& registry = config::builtin_config_registry();
  std::vector<config::ConfigLayer> layers;
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

[[nodiscard]] auto configured_model(const config::ResolvedConfig& resolved)
    -> std::expected<domain::ModelId, cli::CommandFailure> {
  const auto* entry = resolved.find("model");
  if (entry == nullptr || !entry->value) {
    return failure(cli::CommandFailureKind::runtime,
                   "model is not configured; set AIFORGE_MODEL or config model");
  }
  const auto* text = std::get_if<std::string>(&*entry->value);
  if (text == nullptr || text->empty()) {
    return failure(cli::CommandFailureKind::runtime,
                   "configured model is invalid");
  }
  auto model = domain::ModelId::from(*text);
  if (!model) {
    return failure(cli::CommandFailureKind::runtime,
                   "configured model is invalid");
  }
  return std::move(*model);
}

[[nodiscard]] auto read_stdin(cli::CommandEnvironment& environment,
                              const std::size_t maximum_bytes)
    -> std::expected<std::optional<std::string>, cli::CommandFailure> {
  if (environment.input_is_terminal) return std::nullopt;
  std::string value;
  std::array<char, 4096> buffer{};
  for (;;) {
    if (environment.stop_token.stop_requested()) {
      return failure(cli::CommandFailureKind::cancelled, "request cancelled");
    }
    environment.input.read(buffer.data(),
                           static_cast<std::streamsize>(buffer.size()));
    const auto count = environment.input.gcount();
    if (count > 0) {
      const auto size = static_cast<std::size_t>(count);
      if (size > maximum_bytes - std::min(maximum_bytes, value.size())) {
        return failure(cli::CommandFailureKind::usage,
                       "standard input exceeds 1 MiB");
      }
      value.append(buffer.data(), size);
    }
    if (environment.input.eof()) break;
    if (!environment.input) {
      return failure(cli::CommandFailureKind::runtime,
                     "standard input could not be read");
    }
  }
  if (value.empty()) return std::nullopt;
  return std::optional<std::string>{std::move(value)};
}

[[nodiscard]] auto command_failure(const surfaces::OneShotError& error)
    -> cli::CommandFailure {
  switch (error.code) {
    case surfaces::OneShotErrorCode::invalid_input:
    case surfaces::OneShotErrorCode::input_too_large:
      return {cli::CommandFailureKind::usage, error.message};
    case surfaces::OneShotErrorCode::cancelled:
      return {cli::CommandFailureKind::cancelled, error.message};
    case surfaces::OneShotErrorCode::model_lookup_failed:
    case surfaces::OneShotErrorCode::context_failed:
    case surfaces::OneShotErrorCode::run_failed:
    case surfaces::OneShotErrorCode::output_failed:
    case surfaces::OneShotErrorCode::internal_failure:
      return {cli::CommandFailureKind::runtime, error.message};
  }
  return {cli::CommandFailureKind::runtime,
          "one-shot execution failed internally"};
}

}  // namespace

auto ProcessOneShotCommand::execute(cli::OneShotCommand::Request request,
                                    cli::CommandEnvironment& environment,
                                    std::ostream& output,
                                    std::ostream& error)
    -> std::expected<void, cli::CommandFailure> {
  try {
    if (m_maximum_input_bytes == 0) {
      return failure(cli::CommandFailureKind::runtime,
                     "one-shot input limit is invalid");
    }
    auto input = read_stdin(environment, m_maximum_input_bytes);
    if (!input) return std::unexpected(std::move(input.error()));
    if (request.prompt.size() > m_maximum_input_bytes ||
        (input->has_value() &&
         (*input)->size() > m_maximum_input_bytes - request.prompt.size())) {
      return failure(cli::CommandFailureKind::usage,
                     "one-shot input exceeds 1 MiB");
    }

    auto resolved = load_process_config(error);
    if (!resolved) return std::unexpected(std::move(resolved.error()));
    auto model = configured_model(*resolved);
    if (!model) return std::unexpected(std::move(model.error()));

    const char* raw_key = std::getenv("VENICE_API_KEY");
    if (raw_key == nullptr || *raw_key == '\0') {
      return failure(cli::CommandFailureKind::runtime,
                     "VENICE_API_KEY is not configured");
    }
    const std::string api_key{raw_key};
    if (api_key.size() > 64U * 1024U) {
      return failure(cli::CommandFailureKind::runtime,
                     "VENICE_API_KEY is invalid");
    }

    VeniceBackendOptions backend_options;
    backend_options.api_key = api_key;
    VeniceBackend backend{std::move(backend_options)};
    const auto session_mode = [&] {
      switch (request.session_mode) {
        case cli::OneShotCommand::SessionMode::create:
          return surfaces::OneShotRequest::SessionMode::create;
        case cli::OneShotCommand::SessionMode::resume:
          return surfaces::OneShotRequest::SessionMode::resume;
        case cli::OneShotCommand::SessionMode::continue_latest:
          return surfaces::OneShotRequest::SessionMode::continue_latest;
        case cli::OneShotCommand::SessionMode::ephemeral:
          return surfaces::OneShotRequest::SessionMode::ephemeral;
      }
      return surfaces::OneShotRequest::SessionMode::create;
    }();
    // The variable name only. The key itself never leaves `api_key`.
    auto credential_source = domain::CredentialSourceReference{
        domain::CredentialSourceKind::environment, "VENICE_API_KEY"};
    auto provenance = process_run_provenance(*resolved, *model, "venice",
                                             std::move(credential_source));
    surfaces::OneShotRequest one_shot_request{
        std::string{request.prompt}, std::move(*input), std::move(*model),
        session_mode, std::move(request.session_id), std::move(provenance)};

    std::expected<surfaces::OneShotResult, surfaces::OneShotError> result =
        std::unexpected(surfaces::OneShotError{
            surfaces::OneShotErrorCode::internal_failure,
            "one-shot session setup failed"});
    if (session_mode == surfaces::OneShotRequest::SessionMode::ephemeral) {
      surfaces::OneShotSurface surface{
          backend, backend, {m_maximum_input_bytes, 4096}};
      result = surface.run(std::move(one_shot_request), output, error,
                           environment.stop_token);
    } else {
      auto path = process_session_store_path();
      if (!path) {
        return failure(cli::CommandFailureKind::runtime,
                       "session storage path could not be resolved");
      }
      auto store = SqliteSessionStore::open(*path);
      if (!store) {
        return failure(cli::CommandFailureKind::runtime,
                       "session storage could not be opened");
      }
      surfaces::OneShotSurface surface{
          backend, backend, **store, {m_maximum_input_bytes, 4096}};
      result = surface.run(std::move(one_shot_request), output, error,
                           environment.stop_token);
    }
    if (!result) return std::unexpected(command_failure(result.error()));
    return {};
  } catch (...) {
    return failure(cli::CommandFailureKind::runtime,
                   "one-shot command failed internally");
  }
}

}  // namespace aiforge::adapters
