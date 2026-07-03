// Time stdlib library (DESIGN-stdlib-next-libs-order-2026-06-15 §4.5).
//
// v2 surface extension:
//   TimeZone fixed-offset values and UTC/Moscow lookup aliases
//   Time values carry a display/resolution zone
//   Time#in_time_zone/#in_tz and Time#as_time_zone/#as_tz are pure
//   Time#start_of_* / #end_of_* local calendar helpers
//   Time.parse(format:, zone:) and Time#to_str(format)

#include "runtime/stdlib_registry.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <unistd.h>

namespace amber::runtime {

namespace {

constexpr std::int64_t kNanosPerSecond = 1000000000LL;
constexpr std::int64_t kSecondsPerDay = 86400LL;

struct CivilDate {
  std::int64_t year = 1970;
  int month = 1;
  int day = 1;
};

struct LocalFields {
  CivilDate date;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int nanosecond = 0;
};

struct ParsedDateTime {
  LocalFields fields;
  bool has_offset = false;
  int offset_seconds = 0;
  std::string offset_name = "UTC";
  bool offset_fixed = true;
};

enum class ParseZoneMode { Utc, Parsed, Explicit };

struct ParseZone {
  ParseZoneMode mode = ParseZoneMode::Utc;
  RuntimeTimeZoneValue zone;
};

enum class GapPolicy { Raise, Forward, Backward };
enum class FoldPolicy { Earlier, Later, Raise };

struct ResolvePolicy {
  GapPolicy gap = GapPolicy::Raise;
  FoldPolicy fold = FoldPolicy::Earlier;
};

struct ZoneTypeInfo {
  int offset_seconds = 0;
  bool is_dst = false;
  std::string abbreviation = "UTC";
};

struct ZoneTransition {
  std::int64_t utc_seconds = 0;
  std::uint8_t type_index = 0;
};

struct PosixDateRule {
  int month = 0;
  int week = 0;
  int weekday = 0;
  int seconds = 2 * 3600;
};

struct PosixRule {
  std::string std_abbreviation;
  int std_offset = 0;
  std::string dst_abbreviation;
  int dst_offset = 0;
  PosixDateRule start;
  PosixDateRule end;
  bool has_dst = false;
};

struct ZoneData {
  std::string name;
  std::vector<ZoneTransition> transitions;
  std::vector<ZoneTypeInfo> types;
  std::size_t default_type = 0;
  std::optional<PosixRule> future_rule;
};

struct ZoneInstantInfo {
  int offset_seconds = 0;
  bool is_dst = false;
  std::string abbreviation = "UTC";
};

std::optional<std::int64_t> checked_i128(__int128 value) {
  if (value < static_cast<__int128>(std::numeric_limits<std::int64_t>::min()) ||
      value > static_cast<__int128>(std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(value);
}

std::int64_t floor_div_i64(std::int64_t a, std::int64_t b) {
  std::int64_t q = a / b;
  const std::int64_t r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) {
    --q;
  }
  return q;
}

__int128 floor_div_i128(__int128 a, __int128 b) {
  __int128 q = a / b;
  const __int128 r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) {
    --q;
  }
  return q;
}

int floor_mod_i64(std::int64_t a, std::int64_t b) {
  return static_cast<int>(a - floor_div_i64(a, b) * b);
}

std::int64_t days_from_civil(std::int64_t y, int m, int d) {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned mp = static_cast<unsigned>(m + (m > 2 ? -3 : 9));
  const unsigned doy = (153U * mp + 2U) / 5U + static_cast<unsigned>(d) - 1U;
  const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

CivilDate civil_from_days(std::int64_t z) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe =
      (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
  std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
  const unsigned mp = (5U * doy + 2U) / 153U;
  const unsigned d = doy - (153U * mp + 2U) / 5U + 1U;
  const int m = static_cast<int>(mp) + (mp < 10U ? 3 : -9);
  y += m <= 2 ? 1 : 0;
  return CivilDate{y, m, static_cast<int>(d)};
}

bool leap_year(std::int64_t year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int days_in_month(std::int64_t year, int month) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
  if (month == 2 && leap_year(year)) {
    return 29;
  }
  return kDays[month - 1];
}

bool valid_date(std::int64_t year, int month, int day) {
  return month >= 1 && month <= 12 && day >= 1 &&
         day <= days_in_month(year, month);
}

std::int64_t local_seconds_from_fields(const LocalFields &fields) {
  return days_from_civil(fields.date.year, fields.date.month,
                         fields.date.day) *
             kSecondsPerDay +
         fields.hour * 3600 + fields.minute * 60 + fields.second;
}

bool same_local_fields(const LocalFields &lhs, const LocalFields &rhs) {
  return lhs.date.year == rhs.date.year && lhs.date.month == rhs.date.month &&
         lhs.date.day == rhs.date.day && lhs.hour == rhs.hour &&
         lhs.minute == rhs.minute && lhs.second == rhs.second &&
         lhs.nanosecond == rhs.nanosecond;
}

RuntimeTimeZoneValue utc_zone() { return RuntimeTimeZoneValue{0, "UTC", true}; }

std::string two_digits(int value) {
  std::ostringstream out;
  out << std::setw(2) << std::setfill('0') << value;
  return out.str();
}

std::string four_digits(std::int64_t value) {
  std::ostringstream out;
  out << std::setw(4) << std::setfill('0') << value;
  return out.str();
}

std::string offset_name(int offset_seconds) {
  if (offset_seconds == 0) {
    return "UTC";
  }
  const char sign = offset_seconds < 0 ? '-' : '+';
  int total = offset_seconds < 0 ? -offset_seconds : offset_seconds;
  const int hour = total / 3600;
  const int minute = (total / 60) % 60;
  std::ostringstream out;
  out << sign << std::setw(2) << std::setfill('0') << hour << ":"
      << std::setw(2) << minute;
  return out.str();
}

std::string offset_text(int offset_seconds, bool colon, bool z_for_utc) {
  if (offset_seconds == 0 && z_for_utc) {
    return "Z";
  }
  const char sign = offset_seconds < 0 ? '-' : '+';
  int total = offset_seconds < 0 ? -offset_seconds : offset_seconds;
  const int hour = total / 3600;
  const int minute = (total / 60) % 60;
  std::ostringstream out;
  out << sign << std::setw(2) << std::setfill('0') << hour;
  if (colon) {
    out << ":";
  }
  out << std::setw(2) << minute;
  return out.str();
}

std::string ascii_lower(std::string text) {
  for (char &c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

std::string compact_zone_key(const std::string &text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9')) {
      out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a')
                                          : c);
    }
  }
  return out;
}

std::string trim_ascii_space(std::string text) {
  auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
  };
  while (!text.empty() && is_space(text.back())) {
    text.pop_back();
  }
  std::size_t start = 0;
  while (start < text.size() && is_space(text[start])) {
    ++start;
  }
  return start == 0 ? text : text.substr(start);
}

bool parse_fixed_digits(const std::string &text, std::size_t offset,
                        std::size_t count, int *out) {
  if (offset + count > text.size()) {
    return false;
  }
  int value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[offset + i];
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  *out = value;
  return true;
}

bool parse_variable_digits(const std::string &text, std::size_t *pos,
                           std::size_t min_count, std::size_t max_count,
                           int *out) {
  int value = 0;
  std::size_t count = 0;
  while (*pos < text.size() && count < max_count) {
    const char c = text[*pos];
    if (c < '0' || c > '9') {
      break;
    }
    value = value * 10 + (c - '0');
    ++*pos;
    ++count;
  }
  if (count < min_count) {
    return false;
  }
  *out = value;
  return true;
}

bool parse_year4(const std::string &text, std::size_t offset,
                 std::int64_t *out) {
  int year = 0;
  if (!parse_fixed_digits(text, offset, 4, &year)) {
    return false;
  }
  *out = year;
  return true;
}

bool parse_offset_text_at(const std::string &text, std::size_t *pos,
                          int *offset_seconds, std::string *name) {
  if (*pos >= text.size()) {
    return false;
  }
  if (text[*pos] == 'Z' || text[*pos] == 'z') {
    ++*pos;
    *offset_seconds = 0;
    *name = "UTC";
    return true;
  }
  if (text[*pos] != '+' && text[*pos] != '-') {
    return false;
  }
  const int sign = text[*pos] == '-' ? -1 : 1;
  ++*pos;
  int hour = 0;
  int minute = 0;
  if (!parse_fixed_digits(text, *pos, 2, &hour)) {
    return false;
  }
  *pos += 2;
  if (*pos < text.size() && text[*pos] == ':') {
    ++*pos;
  }
  if (!parse_fixed_digits(text, *pos, 2, &minute)) {
    return false;
  }
  *pos += 2;
  if (hour > 23 || minute > 59) {
    return false;
  }
  *offset_seconds = sign * (hour * 3600 + minute * 60);
  *name = offset_name(*offset_seconds);
  return true;
}

std::optional<RuntimeTimeZoneValue>
parse_fixed_offset_zone(const std::string &text) {
  if (text == "Z" || text == "z") {
    return utc_zone();
  }
  if (text.empty() || (text[0] != '+' && text[0] != '-')) {
    return std::nullopt;
  }
  std::size_t pos = 0;
  int offset = 0;
  std::string name;
  if (!parse_offset_text_at(text, &pos, &offset, &name) || pos != text.size()) {
    return std::nullopt;
  }
  return RuntimeTimeZoneValue{offset, name, true};
}

bool valid_iana_zone_name(const std::string &name) {
  if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos) {
    return false;
  }
  for (char c : name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                    c == '+' || c == '/';
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> read_binary_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string path_dirname(const std::string &path) {
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return path.substr(0, slash);
}

std::optional<std::string> executable_dirname() {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) {
    return std::nullopt;
  }
  std::string path(size, '\0');
  if (_NSGetExecutablePath(path.data(), &size) != 0) {
    return std::nullopt;
  }
  const std::size_t end = path.find('\0');
  if (end != std::string::npos) {
    path.resize(end);
  }
  return path_dirname(path);
#elif defined(__linux__)
  std::array<char, 4096> buffer{};
  const ssize_t count =
      readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
  if (count <= 0) {
    return std::nullopt;
  }
  return path_dirname(
      std::string(buffer.data(), static_cast<std::size_t>(count)));
#else
  return std::nullopt;
#endif
}

void push_unique_root(std::vector<std::string> *roots, std::string root) {
  if (root.empty()) {
    return;
  }
  while (root.size() > 1U && root.back() == '/') {
    root.pop_back();
  }
  if (std::find(roots->begin(), roots->end(), root) == roots->end()) {
    roots->push_back(std::move(root));
  }
}

std::vector<std::string> zoneinfo_roots() {
  std::vector<std::string> roots;
  if (const char *env = std::getenv("AMBER_TZDB_DIR")) {
    push_unique_root(&roots, env);
  }
  if (const std::optional<std::string> exe_dir = executable_dirname()) {
    push_unique_root(&roots, *exe_dir + "/../third_party/tzdb/zoneinfo");
    push_unique_root(&roots, *exe_dir + "/third_party/tzdb/zoneinfo");
  }
  push_unique_root(&roots, "third_party/tzdb/zoneinfo");
  push_unique_root(&roots, "../third_party/tzdb/zoneinfo");
  push_unique_root(&roots, "/usr/share/zoneinfo");
  push_unique_root(&roots, "/var/db/timezone/zoneinfo");
  push_unique_root(&roots, "/usr/share/lib/zoneinfo");
  push_unique_root(&roots, "/etc/zoneinfo");
  return roots;
}

std::optional<std::string> zoneinfo_file_for_name(const std::string &name) {
  if (!valid_iana_zone_name(name)) {
    return std::nullopt;
  }
  const std::vector<std::string> roots = zoneinfo_roots();
  for (const std::string &root : roots) {
    const std::string path = root + "/" + name;
    if (read_binary_file(path).has_value()) {
      return path;
    }
  }
  return std::nullopt;
}

std::uint32_t read_be32u(const std::string &data, std::size_t pos) {
  return (static_cast<std::uint32_t>(
              static_cast<unsigned char>(data[pos])) << 24U) |
         (static_cast<std::uint32_t>(
              static_cast<unsigned char>(data[pos + 1U])) << 16U) |
         (static_cast<std::uint32_t>(
              static_cast<unsigned char>(data[pos + 2U])) << 8U) |
         static_cast<std::uint32_t>(
             static_cast<unsigned char>(data[pos + 3U]));
}

std::int32_t read_be32s(const std::string &data, std::size_t pos) {
  return static_cast<std::int32_t>(read_be32u(data, pos));
}

std::int64_t read_be64s(const std::string &data, std::size_t pos) {
  std::uint64_t out = 0;
  for (std::size_t i = 0; i < 8U; ++i) {
    out = (out << 8U) |
          static_cast<unsigned char>(data[pos + i]);
  }
  return static_cast<std::int64_t>(out);
}

struct TzifCounts {
  std::uint32_t ttisgmt = 0;
  std::uint32_t ttisstd = 0;
  std::uint32_t leap = 0;
  std::uint32_t time = 0;
  std::uint32_t type = 0;
  std::uint32_t chars = 0;
};

bool parse_tzif_header(const std::string &data, std::size_t pos,
                       char *version, TzifCounts *counts) {
  if (pos + 44U > data.size() || data.compare(pos, 4U, "TZif") != 0) {
    return false;
  }
  *version = data[pos + 4U];
  counts->ttisgmt = read_be32u(data, pos + 20U);
  counts->ttisstd = read_be32u(data, pos + 24U);
  counts->leap = read_be32u(data, pos + 28U);
  counts->time = read_be32u(data, pos + 32U);
  counts->type = read_be32u(data, pos + 36U);
  counts->chars = read_be32u(data, pos + 40U);
  return counts->type > 0;
}

std::size_t tzif_block_size(const TzifCounts &counts, int time_size) {
  return static_cast<std::size_t>(counts.time) * time_size +
         static_cast<std::size_t>(counts.time) +
         static_cast<std::size_t>(counts.type) * 6U + counts.chars +
         static_cast<std::size_t>(counts.leap) * (time_size + 4U) +
         counts.ttisstd + counts.ttisgmt;
}

std::optional<ZoneData> parse_tzif_block(const std::string &data,
                                         std::size_t *pos,
                                         const TzifCounts &counts,
                                         int time_size,
                                         const std::string &name) {
  if (*pos + tzif_block_size(counts, time_size) > data.size()) {
    return std::nullopt;
  }
  ZoneData zone;
  zone.name = name;
  zone.transitions.reserve(counts.time);
  std::vector<std::int64_t> transition_times;
  transition_times.reserve(counts.time);
  for (std::uint32_t i = 0; i < counts.time; ++i) {
    transition_times.push_back(time_size == 8 ? read_be64s(data, *pos)
                                               : read_be32s(data, *pos));
    *pos += static_cast<std::size_t>(time_size);
  }
  std::vector<std::uint8_t> transition_types;
  transition_types.reserve(counts.time);
  for (std::uint32_t i = 0; i < counts.time; ++i) {
    transition_types.push_back(
        static_cast<std::uint8_t>(static_cast<unsigned char>(data[*pos])));
    ++*pos;
  }
  struct RawType {
    int offset = 0;
    bool is_dst = false;
    std::uint8_t abbr_index = 0;
  };
  std::vector<RawType> raw_types;
  raw_types.reserve(counts.type);
  for (std::uint32_t i = 0; i < counts.type; ++i) {
    RawType type;
    type.offset = read_be32s(data, *pos);
    *pos += 4U;
    type.is_dst = data[*pos] != '\0';
    ++*pos;
    type.abbr_index =
        static_cast<std::uint8_t>(static_cast<unsigned char>(data[*pos]));
    ++*pos;
    raw_types.push_back(type);
  }
  const std::string abbreviations = data.substr(*pos, counts.chars);
  *pos += counts.chars;
  zone.types.reserve(raw_types.size());
  for (const RawType &raw : raw_types) {
    std::string abbreviation;
    if (raw.abbr_index < abbreviations.size()) {
      for (std::size_t i = raw.abbr_index; i < abbreviations.size() &&
                                          abbreviations[i] != '\0';
           ++i) {
        abbreviation.push_back(abbreviations[i]);
      }
    }
    if (abbreviation.empty()) {
      abbreviation = offset_name(raw.offset);
    }
    zone.types.push_back(ZoneTypeInfo{raw.offset, raw.is_dst, abbreviation});
  }
  for (std::uint32_t i = 0; i < counts.time; ++i) {
    if (transition_types[i] >= zone.types.size()) {
      return std::nullopt;
    }
    zone.transitions.push_back(
        ZoneTransition{transition_times[i], transition_types[i]});
  }
  for (std::size_t i = 0; i < zone.types.size(); ++i) {
    if (!zone.types[i].is_dst) {
      zone.default_type = i;
      break;
    }
  }
  *pos += static_cast<std::size_t>(counts.leap) * (time_size + 4U);
  *pos += counts.ttisstd;
  *pos += counts.ttisgmt;
  return zone;
}

bool parse_posix_name(const std::string &text, std::size_t *pos,
                      std::string *out) {
  out->clear();
  if (*pos >= text.size()) {
    return false;
  }
  if (text[*pos] == '<') {
    ++*pos;
    while (*pos < text.size() && text[*pos] != '>') {
      out->push_back(text[*pos]);
      ++*pos;
    }
    if (*pos >= text.size() || text[*pos] != '>') {
      return false;
    }
    ++*pos;
    return !out->empty();
  }
  while (*pos < text.size() &&
         ((text[*pos] >= 'A' && text[*pos] <= 'Z') ||
          (text[*pos] >= 'a' && text[*pos] <= 'z'))) {
    out->push_back(text[*pos]);
    ++*pos;
  }
  return out->size() >= 3U;
}

bool parse_posix_offset(const std::string &text, std::size_t *pos,
                        int *offset) {
  int sign = 1;
  if (*pos < text.size() && (text[*pos] == '+' || text[*pos] == '-')) {
    sign = text[*pos] == '-' ? -1 : 1;
    ++*pos;
  }
  int hour = 0;
  if (!parse_variable_digits(text, pos, 1, 2, &hour)) {
    return false;
  }
  int minute = 0;
  int second = 0;
  if (*pos < text.size() && text[*pos] == ':') {
    ++*pos;
    if (!parse_variable_digits(text, pos, 1, 2, &minute)) {
      return false;
    }
    if (*pos < text.size() && text[*pos] == ':') {
      ++*pos;
      if (!parse_variable_digits(text, pos, 1, 2, &second)) {
        return false;
      }
    }
  }
  if (hour > 24 || minute > 59 || second > 59) {
    return false;
  }
  // POSIX spells the amount to add to local time to get UTC. The runtime stores
  // the usual UTC offset to add to UTC to get local time, so invert it.
  *offset = -sign * (hour * 3600 + minute * 60 + second);
  return true;
}

bool parse_posix_date_rule(const std::string &text, std::size_t *pos,
                           PosixDateRule *rule) {
  if (*pos >= text.size() || text[*pos] != 'M') {
    return false;
  }
  ++*pos;
  if (!parse_variable_digits(text, pos, 1, 2, &rule->month) ||
      *pos >= text.size() || text[*pos] != '.') {
    return false;
  }
  ++*pos;
  if (!parse_variable_digits(text, pos, 1, 1, &rule->week) ||
      *pos >= text.size() || text[*pos] != '.') {
    return false;
  }
  ++*pos;
  if (!parse_variable_digits(text, pos, 1, 1, &rule->weekday)) {
    return false;
  }
  if (rule->month < 1 || rule->month > 12 || rule->week < 1 ||
      rule->week > 5 || rule->weekday < 0 || rule->weekday > 6) {
    return false;
  }
  if (*pos < text.size() && text[*pos] == '/') {
    ++*pos;
    int seconds = 0;
    int sign = 1;
    if (*pos < text.size() && (text[*pos] == '+' || text[*pos] == '-')) {
      sign = text[*pos] == '-' ? -1 : 1;
      ++*pos;
    }
    int hour = 0;
    if (!parse_variable_digits(text, pos, 1, 2, &hour)) {
      return false;
    }
    seconds = hour * 3600;
    if (*pos < text.size() && text[*pos] == ':') {
      ++*pos;
      int minute = 0;
      if (!parse_variable_digits(text, pos, 1, 2, &minute)) {
        return false;
      }
      seconds += minute * 60;
      if (*pos < text.size() && text[*pos] == ':') {
        ++*pos;
        int second = 0;
        if (!parse_variable_digits(text, pos, 1, 2, &second)) {
          return false;
        }
        seconds += second;
      }
    }
    rule->seconds = sign * seconds;
  }
  return true;
}

std::optional<PosixRule> parse_posix_rule(const std::string &text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::size_t pos = 0;
  PosixRule rule;
  if (!parse_posix_name(text, &pos, &rule.std_abbreviation) ||
      !parse_posix_offset(text, &pos, &rule.std_offset)) {
    return std::nullopt;
  }
  if (pos >= text.size()) {
    return rule;
  }
  if (!parse_posix_name(text, &pos, &rule.dst_abbreviation)) {
    return std::nullopt;
  }
  rule.has_dst = true;
  if (pos < text.size() && text[pos] != ',') {
    if (!parse_posix_offset(text, &pos, &rule.dst_offset)) {
      return std::nullopt;
    }
  } else {
    rule.dst_offset = rule.std_offset + 3600;
  }
  if (pos >= text.size() || text[pos] != ',') {
    return std::nullopt;
  }
  ++pos;
  if (!parse_posix_date_rule(text, &pos, &rule.start) ||
      pos >= text.size() || text[pos] != ',') {
    return std::nullopt;
  }
  ++pos;
  if (!parse_posix_date_rule(text, &pos, &rule.end) || pos != text.size()) {
    return std::nullopt;
  }
  return rule;
}

std::int64_t posix_rule_local_seconds(std::int64_t year,
                                      const PosixDateRule &rule) {
  const std::int64_t first_day = days_from_civil(year, rule.month, 1);
  const int first_weekday = floor_mod_i64(first_day + 4, 7); // Sunday == 0.
  int day = 1 + floor_mod_i64(rule.weekday - first_weekday, 7) +
            (rule.week - 1) * 7;
  const int dim = days_in_month(year, rule.month);
  if (day > dim) {
    day -= 7;
  }
  return days_from_civil(year, rule.month, day) * kSecondsPerDay +
         rule.seconds;
}

struct PosixYearTransitions {
  std::int64_t start_utc = 0;
  std::int64_t end_utc = 0;
};

PosixYearTransitions posix_year_transitions(std::int64_t year,
                                            const PosixRule &rule) {
  const std::int64_t start_local =
      posix_rule_local_seconds(year, rule.start);
  const std::int64_t end_local = posix_rule_local_seconds(year, rule.end);
  return PosixYearTransitions{start_local - rule.std_offset,
                              end_local - rule.dst_offset};
}

std::optional<ZoneInstantInfo> posix_info_at(const PosixRule &rule,
                                             std::int64_t utc_seconds) {
  if (!rule.has_dst) {
    return ZoneInstantInfo{rule.std_offset, false, rule.std_abbreviation};
  }
  const CivilDate approx =
      civil_from_days(floor_div_i64(utc_seconds, kSecondsPerDay));
  const PosixYearTransitions trans =
      posix_year_transitions(approx.year, rule);
  bool in_dst = false;
  if (trans.start_utc < trans.end_utc) {
    in_dst = utc_seconds >= trans.start_utc && utc_seconds < trans.end_utc;
  } else {
    in_dst = utc_seconds >= trans.start_utc || utc_seconds < trans.end_utc;
  }
  return in_dst ? ZoneInstantInfo{rule.dst_offset, true,
                                  rule.dst_abbreviation}
                : ZoneInstantInfo{rule.std_offset, false,
                                  rule.std_abbreviation};
}

std::shared_ptr<const ZoneData> load_zone_data_uncached(
    const std::string &name) {
  const std::optional<std::string> path = zoneinfo_file_for_name(name);
  if (!path.has_value()) {
    return nullptr;
  }
  const std::optional<std::string> data = read_binary_file(*path);
  if (!data.has_value()) {
    return nullptr;
  }
  char version = '\0';
  TzifCounts counts;
  if (!parse_tzif_header(*data, 0, &version, &counts)) {
    return nullptr;
  }
  std::size_t pos = 44U;
  std::optional<ZoneData> zone;
  if (version == '2' || version == '3' || version == '4') {
    pos += tzif_block_size(counts, 4);
    TzifCounts counts64;
    char version64 = '\0';
    if (!parse_tzif_header(*data, pos, &version64, &counts64)) {
      return nullptr;
    }
    pos += 44U;
    zone = parse_tzif_block(*data, &pos, counts64, 8, name);
  } else {
    zone = parse_tzif_block(*data, &pos, counts, 4, name);
  }
  if (!zone.has_value() || zone->types.empty()) {
    return nullptr;
  }
  if (pos < data->size() && (*data)[pos] == '\n') {
    ++pos;
    const std::size_t start = pos;
    while (pos < data->size() && (*data)[pos] != '\n') {
      ++pos;
    }
    zone->future_rule = parse_posix_rule(data->substr(start, pos - start));
  }
  return std::make_shared<ZoneData>(std::move(*zone));
}

std::shared_ptr<const ZoneData> zone_data_for_name(const std::string &name) {
  static std::mutex mutex;
  static std::unordered_map<std::string, std::shared_ptr<const ZoneData>> cache;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = cache.find(name);
    if (it != cache.end()) {
      return it->second;
    }
  }
  std::shared_ptr<const ZoneData> loaded = load_zone_data_uncached(name);
  std::lock_guard<std::mutex> lock(mutex);
  cache.emplace(name, loaded);
  return loaded;
}

