# DESIGN: Json library — module/API surface

Date: 2026-06-16
Status: design / API-shape proposal — no code change in this doc
Scope: the Amber-facing surface of `Json`, the first library on the Layer 0
stdlib substrate (`DESIGN-stdlib-next-libs-order-2026-06-15.md` §4.1). Defines
the module methods, value method (`.to_json`), streaming-parse hook, JSONL file
I/O, error surface, and the `StdlibHost` facade growth that `Json` forces.
Follows: `amber_unified_final_spec.md` §8 (JSON & external data integration) and
the §4.1 DoD (round-trip corpus, RFC 8259 conformance, pattern-matchable result).

This doc is the *API contract* step. It deliberately stops short of the parser
internals (recursive descent, number/escape edge cases) — those are the
implementation, gated on the contract here being agreed.

---

## 1. Where Json sits

`Json` is a native runtime type, identical in shape to `Math`: one
`RuntimeNativeTypeKind::Json`, one path registration (`"Json"`), one
`register_json(registry)` adding a `json_dispatch(NativeStdlibCall&)` handler in
`runtime/stdlib_json.cpp`. No `.am` source ships; the surface below is all
selector dispatch on the `Json` native type, plus one value method (`.to_json`).

Unlike `Math`, `Json` is *not* a pure-scalar library. It constructs and walks
`Map`/`List`/`Str` heap values, optionally invokes a user block, and optionally
touches the filesystem. That is the real cost of "Json first": **it is the
library that grows the `StdlibHost` facade past Math's scalar-only surface.**
§7 enumerates exactly what it forces. The API in §2–§6 is written so that growth
is additive and each capability maps to one facade method.

---

## 2. Core API (spec §8, doc §4.1)

```amber
Json.parse(text)                       # -> Map | List | Str | Int | Float | Bool | null
Json.parse(text, map: StrictMap)       # exact-key preservation opt-in
Json.generate(value)                   # -> Str (compact, no insignificant whitespace)
Json.pretty_generate(value)            # -> Str (2-space indent, "\n" newlines)
Json.pretty_generate(value, indent: 4) # custom indent width (Int spaces)
value.to_json                          # value method == Json.generate(value)
```

### 2.1 `parse` result model

- Default: ordinary **name-indifferent `Map`** with string keys, *not* globally
  symbolized (spec §8.1, §8.4). So `payload[:user_id]` and `payload["user_id"]`
  both resolve, and `case payload in {user_id: id}` matches a `"user_id"` key
  (spec §8.2). This is the work §4.1 calls "exercises the name-indifferent-map
  work end to end."
- `map: StrictMap` switches object construction to exact-key `StrictMap` (spec
  §8.3): string keys stay `Str`, distinct from any `Symbol`. The keyword takes
  the map *constructor* (composes with custom map impls), not a mode atom; we do
  **not** ship the alternative `keys::strict` spelling (spec §8.3 lists it but
  recommends the constructor form).
- JSON scalars map to: `true`/`false` → `Bool`, `null` → `null`, string → `Str`,
  array → `List`. Numbers: §2.3.
- Top-level value need not be an object/array (RFC 8259 §2): `Json.parse("42")`
  → `Int 42`, `Json.parse("\"hi\"")` → `Str`.

### 2.2 `generate` / `pretty_generate` / `.to_json`

- Accepts the same value universe `parse` produces, plus: `Symbol` keys in a
  `Map` are emitted as their string name (so a hand-built `{user_id: 1}` and a
  parsed `{"user_id": 1}` generate identically); `Set`/`Tuple` are **not** valid
  JSON values → `Json.GenerateError` (see §6). A value that is not
  JSON-representable (closure, native handle, instance object without a
  `.to_json`) faults rather than emitting a lossy string.
- Compact `generate`: minimal separators (`,` and `:`), keys in stored order.
- `pretty_generate`: `indent:` spaces per level (default 2), `": "` after keys,
  `"\n"` between members, no trailing comma, no trailing newline (callers add
  one; `save_to_file` adds it — §4).
