#!/usr/bin/env python3
"""One-time bootstrap: seed docs/module-examples/*.sidecar from the current
site/modules-data.js baseline so no hand-authored content is lost when
gen_module_docs.py takes over generation.

Run once (already run to create the initial sidecars); kept for provenance and
in case the sidecars need to be regenerated from a known-good baseline. It reads
a JSON dump of `window.AMBER_MODULES` (produced with node, see the module docs)
and writes one `.sidecar` per module in the format documented in
docs/engineering/doc-system.md.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "docs" / "module-examples"

# Which runtime translation unit backs each site module (used by the drift
# guard in gen_module_docs.py).
SOURCE_BY_ID = {
    "math": "runtime/stdlib_math.cpp",
    "json": "runtime/stdlib_json.cpp",
    "base64": "runtime/stdlib_codecs.cpp",
    "base64url": "runtime/stdlib_codecs.cpp",
    "hex": "runtime/stdlib_codecs.cpp",
    "digest": "runtime/stdlib_digest.cpp",
    "secure-random": "runtime/stdlib_secure_random.cpp",
    "argparser": "runtime/stdlib_argparser.cpp",
    "uuid": "runtime/stdlib_uuid.cpp",
    "time": "runtime/stdlib_time.cpp",
    "time-period": "runtime/stdlib_time.cpp",
    "url": "runtime/stdlib_url.cpp",
}


def emit_module(module: dict) -> str:
    lines: list[str] = []
    mid = module["id"]
    lines.append(f"id: {mid}")
    lines.append(f"path: {module['path']}")
    lines.append(f"title: {module['title']}")
    lines.append(f"icon: {module['icon']}")
    lines.append(f"source: {SOURCE_BY_ID.get(mid, '')}")
    lines.append(f"description.ru: {module['description']['ru']}")
    lines.append(f"description.en: {module['description']['en']}")
    lines.append(f"note.ru: {module['note']['ru']}")
    lines.append(f"note.en: {module['note']['en']}")
    lines.append("drift_ignore:")
    lines.append("")

    for entry in module["methods"]:
        lines.append(f"method: {entry['sig']}")
        lines.append(f"ru: {entry['ru']}")
        lines.append(f"en: {entry['en']}")
        lines.append("example:")
        lines.extend(entry["example"].split("\n"))
        lines.append(".end")
        lines.append("")

    return "\n".join(lines).rstrip("\n") + "\n"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: seed_module_sidecars.py <modules-baseline.json>", file=sys.stderr)
        return 2
    modules = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for module in modules:
        target = OUT_DIR / f"{module['id']}.sidecar"
        target.write_text(emit_module(module), encoding="utf-8")
        print(f"wrote {target.relative_to(ROOT)} ({len(module['methods'])} methods)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
