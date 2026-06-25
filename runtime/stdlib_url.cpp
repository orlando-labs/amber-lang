// Url stdlib library (DESIGN-stdlib-next-libs-order-2026-06-15 §4.7).
//
// v1 surface:
//   Url.parse(text) -> Map
//   Url.build(parts) -> Str
//   Url.percent_encode(text) / Url.percent_decode(text)
//   Url.parse_query(query) / Url.build_query(map)
//
// Parsed URL values intentionally use ordinary Amber Maps/Lists/Strings/Ints
// instead of a new value kind. That keeps the API lightweight while still
// giving the later net.http layer a normalized parse helper.

#include "runtime/stdlib_url.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace amber::runtime {

RuntimeUrlQueryValue RuntimeUrlQueryValue::string(std::string value) {
  RuntimeUrlQueryValue out;
  out.kind = Kind::String;
  out.text = std::move(value);
  return out;
}

RuntimeUrlQueryValue RuntimeUrlQueryValue::list_value(
    std::vector<RuntimeUrlQueryValue> values) {
  RuntimeUrlQueryValue out;
  out.kind = Kind::List;
  out.list = std::move(values);
  return out;
}

RuntimeUrlQueryValue RuntimeUrlQueryValue::map_value(
    std::vector<std::pair<std::string, RuntimeUrlQueryValue>> entries) {
  RuntimeUrlQueryValue out;
  out.kind = Kind::Map;
  out.map = std::move(entries);
  return out;
}

namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

bool is_alpha(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_scheme_char(char c) {
  return is_alpha(c) || is_digit(c) || c == '+' || c == '-' || c == '.';
}

bool is_unreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
         c == '~';
}

bool has_forbidden_url_byte(const std::string &text) {
  for (const unsigned char c : text) {
    if (c <= 0x20 || c == 0x7F) {
      return true;
    }
  }
  return false;
}

std::string ascii_lower(std::string text) {
  for (char &c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + c - 'a';
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + c - 'A';
  }
  return -1;
}

bool parse_port(const std::string &text, std::int64_t *out,
                std::string *error) {
  if (text.empty()) {
    *error = "empty URL port";
    return false;
  }
  std::int64_t value = 0;
  for (char c : text) {
    if (!is_digit(c)) {
      *error = "URL port must contain only digits";
      return false;
    }
    const int digit = c - '0';
    if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
      *error = "URL port is too large";
      return false;
    }
    value = value * 10 + digit;
  }
  if (value > 65535) {
    *error = "URL port is outside 0..65535";
    return false;
  }
  *out = value;
  return true;
}

bool valid_scheme(const std::string &scheme) {
  if (scheme.empty() || !is_alpha(scheme[0])) {
    return false;
  }
  for (char c : scheme) {
    if (!is_scheme_char(c)) {
      return false;
    }
  }
  return true;
}

bool parse_authority(const std::string &authority, RuntimeUrlParts *out,
                     std::string *error) {
  out->authority = authority;
  std::string hostport = authority;
  const std::size_t at = hostport.rfind('@');
  if (at != std::string::npos) {
    out->userinfo = hostport.substr(0, at);
    hostport = hostport.substr(at + 1U);
  }
  if (hostport.empty()) {
    out->host.clear();
    return true;
  }

  if (hostport[0] == '[') {
    const std::size_t close = hostport.find(']');
    if (close == std::string::npos) {
      *error = "unterminated IPv6 URL host";
      return false;
    }
    out->host = ascii_lower(hostport.substr(1U, close - 1U));
    if (close + 1U < hostport.size()) {
      if (hostport[close + 1U] != ':') {
        *error = "unexpected text after bracketed URL host";
        return false;
      }
      out->has_port = true;
      return parse_port(hostport.substr(close + 2U), &out->port, error);
    }
    return true;
  }

  const std::size_t first_colon = hostport.find(':');
  const std::size_t last_colon = hostport.rfind(':');
  if (first_colon != std::string::npos && first_colon == last_colon) {
    out->host = ascii_lower(hostport.substr(0, first_colon));
    out->has_port = true;
    return parse_port(hostport.substr(first_colon + 1U), &out->port, error);
  }
  out->host = ascii_lower(hostport);
  return true;
}

Value null_or_string(NativeStdlibCall &call, bool present,
                     const std::string &text) {
  return present ? call.string_value(text) : Value::null();
}