ZoneInstantInfo zone_info_from_data(const ZoneData &data,
                                    std::int64_t utc_seconds) {
  if (!data.transitions.empty() && data.future_rule.has_value() &&
      utc_seconds > data.transitions.back().utc_seconds) {
    if (const std::optional<ZoneInstantInfo> info =
            posix_info_at(*data.future_rule, utc_seconds)) {
      return *info;
    }
  }
  std::size_t type_index = data.default_type;
  const auto it = std::upper_bound(
      data.transitions.begin(), data.transitions.end(), utc_seconds,
      [](std::int64_t value, const ZoneTransition &transition) {
        return value < transition.utc_seconds;
      });
  if (it != data.transitions.begin()) {
    type_index = static_cast<std::size_t>((it - 1)->type_index);
  }
  if (type_index >= data.types.size()) {
    type_index = data.default_type;
  }
  const ZoneTypeInfo &type = data.types[type_index];
  return ZoneInstantInfo{type.offset_seconds, type.is_dst,
                         type.abbreviation};
}

ZoneInstantInfo zone_info_at(const RuntimeTimeZoneValue &zone,
                             std::int64_t utc_seconds) {
  if (zone.fixed_offset) {
    return ZoneInstantInfo{zone.offset_seconds, false,
                           zone.name == "UTC" ? "UTC" : zone.name};
  }
  const std::shared_ptr<const ZoneData> data = zone_data_for_name(zone.name);
  if (data == nullptr) {
    return ZoneInstantInfo{zone.offset_seconds, false, zone.name};
  }
  return zone_info_from_data(*data, utc_seconds);
}

