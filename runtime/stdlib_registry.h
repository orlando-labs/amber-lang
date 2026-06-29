#pragma once

// Layer 0 stdlib substrate (DESIGN-stdlib-next-libs-order-2026-06-15 §4.0).
//
// A native stdlib library is reached through two hand-written `if`-chains in
// the 1.1 MB `runtime/vm.cpp`: dispatch (`try_apply_native_stdlib_send`) and
// name resolution (`lookup_native_prelude_constant`). This header is the seam
// that lets a library land as a self-contained `runtime/stdlib_<name>.{h,cpp}`
// translation unit registered through a table, instead of more branches.
//
// The ABI deliberately names no `vm.cpp`-internal type. `Value` and
// `RuntimeNativeTypeKind` come from the public runtime header; the active call
// frame is type-erased to `const void *` (the host casts it back), so a stdlib
// translation unit never has to see `Frame`, `Vm`, or their headers.

#include "runtime/io.h"
#include "runtime/vm.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace amber::runtime {

// Outcome of a SEND attempt. `NotHandled` means "not mine, keep looking" and is
// what lets a registered handler coexist with the legacy inline chain: an
// unknown selector for a migrated kind returns `NotHandled` and falls through.
enum class SendStatus { Matched, NotHandled, Faulted };

enum class StdlibBlockStatus { Returned, Stopped, Raised, Faulted };

struct StdlibBlockResult {
  StdlibBlockStatus status = StdlibBlockStatus::Faulted;
  Value value = Value::null();
  Value exception = Value::null();
  bool stop_value_present = false;
};

struct StdlibIntegerRange {
  std::int64_t start = 0;
  std::int64_t step = 1;
  std::uint64_t count = 0;
};

// The narrow runtime facade a stdlib handler is allowed to touch. Implemented
// by the VM; every method that needs the active frame takes it type-erased so
// this interface stays free of `vm.cpp`-internal types.
class StdlibHost {
public:
  virtual ~StdlibHost() = default;

  // Record a terminal fault on the active frame (the VM's `set_fault`).
  virtual void stdlib_set_fault(const void *frame,
                                const std::string &error_class,
                                const std::string &message) = 0;

  // Raise an existing exception value through ordinary VM unwinding.
  virtual void stdlib_raise_exception(const void *frame, Value exception) = 0;

  // Raise a builtin runtime error by class name as a *rescuable* exception that
  // unwinds to the nearest handler (degrading to a terminal fault only when the
  // name is unknown or no handler exists). This is the rescuable counterpart to
  // stdlib_set_fault, used by the native-extension ABI's amber_fault so a thunk
  // fault is catchable by `rescue` (native-packages design §6).
  virtual void stdlib_raise_runtime_error(const void *frame,
                                          const std::string &error_class,
                                          const std::string &message) = 0;

  // Write CLI-facing output through the runtime's logical stdout/stderr.
  virtual bool stdlib_write_output(const void *frame, bool stderr_stream,
                                   const std::string &text) = 0;

  // Look up a keyword argument by name; `nullopt` when absent.
  virtual std::optional<Value> stdlib_keyword_arg_value(
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      const std::string &name) = 0;

  // Fault unless every supplied keyword is in `allowed`. Returns false (and
  // sets the fault) on the first stray keyword.
  virtual bool stdlib_reject_unknown_keywords(
      const void *frame,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      std::initializer_list<const char *> allowed) = 0;

  // Intern `text` into the module string table and wrap it as a String value.
  virtual Value stdlib_string_value_from_text(std::string text) = 0;

  // Intern `text` into the module symbol table and wrap it as a Symbol value.
  virtual Value stdlib_symbol_value_from_text(std::string text) = 0;

  // --- value introspection (used by generators) -----------------------------
  // The text of a String or Symbol value (a Symbol yields its name); nullopt
  // for any other kind. Lets a stdlib unit read string/key text without
  // touching the module string/symbol tables directly.
  virtual std::optional<std::string> stdlib_text_of(const Value &value) = 0;

  // Raw immutable byte extraction/construction for pure binary codecs. This is
  // intentionally narrower than `Bytes.new`: String is not accepted here, so
  // codec encoders must be fed an explicit Bytes/ByteSlice/ByteBuffer value.
  virtual std::optional<std::string> stdlib_bytes_of(const void *frame,
                                                     const Value &value) = 0;
  virtual Value stdlib_bytes_value_from_bytes(std::string bytes) = 0;