Value query_value_to_amber(NativeStdlibCall &call,
                           const RuntimeUrlQueryValue &value);

Value make_query_map(
    NativeStdlibCall &call,
    const std::vector<std::pair<std::string, RuntimeUrlQueryValue>> &entries) {
  std::vector<std::pair<std::string, Value>> object;
  object.reserve(entries.size());
  for (const auto &entry : entries) {
    object.push_back({entry.first, query_value_to_amber(call, entry.second)});
  }
  return call.make_object(std::move(object));
}

Value query_value_to_amber(NativeStdlibCall &call,
                           const RuntimeUrlQueryValue &value) {
  if (value.kind == RuntimeUrlQueryValue::Kind::String) {
    return call.string_value(value.text);
  }
  if (value.kind == RuntimeUrlQueryValue::Kind::List) {
    std::vector<Value> items;
    items.reserve(value.list.size());
    for (const RuntimeUrlQueryValue &item : value.list) {
      items.push_back(query_value_to_amber(call, item));
    }
    return call.make_list(std::move(items));
  }
  return make_query_map(call, value.map);
}

Value parts_to_map(NativeStdlibCall &call, const RuntimeUrlParts &parts) {
  std::vector<std::pair<std::string, Value>> entries;
  entries.reserve(10U);
  entries.push_back({"scheme", null_or_string(call, !parts.scheme.empty(),
                                               parts.scheme)});
  entries.push_back(
      {"authority", null_or_string(call, parts.has_authority, parts.authority)});
  entries.push_back({"userinfo", null_or_string(call, !parts.userinfo.empty(),
                                                parts.userinfo)});
  entries.push_back(
      {"host", null_or_string(call, parts.has_authority, parts.host)});
  entries.push_back({"port", parts.has_port ? Value::integer(parts.port)
                                             : Value::null()});
  entries.push_back({"path", call.string_value(parts.path)});
  entries.push_back(
      {"query", null_or_string(call, parts.has_query, parts.query)});
  entries.push_back(
      {"fragment",
       null_or_string(call, parts.has_fragment, parts.fragment)});
  if (parts.has_query) {
    std::vector<std::pair<std::string, RuntimeUrlQueryValue>> query;
    std::string error;
    if (runtime_url_parse_query(parts.query, &query, &error)) {
      entries.push_back({"query_map", make_query_map(call, query)});
    } else {
      entries.push_back({"query_map", Value::null()});
    }
  } else {
    entries.push_back({"query_map", Value::null()});
  }
  return call.make_object(std::move(entries));
}

std::optional<Value> map_lookup(
    const std::vector<std::pair<std::string, Value>> &entries,
    const std::string &name) {
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    if (it->first == name) {
      return it->second;
    }
  }
  return std::nullopt;
}

bool optional_string_field(
    NativeStdlibCall &call,
    const std::vector<std::pair<std::string, Value>> &entries,
    const std::string &name, std::string *out, bool *present) {
  const std::optional<Value> value = map_lookup(entries, name);
  if (!value.has_value() || value->is_null()) {
    *present = false;
    out->clear();
    return true;
  }
  if (!value->is_string()) {
    call.fault("TypeError", "Url." + name + " must be Str or null");
    return false;
  }
  const std::optional<std::string> text = call.text_of(*value);
  if (!text.has_value()) {
    call.fault("TypeError", "Url." + name + " must be Str or null");
    return false;
  }
  *present = true;
  *out = *text;
  return true;
}

bool query_entries_from_amber_map(
    NativeStdlibCall &call, const Value &value,
    std::vector<std::pair<std::string, RuntimeUrlQueryValue>> *out,
    const std::string &context);

