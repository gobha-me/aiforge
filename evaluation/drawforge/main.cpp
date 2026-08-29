#include <aiforge/adapters/process_credentials.hpp>
#include <aiforge/adapters/process_tool.hpp>
#include <aiforge/adapters/venice_backend.hpp>
#include <aiforge/backend/backend.hpp>
#include <aiforge/domain/money.hpp>
#include <aiforge/runtime/tool_policy.hpp>
#include <aiforge/runtime/tool_registry.hpp>
#include <aiforge/storage/artifact_store.hpp>
#include <aiforge/surfaces/one_shot.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::uint64_t budget_microusd{3'000'000};

struct Arguments {
  fs::path run;
  fs::path matrix_root;
  fs::path drawforge;
  fs::path helper;
};

[[nodiscard]] auto path_is_within(const fs::path& child, const fs::path& root)
    -> bool {
  const auto mismatch =
      std::mismatch(root.begin(), root.end(), child.begin(), child.end());
  return mismatch.first == root.end();
}

[[nodiscard]] auto parse_arguments(const int argc, char** argv)
    -> std::expected<Arguments, std::string> {
  Arguments result;
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) return std::unexpected("missing option value");
    const std::string_view option{argv[index]};
    const fs::path value{argv[index + 1]};
    if (option == "--run")
      result.run = value;
    else if (option == "--matrix-root")
      result.matrix_root = value;
    else if (option == "--drawforge")
      result.drawforge = value;
    else if (option == "--helper")
      result.helper = value;
    else
      return std::unexpected("unknown option: " + std::string{option});
  }
  if (result.run.empty() || result.matrix_root.empty() ||
      result.drawforge.empty()) {
    return std::unexpected(
        "usage: aiforge_drawforge_evaluation --run DIR --matrix-root DIR "
        "--drawforge FILE [--helper FILE]");
  }
  if (result.helper.empty()) {
    result.helper = fs::path{__FILE__}.parent_path() / "tool.py";
  }
  try {
    result.run = fs::canonical(result.run);
    result.matrix_root = fs::canonical(result.matrix_root);
    result.drawforge = fs::canonical(result.drawforge);
    result.helper = fs::canonical(result.helper);
  } catch (const fs::filesystem_error&) {
    return std::unexpected("an evaluation path does not exist");
  }
  if (!fs::is_directory(result.run) || !fs::is_directory(result.matrix_root) ||
      !fs::is_regular_file(result.drawforge) ||
      !fs::is_regular_file(result.helper)) {
    return std::unexpected("an evaluation path has the wrong type");
  }
  if (!path_is_within(result.run, result.matrix_root)) {
    return std::unexpected("run directory is outside the matrix root");
  }
  return result;
}

class MatrixLock final {
 public:
  [[nodiscard]] static auto acquire(const fs::path& root)
      -> std::expected<MatrixLock, std::string> {
    try {
      auto path = root / ".aiforge-drawforge-eval.lock";
      if (!fs::create_directory(path)) {
        return std::unexpected("another matrix runner is active");
      }
      return MatrixLock{std::move(path)};
    } catch (const fs::filesystem_error&) {
      return std::unexpected("could not acquire the matrix runner lock");
    }
  }

  MatrixLock(const MatrixLock&) = delete;
  auto operator=(const MatrixLock&) -> MatrixLock& = delete;
  MatrixLock(MatrixLock&& other) noexcept
      : m_path(std::exchange(other.m_path, {})) {}
  auto operator=(MatrixLock&&) -> MatrixLock& = delete;

  ~MatrixLock() {
    if (m_path.empty()) return;
    std::error_code error;
    static_cast<void>(fs::remove(m_path, error));
  }

 private:
  explicit MatrixLock(fs::path path) : m_path(std::move(path)) {}
  fs::path m_path;
};

[[nodiscard]] auto read_json(const fs::path& path)
    -> std::expected<Json, std::string> {
  try {
    std::ifstream input{path};
    if (!input) return std::unexpected("could not read " + path.string());
    auto value = Json::parse(input);
    if (!value.is_object()) {
      return std::unexpected(path.string() + " must contain an object");
    }
    return value;
  } catch (const std::exception&) {
    return std::unexpected("could not parse " + path.string());
  }
}

