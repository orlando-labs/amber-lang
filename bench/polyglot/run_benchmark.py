#!/usr/bin/env python3
import argparse
from dataclasses import dataclass
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


@dataclass(frozen=True)
class Workload:
    name: str
    expected_checksum: str
    amber_module: str
    amber_entry: str
    amber_source: str
    python_source: str
    ruby_source: str
    cpp_source: str
    cpp_binary: str


WORKLOADS = {
    "arithmetic": Workload(
        name="arithmetic",
        expected_checksum="715609516598740",
        amber_module="bench.polyglot",
        amber_entry="main",
        amber_source="main.am",
        python_source="main.py",
        ruby_source="main.rb",
        cpp_source="main.cpp",
        cpp_binary="main",
    ),
    "calls-collections": Workload(
        name="calls-collections",
        expected_checksum="2047795430",
        amber_module="bench.polyglot.calls_collections",
        amber_entry="__init__",
        amber_source="calls_collections.am",
        python_source="calls_collections.py",
        ruby_source="calls_collections.rb",
        cpp_source="calls_collections.cpp",
        cpp_binary="calls_collections",
    ),
}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def run_command(command, cwd: Path, *, capture=True) -> subprocess.CompletedProcess:
    completed = subprocess.run(
        [str(part) for part in command],
        cwd=str(cwd),
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    if completed.returncode != 0:
        stdout = completed.stdout or ""
        stderr = completed.stderr or ""
        raise RuntimeError(
            "command failed with exit code "
            f"{completed.returncode}: {' '.join(map(str, command))}\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}"
        )
    return completed


def choose_cxx() -> str:
    env_cxx = os.environ.get("CXX")
    if env_cxx:
        return env_cxx
    for candidate in ("clang++", "g++", "c++"):
        path = shutil.which(candidate)
        if path:
            return path
    raise RuntimeError("no C++ compiler found in PATH")


def ensure_amber_tools(root: Path) -> None:
    amberc = root / "build" / "amberc"
    iamber = root / "build" / "iamber"
    if amberc.exists() and iamber.exists():
        return
    run_command(["make", "build/amberc", "build/iamber"], root, capture=False)


def amber_artifact_path(build_dir: Path, workload: Workload) -> Path:
    return build_dir / "amber" / "out" / f"{workload.amber_module}.amberbc"


def build_amber_bytecode(root: Path, build_dir: Path) -> None:
    out_dir = build_dir / "amber" / "out"
    cache_dir = build_dir / "amber" / "cache"
    out_dir.mkdir(parents=True, exist_ok=True)
    cache_dir.mkdir(parents=True, exist_ok=True)
    run_command(
        [
            root / "build" / "amberc",
            "build",
            root / "bench" / "polyglot" / "amber" / "amber.build.json",
            "--out-dir",
            out_dir,
            "--cache-dir",
            cache_dir,
        ],
        root,
    )
    for workload in WORKLOADS.values():
        artifact = amber_artifact_path(build_dir, workload)
        if not artifact.exists():
            raise RuntimeError(
                f"expected Amber bytecode artifact is missing: {artifact}"
            )


def compile_cpp_program(
    root: Path, build_dir: Path, cxx: str, workload: Workload
) -> Path:
    output = build_dir / "cpp" / workload.cpp_binary
    output.parent.mkdir(parents=True, exist_ok=True)
    run_command(
        [
            cxx,
            "-std=c++17",
            "-O2",
            "-pipe",
            root / "bench" / "polyglot" / "cpp" / workload.cpp_source,
            "-o",
            output,
        ],
        root,
    )
    return output


def compile_amberbc_runner(root: Path, build_dir: Path, cxx: str) -> Path:
    output = build_dir / "amberbc_run"
    output.parent.mkdir(parents=True, exist_ok=True)
    sources = [
        root / "bench" / "polyglot" / "tools" / "amberbc_run.cpp",
        root / "bytecode" / "format.cpp",
        root / "frontend" / "lexer" / "token.cpp",
        root / "profile" / "capabilities.cpp",
        root / "profile" / "effects.cpp",
        root / "profile" / "replay.cpp",
        root / "profile" / "data.cpp",
        root / "profile" / "wasm_accel.cpp",
        root / "profile" / "modern.cpp",
        root / "package" / "package.cpp",
        root / "runtime" / "vm.cpp",
    ]
    run_command(
        [
            cxx,
            "-std=c++17",
            "-O2",
            "-pipe",
            "-pthread",
            "-I",
            root,
            *sources,
            "-o",
            output,
        ],
        root,
    )
    return output


def measure(command, cwd: Path) -> dict:
    helper = r"""
import json
import resource
import subprocess
import sys
import time

command = sys.argv[1:]
start = time.perf_counter()
completed = subprocess.run(
    command,
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
elapsed = time.perf_counter() - start
rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
rss_mb = rss / (1024 * 1024) if sys.platform == "darwin" else rss / 1024
print(json.dumps({
    "returncode": completed.returncode,
    "stdout": completed.stdout,
    "stderr": completed.stderr,
    "elapsed_s": elapsed,
    "peak_rss_mb": rss_mb,
}))
"""
    completed = subprocess.run(
        [sys.executable, "-c", helper, *[str(part) for part in command]],
        cwd=str(cwd),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "measurement helper failed\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return json.loads(completed.stdout)


def extract_checksum(name: str, stdout: str) -> str:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if name == "amber-interpreted":
        for line in lines:
            if line.startswith("=> "):
                return line[3:].strip()
    if lines:
        return lines[-1]
    return ""


def aggregate(
    name: str, command, samples: list[dict], expected_checksum: str
) -> dict:
    good = [sample for sample in samples if sample["returncode"] == 0]
    if len(good) != len(samples):
        failed = next(sample for sample in samples if sample["returncode"] != 0)
        raise RuntimeError(
            f"{name} failed with exit code {failed['returncode']}\n"
            f"stdout:\n{failed['stdout']}\nstderr:\n{failed['stderr']}"
        )
    checksums = [extract_checksum(name, sample["stdout"]) for sample in good]
    mismatches = [
        checksum for checksum in checksums if checksum != expected_checksum
    ]
    if mismatches:
        raise RuntimeError(
            f"{name} produced unexpected checksum {mismatches[0]}, "
            f"expected {expected_checksum}"
        )
    elapsed = [sample["elapsed_s"] for sample in good]
    rss_values = [
        sample["peak_rss_mb"]
        for sample in good
        if sample["peak_rss_mb"] is not None
    ]
    return {
        "name": name,
        "command": [str(part) for part in command],
        "runs": len(good),
        "checksum": checksums[0] if checksums else "",
        "elapsed_s": elapsed,
        "mean_s": sum(elapsed) / len(elapsed),
        "best_s": min(elapsed),
        "peak_rss_mb": max(rss_values) if rss_values else None,
    }


def print_table(results: list[dict]) -> None:
    print(
        f"{'program':<19} {'runs':>4} {'mean_s':>10} "
        f"{'best_s':>10} {'peak_rss_mb':>12} {'checksum':>18}"
    )
    print("-" * 82)
    for result in results:
        rss = (
            f"{result['peak_rss_mb']:.1f}"
            if result["peak_rss_mb"] is not None
            else "n/a"
        )
        print(
            f"{result['name']:<19} {result['runs']:>4} "
            f"{result['mean_s']:>10.4f} {result['best_s']:>10.4f} "
            f"{rss:>12} {result['checksum']:>18}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run the Amber/Python/Ruby/C++ polyglot benchmark."
    )
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument(
        "--workload",
        choices=sorted(WORKLOADS.keys()),
        default="arithmetic",
        help="benchmark program to run",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="reuse existing generated binaries and Amber bytecode artifacts",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=None,
        help="directory for generated binaries and Amber bytecode artifacts",
    )
    parser.add_argument(
        "--json-out",
        type=Path,
        default=None,
        help="write raw aggregate results to this JSON file",
    )
    args = parser.parse_args()
    if args.repeats < 1:
        raise RuntimeError("--repeats must be at least 1")

    root = repo_root()
    workload = WORKLOADS[args.workload]
    build_dir = args.build_dir or (root / "bench" / "polyglot" / "build")
    if not build_dir.is_absolute():
        build_dir = root / build_dir
    build_dir.mkdir(parents=True, exist_ok=True)

    cxx = choose_cxx()
    if args.no_build:
        amberbc = amber_artifact_path(build_dir, workload)
        amberbc_runner = build_dir / "amberbc_run"
        cpp_binary = build_dir / "cpp" / workload.cpp_binary
    else:
        ensure_amber_tools(root)
        build_amber_bytecode(root, build_dir)
        amberbc = amber_artifact_path(build_dir, workload)
        amberbc_runner = compile_amberbc_runner(root, build_dir, cxx)
        cpp_binary = compile_cpp_program(root, build_dir, cxx, workload)

    ruby = shutil.which("ruby")
    if ruby is None:
        raise RuntimeError("ruby was not found in PATH")

    programs = [
        (
            "amber-interpreted",
            [
                root / "build" / "iamber",
                "--eval-file",
                root
                / "bench"
                / "polyglot"
                / "amber"
                / "src"
                / workload.amber_source,
            ],
        ),
        ("amber-built", [amberbc_runner, amberbc, workload.amber_entry]),
        (
            "python",
            [
                sys.executable,
                root / "bench" / "polyglot" / "python" / workload.python_source,
            ],
        ),
        (
            "ruby",
            [ruby, root / "bench" / "polyglot" / "ruby" / workload.ruby_source],
        ),
        ("cpp", [cpp_binary]),
    ]

    results = []
    for name, command in programs:
        samples = [measure(command, root) for _ in range(args.repeats)]
        results.append(
            aggregate(name, command, samples, workload.expected_checksum)
        )

    print_table(results)
    default_json = (
        build_dir / "results.json"
        if workload.name == "arithmetic"
        else build_dir / f"{workload.name}.results.json"
    )
    json_out = args.json_out or default_json
    json_out.write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"\nWrote JSON results to {json_out}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"benchmark error: {error}", file=sys.stderr)
        raise SystemExit(1)
