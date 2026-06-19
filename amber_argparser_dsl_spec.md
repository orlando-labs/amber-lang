# Amber ArgParser DSL — draft specification

**Status:** draft / library-facing language design note  
**Scope:** standard-library `ArgParser` DSL for Amber command-line interfaces  
**Primary goal:** concise Ruby-like / Amber-like CLI parser syntax with local validation blocks, structured parse errors, and canonical Unix-style terminal diagnostics.

---

## 1. Design goals

`ArgParser` provides a standard Amber DSL for describing command-line interfaces.

The design goals are:

- keep CLI declarations readable in method-chaining style;
- use Amber block suffix syntax for local per-argument transformation and validation;
- distinguish programmer errors from user CLI errors;
- produce canonical Unix-style `usage:` and `program: error:` diagnostics;
- avoid stack traces for ordinary user input failures;
- preserve a strict mode for tests, libraries, and embedded use;
- support options with values, boolean flags, positional arguments, and rest positional arguments;
- keep the default simple enough for one-file scripts.

---

## 2. Canonical surface syntax

The canonical form is a chained `ArgParser()` construction followed by argument declarations and `.parse()`.

```amber
args = ArgParser()
 .name("copy")
 .about("Copy files")
 .arg("-f", "--from",
   name: :from,
   desc: "Source path",
   type: Path,
   required: true
 )
 .arg("-t", "--to",
   name: :to,
   desc: "Destination path",
   type: Path,
   required: true
 )
 .flag("-v", "--verbose",
   desc: "Print detailed progress"
 )
 .parse()
```

The parsed result is accessed by canonical key lookup:

```amber
args[:from]
args[:to]
args[:verbose]
```

Property-style access may be supported as convenience for non-conflicting names, but `args[:name]` is the normative access form.

---

## 3. Constructor

```amber
def init(cmdline: Process.cmdline, name: null, env: Process.env): ...
```

### 3.1. Default command line

`ArgParser()` defaults to the command line given to the current process.

```amber
args = ArgParser()
 .arg("-o", "--output", type: Path)
 .parse()
```

### 3.2. Explicit command line

An explicit `cmdline:` is used for tests and embedding.

```amber
args = ArgParser(cmdline: ["--output", "out.txt"])
 .arg("-o", "--output", type: Path)
 .parse!()
```

---

## 4. Option spelling

Option spellings are strings.

```amber
.arg("-f", "--from", ...)
.flag("-v", "--verbose", ...)
```

Symbols such as `:f` and `:from` are not the canonical spelling for options.

Rationale:

- strings preserve the actual CLI surface form;
- dashed names such as `--dry-run` and `--input-file` are natural strings;
- diagnostics can quote the exact user-facing option;
- no hidden rule is needed to synthesize leading dashes.

### 4.1. Validation of spelling

A short option must match:

```text
-x
```

A long option must match:

```text
--name
--name-with-dashes
```

Invalid option spellings are programmer errors and should raise ordinary API exceptions during parser construction, not `ArgParser.ParseError`.

---

## 5. Name inference

If `name:` is omitted, the name is inferred from the long option when available.

```amber
.arg("-o", "--output-file")
# name => :output_file
```

Inference rules:

```text
--output       => :output
--output-file  => :output_file
-v             => :v, only if no long form exists
```

If the inferred name would conflict with a reserved word, be ambiguous, or be undesirable, the user writes `name:` explicitly.

```amber
.arg("-f", "--from", name: :source, type: Path)
```

The canonical result lookup remains:

```amber
args[:source]
```

---

## 6. `.arg` — option with value

`.arg` declares an option that consumes a value.

```amber
def arg(short_or_long, long = null,
  name: null,
  desc: "",
  type: Str,
  default: null,
  required: false,
  choices: null,
  multiple: false,
  metavar: null,
  env: null
): ...
```

Examples:

```amber
.arg("-n", "--count",
  desc: "Repeat count",
  type: Int,
  default: 1
)
```

```amber
.arg("--config",
  desc: "Config file",
  type: Path,
  required: true
)
```

---

## 7. Local `.arg` block

A local block may be attached directly to `.arg`.

