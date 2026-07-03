# DESIGN: Time zones, local calendar helpers, and flexible time text

Status: implemented runtime design
Date: 2026-07-02
Target: Amber standard library `Time` / `TimePeriod` extension
Scope: time-zone-aware `Time` values, fixed-offset and IANA time-zone lookup,
local calendar boundary helpers, expanded `Time.parse`, and formatted
`Time#to_str`
Out of scope for v1: mutable time-zone changes, leap-second modeling, locale
objects, non-Gregorian calendars, cron/schedule recurrence, and host-dependent
implicit local time

## 1. Goals

This design extends the existing UTC-instant `Time` module without changing the
core identity of `Time`: a `Time` is still an instant with nanosecond precision.
The extension adds an attached display/resolution time zone so that field access,
formatting, and local calendar helpers can be expressed in user-facing civil
time.

The surface keeps Amber's stdlib conventions:

- lower_snake_case selectors;
- pure methods by default;
- no `!` forms unless receiver mutation exists;
- `to_str` as the VM-wide string conversion convention;
- explicit deterministic defaults instead of hidden host-local behavior.

## 2. Core model

`Time` becomes:

```text
instant = epoch_seconds + nanosecond
zone    = display/resolution TimeZone
```

The instant determines equality, ordering, Unix epoch fields, and duration
difference. The zone determines calendar fields, `to_str`, parsing of local
texts, and helpers such as `start_of_day`.

```amber
t = Time.parse("2026-07-02T12:00:00Z")

t.in_tz("Europe/Moscow").to_str
# "2026-07-02T15:00:00+03:00"

t.as_tz("Europe/Moscow").to_str
# "2026-07-02T12:00:00+03:00"
```

The important split:

- `in_time_zone` / `in_tz` keeps the same instant and changes the displayed local
  digits.
- `as_time_zone` / `as_tz` keeps the same displayed local digits and changes the
  instant by interpreting those digits in another zone.

Both operations are pure and return a new `Time`.

## 3. No mutating time-zone API

The brainstormed mutating forms are deliberately not adopted:

```amber
time.in_tz!(zone)       # not provided
time.as_tz!(zone)       # not provided
time.time_zone = zone   # not provided
```

`Time` should remain scalar-like. A call site that changes a time's zone should
produce a new value:

```amber
local = time.in_tz("Europe/Moscow")
wall  = time.as_tz("Europe/Moscow")
```

This preserves the Amber mutation law: no bang, no receiver mutation; no
setter-like spelling for what is better modeled as value transformation.

## 4. `TimeZone`

Introduce an immutable `TimeZone` value type.

```amber
utc = TimeZone.utc
moscow = TimeZone["Europe/Moscow"]
fixed = TimeZone.offset("+03:00")
same = Time.time_zone("Europe/Moscow")
```

Core API:

```amber
TimeZone.utc
TimeZone.offset("+03:00")
TimeZone.offset(seconds: 10_800)
TimeZone["Europe/Moscow"]
TimeZone.find("Europe/Moscow")      # -> TimeZone?
TimeZone.local                      # explicit host-local lookup

zone.name                           # "Europe/Moscow" or "+03:00"
zone.fixed?                         # true for fixed offsets
zone.offset_at(time)                # TimePeriod, usually whole minutes
zone.abbreviation_at(time)          # "MSK", "UTC", ...
```

`Time.time_zone(name)` is an alias for lookup, included because it reads well at
call sites. `TimeZone[...]` is the compact form for code that already thinks in
terms of zone objects.

### 4.1 Determinism

Fixed-offset zones are pure and deterministic.

IANA zones use the bundled tzdb snapshot by default. A runtime may also provide
host tzdb lookup as fallback, but replay/deterministic execution must either
record the tzdb version or reject host-dependent fallback lookup.

`TimeZone.local` is explicit because it reads host configuration. It requires the
same kind of replay care as wall-clock time. `Time.parse` does not silently use
`TimeZone.local`.

## 5. Zone conversion and reinterpretation

```amber
time.time_zone              # -> TimeZone
time.in_time_zone(zone)     # alias: in_tz
time.as_time_zone(zone)     # alias: as_tz
```

`zone` may be a `TimeZone`, a fixed-offset string such as `"+03:00"`, or an IANA
name such as `"Europe/Moscow"`.

### 5.1 `in_time_zone`

`in_time_zone` keeps the instant unchanged:

```amber
t = Time.parse("2026-07-02T12:00:00Z")
m = t.in_tz("Europe/Moscow")

t.unix_nanoseconds == m.unix_nanoseconds
# true

m.hour
# 15
```

### 5.2 `as_time_zone`

`as_time_zone` keeps local fields unchanged and resolves them in the requested
zone:

```amber
t = Time.parse("2026-07-02T12:00:00Z")
m = t.as_tz("Europe/Moscow")

m.to_str
# "2026-07-02T12:00:00+03:00"

m.in_tz(:utc).to_str
# "2026-07-02T09:00:00Z"
```

