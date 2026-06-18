// Dependency-free digest primitives. Streebog follows RFC 6986.

#include "runtime/digest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace amber::runtime {

namespace {

std::uint32_t load_be32(const unsigned char *bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) |
         static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t load_le64(const unsigned char *bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8U; ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
  }
  return value;
}

void append_be32(std::string *out, std::uint32_t value) {
  out->push_back(static_cast<char>(value >> 24U));
  out->push_back(static_cast<char>(value >> 16U));
  out->push_back(static_cast<char>(value >> 8U));
  out->push_back(static_cast<char>(value));
}

void append_le32(std::string *out, std::uint32_t value) {
  out->push_back(static_cast<char>(value));
  out->push_back(static_cast<char>(value >> 8U));
  out->push_back(static_cast<char>(value >> 16U));
  out->push_back(static_cast<char>(value >> 24U));
}

void append_le64(std::string *out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    out->push_back(static_cast<char>(value >> shift));
  }
}

std::uint32_t rotl32(std::uint32_t value, unsigned count) {
  return (value << count) | (value >> (32U - count));
}

std::uint32_t rotr32(std::uint32_t value, unsigned count) {
  return (value >> count) | (value << (32U - count));
}

std::vector<unsigned char> padded_message(std::string_view bytes,
                                          bool little_length) {
  std::vector<unsigned char> padded(bytes.begin(), bytes.end());
  padded.push_back(0x80U);
  while (padded.size() % 64U != 56U) {
    padded.push_back(0U);
  }
  const std::uint64_t bit_length =
      static_cast<std::uint64_t>(bytes.size()) * 8U;
  for (unsigned i = 0; i < 8U; ++i) {
    const unsigned shift = little_length ? i * 8U : (7U - i) * 8U;
    padded.push_back(static_cast<unsigned char>(bit_length >> shift));
  }
  return padded;
}

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

std::array<std::uint32_t, 8> sha256_words(std::string_view bytes) {
  std::array<std::uint32_t, 8> hash = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  const std::vector<unsigned char> padded = padded_message(bytes, false);
  for (std::size_t offset = 0; offset < padded.size(); offset += 64U) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16U; ++i) {
      words[i] = load_be32(padded.data() + offset + i * 4U);
    }
    for (std::size_t i = 16U; i < words.size(); ++i) {
      const std::uint32_t x = words[i - 15U];
      const std::uint32_t y = words[i - 2U];
      const std::uint32_t small0 = rotr32(x, 7U) ^ rotr32(x, 18U) ^ (x >> 3U);
      const std::uint32_t small1 = rotr32(y, 17U) ^ rotr32(y, 19U) ^ (y >> 10U);
      words[i] = words[i - 16U] + small0 + words[i - 7U] + small1;
    }

    std::uint32_t a = hash[0];
    std::uint32_t b = hash[1];
    std::uint32_t c = hash[2];
    std::uint32_t d = hash[3];
    std::uint32_t e = hash[4];
    std::uint32_t f = hash[5];
    std::uint32_t g = hash[6];
    std::uint32_t h = hash[7];
    for (std::size_t i = 0; i < words.size(); ++i) {
      const std::uint32_t big1 =
          rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
      const std::uint32_t choice = (e & f) ^ (~e & g);
      const std::uint32_t temp1 =
          h + big1 + choice + kSha256RoundConstants[i] + words[i];
      const std::uint32_t big0 =
          rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = big0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }
  return hash;
}

constexpr std::array<std::uint32_t, 64> kMd5RoundConstants = {
    0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU,
    0x4787c62aU, 0xa8304613U, 0xfd469501U, 0x698098d8U, 0x8b44f7afU,
    0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U, 0xa679438eU,
    0x49b40821U, 0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU,
    0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U, 0x21e1cde6U,
    0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U,
    0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U,
    0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
    0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U, 0xd9d4d039U,
    0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U, 0xf4292244U, 0x432aff97U,
    0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU,
    0x85845dd1U, 0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
    0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U,
};

