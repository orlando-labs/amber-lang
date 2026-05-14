# amber.diag.v1

Status: seed contract.

Diagnostics use deterministic JSON with:

- `code`;
- `severity`;
- `phase`;
- `message`;
- `primary_span`;
- `related`;
- `notes`.

Compiler diagnostics are sorted by primary span, then code. The initial registry lives in `spec/registries/diagnostics.yaml`.
