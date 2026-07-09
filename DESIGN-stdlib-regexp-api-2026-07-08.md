# DESIGN: Regexp standard-library module and `r` string tag

Date: 2026-07-08
Status: design / API-shape proposal - no code change in this doc
Scope: a native `Regexp` stdlib module, the canonical `r` string-tag macro,
compiled regexp and match values, replacement/splitting/scanning APIs, Unicode
offset semantics, error classes, and the full-native coverage contract.
Follows: `DESIGN-macro-system-2026-06-29.md` string-tag trigger,
`DESIGN-multiline-string-literals-2026-06-29.md` text blocks, the immutable
UTF-8 `Str` invariant from `DESIGN-stdlib-encoding-api-2026-06-29.md`, and the
descriptor-routed native stdlib model in `runtime/stdlib_registry.*`.

This doc is the contract step. It stops short of the matcher VM/JIT details and
native codegen patches, which are implementation work gated on this surface.

## 1. The core shape

`Regexp` is a pure native stdlib module. It owns two immutable runtime values:

- `Regexp.Pattern` - a compiled regexp program plus its canonical source and
  flags.
- `Regexp.Match` - one successful match against a specific source string, with
  full-match and capture ranges.

The ergonomic literal surface is a string-tag macro:

```amber
from Regexp import r

digits = r"""\d+"""
digits.match?("123")      # true
digits.match?("abc")      # false
```

Plain untagged strings are not regexp literals:

```amber
text = """\\d+"""         # ordinary Str, no tag macro expansion
pat = r"""\d+"""          # Regexp.Pattern
```

That distinction is the important one. In this design, "plain" or "untagged"
means no string-tag macro runs. It does not introduce a competing lexer prefix
named `r`.

## 2. Syntax surfaces

### 2.1 Text-block tag

The primary v1 surface is the already-implemented string-tag trigger:

```amber
pat = r"""
  ^\p{L}+@\p{L}+\.\p{L}+$
  """
```

The tag head `r` must be adjacent to the string opener, with no whitespace, just
like `sql"""..."""` and `html"""..."""`.

The untagged text block is an ordinary `Str`. The tagged form is a macro
invocation whose provider is the `Regexp` module.

### 2.2 Single-line tag

Regexps often fit on one line. The regexp design therefore wants the string-tag
trigger generalized from text blocks to any string literal opener:

```amber
from Regexp import r

word = r"\w+"
email = r"^\S+@\S+$"
```

This is not a raw-string prefix. It is the same macro trigger as `r"""..."""`,
applied to a single-line string literal. If the parser extension is staged after
the module, `r"""..."""` remains the first shippable surface and `r"..."` is the
next parser milestone.

### 2.3 Pattern source handed to `r`

The `r` tag consumes the template's authored source chunks after text-block
dedent, not the cooked `Str` value after Amber string escapes. That keeps regexp
escapes readable:

```amber
r"""\d+\s+\w+"""          # pattern source is \d+\s+\w+
```

The string-template AST therefore needs both views for tagged strings:

- `source_chunks`: the dedented literal text exactly as authored, with
  backslashes preserved for the tag.
- `cooked_chunks`: the ordinary string-literal value view for tags that want
  Amber escape handling.

`Regexp.r` uses `source_chunks`. Plain untagged strings keep the ordinary `Str`
lowering.

## 3. Module surface

```amber
import Regexp
from Regexp import Pattern, Match, r

pat = Regexp.compile("\\d+")
pat = Regexp.compile(text, ignore_case: true, multiline: true)

Regexp.escape("a+b?")                # "a\\+b\\?"
Regexp.valid?("\\d+")                # true
Regexp.try_compile("(")              # Result.err(Regexp.CompileError)
```

Module methods:

```amber
Regexp.compile(pattern,
  ignore_case: false,
  multiline: false,
  dot_all: false,
  extended: false,
  ungreedy: false,
  literal: false,
  match_limit: null)                 # -> Regexp.Pattern

Regexp.try_compile(pattern, ...)     # -> Result[Regexp.Pattern, Regexp.CompileError]
Regexp.valid?(pattern, ...)          # -> Bool
Regexp.escape(text)                  # -> Str, literal-safe regexp fragment
Regexp.fragment(pattern)             # -> Regexp.Fragment, explicit syntax opt-in
Regexp.default_match_limit           # -> Int?
Regexp.version                       # -> Map with engine/build metadata
```

