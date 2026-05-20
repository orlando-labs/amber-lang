#pragma once

#include "optimizer/native.h"
#include "runtime/vm.h"

namespace amber::runtime {

struct NativeExecutionOptions {
  bool allow_bytecode_fallback = false;
};

native::NativeModule
bind_native_module_to_world(const native::NativeModule &module,
                            const RuntimeWorldMirror &mirror);

ExecutionResult execute_native_code(RuntimeWorld &world,
                                    const native::NativeModule &module,
                                    std::uint32_t native_id,
                                    const std::vector<Value> &args = {},
                                    Value self = Value::null(),
                                    Value block = Value::null(),
                                    NativeExecutionOptions options = {});

} // namespace amber::runtime