bool query_value_from_amber(NativeStdlibCall &call, const Value &value,
                            RuntimeUrlQueryValue *out,
                            const std::string &context) {
  if (value.is_string()) {
    const std::optional<std::string> text = call.text_of(value);
    if (!text.has_value()) {
      call.fault("TypeError", context + " values must be Str, List, or Map");
      return false;
    }
    *out = RuntimeUrlQueryValue::string(*text);
    return true;
  }
  if (value.is_list()) {
    std::vector<Value> items;
    if (!call.list_items(value, &items)) {
      return false;
    }
    std::vector<RuntimeUrlQueryValue> list;
    list.reserve(items.size());
    for (const Value &item : items) {
      RuntimeUrlQueryValue converted;
      if (!query_value_from_amber(call, item, &converted, context)) {
        return false;
      }
      list.push_back(std::move(converted));
    }
    *out = RuntimeUrlQueryValue::list_value(std::move(list));
    return true;
  }
  if (value.is_map()) {
    std::vector<std::pair<std::string, RuntimeUrlQueryValue>> map;
    if (!query_entries_from_amber_map(call, value, &map, context)) {
      return false;
    }
    *out = RuntimeUrlQueryValue::map_value(std::move(map));
    return true;
  }
  call.fault("TypeError", context + " values must be Str, List, or Map");
  return false;
}

bool query_entries_from_amber_map(
    NativeStdlibCall &call, const Value &value,
    std::vector<std::pair<std::string, RuntimeUrlQueryValue>> *out,
    const std::string &context) {
  std::vector<std::pair<std::string, Value>> entries;
  if (!call.string_keyed_entries(value, &entries)) {
    return false;
  }
  out->clear();
  out->reserve(entries.size());
  for (const auto &entry : entries) {
    RuntimeUrlQueryValue converted;
    if (!query_value_from_amber(call, entry.second, &converted, context)) {
      return false;
    }
    out->push_back({entry.first, std::move(converted)});
  }
  return true;
}

bool parts_from_map(NativeStdlibCall &call, const Value &value,
                    RuntimeUrlParts *parts) {
  std::vector<std::pair<std::string, Value>> entries;
  if (!call.string_keyed_entries(value, &entries)) {
    return false;
  }
  bool has_scheme = false;
  bool has_userinfo = false;
  bool has_host = false;
  bool has_path = false;
  if (!optional_string_field(call, entries, "scheme", &parts->scheme,
                             &has_scheme) ||
      !optional_string_field(call, entries, "userinfo", &parts->userinfo,
                             &has_userinfo) ||
      !optional_string_field(call, entries, "host", &parts->host, &has_host) ||
      !optional_string_field(call, entries, "path", &parts->path, &has_path) ||
      !optional_string_field(call, entries, "query", &parts->query,
                             &parts->has_query) ||
      !optional_string_field(call, entries, "fragment", &parts->fragment,
                             &parts->has_fragment)) {
    return false;
  }
  (void)has_scheme;
  (void)has_userinfo;
  parts->has_authority = has_host;
  if (!has_path) {
    parts->path.clear();
  }

  const std::optional<Value> port = map_lookup(entries, "port");
  if (port.has_value() && !port->is_null()) {
    if (!port->is_integer()) {
      call.fault("TypeError", "Url.port must be Int or null");
      return false;
    }
    parts->has_port = true;
    parts->port = port->as_integer();
  }

  const std::optional<Value> query_map = map_lookup(entries, "query_map");
  if ((!parts->has_query || parts->query.empty()) && query_map.has_value() &&
      !query_map->is_null()) {
    std::vector<std::pair<std::string, RuntimeUrlQueryValue>> parsed;
    if (!query_entries_from_amber_map(call, *query_map, &parsed,
                                      "query_map")) {
      return false;
    }
    parts->query = runtime_url_build_query(parsed);
    parts->has_query = true;
  }

  std::string error;
  if (!runtime_url_validate_parts(*parts, &error)) {
    call.fault("UrlBuildError", error);
    return false;
  }
  return true;
}

bool query_pairs_from_map(
    NativeStdlibCall &call, const Value &value,
    std::vector<std::pair<std::string, RuntimeUrlQueryValue>> *out) {
  return query_entries_from_amber_map(call, value, out, "Url.build_query");
}

SendStatus url_parse(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  if (!call.args[0].is_string()) {
    return call.fault("TypeError", "Url.parse expects Str");
  }
  const std::optional<std::string> text = call.text_of(call.args[0]);
  if (!text.has_value()) {
    return call.fault("TypeError", "Url.parse expects Str");
  }
  RuntimeUrlParts parts;
  std::string error;
  if (!runtime_url_parse(*text, &parts, &error)) {
    return call.fault("UrlParseError", error);
  }
  if (parts.has_query) {
    std::vector<std::pair<std::string, RuntimeUrlQueryValue>> query;
    if (!runtime_url_parse_query(parts.query, &query, &error)) {
      return call.fault("UrlDecodeError", error);
    }
  }
  *call.out = parts_to_map(call, parts);
  return SendStatus::Matched;
}

