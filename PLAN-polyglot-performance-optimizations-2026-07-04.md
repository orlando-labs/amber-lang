# Polyglot performance optimization plan

Date: 2026-07-04

Context: `bench/polyglot/README.md` was rerun locally with Ruby 4.0.5 from RVM,
Go 1.26.4, Rust 1.96.0, and Python 3.9.6. The reproduced results match the
latest documented run: most Amber rows are in the same performance band, while
`map-words` is the clear outlier and `string-ops` exposes runtime string-table
retention.

## Reproduced findings

- `map-words` dominates the performance gap. Amber interpreted was about 54x
  slower than Ruby and about 191x slower than Rust in the local rerun;
  Amber-built was about 82x slower than C++ and about 60x slower than Rust.
- The VM map path copies `MapValue::entries` through `extract_map_entries` for
  read selectors, then scans linearly for `include?` and `[]`.
- The generated native map path mirrors the same vector-only representation:
  map lookup, containment, and store scan `NativeMap::entries`.
- `string-ops` is primarily a memory-retention signal. String/symbol intern
  lookup is already indexed; the remaining issue is that runtime-created
  strings are still permanent entries in the VM string table.
- Amber-built short workloads carry a fixed startup floor from the native
  executable/runtime footprint. Tiny Amber native binaries are megabytes while
  equivalent C++ benchmark binaries are tens of kilobytes.

## Optimization order

1. Add a canonical-key hash index beside `MapValue::entries` for ordinary
   name-indifferent maps, preserving insertion order while making Symbol/Str
   lookup and upsert O(1). Status: implemented for the VM/runtime on
   2026-07-04.
2. Change VM map read selectors to borrow entries instead of copying the whole
   vector for `count`, `include?`, `[]`, `keys`, `values`, and no-block copy
   selectors. Status: implemented for safe no-block paths on 2026-07-04; block
   iteration still snapshots to preserve mutation semantics.
3. Mirror the indexed map representation in the generated native backend so
   Amber-built no longer inherits vector-scan map traffic. Status: implemented
   for generated `NativeMap` lookup, containment, and mutation on 2026-07-04.
4. Split ephemeral runtime strings from permanent literal/symbol interning so
   string-heavy workloads do not retain every intermediate forever. Status:
   partially implemented on 2026-07-04 by removing the duplicate owned string
   copies from VM/native intern indices; full ephemeral string values remain.
5. Reduce Amber-built startup cost by pruning or lazily initializing unused
   runtime/world components and tightening fallback linkage.

## First implementation target

Start with item 1 in the VM/runtime:

- Keep `MapValue::entries` as the source of truth for order, `keys`, `values`,
  `entries`, display, JSON, and pattern rest materialization.
- Add an index for ordinary Symbol/Str keys keyed by `MapEntry::symbol_id`.
- Rebuild or maintain the index whenever a map is constructed or mutated.
- Use the index for ordinary map `include?`, `[]`, and `[]=` / `store!` hot
  paths, falling back to the existing scan for strict maps and non-nameable
  keys.
- Preserve current semantics: first key representation wins, last assignment
  wins, strict maps keep Symbol and Str distinct, and non-nameable keys compare
  by value.

## First implementation result

After adding `MapValue::name_index` and routing ordinary map lookup/upsert
through it, `bench/polyglot/run_benchmark.py --workload map-words --repeats 10
--no-build --build-dir /private/tmp/amber_polyglot_suite_codex_20260704_01`
reported:

```text
amber-interpreted best_s: 0.1618
checksum: 235174
```

The reproduced pre-change interpreted best was 0.9603 seconds in the same
benchmark directory, so the VM/runtime portion improved by about 5.9x. The
`amber-built` row still uses the old generated native executable until item 3 is
implemented.

## Follow-up implementation results

Item 2 removed the remaining whole-vector copies from safe no-block VM map
selectors. The block-taking selectors (`each`, `map`, `select`, `filter_map`,
`transform*`, and block `count`) still snapshot the entry list so a block can
mutate the receiver without invalidating iteration.

The package macro staging work in `tools/amberc/main.cpp` had introduced a
compile-order error where `run_package_command` called `harvest_macro_exports`
before it was declared. A temporary forward declaration now unblocks
`build/amberc`; the actual macro-driver cleanup is intentionally left for the
later macro fix.

