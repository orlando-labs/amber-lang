#include "buildsys/build.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace amber::build {

namespace {

struct JsonValue {
  enum class Kind { Null, Bool, String, Array, Object };

  Kind kind = Kind::Null;
  bool bool_value = false;
  std::string string_value;
  std::vector<JsonValue> array_value;
  std::map<std::string, JsonValue> object_value;
};

BuildDiagnostic diagnostic(std::string error_name, std::string message,
                           std::string path = {}) {
  BuildDiagnostic out;
  out.error_name = std::move(error_name);
  out.message = std::move(message);
  out.path = std::move(path);
  return out;
}

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (const char c : value) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      out << c;
      break;
    }
  }
  return out.str();
}

class JsonParser {
public:
  JsonParser(const std::string &source, std::string path)
      : source_(source), path_(std::move(path)) {}

  JsonValue parse() {
    JsonValue value = parse_value();
    skip_ws();
    if (ok_ && pos_ != source_.size()) {
      fail("unexpected trailing input");
    }
    return value;
  }

  bool ok() const { return ok_; }
  const std::vector<BuildDiagnostic> &diagnostics() const {
    return diagnostics_;
  }

private:
  void skip_ws() {
    while (pos_ < source_.size() &&
           std::isspace(static_cast<unsigned char>(source_[pos_])) != 0) {
      ++pos_;
    }
  }

  void fail(const std::string &message) {
    if (!ok_) {
      return;
    }
    ok_ = false;
    std::ostringstream out;
    out << message << " at byte " << pos_;
    diagnostics_.push_back(diagnostic("BuildManifestError", out.str(), path_));
  }

  bool consume(char expected) {
    skip_ws();
    if (pos_ >= source_.size() || source_[pos_] != expected) {
      std::string message = "expected '";
      message.push_back(expected);
      message.push_back('\'');
      fail(message);
      return false;
    }
    ++pos_;
    return true;
  }

  JsonValue parse_value() {
    skip_ws();
    if (pos_ >= source_.size()) {
      fail("unexpected end of input");
      return {};
    }
    const char c = source_[pos_];
    if (c == '"') {
      JsonValue value;
      value.kind = JsonValue::Kind::String;
      value.string_value = parse_string();
      return value;
    }
    if (c == '[') {
      return parse_array();
    }
    if (c == '{') {
      return parse_object();
    }
    if (source_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      JsonValue value;
      value.kind = JsonValue::Kind::Bool;
      value.bool_value = true;
      return value;
    }
    if (source_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      JsonValue value;
      value.kind = JsonValue::Kind::Bool;
      value.bool_value = false;
      return value;
    }
    if (source_.compare(pos_, 4, "null") == 0) {
      pos_ += 4;
      return {};
    }
    fail("expected JSON value");
    return {};
  }

  std::string parse_string() {
    if (!consume('"')) {
      return {};
    }
    std::string out;
    while (pos_ < source_.size()) {
      const char c = source_[pos_++];
      if (c == '"') {
        return out;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= source_.size()) {
        fail("unterminated escape sequence");
        return {};
      }
      const char escaped = source_[pos_++];
      switch (escaped) {
      case '"':
      case '\\':
      case '/':
        out.push_back(escaped);
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      default:
        fail("unsupported escape sequence");
        return {};
      }
    }
    fail("unterminated string");
    return {};
  }

  JsonValue parse_array() {
    JsonValue value;
    value.kind = JsonValue::Kind::Array;
    if (!consume('[')) {
      return value;
    }
    skip_ws();
    if (pos_ < source_.size() && source_[pos_] == ']') {
      ++pos_;
      return value;
    }
    while (ok_) {
      value.array_value.push_back(parse_value());
      skip_ws();
      if (pos_ < source_.size() && source_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < source_.size() && source_[pos_] == ']') {
        ++pos_;
        return value;
      }
      fail("expected ',' or ']'");
    }
    return value;
  }

  JsonValue parse_object() {
    JsonValue value;
    value.kind = JsonValue::Kind::Object;
    if (!consume('{')) {
      return value;
    }
    skip_ws();
    if (pos_ < source_.size() && source_[pos_] == '}') {
      ++pos_;
      return value;
    }
    while (ok_) {
      skip_ws();
      if (pos_ >= source_.size() || source_[pos_] != '"') {
        fail("expected object key");
        return value;
      }
      const std::string key = parse_string();
      if (!consume(':')) {
        return value;
      }
      value.object_value[key] = parse_value();
      skip_ws();
      if (pos_ < source_.size() && source_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < source_.size() && source_[pos_] == '}') {
        ++pos_;
        return value;
      }
      fail("expected ',' or '}'");
    }
    return value;
  }

  const std::string &source_;
  std::string path_;
  std::size_t pos_ = 0;
  bool ok_ = true;
  std::vector<BuildDiagnostic> diagnostics_;
};

std::string trim(std::string value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return value.substr(begin, end - begin);
}

bool yaml_key_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' ||
         c == '-' || c == '.';
}