std::optional<RuntimeTimeZoneValue> lookup_time_zone(const std::string &text);

std::optional<std::string> local_zone_name() {
  if (const char *tz = std::getenv("TZ")) {
    std::string text = tz;
    if (!text.empty() && text[0] != ':' && lookup_time_zone(text).has_value()) {
      return text;
    }
    if (text.size() > 1U && text[0] == ':' &&
        lookup_time_zone(text.substr(1)).has_value()) {
      return text.substr(1);
    }
  }
  std::array<char, 4096> buffer{};
  const ssize_t count =
      readlink("/etc/localtime", buffer.data(), buffer.size() - 1U);
  if (count > 0) {
    std::string path(buffer.data(), static_cast<std::size_t>(count));
    const std::string marker = "/zoneinfo/";
    const std::size_t pos = path.rfind(marker);
    if (pos != std::string::npos) {
      const std::string name = path.substr(pos + marker.size());
      if (lookup_time_zone(name).has_value()) {
        return name;
      }
    }
  }
  if (const std::optional<std::string> text = read_binary_file("/etc/timezone")) {
    const std::string name = trim_ascii_space(*text);
    if (!name.empty() && lookup_time_zone(name).has_value()) {
      return name;
    }
  }
  return std::nullopt;
}

std::optional<RuntimeTimeZoneValue> lookup_time_zone(const std::string &text) {
  const std::string key = compact_zone_key(text);
  if (key == "utc" || key == "gmt" || key == "z") {
    return utc_zone();
  }
  if (key == "msk" || text == "МСК" || text == "мск") {
    const std::string name = "Europe/Moscow";
    if (const std::shared_ptr<const ZoneData> data = zone_data_for_name(name)) {
      const ZoneInstantInfo epoch = zone_info_from_data(*data, 0);
      return RuntimeTimeZoneValue{epoch.offset_seconds, name, false};
    }
    return RuntimeTimeZoneValue{3 * 3600, name, false};
  }
  if (const std::optional<RuntimeTimeZoneValue> fixed =
          parse_fixed_offset_zone(text)) {
    return fixed;
  }
  if (valid_iana_zone_name(text) && zone_data_for_name(text) != nullptr) {
    const ZoneInstantInfo epoch =
        zone_info_from_data(*zone_data_for_name(text), 0);
    return RuntimeTimeZoneValue{epoch.offset_seconds, text, false};
  }
  return std::nullopt;
}

std::optional<RuntimeTimeValue>
make_time_from_parts(__int128 seconds, __int128 nanosecond,
                     const RuntimeTimeZoneValue &zone = utc_zone()) {
  const __int128 second_delta = floor_div_i128(nanosecond, kNanosPerSecond);
  __int128 normalized_nano =
      nanosecond - second_delta * static_cast<__int128>(kNanosPerSecond);
  seconds += second_delta;
  const std::optional<std::int64_t> checked_seconds = checked_i128(seconds);
  if (!checked_seconds.has_value()) {
    return std::nullopt;
  }
  RuntimeTimeValue out;
  out.epoch_seconds = *checked_seconds;
  out.nanosecond = static_cast<std::uint32_t>(normalized_nano);
  out.zone_offset_seconds =
      zone_info_at(zone, out.epoch_seconds).offset_seconds;
  out.zone_name = zone.name;
  out.zone_fixed_offset = zone.fixed_offset;
  return out;
}

Value time_value(RuntimeTimeValue time) {
  return Value::time(std::make_shared<RuntimeTimeValue>(std::move(time)));
}

Value zone_value(RuntimeTimeZoneValue zone) {
  return Value::time_zone(
      std::make_shared<RuntimeTimeZoneValue>(std::move(zone)));
}

Value period_value(RuntimeTimePeriodValue period) {
  return Value::time_period(std::make_shared<RuntimeTimePeriodValue>(period));
}

RuntimeTimeZoneValue zone_of(const RuntimeTimeValue &time) {
  return RuntimeTimeZoneValue{time.zone_offset_seconds, time.zone_name,
                              time.zone_fixed_offset};
}

ZoneInstantInfo zone_info_for_time(const RuntimeTimeValue &time) {
  return zone_info_at(zone_of(time), time.epoch_seconds);
}

LocalFields local_fields(const RuntimeTimeValue &time) {
  const RuntimeTimeZoneValue zone = zone_of(time);
  const int offset = zone_info_at(zone, time.epoch_seconds).offset_seconds;
  const std::int64_t local_seconds = time.epoch_seconds + offset;
  const std::int64_t days = floor_div_i64(local_seconds, kSecondsPerDay);
  const int second_of_day = floor_mod_i64(local_seconds, kSecondsPerDay);
  LocalFields out;
  out.date = civil_from_days(days);
  out.hour = second_of_day / 3600;
  out.minute = (second_of_day / 60) % 60;
  out.second = second_of_day % 60;
  out.nanosecond = static_cast<int>(time.nanosecond);
  return out;
}

enum class LocalResolveStatus { Ok, Invalid, Gap, Fold };

struct LocalResolveResult {
  LocalResolveStatus status = LocalResolveStatus::Invalid;
  RuntimeTimeValue time;
};

std::vector<int> candidate_offsets_for_zone(const RuntimeTimeZoneValue &zone) {
  std::vector<int> offsets;
  auto add = [&](int offset) {
    if (std::find(offsets.begin(), offsets.end(), offset) == offsets.end()) {
      offsets.push_back(offset);
    }
  };
  add(zone.offset_seconds);
  if (zone.fixed_offset) {
    return offsets;
  }
  const std::shared_ptr<const ZoneData> data = zone_data_for_name(zone.name);
  if (data == nullptr) {
    return offsets;
  }
  for (const ZoneTypeInfo &type : data->types) {
    add(type.offset_seconds);
  }
  if (data->future_rule.has_value()) {
    add(data->future_rule->std_offset);
    if (data->future_rule->has_dst) {
      add(data->future_rule->dst_offset);
    }
  }
  return offsets;
}

std::optional<std::pair<int, int>>
gap_offsets_for_local_seconds(const RuntimeTimeZoneValue &zone,
                              std::int64_t target_local_seconds) {
  if (zone.fixed_offset) {
    return std::nullopt;
  }
  const std::shared_ptr<const ZoneData> data = zone_data_for_name(zone.name);
  if (data == nullptr) {
    return std::nullopt;
  }
  auto consider = [&](std::int64_t transition_utc, int before_offset,
                      int after_offset)
      -> std::optional<std::pair<int, int>> {
    if (after_offset <= before_offset) {
      return std::nullopt;
    }
    const std::int64_t gap_start = transition_utc + before_offset;
    const std::int64_t gap_end = transition_utc + after_offset;
    if (target_local_seconds >= gap_start && target_local_seconds < gap_end) {
      return std::make_pair(before_offset, after_offset);
    }
    return std::nullopt;
  };
  for (const ZoneTransition &transition : data->transitions) {
    const int before =
        zone_info_from_data(*data, transition.utc_seconds - 1).offset_seconds;
    const int after =
        zone_info_from_data(*data, transition.utc_seconds).offset_seconds;
    if (const auto gap = consider(transition.utc_seconds, before, after)) {
      return gap;
    }
  }
  if (data->future_rule.has_value() && data->future_rule->has_dst) {
    const CivilDate date =
        civil_from_days(floor_div_i64(target_local_seconds, kSecondsPerDay));
    for (std::int64_t year = date.year - 1; year <= date.year + 1; ++year) {
      const PosixYearTransitions trans =
          posix_year_transitions(year, *data->future_rule);
      if (const auto gap = consider(trans.start_utc,
                                    data->future_rule->std_offset,
                                    data->future_rule->dst_offset)) {
        return gap;
      }
      if (const auto gap = consider(trans.end_utc,
                                    data->future_rule->dst_offset,
                                    data->future_rule->std_offset)) {
        return gap;
      }
    }
  }
  return std::nullopt;
}

LocalResolveResult resolve_local_fields(const LocalFields &fields,
                                        const RuntimeTimeZoneValue &zone,
                                        const ResolvePolicy &policy) {
  LocalResolveResult result;
  if (!valid_date(fields.date.year, fields.date.month, fields.date.day) ||
      fields.hour < 0 || fields.hour > 23 || fields.minute < 0 ||
      fields.minute > 59 || fields.second < 0 || fields.second > 59 ||
      fields.nanosecond < 0 || fields.nanosecond >= kNanosPerSecond) {
    result.status = LocalResolveStatus::Invalid;
    return result;
  }
  const std::int64_t target_local_seconds = local_seconds_from_fields(fields);
  std::vector<RuntimeTimeValue> candidates;
  for (int offset : candidate_offsets_for_zone(zone)) {
    const std::int64_t utc_seconds = target_local_seconds - offset;
    RuntimeTimeValue candidate;
    candidate.epoch_seconds = utc_seconds;
    candidate.nanosecond = static_cast<std::uint32_t>(fields.nanosecond);
    candidate.zone_name = zone.name;
    candidate.zone_fixed_offset = zone.fixed_offset;
    candidate.zone_offset_seconds =
        zone_info_at(zone, utc_seconds).offset_seconds;
    if (candidate.zone_offset_seconds != offset) {
      continue;
    }
    if (same_local_fields(local_fields(candidate), fields)) {
      candidates.push_back(candidate);
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const RuntimeTimeValue &lhs, const RuntimeTimeValue &rhs) {
              return lhs.epoch_seconds < rhs.epoch_seconds ||
                     (lhs.epoch_seconds == rhs.epoch_seconds &&
                      lhs.nanosecond < rhs.nanosecond);
            });
  candidates.erase(
      std::unique(candidates.begin(), candidates.end(),
                  [](const RuntimeTimeValue &lhs, const RuntimeTimeValue &rhs) {
                    return lhs.epoch_seconds == rhs.epoch_seconds &&
                           lhs.nanosecond == rhs.nanosecond;
                  }),
      candidates.end());
  if (candidates.size() == 1U) {
    result.status = LocalResolveStatus::Ok;
    result.time = candidates.front();
    return result;
  }
  if (candidates.size() > 1U) {
    if (policy.fold == FoldPolicy::Raise) {
      result.status = LocalResolveStatus::Fold;
      return result;
    }
    result.status = LocalResolveStatus::Ok;
    result.time = policy.fold == FoldPolicy::Earlier ? candidates.front()
                                                     : candidates.back();
    return result;
  }
  const std::optional<std::pair<int, int>> gap =
      gap_offsets_for_local_seconds(zone, target_local_seconds);
  if (!gap.has_value() || policy.gap == GapPolicy::Raise) {
    result.status = LocalResolveStatus::Gap;
    return result;
  }
  const int chosen_offset =
      policy.gap == GapPolicy::Forward ? gap->first : gap->second;
  const std::int64_t utc_seconds = target_local_seconds - chosen_offset;
  result.status = LocalResolveStatus::Ok;
  result.time.epoch_seconds = utc_seconds;
  result.time.nanosecond = static_cast<std::uint32_t>(fields.nanosecond);
  result.time.zone_name = zone.name;
  result.time.zone_fixed_offset = zone.fixed_offset;
  result.time.zone_offset_seconds =
      zone_info_at(zone, utc_seconds).offset_seconds;
  return result;
}

std::optional<RuntimeTimeValue>
time_from_local_fields(std::int64_t year, int month, int day, int hour,
                       int minute, int second, int nanosecond,
                       const RuntimeTimeZoneValue &zone) {
  LocalFields fields;
  fields.date = CivilDate{year, month, day};
  fields.hour = hour;
  fields.minute = minute;
  fields.second = second;
  fields.nanosecond = nanosecond;
  const LocalResolveResult resolved =
      resolve_local_fields(fields, zone, ResolvePolicy{});
  if (resolved.status != LocalResolveStatus::Ok) {
    return std::nullopt;
  }
  return resolved.time;
}

std::optional<RuntimeTimeValue>
time_from_utc_fields(std::int64_t year, int month, int day, int hour,
                     int minute, int second, int nanosecond) {
  return time_from_local_fields(year, month, day, hour, minute, second,
                                nanosecond, utc_zone());
}

std::optional<RuntimeTimeValue>
time_from_fields(const LocalFields &fields, const RuntimeTimeZoneValue &zone) {
  return time_from_local_fields(fields.date.year, fields.date.month,
                                fields.date.day, fields.hour, fields.minute,
                                fields.second, fields.nanosecond, zone);
}