  // Borrowed zero-copy view of an immutable byte sequence (Bytes/ByteSlice/
  // ByteBuffer). On success sets `*ptr`/`*len` to storage that stays valid for
  // as long as `*keepalive` is retained, and returns true. For an
  // already-immutable input the keepalive is the input value; for a ByteBuffer
  // it is a frozen snapshot. Faults and returns false on the wrong kind. This
  // is the zero-copy counterpart to `stdlib_bytes_of`, backing the ABI's
  // `amber_bytes_view` (native-packages design §6); the borrowed view is the
  // RuntimePinViewKind::ValueBuffer model -- a pointer into runtime-owned
  // buffer storage that the caller must not outlive the keepalive.
  virtual bool stdlib_bytes_view(const void *frame, const Value &value,
                                 const std::uint8_t **ptr, std::size_t *len,
                                 Value *keepalive) = 0;

  // --- value construction (used by parsers) ----------------------------------
  // Build a List value from `items`.
  virtual Value stdlib_make_list(std::vector<Value> items) = 0;

  // Build a Tuple value from `items`.
  virtual Value stdlib_make_tuple(std::vector<Value> items) = 0;

  // Build a map from string-keyed entries. `strict=false` yields an ordinary
  // name-indifferent Map (keys stored as Str, canonical identity interned);
  // `strict=true` yields a StrictMap. Entry order is preserved; later duplicate
  // keys overwrite earlier ones per the map's strictness.
  virtual Value
  stdlib_make_object(std::vector<std::pair<std::string, Value>> entries,
                     bool strict) = 0;

  // Extract ordinary structured values without exposing MapValue/ListValue to
  // stdlib translation units. Map keys must be Str/Symbol and are returned by
  // text; values preserve their original Value.
  virtual bool stdlib_string_keyed_entries(
      const void *frame, const Value &value,
      std::vector<std::pair<std::string, Value>> *out) = 0;
  virtual bool stdlib_lookup_string_key(const void *frame, const Value &value,
                                        const std::string &key, Value *out,
                                        bool *found) = 0;
  virtual bool stdlib_map_values(const void *frame, const Value &value,
                                 std::vector<Value> *out) = 0;
  virtual bool stdlib_list_items(const void *frame, const Value &value,
                                 std::vector<Value> *out) = 0;
  virtual bool stdlib_sequence_items(const void *frame, const Value &value,
                                     std::vector<Value> *out) = 0;

  // Invoke the block supplied to a streaming stdlib method with one value. A
  // `Json.stop` escaped throw is reported as Stopped; other throws/exceptions
  // are re-entered into the parent VM and reported as Faulted.
  virtual StdlibBlockResult stdlib_call_stream_block(const void *frame,
                                                     const Value &block,
                                                     Value value) = 0;
  virtual StdlibBlockResult stdlib_call_path_block(const void *frame,
                                                   const Value &block,
                                                   Value value,
                                                   Value accumulator) = 0;
  virtual StdlibBlockResult stdlib_call_block(const void *frame,
                                              const Value &block,
                                              std::vector<Value> args) = 0;

  // Raise the host-owned non-local stop used by Json stream/path APIs.
  virtual void stdlib_throw_json_stop(const void *frame,
                                      std::optional<Value> value) = 0;

  // Extract a bounded integer range descriptor without exposing the VM's Range
  // representation. Faults and returns false for non-ranges, float/open/empty
  // ranges, or ranges too large to sample with a uint64 ordinal.
  virtual bool stdlib_integer_range(const void *frame, const Value &value,
                                    StdlibIntegerRange *out) = 0;

