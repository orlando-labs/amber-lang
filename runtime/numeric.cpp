#include "runtime/numeric.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace amber::runtime {

namespace {

// --- BigInt magnitude arithmetic (amber.numeric-profile.v1) ---------------
// Magnitudes are little-endian base-2^32 limb vectors internally so that all
// products and partial divisions fit 64-bit accumulators (-Wpedantic builds
// stay free of __int128). BigIntValue stores base-2^64 limbs; conversion
// happens at the boundaries.

std::vector<std::uint32_t> big_u32_from_value(const BigIntValue &value) {
  std::vector<std::uint32_t> out;
  out.reserve(value.magnitude.size() * 2U);
  for (std::uint64_t limb : value.magnitude) {
    out.push_back(static_cast<std::uint32_t>(limb & 0xFFFFFFFFULL));
    out.push_back(static_cast<std::uint32_t>(limb >> 32U));
  }
  while (!out.empty() && out.back() == 0U) {
    out.pop_back();
  }
  return out;
}

std::shared_ptr<BigIntValue>
big_from_u32(bool negative, const std::vector<std::uint32_t> &mag) {
  auto out = std::make_shared<BigIntValue>();
  for (std::size_t i = 0; i < mag.size(); i += 2U) {
    std::uint64_t limb = mag[i];
    if (i + 1U < mag.size()) {
      limb |= static_cast<std::uint64_t>(mag[i + 1U]) << 32U;
    }
    out->magnitude.push_back(limb);
  }
  while (!out->magnitude.empty() && out->magnitude.back() == 0U) {
    out->magnitude.pop_back();
  }
  out->negative = negative && !out->magnitude.empty();
  return out;
}

int big_compare_mag_u32(const std::vector<std::uint32_t> &lhs,
                        const std::vector<std::uint32_t> &rhs) {
  if (lhs.size() != rhs.size()) {
    return lhs.size() < rhs.size() ? -1 : 1;
  }
  for (std::size_t i = lhs.size(); i-- > 0;) {
    if (lhs[i] != rhs[i]) {
      return lhs[i] < rhs[i] ? -1 : 1;
    }
  }
  return 0;
}

std::vector<std::uint32_t>
big_add_mag_u32(const std::vector<std::uint32_t> &lhs,
                const std::vector<std::uint32_t> &rhs) {
  std::vector<std::uint32_t> out;
  out.reserve(std::max(lhs.size(), rhs.size()) + 1U);
  std::uint64_t carry = 0;
  for (std::size_t i = 0; i < lhs.size() || i < rhs.size(); ++i) {
    std::uint64_t sum = carry;
    if (i < lhs.size()) {
      sum += lhs[i];
    }
    if (i < rhs.size()) {
      sum += rhs[i];
    }
    out.push_back(static_cast<std::uint32_t>(sum & 0xFFFFFFFFULL));
    carry = sum >> 32U;
  }
  if (carry != 0U) {
    out.push_back(static_cast<std::uint32_t>(carry));
  }
  return out;
}

// Requires lhs >= rhs.
std::vector<std::uint32_t>
big_sub_mag_u32(const std::vector<std::uint32_t> &lhs,
                const std::vector<std::uint32_t> &rhs) {
  std::vector<std::uint32_t> out;
  out.reserve(lhs.size());
  std::int64_t borrow = 0;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    std::int64_t diff = static_cast<std::int64_t>(lhs[i]) - borrow;
    if (i < rhs.size()) {
      diff -= static_cast<std::int64_t>(rhs[i]);
    }
    if (diff < 0) {
      diff += 0x100000000LL;
      borrow = 1;
    } else {
      borrow = 0;
    }
    out.push_back(static_cast<std::uint32_t>(diff));
  }
  while (!out.empty() && out.back() == 0U) {
    out.pop_back();
  }
  return out;
}

std::vector<std::uint32_t>
big_mul_mag_u32(const std::vector<std::uint32_t> &lhs,
                const std::vector<std::uint32_t> &rhs) {
  if (lhs.empty() || rhs.empty()) {
    return {};
  }
  std::vector<std::uint32_t> out(lhs.size() + rhs.size(), 0U);
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    std::uint64_t carry = 0;
    for (std::size_t j = 0; j < rhs.size(); ++j) {
      std::uint64_t acc =
          static_cast<std::uint64_t>(lhs[i]) * rhs[j] + out[i + j] + carry;
      out[i + j] = static_cast<std::uint32_t>(acc & 0xFFFFFFFFULL);
      carry = acc >> 32U;
    }
    std::size_t k = i + rhs.size();
    while (carry != 0U) {
      const std::uint64_t acc = static_cast<std::uint64_t>(out[k]) + carry;
      out[k] = static_cast<std::uint32_t>(acc & 0xFFFFFFFFULL);
      carry = acc >> 32U;
      ++k;
    }
  }
  while (!out.empty() && out.back() == 0U) {
    out.pop_back();
  }
  return out;
}

