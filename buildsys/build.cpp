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
  if (root.kind != JsonValue::Kind::Object) {
    result.diagnostics.push_back(diagnostic(
        "BuildManifestError", "amber.build.json root must be an object", path));
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
      "workflow.v1",    "native.mir.v1", "notebook.watch.v1",
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
