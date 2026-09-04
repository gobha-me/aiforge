#include <aiforge/adapters/image_tool.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <aiforge/detail/sha256.hpp>
#include <aiforge/detail/utf8_text.hpp>
#include <aiforge/runtime/tool_policy.hpp>

#include <nlohmann/json.hpp>

namespace aiforge::adapters {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumArgumentBytes{3U * 1024U * 1024U};
constexpr std::size_t kMaximumPromptBytes{1024U * 1024U};
constexpr std::size_t kArgumentFramingBytes{256U};
constexpr std::chrono::hours kMaximumCatalogLifetime{24U * 7U};
constexpr std::chrono::milliseconds kMaximumTimeout{std::chrono::minutes{10}};

class DuplicateJsonKey final : public std::exception {};

struct ParsedArguments {
  std::string prompt;
  std::string format;
};

[[nodiscard]] auto registry_error(std::string message)
    -> std::unexpected<runtime::ToolRegistryError> {
  return std::unexpected(runtime::ToolRegistryError{
      runtime::ToolRegistryErrorCode::invalid_declaration, std::move(message)});
}

[[nodiscard]] auto execution_error(
    const runtime::ToolExecutionErrorCode code, std::string message,
    const bool retryable = false,
    std::optional<runtime::ToolSpendDisposition> spend = std::nullopt)
    -> std::unexpected<runtime::ToolExecutionError> {
  return std::unexpected(runtime::ToolExecutionError{
      code, std::move(message), retryable, std::move(spend)});
}

[[nodiscard]] auto parse_json(const std::string& text) -> Json {
  std::vector<std::set<std::string>> keys;
  const auto callback = [&keys](const int, const Json::parse_event_t event,
                                Json& parsed) {
    if (event == Json::parse_event_t::object_start) {
      keys.emplace_back();
    } else if (event == Json::parse_event_t::key) {
      if (keys.empty() ||
          !keys.back().insert(parsed.get<std::string>()).second) {
        throw DuplicateJsonKey{};
      }
    } else if (event == Json::parse_event_t::object_end) {
      keys.pop_back();
    }
    return true;
  };
  return Json::parse(text, callback, true, false);
}

[[nodiscard]] auto valid_prompt(const std::string& prompt,
                                const std::size_t maximum) -> bool {
  return !prompt.empty() && prompt.size() <= maximum &&
         detail::is_safe_utf8_text(prompt);
}

[[nodiscard]] auto valid_format(const std::string_view format) -> bool {
  return format == "auto" || format == "png" || format == "jpeg" ||
         format == "webp";
}

[[nodiscard]] auto parse_provider_arguments(
    const domain::StructuredDataBlock& arguments,
    const ImageToolConfiguration& configuration)
    -> std::expected<ParsedArguments, runtime::ToolExecutionError> {
  if (arguments.media_type != "application/json" || arguments.data.empty() ||
      arguments.data.size() > configuration.maximum_argument_bytes) {
    return execution_error(runtime::ToolExecutionErrorCode::invalid_arguments,
                           "generate_image arguments must be bounded JSON");
  }
  try {
    const auto root = parse_json(arguments.data);
    if (!root.is_object() || root.empty() || root.size() > 2 ||
        !root.contains("prompt") || !root.at("prompt").is_string() ||
        (root.contains("format") && !root.at("format").is_string()) ||
        std::ranges::any_of(root.items(), [](const auto& item) {
          return item.key() != "prompt" && item.key() != "format";
        })) {
      return execution_error(
          runtime::ToolExecutionErrorCode::invalid_arguments,
          "generate_image requires only prompt and optional format fields");
    }
    ParsedArguments result{root.at("prompt").get<std::string>(), "auto"};
    if (root.contains("format"))
      result.format = root.at("format").get<std::string>();
    if (!valid_prompt(result.prompt,
                      configuration.image_options.maximum_prompt_bytes) ||
        !valid_format(result.format)) {
      return execution_error(
          runtime::ToolExecutionErrorCode::invalid_arguments,
          "generate_image prompt or format is invalid or oversized");
    }
    return result;
  } catch (...) {
    return execution_error(runtime::ToolExecutionErrorCode::invalid_arguments,
                           "generate_image arguments are malformed");
  }
}

[[nodiscard]] auto parse_normalized_arguments(
    const domain::StructuredDataBlock& arguments,
    const ImageToolConfiguration& configuration)
    -> std::expected<ParsedArguments, runtime::ToolExecutionError> {
  if (arguments.media_type != "application/json" || arguments.data.empty() ||
      arguments.data.size() > configuration.maximum_argument_bytes) {
    return execution_error(runtime::ToolExecutionErrorCode::invalid_arguments,
                           "generate_image launch arguments are invalid");
  }
  try {
    const auto root = parse_json(arguments.data);
    if (!root.is_object() || root.size() != 3 || !root.contains("model") ||
        !root.contains("prompt") || !root.contains("format") ||
        !root.at("model").is_string() || !root.at("prompt").is_string() ||
        !root.at("format").is_string() ||
        root.at("model").get<std::string>() != configuration.model_id.value()) {
      return execution_error(runtime::ToolExecutionErrorCode::invalid_arguments,
                             "generate_image launch arguments are invalid");
    }
    ParsedArguments result{root.at("prompt").get<std::string>(),
                           root.at("format").get<std::string>()};
    if (!valid_prompt(result.prompt,
                      configuration.image_options.maximum_prompt_bytes) ||
        !valid_format(result.format)) {
      return execution_error(runtime::ToolExecutionErrorCode::invalid_arguments,
                             "generate_image launch arguments are invalid");
    }
    return result;
  } catch (...) {
    return execution_error(runtime::ToolExecutionErrorCode::invalid_arguments,
                           "generate_image launch arguments are malformed");
  }
}

[[nodiscard]] auto media_type(const std::string_view format)
    -> std::optional<std::string> {
  if (format == "auto") return std::nullopt;
  if (format == "png") return std::string{"image/png"};
  if (format == "jpeg") return std::string{"image/jpeg"};
  if (format == "webp") return std::string{"image/webp"};
  return std::nullopt;
}

[[nodiscard]] auto microunits(const domain::DecimalAmount& amount)
    -> std::optional<std::uint64_t> {
  if (amount.scale() > 6) return std::nullopt;
  auto value = amount.coefficient();
  for (std::uint8_t scale = amount.scale(); scale < 6; ++scale) {
    if (value > std::numeric_limits<std::uint64_t>::max() / 10U)
      return std::nullopt;
    value *= 10U;
  }
  return value;
}

[[nodiscard]] auto scopes(const ImageToolConfiguration& configuration)
    -> std::expected<std::vector<domain::CapabilityScope>,
                     runtime::ToolRegistryError> {
  const auto maximum = microunits(configuration.spend_quote.maximum.amount());
  if (!maximum || *maximum == 0) {
    return registry_error("generate_image USD price is not representable");
  }
  std::vector<domain::CapabilityScope> result{
      {domain::Effect::write, "filesystem.root", configuration.artifact_root},
      {domain::Effect::network, "network.host", configuration.network_host},
      {domain::Effect::spend, "spend.microunits", std::to_string(*maximum)}};
  for (auto& scope : result) {
    auto normalized = runtime::normalize_capability_scope(scope);
    if (!normalized || *normalized != scope) {
      return registry_error("generate_image capability scope is invalid");
    }
  }
  return result;
}

[[nodiscard]] auto artifact_id(const domain::InvocationId& invocation_id)
    -> std::expected<domain::ArtifactId, runtime::ToolExecutionError> {
  detail::Sha256 hash;
  const auto value = invocation_id.value();
  hash.update(std::as_bytes(std::span{value.data(), value.size()}));
  auto result = domain::ArtifactId::from("image-" + hash.finish());
  if (!result) {
    return execution_error(runtime::ToolExecutionErrorCode::internal_failure,
                           "generate_image artifact identity failed");
  }
  return std::move(*result);
}

[[nodiscard]] auto spend_release(const domain::InvocationId& invocation_id)
    -> runtime::ToolSpendDisposition {
  return domain::ToolSpendReleased{invocation_id};
}

[[nodiscard]] auto spend_reconciliation(
    const domain::InvocationId& invocation_id)
    -> runtime::ToolSpendDisposition {
  return domain::ToolSpendReconciliationRequired{
      invocation_id,
      domain::ToolSpendReconciliationReason::transport_outcome_unknown};
}

[[nodiscard]] auto spend_finalization(const domain::InvocationId& invocation_id,
                                      const domain::ToolSpendQuote& quote)
    -> runtime::ToolSpendDisposition {
  return domain::ToolSpendFinalized{domain::ToolSpendFinalization{
      invocation_id, quote.maximum,
      domain::ToolSpendFinalizationBasis::catalog_estimate, std::nullopt}};
}

[[nodiscard]] auto spend_after(
    const domain::InvocationId& invocation_id,
    const ImageArtifactTransportState transport_state,
    const domain::ToolSpendQuote& quote) -> runtime::ToolSpendDisposition {
  switch (transport_state) {
    case ImageArtifactTransportState::not_started:
      return spend_release(invocation_id);
    case ImageArtifactTransportState::outcome_unknown:
      return spend_reconciliation(invocation_id);
    case ImageArtifactTransportState::response_received:
      return spend_finalization(invocation_id, quote);
  }
  return spend_reconciliation(invocation_id);
}

[[nodiscard]] auto tool_error_code(const backend::BackendErrorKind kind)
    -> runtime::ToolExecutionErrorCode {
  switch (kind) {
    case backend::BackendErrorKind::request_rejected:
    case backend::BackendErrorKind::protocol:
      return runtime::ToolExecutionErrorCode::protocol_failure;
    case backend::BackendErrorKind::cancelled:
      return runtime::ToolExecutionErrorCode::cancelled;
    case backend::BackendErrorKind::rate_limited:
    case backend::BackendErrorKind::network:
    case backend::BackendErrorKind::authentication:
    case backend::BackendErrorKind::credential_unavailable:
    case backend::BackendErrorKind::unavailable:
      return runtime::ToolExecutionErrorCode::unavailable;
    case backend::BackendErrorKind::script_mismatch:
    case backend::BackendErrorKind::script_exhausted:
      return runtime::ToolExecutionErrorCode::internal_failure;
  }
  return runtime::ToolExecutionErrorCode::internal_failure;
}

class ImageToolStream final : public runtime::ToolExecutionStream {
 public:
  explicit ImageToolStream(runtime::ToolResult result)
      : m_result(std::move(result)) {}

