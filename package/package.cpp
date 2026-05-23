#include "package/package.h"

#include "frontend/lexer/token.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace amber::pkg {

namespace {

constexpr const char *kPackageSchema = "amber.pkg.v1";
constexpr const char *kSignatureAlgorithm = "amber-sha256-dev-v1";

PackageDiagnostic diagnostic(std::string error_name, std::string message,
                             std::string path = {}) {
  PackageDiagnostic out;
  out.error_name = std::move(error_name);
  out.message = std::move(message);
  out.path = std::move(path);
  return out;
}

std::string trim(const std::string &value) {
  std::size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  std::size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string strip_comment(const std::string &line) {
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (c == '\\' && in_string) {
      escaped = true;
      continue;
    }
    if (c == '"') {
      in_string = !in_string;
      continue;
    }
    if (c == '#' && !in_string) {
      return line.substr(0, i);
    }
  }
  return line;
}

bool unquote_toml_string(const std::string &raw, std::string *out) {
  const std::string value = trim(raw);
  if (value.size() < 2U || value.front() != '"' || value.back() != '"') {
    *out = value;
    return true;
  }
  std::string decoded;
  for (std::size_t i = 1; i + 1U < value.size(); ++i) {
    const char c = value[i];
    if (c != '\\') {
      decoded.push_back(c);
      continue;
    }
    if (i + 2U >= value.size()) {
      return false;
    }
    const char escaped = value[++i];
    switch (escaped) {
    case '"':
    case '\\':
      decoded.push_back(escaped);
      break;
    case 'n':
      decoded.push_back('\n');
      break;
    case 'r':
      decoded.push_back('\r');
      break;
    case 't':
      decoded.push_back('\t');
      break;
    default:
      return false;
    }
  }
  *out = decoded;
  return true;
}

bool parse_toml_bool(const std::string &raw, bool *out) {
  const std::string value = trim(raw);
  if (value == "true") {
    *out = true;
    return true;
  }
  if (value == "false") {
    *out = false;
    return true;
  }
  return false;
}

bool parse_toml_string_array(const std::string &raw,
                             std::vector<std::string> *out) {
  const std::string value = trim(raw);
  if (value.size() < 2U || value.front() != '[' || value.back() != ']') {
    return false;
  }
  std::vector<std::string> items;
  std::string current;
  bool in_string = false;
  bool escaped = false;
  bool expecting_item = true;
  for (std::size_t i = 1; i + 1U < value.size(); ++i) {
    const char c = value[i];
    if (in_string) {
      if (escaped) {
        switch (c) {
        case '"':
        case '\\':
          current.push_back(c);
          break;
        case 'n':
          current.push_back('\n');
          break;
        case 'r':
          current.push_back('\r');
          break;
        case 't':
          current.push_back('\t');
          break;
        default:
          return false;
        }
        escaped = false;
        continue;
      }
      if (c == '\\') {
        escaped = true;
        continue;
      }
      if (c == '"') {
        in_string = false;
        items.push_back(current);
        current.clear();
        expecting_item = false;
        continue;
      }
      current.push_back(c);
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      continue;
    }
    if (c == ',') {
      if (expecting_item) {
        return false;
      }
      expecting_item = true;
      continue;
    }
    if (c == '"' && expecting_item) {
      in_string = true;
      continue;
    }
    return false;
  }
  if (in_string || escaped || (expecting_item && !items.empty())) {
    return false;
  }
  *out = std::move(items);
  return true;
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

std::string line_escape(const std::string &value) {
  std::ostringstream out;
  const char *hex = "0123456789ABCDEF";
  for (const unsigned char c : value) {
    if (c == '%' || c == '=' || c == '\n' || c == '\r') {
      out << '%' << hex[c >> 4U] << hex[c & 0x0FU];
    } else {
      out << static_cast<char>(c);
    }
  }
  return out.str();
}

int from_hex_digit(char c) {
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

bool line_unescape(const std::string &value, std::string *out) {
  std::string decoded;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c != '%') {
      decoded.push_back(c);
      continue;
    }
    if (i + 2U >= value.size()) {
      return false;
    }
    const int hi = from_hex_digit(value[i + 1U]);
    const int lo = from_hex_digit(value[i + 2U]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    decoded.push_back(static_cast<char>((hi << 4U) | lo));
    i += 2U;
  }
  *out = decoded;
  return true;
}

std::string bytes_to_string(const std::vector<std::uint8_t> &bytes) {
  return std::string(bytes.begin(), bytes.end());
}

std::string sha256_hex_bytes(const std::vector<std::uint8_t> &bytes) {
  return amber::lexer::sha256_hex(bytes_to_string(bytes));
}

std::string sha256_prefixed(const std::string &value) {
  return "sha256:" + amber::lexer::sha256_hex(value);
}

std::string bytes_to_hex(const std::vector<std::uint8_t> &bytes) {
  std::ostringstream out;
  const char *hex = "0123456789abcdef";
  for (const std::uint8_t byte : bytes) {
    out << hex[(byte >> 4U) & 0x0FU] << hex[byte & 0x0FU];
  }
  return out.str();
}

bool hex_to_bytes(const std::string &hex, std::vector<std::uint8_t> *bytes) {
  if (hex.size() % 2U != 0U) {
    return false;
  }
  std::vector<std::uint8_t> decoded;
  decoded.reserve(hex.size() / 2U);
  for (std::size_t i = 0; i < hex.size(); i += 2U) {
    const int hi = from_hex_digit(hex[i]);
    const int lo = from_hex_digit(hex[i + 1U]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    decoded.push_back(static_cast<std::uint8_t>((hi << 4U) | lo));
  }
  *bytes = std::move(decoded);
  return true;
}

bool safe_registry_component(const std::string &value) {
  if (value.empty() || value == "." || value == ".." ||
      value.find("..") != std::string::npos) {
    return false;
  }
  for (const unsigned char c : value) {
    const bool ok = std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::vector<PackageDependency>
sorted_dependencies(std::vector<PackageDependency> dependencies) {
  std::sort(dependencies.begin(), dependencies.end(),
            [](const PackageDependency &left, const PackageDependency &right) {
              return left.name < right.name;
            });
  return dependencies;
}

std::vector<PackageModule> sorted_modules(std::vector<PackageModule> modules) {
  std::sort(modules.begin(), modules.end(),
            [](const PackageModule &left, const PackageModule &right) {
              return left.name < right.name;
            });
  return modules;
}

std::vector<PackageModuleBlob>
sorted_module_blobs(std::vector<PackageModuleBlob> modules) {
  std::sort(modules.begin(), modules.end(),
            [](const PackageModuleBlob &left, const PackageModuleBlob &right) {
              return left.name < right.name;
            });
  return modules;
}

std::vector<capability::CapabilityRequest>
sorted_capabilities(std::vector<capability::CapabilityRequest> capabilities) {
  std::sort(capabilities.begin(), capabilities.end(),
            [](const capability::CapabilityRequest &left,
               const capability::CapabilityRequest &right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              if (left.target != right.target) {
                return left.target < right.target;
              }
              return left.reason < right.reason;
            });
  return capabilities;
}

std::string canonical_manifest_text(const PackageManifest &manifest) {
  std::ostringstream out;
  out << "name=" << line_escape(manifest.name) << "\n";
  out << "version=" << line_escape(manifest.version) << "\n";
  out << "root=" << line_escape(manifest.root_module) << "\n";
  const std::vector<PackageModule> modules = sorted_modules(manifest.modules);
  out << "module.count=" << modules.size() << "\n";
  for (std::size_t i = 0; i < modules.size(); ++i) {
    out << "module." << i << ".name=" << line_escape(modules[i].name) << "\n";
    out << "module." << i << ".path=" << line_escape(modules[i].path) << "\n";
  }
  const std::vector<PackageDependency> dependencies =
      sorted_dependencies(manifest.dependencies);
  out << "dependency.count=" << dependencies.size() << "\n";
  for (std::size_t i = 0; i < dependencies.size(); ++i) {
    out << "dependency." << i << ".name=" << line_escape(dependencies[i].name)
        << "\n";
    out << "dependency." << i
        << ".version=" << line_escape(dependencies[i].version) << "\n";
    out << "dependency." << i
        << ".source=" << line_escape(dependencies[i].source) << "\n";
  }
  const std::vector<capability::CapabilityRequest> capabilities =
      sorted_capabilities(manifest.capabilities);
  out << "capability.count=" << capabilities.size() << "\n";
  for (std::size_t i = 0; i < capabilities.size(); ++i) {
    out << "capability." << i << ".name=" << line_escape(capabilities[i].name)
        << "\n";
    out << "capability." << i
        << ".target=" << line_escape(capabilities[i].target) << "\n";
    out << "capability." << i
        << ".reason=" << line_escape(capabilities[i].reason) << "\n";
    out << "capability." << i << ".flags=" << capabilities[i].flags << "\n";
  }
  return out.str();
}

std::string dependency_checksum(const PackageDependency &dependency) {
  return sha256_prefixed(dependency.name + "@" + dependency.version + "@" +
                         dependency.source);
}

std::string signature_digest(const std::string &unsigned_payload,
                             const std::string &key_id,
                             const std::string &signing_key) {
  return sha256_prefixed(signing_key + "\n" + key_id + "\n" + unsigned_payload);
}

std::string serialize_unsigned_package(const PackageArtifact &artifact) {
  std::ostringstream out;
  out << kPackageSchema << "\n";
  out << "package.name=" << line_escape(artifact.manifest.name) << "\n";
  out << "package.version=" << line_escape(artifact.manifest.version) << "\n";
  out << "package.root=" << line_escape(artifact.manifest.root_module) << "\n";
  out << "manifest.sha256=" << artifact.manifest_digest << "\n";
  out << "lock.sha256=" << artifact.lock_digest << "\n";
  out << "lock.bytes=";
  const std::string lockfile = artifact.lockfile;
  out << bytes_to_hex(
             std::vector<std::uint8_t>(lockfile.begin(), lockfile.end()))
      << "\n";

  const std::vector<PackageDependency> dependencies =
      sorted_dependencies(artifact.manifest.dependencies);
  out << "dependency.count=" << dependencies.size() << "\n";
  for (std::size_t i = 0; i < dependencies.size(); ++i) {
    out << "dependency." << i << ".name=" << line_escape(dependencies[i].name)
        << "\n";
    out << "dependency." << i
        << ".version=" << line_escape(dependencies[i].version) << "\n";
    out << "dependency." << i
        << ".source=" << line_escape(dependencies[i].source) << "\n";
    out << "dependency." << i
        << ".checksum=" << line_escape(dependencies[i].checksum) << "\n";
  }

  const std::vector<capability::CapabilityRequest> capabilities =
      sorted_capabilities(artifact.manifest.capabilities);
  out << "capability.count=" << capabilities.size() << "\n";
  for (std::size_t i = 0; i < capabilities.size(); ++i) {
    out << "capability." << i << ".name=" << line_escape(capabilities[i].name)
        << "\n";
    out << "capability." << i
        << ".target=" << line_escape(capabilities[i].target) << "\n";
    out << "capability." << i
        << ".reason=" << line_escape(capabilities[i].reason) << "\n";
    out << "capability." << i << ".flags=" << capabilities[i].flags << "\n";
  }

  const std::vector<PackageModuleBlob> modules =
      sorted_module_blobs(artifact.modules);
  out << "module.count=" << modules.size() << "\n";
  for (std::size_t i = 0; i < modules.size(); ++i) {
    out << "module." << i << ".name=" << line_escape(modules[i].name) << "\n";
    out << "module." << i << ".path=" << line_escape(modules[i].path) << "\n";
    const std::string digest =
        modules[i].digest.empty()
            ? "sha256:" + sha256_hex_bytes(modules[i].bytes)
            : modules[i].digest;
    out << "module." << i << ".sha256=" << digest << "\n";
    out << "module." << i << ".bytes=" << bytes_to_hex(modules[i].bytes)
        << "\n";
  }
  return out.str();
}

std::string serialize_signed_package(const PackageArtifact &artifact) {
  std::string out = serialize_unsigned_package(artifact);
  if (!artifact.signature.digest.empty()) {
    out += "signature.algorithm=" + line_escape(artifact.signature.algorithm) +
           "\n";
    out += "signature.key_id=" + line_escape(artifact.signature.key_id) + "\n";
    out += "signature.digest=" + line_escape(artifact.signature.digest) + "\n";
  }
  return out;
}

bool read_file(const std::filesystem::path &path, std::string *out) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  *out = buffer.str();
  return true;
}

bool write_file(const std::filesystem::path &path, const std::string &value) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    return false;
  }
  output << value;
  return static_cast<bool>(output);
}

std::uint64_t parse_count(const std::map<std::string, std::string> &values,
                          const std::string &key, bool *ok) {
  const auto found = values.find(key);
  if (found == values.end()) {
    *ok = false;
    return 0;
  }
  try {
    std::size_t consumed = 0;
    const std::uint64_t count = std::stoull(found->second, &consumed, 10);
    if (consumed != found->second.size()) {
      *ok = false;
      return 0;
    }
    return count;
  } catch (const std::exception &) {
    *ok = false;
    return 0;
  }
}

bool get_escaped_value(const std::map<std::string, std::string> &values,
                       const std::string &key, std::string *out) {
  const auto found = values.find(key);
  if (found == values.end()) {
    return false;
  }
  return line_unescape(found->second, out);
}

PackageRegistryResult registry_write(const std::string &serialized,
                                     const std::string &registry_root,
                                     const std::string &signing_key,
                                     bool publish_mode) {
  PackageRegistryResult result;
  const PackageVerifyResult verified =
      verify_package_artifact(serialized, signing_key);
  if (!verified.ok) {
    result.diagnostics = verified.diagnostics;
    return result;
  }

  const PackageParseResult parsed = parse_package_artifact(serialized);
  if (!parsed.ok()) {
    result.diagnostics = parsed.diagnostics;
    return result;
  }
  const PackageArtifact &artifact = parsed.artifact;
  result.package_name = artifact.manifest.name;
  result.version = artifact.manifest.version;

  if (!safe_registry_component(artifact.manifest.name) ||
      !safe_registry_component(artifact.manifest.version)) {
    result.diagnostics.push_back(diagnostic(
        "PackageRegistryError",
        "package name and version must be registry-safe identifiers"));
    return result;
  }

  std::filesystem::path package_dir = std::filesystem::path(registry_root) /
                                      artifact.manifest.name /
                                      artifact.manifest.version;
  std::error_code error;
  std::filesystem::create_directories(package_dir, error);
  if (error) {
    result.diagnostics.push_back(
        diagnostic("PackageRegistryError",
                   "failed to create registry directory: " + error.message()));
    return result;
  }

  const std::filesystem::path package_path =
      package_dir /
      (artifact.manifest.name + "-" + artifact.manifest.version + ".amberpkg");
  if (std::filesystem::exists(package_path)) {
    std::string existing;
    if (!read_file(package_path, &existing)) {
      result.diagnostics.push_back(diagnostic(
          "PackageRegistryError", "failed to read existing registry artifact"));
      return result;
    }
    if (existing != serialized) {
      result.diagnostics.push_back(diagnostic(
          "PackageRegistryError",
          publish_mode ? "package version is already published"
                       : "installed package artifact differs from registry"));
      return result;
    }
    if (publish_mode) {
      result.diagnostics.push_back(diagnostic(
          "PackageRegistryError", "package version is already published"));
      return result;
    }
  } else if (!write_file(package_path, serialized)) {
    result.diagnostics.push_back(
        diagnostic("PackageRegistryError", "failed to write package artifact"));
    return result;
  }

  if (!write_file(package_dir / "amber.lock", artifact.lockfile) ||
      !write_file(package_dir / "manifest.json",
                  manifest_to_json(artifact.manifest))) {
    result.diagnostics.push_back(diagnostic(
        "PackageRegistryError", "failed to write registry metadata"));
    return result;
  }

  result.ok = true;
  result.installed_path = package_path.string();
  return result;
}

} // namespace

PackageManifestResult parse_manifest_toml(const std::string &source,
                                          const std::string &path) {
  enum class Section { None, Package, Modules, Dependencies, Capabilities };

  PackageManifestResult result;
  Section section = Section::None;
  PackageModule *current_module = nullptr;

  std::istringstream input(source);
  std::string raw_line;
  std::size_t line_no = 0;
  while (std::getline(input, raw_line)) {
    ++line_no;
    const std::string line = trim(strip_comment(raw_line));
    if (line.empty()) {
      continue;
    }
    if (line == "[package]") {
      section = Section::Package;
      current_module = nullptr;
      continue;
    }
    if (line == "[dependencies]") {
      section = Section::Dependencies;
      current_module = nullptr;
      continue;
    }
    if (line == "[capabilities]") {
      section = Section::Capabilities;
      current_module = nullptr;
      continue;
    }
    if (line == "[[modules]]" || line == "[[module]]") {
      result.manifest.modules.push_back({});
      current_module = &result.manifest.modules.back();
      section = Section::Modules;
      continue;
    }
    if (!line.empty() && line.front() == '[') {
      std::ostringstream message;
      message << "unsupported manifest section at line " << line_no << ": "
              << line;
      result.diagnostics.push_back(
          diagnostic("PackageManifestError", message.str(), path));
      continue;
    }

    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      std::ostringstream message;
      message << "expected key/value pair at line " << line_no;
      result.diagnostics.push_back(
          diagnostic("PackageManifestError", message.str(), path));
      continue;
    }
    const std::string key = trim(line.substr(0, equals));
    std::string value;
    if (!unquote_toml_string(line.substr(equals + 1U), &value)) {
      std::ostringstream message;
      message << "invalid string literal at line " << line_no;
      result.diagnostics.push_back(
          diagnostic("PackageManifestError", message.str(), path));
      continue;
    }

    switch (section) {
    case Section::Package:
      if (key == "name") {
        result.manifest.name = value;
      } else if (key == "version") {
        result.manifest.version = value;
      } else if (key == "root" || key == "root_module" || key == "module") {
        result.manifest.root_module = value;
      }
      break;
    case Section::Modules:
      if (current_module == nullptr) {
        result.diagnostics.push_back(diagnostic(
            "PackageManifestError",
            "module key/value pair must appear inside [[modules]]", path));
        break;
      }
      if (key == "name") {
        current_module->name = value;
      } else if (key == "path") {
        current_module->path = value;
      }
      break;
    case Section::Dependencies: {
      PackageDependency dependency;
      dependency.name = key;
      dependency.version = value;
      dependency.source = "registry";
      result.manifest.dependencies.push_back(std::move(dependency));
      break;
    }
    case Section::Capabilities: {
      const std::vector<std::string> names =
          capability::canonical_names_for_manifest_key(key);
      if (names.empty()) {
        result.diagnostics.push_back(
            diagnostic("PackageManifestError",
                       "unsupported capability key: " + key, path));
        break;
      }
      bool bool_value = false;
      if (parse_toml_bool(value, &bool_value)) {
        if (bool_value) {
          for (const std::string &name : names) {
            result.manifest.capabilities.push_back(capability::make_capability(
                name, "*", "manifest boolean request",
                capability::kCapabilityFlagWildcardTarget));
          }
        }
        break;
      }
      std::vector<std::string> targets;
      if (parse_toml_string_array(value, &targets)) {
        for (const std::string &target : targets) {
          for (const std::string &name : names) {
            result.manifest.capabilities.push_back(
                capability::make_capability(name, target));
          }
        }
        break;
      }
      std::string string_value;
      const std::string raw_value = trim(line.substr(equals + 1U));
      if (!raw_value.empty() && raw_value.front() == '"' &&
          unquote_toml_string(raw_value, &string_value)) {
        for (const std::string &name : names) {
          result.manifest.capabilities.push_back(
              capability::make_capability(name, string_value));
        }
        break;
      }
      result.diagnostics.push_back(
          diagnostic("PackageManifestError",
                     "capability values must be boolean, string, or string "
                     "array at line " +
                         std::to_string(line_no),
                     path));
      break;
    }
    case Section::None:
      result.diagnostics.push_back(diagnostic(
          "PackageManifestError",
          "manifest key/value pair must appear inside a section", path));
      break;
    }
  }