- `.to_json` is the value-method spelling. It dispatches to the same generator;
  for instance objects it is the override hook (a user type may define
  `to_json`; we look for it before falling back to the structural generator).
- Float formatting is round-trip shortest (`17`-significant-digit fallback);
  `NaN`/`Infinity` are not valid JSON → fault (RFC 8259 §6).

### 2.3 Number typing (the edge-case work)

- A JSON number with no `.`, no `e`/`E`, that fits `i64` → **`Int`**.
- Anything with a fraction or exponent → **`Float`**.
- An integer literal that overflows `i64`: v1 → `Float` (lossy) **or**
  `Json.ParseError` — see §8 decision D4 (recommend `Float` + a
  `precision: :exact` future hook rather than blocking on BigInt now).
- Generation is the inverse: `Int` → no decimal point; `Float` → shortest
  round-trip with a `.0` when integral-valued (so `1.0` never re-parses as
  `Int`).

---

## 3. Streaming parse with a depth hook (the requested feature)

Large inputs (a multi-MB array of records, an NDJSON export) should not
materialize as one giant `List`. `Json.stream_parse` walks the document and
invokes a block **once per fully-parsed value at a chosen nesting depth**,
discarding each value after the block returns so memory stays bounded to one
record plus the enclosing-frame skeleton.

### 3.1 Surface

```amber
# Count records without holding them all:
n = Json.stream_parse(text, depth: 1) do |record|
  index(record)
end

# Stream from a file (large array or JSONL — see §4):
Json.stream_parse_file("events.json", depth: 1) do |event|
  handle(event)
end
```

- `depth:` (Int, default `1`) — the nesting level whose completed values are
  handed to the block. Depth is defined in §3.2.
- Block arity **1**: the parsed value at that depth. (A 2-arity form
  `|value, path|` giving the JSON Pointer / key path to the value is a proposed
  extension — §8 decision D2.)
- Block is **required**; absent block → `TypeError` (mirrors the collection-SEND
  contract in `vm.cpp`).
- Return value of `stream_parse`: the **count** of values delivered to the
  block. The block's own return value is always ignored; early exit is the
  non-local `Json.stop` (§3.3), not a magic return.

### 3.2 Depth semantics (precise)

Depth is the count of enclosing JSON containers (`{`/`[`) around a value. The
root document is depth 0.

| Document | `depth:` | Block receives |
|---|---|---|
| `[ {...}, {...}, {...} ]` | 1 | each top-level array element |
| `{ "items": [ a, b ] }` | 1 | the value of each top-level object member (`[a,b]` as one `List`) |
| `{ "items": [ a, b ] }` | 2 | `a`, then `b` |
| `42` (scalar root) | 0 | `42` once |
| NDJSON: `{...}\n{...}` | 0 | each line's value (each is a depth-0 document) — see §4.3 |

Rules:
- The block fires when the parser *closes* a value at exactly `depth`. Values
  shallower than `depth` are traversed but never delivered; values deeper are
  built into their parent (which is what gets delivered at `depth`).
- For an object at the hook depth, the *whole object* is delivered (not its
  members) — members are at `depth+1`.
- `depth: 0` delivers the single root value (equivalent to `parse`, but
  streaming the source). For NDJSON it delivers each line.

### 3.3 Early stop (decided: `Json.stop` == implicit throw/catch)

`Json.stop` is a **non-local exit**, not a sentinel return value. Calling
`Json.stop` from inside the block — or anything the block calls — behaves like an
implicit `throw`: it unwinds the block, and `stream_parse` has installed an
implicit `catch` for it. The parser then halts cleanly, delivers no further
values, and `stream_parse` returns the count delivered so far. The block's return
value is never inspected, so a legitimate `false`/`null` payload return is never
mistaken for a control signal, and `Json.stop` composes through helper calls.

