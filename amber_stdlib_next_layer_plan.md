# Amber Stdlib: план и приоритеты следующего слоя

## Контекст

Язык Amber v20.1 уже зафиксирован. Следующий слой развития — стандартная библиотека и runtime-facing API вокруг уже существующих решений:

- базовые структуры данных и их методы;
- `task` / threading / async primitives;
- `Kernel.watch` / notebook watch profile;
- `io` и networking;
- error registry и conformance corpus.

Главный принцип: не делать `networking` раньше, чем закрыты collections, task/sync и IO resource contracts.

---

## Рекомендуемый порядок развития

```text
collections
  -> task/sync
  -> watch profile
  -> io foundation
  -> networking
  -> HTTP client
  -> advanced concurrency
```

---

# S1. Core collections stdlib

## Приоритет

**Самый высокий.**

Collections должны идти первыми, потому что ими будут пользоваться почти все остальные части stdlib: async, networking, watch, loader diagnostics, test runner и notebook runtime.

## Цель

Закрыть минимальный chainable API для:

- `Array`
- `Tuple`
- `Range`
- `Set`
- `Map`
- `LazySeq`

## Базовый Enumerable-like contract

```amber
collection.each |x|:
  ...

collection.map |x|:
  ...

collection.flat_map |x|:
  ...

collection.select |x|:
  ...

collection.reject |x|:
  ...

collection.reduce(init) |acc, x|:
  ...

collection.find |x|:
  ...

collection.any? |x|:
  ...

collection.all? |x|:
  ...

collection.none? |x|:
  ...

collection.first()
collection.count()
collection.to_a()
collection.lazy()
```

## Map contract

```amber
map.each |k, v|:
  ...

map.map |k, v|:
  ...

map.select |k, v|:
  ...

map.reject |k, v|:
  ...

map.transform |k, v|:
  (new_key, new_value)

map.transform_values |v, k|:
  ...

map.keys()
map.values()
map.entries()
```

## Error classes

Минимально нужны:

```text
EmptyCollectionError
IndexError
KeyError
ArgumentError
TypeError
```

## DoD

- green corpus для `Array`, `Tuple`, `Range`, `Set`, `Map`, `LazySeq`;
- тесты на пустые коллекции;
- тесты на mutation during iteration;
- тесты на lazy materialization;
- тесты на block arity;
- тесты на exception propagation из блока.

---

# S2. Task / threading / async modules

## Приоритет

**Второй после collections.**

Это не просто «удобная библиотека», а surface API поверх no-GIL runtime: strands, tasks, worker pool, structured concurrency, cancellation, channels, mutexes и atomics.

## Namespace proposal

```amber
import task
from sync import Channel, Mutex, Atomic
```

## Task API

```amber
handle = task.async:
  compute()
```

```amber
handle = task.spawn:
  compute_shareable()
```

```amber
task.sleep(0.1)
task.yield()
```

```amber
handle.wait()
handle.wait(timeout: 1.0)
handle.cancel()
handle.cancelled?()
handle.done?()
handle.result()
handle.failure()
```

## Semantic split

| API | Семантика |
|---|---|
| `task.async` | дочерняя task в том же strand |
| `task.spawn` | дочерняя task в новом strand |
| `task.sleep` | suspend current task |
| `task.yield` | cooperative yield |
| `handle.wait` | join/wait |
| `handle.cancel` | request cancellation |

## Channel API

```amber
ch = Channel.new(capacity: 16)

ch.send(value)
value = ch.recv()

ch.close()
ch.closed?()
```

## Channel rules

- FIFO;
- `send` cross-strand требует shareable payload;
- `recv` из closed empty channel бросает `ChannelClosedError`;
- `close()` idempotent или clearly specified;
- send после close бросает `ChannelClosedError`.

## Mutex API

```amber
m = Mutex.new()

m.lock()
m.unlock()

m.synchronize:
  critical_section()
```

## Mutex rules

- mutex non-reentrant;
- повторный `lock` той же task/strand бросает `DeadlockError`;
- unlock не-владельцем бросает `OwnershipError` или `RuntimeError`-подкласс;
- `synchronize` гарантирует unlock через unwind.

## Atomic API

