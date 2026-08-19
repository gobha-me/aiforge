#pragma once

#include <aiforge/runtime/run_kernel.hpp>
#include <aiforge/surfaces/chat_session.hpp>
#include <expected>
#include <termforge/core/app.hpp>
#include <vector>

namespace aiforge::adapters {

// Adapts the neutral worker wake and cancellation seams to TermForge without
// allowing a worker to touch widgets or projections.
class TermForgeRunBridge final : public runtime::RunWakeSink {
 public:
  explicit TermForgeRunBridge(termforge::App& app) : m_app(app) {}

  auto wake() noexcept -> void override;

  [[nodiscard]] auto handle(const termforge::Event& event,
                            runtime::RunKernel& kernel)
      -> std::expected<std::vector<domain::RunEvent>, runtime::RunKernelError>;
  [[nodiscard]] auto handle(const termforge::Event& event,
                            surfaces::ChatSession& session)
      -> std::expected<std::vector<domain::RunEvent>,
                       surfaces::ChatSessionError>;

 private:
  termforge::App& m_app;
};

}  // namespace aiforge::adapters