std::size_t yaml_key_colon(const std::string &text) {
  bool in_single = false;
  bool in_double = false;
  bool escaped = false;
  int flow_depth = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (in_double) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_double = false;
      }
      continue;
    }
    if (in_single) {
      if (c == '\'') {
        if (i + 1U < text.size() && text[i + 1U] == '\'') {
          ++i;
        } else {
          in_single = false;
        }
      }
      continue;
    }
    if (c == '"') {
      in_double = true;
      continue;
    }
    if (c == '\'') {
      in_single = true;
      continue;
    }
    if (c == '[' || c == '{') {
      ++flow_depth;
      continue;
    }
    if (c == ']' || c == '}') {
      --flow_depth;
      continue;
    }
    if (c != ':' || flow_depth != 0) {
      continue;
    }
    if (i + 1U < text.size()) {
      const char next = text[i + 1U];
      if (std::isspace(static_cast<unsigned char>(next)) == 0 &&
          next != '[' && next != '{' && next != '"' && next != '\'') {
        continue;
      }
    }
    const std::string key = trim(text.substr(0, i));
    if (key.empty()) {
      continue;
    }
    if (key.front() == '"' || key.front() == '\'') {
      return i;
    }
    bool plain_key = true;
    for (const char key_c : key) {
      plain_key = plain_key && yaml_key_char(key_c);
    }
    if (plain_key) {
      return i;
    }
  }
  return std::string::npos;
}

std::vector<std::string> split_flow_items(const std::string &text,
                                          bool *ok) {
  std::vector<std::string> items;
  bool in_single = false;
  bool in_double = false;
  bool escaped = false;
  int flow_depth = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (in_double) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_double = false;
      }
      continue;
    }
    if (in_single) {
      if (c == '\'') {
        if (i + 1U < text.size() && text[i + 1U] == '\'') {
          ++i;
        } else {
          in_single = false;
        }
      }
      continue;
    }
    if (c == '"') {
      in_double = true;
      continue;
    }
    if (c == '\'') {
      in_single = true;
      continue;
    }
    if (c == '[' || c == '{') {
      ++flow_depth;
      continue;
    }
    if (c == ']' || c == '}') {
      --flow_depth;
      if (flow_depth < 0) {
        *ok = false;
        return {};
      }
      continue;
    }
    if (c == ',' && flow_depth == 0) {
      items.push_back(trim(text.substr(start, i - start)));
      start = i + 1U;
    }
  }
  if (in_single || in_double || flow_depth != 0) {
    *ok = false;
    return {};
  }
  items.push_back(trim(text.substr(start)));
  return items;
}

class YamlParser {
public:
  YamlParser(const std::string &source, std::string path)
      : source_(source), path_(std::move(path)) {}

  JsonValue parse() {
    preprocess();
    if (!ok_) {
      return {};
    }
    if (lines_.empty()) {
      fail(1, "empty YAML build manifest");
      return {};
    }
    if (lines_.front().indent != 0U) {
      fail(lines_.front().line_no,
           "YAML build manifest must start at indentation 0");
      return {};
    }
    JsonValue value = parse_node(lines_.front().indent);
    if (ok_ && pos_ != lines_.size()) {
      fail(lines_[pos_].line_no, "unexpected trailing YAML input");
    }
    return value;
  }

  bool ok() const { return ok_; }
  const std::vector<BuildDiagnostic> &diagnostics() const {
    return diagnostics_;
  }

private:
  struct Line {
    std::size_t indent = 0;
    std::string text;
    std::size_t line_no = 0;
  };

  void fail(std::size_t line_no, const std::string &message) {
    if (!ok_) {
      return;
    }
    ok_ = false;
    std::ostringstream out;
    out << message << " at line " << line_no;
    diagnostics_.push_back(diagnostic("BuildManifestError", out.str(), path_));
  }