  // File access routed through the same runtime IO policy/provider layer as fs.
  virtual bool stdlib_fs_exists(const void *frame, const std::string &path,
                                bool *out) = 0;
  virtual bool stdlib_fs_file(const void *frame, const std::string &path,
                              bool *out) = 0;
  virtual bool stdlib_fs_dir(const void *frame, const std::string &path,
                             bool *out) = 0;
  virtual bool stdlib_fs_metadata(const void *frame, const std::string &path,
                                  Value *out) = 0;
  virtual bool stdlib_fs_read_bytes_limited(const void *frame,
                                            const std::string &path,
                                            std::optional<std::size_t> limit,
                                            std::string *out) = 0;
  virtual bool stdlib_fs_read_text_limited(const void *frame,
                                           const std::string &path,
                                           std::optional<std::size_t> limit,
                                           std::string *out) = 0;
  virtual bool stdlib_fs_read_text(const void *frame, const std::string &path,
                                   std::string *out) = 0;
  virtual bool stdlib_fs_write_text(const void *frame, const std::string &path,
                                    const std::string &text) = 0;
  virtual bool stdlib_fs_write_bytes_value(const void *frame,
                                           const std::string &path,
                                           const std::string &bytes,
                                           bool create, bool truncate) = 0;
  virtual bool stdlib_fs_write_text_value(const void *frame,
                                          const std::string &path,
                                          const std::string &text, bool create,
                                          bool truncate) = 0;
  virtual bool stdlib_fs_mkdir(const void *frame, const std::string &path) = 0;
  virtual bool stdlib_fs_mkdir_p(const void *frame,
                                 const std::string &path) = 0;
  virtual bool stdlib_fs_remove(const void *frame, const std::string &path) = 0;
  virtual bool stdlib_fs_rename(const void *frame, const std::string &from,
                                const std::string &to) = 0;
  virtual bool stdlib_fs_copy(const void *frame, const std::string &from,
                              const std::string &to, std::size_t *count) = 0;
  virtual bool stdlib_fs_open_file(const void *frame, const std::string &path,
                                   RuntimeFileMode mode,
                                   RuntimeFileOpenOptions options,
                                   RuntimeIsolationMode isolation,
                                   Value *out) = 0;
  virtual bool stdlib_fs_close_file(const void *frame, const Value &file,
                                    bool report_fault) = 0;
  virtual bool stdlib_net_udp_bind(const void *frame,
                                   const RuntimeEndpoint &endpoint,
                                   RuntimeIsolationMode isolation,
                                   Value *out) = 0;
  virtual bool stdlib_net_udp_open(const void *frame, const std::string &family,
                                   RuntimeIsolationMode isolation,
                                   Value *out) = 0;
  virtual bool stdlib_net_tcp_connect(const void *frame,
                                      const RuntimeEndpoint &endpoint,
                                      std::chrono::milliseconds timeout,
                                      RuntimeIsolationMode isolation,
                                      Value *out) = 0;
  virtual bool stdlib_net_tcp_listen(const void *frame,
                                     const RuntimeEndpoint &endpoint,
                                     int backlog, bool reuse_addr,
                                     RuntimeIsolationMode isolation,
                                     Value *out) = 0;
  virtual bool stdlib_net_tcp_close(const void *frame, const Value &resource,
                                    bool report_fault) = 0;
  virtual SendStatus stdlib_net_http_construct_client(
      const void *frame, const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      Value *out) = 0;
  virtual SendStatus stdlib_net_http_construct_request(
      const void *frame, const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      Value *out) = 0;
  virtual SendStatus stdlib_net_http_construct_headers(
      const void *frame, const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      Value *out) = 0;
  virtual SendStatus stdlib_net_http_construct_server(
      const void *frame, const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      Value *out) = 0;
  virtual SendStatus stdlib_net_http_construct_server_response(
      const void *frame, const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      Value *out) = 0;
  virtual SendStatus stdlib_net_http_construct_form_body(
      const void *frame, const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      Value *out) = 0;
  virtual SendStatus stdlib_net_http_json_get(
      const void *frame, const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      Value *out) = 0;
  virtual SendStatus stdlib_net_http_json_post(
      const void *frame, const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      Value *out) = 0;
  virtual SendStatus stdlib_net_http_request_body_type_send(
      const void *frame, const std::string &selector,
      const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      Value *out) = 0;
  virtual SendStatus stdlib_net_http_server_response_type_send(
      const void *frame, const std::string &selector,
      const std::vector<Value> &args, const Value &block,
      const std::vector<std::pair<std::uint32_t, Value>> &kw_args,
      Value *out) = 0;

  // OS-backed cryptographic entropy. The VM owns capability/effect/replay
  // policy for this host resource; stdlib handlers only request byte counts.
  virtual bool stdlib_secure_random_bytes(const void *frame, std::size_t count,
                                          std::string *out) = 0;

  // Host clocks. Wall time is a UTC Unix instant; monotonic time is a signed
  // period from an opaque steady-clock origin.
  virtual std::optional<RuntimeTimeValue>
  stdlib_wall_time_now(const void *frame) = 0;
  virtual std::optional<RuntimeTimePeriodValue>
  stdlib_monotonic_time(const void *frame) = 0;
};

