#include "runtime/io.h"
#include "runtime/stdlib_registry.h"

#include <memory>
#include <optional>
#include <string>

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

SendStatus bytes_type_send(NativeStdlibCall &call) {
  if (call.selector != "new") {
    return SendStatus::NotHandled;
  }
  if (!call.require_arity(1) || !call.kw_args.empty() ||
      !call.require_no_block()) {
    return SendStatus::Faulted;
  }
  const std::optional<std::string> bytes = bytes_from_value(call, call.args[0]);
  if (!bytes.has_value()) {
    return SendStatus::Faulted;
  }
  *call.out = Value::io_value(std::make_shared<RuntimeBytes>(*bytes));
  return SendStatus::Matched;
}

SendStatus byte_buffer_type_send(NativeStdlibCall &call) {
  if (call.selector != "new" && call.selector != "from" &&
      call.selector != "wrap") {
    return SendStatus::NotHandled;
  }
  if (!call.require_arity(1) || !call.kw_args.empty() ||
      !call.require_no_block()) {
    return SendStatus::Faulted;
  }
  if (call.selector == "new") {
    if (!call.args[0].is_integer()) {
      return call.fault("TypeError", "ByteBuffer capacity must be Int");
    }
    if (call.args[0].as_integer() < 0) {
      return call.fault("ArgumentError",
                        "ByteBuffer capacity must be non-negative");
    }
    *call.out = Value::io_value(std::make_shared<RuntimeByteBuffer>(
        static_cast<std::size_t>(call.args[0].as_integer())));
    return SendStatus::Matched;
  }
  const std::optional<std::string> bytes = bytes_from_value(call, call.args[0]);
  if (!bytes.has_value()) {
    return SendStatus::Faulted;
  }
  *call.out = Value::io_value(
      std::make_shared<RuntimeByteBuffer>(RuntimeBytes(*bytes)));
  return SendStatus::Matched;
}

RuntimeNativeModuleDescriptor io_module_descriptor() {
  return {{{"io", RuntimeNativeTypeKind::Io},
           {"io.Buffer", RuntimeNativeTypeKind::TextBuffer},
           {"io.Logger", RuntimeNativeTypeKind::Logger},
           {"Bytes", RuntimeNativeTypeKind::Bytes},
           {"io.ByteBuffer", RuntimeNativeTypeKind::ByteBuffer},
           {"io.ByteSlice", RuntimeNativeTypeKind::ByteSlice},
           {"io.Pipe", RuntimeNativeTypeKind::IoPipe}},
          {{RuntimeNativeTypeKind::Bytes, bytes_type_send},
           {RuntimeNativeTypeKind::ByteBuffer, byte_buffer_type_send}},
          {{RuntimeNativeTypeKind::Bytes, "new"},
           {RuntimeNativeTypeKind::ByteBuffer, "new"},
           {RuntimeNativeTypeKind::IoPipe, "__call__"}}};
}

} // namespace

void register_io_runtime_module(RuntimeModuleRegistry &modules,
                                RuntimeDispatchRegistry &dispatch,
                                RuntimeTypeRegistry &types) {
  const RuntimeNativeModuleDescriptor descriptor = io_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
}

} // namespace amber::runtime
