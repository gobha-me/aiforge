#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <aiforge/adapters/filesystem_artifact_store.hpp>
#include <aiforge/adapters/image_backend.hpp>
#include <aiforge/adapters/image_tool.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/adapters/termforge_image_renderer.hpp>
#include <aiforge/adapters/venice_backend.hpp>
#include <aiforge/adapters/venice_image_generator.hpp>
#include <aiforge/surfaces/image.hpp>
#include <aiforge/testing/scripted_artifact_store.hpp>
#include <aiforge/testing/scripted_image_generator.hpp>
#include <httplib.h>
#include <termforge/drivers/fallback_driver.hpp>
#include <termforge/drivers/kitty_driver.hpp>

namespace {

using namespace aiforge;

constexpr std::array<std::uint8_t, 72> png_bytes{{
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x7B, 0x40, 0xE8, 0xDD, 0x00, 0x00, 0x00,
    0x0F, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xE0, 0x12, 0x91, 0x3B,
    0x31, 0x2D, 0x05, 0x00, 0x05, 0x07, 0x01, 0xFF, 0xBF, 0x07, 0x0A, 0xBA,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
}};

const std::vector<std::uint8_t> webp_bytes{
    82,  73, 70,  70,  34,  0,  0,   0,   87, 69, 66,  80,  86,  80,
    56,  76, 22,  0,   0,   0,  47,  1,   0,  0,  16,  15,  112, 10,
    168, 43, 248, 158, 194, 99, 255, 131, 7,  21, 136, 232, 127, 0,
};

const std::vector<std::uint8_t> animated_webp_bytes{
    82,  73,  70,  70,  192, 0,   0,   0,   87,  69,  66,  80,  86,  80,  56,
    88,  10,  0,   0,   0,   2,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    65,  78,  73,  77,  6,   0,   0,   0,   255, 255, 255, 255, 0,   0,   65,
    78,  77,  70,  72,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   100, 0,   0,   2,   86,  80,  56,  32,  48,  0,   0,
    0,   208, 1,   0,   157, 1,   42,  1,   0,   1,   0,   2,   0,   52,  37,
    160, 2,   116, 186, 1,   248, 0,   3,   176, 0,   254, 240, 196, 11,  255,
    32,  185, 97,  117, 200, 215, 255, 32,  63,  228, 7,   252, 128, 255, 248,
    242, 0,   0,   0,   65,  78,  77,  70,  68,  0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   100, 0,   0,   0,   86,  80,
    56,  32,  44,  0,   0,   0,   148, 1,   0,   157, 1,   42,  1,   0,   1,
    0,   0,   0,   52,  37,  160, 2,   116, 186, 0,   3,   152, 0,   254, 249,
    147, 111, 255, 144, 31,  255, 144, 31,  255, 144, 31,  255, 32,  63,  226,
    23,  123, 32,  48,  0,
};

constexpr std::string_view jpeg_base64{
    "/9j/4AAQSkZJRgABAQAAAAAAAAD/"
    "2wBDAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB"
    "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQH/wAALCAABAAIBAREA/"
    "8QAFAABAAAAAAAAAAAAAAAAAAAAAP/EABQQAQAAAAAAAAAAAAAAAAAAAAD/"
    "2gAIAQEAAD8AP//Z"};

template <typename IdType> auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto bytes(const std::span<const std::uint8_t> value)
    -> std::vector<std::byte> {
  std::vector<std::byte> result;
  result.reserve(value.size());
  for (const auto byte : value)
    result.push_back(static_cast<std::byte>(byte));
  return result;
}

auto decoded_base64(const std::string_view value) -> std::vector<std::byte> {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<std::byte> result;
  std::uint32_t bits{};
  int available{};
  for (const char character : value) {
    if (character == '=') break;
    const auto position = alphabet.find(character);
    REQUIRE(position != std::string_view::npos);
    bits = (bits << 6U) | static_cast<std::uint32_t>(position);
    available += 6;
    if (available >= 8) {
      available -= 8;
      result.push_back(static_cast<std::byte>((bits >> available) & 0xFFU));
    }
  }
  return result;
}

auto context(const std::string& prompt = "draw a blue square")
    -> domain::ConstructedContext {
  return domain::ConstructedContext{
      {{make_id<domain::ContextEntryId>("runtime-entry"),
        domain::ContextEntryKind::instruction,
        domain::InstructionLayer::application_runtime,
        {make_id<domain::MessageId>("runtime-message"),
         domain::Role::system,
         {domain::TextBlock{"generate an image"}},
         std::nullopt},
        {make_id<domain::ContextSourceId>("runtime-source"), std::nullopt,
         std::nullopt},
        0,
        1,
        1},
       {make_id<domain::ContextEntryId>("user-entry"),
        domain::ContextEntryKind::conversation,
        std::nullopt,
        {make_id<domain::MessageId>("user-message"),
         domain::Role::user,
         {domain::TextBlock{prompt}},
         std::nullopt},
        {make_id<domain::ContextSourceId>("user-source"), std::nullopt,
         std::nullopt},
        0,
        2,
        1}},
      {},
      {4096, 1, 0},
      2};
}

auto backend_request(const std::string& inference = "inference")
    -> backend::BackendRequest {
  return {make_id<domain::InferenceId>(inference),
          make_id<domain::MessageId>("assistant"),
          make_id<domain::ModelId>("image-model"),
          context(),
          {},
          {}};
}

auto metadata(const std::string& inference, const std::string& media,
              const std::size_t size) -> domain::ArtifactMetadata {
  return {
      make_id<domain::ArtifactId>("image-" + inference),
      media,
      static_cast<std::uint64_t>(size),
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      std::nullopt,
      2,
      1,
      make_id<domain::InferenceId>(inference)};
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    m_path = std::filesystem::temp_directory_path() /
             ("aiforge-image-test-" + suffix);
    std::filesystem::create_directories(m_path);
#ifndef _WIN32
    REQUIRE(::chmod(m_path.c_str(), S_IRWXU) == 0);
#endif
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
  }
  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

class ImageServer final {
 public:
  explicit ImageServer(const std::size_t response_bytes = 0) {
    if (response_bytes != 0) {
      m_response_content.assign(response_bytes, 'x');
      m_response_media_type = "image/png";
    }
    m_server.Post(
        "/api/v1/image/generate",
        [this](const httplib::Request& request, httplib::Response& response) {
          {
            std::lock_guard lock(m_mutex);
            m_authorization = request.get_header_value("Authorization");
            m_request_body = request.body;
          }
          response.set_content(m_response_content, m_response_media_type);
        });
    m_port = m_server.bind_to_any_port("127.0.0.1");
    REQUIRE(m_port > 0);
    m_thread = std::jthread([this] { m_server.listen_after_bind(); });
  }

