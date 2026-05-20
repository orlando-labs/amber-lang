#include "runtime/frozen_image.h"

#include <utility>

namespace amber::runtime {

namespace {

frozen::FrozenImageDiagnostic diagnostic(std::string error_name,
                                         std::string message,
                                         std::string module_name = {}) {
  frozen::FrozenImageDiagnostic out;
  out.error_name = std::move(error_name);
  out.message = std::move(message);
  out.module_name = std::move(module_name);
  return out;
}

} // namespace

RuntimeFrozenImageLoadResult
load_frozen_image(const frozen::FrozenImageArtifact &artifact) {
  RuntimeFrozenImageLoadResult result;

  const pkg::PackageVerifyResult package_verify =
      pkg::verify_package_artifact(artifact.serialized_package);
  if (!package_verify.ok) {
    for (const pkg::PackageDiagnostic &entry : package_verify.diagnostics) {
      result.diagnostics.push_back(diagnostic(
          "FrozenPackageError", entry.error_name + ": " + entry.message));
    }
    return result;
  }

  result.world = std::make_unique<RuntimeWorld>(artifact.package);
  const ExecutionResult frozen = result.world->freeze_world();
  if (!frozen.ok()) {
    result.diagnostics.push_back(
        diagnostic(frozen.fault->error_name, frozen.fault->message,
                   artifact.package.manifest.root_module));
    return result;
  }

  const RuntimeWorldMirror mirror = result.world->world_mirror();
  for (const frozen::FrozenImageNativeModule &module :
       artifact.native_modules) {
    if (module.native_module.code_objects.empty()) {
      continue;
    }
    result.bound_native_modules.push_back(
        bind_native_module_to_world(module.native_module, mirror));
  }

  if (!result.world->is_world_frozen()) {
    result.diagnostics.push_back(
        diagnostic("WorldFrozenError",
                   "frozen image loader did not install freeze barrier",
                   artifact.package.manifest.root_module));
    return result;
  }
  result.ok = true;
  return result;
}

RuntimeFrozenImageLoadResult
load_frozen_image(const std::string &serialized_image) {
  RuntimeFrozenImageLoadResult result;
  const frozen::FrozenImageVerifyResult verified =
      frozen::verify_frozen_image_artifact(serialized_image);
  if (!verified.ok) {
    result.diagnostics = verified.diagnostics;
    return result;
  }
  const frozen::FrozenImageParseResult parsed =
      frozen::parse_frozen_image_artifact(serialized_image);
  if (!parsed.ok()) {
    result.diagnostics = parsed.diagnostics;
    return result;
  }
  return load_frozen_image(parsed.artifact);
}

} // namespace amber::runtime
