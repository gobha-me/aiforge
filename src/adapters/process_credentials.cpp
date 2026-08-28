#include <aiforge/adapters/process_credentials.hpp>

#include <cstdlib>
#include <ostream>
#include <string>
#include <utility>

namespace aiforge::adapters {
namespace {

[[nodiscard]] auto failure(std::string message) -> cli::CommandFailure {
  return {cli::CommandFailureKind::runtime, std::move(message)};
}

} // namespace

auto resolve_process_credential(std::ostream& diagnostics)
    -> std::expected<credentials::CredentialResolution, cli::CommandFailure> {
  try {
    if (const auto* value = std::getenv("VENICE_API_KEY")) {
      auto secret = credentials::make_secret(std::string{value});
      if (!secret) {
        return std::unexpected(failure("VENICE_API_KEY is invalid"));
      }
      credentials::CredentialResolution resolution;
      resolution.credential.emplace(credentials::ResolvedCredential{
          std::move(*secret),
          {domain::CredentialSourceKind::environment, "VENICE_API_KEY"}});
      return resolution;
    }

    auto path = credentials::process_credential_path();
    if (!path) {
      diagnostics << "aiforge: warning: " << path.error().message << '\n';
      return credentials::CredentialResolution{};
    }
    credentials::FileCredentialStore store{std::move(*path)};
    auto resolution = credentials::resolve_credential(std::nullopt, store);
    if (!resolution) {
      return std::unexpected(failure(resolution.error().message));
    }
    for (const auto& warning : resolution->warnings) {
      diagnostics << "aiforge: warning: " << warning << '\n';
    }
    return std::move(*resolution);
  } catch (...) {
    return std::unexpected(failure("credential resolution failed internally"));
  }
}

} // namespace aiforge::adapters
