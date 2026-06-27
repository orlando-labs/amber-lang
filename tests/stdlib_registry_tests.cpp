// Unit tests for the Layer 0 stdlib substrate
// (DESIGN-stdlib-next-libs-order-2026-06-15 §4.0): the registry tables and the
// NativeStdlibCall facade ABI, exercised through the migrated Math handler with
// a mock host -- no full VM required, which is the point of the seam.

#include "runtime/stdlib_registry.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using amber::runtime::NativeRegistry;
using amber::runtime::NativeStdlibCall;
using amber::runtime::NativeStdlibHandler;
using amber::runtime::RuntimeBindingKind;
using amber::runtime::RuntimeBindingRef;
using amber::runtime::RuntimeDispatchRegistry;
using amber::runtime::RuntimeErrorRegistry;
using amber::runtime::RuntimeModuleRegistry;
using amber::runtime::RuntimeNativeFunctionKind;
using amber::runtime::RuntimeNativeTypeKind;
using amber::runtime::RuntimeTypeCallDescriptor;
using amber::runtime::RuntimeTypeRegistry;
using amber::runtime::SendStatus;
using amber::runtime::StdlibHost;
using amber::runtime::Value;

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "stdlib registry test failed: " << message << "\n";
    std::exit(1);
  }
}

// A minimal StdlibHost: records the last fault and answers keyword queries from
// an in-memory list. Enough to drive any pure-compute handler through the ABI.
struct MockHost : StdlibHost {
  bool faulted = false;
  std::string fault_class;
  std::string fault_message;

  void stdlib_set_fault(const void * /*frame*/, const std::string &error_class,
                        const std::string &message) override {
    faulted = true;
    fault_class = error_class;
    fault_message = message;
  }
  void stdlib_raise_exception(const void * /*frame*/,
                              Value /*exception*/) override {}
  void stdlib_raise_runtime_error(const void * /*frame*/,
                                  const std::string & /*error_class*/,
                                  const std::string & /*message*/) override {}
  bool stdlib_write_output(const void * /*frame*/, bool /*stderr_stream*/,
                           const std::string & /*text*/) override {
    return true;
  }
  std::optional<Value> stdlib_keyword_arg_value(
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      const std::string & /*name*/) override {
    return std::nullopt;
  }
  bool stdlib_reject_unknown_keywords(
      const void * /*frame*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      std::initializer_list<const char *> /*allowed*/) override {
    return true;
  }
  Value stdlib_string_value_from_text(std::string /*text*/) override {
    return Value::null();
  }
  Value stdlib_symbol_value_from_text(std::string /*text*/) override {
    return Value::null();
  }
  std::optional<std::string> stdlib_text_of(const Value & /*value*/) override {
    return std::nullopt;
  }
  std::optional<std::string> stdlib_bytes_of(const void * /*frame*/,
                                             const Value & /*value*/) override {
    return std::nullopt;
  }
  Value stdlib_bytes_value_from_bytes(std::string /*bytes*/) override {
    return Value::null();
  }
  bool stdlib_bytes_view(const void * /*frame*/, const Value & /*value*/,
                         const std::uint8_t ** /*ptr*/, std::size_t * /*len*/,
                         Value * /*keepalive*/) override {
    return false;
  }
  Value stdlib_make_list(std::vector<Value> /*items*/) override {
    return Value::null();
  }
  Value
  stdlib_make_object(std::vector<std::pair<std::string, Value>> /*entries*/,
                     bool /*strict*/) override {
    return Value::null();
  }
  bool stdlib_string_keyed_entries(
      const void * /*frame*/, const Value & /*value*/,
      std::vector<std::pair<std::string, Value>> * /*out*/) override {
    return false;
  }
  bool stdlib_lookup_string_key(const void * /*frame*/, const Value & /*value*/,
                                const std::string & /*key*/, Value * /*out*/,
                                bool * /*found*/) override {
    return false;
  }
  bool stdlib_map_values(const void * /*frame*/, const Value & /*value*/,
                         std::vector<Value> * /*out*/) override {
    return false;
  }
  bool stdlib_list_items(const void * /*frame*/, const Value & /*value*/,
                         std::vector<Value> * /*out*/) override {
    return false;
  }
  bool stdlib_sequence_items(const void * /*frame*/, const Value & /*value*/,
                             std::vector<Value> * /*out*/) override {
    return false;
  }
  amber::runtime::StdlibBlockResult
  stdlib_call_stream_block(const void * /*frame*/, const Value & /*block*/,
                           Value /*value*/) override {
    return {};
  }
  amber::runtime::StdlibBlockResult
  stdlib_call_path_block(const void * /*frame*/, const Value & /*block*/,
                         Value /*value*/, Value /*accumulator*/) override {
    return {};
  }
  amber::runtime::StdlibBlockResult
  stdlib_call_block(const void * /*frame*/, const Value & /*block*/,
                    std::vector<Value> /*args*/) override {
    return {};
  }
  void stdlib_throw_json_stop(const void * /*frame*/,
                              std::optional<Value> /*value*/) override {}
  bool
  stdlib_integer_range(const void * /*frame*/, const Value & /*value*/,
                       amber::runtime::StdlibIntegerRange * /*out*/) override {
    return false;
  }
  bool stdlib_fs_read_text(const void * /*frame*/, const std::string & /*path*/,
                           std::string * /*out*/) override {
    return false;
  }
  bool stdlib_fs_write_text(const void * /*frame*/,
                            const std::string & /*path*/,
                            const std::string & /*text*/) override {
    return false;
  }
  bool stdlib_secure_random_bytes(const void * /*frame*/, std::size_t /*count*/,
                                  std::string * /*out*/) override {
    return false;
  }
  std::optional<amber::runtime::RuntimeTimeValue>
  stdlib_wall_time_now(const void * /*frame*/) override {
    return amber::runtime::RuntimeTimeValue{};
  }
  std::optional<amber::runtime::RuntimeTimePeriodValue>
  stdlib_monotonic_time(const void * /*frame*/) override {
    return amber::runtime::RuntimeTimePeriodValue{};
  }
};