```amber
a = Atomic.new(0)

a.get()
a.set(1)
a.compare_and_set(1, 2)

a.update |x|:
  x + 1
```

## Atomic rules

- seq-cst semantics в reference profile;
- payload должен быть atomic-compatible;
- `update` — stdlib sugar поверх CAS loop.

## DoD

- scheduler corpus;
- parallel strand tests;
- isolation violation tests;
- cancellation propagation tests;
- timeout tests;
- channel FIFO tests;
- non-reentrant mutex tests;
- atomic compare-and-set tests.

---

# S3. Watch profile

## Приоритет

**После task/sync, но до networking.**

Watch — это profile stdlib / notebook runtime layer, а не обычный core object method.

## Canonical API

Canonical spelling лучше оставить таким:

```amber
Kernel.watch(x)
Kernel.watch(@x)
Kernel.watch(@@x)
```

Не стоит вводить `object.watch` как core API на этом этапе.

## Почему не `object.watch`

`watch` выглядит как обычный method call, но на самом деле требует:

- compiler/kernel intrinsic recognition;
- restricted target grammar;
- special lowering;
- watch-cell replacement;
- revision tracking;
- notebook dependency capture.

Поэтому `object.watch` создаст ложное ожидание, что это обычный dynamic method.

## Допустимые targets

```amber
Kernel.watch(x)
Kernel.watch(@x)
Kernel.watch(@@x)
```

## Недопустимые targets

```amber
Kernel.watch(foo())
Kernel.watch(user.name)
Kernel.watch(xs[0])
Kernel.watch(1 + 2)
```

## Optional ergonomic alias

В Notebook profile можно добавить:

```amber
from notebook import watch

watch(x)
watch(@x)
watch(@@x)
```

Но lowering всё равно должен идти в canonical watch intrinsics.

## Internal runtime objects

```text
WatchCell
WatchObjectState
WatchEvent
WatchSubscriber
```

## Minimal WatchCell

```text
WatchCell(
  value,
  revision,
  subscribers
)
```

## Minimal WatchObjectState

```text
WatchObjectState(
  object_id,
  object_revision,
  field_revisions,
  subscribers
)
```

## Watch rules

- `Kernel.watch` не bump’ает `world_epoch`;
- watch не является world mutation;
- watch не меняет production semantics;
- object identity не меняется;
- class/equality/dispatch semantics не меняются;
- failed write не публикует watch event;
- successful watched ivar write bump’ает revision.

## Priority внутри watch

1. local/top-level binding watch;
2. ivar/class-var watch;
3. dependency capture для notebook cells;
4. object field revision tracking;
5. subscriber/event API;
6. debug inspection API.

## Не делать в первой итерации

- deep watch object graph;
- watch arbitrary expression;
- watch indexing;
- watch method call result;
- watch production semantics;
- `Object#watch` как обычный method.

---

# S4. IO foundation

## Приоритет

**Перед networking.**

Networking нельзя нормально сделать без общего IO/resource layer.

## Suggested module

```amber
import io
```

## Core protocols

```amber
reader.read(max_bytes:)
writer.write(bytes)
resource.close()
resource.closed?()
```

## Suggested interfaces/classes

```text
io.Reader
io.Writer
io.Closeable
io.Buffer
io.Bytes
io.ByteArray
io.Error
```

## Resource rules

- close должен быть idempotent или строго specified;
- read/write after close бросают `ClosedResourceError`;
- blocking operations должны иметь cancellation points;
- timeout должен быть согласован с `task` runtime;
- native handles не должны протекать как raw pointer.

## Minimal errors

```text
IOError
ClosedResourceError
TimeoutError
CancelledError
WouldBlockError
```

## DoD

- read/write/close corpus;
- cancellation during IO tests;
- timeout tests;
- close during pending read/write tests;
- native handle lifetime tests.

---

# S5. Low-level networking

## Приоритет

**После IO foundation.**

Первый networking layer должен быть низкоуровневым: TCP + DNS. HTTP лучше делать позже.

## Suggested modules

```amber
from net import TcpListener, TcpStream
```

## TcpListener API

```amber
listener = TcpListener.bind("127.0.0.1", 8080)

loop:
  conn = listener.accept()
  task.async:
    handle(conn)
```