  ~ImageServer() {
    m_server.stop();
    if (m_thread.joinable()) m_thread.join();
  }

  [[nodiscard]] auto base_url() const -> std::string {
    return "http://127.0.0.1:" + std::to_string(m_port) + "/api/v1";
  }

  [[nodiscard]] auto authorization() -> std::string {
    std::lock_guard lock(m_mutex);
    return m_authorization;
  }

  [[nodiscard]] auto request_body() -> std::string {
    std::lock_guard lock(m_mutex);
    return m_request_body;
  }

 private:
  httplib::Server m_server;
  int m_port{};
  std::jthread m_thread;
  std::mutex m_mutex;
  std::string m_authorization;
  std::string m_request_body;
  std::string m_response_content{
      R"({"id":"unexpected-json","images":["remote"]})"};
  std::string m_response_media_type{"application/json"};
};

class CancellingImageGenerator final : public backend::ImageGenerator {
 public:
  CancellingImageGenerator(std::stop_source& cancellation,
                           std::vector<std::byte> encoded)
      : m_cancellation(cancellation), m_encoded(std::move(encoded)) {}

  auto generate(backend::ImageGenerationRequest, std::stop_token)
      -> std::expected<backend::GeneratedImage,
                       backend::ImageGenerationError> override {
    m_cancellation.request_stop();
    return backend::GeneratedImage{m_encoded, "image/png"};
  }

 private:
  std::stop_source& m_cancellation;
  std::vector<std::byte> m_encoded;
};

class ThrowingImageGenerator final : public backend::ImageGenerator {
 public:
  auto generate(backend::ImageGenerationRequest, std::stop_token)
      -> std::expected<backend::GeneratedImage,
                       backend::ImageGenerationError> override {
    throw std::runtime_error{"provider adapter failure"};
  }
};

class ThrowingArtifactStore final : public storage::ArtifactStore {
 public:
  auto put(storage::ArtifactWrite, std::span<const std::byte>, std::stop_token)
      -> std::expected<domain::ArtifactMetadata,
                       storage::ArtifactStoreError> override {
    ++calls;
    throw std::runtime_error{"artifact adapter failure"};
  }

  std::size_t calls{};
};

auto image_catalog(
    const model::CatalogOrigin origin = model::CatalogOrigin::live,
    const std::string& price = "0.25") -> model::CatalogSnapshot {
  model::CatalogEntry image{make_id<domain::ModelId>("configured-image"),
                            "image"};
  model::Pricing pricing;
  pricing.generation =
      model::Price{domain::DecimalAmount::from(price).value(), std::nullopt};
  image.pricing = std::move(pricing);
  model::CatalogSnapshot result{
      domain::EventTimestamp{std::chrono::milliseconds{1000}},
      {std::move(image)}};
  result.origin = origin;
  result.source_id = "test.image-models";
  result.source_revision = "revision-1";
  return result;
}

auto image_tool_configuration(
    const model::CatalogSnapshot& catalog = image_catalog())
    -> adapters::ImageToolConfiguration {
  auto result = adapters::resolve_image_tool_configuration(
      catalog, make_id<domain::ModelId>("configured-image"),
      "/state/aiforge/artifacts", "api.example.test",
      domain::EventTimestamp{std::chrono::milliseconds{2000}});
  REQUIRE(result);
  result->image_options.maximum_encoded_bytes = 1024;
  return std::move(*result);
}

class RecordingArtifactStore final : public storage::ArtifactStore {
 public:
  auto put(storage::ArtifactWrite write, std::span<const std::byte> content,
           std::stop_token stop_token)
      -> std::expected<domain::ArtifactMetadata,
                       storage::ArtifactStoreError> override {
    if (stop_token.stop_requested()) {
      return std::unexpected(storage::ArtifactStoreError{
          storage::ArtifactStoreErrorCode::cancelled,
          "artifact write cancelled", false});
    }
    calls.push_back({write, {content.begin(), content.end()}});
    return domain::ArtifactMetadata{
        write.artifact_id,
        write.media_type,
        static_cast<std::uint64_t>(content.size()),
        "sha256:"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        write.producing_invocation_id,
        write.width,
        write.height,
        write.producing_inference_id};
  }