// Build and dispatch one Math SEND through a registry handler. Returns the
// status; `out` and `host` carry the result / fault back to the caller.
SendStatus dispatch_math_handler(NativeStdlibHandler handler, MockHost &host,
                                 const std::string &selector,
                                 const std::vector<Value> &args,
                                 const Value &block, Value *out) {
  expect(handler != nullptr, "Math must be registered");
  int frame_marker = 0;
  const std::vector<std::pair<std::uint32_t, Value>> kw_args;
  NativeStdlibCall call{host,
                        &frame_marker,
                        Value::native_type(RuntimeNativeTypeKind::Math),
                        RuntimeNativeTypeKind::Math,
                        selector,
                        args,
                        block,
                        kw_args,
                        out};
  return handler(call);
}

SendStatus dispatch_math(const NativeRegistry &registry, MockHost &host,
                         const std::string &selector,
                         const std::vector<Value> &args, const Value &block,
                         Value *out) {
  return dispatch_math_handler(
      registry.handler_for(RuntimeNativeTypeKind::Math), host, selector, args,
      block, out);
}

void test_path_resolution(const NativeRegistry &registry) {
  const std::optional<RuntimeNativeTypeKind> math =
      registry.kind_for_path("Math");
  expect(math.has_value() && *math == RuntimeNativeTypeKind::Math,
         "kind_for_path(\"Math\") resolves to Math");
  const std::optional<RuntimeNativeTypeKind> hex =
      registry.kind_for_path("Hex");
  expect(hex.has_value() && *hex == RuntimeNativeTypeKind::Hex,
         "kind_for_path(\"Hex\") resolves to Hex");
  const std::optional<RuntimeNativeTypeKind> secure_random =
      registry.kind_for_path("SecureRandom");
  expect(secure_random.has_value() &&
             *secure_random == RuntimeNativeTypeKind::SecureRandom,
         "kind_for_path(\"SecureRandom\") resolves to SecureRandom");
  const std::optional<RuntimeNativeTypeKind> argparser =
      registry.kind_for_path("ArgParser");
  expect(argparser.has_value() &&
             *argparser == RuntimeNativeTypeKind::ArgParser,
         "kind_for_path(\"ArgParser\") resolves to ArgParser");
  const std::optional<RuntimeNativeTypeKind> digest =
      registry.kind_for_path("Digest");
  expect(digest.has_value() && *digest == RuntimeNativeTypeKind::Digest,
         "kind_for_path(\"Digest\") resolves to Digest");
  const std::optional<RuntimeNativeTypeKind> url =
      registry.kind_for_path("Url");
  expect(url.has_value() && *url == RuntimeNativeTypeKind::Url,
         "kind_for_path(\"Url\") resolves to Url");
  const std::optional<RuntimeNativeTypeKind> time =
      registry.kind_for_path("Time");
  expect(time.has_value() && *time == RuntimeNativeTypeKind::Time,
         "kind_for_path(\"Time\") resolves to Time");
  const std::optional<RuntimeNativeTypeKind> period =
      registry.kind_for_path("TimePeriod");
  expect(period.has_value() && *period == RuntimeNativeTypeKind::TimePeriod,
         "kind_for_path(\"TimePeriod\") resolves to TimePeriod");
  const std::optional<RuntimeNativeTypeKind> uuid =
      registry.kind_for_path("Uuid");
  expect(uuid.has_value() && *uuid == RuntimeNativeTypeKind::Uuid,
         "kind_for_path(\"Uuid\") resolves to Uuid");
  const std::optional<RuntimeNativeTypeKind> uuid_alias =
      registry.kind_for_path("UUID");
  expect(uuid_alias.has_value() && *uuid_alias == RuntimeNativeTypeKind::Uuid,
         "kind_for_path(\"UUID\") resolves to Uuid");
  expect(!registry.kind_for_path("NotALibrary").has_value(),
         "unregistered path resolves to nullopt");
}

