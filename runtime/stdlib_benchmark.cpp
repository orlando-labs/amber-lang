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
#include <vector>

namespace amber::runtime {

namespace {

constexpr const char *kSchema = "amber.benchmark.v1";
constexpr const char *kBoldOn = "\x1b[1m";
constexpr const char *kBoldOff = "\x1b[22m";

struct RunOptions {
  std::int64_t iterations = 1;
  std::int64_t warmup = 0;
  std::int64_t samples = 1;
  std::int64_t min_time_ns = 0;
  bool gc = false;
};

struct FormatOptions {
  std::string layout = "summary";
  std::string unit = "auto";
  std::string style = "plain";
  std::string highlight = "none";
  std::string sort = "total";
  std::string metric = "mean";
};

struct ProfileSpan {
  std::string label;
  Value data = Value::null();
  std::int64_t elapsed_ns = 0;
  std::int64_t self_ns = 0;
  std::int64_t parent = -1;
  std::int64_t depth = 0;
  std::vector<std::size_t> children;
};

struct ProfileState {
  std::int64_t id = 0;
  std::vector<ProfileSpan> spans;
  std::vector<std::size_t> stack;
};

thread_local std::int64_t next_profile_id = 1;
thread_local std::vector<ProfileState *> active_profiles;

std::int64_t clamp_i128_to_i64(__int128 value) {
  if (value < static_cast<__int128>(std::numeric_limits<std::int64_t>::min())) {
    return std::numeric_limits<std::int64_t>::min();
  }
  if (value > static_cast<__int128>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return static_cast<std::int64_t>(value);
}

std::int64_t period_to_ns(const RuntimeTimePeriodValue &period) {
  const __int128 days_ns =
      static_cast<__int128>(period.days) * 86400 * 1000000000;
  return clamp_i128_to_i64(days_ns + period.nanoseconds);
}

std::optional<std::int64_t> monotonic_ns(NativeStdlibCall &call) {
  const std::optional<RuntimeTimePeriodValue> now = call.monotonic_time();
  if (!now.has_value()) {
    return std::nullopt;
  }
  return period_to_ns(*now);
}

Value period_value(std::int64_t ns) {
  auto period = std::make_shared<RuntimeTimePeriodValue>();
  period->nanoseconds = ns;
  return Value::time_period(std::move(period));
}

Value s(NativeStdlibCall &call, std::string text) {
  return call.string_value(std::move(text));
}

Value obj(NativeStdlibCall &call, std::vector<std::pair<std::string, Value>> xs,
          bool strict = false) {
  return call.make_object(std::move(xs), strict);
}

Value list(NativeStdlibCall &call, std::vector<Value> xs) {
  return call.make_list(std::move(xs));
}

bool value_to_text(NativeStdlibCall &call, const Value &value,
                   const std::string &label, std::string *out) {
  const std::optional<std::string> text = call.text_of(value);
  if (!text.has_value()) {
    call.fault("TypeError", label + " must be Str or Symbol");
    return false;
  }
  *out = *text;
  return true;
}

bool optional_label_arg(NativeStdlibCall &call, std::size_t max_args,
                        Value *out) {
  if (call.args.size() > max_args) {
    call.fault("TypeError", "Benchmark label accepts at most one argument");
    return false;
  }
  if (call.args.empty() || call.args[0].is_null()) {
    *out = Value::null();
    return true;
  }
  std::string text;
  if (!value_to_text(call, call.args[0], "label", &text)) {
    return false;
  }
  *out = s(call, std::move(text));
  return true;
}

bool bool_keyword(NativeStdlibCall &call, const std::string &name,
                  bool fallback, bool *out) {
  const std::optional<Value> value = call.keyword(name);
  if (!value.has_value()) {
    *out = fallback;
    return true;
  }
  if (!value->is_bool()) {
    call.fault("TypeError", name + " must be Bool");
    return false;
  }
  *out = value->as_bool();
  return true;
}

bool int_keyword(NativeStdlibCall &call, const std::string &name,
                 std::int64_t fallback, std::int64_t min, std::int64_t *out) {
  const std::optional<Value> value = call.keyword(name);
  if (!value.has_value()) {
    *out = fallback;
    return true;
  }
  if (!value->is_integer()) {
    call.fault("TypeError", name + " must be Int");
    return false;
  }
  const std::int64_t raw = value->as_integer();
  if (raw < min) {
    call.fault("ArgumentError", name + " is out of range");
    return false;
  }
  *out = raw;
  return true;
}

bool text_keyword(NativeStdlibCall &call, const std::string &name,
                  std::string fallback, std::string *out) {
  const std::optional<Value> value = call.keyword(name);
  if (!value.has_value()) {
    *out = std::move(fallback);
    return true;
  }
  return value_to_text(call, *value, name, out);
}

bool period_keyword_ns(NativeStdlibCall &call, const std::string &name,
                       std::int64_t fallback, std::int64_t *out) {
  const std::optional<Value> value = call.keyword(name);
  if (!value.has_value() || value->is_null()) {
    *out = fallback;
    return true;
  }
  if (!value->is_time_period() || value->as_time_period() == nullptr) {
    call.fault("TypeError", name + " must be TimePeriod or null");
    return false;
  }
  *out = std::max<std::int64_t>(0, period_to_ns(*value->as_time_period()));
  return true;
}

bool run_options(NativeStdlibCall &call, RunOptions *options) {
  if (!call.reject_unknown_keywords(
          {"iterations", "warmup", "samples", "min_time", "gc"})) {
    return false;
  }
  if (!int_keyword(call, "iterations", 1, 0, &options->iterations) ||
      !int_keyword(call, "warmup", 0, 0, &options->warmup) ||
      !int_keyword(call, "samples", 1, 1, &options->samples) ||
      !period_keyword_ns(call, "min_time", 0, &options->min_time_ns) ||
      !bool_keyword(call, "gc", false, &options->gc)) {
    return false;
  }
  if (options->iterations == 0 && options->min_time_ns == 0) {
    call.fault("ArgumentError",
               "iterations must be positive unless min_time is supplied");
    return false;
  }
  if (options->gc) {
    call.fault("BenchmarkUnsupportedError",
               "Benchmark gc: true is not supported by this runtime");
    return false;
  }
  return true;
}

std::string format_decimal(double value, int precision = 2) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

std::string format_ns(std::int64_t ns, const std::string &unit) {
  const double v = static_cast<double>(ns);
  std::string selected = unit;
  if (selected == "auto") {
    const double a = std::fabs(v);
    if (a < 1000.0) {
      selected = "ns";
    } else if (a < 1000000.0) {
      selected = "us";
    } else if (a < 1000000000.0) {
      selected = "ms";
    } else {
      selected = "s";
    }
  }
  if (selected == "ns") {
    return std::to_string(ns) + " ns";
  }
  if (selected == "us") {
    return format_decimal(v / 1000.0, 3) + " us";
  }
  if (selected == "ms") {
    return format_decimal(v / 1000000.0, 3) + " ms";
  }
  if (selected == "s") {
    return format_decimal(v / 1000000000.0, 3) + " s";
  }
  return std::to_string(ns) + " ns";
}

std::size_t percentile_index(std::size_t size, double percentile) {
  if (size == 0) {
    return 0;
  }
  const double rank = std::ceil(percentile * static_cast<double>(size));
  const std::size_t index = static_cast<std::size_t>(std::max(1.0, rank)) - 1U;
  return std::min(index, size - 1U);
}

Value measurement_map(NativeStdlibCall &call, Value label, std::int64_t elapsed,
                      Value value, const std::string &kind) {
  Value data =
      obj(call, {{"elapsed_ns", Value::integer(elapsed)},
                 {"elapsed_text", s(call, format_ns(elapsed, "auto"))},
                 {"iterations", Value::integer(1)}});
  return obj(call, {{"schema", s(call, kSchema)},
                    {"kind", s(call, kind)},
                    {"label", std::move(label)},
                    {"data", std::move(data)},
                    {"value", std::move(value)}});
}

Value profiler_map(NativeStdlibCall &call, std::int64_t id) {
  return obj(call, {{"schema", s(call, "amber.benchmark.profiler.v1")},
                    {"id", Value::integer(id)}});
}

bool profiler_id(NativeStdlibCall &call, const Value &profiler,
                 std::int64_t *id) {
  Value raw = Value::null();
  bool found = false;
  if (!call.lookup_string_key(profiler, "id", &raw, &found)) {
    return false;
  }
  if (!found || !raw.is_integer()) {
    call.fault("BenchmarkProfileError", "invalid Benchmark profiler");
    return false;
  }
  *id = raw.as_integer();
  return true;
}

ProfileState *active_profile_for(NativeStdlibCall &call,
                                 const Value &profiler) {
  if (active_profiles.empty()) {
    call.fault("BenchmarkProfileError",
               "Benchmark profiler is no longer active");
    return nullptr;
  }
  std::int64_t id = 0;
  if (!profiler_id(call, profiler, &id)) {
    return nullptr;
  }
  ProfileState *state = active_profiles.back();
  if (state == nullptr || state->id != id) {
    call.fault("BenchmarkProfileError",
               "Benchmark profiler belongs to a different profile block");
    return nullptr;
  }
  return state;
}

void finalize_profile_self_times(ProfileState *state) {
  if (state == nullptr) {
    return;
  }
  for (ProfileSpan &span : state->spans) {
    std::int64_t child_ns = 0;
    for (std::size_t child : span.children) {
      if (child < state->spans.size()) {
        child_ns = clamp_i128_to_i64(static_cast<__int128>(child_ns) +
                                     state->spans[child].elapsed_ns);
      }
    }
    span.self_ns = std::max<std::int64_t>(0, span.elapsed_ns - child_ns);
  }
}

Value profile_span_map(NativeStdlibCall &call, const ProfileState &state,
                       std::size_t index) {
  const ProfileSpan &span = state.spans[index];
  std::vector<Value> children;
  children.reserve(span.children.size());
  for (std::size_t child : span.children) {
    if (child < state.spans.size()) {
      children.push_back(profile_span_map(call, state, child));
    }
  }
  return obj(call, {{"label", s(call, span.label)},
                    {"data", span.data},
                    {"elapsed_ns", Value::integer(span.elapsed_ns)},
                    {"elapsed_text", s(call, format_ns(span.elapsed_ns, "auto"))},
                    {"self_ns", Value::integer(span.self_ns)},
                    {"self_text", s(call, format_ns(span.self_ns, "auto"))},
                    {"depth", Value::integer(span.depth)},
                    {"parent_index", Value::integer(span.parent)},
                    {"children", list(call, std::move(children))}});
}

Value profile_spans_value(NativeStdlibCall &call, const ProfileState &state) {
  std::vector<Value> spans;
  spans.reserve(state.spans.size());
  for (std::size_t i = 0; i < state.spans.size(); ++i) {
    spans.push_back(profile_span_map(call, state, i));
  }
  return list(call, std::move(spans));
}

Value profile_summary_value(NativeStdlibCall &call, const ProfileState &state) {
  struct Row {
    std::string label;
    std::int64_t count = 0;
    std::int64_t total_ns = 0;
    std::int64_t self_ns = 0;
    std::int64_t min_ns = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_ns = 0;
  };
  std::vector<Row> rows;
  for (const ProfileSpan &span : state.spans) {
    auto found = std::find_if(rows.begin(), rows.end(), [&](const Row &row) {
      return row.label == span.label;
    });
    if (found == rows.end()) {
      rows.push_back(Row{span.label});
      found = rows.end() - 1;
    }
    found->count += 1;
    found->total_ns = clamp_i128_to_i64(static_cast<__int128>(found->total_ns) +
                                        span.elapsed_ns);
    found->self_ns = clamp_i128_to_i64(static_cast<__int128>(found->self_ns) +
                                       span.self_ns);
    found->min_ns = std::min(found->min_ns, span.elapsed_ns);
    found->max_ns = std::max(found->max_ns, span.elapsed_ns);
  }

  std::vector<Value> values;
  values.reserve(rows.size());
  for (const Row &row : rows) {
    const std::int64_t mean =
        row.count <= 0 ? 0 : row.total_ns / std::max<std::int64_t>(1, row.count);
    const std::int64_t min_ns =
        row.min_ns == std::numeric_limits<std::int64_t>::max() ? 0 : row.min_ns;
    values.push_back(
        obj(call, {{"label", s(call, row.label)},
                   {"count", Value::integer(row.count)},
                   {"total_ns", Value::integer(row.total_ns)},
                   {"total_text", s(call, format_ns(row.total_ns, "auto"))},
                   {"self_ns", Value::integer(row.self_ns)},
                   {"self_text", s(call, format_ns(row.self_ns, "auto"))},
                   {"mean_ns", Value::integer(mean)},
                   {"mean_text", s(call, format_ns(mean, "auto"))},
                   {"min_ns", Value::integer(min_ns)},
                   {"min_text", s(call, format_ns(min_ns, "auto"))},
                   {"max_ns", Value::integer(row.max_ns)},
                   {"max_text", s(call, format_ns(row.max_ns, "auto"))}}));
  }
  return list(call, std::move(values));
}

Value report_map(NativeStdlibCall &call, Value label, std::int64_t iterations,
                 const std::vector<std::int64_t> &sample_ns) {
  std::int64_t elapsed = 0;
  std::vector<std::int64_t> per_iter;
  per_iter.reserve(sample_ns.size());
  for (std::int64_t ns : sample_ns) {
    elapsed = clamp_i128_to_i64(static_cast<__int128>(elapsed) + ns);
    const std::int64_t denom = std::max<std::int64_t>(1, iterations);
    per_iter.push_back(ns / denom);
  }
  std::vector<std::int64_t> sorted = per_iter;
  std::sort(sorted.begin(), sorted.end());
  const std::int64_t min_ns = sorted.empty() ? 0 : sorted.front();
  const std::int64_t max_ns = sorted.empty() ? 0 : sorted.back();
  const std::int64_t mean_ns =
      per_iter.empty()
          ? 0
          : clamp_i128_to_i64(
                static_cast<__int128>(elapsed) /
                static_cast<__int128>(std::max<std::int64_t>(
                    1, iterations * static_cast<std::int64_t>(sample_ns.size()))));
  auto percentile = [&](double p) -> std::int64_t {
    return sorted.empty() ? 0 : sorted[percentile_index(sorted.size(), p)];
  };
  const double seconds = static_cast<double>(elapsed) / 1000000000.0;
  const double ops =
      seconds <= 0.0
          ? 0.0
          : static_cast<double>(iterations) *
                static_cast<double>(sample_ns.size()) / seconds;

  std::vector<Value> sample_values;
  sample_values.reserve(sample_ns.size());
  for (std::int64_t ns : sample_ns) {
    sample_values.push_back(Value::integer(ns));
  }

  Value data =
      obj(call, {{"iterations", Value::integer(iterations *
                                                static_cast<std::int64_t>(
                                                    sample_ns.size()))},
                 {"iterations_per_sample", Value::integer(iterations)},
                 {"samples", Value::integer(static_cast<std::int64_t>(
                                  sample_ns.size()))},
                 {"elapsed_ns", Value::integer(elapsed)},
                 {"elapsed_text", s(call, format_ns(elapsed, "auto"))},
                 {"per_iteration_ns", Value::integer(mean_ns)},
                 {"mean_ns", Value::integer(mean_ns)},
                 {"min_ns", Value::integer(min_ns)},
                 {"max_ns", Value::integer(max_ns)},
                 {"p50_ns", Value::integer(percentile(0.50))},
                 {"p90_ns", Value::integer(percentile(0.90))},
                 {"p95_ns", Value::integer(percentile(0.95))},
                 {"p99_ns", Value::integer(percentile(0.99))},
                 {"ops_per_second", Value::floating(ops)},
                 {"sample_ns", list(call, std::move(sample_values))}});
  return obj(call, {{"schema", s(call, kSchema)},
                    {"kind", s(call, "report")},
                    {"label", std::move(label)},
                    {"data", std::move(data)}});
}

bool map_text_field(NativeStdlibCall &call, const Value &map,
                    const std::string &key, std::string *out) {
  Value raw = Value::null();
  bool found = false;
  if (!call.lookup_string_key(map, key, &raw, &found)) {
    return false;
  }
  if (!found || raw.is_null()) {
    *out = "";
    return true;
  }
  return value_to_text(call, raw, key, out);
}

bool map_value_field(NativeStdlibCall &call, const Value &map,
                     const std::string &key, Value *out, bool *found) {
  return call.lookup_string_key(map, key, out, found);
}

bool data_map(NativeStdlibCall &call, const Value &result, Value *data) {
  bool found = false;
  if (!map_value_field(call, result, "data", data, &found)) {
    return false;
  }
  if (!found || !data->is_map()) {
    call.fault("BenchmarkImportError", "benchmark result data must be a Map");
    return false;
  }
  return true;
}

bool int_field(NativeStdlibCall &call, const Value &map, const std::string &key,
               std::int64_t *out) {
  Value raw = Value::null();
  bool found = false;
  if (!map_value_field(call, map, key, &raw, &found)) {
    return false;
  }
  if (!found || !raw.is_integer()) {
    *out = 0;
    return found;
  }
  *out = raw.as_integer();
  return true;
}

bool double_field(NativeStdlibCall &call, const Value &map,
                  const std::string &key, double *out) {
  Value raw = Value::null();
  bool found = false;
  if (!map_value_field(call, map, key, &raw, &found)) {
    return false;
  }
  if (!found) {
    *out = 0.0;
    return true;
  }
  if (raw.is_float()) {
    *out = raw.as_float();
  } else if (raw.is_integer()) {
    *out = static_cast<double>(raw.as_integer());
  } else {
    *out = 0.0;
  }
  return true;
}

bool required_int_field(NativeStdlibCall &call, const Value &map,
                        const std::string &key, std::int64_t *out,
                        std::int64_t min = 0) {
  Value raw = Value::null();
  bool found = false;
  if (!map_value_field(call, map, key, &raw, &found)) {
    return false;
  }
  if (!found || !raw.is_integer()) {
    call.fault("BenchmarkImportError",
               "benchmark result " + key + " must be an Int");
    return false;
  }
  const std::int64_t value = raw.as_integer();
  if (value < min) {
    call.fault("BenchmarkImportError",
               "benchmark result " + key + " is out of range");
    return false;
  }
  *out = value;
  return true;
}

bool required_number_field(NativeStdlibCall &call, const Value &map,
                           const std::string &key, double *out,
                           double min = 0.0) {
  Value raw = Value::null();
  bool found = false;
  if (!map_value_field(call, map, key, &raw, &found)) {
    return false;
  }
  if (!found || (!raw.is_integer() && !raw.is_float())) {
    call.fault("BenchmarkImportError",
               "benchmark result " + key + " must be numeric");
    return false;
  }
  const double value =
      raw.is_integer() ? static_cast<double>(raw.as_integer()) : raw.as_float();
  if (!std::isfinite(value) || value < min) {
    call.fault("BenchmarkImportError",
               "benchmark result " + key + " is out of range");
    return false;
  }
  *out = value;
  return true;
}

bool optional_text_or_null_field(NativeStdlibCall &call, const Value &map,
                                 const std::string &key) {
  Value raw = Value::null();
  bool found = false;
  if (!map_value_field(call, map, key, &raw, &found)) {
    return false;
  }
  if (!found || raw.is_null()) {
    return true;
  }
  std::string ignored;
  return value_to_text(call, raw, key, &ignored);
}

bool required_text_field(NativeStdlibCall &call, const Value &map,
                         const std::string &key, std::string *out = nullptr) {
  Value raw = Value::null();
  bool found = false;
  if (!map_value_field(call, map, key, &raw, &found)) {
    return false;
  }
  if (!found || raw.is_null()) {
    call.fault("BenchmarkImportError",
               "benchmark result " + key + " must be a Str");
    return false;
  }
  std::string text;
  if (!value_to_text(call, raw, key, &text)) {
    return false;
  }
  if (out != nullptr) {
    *out = std::move(text);
  }
  return true;
}

bool required_list_field(NativeStdlibCall &call, const Value &map,
                         const std::string &key, std::vector<Value> *out) {
  Value raw = Value::null();
  bool found = false;
  if (!map_value_field(call, map, key, &raw, &found)) {
    return false;
  }
  if (!found || !raw.is_list() || !call.list_items(raw, out)) {
    call.fault("BenchmarkImportError",
               "benchmark result " + key + " must be a List");
    return false;
  }
  return true;
}

bool result_kind(NativeStdlibCall &call, const Value &result, std::string *kind) {
  std::string schema;
  if (!map_text_field(call, result, "schema", &schema) || schema != kSchema) {
    call.fault("BenchmarkImportError", "expected amber.benchmark.v1 map");
    return false;
  }
  if (!map_text_field(call, result, "kind", kind) || kind->empty()) {
    call.fault("BenchmarkImportError", "benchmark result kind is missing");
    return false;
  }
  return true;
}

Value sanitize_result_map(NativeStdlibCall &call, const Value &result) {
  std::vector<std::pair<std::string, Value>> entries;
  if (!call.string_keyed_entries(result, &entries)) {
    return Value::null();
  }
  std::vector<std::pair<std::string, Value>> clean;
  clean.reserve(entries.size());
  for (auto &[key, value] : entries) {
    if (key == "value") {
      continue;
    }
    clean.push_back({std::move(key), std::move(value)});
  }
  return obj(call, std::move(clean));
}

bool validate_span_shape(NativeStdlibCall &call, const Value &span) {
  if (!span.is_map()) {
    call.fault("BenchmarkImportError", "benchmark profile span must be a Map");
    return false;
  }
  std::int64_t ignored = 0;
  std::vector<Value> children;
  if (!required_text_field(call, span, "label") ||
      !required_int_field(call, span, "elapsed_ns", &ignored) ||
      !required_int_field(call, span, "self_ns", &ignored) ||
      !required_int_field(call, span, "depth", &ignored) ||
      !required_int_field(call, span, "parent_index", &ignored,
                          std::numeric_limits<std::int64_t>::min()) ||
      !required_list_field(call, span, "children", &children)) {
    return false;
  }
  for (const Value &child : children) {
    if (!validate_span_shape(call, child)) {
      return false;
    }
  }
  return true;
}

bool validate_summary_shape(NativeStdlibCall &call, const Value &row) {
  if (!row.is_map()) {
    call.fault("BenchmarkImportError",
               "benchmark profile summary row must be a Map");
    return false;
  }
  std::int64_t ignored = 0;
  return required_text_field(call, row, "label") &&
         required_int_field(call, row, "count", &ignored) &&
         required_int_field(call, row, "total_ns", &ignored) &&
         required_int_field(call, row, "self_ns", &ignored) &&
         required_int_field(call, row, "mean_ns", &ignored) &&
         required_int_field(call, row, "min_ns", &ignored) &&
         required_int_field(call, row, "max_ns", &ignored);
}

bool validate_result_shape(NativeStdlibCall &call, const Value &value,
                           const std::string &expected_kind = "");

bool validate_report_shape(NativeStdlibCall &call, const Value &data) {
  std::int64_t iterations = 0;
  std::int64_t per_sample = 0;
  std::int64_t samples = 0;
  std::int64_t ignored = 0;
  double ops = 0.0;
  if (!required_int_field(call, data, "iterations", &iterations) ||
      !required_int_field(call, data, "iterations_per_sample", &per_sample) ||
      !required_int_field(call, data, "samples", &samples, 1) ||
      !required_int_field(call, data, "elapsed_ns", &ignored) ||
      !required_int_field(call, data, "per_iteration_ns", &ignored) ||
      !required_int_field(call, data, "mean_ns", &ignored) ||
      !required_int_field(call, data, "min_ns", &ignored) ||
      !required_int_field(call, data, "max_ns", &ignored) ||
      !required_int_field(call, data, "p50_ns", &ignored) ||
      !required_int_field(call, data, "p90_ns", &ignored) ||
      !required_int_field(call, data, "p95_ns", &ignored) ||
      !required_int_field(call, data, "p99_ns", &ignored) ||
      !required_number_field(call, data, "ops_per_second", &ops)) {
    return false;
  }
  if (per_sample < 1 || iterations < per_sample ||
      iterations != per_sample * samples) {
    call.fault("BenchmarkImportError",
               "benchmark report iteration counts are inconsistent");
    return false;
  }
  std::vector<Value> sample_values;
  if (!required_list_field(call, data, "sample_ns", &sample_values)) {
    return false;
  }
  if (sample_values.size() != static_cast<std::size_t>(samples)) {
    call.fault("BenchmarkImportError",
               "benchmark report sample count is inconsistent");
    return false;
  }
  for (const Value &sample : sample_values) {
    if (!sample.is_integer() || sample.as_integer() < 0) {
      call.fault("BenchmarkImportError",
                 "benchmark report sample_ns entries must be non-negative Int");
      return false;
    }
  }
  return true;
}

bool validate_compare_shape(NativeStdlibCall &call, const Value &data) {
  std::vector<Value> cases;
  if (!required_list_field(call, data, "cases", &cases)) {
    return false;
  }
  if (cases.empty()) {
    call.fault("BenchmarkImportError",
               "benchmark compare report must contain cases");
    return false;
  }
  for (const Value &case_value : cases) {
    if (!validate_result_shape(call, case_value, "report")) {
      return false;
    }
  }
  std::int64_t fastest = 0;
  std::int64_t slowest = 0;
  if (!required_int_field(call, data, "fastest_index", &fastest) ||
      !required_int_field(call, data, "slowest_index", &slowest)) {
    return false;
  }
  if (fastest >= static_cast<std::int64_t>(cases.size()) ||
      slowest >= static_cast<std::int64_t>(cases.size())) {
    call.fault("BenchmarkImportError",
               "benchmark compare report indexes are out of range");
    return false;
  }
  std::vector<Value> relative;
  if (!required_list_field(call, data, "relative", &relative)) {
    return false;
  }
  if (relative.size() != cases.size()) {
    call.fault("BenchmarkImportError",
               "benchmark compare relative rows are inconsistent");
    return false;
  }
  return true;
}

bool validate_profile_shape(NativeStdlibCall &call, const Value &data) {
  std::int64_t ignored = 0;
  if (!required_int_field(call, data, "total_ns", &ignored)) {
    return false;
  }
  std::vector<Value> spans;
  if (!required_list_field(call, data, "spans", &spans)) {
    return false;
  }
  for (const Value &span : spans) {
    if (!validate_span_shape(call, span)) {
      return false;
    }
  }
  std::vector<Value> summary;
  if (!required_list_field(call, data, "summary", &summary)) {
    return false;
  }
  for (const Value &row : summary) {
    if (!validate_summary_shape(call, row)) {
      return false;
    }
  }
  return true;
}

bool validate_result_shape(NativeStdlibCall &call, const Value &value,
                           const std::string &expected_kind) {
  std::string kind;
  if (!result_kind(call, value, &kind)) {
    return false;
  }
  if (!expected_kind.empty() && kind != expected_kind) {
    call.fault("BenchmarkImportError",
               "expected benchmark " + expected_kind + " result");
    return false;
  }
  if (kind != "measurement" && kind != "report" && kind != "compare_report" &&
      kind != "profile") {
    call.fault("BenchmarkImportError", "unknown benchmark result kind");
    return false;
  }
  if (!optional_text_or_null_field(call, value, "label")) {
    return false;
  }
  Value data = Value::null();
  if (!data_map(call, value, &data)) {
    return false;
  }
  if (kind == "measurement") {
    std::int64_t ignored = 0;
    return required_int_field(call, data, "elapsed_ns", &ignored) &&
           required_int_field(call, data, "iterations", &ignored);
  }
  if (kind == "report") {
    return validate_report_shape(call, data);
  }
  if (kind == "compare_report") {
    return validate_compare_shape(call, data);
  }
  return validate_profile_shape(call, data);
}

bool validate_importable(NativeStdlibCall &call, const Value &value) {
  return validate_result_shape(call, value);
}

bool call_user_block(NativeStdlibCall &call, const Value &block,
                     std::vector<Value> args, Value *out) {
  const StdlibBlockResult result = call.call_block(block, std::move(args));
  if (result.status == StdlibBlockStatus::Returned) {
    *out = result.value;
    return true;
  }
  if (result.status == StdlibBlockStatus::Raised) {
    call.raise(result.exception);
  }
  return false;
}

SendStatus benchmark_time(NativeStdlibCall &call, bool full_measure) {
  if (call.block.is_null()) {
    return call.fault("TypeError", "Benchmark timing methods require block");
  }
  if (!call.reject_unknown_keywords({"gc"})) {
    return SendStatus::Faulted;
  }
  bool gc = false;
  if (!bool_keyword(call, "gc", false, &gc)) {
    return SendStatus::Faulted;
  }
  if (gc) {
    return call.fault("BenchmarkUnsupportedError",
                      "Benchmark gc: true is not supported by this runtime");
  }
  Value label = Value::null();
  if (!optional_label_arg(call, 1, &label)) {
    return SendStatus::Faulted;
  }
  const std::optional<std::int64_t> start = monotonic_ns(call);
  if (!start.has_value()) {
    return SendStatus::Faulted;
  }
  Value value = Value::null();
  if (!call_user_block(call, call.block, {}, &value)) {
    return SendStatus::Faulted;
  }
  const std::optional<std::int64_t> finish = monotonic_ns(call);
  if (!finish.has_value()) {
    return SendStatus::Faulted;
  }
  const std::int64_t elapsed = std::max<std::int64_t>(0, *finish - *start);
  *call.out = full_measure ? measurement_map(call, std::move(label), elapsed,
                                             std::move(value), "measurement")
                           : period_value(elapsed);
  return SendStatus::Matched;
}

SendStatus benchmark_section(NativeStdlibCall &call) {
  if (call.block.is_null()) {
    return call.fault("TypeError", "Benchmark profiler section requires block");
  }
  if (!call.reject_unknown_keywords({"data"})) {
    return SendStatus::Faulted;
  }
  if (call.args.size() != 2U) {
    return call.fault("TypeError",
                      "Benchmark profiler section expects label");
  }
  ProfileState *state = active_profile_for(call, call.args[0]);
  if (state == nullptr) {
    return SendStatus::Faulted;
  }
  std::string label;
  if (!value_to_text(call, call.args[1], "section label", &label)) {
    return SendStatus::Faulted;
  }
  Value data = Value::null();
  if (const std::optional<Value> supplied = call.keyword("data")) {
    data = *supplied;
  }

  const std::optional<std::int64_t> start = monotonic_ns(call);
  if (!start.has_value()) {
    return SendStatus::Faulted;
  }
  const std::int64_t parent =
      state->stack.empty()
          ? -1
          : static_cast<std::int64_t>(state->stack.back());
  const std::size_t index = state->spans.size();
  ProfileSpan span;
  span.label = std::move(label);
  span.data = std::move(data);
  span.parent = parent;
  span.depth = static_cast<std::int64_t>(state->stack.size());
  state->spans.push_back(std::move(span));
  if (parent >= 0 &&
      static_cast<std::size_t>(parent) < state->spans.size()) {
    state->spans[static_cast<std::size_t>(parent)].children.push_back(index);
  }
  state->stack.push_back(index);

  const StdlibBlockResult result = call.call_block(call.block, {});
  const std::optional<std::int64_t> finish = monotonic_ns(call);
  state->stack.pop_back();
  if (finish.has_value()) {
    state->spans[index].elapsed_ns =
        std::max<std::int64_t>(0, *finish - *start);
  }
  if (!finish.has_value()) {
    return SendStatus::Faulted;
  }
  if (result.status == StdlibBlockStatus::Returned) {
    *call.out = result.value;
    return SendStatus::Matched;
  }
  if (result.status == StdlibBlockStatus::Raised) {
    call.raise(result.exception);
  }
  return SendStatus::Faulted;
}

SendStatus benchmark_profile(NativeStdlibCall &call) {
  if (call.block.is_null()) {
    return call.fault("TypeError", "Benchmark.profile requires block");
  }
  if (!call.reject_unknown_keywords({"gc"})) {
    return SendStatus::Faulted;
  }
  bool gc = false;
  if (!bool_keyword(call, "gc", false, &gc)) {
    return SendStatus::Faulted;
  }
  if (gc) {
    return call.fault("BenchmarkUnsupportedError",
                      "Benchmark gc: true is not supported by this runtime");
  }
  Value label = Value::null();
  if (!optional_label_arg(call, 1, &label)) {
    return SendStatus::Faulted;
  }
  const std::optional<std::int64_t> start = monotonic_ns(call);
  if (!start.has_value()) {
    return SendStatus::Faulted;
  }
  ProfileState state;
  state.id = next_profile_id++;
  const Value profiler = profiler_map(call, state.id);
  active_profiles.push_back(&state);
  Value value = Value::null();
  const bool ok = call_user_block(call, call.block, {profiler}, &value);
  active_profiles.pop_back();
  if (!ok) {
    return SendStatus::Faulted;
  }
  const std::optional<std::int64_t> finish = monotonic_ns(call);
  if (!finish.has_value()) {
    return SendStatus::Faulted;
  }
  const std::int64_t total = std::max<std::int64_t>(0, *finish - *start);
  finalize_profile_self_times(&state);
  Value data = obj(call, {{"total_ns", Value::integer(total)},
                          {"total_text", s(call, format_ns(total, "auto"))},
                          {"spans", profile_spans_value(call, state)},
                          {"summary", profile_summary_value(call, state)}});
  *call.out = obj(call, {{"schema", s(call, kSchema)},
                         {"kind", s(call, "profile")},
                         {"label", std::move(label)},
                         {"data", std::move(data)},
                         {"value", std::move(value)}});
  return SendStatus::Matched;
}

SendStatus benchmark_run(NativeStdlibCall &call) {
  if (call.block.is_null()) {
    return call.fault("TypeError", "Benchmark.run requires block");
  }
  Value label = Value::null();
  if (!optional_label_arg(call, 1, &label)) {
    return SendStatus::Faulted;
  }
  RunOptions options;
  if (!run_options(call, &options)) {
    return SendStatus::Faulted;
  }
  for (std::int64_t i = 0; i < options.warmup; ++i) {
    Value ignored = Value::null();
    if (!call_user_block(call, call.block, {Value::integer(i)}, &ignored)) {
      return SendStatus::Faulted;
    }
  }

  std::vector<std::int64_t> sample_ns;
  sample_ns.reserve(static_cast<std::size_t>(options.samples));
  std::int64_t iterations = std::max<std::int64_t>(1, options.iterations);
  for (std::int64_t sample = 0; sample < options.samples; ++sample) {
    std::int64_t elapsed = 0;
    do {
      const std::optional<std::int64_t> start = monotonic_ns(call);
      if (!start.has_value()) {
        return SendStatus::Faulted;
      }
      for (std::int64_t i = 0; i < iterations; ++i) {
        Value ignored = Value::null();
        if (!call_user_block(call, call.block, {Value::integer(i)}, &ignored)) {
          return SendStatus::Faulted;
        }
      }
      const std::optional<std::int64_t> finish = monotonic_ns(call);
      if (!finish.has_value()) {
        return SendStatus::Faulted;
      }
      elapsed = std::max<std::int64_t>(0, *finish - *start);
      if (options.min_time_ns > 0 && elapsed < options.min_time_ns) {
        iterations = std::max<std::int64_t>(iterations + 1, iterations * 2);
      }
    } while (options.min_time_ns > 0 && elapsed < options.min_time_ns);
    sample_ns.push_back(elapsed);
  }

  *call.out = report_map(call, std::move(label), iterations, sample_ns);
  return SendStatus::Matched;
}

bool reports_from_args(NativeStdlibCall &call, std::vector<Value> *reports) {
  reports->clear();
  if (call.args.size() == 1 && call.args[0].is_list()) {
    return call.list_items(call.args[0], reports);
  }
  *reports = call.args;
  return true;
}

SendStatus benchmark_compare(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  std::vector<Value> reports;
  if (!reports_from_args(call, &reports)) {
    return SendStatus::Faulted;
  }
  if (reports.empty()) {
    return call.fault("ArgumentError", "Benchmark.compare expects reports");
  }
  std::int64_t fastest = std::numeric_limits<std::int64_t>::max();
  std::int64_t slowest = std::numeric_limits<std::int64_t>::min();
  std::size_t fastest_index = 0;
  std::size_t slowest_index = 0;
  std::vector<Value> clean_reports;
  clean_reports.reserve(reports.size());
  for (std::size_t i = 0; i < reports.size(); ++i) {
    std::string kind;
    if (!result_kind(call, reports[i], &kind)) {
      return SendStatus::Faulted;
    }
    if (kind != "report") {
      return call.fault("ArgumentError",
                        "Benchmark.compare expects Benchmark.run reports");
    }
    Value data = Value::null();
    if (!data_map(call, reports[i], &data)) {
      return SendStatus::Faulted;
    }
    std::int64_t mean = 0;
    if (!int_field(call, data, "mean_ns", &mean)) {
      return SendStatus::Faulted;
    }
    if (mean < fastest) {
      fastest = mean;
      fastest_index = i;
    }
    if (mean > slowest) {
      slowest = mean;
      slowest_index = i;
    }
    clean_reports.push_back(sanitize_result_map(call, reports[i]));
  }

  std::vector<Value> relative;
  relative.reserve(reports.size());
  for (std::size_t i = 0; i < reports.size(); ++i) {
    Value data = Value::null();
    (void)data_map(call, reports[i], &data);
    std::int64_t mean = 0;
    (void)int_field(call, data, "mean_ns", &mean);
    std::string label;
    (void)map_text_field(call, reports[i], "label", &label);
    if (label.empty()) {
      label = "case " + std::to_string(i + 1U);
    }
    relative.push_back(obj(
        call, {{"label", s(call, label)},
               {"mean_ns", Value::integer(mean)},
               {"ratio_to_fastest",
                Value::floating(fastest <= 0
                                    ? 1.0
                                    : static_cast<double>(mean) /
                                          static_cast<double>(fastest))}}));
  }

  Value data = obj(call, {{"cases", list(call, std::move(clean_reports))},
                          {"fastest_index", Value::integer(
                                                static_cast<std::int64_t>(
                                                    fastest_index))},
                          {"slowest_index", Value::integer(
                                               static_cast<std::int64_t>(
                                                   slowest_index))},
                          {"relative", list(call, std::move(relative))}});
  *call.out = obj(call, {{"schema", s(call, kSchema)},
                         {"kind", s(call, "compare_report")},
                         {"label", Value::null()},
                         {"data", std::move(data)}});
  return SendStatus::Matched;
}

bool format_options(NativeStdlibCall &call, FormatOptions *options) {
  if (!call.reject_unknown_keywords(
          {"layout", "unit", "style", "highlight", "sort", "metric"})) {
    return false;
  }
  return text_keyword(call, "layout", options->layout, &options->layout) &&
         text_keyword(call, "unit", options->unit, &options->unit) &&
         text_keyword(call, "style", options->style, &options->style) &&
         text_keyword(call, "highlight", options->highlight,
                      &options->highlight) &&
         text_keyword(call, "sort", options->sort, &options->sort) &&
         text_keyword(call, "metric", options->metric, &options->metric);
}

std::string join_row(const std::vector<std::string> &row,
                     const std::vector<std::size_t> &widths) {
  std::ostringstream out;
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (i > 0) {
      out << " | ";
    }
    out << std::left << std::setw(static_cast<int>(widths[i])) << row[i];
  }
  return out.str();
}

std::string render_table(std::vector<std::vector<std::string>> rows,
                         std::optional<std::size_t> bold_row) {
  std::vector<std::size_t> widths;
  for (const auto &row : rows) {
    if (widths.size() < row.size()) {
      widths.resize(row.size(), 0);
    }
    for (std::size_t i = 0; i < row.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].size());
    }
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    std::string line = join_row(rows[i], widths);
    if (bold_row.has_value() && *bold_row == i) {
      line = std::string(kBoldOn) + line + kBoldOff;
    }
    out << line;
    if (i + 1U < rows.size()) {
      out << '\n';
    }
  }
  return out.str();
}

