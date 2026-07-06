# RESEARCH: Heap fragmentation and RSS retention in the reference VM — jemalloc vs. alternatives

Date: 2026-06-12
Status: research only, no code changes
Scope: `runtime/vm.cpp` RuntimeHeap, allocator selection, page-reuse strategy

## 1. Problem statement

Heavy Amber apps end with very high RSS and heavy fragmentation even though the
GC and refcount deallocation are running. Freed memory is not being returned to
the OS and pages are reused poorly. The question: adopt jemalloc, or is there a
better solution?

Short answer up front: **an allocator swap (mimalloc or jemalloc) is worth
doing and will help, but it is the second-biggest lever, not the first.** The
VM's own allocation pattern manufactures fragmentation faster than any
general-purpose allocator can absorb it, and at least one subsystem (the
runtime string table) retains memory unboundedly in a way no allocator can
fix. The right plan is layered: swap the allocator now (cheap, measurable),
then remove the VM-side fragmentation generators, and keep the existing
generation/pin machinery pointed at a moving young space as the endgame.

## 2. How the VM allocates today (what the code actually does)

All six heap object kinds (Instance, List, Tuple, Set, Map, Closure) go
through one choke point, `RuntimeHeap::Impl::allocate<T>`
(`runtime/vm.cpp:5039`). Per Amber object, the VM performs **four separate
system-malloc allocations in three or more different size classes**:

1. **The object shell** — `new T()` straight to `operator new`
   (`runtime/vm.cpp:5041`). Each shell starts with `ObjHeader`
   (`runtime/vm.h:81`), roughly 100–120 bytes on LP64 (it embeds an
   `OwnerToken`, a `shared_ptr<const ShapeDescriptor>`, and seven 64-bit
   bookkeeping fields).
2. **A separate `shared_ptr` control block** — the handle is constructed as
   `shared_ptr<T>(raw, deleter)` (`runtime/vm.cpp:5054`), so the control block
   is a second allocation, and the deleter captures
   `{shared_ptr<Impl>, worker_id, kind, allocation_id}` (~40 bytes of
   captures), putting the block at ~80 bytes. `make_shared`-style coalescing
   is not used.
3. **A side-table node** — `record_allocation` inserts an `ObjectRecord` into
   the global `std::unordered_map<uint64_t, ObjectRecord> objects_`
   (`runtime/vm.cpp:5817`), one node allocation per object.
4. **The payload** — `std::vector<Value>` items/captures, `MapEntry` vectors,
   per-instance `unordered_map<std::string, Value>` legacy ivars
   (`runtime/vm.h:593`), strings, etc.

Concrete cost: a two-element list carries ~48 bytes of payload inside
~350–400 bytes spread over four allocations. Cross-strand frees are deferred
into per-worker `remote_frees` queues (`runtime/vm.cpp:5842`) and only
physically freed when the owning worker drains them; drains happen at a
handful of interpreter points and at heap teardown.

The GC is **non-moving and does not free memory directly**: `collect_garbage`
marks logical liveness and `reclaim_record_locked` (`runtime/vm.cpp:5989`)
clears payloads and flips lifetime flags, but the shell itself is freed only
when the last `shared_ptr` drops. The "arenas" in `ArenaState`
(`runtime/vm.cpp:4993`) are stats and remote-free queues, not memory arenas —
there is no pooling anywhere.

## 3. Why RSS stays high despite GC

Three distinct mechanisms stack on each other. They look identical from the
outside ("GC ran, RSS didn't drop") but need different fixes.

### 3.1 Page-level pinning from mixed lifetimes (true external fragmentation)

The four allocations per object have **different lifetimes and different size
classes**. Short-lived young objects, their control blocks, immortal
`objects_` map nodes, remembered-set nodes, interned strings, and shape tables
all interleave on the same pages of the same general-purpose heap. When the
young objects die, nearly every page they occupied still holds one live
bookkeeping node, so the allocator cannot return the page. This is the
textbook fragmentation generator: high allocation rate of small mixed-size
objects + a sprinkling of long-lived allocations made at the same time.

### 3.2 Default allocator retention

- **Linux/glibc (ptmalloc2)**: freed chunks go to bins and are returned to the
  OS only when the top of an arena can be trimmed; a single live chunk near
  the break pins everything below it. Per-thread arenas (up to 8× cores)
  multiply the high-water mark. This is the classic "RSS never comes down"
  allocator. Transparent huge pages make it worse: one live byte holds 2 MiB
  resident.