This block is the canonical place for per-argument transformation and validation.

```amber
args = ArgParser()
 .arg("-p", "--port",
   desc: "Port number",
   type: Int,
   default: 8080,
   metavar: "PORT"
 ) |port, arg|:
   if port < 1 or port > 65535:
    arg.invalid("must be between 1 and 65535")
   port
 .parse()
```

The block belongs to the nearest `.arg(...)` call.

### 7.1. Block parameters

The canonical block arities are:

```amber
|value|:
```

and:

```amber
|value, arg|:
```

`value` is the current parsed value after built-in type conversion.

`arg` is an `ArgParser.ArgContext` object for the currently declared argument.

### 7.2. Transformation pipeline

For explicit command-line input, the pipeline is:

```text
raw CLI token -> type conversion -> local arg block -> validation -> stored value
```

For default values, the same value-shaping pipeline is applied unless the implementation explicitly marks a default as already-final.

The recommended default behavior is to apply the same type and block path to defaults so that stored values are consistent.

### 7.3. Returning from the block

The block result becomes the stored argument value.

```amber
.arg("-s", "--size", type: Str, default: "1mb") |text|:
 Size.parse(text)
```

If the block only validates and does not transform, it should return the original value.

```amber
.arg("-p", "--port", type: Int) |port, arg|:
 if port < 1 or port > 65535:
  arg.invalid("must be between 1 and 65535")
 port
```

---

## 8. `ArgContext`

`ArgParser.ArgContext` is passed to local `.arg` blocks when the block accepts a second parameter.

It exposes user-facing diagnostics helpers.

```amber
class ArgParser:
 class ArgContext:
  def invalid(message, value: null): ...
  def missing(message = null): ...
  def reject(message, value: null): ...
```

These helpers raise `ArgParser.ParseError` subclasses.

Example:

```amber
.arg("-p", "--port", type: Int) |port, arg|:
 if port < 1 or port > 65535:
  arg.invalid("must be between 1 and 65535")
 port
```

Equivalent explicit form:

```amber
.arg("-p", "--port", type: Int) |port|:
 if port < 1 or port > 65535:
  raise ArgParser.InvalidValue(
    "must be between 1 and 65535",
    option: "--port",
    value: port
  )
 port
```

The helper form is preferred because it keeps option metadata local and avoids duplicate spelling.

---

## 9. Boolean flags

`.flag` declares a boolean option.

```amber
def flag(short_or_long, long = null,
  name: null,
  desc: "",
  default: false,
  negatable: false
): ...
```

Example:

```amber
.flag("-v", "--verbose",
  desc: "Enable verbose output"
)
```

The result is:

```amber
args[:verbose] # Bool
```

### 9.1. Negatable flags

```amber
.flag(null, "--color",
  desc: "Colorize output",
  default: true,
  negatable: true
)
```

Accepted forms:

```text
--color     => true
--no-color  => false
```

A negatable flag should not be declared with a short-only spelling.

---

## 10. Positional arguments

`.pos` declares a positional argument.

```amber
def pos(name,
  desc: "",
  type: Str,
  default: null,
  required: true,
  multiple: false,
  metavar: null
): ...
```

Example:

```amber
args = ArgParser()
 .pos("source", type: Path, desc: "Source file")
 .pos("target", type: Path, desc: "Target file")
 .parse()
```

Result:

```amber
args[:source]
args[:target]
```

---

## 11. Rest positional arguments

`.rest` declares a rest positional argument consuming all remaining positional values.

```amber
def rest(name,
  desc: "",
  type: Str,
  metavar: null
): ...
```

Example:

```amber
args = ArgParser()
 .rest("files", type: Path, desc: "Files to process")
 .parse()
```

Result:

```amber
args[:files] # Array[Path]
```

At most one `.rest` argument may be declared.

---

## 12. Multiple values

`.arg(..., multiple: true)` allows the same option to appear multiple times.

```amber
args = ArgParser()
 .arg("-I", "--include",
   name: :includes,
   type: Path,
   multiple: true,
   default: []
 )
 .parse()
```

CLI:

```text
mytool -I src -I lib --include vendor
```

