#pragma once

#include "profile/capabilities.h"

#include <cstdint>
#include <string>
#include <vector>

namespace amber::pkg {

struct PackageDiagnostic {
  std::string error_name;
  std::string message;
  std::string path;
};

struct PackageDependency {
  std::string name;
  std::string version;
  std::string source = "registry";
  std::string checksum;
};

struct PackageModule {
  std::string name;
  std::string path;
};

// A `[native.symbols]` entry: the logical name a `native def`/`native class`
// declares in source (`from "logical"`), mapped to the physical C symbol.
struct PackageNativeSymbol {
  std::string logical;
  std::string symbol;
};

// A `[[native.types]]` entry: the foreign-handle dispatch identity for a
// `native class` (the per-(package,type) tag) plus its ownership and reclaim.
struct PackageNativeType {
  std::string amber;
  std::string tag;
  std::string ownership;
  std::string destructor;
};

// A `[[native.errors]]` entry: a package-owned runtime error class registered
// into the active runtime error registry so typed rescue can match it.
struct PackageNativeError {
  std::string name;
  std::string parent;
  std::string default_message;
  std::string default_exit_code;
};

// A `[[native]]` extension unit: the build facts behind a package's native
// bindings (sources/flags/libs), the logical-name->symbol map, and the
// foreign-handle type table. The Amber surface declares which defs are native;
// this carries only how to build the native side (native-packages design §5).
struct PackageNativeExtension {
  std::string name;
  std::string language;
  std::vector<std::string> sources;
  std::vector<std::string> headers;
  std::vector<std::string> include_dirs;
  std::vector<std::string> defines;
  std::vector<std::string> cxxflags;
  std::vector<std::string> link_libraries;
  std::vector<PackageNativeSymbol> symbols;
  std::vector<PackageNativeType> types;
  std::vector<PackageNativeError> errors;
};

struct PackageManifest {
  std::string name;
  std::string version;
  std::string root_module;
  std::vector<PackageModule> modules;
  std::vector<PackageDependency> dependencies;
  std::vector<capability::CapabilityRequest> capabilities;
  std::vector<PackageNativeExtension> native_extensions;
};

struct PackageManifestResult {
  PackageManifest manifest;
  std::vector<PackageDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

struct PackageModuleBlob {
  std::string name;
  std::string path;
  std::string digest;
  std::vector<std::uint8_t> bytes;
};

struct PackageNativeBlob {
  std::string extension_name;
  std::string kind; // "source" or "header"
  std::string path;
  std::string digest;
  std::vector<std::uint8_t> bytes;
};

struct PackageNativeExtensionMetadata {
  std::string name;
  std::uint32_t amber_ext_abi_version = 1;
  std::string target_triple;
  std::string native_source_digest;
  std::string exported_symbol_digest;
  std::vector<PackageNativeType> types;
  std::vector<PackageNativeError> errors;
};

struct PackageSignature {
  std::string algorithm;
  std::string key_id;
  std::string digest;
};

struct PackageArtifact {
  PackageManifest manifest;
  std::string manifest_digest;
  std::string lockfile;
  std::string lock_digest;
  std::vector<PackageModuleBlob> modules;
  std::vector<PackageNativeBlob> native_blobs;
  std::vector<PackageNativeExtensionMetadata> native_extensions;
  PackageSignature signature;
};

struct PackageBuildOptions {
  std::string key_id;
  std::string signing_key;
  std::string target_triple;
  std::vector<PackageNativeBlob> native_blobs;
};

struct PackageBuildResult {
  bool ok = false;
  std::string serialized;
  PackageArtifact artifact;
  std::vector<PackageDiagnostic> diagnostics;
};

struct PackageParseResult {
  PackageArtifact artifact;
  std::vector<PackageDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

struct PackageVerifyResult {
  bool ok = false;
  bool structurally_valid = false;
  bool signature_present = false;
  bool signature_checked = false;
  bool signature_valid = false;
  std::string package_name;
  std::string version;
  std::string root_module;
  std::string digest;
  std::vector<PackageDiagnostic> diagnostics;
};

struct PackageRegistryResult {
  bool ok = false;
  std::string package_name;
  std::string version;
  std::string installed_path;
  std::vector<PackageDiagnostic> diagnostics;
};

PackageManifestResult parse_manifest_toml(const std::string &source,
                                          const std::string &path = {});

std::string manifest_to_json(const PackageManifest &manifest);
std::string render_lockfile(const PackageManifest &manifest);
std::vector<PackageNativeExtensionMetadata>
native_extension_metadata(const std::vector<PackageNativeExtension> &extensions,
                          const std::vector<PackageNativeBlob> &blobs,
                          const std::string &target_triple = {});

PackageBuildResult
build_package_artifact(const PackageManifest &manifest,
                       const std::vector<PackageModuleBlob> &modules,
                       const PackageBuildOptions &options = {});

PackageParseResult parse_package_artifact(const std::string &serialized,
                                          const std::string &path = {});
PackageVerifyResult verify_package_artifact(const std::string &serialized,
                                            const std::string &signing_key = {},
                                            const std::string &path = {});

PackageRegistryResult
install_package_artifact(const std::string &serialized,
                         const std::string &registry_root,
                         const std::string &signing_key = {});
PackageRegistryResult
publish_package_artifact(const std::string &serialized,
                         const std::string &registry_root,
                         const std::string &signing_key = {});

std::string artifact_to_json(const PackageArtifact &artifact);
std::string verify_result_to_json(const PackageVerifyResult &result);
std::string registry_result_to_json(const PackageRegistryResult &result);

} // namespace amber::pkg
