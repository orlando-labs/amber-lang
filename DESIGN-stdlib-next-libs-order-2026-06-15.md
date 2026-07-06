# DESIGN: Next stdlib libraries — implementation order

Date: 2026-06-15
Status: design / decision-made — no code change in this doc
Scope: new native libraries above the runtime-facing layer — `Json`, `Encoding`,
`SecureRandom`, `Uuid`, `Time`, `Digest`, `Url`, `net.http`; their registration
in `runtime/vm.cpp` and `runtime/vm.h`
Follows: `amber_unified_final_spec.md` Part III ("Stdlib: план и приоритеты
слоя", S1–S7 / STD-001…STD-042). The S-series runtime layers (collections,
task/sync, watch, io, low-level net) are done or functional; this doc orders the
*application-level* libraries that sit on top of them.

## 1. Context and the question

The S-series roadmap in the spec ordered the **runtime-facing primitives**:
collections → task/sync → watch → io → networking → HTTP. Those are built. The
open question is the order for the **application libraries** that ride on that
foundation — the ones a user reaches for directly: JSON, random, hashing, time,
encodings, URLs, an HTTP client.

This doc fixes that order and the v1 scope of each. It does not change the
language or add core syntax; every library is a native runtime type, same as
`Math` / `io` / `net` today.

## 2. State of the world (2026-06-15)

### 2.1 Foundation already shipped (reused by everything below)

- **Collections / Map / Bytes** — `STD-001`…`STD-006` done. Crucially,
  name-indifferent `Map` (so `{user_id: id}` patterns match string-keyed
  payloads) is resolved, which is the precondition the spec attaches to `Json`.
- **io foundation** — `Bytes`, `ByteBuffer`, `ByteSlice`, `Pipe`, `File`,
  reader/writer/closeable contracts (`runtime/io.h`, `runtime/io.cpp`).
- **Low-level net** — `net.tcp` / `net.udp` wired (`runtime/vm.cpp:12635`) over a
  real socket layer (`runtime/io.cpp`). **DNS already resolves** via
  `getaddrinfo` in `resolve_endpoint` (`runtime/io.cpp:343`). So `STD-040`
  (TCP) and `STD-041` (DNS) are functionally present; only the conformance
  corpus is unmarked.

### 2.2 Greenfield (this doc)

Confirmed absent — 0 references in `runtime/vm.cpp`: `Json`, `Random` /
`SecureRandom`, crypto / `Digest` / SHA, wall-clock `Time`, `Base64` / hex,
`Uuid`, `Url`, `net.http`. The spec already *specifies* most of them: `Json`
(`Json.parse → Map`, `map: StrictMap` opt-in), `net.http` (`package net.http`,
client-first, "TLS feature-gated"), `Time` and `UUID` as type-annotation forms.

### 2.3 Missing primitives

- **No entropy source.** No `getentropy` / `getrandom` / `arc4random` anywhere
  in `runtime/`. `SecureRandom` introduces it (a small, isolated syscall).
- **No TLS.** Nothing links OpenSSL / Secure Transport. This is the single fact
  that scopes the HTTP milestone — see §5.

### 2.4 Implementation substrate

There is no `.am`-source stdlib shipping path; libraries are native C++. Each
new library is three edits:

1. a `RuntimeNativeTypeKind` enum value (`runtime/vm.h:218`);
2. a path registration in `lookup_native_prelude_constant`
   (`runtime/vm.cpp:12598`);
3. a selector-dispatch handler (pattern: the `Math` handler at
   `runtime/vm.cpp:18922`).

Errors normalize into existing/added Amber error classes via the error registry,
exactly as the io/net layers do.

Both of those edits land in two ~4,000- and ~40-branch `if`-chains inside the
1.1 MB `vm.cpp`. Layer 0 (§4.0) replaces that pattern with a registration/dispatch
table so a new library is a self-contained `runtime/stdlib_<name>.*` translation
unit instead of more branches — which is why it is built before `Json`.

## 3. Recommended order

```text
Layer 0 stdlib substrate              registry/dispatch seam, build FIRST
Tier 1  Json  ->  Encoding                 pure compute, no new OS surface
Tier 2  SecureRandom -> Uuid               small OS surface, identifiers
        Time                               (independent)
        Digest                             (independent)
Tier 3  Url  ->  net.http (plaintext)      networking payoff
Deferred  regex · TLS/https · gzip · http server
```

