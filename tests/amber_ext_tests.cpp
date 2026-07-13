// Unit tests for the native-extension ABI (runtime/amber_ext.{h,cpp}) and its
// in-tree driver surface (runtime/amber_ext_runtime.h), 5c-ii. An AmberCtx is
// exercised over a recording StdlibHost -- the marshalling logic (arena, handle
// encoding, predicates, readers, builders, foreign-handle construction +
// tombstone) is what is under test, so a full VM frame is not required; the
// frame-dependent paths (fault recording) are routed to the recording host.

#include "runtime/amber_ext.h"
#include "runtime/amber_ext_runtime.h"
#include "runtime/io.h"
#include "runtime/stdlib_registry.h"
#include "runtime/world.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using amber::runtime::NativeTagRegistry;
using amber::runtime::NativeTypeDescriptor;
using amber::runtime::NativeExtErrorDescriptor;
using amber::runtime::NativeExtRegistry;
using amber::runtime::RuntimeBytes;
using amber::runtime::RuntimeDispatchRegistry;
using amber::runtime::RuntimeErrorRegistry;
using amber::runtime::RuntimeForeignHandle;
using amber::runtime::RuntimeNativePackageDescriptor;
using amber::runtime::RuntimeTypeRegistry;
using amber::runtime::RuntimeWorld;
using amber::runtime::SendStatus;
using amber::runtime::StdlibHost;
using amber::runtime::Value;

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "amber_ext test failed: " << message << "\n";
    std::exit(1);
  }
}

// Records faults and answers the few host calls the ABI shims actually make
// (string interning, immutable bytes + zero-copy view). Everything else is a
// stub: the ABI round-trip never reaches it.
struct RecordingHost : StdlibHost {
  bool faulted = false;
  std::string fault_class;
  std::string fault_message;
  std::vector<std::string> strings;

