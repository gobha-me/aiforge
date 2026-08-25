#include <aiforge/domain/money.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace aiforge::domain {
namespace {

constexpr std::uint8_t kMaximumScale = 18;
constexpr std::size_t kMaximumUnits = 16;

[[nodiscard]] auto money_error(const MoneyErrorCode code, std::string message)
    -> MoneyError {
  return {code, std::move(message)};
}

[[nodiscard]] auto checked_multiply(std::uint64_t &value,
                                    const std::uint64_t multiplier) -> bool {
  if (value != 0 &&
      multiplier > std::numeric_limits<std::uint64_t>::max() / value) {
    return false;
  }
  value *= multiplier;
  return true;
}

[[nodiscard]] auto power_of_ten(const std::uint8_t exponent) -> std::uint64_t {
  std::uint64_t result = 1;
  for (std::uint8_t index = 0; index < exponent; ++index)
    result *= 10;
  return result;
}

[[nodiscard]] auto valid_unit(const std::string_view unit) -> bool {
  const auto ascii_alpha = [](const char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z');
  };
  if (unit.empty() || unit.size() > 64 || !ascii_alpha(unit.front())) {
    return false;
  }
  return std::ranges::all_of(unit.substr(1), [&](const char character) {
    return ascii_alpha(character) || (character >= '0' && character <= '9') ||
           character == '.' || character == '_' || character == '-';
  });
}

[[nodiscard]] auto ascii_digit(const char character) -> bool {
  return character >= '0' && character <= '9';
}

} // namespace

