// Binary codecs stdlib library (DESIGN-stdlib-next-libs-order-2026-06-15 §4.2).
//
// v1 surface:
//   Base64.encode(bytes, padding: true) / Base64.decode(text, mode: :strict)
//   Base64Url.encode(bytes, padding: false) / Base64Url.decode(...)
//   Hex.encode(bytes) / Hex.decode(text, mode: :strict)
//
// `Encoding` intentionally remains free for future text transcoding APIs.

#include "runtime/stdlib_registry.h"

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace amber::runtime {

namespace {

enum class DecodeMode { Strict, Lenient };
enum class CodecKind { Base64, Base64Url, Hex };

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kBase64UrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr char kHexDigits[] = "0123456789abcdef";

bool ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

SendStatus decode_fault(NativeStdlibCall &call, const std::string &message) {
  return call.fault("CodecDecodeError", message);
}

std::optional<std::string> required_string_arg(NativeStdlibCall &call,
                                               const std::string &context) {
  if (!call.args[0].is_string()) {
    call.fault("TypeError", context + " expects a Str");
    return std::nullopt;
  }
  const std::optional<std::string> text = call.text_of(call.args[0]);
  if (!text.has_value()) {
    call.fault("VMError", "string ref is invalid");
    return std::nullopt;
  }
  return text;
}

bool mode_keyword(NativeStdlibCall &call, DecodeMode *mode) {
  *mode = DecodeMode::Strict;
  const std::optional<Value> value = call.keyword("mode");
  if (!value.has_value()) {
    return true;
  }
  const std::optional<std::string> text = call.text_of(*value);
  if (!text.has_value()) {
    call.fault("TypeError", "mode must be Symbol or Str");
    return false;
  }
  if (*text == "strict") {
    *mode = DecodeMode::Strict;
    return true;
  }
  if (*text == "lenient") {
    *mode = DecodeMode::Lenient;
    return true;
  }
  call.fault("ArgumentError", "mode must be :strict or :lenient");
  return false;
}

int base64_value(char c, bool url) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return 26 + c - 'a';
  }
  if (c >= '0' && c <= '9') {
    return 52 + c - '0';
  }
  if (url) {
    if (c == '-') {
      return 62;
    }
    if (c == '_') {
      return 63;
    }
  } else {
    if (c == '+') {
      return 62;
    }
    if (c == '/') {
      return 63;
    }
  }
  return -1;
}

std::string base64_encode_bytes(const std::string &bytes, bool url,
                                bool padding) {
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

bool base64_decode_text(NativeStdlibCall &call, const std::string &text,
                        bool url, DecodeMode mode, std::string *out) {
  std::string normalized;
  normalized.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (ascii_space(c)) {
      if (mode == DecodeMode::Lenient) {
        continue;
      }
      decode_fault(call, "base64 input contains whitespace at offset " +
                             std::to_string(i));
      return false;
    }
    if (c == '=' || base64_value(c, url) >= 0) {
      normalized.push_back(c);
      continue;
    }
    decode_fault(call, "base64 input contains invalid character at offset " +
                           std::to_string(i));
    return false;
  }

  const std::size_t first_pad = normalized.find('=');
  if (first_pad != std::string::npos) {
    for (std::size_t i = first_pad; i < normalized.size(); ++i) {
      if (normalized[i] != '=') {
        decode_fault(call, "base64 padding must be trailing");
        return false;
      }
    }
    const std::size_t pad_count = normalized.size() - first_pad;
    if (pad_count > 2U || normalized.size() % 4U != 0U) {
      decode_fault(call, "base64 input has malformed padding");
      return false;
    }
  } else {
    const std::size_t residue = normalized.size() % 4U;
    if (residue == 1U) {
      decode_fault(call, "base64 input has impossible length");
      return false;
    }
    if (residue != 0U) {
      normalized.append(4U - residue, '=');
    }
  }

  out->clear();
  out->reserve((normalized.size() / 4U) * 3U);
  for (std::size_t i = 0; i < normalized.size(); i += 4U) {
    const char c0 = normalized[i];
    const char c1 = normalized[i + 1U];
    const char c2 = normalized[i + 2U];
    const char c3 = normalized[i + 3U];
    const bool last = i + 4U == normalized.size();
    if (c0 == '=' || c1 == '=' || (c2 == '=' && c3 != '=') ||
        ((c2 == '=' || c3 == '=') && !last)) {
      decode_fault(call, "base64 input has malformed padding");
      return false;
    }
    const int v0 = base64_value(c0, url);
    const int v1 = base64_value(c1, url);
    const int v2 = c2 == '=' ? 0 : base64_value(c2, url);
    const int v3 = c3 == '=' ? 0 : base64_value(c3, url);
    if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
      decode_fault(call, "base64 input contains invalid character");
      return false;
    }
    if (c2 == '=') {
      if ((v1 & 0x0F) != 0) {
        decode_fault(call, "base64 input has non-zero padding bits");
        return false;
      }
      out->push_back(static_cast<char>((v0 << 2U) | (v1 >> 4U)));
    } else if (c3 == '=') {
      if ((v2 & 0x03) != 0) {
        decode_fault(call, "base64 input has non-zero padding bits");
        return false;
      }
      out->push_back(static_cast<char>((v0 << 2U) | (v1 >> 4U)));
      out->push_back(static_cast<char>(((v1 & 0x0FU) << 4U) | (v2 >> 2U)));
    } else {
      out->push_back(static_cast<char>((v0 << 2U) | (v1 >> 4U)));
      out->push_back(static_cast<char>(((v1 & 0x0FU) << 4U) | (v2 >> 2U)));
      out->push_back(static_cast<char>(((v2 & 0x03U) << 6U) | v3));
    }
  }
  return true;
}

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

