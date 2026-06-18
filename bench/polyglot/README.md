# Polyglot benchmark

This benchmark compares identical workloads across:

- Amber interpreted: `build/iamber --eval-file`
- Amber built native executable: `amberc build <file.am>` first, then the
  generated host binary
- Python
- Ruby
- C++ compiled with `-O2`
- Go when `go` is available in `PATH`

The Amber built path intentionally runs an already generated executable, so
compile time is not included in the measured run. The runner still builds fresh
`.amberbc` artifacts as a bytecode sanity check. For workloads that report
`amber-built`, that row is the native executable from `amberc build`, not
`amberbc_run`.

Run:

```sh
python3 bench/polyglot/run_benchmark.py --repeats 3
python3 bench/polyglot/run_benchmark.py --workload calls-collections --repeats 5
python3 bench/polyglot/run_benchmark.py --workload json --repeats 3
python3 bench/polyglot/run_benchmark.py --workload codecs --repeats 3
python3 bench/polyglot/run_benchmark.py --workload secure-random --repeats 3
python3 bench/polyglot/run_benchmark.py --workload time-flow --repeats 3
python3 bench/polyglot/run_benchmark.py --workload uuid --repeats 3
```

The script prints mean/best wall-clock time and peak RSS reported by a small
Python measurement helper via `resource.getrusage(RUSAGE_CHILDREN)`. It also
validates that every implementation returns the same checksum for the selected
workload:

- `arithmetic`: `715609516598740`
- `calls-collections`: `2047795430`
- `sha-digest`: `2242493101`
- `json`: `1531352227`
- `codecs`: `2056190`
- `secure-random`: `296000`
- `time-flow`: `110397732`
- `uuid`: `1040000`

The `json` workload exercises compact JSON generation, parse round-trips,
small pretty-generation round-trips, and streaming JSONL reads. The runner
prepares `bench/polyglot/build/json/events.jsonl` with 20,000 deterministic
records before measurement so peak RSS reflects streaming consumption instead
of input generation. The Amber built executable is compiled with
`--grant fs.read=bench/polyglot/build/json/events.jsonl` so its VM fallback can
profile the same file-I/O path under the capability-aware runtime world. The
runner requires full direct-native coverage for this workload; the generated
launcher still contains its ordinary bailout VM fallback, but the measured path
does not use it.

The `codecs` workload exercises `Bytes.new`, `Base64`, `Base64Url`, and `Hex`
encode/decode round-trips, including lenient base64/hex branches. The runner
requires full direct-native coverage for this workload as well.

The `secure-random` workload exercises `SecureRandom.bytes`, `.hex`,
`.base64`, `.base64url`, `.uuid`, and `.int(range)` using real OS entropy.
The checksum validates lengths, codec round-trips, UUID shape, and integer
range membership rather than random contents. The Amber built executable is
compiled with `--grant random.secure`. The runner requires full direct-native
coverage for this workload, including `Range.new`, `SecureRandom.int(range)`,
and String shape checks.

The `time-flow` workload exercises `Time.parse`, `Time.utc`,
`Time.from_unix_ms`, `Time.from_unix_ns`, numeric `TimePeriod` literals,
calendar month arithmetic with end-of-month clamping, `TimePeriod + Time`,
`Time - Time`, field reads, ISO formatting, and comparisons. The runner
requires full direct-native coverage for this workload; the generated launcher
still embeds its ordinary bailout VM fallback, but `native_entry` is true and
every bytecode code object has direct native code.

The `uuid` workload exercises UUID v4 and v7 generation, canonical parse and
format round-trips, `inspect`, JSON formatting, version extraction, byte-value
equality, type matching, the `UUID` alias, and `SecureRandom.uuid`. It uses real
OS entropy and wall time, while its checksum depends only on UUID invariants.
The Amber executable is compiled with `--grant random.secure`, and the runner
requires every bytecode code object to have direct native coverage.

Latest local `uuid` warm rerun on 2026-06-18 (Darwin arm64,
`go version go1.26.4 darwin/arm64`):

```sh
python3 bench/polyglot/run_benchmark.py --workload uuid --repeats 3 --no-build \
  --build-dir /tmp/amber-polyglot-uuid-final
```

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted      3     0.0508     0.0473          8.8            1040000
amber-built            3     0.0123     0.0114          5.8            1040000
python                 3     0.0985     0.0924         13.1            1040000
ruby                   3     0.2222     0.2130         24.0            1040000
cpp                    3     0.0080     0.0076          1.4            1040000
go                     3     0.0126     0.0113          9.0            1040000
```

Latest local `time-flow` warm rerun on 2026-06-18 (Darwin arm64,
`go version go1.26.4 darwin/arm64`):

```sh
python3 bench/polyglot/run_benchmark.py --workload time-flow --repeats 10 --no-build
```

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted     10     0.1026     0.1010          5.0          110397732
amber-built           10     0.0183     0.0181         11.3          110397732
python                10     0.2758     0.2411         18.8          110397732
ruby                  10     0.0640     0.0630         13.4          110397732
cpp                   10     0.0034     0.0031          1.4          110397732
go                    10     0.0059     0.0051          4.1          110397732
```

Latest local full-suite warm rerun on 2026-06-17 (Darwin arm64,
`go version go1.26.4 darwin/arm64`):

Each workload was first built once into a fresh
`/private/tmp/amber_polyglot_suite_20260617_*` directory. The tables below are
the immediate stable reruns against those fresh artifacts:

```sh
python3 bench/polyglot/run_benchmark.py --workload <workload> --repeats 10 --no-build --build-dir <fresh-build-dir>
```

