#include <aiforge/adapters/json_model_catalog_cache.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aiforge::adapters {
namespace {

using Json = nlohmann::json;
constexpr int cache_schema_version = 2;

class DuplicateJsonKey final : public std::exception {};

[[nodiscard]] auto failure(std::string message)
    -> std::unexpected<model::CatalogError> {
  return std::unexpected(model::CatalogError{
      model::CatalogErrorCode::storage, std::move(message), false});
}

[[nodiscard]] auto cancelled()
    -> std::unexpected<model::CatalogError> {
  return std::unexpected(model::CatalogError{
      model::CatalogErrorCode::cancelled,
      "model catalog cache operation cancelled", false});
}

[[nodiscard]] auto capability_from_name(const std::string_view name)
    -> std::optional<model::Capability> {
  for (const auto capability : {
           model::Capability::tool_calling,
           model::Capability::vision,
           model::Capability::multiple_images,
           model::Capability::video_input,
           model::Capability::audio_input,
           model::Capability::reasoning,
           model::Capability::reasoning_effort,
           model::Capability::response_schema,
           model::Capability::log_probabilities,
           model::Capability::web_search,
           model::Capability::x_search,
           model::Capability::tee_attestation,
           model::Capability::end_to_end_encryption,
           model::Capability::optimized_for_code,
       }) {
    if (model::capability_name(capability) == name) return capability;
  }
  return std::nullopt;
}

[[nodiscard]] auto price_json(const model::Price& price) -> Json {
  return {{"usd", price.usd ? Json(price.usd->to_string()) : Json(nullptr)},
          {"diem", price.diem ? Json(price.diem->to_string()) : Json(nullptr)}};
}

[[nodiscard]] auto parse_price(const Json& value) -> model::Price {
  model::Price result;
  if (!value.is_object())
    throw std::runtime_error{"price is not an object"};
  if (value.contains("usd") && !value.at("usd").is_null()) {
    auto amount =
        domain::DecimalAmount::from(value.at("usd").get<std::string>());
    if (!amount)
      throw std::runtime_error{"USD price is invalid"};
    result.usd = *amount;
  }
  if (value.contains("diem") && !value.at("diem").is_null()) {
    auto amount =
        domain::DecimalAmount::from(value.at("diem").get<std::string>());
    if (!amount)
      throw std::runtime_error{"diem price is invalid"};
    result.diem = *amount;
  }
  return result;
}

[[nodiscard]] auto optional_price_json(
    const std::optional<model::Price>& price) -> Json {
  return price ? price_json(*price) : Json(nullptr);
}

[[nodiscard]] auto parse_optional_price(const Json& value)
    -> std::optional<model::Price> {
  return value.is_null() ? std::nullopt
                         : std::optional<model::Price>{parse_price(value)};
}

[[nodiscard]] auto tier_json(const model::PriceTier& tier) -> Json {
  return {{"input", optional_price_json(tier.input)},
          {"output", optional_price_json(tier.output)},
          {"cache_input", optional_price_json(tier.cache_input)},
          {"cache_write", optional_price_json(tier.cache_write)}};
}

[[nodiscard]] auto parse_tier(const Json& value) -> model::PriceTier {
  if (!value.is_object()) throw std::runtime_error{"price tier is not an object"};
  return {parse_optional_price(value.at("input")),
          parse_optional_price(value.at("output")),
          parse_optional_price(value.at("cache_input")),
          parse_optional_price(value.at("cache_write"))};
}

[[nodiscard]] auto entry_json(const model::CatalogEntry& entry) -> Json {
  auto capabilities = Json::array();
  for (const auto& capability : entry.capabilities) {
    capabilities.push_back(
        {{"name", model::capability_name(capability.capability)},
         {"supported", capability.supported ? Json(*capability.supported)
                                              : Json(nullptr)}});
  }
  Json pricing = nullptr;
  if (entry.pricing) {
    pricing = {{"base", tier_json(entry.pricing->base)},
               {"extended_threshold_tokens",
                entry.pricing->extended_threshold_tokens
                    ? Json(*entry.pricing->extended_threshold_tokens)
                    : Json(nullptr)},
               {"extended", entry.pricing->extended
                                ? tier_json(*entry.pricing->extended)
                                : Json(nullptr)},
               {"generation", optional_price_json(entry.pricing->generation)}};
  }
  return {{"id", entry.id.value()},
          {"type", entry.type},
          {"name", entry.name ? Json(*entry.name) : Json(nullptr)},
          {"context_window_tokens",
           entry.context_window_tokens ? Json(*entry.context_window_tokens)
                                       : Json(nullptr)},
          {"maximum_output_tokens",
           entry.maximum_output_tokens ? Json(*entry.maximum_output_tokens)
                                       : Json(nullptr)},
          {"offline", entry.offline},
          {"traits", entry.traits},
          {"capabilities", std::move(capabilities)},
          {"pricing", std::move(pricing)}};
}

[[nodiscard]] auto parse_entry(const Json& value) -> model::CatalogEntry {
  if (!value.is_object()) throw std::runtime_error{"entry is not an object"};
  auto id = domain::ModelId::from(value.at("id").get<std::string>());
  if (!id) throw std::runtime_error{"model ID is invalid"};
  model::CatalogEntry result{std::move(*id),
                             value.at("type").get<std::string>()};
  if (!value.at("name").is_null())
    result.name = value.at("name").get<std::string>();
  if (!value.at("context_window_tokens").is_null())
    result.context_window_tokens =
        value.at("context_window_tokens").get<std::uint64_t>();
  if (!value.at("maximum_output_tokens").is_null())
    result.maximum_output_tokens =
        value.at("maximum_output_tokens").get<std::uint64_t>();
  result.offline = value.at("offline").get<bool>();
  result.traits = value.at("traits").get<std::vector<std::string>>();
  for (const auto& encoded : value.at("capabilities")) {
    auto capability = capability_from_name(encoded.at("name").get<std::string>());
    if (!capability) continue;
    std::optional<bool> supported;
    if (!encoded.at("supported").is_null())
      supported = encoded.at("supported").get<bool>();
    result.capabilities.push_back({*capability, supported});
  }
  if (!value.at("pricing").is_null()) {
    const auto& pricing = value.at("pricing");
    model::Pricing parsed{parse_tier(pricing.at("base"))};
    if (!pricing.at("extended_threshold_tokens").is_null())
      parsed.extended_threshold_tokens =
          pricing.at("extended_threshold_tokens").get<std::uint64_t>();
    if (!pricing.at("extended").is_null())
      parsed.extended = parse_tier(pricing.at("extended"));
    parsed.generation = parse_optional_price(pricing.at("generation"));
    result.pricing = std::move(parsed);
  }
  return result;
}

[[nodiscard]] auto parse_document(const std::string& bytes)
    -> std::expected<model::CatalogSnapshot, model::CatalogError> {
  try {
    std::vector<std::unordered_set<std::string>> object_keys;
    auto document = Json::parse(
        bytes, [&object_keys](const int, const Json::parse_event_t event,
                             Json& parsed) {
          if (event == Json::parse_event_t::object_start) {
            object_keys.emplace_back();
          } else if (event == Json::parse_event_t::key) {
            if (object_keys.empty() ||
                !object_keys.back().insert(parsed.get<std::string>()).second) {
              throw DuplicateJsonKey{};
            }
          } else if (event == Json::parse_event_t::object_end &&
                     !object_keys.empty()) {
            object_keys.pop_back();
          }
          return true;
        });
    if (!document.is_object() || document.at("schema_version") != cache_schema_version ||
        !document.at("entries").is_array()) {
      return failure("model catalog cache schema is invalid");
    }
    model::CatalogSnapshot result{
        std::chrono::sys_time<std::chrono::milliseconds>{
            std::chrono::milliseconds{
                document.at("fetched_at_ms").get<std::int64_t>()}}};
    result.source_id = document.at("source_id").get<std::string>();
    if (!document.at("source_revision").is_null()) {
      result.source_revision =
          document.at("source_revision").get<std::string>();
    }
    result.entries.reserve(document.at("entries").size());
    for (const auto& entry : document.at("entries"))
      result.entries.push_back(parse_entry(entry));
    return result;
  } catch (...) {
    return failure("model catalog cache is not valid schema-versioned JSON");
  }
}

[[nodiscard]] auto encode_document(const model::CatalogSnapshot& snapshot)
    -> std::string {
  auto entries = Json::array();
  for (const auto& entry : snapshot.entries) entries.push_back(entry_json(entry));
  return Json{{"schema_version", cache_schema_version},
              {"fetched_at_ms", snapshot.fetched_at.time_since_epoch().count()},
              {"source_id", snapshot.source_id},
              {"source_revision", snapshot.source_revision
                                      ? Json(*snapshot.source_revision)
                                      : Json(nullptr)},
              {"entries", std::move(entries)}}
             .dump();
}

}  // namespace

