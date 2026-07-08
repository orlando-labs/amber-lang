// Yaml — YAML parse/generate stdlib library on the Layer 0 substrate
// (DESIGN-stdlib-next-libs-order-2026-06-15 §4).
//
// v1 surface implemented here: `Yaml.parse`, `Yaml.generate`,
// `Yaml.load_from_file`, `Yaml.save_to_file`. Pure compute against a pragmatic
// YAML subset: block mappings and sequences resolved by indentation, flow
// collections (`[...]`, `{...}`), single/double quoted and plain scalars typed
// per the YAML core schema (null / bool / int / float / str), `#` comments and
// leading `---` / trailing `...` document markers. Malformed input faults
// `YamlParseError`; non-representable values fault `YamlGenerateError`.
//
// The generator emits block style: nested collections hang under their `key:`
// or `-` on the following indented lines, empty collections render inline as
// `{}` / `[]`. Its output is a fixed point of the parser (round-trips), and it
// mirrors the native-lane emitter in tools/amberc/main.cpp so both backends
// produce byte-identical results.

#include "runtime/stdlib_registry.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace amber::runtime {

namespace {

constexpr int kMaxDepth = 256;

// ---------------------------------------------------------------------------
// Shared scalar helpers
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

std::string rstrip(const std::string &text) {
  std::size_t end = text.size();
  while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
    --end;
  }
  return text.substr(0, end);
}

std::string strip(const std::string &text) {
  std::size_t begin = 0;
  while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

// True when `text` (already trimmed) resolves as a YAML plain string rather
// than null/bool/int/float — used by both the parser and the generator's
// quoting decision so the two agree exactly.
bool matches_integer(const std::string &s) {
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    ++i;
  }
  if (i >= s.size()) {
    return false;
  }
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
  }
  return true;
}

bool matches_float(const std::string &s) {
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    ++i;
  }
  std::size_t digits_before = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    ++i;
    ++digits_before;
  }
  bool has_dot = false;
  std::size_t digits_after = 0;
  if (i < s.size() && s[i] == '.') {
    has_dot = true;
    ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
      ++digits_after;
    }
  }
  bool has_exp = false;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    has_exp = true;
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
      ++i;
    }
    std::size_t exp_digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
      ++exp_digits;
    }
    if (exp_digits == 0) {
      return false;
    }
  }
  if (i != s.size()) {
    return false;
  }
  if (!has_dot && !has_exp) {
    return false; // a pure integer, handled by matches_integer
  }
  if (digits_before == 0 && digits_after == 0) {
    return false; // "." / ".e5" / "+"
  }
  return true;
}

Value integer_or_float(const std::string &s) {
  const bool negative = s[0] == '-';
  const std::size_t digits_begin = (s[0] == '-' || s[0] == '+') ? 1U : 0U;
  constexpr std::uint64_t kInt64Max =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  const std::uint64_t limit = negative ? kInt64Max + 1U : kInt64Max;
  std::uint64_t magnitude = 0;
  bool overflow = false;
  for (std::size_t i = digits_begin; i < s.size(); ++i) {
    const std::uint64_t digit = static_cast<std::uint64_t>(s[i] - '0');
    if (magnitude > (limit - digit) / 10U) {
      overflow = true;
      break;
    }
    magnitude = magnitude * 10U + digit;
  }
  if (!overflow) {
    if (negative) {
      if (magnitude == limit) {
        return Value::integer(std::numeric_limits<std::int64_t>::min());
      }
      return Value::integer(-static_cast<std::int64_t>(magnitude));
    }
    return Value::integer(static_cast<std::int64_t>(magnitude));
  }
  return Value::floating(std::strtod(s.c_str(), nullptr));
}

