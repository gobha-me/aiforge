#include <aiforge/backend/backend.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>

namespace aiforge::backend {
namespace {

[[nodiscard]] auto valid_capability_name(const std::string& value) -> bool {
  constexpr std::size_t maximum_capability_bytes{128};
  return !value.empty() && value.size() <= maximum_capability_bytes &&
         std::ranges::all_of(value, [](const unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '-' ||
                  character == '.' || character == '_' || character == ':';
         });
}

[[nodiscard]] auto rejection(std::string message) -> BackendError {
  return {BackendErrorKind::request_rejected, std::move(message), false,
          std::nullopt};
}

} // namespace

auto validate_generation_requirements(const GenerationOptions& options,
                                      const ModelContextInfo& model)
    -> std::expected<void, BackendError> {
  try {
    constexpr std::size_t maximum_required_capabilities{64};
    if (options.required_model_capabilities.size() >
        maximum_required_capabilities) {
      return std::unexpected(
          rejection("generation options require too many model capabilities"));
    }

    std::set<std::string> seen;
    for (const auto& required : options.required_model_capabilities) {
      if (!valid_capability_name(required) || !seen.insert(required).second) {
        return std::unexpected(
            rejection("generation options contain invalid model capabilities"));
      }
      const auto found = model.capabilities.find(required);
      if (found == model.capabilities.end() || !found->second.value_or(false)) {
        return std::unexpected(
            rejection("selected model does not confirm required capability '" +
                      required + "'"));
      }
    }
    return {};
  } catch (...) {
    return std::unexpected(
        BackendError{BackendErrorKind::unavailable,
                     "generation capability validation failed internally", true,
                     std::nullopt});
  }
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicit exact coupling.
auto validate_effective_request_options(
    const GenerationOptions& options,
    const std::span<const domain::EffectiveRequestOption> effective_options)
    -> std::expected<void, BackendError> {
  // clang-format on
  try {
    std::size_t asserted{};
    for (const auto& option : effective_options) {
      switch (option.source) {
        case domain::RequestOptionSource::provider_default: break;
        case domain::RequestOptionSource::configuration:
        case domain::RequestOptionSource::session_override: ++asserted; break;
        default:
          return std::unexpected(rejection(
              "effective request option provenance has an invalid source"));
      }
    }
    if (asserted != options.extensions.size()) {
      return std::unexpected(rejection(
          "effective request option provenance does not match extensions"));
    }
    std::set<std::string_view> seen;
    for (const auto& effective : effective_options) {
      if (!seen.insert(effective.key).second) {
        return std::unexpected(rejection(
            "effective request option provenance contains duplicate keys"));
      }
      const auto extension = options.extensions.find(effective.key);
      if (effective.source == domain::RequestOptionSource::provider_default) {
        if (effective.value || extension != options.extensions.end()) {
          return std::unexpected(rejection(
              "provider-default request option provenance is inconsistent"));
        }
        continue;
      }
      if (!effective.value || extension == options.extensions.end() ||
          extension->second.media_type != "application/json") {
        return std::unexpected(
            rejection("effective request option provenance is inconsistent"));
      }
      const auto& encoded = extension->second.data;
      const auto& value = *effective.value;
      const bool exact_primitive = encoded == value;
      const bool exact_simple_string =
          encoded.size() == value.size() + 2U && encoded.front() == '"' &&
          encoded.back() == '"' &&
          std::string_view{encoded}.substr(1, value.size()) == value;
      if (!exact_primitive && !exact_simple_string) {
        return std::unexpected(rejection(
            "effective request option value does not match its extension"));
      }
    }
    for (const auto& [key, extension] : options.extensions) {
      static_cast<void>(extension);
      if (!seen.contains(key)) {
        return std::unexpected(rejection(
            "generation extension has no effective request provenance"));
      }
    }
    return {};
  } catch (...) {
    return std::unexpected(
        BackendError{BackendErrorKind::unavailable,
                     "request option provenance validation failed internally",
                     true, std::nullopt});
  }
}

} // namespace aiforge::backend
