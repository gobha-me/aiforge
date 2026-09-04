#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <aiforge/adapters/ask_user_dialog.hpp>
#include <aiforge/adapters/filesystem_persona_source.hpp>
#include <aiforge/adapters/filesystem_user_global_instruction_source.hpp>
#include <aiforge/adapters/interactive_chat_app.hpp>
#include <aiforge/adapters/json_model_catalog_cache.hpp>
#include <aiforge/adapters/model_picker_dialog.hpp>
#include <aiforge/adapters/process_credentials.hpp>
#include <aiforge/adapters/process_provenance.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/adapters/termforge_run_bridge.hpp>
#include <aiforge/adapters/venice_backend.hpp>
#include <aiforge/adapters/venice_generation_options.hpp>
#include <aiforge/adapters/venice_model_catalog_source.hpp>
#include <aiforge/runtime/ask_user_tool.hpp>
#include <aiforge/testing/scripted_backend.hpp>

namespace {

using namespace std::chrono_literals;
using namespace aiforge;

template <typename IdType> auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto secret(std::string value) -> credentials::Secret {
  return credentials::make_secret(std::move(value)).value();
}

auto type_text(adapters::ModelPickerDialog& dialog, const std::string_view text)
    -> void {
  for (const char character : text) {
    REQUIRE(dialog.on_event(termforge::KeyEvent{
        termforge::Key::Char, static_cast<char32_t>(character), false, false,
        false, termforge::KeyAction::Press}));
  }
}

auto screen_text(const termforge::Screen& screen) -> std::string {
  std::string result;
  for (int row{}; row < screen.rows(); ++row) {
    for (int column{}; column < screen.cols(); ++column) {
      const auto value = screen.text_at(column, row);
      result += value.empty() ? " " : std::string{value};
    }
    result += '\n';
  }
  return result;
}

auto context(domain::ContentBlock content = domain::TextBlock{"hello"})
    -> domain::ConstructedContext {
  return domain::ConstructedContext{
      {domain::ContextEntry{make_id<domain::ContextEntryId>("context"),
                            domain::ContextEntryKind::conversation,
                            std::nullopt,
                            domain::Message{make_id<domain::MessageId>("user"),
                                            domain::Role::user,
                                            {std::move(content)},
                                            std::nullopt},
                            {make_id<domain::ContextSourceId>("source"),
                             std::nullopt, std::nullopt},
                            0,
                            1,
                            1}},
      {{make_id<domain::ContextEntryId>("context"),
        domain::ContextDecision::admitted, std::nullopt}},
      {4096, 512, 0},
      1};
}

auto request(domain::ConstructedContext built = context())
    -> backend::BackendRequest {
  return backend::BackendRequest{make_id<domain::InferenceId>("inference"),
                                 make_id<domain::MessageId>("assistant"),
                                 make_id<domain::ModelId>("test-model"),
                                 std::move(built),
                                 {},
                                 {0.5, 64, 7, {}, {}}};
}

class LocalServer final {
 public:
  explicit LocalServer(std::optional<std::string> echoed_error = std::nullopt,
                       const bool duplicate_cost = false,
                       std::string cost_value = "0.0645375",
                       const bool duplicate_finish = false,
                       const bool omit_finish = false,
                       const bool reasoning_state = false)
      : m_echoed_error(std::move(echoed_error)),
        m_duplicate_cost(duplicate_cost), m_duplicate_finish(duplicate_finish),
        m_omit_finish(omit_finish), m_reasoning_state(reasoning_state),
        m_cost_value(std::move(cost_value)) {
    m_server.Get("/api/v1/models", [this](const httplib::Request& request,
                                          httplib::Response& response) {
      {
        std::lock_guard lock(m_mutex);
        m_model_authorization = request.get_header_value("Authorization");
      }
      response.set_content(
          R"({"data":[{"id":"test-model","type":"text","context_length":8192,"model_spec":{"availableContextTokens":8192,"maxCompletionTokens":1024,"offline":false,"capabilities":{"supportsWebSearch":true,"supportsReasoning":null},"pricing":{"input":{"usd":1.42,"diem":2.5},"output":{"usd":2.83},"cache_input":{"usd":0.23}}}}]})",
          "application/json");
    });
    m_server.Post(
        "/api/v1/chat/completions",
        [this](const httplib::Request& request, httplib::Response& response) {
          {
            std::lock_guard lock(m_mutex);
            m_body = request.body;
            m_authorization = request.get_header_value("Authorization");
          }
          if (m_echoed_error) {
            response.status = 401;
            response.set_content("provider echoed " + *m_echoed_error,
                                 "text/plain");
            return;
          }
          std::string stream =
              "data: "
              "{\"id\":\"response\",\"choices\":[{\"delta\":{\"role\":"
              "\"assistant\"";
          if (m_reasoning_state) {
            stream +=
                ",\"reasoning_content\":\"plan\",\"reasoning_details\":[{"
                "\"type\":\"reasoning.text\",\"text\":\"opaque\"}],"
                "\"thought_signature\":\"\",\"tool_calls\":[{"
                "\"index\":0,\"id\":\"lookup-call\",\"type\":\"function\","
                "\"thought_signature\":\"\",\"function\":{"
                "\"name\":\"lookup\",\"arguments\":\"{}\"}}]";
          }
          stream += "}}]}\n\n";
          if (m_reasoning_state) {
            stream += "data: "
                      "{\"id\":\"response\",\"choices\":[{\"delta\":{"
                      "\"thought_signature\":\"signature\",\"tool_calls\":[{"
                      "\"index\":0,\"thought_signature\":\"tool-signature\","
                      "\"function\":{\"arguments\":\"\"}}]}}]}\n\n";
          }
          stream += "data: "
                    "{\"id\":\"response\",\"choices\":[{\"delta\":{\"content\":"
                    "\"hello\"}}]}\n\n";
          if (!m_omit_finish) {
            stream +=
                "data: "
                "{\"id\":\"response\",\"choices\":[{\"delta\":{},\"finish_"
                "reason\":\"stop\"}]}\n\n";
            if (m_duplicate_finish) {
              stream +=
                  "data: "
                  "{\"id\":\"response\",\"choices\":[{\"delta\":{},\"finish_"
                  "reason\":\"stop\"}]}\n\n";
            }
          }
          stream += "data: "
                    "{\"id\":\"response\",\"choices\":[],"
                    "\"cost\":{\"usd\":0,\"diem\":" +
                    m_cost_value + "}}\n\n";
          if (m_duplicate_cost) {
            stream += "data: "
                      "{\"id\":\"response\",\"choices\":[],"
                      "\"cost\":{\"diem\":0.01}}\n\n";
          }
          stream += "data: "
                    "{\"id\":\"response\",\"choices\":[],\"usage\":{\"prompt_"
                    "tokens\":2,\"completion_"
                    "tokens\":1,\"total_tokens\":3}}\n\n"
                    "data: [DONE]\n\n";
          response.set_content(std::move(stream), "text/event-stream");
        });
    m_port = m_server.bind_to_any_port("127.0.0.1");
    REQUIRE(m_port > 0);
    m_thread = std::jthread([this] { m_server.listen_after_bind(); });
    m_server.wait_until_ready();
    REQUIRE(m_server.is_running());
  }

  ~LocalServer() {
    m_server.stop();
    if (m_thread.joinable()) m_thread.join();
  }

  auto base_url() const -> std::string {
    return "http://127.0.0.1:" + std::to_string(m_port) + "/api/v1";
  }

  auto body() -> std::string {
    std::lock_guard lock(m_mutex);
    return m_body;
  }

  auto authorization() -> std::string {
    std::lock_guard lock(m_mutex);
    return m_authorization;
  }

  auto model_authorization() -> std::string {
    std::lock_guard lock(m_mutex);
    return m_model_authorization;
  }

 private:
  httplib::Server m_server;
  int m_port{};
  std::jthread m_thread;
  std::mutex m_mutex;
  std::string m_body;
  std::string m_authorization;
  std::string m_model_authorization;
  std::optional<std::string> m_echoed_error;
  bool m_duplicate_cost{};
  bool m_duplicate_finish{};
  bool m_omit_finish{};
  bool m_reasoning_state{};
  std::string m_cost_value;
};

struct CharacterReply {
  int status{200};
  std::string body;
};

class CharacterServer final {
 public:
  explicit CharacterServer(std::vector<CharacterReply> pages,
                           std::map<std::string, CharacterReply> lookups = {},
                           const bool repeat_last_page = false,
                           std::function<void(std::size_t)> on_list = {})
      : m_pages(std::move(pages)), m_lookups(std::move(lookups)),
        m_repeat_last_page(repeat_last_page), m_on_list(std::move(on_list)) {
    m_server.Get("/api/v1/characters", [this](const httplib::Request& request,
                                              httplib::Response& response) {
      std::size_t index{};
      {
        std::lock_guard lock(m_mutex);
        index = m_targets.size();
        m_targets.push_back(request.target);
        m_authorizations.push_back(request.get_header_value("Authorization"));
      }
      CharacterReply reply{500, R"({"error":"unexpected page"})"};
      if (index < m_pages.size()) {
        reply = m_pages[index];
      } else if (m_repeat_last_page && !m_pages.empty()) {
        reply = m_pages.back();
      }
      response.status = reply.status;
      response.set_content(std::move(reply.body), "application/json");
      if (m_on_list) m_on_list(index);
    });
    m_server.Get(
        R"(/api/v1/characters/(.+))",
        [this](const httplib::Request& request, httplib::Response& response) {
          const auto slug = request.matches[1].str();
          {
            std::lock_guard lock(m_mutex);
            m_targets.push_back(request.target);
            m_authorizations.push_back(
                request.get_header_value("Authorization"));
          }
          const auto found = m_lookups.find(slug);
          const auto reply = found == m_lookups.end()
                                 ? CharacterReply{404, R"({"error":"missing"})"}
                                 : found->second;
          response.status = reply.status;
          response.set_content(reply.body, "application/json");
        });
    m_port = m_server.bind_to_any_port("127.0.0.1");
    REQUIRE(m_port > 0);
    m_thread = std::jthread([this] { m_server.listen_after_bind(); });
    m_server.wait_until_ready();
    REQUIRE(m_server.is_running());
  }

  ~CharacterServer() {
    m_server.stop();
    if (m_thread.joinable()) m_thread.join();
  }

  [[nodiscard]] auto base_url() const -> std::string {
    return "http://127.0.0.1:" + std::to_string(m_port) + "/api/v1";
  }

  [[nodiscard]] auto targets() -> std::vector<std::string> {
    std::lock_guard lock(m_mutex);
    return m_targets;
  }

  [[nodiscard]] auto authorizations() -> std::vector<std::string> {
    std::lock_guard lock(m_mutex);
    return m_authorizations;
  }

 private:
  httplib::Server m_server;
  int m_port{};
  std::jthread m_thread;
  std::mutex m_mutex;
  std::vector<CharacterReply> m_pages;
  std::map<std::string, CharacterReply> m_lookups;
  bool m_repeat_last_page{};
  std::function<void(std::size_t)> m_on_list;
  std::vector<std::string> m_targets;
  std::vector<std::string> m_authorizations;
};

[[nodiscard]] auto character_json(
    std::string slug, std::optional<std::string> model = "test-model")
    -> nlohmann::json {
  auto result = nlohmann::json{{"slug", std::move(slug)},
                               {"name", "Character"},
                               {"description", "Helpful character"},
                               {"adult", false},
                               {"featured", true},
                               {"webEnabled", false},
                               {"tags", {"helpful"}},
                               {"author", "must-not-cross"},
                               {"photoUrl", "https://example.invalid/photo"},
                               {"stats", {{"imports", 42}}}};
  if (model) result["modelId"] = *model;
  return result;
}

[[nodiscard]] auto character_page(const std::vector<nlohmann::json>& entries)
    -> CharacterReply {
  return {200, nlohmann::json{{"data", entries}, {"object", "list"}}.dump()};
}

TEST_CASE("local adapter server stops without receiving a request",
          "[adapter][venice][lifetime]") {
  for (int iteration = 0; iteration < 64; ++iteration) {
    CAPTURE(iteration);
    LocalServer server;
  }
}

class TestApp final : public termforge::App {
 private:
  auto on_render(termforge::Screen&) -> void override {}
};

class TempDirectory final {
 public:
  explicit TempDirectory(std::string prefix) {
    auto pattern =
        (std::filesystem::temp_directory_path() / (prefix + "-XXXXXX"))
            .string();
    pattern.push_back('\0');
    const auto* created = ::mkdtemp(pattern.data());
    REQUIRE(created != nullptr);
    m_path = created;
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
  }

  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return m_path;
  }

 private:
  std::filesystem::path m_path;
};

class EnvironmentGuard final {
 public:
  explicit EnvironmentGuard(std::string name) : m_name(std::move(name)) {
    if (const auto* value = std::getenv(m_name.c_str())) m_value = value;
  }

  ~EnvironmentGuard() {
    if (m_value) {
      static_cast<void>(::setenv(m_name.c_str(), m_value->c_str(), 1));
    } else {
      static_cast<void>(::unsetenv(m_name.c_str()));
    }
  }

 private:
  std::string m_name;
  std::optional<std::string> m_value;
};

auto write_file(const std::filesystem::path& path, const std::string& bytes)
    -> void {
  std::ofstream output{path, std::ios::binary};
  REQUIRE(output);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  REQUIRE(output);
}

