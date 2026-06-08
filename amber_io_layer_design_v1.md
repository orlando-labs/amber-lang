# Amber IO Layer Project v1

**Проектный слой для разработки `io` / `fs` / `net` API Amber**  
**Версия:** v1.0-project  
**Дата:** 8 июня 2026  
**Основание:** Amber unified language/runtime spec, Amber Threading / Async API Project v1, обсуждение IO-дизайна  
**Статус:** engineering blueprint для реализации stdlib/runtime-facing IO слоя

---

## 0. Статус документа

Этот документ проектирует IO-слой Amber поверх уже зафиксированной модели языка и runtime:

- object model, methods, block suffix, symbols, exceptions;
- no-GIL execution model `Worker -> Strand -> Task`;
- checked strand isolation by default;
- explicit `isolation: :unchecked` для advanced/system code;
- capability/effect profiles;
- scheduler-aware cancellation/timeout semantics.

Документ не вводит новый core syntax. IO реализуется как stdlib/runtime-facing profile через обычные классы, миксины, методы, блоки и runtime intrinsics.

---

## 1. Goals

Amber IO layer должен дать:

- byte streams;
- text wrappers;
- bounded in-memory pipes;
- filesystem paths/files/metadata;
- TCP/UDP socket IO;
- scheduler-aware blocking operations;
- cancellation and timeout integration;
- capability/effect checks;
- deterministic error surface;
- support for checked isolation by default;
- explicit unsafe cross-strand descriptor sharing under `isolation: :unchecked`;
- implementation path for bytecode VM and amber-native optimization.

Минимальный успешный результат:

```text
Amber source
  -> ordinary sends / intrinsic sends
  -> HIR IO nodes or tagged HSend
  -> bytecode/runtime ABI
  -> verifier checks
  -> scheduler-aware IO runtime
  -> conformance corpus
```

---

## 2. Non-goals for v1

В v1 не входят:

- TLS;
- HTTP;
- process spawning;
- shell pipeline syntax;
- file watching;
- mmap;
- public epoll/kqueue/io_uring API;
- generic `select` over IO objects;
- stream backpressure protocol beyond `Pipe` and blocking writer semantics;
- async file IO as distinct public concept;
- general `move(value)` ownership transfer;
- enums as language feature;
- typed resource capabilities beyond existing effect/capability profiles.

---

## 3. Namespaces

Canonical modules:

```amber
import io
import fs
import net

from io import ByteBuffer, Reader, Writer, Pipe
from fs import Path, File
from net import Endpoint
```

Recommended submodules:

```text
io              basic stream/buffer/pipe protocols
io.text         encoding/decoding, line reader/writer wrappers
io.buffer       ByteBuffer, ByteSlice, Bytes helpers
fs              paths, files, dirs, metadata
net             endpoints, DNS-facing helpers, common socket API
net.tcp         TCP streams and listeners
net.udp         UDP datagram sockets
net.unix        Unix domain sockets, staged/host-dependent
```

Runtime-visible public types:

```text
io.ByteBuffer
io.ByteSlice
io.Reader
io.Writer
io.Seeker
io.Closer
io.Flushable
io.Pipe
io.PipeReader
io.PipeWriter

fs.Path
fs.File
fs.Dir
fs.Metadata

net.Endpoint
net.Socket
net.TcpStream
net.TcpListener
net.UdpSocket
```

---

## 4. Design anchors

### 4.1. No new syntax

IO uses ordinary Amber method calls:

```amber
File.open("/tmp/a.txt", :read) |f|:
 f.read_all!()
```

No `await`, `using`, `defer`, shell pipe syntax, or enum syntax is introduced in v1.

### 4.2. Resource-oriented IO

Host-backed IO objects are managed runtime resources:

```text
File / Socket / Listener = object + native handle + runtime resource state
```

Native handles are never exposed as raw pointers.

### 4.3. Effect/capability gated

Filesystem and network operations require both:

```text
capability permission + allowed effect row
```

Missing capability raises `CapabilityError`. Forbidden effect raises `EffectViolationError` or a compile/load-time diagnostic when statically visible.

### 4.4. Scheduler-aware blocking

