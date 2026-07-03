#pragma once

#include "runtime/value.h"

#include <cstddef>
#include <string>
#include <vector>

namespace amber::bytecode {
struct BcModule;
}

namespace amber::runtime {

enum class RuntimeStringifyMode { Display, Inspect, Pretty };

struct RuntimePrettyPrintOptions {
  std::size_t max_width = 80;
  std::size_t max_depth = 20;
  std::size_t max_items = 100;
};

std::string runtime_uuid_to_string(const RuntimeUuidValue &value);
std::string runtime_time_to_iso8601(const RuntimeTimeValue &value);
std::string runtime_time_zone_to_string(const RuntimeTimeZoneValue &value);
std::string runtime_time_period_to_string(const RuntimeTimePeriodValue &value);

std::string runtime_stringify_value(
    const Value &value, RuntimeStringifyMode mode,
    const bytecode::BcModule *module = nullptr,
    const std::vector<std::string> *runtime_strings = nullptr,
    const std::vector<std::string> *runtime_symbols = nullptr,
    RuntimePrettyPrintOptions options = {});

std::string value_to_debug_string(
    const Value &value, const bytecode::BcModule *module = nullptr,
    const std::vector<std::string> *runtime_strings = nullptr,
    const std::vector<std::string> *runtime_symbols = nullptr);

} // namespace amber::runtime
