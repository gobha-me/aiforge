#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "runner.hpp"

#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace audio_evidence = aiforge::evaluation::audio_device;

namespace {

struct Arguments {
  std::string source_sha;
  std::filesystem::path output;
};

[[nodiscard]] auto parse_arguments(const int argc, char* argv[])
    -> std::optional<Arguments> {
  if (argc != 5 || argv == nullptr) return std::nullopt;
  Arguments result;
  bool saw_source{};
  bool saw_output{};
  for (int index = 1; index < argc; index += 2) {
    if (argv[index] == nullptr || argv[index + 1] == nullptr)
      return std::nullopt;
    const std::string_view option{argv[index]};
    if (option == "--source-sha" && !saw_source) {
      result.source_sha = argv[index + 1];
      saw_source = true;
    } else if (option == "--output" && !saw_output) {
      result.output = argv[index + 1];
      saw_output = true;
    } else {
      return std::nullopt;
    }
  }
  if (!saw_source || !saw_output || result.source_sha.empty() ||
      result.output.empty())
    return std::nullopt;
  return result;
}

[[nodiscard]] auto own_executable_directory()
    -> std::optional<std::filesystem::path> {
  std::string path(4096, '\0');
  const auto count = ::readlink("/proc/self/exe", path.data(), path.size());
  if (count <= 0 || static_cast<std::size_t>(count) == path.size())
    return std::nullopt;
  path.resize(static_cast<std::size_t>(count));
  return std::filesystem::path{path}.parent_path();
}

[[nodiscard]] auto write_report(const std::filesystem::path& path,
                                const std::string_view document) -> bool {
  const auto descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
             S_IRUSR | S_IWUSR);
  if (descriptor < 0) return false;
  std::size_t offset{};
  bool okay{true};
  while (offset < document.size()) {
    const auto count =
        ::write(descriptor, document.data() + offset, document.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      okay = false;
      break;
    }
    if (count == 0) {
      okay = false;
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  if (okay && ::fsync(descriptor) != 0) okay = false;
  if (::close(descriptor) != 0) okay = false;
  if (!okay) static_cast<void>(::unlink(path.c_str()));
  return okay;
}

} // namespace

auto main(const int argc, char* argv[]) -> int {
  try {
    const auto arguments = parse_arguments(argc, argv);
    const auto executable_directory = own_executable_directory();
    if (!arguments || !executable_directory) return 64;

    audio_evidence::RunnerOptions options;
    options.contract.executable =
        *executable_directory / "aiforge_audio_device_contract_probe";
    options.rtaudio.executable =
        *executable_directory / "aiforge_audio_device_rtaudio_probe";
    options.miniaudio.executable =
        *executable_directory / "aiforge_audio_device_miniaudio_probe";
    const auto report =
        audio_evidence::run_evaluation(arguments->source_sha, options);
    if (!report) return 70;
    const auto document = audio_evidence::serialize_report(*report);
    if (!document || !write_report(arguments->output, *document)) return 70;
    const auto succeeded = audio_evidence::evidence_run_succeeded(*report);
    return succeeded && *succeeded ? 0 : 1;
  } catch (...) {
    return 70;
  }
}