Blocking IO is a task safepoint. `read!`, `write!`, `accept!`, `connect`, `flush!` and close-wake paths must integrate with task cancellation and timeout.

### 4.5. Checked isolation by default, unchecked escape hatch

File/socket handles are strand-confined by default. Cross-strand sharing is rejected in checked mode, but allowed inside explicit unchecked concurrency regions.

---

## 5. NameEnum convention: symbols and strings

Amber v1 has no language-level enums. Enum-like stdlib parameters therefore accept `Symbol` or `Str` and normalize by exact name to an interned symbol.

### 5.1. Normative rule

```text
NameEnum argument accepts Symbol or Str.
Str is normalized to Symbol by exact supported-name lookup.
Unsupported Symbol/Str -> ArgumentError.
Other types -> TypeError.
```

Examples:

```amber
File.open("/tmp/a", :read)   # ok
File.open("/tmp/a", "read")  # ok, normalized to :read

File.open("/tmp/a", :banana)   # ArgumentError
File.open("/tmp/a", "banana")  # ArgumentError

File.open("/tmp/a", 1)         # TypeError
```

### 5.2. No implicit aliases in v1

Normalization is strict:

```text
"read"       -> :read
"write"      -> :write
"append"     -> :append
"read_write" -> :read_write

"READ"       -> ArgumentError
"r"          -> ArgumentError unless explicitly admitted by future profile
"read-write" -> ArgumentError
```

### 5.3. Native optimization note

`amber-native` can optimize static symbol/string literals as enum-like switches:

```amber
File.open(path, "read")
```

may lower to:

```text
mode = SYM(:read)
switch mode:
  :read       -> OPEN_READ
  :write      -> OPEN_WRITE
  :append     -> OPEN_APPEND
  :read_write -> OPEN_READ_WRITE
  else        -> ArgumentError
```

No language-level enum feature is required for v1 performance.

---

## 6. Protocol mixins

IO protocols are represented as mixins. Host-backed concrete classes include them.

```amber
mixin io.Reader:
 def read!(buf as ByteBuffer, timeout: null) -> Int:...
 def try_read!(buf as ByteBuffer) -> Int?:...

mixin io.Writer:
 def write!(bytes, timeout: null) -> Int:...
 def try_write!(bytes) -> Int?:...
 def write_all!(bytes, timeout: null) -> null:...

mixin io.Closer:
 def close!() -> null:...
 def closed?() -> Bool:...

mixin io.Flushable:
 def flush!() -> null:...

mixin io.Seeker:
 def seek!(offset as Int, whence: :start) -> Int:...
```

The `!` suffix is normative for mutating/effectful IO operations.

---

## 7. Byte buffers

### 7.1. Types

```amber
class io.ByteBuffer:
 class_method def new(size as Int):...
 def count() -> Int:...
 def capacity() -> Int:...
 def remaining() -> Int:...
 def clear!() -> null:...
 def slice(start as Int, length: null) -> ByteSlice:...
 def bytes() -> Bytes:...
```

`ByteBuffer` is mutable and strand-confined by default.

`Bytes` / `ByteSlice` should be immutable or view-like according to the memory profile. If shareable, they may cross strand boundary in checked mode.

### 7.2. Empty buffer rule

Calling `read!(buf)` or `try_read!(buf)` with no writable remaining capacity raises `ArgumentError`.

Rationale: return value `0` is reserved for EOF in buffer-based read APIs.

---

## 8. Reader semantics

### 8.1. Blocking/cooperative read

```amber
n = reader.read!(buf, timeout: null)
```

Contract:

```text
n > 0  bytes read
0      EOF
wait   if no data yet and stream is open
```

Errors:

```text
TimeoutError       deadline expired
CancelledError     current task cancelled at IO safepoint
AlreadyClosedError resource already closed
IOError subclass   host/resource failure
ArgumentError      empty buffer or invalid timeout
TypeError          invalid buffer object
```

### 8.2. Non-blocking read

```amber
n = reader.try_read!(buf)
```

Contract:

```text
n > 0  bytes read
0      EOF
null   would block / no data available now
```