Layer 0 is not a library — it is the seam that lets every library below it land
as `runtime/stdlib_<name>.{h,cpp}` instead of another branch in `vm.cpp`. It is
specified in §4.0 and must precede `Json`.

Ordering principle (inherited from the spec): nothing depends downward on
something not yet built, cheapest-and-highest-leverage first, new OS surface
introduced in the smallest possible isolated step.

## 4. Per-library v1 scope

### 4.0 Layer 0 — stdlib substrate cleanup *(build before 4.1)*

Do this before adding any new library. Without it, every lib in §4.1–4.8 adds
two branches to two already-huge `if`-chains in `runtime/vm.cpp`; with it, a lib
is a self-contained translation unit plus one registration line.

**The problem in the current code.** A native library is reached through two
parallel hand-written chains, both inside the 1.1 MB `vm.cpp`:

- **Dispatch:** `try_apply_native_stdlib_send` (`runtime/vm.cpp:18841`) — a single
  ~4,000-line member function that is one long `if (kind ==
  RuntimeNativeTypeKind::X) { … }` per native type. Method bodies for `Math`,
  `Amber`, `io`, `net`, the scalar/collection types, etc. all live inline here.
- **Name resolution:** `lookup_native_prelude_constant` (`runtime/vm.cpp:12598`)
  — a ~40-branch `if (path == "Json") return Value::native_type(…)` chain that
  maps a source path (`"Math"`, `"io.ByteBuffer"`, `"net.tcp"`) to a
  `RuntimeNativeTypeKind`.

Adding a library means editing both, plus the `RuntimeNativeTypeKind` enum
(`runtime/vm.h:218`) and `native_type_name` (`runtime/vm.cpp:6872`). Four edits
across two giant files, every time. That is what to stop before the count of
libs grows.

**What already helps.** The dispatch contract is a clean chain-of-responsibility
already: `SendStatus { Matched, NotHandled, Faulted }` (`runtime/vm.cpp:9083`),
and the caller `try_apply_scalar_send` (`runtime/vm.cpp:23571`) treats
`NotHandled` as "fall through." So a registry can be inserted as a pure addition
that returns `NotHandled` for anything it doesn't own — no behavior change, no
big-bang rewrite.

**Design.**

1. **Keep the proven handler contract.** A library handler is a function with
   the existing shape, packaged as one context object instead of seven
   positional params:

   ```text
   SendStatus json_dispatch(NativeStdlibCall &call);
   ```

   `NativeStdlibCall` carries `{ frame, kind, selector, args, block, kw_args,
   out }` and a narrow helper facade — the guards already used inline today:
   `fault(class, msg)`, `require_arity(n)`, `require_no_block()`, keyword lookup
   (`keyword_arg_value`, `reject_unknown_keywords`, `bool_keyword`), value
   constructors, and string interning into `module_`. Pure-compute libs
   (`Json`/`Encoding`/`SecureRandom`/`Uuid`/`Time`/`Digest`) need only this small
   facade; io/net libs get scheduler access through the same context. The facade
   is the stdlib runtime ABI gestured at in the io project docs — pin it here.

2. **Two registries, populated once.** Replace the two if-chains with tables:
   - `kind -> handler` for dispatch;
   - `path -> kind` (+ display name) for resolution and `native_type_name`.

   Populate them with an explicit `register_builtin_stdlib(NativeRegistry&)`
   called once during `Vm` construction — *not* static initializers, to avoid
   static-init-order fiascos. Each lib contributes a `register_json(registry)`
   that adds its names and its handler.

3. **Dispatch becomes a lookup.** `try_apply_native_stdlib_send` first consults
   the `kind -> handler` table; a registered handler's `Matched`/`Faulted` is
   returned directly, `NotHandled` falls through to the remaining legacy inline
   chain. New libs are born on the registry; legacy handlers stay where they are.

4. **Migrate one reference handler, not all.** Move `Math` (small,
   self-contained, no Vm-state coupling beyond the facade) into
   `runtime/stdlib_math.cpp` against the new ABI first. It validates the context
   facade before `Json` depends on it. The other legacy handlers migrate
   opportunistically, never in a single sweep.

