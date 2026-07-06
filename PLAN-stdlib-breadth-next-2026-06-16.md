# PLAN — next Amber language-breadth tasks (2026-06-16)

Self-contained plan for a **fresh-context session**. Scope: **further language &
core-stdlib breadth** — the common collection / string / numeric / map methods and
language idioms that are still missing, verified by probing `build/iamber` on
2026-06-16. Read §0 first.

> Out of scope (owned by a parallel session): application stdlib **libraries**
> (Json, Encoding, SecureRandom, Uuid, Time, Digest, Url, net.http). See
> `DESIGN-stdlib-next-libs-order-2026-06-15.md` / memory `stdlib-next-libs-order`.
> Also out of scope: Phase 4 value-repr (prototype done) and Result (done).

---

## 0. Repo state, conventions, gotchas (read first)

**main** is at `b1e5811`. The language surface is already broad: control flow
(`break <value>`, `next` in loops + block-local, `throw`/`catch` + `raise`/`rescue`
that tunnel through iterator blocks), lambdas, block-params (`&blk`, typed-required
`AMB_BLOCK_REQUIRED`), array `min`/`max`/`minmax` (key-block), `Math` namespace,
string methods (length/upcase/downcase/reverse/chars/trim/contains?/starts_with?/
ends_with?/split/replace), short-circuit combinators (take/drop/find/any?/all?/none?/
take_while/drop_while/find_index), conversions (str/int props + to_str/to_int methods,
no to_s/to_i), `;` separator, `Result[T,E]` (Ok/Err/.or/.or_raise/…), and a tagged
16-byte `Value` behind `VALUE_REPR=variant|tagged`. Corpus ~120+ fixtures. `main` is
ahead of `origin/main` (unpushed — user's call).

**A concurrent session is often active in this repo** (stdlib libraries). Expect
uncommitted changes in `runtime/vm.cpp` / `frontend/binder/binder.cpp`. Rebase /
re-pull often; add files explicitly on commit (never `git add -A`); coordinate big
`vm.cpp` edits.

**Build/test:** `make build/iamber` (interpreter); `make test` (units + corpus,
slow — NEVER run two `make` at once); `make backend-equivalence` (VM vs native —
run after object-model/opcode/value changes); probe with `build/iamber --eval`.
**`VALUE_REPR` matters:** anything touching `Value` (e.g. a new tail kind) must build
& pass under BOTH `VALUE_REPR=variant` and `VALUE_REPR=tagged`. Most stdlib-method
work here does NOT touch Value (it dispatches on existing kinds), so it's safe under both.

**Fixtures:** `corpus/run/<name>/{source.am (def probe()), meta.json, expect.run.json}`;
string returns serialize with quotes (`"a:b"` → `value:"\"a:b\""`).

**Conventions:** branch off main; FF-merge back; end commits with
`Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`; commit only when asked.
Add one conformance fixture per feature; keep `make test` + `backend-equivalence` green.

**Gotchas:** methods need `()` (bare = property/bare-nullary); `;` IS a separator
(discouraged); conversions are props (`42.str`) + methods (`42.to_str()`), no
`to_s`/`to_i`; `x as T` asserts (no convert), `T(x)`/`x.cast(T)` convert; `--eval`
doesn't auto-run `main` — use module-level + `def probe()`.

**Design steer learned this session — DO NOT add `_by` variants.** Amber prefers
**key blocks** over `min_by`/`max_by`/`sort_by`: use `xs.min |k|: …`, `xs.max |k|: …`,
`xs.sorted: _1.field`. `sort_by` is intentionally rejected with a corrective
diagnostic. Don't reintroduce the `_by` family.

---

## 1. Tier 1 — core-stdlib methods (high value, mechanical; reuse this session's patterns)

Each is a small addition following an established pattern. Verify each is still
missing first (`build/iamber --eval`), then implement + fixture.

### 1a. Collection methods — in `apply_sequence_set_operation` (`runtime/vm.cpp`)
Pattern: add the selector to the `sequence_extra_operation_selector` set (and to
`block_allowed` if it takes a block), then implement in `apply_sequence_set_operation`
(operates on materialized `items`, serves eager + lazy). This is exactly how
`take`/`drop`/`min`/`max`/`minmax` were added.

Missing (verified 2026-06-16):
- **`join(sep)`** → Str. High value. (stringify each element via the display path,
  join with `sep`.) Pairs with `split`.
- **`sum`** (and `sum(init)`), **`product`** — numeric fold; define behaviour on
  empty (sum→0, product→1) and on non-numeric (TypeError).
- **`each_with_index |v, i|`** — block gets value + index; returns the receiver.
  (NOTE: block arity 2 — check `call_block_to_value` supports 2 args; it does.)
- **`flatten`** / **`flatten(depth)`** — recursively flatten nested lists.
- **`zip(other, …)`** — pairwise combine into tuples/lists; truncate to shortest (decide).
- **`compact`** — drop `null` elements.
- **`partition |pred|`** → `[matching, non_matching]` (two lists).
- **`tally`** → Map of element→count.
- **`last`** / **`last(n)`** — counterpart to the existing `first`/`first(n)`.
- **`count(elem)`** — count equal elements (bare `count` = size already works; the
  1-arg form currently errors "wrong builtin SEND arity" — add the arg form;
  `count |pred|` block form too).
Already present (do NOT re-add): `uniq`, `flat_map`, `each_slice(n)`, `each_cons(n)`,
`first`/`first(n)`, `sorted`, `group`, `min`/`max`/`minmax` (key-block), set ops.

### 1b. String methods — in the `receiver.is_string()` block (`runtime/vm.cpp`, after `+`/`concat`)
Pattern: the block added this session (length/upcase/split/replace/trim/…). Codepoint-
correct where it matters (count at UTF-8 lead bytes); byte-safe substring ops.

#### Naming + mutation convention (Ivan, 2026-06-16)
String transformations come in **pairs**: a **pure** form that returns a new string
(past-participle/adjective: `upcased`, `downcased`, `capitalized`, …) and a
**self-mutating** form with `!` (`upcase!`, `downcase!`, …) — matching the
collection-mutation RFC (`sorted`↔`sort!`, `reversed`↔`reverse!`). **Align the
verb-form methods added earlier** (`upcase`/`downcase`/`reverse`) to the `-ed` pure
names (`upcased`/`downcased`/`reversed`) so the surface is consistent (keep aliases
if back-compat matters, but `-ed` is canonical).

#### Case / transform family — the priority. **Must be Unicode-correct.**
The current `upcase`/`downcase` are ASCII-only (v1) and must be upgraded to use the
**Unicode case table** (decode codepoints — the runtime already has
`decode_keyword_utf8_codepoint` — map lower↔upper via Unicode simple-case-mapping,
re-encode). Pure / mutating pairs:
- `upcased` / `upcase!`
- `downcased` / `downcase!`
- `capitalized` / `capitalize!` (first cased letter upper, rest lower)
- `humanized` / `humanize!` (`"user_id"` → `"User id"`)
- `underscored` / `underscore!` (CamelCase → snake_case)
- `camelized` / `camelize!` (snake_case → CamelCase)
- `titlecased` / `titlecase!`, `swapcased` / `swapcase!`, `dasherized` / `dasherize!` — consider
- `reversed` / `reverse!` (codepoint reverse — `reverse` exists, align)
- `replaced(from, to)` / `replace!(from, to)` — substring replacement, both forms.
  The pure `replace` added this session aligns to `replaced` (per the `-ed`
  convention). **Do NOT add `gsub`/`sub`** (Ruby substitution names) — `replace` /
  `replaced` / `replace!` is the Amber spelling (Ivan: discouraged from Ruby-isms).
- `trimmed` / `trim!`, `lstripped` / `lstrip!`, `rstripped` / `rstrip!`

**Unicode case mapping (sub-task):** add a simple-case-mapping table — generate from
`UnicodeData.txt`, or curate Latin-1 + Cyrillic + Greek for v1. Used by every case
transform so `"straße".upcased`, Cyrillic, Greek, etc. fold correctly.

**Mutable-string design GATE (resolve first):** the `!` forms mutate the receiver in
place, but strings are currently **interned / immutable** (`StringValue` = a
`string_id`). The `!` family therefore requires a mutable string representation (a
heap string buffer distinct from interned literals, or copy-on-write) — a runtime
sub-task that gates ALL string `!` methods. The pure `-ed` forms are straightforward
(intern + return new) and can ship first, independent of this decision.

#### Other string methods (pure, return-new — no mutation pair needed)
- **`*` (repeat)** — `"ab" * 3` → `"ababab"`.
- **`ljust(n[, pad])`** / **`rjust(n[, pad])`** — pad to codepoint width.
- **`index(sub[, from])`** / **`rindex(sub)`** — codepoint offset, or null.
- **`lines`** — split on `\n` (keep/strip terminator — decide).
- **`slice(range)` / `[]`** — codepoint-indexed substring by IntRange / index.
- **`count(sub)`** — non-overlapping occurrence count.
- `bytes` (list of Int byte values); `chars` already exists.

### 1c. Numeric methods — in `try_apply_scalar_send` (`runtime/vm.cpp`, Int/Float receiver)
Pattern: the scalar-send path (where Int/Float selectors dispatch). `Math` namespace
already has free-function forms (sqrt/pow/abs/…); these are the **receiver-method**
forms.

Missing:
- **`abs`** on Int/Float (`(0-5).abs()` → 5; type-preserving). (Math.abs exists; the
  method form does not.)
- **`even?`** / **`odd?`** (Int).
- **`divmod(n)`** → `[quotient, remainder]` (floor semantics, matching `/`//`%`).
- **`gcd(n)`** / **`lcm(n)`** (Int).
- **`clamp(lo, hi)`** (Int/Float).
- **`upto(n) |i|`** / **`downto(n) |i|`** / **`step(to, by) |i|`** — iteration (mirror
  the existing `times`). Returns the receiver.
- Consider: `digits([base])`, `zero?`/`positive?`/`negative?`, `to_str(radix)`.

### 1d. Map methods (`runtime/vm.cpp`)
- **`fetch(key[, default])`** — value or default/raise KeyError.
- **`dig(*keys)`** — nested fetch through Maps/Lists, null on miss.
(`merge` already works; check `invert`, `transform_keys`/`transform_values`, `to_a`,
`min`/`max` key-block — add any missing.)

---

## 2. Tier 2 — language idioms (more involved; parser/binder)

### 2a. Destructuring / multiple assignment — `a, b = expr`
`a, b = [1, 2]` currently → **E1008** ("bare matcher expression"): a comma-LHS isn't
recognized as a destructuring assignment. High-value, common idiom.
- Investigate first: Amber already has **pattern assignment** (`HPatternAssign`,
  `frontend/hir/hir.cpp`) and pattern destructuring in block params / `case`. The
  likely fix is **parser-only**: recognize a comma-separated LHS before `=` and lower
  it to the existing pattern-assign machinery (no new runtime). Also support
  `a, b, *rest = …` if the rest-pattern exists.
- Check interaction with the `;` separator and with tuple/array RHS.

### 2b. Symbol-to-proc — `&:method`
`xs.map(&:upcase)` currently → **AMB_BLOCK_PASS_TARGET** (v1 block-pass accepts only a
bare local). A `&:symbol` in trailing block-pass position should produce a callable
that sends `symbol` to each element. Extends the `&`-forwarding rule (RFC
block-parameters §6.2 explicitly deferred generalized `&Expr`); `&:sym` is the most
common case. Decide whether to special-case `&:sym` or open `&Expr` generally.

### 2c. Nested tuple destructuring in block params — `|x, (a, b)|`
`[[1,[2,3]]].map |x, (a, b)|: …` → **NameError: read of uninitialized local**.
Investigate whether nested destructuring patterns in block params bind correctly
(could be a real bug in `build_block_param_patterns` / the pattern prologue, or a
data-shape issue in the probe). Confirm with a minimal case; fix if a bug.

---

## 3. Tier 3 — fixes & smaller

- **Range step — clarify API + fix (Ivan, 2026-06-16).** The canonical stepped-range
  form is the **literal `(1..10:3)`** (step lives in the range literal), not a
  `.step(n)` method. So: (a) ensure `(1..N:K)` literals parse + iterate correctly;
  (b) per the mutation convention, a `.step` that returns a **new** (re-stepped) range
  is fine as `.step`, but one that **mutates the receiver's step in place must be
  `.step!`** (`!` = self-mutation, collection-mutation RFC). The current
  `(1..10).step(3) |i|: …` crashes (`VMError: class dispatch ref is out of range`) —
  fix the crash and align the method name/semantics to the literal form + the `!` rule.
  (Range `to_a`/`each`/`map` already work.)
- Range numeric helpers: confirm `sum`, `include?`, `min`/`max` on ranges route through
  the collection path.
- Set: confirm the pure/bang op set is complete (the collection-mutation RFC covered
  most); add any common gap.

---

## 4. Suggested order & navigation

**Order:** start with Tier 1a/1b/1c (collection `join`/`sum`/`flatten`/`zip`/`compact`/
`partition`/`each_with_index`/`last`/`count`; string `*`/`capitalize`/`ljust`/`index`/
`lstrip`/`rstrip`/`lines`; numeric `abs`/`even?`/`odd?`/`divmod`/`gcd`/`clamp`/`upto`/
`downto`/`step`) — each is a small, low-risk addition with a fixture, batchable into a
few commits. Then the `range.step` bug (Tier 3). Then the Tier 2 language features
(destructuring assignment is the highest-value of those). Each commit: `make test` +
(for anything touching dispatch broadly) `make backend-equivalence`.

**Key file locations** (grep to confirm — line numbers drift):
- Collection method dispatch: `apply_sequence_set_operation` + the
  `sequence_extra_operation_selector` / `sequence_collection_selector` sets +
  `block_allowed` (all in `runtime/vm.cpp`). Element comparison: `compare_values_for_sort`.
- String methods: the `if (receiver.is_string())` block right after the string
  `+`/`concat` handler in `runtime/vm.cpp`.
- Numeric (Int/Float) methods: `try_apply_scalar_send` in `runtime/vm.cpp`.
- Prelude namespaces (if a new one is ever needed): enum `RuntimeNativeTypeKind`
  (`runtime/vm.h`), `lookup_native_prelude_constant` + dispatch (`runtime/vm.cpp`),
  binder whitelist `is_native_prelude_name` (`frontend/binder/binder.cpp`).
- Pattern assignment / block-param patterns (for Tier 2): `frontend/hir/hir.cpp`
  (`build_block_param_patterns`, `build_block_signature`, `HPatternAssign`), pattern
  compiler `frontend/pattern/pattern.cpp` (E1008 source), parser `frontend/parser/parser.cpp`.
- Block-pass / `&` forwarding (for symbol-to-proc): parser `&`-arg handling +
  `AMB_BLOCK_PASS_TARGET` diagnostic in `frontend/parser/parser.cpp`.
- Stringify (for `join`): the display-stringify path used by interpolation /
  `to_str` in `runtime/vm.cpp`.

**Spec:** add the new methods to the relevant stdlib spec sections (collections,
string, numeric); if any section heading changes, regenerate the anchor map
(`python3 tools/spec_sync.py anchor-map > docs/engineering/spec-anchor-map-v1.md`,
then `python3 tools/spec_sync.py check`).

Related memories: `language-breadth-gaps`, `phased-work-plan`, `stdlib-next-libs-order`
(parallel track), `bare-nullary-rfc-status`.