  std::vector<testing::ArtifactStoreCall> calls;
};

auto registered_image_tool(
    testing::ScriptedImageGenerator& generator, storage::ArtifactStore& store,
    adapters::ImageToolConfiguration configuration = image_tool_configuration())
    -> runtime::RegisteredTool {
  runtime::ToolRegistry registry;
  REQUIRE(adapters::register_image_tool(registry, generator, store,
                                        std::move(configuration)));
  auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  const auto* registered = snapshot->find("generate_image");
  REQUIRE(registered != nullptr);
  return *registered;
}

TEST_CASE("image tool catalog gate requires exact fresh capable USD model",
          "[image][tool][catalog][failure]") {
  const auto model_id = make_id<domain::ModelId>("configured-image");
  const auto now = domain::EventTimestamp{std::chrono::milliseconds{2000}};
  const auto resolve = [&](const model::CatalogSnapshot& catalog) {
    return adapters::resolve_image_tool_configuration(
        catalog, model_id, "/state/aiforge/artifacts", "api.example.test", now);
  };

  auto stale = image_catalog(model::CatalogOrigin::stale_cache);
  REQUIRE_FALSE(resolve(stale));

  auto invalid_origin = image_catalog();
  invalid_origin.origin = static_cast<model::CatalogOrigin>(100);
  REQUIRE_FALSE(resolve(invalid_origin));

  auto expired = image_catalog();
  expired.fetched_at = now - std::chrono::hours{24};
  REQUIRE_FALSE(resolve(expired));

  auto future = image_catalog();
  future.fetched_at = now + std::chrono::milliseconds{1};
  REQUIRE_FALSE(resolve(future));

  auto missing_source = image_catalog();
  missing_source.source_id.clear();
  REQUIRE_FALSE(resolve(missing_source));

  auto unsafe_revision = image_catalog();
  unsafe_revision.source_revision = "revision\x1b";
  REQUIRE_FALSE(resolve(unsafe_revision));

  auto missing = image_catalog();
  missing.entries.front().id = make_id<domain::ModelId>("other-image");
  REQUIRE_FALSE(resolve(missing));

  auto wrong_type = image_catalog();
  wrong_type.entries.front().type = "text";
  REQUIRE_FALSE(resolve(wrong_type));

  auto offline = image_catalog();
  offline.entries.front().offline = true;
  REQUIRE_FALSE(resolve(offline));

  auto no_usd = image_catalog();
  no_usd.entries.front().pricing->generation->usd.reset();
  no_usd.entries.front().pricing->generation->diem =
      domain::DecimalAmount::from("2").value();
  REQUIRE_FALSE(resolve(no_usd));

  REQUIRE_FALSE(resolve(image_catalog(model::CatalogOrigin::live, "0")));
  REQUIRE_FALSE(
      resolve(image_catalog(model::CatalogOrigin::live, "0.0000001")));

  auto no_pricing = image_catalog();
  no_pricing.entries.front().pricing.reset();
  REQUIRE_FALSE(resolve(no_pricing));

  auto no_generation_price = image_catalog();
  no_generation_price.entries.front().pricing->generation.reset();
  REQUIRE_FALSE(resolve(no_generation_price));

  const auto accepted = resolve(image_catalog());
  REQUIRE(accepted);
  CHECK(accepted->model_id == model_id);
  CHECK(accepted->spend_quote.maximum.unit() == "USD");
  CHECK(accepted->spend_quote.maximum.amount().to_string() == "0.25");
  CHECK(accepted->spend_quote.basis ==
        domain::ToolSpendEstimateBasis::catalog_estimate);
  CHECK(accepted->spend_quote.valid_until ==
        domain::EventTimestamp{std::chrono::milliseconds{1000}} +
            std::chrono::hours{24});
  CHECK(resolve(image_catalog(model::CatalogOrigin::fresh_cache)));
}

TEST_CASE("image tool quote survives live catalog restart as fresh cache",
          "[image][tool][catalog][resume]") {
  const auto model_id = make_id<domain::ModelId>("configured-image");
  const auto now = domain::EventTimestamp{std::chrono::milliseconds{2000}};
  auto live = image_catalog(model::CatalogOrigin::live);
  auto cached = live;
  cached.origin = model::CatalogOrigin::fresh_cache;

  const auto resolve = [&](const model::CatalogSnapshot& catalog) {
    return adapters::resolve_image_tool_configuration(
        catalog, model_id, "/state/aiforge/artifacts", "api.example.test", now);
  };
  const auto before_restart = resolve(live);
  const auto after_restart = resolve(cached);
  REQUIRE(before_restart);
  REQUIRE(after_restart);
  CHECK(after_restart->spend_quote == before_restart->spend_quote);
}

TEST_CASE(
    "image tool declaration is exact and model arguments are runtime-owned",
    "[image][tool][policy][failure]") {
  testing::ScriptedImageGenerator generator;
  RecordingArtifactStore artifacts;
  const auto tool = registered_image_tool(generator, artifacts);

  CHECK(tool.category == runtime::ToolCategory::media);
  CHECK(tool.declaration.effects ==
        (std::vector{domain::Effect::write, domain::Effect::network,
                     domain::Effect::spend}));
  REQUIRE(tool.declaration.capability_scopes.size() == 3);
  CHECK(tool.declaration.capability_scopes[0] ==
        (domain::CapabilityScope{domain::Effect::write, "filesystem.root",
                                 "/state/aiforge/artifacts"}));
  CHECK(tool.declaration.capability_scopes[1] ==
        (domain::CapabilityScope{domain::Effect::network, "network.host",
                                 "api.example.test"}));
  CHECK(tool.declaration.capability_scopes[2] ==
        (domain::CapabilityScope{domain::Effect::spend, "spend.microunits",
                                 "250000"}));
  CHECK(tool.declaration.input_schema.data.find("model") == std::string::npos);
  CHECK(tool.declaration.input_schema.data.find("\"maxLength\":1048576") !=
        std::string::npos);

  for (const auto* forged :
       {R"({"prompt":"blue","model":"attacker-model"})",
        R"({"prompt":"blue","unknown":true})",
        R"({"prompt":"blue","prompt":"red"})", R"({"prompt":""})",
        R"({"prompt":"blue","format":"gif"})", "{"}) {
    CAPTURE(forged);
    const auto rejected =
        tool.executor->validate({"application/json", std::string{forged}});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code ==
          runtime::ToolExecutionErrorCode::invalid_arguments);
  }
  CHECK_FALSE(
      tool.executor->validate({"application/cbor", R"({"prompt":"blue"})"}));
  const auto default_maximum_prompt =
      image_tool_configuration().image_options.maximum_prompt_bytes;
  const auto boundary_prompt = std::string{R"({"prompt":")"} +
                               std::string(default_maximum_prompt, 'x') +
                               R"("})";
  CHECK(tool.executor->validate({"application/json", boundary_prompt}));
  std::string escaped_boundary_prompt{R"({"prompt":")"};
  escaped_boundary_prompt.reserve(default_maximum_prompt * 2U + 16U);
  for (std::size_t index{}; index < default_maximum_prompt; ++index)
    escaped_boundary_prompt += R"(\")";
  escaped_boundary_prompt += R"("})";
  CHECK(tool.executor->validate({"application/json", escaped_boundary_prompt}));
  const auto oversized_prompt = std::string{R"({"prompt":")"} +
                                std::string(default_maximum_prompt + 1U, 'x') +
                                R"("})";
  CHECK_FALSE(tool.executor->validate({"application/json", oversized_prompt}));

  const auto validated = tool.executor->validate(
      {"application/json", R"({"prompt":"blue","format":"png"})"});
  REQUIRE(validated);
  CHECK(validated->value.data ==
        R"({"format":"png","model":"configured-image","prompt":"blue"})");
  CHECK(validated->required_scopes == tool.declaration.capability_scopes);
  CHECK(validated->spend_quote == image_tool_configuration().spend_quote);

  auto invalid_root = image_tool_configuration();
  invalid_root.artifact_root = "/state/../artifacts";
  CHECK_FALSE(adapters::image_tool_declaration(invalid_root));
  auto fixed_media = image_tool_configuration();
  fixed_media.image_options.requested_media_type = "image/png";
  CHECK_FALSE(adapters::image_tool_declaration(fixed_media));
  auto inconsistent_arguments = image_tool_configuration();
  inconsistent_arguments.maximum_argument_bytes = 64U * 1024U;
  CHECK_FALSE(adapters::image_tool_declaration(inconsistent_arguments));
  auto mislabeled_quote = image_tool_configuration();
  mislabeled_quote.spend_quote.basis =
      domain::ToolSpendEstimateBasis::policy_upper_bound;
  CHECK_FALSE(adapters::image_tool_declaration(mislabeled_quote));
}

