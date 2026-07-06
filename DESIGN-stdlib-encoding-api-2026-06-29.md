# DESIGN: Encoding library — module/API surface

Date: 2026-06-29
Status: design / API-shape proposal — no code change in this doc
Scope: the Amber-facing surface of `encoding`, the second library on the Layer 0
stdlib substrate (`DESIGN-stdlib-next-libs-order-2026-06-15.md`, Tier 1, after
`Json`). Defines the charset `Encoding` codec, the `Bytes`↔`Str` bridge, the
`Encoded` staging type for untrusted input (the Ruby `force_encoding`/`encode!`
workflow), encoding detection, the error model, and the runtime shape.
Follows: `amber_unified_final_spec.md` §4.4 + §3.2 (`io.TextWriter`,
`encoding::utf8`), the existing `EncodingError` registry entry, the immutable
UTF-8 `Str` invariant (spec §"String literals are immutable UTF-8 `Str`"), and
the `Bytes`/`ByteBuffer`/`ByteSlice` family in `runtime/io.h`.

This doc is the *API contract* step. It stops short of transcoder internals
(state machines, per-charset lookup tables, the detection classifier) — those are
the implementation, gated on the contract here being agreed.

---

## 1. Where `encoding` sits

`encoding` is a native runtime type, identical in shape to `Json`/`Math`: one
`RuntimeNativeTypeKind::Encoding`, one path registration (`"encoding"`), one
`register_encoding(registry)` adding an `encoding_dispatch(NativeStdlibCall&)`
handler in `runtime/stdlib_encoding.cpp`. No `.am` source ships.

Unlike `Json`, `encoding` introduces a **new heap value type**, `Encoded` (§5),
carried as a `Value` tail kind on both value representations (the same mechanism
`Result` uses). It also introduces an `Encoding` codec value — a constant,
interned, shareable language metaobject (like a frozen `Str`), so `encoding::utf8`
is cheap and strand-shareable.

`encoding` is **pure compute**: deterministic, no OS surface, **no capability
required** (matches the §4.2 roadmap class "pure compute, no new OS surface").
Only `fs.read_text` / `io.TextWriter` wiring touch capabilities, and that wiring
already exists; this library is the charset engine underneath it.

---

## 2. The core decision: text/bytes split, not tagged strings

Amber has already made the decision most encoding APIs agonize over:

- `Str` is **always valid UTF-8, immutable text**. Source is UTF-8; no normalization.
- Raw bytes live in a **separate family**: `Bytes` / `ByteBuffer` / `ByteSlice`.
- `EncodingError` already exists for "invalid runtime string/buffer encoding at a
  boundary."

That is the Python 3 / Rust / Swift model, **not** the Ruby model (a `String` is
`bytes + a mutable encoding tag`). This design keeps `Str` pristine and puts all
charset machinery at the `Bytes`↔`Str` boundary. The consequence that drives the
whole API:

> A "CP1251 `Str`" cannot exist — a `Str` *is* UTF-8. Encoding text to a non-UTF-8
> charset therefore produces **`Bytes`**, not a `Str`.

The design is a **hybrid** (per the 2026-06-29 design ruling): the text/bytes
bridge is the foundation (§4), and the full Ruby workflow — which is *not* niche;
it is the default condition of every byte stream from an untrusted third party —
is supported as a first-class **`Encoded`** staging type (§5) that carries a
*believed* encoding **without poisoning plain `Str`**.

### 2.1 Why a bridge: the best of both worlds

There are two schools for working with encodings, and each is excellent at the
thing the other is bad at:

- **The Ruby school** — a string is `bytes + a mutable encoding tag`. This is
  *ergonomically unbeatable at the messy boundary*: you can carry foreign bytes
  around as if they were a string, re-tag them in O(1) when you learn more, repair
  ill-formed runs, detect, and decide the true encoding *lazily*. That is exactly
  what real third-party input demands. Its cost is that *every* string is now
  potentially invalid — "might not be valid text" becomes an ambient property of
  the whole type, and `CompatibilityError` leaks into code that never asked to care
  about encodings.

- **The Python/Rust/Swift school** — text and bytes are different types; `str` is
  *provably valid* by construction. This is *unbeatable everywhere downstream*:
  comparison, regex, interpolation, JSON, and hashing never defend against invalid
  sequences, and the type is fast and predictable. Its cost is that the messy
  boundary becomes awkward — you lose the "carry and decide later" workflow and end
  up manually juggling a `bytes` value next to a separate encoding variable.

