#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate a nanolance checkout on your own machine, from scratch, and write one report to send back.

    python3 tools/local_validate.py                                  # everything but real S3
    python3 tools/local_validate.py --s3 s3://my-bucket/scratch/nl   # plus a real bucket
    python3 tools/local_validate.py --quick                          # skip the slow repeats

What it does, each step in its own log under .validate/<time>/ (nothing outside the checkout is changed):
  env        compilers, cmake, Python, uv, OS, CPU and memory, the commit and whether the tree is clean
  venv       a new virtualenv (from --python, default this interpreter); no global packages
  cpp        a clean CMake build (Release) and ctest
  install    the Python binding built in a new build directory (never bindings/python/build), editable,
             with its test dependencies
  pytest     the binding's tests, as CI runs them (with a local moto S3 server when moto installs)
  stress     the same tests with every read cut into 1 KiB pieces on 8 threads (skipped by --quick)
  pylance    pylance 12's own test suite against nanolance (tools/pylance_suite.py)
  lance_c    lance-c's C/C++ tests against nanolance's liblance_c (tools/lance_c_suite.py)
  s3         tools/s3_real_check.py --large --faults against the bucket prefix you give (--s3)

Real S3: credentials come from the usual AWS chain (AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY /
AWS_SESSION_TOKEN, AWS_PROFILE, AWS_REGION); --s3-endpoint / --s3-region / --s3-allow-http for MinIO, R2
and other S3-compatible stores. Use an empty scratch prefix: the check writes there and deletes what it
wrote. Secrets are never written to the report: the values of AWS_* secret variables are scrubbed from
everything it includes.

The report: .validate/<time>/report.txt (at most --max-kb, default 190 KB: attach it, or paste its first
section, which is short) and report.json. The full logs stay next to it in case more is needed.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SECRET_VARS = ("AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN", "AWS_ACCESS_KEY_ID", "AWS_SECURITY_TOKEN")
CI_EXTRA = ["pytest-xdist", "pytest-timeout", "pandas", "polars", "duckdb", "tqdm", "ml_dtypes", "pydantic",
            "pylance==12.0.0"]


def scrub(text: str) -> str:
    for name in SECRET_VARS:
        value = os.environ.get(name)
        if value and len(value) >= 4:
            text = text.replace(value, f"<{name}>")
    return text


class Run:
    def __init__(self, out: Path):
        self.out = out
        self.steps: list[dict] = []

    def sh(self, step: str, cmd, log: str, cwd=ROOT, env=None, timeout=None) -> tuple[int, float]:
        """Runs `cmd`, appending its output to `log`; returns (exit code, seconds)."""
        path = self.out / log
        start = time.time()
        with open(path, "a", encoding="utf-8", errors="replace") as f:
            f.write(f"\n$ {cmd if isinstance(cmd, str) else ' '.join(map(str, cmd))}\n")
            f.flush()
            try:
                p = subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT,
                                   shell=isinstance(cmd, str), timeout=timeout)
                code = p.returncode
            except subprocess.TimeoutExpired:
                f.write(f"\n[timed out after {timeout} s]\n")
                code = 124
            except OSError as e:
                f.write(f"\n[could not run: {e}]\n")
                code = 127
        return code, time.time() - start

    def record(self, name, status, seconds, log, detail=""):
        self.steps.append({"step": name, "status": status, "seconds": round(seconds, 1), "log": log,
                           "detail": detail})
        print(f"[{status:>4}] {name:<8} {seconds:7.1f} s  {detail}", flush=True)


def tool_version(cmd) -> str:
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return (p.stdout or p.stderr).strip().splitlines()[0] if (p.stdout or p.stderr).strip() else "?"
    except Exception as e:  # noqa: BLE001 - only reporting
        return f"not found ({type(e).__name__})"


def environment(python: str) -> dict:
    env = {
        "date_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "cpus": os.cpu_count(),
        "python_used_for_tests": tool_version([python, "--version"]),
        "python_running_this": sys.version.split()[0],
        "cmake": tool_version(["cmake", "--version"]),
        "ninja": tool_version(["ninja", "--version"]),
        "c++": tool_version([os.environ.get("CXX", "c++"), "--version"]),
        "cc": tool_version([os.environ.get("CC", "cc"), "--version"]),
        "g++": tool_version(["g++", "--version"]),
        "clang++": tool_version(["clang++", "--version"]),
        "uv": tool_version(["uv", "--version"]),
        "curl-config": tool_version(["curl-config", "--version"]),
        "git_commit": tool_version(["git", "-C", str(ROOT), "rev-parse", "HEAD"]),
        "git_branch": tool_version(["git", "-C", str(ROOT), "rev-parse", "--abbrev-ref", "HEAD"]),
        "env_CC_CXX_CMAKE": {k: os.environ[k] for k in ("CC", "CXX", "CMAKE_GENERATOR", "CMAKE_ARGS",
                                                          "CFLAGS", "CXXFLAGS", "LDFLAGS") if k in os.environ},
    }
    try:
        dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain"], capture_output=True,
                               text=True, timeout=30).stdout.splitlines()
        env["git_uncommitted"] = dirty[:30]
    except Exception:  # noqa: BLE001
        env["git_uncommitted"] = ["?"]
    try:
        with open("/proc/meminfo") as f:
            env["memory"] = f.readline().strip()
    except OSError:
        env["memory"] = tool_version(["sysctl", "-n", "hw.memsize"])
    return env


