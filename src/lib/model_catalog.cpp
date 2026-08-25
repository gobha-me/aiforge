#include <aiforge/model/catalog.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace aiforge::model {
namespace {

[[nodiscard]] auto catalog_error(const CatalogErrorCode code,
                                 std::string message,
                                 const bool retryable = false)
    -> std::unexpected<CatalogError> {
  return std::unexpected(CatalogError{code, std::move(message), retryable});
}

[[nodiscard]] auto valid_text(const std::string_view value,
                              const std::size_t maximum_bytes,
                              const bool allow_empty = false) -> bool {
  if ((!allow_empty && value.empty()) || value.size() > maximum_bytes) {
    return false;
  }
  std::size_t index{};
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first == 0 || first == 0x7fU || first < 0x20U) return false;
    std::size_t length{};
    std::uint32_t codepoint{};
    if (first <= 0x7fU) {
      length = 1;
      codepoint = first;
    } else if ((first & 0xe0U) == 0xc0U) {
      length = 2;
      codepoint = first & 0x1fU;
      if (codepoint < 2) return false;
    } else if ((first & 0xf0U) == 0xe0U) {
      length = 3;
      codepoint = first & 0x0fU;
    } else if ((first & 0xf8U) == 0xf0U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (length > value.size() - index) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xc0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3fU);
    }
    if ((length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
        codepoint > 0x10ffffU) {
      return false;
    }
    index += length;
  }
  return true;
}

[[nodiscard]] auto lower_ascii(const std::string_view value) -> std::string {
  std::string result;
  result.reserve(value.size());
  for (const auto character : value) {
    result.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(character))));
  }
  return result;
}

[[nodiscard]] auto edit_distance(const std::string_view left,
                                 const std::string_view right)
    -> std::size_t {
  std::vector<std::size_t> previous(right.size() + 1);
  std::vector<std::size_t> current(right.size() + 1);
  for (std::size_t index{}; index <= right.size(); ++index) previous[index] = index;
  for (std::size_t row = 1; row <= left.size(); ++row) {
    current[0] = row;
    for (std::size_t column = 1; column <= right.size(); ++column) {
      const auto substitution = previous[column - 1] +
                                (left[row - 1] == right[column - 1] ? 0U : 1U);
      current[column] = std::min(
          {previous[column] + 1, current[column - 1] + 1, substitution});
    }
    previous.swap(current);
  }
  return previous.back();
}

[[nodiscard]] auto backend_error(const CatalogError& error)
    -> backend::BackendError {
  const auto kind = [&] {
    switch (error.code) {
      case CatalogErrorCode::cancelled:
        return backend::BackendErrorKind::cancelled;
      case CatalogErrorCode::invalid_data:
        return backend::BackendErrorKind::protocol;
      case CatalogErrorCode::unavailable:
      case CatalogErrorCode::storage:
      case CatalogErrorCode::internal_failure:
        return backend::BackendErrorKind::unavailable;
    }
    return backend::BackendErrorKind::unavailable;
  }();
  return {kind, error.message, error.retryable, std::nullopt};
}

[[nodiscard]] auto valid_price(const std::optional<Price>& value) -> bool {
  const auto valid = [](const std::optional<double> amount) {
    return !amount || (std::isfinite(*amount) && *amount >= 0.0);
  };
  return !value || (valid(value->usd) && valid(value->diem));
}

[[nodiscard]] auto valid_tier(const PriceTier& tier) -> bool {
  return valid_price(tier.input) && valid_price(tier.output) &&
         valid_price(tier.cache_input) && valid_price(tier.cache_write);
}

}  // namespace

CatalogService::CatalogService(CatalogSource& source, CatalogCache* cache,
                               const std::chrono::hours time_to_live,
                               CatalogClock clock, CatalogLimits limits)
    : m_source(source),
      m_cache(cache),
      m_time_to_live(time_to_live),
      m_clock(std::move(clock)),
      m_limits(limits) {
  if (!m_clock) {
    m_clock = [] {
      return std::chrono::floor<std::chrono::milliseconds>(
          std::chrono::system_clock::now());
    };
  }
}