  if (result.manifest.root_module.empty() && !result.manifest.modules.empty()) {
    result.manifest.root_module = result.manifest.modules.front().name;
  }

  if (result.manifest.name.empty()) {
    result.diagnostics.push_back(
        diagnostic("PackageManifestError", "package.name is required", path));
  }
  if (result.manifest.version.empty()) {
    result.diagnostics.push_back(diagnostic(
        "PackageManifestError", "package.version is required", path));
  }
  if (result.manifest.modules.empty()) {
    result.diagnostics.push_back(
        diagnostic("PackageManifestError",
                   "at least one [[modules]] entry is required", path));
  }

  std::set<std::string> module_names;
  for (const PackageModule &module : result.manifest.modules) {
    if (module.name.empty() || module.path.empty()) {
      result.diagnostics.push_back(
          diagnostic("PackageManifestError",
                     "module entries require name and path", path));
      continue;
    }
    if (!module_names.insert(module.name).second) {
      result.diagnostics.push_back(
          diagnostic("PackageManifestError",
                     "duplicate module entry: " + module.name, path));
    }
  }
  if (!result.manifest.root_module.empty() &&
      module_names.find(result.manifest.root_module) == module_names.end()) {
    result.diagnostics.push_back(
        diagnostic("PackageManifestError",
                   "package root module is not declared in [[modules]]: " +
                       result.manifest.root_module,
                   path));
  }

