#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

auto environment(const char* name) -> std::string_view {
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
    if (argc < 4) return 64;
    const auto count = static_cast<std::size_t>(std::stoull(argv[2]));
    const char byte = argv[3][0];
    const std::string output(count, byte);
    std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
    return 0;
  }
  if (mode == "binary") {
    const char bytes[]{'a', '\0', '\x1b', 'z'};
    std::cout.write(bytes, sizeof(bytes));
    return 0;
  }
  if (mode == "duplex") {
    if (argc < 3) return 64;
    const auto count = static_cast<std::size_t>(std::stoull(argv[2]));
    const std::string output(count, 'o');
    const std::string error(count, 'e');
    std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
    std::cerr.write(error.data(), static_cast<std::streamsize>(error.size()));
    return 0;
  }
  if (mode == "signal") {
    std::raise(SIGTERM);
    return 1;
  }
#ifndef _WIN32
  if (mode == "descriptor") {
    if (argc < 3) return 64;
    errno = 0;
    const auto flags = ::fcntl(std::stoi(argv[2]), F_GETFD);
    std::cout << "descriptor="
              << (flags < 0 && errno == EBADF ? "closed" : "open") << '\n';
    return 0;
  }
  if (mode == "hang") {
    const auto child = ::fork();
    if (child < 0) return errno;
    if (child == 0) {
      const auto marker =
          argc >= 3 ? ::open(argv[2], O_WRONLY | O_CREAT | O_APPEND, 0600) : -1;
      for (;;) {
        if (marker >= 0) static_cast<void>(::write(marker, ".", 1));
        ::usleep(5'000);
      }
    }
    std::cout << "descendant=" << child << '\n' << std::flush;
    for (;;)
      ::pause();
  }
#endif
  return 64;
}
