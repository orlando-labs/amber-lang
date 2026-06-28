#include "runtime/io.h"
#include "runtime/stdlib_registry.h"

#include <memory>
#include <optional>
#include <string>

namespace amber::runtime {

namespace {

std::shared_ptr<RuntimePath> path_from_value(NativeStdlibCall &call,
                                             const Value &value) {
  if (value.is_string()) {
    const std::optional<std::string> text = call.text_of(value);
    if (!text.has_value()) {
      call.fault("VMError", "path string ref is invalid");
      return nullptr;
    }
    return std::make_shared<RuntimePath>(*text);
  }
  if (!value.is_io_value()) {
    call.fault("TypeError", "expected fs.Path or Str");
    return nullptr;
  }
  std::shared_ptr<RuntimePath> path =
      std::dynamic_pointer_cast<RuntimePath>(value.as_io_value());
  if (path == nullptr) {
    call.fault("TypeError", "expected fs.Path or Str");
  }
  return path;
}

SendStatus build_path_value(NativeStdlibCall &call, bool block_checked) {
  if (!call.require_arity(1) || !call.kw_args.empty() ||
      (!block_checked && !call.require_no_block())) {
    return SendStatus::Faulted;
  }
  const std::shared_ptr<RuntimePath> path = path_from_value(call, call.args[0]);
  if (path == nullptr) {
    return SendStatus::Faulted;
  }
  *call.out = Value::io_value(path);
  return SendStatus::Matched;
}

SendStatus fs_path_type_send(NativeStdlibCall &call) {
  if (call.selector != "new") {
    return SendStatus::NotHandled;
  }
  return build_path_value(call, false);
}

SendStatus fs_namespace_send(NativeStdlibCall &call) {
  if (call.selector != "Path" && call.selector != "File") {
    return SendStatus::NotHandled;
  }
  if (!call.require_no_block()) {
    return SendStatus::Faulted;
  }
  const RuntimeNativeTypeKind member_kind = call.selector == "Path"
                                                ? RuntimeNativeTypeKind::FsPath
                                                : RuntimeNativeTypeKind::FsFile;
  if (call.args.empty()) {
    if (!call.kw_args.empty()) {
      return SendStatus::Faulted;
    }
    *call.out = Value::native_type(member_kind);
    return SendStatus::Matched;
  }
  if (call.selector == "File") {
    return call.fault("TypeError", "fs.File is not directly callable");
  }
  return build_path_value(call, true);
}

RuntimeNativeModuleDescriptor fs_module_descriptor() {
  return {{{"fs", RuntimeNativeTypeKind::Fs},
           {"fs.Path", RuntimeNativeTypeKind::FsPath},
           {"fs.File", RuntimeNativeTypeKind::FsFile}},
          {{RuntimeNativeTypeKind::Fs, fs_namespace_send},
           {RuntimeNativeTypeKind::FsPath, fs_path_type_send}},
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
