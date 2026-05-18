# amber.typed.v1

Status: `W9.1` optional Amber/Typed checker lane is implemented.

The typed profile is intentionally separate from the dynamic M5 gate. Dynamic
Amber still compiles/runs through the existing lexer, parser, binder, HIR,
bytecode, VM, and loader path; `amberc typed` adds a static profile pass that
can fail without changing dynamic semantics.

Implemented surface:

- parser support for `def ...(...) -> TypeTerm:` return boundaries;
- parameter `as TypeTerm` capture with nested generic/record punctuation;
- `TypeTerm` canonicalization for named, optional, union, tuple, record, and
  invariant generic terms;
- exported callable checks requiring parameter and return annotations;
- statically obvious parameter default and return-boundary mismatch
  diagnostics;
- basic truthiness flow for `and` / `or` return inference;
- strict `case!` exhaustiveness for boolean subjects;
- bytecode `BcMethod.type_hook_ids[]` metadata for annotated parameter/return
  boundaries.

Tooling:

```sh
amberc typed <file>
ambertest run corpus --bundle M6
```

Diagnostic codes:

- `T0001`: exported callable parameter is missing a `TypeTerm`;
- `T0002`: exported callable is missing a return `TypeTerm`;
- `T0003`: malformed `TypeTerm`;
- `T0004`: default value fails a statically provable parameter boundary;
- `T0005`: inferred return fails a statically provable return boundary;
- `T0006`: `case!` is not statically exhaustive in typed profile.

Current limits:

- package-level typed mode is represented by invoking `amberc typed`; package
  manifest/profile selection belongs to `W9.4`;
- reflective `Any` boundaries are modeled conservatively as `Any` inference;
- generic terms are invariant by canonical equality, while runtime deep generic
  checks remain future stdlib/runtime work;
- pattern narrowing beyond boolean `case!` exhaustiveness belongs to the next
  typed-flow expansion.