`literal: true` compiles `Regexp.escape(pattern)` with the supplied flags.
`match_limit:` overrides the module default for a pattern; `null` uses the
runtime default.

Pattern methods:

```amber
pat.source                           # canonical pattern source
pat.flags                            # [:unicode, :ignore_case, ...]
pat.names                            # named capture names, in pattern order
pat.group_count                      # number of capture groups

pat.match(text, from: 0)             # -> Regexp.Match?    search semantics
pat.match?(text, from: 0)            # -> Bool
pat.full_match(text)                 # -> Regexp.Match?
pat.full_match?(text)                # -> Bool

pat.scan(text)                       # -> List[Regexp.Match]
pat.each_match(text) { |m| ... }     # -> Int count delivered

pat.split(text,
  limit: null,
  keep_empty: false,
  include_captures: false)           # -> List[Str]

pat.replace(text, replacement,
  count: :all)                       # -> Str
pat.replace_first(text, replacement) # -> Str
pat.replace(text) { |m| ... }        # -> Str
```

Match operators:

```amber
text =~ pat                          # -> Regexp.Match?
pat =~ text                          # -> Regexp.Match?
text !~ pat                          # -> Bool
pat !~ text                          # -> Bool
```

Recommended `Str` sugar, once value-method registration is in place:

```amber
text.match(pat)                      # == pat.match(text)
text.match?(pat)
text.scan(pat)
text.split(pat, ...)
text.replace(pat, replacement,
  count: :all)                       # replace matches with a substitution Str
text.replace_first(pat, replacement)
text.replace(pat) { |m| ... }        # block required; block returns substitute
text.replaced(pat, replacement, ...)
text.replaced(pat) { |m| ... }
```

The module methods are the canonical surface; `Str` sugar is convenience only.

## 4. Match operators

Amber should add two regexp-specific infix operators:

- `=~` - search match; returns `Regexp.Match?`.
- `!~` - search non-match; returns `Bool`.

Both operand orders are accepted when exactly one operand is a
`Regexp.Pattern` and the other is a `Str`:

```amber
input =~ r"""^\d+"""                 # r"""^\d+""".match(input)
r"""^\d+""" =~ input                 # same result

input !~ r"""^\d+"""                 # (input =~ r"""^\d+""") == null
r"""^\d+""" !~ input                 # same boolean
```

`=~` deliberately returns the match object, not a boolean, so it composes with
capture extraction while still being naturally truthy/falsey in guards:

```amber
if m = input =~ r"""(?<name>\w+)=(\d+)""":
  config[m[:name]] = Int.parse(m[2])
```

Because Amber already defines `$_` as the last expression result, the terse form
also works when the match expression is the branch condition:

```amber
if input =~ r"""(?<name>\w+)=(\d+)""":
  config[$_[:name]] = Int.parse($_[2])

if r"""(?<name>\w+)=(\d+)""".match(input):
  config[$_[:name]] = Int.parse($_[2])
```

Named binding is still the clearer form when the body is more than a tiny
capture use, or when nested conditionals/calls would make `$_` easy to shadow by
another expression result.

`!~` is the boolean convenience form. It never returns a match value.

The operator always uses `match` search semantics, not `full_match`. Full-string
matching remains explicit through anchors or `.full_match`:

```amber
input =~ r"""^\d+$"""                # anchored search
r"""\d+""".full_match(input)         # explicit full-match method
```

No implicit compile happens for `Str =~ Str` or `Str !~ Str`; that would hide
allocation and diagnostics in an operator. Dynamic patterns must be compiled
explicitly:

```amber
input =~ Regexp.compile(pattern_text)
```

Parser/lowering notes:

- Add lexer tokens for `=~` and `!~`.
- Put both operators at comparison precedence, beside `==`, `<=>`, and `in`
  (currently precedence 4 in `Parser::infix_info`), left-associative.
