#include <chrono>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

auto main(const int argc, char** argv) -> int {
  if (argc != 2) return 2;
  if (std::string_view{argv[1]} == "complete") {
    std::puts("owned process stdout");
    std::fputs("owned process stderr\n", stderr);
    return 0;
  }
  if (std::string_view{argv[1]} != "wait") return 2;
  const auto child = ::fork();
  if (child < 0) return 3;
  if (child == 0) {
    std::this_thread::sleep_for(std::chrono::seconds{10});
    ::_exit(0);
  }
  {
    std::ofstream marker{"owned-pids"};
    marker << ::getpid() << ' ' << child << '\n';
    if (!marker) return 4;
  }
  std::puts("owned process ready");
  std::fflush(stdout);
  // Even a broken test harness cannot leave an unbounded child running.
  std::this_thread::sleep_for(std::chrono::seconds{10});
  int status{};
  static_cast<void>(::waitpid(child, &status, 0));
  return 0;
}