## TcpStream API

```amber
stream = TcpStream.connect("example.com", 80, timeout: 5.0)

stream.write(bytes)
chunk = stream.read(max_bytes: 4096)

stream.close()
```

## DNS API

```amber
from net import dns

addresses = dns.resolve("example.com")
```

## Minimal networking errors

```text
NetworkError
ConnectionError
ConnectionRefusedError
ConnectionResetError
AddressInUseError
DnsError
TimeoutError
CancelledError
ClosedResourceError
```

## Rules

- all blocking operations are cancellation points;
- all blocking operations can support timeout;
- TCP streams implement `io.Reader`, `io.Writer`, `io.Closeable`;
- no raw socket handle exposure in safe stdlib;
- OS-specific errors normalize into Amber error classes.

## DoD

- connect success/failure tests;
- accept loop tests;
- cancellation during connect/read/write;
- timeout during connect/read/write;
- close during blocked read;
- DNS success/failure;
- deterministic error normalization.

---

# S6. HTTP client

## Приоритет

**После TCP + IO.**

HTTP should be client-only first.

## Suggested module

```amber
from net.http import Client, Request
```

## Minimal API

```amber
client = Client.new(timeout: 10.0)

res = client.get("https://example.com")

res.status
res.headers
res.body_bytes()
res.body_text()
```

## Request API

```amber
req = Request.new(
  method: "POST",
  url: "https://example.com/api",
  headers: {"content-type": "application/json"},
  body: bytes
)

res = client.send(req)
```

## HTTP response

```text
Response(
  status,
  headers,
  body
)
```

## Rules

- redirects off by default or explicitly configured;
- body reading is cancellation-aware;
- timeout covers connect + read unless split later;
- no server framework in first HTTP layer;
- TLS can be feature-gated if implementation host is not ready.

## DoD

- GET success;
- POST success;
- timeout;
- cancellation;
- connection failure;
- invalid URL;
- header normalization;
- body streaming later, not first.

---

# S7. Advanced concurrency

## Приоритет

**После basic task/sync/networking.**

Это уже не минимальный stdlib layer, а production-grade concurrency profile.

## Candidates

```amber
task.select:
  case ch1.recv():
    ...
  case ch2.recv():
    ...
```

```amber
task.with_timeout(5.0):
  operation()
```

```amber
supervisor = task.Supervisor.new(policy: :one_for_one)
```

```amber
moved = move(value)
```

## Не включать в первый stdlib release

- `select`;
- `move`;
- supervisor policies;
- async file IO;
- stream backpressure protocol;
- structured service runtime;
- actor framework.

---

# Roadmap matrix

| Этап | Слой | Что входит | Почему сейчас |
|---|---|---|---|
| S1 | collections core | `Array/Tuple/Range/Set/LazySeq/Map`, `Enumerable`, errors | база для всего stdlib |
| S2 | sync/task runtime | `task`, `TaskHandle`, `Channel`, `Mutex`, `Atomic` | обязательная no-GIL поверхность |
| S3 | watch profile | `Kernel.watch`, watch cells, watchable object state, dependency capture | optional notebook/IDE слой без изменения production semantics |
| S4 | io foundation | `Reader/Writer/Closeable`, bytes/buffer, resource close, timeout/cancel hooks | мост к networking |
| S5 | networking low-level | TCP, DNS minimal, TLS later or feature-gated | требует async readiness |
| S6 | HTTP client | request/response, headers, body APIs | первый полезный net API |
| S7 | advanced concurrency | `select`, `move`, supervisor policies, async I/O polishing | за пределами минимального v1 runtime |

---

# Concrete backlog

## Collections

- `STD-001` Collection protocol conformance suite.
- `STD-002` Array/Tuple/Range/Set eager methods. Done: finite
  `Range`, exclusive-end `Range`, and open-ended `Range` edge cases are
  covered by the VM collection dispatch and stdlib collection tests.
- `STD-003` LazySeq pipeline and materialization. Done: lazy wrappers now
  defer `map` / `flat_map` / `select` / `reject`, terminal operations
  materialize or short-circuit the pipeline, and open-ended `Range.lazy`
  supports bounded `first(count)` while rejecting unbounded materialization.