auto read_file(const std::filesystem::path& path) -> std::string {
  std::ifstream input{path, std::ios::binary};
  REQUIRE(input);
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("Venice model discovery is public and maps neutral context metadata",
          "[adapter][models]") {
  LocalServer server;
  adapters::VeniceModelCatalogOptions options;
  options.base_url = server.base_url();
  adapters::VeniceModelCatalogSource source{std::move(options)};
  const auto catalog = source.fetch({});
  REQUIRE(catalog);
  REQUIRE(catalog->entries.size() == 1);
  REQUIRE(catalog->entries.front().id ==
          make_id<domain::ModelId>("test-model"));
  REQUIRE(catalog->entries.front().type == "text");
  REQUIRE(catalog->entries.front().context_window_tokens == 8192);
  REQUIRE(catalog->entries.front().maximum_output_tokens == 1024);
  REQUIRE(catalog->source_id == "venice.models");
  REQUIRE(catalog->entries.front().pricing->base.input->usd->to_string() ==
          "1.42");
  REQUIRE(catalog->entries.front().pricing->base.input->diem->to_string() ==
          "2.5");
  REQUIRE(
      catalog->entries.front().pricing->base.cache_input->usd->to_string() ==
      "0.23");
  REQUIRE_FALSE(catalog->entries.front().pricing->base.cache_write);
  REQUIRE(server.model_authorization().empty());
}

TEST_CASE("Venice character discovery uses authenticated returned pagination",
          "[adapter][venice][characters]") {
  std::vector<nlohmann::json> first_page;
  for (int index = 0; index < 100; ++index) {
    first_page.push_back(character_json("character-" + std::to_string(index)));
  }
  CharacterServer server{
      {character_page(first_page), character_page({character_json("last")})}};
  adapters::VeniceBackend source{secret("catalog-secret"),
                                 {server.base_url(), 1s, 1s, 1s, 8}};

  auto limits = backend::ProviderCharacterLimits{};
  limits.maximum_entries = 200;
  const auto catalog = source.list(limits);
  REQUIRE(catalog);
  REQUIRE(catalog->source_id == "venice.characters");
  REQUIRE(catalog->entries.size() == 101);
  REQUIRE(catalog->entries.front().id ==
          make_id<domain::ProviderCharacterId>("character-0"));
  REQUIRE(catalog->entries.back().id ==
          make_id<domain::ProviderCharacterId>("last"));
  REQUIRE(catalog->entries.front().model_id ==
          make_id<domain::ModelId>("test-model"));
  REQUIRE(catalog->entries.front().name == "Character");
  REQUIRE(catalog->entries.front().description == "Helpful character");
  REQUIRE(catalog->entries.front().featured == true);
  REQUIRE(catalog->entries.front().web_enabled == false);
  REQUIRE(catalog->entries.front().tags == std::vector<std::string>{"helpful"});

  const auto targets = server.targets();
  REQUIRE(targets ==
          std::vector<std::string>{
              "/api/v1/characters?limit=100&offset=0&isAdult=false",
              "/api/v1/characters?limit=100&offset=100&isAdult=false"});
  REQUIRE(server.authorizations() ==
          std::vector<std::string>{"Bearer catalog-secret",
                                   "Bearer catalog-secret"});
}

TEST_CASE("Venice character pagination offset advancement is checked",
          "[adapter][venice][characters][failure]") {
  const auto maximum = std::numeric_limits<int>::max();
  REQUIRE(adapters::detail::advance_venice_character_offset(maximum - 1, 1) ==
          maximum);
  REQUIRE_FALSE(adapters::detail::advance_venice_character_offset(maximum, 1));
  REQUIRE_FALSE(adapters::detail::advance_venice_character_offset(
      0, static_cast<std::size_t>(maximum) + 1U));
  REQUIRE_FALSE(adapters::detail::advance_venice_character_offset(-1, 1));
}

TEST_CASE("Venice character discovery fails closed without partial results",
          "[adapter][venice][characters][failure]") {
  SECTION("invalid limits reject before transport") {
    CharacterServer server{{character_page({})}};
    adapters::VeniceBackend source{secret("secret"),
                                   {server.base_url(), 1s, 1s, 1s, 8}};
    auto limits = backend::ProviderCharacterLimits{};
    limits.maximum_response_bytes = 0;
    const auto catalog = source.list(limits);
    REQUIRE_FALSE(catalog);
    REQUIRE(catalog.error().code ==
            backend::ProviderCharacterErrorCode::invalid_request);
    REQUIRE(server.targets().empty());
  }

  SECTION("unsafe text and duplicate tags are invalid data") {
    auto unsafe = character_json("unsafe");
    unsafe["description"] = "hidden\xE2\x80\xAEtext";
    auto duplicate_tags = character_json("duplicate-tags");
    duplicate_tags["tags"] = {"same", "same"};
    for (const auto& entry : {unsafe, duplicate_tags}) {
      CharacterServer server{{character_page({entry})}};
      adapters::VeniceBackend source{secret("secret"),
                                     {server.base_url(), 1s, 1s, 1s, 8}};
      const auto catalog = source.list();
      REQUIRE_FALSE(catalog);
      REQUIRE(catalog.error().code ==
              backend::ProviderCharacterErrorCode::invalid_data);
    }
  }

  SECTION("dropped unusable entries are partial data and fail closed") {
    std::vector<nlohmann::json> entries;
    for (int index = 0; index < 99; ++index) {
      entries.push_back(character_json("entry-" + std::to_string(index)));
    }
    entries.emplace_back(nlohmann::json::object());
    CharacterServer server{{character_page(entries)}};
    adapters::VeniceBackend source{secret("secret"),
                                   {server.base_url(), 1s, 1s, 1s, 8}};
    const auto catalog = source.list();
    REQUIRE_FALSE(catalog);
    REQUIRE(catalog.error().code ==
            backend::ProviderCharacterErrorCode::invalid_data);
    REQUIRE(server.targets().size() == 1);
  }

  SECTION("provider slugs must use the shared conservative grammar") {
    const std::array slugs{std::string{"has space"}, std::string{"has\"quote"},
                           std::string{"has\\slash"},
                           std::string{"has\xE2\x80\xAE"
                                       "format"}};
    for (const auto& slug : slugs) {
      CharacterServer server{{character_page({character_json(slug)})}};
      adapters::VeniceBackend source{secret("secret"),
                                     {server.base_url(), 1s, 1s, 1s, 8}};
      const auto catalog = source.list();
      REQUIRE_FALSE(catalog);
      REQUIRE(catalog.error().code ==
              backend::ProviderCharacterErrorCode::invalid_data);
    }
  }

  SECTION("wrong-typed mapped fields are invalid data") {
    const std::array fields{"name",     "description", "modelId", "adult",
                            "featured", "webEnabled",  "tags"};
    for (const auto* field : fields) {
      auto entry = character_json(std::string{"wrong-"} + field);
      if (field == std::string_view{"tags"}) {
        entry[field] = nlohmann::json::array({"valid", 42});
      } else if (field == std::string_view{"adult"} ||
                 field == std::string_view{"featured"} ||
                 field == std::string_view{"webEnabled"}) {
        entry[field] = "true";
      } else {
        entry[field] = nlohmann::json::array();
      }
      CharacterServer server{{character_page({entry})}};
      adapters::VeniceBackend source{secret("secret"),
                                     {server.base_url(), 1s, 1s, 1s, 8}};
      const auto catalog = source.list();
      REQUIRE_FALSE(catalog);
      REQUIRE(catalog.error().code ==
              backend::ProviderCharacterErrorCode::invalid_data);
    }
  }

  SECTION("adult and invalid-model entries are rejected") {
    auto adult = character_json("adult");
    adult["adult"] = true;
    auto invalid_model = character_json("invalid-model");
    invalid_model["modelId"] = "bad\nmodel";
    for (const auto& entry : {adult, invalid_model}) {
      CharacterServer server{{character_page({entry})}};
      adapters::VeniceBackend source{secret("secret"),
                                     {server.base_url(), 1s, 1s, 1s, 8}};
      const auto catalog = source.list();
      REQUIRE_FALSE(catalog);
      REQUIRE(catalog.error().code ==
              backend::ProviderCharacterErrorCode::invalid_data);
    }
  }

  SECTION("missing and null adult classification are rejected") {
    auto missing = character_json("missing-adult");
    missing.erase("adult");
    auto null = character_json("null-adult");
    null["adult"] = nullptr;
    for (const auto& entry : {missing, null}) {
      CharacterServer server{{character_page({entry})}};
      adapters::VeniceBackend source{secret("secret"),
                                     {server.base_url(), 1s, 1s, 1s, 8}};
      const auto catalog = source.list();
      REQUIRE_FALSE(catalog);
      REQUIRE(catalog.error().code ==
              backend::ProviderCharacterErrorCode::invalid_data);
    }
  }

  SECTION("count and cumulative byte limits reject the whole catalog") {
    for (const bool count_limit : {true, false}) {
      std::vector<nlohmann::json> entries;
      const auto count = count_limit ? 100 : 1;
      for (int index = 0; index < count; ++index) {
        entries.push_back(character_json("entry-" + std::to_string(index)));
      }
      CharacterServer server{{character_page(entries)}};
      adapters::VeniceBackend source{secret("secret"),
                                     {server.base_url(), 1s, 1s, 1s, 8}};
      auto limits = backend::ProviderCharacterLimits{};
      if (count_limit) {
        limits.maximum_entries = 99;
      } else {
        limits.maximum_total_text_bytes = 40;
      }
      const auto catalog = source.list(limits);
      REQUIRE_FALSE(catalog);
      REQUIRE(catalog.error().code ==
              backend::ProviderCharacterErrorCode::too_large);
    }
  }

  SECTION("an endless full page is bounded by returned count") {
    std::vector<nlohmann::json> entries;
    for (int index = 0; index < 100; ++index) {
      entries.push_back(character_json("entry-" + std::to_string(index)));
    }
    CharacterServer server{{character_page(entries)}, {}, true};
    adapters::VeniceBackend source{secret("secret"),
                                   {server.base_url(), 1s, 1s, 1s, 8}};
    auto limits = backend::ProviderCharacterLimits{};
    limits.maximum_entries = 100;
    const auto catalog = source.list(limits);
    REQUIRE_FALSE(catalog);
    REQUIRE(catalog.error().code ==
            backend::ProviderCharacterErrorCode::too_large);
    REQUIRE(server.targets().size() == 2);
  }

  SECTION("cross-page duplicate identifiers reject the whole catalog") {
    std::vector<nlohmann::json> first_page;
    for (int index = 0; index < 100; ++index) {
      first_page.push_back(character_json("entry-" + std::to_string(index)));
    }
    CharacterServer server{{character_page(first_page),
                            character_page({character_json("entry-0")})}};
    adapters::VeniceBackend source{secret("secret"),
                                   {server.base_url(), 1s, 1s, 1s, 8}};
    auto limits = backend::ProviderCharacterLimits{};
    limits.maximum_entries = 101;
    const auto catalog = source.list(limits);
    REQUIRE_FALSE(catalog);
    REQUIRE(catalog.error().code ==
            backend::ProviderCharacterErrorCode::invalid_data);
  }

  SECTION("second-page transport failure never returns the first page") {
    std::vector<nlohmann::json> first_page;
    for (int index = 0; index < 100; ++index) {
      first_page.push_back(character_json("entry-" + std::to_string(index)));
    }
    CharacterServer server{
        {character_page(first_page), {503, R"({"error":"down"})"}}};
    adapters::VeniceBackend source{secret("secret"),
                                   {server.base_url(), 1s, 1s, 1s, 8}};
    auto limits = backend::ProviderCharacterLimits{};
    limits.maximum_entries = 200;
    const auto catalog = source.list(limits);
    REQUIRE_FALSE(catalog);
    REQUIRE(catalog.error().code ==
            backend::ProviderCharacterErrorCode::unavailable);
    REQUIRE(server.targets().size() == 2);
  }

  SECTION("cancellation between full pages returns no catalog") {
    std::stop_source stop;
    std::vector<nlohmann::json> entries;
    for (int index = 0; index < 100; ++index) {
      entries.push_back(character_json("entry-" + std::to_string(index)));
    }
    CharacterServer server{{character_page(entries)}, {}, true, [&](auto) {
                             stop.request_stop();
                           }};
    adapters::VeniceBackend source{secret("secret"),
                                   {server.base_url(), 1s, 1s, 1s, 8}};
    auto limits = backend::ProviderCharacterLimits{};
    limits.maximum_entries = 200;
    const auto catalog = source.list(limits, stop.get_token());
    REQUIRE_FALSE(catalog);
    REQUIRE(catalog.error().code ==
            backend::ProviderCharacterErrorCode::cancelled);
    REQUIRE(server.targets().size() == 1);
  }

  SECTION("oversized response body is a redacted typed size error") {
    CharacterServer server{{character_page({character_json("too-large")})}};
    adapters::VeniceBackend source{secret("body-secret"),
                                   {server.base_url(), 1s, 1s, 1s, 8}};
    auto limits = backend::ProviderCharacterLimits{};
    limits.maximum_response_bytes = 32;
    const auto catalog = source.list(limits);
    REQUIRE_FALSE(catalog);
    REQUIRE(catalog.error().code ==
            backend::ProviderCharacterErrorCode::too_large);
    REQUIRE(catalog.error().message.find("body-secret") == std::string::npos);
  }

  SECTION("authentication unavailable and malformed data remain distinct") {
    const std::array cases{
        std::pair{CharacterReply{401, R"({"error":"secret"})"},
                  backend::ProviderCharacterErrorCode::authentication},
        std::pair{CharacterReply{503, R"({"error":"down"})"},
                  backend::ProviderCharacterErrorCode::unavailable},
        std::pair{CharacterReply{200, R"({"data":{}})"},
                  backend::ProviderCharacterErrorCode::invalid_data}};
    for (const auto& [reply, expected] : cases) {
      CharacterServer server{{reply}};
      adapters::VeniceBackend source{secret("secret"),
                                     {server.base_url(), 1s, 1s, 1s, 8}};
      const auto catalog = source.list();
      REQUIRE_FALSE(catalog);
      REQUIRE(catalog.error().code == expected);
      REQUIRE(catalog.error().message.find("secret") == std::string::npos);
    }
  }
}

TEST_CASE("Venice character lookup revalidates exact neutral identity",
          "[adapter][venice][characters][lookup]") {
  const auto envelope = [](nlohmann::json character) {
    return CharacterReply{200, nlohmann::json{{"data", std::move(character)},
                                              {"object", "character"}}
                                   .dump()};
  };
  CharacterServer server{
      {},
      {{"chosen", envelope(character_json("chosen"))},
       {"mismatch", envelope(character_json("different"))},
       {"wrong-slug",
        envelope(nlohmann::json{{"slug", 42}, {"adult", false}})}}};
  adapters::VeniceBackend source{secret("lookup-secret"),
                                 {server.base_url(), 1s, 1s, 1s, 8}};

  const auto chosen =
      source.lookup(make_id<domain::ProviderCharacterId>("chosen"));
  REQUIRE(chosen);
  REQUIRE(chosen->id == make_id<domain::ProviderCharacterId>("chosen"));
  REQUIRE(chosen->model_id == make_id<domain::ModelId>("test-model"));

  const auto mismatch =
      source.lookup(make_id<domain::ProviderCharacterId>("mismatch"));
  REQUIRE_FALSE(mismatch);
  REQUIRE(mismatch.error().code ==
          backend::ProviderCharacterErrorCode::invalid_data);

  const auto wrong_slug =
      source.lookup(make_id<domain::ProviderCharacterId>("wrong-slug"));
  REQUIRE_FALSE(wrong_slug);
  REQUIRE(wrong_slug.error().code ==
          backend::ProviderCharacterErrorCode::invalid_data);

  const auto missing =
      source.lookup(make_id<domain::ProviderCharacterId>("missing"));
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().code ==
          backend::ProviderCharacterErrorCode::not_found);
  REQUIRE(
      server.authorizations() ==
      std::vector<std::string>{"Bearer lookup-secret", "Bearer lookup-secret",
                               "Bearer lookup-secret", "Bearer lookup-secret"});

  CharacterServer oversized_server{
      {}, {{"large", envelope(character_json("large"))}}};
  adapters::VeniceBackend oversized_source{
      secret("lookup-secret"), {oversized_server.base_url(), 1s, 1s, 1s, 8}};
  auto limits = backend::ProviderCharacterLimits{};
  limits.maximum_response_bytes = 32;
  const auto oversized = oversized_source.lookup(
      make_id<domain::ProviderCharacterId>("large"), limits);
  REQUIRE_FALSE(oversized);
  REQUIRE(oversized.error().code ==
          backend::ProviderCharacterErrorCode::too_large);

  for (const bool null_adult : {false, true}) {
    auto unclassified =
        character_json(null_adult ? "null-adult" : "missing-adult");
    if (null_adult) {
      unclassified["adult"] = nullptr;
    } else {
      unclassified.erase("adult");
    }
    const auto slug = unclassified.at("slug").get<std::string>();
    CharacterServer unclassified_server{
        {}, {{slug, envelope(std::move(unclassified))}}};
    adapters::VeniceBackend unclassified_source{
        secret("lookup-secret"),
        {unclassified_server.base_url(), 1s, 1s, 1s, 8}};
    const auto result =
        unclassified_source.lookup(make_id<domain::ProviderCharacterId>(slug));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code ==
            backend::ProviderCharacterErrorCode::invalid_data);
  }
}

