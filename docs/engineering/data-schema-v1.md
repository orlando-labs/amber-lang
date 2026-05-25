# amber.data/schema.v1

Status: implemented for the `W11.4` dataframe/schema baseline.

This baseline is metadata-first. `SCMA` carries schema definitions and migration
records. `TABL` carries lazy table/query-plan descriptors, column dependencies,
effect rows, and stable plan fingerprints.

Schemas are validated by name/version. Field types are canonicalized across
common aliases such as `int -> integer`, `str -> string`, and
`boolean -> bool`. Compatible migrations may not remove fields, change existing
field types, or add required non-nullable fields without a default.

Table plans normalize their dependencies and effect rows, then derive a stable
SHA-256 fingerprint from the plan id, operation, ordered inputs/arguments,
column dependencies, effect row, and flags. This gives notebook/watch tooling a
stable query-plan identity without requiring a full table runtime yet.

CLI:

```text
amberc schema-check schema.amberschema
amberc table-explain query.ambertable
```

Both commands use deterministic line-oriented profile documents and emit
machine-readable JSON. `schema-check` can validate example records against a
schema. `table-explain` validates table plans and emits their fingerprints.

Runtime worlds expose `schema_validation()`, `table_plan_validation()`, and the
same metadata through read-only package mirrors.
