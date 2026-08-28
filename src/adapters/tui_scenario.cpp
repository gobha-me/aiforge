#include <aiforge/testing/tui_scenario.hpp>
#include <algorithm>
#include <array>
#include <exception>
#include <iomanip>
#include <optional>
#include <sstream>
#include <streambuf>
#include <utility>

namespace aiforge::testing {
namespace {

constexpr std::uint64_t kFnvOffset{14695981039346656037ULL};
constexpr std::uint64_t kFnvPrime{1099511628211ULL};

[[nodiscard]] auto failure(const TuiScenarioErrorCode code, std::string message)
    -> std::unexpected<TuiScenarioError> {
  return std::unexpected(TuiScenarioError{code, std::move(message)});
}

[[nodiscard]] auto bounded_text(const std::string_view value,
                                const std::size_t maximum) -> bool {
  if (value.empty() || value.size() > maximum) return false;
  return std::ranges::none_of(value, [](const unsigned char byte) {
    return byte == 0 || (byte < 0x20U && byte != '\t') || byte == 0x7FU;
  });
}

auto hash_append(std::uint64_t& hash, const std::string_view bytes) -> void {
  for (const unsigned char byte : bytes) {
    hash ^= byte;
    hash *= kFnvPrime;
  }
}

auto hash_size(std::uint64_t& hash, std::uint64_t value) -> void {
  std::array<char, sizeof(std::uint64_t)> bytes{};
  for (std::size_t index{}; index < bytes.size(); ++index) {
    bytes[index] = static_cast<char>(value & 0xFFU);
    value >>= 8U;
  }
  hash_append(hash, std::string_view{bytes.data(), bytes.size()});
}

[[nodiscard]] auto digest(const std::vector<std::string>& backend,
                          const std::vector<std::string>& tools)
    -> std::string {
  std::uint64_t hash{kFnvOffset};
  const auto append_list = [&](const char marker,
                               const std::vector<std::string>& values) {
    hash_append(hash, std::string_view{&marker, 1});
    hash_size(hash, values.size());
    for (const auto& value : values) {
      hash_size(hash, value.size());
      hash_append(hash, value);
    }
  };
  append_list('b', backend);
  append_list('t', tools);
  std::ostringstream output;
  output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16)
         << hash;
  return output.str();
}

[[nodiscard]] auto digest(const std::string_view value) -> std::string {
  std::uint64_t hash{kFnvOffset};
  hash_append(hash, value);
  std::ostringstream output;
  output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16)
         << hash;
  return output.str();
}

[[nodiscard]] auto same_capabilities(const termforge::Capabilities& lhs,
                                     const termforge::Capabilities& rhs)
    -> bool {
  return lhs.kitty_graphics == rhs.kitty_graphics && lhs.sixel == rhs.sixel &&
         lhs.truecolor == rhs.truecolor &&
         lhs.color_levels == rhs.color_levels &&
         lhs.kitty_keyboard == rhs.kitty_keyboard &&
         lhs.sync_updates == rhs.sync_updates &&
         lhs.kitty_animation == rhs.kitty_animation;
}

[[nodiscard]] auto checked_add(std::size_t& total, const std::size_t value,
                               const std::size_t maximum) -> bool {
  if (value > maximum || total > maximum - value) return false;
  total += value;
  return true;
}

class BoundedStringSink final : public termforge::ByteSink {
 public:
  explicit BoundedStringSink(const std::size_t maximum) : m_maximum(maximum) {}

  [[nodiscard]] auto write(const std::span<const char> bytes)
      -> std::expected<void, termforge::ErrorEvent> override {
    if (bytes.size() > m_maximum - m_value.size()) {
      m_exceeded = true;
      return std::unexpected(
          termforge::ErrorEvent{termforge::Severity::Error, "aiforge.scenario",
                                "rendered output exceeds its scenario limit"});
    }
    if (!bytes.empty()) m_value.append(bytes.data(), bytes.size());
    return {};
  }

  [[nodiscard]] auto exceeded() const noexcept -> bool { return m_exceeded; }
  [[nodiscard]] auto take() -> std::string { return std::move(m_value); }

 private:
  std::size_t m_maximum{};
  std::string m_value;
  bool m_exceeded{};
};

