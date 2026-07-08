#include "runtime/stdlib_registry.h"

namespace amber::runtime {

namespace {

std::string registry_string_or_empty(const bytecode::BcModule &module,
                                     std::uint32_t id) {
  return id < module.strings.size() ? module.strings[id] : std::string();
}

bool parse_native_binding_code_id(const std::string &key,
                                  const std::string &prefix,
                                  std::uint32_t *out) {
  std::uint32_t code_id = 0;
  bool parsed = key.size() > prefix.size();
  for (std::size_t i = prefix.size(); i < key.size(); ++i) {
    const char digit = key[i];
    if (digit < '0' || digit > '9') {
      parsed = false;
      break;
    }
    code_id = code_id * 10U + static_cast<std::uint32_t>(digit - '0');
  }
  if (!parsed || code_id == 0U) {
    return false;
  }
  *out = code_id;
  return true;
}

} // namespace

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

void RuntimeTypeRegistry::register_native_package_type(
    NativeTypeDescriptor descriptor) {
  native_package_tags_.register_type(std::move(descriptor));
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

void RuntimeDispatchRegistry::register_io_value_handler(
    std::string type_name, RuntimeNativeTypeKind kind,
    NativeStdlibHandler handler) {
  RuntimeIoValueHandlerDescriptor descriptor;
  descriptor.type_name = type_name;
  descriptor.kind = kind;
  descriptor.handler = handler;
  io_value_handlers_[std::move(type_name)] = std::move(descriptor);
}

std::optional<RuntimeIoValueHandlerDescriptor>
RuntimeDispatchRegistry::io_value_handler(const std::string &type_name) const {
  const auto it = io_value_handlers_.find(type_name);
  if (it == io_value_handlers_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void RuntimeDispatchRegistry::register_native_package_thunk(
    std::string logical, void *fn) {
  native_package_thunks_[std::move(logical)] = fn;
}

void *RuntimeDispatchRegistry::native_package_thunk(
    const std::string &logical) const {
  const auto it = native_package_thunks_.find(logical);
  return it == native_package_thunks_.end() ? nullptr : it->second;
}

void RuntimeDispatchRegistry::register_native_package_code_binding(
    std::uint32_t code_id, bool method, std::string logical) {
  native_package_code_bindings_[code_id] = {code_id, method,
                                            std::move(logical)};
}

const RuntimeNativePackageCodeBindingDescriptor *
RuntimeDispatchRegistry::native_package_code_binding(
    std::uint32_t code_id) const {
  const auto it = native_package_code_bindings_.find(code_id);
  return it == native_package_code_bindings_.end() ? nullptr : &it->second;
}

void RuntimeDispatchRegistry::register_native_package_method_binding(
    std::string tag, std::string selector, std::string logical) {
  std::string key = std::move(tag);
  key.push_back('\t');
  key += selector;
  native_package_method_bindings_[std::move(key)] = std::move(logical);
}

const std::string *RuntimeDispatchRegistry::native_package_method_binding(
    const std::string &tag, const std::string &selector) const {
  const auto it = native_package_method_bindings_.find(tag + "\t" + selector);
  return it == native_package_method_bindings_.end() ? nullptr : &it->second;
}

void RuntimeDispatchRegistry::import_native_package_bindings(
    const bytecode::BcModule &module) {
  const RuntimeNativePackageDescriptor descriptor =
      runtime_native_package_descriptor_from_module(module);
  for (const RuntimeNativePackageCodeBindingDescriptor &binding :
       descriptor.code_bindings) {
    register_native_package_code_binding(binding.code_id, binding.method,
                                         binding.logical);
  }
  for (const RuntimeNativePackageMethodBindingDescriptor &binding :
       descriptor.method_bindings) {
    register_native_package_method_binding(binding.tag, binding.selector,
                                           binding.logical);
  }
}

RuntimeNativePackageDescriptor
runtime_native_package_descriptor_from_module(const bytecode::BcModule &module) {
  static const std::string kBindPrefix = "amber.native.bind:";
  static const std::string kMethodPrefix = "amber.native.method:";
  RuntimeNativePackageDescriptor descriptor;
  for (const bytecode::AttrEntry &attr : module.attrs) {
    const std::string key = registry_string_or_empty(module, attr.key_str_id);
    if (key.compare(0, kBindPrefix.size(), kBindPrefix) == 0) {
      std::uint32_t code_id = 0;
      if (!parse_native_binding_code_id(key, kBindPrefix, &code_id)) {
        continue;
      }
      const std::string value =
          registry_string_or_empty(module, attr.value_str_id);
      if (value.size() < 2U || value[1] != ':') {
        continue;
      }
      descriptor.code_bindings.push_back(
          {code_id, value[0] == 'M', value.substr(2)});
      continue;
    }
    if (key.compare(0, kMethodPrefix.size(), kMethodPrefix) == 0) {
      const std::string logical =
          registry_string_or_empty(module, attr.value_str_id);
      const std::string method_key = key.substr(kMethodPrefix.size());
      const std::size_t separator = method_key.find('\t');
      if (separator == std::string::npos) {
        continue;
      }
      descriptor.method_bindings.push_back(
          {method_key.substr(0, separator),
           method_key.substr(separator + 1U), logical});
    }
  }
  return descriptor;
}

void RuntimeDispatchRegistry::import_native_handlers(
    const NativeRegistry &registry) {
  for (const auto &[kind, handler] : registry.registered_handlers()) {
    register_native_handler(kind, handler);
  }
}

std::optional<std::uint16_t>
RuntimeErrorRegistry::register_error(std::string name, std::string parent,
                                     std::string default_message,
                                     std::int64_t default_exit_code,
                                     std::uint32_t field_mask) {
  if (name.empty()) {
    return std::nullopt;
  }
  const auto existing = error_ids_.find(name);
  if (existing != error_ids_.end()) {
    const ErrorRecord &record = errors_[existing->second];
    if (record.parent != parent ||
        record.default_message != default_message ||
        record.default_exit_code != default_exit_code ||
        record.field_mask != field_mask) {
      return std::nullopt;
    }
    return existing->second;
  }
  return append_error({std::move(name), std::move(parent),
                       std::move(default_message), default_exit_code,
                       field_mask},
                      true);
}

RuntimeErrorRegistry::RuntimeErrorRegistry(Seed seed) {
  if (seed == Seed::Empty) {
    return;
  }
#define AMBER_RUNTIME_ERROR(name, parent, default_message, default_exit_code,  \
                            field_mask)                                        \
  append_error({name, parent, default_message, default_exit_code, field_mask},  \
               error_ids_.find(name) == error_ids_.end());
#include "spec/registries/runtime_errors.def"
#undef AMBER_RUNTIME_ERROR
}

std::uint16_t RuntimeErrorRegistry::append_error(ErrorRecord record,
                                                 bool update_name_index) {
  const std::uint16_t id = static_cast<std::uint16_t>(errors_.size());
  if (update_name_index) {
    error_ids_.emplace(record.name, id);
  }
  errors_.push_back(std::move(record));
  return id;
}

std::optional<std::uint16_t>
RuntimeErrorRegistry::error_id(const std::string &name) const {
  const auto it = error_ids_.find(name);
  if (it == error_ids_.end()) {
    return std::nullopt;
  }
  return it->second;
}

const char *RuntimeErrorRegistry::error_name(std::uint16_t error_id) const {
  if (error_id >= errors_.size()) {
    return "Error";
  }
  return errors_[error_id].name.c_str();
}

bool RuntimeErrorRegistry::error_is_a(std::uint16_t error_id,
                                      std::uint16_t ancestor_error_id) const {
  for (std::uint16_t depth = 0; error_id < errors_.size() &&
                                depth < errors_.size(); ++depth) {
    if (error_id == ancestor_error_id) {
      return true;
    }
    const std::string &parent = errors_[error_id].parent;
    if (parent.empty()) {
      return false;
    }
    const std::optional<std::uint16_t> parent_id = this->error_id(parent);
    if (!parent_id.has_value()) {
      return false;
    }
    error_id = *parent_id;
  }
  return false;
}

std::uint32_t
RuntimeErrorRegistry::error_effective_field_mask(std::uint16_t error_id) const {
  std::uint32_t mask = 0;
  for (std::uint16_t depth = 0; error_id < errors_.size() &&
                                depth < errors_.size(); ++depth) {
    const ErrorRecord &record = errors_[error_id];
    mask |= record.field_mask;
    if (record.parent.empty()) {
      break;
    }
    const std::optional<std::uint16_t> parent_id =
        this->error_id(record.parent);
    if (!parent_id.has_value()) {
      break;
    }
    error_id = *parent_id;
  }
  return mask;
}

std::optional<std::int64_t>
RuntimeErrorRegistry::error_default_exit_code(std::uint16_t error_id) const {
  for (std::uint16_t depth = 0; error_id < errors_.size() &&
                                depth < errors_.size(); ++depth) {
    const ErrorRecord &record = errors_[error_id];
    if (record.default_exit_code >= 0) {
      return record.default_exit_code;
    }
    if (record.parent.empty()) {
      break;
    }
    const std::optional<std::uint16_t> parent_id =
        this->error_id(record.parent);
    if (!parent_id.has_value()) {
      break;
    }
    error_id = *parent_id;
  }
  return std::nullopt;
}

const char *
RuntimeErrorRegistry::error_default_message(std::uint16_t error_id) const {
  for (std::uint16_t depth = 0; error_id < errors_.size() &&
                                depth < errors_.size(); ++depth) {
    const ErrorRecord &record = errors_[error_id];
    if (!record.default_message.empty()) {
      return record.default_message.c_str();
    }
    if (record.parent.empty()) {
      break;
    }
    const std::optional<std::uint16_t> parent_id =
        this->error_id(record.parent);
    if (!parent_id.has_value()) {
      break;
    }
    error_id = *parent_id;
  }
  return "";
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
  for (const RuntimeNativeModuleIoHandlerDescriptor &handler :
       descriptor.io_handlers) {
    dispatch.register_io_value_handler(handler.type_name, handler.kind,
                                       handler.handler);
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

void register_runtime_error_descriptor(
    RuntimeErrorRegistry &errors,
    const RuntimeNativeModuleDescriptor &descriptor) {
  for (const RuntimeNativeModuleErrorDescriptor &error : descriptor.errors) {
    (void)errors.register_error(error.name, error.parent,
                                error.default_message,
                                error.default_exit_code, error.field_mask);
  }
}

void register_runtime_native_package_descriptor(
    RuntimeDispatchRegistry &dispatch, RuntimeTypeRegistry &types,
    RuntimeErrorRegistry &errors,
    const RuntimeNativePackageDescriptor &descriptor) {
  for (const RuntimeNativePackageThunkDescriptor &thunk : descriptor.thunks) {
    dispatch.register_native_package_thunk(thunk.logical, thunk.fn);
  }
  for (const RuntimeNativePackageCodeBindingDescriptor &binding :
       descriptor.code_bindings) {
    dispatch.register_native_package_code_binding(binding.code_id,
                                                  binding.method,
                                                  binding.logical);
  }
  for (const RuntimeNativePackageMethodBindingDescriptor &binding :
       descriptor.method_bindings) {
    dispatch.register_native_package_method_binding(binding.tag,
                                                    binding.selector,
                                                    binding.logical);
  }
  for (NativeTypeDescriptor type : descriptor.types) {
    types.register_native_package_type(std::move(type));
  }
  for (const RuntimeNativePackageErrorDescriptor &error : descriptor.errors) {
    (void)errors.register_error(error.name,
                                error.parent.empty() ? "NativeError"
                                                     : error.parent,
                                error.default_message,
                                error.default_exit_code, error.field_mask);
  }
}

// The single list of builtin libraries. New libraries add one line here and
// ship as `runtime/stdlib_<name>.{cpp}` — no further edit to `vm.cpp`.
void register_builtin_stdlib(NativeRegistry &registry) {
  register_math(registry);
  register_json(registry);
  register_codecs(registry);
  register_digest(registry);
  register_benchmark(registry);
  register_secure_random(registry);
  register_argparser(registry);
  register_uuid(registry);
  register_time(registry);
  register_url(registry);
  register_yaml(registry);
}

void register_builtin_runtime_modules(RuntimeModuleRegistry &modules,
                                      RuntimeDispatchRegistry &dispatch,
                                      RuntimeTypeRegistry &types,
                                      RuntimeErrorRegistry *errors) {
  register_io_runtime_module(modules, dispatch, types);
  register_fs_runtime_module(modules, dispatch, types);
  register_net_runtime_module(modules, dispatch, types, errors);
  register_net_http_runtime_module(modules, dispatch, types, errors);
  register_task_runtime_module(modules, dispatch, types, errors);
  register_math_runtime_module(modules, dispatch, types);
  register_json_runtime_module(modules, dispatch, types, errors);
  register_codecs_runtime_module(modules, dispatch, types, errors);
  register_digest_runtime_module(modules, dispatch, types);
  register_benchmark_runtime_module(modules, dispatch, types, errors);
  register_secure_random_runtime_module(modules, dispatch, types, errors);
  register_argparser_runtime_module(modules, dispatch, types, errors);
  register_uuid_runtime_module(modules, dispatch, types, errors);
  register_time_runtime_module(modules, dispatch, types, errors);
  register_url_runtime_module(modules, dispatch, types, errors);
  register_yaml_runtime_module(modules, dispatch, types, errors);
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
  // Macro `Ast` builder module (macro.v1). `Ast.node(...)` constructs Ast
  // values; quote lowering targets it.
  registry.register_native_type_path("Ast", RuntimeNativeTypeKind::Ast);
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

} // namespace amber::runtime
