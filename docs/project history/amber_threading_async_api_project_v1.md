# Amber Threading / Async API Project v1

**Проектный слой для разработки `task` / `sync` / flow concurrency API Amber**  
**Версия:** v1.0-project  
**Дата:** 30 мая 2026  
**Основание:** Amber v20.1 consolidated spec, Amber compilable project layer, Amber stdlib next-layer plan  
**Статус:** engineering blueprint для реализации stdlib/runtime/API слоя

---

## 0. Статус документа

Этот документ является проектным слоем для разработки threading/async API Amber поверх уже зафиксированной спецификации Amber v20.1 и проектного слоя компилируемого Amber.

Документ не меняет базовую семантику языка, не вводит новый общий синтаксис выражений и не переоткрывает закрытые решения core language. Его задача — превратить уже зафиксированную runtime-модель `Worker -> Strand -> Task`, no-GIL scheduler, structured concurrency, `Channel`, `Mutex`, `Atomic`, cancellation и shareability/isolation rules в подробный инженерный контракт для реализации.

Дополнительно этот документ вводит два расширения к базовому task/sync слою:

1. **MPI-like scatter/gather flow API** для data-parallel threading workflows.
2. **Explicit unchecked isolation mode** для настоящего no-GIL threading без автоматического `IsolationError`, когда пользователь явно принимает ответственность за synchronization и data races.

Каноническая граница:

- Amber language spec определяет язык, syntax, object model, pattern matching, callable refs, modules, profiles.
- Amber compilable project layer определяет VM, bytecode, loader, verifier, scheduler/runtime ABI и implementation matrix.
- Этот документ определяет stdlib/runtime-facing API для threading/async, включая user API, runtime hooks, verifier requirements, errors, conformance corpus и backlog.

---

## 1. Цели слоя

Threading/async layer должен дать Amber:

- cooperative async tasks внутри одного strand;
- настоящую параллельность через `task.spawn` и несколько worker threads;
- safe default на основе strand isolation;
- explicit unsafe escape hatch для performance-critical/system code;
- structured concurrency как default;
- cancellation и timeout semantics;
- bounded/unbounded synchronization primitives;
- MPI-like scatter/gather flows;
- deterministic diagnostics, traces и conformance tests;
- основу для последующих слоёв: Watch, IO, networking, HTTP, advanced concurrency.

Минимальный успешный результат:

```text
Amber source
  -> HIR task/sync/flow nodes
  -> bytecode concurrency opcodes / intrinsic sends
  -> verifier checks
  -> no-GIL VM scheduler
  -> deterministic runtime behavior under conformance corpus
```

---

## 2. Non-goals для v1

В первый релиз не входят:

- `task.select`;
- `move(value)` ownership transfer;
- supervisor trees / policies;
- actor framework;
- async file IO;
- TCP/HTTP implementation;
- stream backpressure protocol;
- distributed multi-process MPI;
- deterministic replay scheduler как обязательный production mode;
- automatic data-race detection в release runtime.

Эти возможности могут быть реализованы позже поверх стабильного `task/sync/flow` слоя.

---

## 3. Базовая execution model

Amber использует три уровня исполнения:

```text
Worker = системный поток ОС
Strand = последовательная область исполнения с собственной runnable-очередью
Task   = кооперативная fiber/coroutine внутри strand
```

Основной инвариант:

```text
В одном strand одновременно исполняется не более одной task.
Разные strand могут исполняться параллельно на разных worker threads.
```

Следствия:

- VM не имеет global interpreter lock.
- Внутри одного strand ordinary mutable state безопасна без lock, потому что нет параллельного исполнения двух task одного strand.
- Параллельность возникает между разными strand.
- Cross-strand доступ в checked mode разрешён только для shareable values и sync objects.
- В unchecked mode isolation gate отключается явно, но VM memory safety остаётся обязательной.

---

## 4. Namespaces

Канонический импорт:

```amber
import task
from sync import Channel, Mutex, Atomic
```

Flow API:

```amber
from task.flow import scatter, gather, scatter_map, scatter_reduce, broadcast
```

Runtime-visible types:

```amber
task.TaskHandle
task.CancelToken
task.Timeout
task.Scheduler

task.flow.Flow
task.flow.Partition
task.flow.ScatterPlan
task.flow.GatherResult
task.flow.WorkerGroup

sync.Channel
sync.Mutex
sync.Atomic
```

`TaskHandle`, `Channel`, `Mutex`, `Atomic` являются sync/shareable runtime objects и могут пересекать strand boundary.

---

## 5. Normative decisions summary

1. `task.async` создаёт child task в том же strand.
2. `task.spawn` создаёт child task в новом strand.
3. Ordinary mutable objects являются strand-confined by default.
4. Cross-strand boundaries в checked mode принимают только shareable values и sync objects.
5. Нарушение isolation в checked mode даёт compile-time diagnostic или runtime `IsolationError`.
6. `isolation: :unchecked` явно отключает isolation checks для конкретного spawn/flow/channel boundary.
7. Unchecked mode не отключает lifetime checks, GC checks, verifier checks, write barriers, root maps или object validity checks.
8. Unchecked mode не создаёт happens-before edge; synchronization остаётся обязанностью пользователя.
9. `Channel` payloads требуют shareability by default.
10. `Mutex` non-reentrant.
11. `Atomic` seq-cst only в v1.
12. `wait(timeout:)` не отменяет child автоматически.
13. `cancel()` cooperative и idempotent.
14. Structured concurrency является default.
15. Public orphan tasks не входят в v1.
16. Scatter/gather flow API предоставляет MPI-like high-level threading flows.
17. Scatter/gather сохраняет input order by default.
18. Flow failure policy defaults to `fail: :first`.
19. Artifact с unchecked concurrency должен иметь `unsafe_concurrency` feature flag.
20. Host policy может запретить unsafe concurrency при compile/load/runtime.

---

## 6. Root async scope

### 6.1. Basic form

```amber
async |task|:
  # root task body
  ...
```

Семантика:

- создаёт root strand scope;
- создаёт root task внутри этого strand;
- передаёт в блок task-context object;
- по выходу из блока выполняет structured join всех дочерних task;
- если child task упала, root scope получает failure propagation;
- если root scope cancelled, все structured children получают cancellation request.

Результат async scope:

```amber
result = async |task|:
  1 + 2
# result == 3
```

Если внутри scope есть незавершённые children, scope не завершается до auto-join или cancellation-unwind.

---

## 7. `task.async` — same-strand cooperative task

```amber
handle = task.async |child|:
  compute()
```

Семантика:

- создаёт child task в том же strand;
- child task разделяет sequential execution domain с parent и siblings;
- ordinary mutable objects можно захватывать по ссылке;
- параллельного доступа к таким объектам нет, потому что strand исполняет только одну task за раз;
- scheduling cooperative: переключение происходит на safepoints.

Пример:

```amber
async |task|:
  rows = []

  producer = task.async |child|:
    rows << read_row()

  consumer = task.async |child|:
    if rows.count() > 0:
      commit rows
```

`rows` остаётся ordinary mutable Array, потому что обе tasks находятся в одном strand.

---

### 7.1. Synchronous block inside async scopes

```amber
async |task|:
  task.async |child|:
    ...

  sync:
    critical_without_task_switching()
```

Equivalent explicit spelling:

```amber
async |task|:
  task.sync:
    critical_without_task_switching()
```

Semantics:

- `sync:` / `task.sync:` does not create a new task;
- the block runs inline in the current task and strand;
- cooperative task switching is suppressed while the block is active;
- nested sync blocks keep switching suppressed until the outermost block exits;
- cancellation and lifetime checks remain valid at explicit runtime checks;
- blocking OS calls inside the block remain blocking OS calls and do not become
  reactor/cooperative waits.

The construct is intended for small critical sections that must not be
interleaved with other same-strand async tasks. It is not a replacement for
`Mutex` across strands.

---

## 8. `task.spawn` — new-strand parallel task

### 8.1. Checked default

```amber
handle = task.spawn |child|:
  compute_shareable()
```

Семантика:

- создаёт child task в новом strand;
- новый strand может быть выполнен любым worker;
- task реально может исполняться параллельно с parent;
- closure captures должны быть shareable;
- direct capture ordinary mutable objects запрещён;
- нарушение даёт compile-time diagnostic, если видно статически, или `IsolationError` runtime.

Valid:

```amber
path = "/tmp/input.txt".freeze()

async |task|:
  worker = task.spawn |child|:
    parse_file(path)

  result = worker.wait()
```

Invalid in checked mode:

```amber
async |task|:
  rows = []

  worker = task.spawn |child|:
    rows << 1
# IsolationError или compile-time diagnostic
```

---

## 9. Explicit unchecked isolation mode

### 9.1. Motivation

Amber VM рассчитана на настоящий no-GIL threading. Safe default через strand isolation нужен для обычного пользовательского кода, но low-level, system, HPC и performance-critical code должен иметь явный способ отключить isolation gate.

Unchecked mode означает:

```text
Пользователь явно разрешает cross-strand access к non-shareable объектам
и принимает ответственность за synchronization, data races и nondeterministic interleavings.
```

---

### 9.2. Syntax

Default:

```amber
task.spawn:
  work()
```

Эквивалентно:

```amber
task.spawn(isolation: :checked):
  work()
```

Unchecked:

```amber
task.spawn(isolation: :unchecked):
  work()
```

Допустимый ergonomic alias:

```amber
task.safe_spawn:
  work()
task.unsafe_spawn:
  work()
```

Каноническая форма документации:

```amber
task.spawn(isolation: :unchecked):
  work()
```

---

### 9.3. Allowed isolation modes

```text
:checked     — default safe mode
:unchecked   — disable shareability/isolation gate for this boundary
```

Future modes, not v1:

```text
:borrowed
:moved
:shared_lock
```

---

### 9.4. Semantics of `isolation: :unchecked`

При `isolation: :unchecked`:

- `task.spawn` не требует shareable captures;
- closure может захватывать strand-confined objects;
- runtime не бросает `IsolationError` только за факт cross-strand capture/access;
- ordinary object operations выполняются как обычные VM operations;
- data races становятся возможными;
- memory visibility гарантируется только explicit synchronization edges;
- GC/lifetime safety остаётся обязательной;
- destroyed/deallocated/pin/object-header violations остаются runtime errors.

Ключевой инвариант:

```text
Unchecked isolation disables ownership/isolation checks.
It does not disable lifetime checks, verifier checks, GC root maps, write barriers or object validity checks.
```

---

### 9.5. Happens-before in unchecked mode

Unchecked mode не создаёт happens-before edge.

Между strand'ами видимость появляется только через:

```text
Channel.send / Channel.recv
Channel.close
Mutex.unlock / Mutex.lock
Task completion / TaskHandle.wait
Atomic operations
flow/gather join
```

Если два strand одновременно пишут в ordinary object без synchronization:

```text
Amber-level data race with unspecified interleaving
```

Но VM не имеет права допустить:

```text
memory corruption
dangling pointer
invalid object header
collector unsoundness
write barrier omission
root-map loss
```

Иными словами:

```text
Amber-level state may be racy.
VM-level memory safety must remain intact.
```

---

### 9.6. Example

Checked violation:

```amber
rows = []

task.spawn:
  rows << 1
# compile-time diagnostic or IsolationError
```

Explicit unchecked:

```amber
rows = []

task.spawn(isolation: :unchecked):
  rows << 1
# allowed; module marked unsafe_concurrency
```

Recommended synchronized unchecked sharing:

```amber
m = Mutex.new()
shared = []

task.spawn(isolation: :unchecked):
  m.synchronize:
    shared << 1
```

---

### 9.7. Artifact markers

Любой `.amberbc`, содержащий unchecked spawn/flow/channel, должен иметь module flag:

```text
feature_flags: ["unsafe_concurrency"]
```

Code/debug metadata:

```text
unsafe_regions:
  - kind: "unchecked_spawn"
    span: source_span
    captures: [...]
```

Verifier обязан:

- разрешить unchecked regions только при наличии feature flag;
- сохранить stack/root maps;
- проверить handler tables;
- проверить safepoint metadata;
- не доказывать shareability captures внутри unchecked region;
- не отключать lifetime/GC/write-barrier validation.

Loader может запретить такой module host policy'ей:

```text
UnsafeConcurrencyDeniedError
```

---

### 9.8. Runtime implementation strategy

Не рекомендуется рекурсивно retag'ать object graph в `unchecked_shared` для v1.

Reference VM approach:

```text
Frame.flags += UNSAFE_CONCURRENCY_REGION
```

Ownership checks:

```text
if current_frame.unsafe_concurrency:
  skip IsolationError check
else:
  enforce owner_token
```

Lifetime checks always run.

Rationale:

- recursive graph retagging expensive;
- object graph may be cyclic;
- hidden ownership mutation complicates diagnostics;
- frame-level unsafe flag is traceable and reversible.

---

### 9.9. CLI / package policy

Compiler/runtime flags:

```text
amberc --disallow-unsafe-concurrency
ambervm --disallow-unsafe-concurrency
ambertest --disallow-unsafe-concurrency
```

Package manifest:

```toml
[features]
unsafe_concurrency = false
```

Default:

```text
unsafe_concurrency = true
```

If artifact contains `unsafe_concurrency`, but policy denies it:

```text
UnsafeConcurrencyDeniedError
```

---

## 10. TaskHandle API

### 10.1. Construction

`TaskHandle` не создаётся напрямую пользовательским кодом.

Invalid:

```amber
h = TaskHandle.new()
```

Valid factory paths:

```amber
h1 = task.async: work()
h2 = task.spawn: work()
```

---

### 10.2. Public methods

```amber
handle.wait()
handle.wait(timeout: seconds)

handle.cancel()
handle.cancelled?()
handle.done?()
handle.running?()
handle.failed?()

handle.result()
handle.failure()

handle.resume()
handle.strand_id()
handle.task_id()
```

---

### 10.3. `wait()`

```amber
value = handle.wait()
```

Semantics:

- if task completed successfully, return result;
- if task failed, re-raise failure in waiter;
- if task cancelled, throw `CancelledError`;
- if current task is cancelled while waiting, throw `CancelledError`;
- `wait()` is a cancellation point;
- `wait()` is a scheduler safepoint.

---

### 10.4. `wait(timeout:)`

```amber
value = handle.wait(timeout: 1.0)
```

Semantics:

- timeout is seconds;
- deadline uses scheduler clock;
- if deadline expires first, throw `TimeoutError`;
- timeout does not cancel child automatically;
- user may call `handle.cancel()` explicitly.

Example:

```amber
handle = task.spawn:
  long_job()

try:
  value = handle.wait(timeout: 2.0)
catch TimeoutError:
  handle.cancel()
```

---

### 10.5. `cancel()`

```amber
handle.cancel()
```

Semantics:

- sets cancellation flag;
- wakes sleeping/waiting task if possible;
- does not kill task preemptively;
- task observes cancellation at safepoints;
- repeated `cancel()` is idempotent;
- returns `true` if request was new, otherwise `false`.

---

### 10.6. `result()`

```amber
value = handle.result()
```

Semantics:

- if task done successfully, returns result;
- if task not done, throws `TaskNotDoneError`;
- if task failed, throws `TaskFailedError` or re-raises original failure depending on profile;
- if task cancelled, throws `CancelledError`.

Normative distinction:

```text
result() is non-blocking.
wait() is blocking/cooperative waiting.
```

---

### 10.7. `failure()`

```amber
err = handle.failure()
```

Semantics:

- if task failed, returns error object;
- if task done successfully, returns `null`;
- if task cancelled, returns `CancelledError` object or `null` depending on internal representation;
- if task not done, throws `TaskNotDoneError`.

---

### 10.8. `resume()`

```amber
handle.resume()
```

Semantics:

- makes sleeping/waiting task runnable;
- idempotent if task already runnable/running/done;
- does not override cancellation;
- does not migrate task to another strand.

---

## 11. Task context API

Task block receives a context object:

```amber
async |task|:
  task.sleep(0.1)
  task.yield()
```

Methods:

```amber
task.async |child|: ...
task.spawn |child|: ...

task.sleep(seconds)
task.yield()

task.cancelled?()
task.check_cancelled!()

task.current()
task.current_handle()
task.strand_id()
task.task_id()
```

### 11.1. `sleep(seconds)`

```amber
task.sleep(0.1)
```

Semantics:

- puts current task into sleeping state;
- registers timer in scheduler;
- cancellation point;
- `seconds <= 0` behaves like `yield()`;
- `NaN`, invalid negative values and non-numbers produce `ArgumentError` / `TypeError`.

### 11.2. `yield()`

```amber
task.yield()
```

Semantics:

- voluntarily yields to scheduler;
- current task returns to runnable queue;
- cancellation point;
- does not guarantee another task runs first if runnable queue is empty.

### 11.3. `check_cancelled!()`

```amber
task.check_cancelled!()
```

Semantics:

- if cancellation flag is set, throws `CancelledError`;
- otherwise returns `null`;
- intended for CPU-bound loops.

---

## 12. Structured concurrency

### 12.1. Parent-child ownership

Each task has:

```text
parent?
children_set
state
result_or_failure
cancel_flag
```

Rules:

- `task.async` and `task.spawn` create structured child tasks;
- child belongs to lexical/root async scope;
- parent scope does not complete until children done/failed/cancelled;
- public orphan tasks are not v1.

### 12.2. Failure propagation

If child failed:

1. failure is stored in `TaskHandle`;
2. sibling tasks receive cancellation request;
3. parent observes failure during structured join;
4. stack trace includes task boundary;
5. diagnostics are deterministic.

### 12.3. Cancellation propagation

If parent cancelled:

```text
parent.cancel()
  -> mark parent cancel_flag
  -> request cancellation for all structured children
  -> wake sleeping/waiting children
```

Child may finish as:

- `cancelled`, if it observes cancellation;
- `done`, if it completes before safepoint;
- `failed`, if it throws another error.

---

## 13. Ownership and shareability

Runtime owner modes:

```text
shareable
confined(strand_id)
sync
```

Optional internal mode for debug/accounting:

```text
unchecked_shared
```

### 13.1. Shareable values

Shareable:

```text
null
true
false
numbers
symbols
frozen strings
frozen tuples/lists/maps
constant language metaobjects
closures with only shareable captures
TaskHandle
Channel
Mutex
Atomic
```

### 13.2. Strand-confined by default

```text
Array
Map
mutable String
user objects with mutable state
capture cells
closures capturing non-shareable references
```

### 13.3. Boundary checks

Runtime checks required at:

```text
task.spawn capture verification
Channel.send payload boundary
SEND/CALL receiver ownership check
LOAD_IVAR / STORE_IVAR
indexing fast paths
collection builtin fast paths
```

Violation in checked mode:

```text
IsolationError
```

No implicit behavior in checked mode:

- no hidden deep clone;
- no transparent thread-safe wrapper;
- no automatic ownership transfer;
- no copy-on-send;
- no move semantics.

---

## 14. Channel API

### 14.1. Construction

```amber
ch = Channel.new()
ch = Channel.new(capacity: 0)
ch = Channel.new(capacity: 16)
```

Defaults:

```text
capacity: 0
isolation: :checked
```

Meaning:

- `capacity: 0` — rendezvous/unbuffered channel;
- `capacity > 0` — bounded buffered channel;
- `capacity < 0` — `ArgumentError`;
- non-integer capacity — `TypeError`.

Unchecked channel:

```amber
ch = Channel.new(capacity: 16, isolation: :unchecked)
```

---

### 14.2. Methods

Required v1:

```amber
ch.send(value)
value = ch.recv()

ch.close()
ch.closed?()
```

Recommended staged methods:

```amber
ch.capacity()
ch.count()

ch.try_send(value)
ch.try_recv()

ch.send(value, timeout: seconds)
ch.recv(timeout: seconds)
```

---

### 14.3. `send(value)`

Rules:

- if channel open and receiver waiting, deliver directly;
- if channel open and buffer has space, enqueue;
- if channel open and buffer full, current task waits;
- if channel closed, throw `ChannelClosedError`;
- in checked mode, payload must be shareable;
- operation is FIFO per channel;
- operation is cancellation point.

Reference v1 rule:

```text
Channel payload must be shareable unless channel isolation is explicitly :unchecked.
```

---

### 14.4. Unchecked channel

```amber
ch = Channel.new(capacity: 16, isolation: :unchecked)
```

Rules:

- channel is marked unsafe;
- values transferred without shareability gate;
- FIFO/wake semantics unchanged;
- send/recv still provide synchronization edge;
- sender and receiver may still race if both keep aliases and mutate shared object concurrently.

---

### 14.5. `recv()`

Rules:

- if buffer non-empty, returns next FIFO value;
- if sender waiting on unbuffered channel, receives directly and wakes sender;
- if channel open and no value, current task waits;
- if channel closed and buffer non-empty, drains buffered values;
- if channel closed and empty, throws `ChannelClosedError`;
- operation is cancellation point.

---

### 14.6. `close()`

Rules:

- idempotent;
- marks channel closed;
- wakes waiting receivers;
- wakes waiting senders with `ChannelClosedError`;
- does not discard buffered values;
- successful `recv()` after close can still return buffered values;
- `send()` after close always throws `ChannelClosedError`.

---

### 14.7. Fairness

Normative v1 fairness:

```text
Per-channel waiting sender queue is FIFO.
Per-channel waiting receiver queue is FIFO.
Element order is FIFO.
Global scheduler fairness is not guaranteed.
```

---

## 15. Mutex API

### 15.1. Construction

