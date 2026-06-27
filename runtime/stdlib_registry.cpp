#include "runtime/stdlib_registry.h"

namespace amber::runtime {

void RuntimeModuleRegistry::register_native_type_path(
    std::string path, RuntimeNativeTypeKind kind) {
  bindings_[std::move(path)] = RuntimeBindingRef::native_type_binding(kind);
}

void RuntimeModuleRegistry::register_native_function_path(
    std::string path, RuntimeNativeFunctionKind kind) {
  bindings_[std::move(path)] = RuntimeBindingRef::native_function_binding(kind);
}

void RuntimeModuleRegistry::register_task_module_path(std::string path) {
  bindings_[std::move(path)] = RuntimeBindingRef::task_module_binding();
}

void RuntimeModuleRegistry::register_flow_module_path(std::string path) {
  bindings_[std::move(path)] = RuntimeBindingRef::flow_module_binding();
}

std::optional<RuntimeBindingRef>
RuntimeModuleRegistry::binding_for_path(const std::string &path) const {
  const auto it = bindings_.find(path);
  if (it == bindings_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void RuntimeModuleRegistry::import_native_paths(
    const NativeRegistry &registry) {
  for (const auto &[path, kind] : registry.registered_paths()) {
    register_native_type_path(path, kind);
  }
}

void RuntimeTypeRegistry::register_native_type_call(RuntimeNativeTypeKind kind,
                                                    std::string selector) {
  RuntimeTypeCallDescriptor descriptor;
  descriptor.kind = kind;
  descriptor.selector = std::move(selector);
  native_calls_[kind] = std::move(descriptor);
}

std::optional<RuntimeTypeCallDescriptor>
RuntimeTypeRegistry::native_type_call(RuntimeNativeTypeKind kind) const {
  const auto it = native_calls_.find(kind);
  if (it == native_calls_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void RuntimeDispatchRegistry::register_native_handler(
    RuntimeNativeTypeKind kind, NativeStdlibHandler handler) {
  native_handlers_[kind] = handler;
}

std::optional<NativeStdlibHandler>
RuntimeDispatchRegistry::native_handler(RuntimeNativeTypeKind kind) const {
  const auto it = native_handlers_.find(kind);
  if (it == native_handlers_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void RuntimeDispatchRegistry::import_native_handlers(
    const NativeRegistry &registry) {
  for (const auto &[kind, handler] : registry.registered_handlers()) {
    register_native_handler(kind, handler);
  }
}

std::optional<std::uint16_t>
RuntimeErrorRegistry::error_id(const std::string &name) const {
  return runtime_error_id(name);
}

const char *RuntimeErrorRegistry::error_name(std::uint16_t error_id) const {
  return runtime_error_name(error_id);
}

bool RuntimeErrorRegistry::error_is_a(std::uint16_t error_id,
                                      std::uint16_t ancestor_error_id) const {
  return runtime_error_is_a(error_id, ancestor_error_id);
}

void NativeRegistry::register_handler(RuntimeNativeTypeKind kind,
                                      NativeStdlibHandler handler) {
  handlers_[kind] = handler;
}

void NativeRegistry::register_path(std::string path,
                                   RuntimeNativeTypeKind kind) {
  paths_.emplace(std::move(path), kind);
}

NativeStdlibHandler
NativeRegistry::handler_for(RuntimeNativeTypeKind kind) const {
  const auto it = handlers_.find(kind);
  return it == handlers_.end() ? nullptr : it->second;
}

std::optional<RuntimeNativeTypeKind>
NativeRegistry::kind_for_path(const std::string &path) const {
  const auto it = paths_.find(path);
  if (it == paths_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<std::pair<std::string, RuntimeNativeTypeKind>>
NativeRegistry::registered_paths() const {
  std::vector<std::pair<std::string, RuntimeNativeTypeKind>> paths;
  paths.reserve(paths_.size());
  for (const auto &[path, kind] : paths_) {
    paths.emplace_back(path, kind);
  }
  return paths;
}

std::vector<std::pair<RuntimeNativeTypeKind, NativeStdlibHandler>>
NativeRegistry::registered_handlers() const {
  std::vector<std::pair<RuntimeNativeTypeKind, NativeStdlibHandler>> handlers;
  handlers.reserve(handlers_.size());
  for (const auto &[kind, handler] : handlers_) {
    handlers.emplace_back(kind, handler);
  }
  return handlers;
}

void register_native_module_descriptor(
    NativeRegistry &registry, const RuntimeNativeModuleDescriptor &descriptor) {
  for (const RuntimeNativeModulePathDescriptor &path : descriptor.paths) {
    registry.register_path(path.path, path.kind);
  }
  for (const RuntimeNativeModuleHandlerDescriptor &handler :
       descriptor.handlers) {
    registry.register_handler(handler.kind, handler.handler);
  }
}

void register_runtime_module_descriptor(
    RuntimeModuleRegistry &modules,
    const RuntimeNativeModuleDescriptor &descriptor) {
  for (const RuntimeNativeModulePathDescriptor &path : descriptor.paths) {
    modules.register_native_type_path(path.path, path.kind);
  }
}

void register_runtime_dispatch_descriptor(
    RuntimeDispatchRegistry &dispatch,
    const RuntimeNativeModuleDescriptor &descriptor) {
  for (const RuntimeNativeModuleHandlerDescriptor &handler :
       descriptor.handlers) {
    dispatch.register_native_handler(handler.kind, handler.handler);
  }
}

void register_runtime_type_descriptor(
    RuntimeTypeRegistry &types,
    const RuntimeNativeModuleDescriptor &descriptor) {
  for (const RuntimeNativeModuleTypeCallDescriptor &type_call :
       descriptor.type_calls) {
    types.register_native_type_call(type_call.kind, type_call.selector);
  }
}

// The single list of builtin libraries. New libraries add one line here and
// ship as `runtime/stdlib_<name>.{cpp}` — no further edit to `vm.cpp`.
void register_builtin_stdlib(NativeRegistry &registry) {
  register_math(registry);
  register_json(registry);
  register_codecs(registry);
  register_digest(registry);
  register_secure_random(registry);
  register_argparser(registry);
  register_uuid(registry);
  register_time(registry);
  register_url(registry);
}

void register_builtin_runtime_modules(RuntimeModuleRegistry &modules,
                                      RuntimeDispatchRegistry &dispatch,
                                      RuntimeTypeRegistry &types) {
  register_io_runtime_module(modules, dispatch, types);
  register_fs_runtime_module(modules, dispatch, types);
  register_net_runtime_module(modules, dispatch, types);
  register_net_http_runtime_module(modules, dispatch, types);
  register_task_runtime_module(modules, dispatch, types);
  register_math_runtime_module(modules, dispatch, types);
  register_json_runtime_module(modules, dispatch, types);
  register_codecs_runtime_module(modules, dispatch, types);
  register_digest_runtime_module(modules, dispatch, types);
  register_secure_random_runtime_module(modules, dispatch, types);
  register_argparser_runtime_module(modules, dispatch, types);
  register_uuid_runtime_module(modules, dispatch, types);
  register_time_runtime_module(modules, dispatch, types);
  register_url_runtime_module(modules, dispatch, types);
}

void register_core_prelude_bindings(RuntimeModuleRegistry &registry) {
  registry.register_native_function_path("print",
                                         RuntimeNativeFunctionKind::Print);
  registry.register_native_function_path("p", RuntimeNativeFunctionKind::P);
  registry.register_native_function_path("pp", RuntimeNativeFunctionKind::Pp);
  registry.register_native_function_path("desc",
                                         RuntimeNativeFunctionKind::Desc);
  registry.register_native_function_path("Ok",
                                         RuntimeNativeFunctionKind::ResultOk);
  registry.register_native_function_path("Err",
                                         RuntimeNativeFunctionKind::ResultErr);
  registry.register_task_module_path("task");
  registry.register_flow_module_path("task.flow");
}

void register_legacy_native_type_paths(RuntimeModuleRegistry &registry) {
  registry.register_native_type_path("Kernel", RuntimeNativeTypeKind::Kernel);
  registry.register_native_type_path("Amber", RuntimeNativeTypeKind::Amber);
  registry.register_native_type_path("Str", RuntimeNativeTypeKind::Str);
  registry.register_native_type_path("Int", RuntimeNativeTypeKind::Int);
  registry.register_native_type_path("BigInt", RuntimeNativeTypeKind::BigInt);
  registry.register_native_type_path("Float", RuntimeNativeTypeKind::Float);
  registry.register_native_type_path("Bool", RuntimeNativeTypeKind::Bool);
  registry.register_native_type_path("Symbol", RuntimeNativeTypeKind::Symbol);
  registry.register_native_type_path("Array", RuntimeNativeTypeKind::Array);
  registry.register_native_type_path("Tuple", RuntimeNativeTypeKind::Tuple);
  registry.register_native_type_path("Set", RuntimeNativeTypeKind::Set);
  registry.register_native_type_path("Map", RuntimeNativeTypeKind::Map);
  registry.register_native_type_path("StrictMap",
                                     RuntimeNativeTypeKind::StrictMap);
  registry.register_native_type_path("StrictHashMap",
                                     RuntimeNativeTypeKind::StrictMap);
  registry.register_native_type_path("Range", RuntimeNativeTypeKind::Range);
  registry.register_native_type_path("Null", RuntimeNativeTypeKind::Null);
  registry.register_native_type_path("Object", RuntimeNativeTypeKind::Object);
}

void register_legacy_native_type_calls(RuntimeTypeRegistry &registry) {
  (void)registry;
}

} // namespace amber::runtime