std::string label_of(NativeStdlibCall &call, const Value &result,
                     std::size_t fallback_index) {
  std::string label;
  (void)map_text_field(call, result, "label", &label);
  return label.empty() ? "case " + std::to_string(fallback_index + 1U) : label;
}

std::string report_summary(NativeStdlibCall &call, const Value &result,
                           const FormatOptions &options) {
  Value data = Value::null();
  if (!data_map(call, result, &data)) {
    return "";
  }
  std::int64_t mean = 0;
  std::int64_t p95 = 0;
  std::int64_t iterations = 0;
  double ops = 0.0;
  (void)int_field(call, data, "mean_ns", &mean);
  (void)int_field(call, data, "p95_ns", &p95);
  (void)int_field(call, data, "iterations", &iterations);
  (void)double_field(call, data, "ops_per_second", &ops);
  return render_table({{"case", "iterations", "mean", "p95", "ops/s"},
                       {label_of(call, result, 0),
                        std::to_string(iterations), format_ns(mean, options.unit),
                        format_ns(p95, options.unit), format_decimal(ops, 2)}},
                      std::nullopt);
}

std::string compare_table(NativeStdlibCall &call, const Value &result,
                          const FormatOptions &options) {
  Value data = Value::null();
  if (!data_map(call, result, &data)) {
    return "";
  }
  Value cases_value = Value::null();
  bool found = false;
  if (!map_value_field(call, data, "cases", &cases_value, &found)) {
    return "";
  }
  std::vector<Value> cases;
  if (!found || !call.list_items(cases_value, &cases)) {
    return "";
  }
  std::int64_t fastest_index = -1;
  (void)int_field(call, data, "fastest_index", &fastest_index);
  std::vector<std::vector<std::string>> rows{
      {"case", "iterations", "mean", "p95", "ops/s", "relative"}};
  std::int64_t fastest_mean = 0;
  if (fastest_index >= 0 &&
      static_cast<std::size_t>(fastest_index) < cases.size()) {
    Value fastest_data = Value::null();
    (void)data_map(call, cases[static_cast<std::size_t>(fastest_index)],
                   &fastest_data);
    (void)int_field(call, fastest_data, "mean_ns", &fastest_mean);
  }
  for (std::size_t i = 0; i < cases.size(); ++i) {
    Value case_data = Value::null();
    if (!data_map(call, cases[i], &case_data)) {
      continue;
    }
    std::int64_t iterations = 0;
    std::int64_t mean = 0;
    std::int64_t p95 = 0;
    double ops = 0.0;
    (void)int_field(call, case_data, "iterations", &iterations);
    (void)int_field(call, case_data, "mean_ns", &mean);
    (void)int_field(call, case_data, "p95_ns", &p95);
    (void)double_field(call, case_data, "ops_per_second", &ops);
    const double ratio = fastest_mean <= 0
                             ? 1.0
                             : static_cast<double>(mean) /
                                   static_cast<double>(fastest_mean);
    rows.push_back({label_of(call, cases[i], i),
                    std::to_string(iterations), format_ns(mean, options.unit),
                    format_ns(p95, options.unit), format_decimal(ops, 2),
                    format_decimal(ratio, 2) + "x"});
  }
  std::optional<std::size_t> bold;
  if ((options.style == "ansi" || options.style == "xterm") &&
      options.highlight == "best" && fastest_index >= 0) {
    bold = static_cast<std::size_t>(fastest_index) + 1U;
  }
  return render_table(std::move(rows), bold);
}