std::size_t big_bit_length_u32(const std::vector<std::uint32_t> &mag) {
  if (mag.empty()) {
    return 0;
  }
  std::uint32_t top = mag.back();
  std::size_t bits = (mag.size() - 1U) * 32U;
  while (top != 0U) {
    ++bits;
    top >>= 1U;
  }
  return bits;
}

bool big_get_bit_u32(const std::vector<std::uint32_t> &mag, std::size_t bit) {
  const std::size_t limb = bit / 32U;
  if (limb >= mag.size()) {
    return false;
  }
  return ((mag[limb] >> (bit % 32U)) & 1U) != 0U;
}

void big_set_bit_u32(std::vector<std::uint32_t> *mag, std::size_t bit) {
  const std::size_t limb = bit / 32U;
  if (limb >= mag->size()) {
    mag->resize(limb + 1U, 0U);
  }
  (*mag)[limb] |= std::uint32_t{1} << (bit % 32U);
}

// Binary shift-subtract long division on magnitudes. O(bits * limbs) — slow
// for huge operands but simple and allocation-light; fine for a reference
// implementation. Requires a non-zero divisor.
void big_divmod_mag_u32(const std::vector<std::uint32_t> &dividend,
                        const std::vector<std::uint32_t> &divisor,
                        std::vector<std::uint32_t> *quotient,
                        std::vector<std::uint32_t> *remainder) {
  quotient->clear();
  remainder->clear();
  if (big_compare_mag_u32(dividend, divisor) < 0) {
    *remainder = dividend;
    while (!remainder->empty() && remainder->back() == 0U) {
      remainder->pop_back();
    }
    return;
  }
  std::vector<std::uint32_t> current;
  for (std::size_t bit = big_bit_length_u32(dividend); bit-- > 0;) {
    // current = (current << 1) | dividend[bit]
    std::uint32_t carry = big_get_bit_u32(dividend, bit) ? 1U : 0U;
    for (std::size_t i = 0; i < current.size(); ++i) {
      const std::uint32_t next_carry = current[i] >> 31U;
      current[i] = (current[i] << 1U) | carry;
      carry = next_carry;
    }
    if (carry != 0U) {
      current.push_back(carry);
    }
    if (big_compare_mag_u32(current, divisor) >= 0) {
      current = big_sub_mag_u32(current, divisor);
      big_set_bit_u32(quotient, bit);
    }
  }
  *remainder = std::move(current);
  while (!remainder->empty() && remainder->back() == 0U) {
    remainder->pop_back();
  }
  while (!quotient->empty() && quotient->back() == 0U) {
    quotient->pop_back();
  }
}

} // namespace

std::shared_ptr<BigIntValue> big_from_int64(std::int64_t value) {
  auto out = std::make_shared<BigIntValue>();
  if (value == 0) {
    return out;
  }
  out->negative = value < 0;
  // Negate via uint64 to keep INT64_MIN well-defined.
  const std::uint64_t magnitude = value < 0
                                      ? ~static_cast<std::uint64_t>(value) + 1U
                                      : static_cast<std::uint64_t>(value);
  out->magnitude.push_back(magnitude);
  return out;
}