TEST_CASE("image tool owns artifact and catalog-estimate spend lifecycle",
          "[image][tool][artifact][spend]") {
  const auto encoded = bytes(png_bytes);
  testing::ScriptedImageGenerator generator{{
      {{make_id<domain::ModelId>("configured-image"), "blue", "image/png"},
       backend::GeneratedImage{encoded, "image/png"}},
  }};
  RecordingArtifactStore artifacts;
  const auto tool = registered_image_tool(generator, artifacts);
  const auto validated = tool.executor->validate(
      {"application/json", R"({"prompt":"blue","format":"png"})"});
  REQUIRE(validated);
  const auto invocation = make_id<domain::InvocationId>("image-call");
  auto stream = tool.executor->start({invocation, std::nullopt,
                                      "generate_image", *validated,
                                      validated->required_scopes, tool.limits},
                                     {});
  REQUIRE(stream);
  auto event = (*stream)->next({});
  REQUIRE(event);
  REQUIRE(*event);
  const auto* result = std::get_if<runtime::ToolResult>(&**event);
  REQUIRE(result != nullptr);
  REQUIRE(result->created_artifacts.size() == 1);
  const auto& artifact = result->created_artifacts.front();
  CHECK(artifact.producing_invocation_id == invocation);
  CHECK_FALSE(artifact.producing_inference_id);
  CHECK(artifact.media_type == "image/png");
  REQUIRE(result->spend_disposition);
  const auto* finalized =
      std::get_if<domain::ToolSpendFinalized>(&*result->spend_disposition);
  REQUIRE(finalized != nullptr);
  CHECK(finalized->finalization.invocation_id == invocation);
  CHECK(finalized->finalization.basis ==
        domain::ToolSpendFinalizationBasis::catalog_estimate);
  CHECK(finalized->finalization.amount.amount().to_string() == "0.25");
  CHECK(generator.remaining_exchanges() == 0);
  REQUIRE(artifacts.calls.size() == 1);
  CHECK(artifacts.calls.front().write.producing_invocation_id == invocation);
}

