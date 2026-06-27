#include "runtime/stdlib_registry.h"

namespace amber::runtime {

namespace {

RuntimeNativeModuleDescriptor task_module_descriptor() {
  return {{{"Flow", RuntimeNativeTypeKind::Flow},
           {"task.flow.Flow", RuntimeNativeTypeKind::Flow},
           {"Channel", RuntimeNativeTypeKind::Channel},
           {"sync.Channel", RuntimeNativeTypeKind::Channel},
           {"Mutex", RuntimeNativeTypeKind::Mutex},
           {"sync.Mutex", RuntimeNativeTypeKind::Mutex},
           {"Atomic", RuntimeNativeTypeKind::Atomic},
           {"sync.Atomic", RuntimeNativeTypeKind::Atomic},
           {"Barrier", RuntimeNativeTypeKind::Barrier},
           {"sync.Barrier", RuntimeNativeTypeKind::Barrier},
           {"ThreadedCollection", RuntimeNativeTypeKind::ThreadedCollection},
           {"task.flow.ThreadedCollection",
            RuntimeNativeTypeKind::ThreadedCollection}},
          {},
          {}};
}

} // namespace

void register_task_runtime_module(RuntimeModuleRegistry &modules,
                                  RuntimeDispatchRegistry &dispatch,
                                  RuntimeTypeRegistry &types) {
  const RuntimeNativeModuleDescriptor descriptor = task_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
}

} // namespace amber::runtime