- `STD-004` Map iteration and transform contract. Done: `Map` preserves
  insertion order for `keys` / `values` / `entries` / `to_a`, supports
  symbol/string key lookup, passes `k, v` to iteration/filter/map blocks,
  returns `Array` from `map`, returns `Map` from `select` / `reject`, supports
  key-changing `transform`, and lets `transform_values` blocks read `k` as an
  optional second argument while preserving keys.
- `STD-005` Suitable collections operations. Done: eager finite
  collections now support `union`, `intersection`, `difference`,
  `left_difference`, `symmetric_difference`, subset/superset/disjoint
  predicates, `contains?` / `include?`, `permutation(count)`, and
  `combination(count)`, plus operator aliases `&`, `|`, `-`, `^`,
  `<` / `<=` / `>` / `>=`, `+` concatenation, `*` repetition, `concat`,
  `take_while`, `reverse`, `sort`, and `uniq` with an optional block;
  `each(size, step:)`, `each_pair`, and `each_cons(size)` provide
  Ruby-friendly window iteration; `Set` preserves set result shape for
  set-like operations, finite `LazySeq` materializes these operations, and
  `Map#merge` / `Map#+` / `Map#|` preserve insertion order with right-wins or
  block-resolved conflicts.
- Ruby-friendly and common aliases are covered for collections: `collect`,
  `collect_concat`, `filter`, `find_all`, `detect`, `inject`, `member?`,
  `includes?`, `each_slice`, `entries`, and `count` / `length` / `size`; the
  size trio is also available as read-only collection properties.
- `STD-006` Collection error registry and edge cases. Done: the collection
  error surface is pinned to `EmptyCollectionError`, `IndexError`, `KeyError`,
  `ArgumentError`, and `TypeError`; ordinary sequence/range/lazy indexing now
  reports `IndexError` on out-of-bounds access, `Map#[]` reports `KeyError`
  for absent keys, `Map#contains?` / `Map#include?` provide non-raising key
  presence checks, and the stdlib collections suite covers these negative
  edges.

## Task (Async or Threading) / Synchronization primitives

Detailed project see in amber_threading_async_api_project_v1.md

- `STD-010` Task module public API. Done: `RuntimeTaskModule` exposes
  `async`, `spawn`, `sync`, `sleep`, and `yield_current` over the existing
  scheduler, returning `RuntimeTaskHandle` values with task/strand ids,
  `wait(timeout)`, non-blocking `result`, `failure`, `cancel`, `resume`, and
  state predicates;
  `sync` creates a non-yielding synchronous block for future `sync:` /
  `task.sync:` lowering inside async scopes; `stdlib_task_tests` covers success,
  timeout without auto-cancel, cooperative cancellation, sync-block yield
  suppression, and failure surfacing.
- `STD-011` TaskHandle state/result/failure API. Done:
  `RuntimeTaskHandleState`, `RuntimeTaskHandleSnapshot`, `state()`, and
  `snapshot()` expose inactive/running/terminal states; `wait`, `result`, and
  `failure` carry state and canonical `TaskNotDoneError`, `TaskFailedError`,
  `CancelledError`, `TimeoutError`, and `LifetimeError` surfaces; task tests
  cover inactive handles, unfinished non-blocking reads, success, failure, and
  cancellation.
- `STD-012` Channel API and FIFO corpus. Done: `RuntimeChannel` exposes
  bounded buffered and rendezvous construction, `send`, move-aware `send`,
  `recv`, `close`, `closed`, and `stats`; checked sends reject non-shareable
  payloads with `IsolationError`; close/empty recv and send-after-close report
  `ChannelClosedError`; `stdlib_task_tests` covers buffered FIFO, waiting
  sender FIFO, waiting receiver FIFO, close idempotency, closed-channel drain
  semantics, send/recv timeouts, receive cancellation, and isolation
  rejection.
- `STD-013` Mutex API plus `synchronize`. Done: `RuntimeMutex` exposes
  `lock`, `unlock`, `locked`, `owned`, `synchronize`, and `stats`; same-owner
  double lock reports `DeadlockError`, unlocked/non-owner unlock reports
  `OwnershipError`, waiting lockers acquire FIFO, `synchronize` returns the
  block value and unlocks on exception unwind, and cancellation-aware lock waits
  are covered by `stdlib_task_tests`.
