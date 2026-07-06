# DESIGN: `Signal` stdlib module — handling and propagating process signals

Status: proposed design
Date: 2026-07-04
Target: Amber standard library `Signal` module + runtime signal hub
Scope: observing POSIX signals delivered to the Amber process, running
handlers as ordinary scheduler tasks, disposition control (ignore/default),
correct propagation of fatal signals (exit status via re-raise), self-delivery
for coordination and tests, capability/replay integration
Out of scope for v1: sending signals to *other* processes (future `process`
module), child-process reaping policy around `SIGCHLD`, `fork`/`exec`
disposition inheritance, realtime signals (`SIGRTMIN..`), per-thread signals,
non-POSIX hosts (Windows), embedding hosts that install their own handlers

## 1. Goals

Signals are the one asynchronous input channel every long-running process has
whether it wants it or not. Today the Amber runtime installs no handlers: an
untrapped `SIGINT` kills a server mid-request, and there is no way to express
"drain connections, then die with the right exit status".

The module must deliver four things:

1. **Safety.** User code never runs in async-signal context. A signal becomes
   an ordinary value delivered through the cooperative scheduler; handlers are
   ordinary tasks that may allocate, do IO, use channels, and suspend.
2. **Composability.** Signal arrival is just another event source: it must
   plug into the existing `Channel`/`select:` machinery rather than invent a
   parallel wait primitive.
3. **Correct propagation.** A process that catches `SIGTERM` to clean up must
   still be able to die *by* `SIGTERM` (`WIFSIGNALED`), not exit 0 or exit 1.
   This is the "propagate after cleanup" contract that supervisors (systemd,
   Kubernetes, foreman) rely on.
4. **Explicit effects.** Trapping and delivering signals mutate process-global
   state and introduce host nondeterminism. Both go through the capability
   model and the replay/trace story, like entropy and `TimeZone.local`.

The surface keeps Amber stdlib conventions for singleton native modules:
capitalized module namespace (`import Signal`, like `Json`, `Time`,
`SecureRandom`, `Benchmark`), lower_snake_case selectors, no `!` forms (the
module mutates process state, not a receiver value), explicit errors from the
shared registry, and text-equivalent `Symbol`/`Str` inputs.

## 2. Model

Three concepts, deliberately separated:

```text
designator     what the caller names:      :term | "TERM" | "SIGTERM" | 15 | a Signal.Event
Signal.Event   immutable event value:      name, number, count, generation
Signal.Source  per-subscriber event queue: channel-contract recv/close, select-compatible
```

And one process-global registry the runtime owns:

```text
disposition(sig) = :default | :ignore | :handled
```

- `:default` — the OS default applies (terminate, stop, or ignore, per
  signal). The runtime has no hook installed.
- `:ignore` — the runtime installed `SIG_IGN`-equivalent; arrivals are
  discarded.
- `:handled` — the runtime consumes the signal and dispatches a
  `Signal.Event` to every live `Signal.Source` watching it.

The runtime has managed state for a signal while at least one consumer (a
`watch` source, a `trap`, or an in-progress `wait`) exists for it, or
`Signal.ignore` / `Signal.default` pinned a base disposition explicitly. The
effective disposition is `:handled` only while at least one live consumer
exists. When the last consumer disappears the runtime restores the disposition
that `Signal.ignore`/`Signal.default` pinned, or the process-start disposition
if none was pinned.

### 2.1 Signal designators

Everywhere the API takes a signal it accepts:

- a `Symbol`: `:term`, `:int`, `:usr1` (canonical spelling in docs/examples);
- a `Str`: `"TERM"`, `"term"`, `"SIGTERM"` (case-insensitive, optional `SIG`
  prefix);
- an `Int` number (accepted for interop, discouraged: numbers vary by
  platform);
- a `Signal.Event` event value (so `Signal.propagate(sig)` works directly on
  what a handler received).

Unknown or unsupported designators raise `SignalLookupError`. Untrappable and
runtime-reserved signals raise `SignalControlError` from any consuming or
disposition-changing call (§3).

### 2.2 The `Signal.Event` event value