TEST_CASE(
    "model catalog cache round trips atomically with restrictive permissions",
    "[adapter][models][cache]") {
  TempDirectory temporary{"aiforge-model-cache"};
  const auto path = temporary.path() / "cache" / "model-catalog.json";
  adapters::JsonModelCatalogCache cache{path};
  model::CatalogEntry entry{make_id<domain::ModelId>("text-model"), "text"};
  entry.name = "Text Model";
  entry.context_window_tokens = 8192;
  entry.capabilities = {{model::Capability::tool_calling, true},
                        {model::Capability::vision, std::nullopt}};
  entry.pricing = model::Pricing{};
  entry.pricing->base.input =
      model::Price{domain::DecimalAmount::from("0.5").value(),
                   domain::DecimalAmount::from("1").value()};
  model::CatalogSnapshot snapshot{
      std::chrono::sys_time<std::chrono::milliseconds>{1234ms},
      {std::move(entry)}};

  REQUIRE(cache.store(snapshot, {}));
  auto loaded = cache.load({});
  REQUIRE(loaded);
  REQUIRE(loaded->has_value());
  REQUIRE((*loaded)->entries == snapshot.entries);
  REQUIRE((*loaded)->fetched_at == snapshot.fetched_at);
  REQUIRE((*loaded)->source_id == snapshot.source_id);

  const auto permissions = std::filesystem::status(path).permissions();
  REQUIRE((permissions & (std::filesystem::perms::group_all |
                          std::filesystem::perms::others_all)) ==
          std::filesystem::perms::none);
}

TEST_CASE("model cache rejects duplicate JSON, loose modes, symlinks and "
          "cancellation",
          "[adapter][models][cache][failure]") {
  TempDirectory temporary{"aiforge-model-cache-hostile"};
  const auto path = temporary.path() / "model-catalog.json";
  adapters::JsonModelCatalogCache cache{path, 4096};
  write_file(
      path,
      R"({"schema_version":1,"schema_version":1,"fetched_at_ms":0,"entries":[]})");
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
  REQUIRE_FALSE(cache.load({}));

  write_file(path, R"({"schema_version":1,"fetched_at_ms":0,"entries":[]})");
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
  REQUIRE_FALSE(cache.load({}));

  write_file(
      path,
      R"({"schema_version":2,"fetched_at_ms":0,"source_id":"test.models","source_revision":null,"entries":[]})");
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write |
                                   std::filesystem::perms::group_read,
                               std::filesystem::perm_options::replace);
  REQUIRE_FALSE(cache.load({}));

  std::filesystem::remove(path);
  const auto target = temporary.path() / "target";
  write_file(target, "{}");
  std::filesystem::create_symlink(target, path);
  REQUIRE_FALSE(cache.load({}));

  std::stop_source stop;
  stop.request_stop();
  auto cancelled = cache.load(stop.get_token());
  REQUIRE_FALSE(cancelled);
  REQUIRE(cancelled.error().code == model::CatalogErrorCode::cancelled);
}

TEST_CASE("model picker navigates from either focus and survives tiny geometry",
          "[adapter][models][picker]") {
  model::CatalogEntry alpha{make_id<domain::ModelId>("alpha-chat"), "text"};
  alpha.name = "Alpha";
  alpha.context_window_tokens = 8192;
  model::CatalogEntry beta{make_id<domain::ModelId>("beta-chat"), "text"};
  beta.name = "Beta";
  beta.context_window_tokens = 8192;
  model::CatalogSnapshot snapshot{
      std::chrono::sys_time<std::chrono::milliseconds>{1ms},
      {std::move(alpha), std::move(beta)}};

  adapters::ModelPickerDialog dialog;
  std::optional<domain::ModelId> selected;
  bool reported{};
  dialog.on_result([&](std::optional<domain::ModelId> result) {
    reported = true;
    selected = std::move(result);
  });
  dialog.set_models(snapshot, make_id<domain::ModelId>("alpha-chat"));
  termforge::Screen screen{60, 16};
  dialog.draw(screen);
  REQUIRE(
      dialog.on_event(termforge::KeyEvent{termforge::Key::Down, 0, false, false,
                                          false, termforge::KeyAction::Press}));
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Enter, 0, false,
                                              false, false,
                                              termforge::KeyAction::Press}));
  REQUIRE(reported);
  REQUIRE(selected == make_id<domain::ModelId>("beta-chat"));

  adapters::ModelPickerDialog refocused_dialog;
  std::optional<domain::ModelId> refocused_selection;
  refocused_dialog.on_result([&](std::optional<domain::ModelId> result) {
    refocused_selection = result;
  });
  refocused_dialog.set_models(snapshot, {});
  refocused_dialog.draw(screen);
  REQUIRE(refocused_dialog.on_event(
      termforge::KeyEvent{termforge::Key::Tab, 0, false, false, false,
                          termforge::KeyAction::Press}));
  type_text(refocused_dialog, "beta");
  REQUIRE(refocused_dialog.on_event(
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}));
  REQUIRE(refocused_selection == make_id<domain::ModelId>("beta-chat"));

  adapters::ModelPickerDialog cursor_dialog;
  cursor_dialog.set_models(snapshot, {});
  cursor_dialog.draw(screen);
  type_text(cursor_dialog, "beta");
  REQUIRE(cursor_dialog.on_event(
      termforge::KeyEvent{termforge::Key::Home, 0, false, false, false,
                          termforge::KeyAction::Press}));
  type_text(cursor_dialog, "x");
  REQUIRE(cursor_dialog.on_event(
      termforge::KeyEvent{termforge::Key::End, 0, false, false, false,
                          termforge::KeyAction::Press}));
  type_text(cursor_dialog, "y");
  termforge::Screen cursor_screen{60, 16};
  cursor_dialog.draw(cursor_screen);
  REQUIRE(screen_text(cursor_screen).find("xbetay") != std::string::npos);

  adapters::ModelPickerDialog stable_dialog;
  std::optional<domain::ModelId> stable_selection;
  stable_dialog.on_result([&](std::optional<domain::ModelId> result) {
    stable_selection = result;
  });
  stable_dialog.set_models(snapshot, {});
  stable_dialog.draw(screen);
  REQUIRE(stable_dialog.on_event(
      termforge::KeyEvent{termforge::Key::Down, 0, false, false, false,
                          termforge::KeyAction::Press}));
  type_text(stable_dialog, "a");
  REQUIRE(stable_dialog.on_event(
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}));
  REQUIRE(stable_selection == make_id<domain::ModelId>("beta-chat"));

  adapters::ModelPickerDialog cancelled_dialog;
  bool cancelled_result{};
  cancelled_dialog.on_result([&](std::optional<domain::ModelId> result) {
    cancelled_result = !result.has_value();
  });
  cancelled_dialog.set_models(snapshot, make_id<domain::ModelId>("alpha-chat"));
  termforge::Screen tiny{1, 1};
  cancelled_dialog.draw(tiny);
  REQUIRE(cancelled_dialog.on_event(
      termforge::KeyEvent{termforge::Key::Escape, 0, false, false, false,
                          termforge::KeyAction::Press}));
  REQUIRE(cancelled_result);
}

TEST_CASE("model picker filters explicit capability support states",
          "[adapter][models][picker]") {
  model::CatalogEntry supported{make_id<domain::ModelId>("supported"), "text"};
  supported.context_window_tokens = 8192;
  supported.capabilities.push_back({model::Capability::tool_calling, true});
  model::CatalogEntry unsupported{make_id<domain::ModelId>("unsupported"),
                                  "text"};
  unsupported.context_window_tokens = 8192;
  unsupported.capabilities.push_back({model::Capability::tool_calling, false});
  model::CatalogEntry unknown{make_id<domain::ModelId>("unknown"), "text"};
  unknown.context_window_tokens = 8192;
  model::CatalogSnapshot snapshot{
      std::chrono::sys_time<std::chrono::milliseconds>{1ms},
      {std::move(supported), std::move(unsupported), std::move(unknown)}};

  const auto select = [&](const std::string_view filter) {
    adapters::ModelPickerDialog dialog;
    std::optional<domain::ModelId> selected;
    dialog.on_result(
        [&](std::optional<domain::ModelId> result) { selected = result; });
    dialog.set_models(snapshot, {});
    termforge::Screen screen{100, 24};
    dialog.draw(screen);
    type_text(dialog, filter);
    REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Enter, 0, false,
                                                false, false,
                                                termforge::KeyAction::Press}));
    return selected;
  };

  REQUIRE(select("cap:tools=true") == make_id<domain::ModelId>("supported"));
  REQUIRE(select("cap:tools=false") == make_id<domain::ModelId>("unsupported"));
  REQUIRE(select("cap:tools=unknown") == make_id<domain::ModelId>("unknown"));
}

TEST_CASE("model picker explains malformed, empty, and unmatched filters",
          "[adapter][models][picker][failure]") {
  model::CatalogEntry entry{make_id<domain::ModelId>("model"), "text"};
  entry.context_window_tokens = 8192;
  model::CatalogSnapshot snapshot{
      std::chrono::sys_time<std::chrono::milliseconds>{1ms},
      {std::move(entry)}};

  adapters::ModelPickerDialog malformed;
  malformed.set_models(snapshot, {});
  termforge::Screen malformed_screen{110, 28};
  malformed.draw(malformed_screen);
  type_text(malformed, "cap:tools=maybe");
  malformed.draw(malformed_screen);
  REQUIRE(screen_text(malformed_screen).find("Filter error") !=
          std::string::npos);
  bool malformed_reported{};
  malformed.on_result(
      [&](std::optional<domain::ModelId>) { malformed_reported = true; });
  REQUIRE_FALSE(malformed.on_event(
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}));
  REQUIRE_FALSE(malformed_reported);

  adapters::ModelPickerDialog unmatched;
  unmatched.set_models(snapshot, {});
  termforge::Screen unmatched_screen{110, 28};
  unmatched.draw(unmatched_screen);
  type_text(unmatched, "absent");
  unmatched.draw(unmatched_screen);
  REQUIRE(screen_text(unmatched_screen).find("No models match") !=
          std::string::npos);

  adapters::ModelPickerDialog empty;
  empty.set_models(
      model::CatalogSnapshot{
          std::chrono::sys_time<std::chrono::milliseconds>{1ms}},
      {});
  termforge::Screen empty_screen{110, 28};
  empty.draw(empty_screen);
  REQUIRE(screen_text(empty_screen).find("No models match") !=
          std::string::npos);

  model::CatalogEntry offline_entry{make_id<domain::ModelId>("offline"),
                                    "text"};
  offline_entry.context_window_tokens = 8192;
  offline_entry.offline = true;
  adapters::ModelPickerDialog offline;
  bool offline_reported{};
  offline.on_result(
      [&](std::optional<domain::ModelId>) { offline_reported = true; });
  offline.set_models(
      model::CatalogSnapshot{
          std::chrono::sys_time<std::chrono::milliseconds>{1ms},
          {std::move(offline_entry)}},
      {});
  termforge::Screen offline_screen{110, 28};
  offline.draw(offline_screen);
  REQUIRE(offline.on_event(termforge::KeyEvent{termforge::Key::Enter, 0, false,
                                               false, false,
                                               termforge::KeyAction::Press}));
  REQUIRE_FALSE(offline_reported);
}

TEST_CASE("model picker bounds large catalogs and exposes selected metadata",
          "[adapter][models][picker]") {
  std::vector<model::CatalogEntry> entries;
  entries.reserve(4096);
  for (int index{}; index < 4096; ++index) {
    auto id = std::string{"model-"};
    id += std::to_string(10000 + index).substr(1);
    model::CatalogEntry entry{make_id<domain::ModelId>(id), "text"};
    entry.context_window_tokens = 8192;
    entries.push_back(std::move(entry));
  }
  auto& selected_entry = entries.front();
  selected_entry.name = "Metadata model";
  selected_entry.maximum_output_tokens = 1024;
  selected_entry.capabilities = {
      {model::Capability::tool_calling, true},
      {model::Capability::reasoning, std::nullopt},
      {model::Capability::web_search, false},
      {model::Capability::vision, true},
  };
  model::Pricing pricing;
  pricing.base.input =
      model::Price{domain::DecimalAmount::from("1").value(), std::nullopt};
  pricing.extended_threshold_tokens = 64000;
  pricing.extended = model::PriceTier{};
  pricing.extended->output =
      model::Price{std::nullopt, domain::DecimalAmount::from("2").value()};
  selected_entry.pricing = std::move(pricing);
  model::CatalogSnapshot snapshot{
      std::chrono::sys_time<std::chrono::milliseconds>{1ms},
      std::move(entries)};
  snapshot.origin = model::CatalogOrigin::stale_cache;

  adapters::ModelPickerDialog metadata;
  metadata.set_models(snapshot, make_id<domain::ModelId>("model-0000"));
  termforge::Screen metadata_screen{120, 30};
  metadata.draw(metadata_screen);
  const auto rendered = screen_text(metadata_screen);
  REQUIRE(rendered.find("reasoning-effort") != std::string::npos);
  for (const auto name : {"schema", "logprobs", "web-search", "x-search", "tee",
                          "e2ee", "code"}) {
    CAPTURE(name);
    REQUIRE(rendered.find(name) != std::string::npos);
  }
  REQUIRE(rendered.find("Limits: context 8192 | output 1024 | status online") !=
          std::string::npos);
  REQUIRE(rendered.find("tools true | reasoning unknown | web false") !=
          std::string::npos);
  REQUIRE(rendered.find("Pricing base: input USD") != std::string::npos);
  REQUIRE(rendered.find("Pricing extended@64000: input none | output diem") !=
          std::string::npos);
  REQUIRE(rendered.find("Catalog: stale cache") != std::string::npos);

  std::optional<domain::ModelId> selected;
  metadata.on_result(
      [&](std::optional<domain::ModelId> result) { selected = result; });
  REQUIRE(metadata.on_event(termforge::KeyEvent{termforge::Key::Tab, 0, false,
                                                false, false,
                                                termforge::KeyAction::Press}));
  REQUIRE(metadata.on_event(termforge::KeyEvent{termforge::Key::End, 0, false,
                                                false, false,
                                                termforge::KeyAction::Press}));
  REQUIRE(metadata.on_event(termforge::KeyEvent{termforge::Key::Enter, 0, false,
                                                false, false,
                                                termforge::KeyAction::Press}));
  REQUIRE(selected == make_id<domain::ModelId>("model-4095"));
}

TEST_CASE("interactive model selection rejects stale and unusable entries",
          "[adapter][models][picker][failure]") {
  model::CatalogEntry available{make_id<domain::ModelId>("available"), "text"};
  available.context_window_tokens = 8192;
  model::CatalogSnapshot snapshot{
      std::chrono::sys_time<std::chrono::milliseconds>{1ms},
      {std::move(available)}};
  REQUIRE(adapters::validate_interactive_model_selection(
      snapshot, make_id<domain::ModelId>("available")));

  const auto vanished = adapters::validate_interactive_model_selection(
      snapshot, make_id<domain::ModelId>("vanished"));
  REQUIRE_FALSE(vanished);
  REQUIRE(vanished.error() == "selected text model is no longer available");

  model::CatalogEntry offline{make_id<domain::ModelId>("offline"), "text"};
  offline.context_window_tokens = 8192;
  offline.offline = true;
  model::CatalogSnapshot offline_snapshot{
      std::chrono::sys_time<std::chrono::milliseconds>{1ms},
      {std::move(offline)}};
  const auto unavailable = adapters::validate_interactive_model_selection(
      offline_snapshot, make_id<domain::ModelId>("offline"));
  REQUIRE_FALSE(unavailable);
  REQUIRE(unavailable.error() == "selected text model is offline");

  model::CatalogEntry incomplete{make_id<domain::ModelId>("incomplete"),
                                 "text"};
  model::CatalogSnapshot incomplete_snapshot{
      std::chrono::sys_time<std::chrono::milliseconds>{1ms},
      {std::move(incomplete)}};
  const auto malformed = adapters::validate_interactive_model_selection(
      incomplete_snapshot, make_id<domain::ModelId>("incomplete"));
  REQUIRE_FALSE(malformed);
  REQUIRE(malformed.error() ==
          "selected text model has invalid context metadata");
}