void test_module_registry_imports_native_paths(const NativeRegistry &registry) {
  RuntimeModuleRegistry modules;
  amber::runtime::register_core_prelude_bindings(modules);
  modules.import_native_paths(registry);
  amber::runtime::register_legacy_native_type_paths(modules);

  const std::optional<RuntimeBindingRef> print =
      modules.binding_for_path("print");
  expect(print.has_value() &&
             print->kind == RuntimeBindingKind::NativeFunction &&
             print->native_function == RuntimeNativeFunctionKind::Print,
         "module registry resolves print native function");

  const std::optional<RuntimeBindingRef> task_flow =
      modules.binding_for_path("task.flow");
  expect(task_flow.has_value() &&
             task_flow->kind == RuntimeBindingKind::FlowModule,
         "module registry resolves task.flow module binding");

  const std::optional<RuntimeBindingRef> math =
      modules.binding_for_path("Math");
  expect(math.has_value() && math->kind == RuntimeBindingKind::NativeType &&
             math->native_type == RuntimeNativeTypeKind::Math,
         "module registry resolves Math native binding");

  const std::optional<RuntimeBindingRef> uuid_alias =
      modules.binding_for_path("UUID");
  expect(uuid_alias.has_value() &&
             uuid_alias->kind == RuntimeBindingKind::NativeType &&
             uuid_alias->native_type == RuntimeNativeTypeKind::Uuid,
         "module registry preserves native path aliases");

  const std::optional<RuntimeBindingRef> http_client =
      modules.binding_for_path("net.http.Client");
  expect(!http_client.has_value(),
         "legacy module registry no longer owns net.http.Client binding");

  const std::optional<RuntimeBindingRef> strict_hash =
      modules.binding_for_path("StrictHashMap");
  expect(strict_hash.has_value() &&
             strict_hash->kind == RuntimeBindingKind::NativeType &&
             strict_hash->native_type == RuntimeNativeTypeKind::StrictMap,
         "module registry preserves StrictHashMap alias");

  const std::optional<RuntimeBindingRef> sync_channel =
      modules.binding_for_path("sync.Channel");
  expect(!sync_channel.has_value(),
         "legacy module registry no longer owns sync.Channel alias");

  expect(!modules.binding_for_path("NotALibrary").has_value(),
         "module registry reports unknown paths");
}

