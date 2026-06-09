#include <array>
#include <cstdint>
#include <iostream>

namespace {

constexpr std::uint32_t kRounds = 600;

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

std::uint32_t rotr(std::uint32_t value, std::uint32_t count) {
  return (value >> count) | (value << (32U - count));
}

std::uint32_t ch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
  return (x & y) ^ (~x & z);
}

std::uint32_t maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}

std::uint32_t big0(std::uint32_t x) {
  return rotr(x, 2U) ^ rotr(x, 13U) ^ rotr(x, 22U);
}

std::uint32_t big1(std::uint32_t x) {
  return rotr(x, 6U) ^ rotr(x, 11U) ^ rotr(x, 25U);
}

std::uint32_t small0(std::uint32_t x) {
  return rotr(x, 7U) ^ rotr(x, 18U) ^ (x >> 3U);
}

std::uint32_t small1(std::uint32_t x) {
  return rotr(x, 17U) ^ rotr(x, 19U) ^ (x >> 10U);
}

void compress(std::array<std::uint32_t, 8> &digest,
              const std::array<std::uint32_t, 16> &block) {
  std::array<std::uint32_t, 64> w{};
  for (std::uint32_t i = 0; i < 16U; ++i) {
    w[i] = block[i];
  }
  for (std::uint32_t i = 16U; i < 64U; ++i) {
    w[i] = w[i - 16U] + small0(w[i - 15U]) + w[i - 7U] +
           small1(w[i - 2U]);
  }

  std::uint32_t a = digest[0];
  std::uint32_t b = digest[1];
  std::uint32_t c = digest[2];
  std::uint32_t d = digest[3];
  std::uint32_t e = digest[4];
  std::uint32_t f = digest[5];
  std::uint32_t g = digest[6];
  std::uint32_t h = digest[7];

  for (std::uint32_t i = 0; i < 64U; ++i) {
    const std::uint32_t t1 =
        h + big1(e) + ch(e, f, g) + kRoundConstants[i] + w[i];
    const std::uint32_t t2 = big0(a) + maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  digest[0] += a;
  digest[1] += b;
  digest[2] += c;
  digest[3] += d;
  digest[4] += e;
  digest[5] += f;
  digest[6] += g;
  digest[7] += h;
}

void mix_block(std::array<std::uint32_t, 16> &block,
               std::uint32_t round_index) {
  std::uint32_t carry = (round_index + 1U) * 0x9e3779b1U;
  for (std::uint32_t i = 0; i < 16U; ++i) {
    const std::uint32_t left = block[(i + 1U) % 16U];
    const std::uint32_t right = block[(i + 9U) % 16U];
    block[i] = block[i] + carry + (left ^ right) +
               ((i + 17U) * (round_index + 3U));
    carry = rotr(carry ^ block[i], (i % 13U) + 1U);
  }
}

std::uint32_t fold_digest(const std::array<std::uint32_t, 8> &digest) {
  std::uint32_t folded = 0;
  for (std::uint32_t i = 0; i < 8U; ++i) {
    folded = (folded ^ digest[i]) + ((i + 1U) * 0x9e3779b1U);
  }
  return folded;
}

std::uint32_t main_workload() {
  std::array<std::uint32_t, 8> digest = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  std::array<std::uint32_t, 16> block = {
      0x416d6265U, 0x72205348U, 0x41206469U, 0x67657374U,
      0x20706f6cU, 0x79676c6fU, 0x74206265U, 0x6e636820U,
      0x76310000U, 0U,          0U,          0U,
      0U,          0U,          0U,          0x00000120U,
  };
  for (std::uint32_t round_index = 0; round_index < kRounds; ++round_index) {
    compress(digest, block);
    mix_block(block, round_index);
  }
  return fold_digest(digest);
}

} // namespace

int main() {
  std::cout << main_workload() << '\n';
  return 0;
}