SendStatus url_build(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  RuntimeUrlParts parts;
  if (!parts_from_map(call, call.args[0], &parts)) {
    return SendStatus::Faulted;
  }
  *call.out = call.string_value(runtime_url_build(parts));
  return SendStatus::Matched;
}

SendStatus url_percent_encode(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  if (!call.args[0].is_string()) {
    return call.fault("TypeError", "Url.percent_encode expects Str");
  }
  const std::optional<std::string> text = call.text_of(call.args[0]);
  if (!text.has_value()) {
    return call.fault("TypeError", "Url.percent_encode expects Str");
  }
  *call.out = call.string_value(
      runtime_url_percent_encode(*text, RuntimeUrlEncodeMode::Component));
  return SendStatus::Matched;
}

SendStatus url_percent_decode(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  if (!call.args[0].is_string()) {
    return call.fault("TypeError", "Url.percent_decode expects Str");
  }
  const std::optional<std::string> text = call.text_of(call.args[0]);
  if (!text.has_value()) {
    return call.fault("TypeError", "Url.percent_decode expects Str");
  }
  std::string decoded;
  std::string error;
  if (!runtime_url_percent_decode(*text, false, &decoded, &error)) {
    return call.fault("UrlDecodeError", error);
  }
  *call.out = call.string_value(std::move(decoded));
  return SendStatus::Matched;
}

SendStatus url_parse_query(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  if (!call.args[0].is_string()) {
    return call.fault("TypeError", "Url.parse_query expects Str");
  }
  const std::optional<std::string> text = call.text_of(call.args[0]);
  if (!text.has_value()) {
    return call.fault("TypeError", "Url.parse_query expects Str");
  }
  std::vector<std::pair<std::string, RuntimeUrlQueryValue>> entries;
  std::string error;
  if (!runtime_url_parse_query(*text, &entries, &error)) {
    return call.fault("UrlDecodeError", error);
  }
  *call.out = make_query_map(call, entries);
  return SendStatus::Matched;
}

SendStatus url_build_query(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  std::vector<std::pair<std::string, RuntimeUrlQueryValue>> entries;
  if (!query_pairs_from_map(call, call.args[0], &entries)) {
    return SendStatus::Faulted;
  }
  *call.out = call.string_value(runtime_url_build_query(entries));
  return SendStatus::Matched;
}

SendStatus url_dispatch(NativeStdlibCall &call) {
  if (call.kind != RuntimeNativeTypeKind::Url) {
    return SendStatus::NotHandled;
  }
  if (call.receiver.is_native_type()) {
    if (call.selector == "parse") {
      return url_parse(call);
    }
    if (call.selector == "build") {
      return url_build(call);
    }
    if (call.selector == "percent_encode") {
      return url_percent_encode(call);
    }
    if (call.selector == "percent_decode") {
      return url_percent_decode(call);
    }
    if (call.selector == "parse_query") {
      return url_parse_query(call);
    }
    if (call.selector == "build_query") {
      return url_build_query(call);
    }
  }
  return SendStatus::NotHandled;
}

} // namespace

bool runtime_url_parse(const std::string &text, RuntimeUrlParts *out,
                       std::string *error) {
  if (text.empty()) {
    *error = "empty URL";
    return false;
  }
  if (has_forbidden_url_byte(text)) {
    *error = "URL contains raw ASCII whitespace or controls";
    return false;
  }

  RuntimeUrlParts parts;
  std::size_t pos = 0;
  const std::size_t first_delim = text.find_first_of(":/?#");
  if (first_delim != std::string::npos && text[first_delim] == ':') {
    const std::string scheme = text.substr(0, first_delim);
    if (!valid_scheme(scheme)) {
      *error = "invalid URL scheme";
      return false;
    }
    parts.scheme = ascii_lower(scheme);
    pos = first_delim + 1U;
  }

  if (pos + 1U < text.size() && text[pos] == '/' && text[pos + 1U] == '/') {
    parts.has_authority = true;
    pos += 2U;
    const std::size_t authority_end = text.find_first_of("/?#", pos);
    const std::string authority =
        authority_end == std::string::npos
            ? text.substr(pos)
            : text.substr(pos, authority_end - pos);
    if (!parse_authority(authority, &parts, error)) {
      return false;
    }
    pos = authority_end == std::string::npos ? text.size() : authority_end;
  }

  const std::size_t fragment_pos = text.find('#', pos);
  const std::size_t query_search_end =
      fragment_pos == std::string::npos ? text.size() : fragment_pos;
  const std::size_t query_pos = text.find('?', pos);
  const bool has_query =
      query_pos != std::string::npos && query_pos < query_search_end;
  const std::size_t path_end = has_query ? query_pos : query_search_end;
  parts.path = text.substr(pos, path_end - pos);
  if (parts.has_authority && parts.path.empty()) {
    parts.path = "/";
  }
  if (has_query) {
    parts.has_query = true;
    parts.query = text.substr(query_pos + 1U, query_search_end - query_pos - 1U);
  }
  if (fragment_pos != std::string::npos) {
    parts.has_fragment = true;
    parts.fragment = text.substr(fragment_pos + 1U);
  }

  std::string validation;
  if (!runtime_url_validate_parts(parts, &validation)) {
    *error = validation;
    return false;
  }
  *out = std::move(parts);
  return true;
}