constexpr std::array<unsigned, 64> kMd5Shifts = {
    7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U, 7U, 12U, 17U, 22U,
    5U, 9U,  14U, 20U, 5U, 9U,  14U, 20U, 5U, 9U,  14U, 20U, 5U, 9U,  14U, 20U,
    4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U, 4U, 11U, 16U, 23U,
    6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U, 6U, 10U, 15U, 21U,
};

using StreebogBlock = std::array<std::uint64_t, 8>;

// The Streebog core follows RFC 6986. Its little-endian word layout and table
// orientation match the public-domain-style ISC implementation in RHash
// (Copyright 2019 Aleksey Kravchenko), used here as an implementation
// cross-check. The compact transform below derives table entries from the RFC
// Pi substitution and A matrix instead of embedding the 8x256 lookup table.
constexpr std::array<std::uint8_t, 256> kStreebogPi = {
    252, 238, 221, 17,  207, 110, 49,  22,  251, 196, 250, 218, 35,  197, 4,
    77,  233, 119, 240, 219, 147, 46,  153, 186, 23,  54,  241, 187, 20,  205,
    95,  193, 249, 24,  101, 90,  226, 92,  239, 33,  129, 28,  60,  66,  139,
    1,   142, 79,  5,   132, 2,   174, 227, 106, 143, 160, 6,   11,  237, 152,
    127, 212, 211, 31,  235, 52,  44,  81,  234, 200, 72,  171, 242, 42,  104,
    162, 253, 58,  206, 204, 181, 112, 14,  86,  8,   12,  118, 18,  191, 114,
    19,  71,  156, 183, 93,  135, 21,  161, 150, 41,  16,  123, 154, 199, 243,
    145, 120, 111, 157, 158, 178, 177, 50,  117, 25,  61,  255, 53,  138, 126,
    109, 84,  198, 128, 195, 189, 13,  87,  223, 245, 36,  169, 62,  168, 67,
    201, 215, 121, 214, 246, 124, 34,  185, 3,   224, 15,  236, 222, 122, 148,
    176, 188, 220, 232, 40,  80,  78,  51,  10,  74,  167, 151, 96,  115, 30,
    0,   98,  68,  26,  184, 56,  130, 100, 159, 38,  65,  173, 69,  70,  146,
    39,  94,  85,  47,  140, 163, 165, 125, 105, 213, 149, 59,  7,   88,  179,
    64,  134, 172, 29,  247, 48,  55,  107, 228, 136, 217, 231, 137, 225, 27,
    131, 73,  76,  63,  248, 254, 141, 83,  170, 144, 202, 216, 133, 97,  32,
    113, 103, 164, 45,  43,  9,   91,  203, 155, 37,  208, 190, 229, 108, 82,
    89,  166, 116, 210, 230, 244, 180, 192, 209, 102, 175, 194, 57,  75,  99,
    182,
};