5. **Files & build.** New: `runtime/stdlib_registry.{h,cpp}` (registry +
   `NativeStdlibCall`), `runtime/stdlib_math.cpp` (reference migration), then
   `runtime/stdlib_json.cpp`, `runtime/stdlib_random.cpp`, … Add each `.cpp` to
   the runtime/vm sources in `Makefile` and to the cached runtime archive used by
   native builds. Per-lib tests follow the existing pattern
   (`stdlib_collections_tests`, `stdlib_task_tests`) as `stdlib_json_tests`, etc.

**Deps:** none — this is below every library. **Cost:** small–medium. The only
real risk is getting the `NativeStdlibCall` facade surface right; migrating
`Math` first de-risks it.

**DoD:**
- registry routes at least one migrated kind (`Math`) end to end; unknown
  kinds/selectors return `NotHandled` and hit the unchanged legacy path;
- full existing corpus is byte-for-byte unchanged (pure refactor gate — reuse the
  backend-equivalence harness);
- adding a new library touches `vm.cpp`/`vm.h` at most once (its
  `register_*` call and enum entry), ideally zero once registration is fully
  data-driven; everything else lives in `runtime/stdlib_<name>.*`.

### 4.1 Json — *first*

- **Why first:** depends only on `Map` / `Array` / `Float` / `String` (all
  done); zero syscalls; fully deterministic and trivially corpus-fuzzable. It is
  the highest-leverage lib — machine-readable tooling output (already wanted by
  the spec), every HTTP body, every config file — and it exercises the
  name-indifferent-map work end to end.
- **API (match the spec):** `Json.parse(text) -> Map` (ordinary map by default;
  string keys, name-indifferent, not globally symbolized). Strict preservation
  opt-in `Json.parse(text, map: StrictMap)`. `Json.generate(value)` /
  `value.to_json`. Correct number (Int vs Float), escape, and UTF-8 handling.
- **Deps:** none beyond foundation.
- **Cost:** medium (recursive-descent parser + generator; number/escape edge
  cases are the work).
- **DoD:** round-trip corpus; RFC 8259 conformance set incl. malformed inputs →
  `Json`-namespace parse error; `Json.parse(body)` feeding `case … in {…}`
  pattern matching (per spec §16.5).

### 4.2 Encoding (base64 / base64url / hex)

- **Why here:** pure, tiny, depends only on `Bytes`; a prerequisite for Tier 2
  (token formatting, digest output) and Tier 3 (basic-auth, data URLs).
- **API:** `Encoding.base64(bytes)` / `.base64_decode`, `.base64url` variants,
  `.hex` / `.hex_decode`. Strict and lenient decode modes.
- **Cost:** small. **DoD:** RFC 4648 vectors + invalid-input errors.

### 4.3 SecureRandom

- **Why here:** introduces the one missing primitive — ~10 lines wrapping
  `getentropy` / `arc4random_buf` (macOS) and `getrandom` (Linux), kept in one
  isolated spot. Everything random downstream composes on it.
- **API:** `SecureRandom.bytes(n)`, `.int(range)`, `.hex(n)`, `.base64(n)`,
  `.uuid` (delegates to §4.4).
- **Cost:** small. **DoD:** length/range/exclusivity tests; never blocks; error
  surface on syscall failure.

### 4.4 Uuid

- **API:** `Uuid.v4()` now; `Uuid.v7()` once Time lands; `parse` / `to_str`.
  Composes SecureRandom + hex.
- **Cost:** small. **DoD:** version/variant bits, canonical formatting,
  parse round-trip; aligns with the `id as UUID` annotation grammar in the spec.

### 4.5 Time

- **Why here:** needs a wall-clock syscall (`clock_gettime`) — today only
  `steady_clock` exists, used for scheduling. Needed for HTTP `Date` headers,
  log/JSON timestamps, and Uuid v7.
- **v1 scope:** monotonic clock + UTC instant + epoch (s/ms/ns) + ISO-8601
  format & parse. **Defer** the timezone database and civil-calendar arithmetic.
- **Cost:** medium. **DoD:** epoch round-trip, ISO-8601 parse/format vectors,
  monotonic vs wall separation.

### 4.6 Digest

- **API:** `Digest.sha256(bytes)`, `.sha1`, `.hmac_sha256(key, bytes)`;
  `crc32`, `md5`, `sha1`, russian ГОСТ digests. Output via Encoding (§4.2). Pure compute (vendor SHA-256 or call
  platform CommonCrypto). Independent of §4.3–4.5, so schedulable anywhere in
  Tier 2. Full AOT/native.
