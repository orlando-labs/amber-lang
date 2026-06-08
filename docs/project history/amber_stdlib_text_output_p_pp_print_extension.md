# Amber Stdlib Draft Extension: Text Output, Debug Print and Pretty Print

**Status:** proposed stdlib extension  
**Target base:** Amber v20.3 stdlib planning layer + IO foundation  
**Patch scope:** standard library, prelude/Kernel helpers, IO text writer contracts, debug stringification, pretty printing, iamber output capture profile  
**Non-goals:** new core syntax, raw OS handle exposure, logging framework, terminal styling, binary serialization, notebook rich-display MIME protocol beyond text output

---

# 0. Integration note

This extension defines a minimal but explicit text-output layer for Amber stdlib.

It introduces three user-facing output helpers:

```amber
print value
p value
pp value
```

and their canonical call forms:

```amber
print(value)
p(value)
pp(value)
```

`print` is for ordinary user-facing string/display output.

`p` is for compact debug output, inspired by Ruby's `p`.

`pp` is for structured pretty debug output, primarily useful for collections, maps, nested objects and diagnostic state.

All three write to the current logical stdout by default, but accept an explicit output sink.

In the `iamber` interactive console / notebook profile, writes to the logical stdout/stderr streams are forcibly routed to the active cell output sink so that the complete execution log of a cell can be preserved and inspected independently.

---

# 1. Design principles

## 1.1. Output helpers are stdlib, not syntax

`print`, `p` and `pp` are ordinary stdlib functions exposed through `Kernel` and imported by the default prelude.

The command form:

```amber
p x
pp xs
print "hello"
```

uses Amber's existing command-call syntax.

This extension does not introduce a new language-level print statement.

---

## 1.2. Logical streams, not raw global OS handles

`stdout` and `stderr` are logical streams.

In a CLI process, they normally map to the host process stdout/stderr.

In an embedded environment, test runner, iamber cell, IDE console or sandbox, they may be dynamically rebound.

Therefore this:

```amber
p value
```

means:

```amber
Kernel.p(value, to: io.current_stdout())
```

not:

```amber
Kernel.p(value, to: io.host_stdout())
```

---

## 1.3. Explicit sinks are supported

All output helpers accept an explicit sink:

```amber
print "message", to: io.stderr()
p value, to: io.stderr()
pp config, to: file
```

The sink must implement the text writer protocol or be adaptable to it.

---

## 1.4. iamber must capture logical output

In the `iamber` execution profile, all writes to `io.current_stdout()` and `io.current_stderr()` during cell evaluation are routed to the active cell output sink.

This preserves the cell execution log even if the rendered notebook output is cleared, collapsed or re-ordered by UI.

---

# 2. Module and namespace placement

## 2.1. Canonical placement

Canonical definitions live on `Kernel`:

```amber
Kernel.print(...)
Kernel.p(...)
Kernel.pp(...)
```

The prelude exposes them as bare functions:

```amber
print "hello"
p value
pp value
```

## 2.2. IO module support

The `io` module provides logical stream accessors:

```amber
io.current_stdout()
io.current_stderr()

io.stdout()
io.stderr()
```

Recommended equivalence:

```amber
io.stdout() == io.current_stdout()
io.stderr() == io.current_stderr()
```

The shorter names are ergonomic aliases. The `current_*` names emphasize dynamic rebinding.

Host/raw streams, if exposed at all, must be profile-gated:

```amber
io.host_stdout()
io.host_stderr()
```

These are not available in the safe notebook profile unless explicitly enabled by the host.

---

# 3. Text writer protocol

## 3.1. Minimal protocol

Any object accepted as `to:` by `print`, `p` or `pp` must support:

```amber
writer.write_str(str as Str)
```

Recommended optional operations:

```amber
writer.write_line(str as Str = "")
writer.flush()
writer.closed?()
writer.close()
```

## 3.2. Byte writer adaptation

A byte-oriented `io.Writer` may be adapted into a text writer through UTF-8 encoding:

```amber
text_writer = io.TextWriter.new(byte_writer, encoding: :utf8)
```

If a function receives a byte writer as `to:`, the stdlib may either:

1. reject it with `TypeError`; or
2. adapt it using the default UTF-8 text encoding.

Recommended v1 rule:

> `print`, `p` and `pp` require a text writer. Byte writer adaptation must be explicit.

This avoids hidden encoding policy at output call sites.

## 3.3. Closed writer behavior

Writing to a closed writer raises:

```text
ClosedResourceError
```

If the underlying resource fails, the implementation raises:

```text
IOError
```

or a more specific subtype.

---

# 4. `print`

## 4.1. Purpose

`print` is ordinary user-facing display output.

It converts each argument to display text and writes each argument followed by a newline.

This intentionally differs from Ruby's `print`, which does not append a newline.

Amber's `print` follows Python-like line-oriented behavior.

---

## 4.2. Surface forms

Canonical call form:

```amber
print(value)
print(a, b, c)
print(value, to: io.stderr())
```

Command-call form:

```amber
print value
print a, b, c
print value, to: io.stderr()
```

---

## 4.3. Semantics

```amber
print(a, b, c, to: writer)
```

is observationally equivalent to:

```amber
writer.write_str(Amber.stringify(a, mode: :display))
writer.write_str("\n")

writer.write_str(Amber.stringify(b, mode: :display))
writer.write_str("\n")

writer.write_str(Amber.stringify(c, mode: :display))
writer.write_str("\n")
```

Every argument is printed on its own line.

Examples:

```amber
print "hello"
```

Output:

```text
hello
```

```amber
print "a", "b", 3
```

Output:

```text
a
b
3
```

---

## 4.4. String handling

For `Str`, `print` writes the string contents without debug quotes.

```amber
print "hello"
```

Output:

```text
hello
```

This is different from `p`:

```amber
p "hello"
```

Output:

```text
"hello"
```

---

## 4.5. Return value

`print` returns `null`.

Rationale:

* `print` is an effect-oriented user-output operation.
* Returning the printed value would make `print` too close to `p`.
* Python-like behavior maps naturally to Amber's `null`.

Examples:

```amber
x = print "hello"
# x == null
```

---

## 4.6. Zero arguments

`print()` writes a single newline and returns `null`.

```amber
print()
```

Output:

```text

```

That is equivalent to:

```amber
io.current_stdout().write_str("\n")
```

---

# 5. `p`

## 5.1. Purpose

`p` is compact debug output.

It writes the inspect representation of each argument followed by a newline.

It is intended for fast debugging in scripts, REPL sessions, tests and notebook cells.

---

## 5.2. Surface forms

Canonical call form:

```amber
p(value)
p(a, b, c)
p(value, to: io.stderr())
```

Command-call form:

```amber
p value
p a, b, c
p value, to: io.stderr()
```

---

## 5.3. Semantics

```amber
p(a, b, c, to: writer)
```

is observationally equivalent to:

```amber
writer.write_str(Amber.stringify(a, mode: :inspect))
writer.write_str("\n")

writer.write_str(Amber.stringify(b, mode: :inspect))
writer.write_str("\n")

writer.write_str(Amber.stringify(c, mode: :inspect))
writer.write_str("\n")
```

Examples:

```amber
p "hello"
```

Output:

```text
"hello"
```

```amber
p [1, 2, 3]
```

Output:

```text
[1, 2, 3]
```

---

## 5.4. Return value

For one positional value:

```amber
p(value)
```

returns `value`.

For multiple positional values:

```amber
p(a, b, c)
```

returns:

```amber
Tuple(a, b, c)
```

For zero positional values:

```amber
p()
```

writes nothing and returns `null`.

Rationale:

* single-value `p` is useful in expression pipelines;
* multi-value `p` preserves the argument set without inventing an `Array`;
* zero-value `p` has no natural inspected value.

Examples:

```amber
result = p compute()
# result is the result of compute()

a, b = p left(), right()
# returns Tuple(left_result, right_result), subject to ordinary destructuring rules
```

---

# 6. `pp`

## 6.1. Purpose

`pp` is structured pretty debug output.

It is primarily intended for:

* nested arrays, tuples, sets and maps;
* objects with meaningful field/property state;
* diagnostics;
* AST/HIR/runtime structures;
* notebook and REPL inspection.