void test_handler_table(const NativeRegistry &registry) {
  expect(registry.handler_for(RuntimeNativeTypeKind::Math) != nullptr,
         "Math handler is registered");
  expect(registry.handler_for(RuntimeNativeTypeKind::Base64) != nullptr,
         "Base64 handler is registered");
  expect(registry.handler_for(RuntimeNativeTypeKind::Digest) != nullptr,
         "Digest handler is registered");
  expect(registry.handler_for(RuntimeNativeTypeKind::Url) != nullptr,
         "Url handler is registered");
  expect(registry.handler_for(RuntimeNativeTypeKind::SecureRandom) != nullptr,
         "SecureRandom handler is registered");
  expect(registry.handler_for(RuntimeNativeTypeKind::Uuid) != nullptr,
         "Uuid handler is registered");
  expect(registry.handler_for(RuntimeNativeTypeKind::Time) != nullptr,
         "Time handler is registered");
  expect(registry.handler_for(RuntimeNativeTypeKind::TimePeriod) != nullptr,
         "TimePeriod handler is registered");
  // Kernel has not migrated onto the registry; it must stay on the legacy
  // chain.
  expect(registry.handler_for(RuntimeNativeTypeKind::Kernel) == nullptr,
         "unmigrated kind has no registered handler");
}

void test_dispatch_registry_imports_native_handlers(
    const NativeRegistry &registry) {
  RuntimeDispatchRegistry dispatch;
  dispatch.import_native_handlers(registry);

  const std::optional<NativeStdlibHandler> math =
      dispatch.native_handler(RuntimeNativeTypeKind::Math);
  expect(math.has_value() &&
             *math == registry.handler_for(RuntimeNativeTypeKind::Math),
         "dispatch registry imports Math handler");

  const std::optional<NativeStdlibHandler> json =
      dispatch.native_handler(RuntimeNativeTypeKind::Json);
  expect(json.has_value() &&
             *json == registry.handler_for(RuntimeNativeTypeKind::Json),
         "dispatch registry imports Json handler");

  expect(!dispatch.native_handler(RuntimeNativeTypeKind::Kernel).has_value(),
         "dispatch registry preserves unmigrated native kinds");
}