Arithmetic:

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted     10     0.1602     0.1572          3.9    715609516598740
amber-built           10     0.0159     0.0154          1.4    715609516598740
python                10     0.2013     0.1917          8.3    715609516598740
ruby                  10     0.0768     0.0750         11.9    715609516598740
cpp                   10     0.0048     0.0044          1.3    715609516598740
go                    10     0.0066     0.0065          3.9    715609516598740
```

Calls and collections:

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted     10     0.0131     0.0129          5.3         2047795430
amber-built           10     0.0051     0.0046          1.4         2047795430
python                10     0.0246     0.0238          8.7         2047795430
ruby                  10     0.0119     0.0116         12.0         2047795430
cpp                   10     0.0038     0.0025          1.3         2047795430
go                    10     0.0033     0.0030          3.9         2047795430
```

SHA digest:

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted     10     0.0770     0.0751          6.4         2242493101
amber-built           10     0.0067     0.0063          2.1         2242493101
python                10     0.0991     0.0982          8.9         2242493101
ruby                  10     0.0365     0.0360         12.7         2242493101
cpp                   10     0.0027     0.0024          1.3         2242493101
go                    10     0.0033     0.0032          4.0         2242493101
```

JSON:

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted     10     0.0268     0.0261          9.5         1531352227
amber-built           10     0.0118     0.0112          7.1         1531352227
python                10     0.0433     0.0428          9.8         1531352227
ruby                  10     0.0193     0.0189         13.5         1531352227
cpp                   10     0.0043     0.0041          1.4         1531352227
go                    10     0.0144     0.0142          9.2         1531352227
```

Codecs:

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted     10     0.0450     0.0440         11.5            2056190
amber-built           10     0.0109     0.0105          8.0            2056190
python                10     0.0288     0.0283         11.2            2056190
ruby                  10     0.0169     0.0164         12.7            2056190
cpp                   10     0.0055     0.0053          1.4            2056190
go                    10     0.0039     0.0038          4.7            2056190
```

Secure random:

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted     10     0.0272     0.0260          7.1             296000
amber-built           10     0.0108     0.0105          4.3             296000
python                10     0.0571     0.0553         12.9             296000
ruby                  10     0.0241     0.0238         13.1             296000
cpp                   10     0.0062     0.0059          1.4             296000
go                    10     0.0062     0.0059          4.8             296000
```

Latest local `codecs` warm rerun on 2026-06-17 (Darwin arm64):

```sh
python3 bench/polyglot/run_benchmark.py --workload codecs --repeats 15 --no-build
```

This replaces the earlier 3-run cold sample whose means were skewed by first-run
process/cache outliers for the short native workloads.

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted     15     0.0433     0.0426         11.4            2056190
amber-built           15     0.0111     0.0105          8.0            2056190
python                15     0.0275     0.0267         11.3            2056190
ruby                  15     0.0165     0.0160         12.7            2056190
cpp                   15     0.0054     0.0051          1.4            2056190
go                    15     0.0038     0.0036          4.7            2056190
```

Latest local `json` rerun on 2026-06-17 after interpreted-VM JSON
optimizations (Darwin arm64, `go version go1.26.4 darwin/arm64`):

```sh
python3 bench/polyglot/run_benchmark.py --workload json --repeats 10
```

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted     10     0.0263     0.0260          9.5         1531352227
amber-built           10     0.0607     0.0111          7.0         1531352227
python                10     0.0439     0.0427          9.8         1531352227
ruby                  10     0.0209     0.0192         13.4         1531352227
cpp                   10     0.0381     0.0040          1.4         1531352227
go                    10     0.0460     0.0148          9.2         1531352227
```

Baseline before bytecode arithmetic fast paths:

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted      3     2.2844     2.2363          3.7    715609516598740
amber-built            3     2.2767     2.2587          2.5    715609516598740
python                 3     0.1232     0.1228         14.7    715609516598740
ruby                   3     0.0775     0.0772         16.0    715609516598740
cpp                    3     0.0042     0.0042          1.4    715609516598740
```

After integer bytecode fast paths (`IADD`/`ISUB`/`ILT`/`IGT` and `*K`
variants):

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted      3     0.3863     0.3833          3.6    715609516598740
amber-built-fresh      3     0.3888     0.3824          2.6    715609516598740
python                 3     0.1311     0.1288         14.7    715609516598740
ruby                   3     0.0878     0.0797         15.9    715609516598740
cpp                    3     0.0955     0.0045          1.4    715609516598740
```

`amber-built*` may be produced by the benchmark script with an existing
compiler cache. Because the cache key is source-hash based, it can reuse stale
bytecode after compiler/emitter changes.

The hot `main` loop disassembles to zero generic `SEND` instructions for `+`,
`-`, `<`, and `>` where operands are integer. It contains 15 integer
arithmetic/comparison opcodes in the loop body and 19 in the method.

VM profiling after those bytecode fast paths showed that the remaining time was
not in the benchmark runner. A fresh `.amberbc` run and `iamber --eval-file`
both settled around 0.38s. Instrumented counters for one fresh `amberbc_run`
execution showed:

```text
steps=19499815
integer_binary=11999999
integer_binary_fast=11999999
integer_binary_fallback=0
operand_u32=46499817
read_reg=23000004
unwrap_watch_probe=23000004
unwrap_watch_hit=0
write_reg=12000004
write_reg_watch_probe=12000004
write_reg_watch_hit=0
write_reg_pattern_cleanup=12000004
write_reg_pattern_cleanup_nonempty=0
value_is_integer=23999999
value_as_integer=23999999
safepoint=1000001
safepoint_no_gc=1000001
```

Temporary probes isolated the next cost:

```text
variant                    best_s   evidence
baseline                   0.3782   fresh .amberbc, existing VM
noop safepoint             0.3657   upper bound for safepoint lock/check cost
skip empty pattern erase   0.3475   avoids 12M empty unordered_map erases
direct integer registers   0.2710   avoids generic read/write/variant probes
```

The selected VM optimization keeps the same bytecode format and opcode mix. It
adds a plain-register path inside `IADD`/`ISUB`/`ILT`/`IGT` and `*K` when both
operands are already unboxed integer register payloads. Watched locals,
non-integer operands, uninitialized reads, out-of-range registers, and active
pattern state all fall back to the existing generic helpers. Generic `write_reg`
also skips pattern-state erases when all pattern maps are empty.

Fresh-cache results after the VM integer-register fast path on 2026-06-02
(Darwin arm64):

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted      3     0.2529     0.2514          3.6    715609516598740
amber-built-fresh      3     0.2578     0.2568          2.3    715609516598740
python                 3     0.1132     0.1126         14.7    715609516598740
ruby                   3     0.0710     0.0705         15.6    715609516598740
cpp                    3     0.0036     0.0035          1.4    715609516598740
```

The next VM optimization keeps the serialized `.amberbc` format unchanged and
builds a VM-local quickened instruction array per `BcCode`. Fixed-arity hot
opcodes read decoded `a/b/c/imm` fields instead of
`std::vector<InstructionOperand>` on every dispatch. The VM also fuses
`ILT`/`IGT`/`ILTK`/`IGTK` followed by `JUMP_IF_FALSE` when a conservative CFG
walk proves the compare result register is dead and not present in
`local_layout`, preserving debug/completed locals.

Fresh-cache results after VM quickening and fused compare-branch on 2026-06-03
(Darwin arm64):

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted      5     0.2113     0.1353          3.7    715609516598740
amber-built-fresh      5     0.1372     0.1347          2.3    715609516598740
python                 5     0.1206     0.1179         14.7    715609516598740
ruby                   5     0.0793     0.0749         15.9    715609516598740
cpp                    5     0.0044     0.0042          1.4    715609516598740
```

The next VM optimization keeps the serialized `.amberbc` format unchanged and
adds a VM-frame integer sidecar (`int64_regs` plus validity bits). `LOADK`
integer constants and integer args seed the sidecar, `IADD`/`ISUB` write
unboxed integer results directly, and integer compare/fused-branch paths read
the sidecar first. Generic `Value` registers are materialized at boundaries
such as `read_reg`, `Return`, completed locals, watch storage, and GC root
collection; generic writes invalidate the sidecar.

Fresh-cache results after the integer sidecar on 2026-06-03 (Darwin arm64):

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted      5     0.1269     0.1253          3.7    715609516598740
amber-built-fresh      5     0.1273     0.1266          2.3    715609516598740
python                 5     0.1179     0.1153         14.7    715609516598740
ruby                   5     0.0735     0.0728         16.0    715609516598740
cpp                    5     0.0040     0.0039          1.4    715609516598740
```

The latest `amber-built-fresh` row used a fresh bytecode build:

```sh
build/amberc build bench/polyglot/amber/amber.build.json \
  --out-dir /private/tmp/amber_sidecar_bench_rebuilt_ly31k6ae/amber/out \
  --cache-dir /private/tmp/amber_sidecar_bench_rebuilt_ly31k6ae/amber/cache
/private/tmp/amber_sidecar_bench_rebuilt_ly31k6ae/amberbc_run \
  /private/tmp/amber_sidecar_bench_rebuilt_ly31k6ae/amber/out/bench.polyglot.amberbc \
  main
```

The first measurement pass against that freshly built artifact had cold-process
outliers (`amber-interpreted` first sample `0.4596s`, `amber-built-fresh`
first sample `0.3544s`, and C++ first sample `0.1843s`). The table above is
the immediate stable rerun against the same fresh `.amberbc` artifact.
For compiler/emitter changes, continue to use a fresh `--out-dir` and
`--cache-dir`, or clear the generated benchmark cache deliberately. The cache
key is source-hash based and can reuse stale bytecode after compiler/emitter
changes.

The `calls-collections` workload adds helper-function calls and collection
traffic: nested list literals, `[]`, `first`, `count`, list arguments, and
helper loops over collections. Its Amber bytecode path runs module init through
`amberbc_run ... __init__`, because the compiled `main` closure captures helper
closures created by module initialization.

Fresh-cache results for `calls-collections` on 2026-06-03 (Darwin arm64):

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted      5     0.1455     0.1424          5.2         2047795430
amber-built            5     0.1441     0.1398          3.3         2047795430
python                 5     0.0196     0.0193         15.1         2047795430
ruby                   5     0.0298     0.0294         16.0         2047795430
cpp                    5     0.0024     0.0023          1.3         2047795430
```

That table is the immediate stable rerun against fresh artifacts built with:

```sh
python3 bench/polyglot/run_benchmark.py \
  --workload calls-collections \
  --repeats 5 \
  --build-dir /private/tmp/amber_polyglot_calls_results
python3 bench/polyglot/run_benchmark.py \
  --workload calls-collections \
  --repeats 5 \
  --no-build \
  --build-dir /private/tmp/amber_polyglot_calls_results
```

Latest local rerun after adding the captured-closure ABI and native list fast
path on 2026-06-04 (Darwin arm64, `go version go1.26.4 darwin/arm64`). The
runner prepares an `amberc build <file.am>` native executable for each workload
and now rejects builds whose selected entry is not native or whose native code
count does not cover every bytecode code object. It passes `--entry main-only`
for the arithmetic workload and `--entry init` for `calls-collections`.

