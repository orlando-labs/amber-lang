#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#if defined(__linux__)
#include <sys/random.h>
#include <sys/types.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||    \
    defined(__NetBSD__)
#include <stdlib.h>
#endif

namespace {

constexpr int kRounds = 2000;
constexpr char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kBase64Url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr char kHex[] = "0123456789abcdef";

std::string secure_random_bytes(std::size_t count) {
  std::string out(count, '\0');
  if (count == 0U) {
    return out;
  }
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||      \
    defined(__NetBSD__)
  arc4random_buf(&out[0], out.size());
  return out;
#elif defined(__linux__)
  char *cursor = &out[0];
  std::size_t remaining = out.size();
  while (remaining > 0U) {
    const ssize_t got = getrandom(cursor, remaining, 0);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("getrandom failed: ") +
                               std::strerror(errno));
    }
    if (got == 0) {
      throw std::runtime_error("getrandom returned no entropy");
    }
    cursor += got;
    remaining -= static_cast<std::size_t>(got);
  }
  return out;
#else
  throw std::runtime_error("secure random is not supported on this platform");
#endif
}

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

std::string base64_encode(const std::string &bytes, bool url, bool padding) {
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
    return c == '-' ? 62 : (c == '_' ? 63 : -1);
  }
  return c == '+' ? 62 : (c == '/' ? 63 : -1);
}

std::string base64_decode(std::string text, bool url) {
  const std::size_t residue = text.size() % 4U;
  if (residue != 0U) {
    text.append(4U - residue, '=');
  }
  std::string out;
  out.reserve((text.size() / 4U) * 3U);
  for (std::size_t i = 0; i < text.size(); i += 4U) {
    const int v0 = base64_value(text[i], url);
    const int v1 = base64_value(text[i + 1U], url);
    const char c2 = text[i + 2U];
    const char c3 = text[i + 3U];
    const int v2 = c2 == '=' ? 0 : base64_value(c2, url);
    const int v3 = c3 == '=' ? 0 : base64_value(c3, url);
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

std::uint64_t random_u64() {
  const std::string bytes = secure_random_bytes(8U);
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8U; ++i) {
    value |= static_cast<std::uint64_t>(
                 static_cast<unsigned char>(bytes[i]))
             << (i * 8U);
  }
  return value;
}

std::int64_t secure_random_int(std::int64_t min, std::int64_t max) {
  const std::uint64_t count =
      static_cast<std::uint64_t>(max - min) + 1U;
  const std::uint64_t threshold =
      (std::numeric_limits<std::uint64_t>::max() - count + 1U) % count;
  while (true) {
    const std::uint64_t sample = random_u64();
    if (sample >= threshold) {
      return min + static_cast<std::int64_t>(sample % count);
    }
  }
}

std::string uuid_v4() {
  std::string bytes = secure_random_bytes(16U);
  bytes[6] = static_cast<char>((static_cast<unsigned char>(bytes[6]) & 0x0FU) |
                               0x40U);
  bytes[8] = static_cast<char>((static_cast<unsigned char>(bytes[8]) & 0x3FU) |
                               0x80U);
  const std::string hex = hex_encode(bytes);
  std::string out;
  out.reserve(36U);
  out.append(hex, 0U, 8U);
  out.push_back('-');
  out.append(hex, 8U, 4U);
  out.push_back('-');
  out.append(hex, 12U, 4U);
  out.push_back('-');
  out.append(hex, 16U, 4U);
  out.push_back('-');
  out.append(hex, 20U, 12U);
  return out;
}

} // namespace

int main() {
  std::int64_t checksum = 0;
  for (int i = 0; i < kRounds; ++i) {
    checksum += static_cast<std::int64_t>(secure_random_bytes(32U).size());
    checksum += static_cast<std::int64_t>(
        hex_decode(hex_encode(secure_random_bytes(16U))).size());
    checksum += static_cast<std::int64_t>(
        base64_decode(base64_encode(secure_random_bytes(18U), false, true),
                      false)
            .size());
    checksum += static_cast<std::int64_t>(
        base64_decode(base64_encode(secure_random_bytes(17U), false, false),
                      false)
            .size());
    checksum += static_cast<std::int64_t>(
        base64_decode(base64_encode(secure_random_bytes(18U), true, false),
                      true)
            .size());

    const std::string id = uuid_v4();
    if (id.size() == 36U && id.find('-') != std::string::npos) {
      checksum += 37;
    }

    const std::int64_t value = secure_random_int(100, 999);
    if (value >= 100 && value <= 999) {
      checksum += 10;
    }
  }
  std::cout << checksum << "\n";
  return 0;
}
