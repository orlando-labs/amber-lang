#include "frozen/image.h"

#include "bytecode/format.h"
#include "frontend/lexer/token.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace amber::frozen {

namespace {

constexpr const char *kImageSchema = "amber.image.v1";

FrozenImageDiagnostic diagnostic(std::string error_name, std::string message,
                                 std::string module_name = {}) {
  FrozenImageDiagnostic out;
  out.error_name = std::move(error_name);
  out.message = std::move(message);
  out.module_name = std::move(module_name);
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

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (const unsigned char c : value) {
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
      if (c < 0x20U) {
        const char *hex = "0123456789abcdef";
        out << "\\u00" << hex[(c >> 4U) & 0x0FU] << hex[c & 0x0FU];
      } else {
        out << static_cast<char>(c);
      }
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
  *out = std::move(decoded);
  return true;
}

std::string bytes_to_string(const std::vector<std::uint8_t> &bytes) {
  return std::string(bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> string_to_bytes(const std::string &value) {
  return std::vector<std::uint8_t>(value.begin(), value.end());
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

std::string sha256_prefixed(const std::string &value) {
  return "sha256:" + amber::lexer::sha256_hex(value);
}

std::string sha256_prefixed_bytes(const std::vector<std::uint8_t> &bytes) {
  return sha256_prefixed(bytes_to_string(bytes));
}

bool parse_u64(const std::string &value, std::uint64_t *out) {
  try {
    std::size_t consumed = 0;
    const std::uint64_t parsed = std::stoull(value, &consumed, 10);
    if (consumed != value.size()) {
      return false;
    }
    *out = parsed;
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

bool parse_u32(const std::string &value, std::uint32_t *out) {
  std::uint64_t parsed = 0;
  if (!parse_u64(value, &parsed) || parsed > UINT32_MAX) {
    return false;
  }
  *out = static_cast<std::uint32_t>(parsed);
  return true;
}

bool parse_count(const std::map<std::string, std::string> &values,
                 const std::string &key, std::uint64_t *out) {
  const auto found = values.find(key);
  if (found == values.end()) {
    return false;
  }
  return parse_u64(found->second, out);
}

bool get_escaped_value(const std::map<std::string, std::string> &values,
                       const std::string &key, std::string *out) {
  const auto found = values.find(key);
  if (found == values.end()) {
    return false;
  }
  return line_unescape(found->second, out);
}

bool get_bool_value(const std::map<std::string, std::string> &values,
                    const std::string &key, bool *out) {
  const auto found = values.find(key);
  if (found == values.end()) {
    return false;
  }
  if (found->second == "true") {
    *out = true;
    return true;
  }
  if (found->second == "false") {
    *out = false;
    return true;
  }
  return false;
}

std::vector<pkg::PackageModuleBlob>
sorted_module_blobs(std::vector<pkg::PackageModuleBlob> modules) {
  std::sort(modules.begin(), modules.end(),
            [](const pkg::PackageModuleBlob &left,
               const pkg::PackageModuleBlob &right) {
              return left.name < right.name;
            });
  return modules;
}

std::vector<FrozenImageNativeModule>
sorted_native_modules(std::vector<FrozenImageNativeModule> modules) {
  std::sort(modules.begin(), modules.end(),
            [](const FrozenImageNativeModule &left,
               const FrozenImageNativeModule &right) {
              return left.module_name < right.module_name;
            });
  return modules;
}

std::vector<pkg::PackageNativeExtensionMetadata> sorted_native_extensions(
    std::vector<pkg::PackageNativeExtensionMetadata> extensions) {
  std::sort(extensions.begin(), extensions.end(),
            [](const pkg::PackageNativeExtensionMetadata &left,
               const pkg::PackageNativeExtensionMetadata &right) {
              return left.name < right.name;
            });
  return extensions;
}

bool same_native_type(const pkg::PackageNativeType &left,
                      const pkg::PackageNativeType &right) {
  return left.amber == right.amber && left.tag == right.tag &&
         left.ownership == right.ownership &&
         left.destructor == right.destructor;
}

bool same_native_error(const pkg::PackageNativeError &left,
                       const pkg::PackageNativeError &right) {
  return left.name == right.name && left.parent == right.parent &&
         left.default_message == right.default_message &&
         left.default_exit_code == right.default_exit_code;
}

const pkg::PackageModuleBlob *
package_blob_by_name(const pkg::PackageArtifact &artifact,
                     const std::string &module_name) {
  for (const pkg::PackageModuleBlob &blob : artifact.modules) {
    if (blob.name == module_name) {
      return &blob;
    }
  }
  return nullptr;
}

const pkg::PackageModule *
manifest_module_by_name(const pkg::PackageManifest &manifest,
                        const std::string &module_name) {
  for (const pkg::PackageModule &module : manifest.modules) {
    if (module.name == module_name) {
      return &module;
    }
  }
  return nullptr;
}

bool native_requires_frozen_world(const native::NativeModule &module) {
  if (!module.requires_frozen_world) {
    return false;
  }
  for (const native::NativeCodeObject &code : module.code_objects) {
    if (!code.requires_frozen_world) {
      return false;
    }
  }
  return true;
}

void append_analysis(FrozenImageArtifact *artifact, std::string check,
                     std::string status, std::string module_name,
                     std::string message) {
  artifact->analysis.push_back({std::move(check), std::move(status),
                                std::move(module_name), std::move(message)});
}

void append_native_diagnostics(
    std::vector<FrozenImageDiagnostic> *diagnostics,
    const std::vector<native::NativeDiagnostic> &native_diagnostics,
    const std::string &module_name) {
  for (const native::NativeDiagnostic &entry : native_diagnostics) {
    diagnostics->push_back(diagnostic(
        "FrozenNativeError", entry.code + ": " + entry.message, module_name));
  }
}

bool metadata_declares_native_readiness(const std::string &metadata_json) {
  return metadata_json.find("\"format\": \"amber.native.v1\"") !=
             std::string::npos &&
         metadata_json.find("\"requires_frozen_world\": true") !=
             std::string::npos &&
         metadata_json.find("\"slowpath_table\"") != std::string::npos &&
         metadata_json.find("\"assumption_invalidation\"") !=
             std::string::npos &&
         metadata_json.find("\"may_reenter_bytecode\":true") !=
             std::string::npos &&
         metadata_json.find("\"root_maps\"") != std::string::npos &&
         metadata_json.find("\"exception_maps\"") != std::string::npos &&
         metadata_json.find("\"safepoint_maps\"") != std::string::npos &&
         metadata_json.find("\"world_epoch_assumptions\"") != std::string::npos;
}

} // namespace

FrozenImageBuildResult build_frozen_image_artifact(
    const pkg::PackageArtifact &package_artifact,
    const std::string &serialized_package,
    const std::vector<native::NativeModule> &native_modules,
    const FrozenImageBuildOptions &options) {
  FrozenImageBuildResult result;
  result.artifact.package = package_artifact;
  result.artifact.serialized_package = serialized_package;
  result.artifact.package_digest = sha256_prefixed(serialized_package);
  result.artifact.world_epoch =
      options.world_epoch == 0 ? 1 : options.world_epoch;

  const pkg::PackageVerifyResult package_verify =
      pkg::verify_package_artifact(serialized_package);
  if (!package_verify.ok) {
    for (const pkg::PackageDiagnostic &entry : package_verify.diagnostics) {
      result.diagnostics.push_back(diagnostic(
          "FrozenPackageError", entry.error_name + ": " + entry.message));
    }
    append_analysis(&result.artifact, "package_verify", "error", {},
                    "embedded package artifact does not verify");
  } else {
    append_analysis(&result.artifact, "package_verify", "ok", {},
                    "embedded package artifact verifies structurally");
  }

  std::map<std::string, native::NativeModule> native_by_name;
  for (const native::NativeModule &module : native_modules) {
    if (module.module_name.empty()) {
      result.diagnostics.push_back(
          diagnostic("FrozenImageBuildError", "native module name is empty"));
      continue;
    }
    if (!native_by_name.emplace(module.module_name, module).second) {
      result.diagnostics.push_back(
          diagnostic("FrozenImageBuildError",
                     "duplicate native module: " + module.module_name,
                     module.module_name));
    }
  }

  if (package_artifact.manifest.root_module.empty() ||
      package_blob_by_name(package_artifact,
                           package_artifact.manifest.root_module) == nullptr) {
    result.diagnostics.push_back(
        diagnostic("FrozenImageBuildError",
                   "package root module is missing: " +
                       package_artifact.manifest.root_module,
                   package_artifact.manifest.root_module));
    append_analysis(&result.artifact, "root_module", "error",
                    package_artifact.manifest.root_module,
                    "root module is not present in package");
  } else {
    append_analysis(&result.artifact, "root_module", "ok",
                    package_artifact.manifest.root_module,
                    "root module is present in package");
  }

  for (const pkg::PackageModuleBlob &blob :
       sorted_module_blobs(package_artifact.modules)) {
    const bytecode::DecodeResult decoded =
        bytecode::deserialize_module(blob.bytes);
    if (!decoded.ok()) {
      result.diagnostics.push_back(diagnostic(
          "FrozenBytecodeError",
          bytecode::verify_errors_to_json(decoded.errors), blob.name));
      append_analysis(&result.artifact, "bytecode_verify", "error", blob.name,
                      "bytecode verification failed");
      continue;
    }
    append_analysis(&result.artifact, "bytecode_verify", "ok", blob.name,
                    "bytecode module verifies");

    const auto native_found = native_by_name.find(blob.name);
    if (native_found == native_by_name.end()) {
      result.diagnostics.push_back(
          diagnostic("FrozenImageBuildError",
                     "missing native module: " + blob.name, blob.name));
      append_analysis(&result.artifact, "native_verify", "error", blob.name,
                      "native module is missing");
      continue;
    }

    const native::NativeModule &native_module = native_found->second;
    const native::NativeValidationResult native_validation =
        native::validate_native_module(native_module, &decoded.module);
    if (!native_validation.ok()) {
      append_native_diagnostics(&result.diagnostics,
                                native_validation.diagnostics, blob.name);
      append_analysis(&result.artifact, "native_verify", "error", blob.name,
                      "native metadata verification failed");
      continue;
    }
    if (!native_requires_frozen_world(native_module)) {
      result.diagnostics.push_back(
          diagnostic("FrozenImageBuildError",
                     "native module must require a frozen world", blob.name));
      append_analysis(&result.artifact, "native_frozen_assumptions", "error",
                      blob.name,
                      "native module does not require frozen-world guards");
      continue;
    }

    FrozenImageNativeModule summary;
    summary.module_name = blob.name;
    if (const pkg::PackageModule *manifest_module =
            manifest_module_by_name(package_artifact.manifest, blob.name)) {
      summary.module_path = manifest_module->path;
    } else {
      summary.module_path = blob.path;
    }
    summary.format = native_module.format;
    summary.requires_frozen_world = native_module.requires_frozen_world;
    summary.code_object_count =
        static_cast<std::uint32_t>(native_module.code_objects.size());
    summary.native_module = native_module;
    summary.metadata_json = native::module_to_json(native_module, blob.digest);
    summary.metadata_digest = sha256_prefixed(summary.metadata_json);
    result.artifact.native_modules.push_back(std::move(summary));
    append_analysis(&result.artifact, "native_verify", "ok", blob.name,
                    "native metadata verifies against bytecode");
    append_analysis(&result.artifact, "native_frozen_assumptions", "ok",
                    blob.name, "native metadata requires frozen-world guards");
  }

  append_analysis(&result.artifact, "world_barrier", "ok", {},
                  "image load must freeze the runtime world before execution");
  append_analysis(&result.artifact, "reload_barrier", "ok", {},
                  "frozen runtime world rejects package hot reload");

  result.artifact.native_modules =
      sorted_native_modules(result.artifact.native_modules);
  result.artifact.native_extensions =
      sorted_native_extensions(package_artifact.native_extensions);
  if (!result.diagnostics.empty()) {
    return result;
  }
  result.serialized = serialize_frozen_image_artifact(result.artifact);
  result.artifact.image_digest = sha256_prefixed(result.serialized);
  result.ok = true;
  return result;
}

std::string
serialize_frozen_image_artifact(const FrozenImageArtifact &artifact) {
  std::ostringstream out;
  out << kImageSchema << "\n";
  out << "package.sha256=" << line_escape(artifact.package_digest) << "\n";
  out << "package.bytes="
      << bytes_to_hex(string_to_bytes(artifact.serialized_package)) << "\n";
  out << "world.epoch=" << artifact.world_epoch << "\n";

  const std::vector<FrozenImageNativeModule> modules =
      sorted_native_modules(artifact.native_modules);
  out << "module.count=" << modules.size() << "\n";
  for (std::size_t i = 0; i < modules.size(); ++i) {
    const FrozenImageNativeModule &module = modules[i];
    const std::string prefix = "module." + std::to_string(i) + ".";
    out << prefix << "name=" << line_escape(module.module_name) << "\n";
    out << prefix << "path=" << line_escape(module.module_path) << "\n";
    out << prefix << "native.format=" << line_escape(module.format) << "\n";
    out << prefix << "native.requires_frozen_world="
        << (module.requires_frozen_world ? "true" : "false") << "\n";
    out << prefix << "native.code_objects=" << module.code_object_count << "\n";
    out << prefix << "native.sha256=" << line_escape(module.metadata_digest)
        << "\n";
    out << prefix << "native.bytes="
        << bytes_to_hex(string_to_bytes(module.metadata_json)) << "\n";
  }

  const std::vector<pkg::PackageNativeExtensionMetadata> extensions =
      sorted_native_extensions(artifact.native_extensions);
  out << "native_extension.count=" << extensions.size() << "\n";
  for (std::size_t i = 0; i < extensions.size(); ++i) {
    const pkg::PackageNativeExtensionMetadata &extension = extensions[i];
    const std::string prefix =
        "native_extension." + std::to_string(i) + ".";
    out << prefix << "name=" << line_escape(extension.name) << "\n";
    out << prefix << "amber_ext_abi_version="
        << extension.amber_ext_abi_version << "\n";
    out << prefix << "target_triple="
        << line_escape(extension.target_triple) << "\n";
    out << prefix << "native_source_sha256="
        << line_escape(extension.native_source_digest) << "\n";
    out << prefix << "exported_symbol_sha256="
        << line_escape(extension.exported_symbol_digest) << "\n";
    out << prefix << "type.count=" << extension.types.size() << "\n";
    for (std::size_t j = 0; j < extension.types.size(); ++j) {
      out << prefix << "type." << j
          << ".amber=" << line_escape(extension.types[j].amber) << "\n";
      out << prefix << "type." << j
          << ".tag=" << line_escape(extension.types[j].tag) << "\n";
      out << prefix << "type." << j
          << ".ownership=" << line_escape(extension.types[j].ownership)
          << "\n";
      out << prefix << "type." << j
          << ".destructor=" << line_escape(extension.types[j].destructor)
          << "\n";
    }
    out << prefix << "error.count=" << extension.errors.size() << "\n";
    for (std::size_t j = 0; j < extension.errors.size(); ++j) {
      out << prefix << "error." << j
          << ".name=" << line_escape(extension.errors[j].name) << "\n";
      out << prefix << "error." << j
          << ".parent=" << line_escape(extension.errors[j].parent) << "\n";
      out << prefix << "error." << j
          << ".default_message="
          << line_escape(extension.errors[j].default_message) << "\n";
      out << prefix << "error." << j
          << ".default_exit_code="
          << line_escape(extension.errors[j].default_exit_code) << "\n";
    }
  }

  out << "analysis.count=" << artifact.analysis.size() << "\n";
  for (std::size_t i = 0; i < artifact.analysis.size(); ++i) {
    const FrozenImageAnalysisEntry &entry = artifact.analysis[i];
    const std::string prefix = "analysis." + std::to_string(i) + ".";
    out << prefix << "check=" << line_escape(entry.check) << "\n";
    out << prefix << "status=" << line_escape(entry.status) << "\n";
    out << prefix << "module=" << line_escape(entry.module_name) << "\n";
    out << prefix << "message=" << line_escape(entry.message) << "\n";
  }
  return out.str();
}

FrozenImageParseResult
parse_frozen_image_artifact(const std::string &serialized,
                            const std::string &path) {
  FrozenImageParseResult result;
  result.artifact.image_digest = sha256_prefixed(serialized);

  std::istringstream input(serialized);
  std::string first_line;
  if (!std::getline(input, first_line) || trim(first_line) != kImageSchema) {
    result.diagnostics.push_back(diagnostic(
        "FrozenImageParseError", "frozen image schema header is missing"));
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
      message << "invalid frozen image line " << line_no;
      result.diagnostics.push_back(
          diagnostic("FrozenImageParseError", message.str()));
      continue;
    }
    values[line.substr(0, equals)] = line.substr(equals + 1U);
  }
  if (!result.diagnostics.empty()) {
    return result;
  }

  if (!get_escaped_value(values, "package.sha256",
                         &result.artifact.package_digest)) {
    result.diagnostics.push_back(diagnostic(
        "FrozenImageParseError", "frozen image is missing package digest"));
    return result;
  }

  const auto package_bytes_it = values.find("package.bytes");
  if (package_bytes_it == values.end()) {
    result.diagnostics.push_back(diagnostic(
        "FrozenImageParseError", "frozen image is missing package bytes"));
    return result;
  }
  std::vector<std::uint8_t> package_bytes;
  if (!hex_to_bytes(package_bytes_it->second, &package_bytes)) {
    result.diagnostics.push_back(diagnostic(
        "FrozenImageParseError", "frozen image package bytes are invalid"));
    return result;
  }
  result.artifact.serialized_package = bytes_to_string(package_bytes);
  const pkg::PackageParseResult package =
      pkg::parse_package_artifact(result.artifact.serialized_package, path);
  if (!package.ok()) {
    for (const pkg::PackageDiagnostic &entry : package.diagnostics) {
      result.diagnostics.push_back(diagnostic(
          "FrozenPackageError", entry.error_name + ": " + entry.message));
    }
    return result;
  }
  result.artifact.package = package.artifact;

  const auto epoch_it = values.find("world.epoch");
  if (epoch_it == values.end() ||
      !parse_u64(epoch_it->second, &result.artifact.world_epoch) ||
      result.artifact.world_epoch == 0) {
    result.diagnostics.push_back(diagnostic(
        "FrozenImageParseError", "frozen image has invalid world epoch"));
    return result;
  }

  std::uint64_t module_count = 0;
  if (!parse_count(values, "module.count", &module_count)) {
    result.diagnostics.push_back(diagnostic(
        "FrozenImageParseError", "frozen image has invalid module count"));
    return result;
  }
  for (std::uint64_t i = 0; i < module_count; ++i) {
    FrozenImageNativeModule module;
    const std::string prefix = "module." + std::to_string(i) + ".";
    if (!get_escaped_value(values, prefix + "name", &module.module_name) ||
        !get_escaped_value(values, prefix + "path", &module.module_path) ||
        !get_escaped_value(values, prefix + "native.format", &module.format) ||
        !get_bool_value(values, prefix + "native.requires_frozen_world",
                        &module.requires_frozen_world) ||
        !get_escaped_value(values, prefix + "native.sha256",
                           &module.metadata_digest)) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageParseError", "frozen image native module is incomplete"));
      continue;
    }
    if (!parse_u32(values[prefix + "native.code_objects"],
                   &module.code_object_count)) {
      result.diagnostics.push_back(
          diagnostic("FrozenImageParseError",
                     "frozen image native code object count is invalid",
                     module.module_name));
      continue;
    }
    const auto native_bytes_it = values.find(prefix + "native.bytes");
    if (native_bytes_it == values.end()) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageParseError", "frozen image native bytes are missing",
          module.module_name));
      continue;
    }
    std::vector<std::uint8_t> native_bytes;
    if (!hex_to_bytes(native_bytes_it->second, &native_bytes)) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageParseError", "frozen image native bytes are invalid",
          module.module_name));
      continue;
    }
    module.metadata_json = bytes_to_string(native_bytes);
    result.artifact.native_modules.push_back(std::move(module));
  }

  std::uint64_t native_extension_count = 0;
  if (parse_count(values, "native_extension.count",
                  &native_extension_count)) {
    for (std::uint64_t i = 0; i < native_extension_count; ++i) {
      pkg::PackageNativeExtensionMetadata extension;
      const std::string prefix =
          "native_extension." + std::to_string(i) + ".";
      if (!get_escaped_value(values, prefix + "name", &extension.name) ||
          !get_escaped_value(values, prefix + "target_triple",
                             &extension.target_triple) ||
          !get_escaped_value(values, prefix + "native_source_sha256",
                             &extension.native_source_digest) ||
          !get_escaped_value(values, prefix + "exported_symbol_sha256",
                             &extension.exported_symbol_digest) ||
          !parse_u32(values[prefix + "amber_ext_abi_version"],
                     &extension.amber_ext_abi_version)) {
        result.diagnostics.push_back(diagnostic(
            "FrozenImageParseError",
            "frozen image native extension is incomplete"));
        continue;
      }
      std::uint64_t type_count = 0;
      if (!parse_count(values, prefix + "type.count", &type_count)) {
        result.diagnostics.push_back(diagnostic(
            "FrozenImageParseError",
            "frozen image native extension type count is invalid",
            extension.name));
        continue;
      }
      for (std::uint64_t j = 0; j < type_count; ++j) {
        pkg::PackageNativeType type;
        const std::string entry =
            prefix + "type." + std::to_string(j) + ".";
        if (!get_escaped_value(values, entry + "amber", &type.amber) ||
            !get_escaped_value(values, entry + "tag", &type.tag) ||
            !get_escaped_value(values, entry + "ownership",
                               &type.ownership) ||
            !get_escaped_value(values, entry + "destructor",
                               &type.destructor)) {
          result.diagnostics.push_back(diagnostic(
              "FrozenImageParseError",
              "frozen image native extension type is incomplete",
              extension.name));
          continue;
        }
        extension.types.push_back(std::move(type));
      }
      std::uint64_t error_count = 0;
      if (!parse_count(values, prefix + "error.count", &error_count)) {
        result.diagnostics.push_back(diagnostic(
            "FrozenImageParseError",
            "frozen image native extension error count is invalid",
            extension.name));
        continue;
      }
      for (std::uint64_t j = 0; j < error_count; ++j) {
        pkg::PackageNativeError error;
        const std::string entry =
            prefix + "error." + std::to_string(j) + ".";
        if (!get_escaped_value(values, entry + "name", &error.name) ||
            !get_escaped_value(values, entry + "parent", &error.parent) ||
            !get_escaped_value(values, entry + "default_message",
                               &error.default_message) ||
            !get_escaped_value(values, entry + "default_exit_code",
                               &error.default_exit_code)) {
          result.diagnostics.push_back(diagnostic(
              "FrozenImageParseError",
              "frozen image native extension error is incomplete",
              extension.name));
          continue;
        }
        extension.errors.push_back(std::move(error));
      }
      result.artifact.native_extensions.push_back(std::move(extension));
    }
  }

  std::uint64_t analysis_count = 0;
  if (parse_count(values, "analysis.count", &analysis_count)) {
    for (std::uint64_t i = 0; i < analysis_count; ++i) {
      FrozenImageAnalysisEntry entry;
      const std::string prefix = "analysis." + std::to_string(i) + ".";
      if (!get_escaped_value(values, prefix + "check", &entry.check) ||
          !get_escaped_value(values, prefix + "status", &entry.status) ||
          !get_escaped_value(values, prefix + "module", &entry.module_name) ||
          !get_escaped_value(values, prefix + "message", &entry.message)) {
        result.diagnostics.push_back(
            diagnostic("FrozenImageParseError",
                       "frozen image analysis entry is incomplete"));
        continue;
      }
      result.artifact.analysis.push_back(std::move(entry));
    }
  }

  result.artifact.native_extensions =
      sorted_native_extensions(result.artifact.native_extensions);
  return result;
}