  auto next(std::stop_token)
      -> std::expected<std::optional<runtime::ToolExecutionEvent>,
                       runtime::ToolExecutionError> override {
    if (m_emitted) return std::optional<runtime::ToolExecutionEvent>{};
    m_emitted = true;
    return std::optional<runtime::ToolExecutionEvent>{std::move(m_result)};
  }

 private:
  runtime::ToolResult m_result;
  bool m_emitted{};
};

class ImageToolExecutor final : public runtime::ToolExecutor {
 public:
  ImageToolExecutor(backend::ImageGenerator& generator,
                    storage::ArtifactStore& artifact_store,
                    ImageToolConfiguration configuration)
      : m_generator(generator), m_artifact_store(artifact_store),
        m_configuration(std::move(configuration)) {}

  auto validate(const domain::StructuredDataBlock& arguments) const
      -> std::expected<runtime::ValidatedToolArguments,
                       runtime::ToolExecutionError> override {
    auto parsed = parse_provider_arguments(arguments, m_configuration);
    if (!parsed) return std::unexpected(std::move(parsed.error()));
    auto required_scopes = scopes(m_configuration);
    if (!required_scopes) {
      return execution_error(runtime::ToolExecutionErrorCode::internal_failure,
                             "generate_image configuration is invalid");
    }
    Json normalized{{"format", parsed->format},
                    {"model", m_configuration.model_id.value()},
                    {"prompt", parsed->prompt}};
    auto encoded = normalized.dump();
    if (encoded.size() > m_configuration.maximum_argument_bytes) {
      return execution_error(runtime::ToolExecutionErrorCode::invalid_arguments,
                             "generate_image arguments are oversized");
    }
    return runtime::ValidatedToolArguments{
        {"application/json", std::move(encoded)},
        std::move(*required_scopes),
        {domain::Effect::write, domain::Effect::network, domain::Effect::spend},
        m_configuration.spend_quote};
  }

