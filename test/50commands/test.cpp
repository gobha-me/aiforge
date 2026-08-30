#include <aiforge/cli/command_registry.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace aiforge::cli;

auto success_handler(CommandContext& context) -> int {
  context.output << "handled:" << context.invocation.command_path.back()
                 << '\n';
  return 7;
}

auto throwing_handler(CommandContext&) -> int {
  throw std::runtime_error{"sensitive handler failure"};
}

auto typed_handler(CommandContext& context) -> int {
  const auto& value = context.invocation.arguments.front().values.front();
  context.output << "count:" << std::get<std::int64_t>(value) << '\n';
  return 0;
}

class FakeOneShot final : public OneShotCommand {
 public:
  auto execute(Request request, CommandEnvironment& environment,
               std::ostream& output, std::ostream& error)
      -> std::expected<void, CommandFailure> override {
    seen_prompt = std::string{request.prompt};
    seen_session_mode = request.session_mode;
    seen_session_id = std::move(request.session_id);
    seen_persona = std::move(request.persona);
    seen_model = std::move(request.model);
    seen_spend_ceiling = std::move(request.session_spend_ceiling);
    saw_terminal_input = environment.input_is_terminal;
    output << "answer";
    error << "usage\n";
    if (failure) return std::unexpected(*failure);
    return {};
  }

  std::string seen_prompt;
  SessionMode seen_session_mode{SessionMode::create};
  std::optional<aiforge::domain::SessionId> seen_session_id;
  aiforge::persona::PersonaDirective seen_persona;
  std::optional<std::string> seen_model;
  std::optional<aiforge::domain::SessionSpendCeiling> seen_spend_ceiling;
  bool saw_terminal_input{};
  std::optional<CommandFailure> failure;
};

class FakeInteractive final : public InteractiveCommand {
 public:
  auto execute(Request request, CommandEnvironment&, std::ostream& output,
               std::ostream&) -> std::expected<void, CommandFailure> override {
    seen_session_mode = request.session_mode;
    seen_session_id = std::move(request.session_id);
    seen_persona = std::move(request.persona);
    seen_model = std::move(request.model);
    seen_spend_ceiling = std::move(request.session_spend_ceiling);
    output << "interactive";
    if (failure) return std::unexpected(*failure);
    return {};
  }

  SessionMode seen_session_mode{SessionMode::create};
  std::optional<aiforge::domain::SessionId> seen_session_id;
  aiforge::persona::PersonaDirective seen_persona;
  std::optional<std::string> seen_model;
  std::optional<aiforge::domain::SessionSpendCeiling> seen_spend_ceiling;
  std::optional<CommandFailure> failure;
};

class FakeModels final : public ModelsCommand {
 public:
  auto execute(CommandEnvironment&, std::ostream& output, std::ostream& error)
      -> std::expected<void, CommandFailure> override {
    ++calls;
    output << "models\n";
    error << "warning\n";
    if (failure) return std::unexpected(*failure);
    return {};
  }

  int calls{};
  std::optional<CommandFailure> failure;
};

class FakeImage final : public ImageCommand {
 public:
  auto generate(GenerateRequest request, CommandEnvironment&,
                std::ostream& output, std::ostream&)
      -> std::expected<void, CommandFailure> override {
    ++generate_calls;
    generated = std::move(request);
    output << "generated\n";
    return {};
  }

  auto show(ShowRequest request, CommandEnvironment&, std::ostream& output,
            std::ostream&) -> std::expected<void, CommandFailure> override {
    ++show_calls;
    shown = std::move(request);
    output << "shown\n";
    return {};
  }

  int generate_calls{};
  int show_calls{};
  std::optional<GenerateRequest> generated;
  std::optional<ShowRequest> shown;
};

class FakeAudio final : public AudioCommand {
 public:
  auto synthesize(SynthesizeRequest request, CommandEnvironment&,
                  std::ostream& output, std::ostream&)
      -> std::expected<void, CommandFailure> override {
    synthesized = std::move(request);
    output << "synthesized\n";
    return {};
  }