  std::string strip_comment(const std::string &line) const {
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
      const char c = line[i];
      if (in_double) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          in_double = false;
        }
        continue;
      }
      if (in_single) {
        if (c == '\'') {
          if (i + 1U < line.size() && line[i + 1U] == '\'') {
            ++i;
          } else {
            in_single = false;
          }
        }
        continue;
      }
      if (c == '"') {
        in_double = true;
        continue;
      }
      if (c == '\'') {
        in_single = true;
        continue;
      }
      if (c == '#' &&
          (i == 0U ||
           std::isspace(static_cast<unsigned char>(line[i - 1U])) != 0)) {
        return line.substr(0, i);
      }
    }
    return line;
  }

  void preprocess() {
    std::istringstream input(source_);
    std::string line;
    std::size_t line_no = 0;
    bool skipped_document_start = false;
    while (std::getline(input, line)) {
      ++line_no;
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      std::size_t indent = 0;
      while (indent < line.size() && line[indent] == ' ') {
        ++indent;
      }
      if (indent < line.size() && line[indent] == '\t') {
        fail(line_no, "tabs are not valid indentation in YAML build manifests");
        return;
      }
      std::string text = trim(strip_comment(line.substr(indent)));
      if (text.empty()) {
        continue;
      }
      if (text == "---" && !skipped_document_start && lines_.empty()) {
        skipped_document_start = true;
        continue;
      }
      if (text == "---" || text == "...") {
        fail(line_no, "multiple YAML documents are not supported");
        return;
      }
      lines_.push_back({indent, std::move(text), line_no});
    }
  }

  bool is_sequence_line(const Line &line) const {
    return line.text == "-" ||
           (line.text.size() > 1U && line.text[0] == '-' &&
            std::isspace(static_cast<unsigned char>(line.text[1])) != 0);
  }

  bool split_mapping(const std::string &text, std::string *key,
                     std::string *rest, std::size_t line_no) {
    const std::size_t colon = yaml_key_colon(text);
    if (colon == std::string::npos) {
      fail(line_no, "expected YAML mapping entry");
      return false;
    }
    std::string raw_key = trim(text.substr(0, colon));
    if (!raw_key.empty() &&
        (raw_key.front() == '"' || raw_key.front() == '\'')) {
      JsonValue parsed_key = parse_inline_value(raw_key, line_no);
      if (!ok_) {
        return false;
      }
      if (parsed_key.kind != JsonValue::Kind::String ||
          parsed_key.string_value.empty()) {
        fail(line_no, "YAML mapping key must be a non-empty string");
        return false;
      }
      *key = parsed_key.string_value;
    } else {
      *key = raw_key;
    }
    *rest = trim(text.substr(colon + 1U));
    return true;
  }

  JsonValue parse_node(std::size_t indent) {
    if (pos_ >= lines_.size()) {
      return {};
    }
    if (lines_[pos_].indent != indent) {
      fail(lines_[pos_].line_no, "unexpected YAML indentation");
      return {};
    }
    if (is_sequence_line(lines_[pos_])) {
      return parse_sequence(indent);
    }
    return parse_mapping(indent);
  }

  JsonValue parse_mapping(std::size_t indent) {
    JsonValue value;
    value.kind = JsonValue::Kind::Object;
    while (ok_ && pos_ < lines_.size()) {
      const Line &line = lines_[pos_];
      if (line.indent < indent) {
        break;
      }
      if (line.indent > indent) {
        fail(line.line_no, "unexpected YAML indentation");
        break;
      }
      if (is_sequence_line(line)) {
        break;
      }
      std::string key;
      std::string rest;
      if (!split_mapping(line.text, &key, &rest, line.line_no)) {
        break;
      }
      ++pos_;
      if (rest.empty()) {
        if (pos_ < lines_.size() && lines_[pos_].indent > indent) {
          value.object_value[key] = parse_node(lines_[pos_].indent);
        } else {
          value.object_value[key] = {};
        }
      } else {
        value.object_value[key] = parse_inline_value(rest, line.line_no);
      }
    }
    return value;
  }

  JsonValue parse_sequence(std::size_t indent) {
    JsonValue value;
    value.kind = JsonValue::Kind::Array;
    while (ok_ && pos_ < lines_.size()) {
      const Line &line = lines_[pos_];
      if (line.indent < indent) {
        break;
      }
      if (line.indent > indent) {
        fail(line.line_no, "unexpected YAML indentation");
        break;
      }
      if (!is_sequence_line(line)) {
        break;
      }

      std::string rest =
          line.text == "-" ? std::string{} : trim(line.text.substr(1U));
      if (rest.empty()) {
        ++pos_;
        if (pos_ < lines_.size() && lines_[pos_].indent > indent) {
          value.array_value.push_back(parse_node(lines_[pos_].indent));
        } else {
          value.array_value.push_back({});
        }
        continue;
      }

      std::string key;
      std::string entry_rest;
      if (yaml_key_colon(rest) != std::string::npos &&
          split_mapping(rest, &key, &entry_rest, line.line_no)) {
        JsonValue item;
        item.kind = JsonValue::Kind::Object;
        ++pos_;
        if (entry_rest.empty()) {
          if (pos_ < lines_.size() && lines_[pos_].indent > indent) {
            item.object_value[key] = parse_node(lines_[pos_].indent);
          } else {
            item.object_value[key] = {};
          }
        } else {
          item.object_value[key] = parse_inline_value(entry_rest, line.line_no);
        }
        if (pos_ < lines_.size() && lines_[pos_].indent > indent) {
          JsonValue extra = parse_node(lines_[pos_].indent);
          if (extra.kind != JsonValue::Kind::Object) {
            fail(line.line_no, "YAML sequence mapping item expected a mapping");
            return value;
          }
          item.object_value.insert(extra.object_value.begin(),
                                   extra.object_value.end());
        }
        value.array_value.push_back(std::move(item));
        continue;
      }

      value.array_value.push_back(parse_inline_value(rest, line.line_no));
      ++pos_;
      if (pos_ < lines_.size() && lines_[pos_].indent > indent) {
        fail(lines_[pos_].line_no,
             "YAML scalar sequence item cannot have nested content");
      }
    }
    return value;
  }

  JsonValue parse_inline_value(const std::string &raw, std::size_t line_no) {
    const std::string text = trim(raw);
    if (text.empty() || text == "null" || text == "Null" || text == "NULL" ||
        text == "~") {
      return {};
    }
    if (text == "true" || text == "True" || text == "TRUE") {
      JsonValue value;
      value.kind = JsonValue::Kind::Bool;
      value.bool_value = true;
      return value;
    }
    if (text == "false" || text == "False" || text == "FALSE") {
      JsonValue value;
      value.kind = JsonValue::Kind::Bool;
      value.bool_value = false;
      return value;
    }
    if (text.front() == '[') {
      return parse_flow_sequence(text, line_no);
    }
    if (text.front() == '{') {
      return parse_flow_mapping(text, line_no);
    }
    if (text.front() == '"' || text.front() == '\'') {
      return parse_quoted_scalar(text, line_no);
    }
    JsonValue value;
    value.kind = JsonValue::Kind::String;
    value.string_value = text;
    return value;
  }

  JsonValue parse_quoted_scalar(const std::string &text, std::size_t line_no) {
    JsonValue value;
    value.kind = JsonValue::Kind::String;
    const char quote = text.front();
    std::string out;
    std::size_t i = 1;
    for (; i < text.size(); ++i) {
      const char c = text[i];
      if (quote == '"' && c == '\\') {
        if (i + 1U >= text.size()) {
          fail(line_no, "unterminated YAML escape sequence");
          return value;
        }
        const char escaped = text[++i];
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
          out.push_back(escaped);
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        default:
          fail(line_no, "unsupported YAML escape sequence");
          return value;
        }
        continue;
      }
      if (quote == '\'' && c == '\'' && i + 1U < text.size() &&
          text[i + 1U] == '\'') {
        out.push_back('\'');
        ++i;
        continue;
      }
      if (c == quote) {
        if (!trim(text.substr(i + 1U)).empty()) {
          fail(line_no, "unexpected characters after YAML quoted scalar");
        }
        value.string_value = std::move(out);
        return value;
      }
      out.push_back(c);
    }
    fail(line_no, "unterminated YAML quoted scalar");
    return value;
  }

  JsonValue parse_flow_sequence(const std::string &text, std::size_t line_no) {
    JsonValue value;
    value.kind = JsonValue::Kind::Array;
    if (text.size() < 2U || text.back() != ']') {
      fail(line_no, "unterminated YAML flow sequence");
      return value;
    }
    const std::string inner = trim(text.substr(1U, text.size() - 2U));
    if (inner.empty()) {
      return value;
    }
    bool ok = true;
    const std::vector<std::string> items = split_flow_items(inner, &ok);
    if (!ok) {
      fail(line_no, "invalid YAML flow sequence");
      return value;
    }
    for (const std::string &item : items) {
      if (item.empty()) {
        fail(line_no, "empty YAML flow sequence item");
        return value;
      }
      value.array_value.push_back(parse_inline_value(item, line_no));
      if (!ok_) {
        return value;
      }
    }
    return value;
  }

  JsonValue parse_flow_mapping(const std::string &text, std::size_t line_no) {
    JsonValue value;
    value.kind = JsonValue::Kind::Object;
    if (text.size() < 2U || text.back() != '}') {
      fail(line_no, "unterminated YAML flow mapping");
      return value;
    }
    const std::string inner = trim(text.substr(1U, text.size() - 2U));
    if (inner.empty()) {
      return value;
    }
    bool ok = true;
    const std::vector<std::string> entries = split_flow_items(inner, &ok);
    if (!ok) {
      fail(line_no, "invalid YAML flow mapping");
      return value;
    }
    for (const std::string &entry : entries) {
      std::string key;
      std::string rest;
      if (!split_mapping(entry, &key, &rest, line_no)) {
        return value;
      }
      value.object_value[key] = parse_inline_value(rest, line_no);
      if (!ok_) {
        return value;
      }
    }
    return value;
  }

  const std::string &source_;
  std::string path_;
  std::vector<Line> lines_;
  std::size_t pos_ = 0;
  bool ok_ = true;
  std::vector<BuildDiagnostic> diagnostics_;
};