  auto start(runtime::ToolInvocation invocation,
             const std::stop_token stop_token)
      -> std::expected<std::unique_ptr<runtime::ToolExecutionStream>,
                       runtime::ToolExecutionError> override {
    auto transport_state = ImageArtifactTransportState::not_started;
    try {
      auto parsed = parse_normalized_arguments(invocation.arguments.value,
                                               m_configuration);
      if (!parsed) {
        auto error = std::move(parsed.error());
        error.spend_disposition = spend_release(invocation.invocation_id);
        return std::unexpected(std::move(error));
      }
      if (!invocation.arguments.spend_quote ||
          *invocation.arguments.spend_quote != m_configuration.spend_quote) {
        return execution_error(
            runtime::ToolExecutionErrorCode::unavailable,
            "generate_image durable price quote no longer matches", false,
            spend_release(invocation.invocation_id));
      }
      auto required_scopes = scopes(m_configuration);
      if (!required_scopes ||
          !std::ranges::all_of(*required_scopes, [&](const auto& required) {
            return std::ranges::any_of(
                invocation.granted_scopes, [&](const auto& granted) {
                  return runtime::capability_scope_covers(granted, required);
                });
          })) {
        return execution_error(runtime::ToolExecutionErrorCode::unavailable,
                               "generate_image authority is unavailable", false,
                               spend_release(invocation.invocation_id));
      }
      if (stop_token.stop_requested()) {
        return execution_error(runtime::ToolExecutionErrorCode::cancelled,
                               "generate_image was cancelled", false,
                               spend_release(invocation.invocation_id));
      }
      auto id = artifact_id(invocation.invocation_id);
      if (!id) {
        auto error = std::move(id.error());
        error.spend_disposition = spend_release(invocation.invocation_id);
        return std::unexpected(std::move(error));
      }
      transport_state = ImageArtifactTransportState::outcome_unknown;
      auto generated = generate_image_artifact(
          m_generator, m_artifact_store,
          {m_configuration.model_id, std::move(parsed->prompt),
           media_type(parsed->format), std::move(*id), invocation.invocation_id,
           std::nullopt},
          m_configuration.image_options, stop_token);
      if (!generated) {
        transport_state = generated.error().transport_state;
        const auto spend =
            spend_after(invocation.invocation_id, transport_state,
                        m_configuration.spend_quote);
        const auto& error = generated.error().error;
        return execution_error(tool_error_code(error.kind),
                               error.redacted_message, error.retryable, spend);
      }
      transport_state = ImageArtifactTransportState::response_received;
      Json result{{"artifact_id", generated->artifact_id.value()},
                  {"byte_size", generated->byte_size},
                  {"media_type", generated->media_type},
                  {"model", m_configuration.model_id.value()},
                  {"status", "generated"}};
      if (generated->width) result["width"] = *generated->width;
      if (generated->height) result["height"] = *generated->height;
      std::vector<domain::ContentBlock> content{
          domain::StructuredDataBlock{"application/json", result.dump()},
          domain::ArtifactReferenceBlock{generated->artifact_id,
                                         std::string{"generated image"}}};
      return std::make_unique<ImageToolStream>(
          runtime::ToolResult{std::move(content),
                              {*generated},
                              spend_finalization(invocation.invocation_id,
                                                 m_configuration.spend_quote)});
    } catch (...) {
      return execution_error(runtime::ToolExecutionErrorCode::internal_failure,
                             "generate_image failed internally", false,
                             spend_after(invocation.invocation_id,
                                         transport_state,
                                         m_configuration.spend_quote));
    }
  }

