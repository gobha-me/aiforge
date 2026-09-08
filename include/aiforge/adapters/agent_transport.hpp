#pragma once

#include <chrono>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>

#include <aiforge/surfaces/agent.hpp>

namespace aiforge::adapters {
enum class TransportLineState { idle, line, end };
struct TransportLine {
  TransportLineState state{TransportLineState::idle};
  std::string text;
};

// Borrows descriptors. The caller owns SIGPIPE handling and must keep the
// descriptors open, with no concurrent flag mutations, until destruction.
class AgentTransport final : public surfaces::AgentRecordSink {
 public:
  [[nodiscard]] static auto open(
      int input_descriptor, int output_descriptor, std::stop_token stop_token,
      std::chrono::milliseconds output_timeout = std::chrono::seconds{2})
      -> std::expected<std::unique_ptr<AgentTransport>, surfaces::AgentError>;
  ~AgentTransport() override;
  AgentTransport(const AgentTransport&) = delete;
  auto operator=(const AgentTransport&) -> AgentTransport& = delete;

  [[nodiscard]] auto read_request()
      -> std::expected<std::string, surfaces::AgentError>;
  // Additive JSONL mode: consumes at most one bounded chunk and waits at most
  // 20ms. Idle is distinct from EOF. Cannot be mixed with read_request().
  [[nodiscard]] auto poll_line()
      -> std::expected<TransportLine, surfaces::AgentError>;
  [[nodiscard]] auto write_record(std::string_view record)
      -> std::expected<void, surfaces::AgentError> override;

 private:
  struct Impl;
  explicit AgentTransport(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

} // namespace aiforge::adapters
