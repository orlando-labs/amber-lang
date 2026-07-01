#pragma once

#include "optimizer/native.h"
#include "package/package.h"

#include <cstdint>
#include <string>
#include <vector>

namespace amber::frozen {

struct FrozenImageDiagnostic {
  std::string error_name;
  std::string message;
  std::string module_name;
};

struct FrozenImageAnalysisEntry {
  std::string check;
  std::string status;
  std::string module_name;
  std::string message;
};

struct FrozenImageBuildOptions {
  std::uint64_t world_epoch = 1;
};

struct FrozenImageNativeModule {
  std::string module_name;
  std::string module_path;
  std::string format = "amber.native.v1";
  bool requires_frozen_world = true;
  std::uint32_t code_object_count = 0;
  std::string metadata_digest;
  std::string metadata_json;
  native::NativeModule native_module;
};

struct FrozenImageArtifact {
  std::string format = "amber.image.v1";
  std::string image_digest;
  std::string package_digest;
  std::string serialized_package;
  pkg::PackageArtifact package;
  std::uint64_t world_epoch = 1;
  std::vector<FrozenImageNativeModule> native_modules;
  std::vector<pkg::PackageNativeExtensionMetadata> native_extensions;
  std::vector<FrozenImageAnalysisEntry> analysis;
};

struct FrozenImageBuildResult {
  bool ok = false;
  std::string serialized;
  FrozenImageArtifact artifact;
  std::vector<FrozenImageDiagnostic> diagnostics;
};

struct FrozenImageParseResult {
  FrozenImageArtifact artifact;
  std::vector<FrozenImageDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

struct FrozenImageVerifyResult {
  bool ok = false;
  bool structurally_valid = false;
  bool package_valid = false;
  bool freeze_analysis_valid = false;
  bool loadable = false;
  std::string package_name;
  std::string version;
  std::string root_module;
  std::string digest;
  std::vector<FrozenImageDiagnostic> diagnostics;
};

FrozenImageBuildResult build_frozen_image_artifact(
    const pkg::PackageArtifact &package_artifact,
    const std::string &serialized_package,
    const std::vector<native::NativeModule> &native_modules,
    const FrozenImageBuildOptions &options = {});

std::string
serialize_frozen_image_artifact(const FrozenImageArtifact &artifact);

FrozenImageParseResult
parse_frozen_image_artifact(const std::string &serialized,
                            const std::string &path = {});

FrozenImageVerifyResult
verify_frozen_image_artifact(const std::string &serialized,
                             const std::string &signing_key = {},
                             const std::string &path = {});

std::string artifact_to_json(const FrozenImageArtifact &artifact);
std::string verify_result_to_json(const FrozenImageVerifyResult &result);
std::string
diagnostics_to_json(const std::vector<FrozenImageDiagnostic> &diagnostics);

} // namespace amber::frozen
