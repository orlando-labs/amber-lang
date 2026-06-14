# Amber for Visual Studio Code

Editor support for the [Amber](https://github.com/orlando-labs/amber-lang) programming language (`.am` files).

## Features (v1)

- **Syntax highlighting** — keywords, instance/class variables (`@x`, `@@x`), numbers
  (`0x`/`0b`/`0o`, floats, `_` separators), strings (`"..."` with `#{ }` interpolation and
  `'...'` literal), comments, `def`/`class`/`mixin` definitions, and operators. Unicode
  identifiers (e.g. `α`, `@масса`) are supported.
- **Run / Build tasks** — run or compile the current file via `amberc`:
  - **Amber: Run File** — `amberc <file>` (output streams to the integrated terminal).
  - **Amber: Build File** — `amberc build <file> -o <outDir>/<stem> --target <target>`.
  - Both are also exposed as VS Code tasks (type `amber`) so you can bind keys or use
    *Tasks: Run Task*.

Diagnostics, outline, hover, go-to-definition and rename are planned follow-ups — the Amber
compiler already emits the JSON needed for them (`amber.diag.v1`, `amber.agent_tooling.v1`,
`amber.explain.v1`).

## Requirements

The `amberc` compiler must be available. The extension resolves it in this order:

1. `amber.compilerPath` setting (default `"amberc"`), if it is absolute or found on `PATH`.
2. Fallback to `<workspace>/build/amberc` (the in-repo build output).

Build it from the repository root with `make build`.

## Settings

| Setting | Default | Description |
| --- | --- | --- |
| `amber.compilerPath` | `"amberc"` | Path to the `amberc` executable. |
| `amber.build.target` | `"native"` | `--target` for `amberc build`. |
| `amber.build.outDir` | `"build"` | Output directory for build artifacts. |
| `amber.run.args` | `[]` | Extra args appended to `amberc <file>`. |

## Developing

```sh
cd editors/vscode
npm install
npm run compile      # or: npm run watch
```

Press **F5** to launch an Extension Development Host. Package a `.vsix` with
`npx vsce package`, then install via `code --install-extension amber-lang-*.vsix`.