`try_read!` never suspends the current task.

### 8.3. Helper reads

```amber
reader.read_exact!(n, timeout: null) -> Bytes
reader.read_all!(limit: null) -> Bytes
reader.read_line!(limit: null, timeout: null) -> Str?
reader.each_chunk(size: 8192) |chunk|: ...
```

Contracts:

```text
read_exact!(n)
  returns exactly n bytes or raises EOFError

read_all!(limit:)
  returns accumulated bytes; EOF is normal termination
  raises ArgumentError if limit exceeded or invalid

read_line!()
  returns Str line
  returns null on EOF before any chars
  preserves empty lines because "" is truthy in Amber

each_chunk(size:)
  yields non-empty chunks until EOF
```

Canonical loop:

```amber
while line = reader.read_line!():
 process(line)
```

Because only `null` and `false` are falsy, empty lines do not terminate the loop.

---

## 9. Writer semantics

### 9.1. Blocking/cooperative write

```amber
n = writer.write!(bytes, timeout: null)
```

Contract:

```text
n > 0  bytes written
partial writes are allowed
wait   if resource cannot currently accept data
```

For non-empty input, returning `0` from `write!` is a protocol error unless the resource is at EOF/closed-equivalent and reports a concrete error such as `BrokenPipeError` or `AlreadyClosedError`.

### 9.2. Non-blocking write

```amber
n = writer.try_write!(bytes)
```

Contract:

```text
n > 0  bytes written
0      allowed only for empty input
null   would block / cannot write now
```

### 9.3. Write-all

```amber
writer.write_all!(bytes, timeout: null) -> null
```

Contract:

```text
Repeatedly calls write! until all bytes are written or an error occurs.
Does not close writer.
Honors timeout as a single operation deadline, not per-chunk reset.
```

---

## 10. EOF and would-block matrix

```text
read!(buf)
  n > 0  data
  0      EOF
  waits  if no data yet

try_read!(buf)
  n > 0  data
  0      EOF
  null   would block

read_exact!(n)
  Bytes length n
  EOFError if stream ends before n bytes

read_all!(limit:)
  Bytes
  EOF is normal termination

read_line!()
  Str line
  null on EOF before any chars
```

Only buffer-based read APIs expose EOF as `0`. Non-blocking no-data is always `null`, never `0`.

---

## 11. Path API

### 11.1. Operator method accepted

Amber supports operator methods such as:

```amber
def /(rval):
 ...
```

Therefore `Path#/` is accepted as canonical ergonomic path composition.

```amber
p = fs.Path("/var") / "log" / "amber.log"
```

Equivalent explicit spelling:

```amber
p = fs.Path("/var").join("log", "amber.log")
```

### 11.2. Contract

```amber
class fs.Path:
 def /(rval):
  join(rval)

 def join(*parts) -> Path:...
 def basename() -> Str:...
 def extname() -> Str:...
 def parent() -> Path:...
 def absolute?() -> Bool:...
 def normalize() -> Path:...
```

Argument rules:

```text
Path / Str   -> Path
Path / Path  -> Path
Path / other -> TypeError
```

`Path` should be immutable and shareable if internally normalized/frozen.

---

## 12. Filesystem API

### 12.1. File open

Canonical API:

```amber
File.open("/tmp/a.txt", :read) |f|:
 f.read_all!()

File.open("/tmp/a.txt", "write", create: true, truncate: true) |f|:
 f.write_all!("hello\n".bytes())
```

Signature sketch:

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
  permissions: null
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

String modes normalize by NameEnum rule:

```text
"read"       -> :read
"write"      -> :write
"append"     -> :append
"read_write" -> :read_write
```

### 12.2. File open validation

```text
mode not Symbol/Str       -> TypeError
unsupported Symbol/Str    -> ArgumentError
invalid option combination -> ArgumentError
missing fs capability      -> CapabilityError
forbidden effect           -> EffectViolationError
```

Invalid option combinations:

```text
:read + truncate:true                 -> ArgumentError
:append + truncate:true               -> ArgumentError
exclusive:true without create:true    -> ArgumentError
permissions not Int/null              -> TypeError
```