const JsonValue *member(const JsonValue &object, const std::string &key) {
  if (object.kind != JsonValue::Kind::Object) {
    return nullptr;
  }
  const auto found = object.object_value.find(key);
  if (found == object.object_value.end()) {
    return nullptr;
  }
  return &found->second;
}

bool read_string_member(const JsonValue &object, const std::string &key,
                        std::string *out) {
  const JsonValue *value = member(object, key);
  if (value == nullptr) {
    return false;
  }
  if (value->kind != JsonValue::Kind::String) {
    return false;
  }
  *out = value->string_value;
  return true;
}

bool read_string_array_member(const JsonValue &object, const std::string &key,
                              std::vector<std::string> *out) {
  const JsonValue *value = member(object, key);
  if (value == nullptr) {
    return true;
  }
  if (value->kind != JsonValue::Kind::Array) {
    return false;
  }
  std::vector<std::string> items;
  for (const JsonValue &item : value->array_value) {
    if (item.kind != JsonValue::Kind::String) {
      return false;
    }
    items.push_back(item.string_value);
  }
  *out = std::move(items);
  return true;
}

std::vector<std::string> sorted_unique(std::vector<std::string> values) {
  values.erase(std::remove(values.begin(), values.end(), ""), values.end());
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

std::vector<BuildModule> sorted_modules(std::vector<BuildModule> modules) {
  std::sort(modules.begin(), modules.end(),
            [](const BuildModule &left, const BuildModule &right) {
              return left.name < right.name;
            });
  return modules;
}

bool read_modules(const JsonValue &root, const std::string &key, bool stdlib,
                  const std::string &path, std::vector<BuildModule> *out,
                  std::vector<BuildDiagnostic> *diagnostics) {
  const JsonValue *value = member(root, key);
  if (value == nullptr) {
    return true;
  }
  if (value->kind != JsonValue::Kind::Array) {
    diagnostics->push_back(diagnostic("BuildManifestError",
                                      "'" + key + "' must be an array", path));
    return false;
  }

  std::vector<BuildModule> modules;
  for (const JsonValue &item : value->array_value) {
    if (item.kind != JsonValue::Kind::Object) {
      diagnostics->push_back(diagnostic(
          "BuildManifestError", "'" + key + "' entries must be objects", path));
      return false;
    }
    BuildModule module;
    module.stdlib = stdlib;
    if (!read_string_member(item, "name", &module.name) ||
        !read_string_member(item, "path", &module.path)) {
      diagnostics->push_back(diagnostic(
          "BuildManifestError",
          "'" + key + "' entries require string 'name' and 'path'", path));
      return false;
    }
    if (stdlib &&
        !read_string_member(item, "bootstrap", &module.bootstrap_layer)) {
      module.bootstrap_layer = "B2";
    }
    modules.push_back(std::move(module));
  }
  *out = sorted_modules(std::move(modules));
  return true;
}

void emit_string_array(std::ostringstream &out,
                       const std::vector<std::string> &values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0U) {
      out << ", ";
    }
    out << "\"" << json_escape(values[i]) << "\"";
  }
  out << "]";
}