- **macOS default malloc**: nano + scalable zones retain freed blocks in
  per-size-class magazines; `madvise` reuse is lazy. Better than glibc at
  page return, still retains heavily under churn.

### 3.3 VM-side retention that *looks* like fragmentation but is not

These will survive any allocator swap untouched:

- **The runtime string table is immortal and unbounded.**
  `intern_runtime_string` (`runtime/vm.cpp:14983`) appends into
  `module_.strings` and nothing ever removes entries. Every string
  concatenation result (`runtime/vm.cpp:22501`), every `Str()` conversion,
  every error-name stringification allocates a permanent table slot. A heavy
  app that builds strings in a loop grows the table without bound — "high
  memory usage despite the GC" is partly by construction. (Also: intern does
  a linear scan of the whole table, so string-heavy code is quadratic in
  time as well as unbounded in space.)
- **Remote-free queues hold dead objects** until the owning worker drains
  them. An idle or finished worker strand leaves its queue full; the stat
  `remote_queue_depth` exists precisely to observe this.
- **GC-reclaimed shells are not freed.** After a cycle, dead objects have
  payloads cleared but their ~150–200 byte shells (header + struct) stay
  malloc-live as long as any stale `shared_ptr` survives.

## 4. Option A: jemalloc

What it actually buys for this workload:

- **Size-class segregation by extent.** Same-size-class allocations share
  runs/extents, so a page holds only same-sized chunks. This directly
  attacks 3.1: a dead young object's slot is immediately reusable by the next
  same-class allocation, and a fully-dead extent is purgeable.
- **Decay-based purging.** `dirty_decay_ms` / `muzzy_decay_ms` plus
  `background_thread:true` return dirty pages to the OS via `madvise` on a
  schedule, instead of glibc's "almost never." This is the direct fix for
  "poor memory page reuse."
- **Best-in-class observability.** `stats.allocated` vs `stats.active` vs
  `stats.resident` separates *allocator-level* fragmentation from *VM-level*
  retention numerically; heap profiling (`prof:true` + jeprof) attributes it.
  Even if jemalloc never ships, one diagnostic run answers "how much of our
  RSS is fragmentation vs. live data" definitively.
- **Precedent.** Redis adopted jemalloc specifically for this symptom
  (fragmentation under churn with a non-moving heap) and built
  `mem_fragmentation_ratio` and active defrag on its APIs; mozjemalloc is
  Firefox's answer to the same problem.

Costs/caveats:

- macOS is second-tier for jemalloc: it works via zone interposition when
  linked, but it is less exercised than Linux, and `DYLD_INSERT_LIBRARIES`
  experiments are fragile. Linux integration is trivial (link or
  `LD_PRELOAD`).
- It cannot move objects. Long-lived allocations still pin their extents;
  fragmentation *within* a size class drops a lot but does not go to zero.
- None of §3.3 changes.

## 5. Option B: mimalloc (recommended default)

- Equal-or-better fragmentation behavior in published comparisons; free-list
  sharding keeps pages homogeneous much like jemalloc extents.
- **First-class macOS support** (static link with override, or dynamic
  interpose) — matches this repo's dev platform; equally happy on Linux.
- Aggressive page return is a one-knob configuration
  (`MIMALLOC_PURGE_DELAY=0` / `mi_option_purge_delay`), with
  `purge_decommits` for hard decommit.
- Simplest integration story of all candidates: single source amalgamation
  (`mimalloc.c`) compiles into the existing Makefile with no build-system
  surgery; a `MALLOC=` flag can select system/mimalloc/jemalloc flavors.
- Weaker stats than jemalloc — keep a jemalloc build flavor for diagnosis.

## 6. Options C: tcmalloc, snmalloc

- **tcmalloc** (google/tcmalloc): excellent throughput via per-CPU caches, but
  page-heap fragmentation for mixed-lifetime small objects is historically the
  weak spot, and the modern version drags in Bazel. The gperftools variant is
  legacy. Not the pick.
- **snmalloc**: built around producer/consumer message-passing frees — remote
  frees are queued back to the owning allocator, which is *exactly* the
  semantics `RuntimeHeap` hand-implements with `remote_frees`. Architecturally
  the best match for the strand-ownership model, and worth a note if the
  custom-heap route (§7) ever wants a backing allocator; too niche to be the
  default recommendation.