```amber
m = Mutex.new()
```

### 15.2. Methods

```amber
m.lock()
m.unlock()
m.locked?()
m.owned?()
m.synchronize:
  critical_section()
```

Optional staged methods:

```amber
m.try_lock()
m.lock(timeout: seconds)
```

### 15.3. `lock()`

Rules:

- if unlocked, current task becomes owner;
- if locked by another task, current task waits FIFO;
- if locked by same task, throw `DeadlockError`;
- operation is cancellation point while waiting.

Owner identity:

```text
(task_id, strand_id)
```

### 15.4. `unlock()`

Rules:

- if unlocked, throw `OwnershipError`;
- if caller is not owner, throw `OwnershipError`;
- otherwise release lock;
- wake next waiter FIFO if any;
- return `null`.

### 15.5. `synchronize`

```amber
m.synchronize:
  critical_section()
```

Lowering-equivalent semantics:

```amber
m.lock()
try:
  critical_section()
finally:
  m.unlock()
```

Rules:

- guarantees unlock during normal return, exception and cancellation unwind;
- returns block result;
- propagates block exception;
- non-reentrant rule still applies.

---

## 16. Atomic API

### 16.1. Construction

```amber
a = Atomic.new(0)
```

Atomic-compatible v1 values:

```text
null
Bool
Integer
Symbol
shareable object reference
```

Optional host-supported values:

```text
Float, if host guarantees atomic representation or boxes safely
frozen String
```

### 16.2. Methods

```amber
a.get()
a.set(value)
a.compare_and_set(expected, replacement)
a.update |x|:
  x + 1
```

Optional staged methods:

```amber
a.swap(value)
a.fetch_update |x|:
  ...
```

### 16.3. Memory ordering

Reference profile:

```text
Atomic.get              seq-cst load
Atomic.set              seq-cst store
Atomic.compare_and_set  seq-cst CAS
Atomic.update           CAS loop using seq-cst CAS
```

No weaker memory orders in v1 public API.

### 16.4. `compare_and_set`

```amber
ok = a.compare_and_set(1, 2)
```

Returns:

```text
true  if value changed
false if current value != expected
```

Comparison semantics:

- primitive values compare by value;
- heap references compare by identity;
- user code must not execute inside CAS comparison.

### 16.5. `update`

```amber
a.update |x|:
  x + 1
```

Semantics:

```text
loop:
  old = a.get()
  new = block(old)
  if a.compare_and_set(old, new):
    return new
```

Rules:

- block may be re-executed;
- block must be side-effect-safe or user accepts repeated side effects;
- if block raises, update aborts and propagates exception;
- replacement must be atomic-compatible.

Mandatory documentation warning:

```text
Atomic.update block can run more than once.
```

---

## 17. MPI-like scatter/gather flow API

### 17.1. Purpose

Flow API provides data-parallel high-level operations:

```text
scatter -> parallel workers -> gather
scatter -> parallel workers -> reduce
broadcast -> parallel workers -> gather
```

This is not distributed MPI. It is MPI-like local no-GIL threading over Amber strands/workers.

---

### 17.2. Namespace

```amber
from task.flow import scatter, gather, scatter_map, scatter_reduce, broadcast
```

---

### 17.3. Core runtime types

```text
Flow(
  flow_id,
  parent_task_id,
  worker_handles[],
  input_partitions[],
  result_policy,
  failure_policy,
  cancellation_policy,
  isolation_mode
)

Partition(
  index,
  value,
  size_hint?,
  metadata?
)

GatherResult(
  values[],
  failures[],
  cancelled?,
  completed_count,
  failed_count
)
```

---

### 17.4. `scatter`

```amber
flow = task.flow.scatter(items, workers: 4) |part, ctx|:
  process(part)
```

Semantics:

- `items` split into partitions;
- each partition handled by child task via `task.spawn` semantics;
- each worker task gets `part` and flow-local context `ctx`;
- result stored by partition index;
- parent performs structured gather.

Example:

```amber
from task.flow import scatter

parsed = scatter(files, workers: 8) |file, ctx|:
  parse_file(file)
```

Result:

```text
Array[result] in original partition order
```

---

### 17.5. `scatter_map`

```amber
parsed = task.flow.scatter_map(files, workers: 8) |file|:
  parse_file(file)
```

Equivalent user-level sketch:

```amber
handles = files.map |file|:
  task.spawn:
    parse_file(file)

parsed = handles.map |h|:
  h.wait()
```

Runtime receives additional structured metadata:

- workers count;
- partition boundaries;
- ordered gather;
- cancellation policy;
- trace metadata;
- possible auto-batching optimization.

---

### 17.6. `scatter_reduce`

```amber
total = task.flow.scatter_reduce(
  items,
  init: 0,
  workers: 8,
  map: |x|: expensive_score(x),
  reduce: |acc, x|: acc + x
)
```

One-block form:

```amber
total = task.flow.scatter_reduce(items, init: 0, workers: 8) |partition|:
  partition.reduce(0) |acc, x|:
    acc + expensive_score(x)
```

Semantics:

1. input split into partitions;
2. each partition computed in parallel;
3. partial results gathered;
4. reduce performed in parent strand or tree-reduce mode;
5. final result returned.

Numerics warning:

```text
Parallel floating-point reduction may change operation order and produce slightly different low-level numeric results.
```

---

### 17.7. `broadcast`

```amber
flow = task.flow.broadcast(config, workers: 8) |cfg, ctx|:
  worker_loop(cfg)
```

Semantics:

- one value sent to all workers;
- in checked mode, value must be shareable;
- in unchecked mode, mutable broadcast is allowed but unsafe;
- each worker receives same value/reference according to isolation mode.

---

### 17.8. `gather`

```amber
handles = items.map |x|:
  task.spawn:
    process(x)

values = task.flow.gather(handles)
```

API:

```amber
task.flow.gather(handles)
task.flow.gather(handles, timeout: 5.0)
task.flow.gather(handles, ordered: true)
task.flow.gather(handles, fail: :first)
task.flow.gather(handles, fail: :collect)
```

Failure policies:

```text
:first    — first failure cancels siblings and is rethrown
:collect  — gather successes and failures in GatherResult
:ignore   — return only successes; failures accessible in result.failures
```

Default:

```text
fail: :first
ordered: true
```

---

### 17.9. Partitioning policy

```amber
scatter_map(items, workers: 8, partition: :chunks) |x|:
  ...
```

Supported v1 policies:

```text
:items    — one logical work item per input item; runtime may batch
:chunks   — contiguous chunks
:stride   — round-robin distribution
:custom   — user partitioner block
```

Custom partitioner:

```amber
parts = task.flow.scatter(items, partition: |items, workers|:
  items.group_by: _1.customer_id.hash() % workers
) |group|:
  process_group(group)
```

In checked mode, custom partitions must be shareable.

---

### 17.10. Flow cancellation

If parent flow cancelled:

```text
flow.cancel()
  -> cancel all worker handles
  -> wake blocked gather
  -> return/raise CancelledError according to caller context
```

If worker failed under default `fail: :first`:

1. store first failure;
2. cancel siblings;
3. gather rethrows failure in parent;
4. stack trace contains flow boundary.

---

### 17.11. Ordering

Default result order preserves input order:

```amber
out = scatter_map([a, b, c], workers: 2) |x|:
  f(x)

# out[0] corresponds to a
# out[1] corresponds to b
# out[2] corresponds to c
```

Future streaming API:

```amber
task.flow.gather_each(handles) |result|:
  consume(result)
```

`gather_each` may return in completion order and must not be default.

---

### 17.12. Flow isolation modes

Checked mode:

```amber
task.flow.scatter_map(items, workers: 8) |x|:
  process(x)
```

Rules:

- input partitions must be shareable;
- closure captures must be shareable;
- worker results crossing back to parent must be shareable;
- non-shareable input gives diagnostic or `IsolationError`.

Unchecked mode:

```amber
task.flow.scatter_map(
  items,
  workers: 8,
  isolation: :unchecked
) |x|:
  mutate_shared_state(x)
```

Rules:

- flow marked unsafe;
- partition/capture shareability gate skipped;
- lifetime/GC/verifier checks still mandatory;
- user must synchronize shared mutation manually.

---

## 18. Task state machine

Minimal task states:

```text
new
runnable
running
sleeping
waiting
done
failed
cancelled
```

Transitions:

```text
new -> runnable
runnable -> running
running -> runnable        on yield
running -> sleeping        on sleep
running -> waiting         on wait/channel/mutex
sleeping -> runnable       on timer/resume/cancel wake
waiting -> runnable        on dependency ready/resume/cancel wake
running -> done            on normal return
running -> failed          on unhandled exception
running -> cancelled       on observed cancellation
```

Illegal internal transitions produce `InvalidTaskStateError`.

---

## 19. Scheduler design

### 19.1. Worker pool

```text
WorkerPool(
  workers[],
  global_inject_queue,
  timer_wheel_or_heap,
  io_reactor?,
  shutdown_flag
)
```

Worker:

```text
Worker(
  worker_id,
  local_strand_queue,
  current_strand?,
  stats
)
```

### 19.2. Strand

```text
Strand(
  strand_id,
  owner_worker_hint,
  runnable_tasks,
  sleeping_tasks,
  waiting_tasks,
  current_task?,
  status,
  epoch,
  local_alloc_cache
)
```

Invariants:

- at most one running task per strand;
- runnable queue local to strand;
- strand may migrate between workers only when no task is running;
- migration preserves task order inside strand.

### 19.3. Reference scheduling algorithm

1. Worker pops runnable strand from local queue.
2. Worker runs one task from strand until safepoint, block, return, failure or budget expiration.
3. If strand still has runnable tasks, requeue strand.
4. If strand blocks completely, remove from runnable queues.
5. Timers/channel/mutex/resume events re-enqueue strand.
6. Work stealing may move runnable strand between workers.

Budget:

```text
cooperative_budget = instruction_count or time_slice
```

---

## 20. Safepoints

Required safepoints:

```text
task.sleep
task.yield
TaskHandle.wait
Channel.send blocking path
Channel.recv blocking path
Mutex.lock blocking path
loop back-edge
function/method call boundary
explicit SAFEPOINT bytecode
IO awaitable boundary, later
```

At safepoint:

1. poll cancellation;
2. process pending wake/resume;
3. allow GC root scanning;
4. allow scheduler preemption budget check;
5. update trace events if observability profile enabled.

---

## 21. HIR lowering

Surface forms remain ordinary AST send/block syntax unless builtin/intrinsic resolution proves canonical identity.

HIR nodes:

```text
HAsyncRoot
HSpawnSameStrand
HSpawnNewStrand
HSpawnUncheckedNewStrand
HTaskSleep
HTaskYield
HWait
HCancel
HResume
HTaskCheckCancelled
HChannelSend
HChannelRecv
HMutexLock
HMutexUnlock
HAtomicGet
HAtomicSet
HAtomicCAS
HFlowScatter
HFlowGather
HFlowScatterMap
HFlowScatterReduce
HFlowBroadcast
```

Lowering examples:

```text
async |task|: body
  -> HAsyncRoot(task_param, body)

task.async |child|: body
  -> HSpawnSameStrand(task_context, child_param, body)

task.spawn |child|: body
  -> HSpawnNewStrand(isolation=:checked, child_param, body)

task.spawn(isolation: :unchecked) |child|: body
  -> HSpawnNewStrand(isolation=:unchecked, child_param, body)

task.flow.scatter_map(items, workers: n) |x|: body
  -> HFlowScatterMap(isolation=:checked, items, workers, body)

task.flow.scatter_map(items, workers: n, isolation: :unchecked) |x|: body
  -> HFlowScatterMap(isolation=:unchecked, items, workers, body)
```

If `task`, `flow`, `spawn`, `sleep`, `yield`, `wait`, `cancel`, `resume`, `Channel`, `Mutex` or `Atomic` are shadowed by user bindings, lowering must not treat them as intrinsics. They remain ordinary dynamic calls.

---

## 22. Bytecode / VM opcodes

Minimum concurrency opcode family:

```text
SPAWN_SAME
SPAWN_NEW
SPAWN_NEW_UNCHECKED
TASK_CURRENT
TASK_SLEEP
TASK_YIELD
TASK_WAIT
TASK_CANCEL
TASK_RESUME
TASK_CHECK_CANCELLED

CHANNEL_NEW
CHANNEL_NEW_UNCHECKED
CHANNEL_SEND
CHANNEL_RECV
CHANNEL_CLOSE
CHANNEL_CLOSED

MUTEX_NEW
MUTEX_LOCK
MUTEX_UNLOCK
MUTEX_SYNCHRONIZE_ENTER
MUTEX_SYNCHRONIZE_EXIT

ATOMIC_NEW
ATOMIC_GET
ATOMIC_SET
ATOMIC_CAS
ATOMIC_UPDATE_LOOP

FLOW_SCATTER
FLOW_GATHER
FLOW_REDUCE
FLOW_CANCEL

SAFEPOINT
```

Implementation may also dispatch via ordinary `SEND`/`CALL`, but intrinsic opcodes are recommended for verifier checks and performance.

---

## 23. Verifier rules

Verifier must check:

```text
SPAWN_NEW closure operand exists and is callable
SPAWN_NEW closure capture map is encoded
SPAWN_NEW checked regions have shareability metadata or dynamic guards
SPAWN_NEW_UNCHECKED appears only with unsafe_concurrency feature flag
unchecked regions have source spans and unsafe metadata
TASK_WAIT target register has TaskHandle-compatible value or dynamic guard
CHANNEL_SEND has safepoint metadata
MUTEX_LOCK has safepoint metadata
loop back-edges include SAFEPOINT or equivalent call-boundary poll
handler tables cover synchronize unwind regions
root maps include blocked task frames
shareable sections contain no raw pointers
FLOW_* sections preserve partition/gather metadata
FLOW_* unchecked mode requires unsafe_concurrency feature flag
```

Verifier must not:

- skip GC root-map validation in unsafe regions;
- skip write-barrier validation;
- allow unsafe concurrency without artifact marker;
- execute user code.

---

## 24. Runtime ABI

### 24.1. Task

```text
Task(
  task_id,
  strand_id,
  parent_task_id?,
  state,
  frame_stack,
  result?,
  failure?,
  cancel_flag,
  wake_pending,
  structured_children,
  waiting_on?,
  deadline?,
  trace_context?
)
```

### 24.2. TaskHandle