TEST_CASE("image tool distinguishes pre-transport and uncertain failures",
          "[image][tool][spend][failure][cancel]") {
  const auto invocation = make_id<domain::InvocationId>("image-call");

  SECTION("forged normalized model releases before provider transport") {
    testing::ScriptedImageGenerator generator;
    RecordingArtifactStore artifacts;
    const auto tool = registered_image_tool(generator, artifacts);
    auto validated =
        tool.executor->validate({"application/json", R"({"prompt":"blue"})"});
    REQUIRE(validated);
    validated->value.data =
        R"({"format":"auto","model":"forged-image","prompt":"blue"})";
    const auto started = tool.executor->start(
        {invocation, std::nullopt, "generate_image", *validated,
         validated->required_scopes, tool.limits},
        {});
    REQUIRE_FALSE(started);
    REQUIRE(started.error().spend_disposition);
    CHECK(std::holds_alternative<domain::ToolSpendReleased>(
        *started.error().spend_disposition));
    CHECK(generator.recorded_requests().empty());
    CHECK(artifacts.calls.empty());
  }

  SECTION("missing exact scope releases before provider transport") {
    testing::ScriptedImageGenerator generator;
    RecordingArtifactStore artifacts;
    const auto tool = registered_image_tool(generator, artifacts);
    const auto validated =
        tool.executor->validate({"application/json", R"({"prompt":"blue"})"});
    REQUIRE(validated);
    auto incomplete_scopes = validated->required_scopes;
    incomplete_scopes.pop_back();
    const auto started = tool.executor->start(
        {invocation, std::nullopt, "generate_image", *validated,
         std::move(incomplete_scopes), tool.limits},
        {});
    REQUIRE_FALSE(started);
    REQUIRE(started.error().spend_disposition);
    CHECK(std::holds_alternative<domain::ToolSpendReleased>(
        *started.error().spend_disposition));
    CHECK(generator.recorded_requests().empty());
    CHECK(artifacts.calls.empty());
  }

  SECTION("durable quote drift releases before provider transport") {
    for (const std::string_view drift :
         {"missing", "amount", "evidence", "expiry"}) {
      CAPTURE(drift);
      testing::ScriptedImageGenerator generator;
      RecordingArtifactStore artifacts;
      const auto tool = registered_image_tool(generator, artifacts);
      auto validated =
          tool.executor->validate({"application/json", R"({"prompt":"blue"})"});
      REQUIRE(validated);
      REQUIRE(validated->spend_quote);
      if (drift == std::string_view{"missing"}) {
        validated->spend_quote.reset();
      } else if (drift == std::string_view{"amount"}) {
        validated->spend_quote->maximum =
            domain::MonetaryAmount::create(
                "USD", domain::DecimalAmount::from("0.3").value())
                .value();
      } else if (drift == std::string_view{"evidence"}) {
        validated->spend_quote->evidence_digest.value.front() = 'f';
      } else {
        validated->spend_quote->valid_until += std::chrono::milliseconds{1};
      }
      const auto started = tool.executor->start(
          {invocation, std::nullopt, "generate_image", *validated,
           validated->required_scopes, tool.limits},
          {});
      REQUIRE_FALSE(started);
      REQUIRE(started.error().spend_disposition);
      CHECK(std::holds_alternative<domain::ToolSpendReleased>(
          *started.error().spend_disposition));
      CHECK(generator.recorded_requests().empty());
      CHECK(artifacts.calls.empty());
    }
  }

  SECTION("cancellation before provider transport releases the reservation") {
    testing::ScriptedImageGenerator generator;
    RecordingArtifactStore artifacts;
    const auto tool = registered_image_tool(generator, artifacts);
    const auto validated =
        tool.executor->validate({"application/json", R"({"prompt":"blue"})"});
    REQUIRE(validated);
    std::stop_source stop;
    stop.request_stop();
    const auto started = tool.executor->start(
        {invocation, std::nullopt, "generate_image", *validated,
         validated->required_scopes, tool.limits},
        stop.get_token());
    REQUIRE_FALSE(started);
    REQUIRE(started.error().spend_disposition);
    CHECK(std::holds_alternative<domain::ToolSpendReleased>(
        *started.error().spend_disposition));
    CHECK(generator.recorded_requests().empty());
    CHECK(artifacts.calls.empty());
  }

  SECTION("provider rejection retains the reservation for reconciliation") {
    testing::ScriptedImageGenerator generator{{
        {{make_id<domain::ModelId>("configured-image"), "blue", std::nullopt},
         backend::ImageGenerationError{
             backend::ImageGenerationErrorCode::rate_limited,
             "image provider rejected the request", true, 429}},
    }};
    RecordingArtifactStore artifacts;
    const auto tool = registered_image_tool(generator, artifacts);
    const auto validated =
        tool.executor->validate({"application/json", R"({"prompt":"blue"})"});
    REQUIRE(validated);
    const auto started = tool.executor->start(
        {invocation, std::nullopt, "generate_image", *validated,
         validated->required_scopes, tool.limits},
        {});
    REQUIRE_FALSE(started);
    REQUIRE(started.error().spend_disposition);
    const auto* reconciliation =
        std::get_if<domain::ToolSpendReconciliationRequired>(
            &*started.error().spend_disposition);
    REQUIRE(reconciliation != nullptr);
    CHECK(reconciliation->reason ==
          domain::ToolSpendReconciliationReason::transport_outcome_unknown);
    CHECK(artifacts.calls.empty());
  }

  SECTION("throwing provider retains the reservation for reconciliation") {
    ThrowingImageGenerator generator;
    RecordingArtifactStore artifacts;
    runtime::ToolRegistry registry;
    REQUIRE(adapters::register_image_tool(registry, generator, artifacts,
                                          image_tool_configuration()));
    const auto snapshot = registry.snapshot();
    REQUIRE(snapshot);
    const auto* tool = snapshot->find("generate_image");
    REQUIRE(tool != nullptr);
    const auto validated =
        tool->executor->validate({"application/json", R"({"prompt":"blue"})"});
    REQUIRE(validated);
    const auto started = tool->executor->start(
        {invocation, std::nullopt, "generate_image", *validated,
         validated->required_scopes, tool->limits},
        {});
    REQUIRE_FALSE(started);
    REQUIRE(started.error().spend_disposition);
    CHECK(std::holds_alternative<domain::ToolSpendReconciliationRequired>(
        *started.error().spend_disposition));
    CHECK(artifacts.calls.empty());
  }

  SECTION("invalid returned media retains its catalog estimate basis") {
    std::vector<std::byte> truncated(16, std::byte{0});
    testing::ScriptedImageGenerator generator{{
        {{make_id<domain::ModelId>("configured-image"), "blue", std::nullopt},
         backend::GeneratedImage{truncated, "image/png"}},
    }};
    RecordingArtifactStore artifacts;
    const auto tool = registered_image_tool(generator, artifacts);
    const auto validated =
        tool.executor->validate({"application/json", R"({"prompt":"blue"})"});
    REQUIRE(validated);
    const auto started = tool.executor->start(
        {invocation, std::nullopt, "generate_image", *validated,
         validated->required_scopes, tool.limits},
        {});
    REQUIRE_FALSE(started);
    REQUIRE(started.error().spend_disposition);
    const auto* finalized = std::get_if<domain::ToolSpendFinalized>(
        &*started.error().spend_disposition);
    REQUIRE(finalized != nullptr);
    CHECK(finalized->finalization.basis ==
          domain::ToolSpendFinalizationBasis::catalog_estimate);
    CHECK(artifacts.calls.empty());
  }

  SECTION("artifact adapter exception retains its catalog estimate basis") {
    const auto encoded = bytes(png_bytes);
    testing::ScriptedImageGenerator generator{{
        {{make_id<domain::ModelId>("configured-image"), "blue", std::nullopt},
         backend::GeneratedImage{encoded, "image/png"}},
    }};
    ThrowingArtifactStore artifacts;
    runtime::ToolRegistry registry;
    REQUIRE(adapters::register_image_tool(registry, generator, artifacts,
                                          image_tool_configuration()));
    const auto snapshot = registry.snapshot();
    REQUIRE(snapshot);
    const auto* tool = snapshot->find("generate_image");
    REQUIRE(tool != nullptr);
    const auto validated =
        tool->executor->validate({"application/json", R"({"prompt":"blue"})"});
    REQUIRE(validated);
    const auto started = tool->executor->start(
        {invocation, std::nullopt, "generate_image", *validated,
         validated->required_scopes, tool->limits},
        {});
    REQUIRE_FALSE(started);
    REQUIRE(started.error().spend_disposition);
    const auto* finalized = std::get_if<domain::ToolSpendFinalized>(
        &*started.error().spend_disposition);
    REQUIRE(finalized != nullptr);
    CHECK(finalized->finalization.basis ==
          domain::ToolSpendFinalizationBasis::catalog_estimate);
    CHECK(artifacts.calls == 1);
  }
}