 private:
  backend::ImageGenerator& m_generator;
  storage::ArtifactStore& m_artifact_store;
  ImageToolConfiguration m_configuration;
};

auto append_field(std::string& canonical, const std::string_view value)
    -> void {
  canonical += std::to_string(value.size());
  canonical.push_back(':');
  canonical.append(value);
  canonical.push_back(';');
}

[[nodiscard]] auto price_evidence_digest(const model::CatalogSnapshot& snapshot,
                                         const model::CatalogEntry& entry,
                                         const domain::DecimalAmount& amount)
    -> domain::ContentDigest {
  std::string canonical;
  append_field(canonical, "aiforge.image-tool-price.v1");
  append_field(canonical, snapshot.source_id);
  append_field(canonical, snapshot.source_revision.value_or(""));
  append_field(canonical,
               std::to_string(snapshot.fetched_at.time_since_epoch().count()));
  append_field(canonical, entry.id.value());
  append_field(canonical, entry.type);
  append_field(canonical, "generation");
  append_field(canonical, "USD");
  append_field(canonical, amount.to_string());
  detail::Sha256 hash;
  hash.update(std::as_bytes(std::span{canonical.data(), canonical.size()}));
  return {"sha256", hash.finish(),
          static_cast<std::uint64_t>(canonical.size())};
}

} // namespace

