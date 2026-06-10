# Amber IO API Project v2

**Тема:** конкретный API `ByteBuffer`, `Pipe`, `File IO` и `Socket IO` поверх базового IO-слоя Amber  
**Версия:** v2.0-project  
**Дата:** 10 июня 2026  
**Статус:** engineering blueprint для stdlib/runtime-facing реализации  
**Основание:** Amber unified language/runtime spec, Amber IO Layer Project v1, модель `Worker -> Strand -> Task`, checked isolation, capability/effect profiles, scheduler-aware cancellation/timeout semantics.

---

## Оглавление

- [0. Главная позиция](#0-главная-позиция)
- [1. Общая модель IO object](#1-общая-модель-io-object)
- [2. Base protocols](#2-base-protocols)
- [3. ByteBuffer API](#3-bytebuffer-api)
- [4. Bytes и ByteSlice](#4-bytes-и-byteslice)
- [5. Pipe API](#5-pipe-api)
- [6. File IO API](#6-file-io-api)
- [7. Socket IO API](#7-socket-io-api)
- [8. UDP API](#8-udp-api)
- [9. Timeout and cancellation](#9-timeout-and-cancellation)
- [10. Capability/effect model](#10-capabilityeffect-model)
- [11. Runtime ABI](#11-runtime-abi)
- [12. Многозадачность: единая таблица поведения](#12-многозадачность-единая-таблица-поведения)
- [13. Recommended examples](#13-recommended-examples)
- [14. Error hierarchy](#14-error-hierarchy)
- [15. Conformance additions for v2](#15-conformance-additions-for-v2)
- [16. Release gates v2](#16-release-gates-v2)
- [17. Итоговое решение](#17-итоговое-решение)

---

# 0. Главная позиция

`io`, `fs`, `net` должны быть **одним синхронно выглядящим API**, который внутри является scheduler-aware.

Пользователь пишет обычный Amber-код без `await`:

```amber
buf = io.ByteBuffer(8192)

fs.File.open("input.bin", :read) |f|:
 while n = f.read!(buf):
  process(buf.flip().bytes())
  buf.clear!()
```

Но `read!` внутри ведёт себя по-разному в зависимости от модели исполнения:

```text
same-strand async      -> suspends current Task, not Strand
checked spawn          -> parks Task on owning Strand/Worker-safe IO wait
unchecked spawn        -> allows shared descriptor only with unsafe boundary
flow/scatter           -> IO remains per-task cancellable and trace-visible
deterministic replay   -> external IO must pass through recorded providers
```

API не должен заставлять пользователя выбирать `sync` vs `async` варианты. В Amber v1 нет `await`, и IO-слой не должен добавлять его.

Опорные решения:

- IO — stdlib/runtime-facing слой без новой core syntax.
- Host-backed `File / Socket / Listener` — managed runtime resources.
- Blocking IO является task safepoint.
- Handles по умолчанию strand-confined.
- Cross-strand sharing разрешается только через explicit `isolation: :unchecked`.
- Filesystem/network IO gated через capability/effect checks.

---

# 1. Общая модель IO object

## 1.1. Runtime resource header

Каждый host-backed объект имеет скрытый runtime header:

```text
IoResourceHeader:
  resource_id: ResourceId
  kind: :file | :tcp_stream | :tcp_listener | :udp_socket | :pipe_endpoint
  owner_strand: StrandId?
  isolation_mode: :checked | :unchecked
  state: :open | :closing | :closed | :destroyed
  readable: Bool
  writable: Bool
  waiters: WaitQueue
  native_handle: NativeHandle?
  capability_token: CapabilityToken?
  effect_profile: EffectRow
  trace_tags: TraceTags
```

Наблюдаемое правило:

```text
closed?() == true       -> state is :closed or :destroyed
AlreadyClosedError      -> operation on closed resource
ResourceDestroyedError  -> unsafe/shared stale handle after runtime destruction
IsolationError          -> checked cross-strand access
```

## 1.2. Isolation matrix

```text
same task, same strand:
  allowed

different task, same strand:
  allowed, because execution is serialized by strand scheduler

different strand, checked:
  rejected with IsolationError for mutable buffers and handles

different strand, immutable Bytes / frozen Path / Endpoint:
  allowed

different strand, unchecked:
  allowed only in explicit unsafe concurrency region and loader policy must permit it

different worker, same strand:
  impossible as simultaneous execution; strand may migrate only at scheduler-defined safe boundary
```

Unchecked warning:

```text
isolation: :unchecked disables checked isolation.
It does not make objects logically thread-safe.
User code must establish happens-before through Mutex, Atomic, Channel, Pipe, or another synchronization primitive.
```

---

# 2. Base protocols

## 2.1. Protocol mixins

```amber
mixin io.Reader:
 def read!(buf as io.ByteBuffer, timeout: null) -> Int:...
 def try_read!(buf as io.ByteBuffer) -> Int?:...

 def read_exact!(count as Int, timeout: null) -> Bytes:...
 def read_all!(limit: null, chunk_size: 8192) -> Bytes:...
 def read_line!(limit: null, timeout: null) -> Str?:...
 def each_chunk(size: 8192, timeout: null) |chunk|:...

mixin io.Writer:
 def write!(bytes, timeout: null) -> Int:...
 def try_write!(bytes) -> Int?:...
 def write_all!(bytes, timeout: null) -> null:...
 def puts!(text, encoding: :utf8, timeout: null) -> null:...

mixin io.Closer:
 def close!() -> null:...
 def closed?() -> Bool:...

mixin io.Flushable:
 def flush!(timeout: null) -> null:...

mixin io.Seeker:
 def seek!(offset as Int, whence: :start) -> Int:...
 def tell!() -> Int:...
```

`!` остаётся обязательным для effectful/mutating IO operations.

## 2.2. Return contract

```text
read!(buf)
  n > 0  data read
  0      EOF
  wait   no data yet

try_read!(buf)
  n > 0  data read
  0      EOF
  null   would block

write!(bytes)
  n > 0  bytes written
  wait   resource currently cannot accept data

try_write!(bytes)
  n > 0  bytes written
  0      only for empty input
  null   would block
```

Ключевая норма:

```text
0 у buffer-based read означает только EOF.
would-block всегда null в try_read!.
```

---

# 3. ByteBuffer API

## 3.1. Цель

`ByteBuffer` — mutable, strand-confined рабочий буфер для zero-copy-ish чтения/записи внутри одного strand.

Для передачи между strands используются `Bytes` или frozen/copy slice.

## 3.2. Concrete API

```amber
class io.ByteBuffer:
 class_method def new(capacity as Int) -> ByteBuffer:...

 class_method def from(bytes) -> ByteBuffer:...
 class_method def wrap(bytes) -> ByteBuffer:...

 def capacity() -> Int:...
 def position() -> Int:...
 def limit() -> Int:...
 def count() -> Int:...
 def remaining() -> Int:...
 def empty?() -> Bool:...
 def full?() -> Bool:...

 def clear!() -> null:...
 def flip!() -> ByteBuffer:...
 def rewind!() -> ByteBuffer:...
 def compact!() -> ByteBuffer:...

 def get!() -> Int:...
 def get_at(index as Int) -> Int:...
 def put!(byte as Int) -> ByteBuffer:...
 def put_all!(bytes) -> ByteBuffer:...

 def read_slice(length: null) -> ByteSlice:...
 def write_slice(length: null) -> ByteSlice:...

 def slice(start as Int, length: null) -> ByteSlice:...
 def bytes() -> Bytes:...
 def copy_bytes() -> Bytes:...
 def freeze_bytes!() -> Bytes:...
```

## 3.3. Buffer modes

`ByteBuffer` имеет две фазы, но без отдельного enum-типа:

```text
write mode:
  position = next write position
  limit    = capacity
  remaining = writable bytes

read mode after flip!:
  limit    = old position
  position = 0
  remaining = readable bytes
```

Пример:

```amber
buf = io.ByteBuffer(8192)

n = file.read!(buf)
if n > 0:
 buf.flip!()
 chunk = buf.bytes()
 process(chunk)
 buf.clear!()
```

## 3.4. Ошибки

```text
capacity < 0              -> ArgumentError
capacity not Int          -> TypeError
read! with remaining == 0 -> ArgumentError
put! byte outside 0..255  -> ArgumentError
index out of bounds       -> IndexError
cross-strand access       -> IsolationError
```

---

# 4. Bytes и ByteSlice

## 4.1. Bytes

```amber
class Bytes:
 def count() -> Int:...
 def empty?() -> Bool:...
 def [](index as Int) -> Int:...
 def slice(start as Int, length: null) -> Bytes:...
 def to_str(encoding: :utf8) -> Str:...
 def hex() -> Str:...
```

Normative:

```text
Bytes immutable
Bytes shareable across checked strands
Bytes safe as message payload in Channel
```

## 4.2. ByteSlice

```amber
class io.ByteSlice:
 def count() -> Int:...
 def bytes() -> Bytes:...
 def copy_bytes() -> Bytes:...
 def owner() -> ByteBuffer?:...
 def shareable?() -> Bool:...
```

Правило:

```text
ByteSlice over mutable ByteBuffer is not shareable.
ByteSlice materialized as immutable Bytes is shareable.
```

Это важно для no-GIL: нельзя случайно передать view на mutable buffer в другой strand.

---

# 5. Pipe API

## 5.1. Public construction

```amber
reader, writer = io.Pipe.new(capacity: 64 * 1024)
```

или явно:

```amber
pipe = io.Pipe(capacity: 4096)
r = pipe.reader()
w = pipe.writer()
```

## 5.2. Classes

```amber
class io.Pipe:
 class_method def new(capacity: 65536, isolation: :checked) -> (PipeReader, PipeWriter):...

 def reader() -> PipeReader:...
 def writer() -> PipeWriter:...
 def capacity() -> Int:...
 def buffered() -> Int:...
 def closed?() -> Bool:...
 def close!() -> null:...

class io.PipeReader:
 include io.Reader, io.Closer

 def close!() -> null:...
 def closed?() -> Bool:...
 def pipe() -> Pipe:...

class io.PipeWriter:
 include io.Writer, io.Closer, io.Flushable

 def close!() -> null:...
 def closed?() -> Bool:...
 def pipe() -> Pipe:...
```

## 5.3. Capacity semantics

```text
capacity > 0   bounded buffered pipe
capacity == 0  rendezvous pipe
capacity < 0   ArgumentError
capacity other TypeError
```

## 5.4. Blocking semantics

```text
read!:
  waits when buffer empty and writer open
  returns EOF 0 when writer closed and buffer empty
  returns buffered bytes before EOF

write!:
  writes partial if capacity allows partial
  waits when buffer full
  raises BrokenPipeError if reader closed

close reader:
  wakes blocked writers with BrokenPipeError

close writer:
  wakes blocked readers; after buffer drains, read! returns 0

close pipe:
  closes both endpoints, wakes all waiters

close!:
  idempotent
```

## 5.5. Pipe under multitasking models

```text
same-strand async:
  Pipe wait queues are task queues; no OS blocking.

checked spawn:
  Pipe endpoints are sync resources only if created with shareable pipe state.
  Reader and Writer endpoints may cross strands if Pipe state lives in shared/sync space.

unchecked:
  Pipe may skip endpoint ownership checks, but buffer state must still be protected by runtime lock/atomic queue.

flow:
  Pipe can connect producer/consumer tasks in scatter pipelines.

deterministic:
  Pipe send/read ordering is recorded as channel-like operation order.
```

## 5.6. Recommended rule

`Pipe` should be **shareable sync object by default**, unlike `File` and `Socket`.

Reason: pipe is a coordination primitive, closer to `Channel` than to host descriptor. Internal ring buffer must live in shared/sync region. User payload still obeys shareability rules:

```text
write!(Bytes)        -> ok across checked strands
write!(ByteSlice)    -> ok only if shareable
write!(ByteBuffer)   -> TypeError or IsolationError unless same strand
```

---

# 6. File IO API

## 6.1. Path

```amber
class fs.Path:
 class_method def new(value) -> Path:...

 def /(part) -> Path:...
 def join(*parts) -> Path:...
 def basename() -> Str:...
 def extname() -> Str:...
 def parent() -> Path:...
 def absolute?() -> Bool:...
 def normalize() -> Path:...
 def to_str() -> Str:...
```

`Path#/` принят как canonical ergonomic path composition.

## 6.2. File open

```amber
class fs.File:
 include io.Reader, io.Writer, io.Seeker, io.Closer, io.Flushable

 class_method def open(
  path,
  mode = :read,
  create: false,
  truncate: false,
  append: false,
  exclusive: false,
  permissions: null,
  isolation: :checked
 ) |file|:
  ...
```

Supported modes:

```text
:read
:write
:append
:read_write
```

NameEnum rule:

```text
:read        ok
"read"       normalized to :read
:banana      ArgumentError
"banana"     ArgumentError
1            TypeError
```

## 6.3. File instance API

```amber
class fs.File:
 def path() -> fs.Path:...
 def mode() -> Symbol:...

 def read!(buf as io.ByteBuffer, timeout: null) -> Int:...
 def try_read!(buf as io.ByteBuffer) -> Int?:...

 def write!(bytes, timeout: null) -> Int:...
 def try_write!(bytes) -> Int?:...
 def write_all!(bytes, timeout: null) -> null:...

 def flush!(timeout: null) -> null:...
 def sync!(timeout: null) -> null:...

 def seek!(offset as Int, whence: :start) -> Int:...
 def tell!() -> Int:...
 def size!() -> Int:...
 def metadata!() -> fs.Metadata:...

 def close!() -> null:...
 def closed?() -> Bool:...
```

## 6.4. Convenience API

```amber
fs.exists?(path) -> Bool
fs.file?(path) -> Bool
fs.dir?(path) -> Bool
fs.metadata(path) -> fs.Metadata

fs.read_bytes(path, limit: null) -> Bytes
fs.write_bytes(path, bytes, create: true, truncate: true) -> null

fs.read_text(path, encoding: :utf8, limit: null) -> Str
fs.write_text(path, text, encoding: :utf8, create: true, truncate: true) -> null

fs.mkdir(path) -> null
fs.mkdir_p(path) -> null
fs.remove(path) -> null
fs.rename(from, to) -> null
fs.copy(from, to) -> Int
```

## 6.5. Validation

```text
:read + truncate:true              -> ArgumentError
:append + truncate:true            -> ArgumentError
exclusive:true without create:true -> ArgumentError
permissions not Int/null           -> TypeError
missing fs capability              -> CapabilityError
forbidden effect                   -> EffectViolationError
```

## 6.6. Block form

```amber
result = fs.File.open("data.bin", :read) |f|:
 f.read_all!(limit: 10_000_000)
```

Normative unwind:

```text
1. open resource
2. call block
3. close during ensure/unwind
4. return block result
5. if close fails after block success, raise close error
6. if both block and close fail, block error is primary; close error is suppressed/cause metadata
```

## 6.7. File under multitasking

```text
same-strand async:
  File read/write parks only current Task.

checked spawn:
  File object cannot cross strand. Passing it to spawned strand raises IsolationError.

unchecked spawn:
  File may cross strand only in isolation::unchecked and with unsafe_concurrency artifact flag.

flow:
  Each worker task should open its own File handle unless explicitly unchecked.

deterministic:
  Direct host File IO forbidden unless provider is recorded/mocked.
```

Recommended ergonomic pattern:

```amber
paths.scatter_map |path|:
 fs.read_bytes(path, limit: 50_000_000)
```

Not:

```amber
f = fs.File.open("shared.log", :append)
items.scatter_map |item|:
 f.write_all!(item.bytes()) # checked mode rejects cross-strand handle
```

For shared output:

```amber
mutex = sync.Mutex()
f = fs.File.open("shared.log", :append, isolation: :unchecked)

items.scatter_map(isolation: :unchecked) |item|:
 mutex.synchronize:
  f.write_all!(item.bytes())
```

---

# 7. Socket IO API

## 7.1. Endpoint

```amber
class net.Endpoint:
 class_method def new(host as Str, port as Int) -> Endpoint:...
 class_method def parse(value as Str) -> Endpoint:...

 def host() -> Str:...
 def port() -> Int:...
 def family() -> Symbol:... # :inet | :inet6 | :unix staged
 def to_str() -> Str:...
```

`Endpoint` should be immutable and shareable.

## 7.2. TCP connect

```amber
module net.tcp:
 def connect(host as Str, port as Int, timeout: null, isolation: :checked) -> TcpStream:...
 def connect(endpoint as net.Endpoint, timeout: null, isolation: :checked) -> TcpStream:...

 def listen(host as Str, port as Int, backlog: 128, reuse_addr: false, isolation: :checked) -> TcpListener:...
```

Usage:

```amber
net.tcp.connect("example.com", 443, timeout: 5.0) |s|:
 s.write_all!(request)
 s.read_all!(limit: 1_000_000)
```

## 7.3. TcpStream

```amber
class net.TcpStream:
 include io.Reader, io.Writer, io.Closer, io.Flushable

 def read!(buf as io.ByteBuffer, timeout: null) -> Int:...
 def try_read!(buf as io.ByteBuffer) -> Int?:...

 def write!(bytes, timeout: null) -> Int:...
 def try_write!(bytes) -> Int?:...
 def write_all!(bytes, timeout: null) -> null:...

 def flush!(timeout: null) -> null:...

 def local_endpoint() -> net.Endpoint:...
 def remote_endpoint() -> net.Endpoint:...

 def shutdown!(side = :both) -> null:...
 def close_read!() -> null:...
 def close_write!() -> null:...

 def set_option!(name, value) -> null:...
 def get_option(name):...

 def close!() -> null:...
 def closed?() -> Bool:...
```

`shutdown!` NameEnum:

```text
:read / "read"
:write / "write"
:both / "both"
```

## 7.4. TcpListener

```amber
class net.TcpListener:
 include io.Closer

 def accept!(timeout: null) -> net.TcpStream:...
 def try_accept!() -> net.TcpStream?:...

 def local_endpoint() -> net.Endpoint:...

 def set_option!(name, value) -> null:...
 def close!() -> null:...
 def closed?() -> Bool:...
```

Server example:

```amber
def handle_client(client):
 buf = io.ByteBuffer(8192)

 loop:
  n = client.read!(buf)
  if n == 0:
   break null

  buf.flip!()
  client.write_all!(buf.bytes())
  buf.clear!()

net.tcp.listen("127.0.0.1", 9000, reuse_addr: true) |listener|:
 loop:
  client = listener.accept!()
  task.async:
   handle_client(client)
```

Same-strand `task.async` works because accepted `client` remains on the same strand unless explicitly spawned elsewhere.

Checked parallel server:

```amber
listener = net.tcp.listen("127.0.0.1", 8080)

loop:
 client = listener.accept!()

 task.spawn |child|:
  # invalid in checked mode if client crosses strand directly
  handle_client(client)
```

Correct checked pattern:

```amber
listener = net.tcp.listen("127.0.0.1", 8080)

loop:
 client = listener.accept!()
 task.async |child|:
  handle_client(client)
```

or runtime-supported transfer profile, if later added. In v1/v2 here we keep no general `move(value)` ownership transfer.

Unchecked parallel server:

```amber
listener = net.tcp.listen("127.0.0.1", 8080)

loop:
 client = listener.accept!(isolation: :unchecked)

 task.spawn(isolation: :unchecked) |child|:
  handle_client(client)
```

This must emit unsafe-boundary trace metadata and require loader policy approval.

## 7.5. TCP options

```amber
stream.set_option!(:nodelay, true)
stream.set_option!("keepalive", true)
listener.set_option!(:reuse_addr, true)
```

Supported v2:

```text
:nodelay       TcpStream, Bool
:keepalive     TcpStream, Bool
:reuse_addr    TcpListener, Bool
:reuse_port    TcpListener, Bool, optional/platform
:recv_buffer   TcpStream/UdpSocket, Int
:send_buffer   TcpStream/UdpSocket, Int
```

Errors:

```text
unsupported option -> ArgumentError
wrong value type   -> TypeError
unsupported host   -> UnsupportedOperationError
closed socket      -> AlreadyClosedError
```

---

# 8. UDP API

## 8.1. UdpSocket

```amber
module net.udp:
 def bind(host as Str, port as Int, isolation: :checked) -> UdpSocket:...
 def open(family: :inet, isolation: :checked) -> UdpSocket:...

class net.UdpSocket:
 include io.Closer

 def recv_from!(max: 65507, timeout: null) -> (Bytes, net.Endpoint):...
 def try_recv_from!(max: 65507) -> (Bytes, net.Endpoint)?:...

 def send_to!(bytes, endpoint, timeout: null) -> Int:...
 def try_send_to!(bytes, endpoint) -> Int?:...

 def connect!(endpoint) -> null:...
 def recv!(max: 65507, timeout: null) -> Bytes:...
 def send!(bytes, timeout: null) -> Int:...

 def local_endpoint() -> net.Endpoint:...
 def set_option!(name, value) -> null:...

 def close!() -> null:...
 def closed?() -> Bool:...
```

## 8.2. UDP datagram contract

```text
recv_from! preserves datagram boundary
max too small raises DatagramTooLargeError unless truncate: true
try_recv_from! returns null on would-block
send_to! returns bytes accepted by kernel/runtime
```

Recommended signature extension:

```amber
sock.recv_from!(max: 1500, truncate: false, timeout: 1.0)
```

Validation:

```text
max <= 0       -> ArgumentError
max not Int    -> TypeError
endpoint wrong -> TypeError
```

---

# 9. Timeout and cancellation

## 9.1. Timeout values

Accepted:

```text
null          no explicit deadline
Int/Float > 0 seconds
Duration      if stdlib Duration exists
0             immediate poll-like attempt
negative      ArgumentError
other         TypeError
```

## 9.2. Deadline rule

For compound operations:

```amber
writer.write_all!(bytes, timeout: 5.0)
reader.read_exact!(4096, timeout: 5.0)
```

Timeout is a **single operation deadline**, not reset per internal chunk.

## 9.3. Cancellation rule

At every blocking IO safepoint:

```text
if task cancelled before parking:
  raise CancelledError

if task cancelled while parked:
  remove waiter
  raise CancelledError

if IO readiness and cancellation race:
  deterministic priority: cancellation wins unless operation already committed bytes
```

Commit rule:

```text
read/write returning n > 0 means operation committed.
After commit, cancellation is observed at next safepoint, not retroactively.
```

---

# 10. Capability/effect model

## 10.1. Effects

```text
:fs_read
:fs_write
:fs_metadata
:net_connect
:net_listen
:net_accept
:net_udp
:io_wait
```

## 10.2. Capabilities

```text
fs.read(path_scope)
fs.write(path_scope)
net.connect(host_scope, port_scope)
net.listen(bind_scope)
net.udp(bind_scope)
```

## 10.3. Error order

Recommended deterministic validation order:

```text
1. TypeError / ArgumentError for local arguments
2. EffectViolationError if static/dynamic effect row forbids operation
3. CapabilityError if permission missing
4. IsolationError if resource ownership invalid
5. AlreadyClosedError if resource closed
6. TimeoutError / CancelledError / IOError subclass during operation
```

Rationale: programmer errors before environment errors; policy errors before host IO errors.

---

# 11. Runtime ABI

## 11.1. HIR nodes

```text
HIoRead(resource, buffer, deadline)
HIoTryRead(resource, buffer)
HIoWrite(resource, bytes, deadline)
HIoTryWrite(resource, bytes)
HIoClose(resource)
HIoFlush(resource, deadline)
HIoAccept(listener, deadline)
HIoConnect(endpoint, deadline)
HIoUdpRecv(socket, max, flags, deadline)
HIoUdpSend(socket, bytes, endpoint, deadline)
```

Ordinary sends may lower to these when receiver/method are statically known. Otherwise keep tagged `HSend`.

## 11.2. Bytecode opcodes

```text
IO_READ
IO_TRY_READ
IO_WRITE
IO_TRY_WRITE
IO_CLOSE
IO_FLUSH
IO_ACCEPT
IO_CONNECT
IO_UDP_RECV
IO_UDP_SEND
```

## 11.3. Runtime entrypoints

```c
amber_io_read(Task* task, Resource* r, ByteBuffer* b, Deadline d) -> IoResult
amber_io_try_read(Task* task, Resource* r, ByteBuffer* b) -> IoResult

amber_io_write(Task* task, Resource* r, BytesLike* bytes, Deadline d) -> IoResult
amber_io_try_write(Task* task, Resource* r, BytesLike* bytes) -> IoResult

amber_io_close(Task* task, Resource* r) -> AmberValue
amber_io_accept(Task* task, Resource* listener, Deadline d) -> IoResult
amber_io_connect(Task* task, Endpoint* ep, Deadline d) -> IoResult
```

## 11.4. Wait record

```text
IoWaitRecord:
  task_id
  strand_id
  worker_id
  resource_id
  interest: :read | :write | :accept | :connect | :flush | :close
  deadline
  cancel_token
  buffer_pin?
  trace_span
```

GC requirement:

```text
task stack, suspend-state, pending wake records and native handles must be visible to GC.
```

---

# 12. Многозадачность: единая таблица поведения

| Модель Amber | `ByteBuffer` | `Pipe` | `File` | `TcpStream` / `UdpSocket` |
|---|---|---|---|---|
| `task.async` same-strand | allowed | allowed | allowed | allowed |
| `task.spawn` checked | cannot share mutable buffer | pipe endpoints shareable if sync-backed | handle rejected cross-strand | handle rejected cross-strand |
| `Channel` checked | only `Bytes`, not mutable buffer | endpoints allowed if shareable | rejected | rejected |
| `Mutex` checked | protects shared sync objects, not confined buffer | useful | cannot make confined handle shareable by itself | cannot make confined handle shareable by itself |
| `isolation: :unchecked` | allowed with unsafe risk | allowed | allowed with policy flag | allowed with policy flag |
| `scatter_map` | per-task local buffers | good for pipelines | open per task | connect per task |
| deterministic/replay | buffer local | pipe ordering recorded | provider-recorded only | provider-recorded only |

---

# 13. Recommended examples

## 13.1. File copy

```amber
def copy_file(src, dst, chunk_size: 64 * 1024):
 fs.File.open(src, :read) |input|:
  fs.File.open(dst, :write, create: true, truncate: true) |output|:
   buf = io.ByteBuffer(chunk_size)

   loop:
    n = input.read!(buf)
    if n == 0:
     break null

    buf.flip!()
    output.write_all!(buf.bytes())
    buf.clear!()
```

## 13.2. Pipe producer/consumer

```amber
reader, writer = io.Pipe.new(capacity: 8192)

producer = task.async:
 writer.write_all!("hello\n".bytes())
 writer.close!()

consumer = task.async:
 line = reader.read_line!()
 log line

producer.wait()
consumer.wait()
```

## 13.3. Checked scatter file processing

```amber
paths.scatter_map |path|:
 data = fs.read_bytes(path, limit: 100_000_000)
 process(data)
```

## 13.4. TCP echo server, same-strand task concurrency

```amber
def handle_client(client):
 buf = io.ByteBuffer(8192)

 loop:
  n = client.read!(buf)
  if n == 0:
   break null

  buf.flip!()
  client.write_all!(buf.bytes())
  buf.clear!()

net.tcp.listen("127.0.0.1", 9000, reuse_addr: true) |listener|:
 loop:
  client = listener.accept!()
  task.async:
   handle_client(client)
```

## 13.5. Parallel accept with unchecked descriptor sharing

```amber
mutex = sync.Mutex()

net.tcp.listen("127.0.0.1", 9000, isolation: :unchecked) |listener|:
 workers = 4.times.map:
  task.spawn(isolation: :unchecked):
   loop:
    client = mutex.synchronize:
     listener.accept!()

    task.async:
     handle_client(client)

 workers.each: _1.wait()
```

This is explicitly unsafe at descriptor-sharing boundary, but still VM-memory-safe: lifetime checks, GC correctness and write barriers remain mandatory.

---

# 14. Error hierarchy

```amber
class IOError < StandardError:...

class EOFError < IOError:...
class TimeoutError < IOError:...
class CancelledError < IOError:...

class AlreadyClosedError < IOError:...
class BrokenPipeError < IOError:...
class ConnectionError < IOError:...
class ConnectionRefusedError < ConnectionError:...
class ConnectionResetError < ConnectionError:...
class NetworkUnreachableError < ConnectionError:...
class AddressInUseError < IOError:...
class DatagramTooLargeError < IOError:...

class FileNotFoundError < IOError:...
class PermissionDeniedError < IOError:...
class IsDirectoryError < IOError:...
class NotDirectoryError < IOError:...

class CapabilityError < SecurityError:...
class EffectViolationError < SecurityError:...
class IsolationError < RuntimeError:...
class ResourceDestroyedError < RuntimeError:...
```

---

# 15. Conformance additions for v2

```text
bytebuffer_flip_clear_roundtrip
bytebuffer_slice_not_shareable_when_backed_by_mutable_buffer
bytebuffer_freeze_bytes_shareable_cross_strand
bytebuffer_cross_strand_checked_rejected

pipe_endpoint_checked_spawn_allowed_when_sync_backed
pipe_payload_mutable_buffer_checked_rejected
pipe_payload_bytes_checked_allowed
pipe_close_wakes_all_waiters

file_handle_spawn_checked_rejected
file_handle_spawn_unchecked_allowed_with_policy
file_write_all_timeout_is_single_deadline
file_close_wakes_blocked_read

tcp_stream_spawn_checked_rejected
tcp_stream_spawn_unchecked_allowed_with_policy
tcp_accept_close_wakes_waiter
tcp_read_cancel_race_committed_bytes_rule

udp_recv_datagram_too_large_error
udp_recv_truncate_true_returns_truncated_packet
udp_endpoint_is_shareable

deterministic_file_io_without_provider_error
deterministic_pipe_order_recorded
trace_marks_io_wait_records
```

---

# 16. Release gates v2

## Gate A — buffers and base protocols

```text
ByteBuffer state machine
Bytes/ByteSlice shareability
Reader/Writer EOF/would-block matrix
timeout validation
```

## Gate B — scheduler-aware wait

```text
read/write/accept/connect park Task, not Worker
CancelledError at IO safepoint
close wakes waiters
GC roots valid while task is blocked
```

## Gate C — pipe

```text
bounded pipe
rendezvous pipe
cross-strand checked pipe endpoints
payload shareability checks
```

## Gate D — file

```text
File.open block form
NameEnum mode normalization
capability/effect checks
checked handle confinement
unchecked descriptor sharing
```

## Gate E — socket

```text
TCP connect/listen/accept
TCP read/write/shutdown
UDP send/recv datagram semantics
socket options
network capability checks
```

## Gate F — all multitasking models

```text
same-strand async
checked spawn
sync coordination
unchecked concurrency
scatter/gather flow
deterministic/replay provider behavior
trace propagation across IO wait
```

---

# 17. Итоговое решение

Зафиксировать v2 так:

```text
1. Public API remains synchronous-looking.
2. Every blocking IO operation is a scheduler-aware Task safepoint.
3. ByteBuffer is mutable and strand-confined.
4. Bytes is immutable and shareable.
5. Pipe is the primary checked cross-strand IO coordination primitive.
6. File/socket/listener handles are strand-confined by default.
7. Cross-strand descriptor sharing exists only through isolation::unchecked.
8. Mutex does not make confined handles checked-shareable; it only provides synchronization once unsafe sharing is explicitly enabled.
9. EOF/would-block semantics remain uniform across files, pipes and TCP.
10. UDP stays datagram-oriented and does not include Reader/Writer.
11. Capability/effect checks happen before host IO.
12. Deterministic/replay profile must route external IO through recorded providers.
```

Это даёт Amber один цельный IO-слой без `await`, без нового синтаксиса, без public epoll/io_uring API, но с достаточно строгими контрактами для VM, native backend, verifier, scheduler, GC, conformance и unsafe/system code.