TEST_CASE("image backend validates and stores original PNG bytes") {
  const auto encoded = bytes(png_bytes);
  const auto stored = metadata("inference", "image/png", encoded.size());
  testing::ScriptedImageGenerator generator{{
      {{make_id<domain::ModelId>("image-model"), "draw a blue square",
        "image/png"},
       backend::GeneratedImage{encoded, "image/png"}},
  }};
  testing::ScriptedArtifactStore artifacts{{
      {{{make_id<domain::ArtifactId>("image-inference"), "image/png",
         std::nullopt, make_id<domain::InferenceId>("inference"), 2, 1},
        encoded},
       stored},
  }};
  adapters::ImageBackend backend{
      generator, artifacts, {"image/png", 1024, 1024}};

  auto stream = backend.start(backend_request(), {});
  REQUIRE(stream);
  auto started = (*stream)->next({});
  auto produced = (*stream)->next({});
  auto finished = (*stream)->next({});
  REQUIRE(started);
  REQUIRE(produced);
  REQUIRE(finished);
  CHECK(std::holds_alternative<backend::ResponseStarted>(**started));
  CHECK(std::get<backend::ImageArtifactProduced>(**produced).artifact ==
        stored);
  CHECK(std::holds_alternative<backend::ResponseFinished>(**finished));
  CHECK(generator.remaining_exchanges() == 0);
  CHECK(artifacts.remaining_exchanges() == 0);
}

TEST_CASE("image backend rejects corrupt and mismatched media before storage") {
  SECTION("truncated PNG") {
    auto truncated = bytes(png_bytes);
    truncated.resize(20);
    testing::ScriptedImageGenerator generator{{
        {{make_id<domain::ModelId>("image-model"), "draw a blue square",
          std::nullopt},
         backend::GeneratedImage{truncated, "image/png"}},
    }};
    testing::ScriptedArtifactStore artifacts;
    adapters::ImageBackend backend{
        generator, artifacts, {std::nullopt, 1024, 1024}};
    auto result = backend.start(backend_request(), {});
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == backend::BackendErrorKind::protocol);
    CHECK(artifacts.recorded_calls().empty());
  }

  SECTION("signature and media type disagree") {
    const auto encoded = bytes(png_bytes);
    testing::ScriptedImageGenerator generator{{
        {{make_id<domain::ModelId>("image-model"), "draw a blue square",
          std::nullopt},
         backend::GeneratedImage{encoded, "image/jpeg"}},
    }};
    testing::ScriptedArtifactStore artifacts;
    adapters::ImageBackend backend{
        generator, artifacts, {std::nullopt, 1024, 1024}};
    auto result = backend.start(backend_request(), {});
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == backend::BackendErrorKind::protocol);
    CHECK(artifacts.recorded_calls().empty());
  }
}

TEST_CASE("image backend accepts bounded static WebP") {
  const auto encoded = bytes(webp_bytes);
  const auto stored = metadata("webp-inference", "image/webp", encoded.size());
  testing::ScriptedImageGenerator generator{{
      {{make_id<domain::ModelId>("image-model"), "draw a blue square",
        std::nullopt},
       backend::GeneratedImage{encoded, "image/webp"}},
  }};
  testing::ScriptedArtifactStore artifacts{{
      {{{make_id<domain::ArtifactId>("image-webp-inference"), "image/webp",
         std::nullopt, make_id<domain::InferenceId>("webp-inference"), 2, 1},
        encoded},
       stored},
  }};
  adapters::ImageBackend backend{
      generator, artifacts, {std::nullopt, 1024, 1024}};
  auto result = backend.start(backend_request("webp-inference"), {});
  CHECK(result);
  CHECK(artifacts.remaining_exchanges() == 0);
}

TEST_CASE("image backend rejects animated WebP before artifact publication") {
  const auto encoded = bytes(animated_webp_bytes);
  testing::ScriptedImageGenerator generator{{
      {{make_id<domain::ModelId>("image-model"), "draw a blue square",
        std::nullopt},
       backend::GeneratedImage{encoded, "image/webp"}},
  }};
  testing::ScriptedArtifactStore artifacts;
  adapters::ImageBackend backend{
      generator, artifacts, {std::nullopt, 1024, 1024}};
  auto result = backend.start(backend_request(), {});
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == backend::BackendErrorKind::protocol);
  CHECK(artifacts.recorded_calls().empty());
}

TEST_CASE("image backend validates and stores original JPEG bytes") {
  const auto encoded = decoded_base64(jpeg_base64);
  const auto stored = metadata("jpeg-inference", "image/jpeg", encoded.size());
  testing::ScriptedImageGenerator generator{{
      {{make_id<domain::ModelId>("image-model"), "draw a blue square",
        "image/jpeg"},
       backend::GeneratedImage{encoded, "image/jpeg"}},
  }};
  testing::ScriptedArtifactStore artifacts{{
      {{{make_id<domain::ArtifactId>("image-jpeg-inference"), "image/jpeg",
         std::nullopt, make_id<domain::InferenceId>("jpeg-inference"), 2, 1},
        encoded},
       stored},
  }};
  adapters::ImageBackend backend{
      generator, artifacts, {"image/jpeg", 1024, 1024}};
  auto result = backend.start(backend_request("jpeg-inference"), {});
  CHECK(result);
  CHECK(artifacts.remaining_exchanges() == 0);
}

TEST_CASE("Venice image adapter rejects JSON where owned binary was required") {
  ImageServer server;
  constexpr std::string_view api_key = "test-image-secret";
  auto secret = credentials::make_secret(std::string{api_key});
  REQUIRE(secret);
  adapters::VeniceImageGenerator generator{
      std::move(*secret),
      {server.base_url(), std::chrono::seconds{1}, std::chrono::seconds{1},
       std::chrono::seconds{1}}};

  auto generated = generator.generate(
      {make_id<domain::ModelId>("image-model"), "blue square", "image/png"});
  REQUIRE_FALSE(generated);
  CHECK(generated.error().code == backend::ImageGenerationErrorCode::protocol);
  CHECK(generated.error().redacted_message.find(api_key) == std::string::npos);
  CHECK(server.authorization() == "Bearer " + std::string{api_key});
  CHECK(server.request_body().find("\"return_binary\":true") !=
        std::string::npos);
}