// One SEND, packaged as a single context object instead of seven positional
// parameters. References borrow the caller's storage for the duration of the
// dispatch; `out` receives the result on `Matched`.
struct NativeStdlibCall {
  StdlibHost &host;
  const void *frame;
  const Value &receiver;
  RuntimeNativeTypeKind kind;
  const std::string &selector;
  const std::vector<Value> &args;
  const Value &block;
  const std::vector<std::pair<std::uint32_t, Value>> &kw_args;
  Value *out;

  // ---- guard / helper facade (the inline guards used in vm.cpp today) -------

  // Set `error_class`/`message` and report `Faulted` in one expression.
  SendStatus fault(const std::string &error_class,
                   const std::string &message) const {
    host.stdlib_set_fault(frame, error_class, message);
    return SendStatus::Faulted;
  }

  SendStatus raise(Value exception) const {
    host.stdlib_raise_exception(frame, std::move(exception));
    return SendStatus::Faulted;
  }

  bool write_stdout(const std::string &text) const {
    return host.stdlib_write_output(frame, false, text);
  }

  bool write_stderr(const std::string &text) const {
    return host.stdlib_write_output(frame, true, text);
  }

  bool require_arity(std::size_t expected) const {
    if (args.size() != expected) {
      host.stdlib_set_fault(frame, "TypeError",
                            "wrong native stdlib SEND arity");
      return false;
    }
    return true;
  }

  bool require_no_block() const {
    if (!block.is_null()) {
      host.stdlib_set_fault(
          frame, "TypeError",
          "native stdlib selector does not accept block arguments");
      return false;
    }
    return true;
  }

  std::optional<Value> keyword(const std::string &name) const {
    return host.stdlib_keyword_arg_value(kw_args, name);
  }

  bool
  reject_unknown_keywords(std::initializer_list<const char *> allowed) const {
    return host.stdlib_reject_unknown_keywords(frame, kw_args, allowed);
  }

  bool bool_keyword(const std::string &name, bool fallback, bool *value) const {
    const std::optional<Value> argument = keyword(name);
    if (!argument.has_value()) {
      *value = fallback;
      return true;
    }
    if (!argument->is_bool()) {
      host.stdlib_set_fault(frame, "TypeError", name + " must be Bool");
      return false;
    }
    *value = argument->as_bool();
    return true;
  }

  Value string_value(std::string text) const {
    return host.stdlib_string_value_from_text(std::move(text));
  }

  Value symbol_value(std::string text) const {
    return host.stdlib_symbol_value_from_text(std::move(text));
  }

  std::optional<std::string> text_of(const Value &value) const {
    return host.stdlib_text_of(value);
  }

  std::optional<std::string> bytes_of(const Value &value) const {
    return host.stdlib_bytes_of(frame, value);
  }

  bool bytes_view(const Value &value, const std::uint8_t **ptr,
                  std::size_t *len, Value *keepalive) const {
    return host.stdlib_bytes_view(frame, value, ptr, len, keepalive);
  }

  Value bytes_value(std::string bytes) const {
    return host.stdlib_bytes_value_from_bytes(std::move(bytes));
  }

  Value make_list(std::vector<Value> items) const {
    return host.stdlib_make_list(std::move(items));
  }

  Value make_tuple(std::vector<Value> items) const {
    return host.stdlib_make_tuple(std::move(items));
  }

  Value make_object(std::vector<std::pair<std::string, Value>> entries,
                    bool strict = false) const {
    return host.stdlib_make_object(std::move(entries), strict);
  }

  bool
  string_keyed_entries(const Value &value,
                       std::vector<std::pair<std::string, Value>> *out) const {
    return host.stdlib_string_keyed_entries(frame, value, out);
  }

  bool lookup_string_key(const Value &value, const std::string &key, Value *out,
                         bool *found) const {
    return host.stdlib_lookup_string_key(frame, value, key, out, found);
  }

  bool map_values(const Value &value, std::vector<Value> *out) const {
    return host.stdlib_map_values(frame, value, out);
  }

  bool list_items(const Value &value, std::vector<Value> *out) const {
    return host.stdlib_list_items(frame, value, out);
  }

  bool sequence_items(const Value &value, std::vector<Value> *out) const {
    return host.stdlib_sequence_items(frame, value, out);
  }

