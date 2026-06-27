#include "runtime/stdlib_registry.h"

namespace amber::runtime {

namespace {

RuntimeNativeModuleDescriptor fs_module_descriptor() {
  return {{{"fs", RuntimeNativeTypeKind::Fs},
           {"fs.Path", RuntimeNativeTypeKind::FsPath},
           {"fs.File", RuntimeNativeTypeKind::FsFile}},
          {},
          {{RuntimeNativeTypeKind::FsPath, "new"}}};
}

} // namespace

void register_fs_runtime_module(RuntimeModuleRegistry &modules,
                                RuntimeDispatchRegistry &dispatch,
                                RuntimeTypeRegistry &types) {
  const RuntimeNativeModuleDescriptor descriptor = fs_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
}

} // namespace amber::runtime