FrozenImageVerifyResult
verify_frozen_image_artifact(const std::string &serialized,
                             const std::string &signing_key,
                             const std::string &path) {
  FrozenImageVerifyResult result;
  result.digest = sha256_prefixed(serialized);
  const FrozenImageParseResult parsed =
      parse_frozen_image_artifact(serialized, path);
  if (!parsed.ok()) {
    result.diagnostics = parsed.diagnostics;
    return result;
  }

  const FrozenImageArtifact &artifact = parsed.artifact;
  result.structurally_valid = true;
  result.package_name = artifact.package.manifest.name;
  result.version = artifact.package.manifest.version;
  result.root_module = artifact.package.manifest.root_module;

  if (artifact.package_digest != sha256_prefixed(artifact.serialized_package)) {
    result.diagnostics.push_back(diagnostic(
        "FrozenImageVerifyError", "embedded package digest does not match"));
  }

  const pkg::PackageVerifyResult package_verify = pkg::verify_package_artifact(
      artifact.serialized_package, signing_key, path);
  result.package_valid = package_verify.ok;
  if (!package_verify.ok) {
    for (const pkg::PackageDiagnostic &entry : package_verify.diagnostics) {
      result.diagnostics.push_back(diagnostic(
          "FrozenPackageError", entry.error_name + ": " + entry.message));
    }
  }

  std::map<std::string, std::uint32_t> code_count_by_module;
  std::set<std::string> package_module_names;
  for (const pkg::PackageModuleBlob &blob : artifact.package.modules) {
    package_module_names.insert(blob.name);
    if (blob.digest != sha256_prefixed_bytes(blob.bytes)) {
      result.diagnostics.push_back(
          diagnostic("FrozenImageVerifyError",
                     "package module digest does not match", blob.name));
    }
    const bytecode::DecodeResult decoded =
        bytecode::deserialize_module(blob.bytes);
    if (!decoded.ok()) {
      result.diagnostics.push_back(diagnostic(
          "FrozenBytecodeError",
          bytecode::verify_errors_to_json(decoded.errors), blob.name));
      continue;
    }
    code_count_by_module[blob.name] =
        static_cast<std::uint32_t>(decoded.module.code_objects.size());
  }

  if (package_module_names.find(artifact.package.manifest.root_module) ==
      package_module_names.end()) {
    result.diagnostics.push_back(
        diagnostic("FrozenImageVerifyError",
                   "root module is missing from frozen image package",
                   artifact.package.manifest.root_module));
  }

  std::set<std::string> native_module_names;
  for (const FrozenImageNativeModule &module : artifact.native_modules) {
    if (!native_module_names.insert(module.module_name).second) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "duplicate native module in frozen image: " + module.module_name,
          module.module_name));
    }
    if (package_module_names.find(module.module_name) ==
        package_module_names.end()) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "native module has no matching package module", module.module_name));
    }
    if (module.format != "amber.native.v1") {
      result.diagnostics.push_back(
          diagnostic("FrozenImageVerifyError",
                     "unsupported native metadata format", module.module_name));
    }
    if (!module.requires_frozen_world) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "native module does not require frozen world", module.module_name));
    }
    if (module.metadata_digest != sha256_prefixed(module.metadata_json)) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError", "native metadata digest does not match",
          module.module_name));
    }
    if (module.metadata_json.empty() ||
        !metadata_declares_native_readiness(module.metadata_json)) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "native metadata does not declare amber.native.v1 readiness guards",
          module.module_name));
    }
    const auto code_count = code_count_by_module.find(module.module_name);
    if (code_count != code_count_by_module.end() &&
        module.code_object_count != code_count->second) {
      result.diagnostics.push_back(
          diagnostic("FrozenImageVerifyError",
                     "native code object count does not match bytecode module",
                     module.module_name));
    }
  }

  for (const std::string &module_name : package_module_names) {
    if (native_module_names.find(module_name) == native_module_names.end()) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "frozen image is missing native metadata for package module",
          module_name));
    }
  }

  std::set<std::string> image_extension_names;
  for (const pkg::PackageNativeExtensionMetadata &extension :
       artifact.native_extensions) {
    if (!image_extension_names.insert(extension.name).second) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "duplicate native extension in frozen image: " + extension.name,
          extension.name));
    }
    const pkg::PackageNativeExtensionMetadata *package_extension = nullptr;
    for (const pkg::PackageNativeExtensionMetadata &candidate :
         artifact.package.native_extensions) {
      if (candidate.name == extension.name) {
        package_extension = &candidate;
        break;
      }
    }
    if (package_extension == nullptr) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "native extension has no matching package metadata",
          extension.name));
      continue;
    }
    if (extension.amber_ext_abi_version != 1U ||
        extension.amber_ext_abi_version !=
            package_extension->amber_ext_abi_version) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "native extension ABI version does not match",
          extension.name));
    }
    if (extension.target_triple != package_extension->target_triple) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "native extension target triple does not match",
          extension.name));
    }
    if (extension.native_source_digest !=
        package_extension->native_source_digest) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "native extension source digest does not match", extension.name));
    }
    if (extension.exported_symbol_digest !=
        package_extension->exported_symbol_digest) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "native extension exported-symbol digest does not match",
          extension.name));
    }
    if (extension.types.size() != package_extension->types.size()) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "native extension ownership metadata does not match",
          extension.name));
    } else {
      for (std::size_t i = 0; i < extension.types.size(); ++i) {
        if (!same_native_type(extension.types[i],
                              package_extension->types[i])) {
          result.diagnostics.push_back(diagnostic(
              "FrozenImageVerifyError",
              "native extension ownership metadata does not match",
              extension.name));
          break;
        }
      }
    }
    if (extension.errors.size() != package_extension->errors.size()) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "native extension error metadata does not match", extension.name));
    } else {
      for (std::size_t i = 0; i < extension.errors.size(); ++i) {
        if (!same_native_error(extension.errors[i],
                               package_extension->errors[i])) {
          result.diagnostics.push_back(diagnostic(
              "FrozenImageVerifyError",
              "native extension error metadata does not match",
              extension.name));
          break;
        }
      }
    }
  }
  for (const pkg::PackageNativeExtensionMetadata &extension :
       artifact.package.native_extensions) {
    if (image_extension_names.find(extension.name) ==
        image_extension_names.end()) {
      result.diagnostics.push_back(diagnostic(
          "FrozenImageVerifyError",
          "frozen image is missing native extension metadata",
          extension.name));
    }
  }

  result.freeze_analysis_valid = result.diagnostics.empty();
  result.loadable = result.package_valid && result.freeze_analysis_valid;
  result.ok = result.loadable;
  return result;
}