  StdlibBlockResult call_stream_block(Value value) const {
    return host.stdlib_call_stream_block(frame, block, std::move(value));
  }

  StdlibBlockResult call_path_block(Value value, Value accumulator) const {
    return host.stdlib_call_path_block(frame, block, std::move(value),
                                       std::move(accumulator));
  }

  StdlibBlockResult call_block(const Value &target,
                               std::vector<Value> block_args) const {
    return host.stdlib_call_block(frame, target, std::move(block_args));
  }

  void throw_json_stop(std::optional<Value> value = std::nullopt) const {
    host.stdlib_throw_json_stop(frame, std::move(value));
  }

  bool integer_range(const Value &value, StdlibIntegerRange *out) const {
    return host.stdlib_integer_range(frame, value, out);
  }

  bool fs_exists(const std::string &path, bool *out) const {
    return host.stdlib_fs_exists(frame, path, out);
  }

  bool fs_file(const std::string &path, bool *out) const {
    return host.stdlib_fs_file(frame, path, out);
  }

  bool fs_dir(const std::string &path, bool *out) const {
    return host.stdlib_fs_dir(frame, path, out);
  }

  bool fs_metadata(const std::string &path, Value *out) const {
    return host.stdlib_fs_metadata(frame, path, out);
  }

  bool fs_read_bytes(const std::string &path, std::optional<std::size_t> limit,
                     std::string *out) const {
    return host.stdlib_fs_read_bytes_limited(frame, path, limit, out);
  }

  bool fs_read_text(const std::string &path, std::optional<std::size_t> limit,
                    std::string *out) const {
    return host.stdlib_fs_read_text_limited(frame, path, limit, out);
  }

  bool fs_read_text(const std::string &path, std::string *out) const {
    return host.stdlib_fs_read_text(frame, path, out);
  }

  bool fs_write_text(const std::string &path, const std::string &text) const {
    return host.stdlib_fs_write_text(frame, path, text);
  }

  bool fs_write_bytes(const std::string &path, const std::string &bytes,
                      bool create, bool truncate) const {
    return host.stdlib_fs_write_bytes_value(frame, path, bytes, create,
                                            truncate);
  }

  bool fs_write_text(const std::string &path, const std::string &text,
                     bool create, bool truncate) const {
    return host.stdlib_fs_write_text_value(frame, path, text, create, truncate);
  }

  bool fs_mkdir(const std::string &path) const {
    return host.stdlib_fs_mkdir(frame, path);
  }

  bool fs_mkdir_p(const std::string &path) const {
    return host.stdlib_fs_mkdir_p(frame, path);
  }

  bool fs_remove(const std::string &path) const {
    return host.stdlib_fs_remove(frame, path);
  }

  bool fs_rename(const std::string &from, const std::string &to) const {
    return host.stdlib_fs_rename(frame, from, to);
  }

  bool fs_copy(const std::string &from, const std::string &to,
               std::size_t *count) const {
    return host.stdlib_fs_copy(frame, from, to, count);
  }

  bool fs_open_file(const std::string &path, RuntimeFileMode mode,
                    RuntimeFileOpenOptions options,
                    RuntimeIsolationMode isolation, Value *out) const {
    return host.stdlib_fs_open_file(frame, path, mode, options, isolation, out);
  }

  bool fs_close_file(const Value &file, bool report_fault) const {
    return host.stdlib_fs_close_file(frame, file, report_fault);
  }

  bool net_udp_bind(const RuntimeEndpoint &endpoint,
                    RuntimeIsolationMode isolation, Value *out) const {
    return host.stdlib_net_udp_bind(frame, endpoint, isolation, out);
  }

  bool net_udp_open(const std::string &family, RuntimeIsolationMode isolation,
                    Value *out) const {
    return host.stdlib_net_udp_open(frame, family, isolation, out);
  }

  bool net_tcp_connect(const RuntimeEndpoint &endpoint,
                       std::chrono::milliseconds timeout,
                       RuntimeIsolationMode isolation, Value *out) const {
    return host.stdlib_net_tcp_connect(frame, endpoint, timeout, isolation,
                                       out);
  }

  bool net_tcp_listen(const RuntimeEndpoint &endpoint, int backlog,
                      bool reuse_addr, RuntimeIsolationMode isolation,
                      Value *out) const {
    return host.stdlib_net_tcp_listen(frame, endpoint, backlog, reuse_addr,
                                      isolation, out);
  }

