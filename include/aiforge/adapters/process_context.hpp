#pragma once

#include <aiforge/adapters/agent_transport.hpp>
#include <aiforge/cli/command_registry.hpp>
#include <aiforge/surfaces/chat_session.hpp>

namespace aiforge::adapters {
// Drives only producers started by this invocation; existing unfinished runs
// must be resolved before entering the loop. The identity is process-local.
[[nodiscard]] auto run_context_jsonl(surfaces::ChatSession& session,
                                     AgentTransport& transport,
                                     std::stop_token stop,
                                     std::string instance_identity)
    -> std::expected<void, cli::CommandFailure>;
class ProcessContextCommand final : public cli::ContextCommand {
 public:
  [[nodiscard]] auto execute(Request request,
                             cli::CommandEnvironment& environment,
                             std::ostream& output, std::ostream& diagnostics)
      -> std::expected<void, cli::CommandFailure> override;
};
} // namespace aiforge::adapters
