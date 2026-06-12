# amber.mir.v1

Status: **demoted to validation/diagnostic artifact** (decision 2026-06-12,
see below). Originally implemented for the `W10.3` MIR/SSA baseline.

## Decision: MIR is a validation artifact, not a codegen input

Recorded 2026-06-12 during the native-backend phase (research plan §5.6,
work item 15). The facts that drove it:

- the production native lane (`cpp-bytecode-direct-v1` in
  `tools/amberc/main.cpp`) transpiles **bytecode** directly and is the only
  lane that ships executables; it never reads MIR;
- `run_pass_pipeline` has no callers outside `tests/mir_tests.cpp` — no
  optimization pass has ever run in the execution flow;
- MIR's string-encoded SSA (`%vN` names, string opcodes) would need a full
  numeric re-encoding before an optimizing backend could consume it;
- a MIR-input codegen path alongside the bytecode-direct backend would be a
  third lowering whose semantics nothing verifies (the backend-equivalence
  gate covers bytecode-vs-native only).

Consequences:

- MIR remains as: HIR lowering sanity validator, deterministic dump format
  for diagnostics (`amberc mir/mir-dump/mir-verify`), and the input for
  `amber.native.v1` *metadata* (eligibility/trampoline descriptors).
- No new consumers of MIR may be added to execution or codegen paths. If a
  future optimizing backend needs an IR, it starts from bytecode (the
  verified, executed format) or introduces a numerically-encoded IR with its
  own conformance gate — it must not grow out of `amber.mir.v1`.
- The pass harness stays only as long as its tests do; it documents the
  schema's invalidation contract, nothing more.

The MIR layer is a deterministic optimizer-facing IR between HIR and the
`amber.native.v1` metadata layer. It does not participate in the execution
path.

W10.4 consumes this layer through `amber.native.v1`; see
[native-v1.md](../../docs/engineering/native-v1.md:1)
for the native/JIT metadata and frozen runtime bridge.

Implemented surface:

- typed MIR schema in [optimizer/mir.h](../../optimizer/mir.h:1)
- HIR-to-MIR lowering, validator, deterministic JSON/text dumps, and pass
  harness in [optimizer/mir.cpp](../../optimizer/mir.cpp:1)
- CLI inspection in [tools/amberc/main.cpp](../../tools/amberc/main.cpp:1):
  - `amberc mir <file>`
  - `amberc mir-dump <file>`
  - `amberc mir-verify <file>`

## IR Shape

Top-level JSON format is `amber.mir.v1`:

- `module`: package name or `null`;
- `functions[]`: one MIR function per HIR procedure;
- `passes[]`: deterministic pass records appended by the pass harness;
- `source_hash`: source SHA-256 for dump stability.

Each function contains:

- stable procedure identity: `id`, `name`, `kind`, `owner`;
- `entry` block id;
- copied local/capture layout metadata from HIR;
- basic blocks with instruction arrays and one terminator.

Instruction results are SSA values named `%vN`. Local and capture slots remain
explicit storage operands (`local(lN)`, `capture(uN)`) so the initial MIR can
represent imperative Amber semantics without inventing incomplete memory-SSA
proofs. Expression values, branch conditions, send results, closure values, and
phi results are SSA.

Current node families:

- constants and lookup: `const`, `name.lookup`, `const.lookup`;
- storage: `local.load/store`, `capture.load/store`, `ivar.load/store`,
  `cvar.load/store`, `last.get/set`;
- calls and dispatch: `send`, `send.dynamic`, `call`, `keyword.arg`,
  `closure.make`;
- control flow: `branch_if`, `jump`, `return`, `phi`, `safepoint`;
- matcher bridge: `match.dispatch`, `pattern.assign`;
- fallback: `unsupported` with the original HIR node kind.

## SSA Validator

The validator checks the executable MIR contract:

- function entry block exists;
- block ids and SSA definitions are unique;
- every block has a terminator;
- SSA operands reference defined values;
- local/capture operands reference declared slots;
- `phi` nodes are at the top of a block and use block/value operand pairs;
- terminators have the expected operand and target shape;
- branch and phi targets reference existing blocks.

Representative verifier codes:

- `MIR1001` missing function entry block
- `MIR1003` block without terminator
- `MIR1004` duplicate SSA definition
- `MIR1005` undefined SSA operand
- `MIR1008` invalid phi shape
- `MIR1010` unknown block target

## Pass Harness

`run_pass_pipeline(...)` records each pass as `{name, phase_order,
invalidates}` and enforces:

- non-empty pass names;
- monotonic `phase_order`;
- optional validation before a pass that requires valid SSA;
- optional validation after a pass that promises to preserve SSA;
- explicit rejection of passes that both invalidate and preserve SSA.

Invalidation bits are:

- `kInvalidatesControlFlow`
- `kInvalidatesSsa`
- `kInvalidatesAnalyses`

The current harness is intentionally small. It provides the phase-ordering and
post-pass validation boundary needed before optimization passes can become
observable.
