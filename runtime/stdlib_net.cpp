#include "runtime/io.h"
#include "runtime/stdlib_registry.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

SendStatus resource_lifecycle_send(NativeStdlibCall &call,
                                   RuntimeIoResource &resource) {
  if (call.selector != "closed?" && call.selector != "close!") {
    return SendStatus::NotHandled;
  }
  if (!call.require_arity(0) || !call.kw_args.empty() ||
      !call.require_no_block()) {
    return SendStatus::Faulted;
  }
  if (call.selector == "closed?") {
    *call.out = Value::boolean(resource.closed());
    return SendStatus::Matched;
  }
  if (!resource.closed()) {
    const RuntimeIoStatus access = resource.access_status();
    if (!set_fault_from_io_status(call, access)) {
      return SendStatus::Faulted;
    }
  }
  RuntimeIoStatus result = resource.close();
  if (!set_fault_from_io_status(call, result)) {
    return SendStatus::Faulted;
  }
  *call.out = Value::null();
  return SendStatus::Matched;
}

std::optional<RuntimeEndpoint> endpoint_from_value(NativeStdlibCall &call,
                                                   const Value &value) {
  if (!value.is_io_value()) {
    call.fault("TypeError", "expected net.Endpoint");
    return std::nullopt;
  }
  const std::shared_ptr<RuntimeEndpoint> endpoint =
      std::dynamic_pointer_cast<RuntimeEndpoint>(value.as_io_value());
  if (endpoint == nullptr) {
    call.fault("TypeError", "expected net.Endpoint");
    return std::nullopt;
  }
  return *endpoint;
}