class BoundedStreamBuffer final : public std::streambuf {
 public:
  explicit BoundedStreamBuffer(const std::size_t maximum)
      : m_maximum(maximum) {}

  [[nodiscard]] auto exceeded() const noexcept -> bool { return m_exceeded; }
  [[nodiscard]] auto take() -> std::string { return std::move(m_value); }

 protected:
  auto xsputn(const char* data, const std::streamsize count)
      -> std::streamsize override {
    if (count <= 0) return 0;
    const auto requested = static_cast<std::size_t>(count);
    const auto accepted = std::min(requested, m_maximum - m_value.size());
    m_value.append(data, accepted);
    if (accepted != requested) m_exceeded = true;
    return static_cast<std::streamsize>(accepted);
  }

  auto overflow(const int value) -> int override {
    if (traits_type::eq_int_type(value, traits_type::eof())) {
      return traits_type::not_eof(value);
    }
    if (m_value.size() == m_maximum) {
      m_exceeded = true;
      return traits_type::eof();
    }
    m_value.push_back(traits_type::to_char_type(value));
    return value;
  }

 private:
  std::size_t m_maximum{};
  std::string m_value;
  bool m_exceeded{};
};

[[nodiscard]] auto validate(const TuiScenario& scenario)
    -> std::expected<void, TuiScenarioError> {
  if (scenario.schema_version != 1 ||
      !bounded_text(scenario.scenario_id, 256) ||
      !bounded_text(scenario.corpus_version, 256) ||
      !bounded_text(scenario.application_revision, 512)) {
    return failure(TuiScenarioErrorCode::invalid_scenario,
                   "scenario identity or schema is invalid");
  }
  const auto& limits = scenario.limits;
  if (limits.maximum_steps == 0 || limits.maximum_frames == 0 ||
      limits.maximum_script_bytes == 0 || limits.maximum_trace_bytes == 0 ||
      limits.maximum_wire_bytes == 0 || limits.maximum_frame_bytes == 0 ||
      limits.maximum_total_frame_bytes == 0 ||
      scenario.steps.size() > limits.maximum_steps ||
      scenario.initial_size.cols <= 0 || scenario.initial_size.rows <= 0 ||
      scenario.initial_size.cols > termforge::App::kMaxPushedDim ||
      scenario.initial_size.rows > termforge::App::kMaxPushedDim ||
      scenario.initial_size.px_w < 0 || scenario.initial_size.px_h < 0 ||
      scenario.initial_size.px_w > termforge::App::kMaxPushedDim ||
      scenario.initial_size.px_h > termforge::App::kMaxPushedDim) {
    return failure(TuiScenarioErrorCode::invalid_scenario,
                   "scenario limits or initial terminal size are invalid");
  }
  std::size_t script_bytes{};
  for (const auto* script : {&scenario.backend_script, &scenario.tool_script}) {
    for (const auto& step : *script) {
      if (!bounded_text(step, limits.maximum_script_bytes) ||
          !checked_add(script_bytes, step.size(),
                       limits.maximum_script_bytes)) {
        return failure(TuiScenarioErrorCode::resource_limit,
                       "scenario fake script exceeds its byte limit");
      }
    }
  }
  std::uint64_t previous{};
  bool first{true};
  std::size_t backend_releases{};
  std::size_t tool_releases{};
  for (const auto& step : scenario.steps) {
    if (step.after_frame >= limits.maximum_frames ||
        (!first && step.after_frame < previous)) {
      return failure(
          TuiScenarioErrorCode::invalid_scenario,
          "scenario actions are out of order or outside the frame limit");
    }
    first = false;
    previous = step.after_frame;
    if (const auto* resize = std::get_if<TuiScenarioResize>(&step.action)) {
      if (resize->size.cols <= 0 || resize->size.rows <= 0 ||
          resize->size.cols > termforge::App::kMaxPushedDim ||
          resize->size.rows > termforge::App::kMaxPushedDim ||
          resize->size.px_w < 0 || resize->size.px_h < 0 ||
          resize->size.px_w > termforge::App::kMaxPushedDim ||
          resize->size.px_h > termforge::App::kMaxPushedDim) {
        return failure(TuiScenarioErrorCode::invalid_scenario,
                       "scenario resize is invalid");
      }
    }
    if (const auto* release = std::get_if<TuiScenarioRelease>(&step.action)) {
      if (release->producer == TuiScenarioProducer::backend) {
        ++backend_releases;
      } else {
        ++tool_releases;
      }
    }
  }
  if (backend_releases != scenario.backend_script.size() ||
      tool_releases != scenario.tool_script.size()) {
    return failure(TuiScenarioErrorCode::script_mismatch,
                   "scenario releases do not match the fake scripts");
  }
  return {};
}

