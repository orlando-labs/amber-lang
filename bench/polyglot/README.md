# Polyglot benchmark

This benchmark compares identical workloads across:

- Amber interpreted: `build/iamber --eval-file`
- Amber built native executable: `amberc build <file.am>` first, then the
  generated host binary
- Python
- Ruby
- C++ compiled with `-O2`
- Go when `go` is available in `PATH`

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
- `sha-digest`: `2242493101`

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

SHA digest:

```text
program             runs     mean_s     best_s  peak_rss_mb           checksum
----------------------------------------------------------------------------------
amber-interpreted     10     0.1341     0.1314          6.2         2242493101
amber-built           10     0.0043     0.0042          2.1         2242493101
python                10     0.0778     0.0768         15.3         2242493101
ruby                  10     0.0521     0.0515         16.5         2242493101
cpp                   10     0.0024     0.0023          1.3         2242493101
go                    10     0.0032     0.0029          4.0         2242493101
```

The arithmetic executable reports full native coverage for 2/2 code objects.
The `calls-collections` executable reports full native coverage for 10/10 code
objects: module-init closures use shared native capture cells, closure calls
stay in generated C++, and list literals plus `[]`, `count`, and `first` use
the direct list path. Its `amber-built` mean improved from the previous
`0.0101s` fallback result to `0.0032s`, about 3.2x faster and close to the Go
result. Stack-backed native register frames also improved arithmetic from the
previous `0.0051s` mean to `0.0042s`.

The `sha-digest` executable reports full native coverage for 9/9 code objects.
The workload exercises the SHA-256 compression schedule, list index assignment,
integer `&`, `|`, `^`, `<<`, and `>>`. The Amber source keeps the hot 32-bit
rotate/mix formulas inline so the native path does not pay closure-call ABI
cost for tiny bitwise helpers; 32-bit wrap uses `& 0xffffffff` instead of
division-based modulo.

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
python3 bench/polyglot/run_benchmark.py \
  --workload sha-digest \
  --repeats 10 \
  --build-dir /private/tmp/amber_polyglot_sha_digest_final
python3 bench/polyglot/run_benchmark.py \
  --workload sha-digest \
  --repeats 10 \
  --no-build \
  --build-dir /private/tmp/amber_polyglot_sha_digest_final