constexpr std::array<std::uint64_t, 64> kStreebogA = {
    0x8e20faa72ba0b470ULL, 0x47107ddd9b505a38ULL, 0xad08b0e0c3282d1cULL,
    0xd8045870ef14980eULL, 0x6c022c38f90a4c07ULL, 0x3601161cf205268dULL,
    0x1b8e0b0e798c13c8ULL, 0x83478b07b2468764ULL, 0xa011d380818e8f40ULL,
    0x5086e740ce47c920ULL, 0x2843fd2067adea10ULL, 0x14aff010bdd87508ULL,
    0x0ad97808d06cb404ULL, 0x05e23c0468365a02ULL, 0x8c711e02341b2d01ULL,
    0x46b60f011a83988eULL, 0x90dab52a387ae76fULL, 0x486dd4151c3dfdb9ULL,
    0x24b86a840e90f0d2ULL, 0x125c354207487869ULL, 0x092e94218d243cbaULL,
    0x8a174a9ec8121e5dULL, 0x4585254f64090fa0ULL, 0xaccc9ca9328a8950ULL,
    0x9d4df05d5f661451ULL, 0xc0a878a0a1330aa6ULL, 0x60543c50de970553ULL,
    0x302a1e286fc58ca7ULL, 0x18150f14b9ec46ddULL, 0x0c84890ad27623e0ULL,
    0x0642ca05693b9f70ULL, 0x0321658cba93c138ULL, 0x86275df09ce8aaa8ULL,
    0x439da0784e745554ULL, 0xafc0503c273aa42aULL, 0xd960281e9d1d5215ULL,
    0xe230140fc0802984ULL, 0x71180a8960409a42ULL, 0xb60c05ca30204d21ULL,
    0x5b068c651810a89eULL, 0x456c34887a3805b9ULL, 0xac361a443d1c8cd2ULL,
    0x561b0d22900e4669ULL, 0x2b838811480723baULL, 0x9bcf4486248d9f5dULL,
    0xc3e9224312c8c1a0ULL, 0xeffa11af0964ee50ULL, 0xf97d86d98a327728ULL,
    0xe4fa2054a80b329cULL, 0x727d102a548b194eULL, 0x39b008152acb8227ULL,
    0x9258048415eb419dULL, 0x492c024284fbaec0ULL, 0xaa16012142f35760ULL,
    0x550b8e9e21f7a530ULL, 0xa48b474f9ef5dc18ULL, 0x70a6a56e2440598eULL,
    0x3853dc371220a247ULL, 0x1ca76e95091051adULL, 0x0edd37c48a08a6d8ULL,
    0x07e095624504536cULL, 0x8d70c431ac02a736ULL, 0xc83862965601dd1bULL,
    0x641c314b2b8ee083ULL,
};

constexpr std::array<StreebogBlock, 12> kStreebogRoundConstants = {{
    {{0xdd806559f2a64507ULL, 0x05767436cc744d23ULL, 0xa2422a08a460d315ULL,
      0x4b7ce09192676901ULL, 0x714eb88d7585c4fcULL, 0x2f6a76432e45d016ULL,
      0xebcb2f81c0657c1fULL, 0xb1085bda1ecadae9ULL}},
    {{0xe679047021b19bb7ULL, 0x55dda21bd7cbcd56ULL, 0x5cb561c2db0aa7caULL,
      0x9ab5176b12d69958ULL, 0x61d55e0f16b50131ULL, 0xf3feea720a232b98ULL,
      0x4fe39d460f70b5d7ULL, 0x6fa3b58aa99d2f1aULL}},
    {{0x991e96f50aba0ab2ULL, 0xc2b6f443867adb31ULL, 0xc1c93a376062db09ULL,
      0xd3e20fe490359eb1ULL, 0xf2ea7514b1297b7bULL, 0x06f15e5f529c1f8bULL,
      0x0a39fc286a3d8435ULL, 0xf574dcac2bce2fc7ULL}},
    {{0x220cbebc84e3d12eULL, 0x3453eaa193e837f1ULL, 0xd8b71333935203beULL,
      0xa9d72c82ed03d675ULL, 0x9d721cad685e353fULL, 0x488e857e335c3c7dULL,
      0xf948e1a05d71e4ddULL, 0xef1fdfb3e81566d2ULL}},
    {{0x601758fd7c6cfe57ULL, 0x7a56a27ea9ea63f5ULL, 0xdfff00b723271a16ULL,
      0xbfcd1747253af5a3ULL, 0x359e35d7800fffbdULL, 0x7f151c1f1686104aULL,
      0x9a3f410c6ca92363ULL, 0x4bea6bacad474799ULL}},
    {{0xfa68407a46647d6eULL, 0xbf71c57236904f35ULL, 0x0af21f66c2bec6b6ULL,
      0xcffaa6b71c9ab7b4ULL, 0x187f9ab49af08ec6ULL, 0x2d66c4f95142a46cULL,
      0x6fa4c33b7a3039c0ULL, 0xae4faeae1d3ad3d9ULL}},
    {{0x8886564d3a14d493ULL, 0x3517454ca23c4af3ULL, 0x06476983284a0504ULL,
      0x0992abc52d822c37ULL, 0xd3473e33197a93c9ULL, 0x399ec6c7e6bf87c9ULL,
      0x51ac86febf240954ULL, 0xf4c70e16eeaac5ecULL}},
    {{0xa47f0dd4bf02e71eULL, 0x36acc2355951a8d9ULL, 0x69d18d2bd1a5c42fULL,
      0xf4892bcb929b0690ULL, 0x89b4443b4ddbc49aULL, 0x4eb7f8719c36de1eULL,
      0x03e7aa020c6e4141ULL, 0x9b1f5b424d93c9a7ULL}},
    {{0x7261445183235adbULL, 0x0e38dc92cb1f2a60ULL, 0x7b2b8a9aa6079c54ULL,
      0x800a440bdbb2ceb1ULL, 0x3cd955b7e00d0984ULL, 0x3a7d3a1b25894224ULL,
      0x944c9ad8ec165fdeULL, 0x378f5a541631229bULL}},
    {{0x74b4c7fb98459cedULL, 0x3698fad1153bb6c3ULL, 0x7a1e6c303b7652f4ULL,
      0x9fe76702af69334bULL, 0x1fffe18a1b336103ULL, 0x8941e71cff8a78dbULL,
      0x382ae548b2e4f3f3ULL, 0xabbedea680056f52ULL}},
    {{0x6bcaa4cd81f32d1bULL, 0xdea2594ac06fd85dULL, 0xefbacd1d7d476e98ULL,
      0x8a1d71efea48b9caULL, 0x2001802114846679ULL, 0xd8fa6bbbebab0761ULL,
      0x3002c6cd635afe94ULL, 0x7bcd9ed0efc889fbULL}},
    {{0x48bc924af11bd720ULL, 0xfaf417d5d9b21b99ULL, 0xe71da4aa88e12852ULL,
      0x5d80ef9d1891cc86ULL, 0xf82012d430219f9bULL, 0xcda43c32bcdf1d77ULL,
      0xd21380b00449b17aULL, 0x378ee767f11631baULL}},
}};

