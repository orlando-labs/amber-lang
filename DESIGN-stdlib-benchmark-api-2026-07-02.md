# DESIGN: Benchmark stdlib module

Status: implemented v1 baseline plus proposed extensions
Date: 2026-07-02
Target: Amber standard library and runtime-facing API
Scope: user-instrumented timing, repeated micro-benchmark runs, named section
profiling, result comparison, pretty terminal output, and importable report data
Out of scope for v1: sampling CPU profiler, flamegraph generation, OS perf
counters, automatic function-level instrumentation, distributed tracing, and
memory-allocation accounting unless the runtime later exposes stable counters

## 1. Goals

`Benchmark` gives Amber programs a small, predictable way to measure code:

- one-off timing for a block;
- repeated runs with warmup and summary statistics;
- named section profiling for larger operations;
- pretty table output for terminals and logs, including optional
  xterm-compatible bold highlighting for best results;
- stable structured results that round-trip through `Map` and JSON;
- no implicit stdout/stderr writes.

The surface follows the stdlib style already used by `Json`, `Time`, `Digest`,
`Url`, and `net.http`: a top-level native module, lower_snake_case selectors,
block-suffix APIs for scoped work, `?` only for predicates, `!` only for
mutation or explicit lifecycle operations, `TimePeriod` for durations, and
Map/List-shaped data for portable reports.

## 2. Overview

```amber
measurement = Benchmark.measure("parse"):
  Json.parse(payload)

measurement["data"]["elapsed_ns"] # Int
measurement["value"][:id]         # block result
```

```amber
report = Benchmark.run("parse", iterations: 10_000, warmup: 500) |i|:
  Json.parse(payloads[i % payloads.count()])

report.map["data"]["mean_ns"]
report.map["data"]["p95_ns"]
report.map["data"]["ops_per_second"]
```

```amber
profile = Benchmark.profile("checkout") |p|:
  user = p.section("load_user"):
    load_user(user_id)

  total = p.section("price"):
    price(user.cart)

  p.section("persist"):
    save_order(user, total)

profile.map["data"]["total_ns"]
profile.map["data"]["summary"][0]["label"]
```

```amber
compact = Benchmark.run("compact", iterations: 5_000, samples: 5) |i|:
  Json.generate(value)

pretty = Benchmark.run("pretty", iterations: 5_000, samples: 5) |i|:
  Json.pretty_generate(value)

suite = Benchmark.compare(compact, pretty)

p suite.table(style: :ansi, highlight: :best)

data = suite.map
same_data = suite.to_map
json = suite.to_json(pretty: true)
same = Benchmark.from_json(json)
```

## 3. Core API

### 3.1 `Benchmark.measure`

```amber
Benchmark.measure(label = null, gc: false): ... -> Benchmark.Measurement
```

Runs the block exactly once and returns a `Benchmark.Measurement`.

Fields and methods:

| Name | Value |
| --- | --- |
| `label` | `Str` or `null` |
| `elapsed` | `TimePeriod` |
| `elapsed_ns` | elapsed nanoseconds as `Int` |
| `value` | the block return value |
| `iterations` | always `1` |
| `map` / `to_map` | canonical JSON-safe `Map` without `value` |
| `map(value: :raw)` / `to_map(value: :raw)` | includes the raw `value`; not guaranteed JSON-safe |
| `to_json(pretty: false)` | canonical JSON string |
| `pretty(...)` / `table(...)` / `format(...)` | human table string |
| `to_str` / `inspect` | compact human summary |

`gc: true` requests a full runtime collection before the block when the active
runtime profile supports explicit collection. Unsupported profiles raise
`BenchmarkUnsupportedError` rather than silently pretending.

If the measured block raises, the original exception is re-raised. Benchmark
does not wrap user failures.

### 3.2 `Benchmark.time`

```amber
Benchmark.time(label = null, gc: false): ... -> TimePeriod
```

Convenience form for callers that only need the duration. It has the same timing
semantics as `measure`, but discards the block result.

### 3.3 `Benchmark.run`

```amber
Benchmark.run(
  label = null,
  iterations: 1,
  warmup: 0,
  samples: 1,
  min_time: null,
  gc: false
) |i|: ... -> Benchmark.Report
```