```amber
sig.name        # "TERM"        canonical upper-case name, no SIG prefix
sig.symbol      # :term
sig.number      # 15            host value, informative
sig.count       # 3             coalesced deliveries folded into this event (§6)
sig.generation  # 42            runtime subscription generation, introspection/debug
sig.to_str      # "SIGTERM"
sig.inspect     # "Signal.Event(TERM x3)"
```

`Signal.Event` is immutable and shareable across strands. Equality is by signal
identity (`name`), ignoring `count` and `generation`. Portable JSON encoding
preserves coalescing metadata and intentionally omits the runtime-local
`generation`:

```json
{"signal":"SIGTERM","name":"TERM","symbol":"term","number":15,"count":3}
```

The lossy string form remains explicit through `to_str`.

## 3. Portable signal set

v1 specifies behavior for the portable POSIX set. `Signal.supported` reports
what the host actually offers; conforming programs match on names, never
numbers.

| Name | OS default | v1 class |
| --- | --- | --- |
| `HUP` `INT` `QUIT` `TERM` `USR1` `USR2` `ALRM` `PIPE` | terminate | trappable |
| `TSTP` `TTIN` `TTOU` | stop | trappable |
| `CHLD` `WINCH` `URG` `CONT` | ignore/continue | trappable |
| `KILL` `STOP` | terminate/stop | untrappable — `SignalControlError` |
| `SEGV` `BUS` `FPE` `ILL` `TRAP` `ABRT` `SYS` | core | runtime-reserved — `SignalControlError` |
| `VTALRM` `PROF` | terminate | runtime-reserved (future sampling profiler) |

Runtime-reserved synchronous faults stay terminal VM faults: a program that
dereferences bad native memory must not be able to observe its own `SIGSEGV`
as a friendly event. `Signal.reserved` lists the reserved set so tooling can
explain refusals.

`CHLD` is trappable in v1 but documented as "observe only": the future
`process` module will own reaping, and its design must reconcile with any user
`CHLD` watchers (recorded here as a forward obligation).

## 4. Module surface

```amber
import Signal
from Signal import Source, Event   # types, rarely needed by name
```

### 4.1 `Signal.watch` — the primitive

```amber
source = Signal.watch(:int, :term)

sig = source.recv()                # parks the task until a signal arrives
sig.name                           # "INT" or "TERM"
source.close()
```

`Signal.watch(*sigs)` subscribes to one or more signals and returns a
`Signal.Source`. Each source is an independent subscriber: when a watched
signal arrives, **every**
live source watching it gets its own event (broadcast), and within one source
competing `recv()` callers get exactly one event each (channel semantics).

`Source` presents the channel receiving contract so it composes with
everything channels already work with:

```amber
source.recv()                      # blocks cooperatively; Signal.Event
source.recv(timeout: 5.0)          # TimeoutError on expiry
source.close()                     # idempotent; unsubscribes
source.closed?()
source.signals                     # [:int, :term]
source.pending?()                  # true if recv() would not block
```

- `recv()` on a closed, drained source raises `ChannelClosedError` — the same
  error channels raise, deliberately, so generic consumer loops need no
  special case.
- A `Source` is shareable across strands (it is runtime-synchronized, like
  `Channel`); it is not strand-confined and needs no `adopt!`.
- Sources are select-compatible receive arms:

```amber
select:
when job = jobs.recv():
  handle(job)
when sig = signals.recv():
  drain_and_exit(sig)
timeout 30.0:
  heartbeat()
```

Watching a signal flips its disposition to `:handled` (the runtime consumes
it; the OS default no longer fires). Closing the last source for a signal
reverts the disposition (§2). This mirrors Go's `signal.Notify` and is the
price of observation — a program that watches `SIGTERM` and never acts on it
has opted out of dying by `SIGTERM`. `Signal.propagate` (§4.5) is the
sanctioned way back.

### 4.2 `Signal.trap` — callback sugar

```amber
trap = Signal.trap(:term) |sig|:
  server.drain()
  Signal.propagate(sig)
```

`trap(sig)` with a required block is sugar over a dedicated source plus a
hidden dispatcher task:

- The dispatcher runs handler invocations **sequentially, in arrival order**
  for that trap: one at a time, each invocation as its own task run. A slow
  handler coalesces further arrivals into the next event's `count` rather
  than piling up concurrent handler tasks. Programs that want concurrent
  handling use `watch` and spawn their own tasks.