Because local civil times can be invalid or ambiguous during daylight-saving
transitions, `as_time_zone` accepts explicit resolution policy:

```amber
time.as_tz(
  "America/New_York",
  on_gap: "raise",      # "raise" | :forward | :backward
  on_fold: :earlier     # :earlier | :later | "raise"
)
```

Defaults:

- `on_gap: "raise"`
- `on_fold: :earlier`

Gaps raise `TimeZoneGapError` by default. Ambiguous folds choose the earlier
instant by default, matching the common "first occurrence" behavior while still
allowing strict code to request `on_fold: "raise"`.

## 6. Calendar fields and arithmetic

Existing field methods become zone-aware:

```amber
time.year
time.month
time.day
time.hour
time.minute
time.second
time.nanosecond
time.weekday
time.yearday
```

They read the fields in `time.time_zone`. UTC-created values keep the current
behavior because their zone is `TimeZone.utc`.

Calendar `TimePeriod` arithmetic also uses `time.time_zone` for month/year/day
rollover. Fixed-duration arithmetic remains instant arithmetic.

## 7. Local boundary helpers

Canonical names use `start_of_*`.

```amber
time.start_of_minute
time.end_of_minute
time.start_of_hour
time.end_of_hour
time.start_of_day
time.end_of_day
time.start_of_week(week_start: :monday)
time.end_of_week(week_start: :monday)
time.start_of_month
time.end_of_month
time.start_of_quarter
time.end_of_quarter
time.start_of_year
time.end_of_year
```

Discouraged aliases exist for Rails familiarity:

```amber
time.beginning_of_day
time.beginning_of_week
time.beginning_of_month
time.beginning_of_quarter
time.beginning_of_year
```

Documentation should present `start_of_*` first and mark `beginning_of_*` as
compatibility/readability aliases, not the preferred spelling.

Boundary helpers are pure. They return new `Time` values in the receiver's zone.

`end_of_*` means "start of the next unit minus 1 nanosecond", computed in local
civil time. It must not be implemented as `start + 24.hours - 1.nanosecond`,
because DST days can be shorter or longer than 24 hours.

```amber
time.in_tz("America/New_York").start_of_day
# local New York midnight
```

## 8. Parsing

`Time.parse` accepts an optional `format:` keyword. The format may be a `Symbol`
preset or a `Str` pattern.

```amber
Time.parse(text)
Time.parse(text, format: :auto)
Time.parse(text, format: :iso8601)
Time.parse(text, format: :ru_datetime, zone: "Europe/Moscow")
Time.parse(text, format: "%d.%m.%Y %H:%M", zone: "+03:00")
```

### 8.1 Parse signature

```amber
Time.parse(
  text,
  format: :auto,
  zone: :utc,
  on_gap: "raise",
  on_fold: :earlier
)
```

`zone:` is used for local/no-offset input and for the display zone of the result.
Accepted values:

- `:utc` - deterministic default and legacy-compatible display zone;
- `:parsed` - preserve a parsed numeric offset or named zone when the text
  contains one; fall back to UTC for local text;
- `:local` - explicit host-local zone, effectful/replay-sensitive;
- a `TimeZone`;
- a zone string such as `"Europe/Moscow"` or `"+03:00"`.

If the input contains an offset and `zone: :utc`, the instant is normalized to
UTC, preserving current `Time.parse` behavior. If the caller wants the parsed
offset to drive displayed fields, use `zone: :parsed`.

Local/no-offset input is resolved in `zone:`. For deterministic programs,
provide an explicit zone rather than `:local`.

The current grammar reserves `raise`, so strict policy values should be written
as `"raise"`. The runtime policy parser accepts text-equivalent `Symbol` or
`Str` values, so a future reserved-symbol spelling can remain compatible.

### 8.2 Symbol formats

`format: :auto` recognizes common unambiguous forms:

```text
2026-07-02T12:30:45Z
2026-07-02T12:30:45.123456789+03:00
2026-07-02 12:30:45 +0300
2026-07-02 12:30:45
2026-07-02 12:30
2026-07-02
02.07.2026 12:30:45
02.07.2026 12:30
02.07.2026
Thu, 02 Jul 2026 12:30:45 GMT
```

Ambiguous slash dates such as `07/02/2026` are not accepted by `:auto`; callers
must provide an explicit string pattern.

Named presets:

| Format | Examples |
| --- | --- |
| `:auto` | unambiguous common forms above |
| `:iso8601` | `2026-07-02T12:30:45Z`, `2026-07-02T12:30:45+03:00` |
| `:rfc3339` | strict internet timestamp profile |
| `:date` | `2026-07-02` |
| `:datetime` | `2026-07-02 12:30`, `2026-07-02 12:30:45` |
| `:http_date` | `Thu, 02 Jul 2026 12:30:45 GMT` |
| `:ru_date` | `02.07.2026` |
| `:ru_datetime` | `02.07.2026 12:30`, `02.07.2026 12:30:45` |
| `:ru_long` | `2 июля 2026`, `2 июля 2026 12:30` |