void emit_profile_set(std::ostringstream &out, const BuildProfileSet &profiles,
                      const std::string &indent) {
  out << indent << "\"profiles\": {\n";
  out << indent << "  \"required\": ";
  emit_string_array(out, profiles.required_features);
  out << ",\n";
  out << indent << "  \"optional\": ";
  emit_string_array(out, profiles.optional_features);
  out << ",\n";
  out << indent << "  \"forbidden\": ";
  emit_string_array(out, profiles.forbidden_features);
  out << "\n" << indent << "}";
}

void emit_module_array(std::ostringstream &out, const char *name,
                       const std::vector<BuildModule> &modules,
                       const std::string &indent) {
  out << indent << "\"" << name << "\": [";
  for (std::size_t i = 0; i < modules.size(); ++i) {
    const BuildModule &module = modules[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n"
        << indent << "  {\"name\":\"" << json_escape(module.name)
        << "\",\"path\":\"" << json_escape(module.path) << "\"";
    if (module.stdlib) {
      out << ",\"bootstrap\":\"" << json_escape(module.bootstrap_layer) << "\"";
    }
    out << "}";
  }
  if (!modules.empty()) {
    out << "\n" << indent;
  }
  out << "]";
}

bool read_native_extensions(
    const JsonValue &root, const std::string &path,
    std::vector<amber::pkg::PackageNativeExtension> *out,
    std::vector<BuildDiagnostic> *diagnostics) {
  const JsonValue *value = member(root, "native_extensions");
  if (value == nullptr) {
    return true;
  }
  if (value->kind != JsonValue::Kind::Array) {
    diagnostics->push_back(diagnostic(
        "BuildManifestError", "'native_extensions' must be an array", path));
    return false;
  }
  for (const JsonValue &item : value->array_value) {
    if (item.kind != JsonValue::Kind::Object) {
      diagnostics->push_back(diagnostic(
          "BuildManifestError", "'native_extensions' entries must be objects",
          path));
      return false;
    }
    amber::pkg::PackageNativeExtension extension;
    read_string_member(item, "name", &extension.name);
    read_string_member(item, "language", &extension.language);
    read_string_array_member(item, "sources", &extension.sources);
    read_string_array_member(item, "headers", &extension.headers);
    read_string_array_member(item, "include_dirs", &extension.include_dirs);
    read_string_array_member(item, "defines", &extension.defines);
    read_string_array_member(item, "cxxflags", &extension.cxxflags);
    read_string_array_member(item, "link_libraries", &extension.link_libraries);
    if (member(item, "capabilities") != nullptr) {
      diagnostics->push_back(diagnostic(
          "BuildManifestError",
          "native extension 'capabilities' is obsolete; require ffi.v1 in the "
          "build profile instead",
          path));
    }
    if (const JsonValue *symbols = member(item, "symbols")) {
      for (const JsonValue &entry : symbols->array_value) {
        amber::pkg::PackageNativeSymbol symbol;
        read_string_member(entry, "logical", &symbol.logical);
        read_string_member(entry, "symbol", &symbol.symbol);
        extension.symbols.push_back(std::move(symbol));
      }
    }
    if (const JsonValue *types = member(item, "types")) {
      for (const JsonValue &entry : types->array_value) {
        amber::pkg::PackageNativeType type;
        read_string_member(entry, "amber", &type.amber);
        read_string_member(entry, "tag", &type.tag);
        read_string_member(entry, "ownership", &type.ownership);
        read_string_member(entry, "destructor", &type.destructor);
        extension.types.push_back(std::move(type));
      }
    }
    if (const JsonValue *errors = member(item, "errors")) {
      for (const JsonValue &entry : errors->array_value) {
        amber::pkg::PackageNativeError error;
        read_string_member(entry, "name", &error.name);
        read_string_member(entry, "parent", &error.parent);
        read_string_member(entry, "default_message", &error.default_message);
        read_string_member(entry, "default_exit_code",
                           &error.default_exit_code);
        extension.errors.push_back(std::move(error));
      }
    }
    if (extension.name.empty()) {
      diagnostics->push_back(diagnostic(
          "BuildManifestError", "native extension entries require a name",
          path));
    }
    for (const amber::pkg::PackageNativeError &error : extension.errors) {
      if (error.name.empty()) {
        diagnostics->push_back(diagnostic(
            "BuildManifestError", "native extension errors require a name",
            path));
      }
    }
    out->push_back(std::move(extension));
  }
  return true;
}

BuildManifestResult manifest_from_value(const JsonValue &root,
                                        const std::string &path) {
  BuildManifestResult result;
  if (root.kind != JsonValue::Kind::Object) {
    result.diagnostics.push_back(diagnostic(
        "BuildManifestError", "build manifest root must be an object", path));
    return result;
  }

  std::string schema;
  if (read_string_member(root, "schema", &schema)) {
    result.manifest.schema = schema;
  }
  if (result.manifest.schema != "amber.build.v1") {
    result.diagnostics.push_back(diagnostic(
        "BuildManifestError",
        "unsupported build manifest schema '" + result.manifest.schema + "'",
        path));
  }
  if (!read_string_member(root, "name", &result.manifest.name)) {
    result.diagnostics.push_back(
        diagnostic("BuildManifestError", "missing string 'name'", path));
  }
  if (!read_string_member(root, "root", &result.manifest.root_module)) {
    result.diagnostics.push_back(
        diagnostic("BuildManifestError", "missing string 'root'", path));
  }

  if (const JsonValue *profiles = member(root, "profiles")) {
    if (profiles->kind != JsonValue::Kind::Object) {
      result.diagnostics.push_back(diagnostic(
          "BuildManifestError", "'profiles' must be an object", path));
    } else {
      if (!read_string_array_member(
              *profiles, "required",
              &result.manifest.profiles.required_features) ||
          !read_string_array_member(
              *profiles, "optional",
              &result.manifest.profiles.optional_features) ||
          !read_string_array_member(
              *profiles, "forbidden",
              &result.manifest.profiles.forbidden_features)) {
        result.diagnostics.push_back(diagnostic(
            "BuildManifestError",
            "profile feature lists must be arrays of strings", path));
      }
      if (const JsonValue *numeric = member(*profiles, "numeric")) {
        if (numeric->kind != JsonValue::Kind::Object) {
          result.diagnostics.push_back(diagnostic(
              "BuildManifestError", "'profiles.numeric' must be an object",
              path));
        } else {
          read_string_member(*numeric, "int",
                             &result.manifest.profiles.numeric_int);
          read_string_member(*numeric, "overflow",
                             &result.manifest.profiles.numeric_overflow);
          const std::string &int_type = result.manifest.profiles.numeric_int;
          if (!int_type.empty() && int_type != "Int8" && int_type != "Int16" &&
              int_type != "Int32" && int_type != "Int64" &&
              int_type != "UInt8" && int_type != "UInt16" &&
              int_type != "UInt32" && int_type != "UInt64" &&
              int_type != "BigInt") {
            result.diagnostics.push_back(diagnostic(
                "BuildManifestError",
                "'profiles.numeric.int' must be one of Int8/Int16/Int32/"
                "Int64, UInt8/UInt16/UInt32/UInt64, or BigInt",
                path));
          }
          const std::string &overflow =
              result.manifest.profiles.numeric_overflow;
          if (!overflow.empty() && overflow != "checked" &&
              overflow != "wrapping" && overflow != "saturating") {
            result.diagnostics.push_back(diagnostic(
                "BuildManifestError",
                "'profiles.numeric.overflow' must be checked, wrapping, or "
                "saturating",
                path));
          }
        }
      }
    }
  }
  result.manifest.profiles = normalize_profiles(result.manifest.profiles);
  if (result.manifest.profiles.required_features.empty()) {
    result.manifest.profiles.required_features.push_back("core.v1");
  }

  read_modules(root, "stdlib", true, path, &result.manifest.stdlib_modules,
               &result.diagnostics);
  read_modules(root, "modules", false, path, &result.manifest.modules,
               &result.diagnostics);
  read_native_extensions(root, path, &result.manifest.native_extensions,
                         &result.diagnostics);

  // Native extensions execute foreign code, so a build that declares any must
  // opt into FFI by enabling the `ffi.v1` profile feature (native-packages
  // design §2.3/§6). Bytecode runtimes do not support ffi.v1, so this also
  // ensures such builds target the native backend.
  if (!result.manifest.native_extensions.empty()) {
    bool declares_ffi = false;
    for (const std::string &feature :
         result.manifest.profiles.required_features) {
      declares_ffi = declares_ffi || feature == "ffi.v1";
    }
    for (const std::string &feature :
         result.manifest.profiles.optional_features) {
      declares_ffi = declares_ffi || feature == "ffi.v1";
    }
    if (!declares_ffi) {
      result.diagnostics.push_back(diagnostic(
          "BuildManifestError",
          "native extensions require the 'ffi.v1' profile feature", path));
    }
  }

  if (result.manifest.modules.empty()) {
    result.diagnostics.push_back(diagnostic(
        "BuildManifestError", "at least one module entry is required", path));
  }
  bool root_found = false;
  for (const BuildModule &module : result.manifest.modules) {
    root_found = root_found || module.name == result.manifest.root_module;
  }
  if (!root_found && !result.manifest.root_module.empty()) {
    result.diagnostics.push_back(
        diagnostic("BuildManifestError",
                   "root module is not declared in modules: " +
                       result.manifest.root_module,
                   path));
  }

  std::set<std::string> required(
      result.manifest.profiles.required_features.begin(),
      result.manifest.profiles.required_features.end());
  for (const std::string &feature :
       result.manifest.profiles.forbidden_features) {
    if (required.find(feature) != required.end()) {
      result.diagnostics.push_back(diagnostic(
          "BuildManifestError",
          "profile feature cannot be both required and forbidden: " + feature,
          path));
    }
  }
  return result;
}

} // namespace

