#include "runtime/stdlib_registry.h"

namespace amber::runtime {

namespace {

RuntimeNativeModuleDescriptor io_module_descriptor() {
  return {{{"io", RuntimeNativeTypeKind::Io},
           {"io.Buffer", RuntimeNativeTypeKind::TextBuffer},
           {"io.Logger", RuntimeNativeTypeKind::Logger},
           {"Bytes", RuntimeNativeTypeKind::Bytes},
           {"io.ByteBuffer", RuntimeNativeTypeKind::ByteBuffer},
           {"io.ByteSlice", RuntimeNativeTypeKind::ByteSlice},
           {"io.Pipe", RuntimeNativeTypeKind::IoPipe}},
          {},
          {{RuntimeNativeTypeKind::Bytes, "new"},
           {RuntimeNativeTypeKind::ByteBuffer, "new"},
           {RuntimeNativeTypeKind::IoPipe, "__call__"}}};
}

} // namespace

void register_io_runtime_module(RuntimeModuleRegistry &modules,
                                RuntimeDispatchRegistry &dispatch,
                                RuntimeTypeRegistry &types) {
  const RuntimeNativeModuleDescriptor descriptor = io_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
}

} // namespace amber::runtime