This rides the existing throw/catch escape machinery: a block runs in a child Vm,
and a throw that escapes it re-enters the parent through `escaped_throw_`
(`vm.cpp:10010`). `stream_parse` recognizes the unique `Json.stop` throw tag and
converts it to a clean stop; **any other** escaped throw/exception propagates out
of `stream_parse` unchanged (§3.4).

### 3.4 Memory & error model

- **Bounded memory**: once the block returns, the delivered value is dropped by
  the parser; enclosing containers retain only structural depth, not children
  already emitted. (For `depth ≥ 1` over a top array, peak live JSON is one
  record.)
- A parse error mid-stream faults with `Json.ParseError` carrying byte offset /
  line:col, *after* the blocks for already-completed values have run (their work
  is not rolled back — streaming is observably incremental).
- An exception raised *inside the block* propagates out of `stream_parse` using
  the existing block-exception re-entry path (`call_block_to_value` already
  re-raises a child-Vm escape into the parent) — it is not swallowed.

### 3.5 Sources

v1 accepts a `Str` (`stream_parse`) and a path (`stream_parse_file`). A
reader-backed form (`stream_parse(io_reader)`) over the `Reader` contract is a
natural follow-on once the io-facade access is wired (§7), but is **not** v1 —
it pulls scheduler access into the Json handler.

---

## 4. File I/O with JSONL (the requested feature)

```amber
Json.load_from_file(path)                 # -> parsed value (whole document)
Json.load_from_file(path, jsonl: true)    # -> List of per-line values
Json.save_to_file(path, value)            # whole-document generate + trailing "\n"
Json.save_to_file(path, value, pretty: true)         # pretty_generate
Json.save_to_file(path, value, jsonl: true)          # value must be a List/iterable
Json.save_to_file(path, value, indent: 2, jsonl: false)
```

### 4.1 `load_from_file`

- Reads the whole file as UTF-8, then `parse`s it. File-not-found / read error
  surfaces through the existing io fault convention (see §6 / §8 D3).
- `jsonl: true` → parse **one value per non-empty line** (LF or CRLF), return a
  `List` in file order. A blank line is skipped; a malformed line faults
  `Json.ParseError` with the **line number**.

### 4.2 `save_to_file`

- Default: `generate(value)` + single trailing `"\n"`, write atomically
  (write-temp-then-rename if the io layer supports it; otherwise plain write —
  §8 D3). `pretty: true` uses `pretty_generate`; `indent:` forwards.
- `jsonl: true`: `value` must be a `List` (or array-like); each element is
  `generate`d compact onto its own line, `"\n"`-separated, trailing newline.
  A non-list under `jsonl: true` → `Json.GenerateError`. `pretty:` is ignored
  in JSONL mode (each record is one line, by definition) — faulting on
  `pretty: true, jsonl: true` is the stricter alternative (§8 D5).

### 4.3 JSONL detection

Recommendation: **explicit `jsonl:` keyword only**; do *not* auto-detect by
`.jsonl`/`.ndjson` extension (silent mode-switching on a filename is a footgun,
and `load_from_file("x.json")` on an NDJSON file should fail loudly, not guess).
`stream_parse_file` with `jsonl: true` (or `depth: 0` on a newline-delimited
source) is the streaming counterpart for large NDJSON. (Auto-detect is §8 D1 if
preferred.)

---

## 5. Full surface summary

```amber
# --- parse ---
Json.parse(text)                              -> value
Json.parse(text, map: StrictMap)              -> value

# --- generate ---
Json.generate(value)                          -> Str
Json.pretty_generate(value)                   -> Str
Json.pretty_generate(value, indent: Int)      -> Str
value.to_json                                 -> Str

# --- streaming ---
Json.stream_parse(text, depth: Int) { |v| } -> Int   # count delivered
Json.stream_parse_file(path, depth: Int, jsonl: Bool) { |v| } -> Int
Json.stop                                     -> non-local exit; call inside block, stream_parse catches it

# --- files ---
Json.load_from_file(path, jsonl: Bool)        -> value | List
Json.save_to_file(path, value, pretty: Bool, indent: Int, jsonl: Bool)  -> null
```

