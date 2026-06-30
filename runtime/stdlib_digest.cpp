// Digest stdlib library (DESIGN-stdlib-next-libs-order-2026-06-15 §4.6).

#include "runtime/digest.h"
#include "runtime/stdlib_registry.h"

#include <optional>
#include <string>
#include <utility>

namespace amber::runtime {

namespace {

bool digest_one_shot_selector(const std::string &selector) {
  return selector == "crc32" || selector == "md5" || selector == "sha1" ||
         selector == "sha256" || selector == "streebog256" ||
         selector == "streebog512" || selector == "gost256" ||
         selector == "gost512" || selector == "гост256" ||
         selector == "гост512" || selector == "стрибог256" ||
         selector == "стрибог512";
}

SendStatus digest_dispatch(NativeStdlibCall &call) {
  if (call.kind != RuntimeNativeTypeKind::Digest) {
    return SendStatus::NotHandled;
  }
  const bool hmac = call.selector == "hmac_sha256";
  if (!hmac && !digest_one_shot_selector(call.selector)) {
    return SendStatus::NotHandled;
  }
  if (!call.require_no_block() || !call.reject_unknown_keywords({}) ||
      !call.require_arity(hmac ? 2U : 1U)) {
    return SendStatus::Faulted;
  }

  const std::optional<std::string> first = call.bytes_of(call.args[0]);
  if (!first.has_value()) {
    return SendStatus::Faulted;
  }
  std::string result;
  if (hmac) {
    const std::optional<std::string> bytes = call.bytes_of(call.args[1]);
    if (!bytes.has_value()) {
      return SendStatus::Faulted;
    }
    result = digest_hmac_sha256(*first, *bytes);
  } else if (call.selector == "crc32") {
    result = digest_crc32(*first);
  } else if (call.selector == "md5") {
    result = digest_md5(*first);
  } else if (call.selector == "sha1") {
    result = digest_sha1(*first);
  } else if (call.selector == "sha256") {
    result = digest_sha256(*first);
  } else if (call.selector == "streebog256" || call.selector == "gost256" ||
             call.selector == "гост256" || call.selector == "стрибог256") {
    result = digest_streebog256(*first);
  } else {
    result = digest_streebog512(*first);
  }
  *call.out = call.bytes_value(std::move(result));
  return SendStatus::Matched;
}

RuntimeNativeModuleDescriptor digest_module_descriptor() {
  return {{{"Digest", RuntimeNativeTypeKind::Digest}},
          {{RuntimeNativeTypeKind::Digest, &digest_dispatch}},
          {},
          {},
          {}};
}

} // namespace

void register_digest(NativeRegistry &registry) {
  register_native_module_descriptor(registry, digest_module_descriptor());
}

void register_digest_runtime_module(RuntimeModuleRegistry &modules,
                                    RuntimeDispatchRegistry &dispatch,
                                    RuntimeTypeRegistry &types) {
  const RuntimeNativeModuleDescriptor descriptor = digest_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
}

} // namespace amber::runtime