auto CatalogService::snapshot(const std::stop_token stop_token)
    -> std::expected<std::reference_wrapper<const CatalogSnapshot>,
                     CatalogError> {
  try {
    if (stop_token.stop_requested()) {
      return catalog_error(CatalogErrorCode::cancelled,
                           "model catalog request cancelled");
    }
    if (m_snapshot) return std::cref(*m_snapshot);
    const auto now = m_clock();
    std::optional<CatalogSnapshot> cached;
    std::vector<std::string> warnings;
    if (m_cache != nullptr) {
      auto loaded = m_cache->load(stop_token);
      if (!loaded) {
        if (loaded.error().code == CatalogErrorCode::cancelled) {
          return std::unexpected(std::move(loaded.error()));
        }
        warnings.push_back(loaded.error().message);
      } else if (*loaded) {
        cached = std::move(**loaded);
        if (auto valid = validate_catalog(*cached, m_limits); !valid) {
          warnings.push_back(valid.error().message);
          cached.reset();
        } else if (cached->fetched_at <= now &&
                   now - cached->fetched_at <= m_time_to_live) {
          cached->origin = CatalogOrigin::fresh_cache;
          cached->warnings.insert(cached->warnings.end(), warnings.begin(),
                                  warnings.end());
          m_snapshot = std::move(cached);
          return std::cref(*m_snapshot);
        } else if (cached->fetched_at > now) {
          warnings.push_back(
              "model catalog cache timestamp is in the future");
          cached.reset();
        }
      }
    }

    auto fetched = m_source.fetch(stop_token);
    if (fetched) {
      if (auto valid = validate_catalog(*fetched, m_limits); !valid) {
        return std::unexpected(std::move(valid.error()));
      }
      fetched->origin = CatalogOrigin::live;
      fetched->warnings.insert(fetched->warnings.end(), warnings.begin(),
                               warnings.end());
      if (m_cache != nullptr) {
        auto stored = m_cache->store(*fetched, stop_token);
        if (!stored) {
          if (stored.error().code == CatalogErrorCode::cancelled) {
            return std::unexpected(std::move(stored.error()));
          }
          fetched->warnings.push_back(stored.error().message);
        }
      }
      m_snapshot = std::move(*fetched);
      return std::cref(*m_snapshot);
    }
    if (fetched.error().code == CatalogErrorCode::cancelled ||
        stop_token.stop_requested()) {
      return catalog_error(CatalogErrorCode::cancelled,
                           "model catalog request cancelled");
    }
    if (cached) {
      cached->origin = CatalogOrigin::stale_cache;
      cached->warnings.insert(cached->warnings.end(), warnings.begin(),
                              warnings.end());
      cached->warnings.push_back(
          "live model catalog is unavailable; using stale cached data");
      m_snapshot = std::move(*cached);
      return std::cref(*m_snapshot);
    }
    return std::unexpected(std::move(fetched.error()));
  } catch (...) {
    return catalog_error(CatalogErrorCode::internal_failure,
                         "model catalog resolution failed internally");
  }
}

auto CatalogService::lookup(const domain::ModelId& model_id,
                            const std::stop_token stop_token)
    -> std::expected<backend::ModelContextInfo, backend::BackendError> {
  auto resolved = snapshot(stop_token);
  if (!resolved) return std::unexpected(backend_error(resolved.error()));
  const auto* entry = find_model(resolved->get(), model_id, "text");
  if (entry == nullptr) {
    return std::unexpected(backend::BackendError{
        backend::BackendErrorKind::request_rejected,
        "configured model was not found in the text model catalog", false,
        std::nullopt});
  }
  if (entry->offline) {
    return std::unexpected(backend::BackendError{
        backend::BackendErrorKind::unavailable,
        "configured model is unavailable", true, std::nullopt});
  }
  if (!entry->context_window_tokens || *entry->context_window_tokens == 0) {
    return std::unexpected(backend::BackendError{
        backend::BackendErrorKind::protocol,
        "configured model has no context capacity", false, std::nullopt});
  }
  return backend::ModelContextInfo{entry->id, *entry->context_window_tokens,
                                   entry->maximum_output_tokens};
}

auto CatalogService::clear_memory_cache() noexcept -> void { m_snapshot.reset(); }