```

## Numeric profile v1: checked Int arithmetic cost (2026-06-12)

amber.numeric-profile.v1 landed checked Int64 arithmetic as the default
profile (`__builtin_*_overflow` + policy resolution in every VM int path,
checked helpers + `NativeBailout` on overflow in the native lane). Same-day
same-machine A/B of the arithmetic workload, 10 repeats, against a clean
`HEAD` worktree without the numeric changes:

```text
lane                       baseline best   checked best   delta
amber-interpreted                 0.2984         0.3070    ~+3%
amber-built (native)              0.0046         0.0056   ~+0.001s (noise-level)
```

Absolute numbers are not comparable with the earlier tables in this journal
(different machine power/thermal state — the same-day baseline rerun of the
unchanged HEAD measured 0.2984s where the old table recorded 0.1409s). The
delta column is the meaningful result: overflow checking costs ~3% in the
interpreter, in line with the design expectation that interpreter dispatch
dominates. Non-default profiles (wrapping/saturating/narrow widths) run
VM-only; the native lane keeps default-profile semantics via bailout-restart.

## Phase 2 interpreter tuning (2026-06-12)

Same-day A/B against the pre-change HEAD build, best of 10 direct
`build/iamber --eval-file` runs per workload (Darwin arm64). The serialized
`.amberbc` format is unchanged; all changes are VM-internal. Conformance
corpus (78), full `make test`, and `make backend-equivalence` (22) pass at
every step.

```text
workload            baseline   step1    step2    final    speedup
---------------------------------------------------------------------
arithmetic            0.3127   0.1621   0.1611   0.1549     2.0x
calls-collections     0.0145   0.0107   0.0110   0.0104     1.4x
sha-digest            0.1509   0.0880   0.0717   0.0715     2.1x
blocks micro (below)  3.50     —        —        0.54       6.5x
```

Step 1 — on-demand text source locations. `execute()` built a
`RuntimeTextSourceLocationScope` around every `step()`: a frame walk, a
source-span search, a file-string copy, and a TLS round trip per
instruction, only so text output events could be attributed to module
statements. Sampling profiles attributed ~25-35% of interpreter time to it
on both arithmetic and sha-digest. Output attribution now resolves through a
TLS provider installed once per `execute()` loop; text writers call
`resolve_runtime_text_source_location()` at output-event time and get the
same module-statement attribution.

Step 2 — quickened list `[]=` + allocation-free selector aliasing. Send
quickening previously stopped at one positional argument, so the sha-digest
inner loops (`w[i] = ...`) paid the full generic send path: operand vector
decode, args vector build, and ~24 selector string compares per store.
`SendSeqIndexSet` now handles two-argument `[]=` sends on lists with the
same fault classification as the generic handler (non-list receivers,
frozen lists, and non-integer indexes still fall back).
`canonical_collection_selector` returns references to static strings
instead of allocating per call.

Step 3 — block-call path. Builtin collection blocks (`each`/`map`/...)
constructed a fresh nested `Vm` per block invocation, which deep-copied the
entire `BcModule` (strings, symbols, const pool, all code objects),
re-quickened the block code, and threw both away after one element. Block
invocations now lease a pooled child Vm with append-only string/symbol
table sync at the lease boundaries in both directions. The sync also fixes
a correctness bug: strings interned inside a block escaped as
`<invalid-string>` because the nested module copy was dropped (pinned in
`corpus/run/block_string_intern_escape`). On top of that: per-frame pattern
state (`prepared_seq_regs`/`prepared_map_regs`/`pending_pattern_bindings`)
moved from `unordered_map` to flat vectors, so block-parameter prologues
stop paying hash-node malloc/free per invocation; block args pass as
`initializer_list` instead of heap vectors; per-opcode safepoints check
atomic mirrors of the remote-free queue depth and pending-GC flag before
touching the heap mutex; pooled block Vms skip the completed-frame register
snapshot that `finish_return` captures for `execute()` consumers.

The block microbench (not part of the polyglot suite; kept here for
reproducibility — 200k iterations x 8-element `each` = 1.6M block calls):

```amber
def main():
  data = [1, 2, 3, 4, 5, 6, 7, 8]
  total = 0
  i = 0
  while i < 200000:
    sum = 0
    data.each |x|:
      sum = sum + x
    total = total + sum
    i = i + 1
  total