auto resolve_image_tool_configuration(
    const model::CatalogSnapshot& snapshot,
    const domain::ModelId& configured_model, std::string artifact_root,
    std::string network_host, const domain::EventTimestamp now,
    const std::chrono::hours catalog_time_to_live)
    -> std::expected<ImageToolConfiguration, runtime::ToolRegistryError> {
  try {
    if (auto valid = model::validate_catalog(snapshot); !valid) {
      return registry_error("generate_image model catalog is invalid");
    }
    if (snapshot.source_id.empty() || snapshot.source_id.size() > 4096 ||
        !detail::is_safe_utf8_text(snapshot.source_id) ||
        (snapshot.source_revision &&
         (snapshot.source_revision->empty() ||
          snapshot.source_revision->size() > 4096 ||
          !detail::is_safe_utf8_text(*snapshot.source_revision)))) {
      return registry_error(
          "generate_image model catalog provenance is invalid");
    }
    if ((snapshot.origin != model::CatalogOrigin::live &&
         snapshot.origin != model::CatalogOrigin::fresh_cache) ||
        catalog_time_to_live <= std::chrono::hours::zero() ||
        catalog_time_to_live > kMaximumCatalogLifetime ||
        snapshot.fetched_at > now) {
      return registry_error("generate_image requires a fresh model catalog");
    }
    const auto lifetime = std::chrono::duration_cast<std::chrono::milliseconds>(
        catalog_time_to_live);
    if (snapshot.fetched_at > domain::EventTimestamp::max() - lifetime) {
      return registry_error("generate_image catalog lifetime is invalid");
    }
    const auto valid_until = snapshot.fetched_at + lifetime;
    if (now >= valid_until) {
      return registry_error("generate_image requires a fresh model catalog");
    }
    const auto* entry = model::find_model(snapshot, configured_model, "image");
    if (entry == nullptr || entry->offline) {
      return registry_error(
          "configured generate_image model is unavailable or unsupported");
    }
    if (!entry->pricing || !entry->pricing->generation ||
        !entry->pricing->generation->usd ||
        entry->pricing->generation->usd->coefficient() == 0 ||
        !microunits(*entry->pricing->generation->usd)) {
      return registry_error(
          "configured generate_image model has no bounded USD price");
    }
    auto maximum =
        domain::MonetaryAmount::create("USD", *entry->pricing->generation->usd);
    if (!maximum) {
      return registry_error(
          "configured generate_image model has no bounded USD price");
    }
    ImageToolConfiguration result{
        configured_model,
        {std::move(*maximum), domain::ToolSpendEstimateBasis::catalog_estimate,
         price_evidence_digest(snapshot, *entry,
                               *entry->pricing->generation->usd),
         valid_until},
        std::move(artifact_root),
        std::move(network_host)};
    if (!domain::valid_tool_spend_quote(result.spend_quote) ||
        !scopes(result)) {
      return registry_error("generate_image configuration is invalid");
    }
    return result;
  } catch (...) {
    return std::unexpected(runtime::ToolRegistryError{
        runtime::ToolRegistryErrorCode::internal_failure,
        "generate_image configuration failed internally"});
  }
}

