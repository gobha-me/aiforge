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
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/adapters/termforge_image_renderer.hpp>
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
  ImageServer() {
    m_server.Post("/api/v1/image/generate", [this](
                                                const httplib::Request& request,
                                                httplib::Response& response) {
      {
        std::lock_guard lock(m_mutex);
        m_authorization = request.get_header_value("Authorization");
        m_request_body = request.body;
      }
      response.set_content(R"({"id":"unexpected-json","images":["remote"]})",
                           "application/json");
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