Russian named formats accept Russian month names in genitive form for
`:ru_long` (`января`, `февраля`, ...), and may accept the limited zone marker
`МСК` / `мск` as `"Europe/Moscow"`. Other abbreviations are rejected unless the
format string and zone resolver explicitly support them.

### 8.3 String formats

A string format is a strftime-compatible pattern used for parsing:

```amber
Time.parse("02/07/2026 15:30", format: "%d/%m/%Y %H:%M", zone: "+03:00")
```

The supported v1 directive set should be explicit and portable:

| Directive | Meaning |
| --- | --- |
| `%Y` | four-digit year |
| `%y` | two-digit year, windowed by documented runtime policy |
| `%m`, `%-m` | month number |
| `%d`, `%-d` | day of month |
| `%H` | hour, 00-23 |
| `%M` | minute |
| `%S` | second |
| `%N` | fractional seconds, 1-9 digits |
| `%L` | milliseconds, 3 digits |
| `%z` | numeric offset, `+0300` |
| `%:z` | numeric offset, `+03:00` |
| `%Z` | recognized zone name/abbreviation; strict and implementation-defined |
| `%F` | `%Y-%m-%d` |
| `%T` | `%H:%M:%S` |
| `%%` | literal percent |

Unsupported directives raise `ArgumentError`. Invalid text raises
`TimeParseError`.

## 9. Formatting with `to_str`

`to_str` is the encouraged formatting API. It accepts zero or one positional
argument. The argument may be a `Symbol` preset or a `Str` pattern.

```amber
time.to_str
time.to_str(:iso8601)
time.to_str(:date)
time.to_str(:datetime)
time.to_str(:ru_date)
time.to_str(:ru_datetime)
time.to_str("%Y-%m-%d %H:%M:%S %:z")
```

`time.to_str` with no argument is equivalent to `time.to_str(:iso8601)`.

There is no separate encouraged `format` or `strftime` method in v1. A string
argument to `to_str` provides the needed strftime-style power without adding a
second formatting verb.

### 9.1 Format presets

| Format | Output example |
| --- | --- |
| `:iso8601` | `2026-07-02T15:30:45+03:00`, `2026-07-02T12:30:45Z` |
| `:iso8601_zone` | `2026-07-02T15:30:45+03:00[Europe/Moscow]` |
| `:date` | `2026-07-02` |
| `:datetime` | `2026-07-02 15:30:45` |
| `:ru_date` | `02.07.2026` |
| `:ru_datetime` | `02.07.2026 15:30:45` |
| `:ru_long` | `2 июля 2026 15:30` |
| `:http_date` | `Thu, 02 Jul 2026 12:30:45 GMT` |

`:http_date` always formats in GMT/UTC, as required by HTTP.

`inspect` remains an inspection/debug surface and does not accept formatting
arguments.

## 10. ISO and JSON behavior

`time.iso8601` remains as a zero-argument convenience alias for
`time.to_str(:iso8601)`.

`to_json` should keep emitting an ISO-8601 string. For UTC values this is
unchanged. For non-UTC display zones, v1 should emit the offset form:

```amber
time.in_tz("Europe/Moscow").to_json
# "\"2026-07-02T15:00:00+03:00\""
```

If a caller needs to preserve IANA zone identity through JSON, they should store
the zone name separately or use `to_str(:iso8601_zone)` explicitly.

## 11. Errors

Existing:

- `TimeError`
- `TimeParseError < TimeError`

Add:

- `TimeZoneError < TimeError`
- `TimeZoneLookupError < TimeZoneError`
- `TimeZoneGapError < TimeZoneError`
- `TimeZoneAmbiguousError < TimeZoneError`

`TimeZoneLookupError` is raised for unknown IANA names or invalid offset syntax.
`TimeZoneGapError` is raised for nonexistent local times when `on_gap: "raise"`.
`TimeZoneAmbiguousError` is raised for folded local times when
`on_fold: "raise"`.

## 12. Implemented runtime notes

The current implementation includes:

- immutable `TimeZone` values for UTC, fixed offsets, host IANA zones, and
  explicit host-local lookup;
- bundled TZif parsing for common IANA files, including 64-bit transition tables
  and POSIX future-transition tails, with host tzdb fallback;
- `in_tz` instant conversion and `as_tz` local-field reinterpretation;
- DST gap/fold resolution policies for parsing and `as_tz`;
- zone-aware calendar fields, JSON/ISO formatting, `to_str` presets/patterns,
  Russian numeric and long-month parsing, and local boundary helpers.

This implementation vendors `third_party/tzdb/zoneinfo` as the default snapshot.
Deterministic/replay profiles still need to record the snapshot version and
reject or explicitly record any host-dependent fallback or `TimeZone.local`
lookup before they can be considered replay-stable.
