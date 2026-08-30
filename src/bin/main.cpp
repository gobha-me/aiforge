#include <aiforge/cli/command_registry.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

#ifdef AIFORGE_HAS_ADAPTERS
#include <aiforge/adapters/process_image.hpp>
#include <aiforge/adapters/process_interactive.hpp>
#include <aiforge/adapters/process_login.hpp>
#include <aiforge/adapters/process_models.hpp>
#include <aiforge/adapters/process_one_shot.hpp>
#include <aiforge/adapters/process_plan.hpp>
#endif

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

std::atomic<std::sig_atomic_t> interrupted{};
static_assert(decltype(interrupted)::is_always_lock_free);

extern "C" auto handle_interrupt(int) -> void {
  interrupted.store(1, std::memory_order_relaxed);
}

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
  if (argc > 1) {
    arguments.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }
  }

  std::signal(SIGINT, handle_interrupt);
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif
  std::stop_source cancellation;
  std::jthread signal_watcher{[&](const std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      if (interrupted.load(std::memory_order_relaxed) != 0) {
        cancellation.request_stop();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
  }};

#ifdef AIFORGE_HAS_ADAPTERS
  aiforge::adapters::ProcessOneShotCommand one_shot;
  aiforge::adapters::ProcessInteractiveCommand interactive;
  aiforge::adapters::ProcessImageCommand image;
  aiforge::adapters::ProcessLoginCommand login;
  aiforge::adapters::ProcessModelsCommand models;
  aiforge::adapters::ProcessPlanCommand plan;
  aiforge::cli::OneShotCommand* one_shot_service = &one_shot;
  aiforge::cli::InteractiveCommand* interactive_service = &interactive;
  aiforge::cli::ModelsCommand* models_service = &models;
  aiforge::cli::LoginCommand* login_service = &login;
  aiforge::cli::PlanCommand* plan_service = &plan;
  aiforge::cli::ImageCommand* image_service = &image;
#else
  aiforge::cli::OneShotCommand* one_shot_service = nullptr;
  aiforge::cli::InteractiveCommand* interactive_service = nullptr;
  aiforge::cli::ModelsCommand* models_service = nullptr;
  aiforge::cli::LoginCommand* login_service = nullptr;
  aiforge::cli::PlanCommand* plan_service = nullptr;
  aiforge::cli::ImageCommand* image_service = nullptr;
#endif
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
                                               one_shot_service,
                                               interactive_service,
                                               models_service,
                                               login_service,
#ifdef _WIN32
                                               -1,
                                               plan_service,
                                               image_service,
                                               -1};
#else
                                               STDIN_FILENO,
                                               plan_service,
                                               image_service,
                                               STDOUT_FILENO};
#endif
  const auto result =
      aiforge::cli::run_cli(arguments, environment, std::cout, std::cerr);
  signal_watcher.request_stop();
  return result;
}
