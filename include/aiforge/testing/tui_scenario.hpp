#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <termforge/core/app.hpp>
#include <termforge/core/byte_sink.hpp>
#include <variant>
#include <vector>

namespace aiforge::testing {

enum class TuiScenarioPass {
  record,
  replay,
};

enum class TuiScenarioProducer {
  backend,
  tool,
};

struct TuiScenarioPost {
  termforge::Event event;
};

struct TuiScenarioResize {
  termforge::App::Size size;
};

struct TuiScenarioRelease {
  TuiScenarioProducer producer{TuiScenarioProducer::backend};
};

using TuiScenarioAction =
    std::variant<TuiScenarioPost, TuiScenarioResize, TuiScenarioRelease>;

struct TuiScenarioStep {
  // Actions run from the frame observer after this zero-based frame. Semantic
  // input and resize actions are recorded once and come from the trace during
  // replay; fake producer releases run in both passes.
  std::uint64_t after_frame{};
  TuiScenarioAction action;
};

struct TuiScenarioLimits {
  std::size_t maximum_steps{4096};
  std::size_t maximum_frames{4096};
  std::size_t maximum_script_bytes{8U * 1024U * 1024U};
  std::size_t maximum_trace_bytes{64U * 1024U * 1024U};
  std::size_t maximum_wire_bytes{64U * 1024U * 1024U};
  std::size_t maximum_frame_bytes{4U * 1024U * 1024U};
  std::size_t maximum_total_frame_bytes{64U * 1024U * 1024U};
};

struct TuiScenario {
  std::uint32_t schema_version{1};
  std::string scenario_id;
  std::string corpus_version;
  std::string application_revision;
  termforge::Capabilities terminal_capabilities{};
  termforge::App::Size initial_size{80, 24, 0, 0};
  std::vector<std::string> backend_script;
  std::vector<std::string> tool_script;
  std::vector<TuiScenarioStep> steps;
  TuiScenarioLimits limits{};
};

enum class TuiScenarioErrorCode {
  invalid_scenario,
  resource_limit,
  target_failure,
  script_mismatch,
  provenance_mismatch,
  trace_failure,
  replay_diverged,
  internal_failure,
};

struct TuiScenarioError {
  TuiScenarioErrorCode code{TuiScenarioErrorCode::internal_failure};
  std::string message;
  auto operator==(const TuiScenarioError&) const -> bool = default;
};

using TuiScenarioReleaseStep =
    std::function<std::expected<void, std::string>(std::string_view)>;

struct TuiScenarioTarget {
  std::unique_ptr<termforge::App> app;
  std::function<std::expected<void, std::string>(
      const termforge::Capabilities&)>
      configure_terminal;
  std::function<int()> run_recording;
  TuiScenarioReleaseStep release_backend_step;
  TuiScenarioReleaseStep release_tool_step;
  std::function<std::string()> normalized_frame;
  std::function<std::string()> semantic_state;
};

// The output sink is borrowed for the lifetime of the returned app and
// enforces the scenario's wire-byte limit while the app renders.
using TuiScenarioTargetFactory =
    std::function<std::expected<TuiScenarioTarget, TuiScenarioError>(
        TuiScenarioPass, termforge::ByteSink*)>;

struct TuiScenarioObservation {
  std::vector<std::string> normalized_frames;
  std::string wire_output;
  std::string semantic_state;
  auto operator==(const TuiScenarioObservation&) const -> bool = default;
};

struct TuiScenarioResult {
  std::string scenario_id;
  std::string corpus_version;
  std::string application_revision;
  std::string fake_script_digest;
  std::string trace_digest;
  termforge::Capabilities terminal_capabilities{};
  std::string trace;
  TuiScenarioObservation recorded;
  TuiScenarioObservation replayed;
};

[[nodiscard]] auto run_tui_scenario(const TuiScenario& scenario,
                                    const TuiScenarioTargetFactory& factory)
    -> std::expected<TuiScenarioResult, TuiScenarioError>;

[[nodiscard]] auto replay_tui_scenario(const TuiScenario& scenario,
                                       std::string_view trace,
                                       const TuiScenarioTargetFactory& factory)
    -> std::expected<TuiScenarioObservation, TuiScenarioError>;

[[nodiscard]] auto replay_tui_scenario(const TuiScenario& scenario,
                                       const TuiScenarioResult& recording,
                                       const TuiScenarioTargetFactory& factory)
    -> std::expected<TuiScenarioObservation, TuiScenarioError>;

}  // namespace aiforge::testing