Amber already committed to the second school (`Str` is immutable UTF-8; `Bytes` is
separate). The bridge in this design keeps that strength **and buys back the Ruby
strength precisely where it pays off**, by separating the two concerns into two
types instead of one overloaded one:

- `Str` stays the clean, provably-UTF-8 text type — the 99% of code that is *past*
  the boundary never sees an encoding at all, and never has to.
- `Encoded` (§5) is a dedicated **staging type** at the boundary that resurrects the
  *entire* Ruby workflow — carry, re-tag, detect, repair, commit — without leaking
  that messiness into the rest of the language.

The two worlds meet at exactly one, visible, deliberate point: the
`Encoded → Str` commit (`to_str`). Untrusted bytes get all the lazy,
re-taggable, repairable ergonomics of Ruby while they are `Encoded`; the moment
they cross into `Str` they are clean text with all the downstream guarantees of
the text/bytes split. That single gate is the whole value of the bridge — you pay
the cost of "this might be invalid" *only* on the staging side, and you get it
back as a hard guarantee on the text side.

---

## 3. The two layers

```
              Layer 1: the bridge (know the encoding -> get a Str now)
        encoding::cp1251.decode(bytes)
   Bytes ───────────────────────────────▶ Str   (always valid UTF-8)
         ◀───────────────────────────────
        encoding::cp1251.encode(str)  → Bytes

              Layer 2: Encoded (don't trust it -> stage, inspect, commit)
   Bytes ──tag──▶ Encoded ──force_encoding! / detect / scrub! / valid?──▶ to_str ──▶ Str
```

Rule of thumb: **know the encoding → Layer 1, immediate `Str`. Don't trust it /
need to inspect first → stage in `Encoded`, then commit with `to_str`.**

---

## 4. Layer 1 — the `Encoding` codec + bridge

### 4.1 The `Encoding` value

`Encoding` values are constants reached as `encoding::<name>` or via lookup. They
are immutable, interned, and shareable.

```amber
enc = encoding::cp1251

enc.name                  # "windows-1251"   (canonical)
enc.aliases               # ["cp1251", "windows-1251", "1251"]
enc.ascii_compatible?     # true             (0x00..0x7F decode as US-ASCII)
enc.single_byte?          # true
enc.replacement           # default substitute on :replace (Str for decode side)

enc.decode(bytes)         # Bytes -> Str    (raises EncodingError on malformed input)
enc.encode(str)           # Str -> Bytes    (raises if a char isn't representable)

enc.try_decode(bytes)     # -> Result[Str, EncodingError]
enc.try_encode(str)       # -> Result[Bytes, EncodingError]

enc.valid?(bytes)         # is this byte sequence well-formed in this charset?
enc.representable?(str)    # can this text round-trip losslessly into this charset?
```

`decode`/`encode` accept the shared error policy (§7):

```amber
enc.decode(bytes, on_error: :replace)
enc.encode(str,   on_error: :transliterate)
enc.encode(str,   on_error: :replace, replacement: "_")
```

### 4.2 Module-level surface

```amber
encoding::utf8                 # constant fast-path (and ::utf16le, ::cp1251, ... — §8)
encoding::default              # the process default == encoding::utf8

Encoding.find("cp-1251")       # -> Encoding?   name/alias lookup; case- and separator-insensitive
Encoding.all                   # -> List[Encoding]
Encoding.detect(bytes)         # -> Encoding?   (== bytes.detect_encoding, §6)
Encoding.sniff(bytes)          # -> Detection   (rich result, §6)
```

`Encoding.find` normalizes by lower-casing and stripping non-alphanumerics, so
`"UTF-8"`, `"utf_8"`, and `"utf8"` all resolve to `encoding::utf8`.

### 4.3 `Str` sugar (text → bytes; text → text)

```amber
str.to_bytes                          # == encoding::utf8.encode(str) == the existing Str#bytes
str.to_bytes(encoding::cp1251)        # encode to a charset -> Bytes
str.to_bytes(encoding::cp1251, on_error: :replace)

str.reinterpret(from: encoding::latin1, to: encoding::cp1251)
#   one-shot mis-decode recovery, lossless when `from` is byte-preserving:
#   ≡ encoding::cp1251.decode( encoding::latin1.encode(str) )
```