```text
TaskHandle(
  target_task_id,
  target_strand_id,
  sync_header,
  result_cell,
  failure_cell,
  state_cell
)
```

Owner mode:

```text
sync
```

### 24.3. Channel

```text
Channel(
  sync_header,
  capacity,
  closed_flag,
  isolation_mode,
  buffer_queue,
  waiting_senders,
  waiting_receivers
)
```

Waiters:

```text
SenderWaiter(task_id, strand_id, value, deadline?)
ReceiverWaiter(task_id, strand_id, target_register, deadline?)
```

### 24.4. Mutex

```text
Mutex(
  sync_header,
  owner_task_id?,
  owner_strand_id?,
  recursion_guard,
  waiting_tasks
)
```

### 24.5. Atomic

```text
Atomic(
  sync_header,
  atomic_cell,
  compatibility_tag
)
```

### 24.6. Flow

```text
Flow(
  flow_id,
  parent_task_id,
  worker_group?,
  worker_handles[],
  partitions[],
  result_cells[],
  failure_cells[],
  isolation_mode,
  failure_policy,
  ordered,
  deadline?
)
```

---

## 25. Interaction with `$_`

Each task has its own frame stack and `last_result` slot.

Rules:

- `$_` is task-local through current frame;
- same-strand sibling tasks do not share `$_`;
- spawned tasks do not share `$_`;
- flow worker tasks do not share `$_`;
- root async scope result is final `$_` of root task unless explicit return/break rules apply.

---

## 26. Interaction with exceptions

Rules:

- unhandled exception in task marks task `failed`;
- `handle.wait()` re-raises failure in waiter;
- structured parent observes child failure during auto-join;
- sibling cancellation follows structured policy;
- stack trace includes async/task/flow boundary;
- stack trace is deterministic.

Example trace shape:

```text
Unhandled ParseError: invalid record
  at parse_file(path) input.am:12
  in task.spawn child task #stable-task-3
  in flow.scatter worker partition #4
  awaited by main.am:44
  in async root task #stable-task-1
```

Task IDs in diagnostics must be normalized/stable for tests, not raw memory addresses.

Unsafe boundary trace example:

```text
DataRaceDetectedError: unsynchronized ivar write
  at update_shared(state) worker.am:18
  in unsafe concurrency region task.spawn(isolation: :unchecked) worker.am:14
  in async root task #stable-task-1
```

---

## 27. Interaction with IO / networking

`task/sync/flow` must precede IO/networking.

Future IO rules:

- blocking read/write are cancellation points;
- timeout uses same scheduler deadline model;
- close during pending read/write wakes blocked tasks;
- native handles never leak as raw pointers;
- TCP/HTTP reuse `TimeoutError` and `CancelledError`.

VM-internal hooks needed:

```text
register_awaitable(task, readiness_source, deadline?)
wake_task(task_id)
cancel_wait(task_id)
```

---

## 28. Observability events

Minimum task/sync events:

```text
task.started
task.blocked
task.resumed
task.cancelled
task.completed
task.failed

strand.enqueued
strand.migrated

channel.send
channel.recv
channel.close

mutex.wait
mutex.lock
mutex.unlock

atomic.cas
```

Flow events:

```text
flow.scatter.start
flow.scatter.partition
flow.worker.start
flow.worker.done
flow.worker.failed
flow.gather.start
flow.gather.done
flow.reduce.start
flow.reduce.done
flow.cancel
```

Unsafe events:

```text
unsafe.region.enter
unsafe.region.exit
unsafe.spawn
unsafe.channel
unsafe.policy.denied
```

Tracing must not change shareability rules or scheduling result unless deterministic scheduler profile is explicitly enabled.

---

## 29. Error registry

Required errors:

```text
TaskError
TaskNotDoneError
TaskFailedError
CancelledError
TimeoutError
IsolationError

ChannelError
ChannelClosedError

MutexError
DeadlockError
OwnershipError

AtomicError
AtomicCompatibilityError

FlowError
FlowCancelledError
FlowPartitionError
FlowGatherError

SchedulerError
InvalidTaskStateError

UnsafeConcurrencyError
UnsafeConcurrencyDeniedError
DataRaceDetectedError
```

Inheritance proposal:

```text
RuntimeError
  TaskError
    TaskNotDoneError
    TaskFailedError
    CancelledError
    TimeoutError
  IsolationError
  ChannelError
    ChannelClosedError
  MutexError
    DeadlockError
    OwnershipError
  AtomicError
    AtomicCompatibilityError
  FlowError
    FlowCancelledError
    FlowPartitionError
    FlowGatherError
  SchedulerError
    InvalidTaskStateError
  UnsafeConcurrencyError
    UnsafeConcurrencyDeniedError
    DataRaceDetectedError
```

Notes:

- `TimeoutError` shared by task, IO, networking.
- `CancelledError` shared by task, IO, networking.
- `IsolationError` shared by ownership runtime and task/sync.
- `DataRaceDetectedError` optional sanitizer/debug profile; release VM need not detect all races.

---

## 30. API examples

### 30.1. Same-strand producer/consumer

```amber
async |task|:
  rows = []

  producer = task.async |child|:
    100.times |i|:
      rows << i
      child.yield()

  consumer = task.async |child|:
    loop:
      child.yield()
      if rows.count() >= 100:
        break rows.reduce(0) |acc, x|: acc + x

  consumer.wait()
```

### 30.2. Cross-strand computation

```amber
async |task|:
  input = [1, 2, 3].freeze()

  worker = task.spawn |child|:
    input.reduce(0) |acc, x|:
      acc + x

  worker.wait()
```

### 30.3. Checked channel pipeline

```amber
from sync import Channel

async |task|:
  ch = Channel.new(capacity: 16)

  producer = task.spawn |child|:
    [1, 2, 3].freeze().each |x|:
      ch.send(x)
    ch.close()

  consumer = task.async |child|:
    total = 0
    loop:
      try:
        total = total + ch.recv()
      catch ChannelClosedError:
        break total

  consumer.wait()
```

### 30.4. Unchecked shared state with explicit mutex

```amber
from sync import Mutex

async |task|:
  m = Mutex.new()
  shared = []

  workers = (1..4).map |i|:
    task.spawn(isolation: :unchecked) |child|:
      m.synchronize:
        shared << i

  workers.each |h|:
    h.wait()

  shared.count()
```

### 30.5. Atomic counter

```amber
from sync import Atomic

async |task|:
  counter = Atomic.new(0)

  workers = (1..4).map |i|:
    task.spawn |child|:
      1000.times:
        counter.update |x|: x + 1

  workers.each |h|:
    h.wait()

  counter.get()
```

### 30.6. Scatter map

```amber
from task.flow import scatter_map

async |task|:
  files = discover_files().freeze()

  parsed = scatter_map(files, workers: 8) |file|:
    parse_file(file)

  parsed.count()
```

### 30.7. Scatter reduce

```amber
from task.flow import scatter_reduce

total = scatter_reduce(items, init: 0, workers: 8) |partition|:
  partition.reduce(0) |acc, x|:
    acc + score(x)
```

### 30.8. Unchecked scatter map

```amber
from task.flow import scatter_map
from sync import Mutex

m = Mutex.new()
shared = []

scatter_map(items, workers: 8, isolation: :unchecked) |x|:
  value = compute(x)
  m.synchronize:
    shared << value
```