  bool net_tcp_close(const Value &resource, bool report_fault) const {
    return host.stdlib_net_tcp_close(frame, resource, report_fault);
  }

  SendStatus net_http_construct_client() const {
    return host.stdlib_net_http_construct_client(frame, args, block, kw_args,
                                                 out);
  }

  SendStatus net_http_construct_request() const {
    return host.stdlib_net_http_construct_request(frame, args, block, kw_args,
                                                  out);
  }

  SendStatus net_http_construct_headers() const {
    return host.stdlib_net_http_construct_headers(frame, args, block, kw_args,
                                                  out);
  }

  SendStatus net_http_construct_server() const {
    return host.stdlib_net_http_construct_server(frame, args, block, kw_args,
                                                 out);
  }

  SendStatus net_http_construct_server_response() const {
    return host.stdlib_net_http_construct_server_response(frame, args, block,
                                                          kw_args, out);
  }

  SendStatus net_http_construct_form_body() const {
    return host.stdlib_net_http_construct_form_body(frame, args, block, kw_args,
                                                    out);
  }

  SendStatus net_http_json_get() const {
    return host.stdlib_net_http_json_get(frame, args, block, kw_args, out);
  }

  SendStatus net_http_json_post() const {
    return host.stdlib_net_http_json_post(frame, args, block, kw_args, out);
  }

  SendStatus net_http_request_body_type_send() const {
    return host.stdlib_net_http_request_body_type_send(frame, selector, args,
                                                       block, kw_args, out);
  }

  SendStatus net_http_server_response_type_send() const {
    return host.stdlib_net_http_server_response_type_send(frame, selector, args,
                                                          block, kw_args, out);
  }

  bool secure_random_bytes(std::size_t count, std::string *out) const {
    return host.stdlib_secure_random_bytes(frame, count, out);
  }

  std::optional<RuntimeTimeValue> wall_time_now() const {
    return host.stdlib_wall_time_now(frame);
  }

  std::optional<RuntimeTimePeriodValue> monotonic_time() const {
    return host.stdlib_monotonic_time(frame);
  }
};

// A library handler keeps the proven chain-of-responsibility shape; it owns its
// kind's selectors and returns `NotHandled` for anything it does not.
using NativeStdlibHandler = SendStatus (*)(NativeStdlibCall &call);

class NativeRegistry;

enum class RuntimeBindingKind {
  NativeType,
  NativeFunction,
  TaskModule,
  FlowModule
};

struct RuntimeBindingRef {
  RuntimeBindingKind kind = RuntimeBindingKind::NativeType;
  RuntimeNativeTypeKind native_type = RuntimeNativeTypeKind::TaskModule;
  RuntimeNativeFunctionKind native_function = RuntimeNativeFunctionKind::Print;

  static RuntimeBindingRef native_type_binding(RuntimeNativeTypeKind kind) {
    RuntimeBindingRef ref;
    ref.kind = RuntimeBindingKind::NativeType;
    ref.native_type = kind;
    return ref;
  }

  static RuntimeBindingRef
  native_function_binding(RuntimeNativeFunctionKind kind) {
    RuntimeBindingRef ref;
    ref.kind = RuntimeBindingKind::NativeFunction;
    ref.native_function = kind;
    return ref;
  }

  static RuntimeBindingRef task_module_binding() {
    RuntimeBindingRef ref;
    ref.kind = RuntimeBindingKind::TaskModule;
    return ref;
  }

  static RuntimeBindingRef flow_module_binding() {
    RuntimeBindingRef ref;
    ref.kind = RuntimeBindingKind::FlowModule;
    return ref;
  }
};

class RuntimeModuleRegistry {
public:
  void register_native_type_path(std::string path, RuntimeNativeTypeKind kind);
  void register_native_function_path(std::string path,
                                     RuntimeNativeFunctionKind kind);
  void register_task_module_path(std::string path);
  void register_flow_module_path(std::string path);

  std::optional<RuntimeBindingRef>
  binding_for_path(const std::string &path) const;

  void import_native_paths(const NativeRegistry &registry);

private:
  std::unordered_map<std::string, RuntimeBindingRef> bindings_;
};

struct RuntimeTypeCallDescriptor {
  RuntimeNativeTypeKind kind = RuntimeNativeTypeKind::TaskModule;
  std::string selector;
};

