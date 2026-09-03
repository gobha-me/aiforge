#include <aiforge/backend/provider_character_catalog.hpp>

#include <set>
#include <string_view>
#include <utility>

#include <aiforge/detail/utf8_text.hpp>

namespace aiforge::backend {
namespace {

[[nodiscard]] auto failure(const ProviderCharacterErrorCode code,
                           std::string message)
    -> std::unexpected<ProviderCharacterError> {
  return std::unexpected(
      ProviderCharacterError{code, std::move(message), false, std::nullopt});
}

[[nodiscard]] auto validate_limits(const ProviderCharacterLimits& limits)
    -> std::expected<void, ProviderCharacterError> {
  if (limits.maximum_entries == 0 || limits.maximum_text_bytes == 0 ||
      limits.maximum_tags_per_entry == 0 ||
      limits.maximum_total_text_bytes == 0 ||
      limits.maximum_response_bytes == 0) {
    return failure(ProviderCharacterErrorCode::invalid_request,
                   "provider character catalog limits are invalid");
  }
  return {};
}

[[nodiscard]] auto account_text(const std::string_view value,
                                const ProviderCharacterLimits& limits,
                                std::size_t& total)
    -> std::expected<void, ProviderCharacterError> {
  if (value.empty()) {
    return failure(ProviderCharacterErrorCode::invalid_data,
                   "provider character catalog contains empty text");
  }
  if (value.size() > limits.maximum_text_bytes) {
    return failure(ProviderCharacterErrorCode::too_large,
                   "provider character catalog text exceeds its byte limit");
  }
  if (!detail::is_safe_utf8_text(value)) {
    return failure(ProviderCharacterErrorCode::invalid_data,
                   "provider character catalog contains unsafe text");
  }
  if (value.size() > limits.maximum_total_text_bytes - total) {
    return failure(
        ProviderCharacterErrorCode::too_large,
        "provider character catalog exceeds its cumulative text byte limit");
  }
  total += value.size();
  return {};
}

[[nodiscard]] auto validate_summary(const ProviderCharacterSummary& summary,
                                    const ProviderCharacterLimits& limits,
                                    std::size_t& total)
    -> std::expected<void, ProviderCharacterError> {
  if (auto checked = account_text(summary.id.value(), limits, total); !checked)
    return checked;
  if (summary.name) {
    if (auto checked = account_text(*summary.name, limits, total); !checked)
      return checked;
  }
  if (summary.description) {
    if (auto checked = account_text(*summary.description, limits, total);
        !checked)
      return checked;
  }
  if (summary.model_id) {
    if (auto checked = account_text(summary.model_id->value(), limits, total);
        !checked)
      return checked;
  }
  if (summary.tags.size() > limits.maximum_tags_per_entry) {
    return failure(ProviderCharacterErrorCode::too_large,
                   "provider character catalog has too many tags");
  }
  std::set<std::string_view> tags;
  for (const auto& tag : summary.tags) {
    if (!tags.insert(tag).second) {
      return failure(ProviderCharacterErrorCode::invalid_data,
                     "provider character catalog repeats a tag");
    }
    if (auto checked = account_text(tag, limits, total); !checked)
      return checked;
  }
  return {};
}

} // namespace

auto validate_provider_character_catalog(
    const ProviderCharacterCatalog& catalog,
    const ProviderCharacterLimits limits)
    -> std::expected<void, ProviderCharacterError> {
  try {
    if (auto checked = validate_limits(limits); !checked) return checked;
    if (catalog.entries.size() > limits.maximum_entries) {
      return failure(ProviderCharacterErrorCode::too_large,
                     "provider character catalog has too many entries");
    }

    std::size_t total{};
    if (auto checked = account_text(catalog.source_id, limits, total); !checked)
      return checked;
    std::set<domain::ProviderCharacterId> ids;
    for (const auto& summary : catalog.entries) {
      if (!ids.insert(summary.id).second) {
        return failure(ProviderCharacterErrorCode::invalid_data,
                       "provider character catalog repeats an identifier");
      }
      if (auto checked = validate_summary(summary, limits, total); !checked)
        return checked;
    }
    return {};
  } catch (...) {
    return failure(ProviderCharacterErrorCode::internal_failure,
                   "provider character catalog validation failed internally");
  }
}

auto validate_provider_character_summary(
    const ProviderCharacterSummary& summary,
    const ProviderCharacterLimits limits)
    -> std::expected<void, ProviderCharacterError> {
  try {
    if (auto checked = validate_limits(limits); !checked) return checked;
    std::size_t total{};
    return validate_summary(summary, limits, total);
  } catch (...) {
    return failure(ProviderCharacterErrorCode::internal_failure,
                   "provider character validation failed internally");
  }
}

} // namespace aiforge::backend