constexpr std::uint64_t streebog_table_value(std::size_t column,
                                             std::uint8_t value) {
  const std::uint8_t substituted = kStreebogPi[value];
  std::uint64_t result = 0;
  const std::size_t matrix_base = (7U - column) * 8U;
  for (std::size_t bit = 0; bit < 8U; ++bit) {
    if ((substituted & (0x80U >> bit)) != 0U) {
      result ^= kStreebogA[matrix_base + bit];
    }
  }
  return result;
}

using StreebogTables = std::array<std::array<std::uint64_t, 256>, 8>;

constexpr StreebogTables make_streebog_tables() {
  StreebogTables tables{};
  for (std::size_t column = 0; column < tables.size(); ++column) {
    for (std::size_t value = 0; value < tables[column].size(); ++value) {
      tables[column][value] =
          streebog_table_value(column, static_cast<std::uint8_t>(value));
    }
  }
  return tables;
}

constexpr StreebogTables kStreebogTables = make_streebog_tables();

StreebogBlock streebog_lps_x(const StreebogBlock &left,
                             const StreebogBlock &right) {
  StreebogBlock mixed{};
  for (std::size_t i = 0; i < mixed.size(); ++i) {
    mixed[i] = left[i] ^ right[i];
  }
  StreebogBlock result{};
  for (std::size_t byte_index = 0; byte_index < 8U; ++byte_index) {
    std::uint64_t word = 0;
    for (std::size_t column = 0; column < 8U; ++column) {
      const std::uint8_t value =
          static_cast<std::uint8_t>(mixed[column] >> (byte_index * 8U));
      word ^= kStreebogTables[column][value];
    }
    result[byte_index] = word;
  }
  return result;
}

void streebog_g(const StreebogBlock &n, StreebogBlock *hash,
                const StreebogBlock &message) {
  StreebogBlock key = streebog_lps_x(*hash, n);
  StreebogBlock state = streebog_lps_x(key, message);
  for (std::size_t round = 0; round < 11U; ++round) {
    key = streebog_lps_x(key, kStreebogRoundConstants[round]);
    state = streebog_lps_x(key, state);
  }
  key = streebog_lps_x(key, kStreebogRoundConstants[11]);
  for (std::size_t i = 0; i < hash->size(); ++i) {
    (*hash)[i] = key[i] ^ state[i] ^ (*hash)[i] ^ message[i];
  }
}

