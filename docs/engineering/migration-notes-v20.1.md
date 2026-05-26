# Amber v20.1 Migration Notes

Status: W14 migration notes for the current reference implementation baseline.

## Runtime And Artifact Compatibility

W12 was documentation/spec synchronization only. W13 closes compiler/runtime
contracts for source rules, bytecode verifier initializedness, runtime UNINIT
guards, `CALL`, and source traces. W14 adds build/bootstrap/conformance
metadata through `amber.build.v1`, `amberc build`, and optional `.amberbc`
`PROF` profile metadata.

Existing artifacts produced by the W11.6 baseline remain in the same format
families. Invalid hand-authored `.amberbc` fixtures may now fail earlier under
the stricter W13 verifier if they read registers before definite
initialization, jump outside code bounds, or reference operands outside declared
table/register ranges.

| Artifact family | Migration impact |
| --- | --- |
| `.amberbc` / `amber.bc.v1` | optional `PROF` section added; old artifacts without `PROF` still decode |
| `.amberpkg` / `amber.package.v1` | none |
| `.amberimg` / `amber.image.v1` | none |
| `.ambertrace` / `amber.replay.v1` | none |
| `.amberschema` / `.ambertable` | none |
| `.amberwasm` / `.amberaccel` | none |
| `.ambermodern` | none |

## Repository Layout Changes

W12 added documentation synchronization artifacts. W13 also updates compiler,
bytecode, runtime, and focused test sources. W14 adds a small build support
module and fixtures:

| Path | Purpose |
| --- | --- |
| `tools/spec_sync.py` | Generates the anchor map and checks local Markdown links. |
| `docs/engineering/spec-anchor-map-v1.md` | Generated H1/H2 anchor map for canonical docs/spec files plus registry digests. |
| `docs/engineering/implementation-status-v1.md` | Status dashboard for W0-W15 and release gates. |
| `spec/changelog/v20.1.md` | Changelog for the current v20.1 implementation baseline. |
| `docs/engineering/migration-notes-v20.1.md` | This migration note. |
| `build/build.*` | `amber.build.v1` manifest parser, profile normalization, and summary JSON helpers. |
| `docs/engineering/build-v1.md` | W14 build/bootstrap/conformance engineering note. |
| `tests/fixtures/w14_build/` | CLI smoke fixture for `amberc build`. |

## Maintainer Checklist

1. Run `make spec-sync-check` after documentation or heading changes.
2. Regenerate the anchor map with `python3 tools/spec_sync.py anchor-map > docs/engineering/spec-anchor-map-v1.md` when the check reports drift.
3. Keep implementation status, changelog, migration notes, and README matrix rows in sync with each work-package transition.
4. Treat W15 as planned closure work, not as a retroactive format change to W0-W14 artifacts.
