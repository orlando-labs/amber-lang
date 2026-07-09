#pragma once

#include "runtime/stdlib_registry.h"

#include <cstddef>
#include <memory>
#include <regex>
#include <string>
#include <vector>

namespace amber::runtime {

struct RuntimeRegexpCaptureRange {
  bool matched = false;
  std::size_t start = 0;
  std::size_t end = 0;
};

struct RuntimeRegexpPatternValue {
  std::string source;
  std::regex::flag_type flags = std::regex::ECMAScript;
  std::regex compiled;
};

struct RuntimeRegexpMatchValue {
  std::string source;
  std::shared_ptr<RuntimeRegexpPatternValue> pattern;
  std::vector<RuntimeRegexpCaptureRange> captures;
};

std::string
runtime_regexp_pattern_to_string(const RuntimeRegexpPatternValue &pattern);
std::string
runtime_regexp_match_to_string(const RuntimeRegexpMatchValue &match);

// String-owned overloads (`Str#replace`, `Str#replaced`, `=~`, `!~`) enter the
// regexp module from the VM's scalar string dispatch.
SendStatus regexp_string_replace(NativeStdlibCall &call,
                                 const std::string &self);
SendStatus regexp_string_match_operator(NativeStdlibCall &call,
                                        const std::string &self);

} // namespace amber::runtime
