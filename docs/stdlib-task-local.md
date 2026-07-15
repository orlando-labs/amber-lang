# Task-local context

`task.local` creates a slot whose binding belongs to the current logical Amber
task. It is not operating-system thread-local storage. A task may suspend on
`task.sleep`, I/O, or another await-like operation and later resume on a
different native worker; its task-local bindings move with its continuation.

```amber
import task

CURRENT_REQUEST = task.local(inherit: true)

CURRENT_REQUEST.bound?()
CURRENT_REQUEST.get(default: null)
CURRENT_REQUEST.set!(request)
CURRENT_REQUEST.clear!()

CURRENT_REQUEST.with(request):
  handle_request()
```

`with(value)` is the recommended scoped form. It returns the block result and
restores the previous binding after normal return or exception. Scopes may be
nested. The active scope remains installed while the block is suspended, and
task cancellation or terminal unwind releases both the active value and saved
outer bindings.

## Spawn inheritance

Inheritance is an explicit slot property:

- `task.local(inherit: false)` is the default. A child created by `task.spawn`
  begins without this slot. Use it for transactions, database connections,
  leases, locks, and other exclusive resources.
- `task.local(inherit: true)` copies the parent's binding into the child at the
  instant of `task.spawn`. Use it for request metadata, tracing, and logging.

The copied binding is a shallow snapshot. Rebinding or clearing the slot in the
child does not change the parent's binding, but a mutable object stored as the
value is the same object in both tasks. It must therefore already be safe to
share under Amber's ownership rules.

Task-local state is owned by the logical scheduler task and is discarded as a
unit on completion or cancellation. Libraries do not need a global task-ID map
or per-value cleanup. Lookup reads a task-owned immutable snapshot and does not
take a process-global mutex; mutation copies the normally small binding map.

## Exclusive-resource example

A connection pool should make its transaction slot non-inheritable:

```amber
class Pool:
  def init(...):
    @current_connection = task.local(inherit: false)

  def _with_executor_connection(&blk):
    if @current_connection.bound?():
      blk(@current_connection.get())
    else:
      checkout |db|:
        blk(db)

  def transaction(mode: :deferred, &blk):
    checkout |db|:
      db.transaction(mode: mode) |tx|:
        @current_connection.with(tx):
          blk(tx)
```

This lets nested ORM calls route through the transaction's connection without
`task.sync`. A child spawned inside the transaction does not inherit the
connection and therefore cannot accidentally use the same SQLite connection
concurrently.

Implementation rationale and lifecycle details are recorded in
[`DESIGN-task-local-context-2026-07-14.md`](../DESIGN-task-local-context-2026-07-14.md).