`Str#valid_encoding?` is deliberately **absent**: an Amber `Str` is valid UTF-8 by
construction, so the predicate would always be `true`. Validity questions live on
`Bytes` / `Encoded`.

### 4.4 `Bytes` sugar (bytes → text; detection)

```amber
bytes.decode                          # assume UTF-8 -> Str
bytes.decode(encoding::cp1251)        # -> Str   (exact inverse of str.to_bytes)
bytes.decode(encoding::utf8, on_error: :replace)
bytes.decode(:detect)                 # detect then decode -> Str  (§6.2)
bytes.try_decode(encoding::cp1251)    # -> Result[Str, EncodingError]

bytes.valid_as?(encoding::utf8)       # -> Bool
bytes.detect_encoding                 # -> Encoding?   (best-effort; §6)
bytes.sniff_encoding                  # -> Detection
bytes.tagged(encoding::cp1251)        # -> Encoded     (stage into Layer 2; §5)
bytes.tagged(:detect)                 # -> Encoded tagged with the detected encoding
```

`str.to_bytes(encoding::utf8)` and `bytes.decode(encoding::utf8)` are exact
inverses; `Str#bytes` / `Bytes#to_str` are their UTF-8 shortcuts.

### 4.5 base64 / hex are **not** here

Binary-transport encodings (base64, base64url, base32, hex) are bytes→ASCII, a
different domain from charsets. They become **`Bytes` methods**, retiring the
`Encoding` name reservation in roadmap §4.2 (noted as an erratum there):

```amber
bytes.base64          bytes.base64url        bytes.hex        bytes.base32
Bytes.from_base64(str)   Bytes.from_hex(str)   ...            # constructors (raise on malformed)
```

---

## 5. Layer 2 — `Encoded`: the untrusted-input staging type

`Encoded` is Amber's equivalent of a Ruby `String` / a byte-string: **raw bytes
carrying a *claimed-but-unverified* charset tag.** It is **mutable**, it **may be
invalid**, and it is where all untrusted-input handling happens. Plain `Str` stays
provably-UTF-8 — the "might not be valid" property is corralled in exactly one
type, never ambient.

### 5.1 Construction

```amber
e = bytes.tagged(encoding::cp1251)        # O(1), no transcode
e = encoding::cp1251.tag(bytes)           # same, codec-first spelling
e = bytes.tagged(:detect)                 # tag with the detected encoding (§6)
e = Encoded.new(bytes, encoding::cp1251)
```

### 5.2 The Ruby workflow — the familiar verbs

```amber
e.encoding                     # the currently-believed Encoding
e.confidence                   # Float? — set when created via :detect, else null
e.bytesize                     # raw byte count

e.force_encoding!(encoding::cp1251)   # O(1) re-tag, NO transcode — bytes untouched
e.with_encoding(encoding::cp1251)     # pure variant -> new Encoded
e.as_encoded!(encoding::cp1251)       # superset of force_encoding!; also accepts :detect ...
e.as_encoded!(:detect)                #    ... run detection, then re-tag

e.valid?                       # do the bytes ACTUALLY conform to the believed encoding?
e.scrub!("?")                  # repair ill-formed sequences in place (default replacement U+FFFD)
e.scrub("?")                   # pure variant -> new Encoded

e.encode!(encoding::utf8)      # transcode bytes in place (believed -> target), re-tag to target
e.encode!(encoding::utf8, on_error: :replace)

e.to_str                       # COMMIT: transcode believed -> UTF-8 Str. The trust boundary.
e.to_str(on_error: :replace)
e.try_to_str                   # -> Result[Str, EncodingError]
e.bytes                        # untag back to raw Bytes
```

`force_encoding!` takes a concrete `Encoding` (it is the O(1) re-tag); passing
`:detect` to it is an error — use `as_encoded!(:detect)`, which is the
detection-aware superset.

The `!` convention holds throughout: mutating forms (`force_encoding!`, `encode!`,
`scrub!`) **preserve the `Encoded` type**; pure forms (`with_encoding`, `scrub`)
return a new `Encoded`; `to_str` is the only operation that crosses into `Str`.

### 5.3 The pipeline

