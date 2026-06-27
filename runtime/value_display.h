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

std::string value_to_debug_string(
    const Value &value, const bytecode::BcModule *module = nullptr,
    const std::vector<std::string> *runtime_strings = nullptr,
    const std::vector<std::string> *runtime_symbols = nullptr);

} // namespace amber::runtime