// Resolve an unquoted plain scalar to its core-schema type.
Value classify_plain(NativeStdlibCall &call, const std::string &raw) {
  const std::string s = strip(raw);
  if (s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL") {
    return Value::null();
  }
  if (s == "true" || s == "True" || s == "TRUE") {
    return Value::boolean(true);
  }
  if (s == "false" || s == "False" || s == "FALSE") {
    return Value::boolean(false);
  }
  if (matches_integer(s)) {
    return integer_or_float(s);
  }
  if (matches_float(s)) {
    return Value::floating(std::strtod(s.c_str(), nullptr));
  }
  return call.string_value(s);
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

struct Line {
  std::size_t indent = 0;
  std::string content; // leading indentation removed, trailing ws stripped
  std::size_t number = 0;
};

struct YamlParser {
  YamlParser(NativeStdlibCall &call_in, const std::string &src_in)
      : call(call_in), src(src_in) {}

  NativeStdlibCall &call;
  const std::string &src;
  std::vector<Line> lines;
  std::size_t cursor = 0;
  bool faulted = false;

  Value fail(const std::string &message) {
    call.fault("YamlParseError", message);
    faulted = true;
    return Value::null();
  }

  // Index at which a trailing `#` comment starts (npos if none), skipping
  // quoted spans. A `#` only opens a comment at the start or after whitespace.
  static std::size_t comment_index(const std::string &s) {
    std::size_t i = 0;
    while (i < s.size()) {
      const char c = s[i];
      if (c == '"') {
        ++i;
        while (i < s.size() && s[i] != '"') {
          if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
          }
          ++i;
        }
        if (i < s.size()) {
          ++i; // closing quote
        }
        continue;
      }
      if (c == '\'') {
        ++i;
        while (i < s.size()) {
          if (s[i] == '\'') {
            if (i + 1 < s.size() && s[i + 1] == '\'') {
              i += 2;
              continue;
            }
            ++i;
            break;
          }
          ++i;
        }
        continue;
      }
      if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
        return i;
      }
      ++i;
    }
    return std::string::npos;
  }

  static std::string without_comment(const std::string &s) {
    const std::size_t idx = comment_index(s);
    if (idx == std::string::npos) {
      return s;
    }
    return s.substr(0, idx);
  }

  // Split the source into significant lines (blank and comment-only lines
  // dropped), honouring the leading `---` document marker. Rejects tabs used
  // for indentation and multi-document streams.
  bool tokenize() {
    std::size_t line_start = 0;
    std::size_t line_no = 0;
    bool seen_content = false;
    while (line_start <= src.size()) {
      std::size_t line_end = src.find('\n', line_start);
      const bool last = line_end == std::string::npos;
      if (last) {
        line_end = src.size();
      }
      ++line_no;
      std::string raw = src.substr(line_start, line_end - line_start);
      if (!raw.empty() && raw.back() == '\r') {
        raw.pop_back();
      }
      std::size_t indent = 0;
      while (indent < raw.size() && raw[indent] == ' ') {
        ++indent;
      }
      if (indent < raw.size() && raw[indent] == '\t') {
        (void)fail("tabs are not allowed for indentation at line " +
                   std::to_string(line_no));
        return false;
      }
      std::string content = rstrip(raw.substr(indent));
      const bool blank = content.empty();
      const bool comment_only = !content.empty() && content[0] == '#';
      if (content == "---") {
        if (seen_content) {
          (void)fail("multiple YAML documents are not supported (line " +
                     std::to_string(line_no) + ")");
          return false;
        }
        // Leading document marker: reset and continue.
      } else if (content == "...") {
        break; // end of document
      } else if (!blank && !comment_only) {
        Line line;
        line.indent = indent;
        line.content = content;
        line.number = line_no;
        lines.push_back(std::move(line));
        seen_content = true;
      }
      if (last) {
        break;
      }
      line_start = line_end + 1;
    }
    return true;
  }

  bool is_dash(const std::string &c) const {
    return c == "-" || (c.size() >= 2 && c[0] == '-' && c[1] == ' ');
  }

  // Position of the `key:` colon at flow depth 0 (npos if the line is not a
  // mapping entry). Honours quotes and flow nesting; a `:` counts only when
  // it ends the content or is followed by a space.
  static std::size_t key_colon(const std::string &s) {
    int depth = 0;
    std::size_t i = 0;
    while (i < s.size()) {
      const char c = s[i];
      if (c == '"') {
        ++i;
        while (i < s.size() && s[i] != '"') {
          if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
          }
          ++i;
        }
        if (i < s.size()) {
          ++i;
        }
        continue;
      }
      if (c == '\'') {
        ++i;
        while (i < s.size()) {
          if (s[i] == '\'') {
            if (i + 1 < s.size() && s[i + 1] == '\'') {
              i += 2;
              continue;
            }
            ++i;
            break;
          }
          ++i;
        }
        continue;
      }
      if (c == '[' || c == '{') {
        ++depth;
      } else if (c == ']' || c == '}') {
        if (depth > 0) {
          --depth;
        }
      } else if (c == '#' && depth == 0 &&
                 (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
        return std::string::npos; // comment before any key colon
      } else if (c == ':' && depth == 0 &&
                 (i + 1 == s.size() || s[i + 1] == ' ' || s[i + 1] == '\t')) {
        return i;
      }
      ++i;
    }
    return std::string::npos;
  }

  Value parse_node(std::size_t indent, int depth) {
    if (depth > kMaxDepth) {
      return fail("YAML nesting is too deep");
    }
    if (cursor >= lines.size()) {
      return Value::null();
    }
    const std::string &c = lines[cursor].content;
    if (!c.empty() && (c[0] == '[' || c[0] == '{')) {
      Value value = parse_flow_line(c);
      ++cursor;
      return value;
    }
    if (is_dash(c)) {
      return parse_sequence(indent, depth);
    }
    if (key_colon(c) != std::string::npos) {
      return parse_mapping(indent, depth);
    }
    // Bare single-line scalar node.
    Value value = parse_scalar(c);
    ++cursor;
    return value;
  }

  Value parse_mapping(std::size_t indent, int depth) {
    std::vector<std::pair<std::string, Value>> entries;
    while (cursor < lines.size() && lines[cursor].indent == indent) {
      const std::string content = lines[cursor].content;
      if (is_dash(content)) {
        break;
      }
      const std::size_t colon = key_colon(content);
      if (colon == std::string::npos) {
        return fail("expected `key:` in mapping at line " +
                    std::to_string(lines[cursor].number));
      }
      std::string key;
      if (!parse_key(content.substr(0, colon), &key)) {
        return Value::null();
      }
      const std::string rest = strip(without_comment(content.substr(colon + 1)));
      ++cursor;
      Value value;
      if (rest.empty()) {
        if (cursor < lines.size() && lines[cursor].indent > indent) {
          value = parse_node(lines[cursor].indent, depth + 1);
        } else if (cursor < lines.size() && lines[cursor].indent == indent &&
                   is_dash(lines[cursor].content)) {
          // A block sequence may sit at the parent key's indentation.
          value = parse_sequence(indent, depth + 1);
        } else {
          value = Value::null();
        }
      } else {
        value = parse_scalar(rest);
      }
      if (faulted) {
        return Value::null();
      }
      entries.emplace_back(std::move(key), value);
    }
    return call.make_object(std::move(entries), /*strict=*/false);
  }

  Value parse_sequence(std::size_t indent, int depth) {
    std::vector<Value> items;
    while (cursor < lines.size() && lines[cursor].indent == indent &&
           is_dash(lines[cursor].content)) {
      const std::string content = lines[cursor].content;
      std::size_t pos = 1;
      while (pos < content.size() && content[pos] == ' ') {
        ++pos;
      }
      const std::string after = content.substr(pos);
      const std::size_t after_col = indent + pos;
      const std::string after_stripped = strip(without_comment(after));
      if (after_stripped.empty()) {
        ++cursor;
        if (cursor < lines.size() && lines[cursor].indent > indent) {
          items.push_back(parse_node(lines[cursor].indent, depth + 1));
        } else {
          items.push_back(Value::null());
        }
      } else {
        // Re-dispatch the inline content as a node beginning at `after_col`;
        // continuation lines (e.g. sibling mapping keys) align to that column.
        lines[cursor].indent = after_col;
        lines[cursor].content = after;
        items.push_back(parse_node(after_col, depth + 1));
      }
      if (faulted) {
        return Value::null();
      }
    }
    return call.make_list(std::move(items));
  }

  bool parse_key(const std::string &raw, std::string *out) {
    const std::string s = strip(raw);
    if (!s.empty() && s[0] == '"') {
      std::size_t consumed = 0;
      if (!parse_double_quoted(s, &consumed, out)) {
        return false;
      }
      return true;
    }
    if (!s.empty() && s[0] == '\'') {
      std::size_t consumed = 0;
      if (!parse_single_quoted(s, &consumed, out)) {
        return false;
      }
      return true;
    }
    *out = s;
    return true;
  }

  // Parse a scalar in value position: flow collection, quoted string, or plain.
  Value parse_scalar(const std::string &raw) {
    const std::string s = strip(raw);
    if (s.empty()) {
      return Value::null();
    }
    if (s[0] == '[' || s[0] == '{') {
      return parse_flow_line(s);
    }
    if (s[0] == '"') {
      std::size_t consumed = 0;
      std::string text;
      if (!parse_double_quoted(s, &consumed, &text)) {
        return Value::null();
      }
      return call.string_value(std::move(text));
    }
    if (s[0] == '\'') {
      std::size_t consumed = 0;
      std::string text;
      if (!parse_single_quoted(s, &consumed, &text)) {
        return Value::null();
      }
      return call.string_value(std::move(text));
    }
    return classify_plain(call, without_comment(s));
  }

  bool parse_double_quoted(const std::string &s, std::size_t *consumed,
                           std::string *out) {
    std::size_t i = 1; // past opening quote
    while (i < s.size()) {
      const char c = s[i];
      if (c == '"') {
        *consumed = i + 1;
        return true;
      }
      if (c == '\\') {
        ++i;
        if (i >= s.size()) {
          (void)fail("unterminated escape in double-quoted scalar");
          return false;
        }
        const char e = s[i];
        switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case '0': out->push_back('\0'); break;
        case 'a': out->push_back('\a'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'v': out->push_back('\v'); break;
        case 'e': out->push_back('\x1b'); break;
        case ' ': out->push_back(' '); break;
        case 'u': {
          std::uint32_t cp = 0;
          if (!parse_hex(s, &i, 4, &cp)) {
            return false;
          }
          encode_utf8(cp, *out);
          break;
        }
        case 'x': {
          std::uint32_t cp = 0;
          if (!parse_hex(s, &i, 2, &cp)) {
            return false;
          }
          encode_utf8(cp, *out);
          break;
        }
        default:
          (void)fail("invalid escape in double-quoted scalar");
          return false;
        }
        ++i;
        continue;
      }
      out->push_back(c);
      ++i;
    }
    (void)fail("unterminated double-quoted scalar");
    return false;
  }

  bool parse_hex(const std::string &s, std::size_t *i, int count,
                 std::uint32_t *out) {
    std::uint32_t value = 0;
    for (int n = 0; n < count; ++n) {
      ++(*i);
      if (*i >= s.size()) {
        (void)fail("truncated \\u/\\x escape");
        return false;
      }
      const char c = s[*i];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        (void)fail("invalid hex digit in escape");
        return false;
      }
    }
    *out = value;
    return true;
  }

  bool parse_single_quoted(const std::string &s, std::size_t *consumed,
                           std::string *out) {
    std::size_t i = 1; // past opening quote
    while (i < s.size()) {
      const char c = s[i];
      if (c == '\'') {
        if (i + 1 < s.size() && s[i + 1] == '\'') {
          out->push_back('\'');
          i += 2;
          continue;
        }
        *consumed = i + 1;
        return true;
      }
      out->push_back(c);
      ++i;
    }
    (void)fail("unterminated single-quoted scalar");
    return false;
  }

  // ----- flow collections (single logical line) ---------------------------

  Value parse_flow_line(const std::string &s) {
    std::size_t pos = 0;
    Value value = parse_flow_node(s, &pos, 0);
    if (faulted) {
      return Value::null();
    }
    skip_spaces(s, &pos);
    // Only a trailing comment may follow the flow collection.
    if (pos < s.size() && s[pos] != '#') {
      return fail("unexpected content after flow collection");
    }
    return value;
  }

  static void skip_spaces(const std::string &s, std::size_t *pos) {
    while (*pos < s.size() && (s[*pos] == ' ' || s[*pos] == '\t')) {
      ++(*pos);
    }
  }

  Value parse_flow_node(const std::string &s, std::size_t *pos, int depth) {
    if (depth > kMaxDepth) {
      return fail("YAML flow nesting is too deep");
    }
    skip_spaces(s, pos);
    if (*pos >= s.size()) {
      return fail("unexpected end of flow scalar");
    }
    const char c = s[*pos];
    if (c == '[') {
      return parse_flow_seq(s, pos, depth);
    }
    if (c == '{') {
      return parse_flow_map(s, pos, depth);
    }
    return parse_flow_scalar(s, pos, /*as_key=*/false);
  }

  Value parse_flow_seq(const std::string &s, std::size_t *pos, int depth) {
    ++(*pos); // '['
    std::vector<Value> items;
    skip_spaces(s, pos);
    if (*pos < s.size() && s[*pos] == ']') {
      ++(*pos);
      return call.make_list(std::move(items));
    }
    while (true) {
      Value item = parse_flow_node(s, pos, depth + 1);
      if (faulted) {
        return Value::null();
      }
      items.push_back(item);
      skip_spaces(s, pos);
      if (*pos >= s.size()) {
        return fail("unterminated flow sequence");
      }
      if (s[*pos] == ',') {
        ++(*pos);
        skip_spaces(s, pos);
        if (*pos < s.size() && s[*pos] == ']') {
          ++(*pos); // trailing comma allowed
          return call.make_list(std::move(items));
        }
        continue;
      }
      if (s[*pos] == ']') {
        ++(*pos);
        return call.make_list(std::move(items));
      }
      return fail("expected ',' or ']' in flow sequence");
    }
  }

  Value parse_flow_map(const std::string &s, std::size_t *pos, int depth) {
    ++(*pos); // '{'
    std::vector<std::pair<std::string, Value>> entries;
    skip_spaces(s, pos);
    if (*pos < s.size() && s[*pos] == '}') {
      ++(*pos);
      return call.make_object(std::move(entries), /*strict=*/false);
    }
    while (true) {
      skip_spaces(s, pos);
      Value key_value = parse_flow_scalar(s, pos, /*as_key=*/true);
      if (faulted) {
        return Value::null();
      }
      const std::optional<std::string> key = call.text_of(key_value);
      if (!key.has_value()) {
        return fail("flow mapping keys must be scalars");
      }
      skip_spaces(s, pos);
      if (*pos >= s.size() || s[*pos] != ':') {
        return fail("expected ':' in flow mapping");
      }
      ++(*pos);
      Value value = parse_flow_node(s, pos, depth + 1);
      if (faulted) {
        return Value::null();
      }
      entries.emplace_back(*key, value);
      skip_spaces(s, pos);
      if (*pos >= s.size()) {
        return fail("unterminated flow mapping");
      }
      if (s[*pos] == ',') {
        ++(*pos);
        skip_spaces(s, pos);
        if (*pos < s.size() && s[*pos] == '}') {
          ++(*pos); // trailing comma allowed
          return call.make_object(std::move(entries), /*strict=*/false);
        }
        continue;
      }
      if (s[*pos] == '}') {
        ++(*pos);
        return call.make_object(std::move(entries), /*strict=*/false);
      }
      return fail("expected ',' or '}' in flow mapping");
    }
  }

  Value parse_flow_scalar(const std::string &s, std::size_t *pos, bool as_key) {
    skip_spaces(s, pos);
    if (*pos >= s.size()) {
      return fail("unexpected end of flow scalar");
    }
    if (s[*pos] == '"') {
      std::string text;
      std::size_t consumed = 0;
      if (!parse_double_quoted(s.substr(*pos), &consumed, &text)) {
        return Value::null();
      }
      *pos += consumed;
      return call.string_value(std::move(text));
    }
    if (s[*pos] == '\'') {
      std::string text;
      std::size_t consumed = 0;
      if (!parse_single_quoted(s.substr(*pos), &consumed, &text)) {
        return Value::null();
      }
      *pos += consumed;
      return call.string_value(std::move(text));
    }
    const std::size_t begin = *pos;
    while (*pos < s.size()) {
      const char c = s[*pos];
      if (c == ',' || c == ']' || c == '}') {
        break;
      }
      if (as_key && c == ':') {
        break;
      }
      ++(*pos);
    }
    const std::string token = strip(s.substr(begin, *pos - begin));
    if (as_key) {
      return call.string_value(token);
    }
    return classify_plain(call, token);
  }

  Value parse_document() {
    if (!tokenize()) {
      return Value::null();
    }
    if (lines.empty()) {
      return Value::null();
    }
    Value value = parse_node(lines.front().indent, 0);
    if (faulted) {
      return Value::null();
    }
    if (cursor != lines.size()) {
      return fail("unexpected content at line " +
                  std::to_string(lines[cursor].number));
    }
    return value;
  }
};

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------

