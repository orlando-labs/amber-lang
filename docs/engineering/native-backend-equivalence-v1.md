# amber.native-backend-equivalence.v1

Status: active guard; run via `make backend-equivalence`
(`tools/backend_equivalence.py`), wired into CI after `make conformance`.

## What it checks

Every `corpus/run` fixture is compiled into two executables with
`amberc build`:

- `--target bytecode-wrapper`: the VM lane (the semantic oracle);
- `--target native`: the `cpp-bytecode-direct-v1` lane, which runs generated
  C++ for eligible code objects and falls back to the VM through the
  whole-program `NativeBailout` restart.

Both executables must produce byte-identical stdout, stderr, and exit codes.
Fixtures whose `meta.json` entry is a callable are driven by appending an
entry call to module init, so the executable's printed init result is the
entry result. Fault fixtures (e.g. `OverflowError`, `ZeroDivisionError`)
compare the full trace output and the nonzero exit code.

## The bailout/restart soundness invariant

The native lane handles anything it cannot execute (including checked-Int
overflow, which it detects with `__builtin_*_overflow` helpers) by throwing
`NativeBailout`; the generated `main()` catches it and re-runs the entire
program, including module init, under the VM.

This is observably equivalent **only while native-eligible code performs no
observable side effects before a bailout**. Today that invariant is enforced
structurally: the eligibility scan in `native_cpp_code_supported`
(`tools/amberc/main.cpp`) is a strict allowlist of side-effect-free opcodes
and selectors (scalar/int ops, list construction and access, closures, calls,
jumps, returns). Output selectors (`print`/`p`/`pp`), IO, channels, ivar
stores on shared state, and every other effectful operation make the whole
code object ineligible.

Rule for widening native coverage: **an opcode or selector may be added to
the allowlist only if it is observably side-effect-free, or only after the
whole-program restart is replaced with per-function VM fallback** (see
`docs/engineering/full-native-implementation-plan-v1.md`). Any violation
shows up in this bundle as duplicated side effects in the native lane's
output.

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
2. **Bridge immutability (runtime):** all arguments must be scalars
   (null/bool/Int/Float); scalars are immutable, so the copy across the
   value bridge cannot diverge. Any heap-valued argument throws
   `NativeBailout` before the callee runs.
3. **Restart compatibility (runtime):** faults and results that do not
   convert back to native values throw `NativeBailout`. Convertible results
   are null/bool/Int/Float, strings (re-interned into the native string table
   by content), and lists nesting any of those; maps, sets, closures, and
   other heap kinds bail. The restart stays sound because vm-callable code
   produces no observable effect before it bails (constraint 1), and it
   reproduces full VM fault traces exactly.

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