struct PassState {
  std::size_t next_action{};
  std::size_t next_backend{};
  std::size_t next_tool{};
  std::size_t frame_count{};
  std::size_t total_frame_bytes{};
  std::optional<TuiScenarioError> error;
  TuiScenarioObservation observation;
};

auto set_error(PassState& state, const TuiScenarioErrorCode code,
               std::string message) -> void {
  if (!state.error) state.error = TuiScenarioError{code, std::move(message)};
}

auto release_step(PassState& state, const TuiScenarioTarget& target,
                  const TuiScenario& scenario,
                  const TuiScenarioProducer producer) -> void {
  const auto backend = producer == TuiScenarioProducer::backend;
  auto& index = backend ? state.next_backend : state.next_tool;
  const auto& script = backend ? scenario.backend_script : scenario.tool_script;
  const auto& callback =
      backend ? target.release_backend_step : target.release_tool_step;
  if (index >= script.size() || !callback) {
    set_error(state, TuiScenarioErrorCode::script_mismatch,
              backend ? "backend script release is unavailable"
                      : "tool script release is unavailable");
    return;
  }
  try {
    auto released = callback(script[index]);
    if (!released) {
      set_error(state, TuiScenarioErrorCode::target_failure,
                backend ? "backend script release failed: " + released.error()
                        : "tool script release failed: " + released.error());
      return;
    }
    ++index;
  } catch (...) {
    set_error(state, TuiScenarioErrorCode::target_failure,
              backend ? "backend script release threw"
                      : "tool script release threw");
  }
}

auto observe_frame(PassState& state, TuiScenarioTarget& target,
                   const TuiScenario& scenario, const TuiScenarioPass pass)
    -> void {
  if (state.error) {
    target.app->quit();
    return;
  }
  std::string normalized;
  try {
    if (target.normalized_frame) normalized = target.normalized_frame();
  } catch (...) {
    set_error(state, TuiScenarioErrorCode::target_failure,
              "normalized frame capture threw");
    target.app->quit();
    return;
  }
  if (normalized.size() > scenario.limits.maximum_frame_bytes ||
      !checked_add(state.total_frame_bytes, normalized.size(),
                   scenario.limits.maximum_total_frame_bytes)) {
    set_error(state, TuiScenarioErrorCode::resource_limit,
              "normalized frame output exceeds its limit");
    target.app->quit();
    return;
  }
  state.observation.normalized_frames.push_back(std::move(normalized));

  const auto completed_frame = state.frame_count++;
  while (state.next_action < scenario.steps.size() &&
         scenario.steps[state.next_action].after_frame == completed_frame) {
    const auto& action = scenario.steps[state.next_action++].action;
    if (const auto* release = std::get_if<TuiScenarioRelease>(&action)) {
      release_step(state, target, scenario, release->producer);
    } else if (pass == TuiScenarioPass::record) {
      if (const auto* posted = std::get_if<TuiScenarioPost>(&action)) {
        target.app->post(posted->event);
      } else if (const auto* resize = std::get_if<TuiScenarioResize>(&action)) {
        auto pushed = target.app->set_size(resize->size);
        if (!pushed) {
          set_error(state, TuiScenarioErrorCode::target_failure,
                    "scenario resize was refused");
        }
      }
    }
    if (state.error) {
      target.app->quit();
      return;
    }
  }
  if (state.frame_count >= scenario.limits.maximum_frames &&
      target.app->running()) {
    set_error(state, TuiScenarioErrorCode::resource_limit,
              "scenario exceeded its frame limit");
    target.app->quit();
  }
}

