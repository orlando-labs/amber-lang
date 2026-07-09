# DESIGN: Multiline string literals — text blocks for SQL/HTML/templates

Date: 2026-06-29
Status: design / language-extension proposal — no code change in this doc.
Scope: a surface form for long, formatted, multi-line string literals (SQL,
HTML, shell scripts, JSON fixtures, help text) with indentation that follows the
surrounding code but is stripped from the value. Reviews the Ruby squiggly-
heredoc baseline and three alternatives, recommends one, and pins how it reuses
Amber's existing string machinery.
Follows: `amber_unified_final_spec.md` §7 (interpolated string literals, line
~6432), §8.1 (`AstStringLiteral` parts model, line ~6539), §6/§7.4
(`Amber.stringify`, lines ~6398/~6516), invariant "Privacy/Taint profile labels
propagate through string interpolation" (line 6022), and the lexer's
INDENT/DEDENT layout model (lines ~21945, ~22107). Interacts with
`DESIGN-macro-system-2026-06-29.md` (`string_tag macro def` surface) and the
`html`/`sql` template need in `DESIGN-web-framework-dsl-sketch-2026-06-29.md`.

---

## 1. The problem and the one constraint that decides it

Today Amber has exactly one string surface: the double-quoted, single-logical-
line interpolated literal (§7). A SQL query or HTML fragment must be written as

```amber
sql = "SELECT id, name\nFROM #{table}\nWHERE active = #{flag}\n"
```

— `\n`-laden, unreadable, and impossible to diff cleanly. Every language solves
this; the question is *which* solution fits Amber.

**The deciding constraint: Amber is indentation-significant.** The lexer emits
structural `INDENT`/`DEDENT`/`NEWLINE` tokens (§ lexer, ~21945) and blocks are
`INDENT Statement+ DEDENT` (~22107). A multi-line string lives in tension with
this: its body contains newlines and leading whitespace that are *data*, not
layout. Whatever we pick has to tell the lexer "stop doing layout here" cleanly,
and — critically — let the author indent the body to match the surrounding code
while keeping that indentation *out of the value*.

This constraint is the whole reason Ruby's `<<~EOS` is the wrong thing to copy.

## 2. Baseline: Ruby squiggly heredoc — and why it's a poor fit for Amber

```ruby
conn.execute(<<~SQL)
  SELECT *
  FROM #{table_name}
SQL
```

What's nice: the body is indented under the statement and `<<~` strips the
common leading whitespace. What makes it a bad fit for Amber specifically:

1. **The body floats away from its position in the expression.** `<<~SQL` is a
   placeholder token sitting mid-expression (inside `execute(...)`), but the
   actual content begins on the *next* line and the call's `)` closes *before*
   the body. In a NEWLINE-terminated grammar this is hostile: the statement's
   logical line ends, then more lines belong to a token from the previous line.
   Two heredocs on one line (`foo(<<~A, <<~B)`) compounds it. Ruby tolerates
   this because newlines are not significant; Amber's are.
2. **It introduces a second, ad-hoc indentation algorithm** (strip the common
   prefix of body lines) bolted *next to* the real INDENT/DEDENT one, with its
   own edge cases (blank lines, tabs-vs-spaces, the terminator's own indent).
3. **The terminator is a bareword** (`SQL`/`EOS`) that can collide with content
   and has no relationship to the language's block structure.
4. It is **not a clean expression** — you cannot nest it naturally, and tooling
   (formatter, the syntax-faithful AST in §8.1) has to special-case a token
   whose payload lives elsewhere.

We should take the *ergonomic goal* (indent-to-match, strip-on-read,
interpolation) and reject the *mechanism*.

## 3. Alternatives reviewed

### A. Delimited text block `"""…"""` with structural dedent  ← recommended

Borrowed from Java text blocks / Swift multiline strings / Scala 3, adapted to
Amber's interpolation:

```amber
sql = """
  SELECT id, name
  FROM #{table}
  WHERE active = #{flag}
  """
conn.execute(sql)
```

Rules:

- Opens with `"""` which **must be followed by a newline** (the opening line
  carries no content). Closes with `"""` on its own line.
- **Dedent is structural, not "common prefix": the column of the closing `"""`
  defines the strip amount.** Every body line has exactly that many leading
  columns removed; the value above is `"SELECT id, name\nFROM #{table}\nWHERE
  active = #{flag}\n"`. This is the Java "incidental whitespace" rule keyed to
  the closing delimiter — deterministic, and it *harmonizes* with indentation-
  significant code because you indent the block under the statement and place
  `"""` at the code's indent level.
- Inside `"""…"""`, the lexer **suspends layout** (no INDENT/DEDENT/NEWLINE
  emission) until the closing delimiter — the body is one lexical span. This is
  the one lexer change, and it is local and well-bounded by a real token pair,
  unlike the heredoc's float-away terminator.
- **`#{}` interpolation, escapes, and `\#` all work unchanged.** The body is the
  same `StringPart*` stream as §7 — `TextChunk | EscapeSequence | Interpolation`
  — so HIR lowering to `Amber.stringify` (§7.4) is *identical*; only the AST's
  `quote_kind` changes (see §5).

Trade-off: it's still a delimited expression (good — composes everywhere a
string does), but the body is not a layout block, so the lexer must carry one
"inside a text block" mode.

