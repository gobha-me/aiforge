#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
#endif

#include <aiforge/adapters/audio_backend.hpp>
#include <aiforge/adapters/filesystem_artifact_store.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/adapters/venice_audio_service.hpp>
#include <aiforge/audio/wav.hpp>
#include <aiforge/surfaces/audio.hpp>
#include <aiforge/testing/scripted_artifact_store.hpp>
#include <aiforge/testing/scripted_audio_service.hpp>
#include <httplib.h>

namespace {

using namespace aiforge;

template <typename IdType>
[[nodiscard]] auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

[[nodiscard]] auto pcm_wav() -> std::vector<std::byte> {
  constexpr std::array<std::uint8_t, 48> bytes{
      'R',  'I',  'F', 'F', 40,   0,    0, 0, 'W', 'A', 'V', 'E',
      'f',  'm',  't', ' ', 16,   0,    0, 0, 1,   0,   1,   0,
      0x40, 0x1f, 0,   0,   0x80, 0x3e, 0, 0, 2,   0,   16,  0,
      'd',  'a',  't', 'a', 4,    0,    0, 0, 0,   0,   0,   0};
  std::vector<std::byte> result;
  result.reserve(bytes.size());
  for (const auto byte : bytes)
    result.push_back(static_cast<std::byte>(byte));
  return result;
}

[[nodiscard]] auto metadata(const std::string& artifact, const std::size_t size,
                            std::optional<domain::InferenceId> inference = {})
    -> domain::ArtifactMetadata {
  return {
      make_id<domain::ArtifactId>(artifact),
      "audio/wav",
      static_cast<std::uint64_t>(size),
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::move(inference)};
}

[[nodiscard]] auto speech_context() -> domain::ConstructedContext {
  return domain::ConstructedContext{
      {{make_id<domain::ContextEntryId>("runtime-entry"),
        domain::ContextEntryKind::instruction,
        domain::InstructionLayer::application_runtime,
        {make_id<domain::MessageId>("runtime-message"),
         domain::Role::system,
         {domain::TextBlock{"synthesize speech"}},
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
         {domain::TextBlock{"hello"}},
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

[[nodiscard]] auto speech_backend_request() -> backend::BackendRequest {
  return {make_id<domain::InferenceId>("inference"),
          make_id<domain::MessageId>("assistant"),
          make_id<domain::ModelId>("tts-model"),
          speech_context(),
          {},
          {}};
}

[[nodiscard]] auto transcription_context(const domain::ArtifactId& artifact)
    -> domain::ConstructedContext {
  auto context = speech_context();
  context.entries.back().message.content = {domain::ArtifactReferenceBlock{
      artifact, std::string{"audio to transcribe"}}};
  return context;
}

[[nodiscard]] auto transcription_backend_request(
    const domain::ArtifactId& artifact) -> backend::BackendRequest {
  return {make_id<domain::InferenceId>("transcription"),
          make_id<domain::MessageId>("assistant"),
          make_id<domain::ModelId>("asr-model"),
          transcription_context(artifact),
          {},
          {}};
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    m_path = std::filesystem::temp_directory_path() /
             ("aiforge-audio-test-" + suffix);
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

class AudioServer final {
 public:
  AudioServer() {
    const auto wav = pcm_wav();
    std::string encoded;
    for (const auto byte : wav)
      encoded.push_back(
          static_cast<char>(std::to_integer<unsigned char>(byte)));
    m_server.Post(
        "/api/v1/audio/speech", [this, encoded](const httplib::Request& request,
                                                httplib::Response& response) {
          {
            std::lock_guard lock(m_mutex);
            m_speech_body = request.body;
            m_authorization = request.get_header_value("Authorization");
          }
          response.set_content(encoded, "audio/wav");
        });
    m_server.Post(
        "/api/v1/audio/transcriptions",
        [this](const httplib::Request& request, httplib::Response& response) {
          {
            std::lock_guard lock(m_mutex);
            const auto file = request.files.find("file");
            if (file != request.files.end()) {
              m_transcription_filename = file->second.filename;
              m_transcription_media_type = file->second.content_type;
              m_transcription_content = file->second.content;
            }
          }
          response.set_content("hello transcript", "text/plain");
        });
    m_port = m_server.bind_to_any_port("127.0.0.1");
    REQUIRE(m_port > 0);
    m_thread = std::jthread([this] { m_server.listen_after_bind(); });
  }

  ~AudioServer() {
    m_server.stop();
    if (m_thread.joinable()) m_thread.join();
  }

  [[nodiscard]] auto base_url() const -> std::string {
    return "http://127.0.0.1:" + std::to_string(m_port) + "/api/v1";
  }
  [[nodiscard]] auto speech_body() const -> std::string {
    std::lock_guard lock(m_mutex);
    return m_speech_body;
  }
  [[nodiscard]] auto transcription_filename() const -> std::string {
    std::lock_guard lock(m_mutex);
    return m_transcription_filename;
  }
  [[nodiscard]] auto transcription_media_type() const -> std::string {
    std::lock_guard lock(m_mutex);
    return m_transcription_media_type;
  }
  [[nodiscard]] auto transcription_content() const -> std::string {
    std::lock_guard lock(m_mutex);
    return m_transcription_content;
  }
  [[nodiscard]] auto authorization() const -> std::string {
    std::lock_guard lock(m_mutex);
    return m_authorization;
  }

 private:
  mutable std::mutex m_mutex;
  httplib::Server m_server;
  std::jthread m_thread;
  int m_port{};
  std::string m_speech_body;
  std::string m_transcription_filename;
  std::string m_transcription_media_type;
  std::string m_transcription_content;
  std::string m_authorization;
};

TEST_CASE("PCM WAV validator fails closed before its smoke case") {
  const auto valid = pcm_wav();

  for (const auto size : {std::size_t{0}, std::size_t{11}, std::size_t{47}}) {
    auto truncated = valid;
    truncated.resize(size);
    CHECK_FALSE(audio::validate_pcm_wav(truncated));
  }

  auto wrong_riff = valid;
  wrong_riff[0] = std::byte{'X'};
  CHECK_FALSE(audio::validate_pcm_wav(wrong_riff));

  auto compressed = valid;
  compressed[20] = std::byte{3};
  auto unsupported = audio::validate_pcm_wav(compressed);
  REQUIRE_FALSE(unsupported);
  CHECK(unsupported.error().code == audio::PcmWavErrorCode::unsupported);

  auto inconsistent_rate = valid;
  inconsistent_rate[28] = std::byte{0};
  CHECK_FALSE(audio::validate_pcm_wav(inconsistent_rate));

  auto parsed = audio::validate_pcm_wav(valid);
  REQUIRE(parsed);
  CHECK(parsed->channels == 1);
  CHECK(parsed->sample_rate == 8000);
  CHECK(parsed->bits_per_sample == 16);
  CHECK(parsed->frames == 2);
}

TEST_CASE("speech backend validates WAV before publishing one artifact") {
  const auto wav = pcm_wav();
  const auto stored = metadata("audio-inference", wav.size(),
                               make_id<domain::InferenceId>("inference"));
  testing::ScriptedAudioService service{{
      {{make_id<domain::ModelId>("tts-model"),
        make_id<domain::VoiceId>("voice"), "hello", "en-US"},
       backend::SynthesizedAudio{wav, "audio/wav"}},
  }};
  testing::ScriptedArtifactStore artifacts{{
      {{{make_id<domain::ArtifactId>("audio-inference"), "audio/wav",
         std::nullopt, make_id<domain::InferenceId>("inference")},
        wav},
       stored},
  }};
  adapters::SpeechBackend backend{service, artifacts,
                                  make_id<domain::VoiceId>("voice"), "en-US"};

  auto stream = backend.start(speech_backend_request(), {});
  REQUIRE(stream);
  REQUIRE((*stream)->next({}));
  auto produced = (*stream)->next({});
  REQUIRE(produced);
  REQUIRE(*produced);
  CHECK(std::get<backend::ArtifactProduced>(**produced).artifact == stored);
  CHECK(std::get<backend::ArtifactProduced>(**produced).label ==
        "synthesized speech");
  CHECK(service.recorded_synthesis().size() == 1);
  CHECK(artifacts.remaining_exchanges() == 0);

  auto malformed = wav;
  malformed.resize(20);
  testing::ScriptedAudioService invalid_service{{
      {{make_id<domain::ModelId>("tts-model"),
        make_id<domain::VoiceId>("voice"), "hello", std::nullopt},
       backend::SynthesizedAudio{malformed, "audio/wav"}},
  }};
  testing::ScriptedArtifactStore rejecting_store;
  adapters::SpeechBackend invalid_backend{invalid_service, rejecting_store,
                                          make_id<domain::VoiceId>("voice")};
  auto rejected = invalid_backend.start(speech_backend_request(), {});
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().kind == backend::BackendErrorKind::protocol);
  CHECK(rejecting_store.recorded_calls().empty());

  testing::ScriptedAudioService unused_service;
  adapters::SpeechBackend invalid_language_backend{
      unused_service, rejecting_store, make_id<domain::VoiceId>("voice"),
      "-en"};
  auto invalid_language =
      invalid_language_backend.start(speech_backend_request(), {});
  REQUIRE_FALSE(invalid_language);
  CHECK(invalid_language.error().kind ==
        backend::BackendErrorKind::request_rejected);
  CHECK(unused_service.recorded_synthesis().empty());
}

TEST_CASE("transcription backend reads exact artifact and rejects controls") {
  const auto wav = pcm_wav();
  const auto input = metadata("input", wav.size());
  testing::ScriptedArtifactStore artifacts{
      {}, {{input, 32U * 1024U * 1024U, storage::ArtifactRead{input, wav}}}};
  testing::ScriptedAudioService service{
      {},
      {{{make_id<domain::ModelId>("asr-model"), wav, "audio/wav", "en"},
        backend::AudioTranscription{"hello transcript"}}}};
  adapters::TranscriptionBackend backend{service, artifacts, input, "en"};
  auto stream =
      backend.start(transcription_backend_request(input.artifact_id), {});
  REQUIRE(stream);
  REQUIRE((*stream)->next({}));
  auto delta = (*stream)->next({});
  REQUIRE(delta);
  REQUIRE(*delta);
  CHECK(std::get<domain::TextBlock>(
            std::get<backend::ContentDelta>(**delta).delta)
            .text == "hello transcript");

  testing::ScriptedArtifactStore unsafe_artifacts{
      {}, {{input, 32U * 1024U * 1024U, storage::ArtifactRead{input, wav}}}};
  testing::ScriptedAudioService unsafe_service{
      {},
      {{{make_id<domain::ModelId>("asr-model"), wav, "audio/wav", std::nullopt},
        backend::AudioTranscription{"hello\x1b[31m"}}}};
  adapters::TranscriptionBackend unsafe_backend{unsafe_service,
                                                unsafe_artifacts, input};
  auto rejected = unsafe_backend.start(
      transcription_backend_request(input.artifact_id), {});
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().kind == backend::BackendErrorKind::protocol);
}

TEST_CASE("durable audio surfaces preserve output and input run truth") {
  TemporaryDirectory temporary;
  auto sessions =
      adapters::SqliteSessionStore::open(temporary.path() / "sessions.sqlite3");
  auto artifacts =
      adapters::FilesystemArtifactStore::open(temporary.path() / "artifacts");
  REQUIRE(sessions);
  REQUIRE(artifacts);
  const auto wav = pcm_wav();

  testing::ScriptedAudioService speech_service{{
      {{make_id<domain::ModelId>("tts-model"),
        make_id<domain::VoiceId>("voice"), "hello", std::nullopt},
       backend::SynthesizedAudio{wav, "audio/wav"}},
  }};
  adapters::SpeechBackend speech_backend{speech_service, **artifacts,
                                         make_id<domain::VoiceId>("voice")};
  surfaces::SpeechSurface speech{speech_backend, **sessions};
  auto synthesized = speech.synthesize(
      {"hello", make_id<domain::ModelId>("tts-model"), std::nullopt});
  REQUIRE(synthesized);
  auto invalid_utf8 = std::string{"invalid"};
  invalid_utf8.push_back(static_cast<char>(0xC0));
  auto rejected =
      speech.synthesize({std::move(invalid_utf8),
                         make_id<domain::ModelId>("tts-model"), std::nullopt});
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == surfaces::AudioErrorCode::invalid_input);
  CHECK(speech_service.recorded_synthesis().size() == 1);
  auto speech_events = (*sessions)->replay_events(synthesized->session_id);
  REQUIRE(speech_events);
  CHECK(std::ranges::count_if(*speech_events, [](const auto& event) {
          return std::holds_alternative<domain::ArtifactCreated>(event.payload);
        }) == 1);

  auto stored_input = (*artifacts)
                          ->put({make_id<domain::ArtifactId>("input"),
                                 "audio/wav", std::nullopt},
                                wav);
  REQUIRE(stored_input);
  testing::ScriptedAudioService transcription_service{
      {},
      {{{make_id<domain::ModelId>("asr-model"), wav, "audio/wav", std::nullopt},
        backend::AudioTranscription{"hello transcript"}}}};
  adapters::TranscriptionBackend transcription_backend{
      transcription_service, **artifacts, *stored_input};
  surfaces::TranscriptionSurface transcription{transcription_backend,
                                               **sessions};
  auto transcribed = transcription.transcribe(
      {*stored_input, make_id<domain::ModelId>("asr-model"), std::nullopt});
  REQUIRE(transcribed);
  CHECK(transcribed->text == "hello transcript");
  auto transcript_events = (*sessions)->replay_events(transcribed->session_id);
  REQUIRE(transcript_events);
  CHECK(std::ranges::count_if(*transcript_events, [](const auto& event) {
          return std::holds_alternative<domain::ArtifactCreated>(event.payload);
        }) == 1);
  CHECK(std::ranges::count_if(*transcript_events, [](const auto& event) {
          return std::holds_alternative<domain::ArtifactReferenced>(
              event.payload);
        }) == 1);
  CHECK(transcription_service.recorded_transcription().size() == 1);
}

TEST_CASE("Venice audio adapter uses WAV and a fixed upload filename") {
  AudioServer server;
  constexpr std::string_view api_key = "test-audio-secret";
  auto secret = credentials::make_secret(std::string{api_key});
  REQUIRE(secret);
  adapters::VeniceAudioService service{
      std::move(*secret),
      {server.base_url(), std::chrono::seconds{1}, std::chrono::seconds{1},
       std::chrono::seconds{1}}};
  auto synthesized =
      service.synthesize({make_id<domain::ModelId>("tts-model"),
                          make_id<domain::VoiceId>("voice"), "hello", "en-US"});
  REQUIRE(synthesized);
  CHECK(synthesized->media_type == "audio/wav");
  CHECK(server.authorization() == "Bearer " + std::string{api_key});
  CHECK(server.speech_body().find("\"response_format\":\"wav\"") !=
        std::string::npos);
  CHECK(server.speech_body().find("\"voice\":\"voice\"") != std::string::npos);

  auto transcribed = service.transcribe(
      {make_id<domain::ModelId>("asr-model"), pcm_wav(), "audio/wav", "en"});
  REQUIRE(transcribed);
  CHECK(transcribed->text == "hello transcript");
  CHECK(server.transcription_filename() == "audio.wav");
  CHECK(server.transcription_media_type() == "audio/wav");
  CHECK(server.transcription_content().size() == pcm_wav().size());
  CHECK(server.transcription_content().find(api_key) == std::string::npos);
}

} // namespace