BuildManifestResult parse_build_manifest_json(const std::string &source,
                                              const std::string &path) {
  BuildManifestResult result;
  JsonParser parser(source, path);
  const JsonValue root = parser.parse();
  if (!parser.ok()) {
    result.diagnostics = parser.diagnostics();
    return result;
  }
  return manifest_from_value(root, path);
}

BuildManifestResult parse_build_manifest_yaml(const std::string &source,
                                              const std::string &path) {
  BuildManifestResult result;
  YamlParser parser(source, path);
  const JsonValue root = parser.parse();
  if (!parser.ok()) {
    result.diagnostics = parser.diagnostics();
    return result;
  }
  return manifest_from_value(root, path);
}

BuildManifestResult parse_build_manifest(const std::string &source,
                                         const std::string &path) {
  for (const char c : source) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      continue;
    }
    if (c == '{' || c == '[') {
      return parse_build_manifest_json(source, path);
    }
    return parse_build_manifest_yaml(source, path);
  }
  return parse_build_manifest_yaml(source, path);
}

BuildProfileSet normalize_profiles(BuildProfileSet profiles) {
  profiles.required_features =
      sorted_unique(std::move(profiles.required_features));
  profiles.optional_features =
      sorted_unique(std::move(profiles.optional_features));
  profiles.forbidden_features =
      sorted_unique(std::move(profiles.forbidden_features));
  return profiles;
}

