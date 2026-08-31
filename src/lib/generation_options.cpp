#include <aiforge/backend/backend.hpp>

#include <algorithm>
#include <set>
#include <string>

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
        return std::unexpected(rejection(
            "generation options contain invalid model capabilities"));
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

} // namespace aiforge::backend
