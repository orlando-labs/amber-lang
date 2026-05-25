# amber.replay.v1

Status: implemented for the `W11.3` observability/replay baseline.

The profile defines canonical semantic event names such as `task.started`,
`task.completed`, `capability.check`, `capability.denied`, `effect.boundary`,
`loader.module.load`, `world.freeze`, and `world.mutation`. Vendor event names
remain valid when they use a dotted multi-component namespace.

`.amberbc` has optional profile metadata:

- `OBSV` records observability site rows with event name, kind, owner, source,
  and flags.
- `RPLY` records required replay event names, deterministic source categories,
  and deterministic-mode flags.

Runtime hosts can enable `RuntimeWorldOptions::record_replay_trace` to collect a
stable `amber.replay.v1` trace. The recorder assigns deterministic event ids and
virtual timestamps, captures capability/effect/freeze/load boundaries, and
serializes traces as a reproducible `.ambertrace` envelope with a content
digest.

Runtime hosts can also set `RuntimeWorldOptions::enforce_replay` with an
expected trace. Recorded events are compared in order; the first mismatch
produces `ReplayDivergenceError`. The current reference baseline does not yet
virtualize all host I/O or scheduler decisions, but it establishes the stable
trace format and runtime comparison path used by later scheduler/workflow work.

The CLI preflights are:

```sh
amberc replay-check run.ambertrace
amberc trace-inspect run.ambertrace
```

`replay-check` validates the envelope and event stream. `trace-inspect` emits
deterministic JSON for tooling and golden tests.
