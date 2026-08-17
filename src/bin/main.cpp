#include <iostream>
#include <string_view>
#include <vector>

#include <aiforge/cli/command_registry.hpp>

auto main(const int argc, char* argv[]) -> int {
  std::vector<std::string_view> arguments;
  if (argc > 1) {
    arguments.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }
  }
  return aiforge::cli::run_cli(arguments, std::cout, std::cerr);
}