def junit_failures(path: Path) -> tuple[dict, list]:
    """Counts and [(test id, message)] from a pytest junit file."""
    counts = {"passed": 0, "failed": 0, "skipped": 0}
    failures = []
    try:
        cases = list(ET.parse(path).iter("testcase"))
    except (OSError, ET.ParseError):  # no report: pytest died before writing it (the log says why)
        return counts, failures
    for case in cases:
        tid = f"{case.get('classname', '')}::{case.get('name', '')}"
        state = "passed"
        for child in case:
            if child.tag in ("failure", "error"):
                state = "failed"
                failures.append((tid, (child.get("message") or "") + "\n" + (child.text or "")))
            elif child.tag == "skipped":
                state = "skipped"
        counts[state] += 1
    return counts, failures


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start_moto(run: Run, venv_root: Path) -> tuple[subprocess.Popen | None, str]:
    """A local moto S3 server (its own venv, as CI does), or None when moto cannot be installed."""
    motoenv = venv_root.parent / "motoenv"
    code, _ = run.sh("moto", [sys.executable, "-m", "venv", str(motoenv)], "moto.log")
    pip = motoenv / ("Scripts" if os.name == "nt" else "bin") / "pip"
    if code == 0:
        code, _ = run.sh("moto", [str(pip), "install", "--quiet", "moto[server]==5.2.4"], "moto.log", timeout=900)
    if code != 0:
        return None, "moto did not install (object-store tests skip)"
    port = free_port()
    server = motoenv / "bin" / "moto_server"
    log = open(run.out / "moto_server.log", "w")
    proc = subprocess.Popen([str(server), "-H", "127.0.0.1", "-p", str(port)], stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True)
    endpoint = f"http://127.0.0.1:{port}"
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    for _ in range(60):
        try:
            opener.open(endpoint + "/", timeout=2)
            break
        except Exception:  # noqa: BLE001
            time.sleep(1)
    try:
        opener.open(urllib.request.Request(endpoint + "/nl-test", method="PUT"), timeout=5)
    except Exception as e:  # noqa: BLE001
        proc.terminate()
        return None, f"moto did not start ({e})"
    return proc, endpoint


