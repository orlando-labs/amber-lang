#include <cstdint>
#include <iostream>
#include <string>

namespace {

const char *kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char *kBase64Url =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
const char *kHex = "0123456789abcdef";

std::string hex_encode(const std::string &bytes) {
  std::string out;
  out.reserve(bytes.size() * 2U);
  for (unsigned char byte : bytes) {
    out.push_back(kHex[(byte >> 4U) & 0x0FU]);
    out.push_back(kHex[byte & 0x0FU]);
  }
  return out;
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

std::string hex_decode(const std::string &text) {
  std::string out;
  out.reserve(text.size() / 2U);
  for (std::size_t i = 0; i + 1U < text.size(); i += 2U) {
    out.push_back(static_cast<char>((hex_value(text[i]) << 4U) |
                                    hex_value(text[i + 1U])));
  }
  return out;
}

std::string b64_encode(const std::string &bytes, bool url, bool padding) {
  const char *alphabet = url ? kBase64Url : kBase64;
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

int b64_value(char c, bool url) {
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
    return c == '-' ? 62 : (c == '_' ? 63 : -1);
  }
  return c == '+' ? 62 : (c == '/' ? 63 : -1);
}

std::string b64_decode(std::string text, bool url, bool lenient) {
  std::string normalized;
  normalized.reserve(text.size() + 4U);
  for (char c : text) {
    if (lenient && (c == ' ' || c == '\n' || c == '\r' || c == '\t')) {
      continue;
    }
    normalized.push_back(c);
  }
  const std::size_t residue = normalized.size() % 4U;
  if (residue != 0U) {
    normalized.append(4U - residue, '=');
  }
  std::string out;
  out.reserve((normalized.size() / 4U) * 3U);
  for (std::size_t i = 0; i < normalized.size(); i += 4U) {
    const int v0 = b64_value(normalized[i], url);
    const int v1 = b64_value(normalized[i + 1U], url);
    const char c2 = normalized[i + 2U];
    const char c3 = normalized[i + 3U];
    const int v2 = c2 == '=' ? 0 : b64_value(c2, url);
    const int v3 = c3 == '=' ? 0 : b64_value(c3, url);
    out.push_back(static_cast<char>((v0 << 2U) | (v1 >> 4U)));
    if (c2 != '=') {
      out.push_back(static_cast<char>(((v1 & 0x0FU) << 4U) | (v2 >> 2U)));
    }
    if (c3 != '=') {
      out.push_back(static_cast<char>(((v2 & 0x03U) << 6U) | v3));
    }
  }
  return out;
}

} // namespace

int main() {
  std::int64_t checksum = 0;
  for (int i = 0; i < 5000; ++i) {
    const std::string raw =
        "event-" + std::to_string(i) + ":" + std::to_string((i * 17) % 100000);

    const std::string b64 = b64_encode(raw, false, true);
    const std::string roundtrip = b64_decode(b64, false, false);
    checksum += static_cast<std::int64_t>(roundtrip.size());
    checksum += static_cast<unsigned char>(roundtrip.front()) +
                static_cast<unsigned char>(roundtrip.back());

    const std::string hx = hex_encode(raw);
    const std::string url = b64_encode(hex_decode(hx), true, false);
    const std::string back = b64_decode(url, true, false);
    checksum += static_cast<std::int64_t>(back.size());
    checksum += static_cast<unsigned char>(back[1]);

    const std::string decoded = hex_decode(hex_encode(back));
    checksum += static_cast<unsigned char>(decoded[2]);

    if (i % 16 == 0) {
      checksum += static_cast<std::int64_t>(
          b64_decode(b64 + "\n", false, true).size());
    }
    if (i % 31 == 0) {
      checksum += static_cast<std::int64_t>(
          b64_decode(b64_encode(raw, true, true), true, false).size());
    }
    if (i % 17 == 0) {
      checksum += static_cast<unsigned char>(hex_decode(hx)[0]);
    }
  }
  std::cout << checksum << "\n";
  return 0;
}