```

Remaining known hot spots after this pass (sampled on sha-digest and the
block micro): interpreter dispatch in `step()`, the integer-sidecar
read/write helpers, and `Frame` move/destroy traffic in the call path. The
first two are the Phase 4 value-representation work; the frame moves need a
`finish_return` restructure that did not pay for its risk in this pass.

Step 4 — ivar sites (research plan §5.4), same-day follow-up. An
ivar-heavy micro (300k method calls, 8 ivar accesses each: a `Counter`
class whose `tick(i)` mutates `@a`/`@b`/`@total`) measured 0.397s best.
Profiling showed the cost was strings, not slots: every `LoadIvar`/
`StoreIvar` copied the ivar name out of the symbol table
(`optional<string>` per access), `StoreIvar` probed the inline cache and
then discarded the hit (`(void)cached_slot`) before re-resolving the slot
by name three times, and `try_apply_scalar_send` evaluated its
sequence/numeric selector-set memberships (~70 string compares)
unconditionally for every send — including plain user method calls.
`LoadIvar`/`StoreIvar` are now quickened with cache-hit fast paths
(pre-decoded operands, no name copy, store uses the cached slot and keeps
the legacy string-keyed `ivars` mirror in sync because GC tracing and
legacy lookups read it); watched objects, lifecycle faults, and cache
misses fall back to the generic handlers. The selector-set tests are gated
by receiver kind. Ivar micro: 0.397s -> 0.286s (-28%); the three polyglot
workloads and the block micro are unchanged (within noise). Remaining ivar
cost is the per-call method-send path (`call_caches` probe + generic
operand decode) and the global `ivar_caches` hash probe per access —
inline ICs in the quickened stream would be the next step but need
mutable per-site state, which the shared QuickCode does not have today.

## §5.8 emitter parameter int-speculation: attempted, reverted (2026-06-13)

Tried marking `param`/`block_param`/`implicit_block_param` slots as
speculative integer candidates in `analyze_integer_locals`
(`bytecode/emitter.cpp`), so parameter arithmetic emits `I*`/`I*K` opcodes
instead of generic `SEND`. The speculation is sound — every integer opcode
deopts to the full send path when an operand is not an `Int` at runtime, so
it only chooses opcodes, never semantics — and it required first fixing the
`I*K` constant-operand deopt to rebuild a full synthetic `Send` like the
register-operand deopt already does.

Same-day same-machine A/B, best of 8 after 2 warmups (Darwin arm64):

```text
workload            no-spec   with-spec   delta
---------------------------------------------------------
arithmetic           0.1529     0.1467     -4%   (noise band)
calls-collections    0.0109     0.0403    +270%  REGRESSION
sha-digest           0.0705     0.0691     ~0
params (micro)       0.0964     0.0891     -8%   (real, modest)
blocks (micro)       0.4993     0.4832     -3%
```

Reverted: a 3.7x regression on a shipped benchmark is not worth an 8% win on
an integer-parameter-leaf micro.

Root cause — `classify_direct_closure` (`runtime/vm.cpp`) holds hand-written
recognizers (`SequenceIndex`/`LaneIndex`/`WrapSubtract`/`MixLinearWrap`/
`ScoreRow`) that match the **exact `SEND`-shaped bytecode** of this
benchmark's leaf functions (`pick`/`lane_index`/`wrap`/`mix`/`score_row`)
and evaluate them inline with no frame push. Speculation rewrites those
`SEND`s to `IDIV`/`IMUL`/`ISUB`/`IGT`/`IADD(K)`, so 3 of the 5 recognizers
(`LaneIndex`, `WrapSubtract`, `MixLinearWrap`) stop matching and those
functions fall back to full frame pushes — the regression is call overhead,
not the integer ops. (`SequenceIndex`/`pick` and `ScoreRow` still match: `[]`
stays `SEND` and `score_row`'s arithmetic is on non-candidate locals.)

To unblock, in order:
1. Land the `I*K` constant-operand deopt fix (route through a full synthetic
   `Send` via a scratch register, matching the register-operand deopt). Today
   that path is unreachable — only proven-integer locals get `I*K` and their
   operands are always `Int` — so it cannot be landed or tested on its own;
   it must land with the speculation that makes it reachable.
2. Make the recognizers opcode-agnostic. A binary-op "view" that
   canonicalizes both a 1-arg binary `SEND` and the `I*`/`I*K` forms to
   `(selector, dst, lhs, rhs[, const])` lets `LaneIndex`/`WrapSubtract` match
   either shape unchanged (same instruction count, reg-reg ops). `MixLinearWrap`
   genuinely restructures (`LoadK`+`SEND` collapses to `IADDK`/`IMULK`, fewer
   instructions) and needs a second matcher.
   Better still: retire these benchmark-specific recognizers in favor of a
   general frameless-leaf-call path that consumes specialized bytecode.

## §6.1 heap allocator flag + fragmentation measurement (2026-06-13)

Layer-1 of `RESEARCH-heap-fragmentation-allocators-2026-06-12.md` §10 ("Now
(days)"): make the allocator swappable, instrument RSS vs. live bytes, add a
churn benchmark, and run the §9 measurement. Layers 2–3 (string-table
lifecycle, intrusive refcounts, slab pools, moving young gen) are unchanged and
remain the larger follow-on phases.

Landed:
- **`MALLOC=system|mimalloc|jemalloc` Makefile flag.** Sets the `AMBER_ALLOCATOR`
  macro; for mimalloc/jemalloc it discovers the Homebrew prefix and, on macOS,
  `-Wl,-force_load`s the static archive so the malloc-zone override actually
  wins (plain `-l` does not interpose on Darwin). Default `system` build and
  flags are unchanged.
- **`RuntimeHeapStats.live_object_bytes` / `tracked_object_bytes`**, summed over
  `objects_` in `stats()` (allocation_size was already recorded per object).
  Shell bytes only — a cheap proxy, not a payload-exact live total.
- **`AMBER_HEAP_STATS=1` dump** in the runner: one stderr line with allocator,
  current + peak RSS (mach `task_info` / `getrusage`), live/tracked bytes, and
  the RSS / live-bytes ratio. Off by default; never touches stdout.
- **`bench/heap/churn.am`**: a large (RING_CAP=40000) simultaneous live set of
  mixed-size graphs with mixed lifetimes, plus a distinct-interned-string loop.
  A small ring frees too promptly to show anything; the large ring builds a
  real fragmented heap then drops it.

Same-machine sweep, churn.am, Darwin arm64 (mimalloc 3.3.2, jemalloc 5.3.0):

```text
allocator   config                          peak RSS   end RSS   returned
-----------------------------------------------------------------------------
system      macOS libmalloc                  ~97 MB     ~97 MB    ~0%
mimalloc    default / PURGE_DELAY=0          106 MB     106 MB    ~0%  (see note)
jemalloc    default                           95 MB      27 MB    ~72%
jemalloc    dirty/muzzy_decay_ms:0            95 MB    10.8 MB    ~89%  (≈baseline)
```

Output value `1411573421` is identical under all three — correctness preserved.

Findings:
1. **Peak RSS is allocator-independent (~95–106 MB).** It is set by the VM's
   4-malloc-per-object pattern over a large simultaneous live set; no allocator
   lowers it. Only §7 structural fixes (intrusive refcounts, slabs) cut peak.
2. **Page return is the allocator-sensitive symptom, and jemalloc wins on
   macOS**: 95 MB → 10.8 MB with aggressive decay. mimalloc's reclaim is real
   but uses `madvise(MADV_FREE)`, which marks pages reclaimable-under-pressure
   without dropping `resident_size` on Darwin — so its win is invisible to RSS
   here (it would show on Linux / via `vmmap` dirty-vs-clean). This is the doc's
   "macOS is second-tier, measure with vmmap" caveat (§4, §9) made concrete, and
   it nuances the "mimalloc default" pick: on macOS the *measurable* RSS win is
   jemalloc+decay; the mimalloc-vs-jemalloc default should be settled on Linux.
3. **Live heap is < 0.5 MB at a ~95 MB peak.** `live_object_bytes` ≈ 1.2 KB and
   jemalloc's at-exit `Allocated` is 474 KB; `gc_cycles=0` (refcounting freed
   all 2.4M objects promptly, GC never ran). RSS is overhead + page retention,
   not live data — exactly the doc's §3 thesis. The RSS/live-bytes ratio at
   end-of-run is therefore dominated by fixed baseline; **peak + post-churn end
   RSS across flavors, and jemalloc `stats_print`, are the informative numbers.**
4. **4-malloc/object spot-check (§9):** jemalloc cumulative `nrequests` =
   16,521,121 for the churn vs 642 for a trivial program ≈ 6.9 malloc calls per
   VM heap object. That is *above* the documented 4 (the remainder is per-iter
   interpreter temporaries + the string loop), consistent with — and a lower
   bound on — the 4-shell-allocations claim. Exact isolation needs a counter
   inside `allocate<T>`; deferred.

Net: the swap is worth keeping as a flag (jemalloc+decay is a real, free RSS win
on this workload), but it treats the symptom — peak is unchanged and the live
set is tiny, so the §7.1 string-table lifecycle and §7.2 intrusive-refcount work
remain the higher-impact levers. Run the sweep with `bench/heap/README.md`.