`pp` should produce stable, deterministic and readable output.

---

## 6.2. Surface forms

Canonical call form:

```amber
pp(value)
pp(value, max_width: 80)
pp(value, max_depth: 20, max_items: 100)
pp(value, to: io.stderr())
```

Command-call form:

```amber
pp value
pp value, max_width: 100
pp value, to: io.stderr()
```

---

## 6.3. Semantics

```amber
pp(value, to: writer, max_width: 80, max_depth: 20, max_items: 100)
```

creates a pretty-printer configured with the given options, renders `value`, writes the rendered text, then writes a trailing newline.

Conceptually:

```amber
printer = PrettyPrinter.new(
  max_width: max_width,
  max_depth: max_depth,
  max_items: max_items
)

writer.write_str(printer.render(value))
writer.write_str("\n")
```

---

## 6.4. Return value

`pp` follows `p` return semantics.

For one positional value:

```amber
pp(value)
```

returns `value`.

For multiple positional values:

```amber
pp(a, b, c)
```

returns:

```amber
Tuple(a, b, c)
```

For zero positional values:

```amber
pp()
```

writes nothing and returns `null`.

---

## 6.5. Pretty output for collections

Example:

```amber
pp {
  name: "Ada",
  roles: [:admin, :editor],
  settings: {
    theme: "dark",
    retries: 3,
  },
}
```

Possible output:

```text
{
  name: "Ada",
  roles: [
    :admin,
    :editor,
  ],
  settings: {
    theme: "dark",
    retries: 3,
  },
}
```

The exact layout is implementation-defined within the constraints below.

Required constraints:

1. output must be deterministic;
2. output must not contain raw memory addresses;
3. map order must follow the standard `Map` iteration order;
4. set order must follow the standard `Set` iteration order if the implementation defines one;
5. cycles must not recurse forever;
6. infinite or open-ended lazy sequences must not be silently exhausted.

---

# 7. Stringification modes

## 7.1. Canonical entrypoint

The stdlib stringification operation is:

```amber
Amber.stringify(value, mode: mode)
```

where `mode` is one of:

```text
:display
:inspect
:pretty
```

## 7.2. Display mode

Used by:

```amber
print
string interpolation
value.to_str()
```

Display mode is human-facing.

For `Str`, it returns the receiver unchanged.

Examples:

```amber
Amber.stringify("hello", mode: :display)  # "hello"
Amber.stringify(42, mode: :display)       # "42"
Amber.stringify(null, mode: :display)     # "null"
```

---

## 7.3. Inspect mode

Used by:

```amber
p
```

Inspect mode is debug-facing and should make type/structure visible.

Examples:

```amber
Amber.stringify("hello", mode: :inspect)  # "\"hello\""
Amber.stringify(:name, mode: :inspect)    # ":name"
Amber.stringify([1, 2], mode: :inspect)   # "[1, 2]"
```

Resolution order:

```text
1. If value is Str, produce a quoted escaped string literal representation.
2. Else if value responds to inspect(), call it and require Str.
3. Else if value is a known builtin collection/scalar, use the builtin inspect formatter.
4. Else produce deterministic fallback object representation.
```

Fallback object representations must not include raw memory addresses.

---

## 7.4. Pretty mode

Used by:

```amber
pp
```

Pretty mode may be implemented through `PrettyPrinter`.

Resolution order:

```text
1. If value responds to pretty(pp), call it with the active PrettyPrinter.
2. Else if value is a known collection/scalar, use the builtin pretty formatter.
3. Else use inspect mode.
```

`pretty(pp)` writes into the supplied pretty-printer and does not need to return `Str`.

---

# 8. PrettyPrinter protocol

## 8.1. Class

Recommended stdlib class:

```amber
class PrettyPrinter:
  attr var max_width
  attr var max_depth
  attr var max_items
  attr var sort_keys

  def text(str as Str)
  def line()
  def group:
    ...
  def indent:
    ...
  def render(value)
```

The exact internal layout algorithm is implementation-defined.

The public contract is deterministic output under equal input and equal options.

---

