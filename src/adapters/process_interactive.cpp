#include <aiforge/adapters/ask_user_dialog.hpp>
#include <aiforge/adapters/filesystem_persona_source.hpp>
#include <aiforge/adapters/filesystem_user_global_instruction_source.hpp>
#include <aiforge/adapters/git_exact_source_editor.hpp>
#include <aiforge/adapters/interactive_chat_app.hpp>
#include <aiforge/adapters/model_picker_dialog.hpp>
#include <aiforge/adapters/persona_editor_dialog.hpp>
#include <aiforge/adapters/process_credentials.hpp>
#include <aiforge/adapters/process_draft_editor.hpp>
#include <aiforge/adapters/process_interactive.hpp>
#include <aiforge/adapters/process_model_catalog.hpp>
#include <aiforge/adapters/process_provenance.hpp>
#include <aiforge/adapters/process_repository.hpp>
#include <aiforge/adapters/provider_character_picker_dialog.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/adapters/termforge_run_bridge.hpp>
#include <aiforge/adapters/tool_approval_dialog.hpp>
#include <aiforge/adapters/transcript_view.hpp>
#include <aiforge/adapters/venice_backend.hpp>
#include <aiforge/adapters/venice_generation_options.hpp>
#include <aiforge/config/config.hpp>
#include <aiforge/config/file_store.hpp>
#include <aiforge/config/provenance.hpp>
#include <aiforge/domain/tool_spend.hpp>
#include <aiforge/domain/usage_ledger.hpp>
#include <aiforge/runtime/ask_user_tool.hpp>
#include <aiforge/runtime/memory_controller.hpp>
#include <aiforge/runtime/memory_tool.hpp>
#include <aiforge/runtime/repository_read_tool.hpp>
#include <aiforge/runtime/tool_launch_policy.hpp>
#include <aiforge/runtime/tool_profiles.hpp>
#include <aiforge/runtime/tool_registry.hpp>
#include <aiforge/surfaces/chat_session.hpp>
#include <aiforge/surfaces/slash_commands.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <concepts>
#include <cstdlib>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <termforge/core/app.hpp>
#include <termforge/widgets/choice_wizard_dialog.hpp>
#include <termforge/widgets/composer.hpp>
#include <termforge/widgets/detail/width.hpp>
#include <termforge/widgets/focus_ring.hpp>
#include <termforge/widgets/text_box.hpp>
#include <utility>
#include <variant>
#include <version.hpp>

