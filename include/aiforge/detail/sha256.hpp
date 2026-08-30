#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>

namespace aiforge::detail {

class Sha256 final {
 public:
  auto update(const std::span<const std::byte> bytes) -> void {
    for (const auto value : bytes) {
      m_block[m_block_size++] = std::to_integer<std::uint8_t>(value);
      ++m_byte_count;
      if (m_block_size == m_block.size()) transform();
    }
  }

  [[nodiscard]] auto finish() -> std::string {
    const auto bit_count = m_byte_count * 8U;
    m_block[m_block_size++] = 0x80U;
    if (m_block_size > 56U) {
      while (m_block_size < m_block.size())
        m_block[m_block_size++] = 0;
      transform();
    }
    while (m_block_size < 56U)
      m_block[m_block_size++] = 0;
    for (int shift = 56; shift >= 0; shift -= 8) {
      m_block[m_block_size++] =
          static_cast<std::uint8_t>(bit_count >> static_cast<unsigned>(shift));
    }
    transform();
    std::string output;
    output.reserve(64);
    for (const auto word : m_state)
      output += std::format("{:08x}", word);
    return output;
  }

 private:
  static constexpr std::array<std::uint32_t, 64> constants{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
      0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
      0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
      0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
      0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
      0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
      0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
      0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
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
    auto [a, b, c, d, e, f, g, h] = m_state;
    for (std::size_t index{}; index < words.size(); ++index) {
      const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto choice = (e & f) ^ (~e & g);
      const auto t1 = h + sum1 + choice + constants[index] + words[index];
      const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto t2 = sum0 + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
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

  std::array<std::uint32_t, 8> m_state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                       0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                       0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> m_block{};
  std::size_t m_block_size{};
  std::uint64_t m_byte_count{};
};

} // namespace aiforge::detail
