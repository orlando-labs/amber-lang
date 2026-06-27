#include "runtime/stdlib_registry.h"

namespace amber::runtime {

namespace {

RuntimeNativeModuleDescriptor net_module_descriptor() {
  return {{{"net", RuntimeNativeTypeKind::Net},
           {"net.Endpoint", RuntimeNativeTypeKind::NetEndpoint},
           {"net.tcp", RuntimeNativeTypeKind::NetTcp},
           {"net.udp", RuntimeNativeTypeKind::NetUdp}},
          {},
          {{RuntimeNativeTypeKind::NetEndpoint, "new"}}};
}

} // namespace

void register_net_runtime_module(RuntimeModuleRegistry &modules,
                                 RuntimeDispatchRegistry &dispatch,
                                 RuntimeTypeRegistry &types) {
  const RuntimeNativeModuleDescriptor descriptor = net_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
}

} // namespace amber::runtime