TEST_CASE("shared Venice backend exposes the same bounded image boundary") {
  ImageServer server;
  constexpr std::string_view api_key = "test-shared-backend-secret";
  auto secret = credentials::make_secret(std::string{api_key});
  REQUIRE(secret);
  adapters::VeniceBackend shared_backend{
      std::move(*secret),
      {server.base_url(), std::chrono::seconds{1}, std::chrono::seconds{1},
       std::chrono::seconds{1}, 16}};

  auto generated =
      shared_backend.generate({make_id<domain::ModelId>("configured-image"),
                               "blue square", "image/png"});
  REQUIRE_FALSE(generated);
  CHECK(generated.error().code == backend::ImageGenerationErrorCode::protocol);
  CHECK(generated.error().redacted_message.find(api_key) == std::string::npos);
  CHECK(server.authorization() == "Bearer " + std::string{api_key});
  CHECK(server.request_body().find("\"model\":\"configured-image\"") !=
        std::string::npos);
  CHECK(server.request_body().find("\"return_binary\":true") !=
        std::string::npos);
}

TEST_CASE("Venice image transport bounds response bytes before buffering") {
  ImageServer server{33};
  auto secret = credentials::make_secret("test-response-limit-secret");
  REQUIRE(secret);
  adapters::VeniceBackendOptions options;
  options.base_url = server.base_url();
  options.connect_timeout = std::chrono::seconds{1};
  options.read_timeout = std::chrono::seconds{1};
  options.write_timeout = std::chrono::seconds{1};
  options.maximum_image_response_bytes = 32;
  adapters::VeniceBackend shared_backend{std::move(*secret), options};

  const auto generated =
      shared_backend.generate({make_id<domain::ModelId>("configured-image"),
                               "blue square", "image/png"});
  REQUIRE_FALSE(generated);
  CHECK(generated.error().code == backend::ImageGenerationErrorCode::protocol);
  CHECK(server.request_body().find("\"return_binary\":true") !=
        std::string::npos);
}

TEST_CASE("image backend fails closed on cancellation and encoded limits") {
  SECTION("already cancelled") {
    testing::ScriptedImageGenerator generator;
    testing::ScriptedArtifactStore artifacts;
    adapters::ImageBackend backend{
        generator, artifacts, {std::nullopt, 1024, 1024}};
    std::stop_source cancellation;
    cancellation.request_stop();
    auto result = backend.start(backend_request(), cancellation.get_token());
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == backend::BackendErrorKind::cancelled);
    CHECK(generator.recorded_requests().empty());
  }

  SECTION("cancelled after provider completion but before decode") {
    std::stop_source cancellation;
    CancellingImageGenerator generator{cancellation, bytes(png_bytes)};
    testing::ScriptedArtifactStore artifacts;
    adapters::ImageBackend backend{
        generator, artifacts, {std::nullopt, 1024, 1024}};
    auto result = backend.start(backend_request(), cancellation.get_token());
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == backend::BackendErrorKind::cancelled);
    CHECK(artifacts.recorded_calls().empty());
  }

  SECTION("encoded byte limit") {
    const auto encoded = bytes(png_bytes);
    testing::ScriptedImageGenerator generator{{
        {{make_id<domain::ModelId>("image-model"), "draw a blue square",
          std::nullopt},
         backend::GeneratedImage{encoded, "image/png"}},
    }};
    testing::ScriptedArtifactStore artifacts;
    adapters::ImageBackend backend{
        generator, artifacts, {std::nullopt, 1024, encoded.size() - 1}};
    auto result = backend.start(backend_request(), {});
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == backend::BackendErrorKind::protocol);
    CHECK(artifacts.recorded_calls().empty());
  }

  SECTION("decoded byte limit") {
    const auto encoded = bytes(png_bytes);
    testing::ScriptedImageGenerator generator{{
        {{make_id<domain::ModelId>("image-model"), "draw a blue square",
          std::nullopt},
         backend::GeneratedImage{encoded, "image/png"}},
    }};
    testing::ScriptedArtifactStore artifacts;
    adapters::ImageBackendOptions options{std::nullopt, 1024, 1024};
    options.maximum_decoded_bytes = 7;
    adapters::ImageBackend backend{generator, artifacts, options};
    auto result = backend.start(backend_request(), {});
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == backend::BackendErrorKind::protocol);
    CHECK(artifacts.recorded_calls().empty());
  }
}

TEST_CASE("image backend rejects inconsistent artifact-store metadata") {
  const auto encoded = bytes(png_bytes);
  auto forged = metadata("inference", "image/png", encoded.size());
  forged.digest = "not-a-content-digest";
  testing::ScriptedImageGenerator generator{{
      {{make_id<domain::ModelId>("image-model"), "draw a blue square",
        std::nullopt},
       backend::GeneratedImage{encoded, "image/png"}},
  }};
  testing::ScriptedArtifactStore artifacts{{
      {{{make_id<domain::ArtifactId>("image-inference"), "image/png",
         std::nullopt, make_id<domain::InferenceId>("inference"), 2, 1},
        encoded},
       forged},
  }};
  adapters::ImageBackend backend{
      generator, artifacts, {std::nullopt, 1024, 1024}};
  auto result = backend.start(backend_request(), {});
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == backend::BackendErrorKind::protocol);
}

