#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
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

constexpr int kRounds = 5000;
constexpr char kHex[] = "0123456789abcdef";
constexpr char kFixed[] = "550e8400-e29b-41d4-a716-446655440000";

struct Uuid {
  std::array<std::uint8_t, 16> bytes{};

  static Uuid v4();
  static Uuid v7();
  static Uuid parse(const std::string &text);

  std::string to_string() const;
  std::string inspect() const { return to_string(); }
  std::string to_json() const { return "\"" + to_string() + "\""; }
  int version() const { return (bytes[6] >> 4U) & 0x0FU; }

  bool operator==(const Uuid &other) const { return bytes == other.bytes; }
};

void secure_random_fill(std::uint8_t *out, std::size_t count) {
  if (count == 0U) {
    return;
  }
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||      \
    defined(__NetBSD__)
  arc4random_buf(out, count);
#elif defined(__linux__)
  std::size_t remaining = count;
  while (remaining > 0U) {
    const ssize_t got = getrandom(out, remaining, 0);
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
    out += got;
    remaining -= static_cast<std::size_t>(got);
  }
#else
  throw std::runtime_error("secure random is not supported on this platform");
#endif
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

Uuid Uuid::v4() {
  Uuid out;
  secure_random_fill(out.bytes.data(), out.bytes.size());
  out.bytes[6] = static_cast<std::uint8_t>((out.bytes[6] & 0x0FU) | 0x40U);
  out.bytes[8] = static_cast<std::uint8_t>((out.bytes[8] & 0x3FU) | 0x80U);
  return out;
}

Uuid Uuid::v7() {
  Uuid out;
  secure_random_fill(out.bytes.data(), out.bytes.size());
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const std::uint64_t milliseconds = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
  for (std::size_t index = 0; index < 6U; ++index) {
    out.bytes[index] =
        static_cast<std::uint8_t>(milliseconds >> ((5U - index) * 8U));
  }
  out.bytes[6] = static_cast<std::uint8_t>((out.bytes[6] & 0x0FU) | 0x70U);
  out.bytes[8] = static_cast<std::uint8_t>((out.bytes[8] & 0x3FU) | 0x80U);
  return out;
}

Uuid Uuid::parse(const std::string &text) {
  if (text.size() != 36U || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') {
    throw std::runtime_error("invalid UUID");
  }
  Uuid out;
  std::size_t byte_index = 0;
  for (std::size_t index = 0; index < text.size();) {
    if (text[index] == '-') {
      ++index;
      continue;
    }
    const int high = hex_value(text[index]);
    const int low = hex_value(text[index + 1U]);
    if (high < 0 || low < 0 || byte_index >= out.bytes.size()) {
      throw std::runtime_error("invalid UUID");
    }
    out.bytes[byte_index++] = static_cast<std::uint8_t>((high << 4U) | low);
    index += 2U;
  }
  if (byte_index != out.bytes.size()) {
    throw std::runtime_error("invalid UUID");
  }
  return out;
}

std::string Uuid::to_string() const {
  std::string out;
  out.reserve(36U);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4U || index == 6U || index == 8U || index == 10U) {
      out.push_back('-');
    }
    out.push_back(kHex[(bytes[index] >> 4U) & 0x0FU]);
    out.push_back(kHex[bytes[index] & 0x0FU]);
  }
  return out;
}

Uuid secure_random_uuid() { return Uuid::v4(); }

} // namespace

int main() {
  std::int64_t checksum = 0;
  for (int i = 0; i < kRounds; ++i) {
    const Uuid v4 = Uuid::v4();
    const std::string v4_text = v4.to_string();
    const Uuid parsed_v4 = Uuid::parse(v4_text);

    const Uuid v7 = Uuid::v7();
    const std::string v7_text = v7.inspect();
    const Uuid parsed_v7 = Uuid::parse(v7_text);

    const Uuid delegated = secure_random_uuid();
    const std::string delegated_json = delegated.to_json();

    const Uuid fixed = Uuid::parse("550E8400-E29B-41D4-A716-446655440000");
    const std::string fixed_text = fixed.to_string();

    checksum += v4.version();
    checksum += v7.version();
    checksum += delegated.version();
    checksum += static_cast<std::int64_t>(v4_text.size());
    checksum += static_cast<std::int64_t>(v7_text.size());
    checksum += static_cast<std::int64_t>(delegated_json.size());

    if (v4 == parsed_v4) {
      checksum += 11;
    }
    if (v7 == parsed_v7) {
      checksum += 13;
    }
    checksum += 17;
    if (fixed.version() == 4 && fixed_text == kFixed) {
      checksum += 19;
    }
    if (v4_text.find('-') != std::string::npos &&
        v7_text.find('-') != std::string::npos) {
      checksum += 23;
    }
  }
  std::cout << checksum << "\n";
  return 0;
}
