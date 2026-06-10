# amber.numeric-profile.v1

Status: proposed language design; not implemented in the current parser,
bytecode, VM, or native backend.

Amber numeric semantics use arbitrary-precision `BigInt` as the default
resolution of `Int`. A compile-time numeric profile may opt into a fixed-width
`Int` for performance-sensitive packages. The goal is to preserve safe default
integer semantics while giving VM and native builds a predictable fixed-width
path when a package explicitly asks for it.

## Source Preamble

Canonical fixed-width source spelling:

```amber
numeric:
 int: Int64
 overflow: checked

x = 3
y = BigInt(3)
```

`numeric:` is a compile-time module/package preamble directive, not an
expression statement. It does not update `$_`, cannot be conditional, and is
valid only before executable top-level code. It may appear after an optional
`package` declaration and before ordinary imports, exports, declarations, or
statements.

Accepted `int` values:

- `BigInt`
- `Int8`, `Int16`, `Int32`, `Int64`
- `UInt8`, `UInt16`, `UInt32`, `UInt64`

Accepted `overflow` values:

- `checked`
- `wrapping`
- `saturating`

If a source file omits the preamble, defaults are:

```text
int: BigInt
overflow: checked
```

When `int: BigInt`, the `overflow` value is accepted for profile shape
stability but has no effect on ordinary `Int` arithmetic, because `BigInt`
operations do not overflow by width.

## Manifest Mirror

Package builds mirror the same choice in `amber.build.json`:

```json
{
  "profiles": {
    "numeric": {
      "int": "Int64",
      "overflow": "checked"
    }
  }
}
```

The manifest value is the reproducibility anchor for package builds. Source
preambles are allowed as local documentation and validation, but their resolved
values must match the manifest. A mismatch is a build/profile diagnostic and
the package must not compile.

Single-file builds without a manifest use the source preamble when present, or
the default numeric profile otherwise.

## Type Semantics

`Int` is a compile-time alias for the selected integer type. It is not a
runtime-mutable property and it is not ordinary user-defined type aliasing.

Examples under the default profile:

```amber
type Int = BigInt # conceptual resolution only, not surface syntax

x = 3           # Int, resolved as BigInt
y = BigInt(3)   # explicit arbitrary-precision value
```

Public ABI metadata records the resolved concrete type. If a package exports a
function accepting `Int` while its numeric profile resolves `Int` to `BigInt`,
dependent packages see the boundary as `BigInt`, even if their own `Int` alias
uses a different fixed-width type.

`BigInt` is a distinct arbitrary-precision numeric type. Under the default
numeric profile, `Int` resolves to `BigInt`. Under a fixed-width profile,
`BigInt` remains available explicitly and is never introduced by implicit
promotion from fixed-width `Int`.

## Literals

Unsuffixed integer literals have type `Int`, meaning the selected concrete
integer type.

```amber
n = 123 # Int -> BigInt by default
```

A literal has arbitrary precision under the default `BigInt` profile. Under a
fixed-width `Int` profile, a literal outside the selected range is a
compile-time diagnostic unless it is explicitly targeted as `BigInt`, for
example through `BigInt(...)` or an explicit `BigInt` boundary accepted by the
checker/lowering pipeline.

The lexer/parser may keep integer literal text at arbitrary precision for
diagnostics and for explicit `BigInt` construction, but ordinary fixed-width
lowering must reject out-of-range `Int` literals before bytecode execution.

## Overflow Semantics

The selected overflow mode applies only to ordinary fixed-width `Int`
arithmetic. It has no effect when `Int` resolves to `BigInt`.

`checked`:

- `+`, `-`, `*`, unary `-`, shifts, and other bounded operations detect
  overflow;
- overflow raises language-level `OverflowError`;
- division by zero remains the existing numeric runtime error.

`wrapping`:

- arithmetic wraps according to the selected signed or unsigned width;
- no `BigInt` promotion occurs;
- division by zero still raises.

`saturating`:

- arithmetic clamps to the selected type's minimum or maximum value;
- for unsigned types, the lower clamp is zero;
- division by zero still raises.

Operations whose mathematical result is naturally non-integer, such as mixed
`Int`/`Float` operations, continue to follow the ordinary numeric dispatch and
conversion rules for those operand types.

## VM And Native Guidance

VM, MIR, and native lowering should specialize on the resolved concrete integer
type, not on an abstract promoting `Int`.

Implementation expectations:

- bytecode/profile metadata records the numeric profile used for compilation;
- integer constants are range-checked only when the resolved `Int` type is
  fixed-width;
- VM quick paths dispatch to concrete fixed-width helpers for checked,
  wrapping, or saturating arithmetic;
- native lowering can emit direct fixed-width arithmetic and only the overflow
  checks required by the selected policy;
- default `BigInt` operations use runtime helpers and allocation-aware
  slowpaths;
- there is no hidden `will overflow -> promote to BigInt` edge on ordinary
  fixed-width `Int` operations.

This keeps the default language safe and arbitrary-precision, while fixed-width
profiles keep hot integer loops predictable for both the interpreter and
generated native code. Programs that need fixed-width performance opt in at the
package/profile level.

## Cross-Package Rules

Packages may choose different numeric profiles. Package boundaries remain
stable because exported signatures and ABI metadata expose concrete resolved
types rather than the local alias spelling.

Rules:

- source preamble and manifest numeric profile must agree in package builds;
- exported `Int` is serialized as the resolved concrete type, including
  `BigInt` under the default profile;
- imports do not inherit the importing package's `Int` alias at already-compiled
  dependency boundaries;
- diagnostics should report both the local alias spelling and the resolved
  concrete type when that helps explain a mismatch.

## Open Implementation Work

- Add parser support for the `numeric:` preamble form.
- Extend build manifest parsing with `profiles.numeric`.
- Store numeric profile metadata in `.amberbc` and package ABI summaries.
- Update checker/binder type display to resolve `Int` consistently.
- Replace `std::int64_t`-only runtime assumptions with concrete fixed-width
  helpers and explicit `BigInt` support.
- Add VM, native, and conformance tests for checked, wrapping, and saturating
  behavior.