std::string measurement_summary(NativeStdlibCall &call, const Value &result,
                                const FormatOptions &options) {
  Value data = Value::null();
  if (!data_map(call, result, &data)) {
    return "";
  }
  std::int64_t elapsed = 0;
  (void)int_field(call, data, "elapsed_ns", &elapsed);
  return render_table({{"label", "elapsed"},
                       {label_of(call, result, 0),
                        format_ns(elapsed, options.unit)}},
                      std::nullopt);
}

std::string profile_tree(NativeStdlibCall &call, const Value &result,
                         const FormatOptions &options) {
  Value data = Value::null();
  if (!data_map(call, result, &data)) {
    return "";
  }
  Value spans_value = Value::null();
  bool found = false;
  if (!map_value_field(call, data, "spans", &spans_value, &found) || !found) {
    return "";
  }
  std::vector<Value> spans;
  if (!call.list_items(spans_value, &spans)) {
    return "";
  }
  std::vector<std::vector<std::string>> rows{{"section", "total", "self"}};
  for (const Value &span : spans) {
    if (!span.is_map()) {
      continue;
    }
    std::string label;
    std::int64_t elapsed = 0;
    std::int64_t self = 0;
    std::int64_t depth = 0;
    (void)map_text_field(call, span, "label", &label);
    (void)int_field(call, span, "elapsed_ns", &elapsed);
    (void)int_field(call, span, "self_ns", &self);
    (void)int_field(call, span, "depth", &depth);
    rows.push_back(
        {std::string(static_cast<std::size_t>(std::max<std::int64_t>(0, depth)) *
                         2U,
                     ' ') +
             label,
         format_ns(elapsed, options.unit), format_ns(self, options.unit)});
  }
  return render_table(std::move(rows), std::nullopt);
}