struct RuntimeIoValueHandlerDescriptor {
  std::string type_name;
  RuntimeNativeTypeKind kind = RuntimeNativeTypeKind::TaskModule;
  NativeStdlibHandler handler = nullptr;
};

class RuntimeTypeRegistry {
public:
  void register_native_type_call(RuntimeNativeTypeKind kind,
                                 std::string selector);

  std::optional<RuntimeTypeCallDescriptor>
  native_type_call(RuntimeNativeTypeKind kind) const;

private:
  struct KindHash {
    std::size_t operator()(RuntimeNativeTypeKind kind) const {
      return static_cast<std::size_t>(kind);
    }
  };
  std::unordered_map<RuntimeNativeTypeKind, RuntimeTypeCallDescriptor, KindHash>
      native_calls_;
};

class RuntimeDispatchRegistry {
public:
  void register_native_handler(RuntimeNativeTypeKind kind,
                               NativeStdlibHandler handler);

  std::optional<NativeStdlibHandler>
  native_handler(RuntimeNativeTypeKind kind) const;

  void register_io_value_handler(std::string type_name,
                                 RuntimeNativeTypeKind kind,
                                 NativeStdlibHandler handler);

  std::optional<RuntimeIoValueHandlerDescriptor>
  io_value_handler(const std::string &type_name) const;

  void import_native_handlers(const NativeRegistry &registry);

private:
  struct KindHash {
    std::size_t operator()(RuntimeNativeTypeKind kind) const {
      return static_cast<std::size_t>(kind);
    }
  };
  std::unordered_map<RuntimeNativeTypeKind, NativeStdlibHandler, KindHash>
      native_handlers_;
  std::unordered_map<std::string, RuntimeIoValueHandlerDescriptor>
      io_value_handlers_;
};

class RuntimeErrorRegistry {
public:
  std::optional<std::uint16_t> error_id(const std::string &name) const;
  const char *error_name(std::uint16_t error_id) const;
  bool error_is_a(std::uint16_t error_id,
                  std::uint16_t ancestor_error_id) const;
};

struct RuntimeNativeModulePathDescriptor {
  const char *path = "";
  RuntimeNativeTypeKind kind = RuntimeNativeTypeKind::TaskModule;
};

struct RuntimeNativeModuleHandlerDescriptor {
  RuntimeNativeTypeKind kind = RuntimeNativeTypeKind::TaskModule;
  NativeStdlibHandler handler = nullptr;
};

struct RuntimeNativeModuleIoHandlerDescriptor {
  const char *type_name = "";
  RuntimeNativeTypeKind kind = RuntimeNativeTypeKind::TaskModule;
  NativeStdlibHandler handler = nullptr;
};

struct RuntimeNativeModuleTypeCallDescriptor {
  RuntimeNativeTypeKind kind = RuntimeNativeTypeKind::TaskModule;
  const char *selector = "";
};

struct RuntimeNativeModuleDescriptor {
  std::vector<RuntimeNativeModulePathDescriptor> paths;
  std::vector<RuntimeNativeModuleHandlerDescriptor> handlers;
  std::vector<RuntimeNativeModuleIoHandlerDescriptor> io_handlers;
  std::vector<RuntimeNativeModuleTypeCallDescriptor> type_calls;
};

// Compatibility facade populated once during VM construction (no static
// initializers, to avoid static-init-order fiascos). New runtime paths register
// module/dispatch/type-call descriptors directly on the world registries.
class NativeRegistry {
public:
  void register_handler(RuntimeNativeTypeKind kind,
                        NativeStdlibHandler handler);
  void register_path(std::string path, RuntimeNativeTypeKind kind);

  // The handler that owns `kind`, or nullptr when the kind is still on the
  // legacy inline chain.
  NativeStdlibHandler handler_for(RuntimeNativeTypeKind kind) const;

  // The kind a source path (`"Math"`, `"io.ByteBuffer"`, ...) resolves to, or
  // `nullopt` when unregistered.
  std::optional<RuntimeNativeTypeKind>
  kind_for_path(const std::string &path) const;

  std::vector<std::pair<std::string, RuntimeNativeTypeKind>>
  registered_paths() const;

  std::vector<std::pair<RuntimeNativeTypeKind, NativeStdlibHandler>>
  registered_handlers() const;

private:
  struct KindHash {
    std::size_t operator()(RuntimeNativeTypeKind kind) const {
      return static_cast<std::size_t>(kind);
    }
  };
  std::unordered_map<RuntimeNativeTypeKind, NativeStdlibHandler, KindHash>
      handlers_;
  std::unordered_map<std::string, RuntimeNativeTypeKind> paths_;
};

