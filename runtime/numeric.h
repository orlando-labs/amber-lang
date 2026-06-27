#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace amber::runtime {

// Overflow policy for fixed-width Int arithmetic (amber.numeric-profile.v1).
// `checked` raises OverflowError; `wrapping` wraps two's-complement;
// `saturating` clamps to the type bounds.
enum class NumericOverflowMode : std::uint8_t { Checked, Wrapping, Saturating };

// Resolved module numeric profile: the selected overflow mode plus the bounds
// of the concrete `Int` width. Defaults describe the default profile
// (`int: Int64`, `overflow: checked`). `min == 0` marks unsigned widths.
struct NumericPolicy {
  NumericOverflowMode mode = NumericOverflowMode::Checked;
  std::int64_t min = std::numeric_limits<std::int64_t>::min();
  std::int64_t max = std::numeric_limits<std::int64_t>::max();
  std::uint32_t bits = 64;
};

// Maps a numeric profile `int` type name (e.g. "Int32", "UInt8") to bounds.
// Returns nullopt for types the reference VM cannot represent (UInt64,
// BigInt-as-Int) or unknown names.
std::optional<NumericPolicy> numeric_policy_for(const std::string &int_type,
                                                const std::string &overflow);

std::int64_t floor_div_int64(std::int64_t lhs, std::int64_t rhs);
std::int64_t floor_mod_int64(std::int64_t lhs, std::int64_t rhs);
double floor_mod_double(double lhs, double rhs);

std::int64_t bit_xor_int64(std::int64_t lhs, std::int64_t rhs);
std::int64_t bit_and_int64(std::int64_t lhs, std::int64_t rhs);
std::int64_t bit_or_int64(std::int64_t lhs, std::int64_t rhs);
std::int64_t shl_int64(std::int64_t lhs, std::int64_t rhs);
std::int64_t shr_int64(std::int64_t lhs, std::int64_t rhs);

bool numeric_add_int64(std::int64_t lhs, std::int64_t rhs,
                       const NumericPolicy &policy, std::int64_t *out);
bool numeric_sub_int64(std::int64_t lhs, std::int64_t rhs,
                       const NumericPolicy &policy, std::int64_t *out);
bool numeric_mul_int64(std::int64_t lhs, std::int64_t rhs,
                       const NumericPolicy &policy, std::int64_t *out);
bool numeric_neg_int64(std::int64_t value, const NumericPolicy &policy,
                       std::int64_t *out);
bool numeric_div_int64(std::int64_t lhs, std::int64_t rhs,
                       const NumericPolicy &policy, std::int64_t *out);
bool numeric_floor_div_int64(std::int64_t lhs, std::int64_t rhs,
                             const NumericPolicy &policy, std::int64_t *out);
bool numeric_shl_int64(std::int64_t lhs, std::int64_t rhs,
                       const NumericPolicy &policy, std::int64_t *out);
bool numeric_pow_int64(std::int64_t lhs, std::int64_t rhs,
                       const NumericPolicy &policy, std::int64_t *out);

} // namespace amber::runtime