TEST_CASE("global instruction paths follow fixed XDG configuration semantics",
          "[adapter][global-instructions][failure]") {
  const auto xdg = adapters::resolve_user_global_instruction_path(
      {.xdg_config_home = std::filesystem::path{"/tmp/xdg"},
       .home = std::filesystem::path{"/tmp/home"}});
  REQUIRE(xdg ==
          std::filesystem::path{"/tmp/xdg/aiforge/instructions/global.md"});

  const auto home = adapters::resolve_user_global_instruction_path(
      {.xdg_config_home = std::filesystem::path{"relative"},
       .home = std::filesystem::path{"/tmp/home"}});
  REQUIRE(home == std::filesystem::path{
                      "/tmp/home/.config/aiforge/instructions/global.md"});

  const auto missing = adapters::resolve_user_global_instruction_path({});
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().code ==
          instructions::UserGlobalInstructionErrorCode::missing_home);
}

TEST_CASE("filesystem global instructions are private bounded exact writes",
          "[adapter][global-instructions][editor]") {
  TempDirectory temporary{"aiforge-global-instruction"};
  const auto path = temporary.path() / "aiforge" / "instructions" / "global.md";
  adapters::FilesystemUserGlobalInstructionSource source{path};

  const auto missing = source.load();
  REQUIRE(missing);
  REQUIRE_FALSE(*missing);

  const instructions::UserGlobalInstructionWrite create{
      std::nullopt, "Review carefully.\n", {}};
  const auto created = source.write(create);
  REQUIRE(created);
  REQUIRE_FALSE(created->previous);
  REQUIRE(created->resulting.source_id.value() ==
          domain::user_global_instruction_source_identity);
  REQUIRE(created->resulting.source_location ==
          domain::user_global_instruction_source_location);
  REQUIRE(created->resulting.content_digest.byte_size == create.text.size());
  REQUIRE((std::filesystem::status(path.parent_path()).permissions() &
           std::filesystem::perms::all) == std::filesystem::perms::owner_all);
  REQUIRE((std::filesystem::status(path).permissions() &
           std::filesystem::perms::all) ==
          (std::filesystem::perms::owner_read |
           std::filesystem::perms::owner_write));

  const auto duplicate = source.write(create);
  REQUIRE_FALSE(duplicate);
  REQUIRE(duplicate.error().code ==
          instructions::UserGlobalInstructionEditorErrorCode::already_exists);
  REQUIRE_FALSE(duplicate.error().may_have_applied);

  const instructions::UserGlobalInstructionWrite replace{
      created->resulting, "Use exact evidence.\n", {}};
  const auto replaced = source.write(replace);
  REQUIRE(replaced);
  REQUIRE(replaced->previous == created->resulting);
  REQUIRE(replaced->resulting != created->resulting);
  const auto loaded = source.load();
  REQUIRE(loaded);
  REQUIRE(*loaded);
  REQUIRE((*loaded)->text == replace.text);
  REQUIRE((*loaded)->reference == replaced->resulting);

  const auto stale = source.write(replace);
  REQUIRE_FALSE(stale);
  REQUIRE(stale.error().code ==
          instructions::UserGlobalInstructionEditorErrorCode::source_mismatch);
  REQUIRE(stale.error().observed == replaced->resulting);
  REQUIRE_FALSE(stale.error().may_have_applied);

  const auto bounded = source.load({.maximum_file_bytes = 4});
  REQUIRE_FALSE(bounded);
  REQUIRE(bounded.error().code ==
          instructions::UserGlobalInstructionErrorCode::resource_exhausted);

  std::stop_source cancellation;
  cancellation.request_stop();
  const auto cancelled = source.write(
      {replaced->resulting, "must not publish", {}}, cancellation.get_token());
  REQUIRE_FALSE(cancelled);
  REQUIRE(cancelled.error().code ==
          instructions::UserGlobalInstructionEditorErrorCode::cancelled);
  REQUIRE(read_file(path) == replace.text);
}

TEST_CASE("filesystem global instructions reject hostile paths and entries",
          "[adapter][global-instructions][failure]") {
  TempDirectory temporary{"aiforge-global-hostile"};
  const auto app = temporary.path() / "aiforge";
  const auto root = app / "instructions";
  const auto path = root / "global.md";
  REQUIRE(std::filesystem::create_directories(root));
  REQUIRE(::chmod(app.c_str(), 0700) == 0);
  REQUIRE(::chmod(root.c_str(), 0700) == 0);

  write_file(temporary.path() / "outside.md", "outside");
  std::error_code link_error;
  std::filesystem::create_symlink(temporary.path() / "outside.md", path,
                                  link_error);
  REQUIRE_FALSE(link_error);
  adapters::FilesystemUserGlobalInstructionSource source{path};
  const auto escaped = source.load();
  REQUIRE_FALSE(escaped);
  REQUIRE(escaped.error().code ==
          instructions::UserGlobalInstructionErrorCode::path_escape);

  REQUIRE(std::filesystem::remove(path));
  write_file(path, "private");
  REQUIRE(::chmod(path.c_str(), 0644) == 0);
  const auto broad = source.load();
  REQUIRE_FALSE(broad);
  REQUIRE(broad.error().code ==
          instructions::UserGlobalInstructionErrorCode::permission_denied);

  REQUIRE(::chmod(path.c_str(), 0600) == 0);
  std::filesystem::create_hard_link(path, root / "alias.md", link_error);
  REQUIRE_FALSE(link_error);
  const auto linked = source.load();
  REQUIRE_FALSE(linked);
  REQUIRE(linked.error().code ==
          instructions::UserGlobalInstructionErrorCode::unsupported_entry);
  REQUIRE(std::filesystem::remove(root / "alias.md"));

  REQUIRE(std::filesystem::remove(path));
  REQUIRE(std::filesystem::create_directory(path));
  const auto directory = source.load();
  REQUIRE_FALSE(directory);
  REQUIRE(directory.error().code ==
          instructions::UserGlobalInstructionErrorCode::unsupported_entry);

  REQUIRE(std::filesystem::remove(path));
  REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
  const auto fifo = source.load();
  REQUIRE_FALSE(fifo);
  REQUIRE(fifo.error().code ==
          instructions::UserGlobalInstructionErrorCode::unsupported_entry);

  const auto real_app = temporary.path() / "real-aiforge";
  REQUIRE(std::filesystem::create_directories(real_app / "instructions"));
  REQUIRE(::chmod(real_app.c_str(), 0700) == 0);
  REQUIRE(::chmod((real_app / "instructions").c_str(), 0700) == 0);
  const auto alias_app = temporary.path() / "alias-aiforge";
  std::filesystem::create_directory_symlink(real_app, alias_app, link_error);
  REQUIRE_FALSE(link_error);
  adapters::FilesystemUserGlobalInstructionSource traversed{
      alias_app / "instructions" / "global.md"};
  const auto parent_escape = traversed.load();
  REQUIRE_FALSE(parent_escape);
  REQUIRE(parent_escape.error().code ==
          instructions::UserGlobalInstructionErrorCode::path_escape);
}