std::optional<RuntimeEndpoint>
endpoint_from_host_port(NativeStdlibCall &call, const std::string &selector) {
  if (call.args.size() != 2U || !call.args[0].is_string() ||
      !call.args[1].is_integer()) {
    call.fault("TypeError", selector + " expects host and port");
    return std::nullopt;
  }
  const std::optional<std::string> host = call.text_of(call.args[0]);
  if (!host.has_value() || call.args[1].as_integer() < 0 ||
      call.args[1].as_integer() > 65535) {
    call.fault("ArgumentError", "invalid network endpoint");
    return std::nullopt;
  }
  return RuntimeEndpoint{*host,
                         static_cast<std::uint16_t>(call.args[1].as_integer())};
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

SendStatus endpoint_instance_send(NativeStdlibCall &call) {
  const auto endpoint =
      std::dynamic_pointer_cast<RuntimeEndpoint>(call.receiver.as_io_value());
  if (endpoint == nullptr) {
    return SendStatus::NotHandled;
  }
  if (!call.require_arity(0) || !call.kw_args.empty() ||
      !call.require_no_block()) {
    return SendStatus::Faulted;
  }
  if (call.selector == "host") {
    *call.out = call.string_value(endpoint->host);
  } else if (call.selector == "port") {
    *call.out = Value::integer(endpoint->port);
  } else if (call.selector == "family") {
    *call.out = call.symbol_value(endpoint->family());
  } else if (call.selector == "to_str") {
    *call.out = call.string_value(endpoint->to_string());
  } else {
    return SendStatus::NotHandled;
  }
  return SendStatus::Matched;
}

SendStatus tcp_stream_instance_send(NativeStdlibCall &call) {
  const auto stream =
      std::dynamic_pointer_cast<RuntimeTcpStream>(call.receiver.as_io_value());
  if (stream == nullptr) {
    return SendStatus::NotHandled;
  }
  if (call.selector == "closed?" || call.selector == "close!") {
    return resource_lifecycle_send(call, *stream);
  }
  if (call.selector != "local_endpoint" && call.selector != "remote_endpoint") {
    return call.vm_io_value_intrinsic_send();
  }
  if (!call.require_arity(0) || !call.kw_args.empty() ||
      !call.require_no_block()) {
    return SendStatus::Faulted;
  }
  *call.out = endpoint_value(call.selector == "local_endpoint"
                                 ? stream->local_endpoint()
                                 : stream->remote_endpoint());
  return SendStatus::Matched;
}

SendStatus tcp_listener_instance_send(NativeStdlibCall &call) {
  const auto listener = std::dynamic_pointer_cast<RuntimeTcpListener>(
      call.receiver.as_io_value());
  if (listener == nullptr) {
    return SendStatus::NotHandled;
  }
  if (call.selector == "closed?" || call.selector == "close!") {
    return resource_lifecycle_send(call, *listener);
  }
  if (call.selector != "local_endpoint") {
    return call.vm_io_value_intrinsic_send();
  }
  if (!call.require_arity(0) || !call.kw_args.empty() ||
      !call.require_no_block()) {
    return SendStatus::Faulted;
  }
  *call.out = endpoint_value(listener->local_endpoint());
  return SendStatus::Matched;
}

SendStatus udp_socket_instance_send(NativeStdlibCall &call) {
  const auto socket =
      std::dynamic_pointer_cast<RuntimeUdpSocket>(call.receiver.as_io_value());
  if (socket == nullptr) {
    return SendStatus::NotHandled;
  }
  if (call.selector == "closed?" || call.selector == "close!") {
    return resource_lifecycle_send(call, *socket);
  }
  if (call.selector != "local_endpoint") {
    return call.vm_io_value_intrinsic_send();
  }
  if (!call.require_arity(0) || !call.kw_args.empty() ||
      !call.require_no_block()) {
    return SendStatus::Faulted;
  }
  *call.out = endpoint_value(socket->local_endpoint());
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

std::optional<std::chrono::milliseconds>
timeout_from_value(NativeStdlibCall &call, const Value &value) {
  if (value.is_null()) {
    return std::chrono::milliseconds::max();
  }
  double seconds = 0.0;
  if (value.is_integer()) {
    seconds = static_cast<double>(value.as_integer());
  } else if (value.is_float()) {
    seconds = value.as_float();
  } else {
    call.fault("TypeError", "timeout must be numeric or null");
    return std::nullopt;
  }
  if (!std::isfinite(seconds) || seconds < 0.0) {
    call.fault("ArgumentError", "timeout must be non-negative");
    return std::nullopt;
  }
  const double milliseconds = seconds * 1000.0;
  if (milliseconds >
      static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return std::chrono::milliseconds::max();
  }
  return std::chrono::milliseconds(static_cast<std::int64_t>(milliseconds));
}

std::optional<std::chrono::milliseconds>
timeout_from_keywords(NativeStdlibCall &call) {
  return timeout_from_value(call,
                            call.keyword("timeout").value_or(Value::null()));
}

SendStatus tcp_block_result(NativeStdlibCall &call, const Value &resource) {
  if (call.block.is_null()) {
    *call.out = resource;
    return SendStatus::Matched;
  }
  if (!call.block.is_closure()) {
    call.net_tcp_close(resource, false);
    return call.fault("TypeError",
                      call.selector == "connect"
                          ? "net.tcp.connect block must be closure"
                          : "net.tcp.listen block must be closure");
  }

  StdlibBlockResult block =
      call.call_block(call.block, std::vector<Value>{resource});
  if (block.status == StdlibBlockStatus::Returned) {
    if (!call.net_tcp_close(resource, true)) {
      return SendStatus::Faulted;
    }
    *call.out = std::move(block.value);
    return SendStatus::Matched;
  }
  call.net_tcp_close(resource, false);
  if (block.status == StdlibBlockStatus::Raised) {
    return call.raise(block.exception);
  }
  return SendStatus::Faulted;
}

SendStatus tcp_type_send(NativeStdlibCall &call) {
  if (call.selector != "connect" && call.selector != "listen") {
    return SendStatus::NotHandled;
  }
  const bool keywords_ok =
      call.selector == "connect"
          ? call.reject_unknown_keywords({"timeout", "isolation"})
          : call.reject_unknown_keywords(
                {"backlog", "reuse_addr", "isolation"});
  if (!keywords_ok) {
    return SendStatus::Faulted;
  }

  std::optional<RuntimeEndpoint> endpoint;
  if (call.selector == "connect" && call.args.size() == 1U) {
    endpoint = endpoint_from_value(call, call.args[0]);
  } else {
    endpoint = endpoint_from_host_port(call, call.selector);
  }
  if (!endpoint.has_value()) {
    return SendStatus::Faulted;
  }
  const std::optional<RuntimeIsolationMode> isolation =
      isolation_from_keywords(call);
  if (!isolation.has_value()) {
    return SendStatus::Faulted;
  }

  Value resource = Value::null();
  if (call.selector == "connect") {
    const std::optional<std::chrono::milliseconds> timeout =
        timeout_from_keywords(call);
    if (!timeout.has_value()) {
      return SendStatus::Faulted;
    }
    if (!call.net_tcp_connect(*endpoint, *timeout, *isolation, &resource)) {
      return SendStatus::Faulted;
    }
    return tcp_block_result(call, resource);
  }

  int backlog = 128;
  if (const std::optional<Value> value = call.keyword("backlog")) {
    if (!value->is_integer()) {
      return call.fault("TypeError", "backlog must be Int");
    }
    backlog = static_cast<int>(value->as_integer());
  }
  if (backlog <= 0) {
    return call.fault("ArgumentError", "backlog must be positive");
  }
  bool reuse_addr = false;
  if (!call.bool_keyword("reuse_addr", false, &reuse_addr)) {
    return SendStatus::Faulted;
  }
  if (!call.net_tcp_listen(*endpoint, backlog, reuse_addr, *isolation,
                           &resource)) {
    return SendStatus::Faulted;
  }
  return tcp_block_result(call, resource);
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
           {RuntimeNativeTypeKind::NetTcp, tcp_type_send},
           {RuntimeNativeTypeKind::NetUdp, udp_type_send}},
          {{"net.Endpoint", RuntimeNativeTypeKind::NetEndpoint,
            endpoint_instance_send},
           {"net.TcpStream", RuntimeNativeTypeKind::NetTcp,
            tcp_stream_instance_send},
           {"net.TcpListener", RuntimeNativeTypeKind::NetTcp,
            tcp_listener_instance_send},
           {"net.UdpSocket", RuntimeNativeTypeKind::NetUdp,
            udp_socket_instance_send}},
          {{RuntimeNativeTypeKind::NetEndpoint, "new"}},
          {{"ConnectionError", "Exception"},
           {"ConnectionRefusedError", "ConnectionError"},
           {"ConnectionResetError", "ConnectionError"},
           {"DnsError", "Exception"}}};
}

} // namespace

void register_net_runtime_module(RuntimeModuleRegistry &modules,
                                 RuntimeDispatchRegistry &dispatch,
                                 RuntimeTypeRegistry &types,
                                 RuntimeErrorRegistry *errors) {
  const RuntimeNativeModuleDescriptor descriptor = net_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
  if (errors != nullptr) {
    register_runtime_error_descriptor(*errors, descriptor);
  }
}

} // namespace amber::runtime
