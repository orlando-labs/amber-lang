#include "runtime/io.h"
#include "runtime/stdlib_registry.h"
#include "runtime/text.h"

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

std::optional<std::shared_ptr<RuntimeTextWriter>>
text_writer_from_value(NativeStdlibCall &call, const Value &value,
                       const std::string &context) {
  if (!value.is_text_writer()) {
    call.fault("TypeError", context + " expects text writer");
    return std::nullopt;
  }
  std::shared_ptr<RuntimeTextWriter> writer = value.as_text_writer();
  if (writer == nullptr) {
    call.fault("TypeError", context + " text writer is null");
    return std::nullopt;
  }
  return writer;
}

std::optional<RuntimeLogLevel>
log_level_from_value(NativeStdlibCall &call, const Value &value,
                     const std::string &context) {
  const std::optional<std::string> text = call.text_of(value);
  if (!text.has_value()) {
    call.fault("TypeError",
               context + " must be :fatal, :error, :warn, :info, or :debug");
    return std::nullopt;
  }
  if (*text == "fatal") {
    return RuntimeLogLevel::Fatal;
  }
  if (*text == "error") {
    return RuntimeLogLevel::Error;
  }
  if (*text == "warn" || *text == "warning") {
    return RuntimeLogLevel::Warn;
  }
  if (*text == "info") {
    return RuntimeLogLevel::Info;
  }
  if (*text == "debug") {
    return RuntimeLogLevel::Debug;
  }
  call.fault("TypeError",
             context + " must be :fatal, :error, :warn, :info, or :debug");
  return std::nullopt;
}

std::optional<RuntimeLogColorMode>
log_color_mode_from_value(NativeStdlibCall &call, const Value &value,
                          const std::string &context) {
  if (value.is_bool()) {
    return value.as_bool() ? RuntimeLogColorMode::Always
                           : RuntimeLogColorMode::Never;
  }
  const std::optional<std::string> text = call.text_of(value);
  if (!text.has_value()) {
    call.fault("TypeError",
               context + " must be Bool, :auto, :always, or :never");
    return std::nullopt;
  }
  if (*text == "auto") {
    return RuntimeLogColorMode::Auto;
  }
  if (*text == "always") {
    return RuntimeLogColorMode::Always;
  }
  if (*text == "never") {
    return RuntimeLogColorMode::Never;
  }
  call.fault("TypeError", context + " must be Bool, :auto, :always, or :never");
  return std::nullopt;
}

SendStatus text_buffer_type_send(NativeStdlibCall &call) {
  if (call.selector != "new") {
    return SendStatus::NotHandled;
  }
  if (!call.require_arity(0) || !call.kw_args.empty() ||
      !call.require_no_block()) {
    if (!call.kw_args.empty()) {
      call.fault("TypeError", "io.Buffer.new does not accept keywords");
    }
    return SendStatus::Faulted;
  }
  *call.out = Value::text_writer(RuntimeTextWriter::buffer());
  return SendStatus::Matched;
}

SendStatus logger_type_send(NativeStdlibCall &call) {
  if (call.selector != "new") {
    return SendStatus::NotHandled;
  }
  if (!call.require_arity(0) ||
      !call.reject_unknown_keywords({"to", "level", "color"}) ||
      !call.require_no_block()) {
    return SendStatus::Faulted;
  }
  std::shared_ptr<RuntimeTextWriter> writer = current_runtime_stderr();
  RuntimeLogLevel level = RuntimeLogLevel::Info;
  RuntimeLogColorMode color_mode = RuntimeLogColorMode::Auto;
  if (const std::optional<Value> to = call.keyword("to")) {
    const std::optional<std::shared_ptr<RuntimeTextWriter>> explicit_writer =
        text_writer_from_value(call, *to, "to:");
    if (!explicit_writer.has_value()) {
      return SendStatus::Faulted;
    }
    writer = *explicit_writer;
  }
  if (const std::optional<Value> level_value = call.keyword("level")) {
    const std::optional<RuntimeLogLevel> parsed =
        log_level_from_value(call, *level_value, "level");
    if (!parsed.has_value()) {
      return SendStatus::Faulted;
    }
    level = *parsed;
  }
  if (const std::optional<Value> color_value = call.keyword("color")) {
    const std::optional<RuntimeLogColorMode> parsed =
        log_color_mode_from_value(call, *color_value, "color");
    if (!parsed.has_value()) {
      return SendStatus::Faulted;
    }
    color_mode = *parsed;
  }
  *call.out =
      Value::logger(std::make_shared<RuntimeLogger>(writer, level, color_mode));
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
          {{RuntimeNativeTypeKind::TextBuffer, text_buffer_type_send},
           {RuntimeNativeTypeKind::Logger, logger_type_send},
           {RuntimeNativeTypeKind::Bytes, bytes_type_send},
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
