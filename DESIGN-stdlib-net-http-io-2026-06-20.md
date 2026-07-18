# DESIGN — `io` foundation and `net.http` HTTP/1.1 semantics for Amber

**Status:** proposed normative design  
**Date:** 2026-06-20  
**Target:** Amber standard library and runtime-facing API  
**Scope:** `io`, `net`, `net.http` HTTP/1.1 client plus the Phase 7 basic
server hook over plaintext TCP  
**Out of scope for v1:** TLS/HTTPS, HTTP/2, HTTP/3, proxy/CONNECT, web
framework/routing layer, cookie jar, cache, automatic compression decoding,
multipart helpers, WebSocket/Upgrade, `Expect: 100-continue`, request trailers

**Implementation note (2026-07-01):** the repository now contains the Phase 7
basic server slice: `net.http.Server`, `net.http.ServerRequest`, and
`net.http.ServerResponse` are exported native stdlib types backed by
`runtime/vm.cpp` and `runtime/stdlib_net_http.cpp`. The focused
`build/vm_net_http_tests` loopback suite covers Amber-source `Server#serve`
hooks and cooperative request concurrency. This is a low-level hook server, not
the future web-framework/routing layer.

---

## 0. Executive summary

The original minimal `net.http` sketch is too weak as a language/stdlib specification. It exposes a `Client`, `Request`, `Response`, and a small body-reading API, but it does not define enough semantics for a conforming implementation: response ownership, body streaming, resource closure, connection reuse, timeout phases, header multiplicity, redirect safety, HTTP/1.1 message framing, cancellation, capability checks, and conformance tests.

This document replaces that sketch with a layered design:

1. **`io` owns generic streaming primitives.**  
   `Reader`, `Writer`, `Closeable`, `Flushable`, `HalfCloseable`, `Duplex`, and resource/lifecycle rules belong here. Generic concepts such as pipe/channel/exchange coordination are not `net.http` concepts.

2. **`task` owns async composition.**  
   HTTP must not invent its own async pipeline abstraction. Existing `task.async`, futures, cancellation, and scheduling compose with ordinary `io` handles.

3. **`net` owns TCP/DNS.**  
   TCP streams implement the `io` protocols. DNS/connect/read/write operations are cancellation-aware and timeout-aware.

4. **`net.http` owns HTTP semantics.**  
   `Client`, `Request`, `Response`, `Headers`, `RequestBody`, `ResponseBody`,
   redirects, pooling, protocol parsing, HTTP errors, the Phase 7
   `Server`/`ServerRequest`/`ServerResponse` hook surface, and
   request/response lifecycle are HTTP-specific.

The crucial API decision is:

```amber
# Scoped response form.
client.send(req) |res|:
 use(res)

# Explicit ownership form.
res = client.send(req)
try:
 use(res)
ensure:
 res.close!()
```

All response-producing operations must have both forms.

For imperative streaming uploads, `net.http` does **not** define a generic `Pipe` or generic `Exchange`. Instead it exposes a protocol-specific request-in-progress handle:

```amber
h = client.begin(method: :post, url: url, length: null)

producer.write_intro(h)
producer.write_body(h)
producer.write_tail(h)

h.finish!()

res = h.response()
try:
 consume(res)
ensure:
 res.close!()
```

`RequestHandle` implements `io.Writer`. It is HTTP-specific only in its `finish!`, `response`, and `abort!` semantics.

---

## 1. Design goals

### 1.1. Primary goals

- Provide a serious HTTP/1.1 client suitable for real programs, not only examples.
- Preserve Amber style: constructor-call `Client(...)`, block suffix convenience, explicit resource ownership where needed, `?`/`!` naming conventions, and method-chain friendliness.
- Make streaming v1, not a future afterthought.
- Separate generic IO semantics from HTTP protocol semantics.
- Make response lifetime explicit and testable.
- Preserve duplicate headers and field-line order.
- Distinguish timeout phases.
- Support persistent connection pooling without hidden resource leaks.
- Define safe redirect behavior and default redirects off.
- Keep HTTP/1.1 wire parsing strict and bounded.
- Make cancellation deterministic.
- Route network access through the capability model.
- Provide a conformance matrix sufficient for VM/native parity.

### 1.2. Non-goals for v1

- No TLS/HTTPS in `net.http` core.
- No HTTP/2 or HTTP/3.
- No proxy, CONNECT, or Upgrade support.
- No web framework, routing DSL, templating, sessions, CSRF, cookies, or
  browser-style security policy layer in core. The Phase 7 `Server` API is only
  the low-level request-hook adapter over `net.tcp`.
- No automatic cookies.
- No automatic cache.
- No automatic gzip/br/deflate decoding.
- No multipart/form-data builder in core.
- No request trailers.
- No `Expect: 100-continue`.
- No browser security policy layer.

---

## 2. Layering model

### 2.1. Module responsibilities

```text
io
  generic byte-stream protocols and resource lifecycle

task
  async scheduling, futures, cancellation propagation

net
  DNS, TCP listener/stream, normalized network errors

net.http
  HTTP request/response model, headers, body framing, redirects, pooling

net.https / net.tls
  future TLS-enabled transport profile
```

### 2.2. Rule: HTTP must not redefine generic IO

`net.http` must not expose `Pipe`, generic `Exchange`, generic channel, or generic duplex abstractions. Those belong to `io`/`task`.

HTTP may expose handles that implement generic IO protocols:

```text
RequestHandle implements io.Writer
ResponseBody implements io.Reader
TcpStream implements io.Duplex
```

But the generic contracts live in `io`.

### 2.3. Rule: async composition belongs to `task`

`net.http` operations are blocking from the perspective of the current strand, but all blocking operations are cancellation points. Users compose independent reads/writes with `task.async`.

```amber
task.async:
 h.write_all!(chunk)
 h.finish!()

task.async:
 res = h.response()
 consume(res)
```

HTTP does not introduce its own future/task type.

---

## 3. `io` foundation

### 3.1. `io.Reader`

```amber
interface io.Reader:
 def read(max_bytes:) -> Bytes
 def each_chunk(size:) |bytes|: ...
 def close!()
 def closed?() -> Bool
```