Result:

```amber
args[:includes] # [Path("src"), Path("lib"), Path("vendor")]
```

For `multiple: true`, the local `.arg` block receives each converted occurrence before aggregation unless the argument declaration explicitly opts into aggregate validation.

The default rule is per-occurrence transformation.

---

## 13. Choices

`choices:` restricts accepted values.

```amber
args = ArgParser()
 .arg("-m", "--mode",
   type: Symbol,
   choices: [:dev, :test, :prod],
   default: :dev
 )
 .parse()
```

CLI:

```text
--mode prod
```

Result:

```amber
args[:mode] # :prod
```

If the value is not in `choices`, parsing raises `ArgParser.InvalidChoice` internally and `.parse()` renders a CLI diagnostic.

---

## 14. Parse result object

`.parse()` returns an `ArgParser.Args` object.

The normative access form is:

```amber
args[:name]
```

Recommended API:

```amber
args.has?(:name)
args.fetch(:name)
args.fetch(:name, default)
args.to_map()
```

The result object should be immutable after parse completion.

---

## 15. Parse error hierarchy

`ArgParser` defines a recoverable CLI parse error family.

```amber
class ArgParser:
 class ParseError < Exception:
  def init(@message, @option: null, @value: null, @exit_code: 2):
   pass

 class MissingRequired < ParseError:
  pass

 class UnknownOption < ParseError:
  pass

 class InvalidValue < ParseError:
  pass

 class InvalidChoice < ParseError:
  pass
```

Only `ArgParser.ParseError` and its subclasses are considered ordinary user CLI errors.

Other exceptions are programmer errors, runtime errors, or application errors and must not be swallowed by default.

---

## 16. `.parse`, `.parse!`, and `.try_parse`

The parser exposes three parse modes.

### 16.1. `.parse()`

`.parse()` is the default CLI-entrypoint mode.

```amber
args = parser.parse()
```

Behavior:

- parses the command line;
- catches `ArgParser.ParseError` only;
- prints usage and error diagnostics to `stderr`;
- exits with `error.exit_code`, default `2`;
- does not print a stack trace for parse errors.

Equivalent conceptual lowering:

```amber
def parse(cmdline: null):
 try:
  parse_internal(cmdline or @cmdline)
 rescue ArgParser.ParseError |e|:
  render_error(e, stderr)
  Process.exit(e.exit_code)
```

### 16.2. `.parse!()`

`.parse!()` is strict mode.

```amber
args = parser.parse!(cmdline: ["--port", "99999"])
```

Behavior:

- parses the command line;
- raises `ArgParser.ParseError` on CLI input errors;
- never catches parse errors internally;
- useful for tests, libraries, embedding, and custom error handling.

Example:

```amber
try:
 parser.parse!(cmdline: ["--port", "99999"])
rescue ArgParser.InvalidValue |e|:
 assert e.option == "--port"
```

### 16.3. `.try_parse()`

`.try_parse()` is result mode.

```amber
result = parser.try_parse(cmdline: ["--port", "99999"])
```

It returns either:

```amber
ArgParser.Ok(args: args)
```

or:

```amber
ArgParser.Err(error: error)
```

This mode is optional for the minimal profile but recommended for embedding.

---

## 17. Error handling contract

The central rule is:

```text
ArgParser.parse() catches only ArgParser.ParseError.
```

It must not catch arbitrary `Exception`.

Valid parse error from a local `.arg` block:

```amber
.arg("-p", "--port", type: Int) |port, arg|:
 if port < 1 or port > 65535:
  arg.invalid("must be between 1 and 65535")
 port
```

Application error that must not be swallowed:

```amber
.arg("--config", type: Path) |path|:
 db.connect(path) # DBError must propagate normally
 path
```

This preserves the distinction between user input errors and real program failures.

---

## 18. Canonical Unix-style diagnostics

On parse failure, `.parse()` writes diagnostics to `stderr`.

The canonical form is:

```text
usage: PROGRAM [OPTIONS] [ARGS]

PROGRAM: error: MESSAGE
```

For option-specific errors:

```text
usage: server [-h] [-p PORT]

server: error: --port: must be between 1 and 65535
```