[[nodiscard]] auto write_json(const fs::path& path, const Json& value)
    -> std::expected<void, std::string> {
  try {
    const auto temporary = path.string() + ".new";
    {
      std::ofstream output{temporary, std::ios::trunc};
      output << value.dump(2) << '\n';
      if (!output) return std::unexpected("could not write run metadata");
    }
    fs::rename(temporary, path);
    return {};
  } catch (const fs::filesystem_error&) {
    return std::unexpected("could not replace run metadata");
  }
}

[[nodiscard]] auto read_text(const fs::path& path)
    -> std::expected<std::string, std::string> {
  std::ifstream input{path, std::ios::binary};
  if (!input) return std::unexpected("could not read " + path.string());
  std::ostringstream contents;
  contents << input.rdbuf();
  if (input.bad()) return std::unexpected("could not read " + path.string());
  return std::move(contents).str();
}

[[nodiscard]] auto microusd(const Json& value)
    -> std::expected<std::uint64_t, std::string> {
  if (!value.is_number()) return std::unexpected("cost_usd is not numeric");
  const auto amount = value.get<double>();
  if (!std::isfinite(amount) || amount < 0.0 || amount > 3.0) {
    return std::unexpected("cost_usd is outside the matrix ceiling");
  }
  const auto rounded_up = std::ceil(amount * 1'000'000.0);
  return static_cast<std::uint64_t>(rounded_up);
}

[[nodiscard]] auto remaining_budget(const fs::path& matrix_root)
    -> std::expected<std::uint64_t, std::string> {
  std::uint64_t total{};
  try {
    for (const auto& entry : fs::recursive_directory_iterator{matrix_root}) {
      if (!entry.is_regular_file() || entry.path().filename() != "run.json")
        continue;
      auto metadata = read_json(entry.path());
      if (!metadata) return std::unexpected(std::move(metadata.error()));
      if (!metadata->contains("usage") || !(*metadata)["usage"].is_object() ||
          !(*metadata)["usage"].contains("cost_usd")) {
        return std::unexpected("matrix run metadata lacks cost evidence");
      }
      const auto& usage = (*metadata)["usage"];
      const auto& cost = usage["cost_usd"];
      const auto started =
          fs::exists(entry.path().parent_path() / ".aiforge-eval-state.json") ||
          fs::exists(entry.path().parent_path() / "assistant.txt") ||
          fs::exists(entry.path().parent_path() / "result.json");
      if (cost.is_null()) {
        if (started) {
          return std::unexpected(
              "a started matrix run lacks provider-reported USD cost");
        }
        continue;
      }
      auto amount = microusd(cost);
      if (!amount) return std::unexpected(std::move(amount.error()));
      if (*amount > budget_microusd - std::min(total, budget_microusd)) {
        return std::unexpected("matrix spend reaches the USD 3 ceiling");
      }
      total += *amount;
    }
  } catch (const fs::filesystem_error&) {
    return std::unexpected("could not inspect matrix spend evidence");
  }
  if (total >= budget_microusd) {
    return std::unexpected("matrix spend reaches the USD 3 ceiling");
  }
  return budget_microusd - total;
}

[[nodiscard]] auto ceiling_text(const std::uint64_t micros) -> std::string {
  return std::to_string(micros / 1'000'000) + "." + [&] {
    auto fraction = std::to_string(micros % 1'000'000);
    fraction.insert(0, 6U - fraction.size(), '0');
    return fraction;
  }();
}

class RejectingArtifactStore final : public aiforge::storage::ArtifactStore {
 public:
  [[nodiscard]] auto put(aiforge::storage::ArtifactWrite,
                         std::span<const std::byte>, std::stop_token)
      -> std::expected<aiforge::domain::ArtifactMetadata,
                       aiforge::storage::ArtifactStoreError> override {
    return std::unexpected(aiforge::storage::ArtifactStoreError{
        aiforge::storage::ArtifactStoreErrorCode::unavailable,
        "evaluation output must remain inline", false});
  }
};