std::string profile_summary(NativeStdlibCall &call, const Value &result,
                            const FormatOptions &options) {
  if (options.layout == "tree") {
    return profile_tree(call, result, options);
  }
  Value data = Value::null();
  if (!data_map(call, result, &data)) {
    return "";
  }
  Value summary_value = Value::null();
  bool found = false;
  if (map_value_field(call, data, "summary", &summary_value, &found) && found) {
    std::vector<Value> rows_value;
    if (call.list_items(summary_value, &rows_value) && !rows_value.empty()) {
      struct Row {
        std::string label;
        std::int64_t count = 0;
        std::int64_t total = 0;
        std::int64_t self = 0;
        std::int64_t mean = 0;
        std::int64_t max = 0;
        std::size_t source_index = 0;
      };
      std::vector<Row> parsed;
      parsed.reserve(rows_value.size());
      for (std::size_t i = 0; i < rows_value.size(); ++i) {
        const Value &row_value = rows_value[i];
        if (!row_value.is_map()) {
          continue;
        }
        Row row;
        row.source_index = i;
        (void)map_text_field(call, row_value, "label", &row.label);
        (void)int_field(call, row_value, "count", &row.count);
        (void)int_field(call, row_value, "total_ns", &row.total);
        (void)int_field(call, row_value, "self_ns", &row.self);
        (void)int_field(call, row_value, "mean_ns", &row.mean);
        (void)int_field(call, row_value, "max_ns", &row.max);
        parsed.push_back(std::move(row));
      }
      if (options.sort != "source_order") {
        const auto metric = [&](const Row &row) -> std::int64_t {
          if (options.sort == "self") {
            return row.self;
          }
          if (options.sort == "count") {
            return row.count;
          }
          return row.total;
        };
        std::stable_sort(parsed.begin(), parsed.end(),
                         [&](const Row &lhs, const Row &rhs) {
                           return metric(lhs) > metric(rhs);
                         });
      }
      std::optional<std::size_t> bold;
      if ((options.style == "ansi" || options.style == "xterm") &&
          options.highlight == "best" && options.sort != "source_order" &&
          !parsed.empty()) {
        bold = 1U;
      }
      std::vector<std::vector<std::string>> rows{
          {"section", "count", "total", "self", "mean", "max"}};
      for (const Row &row : parsed) {
        rows.push_back({row.label, std::to_string(row.count),
                        format_ns(row.total, options.unit),
                        format_ns(row.self, options.unit),
                        format_ns(row.mean, options.unit),
                        format_ns(row.max, options.unit)});
      }
      return render_table(std::move(rows), bold);
    }
  }
  std::int64_t total = 0;
  (void)int_field(call, data, "total_ns", &total);
  return render_table({{"profile", "total"},
                       {label_of(call, result, 0), format_ns(total, options.unit)}},
                      std::nullopt);
}

