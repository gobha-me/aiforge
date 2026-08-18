#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <aiforge/backend/backend.hpp>
#include <aiforge/domain/event_log.hpp>
#include <aiforge/domain/run_projection.hpp>
#include <aiforge/storage/session_store.hpp>

namespace aiforge::runtime {

enum class RunKernelErrorCode {
  invalid_limits,
  invalid_start,
  run_already_active,
  no_active_run,
  wrong_run,
  wrong_inference,
  already_terminal,
  protocol_failure,
  event_sequence_overflow,
  event_log_rejected,
  projection_rejected,
  storage_failure,
  replay_rejected,
  internal_failure,
};

struct RunKernelError {
  RunKernelErrorCode code;
  std::string message;
  bool retryable{};
  auto operator==(const RunKernelError&) const -> bool = default;
};

enum class DurableSessionMode {
  create,
  resume,
};

struct DurableSessionOpen {
  domain::SessionId session_id;
  DurableSessionMode mode{DurableSessionMode::create};
  domain::EventTimestamp created_at;
  auto operator==(const DurableSessionOpen&) const -> bool = default;
};

struct RunKernelLimits {
  std::size_t pending_updates{256};
  std::size_t tool_argument_bytes{8U * 1024U * 1024U};
  auto operator==(const RunKernelLimits&) const -> bool = default;
};

struct RunStart {
  domain::RunId run_id;
  domain::RunStarted attributes;
  domain::Message user_message;
  backend::BackendRequest request;
  auto operator==(const RunStart&) const -> bool = default;
};

// Implementations may be called from a backend worker. They must not mutate
// UI state directly; a surface should only wake or enqueue an event for its
// owner thread.
class RunWakeSink {
 public:
  virtual ~RunWakeSink() = default;
  virtual auto wake() noexcept -> void = 0;
};

using TimestampSource = std::function<domain::EventTimestamp()>;

class RunKernel final {
 public:
  RunKernel(domain::SessionId session_id, backend::Backend& backend,
            RunWakeSink* wake_sink = nullptr,
            TimestampSource timestamp_source = {}, RunKernelLimits limits = {});

  [[nodiscard]] static auto open_durable(
      DurableSessionOpen session, storage::SessionStore& store,
      backend::Backend& backend, RunWakeSink* wake_sink = nullptr,
      TimestampSource timestamp_source = {}, RunKernelLimits limits = {})
      -> std::expected<std::unique_ptr<RunKernel>, RunKernelError>;
  ~RunKernel();

  RunKernel(const RunKernel&) = delete;
  auto operator=(const RunKernel&) -> RunKernel& = delete;
  RunKernel(RunKernel&&) = delete;
  auto operator=(RunKernel&&) -> RunKernel& = delete;

  [[nodiscard]] auto start(RunStart start)
      -> std::expected<void, RunKernelError>;
  [[nodiscard]] auto cancel(const domain::RunId& run_id,
                            const domain::InferenceId& inference_id,
                            std::optional<std::string> reason = std::nullopt)
      -> std::expected<void, RunKernelError>;

  // Drain worker observations and apply their run events on the calling
  // thread. The returned events are exactly those committed by this call.
  [[nodiscard]] auto drain()
      -> std::expected<std::vector<domain::RunEvent>, RunKernelError>;

  [[nodiscard]] auto event_log() const noexcept
      -> const domain::SessionEventLog&;
  [[nodiscard]] auto projection(const domain::RunId& run_id) const noexcept
      -> const domain::RunProjection*;
  [[nodiscard]] auto active_run_id() const noexcept
      -> std::optional<domain::RunId>;
  [[nodiscard]] auto active_inference_id() const noexcept
      -> std::optional<domain::InferenceId>;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace aiforge::runtime