TEST_CASE("filesystem global instructions reject embedded null paths",
          "[adapter][global-instructions][failure]") {
  TempDirectory temporary{"aiforge-global-null-path"};
  const auto root = temporary.path() / "aiforge" / "instructions";
  const auto path = root / "global.md";
  REQUIRE(std::filesystem::create_directories(root));
  REQUIRE(::chmod((temporary.path() / "aiforge").c_str(), 0700) == 0);
  REQUIRE(::chmod(root.c_str(), 0700) == 0);
  write_file(path, "must remain private");
  REQUIRE(::chmod(path.c_str(), 0600) == 0);

  auto poisoned_path = temporary.path().string();
  poisoned_path.push_back('\0');
  poisoned_path += "shadow/aiforge/instructions/global.md";
  adapters::FilesystemUserGlobalInstructionSource source{
      std::filesystem::path{poisoned_path}};

  const auto loaded = source.load();
  REQUIRE_FALSE(loaded);
  REQUIRE(loaded.error().code ==
          instructions::UserGlobalInstructionErrorCode::invalid_request);
  REQUIRE(loaded.error().message.find(temporary.path().string()) ==
          std::string::npos);
  REQUIRE(loaded.error().message.find("must remain private") ==
          std::string::npos);

  REQUIRE(std::filesystem::remove(path));
  const auto written = source.write({std::nullopt, "must not publish", {}});
  REQUIRE_FALSE(written);
  REQUIRE(written.error().code ==
          instructions::UserGlobalInstructionEditorErrorCode::invalid_request);
  REQUIRE(written.error().message.find(temporary.path().string()) ==
          std::string::npos);
  REQUIRE(written.error().message.find("must not publish") ==
          std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("filesystem global replacement preserves racing external content",
          "[adapter][global-instructions][editor][failure]") {
  TempDirectory temporary{"aiforge-global-race"};
  const auto root = temporary.path() / "aiforge" / "instructions";
  const auto path = root / "global.md";
  adapters::FilesystemUserGlobalInstructionSource initial{path};
  const auto created = initial.write({std::nullopt, "original", {}});
  REQUIRE(created);

  bool raced{};
  adapters::FilesystemUserGlobalInstructionSource racing{
      path,
      [&](const adapters::UserGlobalInstructionFilesystemCheckpointStage stage)
          -> std::expected<void,
                           instructions::UserGlobalInstructionEditorError> {
        if (stage == adapters::UserGlobalInstructionFilesystemCheckpointStage::
                         replacement_ready &&
            !raced) {
          raced = true;
          write_file(path, "external change");
        }
        return {};
      }};
  const auto replacement =
      racing.write({created->resulting, "must not replace external", {}});
  REQUIRE_FALSE(replacement);
  REQUIRE(
      replacement.error().code ==
      instructions::UserGlobalInstructionEditorErrorCode::concurrent_change);
  REQUIRE_FALSE(replacement.error().may_have_applied);
  REQUIRE(read_file(path) == "external change");

  const auto current = initial.load();
  REQUIRE(current);
  REQUIRE(*current);
  const auto detached = temporary.path() / "detached-instructions";
  adapters::FilesystemUserGlobalInstructionSource swapping{
      path,
      [&](const adapters::UserGlobalInstructionFilesystemCheckpointStage stage)
          -> std::expected<void,
                           instructions::UserGlobalInstructionEditorError> {
        if (stage == adapters::UserGlobalInstructionFilesystemCheckpointStage::
                         root_revalidation_ready) {
          std::filesystem::rename(root, detached);
          REQUIRE(std::filesystem::create_directory(root));
          REQUIRE(::chmod(root.c_str(), 0700) == 0);
        }
        return {};
      }};
  const auto swapped =
      swapping.write({(*current)->reference, "published detached", {}});
  REQUIRE_FALSE(swapped);
  REQUIRE(
      swapped.error().code ==
      instructions::UserGlobalInstructionEditorErrorCode::concurrent_change);
  REQUIRE(swapped.error().may_have_applied);
  REQUIRE(swapped.error().message.find(temporary.path().string()) ==
          std::string::npos);
  REQUIRE(swapped.error().message.find("published detached") ==
          std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(path));
  REQUIRE(read_file(detached / "global.md") == "published detached");
}

TEST_CASE("persona roots follow XDG configuration semantics",
          "[adapter][persona]") {
  const auto xdg = adapters::resolve_persona_root(
      {.xdg_config_home = std::filesystem::path{"/tmp/xdg"},
       .home = std::filesystem::path{"/tmp/home"}});
  REQUIRE(xdg == std::filesystem::path{"/tmp/xdg/aiforge/personas"});

  const auto home = adapters::resolve_persona_root(
      {.xdg_config_home = std::filesystem::path{"relative"},
       .home = std::filesystem::path{"/tmp/home"}});
  REQUIRE(home == std::filesystem::path{"/tmp/home/.config/aiforge/personas"});

  const auto missing = adapters::resolve_persona_root({});
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().code == persona::PersonaErrorCode::missing_home);
}

TEST_CASE("filesystem personas are deterministic bounded attributed text",
          "[adapter][persona]") {
  TempDirectory temporary{"aiforge-personas"};
  const auto root = temporary.path() / "personas";
  adapters::FilesystemPersonaSource source{root};

  const auto empty = source.list();
  REQUIRE(empty);
  REQUIRE(empty->empty());
  const auto absent = source.load("Reviewer");
  REQUIRE_FALSE(absent);
  REQUIRE(absent.error().code == persona::PersonaErrorCode::not_found);

  REQUIRE(std::filesystem::create_directory(root));
  write_file(root / "Reviewer.md", "abc");
  write_file(root / "writer.txt", "Write concisely.\nSecond line.");

  const auto listed = source.list();
  REQUIRE(listed);
  REQUIRE(listed->size() == 2);
  REQUIRE((*listed)[0].reference.name == "Reviewer");
  REQUIRE((*listed)[0].description == "abc");
  REQUIRE((*listed)[1].reference.name == "writer");

  const auto loaded = source.load("reviewer");
  REQUIRE(loaded);
  REQUIRE(loaded->reference.persona_id.value() == "persona:reviewer");
  REQUIRE(loaded->reference.source_location == "personas/Reviewer.md");
  REQUIRE(loaded->reference.content_digest.algorithm == "sha256");
  REQUIRE(loaded->reference.content_digest.value ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  REQUIRE(loaded->reference.content_digest.byte_size == 3);
  REQUIRE(loaded->text == "abc");

  const auto traversed = source.load("../Reviewer");
  REQUIRE_FALSE(traversed);
  REQUIRE(traversed.error().code == persona::PersonaErrorCode::invalid_name);

  const auto bounded =
      source.load("Reviewer", {.maximum_personas = 256,
                               .maximum_name_bytes = 96,
                               .maximum_file_bytes = 2,
                               .maximum_description_bytes = 160});
  REQUIRE_FALSE(bounded);
  REQUIRE(bounded.error().code ==
          persona::PersonaErrorCode::resource_exhausted);
}

TEST_CASE("filesystem personas fail closed on aliases symlinks and bad text",
          "[adapter][persona][failure]") {
  TempDirectory temporary{"aiforge-persona-hostile"};
  const auto root = temporary.path() / "personas";
  REQUIRE(std::filesystem::create_directory(root));
  adapters::FilesystemPersonaSource source{root};

  write_file(root / "reviewer.md", "safe");
  write_file(root / "Reviewer.txt", "alias");
  const auto ambiguous = source.list();
  REQUIRE_FALSE(ambiguous);
  REQUIRE(ambiguous.error().code == persona::PersonaErrorCode::ambiguous_name);

  REQUIRE(std::filesystem::remove(root / "Reviewer.txt"));
  write_file(temporary.path() / "outside.md", "outside");
  std::error_code symlink_error;
  std::filesystem::create_symlink(temporary.path() / "outside.md",
                                  root / "escape.md", symlink_error);
  REQUIRE_FALSE(symlink_error);
  const auto escaped = source.load("escape");
  REQUIRE_FALSE(escaped);
  REQUIRE(escaped.error().code == persona::PersonaErrorCode::path_escape);

  std::error_code root_symlink_error;
  std::filesystem::create_directory_symlink(root, temporary.path() / "alias",
                                            root_symlink_error);
  REQUIRE_FALSE(root_symlink_error);
  adapters::FilesystemPersonaSource aliased_root{temporary.path() / "alias"};
  const auto rejected_root = aliased_root.list();
  REQUIRE_FALSE(rejected_root);
  REQUIRE(rejected_root.error().code == persona::PersonaErrorCode::path_escape);

  REQUIRE(std::filesystem::remove(root / "escape.md"));
  write_file(root / "unsafe.md", std::string{"bad\x01text", 8});
  const auto malformed = source.load("unsafe");
  REQUIRE_FALSE(malformed);
  REQUIRE(malformed.error().code == persona::PersonaErrorCode::malformed_text);

  write_file(root / "unicode-control.md", std::string{"bad\xc2\x85text", 9});
  const auto unicode_control = source.load("unicode-control");
  REQUIRE_FALSE(unicode_control);
  REQUIRE(unicode_control.error().code ==
          persona::PersonaErrorCode::malformed_text);

  write_file(root / "empty.md", "");
  const auto empty = source.load("empty");
  REQUIRE_FALSE(empty);
  REQUIRE(empty.error().code == persona::PersonaErrorCode::malformed_text);

  const auto too_many = source.list({.maximum_personas = 1,
                                     .maximum_name_bytes = 96,
                                     .maximum_file_bytes = 1024U * 1024U,
                                     .maximum_description_bytes = 160});
  REQUIRE_FALSE(too_many);
  REQUIRE(too_many.error().code ==
          persona::PersonaErrorCode::resource_exhausted);

  std::stop_source cancelled;
  cancelled.request_stop();
  const auto stopped = source.load("reviewer", {}, cancelled.get_token());
  REQUIRE_FALSE(stopped);
  REQUIRE(stopped.error().code == persona::PersonaErrorCode::cancelled);

  REQUIRE(std::filesystem::create_directory(root / "directory.md"));
  const auto nonregular = source.load("directory");
  REQUIRE_FALSE(nonregular);
  REQUIRE(nonregular.error().code ==
          persona::PersonaErrorCode::unsupported_entry);
}

TEST_CASE(
    "filesystem persona creation is restrictive atomic and collision safe",
    "[adapter][persona][editor]") {
  TempDirectory temporary{"aiforge-persona-create"};
  const auto app_root = temporary.path() / "aiforge";
  const auto root = app_root / "personas";
  adapters::FilesystemPersonaSource source{root};

  const persona::PersonaCreate request{
      {"Reviewer", persona::PersonaFileKind::markdown, "Review carefully.\n"}};
  const auto created = source.create(request);
  REQUIRE(created);
  REQUIRE_FALSE(created->previous);
  REQUIRE(created->resulting.name == "Reviewer");
  REQUIRE(created->resulting.source_location == "personas/Reviewer.md");
  REQUIRE(created->resulting.content_digest.byte_size == 18);
  REQUIRE((std::filesystem::status(app_root).permissions() &
           std::filesystem::perms::all) == std::filesystem::perms::owner_all);
  REQUIRE((std::filesystem::status(root).permissions() &
           std::filesystem::perms::all) == std::filesystem::perms::owner_all);
  REQUIRE((std::filesystem::status(root / "Reviewer.md").permissions() &
           std::filesystem::perms::all) ==
          (std::filesystem::perms::owner_read |
           std::filesystem::perms::owner_write));

  const auto loaded = source.load("reviewer");
  REQUIRE(loaded);
  REQUIRE(loaded->reference == created->resulting);
  REQUIRE(loaded->text == request.draft.text);

  const auto alias_collision = source.create(
      {{"reviewer", persona::PersonaFileKind::text, "different"}, {}});
  REQUIRE_FALSE(alias_collision);
  REQUIRE(alias_collision.error().code ==
          persona::PersonaEditorErrorCode::already_exists);
  REQUIRE_FALSE(alias_collision.error().may_have_applied);
  REQUIRE(std::filesystem::exists(root / "Reviewer.md"));
  REQUIRE_FALSE(std::filesystem::exists(root / "reviewer.txt"));
}

TEST_CASE("filesystem persona replacement checks the exact digest twice",
          "[adapter][persona][editor][failure]") {
  TempDirectory temporary{"aiforge-persona-replace"};
  const auto root = temporary.path() / "personas";
  REQUIRE(std::filesystem::create_directory(root));
  std::filesystem::permissions(root, std::filesystem::perms::owner_all);
  write_file(root / "Reviewer.md", "original");
  adapters::FilesystemPersonaSource source{root};

  const auto original = source.load("Reviewer");
  REQUIRE(original);
  const persona::PersonaReplace request{original->reference, "replacement"};
  const auto replaced = source.replace(request);
  REQUIRE(replaced);
  REQUIRE(replaced->previous == original->reference);
  REQUIRE(replaced->resulting != original->reference);
  const auto replacement = source.load("Reviewer");
  REQUIRE(replacement);
  REQUIRE(replacement->text == "replacement");
  REQUIRE((std::filesystem::status(root / "Reviewer.md").permissions() &
           std::filesystem::perms::all) ==
          (std::filesystem::perms::owner_read |
           std::filesystem::perms::owner_write));

  const auto stale = source.replace(request);
  REQUIRE_FALSE(stale);
  REQUIRE(stale.error().code ==
          persona::PersonaEditorErrorCode::source_mismatch);
  REQUIRE(stale.error().observed == replaced->resulting);
  REQUIRE_FALSE(stale.error().may_have_applied);

  const auto current = source.load("Reviewer");
  REQUIRE(current);
  bool changed{};
  adapters::FilesystemPersonaSource racing{
      root,
      [&](const adapters::PersonaFilesystemCheckpointStage stage)
          -> std::expected<void, persona::PersonaEditorError> {
        if (stage ==
                adapters::PersonaFilesystemCheckpointStage::temporary_synced &&
            !changed) {
          changed = true;
          write_file(root / "Reviewer.md", "external change");
        }
        return {};
      }};
  const auto concurrent =
      racing.replace({current->reference, "must not publish"});
  REQUIRE_FALSE(concurrent);
  REQUIRE(concurrent.error().code ==
          persona::PersonaEditorErrorCode::concurrent_change);
  REQUIRE_FALSE(concurrent.error().may_have_applied);
  const auto externally_changed = source.load("Reviewer");
  REQUIRE(externally_changed);
  REQUIRE(externally_changed->text == "external change");

  bool changed_at_publication{};
  adapters::FilesystemPersonaSource late_racing{
      root,
      [&](const adapters::PersonaFilesystemCheckpointStage stage)
          -> std::expected<void, persona::PersonaEditorError> {
        if (stage ==
                adapters::PersonaFilesystemCheckpointStage::replacement_ready &&
            !changed_at_publication) {
          changed_at_publication = true;
          write_file(root / "Reviewer.md", "last moment change");
        }
        return {};
      }};
  const auto late = late_racing.replace(
      {externally_changed->reference, "must roll back atomically"});
  REQUIRE_FALSE(late);
  REQUIRE(late.error().code ==
          persona::PersonaEditorErrorCode::concurrent_change);
  REQUIRE_FALSE(late.error().may_have_applied);
  const auto survived = source.load("Reviewer");
  REQUIRE(survived);
  REQUIRE(survived->text == "last moment change");

  bool first_race{};
  bool second_race{};
  adapters::FilesystemPersonaSource double_racing{
      root,
      [&](const adapters::PersonaFilesystemCheckpointStage stage)
          -> std::expected<void, persona::PersonaEditorError> {
        if (stage ==
                adapters::PersonaFilesystemCheckpointStage::replacement_ready &&
            !first_race) {
          first_race = true;
          write_file(root / "Reviewer.md", "first racing change");
        }
        if (stage ==
                adapters::PersonaFilesystemCheckpointStage::rollback_ready &&
            !second_race) {
          second_race = true;
          const auto second = root / ".second-writer";
          write_file(second, "second racing change");
          std::filesystem::rename(second, root / "Reviewer.md");
        }
        return {};
      }};
  const auto double_race = double_racing.replace(
      {survived->reference, "must preserve the second writer"});
  REQUIRE_FALSE(double_race);
  REQUIRE(double_race.error().code ==
          persona::PersonaEditorErrorCode::concurrent_change);
  REQUIRE(double_race.error().may_have_applied);
  const auto restored_first = source.load("Reviewer");
  REQUIRE(restored_first);
  REQUIRE(restored_first->text == "first racing change");
  bool preserved_second{};
  for (const auto& entry : std::filesystem::directory_iterator{root}) {
    if (!entry.path().filename().string().starts_with(".aiforge-persona-")) {
      continue;
    }
    std::ifstream input{entry.path(), std::ios::binary};
    const std::string text{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
    preserved_second = preserved_second || text == "second racing change";
  }
  REQUIRE(preserved_second);
}

TEST_CASE("filesystem persona writes recheck the resolved root after publish",
          "[adapter][persona][editor][failure]") {
  TempDirectory temporary{"aiforge-persona-root-race"};

  const auto create_root = temporary.path() / "create-app" / "personas";
  const auto detached_create = temporary.path() / "detached-create";
  adapters::FilesystemPersonaSource creating{
      create_root,
      [&](const adapters::PersonaFilesystemCheckpointStage stage)
          -> std::expected<void, persona::PersonaEditorError> {
        if (stage == adapters::PersonaFilesystemCheckpointStage::published) {
          std::filesystem::rename(create_root, detached_create);
          REQUIRE(std::filesystem::create_directory(create_root));
          std::filesystem::permissions(create_root,
                                       std::filesystem::perms::owner_all);
        }
        return {};
      }};
  const auto created = creating.create(
      {{"Reviewer", persona::PersonaFileKind::markdown, "created"}, {}});
  REQUIRE_FALSE(created);
  REQUIRE(created.error().code ==
          persona::PersonaEditorErrorCode::concurrent_change);
  REQUIRE(created.error().may_have_applied);
  REQUIRE_FALSE(std::filesystem::exists(create_root / "Reviewer.md"));
  REQUIRE(std::filesystem::exists(detached_create / "Reviewer.md"));

  const auto replace_root = temporary.path() / "replace-personas";
  REQUIRE(std::filesystem::create_directory(replace_root));
  std::filesystem::permissions(replace_root, std::filesystem::perms::owner_all);
  write_file(replace_root / "Reviewer.md", "original");
  adapters::FilesystemPersonaSource reader{replace_root};
  const auto original = reader.load("Reviewer");
  REQUIRE(original);
  const auto detached_replace = temporary.path() / "detached-replace";
  adapters::FilesystemPersonaSource replacing{
      replace_root,
      [&](const adapters::PersonaFilesystemCheckpointStage stage)
          -> std::expected<void, persona::PersonaEditorError> {
        if (stage == adapters::PersonaFilesystemCheckpointStage::published) {
          std::filesystem::rename(replace_root, detached_replace);
          REQUIRE(std::filesystem::create_directory(replace_root));
          std::filesystem::permissions(replace_root,
                                       std::filesystem::perms::owner_all);
        }
        return {};
      }};
  const auto replaced = replacing.replace({original->reference, "replacement"});
  REQUIRE_FALSE(replaced);
  REQUIRE(replaced.error().code ==
          persona::PersonaEditorErrorCode::concurrent_change);
  REQUIRE(replaced.error().may_have_applied);
  REQUIRE_FALSE(std::filesystem::exists(replace_root / "Reviewer.md"));
  REQUIRE(std::filesystem::exists(detached_replace / "Reviewer.md"));
}

TEST_CASE("filesystem persona checkpoints preserve the publication boundary",
          "[adapter][persona][editor][failure]") {
  TempDirectory temporary{"aiforge-persona-checkpoints"};
  const auto root = temporary.path() / "personas";

  adapters::FilesystemPersonaSource before{
      root,
      [](const adapters::PersonaFilesystemCheckpointStage stage)
          -> std::expected<void, persona::PersonaEditorError> {
        if (stage ==
            adapters::PersonaFilesystemCheckpointStage::temporary_synced) {
          return std::unexpected(persona::PersonaEditorError{
              persona::PersonaEditorErrorCode::io_failure,
              "injected pre-publication interruption",
              {},
              false,
              false});
        }
        return {};
      }};
  const auto interrupted_before = before.create(
      {{"before", persona::PersonaFileKind::markdown, "complete"}, {}});
  REQUIRE_FALSE(interrupted_before);
  REQUIRE_FALSE(interrupted_before.error().may_have_applied);
  REQUIRE_FALSE(std::filesystem::exists(root / "before.md"));
  for (const auto& entry : std::filesystem::directory_iterator{root}) {
    REQUIRE_FALSE(
        entry.path().filename().string().starts_with(".aiforge-persona-"));
  }

  adapters::FilesystemPersonaSource after{
      root,
      [](const adapters::PersonaFilesystemCheckpointStage stage)
          -> std::expected<void, persona::PersonaEditorError> {
        if (stage == adapters::PersonaFilesystemCheckpointStage::published) {
          return std::unexpected(persona::PersonaEditorError{
              persona::PersonaEditorErrorCode::io_failure,
              "injected post-publication interruption",
              {},
              false,
              false});
        }
        return {};
      }};
  const auto interrupted_after =
      after.create({{"after", persona::PersonaFileKind::text, "complete"}, {}});
  REQUIRE_FALSE(interrupted_after);
  REQUIRE(interrupted_after.error().may_have_applied);
  const auto published = after.load("after");
  REQUIRE(published);
  REQUIRE(published->text == "complete");

  std::size_t persona_entries{};
  for (const auto& entry : std::filesystem::directory_iterator{root}) {
    if (entry.path().extension() == ".md" ||
        entry.path().extension() == ".txt") {
      ++persona_entries;
    }
  }
  REQUIRE(persona_entries == 1);
}

TEST_CASE("filesystem persona writes reject unsafe roots and entries",
          "[adapter][persona][editor][failure]") {
  TempDirectory temporary{"aiforge-persona-write-hostile"};
  const auto outside = temporary.path() / "outside";
  REQUIRE(std::filesystem::create_directory(outside));
  std::filesystem::permissions(outside, std::filesystem::perms::owner_all);
  const auto alias = temporary.path() / "alias";
  std::error_code alias_error;
  std::filesystem::create_directory_symlink(outside, alias, alias_error);
  REQUIRE_FALSE(alias_error);
  adapters::FilesystemPersonaSource symlinked{alias};
  const auto root_escape = symlinked.create(
      {{"unsafe", persona::PersonaFileKind::markdown, "text"}, {}});
  REQUIRE_FALSE(root_escape);
  REQUIRE(root_escape.error().code ==
          persona::PersonaEditorErrorCode::path_escape);

  const auto real_parent = temporary.path() / "real-parent";
  REQUIRE(std::filesystem::create_directory(real_parent));
  std::filesystem::permissions(real_parent, std::filesystem::perms::owner_all);
  const auto parent_alias = temporary.path() / "parent-alias";
  std::error_code parent_alias_error;
  std::filesystem::create_directory_symlink(real_parent, parent_alias,
                                            parent_alias_error);
  REQUIRE_FALSE(parent_alias_error);
  adapters::FilesystemPersonaSource traversed_parent{parent_alias / "aiforge" /
                                                     "personas"};
  const auto parent_escape = traversed_parent.create(
      {{"unsafe", persona::PersonaFileKind::markdown, "text"}, {}});
  REQUIRE_FALSE(parent_escape);
  REQUIRE(parent_escape.error().code ==
          persona::PersonaEditorErrorCode::path_escape);
  REQUIRE_FALSE(std::filesystem::exists(real_parent / "aiforge"));

  const auto safe_root = temporary.path() / "safe";
  REQUIRE(std::filesystem::create_directory(safe_root));
  std::filesystem::permissions(safe_root, std::filesystem::perms::owner_all);
  write_file(temporary.path() / "outside.md", "outside");
  std::error_code entry_error;
  std::filesystem::create_symlink(temporary.path() / "outside.md",
                                  safe_root / "escape.md", entry_error);
  REQUIRE_FALSE(entry_error);
  adapters::FilesystemPersonaSource escaped_entry{safe_root};
  const auto entry_escape = escaped_entry.create(
      {{"safe", persona::PersonaFileKind::markdown, "text"}, {}});
  REQUIRE_FALSE(entry_escape);
  REQUIRE(entry_escape.error().code ==
          persona::PersonaEditorErrorCode::path_escape);

  const auto broad = temporary.path() / "broad";
  REQUIRE(std::filesystem::create_directory(broad));
  std::filesystem::permissions(broad, std::filesystem::perms::owner_all |
                                          std::filesystem::perms::group_read |
                                          std::filesystem::perms::others_read);
  adapters::FilesystemPersonaSource insecure{broad / "personas"};
  const auto denied = insecure.create(
      {{"denied", persona::PersonaFileKind::markdown, "text"}, {}});
  REQUIRE_FALSE(denied);
  REQUIRE(denied.error().code ==
          persona::PersonaEditorErrorCode::permission_denied);
}

TEST_CASE("Venice adapter rejects unsupported content without a request",
          "[adapter][venice][failure]") {
  adapters::VeniceBackend backend{secret("secret"),
                                  {"http://127.0.0.1:1", 10ms, 10ms, 10ms, 4}};
  auto unsupported = context(domain::ArtifactReferenceBlock{
      make_id<domain::ArtifactId>("artifact"), std::nullopt});
  const auto started = backend.start(request(std::move(unsupported)), {});
  REQUIRE_FALSE(started);
  REQUIRE(started.error().kind == backend::BackendErrorKind::request_rejected);
  REQUIRE(started.error().redacted_message.find("secret") == std::string::npos);
}

TEST_CASE("Venice adapter terminates after trailing response accounting",
          "[adapter][venice]") {
  LocalServer server;
  adapters::VeniceBackend backend{secret("test-secret"),
                                  {server.base_url(), 1s, 1s, 1s, 8}};
  auto started = backend.start(request(), {});
  REQUIRE(started);

  std::vector<backend::BackendEvent> events;
  for (;;) {
    auto next = (*started)->next({});
    REQUIRE(next);
    if (!*next) break;
    events.push_back(std::move(**next));
  }

  REQUIRE(events.size() == 5);
  REQUIRE(std::holds_alternative<backend::ResponseStarted>(events[0]));
  REQUIRE(std::holds_alternative<backend::ContentDelta>(events[1]));
  REQUIRE(std::get<backend::ContentDelta>(events[1]).message_id ==
          make_id<domain::MessageId>("assistant"));
  REQUIRE(std::holds_alternative<backend::CostObserved>(events[2]));
  const auto& cost = std::get<backend::CostObserved>(events[2]).cost;
  REQUIRE(cost.amounts().size() == 2);
  REQUIRE(cost.amounts()[0].unit() == "USD");
  REQUIRE(cost.amounts()[0].amount().to_string() == "0");
  REQUIRE(cost.amounts()[1].unit() == "venice.diem");
  REQUIRE(cost.amounts()[1].amount().to_string() == "0.0645375");
  REQUIRE(std::holds_alternative<backend::UsageObserved>(events[3]));
  REQUIRE(std::get<backend::UsageObserved>(events[3]).usage ==
          domain::Usage{2, 1, 0, 0});
  REQUIRE(std::holds_alternative<backend::ResponseFinished>(events[4]));

  const auto sent = nlohmann::json::parse(server.body());
  REQUIRE(sent.at("model") == "test-model");
  REQUIRE(sent.at("messages").at(0).at("content") == "hello");
  REQUIRE(server.authorization() == "Bearer test-secret");
  REQUIRE(server.body().find("test-secret") == std::string::npos);
}

TEST_CASE("Venice adapter maps bounded web-search extensions exactly",
          "[adapter][venice][extensions]") {
  for (const auto& mode :
       {std::string{"auto"}, std::string{"on"}, std::string{"off"}}) {
    CAPTURE(mode);
    LocalServer server;
    adapters::VeniceBackend backend{secret("test-secret"),
                                    {server.base_url(), 1s, 1s, 1s, 8}};
    auto value = request();
    value.options.extensions.emplace(
        std::string{adapters::venice_web_search_extension},
        domain::StructuredDataBlock{"application/json", "\"" + mode + "\""});
    if (mode != "off") {
      value.options.required_model_capabilities.emplace_back(
          adapters::web_search_model_capability);
    }
    auto started = backend.start(std::move(value), {});
    REQUIRE(started);
    while (true) {
      auto next = (*started)->next({});
      REQUIRE(next);
      if (!*next) break;
    }
    const auto sent = nlohmann::json::parse(server.body());
    REQUIRE(sent.at("venice_parameters").at("enable_web_search") == mode);
  }
}

TEST_CASE("Venice adapter maps the explicit system-prompt boolean exactly",
          "[adapter][venice][extensions]") {
  for (const bool include : {true, false}) {
    CAPTURE(include);
    LocalServer server;
    adapters::VeniceBackend backend{secret("test-secret"),
                                    {server.base_url(), 1s, 1s, 1s, 8}};
    auto value = request();
    value.options.extensions.emplace(
        std::string{adapters::venice_system_prompt_extension},
        domain::StructuredDataBlock{"application/json",
                                    include ? "true" : "false"});
    auto started = backend.start(std::move(value), {});
    REQUIRE(started);
    while (true) {
      auto next = (*started)->next({});
      REQUIRE(next);
      if (!*next) break;
    }
    const auto sent = nlohmann::json::parse(server.body());
    REQUIRE(sent.at("venice_parameters").at("include_venice_system_prompt") ==
            include);
  }
}

TEST_CASE("Venice adapter maps a validated character slug exactly",
          "[adapter][venice][extensions][characters]") {
  LocalServer server;
  adapters::VeniceBackend backend{secret("test-secret"),
                                  {server.base_url(), 1s, 1s, 1s, 8}};
  auto value = request();
  value.options.extensions.emplace(
      std::string{adapters::venice_character_slug_extension},
      domain::StructuredDataBlock{"application/json", R"("chosen-slug")"});
  auto started = backend.start(std::move(value), {});
  REQUIRE(started);
  while (true) {
    auto next = (*started)->next({});
    REQUIRE(next);
    if (!*next) break;
  }
  const auto sent = nlohmann::json::parse(server.body());
  REQUIRE(sent.at("venice_parameters").at("character_slug") == "chosen-slug");
}

TEST_CASE("Venice adapter combines both bounded Chat request settings",
          "[adapter][venice][extensions]") {
  LocalServer server;
  adapters::VeniceBackend backend{secret("test-secret"),
                                  {server.base_url(), 1s, 1s, 1s, 8}};
  auto value = request();
  value.options.extensions.emplace(
      std::string{adapters::venice_web_search_extension},
      domain::StructuredDataBlock{"application/json", R"("off")"});
  value.options.extensions.emplace(
      std::string{adapters::venice_system_prompt_extension},
      domain::StructuredDataBlock{"application/json", "false"});
  auto started = backend.start(std::move(value), {});
  REQUIRE(started);
  while (true) {
    auto next = (*started)->next({});
    REQUIRE(next);
    if (!*next) break;
  }
  const auto sent = nlohmann::json::parse(server.body());
  REQUIRE(sent.at("venice_parameters").at("enable_web_search") == "off");
  REQUIRE_FALSE(sent.at("venice_parameters")
                    .at("include_venice_system_prompt")
                    .get<bool>());
}

TEST_CASE("Venice adapter rejects malformed extensions before transport",
          "[adapter][venice][extensions][failure]") {
  std::vector<backend::GenerationOptions> invalid;
  const auto extension = [](std::string name, std::string media_type,
                            std::string data) {
    backend::GenerationOptions options;
    options.extensions.emplace(
        std::move(name),
        domain::StructuredDataBlock{std::move(media_type), std::move(data)});
    return options;
  };
  invalid.push_back(
      extension("other.chat.web-search", "application/json", R"("on")"));
  invalid.push_back(
      extension("venice.media.safe-mode", "application/json", "true"));
  invalid.push_back(
      extension(std::string{adapters::venice_web_search_extension},
                "text/plain", R"("on")"));
  invalid.push_back(
      extension(std::string{adapters::venice_web_search_extension},
                "application/json", "{"));
  invalid.push_back(
      extension(std::string{adapters::venice_web_search_extension},
                "application/json", "true"));
  invalid.push_back(
      extension(std::string{adapters::venice_web_search_extension},
                "application/json", R"("sometimes")"));
  invalid.push_back(
      extension(std::string{adapters::venice_web_search_extension},
                "application/json", std::string(17, 'x')));
  invalid.push_back(
      extension(std::string{adapters::venice_system_prompt_extension},
                "application/json", R"("true")"));
  invalid.push_back(
      extension(std::string{adapters::venice_system_prompt_extension},
                "text/plain", "true"));
  invalid.push_back(
      extension(std::string{adapters::venice_character_slug_extension},
                "text/plain", R"("chosen")"));
  invalid.push_back(
      extension(std::string{adapters::venice_character_slug_extension},
                "application/json", "{"));
  invalid.push_back(
      extension(std::string{adapters::venice_character_slug_extension},
                "application/json", "true"));
  invalid.push_back(
      extension(std::string{adapters::venice_character_slug_extension},
                "application/json", R"("")"));
  invalid.push_back(
      extension(std::string{adapters::venice_character_slug_extension},
                "application/json", R"("unsafe\u202evalue")"));
  invalid.push_back(
      extension(std::string{adapters::venice_character_slug_extension},
                "application/json", R"("has space")"));
  invalid.push_back(
      extension(std::string{adapters::venice_character_slug_extension},
                "application/json", R"("has\"quote")"));
  invalid.push_back(
      extension(std::string{adapters::venice_character_slug_extension},
                "application/json", R"("has\\slash")"));
  auto malformed_utf8 = std::string{"\"bad"};
  malformed_utf8.push_back(static_cast<char>(0xFF));
  malformed_utf8.push_back('"');
  invalid.push_back(
      extension(std::string{adapters::venice_character_slug_extension},
                "application/json", std::move(malformed_utf8)));
  invalid.push_back(
      extension(std::string{adapters::venice_character_slug_extension},
                "application/json", '"' + std::string(257, 'x') + '"'));
  invalid.push_back(
      extension(std::string{adapters::venice_web_search_extension},
                "application/json", R"("on")"));
  auto inconsistent_off =
      extension(std::string{adapters::venice_web_search_extension},
                "application/json", R"("off")");
  inconsistent_off.required_model_capabilities.emplace_back(
      adapters::web_search_model_capability);
  invalid.push_back(std::move(inconsistent_off));
  auto multiple_extensions =
      extension(std::string{adapters::venice_web_search_extension},
                "application/json", R"("off")");
  multiple_extensions.extensions.emplace(
      "other.extension",
      domain::StructuredDataBlock{"application/json", "null"});
  invalid.push_back(std::move(multiple_extensions));
  backend::GenerationOptions excessive_requirements;
  excessive_requirements.required_model_capabilities.resize(65, "unused");
  invalid.push_back(std::move(excessive_requirements));

  for (auto& options : invalid) {
    LocalServer server;
    adapters::VeniceBackend backend{secret("test-secret"),
                                    {server.base_url(), 1s, 1s, 1s, 8}};
    auto value = request();
    value.options = std::move(options);
    const auto started = backend.start(std::move(value), {});
    REQUIRE_FALSE(started);
    REQUIRE(started.error().kind ==
            backend::BackendErrorKind::request_rejected);
    REQUIRE(server.body().empty());
  }
}

TEST_CASE("Venice adapter captures opaque assistant continuation state",
          "[adapter][venice][reasoning]") {
  LocalServer server{std::nullopt, false, "0.0645375", false, false, true};
  adapters::VeniceBackend backend{secret("test-secret"),
                                  {server.base_url(), 1s, 1s, 1s, 8}};
  auto started = backend.start(request(), {});
  REQUIRE(started);

  std::optional<std::string> reasoning_text;
  domain::Metadata reasoning_metadata;
  for (;;) {
    auto next = (*started)->next({});
    REQUIRE(next);
    if (!*next) break;
    if (const auto* observed = std::get_if<backend::ReasoningDelta>(&**next)) {
      if (observed->text) reasoning_text = observed->text;
      reasoning_metadata.insert(reasoning_metadata.end(),
                                observed->metadata.begin(),
                                observed->metadata.end());
    }
  }

  REQUIRE(reasoning_text == "plan");
  REQUIRE(
      reasoning_metadata ==
      domain::Metadata{
          {"application/vnd.venice.reasoning-"
           "details+json",
           R"([{"text":"opaque","type":"reasoning.text"}])"},
          {"application/vnd.venice.thought-signature", "signature"},
          {"application/vnd.venice.tool-thought-signature+json",
           R"({"invocation_id":"lookup-call","thought_signature":"tool-signature"})"},
      });
}

TEST_CASE("Venice adapter rejects duplicate provider cost frames",
          "[adapter][venice][cost][failure]") {
  LocalServer server{std::nullopt, true};
  adapters::VeniceBackend backend{secret("test-secret"),
                                  {server.base_url(), 1s, 1s, 1s, 8}};
  auto started = backend.start(request(), {});
  REQUIRE(started);

  std::optional<backend::BackendError> failure;
  for (;;) {
    auto next = (*started)->next({});
    if (!next) {
      failure = next.error();
      break;
    }
    if (!*next) break;
  }
  REQUIRE(failure);
  REQUIRE(failure->kind == backend::BackendErrorKind::protocol);
  REQUIRE(failure->redacted_message == "Venice stream repeated provider cost");
}

TEST_CASE("Venice adapter rejects duplicate provider finish frames",
          "[adapter][venice][failure]") {
  LocalServer server{std::nullopt, false, "0.0645375", true};
  adapters::VeniceBackend backend{secret("test-secret"),
                                  {server.base_url(), 1s, 1s, 1s, 8}};
  auto started = backend.start(request(), {});
  REQUIRE(started);

  std::optional<backend::BackendError> failure;
  for (;;) {
    auto next = (*started)->next({});
    if (!next) {
      failure = next.error();
      break;
    }
    if (!*next) break;
  }
  REQUIRE(failure);
  REQUIRE(failure->kind == backend::BackendErrorKind::protocol);
  REQUIRE(failure->redacted_message ==
          "Venice stream repeated its finish marker");
}

TEST_CASE("Venice adapter rejects streams without a finish marker",
          "[adapter][venice][failure]") {
  LocalServer server{std::nullopt, false, "0.0645375", false, true};
  adapters::VeniceBackend backend{secret("test-secret"),
                                  {server.base_url(), 1s, 1s, 1s, 8}};
  auto started = backend.start(request(), {});
  REQUIRE(started);

  std::optional<backend::BackendError> failure;
  for (;;) {
    auto next = (*started)->next({});
    if (!next) {
      failure = next.error();
      break;
    }
    if (!*next) break;
  }
  REQUIRE(failure);
  REQUIRE(failure->kind == backend::BackendErrorKind::protocol);
  REQUIRE(failure->redacted_message ==
          "Venice stream omitted its finish marker");
}

TEST_CASE("Venice adapter rejects invalid provider cost values",
          "[adapter][venice][cost][failure]") {
  for (const auto* value : {"-1", "\"invalid\""}) {
    CAPTURE(value);
    LocalServer server{std::nullopt, false, value};
    adapters::VeniceBackend backend{secret("test-secret"),
                                    {server.base_url(), 1s, 1s, 1s, 8}};
    auto started = backend.start(request(), {});
    REQUIRE(started);

    std::optional<backend::BackendError> failure;
    for (;;) {
      auto next = (*started)->next({});
      if (!next) {
        failure = next.error();
        break;
      }
      if (!*next) break;
    }
    REQUIRE(failure);
    REQUIRE(failure->kind == backend::BackendErrorKind::protocol);
    REQUIRE(failure->redacted_message ==
            (std::string_view{value} == "-1"
                 ? "Venice stream reported an invalid cost amount"
                 : "Venice stream cost field was not numeric"));
  }
}

TEST_CASE("Venice adapter bounds provider cost decimal precision",
          "[adapter][venice][cost]") {
  LocalServer server{std::nullopt, false, "0.0012345678901234567"};
  adapters::VeniceBackend backend{secret("test-secret"),
                                  {server.base_url(), 1s, 1s, 1s, 8}};
  auto started = backend.start(request(), {});
  REQUIRE(started);

  std::optional<domain::ReportedCost> cost;
  for (;;) {
    auto next = (*started)->next({});
    REQUIRE(next);
    if (!*next) break;
    if (const auto* observed = std::get_if<backend::CostObserved>(&**next)) {
      cost = observed->cost;
    }
  }
  REQUIRE(cost);
  REQUIRE(cost->amounts().size() == 2);
  REQUIRE(cost->amounts()[1].unit() == "venice.diem");
  REQUIRE(cost->amounts()[1].amount().to_string() == "0.001234567890123457");
}

TEST_CASE("Venice adapter sends JSON structured tool results as tool messages",
          "[adapter][venice][tools]") {
  for (const auto* media_type : {
           "application/json",
           "application/vnd.aiforge.process-result+json",
       }) {
    CAPTURE(media_type);
    LocalServer server;
    adapters::VeniceBackend backend{secret("test-secret"),
                                    {server.base_url(), 1s, 1s, 1s, 8}};
    auto built = context();
    built.entries.front().kind = domain::ContextEntryKind::tool_result;
    built.entries.front().message = {
        make_id<domain::MessageId>("tool-result"),
        domain::Role::tool,
        {domain::StructuredDataBlock{media_type, R"({"status":"cancelled"})"}},
        make_id<domain::InvocationId>("ask-call")};
    auto started = backend.start(request(std::move(built)), {});
    REQUIRE(started);
    while (true) {
      auto next = (*started)->next({});
      REQUIRE(next);
      if (!*next) break;
    }
    const auto sent = nlohmann::json::parse(server.body());
    REQUIRE(sent.at("messages").at(0).at("role") == "tool");
    REQUIRE(sent.at("messages").at(0).at("tool_call_id") == "ask-call");
    REQUIRE(sent.at("messages").at(0).at("content") ==
            R"({"status":"cancelled"})");
  }
}

TEST_CASE("Venice adapter rejects unsupported structured tool results",
          "[adapter][venice][tools][failure]") {
  const std::vector<std::pair<domain::StructuredDataBlock, std::string>> cases{
      {{"application/vnd.aiforge.process-result+json", "{"},
       "Venice adapter received malformed JSON tool content"},
      {{"text/plain", "complete"},
       "Venice adapter does not support this input content block"},
  };
  for (const auto& [content, expected] : cases) {
    CAPTURE(content.media_type);
    adapters::VeniceBackend backend{
        secret("test-secret"), {"http://127.0.0.1:1", 10ms, 10ms, 10ms, 4}};
    auto built = context();
    built.entries.front().kind = domain::ContextEntryKind::tool_result;
    built.entries.front().message = {
        make_id<domain::MessageId>("tool-result"),
        domain::Role::tool,
        {content},
        make_id<domain::InvocationId>("process-call")};

    const auto started = backend.start(request(std::move(built)), {});

    REQUIRE_FALSE(started);
    REQUIRE(started.error().kind ==
            backend::BackendErrorKind::request_rejected);
    REQUIRE(started.error().redacted_message == expected);
  }
}

TEST_CASE("Venice adapter rejects tool calls outside assistant messages",
          "[adapter][venice][tools][failure]") {
  adapters::VeniceBackend backend{secret("test-secret"),
                                  {"http://127.0.0.1:1", 10ms, 10ms, 10ms, 4}};
  auto built = context();
  built.entries.front().message.tool_calls.push_back(
      {make_id<domain::InvocationId>("ask-call"),
       "ask_user",
       {"application/json", R"({"question":"Continue?"})"}});

  const auto started = backend.start(request(std::move(built)), {});

  REQUIRE_FALSE(started);
  REQUIRE(started.error().kind == backend::BackendErrorKind::request_rejected);
}

TEST_CASE("Venice adapter replays assistant tool calls before tool results",
          "[adapter][venice][tools]") {
  LocalServer server;
  adapters::VeniceBackend backend{secret("test-secret"),
                                  {server.base_url(), 1s, 1s, 1s, 8}};
  auto built = context();
  built.entries.front().message = {
      make_id<domain::MessageId>("assistant-tool"),
      domain::Role::assistant,
      {},
      std::nullopt,
      {{make_id<domain::InvocationId>("ask-call"),
        "ask_user",
        {"application/json", R"({"question":"Continue?"})"}}}};
  built.entries.push_back({make_id<domain::ContextEntryId>("tool-result-entry"),
                           domain::ContextEntryKind::tool_result,
                           std::nullopt,
                           {make_id<domain::MessageId>("tool-result"),
                            domain::Role::tool,
                            {domain::StructuredDataBlock{
                                "application/json", R"({"answer":"yes"})"}},
                            make_id<domain::InvocationId>("ask-call")},
                           {make_id<domain::ContextSourceId>("tool-source"),
                            std::nullopt, std::nullopt},
                           0,
                           2,
                           1});
  auto started = backend.start(request(std::move(built)), {});
  REQUIRE(started);
  while (true) {
    auto next = (*started)->next({});
    REQUIRE(next);
    if (!*next) break;
  }

  const auto sent = nlohmann::json::parse(server.body());
  REQUIRE(sent.at("messages").size() == 2);
  REQUIRE(sent.at("messages").at(0).at("role") == "assistant");
  REQUIRE(sent.at("messages").at(0).at("content") == "");
  REQUIRE(sent.at("messages").at(0).at("tool_calls").at(0).at("id") ==
          "ask-call");
  REQUIRE(sent.at("messages")
              .at(0)
              .at("tool_calls")
              .at(0)
              .at("function")
              .at("name") == "ask_user");
  REQUIRE(sent.at("messages")
              .at(0)
              .at("tool_calls")
              .at(0)
              .at("function")
              .at("arguments") == R"({"question":"Continue?"})");
  REQUIRE(sent.at("messages").at(1).at("role") == "tool");
  REQUIRE(sent.at("messages").at(1).at("tool_call_id") == "ask-call");
}

TEST_CASE("Venice adapter replays message-bound reasoning state",
          "[adapter][venice][reasoning]") {
  LocalServer server;
  adapters::VeniceBackend backend{secret("test-secret"),
                                  {server.base_url(), 1s, 1s, 1s, 8}};
  auto built = context();
  built.entries.front().message.role = domain::Role::assistant;
  built.entries.front().message.tool_calls.push_back(
      {make_id<domain::InvocationId>("lookup-call"),
       "lookup",
       {"application/json", "{}"}});
  auto backend_request = request(std::move(built));
  const auto message_id =
      backend_request.context.entries.front().message.message_id;
  backend_request.assistant_continuation_state.push_back({
      message_id,
      "plan",
      {{"application/vnd.venice.reasoning-details+json",
        R"([{"type":"reasoning.text","text":"opaque"}])"},
       {"application/vnd.venice.thought-signature", "signature"},
       {"application/vnd.venice.tool-thought-signature+json",
        R"({"invocation_id":"lookup-call","thought_signature":"tool-signature"})"}},
  });

  auto started = backend.start(std::move(backend_request), {});
  REQUIRE(started);
  while (true) {
    auto next = (*started)->next({});
    REQUIRE(next);
    if (!*next) break;
  }

  const auto sent = nlohmann::json::parse(server.body());
  const auto& message = sent.at("messages").front();
  REQUIRE(message.at("reasoning_content") == "plan");
  REQUIRE(message.at("reasoning_details") ==
          nlohmann::json::array(
              {{{"type", "reasoning.text"}, {"text", "opaque"}}}));
  REQUIRE(message.at("thought_signature") == "signature");
  REQUIRE(message.at("tool_calls").front().at("thought_signature") ==
          "tool-signature");
}

TEST_CASE("Venice adapter rejects malformed assistant continuation state",
          "[adapter][venice][reasoning][failure]") {
  adapters::VeniceBackend backend{secret("test-secret"),
                                  {"http://127.0.0.1:1", 10ms, 10ms, 10ms, 4}};
  auto make_request = [] {
    auto built = context();
    built.entries.front().message.role = domain::Role::assistant;
    return request(std::move(built));
  };

  SECTION("orphaned message") {
    auto value = make_request();
    value.assistant_continuation_state.push_back(
        {make_id<domain::MessageId>("missing"), "plan", {}});
    const auto started = backend.start(std::move(value), {});
    REQUIRE_FALSE(started);
    REQUIRE(started.error().kind ==
            backend::BackendErrorKind::request_rejected);
  }
  SECTION("duplicate message") {
    auto value = make_request();
    const auto message_id = value.context.entries.front().message.message_id;
    value.assistant_continuation_state.push_back({message_id, "one", {}});
    value.assistant_continuation_state.push_back({message_id, "two", {}});
    const auto started = backend.start(std::move(value), {});
    REQUIRE_FALSE(started);
    REQUIRE(started.error().kind ==
            backend::BackendErrorKind::request_rejected);
  }
  SECTION("malformed reasoning details") {
    auto value = make_request();
    value.assistant_continuation_state.push_back(
        {value.context.entries.front().message.message_id,
         std::nullopt,
         {{"application/vnd.venice.reasoning-details+json", "{"}}});
    const auto started = backend.start(std::move(value), {});
    REQUIRE_FALSE(started);
    REQUIRE(started.error().kind ==
            backend::BackendErrorKind::request_rejected);
  }
  SECTION("unknown metadata type") {
    auto value = make_request();
    value.assistant_continuation_state.push_back(
        {value.context.entries.front().message.message_id,
         std::nullopt,
         {{"application/vnd.example.unknown", "opaque"}}});
    const auto started = backend.start(std::move(value), {});
    REQUIRE_FALSE(started);
    REQUIRE(started.error().kind ==
            backend::BackendErrorKind::request_rejected);
  }
  SECTION("duplicate thought signature") {
    auto value = make_request();
    value.assistant_continuation_state.push_back(
        {value.context.entries.front().message.message_id,
         std::nullopt,
         {{"application/vnd.venice.thought-signature", "one"},
          {"application/vnd.venice.thought-signature", "two"}}});
    const auto started = backend.start(std::move(value), {});
    REQUIRE_FALSE(started);
    REQUIRE(started.error().kind ==
            backend::BackendErrorKind::request_rejected);
  }
  SECTION("orphaned tool thought signature") {
    auto value = make_request();
    value.assistant_continuation_state.push_back(
        {value.context.entries.front().message.message_id,
         std::nullopt,
         {{"application/vnd.venice.tool-thought-signature+json",
           R"({"invocation_id":"missing","thought_signature":"signature"})"}}});
    const auto started = backend.start(std::move(value), {});
    REQUIRE_FALSE(started);
    REQUIRE(started.error().kind ==
            backend::BackendErrorKind::request_rejected);
  }
}

TEST_CASE("Venice adapter exposes neutral model context metadata",
          "[adapter][venice][models]") {
  LocalServer server;
  adapters::VeniceBackend backend{secret("test-secret"),
                                  {server.base_url(), 1s, 1s, 1s, 8}};
  const auto model_id = make_id<domain::ModelId>("test-model");
  const auto context = backend.lookup(model_id, {});
  REQUIRE(context);
  REQUIRE(context->model_id == model_id);
  REQUIRE(context->context_window_tokens == 8192);
  REQUIRE(context->maximum_output_tokens == 1024);
  REQUIRE(context->capabilities.at("web-search") == true);
  REQUIRE_FALSE(context->capabilities.at("reasoning").has_value());

  const auto missing =
      backend.lookup(make_id<domain::ModelId>("missing-model"), {});
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().kind == backend::BackendErrorKind::request_rejected);
  REQUIRE(missing.error().redacted_message.find("test-secret") ==
          std::string::npos);
}

TEST_CASE("Venice adapter redacts provider bodies containing credentials",
          "[adapter][venice][failure][redaction]") {
  const std::string credential{"provider-echoed-secret"};
  LocalServer server{credential};
  adapters::VeniceBackend backend{secret(credential),
                                  {server.base_url(), 1s, 1s, 1s, 8}};
  auto started = backend.start(request(), {});
  REQUIRE(started);

  auto next = (*started)->next({});
  REQUIRE_FALSE(next);
  REQUIRE(next.error().kind == backend::BackendErrorKind::authentication);
  REQUIRE(next.error().redacted_message == "Venice authentication failed");
  REQUIRE(next.error().redacted_message.find(credential) == std::string::npos);
  REQUIRE(request().context.entries.front().message.content ==
          std::vector<domain::ContentBlock>{domain::TextBlock{"hello"}});
}

TEST_CASE("TermForge bridge converts its marker to an owner-thread drain",
          "[adapter][termforge]") {
  auto backend_request = request();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{
          {backend::ResponseStarted{"response"},
           backend::ResponseFinished{domain::FinishReason::stop},
           testing::EndOfStream{}}}}}};
  TestApp app;
  adapters::TermForgeRunBridge bridge{app};
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake,
                            &bridge};
  const runtime::RunStart start{
      make_id<domain::RunId>("run"),
      {make_id<domain::SurfaceId>("tui"), make_id<domain::WorkspaceId>("chat"),
       make_id<domain::PermissionProfileId>("observe"), std::nullopt},
      {make_id<domain::MessageId>("user"),
       domain::Role::user,
       {domain::TextBlock{"hello"}},
       std::nullopt},
      backend_request};
  REQUIRE(kernel.start(start));

  const termforge::Event marker = termforge::ErrorEvent{
      termforge::Severity::Info, "aiforge.runtime", "events-ready"};
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (kernel.active_run_id() &&
         std::chrono::steady_clock::now() < deadline) {
    auto handled = bridge.handle(marker, kernel);
    REQUIRE(handled);
    std::this_thread::sleep_for(1ms);
  }
  REQUIRE_FALSE(kernel.active_run_id());
  REQUIRE(kernel.projection(make_id<domain::RunId>("run"))->status() ==
          domain::RunStatus::completed);
}