### 12.3. Block form

If block is supplied:

```text
1. open resource
2. call block
3. close resource during ensure/unwind path
4. return block result
5. if close fails after block success, raise close error
6. if both block and close fail, primary error is block error and close error is attached as suppressed/cause metadata
```

### 12.4. Convenience functions

```amber
fs.exists?(path) -> Bool
fs.file?(path) -> Bool
fs.dir?(path) -> Bool
fs.metadata(path) -> Metadata

fs.read_bytes(path, limit: null) -> Bytes
fs.write_bytes(path, bytes) -> null

fs.read_text(path, encoding: :utf8) -> Str
fs.write_text(path, text, encoding: :utf8) -> null

fs.mkdir(path) -> null
fs.mkdir_p(path) -> null
fs.remove(path) -> null
fs.rename(from, to) -> null
fs.copy(from, to) -> Int
```

`encoding:` is also NameEnum-like and accepts `:utf8` or `"utf8"`.

---

## 13. Text IO

Text IO is a wrapper over byte IO.

```amber
reader = io.text.Reader(file, encoding: :utf8)
line = reader.read_line!()

writer = io.text.Writer(file, encoding: "utf8")
writer.puts!("hello")
```

Errors:

```text
EncodingError
InvalidByteSequenceError
IOError subclasses from underlying reader/writer
```

Convenience:

```amber
fs.read_text("README.md", encoding: :utf8)
fs.write_text("out.txt", text, encoding: "utf8")
```

---

## 14. Pipes

### 14.1. In-memory pipe

```amber
reader, writer = io.Pipe.new(capacity: 64 * 1024)
```

Capacity:

```text
capacity: 0      rendezvous pipe
capacity > 0     bounded buffered pipe
capacity < 0     ArgumentError
non-integer       TypeError
```

Semantics:

```text
write! suspends when buffer is full
read! suspends when buffer is empty and writer is open
read! returns EOF when writer is closed and buffer empty
write! after reader close raises BrokenPipeError
close! is idempotent
```

### 14.2. Backpressure

`Pipe` is the normative v1 backpressure primitive:

```text
bounded capacity + blocking/cooperative write! + blocking/cooperative read!
```

No generic stream backpressure protocol is required in v1.

### 14.3. OS pipe

Staged API:

```amber
r, w = io.pipe.os()
```

May be required later by process spawning. Not mandatory for v1 core IO gate.

---

## 15. Socket IO

## 15.1. TCP connect

```amber
s = net.tcp.connect("example.com", 443, timeout: 5.0)

s.write_all!(request)
response = s.read_all!(limit: 1_000_000)
s.close!()
```

Block form:

```amber
net.tcp.connect("example.com", 443, timeout: 5.0) |s|:
 s.write_all!(request)
 s.read_all!(limit: 1_000_000)
```

### 15.2. TCP listener

```amber
listener = net.tcp.listen("127.0.0.1", 8080, backlog: 128)

loop:
 client = listener.accept!()
 task.async |child|:
  handle_client(client)
```

Required API:

```amber
class net.TcpStream:
 include io.Reader, io.Writer, io.Closer, io.Flushable

 def local_endpoint() -> Endpoint:...
 def remote_endpoint() -> Endpoint:...
 def shutdown!(side) -> null:...
 def close_read!() -> null:...
 def close_write!() -> null:...
 def set_option!(name, value) -> null:...

class net.TcpListener:
 include io.Closer

 class_method def listen(host as Str, port as Int, backlog: 128) -> TcpListener:...
 def accept!(timeout: null) -> TcpStream:...
 def local_endpoint() -> Endpoint:...
```

### 15.3. Shutdown sides

`shutdown!(side)` uses NameEnum:

```text
:read / "read"
:write / "write"
:both / "both"
```

Unsupported side raises `ArgumentError`. Non-symbol/non-string side raises `TypeError`.

### 15.4. Socket options

```amber
s.set_option!(:nodelay, true)
listener.set_option!("reuse_addr", true)
```

v1 supported options:

```text
:nodelay      TcpStream, Bool
:reuse_addr   TcpListener, Bool
:keepalive    TcpStream, Bool, optional/staged
```

