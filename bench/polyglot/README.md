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