void streebog_add(StreebogBlock *sum, const StreebogBlock &value) {
  std::uint64_t carry = 0;
  for (std::size_t i = 0; i < sum->size(); ++i) {
    const std::uint64_t original = (*sum)[i];
    const std::uint64_t with_value = original + value[i];
    const std::uint64_t carry1 = with_value < original ? 1U : 0U;
    const std::uint64_t with_carry = with_value + carry;
    const std::uint64_t carry2 = with_carry < with_value ? 1U : 0U;
    (*sum)[i] = with_carry;
    carry = carry1 | carry2;
  }
}

StreebogBlock streebog_load_block(const unsigned char *bytes) {
  StreebogBlock block{};
  for (std::size_t i = 0; i < block.size(); ++i) {
    block[i] = load_le64(bytes + i * 8U);
  }
  return block;
}

std::string streebog_hash(std::string_view bytes, std::size_t output_size) {
  StreebogBlock hash{};
  if (output_size == 32U) {
    hash.fill(0x0101010101010101ULL);
  }
  StreebogBlock n{};
  StreebogBlock sum{};
  const StreebogBlock full_block_bits = {512U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};

  std::size_t offset = 0;
  while (bytes.size() - offset >= 64U) {
    const auto *data =
        reinterpret_cast<const unsigned char *>(bytes.data() + offset);
    const StreebogBlock block = streebog_load_block(data);
    streebog_g(n, &hash, block);
    streebog_add(&n, full_block_bits);
    streebog_add(&sum, block);
    offset += 64U;
  }

  std::array<unsigned char, 64> final_bytes{};
  const std::size_t remaining = bytes.size() - offset;
  if (remaining != 0U) {
    std::memcpy(final_bytes.data(), bytes.data() + offset, remaining);
  }
  final_bytes[remaining] = 0x01U;
  const StreebogBlock final_block = streebog_load_block(final_bytes.data());
  streebog_g(n, &hash, final_block);
  StreebogBlock remaining_bits{};
  remaining_bits[0] = static_cast<std::uint64_t>(remaining) * 8U;
  streebog_add(&n, remaining_bits);
  streebog_add(&sum, final_block);
  const StreebogBlock zero{};
  streebog_g(zero, &hash, n);
  streebog_g(zero, &hash, sum);

  std::string out;
  out.reserve(output_size);
  const std::size_t first_word = 8U - output_size / 8U;
  for (std::size_t i = first_word; i < hash.size(); ++i) {
    append_le64(&out, hash[i]);
  }
  return out;
}

} // namespace

std::string digest_crc32(std::string_view bytes) {
  std::uint32_t crc = 0xffffffffU;
  for (unsigned char byte : bytes) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8U; ++bit) {
      const std::uint32_t mask =
          static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1U)));
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  std::string out;
  out.reserve(4U);
  append_be32(&out, crc ^ 0xffffffffU);
  return out;
}

