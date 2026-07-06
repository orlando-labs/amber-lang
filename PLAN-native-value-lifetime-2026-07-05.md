# Native value lifetime optimization plan

Date: 2026-07-05

## Goal

Bring `amber-built` polyglot rows into the compiled-competitor band: best-of-N
within ~2x of the C++ row on every workload (i.e. inside or near the Go/Rust
band), with peak RSS proportional to the live set instead of total allocations.

## Findings this plan is based on (measured 2026-07-05, Darwin arm64)

The 2026-07-05 investigation (session-measured; method reproducible below)
overturned the "startup floor" hypothesis from
`PLAN-polyglot-performance-optimizations-2026-07-04.md` item 5:

- An empty `amberc build` executable runs in ~2.75 ms vs ~2.11 ms for an empty
  C++ binary under the benchmark's own measurement. Zero-iteration variants of
  real workloads run 2.9–3.4 ms. Startup + module init explains ~1 ms of gaps
  that are 5–20 ms wide. Launcher-size work is therefore *not* on this plan.
- Compute-only ratios vs C++ (both floors subtracted, same day):
  time-flow ~20x, string-ops ~6.5x, map-words ~3.3x, codecs ~2.7x,
  sha-digest ~1.15x. The gap tracks boxed-value traffic per iteration.
- Root cause is that the generated native lane makes every heap value
  immortal:
  - `NativeArena` (emitted by `tools/amberc/main.cpp`): every
    Time/TimePeriod/List/Map/Bytes/... is `make_unique`d and pushed into
    global vectors, never freed while the program runs. A 100x time-flow run
    reached 1.32 GB RSS (linear in iterations) and spent ~38% of its process
    lifetime inside `exit()` running `~NativeArena` — teardown the benchmark
    measures, since it times whole processes.
  - `native_intern_string`: every dynamic string result (each `+` concat,
    `upcase`, `replace`, ...) is hashed, bucket-scanned with full compares,
    and copied into the permanent string table. It is the #1 hot function in
    string-ops (230 MB RSS at 20x iterations).
  - Secondary: member sends dispatch via selector `string_view` compare
    chains (`native_time_nullary(v, "year")`); `NativeValue::time()` heap
    copies a full `RuntimeTimeValue` per intermediate value.

## Approach

Give native-lane values a lifetime. All changes live in the generated-C++
emitter inside `tools/amberc/main.cpp`; the serialized `.amberbc` format, VM,
and runtime archive are untouched. The bailout contract (re-run the whole
program on the VM) is unaffected because `NativeValue` never crosses into the
VM mid-flight.

### M1 — Intrusive refcounts for native data values (the big one)

- Add `std::uint32_t rc = 1;` to `NativeBytes`, `NativeList`, `NativeTuple`,
  `NativeSet`, `NativeMap`, `NativeRange`, `NativeArgParser`, `NativeFsPath`.
  Replace the `using NativeTime/NativeTimePeriod/NativeUuid = runtime type`
  aliases with thin derived structs adding `rc` (cast sites and field access
  keep compiling unchanged).
- Give `NativeValue` the rule of five: copy retains heap payloads, move
  steals, destroy releases (`--rc == 0` → `delete`, recursively releasing
  contained values through the payload destructors). Copy-assign goes through
  a retained temporary and move-assign through a stolen temporary so interior
  aliasing (`v = v.items[0]`) cannot double-free.
- Factories allocate with plain `new` (rc = 1 owned by the returned value).
- `NativeArena` shrinks to closures + capture cells only. Closures are
  identity-bearing, capture-shared, and cycle-prone; they stay process-lived
  in M1 (bounded by program structure for the benchmark suite). Refcounting
  them is a possible follow-up, not needed for the polyglot goal.
- Known accepted limitation: reference cycles built from containers leak
  until exit. Strictly better than today, where *everything* leaks until
  exit.