Normative rules:

- `read(max_bytes:)` returns up to `max_bytes` bytes.
- EOF returns empty `Bytes` or a designated EOF result; the exact representation must be consistent across stdlib.
- Reads after `close!` raise `ClosedResourceError`.
- Blocking reads are cancellation points.
- Read timeouts are protocol/transport-specific but must map into the common timeout hierarchy.

### 3.2. `io.Writer`

```amber
interface io.Writer:
 def write(bytes) -> Int
 def write_all!(bytes)
 def flush!()
 def close!()
 def closed?() -> Bool
```

Normative rules:

- `write(bytes)` may write fewer bytes and returns the count.
- `write_all!(bytes)` either writes all bytes or raises.
- Writes after `close!` raise `ClosedResourceError`.
- Blocking writes are cancellation points.
- `flush!` is a no-op for unbuffered writers, but still valid.

### 3.3. `io.Closeable`

```amber
interface io.Closeable:
 def close!()
 def closed?() -> Bool
```

Normative rules:

- `close!` should be idempotent unless a concrete type explicitly specifies stricter behavior.
- Resource cleanup must not expose raw OS handles to safe user code.
- Errors during close may be raised, but double-close should not corrupt runtime state.

### 3.4. `io.HalfCloseable`

```amber
interface io.HalfCloseable:
 def close_read!()
 def close_write!()
```

Normative rules:

- `close_write!` declares that no more bytes will be written by this side, while reads may continue if the transport/protocol supports it.
- `close_read!` declares that no more bytes will be read by this side, while writes may continue if allowed.
- If a backend does not support half-close, it may map half-close to full close, but this must be documented by the concrete type.

### 3.5. `io.Duplex`

```amber
interface io.Duplex < io.Reader, io.Writer, io.HalfCloseable:
 pass
```

`io.Duplex` is the generic base for bidirectional byte streams such as TCP streams, TLS streams, subprocess pipes, or future protocol channels.

### 3.6. Resource scope sugar

The standard library may define scoped helpers, but they are always sugar over explicit ownership.

```amber
resource.open(...) |r|:
 use(r)
```

must be equivalent to:

```amber
r = resource.open(...)
try:
 use(r)
ensure:
 r.close!()
```

This rule is also used by `net.http.Client#send` and convenience methods.

---

## 4. `net` foundation

### 4.1. TCP

```amber
from net import TcpStream, TcpListener

stream = TcpStream.connect("example.com", 80, timeout: 5.0)
stream.write_all!(bytes)
chunk = stream.read(max_bytes: 4096)
stream.close!()
```

`TcpStream` implements `io.Duplex`.

Rules:

- DNS, connect, read, write, accept, and close waits are cancellation points.
- Timeout errors are normalized.
- OS-specific errors map into Amber error classes.
- No raw socket handle is exposed by safe stdlib.

### 4.2. DNS

```amber
from net import dns

addresses = dns.resolve("example.com")
```

Rules:

- DNS must be cancellable.
- DNS must participate in `open_timeout` when used by `net.http`.
- Resolution errors map to `DnsError` or a subclass.

---

## 5. `net.http` v1 scope

### 5.1. Supported protocol

`net.http` v1 supports:

```text
HTTP/1.1 client over plaintext TCP
basic HTTP/1.1 server hook over plaintext TCP
scheme: http://
default port: 80
request target: origin-form
persistent connections: yes
server keep-alive/pipelining: no; one request per accepted connection
pipelining: no
```

### 5.2. Unsupported schemes

```text
https://  -> UnsupportedSchemeError in net.http v1
http+unix:// -> unsupported in v1
proxy URLs -> unsupported in v1
```

TLS support belongs to a future `net.https` or transport injection layer.

---

## 6. Public API overview

```amber
from net.http import Client, Request, RequestBody, Response, ResponseBody, Headers, Url
from net.http import Server, ServerRequest, ServerResponse

client = Client(
 timeout: null,
 pool_timeout: 5.0,
 open_timeout: 10.0,
 read_timeout: 30.0,
 write_timeout: 30.0,
 idle_timeout: 90.0,

 max_header_bytes: 65536,
 max_status_line_bytes: 8192,
 max_chunk_header_bytes: 8192,
 max_body_bytes: null,

 max_idle_connections: 64,
 max_idle_per_origin: 4,
 max_active_per_origin: 16,

 redirects: :off,
 max_redirects: 5,

 user_agent: "Amber/#{Amber.version}",
 auto_host: true,
 auto_content_length: true
)
```

### 6.1. Scoped response

```amber
client.get("http://example.com/items") |res|:
 if res.ok?():
  res.body_text(limit: 1_000_000)
 else:
  raise HttpStatusError(res.status)
```

### 6.2. Explicit response ownership

```amber
res = client.get("http://example.com/items")
try:
 res.body_text(limit: 1_000_000)
ensure:
 res.close!()
```

### 6.3. Manual streaming upload

```amber
h = client.begin(
 method: :post,
 url: "http://example.com/upload",
 headers: {"content-type": "application/octet-stream"},
 length: null
)

producer.write_intro(h)
producer.write_body(h)
producer.write_tail(h)

h.finish!()

res = h.response()
try:
 res.status
ensure:
 res.close!()
```

---

## 7. `Client`

### 7.1. Constructor

```amber
client = Client(
 timeout: null,
 pool_timeout: 5.0,
 open_timeout: 10.0,
 read_timeout: 30.0,
 write_timeout: 30.0,
 idle_timeout: 90.0,
 max_idle_connections: 64,
 max_idle_per_origin: 4,
 max_active_per_origin: 16,
 redirects: :off,
 max_redirects: 5,
 max_header_bytes: 65536
)
```

`Client(...)` is the preferred constructor form. `Client.new(...)` remains equivalent by Amber constructor-call semantics.

### 7.2. Request execution methods

