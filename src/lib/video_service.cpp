#include <aiforge/backend/video.hpp>

#include <algorithm>
#include <ranges>

namespace aiforge::backend {

auto TransientVideoUrl::from(std::string value)
    -> std::expected<TransientVideoUrl, std::string> {
  constexpr std::size_t maximum_url_bytes = 8192;
  constexpr std::string_view secure_scheme{"https://"};
  const auto has_secure_scheme = value.starts_with(secure_scheme);
  const auto authority_end =
      has_secure_scheme ? value.find_first_of("/?#", secure_scheme.size())
                        : std::size_t{};
  const auto authority = has_secure_scheme
                             ? std::string_view{value}.substr(
                                   secure_scheme.size(),
                                   authority_end == std::string::npos
                                       ? std::string_view::npos
                                       : authority_end - secure_scheme.size())
                             : std::string_view{};
  if (value.empty() || value.size() > maximum_url_bytes || !has_secure_scheme ||
      authority.empty() || authority.contains('@') ||
      std::ranges::any_of(value, [](const unsigned char character) {
        return character <= 0x20U || character == 0x7FU;
      })) {
    return std::unexpected("transient video URL is invalid");
  }
  return TransientVideoUrl{std::move(value)};
}

auto TransientVideoUrl::value() const noexcept -> std::string_view {
  return m_value;
}

} // namespace aiforge::backend
