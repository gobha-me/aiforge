#include <aiforge/adapters/filesystem_persona_source.hpp>
#include <aiforge/adapters/interactive_chat_app.hpp>
#include <aiforge/adapters/model_picker_dialog.hpp>
#include <aiforge/adapters/process_credentials.hpp>
#include <aiforge/adapters/process_draft_editor.hpp>
#include <aiforge/adapters/process_interactive.hpp>
#include <aiforge/adapters/process_model_catalog.hpp>
#include <aiforge/adapters/process_provenance.hpp>
#include <aiforge/adapters/process_repository.hpp>
#include <aiforge/adapters/sqlite_session_store.hpp>
#include <aiforge/adapters/termforge_run_bridge.hpp>
#include <aiforge/adapters/transcript_view.hpp>
#include <aiforge/adapters/venice_backend.hpp>
#include <aiforge/config/config.hpp>
#include <aiforge/config/file_store.hpp>
#include <aiforge/domain/usage_ledger.hpp>
#include <aiforge/runtime/memory_controller.hpp>
#include <aiforge/runtime/memory_tool.hpp>
#include <aiforge/runtime/tool_registry.hpp>
#include <aiforge/surfaces/chat_session.hpp>
#include <aiforge/surfaces/slash_commands.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
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

[[nodiscard]] auto load_config(
    std::ostream& diagnostics,
    const std::optional<std::string>& requested_model)
    -> std::expected<config::ResolvedConfig, cli::CommandFailure> {
  const auto& registry = config::builtin_config_registry();
  std::vector<config::ConfigLayer> layers;
  if (requested_model) {
    layers.push_back(config::ConfigLayer{
        config::ConfigSource::command_line,
        {{"model", config::ConfigValue{*requested_model}, std::nullopt}},
        {}});
  }
  auto environment = config::environment_config_layer(registry);
  if (!environment) {
    return failure(cli::CommandFailureKind::runtime,
                   "configuration environment could not be read");
  }
  layers.push_back(std::move(*environment));
  auto path = config::process_config_path();
  if (!path) {
    if (!warning(diagnostics, path.error().message)) {
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
    }
  } else {
    auto file = config::JsonConfigFileStore{*path}.load(registry);
    if (file) {
      layers.push_back(std::move(*file));
    } else if (!warning(diagnostics, file.error().message)) {
      return failure(cli::CommandFailureKind::runtime,
                     "diagnostic output failed");
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

[[nodiscard]] auto configured_model(const config::ResolvedConfig& resolved)
    -> std::expected<domain::ModelId, cli::CommandFailure> {
  const auto* entry = resolved.find("model");
  if (entry == nullptr || !entry->value) {
    return failure(
        cli::CommandFailureKind::runtime,
        "model is not configured; set AIFORGE_MODEL or config model");
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
  return std::move(*model);
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
    const auto spend =
        domain::summarize_session_spend(ledger.records(), *ceiling.ceiling());
    result += " | spend ";
    if (spend.accounted) {
      result += spend.accounted->amount().to_string() + "/" +
                spend.ceiling.amount().to_string() + " USD";
      if (spend.reached) result += " reached";
    } else {
      result += "unavailable/" + spend.ceiling.amount().to_string() + " USD";
    }
  }
  return result;
}

[[nodiscard]] auto usage_panel_lines(
    const domain::UsageLedgerProjection& ledger,
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
    const auto spend =
        domain::summarize_session_spend(ledger.records(), *ceiling.ceiling());
    lines.push_back("Spend ceiling (USD): " +
                    spend.ceiling.amount().to_string());
    if (spend.accounted) {
      lines.push_back("Accounted spend (USD): " +
                      spend.accounted->amount().to_string());
      lines.push_back("Remaining spend (USD): " +
                      spend.remaining->amount().to_string());
      lines.push_back(std::string{"Spend ceiling state: "} +
                      (spend.reached ? "reached" : "open"));
      lines.push_back(std::format("Spend coverage: {} provider-reported + {} "
                                  "catalog-derived of {} inferences",
                                  spend.reported_inferences,
                                  spend.estimated_inferences,
                                  spend.total_inferences));
    } else {
      lines.push_back("Accounted spend (USD): unavailable");
      for (const auto& failure : spend.unavailable) {
        lines.push_back(
            "Spend unavailable: " +
            std::string{domain::cost_estimate_reason_name(failure.reason)} +
            "=" + std::to_string(failure.count));
      }
      if (spend.aggregation_failure) {
        lines.push_back("Spend unavailable: " +
                        std::string{domain::cost_estimate_reason_name(
                            *spend.aggregation_failure)});
      }
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
    const auto original = m_composer.text();
    auto edited = m_editor.edit(original, m_stop_token);
    if (!edited) {
      m_status = edited.error().message;
      return;
    }
    m_composer.set_text(std::move(*edited));
    m_status = "Draft updated by editor";
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
    ensure_plan_review();
  }

  auto on_event(const termforge::Event& event) -> void override {
    if (!m_session) return;
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
    ensure_plan_review();
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
    header += " | " + usage_header_text(m_usage_ledger, m_spend_ceiling);
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
    quit();
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

  auto show_usage() -> bool {
    show_panel("Session usage, cost, and spend ceiling",
               usage_panel_lines(m_usage_ledger, m_spend_ceiling),
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
      if (*session_id == m_session->session_id()) {
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
    auto rebuilt = m_transcript.rebuild((*candidate)->event_log().events());
    if (!rebuilt) {
      m_status = "Interactive transcript replay failed";
      return false;
    }

    m_session = std::move(*candidate);
    m_usage_ledger = std::move(*candidate_usage);
    m_spend_ceiling = std::move(*candidate_ceiling);
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

  auto execute_command(const surfaces::SlashCommandResult& command) -> bool {
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
      case surfaces::SlashCommandAction::choose_model: {
        if (command.subject) {
          auto model_id = domain::ModelId::from(*command.subject);
          if (!model_id) {
            m_status = "Model ID is invalid";
            return false;
          }
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
      pop_overlay();
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
    push_overlay(*m_plan_dialog, {.backdrop = termforge::Backdrop::Dim,
                                  .dismiss_on_click_outside = false});
    m_status = "Review the exact proposed plan";
  }

  auto request_close(std::function<void()> action = {}) -> void {
    if (!action) action = [this] { quit(); };
    if (m_close_dialog_active || m_plan_review_active) {
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
          pop_overlay();
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
    push_overlay(*m_close_dialog, {.backdrop = termforge::Backdrop::Dim,
                                   .dismiss_on_click_outside = false});
    m_status = "Resolve session cleanup before leaving";
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
      m_model_picker->on_close([this] { pop_overlay(); });
      m_model_picker->on_result(
          [this](std::optional<domain::ModelId> selected) {
            if (!selected) {
              m_status = "Model selection cancelled";
              return;
            }
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
    push_overlay(*m_model_picker, {.backdrop = termforge::Backdrop::Dim,
                                   .dismiss_on_click_outside = false});
    if (!snapshot->get().warnings.empty())
      m_status = snapshot->get().warnings.back();
    else
      m_status = "Choose a model";
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
      auto applied = m_transcript.apply(event);
      if (!applied) {
        fail({cli::CommandFailureKind::runtime,
              "interactive transcript update failed"});
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
    return true;
  }

  auto fail(cli::CommandFailure value) -> void {
    if (!m_failure) m_failure = std::move(value);
    quit();
  }

  backend::Backend& m_backend;
  backend::ModelContextProvider& m_model_context;
  storage::SessionStore* m_session_store{};
  model::CatalogService* m_model_catalog{};
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
  std::unique_ptr<termforge::ChoiceWizardDialog> m_plan_dialog;
  std::unique_ptr<termforge::ChoiceWizardDialog> m_close_dialog;
  bool m_plan_review_active{};
  bool m_close_dialog_active{};
  std::optional<std::pair<domain::SessionId, domain::PlanRevisionId>>
      m_reviewed_plan;
  std::function<void()> m_close_action;
};

} // namespace

auto make_interactive_chat_app(
    backend::Backend& backend, backend::ModelContextProvider& model_context,
    storage::SessionStore* session_store, surfaces::ChatSessionOpen open,
    surfaces::DraftEditor& editor, const std::stop_token stop_token,
    InteractiveChatAppOptions options) -> std::unique_ptr<InteractiveChatApp> {
  return std::make_unique<ChatAppImpl>(backend, model_context, session_store,
                                       std::move(open), editor, stop_token,
                                       std::move(options));
}

auto ProcessInteractiveCommand::execute(Request request,
                                        cli::CommandEnvironment& environment,
                                        std::ostream& output,
                                        std::ostream& diagnostics)
    -> std::expected<void, cli::CommandFailure> {
  try {
    static_cast<void>(output);
    if (!environment.input_is_terminal || !environment.output_is_terminal) {
      return failure(cli::CommandFailureKind::usage,
                     "interactive chat requires terminal input and output");
    }
    auto resolved = load_config(diagnostics, request.model);
    if (!resolved) return std::unexpected(std::move(resolved.error()));
    auto model = configured_model(*resolved);
    if (!model) return std::unexpected(std::move(model.error()));
    auto catalog = ProcessModelCatalog::create();
    if (!catalog)
      return failure(cli::CommandFailureKind::runtime, catalog.error().message);
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
    if (credential->credential) {
      auto resolved_credential = std::move(*credential->credential);
      credential_source = resolved_credential.source;
      backend = std::make_unique<VeniceBackend>(
          std::move(resolved_credential.secret));
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
    auto persona_root = process_persona_root();
    std::optional<FilesystemPersonaSource> personas;
    if (persona_root) personas.emplace(std::move(*persona_root));
    surfaces::ChatSessionOpen open{std::move(*model),
                                   mode,
                                   std::move(request.session_id),
                                   std::move(provenance),
                                   std::move(request.persona),
                                   std::move(request.session_spend_ceiling)};

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
    std::optional<domain::RepositoryId> repository_id;
    if (auto snapshot = observe_process_repository(environment.stop_token)) {
      repository_id = snapshot->root.repository_id;
    }
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
        auto snapshot = tool_registry.snapshot();
        if (!snapshot) {
          return failure(cli::CommandFailureKind::runtime,
                         snapshot.error().message);
        }
        tools = std::move(*snapshot);
      }
    }

    InteractiveChatAppOptions app_options;
    app_options.model_catalog = &(*catalog)->service();
    app_options.session_dependencies.persona_source =
        personas ? &*personas : nullptr;
    app_options.session_dependencies.tools = std::move(tools);
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