namespace aiforge::adapters {
namespace {

class CredentialUnavailableBackend final : public backend::Backend {
 public:
  [[nodiscard]] auto start(backend::BackendRequest, std::stop_token stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    if (stop_token.stop_requested()) {
      return std::unexpected(backend::BackendError{
          backend::BackendErrorKind::cancelled, "Venice request cancelled",
          false, std::nullopt});
    }
    return std::unexpected(
        backend::BackendError{backend::BackendErrorKind::credential_unavailable,
                              "Venice credential is not configured; run "
                              "'aiforge login' or set VENICE_API_KEY",
                              false, std::nullopt});
  }
};

constexpr std::size_t interactive_session_list_limit = 100U;

[[nodiscard]] auto web_search_setting_name(const VeniceWebSearchSetting setting)
    -> std::string_view {
  switch (setting) {
    case VeniceWebSearchSetting::inherit: return "inherit/provider default";
    case VeniceWebSearchSetting::automatic: return "auto";
    case VeniceWebSearchSetting::on: return "on";
    case VeniceWebSearchSetting::off: return "off";
  }
  return "unknown";
}

[[nodiscard]] auto system_prompt_setting_name(
    const VeniceSystemPromptSetting setting) -> std::string_view {
  switch (setting) {
    case VeniceSystemPromptSetting::inherit: return "inherit/provider default";
    case VeniceSystemPromptSetting::include: return "include";
    case VeniceSystemPromptSetting::exclude: return "exclude";
  }
  return "unknown";
}

[[nodiscard]] auto tool_restriction(const std::optional<std::string>& requested)
    -> std::expected<runtime::RestrictionLevel, std::string> {
  if (!requested || *requested == "high") {
    return runtime::RestrictionLevel::high;
  }
  if (*requested == "medium") return runtime::RestrictionLevel::medium;
  if (*requested == "low") return runtime::RestrictionLevel::low;
  if (*requested == "none") return runtime::RestrictionLevel::none;
  return std::unexpected("tool restriction must be none, low, medium, or high");
}

[[nodiscard]] auto tool_approval(const std::optional<std::string>& requested)
    -> std::expected<runtime::ApprovalMode, std::string> {
  if (!requested || *requested == "prompt") {
    return runtime::ApprovalMode::prompt;
  }
  if (*requested == "auto") return runtime::ApprovalMode::automatic;
  if (*requested == "allow-all") return runtime::ApprovalMode::allow_all;
  return std::unexpected("tool approval must be prompt, auto, or allow-all");
}

[[nodiscard]] auto tool_launch_profile_id(
    const runtime::RestrictionLevel restriction,
    const runtime::ApprovalMode approval)
    -> std::optional<domain::PermissionProfileId> {
  const auto restriction_name = [restriction]() -> std::string_view {
    switch (restriction) {
      case runtime::RestrictionLevel::high: return "high";
      case runtime::RestrictionLevel::medium: return "medium";
      case runtime::RestrictionLevel::low: return "low";
      case runtime::RestrictionLevel::none: return "none";
    }
    return "invalid";
  }();
  const auto approval_name = [approval]() -> std::string_view {
    switch (approval) {
      case runtime::ApprovalMode::prompt: return "prompt";
      case runtime::ApprovalMode::automatic: return "auto";
      case runtime::ApprovalMode::allow_all: return "allow-all";
    }
    return "invalid";
  }();
  auto profile = domain::PermissionProfileId::from(
      "tools-" + std::string{restriction_name} + "-" +
      std::string{approval_name} + "-v1");
  if (!profile) return std::nullopt;
  return std::move(*profile);
}

[[nodiscard]] auto configured_source_name(
    const std::optional<config::ConfigSource> source) -> std::string_view {
  return source ? config::config_source_name(*source) : "provider default";
}

[[nodiscard]] auto failure(const cli::CommandFailureKind kind,
                           std::string message)
    -> std::unexpected<cli::CommandFailure> {
  return std::unexpected(cli::CommandFailure{kind, std::move(message)});
}

auto warning(std::ostream& stream, const std::string_view message) -> bool {
  try {
    stream << "aiforge: warning: " << message << '\n';
    return static_cast<bool>(stream);
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto common_prefix(const std::vector<std::string>& values)
    -> std::string {
  if (values.empty()) return {};
  auto result = values.front();
  for (const auto& value : values) {
    const auto length =
        std::mismatch(result.begin(), result.end(), value.begin(), value.end())
            .first -
        result.begin();
    result.resize(static_cast<std::size_t>(length));
  }
  return result;
}

[[nodiscard]] auto completion_status(const std::vector<std::string>& values)
    -> std::string {
  std::string result{"Matches:"};
  for (const auto& value : values) {
    result += " /";
    result += value;
  }
  return result;
}

[[nodiscard]] auto format_timestamp(const domain::EventTimestamp timestamp)
    -> std::string {
  const auto day = std::chrono::floor<std::chrono::days>(timestamp);
  const std::chrono::year_month_day date{day};
  if (!date.ok()) {
    return std::to_string(timestamp.time_since_epoch().count()) +
           "ms since epoch";
  }
  const std::chrono::hh_mm_ss time{timestamp - day};
  return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
                     static_cast<int>(date.year()),
                     static_cast<unsigned>(date.month()),
                     static_cast<unsigned>(date.day()), time.hours().count(),
                     time.minutes().count(), time.seconds().count(),
                     time.subseconds().count());
}

[[nodiscard]] auto next_memory_suffix() -> std::uint64_t {
  static std::atomic<std::uint64_t> sequence{};
  const auto count = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto tick = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return tick ^ count;
}

[[nodiscard]] auto current_timestamp() -> domain::EventTimestamp {
  return std::chrono::floor<std::chrono::milliseconds>(
      std::chrono::system_clock::now());
}

[[nodiscard]] auto runtime_version() -> std::string {
  return std::format("{}.{}.{}", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
}

[[nodiscard]] auto memory_scope_text(const domain::MemoryScope scope)
    -> std::string_view {
  return scope == domain::MemoryScope::project ? "project" : "global";
}

[[nodiscard]] auto memory_kind_text(const domain::MemoryKind kind)
    -> std::string_view {
  switch (kind) {
    case domain::MemoryKind::user_preference: return "preference";
    case domain::MemoryKind::project_convention: return "convention";
    case domain::MemoryKind::workflow: return "workflow";
    case domain::MemoryKind::reusable_fact: return "fact";
    case domain::MemoryKind::unknown: return "unknown";
  }
  return "unknown";
}

[[nodiscard]] auto lower_copy(const std::string_view value) -> std::string {
  std::string result{value};
  std::ranges::transform(result, result.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return result;
}

[[nodiscard]] auto take_word(std::string_view& value) -> std::string_view {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    value = {};
    return {};
  }
  value.remove_prefix(first);
  const auto end = value.find_first_of(" \t");
  if (end == std::string_view::npos) {
    const auto result = value;
    value = {};
    return result;
  }
  const auto result = value.substr(0, end);
  value.remove_prefix(end);
  return result;
}

[[nodiscard]] auto trim_words(std::string_view value) -> std::string_view {
  const auto first = value.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  const auto last = value.find_last_not_of(" \t");
  return value.substr(first, last - first + 1);
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicit precedence checks.
[[nodiscard]] auto load_config(
    std::ostream& diagnostics,
    const std::optional<std::string>& requested_model,
    const std::optional<std::string>& requested_web_search,
    std::optional<config::ConfigCandidate> file_override = std::nullopt)
    -> std::expected<config::ResolvedConfig, cli::CommandFailure> {
  // clang-format on
  const auto& registry = config::builtin_config_registry();
  std::vector<config::ConfigLayer> layers;
  std::vector<config::ConfigCandidate> command_line;
  if (requested_model) {
    command_line.push_back(
        {"model", config::ConfigValue{*requested_model}, std::nullopt});
  }
  if (requested_web_search) {
    command_line.push_back({"venice.web_search",
                            config::ConfigValue{*requested_web_search},
                            std::nullopt});
  }
  if (!command_line.empty()) {
    layers.push_back(config::ConfigLayer{
        config::ConfigSource::command_line, std::move(command_line), {}});
  }
  auto environment = config::environment_config_layer(registry);
  if (!environment) {
    return failure(cli::CommandFailureKind::runtime,
                   "configuration environment could not be read");
  }
  layers.push_back(std::move(*environment));
  auto path = config::process_config_path();
  if (!path) {
    if (file_override) {
      return failure(cli::CommandFailureKind::runtime, path.error().message);
    }
    if (!warning(diagnostics, path.error().message)) {
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
    }
  } else {
    auto file = config::JsonConfigFileStore{*path}.load(registry);
    if (file) {
      if (file_override) {
        std::erase_if(file->candidates, [&](const auto& candidate) {
          return candidate.key == file_override->key;
        });
        if (file_override->value) {
          file->candidates.push_back(std::move(*file_override));
        }
      }
      layers.push_back(std::move(*file));
    } else {
      // An unreadable document may contain narrowing ceilings. Ignoring it
      // could silently widen tool availability, so interactive startup fails
      // closed whether or not this load was preparing a mutation.
      return failure(cli::CommandFailureKind::runtime, file.error().message);
    }
  }
  auto resolved = config::resolve_config(registry, layers);
  if (!resolved) {
    return failure(cli::CommandFailureKind::runtime,
                   "configuration could not be resolved");
  }
  for (const auto& diagnostic : resolved->diagnostics) {
    if (!warning(diagnostics, diagnostic.message)) {
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
    }
  }
  return std::move(*resolved);
}

struct VeniceConfigMutation {
  std::string key;
  std::optional<config::ConfigValue> value;
};

[[nodiscard]] auto venice_config_mutation(const VeniceRequestSettingSave& save)
    -> std::expected<VeniceConfigMutation, std::string> {
  const bool web = save.web_search.has_value();
  const bool prompt = save.system_prompt.has_value();
  if (web == prompt) {
    return std::unexpected(
        "exactly one request setting must be saved at a time");
  }
  if (web) {
    switch (*save.web_search) {
      case VeniceWebSearchSetting::inherit:
        return VeniceConfigMutation{"venice.web_search", std::nullopt};
      case VeniceWebSearchSetting::automatic:
        return VeniceConfigMutation{"venice.web_search",
                                    config::ConfigValue{std::string{"auto"}}};
      case VeniceWebSearchSetting::on:
        return VeniceConfigMutation{"venice.web_search",
                                    config::ConfigValue{std::string{"on"}}};
      case VeniceWebSearchSetting::off:
        return VeniceConfigMutation{"venice.web_search",
                                    config::ConfigValue{std::string{"off"}}};
    }
    return std::unexpected("Venice web-search setting is invalid");
  }
  switch (*save.system_prompt) {
    case VeniceSystemPromptSetting::inherit:
      return VeniceConfigMutation{"venice.include_system_prompt", std::nullopt};
    case VeniceSystemPromptSetting::include:
      return VeniceConfigMutation{"venice.include_system_prompt",
                                  config::ConfigValue{true}};
    case VeniceSystemPromptSetting::exclude:
      return VeniceConfigMutation{"venice.include_system_prompt",
                                  config::ConfigValue{false}};
  }
  return std::unexpected("Venice system-prompt setting is invalid");
}

[[nodiscard]] auto persist_tool_profile_maximum_mapping(
    config::JsonConfigFileStore& store,
    config::ToolProfileMaximumMappings& mappings,
    const ToolProfileMaximumSave& save)
    -> std::expected<void, ToolProfileMaximumPersistError> {
  std::string_view key;
  std::string subject;
  if (const auto* model = std::get_if<domain::ModelId>(&save.subject)) {
    key = config::model_maximum_tool_profiles_key;
    subject = model->value();
  } else {
    const auto& persona = std::get<domain::PersonaId>(save.subject);
    key = config::persona_maximum_tool_profiles_key;
    subject = persona.value();
  }
  auto persisted = store.update_text_map_entry(
      config::builtin_config_registry(), key, std::move(subject),
      save.maximum_profile_id
          ? std::optional<std::string>{save.maximum_profile_id->value()}
          : std::nullopt);
  if (!persisted) {
    return std::unexpected(ToolProfileMaximumPersistError{
        persisted.error().message, persisted.error().effect_may_have_applied});
  }
  if (const auto* model = std::get_if<domain::ModelId>(&save.subject)) {
    if (save.maximum_profile_id) {
      mappings.models.insert_or_assign(*model, *save.maximum_profile_id);
    } else {
      mappings.models.erase(*model);
    }
  } else {
    const auto& persona = std::get<domain::PersonaId>(save.subject);
    if (save.maximum_profile_id) {
      mappings.personas.insert_or_assign(persona, *save.maximum_profile_id);
    } else {
      mappings.personas.erase(persona);
    }
  }
  return {};
}

[[nodiscard]] auto configured_model(const config::ResolvedConfig& resolved)
    -> std::expected<std::optional<domain::ModelId>, cli::CommandFailure> {
  const auto* entry = resolved.find("model");
  if (entry == nullptr || !entry->value) {
    return std::nullopt;
  }
  const auto* text = std::get_if<std::string>(&*entry->value);
  if (text == nullptr || text->empty()) {
    return failure(cli::CommandFailureKind::runtime,
                   "configured model is invalid");
  }
  auto model = domain::ModelId::from(*text);
  if (!model) {
    return failure(cli::CommandFailureKind::runtime,
                   "configured model is invalid");
  }
  return std::optional<domain::ModelId>{std::move(*model)};
}

[[nodiscard]] auto select_startup_model(
    model::CatalogService& catalog,
    const backend::GenerationOptions& generation_options,
    const std::stop_token stop_token)
    -> std::expected<domain::ModelId, cli::CommandFailure> {
  auto snapshot = catalog.snapshot(stop_token);
  if (!snapshot) {
    return failure(snapshot.error().code == model::CatalogErrorCode::cancelled
                       ? cli::CommandFailureKind::cancelled
                       : cli::CommandFailureKind::runtime,
                   snapshot.error().message);
  }
  const bool has_selectable = std::ranges::any_of(
      snapshot->get().entries, [](const model::CatalogEntry& entry) {
        return entry.type == "text" && !entry.offline &&
               entry.context_window_tokens.has_value();
      });
  if (!has_selectable) {
    return failure(cli::CommandFailureKind::runtime,
                   "model catalog contains no selectable text models");
  }
  auto picker = make_interactive_model_picker_app(snapshot->get(), stop_token);
  const int picker_result = picker->run();
  if (stop_token.stop_requested() || picker->cancelled()) {
    return failure(cli::CommandFailureKind::cancelled,
                   "model selection cancelled");
  }
  if (picker_result != 0) {
    return failure(cli::CommandFailureKind::runtime,
                   "model selection terminal setup failed");
  }
  auto selected = picker->selected_model();
  if (!selected) {
    return failure(cli::CommandFailureKind::runtime,
                   "model selection produced no result");
  }
  if (auto valid =
          validate_interactive_model_selection(snapshot->get(), *selected);
      !valid) {
    return failure(cli::CommandFailureKind::runtime, valid.error());
  }
  auto context = catalog.lookup(*selected, stop_token);
  if (!context) {
    return failure(context.error().kind == backend::BackendErrorKind::cancelled
                       ? cli::CommandFailureKind::cancelled
                       : cli::CommandFailureKind::runtime,
                   context.error().redacted_message);
  }
  if (auto supported = backend::validate_generation_requirements(
          generation_options, *context);
      !supported) {
    return failure(cli::CommandFailureKind::runtime,
                   supported.error().redacted_message);
  }
  return std::move(*selected);
}

[[nodiscard]] auto resolve_interactive_model(
    const config::ResolvedConfig& resolved, model::CatalogService& catalog,
    const backend::GenerationOptions& generation_options,
    const std::stop_token stop_token)
    -> std::expected<domain::ModelId, cli::CommandFailure> {
  auto configured = configured_model(resolved);
  if (!configured) return std::unexpected(std::move(configured.error()));
  if (*configured) return std::move(**configured);
  return select_startup_model(catalog, generation_options, stop_token);
}

[[nodiscard]] auto session_error(const surfaces::ChatSessionError& value)
    -> cli::CommandFailure {
  switch (value.code) {
    case surfaces::ChatSessionErrorCode::invalid_input:
    case surfaces::ChatSessionErrorCode::input_too_large:
      return {cli::CommandFailureKind::usage, value.message};
    case surfaces::ChatSessionErrorCode::cancelled:
      return {cli::CommandFailureKind::cancelled, value.message};
    default: return {cli::CommandFailureKind::runtime, value.message};
  }
}

[[nodiscard]] auto rebuild_usage_ledger(
    const std::span<const domain::RunEvent> events)
    -> std::expected<domain::UsageLedgerProjection, std::string> {
  domain::UsageLedgerProjection result;
  for (const auto& event : events) {
    auto applied = result.apply(event);
    if (!applied) return std::unexpected(applied.error().message);
  }
  return result;
}

[[nodiscard]] auto rebuild_spend_ceiling(
    const std::span<const domain::RunEvent> events)
    -> std::expected<domain::SessionSpendCeilingProjection, std::string> {
  domain::SessionSpendCeilingProjection result;
  for (const auto& event : events) {
    auto applied = result.apply(event);
    if (!applied) return std::unexpected(applied.error().message);
  }
  return result;
}

[[nodiscard]] auto rebuild_tool_spend_ledger(
    const std::span<const domain::RunEvent> events)
    -> std::expected<domain::ToolSpendLedgerProjection, std::string> {
  domain::ToolSpendLedgerProjection result;
  for (const auto& event : events) {
    auto applied = result.apply(event);
    if (!applied) return std::unexpected(applied.error().message);
  }
  return result;
}

[[nodiscard]] auto reported_cost_text(const domain::ReportedCost& cost)
    -> std::string {
  std::string result;
  for (const auto& amount : cost.amounts()) {
    if (!result.empty()) result += " + ";
    result += amount.amount().to_string();
    result += ' ';
    result += amount.unit();
  }
  return result;
}

struct InferenceCounts {
  std::size_t active{};
  std::size_t completed{};
  std::size_t failed{};
  std::size_t cancelled{};
  std::size_t costs_reported{};
};

[[nodiscard]] auto inference_counts(const domain::UsageLedgerProjection& ledger)
    -> InferenceCounts {
  InferenceCounts result;
  for (const auto& record : ledger.records()) {
    if (record.reported_cost) ++result.costs_reported;
    switch (record.status) {
      case domain::InferenceUsageStatus::active: ++result.active; break;
      case domain::InferenceUsageStatus::completed: ++result.completed; break;
      case domain::InferenceUsageStatus::failed: ++result.failed; break;
      case domain::InferenceUsageStatus::cancelled: ++result.cancelled; break;
    }
  }
  return result;
}

[[nodiscard]] auto plan_state_text(const domain::PlanGraphState state)
    -> std::string_view {
  switch (state) {
    case domain::PlanGraphState::not_started: return "not started";
    case domain::PlanGraphState::proposed: return "proposed";
    case domain::PlanGraphState::revision_requested:
      return "revision requested";
    case domain::PlanGraphState::approved: return "approved";
    case domain::PlanGraphState::rejected: return "rejected";
    case domain::PlanGraphState::invalidated: return "invalidated";
  }
  return "unknown";
}

[[nodiscard]] auto readiness_text(const domain::TaskReadinessState state)
    -> std::string_view {
  switch (state) {
    case domain::TaskReadinessState::ready: return "ready";
    case domain::TaskReadinessState::waiting_for_dependencies:
      return "waiting for dependencies";
    case domain::TaskReadinessState::blocked_by_dependency:
      return "blocked by dependency";
    case domain::TaskReadinessState::blocked_by_resource:
      return "blocked by resource";
    case domain::TaskReadinessState::waiting_for_capacity:
      return "waiting for capacity";
    case domain::TaskReadinessState::running: return "running";
    case domain::TaskReadinessState::completed: return "completed";
    case domain::TaskReadinessState::failed: return "failed";
  }
  return "unknown";
}

[[nodiscard]] auto session_task_state_text(
    const runtime::SessionTaskState state) -> std::string_view {
  switch (state) {
    case runtime::SessionTaskState::pending: return "pending";
    case runtime::SessionTaskState::dispatched: return "dispatched";
    case runtime::SessionTaskState::completed: return "completed";
    case runtime::SessionTaskState::failed: return "failed";
    case runtime::SessionTaskState::cancelled: return "cancelled";
    case runtime::SessionTaskState::timed_out: return "timed out";
    case runtime::SessionTaskState::budget_exhausted: return "budget exhausted";
    case runtime::SessionTaskState::unavailable: return "unavailable";
  }
  return "unknown";
}

[[nodiscard]] auto effect_text(const domain::Effect effect)
    -> std::string_view {
  switch (effect) {
    case domain::Effect::read: return "read";
    case domain::Effect::write: return "write";
    case domain::Effect::remove: return "remove";
    case domain::Effect::execute: return "execute";
    case domain::Effect::network: return "network";
    case domain::Effect::communicate: return "communicate";
    case domain::Effect::spend: return "spend";
    case domain::Effect::change_infrastructure: return "change infrastructure";
    case domain::Effect::change_privileges: return "change privileges";
  }
  return "unknown";
}

[[nodiscard]] auto estimate_summary_text(
    const domain::SessionCostEstimate& estimate) -> std::string {
  if (!estimate.subtotal) return "unavailable";
  auto result = estimate.subtotal->amount().to_string() + " " +
                std::string{estimate.subtotal->unit()};
  if (estimate.estimated_inferences != estimate.total_inferences) {
    result += std::format(" ({} of {})", estimate.estimated_inferences,
                          estimate.total_inferences);
  }
  return result;
}

[[nodiscard]] auto estimate_failures_text(
    const domain::SessionCostEstimate& estimate) -> std::string {
  std::string result;
  for (const auto& failure : estimate.unavailable) {
    if (!result.empty()) result += ", ";
    result += std::string{domain::cost_estimate_reason_name(failure.reason)};
    result += "=" + std::to_string(failure.count);
  }
  if (estimate.aggregation_failure) {
    if (!result.empty()) result += ", ";
    result += std::string{
        domain::cost_estimate_reason_name(*estimate.aggregation_failure)};
  }
  return result;
}

[[nodiscard]] auto usage_header_text(
    const domain::UsageLedgerProjection& ledger,
    const domain::ToolSpendLedgerProjection& tool_spend,
    const domain::SessionSpendCeilingProjection& ceiling) -> std::string {
  const auto& usage = ledger.total_usage();
  auto result = std::format("usage {} in/{} out", usage.input_tokens,
                            usage.output_tokens);
  if (usage.cached_input_tokens != 0) {
    result += std::format("/{} cached", usage.cached_input_tokens);
  }
  if (usage.reasoning_tokens != 0) {
    result += std::format("/{} reasoning", usage.reasoning_tokens);
  }
  if (ledger.total_reported_cost()) {
    const auto counts = inference_counts(ledger);
    result += " | reported ";
    result += reported_cost_text(*ledger.total_reported_cost());
    if (counts.costs_reported != ledger.records().size())
      result += " (partial)";
  }
  const auto usd = domain::summarize_cost_estimates(
      ledger.records(), domain::CostEstimateUnit::usd);
  const auto diem = domain::summarize_cost_estimates(
      ledger.records(), domain::CostEstimateUnit::venice_diem);
  if (usd.subtotal || diem.subtotal) {
    result += " | estimated";
    if (usd.subtotal) result += " " + estimate_summary_text(usd);
    if (diem.subtotal) result += " " + estimate_summary_text(diem);
  }
  if (ceiling.ceiling()) {
    const auto spend = domain::summarize_combined_session_spend(
        ledger.records(), tool_spend, *ceiling.ceiling());
    result += " | spend ";
    if (!spend) {
      result +=
          "unavailable/" + ceiling.ceiling()->amount().to_string() + " USD";
    } else if (spend->accounted) {
      result += spend->accounted->amount().to_string() + "/" +
                spend->ceiling.amount().to_string() + " USD";
      if (spend->reached) result += " reached";
    } else {
      result += "unavailable/" + spend->ceiling.amount().to_string() + " USD";
    }
  }
  return result;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Spend states.
[[nodiscard]] auto usage_panel_lines(
    const domain::UsageLedgerProjection& ledger,
    const domain::ToolSpendLedgerProjection& tool_spend,
    const domain::SessionSpendCeilingProjection& ceiling)
    -> std::vector<std::string> {
  const auto& usage = ledger.total_usage();
  const auto counts = inference_counts(ledger);
  const auto total = ledger.records().size();
  std::vector<std::string> lines{
      std::format("Input tokens: {}", usage.input_tokens),
      std::format("Output tokens: {}", usage.output_tokens),
      std::format("Cached input tokens: {}", usage.cached_input_tokens),
      std::format("Reasoning tokens: {}", usage.reasoning_tokens),
      std::format("Inferences: {} total | {} completed | {} failed | {} "
                  "cancelled | {} active",
                  total, counts.completed, counts.failed, counts.cancelled,
                  counts.active),
  };
  if (ledger.total_reported_cost()) {
    auto cost =
        "Reported cost: " + reported_cost_text(*ledger.total_reported_cost());
    cost += std::format(" ({} of {} inferences reported)",
                        counts.costs_reported, total);
    lines.push_back(std::move(cost));
  } else if (total == 0) {
    lines.push_back("Reported cost: unavailable (no inferences)");
  } else {
    lines.push_back(std::format(
        "Reported cost: unavailable (0 of {} inferences reported)", total));
  }
  for (const auto unit :
       {domain::CostEstimateUnit::usd, domain::CostEstimateUnit::venice_diem}) {
    const auto estimate =
        domain::summarize_cost_estimates(ledger.records(), unit);
    auto line = "Catalog estimate (" +
                std::string{domain::cost_estimate_unit_name(unit)} +
                "): " + estimate_summary_text(estimate);
    line +=
        std::format(" ({} of {} inferences estimated)",
                    estimate.estimated_inferences, estimate.total_inferences);
    const auto failures = estimate_failures_text(estimate);
    if (!failures.empty()) line += "; unavailable: " + failures;
    lines.push_back(std::move(line));
  }
  if (ceiling.ceiling()) {
    const auto spend = domain::summarize_combined_session_spend(
        ledger.records(), tool_spend, *ceiling.ceiling());
    lines.push_back("Spend ceiling (USD): " +
                    ceiling.ceiling()->amount().to_string());
    if (!spend) {
      lines.push_back("Accounted spend (USD): unavailable");
      lines.push_back("Tool spend unavailable: " + spend.error().message);
    } else if (spend->accounted && spend->remaining) {
      lines.push_back("Accounted spend (USD): " +
                      spend->accounted->amount().to_string());
      lines.push_back("Remaining spend (USD): " +
                      spend->remaining->amount().to_string());
      lines.push_back(std::string{"Spend ceiling state: "} +
                      (spend->reached ? "reached" : "open"));
      lines.push_back(std::format("Spend coverage: {} provider-reported + {} "
                                  "catalog-derived of {} inferences",
                                  spend->reported_inferences,
                                  spend->estimated_inferences,
                                  spend->total_inferences));
    } else {
      lines.push_back("Accounted spend (USD): unavailable");
      for (const auto& failure : spend->unavailable) {
        lines.push_back(
            "Spend unavailable: " +
            std::string{domain::cost_estimate_reason_name(failure.reason)} +
            "=" + std::to_string(failure.count));
      }
      if (spend->aggregation_failure) {
        lines.push_back("Spend unavailable: " +
                        std::string{domain::cost_estimate_reason_name(
                            *spend->aggregation_failure)});
      }
    }
    if (spend) {
      const auto amount_text = [](const auto& amount) {
        return amount ? amount->amount().to_string()
                      : std::string{"unavailable"};
      };
      lines.push_back("Inference accounted spend (USD): " +
                      amount_text(spend->inference_accounted));
      lines.push_back("Tool accounted spend (USD): " +
                      amount_text(spend->tool_accounted));
      lines.push_back(std::format("Tool reserved maximum (USD): {} ({} active)",
                                  amount_text(spend->tool_reserved_maximum),
                                  spend->tool_reserved));
      lines.push_back(
          std::format("Tool reconciliation maximum (USD): {} ({} unresolved)",
                      amount_text(spend->tool_reconciliation_maximum),
                      spend->tool_reconciliation_required));
      lines.push_back(
          std::format("Tool released reservations: {}", spend->tool_released));
      lines.push_back(std::format(
          "Tool finalized provider-reported (USD): {} ({} finalized)",
          amount_text(spend->tool_provider_reported_amount),
          spend->tool_provider_reported));
      lines.push_back(std::format(
          "Tool finalized catalog-estimate (USD): {} ({} finalized)",
          amount_text(spend->tool_catalog_estimate_amount),
          spend->tool_catalog_estimate));
      lines.push_back(std::format(
          "Tool finalized policy-upper-bound (USD): {} ({} finalized)",
          amount_text(spend->tool_policy_upper_bound_amount),
          spend->tool_policy_upper_bound));
    }
  } else {
    lines.push_back("Spend ceiling: not set");
  }
  lines.push_back("Reported amounts are provider observations; catalog "
                  "estimates are derived, not quotes.");
  return lines;
}

class ChatAppImpl final : public InteractiveChatApp {
 public:
  ChatAppImpl(backend::Backend& backend,
              backend::ModelContextProvider& model_context,
              storage::SessionStore* session_store,
              surfaces::ChatSessionOpen open, surfaces::DraftEditor& editor,
              const std::stop_token stop_token,
              InteractiveChatAppOptions options)
      : m_backend(backend), m_model_context(model_context),
        m_session_store(session_store), m_model_catalog(options.model_catalog),
        m_provider_character_catalog(options.provider_character_catalog),
        m_configured_request_settings(options.configured_request_settings),
        m_preview_request_setting(std::move(options.preview_request_setting)),
        m_persist_request_setting(std::move(options.persist_request_setting)),
        m_persist_tool_profile_maximum(
            std::move(options.persist_tool_profile_maximum)),
        m_user_global_instruction_path(
            std::move(options.user_global_instruction_path)),
        m_user_global_instructions_enabled(
            options.user_global_instructions_enabled),
        m_preview_user_global_instruction_enabled(
            std::move(options.preview_user_global_instruction_enabled)),
        m_persist_user_global_instruction_enabled(
            std::move(options.persist_user_global_instruction_enabled)),
        m_open_template(std::move(open)),
        m_session_dependencies(std::move(options.session_dependencies)),
        m_bridge(*this, options.live_wake_enabled,
                 std::move(options.wake_observer)),
        m_editor(editor), m_stop_token(stop_token),
        m_rendered_output(options.rendered_output),
        m_rendered_frame(std::move(options.rendered_frame)),
        m_poll_worker_updates(options.poll_worker_updates) {
    set_frame_ms(33);
    m_composer.set_max_height(8);
    m_focus.add(&m_composer);
    auto session = open_chat_session(m_open_template);
    if (!session) {
      m_setup_error = session_error(session.error());
      return;
    }
    m_session = std::move(*session);
    auto rebuilt = m_transcript.rebuild(m_session->event_log().events());
    if (!rebuilt) {
      m_setup_error =
          cli::CommandFailure{cli::CommandFailureKind::runtime,
                              "interactive transcript replay failed"};
      m_session.reset();
      return;
    }
    auto usage = rebuild_usage_ledger(m_session->event_log().events());
    if (!usage) {
      m_setup_error = cli::CommandFailure{cli::CommandFailureKind::runtime,
                                          "interactive usage replay failed: " +
                                              usage.error()};
      m_session.reset();
      return;
    }
    m_usage_ledger = std::move(*usage);
    auto tool_spend =
        rebuild_tool_spend_ledger(m_session->event_log().events());
    if (!tool_spend) {
      m_setup_error = cli::CommandFailure{
          cli::CommandFailureKind::runtime,
          "interactive tool spend replay failed: " + tool_spend.error()};
      m_session.reset();
      return;
    }
    m_tool_spend_ledger = std::move(*tool_spend);
    auto ceiling = rebuild_spend_ceiling(m_session->event_log().events());
    if (!ceiling) {
      m_setup_error = cli::CommandFailure{
          cli::CommandFailureKind::runtime,
          "interactive spend ceiling replay failed: " + ceiling.error()};
      m_session.reset();
      return;
    }
    m_spend_ceiling = std::move(*ceiling);
    sync_history();
    const auto persona = m_session->persona_state();
    m_status = persona.requires_attention ? persona.message
               : persona.selected
                   ? "Ready with persona " + persona.selected->name
                   : "Ready";
    sync_composer_focus();
  }

  [[nodiscard]] auto ready() const noexcept -> bool override {
    return m_session != nullptr && !m_setup_error.has_value();
  }

  [[nodiscard]] auto setup_error() const -> cli::CommandFailure override {
    return m_setup_error.value_or(cli::CommandFailure{
        cli::CommandFailureKind::runtime, "interactive session setup failed"});
  }

  [[nodiscard]] auto pending_edit() const noexcept -> bool override {
    return m_pending_edit;
  }

  auto perform_edit() -> void override {
    m_pending_edit = false;
    if (m_pending_user_global_instruction_edit) {
      auto pending = std::move(*m_pending_user_global_instruction_edit);
      m_pending_user_global_instruction_edit.reset();
      auto edited = m_editor.edit(pending.text, m_stop_token);
      if (!edited) {
        m_status = edited.error().message;
        sync_composer_focus();
        return;
      }
      if (*edited == pending.text) {
        m_status = "User-global instructions unchanged";
        sync_composer_focus();
        return;
      }
      show_user_global_instruction_review(std::move(pending.expected),
                                          std::move(*edited));
      return;
    }
    const auto original = m_composer.text();
    auto edited = m_editor.edit(original, m_stop_token);
    if (!edited) {
      m_status = edited.error().message;
      sync_composer_focus();
      return;
    }
    m_composer.set_text(std::move(*edited));
    m_status = "Draft updated by editor";
    sync_composer_focus();
  }

  [[nodiscard]] auto failure_state() const
      -> std::optional<cli::CommandFailure> override {
    return m_failure;
  }

  [[nodiscard]] auto events() const noexcept
      -> std::span<const domain::RunEvent> override {
    if (!m_session) return {};
    return m_session->event_log().events();
  }

  [[nodiscard]] auto status_text() const noexcept -> std::string_view override {
    return m_status;
  }

  [[nodiscard]] auto configure_terminal_for_scenario(
      const termforge::TerminalIo io,
      const termforge::Capabilities& capabilities)
      -> std::expected<void, std::string> override {
    auto configured_io = terminal().set_io(io);
    if (!configured_io) {
      return std::unexpected(configured_io.error().message);
    }
    auto configured_capabilities = terminal().set_capabilities(capabilities);
    if (!configured_capabilities) {
      return std::unexpected(configured_capabilities.error().message);
    }
    return {};
  }

  auto on_start() -> void override {
    if (m_rendered_output != nullptr) driver().set_output(m_rendered_output);
    sync_composer_focus();
    if (ensure_tool_approval_dialog() && !m_tool_approval_dialog_active &&
        ensure_question_dialog() && !m_question_dialog_active) {
      ensure_plan_review();
    }
  }

  // clang-format off
  // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicit modal input routing preserves deterministic precedence.
  auto on_event(const termforge::Event& event) -> void override {
    // clang-format on
    if (!m_session) return;
    if (m_question_dialog_active || m_tool_approval_dialog_active) {
      if (const auto* key = std::get_if<termforge::KeyEvent>(&event);
          key != nullptr && key->action == termforge::KeyAction::Press &&
          key->ctrl && key->key == termforge::Key::Char && key->ch == U'c') {
        pop_modal();
        m_question_dialog_active = false;
        m_tool_approval_dialog_active = false;
        m_question_controller.reset();
        m_question_dialog.reset();
        m_tool_approval_controller.reset();
        m_tool_approval_dialog.reset();
        auto cancelled = m_session->cancel_active("interrupt");
        if (!cancelled) {
          fail(session_error(cancelled.error()));
          return;
        }
        auto events = m_session->drain();
        if (!events) {
          fail(session_error(events.error()));
          return;
        }
        if (!apply_events(*events)) return;
        m_status = "Run cancelled";
        return;
      }
      termforge::App::on_event(event);
      return;
    }
    auto bridged = m_bridge.handle(event, *m_session);
    if (!bridged) {
      fail(session_error(bridged.error()));
      return;
    }
    if (!apply_events(*bridged)) return;

    if (const auto* mouse = std::get_if<termforge::MouseEvent>(&event)) {
      if (mouse->pressed && !m_session->active()) {
        static_cast<void>(m_focus.focus_at(mouse->x, mouse->y));
      }
      if (m_help_visible) {
        static_cast<void>(m_help.on_event(event));
      } else {
        static_cast<void>(m_transcript.on_event(event));
      }
      if (!m_session->active()) {
        route_mouse(*mouse, {&m_composer});
      }
      return;
    }

    if (const auto* key = std::get_if<termforge::KeyEvent>(&event)) {
      if (key->action != termforge::KeyAction::Press) return;
      if (key->key == termforge::Key::Escape) {
        if (m_session->active()) return;
        if (m_help_visible) {
          m_help_visible = false;
          m_status = "Ready";
        }
        return;
      }
      if (key->ctrl && key->key == termforge::Key::Char && key->ch == U'c') {
        if (m_session->active()) {
          auto cancelled = m_session->cancel_active("interrupt");
          if (!cancelled) fail(session_error(cancelled.error()));
        } else if (m_composer.text().empty()) {
          m_status = "Draft is already empty";
        } else {
          m_composer.clear();
          m_status = "Draft cleared";
        }
        return;
      }
      if (key->ctrl && key->key == termforge::Key::Char && key->ch == U'd') {
        if (m_session->active()) {
          m_status = "Ctrl+D is unavailable while a run is active";
        } else {
          request_close();
        }
        return;
      }
      if (!m_session->active() && key->ctrl &&
          key->key == termforge::Key::Char && key->ch == U'e') {
        request_edit();
        return;
      }
      if (!m_session->active() && key->ctrl &&
          key->key == termforge::Key::Char && key->ch == U'h') {
        m_history_enabled = !m_history_enabled;
        sync_history();
        m_status = m_history_enabled ? "Prompt recall enabled"
                                     : "Prompt recall disabled";
        return;
      }
      if (!m_session->active() && key->ctrl &&
          key->key == termforge::Key::Char && key->ch == U'k') {
        m_history_cutoff = m_session->submitted_prompts().size();
        sync_history();
        m_status = "Prompt recall cleared; durable events retained";
        return;
      }
      if (key->key == termforge::Key::PageUp) {
        active_text_box().scroll(-10);
        return;
      }
      if (key->key == termforge::Key::PageDown) {
        active_text_box().scroll(10);
        return;
      }
      if (!m_session->active() && key->key == termforge::Key::Tab &&
          complete_command()) {
        return;
      }
    }

    if (!m_session->active() && m_focus.handle_key(event)) return;
    if (const auto* key = std::get_if<termforge::KeyEvent>(&event);
        key != nullptr && key->action == termforge::KeyAction::Press &&
        key->key == termforge::Key::Enter && !m_session->active()) {
      submit();
      return;
    }
    termforge::App::on_event(event);
  }

  auto on_tick(std::chrono::duration<double>) -> void override {
    if (!m_session) return;
    if (m_poll_worker_updates) {
      auto drained = m_session->drain();
      if (!drained) {
        fail(session_error(drained.error()));
        return;
      }
      if (!apply_events(*drained)) return;
    }
    if (!ensure_tool_approval_dialog()) return;
    if (!m_tool_approval_dialog_active && !ensure_question_dialog()) return;
    if (!m_tool_approval_dialog_active && !m_question_dialog_active) {
      ensure_plan_review();
    }
    if (m_stop_token.stop_requested()) {
      if (m_session->active()) {
        auto cancelled = m_session->cancel_active("interrupt");
        if (!cancelled) fail(session_error(cancelled.error()));
      } else {
        quit();
      }
    }
  }

  auto on_render(termforge::Screen& screen) -> void override {
    screen.clear();
    const int columns = screen.cols();
    const int rows = screen.rows();
    if (columns <= 0 || rows <= 0 || !m_session) return;

    if (rows <= 2) {
      m_composer.set_geometry({0, 0, columns, 1});
      m_composer.draw(screen);
      if (rows == 2) {
        std::string footer =
            m_session->active()
                ? "Running — Esc/Ctrl+C cancel | Ctrl+D unavailable"
            : m_help_visible
                ? "Slash command help — Esc closes | Ctrl+D exits"
                : "Enter submit | Tab | Ctrl+C clear | Ctrl+D exit | ^E "
                  "editor | /help";
        if (!m_status.empty()) footer += " | " + m_status;
        screen.write_text(0, 1, footer, termforge::theme::kDim,
                          termforge::theme::kBg);
      }
      if (m_rendered_frame) m_rendered_frame(screen);
      return;
    }

    std::string header =
        "AIForge  session " + std::string{m_session->session_id().value()};
    if (!m_session->durable()) header += " (ephemeral)";
    header += " | model " + std::string{m_session->model_id().value()};
    const auto persona = m_session->persona_state();
    if (persona.requires_attention) {
      header += " | persona attention";
    } else if (persona.selected) {
      header += " | persona " + persona.selected->name;
    }
    header += " | " + usage_header_text(m_usage_ledger, m_tool_spend_ledger,
                                        m_spend_ceiling);
    screen.write_text(0, 0,
                      termforge::detail::truncate_to_width(header, columns),
                      termforge::theme::kFg, termforge::Rgb{0x20, 0x20, 0x40});

    const int usable = std::max(0, rows - 2);
    const int composer_rows =
        std::min(usable, m_composer.preferred_height(columns));
    const int transcript_rows = usable - composer_rows;
    m_transcript.set_geometry({0, 1, columns, transcript_rows});
    m_help.set_geometry({0, 1, columns, transcript_rows});
    m_composer.set_geometry({0, 1 + transcript_rows, columns, composer_rows});
    if (m_help_visible) {
      m_help.draw(screen);
    } else {
      m_transcript.draw(screen);
    }
    m_composer.draw(screen);

    std::string footer =
        m_session->active() ? "Running — Esc/Ctrl+C cancel | Ctrl+D unavailable"
        : m_help_visible    ? "Slash command help — Esc closes | Ctrl+D exits"
                         : "Enter submit | Tab | Ctrl+C clear | Ctrl+D exit | "
                           "^E editor | /help";
    if (!m_status.empty()) footer += " | " + m_status;
    screen.write_text(0, rows - 1, footer, termforge::theme::kDim,
                      termforge::theme::kBg);
    if (m_rendered_frame) m_rendered_frame(screen);
  }

 private:
  auto request_edit() -> void {
    m_pending_edit = true;
    sync_composer_focus();
    quit();
  }

  auto sync_composer_focus() -> void {
    m_composer.set_focused(m_session != nullptr && !m_session->active() &&
                           !modal() && !m_pending_edit);
  }

  auto push_modal(termforge::Widget& widget, termforge::OverlayOptions options)
      -> void {
    m_composer.set_focused(false);
    push_overlay(widget, options);
  }

  auto pop_modal() -> void {
    pop_overlay();
    sync_composer_focus();
  }

  [[nodiscard]] auto open_chat_session(surfaces::ChatSessionOpen request)
      -> std::expected<std::unique_ptr<surfaces::ChatSession>,
                       surfaces::ChatSessionError> {
    return surfaces::ChatSession::open(
        std::move(request), m_backend, m_model_context, m_session_store,
        &m_bridge, m_stop_token, {}, m_session_dependencies);
  }

  [[nodiscard]] auto active_text_box() -> termforge::TextBox& {
    return m_help_visible ? m_help : m_transcript.widget();
  }

  auto show_panel(std::string title, std::vector<std::string> lines,
                  std::string status) -> void {
    m_help.clear();
    m_help.append(std::move(title));
    for (auto& line : lines)
      m_help.append(std::move(line));
    m_help_visible = true;
    m_help.scroll(-1000000);
    m_status = std::move(status);
  }

  auto show_help(const std::optional<std::string>& subject) -> bool {
    const auto descriptions = m_slash_commands.describe(
        subject ? std::optional<std::string_view>{*subject} : std::nullopt,
        {.run_active = m_session->active(),
         .editor_available = true,
         .stop_token = m_stop_token});
    if (!descriptions) {
      m_status = descriptions.error().message;
      return false;
    }
    std::vector<std::string> lines;
    lines.reserve(descriptions->size());
    for (const auto& command : *descriptions) {
      std::string line{"/"};
      line += command.name;
      if (!command.arguments.empty()) {
        line += ' ';
        line += command.arguments;
      }
      line += " — ";
      line += command.help;
      if (!command.available) line += " (unavailable)";
      lines.push_back(std::move(line));
    }
    show_panel(subject ? "Slash command" : "Slash commands", std::move(lines),
               "Help is not added to session history");
    return true;
  }

  auto show_sessions() -> bool {
    if (m_session_store == nullptr) {
      show_panel(
          "Sessions",
          {"Current session is ephemeral.",
           "There are no durable sessions to list or resume."},
          "Ephemeral session; /session new starts another ephemeral session");
      return true;
    }

    auto sessions = m_session_store->list_sessions(
        interactive_session_list_limit, m_stop_token);
    if (!sessions) {
      m_status = "Sessions could not be listed: " + sessions.error().message;
      return false;
    }

    std::vector<std::string> lines;
    lines.reserve(sessions->size() + 1);
    if (sessions->empty()) {
      lines.push_back("No durable sessions were found.");
    } else {
      lines.push_back(
          "* marks the current session; most recent activity first.");
      for (const auto& info : *sessions) {
        const bool current = info.session_id == m_session->session_id();
        lines.push_back(std::format(
            "{}{} | created {} | active {} | runs {}", current ? "* " : "  ",
            info.session_id.value(), format_timestamp(info.created_at),
            format_timestamp(info.last_activity_at), info.run_count));
      }
    }
    show_panel("Durable sessions", std::move(lines),
               sessions->size() == interactive_session_list_limit
                   ? "Showing the 100 most recently active sessions"
                   : "Session list is not added to durable history");
    return true;
  }

  auto show_personas() -> bool {
    auto personas = m_session->list_personas();
    if (!personas) {
      m_status = "Personas could not be listed: " + personas.error().message;
      return false;
    }

    const auto state = m_session->persona_state();
    std::vector<std::string> lines;
    lines.reserve(personas->size() + 1);
    if (personas->empty()) {
      lines.push_back("No personas were found in the personas directory.");
    } else {
      lines.push_back("* marks the selected persona.");
      for (const auto& summary : *personas) {
        const bool selected =
            state.selected &&
            state.selected->persona_id == summary.reference.persona_id;
        auto line = std::format("{}{} | {}", selected ? "* " : "  ",
                                summary.reference.name,
                                summary.reference.source_location);
        if (!summary.description.empty()) line += " | " + summary.description;
        lines.push_back(std::move(line));
      }
    }
    show_panel("File-backed personas", std::move(lines),
               "Persona list is not added to durable history");
    return true;
  }

  auto show_persona_editor(PersonaEditorSubmission submission,
                           const bool selected) -> void {
    if (!m_persona_editor_dialog) {
      m_persona_editor_dialog = std::make_unique<PersonaEditorDialog>();
    }
    m_persona_editor_dialog->set_submission(std::move(submission), selected);
    m_persona_editor_dialog->on_save(
        [this](PersonaEditorSubmission request)
            -> std::expected<persona::PersonaWriteReceipt,
                             persona::PersonaEditorError> {
          auto written = std::visit(
              [this](
                  auto&& value) -> std::expected<persona::PersonaWriteReceipt,
                                                 surfaces::ChatSessionError> {
                using Request = std::decay_t<decltype(value)>;
                if constexpr (std::same_as<Request, persona::PersonaCreate>) {
                  return m_session->create_persona(std::move(value.draft));
                } else {
                  return m_session->replace_persona(std::move(value.expected),
                                                    std::move(value.text));
                }
              },
              std::move(request));
          if (!written) {
            return std::unexpected(persona::PersonaEditorError{
                persona::PersonaEditorErrorCode::internal_failure,
                written.error().message, std::nullopt,
                written.error().retryable,
                written.error().effect_may_have_applied});
          }
          return std::move(*written);
        });
    m_persona_editor_dialog->on_result(
        [this](PersonaEditorDialogResult result) {
          pop_modal();
          m_persona_editor_active = false;
          const auto state = m_session->persona_state();
          if (state.requires_attention) {
            m_status = state.message;
            return;
          }
          if (result.effect_may_have_applied) {
            m_status = "Persona write may have applied; reopen to reload";
            return;
          }
          if (!result.receipt) {
            m_status = "Persona unchanged";
            return;
          }
          m_status =
              result.receipt->previous ? "Persona updated" : "Persona created";
        });
    m_persona_editor_active = true;
    push_modal(*m_persona_editor_dialog, {.backdrop = termforge::Backdrop::Dim,
                                          .dismiss_on_click_outside = false});
    m_status =
        selected ? "Edit the selected persona" : "Edit bounded persona content";
  }

  auto show_persona_create_setup(std::string validation_message = {}) -> void {
    termforge::ChoiceWizardPage name;
    name.title = "Create persona";
    name.text = "Enter a bare name. Names start with an ASCII letter or digit "
                "and may also contain '_' or '-'.";
    if (!validation_message.empty()) {
      name.text += " Previous value rejected: " + validation_message;
    }
    name.mode = termforge::ChoiceMode::Single;
    name.minimum_selected = 1;
    name.maximum_selected = 1;
    name.other_enabled = true;
    name.other_label = "Name";
    name.other_placeholder = "reviewer";
    name.other_selected = true;

    termforge::ChoiceWizardPage kind;
    kind.title = "Persona file type";
    kind.text = "Choose the exact file extension under the persona root.";
    kind.mode = termforge::ChoiceMode::Single;
    kind.minimum_selected = 1;
    kind.maximum_selected = 1;
    kind.choices = {{"Markdown (.md)", "Plain UTF-8 Markdown instructions."},
                    {"Text (.txt)", "Plain UTF-8 text instructions."}};
    kind.selected_indices = {0};
    if (!m_persona_manager_dialog->set_pages(
            {std::move(name), std::move(kind)})) {
      m_status = "Persona creation dialog rejected its fields";
      return;
    }
    m_persona_manager_dialog->on_result(
        [this](std::optional<termforge::ChoiceWizardResult> result) {
          pop_modal();
          m_persona_manager_active = false;
          if (!result || result->pages.size() != 2 || !result->pages[0].other ||
              result->pages[0].other->empty() ||
              result->pages[1].selected_indices.size() != 1) {
            m_status = "Persona creation cancelled";
            return;
          }
          const auto kind = result->pages[1].selected_indices.front();
          if (kind > 1U) {
            m_status = "Persona creation dialog returned an invalid file type";
            return;
          }
          persona::PersonaCreate request{
              {std::move(*result->pages[0].other),
               kind == 0U ? persona::PersonaFileKind::markdown
                          : persona::PersonaFileKind::text,
               "validate"},
              m_session->persona_limits()};
          auto valid = persona::prepare_persona_create(request);
          if (!valid) {
            show_persona_create_setup(valid.error().message);
            return;
          }
          request.draft.text.clear();
          show_persona_editor(std::move(request), false);
        });
    m_persona_manager_active = true;
    push_modal(*m_persona_manager_dialog, {.backdrop = termforge::Backdrop::Dim,
                                           .dismiss_on_click_outside = false});
    m_status = validation_message.empty() ? "Define the new persona"
                                          : "Correct the rejected persona name";
  }

  auto show_persona_manager() -> bool {
    if (m_session->active()) {
      m_status = "Finish or cancel the active run before managing personas";
      return false;
    }
    auto personas = m_session->list_personas();
    if (!personas) {
      m_status = "Personas could not be listed: " + personas.error().message;
      return false;
    }
    if (!m_persona_manager_dialog) {
      m_persona_manager_dialog =
          std::make_unique<termforge::ChoiceWizardDialog>();
    }
    termforge::ChoiceWizardPage page;
    page.title = "Manage personas";
    page.text = "Create a bounded persona or edit one exact existing file. "
                "Deletion and arbitrary paths are unavailable.";
    page.mode = termforge::ChoiceMode::Single;
    page.minimum_selected = 1;
    page.maximum_selected = 1;
    page.choices.push_back({"Create new", "Create one .md or .txt persona."});
    for (const auto& entry : *personas) {
      page.choices.push_back(
          {entry.reference.name, entry.reference.source_location});
    }
    page.selected_indices = {0};
    if (!m_persona_manager_dialog->set_pages({std::move(page)})) {
      m_status = "Persona manager rejected its choices";
      return false;
    }
    m_persona_manager_dialog->on_result(
        [this, personas = std::move(*personas)](
            std::optional<termforge::ChoiceWizardResult> result) mutable {
          pop_modal();
          m_persona_manager_active = false;
          if (!result || result->pages.size() != 1 ||
              result->pages.front().selected_indices.size() != 1) {
            m_status = "Persona manager closed without changes";
            return;
          }
          const auto selected = result->pages.front().selected_indices.front();
          if (selected == 0U) {
            show_persona_create_setup();
            return;
          }
          if (selected > personas.size()) {
            m_status = "Persona manager returned an invalid choice";
            return;
          }
          auto document =
              m_session->load_persona(personas[selected - 1].reference.name);
          if (!document) {
            m_status =
                "Persona could not be loaded: " + document.error().message;
            return;
          }
          const auto state = m_session->persona_state();
          const bool is_selected =
              state.selected &&
              state.selected->persona_id == document->reference.persona_id;
          show_persona_editor(
              persona::PersonaReplace{document->reference, document->text,
                                      m_session->persona_limits()},
              is_selected);
        });
    m_persona_manager_active = true;
    push_modal(*m_persona_manager_dialog, {.backdrop = termforge::Backdrop::Dim,
                                           .dismiss_on_click_outside = false});
    m_status = "Choose a persona manager action";
    return true;
  }

  auto show_usage() -> bool {
    show_panel(
        "Session usage, cost, and spend ceiling",
        usage_panel_lines(m_usage_ledger, m_tool_spend_ledger, m_spend_ceiling),
        "Usage summary is derived from session events");
    return true;
  }

  [[nodiscard]] auto repository_snapshot()
      -> std::expected<domain::RepositorySnapshot, std::string> {
    return observe_process_repository(m_stop_token);
  }

  auto show_plan() -> bool {
    auto state = m_session->plan_task_state();
    if (!state) {
      m_status = state.error().message;
      return false;
    }
    if (!state->plan) {
      show_panel("Plan review", {"This session has no plan."},
                 "No plan is available");
      return true;
    }
    std::vector<std::string> lines;
    lines.push_back("State: " +
                    std::string{plan_state_text(state->plan_state)});
    lines.push_back("Goal: " + state->plan->revision.goal);
    lines.push_back("Plan: " +
                    std::string{state->plan->revision.plan_id.value()});
    lines.push_back("Revision: " +
                    std::string{state->plan->revision.revision_id.value()});
    if (state->plan->revision.source_snapshot) {
      const auto& source = *state->plan->revision.source_snapshot;
      lines.push_back("Source: " + std::string{source.repository_id.value()} +
                      " " + source.fingerprint.algorithm + ":" +
                      source.fingerprint.value);
    }
    for (const auto& evidence : state->plan->revision.evidence) {
      lines.push_back("Evidence: " + std::string{evidence.evidence_id.value()} +
                      " " + evidence.digest.algorithm + ":" +
                      evidence.digest.value);
    }
    for (const auto& task : state->plan->revision.tasks) {
      std::string line =
          "[" + std::string{task.task_id.value()} + "] " + task.title;
      if (task.parent_task_id) {
        line += " (parent " + std::string{task.parent_task_id->value()} + ")";
      }
      lines.push_back(std::move(line));
      for (const auto& criterion : task.acceptance_criteria) {
        lines.push_back("  criterion: " + criterion);
      }
      std::string effects{"  effects:"};
      for (const auto effect : task.intended_effects) {
        effects += " " + std::string{effect_text(effect)};
      }
      lines.push_back(std::move(effects));
      if (!task.dependency_task_ids.empty()) {
        std::string dependencies{"  depends:"};
        for (const auto& dependency : task.dependency_task_ids) {
          dependencies += " " + std::string{dependency.value()};
        }
        lines.push_back(std::move(dependencies));
      }
      for (const auto& intent : task.resource_intents) {
        lines.push_back("  resource: " + intent.kind + ":" + intent.value);
      }
    }
    if (state->schedule) {
      lines.push_back(
          "Proposed concurrency: " +
          std::to_string(state->schedule->dispatchable_task_ids.size()));
      for (const auto& task : state->schedule->tasks) {
        std::string schedule = "Schedule " + std::string{task.task_id.value()} +
                               ": " + std::string{readiness_text(task.state)};
        if (!task.blockers.empty()) {
          schedule += " (blocked by";
          for (const auto& blocker : task.blockers) {
            schedule += " " + std::string{blocker.value()};
          }
          schedule += ")";
        }
        lines.push_back(std::move(schedule));
      }
    }
    show_panel("Plan review", std::move(lines),
               state->pending_decision
                   ? "Plan decision is pending in the review dialog"
                   : "Plan state is rebuilt from durable events");
    return true;
  }

  auto show_tasks() -> bool {
    auto snapshot = repository_snapshot();
    auto state = m_session->plan_task_state(
        snapshot ? std::optional{snapshot->root.repository_id} : std::nullopt);
    if (!state) {
      m_status = state.error().message;
      return false;
    }
    std::vector<std::string> lines{"Active session tasks"};
    std::vector<std::string> completed;
    for (const auto& task : state->session_tasks) {
      auto line = "[" + std::string{task.task.task_id.value()} + "] " +
                  task.task.title + " — " +
                  std::string{session_task_state_text(task.state)};
      if (task.state == runtime::SessionTaskState::completed) {
        completed.push_back(std::move(line));
      } else {
        lines.push_back(std::move(line));
      }
    }
    if (lines.size() == 1) lines.push_back("  none");
    lines.push_back("Project backlog");
    if (!snapshot) {
      lines.push_back("  unavailable: " + snapshot.error());
    } else if (state->project_backlog.empty()) {
      lines.push_back("  none");
    } else {
      for (const auto& item : state->project_backlog) {
        lines.push_back("[" + std::string{item.item.item_id.value()} + "] " +
                        item.item.task.title + " — " +
                        (item.status == domain::ProjectBacklogItemStatus::open
                             ? "open"
                             : "resolved"));
      }
    }
    if (!completed.empty()) {
      lines.push_back("Completed session history");
      lines.insert(lines.end(), std::make_move_iterator(completed.begin()),
                   std::make_move_iterator(completed.end()));
    }
    show_panel("Tasks", std::move(lines),
               "Session and project task state is event-derived");
    return true;
  }

  [[nodiscard]] auto memory_target(const std::string_view scope)
      -> std::optional<runtime::MemoryMutationTarget> {
    if (scope == "global") {
      return runtime::MemoryMutationTarget{domain::MemoryScope::global,
                                           std::nullopt};
    }
    if (scope != "project") return std::nullopt;
    auto snapshot = repository_snapshot();
    if (!snapshot) {
      m_status = "Project memory is unavailable: " + snapshot.error();
      return std::nullopt;
    }
    return runtime::MemoryMutationTarget{domain::MemoryScope::project,
                                         snapshot->root.repository_id};
  }

  auto show_memory(std::optional<std::string> search = std::nullopt) -> bool {
    std::vector<runtime::MemoryState> states;
    auto global =
        m_session->memory_state({domain::MemoryScope::global, std::nullopt});
    if (!global) {
      show_panel("Memory — Proposed | Saved | History",
                 {"Durable memory is unavailable: " + global.error().message},
                 "Memory is unavailable for this session");
      return true;
    }
    states.push_back(std::move(*global));
    if (auto snapshot = repository_snapshot()) {
      auto project = m_session->memory_state(
          {domain::MemoryScope::project, snapshot->root.repository_id});
      if (!project) {
        m_status = project.error().message;
        return false;
      }
      states.push_back(std::move(*project));
    }
    const auto needle = search ? lower_copy(*search) : std::string{};
    const auto matches = [&](const std::string_view id,
                             const std::string_view content,
                             const std::string_view rationale) {
      return needle.empty() || lower_copy(id).contains(needle) ||
             lower_copy(content).contains(needle) ||
             lower_copy(rationale).contains(needle);
    };
    std::vector<std::string> proposed{"Proposed"};
    std::vector<std::string> saved{"Saved"};
    std::vector<std::string> history{"History"};
    for (const auto& state : states) {
      for (const auto& view : state.proposals) {
        const auto& value = view.projected;
        if (!matches(value.proposal.proposal_id.value(), value.proposal.content,
                     value.proposal.rationale)) {
          continue;
        }
        auto line = "[" + std::string{value.proposal.proposal_id.value()} +
                    "] " +
                    std::string{memory_scope_text(value.proposal.scope)} + "/" +
                    std::string{memory_kind_text(value.proposal.kind)} + " — " +
                    value.proposal.content + " @ " +
                    format_timestamp(value.proposed_at) +
                    (view.source_available ? "" : " [source unavailable]");
        if (value.state == domain::ProjectedMemoryProposalState::pending) {
          proposed.push_back(std::move(line));
        } else {
          line += value.state == domain::ProjectedMemoryProposalState::accepted
                      ? " [accepted]"
                      : " [rejected]";
          history.push_back(std::move(line));
        }
      }
      for (const auto& view : state.records) {
        const auto& value = view.projected;
        if (!matches(value.record.record_id.value(), value.record.content,
                     value.record.rationale)) {
          continue;
        }
        auto line = "[" + std::string{value.record.record_id.value()} + "] " +
                    std::string{memory_scope_text(value.record.scope)} + "/" +
                    std::string{memory_kind_text(value.record.kind)} + " — " +
                    value.record.content + " @ " +
                    format_timestamp(
                        value.state_changed_at.value_or(value.accepted_at)) +
                    (view.source_available ? "" : " [source unavailable]");
        if (value.state == domain::ProjectedMemoryRecordState::current) {
          saved.push_back(std::move(line));
        } else {
          line += value.state == domain::ProjectedMemoryRecordState::superseded
                      ? " [superseded]"
                      : " [expired]";
          history.push_back(std::move(line));
        }
      }
    }
    if (proposed.size() == 1) proposed.push_back("  none");
    if (saved.size() == 1) saved.push_back("  none");
    if (history.size() == 1) history.push_back("  none");
    std::vector<std::string> lines;
    lines.reserve(proposed.size() + saved.size() + history.size());
    lines.insert(lines.end(), std::make_move_iterator(proposed.begin()),
                 std::make_move_iterator(proposed.end()));
    lines.insert(lines.end(), std::make_move_iterator(saved.begin()),
                 std::make_move_iterator(saved.end()));
    lines.insert(lines.end(), std::make_move_iterator(history.begin()),
                 std::make_move_iterator(history.end()));
    show_panel("Memory — Proposed | Saved | History", std::move(lines),
               search ? "Memory view filtered by '" + *search + "'"
                      : "Memory state is rebuilt from durable events");
    return true;
  }

  auto manage_memory(const std::optional<std::string>& arguments) -> bool {
    if (!arguments || *arguments == "list") return show_memory();
    std::string_view rest{*arguments};
    const auto action = take_word(rest);
    if (action == "search") {
      const auto query = trim_words(rest);
      if (query.empty()) {
        m_status = "Memory search requires text";
        return false;
      }
      return show_memory(std::string{query});
    }
    const auto scope = take_word(rest);
    auto target = memory_target(scope);
    if (!target) {
      if (m_status.empty()) m_status = "Memory scope must be global or project";
      return false;
    }
    auto state = m_session->memory_state(*target);
    if (!state) {
      m_status = state.error().message;
      return false;
    }
    constexpr std::size_t maximum_bulk_mutations = 64;
    if (action == "accept-all" || action == "reject-all") {
      if (!trim_words(rest).empty()) {
        m_status = "Bulk memory action accepts only a scope";
        return false;
      }
      std::size_t changed{};
      for (const auto& view : state->proposals) {
        if (changed >= maximum_bulk_mutations) break;
        const auto& proposal = view.projected;
        if (proposal.state != domain::ProjectedMemoryProposalState::pending)
          continue;
        if (action == "accept-all") {
          if (!proposal.proposal.overlap_record_ids.empty()) continue;
          auto accepted = m_session->accept_memory(
              {*target, proposal.proposal.proposal_id,
               proposal.proposal_event_id, std::nullopt, std::nullopt,
               std::nullopt});
          if (!accepted) {
            m_status = accepted.error().message;
            return false;
          }
        } else {
          auto rejected = m_session->reject_memory(
              {*target, proposal.proposal.proposal_id,
               proposal.proposal_event_id, "rejected by bounded bulk action"});
          if (!rejected) {
            m_status = rejected.error().message;
            return false;
          }
        }
        ++changed;
      }
      if (!show_memory()) return false;
      m_status = std::string{action} + " changed " + std::to_string(changed) +
                 " memories (limit 64)";
      return true;
    }

    const auto identity = take_word(rest);
    if (identity.empty()) {
      m_status = "Memory action requires an identity";
      return false;
    }
    if (action == "accept" || action == "edit") {
      auto proposal_id = domain::MemoryProposalId::from(std::string{identity});
      if (!proposal_id) {
        m_status = "Memory proposal identity is invalid";
        return false;
      }
      const auto found = std::ranges::find(
          state->proposals, *proposal_id, [](const auto& value) {
            return value.projected.proposal.proposal_id;
          });
      if (found == state->proposals.end() ||
          found->projected.state !=
              domain::ProjectedMemoryProposalState::pending) {
        m_status = "Pending memory proposal was not found";
        return false;
      }
      std::optional<std::string> edited;
      if (action == "edit") {
        const auto content = trim_words(rest);
        if (content.empty()) {
          m_status = "Edited acceptance requires replacement content";
          return false;
        }
        edited = std::string{content};
      } else if (!trim_words(rest).empty()) {
        m_status = "Memory accept does not take content; use edit";
        return false;
      }
      std::optional<domain::EventId> expected_record_event_id;
      const auto replacement = found->projected.proposal.replacement_record_id;
      if (replacement) {
        const auto record = std::ranges::find(
            state->records, *replacement,
            [](const auto& value) { return value.projected.record.record_id; });
        if (record == state->records.end() ||
            record->projected.state !=
                domain::ProjectedMemoryRecordState::current) {
          m_status = "Replacement memory is not current";
          return false;
        }
        expected_record_event_id = record->projected.record_event_id;
      } else if (!found->projected.proposal.overlap_record_ids.empty()) {
        m_status = "Contradictory memory requires an explicit replacement";
        return false;
      }
      auto accepted = m_session->accept_memory(
          {*target, *proposal_id, found->projected.proposal_event_id,
           std::move(edited), replacement, expected_record_event_id});
      if (!accepted) {
        m_status = accepted.error().message;
        return false;
      }
    } else if (action == "reject") {
      auto proposal_id = domain::MemoryProposalId::from(std::string{identity});
      if (!proposal_id) {
        m_status = "Memory proposal identity is invalid";
        return false;
      }
      const auto found = std::ranges::find(
          state->proposals, *proposal_id, [](const auto& value) {
            return value.projected.proposal.proposal_id;
          });
      if (found == state->proposals.end() ||
          found->projected.state !=
              domain::ProjectedMemoryProposalState::pending) {
        m_status = "Pending memory proposal was not found";
        return false;
      }
      const auto reason = trim_words(rest);
      auto rejected = m_session->reject_memory(
          {*target, *proposal_id, found->projected.proposal_event_id,
           reason.empty() ? "rejected by user" : std::string{reason}});
      if (!rejected) {
        m_status = rejected.error().message;
        return false;
      }
    } else if (action == "expire") {
      auto record_id = domain::MemoryRecordId::from(std::string{identity});
      if (!record_id) {
        m_status = "Memory record identity is invalid";
        return false;
      }
      const auto found =
          std::ranges::find(state->records, *record_id, [](const auto& value) {
            return value.projected.record.record_id;
          });
      if (found == state->records.end() ||
          found->projected.state !=
              domain::ProjectedMemoryRecordState::current) {
        m_status = "Current memory record was not found";
        return false;
      }
      const auto reason = trim_words(rest);
      auto expired = m_session->expire_memory(
          {*target, *record_id, found->projected.record_event_id,
           reason.empty() ? "expired by user" : std::string{reason}});
      if (!expired) {
        m_status = expired.error().message;
        return false;
      }
    } else {
      m_status = "Memory action must be search, accept, edit, reject, expire, "
                 "accept-all, or reject-all";
      return false;
    }
    if (!show_memory()) return false;
    m_status = "Memory action recorded in durable history";
    return true;
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Session flow.
  auto switch_session(const surfaces::ChatSessionOpen::Mode mode,
                      std::optional<domain::SessionId> session_id) -> bool {
    if (m_session->active()) {
      m_status = "Finish or cancel the active run before switching sessions";
      return false;
    }
    if (mode == surfaces::ChatSessionOpen::Mode::resume) {
      if (!session_id || m_session_store == nullptr) {
        m_status = m_session_store == nullptr
                       ? "Ephemeral sessions cannot resume durable history"
                       : "A session ID is required";
        return false;
      }
      if (session_id == m_session->session_id()) {
        m_help_visible = false;
        m_status = "Session is already current";
        return true;
      }
    }

    auto request = m_open_template;
    request.mode = mode;
    request.session_id = std::move(session_id);
    if (mode == surfaces::ChatSessionOpen::Mode::resume) {
      request.persona = {persona::PersonaDirectiveKind::inherit, std::nullopt,
                         domain::PersonaSelectionSource::resumed};
    } else {
      const auto state = m_session->persona_state();
      request.persona =
          state.selected
              ? persona::PersonaDirective{persona::PersonaDirectiveKind::select,
                                          state.selected->name,
                                          domain::PersonaSelectionSource::
                                              interactive}
              : persona::PersonaDirective{
                    persona::PersonaDirectiveKind::disable, std::nullopt,
                    domain::PersonaSelectionSource::interactive};
    }
    auto candidate = open_chat_session(std::move(request));
    if (!candidate) {
      m_status = candidate.error().message;
      return false;
    }

    TranscriptView candidate_view;
    auto candidate_projection =
        candidate_view.rebuild((*candidate)->event_log().events());
    if (!candidate_projection) {
      m_status = "Interactive transcript replay failed";
      return false;
    }
    auto candidate_usage =
        rebuild_usage_ledger((*candidate)->event_log().events());
    if (!candidate_usage) {
      m_status = "Interactive usage replay failed: " + candidate_usage.error();
      return false;
    }
    auto candidate_ceiling =
        rebuild_spend_ceiling((*candidate)->event_log().events());
    if (!candidate_ceiling) {
      m_status = "Interactive spend ceiling replay failed: " +
                 candidate_ceiling.error();
      return false;
    }
    auto candidate_tool_spend =
        rebuild_tool_spend_ledger((*candidate)->event_log().events());
    if (!candidate_tool_spend) {
      m_status = "Interactive tool spend replay failed: " +
                 candidate_tool_spend.error();
      return false;
    }
    auto rebuilt = m_transcript.rebuild((*candidate)->event_log().events());
    if (!rebuilt) {
      m_status = "Interactive transcript replay failed";
      return false;
    }

    m_session = std::move(*candidate);
    m_request_setting_overrides = {};
    m_usage_ledger = std::move(*candidate_usage);
    m_spend_ceiling = std::move(*candidate_ceiling);
    m_tool_spend_ledger = std::move(*candidate_tool_spend);
    m_help_visible = false;
    m_history_cutoff = 0;
    sync_history();
    m_transcript.widget().scroll_to_bottom();
    const auto persona = m_session->persona_state();
    if (persona.requires_attention) {
      m_status = persona.message;
    } else {
      m_status = mode == surfaces::ChatSessionOpen::Mode::resume
                     ? "Resumed session " +
                           std::string{m_session->session_id().value()}
                     : "Started session " +
                           std::string{m_session->session_id().value()};
    }
    return true;
  }

  auto complete_command() -> bool {
    const auto draft = m_composer.text();
    if (draft.empty() || draft.front() != '/' ||
        m_composer.cursor_pos() != draft.size() ||
        draft.find_first_of(" \t\n") != std::string::npos) {
      return false;
    }
    const auto matches =
        m_slash_commands.complete(draft, {.run_active = m_session->active(),
                                          .editor_available = true,
                                          .stop_token = m_stop_token});
    if (!matches) {
      m_status = matches.error().message;
      return true;
    }
    if (matches->empty()) {
      m_status = "No matching slash command";
      return true;
    }
    if (matches->size() == 1) {
      const auto description = m_slash_commands.describe(
          matches->front(), {.run_active = m_session->active(),
                             .editor_available = true,
                             .stop_token = m_stop_token});
      if (!description) {
        m_status = description.error().message;
        return true;
      }
      auto replacement = "/" + matches->front();
      if (!description->front().arguments.empty()) replacement += ' ';
      m_composer.set_text(std::move(replacement));
      m_status = "Command completed";
      return true;
    }
    const auto shared = common_prefix(*matches);
    if (shared.size() + 1 > draft.size()) m_composer.set_text('/' + shared);
    m_status = completion_status(*matches);
    return true;
  }

  // clang-format off
  // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicit command guards.
  auto execute_command(const surfaces::SlashCommandResult& command) -> bool {
    // clang-format on
    switch (command.action) {
      case surfaces::SlashCommandAction::show_help:
        if (!show_help(command.subject)) return false;
        m_composer.clear();
        return true;
      case surfaces::SlashCommandAction::quit:
        m_composer.clear();
        request_close();
        return true;
      case surfaces::SlashCommandAction::clear_view: {
        auto cleared = m_transcript.clear_view();
        if (!cleared) {
          m_status = cleared.error().message;
          return false;
        }
        m_help_visible = false;
        m_composer.clear();
        m_status = "Transcript view cleared; durable events retained";
        return true;
      }
      case surfaces::SlashCommandAction::edit_draft:
        m_help_visible = false;
        m_composer.clear();
        request_edit();
        return true;
      case surfaces::SlashCommandAction::list_sessions:
        if (!show_sessions()) return false;
        m_composer.clear();
        return true;
      case surfaces::SlashCommandAction::resume_session: {
        if (!command.subject) {
          m_status = "A session ID is required";
          return false;
        }
        auto session_id = domain::SessionId::from(*command.subject);
        if (!session_id) {
          m_status = "Session ID is invalid";
          return false;
        }
        request_close([this, session_id = std::move(*session_id)]() mutable {
          if (switch_session(surfaces::ChatSessionOpen::Mode::resume,
                             std::move(session_id))) {
            m_composer.clear();
            ensure_plan_review();
          }
        });
        return true;
      }
      case surfaces::SlashCommandAction::new_session:
        request_close([this] {
          if (switch_session(m_session_store == nullptr
                                 ? surfaces::ChatSessionOpen::Mode::ephemeral
                                 : surfaces::ChatSessionOpen::Mode::create,
                             std::nullopt)) {
            m_composer.clear();
            ensure_plan_review();
          }
        });
        return true;
      case surfaces::SlashCommandAction::list_personas:
        if (!show_personas()) return false;
        m_composer.clear();
        return true;
      case surfaces::SlashCommandAction::select_persona: {
        if (!command.subject) {
          m_status = "A persona name is required";
          return false;
        }
        auto selected = m_session->select_persona(*command.subject);
        if (!selected) {
          m_status = selected.error().message;
          return false;
        }
        m_help_visible = false;
        m_composer.clear();
        m_status = "Selected persona " + *command.subject;
        return true;
      }
      case surfaces::SlashCommandAction::disable_persona: {
        auto disabled = m_session->disable_persona();
        if (!disabled) {
          m_status = disabled.error().message;
          return false;
        }
        m_help_visible = false;
        m_composer.clear();
        m_status = "Persona disabled";
        return true;
      }
      case surfaces::SlashCommandAction::manage_personas:
        if (!show_persona_manager()) return false;
        m_composer.clear();
        return true;
      case surfaces::SlashCommandAction::choose_provider_character: {
        if (!command.subject) {
          if (!show_provider_characters()) return false;
          m_composer.clear();
          return true;
        }
        auto character = domain::ProviderCharacterId::from(*command.subject);
        if (!character) {
          m_status = "Provider character ID is invalid";
          return false;
        }
        return select_provider_character(*character);
      }
      case surfaces::SlashCommandAction::disable_provider_character:
        return commit_provider_character(std::nullopt);
      case surfaces::SlashCommandAction::choose_model: {
        if (command.subject) {
          auto model_id = domain::ModelId::from(*command.subject);
          if (!model_id) {
            m_status = "Model ID is invalid";
            return false;
          }
          if (!validate_model_change_for_provider_character(*model_id))
            return false;
          auto selected = m_session->select_model(std::move(*model_id));
          if (!selected) {
            m_status = selected.error().message;
            return false;
          }
          m_help_visible = false;
          m_composer.clear();
          m_status = "Selected model " + *command.subject;
          return true;
        }
        if (!show_models()) return false;
        m_composer.clear();
        return true;
      }
      case surfaces::SlashCommandAction::manage_request_settings:
        if (!show_request_settings()) return false;
        m_composer.clear();
        return true;
      case surfaces::SlashCommandAction::manage_user_global_instructions:
        if (!manage_user_global_instructions(command.subject)) return false;
        m_composer.clear();
        return true;
      case surfaces::SlashCommandAction::manage_tool_profile:
        if (!show_tool_profiles()) return false;
        m_composer.clear();
        return true;
      case surfaces::SlashCommandAction::select_tool_profile:
        if (!command.subject) {
          m_status = "A tool profile is required";
          return false;
        }
        return select_tool_profile(*command.subject);
      case surfaces::SlashCommandAction::disable_tools:
        return select_tool_profile("off");
      case surfaces::SlashCommandAction::reset_tool_narrowing:
        return finish_tool_mutation(m_session->reset_tool_narrowing());
      case surfaces::SlashCommandAction::enable_tool_category:
      case surfaces::SlashCommandAction::disable_tool_category:
        if (!command.subject) {
          m_status = "A tool category is required";
          return false;
        }
        return set_tool_category(
            *command.subject,
            command.action ==
                surfaces::SlashCommandAction::enable_tool_category);
      case surfaces::SlashCommandAction::enable_tool:
      case surfaces::SlashCommandAction::disable_tool:
        if (!command.subject) {
          m_status = "A tool name is required";
          return false;
        }
        return finish_tool_mutation(m_session->set_tool_enabled(
            *command.subject,
            command.action == surfaces::SlashCommandAction::enable_tool));
      case surfaces::SlashCommandAction::set_model_tool_profile_maximum:
      case surfaces::SlashCommandAction::set_persona_tool_profile_maximum: {
        if (!command.subject) {
          m_status = "A maximum tool profile is required";
          return false;
        }
        auto maximum = domain::ToolProfileId::from(*command.subject);
        if (!maximum) {
          m_status = "Maximum tool profile identity is invalid";
          return false;
        }
        return persist_tool_profile_maximum(
            command.action ==
                surfaces::SlashCommandAction::set_persona_tool_profile_maximum,
            std::move(*maximum));
      }
      case surfaces::SlashCommandAction::inherit_model_tool_profile_maximum:
        return persist_tool_profile_maximum(false, std::nullopt);
      case surfaces::SlashCommandAction::inherit_persona_tool_profile_maximum:
        return persist_tool_profile_maximum(true, std::nullopt);
      case surfaces::SlashCommandAction::set_reasoning_visibility: {
        if (!command.subject) {
          m_status = "Reasoning visibility is required";
          return false;
        }
        if (*command.subject != "show" && *command.subject != "hide") {
          m_status = "Reasoning visibility is invalid";
          return false;
        }
        const auto visibility = *command.subject == "show"
                                    ? ReasoningVisibility::expanded
                                    : ReasoningVisibility::collapsed;
        auto updated = m_transcript.set_reasoning_visibility(visibility);
        if (!updated) {
          m_status = updated.error().message;
          return false;
        }
        m_help_visible = false;
        m_composer.clear();
        m_status = visibility == ReasoningVisibility::expanded
                       ? "Reasoning text shown"
                       : "Reasoning text hidden";
        return true;
      }
      case surfaces::SlashCommandAction::show_usage:
        show_usage();
        m_composer.clear();
        return true;
      case surfaces::SlashCommandAction::show_plan:
        if (!show_plan()) return false;
        m_composer.clear();
        return true;
      case surfaces::SlashCommandAction::show_tasks:
        if (!show_tasks()) return false;
        m_composer.clear();
        return true;
      case surfaces::SlashCommandAction::manage_memory:
        if (!manage_memory(command.subject)) return false;
        m_composer.clear();
        return true;
    }
    m_status = "Slash command result is unsupported";
    return false;
  }

  auto submit() -> void {
    const std::string draft = m_composer.text();
    if (draft.empty()) {
      m_status = "Draft is empty";
      return;
    }
    const auto command =
        m_slash_commands.dispatch(draft, {.run_active = m_session->active(),
                                          .editor_available = true,
                                          .stop_token = m_stop_token});
    if (!command) {
      m_status = command.error().message;
      return;
    }
    if (command->has_value()) {
      static_cast<void>(execute_command(**command));
      return;
    }
    auto submitted = m_session->submit(draft);
    if (!submitted) {
      m_status = submitted.error().message;
      return;
    }
    if (!apply_events(submitted->committed_events)) return;
    m_help_visible = false;
    m_composer.clear();
    sync_history();
    m_transcript.widget().scroll_to_bottom();
    m_status = "Running";
  }

  auto sync_history() -> void {
    if (!m_session) return;
    m_composer.clear_history();
    if (m_history_enabled) {
      auto prompts = m_session->submitted_prompts();
      const auto begin = std::min(m_history_cutoff, prompts.size());
      for (std::size_t index = begin; index < prompts.size(); ++index) {
        m_composer.push_history(std::move(prompts[index]));
      }
    }
  }

  template <typename Id>
  [[nodiscard]] auto control_id(const std::string_view prefix,
                                const std::uint64_t suffix)
      -> std::optional<Id> {
    auto value = Id::from(std::string{prefix} + '-' + std::to_string(suffix));
    if (!value) return std::nullopt;
    return std::move(*value);
  }

  [[nodiscard]] auto control_attributes() -> std::optional<domain::RunStarted> {
    auto surface = domain::SurfaceId::from("interactive");
    auto workspace = domain::WorkspaceId::from("chat");
    auto permission = domain::PermissionProfileId::from("plan-control");
    if (!surface || !workspace || !permission) return std::nullopt;
    return domain::RunStarted{*surface, *workspace, *permission, std::nullopt};
  }

  auto ensure_plan_review() -> void {
    if (!m_session || m_plan_review_active || m_close_dialog_active) return;
    auto state = m_session->plan_task_state();
    if (!state || !state->pending_decision || !state->plan) return;
    const auto pending = *state->pending_decision;
    const auto revision = state->plan->revision;
    const auto review_key =
        std::pair{m_session->session_id(), revision.revision_id};
    if (m_reviewed_plan == review_key) return;
    if (!m_plan_dialog) {
      m_plan_dialog = std::make_unique<termforge::ChoiceWizardDialog>();
    }
    std::string text = "Plan " + std::string{revision.plan_id.value()} +
                       "\nGoal: " + revision.goal;
    if (revision.source_snapshot) {
      text += "\nSource: " +
              std::string{revision.source_snapshot->repository_id.value()} +
              " " + revision.source_snapshot->fingerprint.algorithm + ":" +
              revision.source_snapshot->fingerprint.value;
    }
    for (const auto& evidence : revision.evidence) {
      text += "\nEvidence: " + std::string{evidence.evidence_id.value()} + " " +
              evidence.digest.algorithm + ":" + evidence.digest.value;
    }
    for (const auto& task : revision.tasks) {
      text += "\n\n[" + std::string{task.task_id.value()} + "] " + task.title;
      if (!task.dependency_task_ids.empty()) {
        text += "\nDepends on:";
        for (const auto& dependency : task.dependency_task_ids) {
          text += " " + std::string{dependency.value()};
        }
      }
      for (const auto& criterion : task.acceptance_criteria) {
        text += "\nCriterion: " + criterion;
      }
      text += "\nEffects:";
      for (const auto effect : task.intended_effects) {
        text += " " + std::string{effect_text(effect)};
      }
      for (const auto& intent : task.resource_intents) {
        text += "\nResource: " + intent.kind + ":" + intent.value;
      }
    }
    if (state->schedule) {
      text += "\n\nProposed concurrency: " +
              std::to_string(state->schedule->dispatchable_task_ids.size());
      for (const auto& task : state->schedule->tasks) {
        text += "\nSchedule " + std::string{task.task_id.value()} + ": " +
                std::string{readiness_text(task.state)};
        if (!task.blockers.empty()) {
          text += " (blocked by";
          for (const auto& blocker : task.blockers) {
            text += " " + std::string{blocker.value()};
          }
          text += ")";
        }
      }
    }
    termforge::ChoiceWizardPage page;
    page.title = "Review plan " + std::string{revision.revision_id.value()};
    page.text = std::move(text);
    page.mode = termforge::ChoiceMode::Single;
    page.minimum_selected = 1;
    page.maximum_selected = 1;
    page.choices = {
        {"Approve", "Materialize the exact displayed revision."},
        {"Revise", "Request a superseding revision; enter a reason below."},
        {"Reject", "Reject the exact displayed revision."}};
    page.other_enabled = true;
    page.other_label = "Reason";
    page.other_placeholder = "Required for Revise; optional for Reject";
    if (!m_plan_dialog->set_pages({std::move(page)})) {
      m_status = "Plan review dialog rejected the plan";
      return;
    }
    m_reviewed_plan = review_key;
    m_plan_review_active = true;
    m_plan_dialog->on_result([this, pending, revision](
                                 std::optional<termforge::ChoiceWizardResult>
                                     result) {
      pop_modal();
      m_plan_review_active = false;
      if (!result || result->pages.size() != 1 ||
          result->pages.front().selected_indices.size() != 1) {
        m_status = "Plan review cancelled; the revision remains pending";
        return;
      }
      const auto selected = result->pages.front().selected_indices.front();
      if (selected > 2) {
        m_status = "Plan review returned an invalid action";
        return;
      }
      auto reason = result->pages.front().other;
      if (selected == 1 && (!reason || reason->empty())) {
        m_status = "A revision request requires a reason";
        m_reviewed_plan.reset();
        ensure_plan_review();
        return;
      }
      runtime::PlanApprovalEnvironment environment;
      if (selected == 0) {
        if (!revision.evidence.empty()) {
          m_status =
              "Plan approval blocked: bound evidence cannot be re-established";
          return;
        }
        if (revision.source_snapshot) {
          auto snapshot = repository_snapshot();
          if (!snapshot) {
            m_status = "Plan approval blocked: " + snapshot.error();
            return;
          }
          environment.source_snapshot = domain::snapshot_identity(*snapshot);
        }
      }
      const auto decision = selected == 0 ? domain::PlanDecision::approved
                            : selected == 1
                                ? domain::PlanDecision::revision_requested
                                : domain::PlanDecision::rejected;
      auto decided = m_session->decide_plan(
          pending.run_id,
          {pending.plan_id, pending.revision_id, decision,
           domain::PlanDecisionSource::user, std::move(reason)},
          std::move(environment));
      if (!decided) {
        m_status = decided.error().message;
        return;
      }
      m_status = decision == domain::PlanDecision::approved
                     ? "Plan approved and session tasks materialized"
                 : decision == domain::PlanDecision::revision_requested
                     ? "Plan revision requested"
                     : "Plan rejected";
    });
    push_modal(*m_plan_dialog, {.backdrop = termforge::Backdrop::Dim,
                                .dismiss_on_click_outside = false});
    m_status = "Review the exact proposed plan";
  }

  auto request_close(std::function<void()> action = {}) -> void {
    if (!action) action = [this] { quit(); };
    if (m_close_dialog_active || m_plan_review_active ||
        m_settings_dialog_active || m_user_global_instruction_dialog_active ||
        m_user_global_instruction_review_active || m_persona_manager_active ||
        m_persona_editor_active) {
      m_status = "Close is unavailable while another decision is open";
      return;
    }
    auto state = m_session->plan_task_state();
    if (!state) {
      m_status = state.error().message;
      return;
    }
    std::vector<runtime::ActiveSessionTask> unresolved;
    for (const auto& task : state->session_tasks) {
      if (task.state == runtime::SessionTaskState::completed) continue;
      unresolved.push_back(task);
    }
    if (unresolved.empty()) {
      action();
      return;
    }

    auto snapshot = repository_snapshot();
    if (snapshot) {
      state = m_session->plan_task_state(snapshot->root.repository_id);
      if (!state) {
        m_status = state.error().message;
        return;
      }
      unresolved.erase(
          std::remove_if(
              unresolved.begin(), unresolved.end(),
              [&](const auto& task) {
                const auto promoted = std::ranges::any_of(
                    state->project_backlog, [&](const auto& item) {
                      return item.item.origin.session_id ==
                                 m_session->session_id() &&
                             item.item.origin.plan_id == task.plan_id &&
                             item.item.origin.revision_id == task.revision_id &&
                             item.item.origin.task_id == task.task.task_id;
                    });
                return promoted;
              }),
          unresolved.end());
    }
    if (unresolved.empty()) {
      action();
      return;
    }
    if (!m_close_dialog) {
      m_close_dialog = std::make_unique<termforge::ChoiceWizardDialog>();
    }
    termforge::ChoiceWizardPage tasks;
    tasks.title = "Unresolved session tasks";
    tasks.text = "Select tasks to promote before leaving this session.";
    tasks.mode = termforge::ChoiceMode::Multiple;
    tasks.minimum_selected = 0;
    tasks.maximum_selected = unresolved.size();
    for (const auto& task : unresolved) {
      tasks.choices.push_back(
          {task.task.title, std::string{session_task_state_text(task.state)}});
    }
    termforge::ChoiceWizardPage decision;
    decision.title = "Session cleanup";
    decision.text = snapshot ? "Promotion is explicit; unselected tasks remain "
                               "only in this session."
                             : "The repository is unavailable, so tasks can "
                               "only remain session-local.";
    decision.mode = termforge::ChoiceMode::Single;
    decision.minimum_selected = 1;
    decision.maximum_selected = 1;
    if (snapshot) {
      decision.choices.push_back(
          {"Promote selected", "Append project-backlog promotion facts."});
    }
    decision.choices.push_back(
        {"Leave session-local",
         "Keep unresolved work only in session history."});
    decision.choices.push_back({"Cancel", "Return to the current session."});
    if (!m_close_dialog->set_pages({std::move(tasks), std::move(decision)})) {
      m_status = "Session cleanup dialog rejected its task list";
      return;
    }
    m_close_action = std::move(action);
    m_close_dialog_active = true;
    const auto repository_id =
        snapshot ? std::optional{snapshot->root.repository_id} : std::nullopt;
    m_close_dialog->on_result(
        [this, unresolved = std::move(unresolved), repository_id](
            std::optional<termforge::ChoiceWizardResult> result) mutable {
          pop_modal();
          m_close_dialog_active = false;
          if (!result || result->pages.size() != 2 ||
              result->pages[1].selected_indices.size() != 1) {
            m_status = "Session close cancelled";
            m_close_action = {};
            return;
          }
          const auto decision = result->pages[1].selected_indices.front();
          const auto cancel_index = repository_id ? 2U : 1U;
          const auto leave_index = repository_id ? 1U : 0U;
          if (decision == cancel_index) {
            m_status = "Session close cancelled";
            m_close_action = {};
            return;
          }
          if (repository_id && decision == 0U) {
            for (const auto index : result->pages[0].selected_indices) {
              if (index >= unresolved.size()) {
                m_status = "Session cleanup returned an invalid task";
                m_close_action = {};
                return;
              }
              const auto suffix = static_cast<std::uint64_t>(
                  m_session->event_log().last_sequence() + 1U);
              auto run_id = control_id<domain::RunId>("task-promotion", suffix);
              auto item_id = control_id<domain::ProjectBacklogItemId>(
                  "backlog-item", suffix);
              auto attributes = control_attributes();
              if (!run_id || !item_id || !attributes) {
                m_status = "Project task identity generation failed";
                m_close_action = {};
                return;
              }
              const auto& task = unresolved[index];
              auto promoted = m_session->promote_project_task(
                  {*run_id,
                   *attributes,
                   {*item_id,
                    *repository_id,
                    {m_session->session_id(), task.plan_id, task.revision_id,
                     task.task.task_id},
                    task.task,
                    domain::ProjectBacklogDecisionSource::user}});
              if (!promoted) {
                m_status = promoted.error().message;
                m_close_action = {};
                return;
              }
            }
          } else if (decision != leave_index) {
            m_status = "Session cleanup returned an invalid action";
            m_close_action = {};
            return;
          }
          auto next = std::move(m_close_action);
          m_close_action = {};
          if (next) next();
        });
    push_modal(*m_close_dialog, {.backdrop = termforge::Backdrop::Dim,
                                 .dismiss_on_click_outside = false});
    m_status = "Resolve session cleanup before leaving";
  }

  auto validate_model_change_for_provider_character(
      const domain::ModelId& target) -> bool {
    if (!m_request_setting_overrides.character_slug) return true;
    const auto& selected = *m_request_setting_overrides.character_slug;
    const auto reject = [this, &selected](std::string reason) {
      m_status = std::move(reason) + "; disable provider character " +
                 std::string{selected.value()} +
                 " with /character off before changing models";
      return false;
    };
    if (m_provider_character_catalog == nullptr)
      return reject("Provider character catalog is unavailable");
    auto current =
        m_provider_character_catalog->lookup(selected, {}, m_stop_token);
    if (!current)
      return reject("Selected provider character could not be revalidated: " +
                    current.error().message);
    if (auto valid = backend::validate_provider_character_summary(*current);
        !valid)
      return reject("Selected provider character is invalid: " +
                    valid.error().message);
    if (current->id != selected)
      return reject("Provider character lookup returned a different character");
    if (!current->model_id)
      return reject("Selected provider character has no model metadata");
    if (*current->model_id != target)
      return reject("Selected provider character requires model " +
                    std::string{current->model_id->value()});
    return true;
  }

  auto show_models() -> bool {
    if (m_model_catalog == nullptr) {
      m_status = "Model catalog is unavailable";
      return false;
    }
    auto snapshot = m_model_catalog->snapshot(m_stop_token);
    if (!snapshot) {
      m_status = snapshot.error().message;
      return false;
    }
    if (!m_model_picker) {
      m_model_picker = std::make_unique<ModelPickerDialog>();
      m_model_picker->on_close([this] { pop_modal(); });
      m_model_picker->on_result(
          [this](std::optional<domain::ModelId> selected) {
            if (!selected) {
              m_status = "Model selection cancelled";
              return;
            }
            if (!validate_model_change_for_provider_character(*selected))
              return;
            auto changed = m_session->select_model(std::move(*selected));
            if (!changed) {
              m_status = changed.error().message;
              return;
            }
            m_status =
                "Selected model " + std::string{m_session->model_id().value()};
          });
    }
    m_model_picker->set_models(snapshot->get(), m_session->model_id());
    push_modal(*m_model_picker, {.backdrop = termforge::Backdrop::Dim,
                                 .dismiss_on_click_outside = false});
    if (!snapshot->get().warnings.empty())
      m_status = snapshot->get().warnings.back();
    else
      m_status = "Choose a model";
    return true;
  }

  auto commit_provider_character(
      std::optional<domain::ProviderCharacterId> character) -> bool {
    auto overrides = m_request_setting_overrides;
    overrides.character_slug = std::move(character);
    auto options =
        venice_generation_options(m_configured_request_settings, overrides);
    if (!options) {
      m_status = options.error();
      return false;
    }
    auto effective = venice_effective_request_options(
        m_configured_request_settings, overrides);
    if (!effective) {
      m_status = effective.error();
      return false;
    }
    auto prepared = m_session->prepare_generation_options(
        std::move(*options), std::move(*effective));
    if (!prepared) {
      m_status = prepared.error().message;
      return false;
    }
    auto committed = m_session->commit_generation_options(std::move(*prepared));
    if (!committed) {
      m_status = committed.error().message;
      return false;
    }
    m_request_setting_overrides = std::move(overrides);
    m_help_visible = false;
    m_composer.clear();
    m_status =
        m_request_setting_overrides.character_slug
            ? "Selected provider character " +
                  std::string{
                      m_request_setting_overrides.character_slug->value()} +
                  "; applies next inference"
            : "Provider character disabled; applies next inference";
    return true;
  }

  auto select_provider_character(
      const domain::ProviderCharacterId& requested,
      std::optional<domain::ModelId> expected_model = std::nullopt) -> bool {
    if (m_provider_character_catalog == nullptr) {
      m_status = "Provider character catalog is unavailable";
      return false;
    }
    auto character =
        m_provider_character_catalog->lookup(requested, {}, m_stop_token);
    if (!character) {
      m_status = "Provider character could not be selected: " +
                 character.error().message;
      return false;
    }
    if (auto valid = backend::validate_provider_character_summary(*character);
        !valid) {
      m_status =
          "Provider character could not be selected: " + valid.error().message;
      return false;
    }
    if (character->id != requested) {
      m_status = "Provider character lookup returned a different character";
      return false;
    }
    if (expected_model && character->model_id != expected_model) {
      m_status = "Provider character model changed; reopen /character";
      return false;
    }
    if (!character->model_id) {
      m_status = "Provider character has no model metadata";
      return false;
    }
    if (*character->model_id != m_session->model_id()) {
      m_status = "Provider character " + std::string{requested.value()} +
                 " requires model " +
                 std::string{character->model_id->value()} +
                 "; select that model first";
      return false;
    }
    if (m_model_catalog == nullptr) {
      m_status = "Model catalog is unavailable";
      return false;
    }
    auto snapshot = m_model_catalog->snapshot(m_stop_token);
    if (!snapshot) {
      m_status = snapshot.error().message;
      return false;
    }
    const auto* model =
        model::find_model(snapshot->get(), *character->model_id, "text");
    if (model == nullptr || model->offline || !model->context_window_tokens) {
      m_status = "Provider character's required model is unavailable";
      return false;
    }
    return commit_provider_character(character->id);
  }

  auto show_provider_characters() -> bool {
    if (m_provider_character_catalog == nullptr) {
      m_status = "Provider character catalog is unavailable";
      return false;
    }
    if (m_model_catalog == nullptr) {
      m_status = "Model catalog is unavailable";
      return false;
    }
    auto characters = m_provider_character_catalog->list({}, m_stop_token);
    if (!characters) {
      m_status = "Provider characters could not be listed: " +
                 characters.error().message;
      return false;
    }
    if (auto valid = backend::validate_provider_character_catalog(*characters);
        !valid) {
      m_status =
          "Provider characters could not be listed: " + valid.error().message;
      return false;
    }
    auto models = m_model_catalog->snapshot(m_stop_token);
    if (!models) {
      m_status = models.error().message;
      return false;
    }
    if (!m_provider_character_picker) {
      m_provider_character_picker =
          std::make_unique<ProviderCharacterPickerDialog>();
      m_provider_character_picker->on_close([this] { pop_modal(); });
      m_provider_character_picker->on_result(
          [this](std::optional<ProviderCharacterPickerResult> result) {
            if (!result) {
              m_status = "Provider character selection cancelled";
              return;
            }
            if (!result->selection) {
              static_cast<void>(commit_provider_character(std::nullopt));
              return;
            }
            static_cast<void>(select_provider_character(
                result->selection->id, result->selection->model_id));
          });
    }
    m_provider_character_picker->set_characters(
        *characters, models->get(), m_session->model_id(),
        m_request_setting_overrides.character_slug);
    push_modal(*m_provider_character_picker,
               {.backdrop = termforge::Backdrop::Dim,
                .dismiss_on_click_outside = false});
    m_status = "Choose a provider character; no model will be switched";
    return true;
  }

  [[nodiscard]] auto request_setting_summary(const bool web_search) const
      -> std::string {
    std::string result;
    if (web_search) {
      const auto configured = m_configured_request_settings.web_search;
      const auto overridden = m_request_setting_overrides.web_search;
      const auto effective = overridden.value_or(configured);
      result =
          "Configured: " + std::string{web_search_setting_name(configured)} +
          " (" +
          std::string{configured_source_name(
              m_configured_request_settings.web_search_source)} +
          ")\nSession override: " +
          (overridden ? std::string{web_search_setting_name(*overridden)}
                      : std::string{"none"}) +
          "\nEffective winner: " +
          std::string{web_search_setting_name(effective)} +
          (overridden
               ? " (session override)"
               : " (" +
                     std::string{configured_source_name(
                         m_configured_request_settings.web_search_source)} +
                     ")");
      const auto found = m_session->model_info().capabilities.find(
          std::string{web_search_model_capability});
      result += "\nSelected-model support: ";
      if (found == m_session->model_info().capabilities.end() ||
          !found->second) {
        result += "unknown";
      } else {
        result += found->second.value_or(false) ? "true" : "false";
      }
    } else {
      const auto configured = m_configured_request_settings.system_prompt;
      const auto overridden = m_request_setting_overrides.system_prompt;
      const auto effective = overridden.value_or(configured);
      result =
          "Configured: " + std::string{system_prompt_setting_name(configured)} +
          " (" +
          std::string{configured_source_name(
              m_configured_request_settings.system_prompt_source)} +
          ")\nSession override: " +
          (overridden ? std::string{system_prompt_setting_name(*overridden)}
                      : std::string{"none"}) +
          "\nEffective winner: " +
          std::string{system_prompt_setting_name(effective)} +
          (overridden
               ? " (session override)"
               : " (" +
                     std::string{configured_source_name(
                         m_configured_request_settings.system_prompt_source)} +
                     ")") +
          "\nSelected-model support: Venice chat operation";
    }
    result += "\nTakes effect: next inference";
    return result;
  }

  // clang-format off
  // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicit commit stages.
  auto apply_request_setting(const bool web_search, const std::size_t value,
                             const bool persist) -> void {
    // clang-format on
    auto overrides = m_request_setting_overrides;
    VeniceRequestSettingSave save;
    if (web_search) {
      const auto selected = value == 0   ? VeniceWebSearchSetting::inherit
                            : value == 1 ? VeniceWebSearchSetting::automatic
                            : value == 2 ? VeniceWebSearchSetting::on
                                         : VeniceWebSearchSetting::off;
      if (persist) {
        save.web_search = selected;
      } else if (selected == VeniceWebSearchSetting::inherit) {
        overrides.web_search.reset();
      } else {
        overrides.web_search = selected;
      }
    } else {
      const auto selected = value == 0   ? VeniceSystemPromptSetting::inherit
                            : value == 1 ? VeniceSystemPromptSetting::include
                                         : VeniceSystemPromptSetting::exclude;
      if (persist) {
        save.system_prompt = selected;
      } else if (selected == VeniceSystemPromptSetting::inherit) {
        overrides.system_prompt.reset();
      } else {
        overrides.system_prompt = selected;
      }
    }

    auto candidate_configured = m_configured_request_settings;
    std::optional<std::vector<domain::ConfigurationProvenanceEntry>>
        candidate_configuration;
    if (persist) {
      if (!m_preview_request_setting || !m_persist_request_setting) {
        m_status = "Persisted request settings are unavailable";
        return;
      }
      auto previewed = m_preview_request_setting(save);
      if (!previewed) {
        m_status = previewed.error();
        return;
      }
      if (web_search) {
        candidate_configured.web_search = previewed->configured.web_search;
        candidate_configured.web_search_source =
            previewed->configured.web_search_source;
        overrides.web_search.reset();
      } else {
        candidate_configured.system_prompt =
            previewed->configured.system_prompt;
        candidate_configured.system_prompt_source =
            previewed->configured.system_prompt_source;
        overrides.system_prompt.reset();
      }
      if (m_open_template.provenance) {
        auto& configuration = candidate_configuration.emplace(
            m_open_template.provenance->configuration);
        auto existing = std::ranges::find(
            configuration, previewed->configuration_provenance.key,
            &domain::ConfigurationProvenanceEntry::key);
        if (existing == configuration.end()) {
          configuration.push_back(
              std::move(previewed->configuration_provenance));
        } else {
          *existing = std::move(previewed->configuration_provenance);
        }
      }
    }

    auto options = venice_generation_options(candidate_configured, overrides);
    if (!options) {
      m_status = options.error();
      return;
    }
    auto effective =
        venice_effective_request_options(candidate_configured, overrides);
    if (!effective) {
      m_status = effective.error();
      return;
    }
    auto prepared = m_session->prepare_generation_options(
        std::move(*options), std::move(*effective), candidate_configuration);
    if (!prepared) {
      m_status = prepared.error().message;
      return;
    }
    if (persist) {
      auto saved = m_persist_request_setting(save);
      if (!saved) {
        m_status = saved.error();
        return;
      }
    }
    auto committed = m_session->commit_generation_options(std::move(*prepared));
    if (!committed) {
      m_status = persist ? "Request default was saved, but the live session "
                           "changed before it could be applied"
                         : committed.error().message;
      return;
    }
    m_configured_request_settings = candidate_configured;
    m_request_setting_overrides = overrides;
    if (persist) {
      auto template_options =
          venice_generation_options(m_configured_request_settings);
      auto template_effective =
          venice_effective_request_options(m_configured_request_settings);
      if (template_options && template_effective) {
        m_open_template.generation_options = std::move(*template_options);
        if (m_open_template.provenance) {
          m_open_template.provenance->effective_request_options =
              std::move(*template_effective);
          m_open_template.provenance->configuration =
              candidate_configuration.value_or(
                  m_open_template.provenance->configuration);
        }
      }
    }
    const auto winning_source =
        web_search ? m_configured_request_settings.web_search_source
                   : m_configured_request_settings.system_prompt_source;
    const bool shadowed =
        persist && winning_source &&
        (*winning_source == config::ConfigSource::command_line ||
         *winning_source == config::ConfigSource::environment);
    const auto active_source =
        winning_source.value_or(config::ConfigSource::file);
    m_status =
        !persist ? "Session request override updated; applies next inference"
        : shadowed
            ? "Saved request default; active " +
                  std::string{config::config_source_name(active_source)} +
                  " value still wins next inference"
            : "Saved request default; applies next inference";
  }

  // clang-format off
  // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicit provider choices.
  auto show_request_setting_values(const bool web_search) -> void {
    // clang-format on
    termforge::ChoiceWizardPage value;
    value.title = web_search ? "Web search" : "Venice system prompt";
    value.text = request_setting_summary(web_search);
    value.mode = termforge::ChoiceMode::Single;
    value.minimum_selected = 1;
    value.maximum_selected = 1;
    if (web_search) {
      value.choices = {{"Inherit", "Use the configured or provider default."},
                       {"Auto", "Let Venice decide when to search."},
                       {"On", "Enable web search for each request."},
                       {"Off", "Explicitly disable web search."}};
      const auto current = m_request_setting_overrides.web_search;
      value.selected_indices = {!current ? 0U
                                : *current == VeniceWebSearchSetting::automatic
                                    ? 1U
                                : *current == VeniceWebSearchSetting::on ? 2U
                                                                         : 3U};
    } else {
      value.choices = {
          {"Inherit", "Use the configured or provider default."},
          {"Include", "Include the Venice system prompt."},
          {"Exclude", "Explicitly exclude the Venice system prompt."}};
      const auto current = m_request_setting_overrides.system_prompt;
      value.selected_indices = {!current ? 0U
                                : *current == VeniceSystemPromptSetting::include
                                    ? 1U
                                    : 2U};
    }
    termforge::ChoiceWizardPage target;
    target.title = "Apply request setting";
    target.text = "Session overrides are transient. Saved defaults update one "
                  "registered configuration key atomically.";
    target.mode = termforge::ChoiceMode::Single;
    target.minimum_selected = 1;
    target.maximum_selected = 1;
    target.choices = {
        {"This session", "Apply only until this live session is replaced."}};
    if (m_preview_request_setting && m_persist_request_setting) {
      target.choices.push_back(
          {"Save default", "Persist this setting in the user config file."});
    }
    target.selected_indices = {0};
    if (!m_settings_dialog->set_pages({std::move(value), std::move(target)})) {
      m_status = "Request settings dialog rejected its choices";
      return;
    }
    m_settings_dialog->on_result(
        [this,
         web_search](std::optional<termforge::ChoiceWizardResult> result) {
          pop_modal();
          m_settings_dialog_active = false;
          if (!result || result->pages.size() != 2 ||
              result->pages[0].selected_indices.size() != 1 ||
              result->pages[1].selected_indices.size() != 1) {
            m_status = "Request settings unchanged";
            return;
          }
          const auto value = result->pages[0].selected_indices.front();
          const auto target = result->pages[1].selected_indices.front();
          if (value >= (web_search ? 4U : 3U) || target >= 2U ||
              (target == 1U &&
               (!m_preview_request_setting || !m_persist_request_setting))) {
            m_status = "Request settings dialog returned an invalid choice";
            return;
          }
          apply_request_setting(web_search, value, target == 1U);
        });
    m_settings_dialog_active = true;
    push_modal(*m_settings_dialog, {.backdrop = termforge::Backdrop::Dim,
                                    .dismiss_on_click_outside = false});
  }

  auto show_request_settings() -> bool {
    if (m_session->active()) {
      m_status = "Finish or cancel the active run before changing settings";
      return false;
    }
    if (!m_settings_dialog) {
      m_settings_dialog = std::make_unique<termforge::ChoiceWizardDialog>();
    }
    termforge::ChoiceWizardPage page;
    page.title = "Request settings";
    page.text = "Choose one Chat request setting. Safe mode is not a Chat "
                "setting and is available only to media operations.";
    page.mode = termforge::ChoiceMode::Single;
    page.minimum_selected = 1;
    page.maximum_selected = 1;
    page.choices = {{"Web search", request_setting_summary(true)},
                    {"Venice system prompt", request_setting_summary(false)}};
    page.selected_indices = {0};
    if (!m_settings_dialog->set_pages({std::move(page)})) {
      m_status = "Request settings dialog rejected its choices";
      return false;
    }
    m_settings_dialog->on_result(
        [this](std::optional<termforge::ChoiceWizardResult> result) {
          pop_modal();
          m_settings_dialog_active = false;
          if (!result || result->pages.size() != 1 ||
              result->pages.front().selected_indices.size() != 1) {
            m_status = "Request settings unchanged";
            return;
          }
          const auto selected = result->pages.front().selected_indices.front();
          if (selected > 1U) {
            m_status = "Request settings dialog returned an invalid choice";
            return;
          }
          show_request_setting_values(selected == 0U);
        });
    m_settings_dialog_active = true;
    push_modal(*m_settings_dialog, {.backdrop = termforge::Backdrop::Dim,
                                    .dismiss_on_click_outside = false});
    m_status = "Inspect request settings";
    return true;
  }

  [[nodiscard]] auto user_global_instruction_summary(
      const std::optional<domain::UserGlobalInstructionDocument>& document)
      const -> std::string {
    const auto maximum_bytes =
        m_session->user_global_instruction_limits().maximum_file_bytes;
    const auto bytes = document ? document->text.size() : 0U;
    const auto estimated_tokens = bytes;
    std::string summary =
        std::string{"Enabled: "} +
        (m_user_global_instructions_enabled ? "on" : "off") +
        " (future runs; an active run remains pinned)\n" +
        "Present: " + (document ? std::string{"yes"} : std::string{"no"}) +
        "\nAuthority: user-global instruction layer\nPath: " +
        (m_user_global_instruction_path.empty()
             ? std::string{domain::user_global_instruction_source_location}
             : m_user_global_instruction_path) +
        "\nBytes: " + std::to_string(bytes) + " / " +
        std::to_string(maximum_bytes) +
        "\nConservative context estimate: " + std::to_string(estimated_tokens) +
        " tokens (one per byte)";
    if (document) {
      summary += "\nDigest: " + document->reference.content_digest.algorithm +
                 ":" + document->reference.content_digest.value;
    } else {
      summary += "\nDigest: none";
    }
    return summary;
  }

  auto show_user_global_instruction_document(
      const std::optional<domain::UserGlobalInstructionDocument>& document)
      -> void {
    std::vector<std::string> lines;
    lines.push_back(user_global_instruction_summary(document));
    lines.emplace_back();
    if (!document) {
      lines.push_back("The fixed user-global instruction document does not "
                      "exist. Use /instructions and choose Edit to create it.");
    } else {
      lines.push_back("Document content:");
      std::string_view remaining{document->text};
      while (true) {
        const auto newline = remaining.find('\n');
        lines.emplace_back(remaining.substr(0, newline));
        if (newline == std::string_view::npos) break;
        remaining.remove_prefix(newline + 1U);
      }
    }
    show_panel("User-global instructions", std::move(lines),
               "Viewing user-global instructions; Esc returns to chat");
  }

  auto request_user_global_instruction_edit(
      std::optional<domain::UserGlobalInstructionReference> expected,
      std::string text) -> void {
    m_pending_user_global_instruction_edit = {
        std::move(expected), text.empty()
                                 ? std::string{"# User-global instructions\n\n"}
                                 : std::move(text)};
    m_pending_edit = true;
    sync_composer_focus();
    quit();
  }

  auto save_user_global_instruction(
      std::optional<domain::UserGlobalInstructionReference> expected,
      std::string text) -> void {
    instructions::UserGlobalInstructionWrite request{
        std::move(expected), std::move(text),
        m_session->user_global_instruction_limits()};
    auto written = m_session->write_user_global_instruction(request);
    if (!written) {
      if (written.error().effect_may_have_applied) {
        fail({cli::CommandFailureKind::runtime,
              "User-global instruction persistence may have applied; "
              "restart and reload before retrying: " +
                  written.error().message});
        return;
      }
      m_status = written.error().message;
      return;
    }
    auto valid = instructions::validate_user_global_instruction_write_receipt(
        request, *written);
    if (!valid) {
      fail({cli::CommandFailureKind::runtime,
            "User-global instruction persistence returned an uncertain "
            "receipt; restart and reload before retrying"});
      return;
    }
    m_status = "Saved user-global instructions; future runs will use the new "
               "digest";
  }

  auto show_user_global_instruction_review(
      std::optional<domain::UserGlobalInstructionReference> expected,
      std::string text) -> void {
    instructions::UserGlobalInstructionWrite request{
        expected, text, m_session->user_global_instruction_limits()};
    auto prepared =
        instructions::prepare_user_global_instruction_write(request);
    if (!prepared) {
      m_status = prepared.error().message;
      sync_composer_focus();
      return;
    }
    if (!m_user_global_instruction_review_dialog) {
      m_user_global_instruction_review_dialog =
          std::make_unique<termforge::ChoiceWizardDialog>();
    }
    termforge::ChoiceWizardPage page;
    page.title = "Review user-global instructions";
    page.text = user_global_instruction_summary(*prepared) +
                "\n\nReview the exact proposed document before saving. A "
                "stale digest will be rejected.\n\n" +
                prepared->text;
    page.mode = termforge::ChoiceMode::Single;
    page.minimum_selected = 1;
    page.maximum_selected = 1;
    page.choices = {
        {"Save", "Compare the observed digest and atomically replace the "
                 "fixed document."},
        {"Back", "Return the proposed content to the external editor."}};
    page.selected_indices = {1};
    if (!m_user_global_instruction_review_dialog->set_pages(
            {std::move(page)})) {
      m_status = "User-global instruction review rejected its content";
      sync_composer_focus();
      return;
    }
    m_user_global_instruction_review_dialog->on_result(
        [this, expected = std::move(expected), text = std::move(text)](
            std::optional<termforge::ChoiceWizardResult> result) mutable {
          pop_modal();
          m_user_global_instruction_review_active = false;
          if (!result) {
            m_status = "User-global instructions unchanged";
            return;
          }
          if (result->pages.size() != 1 ||
              result->pages.front().selected_indices.size() != 1) {
            m_status = "User-global instruction review returned an invalid "
                       "choice";
            return;
          }
          switch (result->pages.front().selected_indices.front()) {
            case 0:
              save_user_global_instruction(std::move(expected),
                                           std::move(text));
              return;
            case 1:
              request_user_global_instruction_edit(std::move(expected),
                                                   std::move(text));
              return;
            default:
              m_status = "User-global instruction review returned an invalid "
                         "choice";
              return;
          }
        });
    m_user_global_instruction_review_active = true;
    push_modal(*m_user_global_instruction_review_dialog,
               {.backdrop = termforge::Backdrop::Dim,
                .dismiss_on_click_outside = false});
    m_status = "Review user-global instructions before saving";
  }

  // clang-format off
  // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicit preview, provenance validation, persistence, and activation stages keep the fail-closed transaction auditable.
  auto set_user_global_instructions_enabled(const bool enabled) -> bool {
    // clang-format on
    if (!m_preview_user_global_instruction_enabled ||
        !m_persist_user_global_instruction_enabled) {
      m_status = "Persisted user-global instruction settings are unavailable";
      return false;
    }
    auto previewed = m_preview_user_global_instruction_enabled(enabled);
    if (!previewed) {
      m_status = previewed.error();
      return false;
    }
    std::optional<std::vector<domain::ConfigurationProvenanceEntry>>
        candidate_configuration;
    if (m_open_template.provenance) {
      auto& configuration = candidate_configuration.emplace(
          m_open_template.provenance->configuration);
      auto existing = std::ranges::find(
          configuration, previewed->configuration_provenance.key,
          &domain::ConfigurationProvenanceEntry::key);
      if (existing == configuration.end()) {
        configuration.push_back(previewed->configuration_provenance);
      } else {
        *existing = previewed->configuration_provenance;
      }
      auto candidate_provenance = *m_open_template.provenance;
      candidate_provenance.configuration = configuration;
      if (!domain::validate_run_provenance(candidate_provenance)) {
        m_status =
            "User-global instruction configuration provenance is invalid";
        return false;
      }
    }
    auto persisted = m_persist_user_global_instruction_enabled(enabled);
    if (!persisted) {
      if (persisted.error().effect_may_have_applied) {
        fail({cli::CommandFailureKind::runtime,
              "User-global instruction enable persistence may have applied; "
              "restart and reload before retrying: " +
                  persisted.error().message});
      } else {
        m_status = persisted.error().message;
      }
      return false;
    }
    auto activated = m_session->set_user_global_instructions_enabled(
        previewed->effective_enabled, candidate_configuration);
    if (!activated) {
      m_status =
          "User-global instruction setting was saved, but the live session "
          "changed before it could be activated";
      return false;
    }
    m_user_global_instructions_enabled = previewed->effective_enabled;
    m_session_dependencies.user_global_instructions_enabled =
        previewed->effective_enabled;
    if (m_open_template.provenance && candidate_configuration) {
      m_open_template.provenance->configuration =
          std::move(*candidate_configuration);
    }
    const auto source = previewed->configuration_provenance.source;
    const bool shadowed =
        source && (*source == domain::ProvenanceSource::command_line ||
                   *source == domain::ProvenanceSource::environment);
    const auto source_name = [&]() -> std::string_view {
      if (!source) return "unknown";
      switch (*source) {
        case domain::ProvenanceSource::command_line: return "command-line";
        case domain::ProvenanceSource::environment: return "environment";
        case domain::ProvenanceSource::file: return "file";
        case domain::ProvenanceSource::compiled_default:
          return "compiled default";
      }
      return "unknown";
    }();
    if (shadowed) {
      m_status = "Saved user-global instruction default; active " +
                 std::string{source_name} + " value still wins for future runs";
    } else {
      m_status = previewed->effective_enabled
                     ? "Enabled user-global instructions for future runs"
                     : "Disabled user-global instructions for future runs; "
                       "the document was retained";
    }
    return true;
  }

  auto show_user_global_instruction_manager() -> bool {
    auto document = m_session->load_user_global_instruction();
    if (!document) {
      m_status = document.error().message;
      return false;
    }
    if (!m_user_global_instruction_dialog) {
      m_user_global_instruction_dialog =
          std::make_unique<termforge::ChoiceWizardDialog>();
    }
    termforge::ChoiceWizardPage page;
    page.title = "User-global instructions";
    page.text = user_global_instruction_summary(*document);
    page.mode = termforge::ChoiceMode::Single;
    page.minimum_selected = 1;
    page.maximum_selected = 1;
    page.choices = {
        {"View", "View the fixed document and its authority metadata."},
        {"Edit", *document ? "Edit with a digest precondition."
                           : "Create the fixed instruction document."},
        {m_user_global_instructions_enabled ? "Disable" : "Enable",
         m_user_global_instructions_enabled
             ? "Disable future-run use without deleting the document."
             : "Enable the document for future runs."}};
    page.selected_indices = {0};
    if (!m_user_global_instruction_dialog->set_pages({std::move(page)})) {
      m_status = "User-global instruction manager rejected its content";
      return false;
    }
    m_user_global_instruction_dialog->on_result(
        [this, document = std::move(*document)](
            std::optional<termforge::ChoiceWizardResult> result) mutable {
          pop_modal();
          m_user_global_instruction_dialog_active = false;
          if (!result) {
            m_status = "User-global instructions unchanged";
            return;
          }
          if (result->pages.size() != 1 ||
              result->pages.front().selected_indices.size() != 1) {
            m_status = "User-global instruction manager returned an invalid "
                       "choice";
            return;
          }
          switch (result->pages.front().selected_indices.front()) {
            case 0: show_user_global_instruction_document(document); return;
            case 1:
              request_user_global_instruction_edit(
                  document ? std::optional{document->reference} : std::nullopt,
                  document ? std::move(document->text) : std::string{});
              return;
            case 2:
              static_cast<void>(set_user_global_instructions_enabled(
                  !m_user_global_instructions_enabled));
              return;
            default:
              m_status = "User-global instruction manager returned an invalid "
                         "choice";
              return;
          }
        });
    m_user_global_instruction_dialog_active = true;
    push_modal(*m_user_global_instruction_dialog,
               {.backdrop = termforge::Backdrop::Dim,
                .dismiss_on_click_outside = false});
    m_status = "Inspect user-global instructions";
    return true;
  }

  auto manage_user_global_instructions(
      const std::optional<std::string>& operation) -> bool {
    if (m_session->active()) {
      m_status = "Finish or cancel the active run before changing instructions";
      return false;
    }
    if (!operation) return show_user_global_instruction_manager();
    if (*operation == "on") return set_user_global_instructions_enabled(true);
    if (*operation == "off") return set_user_global_instructions_enabled(false);
    m_status = "User-global instruction operation is invalid";
    return false;
  }

  // clang-format off
  // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Exhaustive ceiling and availability rendering keeps the manager auditable.
  [[nodiscard]] auto tool_profile_summary(
      const runtime::ToolProfileResolution& state) const -> std::string {
    // clang-format on
    std::string summary = "Selected " + state.selected_profile.name + ".";
    if (state.selection.desired_tool_names) {
      summary += " Session desired:";
      if (state.selection.desired_tool_names->empty()) summary += " none";
      for (const auto& name : *state.selection.desired_tool_names) {
        summary += " " + name;
      }
      summary += ".";
    } else {
      summary += " Session desired: all selected-profile tools.";
    }
    summary += " Model maximum: ";
    summary +=
        state.selection.model_maximum_profile_id
            ? std::string{state.selection.model_maximum_profile_id->value()}
            : "inherit";
    summary += ". Persona maximum: ";
    summary +=
        state.selection.persona_maximum_profile_id
            ? std::string{state.selection.persona_maximum_profile_id->value()}
            : "inherit";
    summary += ".";
    summary += " Model tool calling: ";
    summary += !state.selection.model_tool_calling_support   ? "unknown"
               : *state.selection.model_tool_calling_support ? "supported"
                                                             : "unsupported";
    summary += ".";
    if (state.effective_tools.empty()) {
      summary += " Effective tools: none.";
    } else {
      summary += " Effective tools:";
      for (const auto& declaration : state.effective_tools.declarations()) {
        summary += " " + declaration.name;
      }
      summary += ".";
    }
    const auto* policy = m_session_dependencies.tool_policy
                             ? m_session_dependencies.tool_policy->provenance()
                             : nullptr;
    if (policy != nullptr) {
      summary += " Launch authority effects:";
      if (policy->effect_ceiling.empty()) summary += " none";
      for (const auto effect : policy->effect_ceiling) {
        summary += " " + std::string{effect_text(effect)};
      }
      summary += ". Capability ceilings:";
      if (policy->capability_ceiling.empty()) summary += " none";
      for (const auto& scope : policy->capability_ceiling) {
        summary += " " + std::string{effect_text(scope.effect)} + ":" +
                   scope.kind + "=" + scope.value;
      }
      summary += ". Approval: ";
      switch (policy->approval_mode) {
        case domain::ToolApprovalMode::prompt: summary += "prompt"; break;
        case domain::ToolApprovalMode::automatic: summary += "auto"; break;
        case domain::ToolApprovalMode::allow_all: summary += "allow-all"; break;
      }
      summary += ". Restriction: ";
      switch (policy->restriction_level) {
        case domain::ToolRestrictionLevel::high: summary += "high"; break;
        case domain::ToolRestrictionLevel::medium: summary += "medium"; break;
        case domain::ToolRestrictionLevel::low: summary += "low"; break;
        case domain::ToolRestrictionLevel::none: summary += "none"; break;
      }
      summary += ".";
    } else {
      summary += " Launch policy provenance unavailable; authority-bearing "
                 "declarations are unavailable.";
    }
    for (const auto& availability : state.tool_availability) {
      if (availability.reason ==
          runtime::ToolProfileAvailabilityReason::available) {
        continue;
      }
      summary += " " + availability.tool_name + ": ";
      summary +=
          runtime::tool_profile_availability_reason_text(availability.reason);
      summary += ".";
    }
    return summary;
  }

  auto select_tool_profile(const std::string_view profile) -> bool {
    auto profile_id = domain::ToolProfileId::from(std::string{profile});
    if (!profile_id) {
      m_status = "Tool profile identity is invalid";
      return false;
    }
    auto selected = m_session->select_tool_profile(std::move(*profile_id));
    if (!selected) {
      m_status = selected.error().message;
      return false;
    }
    auto state = m_session->tool_profile_state();
    if (!state) {
      m_status = state.error().message;
      return false;
    }
    m_help_visible = false;
    m_composer.clear();
    m_status = tool_profile_summary(*state);
    return true;
  }

  auto finish_tool_mutation(
      std::expected<void, surfaces::ChatSessionError> changed) -> bool {
    if (!changed) {
      m_status = changed.error().message;
      return false;
    }
    auto state = m_session->tool_profile_state();
    if (!state) {
      m_status = state.error().message;
      return false;
    }
    m_help_visible = false;
    m_composer.clear();
    m_status = tool_profile_summary(*state);
    return true;
  }

  auto set_tool_category(const std::string_view category_name,
                         const bool enabled) -> bool {
    const auto category = runtime::tool_category_from_name(category_name);
    if (!category) {
      m_status = "Tool category is invalid";
      return false;
    }
    return finish_tool_mutation(
        m_session->set_tool_category_enabled(*category, enabled));
  }

  auto persist_tool_profile_maximum(
      const bool persona, std::optional<domain::ToolProfileId> profile_id)
      -> bool {
    if (!m_persist_tool_profile_maximum) {
      m_status = "Tool maximum persistence is unavailable";
      return false;
    }
    ToolProfileMaximumSubject subject{m_session->model_id()};
    if (persona) {
      const auto persona_state = m_session->persona_state();
      if (!persona_state.selected) {
        m_status = "Select a persona before configuring its tool maximum";
        return false;
      }
      subject = persona_state.selected->persona_id;
    }
    ToolProfileMaximumSave save{std::move(subject), profile_id};
    auto prepared =
        persona ? m_session->prepare_persona_tool_profile_maximum(profile_id)
                : m_session->prepare_model_tool_profile_maximum(profile_id);
    if (!prepared) {
      m_status = prepared.error().message;
      return false;
    }
    auto persisted = m_persist_tool_profile_maximum(save);
    if (!persisted) {
      m_composer.clear();
      if (persisted.error().effect_may_have_applied) {
        fail({cli::CommandFailureKind::runtime,
              "Tool maximum persistence had an indeterminate result: " +
                  persisted.error().message});
      } else {
        m_status = "Tool maximum unchanged: " + persisted.error().message;
      }
      return false;
    }
    const auto changed =
        m_session->commit_tool_profile_maximum(std::move(*prepared));
    if (!changed) {
      fail({cli::CommandFailureKind::runtime,
            "Tool maximum was persisted but could not be activated: " +
                changed.error().message});
      return false;
    }
    if (const auto* model = std::get_if<domain::ModelId>(&save.subject)) {
      if (profile_id) {
        m_session_dependencies.model_tool_profile_maximums.insert_or_assign(
            *model, *profile_id);
      } else {
        m_session_dependencies.model_tool_profile_maximums.erase(*model);
      }
    } else if (const auto* selected_persona =
                   std::get_if<domain::PersonaId>(&save.subject)) {
      if (profile_id) {
        m_session_dependencies.persona_tool_profile_maximums.insert_or_assign(
            *selected_persona, *profile_id);
      } else {
        m_session_dependencies.persona_tool_profile_maximums.erase(
            *selected_persona);
      }
    }
    return finish_tool_mutation({});
  }

  auto show_tool_profile_picker() -> bool {
    if (m_session->active()) {
      m_status = "Finish or cancel the active run before selecting tools";
      return false;
    }
    auto state = m_session->tool_profile_state();
    if (!state) {
      m_status = state.error().message;
      return false;
    }
    if (!m_tool_profile_dialog) {
      m_tool_profile_dialog = std::make_unique<termforge::ChoiceWizardDialog>();
    }
    termforge::ChoiceWizardPage page;
    page.title = "Chat tools";
    page.text = tool_profile_summary(*state) +
                " Selection changes declarations only; it grants no authority.";
    page.mode = termforge::ChoiceMode::Single;
    page.minimum_selected = 1;
    page.maximum_selected = 1;
    std::vector<domain::ToolProfileId> profile_ids;
    for (const auto& profile : runtime::builtin_tool_profiles()) {
      profile_ids.push_back(profile.profile_id);
      std::string description = profile.tool_names.empty()
                                    ? "Advertise no model-callable tools."
                                    : "Advertised tools (authority is governed "
                                      "by launch policy):";
      for (const auto& tool : profile.tool_names)
        description += " " + tool;
      page.choices.push_back({profile.name, std::move(description)});
      if (profile.profile_id == state->selected_profile.profile_id) {
        page.selected_indices = {profile_ids.size() - 1};
      }
    }
    if (page.selected_indices.empty() ||
        !m_tool_profile_dialog->set_pages({std::move(page)})) {
      m_status = "Tool profile dialog rejected its choices";
      return false;
    }
    m_tool_profile_dialog->on_result(
        [this, profile_ids = std::move(profile_ids)](
            std::optional<termforge::ChoiceWizardResult> result) mutable {
          pop_modal();
          m_tool_profile_dialog_active = false;
          if (!result || result->pages.size() != 1 ||
              result->pages.front().selected_indices.size() != 1) {
            static_cast<void>(show_tool_profiles());
            return;
          }
          const auto selected = result->pages.front().selected_indices.front();
          if (selected >= profile_ids.size()) {
            m_status = "Tool profile dialog returned an invalid choice";
            return;
          }
          static_cast<void>(select_tool_profile(profile_ids[selected].value()));
        });
    m_tool_profile_dialog_active = true;
    push_modal(*m_tool_profile_dialog, {.backdrop = termforge::Backdrop::Dim,
                                        .dismiss_on_click_outside = false});
    m_status = "Choose a Chat tool profile";
    return true;
  }

  auto show_tool_category_picker() -> bool {
    auto state = m_session->tool_profile_state();
    if (!state) {
      m_status = state.error().message;
      return false;
    }
    constexpr std::array categories{
        runtime::ToolCategory::interaction, runtime::ToolCategory::memory,
        runtime::ToolCategory::repository,  runtime::ToolCategory::process,
        runtime::ToolCategory::media,       runtime::ToolCategory::other};
    termforge::ChoiceWizardPage page;
    page.title = "Tool categories";
    page.text = tool_profile_summary(*state) +
                " Categories only narrow exact selected-profile members.";
    page.mode = termforge::ChoiceMode::Multiple;
    page.minimum_selected = 0;
    page.maximum_selected = categories.size();
    const auto desired = state->selection.desired_tool_names.value_or(
        state->selected_profile.tool_names);
    for (std::size_t index = 0; index < categories.size(); ++index) {
      const auto members = runtime::tool_profile_category_members(
          m_session_dependencies.tools, state->selected_profile.profile_id,
          categories[index]);
      const bool enabled =
          members && !members->empty() &&
          std::ranges::all_of(*members, [&](const auto& name) {
            return std::ranges::find(desired, name) != desired.end();
          });
      page.choices.push_back(
          {std::string{runtime::tool_category_name(categories[index])},
           members && !members->empty()
               ? "Toggle the category's exact registered profile members."
               : "No registered member in the selected profile."});
      if (enabled) page.selected_indices.push_back(index);
    }
    if (!m_tool_profile_dialog->set_pages({std::move(page)})) {
      m_status = "Tool category dialog rejected its choices";
      return false;
    }
    m_tool_profile_dialog->on_result(
        [this,
         categories](std::optional<termforge::ChoiceWizardResult> result) {
          pop_modal();
          m_tool_profile_dialog_active = false;
          if (!result || result->pages.size() != 1) {
            static_cast<void>(show_tool_profiles());
            return;
          }
          const auto& selected = result->pages.front().selected_indices;
          for (std::size_t index = 0; index < categories.size(); ++index) {
            const bool enabled =
                std::ranges::find(selected, index) != selected.end();
            auto changed = m_session->set_tool_category_enabled(
                categories[index], enabled);
            if (!changed) {
              m_status = changed.error().message;
              return;
            }
          }
          static_cast<void>(finish_tool_mutation({}));
        });
    m_tool_profile_dialog_active = true;
    push_modal(*m_tool_profile_dialog, {.backdrop = termforge::Backdrop::Dim,
                                        .dismiss_on_click_outside = false});
    m_status = "Choose enabled tool categories";
    return true;
  }

  // clang-format off
  // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Exact effects and scopes are rendered beside every selectable declaration.
  auto show_individual_tool_picker() -> bool {
    // clang-format on
    auto state = m_session->tool_profile_state();
    if (!state) {
      m_status = state.error().message;
      return false;
    }
    if (state->selected_profile.tool_names.empty()) {
      m_status = "The selected profile contains no individual tools";
      return false;
    }
    termforge::ChoiceWizardPage page;
    page.title = "Individual tools";
    page.text = tool_profile_summary(*state) +
                " A checked tool remains subject to every other ceiling.";
    page.mode = termforge::ChoiceMode::Multiple;
    page.minimum_selected = 0;
    page.maximum_selected = state->selected_profile.tool_names.size();
    const auto desired = state->selection.desired_tool_names.value_or(
        state->selected_profile.tool_names);
    for (std::size_t index = 0;
         index < state->selected_profile.tool_names.size(); ++index) {
      const auto& name = state->selected_profile.tool_names[index];
      const auto* registered = m_session_dependencies.tools.find(name);
      std::string description =
          registered != nullptr
              ? "Category: " + std::string{runtime::tool_category_name(
                                   registered->category)}
              : "Not registered in this runtime";
      if (registered != nullptr && !registered->declaration.effects.empty()) {
        description += "; effects:";
        for (const auto effect : registered->declaration.effects)
          description += " " + std::string{effect_text(effect)};
      }
      if (registered != nullptr &&
          !registered->declaration.capability_scopes.empty()) {
        description += "; scopes:";
        for (const auto& scope : registered->declaration.capability_scopes) {
          description += " " + std::string{effect_text(scope.effect)} + ":" +
                         scope.kind + "=" + scope.value;
        }
      }
      page.choices.push_back({name, std::move(description)});
      if (std::ranges::find(desired, name) != desired.end())
        page.selected_indices.push_back(index);
    }
    if (!m_tool_profile_dialog->set_pages({std::move(page)})) {
      m_status = "Tool selection dialog rejected its choices";
      return false;
    }
    const auto tool_names = state->selected_profile.tool_names;
    m_tool_profile_dialog->on_result(
        [this,
         tool_names](std::optional<termforge::ChoiceWizardResult> result) {
          pop_modal();
          m_tool_profile_dialog_active = false;
          if (!result || result->pages.size() != 1) {
            static_cast<void>(show_tool_profiles());
            return;
          }
          const auto& selected = result->pages.front().selected_indices;
          for (std::size_t index = 0; index < tool_names.size(); ++index) {
            const bool enabled =
                std::ranges::find(selected, index) != selected.end();
            auto changed =
                m_session->set_tool_enabled(tool_names[index], enabled);
            if (!changed) {
              m_status = changed.error().message;
              return;
            }
          }
          static_cast<void>(finish_tool_mutation({}));
        });
    m_tool_profile_dialog_active = true;
    push_modal(*m_tool_profile_dialog, {.backdrop = termforge::Backdrop::Dim,
                                        .dismiss_on_click_outside = false});
    m_status = "Choose individual tools";
    return true;
  }

  auto show_tool_maximum_picker(const bool persona) -> bool {
    if (persona && !m_session->persona_state().selected) {
      m_status = "Select a persona before configuring its tool maximum";
      return false;
    }
    auto state = m_session->tool_profile_state();
    if (!state) {
      m_status = state.error().message;
      return false;
    }
    termforge::ChoiceWizardPage page;
    page.title = persona ? "Persona tool maximum" : "Model tool maximum";
    page.text = "Choose an exact built-in maximum profile. Inherit clears the "
                "association; Off is an explicit deny-all maximum.";
    page.mode = termforge::ChoiceMode::Single;
    page.minimum_selected = 1;
    page.maximum_selected = 1;
    page.choices.push_back({"Inherit", "No associated maximum profile."});
    const auto current = persona ? state->selection.persona_maximum_profile_id
                                 : state->selection.model_maximum_profile_id;
    if (!current) page.selected_indices = {0};
    std::vector<domain::ToolProfileId> profiles;
    for (const auto& profile : runtime::builtin_tool_profiles()) {
      profiles.push_back(profile.profile_id);
      page.choices.push_back(
          {profile.name,
           "Maximum profile " + std::string{profile.profile_id.value()}});
      if (current == profile.profile_id) {
        page.selected_indices = {profiles.size()};
      }
    }
    if (!m_tool_profile_dialog->set_pages({std::move(page)})) {
      m_status = "Tool maximum dialog rejected its choices";
      return false;
    }
    m_tool_profile_dialog->on_result(
        [this, persona, profiles = std::move(profiles)](
            std::optional<termforge::ChoiceWizardResult> result) mutable {
          pop_modal();
          m_tool_profile_dialog_active = false;
          if (!result || result->pages.size() != 1 ||
              result->pages.front().selected_indices.size() != 1) {
            static_cast<void>(show_tool_profiles());
            return;
          }
          const auto selected = result->pages.front().selected_indices.front();
          if (selected > profiles.size()) {
            m_status = "Tool maximum dialog returned an invalid choice";
            return;
          }
          static_cast<void>(persist_tool_profile_maximum(
              persona, selected == 0 ? std::nullopt
                                     : std::optional<domain::ToolProfileId>{
                                           profiles[selected - 1]}));
        });
    m_tool_profile_dialog_active = true;
    push_modal(*m_tool_profile_dialog, {.backdrop = termforge::Backdrop::Dim,
                                        .dismiss_on_click_outside = false});
    m_status = "Choose a maximum tool profile";
    return true;
  }

  auto show_tool_profiles() -> bool {
    if (m_session->active()) {
      m_status = "Finish or cancel the active run before selecting tools";
      return false;
    }
    if (!m_tool_profile_dialog) {
      m_tool_profile_dialog = std::make_unique<termforge::ChoiceWizardDialog>();
    }
    auto state = m_session->tool_profile_state();
    if (!state) {
      m_status = state.error().message;
      return false;
    }
    termforge::ChoiceWizardPage page;
    page.title = "Manage Chat tools";
    page.text =
        tool_profile_summary(*state) +
        " Every operation narrows declarations and grants no authority.";
    page.mode = termforge::ChoiceMode::Single;
    page.minimum_selected = 1;
    page.maximum_selected = 1;
    page.choices = {
        {"Named profile", "Choose the selected built-in profile."},
        {"Categories", "Enable or disable runtime tool categories."},
        {"Individual tools", "Narrow exact tools in the selected profile."},
        {"Model maximum", "Set or inherit this model's maximum profile."},
        {"Persona maximum", "Set or inherit the active persona maximum."},
        {"Reset narrowing", "Restore all tools in the selected profile."}};
    page.selected_indices = {0};
    if (!m_tool_profile_dialog->set_pages({std::move(page)})) {
      m_status = "Tool manager rejected its choices";
      return false;
    }
    m_tool_profile_dialog->on_result(
        [this](std::optional<termforge::ChoiceWizardResult> result) {
          pop_modal();
          m_tool_profile_dialog_active = false;
          if (!result || result->pages.size() != 1 ||
              result->pages.front().selected_indices.size() != 1) {
            m_status = "Tool selection unchanged";
            return;
          }
          switch (result->pages.front().selected_indices.front()) {
            case 0: static_cast<void>(show_tool_profile_picker()); return;
            case 1: static_cast<void>(show_tool_category_picker()); return;
            case 2: static_cast<void>(show_individual_tool_picker()); return;
            case 3: static_cast<void>(show_tool_maximum_picker(false)); return;
            case 4: static_cast<void>(show_tool_maximum_picker(true)); return;
            case 5:
              static_cast<void>(
                  finish_tool_mutation(m_session->reset_tool_narrowing()));
              return;
            default:
              m_status = "Tool manager returned an invalid choice";
              return;
          }
        });
    m_tool_profile_dialog_active = true;
    push_modal(*m_tool_profile_dialog, {.backdrop = termforge::Backdrop::Dim,
                                        .dismiss_on_click_outside = false});
    m_status = "Choose a tool manager action";
    return true;
  }

  auto ensure_tool_approval_dialog() -> bool {
    if (m_tool_approval_dialog_active) return true;
    const auto pending = m_session->pending_tool_approval();
    if (!pending) return true;
    if (modal()) {
      m_status = "A tool approval is waiting for the active dialog to close";
      return true;
    }
    if (!m_tool_approval_dialog) {
      m_tool_approval_dialog =
          std::make_unique<termforge::ChoiceWizardDialog>();
      m_tool_approval_controller =
          std::make_unique<ToolApprovalDialogController>(
              *m_tool_approval_dialog);
    }
    const auto run_id = pending->run_id;
    const auto invocation_id = pending->invocation_id;
    auto presented = m_tool_approval_controller->present(
        {pending->tool_name, pending->effects, pending->scopes},
        [this, run_id,
         invocation_id](runtime::ToolApprovalResolution resolution) {
          const auto decision = resolution.decision;
          pop_modal();
          m_tool_approval_dialog_active = false;
          auto decided = m_session->decide_tool_approval(run_id, invocation_id,
                                                         std::move(resolution));
          if (!decided) {
            fail(session_error(decided.error()));
            return;
          }
          auto events = m_session->drain();
          if (!events) {
            fail(session_error(events.error()));
            return;
          }
          if (!apply_events(*events)) return;
          switch (decision) {
            case domain::ApprovalDecision::approved:
              m_status = "Tool approved once; continuing run";
              break;
            case domain::ApprovalDecision::denied:
              m_status = "Tool denied; continuing run";
              break;
            case domain::ApprovalDecision::cancelled:
              m_status = "Tool approval cancelled; continuing run";
              break;
          }
        });
    if (!presented) {
      m_status = presented.error().message;
      return false;
    }
    m_tool_approval_dialog_active = true;
    push_modal(*m_tool_approval_dialog, {.backdrop = termforge::Backdrop::Dim,
                                         .dismiss_on_click_outside = false});
    m_status = "Tool approval required";
    return true;
  }

  auto ensure_question_dialog() -> bool {
    if (m_question_dialog_active) return true;
    const auto pending = m_session->pending_question_input();
    if (!pending) return true;
    if (modal()) {
      m_status = "A model question is waiting for the active dialog to close";
      return true;
    }
    if (!m_question_dialog) {
      m_question_dialog = std::make_unique<termforge::ChoiceWizardDialog>();
      m_question_controller =
          std::make_unique<AskUserDialogController>(*m_question_dialog);
    }
    auto presented =
        m_question_controller->present(*pending, *m_session, [this] {
          const bool was_cancelled = m_question_controller->was_cancelled();
          pop_modal();
          m_question_dialog_active = false;
          if (const auto& failure = m_question_controller->last_error();
              failure) {
            fail({cli::CommandFailureKind::runtime, failure->message});
            return;
          }
          auto events = m_session->drain();
          if (!events) {
            fail(session_error(events.error()));
            return;
          }
          if (!apply_events(*events)) return;
          m_status = was_cancelled ? "Question cancelled; continuing run"
                                   : "Answer recorded; continuing run";
        });
    if (!presented) {
      m_status = presented.error().message;
      return false;
    }
    m_question_dialog_active = true;
    push_modal(*m_question_dialog, {.backdrop = termforge::Backdrop::Dim,
                                    .dismiss_on_click_outside = false});
    m_status = "The model needs input";
    return true;
  }

  [[nodiscard]] auto apply_events(const std::vector<domain::RunEvent>& events)
      -> bool {
    for (const auto& event : events) {
      auto usage = m_usage_ledger.apply(event);
      if (!usage) {
        fail({cli::CommandFailureKind::runtime,
              "interactive usage update failed: " + usage.error().message});
        return false;
      }
      auto ceiling = m_spend_ceiling.apply(event);
      if (!ceiling) {
        fail({cli::CommandFailureKind::runtime,
              "interactive spend ceiling update failed: " +
                  ceiling.error().message});
        return false;
      }
      auto tool_spend = m_tool_spend_ledger.apply(event);
      if (!tool_spend) {
        fail({cli::CommandFailureKind::runtime,
              "interactive tool spend update failed: " +
                  tool_spend.error().message});
        return false;
      }
      auto applied = m_transcript.apply(event);
      if (!applied) {
        fail({cli::CommandFailureKind::runtime,
              "interactive transcript update failed: " +
                  applied.error().message});
        return false;
      }
      if (std::holds_alternative<domain::RunCompleted>(event.payload)) {
        m_status = "Ready";
      } else if (std::holds_alternative<domain::RunCancelled>(event.payload)) {
        m_status = "Cancelled";
      } else if (const auto* failed =
                     std::get_if<domain::RunFailed>(&event.payload)) {
        m_status = failed->error.message;
      }
    }
    sync_composer_focus();
    return ensure_tool_approval_dialog() &&
           (m_tool_approval_dialog_active || ensure_question_dialog());
  }

  auto fail(cli::CommandFailure value) -> void {
    if (!m_failure) m_failure = std::move(value);
    quit();
  }

  backend::Backend& m_backend;
  backend::ModelContextProvider& m_model_context;
  storage::SessionStore* m_session_store{};
  model::CatalogService* m_model_catalog{};
  backend::ProviderCharacterCatalogSource* m_provider_character_catalog{};
  VeniceConfiguredRequestSettings m_configured_request_settings;
  VeniceRequestSettingOverrides m_request_setting_overrides;
  PreviewVeniceRequestSetting m_preview_request_setting;
  PersistVeniceRequestSetting m_persist_request_setting;
  PersistToolProfileMaximum m_persist_tool_profile_maximum;
  std::string m_user_global_instruction_path;
  bool m_user_global_instructions_enabled{};
  PreviewUserGlobalInstructionEnabled m_preview_user_global_instruction_enabled;
  PersistUserGlobalInstructionEnabled m_persist_user_global_instruction_enabled;
  surfaces::ChatSessionOpen m_open_template;
  surfaces::ChatSessionDependencies m_session_dependencies;
  TermForgeRunBridge m_bridge;
  surfaces::DraftEditor& m_editor;
  std::stop_token m_stop_token;
  termforge::ByteSink* m_rendered_output{};
  std::function<void(const termforge::Screen&)> m_rendered_frame;
  bool m_poll_worker_updates{true};
  domain::UsageLedgerProjection m_usage_ledger;
  domain::SessionSpendCeilingProjection m_spend_ceiling;
  domain::ToolSpendLedgerProjection m_tool_spend_ledger;
  TranscriptView m_transcript;
  termforge::TextBox m_help;
  termforge::Composer m_composer;
  termforge::FocusRing m_focus;
  std::unique_ptr<surfaces::ChatSession> m_session;
  std::optional<cli::CommandFailure> m_setup_error;
  std::optional<cli::CommandFailure> m_failure;
  std::string m_status;
  const surfaces::SlashCommandRegistry& m_slash_commands{
      surfaces::builtin_slash_command_registry()};
  bool m_pending_edit{};
  bool m_help_visible{};
  bool m_history_enabled{true};
  std::size_t m_history_cutoff{};
  std::unique_ptr<ModelPickerDialog> m_model_picker;
  std::unique_ptr<ProviderCharacterPickerDialog> m_provider_character_picker;
  std::unique_ptr<termforge::ChoiceWizardDialog> m_settings_dialog;
  std::unique_ptr<termforge::ChoiceWizardDialog>
      m_user_global_instruction_dialog;
  std::unique_ptr<termforge::ChoiceWizardDialog>
      m_user_global_instruction_review_dialog;
  std::unique_ptr<termforge::ChoiceWizardDialog> m_tool_profile_dialog;
  std::unique_ptr<termforge::ChoiceWizardDialog> m_tool_approval_dialog;
  std::unique_ptr<ToolApprovalDialogController> m_tool_approval_controller;
  std::unique_ptr<termforge::ChoiceWizardDialog> m_question_dialog;
  std::unique_ptr<AskUserDialogController> m_question_controller;
  std::unique_ptr<termforge::ChoiceWizardDialog> m_persona_manager_dialog;
  std::unique_ptr<PersonaEditorDialog> m_persona_editor_dialog;
  std::unique_ptr<termforge::ChoiceWizardDialog> m_plan_dialog;
  std::unique_ptr<termforge::ChoiceWizardDialog> m_close_dialog;
  bool m_plan_review_active{};
  bool m_close_dialog_active{};
  bool m_settings_dialog_active{};
  bool m_user_global_instruction_dialog_active{};
  bool m_user_global_instruction_review_active{};
  bool m_tool_profile_dialog_active{};
  bool m_tool_approval_dialog_active{};
  bool m_question_dialog_active{};
  bool m_persona_manager_active{};
  bool m_persona_editor_active{};
  std::optional<std::pair<domain::SessionId, domain::PlanRevisionId>>
      m_reviewed_plan;
  std::function<void()> m_close_action;
  struct PendingUserGlobalInstructionEdit {
    std::optional<domain::UserGlobalInstructionReference> expected;
    std::string text;
  };
  std::optional<PendingUserGlobalInstructionEdit>
      m_pending_user_global_instruction_edit;
};

class ModelPickerAppImpl final : public InteractiveModelPickerApp {
 public:
  ModelPickerAppImpl(const model::CatalogSnapshot& snapshot,
                     const std::stop_token stop_token,
                     InteractiveModelPickerAppOptions options)
      : m_stop_token(stop_token), m_rendered_output(options.rendered_output),
        m_rendered_frame(std::move(options.rendered_frame)) {
    set_frame_ms(33);
    m_picker.set_models(snapshot);
    m_picker.on_result([this](std::optional<domain::ModelId> selected) {
      m_selected = std::move(selected);
      m_cancelled = !m_selected.has_value();
      m_status = m_selected
                     ? "Selected model " + std::string{m_selected->value()}
                     : "Model selection cancelled";
      pop_overlay();
      quit();
    });
    if (!snapshot.warnings.empty()) {
      m_status = snapshot.warnings.back();
    } else {
      m_status = "Choose a model";
    }
  }

  [[nodiscard]] auto selected_model() const
      -> std::optional<domain::ModelId> override {
    return m_selected;
  }

  [[nodiscard]] auto cancelled() const noexcept -> bool override {
    return m_cancelled;
  }

  [[nodiscard]] auto status_text() const noexcept -> std::string_view override {
    return m_status;
  }

  [[nodiscard]] auto configure_terminal_for_scenario(
      const termforge::TerminalIo io,
      const termforge::Capabilities& capabilities)
      -> std::expected<void, std::string> override {
    auto configured_io = terminal().set_io(io);
    if (!configured_io) return std::unexpected(configured_io.error().message);
    auto configured_capabilities = terminal().set_capabilities(capabilities);
    if (!configured_capabilities) {
      return std::unexpected(configured_capabilities.error().message);
    }
    return {};
  }

  auto on_start() -> void override {
    if (m_rendered_output != nullptr) driver().set_output(m_rendered_output);
    push_overlay(m_picker, {.backdrop = termforge::Backdrop::Fill,
                            .dismiss_on_click_outside = false});
  }

  auto on_tick(std::chrono::duration<double>) -> void override {
    if (!m_stop_token.stop_requested()) return;
    m_cancelled = true;
    m_status = "Model selection cancelled";
    quit();
  }

  auto on_render(termforge::Screen& screen) -> void override {
    screen.clear();
    if (screen.rows() > 0) {
      screen.write_text(0, screen.rows() - 1, m_status, termforge::theme::kDim,
                        termforge::theme::kBg);
    }
    if (m_rendered_frame) m_rendered_frame(screen);
  }

 private:
  std::stop_token m_stop_token;
  termforge::ByteSink* m_rendered_output{};
  std::function<void(const termforge::Screen&)> m_rendered_frame;
  ModelPickerDialog m_picker;
  std::optional<domain::ModelId> m_selected;
  std::string m_status;
  bool m_cancelled{};
};

} // namespace

auto validate_interactive_model_selection(
    const model::CatalogSnapshot& snapshot, const domain::ModelId& selected)
    -> std::expected<void, std::string> {
  if (auto valid = model::validate_catalog(snapshot); !valid) {
    return std::unexpected(valid.error().message);
  }
  const auto* entry = model::find_model(snapshot, selected, "text");
  if (entry == nullptr) {
    return std::unexpected("selected text model is no longer available");
  }
  if (entry->offline) {
    return std::unexpected("selected text model is offline");
  }
  if (!entry->context_window_tokens) {
    return std::unexpected("selected text model has invalid context metadata");
  }
  return {};
}

auto make_interactive_model_picker_app(const model::CatalogSnapshot& snapshot,
                                       const std::stop_token stop_token,
                                       InteractiveModelPickerAppOptions options)
    -> std::unique_ptr<InteractiveModelPickerApp> {
  return std::make_unique<ModelPickerAppImpl>(snapshot, stop_token,
                                              std::move(options));
}

auto make_interactive_chat_app(
    backend::Backend& backend, backend::ModelContextProvider& model_context,
    storage::SessionStore* session_store, surfaces::ChatSessionOpen open,
    surfaces::DraftEditor& editor, const std::stop_token stop_token,
    InteractiveChatAppOptions options) -> std::unique_ptr<InteractiveChatApp> {
  return std::make_unique<ChatAppImpl>(backend, model_context, session_store,
                                       std::move(open), editor, stop_token,
                                       std::move(options));
}

// clang-format off
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- Explicit startup boundaries.
auto ProcessInteractiveCommand::execute(Request request,
                                        cli::CommandEnvironment& environment,
                                        std::ostream& output,
                                        std::ostream& diagnostics)
    -> std::expected<void, cli::CommandFailure> {
  // clang-format on
  try {
    static_cast<void>(output);
    if (!environment.input_is_terminal || !environment.output_is_terminal) {
      return failure(cli::CommandFailureKind::usage,
                     "interactive chat requires terminal input and output");
    }
    auto resolved = load_config(diagnostics, request.model, request.web_search);
    if (!resolved) return std::unexpected(std::move(resolved.error()));
    auto tool_profile_maximums =
        config::resolve_tool_profile_maximum_mappings(*resolved);
    if (!tool_profile_maximums) {
      return failure(cli::CommandFailureKind::runtime,
                     tool_profile_maximums.error().message);
    }
    auto generation_options = venice_generation_options(*resolved);
    if (!generation_options) {
      return failure(request.web_search ? cli::CommandFailureKind::usage
                                        : cli::CommandFailureKind::runtime,
                     generation_options.error());
    }
    auto catalog = ProcessModelCatalog::create();
    if (!catalog)
      return failure(cli::CommandFailureKind::runtime, catalog.error().message);
    auto model =
        resolve_interactive_model(*resolved, (*catalog)->service(),
                                  *generation_options, environment.stop_token);
    if (!model) return std::unexpected(std::move(model.error()));
    if (request.model) {
      auto snapshot = (*catalog)->service().snapshot(environment.stop_token);
      if (!snapshot) {
        return failure(snapshot.error().code ==
                               model::CatalogErrorCode::cancelled
                           ? cli::CommandFailureKind::cancelled
                           : cli::CommandFailureKind::runtime,
                       snapshot.error().message);
      }
      if (model::find_model(snapshot->get(), *model, "text") == nullptr) {
        auto suggestions =
            model::suggest_models(snapshot->get(), model->value());
        std::string message =
            "unknown text model '" + std::string{model->value()} + "'";
        if (!suggestions.empty())
          message += "; did you mean '" + suggestions.front() + "'?";
        return failure(cli::CommandFailureKind::usage, std::move(message));
      }
    }

    auto credential = resolve_process_credential(diagnostics);
    if (!credential) return std::unexpected(std::move(credential.error()));
    std::optional<domain::CredentialSourceReference> credential_source;
    std::unique_ptr<backend::Backend> backend;
    backend::ProviderCharacterCatalogSource* provider_character_catalog{};
    if (credential->credential) {
      auto resolved_credential = std::move(*credential->credential);
      credential_source = resolved_credential.source;
      auto venice_backend = std::make_unique<VeniceBackend>(
          std::move(resolved_credential.secret));
      provider_character_catalog = venice_backend.get();
      backend = std::move(venice_backend);
    } else {
      backend = std::make_unique<CredentialUnavailableBackend>();
    }
    ProcessDraftEditor editor;
    const auto mode = [&] {
      switch (request.session_mode) {
        case SessionMode::create:
          return surfaces::ChatSessionOpen::Mode::create;
        case SessionMode::resume:
          return surfaces::ChatSessionOpen::Mode::resume;
        case SessionMode::continue_latest:
          return surfaces::ChatSessionOpen::Mode::continue_latest;
        case SessionMode::ephemeral:
          return surfaces::ChatSessionOpen::Mode::ephemeral;
      }
      return surfaces::ChatSessionOpen::Mode::create;
    }();
    auto provenance = process_run_provenance(*resolved, *model, "venice",
                                             std::move(credential_source));
    auto request_settings = venice_configured_request_settings(*resolved);
    if (!request_settings) {
      return failure(cli::CommandFailureKind::runtime,
                     request_settings.error());
    }
    auto effective_request_options =
        venice_effective_request_options(*request_settings);
    if (!effective_request_options) {
      return failure(cli::CommandFailureKind::runtime,
                     effective_request_options.error());
    }
    provenance.effective_request_options =
        std::move(*effective_request_options);
    auto persona_root = process_persona_root();
    std::optional<FilesystemPersonaSource> personas;
    if (persona_root) personas.emplace(std::move(*persona_root));
    auto user_global_instructions_enabled =
        config::resolve_user_global_instructions_enabled(*resolved);
    if (!user_global_instructions_enabled) {
      return failure(cli::CommandFailureKind::runtime,
                     user_global_instructions_enabled.error().message);
    }
    auto resolved_user_global_instruction_path =
        process_user_global_instruction_path();
    if (!resolved_user_global_instruction_path &&
        *user_global_instructions_enabled) {
      return failure(cli::CommandFailureKind::runtime,
                     resolved_user_global_instruction_path.error().message);
    }
    std::optional<FilesystemUserGlobalInstructionSource>
        user_global_instructions;
    if (resolved_user_global_instruction_path) {
      user_global_instructions.emplace(*resolved_user_global_instruction_path);
    }
    surfaces::ChatSessionOpen open{std::move(*model),
                                   mode,
                                   std::move(request.session_id),
                                   std::move(provenance),
                                   std::move(request.persona),
                                   request.session_spend_ceiling,
                                   std::move(*generation_options)};

    std::unique_ptr<SqliteSessionStore> store;
    if (mode != surfaces::ChatSessionOpen::Mode::ephemeral) {
      auto path = process_session_store_path();
      if (!path) {
        return failure(cli::CommandFailureKind::runtime,
                       "session storage path could not be resolved");
      }
      auto opened = SqliteSessionStore::open(*path);
      if (!opened) {
        return failure(cli::CommandFailureKind::runtime,
                       "session storage could not be opened");
      }
      store = std::move(*opened);
    }

    auto memory_settings = runtime::resolve_memory_settings(*resolved);
    if (!memory_settings) {
      return failure(cli::CommandFailureKind::runtime,
                     memory_settings.error().message);
    }
    auto restriction = tool_restriction(request.tool_restriction);
    if (!restriction) {
      return failure(cli::CommandFailureKind::usage,
                     std::move(restriction.error()));
    }
    auto approval = tool_approval(request.tool_approval);
    if (!approval) {
      return failure(cli::CommandFailureKind::usage,
                     std::move(approval.error()));
    }
    auto permission_profile_id =
        tool_launch_profile_id(*restriction, *approval);
    if (!permission_profile_id) {
      return failure(cli::CommandFailureKind::runtime,
                     "tool launch policy identity is invalid");
    }
    std::optional<domain::RepositoryId> repository_id;
    std::optional<domain::RepositorySnapshot> repository_snapshot;
    if (auto snapshot = observe_process_repository(environment.stop_token)) {
      repository_id = snapshot->root.repository_id;
      repository_snapshot = std::move(*snapshot);
    }
    std::optional<GitRepositorySnapshotSource> repository_source;
    std::optional<GitExactSourceEditor> repository_editor;
    std::unique_ptr<runtime::MemoryController> memory_controller;
    runtime::ToolRegistry tool_registry;
    runtime::ToolRegistrySnapshot tools;
    if (store) {
      memory_controller = std::make_unique<runtime::MemoryController>(
          *store, next_memory_suffix, current_timestamp,
          environment.stop_token);
      const runtime::MemoryToolConfiguration tool_configuration{
          memory_settings->global_capture != domain::MemoryCaptureMode::off,
          repository_id && memory_settings->project_capture !=
                               domain::MemoryCaptureMode::off,
          {}};
      if (tool_configuration.global_enabled ||
          tool_configuration.project_enabled) {
        auto registered =
            runtime::register_memory_tool(tool_registry, tool_configuration);
        if (!registered) {
          return failure(cli::CommandFailureKind::runtime,
                         registered.error().message);
        }
      }
    }
    if (auto registered = runtime::register_ask_user_tool(tool_registry, true);
        !registered) {
      return failure(cli::CommandFailureKind::runtime,
                     registered.error().message);
    }
    if (repository_snapshot && repository_snapshot->vcs &&
        repository_snapshot->vcs->system == "git") {
      auto source =
          open_process_repository_source(GitCommandPolicy::isolated_read_only);
      if (source) {
        repository_source.emplace(std::move(*source));
        repository_editor.emplace(
            *repository_source,
            GitExactSourceReadPolicy::tracked_regular_files);
        if (auto registered = runtime::register_repository_read_tool(
                tool_registry, *repository_source, *repository_editor,
                {repository_snapshot->root.canonical_path});
            !registered) {
          return failure(cli::CommandFailureKind::runtime,
                         registered.error().message);
        }
      }
    }
    auto tool_snapshot = tool_registry.snapshot();
    if (!tool_snapshot) {
      return failure(cli::CommandFailureKind::runtime,
                     tool_snapshot.error().message);
    }
    tools = std::move(*tool_snapshot);
    runtime::ToolLaunchPolicyConfiguration policy_configuration{
        *permission_profile_id, *restriction, *approval, {}};
    if (*approval == runtime::ApprovalMode::automatic &&
        tools.find("read_repository_file") != nullptr) {
      policy_configuration.automatically_eligible_tools = {
          "read_repository_file"};
    }
    auto tool_policy = runtime::make_tool_launch_policy(
        tools, std::move(policy_configuration));
    if (!tool_policy) {
      return failure(cli::CommandFailureKind::runtime,
                     tool_policy.error().message);
    }

    InteractiveChatAppOptions app_options;
    app_options.model_catalog = &(*catalog)->service();
    app_options.provider_character_catalog = provider_character_catalog;
    app_options.configured_request_settings = *request_settings;
    app_options.user_global_instruction_path =
        resolved_user_global_instruction_path
            ? resolved_user_global_instruction_path->string()
            : std::string{domain::user_global_instruction_source_location};
    app_options.user_global_instructions_enabled =
        *user_global_instructions_enabled;
    auto persisted_tool_profile_maximums =
        std::make_shared<config::ToolProfileMaximumMappings>(
            std::move(*tool_profile_maximums));
    if (auto config_path = config::process_config_path(); config_path) {
      app_options.preview_request_setting =
          [requested_model = request.model,
           requested_web_search = request.web_search,
           &diagnostics](const VeniceRequestSettingSave& save)
          -> std::expected<VenicePreparedPersistedSettings, std::string> {
        auto mutation = venice_config_mutation(save);
        if (!mutation) return std::unexpected(std::move(mutation.error()));
        auto resolved =
            load_config(diagnostics, requested_model, requested_web_search,
                        config::ConfigCandidate{mutation->key, mutation->value,
                                                std::nullopt});
        if (!resolved) return std::unexpected(resolved.error().message);
        auto configured = venice_configured_request_settings(*resolved);
        if (!configured) return std::unexpected(std::move(configured.error()));
        auto provenance = config::configuration_provenance(*resolved);
        auto selected =
            std::ranges::find(provenance, mutation->key,
                              &domain::ConfigurationProvenanceEntry::key);
        if (selected == provenance.end()) {
          return std::unexpected(
              "persisted request setting has no configuration provenance");
        }
        return VenicePreparedPersistedSettings{*configured,
                                               std::move(*selected)};
      };
      app_options.persist_request_setting =
          [path = *config_path](const VeniceRequestSettingSave& save)
          -> std::expected<void, std::string> {
        auto mutation = venice_config_mutation(save);
        if (!mutation) return std::unexpected(std::move(mutation.error()));
        config::JsonConfigFileStore store{path};
        const auto& registry = config::builtin_config_registry();
        auto changed = mutation->value ? store.set(registry, mutation->key,
                                                   *mutation->value)
                                       : store.unset(registry, mutation->key);
        if (!changed) return std::unexpected(changed.error().message);
        return {};
      };
      app_options.persist_tool_profile_maximum =
          [path = *config_path,
           persisted_tool_profile_maximums](const ToolProfileMaximumSave& save)
          -> std::expected<void, ToolProfileMaximumPersistError> {
        config::JsonConfigFileStore store{path};
        return persist_tool_profile_maximum_mapping(
            store, *persisted_tool_profile_maximums, save);
      };
      if (user_global_instructions) {
        app_options.preview_user_global_instruction_enabled =
            [requested_model = request.model,
             requested_web_search = request.web_search,
             &diagnostics](const bool enabled)
            -> std::expected<UserGlobalInstructionEnablePreview, std::string> {
          const auto key =
              std::string{config::user_global_instructions_enabled_key};
          auto candidate =
              load_config(diagnostics, requested_model, requested_web_search,
                          config::ConfigCandidate{
                              key, config::ConfigValue{enabled}, std::nullopt});
          if (!candidate) return std::unexpected(candidate.error().message);
          auto effective =
              config::resolve_user_global_instructions_enabled(*candidate);
          if (!effective) return std::unexpected(effective.error().message);
          auto provenance = config::configuration_provenance(*candidate);
          auto selected = std::ranges::find(
              provenance, key, &domain::ConfigurationProvenanceEntry::key);
          if (selected == provenance.end()) {
            return std::unexpected(
                "persisted user-global instruction setting has no "
                "configuration provenance");
          }
          return UserGlobalInstructionEnablePreview{*effective,
                                                    std::move(*selected)};
        };
        app_options.persist_user_global_instruction_enabled =
            [path = *config_path](const bool enabled)
            -> std::expected<void, UserGlobalInstructionEnablePersistError> {
          config::JsonConfigFileStore store{path};
          auto persisted =
              store.set(config::builtin_config_registry(),
                        config::user_global_instructions_enabled_key, enabled);
          if (!persisted) {
            return std::unexpected(UserGlobalInstructionEnablePersistError{
                persisted.error().message,
                persisted.error().effect_may_have_applied});
          }
          return {};
        };
      }
    }
    app_options.session_dependencies.persona_source =
        personas ? &*personas : nullptr;
    app_options.session_dependencies.persona_editor =
        personas ? &*personas : nullptr;
    app_options.session_dependencies.user_global_instruction_source =
        user_global_instructions ? &*user_global_instructions : nullptr;
    app_options.session_dependencies.user_global_instruction_editor =
        user_global_instructions ? &*user_global_instructions : nullptr;
    app_options.session_dependencies.user_global_instructions_enabled =
        *user_global_instructions_enabled;
    app_options.session_dependencies.tools = std::move(tools);
    app_options.session_dependencies.tool_policy = std::move(*tool_policy);
    app_options.session_dependencies.model_tool_profile_maximums =
        persisted_tool_profile_maximums->models;
    app_options.session_dependencies.persona_tool_profile_maximums =
        persisted_tool_profile_maximums->personas;
    app_options.session_dependencies.permission_profile_id =
        std::move(permission_profile_id);
    app_options.session_dependencies.memory_controller =
        memory_controller.get();
    app_options.session_dependencies.memory_settings = *memory_settings;
    app_options.session_dependencies.repository_id = repository_id;
    app_options.session_dependencies.runtime_version = runtime_version();
    auto app = make_interactive_chat_app(
        *backend, (*catalog)->service(), store.get(), std::move(open), editor,
        environment.stop_token, std::move(app_options));
    if (!app->ready()) return std::unexpected(app->setup_error());
    for (;;) {
      const int result = app->run();
      if (app->failure_state()) {
        return std::unexpected(*app->failure_state());
      }
      if (!app->pending_edit()) {
        if (result != 0) {
          return failure(cli::CommandFailureKind::runtime,
                         "interactive terminal setup failed");
        }
        return {};
      }
      app->perform_edit();
      if (environment.stop_token.stop_requested()) {
        return failure(cli::CommandFailureKind::cancelled,
                       "interactive chat cancelled");
      }
    }
  } catch (...) {
    return failure(cli::CommandFailureKind::runtime,
                   "interactive chat failed internally");
  }
}

} // namespace aiforge::adapters
