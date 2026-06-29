#include "runtime/io.h"
#include "runtime/stdlib_registry.h"

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

Value endpoint_value(RuntimeEndpoint endpoint) {
  return Value::io_value(
      std::make_shared<RuntimeEndpoint>(std::move(endpoint)));
}

SendStatus endpoint_type_send(NativeStdlibCall &call) {
  if (call.selector == "new") {
    if (!call.require_arity(2) || !call.kw_args.empty() ||
        !call.require_no_block()) {
      return SendStatus::Faulted;
    }
    if (!call.args[0].is_string() || !call.args[1].is_integer()) {
      return call.fault("TypeError", "Endpoint.new expects Str and Int");
    }
    const std::optional<std::string> host = call.text_of(call.args[0]);
    if (!host.has_value() || call.args[1].as_integer() < 0 ||
        call.args[1].as_integer() > 65535) {
      return call.fault("ArgumentError", "invalid endpoint");
    }
    *call.out = endpoint_value(RuntimeEndpoint{
        *host, static_cast<std::uint16_t>(call.args[1].as_integer())});
    return SendStatus::Matched;
  }
  if (call.selector == "parse") {
    if (!call.require_arity(1) || !call.kw_args.empty() ||
        !call.require_no_block()) {
      return SendStatus::Faulted;
    }
    if (!call.args[0].is_string()) {
      return call.fault("TypeError", "Endpoint.parse expects Str");
    }
    const std::optional<std::string> text = call.text_of(call.args[0]);
    RuntimeEndpoint endpoint;
    const RuntimeIoStatus parsed =
        text.has_value() ? RuntimeEndpoint::parse(*text, &endpoint)
                         : RuntimeIoStatus{};
    if (!text.has_value()) {
      return call.fault("VMError", "endpoint string ref is invalid");
    }
    if (!set_fault_from_io_status(call, parsed)) {
      return SendStatus::Faulted;
    }
    *call.out = endpoint_value(std::move(endpoint));
    return SendStatus::Matched;
  }
  return SendStatus::NotHandled;
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

SendStatus udp_type_send(NativeStdlibCall &call) {
  if (call.selector == "bind") {
    if (!call.require_arity(2) ||
        !call.reject_unknown_keywords({"isolation"}) ||
        !call.require_no_block()) {
      return SendStatus::Faulted;
    }
    if (!call.args[0].is_string() || !call.args[1].is_integer()) {
      return call.fault("TypeError", "udp.bind expects host and port");
    }
    const std::optional<std::string> host = call.text_of(call.args[0]);
    if (!host.has_value() || call.args[1].as_integer() < 0 ||
        call.args[1].as_integer() > 65535) {
      return call.fault("ArgumentError", "invalid UDP endpoint");
    }
    const std::optional<RuntimeIsolationMode> isolation =
        isolation_from_keywords(call);
    if (!isolation.has_value()) {
      return SendStatus::Faulted;
    }
    const RuntimeEndpoint endpoint{
        *host, static_cast<std::uint16_t>(call.args[1].as_integer())};
    return call.net_udp_bind(endpoint, *isolation, call.out)
               ? SendStatus::Matched
               : SendStatus::Faulted;
  }
  if (call.selector == "open") {
    if (!call.args.empty() ||
        !call.reject_unknown_keywords({"family", "isolation"}) ||
        !call.require_no_block()) {
      return SendStatus::Faulted;
    }
    std::string family = "inet";
    if (const std::optional<Value> value = call.keyword("family")) {
      const std::optional<std::string> name = call.text_of(*value);
      if (!name.has_value()) {
        return call.fault("TypeError", "family must be Symbol or Str");
      }
      family = *name;
    }
    const std::optional<RuntimeIsolationMode> isolation =
        isolation_from_keywords(call);
    if (!isolation.has_value()) {
      return SendStatus::Faulted;
    }
    return call.net_udp_open(family, *isolation, call.out)
               ? SendStatus::Matched
               : SendStatus::Faulted;
  }
  return SendStatus::NotHandled;
}

SendStatus net_namespace_send(NativeStdlibCall &call) {
  if (call.selector != "tcp" && call.selector != "udp" &&
      call.selector != "Endpoint" && call.selector != "http") {
    return SendStatus::NotHandled;
  }
  if (!call.require_arity(0) || !call.kw_args.empty() ||
      !call.require_no_block()) {
    return SendStatus::Faulted;
  }
  const RuntimeNativeTypeKind member_kind =
      call.selector == "tcp"    ? RuntimeNativeTypeKind::NetTcp
      : call.selector == "udp"  ? RuntimeNativeTypeKind::NetUdp
      : call.selector == "http" ? RuntimeNativeTypeKind::NetHttp
                                : RuntimeNativeTypeKind::NetEndpoint;
  *call.out = Value::native_type(member_kind);
  return SendStatus::Matched;
}

RuntimeNativeModuleDescriptor net_module_descriptor() {
  return {{{"net", RuntimeNativeTypeKind::Net},
           {"net.Endpoint", RuntimeNativeTypeKind::NetEndpoint},
           {"net.tcp", RuntimeNativeTypeKind::NetTcp},
           {"net.udp", RuntimeNativeTypeKind::NetUdp}},
          {{RuntimeNativeTypeKind::Net, net_namespace_send},
           {RuntimeNativeTypeKind::NetEndpoint, endpoint_type_send},
           {RuntimeNativeTypeKind::NetUdp, udp_type_send}},
          {{RuntimeNativeTypeKind::NetEndpoint, "new"}}};
}

} // namespace

void register_net_runtime_module(RuntimeModuleRegistry &modules,
                                 RuntimeDispatchRegistry &dispatch,
                                 RuntimeTypeRegistry &types) {
  const RuntimeNativeModuleDescriptor descriptor = net_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
}

} // namespace amber::runtime
