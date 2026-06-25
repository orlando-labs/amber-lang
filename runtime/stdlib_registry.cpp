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

void register_legacy_native_type_paths(RuntimeModuleRegistry &registry) {
  registry.register_native_type_path("Kernel", RuntimeNativeTypeKind::Kernel);
  registry.register_native_type_path("io", RuntimeNativeTypeKind::Io);
  registry.register_native_type_path("io.Buffer",
                                     RuntimeNativeTypeKind::TextBuffer);
  registry.register_native_type_path("io.Logger",
                                     RuntimeNativeTypeKind::Logger);
  registry.register_native_type_path("Bytes", RuntimeNativeTypeKind::Bytes);
  registry.register_native_type_path("io.ByteBuffer",
                                     RuntimeNativeTypeKind::ByteBuffer);
  registry.register_native_type_path("io.ByteSlice",
                                     RuntimeNativeTypeKind::ByteSlice);
  registry.register_native_type_path("io.Pipe", RuntimeNativeTypeKind::IoPipe);
  registry.register_native_type_path("fs", RuntimeNativeTypeKind::Fs);
  registry.register_native_type_path("fs.Path", RuntimeNativeTypeKind::FsPath);
  registry.register_native_type_path("fs.File", RuntimeNativeTypeKind::FsFile);
  registry.register_native_type_path("net", RuntimeNativeTypeKind::Net);
  registry.register_native_type_path("net.Endpoint",
                                     RuntimeNativeTypeKind::NetEndpoint);
  registry.register_native_type_path("net.tcp", RuntimeNativeTypeKind::NetTcp);
  registry.register_native_type_path("net.udp", RuntimeNativeTypeKind::NetUdp);
  registry.register_native_type_path("net.http",
                                     RuntimeNativeTypeKind::NetHttp);
  registry.register_native_type_path("net.http.Client",
                                     RuntimeNativeTypeKind::NetHttpClient);
  registry.register_native_type_path("net.http.Request",
                                     RuntimeNativeTypeKind::NetHttpRequest);
  registry.register_native_type_path(
      "net.http.RequestBody", RuntimeNativeTypeKind::NetHttpRequestBody);
  registry.register_native_type_path("net.http.Headers",
                                     RuntimeNativeTypeKind::NetHttpHeaders);
  registry.register_native_type_path("net.http.Server",
                                     RuntimeNativeTypeKind::NetHttpServer);
  registry.register_native_type_path(
      "net.http.ServerRequest", RuntimeNativeTypeKind::NetHttpServerRequest);
  registry.register_native_type_path(
      "net.http.ServerResponse", RuntimeNativeTypeKind::NetHttpServerResponse);
  registry.register_native_type_path("net.http.json",
                                     RuntimeNativeTypeKind::NetHttpJson);
  registry.register_native_type_path(
      "net.http.json.get_json", RuntimeNativeTypeKind::NetHttpJsonGetJson);
  registry.register_native_type_path(
      "net.http.json.post_json", RuntimeNativeTypeKind::NetHttpJsonPostJson);
  registry.register_native_type_path("net.http.form",
                                     RuntimeNativeTypeKind::NetHttpForm);
  registry.register_native_type_path("net.http.form.FormBody",
                                     RuntimeNativeTypeKind::NetHttpFormBody);
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
  registry.register_native_type_path("Flow", RuntimeNativeTypeKind::Flow);
  registry.register_native_type_path("task.flow.Flow",
                                     RuntimeNativeTypeKind::Flow);
  registry.register_native_type_path("Channel",
                                     RuntimeNativeTypeKind::Channel);
  registry.register_native_type_path("sync.Channel",
                                     RuntimeNativeTypeKind::Channel);
  registry.register_native_type_path("Mutex", RuntimeNativeTypeKind::Mutex);
  registry.register_native_type_path("sync.Mutex",
                                     RuntimeNativeTypeKind::Mutex);
  registry.register_native_type_path("Atomic", RuntimeNativeTypeKind::Atomic);
  registry.register_native_type_path("sync.Atomic",
                                     RuntimeNativeTypeKind::Atomic);
  registry.register_native_type_path("Barrier", RuntimeNativeTypeKind::Barrier);
  registry.register_native_type_path("sync.Barrier",
                                     RuntimeNativeTypeKind::Barrier);
  registry.register_native_type_path(
      "ThreadedCollection", RuntimeNativeTypeKind::ThreadedCollection);
  registry.register_native_type_path(
      "task.flow.ThreadedCollection",
      RuntimeNativeTypeKind::ThreadedCollection);
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