### B. Layout-native block string (reuse INDENT/DEDENT)

The most "Amber-native" idea: don't suspend layout — *use* it. A keyword opens a
real block whose body is captured as text:

```amber
sql = text:
  SELECT id, name
  FROM #{table}
  WHERE active = #{flag}
```

Here the body is an ordinary `INDENT … DEDENT` block; the indent level *is* the
strip column by construction, and the string ends exactly when indentation
returns to the parent — the same rule as every block in the language. **No
second whitespace algorithm, no closing delimiter, no terminator word.** Tagged
variants fall out naturally (`raw:`, `sql:`).

Why it's the runner-up, not the pick:

- **It composes badly as a sub-expression.** `conn.execute(text: …)` wants the
  block on following lines, but a block can't sit inside a paren-call argument
  list mid-line in a layout grammar — you'd nearly always bind to a variable
  first. That regresses exactly the call-site ergonomics Ruby's heredoc nailed.
- **`:` is overloaded.** It already opens def/if/class/`when`/block-suffix
  bodies; `text:` as "string block" is a fourth meaning the parser disambiguates
  by a keyword lookahead — workable but a sharper edge than a dedicated token.
- Worthwhile **as a complement** for the standalone-binding case (assigning a
  big template to a name), but not as the primary because of composition.

### C. Implicit adjacent-literal concatenation (C / Python style)

```amber
sql = "SELECT id, name\n"
      "FROM #{table}\n"
      "WHERE active = #{flag}\n"
```

Rejected: still needs explicit `\n`, still one logical line per physical line
with continuation rules, and scales terribly past a few lines. No dedent story.
Mentioned only to dismiss.

### D. Margin-marker + runtime strip (Scala `.stripMargin`)

```amber
sql = "...\n  |SELECT *\n  |FROM x".stripMargin
```

Rejected: `.stripMargin` is a *library method on an already-built string*, not
sugar; the `|` markers are visual noise and the strip happens at runtime on
every evaluation. Structural dedent (A/B) is compile-time and marker-free.

## 4. Comparison