auto DecimalAmount::from(const std::string_view text)
    -> std::expected<DecimalAmount, MoneyError> {
  if (text.empty()) {
    return std::unexpected(
        money_error(MoneyErrorCode::invalid_amount, "amount is empty"));
  }
  if (text.size() > 128) {
    return std::unexpected(money_error(MoneyErrorCode::amount_overflow,
                                       "amount text is too large"));
  }

  std::size_t position{};
  std::string digits;
  digits.reserve(text.size());
  std::size_t fraction_digits{};
  bool saw_digit{};

  while (position < text.size() && ascii_digit(text[position])) {
    digits.push_back(text[position++]);
    saw_digit = true;
  }
  if (position < text.size() && text[position] == '.') {
    ++position;
    const auto fraction_start = position;
    while (position < text.size() && ascii_digit(text[position])) {
      digits.push_back(text[position++]);
      saw_digit = true;
    }
    fraction_digits = position - fraction_start;
    if (fraction_digits == 0) {
      return std::unexpected(money_error(MoneyErrorCode::invalid_amount,
                                         "amount fraction is empty"));
    }
  }
  if (!saw_digit) {
    return std::unexpected(
        money_error(MoneyErrorCode::invalid_amount, "amount has no digits"));
  }

  int exponent{};
  if (position < text.size() &&
      (text[position] == 'e' || text[position] == 'E')) {
    ++position;
    bool negative_exponent{};
    if (position < text.size() &&
        (text[position] == '+' || text[position] == '-')) {
      negative_exponent = text[position] == '-';
      ++position;
    }
    const auto exponent_start = position;
    while (position < text.size() && ascii_digit(text[position])) {
      const auto digit = text[position++] - '0';
      if (exponent > 1000) {
        return std::unexpected(money_error(MoneyErrorCode::amount_overflow,
                                           "amount exponent is too large"));
      }
      exponent = exponent * 10 + digit;
    }
    if (position == exponent_start) {
      return std::unexpected(money_error(MoneyErrorCode::invalid_amount,
                                         "amount exponent is empty"));
    }
    if (negative_exponent)
      exponent = -exponent;
  }
  if (position != text.size()) {
    return std::unexpected(money_error(MoneyErrorCode::invalid_amount,
                                       "amount contains invalid characters"));
  }

  const auto first_nonzero = digits.find_first_not_of('0');
  if (first_nonzero == std::string::npos)
    return DecimalAmount{0, 0};
  digits.erase(0, first_nonzero);

  auto scale = static_cast<long long>(fraction_digits) - exponent;
  while (scale > 0 && digits.back() == '0') {
    digits.pop_back();
    --scale;
  }
  if (scale > kMaximumScale) {
    return std::unexpected(
        money_error(MoneyErrorCode::amount_overflow,
                    "amount has too many fractional digits"));
  }

  std::uint64_t coefficient{};
  for (const auto character : digits) {
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (coefficient >
        (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return std::unexpected(money_error(MoneyErrorCode::amount_overflow,
                                         "amount coefficient overflows"));
    }
    coefficient = coefficient * 10 + digit;
  }
  if (scale < 0) {
    const auto zeroes = static_cast<unsigned long long>(-scale);
    if (zeroes > kMaximumScale) {
      return std::unexpected(money_error(MoneyErrorCode::amount_overflow,
                                         "amount coefficient overflows"));
    }
    if (!checked_multiply(coefficient,
                          power_of_ten(static_cast<std::uint8_t>(zeroes)))) {
      return std::unexpected(money_error(MoneyErrorCode::amount_overflow,
                                         "amount coefficient overflows"));
    }
    scale = 0;
  }
  return DecimalAmount{coefficient, static_cast<std::uint8_t>(scale)};
}

auto DecimalAmount::to_string() const -> std::string {
  auto digits = std::to_string(m_coefficient);
  if (m_scale == 0)
    return digits;
  if (digits.size() <= m_scale) {
    return "0." + std::string(m_scale - digits.size(), '0') + digits;
  }
  digits.insert(digits.size() - m_scale, 1, '.');
  return digits;
}

auto add(const DecimalAmount &left, const DecimalAmount &right)
    -> std::expected<DecimalAmount, MoneyError> {
  const auto scale = std::max(left.m_scale, right.m_scale);
  auto left_coefficient = left.m_coefficient;
  auto right_coefficient = right.m_coefficient;
  if (!checked_multiply(left_coefficient, power_of_ten(scale - left.m_scale)) ||
      !checked_multiply(right_coefficient,
                        power_of_ten(scale - right.m_scale)) ||
      right_coefficient >
          std::numeric_limits<std::uint64_t>::max() - left_coefficient) {
    return std::unexpected(money_error(MoneyErrorCode::amount_overflow,
                                       "amount addition overflows"));
  }
  auto coefficient = left_coefficient + right_coefficient;
  auto normalized_scale = scale;
  while (normalized_scale > 0 && coefficient % 10 == 0) {
    coefficient /= 10;
    --normalized_scale;
  }
  return DecimalAmount{coefficient, normalized_scale};
}

auto MonetaryAmount::create(std::string unit, DecimalAmount amount)
    -> std::expected<MonetaryAmount, MoneyError> {
  if (!valid_unit(unit)) {
    return std::unexpected(
        money_error(MoneyErrorCode::invalid_unit, "money unit is invalid"));
  }
  return MonetaryAmount{std::move(unit), amount};
}

auto ReportedCost::create(std::vector<MonetaryAmount> amounts)
    -> std::expected<ReportedCost, MoneyError> {
  if (amounts.empty()) {
    return std::unexpected(money_error(MoneyErrorCode::empty_cost,
                                       "reported cost has no amounts"));
  }
  if (amounts.size() > kMaximumUnits) {
    return std::unexpected(money_error(MoneyErrorCode::too_many_units,
                                       "reported cost has too many units"));
  }
  std::ranges::sort(amounts, {}, &MonetaryAmount::unit);
  for (std::size_t index = 1; index < amounts.size(); ++index) {
    if (amounts[index - 1].unit() == amounts[index].unit()) {
      return std::unexpected(money_error(MoneyErrorCode::duplicate_unit,
                                         "reported cost repeats a unit"));
    }
  }
  return ReportedCost{std::move(amounts)};
}

auto add(const ReportedCost &left, const ReportedCost &right)
    -> std::expected<ReportedCost, MoneyError> {
  std::vector<MonetaryAmount> result;
  result.reserve(left.amounts().size() + right.amounts().size());
  std::size_t left_index{};
  std::size_t right_index{};
  while (left_index < left.amounts().size() ||
         right_index < right.amounts().size()) {
    if (right_index == right.amounts().size() ||
        (left_index < left.amounts().size() &&
         left.amounts()[left_index].unit() <
             right.amounts()[right_index].unit())) {
      result.push_back(left.amounts()[left_index++]);
      continue;
    }
    if (left_index == left.amounts().size() ||
        right.amounts()[right_index].unit() <
            left.amounts()[left_index].unit()) {
      result.push_back(right.amounts()[right_index++]);
      continue;
    }
    auto amount = add(left.amounts()[left_index].amount(),
                      right.amounts()[right_index].amount());
    if (!amount)
      return std::unexpected(amount.error());
    auto combined = MonetaryAmount::create(
        std::string{left.amounts()[left_index].unit()}, *amount);
    if (!combined)
      return std::unexpected(combined.error());
    result.push_back(std::move(*combined));
    ++left_index;
    ++right_index;
  }
  return ReportedCost::create(std::move(result));
}

} // namespace aiforge::domain