std::optional<RuntimeTimeValue>
apply_period_to_time(const RuntimeTimeValue &time,
                     const RuntimeTimePeriodValue &period) {
  const RuntimeTimeZoneValue zone = zone_of(time);
  LocalFields fields = local_fields(time);
  if (period.months != 0) {
    const __int128 total_month = static_cast<__int128>(fields.date.year) * 12 +
                                 (fields.date.month - 1) + period.months;
    const __int128 year = floor_div_i128(total_month, 12);
    const int month = static_cast<int>(total_month - year * 12) + 1;
    const std::optional<std::int64_t> checked_year = checked_i128(year);
    if (!checked_year.has_value()) {
      return std::nullopt;
    }
    fields.date.year = *checked_year;
    fields.date.month = month;
    fields.date.day =
        std::min(fields.date.day, days_in_month(fields.date.year, month));
  }
  const __int128 base_days =
      static_cast<__int128>(days_from_civil(fields.date.year, fields.date.month,
                                            fields.date.day)) +
      period.days;
  const std::optional<std::int64_t> checked_days = checked_i128(base_days);
  if (!checked_days.has_value()) {
    return std::nullopt;
  }
  fields.date = civil_from_days(*checked_days);
  const std::optional<RuntimeTimeValue> resolved = time_from_fields(fields, zone);
  if (!resolved.has_value()) {
    return std::nullopt;
  }
  return make_time_from_parts(
      resolved->epoch_seconds,
      static_cast<__int128>(resolved->nanosecond) + period.nanoseconds, zone);
}

std::optional<RuntimeTimePeriodValue>
period_add(const RuntimeTimePeriodValue &lhs, const RuntimeTimePeriodValue &rhs,
           int sign = 1) {
  RuntimeTimePeriodValue out;
  const std::optional<std::int64_t> months =
      checked_i128(static_cast<__int128>(lhs.months) +
                   sign * static_cast<__int128>(rhs.months));
  const std::optional<std::int64_t> days = checked_i128(
      static_cast<__int128>(lhs.days) + sign * static_cast<__int128>(rhs.days));
  const std::optional<std::int64_t> nanos =
      checked_i128(static_cast<__int128>(lhs.nanoseconds) +
                   sign * static_cast<__int128>(rhs.nanoseconds));
  if (!months.has_value() || !days.has_value() || !nanos.has_value()) {
    return std::nullopt;
  }
  out.months = *months;
  out.days = *days;
  out.nanoseconds = *nanos;
  return out;
}

std::optional<RuntimeTimePeriodValue>
period_between(const RuntimeTimeValue &lhs, const RuntimeTimeValue &rhs) {
  const __int128 lhs_ns =
      static_cast<__int128>(lhs.epoch_seconds) * kNanosPerSecond +
      lhs.nanosecond;
  const __int128 rhs_ns =
      static_cast<__int128>(rhs.epoch_seconds) * kNanosPerSecond +
      rhs.nanosecond;
  const std::optional<std::int64_t> nanos = checked_i128(lhs_ns - rhs_ns);
  if (!nanos.has_value()) {
    return std::nullopt;
  }
  return RuntimeTimePeriodValue{0, 0, *nanos};
}

std::optional<__int128>
fixed_period_nanoseconds(const RuntimeTimePeriodValue &period) {
  if (period.months != 0) {
    return std::nullopt;
  }
  return static_cast<__int128>(period.days) * kSecondsPerDay * kNanosPerSecond +
         period.nanoseconds;
}

int compare_time(const RuntimeTimeValue &lhs, const RuntimeTimeValue &rhs) {
  if (lhs.epoch_seconds != rhs.epoch_seconds) {
    return lhs.epoch_seconds < rhs.epoch_seconds ? -1 : 1;
  }
  if (lhs.nanosecond != rhs.nanosecond) {
    return lhs.nanosecond < rhs.nanosecond ? -1 : 1;
  }
  return 0;
}

int compare_i128(__int128 lhs, __int128 rhs) {
  if (lhs < rhs) {
    return -1;
  }
  if (lhs > rhs) {
    return 1;
  }
  return 0;
}

bool require_int(const Value &value, std::int64_t *out) {
  if (!value.is_integer()) {
    return false;
  }
  *out = value.as_integer();
  return true;
}

std::optional<std::int64_t> keyword_int(NativeStdlibCall &call,
                                        const std::string &name,
                                        std::int64_t fallback) {
  const std::optional<Value> value = call.keyword(name);
  if (!value.has_value()) {
    return fallback;
  }
  if (!value->is_integer()) {
    call.fault("TypeError", name + " must be Int");
    return std::nullopt;
  }
  return value->as_integer();
}

SendStatus overflow_fault(NativeStdlibCall &call,
                          const std::string &message = "Time overflow") {
  return call.fault("OverflowError", message);
}

SendStatus invalid_time_fault(NativeStdlibCall &call,
                              const std::string &message) {
  return call.fault("ArgumentError", message);
}

SendStatus local_resolve_fault(NativeStdlibCall &call,
                               LocalResolveStatus status) {
  if (status == LocalResolveStatus::Gap) {
    return call.fault("TimeZoneGapError", "local time does not exist");
  }
  if (status == LocalResolveStatus::Fold) {
    return call.fault("TimeZoneAmbiguousError", "local time is ambiguous");
  }
  return invalid_time_fault(call, "invalid local time");
}

std::optional<RuntimeTimeZoneValue>
zone_from_value(NativeStdlibCall &call, const Value &value,
                bool fault_on_missing = true) {
  if (value.is_time_zone()) {
    const std::shared_ptr<RuntimeTimeZoneValue> zone = value.as_time_zone();
    if (zone == nullptr) {
      call.fault("TypeError", "TimeZone value is null");
      return std::nullopt;
    }
    return *zone;
  }
  const std::optional<std::string> text = call.text_of(value);
  if (!text.has_value()) {
    call.fault("TypeError", "time zone must be TimeZone, Str, or Symbol");
    return std::nullopt;
  }
  if (ascii_lower(*text) == "local") {
    const std::optional<std::string> name = local_zone_name();
    if (!name.has_value()) {
      if (fault_on_missing) {
        call.fault("TimeZoneLookupError", "unable to resolve local time zone");
      }
      return std::nullopt;
    }
    const std::optional<RuntimeTimeZoneValue> zone = lookup_time_zone(*name);
    if (!zone.has_value()) {
      if (fault_on_missing) {
        call.fault("TimeZoneLookupError", "unable to resolve local time zone");
      }
      return std::nullopt;
    }
    return zone;
  }
  const std::optional<RuntimeTimeZoneValue> zone = lookup_time_zone(*text);
  if (!zone.has_value() && fault_on_missing) {
    call.fault("TimeZoneLookupError", "unknown time zone: " + *text);
  }
  return zone;
}

std::optional<ParseZone> parse_zone_keyword(NativeStdlibCall &call) {
  ParseZone out;
  out.zone = utc_zone();
  const std::optional<Value> value = call.keyword("zone");
  if (!value.has_value()) {
    return out;
  }
  if (value->is_symbol()) {
    const std::optional<std::string> text = call.text_of(*value);
    if (!text.has_value()) {
      return std::nullopt;
    }
    const std::string key = ascii_lower(*text);
    if (key == "utc") {
      out.mode = ParseZoneMode::Utc;
      out.zone = utc_zone();
      return out;
    }
    if (key == "parsed") {
      out.mode = ParseZoneMode::Parsed;
      out.zone = utc_zone();
      return out;
    }
    if (key == "local") {
      const std::optional<RuntimeTimeZoneValue> zone =
          zone_from_value(call, *value, true);
      if (!zone.has_value()) {
        return std::nullopt;
      }
      out.mode = ParseZoneMode::Explicit;
      out.zone = *zone;
      return out;
    }
  }
  const std::optional<RuntimeTimeZoneValue> zone =
      zone_from_value(call, *value, true);
  if (!zone.has_value()) {
    return std::nullopt;
  }
  out.mode = ParseZoneMode::Explicit;
  out.zone = *zone;
  return out;
}

std::optional<ResolvePolicy> parse_resolve_policy_keywords(
    NativeStdlibCall &call) {
  ResolvePolicy policy;
  if (const std::optional<Value> value = call.keyword("on_gap")) {
    const std::optional<std::string> text = call.text_of(*value);
    if (!text.has_value()) {
      call.fault("TypeError", "on_gap must be Symbol or Str");
      return std::nullopt;
    }
    const std::string key = ascii_lower(*text);
    if (key == "raise") {
      policy.gap = GapPolicy::Raise;
    } else if (key == "forward") {
      policy.gap = GapPolicy::Forward;
    } else if (key == "backward") {
      policy.gap = GapPolicy::Backward;
    } else {
      call.fault("ArgumentError", "unknown on_gap policy");
      return std::nullopt;
    }
  }
  if (const std::optional<Value> value = call.keyword("on_fold")) {
    const std::optional<std::string> text = call.text_of(*value);
    if (!text.has_value()) {
      call.fault("TypeError", "on_fold must be Symbol or Str");
      return std::nullopt;
    }
    const std::string key = ascii_lower(*text);
    if (key == "earlier") {
      policy.fold = FoldPolicy::Earlier;
    } else if (key == "later") {
      policy.fold = FoldPolicy::Later;
    } else if (key == "raise") {
      policy.fold = FoldPolicy::Raise;
    } else {
      call.fault("ArgumentError", "unknown on_fold policy");
      return std::nullopt;
    }
  }
  return policy;
}

bool parse_date_ymd(const std::string &text, std::size_t *pos,
                    LocalFields *fields) {
  std::int64_t year = 0;
  int month = 0;
  int day = 0;
  if (!parse_year4(text, *pos, &year)) {
    return false;
  }
  *pos += 4;
  if (*pos >= text.size() || text[*pos] != '-') {
    return false;
  }
  ++*pos;
  if (!parse_fixed_digits(text, *pos, 2, &month)) {
    return false;
  }
  *pos += 2;
  if (*pos >= text.size() || text[*pos] != '-') {
    return false;
  }
  ++*pos;
  if (!parse_fixed_digits(text, *pos, 2, &day)) {
    return false;
  }
  *pos += 2;
  fields->date = CivilDate{year, month, day};
  return valid_date(year, month, day);
}

bool parse_date_ru_numeric(const std::string &text, std::size_t *pos,
                           LocalFields *fields) {
  int day = 0;
  int month = 0;
  std::int64_t year = 0;
  if (!parse_fixed_digits(text, *pos, 2, &day)) {
    return false;
  }
  *pos += 2;
  if (*pos >= text.size() || text[*pos] != '.') {
    return false;
  }
  ++*pos;
  if (!parse_fixed_digits(text, *pos, 2, &month)) {
    return false;
  }
  *pos += 2;
  if (*pos >= text.size() || text[*pos] != '.') {
    return false;
  }
  ++*pos;
  if (!parse_year4(text, *pos, &year)) {
    return false;
  }
  *pos += 4;
  fields->date = CivilDate{year, month, day};
  return valid_date(year, month, day);
}

bool parse_time_hms(const std::string &text, std::size_t *pos,
                    LocalFields *fields, bool seconds_optional) {
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parse_fixed_digits(text, *pos, 2, &hour)) {
    return false;
  }
  *pos += 2;
  if (*pos >= text.size() || text[*pos] != ':') {
    return false;
  }
  ++*pos;
  if (!parse_fixed_digits(text, *pos, 2, &minute)) {
    return false;
  }
  *pos += 2;
  if (*pos < text.size() && text[*pos] == ':') {
    ++*pos;
    if (!parse_fixed_digits(text, *pos, 2, &second)) {
      return false;
    }
    *pos += 2;
  } else if (!seconds_optional) {
    return false;
  }
  int nanosecond = 0;
  if (*pos < text.size() && text[*pos] == '.') {
    ++*pos;
    int digits = 0;
    while (*pos < text.size() && text[*pos] >= '0' && text[*pos] <= '9') {
      if (digits < 9) {
        nanosecond = nanosecond * 10 + (text[*pos] - '0');
      }
      ++digits;
      ++*pos;
    }
    if (digits == 0 || digits > 9) {
      return false;
    }
    for (; digits < 9; ++digits) {
      nanosecond *= 10;
    }
  }
  if (hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  fields->hour = hour;
  fields->minute = minute;
  fields->second = second;
  fields->nanosecond = nanosecond;
  return true;
}

bool parse_trailing_zone(const std::string &text, std::size_t *pos,
                         ParsedDateTime *parsed) {
  if (*pos >= text.size()) {
    return true;
  }
  if (text[*pos] != ' ') {
    return false;
  }
  ++*pos;
  int offset = 0;
  std::string name;
  const std::size_t before = *pos;
  if (parse_offset_text_at(text, pos, &offset, &name) && *pos == text.size()) {
    parsed->has_offset = true;
    parsed->offset_seconds = offset;
    parsed->offset_name = name;
    parsed->offset_fixed = true;
    return true;
  }
  *pos = before;
  const std::string marker = text.substr(*pos);
  const std::optional<RuntimeTimeZoneValue> zone = lookup_time_zone(marker);
  if (zone.has_value()) {
    *pos = text.size();
    parsed->has_offset = true;
    parsed->offset_seconds = zone->offset_seconds;
    parsed->offset_name = zone->name;
    parsed->offset_fixed = zone->fixed_offset;
    return true;
  }
  return false;
}