// Shortest decimal that round-trips, guaranteeing an integral-valued Float
// keeps a `.0` so it re-parses as Float rather than Int.
std::string format_double(double d) {
  if (std::isnan(d) || std::isinf(d)) {
    return std::string();
  }
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

// A plain scalar string is safe to emit unquoted only when it cannot be
// confused with another type or a structural indicator.
bool needs_quote(const std::string &s) {
  if (s.empty()) {
    return true;
  }
  // Ambiguous with null/bool/number.
  if (s == "~" || s == "null" || s == "Null" || s == "NULL" || s == "true" ||
      s == "True" || s == "TRUE" || s == "false" || s == "False" ||
      s == "FALSE") {
    return true;
  }
  if (matches_integer(s) || matches_float(s)) {
    return true;
  }
  if (s.front() == ' ' || s.back() == ' ' || s.front() == '\t' ||
      s.back() == '\t') {
    return true;
  }
  const char first = s.front();
  switch (first) {
  case '!':
  case '&':
  case '*':
  case '[':
  case ']':
  case '{':
  case '}':
  case ',':
  case '#':
  case '|':
  case '>':
  case '@':
  case '`':
  case '"':
  case '\'':
  case '%':
  case '?':
  case ':':
    return true;
  default:
    break;
  }
  if (first == '-' && (s.size() == 1 || s[1] == ' ')) {
    return true;
  }
  for (std::size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20) {
      return true;
    }
    if (c == ':' && (i + 1 == s.size() || s[i + 1] == ' ')) {
      return true;
    }
    if (c == '#' && i > 0 && s[i - 1] == ' ') {
      return true;
    }
  }
  return false;
}