- **Cost:** medium. **DoD:** NIST/RFC 2104 test vectors.

### 4.7 Url

- **Why here:** prerequisite for HTTP, useful standalone.
- **API:** `Url.parse(s)` returns a Map with `scheme`, `authority`,
  `userinfo`, `host`, `port`, `path`, `query`, `fragment`, and `query_map`;
  `Url.build(parts)` accepts that Map shape (or a compatible Map);
  `Url.percent_encode(s)` / `Url.percent_decode(s)` work on URL components;
  `Url.parse_query(s)` returns nested Maps/Lists/Strs for bracket notation
  such as `map[a]=1&map[b][x]=2&c[]=1&c[]=2`;
  `Url.build_query(map)` accepts nested Str/List/Map values.
- **Cost:** small–medium. **DoD:** WHATWG/RFC 3986 parse vectors, encode
  round-trips.

### 4.8 net.http (client, plaintext)

- **Why last:** depends on TCP + DNS (done), `ByteBuffer` (done), and `Url`
  (§4.7). Biggest surface.
- **v1 scope (per spec S6, and §5 below):** `Client.new(timeout:)`,
  `client.get(url)` / `client.send(Request)`; `Request.new(method:, url:,
  headers:, body:)`; `Response(status, headers, body)` with `body_bytes()` /
  `body_text()`. Bodies are async/strand-aware for asynchronous reading with `.each_chunk`/`.each_chunk_with_ext` (for chunked transfer-encoding, supporting chunk extensions) and writing. Header normalization, chunked transfer-decoding, redirects
  **off** by default, timeout covering connect+read. **No server. Plaintext
  only** — `https://` is gated (see §5).
- **Cost:** large. **DoD:** GET/POST success, timeout, cancellation, connection
  failure, invalid URL, header normalization. Body streaming is a later layer,
  not v1. (Closes a plaintext subset of `STD-042`.)

## 5. Decision: TLS is gated, HTTP ships plaintext-first

**Chosen (2026-06-15):** ship `net.http` over plain TCP now (`http://` only) and
feature-gate HTTPS for a later milestone. This matches the spec's explicit "TLS
can be feature-gated if the implementation host is not ready" and keeps the HTTP
milestone at its diagrammed cost/position instead of dragging a platform-TLS
integration into the critical path.

Consequence to track honestly: a plaintext-only client cannot reach the majority
of real `https://` APIs. The first `net.http` release is therefore a *protocol*
milestone, not a "use it against the public internet" milestone. The follow-up
TLS work (Secure Transport / Network.framework on macOS + OpenSSL on Linux, or a
vendored BearSSL/mbedTLS) is a separate, named effort and should be scheduled
before `net.http` is advertised as generally useful.

## 6. Reconciliation with the spec's S-series

This plan does not replace the S-series; it layers on top of S4/S5:

- S1–S4 (collections, task/sync, watch, io): done — depended on, not reopened.
- S5 (TCP/DNS): functional; this plan consumes it for §4.8 and motivates closing
  the `STD-040`/`STD-041` conformance corpus alongside.
- S6 (HTTP client): realized by §4.7–4.8 at plaintext scope.
- S7 (advanced concurrency): untouched and still out of v1.

The application libs §4.1–4.6 are *new* relative to the spec's S-series, which
stopped at runtime primitives. If desired they can be folded into the spec as an
"S4.5 / application stdlib" band.

## 7. Deferred (explicitly out of v1)

- **regex** — not in the spec at all; a full engine is the single largest item.
  Do it only when a concrete consumer appears.
- **TLS / https** — see §5; separate named milestone.
- **gzip / deflate** — pairs with HTTP `Content-Encoding`; pull forward only if
  target APIs require it.
- **http server** — explicitly out of the spec's first net layer.

## 8. Summary

The runtime foundation (collections, io, TCP/UDP, DNS, name-indifferent maps) is
already shipped, so the next libraries are gated by *compute and a little OS
surface*, not by missing primitives. First build Layer 0 (§4.0) — a
registration/dispatch seam so each lib is a `runtime/stdlib_<name>.*` unit rather
than another branch in `vm.cpp`'s two giant `if`-chains. Then build `Json` first
(pure, highest leverage), then the small encodings, then the
random/identifier/time/hash tier that introduces entropy and wall-clock in
isolated steps, then `Url` and a plaintext `net.http` client — with TLS
deliberately deferred to a later, separately scheduled milestone.