std::optional<ParsedDateTime> parse_iso8601(const std::string &text,
                                            bool require_offset,
                                            bool allow_space) {
  ParsedDateTime parsed;
  std::size_t pos = 0;
  if (!parse_date_ymd(text, &pos, &parsed.fields)) {
    return std::nullopt;
  }
  if (pos >= text.size() ||
      !(text[pos] == 'T' || text[pos] == 't' ||
        (allow_space && text[pos] == ' '))) {
    return std::nullopt;
  }
  ++pos;
  if (!parse_time_hms(text, &pos, &parsed.fields, true)) {
    return std::nullopt;
  }
  if (pos < text.size()) {
    if (text[pos] == ' ') {
      ++pos;
    }
    int offset = 0;
    std::string name;
    if (!parse_offset_text_at(text, &pos, &offset, &name)) {
      return std::nullopt;
    }
    parsed.has_offset = true;
    parsed.offset_seconds = offset;
    parsed.offset_name = name;
  }
  if (pos != text.size() || (require_offset && !parsed.has_offset)) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<ParsedDateTime> parse_ymd_date_or_datetime(
    const std::string &text, bool require_time) {
  ParsedDateTime parsed;
  std::size_t pos = 0;
  if (!parse_date_ymd(text, &pos, &parsed.fields)) {
    return std::nullopt;
  }
  if (pos == text.size()) {
    return require_time ? std::nullopt : std::optional<ParsedDateTime>(parsed);
  }
  if (text[pos] != ' ') {
    return std::nullopt;
  }
  ++pos;
  if (!parse_time_hms(text, &pos, &parsed.fields, true)) {
    return std::nullopt;
  }
  if (!parse_trailing_zone(text, &pos, &parsed) || pos != text.size()) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<ParsedDateTime> parse_ru_date_or_datetime(
    const std::string &text, bool require_time) {
  ParsedDateTime parsed;
  std::size_t pos = 0;
  if (!parse_date_ru_numeric(text, &pos, &parsed.fields)) {
    return std::nullopt;
  }
  if (pos == text.size()) {
    return require_time ? std::nullopt : std::optional<ParsedDateTime>(parsed);
  }
  if (text[pos] != ' ') {
    return std::nullopt;
  }
  ++pos;
  if (!parse_time_hms(text, &pos, &parsed.fields, true)) {
    return std::nullopt;
  }
  if (!parse_trailing_zone(text, &pos, &parsed) || pos != text.size()) {
    return std::nullopt;
  }
  return parsed;
}

int ru_month_number(const std::string &month) {
  static const std::array<const char *, 12> kMonths = {
      "января",   "февраля", "марта",   "апреля",
      "мая",      "июня",    "июля",    "августа",
      "сентября", "октября", "ноября",  "декабря"};
  for (std::size_t i = 0; i < kMonths.size(); ++i) {
    if (month == kMonths[i]) {
      return static_cast<int>(i) + 1;
    }
  }
  return 0;
}

std::vector<std::string> split_spaces(const std::string &text) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos < text.size()) {
    while (pos < text.size() && text[pos] == ' ') {
      ++pos;
    }
    if (pos >= text.size()) {
      break;
    }
    const std::size_t start = pos;
    while (pos < text.size() && text[pos] != ' ') {
      ++pos;
    }
    out.push_back(text.substr(start, pos - start));
  }
  return out;
}

std::optional<ParsedDateTime> parse_ru_long(const std::string &text) {
  const std::vector<std::string> parts = split_spaces(text);
  if (parts.size() != 3U && parts.size() != 4U && parts.size() != 5U) {
    return std::nullopt;
  }
  std::size_t day_pos = 0;
  int day = 0;
  if (!parse_variable_digits(parts[0], &day_pos, 1, 2, &day) ||
      day_pos != parts[0].size()) {
    return std::nullopt;
  }
  const int month = ru_month_number(parts[1]);
  if (month == 0) {
    return std::nullopt;
  }
  std::int64_t year = 0;
  if (!parse_year4(parts[2], 0, &year) || parts[2].size() != 4U ||
      !valid_date(year, month, day)) {
    return std::nullopt;
  }
  ParsedDateTime parsed;
  parsed.fields.date = CivilDate{year, month, day};
  if (parts.size() >= 4U) {
    std::size_t time_pos = 0;
    if (!parse_time_hms(parts[3], &time_pos, &parsed.fields, true) ||
        time_pos != parts[3].size()) {
      return std::nullopt;
    }
  }
  if (parts.size() == 5U) {
    const std::optional<RuntimeTimeZoneValue> zone = lookup_time_zone(parts[4]);
    if (!zone.has_value()) {
      return std::nullopt;
    }
    parsed.has_offset = true;
    parsed.offset_seconds = zone->offset_seconds;
    parsed.offset_name = zone->name;
    parsed.offset_fixed = zone->fixed_offset;
  }
  return parsed;
}

int english_month_number(const std::string &month) {
  static const std::array<const char *, 12> kMonths = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (std::size_t i = 0; i < kMonths.size(); ++i) {
    if (month == kMonths[i]) {
      return static_cast<int>(i) + 1;
    }
  }
  return 0;
}

std::optional<ParsedDateTime> parse_http_date(const std::string &text) {
  const std::vector<std::string> parts = split_spaces(text);
  if (parts.size() != 6U || parts[0].empty() || parts[0].back() != ',') {
    return std::nullopt;
  }
  std::size_t day_pos = 0;
  int day = 0;
  if (!parse_variable_digits(parts[1], &day_pos, 1, 2, &day) ||
      day_pos != parts[1].size()) {
    return std::nullopt;
  }
  const int month = english_month_number(parts[2]);
  std::int64_t year = 0;
  if (month == 0 || !parse_year4(parts[3], 0, &year) ||
      parts[3].size() != 4U || !valid_date(year, month, day)) {
    return std::nullopt;
  }
  ParsedDateTime parsed;
  parsed.fields.date = CivilDate{year, month, day};
  std::size_t time_pos = 0;
  if (!parse_time_hms(parts[4], &time_pos, &parsed.fields, false) ||
      time_pos != parts[4].size()) {
    return std::nullopt;
  }
  if (parts[5] != "GMT" && parts[5] != "UTC") {
    return std::nullopt;
  }
  parsed.has_offset = true;
  parsed.offset_seconds = 0;
  parsed.offset_name = "UTC";
  return parsed;
}

std::optional<ParsedDateTime> parse_named_format(const std::string &text,
                                                 const std::string &format) {
  const std::string key = ascii_lower(format);
  if (key == "iso8601" || key == "rfc3339") {
    return parse_iso8601(text, true, false);
  }
  if (key == "date") {
    return parse_ymd_date_or_datetime(text, false);
  }
  if (key == "datetime") {
    return parse_ymd_date_or_datetime(text, true);
  }
  if (key == "ru_date") {
    return parse_ru_date_or_datetime(text, false);
  }
  if (key == "ru_datetime") {
    return parse_ru_date_or_datetime(text, true);
  }
  if (key == "ru_long") {
    return parse_ru_long(text);
  }
  if (key == "http_date") {
    return parse_http_date(text);
  }
  if (key == "auto") {
    if (const auto parsed = parse_iso8601(text, true, false)) {
      return parsed;
    }
    if (const auto parsed = parse_iso8601(text, false, true)) {
      return parsed;
    }
    if (const auto parsed = parse_ymd_date_or_datetime(text, false)) {
      return parsed;
    }
    if (const auto parsed = parse_ru_date_or_datetime(text, false)) {
      return parsed;
    }
    if (const auto parsed = parse_ru_long(text)) {
      return parsed;
    }
    if (const auto parsed = parse_http_date(text)) {
      return parsed;
    }
  }
  return std::nullopt;
}

bool parse_pattern_directive(const std::string &text, std::size_t *pos,
                             const std::string &directive,
                             ParsedDateTime *parsed, bool *saw_year,
                             bool *saw_month, bool *saw_day) {
  if (directive == "Y") {
    std::int64_t year = 0;
    if (!parse_year4(text, *pos, &year)) {
      return false;
    }
    *pos += 4;
    parsed->fields.date.year = year;
    *saw_year = true;
    return true;
  }
  if (directive == "y") {
    int year = 0;
    if (!parse_fixed_digits(text, *pos, 2, &year)) {
      return false;
    }
    *pos += 2;
    parsed->fields.date.year = year >= 69 ? 1900 + year : 2000 + year;
    *saw_year = true;
    return true;
  }
  if (directive == "m" || directive == "-m") {
    int month = 0;
    if (directive == "m") {
      if (!parse_fixed_digits(text, *pos, 2, &month)) {
        return false;
      }
      *pos += 2;
    } else if (!parse_variable_digits(text, pos, 1, 2, &month)) {
      return false;
    }
    parsed->fields.date.month = month;
    *saw_month = true;
    return true;
  }
  if (directive == "d" || directive == "-d") {
    int day = 0;
    if (directive == "d") {
      if (!parse_fixed_digits(text, *pos, 2, &day)) {
        return false;
      }
      *pos += 2;
    } else if (!parse_variable_digits(text, pos, 1, 2, &day)) {
      return false;
    }
    parsed->fields.date.day = day;
    *saw_day = true;
    return true;
  }
  if (directive == "H") {
    int value = 0;
    if (!parse_fixed_digits(text, *pos, 2, &value) || value > 23) {
      return false;
    }
    *pos += 2;
    parsed->fields.hour = value;
    return true;
  }
  if (directive == "M") {
    int value = 0;
    if (!parse_fixed_digits(text, *pos, 2, &value) || value > 59) {
      return false;
    }
    *pos += 2;
    parsed->fields.minute = value;
    return true;
  }
  if (directive == "S") {
    int value = 0;
    if (!parse_fixed_digits(text, *pos, 2, &value) || value > 59) {
      return false;
    }
    *pos += 2;
    parsed->fields.second = value;
    return true;
  }
  if (directive == "N") {
    int value = 0;
    int digits = 0;
    while (*pos < text.size() && text[*pos] >= '0' && text[*pos] <= '9' &&
           digits < 9) {
      value = value * 10 + (text[*pos] - '0');
      ++*pos;
      ++digits;
    }
    if (digits == 0) {
      return false;
    }
    for (; digits < 9; ++digits) {
      value *= 10;
    }
    parsed->fields.nanosecond = value;
    return true;
  }
  if (directive == "L") {
    int value = 0;
    if (!parse_fixed_digits(text, *pos, 3, &value)) {
      return false;
    }
    *pos += 3;
    parsed->fields.nanosecond = value * 1000000;
    return true;
  }
  if (directive == "z" || directive == ":z") {
    int offset = 0;
    std::string name;
    const std::size_t before = *pos;
    if (!parse_offset_text_at(text, pos, &offset, &name)) {
      return false;
    }
    if (directive == "z" && text.substr(before, *pos - before).find(':') !=
                                std::string::npos) {
      return false;
    }
    if (directive == ":z" && text.substr(before, *pos - before).find(':') ==
                                  std::string::npos && name != "UTC") {
      return false;
    }
    parsed->has_offset = true;
    parsed->offset_seconds = offset;
    parsed->offset_name = name;
    parsed->offset_fixed = true;
    return true;
  }
  if (directive == "Z") {
    const std::size_t start = *pos;
    while (*pos < text.size() && text[*pos] != ' ') {
      ++*pos;
    }
    const std::optional<RuntimeTimeZoneValue> zone =
        lookup_time_zone(text.substr(start, *pos - start));
    if (!zone.has_value()) {
      return false;
    }
    parsed->has_offset = true;
    parsed->offset_seconds = zone->offset_seconds;
    parsed->offset_name = zone->name;
    parsed->offset_fixed = zone->fixed_offset;
    return true;
  }
  return false;
}

std::optional<ParsedDateTime> parse_pattern(const std::string &text,
                                            const std::string &pattern) {
  ParsedDateTime parsed;
  bool saw_year = false;
  bool saw_month = false;
  bool saw_day = false;
  std::size_t pos = 0;
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] != '%') {
      if (pos >= text.size() || text[pos] != pattern[i]) {
        return std::nullopt;
      }
      ++pos;
      continue;
    }
    ++i;
    if (i >= pattern.size()) {
      return std::nullopt;
    }
    std::string directive;
    if (pattern[i] == '-') {
      if (i + 1U >= pattern.size()) {
        return std::nullopt;
      }
      directive = pattern.substr(i, 2);
      ++i;
    } else if (pattern[i] == ':') {
      if (i + 1U >= pattern.size()) {
        return std::nullopt;
      }
      directive = pattern.substr(i, 2);
      ++i;
    } else {
      directive = pattern.substr(i, 1);
    }
    if (directive == "%") {
      if (pos >= text.size() || text[pos] != '%') {
        return std::nullopt;
      }
      ++pos;
      continue;
    }
    if (directive == "F") {
      const std::size_t before = pos;
      if (!parse_date_ymd(text, &pos, &parsed.fields)) {
        return std::nullopt;
      }
      saw_year = saw_month = saw_day = pos > before;
      continue;
    }
    if (directive == "T") {
      if (!parse_time_hms(text, &pos, &parsed.fields, false)) {
        return std::nullopt;
      }
      continue;
    }
    if (!parse_pattern_directive(text, &pos, directive, &parsed, &saw_year,
                                 &saw_month, &saw_day)) {
      return std::nullopt;
    }
  }
  if (pos != text.size() || !saw_year || !saw_month || !saw_day ||
      !valid_date(parsed.fields.date.year, parsed.fields.date.month,
                  parsed.fields.date.day)) {
    return std::nullopt;
  }
  return parsed;
}

