// Json — JSON parse/generate stdlib library on the Layer 0 substrate
// (DESIGN-stdlib-next-libs-order-2026-06-15 §4.1, DESIGN-stdlib-json-api-2026-06-16).
//
// v1 surface implemented here: `Json.parse(text[, strict: Bool])`,
// `Json.generate(value)`, `Json.pretty_generate(value[, indent: Int])`. Streaming
// (`stream_parse`) and file I/O (`load_from_file`/`save_to_file`) land in later
// increments. Pure compute against RFC 8259: correct Int-vs-Float typing, string
// escapes, and UTF-8 (incl. `\u` surrogate pairs). Malformed input faults
// `JsonParseError`; non-representable values fault `JsonGenerateError`.

#include "runtime/stdlib_registry.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace amber::runtime {

namespace {

constexpr int kMaxDepth = 512;

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------

void encode_utf8(std::uint32_t cp, std::string &out) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Shortest decimal that round-trips, guaranteeing the result re-parses as Float
// (so an integral-valued Float keeps a `.0`, never collapsing to Int).
std::string format_double(double d) {
  char buf[40];
  std::string result;
  for (int precision = 1; precision <= 17; ++precision) {
    std::snprintf(buf, sizeof(buf), "%.*g", precision, d);
    if (std::strtod(buf, nullptr) == d) {
      result.assign(buf);
      break;
    }
    if (precision == 17) {
      result.assign(buf);
    }
  }
  if (result.find_first_of(".eEnN") == std::string::npos) {
    result += ".0";
  }
  return result;
}

void write_json_string(const std::string &text, std::string &out) {
  out.push_back('"');
  for (const unsigned char c : text) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        char esc[8];
        std::snprintf(esc, sizeof(esc), "\\u%04x", c);
        out += esc;
      } else {
        out.push_back(static_cast<char>(c)); // UTF-8 bytes pass through
      }
    }
  }
  out.push_back('"');
}

struct Generator {
  explicit Generator(NativeStdlibCall &call_in) : call(call_in) {}

  NativeStdlibCall &call;
  std::string out;
  bool pretty = false;
  int indent_width = 2;
  bool faulted = false;

  void newline(int depth) {
    if (!pretty) {
      return;
    }
    out.push_back('\n');
    out.append(static_cast<std::size_t>(depth) * indent_width, ' ');
  }

  bool fail(const std::string &message) {
    call.fault("JsonGenerateError", message);
    faulted = true;
    return false;
  }

  bool emit(const Value &value, int depth) {
    if (depth > kMaxDepth) {
      return fail("JSON nesting is too deep to generate");
    }
    if (value.is_null()) {
      out += "null";
      return true;
    }
    if (value.is_bool()) {
      out += value.as_bool() ? "true" : "false";
      return true;
    }
    if (value.is_integer()) {
      out += std::to_string(value.as_integer());
      return true;
    }
    if (value.is_float()) {
      const double d = value.as_float();
      if (std::isnan(d) || std::isinf(d)) {
        return fail("NaN and Infinity are not valid JSON");
      }
      out += format_double(d);
      return true;
    }
    if (value.is_string() || value.is_symbol()) {
      write_json_string(call.text_of(value).value_or(""), out);
      return true;
    }
    if (value.is_list()) {
      const IntrusivePtr<ListValue> list = value.as_list();
      if (list == nullptr || list->items.empty()) {
        out += "[]";
        return true;
      }
      out.push_back('[');
      bool first = true;
      for (const Value &item : list->items) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        newline(depth + 1);
        if (!emit(item, depth + 1)) {
          return false;
        }
      }
      newline(depth);
      out.push_back(']');
      return true;
    }
    if (value.is_map()) {
      const IntrusivePtr<MapValue> map = value.as_map();
      if (map == nullptr || map->entries.empty()) {
        out += "{}";
        return true;
      }
      out.push_back('{');
      bool first = true;
      for (const MapEntry &entry : map->entries) {
        const std::optional<std::string> key = call.text_of(entry.key);
        if (!key.has_value()) {
          return fail("JSON object keys must be Str or Symbol");
        }
        if (!first) {
          out.push_back(',');
        }
        first = false;
        newline(depth + 1);
        write_json_string(*key, out);
        out.push_back(':');
        if (pretty) {
          out.push_back(' ');
        }
        if (!emit(entry.value, depth + 1)) {
          return false;
        }
      }
      newline(depth);
      out.push_back('}');
      return true;
    }
    return fail("value is not representable as JSON");
  }
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

struct Parser {
  Parser(NativeStdlibCall &call_in, const std::string &src_in)
      : call(call_in), src(src_in) {}

  NativeStdlibCall &call;
  const std::string &src;
  bool strict_maps = false;
  std::size_t pos = 0;
  bool faulted = false;

  bool fail(const std::string &message) {
    std::size_t line = 1;
    std::size_t col = 1;
    for (std::size_t i = 0; i < pos && i < src.size(); ++i) {
      if (src[i] == '\n') {
        ++line;
        col = 1;
      } else {
        ++col;
      }
    }
    call.fault("JsonParseError", message + " at line " + std::to_string(line) +
                                     ":" + std::to_string(col) + " (offset " +
                                     std::to_string(pos) + ")");
    faulted = true;
    return false;
  }

