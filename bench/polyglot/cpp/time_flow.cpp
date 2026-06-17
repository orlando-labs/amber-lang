#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

constexpr std::int64_t kNanosPerSecond = 1000000000LL;
constexpr std::int64_t kSecondsPerDay = 86400LL;

struct CivilDate {
  std::int64_t year;
  int month;
  int day;
};

struct Fields {
  CivilDate date;
  int hour;
  int minute;
  int second;
  int nanosecond;
};

struct Period;

struct Instant {
  std::int64_t seconds;
  int nanosecond;

  static Instant utc(std::int64_t year, int month, int day, int hour = 0,
                     int minute = 0, int second = 0, int nanosecond = 0);
  static Instant from_unix_ms(std::int64_t milliseconds);
  static Instant from_unix_ns(std::int64_t nanoseconds);
  static Instant parse_iso8601(const std::string &text);

  Fields fields() const;
  std::string iso8601() const;
  std::int64_t unix_nanoseconds() const;
  Instant operator+(const Period &period) const;
  Period operator-(const Instant &other) const;
  bool operator>(const Instant &other) const {
    return seconds > other.seconds ||
           (seconds == other.seconds && nanosecond > other.nanosecond);
  }
};

struct Period {
  std::int64_t months = 0;
  std::int64_t days = 0;
  std::int64_t nanoseconds = 0;

  Period operator+(const Period &other) const {
    return Period{months + other.months, days + other.days,
                  nanoseconds + other.nanoseconds};
  }

  Instant operator+(const Instant &instant) const { return instant + *this; }

  std::int64_t total_nanoseconds() const {
    return days * kSecondsPerDay * kNanosPerSecond + nanoseconds;
  }
};

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
  std::int64_t q = a / b;
  const std::int64_t r = a % b;
  if (r != 0 && ((r < 0) != (b < 0))) {
    --q;
  }
  return q;
}

int floor_mod(std::int64_t a, std::int64_t b) {
  return static_cast<int>(a - floor_div(a, b) * b);
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

Fields Instant::fields() const {
  const std::int64_t days = floor_div(seconds, kSecondsPerDay);
  const int second_of_day = floor_mod(seconds, kSecondsPerDay);
  return Fields{civil_from_days(days), second_of_day / 3600,
                (second_of_day / 60) % 60, second_of_day % 60, nanosecond};
}

Instant Instant::utc(std::int64_t year, int month, int day, int hour,
                     int minute, int second, int nanosecond) {
  return Instant{days_from_civil(year, month, day) * kSecondsPerDay +
                     hour * 3600 + minute * 60 + second,
                 nanosecond};
}

Instant Instant::from_unix_ms(std::int64_t milliseconds) {
  const std::int64_t seconds = floor_div(milliseconds, 1000);
  const int millis = floor_mod(milliseconds, 1000);
  return Instant{seconds, millis * 1000000};
}

Instant Instant::from_unix_ns(std::int64_t nanoseconds) {
  const std::int64_t seconds = floor_div(nanoseconds, kNanosPerSecond);
  const int nanos = floor_mod(nanoseconds, kNanosPerSecond);
  return Instant{seconds, nanos};
}

Instant Instant::parse_iso8601(const std::string &text) {
  const std::int64_t year = std::stoll(text.substr(0, 4));
  const int month = std::stoi(text.substr(5, 2));
  const int day = std::stoi(text.substr(8, 2));
  const int hour = std::stoi(text.substr(11, 2));
  const int minute = std::stoi(text.substr(14, 2));
  const int second = std::stoi(text.substr(17, 2));
  const std::size_t offset_pos = text.size() - 6;
  std::string fraction = text.substr(20, offset_pos - 20);
  while (fraction.size() < 9U) {
    fraction.push_back('0');
  }
  const int nanosecond = std::stoi(fraction.substr(0, 9));
  const int sign = text[offset_pos] == '-' ? -1 : 1;
  const int offset_seconds =
      sign * (std::stoi(text.substr(offset_pos + 1, 2)) * 3600 +
              std::stoi(text.substr(offset_pos + 4, 2)) * 60);
  Instant local = Instant::utc(year, month, day, hour, minute, second,
                               nanosecond);
  local.seconds -= offset_seconds;
  return local;
}

std::string Instant::iso8601() const {
  const Fields f = fields();
  std::ostringstream out;
  out << std::setw(4) << std::setfill('0') << f.date.year << "-"
      << std::setw(2) << f.date.month << "-" << std::setw(2) << f.date.day
      << "T" << std::setw(2) << f.hour << ":" << std::setw(2) << f.minute
      << ":" << std::setw(2) << f.second;
  if (nanosecond != 0) {
    std::string fraction = std::to_string(nanosecond);
    fraction.insert(fraction.begin(), 9U - fraction.size(), '0');
    while (!fraction.empty() && fraction.back() == '0') {
      fraction.pop_back();
    }
    out << "." << fraction;
  }
  out << "Z";
  return out.str();
}

std::int64_t Instant::unix_nanoseconds() const {
  return seconds * kNanosPerSecond + nanosecond;
}

Instant Instant::operator+(const Period &period) const {
  Fields f = fields();
  const std::int64_t total_month =
      f.date.year * 12 + (f.date.month - 1) + period.months;
  const std::int64_t year = floor_div(total_month, 12);
  const int month = static_cast<int>(total_month - year * 12) + 1;
  f.date.year = year;
  f.date.month = month;
  f.date.day = std::min(f.date.day, days_in_month(year, month));
  const std::int64_t base_days =
      days_from_civil(f.date.year, f.date.month, f.date.day) + period.days;
  const std::int64_t total_nanos = f.nanosecond + period.nanoseconds;
  const std::int64_t second_delta = floor_div(total_nanos, kNanosPerSecond);
  const int nanos = floor_mod(total_nanos, kNanosPerSecond);
  return Instant{base_days * kSecondsPerDay + f.hour * 3600 + f.minute * 60 +
                     f.second + second_delta,
                 nanos};
}

Period Instant::operator-(const Instant &other) const {
  return Period{0, 0, unix_nanoseconds() - other.unix_nanoseconds()};
}

Period months(std::int64_t value) { return Period{value, 0, 0}; }
Period days(std::int64_t value) { return Period{0, value, 0}; }
Period seconds(std::int64_t value) {
  return Period{0, 0, value * kNanosPerSecond};
}
Period nanoseconds(std::int64_t value) { return Period{0, 0, value}; }

std::int64_t positive_mod(std::int64_t value, std::int64_t modulus) {
  const std::int64_t result = value % modulus;
  return result < 0 ? result + modulus : result;
}

} // namespace