void write_quoted(const std::string &text, std::string &out) {
  out.push_back('"');
  for (const unsigned char c : text) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\t': out += "\\t"; break;
    case '\r': out += "\\r"; break;
    default:
      if (c < 0x20) {
        char esc[8];
        std::snprintf(esc, sizeof(esc), "\\u%04x", c);
        out += esc;
      } else {
        out.push_back(static_cast<char>(c));
      }
    }
  }
  out.push_back('"');
}

struct Generator {
  explicit Generator(NativeStdlibCall &call_in) : call(call_in) {}

  NativeStdlibCall &call;
  std::string out;
  bool faulted = false;

  bool fail(const std::string &message) {
    call.fault("YamlGenerateError", message);
    faulted = true;
    return false;
  }

  static bool is_nonempty_map(const Value &value) {
    if (!value.is_map()) {
      return false;
    }
    const IntrusivePtr<MapValue> map = value.as_map();
    return map != nullptr && !map->entries.empty();
  }

  static bool is_nonempty_list(const Value &value) {
    if (!value.is_list()) {
      return false;
    }
    const IntrusivePtr<ListValue> list = value.as_list();
    return list != nullptr && !list->items.empty();
  }

  void indent(int depth) { out.append(static_cast<std::size_t>(depth) * 2, ' '); }

