#include "runtime/stdlib_registry.h"

namespace amber::runtime {

void RuntimeModuleRegistry::register_native_type_path(
    std::string path, RuntimeNativeTypeKind kind) {
  bindings_[std::move(path)] = RuntimeBindingRef::native_type_binding(kind);
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

void RuntimeTypeRegistry::register_native_type_call(
    RuntimeNativeTypeKind kind, std::string selector) {
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

void register_legacy_native_type_calls(RuntimeTypeRegistry &registry) {
  registry.register_native_type_call(RuntimeNativeTypeKind::Bytes, "new");
  registry.register_native_type_call(RuntimeNativeTypeKind::ByteBuffer, "new");
  registry.register_native_type_call(RuntimeNativeTypeKind::IoPipe, "__call__");
  registry.register_native_type_call(RuntimeNativeTypeKind::FsPath, "new");
  registry.register_native_type_call(RuntimeNativeTypeKind::NetEndpoint, "new");
  registry.register_native_type_call(RuntimeNativeTypeKind::NetHttpClient,
                                     "new");
  registry.register_native_type_call(RuntimeNativeTypeKind::NetHttpRequest,
                                     "new");
  registry.register_native_type_call(RuntimeNativeTypeKind::NetHttpRequestBody,
                                     "new");
  registry.register_native_type_call(RuntimeNativeTypeKind::NetHttpHeaders,
                                     "new");
  registry.register_native_type_call(RuntimeNativeTypeKind::NetHttpServer,
                                     "new");
  registry.register_native_type_call(
      RuntimeNativeTypeKind::NetHttpServerResponse, "new");
  registry.register_native_type_call(
      RuntimeNativeTypeKind::NetHttpJsonGetJson, "__call__");
  registry.register_native_type_call(
      RuntimeNativeTypeKind::NetHttpJsonPostJson, "__call__");
  registry.register_native_type_call(RuntimeNativeTypeKind::NetHttpFormBody,
                                     "__call__");
  registry.register_native_type_call(RuntimeNativeTypeKind::ArgParser, "new");
}

} // namespace amber::runtime