Runs a block many times and returns a `Benchmark.Report`. The block receives an
iteration index. Warmup iterations run first and are not included in statistics;
their index starts at `0`. Measured iterations then restart at `0` for each
sample.

`iterations:` is the measured iteration count per sample. `samples:` repeats the
measured loop and stores one duration per sample. If `min_time:` is a
`TimePeriod`, the implementation may increase iterations until each sample
lasts at least that long; the actual iteration counts are reported.

`Benchmark.run` is intentionally one case at a time. Multi-case comparison is
handled by `Benchmark.compare`, which makes cross-case options explicit.

### 3.4 `Benchmark.compare`

```amber
Benchmark.compare(report, ...) -> Benchmark.CompareReport
Benchmark.compare([report, ...]) -> Benchmark.CompareReport
```

`Benchmark.compare` compares reports already produced by `Benchmark.run`:

```amber
compact = Benchmark.run("compact", iterations: 5_000, warmup: 500, samples: 5) |i|:
  Json.generate(value)

pretty = Benchmark.run("pretty", iterations: 5_000, warmup: 500, samples: 5) |i|:
  Json.pretty_generate(value)

report = Benchmark.compare(compact, pretty)
```

`CompareReport#cases` preserves declaration order. `fastest`, `slowest`, and
`relative` are computed from mean elapsed time per iteration.

`Benchmark.CompareReport` fields and methods:

| Name | Value |
| --- | --- |
| `cases` | ordered `List<Benchmark.Report>` |
| `fastest` | report with the lowest mean per-iteration time, or `null` |
| `slowest` | report with the highest mean per-iteration time, or `null` |
| `relative` | ordered rows with `label`, `mean_ns`, and `ratio_to_fastest` |
| `map` / `to_map` | canonical JSON-safe Map |
| `to_json(pretty: false)` | canonical JSON string |
| `pretty(...)` / `table(...)` / `format(...)` | human table string |
| `to_str` / `inspect` | compact human summary |

## 4. Section profiling

### 4.1 `Benchmark.profile`

```amber
Benchmark.profile(label = null, gc: false) |profiler|: ... -> Benchmark.Profile
```

`Benchmark.profile` measures a larger block and lets user code mark named
sections. It returns a `Benchmark.Profile` whose `value` is the profile block
return value.

The profiler object is block-confined: it must not be stored and used after the
profile block returns. Cross-task use raises `BenchmarkProfileError`.

### 4.2 `Profiler#section`

```amber
profiler.section(label, data: null): ... -> value
```

Runs the block, records a span, and returns the block value. Sections may be
nested.

```amber
profile = Benchmark.profile("render") |p|:
  p.section("load"):
    load_template()

  p.section("render", data: {template: "invoice"}):
    render_invoice()
```

Span fields:

| Name | Value |
| --- | --- |
| `label` | `Str` |
| `data` | `Map`, `StrictMap`, or `null`; stored as supplied |
| `elapsed` | inclusive `TimePeriod` |
| `elapsed_ns` | inclusive nanoseconds |
| `self` | exclusive `TimePeriod`, subtracting completed child spans |
| `self_ns` | exclusive nanoseconds |
| `depth` | nesting depth, root children start at `0` |
| `children` | direct child spans |

### 4.3 Profile reports

`Benchmark.Profile` fields and methods:

| Name | Value |
| --- | --- |
| `label` | `Str` or `null` |
| `total` | total `TimePeriod` of the profile block |
| `total_ns` | total nanoseconds |
| `value` | profile block result |
| `spans` | flat preorder `List` of spans |
| `summary` | aggregate rows grouped by label |
| `find(label)` | first span with label, or `null` |
| `map` / `to_map` | canonical JSON-safe report without `value` |
| `map(value: :raw)` / `to_map(value: :raw)` | includes the raw `value`; not guaranteed JSON-safe |
| `to_json(pretty: false)` | canonical JSON string |
| `pretty(...)` / `table(...)` / `format(...)` | human table string |
| `to_str` / `inspect` | compact human summary |

Summary rows contain `label`, `count`, `total`, `total_ns`, `self`, `self_ns`,
`mean`, `mean_ns`, `min`, `min_ns`, `max`, and `max_ns`.

## 5. Statistics

`Benchmark.Report` is the result of `run`. It has:

| Name | Value |
| --- | --- |
| `label` | `Str` or `null` |
| `iterations` | total measured iterations |
| `samples` | sample count |
| `elapsed` | total measured `TimePeriod` |
| `elapsed_ns` | total nanoseconds |
| `per_iteration` | mean `TimePeriod` per iteration |
| `mean`, `min`, `max` | per-iteration `TimePeriod` |
| `p50`, `p90`, `p95`, `p99` | percentile per-iteration `TimePeriod` |
| `ops_per_second` | `Float` |
| `sample_times` | `List<TimePeriod>` |
| `sample_ns` | `List<Int>` |
| `map` / `to_map` | canonical JSON-safe Map |
| `to_json(pretty: false)` | canonical JSON string |
| `pretty(...)` / `table(...)` / `format(...)` | human table string |
| `to_str` / `inspect` | compact human summary |

Percentiles are nearest-rank over sample per-iteration durations. With one
sample, every percentile equals that sample. The implementation must report
actual iteration counts if `min_time:` auto-scales them.

## 6. Presentation and data interchange

No Benchmark API writes output implicitly.

### 6.1 Pretty printing

```amber
text = report.pretty(layout: :summary, unit: :auto)
table = compare_report.table(unit: :ms, style: :ansi, highlight: :best)
profile_text = profile.pretty(unit: :us, sort: :self)
```

Formatting APIs:

| API | Result |
| --- | --- |
| `result.format(...)` | general formatter |
| `result.table(...)` | table shorthand |
| `result.pretty(...)` | readable default layout for the result kind |
| `Benchmark.format(result, ...)` | compatibility/interop form |
| `Benchmark.table(result, ...)` | compatibility/interop form |
| `Benchmark.pretty(result, ...)` | compatibility/interop form |

`layout:` accepts `:summary`, `:table`, and `:tree`. `:tree` applies only to
profiles and renders nested spans. `unit:` accepts `:auto`, `:ns`, `:us`,
`:ms`, and `:s`. `sort:` applies to profile summaries and accepts `:total`,
`:self`, `:count`, or `:source_order`.

`style:` accepts:

| Style | Meaning |
| --- | --- |
| `:plain` | no ANSI escapes; default |
| `:ansi` | xterm-compatible ANSI SGR styling |
| `:xterm` | alias for `:ansi` |

`highlight:` accepts `:none` or `:best`. With `style: :ansi` and
`highlight: :best`, the best row or best cell is wrapped in ANSI bold
(`ESC[1m` ... `ESC[22m`). For `CompareReport`, "best" means the lowest mean
per-iteration time unless `metric:` is supplied. For `Profile`, "best" means the
largest selected `sort:` metric, because profiles are usually read as "where did
time go?" rather than "which row won?".

Formatted comparison tables must include at least:

```text
case | iterations | mean | p95 | ops/s | relative
```

Formatted profile tables must include at least:

```text
section | count | total | self | mean | max
```

`format` returns `Str`. Programs that want output use ordinary IO:

```amber
p report.pretty
```

### 6.2 Canonical Map and JSON export

Every result object supports:

```amber
map = report.map
same = report.to_map
json = report.to_json(pretty: true)
```

`to_map` returns only JSON-representable Amber values: `Map`, `List`, `Str`,
`Int`, `Float`, `Bool`, and `null`. Runtime duration objects are exported as
integer nanosecond fields and optional display strings, never as `TimePeriod`
values.

Canonical maps include:

| Key | Value |
| --- | --- |
| `kind` | `"measurement"`, `"report"`, `"compare_report"`, or `"profile"` |
| `schema` | `"amber.benchmark.v1"` |
| `label` | `Str` or `null` |
| `created_by` | optional implementation string |
| `data` | result-kind-specific Map |

Duration fields use the `_ns` suffix. Display-only fields use the `_text`
suffix. Consumers should use `_ns` fields for computation.

`to_json(pretty: true)` returns the canonical pretty JSON representation of the
safe map. `to_json` returns the compact form. Amber code should prefer
`result.to_json(...)` over spelling this as a separate `Json.generate(...)`
pipeline, because Benchmark results are intended to chain left to right.

`Benchmark.to_map(result)` and `Benchmark.to_json(result)` are compatibility
forms for host interop and dynamic dispatch paths. They are deliberately not the
preferred style for Amber code; write `result.map`, `result.to_map`, and
`result.to_json` instead.

