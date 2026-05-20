#include "runtime/native_bridge.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <utility>

namespace amber::runtime {

namespace {

const native::NativeCodeObject *
find_native_code(const native::NativeModule &module, std::uint32_t native_id) {
  for (const native::NativeCodeObject &code : module.code_objects) {
    if (code.native_id == native_id) {
      return &code;
    }
  }
  return nullptr;
}

ExecutionResult native_fault(std::string name, std::string message) {
  return {Value::null(), Fault{std::move(name), std::move(message), 0, 0}};
}

std::string first_native_diagnostic(
    const std::vector<native::NativeDiagnostic> &diagnostics) {
  if (diagnostics.empty()) {
    return "native module validation failed";
  }
  std::ostringstream out;
  out << diagnostics.front().code << ": " << diagnostics.front().message;
  return out.str();
}

void sort_assumptions(native::NativeCodeObject *code) {
  std::sort(code->world_epoch_assumptions.begin(),
            code->world_epoch_assumptions.end(),
            [](const native::NativeWorldAssumption &left,
               const native::NativeWorldAssumption &right) {
              if (left.kind != right.kind) {
                return left.kind < right.kind;
              }
              if (left.owner_index != right.owner_index) {
                return left.owner_index < right.owner_index;
              }
              if (left.world_epoch != right.world_epoch) {
                return left.world_epoch < right.world_epoch;
              }
              return left.method_version < right.method_version;
            });
}

void replace_world_assumptions(native::NativeCodeObject *code,
                               const RuntimeWorldMirror &mirror) {
  code->world_epoch_assumptions.clear();
  code->world_epoch_assumptions.push_back(
      {"world_epoch", 0, mirror.world_epoch, 0});
  for (const RuntimeOwnerMirror &owner : mirror.owners) {
    code->world_epoch_assumptions.push_back(
        {"owner_method_version", owner.index, 0, owner.method_version});
  }
  sort_assumptions(code);
}

std::optional<ExecutionResult>
check_native_assumptions(RuntimeWorld &world,
                         const native::NativeCodeObject &code,
                         const NativeExecutionOptions &options) {
  if (code.requires_frozen_world && !world.is_world_frozen()) {
    if (options.allow_bytecode_fallback) {
      return std::nullopt;
    }
    return native_fault("WorldFrozenError",
                        "native code requires a frozen runtime world");
  }

  bool saw_bound_world_epoch = false;
  for (const native::NativeWorldAssumption &assumption :
       code.world_epoch_assumptions) {
    if (assumption.kind == "world_epoch") {
      if (assumption.world_epoch == 0) {
        if (options.allow_bytecode_fallback) {
          return std::nullopt;
        }
        return native_fault("NativeAssumptionError",
                            "native code is not bound to a world epoch");
      }
      saw_bound_world_epoch = true;
      if (assumption.world_epoch != world.world_epoch()) {
        if (options.allow_bytecode_fallback) {
          return std::nullopt;
        }
        return native_fault("NativeInvalidationError",
                            "native world_epoch assumption is stale");
      }
    } else if (assumption.kind == "owner_method_version") {
      const std::optional<RuntimeOwnerMirror> owner =
          world.owner_mirror(assumption.owner_index);
      if (!owner.has_value()) {
        if (options.allow_bytecode_fallback) {
          return std::nullopt;
        }
        return native_fault("NativeAssumptionError",
                            "native owner assumption target is missing");
      }
      if (owner->method_version != assumption.method_version) {
        if (options.allow_bytecode_fallback) {
          return std::nullopt;
        }
        return native_fault("NativeInvalidationError",
                            "native method table assumption is stale");
      }
    }
  }

  if (code.requires_frozen_world && !saw_bound_world_epoch) {
    if (options.allow_bytecode_fallback) {
      return std::nullopt;
    }
    return native_fault("NativeAssumptionError",
                        "native code has no world_epoch assumption");
  }
  return {};
}

} // namespace

native::NativeModule
bind_native_module_to_world(const native::NativeModule &module,
                            const RuntimeWorldMirror &mirror) {
  native::NativeModule bound = module;
  for (native::NativeCodeObject &code : bound.code_objects) {
    replace_world_assumptions(&code, mirror);
  }
  return bound;
}

ExecutionResult
execute_native_code(RuntimeWorld &world, const native::NativeModule &module,
                    std::uint32_t native_id, const std::vector<Value> &args,
                    Value self, Value block, NativeExecutionOptions options) {
  const native::NativeValidationResult validation =
      native::validate_native_module(module);
  if (!validation.ok()) {
    return native_fault("NativeCodeError",
                        first_native_diagnostic(validation.diagnostics));
  }

  const native::NativeCodeObject *code = find_native_code(module, native_id);
  if (code == nullptr) {
    return native_fault("NativeCodeError", "native code id is unknown");
  }
  if (!code->bytecode_trampoline) {
    return native_fault("NativeCodeError",
                        "host machine-code entry is not available in this "
                        "reference runtime");
  }

  const std::optional<ExecutionResult> assumption_failure =
      check_native_assumptions(world, *code, options);
  if (assumption_failure.has_value()) {
    return *assumption_failure;
  }

  return world.execute(code->source_bc_code_id, args, std::move(self),
                       std::move(block));
}

} // namespace amber::runtime
