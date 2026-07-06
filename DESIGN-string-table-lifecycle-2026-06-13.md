# DESIGN: Runtime string-table lifecycle (bounding unbounded growth)

Date: 2026-06-13
Status: design / decision-needed — no code change in this doc
Scope: `runtime/vm.cpp` runtime string + symbol tables, `Value` string representation
Follows: RESEARCH-heap-fragmentation-allocators-2026-06-12.md §7.1; Layer-2a
(O(1) interning) is landed — this is the second, harder half of §7.1.

## 1. Problem

`module_.strings` / `module_.symbols` are immortal and unbounded: every distinct
interned string is a permanent slot, never reclaimed. A long-running app that
builds distinct strings (logs, JSON, `+` concatenation, `Str()`/`to_str`) grows
the table without bound. Layer-2a replaced the O(n) intern scan with a hash
index, fixing the *time* blowup (O(n²)→O(n), ~125× on `intern_scaling.am`). It
did **not** bound the *space* — that is this doc.

This is the one growth source no allocator swap touches (RESEARCH §3.3): in the
churn benchmark the live heap is < 0.5 MB while the table grows with every
distinct string, so for string-heavy apps this is the dominant retention.

## 2. Current representation and the invariants it imposes

A string value is `Value::string{ std::uint32_t string_id }`
(`runtime/vm.h:103`), an index into the owning `Vm`'s `module_.strings`
(`std::vector<std::string>`). Resolving id→text is an O(1) vector index
(`string_text_from_id`). Three invariants make naive reclamation hard:

- **INV-A (id-equality).** Interning guarantees equal text ⇒ equal id, and
  string `==` is implemented as `string_id == string_id`
  (`runtime/vm.cpp:2533`, `:8800`). Fast equality and map/set keying lean on
  this. Any scheme must either keep interning (ids stay canonical) or rewrite
  every equality/hash site to compare bytes.
- **INV-B (ids are module-local).** A `string_id` is only meaningful relative to
  one `Vm`'s table. Nested modules reconcile their tables by splicing
  (`runtime/vm.cpp:9651`–`9676`); the same text has different ids in different
  modules. So a `Value` in flight does not, by itself, identify which table its
  id belongs to.
- **INV-C (Value is context-free and copied everywhere).** `Value` is a
  `std::variant` copied/destroyed on essentially every interpreter step, with no
  lifecycle hook and no back-pointer to its `Vm`. It is trivially copyable today.

## 3. Why the obvious fixes do not work here

- **Refcount each table slot via Value copy/move/destroy.** Would need `Value`
  to gain custom copy/move/dtor that bump/drop a per-slot count — but the count
  lives on a *specific* `Vm`'s table and a `Value` cannot name its `Vm` (INV-B,
  INV-C), and adding non-trivial copy semantics to the hottest type in the
  interpreter is a large, pervasive cost. Infeasible without a `Value` redesign.
- **Mark-and-compact the table at GC.** The GC is non-moving (RESEARCH §2).
  Heap-resident string `Value`s are reachable through the existing child walk
  (`append_child_values`), but the live *execution state* — operand stacks,
  registers, locals, call frames, pending exceptions — also holds string
  `Value`s, and reclaiming/renumbering ids means finding and rewriting *all* of
  them at a safepoint. That is the same "enumerate every live Value" problem
  that makes the moving young-gen (RESEARCH §7.6) a large project; truncating
  only the dead *tail* without remapping reclaims almost nothing.

The conclusion the research anticipated: bounding the table needs either a
`Value`-representation change or full GC-integrated string liveness. Both are
bigger and riskier than Layer-2a and must not be done blind.

## 4. Options

**Option A — runtime strings as heap objects (the research's preferred answer).**
Add `HeapObjectKind::String` allocated through the existing `allocate<T>` choke
point, so runtime-built strings are refcount-freed and GC-traced like every
other heap value. Keep compile-time constants and symbols as interned ids (they
are bounded and bytecode-referenced). `Value` grows a second string form
(interned-id vs. heap-string handle). Cost: INV-A breaks — equality/hash
(`vm.cpp:2533`, `:8800`, map keying) must handle id↔id (fast), obj↔obj, and the
mixed case by text; every string producer/consumer must accept both forms.
Well-bounded but broad. Naturally rides the §7.2 intrusive-refcount work (a heap
string wants exactly that header), so it should land *after* §7.2, not before.

**Option B — GC-integrated interned-table compaction.** Keep ids; at a full-GC
safepoint, trace all live string ids (heap + execution state), rebuild the table
with only-reachable entries, and remap every live string `Value`. Preserves
INV-A. Cost: requires the full live-Value enumeration + rewrite of §7.6's moving
GC; couples this fix to that endgame. Largest option; only sensible if/when
moving GC happens.

**Option C — inline small-string `Value` variant (interim, partial).** Add an
owned/inline string form to `Value` for runtime strings, leaving the interned
table for constants + symbols. Bounds growth for the common short-string case
without GC work, but is itself a `Value` change, still breaks INV-A's fast path,
and large strings need a heap form anyway — so it is a subset of Option A with
its own equality complexity and little standalone payoff.

**Option D — observability + bound-by-policy (cheap, safe, now).** Not a reclaim
fix, but: (1) surface runtime string-table count/bytes in the `AMBER_HEAP_STATS`
dump so the leak is *measured* (closes RESEARCH §9's "track it") — computable
**runner-side** from `ExecutionResult.runtime_strings` vs. the executed module's
compile-time `module.strings.size()`, with no VM API or `RuntimeHeapStats`
change; (2) optionally a soft cap that warns/faults past a configured ceiling so
a runaway is contained rather than silent. Semantics-preserving for normal runs.

## 5. Recommendation

1. **Now:** Option D's observability — add `runtime_string_count` /
   `runtime_string_bytes` (the `[initial_string_count_, end)` region) to the
   heap-stats dump, computed runner-side from `ExecutionResult.runtime_strings`
   (no VM API change). Cheap, safe, makes the growth a number instead of a
   claim, and lets us confirm any later fix.
2. **Next, with §7.2:** do intrusive refcounts first (RESEARCH §7.2). That work
   already reshapes `Value` and adds the `ObjHeader` intrusive count a heap
   string needs.
3. **Then:** Option A — runtime strings as heap objects, riding the §7.2
   machinery, with INV-A reworked once (equality/hash handle id and heap forms).
   This is the complete fix and the research's intended end state.
4. **Defer** Option B unless/until the moving young gen (§7.6) is on the table;
   then table compaction is a small rider on the same Value-enumeration pass.

Net sequencing: 2a (done) removes the time cost; the space fix is gated on §7.2
and should not precede it. Land Option D's counter now so the leak is tracked in
the interim.