void test_builtin_runtime_module_descriptors() {
  RuntimeModuleRegistry modules;
  RuntimeDispatchRegistry dispatch;
  RuntimeTypeRegistry types;
  amber::runtime::register_builtin_runtime_modules(modules, dispatch, types);

  const auto expect_path = [&](const std::string &path,
                               RuntimeNativeTypeKind kind) {
    const std::optional<RuntimeBindingRef> binding =
        modules.binding_for_path(path);
    expect(binding.has_value() &&
               binding->kind == RuntimeBindingKind::NativeType &&
               binding->native_type == kind,
           "builtin descriptor registers " + path + " module path");
  };
  const auto expect_handler = [&](RuntimeNativeTypeKind kind,
                                  const std::string &name) {
    expect(dispatch.native_handler(kind).has_value(),
           "builtin descriptor registers " + name + " dispatch handler");
  };
  const auto expect_type_call = [&](RuntimeNativeTypeKind kind,
                                    const std::string &selector,
                                    const std::string &name) {
    const std::optional<RuntimeTypeCallDescriptor> descriptor =
        types.native_type_call(kind);
    expect(descriptor.has_value() && descriptor->selector == selector,
           "builtin descriptor registers " + name + " type call");
  };

  expect_path("Math", RuntimeNativeTypeKind::Math);
  expect_path("io", RuntimeNativeTypeKind::Io);
  expect_path("Bytes", RuntimeNativeTypeKind::Bytes);
  expect_path("io.ByteBuffer", RuntimeNativeTypeKind::ByteBuffer);
  expect_path("io.Pipe", RuntimeNativeTypeKind::IoPipe);
  expect_path("fs", RuntimeNativeTypeKind::Fs);
  expect_path("fs.Path", RuntimeNativeTypeKind::FsPath);
  expect_path("fs.File", RuntimeNativeTypeKind::FsFile);
  expect_path("net", RuntimeNativeTypeKind::Net);
  expect_path("net.Endpoint", RuntimeNativeTypeKind::NetEndpoint);
  expect_path("net.tcp", RuntimeNativeTypeKind::NetTcp);
  expect_path("net.udp", RuntimeNativeTypeKind::NetUdp);
  expect_path("net.http", RuntimeNativeTypeKind::NetHttp);
  expect_path("net.http.Client", RuntimeNativeTypeKind::NetHttpClient);
  expect_path("net.http.Request", RuntimeNativeTypeKind::NetHttpRequest);
  expect_path("net.http.RequestBody",
              RuntimeNativeTypeKind::NetHttpRequestBody);
  expect_path("net.http.Headers", RuntimeNativeTypeKind::NetHttpHeaders);
  expect_path("net.http.Server", RuntimeNativeTypeKind::NetHttpServer);
  expect_path("net.http.ServerRequest",
              RuntimeNativeTypeKind::NetHttpServerRequest);
  expect_path("net.http.ServerResponse",
              RuntimeNativeTypeKind::NetHttpServerResponse);
  expect_path("net.http.json", RuntimeNativeTypeKind::NetHttpJson);
  expect_path("net.http.json.get_json",
              RuntimeNativeTypeKind::NetHttpJsonGetJson);
  expect_path("net.http.json.post_json",
              RuntimeNativeTypeKind::NetHttpJsonPostJson);
  expect_path("net.http.form", RuntimeNativeTypeKind::NetHttpForm);
  expect_path("net.http.form.FormBody", RuntimeNativeTypeKind::NetHttpFormBody);
  expect_path("Flow", RuntimeNativeTypeKind::Flow);
  expect_path("task.flow.Flow", RuntimeNativeTypeKind::Flow);
  expect_path("Channel", RuntimeNativeTypeKind::Channel);
  expect_path("sync.Channel", RuntimeNativeTypeKind::Channel);
  expect_path("Mutex", RuntimeNativeTypeKind::Mutex);
  expect_path("sync.Mutex", RuntimeNativeTypeKind::Mutex);
  expect_path("Atomic", RuntimeNativeTypeKind::Atomic);
  expect_path("sync.Atomic", RuntimeNativeTypeKind::Atomic);
  expect_path("Barrier", RuntimeNativeTypeKind::Barrier);
  expect_path("sync.Barrier", RuntimeNativeTypeKind::Barrier);
  expect_path("ThreadedCollection", RuntimeNativeTypeKind::ThreadedCollection);
  expect_path("task.flow.ThreadedCollection",
              RuntimeNativeTypeKind::ThreadedCollection);
  expect_path("Json", RuntimeNativeTypeKind::Json);
  expect_path("Base64Url", RuntimeNativeTypeKind::Base64Url);
  expect_path("Digest", RuntimeNativeTypeKind::Digest);
  expect_path("SecureRandom", RuntimeNativeTypeKind::SecureRandom);
  expect_path("ArgParser", RuntimeNativeTypeKind::ArgParser);
  expect_path("UUID", RuntimeNativeTypeKind::Uuid);
  expect_path("TimePeriod", RuntimeNativeTypeKind::TimePeriod);
  expect_path("Url", RuntimeNativeTypeKind::Url);

  expect_handler(RuntimeNativeTypeKind::Math, "Math");
  expect_handler(RuntimeNativeTypeKind::Json, "Json");
  expect_handler(RuntimeNativeTypeKind::Base64, "Base64");
  expect_handler(RuntimeNativeTypeKind::Digest, "Digest");
  expect_handler(RuntimeNativeTypeKind::SecureRandom, "SecureRandom");
  expect_handler(RuntimeNativeTypeKind::ArgParser, "ArgParser");
  expect_handler(RuntimeNativeTypeKind::Uuid, "Uuid");
  expect_handler(RuntimeNativeTypeKind::Time, "Time");
  expect_handler(RuntimeNativeTypeKind::Url, "Url");
  expect_handler(RuntimeNativeTypeKind::Channel, "Channel");
  expect_handler(RuntimeNativeTypeKind::Mutex, "Mutex");
  expect_handler(RuntimeNativeTypeKind::Atomic, "Atomic");
  expect_handler(RuntimeNativeTypeKind::Barrier, "Barrier");
  expect_handler(RuntimeNativeTypeKind::Flow, "Flow");
  expect_handler(RuntimeNativeTypeKind::ThreadedCollection,
                 "ThreadedCollection");
  expect_type_call(RuntimeNativeTypeKind::Bytes, "new", "Bytes.new");
  expect_type_call(RuntimeNativeTypeKind::ByteBuffer, "new",
                   "io.ByteBuffer.new");
  expect_type_call(RuntimeNativeTypeKind::IoPipe, "__call__", "io.Pipe()");
  expect_type_call(RuntimeNativeTypeKind::FsPath, "new", "fs.Path.new");
  expect_type_call(RuntimeNativeTypeKind::NetEndpoint, "new",
                   "net.Endpoint.new");
  expect_type_call(RuntimeNativeTypeKind::NetHttpClient, "new",
                   "net.http.Client.new");
  expect_type_call(RuntimeNativeTypeKind::NetHttpRequest, "new",
                   "net.http.Request.new");
  expect_type_call(RuntimeNativeTypeKind::NetHttpRequestBody, "new",
                   "net.http.RequestBody.new");
  expect_type_call(RuntimeNativeTypeKind::NetHttpHeaders, "new",
                   "net.http.Headers.new");
  expect_type_call(RuntimeNativeTypeKind::NetHttpServer, "new",
                   "net.http.Server.new");
  expect_type_call(RuntimeNativeTypeKind::NetHttpServerResponse, "new",
                   "net.http.ServerResponse.new");
  expect_type_call(RuntimeNativeTypeKind::NetHttpJsonGetJson, "__call__",
                   "net.http.json.get_json()");
  expect_type_call(RuntimeNativeTypeKind::NetHttpJsonPostJson, "__call__",
                   "net.http.json.post_json()");
  expect_type_call(RuntimeNativeTypeKind::NetHttpFormBody, "__call__",
                   "net.http.form.FormBody()");
  expect_type_call(RuntimeNativeTypeKind::ArgParser, "new", "ArgParser.new");

  const std::optional<NativeStdlibHandler> math_handler =
      dispatch.native_handler(RuntimeNativeTypeKind::Math);
  expect(math_handler.has_value(),
         "Math descriptor registers dispatch handler");

  MockHost host;
  Value out = Value::null();
  expect(dispatch_math_handler(*math_handler, host, "sqrt",
                               {Value::floating(25.0)}, Value::null(),
                               &out) == SendStatus::Matched,
         "Math descriptor dispatches through runtime registry");
  expect(out.is_float() && std::fabs(out.as_float() - 5.0) < 1e-12,
         "Math.sqrt(25.0) == 5.0 through descriptor");
}