int main() {
  std::int64_t checksum = 0;
  const Instant anchor =
      Instant::parse_iso8601("2026-06-17T03:04:05.123456789+03:00");

  for (std::int64_t i = 0; i < 12000; ++i) {
    const Instant base =
        Instant::utc(2020 + (i % 7), 1 + (i % 12), 25 + (i % 4), i % 24,
                     (i * 7) % 60, (i * 11) % 60,
                     (i * 1234567) % 1000000000);
    Period period = months((i % 15) + 1);
    period = period + days(i % 21);
    period = period + seconds(i % 3600);
    period = period + nanoseconds((i % 1000) * 1000);
    const Instant shifted = base + period;
    const Period elapsed = shifted - base;
    const Instant epoch = Instant::from_unix_ms(1700000000000LL + i * 37);
    const Instant tiny =
        Instant::from_unix_ns(1000000000LL + i * 1000003 + 999);
    const Instant clamp = Instant::utc(2024, 1, 31) + months((i % 3) + 1);
    const Instant probe = seconds(5) + days(1) + anchor;
    const Fields shifted_fields = shifted.fields();
    const Fields epoch_fields = epoch.fields();
    const Fields clamp_fields = clamp.fields();
    const Fields probe_fields = probe.fields();

    checksum += shifted_fields.date.year * 37;
    checksum += shifted_fields.date.month * 31;
    checksum += shifted_fields.date.day * 29;
    checksum += shifted_fields.hour * 23;
    checksum += shifted_fields.minute * 19;
    checksum += shifted_fields.second * 17;
    checksum += shifted.nanosecond % 1000003;
    checksum += positive_mod(elapsed.total_nanoseconds(), 1000003);
    checksum += epoch_fields.second + epoch.nanosecond % 1009;
    checksum += tiny.unix_nanoseconds() % 9973;
    checksum += clamp_fields.date.day * 13 + clamp_fields.date.month;
    checksum += probe_fields.date.day + probe_fields.second;
    if (i % 97 == 0) {
      checksum += static_cast<std::int64_t>(shifted.iso8601().size());
    }
    checksum += shifted > base ? 7 : 3;
    checksum %= 2147483647;
  }

  std::cout << checksum << "\n";
  return 0;
}