bool runtime_url_validate_parts(const RuntimeUrlParts &parts,
                                std::string *error) {
  if (!parts.scheme.empty() && !valid_scheme(parts.scheme)) {
    *error = "invalid URL scheme";
    return false;
  }
  if (has_forbidden_url_byte(parts.userinfo) ||
      has_forbidden_url_byte(parts.host) || has_forbidden_url_byte(parts.path) ||
      has_forbidden_url_byte(parts.query) ||
      has_forbidden_url_byte(parts.fragment)) {
    *error = "URL part contains raw ASCII whitespace or controls";
    return false;
  }
  if (parts.has_port && (parts.port < 0 || parts.port > 65535)) {
    *error = "URL port is outside 0..65535";
    return false;
  }
  return true;
}

std::string runtime_url_build(const RuntimeUrlParts &parts) {
  std::string out;
  if (!parts.scheme.empty()) {
    out += ascii_lower(parts.scheme);
    out.push_back(':');
  }
  if (parts.has_authority || !parts.host.empty()) {
    out += "//";
    if (!parts.userinfo.empty()) {
      out += parts.userinfo;
      out.push_back('@');
    }
    const bool needs_brackets =
        parts.host.find(':') != std::string::npos &&
        !(parts.host.size() >= 2U && parts.host.front() == '[' &&
          parts.host.back() == ']');
    if (needs_brackets) {
      out.push_back('[');
    }
    out += ascii_lower(parts.host);
    if (needs_brackets) {
      out.push_back(']');
    }
    if (parts.has_port) {
      out.push_back(':');
      out += std::to_string(parts.port);
    }
  }
  std::string path = parts.path;
  if ((parts.has_authority || !parts.host.empty()) && path.empty()) {
    path = "/";
  } else if ((parts.has_authority || !parts.host.empty()) && !path.empty() &&
             path.front() != '/') {
    out.push_back('/');
  }
  out += path;
  if (parts.has_query) {
    out.push_back('?');
    out += parts.query;
  }
  if (parts.has_fragment) {
    out.push_back('#');
    out += parts.fragment;
  }
  return out;
}

std::string runtime_url_percent_encode(const std::string &text,
                                       RuntimeUrlEncodeMode mode) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    if (is_unreserved(c)) {
      out.push_back(static_cast<char>(c));
    } else if (mode == RuntimeUrlEncodeMode::Query && c == ' ') {
      out.push_back('+');
    } else {
      out.push_back('%');
      out.push_back(kHexDigits[(c >> 4U) & 0x0FU]);
      out.push_back(kHexDigits[c & 0x0FU]);
    }
  }
  return out;
}

bool runtime_url_percent_decode(const std::string &text, bool plus_as_space,
                                std::string *out, std::string *error) {
  out->clear();
  out->reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (plus_as_space && c == '+') {
      out->push_back(' ');
      continue;
    }
    if (c != '%') {
      out->push_back(c);
      continue;
    }
    if (i + 2U >= text.size()) {
      *error = "incomplete percent escape";
      return false;
    }
    const int high = hex_value(text[i + 1U]);
    const int low = hex_value(text[i + 2U]);
    if (high < 0 || low < 0) {
      *error = "invalid percent escape";
      return false;
    }
    out->push_back(static_cast<char>((high << 4U) | low));
    i += 2U;
  }
  return true;
}