std::optional<std::int64_t> big_to_int64(const BigIntValue &value) {
  if (value.magnitude.empty()) {
    return 0;
  }
  if (value.magnitude.size() > 1U) {
    return std::nullopt;
  }
  const std::uint64_t magnitude = value.magnitude[0];
  if (value.negative) {
    if (magnitude >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
            1U) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(~magnitude + 1U);
  }
  if (magnitude >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(magnitude);
}

int big_compare_signed(const BigIntValue &lhs, const BigIntValue &rhs) {
  if (lhs.negative != rhs.negative) {
    return lhs.negative ? -1 : 1;
  }
  const int mag_order =
      big_compare_mag_u32(big_u32_from_value(lhs), big_u32_from_value(rhs));
  return lhs.negative ? -mag_order : mag_order;
}

std::shared_ptr<BigIntValue> big_add_signed(const BigIntValue &lhs,
                                            const BigIntValue &rhs) {
  const std::vector<std::uint32_t> lhs_mag = big_u32_from_value(lhs);
  const std::vector<std::uint32_t> rhs_mag = big_u32_from_value(rhs);
  if (lhs.negative == rhs.negative) {
    return big_from_u32(lhs.negative, big_add_mag_u32(lhs_mag, rhs_mag));
  }
  const int order = big_compare_mag_u32(lhs_mag, rhs_mag);
  if (order == 0) {
    return std::make_shared<BigIntValue>();
  }
  if (order > 0) {
    return big_from_u32(lhs.negative, big_sub_mag_u32(lhs_mag, rhs_mag));
  }
  return big_from_u32(rhs.negative, big_sub_mag_u32(rhs_mag, lhs_mag));
}

std::shared_ptr<BigIntValue> big_negate(const BigIntValue &value) {
  auto out = std::make_shared<BigIntValue>(value);
  out->negative = !value.negative && !value.magnitude.empty();
  return out;
}

std::shared_ptr<BigIntValue> big_mul_signed(const BigIntValue &lhs,
                                            const BigIntValue &rhs) {
  return big_from_u32(
      lhs.negative != rhs.negative,
      big_mul_mag_u32(big_u32_from_value(lhs), big_u32_from_value(rhs)));
}

// Truncated division: quotient rounds toward zero, remainder keeps the
// dividend sign — mirroring fixed-width Int `/`.
void big_divmod_trunc(const BigIntValue &lhs, const BigIntValue &rhs,
                      std::shared_ptr<BigIntValue> *quotient,
                      std::shared_ptr<BigIntValue> *remainder) {
  std::vector<std::uint32_t> q_mag;
  std::vector<std::uint32_t> r_mag;
  big_divmod_mag_u32(big_u32_from_value(lhs), big_u32_from_value(rhs), &q_mag,
                     &r_mag);
  *quotient = big_from_u32(lhs.negative != rhs.negative, q_mag);
  *remainder = big_from_u32(lhs.negative, r_mag);
}

// Floor modulo (sign of divisor) and floor division — mirroring Int `%`/`//`.
std::shared_ptr<BigIntValue> big_floor_mod(const BigIntValue &lhs,
                                           const BigIntValue &rhs) {
  std::shared_ptr<BigIntValue> quotient;
  std::shared_ptr<BigIntValue> remainder;
  big_divmod_trunc(lhs, rhs, &quotient, &remainder);
  if (remainder->magnitude.empty() || remainder->negative == rhs.negative) {
    return remainder;
  }
  return big_add_signed(*remainder, rhs);
}

std::shared_ptr<BigIntValue> big_floor_div(const BigIntValue &lhs,
                                           const BigIntValue &rhs) {
  std::shared_ptr<BigIntValue> quotient;
  std::shared_ptr<BigIntValue> remainder;
  big_divmod_trunc(lhs, rhs, &quotient, &remainder);
  if (remainder->magnitude.empty() || remainder->negative == rhs.negative) {
    return quotient;
  }
  return big_add_signed(*quotient, BigIntValue{true, {1}});
}

std::shared_ptr<BigIntValue> big_pow(const BigIntValue &base,
                                     std::uint64_t exponent) {
  auto result = std::make_shared<BigIntValue>();
  result->magnitude.push_back(1);
  auto current = std::make_shared<BigIntValue>(base);
  while (exponent > 0) {
    if ((exponent & 1U) != 0U) {
      result = big_mul_signed(*result, *current);
    }
    exponent >>= 1U;
    if (exponent > 0) {
      current = big_mul_signed(*current, *current);
    }
  }
  return result;
}

std::optional<BigIntValue> big_from_decimal_text(const std::string &text) {
  std::size_t index = 0;
  bool negative = false;
  if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
    negative = text[index] == '-';
    ++index;
  }
  if (index >= text.size()) {
    return std::nullopt;
  }
  std::vector<std::uint32_t> mag;
  bool any_digit = false;
  for (; index < text.size(); ++index) {
    const char c = text[index];
    if (c == '_') {
      continue;
    }
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    any_digit = true;
    // mag = mag * 10 + digit
    std::uint64_t carry = static_cast<std::uint64_t>(c - '0');
    for (std::size_t i = 0; i < mag.size(); ++i) {
      const std::uint64_t acc =
          static_cast<std::uint64_t>(mag[i]) * 10U + carry;
      mag[i] = static_cast<std::uint32_t>(acc & 0xFFFFFFFFULL);
      carry = acc >> 32U;
    }
    while (carry != 0U) {
      mag.push_back(static_cast<std::uint32_t>(carry & 0xFFFFFFFFULL));
      carry >>= 32U;
    }
  }
  if (!any_digit) {
    return std::nullopt;
  }
  const std::shared_ptr<BigIntValue> built = big_from_u32(negative, mag);
  return *built;
}

