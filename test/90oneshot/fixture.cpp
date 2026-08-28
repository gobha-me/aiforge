#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <aiforge/cli/command_registry.hpp>
#include <aiforge/surfaces/one_shot.hpp>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using namespace aiforge;

std::atomic<std::sig_atomic_t> interrupted{};
static_assert(decltype(interrupted)::is_always_lock_free);

extern "C" auto handle_interrupt(int) -> void {
  interrupted.store(1, std::memory_order_relaxed);
}

template <typename IdType> auto make_id(const std::string& value) -> IdType {
  return IdType::from(value).value();
}

struct End {};
using Item = std::variant<backend::BackendEvent, backend::BackendError, End>;

class VectorStream final : public backend::BackendStream {
 public:
  explicit VectorStream(std::vector<Item> items) : m_items(std::move(items)) {}

  auto next(std::stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_index >= m_items.size())
      return std::optional<backend::BackendEvent>{};
    auto& item = m_items[m_index++];
    if (auto* event = std::get_if<backend::BackendEvent>(&item)) {
      return std::optional<backend::BackendEvent>{std::move(*event)};
    }
    if (auto* error = std::get_if<backend::BackendError>(&item)) {
      return std::unexpected(std::move(*error));
    }
    return std::optional<backend::BackendEvent>{};
  }

 private:
  std::vector<Item> m_items;
  std::size_t m_index{};
};

class CancelStream final : public backend::BackendStream {
 public:
  explicit CancelStream(domain::MessageId message_id)
      : m_message_id(std::move(message_id)) {}

  auto next(std::stop_token stop_token)
      -> std::expected<std::optional<backend::BackendEvent>,
                       backend::BackendError> override {
    if (m_finished) return std::optional<backend::BackendEvent>{};
    if (m_step++ == 0) {
      return backend::BackendEvent{backend::ResponseStarted{"response"}};
    }
    if (m_step == 2) {
      return backend::BackendEvent{
          backend::ContentDelta{m_message_id, domain::TextBlock{"partial"}}};
    }
    std::mutex mutex;
    std::unique_lock lock(mutex);
    std::condition_variable_any ready;
    ready.wait(lock, stop_token, [] { return false; });
    m_finished = true;
    return backend::BackendEvent{backend::ResponseCancelled{"interrupt"}};
  }

 private:
  domain::MessageId m_message_id;
  int m_step{};
  bool m_finished{};
};

[[nodiscard]] auto prompt_text(const backend::BackendRequest& request)
    -> std::string_view {
  for (const auto& entry : request.context.entries) {
    if (entry.message.role != domain::Role::user) continue;
    for (const auto& block : entry.message.content) {
      if (const auto* text = std::get_if<domain::TextBlock>(&block)) {
        return text->text;
      }
    }
  }
  return {};
}

class FixtureBackend final : public backend::Backend,
                             public backend::ModelContextProvider {
 public:
  auto lookup(const domain::ModelId& model_id, std::stop_token)
      -> std::expected<backend::ModelContextInfo,
                       backend::BackendError> override {
    return backend::ModelContextInfo{model_id, 2U * 1024U * 1024U, 1024};
  }

  auto start(backend::BackendRequest request, std::stop_token)
      -> std::expected<std::unique_ptr<backend::BackendStream>,
                       backend::BackendError> override {
    const auto prompt = prompt_text(request);
    if (prompt == "cancel") {
      return std::make_unique<CancelStream>(request.assistant_message_id);
    }
    const auto text = prompt == "flood" ? std::string(512U * 1024U, 'x')
                                        : std::string{"answer"};
    return std::make_unique<VectorStream>(std::vector<Item>{
        backend::BackendEvent{backend::ResponseStarted{"response"}},
        backend::BackendEvent{backend::ContentDelta{
            request.assistant_message_id, domain::TextBlock{text}}},
        backend::BackendEvent{
            backend::CitationObserved{{"https://example.test", "fixture"}}},
        backend::BackendEvent{backend::UsageObserved{{2, 1, 0, 0}}},
        backend::BackendEvent{
            backend::ResponseFinished{domain::FinishReason::stop}},
        End{}});
  }
};

class FixtureCommand final : public cli::OneShotCommand {
 public:
  auto execute(Request request, cli::CommandEnvironment& environment,
               std::ostream& output, std::ostream& error)
      -> std::expected<void, cli::CommandFailure> override {
    std::optional<std::string> evidence;
    if (!environment.input_is_terminal) {
      std::string value{std::istreambuf_iterator<char>{environment.input}, {}};
      if (!value.empty()) evidence = std::move(value);
    }
    FixtureBackend backend;
    surfaces::OneShotSurface surface{backend, backend};
    auto result =
        surface.run({std::string{request.prompt}, std::move(evidence),
                     make_id<domain::ModelId>("fixture-model"),
                     surfaces::OneShotRequest::SessionMode::ephemeral},
                    output, error, environment.stop_token);
    if (result) return {};
    switch (result.error().code) {
      case surfaces::OneShotErrorCode::invalid_input:
      case surfaces::OneShotErrorCode::input_too_large:
        return std::unexpected(cli::CommandFailure{
            cli::CommandFailureKind::usage, result.error().message});
      case surfaces::OneShotErrorCode::cancelled:
        return std::unexpected(cli::CommandFailure{
            cli::CommandFailureKind::cancelled, result.error().message});
      default:
        return std::unexpected(cli::CommandFailure{
            cli::CommandFailureKind::runtime, result.error().message});
    }
  }
};

[[nodiscard]] auto terminal(const int descriptor) -> bool {
#ifdef _WIN32
  static_cast<void>(descriptor);
  return true;
#else
  return ::isatty(descriptor) != 0;
#endif
}

} // namespace

auto main(const int argc, char* argv[]) -> int {
  std::vector<std::string_view> arguments;
  for (int index = 1; index < argc; ++index)
    arguments.emplace_back(argv[index]);
  std::signal(SIGINT, handle_interrupt);
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif
  std::stop_source cancellation;
  std::jthread watcher{[&](const std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      if (interrupted.load(std::memory_order_relaxed) != 0) {
        cancellation.request_stop();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
  }};
  FixtureCommand command;
  aiforge::cli::CommandEnvironment environment{std::cin,
#ifdef _WIN32
                                               true,
                                               true,
                                               true,
#else
                                               terminal(STDIN_FILENO),
                                               terminal(STDOUT_FILENO),
                                               terminal(STDERR_FILENO),
#endif
                                               cancellation.get_token(),
                                               &command};
  const auto result =
      aiforge::cli::run_cli(arguments, environment, std::cout, std::cerr);
  watcher.request_stop();
  return result;
}
