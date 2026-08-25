#include <aiforge/adapters/filesystem_persona_source.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aiforge::adapters {
namespace {

using persona::PersonaErrorCode;

[[nodiscard]] auto failure(PersonaErrorCode code, std::string message,
                           std::optional<std::string> name = std::nullopt,
                           bool retryable = false)
    -> std::unexpected<persona::PersonaError> {
  return std::unexpected(persona::PersonaError{
      code, std::move(message), std::move(name), retryable});
}

[[nodiscard]] auto valid_limits(const persona::PersonaLimits& limits) -> bool {
  return limits.maximum_personas != 0 && limits.maximum_name_bytes != 0 &&
         limits.maximum_file_bytes != 0 &&
         limits.maximum_description_bytes != 0;
}

[[nodiscard]] auto valid_name(const std::string_view value,
                              const std::size_t maximum) -> bool {
  if (value.empty() || value.size() > maximum) return false;
  const auto is_ascii_alnum = [](const unsigned char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
  };
  if (!is_ascii_alnum(static_cast<unsigned char>(value.front()))) return false;
  return std::ranges::all_of(value.substr(1), [](const unsigned char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') || character == '-' ||
           character == '_';
  });
}

[[nodiscard]] auto canonical_name(std::string_view value) -> std::string {
  std::string result{value};
  std::ranges::transform(result, result.begin(), [](const unsigned char ch) {
    return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
  });
  return result;
}

[[nodiscard]] auto valid_utf8_text(const std::string_view value) -> bool {
  if (value.empty()) return false;
  std::size_t index{};
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first == 0 || first == 0x7fU ||
        (first < 0x20U && first != '\n' && first != '\r' && first != '\t')) {
      return false;
    }
    if (first <= 0x7fU) {
      ++index;
      continue;
    }
    std::size_t length{};
    std::uint32_t codepoint{};
    if (first >= 0xc2U && first <= 0xdfU) {
      length = 2;
      codepoint = first & 0x1fU;
    } else if (first >= 0xe0U && first <= 0xefU) {
      length = 3;
      codepoint = first & 0x0fU;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      length = 4;
      codepoint = first & 0x07U;
    } else {
      return false;
    }
    if (length > value.size() - index) return false;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xc0U) != 0x80U) return false;
      codepoint = (codepoint << 6U) | (next & 0x3fU);
    }
    if ((length == 3 && codepoint < 0x800U) ||
        (length == 4 && codepoint < 0x10000U) ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
        codepoint > 0x10ffffU) {
      return false;
    }
    index += length;
  }
  return true;
}

class Sha256 final {
 public:
  auto update(const std::string_view bytes) -> void {
    for (const unsigned char byte : bytes) {
      m_block[m_block_size++] = byte;
      ++m_byte_count;
      if (m_block_size == m_block.size()) transform();
    }
  }

  [[nodiscard]] auto finish() -> std::string {
    const auto bit_count = m_byte_count * 8U;
    m_block[m_block_size++] = 0x80U;
    if (m_block_size > 56U) {
      while (m_block_size < m_block.size()) m_block[m_block_size++] = 0;
      transform();
    }
    while (m_block_size < 56U) m_block[m_block_size++] = 0;
    for (int shift = 56; shift >= 0; shift -= 8) {
      m_block[m_block_size++] =
          static_cast<std::uint8_t>(bit_count >> static_cast<unsigned>(shift));
    }
    transform();
    std::string output;
    output.reserve(64);
    for (const auto word : m_state) output += std::format("{:08x}", word);
    return output;
  }

