#!/usr/bin/env python3
"""Run pylance's own test suite against nanolance's pylance-compatible module (nanolance.lance).

    python tools/pylance_suite.py                 # fetch the tests (once), run, compare with the list
    python tools/pylance_suite.py --update        # ... and rewrite the list of tests expected to pass
    python tools/pylance_suite.py --real-pylance  # the same tests against pylance itself (baseline)
    python tools/pylance_suite.py -k test_take    # extra arguments go to pytest

The tests come from the Lance repository at the tag of the pylance release nanolance.lance tracks
(PYLANCE_API_VERSION), fetched with a sparse git checkout into .deps/pylance-tests/ -- they are not
copied into this repository. pytest runs them with nanolance installed as the ``lance`` module
(nanolance/lance/_pytest_plugin.py).

bindings/python/tests/pylance_suite/expected_pass.txt lists the tests that pass. A run fails when one
of them does not, so the list only grows; a test that newly passes is reported, for --update to add.
The summary (passed / failed / skipped per test file) is written to --report as JSON.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = ROOT / "bindings" / "python" / "tests" / "pylance_suite" / "expected_pass.txt"
REPO = "https://github.com/lance-format/lance.git"


def api_version() -> str:
    text = (ROOT / "bindings" / "python" / "nanolance" / "lance" / "__init__.py").read_text()
    for line in text.splitlines():
        if line.startswith("PYLANCE_API_VERSION"):
            return line.split("=")[1].strip().strip('"')
    raise SystemExit("PYLANCE_API_VERSION not found")


def fetch(version: str, cache: Path) -> Path:
    tests = cache / f"v{version}" / "tests"
    if tests.is_dir():
        return tests
    checkout = cache / f"v{version}-checkout"
    shutil.rmtree(checkout, ignore_errors=True)
    subprocess.run(["git", "clone", "-q", "--depth", "1", "--filter=blob:none", "--sparse", "--branch",
                    f"v{version}", REPO, str(checkout)], check=True)
    subprocess.run(["git", "-C", str(checkout), "sparse-checkout", "set", "python/python/tests"], check=True)
    tests.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(checkout / "python" / "python" / "tests", tests)
    shutil.rmtree(checkout, ignore_errors=True)
    return tests


def outcomes(junit: Path) -> dict:
    result = {}
    for case in ET.parse(junit).iter("testcase"):
        classname = case.get("classname", "")
        name = case.get("name", "")
        module = classname.split(".")
        # tests.test_x.TestClass -> tests/test_x.py::TestClass::name
        parts = [p for p in module if p]
        path = []
        rest = []
        for p in parts:
            if not rest and not p[:1].isupper():
                path.append(p)
            else:
                rest.append(p)
        node = "/".join(path) + ".py::" + "::".join(rest + [name])
        state = "passed"
        for child in case:
            if child.tag in ("failure", "error"):
                state = "failed"
            elif child.tag == "skipped":
                state = "skipped"
        result[node] = state
    return result


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update", action="store_true", help="rewrite the expected-pass list")
    ap.add_argument("--real-pylance", action="store_true", help="run against pylance itself")
    ap.add_argument("--cache", type=Path, default=ROOT / ".deps" / "pylance-tests")
    ap.add_argument("--report", type=Path, default=None, help="write a JSON summary here")
    ap.add_argument("--workers", default="auto", help="pytest-xdist workers (0 for none)")
    ap.add_argument("--timeout", type=int, default=300)
    args, extra = ap.parse_known_args(argv)

    version = api_version()
    tests = fetch(version, args.cache)
    work = tests.parent
    junit = work / ("real.xml" if args.real_pylance else "nanolance.xml")
    cmd = [sys.executable, "-m", "pytest", "tests", "-q", "-p", "no:cacheprovider", f"--junitxml={junit}",
           "-o", "junit_family=xunit1"]
    if not args.real_pylance:
        cmd += ["-p", "nanolance.lance._pytest_plugin"]
    try:
        import xdist  # noqa: F401

        if args.workers != "0":
            cmd += ["-n", args.workers]
    except ImportError:
        pass
    try:
        import pytest_timeout  # noqa: F401

        cmd += ["--timeout", str(args.timeout)]
    except ImportError:
        pass
    cmd += extra
    print("+", " ".join(cmd), flush=True)
    env = dict(os.environ)
    subprocess.run(cmd, cwd=work, env=env)
    if not junit.exists():
        print("pytest produced no report", file=sys.stderr)
        return 2
    got = outcomes(junit)
    counts = Counter(got.values())
    by_file = defaultdict(Counter)
    for node, state in got.items():
        by_file[node.split("::")[0]][state] += 1
    print(f"\npylance {version} test suite against {'pylance' if args.real_pylance else 'nanolance.lance'}: "
          f"{counts['passed']} passed, {counts['failed']} failed, {counts['skipped']} skipped "
          f"of {len(got)}")
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps({
            "pylance_version": version,
            "target": "pylance" if args.real_pylance else "nanolance.lance",
            "totals": dict(counts),
            "files": {f: dict(c) for f, c in sorted(by_file.items())},
        }, indent=1) + "\n")
    if args.real_pylance or extra:
        return 0
    passed = sorted(n for n, s in got.items() if s == "passed")
    expected = set()
    if EXPECTED.exists():
        expected = {line.strip() for line in EXPECTED.read_text().splitlines() if line.strip() and not line.startswith("#")}
    lost = sorted(n for n in expected if got.get(n) != "passed")
    new = sorted(set(passed) - expected)
    if new:
        print(f"{len(new)} test(s) pass that the list does not have yet (--update adds them)")
    if args.update:
        EXPECTED.parent.mkdir(parents=True, exist_ok=True)
        EXPECTED.write_text(
            f"# pylance {version} tests that pass against nanolance.lance (tools/pylance_suite.py --update)\n"
            + "\n".join(passed) + "\n")
        print(f"wrote {len(passed)} tests to {EXPECTED.relative_to(ROOT)}")
        return 0
    if lost:
        print(f"\n{len(lost)} test(s) expected to pass did not:")
        for n in lost[:50]:
            print(f"  - {n}: {got.get(n, 'not run')}")
        return 1
    print(f"all {len(expected)} expected tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
