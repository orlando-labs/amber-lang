#pragma once

#include "frozen/image.h"
#include "runtime/native_bridge.h"

#include <memory>
#include <string>
#include <vector>

namespace amber::runtime {

struct RuntimeFrozenImageLoadResult {
  bool ok = false;
  std::unique_ptr<RuntimeWorld> world;
  std::vector<native::NativeModule> bound_native_modules;
  std::vector<frozen::FrozenImageDiagnostic> diagnostics;
};

RuntimeFrozenImageLoadResult
load_frozen_image(const frozen::FrozenImageArtifact &artifact);

RuntimeFrozenImageLoadResult
load_frozen_image(const std::string &serialized_image);

} // namespace amber::runtime