auto JsonModelCatalogCache::load(const std::stop_token stop_token)
    -> std::expected<std::optional<model::CatalogSnapshot>,
                     model::CatalogError> {
  if (stop_token.stop_requested()) return cancelled();
#ifndef _WIN32
  const auto native = m_path.string();
  const int descriptor = ::open(native.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ENOENT) return std::nullopt;
    return failure("model catalog cache could not be opened safely");
  }
  struct stat metadata {};
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0 || metadata.st_size < 0 ||
      static_cast<std::uintmax_t>(metadata.st_size) > m_maximum_bytes) {
    ::close(descriptor);
    return failure("model catalog cache permissions, type, or size are invalid");
  }
  std::string bytes(static_cast<std::size_t>(metadata.st_size), '\0');
  std::size_t offset{};
  while (offset < bytes.size()) {
    if (stop_token.stop_requested()) {
      ::close(descriptor);
      return cancelled();
    }
    const auto count = ::read(descriptor, bytes.data() + offset,
                              bytes.size() - offset);
    if (count <= 0) {
      ::close(descriptor);
      return failure("model catalog cache could not be read completely");
    }
    offset += static_cast<std::size_t>(count);
  }
  ::close(descriptor);
#else
  std::ifstream input{m_path, std::ios::binary};
  if (!input) return std::nullopt;
  std::string bytes{std::istreambuf_iterator<char>{input}, {}};
  if (bytes.size() > m_maximum_bytes)
    return failure("model catalog cache exceeds its size limit");
