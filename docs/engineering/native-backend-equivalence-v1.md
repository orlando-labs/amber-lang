# amber.native-backend-equivalence.v1

Status: active guard; run via `make backend-equivalence`
(`tools/backend_equivalence.py`), wired into CI after `make conformance`.

## What it checks

Every `corpus/run` fixture is compiled into two executables with
`amberc build`:

- `--target bytecode-wrapper`: the VM lane (the semantic oracle);
- `--target native`: the `cpp-bytecode-direct-v1` lane, which runs generated
  C++ for eligible code objects. Coverage-incomplete builds may fall back to
  the VM through the whole-program `NativeBailout` restart; full-coverage
  builds emit no VM entry or bailout restart path.

Both executables must produce byte-identical stdout, stderr, and exit codes.
Fixtures whose `meta.json` entry is a callable are driven by appending an
entry call to module init, so the executable's printed init result is the
entry result. Fault fixtures (e.g. `OverflowError`, `ZeroDivisionError`)
compare the full trace output and the nonzero exit code.

## The bailout/restart soundness invariant

This section applies only when build metadata reports
`native_bytecode_fallback: true`.

The native lane handles anything it cannot execute (including checked-Int
overflow, which it detects with `__builtin_*_overflow` helpers) by throwing
`NativeBailout`; the generated `main()` catches it and re-runs the entire
program, including module init, under the VM.

This is observably equivalent **only while native-eligible code produces no
effect that survives a bailout**. Effectful direct-native helpers call
`native_commit_effect()` before their first observable operation. After that
barrier, `main()` turns `NativeBailout` into `NativeCodeError` instead of
restarting the VM, so an output, task, or IO operation cannot be duplicated.
Most other admitted operations are simply side-effect-free. Two additional
families are safe by reachability:

- user instances, ivars, and mutable collections allocated by the direct lane
  live only in its private heap; `LOAD_IVAR`, `STORE_IVAR`, user-method
  dispatch, `copy`/`deep_copy`, and admitted collection mutators may change
  that heap, because the complete heap is abandoned before the VM restart;
- discardable samples such as `SecureRandom` results and `Time.monotonic` do
  not mutate externally visible state. If a later instruction bails, the
  sampled value and every value derived from it are abandoned too.

The eligibility scan in `native_cpp_code_supported`
(`tools/amberc/main.cpp`) rejects every effectful shape that lacks a complete
direct-native helper and an effect barrier. Supported text output, task
operations, and the `net.http.Client#query` path commit that barrier
before the effect. No mutable object crosses the scalar VM bridge, so native
ivars cannot alias VM state.

Rule for widening native coverage: **an opcode or selector may be added only
if it is side-effect-free, if all of its mutations are confined to disposable
native state, if it commits the effect barrier and cannot bail out throughout
the supported post-effect shape, or after the whole-program restart is
replaced with a resumable stateful native/VM boundary** (see
`docs/engineering/full-native-implementation-plan-v1.md`). Any violation can
show up in this bundle as duplicated output or external mutation.

`corpus/run/native_object_state` pins the confined-state branch: class-valued
calls, user dispatch, ivar reads/writes, `present?`/`absent?`, monotonic time,
shallow/deep collection copy with topology preservation, `init_copy`, and an
in-place array append all compile with `--require-full-native`.

## Direct native-extension leaves

A code object carrying an `amber.native.bind:<code_id>` attribute is classified
as `native-extension` when its native package is linked. Generated callers
marshal arguments through a thread-local runtime host and call
`RuntimeWorld::invoke_native_extension`, which invokes the registered
`amber_ext.h` ABI thunk directly. It does not push, step, or execute the bound
code object's Amber fallback body.

Native-extension code objects count toward `native_graph_native_code_count`,
while `native_graph_vm_fallback_code_count` remains reserved for bytecode
execution. The amber-orm SQLite selftest pins this distinction at 916/916
native code objects: 881 generated C++ bodies, 35 direct SQLite extension
thunks, 0 VM bridges, and 0 fallback objects. The executable returns `29`.
The standalone SQLite selftest is 477/477 native and returns `35`.

When every code object is either `direct-native` or `native-extension`, the
generated source omits `run_vm_entry`, `amber_vm_fallback_call`, and every
`RuntimeWorld::execute` call. A `NativeBailout` is then a native execution error
rather than permission to restart the program under bytecode.

## Direct native stdlib sends

Runtime-owned stdlib objects can cross the generated/native boundary as opaque
`RuntimeHandle` values. `RuntimeWorld::invoke_native_stdlib_send` enters the
same C++ stdlib dispatcher used by the VM without pushing or stepping a
bytecode frame. Scalar, string, Symbol, Bytes, List, and Map arguments are
marshalled into the host world; runtime-owned results retain their runtime
`Value` and ownership.

The first effectful user of this path is HTTP QUERY. Constant lookup
for `net`, `net.http`, and `net.http.Client`, client construction,
`Client#query`, and the response accessors used to inspect or consume the
result are direct-native eligible. The scoped block form invokes its native
closure with the Response and closes the Response on both normal and exceptional
exit. Capability grants are copied into the host world, so the ordinary
per-origin `net.connect` check remains authoritative.
`tests/native_http_query_test.py` requires full native coverage, asserts that
the executable contains no bytecode fallback, and verifies the QUERY request
line, Content-Type, body, status, and response body against a loopback server.

## Per-function VM fallback (step 2, scalar bridge)

