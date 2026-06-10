# amber.numeric-profile.v1

Status: proposed language design; not implemented in the current parser,
bytecode, VM, or native backend.

Amber numeric semantics use a fixed-width `Int` by default, selected through a
compile-time numeric profile. Arbitrary-precision arithmetic remains available
through explicit `BigInt` values. The goal is to keep ordinary VM and native
integer paths predictable while preserving an opt-in path for unbounded
integers.

## Source Preamble

Canonical source spelling:

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

- `Int8`, `Int16`, `Int32`, `Int64`
- `UInt8`, `UInt16`, `UInt32`, `UInt64`
- `BigInt`

Accepted `overflow` values:

- `checked`
- `wrapping`
- `saturating`

If a source file omits the preamble, defaults are:

```text
int: Int64
overflow: checked
```

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

`Int` is a compile-time alias for the selected fixed-width integer type. It is
not a runtime-mutable property and it is not ordinary user-defined type aliasing.

Examples under the default profile:

```amber
type Int = Int64 # conceptual resolution only, not surface syntax

x = 3           # Int, resolved as Int64
y = BigInt(3)   # explicit arbitrary-precision value
```

Public ABI metadata records the resolved concrete type. If a package exports a
function accepting `Int` while its numeric profile resolves `Int` to `Int64`,
dependent packages see the boundary as `Int64`, even if their own `Int` alias
uses a different fixed-width type.

`BigInt` is a distinct arbitrary-precision numeric type. It does not inherit the
fixed-width overflow policy and is never introduced by implicit promotion from
`Int`.

## Literals

Unsuffixed integer literals have type `Int`, meaning the selected concrete
fixed-width type.

```amber
n = 123 # Int -> Int64 by default
```

A literal outside the selected `Int` range is a compile-time diagnostic unless
it is explicitly targeted as `BigInt`, for example through `BigInt(...)` or an
explicit `BigInt` boundary accepted by the checker/lowering pipeline.

The lexer/parser may keep integer literal text at arbitrary precision for
diagnostics and for explicit `BigInt` construction, but ordinary fixed-width
lowering must reject out-of-range `Int` literals before bytecode execution.

## Overflow Semantics

The selected overflow mode applies to ordinary fixed-width `Int` arithmetic.

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
- integer constants are range-checked against the resolved `Int` type;
- VM quick paths dispatch to concrete fixed-width helpers for checked,
  wrapping, or saturating arithmetic;
- native lowering can emit direct fixed-width arithmetic and only the overflow
  checks required by the selected policy;
- `BigInt` operations use explicit runtime helpers and allocation-aware
  slowpaths;
- there is no hidden `will overflow -> promote to BigInt` edge on ordinary
  `Int` operations.

This keeps hot integer loops predictable for both the interpreter and generated
native code. Programs that need arbitrary precision choose `BigInt` explicitly
and pay its allocation, GC-root, and helper-call costs only at those sites.

## Cross-Package Rules

Packages may choose different numeric profiles. Package boundaries remain
stable because exported signatures and ABI metadata expose concrete resolved
types rather than the local alias spelling.

Rules:

- source preamble and manifest numeric profile must agree in package builds;
- exported `Int` is serialized as the resolved concrete type;
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