TEST_CASE("ask_user dialog maps recommended indices back to stable IDs",
          "[adapter][termforge][questions]") {
  runtime::ToolRegistry registry;
  REQUIRE(runtime::register_ask_user_tool(registry, true));
  auto snapshot = registry.snapshot();
  REQUIRE(snapshot);
  const auto invocation = make_id<domain::InvocationId>("ask-call");
  auto backend_request = request();
  backend_request.tools = snapshot->declarations();
  testing::ScriptedBackend fake{{testing::ScriptedExchange{
      backend_request,
      testing::StreamScript{{
          backend::ResponseStarted{"response"},
          backend::ToolCallDelta{
              invocation, "ask_user",
              R"({"questions":[{"id":"format","prompt":"Choose output","kind":"one","required":true,"minimum_selections":1,"maximum_selections":1,"options":[{"id":"short","label":"Short","recommended":true},{"id":"long","label":"Long"}]},{"id":"confirm","prompt":"Confirm choice","kind":"one","required":true,"minimum_selections":1,"maximum_selections":1,"options":[{"id":"yes","label":"Yes","recommended":true},{"id":"no","label":"No"}]},{"id":"optional","prompt":"Optional detail","kind":"one","required":false,"minimum_selections":0,"maximum_selections":1,"options":[{"id":"extra","label":"Extra"}]}]})"},
          backend::ResponseFinished{domain::FinishReason::tool_call},
          testing::EndOfStream{},
      }}}}};
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"),
                            fake,
                            nullptr,
                            {},
                            {},
                            std::move(*snapshot)};
  REQUIRE(kernel.start(
      {make_id<domain::RunId>("run"),
       {make_id<domain::SurfaceId>("tui"), make_id<domain::WorkspaceId>("chat"),
        make_id<domain::PermissionProfileId>("observe"), std::nullopt},
       {make_id<domain::MessageId>("user"),
        domain::Role::user,
        {domain::TextBlock{"hello"}},
        std::nullopt},
       backend_request}));
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (!kernel.pending_question_input() &&
         std::chrono::steady_clock::now() < deadline) {
    REQUIRE(kernel.drain());
    std::this_thread::sleep_for(1ms);
  }
  const auto pending = kernel.pending_question_input();
  REQUIRE(pending);

  termforge::ChoiceWizardDialog dialog;
  adapters::AskUserDialogController controller{dialog};
  REQUIRE(controller.present(*pending, kernel));
  termforge::Screen tiny{8, 3};
  dialog.draw(tiny);
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Enter, 0, false,
                                              false, false,
                                              termforge::KeyAction::Press}));
  REQUIRE(dialog.current_page() == 1);
  REQUIRE(kernel.pending_question_input());
  tiny.resize(1, 1);
  dialog.draw(tiny);
  tiny.resize(40, 12);
  dialog.draw(tiny);
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Enter, 0, false,
                                              false, false,
                                              termforge::KeyAction::Press}));
  REQUIRE(dialog.current_page() == 2);
  REQUIRE(kernel.pending_question_input());
  REQUIRE(dialog.on_event(termforge::KeyEvent{termforge::Key::Enter, 0, false,
                                              false, false,
                                              termforge::KeyAction::Press}));
  REQUIRE_FALSE(controller.last_error());
  REQUIRE_FALSE(kernel.pending_question_input());

  const auto messages =
      runtime::tool_result_messages(kernel.event_log().events());
  REQUIRE(messages);
  REQUIRE(messages->size() == 1);
  const auto& result =
      std::get<domain::StructuredDataBlock>(messages->front().content.front());
  REQUIRE(result.data.find("short") != std::string::npos);
  REQUIRE(result.data.find("yes") != std::string::npos);
  static_cast<void>(dialog.on_event(
      termforge::KeyEvent{termforge::Key::Enter, 0, false, false, false,
                          termforge::KeyAction::Press}));
  const auto after_duplicate =
      runtime::tool_result_messages(kernel.event_log().events());
  REQUIRE(after_duplicate);
  REQUIRE(after_duplicate->size() == 1);
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
}