The measured tables below are stable `--no-build` reruns against fresh native
executables and language binaries built immediately before the rerun. All
measured checksums matched the expected checksum.

Arithmetic:

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted     10     0.1557     0.1409          3.7    715609516598740
amber-built           10     0.0042     0.0040          1.5    715609516598740
python                10     0.1170     0.1152         14.7    715609516598740
ruby                  10     0.0734     0.0723         15.9    715609516598740
cpp                   10     0.0037     0.0036          1.4    715609516598740
go                    10     0.0058     0.0056          4.1    715609516598740
```

Calls and collections:

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted     10     0.0099     0.0096          5.1         2047795430
amber-built           10     0.0032     0.0030          1.5         2047795430
python                10     0.0185     0.0173         15.1         2047795430
ruby                  10     0.0280     0.0265         16.0         2047795430
cpp                   10     0.0020     0.0019          1.3         2047795430
go                    10     0.0025     0.0024          4.0         2047795430
```

SHA digest:

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted     10     0.1341     0.1314          6.2         2242493101
amber-built           10     0.0043     0.0042          2.1         2242493101
python                10     0.0778     0.0768         15.3         2242493101
ruby                  10     0.0521     0.0515         16.5         2242493101
cpp                   10     0.0024     0.0023          1.3         2242493101
go                    10     0.0032     0.0029          4.0         2242493101
```

The arithmetic executable reports full native coverage for 2/2 code objects.
The `calls-collections` executable reports full native coverage for 10/10 code
objects: module-init closures use shared native capture cells, closure calls
stay in generated C++, and list literals plus `[]`, `count`, and `first` use
the direct list path. Its `amber-built` mean improved from the previous
`0.0101s` fallback result to `0.0032s`, about 3.2x faster and close to the Go
result. Stack-backed native register frames also improved arithmetic from the
previous `0.0051s` mean to `0.0042s`.

The `sha-digest` executable reports full native coverage for 9/9 code objects.
The workload exercises the SHA-256 compression schedule, list index assignment,
integer `&`, `|`, `^`, `<<`, and `>>`. The Amber source keeps the hot 32-bit
rotate/mix formulas inline so the native path does not pay closure-call ABI
cost for tiny bitwise helpers; 32-bit wrap uses `& 0xffffffff` instead of
division-based modulo.

Commands used:

```sh
python3 bench/polyglot/run_benchmark.py \
  --workload arithmetic \
  --repeats 10 \
  --build-dir /private/tmp/amber_polyglot_native_closure_lists_arith
python3 bench/polyglot/run_benchmark.py \
  --workload arithmetic \
  --repeats 10 \
  --no-build \
  --build-dir /private/tmp/amber_polyglot_native_closure_lists_arith
python3 bench/polyglot/run_benchmark.py \
  --workload calls-collections \
  --repeats 10 \
  --build-dir /private/tmp/amber_polyglot_native_closure_lists_calls_final
python3 bench/polyglot/run_benchmark.py \
  --workload calls-collections \
  --repeats 10 \
  --no-build \
  --build-dir /private/tmp/amber_polyglot_native_closure_lists_calls_final
python3 bench/polyglot/run_benchmark.py \
  --workload sha-digest \
  --repeats 10 \
  --build-dir /private/tmp/amber_polyglot_sha_digest_final
python3 bench/polyglot/run_benchmark.py \
  --workload sha-digest \
  --repeats 10 \
  --no-build \
  --build-dir /private/tmp/amber_polyglot_sha_digest_final
```

## Numeric profile v1: checked Int arithmetic cost (2026-06-12)

amber.numeric-profile.v1 landed checked Int64 arithmetic as the default
profile (`__builtin_*_overflow` + policy resolution in every VM int path,
checked helpers + `NativeBailout` on overflow in the native lane). Same-day
same-machine A/B of the arithmetic workload, 10 repeats, against a clean
`HEAD` worktree without the numeric changes:

```text
lane                       baseline best   checked best   delta
amber-interpreted                 0.2984         0.3070    ~+3%
amber-built (native)              0.0046         0.0056   ~+0.001s (noise-level)
```

Absolute numbers are not comparable with the earlier tables in this journal
(different machine power/thermal state — the same-day baseline rerun of the
unchanged HEAD measured 0.2984s where the old table recorded 0.1409s). The
delta column is the meaningful result: overflow checking costs ~3% in the
interpreter, in line with the design expectation that interpreter dispatch
dominates. Non-default profiles (wrapping/saturating/narrow widths) run
VM-only; the native lane keeps default-profile semantics via bailout-restart.

## Phase 2 interpreter tuning (2026-06-12)

Same-day A/B against the pre-change HEAD build, best of 10 direct
`build/iamber --eval-file` runs per workload (Darwin arm64). The serialized
`.amberbc` format is unchanged; all changes are VM-internal. Conformance
corpus (78), full `make test`, and `make backend-equivalence` (22) pass at
every step.

```text
workload            baseline   step1    step2    final    speedup
---------------------------------------------------------------------
arithmetic            0.3127   0.1621   0.1611   0.1549     2.0x
calls-collections     0.0145   0.0107   0.0110   0.0104     1.4x
sha-digest            0.1509   0.0880   0.0717   0.0715     2.1x
blocks micro (below)  3.50     —        —        0.54       6.5x
```

Step 1 — on-demand text source locations. `execute()` built a
`RuntimeTextSourceLocationScope` around every `step()`: a frame walk, a
source-span search, a file-string copy, and a TLS round trip per
instruction, only so text output events could be attributed to module
statements. Sampling profiles attributed ~25-35% of interpreter time to it
on both arithmetic and sha-digest. Output attribution now resolves through a
TLS provider installed once per `execute()` loop; text writers call
`resolve_runtime_text_source_location()` at output-event time and get the
same module-statement attribution.

Step 2 — quickened list `[]=` + allocation-free selector aliasing. Send
quickening previously stopped at one positional argument, so the sha-digest
inner loops (`w[i] = ...`) paid the full generic send path: operand vector
decode, args vector build, and ~24 selector string compares per store.
`SendSeqIndexSet` now handles two-argument `[]=` sends on lists with the
same fault classification as the generic handler (non-list receivers,
frozen lists, and non-integer indexes still fall back).
`canonical_collection_selector` returns references to static strings
instead of allocating per call.

Step 3 — block-call path. Builtin collection blocks (`each`/`map`/...)
constructed a fresh nested `Vm` per block invocation, which deep-copied the
entire `BcModule` (strings, symbols, const pool, all code objects),
re-quickened the block code, and threw both away after one element. Block
invocations now lease a pooled child Vm with append-only string/symbol
table sync at the lease boundaries in both directions. The sync also fixes
a correctness bug: strings interned inside a block escaped as
`<invalid-string>` because the nested module copy was dropped (pinned in
`corpus/run/block_string_intern_escape`). On top of that: per-frame pattern
state (`prepared_seq_regs`/`prepared_map_regs`/`pending_pattern_bindings`)
moved from `unordered_map` to flat vectors, so block-parameter prologues
stop paying hash-node malloc/free per invocation; block args pass as
`initializer_list` instead of heap vectors; per-opcode safepoints check
atomic mirrors of the remote-free queue depth and pending-GC flag before
touching the heap mutex; pooled block Vms skip the completed-frame register
snapshot that `finish_return` captures for `execute()` consumers.

The block microbench (not part of the polyglot suite; kept here for
reproducibility — 200k iterations x 8-element `each` = 1.6M block calls):

```amber
def main():
  data = [1, 2, 3, 4, 5, 6, 7, 8]
  total = 0
  i = 0
  while i < 200000:
    sum = 0
    data.each |x|:
      sum = sum + x
    total = total + sum
    i = i + 1
  total
