#include "runtime/numeric.h"

#include <cmath>

namespace amber::runtime {
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
