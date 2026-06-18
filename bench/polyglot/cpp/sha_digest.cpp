#include "runtime/digest.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr std::array<std::string_view, 4> kPayloads = {
    "Amber digest polyglot benchmark payload zero",
    "Amber digest polyglot benchmark payload one 1234567890",
    "The quick brown fox jumps over the lazy dog",
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
};
constexpr std::string_view kKey = "amber-digest-benchmark-key";

std::uint64_t fold_digest(std::uint64_t checksum, const std::string &digest) {
  return checksum + digest.size() + static_cast<unsigned char>(digest.front()) +
         static_cast<unsigned char>(digest.back());
}

std::uint64_t main_workload() {
  std::uint64_t checksum = 0;
  for (std::size_t i = 0; i < 4000U; ++i) {
    const std::string_view data = kPayloads[i % kPayloads.size()];
    checksum = fold_digest(checksum, amber::runtime::digest_crc32(data));
    checksum = fold_digest(checksum, amber::runtime::digest_md5(data));
    checksum = fold_digest(checksum, amber::runtime::digest_sha1(data));
    checksum = fold_digest(checksum, amber::runtime::digest_sha256(data));
    checksum =
        fold_digest(checksum, amber::runtime::digest_hmac_sha256(kKey, data));
  }
  return checksum;
}

} // namespace

int main() {
  std::cout << main_workload() << '\n';
  return 0;
}
