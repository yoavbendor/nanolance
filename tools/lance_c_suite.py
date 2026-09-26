#!/usr/bin/env python3
"""Run lance-c's own C and C++ tests against nanolance's liblance_c (lance-c's C API over nanolance).

    python tools/lance_c_suite.py                  # fetch (once), build, run; compare with the list
    python tools/lance_c_suite.py --update         # ... and rewrite the list of tests that pass
    python tools/lance_c_suite.py --build-dir build

lance-c's tests (tests/cpp/test_c_api.c and test_cpp_api.cpp, at the lance-c commit whose header
compat/lance-c/ vendors) are fetched with git into .deps/lance-c-tests/, not copied into this
repository. Each is one program that stops at its first failed assertion, so a generated driver
#includes it and runs every test function in a child process of its own, in the order lance-c's
main() does (later tests use the dataset earlier ones wrote). Both are built against liblance_c
with NDEBUG undefined, so their assert()s are live.

The datasets they read are built the way lance-c's harness builds them (tools/lance_c_fixtures.py),
once written by pylance and once by nanolance.lance. tests/lance_c/expected_pass.txt lists
"<c|cpp>/<writer>::<test>" entries that pass; a run fails when one of them does not.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = ROOT / "tests" / "lance_c" / "expected_pass.txt"
REPO = "https://github.com/lance-format/lance-c.git"


def fetch(commit: str, cache: Path) -> Path:
    tests = cache / commit / "tests" / "cpp"
    if (tests / "test_c_api.c").exists():
        return tests
    checkout = cache / f"{commit}-checkout"
    shutil.rmtree(checkout, ignore_errors=True)
    checkout.mkdir(parents=True)
    git = ["git", "-C", str(checkout)]
    subprocess.run(git + ["init", "-q"], check=True)
    subprocess.run(git + ["remote", "add", "origin", REPO], check=True)
    subprocess.run(git + ["fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", commit], check=True)
    subprocess.run(git + ["sparse-checkout", "set", "tests/cpp"], check=True)
    subprocess.run(git + ["checkout", "-q", "FETCH_HEAD"], check=True)
    tests.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(checkout / "tests" / "cpp", tests)
    shutil.rmtree(checkout, ignore_errors=True)
    return tests


def calls_of_main(source: str) -> list:
    body = source[source.index("int main("):]
    return re.findall(r"^\s*(test_\w+)\(([^)]*)\);", body, flags=re.M)


def driver(test_file: str, calls: list, cpp: bool) -> str:
    """A main() that runs each of the file's tests in a forked child and prints one RESULT line."""
    args = {"uri": "argv[1]", "write_uri": "argv[2]", "blob_uri": "argv[3]", "": ""}
    lines = [
        "#undef NDEBUG",
        "#define main lance_c_upstream_main",
        f'#include "{test_file}"',
        "#undef main",
        "#include <stdio.h>",
        "#include <stdlib.h>",
        "#include <string.h>",
        "#include <sys/wait.h>",
        "#include <unistd.h>",
        "static int run(const char* name, const char* only) { return only == NULL || strcmp(only, name) == 0; }",
        "static void report(const char* name, pid_t pid) {",
        "    int status = 0;",
        "    waitpid(pid, &status, 0);",
        '    const char* verdict = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "PASS" : "FAIL";',
        '    printf("RESULT %s %s\\n", name, verdict);',
        "    fflush(stdout);",
        "}",
        "int main(int argc, char** argv) {",
        '    if (argc < 4) { fprintf(stderr, "usage: %s <dataset_uri> <write_uri> <blob_uri> [test]\\n", argv[0]); return 2; }',
        "    const char* only = argc > 4 ? argv[4] : NULL;",
        "    fflush(stdout);",
    ]
    for name, raw in calls:
        params = [p.strip() for p in raw.split(",") if p.strip()]
        call_args = ", ".join(args.get(p, p) for p in params)
        if cpp:
            call_args = ", ".join(f"std::string({a})" for a in [args.get(p, p) for p in params])
        lines += [
            f'    if (run("{name}", only)) {{',
            "        pid_t pid = fork();",
            f"        if (pid == 0) {{ {name}({call_args}); fflush(stdout); _exit(0); }}",
            f'        report("{name}", pid);',
            "    }",
        ]
    lines += ["    return 0;", "}"]
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", type=Path, default=ROOT / "build")
    ap.add_argument("--cache", type=Path, default=ROOT / ".deps" / "lance-c-tests")
    ap.add_argument("--writers", default="pylance,nanolance")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true", help="show each test's own output")
    args = ap.parse_args(argv)

    commit = (ROOT / "compat" / "lance-c" / "UPSTREAM").read_text().strip()
    tests = fetch(commit, args.cache)
    for name, cpp in (("test_c_api.c", False), ("test_cpp_api.cpp", True)):
        source = (tests / name).read_text()
        out = tests / ("driver_cpp.cpp" if cpp else "driver_c.c")
        out.write_text(driver(name, calls_of_main(source), cpp))
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(args.build_dir), f"-DNANOLANCE_LANCE_C_UPSTREAM_TESTS={tests}"],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(args.build_dir), "-j", str(os.cpu_count() or 2), "--target",
                    "lance_c_upstream_c", "lance_c_upstream_cpp"], check=True)

    results = {}
    for writer in args.writers.split(","):
        with tempfile.TemporaryDirectory(prefix=f"lance-c-{writer}-") as tmp:
            subprocess.run([sys.executable, str(ROOT / "tools" / "lance_c_fixtures.py"), tmp, "--writer", writer],
                           check=True)
            for kind in ("c", "cpp"):
                write_uri = os.path.join(tmp, f"write_{kind}")
                exe = args.build_dir / f"lance_c_upstream_{kind}"
                run = subprocess.run([str(exe), os.path.join(tmp, "c_test_ds"), write_uri, os.path.join(tmp, "blob_ds")],
                                     capture_output=True, text=True)
                if args.verbose:
                    print(run.stdout, run.stderr)
                for line in run.stdout.splitlines():
                    if line.startswith("RESULT "):
                        _, test, verdict = line.split()
                        results[f"{kind}/{writer}::{test}"] = verdict == "PASS"
                failures = [l for l in run.stderr.splitlines() if l.startswith("FAIL")]
                if failures and args.verbose:
                    print("\n".join(failures))

    passed = sorted(k for k, ok in results.items() if ok)
    print(f"lance-c {commit[:7]} C/C++ tests against nanolance's liblance_c: {len(passed)} of {len(results)} pass")
    for key in sorted(results):
        print(f"  {'PASS' if results[key] else 'fail'}  {key}")
    expected = set()
    if EXPECTED.exists():
        expected = {l.strip() for l in EXPECTED.read_text().splitlines() if l.strip() and not l.startswith("#")}
    if args.update:
        EXPECTED.parent.mkdir(parents=True, exist_ok=True)
        EXPECTED.write_text(f"# lance-c {commit} tests that pass against nanolance (tools/lance_c_suite.py --update)\n"
                            + "\n".join(passed) + "\n")
        print(f"wrote {len(passed)} tests to {EXPECTED.relative_to(ROOT)}")
        return 0
    lost = sorted(k for k in expected if not results.get(k))
    if lost:
        print(f"\n{len(lost)} test(s) expected to pass did not:\n  " + "\n  ".join(lost))
        return 1
    print(f"all {len(expected)} expected tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
