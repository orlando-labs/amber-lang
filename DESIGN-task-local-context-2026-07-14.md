# Task-local context (2026-07-14)

## Status

Implemented design for logical task-local values shared by the bytecode VM,
the direct-native runtime, and VM fallback paths.

## Problem

Amber tasks are resumable scheduler strands. A suspended strand may resume on
a different native worker, so an OS thread-local value is not a task-local
value. Scheduler-local numeric task ids are also not a safe slot namespace:
different schedulers and resumable VMs may reuse the same numbers.

The primitive must let libraries bind request, tracing, lease, or transaction
state to the logical task without pinning it to a worker and without keeping a
process-global `task_id -> value` table.

## Public surface

```amber
import task

CURRENT = task.local(inherit: false)

CURRENT.bound?()
CURRENT.get(default: null)
CURRENT.set!(value)
CURRENT.clear!()

result = CURRENT.with(value):
  # The binding remains active across task.sleep and cooperative I/O.
  work()
```

`inherit` defaults to `false`. `set!` returns the assigned value, `clear!`
returns whether a binding existed, and `with` returns the block result.

## Slot identity

Each `task.local` call creates an immutable key containing a process-wide
monotonic 64-bit slot id and the inheritance policy. The slot id is allocated
atomically and does not depend on a scheduler, VM, strand id, or native thread.
The key is a small first-class runtime value and is safe to share.

## Storage and scheduling

Every scheduler `StrandRecord` owns a `RuntimeTaskContext`. The context moves
with that record through local/global runnable queues and is installed only
while a worker executes the strand. The worker-local pointer is an execution
cursor, not the source of state; all bindings live in the context owned by the
logical task.

Bindings are a task-owned hash map from slot id to a type-erased retained value
holder. Reads use an immutable snapshot and therefore do not take a global
mutex. Mutations replace the snapshot copy; the usual number of bindings is
small, while lookup remains average O(1). This copy-on-write shape also lets GC
enumerate roots concurrently without racing a task mutation.

The type-erased holder supports both `runtime::Value` and the direct-native
`NativeValue`, so the scheduler and inheritance mechanism are shared while
each backend retains values through its existing ownership implementation.
At a native/VM bridge boundary the immutable slot key is copied with the same
slot id. Payload conversion follows the existing bridge value contract;
bridge-representable runtime values (notably native-extension foreign handles)
are retained as GC-visible runtime roots alongside their native representation.

## Spawn inheritance

At `task.spawn` (and `task.async`) the scheduler snapshots the current logical
context before the child becomes runnable:

- entries whose key has `inherit: false` are omitted;
- entries whose key has `inherit: true` are copied into a new child context;
- the holder/value copy is shallow;
- subsequent child `set!` or `clear!` replaces only the child's map entry.

Mutable payloads inside an inherited value are consequently shared, not
deep-copied. They must already satisfy Amber's ownership/shareability rules and
must be synchronized if mutated concurrently. Exclusive resources such as
database connections, transactions, leases, and file cursors must use
`inherit: false`.

Structured-parent lookup is additionally scoped by scheduler identity. A raw
task id from another scheduler can no longer be mistaken for a local parent
whose numeric id happens to match.

## Scoped binding and suspension

`with` pushes a restoration record into the task context before invoking the
block. The record contains the previous binding and is therefore also a GC
root. In the bytecode VM, the block is pushed onto the same persistent frame
stack as the surrounding task rather than executed by the one-shot stdlib
block helper. The block frame owns only a restoration token.

Normal frame return and exception/throw unwind pop the token and restore the
previous binding. Because both the frame and context remain in the logical
strand while it is parked, the binding and its nested restoration stack survive
sleep, I/O, and worker migration. Terminal cancellation/fault clears the whole
task context even when no language handler resumes execution.

The direct-native lane uses the same context push/pop operation with exception
safe scope cleanup. VM fallback entered while a scheduler task is active sees
the already-installed logical context.

## Lifecycle and GC

Binding holders own ordinary Amber values:

- `set!` retains the new value;
- overwrite releases the replaced holder;
- `clear!` releases the erased holder;
- `with` retains both the current binding and any shadowed binding until exit;
- inherited child entries retain shallow value copies independently;
- normal completion and cancellation clear all current and shadow bindings.

Amber's collector requires explicit roots. Live task contexts are therefore
registered in a compact GC-root registry. The registry stores context pointers,
not bindings or task identities, is consulted only during collection, and
reuses slots on context destruction. Hot-path lookup never touches it. Scheduler
completion clears and releases the task context as one unit, so thousands of
serial short tasks do not grow the registry or retain task-local objects. The
scheduler's pre-existing diagnostic strand records may retain their callable;
that retention is separate from task-local binding lifetime.

## Thread safety

A scheduler runs a logical strand on at most one worker at a time. Context
mutation therefore has a single logical writer. Immutable snapshots make
concurrent GC enumeration and spawn snapshot reads race-free. Parent and child
contexts are separate objects after spawn. No binding is stored in OS TLS and
no process-global lock is taken by `bound?` or `get`.

## Deterministic migration test hook

Runtime tests may park a strand without a timer and wake it onto a selected
worker. The hook targets only the next scheduler enqueue and does not pin the
strand afterward. Production wakeups and work stealing remain unchanged.

## Downstream use

`sqlite3-amber` should allocate one non-inheriting local per pool and bind the
checked-out transaction executor with `with`. Nested ORM calls then resolve the
same executor after suspension/migration, while a child `task.spawn` observes
no transaction binding and cannot concurrently use the exclusive connection.
