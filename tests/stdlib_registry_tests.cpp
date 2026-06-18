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
using amber::runtime::RuntimeNativeTypeKind;
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
  Value stdlib_make_list(std::vector<Value> /*items*/) override {
    return Value::null();
  }
  Value
  stdlib_make_object(std::vector<std::pair<std::string, Value>> /*entries*/,
                     bool /*strict*/) override {
    return Value::null();
  }
  amber::runtime::StdlibBlockResult
  stdlib_call_stream_block(const void * /*frame*/, const Value & /*block*/,
                           Value /*value*/) override {
    return {};
  }
  void stdlib_throw_json_stop(const void * /*frame*/) override {}
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

// Build and dispatch one Math SEND through the registry handler. Returns the
// status; `out` and `host` carry the result / fault back to the caller.
SendStatus dispatch_math(const NativeRegistry &registry, MockHost &host,
                         const std::string &selector,
                         const std::vector<Value> &args, const Value &block,
                         Value *out) {
  const NativeStdlibHandler handler =
      registry.handler_for(RuntimeNativeTypeKind::Math);
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
  const std::optional<RuntimeNativeTypeKind> digest =
      registry.kind_for_path("Digest");
  expect(digest.has_value() && *digest == RuntimeNativeTypeKind::Digest,
         "kind_for_path(\"Digest\") resolves to Digest");
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

void test_handler_table(const NativeRegistry &registry) {
  expect(registry.handler_for(RuntimeNativeTypeKind::Math) != nullptr,
         "Math handler is registered");
  expect(registry.handler_for(RuntimeNativeTypeKind::Base64) != nullptr,
         "Base64 handler is registered");
  expect(registry.handler_for(RuntimeNativeTypeKind::Digest) != nullptr,
         "Digest handler is registered");
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

} // namespace

int main() {
  NativeRegistry registry;
  amber::runtime::register_builtin_stdlib(registry);

  test_path_resolution(registry);
  test_handler_table(registry);
  test_math_compute(registry);
  test_math_not_handled(registry);
  test_math_faults(registry);

  std::cout << "stdlib registry tests passed\n";
  return 0;
}
