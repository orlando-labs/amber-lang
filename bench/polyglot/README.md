# Polyglot benchmark

This benchmark compares one identical integer workload across:

- Amber interpreted: `build/iamber --eval-file`
- Amber built bytecode: `amberc build` first, then `amberbc_run <file.amberbc>`
- Python
- Ruby
- C++ compiled with `-O2`

The Amber built path intentionally runs an already generated `.amberbc` artifact
through a small VM runner, so compile time is not included in the measured run.

Run:

```sh
python3 bench/polyglot/run_benchmark.py --repeats 3
```

The script prints mean/best wall-clock time and peak RSS reported by a small
Python measurement helper via `resource.getrusage(RUSAGE_CHILDREN)`. It also
validates that every implementation returns the same checksum:
`715609516598740`.

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

The required `python3 bench/polyglot/run_benchmark.py --repeats 3` run in the
existing generated benchmark directory reported `amber-built` at 2.1337s mean,
but that artifact was stale: disassembling the generated
`bench/polyglot/build/amber/out/bench.polyglot.amberbc` showed 17 generic
`SEND` instructions and zero integer opcodes. Use a fresh `--out-dir` and
`--cache-dir`, or clear the generated benchmark cache deliberately, for trusted
`amber-built` numbers after compiler or emitter changes.
