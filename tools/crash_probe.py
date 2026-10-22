#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Narrow down a crash in vector index builds on one machine, and write one small report to send back.

    python3 tools/crash_probe.py                     # about 10-20 minutes
    python3 tools/crash_probe.py --cxx g++-12        # also try another compiler

It builds the Python binding several ways, each in a new build directory, and runs the vector-index tests
against each build:
  default     as tools/local_validate.py builds it
  no-clones   -DNANOLANCE_TARGET_CLONES=OFF: the k-means kernel built without its AVX2 variant
  threads1    the default build, every operation on one thread (NANOLANCE_THREADS=1)
  no-avx2     the default build with the k-means kernel's AVX2 build turned off (NANOLANCE_NO_AVX2=1)
  cxx         the default build with --cxx (only when given)
It also prints the native backtrace of the first crash under gdb, when gdb is installed, and which
libstdc++ / libgcc the process loads. Report: .validate/probe-<time>/probe.txt (at most 60 KB).
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ["tests/test_data_overlay.py", "tests/test_hnsw.py", "tests/test_vector_search.py", "tests/test_index_optimize.py",
         "tests/test_ivf_rq.py", "tests/test_binary_ivf.py", "tests/test_float16_vectors.py", "tests/test_multivector.py"]
FIRST = "tests/test_data_overlay.py::test_stale_vector_index_is_not_trusted"


def run(cmd, log: Path, cwd=ROOT, env=None, timeout=3600) -> int:
    with open(log, "a", encoding="utf-8", errors="replace") as f:
        f.write(f"\n$ {' '.join(map(str, cmd))}\n")
        f.flush()
        try:
            return subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            f.write("\n[timed out]\n")
            return 124


def tail(path: Path, n: int) -> str:
    data = path.read_text(errors="replace") if path.exists() else ""
    return data[-n:]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--cxx", help="another C++ compiler to try (e.g. g++-12, /path/to/g++)")
    args = ap.parse_args()
    out = ROOT / ".validate" / ("probe-" + dt.datetime.now().strftime("%Y%m%d-%H%M%S"))
    out.mkdir(parents=True)
    print(f"logs and report: {out}", flush=True)
    report = [f"crash probe at {subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()}"]
    for tool in (["c++", "--version"], [os.environ.get("CXX", "c++"), "-print-file-name=libstdc++.so.6"],
                 ["gcc", "-print-libgcc-file-name"], ["gdb", "--version"], ["ldd", "--version"]):
        try:
            p = subprocess.run(tool, capture_output=True, text=True, timeout=30)
            report.append(f"{' '.join(tool)}: {(p.stdout or p.stderr).strip().splitlines()[0]}")
        except Exception as e:  # noqa: BLE001
            report.append(f"{' '.join(tool)}: not found ({type(e).__name__})")
    report.append("LD_LIBRARY_PATH=" + os.environ.get("LD_LIBRARY_PATH", ""))
    lscpu = subprocess.run(["sh", "-c", "lscpu | grep -E 'Model name|^CPU\\(s\\)|Flags' | cut -c1-600"],
                           capture_output=True, text=True).stdout
    report.append(lscpu.strip())

    venv = out / "venv"
    run([args.python, "-m", "venv", str(venv)], out / "setup.log")
    py = str(venv / "bin" / "python")
    env = dict(os.environ, PIP_NO_CACHE_DIR="1")
    env.pop("PYTHONPATH", None)
    run([py, "-m", "pip", "install", "--quiet", "--upgrade", "pip"], out / "setup.log", env=env)
    first = True
    variants = [("default", {}, []), ("no-clones", {}, ["-C", "cmake.define.NANOLANCE_TARGET_CLONES=OFF"]),
                ("threads1", {"NANOLANCE_THREADS": "1"}, None), ("no-avx2", {"NANOLANCE_NO_AVX2": "1"}, None)]
    if args.cxx:
        variants.append(("cxx", {"CXX": args.cxx}, []))
    pydir = ROOT / "bindings" / "python"
    for name, extra_env, build_args in variants:
        log = out / f"{name}.log"
        e = dict(env, **extra_env)
        if build_args is not None:  # a build of its own (threads1 reuses the default build)
            spec = str(pydir) + ("[test]" if first else "")
            cmd = [py, "-m", "pip", "install", "--no-cache-dir", "-e", spec, "-C", f"build-dir={out / ('build-' + name)}"]
            if not first:
                cmd.insert(5, "--no-deps")
            code = run(cmd + build_args, log, env=e)
            first = False
            if code != 0:
                report.append(f"\n== {name}: BUILD FAILED\n{tail(log, 6000)}")
                continue
        if build_args is None:
            run([py, "-m", "pip", "install", "--no-cache-dir", "--no-deps", "-e", str(pydir), "-C",
                 f"build-dir={out / 'build-default'}"], log, env=e)
        basetemp = out / f"tmp-{name}"
        code = run([py, "-X", "faulthandler", "-m", "pytest", "-q", "-o", "addopts=", "-p", "no:cacheprovider",
                    f"--basetemp={basetemp}", "-x", "-W", "ignore"] + TESTS, log, cwd=pydir, env=e, timeout=3600)
        shutil.rmtree(basetemp, ignore_errors=True)
        verdict = "PASS" if code == 0 else ("CRASH" if code < 0 or "Fatal Python error" in tail(log, 200000) else "FAIL")
        summary = [l for l in tail(log, 200000).splitlines() if " passed" in l or " failed" in l]
        report.append(f"\n== {name}: {verdict} (exit {code}) {summary[-1] if summary else ''}")
        if verdict != "PASS":
            report.append(tail(log, 5000))
        if name == "default" and verdict == "CRASH" and shutil.which("gdb"):
            glog = out / "gdb.log"
            run(["gdb", "-batch", "-ex", "run", "-ex", "bt 40", "-ex", "info sharedlibrary", "--args", py, "-m", "pytest",
                 "-q", "-o", "addopts=", "-p", "no:cacheprovider", "-x", f"--basetemp={out / 'tmp-gdb'}", FIRST],
                glog, cwd=pydir, env=e, timeout=1800)
            text = tail(glog, 400000)
            keep = [l for l in text.splitlines() if l.startswith("#") or "libstdc++" in l or "libgcc" in l
                    or "_nanolance" in l or "signal" in l.lower()]
            report.append("\n== gdb backtrace of " + FIRST + "\n" + "\n".join(keep[:200]))
            shutil.rmtree(out / "tmp-gdb", ignore_errors=True)
    text = "\n".join(report)
    for secret in ("AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN", "AWS_ACCESS_KEY_ID"):
        if os.environ.get(secret):
            text = text.replace(os.environ[secret], f"<{secret}>")
    text = text[:60000]
    (out / "probe.txt").write_text(text)
    print(text[:3000])
    print(f"\nreport: {out / 'probe.txt'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
