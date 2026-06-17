# VM microbenchmarks

Small interpreted-VM benchmark programs for focused hot paths. Run them with:

```sh
build/iamber --eval-file bench/vm/<file>.am
```

The JSONL streaming microbenches read `bench/polyglot/build/json/events.jsonl`.
Prepare that fixture with:

```sh
python3 bench/polyglot/run_benchmark.py --workload json --repeats 1
```