```amber
client.send(request) -> Response
client.send(request) |response|: ...

client.get(url, headers: {}, timeout: inherit) -> Response
client.get(url, headers: {}, timeout: inherit) |response|: ...

client.head(url, headers: {}, timeout: inherit) -> Response
client.head(url, headers: {}, timeout: inherit) |response|: ...

client.query(url, headers: {}, body: null, timeout: inherit) -> Response
client.query(url, headers: {}, body: null, timeout: inherit) |response|: ...

client.post(url, headers: {}, body: null, timeout: inherit) -> Response
client.post(url, headers: {}, body: null, timeout: inherit) |response|: ...

client.put(url, headers: {}, body: null, timeout: inherit) -> Response
client.patch(url, headers: {}, body: null, timeout: inherit) -> Response
client.delete(url, headers: {}, body: null, timeout: inherit) -> Response
```

Every response-producing method has two forms:

1. **Scoped block form**: response is closed automatically.
2. **Explicit ownership form**: caller must consume, discard, or close the response.

### 7.3. Manual body streaming

```amber
h = client.begin(
 method: :post,
 url: url,
 headers: {},
 length: null,
 timeout: inherit,
 open_timeout: inherit,
 read_timeout: inherit,
 write_timeout: inherit
)
```

Returns `RequestHandle`.

### 7.4. Client lifecycle

```amber
client.close_idle!()
client.close!()
client.closed?()
```

Rules:

- `Client` is synchronized and shareable across strands.
- `Client.close_idle!` closes all idle pooled connections.
- `Client.close!` rejects new requests.
- Active responses may finish, but their connections do not return to the pool after `close!`.
- After `close!`, future send/begin attempts raise `ClosedResourceError`.

---

## 8. `Request`

### 8.1. Construction

```amber
req = Request(
 method: :post,
 url: "http://example.com/items",
 headers: {"content-type": "application/json"},
 body: Json.generate({name: "Amber"})
)
```

### 8.2. Properties

```amber
req.method
req.url
req.headers
req.body
req.origin
req.replayable?()
req.content_length
```

Rules:

- `Request` is immutable after construction.
- Construction snapshots headers.
- `method` accepts `Symbol` or `Str`.
- Method names are normalized to uppercase token form for wire serialization.
- `url` accepts `Url` or `Str`.
- Only `http://` URLs are accepted in `net.http` v1.
- URL fragments are stripped before wire serialization.
- URL userinfo is rejected.
- Host is required.
- Default port is 80.

### 8.3. Body policy by method

The core client does not forbid request bodies for methods such as `GET` or `DELETE`, because the low-level client should not invent protocol law. However:

- `client.get` and `client.head` helpers do not accept body.
- `client.query` accepts body and callers must supply a matching `Content-Type`
  as required by RFC 10008.
- `Request(method: :get, body: ...)` is allowed.
- A lint/profile diagnostic may warn about body use where application semantics are unclear.

---

## 9. `Headers`

### 9.1. Rationale

HTTP headers must not be represented as ordinary `Map`. Field names are case-insensitive, duplicate field lines are possible, order can matter, and not all fields can be safely comma-combined.

### 9.2. API

```amber
headers = Headers()
headers.add!("accept", "application/json")
headers.add!("x-tag", "one")
headers.add!("x-tag", "two")

headers.first("x-tag")     # "one"
headers.all("x-tag")       # ["one", "two"]
headers.include?("accept")
headers.each |name, value|: ...
headers.set!("x-tag", "replacement")
headers.delete!("x-tag")
headers.to_pairs()
headers.to_map()           # StrictMap[Str, Array[Str]]
```

### 9.3. Combined field helpers

```amber
headers.combined("accept")
headers.combined?("set-cookie") # false
```

Rules:

- `combined(name)` is valid only for fields known to be list-compatible.
- `Set-Cookie` is not list-compatible.
- Hidden comma folding is forbidden.

### 9.4. Validation rules

- Field names are ASCII tokens.
- Names compare case-insensitively.
- Stored canonical form is lowercase.
- Values reject CR, LF, and NUL.
- Obsolete folded values are rejected.
- Field-line order is preserved.
- Duplicate field lines are preserved.
- Request construction snapshots headers.
- Response headers are read-only.

---

## 10. Request bodies

### 10.1. Accepted body values

```text
null
Str          # UTF-8 encoded
Bytes
ByteSlice
ByteBuffer
RequestBody
```

### 10.2. Static body

```amber
body = RequestBody.bytes(bytes)
body = RequestBody.text(str, encoding: :utf8)
```

Static bodies are replayable.

### 10.3. Producer body

```amber
body = RequestBody.stream(length: null) |writer|:
 source.each_chunk(size: 65536) |chunk|:
  writer.write_all!(chunk)

req = Request(method: :post, url: url, body: body)
```

Rules:

- `length: Int` emits `Content-Length`.
- `length: null` emits `Transfer-Encoding: chunked`.
- Producer runs once.
- Producer failure closes the connection and propagates.
- Timeout/cancellation closes the connection and propagates.
- Producer bodies are not replayable.

### 10.4. Reader-backed body

```amber
body = RequestBody.from_reader(reader, length: null)
```

Rules:

- Reads from `reader` are pulled by the HTTP client during send.
- `length: Int` requires exactly that many bytes.
- `length: null` uses chunked transfer coding.
- Reader errors close the HTTP connection and propagate.

### 10.5. No HTTP pipe abstraction

`net.http` does not define `RequestBody.pipe`. If a program needs producer/consumer buffering, it should use generic `io`/`task` primitives and write into a `RequestHandle` or provide a reader-backed body.

---

## 11. `RequestHandle`

### 11.1. Purpose

`RequestHandle` is the API for imperative, manually streamed request bodies. It is HTTP-specific but implements `io.Writer`.

It replaces the earlier idea of a generic `HttpExchange`.

### 11.2. Construction

```amber
h = client.begin(
 method: :post,
 url: "http://example.com/upload",
 headers: {"content-type": "application/octet-stream"},
 length: null
)
```

### 11.3. API

```amber
class RequestHandle < io.Writer:
 def write(bytes) -> Int
 def write_all!(bytes)
 def flush!()
 def close!()
 def closed?() -> Bool

 def finish!()
 def response() -> Response
 def abort!()
 def bytes_written -> Int
 def finished?() -> Bool
```