namespace {

std::vector<std::string> query_key_segments(const std::string &key,
                                            std::string *error) {
  std::vector<std::string> segments;
  const std::size_t first_bracket = key.find('[');
  if (first_bracket == std::string::npos) {
    segments.push_back(key);
    return segments;
  }
  segments.push_back(key.substr(0, first_bracket));
  std::size_t pos = first_bracket;
  while (pos < key.size()) {
    if (key[pos] != '[') {
      *error = "invalid query bracket syntax";
      return {};
    }
    const std::size_t close = key.find(']', pos + 1U);
    if (close == std::string::npos) {
      *error = "unterminated query bracket";
      return {};
    }
    segments.push_back(key.substr(pos + 1U, close - pos - 1U));
    pos = close + 1U;
  }
  return segments;
}

RuntimeUrlQueryValue *find_query_entry(
    std::vector<std::pair<std::string, RuntimeUrlQueryValue>> *entries,
    const std::string &key) {
  for (auto &entry : *entries) {
    if (entry.first == key) {
      return &entry.second;
    }
  }
  return nullptr;
}

void merge_query_leaf(RuntimeUrlQueryValue *slot, std::string value) {
  if (slot->kind == RuntimeUrlQueryValue::Kind::String) {
    std::vector<RuntimeUrlQueryValue> values;
    values.push_back(RuntimeUrlQueryValue::string(std::move(slot->text)));
    values.push_back(RuntimeUrlQueryValue::string(std::move(value)));
    *slot = RuntimeUrlQueryValue::list_value(std::move(values));
    return;
  }
  if (slot->kind == RuntimeUrlQueryValue::Kind::List) {
    slot->list.push_back(RuntimeUrlQueryValue::string(std::move(value)));
  }
}

bool ensure_query_list(RuntimeUrlQueryValue *slot, std::string *error) {
  if (slot->kind == RuntimeUrlQueryValue::Kind::List) {
    return true;
  }
  if (slot->kind == RuntimeUrlQueryValue::Kind::String) {
    std::vector<RuntimeUrlQueryValue> values;
    values.push_back(RuntimeUrlQueryValue::string(std::move(slot->text)));
    *slot = RuntimeUrlQueryValue::list_value(std::move(values));
    return true;
  }
  *error = "query key mixes array and map shapes";
  return false;
}

bool ensure_query_map(RuntimeUrlQueryValue *slot, std::string *error) {
  if (slot->kind == RuntimeUrlQueryValue::Kind::Map) {
    return true;
  }
  *error = "query key mixes scalar/array and map shapes";
  return false;
}

RuntimeUrlQueryValue container_for_next_segment(
    const std::vector<std::string> &segments, std::size_t next_index) {
  if (next_index < segments.size() && segments[next_index].empty()) {
    return RuntimeUrlQueryValue::list_value();
  }
  return RuntimeUrlQueryValue::map_value();
}

bool insert_query_value(
    RuntimeUrlQueryValue *slot, const std::vector<std::string> &segments,
    std::size_t index, const std::string &value, std::string *error);

bool insert_query_map_entry(
    std::vector<std::pair<std::string, RuntimeUrlQueryValue>> *entries,
    const std::vector<std::string> &segments, std::size_t index,
    const std::string &value, std::string *error) {
  const std::string &key = segments[index];
  RuntimeUrlQueryValue *slot = find_query_entry(entries, key);
  if (index + 1U == segments.size()) {
    if (slot == nullptr) {
      entries->push_back({key, RuntimeUrlQueryValue::string(value)});
      return true;
    }
    if (slot->kind == RuntimeUrlQueryValue::Kind::Map) {
      *error = "query key mixes map and scalar shapes";
      return false;
    }
    merge_query_leaf(slot, value);
    return true;
  }

  if (slot == nullptr) {
    entries->push_back(
        {key, container_for_next_segment(segments, index + 1U)});
    slot = &entries->back().second;
  }
  return insert_query_value(slot, segments, index + 1U, value, error);
}

bool insert_query_value(
    RuntimeUrlQueryValue *slot, const std::vector<std::string> &segments,
    std::size_t index, const std::string &value, std::string *error) {
  if (index >= segments.size()) {
    if (slot->kind == RuntimeUrlQueryValue::Kind::Map) {
      *error = "query key mixes map and scalar shapes";
      return false;
    }
    merge_query_leaf(slot, value);
    return true;
  }

  const std::string &segment = segments[index];
  if (segment.empty()) {
    if (!ensure_query_list(slot, error)) {
      return false;
    }
    if (index + 1U == segments.size()) {
      slot->list.push_back(RuntimeUrlQueryValue::string(value));
      return true;
    }
    RuntimeUrlQueryValue child =
        container_for_next_segment(segments, index + 1U);
    slot->list.push_back(std::move(child));
    return insert_query_value(&slot->list.back(), segments, index + 1U, value,
                              error);
  }

  if (!ensure_query_map(slot, error)) {
    return false;
  }
  return insert_query_map_entry(&slot->map, segments, index, value, error);
}

void append_built_query_value(const std::string &prefix,
                              const RuntimeUrlQueryValue &value,
                              std::string *out, bool *first);

void append_built_query_pair(const std::string &key, const std::string &value,
                             std::string *out, bool *first) {
  if (!*first) {
    out->push_back('&');
  }
  *first = false;
  *out += key;
  out->push_back('=');
  *out += runtime_url_percent_encode(value, RuntimeUrlEncodeMode::Query);
}

void append_built_query_value(const std::string &prefix,
                              const RuntimeUrlQueryValue &value,
                              std::string *out, bool *first) {
  if (value.kind == RuntimeUrlQueryValue::Kind::String) {
    append_built_query_pair(prefix, value.text, out, first);
    return;
  }
  if (value.kind == RuntimeUrlQueryValue::Kind::List) {
    for (const RuntimeUrlQueryValue &item : value.list) {
      append_built_query_value(prefix + "[]", item, out, first);
    }
    return;
  }
  for (const auto &entry : value.map) {
    const std::string child =
        prefix + "[" +
        runtime_url_percent_encode(entry.first, RuntimeUrlEncodeMode::Query) +
        "]";
    append_built_query_value(child, entry.second, out, first);
  }
}

} // namespace