SendStatus benchmark_format(NativeStdlibCall &call, std::string layout) {
  if (call.args.size() != 1 || !call.require_no_block()) {
    return call.fault("TypeError", "Benchmark formatter expects one result");
  }
  FormatOptions options;
  options.layout = std::move(layout);
  if (!format_options(call, &options)) {
    return SendStatus::Faulted;
  }
  if (options.unit != "auto" && options.unit != "ns" && options.unit != "us" &&
      options.unit != "ms" && options.unit != "s") {
    return call.fault("ArgumentError", "unknown Benchmark unit");
  }
  if (options.layout != "summary" && options.layout != "table" &&
      options.layout != "tree") {
    return call.fault("ArgumentError", "unknown Benchmark layout");
  }
  if (options.style != "plain" && options.style != "ansi" &&
      options.style != "xterm") {
    return call.fault("ArgumentError", "unknown Benchmark style");
  }
  if (options.highlight != "none" && options.highlight != "best") {
    return call.fault("ArgumentError", "unknown Benchmark highlight");
  }
  if (options.sort != "total" && options.sort != "self" &&
      options.sort != "count" && options.sort != "source_order") {
    return call.fault("ArgumentError", "unknown Benchmark sort");
  }
  if (options.metric != "mean" && options.metric != "p95" &&
      options.metric != "ops_per_second" && options.metric != "total" &&
      options.metric != "self" && options.metric != "count" &&
      options.metric != "max" && options.metric != "min") {
    return call.fault("ArgumentError", "unknown Benchmark metric");
  }
  std::string kind;
  if (!result_kind(call, call.args[0], &kind)) {
    return SendStatus::Faulted;
  }
  if (options.layout == "tree" && kind != "profile") {
    return call.fault("ArgumentError",
                      "Benchmark tree layout only supports profiles");
  }
  std::string text;
  if (kind == "report") {
    text = report_summary(call, call.args[0], options);
  } else if (kind == "compare_report") {
    text = compare_table(call, call.args[0], options);
  } else if (kind == "measurement") {
    text = measurement_summary(call, call.args[0], options);
  } else if (kind == "profile") {
    text = profile_summary(call, call.args[0], options);
  } else {
    return call.fault("ArgumentError", "unknown Benchmark result kind");
  }
  *call.out = s(call, std::move(text));
  return SendStatus::Matched;
}