void test_type_call_registry() {
  RuntimeTypeRegistry registry;
  amber::runtime::register_legacy_native_type_calls(registry);

  const std::optional<RuntimeTypeCallDescriptor> argparser =
      registry.native_type_call(RuntimeNativeTypeKind::ArgParser);
  expect(!argparser.has_value(),
         "ArgParser type call moved out of the legacy registry");

  expect(!registry.native_type_call(RuntimeNativeTypeKind::IoPipe).has_value(),
         "IoPipe type call moved out of the legacy registry");

  expect(!registry.native_type_call(RuntimeNativeTypeKind::FsPath).has_value(),
         "FsPath type call moved out of the legacy registry");

  expect(!registry.native_type_call(RuntimeNativeTypeKind::NetEndpoint)
              .has_value(),
         "NetEndpoint type call moved out of the legacy registry");

  expect(!registry.native_type_call(RuntimeNativeTypeKind::NetHttpClient)
              .has_value(),
         "NetHttpClient type call moved out of the legacy registry");

  expect(!registry.native_type_call(RuntimeNativeTypeKind::Math).has_value(),
         "Math is not directly callable as a constructor");
}

void test_math_compute(const NativeRegistry &registry) {
  MockHost host;
  Value out = Value::null();

  expect(dispatch_math(registry, host, "sqrt", {Value::floating(16.0)},
                       Value::null(), &out) == SendStatus::Matched,
         "Math.sqrt dispatches");
  expect(out.is_float() && std::fabs(out.as_float() - 4.0) < 1e-12,
         "Math.sqrt(16.0) == 4.0");

  // `abs` preserves the Int type of its argument.
  out = Value::null();
  expect(dispatch_math(registry, host, "abs", {Value::integer(-5)},
                       Value::null(), &out) == SendStatus::Matched,
         "Math.abs dispatches");
  expect(out.is_integer() && out.as_integer() == 5, "Math.abs(-5) == 5 as Int");

  // `sign` yields an Int.
  out = Value::null();
  expect(dispatch_math(registry, host, "sign", {Value::floating(-2.0)},
                       Value::null(), &out) == SendStatus::Matched,
         "Math.sign dispatches");
  expect(out.is_integer() && out.as_integer() == -1, "Math.sign(-2.0) == -1");
}