---

## 31. Conformance corpus

### 31.1. Scheduler corpus

```text
task_async_same_strand_mutable_capture_ok
task_spawn_mutable_capture_isolation_error
task_spawn_shareable_capture_ok
task_yield_allows_sibling_progress
task_sleep_timer_resume
task_wait_returns_result
task_wait_reraises_failure
task_wait_timeout
task_cancel_sleeping_child
task_cancel_waiting_child
structured_auto_join
structured_sibling_cancel_on_failure
task_stack_trace_deterministic
```

### 31.2. Channel corpus

```text
channel_unbuffered_send_recv
channel_buffered_fifo
channel_waiting_senders_fifo
channel_waiting_receivers_fifo
channel_close_idempotent
channel_send_after_close_error
channel_recv_closed_buffered_drains
channel_recv_closed_empty_error
channel_send_non_shareable_isolation_error
channel_recv_cancellation
channel_send_timeout
channel_recv_timeout
```

### 31.3. Mutex corpus

```text
mutex_lock_unlock
mutex_non_owner_unlock_error
mutex_double_lock_same_task_deadlock_error
mutex_waiter_fifo
mutex_synchronize_returns_block_value
mutex_synchronize_unlocks_on_exception
mutex_lock_wait_cancellation
```

### 31.4. Atomic corpus

```text
atomic_get_set
atomic_compare_and_set_success
atomic_compare_and_set_failure
atomic_update_returns_new_value
atomic_update_retries
atomic_rejects_non_compatible_payload
atomic_cross_strand_counter
```

### 31.5. Ownership corpus

```text
spawn_capture_array_rejected
spawn_capture_map_rejected
spawn_capture_user_object_rejected
spawn_capture_frozen_tuple_ok
spawn_capture_closure_with_confined_capture_rejected
cross_strand_send_confined_object_rejected
cross_strand_sync_object_ok
foreign_strand_ivar_load_isolation_error
foreign_strand_method_send_isolation_error
```

### 31.6. Flow corpus

```text
flow_scatter_map_ordered_result
flow_scatter_reduce_sum
flow_scatter_first_failure_cancels_siblings
flow_scatter_collect_failures
flow_scatter_timeout
flow_scatter_parent_cancellation
flow_broadcast_shareable_value
flow_rejects_non_shareable_partition_safe_mode
flow_unchecked_allows_non_shareable_partition
flow_trace_events_deterministic
flow_partition_chunks
flow_partition_stride
flow_partition_custom
```

### 31.7. Unsafe concurrency corpus

Checked-mode tests:

```text
spawn_capture_mutable_array_checked_rejected
spawn_capture_user_object_checked_rejected
channel_send_mutable_checked_rejected
flow_mutable_partition_checked_rejected
```

Unchecked-mode tests:

```text
spawn_capture_mutable_array_unchecked_allowed
spawn_capture_user_object_unchecked_allowed
unchecked_spawn_sets_module_feature_flag
unchecked_spawn_requires_loader_permission
unchecked_channel_allows_non_shareable_payload
unchecked_flow_allows_non_shareable_partition
unchecked_region_still_checks_destroyed_access
unchecked_region_still_checks_use_after_free
unchecked_region_still_runs_write_barriers
unchecked_region_stack_trace_marks_unsafe_boundary
```

Optional sanitizer tests:

```text
race_sanitizer_detects_unsynchronized_array_mutation
race_sanitizer_detects_unsynchronized_ivar_write
```

---

## 32. Implementation backlog

### TASK-001 — Runtime object model for Task / Strand / Worker

Deliverables:

- `Task` struct;
- `TaskHandle` object;
- `Strand` runnable queue;
- worker pool;
- stable task IDs for diagnostics;
- state transition helpers.

DoD:

- root task can run to completion;
- deterministic task state dump;
- no raw pointer IDs in diagnostics.

---

### TASK-002 — `async` root scope

Deliverables:

- parser/HIR acceptance for root `async |task|:`;
- HIR `HAsyncRoot`;
- VM root strand creation;
- structured auto-join.

DoD:

- root async returns block result;
- root async auto-joins children;
- root async propagates child failure.

---

### TASK-003 — `task.async`

Deliverables:

- same-strand child creation;
- cooperative scheduling;
- same-strand mutable capture allowed;
- `TaskHandle` return.

DoD:

- sibling tasks interleave at yield/sleep/wait;
- shared Array within same strand works;
- no parallel access within same strand.

---

### TASK-004 — `task.spawn` checked mode

Deliverables:

- new strand creation;
- worker queue integration;
- capture shareability verification;
- `IsolationError` path;
- cross-worker execution.

DoD:

- spawned tasks can run in parallel;
- mutable captures rejected;
- shareable captures accepted.

---

### TASK-005 — unchecked spawn mode

Deliverables:

- `isolation: :unchecked` parsing/lowering;
- artifact feature flag `unsafe_concurrency`;
- frame unsafe region flag;
- loader policy checks;
- diagnostics/warnings.

DoD:

- mutable captures allowed only when explicit;
- unsafe artifact denied without permission;
- lifetime checks remain active;
- stack trace marks unsafe boundary.

---

### TASK-006 — Cancellation

Deliverables:

- cancellation flag;
- wake sleeping/waiting task on cancel;
- `CancelledError`;
- cancellation polling at safepoints;
- structured propagation.

DoD:

- cancellation visible in sleep/wait/channel/mutex;
- CPU loop can observe via `check_cancelled!`;
- sibling cancellation on failure.

---

### TASK-007 — Timeouts

Deliverables:

- scheduler deadline model;
- timer queue;
- `wait(timeout:)`;
- channel timeout hooks;
- mutex timeout hooks if included.

DoD:

- timeout deterministic under test clock;
- timeout does not automatically cancel child;
- timeout error class shared with IO later.

---

### TASK-008 — Channel

Deliverables:

- bounded buffer;
- unbuffered rendezvous;
- FIFO sender/receiver queues;
- close semantics;
- shareability check;
- unchecked channel mode.

DoD:

- all channel corpus green;
- no send after close;
- closed buffered channel drains before error;
- unchecked channel requires unsafe feature flag.

---

### TASK-009 — Mutex

Deliverables:

- non-reentrant mutex;
- owner tracking;
- waiter FIFO;
- `synchronize` unwind guard.

DoD:

- same owner double lock gives `DeadlockError`;
- non-owner unlock gives `OwnershipError`;
- `synchronize` unlocks during exception/cancellation unwind.

---

### TASK-010 — Atomic

Deliverables:

- seq-cst cell;
- `get`;
- `set`;
- `compare_and_set`;
- `update` CAS loop;
- atomic-compatible payload guard.

DoD:

- atomic counter across spawned strands works;
- incompatible mutable payload rejected;
- `update` retry behavior tested.

---

### TASK-011 — Flow API

Deliverables:

- `scatter`;
- `scatter_map`;
- `scatter_reduce`;
- `broadcast`;
- `gather`;
- partitioning policies;
- ordered gather;
- failure policies.

DoD:

- flow corpus green;
- ordered result stable;
- failure propagation deterministic;
- cancellation propagates to flow workers.

---

### TASK-012 — HIR/bytecode/verifier hooks

Deliverables:

- concurrency HIR nodes;
- flow HIR nodes;
- concurrency opcodes or intrinsic sends;
- safepoint metadata;
- root maps for blocked tasks;
- handler table validation;
- unsafe region verification.

DoD:

- malformed concurrency bytecode rejected before execution;
- safepoints present on loops/blocking ops;
- stack/root maps valid during blocked task GC;
- unsafe concurrency denied without explicit feature flag.

---

### TASK-013 — Diagnostics and error registry

Deliverables:

- canonical error classes;
- diagnostic codes for static isolation violations;
- warnings for unchecked concurrency;
- deterministic task/flow stack traces;
- fixture normalizer support.

DoD:

- negative corpus stable;
- no raw addresses;
- stable task/strand/flow labels in golden output.

---

### TASK-014 — Observability hooks

Deliverables:

- event emission points;
- trace context metadata propagation;
- event normalization for tests;
- unsafe boundary trace events.

DoD:

- task/channel/mutex/atomic/flow event corpus green;
- tracing does not change semantics unless deterministic scheduler profile is enabled.

---

## 33. Release gates

### Gate A — single-strand async

Must pass:

```text
async root
task.async
yield
sleep
wait
same-strand mutable capture
structured auto-join
```

Exit criterion:

```text
Cooperative async works without cross-strand parallelism.
```

---

### Gate B — no-GIL spawn checked mode

Must pass:

```text
task.spawn
worker pool
new strands
shareability checks
IsolationError
parallel strand smoke test
```

Exit criterion:

```text
Multiple strands execute on multiple workers without global interpreter lock under safe default rules.
```

---

### Gate C — sync primitives

Must pass:

```text
Channel
Mutex
Atomic
cancellation while blocked
timeouts
FIFO guarantees
```

Exit criterion:

```text
User code can safely coordinate spawned strands.
```

---

### Gate D — unchecked concurrency

Must pass:

```text
unchecked spawn
unchecked channel
unsafe_concurrency artifact flag
loader policy denial
lifetime checks inside unsafe region
stack traces with unsafe boundary
```

Exit criterion:

```text
System/performance-critical code can explicitly bypass isolation while VM safety remains intact.
```

---

### Gate E — scatter/gather flow

Must pass:

```text
scatter_map
scatter_reduce
broadcast
gather ordered results
failure policy
flow cancellation
flow unchecked mode
```

Exit criterion:

```text
MPI-like local threading workflows are available on top of no-GIL strands.
```

---

### Gate F — full conformance

Must pass:

```text
scheduler corpus
channel corpus
mutex corpus
atomic corpus
ownership corpus
flow corpus
unsafe concurrency corpus
deterministic stack traces
runtime error registry
```

Exit criterion:

```text
task/sync/flow layer is stable enough for Watch, IO, networking and HTTP layers.
```

---

## 34. Documentation warning for unchecked APIs

Every unchecked API page must contain this warning:

```text
`isolation: :unchecked` disables Amber's strand isolation checks for this operation.
It does not make ordinary objects thread-safe.
Use Mutex, Atomic, Channel, or another explicit synchronization mechanism to establish happens-before edges.
Programs using unchecked concurrency can observe data races and nondeterministic Amber-level state.
VM memory safety, GC correctness and lifetime checks remain mandatory.
```

---

## 35. Open implementation choices

The following are implementation choices, not language-level semantic changes:

1. Whether `async |task|:` is parsed as special AST node or ordinary call before intrinsic resolution.
2. Whether flow API lowers to dedicated `FLOW_*` opcodes or to `SPAWN_NEW` + `WAIT` + runtime library calls.
3. Whether `task.unsafe_spawn` alias is included in v1 or only `task.spawn(isolation: :unchecked)`.
4. Whether `Channel.new(isolation: :unchecked)` is v1 or staged after unchecked spawn.
5. Whether debug race sanitizer ships with reference VM or as profile-specific tool.
6. Whether flow worker-group reuses strands or always spawns fresh strands.

Recommended v1 decisions:

```text
Use canonical keyword form: task.spawn(isolation: :unchecked)
Include Channel.new(isolation: :unchecked)
Use HIR flow nodes even if bytecode lowers to primitive spawn/wait
Do not recursively retag object graphs for unsafe sharing
Use frame-level unsafe region flag
Keep race sanitizer optional
```

---

## 36. Immediate first issues

Recommended issue order:

1. `ASYNC-001` Task/Strand/Worker runtime structs.
2. `ASYNC-002` Root async scope and task context.
3. `ASYNC-003` Same-strand `task.async`.
4. `ASYNC-004` Checked `task.spawn` and shareability gate.
5. `ASYNC-005` `TaskHandle.wait/result/failure/cancel`.
6. `ASYNC-006` Scheduler safepoints and cancellation polling.
7. `SYNC-001` Channel checked mode.
8. `SYNC-002` Mutex and `synchronize` unwind safety.
9. `SYNC-003` Atomic seq-cst API.
10. `UNSAFE-001` `isolation: :unchecked` spawn mode.
11. `UNSAFE-002` unsafe artifact flag and loader policy.
12. `FLOW-001` `gather(handles)`.
13. `FLOW-002` `scatter_map` ordered result.
14. `FLOW-003` `scatter_reduce`.
15. `FLOW-004` flow failure/cancellation policy.
16. `TEST-001` scheduler/channel/mutex/atomic corpus.
17. `TEST-002` flow/unsafe concurrency corpus.
18. `DOC-001` unsafe concurrency warnings and examples.

---

## 37. Definition of done

The threading/async API layer is done when:

1. Public API is documented with examples.
2. HIR lowering is deterministic.
3. Bytecode/verifier accepts valid task/sync/flow programs.
4. Malformed bytecode is rejected before execution.
5. Scheduler runs multiple strands on multiple workers without GIL.
6. Checked mode reliably enforces `IsolationError`.
7. Unchecked mode is explicit and artifact-marked.
8. Lifetime/GC safety is preserved inside unsafe regions.
9. Channel FIFO and close semantics pass corpus.
10. Mutex non-reentrant semantics pass corpus.
11. Atomic seq-cst semantics pass corpus.
12. Scatter/gather preserves order by default.
13. Flow failure/cancellation policies pass corpus.
14. Stack traces are deterministic.
15. Diagnostics contain no raw pointer values.
16. Host policy can deny unsafe concurrency.
17. Documentation clearly separates safe default from unsafe escape hatch.

---

## 38. Final implementation stance

Amber should keep safe strand isolation as the default because it gives ordinary users a predictable no-GIL programming model. However, because the VM is explicitly designed for real parallel execution without GIL, the runtime must also expose an explicit `isolation: :unchecked` mode for advanced/system code.

The correct split is:

```text
Default user code:
  checked isolation + IsolationError + shareable/sync boundaries

Performance/system/HPC code:
  explicit unchecked isolation + manual synchronization + artifact policy flag
```

MPI-like scatter/gather belongs above this model as a structured high-level API. In checked mode it is safe and shareability-enforced. In unchecked mode it becomes a powerful low-level threading tool for code that deliberately wants shared mutable state and accepts synchronization responsibility.

This preserves Amber's design goals:

- no global interpreter lock;
- safe default concurrency;
- real parallelism;
- explicit unsafe escape hatch;
- deterministic conformance;
- strong VM memory safety.
