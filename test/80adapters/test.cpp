#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

#include <unistd.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <aiforge/adapters/termforge_run_bridge.hpp>
#include <aiforge/adapters/ask_user_dialog.hpp>
#include <aiforge/adapters/filesystem_persona_source.hpp>
#include <aiforge/adapters/json_model_catalog_cache.hpp>
#include <aiforge/adapters/model_picker_dialog.hpp>
#include <aiforge/adapters/process_provenance.hpp>
#include <aiforge/adapters/process_credentials.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/adapters/venice_backend.hpp>
#include <aiforge/adapters/venice_model_catalog_source.hpp>
#include <aiforge/runtime/ask_user_tool.hpp>
#include <aiforge/testing/scripted_backend.hpp>

namespace {

using namespace std::chrono_literals;
using namespace aiforge;

template <typename IdType>
auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

auto secret(std::string value) -> credentials::Secret {
  return credentials::make_secret(std::move(value)).value();
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
                                 {0.5, 64, 7, {}}};
}

class LocalServer final {
 public:
  explicit LocalServer(std::optional<std::string> echoed_error = std::nullopt,
                       const bool duplicate_cost = false,
                       std::string cost_value = "0.0645375")
      : m_echoed_error(std::move(echoed_error)),
        m_duplicate_cost(duplicate_cost),
        m_cost_value(std::move(cost_value)) {
    m_server.Get(
        "/api/v1/models",
        [this](const httplib::Request& request, httplib::Response& response) {
          {
            std::lock_guard lock(m_mutex);
            m_model_authorization =
                request.get_header_value("Authorization");
          }
          response.set_content(
              R"({"data":[{"id":"test-model","type":"text","context_length":8192,"model_spec":{"availableContextTokens":8192,"maxCompletionTokens":1024,"offline":false}}]})",
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
              "\"assistant\"}}]}\n\n"
              "data: "
              "{\"id\":\"response\",\"choices\":[{\"delta\":{\"content\":"
              "\"hello\"}}]}\n\n";
          stream +=
              "data: "
              "{\"id\":\"response\",\"choices\":[{\"delta\":{}}],"
              "\"cost\":{\"usd\":0,\"diem\":" +
              m_cost_value + "}}\n\n";
          if (m_duplicate_cost) {
            stream +=
                "data: "
                "{\"id\":\"response\",\"choices\":[{\"delta\":{}}],"
                "\"cost\":{\"diem\":0.01}}\n\n";
          }
          stream +=
              "data: "
              "{\"id\":\"response\",\"choices\":[{\"delta\":{},\"finish_"
              "reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":2,\"completion_"
              "tokens\":1,\"total_tokens\":3}}\n\n"
              "data: [DONE]\n\n";
          response.set_content(std::move(stream), "text/event-stream");
        });
    m_port = m_server.bind_to_any_port("127.0.0.1");
    REQUIRE(m_port > 0);
    m_thread = std::jthread([this] { m_server.listen_after_bind(); });
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
  std::string m_cost_value;
};

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

}  // namespace

TEST_CASE("Venice model discovery is public and maps neutral context metadata",
          "[adapter][models]") {
  LocalServer server;
  adapters::VeniceModelCatalogOptions options;
  options.base_url = server.base_url();
  adapters::VeniceModelCatalogSource source{std::move(options)};
  const auto catalog = source.fetch({});
  REQUIRE(catalog);
  REQUIRE(catalog->entries.size() == 1);
  REQUIRE(catalog->entries.front().id == make_id<domain::ModelId>("test-model"));
  REQUIRE(catalog->entries.front().type == "text");
  REQUIRE(catalog->entries.front().context_window_tokens == 8192);
  REQUIRE(catalog->entries.front().maximum_output_tokens == 1024);
  REQUIRE(server.model_authorization().empty());
}

TEST_CASE("model catalog cache round trips atomically with restrictive permissions",
          "[adapter][models][cache]") {
  TempDirectory temporary{"aiforge-model-cache"};
  const auto path = temporary.path() / "cache" / "model-catalog.json";
  adapters::JsonModelCatalogCache cache{path};
  model::CatalogEntry entry{make_id<domain::ModelId>("text-model"), "text"};
  entry.name = "Text Model";
  entry.context_window_tokens = 8192;
  entry.capabilities = {
      {model::Capability::tool_calling, true},
      {model::Capability::vision, std::nullopt}};
  entry.pricing = model::Pricing{};
  entry.pricing->base.input = model::Price{0.5, 1.0};
  model::CatalogSnapshot snapshot{
      std::chrono::sys_time<std::chrono::milliseconds>{1234ms},
      {std::move(entry)}};

  REQUIRE(cache.store(snapshot, {}));
  auto loaded = cache.load({});
  REQUIRE(loaded);
  REQUIRE(loaded->has_value());
  REQUIRE((*loaded)->entries == snapshot.entries);
  REQUIRE((*loaded)->fetched_at == snapshot.fetched_at);

  const auto permissions = std::filesystem::status(path).permissions();
  REQUIRE((permissions & (std::filesystem::perms::group_all |
                          std::filesystem::perms::others_all)) ==
          std::filesystem::perms::none);
}

TEST_CASE("model cache rejects duplicate JSON, loose modes, symlinks and cancellation",
          "[adapter][models][cache][failure]") {
  TempDirectory temporary{"aiforge-model-cache-hostile"};
  const auto path = temporary.path() / "model-catalog.json";
  adapters::JsonModelCatalogCache cache{path, 4096};
  write_file(path,
             R"({"schema_version":1,"schema_version":1,"fetched_at_ms":0,"entries":[]})");
  std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
  REQUIRE_FALSE(cache.load({}));

  write_file(path,
             R"({"schema_version":1,"fetched_at_ms":0,"entries":[]})");
  std::filesystem::permissions(path, std::filesystem::perms::owner_read |
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

TEST_CASE("model picker filters, selects, cancels and survives tiny geometry",
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
  for (const char character : std::string{"beta"}) {
    REQUIRE(dialog.on_event(termforge::KeyEvent{
        termforge::Key::Char, static_cast<char32_t>(character), false, false,
        false, termforge::KeyAction::Press}));
  }
  REQUIRE(dialog.on_event(termforge::KeyEvent{
      termforge::Key::Tab, 0, false, false, false,
      termforge::KeyAction::Press}));
  REQUIRE(dialog.on_event(termforge::KeyEvent{
      termforge::Key::Enter, 0, false, false, false,
      termforge::KeyAction::Press}));
  REQUIRE(reported);
  REQUIRE(selected == make_id<domain::ModelId>("beta-chat"));

  adapters::ModelPickerDialog cancelled_dialog;
  bool cancelled_result{};
  cancelled_dialog.on_result([&](std::optional<domain::ModelId> result) {
    cancelled_result = !result.has_value();
  });
  cancelled_dialog.set_models(snapshot, make_id<domain::ModelId>("alpha-chat"));
  termforge::Screen tiny{1, 1};
  cancelled_dialog.draw(tiny);
  REQUIRE(cancelled_dialog.on_event(termforge::KeyEvent{
      termforge::Key::Escape, 0, false, false, false,
      termforge::KeyAction::Press}));
  REQUIRE(cancelled_result);
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

  const auto bounded = source.load(
      "Reviewer", {.maximum_personas = 256,
                   .maximum_name_bytes = 96,
                   .maximum_file_bytes = 2,
                   .maximum_description_bytes = 160});
  REQUIRE_FALSE(bounded);
  REQUIRE(bounded.error().code == persona::PersonaErrorCode::resource_exhausted);
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

  write_file(root / "empty.md", "");
  const auto empty = source.load("empty");
  REQUIRE_FALSE(empty);
  REQUIRE(empty.error().code == persona::PersonaErrorCode::malformed_text);

  const auto too_many = source.list(
      {.maximum_personas = 1,
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

TEST_CASE("Venice adapter rejects unsupported content without a request",
          "[adapter][venice][failure]") {
  adapters::VeniceBackend backend{
      secret("secret"), {"http://127.0.0.1:1", 10ms, 10ms, 10ms, 4}};
  auto unsupported = context(domain::ArtifactReferenceBlock{
      make_id<domain::ArtifactId>("artifact"), std::nullopt});
  const auto started = backend.start(request(std::move(unsupported)), {});
  REQUIRE_FALSE(started);
  REQUIRE(started.error().kind == backend::BackendErrorKind::request_rejected);
  REQUIRE(started.error().redacted_message.find("secret") == std::string::npos);
}

TEST_CASE("Venice adapter maps structured SSE into neutral events",
          "[adapter][venice]") {
  LocalServer server;
  adapters::VeniceBackend backend{
      secret("test-secret"), {server.base_url(), 1s, 1s, 1s, 8}};
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

TEST_CASE("Venice adapter rejects duplicate provider cost frames",
          "[adapter][venice][cost][failure]") {
  LocalServer server{std::nullopt, true};
  adapters::VeniceBackend backend{
      secret("test-secret"), {server.base_url(), 1s, 1s, 1s, 8}};
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
}

TEST_CASE("Venice adapter rejects invalid provider cost values",
          "[adapter][venice][cost][failure]") {
  for (const auto* value : {"-1", "\"invalid\""}) {
    CAPTURE(value);
    LocalServer server{std::nullopt, false, value};
    adapters::VeniceBackend backend{
        secret("test-secret"), {server.base_url(), 1s, 1s, 1s, 8}};
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
  }
}

TEST_CASE("Venice adapter sends structured tool results as tool messages",
          "[adapter][venice][tools]") {
  LocalServer server;
  adapters::VeniceBackend backend{
      secret("test-secret"), {server.base_url(), 1s, 1s, 1s, 8}};
  auto built = context();
  built.entries.front().kind = domain::ContextEntryKind::tool_result;
  built.entries.front().message = {
      make_id<domain::MessageId>("tool-result"), domain::Role::tool,
      {domain::StructuredDataBlock{"application/json",
                                   R"({"status":"cancelled"})"}},
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

TEST_CASE("Venice adapter exposes neutral model context metadata",
          "[adapter][venice][models]") {
  LocalServer server;
  adapters::VeniceBackend backend{
      secret("test-secret"), {server.base_url(), 1s, 1s, 1s, 8}};
  const auto model_id = make_id<domain::ModelId>("test-model");
  const auto context = backend.lookup(model_id, {});
  REQUIRE(context);
  REQUIRE(context->model_id == model_id);
  REQUIRE(context->context_window_tokens == 8192);
  REQUIRE(context->maximum_output_tokens == 1024);

  const auto missing =
      backend.lookup(make_id<domain::ModelId>("missing-model"), {});
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().kind ==
          backend::BackendErrorKind::request_rejected);
  REQUIRE(missing.error().redacted_message.find("test-secret") ==
          std::string::npos);
}

TEST_CASE("Venice adapter redacts provider bodies containing credentials",
          "[adapter][venice][failure][redaction]") {
  const std::string credential{"provider-echoed-secret"};
  LocalServer server{credential};
  adapters::VeniceBackend backend{
      secret(credential), {server.base_url(), 1s, 1s, 1s, 8}};
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
  runtime::RunKernel kernel{make_id<domain::SessionId>("session"), fake,
                            nullptr, {}, {}, std::move(*snapshot)};
  REQUIRE(kernel.start(
      {make_id<domain::RunId>("run"),
       {make_id<domain::SurfaceId>("tui"),
        make_id<domain::WorkspaceId>("chat"),
        make_id<domain::PermissionProfileId>("observe"), std::nullopt},
       {make_id<domain::MessageId>("user"), domain::Role::user,
        {domain::TextBlock{"hello"}}, std::nullopt},
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
  REQUIRE(dialog.on_event(termforge::KeyEvent{
      termforge::Key::Enter, 0, false, false, false,
      termforge::KeyAction::Press}));
  REQUIRE(dialog.current_page() == 1);
  REQUIRE(kernel.pending_question_input());
  tiny.resize(1, 1);
  dialog.draw(tiny);
  tiny.resize(40, 12);
  dialog.draw(tiny);
  REQUIRE(dialog.on_event(termforge::KeyEvent{
      termforge::Key::Enter, 0, false, false, false,
      termforge::KeyAction::Press}));
  REQUIRE(dialog.current_page() == 2);
  REQUIRE(kernel.pending_question_input());
  REQUIRE(dialog.on_event(termforge::KeyEvent{
      termforge::Key::Enter, 0, false, false, false,
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
  static_cast<void>(dialog.on_event(termforge::KeyEvent{
      termforge::Key::Enter, 0, false, false, false,
      termforge::KeyAction::Press}));
  const auto after_duplicate =
      runtime::tool_result_messages(kernel.event_log().events());
  REQUIRE(after_duplicate);
  REQUIRE(after_duplicate->size() == 1);
  REQUIRE(kernel.cancel_run(make_id<domain::RunId>("run"), "cleanup"));
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
      {{"model", config::ConfigValue{std::string{"venice-model"}}, std::nullopt},
       {"credential", config::ConfigValue{secret}, std::nullopt}},
      {}};
  const std::array layers{environment};
  const auto resolved = config::resolve_config(registry, layers);
  REQUIRE(resolved);
  REQUIRE(std::get<std::string>(*resolved->find("credential")->value) == secret);

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
  const auto credential = std::ranges::find(
      provenance.configuration, "credential",
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
            {make_id<domain::EventId>("e1"), make_id<domain::RunId>("run"), 1, 1,
             domain::EventTimestamp{std::chrono::milliseconds{101}},
             std::nullopt, std::nullopt, std::nullopt},
            domain::RunStarted{make_id<domain::SurfaceId>("one-shot"),
                               make_id<domain::WorkspaceId>("chat"),
                               make_id<domain::PermissionProfileId>("observe"),
                               std::nullopt}},
        domain::RunEvent{
            {make_id<domain::EventId>("e2"), make_id<domain::RunId>("run"), 2, 1,
             domain::EventTimestamp{std::chrono::milliseconds{102}},
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