void test_math_not_handled(const NativeRegistry &registry) {
  MockHost host;
  Value out = Value::null();
  // An unknown selector returns NotHandled (so the VM falls through to the
  // legacy chain) without recording a fault.
  expect(dispatch_math(registry, host, "definitely_not_a_method",
                       {Value::integer(1)}, Value::null(),
                       &out) == SendStatus::NotHandled,
         "unknown Math selector is NotHandled");
  expect(!host.faulted, "NotHandled must not set a fault");
}

void test_math_faults(const NativeRegistry &registry) {
  // Wrong arity faults via the facade's require_arity.
  {
    MockHost host;
    Value out = Value::null();
    expect(dispatch_math(registry, host, "sqrt", {}, Value::null(), &out) ==
               SendStatus::Faulted,
           "Math.sqrt with no args faults");
    expect(host.faulted && host.fault_class == "TypeError" &&
               host.fault_message == "wrong native stdlib SEND arity",
           "arity fault message matches legacy");
  }
  // A non-numeric argument faults with the selector-specific message.
  {
    MockHost host;
    Value out = Value::null();
    expect(dispatch_math(registry, host, "sqrt", {Value::null()}, Value::null(),
                         &out) == SendStatus::Faulted,
           "Math.sqrt of non-number faults");
    expect(host.fault_message == "Math.sqrt expects a number",
           "type fault message matches legacy");
  }
  // A block argument faults via require_no_block: any non-null block value
  // trips the guard before the selector body runs.
  {
    MockHost host;
    Value out = Value::null();
    const Value block = Value::integer(0);
    expect(dispatch_math(registry, host, "sqrt", {Value::floating(1.0)}, block,
                         &out) == SendStatus::Faulted,
           "Math.sqrt with a block faults");
    expect(host.fault_message ==
               "native stdlib selector does not accept block arguments",
           "block fault message matches legacy");
  }
}

void test_runtime_error_registry() {
  RuntimeErrorRegistry errors;
  const auto exception = errors.error_id("Exception");
  const auto parse_error = errors.error_id("ArgParser.ParseError");
  const auto invalid_value = errors.error_id("ArgParser.InvalidValue");
  const auto unknown_option = errors.error_id("ArgParser.UnknownOption");
  const auto help = errors.error_id("ArgParser.HelpRequested");
  expect(exception.has_value() && parse_error.has_value() &&
             invalid_value.has_value() && unknown_option.has_value() &&
             help.has_value(),
         "dotted ArgParser errors are registered");
  expect(std::string(errors.error_name(*invalid_value)) ==
             "ArgParser.InvalidValue",
         "dotted runtime error name round-trips");
  expect(errors.error_is_a(*invalid_value, *parse_error),
         "ArgParser subclass inherits ParseError");
  expect(errors.error_is_a(*invalid_value, *exception),
         "ArgParser parse errors inherit Exception");
  expect(errors.error_is_a(*help, *exception) &&
             !errors.error_is_a(*help, *parse_error),
         "HelpRequested inherits Exception but not ParseError");
  expect(!errors.error_is_a(*invalid_value, *unknown_option),
         "sibling ArgParser errors do not match");

  const auto json_error = errors.error_id("JsonError");
  const auto json_parse = errors.error_id("JsonParseError");
  const auto type_error = errors.error_id("TypeError");
  expect(json_error.has_value() && json_parse.has_value() &&
             errors.error_is_a(*json_parse, *json_error),
         "existing native error family keeps inherited matching");
  expect(type_error.has_value() && errors.error_is_a(*type_error, *exception),
         "existing native errors inherit Exception");
}

} // namespace

int main() {
  NativeRegistry registry;
  amber::runtime::register_builtin_stdlib(registry);

  test_path_resolution(registry);
  test_module_registry_imports_native_paths(registry);
  test_handler_table(registry);
  test_dispatch_registry_imports_native_handlers(registry);
  test_builtin_runtime_module_descriptors();
  test_type_call_registry();
  test_math_compute(registry);
  test_math_not_handled(registry);
  test_math_faults(registry);
  test_runtime_error_registry();

  std::cout << "stdlib registry tests passed\n";
  return 0;
}