## 8.2. User-defined pretty printing

A user type may define:

```amber
def pretty(pp):
  ...
```

Example:

```amber
class User:
  def inspect():
    "#<User name=#{@name.inspect()}>"

  def pretty(pp):
    pp.text("#<User")
    pp.indent:
      pp.line()
      pp.text("name: ")
      pp.render(@name)
      pp.line()
      pp.text("email: ")
      pp.render(@email)
    pp.line()
    pp.text(">")
```

`pretty(pp)` must not assume that it writes to stdout. It only writes into the provided printer.

---

## 8.3. Cycle handling

The pretty-printer must detect recursive structures.

Example:

```amber
xs = []
xs.push(xs)
pp xs
```

Valid output shape:

```text
[
  #<cycle Array>
]
```

The exact cycle marker is implementation-defined but must be deterministic.

---

## 8.4. Depth and item limits

`pp` accepts:

```amber
max_depth:
max_items:
```

Recommended defaults:

```amber
max_depth: 20
max_items: 100
```

If a value exceeds `max_depth`, the printer must emit a deterministic elision marker.

Example:

```text
#<max-depth Array>
```

If a collection exceeds `max_items`, the printer must emit a deterministic truncation marker.

Example:

```text
[
  1,
  2,
  3,
  ... 97 more
]
```

---

## 8.5. Lazy and infinite collections

`pp` must not silently exhaust a lazy or infinite sequence.

Required behavior:

* finite eager collections may be fully printed subject to `max_items`;
* finite lazy collections may be materialized up to `max_items`;
* open-ended or infinite lazy collections must be previewed only up to `max_items`;
* unbounded materialization is invalid.

Example:

```amber
pp (1..).lazy(), max_items: 5
```

Possible output:

```text
#<LazySeq [
  1,
  2,
  3,
  4,
  5,
  ...
]>
```

---

# 9. Sinks and dynamic output contexts

## 9.1. Current output context

The `io` module maintains a dynamic output context.

```amber
io.current_stdout()
io.current_stderr()
```

These are task-local or dynamic-scope-local, depending on the runtime implementation.

They must be safe under Amber's task/strand execution model.

---

## 9.2. Rebinding output

Recommended API:

```amber
io.with_output(stdout: writer, stderr: err_writer):
  ...
```

Example:

```amber
buffer = io.Buffer.new()

io.with_output(stdout: buffer):
  print "hello"
  p [1, 2, 3]

buffer.to_str()
```

Expected buffer content:

```text
hello
[1, 2, 3]
```

---

## 9.3. Explicit sink precedence

An explicit `to:` argument overrides the current stdout default.

```amber
io.with_output(stdout: buffer):
  p value, to: io.stderr()
```

This writes to the current logical stderr, not to `buffer`.

If `io.stderr()` is also dynamically rebound, it writes to that rebound stream.

---

# 10. iamber output capture profile

## 10.1. Cell output sink

The `iamber` profile defines a cell-local sink:

```text
iamber.CellOutputSink
```

It implements the text writer protocol.

It records ordered cell output events.

Recommended event model:

```text
CellOutputEvent(
  stream,
  text,
  timestamp,
  order,
  source_span?
)
```

where:

```text
stream = :stdout | :stderr
```

Future rich-display protocols may add:

```text
stream = :display | :html | :markdown | :json
```

but this extension only defines text output.

---

## 10.2. Cell evaluation rule

During cell evaluation, iamber installs:

```amber
io.with_output(
  stdout: cell_sink.stream(:stdout),
  stderr: cell_sink.stream(:stderr),
):
  eval_cell()
```

Therefore:

```amber
print "hello"
p value
pp data
p warning, to: io.stderr()
```

all enter the active cell log.

---

## 10.3. Forced capture of logical streams

Normative rule:

> In the iamber profile, writes to logical stdout and logical stderr during cell evaluation are captured by the active cell output sink.

This includes:

```amber
io.stdout().write_str("hello\n")
io.stderr().write_str("warning\n")
print "hello"
p value
pp data
```

All of the above are captured.

---

## 10.4. Explicit non-standard resources

Explicit user-created resources are not redirected to the cell log.

