#include "runtime/io.h"
#include "runtime/stdlib_registry.h"
#include "runtime/text.h"

#include <cstdint>
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

SendStatus bytes_instance_send(NativeStdlibCall &call) {
  const auto bytes =
      std::dynamic_pointer_cast<RuntimeBytes>(call.receiver.as_io_value());
  if (bytes == nullptr) {
    return SendStatus::NotHandled;
  }
  if (!call.require_no_block()) {
    return SendStatus::Faulted;
  }
  if (call.selector == "count") {
    if (!call.require_arity(0) || !call.kw_args.empty()) {
      return SendStatus::Faulted;
    }
    *call.out = Value::integer(static_cast<std::int64_t>(bytes->count()));
    return SendStatus::Matched;
  }
  if (call.selector == "empty?") {
    if (!call.require_arity(0) || !call.kw_args.empty()) {
      return SendStatus::Faulted;
    }
    *call.out = Value::boolean(bytes->empty());
    return SendStatus::Matched;
  }
  if (call.selector == "[]") {
    if (!call.require_arity(1) || !call.kw_args.empty() ||
        !call.args[0].is_integer()) {
      if (!call.args.empty() && !call.args[0].is_integer()) {
        call.fault("TypeError", "Bytes index must be Int");
      }
      return SendStatus::Faulted;
    }
    RuntimeByteResult result = bytes->at(call.args[0].as_integer());
    if (!set_fault_from_io_status(call, result)) {
      return SendStatus::Faulted;
    }
    *call.out = Value::integer(result.byte);
    return SendStatus::Matched;
  }
  if (call.selector == "slice") {
    if ((call.args.size() != 1U && call.args.size() != 2U) ||
        !call.kw_args.empty() || !call.args[0].is_integer() ||
        (call.args.size() == 2U && !call.args[1].is_null() &&
         !call.args[1].is_integer())) {
      call.fault("TypeError", "Bytes.slice expects Int start/length");
      return SendStatus::Faulted;
    }
    std::optional<std::size_t> length;
    if (call.args.size() == 2U && !call.args[1].is_null()) {
      if (call.args[1].as_integer() < 0) {
        call.fault("ArgumentError", "slice length must be non-negative");
        return SendStatus::Faulted;
      }
      length = static_cast<std::size_t>(call.args[1].as_integer());
    }
    std::int64_t start = call.args[0].as_integer();
    if (start < 0) {
      start += static_cast<std::int64_t>(bytes->count());
    }
    if (start < 0 || static_cast<std::size_t>(start) > bytes->count()) {
      call.fault("IndexError", "Bytes slice is out of bounds");
      return SendStatus::Faulted;
    }
    *call.out =
        Value::io_value(bytes->slice(call.args[0].as_integer(), length));
    return SendStatus::Matched;
  }
  if (call.selector == "to_str") {
    if (call.args.size() > 1U || !call.reject_unknown_keywords({"encoding"})) {
      return SendStatus::Faulted;
    }
    Value encoding = !call.args.empty()
                         ? call.args[0]
                         : call.keyword("encoding").value_or(Value::null());
    std::string name = "utf8";
    if (!encoding.is_null()) {
      const std::optional<std::string> parsed = call.text_of(encoding);
      if (!parsed.has_value()) {
        call.fault("TypeError", "encoding must be Symbol or Str");
        return SendStatus::Faulted;
      }
      name = *parsed;
    }
    RuntimeIoStatus result = bytes->to_string(name);
    if (!set_fault_from_io_status(call, result)) {
      return SendStatus::Faulted;
    }
    *call.out = call.string_value(result.bytes);
    return SendStatus::Matched;
  }
  if (call.selector == "hex") {
    if (!call.require_arity(0) || !call.kw_args.empty()) {
      return SendStatus::Faulted;
    }
    *call.out = call.string_value(bytes->hex());
    return SendStatus::Matched;
  }
  return SendStatus::NotHandled;
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

SendStatus byte_buffer_instance_send(NativeStdlibCall &call) {
  const auto buffer =
      std::dynamic_pointer_cast<RuntimeByteBuffer>(call.receiver.as_io_value());
  if (buffer == nullptr) {
    return SendStatus::NotHandled;
  }
  RuntimeIoStatus access = buffer->access_status();
  if (!set_fault_from_io_status(call, access)) {
    return SendStatus::Faulted;
  }
  if (!call.require_no_block()) {
    return SendStatus::Faulted;
  }
  if (call.selector == "capacity" || call.selector == "position" ||
      call.selector == "limit" || call.selector == "count" ||
      call.selector == "remaining" || call.selector == "empty?" ||
      call.selector == "full?") {
    if (!call.require_arity(0) || !call.kw_args.empty()) {
      return SendStatus::Faulted;
    }
    if (call.selector == "empty?") {
      *call.out = Value::boolean(buffer->empty());
    } else if (call.selector == "full?") {
      *call.out = Value::boolean(buffer->full());
    } else {
      const std::size_t value =
          call.selector == "capacity"
              ? buffer->capacity()
              : (call.selector == "position"
                     ? buffer->position()
                     : (call.selector == "limit"
                            ? buffer->limit()
                            : (call.selector == "count"
                                   ? buffer->count()
                                   : buffer->remaining())));
      *call.out = Value::integer(static_cast<std::int64_t>(value));
    }
    return SendStatus::Matched;
  }
  if (call.selector == "clear!" || call.selector == "flip!" ||
      call.selector == "rewind!" || call.selector == "compact!") {
    if (!call.require_arity(0) || !call.kw_args.empty()) {
      return SendStatus::Faulted;
    }
    RuntimeIoStatus result =
        call.selector == "clear!"
            ? buffer->clear()
            : (call.selector == "flip!"
                   ? buffer->flip()
                   : (call.selector == "rewind!" ? buffer->rewind()
                                                 : buffer->compact()));
    if (!set_fault_from_io_status(call, result)) {
      return SendStatus::Faulted;
    }
    *call.out = call.selector == "clear!" ? Value::null() : call.receiver;
    return SendStatus::Matched;
  }
  if (call.selector == "get!" || call.selector == "get_at") {
    if ((call.selector == "get!" && !call.require_arity(0)) ||
        (call.selector == "get_at" && !call.require_arity(1)) ||
        !call.kw_args.empty()) {
      return SendStatus::Faulted;
    }
    if (call.selector == "get_at" && !call.args[0].is_integer()) {
      call.fault("TypeError", "ByteBuffer index must be Int");
      return SendStatus::Faulted;
    }
    RuntimeByteResult result = call.selector == "get!"
                                   ? buffer->get()
                                   : buffer->get_at(call.args[0].as_integer());
    if (!set_fault_from_io_status(call, result)) {
      return SendStatus::Faulted;
    }
    *call.out = Value::integer(result.byte);
    return SendStatus::Matched;
  }
  if (call.selector == "put!" || call.selector == "put_all!") {
    if (!call.require_arity(1) || !call.kw_args.empty()) {
      return SendStatus::Faulted;
    }
    RuntimeIoStatus result;
    if (call.selector == "put!") {
      if (!call.args[0].is_integer()) {
        call.fault("TypeError", "ByteBuffer byte must be Int");
        return SendStatus::Faulted;
      }
      result = buffer->put(call.args[0].as_integer());
    } else {
      const std::optional<std::string> input =
          bytes_from_value(call, call.args[0]);
      if (!input.has_value()) {
        return SendStatus::Faulted;
      }
      result = buffer->put_all(*input);
    }
    if (!set_fault_from_io_status(call, result)) {
      return SendStatus::Faulted;
    }
    *call.out = call.receiver;
    return SendStatus::Matched;
  }
  if (call.selector == "read_slice" || call.selector == "write_slice") {
    if (call.args.size() > 1U || !call.kw_args.empty()) {
      return SendStatus::Faulted;
    }
    std::optional<std::size_t> length;
    if (!call.args.empty() && !call.args[0].is_null()) {
      if (!call.args[0].is_integer()) {
        call.fault("TypeError", "slice length must be Int or null");
        return SendStatus::Faulted;
      }
      if (call.args[0].as_integer() < 0) {
        call.fault("ArgumentError", "slice length must be non-negative");
        return SendStatus::Faulted;
      }
      length = static_cast<std::size_t>(call.args[0].as_integer());
    }
    if ((call.selector == "read_slice" && !buffer->read_mode()) ||
        (call.selector == "write_slice" && buffer->read_mode())) {
      call.fault("ArgumentError", call.selector == "read_slice"
                                      ? "read_slice requires read mode"
                                      : "write_slice requires write mode");
      return SendStatus::Faulted;
    }
    *call.out = Value::io_value(call.selector == "read_slice"
                                    ? buffer->read_slice(length)
                                    : buffer->write_slice(length));
    return SendStatus::Matched;
  }
  if (call.selector == "slice") {
    if ((call.args.size() != 1U && call.args.size() != 2U) ||
        !call.kw_args.empty() || !call.args[0].is_integer()) {
      call.fault("TypeError", "ByteBuffer.slice expects Int start");
      return SendStatus::Faulted;
    }
    std::optional<std::size_t> length;
    if (call.args.size() == 2U && !call.args[1].is_null()) {
      if (!call.args[1].is_integer()) {
        call.fault("TypeError", "slice length must be Int or null");
        return SendStatus::Faulted;
      }
      if (call.args[1].as_integer() < 0) {
        call.fault("ArgumentError", "slice length must be non-negative");
        return SendStatus::Faulted;
      }
      length = static_cast<std::size_t>(call.args[1].as_integer());
    }
    std::int64_t start = call.args[0].as_integer();
    if (start < 0) {
      start += static_cast<std::int64_t>(buffer->count());
    }
    if (start < 0 || static_cast<std::size_t>(start) > buffer->count()) {
      call.fault("IndexError", "ByteBuffer slice is out of bounds");
      return SendStatus::Faulted;
    }
    *call.out =
        Value::io_value(buffer->byte_slice(call.args[0].as_integer(), length));
    return SendStatus::Matched;
  }
  if (call.selector == "bytes" || call.selector == "copy_bytes" ||
      call.selector == "freeze_bytes!") {
    if (!call.require_arity(0) || !call.kw_args.empty()) {
      return SendStatus::Faulted;
    }
    *call.out = Value::io_value(
        call.selector == "bytes"
            ? std::make_shared<RuntimeBytes>(buffer->bytes())
            : (call.selector == "copy_bytes" ? buffer->copy_bytes()
                                             : buffer->freeze_bytes()));
    return SendStatus::Matched;
  }
  return SendStatus::NotHandled;
}

SendStatus byte_slice_instance_send(NativeStdlibCall &call) {
  const auto slice =
      std::dynamic_pointer_cast<RuntimeByteSlice>(call.receiver.as_io_value());
  if (slice == nullptr) {
    return SendStatus::NotHandled;
  }
  if (!call.require_no_block()) {
    return SendStatus::Faulted;
  }
  if (call.selector == "count" || call.selector == "bytes" ||
      call.selector == "copy_bytes" || call.selector == "owner" ||
      call.selector == "shareable?") {
    if (!call.require_arity(0) || !call.kw_args.empty()) {
      return SendStatus::Faulted;
    }
    if (call.selector == "count") {
      *call.out = Value::integer(static_cast<std::int64_t>(slice->count()));
    } else if (call.selector == "shareable?") {
      *call.out = Value::boolean(slice->shareable());
    } else if (call.selector == "owner") {
      const std::shared_ptr<RuntimeByteBuffer> owner = slice->owner();
      *call.out = owner == nullptr ? Value::null() : Value::io_value(owner);
    } else {
      *call.out = Value::io_value(
          call.selector == "bytes" ? slice->bytes() : slice->copy_bytes());
    }
    return SendStatus::Matched;
  }
  return SendStatus::NotHandled;
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

std::optional<RuntimeIsolationMode>
isolation_from_keywords(NativeStdlibCall &call) {
  const std::optional<Value> value = call.keyword("isolation");
  if (!value.has_value()) {
    return RuntimeIsolationMode::Checked;
  }
  const std::optional<std::string> name = call.text_of(*value);
  if (!name.has_value()) {
    call.fault("TypeError", "isolation must be Symbol or Str");
    return std::nullopt;
  }
  const std::optional<RuntimeIsolationMode> isolation =
      runtime_isolation_mode_from_name(*name);
  if (!isolation.has_value()) {
    call.fault("ArgumentError", "unsupported isolation mode");
  }
  return isolation;
}

SendStatus io_pipe_type_send(NativeStdlibCall &call) {
  if (call.selector != "new" && call.selector != "__call__") {
    return SendStatus::NotHandled;
  }
  if (call.args.size() > 1U ||
      !call.reject_unknown_keywords({"capacity", "isolation"}) ||
      !call.require_no_block()) {
    return SendStatus::Faulted;
  }
  Value capacity_value =
      call.args.empty()
          ? call.keyword("capacity").value_or(Value::integer(65536))
          : call.args[0];
  if (!capacity_value.is_integer()) {
    return call.fault("TypeError", "pipe capacity must be Int");
  }
  const std::optional<RuntimeIsolationMode> isolation =
      isolation_from_keywords(call);
  if (!isolation.has_value()) {
    return SendStatus::Faulted;
  }
  RuntimePipeResult pipe =
      RuntimePipe::create(capacity_value.as_integer(), *isolation);
  if (!set_fault_from_io_status(call, pipe)) {
    return SendStatus::Faulted;
  }
  if (call.selector == "__call__") {
    *call.out = Value::io_value(pipe.pipe);
  } else {
    *call.out = call.make_tuple(
        {Value::io_value(pipe.reader), Value::io_value(pipe.writer)});
  }
  return SendStatus::Matched;
}

SendStatus io_namespace_send(NativeStdlibCall &call) {
  if (call.selector == "ByteBuffer") {
    if (call.args.empty()) {
      if (!call.kw_args.empty() || !call.require_no_block()) {
        return SendStatus::Faulted;
      }
      *call.out = Value::native_type(RuntimeNativeTypeKind::ByteBuffer);
      return SendStatus::Matched;
    }
    if (!call.require_arity(1) || !call.kw_args.empty() ||
        !call.require_no_block()) {
      return SendStatus::Faulted;
    }
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
  if (call.selector == "Pipe") {
    if (call.args.empty() && call.kw_args.empty()) {
      if (!call.require_no_block()) {
        return SendStatus::Faulted;
      }
      *call.out = Value::native_type(RuntimeNativeTypeKind::IoPipe);
      return SendStatus::Matched;
    }
    const std::string call_selector = "__call__";
    NativeStdlibCall pipe_call{
        call.host,     call.frame, call.receiver, RuntimeNativeTypeKind::IoPipe,
        call_selector, call.args,  call.block,    call.kw_args,
        call.out};
    return io_pipe_type_send(pipe_call);
  }
  if (call.selector == "Buffer") {
    if (!call.require_arity(0) || !call.kw_args.empty() ||
        !call.require_no_block()) {
      if (!call.kw_args.empty()) {
        call.fault("TypeError", "io.Buffer does not accept keywords");
      }
      return SendStatus::Faulted;
    }
    *call.out = Value::native_type(RuntimeNativeTypeKind::TextBuffer);
    return SendStatus::Matched;
  }
  if (call.selector == "Logger") {
    if (!call.require_arity(0) || !call.kw_args.empty() ||
        !call.require_no_block()) {
      if (!call.kw_args.empty()) {
        call.fault("TypeError", "io.Logger does not accept keywords");
      }
      return SendStatus::Faulted;
    }
    *call.out = Value::native_type(RuntimeNativeTypeKind::Logger);
    return SendStatus::Matched;
  }
  if (call.selector == "current_stdout" || call.selector == "stdout") {
    if (!call.require_arity(0) || !call.kw_args.empty() ||
        !call.require_no_block()) {
      if (!call.kw_args.empty()) {
        call.fault("TypeError", "io.stdout does not accept keywords");
      }
      return SendStatus::Faulted;
    }
    *call.out = Value::text_writer(current_runtime_stdout());
    return SendStatus::Matched;
  }
  if (call.selector == "current_stderr" || call.selector == "stderr") {
    if (!call.require_arity(0) || !call.kw_args.empty() ||
        !call.require_no_block()) {
      if (!call.kw_args.empty()) {
        call.fault("TypeError", "io.stderr does not accept keywords");
      }
      return SendStatus::Faulted;
    }
    *call.out = Value::text_writer(current_runtime_stderr());
    return SendStatus::Matched;
  }
  if (call.selector == "with_output") {
    if (!call.require_arity(0) ||
        !call.reject_unknown_keywords({"stdout", "stderr"})) {
      return SendStatus::Faulted;
    }
    std::shared_ptr<RuntimeTextWriter> stdout_writer;
    std::shared_ptr<RuntimeTextWriter> stderr_writer;
    if (const std::optional<Value> stdout_kw = call.keyword("stdout")) {
      const std::optional<std::shared_ptr<RuntimeTextWriter>> writer =
          text_writer_from_value(call, *stdout_kw, "stdout:");
      if (!writer.has_value()) {
        return SendStatus::Faulted;
      }
      stdout_writer = *writer;
    }
    if (const std::optional<Value> stderr_kw = call.keyword("stderr")) {
      const std::optional<std::shared_ptr<RuntimeTextWriter>> writer =
          text_writer_from_value(call, *stderr_kw, "stderr:");
      if (!writer.has_value()) {
        return SendStatus::Faulted;
      }
      stderr_writer = *writer;
    }
    RuntimeOutputScope scope(std::move(stdout_writer),
                             std::move(stderr_writer));
    const StdlibBlockResult block = call.call_block(call.block, {});
    if (block.status == StdlibBlockStatus::Returned) {
      *call.out = block.value;
      return SendStatus::Matched;
    }
    if (block.status == StdlibBlockStatus::Raised) {
      return call.raise(block.exception);
    }
    return SendStatus::Faulted;
  }
  return SendStatus::NotHandled;
}

RuntimeNativeModuleDescriptor io_module_descriptor() {
  return {{{"io", RuntimeNativeTypeKind::Io},
           {"io.Buffer", RuntimeNativeTypeKind::TextBuffer},
           {"io.Logger", RuntimeNativeTypeKind::Logger},
           {"Bytes", RuntimeNativeTypeKind::Bytes},
           {"io.ByteBuffer", RuntimeNativeTypeKind::ByteBuffer},
           {"io.ByteSlice", RuntimeNativeTypeKind::ByteSlice},
           {"io.Pipe", RuntimeNativeTypeKind::IoPipe}},
          {{RuntimeNativeTypeKind::Io, io_namespace_send},
           {RuntimeNativeTypeKind::TextBuffer, text_buffer_type_send},
           {RuntimeNativeTypeKind::Logger, logger_type_send},
           {RuntimeNativeTypeKind::Bytes, bytes_type_send},
           {RuntimeNativeTypeKind::ByteBuffer, byte_buffer_type_send},
           {RuntimeNativeTypeKind::IoPipe, io_pipe_type_send}},
          {{"Bytes", RuntimeNativeTypeKind::Bytes, bytes_instance_send},
           {"io.ByteBuffer", RuntimeNativeTypeKind::ByteBuffer,
            byte_buffer_instance_send},
           {"io.ByteSlice", RuntimeNativeTypeKind::ByteSlice,
            byte_slice_instance_send}},
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
