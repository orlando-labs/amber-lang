# Heap churn benchmark

`churn.am` is the §9 "bench scenario" from
`RESEARCH-heap-fragmentation-allocators-2026-06-12.md`: it drives the two
distinct RSS-retention mechanisms the research separates so they can be measured
independently.

- **`churn_graphs`** — a high allocation rate of small, *mixed-size* object
  graphs (instance + two list sizes + map + set + tuple) with *mixed lifetimes*:
  a large fixed-capacity ring (`RING_CAP`) keeps a big set live simultaneously
  while every other graph dies almost immediately. This is the external-
  fragmentation generator (§3.1) and the part an allocator swap targets.
- **`churn_strings`** — produces many *distinct* interned strings. The runtime
  string table is immortal and unbounded (§3.3 / §7.1), so this grows memory in
  a way no allocator swap can fix; it is the control that proves the point.

The ring is large on purpose: a small ring frees too promptly (refcounting is
eager here, GC never even runs) to leave the allocator holding a fragmented
heap, which would hide the cross-allocator difference.

## Run it

```sh
# Baseline (system allocator). Off by default; set AMBER_HEAP_STATS for the line.
make build/amberc
AMBER_HEAP_STATS=1 ./build/amberc bench/heap/churn.am
```

The stderr line:

```
[amber-heap] allocator=system rss_bytes=… peak_rss_bytes=… live_object_bytes=… \
  tracked_object_bytes=… live_objects=… allocations=… local_frees=… \
  remote_frees_queued=… remote_queue_depth=… gc_cycles=… gc_reclaimed_objects=… \
  frag_ratio_live=… frag_ratio_tracked=…
```

- `peak_rss_bytes` — kernel high-water (`getrusage`); the most comparable number
  across allocators, captured regardless of when objects freed.
- `rss_bytes` — current RSS at end of `main()`. How much each allocator still
  holds *after* the churn freed everything = the "freed memory not returned to
  the OS" symptom.
- `live_object_bytes` / `tracked_object_bytes` — sum of object *shell* sizes
  (`sizeof(T)`) over live / all tracked records. A proxy; excludes payloads.
- `frag_ratio_*` — RSS / live-bytes. Informative only with a large retained live
  set; here the live set is ~0 at end-of-run, so prefer peak vs. end RSS.

## Allocator sweep

```sh
brew install mimalloc jemalloc

for m in system mimalloc jemalloc; do
  rm -f build/amberc && make build/amberc MALLOC=$m
  AMBER_HEAP_STATS=1 ./build/amberc bench/heap/churn.am 2>&1 >/dev/null
done

# page-return knobs (RESEARCH §10 step 1):
MIMALLOC_PURGE_DELAY=0 AMBER_HEAP_STATS=1 ./build/amberc bench/heap/churn.am  # mimalloc
MALLOC_CONF="dirty_decay_ms:0,muzzy_decay_ms:0" \
  AMBER_HEAP_STATS=1 ./build/amberc bench/heap/churn.am                       # jemalloc

# definitive allocator-level split (jemalloc): stats.resident − stats.allocated
MALLOC_CONF="stats_print:true" ./build/amberc bench/heap/churn.am 2>&1 >/dev/null
# confirm the override is live (mimalloc):
MIMALLOC_VERBOSE=1 ./build/amberc bench/heap/churn.am 2>&1 | head
```

## macOS caveat

`resident_size` does **not** drop when an allocator reclaims pages via
`madvise(MADV_FREE)` (the pages stay resident until the OS reclaims under
pressure). mimalloc uses `MADV_FREE` on Darwin, so its page return is invisible
to RSS here even though it is real; jemalloc's decay path does drop RSS on macOS.
For a deploy decision, measure on Linux (`smaps_rollup`, jeprof) or with
`vmmap --summary` (dirty vs. clean), not just `resident_size`. See journal
§6.1 in `bench/polyglot/README.md` for the recorded numbers.