Selectors owned by the `Json` handler: `parse`, `generate`, `pretty_generate`,
`stream_parse`, `stream_parse_file`, `load_from_file`, `save_to_file`, `stop`.
Anything else on the `Json` native type → `NotHandled` (falls through, then the
normal no-such-method path).

---

## 6. Error surface

New runtime error classes (added to `spec/registries/runtime_errors.def` +
`.yaml`, kept in sync by `make spec-sync-check`, bound in expression position by
the binder via the shared X-macro — same path as the recent error-class work):

- `JsonError` — base (rescuable).
- `Json.ParseError` (`JsonParseError`) — malformed input. Message carries byte
  offset and `line:col`. This is the §4.1 DoD "Json-namespace parse error".
- `Json.GenerateError` (`JsonGenerateError`) — value not representable as JSON
  (`Set`, closure, `NaN`/`Infinity`, non-list under `jsonl:`).

Open question on the namespacing spelling (`Json::ParseError` qualified vs flat
`JsonParseError` registry name) — §8 D6. File-system failures from
`load_from_file`/`save_to_file` reuse the **existing io fault convention** rather
than minting a Json-specific IoError (§8 D3).

---

## 7. Substrate consequence: the `StdlibHost` facade must grow

`Math`'s facade (`stdlib_set_fault`, keyword helpers, `string_value_from_text`)
is scalar-in / scalar-out. `Json` needs more. Each line below is one additive
`StdlibHost` virtual the VM implements; none touch `vm.cpp`-internal types beyond
the already-public `Value`/`MapValue`/`ListValue` in `runtime/vm.h`.

**Value construction (parse builds these):**
- `make_list(items)` — already exists as `make_list_value`; expose via facade.
- `make_string_keyed_map(entries, strict)` — name-indifferent ordinary `Map`
  (string keys, no global symbolization) and the `StrictMap` variant. **This is
  the load-bearing one** and ties directly to D7 below.
- `make_int` / `make_float` / `make_bool` / `make_null` — trivial (`Value::`
  statics already public; wrap for symmetry or call directly).

**Value introspection (generate/`.to_json` walk these):**
- `string_text(value) -> std::string` — resolve a `Str`'s `string_id` back to
  bytes (the inverse of `string_value_from_text`; generation needs it).
- `symbol_name(symbol_id) -> std::string` — for `Map` entries keyed by symbol.
- iterate `MapValue.entries` / `ListValue` — these structs are public, so the
  handler can read them directly; only key→text resolution needs the host.
- `value_to_json_override(value) -> optional<Str>` — invoke a user-defined
  `.to_json` on an instance object, if present (lets `.to_json` be overridable).

**Block invocation (stream hook):**
- `call_stream_block(frame, block, value) -> BlockOutcome` — wraps the existing
  `call_block_to_value`. `BlockOutcome` is one of: **continue** (normal return,
  value ignored), **stop** (the escaped throw carried the unique `Json.stop`
  tag — `stream_parse` halts cleanly), or **propagate** (any other escaped
  throw/exception, re-raised out of `stream_parse`). The host owns the `Json.stop`
  tag identity and the catch, so the handler only branches on the outcome enum.
  This is the first facade method that runs user code; it is what makes
  `stream_parse` more than a parser. Keep it narrow: one value in, one
  control-flow outcome out.

**File access (load/save):**
- `fs_read_text(path)` / `fs_write_text(path, text)` — thin pass-throughs to the
  `RuntimeIoProvider` the VM already holds (`world_options_->io_provider`,
  `fs_read_bytes`/`fs_write_bytes`). Respects the same capability/sandbox checks
  the `Fs` native type uses today.

**ABI note.** This is exactly the "get the `NativeStdlibCall` facade surface
right" risk §4.0 flagged as the only real risk of Layer 0. `Json` is where that
surface is actually exercised, so these additions should be reviewed as *the*
stdlib runtime ABI, not as Json-private helpers — `Encoding` (bytes),
`Digest`/`SecureRandom` (bytes + entropy), and `net.http` (io + scheduler) will
reuse the construction/introspection/io halves. Add them on `StdlibHost`
generically (value building, string/bytes text, block call, fs), named for the
capability, not for Json.