  auto transcribe(TranscribeRequest request, CommandEnvironment&,
                  std::ostream& output, std::ostream&)
      -> std::expected<void, CommandFailure> override {
    transcribed = std::move(request);
    output << "transcribed\n";
    return {};
  }

  auto export_artifact(ExportRequest request, CommandEnvironment&,
                       std::ostream& output, std::ostream&)
      -> std::expected<void, CommandFailure> override {
    exported = std::move(request);
    output << "exported\n";
    return {};
  }

  std::optional<SynthesizeRequest> synthesized;
  std::optional<TranscribeRequest> transcribed;
  std::optional<ExportRequest> exported;
};

class FakeLogin final : public LoginCommand {
 public:
  auto execute(CommandEnvironment& environment, std::ostream& output,
               std::ostream& error)
      -> std::expected<void, CommandFailure> override {
    ++calls;
    saw_terminal_input = environment.input_is_terminal;
    input_descriptor = environment.input_descriptor;
    output << "stored\n";
    error << "prompt\n";
    if (failure) return std::unexpected(*failure);
    return {};
  }

  int calls{};
  bool saw_terminal_input{};
  int input_descriptor{-1};
  std::optional<CommandFailure> failure;
};

class FakePlan final : public PlanCommand {
 public:
  auto execute(Request request, CommandEnvironment&, std::ostream& output,
               std::ostream& error)
      -> std::expected<void, CommandFailure> override {
    ++calls;
    seen_mode = request.session_mode;
    seen_session_id = std::move(request.session_id);
    output << "jsonl\n";
    error << "diagnostic\n";
    if (failure) return std::unexpected(*failure);
    return {};
  }

  int calls{};
  SessionMode seen_mode{SessionMode::resume};
  std::optional<aiforge::domain::SessionId> seen_session_id;
  std::optional<CommandFailure> failure;
};

[[nodiscard]] auto root_command(CommandHandler handler = success_handler)
    -> CommandSpec {
  return {"root", "", "Test command registry.", false, {}, {}, {}, handler};
}

[[nodiscard]] auto registry_with(CommandSpec root) -> CommandRegistry {
  return {"forge-test",
          "1.2.3",
          std::move(root),
          {{ControlRequestKind::help, {"-h", "--help"}},
           {ControlRequestKind::version, {"--version"}}}};
}

[[nodiscard]] auto dispatch(const CommandRegistry& registry,
                            std::vector<std::string_view> arguments,
                            std::string& output, std::string& error) -> int {
  std::ostringstream output_stream;
  std::ostringstream error_stream;
  const auto result = CommandDispatcher{}.dispatch(
      registry, arguments, output_stream, error_stream, {32, 256, 1024});
  output = output_stream.str();
  error = error_stream.str();
  return result;
}

} // namespace

TEST_CASE("invalid command registries fail before parser construction",
          "[commands][failure]") {
  auto root = root_command();
  root.handler = nullptr;
  auto result = make_parser_schema(registry_with(root));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == RegistryDiagnosticCode::missing_handler);

  root = root_command();
  root.subcommands = {
      {"duplicate", "one", "One.", false, {}, {}, {}, success_handler},
      {"duplicate", "two", "Two.", false, {}, {}, {}, success_handler}};
  result = make_parser_schema(registry_with(root));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == RegistryDiagnosticCode::duplicate_command_id);

  root = root_command();
  root.subcommands = {
      {"one", "same", "One.", false, {}, {}, {}, success_handler},
      {"two", "same", "Two.", false, {}, {}, {}, success_handler}};
  result = make_parser_schema(registry_with(root));
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          RegistryDiagnosticCode::duplicate_command_name);
}