Code objects that fail the native allowlist but pass
`native_cpp_code_vm_callable` (`tools/amberc/main.cpp`) no longer doom the
program to the whole-program restart. The generated executable lazily
decodes its embedded bytecode into one `RuntimeWorld` (module init is NOT
run inside it) and executes just that code object per call across a scalar
value bridge.

Soundness rests on three constraints, each enforced where stated:

1. **State isolation (static):** vm-callable code has no captures *at the
   entry*, no `Call`/dynamic sends, and no ivar/cvar access, and every send
   selector is on the bridge allow-list. It can therefore neither observe nor
   mutate state shared with the native lane, and module init need not run in
   the fallback world. The check (`native_vm_callable_code_body` in
   `tools/amberc/main.cpp`) is recursive over the block bodies the entry
   constructs (see the block-support bullet). Two selector families are
   admitted:
   - `native_vm_callable_pure_selector` — effect-free value transforms:
     numeric/comparison/bitwise operators, local conversions and reads, the
     **pure copy-edit collection verbs** (`appended`/`inserted`/`deleted`/
     `init`/`tail`/`take`/`drop`/`sorted`/`reversed`/`min`/`max`/`join`, which
     return new values per vm.cpp RFC §7.1), the **pure Set algebra**
     (`union`/`intersection`/`difference`/`symmetric_difference`/`subset?`/…),
     the **pure string transforms** (`upcase`/`downcase`/`trim`/`split`/
     `replace`/`starts_with?`/`ends_with?`/`chars` — `Str` is immutable), and
     the **pure `Math`-prelude / numeric methods** (`sqrt`/`pow`/`hypot`/
     `floor`/`ceil`/`round`/`sin`/`cos`/`log2`/… — `Math` resolves to a
     built-in value without module init, and these run the same libm as every
     other lane, so the result is bit-identical). `log` is intentionally
     excluded because it is overloaded as the effectful `io.Logger#log`;
     `info`/`warn`/`error`/`debug`/`write`/`puts`/`print` are likewise never
     admitted.
   - `native_vm_callable_local_mutator_selector` — the block-free in-place
     `!`-mutators (`push!`/`insert!`/`sort!`/`reverse!`/`store!`/`merge!`/
     `add!`/…). These are *not* pure, but they are still sound here by
     **reachability**: a vm-callable function takes only scalar arguments
     (constraint 2) and has no captures/upvalues/ivars/closures/`Call`s, so
     every heap value it touches is one it constructed itself in the embedded
     fallback world. Mutating it is invisible to the native lane (separate
     heaps, no aliasing) and discarded after the call, and the function still
     emits no observable output before it can bail.

   **Blocks / higher-order enumerables.** A function may construct block
   closures (`MakeClosure`) and pass them to the pure higher-order enumerables
   (`map`/`flat_map`/`select`/`reject`/`find`/`reduce`/`group`/`take_while`/
   `drop_while`/`any?`/`all?`/`none?` and `sorted`/`min`/`max` with a key
   block). This is sound because (a) each block body is verified bridge-pure by
   the same recursive check — effect-free, no `Call`/dyn-send/ivar — and (b) a
   block body's captures are always enclosing locals of the entry frame (the
   entry itself has no captures, so every capture transitively bottoms out at
   an entry local), which live entirely inside the embedded execution. A
   constructed closure can only be consumed by such a block-send: `Call` is
   rejected, and a closure that escapes via the return value bails at result
   conversion (closures are non-convertible). Keyword-argument sends still
   bail. This admits the block-taking mutators too (`delete_if!`, `map!`, …),
   which remain sound by the same reachability argument as the block-free
   mutators.

   The boundary only constrains the bridged function's *parameters* and
   *return value* (next two constraints); its body runs in the real embedded
   VM, so any value it constructs, transforms, or locally mutates is already
   exact — widening these lists only changes which functions skip the
   whole-program restart, never the computed result.
2. **Bridge immutability (runtime):** arguments must be immutable bridge values
   (null/bool/Int/Float/Uuid). UUIDs cross by copying their 16 bytes; the other
   admitted values are scalars, so the two heaps cannot share mutable state.
   Any other heap-valued argument throws `NativeBailout` before the callee runs.
3. **Restart compatibility (runtime):** faults and results that do not
   convert back to native values throw `NativeBailout`. Convertible results
   are null/bool/Int/Float, strings (re-interned into the native string table
   by content), UUIDs (copied by value), and lists nesting any of those; maps,
   sets, closures, and other heap kinds bail. The restart stays sound because
   vm-callable code produces no observable effect before it bails (constraint
   1), and it reproduces full VM fault traces exactly.

`vm_fallback_code_count` in the build JSON reports how many code objects
took this path. Pinned by `corpus/run/native_vm_fallback_scalar_bridge` (a
float-internal helper called from a native loop),
`corpus/run/native_vm_bridge_pure_collection` (pure copy-edit array verbs +
string transforms), and `corpus/run/native_vm_bridge_local_mutation`
(in-place `!`-mutators on local arrays + Set algebra), and
`corpus/run/native_vm_bridge_block_higher_order` (block closures passed to
`map`/`select`/`reduce`/`sorted`-with-key, capturing an enclosing local), and
`corpus/run/native_vm_bridge_map_block_mutators` (block-taking in-place Map
mutators `select!`/`transform_values!` + pure `merge`/`except`).

Lifting the effect-free restriction for vm-callable functions requires
proving result convertibility statically (so the restart recovery is never
needed after an effect) — that is the next widening step, not this one.

Modules selecting a non-default numeric profile (anything other than
`int: Int64`, `overflow: checked`) are wholly VM-executed in the native lane;
the equivalence bundle covers those fixtures too, exercising the fallback
path.
