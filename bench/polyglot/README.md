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

First test run:

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted      3     2.2844     2.2363          3.7    715609516598740
amber-built            3     2.2767     2.2587          2.5    715609516598740
python                 3     0.1232     0.1228         14.7    715609516598740
ruby                   3     0.0775     0.0772         16.0    715609516598740
cpp                    3     0.0042     0.0042          1.4    715609516598740
```