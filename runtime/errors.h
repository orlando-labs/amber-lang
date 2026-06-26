#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace amber::runtime {

inline constexpr std::uint32_t kRuntimeErrorFieldOption = 1U << 0U;
inline constexpr std::uint32_t kRuntimeErrorFieldValue = 1U << 1U;
inline constexpr std::uint32_t kRuntimeErrorFieldExitCode = 1U << 2U;
inline constexpr std::uint32_t kRuntimeErrorFieldUsage = 1U << 3U;
inline constexpr std::uint32_t kRuntimeErrorFieldHelp = 1U << 4U;

const char *runtime_error_name(std::uint16_t error_id);
std::optional<std::uint16_t> runtime_error_id(const std::string &name);
bool runtime_error_is_a(std::uint16_t error_id,
                        std::uint16_t ancestor_error_id);

std::uint32_t runtime_error_effective_field_mask(std::uint16_t error_id);
std::optional<std::int64_t>
runtime_error_default_exit_code(std::uint16_t error_id);
const char *runtime_error_default_message(std::uint16_t error_id);
std::uint32_t runtime_error_field_bit(const std::string &name);

} // namespace amber::runtime