TEST_CASE("filesystem artifact store is content addressed and verifies reads") {
#ifdef _WIN32
  SKIP("POSIX filesystem contract");
#else
  TemporaryDirectory temporary;
  auto store =
      adapters::FilesystemArtifactStore::open(temporary.path() / "artifacts");
  REQUIRE(store);
  const auto content = bytes(png_bytes);
  auto first =
      (*store)->put({make_id<domain::ArtifactId>("first"), "image/png",
                     std::nullopt, make_id<domain::InferenceId>("one"), 2, 1},
                    content);
  REQUIRE(first);
  auto second =
      (*store)->put({make_id<domain::ArtifactId>("second"), "image/png",
                     std::nullopt, make_id<domain::InferenceId>("two"), 2, 1},
                    content);
  REQUIRE(second);
  CHECK(first->digest == second->digest);
  CHECK(first->artifact_id != second->artifact_id);
  auto read = (*store)->get(*first, 1024);
  REQUIRE(read);
  CHECK(read->content == content);

  const std::array<std::byte, 3> abc{std::byte{'a'}, std::byte{'b'},
                                     std::byte{'c'}};
  auto known = (*store)->put({make_id<domain::ArtifactId>("known-digest"),
                              "application/octet-stream", std::nullopt,
                              make_id<domain::InferenceId>("known"),
                              std::nullopt, std::nullopt},
                             abc);
  REQUIRE(known);
  CHECK(known->digest ==
        "sha256:"
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  auto conflicting_producers = (*store)->put(
      {make_id<domain::ArtifactId>("conflicting-producers"),
       "application/octet-stream", make_id<domain::InvocationId>("tool"),
       make_id<domain::InferenceId>("inference"), std::nullopt, std::nullopt},
      abc);
  REQUIRE_FALSE(conflicting_producers);
  CHECK(conflicting_producers.error().code ==
        storage::ArtifactStoreErrorCode::invalid_request);

  const auto hex = first->digest.substr(7);
  const auto blob =
      temporary.path() / "artifacts" / "sha256" / hex.substr(0, 2) / hex;
  REQUIRE(::chmod(blob.c_str(), S_IRUSR | S_IWUSR | S_IRGRP) == 0);
  auto insecure = (*store)->get(*first, 1024);
  REQUIRE_FALSE(insecure);
  CHECK(insecure.error().code ==
        storage::ArtifactStoreErrorCode::permission_denied);

  auto missing = *first;
  missing.digest =
      "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
  auto unavailable = (*store)->get(missing, 1024);
  REQUIRE_FALSE(unavailable);
  CHECK(unavailable.error().code ==
        storage::ArtifactStoreErrorCode::unavailable);
#endif
}

TEST_CASE("existing artifact store open refuses symlinked managed roots") {
#ifdef _WIN32
  SKIP("POSIX filesystem contract");
#else
  TemporaryDirectory temporary;
  const auto real = temporary.path() / "real";
  REQUIRE(adapters::FilesystemArtifactStore::open(real));
  const auto link = temporary.path() / "link";
  std::filesystem::create_directory_symlink(real, link);
  auto opened = adapters::FilesystemArtifactStore::open(
      link, {}, adapters::FilesystemArtifactStoreOpenMode::existing);
  REQUIRE_FALSE(opened);
#endif
}

TEST_CASE("durable image surface emits one replayable artifact reference") {
  TemporaryDirectory temporary;
  auto sessions =
      adapters::SqliteSessionStore::open(temporary.path() / "sessions.sqlite3");
  auto artifacts =
      adapters::FilesystemArtifactStore::open(temporary.path() / "artifacts");
  REQUIRE(sessions);
  REQUIRE(artifacts);
  const auto encoded = bytes(png_bytes);
  testing::ScriptedImageGenerator generator{{
      {{make_id<domain::ModelId>("image-model"), "one blue square",
        std::nullopt},
       backend::GeneratedImage{encoded, "image/png"}},
  }};
  adapters::ImageBackend backend{
      generator, **artifacts, {std::nullopt, 1024, 1024}};
  surfaces::ImageSurface surface{backend, **sessions};

  auto generated =
      surface.generate({"one blue square",
                        make_id<domain::ModelId>("image-model"), std::nullopt});
  REQUIRE(generated);
  CHECK(generated->artifact.media_type == "image/png");
  CHECK(generated->artifact.width == 2);
  CHECK(generated->artifact.height == 1);
  auto replay = (*sessions)->replay_events(generated->session_id);
  REQUIRE(replay);
  CHECK(std::ranges::count_if(*replay, [](const domain::RunEvent& event) {
          return std::holds_alternative<domain::ArtifactCreated>(event.payload);
        }) == 1);
  CHECK(std::ranges::count_if(*replay, [](const domain::RunEvent& event) {
          return std::holds_alternative<domain::ArtifactReferenced>(
              event.payload);
        }) == 1);
  CHECK(std::ranges::count_if(*replay, [](const domain::RunEvent& event) {
          const auto* delta =
              std::get_if<domain::AssistantContentDeltaAdded>(&event.payload);
          return delta != nullptr &&
                 std::holds_alternative<domain::ArtifactReferenceBlock>(
                     delta->delta);
        }) == 1);
  CHECK(generator.recorded_requests().size() == 1);
}

TEST_CASE("TermForge rendering deliberately selects encoded or decoded media") {
  const auto encoded = bytes(png_bytes);
  storage::ArtifactRead artifact{
      metadata("render", "image/png", encoded.size()), encoded};

  SECTION("plain tier receives validated decoded RGBA") {
    termforge::FallbackDriver driver;
    std::string wire;
    driver.set_output(&wire);
    REQUIRE(driver.init());
    auto rendered = adapters::render_image_artifact(
        artifact, driver, termforge::Rect{0, 0, 4, 2});
    REQUIRE(rendered);
    CHECK_FALSE(rendered->encoded_passthrough);
    CHECK_FALSE(wire.empty());
    driver.shutdown();
  }

  SECTION("Kitty tier receives the already validated PNG encoding") {
    termforge::KittyDriver driver;
    std::string wire;
    driver.set_output(&wire);
    REQUIRE(driver.init());
    auto rendered = adapters::render_image_artifact(
        artifact, driver, termforge::Rect{0, 0, 4, 2});
    REQUIRE(rendered);
    CHECK(rendered->encoded_passthrough);
    CHECK_FALSE(wire.empty());
    driver.shutdown();
  }

  SECTION("zero geometry and forged metadata fail before terminal output") {
    termforge::FallbackDriver driver;
    std::string wire;
    driver.set_output(&wire);
    REQUIRE(driver.init());
    auto empty = adapters::render_image_artifact(artifact, driver,
                                                 termforge::Rect{0, 0, 0, 1});
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code ==
          adapters::ImageRenderErrorCode::unsupported_geometry);
    artifact.metadata.media_type = "image/png\x1b[31m";
    auto forged = adapters::render_image_artifact(artifact, driver,
                                                  termforge::Rect{0, 0, 4, 2});
    REQUIRE_FALSE(forged);
    CHECK(forged.error().code ==
          adapters::ImageRenderErrorCode::invalid_artifact);
    CHECK(wire.empty());
    driver.shutdown();
  }
}

} // namespace