  void stdlib_set_fault(const void * /*frame*/, const std::string &error_class,
                        const std::string &message) override {
    faulted = true;
    fault_class = error_class;
    fault_message = message;
  }
  void stdlib_raise_exception(const void * /*frame*/,
                              Value /*exception*/) override {}
  void stdlib_raise_runtime_error(const void * /*frame*/,
                                  const std::string &error_class,
                                  const std::string &message) override {
    // The ABI's amber_fault / amber_handle_ptr now raise rescuable errors; the
    // recording host treats them like a recorded fault for assertions.
    faulted = true;
    fault_class = error_class;
    fault_message = message;
  }
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
  Value stdlib_string_value_from_text(std::string text) override {
    const std::uint32_t id = static_cast<std::uint32_t>(strings.size());
    strings.push_back(std::move(text));
    return Value::string(id);
  }
  Value stdlib_symbol_value_from_text(std::string /*text*/) override {
    return Value::null();
  }
  std::optional<std::string> stdlib_text_of(const Value &value) override {
    if (value.is_string() && value.as_string().string_id < strings.size()) {
      return strings[value.as_string().string_id];
    }
    return std::nullopt;
  }
  std::string stdlib_display_string(const void * /*frame*/,
                                    const Value &value) override {
    if (const std::optional<std::string> text = stdlib_text_of(value)) {
      return *text;
    }
    return "";
  }
  std::optional<std::string> stdlib_bytes_of(const void * /*frame*/,
                                             const Value & /*value*/) override {
    return std::nullopt;
  }
  Value stdlib_bytes_value_from_bytes(std::string bytes) override {
    return Value::io_value(std::make_shared<RuntimeBytes>(std::move(bytes)));
  }
  bool stdlib_bytes_view(const void * /*frame*/, const Value &value,
                         const std::uint8_t **ptr, std::size_t *len,
                         Value *keepalive) override {
    if (!value.is_io_value()) {
      return false;
    }
    const auto bytes =
        std::dynamic_pointer_cast<RuntimeBytes>(value.as_io_value());
    if (bytes == nullptr) {
      return false;
    }
    const std::string &storage = bytes->string();
    *ptr = reinterpret_cast<const std::uint8_t *>(storage.data());
    *len = storage.size();
    *keepalive = value;
    return true;
  }
  Value stdlib_make_list(std::vector<Value> /*items*/) override {
    return Value::null();
  }
  Value stdlib_make_tuple(std::vector<Value> /*items*/) override {
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
  bool stdlib_block_suspension_in_property_arm(
      const void * /*frame*/, const std::string & /*context*/) override {
    return false;
  }
  void stdlib_throw_json_stop(const void * /*frame*/,
                              std::optional<Value> /*value*/) override {}
  bool
  stdlib_integer_range(const void * /*frame*/, const Value & /*value*/,
                       amber::runtime::StdlibIntegerRange * /*out*/) override {
    return false;
  }
  bool stdlib_fs_exists(const void * /*frame*/, const std::string & /*path*/,
                        bool * /*out*/) override {
    return false;
  }
  bool stdlib_fs_file(const void * /*frame*/, const std::string & /*path*/,
                      bool * /*out*/) override {
    return false;
  }
  bool stdlib_fs_dir(const void * /*frame*/, const std::string & /*path*/,
                     bool * /*out*/) override {
    return false;
  }
  bool stdlib_fs_metadata(const void * /*frame*/, const std::string & /*path*/,
                          Value * /*out*/) override {
    return false;
  }
  bool stdlib_fs_read_bytes_limited(const void * /*frame*/,
                                    const std::string & /*path*/,
                                    std::optional<std::size_t> /*limit*/,
                                    std::string * /*out*/) override {
    return false;
  }
  bool stdlib_fs_read_text_limited(const void * /*frame*/,
                                   const std::string & /*path*/,
                                   std::optional<std::size_t> /*limit*/,
                                   std::string * /*out*/) override {
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
  bool stdlib_fs_write_bytes_value(const void * /*frame*/,
                                   const std::string & /*path*/,
                                   const std::string & /*bytes*/,
                                   bool /*create*/,
                                   bool /*truncate*/) override {
    return false;
  }
  bool stdlib_fs_write_text_value(const void * /*frame*/,
                                  const std::string & /*path*/,
                                  const std::string & /*text*/, bool /*create*/,
                                  bool /*truncate*/) override {
    return false;
  }
  bool stdlib_fs_mkdir(const void * /*frame*/,
                       const std::string & /*path*/) override {
    return false;
  }
  bool stdlib_fs_mkdir_p(const void * /*frame*/,
                         const std::string & /*path*/) override {
    return false;
  }
  bool stdlib_fs_remove(const void * /*frame*/,
                        const std::string & /*path*/) override {
    return false;
  }
  bool stdlib_fs_rename(const void * /*frame*/, const std::string & /*from*/,
                        const std::string & /*to*/) override {
    return false;
  }
  bool stdlib_fs_copy(const void * /*frame*/, const std::string & /*from*/,
                      const std::string & /*to*/,
                      std::size_t * /*count*/) override {
    return false;
  }
  bool stdlib_fs_open_file(const void * /*frame*/, const std::string & /*path*/,
                           amber::runtime::RuntimeFileMode /*mode*/,
                           amber::runtime::RuntimeFileOpenOptions /*options*/,
                           amber::runtime::RuntimeIsolationMode /*isolation*/,
                           Value * /*out*/) override {
    return false;
  }
  bool stdlib_fs_close_file(const void * /*frame*/, const Value & /*file*/,
                            bool /*report_fault*/) override {
    return false;
  }
  bool stdlib_net_udp_bind(const void * /*frame*/,
                           const amber::runtime::RuntimeEndpoint & /*endpoint*/,
                           amber::runtime::RuntimeIsolationMode /*isolation*/,
                           Value * /*out*/) override {
    return false;
  }
  bool stdlib_net_udp_open(const void * /*frame*/,
                           const std::string & /*family*/,
                           amber::runtime::RuntimeIsolationMode /*isolation*/,
                           Value * /*out*/) override {
    return false;
  }
  bool
  stdlib_net_tcp_connect(const void * /*frame*/,
                         const amber::runtime::RuntimeEndpoint & /*endpoint*/,
                         std::chrono::milliseconds /*timeout*/,
                         amber::runtime::RuntimeIsolationMode /*isolation*/,
                         Value * /*out*/) override {
    return false;
  }
  bool
  stdlib_net_tcp_listen(const void * /*frame*/,
                        const amber::runtime::RuntimeEndpoint & /*endpoint*/,
                        int /*backlog*/, bool /*reuse_addr*/,
                        amber::runtime::RuntimeIsolationMode /*isolation*/,
                        Value * /*out*/) override {
    return false;
  }
  bool stdlib_net_tcp_close(const void * /*frame*/, const Value & /*resource*/,
                            bool /*report_fault*/) override {
    return false;
  }
  SendStatus stdlib_vm_io_value_intrinsic_send(
      const void * /*frame*/, const Value & /*receiver*/,
      const std::string & /*selector*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_construct_client(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_construct_request(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_construct_headers(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_construct_server(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_construct_server_response(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_construct_form_body(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_json_get(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_json_post(
      const void * /*frame*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_request_body_type_send(
      const void * /*frame*/, const std::string & /*selector*/,
      const std::vector<Value> & /*args*/, const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_net_http_server_response_type_send(
      const void * /*frame*/, const std::string & /*selector*/,
      const std::vector<Value> & /*args*/, const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
  }
  SendStatus stdlib_vm_task_intrinsic_send(
      const void * /*frame*/, const Value & /*receiver*/,
      const std::string & /*selector*/, const std::vector<Value> & /*args*/,
      const Value & /*block*/,
      const std::vector<std::pair<std::uint32_t, Value>> & /*kw_args*/,
      Value * /*out*/) override {
    return SendStatus::NotHandled;
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

// File-scope sinks so the descriptor callbacks can be plain function pointers
// (matching the C ABI signatures, which cannot capture).
int g_owned_destroyed = 0;
void *g_owned_ctx = nullptr;
void *g_owned_ptr = nullptr;
void owned_destructor(void *ctx, void *handle) {
  ++g_owned_destroyed;
  g_owned_ctx = ctx;
  g_owned_ptr = handle;
}

void test_amber_ext_scalar_round_trip() {
  RecordingHost host;
  NativeTagRegistry tags;
  AmberCtx *cx = amber::runtime::amber_ext_ctx_open(host, nullptr, tags);

  const AmberValue null_v = amber_make_null(cx);
  expect(amber_is_null(cx, null_v) == 1, "null builds and reads as null");

  const AmberValue bool_v = amber_make_bool(cx, 1);
  int bool_out = 0;
  expect(amber_is_bool(cx, bool_v) == 1, "bool predicate");
  expect(amber_as_bool(cx, bool_v, &bool_out) == 1 && bool_out == 1,
         "bool round-trips");

  const AmberValue int_v = amber_make_int(cx, -42);
  std::int64_t int_out = 0;
  expect(amber_is_int(cx, int_v) == 1, "int predicate");
  expect(amber_as_int(cx, int_v, &int_out) == 1 && int_out == -42,
         "int round-trips");
  int wrong = 0;
  expect(amber_as_bool(cx, int_v, &wrong) == 0,
         "reading an int as bool reports wrong kind without a fault");
  expect(!host.faulted, "wrong-kind reader sets no fault");

  const AmberValue float_v = amber_make_float(cx, 2.5);
  double float_out = 0.0;
  expect(amber_is_float(cx, float_v) == 1, "float predicate");
  expect(amber_as_float(cx, float_v, &float_out) == 1 && float_out == 2.5,
         "float round-trips");

  amber::runtime::amber_ext_ctx_close(cx);
}

void test_amber_ext_str_and_bytes_round_trip() {
  RecordingHost host;
  NativeTagRegistry tags;
  AmberCtx *cx = amber::runtime::amber_ext_ctx_open(host, nullptr, tags);

  const std::string text = "blake3";
  const AmberValue str_v = amber_make_str(cx, text.data(), text.size());
  expect(amber_is_str(cx, str_v) == 1, "str predicate");
  const char *str_ptr = nullptr;
  std::size_t str_len = 0;
  expect(amber_str_view(cx, str_v, &str_ptr, &str_len) == 1,
         "str view succeeds");
  expect(str_len == text.size() &&
             std::memcmp(str_ptr, text.data(), text.size()) == 0,
         "str view round-trips the bytes");

  const std::string blob = std::string("\x00\x01\xfe\xff", 4);
  const AmberValue bytes_v = amber_make_bytes(
      cx, reinterpret_cast<const std::uint8_t *>(blob.data()), blob.size());
  expect(amber_is_bytes(cx, bytes_v) == 1, "bytes predicate");
  expect(amber_is_str(cx, bytes_v) == 0, "bytes is not a str");
  const std::uint8_t *bytes_ptr = nullptr;
  std::size_t bytes_len = 0;
  expect(amber_bytes_view(cx, bytes_v, &bytes_ptr, &bytes_len) == 1,
         "bytes view succeeds");
  expect(bytes_len == blob.size() &&
             std::memcmp(bytes_ptr, blob.data(), blob.size()) == 0,
         "bytes view is zero-copy and exact");
  expect(amber_bytes_view(cx, str_v, &bytes_ptr, &bytes_len) == 0,
         "bytes view rejects a non-bytes value");
  expect(!host.faulted, "a wrong-kind bytes view sets no fault");

  amber::runtime::amber_ext_ctx_close(cx);
}

void test_amber_ext_handle_lifecycle() {
  g_owned_destroyed = 0;
  g_owned_ctx = nullptr;
  g_owned_ptr = nullptr;

  RecordingHost host;
  NativeTagRegistry tags;
  NativeTypeDescriptor owned;
  owned.tag = "blake3.Hasher";
  owned.ownership = RuntimeForeignHandle::Ownership::Owned;
  owned.owned_destructor = &owned_destructor;
  tags.register_type(owned);

  AmberCtx *cx = amber::runtime::amber_ext_ctx_open(host, nullptr, tags);

  int resource = 99;
  const AmberValue handle_v = amber_make_handle(cx, "blake3.Hasher", &resource);
  expect(!host.faulted, "make_handle with a known tag does not fault");
  expect(amber_is_handle(cx, handle_v) == 1, "handle predicate");

  void *recovered = nullptr;
  expect(amber_handle_ptr(cx, handle_v, "blake3.Hasher", &recovered) == 1 &&
             recovered == &resource,
         "handle_ptr recovers the foreign pointer for a live, matching tag");

  void *mismatch = nullptr;
  expect(amber_handle_ptr(cx, handle_v, "other.Tag", &mismatch) == 0,
         "handle_ptr rejects a tag mismatch");
  expect(host.faulted && host.fault_class == "TypeError",
         "tag mismatch records a TypeError");
  host.faulted = false;

  // Deterministic destroy! threads the destroy-time ctx into the destructor.
  Value handle_value = amber::runtime::amber_ext_ctx_export(cx, handle_v);
  expect(handle_value.is_foreign_handle(),
         "exported value is a foreign handle");
  int destroy_ctx_marker = 0;
  expect(handle_value.as_foreign_handle()->destroy(&destroy_ctx_marker),
         "owned destroy! reports it ran");
  expect(g_owned_destroyed == 1 && g_owned_ctx == &destroy_ctx_marker &&
             g_owned_ptr == &resource,
         "owned destructor runs once with the destroy-time ctx and resource");

  // Use-after-destroy! is a tombstoned LifetimeError.
  void *dead = nullptr;
  expect(amber_handle_ptr(cx, handle_v, "blake3.Hasher", &dead) == 0,
         "handle_ptr fails after destroy!");
  expect(host.faulted && host.fault_class == "LifetimeError",
         "use-after-destroy! records a LifetimeError");

  amber::runtime::amber_ext_ctx_close(cx);
}

void test_amber_ext_make_handle_unknown_tag() {
  RecordingHost host;
  NativeTagRegistry tags; // empty
  AmberCtx *cx = amber::runtime::amber_ext_ctx_open(host, nullptr, tags);

  int resource = 0;
  const AmberValue handle_v = amber_make_handle(cx, "missing.Tag", &resource);
  expect(host.faulted && host.fault_class == "TypeError",
         "an unknown tag records a TypeError");
  expect(amber_is_null(cx, handle_v) == 1,
         "an unknown tag yields a null handle value");

  amber::runtime::amber_ext_ctx_close(cx);
}

void test_amber_ext_fault_and_version() {
  RecordingHost host;
  NativeTagRegistry tags;
  AmberCtx *cx = amber::runtime::amber_ext_ctx_open(host, nullptr, tags);

  const AmberStatus status = amber_fault(cx, "ValueError", "bad input");
  expect(status == AMBER_ERR, "amber_fault returns AMBER_ERR");
  expect(host.faulted && host.fault_class == "ValueError" &&
             host.fault_message == "bad input",
         "amber_fault records the named error class and message");

  expect(amber_ext_abi_version() == AMBER_EXT_ABI_VERSION,
         "ABI version handshake matches");

  amber::runtime::amber_ext_ctx_close(cx);
}

void test_native_extension_type_import() {
  using Ownership = RuntimeForeignHandle::Ownership;

  NativeExtRegistry extension_registry;
  NativeTypeDescriptor owned;
  owned.tag = "pkg.Handle";
  owned.ownership = Ownership::Owned;
  owned.owned_destructor = &owned_destructor;
  extension_registry.register_type(owned);

  RuntimeTypeRegistry types;
  extension_registry.register_types(types);

  const NativeTypeDescriptor *lookup =
      types.native_package_tags().lookup("pkg.Handle");
  expect(lookup != nullptr && lookup->ownership == Ownership::Owned &&
             lookup->owned_destructor == &owned_destructor,
         "native extension types import into RuntimeTypeRegistry");
  expect(types.native_package_tags().lookup("missing.Handle") == nullptr,
         "imported runtime type tags preserve unknown-tag misses");
}

void test_native_extension_thunk_import() {
  NativeExtRegistry extension_registry;
  int marker = 0;
  extension_registry.register_thunk("pkg.fn", &marker);

  RuntimeDispatchRegistry dispatch;
  extension_registry.register_thunks(dispatch);

  expect(dispatch.native_package_thunk("pkg.fn") == &marker,
         "native extension thunks import into RuntimeDispatchRegistry");
  expect(dispatch.native_package_thunk("pkg.missing") == nullptr,
         "imported native extension thunk map preserves misses");
}

void test_native_extension_runtime_contributions() {
  using Ownership = RuntimeForeignHandle::Ownership;

  NativeExtRegistry extension_registry;
  RuntimeNativePackageDescriptor package;
  int marker = 0;
  package.thunks.push_back({"pkg.fn", &marker});
  package.code_bindings.push_back({7, false, "pkg.fn"});
  package.method_bindings.push_back({"pkg.Handle", "bump!", "pkg.fn"});

  NativeTypeDescriptor owned;
  owned.tag = "pkg.Handle";
  owned.ownership = Ownership::Owned;
  owned.owned_destructor = &owned_destructor;
  package.types.push_back(owned);

  NativeExtErrorDescriptor error;
  error.name = "Pkg.NativeLeafError";
  error.default_message = "package failed";
  error.default_exit_code = 23;
  package.errors.push_back(error);
  extension_registry.register_package(std::move(package));

  RuntimeDispatchRegistry dispatch;
  RuntimeTypeRegistry types;
  RuntimeErrorRegistry errors;
  extension_registry.register_runtime_contributions(dispatch, types, errors);

  expect(dispatch.native_package_thunk("pkg.fn") == &marker,
         "native extension contributor imports thunks");
  const auto *code_binding = dispatch.native_package_code_binding(7);
  expect(code_binding != nullptr && !code_binding->method &&
             code_binding->logical == "pkg.fn",
         "native extension contributor imports code bindings");
  const std::string *method_binding =
      dispatch.native_package_method_binding("pkg.Handle", "bump!");
  expect(method_binding != nullptr && *method_binding == "pkg.fn",
         "native extension contributor imports method bindings");

  const NativeTypeDescriptor *lookup =
      types.native_package_tags().lookup("pkg.Handle");
  expect(lookup != nullptr && lookup->ownership == Ownership::Owned &&
             lookup->owned_destructor == &owned_destructor,
         "native extension contributor imports types");

  const auto native_error = errors.error_id("NativeError");
  const auto package_error = errors.error_id("Pkg.NativeLeafError");
  expect(native_error.has_value() && package_error.has_value() &&
             errors.error_is_a(*package_error, *native_error),
         "native extension contributor imports default-parent errors");
  expect(errors.error_default_exit_code(*package_error).has_value() &&
             *errors.error_default_exit_code(*package_error) == 23,
         "native extension contributor imports error exit codes");
  expect(std::string(errors.error_default_message(*package_error)) ==
             "package failed",
         "native extension contributor imports error messages");
}

AmberStatus direct_increment(AmberCtx *cx, const AmberValue *args,
                             std::size_t argc, AmberValue *out) {
  std::int64_t value = 0;
  if (argc != 1U || !amber_as_int(cx, args[0], &value)) {
    return amber_fault(cx, "TypeError", "increment expects one Int");
  }
  *out = amber_make_int(cx, value + 1);
  return AMBER_OK;
}

void test_runtime_world_direct_native_extension_call() {
  amber::bytecode::BcModule module;
  module.strings = {"amber.native.bind:7", "F:test.direct_increment"};
  module.attrs.push_back({0, 1});
  amber::bytecode::BcCode code;
  code.code_id = 7;
  code.reg_count = 1;
  module.code_objects.push_back(std::move(code));
  amber::bytecode::BcCode unbound_code;
  unbound_code.code_id = 8;
  module.code_objects.push_back(std::move(unbound_code));

  RuntimeNativePackageDescriptor package;
  package.thunks.push_back(
      {"test.direct_increment", reinterpret_cast<void *>(&direct_increment)});
  NativeExtRegistry::global().register_package(std::move(package));

  RuntimeWorld world(module);
  const amber::runtime::ExecutionResult result =
      world.invoke_native_extension(7, {Value::integer(41)});
  expect(result.ok() && result.value.is_integer() &&
             result.value.as_integer() == 42,
         "RuntimeWorld invokes a native extension thunk directly");

  const amber::runtime::ExecutionResult missing =
      world.invoke_native_extension(8);
  expect(!missing.ok() && missing.fault.has_value() &&
             missing.fault->error_name == "NativeRequiredError",
         "direct native extension invocation rejects an unbound code id");
}

} // namespace

int main() {
  test_amber_ext_scalar_round_trip();
  test_amber_ext_str_and_bytes_round_trip();
  test_amber_ext_handle_lifecycle();
  test_amber_ext_make_handle_unknown_tag();
  test_amber_ext_fault_and_version();
  test_native_extension_type_import();
  test_native_extension_thunk_import();
  test_native_extension_runtime_contributions();
  test_runtime_world_direct_native_extension_call();
  std::cout << "amber_ext_tests: ok\n";
  return 0;
}