TEST_CASE("one command registration drives schema help and dispatch",
          "[commands]") {
  auto root = root_command();
  root.subcommands.push_back({
      "toy",
      "toy",
      "Run the toy command.",
      false,
      {{{"toy.count", {"--count"}, ArgumentValueKind::signed_integer, 1, 1},
        "number",
        "Number of runs."}},
      {},
      {},
      typed_handler,
  });
  const auto registry = registry_with(std::move(root));

  const auto schema = make_parser_schema(registry);
  REQUIRE(schema);
  REQUIRE(schema->root.subcommands.size() == 1);
  REQUIRE(schema->root.subcommands.front().id == "toy");

  const auto help = render_help(registry, std::vector<std::string>{"root"});
  REQUIRE(help);
  REQUIRE(help->find("toy") != std::string::npos);
  REQUIRE(help->find("Run the toy command.") != std::string::npos);

  std::string output;
  std::string error;
  REQUIRE(dispatch(registry, {"toy", "--count", "0"}, output, error) == 0);
  REQUIRE(output == "count:0\n");
  REQUIRE(error.empty());

  const auto toy_help =
      render_help(registry, std::vector<std::string>{"root", "toy"});
  REQUIRE(toy_help);
  REQUIRE(toy_help->find("--count <number>") != std::string::npos);
  REQUIRE(toy_help->find("Number of runs.") != std::string::npos);
}

TEST_CASE("command-line failures use stderr and usage exit code",
          "[commands][failure]") {
  auto root = root_command();
  root.subcommands.push_back(
      {"known", "known", "Known command.", false, {}, {}, {}, success_handler});
  const auto registry = registry_with(std::move(root));
  std::string output;
  std::string error;

  REQUIRE(dispatch(registry, {"missing"}, output, error) == 2);
  REQUIRE(output.empty());
  REQUIRE(error.find("unknown command") != std::string::npos);

  REQUIRE(dispatch(registry, {"--secret-token"}, output, error) == 2);
  REQUIRE(output.empty());
  REQUIRE(error.find("--secret-token") == std::string::npos);
}

TEST_CASE("handler exceptions are contained and redacted",
          "[commands][failure]") {
  auto root = root_command(throwing_handler);
  const auto registry = registry_with(std::move(root));
  std::string output;
  std::string error;

  REQUIRE(dispatch(registry, {}, output, error) == 1);
  REQUIRE(output.empty());
  REQUIRE(error.find("failed internally") != std::string::npos);
  REQUIRE(error.find("sensitive handler failure") == std::string::npos);
}

TEST_CASE("parser-schema defects are internal dispatch failures",
          "[commands][failure]") {
  auto root = root_command();
  root.options = {{{"duplicate.one", {"--same"}, ArgumentValueKind::flag, 0, 1},
                   "",
                   "One."},
                  {{"duplicate.two", {"--same"}, ArgumentValueKind::flag, 0, 1},
                   "",
                   "Two."}};
  const auto registry = registry_with(std::move(root));
  std::string output;
  std::string error;

  REQUIRE(dispatch(registry, {}, output, error) == 1);
  REQUIRE(output.empty());
  REQUIRE(error.find("internal command dispatch error") != std::string::npos);
}

TEST_CASE("help and version are generated without dispatch", "[commands]") {
  auto root = root_command(throwing_handler);
  root.subcommands.push_back(
      {"child", "child", "Child help.", false, {}, {}, {}, throwing_handler});
  const auto registry = registry_with(std::move(root));
  std::string output;
  std::string error;

  REQUIRE(dispatch(registry, {"--help"}, output, error) == 0);
  REQUIRE(output.starts_with("Usage: forge-test"));
  REQUIRE(output.find("Child help.") != std::string::npos);
  REQUIRE(error.empty());

  REQUIRE(dispatch(registry, {"child", "--help"}, output, error) == 0);
  REQUIRE(output.starts_with("Usage: forge-test child"));
  REQUIRE(output.find("Child help.") != std::string::npos);
  REQUIRE(error.empty());

  REQUIRE(dispatch(registry, {"--version"}, output, error) == 0);
  REQUIRE(output == "forge-test 1.2.3\n");
  REQUIRE(error.empty());
}