  void skip_ws() {
    while (pos < src.size()) {
      const char c = src[pos];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos;
      } else {
        break;
      }
    }
  }

  // Each parse_* sets `*ok=false` (via fail) and returns null on error.
  Value parse_value(int depth, bool *ok) {
    if (depth > kMaxDepth) {
      *ok = fail("JSON nesting is too deep");
      return Value::null();
    }
    skip_ws();
    if (pos >= src.size()) {
      *ok = fail("unexpected end of input");
      return Value::null();
    }
    const char c = src[pos];
    switch (c) {
    case '{':
      return parse_object(depth, ok);
    case '[':
      return parse_array(depth, ok);
    case '"': {
      std::string text;
      if (!parse_string(&text)) {
        *ok = false;
        return Value::null();
      }
      return call.string_value(std::move(text));
    }
    case 't':
    case 'f':
      return parse_bool(ok);
    case 'n':
      return parse_null(ok);
    default:
      if (c == '-' || (c >= '0' && c <= '9')) {
        return parse_number(ok);
      }
      *ok = fail("unexpected character");
      return Value::null();
    }
  }

  bool match_literal(const char *literal) {
    const std::size_t len = std::char_traits<char>::length(literal);
    if (src.compare(pos, len, literal) == 0) {
      pos += len;
      return true;
    }
    return false;
  }

  Value parse_bool(bool *ok) {
    if (match_literal("true")) {
      return Value::boolean(true);
    }
    if (match_literal("false")) {
      return Value::boolean(false);
    }
    *ok = fail("invalid literal");
    return Value::null();
  }

  Value parse_null(bool *ok) {
    if (match_literal("null")) {
      return Value::null();
    }
    *ok = fail("invalid literal");
    return Value::null();
  }

  // Parses a JSON string body (cursor at the opening quote) into `out`.
  bool parse_string(std::string *out) {
    ++pos; // opening quote
    while (pos < src.size()) {
      const unsigned char c = static_cast<unsigned char>(src[pos]);
      if (c == '"') {
        ++pos;
        return true;
      }
      if (c == '\\') {
        ++pos;
        if (pos >= src.size()) {
          return fail("unterminated escape");
        }
        const char e = src[pos];
        switch (e) {
        case '"':
          out->push_back('"');
          break;
        case '\\':
          out->push_back('\\');
          break;
        case '/':
          out->push_back('/');
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          std::uint32_t cp = 0;
          if (!parse_hex4(&cp)) {
            return false;
          }
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate: must be followed by \uDC00-\uDFFF.
            if (pos + 2 <= src.size() && src[pos + 1] == '\\' &&
                src[pos + 2] == 'u') {
              pos += 2;
              std::uint32_t low = 0;
              if (!parse_hex4(&low)) {
                return false;
              }
              if (low < 0xDC00 || low > 0xDFFF) {
                return fail("invalid low surrogate");
              }
              cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else {
              return fail("unpaired high surrogate");
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return fail("unpaired low surrogate");
          }
          encode_utf8(cp, *out);
          break; // fall through to the `++pos` past the last hex digit
        }
        default:
          return fail("invalid escape");
        }
        ++pos;
        continue;
      }
      if (c < 0x20) {
        return fail("unescaped control character in string");
      }
      out->push_back(static_cast<char>(c));
      ++pos;
    }
    return fail("unterminated string");
  }

  // Reads exactly 4 hex digits following a `\u`; leaves pos on the last digit.
  bool parse_hex4(std::uint32_t *out) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      ++pos;
      if (pos >= src.size()) {
        return fail("truncated \\u escape");
      }
      const char c = src[pos];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return fail("invalid hex digit in \\u escape");
      }
    }
    *out = value;
    return true;
  }

  Value parse_number(bool *ok) {
    const std::size_t start = pos;
    bool is_float = false;
    if (pos < src.size() && src[pos] == '-') {
      ++pos;
    }
    if (pos < src.size() && src[pos] == '0') {
      ++pos;
    } else if (pos < src.size() && src[pos] >= '1' && src[pos] <= '9') {
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
        ++pos;
      }
    } else {
      *ok = fail("invalid number");
      return Value::null();
    }
    if (pos < src.size() && src[pos] == '.') {
      is_float = true;
      ++pos;
      if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
        *ok = fail("invalid number: digits expected after decimal point");
        return Value::null();
      }
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
        ++pos;
      }
    }
    if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
      is_float = true;
      ++pos;
      if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) {
        ++pos;
      }
      if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
        *ok = fail("invalid number: digits expected in exponent");
        return Value::null();
      }
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
        ++pos;
      }
    }
    const std::string token = src.substr(start, pos - start);
    if (!is_float) {
      errno = 0;
      char *end = nullptr;
      const long long parsed = std::strtoll(token.c_str(), &end, 10);
      if (errno == 0 && end == token.c_str() + token.size()) {
        return Value::integer(static_cast<std::int64_t>(parsed));
      }
      // Out-of-i64-range integer: fall back to Float (lossy) per design D4.
    }
    return Value::floating(std::strtod(token.c_str(), nullptr));
  }

  Value parse_array(int depth, bool *ok) {
    ++pos; // '['
    std::vector<Value> items;
    skip_ws();
    if (pos < src.size() && src[pos] == ']') {
      ++pos;
      return call.make_list(std::move(items));
    }
    while (true) {
      const Value item = parse_value(depth + 1, ok);
      if (!*ok) {
        return Value::null();
      }
      items.push_back(item);
      skip_ws();
      if (pos >= src.size()) {
        *ok = fail("unterminated array");
        return Value::null();
      }
      if (src[pos] == ',') {
        ++pos;
        continue;
      }
      if (src[pos] == ']') {
        ++pos;
        return call.make_list(std::move(items));
      }
      *ok = fail("expected ',' or ']' in array");
      return Value::null();
    }
  }

  Value parse_object(int depth, bool *ok) {
    ++pos; // '{'
    std::vector<std::pair<std::string, Value>> entries;
    skip_ws();
    if (pos < src.size() && src[pos] == '}') {
      ++pos;
      return call.make_object(std::move(entries), strict_maps);
    }
    while (true) {
      skip_ws();
      if (pos >= src.size() || src[pos] != '"') {
        *ok = fail("expected string key in object");
        return Value::null();
      }
      std::string key;
      if (!parse_string(&key)) {
        *ok = false;
        return Value::null();
      }
      skip_ws();
      if (pos >= src.size() || src[pos] != ':') {
        *ok = fail("expected ':' after object key");
        return Value::null();
      }
      ++pos;
      const Value value = parse_value(depth + 1, ok);
      if (!*ok) {
        return Value::null();
      }
      entries.emplace_back(std::move(key), value);
      skip_ws();
      if (pos >= src.size()) {
        *ok = fail("unterminated object");
        return Value::null();
      }
      if (src[pos] == ',') {
        ++pos;
        continue;
      }
      if (src[pos] == '}') {
        ++pos;
        return call.make_object(std::move(entries), strict_maps);
      }
      *ok = fail("expected ',' or '}' in object");
      return Value::null();
    }
  }

  // Parse a whole document: a single value with only trailing whitespace.
  bool parse_document(Value *out) {
    bool ok = true;
    const Value value = parse_value(0, &ok);
    if (!ok) {
      return false;
    }
    skip_ws();
    if (pos != src.size()) {
      return fail("trailing characters after JSON value");
    }
    *out = value;
    return true;
  }
};

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

