#include "runtime/io.h"
#include "runtime/stdlib_registry.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace amber::runtime {

namespace {

bool set_fault_from_io_status(NativeStdlibCall &call,
                              const RuntimeIoStatus &result) {
  if (result.ok || result.would_block) {
    return true;
  }
  call.fault(result.error_name.empty() ? "IOError" : result.error_name,
             result.message.empty() ? "IO operation failed" : result.message);
  return false;
}

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

std::optional<std::string> bytes_from_value(NativeStdlibCall &call,
                                            const Value &value) {
  if (value.is_string()) {
    const std::optional<std::string> text = call.text_of(value);
    if (!text.has_value()) {
      call.fault("VMError", "string ref is invalid");
    }
    return text;
  }
  if (!value.is_io_value()) {
    call.fault("TypeError", "expected Bytes, ByteSlice, ByteBuffer, or Str");
    return std::nullopt;
  }
  const std::shared_ptr<RuntimeIoValue> io_value = value.as_io_value();
  if (const auto bytes = std::dynamic_pointer_cast<RuntimeBytes>(io_value)) {
    return bytes->string();
  }
  if (const auto slice =
          std::dynamic_pointer_cast<RuntimeByteSlice>(io_value)) {
    return slice->bytes()->string();
  }
  if (const auto buffer =
          std::dynamic_pointer_cast<RuntimeByteBuffer>(io_value)) {
    const RuntimeIoStatus access = buffer->access_status();
    if (!set_fault_from_io_status(call, access)) {
      return std::nullopt;
    }
    return buffer->bytes();
  }
  call.fault("TypeError", "expected Bytes, ByteSlice, ByteBuffer, or Str");
  return std::nullopt;
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

SendStatus fs_metadata_send(NativeStdlibCall &call) {
  if (call.selector != "exists?" && call.selector != "file?" &&
      call.selector != "dir?" && call.selector != "metadata") {
    return SendStatus::NotHandled;
  }
  if (!call.require_no_block() || !call.require_arity(1)) {
    return SendStatus::Faulted;
  }
  const std::shared_ptr<RuntimePath> path = path_from_value(call, call.args[0]);
  if (path == nullptr) {
    return SendStatus::Faulted;
  }
  if (!call.kw_args.empty()) {
    return call.fault("TypeError", call.selector + " does not accept keywords");
  }
  if (call.selector == "metadata") {
    return call.fs_metadata(path->string(), call.out) ? SendStatus::Matched
                                                      : SendStatus::Faulted;
  }
  bool value = false;
  const bool ok =
      call.selector == "exists?" ? call.fs_exists(path->string(), &value)
      : call.selector == "file?" ? call.fs_file(path->string(), &value)
                                 : call.fs_dir(path->string(), &value);
  if (!ok) {
    return SendStatus::Faulted;
  }
  *call.out = Value::boolean(value);
  return SendStatus::Matched;
}

SendStatus fs_read_send(NativeStdlibCall &call) {
  if (call.selector != "read_bytes" && call.selector != "read_text") {
    return SendStatus::NotHandled;
  }
  if (!call.require_no_block() || !call.require_arity(1)) {
    return SendStatus::Faulted;
  }
  const std::shared_ptr<RuntimePath> path = path_from_value(call, call.args[0]);
  if (path == nullptr) {
    return SendStatus::Faulted;
  }
  if (!call.reject_unknown_keywords({"limit", "encoding"})) {
    return SendStatus::Faulted;
  }
  std::optional<std::size_t> limit;
  if (const std::optional<Value> value = call.keyword("limit")) {
    if (!value->is_null()) {
      if (!value->is_integer()) {
        return call.fault("TypeError", "limit must be Int or null");
      }
      if (value->as_integer() < 0) {
        return call.fault("ArgumentError", "limit must be non-negative");
      }
      limit = static_cast<std::size_t>(value->as_integer());
    }
  }
  if (call.selector == "read_text") {
    if (const std::optional<Value> encoding = call.keyword("encoding")) {
      const std::optional<std::string> name = call.text_of(*encoding);
      if (!name.has_value()) {
        return call.fault("TypeError", "encoding must be Symbol or Str");
      }
      if (*name != "utf8") {
        return call.fault("ArgumentError", "only utf8 encoding is supported");
      }
    }
  }
  std::string bytes;
  const bool ok = call.selector == "read_text"
                      ? call.fs_read_text(path->string(), limit, &bytes)
                      : call.fs_read_bytes(path->string(), limit, &bytes);
  if (!ok) {
    return SendStatus::Faulted;
  }
  *call.out = call.selector == "read_text" ? call.string_value(std::move(bytes))
                                           : call.bytes_value(std::move(bytes));
  return SendStatus::Matched;
}

SendStatus fs_write_unary_send(NativeStdlibCall &call) {
  if (call.selector != "mkdir" && call.selector != "mkdir_p" &&
      call.selector != "remove") {
    return SendStatus::NotHandled;
  }
  if (!call.require_no_block() || !call.require_arity(1)) {
    return SendStatus::Faulted;
  }
  const std::shared_ptr<RuntimePath> path = path_from_value(call, call.args[0]);
  if (path == nullptr) {
    return SendStatus::Faulted;
  }
  if (!call.kw_args.empty()) {
    return call.fault("TypeError", call.selector + " does not accept keywords");
  }
  const bool ok = call.selector == "mkdir"     ? call.fs_mkdir(path->string())
                  : call.selector == "mkdir_p" ? call.fs_mkdir_p(path->string())
                                               : call.fs_remove(path->string());
  if (!ok) {
    return SendStatus::Faulted;
  }
  *call.out = Value::null();
  return SendStatus::Matched;
}

SendStatus fs_write_send(NativeStdlibCall &call) {
  if (call.selector != "write_bytes" && call.selector != "write_text") {
    return SendStatus::NotHandled;
  }
  if (!call.require_no_block() || !call.require_arity(2) ||
      !call.reject_unknown_keywords({"create", "truncate", "encoding"})) {
    return SendStatus::Faulted;
  }
  const std::shared_ptr<RuntimePath> path = path_from_value(call, call.args[0]);
  if (path == nullptr) {
    return SendStatus::Faulted;
  }
  std::optional<std::string> bytes;
  if (call.selector == "write_text") {
    if (!call.args[1].is_string()) {
      return call.fault("TypeError", "write_text expects Str text");
    }
    bytes = call.text_of(call.args[1]);
    if (!bytes.has_value()) {
      return call.fault("VMError", "string ref is invalid");
    }
    if (const std::optional<Value> encoding = call.keyword("encoding")) {
      const std::optional<std::string> name = call.text_of(*encoding);
      if (!name.has_value() || *name != "utf8") {
        return call.fault(name.has_value() ? "ArgumentError" : "TypeError",
                          "only utf8 encoding is supported");
      }
    }
  } else {
    bytes = bytes_from_value(call, call.args[1]);
  }
  if (!bytes.has_value()) {
    return SendStatus::Faulted;
  }
  bool create = true;
  bool truncate = true;
  if (!call.bool_keyword("create", true, &create) ||
      !call.bool_keyword("truncate", true, &truncate)) {
    return SendStatus::Faulted;
  }
  const bool ok =
      call.selector == "write_text"
          ? call.fs_write_text(path->string(), *bytes, create, truncate)
          : call.fs_write_bytes(path->string(), *bytes, create, truncate);
  if (!ok) {
    return SendStatus::Faulted;
  }
  *call.out = Value::null();
  return SendStatus::Matched;
}

SendStatus fs_namespace_send(NativeStdlibCall &call) {
  if (const SendStatus status = fs_metadata_send(call);
      status != SendStatus::NotHandled) {
    return status;
  }
  if (const SendStatus status = fs_read_send(call);
      status != SendStatus::NotHandled) {
    return status;
  }
  if (const SendStatus status = fs_write_unary_send(call);
      status != SendStatus::NotHandled) {
    return status;
  }
  if (const SendStatus status = fs_write_send(call);
      status != SendStatus::NotHandled) {
    return status;
  }
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