namespace {

// Wraps an int64 value (already exact mod 2^64) into the policy width:
// masking plus sign extension for signed widths, masking for unsigned ones.
std::int64_t numeric_wrap_to_width(std::int64_t value,
                                   const NumericPolicy &policy) {
  if (policy.bits >= 64U) {
    return value;
  }
  const std::uint64_t mask = (std::uint64_t{1} << policy.bits) - 1U;
  std::uint64_t wrapped = static_cast<std::uint64_t>(value) & mask;
  if (policy.min < 0) {
    const std::uint64_t sign_bit = std::uint64_t{1} << (policy.bits - 1U);
    if ((wrapped & sign_bit) != 0U) {
      wrapped |= ~mask;
    }
  }
  return static_cast<std::int64_t>(wrapped);
}

// Resolves an arithmetic result against the policy. `exact` is the true
// result when `overflowed64` is false; otherwise it is the int64-wrapped
// value (exact mod 2^64, which keeps wrapping-mode results correct).
// `positive_overflow` gives the saturation direction for the overflow case.
// Returns false only for checked-mode overflow (caller raises OverflowError).
bool numeric_resolve(std::int64_t exact, bool overflowed64,
                     bool positive_overflow, const NumericPolicy &policy,
                     std::int64_t *out) {
  if (!overflowed64 && exact >= policy.min && exact <= policy.max) {
    *out = exact;
    return true;
  }
  switch (policy.mode) {
  case NumericOverflowMode::Wrapping:
    *out = numeric_wrap_to_width(exact, policy);
    return true;
  case NumericOverflowMode::Saturating:
    if (overflowed64) {
      *out = positive_overflow ? policy.max : policy.min;
    } else {
      *out = exact > policy.max ? policy.max : policy.min;
    }
    return true;
  case NumericOverflowMode::Checked:
  default:
    return false;
  }
}

// Arithmetic (sign-extending) right shift; both gcc and clang implement
// signed >> as arithmetic shift, which C++20 also mandates.
std::int64_t shr_int64_arithmetic(std::int64_t value, std::int64_t shift) {
  return value >> shift;
}

} // namespace

std::int64_t floor_div_int64(std::int64_t lhs, std::int64_t rhs) {
  std::int64_t quotient = lhs / rhs;
  const std::int64_t remainder = lhs % rhs;
  if (remainder != 0 && ((remainder < 0) != (rhs < 0))) {
    --quotient;
  }
  return quotient;
}

std::int64_t floor_mod_int64(std::int64_t lhs, std::int64_t rhs) {
  if (rhs == -1) {
    // Guards the INT64_MIN % -1 overflow; the result is always zero.
    return 0;
  }
  return lhs - floor_div_int64(lhs, rhs) * rhs;
}

double floor_mod_double(double lhs, double rhs) {
  return lhs - std::floor(lhs / rhs) * rhs;
}

std::int64_t bit_xor_int64(std::int64_t lhs, std::int64_t rhs) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(lhs) ^
                                   static_cast<std::uint64_t>(rhs));
}

std::int64_t bit_and_int64(std::int64_t lhs, std::int64_t rhs) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(lhs) &
                                   static_cast<std::uint64_t>(rhs));
}

std::int64_t bit_or_int64(std::int64_t lhs, std::int64_t rhs) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(lhs) |
                                   static_cast<std::uint64_t>(rhs));
}

std::int64_t shl_int64(std::int64_t lhs, std::int64_t rhs) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(lhs) << rhs);
}

std::int64_t shr_int64(std::int64_t lhs, std::int64_t rhs) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(lhs) >> rhs);
}

std::optional<NumericPolicy> numeric_policy_for(const std::string &int_type,
                                                const std::string &overflow) {
  NumericPolicy policy;
  if (overflow == "checked" || overflow.empty()) {
    policy.mode = NumericOverflowMode::Checked;
  } else if (overflow == "wrapping") {
    policy.mode = NumericOverflowMode::Wrapping;
  } else if (overflow == "saturating") {
    policy.mode = NumericOverflowMode::Saturating;
  } else {
    return std::nullopt;
  }
  if (int_type == "Int64" || int_type.empty()) {
    return policy;
  }
  if (int_type == "Int8") {
    policy.min = -128;
    policy.max = 127;
    policy.bits = 8;
    return policy;
  }
  if (int_type == "Int16") {
    policy.min = -32768;
    policy.max = 32767;
    policy.bits = 16;
    return policy;
  }
  if (int_type == "Int32") {
    policy.min = -2147483648LL;
    policy.max = 2147483647LL;
    policy.bits = 32;
    return policy;
  }
  if (int_type == "UInt8") {
    policy.min = 0;
    policy.max = 255;
    policy.bits = 8;
    return policy;
  }
  if (int_type == "UInt16") {
    policy.min = 0;
    policy.max = 65535;
    policy.bits = 16;
    return policy;
  }
  if (int_type == "UInt32") {
    policy.min = 0;
    policy.max = 4294967295LL;
    policy.bits = 32;
    return policy;
  }
  return std::nullopt;
}