```

Remaining known hot spots after this pass (sampled on sha-digest and the
block micro): interpreter dispatch in `step()`, the integer-sidecar
read/write helpers, and `Frame` move/destroy traffic in the call path. The
first two are the Phase 4 value-representation work; the frame moves need a
`finish_return` restructure that did not pay for its risk in this pass.

Step 4 — ivar sites (research plan §5.4), same-day follow-up. An
ivar-heavy micro (300k method calls, 8 ivar accesses each: a `Counter`
class whose `tick(i)` mutates `@a`/`@b`/`@total`) measured 0.397s best.
Profiling showed the cost was strings, not slots: every `LoadIvar`/
`StoreIvar` copied the ivar name out of the symbol table
(`optional<string>` per access), `StoreIvar` probed the inline cache and
then discarded the hit (`(void)cached_slot`) before re-resolving the slot
by name three times, and `try_apply_scalar_send` evaluated its
sequence/numeric selector-set memberships (~70 string compares)
unconditionally for every send — including plain user method calls.
`LoadIvar`/`StoreIvar` are now quickened with cache-hit fast paths
(pre-decoded operands, no name copy, store uses the cached slot and keeps
the legacy string-keyed `ivars` mirror in sync because GC tracing and
legacy lookups read it); watched objects, lifecycle faults, and cache
misses fall back to the generic handlers. The selector-set tests are gated
by receiver kind. Ivar micro: 0.397s -> 0.286s (-28%); the three polyglot
workloads and the block micro are unchanged (within noise). Remaining ivar
cost is the per-call method-send path (`call_caches` probe + generic
operand decode) and the global `ivar_caches` hash probe per access —
inline ICs in the quickened stream would be the next step but need
mutable per-site state, which the shared QuickCode does not have today.

## §5.8 emitter parameter int-speculation: attempted, reverted (2026-06-13)

Tried marking `param`/`block_param`/`implicit_block_param` slots as
speculative integer candidates in `analyze_integer_locals`
(`bytecode/emitter.cpp`), so parameter arithmetic emits `I*`/`I*K` opcodes
instead of generic `SEND`. The speculation is sound — every integer opcode
deopts to the full send path when an operand is not an `Int` at runtime, so
it only chooses opcodes, never semantics — and it required first fixing the
`I*K` constant-operand deopt to rebuild a full synthetic `Send` like the
register-operand deopt already does.

Same-day same-machine A/B, best of 8 after 2 warmups (Darwin arm64):

```text
workload            no-spec   with-spec   delta
---------------------------------------------------------
arithmetic           0.1529     0.1467     -4%   (noise band)
calls-collections    0.0109     0.0403    +270%  REGRESSION
sha-digest           0.0705     0.0691     ~0
params (micro)       0.0964     0.0891     -8%   (real, modest)
blocks (micro)       0.4993     0.4832     -3%
```

Reverted: a 3.7x regression on a shipped benchmark is not worth an 8% win on
an integer-parameter-leaf micro.

Root cause — `classify_direct_closure` (`runtime/vm.cpp`) holds hand-written
recognizers (`SequenceIndex`/`LaneIndex`/`WrapSubtract`/`MixLinearWrap`/
`ScoreRow`) that match the **exact `SEND`-shaped bytecode** of this
benchmark's leaf functions (`pick`/`lane_index`/`wrap`/`mix`/`score_row`)
and evaluate them inline with no frame push. Speculation rewrites those
`SEND`s to `IDIV`/`IMUL`/`ISUB`/`IGT`/`IADD(K)`, so 3 of the 5 recognizers
(`LaneIndex`, `WrapSubtract`, `MixLinearWrap`) stop matching and those
functions fall back to full frame pushes — the regression is call overhead,
not the integer ops. (`SequenceIndex`/`pick` and `ScoreRow` still match: `[]`
stays `SEND` and `score_row`'s arithmetic is on non-candidate locals.)

To unblock, in order:
1. Land the `I*K` constant-operand deopt fix (route through a full synthetic
   `Send` via a scratch register, matching the register-operand deopt). Today
   that path is unreachable — only proven-integer locals get `I*K` and their
   operands are always `Int` — so it cannot be landed or tested on its own;
   it must land with the speculation that makes it reachable.
2. Make the recognizers opcode-agnostic. A binary-op "view" that
   canonicalizes both a 1-arg binary `SEND` and the `I*`/`I*K` forms to
   `(selector, dst, lhs, rhs[, const])` lets `LaneIndex`/`WrapSubtract` match
   either shape unchanged (same instruction count, reg-reg ops). `MixLinearWrap`
   genuinely restructures (`LoadK`+`SEND` collapses to `IADDK`/`IMULK`, fewer
   instructions) and needs a second matcher.
   Better still: retire these benchmark-specific recognizers in favor of a
   general frameless-leaf-call path that consumes specialized bytecode.

## §6.1 heap allocator flag + fragmentation measurement (2026-06-13)

Layer-1 of `RESEARCH-heap-fragmentation-allocators-2026-06-12.md` §10 ("Now
(days)"): make the allocator swappable, instrument RSS vs. live bytes, add a
churn benchmark, and run the §9 measurement. Layers 2–3 (string-table
lifecycle, intrusive refcounts, slab pools, moving young gen) are unchanged and
remain the larger follow-on phases.

Landed:
- **`MALLOC=system|mimalloc|jemalloc` Makefile flag.** Sets the `AMBER_ALLOCATOR`
  macro; for mimalloc/jemalloc it discovers the Homebrew prefix and, on macOS,
  `-Wl,-force_load`s the static archive so the malloc-zone override actually
  wins (plain `-l` does not interpose on Darwin). Default `system` build and
  flags are unchanged.
- **`RuntimeHeapStats.live_object_bytes` / `tracked_object_bytes`**, summed over
  `objects_` in `stats()` (allocation_size was already recorded per object).
  Shell bytes only — a cheap proxy, not a payload-exact live total.
- **`AMBER_HEAP_STATS=1` dump** in the runner: one stderr line with allocator,
  current + peak RSS (mach `task_info` / `getrusage`), live/tracked bytes, and
  the RSS / live-bytes ratio. Off by default; never touches stdout.
- **`bench/heap/churn.am`**: a large (RING_CAP=40000) simultaneous live set of
  mixed-size graphs with mixed lifetimes, plus a distinct-interned-string loop.
  A small ring frees too promptly to show anything; the large ring builds a
  real fragmented heap then drops it.

Same-machine sweep, churn.am, Darwin arm64 (mimalloc 3.3.2, jemalloc 5.3.0):

```text
allocator   config                          peak RSS   end RSS   returned
-----------------------------------------------------------------------------
system      macOS libmalloc                  ~97 MB     ~97 MB    ~0%
mimalloc    default / PURGE_DELAY=0          106 MB     106 MB    ~0%  (see note)
jemalloc    default                           95 MB      27 MB    ~72%
jemalloc    dirty/muzzy_decay_ms:0            95 MB    10.8 MB    ~89%  (≈baseline)
```

Output value `1411573421` is identical under all three — correctness preserved.

Findings:
1. **Peak RSS is allocator-independent (~95–106 MB).** It is set by the VM's
   4-malloc-per-object pattern over a large simultaneous live set; no allocator
   lowers it. Only §7 structural fixes (intrusive refcounts, slabs) cut peak.
2. **Page return is the allocator-sensitive symptom, and jemalloc wins on
   macOS**: 95 MB → 10.8 MB with aggressive decay. mimalloc's reclaim is real
   but uses `madvise(MADV_FREE)`, which marks pages reclaimable-under-pressure
   without dropping `resident_size` on Darwin — so its win is invisible to RSS
   here (it would show on Linux / via `vmmap` dirty-vs-clean). This is the doc's
   "macOS is second-tier, measure with vmmap" caveat (§4, §9) made concrete, and
   it nuances the "mimalloc default" pick: on macOS the *measurable* RSS win is
   jemalloc+decay; the mimalloc-vs-jemalloc default should be settled on Linux.
3. **Live heap is < 0.5 MB at a ~95 MB peak.** `live_object_bytes` ≈ 1.2 KB and
   jemalloc's at-exit `Allocated` is 474 KB; `gc_cycles=0` (refcounting freed
   all 2.4M objects promptly, GC never ran). RSS is overhead + page retention,
   not live data — exactly the doc's §3 thesis. The RSS/live-bytes ratio at
   end-of-run is therefore dominated by fixed baseline; **peak + post-churn end
   RSS across flavors, and jemalloc `stats_print`, are the informative numbers.**
4. **4-malloc/object spot-check (§9):** jemalloc cumulative `nrequests` =
   16,521,121 for the churn vs 642 for a trivial program ≈ 6.9 malloc calls per
   VM heap object. That is *above* the documented 4 (the remainder is per-iter
   interpreter temporaries + the string loop), consistent with — and a lower
   bound on — the 4-shell-allocations claim. Exact isolation needs a counter
   inside `allocate<T>`; deferred.

Net: the swap is worth keeping as a flag (jemalloc+decay is a real, free RSS win
on this workload), but it treats the symptom — peak is unchanged and the live
set is tiny, so the §7.1 string-table lifecycle and §7.2 intrusive-refcount work
remain the higher-impact levers. Run the sweep with `bench/heap/README.md`.

## §6.2 Layer-2a: O(1) string/symbol interning (2026-06-13)

First half of RESEARCH §7.1: the `intern_runtime_string` /
`intern_runtime_symbol` *time* blowup. Both did a linear scan of the whole
table per intern (`runtime/vm.cpp`), so string/symbol-heavy interpretation was
O(n²) — and `symbol_id_for_text` is also on the per-send selector-cache path
(`try_apply_scalar_send`), so every method send paid an O(symbols) scan.

Replaced the scans with incremental hash indices (`string_index_` /
`symbol_index_`, `text → id`) on `Vm`. The tables only ever grow (never
clear/shrink) and are occasionally appended to outside intern (nested-module
splicing, `vm.cpp:9669`/`9676`), so the index is folded lazily against a
watermark (`fold_string_index`/`fold_symbol_index`): a steady-state intern is a
single `find`; folding is O(new slots) and runs only after an external append.
`emplace` keeps the first id for a text, so ids and dedup semantics are byte-for-
byte unchanged (corpus + vm_tests green, churn value identical).

A/B on a distinct-string micro-bench (`bench/heap/intern_scaling.am`, 30 000
rounds ≈ 90 000 distinct strings), same machine:

```text
build                       time (30k rounds)
old (linear intern scan)        17.2 s
new (hash index)                 0.13 s     ~125× faster
```

This fixes the *quadratic time*; it does **not** bound table growth — the table
is still immortal (every distinct string is a permanent slot). Bounding the
*space* is Layer-2b, designed separately in
`DESIGN-string-table-lifecycle-2026-06-13.md` (it needs a Value-representation
change or GC-integrated string liveness, both larger and riskier than this).

## §6.3 Layer-2b Option D: make string-table growth observable (2026-06-13)

The space fix (DESIGN-string-table-lifecycle §4 Options A/B) is gated on the
§7.2 intrusive-refcount work and is not attempted yet. Its cheap, safe "now"
step (Option D) is landed: `AMBER_HEAP_STATS` now also reports
`runtime_string_count` / `runtime_string_bytes` — the slots interned past the
module's compile-time string count, i.e. the unbounded region. Computed
runner-side from `ExecutionResult.runtime_strings` (`tools/amberc/main.cpp`); no
VM API or `RuntimeHeapStats` change, stderr-only, off by default.

```text
workload                 runtime_string_count   runtime_string_bytes
intern_scaling (30k)            149 897                 1 267 804
churn.am                         39 369                   334 456
run_script fixture                    0                         0
```

Turns "the table leaks" into a number, closes RESEARCH §9's tracking for the
string table, and gives a before/after handle for the eventual Layer-2b fix.

## §7.2 intrusive refcount in ObjHeader (2026-06-13)

RESEARCH §7.2: every heap object paid a second malloc beyond its shell, because
`allocate<T>` wrapped it in `std::shared_ptr<T>(raw, deleter)` whose fat deleter
(captures `{shared_ptr<Impl>, worker, kind, allocation_id}`) forced a separate
~80 B control-block allocation. Moved the strong count into the `ObjHeader`
(which exists on every object already) and switched the six ObjHeader-bearing
kinds (Closure/Instance/List/Tuple/Set/Map) from `shared_ptr<T>` to an
`IntrusivePtr<T>`. Design notes in `DESIGN-intrusive-refcount-2026-06-13.md`.

- `IntrusivePtr<T>` is a single 8-byte pointer. All element-touching logic
  (`runtime_heap_add_ref`/`runtime_heap_release`) is out-of-line — declared in
  vm.h, defined + explicitly instantiated for the six kinds in vm.cpp — so it
  works with incomplete element types at the ~270 `.as_X()` consumer sites, just
  like `shared_ptr`. The count is atomic; drop-to-zero reuses `Impl::release`
  unchanged (same physical-free / cross-strand-queue path).
- The header keeps a type-erased `shared_ptr<void> heap` keepalive set in
  `allocate`. It both locates the heap on the drop path and guarantees the heap
  outlives its objects — the lifetime contract the old deleter's `shared_ptr<Impl>`
  capture provided. (First cut used a raw `Impl*` and crashed at teardown with
  `mutex lock failed` when an object outlived the heap; the keepalive, released
  in `runtime_heap_release` *outside* any Impl method, fixes it.)
- White-box tests that built objects via `make_shared` now use `make_intrusive`
  (unmanaged: `ref_count=1`, `heap=null`, plain-deleted on drop).

Malloc-count A/B (jemalloc cumulative `nrequests`, churn.am, 2,400,009 VM
objects, same machine):

```text
                       nrequests       per object