---

## 8. Decisions

Resolved 2026-06-16:
- **Hook depth** — container count, root = 0 (§3.2).
- **Early stop** — `Json.stop` is a non-local exit (implicit throw/catch), not a
  sentinel return (§3.3).
- **D1 — JSONL detection** — explicit `jsonl:` keyword only, no extension
  auto-detect (§4.3).

Still to confirm (these change the API, not the impl):
- **D2 — stream block arity.** Recommend: 1-arity `|value|` for v1; add optional
  `|value, path|` (JSON-Pointer-ish key path) as a follow-on.
- **D3 — file errors.** Recommend: reuse existing io fault convention for
  read/write failures; only parse/generate failures get Json error classes.
  Also: atomic write (temp+rename) vs plain write for `save_to_file`.
- **D4 — i64-overflow integers.** Recommend: parse to `Float` (lossy) in v1,
  reserve a future `precision:`/BigInt path; alternative is fault.
- **D5 — `pretty: true, jsonl: true`.** Recommend: ignore `pretty:` in JSONL
  mode; alternative is fault on the contradiction.
- **D6 — error-class spelling.** `Json.ParseError` qualified names vs flat
  `JsonParseError` registry entries (the registry is currently flat, e.g.
  `KeyError`).
- **D7 — name-indifferent string-keyed map construction. RESOLVED 2026-06-17.**
  Investigation found the ordinary `Map` was *not* actually name-indifferent in
  the runtime (Symbol and Str keys were distinct for lookup/pattern), contradicting
  spec v20.7/v20.8 — so this was fixed first as its own change before `Json`:
  ordinary `Map`/`HashMap` are now name-indifferent and exact-key behavior moved
  to new `StrictMap`/`StrictHashMap`. Mechanism (matches the recommended model):
  each `MapEntry` keeps its original key Value (for keys()/iteration) and carries
  a canonical Symbol identity in `symbol_id` (interned for Str keys) used by
  `[]`/`has_key?`/dedup/`P_PREP_MAP` pattern indexing. So `Json.parse` simply
  builds ordinary string-keyed entries (e.g. via the VM `make_symbol_map_value`
  wrapper, which canonicalizes) and gets `payload[:user_id]`, `payload["user_id"]`,
  and `case … in {user_id:}` all working, with keys preserved as Str (spec §8.4 /
  §8.2). `map: StrictMap` parse opt-in now has a real target type. See the
  Map/StrictMap change for details.

---

## 9. DoD (restating §4.1, with the additions)

- Round-trip corpus: `generate(parse(x)) == canonical(x)` over the RFC 8259
  conformance set incl. malformed inputs → `Json.ParseError`.
- `Json.parse(body)` result drives `case … in {…}` pattern matching (spec §8.2).
- `pretty_generate` stable, diff-friendly output; `indent:` honored.
- `load_from_file`/`save_to_file` round-trip; `jsonl:` round-trips a `List`.
- `stream_parse(depth:)` delivers exactly the §3.2 table; bounded peak memory
  on a large top-level array (assert live-object ceiling); `Json.stop` halts
  cleanly and is caught (count returned), while a non-stop in-block throw/exception
  propagates out of `stream_parse`.
- Backend-equivalence gate green on both value reps (`VALUE_REPR=variant|tagged`).
- Per-lib `tests/stdlib_json_tests.cpp` following the
  `stdlib_collections_tests` / `stdlib_registry_tests` pattern.

---

## 10. Not in v1

- Codec / schema binding (`Json.codec(T)`, spec §3546/§4286) — separate, larger
  effort; this is the structural value layer it will sit on.
- Reader/stream *source* beyond String + file (needs scheduler/io facade).
- BigInt-exact large integers (D4).
- Comments / trailing commas / JSON5 leniency — strict RFC 8259 only.
