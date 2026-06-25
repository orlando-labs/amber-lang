// Time stdlib library (DESIGN-stdlib-next-libs-order-2026-06-15 §4.5).
//
// v1 surface:
//   Time.now / Time.monotonic / Time.epoch
//   Time.utc(y, m, d, hour:, minute:, second:, nanosecond:)
//   Time.from_unix(seconds, nanosecond: 0)
//   Time.from_unix_ms(ms) / Time.from_unix_ns(ns)
//   Time.parse(iso8601)
//   Time values: UTC calendar fields, epoch fields, ISO formatting, arithmetic
//   TimePeriod values: unit-literal result, arithmetic, fixed-duration compare

#include "runtime/stdlib_registry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace amber::runtime {

namespace {

constexpr std::int64_t kNanosPerSecond = 1000000000LL;
constexpr std::int64_t kSecondsPerDay = 86400LL;

struct CivilDate {
  std::int64_t year = 1970;
  int month = 1;
  int day = 1;
};

struct UtcFields {
  CivilDate date;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int nanosecond = 0;
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
  const unsigned doe =
      yoe * 365U + yoe / 4U - yoe / 100U + doy;
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

std::optional<RuntimeTimeValue> make_time_from_parts(__int128 seconds,
                                                     __int128 nanosecond) {
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
  return out;
}

Value time_value(RuntimeTimeValue time) {
  return Value::time(std::make_shared<RuntimeTimeValue>(time));
}

Value period_value(RuntimeTimePeriodValue period) {
  return Value::time_period(
      std::make_shared<RuntimeTimePeriodValue>(period));
}

UtcFields utc_fields(const RuntimeTimeValue &time) {
  const std::int64_t days = floor_div_i64(time.epoch_seconds, kSecondsPerDay);
  const int second_of_day = floor_mod_i64(time.epoch_seconds, kSecondsPerDay);
  UtcFields out;
  out.date = civil_from_days(days);
  out.hour = second_of_day / 3600;
  out.minute = (second_of_day / 60) % 60;
  out.second = second_of_day % 60;
  out.nanosecond = static_cast<int>(time.nanosecond);
  return out;
}

std::optional<RuntimeTimeValue> time_from_utc_fields(
    std::int64_t year, int month, int day, int hour, int minute, int second,
    int nanosecond) {
  if (!valid_date(year, month, day) || hour < 0 || hour > 23 || minute < 0 ||
      minute > 59 || second < 0 || second > 59 || nanosecond < 0 ||
      nanosecond >= kNanosPerSecond) {
    return std::nullopt;
  }
  const __int128 days = days_from_civil(year, month, day);
  const __int128 seconds =
      days * kSecondsPerDay + hour * 3600 + minute * 60 + second;
  return make_time_from_parts(seconds, nanosecond);
}

std::optional<RuntimeTimeValue> apply_period_to_time(
    const RuntimeTimeValue &time, const RuntimeTimePeriodValue &period) {
  UtcFields fields = utc_fields(time);
  if (period.months != 0) {
    const __int128 total_month =
        static_cast<__int128>(fields.date.year) * 12 +
        (fields.date.month - 1) + period.months;
    const __int128 year = floor_div_i128(total_month, 12);
    const int month =
        static_cast<int>(total_month - year * 12) + 1;
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
  const __int128 seconds =
      base_days * kSecondsPerDay + fields.hour * 3600 + fields.minute * 60 +
      fields.second;
  return make_time_from_parts(seconds, static_cast<__int128>(fields.nanosecond) +
                                           period.nanoseconds);
}

std::optional<RuntimeTimePeriodValue>
period_add(const RuntimeTimePeriodValue &lhs,
           const RuntimeTimePeriodValue &rhs, int sign = 1) {
  RuntimeTimePeriodValue out;
  const std::optional<std::int64_t> months = checked_i128(
      static_cast<__int128>(lhs.months) + sign * static_cast<__int128>(rhs.months));
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
  const __int128 lhs_ns = static_cast<__int128>(lhs.epoch_seconds) *
                              kNanosPerSecond +
                          lhs.nanosecond;
  const __int128 rhs_ns = static_cast<__int128>(rhs.epoch_seconds) *
                              kNanosPerSecond +
                          rhs.nanosecond;
  const std::optional<std::int64_t> nanos = checked_i128(lhs_ns - rhs_ns);
  if (!nanos.has_value()) {
    return std::nullopt;
  }
  return RuntimeTimePeriodValue{0, 0, *nanos};
}

std::optional<__int128> fixed_period_nanoseconds(
    const RuntimeTimePeriodValue &period) {
  if (period.months != 0) {
    return std::nullopt;
  }
  return static_cast<__int128>(period.days) * kSecondsPerDay *
             kNanosPerSecond +
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

bool parse_year4(const std::string &text, std::size_t offset,
                 std::int64_t *out) {
  int year = 0;
  if (!parse_fixed_digits(text, offset, 4, &year)) {
    return false;
  }
  *out = year;
  return true;
}

std::optional<RuntimeTimeValue> parse_iso8601_utc(const std::string &text) {
  std::size_t pos = 0;
  std::int64_t year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parse_year4(text, pos, &year)) {
    return std::nullopt;
  }
  pos += 4;
  if (pos >= text.size() || text[pos++] != '-' ||
      !parse_fixed_digits(text, pos, 2, &month)) {
    return std::nullopt;
  }
  pos += 2;
  if (pos >= text.size() || text[pos++] != '-' ||
      !parse_fixed_digits(text, pos, 2, &day)) {
    return std::nullopt;
  }
  pos += 2;
  if (pos >= text.size() || (text[pos] != 'T' && text[pos] != 't')) {
    return std::nullopt;
  }
  ++pos;
  if (!parse_fixed_digits(text, pos, 2, &hour)) {
    return std::nullopt;
  }
  pos += 2;
  if (pos >= text.size() || text[pos++] != ':' ||
      !parse_fixed_digits(text, pos, 2, &minute)) {
    return std::nullopt;
  }
  pos += 2;
  if (pos >= text.size() || text[pos++] != ':' ||
      !parse_fixed_digits(text, pos, 2, &second)) {
    return std::nullopt;
  }
  pos += 2;
  int nanosecond = 0;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    int digits = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      if (digits < 9) {
        nanosecond = nanosecond * 10 + (text[pos] - '0');
      }
      ++digits;
      ++pos;
    }
    if (digits == 0 || digits > 9) {
      return std::nullopt;
    }
    for (; digits < 9; ++digits) {
      nanosecond *= 10;
    }
  }
  int offset_seconds = 0;
  if (pos < text.size() && (text[pos] == 'Z' || text[pos] == 'z')) {
    ++pos;
  } else if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
    const int sign = text[pos] == '-' ? -1 : 1;
    ++pos;
    int offset_hour = 0;
    int offset_minute = 0;
    if (!parse_fixed_digits(text, pos, 2, &offset_hour)) {
      return std::nullopt;
    }
    pos += 2;
    if (pos >= text.size() || text[pos++] != ':' ||
        !parse_fixed_digits(text, pos, 2, &offset_minute)) {
      return std::nullopt;
    }
    pos += 2;
    if (offset_hour > 23 || offset_minute > 59) {
      return std::nullopt;
    }
    offset_seconds = sign * (offset_hour * 3600 + offset_minute * 60);
  } else {
    return std::nullopt;
  }
  if (pos != text.size()) {
    return std::nullopt;
  }
  std::optional<RuntimeTimeValue> local =
      time_from_utc_fields(year, month, day, hour, minute, second, nanosecond);
  if (!local.has_value()) {
    return std::nullopt;
  }
  return make_time_from_parts(
      static_cast<__int128>(local->epoch_seconds) - offset_seconds,
      local->nanosecond);
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
  *call.out = time_value(*now);
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
  *call.out = time_value(RuntimeTimeValue{0, 0});
  return SendStatus::Matched;
}

