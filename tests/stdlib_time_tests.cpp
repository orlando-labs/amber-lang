#include "bytecode/emitter.h"
#include "frontend/binder/binder.h"
#include "frontend/hir/hir.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "runtime/vm.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "stdlib time test failed: " << message << "\n";
    std::exit(1);
  }
}

amber::bytecode::BcModule compile_source_or_die(const std::string &source) {
  amber::lexer::Lexer lexer(source, "<stdlib-time-source-test>");
  amber::lexer::LexResult lex_result = lexer.lex();
  if (!lex_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(lex_result.diagnostics);
    std::exit(1);
  }

  amber::parser::Parser parser(lex_result.tokens);
  amber::parser::ParseModuleResult parse_result = parser.parse_module_unit();
  if (!parse_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(parse_result.diagnostics);
    std::exit(1);
  }

  amber::binder::BindResult bind_result =
      amber::binder::bind_module(parse_result.items, parse_result.module_name);
  if (!bind_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(bind_result.diagnostics);
    std::exit(1);
  }

  amber::hir::Program program = amber::hir::lower_module(
      parse_result.items, parse_result.module_name, bind_result.graph);
  amber::bytecode::EmitResult emit_result =
      amber::bytecode::emit_program(program, parse_result.module_name);
  if (!emit_result.ok()) {
    std::cerr << amber::lexer::diagnostics_to_json(emit_result.diagnostics);
    std::exit(1);
  }

  amber::bytecode::DecodeResult decoded = amber::bytecode::deserialize_module(
      amber::bytecode::serialize_module(emit_result.module));
  if (!decoded.ok()) {
    std::cerr << amber::bytecode::verify_errors_to_json(decoded.errors);
    std::exit(1);
  }
  return std::move(decoded.module);
}

amber::runtime::ExecutionResult execute_source_or_die(
    const std::string &source) {
  amber::bytecode::BcModule module = compile_source_or_die(source);
  expect(module.init.has_entry_code_id, "source module should have init code");
  return amber::runtime::execute_code(module, module.init.entry_code_id);
}

void expect_ok_integer(const amber::runtime::ExecutionResult &result,
                       std::int64_t expected, const std::string &message) {
  if (!result.ok() && result.fault.has_value()) {
    std::cerr << "[fault] " << message << ": " << result.fault->error_name
              << " / " << result.fault->message << "\n";
  }
  expect(result.ok(), message + " should succeed");
  expect(result.value.is_integer(), message + " should return Int");
  expect(result.value.as_integer() == expected, message + " value");
}

void expect_fault(const std::string &source, const std::string &error_name,
                  const std::string &message) {
  amber::bytecode::BcModule module = compile_source_or_die(source);
  expect(module.init.has_entry_code_id, "fault module should have init");
  amber::runtime::ExecutionResult result =
      amber::runtime::execute_code(module, module.init.entry_code_id);
  expect(!result.ok() && result.fault.has_value(),
         message + " should fault");
  expect(result.fault->error_name == error_name,
         message + " should fault with " + error_name + ", got " +
             (result.fault.has_value() ? result.fault->error_name : ""));
}