std::string json_escape(const std::string &text) {
  std::ostringstream out;
  out << '"';
  for (unsigned char ch : text) {
    switch (ch) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (ch < 0x20) {
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<int>(ch) << std::dec << std::setfill(' ');
      } else {
        out << static_cast<char>(ch);
      }
    }
  }
  out << '"';
  return out.str();
}

bool json_write_value(NativeStdlibCall &call, const Value &value,
                      std::ostringstream *out, bool pretty, int depth);

void json_indent(std::ostringstream *out, int depth) {
  for (int i = 0; i < depth; ++i) {
    *out << "  ";
  }
}

bool json_write_list(NativeStdlibCall &call, const Value &value,
                     std::ostringstream *out, bool pretty, int depth) {
  std::vector<Value> items;
  if (!call.list_items(value, &items)) {
    return false;
  }
  *out << '[';
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      *out << ',';
    }
    if (pretty) {
      *out << '\n';
      json_indent(out, depth + 1);
    }
    if (!json_write_value(call, items[i], out, pretty, depth + 1)) {
      return false;
    }
  }
  if (pretty && !items.empty()) {
    *out << '\n';
    json_indent(out, depth);
  }
  *out << ']';
  return true;
}

bool json_write_map(NativeStdlibCall &call, const Value &value,
                    std::ostringstream *out, bool pretty, int depth) {
  std::vector<std::pair<std::string, Value>> entries;
  if (!call.string_keyed_entries(value, &entries)) {
    return false;
  }
  *out << '{';
  bool first = true;
  for (const auto &[key, child] : entries) {
    if (!first) {
      *out << ',';
    }
    first = false;
    if (pretty) {
      *out << '\n';
      json_indent(out, depth + 1);
    }
    *out << json_escape(key) << (pretty ? ": " : ":");
    if (!json_write_value(call, child, out, pretty, depth + 1)) {
      return false;
    }
  }
  if (pretty && !entries.empty()) {
    *out << '\n';
    json_indent(out, depth);
  }
  *out << '}';
  return true;
}

bool json_write_value(NativeStdlibCall &call, const Value &value,
                      std::ostringstream *out, bool pretty, int depth) {
  if (value.is_null()) {
    *out << "null";
    return true;
  }
  if (value.is_bool()) {
    *out << (value.as_bool() ? "true" : "false");
    return true;
  }
  if (value.is_integer()) {
    *out << value.as_integer();
    return true;
  }
  if (value.is_float()) {
    const double raw = value.as_float();
    if (!std::isfinite(raw)) {
      call.fault("JsonGenerateError", "Benchmark JSON contains non-finite Float");
      return false;
    }
    *out << std::setprecision(17) << raw;
    return true;
  }
  if (value.is_string() || value.is_symbol()) {
    const std::optional<std::string> text = call.text_of(value);
    if (!text.has_value()) {
      return false;
    }
    *out << json_escape(*text);
    return true;
  }
  if (value.is_list()) {
    return json_write_list(call, value, out, pretty, depth);
  }
  if (value.is_map()) {
    return json_write_map(call, value, out, pretty, depth);
  }
  call.fault("JsonGenerateError", "Benchmark result is not JSON-safe");
  return false;
}

class JsonReader {
public:
  JsonReader(NativeStdlibCall &call, std::string text)
      : call_(call), text_(std::move(text)) {}

  bool parse(Value *out) {
    skip_ws();
    if (!parse_value(out)) {
      return false;
    }
    skip_ws();
    if (pos_ != text_.size()) {
      call_.fault("BenchmarkImportError", "trailing text in benchmark JSON");
      return false;
    }
    return true;
  }

private:
  NativeStdlibCall &call_;
  std::string text_;
  std::size_t pos_ = 0;