Unsupported option symbol/string raises `ArgumentError`. Wrong value type raises `TypeError`.

---

## 16. UDP

UDP is datagram-oriented, not stream-oriented.

```amber
sock = net.udp.bind("0.0.0.0", 9999)
bytes, from = sock.recv_from!(max: 1500)
sock.send_to!(bytes, from)
```

API:

```amber
class net.UdpSocket:
 include io.Closer

 class_method def bind(host as Str, port as Int) -> UdpSocket:...
 def recv_from!(max: 65507, timeout: null) -> (Bytes, Endpoint):...
 def try_recv_from!(max: 65507) -> (Bytes, Endpoint)?:...
 def send_to!(bytes, endpoint, timeout: null) -> Int:...
 def try_send_to!(bytes, endpoint) -> Int?:...
 def local_endpoint() -> Endpoint:...
```

Datagram boundaries are preserved.

---

## 17. Descriptor sharing and isolation

### 17.1. Checked mode

File/socket/listener objects are strand-confined by default.

```amber
async |task|:
 s = net.tcp.connect("127.0.0.1", 8080)

 task.spawn |child|:
  s.write_all!("x".bytes())
# compile-time diagnostic or runtime IsolationError
```

### 17.2. Unchecked mode

`isolation: :unchecked` explicitly disables isolation/ownership checks at the boundary.

```amber
async |task|:
 s = net.tcp.connect("127.0.0.1", 8080)

 task.spawn(isolation: :unchecked) |child|:
  s.write_all!("x".bytes())
# allowed; user accepts synchronization responsibility
```

Normative rule:

```text
Unchecked mode permits cross-strand descriptor sharing by capture/access.
It does not make File/Socket objects thread-safe.
It does not create a happens-before edge.
It does not disable lifetime checks, GC checks, verifier checks, write barriers, root maps or object validity checks.
```

### 17.3. Synchronization responsibility

Correct unchecked sharing uses explicit synchronization:

```amber
from sync import Mutex

async |task|:
 s = net.tcp.connect("127.0.0.1", 8080)
 m = Mutex.new()

 workers = (1..4).map |i|:
  task.spawn(isolation: :unchecked) |child|:
   m.synchronize:
    s.write_all!("worker #{i}\n".bytes())

 workers.each |h|:
  h.wait()

 s.close!()
```

### 17.4. Not move semantics

This is not full ownership transfer:

```text
No implicit source-handle invalidation.
No general move(value) in v1.
No automatic thread-safe wrapper.
```

Preferred wording:

```text
unchecked cross-strand descriptor sharing
unsafe handle sharing by capture
```

---

## 18. Resource lifetime and close semantics

Resource state:

```text
open -> closing -> closed
open -> destroyed/deallocated by lifecycle profile
```

Rules:

```text
close! is idempotent for stdlib IO resources.
Operations after close raise AlreadyClosedError.
Operations after destroy/dealloc follow lifecycle errors.
Finalizer may close OS handle as safety net, but deterministic close is user responsibility.
```

Close during pending IO:

```text
close! wakes pending read/write/accept waiters.
The awakened operation returns/raises according to resource-specific semantics:
  - read side close may produce EOF or AlreadyClosedError;
  - write side close usually produces BrokenPipeError or AlreadyClosedError;
  - listener close wakes accept! with AlreadyClosedError.
```

Unchecked regions do not suppress close/lifetime checks.

---

## 19. Capabilities

Suggested capability names:

```text
cap.fs.read(path_scope)
cap.fs.write(path_scope)
cap.fs.metadata(path_scope)
cap.net.connect(endpoint_scope)
cap.net.listen(endpoint_scope)
cap.net.udp(endpoint_scope)
cap.process.stdio
cap.io.pipe
```

Examples:

```amber
fs.read_text("/app/data/a.txt")
# ok if cap.fs.read permits path

fs.read_text("/etc/passwd")
# CapabilityError if outside granted scope

net.tcp.connect("example.com", 443)
# CapabilityError without net.connect capability
```

Capability checks happen before host calls whenever possible.

---

## 20. Effects

