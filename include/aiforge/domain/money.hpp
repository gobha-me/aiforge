#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aiforge::domain {

enum class MoneyErrorCode {
  invalid_amount,
  amount_overflow,
  negative_result,
  invalid_unit,
  duplicate_unit,
  empty_cost,
  too_many_units,
};

struct MoneyError {
  MoneyErrorCode code;
  std::string message;
  auto operator==(const MoneyError&) const -> bool = default;
};

class DecimalAmount final {
 public:
  [[nodiscard]] static auto from(std::string_view text)
      -> std::expected<DecimalAmount, MoneyError>;

  [[nodiscard]] auto coefficient() const noexcept -> std::uint64_t {
    return m_coefficient;
  }
  [[nodiscard]] auto scale() const noexcept -> std::uint8_t { return m_scale; }
  [[nodiscard]] auto to_string() const -> std::string;

  auto operator==(const DecimalAmount&) const -> bool = default;

 private:
  DecimalAmount(std::uint64_t coefficient, std::uint8_t scale)
      : m_coefficient(coefficient), m_scale(scale) {}

  std::uint64_t m_coefficient{};
  std::uint8_t m_scale{};

  friend auto add(const DecimalAmount& left, const DecimalAmount& right)
      -> std::expected<DecimalAmount, MoneyError>;
  friend auto subtract(const DecimalAmount& left, const DecimalAmount& right)
      -> std::expected<DecimalAmount, MoneyError>;
};

[[nodiscard]] auto add(const DecimalAmount& left, const DecimalAmount& right)
    -> std::expected<DecimalAmount, MoneyError>;
[[nodiscard]] auto subtract(const DecimalAmount& left,
                            const DecimalAmount& right)
    -> std::expected<DecimalAmount, MoneyError>;
[[nodiscard]] auto compare(const DecimalAmount& left,
                           const DecimalAmount& right) -> std::strong_ordering;

class SessionSpendCeiling final {
 public:
  [[nodiscard]] static auto from(std::string_view text)
      -> std::expected<SessionSpendCeiling, MoneyError>;
  [[nodiscard]] static auto create(DecimalAmount amount)
      -> std::expected<SessionSpendCeiling, MoneyError>;

  [[nodiscard]] auto amount() const noexcept -> const DecimalAmount& {
    return m_amount;
  }

  auto operator==(const SessionSpendCeiling&) const -> bool = default;

 private:
  explicit SessionSpendCeiling(DecimalAmount amount) : m_amount(amount) {}

  DecimalAmount m_amount;
};

class MonetaryAmount final {
 public:
  [[nodiscard]] static auto create(std::string unit, DecimalAmount amount)
      -> std::expected<MonetaryAmount, MoneyError>;

  [[nodiscard]] auto unit() const noexcept -> std::string_view {
    return m_unit;
  }
  [[nodiscard]] auto amount() const noexcept -> const DecimalAmount& {
    return m_amount;
  }

  auto operator==(const MonetaryAmount&) const -> bool = default;

 private:
  MonetaryAmount(std::string unit, DecimalAmount amount)
      : m_unit(std::move(unit)), m_amount(amount) {}

  std::string m_unit;
  DecimalAmount m_amount;
};

class ReportedCost final {
 public:
  [[nodiscard]] static auto create(std::vector<MonetaryAmount> amounts)
      -> std::expected<ReportedCost, MoneyError>;

  [[nodiscard]] auto amounts() const noexcept
      -> const std::vector<MonetaryAmount>& {
    return m_amounts;
  }

  auto operator==(const ReportedCost&) const -> bool = default;

 private:
  explicit ReportedCost(std::vector<MonetaryAmount> amounts)
      : m_amounts(std::move(amounts)) {}

  std::vector<MonetaryAmount> m_amounts;
};

[[nodiscard]] auto add(const ReportedCost& left, const ReportedCost& right)
    -> std::expected<ReportedCost, MoneyError>;

} // namespace aiforge::domain
