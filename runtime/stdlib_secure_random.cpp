// SecureRandom stdlib library (DESIGN-stdlib-next-libs-order-2026-06-15 §4.3).
//
// v1 surface:
//   SecureRandom.bytes(n)                  -> Bytes
//   SecureRandom.int(range)                -> Int
//   SecureRandom.hex(n)                    -> Str
//   SecureRandom.base64(n, padding: true)  -> Str
//   SecureRandom.base64url(n, padding: false) -> Str
//   SecureRandom.uuid                      -> Uuid, delegates to Uuid.v4()

#include "runtime/stdlib_registry.h"
#include "runtime/stdlib_uuid.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace amber::runtime {

namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kBase64UrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr char kHexDigits[] = "0123456789abcdef";

std::optional<std::size_t> byte_count_arg(NativeStdlibCall &call,
                                          const std::string &context) {
  if (!call.args[0].is_integer()) {
    call.fault("TypeError", context + " expects an Int byte count");
    return std::nullopt;
  }
  const std::int64_t count = call.args[0].as_integer();
  if (count < 0) {
    call.fault("ArgumentError", context + " byte count must be non-negative");
    return std::nullopt;
  }
  return static_cast<std::size_t>(count);
}

std::string hex_encode(const std::string &bytes) {
  std::string out;
  out.reserve(bytes.size() * 2U);
  for (unsigned char byte : bytes) {
    out.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[byte & 0x0FU]);
  }
  return out;
}

std::string base64_encode(const std::string &bytes, bool url, bool padding) {
  const char *alphabet = url ? kBase64UrlAlphabet : kBase64Alphabet;
  std::string out;
  out.reserve(((bytes.size() + 2U) / 3U) * 4U);
  for (std::size_t i = 0; i < bytes.size(); i += 3U) {
    const std::uint32_t b0 = static_cast<unsigned char>(bytes[i]);
    const bool have_b1 = i + 1U < bytes.size();
    const bool have_b2 = i + 2U < bytes.size();
    const std::uint32_t b1 =
        have_b1 ? static_cast<unsigned char>(bytes[i + 1U]) : 0U;
    const std::uint32_t b2 =
        have_b2 ? static_cast<unsigned char>(bytes[i + 2U]) : 0U;
    out.push_back(alphabet[(b0 >> 2U) & 0x3FU]);
    out.push_back(alphabet[((b0 & 0x03U) << 4U) | ((b1 >> 4U) & 0x0FU)]);
    if (have_b1) {
      out.push_back(alphabet[((b1 & 0x0FU) << 2U) | ((b2 >> 6U) & 0x03U)]);
    } else if (padding) {
      out.push_back('=');
    }
    if (have_b2) {
      out.push_back(alphabet[b2 & 0x3FU]);
    } else if (padding) {
      out.push_back('=');
    }
  }
  return out;
}

std::uint64_t load_u64_le(const std::string &bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8U; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i]))
             << (i * 8U);
  }
  return value;
}

SendStatus random_bytes(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const std::optional<std::size_t> count =
      byte_count_arg(call, "SecureRandom.bytes");
  if (!count.has_value()) {
    return SendStatus::Faulted;
  }
  std::string bytes;
  if (!call.secure_random_bytes(*count, &bytes)) {
    return SendStatus::Faulted;
  }
  *call.out = call.bytes_value(std::move(bytes));
  return SendStatus::Matched;
}

SendStatus random_hex(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const std::optional<std::size_t> count =
      byte_count_arg(call, "SecureRandom.hex");
  if (!count.has_value()) {
    return SendStatus::Faulted;
  }
  std::string bytes;
  if (!call.secure_random_bytes(*count, &bytes)) {
    return SendStatus::Faulted;
  }
  *call.out = call.string_value(hex_encode(bytes));
  return SendStatus::Matched;
}

SendStatus random_base64(NativeStdlibCall &call, bool url) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({"padding"})) {
    return SendStatus::Faulted;
  }
  const std::optional<std::size_t> count = byte_count_arg(
      call, url ? "SecureRandom.base64url" : "SecureRandom.base64");
  if (!count.has_value()) {
    return SendStatus::Faulted;
  }
  bool padding = !url;
  if (!call.bool_keyword("padding", padding, &padding)) {
    return SendStatus::Faulted;
  }
  std::string bytes;
  if (!call.secure_random_bytes(*count, &bytes)) {
    return SendStatus::Faulted;
  }
  *call.out = call.string_value(base64_encode(bytes, url, padding));
  return SendStatus::Matched;
}

SendStatus random_int(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  StdlibIntegerRange range;
  if (!call.integer_range(call.args[0], &range)) {
    return SendStatus::Faulted;
  }
  if (range.count == 1U) {
    std::string unused;
    if (!call.secure_random_bytes(0U, &unused)) {
      return SendStatus::Faulted;
    }
    *call.out = Value::integer(range.start);
    return SendStatus::Matched;
  }
  const std::uint64_t threshold =
      (std::numeric_limits<std::uint64_t>::max() - range.count + 1U) %
      range.count;
  while (true) {
    std::string bytes;
    if (!call.secure_random_bytes(8U, &bytes)) {
      return SendStatus::Faulted;
    }
    const std::uint64_t sample = load_u64_le(bytes);
    if (sample < threshold) {
      continue;
    }
    const std::uint64_t offset = sample % range.count;
    *call.out = Value::integer(static_cast<std::int64_t>(
        static_cast<__int128>(range.start) +
        static_cast<__int128>(range.step) * static_cast<__int128>(offset)));
    return SendStatus::Matched;
  }
}

SendStatus secure_random_dispatch(NativeStdlibCall &call) {
  if (call.kind != RuntimeNativeTypeKind::SecureRandom) {
    return SendStatus::NotHandled;
  }
  if (call.selector == "bytes") {
    return random_bytes(call);
  }
  if (call.selector == "int") {
    return random_int(call);
  }
  if (call.selector == "hex") {
    return random_hex(call);
  }
  if (call.selector == "base64") {
    return random_base64(call, false);
  }
  if (call.selector == "base64url") {
    return random_base64(call, true);
  }
  if (call.selector == "uuid") {
    return uuid_v4(call);
  }
  return SendStatus::NotHandled;
}

RuntimeNativeModuleDescriptor secure_random_module_descriptor() {
  return {{{"SecureRandom", RuntimeNativeTypeKind::SecureRandom}},
          {{RuntimeNativeTypeKind::SecureRandom, &secure_random_dispatch}},
          {}};
}

} // namespace

void register_secure_random(NativeRegistry &registry) {
  register_native_module_descriptor(registry,
                                    secure_random_module_descriptor());
}

void register_secure_random_runtime_module(RuntimeModuleRegistry &modules,
                                           RuntimeDispatchRegistry &dispatch,
                                           RuntimeTypeRegistry &types) {
  const RuntimeNativeModuleDescriptor descriptor =
      secure_random_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
}

} // namespace amber::runtime
