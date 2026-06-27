#include "runtime/stdlib_registry.h"

namespace amber::runtime {

namespace {

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
      {},
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