class EvaluationBackend final : public aiforge::backend::Backend,
                                public aiforge::backend::ModelContextProvider {
 public:
  EvaluationBackend(aiforge::adapters::VeniceBackend& backend,
                    std::optional<double> temperature,
                    std::optional<std::uint64_t> seed)
      : m_backend(backend), m_temperature(temperature), m_seed(seed) {}

  [[nodiscard]] auto start(aiforge::backend::BackendRequest request,
                           std::stop_token stop_token)
      -> std::expected<std::unique_ptr<aiforge::backend::BackendStream>,
                       aiforge::backend::BackendError> override {
    request.options.temperature = m_temperature;
    request.options.seed = m_seed;
    return m_backend.start(std::move(request), stop_token);
  }

  [[nodiscard]] auto lookup(const aiforge::domain::ModelId& model_id,
                            std::stop_token stop_token)
      -> std::expected<aiforge::backend::ModelContextInfo,
                       aiforge::backend::BackendError> override {
    return m_backend.lookup(model_id, stop_token);
  }

 private:
  aiforge::adapters::VeniceBackend& m_backend;
  std::optional<double> m_temperature;
  std::optional<std::uint64_t> m_seed;
};

class EvaluationPolicy final : public aiforge::runtime::ToolPolicy {
 public:
  EvaluationPolicy(std::vector<aiforge::domain::Effect> effects,
                   std::vector<aiforge::domain::CapabilityScope> scopes)
      : m_effects(std::move(effects)), m_scopes(std::move(scopes)) {}

  [[nodiscard]] auto evaluate(
      const aiforge::runtime::ToolPolicyRequest& request)
      -> std::expected<aiforge::runtime::ToolPolicyResolution,
                       aiforge::runtime::ToolPolicyError> override {
    if (request.tool_name == "run_process" && request.effects == m_effects &&
        request.scopes == m_scopes) {
      return aiforge::runtime::ToolPolicyResolution{
          aiforge::domain::PolicyDecision::allow, request.scopes,
          "allowed by the fixed DrawForge evaluation profile",
          aiforge::domain::PolicyDecisionSource::permission_profile};
    }
    return aiforge::runtime::ToolPolicyResolution{
        aiforge::domain::PolicyDecision::deny,
        {},
        "outside the fixed DrawForge evaluation profile",
        aiforge::domain::PolicyDecisionSource::permission_profile};
  }

  [[nodiscard]] auto approve(const aiforge::runtime::ToolPolicyRequest&,
                             aiforge::runtime::ToolPolicyApproval)
      -> std::expected<aiforge::runtime::ToolPolicyResolution,
                       aiforge::runtime::ToolPolicyError> override {
    return aiforge::runtime::ToolPolicyResolution{
        aiforge::domain::PolicyDecision::deny,
        {},
        "interactive approval is disabled for evaluation",
        aiforge::domain::PolicyDecisionSource::permission_profile};
  }

 private:
  std::vector<aiforge::domain::Effect> m_effects;
  std::vector<aiforge::domain::CapabilityScope> m_scopes;
};

[[nodiscard]] auto tool_instructions(const Arguments& arguments)
    -> std::string {
  const auto helper = arguments.helper.string();
  const auto run = arguments.run.string();
  return "\n\nUse run_process for every submission. Call it with exactly this "
         "shape, replacing PAYLOAD with one complete SVG string for direct-svg "
         "or one DrawForge request object encoded as a JSON string for "
         "semantic:\n"
         "{\"executable\":" +
         Json{helper}.dump() +
         ",\"arguments\":[\"submit\",\"PAYLOAD\"],\"working_directory\":" +
         Json{run}.dump() + ",\"readable_roots\":[" + Json{run}.dump() +
         "],\"writable_roots\":[" + Json{run}.dump() +
         "],\"environment\":[\"DRAWFORGE_EVAL_BINARY\","
         "\"DRAWFORGE_EVAL_RUN\"],\"stdin\":\"closed\","
         "\"timeout_ms\":30000,\"output_bytes\":8192}. "
         "You have at most three submissions and twelve total tool "
         "interactions. "
         "A tool error is authoritative: correct it and retry when retryable. "
         "Finish only after the tool reports an accepted submission.";
}