#endif
  auto parsed = parse_document(bytes);
  if (!parsed) return std::unexpected(std::move(parsed.error()));
  return std::optional<model::CatalogSnapshot>{std::move(*parsed)};
}

auto JsonModelCatalogCache::store(const model::CatalogSnapshot& snapshot,
                                  const std::stop_token stop_token)
    -> std::expected<void, model::CatalogError> {
  if (stop_token.stop_requested()) return cancelled();
  if (auto valid = model::validate_catalog(snapshot); !valid)
    return std::unexpected(std::move(valid.error()));
  auto bytes = encode_document(snapshot);
  if (bytes.size() > m_maximum_bytes)
    return failure("model catalog cache exceeds its size limit");
  std::error_code error;
  const auto parent_status =
      std::filesystem::symlink_status(m_path.parent_path(), error);
  if (!error && std::filesystem::is_symlink(parent_status))
    return failure("model catalog cache directory cannot be a symlink");
  error.clear();
  std::filesystem::create_directories(m_path.parent_path(), error);
  if (error) return failure("model catalog cache directory could not be created");
  std::filesystem::permissions(
      m_path.parent_path(), std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace, error);
  if (error) return failure("model catalog cache directory permissions could not be set");
  const auto target_status = std::filesystem::symlink_status(m_path, error);
  if (!error && std::filesystem::exists(target_status) &&
      (!std::filesystem::is_regular_file(target_status) ||
       std::filesystem::is_symlink(target_status))) {
    return failure("model catalog cache target is not a safe regular file");
  }
  error.clear();
#ifndef _WIN32
  static std::atomic_uint64_t sequence{};
  auto temporary = m_path;
  temporary += ".tmp-" + std::to_string(::getpid()) + "-" +
               std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
  const auto native = temporary.string();
  const int descriptor = ::open(native.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                    O_NOFOLLOW,
                                S_IRUSR | S_IWUSR);
  if (descriptor < 0) return failure("model catalog cache temporary file could not be created");
  std::size_t offset{};
  bool ok{true};
  while (offset < bytes.size()) {
    if (stop_token.stop_requested()) {
      ok = false;
      break;
    }
    const auto count = ::write(descriptor, bytes.data() + offset,
                               bytes.size() - offset);
    if (count <= 0) {
      ok = false;
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  if (ok) ok = ::fsync(descriptor) == 0;
  if (::close(descriptor) != 0) ok = false;
  if (stop_token.stop_requested()) {
    ::unlink(native.c_str());
    return cancelled();
  }
  if (!ok || ::rename(native.c_str(), m_path.string().c_str()) != 0) {
    ::unlink(native.c_str());
    return failure("model catalog cache could not be published atomically");
  }
  const int directory =
      ::open(m_path.parent_path().string().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0 || ::fsync(directory) != 0) {
    if (directory >= 0) ::close(directory);
    return failure("model catalog cache directory could not be synchronized");
  }
  ::close(directory);
#else
  auto temporary = m_path;
  temporary += ".tmp";
  std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output) return failure("model catalog cache could not be written");
  std::filesystem::rename(temporary, m_path, error);
  if (error) return failure("model catalog cache could not be published atomically");
#endif
  return {};
}

auto process_model_catalog_cache_path()
    -> std::expected<std::filesystem::path, model::CatalogError> {
  try {
    if (const auto* xdg = std::getenv("XDG_CACHE_HOME");
        xdg != nullptr && *xdg != '\0') {
      std::filesystem::path root{xdg};
      if (!root.is_absolute())
        return failure("XDG_CACHE_HOME must be an absolute path");
      return root / "aiforge" / "model-catalog.json";
    }
    const auto* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0')
      return failure("HOME is required when XDG_CACHE_HOME is unset");
    std::filesystem::path root{home};
    if (!root.is_absolute()) return failure("HOME must be an absolute path");
    return root / ".cache" / "aiforge" / "model-catalog.json";
  } catch (...) {
    return failure("model catalog cache path could not be resolved");
  }
}

}  // namespace aiforge::adapters