Example:

```amber
file = io.File.open("debug.log", mode: :write)
p value, to: file
file.close()
```

This writes to `debug.log`, not to the cell output sink.

---

## 10.5. Raw host streams

If the host exposes raw process streams:

```amber
io.host_stdout()
io.host_stderr()
```

then use of those streams inside iamber is host-policy controlled.

In the safe notebook profile, these APIs should be unavailable or require explicit capability access.

---

## 10.6. Cell log preservation

The iamber runtime must preserve the ordered text log independently from the UI rendering state.

This allows:

* inspecting previous cell output;
* replaying output;
* exporting execution logs;
* debugging hidden/collapsed cells;
* separating stdout and stderr streams.

---

# 11. Privacy, taint and policy

`print`, `p` and `pp` are output/export operations.

If the Privacy/Taint/Lineage profile is enabled, stringification and output must apply the same policy checks as other text-export boundaries.

Examples:

```amber
email as Str @pii

print email
p email
pp {email: email}
```

Depending on the active policy, these may raise:

```text
PolicyViolationError
```

`pp` must not bypass policy checks by recursively inspecting object internals.

If a field is not permitted to be exported, the printer must either:

1. raise `PolicyViolationError`; or
2. emit a policy-approved redaction marker.

The choice is profile-defined.

---

# 12. Error behavior

## 12.1. Invalid sink

If `to:` does not implement the required text writer protocol:

```amber
p value, to: 123
```

raises:

```text
TypeError
```

## 12.2. Closed sink

If the sink is closed:

```amber
file.close()
print "hello", to: file
```

raises:

```text
ClosedResourceError
```

## 12.3. Writer failure

If the sink fails during output, the underlying error propagates as:

```text
IOError
```

or a more specific subtype.

## 12.4. Invalid stringification result

If `to_str()`, `inspect()` or a related hook returns a non-`Str` value, raise:

```text
TypeError
```

## 12.5. Pretty hook failure

If `pretty(pp)` raises, the exception propagates.

Partial writes before the failure are allowed unless the sink provides transactional semantics.

---

# 13. Evaluation order

For:

```amber
p a(), b(), to: sink()
```

evaluation order is:

1. evaluate `a()`;
2. evaluate `b()`;
3. evaluate `sink()`;
4. stringify and write the first value;
5. stringify and write the second value;
6. return `Tuple(a_result, b_result)`.

For:

```amber
print a(), b(), to: sink()
```

evaluation order is the same, but the result is `null`.

For:

```amber
pp value(), max_items: n(), to: sink()
```

evaluation order is:

1. evaluate `value()`;
2. evaluate `n()`;
3. evaluate `sink()`;
4. configure the pretty-printer;
5. render and write;
6. return `value_result`.

---

# 14. Command form

Because Amber supports command-call syntax, these are valid:

```amber
print "hello"
p value
pp config
```

They are equivalent to:

```amber
print("hello")
p(value)
pp(config)
```

With multiple arguments:

```amber
print a, b, c
p a, b, c
pp a, b, c
```

equivalent to:

```amber
print(a, b, c)
p(a, b, c)
pp(a, b, c)
```

With keyword arguments:

```amber
p value, to: io.stderr()
pp value, max_width: 100
print value, to: file
```

equivalent to:

```amber
p(value, to: io.stderr())
pp(value, max_width: 100)
print(value, to: file)
```

---

# 15. Examples

## 15.1. Basic print

```amber
name = "Ada"

print "Hello, #{name}"
```

Output:

```text
Hello, Ada
```

---

## 15.2. Multiple print arguments

```amber
print "alpha", "beta", 42
```

Output:

```text
alpha
beta
42
```

---

## 15.3. Debug print

```amber
p "alpha", [:beta, 42]
```

Output:

```text
"alpha"
[:beta, 42]
```

Return value:

```amber
Tuple("alpha", [:beta, 42])
```

---

## 15.4. Pretty print

```amber
pp {
  user: {
    name: "Ada",
    roles: [:admin, :editor],
  },
  ok: true,
}
```

Output:

```text
{
  user: {
    name: "Ada",
    roles: [
      :admin,
      :editor,
    ],
  },
  ok: true,
}
```