  void skip_ws() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\n' ||
            text_[pos_] == '\r' || text_[pos_] == '\t')) {
      ++pos_;
    }
  }

  bool consume(char ch) {
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == ch) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool parse_value(Value *out) {
    skip_ws();
    if (pos_ >= text_.size()) {
      return fail("unexpected end of benchmark JSON");
    }
    const char ch = text_[pos_];
    if (ch == '"') {
      std::string text;
      if (!parse_string(&text)) {
        return false;
      }
      *out = s(call_, std::move(text));
      return true;
    }
    if (ch == '{') {
      return parse_object(out);
    }
    if (ch == '[') {
      return parse_array(out);
    }
    if (ch == 't' && match("true")) {
      *out = Value::boolean(true);
      return true;
    }
    if (ch == 'f' && match("false")) {
      *out = Value::boolean(false);
      return true;
    }
    if (ch == 'n' && match("null")) {
      *out = Value::null();
      return true;
    }
    return parse_number(out);
  }

  bool match(const char *word) {
    const std::string s_word(word);
    if (text_.compare(pos_, s_word.size(), s_word) != 0) {
      return false;
    }
    pos_ += s_word.size();
    return true;
  }

  bool parse_string(std::string *out) {
    if (pos_ >= text_.size() || text_[pos_] != '"') {
      return fail("expected JSON string");
    }
    ++pos_;
    out->clear();
    while (pos_ < text_.size()) {
      const unsigned char ch = static_cast<unsigned char>(text_[pos_++]);
      if (ch == '"') {
        return true;
      }
      if (ch != '\\') {
        out->push_back(static_cast<char>(ch));
        continue;
      }
      if (pos_ >= text_.size()) {
        return fail("bad JSON escape");
      }
      const char esc = text_[pos_++];
      switch (esc) {
      case '"':
      case '\\':
      case '/':
        out->push_back(esc);
        break;
      case 'b':
        out->push_back('\b');
        break;
      case 'f':
        out->push_back('\f');
        break;
      case 'n':
        out->push_back('\n');
        break;
      case 'r':
        out->push_back('\r');
        break;
      case 't':
        out->push_back('\t');
        break;
      case 'u':
        if (!parse_unicode_escape(out)) {
          return false;
        }
        break;
      default:
        return fail("bad JSON escape");
      }
    }
    return fail("unterminated JSON string");
  }

  int hex_digit(char ch) const {
    if (ch >= '0' && ch <= '9') {
      return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
      return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
      return 10 + ch - 'A';
    }
    return -1;
  }

  bool parse_unicode_escape(std::string *out) {
    if (pos_ + 4U > text_.size()) {
      return fail("short JSON unicode escape");
    }
    int code = 0;
    for (int i = 0; i < 4; ++i) {
      const int digit = hex_digit(text_[pos_++]);
      if (digit < 0) {
        return fail("bad JSON unicode escape");
      }
      code = code * 16 + digit;
    }
    if (code <= 0x7F) {
      out->push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | (code >> 6)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xE0 | (code >> 12)));
      out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    return true;
  }

  bool parse_array(Value *out) {
    (void)consume('[');
    std::vector<Value> items;
    skip_ws();
    if (consume(']')) {
      *out = list(call_, std::move(items));
      return true;
    }
    while (true) {
      Value item = Value::null();
      if (!parse_value(&item)) {
        return false;
      }
      items.push_back(std::move(item));
      if (consume(']')) {
        *out = list(call_, std::move(items));
        return true;
      }
      if (!consume(',')) {
        return fail("expected comma in JSON array");
      }
    }
  }

  bool parse_object(Value *out) {
    (void)consume('{');
    std::vector<std::pair<std::string, Value>> entries;
    skip_ws();
    if (consume('}')) {
      *out = obj(call_, std::move(entries), true);
      return true;
    }
    while (true) {
      skip_ws();
      std::string key;
      if (!parse_string(&key)) {
        return false;
      }
      if (!consume(':')) {
        return fail("expected colon in JSON object");
      }
      Value value = Value::null();
      if (!parse_value(&value)) {
        return false;
      }
      entries.push_back({std::move(key), std::move(value)});
      if (consume('}')) {
        *out = obj(call_, std::move(entries), true);
        return true;
      }
      if (!consume(',')) {
        return fail("expected comma in JSON object");
      }
    }
  }

  bool parse_number(Value *out) {
    const std::size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') {
      ++pos_;
    }
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      ++pos_;
    }
    bool floating = false;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      floating = true;
      ++pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      floating = true;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    }
    if (start == pos_) {
      return fail("expected JSON value");
    }
    const std::string raw = text_.substr(start, pos_ - start);
    try {
      if (floating) {
        *out = Value::floating(std::stod(raw));
      } else {
        *out = Value::integer(std::stoll(raw));
      }
    } catch (...) {
      return fail("bad JSON number");
    }
    return true;
  }

  bool fail(const std::string &message) {
    call_.fault("BenchmarkImportError", message);
    return false;
  }
};

SendStatus benchmark_to_map(NativeStdlibCall &call) {
  if (!call.require_no_block() ||
      !call.reject_unknown_keywords({"value"}) || call.args.size() != 1U) {
    return call.fault("TypeError", "Benchmark.to_map expects one result");
  }
  if (!validate_importable(call, call.args[0])) {
    return SendStatus::Faulted;
  }
  std::string value_mode;
  if (!text_keyword(call, "value", "safe", &value_mode)) {
    return SendStatus::Faulted;
  }
  if (value_mode != "safe" && value_mode != "raw") {
    return call.fault("ArgumentError", "unknown Benchmark value mode");
  }
  *call.out = value_mode == "raw" ? call.args[0]
                                  : sanitize_result_map(call, call.args[0]);
  return SendStatus::Matched;
}

SendStatus benchmark_to_json(NativeStdlibCall &call) {
  if (!call.require_no_block() ||
      !call.reject_unknown_keywords({"pretty"}) || call.args.size() != 1U) {
    return call.fault("TypeError", "Benchmark.to_json expects one result");
  }
  if (!validate_importable(call, call.args[0])) {
    return SendStatus::Faulted;
  }
  bool pretty = false;
  if (!bool_keyword(call, "pretty", false, &pretty)) {
    return SendStatus::Faulted;
  }
  const Value clean = sanitize_result_map(call, call.args[0]);
  std::ostringstream out;
  if (!json_write_value(call, clean, &out, pretty, 0)) {
    return SendStatus::Faulted;
  }
  *call.out = s(call, out.str());
  return SendStatus::Matched;
}

SendStatus benchmark_from_map(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.reject_unknown_keywords({}) ||
      call.args.size() != 1U) {
    return call.fault("TypeError", "Benchmark.from_map expects one Map");
  }
  if (!validate_importable(call, call.args[0])) {
    return SendStatus::Faulted;
  }
  *call.out = sanitize_result_map(call, call.args[0]);
  return SendStatus::Matched;
}

SendStatus benchmark_from_json(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.reject_unknown_keywords({}) ||
      call.args.size() != 1U) {
    return call.fault("TypeError", "Benchmark.from_json expects one Str");
  }
  std::string text;
  if (!value_to_text(call, call.args[0], "json", &text)) {
    return SendStatus::Faulted;
  }
  JsonReader reader(call, std::move(text));
  Value parsed = Value::null();
  if (!reader.parse(&parsed) || !validate_importable(call, parsed)) {
    return SendStatus::Faulted;
  }
  *call.out = sanitize_result_map(call, parsed);
  return SendStatus::Matched;
}

Value top_value_or_null(NativeStdlibCall &call, const Value &result,
                        const std::string &key) {
  Value out = Value::null();
  bool found = false;
  if (!map_value_field(call, result, key, &out, &found)) {
    return Value::null();
  }
  return found ? out : Value::null();
}

Value data_value_or_null(NativeStdlibCall &call, const Value &result,
                         const std::string &key) {
  Value data = Value::null();
  if (!data_map(call, result, &data)) {
    return Value::null();
  }
  return top_value_or_null(call, data, key);
}

Value period_from_data_ns(NativeStdlibCall &call, const Value &result,
                          const std::string &key) {
  Value data = Value::null();
  if (!data_map(call, result, &data)) {
    return Value::null();
  }
  std::int64_t ns = 0;
  if (!int_field(call, data, key, &ns)) {
    return Value::null();
  }
  return period_value(ns);
}

Value time_period_list_from_ns_list(NativeStdlibCall &call,
                                    const Value &raw_list) {
  std::vector<Value> values;
  if (!call.list_items(raw_list, &values)) {
    return Value::null();
  }
  std::vector<Value> periods;
  periods.reserve(values.size());
  for (const Value &value : values) {
    if (!value.is_integer()) {
      return Value::null();
    }
    periods.push_back(period_value(value.as_integer()));
  }
  return list(call, std::move(periods));
}

Value benchmark_indexed_case(NativeStdlibCall &call, const Value &result,
                             const std::string &index_key) {
  Value data = Value::null();
  if (!data_map(call, result, &data)) {
    return Value::null();
  }
  std::int64_t index = -1;
  if (!int_field(call, data, index_key, &index) || index < 0) {
    return Value::null();
  }
  Value cases_value = Value::null();
  bool found = false;
  if (!map_value_field(call, data, "cases", &cases_value, &found) || !found) {
    return Value::null();
  }
  std::vector<Value> cases;
  if (!call.list_items(cases_value, &cases) ||
      static_cast<std::size_t>(index) >= cases.size()) {
    return Value::null();
  }
  return cases[static_cast<std::size_t>(index)];
}

Value benchmark_profile_find(NativeStdlibCall &call, const Value &profile,
                             const std::string &label) {
  Value data = Value::null();
  if (!data_map(call, profile, &data)) {
    return Value::null();
  }
  Value spans_value = Value::null();
  bool found = false;
  if (!map_value_field(call, data, "spans", &spans_value, &found) || !found) {
    return Value::null();
  }
  std::vector<Value> spans;
  if (!call.list_items(spans_value, &spans)) {
    return Value::null();
  }
  for (const Value &span : spans) {
    std::string span_label;
    if (span.is_map() && map_text_field(call, span, "label", &span_label) &&
        span_label == label) {
      return span;
    }
  }
  return Value::null();
}