auto image_tool_declaration(const ImageToolConfiguration& configuration)
    -> std::expected<backend::ToolDeclaration, runtime::ToolRegistryError> {
  try {
    if (!domain::valid_tool_spend_quote(configuration.spend_quote) ||
        configuration.spend_quote.basis !=
            domain::ToolSpendEstimateBasis::catalog_estimate ||
        configuration.maximum_argument_bytes == 0 ||
        configuration.maximum_argument_bytes > kMaximumArgumentBytes ||
        configuration.image_options.maximum_prompt_bytes == 0 ||
        configuration.image_options.maximum_prompt_bytes >
            kMaximumPromptBytes ||
        configuration.maximum_argument_bytes <
            configuration.image_options.maximum_prompt_bytes * 2U +
                configuration.model_id.value().size() * 2U +
                kArgumentFramingBytes ||
        configuration.image_options.requested_media_type ||
        configuration.timeout <= std::chrono::milliseconds::zero() ||
        configuration.timeout > kMaximumTimeout) {
      return registry_error("generate_image limits are invalid or unbounded");
    }
    auto capability_scopes = scopes(configuration);
    if (!capability_scopes)
      return std::unexpected(std::move(capability_scopes.error()));
    Json schema{
        {"type", "object"},
        {"additionalProperties", false},
        {"required", Json::array({"prompt"})},
        {"properties",
         {{"prompt",
           {{"type", "string"},
            {"minLength", 1},
            {"maxLength", configuration.image_options.maximum_prompt_bytes}}},
          {"format",
           {{"enum", Json::array({"auto", "png", "jpeg", "webp"})}}}}}};
    return backend::ToolDeclaration{
        "generate_image",
        "Generate one bounded image with the runtime-configured model and "
        "store it as a content-addressed artifact. This operation requires "
        "runtime-owned write, network, and spend approval.",
        {"application/schema+json", schema.dump()},
        {domain::Effect::write, domain::Effect::network, domain::Effect::spend},
        std::move(*capability_scopes)};
  } catch (...) {
    return std::unexpected(runtime::ToolRegistryError{
        runtime::ToolRegistryErrorCode::internal_failure,
        "generate_image declaration failed internally"});
  }
}

auto register_image_tool(runtime::ToolRegistry& registry,
                         backend::ImageGenerator& generator,
                         storage::ArtifactStore& artifact_store,
                         ImageToolConfiguration configuration)
    -> std::expected<void, runtime::ToolRegistryError> {
  try {
    auto declaration = image_tool_declaration(configuration);
    if (!declaration) return std::unexpected(std::move(declaration.error()));
    const auto timeout = configuration.timeout;
    return registry.register_tool(
        std::move(*declaration),
        std::make_shared<ImageToolExecutor>(generator, artifact_store,
                                            std::move(configuration)),
        runtime::ToolExecutionLimits{64U * 1024U, 1, timeout},
        runtime::ToolExecutorContract{"aiforge.adapters.generate_image", "1"},
        runtime::ToolCategory::media);
  } catch (...) {
    return std::unexpected(runtime::ToolRegistryError{
        runtime::ToolRegistryErrorCode::internal_failure,
        "generate_image registration failed internally"});
  }
}

} // namespace aiforge::adapters
