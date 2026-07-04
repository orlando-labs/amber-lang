#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>

int main() {
  std::unordered_map<std::string, std::int64_t> m;
  std::int64_t i = 0;
  while (i < 30000) {
    const std::string key = "w" + std::to_string((i * i + i / 3) % 2000);
    const auto found = m.find(key);
    if (found != m.end()) {
      found->second += 1;
    } else {
      m.emplace(key, 1);
    }
    i += 1;
  }
  std::int64_t checksum = 0;
  for (const auto &entry : m) {
    checksum += entry.second * static_cast<std::int64_t>(entry.first.size());
  }
  std::int64_t hits = 0;
  std::int64_t j = 0;
  while (j < 10000) {
    const std::string probe = "w" + std::to_string((j * 7) % 3000);
    const auto found = m.find(probe);
    if (found != m.end()) {
      hits += found->second;
    }
    j += 1;
  }
  std::cout << checksum + hits + static_cast<std::int64_t>(m.size()) << "\n";
  return 0;
}