- The handler is ordinary task code: it may suspend, do IO, use channels.
  There is no async-signal-context restriction to teach.
- An uncaught exception in a handler is a runtime fault that terminates the
  program, naming the trap and signal (fail-fast, consistent with structured
  concurrency). Resilient handlers `rescue` internally.
- Multiple traps may coexist on the same signal; each is an independent
  subscriber (unlike Ruby's last-wins `Signal.trap`). A trap does not
  overwrite another trap.

The returned `Trap` handle:

```amber
trap.close()                       # cancel dispatcher, unsubscribe; idempotent
trap.active?()
trap.signal                        # :term
```

`close()` requests cooperative cancellation of the dispatcher; an in-flight
handler invocation observes cancellation at its next suspension point, per
normal task cancellation rules.

### 4.3 `Signal.wait` — one-shot

```amber
sig = Signal.wait(:int, :term)             # park until one arrives
sig = Signal.wait(:usr1, timeout: 10.0)    # TimeoutError on expiry
```

Sugar for `watch` + `recv` + `close`. This is the main-function idiom for
servers:

```amber
def main():
  server = start_server()
  sig = Signal.wait(:int, :term)
  server.drain()
  Signal.propagate(sig)
```

### 4.4 Disposition control

```amber
Signal.ignore(:pipe)               # pin :ignore
Signal.default(:hup)               # pin :default (OS behavior)
Signal.disposition(:term)          # -> :default | :ignore | :handled
```

`ignore`/`default` pin a base disposition that applies **whenever no
consumer exists** for that signal. Live watchers/traps still win while they
exist (`:handled`); the pin is what the disposition reverts to afterwards.
Pinning while consumers exist does not disturb them.

Both raise `SignalControlError` for untrappable/reserved signals. There is no
Ruby-style `trap(sig, "IGNORE")` string mode: disposition words get dedicated
verbs, handlers get blocks, and the two never share a parameter.

### 4.5 `Signal.propagate` — die with the right status

```amber
Signal.propagate(:term)            # restore default, re-deliver to self
Signal.propagate(sig)              # same, from a Signal.Event
```

Sequence: atomically detach the runtime's hook for the signal (restoring the
OS default disposition, `SIG_DFL`, not the inherited process-start action),
then re-deliver the signal to the own process. For terminating signals this
call **does not return**; the process dies signaled, so parents observe
`WIFSIGNALED`/`WTERMSIG` — not an imposter exit code. This is true even if the
process inherited `SIG_IGN` for that signal (for example under `nohup`): normal
disposition restoration preserves inherited ignore, but `propagate` means
"perform cleanup and then die honestly by this signal".

For signals whose OS default does not terminate (`CHLD`, `WINCH`, `CONT`, and
stop signals after a subsequent `CONT`), `propagate` returns after re-delivery
and re-installs the runtime state that existed before the call (consumers keep
working). Documented, but the intended use is the fatal case.

Runtime shutdown before re-delivery: `propagate` performs the same flush the
normal exit path performs (flush logical stdout/stderr); it does **not** run
graceful VM teardown — cleanup belongs *before* the call, in the handler.
That asymmetry is the point of the verb.

### 4.6 `Signal.deliver` — self-delivery

```amber
Signal.deliver(:usr1)
```

Sends the signal to the own process (`kill(getpid(), n)`), without touching
dispositions. This is the coordination/testing primitive: corpus tests raise
`:usr1`/`:usr2` at themselves and assert watcher behavior. Delivery to other
pids is explicitly excluded from v1 and reserved for the `process` module.

`deliver` of untrappable (`KILL`/`STOP`) or reserved signals raises
`SignalControlError`. `Signal.propagate` is for re-delivering a trappable signal
after cleanup; it is not a back door for sending runtime-reserved faults to the
process.

### 4.7 Introspection

```amber
Signal.supported                   # [:hup, :int, :quit, :term, ...] on this host
Signal.reserved                    # [:segv, :bus, :fpe, :ill, :trap, :abrt, :sys, :vtalrm, :prof]
Signal.number(:term)               # 15
Signal.name(15)                    # "TERM"
```

Introspection is pure, needs no capability, and is safe under replay.

## 5. Disposition precedence (normative)

For each signal, in order:

1. Untrappable/reserved → all consuming and disposition calls raise
   `SignalControlError`; runtime behavior is unspecified stdlib-wise.
2. ≥1 live consumer (source, trap, in-progress wait) → `:handled`: the
   runtime consumes arrivals and broadcasts events. The OS default never
   fires.
3. No consumer, pinned by `ignore`/`default` → the pinned disposition.
4. Otherwise → the disposition the process started with (normally OS
   default; inherited `SIG_IGN` for e.g. `HUP` under `nohup` is preserved).

An arrival while `:handled` but with zero *ready* receivers is queued
per-source (coalescing, §6). An arrival in the gap where the last consumer
closed but the hook is not yet detached is discarded — indistinguishable from
OS-level coalescing, and therefore permitted.

Every transition into `:handled` creates a new per-signal generation. Captured
arrivals carry that generation; a wake drained after a later close/re-open is
stale and must be counted as a drop, not delivered to the new source set.

## 6. Coalescing and queue semantics (normative)

POSIX already coalesces standard signals (a pending signal delivered twice
before the process runs arrives once). The stdlib model embraces this instead
of pretending signals form a lossless stream:

- Per source, per signal, at most **one** pending event. A new arrival while
  one is pending increments that event's `count` instead of growing a queue.
- `recv()` dequeues pending events in FIFO order of their *first* pending
  arrival across the source's signals.
- Consequences: no capacity parameter, no overflow policy, no unbounded memory.
  For a source that remains live throughout an active handled generation, a
  burst of N deliveries for one standard signal is observed as at least one
  event whose `count`s sum to a lower bound ≤ N. Arrivals can still be coalesced
  by the OS before Amber sees them, and arrivals in an allowed close/detach gap
  can be dropped (§5).

This makes `Source` strictly weaker than `Channel` on purpose. Code that
needs a durable work queue should have the trap handler `send` into a real
`Channel` and own the overflow policy explicitly.

## 7. Execution and shutdown semantics

- Dispatch happens on scheduler workers via the normal park/resume path: the
  runtime signal hub completes waiting `recv()`s exactly the way channel
  sends complete parked receivers. Handler code observes no special context.
- Ordering across *different* sources is unspecified; ordering within one
  source is FIFO (§6); ordering within one trap is sequential (§4.2).
- Live watchers/traps do **not** keep the program alive. When `main` returns,
  normal structured-concurrency teardown cancels dispatcher tasks and closes
  sources; a task blocked in `recv()` gets `ChannelClosedError` (or its
  cancellation, whichever lands first, per existing task rules).
- During VM shutdown initiated by an *unhandled* fatal signal (disposition
  `:default`), the runtime does nothing — the OS terminates the process; no
  Amber code runs. Cleanup-on-signal exists only if the program asked for it.

## 8. `SIGPIPE` policy

Socket IO already suppresses `SIGPIPE` per-fd (`SO_NOSIGPIPE` /
`MSG_NOSIGNAL` in `runtime/io.cpp`), surfacing `EPIPE` as a rescuable IO
error. Writes to broken non-socket pipes (e.g. stdout into a dead pager) can
still raise it and kill the process — which is the correct *default* for CLI
filters (`amber ... | head`).

v1 therefore does **not** globally ignore `SIGPIPE`. Long-running programs
that prefer `EPIPE` errors opt in with one line:

```amber
Signal.ignore(:pipe)
```

## 9. Capability, effects, determinism

One existing canonical capability, with narrow targets:

- `process.signal` target `manage` — required by `watch`, `trap`, `wait`,
  `ignore`, and `default`. Denied → `CapabilityError` before any process state
  changes.
- `process.signal` target `deliver` — required by `deliver` and `propagate`.

This reuses the implemented capability taxonomy (`process.signal`) instead of
adding new `signal.*` names. A host may grant `process.signal=*` for both
targets, or grant only `manage`/`deliver`.

Effect rows use the existing canonical effect labels:

- `watch`/`trap`/`wait`/`ignore`/`default`/`deliver`/`propagate` observe
  `!{world}` because they mutate process-global disposition state or deliver a
  process-global event.
- `Source.recv`, `Signal.wait`, and trap dispatch also observe `!{async}` at
  the suspension/scheduler boundary.