| Criterion                          | A. `"""` text block | B. layout block | Ruby `<<~` | C. adjacent | D. stripMargin |
|------------------------------------|:------:|:------:|:------:|:------:|:------:|
| Fits NEWLINE-significant grammar   | ✅ (delimited expr) | ✅ (is a block) | ❌ floats away | ⚠️ continuation | ✅ |
| Reuses INDENT/DEDENT vs new algo   | one lexer mode | ✅ reuses it | ❌ 2nd algo | n/a | runtime |
| Indent-to-match + auto-strip       | ✅ closing-col | ✅ by construction | ✅ common-prefix | ❌ | ⚠️ markers |
| Composes as a sub-expression/arg   | ✅ | ❌ bind first | ⚠️ | ✅ | ✅ |
| Reuses §7 interpolation/escapes    | ✅ verbatim | ✅ verbatim | (n/a) | ✅ | ✅ |
| Syntax-faithful AST cost (§8.1)    | low (`quote_kind`) | medium (block→text) | high | low | none (it's a call) |
| Compile-time (no per-eval cost)    | ✅ | ✅ | ✅ | ✅ | ❌ |

## 5. Recommendation

**Adopt A (`"""` text block with closing-delimiter-column dedent) as the
primary surface**, with two variants, and **keep B available as the standalone-
binding complement** (`text:` block) if the layout form proves popular for big
templates. A is the pick because it is a *first-class expression* — it drops in
wherever a string goes, including as a call argument, which is precisely the
ergonomic Ruby's heredoc achieves only by floating the body away from the call.

Variants of A:

1. **Plain / untagged (default):** `"""…"""` — an ordinary `Str`; no string-tag
   macro runs. `#{}` and escapes are active exactly like single-line strings.
2. **Literal / no-escape:** `'''…'''` — no interpolation, no escapes; the bytes
   are literal. For Windows paths, shell with literal `$`, or content that itself
   contains `#{}`. (`\#` in §7.2 already escapes the marker in the interpolating
   form; the literal form is for when *nothing* should be processed.)
3. **Tagged:** `sql"""…"""`, `html"""…"""`, `json"""…"""`, `r"""…"""` — see §7
   and `DESIGN-stdlib-regexp-api-2026-07-08.md`.

AST/HIR impact is deliberately tiny:

- **AST (§8.1):** extend `AstStringLiteral.quote_kind` from `double` to
  `{ double, block, block_raw }`. The `parts` array is unchanged
  (`AstStringText | AstStringEscape | AstStringExpr`). Invariant: the parser
  stays syntax-faithful — the raw body text and the strip column are recorded so
  the form round-trips (formatter / `Ast.to_source`).
- **HIR (invariant #2):** the dedent is applied during lowering, producing the
  same explicit `Amber.stringify`-and-concatenate sequence §7.4 already
  specifies. Downstream stages see an ordinary built string; **no new HIR or
  bytecode** is introduced.

## 6. The taint/privacy angle — where Amber beats the baseline

Spec invariant (line 6022): privacy/taint labels propagate through
interpolation. A `"""…"""` SQL string with `#{user_input}` therefore *already*
carries the taint of its interpolants into the built value — so a
capability/sink that refuses tainted SQL can reject it. Ruby's heredoc offers
nothing here. This is the first way Amber's version is safer, for free.

## 7. Tagged text blocks — the real win, via the macro system

The strongest argument for the `"""` form is that it composes with the proposed
macro system (`DESIGN-macro-system-2026-06-29.md`). A **string-tag macro** is a
macro invoked by a prefix on a text block; it receives the literal's `parts`
(static `TextChunk`s plus the interpolant `Ast`s) **unevaluated**, exactly like
JS tagged templates or Scala `sql"…"` interpolators, and runs at F1.5:

```amber
rows = conn.execute(sql"""
  SELECT id, name
  FROM users
  WHERE active = #{flag}
  """)
```

- `sql"""…"""` expands at compile time to a **parameterized** query: the static
  text becomes the prepared statement `SELECT … WHERE active = $1`, and each
  `#{…}` interpolant becomes a *bound parameter*, not concatenated text. This
  closes SQL injection **structurally** — the interpolant can never become SQL
  syntax — which a raw heredoc fundamentally cannot do.
- `html"""…"""` expands so interpolants are HTML-escaped by default (with an
  opt-out marker for pre-sanitized fragments) — the auto-escaping templating the
  web-DSL sketch needs.
- Because expansion is pre-binder, the result is fully typed and **zero runtime
  cost** beyond the bind itself (macro doc §1 table) — no reflective slow path.

So the layering is: plain `"""` (sugar, ships first, no macro dependency) →
tagged `name"""…"""` (safety + DSLs, ships with / after macros; **implemented
2026-07-06** — `string_tag macro def` + F1.5 adjacency classification, the
literal re-kinded `Ast.StringTemplate` at hand-off). The tag surface
is the fifth macro trigger added to macro-doc §8 ("string-tag": `tag`
immediately followed by the block opener, no whitespace). The hand-off is a
dedicated **`Ast.StringTemplate`** node (see §9 Q4): the same parts *model* the
literal already has — post-dedent static chunks plus unevaluated interpolant
`Ast`s, with the text/raw flag — but a distinct node *kind*, because an
`AstStringLiteral` means "evaluate me to a Str" while a template handed to a
macro is data. F1.5 consumes it; a tagged literal that survives to the binder
is a compile-time diagnostic, like every other macro-only shape.

**Provider contract.** A side package provides a tag by exporting a
`string_tag macro def` binding in the compile-time macro namespace. The tag head
in v1 is an imported macro identifier, not an arbitrary expression; use import
aliases for dialects or competing providers:

```amber
package db.postgres
export macro sql

string_tag macro def sql(t as Ast.StringTemplate) -> Ast:
  Sql.expand(t, dialect: Sql.Postgres)
```

```amber
from db.postgres import sql as psql
from db.sqlite import sql as sqlite_sql

rows = conn.execute(psql"""
  SELECT id, name
  FROM users
  WHERE active = #{flag}
  """)
```

The exact `export macro` spelling should be kept in sync with the macro-doc's
import/export decision, but the semantic rule is fixed here: a string tag is a
macro export, resolved before binding, and ambiguous or missing tag imports are
compile-time diagnostics. Qualified heads such as `pg.sql"""…"""` can be future
sugar; v1 does not need them because import aliases already solve the package
selection problem.

**SQL sanitization mechanics.** The SQL tag is not a prettier
`sanitize_sql_array` and it must not build a raw `Str`. It expands to a typed
query/prepared-statement value whose static chunks remain SQL syntax and whose
interpolants are value slots:

- `#{value}` is always a **bound value parameter**. The macro renders the
  dialect's placeholder form (`$1`, `?`, `@p1`, …) and emits an AST that passes
  the value separately to the driver.
- `#{ids}` may expand to a value-parameter list only when the tag accepts the
  interpolant type as a collection; empty-list behavior is dialect/provider
  policy and must be explicit.
- SQL identifiers are **not** value parameters. Interpolating a table, column, or
  order direction through bare `#{…}` is a compile-time diagnostic. Identifier
  insertion must be explicit and provider typed, e.g. `#{Sql.ident(table_name)}`
  or a schema-derived `Sql.Table` / `Sql.Column` value; the provider validates and
  quotes it per dialect.
- Raw SQL fragments are a separate trusted type (`Sql.Fragment`) with loud
  constructors. Tainted `Str` cannot become SQL syntax just because it appears in
  a tagged template.

That makes dialect packages natural rather than bolted on: `db.postgres` can
choose `$1` placeholders and PostgreSQL identifier quoting, while `db.sqlite` can
choose `?` placeholders, without changing call-site shape.

**HTML/XML sanitization mechanics.** Markup tags escape by type and context, not
by hoping a caller remembered the right helper:

- Plain `Str`/ordinary values in `#{…}` are escaped for the current context
  (text node, attribute value, URL-bearing attribute, etc.). XML packages can
  apply XML-specific escaping and stricter name/namespace rules.
- Already-safe markup is represented by branded provider values such as
  `Html.Safe` or `Xml.Safe`. Emitting one is an explicit opt-out,
  `#{ Html.raw(fragment) }`, and `Html.raw` should accept only trusted literals or
  already-validated safe values.
- Sanitizing untrusted markup is a separate policy operation,
  `Html.sanitize(input, policy) -> Html.Safe`. Escaping protects interpolation;
  sanitization parses and filters markup. They are not the same operation.

### 7.1 The ERB question — output tag vs control-flow tag

The natural follow-on: ERB gives templates *control flow* (`<% items.each %> …
<% end %>`), not just value substitution. Can Amber's text blocks offer the same
feature set with different delimiters? Yes — and the mapping is:

| ERB         | Role                                   | Amber                          |
|-------------|----------------------------------------|--------------------------------|
| `<%= e %>`  | evaluate **and emit** one value        | `#{ e }` — already exact (§7)  |
| `<% s %>`   | evaluate, **emit nothing** (loop/cond) | `%for … :` / `%if … :` (§7.2)  |
| `<%# c %>`  | comment                                | `%# c`                         |
| `<%= raw x %>` | emit unescaped                      | `#{ Html.raw(x) }` opt-out marker |

So `#{…} === <%= … %>` holds today. The only open piece is the `<% … %>`
analog — the region-repeating / region-omitting control flow.

**Decision: control flow does NOT go in the base `"""` literal.** The base form
is a *value* — concatenate static chunks and stringified interpolants left to
right (§7.4). `<% each %>…<% end %>` is categorically different: it repeats or
omits *regions of template text*, i.e. it embeds a statement language inside a
string. Putting that in the base literal turns every string into a mini-program
and breaks the "syntax-faithful parse, HIR just lowers to `Amber.stringify`"
model. The base layer's answer to "where's my loop?" is the JSX answer —
`#{}` already takes any Amber expression:

```amber
"""
<ul>
#{ items.map |i|: "<li>#{i.name}</li>".join("") }
</ul>
"""
```

Iteration = map-to-list-then-`join` (an expression yielding one string);
conditional = `if`-expression. The base literal stays a pure value.

### 7.2 Control flow lives in tagged blocks — three spellings

ERB-class control flow belongs in **tagged** blocks, because `html"""…"""` is a
macro (§7) that receives the parts unevaluated at F1.5 and can compile its own
embedded control syntax down to ordinary Amber AST (a loop/branch over a string
builder) — typed and zero runtime cost. Three candidate spellings:

- **X — ERB-faithful dual delimiters.** `#{ e }` emits, `#%{ s }` (or literal
  `<% s %>`) is control flow with an explicit `end`/close; the region between
  open and close is repeated. Maximally familiar — but it is an *unstructured*
  statement grammar in text: open/`end` pairing is by convention (nesting errors
  surface at expansion time, not structurally), and it inherits ERB's worst
  wart, **whitespace chomping** (`-%>` / `<%-`), because a control line on its
  own line otherwise leaves a stray blank line in the output.

- **Y — structured, layout-native control (recommended).** The control tag
  opens a *real* `INDENT` block of template text, closed by `DEDENT` — the same
  rule as every other Amber block (lexer ~22107):

  ```amber
  html"""
    <ul>
    %for item in items:
      <li>#{item.name}</li>
    %if item.flagged:
      <span>!</span>
    </ul>
    """
  ```

  Strictly better than ERB on two counts: (1) nesting is correct **by
  construction** — it is indentation, not a `<% end %>` you can mismatch; and
  (2) **no whitespace-chomping wart** — a `%for …:` / `%if …:` line is
  *structure*, so by definition it emits neither text nor a newline; only the
  block body produces output. The control block's extra indent level is stripped
  from the output exactly like the base dedent (§3.A), so output stays flush
  regardless of nesting depth.

  **Sigil — `%`, not `#`.** Control directives must *not* reuse `#`: `#` is
  Amber's comment character (§7.2 escapes, line ~6473), so `#for item in items:`
  reads as a commented-out loop and `#(…)` reads as a comment. Emit keeps `#{…}`
  (the brace makes it unmistakably not a comment, and it stays consistent with
  base `"""`), while control takes `%` at the (dedent-relative) line start —
  which (a) collides with nothing in Amber syntax there (`%` is only the mid-line
  modulo operator), (b) is *not* comment-like, (c) has direct precedent in
  eRuby/Rails view trim-mode, where a leading `%` means "this line is code, not
  markup" — exactly the `<% %>` role, and (d) keeps the ERB rhyme already in play
  (`#{}≡<%=%>`, so control inherits the `%` of `<% %>`). Alternatives weighed and
  passed: `@for` (Razor) overloads Amber's instance-variable sigil; `{% for %}`
  (Jinja) is the unstructured open/close form Y replaces, and its `%}` is noise
  on a structural line. A literal leading `%` in body text is escaped `%%`.

- **Z — no embedded statement syntax.** Promote §7.1's map+join to the idiom and
  nest `html"…"` fragments inside `#{}`. Zero new grammar, fully typed, but
  verbose for large markup.

**Recommendation: plain `"""` = Z (expression-only, no control tag); tagged
`html"""…"""` = Y.** Tags: `#{}` (emit), `%for … :` / `%if … :` / `%elif … :` /
`%else:` (structured control), `%# …` (comment), `#{ Html.raw(x) }` (unescaped
emit).
This delivers ERB's full feature set with `#{}≡<%= %>`, a structured `<% %>`
analog spelled `%`, and none of ERB's unstructured-`end` or whitespace warts.

### 7.3 Control flow is a per-tag feature, not universal

Crucially, the control tags belong to *specific* tag macros, **not** to the base
literal and **not** to every tag:

- `html` wants control flow — markup-first reading order with logic sprinkled in
  is the entire reason ERB exists.
- `sql` does **not** — you never loop inside query text; a list becomes a
  parameter expansion `IN ($1, $2, …)`, which the `sql` tag produces from a
  *list interpolation* (`#{ids}` where `ids` is a list), not a `%for`. Loops in
  SQL text are how injection and unbounded queries happen.

So control flow rides the tag that asks for it. The base `"""` and tags like
`sql` stay value-only; only template-shaped tags (`html`, future `text`/`md`)
opt into the `%for`/`%if` surface. Each tag macro declares which control tags it
honors — an unrecognized control tag in a tag that doesn't support it is a
compile-time diagnostic, not silent text.

Implementation note: the base lexer still suspends ordinary Amber layout while
inside a text block (§3.A). A template-shaped tag may parse `%` control lines
from the template body using the body text's relative indentation and then emit
ordinary Amber AST. "Layout-native" here means the tag's template grammar reuses
Amber's indentation discipline; it does not mean third-party packages extend the
core lexer with new `INDENT`/`DEDENT` tokens after parsing.

## 8. Edge cases to pin (for the eventual spec patch)

- **Opening line content:** require the opener `"""` to be immediately followed
  by a newline (reject `"""SELECT` on the opening line) — keeps the dedent rule
  unambiguous. Java does this.
- **Final newline:** the body as written ends with a newline before the closing
  `"""` (so the value ends in `\n`). Provide `"""\` line-continuation at the end
  to suppress the trailing newline (Swift/Java convention) when the caller wants
  no terminator.
- **Closing-delimiter column vs body:** if any non-blank body line is indented
  *less* than the closing `"""`, that's a compile-time diagnostic
  (`AMB_TEXTBLOCK_UNDERINDENT`) rather than a silent partial strip.
- **Tabs vs spaces** in the strip region: follow the existing lexer's tab policy
  for INDENT so there is exactly one whitespace model; mismatched leading
  whitespace is a diagnostic, not a guess.
- **Blank lines** inside the block contribute `\n` only (no spurious trailing
  spaces); trailing whitespace on each body line is stripped (Java rule) unless
  in the raw `'''` form.
- **`#{}` spanning lines:** an interpolation expression may contain newlines
  (consistent with §7's "newlines do not terminate the expression inside
  interpolation").

## 9. Open questions

1. **Literal/no-escape spelling:** settled as dedicated `'''…'''`, mirroring
   `"""`. Do not reserve `r"""…"""` as a lexer-level raw-string prefix; `r` is
   the canonical regexp string-tag macro (`r"""…"""`), and plain untagged
   `"""…"""` remains an ordinary `Str` with no macro expansion.
2. **Ship B (`text:`) at all,** or only A? B's only unique value is the
   standalone big-template binding; if A's ergonomics there are fine, drop B to
   keep one surface.
3. **Trailing-newline default:** keep-it (Java) vs strip-it (some templating
   langs). Lean: keep it, with `"""\` to suppress, because SQL/shell usually
   want the terminator.
4. **String-tag trigger:** *settled.* Use a distinct `string_tag macro def`
   trigger, not `name!`; the payload is `Ast.StringTemplate`, not a call's args.
   Side packages export these macro bindings, and consumers import/alias them
   like other static module names (`from db.postgres import sql as psql`).
5. **Bytes text blocks** (`b"""…"""`) for binary/wire fixtures — reserve the
   prefix grammar now or defer.
6. **Control-tag surface** (§7.2-Y): *settled.* Emit `#{ expr }`; control `%for
   … :` / `%if … :` / `%elif … :` / `%else:` (`%` at the dedent-relative line
   start, opening a structured INDENT block); comment `%# …`; raw emit
   `#{ Html.raw(x) }`; a literal leading `%` in body text escapes as `%%`. All
   control keywords carry the trailing `:` uniformly (`%else:` included).
   `#`-prefixed control was rejected as comment-like (`#` is the comment char).
7. **Output indentation under control blocks** (§7.2-Y): confirm the dedent rule
   composes — output indent = line indent − base column − Σ(enclosing control-
   block indent levels). Needs a golden corpus (nested `%for`/`%if`, blank
   lines inside loop bodies, the trailing-newline rule §8 per iteration).
8. **HTML/XML safe-fragment spelling:** *settled.* Provider-qualified marker
   `#{ Html.raw(x) }` / `#{ Xml.raw(x) }`, returning branded safe fragments
   (`Html.Safe` / `Xml.Safe`). Sanitizing untrusted markup is an explicit policy
   operation (`Html.sanitize(input, policy) -> Html.Safe`), not the raw marker.

## 10. Definition of done (for the implementation that follows)

1. Lexer: `"""` / `'''` open a layout-suspended span to the matching
   delimiter; opener-newline and under-indent diagnostics fire.
2. Dedent by closing-delimiter column applied in HIR lowering; golden corpus for
   the whitespace edge cases in §8.
3. AST `quote_kind` extension round-trips (syntax-faithful; formatter restores
   the block form); `parts` model unchanged.
4. Interpolation, escapes, `\#`, taint propagation all verified identical to §7
   on the new form (shared lowering path).
5. Both value reps and the backend-equivalence gate green (the lowering is
   string-build only, so this should be mechanical).
6. (With macros) `string_tag macro def` provider corpus: imported/aliased
   `sql"""…"""` parameterization, dialect-specific placeholder rendering,
   identifier-vs-value diagnostics, and `html"""…"""` / `xml"""…"""` escaping
   with branded safe-fragment opt-outs, proving the tag receives
   static-vs-interpolant parts and the SQL form admits no injection.