TEST_CASE("builtin commands expose honest offline behavior", "[commands]") {
  const auto& registry = builtin_command_registry();
  const auto schema = make_parser_schema(registry);
  REQUIRE(schema);
  REQUIRE(schema->root.subcommands.size() == 8);
  const auto config =
      std::ranges::find(schema->root.subcommands, "config", &CommandSchema::id);
  REQUIRE(config != schema->root.subcommands.end());
  REQUIRE(config->subcommands.size() == 4);
  const auto image =
      std::ranges::find(schema->root.subcommands, "image", &CommandSchema::id);
  REQUIRE(image != schema->root.subcommands.end());
  REQUIRE(image->subcommands.size() == 2);
  const auto audio =
      std::ranges::find(schema->root.subcommands, "audio", &CommandSchema::id);
  REQUIRE(audio != schema->root.subcommands.end());
  REQUIRE(audio->subcommands.size() == 3);

  std::string output;
  std::string error;
  REQUIRE(dispatch(registry, {"chat"}, output, error) == 2);
  REQUIRE(output.empty());
  REQUIRE((error.find("requires") != std::string::npos ||
           error.find("required") != std::string::npos));

  REQUIRE(dispatch(registry, {"chat", "hello"}, output, error) == 1);
  REQUIRE(output.empty());
  REQUIRE(error.find("not available in this build") != std::string::npos);

  REQUIRE(dispatch(registry, {"models"}, output, error) == 1);
  REQUIRE(output.empty());
  REQUIRE(error.find("not available in this build") != std::string::npos);

  REQUIRE(dispatch(registry, {"config"}, output, error) == 2);
  REQUIRE(output.empty());
  REQUIRE(error.find("requires a subcommand") != std::string::npos);

  REQUIRE(dispatch(registry, {"version"}, output, error) == 0);
  REQUIRE(output.starts_with("aiforge "));
  REQUIRE(error.empty());
}

TEST_CASE("plan command requires explicit noninteractive JSONL session mode",
          "[commands][plan]") {
  const auto& registry = builtin_command_registry();
  FakePlan plan;
  std::istringstream input;
  CommandEnvironment environment{input, false, false, false, {}};
  environment.plan = &plan;
  std::ostringstream output;
  std::ostringstream error;

  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{"plan", "--jsonl", "--resume",
                                            "session-42"},
              environment, output, error) == 0);
  REQUIRE(plan.calls == 1);
  REQUIRE(plan.seen_mode == PlanCommand::SessionMode::resume);
  REQUIRE(plan.seen_session_id ==
          aiforge::domain::SessionId::from("session-42").value());
  REQUIRE(output.str() == "jsonl\n");
  REQUIRE(error.str() == "diagnostic\n");

  output.str({});
  error.str({});
  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{"plan", "--jsonl", "--continue"},
              environment, output, error) == 0);
  REQUIRE(plan.calls == 2);
  REQUIRE(plan.seen_mode == PlanCommand::SessionMode::continue_latest);
  REQUIRE_FALSE(plan.seen_session_id);

  for (const auto& arguments :
       {std::vector<std::string_view>{"plan", "--continue"},
        std::vector<std::string_view>{"plan", "--jsonl"},
        std::vector<std::string_view>{"plan", "--jsonl", "--continue",
                                      "--resume", "session-42"}}) {
    output.str({});
    error.str({});
    REQUIRE(CommandDispatcher{}.dispatch(registry, arguments, environment,
                                         output, error) == 2);
  }

  environment.input_is_terminal = true;
  output.str({});
  error.str({});
  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{"plan", "--jsonl", "--continue"},
              environment, output, error) == 2);
  REQUIRE(error.str().find("noninteractive") != std::string::npos);
}

TEST_CASE("builtin one-shot routes root and chat prompts through one service",
          "[commands][one-shot]") {
  const auto& registry = builtin_command_registry();
  FakeOneShot one_shot;
  std::istringstream input;
  CommandEnvironment environment{input, false, false, false, {}, &one_shot};
  std::ostringstream output;
  std::ostringstream error;
  const std::vector<std::string_view> root_arguments{"hello"};

  REQUIRE(CommandDispatcher{}.dispatch(registry, root_arguments, environment,
                                       output, error) == 0);
  REQUIRE(one_shot.seen_prompt == "hello");
  REQUIRE_FALSE(one_shot.saw_terminal_input);
  REQUIRE(output.str() == "answer");
  REQUIRE(error.str() == "usage\n");

  output.str({});
  error.str({});
  const std::vector<std::string_view> chat_arguments{"chat", "again"};
  REQUIRE(CommandDispatcher{}.dispatch(registry, chat_arguments, environment,
                                       output, error) == 0);
  REQUIRE(one_shot.seen_prompt == "again");

  one_shot.failure =
      CommandFailure{CommandFailureKind::cancelled, "request cancelled"};
  output.str({});
  error.str({});
  const std::vector<std::string_view> stop_arguments{"chat", "stop"};
  REQUIRE(CommandDispatcher{}.dispatch(registry, stop_arguments, environment,
                                       output, error) == 130);
  REQUIRE(error.str().find("request cancelled") != std::string::npos);
}

