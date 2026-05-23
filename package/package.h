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

struct PackageManifest {
  std::string name;
  std::string version;
  std::string root_module;
  std::vector<PackageModule> modules;
  std::vector<PackageDependency> dependencies;
  std::vector<capability::CapabilityRequest> capabilities;
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
  PackageSignature signature;
};

struct PackageBuildOptions {
  std::string key_id;
  std::string signing_key;
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
