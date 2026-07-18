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
    return call.net_http_construct_server_response();
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
  if (call.selector == "trace") {
    if (!call.args.empty() || !call.kw_args.empty() || call.block.is_null()) {
      return call.fault("ArgumentError",
                        "net.http.trace requires a block and no arguments");
    }
    if (!call.block.is_closure()) {
      return call.fault("TypeError", "net.http.trace block must be closure");
    }
    *call.out = call.block;
    return SendStatus::Matched;
  }
  if (call.selector == "Client") {
    return call.net_http_construct_client();
  }
  if (call.selector == "Request") {
    return call.net_http_construct_request();
  }
  if (call.selector == "Headers") {
    return call.net_http_construct_headers();
  }
  if (call.selector == "Server") {
    return call.net_http_construct_server();
  }
  return SendStatus::NotHandled;
}

SendStatus net_http_json_send(NativeStdlibCall &call) {
  if (call.selector == "get_json") {
    return call.net_http_json_get();
  }
  if (call.selector == "post_json") {
    return call.net_http_json_post();
  }
  return SendStatus::NotHandled;
}

SendStatus net_http_json_get_send(NativeStdlibCall &call) {
  if (call.selector == "__call__" || call.selector == "new") {
    return call.net_http_json_get();
  }
  return SendStatus::NotHandled;
}

SendStatus net_http_json_post_send(NativeStdlibCall &call) {
  if (call.selector == "__call__" || call.selector == "new") {
    return call.net_http_json_post();
  }
  return SendStatus::NotHandled;
}

SendStatus net_http_form_send(NativeStdlibCall &call) {
  if (call.selector == "FormBody") {
    return call.net_http_construct_form_body();
  }
  return SendStatus::NotHandled;
}

SendStatus net_http_form_body_send(NativeStdlibCall &call) {
  if (call.selector == "__call__" || call.selector == "new") {
    return call.net_http_construct_form_body();
  }
  return SendStatus::NotHandled;
}

SendStatus net_http_client_type_send(NativeStdlibCall &call) {
  if (call.selector == "new" || call.selector == "__call__") {
    return call.net_http_construct_client();
  }
  return SendStatus::NotHandled;
}

SendStatus net_http_request_type_send(NativeStdlibCall &call) {
  if (call.selector == "new" || call.selector == "__call__") {
    return call.net_http_construct_request();
  }
  return SendStatus::NotHandled;
}

SendStatus net_http_headers_type_send(NativeStdlibCall &call) {
  if (call.selector == "new" || call.selector == "__call__") {
    return call.net_http_construct_headers();
  }
  return SendStatus::NotHandled;
}

SendStatus net_http_server_type_send(NativeStdlibCall &call) {
  if (call.selector == "new" || call.selector == "__call__") {
    return call.net_http_construct_server();
  }
  return SendStatus::NotHandled;
}

SendStatus net_http_request_body_type_send(NativeStdlibCall &call) {
  return call.net_http_request_body_type_send();
}

SendStatus net_http_server_request_type_send(NativeStdlibCall & /*call*/) {
  return SendStatus::NotHandled;
}

SendStatus net_http_server_response_type_send(NativeStdlibCall &call) {
  return call.net_http_server_response_type_send();
}