bool runtime_url_parse_query(
    const std::string &text,
    std::vector<std::pair<std::string, RuntimeUrlQueryValue>> *out,
    std::string *error) {
  out->clear();
  std::size_t pos = !text.empty() && text.front() == '?' ? 1U : 0U;
  while (pos <= text.size()) {
    const std::size_t amp = text.find('&', pos);
    const std::size_t end = amp == std::string::npos ? text.size() : amp;
    if (end != pos) {
      const std::size_t eq = text.find('=', pos);
      const bool has_eq = eq != std::string::npos && eq < end;
      const std::string raw_key =
          text.substr(pos, (has_eq ? eq : end) - pos);
      const std::string raw_value =
          has_eq ? text.substr(eq + 1U, end - eq - 1U) : std::string{};
      std::string key;
      std::string value;
      if (!runtime_url_percent_decode(raw_key, true, &key, error) ||
          !runtime_url_percent_decode(raw_value, true, &value, error)) {
        return false;
      }
      const std::vector<std::string> segments = query_key_segments(key, error);
      if (segments.empty() && !key.empty()) {
        return false;
      }
      if (!insert_query_map_entry(out, segments, 0U, value, error)) {
        return false;
      }
    }
    if (amp == std::string::npos) {
      break;
    }
    pos = amp + 1U;
  }
  return true;
}

std::string runtime_url_build_query(
    const std::vector<std::pair<std::string, RuntimeUrlQueryValue>> &entries) {
  std::string out;
  bool first = true;
  for (const auto &entry : entries) {
    const std::string key =
        runtime_url_percent_encode(entry.first, RuntimeUrlEncodeMode::Query);
    append_built_query_value(key, entry.second, &out, &first);
  }
  return out;
}

RuntimeNativeModuleDescriptor url_module_descriptor() {
  return {{{"Url", RuntimeNativeTypeKind::Url}},
          {{RuntimeNativeTypeKind::Url, &url_dispatch}}};
}

void register_url(NativeRegistry &registry) {
  register_native_module_descriptor(registry, url_module_descriptor());
}

void register_url_runtime_module(RuntimeModuleRegistry &modules,
                                 RuntimeDispatchRegistry &dispatch) {
  const RuntimeNativeModuleDescriptor descriptor = url_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
}

} // namespace amber::runtime