pre-§7.2 (shared_ptr)   16,521,121         6.88
post-§7.2 (intrusive)   14,201,092         5.92     -2,320,029  (~ -1 / object)
```

Exactly one fewer malloc per heap object — the control block is gone. ObjHeader
grows 24 B (atomic + keepalive) but a ~80 B per-object allocation disappears, so
each object is net smaller and costs one fewer malloc. `sizeof(Value)` is still
24 B: the variant still holds `shared_ptr` for ~18 non-ObjHeader types (BigInt,
error instances, the Runtime* objects), so the doc's 24→16 shrink needs that
tail converted too — deferred to a follow-up phase. Full `make test` green
(all unit suites + ambertest 81/0).

### Value 24→16 tail conversion: investigated, deferred (2026-06-13)

Attempted next; backed out without committing. Converting the ~15 non-ObjHeader
`shared_ptr` tail alternatives to 8-byte intrusive pointers (the only way to drop
the variant's largest member to 8 B) hits a wall at `shared_ptr<RuntimeIoValue>`:
`RuntimeIoValue` is an abstract polymorphic base (5 derived kinds, ~77 shared_ptr
uses, 33 Value entry points) owned entirely by the io subsystem, which does not
even include vm.h. One unconverted alternative keeps `Value` at 24, so it is
all-or-nothing. Paths: box each alternative behind an 8-byte RcPtr<TailBox<T>>
(contained, but +1 alloc per tail value); full intrusive incl. an io-subsystem
rewrite (large/risky); or defer. Deferred by decision in favor of a smaller,
safer win; the RcPtr/RefCounted machinery was drafted and reverted.

## §7.5 pressure-driven remote-free drains (2026-06-13)

Cross-strand frees are deferred to the owning worker's `remote_frees` queue and
only physically reclaimed when that worker drains — which previously happened
only at four fixed interpreter points and at heap teardown, so an
actively-allocating strand's dead objects could pile up between drains. Added a
drain under the worker's own allocation pressure: `allocate` calls
`maybe_drain_on_pressure`, which every `kRemoteDrainAllocInterval` (256)
allocations drains the current worker's queue. `drain_remote_frees` already has a
lock-free `remote_free_pending_` early-out, so for single-strand workloads (no
cross-strand frees) this is a thread-local increment plus an atomic load every
256 allocations — churn.am time and value are unchanged (`remote_frees_queued=0`
there, so the path is a no-op). Correct-by-construction (it only drains more
often; nesting is safe — `RuntimeWorkerScope` is thread-local save/restore);
validated by the full suite including the cross-strand `stdlib_task_tests`. A
dedicated concurrent bench to quantify the queue-depth reduction is a follow-up.

## Phase 4: value-representation prototype A/B (2026-06-16)

PLAN Phase 4 ("value-representation prototype"): replace the 28-alternative
`std::variant` `Value` (24 B) with a compact tagged union (16 B), behind a
build flag, and A/B it against the variant baseline before committing to a full
migration. Both representations are now selectable with the `VALUE_REPR` Makefile
flag (`variant` default, `tagged` defines `-DAMBER_VALUE_REPR_TAGGED`), mirroring
`MALLOC=`.

Design (tagged): a 1-byte `ValueTag` + an 8-byte union (`static_assert(sizeof ==
16)`). Immediates (null/bool/int/float/symbol-id/string-id/class-id/native-*)
and the six `ObjHeader` heap kinds (closure/instance/list/tuple/set/map, stored
as the inline header pointer) live in the union; the ~15 cold tail kinds
(BigInt/error/task/channel/io/...) are boxed behind a refcounted `ValueTailBox`
(one extra allocation + indirection per tail value, all cold paths). Copy/move/
destroy manage the intrusive `ObjHeader` refcount and the box refcount manually;
heap-kind release recovers the concrete type from `ObjHeader::kind` and reuses
the existing typed `runtime_heap_release` (all RuntimeHeap bookkeeping intact).
The `Value::` public API (factories, `is_X`/`as_X`) is byte-for-byte identical
across reps; only storage + method bodies differ. The three former
direct-variant call sites now go through `kind_index()` / `integer_if()`. The
native lane propagates the host amberc's rep into the runtime-archive compile
flags (hashed into the archive cache key), so the native backend matches.

The int sidecar (`int64_regs`) is **kept** for this measurement, so the numbers
below are the pure representation delta (smaller copies + cheaper dispatch),
*not* the sidecar-removal win — under `tagged`, integers are inline so the
sidecar becomes redundant; removing it is the next lever (it touches many opcode
paths and is left as a follow-up).

A/B, best-of-8 direct `iamber --eval-file`, interleaved reps, Darwin arm64:

```text
workload          variant     tagged     delta
----------------------------------------------
arithmetic         0.1515     0.1481     -2.3%
sha-digest         0.0719     0.0702     -2.3%
calls-collec       0.0109     0.0106     -2.3%
```

Heap churn (`bench/heap/churn.am`, system allocator, `AMBER_HEAP_STATS=1`):

```text
metric              variant      tagged      delta
--------------------------------------------------
peak RSS            97.76 MB     88.88 MB    -9.1%
end RSS             92.98 MB     85.70 MB    -7.8%
live_object_bytes      1344         1288     smaller shells
allocations        2,400,013    2,400,013   identical
best time             1.827s       1.672s    -8.5%
```

Correctness (both reps): `ambertest run corpus` 120/120, `backend-equivalence`
61/61 byte-identical, and the Value-heavy C++ suites (`vm_tests`,
`stdlib_collections_tests`, `stdlib_task_tests`) green. The tagged
`backend-equivalence` runs the VM lane *and* the native lane under the tagged
rep (both byte-identical), and the tail kinds are exercised by the task suite
(channel/mutex/atomic/barrier) and BigInt/error paths.

**Verdict — clear win, no regressions: proceed to the full migration.** A
uniform ~2.3% interpreter speedup *even with the sidecar still shielding the hot
integer path*, ~8.5% faster + ~9% lower peak RSS on collection-heavy churn, a
33% smaller `Value` (24→16 B, so every register file / list / tuple / map / ivar
vector is smaller and more cache-friendly), and zero correctness regressions
across corpus + backend-equivalence + unit suites. The gate in PLAN §1.6 is met.
Next levers (follow-ups), in order: (1) remove the now-redundant int sidecar
under `tagged` and re-measure arithmetic; (2) make `tagged` the default and
delete the variant branch once it has soaked; (3) the `heap_header_from_value`
fast path can become O(1) under `tagged` (return the inline `obj` pointer
instead of the `is_X` cascade).