TEST_CASE("resolved Venice web-search config becomes a bounded extension",
          "[adapter][venice][extensions][config]") {
  for (const auto& mode :
       {std::string{"auto"}, std::string{"on"}, std::string{"off"}}) {
    const config::ConfigLayer layer{
        config::ConfigSource::command_line,
        {{"venice.web_search", config::ConfigValue{mode}, std::nullopt}},
        {}};
    const std::array layers{layer};
    const auto resolved =
        config::resolve_config(config::builtin_config_registry(), layers);
    REQUIRE(resolved);
    const auto options = adapters::venice_generation_options(*resolved);
    REQUIRE(options);
    REQUIRE(
        options->extensions.at(
            std::string{adapters::venice_web_search_extension}) ==
        domain::StructuredDataBlock{"application/json", "\"" + mode + "\""});
    REQUIRE(options->required_model_capabilities.empty() == (mode == "off"));
  }

  const config::ConfigLayer file{
      config::ConfigSource::file,
      {{"venice.web_search", config::ConfigValue{std::string{"off"}},
        std::nullopt}},
      {}};
  const config::ConfigLayer environment{
      config::ConfigSource::environment,
      {{"venice.web_search", config::ConfigValue{std::string{"auto"}},
        std::nullopt}},
      {}};
  const config::ConfigLayer command_line{
      config::ConfigSource::command_line,
      {{"venice.web_search", config::ConfigValue{std::string{"on"}},
        std::nullopt}},
      {}};
  const std::array precedence_layers{file, environment, command_line};
  const auto precedence = config::resolve_config(
      config::builtin_config_registry(), precedence_layers);
  REQUIRE(precedence);
  const auto precedence_options =
      adapters::venice_generation_options(*precedence);
  REQUIRE(precedence_options);
  REQUIRE(precedence_options->extensions.at(
              std::string{adapters::venice_web_search_extension}) ==
          domain::StructuredDataBlock{"application/json", R"("on")"});

  const config::ResolvedConfig invalid{
      {{"venice.web_search",
        config::ConfigValue{std::string{"sometimes"}},
        config::ConfigSource::file,
        false,
        {}}},
      {}};
  REQUIRE_FALSE(adapters::venice_generation_options(invalid));
}