SendStatus time_utc(NativeStdlibCall &call) {
  if (!call.require_no_block() || call.args.size() != 3U ||
      !call.reject_unknown_keywords({"hour", "minute", "second",
                                     "nanosecond"})) {
    if (call.args.size() != 3U) {
      call.fault("TypeError", "Time.utc expects year, month, and day");
    }
    return SendStatus::Faulted;
  }
  std::int64_t year = 0;
  std::int64_t month64 = 0;
  std::int64_t day64 = 0;
  if (!require_int(call.args[0], &year) || !require_int(call.args[1], &month64) ||
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
  if (month64 < 1 || month64 > 12 || day64 < 1 || day64 > 31 ||
      *hour < 0 || *hour > 23 || *minute < 0 || *minute > 59 ||
      *second < 0 || *second > 59 || *nanosecond < 0 ||
      *nanosecond >= kNanosPerSecond) {
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
  *call.out = time_value(RuntimeTimeValue{
      seconds, static_cast<std::uint32_t>(*nanosecond)});
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
  *call.out = time_value(RuntimeTimeValue{
      seconds, static_cast<std::uint32_t>(millis * 1000000)});
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
      !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const std::optional<std::string> text = call.text_of(call.args[0]);
  if (!text.has_value()) {
    return call.fault("TypeError", "Time.parse expects Str");
  }
  const std::optional<RuntimeTimeValue> parsed = parse_iso8601_utc(*text);
  if (!parsed.has_value()) {
    return call.fault("TimeParseError", "invalid ISO-8601 UTC instant");
  }
  *call.out = time_value(*parsed);
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
    const __int128 value =
        static_cast<__int128>(time.epoch_seconds) * 1000 +
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
  const UtcFields fields = utc_fields(time);
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
    const std::int64_t days =
        floor_div_i64(time.epoch_seconds, kSecondsPerDay);
    *call.out = Value::integer(floor_mod_i64(days + 3, 7) + 1);
  } else if (call.selector == "yearday") {
    const std::int64_t jan1 =
        days_from_civil(fields.date.year, 1, 1);
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

SendStatus time_instance_dispatch(NativeStdlibCall &call) {
  const std::shared_ptr<RuntimeTimeValue> time = call.receiver.as_time();
  if (time == nullptr) {
    return call.fault("TypeError", "Time value is null");
  }
  if (call.selector == "iso8601" || call.selector == "to_str" ||
      call.selector == "inspect") {
    if (!call.require_no_block() || !call.require_arity(0) ||
        !call.reject_unknown_keywords({})) {
      return SendStatus::Faulted;
    }
    *call.out = call.string_value(runtime_time_to_iso8601(*time));
    return SendStatus::Matched;
  }
  if (call.selector == "unix_seconds" ||
      call.selector == "unix_milliseconds" ||
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
    const std::optional<RuntimeTimePeriodValue> result =
        period_add(*period, *call.args[0].as_time_period(),
                   call.selector == "-" ? -1 : 1);
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
  if (call.receiver.is_time_period()) {
    return period_instance_dispatch(call);
  }
  if (!call.receiver.is_native_type()) {
    return SendStatus::NotHandled;
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
  return SendStatus::NotHandled;
}

} // namespace

std::string runtime_time_to_iso8601(const RuntimeTimeValue &value) {
  const UtcFields fields = utc_fields(value);
  std::ostringstream out;
  out << std::setw(4) << std::setfill('0') << fields.date.year << "-"
      << std::setw(2) << fields.date.month << "-" << std::setw(2)
      << fields.date.day << "T" << std::setw(2) << fields.hour << ":"
      << std::setw(2) << fields.minute << ":" << std::setw(2)
      << fields.second;
  if (fields.nanosecond != 0) {
    std::string fraction = std::to_string(fields.nanosecond);
    fraction.insert(fraction.begin(), 9U - fraction.size(), '0');
    while (!fraction.empty() && fraction.back() == '0') {
      fraction.pop_back();
    }
    out << "." << fraction;
  }
  out << "Z";
  return out.str();
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
           {"TimePeriod", RuntimeNativeTypeKind::TimePeriod}},
          {{RuntimeNativeTypeKind::Time, &time_dispatch},
           {RuntimeNativeTypeKind::TimePeriod, &time_dispatch}}};
}

void register_time(NativeRegistry &registry) {
  register_native_module_descriptor(registry, time_module_descriptor());
}

void register_time_runtime_module(RuntimeModuleRegistry &modules,
                                  RuntimeDispatchRegistry &dispatch) {
  const RuntimeNativeModuleDescriptor descriptor = time_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
}

} // namespace amber::runtime