  std::set<std::string> dependency_names;
  for (const PackageDependency &dependency : result.manifest.dependencies) {
    if (dependency.name.empty() || dependency.version.empty()) {
      result.diagnostics.push_back(
          diagnostic("PackageManifestError",
                     "dependencies require name and version", path));
      continue;
    }
    if (!dependency_names.insert(dependency.name).second) {
      result.diagnostics.push_back(
          diagnostic("PackageManifestError",
                     "duplicate dependency entry: " + dependency.name, path));
    }
  }

  result.manifest.modules = sorted_modules(result.manifest.modules);
  result.manifest.dependencies =
      sorted_dependencies(result.manifest.dependencies);
  result.manifest.capabilities =
      sorted_capabilities(result.manifest.capabilities);
  return result;
}

std::string manifest_to_json(const PackageManifest &manifest) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.package-manifest.v1\",\n";
  out << "  \"name\": \"" << json_escape(manifest.name) << "\",\n";
  out << "  \"version\": \"" << json_escape(manifest.version) << "\",\n";
  out << "  \"root_module\": \"" << json_escape(manifest.root_module)
      << "\",\n";
  out << "  \"modules\": [";
  const std::vector<PackageModule> modules = sorted_modules(manifest.modules);
  for (std::size_t i = 0; i < modules.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"name\":\"" << json_escape(modules[i].name)
        << "\",\"path\":\"" << json_escape(modules[i].path) << "\"}";
  }
  out << "\n  ],\n";
  out << "  \"dependencies\": [";
  const std::vector<PackageDependency> dependencies =
      sorted_dependencies(manifest.dependencies);
  for (std::size_t i = 0; i < dependencies.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"name\":\"" << json_escape(dependencies[i].name)
        << "\",\"version\":\"" << json_escape(dependencies[i].version)
        << "\",\"source\":\"" << json_escape(dependencies[i].source) << "\"}";
  }
  out << "\n  ],\n";
  out << "  \"capabilities\": [";
  const std::vector<capability::CapabilityRequest> capabilities =
      sorted_capabilities(manifest.capabilities);
  for (std::size_t i = 0; i < capabilities.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"name\":\"" << json_escape(capabilities[i].name)
        << "\",\"target\":\"" << json_escape(capabilities[i].target)
        << "\",\"reason\":\"" << json_escape(capabilities[i].reason)
        << "\",\"flags\":" << capabilities[i].flags << "}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

std::string render_lockfile(const PackageManifest &manifest) {
  std::ostringstream out;
  out << "# amber.lock v1\n";
  out << "schema = \"amber.lock.v1\"\n\n";
  out << "[package]\n";
  out << "name = \"" << json_escape(manifest.name) << "\"\n";
  out << "version = \"" << json_escape(manifest.version) << "\"\n";
  out << "root = \"" << json_escape(manifest.root_module) << "\"\n\n";
  const std::vector<PackageDependency> dependencies =
      sorted_dependencies(manifest.dependencies);
  for (const PackageDependency &dependency : dependencies) {
    out << "[[dependencies]]\n";
    out << "name = \"" << json_escape(dependency.name) << "\"\n";
    out << "version = \"" << json_escape(dependency.version) << "\"\n";
    out << "source = \"" << json_escape(dependency.source) << "\"\n";
    out << "checksum = \"" << dependency_checksum(dependency) << "\"\n\n";
  }
  const std::vector<capability::CapabilityRequest> capabilities =
      sorted_capabilities(manifest.capabilities);
  if (!capabilities.empty()) {
    for (const capability::CapabilityRequest &capability : capabilities) {
      out << "[[capabilities]]\n";
      out << "name = \"" << json_escape(capability.name) << "\"\n";
      out << "target = \"" << json_escape(capability.target) << "\"\n";
      out << "reason = \"" << json_escape(capability.reason) << "\"\n";
      out << "flags = " << capability.flags << "\n\n";
    }
  }
  return out.str();
}

PackageBuildResult
build_package_artifact(const PackageManifest &manifest,
                       const std::vector<PackageModuleBlob> &modules,
                       const PackageBuildOptions &options) {
  PackageBuildResult result;
  if (manifest.name.empty() || manifest.version.empty() ||
      manifest.root_module.empty()) {
    result.diagnostics.push_back(
        diagnostic("PackageBuildError",
                   "package manifest requires name, version, and root module"));
    return result;
  }

  std::map<std::string, PackageModuleBlob> blobs_by_name;
  for (const PackageModuleBlob &module : modules) {
    if (module.name.empty() || module.path.empty() || module.bytes.empty()) {
      result.diagnostics.push_back(diagnostic(
          "PackageBuildError",
          "module blobs require name, path, and non-empty bytecode"));
      continue;
    }
    if (!blobs_by_name.emplace(module.name, module).second) {
      result.diagnostics.push_back(diagnostic(
          "PackageBuildError", "duplicate module blob: " + module.name));
    }
  }
  for (const PackageModule &module : manifest.modules) {
    if (blobs_by_name.find(module.name) == blobs_by_name.end()) {
      result.diagnostics.push_back(diagnostic(
          "PackageBuildError", "missing bytecode for module: " + module.name));
    }
  }
  for (const capability::CapabilityRequest &request : manifest.capabilities) {
    if (!capability::valid_capability_name(request.name)) {
      result.diagnostics.push_back(diagnostic(
          "PackageBuildError", "invalid capability request: " + request.name));
    }
  }
  if (!result.diagnostics.empty()) {
    return result;
  }

  result.artifact.manifest = manifest;
  for (PackageDependency &dependency : result.artifact.manifest.dependencies) {
    if (dependency.source.empty()) {
      dependency.source = "registry";
    }
    dependency.checksum = dependency_checksum(dependency);
  }
  result.artifact.manifest.modules =
      sorted_modules(result.artifact.manifest.modules);
  result.artifact.manifest.dependencies =
      sorted_dependencies(result.artifact.manifest.dependencies);
  result.artifact.manifest.capabilities =
      sorted_capabilities(result.artifact.manifest.capabilities);
  result.artifact.manifest_digest =
      sha256_prefixed(canonical_manifest_text(result.artifact.manifest));
  result.artifact.lockfile = render_lockfile(result.artifact.manifest);
  result.artifact.lock_digest = sha256_prefixed(result.artifact.lockfile);
  for (const PackageModule &module : result.artifact.manifest.modules) {
    PackageModuleBlob blob = blobs_by_name.at(module.name);
    blob.digest = "sha256:" + sha256_hex_bytes(blob.bytes);
    result.artifact.modules.push_back(std::move(blob));
  }
  result.artifact.modules = sorted_module_blobs(result.artifact.modules);

  const std::string unsigned_payload =
      serialize_unsigned_package(result.artifact);
  if (!options.signing_key.empty()) {
    result.artifact.signature.algorithm = kSignatureAlgorithm;
    result.artifact.signature.key_id =
        options.key_id.empty() ? "default" : options.key_id;
    result.artifact.signature.digest =
        signature_digest(unsigned_payload, result.artifact.signature.key_id,
                         options.signing_key);
  }

  result.serialized = serialize_signed_package(result.artifact);
  result.ok = true;
  return result;
}

PackageParseResult parse_package_artifact(const std::string &serialized,
                                          const std::string &path) {
  PackageParseResult result;
  std::istringstream input(serialized);
  std::string first_line;
  if (!std::getline(input, first_line) || trim(first_line) != kPackageSchema) {
    result.diagnostics.push_back(
        diagnostic("PackageParseError",
                   "package artifact schema header is missing", path));
    return result;
  }

  std::map<std::string, std::string> values;
  std::string line;
  std::size_t line_no = 1;
  while (std::getline(input, line)) {
    ++line_no;
    if (line.empty()) {
      continue;
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      std::ostringstream message;
      message << "invalid package artifact line " << line_no;
      result.diagnostics.push_back(
          diagnostic("PackageParseError", message.str(), path));
      continue;
    }
    values[line.substr(0, equals)] = line.substr(equals + 1U);
  }
  if (!result.diagnostics.empty()) {
    return result;
  }

  if (!get_escaped_value(values, "package.name",
                         &result.artifact.manifest.name) ||
      !get_escaped_value(values, "package.version",
                         &result.artifact.manifest.version) ||
      !get_escaped_value(values, "package.root",
                         &result.artifact.manifest.root_module)) {
    result.diagnostics.push_back(
        diagnostic("PackageParseError",
                   "package artifact is missing package identity", path));
    return result;
  }
  result.artifact.manifest_digest = values["manifest.sha256"];
  result.artifact.lock_digest = values["lock.sha256"];

  const auto lock_it = values.find("lock.bytes");
  if (lock_it == values.end()) {
    result.diagnostics.push_back(
        diagnostic("PackageParseError",
                   "package artifact is missing lockfile bytes", path));
    return result;
  }
  std::vector<std::uint8_t> lock_bytes;
  if (!hex_to_bytes(lock_it->second, &lock_bytes)) {
    result.diagnostics.push_back(diagnostic(
        "PackageParseError", "package artifact lockfile hex is invalid", path));
    return result;
  }
  result.artifact.lockfile = bytes_to_string(lock_bytes);

  bool count_ok = true;
  const std::uint64_t dependency_count =
      parse_count(values, "dependency.count", &count_ok);
  const std::uint64_t module_count =
      parse_count(values, "module.count", &count_ok);
  std::uint64_t capability_count = 0;
  if (values.find("capability.count") != values.end()) {
    capability_count = parse_count(values, "capability.count", &count_ok);
  }
  if (!count_ok) {
    result.diagnostics.push_back(
        diagnostic("PackageParseError",
                   "package artifact has invalid entry counts", path));
    return result;
  }

  for (std::uint64_t i = 0; i < dependency_count; ++i) {
    PackageDependency dependency;
    const std::string prefix = "dependency." + std::to_string(i) + ".";
    if (!get_escaped_value(values, prefix + "name", &dependency.name) ||
        !get_escaped_value(values, prefix + "version", &dependency.version) ||
        !get_escaped_value(values, prefix + "source", &dependency.source) ||
        !get_escaped_value(values, prefix + "checksum", &dependency.checksum)) {
      result.diagnostics.push_back(
          diagnostic("PackageParseError",
                     "package artifact dependency is incomplete", path));
      return result;
    }
    result.artifact.manifest.dependencies.push_back(std::move(dependency));
  }

  for (std::uint64_t i = 0; i < capability_count; ++i) {
    capability::CapabilityRequest request;
    const std::string prefix = "capability." + std::to_string(i) + ".";
    if (!get_escaped_value(values, prefix + "name", &request.name) ||
        !get_escaped_value(values, prefix + "target", &request.target) ||
        !get_escaped_value(values, prefix + "reason", &request.reason)) {
      result.diagnostics.push_back(
          diagnostic("PackageParseError",
                     "package artifact capability is incomplete", path));
      return result;
    }
    bool flags_ok = true;
    request.flags = static_cast<std::uint32_t>(
        parse_count(values, prefix + "flags", &flags_ok));
    if (!flags_ok || !capability::valid_capability_name(request.name)) {
      result.diagnostics.push_back(diagnostic(
          "PackageParseError", "package artifact capability is invalid", path));
      return result;
    }
    result.artifact.manifest.capabilities.push_back(std::move(request));
  }

  for (std::uint64_t i = 0; i < module_count; ++i) {
    PackageModuleBlob module;
    const std::string prefix = "module." + std::to_string(i) + ".";
    if (!get_escaped_value(values, prefix + "name", &module.name) ||
        !get_escaped_value(values, prefix + "path", &module.path) ||
        !get_escaped_value(values, prefix + "sha256", &module.digest)) {
      result.diagnostics.push_back(diagnostic(
          "PackageParseError", "package artifact module is incomplete", path));
      return result;
    }
    const auto bytes_it = values.find(prefix + "bytes");
    if (bytes_it == values.end() ||
        !hex_to_bytes(bytes_it->second, &module.bytes)) {
      result.diagnostics.push_back(
          diagnostic("PackageParseError",
                     "package artifact module bytes are invalid", path));
      return result;
    }
    result.artifact.manifest.modules.push_back({module.name, module.path});
    result.artifact.modules.push_back(std::move(module));
  }

  get_escaped_value(values, "signature.algorithm",
                    &result.artifact.signature.algorithm);
  get_escaped_value(values, "signature.key_id",
                    &result.artifact.signature.key_id);
  get_escaped_value(values, "signature.digest",
                    &result.artifact.signature.digest);

  result.artifact.manifest.modules =
      sorted_modules(result.artifact.manifest.modules);
  result.artifact.manifest.dependencies =
      sorted_dependencies(result.artifact.manifest.dependencies);
  result.artifact.manifest.capabilities =
      sorted_capabilities(result.artifact.manifest.capabilities);
  result.artifact.modules = sorted_module_blobs(result.artifact.modules);
  return result;
}

PackageVerifyResult verify_package_artifact(const std::string &serialized,
                                            const std::string &signing_key,
                                            const std::string &path) {
  PackageVerifyResult result;
  const PackageParseResult parsed = parse_package_artifact(serialized, path);
  if (!parsed.ok()) {
    result.diagnostics = parsed.diagnostics;
    return result;
  }
  const PackageArtifact &artifact = parsed.artifact;
  result.structurally_valid = true;
  result.package_name = artifact.manifest.name;
  result.version = artifact.manifest.version;
  result.root_module = artifact.manifest.root_module;
  result.digest = sha256_prefixed(serialized);

  const std::string expected_manifest_digest =
      sha256_prefixed(canonical_manifest_text(artifact.manifest));
  if (artifact.manifest_digest != expected_manifest_digest) {
    result.diagnostics.push_back(diagnostic(
        "PackageVerifyError", "package manifest digest does not match", path));
  }
  const std::string expected_lock_digest = sha256_prefixed(artifact.lockfile);
  if (artifact.lock_digest != expected_lock_digest) {
    result.diagnostics.push_back(diagnostic(
        "PackageVerifyError", "package lockfile digest does not match", path));
  }
  for (const PackageDependency &dependency : artifact.manifest.dependencies) {
    const std::string expected_checksum = dependency_checksum(dependency);
    if (dependency.checksum != expected_checksum) {
      result.diagnostics.push_back(diagnostic(
          "PackageVerifyError",
          "dependency checksum does not match: " + dependency.name, path));
    }
  }
  for (const PackageModuleBlob &module : artifact.modules) {
    if (module.bytes.empty()) {
      result.diagnostics.push_back(
          diagnostic("PackageVerifyError",
                     "package module is empty: " + module.name, path));
    }
    const std::string expected_digest =
        "sha256:" + sha256_hex_bytes(module.bytes);
    if (module.digest != expected_digest) {
      result.diagnostics.push_back(
          diagnostic("PackageVerifyError",
                     "module digest does not match: " + module.name, path));
    }
  }

  result.signature_present = !artifact.signature.digest.empty();
  if (result.signature_present) {
    if (artifact.signature.algorithm != kSignatureAlgorithm) {
      result.diagnostics.push_back(
          diagnostic("PackageVerifyError",
                     "unsupported package signature algorithm: " +
                         artifact.signature.algorithm,
                     path));
    }
    if (!signing_key.empty()) {
      result.signature_checked = true;
      const std::string unsigned_payload = serialize_unsigned_package(artifact);
      const std::string expected_signature = signature_digest(
          unsigned_payload, artifact.signature.key_id, signing_key);
      result.signature_valid = artifact.signature.digest == expected_signature;
      if (!result.signature_valid) {
        result.diagnostics.push_back(diagnostic(
            "PackageVerifyError", "package signature does not match", path));
      }
    }
  }

  result.ok = result.diagnostics.empty();
  return result;
}

PackageRegistryResult install_package_artifact(const std::string &serialized,
                                               const std::string &registry_root,
                                               const std::string &signing_key) {
  return registry_write(serialized, registry_root, signing_key, false);
}

PackageRegistryResult publish_package_artifact(const std::string &serialized,
                                               const std::string &registry_root,
                                               const std::string &signing_key) {
  return registry_write(serialized, registry_root, signing_key, true);
}

std::string artifact_to_json(const PackageArtifact &artifact) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.pkg.inspect.v1\",\n";
  out << "  \"package\": {\"name\":\"" << json_escape(artifact.manifest.name)
      << "\",\"version\":\"" << json_escape(artifact.manifest.version)
      << "\",\"root_module\":\"" << json_escape(artifact.manifest.root_module)
      << "\"},\n";
  out << "  \"manifest_sha256\": \"" << json_escape(artifact.manifest_digest)
      << "\",\n";
  out << "  \"lock_sha256\": \"" << json_escape(artifact.lock_digest)
      << "\",\n";
  out << "  \"modules\": [";
  const std::vector<PackageModuleBlob> modules =
      sorted_module_blobs(artifact.modules);
  for (std::size_t i = 0; i < modules.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    const std::string digest =
        modules[i].digest.empty()
            ? "sha256:" + sha256_hex_bytes(modules[i].bytes)
            : modules[i].digest;
    out << "\n    {\"name\":\"" << json_escape(modules[i].name)
        << "\",\"path\":\"" << json_escape(modules[i].path)
        << "\",\"sha256\":\"" << json_escape(digest)
        << "\",\"size\":" << modules[i].bytes.size() << "}";
  }
  out << "\n  ],\n";
  out << "  \"dependencies\": [";
  const std::vector<PackageDependency> dependencies =
      sorted_dependencies(artifact.manifest.dependencies);
  for (std::size_t i = 0; i < dependencies.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"name\":\"" << json_escape(dependencies[i].name)
        << "\",\"version\":\"" << json_escape(dependencies[i].version)
        << "\",\"source\":\"" << json_escape(dependencies[i].source)
        << "\",\"checksum\":\"" << json_escape(dependencies[i].checksum)
        << "\"}";
  }
  out << "\n  ],\n";
  out << "  \"capabilities\": [";
  const std::vector<capability::CapabilityRequest> capabilities =
      sorted_capabilities(artifact.manifest.capabilities);
  for (std::size_t i = 0; i < capabilities.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"name\":\"" << json_escape(capabilities[i].name)
        << "\",\"target\":\"" << json_escape(capabilities[i].target)
        << "\",\"reason\":\"" << json_escape(capabilities[i].reason)
        << "\",\"flags\":" << capabilities[i].flags << "}";
  }
  out << "\n  ],\n";
  out << "  \"signature\": {\"present\":"
      << (artifact.signature.digest.empty() ? "false" : "true");
  if (!artifact.signature.digest.empty()) {
    out << ",\"algorithm\":\"" << json_escape(artifact.signature.algorithm)
        << "\",\"key_id\":\"" << json_escape(artifact.signature.key_id)
        << "\",\"digest\":\"" << json_escape(artifact.signature.digest) << "\"";
  }
  out << "}\n";
  out << "}\n";
  return out.str();
}