TEST_CASE("one-shot session options are explicit and mutually exclusive",
          "[commands][one-shot][session][failure]") {
  const auto& registry = builtin_command_registry();
  FakeOneShot one_shot;
  FakeInteractive interactive;
  std::istringstream input;
  CommandEnvironment environment{input, true,      true,        true,
                                 {},    &one_shot, &interactive};
  std::ostringstream output;
  std::ostringstream error;

  REQUIRE(CommandDispatcher{}.dispatch(
              registry, std::vector<std::string_view>{"--ephemeral", "hello"},
              environment, output, error) == 0);
  REQUIRE(one_shot.seen_session_mode == OneShotCommand::SessionMode::ephemeral);
  REQUIRE_FALSE(one_shot.seen_session_id);

  output.str({});
  error.str({});
  REQUIRE(
      CommandDispatcher{}.dispatch(
          registry,
          std::vector<std::string_view>{"chat", "--resume", "saved", "again"},
          environment, output, error) == 0);
  REQUIRE(one_shot.seen_session_mode == OneShotCommand::SessionMode::resume);
  REQUIRE(one_shot.seen_session_id ==
          aiforge::domain::SessionId::from("saved").value());

  output.str({});
  error.str({});
  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{"chat", "--persona", "reviewer",
                                            "with persona"},
              environment, output, error) == 0);
  REQUIRE(one_shot.seen_persona.kind ==
          aiforge::persona::PersonaDirectiveKind::select);
  REQUIRE(one_shot.seen_persona.name == "reviewer");

  output.str({});
  error.str({});
  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{"chat", "--model", "text-model",
                                            "with model"},
              environment, output, error) == 0);
  REQUIRE(one_shot.seen_model == "text-model");

  output.str({});
  error.str({});
  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{"chat", "--session-max-spend",
                                            "12.340000", "bounded"},
              environment, output, error) == 0);
  REQUIRE(one_shot.seen_spend_ceiling->amount().to_string() == "12.34");

  for (const auto value : {"0", "-1", "1e2", "0.0000001"}) {
    output.str({});
    error.str({});
    CAPTURE(value);
    REQUIRE(CommandDispatcher{}.dispatch(
                registry,
                std::vector<std::string_view>{"chat", "--session-max-spend",
                                              value, "invalid"},
                environment, output, error) == 2);
    REQUIRE(error.str().find("positive USD decimal") != std::string::npos);
  }

  output.str({});
  error.str({});
  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{"--no-persona", "without"},
              environment, output, error) == 0);
  REQUIRE(one_shot.seen_persona.kind ==
          aiforge::persona::PersonaDirectiveKind::disable);
  REQUIRE_FALSE(one_shot.seen_persona.name);

  output.str({});
  error.str({});
  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{"--persona", "reviewer",
                                            "--no-persona", "conflict"},
              environment, output, error) == 2);
  REQUIRE(error.str().find("mutually exclusive") != std::string::npos);

  output.str({});
  error.str({});
  REQUIRE(
      CommandDispatcher{}.dispatch(registry,
                                   std::vector<std::string_view>{
                                       "--continue", "--ephemeral", "conflict"},
                                   environment, output, error) == 2);
  REQUIRE(error.str().find("mutually exclusive") != std::string::npos);

  output.str({});
  error.str({});
  REQUIRE(CommandDispatcher{}.dispatch(
              registry, std::vector<std::string_view>{"--resume", "saved"},
              environment, output, error) == 0);
  REQUIRE(output.str() == "interactive");
  REQUIRE(interactive.seen_session_mode ==
          InteractiveCommand::SessionMode::resume);
  REQUIRE(interactive.seen_session_id ==
          aiforge::domain::SessionId::from("saved").value());

  output.str({});
  error.str({});
  REQUIRE(CommandDispatcher{}.dispatch(
              registry, std::vector<std::string_view>{"--persona", "reviewer"},
              environment, output, error) == 0);
  REQUIRE(interactive.seen_persona.kind ==
          aiforge::persona::PersonaDirectiveKind::select);
  REQUIRE(interactive.seen_persona.name == "reviewer");

  output.str({});
  error.str({});
  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{"--model", "interactive-model"},
              environment, output, error) == 0);
  REQUIRE(interactive.seen_model == "interactive-model");

  output.str({});
  error.str({});
  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{"--session-max-spend", "5.5"},
              environment, output, error) == 0);
  REQUIRE(interactive.seen_spend_ceiling->amount().to_string() == "5.5");
}

