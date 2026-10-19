"""Shared plumbing for the index benchmarks: nanolance's C++ driver (tools/nlbench_index.cpp) and the Rust
Lance driver (tools/lance_rs_bench, the `index` subcommand) run the same workload file over the same
base dataset. Python generates the data and reads the results; it times nothing.

Protocol (both drivers): a build runs on a fresh copy of the unindexed base, timed until the timed total
reaches min_seconds (at least three builds); a query repeats until its timed total reaches min_seconds
(at least 20 calls, or 5 passes over the vector or full-text query set). Medians are reported with the
minimum and the sample count.
"""

from __future__ import annotations

import json
import os
import platform
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUST_BIN = ROOT / "tools" / "lance_rs_bench" / "target" / "release" / "lance_rs_bench"
CPP_BIN = Path(os.environ.get("NANOLANCE_BUILD_DIR", ROOT / "build")) / "nlbench_index"

# Each engine's command: the Rust driver's index subcommand, and nanolance's C++ driver.
RUST_CMD = [str(RUST_BIN), "index"]
CPP_CMD = [str(CPP_BIN)]


def write_workload(path: Path, lines: list[str], min_seconds: float) -> Path:
    path.write_text("\n".join(["min_seconds\t%g" % min_seconds, *lines]) + "\n")
    return path


def run_driver(prefix: list[str], section: str, workload: Path, work: Path, base: Path, out: Path) -> dict:
    """One driver's JSON for one section. A driver that fails stops the benchmark."""
    cmd = [*prefix, str(workload), section, str(work), str(base), str(out)]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        raise SystemExit(f"{' '.join(prefix)} {section} failed:\n{done.stderr}")
    return json.loads(out.read_text())


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True).stdout.strip()


def environment(section: str) -> dict:
    commit = git("rev-parse", "--short", "HEAD")
    if git("status", "--porcelain", "--untracked-files=no"):
        commit += " with uncommitted changes"
    cpu = ""
    try:
        with open("/proc/cpuinfo") as f:
            cpu = next((line.split(":", 1)[1].strip() for line in f if line.startswith("model name")), "")
    except OSError:
        pass
    return {
        "section": section,
        "nanolance_commit": commit,
        "rust_lance": "12.0.0 (the lance crate, no Python in the process)",
        "cpu": cpu or platform.processor() or platform.machine(),
        "cores": os.cpu_count(),
        "machine": platform.machine(),
        "date": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
    }


def check_drivers() -> None:
    for binary in (RUST_BIN, CPP_BIN):
        if not binary.exists():
            raise SystemExit(f"missing {binary}: build it first (see docs/PERFORMANCE_GATE.md)")


def combine_stats(rust: dict, cpp: dict) -> dict:
    """{'rust': stat, 'nanolance': stat} for one named aspect."""
    return {"rust": rust, "nanolance": cpp}