SendStatus json_parse(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1)) {
    return SendStatus::Faulted;
  }
  if (!call.reject_unknown_keywords({"strict"})) {
    return SendStatus::Faulted;
  }
  bool strict = false;
  if (!call.bool_keyword("strict", false, &strict)) {
    return SendStatus::Faulted;
  }
  const std::optional<std::string> text = call.text_of(call.args[0]);
  if (!text.has_value()) {
    return call.fault("TypeError", "Json.parse expects a Str");
  }
  Parser parser{call, *text};
  parser.strict_maps = strict;
  Value parsed = Value::null();
  if (!parser.parse_document(&parsed)) {
    return SendStatus::Faulted;
  }
  *call.out = parsed;
  return SendStatus::Matched;
}

SendStatus json_generate(NativeStdlibCall &call, bool pretty) {
  if (!call.require_no_block() || !call.require_arity(1)) {
    return SendStatus::Faulted;
  }
  if (!call.reject_unknown_keywords(pretty ? std::initializer_list<const char *>{
                                                 "indent"}
                                           : std::initializer_list<const char *>{})) {
    return SendStatus::Faulted;
  }
  Generator generator{call};
  generator.pretty = pretty;
  if (pretty) {
    const std::optional<Value> indent = call.keyword("indent");
    if (indent.has_value()) {
      if (!indent->is_integer() || indent->as_integer() < 0 ||
          indent->as_integer() > 16) {
        return call.fault("ArgumentError", "indent must be an Int in 0..16");
      }
      generator.indent_width = static_cast<int>(indent->as_integer());
    }
  }
  if (!generator.emit(call.args[0], 0)) {
    return SendStatus::Faulted;
  }
  *call.out = call.string_value(std::move(generator.out));
  return SendStatus::Matched;
}

SendStatus json_dispatch(NativeStdlibCall &call) {
  const std::string &selector = call.selector;
  if (selector == "parse") {
    return json_parse(call);
  }
  if (selector == "generate") {
    return json_generate(call, /*pretty=*/false);
  }
  if (selector == "pretty_generate") {
    return json_generate(call, /*pretty=*/true);
  }
  return SendStatus::NotHandled;
}

} // namespace

void register_json(NativeRegistry &registry) {
  registry.register_path("Json", RuntimeNativeTypeKind::Json);
  registry.register_handler(RuntimeNativeTypeKind::Json, &json_dispatch);
}

} // namespace amber::runtime