  bool scalar_text(const Value &value, std::string *text) {
    if (value.is_null()) {
      *text = "null";
      return true;
    }
    if (value.is_bool()) {
      *text = value.as_bool() ? "true" : "false";
      return true;
    }
    if (value.is_integer()) {
      *text = std::to_string(value.as_integer());
      return true;
    }
    if (value.is_float()) {
      const double d = value.as_float();
      if (std::isnan(d) || std::isinf(d)) {
        return fail("NaN and Infinity are not representable in YAML");
      }
      *text = format_double(d);
      return true;
    }
    if (value.is_string() || value.is_symbol()) {
      const std::string s = call.text_of(value).value_or("");
      if (needs_quote(s)) {
        std::string quoted;
        write_quoted(s, quoted);
        *text = std::move(quoted);
      } else {
        *text = s;
      }
      return true;
    }
    return fail("value is not representable as a YAML scalar");
  }

  // Emit a value that is known to fit on the current line (scalar or empty
  // collection); the caller has already written the `key: ` / `- ` prefix.
  bool emit_inline(const Value &value) {
    if (value.is_map()) {
      out += "{}";
      return true;
    }
    if (value.is_list()) {
      out += "[]";
      return true;
    }
    std::string text;
    if (!scalar_text(value, &text)) {
      return false;
    }
    out += text;
    return true;
  }