For missing required options:

```text
usage: copy [-h] -f FROM -t TO [-v]

copy: error: missing required option --from
```

For unknown options:

```text
usage: copy [-h] -f FROM -t TO [-v]

copy: error: unknown option --fro
```

For invalid choices:

```text
usage: tool [-h] [-m MODE]

tool: error: --mode: expected one of dev, test, prod; got staging
```

Default exit code for parse errors is `2`.

---

## 19. Help behavior

By default, `ArgParser` provides `-h` / `--help` unless disabled.

```text
usage: copy [-h] -f FROM -t TO [-v]

Copy files

options:
  -h, --help         show this help message and exit
  -f, --from FROM    Source path
  -t, --to TO        Destination path
  -v, --verbose      Print detailed progress
```

Help is not an error.

Recommended behavior:

- print help to `stdout`;
- exit with code `0`;
- do not return an `Args` object.

In strict mode, help may raise a distinct `ArgParser.HelpRequested` control exception or return a structured result, depending on the embedding profile.

---

## 20. Type conversion

`type:` accepts a type object or callable converter.

Examples:

```amber
.arg("-n", "--count", type: Int)
.arg("--path", type: Path)
.arg("--mode", type: Symbol)
```

Type conversion failure raises `ArgParser.InvalidValue` internally.

Example diagnostic:

```text
server: error: --port: expected Int; got "abc"
```

If `type:` is a callable, it receives the raw string token and returns the converted value.

```amber
.arg("--size", type: &Size.parse)
```

A local `.arg` block may still be attached after callable conversion.

---

## 21. Required arguments

An option with `required: true` must be provided unless a value is available from an accepted alternate source such as `env:`.

```amber
.arg("-f", "--from", type: Path, required: true)
```

If missing, parsing raises `ArgParser.MissingRequired` internally.

---

## 22. Environment fallback

`env:` declares an environment-variable fallback.

```amber
.arg("--token",
  desc: "API token",
  type: Str,
  env: "API_TOKEN",
  required: true
)
```

Precedence:

```text
explicit CLI value > environment fallback > default value > missing
```

Environment values pass through the same type conversion and local `.arg` block pipeline.

---

## 23. End-of-options marker

The token `--` ends option parsing.

Example:

```text
tool --verbose -- --not-an-option
```

After `--`, remaining tokens are positional values even if they start with `-`.

---

## 24. Recommended complete example

```amber
args = ArgParser()
 .name("server")
 .about("Run development server")
 .arg("-p", "--port",
   desc: "Port number",
   type: Int,
   default: 8080,
   metavar: "PORT"
 ) |port, arg|:
   if port < 1 or port > 65535:
    arg.invalid("must be between 1 and 65535")
   port
 .arg("-H", "--host",
   desc: "Host address",
   type: Str,
   default: "127.0.0.1"
 )
 .flag("-v", "--verbose",
   desc: "Enable verbose logging"
 )
 .parse()

Server.run(
 host: args[:host],
 port: args[:port],
 verbose: args[:verbose]
)
```

Invalid invocation:

```text
server --port 99999
```

Diagnostic:

```text
usage: server [-h] [-p PORT] [-H HOST] [-v]

server: error: --port: must be between 1 and 65535
```

---

## 25. Normative summary

- `ArgParser()` defaults to the current process command line.
- Option spellings are strings: `"-v"`, `"--verbose"`.
- `.arg` is for options with values.
- `.flag` is for boolean options.
- `.pos` is for positional arguments.
- `.rest` is for rest positional arguments.
- A local block attached to `.arg` is the canonical validation/transformation site.
- The local `.arg` block receives `|value|` or `|value, arg|`.
- `arg.invalid(...)` and related helpers raise `ArgParser.ParseError` subclasses.
- `.parse()` catches only `ArgParser.ParseError`.
- `.parse()` renders Unix-style usage/error diagnostics and exits with code `2` by default.
- `.parse!()` raises parse errors for tests and embedding.
- `.try_parse()` returns a structured result and is recommended for embedding.
- Ordinary runtime exceptions are not swallowed by the parser.