def tail(path: Path, max_bytes: int) -> str:
    try:
        data = path.read_bytes()
    except OSError:
        return "(no log)"
    text = data[-max_bytes:].decode("utf-8", "replace")
    return ("[... cut ...]\n" if len(data) > max_bytes else "") + text


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--python", default=sys.executable, help="interpreter for the venv (default: this one)")
    ap.add_argument("--s3", help="s3://bucket/prefix for the real-S3 checks (empty scratch prefix)")
    ap.add_argument("--s3-endpoint")
    ap.add_argument("--s3-region")
    ap.add_argument("--s3-allow-http", action="store_true")
    ap.add_argument("--quick", action="store_true", help="skip the stress repeat of the binding tests")
    ap.add_argument("--skip", default="", help="steps to skip, comma-separated (cpp,stress,pylance,lance_c,...)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--max-kb", type=int, default=190, help="cap on report.txt (default 190 KB)")
    args = ap.parse_args(argv)
    skip = {s.strip() for s in args.skip.split(",") if s.strip()}
    if args.quick:
        skip.add("stress")

    out = ROOT / ".validate" / dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True)
    run = Run(out)
    print(f"logs and report: {out}", flush=True)

    env_info = environment(args.python)
    (out / "env.json").write_text(json.dumps(env_info, indent=2))
    run.record("env", "ok", 0, "env.json", env_info["git_commit"][:12])

    # venv
    venv = out / "venv"
    code, secs = run.sh("venv", [args.python, "-m", "venv", str(venv)], "venv.log")
    py = str(venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python"))
    if code == 0:
        c2, s2 = run.sh("venv", [py, "-m", "pip", "install", "--quiet", "--upgrade", "pip"], "venv.log", timeout=900)
        code, secs = c2, secs + s2
    run.record("venv", "ok" if code == 0 else "FAIL", secs, "venv.log")
    if code != 0:
        return finish(run, env_info, args)

    base_env = dict(os.environ)
    base_env.pop("PYTHONPATH", None)
    base_env["PIP_NO_CACHE_DIR"] = "1"

    # C++ build and ctest
    if "cpp" not in skip:
        build = out / "build-cpp"
        code, secs = run.sh("cpp", ["cmake", "-S", str(ROOT), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"],
                            "cpp.log", env=base_env)
        if code == 0:
            c2, s2 = run.sh("cpp", ["cmake", "--build", str(build), "-j", str(args.jobs)], "cpp.log", env=base_env)
            code, secs = c2, secs + s2
        detail = "build failed" if code != 0 else ""
        if code == 0:
            c2, s2 = run.sh("cpp", ["ctest", "--test-dir", str(build), "-j", str(args.jobs), "--output-on-failure",
                                    "--timeout", "900"], "cpp.log", env=base_env)
            code, secs = c2, secs + s2
            m = re.findall(r"(\d+)% tests passed, (\d+) tests failed out of (\d+)", (out / "cpp.log").read_text(errors="replace"))
            detail = f"ctest: {m[-1][1]} failed of {m[-1][2]}" if m else "ctest did not report"
        run.record("cpp", "ok" if code == 0 else "FAIL", secs, "cpp.log", detail)

    # The binding: a new build directory, editable (the tests import tests/ next to it), with test deps.
    pybuild = out / "build-python"
    code, secs = run.sh("install", [py, "-m", "pip", "install", "--no-cache-dir", "-v", "-e",
                                    str(ROOT / "bindings" / "python") + "[test]",
                                    "-C", f"build-dir={pybuild}"], "install.log", env=base_env, timeout=3600)
    if code == 0:
        c2, s2 = run.sh("install", [py, "-m", "pip", "install", "--quiet"] + CI_EXTRA, "install.log", env=base_env,
                        timeout=1800)
        code, secs = c2, secs + s2
    if code == 0:
        run.sh("install", [py, "-m", "pip", "freeze"], "install.log", env=base_env)
        c2, s2 = run.sh("install", [py, "-c", "import nanolance, nanolance.lance as nl, lance; "
                                    "print('nanolance', nanolance.__file__); print('pylance', lance.__version__)"],
                        "install.log", env=base_env)
        code, secs = c2, secs + s2
    run.record("install", "ok" if code == 0 else "FAIL", secs, "install.log")
    if code != 0:
        return finish(run, env_info, args)

    moto, endpoint = (None, "")
    if "moto" not in skip and "pytest" not in skip:
        moto, endpoint = start_moto(run, venv)
        run.record("moto", "ok" if moto else "skip", 0, "moto.log", endpoint)
    test_env = dict(base_env)
    if moto:
        test_env.update({"NANOLANCE_S3_TEST_ENDPOINT": endpoint, "NO_PROXY": "127.0.0.1,localhost",
                         "no_proxy": "127.0.0.1,localhost"})
    pydir = ROOT / "bindings" / "python"
    try:
        for name, extra in (("pytest", {}), ("stress", {"NANOLANCE_THREADS": "8", "NANOLANCE_MORSEL_KB": "1"})):
            if name in skip:
                continue
            junit = out / f"{name}.xml"
            e = dict(test_env)
            e.update(extra)
            if moto:
                # The object-store tests talk to moto with moto's own test credentials.
                e.update({"AWS_ACCESS_KEY_ID": "testing", "AWS_SECRET_ACCESS_KEY": "testing", "AWS_REGION": "us-east-1"})
                e.pop("AWS_SESSION_TOKEN", None)
                e.pop("AWS_PROFILE", None)
            # Its own temporary directory, removed afterwards: a run leaves gigabytes of test datasets.
            basetemp = out / f"tmp-{name}"
            code, secs = run.sh(name, [py, "-m", "pytest", "-m", "not bench", "-q", "-rs", "-o", "addopts=", "-p",
                                       "no:cacheprovider", "--timeout", "1800", f"--basetemp={basetemp}",
                                       f"--junitxml={junit}", "tests"],
                                f"{name}.log", cwd=pydir, env=e, timeout=4 * 3600)
            shutil.rmtree(basetemp, ignore_errors=True)
            counts, _ = junit_failures(junit)
            detail = f"{counts['passed']} passed, {counts['failed']} failed, {counts['skipped']} skipped"
            if not any(counts.values()):
                detail = "no test report (see the end of the log)"
            run.record(name, "ok" if code == 0 else "FAIL", secs, f"{name}.log", detail)
    finally:
        if moto:
            os.killpg(moto.pid, signal.SIGTERM)

    if "pylance" not in skip:
        # TMPDIR, not --basetemp: any extra pytest argument turns off the suite's expected-pass verdict.
        (out / "tmp-pylance").mkdir()
        code, secs = run.sh("pylance", [py, str(ROOT / "tools" / "pylance_suite.py"), "--report",
                                        str(out / "pylance-suite.json")], "pylance.log",
                            env=dict(base_env, TMPDIR=str(out / "tmp-pylance")), timeout=4 * 3600)
        shutil.rmtree(out / "tmp-pylance", ignore_errors=True)
        last = [l for l in (out / "pylance.log").read_text(errors="replace").splitlines() if "test suite against" in l]
        run.record("pylance", "ok" if code == 0 else "FAIL", secs, "pylance.log", last[-1].strip() if last else "")

    if "lance_c" not in skip:
        code, secs = run.sh("lance_c", [py, str(ROOT / "tools" / "lance_c_suite.py"), "--build-dir",
                                        str(out / "build-lance-c")], "lance_c.log", env=base_env, timeout=3600)
        lines = (out / "lance_c.log").read_text(errors="replace").strip().splitlines()
        run.record("lance_c", "ok" if code == 0 else "FAIL", secs, "lance_c.log", lines[-1][:120] if lines else "")

    if args.s3 and "s3" not in skip:
        cmd = [py, str(ROOT / "tools" / "s3_real_check.py"), args.s3, "--large", "--faults", "--report",
               str(out / "s3-check.json")]
        if args.s3_endpoint:
            cmd += ["--endpoint", args.s3_endpoint]
        if args.s3_region:
            cmd += ["--region", args.s3_region]
        if args.s3_allow_http:
            cmd += ["--allow-http"]
        code, secs = run.sh("s3", cmd, "s3.log", env=base_env, timeout=4 * 3600)
        lines = [l for l in (out / "s3.log").read_text(errors="replace").splitlines() if l.strip()]
        run.record("s3", "ok" if code == 0 else "FAIL", secs, "s3.log", lines[-1][:120] if lines else "")
    elif "s3" not in skip:
        run.record("s3", "skip", 0, "", "no --s3 prefix given")

    return finish(run, env_info, args)


def finish(run: Run, env_info: dict, args) -> int:
    out = run.out
    budget = args.max_kb * 1024
    parts = []
    failed = [s for s in run.steps if s["status"] == "FAIL"]
    head = [f"nanolance local validation: {'ALL OK' if not failed else str(len(failed)) + ' step(s) FAILED'}",
            f"commit {env_info.get('git_commit', '?')} ({env_info.get('git_branch', '?')})", ""]
    for s in run.steps:
        head.append(f"  {s['status']:>4}  {s['step']:<8} {s['seconds']:8.1f} s  {s['detail']}")
    head += ["", "environment:"] + [f"  {k}: {v}" for k, v in env_info.items()]
    parts.append("\n".join(head))
    # Test failures, from the junit files; then the end of each failed step's log.
    details = []
    for name in ("pytest", "stress"):
        _, failures = junit_failures(out / f"{name}.xml")
        if failures:
            details.append(f"\n===== {name}: {len(failures)} failing tests =====")
            details += [f"  {tid}" for tid, _ in failures]
            for tid, text in failures[:60]:
                details.append(f"\n--- {name} {tid}\n{text[-4000:]}")
    for name in ("pytest", "stress"):
        skipped = [l for l in tail(out / f"{name}.log", 200000).splitlines() if l.startswith("SKIPPED")]
        if skipped:
            details.append(f"\n===== {name}: skip reasons =====\n" + "\n".join(skipped[:80]))
    for path, label in ((out / "pylance-suite.json", "pylance suite report"), (out / "s3-check.json", "S3 check report")):
        if path.exists():
            details.append(f"\n===== {label} =====\n{path.read_text(errors='replace')[:40000]}")
    for s in failed:
        if s["log"]:
            details.append(f"\n===== end of {s['log']} =====\n{tail(out / s['log'], 40000)}")
    text = scrub(parts[0] + "\n" + "\n".join(details))
    if len(text.encode()) > budget:
        text = text.encode()[:budget - 200].decode("utf-8", "ignore") + \
            f"\n\n[report cut at {args.max_kb} KB; the full logs are in {out}]\n"
    (out / "report.txt").write_text(text)
    (out / "report.json").write_text(scrub(json.dumps({"steps": run.steps, "env": env_info}, indent=2)))
    print(f"\nreport: {out / 'report.txt'} ({len(text.encode()) // 1024} KB)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
