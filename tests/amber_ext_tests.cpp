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
using amber::runtime::RuntimeBytes;
using amber::runtime::RuntimeForeignHandle;
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
  bool stdlib_fs_write_text(const void * /*frame*/, const std::string & /*path*/,
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
  expect(handle_value.is_foreign_handle(), "exported value is a foreign handle");
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

} // namespace

int main() {
  test_amber_ext_scalar_round_trip();
  test_amber_ext_str_and_bytes_round_trip();
  test_amber_ext_handle_lifecycle();
  test_amber_ext_make_handle_unknown_tag();
  test_amber_ext_fault_and_version();
  std::cout << "amber_ext_tests: ok\n";
  return 0;
}