---

## 15.5. stderr

```amber
p error, to: io.stderr()
```

Writes inspect output to the current logical stderr.

In iamber, this appears in the cell log as a stderr event.

---

## 15.6. Capturing output

```amber
buffer = io.Buffer.new()

io.with_output(stdout: buffer):
  print "hello"
  p [1, 2, 3]

buffer.to_str()
```

Result:

```text
hello
[1, 2, 3]
```

---

# 16. Conformance tests

## 16.1. `print`

Required tests:

```amber
print "hello"
# stdout == "hello\n"
# result == null
```

```amber
print "a", "b"
# stdout == "a\nb\n"
# result == null
```

```amber
print()
# stdout == "\n"
# result == null
```

```amber
print "hello", to: io.stderr()
# stderr == "hello\n"
```

## 16.2. `p`

Required tests:

```amber
x = p "hello"
# stdout == "\"hello\"\n"
# x == "hello"
```

```amber
x = p 1, 2
# stdout == "1\n2\n"
# x == Tuple(1, 2)
```

```amber
x = p()
# stdout == ""
# x == null
```

## 16.3. `pp`

Required tests:

```amber
x = pp [1, 2, 3]
# stdout contains structured representation
# x == [1, 2, 3]
```

```amber
xs = []
xs.push(xs)
pp xs
# must terminate
# output contains deterministic cycle marker
```

```amber
pp (1..).lazy(), max_items: 5
# must not exhaust infinite sequence
# output contains deterministic truncation marker
```

## 16.4. Explicit sink

Required tests:

```amber
buffer = io.Buffer.new()
p "x", to: buffer
# buffer.to_str() == "\"x\"\n"
```

## 16.5. Dynamic output

Required tests:

```amber
buffer = io.Buffer.new()

io.with_output(stdout: buffer):
  print "x"
  p "y"

# buffer.to_str() == "x\n\"y\"\n"
```

## 16.6. iamber capture

Required tests:

```amber
# inside iamber cell
print "x"
p "y"
pp [1, 2]
```

Expected:

```text
cell log contains three stdout events in execution order
```

```amber
# inside iamber cell
p "warning", to: io.stderr()
```

Expected:

```text
cell log contains one stderr event
```

## 16.7. Closed sink

Required tests:

```amber
buffer = io.Buffer.new()
buffer.close()

print "x", to: buffer
# raises ClosedResourceError
```

## 16.8. Invalid sink

Required tests:

```amber
p "x", to: 123
# raises TypeError
```

---

# 17. Open questions

## 17.1. Should `print` support separators?

This extension deliberately does not add Python-style `sep:` or `end:`.

Current rule:

```amber
print a, b
```

means:

```text
a
b
```

If needed, a future extension may add:

```amber
print a, b, sep: " ", end: "\n"
```

but the v1 surface keeps each argument line-oriented.

## 17.2. Should `pp` support colors?

Not in this extension.

Terminal color and notebook styling should be a separate display/styling layer.

## 17.3. Should `p` use `inspect()` or `Amber.stringify(..., mode: :inspect)`?

Normative answer:

```amber
p value
```

uses:

```amber
Amber.stringify(value, mode: :inspect)
```

`inspect()` remains the user-definable hook used by that stringification mode.

## 17.4. Should `print` call `to_str()` directly?

Normative answer:

```amber
print value
```

uses:

```amber
Amber.stringify(value, mode: :display)
```

This keeps interpolation, display conversion and print output aligned.

---

# 18. Summary

This extension standardizes:

```amber
print value   # display string, newline after each argument, returns null
p value       # inspect string, newline after each argument, returns value
pp value      # pretty inspect string, newline, returns value
```

Output goes to logical stdout by default:

```amber
io.current_stdout()
```

but can be routed explicitly:

```amber
p value, to: io.stderr()
pp value, to: file
```

The `iamber` profile captures logical stdout/stderr into a cell-local output sink during cell evaluation, preserving a structured execution log.

The design keeps output as a stdlib/runtime concern, composes with IO resource contracts, respects privacy/export policy, and avoids introducing new core syntax.
