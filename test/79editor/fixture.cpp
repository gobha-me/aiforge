#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

auto main(const int argc, char* argv[]) -> int {
  if (argc != 2) return 64;
  const std::filesystem::path path{argv[1]};
  std::ifstream input{path, std::ios::binary};
  const std::string draft{std::istreambuf_iterator<char>{input}, {}};
  input.close();
  if (draft == "hang") {
    for (;;)
      std::this_thread::sleep_for(std::chrono::seconds{1});
  }
  if (draft == "symlink") {
    std::filesystem::remove(path);
    std::filesystem::create_symlink("/dev/null", path);
    return 0;
  }
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output) return 1;
  if (draft == "invalid") {
    output << "bad\x1btext";
  } else if (draft == "oversized") {
    output << std::string(1024, 'x');
  } else if (draft == "permissions") {
    output << "exposed";
    output.close();
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write |
                                           std::filesystem::perms::group_read);
  } else {
    output << "edited\r\nsecond";
  }
  return output ? 0 : 1;
}