## 7. Option D: fix the VM's allocation structure (the "better solution")

An allocator swap treats the symptom at the page level. The structural fixes
below remove the fragmentation generators themselves, and three of them are
cheap because `allocate<T>` is already a single choke point:

1. **Fix the string table lifecycle first.** This is the only unbounded
   growth and no allocator touches it. Either make runtime strings real heap
   objects (GC-managed, like every other value) or keep the table but make it
   a hash-consed set with refcounts/weak entries and periodic compaction.
   Fixes the quadratic intern scan as a side effect. For string-heavy apps
   this fix alone may dwarf everything else in this document.
2. **Intrusive refcount in `ObjHeader`.** The header already exists on every
   object; adding the count there and switching `Value` to an intrusive
   pointer eliminates the separate control block and its fat captured deleter
   (~80 bytes and one allocation per object), and shrinks every `Value` from
   24 to 16 bytes (the variant's largest member drops from a 16-byte
   `shared_ptr` to an 8-byte pointer) — which shrinks every list/tuple/ivar
   payload by a third.
3. **Per-(worker, kind) slab pools for shells.** Object shells are
   fixed-size per kind at compile time, the allocator already receives
   `sizeof(T)` and the worker id, and per-worker arena bookkeeping already
   exists in name. Slabs make pages single-kind/single-size (perfect reuse),
   make empty-slab `madvise` trivial, and remove the two global mutex
   acquisitions currently paid per allocation
   (`reserve_allocation_id` + `record_allocation`,
   `runtime/vm.cpp:5802`/`5807`) by making ids and records slab-local.
4. **Flatten the side tables.** `objects_` as a node-based `unordered_map`
   is one allocation per live object plus pointer-chasing for the GC; slab
   metadata or an open-addressed table indexed by allocation id removes the
   node churn entirely and makes mark/sweep cache-friendly.
5. **Drain remote frees on allocation pressure**, not just at fixed
   interpreter points — e.g. every N allocations or when `remote_queue_depth`
   crosses a threshold, so dead cross-strand objects do not ride out idle
   periods.
6. **Endgame: moving young generation.** The header already carries
   `generation`, `gc_age`, pin counts/epochs, and the pin API exists precisely
   so native code can hold objects still while others move. A bump-allocated
   young space with evacuation is the only *complete* answer to page reuse —
   compaction is the one cure for fragmentation that allocators cannot
   provide. This is a large project (every interior `std::vector`/`string`
   payload must move or be owned by the object) and belongs in a later phase;
   nothing in options A–C conflicts with it.

## 8. What an allocator swap cannot fix (explicit)

- Unbounded string-table growth (§3.3) — untouched.
- 4 allocations per object — same count, just better-placed.
- Dead shells held by stale `shared_ptr`s and undrained remote-free queues —
  still malloc-live.
- Fragmentation from genuinely long-lived objects scattered across extents —
  reduced, not eliminated (no compaction).

## 9. Measurement plan (do this before and after each step)

- **Define the metric**: fragmentation ratio = RSS / live heap bytes.
  `RuntimeHeapStats` counts objects but not bytes; add a live-bytes counter
  (allocation_size is already recorded per object).
- **One diagnostic run under jemalloc** with `stats_print:true`:
  `stats.resident − stats.allocated` is allocator fragmentation + retention;
  the rest of RSS vs. live bytes is VM-side retention. This single number
  split decides how much budget options A–C vs. D deserve.
- **Platform tools**: macOS `vmmap --summary` (region-level fragmentation),
  `leaks`/`malloc_history` with `MallocStackLogging` (retention attribution);
  Linux `smaps_rollup`, massif or jeprof.
- **Bench scenario**: add a churn benchmark to `bench/` that mimics the heavy
  apps — mixed-size object graphs with mixed lifetimes plus a string-building
  loop — and track peak RSS, post-GC RSS, and the fragmentation ratio across
  allocator flavors.
- **Validate the 4-allocations-per-object claim** once with a malloc-count
  profile so the doc's arithmetic stays honest.

## 10. Recommendation

Layered, in order of effort-to-impact:

1. **Now (days):** add a Makefile allocator flag with three flavors — system,
   mimalloc (default candidate, best macOS story, aggressive purge), jemalloc
   (Linux deploys + diagnosis). Configure for page return
   (`MIMALLOC_PURGE_DELAY=0` / jemalloc decay + background thread). Run the
   §9 measurements; expect a substantial RSS drop and keep whichever flavor
   wins per platform. Disable THP coupling on Linux deploy docs.