std::uint32_t profile_flags_for(const BuildProfileSet &profiles) {
  static const std::map<std::string, std::uint32_t> kKnownFlags = {
      {"core.v1", 1U << 0U},
      {"typed.v1", 1U << 1U},
      {"capabilities.v1", 1U << 2U},
      {"effects.v1", 1U << 3U},
      {"replay.v1", 1U << 4U},
      {"schema.v1", 1U << 5U},
      {"data.v1", 1U << 6U},
      {"wasm.v1", 1U << 7U},
      {"accelerator.v1", 1U << 8U},
      {"agent.v1", 1U << 9U},
      {"contracts.v1", 1U << 10U},
      {"privacy.v1", 1U << 11U},
      {"workflow.v1", 1U << 12U},
      {"native.mir.v1", 1U << 13U},
      {"notebook.watch.v1", 1U << 14U},
      {"ffi.v1", 1U << 15U},
      {"macro.v1", 1U << 16U},
  };

  std::uint32_t flags = 0;
  std::vector<std::string> enabled = profiles.required_features;
  enabled.insert(enabled.end(), profiles.optional_features.begin(),
                 profiles.optional_features.end());
  for (const std::string &feature : enabled) {
    const auto found = kKnownFlags.find(feature);
    if (found != kKnownFlags.end()) {
      flags |= found->second;
    }
  }
  return flags;
}

bool runtime_supports_feature(const std::string &feature) {
  static const std::set<std::string> kSupported = {
      "core.v1",        "typed.v1",      "capabilities.v1",   "effects.v1",
      "replay.v1",      "schema.v1",     "data.v1",           "wasm.v1",
      "accelerator.v1", "agent.v1",      "contracts.v1",      "privacy.v1",
      "workflow.v1",    "native.mir.v1", "notebook.watch.v1", "macro.v1",
  };
  return kSupported.find(feature) != kSupported.end();
}

std::string manifest_to_json(const BuildManifest &manifest) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"" << json_escape(manifest.schema) << "\",\n";
  out << "  \"name\": \"" << json_escape(manifest.name) << "\",\n";
  out << "  \"root\": \"" << json_escape(manifest.root_module) << "\",\n";
  emit_profile_set(out, manifest.profiles, "  ");
  out << ",\n";
  emit_module_array(out, "stdlib", manifest.stdlib_modules, "  ");
  out << ",\n";
  emit_module_array(out, "modules", manifest.modules, "  ");
  out << "\n}\n";
  return out.str();
}