- Emit ordinary `AstBinary` with `op` `"=~"` / `"!~"` so HIR/native lowering can
  route through the same selector-dispatch machinery as other operators.
- Runtime dispatch accepts only the `(Str, Pattern)` and `(Pattern, Str)` pairs;
  wrong operand shapes raise `TypeError`.

## 5. Match values

`Regexp.Match` is immutable and keeps the matched source `Str` alive so capture
access is stable.

```amber
m = r"""(?<name>\w+)=(\d+)""".match("port=8080")

m.text                 # "port=8080"      group 0
m[0]                   # "port=8080"
m[1]                   # "port"
m[2]                   # "8080"
m["name"]              # "port"
m.group(:name)         # "port"

m.range(0)             # 0...9            codepoint offsets
m.range(:name)         # 0...4
m.byte_range(:name)    # 0...4            UTF-8 byte offsets

m.begin(0)             # 0
m.end(0)               # 9
m.pre_match            # text before group 0
m.post_match           # text after group 0
m.captures             # ["port", "8080"]
m.named_captures       # {name: "port"}
```

Rules:

- Group `0` is always the full match.
- Group access accepts `Int`, `Symbol`, or `Str`.
- An unmatched optional group returns `null`; an empty matched group returns
  `""` with a zero-width range.
- Unknown group names and out-of-range group numbers raise
  `Regexp.GroupError`.
- Public ranges are codepoint offsets, matching `Str#[]`, `slice`, `index`, and
  `length`. Byte ranges exist for interop and engine debugging.

## 6. Safe interpolation

Regexp interpolation has the same security shape as SQL interpolation: authored
static chunks are regexp syntax, but ordinary holes are data.

```amber
prefix = "user+"
pat = r"""^#{prefix}\d+$"""
```

The hole above inserts `Regexp.escape(prefix)`, so the pattern means literal
`user+` followed by digits. A plain string cannot smuggle regexp syntax through
`#{...}`.

Trusted syntax fragments require an explicit branded value:

```amber
frag = Regexp.fragment("\\d+")
pat = r"""^id=#{frag}$"""
```

Surface:

```amber
Regexp.escape(text)                  # -> Str
Regexp.fragment(pattern)             # -> Regexp.Fragment
```

`Regexp.fragment(pattern)` validates the fragment and brands it as regexp
syntax. This is intentionally a loud constructor: ordinary user text should use
`Regexp.escape(text)` or plain `#{text}` interpolation instead.

For a static-only `r"""..."""`, the macro should validate the pattern during
expansion and emit a literal-pattern constructor. For a template with data
holes, the macro emits an internal `Regexp.compile_template(static_chunks,
values, flags)` helper where ordinary values are escaped at runtime and trusted
fragments are inserted as syntax.

## 7. Flags and syntax

Amber regexps are Unicode by default because `Str` is UTF-8 by construction.
The engine always runs in UTF mode and Unicode-property mode.

Flags:

| Flag keyword | Inline | Meaning |
| --- | --- | --- |
| `:ignore_case` | `(?i)` | Unicode case-insensitive matching |
| `:multiline` | `(?m)` | `^` and `$` match line boundaries |
| `:dot_all` | `(?s)` | `.` includes newline |
| `:extended` | `(?x)` | ignore pattern whitespace/comments |
| `:ungreedy` | `(?U)` | invert greedy quantifier default |
| `:unicode` | always on | UTF-8 and Unicode character properties |

Tagged literals use inline flags:

```amber
pat = r"""(?im)^\w+$"""
```

Dynamic compile calls may use keyword flags. There is no suffix syntax such as
`r"""..."""i` in v1; it is parser churn with little payoff.

## 8. Engine contract

The reference implementation should use PCRE2 in UTF + UCP mode, vendored or
declared as a build dependency by the Amber build system. C++ `std::regex` is
not acceptable for the reference engine: its Unicode behavior, diagnostics, and
implementation quality are too inconsistent for a language contract.

The language contract is "Amber regexp syntax is PCRE2-compatible UTF syntax
with the Amber API semantics in this document." That gives the stdlib full
coverage for:

- character classes, Unicode properties, POSIX classes;
- numbered and named captures;
- backreferences;
- lookahead and lookbehind where the engine accepts them;
- atomic groups and possessive quantifiers;
- lazy and greedy quantifiers;
- inline flag scopes;
- anchors and word boundaries.

Backtracking engines can be abused. Every compiled pattern carries match limits:

- `Regexp.default_match_limit` is host-configurable but deterministic inside a
  run.
- `Regexp.compile(..., match_limit: n)` pins a pattern-specific limit.
- Exceeding the limit raises `Regexp.MatchLimitError`.

This keeps the "full engine" choice compatible with service code. A future
`Regexp.Linear` or `re2` package can offer a stricter linear-time subset without
changing this module.

## 9. Replacement semantics

Replacement is available from both sides:

```amber
pat.replace(text, replacement, count: :all)
text.replace(pat, replacement, count: :all)
text.replaced(pat, replacement, count: :all)

pat.replace_first(text, replacement)
text.replace_first(pat, replacement)
```

The `Str` forms are pure methods returning a new string, matching the existing
`Str#replace` / `Str#replaced(from, to)` substring family. When the first
argument is a `Regexp.Pattern`, the method replaces regexp matches; when the
first argument is a `Str`, it keeps the existing substring behavior.
`replaced` is the canonical pure spelling, and `replace` remains the existing
alias.

Replacement strings use `$` references, not backslash references:

```amber
pat.replace("a=1 b=2", "${key}:$1")  # named and numbered captures
"a=1 b=2".replace(pat, "${key}:$1")  # same replacement from the Str side
```

Replacement grammar:

- `$0` is the full match.
- `$1`..`$99` are numbered groups.
- `${name}` is a named group.
- `$$` is a literal dollar sign.
- Any other `$` sequence is `Regexp.ReplacementError`.

Backslash has no replacement magic. This avoids a second escaping language
fighting Amber strings and regexp strings.

Block replacement is preferred for nontrivial cases. This is a different method
clause / argument pattern, not a different verb:

```amber
pat.replace(text) |m|:
  m[:name].upcase()

text.replace(pat) |m|:
  m[:name].upcase()

text.replaced(pat) |m|:
  m[:name].upcase()
```

The block-required clause has a `Regexp.Pattern` argument and no replacement
argument. A call with a pattern and no replacement must either provide a block
or raise `ArgumentError`; it must not silently stringify `null` or do nothing.

In block forms the block receives the `Regexp.Match` and must return a `Str` or
a value accepted by `Amber.stringify` as a display string. Exceptions propagate
normally.

## 10. Splitting and scanning

`scan` returns every non-overlapping match in source order. `each_match` streams
matches to a block and returns the count, avoiding a large list.

Zero-width matches are handled with the standard progress rule: after a
zero-width match, scanning advances by one Unicode codepoint before attempting
the next match. This prevents infinite loops while keeping output
deterministic.

`split` defaults to the useful text-processing behavior:

- leading and trailing empty fields are dropped unless `keep_empty: true`;
- captured separators are omitted unless `include_captures: true`;
- `limit: n` returns at most `n` fields; `null` means no limit.

## 11. Error surface

New rescuable error classes:

```text
RegexpError < Exception
RegexpCompileError < RegexpError
RegexpGroupError < RegexpError
RegexpMatchLimitError < RegexpError
RegexpReplacementError < RegexpError
```

`RegexpCompileError` carries:

- `message`
- `pattern`
- `offset` as a codepoint offset when it can be mapped
- `byte_offset` for engine diagnostics

Diagnostics from static `r"""..."""` literals should point at the literal span
and, when possible, the exact pattern offset inside the text block.

## 12. Native runtime shape

Registration:

- `RuntimeNativeTypeKind::Regexp` for the module object.
- `RuntimeNativeTypeKind::RegexpPattern` and `RegexpMatch` only if type objects
  are exposed as native paths; otherwise pattern/match are tail values with
  module-owned dispatch.
- `register_regexp(registry)` and `register_regexp_runtime_module(...)`, using
  the descriptor route like `Url`, `Yaml`, and `Time`.
- Error descriptors for every class in section 11.

Value representation:

```cpp
struct RuntimeRegexpPatternValue {
  std::string source;
  RegexpFlags flags;
  std::shared_ptr<const CompiledRegexp> compiled;
  std::optional<std::uint64_t> match_limit;
};

struct RuntimeRegexpMatchValue {
  Value source; // keeps the Str alive
  std::shared_ptr<const RuntimeRegexpPatternValue> pattern;
  std::vector<RegexpCaptureRange> captures; // byte + lazy codepoint ranges
};
```

Both are cold tail kinds, like `RuntimeTimeValue`, `RuntimeUuidValue`, and
`RuntimeAstNode`. Equality:

- patterns compare by source and flags;
- matches compare by source value, pattern value, and capture ranges.

Frozen images and bytecode never serialize compiled engine pointers. They store
source, flags, and limits; compiled programs are rebuilt deterministically when
the image is loaded.

## 13. Full-native coverage contract

The regexp module is a pure compute stdlib. A program that uses only regexp
compile/match/scan/split/replace and pure block replacements must be eligible
for full native execution once the full-native backend reaches the text stdlib
slice.

Required native coverage:

1. Native values for `Regexp.Pattern` and `Regexp.Match` in both value
   representations.
2. Direct C++ lowering or descriptor-routed calls for every method in sections
   3, 4, 5, 9, and 10.
3. Native backend support for `r"""..."""` literal-pattern construction without
   VM fallback.
4. Native block ABI coverage for `each_match` and block replacement.
5. A build-time full-native gate: regexp fixtures must fail the build if any
   regexp selector falls back to the VM.
6. Backend-equivalence fixtures comparing VM and native output for captures,
   Unicode offsets, replacements, split edge cases, and match-limit errors.

This is "full native coverage" in Amber's native-backend sense: no bytecode
wrapper and no whole-program VM fallback for the regexp workload.

## 14. Pattern matching integration

Do not make `Regexp.Pattern` a bare structural pattern in v1. Search vs
full-match semantics are too easy to misunderstand.

Use explicit calls:

```amber
case value:
when r"""^\d+$""".full_match?(value):
  ...
```

For search semantics, the operator is fine in an ordinary condition:

```amber
if text =~ r"""error \d+""":
  ...
```

After the matcher protocol is settled for custom values, a future extension can
add a named matcher such as:

```amber
case text:
when Regexp.full(r"""[A-Z]+"""):
  ...
when Regexp.contains(r"""error"""):
  ...
```

The explicit wrapper names keep semantics readable.

## 15. Documentation examples

```amber
from Regexp import r

email = r"""(?i)^\S+@\S+\.\S+$"""

if input =~ email:
  print("ok")
```

```amber
from Regexp import r

pair = r"""(?<key>\w+)=(?<value>\d+)"""
if m = "port=8080" =~ pair:
  config = {m[:key]: Int.parse(m[:value])}
```

```amber
from Regexp import r

csvish = r"""\s*,\s*"""
items = csvish.split("a, b, c")
```

```amber
from Regexp import r

slug = r"""[^\p{L}\p{N}]+"""
out = slug.replace(title.downcase(), "-").trim("-")
```

## 16. Definition of done

1. Parser/macro: `r"""..."""` expands through the existing string-tag path;
   `r"..."` is either implemented or explicitly tracked as the single-line tag
   follow-up.
2. `Ast.StringTemplate` exposes source chunks for tags that need authored text.
3. Static `r` literals validate at compile time and produce source-span
   diagnostics.
4. Lexer/parser support `=~` and `!~` as comparison-precedence `AstBinary`
   operators.
5. `runtime/stdlib_regexp.cpp` registers module paths, handlers, and error
   descriptors through the descriptor model.
6. `Value` supports pattern and match tail kinds in both representations.
7. VM tests cover compile, invalid compile, match/full_match, `=~`, `!~`,
   named captures, optional captures, Unicode codepoint ranges, scan, split,
   replacement, block replacement, and match limits.
8. Macro staging tests cover imported and aliased `r` tags and prove that
   untagged `"""..."""` remains an ordinary `Str`.
9. Native tests assert full native coverage for regexp workloads, including the
   operators.
10. Module docs and sidecar examples are generated for `Regexp`.