`RequestHandle` itself is the writer. There is no separate `h.body` required, though implementations may expose `h.body` as an alias if desired.

### 11.4. Lifecycle

```text
client.begin(...)
  -> acquire/open connection
  -> write request line and headers
  -> return RequestHandle

h.write_all!(...)
  -> write request body bytes

h.finish!()
  -> finalize request body

h.response()
  -> wait for final response headers
  -> return Response

h.abort!()
  -> close transport
  -> make connection non-reusable
```

### 11.5. Fixed-length body semantics

If `length` is an integer:

- `Content-Length` is emitted.
- `bytes_written` is tracked.
- Writing more than `length` raises `BodyLengthError` and closes the connection.
- `finish!` requires `bytes_written == length`.
- Mismatch raises `BodyLengthError` and closes the connection.

### 11.6. Chunked body semantics

If `length: null`:

- `Transfer-Encoding: chunked` is emitted.
- `write_all!` emits chunk data.
- `finish!` emits the terminating zero-length chunk.
- Request trailers are unsupported in v1.

### 11.7. Response timing

`h.response()` may be called before `h.finish!`.

This is required because a server may send an early final response while the client is still writing the request body.

Rules:

- `response()` waits until final response headers are parsed.
- `response()` returns exactly once.
- Concurrent `response()` calls are invalid and raise `RequestStateError`.
- If response arrives early, subsequent writes may fail with `EarlyResponseError`, `ConnectionClosedError`, or a transport-specific error.
- If `abort!` is called, pending `response()` and write operations are woken with `ExchangeAbortedError` or `CancelledError`.

### 11.8. Async composition

```amber
h = client.begin(method: :post, url: url, length: null)

writer_task = task.async:
 source.each_chunk |chunk|:
  h.write_all!(chunk)
 h.finish!()

reader_task = task.async:
 res = h.response()
 try:
  handle_response(res)
 ensure:
  res.close!()

writer_task.await()
reader_task.await()
```

This uses existing `task.async`; `net.http` does not define a protocol-specific async primitive.

---

## 12. `Response`

### 12.1. Properties

```amber
res.status          # Int
res.reason          # Str
res.version         # :http_1_0 or :http_1_1
res.url             # final Url
res.request         # effective Request
res.headers         # read-only Headers
res.body            # ResponseBody
res.redirects       # Array[RedirectRecord]
res.closed?()
```

### 12.2. Status helpers

```amber
res.informational?()
res.success?()
res.redirect?()
res.client_error?()
res.server_error?()
res.ok?()
```

### 12.3. Body convenience

```amber
res.body_bytes(limit: null)
res.body_text(encoding: :utf8, limit: null)
res.close!()
```

`body_bytes` and `body_text` delegate to `ResponseBody` and consume it.

### 12.4. Response ownership

Rules:

- A `Response` owns a lease over a transport connection until its body is fully consumed, discarded, or closed.
- If the body reaches a valid EOF, the connection may return to the pool.
- If the body is successfully discarded/drained, the connection may return to the pool.
- If the response is closed early, the connection is closed and not reused.
- Protocol error, timeout, or cancellation closes the connection.
- `Response.close!` is idempotent.

---

## 13. `ResponseBody`

### 13.1. API

```amber
res.body.read(max_bytes: 8192)
res.body.each_chunk(size: 8192) |bytes|:
 consume(bytes)

res.body.bytes(limit: null)
res.body.text(encoding: :utf8, limit: null)
res.body.discard!()
res.body.close!()
res.body.consumed?()
res.body.trailers
```

`ResponseBody` implements `io.Reader`.

### 13.2. Single-consumer rule

- `read`, `each_chunk`, `bytes`, and `text` are mutually exclusive consumption modes.
- Repeated or concurrent consumption raises `BodyConsumedError`.
- `trailers` is `null` before EOF.
- After EOF, `trailers` is read-only `Headers`.

### 13.3. `discard!`

`discard!` drains the response body up to configured policy limits and allows connection reuse if framing completes successfully.

Rules:

- If body size exceeds discard limit, connection is closed and `BodyLimitError` may be raised.
- If discard reaches valid EOF, connection may return to pool.
- If discard is cancelled or times out, connection is closed.

### 13.4. `close!`

`close!` abandons unread response data and closes the underlying connection.

---

## 14. Timeout semantics

### 14.1. Options

| Option | Applies to | Renewal |
|---|---|---|
| `timeout` | Whole exchange: pool wait, DNS, connect, request write, response headers, redirects, response body consumption | absolute deadline |
| `pool_timeout` | Waiting for an available per-origin connection slot | no renewal |
| `open_timeout` | DNS + TCP connect attempts | no renewal |
| `write_timeout` | Each transport write operation | renewed after write progress |
| `read_timeout` | Each transport read operation | renewed after read progress |
| `idle_timeout` | Idle pooled connection lifetime | pool-side |
| `body_timeout` | Optional maximum wall-clock time for body consumption only | absolute deadline |

### 14.2. Inheritance and explicit null

Per-request overrides follow this rule:

```text
omitted option -> inherit from client
explicit null  -> disable that timeout
number         -> use that timeout
```

### 14.3. Total deadline and explicit response ownership

If `timeout` is non-null, it creates an absolute deadline attached to the response lease.

This means:

```amber
res = client.get(url, timeout: 10.0)
# waiting here still consumes the same total deadline
res.body_text()
```

If the total deadline expires during later body consumption, `TotalTimeoutError` is raised and the connection is closed.

### 14.4. Timeout errors

```text
HttpTimeoutError < HttpError, TimeoutError
PoolTimeoutError < HttpTimeoutError
OpenTimeoutError < HttpTimeoutError
ReadTimeoutError < HttpTimeoutError
WriteTimeoutError < HttpTimeoutError
BodyTimeoutError < HttpTimeoutError
TotalTimeoutError < HttpTimeoutError
```

All timeout errors identify phase, request, URL, and cause when available.

---

## 15. Cancellation semantics

Cancellation may occur during:

```text
pool wait
DNS resolution
TCP connect
request header write
request body write
response header read
response body read
redirect resolution
body discard
```

Rules:

- Cancellation raises `CancelledError`.
- Connection involved in a cancelled operation is non-reusable.
- Cancellation wakes all waiters on the same request handle/response lease.
- Cancellation during block-scoped response still runs cleanup.
- If cancellation races with protocol error, implementation may choose either error as primary but should retain the other as `cause` or `suppressed` if the runtime supports it.

---

## 16. Redirect policy

### 16.1. Modes

```amber
Client(redirects: :off)
Client(redirects: :manual)
Client(redirects: :safe)
```

### 16.2. `:off`

- Return 3xx response unchanged.
- Do not parse/follow `Location` beyond exposing headers.

### 16.3. `:manual`

- Return 3xx response unchanged.
- Expose parsed `Location` as `res.redirect_location` if valid.
- Do not follow automatically.

### 16.4. `:safe`

Rules:

- Follow 301, 302, 303, 307, 308 for `GET` and `HEAD`.
- For `QUERY`, preserve the method and replayable body across 301, 302, 307,
  and 308; rewrite 303 to `GET` and drop the body (RFC 10008 section 2.5).
- For other methods, follow only 303 by rewriting to `GET` and dropping the body.
- Never replay non-replayable bodies.
- Never forward `authorization`, `cookie`, `proxy-authorization`, or caller-supplied `host` across origin.
- Remove caller-supplied `Host` after redirect.
- Resolve relative `Location` against the current URL.
- Preserve total timeout deadline across redirects.
- Append `RedirectRecord` for each hop.
- Exceeding `max_redirects` raises `TooManyRedirectsError`.
- Redirect to unsupported scheme raises `UnsupportedSchemeError`.

### 16.5. `RedirectRecord`

```amber
record.status
record.from_url
record.to_url
record.method_before
record.method_after
record.cross_origin?()
```

---

## 17. Connection pooling

### 17.1. Pool key

```text
origin = scheme + host + port
```

Host canonicalization must be deterministic. `http://example.com` and `http://example.com:80` share an origin.

### 17.2. Rules

- No HTTP pipelining in v1.
- One active request per HTTP/1.1 connection.
- Idle connections are reused only for the same origin.
- A connection returns to pool only after valid response body EOF or successful `discard!`.
- Early response close closes the connection.
- Protocol failure closes the connection.
- Timeout closes the connection.
- Cancellation closes the connection.
- `Client.close_idle!` closes idle sockets.
- `Client.close!` rejects new sends and prevents active leases from returning.

### 17.3. Limits

```amber
Client(
 max_idle_connections: 64,
 max_idle_per_origin: 4,
 max_active_per_origin: 16,
 pool_timeout: 5.0
)
```

Rules:

- `max_active_per_origin` caps active connections for an origin.
- Waiting for an active slot observes `pool_timeout`.
- Idle eviction is LRU or similarly deterministic.
- Active connections are not killed merely to satisfy idle limits.

---

## 18. HTTP/1.1 wire rules

### 18.1. Request serialization

- Emit HTTP/1.1 request line.
- Use origin-form request target.
- Generate `Host` unless explicitly supplied and valid.
- Reject user-supplied `Transfer-Encoding` in v1 request headers.
- Reject or sanitize user-supplied hop-by-hop headers controlled by the client.
- Static bodies use `Content-Length`.
- Unknown-length streaming bodies use `Transfer-Encoding: chunked`.
- User-supplied `Content-Length` must exactly match selected body framing.
- Request trailers are unsupported in v1.
- `Expect: 100-continue` is unsupported in v1.

### 18.2. Response parsing

- Strict CRLF parsing.
- Bounded status line.
- Bounded header section.
- Bounded chunk header.
- Informational 1xx responses are consumed until final response.
- 101 Switching Protocols is unsupported.
- `HEAD`, 1xx, 204, and 304 responses have no response body.
- `Transfer-Encoding` plus `Content-Length` is `ProtocolError`.
- Duplicate `Content-Length` values are accepted only when all normalized values are identical.
- Unsupported transfer codings raise `ProtocolError`.
- Chunk extensions are parsed and exposed if chunk-aware iteration is used.
- Trailers are parsed and exposed after EOF.
- Content encodings such as gzip are not decoded by core `net.http`.

### 18.3. Chunk-aware response streaming

Optional bounded chunk-aware API:

```amber
res.body.each_chunk_with_ext(size: 8192) |part|:
 consume(part.bytes)
 if part.first?():
  inspect(part.extensions)
```

`BodyChunk` exposes:

```amber
part.bytes
part.extensions
part.first?()
part.last?()
```

Large HTTP chunks may be delivered as multiple bounded `BodyChunk` parts while preserving transfer-chunk boundaries.

---

## 19. URL and origin model

### 19.1. `Url`

```amber
url = Url.parse("http://example.com:8080/path?q=amber")

url.scheme
url.host
url.port
url.path
url.query
url.fragment
url.origin
```

Rules:

- `http` is the only supported scheme in `net.http` v1.
- Host is required.
- Fragment is not sent on the wire.
- Userinfo is rejected.
- Invalid percent-encoding is rejected.
- Relative URL resolution is used only for redirects.

### 19.2. `Origin`

```amber
origin.scheme
origin.host
origin.port
origin == other_origin
origin.key()
```

Used for:

- connection pooling
- credential stripping on redirect
- capability checks
- observability event grouping

---

## 20. Capability and effects

### 20.1. Capabilities

HTTP operations require:

```text
net.connect(host, port)
time.now / time.sleep if deadlines are modeled through capabilities
trace.emit if tracing hook emits external events
```

Rules:

- Importing `net.http` does not grant network access.
- Constructing `Client` does not connect.
- `send`, `begin`, and redirects require `net.connect` for target origin.
- Denied capability raises `CapabilityError` before opening a socket.
- Redirect to a new origin performs a new capability check.

### 20.2. Effects

Suggested effect annotation style:

```amber
def fetch_json(url as Str) -> Map !{net.connect, time}:
 Client().get(url) |res|:
  Json.parse(res.body_text())
```