std::string compact_result_text(NativeStdlibCall &call, const Value &result,
                                const std::string &kind) {
  std::string label = label_of(call, result, 0);
  Value data = Value::null();
  (void)data_map(call, result, &data);
  if (kind == "measurement") {
    std::int64_t elapsed = 0;
    (void)int_field(call, data, "elapsed_ns", &elapsed);
    return "Benchmark.measurement(" + label + ", elapsed=" +
           format_ns(elapsed, "auto") + ")";
  }
  if (kind == "report") {
    std::int64_t iterations = 0;
    std::int64_t mean = 0;
    (void)int_field(call, data, "iterations", &iterations);
    (void)int_field(call, data, "mean_ns", &mean);
    return "Benchmark.report(" + label + ", iterations=" +
           std::to_string(iterations) + ", mean=" + format_ns(mean, "auto") +
           ")";
  }
  if (kind == "compare_report") {
    Value cases = data_value_or_null(call, result, "cases");
    std::vector<Value> items;
    const std::size_t count =
        call.list_items(cases, &items) ? items.size() : 0U;
    return "Benchmark.compare_report(cases=" + std::to_string(count) + ")";
  }
  std::int64_t total = 0;
  (void)int_field(call, data, "total_ns", &total);
  return "Benchmark.profile(" + label + ", total=" +
         format_ns(total, "auto") + ")";
}

SendStatus benchmark_accessor(NativeStdlibCall &call) {
  if (!call.require_no_block() || !call.reject_unknown_keywords({})) {
    return SendStatus::Faulted;
  }
  const bool is_find = call.selector == "find";
  if ((!is_find && call.args.size() != 1U) ||
      (is_find && call.args.size() != 2U)) {
    return call.fault("TypeError", "Benchmark result accessor arity mismatch");
  }
  if (!validate_importable(call, call.args[0])) {
    return SendStatus::Faulted;
  }
  std::string kind;
  if (!result_kind(call, call.args[0], &kind)) {
    return SendStatus::Faulted;
  }

  const Value &result = call.args[0];
  if (call.selector == "label") {
    *call.out = top_value_or_null(call, result, "label");
    return SendStatus::Matched;
  }
  if (call.selector == "kind") {
    *call.out = s(call, kind);
    return SendStatus::Matched;
  }
  if (call.selector == "data") {
    Value data = Value::null();
    if (!data_map(call, result, &data)) {
      return SendStatus::Faulted;
    }
    *call.out = data;
    return SendStatus::Matched;
  }
  if (call.selector == "value") {
    *call.out = top_value_or_null(call, result, "value");
    return SendStatus::Matched;
  }
  if (call.selector == "to_str" || call.selector == "inspect") {
    *call.out = s(call, compact_result_text(call, result, kind));
    return SendStatus::Matched;
  }

  if (kind == "measurement") {
    if (call.selector == "elapsed") {
      *call.out = period_from_data_ns(call, result, "elapsed_ns");
      return SendStatus::Matched;
    }
    if (call.selector == "elapsed_ns" || call.selector == "iterations") {
      *call.out = data_value_or_null(
          call, result,
          call.selector == "elapsed_ns" ? "elapsed_ns" : "iterations");
      return SendStatus::Matched;
    }
  }

  if (kind == "report") {
    if (call.selector == "elapsed" || call.selector == "per_iteration" ||
        call.selector == "mean" || call.selector == "min" ||
        call.selector == "max" || call.selector == "p50" ||
        call.selector == "p90" || call.selector == "p95" ||
        call.selector == "p99") {
      const std::string key =
          call.selector == "elapsed" ? "elapsed_ns"
          : call.selector == "per_iteration" ? "per_iteration_ns"
                                               : call.selector + "_ns";
      *call.out = period_from_data_ns(call, result, key);
      return SendStatus::Matched;
    }
    if (call.selector == "sample_times") {
      *call.out = time_period_list_from_ns_list(
          call, data_value_or_null(call, result, "sample_ns"));
      return SendStatus::Matched;
    }
    if (call.selector == "iterations" || call.selector == "samples" ||
        call.selector == "elapsed_ns" ||
        call.selector == "per_iteration_ns" || call.selector == "mean_ns" ||
        call.selector == "min_ns" || call.selector == "max_ns" ||
        call.selector == "p50_ns" || call.selector == "p90_ns" ||
        call.selector == "p95_ns" || call.selector == "p99_ns" ||
        call.selector == "ops_per_second" || call.selector == "sample_ns") {
      *call.out = data_value_or_null(call, result, call.selector);
      return SendStatus::Matched;
    }
  }

  if (kind == "compare_report") {
    if (call.selector == "cases" || call.selector == "relative") {
      *call.out = data_value_or_null(call, result, call.selector);
      return SendStatus::Matched;
    }
    if (call.selector == "fastest" || call.selector == "slowest") {
      *call.out = benchmark_indexed_case(
          call, result,
          call.selector == "fastest" ? "fastest_index" : "slowest_index");
      return SendStatus::Matched;
    }
  }

  if (kind == "profile") {
    if (call.selector == "total") {
      *call.out = period_from_data_ns(call, result, "total_ns");
      return SendStatus::Matched;
    }
    if (call.selector == "total_ns" || call.selector == "spans" ||
        call.selector == "summary") {
      *call.out = data_value_or_null(call, result, call.selector);
      return SendStatus::Matched;
    }
    if (call.selector == "find") {
      std::string label;
      if (!value_to_text(call, call.args[1], "profile section label",
                         &label)) {
        return SendStatus::Faulted;
      }
      *call.out = benchmark_profile_find(call, result, label);
      return SendStatus::Matched;
    }
  }

  return call.fault("ArgumentError", "unknown Benchmark result accessor");
}

SendStatus benchmark_dispatch(NativeStdlibCall &call) {
  if (call.kind != RuntimeNativeTypeKind::Benchmark) {
    return SendStatus::NotHandled;
  }
  if (call.selector == "time") {
    return benchmark_time(call, false);
  }
  if (call.selector == "measure") {
    return benchmark_time(call, true);
  }
  if (call.selector == "profile") {
    return benchmark_profile(call);
  }
  if (call.selector == "section") {
    return benchmark_section(call);
  }
  if (call.selector == "run") {
    return benchmark_run(call);
  }
  if (call.selector == "compare") {
    return benchmark_compare(call);
  }
  if (call.selector == "format") {
    return benchmark_format(call, "summary");
  }
  if (call.selector == "table") {
    return benchmark_format(call, "table");
  }
  if (call.selector == "pretty") {
    return benchmark_format(call, "summary");
  }
  if (call.selector == "to_map" || call.selector == "map") {
    return benchmark_to_map(call);
  }
  if (call.selector == "to_json") {
    return benchmark_to_json(call);
  }
  if (call.selector == "from_map") {
    return benchmark_from_map(call);
  }
  if (call.selector == "from_json") {
    return benchmark_from_json(call);
  }
  if (call.selector == "label" || call.selector == "kind" ||
      call.selector == "data" || call.selector == "value" ||
      call.selector == "elapsed" || call.selector == "elapsed_ns" ||
      call.selector == "iterations" || call.selector == "samples" ||
      call.selector == "per_iteration" ||
      call.selector == "per_iteration_ns" || call.selector == "mean" ||
      call.selector == "mean_ns" || call.selector == "min" ||
      call.selector == "min_ns" || call.selector == "max" ||
      call.selector == "max_ns" || call.selector == "p50" ||
      call.selector == "p50_ns" || call.selector == "p90" ||
      call.selector == "p90_ns" || call.selector == "p95" ||
      call.selector == "p95_ns" || call.selector == "p99" ||
      call.selector == "p99_ns" || call.selector == "ops_per_second" ||
      call.selector == "sample_times" || call.selector == "sample_ns" ||
      call.selector == "cases" || call.selector == "fastest" ||
      call.selector == "slowest" || call.selector == "relative" ||
      call.selector == "total" || call.selector == "total_ns" ||
      call.selector == "spans" || call.selector == "summary" ||
      call.selector == "find" || call.selector == "to_str" ||
      call.selector == "inspect") {
    return benchmark_accessor(call);
  }
  return SendStatus::NotHandled;
}

RuntimeNativeModuleDescriptor benchmark_module_descriptor() {
  return {{{"Benchmark", RuntimeNativeTypeKind::Benchmark}},
          {{RuntimeNativeTypeKind::Benchmark, &benchmark_dispatch}},
          {},
          {},
          {{"BenchmarkError", "Exception"},
           {"BenchmarkUnsupportedError", "BenchmarkError"},
           {"BenchmarkProfileError", "BenchmarkError"},
           {"BenchmarkImportError", "BenchmarkError"}}};
}

} // namespace

void register_benchmark(NativeRegistry &registry) {
  register_native_module_descriptor(registry, benchmark_module_descriptor());
}

void register_benchmark_runtime_module(RuntimeModuleRegistry &modules,
                                       RuntimeDispatchRegistry &dispatch,
                                       RuntimeTypeRegistry &types,
                                       RuntimeErrorRegistry *errors) {
  const RuntimeNativeModuleDescriptor descriptor = benchmark_module_descriptor();
  register_runtime_module_descriptor(modules, descriptor);
  register_runtime_dispatch_descriptor(dispatch, descriptor);
  register_runtime_type_descriptor(types, descriptor);
  if (errors != nullptr) {
    register_runtime_error_descriptor(*errors, descriptor);
  }
}

} // namespace amber::runtime