[[nodiscard]] auto configure_target(TuiScenarioTarget& target,
                                    const TuiScenario& scenario)
    -> std::expected<void, TuiScenarioError> {
  if (!target.app || !target.configure_terminal || !target.run_recording ||
      !target.normalized_frame || !target.semantic_state) {
    return failure(TuiScenarioErrorCode::target_failure,
                   "scenario target is incomplete");
  }
  auto capabilities = target.configure_terminal(scenario.terminal_capabilities);
  if (!capabilities) {
    return failure(TuiScenarioErrorCode::target_failure,
                   "scenario terminal capabilities were refused: " +
                       capabilities.error());
  }
  auto size = target.app->set_size(scenario.initial_size);
  if (!size) {
    return failure(TuiScenarioErrorCode::target_failure,
                   "scenario initial terminal size was refused");
  }
  return {};
}

[[nodiscard]] auto finish_observation(PassState& state,
                                      const TuiScenarioTarget& target,
                                      const TuiScenario& scenario,
                                      BoundedStringSink& wire)
    -> std::expected<TuiScenarioObservation, TuiScenarioError> {
  if (state.error) return std::unexpected(std::move(*state.error));
  if (state.next_action != scenario.steps.size() ||
      state.next_backend != scenario.backend_script.size() ||
      state.next_tool != scenario.tool_script.size()) {
    return failure(TuiScenarioErrorCode::script_mismatch,
                   "scenario ended before all actions were consumed");
  }
  if (wire.exceeded()) {
    return failure(TuiScenarioErrorCode::resource_limit,
                   "scenario rendered output exceeds its limit");
  }
  state.observation.wire_output = wire.take();
  try {
    state.observation.semantic_state = target.semantic_state();
  } catch (...) {
    return failure(TuiScenarioErrorCode::target_failure,
                   "semantic state capture threw");
  }
  if (state.observation.semantic_state.size() >
      scenario.limits.maximum_frame_bytes) {
    return failure(TuiScenarioErrorCode::resource_limit,
                   "scenario semantic state exceeds its limit");
  }
  return std::move(state.observation);
}

} // namespace

auto replay_tui_scenario(const TuiScenario& scenario,
                         const std::string_view trace,
                         const TuiScenarioTargetFactory& factory)
    -> std::expected<TuiScenarioObservation, TuiScenarioError> {
  try {
    auto valid = validate(scenario);
    if (!valid) return std::unexpected(std::move(valid.error()));
    if (!factory) {
      return failure(TuiScenarioErrorCode::target_failure,
                     "scenario target factory is unavailable");
    }
    if (trace.empty() || trace.size() > scenario.limits.maximum_trace_bytes) {
      return failure(trace.empty() ? TuiScenarioErrorCode::trace_failure
                                   : TuiScenarioErrorCode::resource_limit,
                     trace.empty() ? "scenario replay trace is empty"
                                   : "scenario trace exceeds its limit");
    }
    BoundedStringSink replayed_wire{scenario.limits.maximum_wire_bytes};
    auto target = factory(TuiScenarioPass::replay, &replayed_wire);
    if (!target) return std::unexpected(std::move(target.error()));
    auto configured = configure_target(*target, scenario);
    if (!configured) return std::unexpected(std::move(configured.error()));
    PassState state;
    target->app->set_frame_observer([&](const termforge::FrameObservation&) {
      observe_frame(state, *target, scenario, TuiScenarioPass::replay);
    });
    std::istringstream trace_input{std::string{trace}};
    auto played = target->app->play(trace_input);
    if (!played) {
      return failure(TuiScenarioErrorCode::trace_failure,
                     "scenario trace replay failed: " + played.error().message);
    }
    return finish_observation(state, *target, scenario, replayed_wire);
  } catch (...) {
    return failure(TuiScenarioErrorCode::internal_failure,
                   "scenario replay failed internally");
  }
}

auto replay_tui_scenario(const TuiScenario& scenario,
                         const TuiScenarioResult& recording,
                         const TuiScenarioTargetFactory& factory)
    -> std::expected<TuiScenarioObservation, TuiScenarioError> {
  try {
    auto valid = validate(scenario);
    if (!valid) return std::unexpected(std::move(valid.error()));
    if (scenario.scenario_id != recording.scenario_id ||
        scenario.corpus_version != recording.corpus_version ||
        scenario.application_revision != recording.application_revision ||
        !same_capabilities(scenario.terminal_capabilities,
                           recording.terminal_capabilities) ||
        digest(scenario.backend_script, scenario.tool_script) !=
            recording.fake_script_digest) {
      return failure(TuiScenarioErrorCode::provenance_mismatch,
                     "scenario recording provenance does not match");
    }
    if (digest(recording.trace) != recording.trace_digest) {
      return failure(TuiScenarioErrorCode::trace_failure,
                     "scenario recording trace identity does not match");
    }
    return replay_tui_scenario(scenario, recording.trace, factory);
  } catch (...) {
    return failure(TuiScenarioErrorCode::internal_failure,
                   "scenario provenance verification failed internally");
  }
}

