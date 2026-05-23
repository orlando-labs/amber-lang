# amber.effects.v1

Status: implemented for the `W11.2` effects baseline.

Callable signatures can declare effect rows:

```amber
def normalize(row as Row) -> Row !{}:
  row

def fetch(id as UserId) -> User !{net, async}:
  http.get(id)
```

The parser records `!{...}` on signatures, the binder preserves it, and the
typed/effects checker emits deterministic `amber.effects.v1` summaries. Empty
`!{}` is the pure boundary. Canonical labels include `alloc`, `mut`, `world`,
`watch`, `async`, `strand`, `fs`, `net`, `env`, `time`, `random`, `ffi`,
`reflect`, `unsafe`, `db`, `gpu`, `schema`, `trace`, and `workflow`.

The checker validates `observed subset_of declared`. Static mutation produces
`mut`; conservative host-call heuristics recognize common roots such as
`File`, `http`, `clock`, `Random`, `Env`, `DB`, `gpu`, `trace`, and
`workflow`; dynamic `send(...)` produces `reflect` and non-literal selectors
also produce `unsafe`. Violations are reported as `FX0003`.

`.amberbc` has an optional `EFCT` section carrying effect summaries per
callable. Package, frozen-image, reload, and runtime mirror paths preserve the
section. Runtime hosts can call `check_effects(...)` with an enforced
allow-list; disallowed effects produce `EffectViolationError`.

The CLI preflight is:

```sh
amberc effects-check src/core.am
```

It emits `amber.effects.v1` JSON with declared/observed rows and effect
diagnostics.