Suggested effect labels:

```text
!{io}
!{fs}
!{net}
!{dns}
!{stdin}
!{stdout}
!{stderr}
!{mut}
!{async}
```

Relationships:

```text
fs ⊂ io
net ⊂ io
dns ⊂ net
stdin/stdout/stderr ⊂ io
```

Examples:

```amber
def read_config(path as fs.Path) -> Str !{fs}:
 fs.read_text(path)

def fetch(host as Str, port as Int) -> Bytes !{net, dns}:
 s = net.tcp.connect(host, port)
 s.read_all!()
```

---

## 21. Stdio

```amber
io.stdin
io.stdout
io.stderr
```

Convenience:

```amber
io.print("hello")
io.puts("hello")
io.warn("bad")
```

Effects:

```text
stdin  !{stdin, io}
stdout !{stdout, io}
stderr !{stderr, io}
```

Capability:

```text
cap.process.stdio
```

Notebook/sandbox hosts may virtualize stdio.

---

## 22. Error registry

New IO-related canonical errors:

```text
IOError
  EOFError
  WouldBlockError
  BrokenPipeError
  ConnectionResetError
  ConnectionRefusedError
  AddressInUseError
  AddressNotAvailableError
  NetworkUnreachableError
  DnsError
  FileNotFoundError
  PermissionError
  AlreadyClosedError
  InvalidPathError
  NotDirectoryError
  IsDirectoryError
  FileExistsError
  ResourceBusyError

EncodingError
InvalidByteSequenceError
```

Existing shared errors reused by IO:

```text
CapabilityError
EffectViolationError
TimeoutError
CancelledError
IsolationError
UseAfterFreeError
DestroyedAccessError
ArgumentError
TypeError
```

`WouldBlockError` is reserved for raw/exception-style nonblocking APIs. Standard `try_read!` / `try_write!` use `null` for would-block.

---

## 23. Pattern matching integration

Structured value objects should support `deconstruct_keys`.

```amber
case net.parse_endpoint("127.0.0.1:8080"):
 when net.Endpoint(host:, port:):
  net.tcp.connect(host, port)
 else:
  raise ArgumentError("invalid endpoint")
```

`fs.Metadata` example:

```amber
case fs.metadata(path):
 when fs.Metadata(file?: true, size: size):
  size
 when fs.Metadata(dir?: true):
  :directory
```

IO operations themselves use exceptions, not `Ok/Err` result unions.

---

## 24. HIR lowering

Surface IO calls remain ordinary call/send syntax unless intrinsic resolution proves canonical stdlib identity.

Implementation may lower to dedicated HIR nodes:

```text
HIOOpenFile
HIORead
HIOTryRead
HIOWrite
HIOTryWrite
HIOClose
HIOFlush
HIOPollWait
HNetConnect
HNetAccept
HNetUdpRecv
HNetUdpSend
HCapabilityCheck
HEffectCheck
```

Alternatively, keep ordinary sends with intrinsic tags:

```text
HSend(receiver=fs.File, selector=:open, intrinsic=IO_OPEN_FILE)
HSend(receiver=file, selector=:read!, intrinsic=IO_READ)
HSend(receiver=file, selector=:try_read!, intrinsic=IO_TRY_READ)
```

If names are shadowed by user bindings, intrinsic lowering must not apply.

---

## 25. Bytecode / VM opcodes

Recommended opcode family:

```text
IO_OPEN_FILE
IO_READ
IO_TRY_READ
IO_WRITE
IO_TRY_WRITE
IO_FLUSH
IO_CLOSE
IO_SEEK
IO_PIPE_NEW

NET_CONNECT
NET_LISTEN
NET_ACCEPT
NET_UDP_BIND
NET_UDP_RECV
NET_UDP_SEND

IO_WAIT_READABLE
IO_WAIT_WRITABLE
IO_CANCEL_WAIT
```

Implementation may dispatch through ordinary `SEND`/`CALL` but intrinsic opcodes are recommended for verifier checks and performance.

---

## 26. Runtime ABI

Suggested ABI hooks:

```text
rt_io_open_file(path, flags, permissions, cap_context)
rt_io_read(handle, buffer, deadline, cancel_token)
rt_io_try_read(handle, buffer)
rt_io_write(handle, bytes, deadline, cancel_token)
rt_io_try_write(handle, bytes)
rt_io_flush(handle, deadline, cancel_token)
rt_io_close(handle)
rt_io_seek(handle, offset, whence)

rt_net_connect(endpoint, deadline, cap_context)
rt_net_listen(endpoint, backlog, cap_context)
rt_net_accept(listener, deadline, cancel_token)
rt_udp_bind(endpoint, cap_context)
rt_udp_recv_from(socket, max, deadline, cancel_token)
rt_udp_try_recv_from(socket, max)
rt_udp_send_to(socket, bytes, endpoint, deadline, cancel_token)
```

Scheduler hooks:

```text
register_awaitable(task, readiness_source, deadline?)
wake_task(task_id)
cancel_wait(task_id)
close_wake(handle)
```

---

## 27. Verifier rules

Verifier must check or ensure dynamic guards for:

```text
- intrinsic target identity not shadowed
- resource handle register has IO-resource-compatible value
- buffer argument is writable ByteBuffer
- timeout is numeric/null
- NameEnum literal belongs to supported set when statically known
- effect/capability metadata is present
- IO wait op has safepoint/root-map metadata
- blocked task frames have valid root maps
- unchecked IO handle sharing appears only inside unsafe_concurrency regions
- unsafe regions preserve lifetime/GC/write-barrier validation
```

Verifier must not:

```text
- skip GC validation in unsafe regions
- skip write barriers in unsafe regions
- treat unchecked descriptor sharing as move semantics
- execute user code during verification
```

---

## 28. Observability events

Minimum events:

```text
io.open
io.read.wait
io.read.ready
io.read.done
io.write.wait
io.write.ready
io.write.done
io.close
io.error

fs.metadata
fs.rename
fs.remove

net.connect.start
net.connect.done
net.accept.wait
net.accept.done
net.udp.recv
net.udp.send

unsafe.io.cross_strand
```

Tracing must not change scheduling or isolation semantics unless deterministic scheduler profile is explicitly enabled.

---

## 29. API examples

### 29.1. Copy file

```amber
import fs


def copy_file(src, dst) !{fs, io}:
 File.open(src, :read) |input|:
  File.open(dst, "write", create: true, truncate: true) |output|:
   input.copy_to!(output)
```

### 29.2. Read lines

```amber
File.open("/tmp/log.txt", :read) |f|:
 while line = f.read_line!():
  process(line)
```

### 29.3. Non-blocking read polling

```amber
buf = ByteBuffer.new(4096)

loop:
 case socket.try_read!(buf):
  when null:
   task.yield()
  when 0:
   break :eof
  when n:
   consume(buf.slice(0, n))
   buf.clear!()
```

### 29.4. Echo server, same strand handling

```amber
import net.tcp


def echo_server(host, port) !{net, io}:
 listener = tcp.listen(host, port, backlog: 128)

 loop:
  client = listener.accept!()
  task.async |child|:
   client.each_chunk(size: 8192) |chunk|:
    client.write_all!(chunk)
   client.close!()
```

### 29.5. Unsafe shared descriptor with mutex

```amber
from sync import Mutex
import net.tcp

async |task|:
 s = tcp.connect("127.0.0.1", 8080)
 m = Mutex.new()

 workers = (1..4).map |i|:
  task.spawn(isolation: :unchecked) |child|:
   m.synchronize:
    s.write_all!("worker #{i}\n".bytes())

 workers.each |h|:
  h.wait()

 s.close!()
```

---

## 30. Conformance corpus

### 30.1. File corpus

```text
file_open_read_block_closes_on_success
file_open_read_block_closes_on_exception
file_open_string_mode_normalizes_to_symbol
file_open_unsupported_mode_symbol_argument_error
file_open_unsupported_mode_string_argument_error
file_open_mode_wrong_type_type_error
file_read_all_eof_normal
file_read_exact_short_eof_error
file_read_after_close_already_closed_error
file_write_after_close_already_closed_error
file_open_missing_file_not_found_error
file_open_directory_as_file_is_directory_error
file_open_no_capability_capability_error
file_open_effect_forbidden_effect_violation_error
path_slash_joins_segments
path_slash_wrong_type_type_error
```