LocalResolveResult materialize_parsed_time(const ParsedDateTime &parsed,
                                           const ParseZone &zone,
                                           const ResolvePolicy &policy) {
  RuntimeTimeZoneValue resolution_zone = zone.zone;
  if (parsed.has_offset) {
    resolution_zone = RuntimeTimeZoneValue{
        parsed.offset_seconds, parsed.offset_name, parsed.offset_fixed};
  }
  LocalResolveResult resolved =
      resolve_local_fields(parsed.fields, resolution_zone, policy);
  if (resolved.status != LocalResolveStatus::Ok) {
    return resolved;
  }
  RuntimeTimeZoneValue display_zone = utc_zone();
  if (zone.mode == ParseZoneMode::Parsed) {
    display_zone = parsed.has_offset ? resolution_zone : utc_zone();
  } else if (zone.mode == ParseZoneMode::Explicit) {
    display_zone = zone.zone;
  }
  const ZoneInstantInfo info =
      zone_info_at(display_zone, resolved.time.epoch_seconds);
  resolved.time.zone_offset_seconds = info.offset_seconds;
  resolved.time.zone_name = display_zone.name;
  resolved.time.zone_fixed_offset = display_zone.fixed_offset;
  return resolved;
}

LocalResolveResult parse_time_text(const std::string &text,
                                   const std::string &format, bool pattern,
                                   const ParseZone &zone,
                                   const ResolvePolicy &policy) {
  const std::optional<ParsedDateTime> parsed =
      pattern ? parse_pattern(text, format) : parse_named_format(text, format);
  if (!parsed.has_value()) {
    return LocalResolveResult{};
  }
  return materialize_parsed_time(*parsed, zone, policy);
}

std::string format_fraction(int nanosecond) {
  if (nanosecond == 0) {
    return "";
  }
  std::string fraction = std::to_string(nanosecond);
  fraction.insert(fraction.begin(), 9U - fraction.size(), '0');
  while (!fraction.empty() && fraction.back() == '0') {
    fraction.pop_back();
  }
  return "." + fraction;
}

int iso_weekday_from_days(std::int64_t days) {
  return floor_mod_i64(days + 3, 7) + 1;
}

int iso_weekday(const RuntimeTimeValue &time) {
  const LocalFields fields = local_fields(time);
  return iso_weekday_from_days(days_from_civil(
      fields.date.year, fields.date.month, fields.date.day));
}

std::string format_iso8601(const RuntimeTimeValue &value, bool with_zone_name) {
  const LocalFields fields = local_fields(value);
  const int offset = zone_info_for_time(value).offset_seconds;
  std::ostringstream out;
  out << four_digits(fields.date.year) << "-" << two_digits(fields.date.month)
      << "-" << two_digits(fields.date.day) << "T" << two_digits(fields.hour)
      << ":" << two_digits(fields.minute) << ":" << two_digits(fields.second)
      << format_fraction(fields.nanosecond) << offset_text(offset, true, true);
  if (with_zone_name && value.zone_name != "UTC" &&
      value.zone_name != offset_name(offset)) {
    out << "[" << value.zone_name << "]";
  }
  return out.str();
}

std::string format_datetime(const RuntimeTimeValue &value) {
  const LocalFields fields = local_fields(value);
  return four_digits(fields.date.year) + "-" + two_digits(fields.date.month) +
         "-" + two_digits(fields.date.day) + " " + two_digits(fields.hour) +
         ":" + two_digits(fields.minute) + ":" + two_digits(fields.second);
}

std::string format_date(const RuntimeTimeValue &value) {
  const LocalFields fields = local_fields(value);
  return four_digits(fields.date.year) + "-" + two_digits(fields.date.month) +
         "-" + two_digits(fields.date.day);
}

std::string format_ru_date(const RuntimeTimeValue &value) {
  const LocalFields fields = local_fields(value);
  return two_digits(fields.date.day) + "." + two_digits(fields.date.month) +
         "." + four_digits(fields.date.year);
}

std::string format_ru_datetime(const RuntimeTimeValue &value) {
  const LocalFields fields = local_fields(value);
  return format_ru_date(value) + " " + two_digits(fields.hour) + ":" +
         two_digits(fields.minute) + ":" + two_digits(fields.second);
}

std::string format_ru_long(const RuntimeTimeValue &value) {
  static const std::array<const char *, 12> kMonths = {
      "января",   "февраля", "марта",   "апреля",
      "мая",      "июня",    "июля",    "августа",
      "сентября", "октября", "ноября",  "декабря"};
  const LocalFields fields = local_fields(value);
  std::ostringstream out;
  out << fields.date.day << " " << kMonths[fields.date.month - 1] << " "
      << fields.date.year << " " << two_digits(fields.hour) << ":"
      << two_digits(fields.minute);
  return out.str();
}

std::string format_http_date(const RuntimeTimeValue &value) {
  static const std::array<const char *, 7> kWeekdays = {
      "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  static const std::array<const char *, 12> kMonths = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  RuntimeTimeValue utc = value;
  utc.zone_offset_seconds = 0;
  utc.zone_name = "UTC";
  utc.zone_fixed_offset = true;
  const LocalFields fields = local_fields(utc);
  const int weekday_index = iso_weekday(utc) - 1;
  std::ostringstream out;
  out << kWeekdays[weekday_index] << ", " << two_digits(fields.date.day) << " "
      << kMonths[fields.date.month - 1] << " " << four_digits(fields.date.year)
      << " " << two_digits(fields.hour) << ":" << two_digits(fields.minute)
      << ":" << two_digits(fields.second) << " GMT";
  return out.str();
}

std::string format_pattern(const RuntimeTimeValue &value,
                           const std::string &pattern) {
  const LocalFields fields = local_fields(value);
  const ZoneInstantInfo info = zone_info_for_time(value);
  std::string out;
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] != '%' || i + 1U >= pattern.size()) {
      out.push_back(pattern[i]);
      continue;
    }
    ++i;
    std::string directive;
    if ((pattern[i] == '-' || pattern[i] == ':') && i + 1U < pattern.size()) {
      directive = pattern.substr(i, 2);
      ++i;
    } else {
      directive = pattern.substr(i, 1);
    }
    if (directive == "Y") {
      out += four_digits(fields.date.year);
    } else if (directive == "y") {
      out += two_digits(static_cast<int>(fields.date.year % 100));
    } else if (directive == "m") {
      out += two_digits(fields.date.month);
    } else if (directive == "-m") {
      out += std::to_string(fields.date.month);
    } else if (directive == "d") {
      out += two_digits(fields.date.day);
    } else if (directive == "-d") {
      out += std::to_string(fields.date.day);
    } else if (directive == "H") {
      out += two_digits(fields.hour);
    } else if (directive == "M") {
      out += two_digits(fields.minute);
    } else if (directive == "S") {
      out += two_digits(fields.second);
    } else if (directive == "N") {
      std::string nano = std::to_string(fields.nanosecond);
      nano.insert(nano.begin(), 9U - nano.size(), '0');
      out += nano;
    } else if (directive == "L") {
      out += two_digits(fields.nanosecond / 10000000);
      out += static_cast<char>('0' + (fields.nanosecond / 1000000) % 10);
    } else if (directive == "z") {
      out += offset_text(info.offset_seconds, false, false);
    } else if (directive == ":z") {
      out += offset_text(info.offset_seconds, true, false);
    } else if (directive == "Z") {
      out += info.abbreviation;
    } else if (directive == "F") {
      out += format_date(value);
    } else if (directive == "T") {
      out += two_digits(fields.hour) + ":" + two_digits(fields.minute) + ":" +
             two_digits(fields.second);
    } else if (directive == "%") {
      out.push_back('%');
    } else {
      out += "%" + directive;
    }
  }
  return out;
}

std::optional<std::string> format_time(const RuntimeTimeValue &value,
                                       const std::string &format,
                                       bool pattern) {
  if (pattern) {
    return format_pattern(value, format);
  }
  const std::string key = ascii_lower(format);
  if (key == "iso8601" || key == "rfc3339") {
    return format_iso8601(value, false);
  }
  if (key == "iso8601_zone") {
    return format_iso8601(value, true);
  }
  if (key == "date") {
    return format_date(value);
  }
  if (key == "datetime") {
    return format_datetime(value);
  }
  if (key == "ru_date") {
    return format_ru_date(value);
  }
  if (key == "ru_datetime") {
    return format_ru_datetime(value);
  }
  if (key == "ru_long") {
    return format_ru_long(value);
  }
  if (key == "http_date") {
    return format_http_date(value);
  }
  return std::nullopt;
}

bool format_arg(NativeStdlibCall &call, std::size_t index, std::string *format,
                bool *pattern) {
  const Value &arg = call.args[index];
  const std::optional<std::string> text = call.text_of(arg);
  if (!text.has_value()) {
    call.fault("TypeError", "time format must be Symbol or Str");
    return false;
  }
  *format = *text;
  *pattern = arg.is_string();
  return true;
}

SendStatus time_now(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(0) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const std::optional<RuntimeTimeValue> now = call.wall_time_now();
  if (!now.has_value()) {
    return SendStatus::Faulted;
  }
  RuntimeTimeValue value = *now;
  if (value.zone_name.empty()) {
    value.zone_name = "UTC";
  }
  *call.out = time_value(value);
  return SendStatus::Matched;
}

SendStatus time_monotonic(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(0) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const std::optional<RuntimeTimePeriodValue> period = call.monotonic_time();
  if (!period.has_value()) {
    return SendStatus::Faulted;
  }
  *call.out = period_value(*period);
  return SendStatus::Matched;
}

SendStatus time_epoch(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(0) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  *call.out = time_value(RuntimeTimeValue{0, 0, 0, "UTC", true});
  return SendStatus::Matched;
}

SendStatus time_utc(NativeStdlibCall &call) {
  if (!call.require_no_block() || call.args.size() != 3U ||
      !call.reject_unknown_keywords(
          {"hour", "minute", "second", "nanosecond"})) {
    if (call.args.size() != 3U) {
      call.fault("TypeError", "Time.utc expects year, month, and day");
    }
    return SendStatus::Faulted;
  }
  std::int64_t year = 0;
  std::int64_t month64 = 0;
  std::int64_t day64 = 0;
  if (!require_int(call.args[0], &year) ||
      !require_int(call.args[1], &month64) ||
      !require_int(call.args[2], &day64)) {
    return call.fault("TypeError", "Time.utc date components must be Int");
  }
  const std::optional<std::int64_t> hour = keyword_int(call, "hour", 0);
  const std::optional<std::int64_t> minute = keyword_int(call, "minute", 0);
  const std::optional<std::int64_t> second = keyword_int(call, "second", 0);
  const std::optional<std::int64_t> nanosecond =
      keyword_int(call, "nanosecond", 0);
  if (!hour.has_value() || !minute.has_value() || !second.has_value() ||
      !nanosecond.has_value()) {
    return SendStatus::Faulted;
  }
  if (month64 < 1 || month64 > 12 || day64 < 1 || day64 > 31 || *hour < 0 ||
      *hour > 23 || *minute < 0 || *minute > 59 || *second < 0 ||
      *second > 59 || *nanosecond < 0 || *nanosecond >= kNanosPerSecond) {
    return invalid_time_fault(call, "Time.utc component out of range");
  }
  const std::optional<RuntimeTimeValue> time = time_from_utc_fields(
      year, static_cast<int>(month64), static_cast<int>(day64),
      static_cast<int>(*hour), static_cast<int>(*minute),
      static_cast<int>(*second), static_cast<int>(*nanosecond));
  if (!time.has_value()) {
    return invalid_time_fault(call, "Time.utc date is invalid");
  }
  *call.out = time_value(*time);
  return SendStatus::Matched;
}

SendStatus time_from_unix(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({"nanosecond"})) {
    return SendStatus::Faulted;
  }
  std::int64_t seconds = 0;
  if (!require_int(call.args[0], &seconds)) {
    return call.fault("TypeError", "Time.from_unix expects Int seconds");
  }
  const std::optional<std::int64_t> nanosecond =
      keyword_int(call, "nanosecond", 0);
  if (!nanosecond.has_value()) {
    return SendStatus::Faulted;
  }
  if (*nanosecond < 0 || *nanosecond >= kNanosPerSecond) {
    return invalid_time_fault(call, "nanosecond must be in 0...999999999");
  }
  *call.out = time_value(
      RuntimeTimeValue{seconds, static_cast<std::uint32_t>(*nanosecond), 0,
                       "UTC", true});
  return SendStatus::Matched;
}

SendStatus time_from_unix_ms(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  std::int64_t milliseconds = 0;
  if (!require_int(call.args[0], &milliseconds)) {
    return call.fault("TypeError", "Time.from_unix_ms expects Int");
  }
  const std::int64_t seconds = floor_div_i64(milliseconds, 1000);
  const int millis = floor_mod_i64(milliseconds, 1000);
  *call.out = time_value(
      RuntimeTimeValue{seconds, static_cast<std::uint32_t>(millis * 1000000),
                       0, "UTC", true});
  return SendStatus::Matched;
}

SendStatus time_from_unix_ns(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  std::int64_t nanoseconds = 0;
  if (!require_int(call.args[0], &nanoseconds)) {
    return call.fault("TypeError", "Time.from_unix_ns expects Int");
  }
  const std::optional<RuntimeTimeValue> time =
      make_time_from_parts(0, nanoseconds);
  if (!time.has_value()) {
    return overflow_fault(call);
  }
  *call.out = time_value(*time);
  return SendStatus::Matched;
}