  bool emit_key(const Value &key) {
    if (!key.is_string() && !key.is_symbol()) {
      return fail("YAML mapping keys must be Str or Symbol");
    }
    const std::string s = call.text_of(key).value_or("");
    if (needs_quote(s)) {
      write_quoted(s, out);
    } else {
      out += s;
    }
    return true;
  }

  bool emit_map(const Value &value, int depth) {
    const IntrusivePtr<MapValue> map = value.as_map();
    if (map == nullptr) {
      return fail("map value is null");
    }
    for (const MapEntry &entry : map->entries) {
      indent(depth);
      if (!emit_key(entry.key)) {
        return false;
      }
      out.push_back(':');
      if (is_nonempty_map(entry.value)) {
        out.push_back('\n');
        if (!emit_map(entry.value, depth + 1)) {
          return false;
        }
      } else if (is_nonempty_list(entry.value)) {
        out.push_back('\n');
        if (!emit_seq(entry.value, depth + 1)) {
          return false;
        }
      } else {
        out.push_back(' ');
        if (!emit_inline(entry.value)) {
          return false;
        }
        out.push_back('\n');
      }
    }
    return true;
  }

  bool emit_seq(const Value &value, int depth) {
    const IntrusivePtr<ListValue> list = value.as_list();
    if (list == nullptr) {
      return fail("list value is null");
    }
    for (const Value &item : list->items) {
      indent(depth);
      out.push_back('-');
      if (is_nonempty_map(item)) {
        out.push_back('\n');
        if (!emit_map(item, depth + 1)) {
          return false;
        }
      } else if (is_nonempty_list(item)) {
        out.push_back('\n');
        if (!emit_seq(item, depth + 1)) {
          return false;
        }
      } else {
        out.push_back(' ');
        if (!emit_inline(item)) {
          return false;
        }
        out.push_back('\n');
      }
    }
    return true;
  }