std::string verify_result_to_json(const PackageVerifyResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.pkg.verify.v1\",\n";
  out << "  \"status\": \"" << (result.ok ? "ok" : "error") << "\",\n";
  out << "  \"package\": \"" << json_escape(result.package_name) << "\",\n";
  out << "  \"version\": \"" << json_escape(result.version) << "\",\n";
  out << "  \"root_module\": \"" << json_escape(result.root_module) << "\",\n";
  out << "  \"digest\": \"" << json_escape(result.digest) << "\",\n";
  out << "  \"signature_present\": "
      << (result.signature_present ? "true" : "false") << ",\n";
  out << "  \"signature_checked\": "
      << (result.signature_checked ? "true" : "false") << ",\n";
  out << "  \"signature_valid\": "
      << (result.signature_valid ? "true" : "false") << ",\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"error_name\":\""
        << json_escape(result.diagnostics[i].error_name) << "\",\"message\":\""
        << json_escape(result.diagnostics[i].message) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

std::string registry_result_to_json(const PackageRegistryResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.pkg.registry.v1\",\n";
  out << "  \"status\": \"" << (result.ok ? "ok" : "error") << "\",\n";
  out << "  \"package\": \"" << json_escape(result.package_name) << "\",\n";
  out << "  \"version\": \"" << json_escape(result.version) << "\",\n";
  out << "  \"path\": \"" << json_escape(result.installed_path) << "\",\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    out << "\n    {\"error_name\":\""
        << json_escape(result.diagnostics[i].error_name) << "\",\"message\":\""
        << json_escape(result.diagnostics[i].message) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

} // namespace amber::pkg