---

## 21. Error model

### 21.1. HTTP errors

```text
HttpError < Error

RequestError < HttpError
InvalidUrlError < RequestError
InvalidMethodError < RequestError
InvalidHeaderError < RequestError
UnsupportedSchemeError < RequestError
RequestStateError < RequestError

ProtocolError < HttpError
HeaderLimitError < ProtocolError
StatusLineLimitError < ProtocolError
ChunkError < ProtocolError
UnexpectedEofError < ProtocolError

BodyError < HttpError
BodyConsumedError < BodyError
BodyLengthError < BodyError
BodyLimitError < BodyError

RedirectError < HttpError
TooManyRedirectsError < RedirectError
NonReplayableRedirectError < RedirectError

EarlyResponseError < HttpError
ExchangeAbortedError < HttpError

HttpTimeoutError < HttpError, TimeoutError
PoolTimeoutError < HttpTimeoutError
OpenTimeoutError < HttpTimeoutError
ReadTimeoutError < HttpTimeoutError
WriteTimeoutError < HttpTimeoutError
BodyTimeoutError < HttpTimeoutError
TotalTimeoutError < HttpTimeoutError
```

### 21.2. Transport errors retained

Lower-level errors remain visible:

```text
DnsError
ConnectionError
ConnectionRefusedError
ConnectionResetError
CancelledError
ClosedResourceError
CapabilityError
```

### 21.3. Error context

HTTP errors should expose:

```amber
err.phase
err.request
err.url
err.origin
err.cause
```

where applicable.

---

## 22. Observability hooks

### 22.1. Trace configuration

```amber
client = Client(
 trace: http.trace |event|:
  log(event.name, event.duration, event.origin)
)
```

### 22.2. Events

```text
request.start
pool.wait.start
pool.wait.end
dns.start
dns.end
connect.start
connect.end
write.headers.start
write.headers.end
write.body.start
write.body.chunk
write.body.end
read.headers.start
read.headers.end
read.body.chunk
read.body.eof
redirect
request.error
request.end
```

### 22.3. Rules

- Hooks must not observe raw body bytes unless body tracing is explicitly enabled.
- Hook failure must not corrupt protocol state.
- Hook callable runs in the sending strand unless configured otherwise.
- Emitting trace events outside the sandbox requires `trace.emit` capability.

---

## 23. Convenience layers outside core

Core `net.http` remains strict and low-level. Higher-level helpers may live in separate modules:

```amber
from net.http.json import get_json, post_json
from net.http.form import FormBody
```

Example:

```amber
post_json("http://api.local/users", {name: "Ada"}) |res|:
 res.expect_status!(201)
 res.json()
```

Rules:

- Helpers lower into `Request`, `Headers`, `RequestBody`, and `Client.send`.
- Helpers must not define alternate protocol semantics.
- Helpers may add application-level defaults such as `accept: application/json`.

---

## 24. Examples

### 24.1. Simple GET with block scope

```amber
client = Client()

client.get("http://example.com/items") |res|:
 if res.ok?():
  puts res.body_text(limit: 1_000_000)
 else:
  puts "HTTP error: #{res.status}"
```

### 24.2. Simple GET with explicit ownership

```amber
res = client.get("http://example.com/items")
try:
 text = res.body_text(limit: 1_000_000)
 puts text
ensure:
 res.close!()
```

### 24.3. POST JSON

```amber
req = Request(
 method: :post,
 url: "http://example.com/items",
 headers: {
  "content-type": "application/json",
  "accept": "application/json"
 },
 body: Json.generate({name: "Amber"})
)

client.send(req) |res|:
 res.body_text(limit: 1_000_000)
```

### 24.4. Producer streaming body

```amber
body = RequestBody.stream(length: null) |w|:
 file.each_chunk(size: 65536) |chunk|:
  w.write_all!(chunk)

req = Request(
 method: :post,
 url: "http://example.com/upload",
 body: body
)

client.send(req) |res|:
 res.status
```

### 24.5. Manual streaming body across functions

```amber
def write_prefix(w):
 w.write_all!(encode_prefix())

def write_payload(w, source):
 source.each_chunk |chunk|:
  w.write_all!(chunk)

def write_suffix(w):
 w.write_all!(encode_suffix())

h = client.begin(
 method: :post,
 url: "http://example.com/report",
 headers: {"content-type": "application/x-amber-report"},
 length: null
)

write_prefix(h)
write_payload(h, source)
write_suffix(h)

h.finish!()

res = h.response()
try:
 res.body_text(limit: 1_000_000)
ensure:
 res.close!()
```

### 24.6. Manual streaming with async read/write

```amber
h = client.begin(method: :post, url: url, length: null)

writer_task = task.async:
 source.each_chunk |chunk|:
  h.write_all!(chunk)
 h.finish!()

reader_task = task.async:
 res = h.response()
 try:
  handle_response(res)
 ensure:
  res.close!()

writer_task.await()
reader_task.await()
```

### 24.7. Reader-backed body using generic IO

```amber
reader = File.open("payload.bin")
body = RequestBody.from_reader(reader, length: reader.size)

req = Request(method: :put, url: url, body: body)

client.send(req) |res|:
 res.status
```

---

## 25. Conformance matrix

### 25.1. API and resource lifecycle

- Constructor defaults.
- Constructor override validation.
- `Client(...)` and `Client.new(...)` equivalence.
- `send(req) |res|` closes response after block.
- `send(req)` requires caller ownership.
- `get/post/...` scoped and explicit forms.
- Response `close!` idempotence.
- Request immutability.
- Response headers read-only.
- Client close behavior.

### 25.2. Headers

- Case-insensitive lookup.
- Lowercase canonical storage.
- Duplicate preservation.
- Field-line order preservation.
- `first`, `all`, `to_pairs`, `to_map`.
- Rejection of invalid names.
- Rejection of CR/LF/NUL in values.
- Obsolete folded input rejection.
- No hidden comma folding.

### 25.3. Static and streaming requests