auto run_tui_scenario(const TuiScenario& scenario,
                      const TuiScenarioTargetFactory& factory)
    -> std::expected<TuiScenarioResult, TuiScenarioError> {
  try {
    auto valid = validate(scenario);
    if (!valid) return std::unexpected(std::move(valid.error()));
    if (!factory) {
      return failure(TuiScenarioErrorCode::target_failure,
                     "scenario target factory is unavailable");
    }

    BoundedStringSink recorded_wire{scenario.limits.maximum_wire_bytes};
    auto recorded_target = factory(TuiScenarioPass::record, &recorded_wire);
    if (!recorded_target) {
      return std::unexpected(std::move(recorded_target.error()));
    }
    auto configured = configure_target(*recorded_target, scenario);
    if (!configured) return std::unexpected(std::move(configured.error()));

    termforge::SyntheticClock clock;
    recorded_target->app->set_clock(&clock);
    PassState recorded_state;
    recorded_target->app->set_frame_observer(
        [&](const termforge::FrameObservation&) {
          observe_frame(recorded_state, *recorded_target, scenario,
                        TuiScenarioPass::record);
        });
    BoundedStreamBuffer trace_buffer{scenario.limits.maximum_trace_bytes};
    std::ostream trace{&trace_buffer};
    recorded_target->app->start_recording(trace);
    const int record_exit = recorded_target->run_recording();
    if (record_exit != 0) {
      return failure(TuiScenarioErrorCode::target_failure,
                     "recording target exited unsuccessfully");
    }
    auto recorded = finish_observation(recorded_state, *recorded_target,
                                       scenario, recorded_wire);
    if (!recorded) return std::unexpected(std::move(recorded.error()));
    if (trace_buffer.exceeded()) {
      return failure(TuiScenarioErrorCode::resource_limit,
                     "scenario trace exceeds its limit");
    }
    const auto trace_bytes = trace_buffer.take();
    if (trace_bytes.empty()) {
      return failure(TuiScenarioErrorCode::trace_failure,
                     "scenario recording produced no trace");
    }

    auto replayed = replay_tui_scenario(scenario, trace_bytes, factory);
    if (!replayed) return std::unexpected(std::move(replayed.error()));
    if (*recorded != *replayed) {
      std::string detail;
      if (recorded->normalized_frames != replayed->normalized_frames) {
        detail += " normalized-frames(" +
                  std::to_string(recorded->normalized_frames.size()) + "/" +
                  std::to_string(replayed->normalized_frames.size()) + ")";
        const auto compared = std::min(recorded->normalized_frames.size(),
                                       replayed->normalized_frames.size());
        for (std::size_t index{}; index < compared; ++index) {
          if (recorded->normalized_frames[index] !=
              replayed->normalized_frames[index]) {
            detail += "@" + std::to_string(index) + "(" +
                      digest(recorded->normalized_frames[index]) + "/" +
                      digest(replayed->normalized_frames[index]) + ")";
            break;
          }
        }
      }
      if (recorded->wire_output != replayed->wire_output) {
        detail += " wire-output(" +
                  std::to_string(recorded->wire_output.size()) + "/" +
                  std::to_string(replayed->wire_output.size()) + ")";
      }
      if (recorded->semantic_state != replayed->semantic_state) {
        detail += " semantic-state";
      }
      return failure(TuiScenarioErrorCode::replay_diverged,
                     "scenario replay diverged from the recording:" + detail);
    }

    return TuiScenarioResult{
        scenario.scenario_id,
        scenario.corpus_version,
        scenario.application_revision,
        digest(scenario.backend_script, scenario.tool_script),
        digest(trace_bytes),
        scenario.terminal_capabilities,
        trace_bytes,
        std::move(*recorded),
        std::move(*replayed),
    };
  } catch (...) {
    return failure(TuiScenarioErrorCode::internal_failure,
                   "scenario execution failed internally");
  }
}

} // namespace aiforge::testing
