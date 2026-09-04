#pragma once

#include <cstddef>
#include <expected>
#include <stop_token>
#include <string>

#include <aiforge/audio/pcm.hpp>

namespace aiforge::audio {

struct CaptureRequest {
  Signed16Format format;
  std::size_t frames{};
  auto operator==(const CaptureRequest&) const -> bool = default;
};

enum class CaptureStage { open, start, stream, stop, close };

class CaptureObserver {
 public:
  virtual ~CaptureObserver() = default;
  [[nodiscard]] virtual auto stage_changed(CaptureStage stage) noexcept
      -> bool = 0;
};

struct CaptureStats {
  std::size_t callbacks{};
  std::size_t frames{};
  std::size_t overruns{};
  std::size_t late_callbacks{};
  auto operator==(const CaptureStats&) const -> bool = default;
};

struct CaptureResult {
  Signed16Buffer buffer;
  CaptureStats stats;
  auto operator==(const CaptureResult&) const -> bool = default;
};

enum class CaptureErrorCode {
  invalid_format,
  invalid_request,
  too_large,
  operation_in_progress,
  device_quarantined,
  cancelled,
  unsupported_format,
  permission_denied,
  unavailable,
  device_lost,
  overrun,
  callback_contract_violation,
  incomplete_stream,
  late_callback,
  cleanup_failed,
  internal_failure,
};

struct CaptureError {
  CaptureErrorCode code{CaptureErrorCode::internal_failure};
  CaptureStage stage{CaptureStage::open};
  std::string message;
  CaptureStats stats;
  auto operator==(const CaptureError&) const -> bool = default;
};

class CapturePort {
 public:
  virtual ~CapturePort() = default;

  [[nodiscard]] virtual auto capture(
      CaptureRequest request, std::stop_token stop_token = {},
      CaptureObserver* observer = nullptr) noexcept
      -> std::expected<CaptureResult, CaptureError> = 0;
};

} // namespace aiforge::audio