SendStatus net_http_instance_send(NativeStdlibCall &call) {
  return call.vm_io_value_intrinsic_send();
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
      {{RuntimeNativeTypeKind::NetHttp, net_http_namespace_send},
       {RuntimeNativeTypeKind::NetHttpJson, net_http_json_send},
       {RuntimeNativeTypeKind::NetHttpJsonGetJson, net_http_json_get_send},
       {RuntimeNativeTypeKind::NetHttpJsonPostJson, net_http_json_post_send},
       {RuntimeNativeTypeKind::NetHttpForm, net_http_form_send},
       {RuntimeNativeTypeKind::NetHttpFormBody, net_http_form_body_send},
       {RuntimeNativeTypeKind::NetHttpClient, net_http_client_type_send},
       {RuntimeNativeTypeKind::NetHttpRequest, net_http_request_type_send},
       {RuntimeNativeTypeKind::NetHttpRequestBody,
        net_http_request_body_type_send},
       {RuntimeNativeTypeKind::NetHttpHeaders, net_http_headers_type_send},
       {RuntimeNativeTypeKind::NetHttpServer, net_http_server_type_send},
       {RuntimeNativeTypeKind::NetHttpServerRequest,
        net_http_server_request_type_send},
       {RuntimeNativeTypeKind::NetHttpServerResponse,
        net_http_server_response_type_send}},
      {{"net.http.Client", RuntimeNativeTypeKind::NetHttpClient,
        net_http_instance_send},
       {"net.http.RequestBody", RuntimeNativeTypeKind::NetHttpRequestBody,
        net_http_instance_send},
       {"net.http.ResponseBody", RuntimeNativeTypeKind::NetHttpRequestBody,
        net_http_instance_send},
       {"net.http.RedirectRecord", RuntimeNativeTypeKind::NetHttp,
        net_http_instance_send},
       {"net.http.Request", RuntimeNativeTypeKind::NetHttpRequest,
        net_http_instance_send},
       {"net.http.Response", RuntimeNativeTypeKind::NetHttp,
        net_http_instance_send},
       {"net.http.RequestHandle", RuntimeNativeTypeKind::NetHttp,
        net_http_instance_send},
       {"net.http.Headers", RuntimeNativeTypeKind::NetHttpHeaders,
        net_http_instance_send},
       {"net.http.Server", RuntimeNativeTypeKind::NetHttpServer,
        net_http_instance_send},
       {"net.http.ServerRequest", RuntimeNativeTypeKind::NetHttpServerRequest,
        net_http_instance_send},
       {"net.http.ServerRequestBody",
        RuntimeNativeTypeKind::NetHttpServerRequest,
        net_http_instance_send},
       {"net.http.ServerRequestChunk",
        RuntimeNativeTypeKind::NetHttpServerRequest,
        net_http_instance_send},
       {"net.http.ServerResponse",
        RuntimeNativeTypeKind::NetHttpServerResponse,
        net_http_instance_send},
       {"net.http.ServerResponseWriter",
        RuntimeNativeTypeKind::NetHttpServerResponse,
        net_http_instance_send}},
      {{RuntimeNativeTypeKind::NetHttpClient, "new"},
       {RuntimeNativeTypeKind::NetHttpRequest, "new"},
       {RuntimeNativeTypeKind::NetHttpRequestBody, "new"},
       {RuntimeNativeTypeKind::NetHttpHeaders, "new"},
       {RuntimeNativeTypeKind::NetHttpServer, "new"},
       {RuntimeNativeTypeKind::NetHttpServerResponse, "new"},
       {RuntimeNativeTypeKind::NetHttpJsonGetJson, "__call__"},
       {RuntimeNativeTypeKind::NetHttpJsonPostJson, "__call__"},
       {RuntimeNativeTypeKind::NetHttpFormBody, "__call__"}},
      {{"HttpError", "Exception"},
       {"RequestError", "HttpError"},
       {"InvalidUrlError", "RequestError"},
       {"InvalidMethodError", "RequestError"},
       {"InvalidHeaderError", "RequestError"},
       {"UnsupportedSchemeError", "RequestError"},
       {"RequestStateError", "RequestError"},
       {"ProtocolError", "HttpError"},
       {"HeaderLimitError", "ProtocolError"},
       {"StatusLineLimitError", "ProtocolError"},
       {"ChunkError", "ProtocolError"},
       {"UnexpectedEofError", "ProtocolError"},
       {"BodyError", "HttpError"},
       {"BodyConsumedError", "BodyError"},
       {"BodyLengthError", "BodyError"},
       {"BodyLimitError", "BodyError"},
       {"RedirectError", "HttpError"},
       {"TooManyRedirectsError", "RedirectError"},
       {"NonReplayableRedirectError", "RedirectError"},
       {"HttpTimeoutError", "HttpError"},
       {"PoolTimeoutError", "HttpTimeoutError"},
       {"OpenTimeoutError", "HttpTimeoutError"},
       {"ReadTimeoutError", "HttpTimeoutError"},
       {"WriteTimeoutError", "HttpTimeoutError"},
       {"BodyTimeoutError", "HttpTimeoutError"},
       {"TotalTimeoutError", "HttpTimeoutError"}}};
}

} // namespace

void register_net_http_runtime_module(RuntimeModuleRegistry &modules,
                                      RuntimeDispatchRegistry &dispatch,
                                      RuntimeTypeRegistry &types,
                                      RuntimeErrorRegistry *errors) {
  const RuntimeNativeModuleDescriptor descriptor = net_http_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
  if (errors != nullptr) {
    register_runtime_error_descriptor(*errors, descriptor);
  }
}

} // namespace amber::runtime