void test_period_literals_and_time_addition() {
  const amber::runtime::ExecutionResult result = execute_source_or_die(
      "p = 5.seconds + 1.day\n"
      "t = p + Time.utc(2026, 6, 17)\n"
      "if TimePeriod === p and Time === t and p.days == 1 and "
      "p.nanoseconds == 5000000000 and "
      "t.iso8601 == \"2026-06-18T00:00:05Z\" and "
      "0.5.seconds.total_nanoseconds == 500000000:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "period literals and Time addition");
}

void test_parse_epoch_fields_and_json() {
  const amber::runtime::ExecutionResult result = execute_source_or_die(
      "epoch = Time.from_unix(0)\n"
      "parsed = Time.parse(\"2026-06-17T03:04:05.123456789+03:00\")\n"
      "negative = Time.from_unix_ns(-1)\n"
      "if epoch.iso8601 == \"1970-01-01T00:00:00Z\" and "
      "parsed.iso8601 == \"2026-06-17T00:04:05.123456789Z\" and "
      "parsed.year == 2026 and parsed.month == 6 and parsed.day == 17 and "
      "parsed.hour == 0 and parsed.minute == 4 and parsed.second == 5 and "
      "parsed.nanosecond == 123456789 and parsed.weekday == 3 and "
      "parsed.yearday == 168 and "
      "negative.iso8601 == \"1969-12-31T23:59:59.999999999Z\" and "
      "parsed.to_json == \"\\\"2026-06-17T00:04:05.123456789Z\\\"\":\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "parse, epoch fields, and JSON");
}

void test_calendar_arithmetic_and_difference() {
  const amber::runtime::ExecutionResult result = execute_source_or_die(
      "leap = Time.utc(2024, 1, 31) + 1.month\n"
      "plain = Time.utc(2023, 1, 31) + 1.month\n"
      "year = Time.utc(2024, 2, 29) + 1.year\n"
      "delta = Time.utc(2026, 6, 18) - Time.utc(2026, 6, 17)\n"
      "earlier = Time.utc(2026, 6, 18) - 1.day\n"
      "if leap.iso8601 == \"2024-02-29T00:00:00Z\" and "
      "plain.iso8601 == \"2023-02-28T00:00:00Z\" and "
      "year.iso8601 == \"2025-02-28T00:00:00Z\" and "
      "delta.total_nanoseconds == 86400000000000 and "
      "earlier.iso8601 == \"2026-06-17T00:00:00Z\" and "
      "1.hour < 2.hours and Time.utc(2026, 6, 18) > Time.utc(2026, 6, 17):\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "calendar arithmetic and difference");
}

void test_time_zone_conversion_and_type() {
  const amber::runtime::ExecutionResult result = execute_source_or_die(
      "t = Time.parse(\"2026-07-02T12:00:00Z\")\n"
      "moscow = TimeZone[\"Europe/Moscow\"]\n"
      "fixed = Time.time_zone(\"+03:00\")\n"
      "inside = t.in_tz(moscow)\n"
      "assumed = t.as_tz(moscow)\n"
      "if TimeZone === moscow and moscow.name == \"Europe/Moscow\" and "
      "moscow.fixed? == false and fixed.fixed? and "
      "fixed.offset_seconds == 10800 and "
      "moscow.abbreviation_at(t) == \"MSK\" and "
      "inside.iso8601 == \"2026-07-02T15:00:00+03:00\" and "
      "inside.unix_seconds == t.unix_seconds and "
      "inside.time_zone.name == \"Europe/Moscow\" and "
      "assumed.iso8601 == \"2026-07-02T12:00:00+03:00\" and "
      "assumed.in_tz(:utc).iso8601 == \"2026-07-02T09:00:00Z\":\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "time zone conversion and type");
}

void test_iana_dst_offsets_and_local_zone() {
  const amber::runtime::ExecutionResult result = execute_source_or_die(
      "ny = TimeZone[\"America/New_York\"]\n"
      "winter = Time.parse(\"2026-01-15T12:00:00Z\").in_tz(ny)\n"
      "summer = Time.parse(\"2026-07-15T12:00:00Z\").in_tz(ny)\n"
      "local = TimeZone.local\n"
      "local_again = TimeZone[local.name]\n"
      "if winter.iso8601 == \"2026-01-15T07:00:00-05:00\" and "
      "summer.iso8601 == \"2026-07-15T08:00:00-04:00\" and "
      "ny.offset_at(winter).total_nanoseconds == -18000000000000 and "
      "ny.offset_at(summer).total_nanoseconds == -14400000000000 and "
      "ny.abbreviation_at(winter) == \"EST\" and "
      "ny.abbreviation_at(summer) == \"EDT\" and "
      "TimeZone === local and local_again == local:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "IANA DST offsets and local zone");
}

void test_dst_gap_fold_and_calendar_arithmetic() {
  const amber::runtime::ExecutionResult result = execute_source_or_die(
      "gap_forward = Time.parse(\"2026-03-08 02:30\", "
      "format: :datetime, zone: \"America/New_York\", on_gap: :forward)\n"
      "gap_backward = Time.parse(\"2026-03-08 02:30\", "
      "format: :datetime, zone: \"America/New_York\", on_gap: :backward)\n"
      "fold_earlier = Time.parse(\"2026-11-01 01:30\", "
      "format: :datetime, zone: \"America/New_York\")\n"
      "fold_later = Time.parse(\"2026-11-01 01:30\", "
      "format: :datetime, zone: \"America/New_York\", on_fold: :later)\n"
      "before = Time.parse(\"2026-03-07 12:00\", "
      "format: :datetime, zone: \"America/New_York\")\n"
      "calendar_day = before + 1.day\n"
      "fixed_day = before + 24.hours\n"
      "if gap_forward.iso8601 == \"2026-03-08T03:30:00-04:00\" and "
      "gap_backward.iso8601 == \"2026-03-08T01:30:00-05:00\" and "
      "fold_earlier.iso8601 == \"2026-11-01T01:30:00-04:00\" and "
      "fold_later.iso8601 == \"2026-11-01T01:30:00-05:00\" and "
      "calendar_day.iso8601 == \"2026-03-08T12:00:00-04:00\" and "
      "fixed_day.iso8601 == \"2026-03-08T13:00:00-04:00\":\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "DST gap/fold policies and arithmetic");
}

void test_parse_formats_and_to_str() {
  const amber::runtime::ExecutionResult result = execute_source_or_die(
      "date = Time.parse(\"2026-06-17\")\n"
      "ru = Time.parse(\"02.07.2026 15:30\", "
      "format: :ru_datetime, zone: \"Europe/Moscow\")\n"
      "pattern = Time.parse(\"02/07/2026 15:30\", "
      "format: \"%d/%m/%Y %H:%M\", zone: \"+03:00\")\n"
      "preserved = Time.parse(\"2026-07-02T15:30:00+03:00\", "
      "zone: :parsed)\n"
      "if date.iso8601 == \"2026-06-17T00:00:00Z\" and "
      "ru.iso8601 == \"2026-07-02T15:30:00+03:00\" and "
      "ru.in_tz(:utc).iso8601 == \"2026-07-02T12:30:00Z\" and "
      "ru.to_str(:ru_date) == \"02.07.2026\" and "
      "ru.to_str(:ru_long) == \"2 июля 2026 15:30\" and "
      "ru.to_str(\"%Y/%m/%d %H:%M %:z\") == \"2026/07/02 15:30 +03:00\" and "
      "pattern.iso8601 == \"2026-07-02T15:30:00+03:00\" and "
      "preserved.hour == 15 and preserved.time_zone.name == \"+03:00\":\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "parse formats and to_str");
}

void test_local_boundary_helpers() {
  const amber::runtime::ExecutionResult result = execute_source_or_die(
      "t = Time.parse(\"2026-07-02T15:45:10Z\").in_tz(\"Europe/Moscow\")\n"
      "if t.start_of_day.iso8601 == \"2026-07-02T00:00:00+03:00\" and "
      "t.end_of_day.iso8601 == \"2026-07-02T23:59:59.999999999+03:00\" and "
      "t.start_of_week(week_start: :monday).iso8601 == "
      "\"2026-06-29T00:00:00+03:00\" and "
      "t.beginning_of_month.iso8601 == \"2026-07-01T00:00:00+03:00\" and "
      "t.start_of_quarter.iso8601 == \"2026-07-01T00:00:00+03:00\" and "
      "t.end_of_year.iso8601 == \"2026-12-31T23:59:59.999999999+03:00\":\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(result, 42, "local boundary helpers");
}

void test_live_clock_shapes_and_replay_fault() {
  const amber::runtime::ExecutionResult shape_result = execute_source_or_die(
      "if Time === Time.now and TimePeriod === Time.monotonic:\n"
      "  42\n"
      "else:\n"
      "  0\n");
  expect_ok_integer(shape_result, 42, "live clock value shapes");

  amber::bytecode::BcModule module = compile_source_or_die("Time.now\n");
  amber::runtime::RuntimeWorldOptions options;
  options.enforce_replay = true;
  amber::runtime::RuntimeWorld world(module, options);
  amber::runtime::ExecutionResult replay =
      world.execute(module.init.entry_code_id);
  expect(!replay.ok() && replay.fault.has_value() &&
             replay.fault->error_name == "DeterminismError",
         "Time.now should fault under replay without a clock provider");
}

void test_faults() {
  expect_fault("Time.parse(\"17/06/2026\")\n", "TimeParseError",
               "invalid parse shape");
  expect_fault("TimeZone[\"Mars/Base\"]\n", "TimeZoneLookupError",
               "unknown time zone");
  expect_fault("Time.parse(\"2026-03-08 02:30\", format: :datetime, "
               "zone: \"America/New_York\")\n",
               "TimeZoneGapError", "DST gap should fault by default");
  expect_fault("Time.parse(\"2026-11-01 01:30\", format: :datetime, "
               "zone: \"America/New_York\", on_fold: \"raise\")\n",
               "TimeZoneAmbiguousError", "DST fold can fault explicitly");
  expect_fault("1.5.months\n", "TypeError", "fractional calendar period");
  expect_fault("Time.utc(2026, 2, 29)\n", "ArgumentError",
               "invalid calendar date");
}

} // namespace

int main() {
  test_period_literals_and_time_addition();
  test_parse_epoch_fields_and_json();
  test_calendar_arithmetic_and_difference();
  test_time_zone_conversion_and_type();
  test_iana_dst_offsets_and_local_zone();
  test_dst_gap_fold_and_calendar_arithmetic();
  test_parse_formats_and_to_str();
  test_local_boundary_helpers();
  test_live_clock_shapes_and_replay_fault();
  test_faults();

  std::cout << "stdlib_time_tests: ok\n";
  return 0;
}
