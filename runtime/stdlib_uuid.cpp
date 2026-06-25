// UUID stdlib library (DESIGN-stdlib-next-libs-order-2026-06-15 §4.4).
//
// v1 surface:
//   Uuid.v4() / Uuid.v7() -> Uuid
//   Uuid.parse(text)       -> Uuid
//   uuid.to_str / inspect / to_json / version
//   UUID is an alias for schema/type-annotation spelling.

#include "runtime/stdlib_uuid.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace amber::runtime {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";
constexpr std::uint64_t kMaxUuidV7Milliseconds = (1ULL << 48U) - 1U;

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + c - 'a';
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + c - 'A';
  }
  return -1;
}

std::optional<RuntimeUuidValue> parse_uuid(const std::string &text) {
  if (text.size() != 36U || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') {
    return std::nullopt;
  }
  RuntimeUuidValue uuid;
  std::size_t byte_index = 0;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '-') {
      ++i;
      continue;
    }
    if (i + 1U >= text.size() || byte_index >= uuid.bytes.size()) {
      return std::nullopt;
    }
    const int high = hex_value(text[i]);
    const int low = hex_value(text[i + 1U]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    uuid.bytes[byte_index++] = static_cast<std::uint8_t>((high << 4U) | low);
    i += 2U;
  }
  if (byte_index != uuid.bytes.size()) {
    return std::nullopt;
  }
  return uuid;
}

Value uuid_value(RuntimeUuidValue uuid) {
  return Value::uuid(std::make_shared<RuntimeUuidValue>(uuid));
}

SendStatus uuid_v7(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(0) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const std::optional<RuntimeTimeValue> now = call.wall_time_now();
  if (!now.has_value()) {
    return SendStatus::Faulted;
  }
  const __int128 milliseconds =
      static_cast<__int128>(now->epoch_seconds) * 1000 +
      now->nanosecond / 1000000U;
  if (milliseconds < 0 ||
      milliseconds > static_cast<__int128>(kMaxUuidV7Milliseconds)) {
    return call.fault("UuidError",
                      "Uuid.v7 timestamp is outside the 48-bit Unix epoch");
  }

  std::string random;
  if (!call.secure_random_bytes(10U, &random)) {
    return SendStatus::Faulted;
  }
  RuntimeUuidValue uuid;
  const std::uint64_t timestamp = static_cast<std::uint64_t>(milliseconds);
  for (std::size_t i = 0; i < 6U; ++i) {
    uuid.bytes[i] = static_cast<std::uint8_t>(timestamp >> ((5U - i) * 8U));
  }
  for (std::size_t i = 0; i < random.size(); ++i) {
    uuid.bytes[6U + i] =
        static_cast<std::uint8_t>(static_cast<unsigned char>(random[i]));
  }
  uuid.bytes[6] = static_cast<std::uint8_t>((uuid.bytes[6] & 0x0FU) | 0x70U);
  uuid.bytes[8] = static_cast<std::uint8_t>((uuid.bytes[8] & 0x3FU) | 0x80U);
  *call.out = uuid_value(uuid);
  return SendStatus::Matched;
}

SendStatus uuid_parse(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const std::optional<std::string> text = call.text_of(call.args[0]);
  if (!text.has_value() || !call.args[0].is_string()) {
    return call.fault("TypeError", "Uuid.parse expects Str");
  }
  const std::optional<RuntimeUuidValue> parsed = parse_uuid(*text);
  if (!parsed.has_value()) {
    return call.fault("UuidParseError", "invalid canonical UUID text");
  }
  *call.out = uuid_value(*parsed);
  return SendStatus::Matched;
}

SendStatus uuid_instance_dispatch(NativeStdlibCall &call) {
  if (call.selector != "to_str" && call.selector != "inspect" &&
      call.selector != "version") {
    return SendStatus::NotHandled;
  }
  if (!call.require_no_block() || !call.require_arity(0) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const std::shared_ptr<RuntimeUuidValue> uuid = call.receiver.as_uuid();
  if (uuid == nullptr) {
    return call.fault("TypeError", "Uuid value is null");
  }
  if (call.selector == "to_str" || call.selector == "inspect") {
    *call.out = call.string_value(runtime_uuid_to_string(*uuid));
    return SendStatus::Matched;
  }
  if (call.selector == "version") {
    *call.out = Value::integer((uuid->bytes[6] >> 4U) & 0x0FU);
    return SendStatus::Matched;
  }
  return SendStatus::Matched;
}

SendStatus uuid_dispatch(NativeStdlibCall &call) {
  if (call.kind != RuntimeNativeTypeKind::Uuid) {
    return SendStatus::NotHandled;
  }
  if (call.receiver.is_uuid()) {
    return uuid_instance_dispatch(call);
  }
  if (call.selector == "v4") {
    return uuid_v4(call);
  }
  if (call.selector == "v7") {
    return uuid_v7(call);
  }
  if (call.selector == "parse") {
    return uuid_parse(call);
  }
  return SendStatus::NotHandled;
}

} // namespace

SendStatus uuid_v4(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(0) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  std::string random;
  if (!call.secure_random_bytes(16U, &random)) {
    return SendStatus::Faulted;
  }
  RuntimeUuidValue uuid;
  for (std::size_t i = 0; i < uuid.bytes.size(); ++i) {
    uuid.bytes[i] =
        static_cast<std::uint8_t>(static_cast<unsigned char>(random[i]));
  }
  uuid.bytes[6] = static_cast<std::uint8_t>((uuid.bytes[6] & 0x0FU) | 0x40U);
  uuid.bytes[8] = static_cast<std::uint8_t>((uuid.bytes[8] & 0x3FU) | 0x80U);
  *call.out = uuid_value(uuid);
  return SendStatus::Matched;
}

std::string runtime_uuid_to_string(const RuntimeUuidValue &value) {
  std::string out;
  out.reserve(36U);
  for (std::size_t i = 0; i < value.bytes.size(); ++i) {
    if (i == 4U || i == 6U || i == 8U || i == 10U) {
      out.push_back('-');
    }
    const std::uint8_t byte = value.bytes[i];
    out.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[byte & 0x0FU]);
  }
  return out;
}

RuntimeNativeModuleDescriptor uuid_module_descriptor() {
  return {{{"Uuid", RuntimeNativeTypeKind::Uuid},
           {"UUID", RuntimeNativeTypeKind::Uuid}},
          {{RuntimeNativeTypeKind::Uuid, &uuid_dispatch}}};
}

void register_uuid(NativeRegistry &registry) {
  register_native_module_descriptor(registry, uuid_module_descriptor());
}

void register_uuid_runtime_module(RuntimeModuleRegistry &modules,
                                  RuntimeDispatchRegistry &dispatch) {
  const RuntimeNativeModuleDescriptor descriptor = uuid_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
}

} // namespace amber::runtime