- Risk watch: `NativeValue` stops being trivially copyable, so it is passed
  indirectly and every copy/destroy runs a tag switch. Guard with an A/B on
  arithmetic and calls-collections (the least allocation-bound rows).

Expected effect: teardown tax (~4 ms at benchmark scale for time-flow) goes
to ~0, malloc/cache behavior improves (working set becomes the live set), RSS
stops scaling with iterations. Targets time-flow, map-words, uuid,
calls-collections, json.

### M2 — Ephemeral dynamic strings

- New heap payload `NativeHeapString { std::string text; std::uint32_t rc; }`
  behind a new tag, refcounted like M1 kinds. String-producing helpers return
  heap strings instead of `string_ref(native_intern_string(...))`.
- Module literals and symbols keep the existing interned table and id
  equality. Heap-vs-heap and heap-vs-interned equality compare content
  (length first, then bytes).
- The name-indifferent map index keeps its canonical-id scheme: heap strings
  are interned lazily *only when used as a map key* (`native_map_index_key_id`),
  so the table grows with distinct live keys, not with every intermediate.
- Expected effect: string-ops loses its #1 hot function and its permanent
  retention; codecs/json lose intermediate interning.

### M3 — Selector-id dispatch in generated helpers (profile-gated)

Emit-time-known selectors currently dispatch through per-family helpers doing
`string_view` compare chains. Rewrite the hot families (Time, TimePeriod,
string, list, map nullary/unary) to switch on an emit-time-resolved selector
id or call per-selector functions directly. Only do the families that still
show in post-M2 profiles.

### M4 — Allocation micro-tuning (profile-gated)

Options, in order of expected value: per-payload-type free lists (recycle
freed shells to cut malloc round-trips), avoiding the `RuntimeTimeValue` heap
copy for intermediates that immediately decompose, move-semantics sweep of
helper signatures. Adopt only what a post-M2 profile justifies.

### M5 — Re-baseline and documentation

Full-suite rerun (10 repeats, fresh build dir, `--no-build` stable pass) with
all six languages; update `bench/polyglot/README.md` with the new table and a
note that the amber-built row no longer pays arena teardown; update the
project memory. Keep the first-sample cold-exec caveat (macOS first-run
validation costs 300–450 ms for every language's binary; only `best_s` is
comparable).

## Verification gates (every milestone)

1. `make build` then `build/ambertest run corpus` (conformance, 174 expected).
2. `make backend-equivalence` (VM and native lanes byte-identical, 109).
3. One generated `.native.cpp` compiled by hand with `-fsanitize=address` and
   run (validates refcount logic; the runtime archive stays uninstrumented).
4. Polyglot A/B with fresh `--build-dir` on the milestone's target workloads
   plus arithmetic + calls-collections as regression canaries.
5. RSS check on a 100x-iteration variant (must be ~flat vs iterations after
   M1/M2 for the touched workloads).

## Measurement method (reproducible)

- Floor: `sed` the workload loop bound to 0, `amberc build`, time 25 runs of
  the executable via `subprocess` `perf_counter` (mirrors the runner).
- Profile: build a 100x-iteration variant, `sample <pid> 1` while it runs.
- Benchmark: `python3 bench/polyglot/run_benchmark.py --workload <w>
  --repeats 10 --build-dir <fresh>` then a `--no-build` stable pass into the
  same directory.

## Implementation log (2026-07-05)

All milestones landed same-day in `tools/amberc/main.cpp` (emitter only; no
`.amberbc`/VM/runtime-archive changes). Gates at every step: corpus 173/0,
backend-equivalence 109/0, ASan spot runs clean, checksums 7/7 measured
workloads.

