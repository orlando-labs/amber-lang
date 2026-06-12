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

1. **State isolation (static):** vm-callable code has no captures, no
   closure creation, no `Call`/dynamic sends, no ivar/cvar access, and only
   selectors from a pure compute allow-list. It can therefore neither
   observe nor mutate state shared with the native lane, and module init
   need not run in the fallback world.
2. **Bridge immutability (runtime):** all arguments must be scalars
   (null/bool/Int); scalars are immutable, so the copy across the value
   bridge cannot diverge. Any heap-valued argument throws `NativeBailout`
   before the callee runs.
3. **Restart compatibility (runtime):** faults and results that do not
   convert back to native values (anything beyond null/bool/Int and lists
   thereof) throw `NativeBailout`. The restart stays sound because
   vm-callable code is effect-free by constraint 1, and it reproduces full
   VM fault traces exactly.

`vm_fallback_code_count` in the build JSON reports how many code objects
took this path; `corpus/run/native_vm_fallback_scalar_bridge` pins the
behavior (a float-internal helper called from a native loop).

Lifting the effect-free restriction for vm-callable functions requires
proving result convertibility statically (so the restart recovery is never
needed after an effect) — that is the next widening step, not this one.

Modules selecting a non-default numeric profile (anything other than
`int: Int64`, `overflow: checked`) are wholly VM-executed in the native lane;
the equivalence bundle covers those fixtures too, exercising the fallback
path.