- Introspection (`supported`, `reserved`, `number`, `name`, `disposition`) is
  pure except that `disposition` requires no effect beyond reading runtime
  state.

Replay/trace: add the `signal` event family to the observability set (spec
currently lists task, strand, channel, …, capability, effect):

```text
signal.arrive        {name, count, generation, seq}       host input — recorded
signal.dispatch      {name, count, generation, source}    derived
signal.disposition   {name, from, to, generation}         derived
signal.deliver       {name}                               effect
signal.propagate     {name}                               effect (usually final)
```

These names must be added to `amber.replay.v1`'s canonical event set; they are
not vendor events.

`signal.arrive` is host nondeterminism of the same kind as entropy and wall
clocks: the record/replay profile records arrivals with their scheduler
sequencing and re-injects them on replay. Strict deterministic mode denies
`process.signal` entirely (both `manage` and `deliver`) before any disposition
change or self-delivery occurs. `deliver` to self is deterministic in intent,
but its *arrival* is still scheduled by the host, so replay records both
`signal.deliver` and the resulting `signal.arrive`; enforced replay injects the
arrival from the trace rather than calling `kill(getpid(), n)` again.

## 10. Errors

Registry additions (`spec/registries/runtime_errors.yaml`):

```yaml
- name: SignalError
  parent: Exception
- name: SignalLookupError      # unknown/unsupported designator
  parent: SignalError
- name: SignalControlError     # untrappable or runtime-reserved signal
  parent: SignalError
```

Reused, not duplicated: `CapabilityError` (denied capability),
`TimeoutError` (`recv(timeout:)`, `wait(timeout:)`), `ChannelClosedError`
(recv on closed source), `CancelledError` (task cancellation during recv),
`ArgumentError` (malformed calls, e.g. `trap` without a block).

## 11. Runtime implementation notes

New translation unit `runtime/stdlib_signal.cpp` registered through the
Layer 0 stdlib registry (`stdlib_registry.h`), plus a small runtime-side
`SignalHub` (likely `runtime/signal_hub.{h,cpp}`) the stdlib unit fronts.

### 11.1 Capture: sigaction + self-pipe (reference backend)

- Lazily, at the first managed-consumer call touching a signal (`watch`,
  `trap`, or `wait`), the hub installs one process-wide `sigaction` handler for
  that signal (`SA_RESTART`), saving the prior action for restoration.
  `Signal.deliver` alone does not install a hook; it sends to the process under
  whatever disposition is currently effective.
- The handler body is the async-signal-safe minimum. It reads the current
  per-signal generation from async-signal-safe storage, sets a per-signal
  pending flag tagged with that generation, then writes a small
  `{signo, generation}` wake record to the hub pipe. The wake record is kept
  below `PIPE_BUF`, so each successful write is atomic. `EAGAIN` is ignored:
  the pending flag preserves the signal identity even when the pipe is full,
  though extra count precision may be lost.
- Handler-based capture works regardless of which thread the kernel picks, so
  **no thread-mask choreography is needed** — workers and the reactor thread
  need no `pthread_sigmask` coordination, and lazy installation after threads
  exist is sound. This is the decisive argument for self-pipe over
  `signalfd` (which requires blocking the signal in every thread, i.e. mask
  discipline before thread spawn) as the *reference* backend.
- The hub registers the pipe read end with `RuntimeReactor::wait_async` in a
  re-arming loop: on readable, drain wake records, tally counts by
  `(signo, generation)`, snapshot and clear matching pending flags, then under
  the hub lock fold current-generation arrivals into each subscribed source's
  pending map. Counts are `max(drained_records, pending_flag ? 1 : 0)` for that
  generation. Stale-generation records are ignored and counted as drops.
  Completion of parked receivers uses the existing channel-style resume path.
  The completion runs on the reactor thread and stays within the documented
  wait_async constraints (cheap, non-blocking, no VM code).
- Permitted alternative backends behind the same hub interface:
  `EVFILT_SIGNAL` on the reactor kqueue (macOS), `signalfd` (Linux, only if
  the runtime later adopts startup masking). Conformance is defined by
  observable semantics, not backend.

### 11.2 Disposition bookkeeping