auto validate_catalog(const CatalogSnapshot& snapshot, const CatalogLimits limits)
    -> std::expected<void, CatalogError> {
  if (limits.maximum_entries == 0 || limits.maximum_text_bytes == 0 ||
      limits.maximum_traits_per_entry == 0 ||
      snapshot.entries.size() > limits.maximum_entries) {
    return catalog_error(CatalogErrorCode::invalid_data,
                         "model catalog exceeds its resource limits");
  }
  std::vector<std::string_view> identities;
  identities.reserve(snapshot.entries.size());
  for (const auto& entry : snapshot.entries) {
    if (!valid_text(entry.id.value(), limits.maximum_text_bytes) ||
        !valid_text(entry.type, limits.maximum_text_bytes) ||
        (entry.name && !valid_text(*entry.name, limits.maximum_text_bytes)) ||
        entry.traits.size() > limits.maximum_traits_per_entry ||
        std::ranges::any_of(entry.traits, [&](const auto& trait) {
          return !valid_text(trait, limits.maximum_text_bytes);
        })) {
      return catalog_error(CatalogErrorCode::invalid_data,
                           "model catalog contains invalid text or metadata");
    }
    if (entry.context_window_tokens == 0 ||
        entry.maximum_output_tokens == 0 ||
        (entry.pricing &&
         (!valid_tier(entry.pricing->base) ||
          (entry.pricing->extended &&
           !valid_tier(*entry.pricing->extended)) ||
          !valid_price(entry.pricing->generation)))) {
      return catalog_error(CatalogErrorCode::invalid_data,
                           "model catalog contains invalid numeric metadata");
    }
    if (std::ranges::find(identities, entry.id.value()) != identities.end()) {
      return catalog_error(CatalogErrorCode::invalid_data,
                           "model catalog contains duplicate model IDs");
    }
    identities.push_back(entry.id.value());
  }
  return {};
}

auto find_model(const CatalogSnapshot& snapshot, const domain::ModelId& model_id,
                const std::string_view type) noexcept -> const CatalogEntry* {
  const auto found = std::ranges::find_if(snapshot.entries, [&](const auto& entry) {
    return entry.id == model_id && (type.empty() || entry.type == type);
  });
  return found == snapshot.entries.end() ? nullptr : &*found;
}

auto suggest_models(const CatalogSnapshot& snapshot,
                    const std::string_view requested, const std::size_t limit,
                    const std::string_view type) -> std::vector<std::string> {
  struct Candidate {
    std::size_t distance{};
    std::string id;
  };
  const auto needle = lower_ascii(requested);
  std::vector<Candidate> candidates;
  for (const auto& entry : snapshot.entries) {
    if ((!type.empty() && entry.type != type) || entry.offline) continue;
    auto lowered = lower_ascii(entry.id.value());
    auto distance = edit_distance(needle, lowered);
    if (lowered.contains(needle) || needle.contains(lowered)) distance /= 2;
    candidates.push_back({distance, std::string{entry.id.value()}});
  }
  std::ranges::sort(candidates, {}, [](const Candidate& value) {
    return std::pair{value.distance, value.id};
  });
  std::vector<std::string> result;
  result.reserve(std::min(limit, candidates.size()));
  for (std::size_t index{}; index < std::min(limit, candidates.size()); ++index) {
    result.push_back(std::move(candidates[index].id));
  }
  return result;
}

auto capability_name(const Capability capability) noexcept -> std::string_view {
  switch (capability) {
    case Capability::tool_calling: return "tools";
    case Capability::vision: return "vision";
    case Capability::multiple_images: return "multi-image";
    case Capability::video_input: return "video";
    case Capability::audio_input: return "audio";
    case Capability::reasoning: return "reasoning";
    case Capability::reasoning_effort: return "reasoning-effort";
    case Capability::response_schema: return "schema";
    case Capability::log_probabilities: return "logprobs";
    case Capability::web_search: return "web-search";
    case Capability::x_search: return "x-search";
    case Capability::tee_attestation: return "tee";
    case Capability::end_to_end_encryption: return "e2ee";
    case Capability::optimized_for_code: return "code";
  }
  return "unknown";
}

}  // namespace aiforge::model
