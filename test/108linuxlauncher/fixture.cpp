#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace {

[[nodiscard]] auto environment(const char* name) -> std::string_view {
  const auto* value = std::getenv(name);
  return value == nullptr ? std::string_view{"<unset>"}
                          : std::string_view{value};
}

} // namespace

auto main(const int argc, char* argv[]) -> int {
  if (argc < 2) return 64;
  const std::string_view mode{argv[1]};
  if (mode == "inspect") {
    std::cout << "cwd=" << std::filesystem::current_path().generic_string()
              << '\n';
    std::cout << "safe=" << environment("SAFE_VALUE") << '\n';
    std::cout << "unlisted=" << environment("UNLISTED_VALUE") << '\n';
    for (int index = 2; index < argc; ++index) {
      std::cout << "arg" << index - 1 << '=' << argv[index] << '\n';
    }
    char input{};
    std::cout << "stdin=" << (std::cin.get(input) ? "data" : "eof") << '\n';
    std::cerr << "stderr=separate\n";
    return 7;
  }
  if (mode == "emit") {
    if (argc != 4) return 64;
    const auto count = static_cast<std::size_t>(std::stoull(argv[2]));
    const std::string output(count, argv[3][0]);
    std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
    return 0;
  }
  if (mode == "signal") {
    std::raise(SIGTERM);
    return 1;
  }
  if (mode == "descriptor") {
    if (argc != 3) return 64;
    errno = 0;
    const auto flags = ::fcntl(std::stoi(argv[2]), F_GETFD);
    std::cout << "descriptor="
              << (flags < 0 && errno == EBADF ? "closed" : "open") << '\n';
    return 0;
  }
  if (mode == "hang") {
    for (;;)
      ::pause();
  }
  if (mode == "ignore-term-flood") {
    static_cast<void>(std::signal(SIGTERM, SIG_IGN));
    std::cout << "ready\n" << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
    const std::array<char, 4096> output{};
    for (;;) {
      std::cout.write(output.data(),
                      static_cast<std::streamsize>(output.size()));
      std::cout.flush();
    }
  }
  return 64;
}