```amber
text =
  res.body_bytes                       # Bytes from an untrusted server
    .tagged(:detect)                   # stage with a best-effort guess
    .scrub!("\u{FFFD}")                # repair anything ill-formed under that guess
    .to_str                            # commit to clean UTF-8 Str
```

### 5.4 Why this beats Ruby for the untrusted case

- **The trust boundary is a visible, unavoidable cast.** Untrusted bytes become a
  `Str` *only* through `to_str`, and that is where strict/replace/scrub is chosen.
  The type system stops you from feeding unverified bytes into code that assumes
  clean text. In Ruby everything is already a `String`, so validity is ambient and
  every downstream consumer must defend itself.
- **No `CompatibilityError` plague.** Concatenation, comparison, regex,
  interpolation, and JSON operate on `Str`, which is provably valid; they never
  handle invalid sequences.
- **`force_encoding!` is honest.** `Encoded` is *defined* as "bytes + believed
  tag," so O(1) re-tagging is total, not a lie about an immutable invariant.

---

## 6. Detection

Detection is heuristic and best-effort. It operates on **bytes**, never on a
decoded `Str`.

### 6.1 The `Detection` result

```amber
d = bytes.sniff_encoding              # -> Detection
d.encoding                            # Encoding?   best candidate, or null if inconclusive
d.confidence                          # Float 0.0..1.0
d.candidates                          # List[(Encoding, Float)]  ranked
```

`Detection` is pattern-matchable (a cold tail value like `Result`):

```amber
case bytes.sniff_encoding
in {encoding: enc, confidence: c} if c >= 0.8 then enc.decode(bytes)
else                                                encoding::utf8.decode(bytes, on_error: :replace)
```

`bytes.detect_encoding` is the convenience returning just `d.encoding`.

Order of evidence: (1) **BOM sniff** — a present BOM wins outright; (2) **UTF-8
well-formedness** — strong signal; (3) a small **statistical classifier** over the
v1 legacy set. v1 scope is honest and limited (§8); the classifier is pluggable
later. **Detection must never gate a security decision** (§9).

### 6.2 The `:detect` shorthand

The first argument of `decode` / `tagged` is polymorphic: an `Encoding` **or** a
detection sentinel. This replaces the buggy `bytes.decode(bytes.detect_encoding)`
(which passes `null` when detection is inconclusive).

```amber
bytes.decode(:detect)                              # detect then decode -> Str
bytes.decode(:detect, fallback: encoding::utf8)    # used when detection is inconclusive (default utf8)
bytes.decode(:detect, fallback: :raise)            # raise EncodingError instead of guessing
bytes.decode(:detect, on_error: :replace)          # composes with the §7 error policy

bytes.tagged(:detect)                              # lazy twin: -> Encoded (inspect .encoding/.confidence
                                                   #   before committing with .to_str)
```

`decode(:detect)` resolves in order: BOM → confident statistical candidate →
`fallback:` (default `encoding::utf8`; `:raise` turns inconclusive into an error).
The `fallback:` rule is the reason this is a real method, not mere sugar — it pins
down the case the naive two-call form leaves undefined.

### 6.3 Sentinel vocabulary

`:detect` is the canonical sentinel everywhere; `:auto` and `:best_effort` are
accepted aliases (so `as_encoded!(:best_effort)` reads as originally proposed).

---

## 7. Error model

One `on_error:` keyword across every decode/encode/commit entry point.
`EncodingError` (existing registry entry, rescuable) carries the failing **byte
offset** (decode) or **codepoint + position** (encode) for actionable messages.

| Policy | decode (malformed bytes) | encode (unrepresentable char) |
|---|---|---|
| `:strict` *(default)* | raise `EncodingError` at offset | raise `EncodingError` |
| `:replace` | emit `U+FFFD` (or `replacement:`) | emit charset substitute / `replacement:` |
| `:ignore` | drop the unit | drop the char |
| `:transliterate` | — | ASCII-fold (`café`→`cafe`), then `:replace` for the rest |

Optional `replacement:` overrides the substitute string. Three escalating tools,
clear roles:

- **`decode`/`encode`/`to_str`** — raise (strict default; fail loud at boundaries).
- **`on_error:`** — in-band recovery when a string is required no matter what.
- **`try_*`** — return `Result[…, EncodingError]` for `.or` / `.or_raise` / match
  (showcases the `Result` type).

---

## 8. Charset coverage (v1)