// Fixed-width Int arithmetic under the module numeric profile. Each helper
// resolves wrapping/saturating overflow inline and returns false only for
// checked-mode overflow, which the caller reports as OverflowError.
bool numeric_add_int64(std::int64_t lhs, std::int64_t rhs,
                       const NumericPolicy &policy, std::int64_t *out) {
  std::int64_t result = 0;
  const bool overflowed = __builtin_add_overflow(lhs, rhs, &result);
  return numeric_resolve(result, overflowed, lhs > 0, policy, out);
}

bool numeric_sub_int64(std::int64_t lhs, std::int64_t rhs,
                       const NumericPolicy &policy, std::int64_t *out) {
  std::int64_t result = 0;
  const bool overflowed = __builtin_sub_overflow(lhs, rhs, &result);
  return numeric_resolve(result, overflowed, lhs >= 0, policy, out);
}

bool numeric_mul_int64(std::int64_t lhs, std::int64_t rhs,
                       const NumericPolicy &policy, std::int64_t *out) {
  std::int64_t result = 0;
  const bool overflowed = __builtin_mul_overflow(lhs, rhs, &result);
  return numeric_resolve(result, overflowed, (lhs < 0) == (rhs < 0), policy,
                         out);
}

bool numeric_neg_int64(std::int64_t value, const NumericPolicy &policy,
                       std::int64_t *out) {
  if (value == std::numeric_limits<std::int64_t>::min()) {
    return numeric_resolve(value, true, true, policy, out);
  }
  return numeric_resolve(-value, false, value < 0, policy, out);
}

// Division overflow: INT64_MIN / -1 at full width, or a narrow-width result
// such as Int8 -128 / -1 == 128. Division by zero is handled by the caller
// (ZeroDivisionError) before these helpers run.
bool numeric_div_int64(std::int64_t lhs, std::int64_t rhs,
                       const NumericPolicy &policy, std::int64_t *out) {
  if (lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1) {
    return numeric_resolve(lhs, true, true, policy, out);
  }
  return numeric_resolve(lhs / rhs, false, true, policy, out);
}

bool numeric_floor_div_int64(std::int64_t lhs, std::int64_t rhs,
                             const NumericPolicy &policy, std::int64_t *out) {
  if (lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1) {
    return numeric_resolve(lhs, true, true, policy, out);
  }
  return numeric_resolve(floor_div_int64(lhs, rhs), false, true, policy, out);
}

// Shift-left overflow: any set bit shifted past bit 62 (or a sign change).
bool numeric_shl_int64(std::int64_t lhs, std::int64_t rhs,
                       const NumericPolicy &policy, std::int64_t *out) {
  const std::int64_t shifted = shl_int64(lhs, rhs);
  const bool overflowed = shr_int64_arithmetic(shifted, rhs) != lhs;
  return numeric_resolve(shifted, overflowed, lhs >= 0, policy, out);
}

// Integer pow with per-step overflow detection; negative exponents are
// resolved by the caller (float fallback) before this helper runs.
bool numeric_pow_int64(std::int64_t lhs, std::int64_t rhs,
                       const NumericPolicy &policy, std::int64_t *out) {
  std::int64_t result = 1;
  std::int64_t base = lhs;
  std::uint64_t exponent = static_cast<std::uint64_t>(rhs);
  bool overflowed = false;
  while (exponent > 0) {
    if ((exponent & 1U) != 0U) {
      if (__builtin_mul_overflow(result, base, &result)) {
        overflowed = true;
      }
    }
    exponent >>= 1U;
    if (exponent > 0 && __builtin_mul_overflow(base, base, &base)) {
      // exponent > 0 guarantees a later multiply consumes this power, so a
      // squaring overflow always implies a true overflow. Two's-complement
      // wrap stays exact mod 2^64, so the wrapping-mode result is unaffected.
      overflowed = true;
    }
  }
  const bool positive_overflow =
      !(lhs < 0 && (static_cast<std::uint64_t>(rhs) & 1U) != 0U);
  return numeric_resolve(result, overflowed, positive_overflow, policy, out);
}

} // namespace amber::runtime