TEST_CASE("models command uses its dedicated service and preserves streams",
          "[commands][models]") {
  const auto& registry = builtin_command_registry();
  FakeModels models;
  std::istringstream input;
  CommandEnvironment environment{input, false,   false,   false,
                                 {},    nullptr, nullptr, &models};
  std::ostringstream output;
  std::ostringstream error;
  REQUIRE(CommandDispatcher{}.dispatch(registry,
                                       std::vector<std::string_view>{"models"},
                                       environment, output, error) == 0);
  REQUIRE(models.calls == 1);
  REQUIRE(output.str() == "models\n");
  REQUIRE(error.str() == "warning\n");

  output.str({});
  error.str({});
  models.failure = CommandFailure{CommandFailureKind::runtime, "offline"};
  REQUIRE(CommandDispatcher{}.dispatch(registry,
                                       std::vector<std::string_view>{"models"},
                                       environment, output, error) == 1);
  REQUIRE(error.str().find("offline") != std::string::npos);
}

TEST_CASE("image subcommands parse explicit generation and replay contracts",
          "[commands][image]") {
  const auto& registry = builtin_command_registry();
  FakeImage image;
  std::istringstream input;
  CommandEnvironment environment{input, false, false, false, {}};
  environment.image = &image;
  std::ostringstream output;
  std::ostringstream error;

  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{
                  "image", "generate", "--model", "image-model", "--format",
                  "png", "--output", "result.png", "blue", "square"},
              environment, output, error) == 0);
  REQUIRE(image.generate_calls == 1);
  REQUIRE(image.generated);
  CHECK(image.generated->prompt == "blue square");
  CHECK(image.generated->model == "image-model");
  CHECK(image.generated->format == "png");
  CHECK(image.generated->output_path == "result.png");

  output.str({});
  error.str({});
  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{"image", "show", "--session",
                                            "image-session-1", "--artifact",
                                            "image-artifact-1"},
              environment, output, error) == 0);
  REQUIRE(image.show_calls == 1);
  REQUIRE(image.shown);
  CHECK(image.shown->session_id ==
        aiforge::domain::SessionId::from("image-session-1").value());
  CHECK(image.shown->artifact_id ==
        aiforge::domain::ArtifactId::from("image-artifact-1").value());

  for (const auto& arguments :
       {std::vector<std::string_view>{"image", "generate", "prompt"},
        std::vector<std::string_view>{"image", "generate", "--model", "m",
                                      "--format", "gif", "prompt"},
        std::vector<std::string_view>{"image", "show"}}) {
    output.str({});
    error.str({});
    CHECK(CommandDispatcher{}.dispatch(registry, arguments, environment, output,
                                       error) == 2);
  }
}

