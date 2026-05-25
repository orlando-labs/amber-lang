# amber.modern-profiles.v1

Status: implemented for the `W11.6` AI-agent tooling, contracts/property,
privacy/lineage, and durable workflow metadata baseline.

`AGNT` carries semantic symbols, structured patch operations, and provenance
records. `symbols` emits a deterministic symbol graph from the current binder
contract; `explain` resolves a source position against that graph; `patch-check`
and `provenance-audit` validate stale symbol references, tool/request digests,
patch capabilities, and recorded checks.

`CNTR` carries `require`/`ensure`/`invariant` descriptors and property-test
metadata. The validator requires stable owners, contract expressions, property
seeds, generator descriptions, profile sets, and valid effect rows.

`PRIV` carries data labels, export policy rules, and lineage nodes. Labels are
normalized, schema/table/export lineage can reference them, and public export
lineage is rejected unless policy explicitly allows or redacts the label. A
deny rule produces `PolicyViolationError` metadata.

`WFLW` carries workflow step descriptors and append-only history events. The
validator checks effect rows, step identity, committed idempotency keys, and
conflicting re-execution of an already committed idempotency key.

Runtime worlds expose these sections through `agent_validation()`,
`contract_validation()`, `privacy_validation()`, `workflow_validation()`, and
the read-only package mirror. `.amberbc` verification maps profile failures to
`BC1410`-`BC1413`.