2. **Next (Phase-sized):** §7 items 1–5 — string-table lifecycle (the
   unbounded one), intrusive refcounts, per-kind slab pools, flat side
   tables, pressure-driven remote-free drains. These remove the root causes
   and also cut allocator traffic ~4× and global-lock traffic per allocation.
3. **Later:** moving/compacting young generation on the existing
   generation+pin machinery — the only complete fix for page reuse; schedule
   alongside the native-backend work since the pin API is the shared
   contract.

## Adjacent findings (out of scope, noted while reading)

- `remove_remembered_edges_for_locked` (`runtime/vm.cpp:6006`) walks the
  entire remembered set on every free — O(remembered set) per free, quadratic
  under mature-object churn.
- `intern_runtime_string`/`intern_runtime_symbol` linear scans make
  string/symbol-heavy interpretation quadratic in time independent of memory.
- Every allocation takes the single global heap mutex twice; per-worker
  "arenas" currently provide no concurrency isolation.

## 11. Results — Layer-1 implemented and measured (2026-06-13)

§10 step 1 is done. The allocator is now a build flag, RSS/live-bytes are
instrumented, and the §9 measurement has been run. Steps 2–3 are unchanged.

What landed (default `system` build untouched):
- `MALLOC=system|mimalloc|jemalloc` in the Makefile (`-DAMBER_ALLOCATOR`,
  Homebrew-prefix discovery, `-Wl,-force_load` of the static archive on macOS so
  the zone override interposes; `-l` alone does not on Darwin).
- `RuntimeHeapStats.live_object_bytes` / `tracked_object_bytes`, summed over
  `objects_` in `stats()` (shell bytes; a proxy, not payload-exact).
- `AMBER_HEAP_STATS=1` runner dump: allocator, current + peak RSS, live/tracked
  bytes, RSS/live ratio (one stderr line, stdout untouched).
- `bench/heap/churn.am` — the §9 bench scenario (large mixed-lifetime live set +
  distinct-string loop). See `bench/heap/README.md`.

Sweep (churn.am, Darwin arm64, mimalloc 3.3.2 / jemalloc 5.3.0):

```text
allocator   config                     peak RSS   end RSS   returned to OS
system      macOS libmalloc             ~97 MB     ~97 MB    ~0%
mimalloc    default / PURGE_DELAY=0     106 MB     106 MB    ~0%  (MADV_FREE; see below)
jemalloc    default                      95 MB      27 MB    ~72%
jemalloc    dirty/muzzy_decay_ms:0       95 MB    10.8 MB    ~89%  (≈ baseline)
```

Findings vs. the predictions above:
1. **Peak RSS is allocator-independent (~95–106 MB)** — confirms §8: the swap
   does not change the allocation count or peak working set; only §7 structural
   fixes reduce peak.
2. **jemalloc returns memory on macOS, mimalloc's return is invisible to RSS.**
   mimalloc reclaims via `madvise(MADV_FREE)`, which does not drop
   `resident_size` on Darwin — exactly the §4/§9 "macOS is second-tier, use
   vmmap" caveat. So the §5 "mimalloc default" pick holds only with a Linux/vmmap
   measurement; on macOS the measurable RSS win is **jemalloc + decay**.
3. **Live heap < 0.5 MB at a 95 MB peak** (live_object_bytes ≈ 1.2 KB, jemalloc
   at-exit `Allocated` 474 KB, `gc_cycles=0`): confirms §3 — RSS is overhead +
   page retention, not live data. Refcounting frees promptly; the cost is
   allocation *traffic* and page-return *policy*, not retained live objects.
4. **4-malloc/object spot-check:** jemalloc cumulative `nrequests` = 16,521,121
   for the churn vs 642 trivial ≈ 6.9 malloc calls per VM heap object — above
   the documented 4 (remainder = interpreter temporaries + string loop), so the
   §2 arithmetic stands as a lower bound. Exact isolation needs a counter inside
   `allocate<T>`; deferred.

Net: keep the flag (jemalloc+decay is a free RSS win on this workload), but it
treats the symptom — peak is unchanged and the live set is tiny, so §7.1
(string-table lifecycle) and §7.2 (intrusive refcounts) remain the higher-impact
levers and are the recommended next phase.