 private:
  static constexpr std::array<std::uint32_t, 64> constants{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
      0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
      0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
      0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
      0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
      0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
      0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
      0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
      0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
      0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  auto transform() -> void {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index{}; index < 16; ++index) {
      const auto offset = index * 4U;
      words[index] = (static_cast<std::uint32_t>(m_block[offset]) << 24U) |
                     (static_cast<std::uint32_t>(m_block[offset + 1]) << 16U) |
                     (static_cast<std::uint32_t>(m_block[offset + 2]) << 8U) |
                     static_cast<std::uint32_t>(m_block[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const auto s0 = std::rotr(words[index - 15], 7) ^
                      std::rotr(words[index - 15], 18) ^
                      (words[index - 15] >> 3U);
      const auto s1 = std::rotr(words[index - 2], 17) ^
                      std::rotr(words[index - 2], 19) ^
                      (words[index - 2] >> 10U);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    auto a = m_state[0];
    auto b = m_state[1];
    auto c = m_state[2];
    auto d = m_state[3];
    auto e = m_state[4];
    auto f = m_state[5];
    auto g = m_state[6];
    auto h = m_state[7];
    for (std::size_t index{}; index < words.size(); ++index) {
      const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto choice = (e & f) ^ (~e & g);
      const auto temporary1 = h + sum1 + choice + constants[index] + words[index];
      const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
    m_block_size = 0;
  }

  std::array<std::uint32_t, 8> m_state{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> m_block{};
  std::size_t m_block_size{};
  std::uint64_t m_byte_count{};
};

struct IndexedPersona {
  std::string canonical;
  std::string name;
  std::string filename;
};

[[nodiscard]] auto root_status(const std::filesystem::path& root,
                               const bool missing_is_empty)
    -> std::expected<bool, persona::PersonaError> {
  if (!root.is_absolute()) {
    return failure(PersonaErrorCode::invalid_root,
                   "persona root must be absolute");
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(root, error);
  if (error == std::errc::no_such_file_or_directory) {
    if (missing_is_empty) return false;
    return failure(PersonaErrorCode::not_found,
                   "persona directory does not exist");
  }
  if (error) {
    return failure(error == std::errc::permission_denied
                       ? PersonaErrorCode::permission_denied
                       : PersonaErrorCode::io_failure,
                   "persona directory could not be inspected");
  }
  if (std::filesystem::is_symlink(status)) {
    return failure(PersonaErrorCode::path_escape,
                   "persona directory cannot be a symbolic link");
  }
  if (!std::filesystem::is_directory(status)) {
    return failure(PersonaErrorCode::invalid_root,
                   "persona root is not a directory");
  }
  return true;
}

[[nodiscard]] auto index_personas(const std::filesystem::path& root,
                                  const persona::PersonaLimits& limits,
                                  const std::stop_token stop_token,
                                  const bool missing_is_empty)
    -> std::expected<std::vector<IndexedPersona>, persona::PersonaError> {
  auto present = root_status(root, missing_is_empty);
  if (!present) return std::unexpected(std::move(present.error()));
  if (!*present) return std::vector<IndexedPersona>{};

  std::map<std::string, IndexedPersona> indexed;
  std::error_code error;
  for (std::filesystem::directory_iterator iterator{root, error}, end;
       iterator != end; iterator.increment(error)) {
    if (stop_token.stop_requested()) {
      return failure(PersonaErrorCode::cancelled, "persona listing cancelled");
    }
    if (error) {
      return failure(error == std::errc::permission_denied
                         ? PersonaErrorCode::permission_denied
                         : PersonaErrorCode::io_failure,
                     "persona directory could not be listed", std::nullopt,
                     true);
    }
    const auto extension = iterator->path().extension().string();
    if (extension != ".md" && extension != ".txt") continue;
    const auto name = iterator->path().stem().string();
    if (!valid_name(name, limits.maximum_name_bytes)) {
      return failure(PersonaErrorCode::invalid_name,
                     "persona directory contains an invalid persona name",
                     name);
    }
    const auto entry_status = iterator->symlink_status(error);
    if (error) {
      return failure(PersonaErrorCode::io_failure,
                     "persona entry could not be inspected", name, true);
    }
    if (std::filesystem::is_symlink(entry_status)) {
      return failure(PersonaErrorCode::path_escape,
                     "persona entry cannot be a symbolic link", name);
    }
    if (!std::filesystem::is_regular_file(entry_status)) {
      return failure(PersonaErrorCode::unsupported_entry,
                     "persona entry must be a regular file", name);
    }
    auto canonical = canonical_name(name);
    IndexedPersona entry{canonical, name, iterator->path().filename().string()};
    if (!indexed.emplace(canonical, std::move(entry)).second) {
      return failure(PersonaErrorCode::ambiguous_name,
                     "persona name has a case or extension alias", name);
    }
    if (indexed.size() > limits.maximum_personas) {
      return failure(PersonaErrorCode::resource_exhausted,
                     "persona listing exceeds its entry limit");
    }
  }
  if (error) {
    return failure(PersonaErrorCode::io_failure,
                   "persona directory could not be listed", std::nullopt,
                   true);
  }
  std::vector<IndexedPersona> result;
  result.reserve(indexed.size());
  for (auto& [key, entry] : indexed) {
    static_cast<void>(key);
    result.push_back(std::move(entry));
  }
  return result;
}

#ifndef _WIN32
class UniqueFd final {
 public:
  explicit UniqueFd(const int value = -1) : m_value(value) {}
  UniqueFd(const UniqueFd&) = delete;
  auto operator=(const UniqueFd&) -> UniqueFd& = delete;
  UniqueFd(UniqueFd&& other) noexcept
      : m_value(std::exchange(other.m_value, -1)) {}
  ~UniqueFd() {
    if (m_value >= 0) static_cast<void>(::close(m_value));
  }
  [[nodiscard]] auto get() const noexcept -> int { return m_value; }

 private:
  int m_value;
};

[[nodiscard]] auto same_file(const struct stat& left,
                             const struct stat& right) -> bool {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
         left.st_size == right.st_size &&
         left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
         left.st_mtim.tv_nsec == right.st_mtim.tv_nsec;
}
#endif

[[nodiscard]] auto bounded_description(const std::string_view text,
                                       const std::size_t maximum)
    -> std::string {
  for (const auto raw : text | std::views::split('\n')) {
    std::string_view line{raw.begin(), raw.end()};
    while (!line.empty() &&
           (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) {
      line.remove_prefix(1);
    }
    while (!line.empty() &&
           (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
      line.remove_suffix(1);
    }
    if (line.empty()) continue;
    std::size_t bytes{};
    while (bytes < line.size() && bytes < maximum) {
      const auto lead = static_cast<unsigned char>(line[bytes]);
      const std::size_t length = lead < 0x80U ? 1U : lead < 0xe0U ? 2U :
                                 lead < 0xf0U ? 3U : 4U;
      if (length > maximum - bytes) break;
      bytes += length;
    }
    return std::string{line.substr(0, bytes)};
  }
  return {};
}

[[nodiscard]] auto load_indexed(const std::filesystem::path& root,
                                const IndexedPersona& entry,
                                const persona::PersonaLimits& limits,
                                const std::stop_token stop_token)
    -> std::expected<domain::PersonaDocument, persona::PersonaError> {
#ifdef _WIN32
  static_cast<void>(root);
  static_cast<void>(entry);
  static_cast<void>(limits);
  static_cast<void>(stop_token);
  return failure(PersonaErrorCode::io_failure,
                 "persona loading is unavailable on this platform");
#else
  UniqueFd directory{
      ::open(root.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
  if (directory.get() < 0) {
    return failure(errno == EACCES ? PersonaErrorCode::permission_denied
                                   : PersonaErrorCode::io_failure,
                   "persona directory could not be opened", entry.name, true);
  }
  UniqueFd descriptor{::openat(directory.get(), entry.filename.c_str(),
                               O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (descriptor.get() < 0) {
    if (errno == ELOOP) {
      return failure(PersonaErrorCode::path_escape,
                     "persona entry cannot be a symbolic link", entry.name);
    }
    return failure(errno == ENOENT ? PersonaErrorCode::not_found
                   : errno == EACCES ? PersonaErrorCode::permission_denied
                                     : PersonaErrorCode::io_failure,
                   "persona file could not be opened", entry.name, true);
  }
  struct stat before {};
  if (::fstat(descriptor.get(), &before) != 0 || !S_ISREG(before.st_mode)) {
    return failure(PersonaErrorCode::unsupported_entry,
                   "persona entry must be a regular file", entry.name);
  }
  if (before.st_size <= 0 ||
      static_cast<std::uint64_t>(before.st_size) > limits.maximum_file_bytes) {
    return failure(before.st_size <= 0 ? PersonaErrorCode::malformed_text
                                      : PersonaErrorCode::resource_exhausted,
                   before.st_size <= 0 ? "persona file is empty"
                                       : "persona file exceeds its byte limit",
                   entry.name);
  }
  std::string text;
  text.reserve(static_cast<std::size_t>(before.st_size));
  std::array<char, 8192> buffer{};
  for (;;) {
    if (stop_token.stop_requested()) {
      return failure(PersonaErrorCode::cancelled, "persona loading cancelled",
                     entry.name);
    }
    const auto count = ::read(descriptor.get(), buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      return failure(PersonaErrorCode::io_failure,
                     "persona file could not be read", entry.name, true);
    }
    if (static_cast<std::size_t>(count) >
        limits.maximum_file_bytes - text.size()) {
      return failure(PersonaErrorCode::resource_exhausted,
                     "persona file exceeds its byte limit", entry.name);
    }
    text.append(buffer.data(), static_cast<std::size_t>(count));
  }
  struct stat after {};
  if (::fstat(descriptor.get(), &after) != 0) {
    return failure(PersonaErrorCode::io_failure,
                   "persona file could not be verified", entry.name, true);
  }
  if (!same_file(before, after) ||
      static_cast<std::uint64_t>(after.st_size) != text.size()) {
    return failure(PersonaErrorCode::unstable,
                   "persona file changed while it was being read", entry.name,
                   true);
  }
  if (!valid_utf8_text(text)) {
    return failure(PersonaErrorCode::malformed_text,
                   "persona file must be nonempty UTF-8 text without unsafe controls",
                   entry.name);
  }
  Sha256 digest;
  digest.update(text);
  auto persona_id =
      domain::PersonaId::from("persona:" + entry.canonical);
  if (!persona_id) {
    return failure(PersonaErrorCode::internal_failure,
                   "persona identity could not be represented", entry.name);
  }
  return domain::PersonaDocument{
      {std::move(*persona_id), entry.name,
       "personas/" + entry.filename,
       {"sha256", digest.finish(), text.size()}},
      std::move(text)};
#endif
}

}  // namespace

auto resolve_persona_root(const config::ConfigPathEnvironment& environment)
    -> std::expected<std::filesystem::path, persona::PersonaError> {
  auto config_path = config::resolve_config_path(environment);
  if (!config_path) {
    return failure(config_path.error().code == config::ConfigFileErrorCode::missing_home
                       ? PersonaErrorCode::missing_home
                       : PersonaErrorCode::invalid_root,
                   "persona root could not be resolved");
  }
  return config_path->parent_path() / "personas";
}

auto process_persona_root()
    -> std::expected<std::filesystem::path, persona::PersonaError> {
  try {
    config::ConfigPathEnvironment environment;
    if (const auto* xdg = std::getenv("XDG_CONFIG_HOME")) {
      environment.xdg_config_home = std::filesystem::path{xdg};
    }
    if (const auto* home = std::getenv("HOME")) {
      environment.home = std::filesystem::path{home};
    }
    return resolve_persona_root(environment);
  } catch (...) {
    return failure(PersonaErrorCode::invalid_root,
                   "persona root could not be resolved");
  }
}

auto FilesystemPersonaSource::list(const persona::PersonaLimits limits,
                                   const std::stop_token stop_token)
    -> std::expected<std::vector<domain::PersonaSummary>,
                     persona::PersonaError> {
  try {
    if (!valid_limits(limits)) {
      return failure(PersonaErrorCode::invalid_request,
                     "persona limits are invalid");
    }
    auto indexed = index_personas(m_root, limits, stop_token, true);
    if (!indexed) return std::unexpected(std::move(indexed.error()));
    std::vector<domain::PersonaSummary> result;
    result.reserve(indexed->size());
    for (const auto& entry : *indexed) {
      auto document = load_indexed(m_root, entry, limits, stop_token);
      if (!document) return std::unexpected(std::move(document.error()));
      result.push_back({document->reference,
                        bounded_description(document->text,
                                            limits.maximum_description_bytes)});
    }
    return result;
  } catch (...) {
    return failure(PersonaErrorCode::internal_failure,
                   "persona listing failed internally");
  }
}

auto FilesystemPersonaSource::load(std::string name,
                                   const persona::PersonaLimits limits,
                                   const std::stop_token stop_token)
    -> std::expected<domain::PersonaDocument, persona::PersonaError> {
  try {
    if (!valid_limits(limits)) {
      return failure(PersonaErrorCode::invalid_request,
                     "persona limits are invalid", std::move(name));
    }
    if (!valid_name(name, limits.maximum_name_bytes)) {
      return failure(PersonaErrorCode::invalid_name,
                     "persona name must be a bounded bare name", std::move(name));
    }
    auto indexed = index_personas(m_root, limits, stop_token, false);
    if (!indexed) return std::unexpected(std::move(indexed.error()));
    const auto canonical = canonical_name(name);
    const auto found = std::ranges::find(*indexed, canonical,
                                         &IndexedPersona::canonical);
    if (found == indexed->end()) {
      return failure(PersonaErrorCode::not_found, "persona was not found",
                     std::move(name));
    }
    return load_indexed(m_root, *found, limits, stop_token);
  } catch (...) {
    return failure(PersonaErrorCode::internal_failure,
                   "persona loading failed internally", std::move(name));
  }
}

}  // namespace aiforge::adapters
