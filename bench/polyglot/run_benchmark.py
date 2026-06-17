#!/usr/bin/env python3
import argparse
from dataclasses import dataclass
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional


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
    go_source: str
    go_binary: str
    requires_full_amber_native: bool = True
    amber_grants: tuple[str, ...] = ()


JSON_RECORDS = 20000
JSON_FIXTURE_RELATIVE = Path("bench/polyglot/build/json/events.jsonl")


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
        go_source="main.go",
        go_binary="main",
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
        go_source="calls_collections.go",
        go_binary="calls_collections",
    ),
    "sha-digest": Workload(
        name="sha-digest",
        expected_checksum="2242493101",
        amber_module="bench.polyglot.sha_digest",
        amber_entry="__init__",
        amber_source="sha_digest.am",
        python_source="sha_digest.py",
        ruby_source="sha_digest.rb",
        cpp_source="sha_digest.cpp",
        cpp_binary="sha_digest",
        go_source="sha_digest.go",
        go_binary="sha_digest",
    ),
    "json": Workload(
        name="json",
        expected_checksum="1531352227",
        amber_module="bench.polyglot.json",
        amber_entry="main",
        amber_source="json.am",
        python_source="json_workload.py",
        ruby_source="json_workload.rb",
        cpp_source="json_workload.cpp",
        cpp_binary="json_workload",
        go_source="json_workload.go",
        go_binary="json_workload",
        amber_grants=("fs.read=bench/polyglot/build/json/events.jsonl",),
    ),
    "codecs": Workload(
        name="codecs",
        expected_checksum="2056190",
        amber_module="bench.polyglot.codecs",
        amber_entry="main",
        amber_source="codecs.am",
        python_source="codecs_workload.py",
        ruby_source="codecs_workload.rb",
        cpp_source="codecs_workload.cpp",
        cpp_binary="codecs_workload",
        go_source="codecs_workload.go",
        go_binary="codecs_workload",
    ),
}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def run_command(
    command, cwd: Path, *, capture=True, env: Optional[dict] = None
) -> subprocess.CompletedProcess:
    run_env = None
    if env is not None:
        run_env = os.environ.copy()
        run_env.update({key: str(value) for key, value in env.items()})
    completed = subprocess.run(
        [str(part) for part in command],
        cwd=str(cwd),
        env=run_env,
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


def choose_go() -> Optional[str]:
    return shutil.which("go")


def ensure_amber_tools(root: Path) -> None:
    amberc = root / "build" / "amberc"
    iamber = root / "build" / "iamber"
    if amberc.exists() and iamber.exists():
        return
    run_command(["make", "build/amberc", "build/iamber"], root, capture=False)


def prepare_json_fixture(root: Path) -> Path:
    path = root / JSON_FIXTURE_RELATIVE
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        for i in range(JSON_RECORDS):
            value = (i * 17) % 100000
            group = i % 8
            name = i % 100
            handle.write(
                f'{{"id":{i},"value":{value},'
                f'"group":{group},"name":"event-{name}"}}\n'
            )
    return path


def amber_artifact_path(build_dir: Path, workload: Workload) -> Path:
    return build_dir / "amber" / "out" / f"{workload.amber_module}.amberbc"


def safe_artifact_name(value: str) -> str:
    return "".join(
        ch if ch.isalnum() or ch in "._-" else "_"
        for ch in value
    ) or "artifact"


def amber_native_path(build_dir: Path, workload: Workload) -> Path:
    return build_dir / "amber" / "native" / safe_artifact_name(workload.amber_module)


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
            "--target",
            "bytecode",
        ],
        root,
    )
    for workload in WORKLOADS.values():
        artifact = amber_artifact_path(build_dir, workload)
        if not artifact.exists():
            raise RuntimeError(
                f"expected Amber bytecode artifact is missing: {artifact}"
            )


def build_amber_native_executable(
    root: Path, build_dir: Path, workload: Workload
) -> Path:
    output = amber_native_path(build_dir, workload)
    output.parent.mkdir(parents=True, exist_ok=True)
    source = root / "bench" / "polyglot" / "amber" / "src" / workload.amber_source
    entry = "init" if workload.amber_entry == "__init__" else "main-only"
    command = [
        root / "build" / "amberc",
        "build",
        source,
        "-o",
        output,
        "--entry",
        entry,
    ]
    for grant in workload.amber_grants:
        command.extend(["--grant", grant])
    completed = run_command(command, root)
    build_result = json.loads(completed.stdout)
    if build_result.get("status") != "ok":
        raise RuntimeError(f"Amber native build failed: {completed.stdout}")
    if not workload.requires_full_amber_native:
        if not output.exists():
            raise RuntimeError(f"expected Amber built executable is missing: {output}")
        return output
    if build_result.get("native_backend") != "cpp-bytecode-direct-v1":
        raise RuntimeError(
            "Amber built benchmark expected native backend "
            f"cpp-bytecode-direct-v1, got {build_result.get('native_backend')!r}"
        )
    if build_result.get("native_entry") is not True:
        raise RuntimeError(
            "Amber built benchmark entry is not native: "
            f"{build_result.get('native_fallback_reason')}"
        )
    native_code_count = build_result.get("native_code_count")
    bytecode_code_count = build_result.get("bytecode_code_count")
    if native_code_count != bytecode_code_count:
        raise RuntimeError(
            "Amber built benchmark requires full native coverage, got "
            f"{native_code_count}/{bytecode_code_count} code objects: "
            f"{build_result.get('native_fallback_reason')}"
        )
    if not output.exists():
        raise RuntimeError(f"expected Amber native executable is missing: {output}")
    return output


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


