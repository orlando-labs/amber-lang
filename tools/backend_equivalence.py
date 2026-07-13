#!/usr/bin/env python3
"""Backend-equivalence bundle (Phase 1 item 8).

Every `corpus/run` fixture is executed through both backends:

- `amberc build --target bytecode-wrapper`: the VM lane (semantic oracle);
- `amberc build --target native`: the cpp-bytecode-direct lane. Incomplete
  coverage may use the whole-program bailout/restart; full coverage emits no
  VM restart path.

Both executables must produce identical stdout, stderr, and exit codes. This
is the cheap guard for all future native-coverage widening: any divergence
between the native lane and the VM shows up as a mismatch here.

The bailout/restart model is sound because native-eligible code produces no
effect that survives a bailout: most operations are pure, while admitted ivar
and collection mutations stay in a disposable native heap (see
docs/engineering/native-backend-equivalence-v1.md). This harness asserts the
observable half of that invariant.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "corpus" / "run"
# AMBERC / AMBER_BE_WORKDIR let an alternate build drive the harness without
# clobbering build/ -- e.g. the tagged value-repr lane:
#   make BUILD_DIR=build-tagged VALUE_REPR=tagged build-tagged/amberc
#   AMBERC=build-tagged/amberc AMBER_BE_WORKDIR=build-tagged/backend-equivalence \
#       python3 tools/backend_equivalence.py
AMBERC = Path(os.environ.get("AMBERC") or (ROOT / "build" / "amberc"))
WORK_DIR = Path(
    os.environ.get("AMBER_BE_WORKDIR") or (ROOT / "build" / "backend-equivalence")
)


def run_capture(command: list[str], timeout: float = 60.0) -> tuple[int, str, str]:
    proc = subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=timeout,
        cwd=ROOT,
    )
    return proc.returncode, proc.stdout, proc.stderr


def build_fixture_source(fixture: Path, meta: dict) -> str:
    source = (fixture / meta["source"]).read_text()
    entry = meta.get("entry", "init")
    if entry not in ("", "init"):
        # Drive the fixture entry from module init so the generated
        # executable's printed init result is the entry result.
        source = source.rstrip("\n") + "\n\n" + entry + "()\n"
    return source


def main() -> int:
    if not AMBERC.exists():
        print("backend-equivalence: build/amberc is missing; run `make build`",
              file=sys.stderr)
        return 2

    WORK_DIR.mkdir(parents=True, exist_ok=True)
    fixtures = sorted(p for p in CORPUS.iterdir() if (p / "meta.json").exists())
    if not fixtures:
        print("backend-equivalence: no corpus/run fixtures found",
              file=sys.stderr)
        return 2

    passed = 0
    failures: list[str] = []
    for fixture in fixtures:
        meta = json.loads((fixture / "meta.json").read_text())
        if meta.get("phase") != "run":
            continue
        name = fixture.name
        source_path = WORK_DIR / f"{name}.am"
        source_path.write_text(build_fixture_source(fixture, meta))

        lanes: dict[str, tuple[int, str, str] | None] = {}
        build_failed = False
        for lane, target in (("vm", "bytecode-wrapper"), ("native", "native")):
            exe = WORK_DIR / f"{name}.{lane}"
            code, out, err = run_capture([
                str(AMBERC), "build", str(source_path), "--target", target,
                "-o", str(exe), "--out-dir", str(WORK_DIR),
            ])
            if code != 0:
                failures.append(f"{name}: {lane} lane build failed:\n{err or out}")
                build_failed = True
                break
            lanes[lane] = run_capture([str(exe)])
        if build_failed:
            continue

        vm_result = lanes["vm"]
        native_result = lanes["native"]
        if vm_result == native_result:
            passed += 1
            continue
        failures.append(
            f"{name}: backend divergence\n"
            f"  vm:     exit={vm_result[0]} stdout={vm_result[1]!r} "
            f"stderr={vm_result[2]!r}\n"
            f"  native: exit={native_result[0]} stdout={native_result[1]!r} "
            f"stderr={native_result[2]!r}"
        )

    for failure in failures:
        print(failure, file=sys.stderr)
    print(f"backend-equivalence: {passed} passed, {len(failures)} failed")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