- `STD-014` Atomic API plus `update`. Done: `RuntimeAtomic` keeps the existing
  integer `get` / `set` / `compare_and_set` facade and adds value-level
  `get_value`, `set_value`, `compare_and_set_value`, and `update`; atomic
  payload writes reject non-compatible confined values with
  `AtomicCompatibilityError`, heap CAS compares by identity, `update` returns
  the replacement value after a seq-cst CAS loop and can re-run the block on
  contention, and `stdlib_task_tests` covers guard failures, deterministic
  retry, and a cross-strand counter.
- `STD-015` Inter-thread communication: send/receive, barrier,
  scatter-gather/map-reduce. Done: `RuntimeBarrier` adds reusable generation
  barriers with timeout/cancellation-aware waits and stats;
  `RuntimeFlowModule` adds ordered `gather`, `scatter`, `scatter_map`,
  `scatter_reduce`, and `broadcast` over `RuntimeTaskHandle` workers with
  checked/unchecked isolation modes, default first-failure cancellation,
  collect/ignore failure policies, shareability validation for partitions and
  worker results, flow stats, and `stdlib_task_tests` coverage for ordered
  gather, reduce, broadcast, failure collection, and isolation edges.
- `STD-016` Auto-parallel collections iteration/combination/permutation
  methods. Done: `RuntimeThreadedCollection` provides the runtime-facing
  facade for `[1, 2, 3].threaded(3).map: ...` style lowering, backed by
  `RuntimeFlowModule`; it supports ordered `each`, `map`, `select`, `reject`,
  `flat_map`, `combination(count)`, and `permutation(count)`, preserves checked
  isolation by default, supports explicit unchecked mode via `RuntimeFlowOptions`,
  surfaces worker failures through the existing flow failure policies, records
  threaded collection and flow stats, and `stdlib_task_tests` covers ordered
  transforms, auto-parallel each, generated combinations/permutations,
  checked/unchecked isolation, result shareability rejection, and failure
  collection.

## Watch

- `STD-020` Notebook watch target diagnostics.
- `STD-021` WatchCell storage replacement.
- `STD-022` WatchObjectState and ivar revision events. Done:
  `Kernel.watch(@ivar)` lowers/emits through `HWatchIvar` / `WATCH_IVAR`;
  runtime instances carry `RuntimeWatchObjectState` with stable object ids,
  object revisions, field revisions, and field subscribers; successful watched
  ivar writes publish `watch.ivar.write` events with old/new values plus
  field/object revision deltas without bumping `world_epoch`; failed writes do
  not publish events; `hir_tests`, `emitter_tests`, and `vm_tests` cover the
  path.
- `STD-023` Dependency capture API for notebook host.
  Done: `RuntimeWorld` exposes `begin_dependency_capture(cell_id)`,
  `end_dependency_capture()`, and `dependency_capture_snapshot()`; watched
  binding reads through locals/upvalues and watched ivar reads publish
  deterministic `RuntimeDependencySet` entries keyed by watch cell or
  object/field revision without bumping `watch_epoch` or `world_epoch`;
  `vm_tests` covers binding reads, repeated-read de-duplication, nested method
  ivar reads, and capture shutdown.

## IO

- `STD-030` IO resource protocol.
- `STD-031` Byte buffer / immutable bytes story.
- `STD-032` Scheduler readiness bridge.

## Networking

- `STD-040` TCP listener/stream.
- `STD-041` DNS resolution.
- `STD-042` HTTP client minimal.

---

# Hard recommendation

Не начинать с networking.

Правильный порядок:

```text
collections
  -> task/sync
  -> watch
  -> io
  -> networking
```

`object.watch` как user-facing method лучше не вводить в core stdlib сейчас. Он будет выглядеть как обычный method call, но по семантике требует compiler/kernel intrinsic, restricted target grammar и notebook-profile lowering.

Canonical form:

```amber
Kernel.watch(target)
```

Ergonomic alias допустим только в Notebook profile:

```amber
from notebook import watch

watch(x)
```

Но не как `Object#watch` в core.