// Populate `registry` with every builtin library. Each library contributes a
// `register_<name>` that adds its names and handler; this is the single place
// that lists them.
void register_builtin_stdlib(NativeRegistry &registry);
void register_builtin_runtime_modules(RuntimeModuleRegistry &modules,
                                      RuntimeDispatchRegistry &dispatch,
                                      RuntimeTypeRegistry &types);
void register_native_module_descriptor(
    NativeRegistry &registry, const RuntimeNativeModuleDescriptor &descriptor);
void register_runtime_module_descriptor(
    RuntimeModuleRegistry &modules,
    const RuntimeNativeModuleDescriptor &descriptor);
void register_runtime_dispatch_descriptor(
    RuntimeDispatchRegistry &dispatch,
    const RuntimeNativeModuleDescriptor &descriptor);
void register_runtime_type_descriptor(
    RuntimeTypeRegistry &types,
    const RuntimeNativeModuleDescriptor &descriptor);
void register_core_prelude_bindings(RuntimeModuleRegistry &registry);
void register_legacy_native_type_paths(RuntimeModuleRegistry &registry);
void register_legacy_native_type_calls(RuntimeTypeRegistry &registry);

// Per-library registration entry points (defined in
// `runtime/stdlib_<name>.cpp`).
void register_io_runtime_module(RuntimeModuleRegistry &modules,
                                RuntimeDispatchRegistry &dispatch,
                                RuntimeTypeRegistry &types);
void register_fs_runtime_module(RuntimeModuleRegistry &modules,
                                RuntimeDispatchRegistry &dispatch,
                                RuntimeTypeRegistry &types);
void register_net_runtime_module(RuntimeModuleRegistry &modules,
                                 RuntimeDispatchRegistry &dispatch,
                                 RuntimeTypeRegistry &types);
void register_net_http_runtime_module(RuntimeModuleRegistry &modules,
                                      RuntimeDispatchRegistry &dispatch,
                                      RuntimeTypeRegistry &types);
void register_task_runtime_module(RuntimeModuleRegistry &modules,
                                  RuntimeDispatchRegistry &dispatch,
                                  RuntimeTypeRegistry &types);
void register_math(NativeRegistry &registry);
void register_math_runtime_module(RuntimeModuleRegistry &modules,
                                  RuntimeDispatchRegistry &dispatch,
                                  RuntimeTypeRegistry &types);
void register_json(NativeRegistry &registry);
void register_json_runtime_module(RuntimeModuleRegistry &modules,
                                  RuntimeDispatchRegistry &dispatch,
                                  RuntimeTypeRegistry &types);
void register_codecs(NativeRegistry &registry);
void register_codecs_runtime_module(RuntimeModuleRegistry &modules,
                                    RuntimeDispatchRegistry &dispatch,
                                    RuntimeTypeRegistry &types);
void register_digest(NativeRegistry &registry);
void register_digest_runtime_module(RuntimeModuleRegistry &modules,
                                    RuntimeDispatchRegistry &dispatch,
                                    RuntimeTypeRegistry &types);
void register_secure_random(NativeRegistry &registry);
void register_secure_random_runtime_module(RuntimeModuleRegistry &modules,
                                           RuntimeDispatchRegistry &dispatch,
                                           RuntimeTypeRegistry &types);
void register_argparser(NativeRegistry &registry);
void register_argparser_runtime_module(RuntimeModuleRegistry &modules,
                                       RuntimeDispatchRegistry &dispatch,
                                       RuntimeTypeRegistry &types);
void register_uuid(NativeRegistry &registry);
void register_uuid_runtime_module(RuntimeModuleRegistry &modules,
                                  RuntimeDispatchRegistry &dispatch,
                                  RuntimeTypeRegistry &types);
void register_time(NativeRegistry &registry);
void register_time_runtime_module(RuntimeModuleRegistry &modules,
                                  RuntimeDispatchRegistry &dispatch,
                                  RuntimeTypeRegistry &types);
void register_url(NativeRegistry &registry);
void register_url_runtime_module(RuntimeModuleRegistry &modules,
                                 RuntimeDispatchRegistry &dispatch,
                                 RuntimeTypeRegistry &types);

} // namespace amber::runtime