std::string hex_encode_bytes(const std::string &bytes) {
  std::string out;
  out.reserve(bytes.size() * 2U);
  for (unsigned char byte : bytes) {
    out.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[byte & 0x0FU]);
  }
  return out;
}

bool hex_decode_text(NativeStdlibCall &call, const std::string &text,
                     DecodeMode mode, std::string *out) {
  std::string normalized;
  normalized.reserve(text.size());
  bool allow_prefix = true;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (mode == DecodeMode::Lenient &&
        (ascii_space(c) || c == ':' || c == '-')) {
      continue;
    }
    if (mode == DecodeMode::Lenient && allow_prefix && c == '0' &&
        i + 1U < text.size() && (text[i + 1U] == 'x' || text[i + 1U] == 'X')) {
      allow_prefix = false;
      ++i;
      continue;
    }
    const int digit = hex_value(c);
    if (digit < 0) {
      decode_fault(call, "hex input contains invalid character at offset " +
                             std::to_string(i));
      return false;
    }
    allow_prefix = false;
    normalized.push_back(kHexDigits[digit]);
  }
  if (normalized.size() % 2U != 0U) {
    decode_fault(call, "hex input has odd length");
    return false;
  }
  out->clear();
  out->reserve(normalized.size() / 2U);
  for (std::size_t i = 0; i < normalized.size(); i += 2U) {
    const int hi = hex_value(normalized[i]);
    const int lo = hex_value(normalized[i + 1U]);
    out->push_back(static_cast<char>((hi << 4U) | lo));
  }
  return true;
}

SendStatus codec_encode(NativeStdlibCall &call, CodecKind kind) {
  if (!call.require_no_block() || !call.require_arity(1)) {
    return SendStatus::Faulted;
  }
  if (kind == CodecKind::Hex) {
    if (!call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
  } else if (!call.reject_unknown_keywords({"padding"})) {
    return SendStatus::Faulted;
  }
  const std::optional<std::string> bytes = call.bytes_of(call.args[0]);
  if (!bytes.has_value()) {
    return SendStatus::Faulted;
  }
  std::string encoded;
  if (kind == CodecKind::Hex) {
    encoded = hex_encode_bytes(*bytes);
  } else {
    const bool url = kind == CodecKind::Base64Url;
    bool padding = kind == CodecKind::Base64;
    if (!call.bool_keyword("padding", padding, &padding)) {
      return SendStatus::Faulted;
    }
    encoded = base64_encode_bytes(*bytes, url, padding);
  }
  *call.out = call.string_value(std::move(encoded));
  return SendStatus::Matched;
}

SendStatus codec_decode(NativeStdlibCall &call, CodecKind kind) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({"mode"})) {
    return SendStatus::Faulted;
  }
  DecodeMode mode = DecodeMode::Strict;
  if (!mode_keyword(call, &mode)) {
    return SendStatus::Faulted;
  }
  const std::string context = kind == CodecKind::Hex
                                  ? "Hex.decode"
                                  : (kind == CodecKind::Base64Url
                                         ? "Base64Url.decode"
                                         : "Base64.decode");
  const std::optional<std::string> text = required_string_arg(call, context);
  if (!text.has_value()) {
    return SendStatus::Faulted;
  }
  std::string decoded;
  const bool ok =
      kind == CodecKind::Hex
          ? hex_decode_text(call, *text, mode, &decoded)
          : base64_decode_text(call, *text, kind == CodecKind::Base64Url, mode,
                               &decoded);
  if (!ok) {
    return SendStatus::Faulted;
  }
  *call.out = call.bytes_value(std::move(decoded));
  return SendStatus::Matched;
}

SendStatus codec_dispatch(NativeStdlibCall &call) {
  CodecKind kind = CodecKind::Hex;
  if (call.kind == RuntimeNativeTypeKind::Base64) {
    kind = CodecKind::Base64;
  } else if (call.kind == RuntimeNativeTypeKind::Base64Url) {
    kind = CodecKind::Base64Url;
  } else if (call.kind == RuntimeNativeTypeKind::Hex) {
    kind = CodecKind::Hex;
  } else {
    return SendStatus::NotHandled;
  }

  if (call.selector == "encode") {
    return codec_encode(call, kind);
  }
  if (call.selector == "decode") {
    return codec_decode(call, kind);
  }
  return SendStatus::NotHandled;
}

} // namespace

void register_codecs(NativeRegistry &registry) {
  registry.register_path("Base64", RuntimeNativeTypeKind::Base64);
  registry.register_path("Base64Url", RuntimeNativeTypeKind::Base64Url);
  registry.register_path("Hex", RuntimeNativeTypeKind::Hex);
  registry.register_handler(RuntimeNativeTypeKind::Base64, &codec_dispatch);
  registry.register_handler(RuntimeNativeTypeKind::Base64Url, &codec_dispatch);
  registry.register_handler(RuntimeNativeTypeKind::Hex, &codec_dispatch);
}

} // namespace amber::runtime
