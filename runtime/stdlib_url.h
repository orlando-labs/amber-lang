#pragma once

#include "runtime/stdlib_registry.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace amber::runtime {

enum class RuntimeUrlEncodeMode { Component, Query };

struct RuntimeUrlParts {
  std::string scheme;
  bool has_authority = false;
  std::string authority;
  std::string userinfo;
  std::string host;
  bool has_port = false;
  std::int64_t port = 0;
  std::string path;
  bool has_query = false;
  std::string query;
  bool has_fragment = false;
  std::string fragment;
};

struct RuntimeUrlQueryValue {
  enum class Kind { String, List, Map };

  Kind kind = Kind::String;
  std::string text;
  std::vector<RuntimeUrlQueryValue> list;
  std::vector<std::pair<std::string, RuntimeUrlQueryValue>> map;

  static RuntimeUrlQueryValue string(std::string value);
  static RuntimeUrlQueryValue list_value(
      std::vector<RuntimeUrlQueryValue> values = {});
  static RuntimeUrlQueryValue map_value(
      std::vector<std::pair<std::string, RuntimeUrlQueryValue>> entries = {});
};

bool runtime_url_parse(const std::string &text, RuntimeUrlParts *out,
                       std::string *error);
bool runtime_url_validate_parts(const RuntimeUrlParts &parts,
                                std::string *error);
std::string runtime_url_build(const RuntimeUrlParts &parts);

std::string runtime_url_percent_encode(const std::string &text,
                                       RuntimeUrlEncodeMode mode);
bool runtime_url_percent_decode(const std::string &text, bool plus_as_space,
                                std::string *out, std::string *error);

bool runtime_url_parse_query(
    const std::string &text,
    std::vector<std::pair<std::string, RuntimeUrlQueryValue>> *out,
    std::string *error);
std::string runtime_url_build_query(
    const std::vector<std::pair<std::string, RuntimeUrlQueryValue>> &entries);

void register_url(NativeRegistry &registry);

} // namespace amber::runtime
