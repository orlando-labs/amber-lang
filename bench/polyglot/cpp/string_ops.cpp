#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string upcase(const std::string &text) {
  std::string out = text;
  for (char &c : out) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 32);
    }
  }
  return out;
}

std::string downcase(const std::string &text) {
  std::string out = text;
  for (char &c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return out;
}

std::vector<std::string> split(const std::string &text, const std::string &sep) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t pos = text.find(sep, start);
    if (pos == std::string::npos) {
      parts.push_back(text.substr(start));
      break;
    }
    parts.push_back(text.substr(start, pos - start));
    start = pos + sep.size();
  }
  return parts;
}

std::string replace_all(const std::string &text, const std::string &from,
                        const std::string &to) {
  std::string out;
  std::size_t start = 0;
  while (true) {
    const std::size_t pos = text.find(from, start);
    if (pos == std::string::npos) {
      out += text.substr(start);
      break;
    }
    out += text.substr(start, pos - start);
    out += to;
    start = pos + from.size();
  }
  return out;
}

std::string trim(const std::string &text) {
  const auto is_ws = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
  };
  std::size_t a = 0;
  std::size_t b = text.size();
  while (a < b && is_ws(static_cast<unsigned char>(text[a]))) {
    ++a;
  }
  while (b > a && is_ws(static_cast<unsigned char>(text[b - 1]))) {
    --b;
  }
  return text.substr(a, b - a);
}

bool starts_with(const std::string &text, const std::string &prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string &text, const std::string &suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

int main() {
  std::int64_t checksum = 0;
  for (int i = 0; i < 4000; ++i) {
    const std::string token = "user-" + std::to_string(i) + "-record";
    const std::string upper = upcase(token);
    const std::string lower = downcase(upper);
    const std::string line =
        lower + "|" + token + "|segment-" + std::to_string(i % 97);
    const std::vector<std::string> parts = split(line, "|");
    const std::string rebuilt = parts[0] + ";" + parts[1] + ";" + parts[2];
    const std::string replaced = replace_all(rebuilt, "user-", "member-");
    const std::string padded = "  " + replaced + "  ";
    const std::string trimmed = trim(padded);
    checksum += static_cast<std::int64_t>(trimmed.size()) +
                static_cast<std::int64_t>(parts.size());
    if (trimmed.find("member-") != std::string::npos) {
      checksum += 3;
    }
    if (starts_with(trimmed, "member-")) {
      checksum += 7;
    }
    if (ends_with(trimmed, "7")) {
      checksum += 11;
    }
    if (i % 5 == 0) {
      checksum += static_cast<std::int64_t>(replace_all(line, "e", "E").size());
    }
  }
  std::cout << checksum << "\n";
  return 0;
}