  bool emit_document(const Value &value) {
    if (is_nonempty_map(value)) {
      return emit_map(value, 0);
    }
    if (is_nonempty_list(value)) {
      return emit_seq(value, 0);
    }
    std::string text;
    if (value.is_map()) {
      out += "{}\n";
      return true;
    }
    if (value.is_list()) {
      out += "[]\n";
      return true;
    }
    if (!scalar_text(value, &text)) {
      return false;
    }
    out += text;
    out.push_back('\n');
    return true;
  }
};

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

bool generate_yaml_text(NativeStdlibCall &call, const Value &value,
                        std::string *out) {
  Generator generator{call};
  if (!generator.emit_document(value)) {
    return false;
  }
  *out = std::move(generator.out);
  return true;
}

SendStatus yaml_parse(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const std::optional<std::string> text = call.text_of(call.args[0]);
  if (!text.has_value()) {
    return call.fault("TypeError", "Yaml.parse expects a Str");
  }
  YamlParser parser{call, *text};
  const Value value = parser.parse_document();
  if (parser.faulted) {
    return SendStatus::Faulted;
  }
  *call.out = value;
  return SendStatus::Matched;
}

SendStatus yaml_generate(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  std::string text;
  if (!generate_yaml_text(call, call.args[0], &text)) {
    return SendStatus::Faulted;
  }
  *call.out = call.string_value(std::move(text));
  return SendStatus::Matched;
}