SendStatus time_parse(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({"format", "zone", "on_gap", "on_fold"})) {
    return SendStatus::Faulted;
  }
  const std::optional<std::string> text = call.text_of(call.args[0]);
  if (!text.has_value()) {
    return call.fault("TypeError", "Time.parse expects Str");
  }
  std::string format = "auto";
  bool pattern = false;
  const std::optional<Value> format_value = call.keyword("format");
  if (format_value.has_value()) {
    const std::optional<std::string> format_text = call.text_of(*format_value);
    if (!format_text.has_value()) {
      return call.fault("TypeError", "format must be Symbol or Str");
    }
    format = *format_text;
    pattern = format_value->is_string();
  }
  const std::optional<ParseZone> zone = parse_zone_keyword(call);
  if (!zone.has_value()) {
    return SendStatus::Faulted;
  }
  const std::optional<ResolvePolicy> policy =
      parse_resolve_policy_keywords(call);
  if (!policy.has_value()) {
    return SendStatus::Faulted;
  }
  const LocalResolveResult parsed =
      parse_time_text(*text, format, pattern, *zone, *policy);
  if (parsed.status == LocalResolveStatus::Invalid) {
    return call.fault("TimeParseError", "invalid time text");
  }
  if (parsed.status != LocalResolveStatus::Ok) {
    return local_resolve_fault(call, parsed.status);
  }
  *call.out = time_value(parsed.time);
  return SendStatus::Matched;
}

SendStatus time_time_zone_lookup(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const std::optional<RuntimeTimeZoneValue> zone =
      zone_from_value(call, call.args[0], true);
  if (!zone.has_value()) {
    return SendStatus::Faulted;
  }
  *call.out = zone_value(*zone);
  return SendStatus::Matched;
}

SendStatus time_epoch_component(NativeStdlibCall &call,
                                const RuntimeTimeValue &time) {
  if (!call.require_no_block() || !call.require_arity(0) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  if (call.selector == "unix_seconds") {
    *call.out = Value::integer(time.epoch_seconds);
    return SendStatus::Matched;
  }
  if (call.selector == "unix_milliseconds") {
    const __int128 value = static_cast<__int128>(time.epoch_seconds) * 1000 +
                           time.nanosecond / 1000000;
    const std::optional<std::int64_t> checked = checked_i128(value);
    if (!checked.has_value()) {
      return overflow_fault(call, "unix_milliseconds overflow");
    }
    *call.out = Value::integer(*checked);
    return SendStatus::Matched;
  }
  if (call.selector == "unix_nanoseconds") {
    const __int128 value =
        static_cast<__int128>(time.epoch_seconds) * kNanosPerSecond +
        time.nanosecond;
    const std::optional<std::int64_t> checked = checked_i128(value);
    if (!checked.has_value()) {
      return overflow_fault(call, "unix_nanoseconds overflow");
    }
    *call.out = Value::integer(*checked);
    return SendStatus::Matched;
  }
  return SendStatus::NotHandled;
}

SendStatus time_field(NativeStdlibCall &call, const RuntimeTimeValue &time) {
  if (!call.require_no_block() || !call.require_arity(0) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const LocalFields fields = local_fields(time);
  if (call.selector == "year") {
    *call.out = Value::integer(fields.date.year);
  } else if (call.selector == "month") {
    *call.out = Value::integer(fields.date.month);
  } else if (call.selector == "day") {
    *call.out = Value::integer(fields.date.day);
  } else if (call.selector == "hour") {
    *call.out = Value::integer(fields.hour);
  } else if (call.selector == "minute") {
    *call.out = Value::integer(fields.minute);
  } else if (call.selector == "second") {
    *call.out = Value::integer(fields.second);
  } else if (call.selector == "nanosecond") {
    *call.out = Value::integer(fields.nanosecond);
  } else if (call.selector == "weekday") {
    *call.out = Value::integer(iso_weekday(time));
  } else if (call.selector == "yearday") {
    const std::int64_t jan1 = days_from_civil(fields.date.year, 1, 1);
    const std::int64_t today =
        days_from_civil(fields.date.year, fields.date.month, fields.date.day);
    *call.out = Value::integer(today - jan1 + 1);
  } else {
    return SendStatus::NotHandled;
  }
  return SendStatus::Matched;
}

SendStatus time_compare_send(NativeStdlibCall &call,
                             const RuntimeTimeValue &lhs) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  if (!call.args[0].is_time() || call.args[0].as_time() == nullptr) {
    return call.fault("TypeError", "Time comparison expects Time");
  }
  const int order = compare_time(lhs, *call.args[0].as_time());
  if (call.selector == "<=>") {
    *call.out = Value::integer(order);
  } else if (call.selector == "<") {
    *call.out = Value::boolean(order < 0);
  } else if (call.selector == "<=") {
    *call.out = Value::boolean(order <= 0);
  } else if (call.selector == ">") {
    *call.out = Value::boolean(order > 0);
  } else if (call.selector == ">=") {
    *call.out = Value::boolean(order >= 0);
  } else {
    return SendStatus::NotHandled;
  }
  return SendStatus::Matched;
}

int week_start_to_iso(const std::string &name) {
  const std::string key = ascii_lower(name);
  if (key == "monday" || key == "mon") {
    return 1;
  }
  if (key == "tuesday" || key == "tue") {
    return 2;
  }
  if (key == "wednesday" || key == "wed") {
    return 3;
  }
  if (key == "thursday" || key == "thu") {
    return 4;
  }
  if (key == "friday" || key == "fri") {
    return 5;
  }
  if (key == "saturday" || key == "sat") {
    return 6;
  }
  if (key == "sunday" || key == "sun") {
    return 7;
  }
  return 0;
}

std::optional<RuntimeTimeValue> start_of_unit(NativeStdlibCall &call,
                                              const RuntimeTimeValue &time,
                                              const std::string &unit) {
  LocalFields fields = local_fields(time);
  const RuntimeTimeZoneValue zone = zone_of(time);
  if (unit == "minute") {
    fields.second = 0;
    fields.nanosecond = 0;
  } else if (unit == "hour") {
    fields.minute = 0;
    fields.second = 0;
    fields.nanosecond = 0;
  } else if (unit == "day") {
    fields.hour = 0;
    fields.minute = 0;
    fields.second = 0;
    fields.nanosecond = 0;
  } else if (unit == "week") {
    if (!call.reject_unknown_keywords({"week_start"})) {
      return std::nullopt;
    }
    int week_start = 1;
    if (const std::optional<Value> value = call.keyword("week_start")) {
      const std::optional<std::string> text = call.text_of(*value);
      if (!text.has_value()) {
        call.fault("TypeError", "week_start must be Symbol or Str");
        return std::nullopt;
      }
      week_start = week_start_to_iso(*text);
      if (week_start == 0) {
        call.fault("ArgumentError", "unknown week_start");
        return std::nullopt;
      }
    }
    const int current = iso_weekday(time);
    const int delta = floor_mod_i64(current - week_start, 7);
    const std::int64_t days =
        days_from_civil(fields.date.year, fields.date.month, fields.date.day) -
        delta;
    fields.date = civil_from_days(days);
    fields.hour = fields.minute = fields.second = fields.nanosecond = 0;
  } else if (unit == "month") {
    fields.date.day = 1;
    fields.hour = fields.minute = fields.second = fields.nanosecond = 0;
  } else if (unit == "quarter") {
    fields.date.month = ((fields.date.month - 1) / 3) * 3 + 1;
    fields.date.day = 1;
    fields.hour = fields.minute = fields.second = fields.nanosecond = 0;
  } else if (unit == "year") {
    fields.date.month = 1;
    fields.date.day = 1;
    fields.hour = fields.minute = fields.second = fields.nanosecond = 0;
  }
  return time_from_fields(fields, zone);
}

std::optional<RuntimeTimeValue> end_of_unit(NativeStdlibCall &call,
                                            const RuntimeTimeValue &time,
                                            const std::string &unit) {
  std::optional<RuntimeTimeValue> start = start_of_unit(call, time, unit);
  if (!start.has_value()) {
    return std::nullopt;
  }
  RuntimeTimePeriodValue step;
  if (unit == "minute") {
    step.nanoseconds = 60LL * kNanosPerSecond;
  } else if (unit == "hour") {
    step.nanoseconds = 3600LL * kNanosPerSecond;
  } else if (unit == "day") {
    step.days = 1;
  } else if (unit == "week") {
    step.days = 7;
  } else if (unit == "month") {
    step.months = 1;
  } else if (unit == "quarter") {
    step.months = 3;
  } else if (unit == "year") {
    step.months = 12;
  }
  step.nanoseconds -= 1;
  return apply_period_to_time(*start, step);
}

std::optional<std::string> boundary_unit_for_selector(std::string selector,
                                                      bool *end) {
  *end = false;
  std::string prefix = "start_of_";
  if (selector.compare(0, prefix.size(), prefix) == 0) {
    return selector.substr(prefix.size());
  }
  prefix = "beginning_of_";
  if (selector.compare(0, prefix.size(), prefix) == 0) {
    return selector.substr(prefix.size());
  }
  prefix = "end_of_";
  if (selector.compare(0, prefix.size(), prefix) == 0) {
    *end = true;
    return selector.substr(prefix.size());
  }
  return std::nullopt;
}

bool valid_boundary_unit(const std::string &unit) {
  return unit == "minute" || unit == "hour" || unit == "day" ||
         unit == "week" || unit == "month" || unit == "quarter" ||
         unit == "year";
}

SendStatus time_boundary_send(NativeStdlibCall &call,
                              const RuntimeTimeValue &time) {
  bool end = false;
  const std::optional<std::string> unit =
      boundary_unit_for_selector(call.selector, &end);
  if (!unit.has_value() || !valid_boundary_unit(*unit)) {
    return SendStatus::NotHandled;
  }
  if (!call.require_no_block() || !call.require_arity(0)) {
    return SendStatus::Faulted;
  }
  if (*unit != "week" && !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  std::optional<RuntimeTimeValue> result =
      end ? end_of_unit(call, time, *unit) : start_of_unit(call, time, *unit);
  if (!result.has_value()) {
    return SendStatus::Faulted;
  }
  *call.out = time_value(*result);
  return SendStatus::Matched;
}

SendStatus time_instance_dispatch(NativeStdlibCall &call) {
  const std::shared_ptr<RuntimeTimeValue> time = call.receiver.as_time();
  if (time == nullptr) {
    return call.fault("TypeError", "Time value is null");
  }
  if (call.selector == "iso8601") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out = call.string_value(runtime_time_to_iso8601(*time));
    return SendStatus::Matched;
  }
  if (call.selector == "to_str") {
    if (!call.require_no_block() || call.args.size() > 1U ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    std::string format = "iso8601";
    bool pattern = false;
    if (!call.args.empty() && !format_arg(call, 0, &format, &pattern)) {
      return SendStatus::Faulted;
    }
    const std::optional<std::string> text = format_time(*time, format, pattern);
    if (!text.has_value()) {
      return call.fault("ArgumentError", "unknown Time format");
    }
    *call.out = call.string_value(*text);
    return SendStatus::Matched;
  }
  if (call.selector == "inspect") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out = call.string_value(runtime_time_to_iso8601(*time));
    return SendStatus::Matched;
  }
  if (call.selector == "unix_seconds" || call.selector == "unix_milliseconds" ||
      call.selector == "unix_nanoseconds") {
    return time_epoch_component(call, *time);
  }
  if (call.selector == "year" || call.selector == "month" ||
      call.selector == "day" || call.selector == "hour" ||
      call.selector == "minute" || call.selector == "second" ||
      call.selector == "nanosecond" || call.selector == "weekday" ||
      call.selector == "yearday") {
    return time_field(call, *time);
  }
  if (call.selector == "time_zone") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out = zone_value(zone_of(*time));
    return SendStatus::Matched;
  }
  if (call.selector == "in_time_zone" || call.selector == "in_tz" ||
      call.selector == "as_time_zone" || call.selector == "as_tz") {
    if (!call.require_no_block() || !call.require_arity(1) ||
        !call.reject_unknown_keywords({"on_gap", "on_fold"})) {
      return SendStatus::Faulted;
    }
    const std::optional<RuntimeTimeZoneValue> zone =
        zone_from_value(call, call.args[0], true);
    if (!zone.has_value()) {
      return SendStatus::Faulted;
    }
    const std::optional<ResolvePolicy> policy =
        parse_resolve_policy_keywords(call);
    if (!policy.has_value()) {
      return SendStatus::Faulted;
    }
    RuntimeTimeValue out = *time;
    if (call.selector == "in_time_zone" || call.selector == "in_tz") {
      const ZoneInstantInfo info = zone_info_at(*zone, out.epoch_seconds);
      out.zone_offset_seconds = info.offset_seconds;
      out.zone_name = zone->name;
      out.zone_fixed_offset = zone->fixed_offset;
    } else {
      const LocalFields fields = local_fields(*time);
      const LocalResolveResult resolved =
          resolve_local_fields(fields, *zone, *policy);
      if (resolved.status != LocalResolveStatus::Ok) {
        return local_resolve_fault(call, resolved.status);
      }
      out = resolved.time;
    }
    *call.out = time_value(out);
    return SendStatus::Matched;
  }
  if (const SendStatus boundary = time_boundary_send(call, *time);
      boundary != SendStatus::NotHandled) {
    return boundary;
  }
  if (call.selector == "+" || call.selector == "-") {
    if (!call.require_no_block() || !call.require_arity(1) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    if (call.args[0].is_time_period() &&
        call.args[0].as_time_period() != nullptr) {
      RuntimeTimePeriodValue period = *call.args[0].as_time_period();
      if (call.selector == "-") {
        const std::optional<RuntimeTimePeriodValue> negated =
            period_add(RuntimeTimePeriodValue{}, period, -1);
        if (!negated.has_value()) {
          return overflow_fault(call, "TimePeriod negation overflow");
        }
        period = *negated;
      }
      const std::optional<RuntimeTimeValue> result =
          apply_period_to_time(*time, period);
      if (!result.has_value()) {
        return overflow_fault(call);
      }
      *call.out = time_value(*result);
      return SendStatus::Matched;
    }
    if (call.selector == "-" && call.args[0].is_time() &&
        call.args[0].as_time() != nullptr) {
      const std::optional<RuntimeTimePeriodValue> period =
          period_between(*time, *call.args[0].as_time());
      if (!period.has_value()) {
        return overflow_fault(call, "Time difference overflow");
      }
      *call.out = period_value(*period);
      return SendStatus::Matched;
    }
    return call.fault("TypeError", "Time arithmetic expects TimePeriod");
  }
  if (call.selector == "<" || call.selector == "<=" || call.selector == ">" ||
      call.selector == ">=" || call.selector == "<=>") {
    return time_compare_send(call, *time);
  }
  return SendStatus::NotHandled;
}

SendStatus zone_type_dispatch(NativeStdlibCall &call) {
  if (call.selector == "utc") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out = zone_value(utc_zone());
    return SendStatus::Matched;
  }
  if (call.selector == "offset") {
    if (!call.require_no_block() || !call.reject_unknown_keywords({"seconds"})) {
      return SendStatus::Faulted;
    }
    if (call.args.size() == 1U) {
      const std::optional<RuntimeTimeZoneValue> zone =
          zone_from_value(call, call.args[0], true);
      if (!zone.has_value()) {
        return SendStatus::Faulted;
      }
      *call.out = zone_value(RuntimeTimeZoneValue{
          zone->offset_seconds, offset_name(zone->offset_seconds), true});
      return SendStatus::Matched;
    }
    if (!call.args.empty()) {
      return call.fault("TypeError", "TimeZone.offset expects Str or seconds:");
    }
    const std::optional<std::int64_t> seconds =
        keyword_int(call, "seconds", 0);
    if (!seconds.has_value()) {
      return SendStatus::Faulted;
    }
    if (*seconds < -23 * 3600 - 59 * 60 ||
        *seconds > 23 * 3600 + 59 * 60) {
      return call.fault("ArgumentError", "time zone offset out of range");
    }
    *call.out = zone_value(RuntimeTimeZoneValue{
        static_cast<std::int32_t>(*seconds),
        offset_name(static_cast<int>(*seconds)), true});
    return SendStatus::Matched;
  }
  if (call.selector == "[]" || call.selector == "find") {
    if (!call.require_no_block() || !call.require_arity(1) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    const std::optional<RuntimeTimeZoneValue> zone =
        zone_from_value(call, call.args[0], call.selector == "[]");
    if (!zone.has_value()) {
      if (call.selector == "find") {
        *call.out = Value::null();
        return SendStatus::Matched;
      }
      return SendStatus::Faulted;
    }
    *call.out = zone_value(*zone);
    return SendStatus::Matched;
  }
  if (call.selector == "local") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    const std::optional<std::string> name = local_zone_name();
    if (!name.has_value()) {
      return call.fault("TimeZoneLookupError",
                        "unable to resolve local time zone");
    }
    const std::optional<RuntimeTimeZoneValue> zone = lookup_time_zone(*name);
    if (!zone.has_value()) {
      return call.fault("TimeZoneLookupError",
                        "unable to resolve local time zone");
    }
    *call.out = zone_value(*zone);
    return SendStatus::Matched;
  }
  return SendStatus::NotHandled;
}