- GET without body.
- POST with static bytes.
- POST with text body.
- Fixed-length streaming upload.
- Chunked streaming upload.
- Reader-backed upload.
- Manual `RequestHandle` upload.
- Write after finish.
- Write after abort.
- Fixed-length underwrite.
- Fixed-length overwrite.
- Producer exception.
- Cancellation during body production.

### 25.4. Response bodies

- Content-Length response.
- Chunked response.
- Chunk extensions.
- Trailers.
- Close-delimited response.
- HEAD response body suppression.
- 204 body suppression.
- 304 body suppression.
- `body_bytes` limit.
- `body_text` encoding.
- Single-consumer enforcement.
- Concurrent consumption failure.
- `discard!` allows reuse.
- Early `close!` closes socket.

### 25.5. Timeouts and cancellation

- Pool timeout.
- DNS/open timeout.
- Connect timeout.
- Request header write timeout.
- Request body write timeout.
- Response header read timeout.
- Response body read timeout.
- Total timeout during body read after explicit `send`.
- Cancellation during pool wait.
- Cancellation during DNS.
- Cancellation during connect.
- Cancellation during write.
- Cancellation during read.
- Cancellation during discard.

### 25.6. Protocol parser

- Valid status line.
- Oversized status line.
- Valid headers.
- Oversized header section.
- Malformed CRLF.
- Duplicate equal Content-Length.
- Duplicate conflicting Content-Length.
- Transfer-Encoding plus Content-Length.
- Unsupported transfer coding.
- Invalid chunk size.
- Oversized chunk header.
- Unexpected EOF.
- Informational 1xx before final response.
- Unsupported 101.

### 25.7. Pooling

- Reuse after full body read.
- Reuse after successful discard.
- No reuse after early close.
- No reuse after timeout.
- No reuse after cancellation.
- No reuse after protocol error.
- Idle eviction.
- Per-origin idle cap.
- Per-origin active cap.
- Global idle cap.
- `Client.close_idle!`.
- `Client.close!`.
- Concurrent strands.

### 25.8. Redirects

- `:off` returns 3xx.
- `:manual` exposes parsed Location.
- `:safe` follows GET/HEAD 301/302/303/307/308 and RFC 10008 QUERY redirects.
- 303 rewrites non-GET/HEAD to GET.
- 307/308 preserve method for allowed methods.
- Non-replayable body is not redirected.
- Relative Location resolution.
- Cross-origin credential stripping.
- Redirect to unsupported scheme.
- Redirect loop.
- `max_redirects`.
- Total timeout across redirects.
- Capability re-check on redirected origin.

### 25.9. Security and capabilities

- Denied `net.connect` before socket open.
- Denied redirected origin.
- Invalid host rejection.
- URL userinfo rejection.
- Header injection rejection.
- No raw socket exposure.
- Trace hook capability rules.

### 25.10. Implementation parity

- VM/native parity.
- Deterministic fake transport tests.
- Loopback integration tests.
- Both runtime `Value` representations if applicable.
- Error ancestry registry conformance.

---

## 26. Implementation notes

### 26.1. Parser/serializer

Implement HTTP/1.1 parser and serializer as incremental state machines over `io.Reader`, `io.Writer`, and `ByteBuffer`.

Avoid reading unbounded lines or headers into memory.

### 26.2. Deadlines

Deadlines should be absolute runtime objects, not repeated relative timeout guesses. A total timeout must propagate through redirects and response body consumption.

### 26.3. Pool lease

`ResponseBody` owns the pool lease. The lease is released only on valid EOF or successful `discard!`.

### 26.4. Manual request handle

`RequestHandle` should be a thin protocol-specific writer. It should not buffer arbitrarily. Backpressure should come from underlying transport writes and task scheduling.

### 26.5. Early response

The implementation should allow response parsing while request body writing is still possible. This is necessary for early server errors. If the transport/backend cannot support concurrent read/write on the same connection, that limitation must be explicit in the profile.

---

## 27. Suggested file split

```text
stdlib/io/protocols.amber
stdlib/io/buffer.amber
stdlib/net/tcp.amber
stdlib/net/dns.amber
stdlib/net/http/client.amber
stdlib/net/http/request.amber
stdlib/net/http/response.amber
stdlib/net/http/headers.amber
stdlib/net/http/body.amber
stdlib/net/http/parser.amber
stdlib/net/http/pool.amber
stdlib/net/http/errors.amber
stdlib/net/http/redirect.amber
```

---

## 28. References

- RFC 9110 — HTTP Semantics: https://www.rfc-editor.org/rfc/rfc9110.html
- RFC 9112 — HTTP/1.1: https://www.rfc-editor.org/rfc/rfc9112.html
- Amber unified language/runtime design, current uploaded draft.
- Prior loose `S6. HTTP client` sketch, superseded by this document.

---

## 29. Final normative summary

The final design decision is:

```text
1. Block-suffix response APIs are convenience resource-scope forms.
   They never replace explicit ownership APIs.

2. Streaming belongs in v1.
   It is required for correct resource, timeout, pooling, and large-body semantics.

3. Generic pipe/exchange primitives do not belong to net.http.
   They belong to io/task.

4. net.http exposes RequestHandle for manual HTTP request-body streaming.
   RequestHandle implements io.Writer and adds HTTP-specific finish!/response()/abort!.

5. ResponseBody implements io.Reader and owns the connection lease.

6. Headers are ordered multi-value structures, not ordinary maps.

7. HTTP/1.1 parsing is strict, bounded, and deterministic.

8. Redirects are off by default and safe only by explicit policy.

9. Connection pooling is observable only through resource behavior, not raw sockets.

10. All network operations go through capabilities, timeout phases, and cancellation points.
```

---

## 30. Implementation plan

**Status (2026-07-01).** The runtime substrate this design assumes is built, and
the `net.http` implementation has advanced through the Phase 7 basic server
slice. The current repository includes the client phases, convenience helpers,
and the low-level `net.http.Server` request-hook surface. Remaining work is
above or beyond this slice: TLS/HTTPS, HTTP/2/3, richer server protocol features,
and web-framework routing/templating.

### 30.1. Substrate already in place

