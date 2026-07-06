#!/usr/bin/env python3
"""Generate the VM module reference data for the Amber website.

Sources, in order of trust:

  1. **Registry selectors** (drift guard): the ``selector == "..."`` literals in
     ``runtime/stdlib_*.cpp`` are the authoritative surface of each native
     module. Any identifier selector not covered by a documented method (or a
     module's ``drift_ignore`` list) is reported as undocumented.
  2. **Sidecars** (``docs/module-examples/<id>.sidecar``): human-authored module
     metadata + per-method signature, RU/EN blurb, and either an inline
     ``example`` snippet or a ``corpus:`` reference. Format is documented in
     ``docs/engineering/doc-system.md``.
  3. **Corpus** (``corpus/run/<dir>``): verified programs. A ``corpus:`` method
     inlines that program's source and its ``expect.run.json`` value as a
     verified ``# =>`` output.

Modes::

    python3 tools/gen_module_docs.py            # write site/modules-data.js + coverage
    python3 tools/gen_module_docs.py --verify    # additionally run inline examples
    python3 tools/gen_module_docs.py --check      # drift guard only, non-zero on drift

``--verify`` runs each machine-checkable inline example (one that ends in a
``# => <output>`` line) through ``build/amberc`` and records whether the printed
value matches. Verification is best-effort: examples that need capabilities,
I/O, randomness or time simply stay ``unverified`` and never fail the run.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SIDECAR_DIR = ROOT / "docs" / "module-examples"
CORPUS_DIR = ROOT / "corpus" / "run"
OUTPUT_JS = ROOT / "site" / "modules-data.js"
COVERAGE_JSON = ROOT / "site" / "modules-coverage.json"
AMBERC = ROOT / "build" / "amberc"

IDENT_RE = re.compile(r"^[A-Za-z_Ѐ-ӿ][\wЀ-ӿ]*$")
# Method selector = the dotted method name of a `Recv.method(...)` / `recv.field`
# signature. Operator signatures ("time + period") have no dotted method and
# yield no selector.
DOTTED_RE = re.compile(
    r"^[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*\.([A-Za-z_Ѐ-ӿ][\wЀ-ӿ]*)"
)


# --------------------------------------------------------------------------- #
# Sidecar parsing
# --------------------------------------------------------------------------- #
def parse_sidecar(path: Path) -> dict:
    lines = path.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n")
    header: dict = {}
    methods: list[dict] = []
    i = 0
    n = len(lines)

    def is_header_key(line: str) -> bool:
        return bool(re.match(r"^[A-Za-z_][\w.]*:", line)) and not line.startswith(
            "method:"
        )

    # Header block until the first `method:`
    while i < n and not lines[i].startswith("method:"):
        line = lines[i]
        if line.strip() and ":" in line and is_header_key(line):
            key, _, value = line.partition(":")
            header[key.strip()] = value.strip()
        i += 1

    while i < n:
        if not lines[i].startswith("method:"):
            i += 1
            continue
        method = {"sig": lines[i].partition(":")[2].strip()}
        i += 1
        while i < n and not lines[i].startswith("method:"):
            line = lines[i]
            if line.startswith("example:"):
                body: list[str] = []
                i += 1
                while i < n and lines[i].strip() != ".end":
                    body.append(lines[i])
                    i += 1
                method["example"] = "\n".join(body)
                i += 1  # skip .end
                continue
            match = re.match(r"^([A-Za-z_][\w.]*):\s?(.*)$", line)
            if match:
                method[match.group(1)] = match.group(2).strip()
            i += 1
        methods.append(method)

    drift = header.get("drift_ignore", "").strip()
    if drift.startswith("["):
        ignore = [x.strip() for x in drift[1:-1].split(",") if x.strip()]
    else:
        ignore = [x for x in drift.split() if x]

    return {
        "id": header.get("id", path.stem),
        "path": header.get("path", f"{path.stem}.html"),
        "title": header.get("title", path.stem),
        "icon": header.get("icon", ""),
        "source": header.get("source", ""),
        "description": {
            "ru": header.get("description.ru", ""),
            "en": header.get("description.en", ""),
        },
        "note": {"ru": header.get("note.ru", ""), "en": header.get("note.en", "")},
        "drift_ignore": ignore,
        "methods": methods,
    }


def infer_selector(method: dict) -> str | None:
    if method.get("selector"):
        return method["selector"]
    match = DOTTED_RE.match(method["sig"])
    return match.group(1) if match else None


# --------------------------------------------------------------------------- #
# Corpus-backed examples (verified by construction)
# --------------------------------------------------------------------------- #
def corpus_example(name: str) -> tuple[str, str]:
    """Return (example_text, verified_output) for a corpus/run/<name> program."""
    base = CORPUS_DIR / name
    source = (base / "source.am").read_text(encoding="utf-8")
    meta = json.loads((base / "meta.json").read_text(encoding="utf-8"))
    expect = json.loads((base / "expect.run.json").read_text(encoding="utf-8"))
    entry = meta.get("entry", "probe")

    body = "\n".join(
        line for line in source.split("\n") if not line.startswith("package ")
    ).strip("\n")
    value = expect.get("value", "")
    example = f"{body}\n\n{entry}()\n# => {value}"
    return example, value


# --------------------------------------------------------------------------- #
# Best-effort verification of inline examples through amberc
# --------------------------------------------------------------------------- #
EXPECT_RE = re.compile(r"^\s*#\s*=>\s*(.*\S)\s*$")


def expected_output(example: str) -> str | None:
    matches = [EXPECT_RE.match(line) for line in example.split("\n")]
    values = [m.group(1) for m in matches if m]
    return values[-1] if values else None


def runnable_program(example: str) -> str:
    code = [line for line in example.split("\n") if not re.match(r"^\s*#", line)]
    while code and not code[-1].strip():
        code.pop()
    if code:
        assign = re.match(r"^\s*([A-Za-z_]\w*)\s*=\s*\S", code[-1])
        if assign:
            code.append(assign.group(1))
    return "\n".join(code) + "\n"


def run_amberc(program: str) -> str | None:
    if not AMBERC.exists():
        return None
    with tempfile.NamedTemporaryFile(
        "w", suffix=".am", delete=False, encoding="utf-8"
    ) as handle:
        handle.write(program)
        temp = handle.name
    try:
        result = subprocess.run(
            [str(AMBERC), temp],
            capture_output=True,
            text=True,
            timeout=20,
        )
    except (subprocess.TimeoutExpired, OSError):
        return None
    finally:
        Path(temp).unlink(missing_ok=True)
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def verify_example(example: str) -> str:
    """Return 'verified', 'failed' or 'unverified'."""
    expected = expected_output(example)
    if expected is None:
        return "unverified"
    actual = run_amberc(runnable_program(example))
    if actual is None:
        return "unverified"
    return "verified" if actual == expected else "failed"


# --------------------------------------------------------------------------- #
# Drift guard
# --------------------------------------------------------------------------- #
def source_selectors(source_rel: str) -> set[str]:
    path = ROOT / source_rel
    if not source_rel or not path.exists():
        return set()
    text = path.read_text(encoding="utf-8")
    found = re.findall(r'selector == "([^"]+)"', text)
    return {sel for sel in found if IDENT_RE.match(sel)}


def compute_drift(modules: list[dict]) -> dict[str, list[str]]:
    """source file -> sorted list of undocumented identifier selectors."""
    documented: dict[str, set[str]] = {}
    for module in modules:
        source = module["source"]
        if not source:
            continue
        bucket = documented.setdefault(source, set())
        bucket.update(module["drift_ignore"])
        for method in module["methods"]:
            selector = infer_selector(method)
            if selector:
                bucket.add(selector)

    drift: dict[str, list[str]] = {}
    for source, covered in documented.items():
        undocumented = source_selectors(source) - covered
        if undocumented:
            drift[source] = sorted(undocumented)
    return drift


# --------------------------------------------------------------------------- #
# Emit
# --------------------------------------------------------------------------- #
def js_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def emit_js(modules: list[dict]) -> str:
    out: list[str] = []
    out.append(
        "// GENERATED by tools/gen_module_docs.py from docs/module-examples/*.sidecar."
    )
    out.append("// Do not edit by hand; edit the sidecars and regenerate.")
    out.append("const method = (sig, ru, en, example) => ({ sig, ru, en, example });")
    out.append("")
    out.append("window.AMBER_MODULES = [")
    for module in modules:
        out.append("  {")
        out.append(f"    id: {js_string(module['id'])},")
        out.append(f"    path: {js_string(module['path'])},")
        out.append(f"    title: {js_string(module['title'])},")
        out.append(f"    icon: {js_string(module['icon'])},")
        out.append(
            "    description: { "
            f"ru: {js_string(module['description']['ru'])}, "
            f"en: {js_string(module['description']['en'])} }},"
        )
        out.append(
            "    note: { "
            f"ru: {js_string(module['note']['ru'])}, "
            f"en: {js_string(module['note']['en'])} }},"
        )
        out.append("    methods: [")
        for method in module["methods"]:
            out.append(
                "      method("
                f"{js_string(method['sig'])}, "
                f"{js_string(method.get('ru', ''))}, "
                f"{js_string(method.get('en', ''))}, "
                f"{js_string(method['example'])}),"
            )
        out.append("    ]")
        out.append("  },")
    out.append("];")
    return "\n".join(out) + "\n"


# --------------------------------------------------------------------------- #
def build_modules(verify: bool) -> tuple[list[dict], list[dict]]:
    modules: list[dict] = []
    coverage: list[dict] = []
    for sidecar in sorted(SIDECAR_DIR.glob("*.sidecar")):
        module = parse_sidecar(sidecar)
        counts = {"verified": 0, "failed": 0, "unverified": 0, "corpus": 0}
        for method in module["methods"]:
            if method.get("corpus"):
                example, _ = corpus_example(method["corpus"])
                method["example"] = example
                counts["corpus"] += 1
                counts["verified"] += 1
                continue
            method.setdefault("example", "")
            if verify:
                counts[verify_example(method["example"])] += 1
        modules.append(module)
        coverage.append(
            {
                "id": module["id"],
                "methods": len(module["methods"]),
                # Selectors present in the source but intentionally not yet
                # documented (tracked TODOs, surfaced instead of hidden).
                "pending": module["drift_ignore"],
                **counts,
            }
        )
    return modules, coverage


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", help="run inline examples")
    parser.add_argument(
        "--check", action="store_true", help="drift guard only; do not write outputs"
    )
    args = parser.parse_args()

    modules, coverage = build_modules(verify=args.verify and not args.check)
    drift = compute_drift(modules)

    for source, selectors in drift.items():
        print(
            f"drift: {source} exposes undocumented selectors: {', '.join(selectors)}",
            file=sys.stderr,
        )

    if args.check:
        if drift:
            print(f"module drift check FAILED ({len(drift)} file(s))", file=sys.stderr)
            return 1
        print(f"module drift check OK: {len(modules)} modules")
        return 0

    OUTPUT_JS.write_text(emit_js(modules), encoding="utf-8")

    report = {
        "schema": "amber.modules.coverage.v1",
        "totals": {
            "modules": len(modules),
            "methods": sum(len(m["methods"]) for m in modules),
        },
        "modules": coverage,
        "drift": drift,
    }
    COVERAGE_JSON.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    total_methods = report["totals"]["methods"]
    print(f"wrote {OUTPUT_JS.relative_to(ROOT)}: {len(modules)} modules, {total_methods} methods")
    if args.verify:
        verified = sum(c["verified"] for c in coverage)
        failed = sum(c["failed"] for c in coverage)
        print(f"verified {verified}/{total_methods} examples ({failed} mismatched)")
    if drift:
        print(f"warning: {len(drift)} source file(s) have undocumented selectors")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