def compile_go_program(root: Path, build_dir: Path, go: str, workload: Workload) -> Path:
    output = build_dir / "go" / workload.go_binary
    cache_dir = build_dir / "go-cache"
    output.parent.mkdir(parents=True, exist_ok=True)
    cache_dir.mkdir(parents=True, exist_ok=True)
    run_command(
        [
            go,
            "build",
            "-o",
            output,
            root / "bench" / "polyglot" / "go" / workload.go_source,
        ],
        root,
        env={"GOCACHE": cache_dir},
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
        root / "runtime" / "context.cpp",
        root / "runtime" / "text.cpp",
        root / "runtime" / "io.cpp",
        root / "runtime" / "vm.cpp",
        root / "runtime" / "stdlib_registry.cpp",
        root / "runtime" / "stdlib_math.cpp",
        root / "runtime" / "stdlib_json.cpp",
        root / "runtime" / "stdlib_codecs.cpp",
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
        "available": True,
        "runs": len(good),
        "checksum": checksums[0] if checksums else "",
        "elapsed_s": elapsed,
        "mean_s": sum(elapsed) / len(elapsed),
        "best_s": min(elapsed),
        "peak_rss_mb": max(rss_values) if rss_values else None,
    }


def unavailable_result(name: str, reason: str) -> dict:
    return {
        "name": name,
        "command": [],
        "available": False,
        "error": reason,
        "runs": 0,
        "checksum": "",
        "elapsed_s": [],
        "mean_s": None,
        "best_s": None,
        "peak_rss_mb": None,
    }


def print_table(results: list[dict]) -> None:
    print(
        f"{'program':<19} {'runs':>4} {'mean_s':>10} "
        f"{'best_s':>10} {'peak_rss_mb':>12} {'checksum':>18}"
    )
    print("-" * 82)
    for result in results:
        mean_s = (
            f"{result['mean_s']:.4f}"
            if result["mean_s"] is not None
            else "n/a"
        )
        best_s = (
            f"{result['best_s']:.4f}"
            if result["best_s"] is not None
            else "n/a"
        )
        rss = (
            f"{result['peak_rss_mb']:.1f}"
            if result["peak_rss_mb"] is not None
            else "n/a"
        )
        checksum = result["checksum"] or result.get("error", "n/a")
        print(
            f"{result['name']:<19} {result['runs']:>4} "
            f"{mean_s:>10} {best_s:>10} "
            f"{rss:>12} {checksum:>18}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run the Amber/Python/Ruby/C++/Go polyglot benchmark."
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
    if workload.name == "json":
        prepare_json_fixture(root)

    cxx = choose_cxx()
    go = choose_go()
    go_unavailable_reason = None
    if args.no_build:
        amber_built = amber_native_path(build_dir, workload)
        amber_built_command = [amber_built]
        if not amber_built.exists():
            raise RuntimeError(f"Amber native executable missing: {amber_built}")
        cpp_binary = build_dir / "cpp" / workload.cpp_binary
        go_binary = build_dir / "go" / workload.go_binary
        if not go_binary.exists():
            if go is None:
                go_unavailable_reason = "go not found in PATH"
            else:
                go_unavailable_reason = f"go binary missing: {go_binary}"
    else:
        ensure_amber_tools(root)
        build_amber_bytecode(root, build_dir)
        amber_built = build_amber_native_executable(root, build_dir, workload)
        amber_built_command = [amber_built]
        cpp_binary = compile_cpp_program(root, build_dir, cxx, workload)
        if go is None:
            go_binary = None
            go_unavailable_reason = "go not found in PATH"
        else:
            go_binary = compile_go_program(root, build_dir, go, workload)

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
        ("amber-built", amber_built_command),
        (
            "python",
            [
                sys.executable,
                root / "bench" / "polyglot" / "python" / workload.python_source,
            ],
        ),
        (
            "ruby",
            [
                ruby,
                "--disable=gems",
                root / "bench" / "polyglot" / "ruby" / workload.ruby_source,
            ],
        ),
        ("cpp", [cpp_binary]),
    ]
    if go_unavailable_reason is None:
        programs.append(("go", [go_binary]))

    results = []
    for name, command in programs:
        samples = [measure(command, root) for _ in range(args.repeats)]
        results.append(
            aggregate(name, command, samples, workload.expected_checksum)
        )
    if go_unavailable_reason is not None:
        results.append(unavailable_result("go", go_unavailable_reason))

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
