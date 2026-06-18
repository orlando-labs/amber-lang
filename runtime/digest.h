#pragma once

#include <string>
#include <string_view>

namespace amber::runtime {

// Dependency-free one-shot digest primitives shared by the VM stdlib handler,
// direct-native executables, and benchmark/reference tools. Results are raw
// bytes in conventional display order.
std::string digest_crc32(std::string_view bytes);
std::string digest_md5(std::string_view bytes);
std::string digest_sha1(std::string_view bytes);
std::string digest_sha256(std::string_view bytes);
std::string digest_hmac_sha256(std::string_view key, std::string_view bytes);
std::string digest_streebog256(std::string_view bytes);
std::string digest_streebog512(std::string_view bytes);

} // namespace amber::runtime
