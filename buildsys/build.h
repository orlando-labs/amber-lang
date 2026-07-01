#pragma once

#include "package/package.h"

#include <cstdint>
#include <string>
#include <vector>

namespace amber::build {

struct BuildDiagnostic {
  std::string error_name;
  std::string message;
  std::string path;
};

struct BuildModule {
  std::string name;
  std::string path;
  bool stdlib = false;
  std::string bootstrap_layer;
};

struct BuildProfileSet {
  std::vector<std::string> required_features;
  std::vector<std::string> optional_features;
  std::vector<std::string> forbidden_features;
  // amber.numeric-profile.v1 manifest mirror (profiles.numeric). Empty means
  // unspecified: source preambles (or the Int64/checked defaults) apply.
  std::string numeric_int;
  std::string numeric_overflow;
};

struct BuildManifest {
  std::string schema = "amber.build.v1";
  std::string name;
  std::string root_module;
  BuildProfileSet profiles;
  std::vector<BuildModule> stdlib_modules;
  std::vector<BuildModule> modules;
  // Native extension units, lowered from the package manifest's [[native]]
  // sections. Reuses the package shape so there is one schema for the build to
  // compile/link against (native-packages design §5).
  std::vector<amber::pkg::PackageNativeExtension> native_extensions;
};

struct BuildManifestResult {
  BuildManifest manifest;
  std::vector<BuildDiagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

struct BuildArtifactRecord {
  std::string name;
  std::string path;
  std::string output_path;
  std::string cache_path;
  std::string cache_key;
  std::string source_hash;
  std::string artifact_hash;
  std::string abi_hash;
  std::string native_output_path;
  std::string native_hash;
  std::string native_backend;
  std::string native_fallback_reason;
  bool stdlib = false;
  bool cached = false;
  bool native_eligible = false;
  std::string bootstrap_layer;
  std::uint64_t byte_size = 0;
  std::uint64_t native_byte_size = 0;
};

struct BuildSummary {
  bool ok = false;
  std::string name;
  std::string root_module;
  std::string target;
  std::string out_dir;
  std::string cache_dir;
  std::string native_output_path;
  std::string native_backend;
  std::string native_hash;
  std::string native_launcher_source;
  std::string native_cxx;
  bool native_bytecode_trampoline = false;
  std::uint64_t native_graph_module_count = 0;
  std::uint64_t native_graph_code_count = 0;
  std::uint64_t native_graph_native_code_count = 0;
  std::uint64_t native_graph_vm_fallback_code_count = 0;
  std::vector<amber::pkg::PackageNativeExtensionMetadata> native_extensions;
  BuildProfileSet profiles;
  std::vector<BuildArtifactRecord> artifacts;
  std::vector<BuildDiagnostic> diagnostics;
};

BuildManifestResult parse_build_manifest_json(const std::string &source,
                                              const std::string &path = {});

BuildProfileSet normalize_profiles(BuildProfileSet profiles);
std::uint32_t profile_flags_for(const BuildProfileSet &profiles);
bool runtime_supports_feature(const std::string &feature);

std::string manifest_to_json(const BuildManifest &manifest);
std::string summary_to_json(const BuildSummary &summary);
std::string
diagnostics_to_string(const std::vector<BuildDiagnostic> &diagnostics);

} // namespace amber::build