Item 3 added the same ordinary Symbol/Str canonical index to the generated
native `NativeMap` representation and keeps it current for `[]=` / `store`
mutations. A fresh `map-words` build followed by a no-build timing pass reported:

```text
build dir: /private/tmp/amber_polyglot_map_p3_20260704_02
amber-interpreted best_s: 0.1584
amber-built best_s:      0.0507
checksum:                235174
```

Compared with the post-item-2 built target best of 0.2940 seconds, generated
native map indexing improves the Amber-built workload by about 5.8x. The next
remaining drawdowns are fixed native startup/codegen overhead and the
string-table retention issue called out in items 4 and 5.

Item 4 partial pass changed both the VM and generated native string intern
indices from `unordered_map<string, id>` to hash buckets of canonical string
ids. This preserves canonical id/equality semantics and first-match behavior
while avoiding a second owned `std::string` copy for every runtime-created
string. A fresh `string-ops` build followed by a no-build timing pass reported:

```text
build dir: /private/tmp/amber_polyglot_string_p4_20260704_01
amber-interpreted best_s: 0.0429
amber-interpreted peak_rss_mb: 16.9
amber-built best_s:      0.0107
amber-built peak_rss_mb: 11.6
checksum:                280113
```

Compared with the documented 2026-07-04 `string-ops` baseline RSS
(`18.3 MB` interpreted, `12.5 MB` built), this trims about 1.4 MB from the
interpreter and 0.9 MB from the built target. The remaining memory gap still
requires the larger ephemeral-string representation or a precise compaction
scheme for runtime-created strings that are no longer live.

Item 5 investigation: full-native benchmarks still link the runtime archive
because the generated translation unit keeps VM fallback and generic native
helpers available. A conditional "normal archive link" experiment for
full-native/no-extension targets compiled and checksummed correctly, but it did
not improve `map-words` timing or RSS and did not shrink the executable
materially (`~5.64 MB` before and after). That experiment was not kept. The
next credible startup-size optimization is more structural: generate a truly
no-fallback launcher mode, or split the generated native helper/runtime surface
so unused stdlib/helper families are not referenced by the main object file.

Additional map-word passes on 2026-07-04:

- The VM quick-send path now handles ordinary non-strict Map `[]`, `[]=`,
  `count`, and `include?`/aliases for Symbol/Str keys without falling through
  to generic SEND. This targets the remaining post-index overhead in the
  interpreted `map-words` hot loop.
- The VM also quickens integer `to_str` and lets the existing `+` quick-send
  handle `Str + Str` directly. This trims repeated key/probe construction in
  `map-words` without changing bytecode format or generic dispatch semantics.
- The generated native `NativeMap` index now canonicalizes nameable keys by
  canonical string id instead of symbol id. Dynamic Str keys already carry
  canonical string ids, so the map hot path avoids a symbol-table lookup for
  the common Str-key case while preserving Symbol/Str name-indifferent lookup.
- Generated native map lookup/store now treats a nameable-key index miss as
  definitive. Because every generated `NativeMap` nameable key is indexed by
  canonical string id, this removes the fallback vector scan for first inserts
  and absent probes.

Fresh build plus stable no-build timing:

```text
build dir: /private/tmp/amber_polyglot_map_p6_20260704_01

after native string-id map keying:
amber-interpreted best_s: 0.1171
amber-built best_s:      0.0492
checksum:                235174

after VM quick map/String construction paths:
amber-interpreted best_s: 0.1015
amber-built best_s:      0.0492
checksum:                235174

after generated-native definitive index misses:
build dir: /private/tmp/amber_polyglot_map_p7_20260704_01
amber-interpreted best_s: 0.1010
amber-built best_s:      0.0075
confirmation built best: 0.0076
checksum:                235174
```

The interpreted path is now roughly 9.2x faster than the documented
2026-07-04 baseline (`0.9344s -> 0.1010s`) and the built path is roughly 39x
faster (`0.2920s -> 0.0075s`). The built workload is now in the Go/Rust timing
band for this benchmark; the remaining gap to C++ is much smaller and mostly
fixed native executable startup plus general helper dispatch.