std::string digest_md5(std::string_view bytes) {
  std::array<std::uint32_t, 4> hash = {0x67452301U, 0xefcdab89U, 0x98badcfeU,
                                       0x10325476U};
  const std::vector<unsigned char> padded = padded_message(bytes, true);
  for (std::size_t offset = 0; offset < padded.size(); offset += 64U) {
    std::array<std::uint32_t, 16> words{};
    for (std::size_t i = 0; i < words.size(); ++i) {
      const unsigned char *word = padded.data() + offset + i * 4U;
      words[i] = static_cast<std::uint32_t>(word[0]) |
                 (static_cast<std::uint32_t>(word[1]) << 8U) |
                 (static_cast<std::uint32_t>(word[2]) << 16U) |
                 (static_cast<std::uint32_t>(word[3]) << 24U);
    }
    std::uint32_t a = hash[0];
    std::uint32_t b = hash[1];
    std::uint32_t c = hash[2];
    std::uint32_t d = hash[3];
    for (std::size_t i = 0; i < 64U; ++i) {
      std::uint32_t f = 0;
      std::size_t word_index = 0;
      if (i < 16U) {
        f = (b & c) | (~b & d);
        word_index = i;
      } else if (i < 32U) {
        f = (d & b) | (~d & c);
        word_index = (5U * i + 1U) % 16U;
      } else if (i < 48U) {
        f = b ^ c ^ d;
        word_index = (3U * i + 5U) % 16U;
      } else {
        f = c ^ (b | ~d);
        word_index = (7U * i) % 16U;
      }
      const std::uint32_t next =
          b + rotl32(a + f + kMd5RoundConstants[i] + words[word_index],
                     kMd5Shifts[i]);
      a = d;
      d = c;
      c = b;
      b = next;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
  }
  std::string out;
  out.reserve(16U);
  for (std::uint32_t word : hash) {
    append_le32(&out, word);
  }
  return out;
}

std::string digest_sha1(std::string_view bytes) {
  std::array<std::uint32_t, 5> hash = {0x67452301U, 0xefcdab89U, 0x98badcfeU,
                                       0x10325476U, 0xc3d2e1f0U};
  const std::vector<unsigned char> padded = padded_message(bytes, false);
  for (std::size_t offset = 0; offset < padded.size(); offset += 64U) {
    std::array<std::uint32_t, 80> words{};
    for (std::size_t i = 0; i < 16U; ++i) {
      words[i] = load_be32(padded.data() + offset + i * 4U);
    }
    for (std::size_t i = 16U; i < words.size(); ++i) {
      words[i] = rotl32(
          words[i - 3U] ^ words[i - 8U] ^ words[i - 14U] ^ words[i - 16U], 1U);
    }
    std::uint32_t a = hash[0];
    std::uint32_t b = hash[1];
    std::uint32_t c = hash[2];
    std::uint32_t d = hash[3];
    std::uint32_t e = hash[4];
    for (std::size_t i = 0; i < words.size(); ++i) {
      std::uint32_t f = 0;
      std::uint32_t constant = 0;
      if (i < 20U) {
        f = (b & c) | (~b & d);
        constant = 0x5a827999U;
      } else if (i < 40U) {
        f = b ^ c ^ d;
        constant = 0x6ed9eba1U;
      } else if (i < 60U) {
        f = (b & c) | (b & d) | (c & d);
        constant = 0x8f1bbcdcU;
      } else {
        f = b ^ c ^ d;
        constant = 0xca62c1d6U;
      }
      const std::uint32_t temp = rotl32(a, 5U) + f + e + constant + words[i];
      e = d;
      d = c;
      c = rotl32(b, 30U);
      b = a;
      a = temp;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
  }
  std::string out;
  out.reserve(20U);
  for (std::uint32_t word : hash) {
    append_be32(&out, word);
  }
  return out;
}

std::string digest_sha256(std::string_view bytes) {
  const std::array<std::uint32_t, 8> hash = sha256_words(bytes);
  std::string out;
  out.reserve(32U);
  for (std::uint32_t word : hash) {
    append_be32(&out, word);
  }
  return out;
}

std::string digest_hmac_sha256(std::string_view key, std::string_view bytes) {
  std::string normalized_key(key);
  if (normalized_key.size() > 64U) {
    normalized_key = digest_sha256(normalized_key);
  }
  normalized_key.resize(64U, '\0');
  std::string inner_pad(64U, '\0');
  std::string outer_pad(64U, '\0');
  for (std::size_t i = 0; i < 64U; ++i) {
    const unsigned char byte = static_cast<unsigned char>(normalized_key[i]);
    inner_pad[i] = static_cast<char>(byte ^ 0x36U);
    outer_pad[i] = static_cast<char>(byte ^ 0x5cU);
  }
  std::string inner = inner_pad;
  inner.append(bytes.data(), bytes.size());
  const std::string inner_hash = digest_sha256(inner);
  std::string outer = outer_pad;
  outer.append(inner_hash);
  return digest_sha256(outer);
}

std::string digest_streebog256(std::string_view bytes) {
  return streebog_hash(bytes, 32U);
}

std::string digest_streebog512(std::string_view bytes) {
  return streebog_hash(bytes, 64U);
}

} // namespace amber::runtime