SendStatus yaml_load_from_file(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const std::optional<std::string> path = call.text_of(call.args[0]);
  if (!path.has_value()) {
    return call.fault("TypeError", "Yaml.load_from_file expects a path Str");
  }
  std::string text;
  if (!call.fs_read_text(*path, &text)) {
    return SendStatus::Faulted;
  }
  YamlParser parser{call, text};
  const Value value = parser.parse_document();
  if (parser.faulted) {
    return SendStatus::Faulted;
  }
  *call.out = value;
  return SendStatus::Matched;
}

SendStatus yaml_save_to_file(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(2) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const std::optional<std::string> path = call.text_of(call.args[0]);
  if (!path.has_value()) {
    return call.fault("TypeError", "Yaml.save_to_file expects a path Str");
  }
  std::string text;
  if (!generate_yaml_text(call, call.args[1], &text)) {
    return SendStatus::Faulted;
  }
  if (!call.fs_write_text(*path, text)) {
    return SendStatus::Faulted;
  }
  *call.out = Value::null();
  return SendStatus::Matched;
}

SendStatus yaml_dispatch(NativeStdlibCall &call) {
  const std::string &selector = call.selector;
  if (selector == "parse") {
    return yaml_parse(call);
  }
  if (selector == "generate") {
    return yaml_generate(call);
  }
  if (selector == "load_from_file") {
    return yaml_load_from_file(call);
  }
  if (selector == "save_to_file") {
    return yaml_save_to_file(call);
  }
  return SendStatus::NotHandled;
}

RuntimeNativeModuleDescriptor yaml_module_descriptor() {
  return {{{"Yaml", RuntimeNativeTypeKind::Yaml}},
          {{RuntimeNativeTypeKind::Yaml, &yaml_dispatch}},
          {},
          {},
          {{"YamlError", "Exception"},
           {"YamlParseError", "YamlError"},
           {"YamlGenerateError", "YamlError"}}};
}

} // namespace

void register_yaml(NativeRegistry &registry) {
  register_native_module_descriptor(registry, yaml_module_descriptor());
}

void register_yaml_runtime_module(RuntimeModuleRegistry &modules,
                                  RuntimeDispatchRegistry &dispatch,
                                  RuntimeTypeRegistry &types,
                                  RuntimeErrorRegistry *errors) {
  const RuntimeNativeModuleDescriptor descriptor = yaml_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
  if (errors != nullptr) {
    register_runtime_error_descriptor(*errors, descriptor);
  }
}

} // namespace amber::runtime
