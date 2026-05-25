# amber.wasm/accelerator.v1

Status: implemented for the `W11.5` Wasm component and accelerator baseline.

This baseline is metadata-first. `WASM` carries frozen-world component
interface mappings: component/world names, host imports, exports,
schema-boundary markers, capability bindings, and effect rows. `ACCL` carries
accelerator kernel descriptors: target, entry symbol, parameter/capture
surface, effect row, and any forbidden subset features discovered by tooling.

Wasm components validate as frozen-world artifacts. Raw FFI and reflective
world mutation are denied by default, and each component must declare at least
one export. Host imports may bind to Amber capabilities, so package/image hosts
can resolve them through the same W11.1 policy path.

Accelerator kernels validate the restricted closure subset at the metadata
boundary. Kernel values are limited to primitive numeric/boolean scalars and
tensor/buffer/slice/device-buffer forms. GPU-family targets must carry
`!{gpu}`. Forbidden operations such as `dynamic_dispatch`, allocation,
reflection, FFI, hidden I/O, object access, watch/random/time/fs/net/env, or
host-stack exceptions produce `AcceleratorError`.

CLI:

```text
amberc wasm-build plugin.amberwasm
amberc accel-check kernels.amberaccel
```

Both commands read deterministic line-oriented profile documents and emit
machine-readable JSON. The same structs round-trip through `.amberbc` optional
sections `WASM` and `ACCL`, are verified during bytecode decode, and are exposed
through runtime package mirrors plus `RuntimeWorld::wasm_validation()` and
`RuntimeWorld::accelerator_validation()`.