[[nodiscard]] auto source_evidence(const Arguments& arguments,
                                   const Json& metadata)
    -> std::expected<std::string, std::string> {
  const auto route = metadata.value("route", "");
  const auto name = route == "direct-svg" ? "source.svg" : "source.jsonl";
  const auto path = arguments.run / name;
  if (!fs::exists(path))
    return std::string{"No starting document is supplied."};
  auto source = read_text(path);
  if (!source) return std::unexpected(std::move(source.error()));
  return "Starting document (" + std::string{name} + "):\n" + *source;
}

[[nodiscard]] auto usd_cost(const aiforge::domain::ReportedCost& cost)
    -> std::expected<std::string, std::string> {
  for (const auto& amount : cost.amounts()) {
    if (amount.unit() == "USD") return amount.amount().to_string();
  }
  return std::unexpected("provider-reported cost contains no USD amount");
}

auto write_text(const fs::path& path, const std::string& value) -> bool {
  std::ofstream output{path, std::ios::trunc};
  output << value;
  return static_cast<bool>(output);
}

} // namespace

auto main(const int argc, char** argv) -> int {
  try {
    auto arguments = parse_arguments(argc, argv);
    if (!arguments) {
      std::cerr << "aiforge-drawforge-eval: " << arguments.error() << '\n';
      return 2;
    }
    auto matrix_lock = MatrixLock::acquire(arguments->matrix_root);
    if (!matrix_lock) {
      std::cerr << "aiforge-drawforge-eval: " << matrix_lock.error() << '\n';
      return 2;
    }
    auto metadata = read_json(arguments->run / "run.json");
    if (!metadata) {
      std::cerr << "aiforge-drawforge-eval: " << metadata.error() << '\n';
      return 2;
    }
    if ((*metadata).value("schema_version", 0) != 2 ||
        (*metadata).value("corpus_id", "") != "drawforge-semantic-svg-v2" ||
        ((*metadata).value("route", "") != "direct-svg" &&
         (*metadata).value("route", "") != "semantic")) {
      std::cerr << "aiforge-drawforge-eval: run metadata is not protocol v2\n";
      return 2;
    }
    auto budget = remaining_budget(arguments->matrix_root);
    if (!budget) {
      std::cerr << "aiforge-drawforge-eval: " << budget.error() << '\n';
      return 2;
    }
    auto prompt = read_text(arguments->run / "prompt.md");
    auto evidence = source_evidence(*arguments, *metadata);
    if (!prompt || !evidence) {
      std::cerr << "aiforge-drawforge-eval: run inputs could not be read\n";
      return 2;
    }
    auto model = aiforge::domain::ModelId::from(
        (*metadata).at("model").at("id").get<std::string>());
    const auto ceiling =
        aiforge::domain::SessionSpendCeiling::from(ceiling_text(*budget));
    if (!model || !ceiling) {
      std::cerr
          << "aiforge-drawforge-eval: model or spend ceiling is invalid\n";
      return 2;
    }
    std::optional<std::uint64_t> seed;
    if (!(*metadata)["sampling"]["seed"].is_null()) {
      const auto raw = (*metadata)["sampling"]["seed"].get<std::int64_t>();
      if (raw < 0) {
        std::cerr
            << "aiforge-drawforge-eval: Venice seed must be non-negative\n";
        return 2;
      }
      seed = static_cast<std::uint64_t>(raw);
    }
    std::optional<double> temperature;
    if (!(*metadata)["sampling"]["temperature"].is_null()) {
      temperature = (*metadata)["sampling"]["temperature"].get<double>();
    }

    std::ostringstream credential_diagnostics;
    auto credential =
        aiforge::adapters::resolve_process_credential(credential_diagnostics);
    if (!credential || !credential->credential) {
      std::cerr << credential_diagnostics.str()
                << "aiforge-drawforge-eval: Venice credential is unavailable\n";
      return 2;
    }
    auto resolved = std::move(*credential->credential);
    aiforge::adapters::VeniceBackend venice{std::move(resolved.secret)};
    EvaluationBackend backend{venice, temperature, seed};

    RejectingArtifactStore artifact_store;
    aiforge::runtime::ToolRegistry registry;
    aiforge::adapters::ProcessToolConfiguration configuration;
    configuration.executable_allowlist = {arguments->helper};
    configuration.readable_roots = {arguments->run};
    configuration.writable_roots = {arguments->run};
    configuration.environment_allowlist = {
        {"DRAWFORGE_EVAL_BINARY", arguments->drawforge.string()},
        {"DRAWFORGE_EVAL_RUN", arguments->run.string()}};
    configuration.limits.timeout = std::chrono::seconds{30};
    configuration.limits.argument_bytes = std::size_t{256} * 1024U;
    configuration.limits.output_bytes = 8192;
    configuration.limits.inline_output_bytes = 8192;
    auto registered = aiforge::adapters::register_process_tool(
        registry, artifact_store, configuration);
    auto tools = registry.snapshot();
    if (!registered || !tools) {
      std::cerr << "aiforge-drawforge-eval: process tool setup failed\n";
      return 2;
    }
    std::vector<aiforge::domain::Effect> effects{
        aiforge::domain::Effect::execute, aiforge::domain::Effect::read,
        aiforge::domain::Effect::write};
    std::vector<aiforge::domain::CapabilityScope> scopes{
        {aiforge::domain::Effect::execute, "process.command",
         arguments->helper.string()},
        {aiforge::domain::Effect::read, "filesystem.root",
         arguments->run.string()},
        {aiforge::domain::Effect::write, "filesystem.root",
         arguments->run.string()}};
    auto policy = std::make_shared<EvaluationPolicy>(effects, scopes);
    aiforge::surfaces::OneShotDependencies dependencies{
        std::move(*tools),          policy, nullptr, {}, std::nullopt,
        "aiforge-drawforge-eval-v1"};
    aiforge::surfaces::OneShotSurface surface{
        backend, backend, {}, nullptr, std::move(dependencies)};
    aiforge::surfaces::OneShotRequest request{
        *prompt + tool_instructions(*arguments),
        *evidence,
        std::move(*model),
        aiforge::surfaces::OneShotRequest::SessionMode::ephemeral,
        std::nullopt,
        std::nullopt,
        {},
        *ceiling};
    std::ostringstream assistant;
    std::ostringstream diagnostics;
    auto result = surface.run(std::move(request), assistant, diagnostics);
    if (!write_text(arguments->run / "assistant.txt", assistant.str()) ||
        !write_text(arguments->run / "runtime.log",
                    credential_diagnostics.str() + diagnostics.str())) {
      std::cerr << "aiforge-drawforge-eval: could not write bounded outputs\n";
      return 2;
    }
    if (!result) {
      std::cerr << "aiforge-drawforge-eval: " << result.error().message << '\n';
      return 1;
    }
    if (!result->reported_cost) {
      std::cerr << "aiforge-drawforge-eval: provider did not report cost\n";
      return 1;
    }
    auto cost = usd_cost(*result->reported_cost);
    if (!cost) {
      std::cerr << "aiforge-drawforge-eval: " << cost.error() << '\n';
      return 1;
    }
    auto refreshed = read_json(arguments->run / "run.json");
    if (!refreshed) {
      std::cerr << "aiforge-drawforge-eval: run metadata disappeared\n";
      return 1;
    }
    (*refreshed)["usage"]["input_tokens"] = result->usage.input_tokens;
    (*refreshed)["usage"]["output_tokens"] = result->usage.output_tokens;
    (*refreshed)["usage"]["cost_usd"] = std::stod(*cost);
    auto written = write_json(arguments->run / "run.json", *refreshed);
    if (!written) {
      std::cerr << "aiforge-drawforge-eval: " << written.error() << '\n';
      return 1;
    }
    const auto& events = (*refreshed)["events"];
    if (!events.is_array() ||
        std::find(events.begin(), events.end(), "submission_accepted") ==
            events.end()) {
      std::cerr << "aiforge-drawforge-eval: no submission was accepted\n";
      return 1;
    }
    std::cout << assistant.str();
    return 0;
  } catch (const std::exception&) {
    std::cerr << "aiforge-drawforge-eval: internal failure\n";
    return 2;
  }
}
