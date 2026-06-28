#include "runtime/stdlib_registry.h"

#include <string>

namespace amber::runtime {

namespace {

SendStatus net_http_namespace_send(NativeStdlibCall &call) {
  if (call.selector == "RequestBody") {
    if (call.args.empty() && call.kw_args.empty() && call.block.is_null()) {
      *call.out = Value::native_type(RuntimeNativeTypeKind::NetHttpRequestBody);
      return SendStatus::Matched;
    }
    return call.fault("TypeError",
                      "net.http.RequestBody is not directly callable");
  }
  if (call.selector == "ServerRequest") {
    if (call.args.empty() && call.kw_args.empty() && call.block.is_null()) {
      *call.out =
          Value::native_type(RuntimeNativeTypeKind::NetHttpServerRequest);
      return SendStatus::Matched;
    }
    return call.fault("TypeError",
                      "net.http.ServerRequest is not directly callable");
  }
  if (call.selector == "ServerResponse") {
    if (call.args.empty() && call.kw_args.empty() && call.block.is_null()) {
      *call.out =
          Value::native_type(RuntimeNativeTypeKind::NetHttpServerResponse);
      return SendStatus::Matched;
    }
    return SendStatus::NotHandled;
  }
  if (call.selector == "json" || call.selector == "form") {
    if (!call.args.empty() || !call.kw_args.empty() || !call.block.is_null()) {
      return call.fault("ArgumentError",
                        "net.http." + call.selector + " takes no arguments");
    }
    *call.out = Value::native_type(call.selector == "json"
                                       ? RuntimeNativeTypeKind::NetHttpJson
                                       : RuntimeNativeTypeKind::NetHttpForm);
    return SendStatus::Matched;
  }
  return SendStatus::NotHandled;
}

RuntimeNativeModuleDescriptor net_http_module_descriptor() {
  return {
      {{"net.http", RuntimeNativeTypeKind::NetHttp},
       {"net.http.Client", RuntimeNativeTypeKind::NetHttpClient},
       {"net.http.Request", RuntimeNativeTypeKind::NetHttpRequest},
       {"net.http.RequestBody", RuntimeNativeTypeKind::NetHttpRequestBody},
       {"net.http.Headers", RuntimeNativeTypeKind::NetHttpHeaders},
       {"net.http.Server", RuntimeNativeTypeKind::NetHttpServer},
       {"net.http.ServerRequest", RuntimeNativeTypeKind::NetHttpServerRequest},
       {"net.http.ServerResponse",
        RuntimeNativeTypeKind::NetHttpServerResponse},
       {"net.http.json", RuntimeNativeTypeKind::NetHttpJson},
       {"net.http.json.get_json", RuntimeNativeTypeKind::NetHttpJsonGetJson},
       {"net.http.json.post_json", RuntimeNativeTypeKind::NetHttpJsonPostJson},
       {"net.http.form", RuntimeNativeTypeKind::NetHttpForm},
       {"net.http.form.FormBody", RuntimeNativeTypeKind::NetHttpFormBody}},
      {{RuntimeNativeTypeKind::NetHttp, net_http_namespace_send}},
      {{RuntimeNativeTypeKind::NetHttpClient, "new"},
       {RuntimeNativeTypeKind::NetHttpRequest, "new"},
       {RuntimeNativeTypeKind::NetHttpRequestBody, "new"},
       {RuntimeNativeTypeKind::NetHttpHeaders, "new"},
       {RuntimeNativeTypeKind::NetHttpServer, "new"},
       {RuntimeNativeTypeKind::NetHttpServerResponse, "new"},
       {RuntimeNativeTypeKind::NetHttpJsonGetJson, "__call__"},
       {RuntimeNativeTypeKind::NetHttpJsonPostJson, "__call__"},
       {RuntimeNativeTypeKind::NetHttpFormBody, "__call__"}}};
}

} // namespace

void register_net_http_runtime_module(RuntimeModuleRegistry &modules,
                                      RuntimeDispatchRegistry &dispatch,
                                      RuntimeTypeRegistry &types) {
  const RuntimeNativeModuleDescriptor descriptor = net_http_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
}

} // namespace amber::runtime