### 30.2. Reader/writer corpus

```text
read_returns_zero_on_eof
read_empty_buffer_argument_error
try_read_returns_null_on_would_block
try_read_returns_zero_on_eof
write_all_handles_partial_writes
try_write_returns_null_on_would_block
read_timeout_timeout_error
read_cancelled_cancelled_error
```

### 30.3. Pipe corpus

```text
pipe_bounded_transfer
pipe_zero_capacity_rendezvous
pipe_writer_blocks_when_full
pipe_reader_blocks_when_empty
pipe_reader_eof_after_writer_close
pipe_write_after_reader_close_broken_pipe_error
pipe_close_idempotent
```

### 30.4. TCP corpus

```text
tcp_connect_success
tcp_connect_refused_connection_refused_error
tcp_connect_timeout_timeout_error
tcp_echo_server_roundtrip
tcp_shutdown_write_rejects_later_write
tcp_accept_listener_close_wakes_accept
tcp_socket_option_symbol_ok
tcp_socket_option_string_ok
tcp_socket_option_unsupported_argument_error
```

### 30.5. UDP corpus

```text
udp_send_recv_preserves_datagram_boundary
udp_try_recv_returns_null_when_no_packet
udp_recv_timeout_timeout_error
udp_send_to_wrong_endpoint_type_error
```

### 30.6. Isolation / unsafe corpus

```text
io_handle_cross_strand_checked_rejected
io_handle_cross_strand_unchecked_allowed
io_handle_unchecked_requires_unsafe_concurrency_feature_flag
io_handle_unchecked_denied_by_loader_policy
io_handle_unchecked_read_after_close_still_already_closed_error
io_handle_unchecked_destroyed_access_still_error
io_handle_unchecked_write_barriers_still_valid
io_handle_unchecked_trace_marks_unsafe_boundary
```

---

## 31. Release gates

### Gate A — byte/file basics

Must pass:

```text
ByteBuffer
Reader/Writer protocols
File.open block form
read_all/read_exact/read_line
Path#/
NameEnum normalization
```

### Gate B — scheduler-aware IO

Must pass:

```text
read/write wait integration
TimeoutError
CancelledError
close wakes waiters
root maps valid while task blocked in IO
```

### Gate C — pipes

Must pass:

```text
bounded pipe
rendezvous pipe
EOF/close semantics
backpressure behavior
```

### Gate D — TCP/UDP

Must pass:

```text
connect/listen/accept
TCP read/write/shutdown
UDP send_to/recv_from
socket option validation
network capability checks
```

### Gate E — unchecked descriptor sharing

Must pass:

```text
checked rejection
unchecked allow
unsafe_concurrency artifact flag
loader policy denial
lifetime checks preserved
trace marks unsafe boundary
```

### Gate F — full conformance

Must pass all file, reader/writer, pipe, TCP, UDP, capability/effect and unsafe corpus tests.

---

## 32. Final design stance

Amber IO v1 should be a stdlib/runtime-facing profile, not a core syntax extension.

Final decisions:

```text
1. Path#/ is accepted and canonical for ergonomic path composition.
2. Enum-like IO parameters use Symbol or Str and normalize to Symbol.
3. Unsupported enum-like values raise ArgumentError; wrong types raise TypeError.
4. read!(buf) returns 0 only for EOF and otherwise suspends on no data.
5. try_read!(buf) returns null for would-block and 0 for EOF.
6. File/socket/listener handles are strand-confined by default.
7. Cross-strand descriptor sharing is allowed only in explicit unchecked isolation regions.
8. Unchecked descriptor sharing does not imply thread safety or move semantics.
9. User is responsible for synchronization and data-race control in unchecked mode.
10. Runtime memory safety, lifetime checks, GC correctness and write barriers remain mandatory.
11. Filesystem/network IO is capability/effect gated.
12. amber-native may optimize static symbols/strings as enum-like switches without adding enums to the language.
```