Hand-written UTF transcoders + tiny 128-entry lookup tables for single-byte legacy
charsets. No external dependency (matches "native-C++ only").

**v1 constants:** `utf8`, `utf16` (BOM-sniff on decode), `utf16le`, `utf16be`,
`utf32`, `utf32le`, `utf32be`, `ascii` (US-ASCII, rejects ≥0x80 strict), `latin1`
(ISO-8859-1), `cp1251` (windows-1251), `cp1252` (windows-1252), `koi8r`, `binary`,
`default` (== `utf8`).

`encoding::binary` is byte-preserving (each byte ↔ U+0000–U+00FF, Latin-1
semantics) — the escape hatch for viewing arbitrary bytes as a `Str`. The actual
binary-data type remains `Bytes`.

**Detection v1 set:** BOM for the UTF family; UTF-8 validity; statistical
discrimination among `latin1` / `cp1251` / `cp1252` / `koi8r` / BOM-less UTF-16.

**Deferred to Phase 2:** CJK multibyte (Shift-JIS, GBK/GB18030, EUC-JP/KR) and the
remaining ISO-8859-* / windows-125x via larger tables; incremental
`enc.decoder()` / `enc.encoder()` for chunk-boundary-safe streaming (pairs with
`io.TextWriter` / `io.text.Reader`); `Str` Unicode `normalize` (NFC/NFD/NFKC/NFKD)
and `transliterate` (both Unicode-data-table-heavy).

---

## 9. Security considerations

- **Strict decode rejects** overlong UTF-8, surrogate code points encoded in UTF-8,
  and noncharacters — overlong/surrogate sequences are classic filter-bypass
  vectors. `valid?` enforces the same rules.
- **Detection is advisory only**: never use a detected encoding (or its confidence)
  to make an authorization or filtering decision. Decode strictly, or commit a
  known charset, before any security-relevant comparison.
- **The `Encoded → Str` commit is the single audit point** for untrusted text:
  reviewers can grep for `to_str` / `decode` to find every place external bytes
  enter the trusted-text domain.

---

## 10. Runtime shape & facade growth

- `RuntimeNativeTypeKind::Encoding` (module) + `Encoding` codec value (interned,
  shareable metaobject) + `Encoded` heap value as a `Value` tail kind on both
  reps (mirrors `Result`).
- `register_encoding(registry)` + `encoding_dispatch(NativeStdlibCall&)` in
  `runtime/stdlib_encoding.cpp`. Charset constants `encoding::<name>` resolve to
  `Encoding` values.
- **GOTCHA:** the native-backend archive source list lives in
  `tools/amberc/main.cpp` (~L5968), separate from `RUNTIME_SRCS` — add
  `stdlib_encoding.cpp` to both.
- **Facade growth is small.** Builds `Str` and `Bytes` (already supported since
  `Json`/io); invokes no user blocks; touches no filesystem. The only new facade
  need is constructing/unwrapping the `Encoded` tail value, which the `Result`
  precedent already covers.

---

## 11. Definition of Done

- Round-trip corpus: for each v1 charset, `encode∘decode` is identity on the
  representable subset; byte-exact fixtures.
- `EncodingError` raised with correct offsets on malformed input across all
  policies; `try_*` returns matchable `Result`.
- `Encoded` lifecycle: tag → `force_encoding!`/`as_encoded!` → `valid?` →
  `scrub!` → `encode!` → `to_str`, green on both value reps + backend-equivalence.
- `bytes.decode(:detect)` / `tagged(:detect)` with all three `fallback:` outcomes.
- Security cases: overlong/surrogate UTF-8 rejected by strict decode and `valid?`.
- net.http `body_encoded` wired to `Encoded` (§ integration), `body_text` ==
  `body_encoded.to_str`.

---

## 12. Open decisions

- **Staging type name.** Working name `Encoded`; alternative `ByteStr` (closer to
  the Ruby "byte string" mental model). Pick before implementation — it pervades
  the surface.
- **`utf16`/`utf32` encode-side BOM policy.** Proposed: BOM-sniff on decode; on
  encode, the explicit-endian forms emit no BOM, the endian-less `utf16`/`utf32`
  emit a BOM (LE). Confirm.
- **`Encoded` slicing.** v1 exposes `.bytes` for byte-level work and omits
  charset-aware slicing on `Encoded` (mid-multibyte hazard). Revisit if the
  net.http streaming bodies want it.
