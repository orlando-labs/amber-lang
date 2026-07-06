# DESIGN: Intrusive refcount in ObjHeader (RESEARCH §7.2)

Date: 2026-06-13
Status: design + in-progress implementation
Scope: `runtime/vm.h` `Value` / `ObjHeader`, `runtime/vm.cpp` `RuntimeHeap::Impl`
Follows: RESEARCH-heap-fragmentation-allocators-2026-06-12.md §7.2

## 1. Goal and what it buys

Today every heap object costs a second malloc beyond its shell: `allocate<T>`
makes `std::shared_ptr<T>(raw, deleter)` where the deleter captures
`{shared_ptr<Impl>, worker_id, kind, allocation_id}` (~40 B), so the control
block is a separate ~80 B allocation per object (`runtime/vm.cpp:5053`). Moving
the refcount into the `ObjHeader` (which exists on every object already) and
switching `Value` to an intrusive pointer removes that allocation and its fat
deleter — one fewer malloc and ~80 B per object, on the highest-churn path.

## 2. Scope decision

Only the **6 ObjHeader-bearing heap kinds** — Closure, Instance, List, Tuple,
Set, Map — go through `allocate<T>`. Convert exactly those. The `Value` variant
also holds `shared_ptr` for ~18 other types (BigInt, error instances, the
Runtime* task/channel/io/etc. objects) that have no `ObjHeader` and are
low-churn; leave them as `shared_ptr`.

Consequence: the variant still contains 16-byte `shared_ptr` alternatives, so
**`Value` stays 24 bytes for now** — the doc's 24→16 shrink needs the ~18-type
tail converted too and is deferred to a follow-up. This phase is purely the
control-block / malloc-per-object win, which is the fragmentation-relevant part.

## 3. Invariants confirmed (audit, 2026-06-13)

- `shared_ptr<{6 kinds}>` is named only in `vm.h` (16) and `vm.cpp` (158); the
  267 `.as_X()` consumers elsewhere use `auto`/temporaries. So an API-compatible
  intrusive pointer keeps the conversion inside the runtime module.
- No `weak_ptr`, `use_count`, `*_pointer_cast`, `shared_ptr<void>`,
  `enable_shared_from_this`, or `make_shared` on the 6 kinds; the only references
  are `Value`s (strong) and the `objects_` map's raw `void*` (non-owning, GC).
- The 6 structs each have `ObjHeader header` as their first member and are never
  value-copied (so an `std::atomic` in the header is safe).
- GC is independent of the count: `reclaim_record_locked` marks logical death +
  clears payload but never frees the shell; `release()` frees on the last ref
  and already no-ops the live-decrement when GC reclaimed first. Drop-to-zero
  reuses `release()` unchanged.

## 4. Design

**ObjHeader** gains `std::atomic<std::uint32_t> ref_count{0};` and
`void *heap = nullptr;` (the owning `RuntimeHeap::Impl*`, type-erased because
`Impl` is private to `vm.cpp`). `allocate<T>` sets `ref_count = 1` (the adopted
handle), `heap = this`, records the raw pointer in `objects_` as today, and
returns an adopting intrusive handle — no `shared_ptr`, no deleter, no control
block.

**IntrusivePtr<T>** (vm.h) holds a single `T*` (8 B). Copy bumps, destroy drops,
move steals; on drop-to-zero it frees. To keep it usable with the **incomplete**
`ListValue` etc. at the many include sites (like `shared_ptr`), all T-touching
logic is out-of-line: vm.h only *declares*

```cpp
template <class T> void runtime_heap_add_ref(T *) noexcept;
template <class T> void runtime_heap_release(T *) noexcept;
```

and vm.cpp *defines* + explicitly instantiates them for the 6 kinds. The
definitions read `obj->header` (count, kind, allocation_id, arena_worker_id,
heap) and, on zero, call `Impl::release({obj, destroy<T>, worker, kind, id})` —
byte-for-byte the old deleter. The count is atomic (objects cross strands); the
decrement is `acq_rel` so exactly one dropper sees zero.

**Cross-strand free** is unchanged: `release()` already queues to the owner
worker when `current_worker != owner`; drop-to-zero on a foreign strand routes
through it identically.

## 5. Rollout

1. Foundation: header fields + `IntrusivePtr` + the two free-fn templates
   (defined, instantiated), wired into `allocate` — but the variant still on
   `shared_ptr` would not compile against the new `allocate`, so the switch is
   effectively one step (§2 below). Keep `ff33dca` as the green checkpoint.
2. Switch: variant's 6 alternatives + `make`/`as` signatures shared_ptr→
   IntrusivePtr; rewrite `allocate`; update the ~174 type sites; delete the
   deleter. Build to green, run full `make test`.
3. Measure: jemalloc `nrequests`/object on `churn.am` should drop by ~1 per
   heap object (control block gone). Journal + commit.
4. Deferred: convert the ~18-type tail to shrink `Value` 24→16 (separate phase).

## 6. Risks

- **Teardown ordering.** The old deleter held `shared_ptr<Impl>`, guaranteeing
  the heap outlived its objects. The raw `header.heap` does not; rely on the
  world freeing all live `Value`s before `~RuntimeHeap::Impl` (which then
  `drain_all_remote_frees`). Any object outliving the heap would be a
  use-after-free — covered by the full suite + churn under ASan if doubtful.
- **Atomic-in-header** makes `ObjHeader` (and the 6 structs) non-copyable; the
  audit found no copies, the compiler enforces the rest.
- **No semantic change** intended: ids, GC, equality, ordering all untouched;
  the full corpus + unit suites are the gate.