TEST_CASE("audio subcommands parse bounded artifact-only contracts",
          "[commands][audio]") {
  const auto& registry = builtin_command_registry();
  FakeAudio audio;
  std::istringstream input;
  CommandEnvironment environment{input, false, false, false, {}};
  environment.audio = &audio;
  std::ostringstream output;
  std::ostringstream error;

  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{"audio", "synthesize", "--model",
                                            "tts-model", "--voice", "voice",
                                            "--language", "en-US", "--output",
                                            "speech.wav", "hello", "world"},
              environment, output, error) == 0);
  REQUIRE(audio.synthesized);
  CHECK(audio.synthesized->text == "hello world");
  CHECK(audio.synthesized->model == "tts-model");
  CHECK(audio.synthesized->voice == "voice");
  CHECK(audio.synthesized->language == "en-US");
  CHECK(audio.synthesized->output_path == "speech.wav");

  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{"audio", "transcribe", "--model",
                                            "asr-model", "input.wav"},
              environment, output, error) == 0);
  REQUIRE(audio.transcribed);
  CHECK(audio.transcribed->model == "asr-model");
  CHECK(audio.transcribed->input_path == "input.wav");

  REQUIRE(CommandDispatcher{}.dispatch(
              registry,
              std::vector<std::string_view>{
                  "audio", "export", "--session", "audio-session-1",
                  "--artifact", "audio-artifact-1", "--output", "copy.wav"},
              environment, output, error) == 0);
  REQUIRE(audio.exported);
  CHECK(audio.exported->session_id ==
        aiforge::domain::SessionId::from("audio-session-1").value());
  CHECK(audio.exported->artifact_id ==
        aiforge::domain::ArtifactId::from("audio-artifact-1").value());
  CHECK(audio.exported->output_path == "copy.wav");

  for (const auto& arguments :
       {std::vector<std::string_view>{"audio", "synthesize", "--model", "m",
                                      "missing voice"},
        std::vector<std::string_view>{"audio", "transcribe", "input.wav"},
        std::vector<std::string_view>{"audio", "export", "--session", "s"}}) {
    CHECK(CommandDispatcher{}.dispatch(registry, arguments, environment, output,
                                       error) == 2);
  }
}

TEST_CASE("login command uses its dedicated terminal service",
          "[commands][login]") {
  const auto& registry = builtin_command_registry();
  FakeLogin login;
  std::istringstream input;
  CommandEnvironment environment{input,   true,    true,    true,   {},
                                 nullptr, nullptr, nullptr, &login, 42};
  std::ostringstream output;
  std::ostringstream error;

  REQUIRE(CommandDispatcher{}.dispatch(registry,
                                       std::vector<std::string_view>{"login"},
                                       environment, output, error) == 0);
  REQUIRE(login.calls == 1);
  REQUIRE(login.saw_terminal_input);
  REQUIRE(login.input_descriptor == 42);
  REQUIRE(output.str() == "stored\n");
  REQUIRE(error.str() == "prompt\n");

  output.str({});
  error.str({});
  login.failure = CommandFailure{CommandFailureKind::cancelled, "stopped"};
  REQUIRE(CommandDispatcher{}.dispatch(registry,
                                       std::vector<std::string_view>{"login"},
                                       environment, output, error) == 130);
  REQUIRE(error.str().find("stopped") != std::string::npos);
}

TEST_CASE("empty terminal root input routes to the interactive service",
          "[commands][interactive]") {
  const auto& registry = builtin_command_registry();
  FakeOneShot one_shot;
  FakeInteractive interactive;
  std::istringstream input;
  CommandEnvironment environment{input, true,      true,        true,
                                 {},    &one_shot, &interactive};
  std::ostringstream output;
  std::ostringstream error;

  REQUIRE(CommandDispatcher{}.dispatch(registry, {}, environment, output,
                                       error) == 0);
  REQUIRE(output.str() == "interactive");
  REQUIRE(error.str().empty());
  REQUIRE(interactive.seen_session_mode ==
          InteractiveCommand::SessionMode::create);

  output.str({});
  error.str({});
  interactive.failure =
      CommandFailure{CommandFailureKind::cancelled, "stopped"};
  REQUIRE(CommandDispatcher{}.dispatch(
              registry, std::vector<std::string_view>{"--ephemeral"},
              environment, output, error) == 130);
  REQUIRE(error.str().find("stopped") != std::string::npos);
}
