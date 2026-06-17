#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr const char *kJsonPath = "bench/polyglot/build/json/events.jsonl";

std::int64_t parse_int_at(const std::string &text, std::size_t &pos) {
  while (pos < text.size() && text[pos] != '-' &&
         !std::isdigit(static_cast<unsigned char>(text[pos]))) {
    ++pos;
  }
  if (pos >= text.size()) {
    throw std::runtime_error("expected integer");
  }
  bool negative = false;
  if (text[pos] == '-') {
    negative = true;
    ++pos;
  }
  std::int64_t value = 0;
  while (pos < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[pos]))) {
    value = value * 10 + (text[pos] - '0');
    ++pos;
  }
  return negative ? -value : value;
}

std::int64_t read_after(const std::string &text, const std::string &needle) {
  std::size_t pos = text.find(needle);
  if (pos == std::string::npos) {
    throw std::runtime_error("missing JSON field");
  }
  pos += needle.size();
  return parse_int_at(text, pos);
}

std::int64_t read_item(const std::string &text, int index) {
  std::size_t pos = text.find("\"items\":[");
  if (pos == std::string::npos) {
    throw std::runtime_error("missing JSON items");
  }
  pos += 9;
  std::int64_t value = 0;
  for (int i = 0; i <= index; ++i) {
    value = parse_int_at(text, pos);
    if (i < index) {
      pos = text.find(',', pos);
      if (pos == std::string::npos) {
        throw std::runtime_error("missing JSON item separator");
      }
      ++pos;
    }
  }
  return value;
}

std::string make_document(int i) {
  return "{\"id\":" + std::to_string(i) +
         ",\"value\":" + std::to_string(i * 2) +
         ",\"group\":" + std::to_string(i % 7) +
         ",\"items\":[" + std::to_string(i) + "," +
         std::to_string(i + 1) + "," + std::to_string(i + 2) +
         "],\"name\":\"event\"}";
}

} // namespace

int main() {
  std::int64_t checksum = 0;
  for (int i = 0; i < 1000; ++i) {
    const std::string compact = make_document(i);
    checksum += read_after(compact, "\"id\":");
    checksum += read_after(compact, "\"value\":");
    checksum += read_after(compact, "\"group\":");
    checksum += read_item(compact, 1);
    if (i < 20) {
      checksum += read_item(compact, 2);
    }
  }

  std::ifstream input(kJsonPath);
  if (!input) {
    std::cerr << "failed to open " << kJsonPath << "\n";
    return 1;
  }
  std::string line;
  std::int64_t count = 0;
  while (std::getline(input, line)) {
    checksum += read_after(line, "\"id\":") * 3;
    checksum += read_after(line, "\"value\":");
    checksum -= read_after(line, "\"group\":");
    ++count;
  }

  std::cout << (checksum + count) << "\n";
  return 0;
}
