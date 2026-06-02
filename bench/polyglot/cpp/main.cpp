#include <cstdint>
#include <iostream>

namespace {

constexpr std::int64_t kIterations = 1000000;
constexpr std::int64_t kLimit = 2147483647;

std::int64_t main_workload() {
  std::int64_t i = 0;
  std::int64_t a = 1;
  std::int64_t b = 2;
  std::int64_t checksum = 0;
  while (i < kIterations) {
    a = a + b + 3;
    if (a > kLimit) {
      a = a - kLimit;
    }
    b = b + a + i;
    if (b > kLimit) {
      b = b - kLimit;
    }
    if (a > b) {
      checksum = checksum + a - b;
    } else {
      checksum = checksum + b - a;
    }
    i = i + 1;
  }
  return checksum + a + b;
}

} // namespace

int main() {
  std::cout << main_workload() << '\n';
  return 0;
}
