# Polyglot benchmark

This benchmark compares identical workloads across:

- Amber interpreted: `build/iamber --eval-file`
- Amber built native executable: `amberc build <file.am>` first, then the
  generated host binary
- Python
- Ruby
- C++ compiled with `-O2`

The Amber built path intentionally runs an already generated executable, so
compile time is not included in the measured run. The runner still builds fresh
`.amberbc` artifacts as a bytecode sanity check, but the `amber-built` row is
the native executable from `amberc build`, not `amberbc_run`.

Run:

```sh
python3 bench/polyglot/run_benchmark.py --repeats 3
python3 bench/polyglot/run_benchmark.py --workload calls-collections --repeats 5
```

The script prints mean/best wall-clock time and peak RSS reported by a small
Python measurement helper via `resource.getrusage(RUSAGE_CHILDREN)`. It also
validates that every implementation returns the same checksum for the selected
workload:

- `arithmetic`: `715609516598740`
- `calls-collections`: `2047795430`

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

The arithmetic executable reports full native coverage for 2/2 code objects.
The `calls-collections` executable reports full native coverage for 10/10 code
objects: module-init closures use shared native capture cells, closure calls
stay in generated C++, and list literals plus `[]`, `count`, and `first` use
the direct list path. Its `amber-built` mean improved from the previous
`0.0101s` fallback result to `0.0032s`, about 3.2x faster and close to the Go
result. Stack-backed native register frames also improved arithmetic from the
previous `0.0051s` mean to `0.0042s`.

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
```