SendStatus zone_instance_dispatch(NativeStdlibCall &call) {
  const std::shared_ptr<RuntimeTimeZoneValue> zone =
      call.receiver.as_time_zone();
  if (zone == nullptr) {
    return call.fault("TypeError", "TimeZone value is null");
  }
  if (call.selector == "name" || call.selector == "to_str" ||
      call.selector == "inspect") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out = call.string_value(
        call.selector == "name" ? zone->name : runtime_time_zone_to_string(*zone));
    return SendStatus::Matched;
  }
  if (call.selector == "fixed?") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out = Value::boolean(zone->fixed_offset);
    return SendStatus::Matched;
  }
  if (call.selector == "offset_seconds") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out = Value::integer(zone->offset_seconds);
    return SendStatus::Matched;
  }
  if (call.selector == "offset_at") {
    if (!call.require_no_block() || !call.require_arity(1) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    if (!call.args[0].is_time() || call.args[0].as_time() == nullptr) {
      return call.fault("TypeError", "offset_at expects Time");
    }
    const ZoneInstantInfo info =
        zone_info_at(*zone, call.args[0].as_time()->epoch_seconds);
    *call.out = period_value(
        RuntimeTimePeriodValue{0, 0,
                               static_cast<std::int64_t>(info.offset_seconds) *
                                   kNanosPerSecond});
    return SendStatus::Matched;
  }
  if (call.selector == "abbreviation_at") {
    if (!call.require_no_block() || !call.require_arity(1) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    if (!call.args[0].is_time() || call.args[0].as_time() == nullptr) {
      return call.fault("TypeError", "abbreviation_at expects Time");
    }
    const ZoneInstantInfo info =
        zone_info_at(*zone, call.args[0].as_time()->epoch_seconds);
    *call.out = call.string_value(info.abbreviation);
    return SendStatus::Matched;
  }
  return SendStatus::NotHandled;
}

SendStatus period_compare_send(NativeStdlibCall &call,
                               const RuntimeTimePeriodValue &lhs) {
  if (!call.require_no_block() || !call.require_arity(1) ||
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  if (!call.args[0].is_time_period() ||
      call.args[0].as_time_period() == nullptr) {
    return call.fault("TypeError", "TimePeriod comparison expects TimePeriod");
  }
  const std::optional<__int128> left = fixed_period_nanoseconds(lhs);
  const std::optional<__int128> right =
      fixed_period_nanoseconds(*call.args[0].as_time_period());
  if (!left.has_value() || !right.has_value()) {
    return call.fault("TypeError",
                      "calendar TimePeriod values cannot be ordered");
  }
  const int order = compare_i128(*left, *right);
  if (call.selector == "<=>") {
    *call.out = Value::integer(order);
  } else if (call.selector == "<") {
    *call.out = Value::boolean(order < 0);
  } else if (call.selector == "<=") {
    *call.out = Value::boolean(order <= 0);
  } else if (call.selector == ">") {
    *call.out = Value::boolean(order > 0);
  } else if (call.selector == ">=") {
    *call.out = Value::boolean(order >= 0);
  } else {
    return SendStatus::NotHandled;
  }
  return SendStatus::Matched;
}

SendStatus period_instance_dispatch(NativeStdlibCall &call) {
  const std::shared_ptr<RuntimeTimePeriodValue> period =
      call.receiver.as_time_period();
  if (period == nullptr) {
    return call.fault("TypeError", "TimePeriod value is null");
  }
  if (call.selector == "to_str" || call.selector == "inspect") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out = call.string_value(runtime_time_period_to_string(*period));
    return SendStatus::Matched;
  }
  if (call.selector == "months" || call.selector == "days" ||
      call.selector == "nanoseconds") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    if (call.selector == "months") {
      *call.out = Value::integer(period->months);
    } else if (call.selector == "days") {
      *call.out = Value::integer(period->days);
    } else {
      *call.out = Value::integer(period->nanoseconds);
    }
    return SendStatus::Matched;
  }
  if (call.selector == "fixed?") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out = Value::boolean(period->months == 0);
    return SendStatus::Matched;
  }
  if (call.selector == "total_nanoseconds") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    const std::optional<__int128> total = fixed_period_nanoseconds(*period);
    if (!total.has_value()) {
      return call.fault("TypeError",
                        "calendar TimePeriod has no fixed total_nanoseconds");
    }
    const std::optional<std::int64_t> checked = checked_i128(*total);
    if (!checked.has_value()) {
      return overflow_fault(call, "total_nanoseconds overflow");
    }
    *call.out = Value::integer(*checked);
    return SendStatus::Matched;
  }
  if (call.selector == "u+" || call.selector == "u-") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    if (call.selector == "u+") {
      *call.out = call.receiver;
    } else {
      const std::optional<RuntimeTimePeriodValue> negated =
          period_add(RuntimeTimePeriodValue{}, *period, -1);
      if (!negated.has_value()) {
        return overflow_fault(call, "TimePeriod negation overflow");
      }
      *call.out = period_value(*negated);
    }
    return SendStatus::Matched;
  }
  if (call.selector == "+" || call.selector == "-") {
    if (!call.require_no_block() || !call.require_arity(1) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    if (call.selector == "+" && call.args[0].is_time() &&
        call.args[0].as_time() != nullptr) {
      const std::optional<RuntimeTimeValue> result =
          apply_period_to_time(*call.args[0].as_time(), *period);
      if (!result.has_value()) {
        return overflow_fault(call);
      }
      *call.out = time_value(*result);
      return SendStatus::Matched;
    }
    if (!call.args[0].is_time_period() ||
        call.args[0].as_time_period() == nullptr) {
      return call.fault("TypeError",
                        "TimePeriod arithmetic expects TimePeriod");
    }
    const std::optional<RuntimeTimePeriodValue> result = period_add(
        *period, *call.args[0].as_time_period(), call.selector == "-" ? -1 : 1);
    if (!result.has_value()) {
      return overflow_fault(call, "TimePeriod overflow");
    }
    *call.out = period_value(*result);
    return SendStatus::Matched;
  }
  if (call.selector == "<" || call.selector == "<=" || call.selector == ">" ||
      call.selector == ">=" || call.selector == "<=>") {
    return period_compare_send(call, *period);
  }
  return SendStatus::NotHandled;
}

SendStatus time_dispatch(NativeStdlibCall &call) {
  if (call.receiver.is_time()) {
    return time_instance_dispatch(call);
  }
  if (call.receiver.is_time_zone()) {
    return zone_instance_dispatch(call);
  }
  if (call.receiver.is_time_period()) {
    return period_instance_dispatch(call);
  }
  if (!call.receiver.is_native_type()) {
    return SendStatus::NotHandled;
  }
  if (call.kind == RuntimeNativeTypeKind::TimeZone) {
    return zone_type_dispatch(call);
  }
  if (call.kind == RuntimeNativeTypeKind::TimePeriod) {
    return SendStatus::NotHandled;
  }
  if (call.kind != RuntimeNativeTypeKind::Time) {
    return SendStatus::NotHandled;
  }
  if (call.selector == "now") {
    return time_now(call);
  }
  if (call.selector == "monotonic") {
    return time_monotonic(call);
  }
  if (call.selector == "epoch") {
    return time_epoch(call);
  }
  if (call.selector == "utc") {
    return time_utc(call);
  }
  if (call.selector == "from_unix") {
    return time_from_unix(call);
  }
  if (call.selector == "from_unix_ms") {
    return time_from_unix_ms(call);
  }
  if (call.selector == "from_unix_ns") {
    return time_from_unix_ns(call);
  }
  if (call.selector == "parse") {
    return time_parse(call);
  }
  if (call.selector == "time_zone") {
    return time_time_zone_lookup(call);
  }
  return SendStatus::NotHandled;
}

} // namespace

std::string runtime_time_to_iso8601(const RuntimeTimeValue &value) {
  return format_iso8601(value, false);
}

std::string runtime_time_zone_to_string(const RuntimeTimeZoneValue &value) {
  return value.name;
}

std::string runtime_time_period_to_string(const RuntimeTimePeriodValue &value) {
  if (value.months == 0 && value.days == 0 && value.nanoseconds == 0) {
    return "0.nanoseconds";
  }
  std::ostringstream out;
  bool first = true;
  auto append = [&](std::int64_t amount, const char *unit) {
    if (amount == 0) {
      return;
    }
    if (!first) {
      out << " + ";
    }
    first = false;
    out << amount << "." << unit;
  };
  append(value.months, "months");
  append(value.days, "days");
  append(value.nanoseconds, "nanoseconds");
  return out.str();
}

RuntimeNativeModuleDescriptor time_module_descriptor() {
  return {{{"Time", RuntimeNativeTypeKind::Time},
           {"TimePeriod", RuntimeNativeTypeKind::TimePeriod},
           {"TimeZone", RuntimeNativeTypeKind::TimeZone}},
          {{RuntimeNativeTypeKind::Time, &time_dispatch},
           {RuntimeNativeTypeKind::TimePeriod, &time_dispatch},
           {RuntimeNativeTypeKind::TimeZone, &time_dispatch}},
          {},
          {},
          {{"TimeError", "Exception"},
           {"TimeParseError", "TimeError"},
           {"TimeZoneError", "TimeError"},
           {"TimeZoneLookupError", "TimeZoneError"},
           {"TimeZoneGapError", "TimeZoneError"},
           {"TimeZoneAmbiguousError", "TimeZoneError"}}};
}

void register_time(NativeRegistry &registry) {
  register_native_module_descriptor(registry, time_module_descriptor());
}

void register_time_runtime_module(RuntimeModuleRegistry &modules,
                                  RuntimeDispatchRegistry &dispatch,
                                  RuntimeTypeRegistry &types,
                                  RuntimeErrorRegistry *errors) {
  const RuntimeNativeModuleDescriptor descriptor = time_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
  if (errors != nullptr) {
    register_runtime_error_descriptor(*errors, descriptor);
  }
}

} // namespace amber::runtime