std::string summary_to_json(const BuildSummary &summary) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.build.result.v1\",\n";
  out << "  \"status\": \"" << (summary.ok ? "ok" : "error") << "\",\n";
  out << "  \"name\": \"" << json_escape(summary.name) << "\",\n";
  out << "  \"root\": \"" << json_escape(summary.root_module) << "\",\n";
  out << "  \"target\": \"" << json_escape(summary.target) << "\",\n";
  out << "  \"out_dir\": \"" << json_escape(summary.out_dir) << "\",\n";
  out << "  \"cache_dir\": \"" << json_escape(summary.cache_dir) << "\",\n";
  out << "  \"native_output\": \"" << json_escape(summary.native_output_path)
      << "\",\n";
  out << "  \"native_backend\": \"" << json_escape(summary.native_backend)
      << "\",\n";
  out << "  \"native_hash\": \"" << json_escape(summary.native_hash) << "\",\n";
  out << "  \"native_source\": \""
      << json_escape(summary.native_launcher_source) << "\",\n";
  out << "  \"native_cxx\": \"" << json_escape(summary.native_cxx) << "\",\n";
  out << "  \"native_bytecode_fallback\": "
      << (summary.native_bytecode_trampoline ? "true" : "false") << ",\n";
  out << "  \"native_graph_module_count\": "
      << summary.native_graph_module_count << ",\n";
  out << "  \"native_graph_code_count\": " << summary.native_graph_code_count
      << ",\n";
  out << "  \"native_graph_native_code_count\": "
      << summary.native_graph_native_code_count << ",\n";
  out << "  \"native_graph_vm_fallback_code_count\": "
      << summary.native_graph_vm_fallback_code_count << ",\n";
  out << "  \"native_graph_fallback_code_count\": "
      << summary.native_graph_fallback_code_count << ",\n";
  out << "  \"native_graph_full_coverage\": "
      << (summary.native_graph_full_coverage ? "true" : "false") << ",\n";
  out << "  \"native_extensions\": [";
  for (std::size_t i = 0; i < summary.native_extensions.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    const amber::pkg::PackageNativeExtensionMetadata &metadata =
        summary.native_extensions[i];
    out << "\n    {\"name\":\"" << json_escape(metadata.name)
        << "\",\"amber_ext_abi_version\":"
        << metadata.amber_ext_abi_version << ",\"target_triple\":\""
        << json_escape(metadata.target_triple)
        << "\",\"native_source_sha256\":\""
        << json_escape(metadata.native_source_digest)
        << "\",\"exported_symbol_sha256\":\""
        << json_escape(metadata.exported_symbol_digest)
        << "\",\"types\":[";
    for (std::size_t j = 0; j < metadata.types.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      out << "{\"amber\":\"" << json_escape(metadata.types[j].amber)
          << "\",\"tag\":\"" << json_escape(metadata.types[j].tag)
          << "\",\"ownership\":\""
          << json_escape(metadata.types[j].ownership)
          << "\",\"destructor\":\""
          << json_escape(metadata.types[j].destructor) << "\"}";
    }
    out << "],\"errors\":[";
    for (std::size_t j = 0; j < metadata.errors.size(); ++j) {
      if (j != 0U) {
        out << ",";
      }
      out << "{\"name\":\"" << json_escape(metadata.errors[j].name)
          << "\",\"parent\":\"" << json_escape(metadata.errors[j].parent)
          << "\",\"default_message\":\""
          << json_escape(metadata.errors[j].default_message)
          << "\",\"default_exit_code\":\""
          << json_escape(metadata.errors[j].default_exit_code) << "\"}";
    }
    out << "]}";
  }
  if (!summary.native_extensions.empty()) {
    out << "\n  ";
  }
  out << "],\n";
  emit_profile_set(out, summary.profiles, "  ");
  out << ",\n";
  out << "  \"artifacts\": [";
  for (std::size_t i = 0; i < summary.artifacts.size(); ++i) {
    const BuildArtifactRecord &artifact = summary.artifacts[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"name\":\"" << json_escape(artifact.name)
        << "\",\"path\":\"" << json_escape(artifact.path) << "\",\"output\":\""
        << json_escape(artifact.output_path) << "\",\"cache_path\":\""
        << json_escape(artifact.cache_path) << "\",\"cache_key\":\""
        << json_escape(artifact.cache_key) << "\",\"source_hash\":\""
        << json_escape(artifact.source_hash) << "\",\"artifact_hash\":\""
        << json_escape(artifact.artifact_hash) << "\",\"abi_hash\":\""
        << json_escape(artifact.abi_hash) << "\",\"native_output\":\""
        << json_escape(artifact.native_output_path) << "\",\"native_hash\":\""
        << json_escape(artifact.native_hash) << "\",\"native_backend\":\""
        << json_escape(artifact.native_backend) << "\",\"native_eligible\":"
        << (artifact.native_eligible ? "true" : "false")
        << ",\"native_fallback_reason\":\""
        << json_escape(artifact.native_fallback_reason)
        << "\",\"stdlib\":" << (artifact.stdlib ? "true" : "false")
        << ",\"cached\":" << (artifact.cached ? "true" : "false")
        << ",\"bootstrap\":\"" << json_escape(artifact.bootstrap_layer)
        << "\",\"bytes\":" << artifact.byte_size
        << ",\"native_bytes\":" << artifact.native_byte_size << "}";
  }
  if (!summary.artifacts.empty()) {
    out << "\n  ";
  }
  out << "],\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < summary.diagnostics.size(); ++i) {
    const BuildDiagnostic &diag = summary.diagnostics[i];
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"error_name\":\"" << json_escape(diag.error_name)
        << "\",\"message\":\"" << json_escape(diag.message) << "\",\"path\":\""
        << json_escape(diag.path) << "\"}";
  }
  if (!summary.diagnostics.empty()) {
    out << "\n  ";
  }
  out << "]\n";
  out << "}\n";
  return out.str();
}

std::string
diagnostics_to_string(const std::vector<BuildDiagnostic> &diagnostics) {
  std::ostringstream out;
  for (const BuildDiagnostic &diagnostic : diagnostics) {
    out << diagnostic.error_name << ": " << diagnostic.message;
    if (!diagnostic.path.empty()) {
      out << " (" << diagnostic.path << ")";
    }
    out << "\n";
  }
  return out.str();
}

} // namespace amber::build