TEST_CASE("resolved Venice chat settings preserve inherit and explicit false",
          "[adapter][venice][extensions][config][provenance]") {
  const config::ConfigLayer layer{config::ConfigSource::file,
                                  {{"venice.include_system_prompt",
                                    config::ConfigValue{false}, std::nullopt}},
                                  {}};
  const std::array layers{layer};
  const auto resolved =
      config::resolve_config(config::builtin_config_registry(), layers);
  REQUIRE(resolved);
  const auto configured =
      adapters::venice_configured_request_settings(*resolved);
  REQUIRE(configured);
  REQUIRE(configured->system_prompt ==
          adapters::VeniceSystemPromptSetting::exclude);
  REQUIRE(configured->system_prompt_source == config::ConfigSource::file);

  const auto options = adapters::venice_generation_options(*configured);
  REQUIRE(options);
  REQUIRE(options->extensions.at(
              std::string{adapters::venice_system_prompt_extension}) ==
          domain::StructuredDataBlock{"application/json", "false"});
  REQUIRE_FALSE(options->extensions.contains(
      std::string{adapters::venice_web_search_extension}));

  const auto snapshot = adapters::venice_effective_request_options(*configured);
  REQUIRE(snapshot);
  REQUIRE(*snapshot ==
          std::vector<domain::EffectiveRequestOption>{
              {"venice.chat.web-search", std::nullopt,
               domain::RequestOptionSource::provider_default},
              {"venice.chat.include-system-prompt", std::string{"false"},
               domain::RequestOptionSource::configuration},
              {"venice.chat.character-slug", std::nullopt,
               domain::RequestOptionSource::provider_default}});

  const adapters::VeniceRequestSettingOverrides overrides{
      .web_search = adapters::VeniceWebSearchSetting::off,
      .system_prompt = adapters::VeniceSystemPromptSetting::include,
      .character_slug = make_id<domain::ProviderCharacterId>("Az09._~-")};
  const auto generated =
      adapters::venice_generation_options(*configured, overrides);
  REQUIRE(generated);
  REQUIRE(generated->extensions.at(
              std::string{adapters::venice_character_slug_extension}) ==
          domain::StructuredDataBlock{"application/json", R"("Az09._~-")"});
  const auto overridden =
      adapters::venice_effective_request_options(*configured, overrides);
  REQUIRE(overridden);
  REQUIRE(overridden->front().value == "off");
  REQUIRE(overridden->front().source ==
          domain::RequestOptionSource::session_override);
  REQUIRE(overridden->at(1).value == "true");
  REQUIRE(overridden->at(1).source ==
          domain::RequestOptionSource::session_override);
  REQUIRE(overridden->back().value == "Az09._~-");
  REQUIRE(overridden->back().source ==
          domain::RequestOptionSource::session_override);
}

TEST_CASE("invalid closed Venice request setting values fail closed",
          "[adapter][venice][extensions][failure]") {
  adapters::VeniceConfiguredRequestSettings configured;
  configured.web_search = static_cast<adapters::VeniceWebSearchSetting>(999);
  REQUIRE_FALSE(adapters::venice_generation_options(configured));
  REQUIRE_FALSE(adapters::venice_effective_request_options(configured));

  configured.web_search = adapters::VeniceWebSearchSetting::inherit;
  configured.system_prompt =
      static_cast<adapters::VeniceSystemPromptSetting>(999);
  REQUIRE_FALSE(adapters::venice_generation_options(configured));
  REQUIRE_FALSE(adapters::venice_effective_request_options(configured));

  configured.system_prompt = adapters::VeniceSystemPromptSetting::inherit;
  const std::array invalid_slugs{std::string{"has space"},
                                 std::string{"has\"quote"},
                                 std::string{"has\\slash"},
                                 std::string{"has\xE2\x80\xAE"
                                             "format"}};
  for (const auto& slug : invalid_slugs) {
    const auto id = domain::ProviderCharacterId::from(slug);
    REQUIRE(id);
    const adapters::VeniceRequestSettingOverrides overrides{
        .web_search = std::nullopt,
        .system_prompt = std::nullopt,
        .character_slug = *id};
    REQUIRE_FALSE(adapters::venice_generation_options(configured, overrides));
    REQUIRE_FALSE(
        adapters::venice_effective_request_options(configured, overrides));
  }
}

TEST_CASE("process provenance names a credential source without its secret",
          "[adapters][provenance][failure]") {
  const std::string secret{"venice-secret-value-do-not-persist"};
  const config::ConfigRegistry registry{
      {{"model", config::ConfigValueKind::text, "AIFORGE_MODEL",
        config::ConfigValue{"default"}, false, true, 64, 4},
       {"credential", config::ConfigValueKind::text, "SECRET_TOKEN",
        std::nullopt, true, false, 64, 4}}};
  const config::ConfigLayer environment{
      config::ConfigSource::environment,
      {{"model", config::ConfigValue{std::string{"venice-model"}},
        std::nullopt},
       {"credential", config::ConfigValue{secret}, std::nullopt}},
      {}};
  const std::array layers{environment};
  const auto resolved = config::resolve_config(registry, layers);
  REQUIRE(resolved);
  REQUIRE(std::get<std::string>(*resolved->find("credential")->value) ==
          secret);

  const auto provenance = adapters::process_run_provenance(
      *resolved, make_id<domain::ModelId>("venice-model"), "venice",
      domain::CredentialSourceReference{
          domain::CredentialSourceKind::environment, "VENICE_API_KEY"});
  REQUIRE(domain::validate_run_provenance(provenance));
  REQUIRE(provenance.backend_id == "venice");
  REQUIRE_FALSE(provenance.aiforge_version.empty());
  // Tool identity stays empty here; the run kernel owns it.
  REQUIRE(provenance.tools.empty());
  REQUIRE(std::ranges::any_of(provenance.components, [](const auto& component) {
    return component.component == "aiforge";
  }));
  const auto credential =
      std::ranges::find(provenance.configuration, "credential",
                        &domain::ConfigurationProvenanceEntry::key);
  REQUIRE(credential != provenance.configuration.end());
  REQUIRE(credential->sensitive);
  REQUIRE(credential->value_present);
  REQUIRE_FALSE(credential->value.has_value());

  // Through the real store: the secret is absent from the database bytes.
  auto pattern =
      (std::filesystem::temp_directory_path() / "aiforge-provenance-XXXXXX")
          .string();
  pattern.push_back('\0');
  const auto* created = ::mkdtemp(pattern.data());
  REQUIRE(created != nullptr);
  const std::filesystem::path directory{created};
  const auto path = directory / "aiforge" / "sessions.sqlite3";
  {
    auto store = adapters::SqliteSessionStore::open(path);
    REQUIRE(store);
    const auto session = make_id<domain::SessionId>("session");
    REQUIRE((*store)->create_session(
        {session, domain::EventTimestamp{std::chrono::milliseconds{100}}}));
    const std::array events{
        domain::RunEvent{
            {make_id<domain::EventId>("e1"), make_id<domain::RunId>("run"), 1,
             1, domain::EventTimestamp{std::chrono::milliseconds{101}},
             std::nullopt, std::nullopt, std::nullopt},
            domain::RunStarted{make_id<domain::SurfaceId>("one-shot"),
                               make_id<domain::WorkspaceId>("chat"),
                               make_id<domain::PermissionProfileId>("observe"),
                               std::nullopt}},
        domain::RunEvent{
            {make_id<domain::EventId>("e2"), make_id<domain::RunId>("run"), 2,
             1, domain::EventTimestamp{std::chrono::milliseconds{102}},
             std::nullopt, std::nullopt, std::nullopt},
            domain::RunProvenanceRecorded{provenance}}};
    REQUIRE((*store)->append_events(session, events));
    const auto replayed = (*store)->replay_events(session);
    REQUIRE(replayed);
    REQUIRE(*replayed == std::vector<domain::RunEvent>{events[0], events[1]});
  }
  std::ifstream database{path, std::ios::binary};
  REQUIRE(database);
  const std::string bytes{std::istreambuf_iterator<char>{database},
                          std::istreambuf_iterator<char>{}};
  REQUIRE(bytes.find(secret) == std::string::npos);
  REQUIRE(bytes.find("VENICE_API_KEY") != std::string::npos);
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("process credential resolution uses environment then XDG storage",
          "[adapters][credentials][failure]") {
  EnvironmentGuard key_guard{"VENICE_API_KEY"};
  EnvironmentGuard xdg_guard{"XDG_CONFIG_HOME"};
  EnvironmentGuard home_guard{"HOME"};
  TempDirectory temporary{"aiforge-process-credential"};
  REQUIRE(::setenv("XDG_CONFIG_HOME", temporary.path().c_str(), 1) == 0);
  REQUIRE(::unsetenv("VENICE_API_KEY") == 0);

  const auto path = temporary.path() / "aiforge" / "credentials";
  credentials::FileCredentialStore store{path};
  auto stored = secret("stored-process-secret");
  REQUIRE(store.store(stored));
  std::ostringstream diagnostics;
  auto resolved = adapters::resolve_process_credential(diagnostics);
  REQUIRE(resolved);
  REQUIRE(resolved->credential);
  REQUIRE(resolved->credential->secret.view() == "stored-process-secret");
  REQUIRE(resolved->credential->source.kind ==
          domain::CredentialSourceKind::configuration_file);
  REQUIRE(diagnostics.str().empty());

  REQUIRE(::setenv("VENICE_API_KEY", "environment-process-secret", 1) == 0);
  resolved = adapters::resolve_process_credential(diagnostics);
  REQUIRE(resolved);
  REQUIRE(resolved->credential);
  REQUIRE(resolved->credential->secret.view() == "environment-process-secret");
  REQUIRE(resolved->credential->source.kind ==
          domain::CredentialSourceKind::environment);

  REQUIRE(::setenv("VENICE_API_KEY", "invalid value", 1) == 0);
  resolved = adapters::resolve_process_credential(diagnostics);
  REQUIRE_FALSE(resolved);
  REQUIRE(resolved.error().message == "VENICE_API_KEY is invalid");
  REQUIRE(resolved.error().message.find("invalid value") == std::string::npos);

  REQUIRE(::unsetenv("VENICE_API_KEY") == 0);
  REQUIRE(::chmod(path.c_str(), 0644) == 0);
  diagnostics.str({});
  resolved = adapters::resolve_process_credential(diagnostics);
  REQUIRE(resolved);
  REQUIRE_FALSE(resolved->credential);
  REQUIRE(diagnostics.str().find("mode 0600") != std::string::npos);
  REQUIRE(diagnostics.str().find("stored-process-secret") == std::string::npos);
}