The measured block value is excluded by default. `to_map(value: :raw)` may
include it for in-process inspection, but such maps are not guaranteed to be
JSON-safe and must not be accepted by `from_map` as portable benchmark data.

### 6.3 Importing results

Benchmark result data can be restored from canonical maps or JSON:

```amber
report = Benchmark.from_map(data)
same = Benchmark.from_json(json)
```

`from_map` validates `schema`, `kind`, required keys, numeric ranges, and list
shapes. `from_json` parses through `Json.parse(text, map: StrictMap)` and then
delegates to `from_map`.

Imported results are read-only result objects with the same accessors,
`to_map`, `to_json`, and formatting methods as live results. They do not contain
the measured block `value`.

## 7. Clocks, effects, and replay

Benchmark uses the host monotonic clock only. It does not use wall-clock time,
so reports are durations, not timestamps.

Effects:

- `measure`, `time`, `run`, `compare`, and `profile` observe `!{time}`.
- `format`, `pretty`, `table`, `to_map`, `to_json`, `from_map`, `from_json`,
  and ordinary report accessors are pure aside from allocation.

Capabilities:

- capability-aware hosts treat Benchmark as monotonic-time access under the
  existing `time` capability family;
- denied clock access raises `CapabilityError`;
- replay-enforced worlds without a recorded monotonic provider raise
  `DeterminismError`, matching `Time.monotonic`.

## 8. Errors

Error classes:

```text
BenchmarkError < Exception
BenchmarkUnsupportedError < BenchmarkError
BenchmarkProfileError < BenchmarkError
BenchmarkImportError < BenchmarkError
```

Typical failures:

| Situation | Error |
| --- | --- |
| missing block for measuring/profiling APIs | `TypeError` |
| negative `iterations`, `warmup`, or `samples` | `ArgumentError` |
| `samples == 0` or `iterations == 0` without `min_time:` | `ArgumentError` |
| unknown `unit:` / `sort:` / metric option | `ArgumentError` |
| profiler used after its block returns | `BenchmarkProfileError` |
| profiler used from a different task/strand | `BenchmarkProfileError` |
| requested `gc: true` on a runtime without explicit collection | `BenchmarkUnsupportedError` |
| invalid benchmark map/json import | `BenchmarkImportError` |

User block exceptions propagate unchanged.

## 9. Implementation notes

The native implementation should live in `runtime/stdlib_benchmark.cpp` and
register the top-level path `Benchmark`.

The current Layer 0 stdlib facade already exposes:

```text
NativeStdlibCall::monotonic_time()
NativeStdlibCall::call_block(...)
NativeStdlibCall::make_object(...)
NativeStdlibCall::make_list(...)
```

That is enough for a first implementation whose report objects are immutable
native records or ordinary Map/List shapes. A conservative first landing can
return ordinary Maps for reports and later introduce native value wrappers only
if method dispatch or memory layout makes that worthwhile.

Implementation order:

1. Add `RuntimeNativeTypeKind::Benchmark` and path registration.
2. Implement `Benchmark.time` and `Benchmark.measure`.
3. Add `Benchmark.run` statistics over repeated block calls.
4. Add `Benchmark.profile` with block-confined profiler state and nested
   sections.
5. Add `Benchmark.compare`.
6. Add canonical `map`/`to_map`/`to_json` and `from_map`/`from_json`.
7. Add result-method `format`, `table`, and `pretty`,
   including ANSI bold highlighting.
8. Add VM tests for chaining exports, option validation, import failures,
   compare/profile formatting, stable map shapes, and nested sections.

## 10. Rationale

`measure` and `time` cover the common small case without introducing a builder.
`run` is deliberately explicit about warmup, iterations, and samples, because
hidden auto-benchmarking tends to produce pretty but misleading numbers.
`profile` uses user-labeled sections instead of automatic sampling because it is
portable across the VM and native backends and can be implemented using the
existing monotonic clock facade.

The result objects expose `TimePeriod` first and raw nanoseconds second. That
keeps the public API idiomatic for Amber code while still making report export
and numeric comparison straightforward.

Human output is intentionally a formatter, not a side effect. This keeps
Benchmark useful in command-line tools, tests, and web/reporting code: the same
result can be printed as an xterm-bold table for a terminal, written as plain
text to a log, or exchanged as canonical JSON.