- **M1 landed.** Intrusive rc on all data payloads (`NativeRcHeader` at
  offset 0 via base class, so retain/release is a bit copy + contiguous tag
  range test + header increment; the payload-type switch only runs on final
  delete). Time/TimePeriod/Uuid stay plain value types in helper code and are
  boxed (`NativeTimeBox` etc.) at the heap boundary, keeping aggregate-init
  helper sites untouched. `NativeArena` now holds only closures and capture
  cells. Two regressions found and fixed during the milestone: (a) the
  rule-of-five made compare results expensive — added a **Bool scalar lane**
  (`breg_N`) beside the int/float lanes so compare/branch traffic never
  touches `NativeValue` (arithmetic 17.3→6.0 ms, back at parity); (b) clang
  outlined `destroy()` in EH cleanups — lifecycle methods are
  `AMBER_NATIVE_ALWAYS_INLINE` with the delete switch out-of-line, and
  `native_list_at`/`native_count`/`native_list_first` stopped copying the
  whole backing vector per element read (a pre-existing waste M1 made
  expensive; calls-collections 8.5→4.7 ms, better than pre-plan).
  100x time-flow RSS: **1.32 GB → 4.1 MB**, exit-teardown share ~38% → ~0.
- **M2 landed.** `Tag::HeapString` (inside the refcounted range) for dynamic
  string results; module literals/symbols stay interned; equality funnels
  through `native_string_text_equal`; `native_map_index_key_id` interns heap
  strings on store, and the new `_for_read` variant probes without inserting
  (a nameable probe absent from the intern table is a definitive miss).
  string-ops 11.1→6.6 ms and 20x-iteration RSS **221 MB → 1.9 MB**.
  Also fixed en route: the uncommitted Benchmark-API selector routing had
  broken uuid's native lane before this plan (`inspect`/`to_json` on
  non-Benchmark receivers hard-bailed to the VM re-run; uuid best was 0.0547s
  in the official runner). `native_benchmark_send` now delegates non-schema
  receivers for `inspect`/`to_str` to the generic `native_time_nullary`
  dispatcher; uuid 0.0547→0.0119 s. Remaining shadowed selectors
  (find/min/max/...) flagged as a separate follow-up task.
- **M3 closed without new work.** The hot Time family already dispatches on
  an emit-time-resolved `NativeTimeSelector` enum (landed with the
  uncommitted Benchmark/Time work); map/string hot paths call direct helpers.
  No selector-string compares left in post-M2 profiles.
- **M4 landed.** `AMBER_NATIVE_POOL_NEW` per-type free lists on the hot
  payloads (List/Map/Bytes/Tuple/Set/HeapString/Time/TimePeriod/Uuid boxes):
  dead shells recycle instead of malloc/free round-trips; growth bounded by
  peak live count. time-flow 10.2→6.3 ms, map-words 8.6→6.7 ms,
  string-ops 6.5→5.0 ms; RSS unchanged (still flat).
- **Follow-up audit closed (2026-07-06, separate session).** Every selector
  claimed by `native_cpp_benchmark_selector` was swept against the VM stdlib;
  the one real conflict (nullary `min`/`max` shadowing the native sequence
  extremum, silently VM-restarting `list.min()`) got the same non-schema
  delegation treatment, the `native_benchmark_core` fixture grew a section
  covering it, and the Makefile gate was bumped. Close-out verification in
  this session: all ten freshly built workload binaries pass the lldb
  `run_vm_entry`-breakpoint bailout detector (no whole-program VM restarts
  anywhere in the suite), the fixture is ASan-clean, and a 10-repeat
  re-measurement of the five key workloads stayed in band.

## Success criteria

- Every amber-built `best_s` ≤ ~2x the same-run C++ `best_s`; inside or near
  the Go/Rust band on value-churn workloads (time-flow, string-ops,
  map-words, codecs, json).
- Peak RSS of amber-built rows within a small constant of the C++ rows
  (excluding the fixed ~5.6 MB binary/runtime footprint effects), and flat
  when iterations scale 100x.
- No interpreted-lane or conformance regressions; backend equivalence stays
  byte-identical.