- Per signal: saved original `sigaction`, pinned base disposition
  (`ignore`/`default`/none), consumer refcount, current handled generation, and
  pending/drop counters. Transitions per §5; all under the hub mutex. The
  `sigaction` syscalls happen outside the handler on the calling task's thread.
  Detaching the last consumer increments the generation and clears the pending
  flag for that signal so late pipe records cannot be delivered to a future
  watcher.
- `propagate`: under the hub lock snapshot current hub state, mark the signal
  detached, increment its generation, clear its pending flag, and install
  `SIG_DFL` (always the OS default, never the saved original action). Flush
  logical stdout/stderr, then `kill(getpid(), n)`. If control returns because
  the default action is non-fatal, re-install the snapshot with a fresh
  generation and return.

### 11.3 Testing hooks

- `SignalHub::stats()` (arrivals, dispatches, coalesced counts, drops in the
  close gap) mirroring `ReactorStats`, for `tests/stdlib_signal_tests.cpp`.
- Exit-status assertions (`WIFSIGNALED` after `propagate`) need
  process-level tests that spawn the CLI, alongside the native e2e harness
  rather than in-process corpus fixtures.

## 12. Conformance / DoD

Corpus (in-process, driven by `Signal.deliver(:usr1)`/`(:usr2)`):

- trap fires with correct `name`/`symbol`/`number`; sequential dispatch order
  under a burst; `count` coalescing observed with a deliberately slow handler;
- two watchers on one signal both receive (broadcast); two receivers on one
  source split events (channel semantics);
- `select:` over a job channel arm and a source arm; timeout arm;
- `recv(timeout:)` raises `TimeoutError`; closed-source `recv` raises
  `ChannelClosedError`; `Signal.watch(:kill)` raises `SignalControlError`;
  `Signal.watch(:nosuch)` raises `SignalLookupError`;
- `Signal.ignore(:usr1)` + `Signal.deliver(:usr1)` is a no-op;
  `Signal.default` un-pins; `Signal.disposition` reflects §5 through a full
  consumer lifecycle;
- capability-denied `Signal.watch` (`process.signal=manage`) and
  `Signal.deliver` (`process.signal=deliver`) raise `CapabilityError` with no
  disposition change;
- a deliberately saturated hub pipe still preserves a distinct current-generation
  signal identity via the pending flag (for example `USR2` is delivered even if
  the pipe is full of `USR1` wake records);
- close/re-open generation churn drops stale wake records instead of delivering
  them to a new `Signal.Source`.

Process-level:

- untouched `SIGTERM` still kills the process (module linked but unused ⇒
  zero handlers installed);
- `Signal.trap` + `Signal.propagate(:term)` exits signaled with `TERM`;
- inherited `SIG_IGN` for `TERM`/`HUP` is preserved by normal restoration but
  `Signal.propagate(:term)` still exits signaled because it installs `SIG_DFL`;
- trap without propagate exits by its normal path (exit 0);
- strict deterministic/replay mode denies both `process.signal=manage` and
  `process.signal=deliver` before disposition changes or self-delivery.

Both reps (interpreter + native backend-eq) for the corpus set; TSan run over
burst-delivery tests (the handler→pipe→reactor→scheduler path crosses three
threads).

Spec/registry sync: error registry entries (§10), canonical `signal.*` trace
events, `process.signal` target rules, effect observations (`world`/`async`),
portable-set table, and a `signal.sidecar` module doc under
`docs/module-examples/` with title `Signal`.

## 13. Resolved design questions

- **Go-style streams or Ruby-style trap?** Both, layered: `watch` is the
  primitive (multiplexed, select-compatible, no last-wins footgun), `trap`
  and `wait` are sugar over it. Ruby's global mutable trap table is
  explicitly rejected.
- **Does observing suppress the default?** Yes (Go semantics), because any
  other rule makes `watch(:term)` + slow consumer a race against death.
  `propagate` restores honesty when the program decides to die.
- **Lossless queues?** No. Per-source-per-signal coalescing with `count`
  matches what the OS already guarantees and eliminates capacity/overflow
  policy from the API surface.
- **Handler restrictions?** None. Handlers are tasks; the async-signal-safe
  frontier lives entirely inside the runtime's ~10-instruction sigaction
  handler.
- **`Signal.raise`?** Rejected: the grammar reserves `raise`, and self-kill
  splits cleanly into `deliver` (observe) vs `propagate` (die correctly).