```text
- Cooperative async IO: epoll/kqueue reactor + scheduler park/resume. A task
  blocked on a socket read!/write! yields its worker and resumes on readiness
  -- the "task owns async" assumption this document relies on (section 2.3).
- net.tcp connect / listen / accept, io.ByteBuffer, Str#bytes -> io.Bytes,
  all reachable from Amber source.
- Strand confinement with an explicit `adopt!` handoff verb: accept on one
  strand, hand the stream to a worker task (the connection-handling pattern),
  on a sound single-namespace owner-id model.
- task / cancellation / structured concurrency; Url, Json, base64/hex codecs.
- DNS via getaddrinfo inside connect (blocking; not yet reactor-cancellable).
```

Substrate gaps to close before they are needed:

```text
- Cooperative IO park currently covers only INFINITE-timeout single-shot
  read!/write!. A finite (read_timeout/write_timeout) op still blocks its
  worker, because under park-and-retry a relative timeout would reset on each
  resume. Carry an absolute deadline across retries (D4) before Phase 4.
- DNS is blocking; open_timeout cannot yet cancel it (deferred, section 30.4).
```

### 30.2. Phase 0 — decisions to lock before coding

```text
D1. io protocol. Adopt the runtime's buffer-based contract (mixin io.Reader:
    read!(buf) -> Int, 0 = EOF; mixin io.Writer: write!/write_all!;
    io.Closer/io.Duplex), NOT the `interface ... < ...` surface sketched in
    section 3 (Amber has no `interface` keyword; protocols are mixins).
    ResponseBody / RequestHandle / TcpStream `include` those mixins. Retires the
    section-3 fork and the read()->Bytes / EOF-representation ambiguity.

D2. Transport injection. Client(connector:) takes an io.Duplex factory
    (default PlainTcpConnector). One seam serves (a) deterministic fake-transport
    conformance tests, (b) a later net.https TlsConnector, and (c) http+unix --
    none requiring net.http changes. The origin capability check lives at connect.

D3. Retry. One transparent retry, only when the connection was reused from the
    pool AND zero response bytes were observed AND the request is replayable.
    Never on a fresh connection; never after any response byte.

D4. Finite-timeout cooperative reads (substrate prerequisite for Phase 4).

D5. Defaults. Client(base_url:, default_headers:, auth:) with request-overrides-
    default precedence; query: map with percent-encoding owned by Url; send
    identity only and do NOT advertise Accept-Encoding (no auto-decompression in
    v1); Map-literal headers are Str-keyed (hyphenated names, no duplicates).
```

### 30.3. Build phases

Each phase closes a slice of the section-25 conformance matrix; file layout per
section 27.

```text
Phase 1  io protocol + wire codec
  io.Reader/Writer/Closer/Duplex mixins (TcpStream includes them); HTTP/1.1
  request serializer + response parser as incremental state machines over
  ByteBuffer (request line/origin-form, header block, status line,
  Content-Length, chunked + extensions/trailers, bounded everywhere).
  Gate: 25.2 (headers) + 25.6 (protocol parser).

Phase 2  core request/response (single exchange)
  Url/Origin, Headers (ordered multimap), Request (immutable), Response,
  ResponseBody.read!/each_chunk/bytes!/text!; Client.send(req) and the scoped
  send(req)|res|: form; per-origin net.connect capability; static bodies.
  Gate: 25.1 (api/lifecycle) + 25.4 (response bodies) + 25.9 (capabilities).

Phase 3  streaming + lease
  RequestBody.bytes/text/stream/from_reader; RequestHandle (io.Writer +
  finish!/response!/abort!); ResponseBody owns the connection lease (EOF or
  discard! -> reusable; early close! -> closed). Uses D4.
  Gate: 25.3 (static/streaming requests) + 25.4 (lease/reuse).

Phase 4  pooling + timeouts
  Per-origin keep-alive pool, LRU idle eviction, max_idle/active limits,
  pool_timeout; absolute total deadline + per-phase open/read/write/body
  timeouts on reactor deadlines; close_idle!/close!.
  Gate: 25.5 (timeouts/cancellation) + 25.7 (pooling).

Phase 5  redirects
  :off / :manual / :safe; 303 method rewrite, 307/308 preserve; strip
  authorization/cookie/host across origin; relative Location resolution;
  max_redirects; capability re-check on the new origin; RedirectRecord.
  Gate: 25.8 (redirects).

Phase 6  convenience + observability (outside core)
  net.http.json get_json/post_json, net.http.form FormBody; trace hooks
  (section 22). Lower into Phase 2-3 primitives only.

Phase 7  basic HTTP server
  IMPLEMENTED as the current low-level server slice. net.http.Server,
  ServerRequest, and ServerResponse provide plaintext HTTP/1.1 request parsing
  over net.tcp listener sockets; one request per accepted connection with
  explicit Connection: close in the first implementation. Server(workers: N,
  max_concurrent_per_worker: M) owns an N-worker scheduler and caps in-flight
  request hooks at N*M. Server#serve accepts a block hook:

    server.serve |req|:
      ServerResponse.text("ok")

  The hook is the integration point for future web frameworks: framework
  routers/adapters accept a ServerRequest and return a ServerResponse-compatible
  value. Hook execution uses the existing task scheduler and cooperative park
  model rather than adding a net.http-specific async abstraction.
```

### 30.4. Deferred (v1 non-goals, per section 1.2)

```text
TLS/net.https (lands via the D2 connector), HTTP/2 & /3, proxy/CONNECT,
cookies, cache, automatic gzip/br/deflate decoding, multipart builder,
WebSocket/Upgrade, Expect: 100-continue, request trailers, cancellable DNS.
```

### 30.5. Engineering conventions

```text
- Native C++ over the StdlibHost facade (as Json/Url/codecs).
- Verify every phase on both value reps (variant + tagged) and through
  backend-equivalence; the client is concurrent, so run ThreadSanitizer.
- Tests: deterministic fake-transport (D2) for protocol/pool/redirect logic;
  loopback net.tcp integration tests; a corpus/C++ test asserting the
  cooperative-park counter for an in-task socket read (proving reads yield).
```