std::string artifact_to_json(const FrozenImageArtifact &artifact) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.image.inspect.v1\",\n";
  out << "  \"format\": \"" << json_escape(artifact.format) << "\",\n";
  out << "  \"digest\": \"" << json_escape(artifact.image_digest) << "\",\n";
  out << "  \"package\": {\"name\":\""
      << json_escape(artifact.package.manifest.name) << "\",\"version\":\""
      << json_escape(artifact.package.manifest.version)
      << "\",\"root_module\":\""
      << json_escape(artifact.package.manifest.root_module) << "\"},\n";
  out << "  \"package_sha256\": \"" << json_escape(artifact.package_digest)
      << "\",\n";
  out << "  \"world_epoch\": " << artifact.world_epoch << ",\n";
  out << "  \"native_modules\": [";
  const std::vector<FrozenImageNativeModule> modules =
      sorted_native_modules(artifact.native_modules);
  for (std::size_t i = 0; i < modules.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    const FrozenImageNativeModule &module = modules[i];
    out << "\n    {\"name\":\"" << json_escape(module.module_name)
        << "\",\"path\":\"" << json_escape(module.module_path)
        << "\",\"format\":\"" << json_escape(module.format)
        << "\",\"requires_frozen_world\":"
        << (module.requires_frozen_world ? "true" : "false")
        << ",\"code_objects\":" << module.code_object_count << ",\"sha256\":\""
        << json_escape(module.metadata_digest) << "\"}";
  }
  out << "\n  ],\n";
  out << "  \"native_extensions\": [";
  const std::vector<pkg::PackageNativeExtensionMetadata> extensions =
      sorted_native_extensions(artifact.native_extensions);
  for (std::size_t i = 0; i < extensions.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    const pkg::PackageNativeExtensionMetadata &extension = extensions[i];
    out << "\n    {\"name\":\"" << json_escape(extension.name)
        << "\",\"amber_ext_abi_version\":"
        << extension.amber_ext_abi_version << ",\"target_triple\":\""
        << json_escape(extension.target_triple)
        << "\",\"native_source_sha256\":\""
        << json_escape(extension.native_source_digest)
        << "\",\"exported_symbol_sha256\":\""
        << json_escape(extension.exported_symbol_digest) << "\"}";
  }
  out << "\n  ],\n";
  out << "  \"analysis\": [";
  for (std::size_t i = 0; i < artifact.analysis.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    const FrozenImageAnalysisEntry &entry = artifact.analysis[i];
    out << "\n    {\"check\":\"" << json_escape(entry.check)
        << "\",\"status\":\"" << json_escape(entry.status) << "\",\"module\":\""
        << json_escape(entry.module_name) << "\",\"message\":\""
        << json_escape(entry.message) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

std::string verify_result_to_json(const FrozenImageVerifyResult &result) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": \"amber.image.verify.v1\",\n";
  out << "  \"status\": \"" << (result.ok ? "ok" : "error") << "\",\n";
  out << "  \"package\": \"" << json_escape(result.package_name) << "\",\n";
  out << "  \"version\": \"" << json_escape(result.version) << "\",\n";
  out << "  \"root_module\": \"" << json_escape(result.root_module) << "\",\n";
  out << "  \"digest\": \"" << json_escape(result.digest) << "\",\n";
  out << "  \"structurally_valid\": "
      << (result.structurally_valid ? "true" : "false") << ",\n";
  out << "  \"package_valid\": " << (result.package_valid ? "true" : "false")
      << ",\n";
  out << "  \"freeze_analysis_valid\": "
      << (result.freeze_analysis_valid ? "true" : "false") << ",\n";
  out << "  \"loadable\": " << (result.loadable ? "true" : "false") << ",\n";
  out << "  \"diagnostics\": [";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    const FrozenImageDiagnostic &entry = result.diagnostics[i];
    out << "\n    {\"error_name\":\"" << json_escape(entry.error_name)
        << "\",\"message\":\"" << json_escape(entry.message)
        << "\",\"module\":\"" << json_escape(entry.module_name) << "\"}";
  }
  out << "\n  ]\n";
  out << "}\n";
  return out.str();
}

std::string
diagnostics_to_json(const std::vector<FrozenImageDiagnostic> &diagnostics) {
  std::ostringstream out;
  out << "{\n  \"format\": \"amber.image.diagnostics.v1\",\n";
  out << "  \"errors\": [";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    if (i != 0U) {
      out << ",";
    }
    const FrozenImageDiagnostic &entry = diagnostics[i];
    out << "\n    {\"error_name\":\"" << json_escape(entry.error_name)
        << "\",\"message\":\"" << json_escape(entry.message)
        << "\",\"module\":\"" << json_escape(entry.module_name) << "\"}";
  }
  out << "\n  ]\n}\n";
  return out.str();
}

} // namespace amber::frozen
