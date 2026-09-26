#!/usr/bin/env python3
"""Run the Lance Rust encoding tests with every round trip also read back by nanolance.

    python tools/rust_suite.py                 # fetch (once), build, run; compare with the list
    python tools/rust_suite.py --update        # ... and rewrite the list of tests that pass
    python tools/rust_suite.py --filter struct # only tests whose name contains "struct"

lance-encoding's tests check the Rust encoder and decoder against each other: each test encodes some
data -- every type, null pattern, page size, encoding choice and compression setting the Lance
authors thought worth testing -- decodes it whole, by row ranges and by row indices, and compares.
This runs those tests unchanged, with one addition (tests/rust_suite/nanolance_hook.rs): each round
trip's pages are also wrapped into a Lance 2.2 file and read by nanolance, the same three ways, and
compared with what the test expects. So every encoding the Rust writer can produce is a read test for
nanolance.

The crate is the one pylance 12.0.0 is built from, downloaded from crates.io into .deps/rust-suite/,
not copied into this repository. It is patched there: the hook module is added, the check function
calls it, and the test binary links libnanolance_rust_shim (tests/rust_suite/nanolance_rust_shim.cpp),
built here with CMake.

Per test, nanolance's result is the worst over its round trips:

    pass      every round trip read back equal, as the Rust decoder is required to
    type      equal values, but a different Arrow type (e.g. another name for a list's item)
    refused   nanolance declined some round trip with an error (an encoding it does not read)
    mismatch  nanolance returned different rows -- a wrong result, the one outcome never acceptable

tests/rust_suite/expected_pass.txt lists the tests whose round trips all pass (or differ only in
type). A run fails when one of them does not, or when any round trip is a mismatch that the list of
known mismatches (tests/rust_suite/known_mismatch.txt) does not name. The summary goes to --report.
Tests with no round trip (unit tests of one component) are counted but not listed.
"""

from __future__ import annotations

import argparse
import collections
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SUITE = ROOT / "tests" / "rust_suite"
EXPECTED = SUITE / "expected_pass.txt"
KNOWN_MISMATCH = SUITE / "known_mismatch.txt"
VERSION = "12.0.0"  # the lance release pylance 12.0.0 is built from
CRATES = ("lance-encoding", "lance-file")
RANK = {"pass": 0, "type": 1, "refused": 2, "mismatch": 3}


def fetch(crate: str, cache: Path) -> Path:
    """The crate's source, as published, in a fresh directory (patched by patch())."""
    src = cache / f"{crate}-{VERSION}"
    if src.exists():
        shutil.rmtree(src)
    cache.mkdir(parents=True, exist_ok=True)
    tarball = cache / f"{crate}-{VERSION}.crate"
    if not tarball.exists():
        url = f"https://static.crates.io/crates/{crate}/{crate}-{VERSION}.crate"
        print(f"downloading {url}")
        with urllib.request.urlopen(url, timeout=120) as r:
            tarball.write_bytes(r.read())
    with tarfile.open(tarball) as tar:
        tar.extractall(cache, filter="data")
    return src


def replace_once(text: str, old: str, new: str, what: str) -> str:
    if text.count(old) != 1:
        sys.exit(f"cannot patch {what}: the anchor is not there exactly once (has the crate changed?)")
    return text.replace(old, new)


def patch(crate: str, src: Path) -> None:
    """Add the hook modules to the crate's `testing` module and call them where the tests check."""
    hook = "nanolance_encoding_hook" if crate == "lance-encoding" else "nanolance_file_hook"
    for name in ("nanolance_hook", hook):
        shutil.copy(SUITE / f"{name}.rs", src / "src" / f"{name}.rs")
    modules = "".join(f'#[path = "{n}.rs"]\nmod {n};\n' for n in ("nanolance_hook", hook))
    testing = src / "src" / "testing.rs"
    t = testing.read_text()
    t = replace_once(t, "\nuse arrow_array::", "\n" + modules + "\nuse arrow_array::", f"{crate} testing.rs (modules)")
    if crate == "lance-encoding":
        t = replace_once(
            t,
            "    let scheduler = Arc::new(SimulatedScheduler::new(encoded_data)) as Arc<dyn EncodingsIo>;",
            "    let nanolance_bytes = encoded_data.clone();\n"
            "    let scheduler = Arc::new(SimulatedScheduler::new(encoded_data)) as Arc<dyn EncodingsIo>;",
            "testing.rs (encoded bytes)",
        )
        t = replace_once(
            t,
            "    let expected_data = expected_override.clone().or_else(|| concat_data.clone());\n",
            "    let expected_data = expected_override.clone().or_else(|| concat_data.clone());\n"
            f"    {hook}::check(\n"
            "        field, &nanolance_bytes, &column_infos, num_rows, expected_data.as_ref(), concat_data.as_ref(),\n"
            "        &test_cases.ranges, &test_cases.indices, &encoding.to_string(),\n"
            "    );\n",
            "testing.rs (check)",
        )
    else:
        t = replace_once(
            t,
            "    file_writer.finish().await.unwrap();\n",
            "    file_writer.finish().await.unwrap();\n"
            f"    {hook}::check(&fs.tmp_path, &data, version);\n",
            "testing.rs (check)",
        )
    testing.write_text(t)
    cargo = src / "Cargo.toml"
    c = cargo.read_text()
    for dep in ("arrow-array", "arrow-schema"):
        c = replace_once(c, f'[dependencies.{dep}]\nversion = "58.0.0"\n',
                         f'[dependencies.{dep}]\nversion = "58.0.0"\nfeatures = ["ffi"]\n', f"{crate} Cargo.toml ({dep})")
    cargo.write_text(c)


def build_shim(build_dir: Path) -> Path:
    steps = [
        ["cmake", "-S", str(ROOT), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", "-DNANOLANCE_BUILD_RUST_SHIM=ON", "-DNANOLANCE_BUILD_TESTS=OFF",
         "-DNANOLANCE_BUILD_TOOLS=OFF", "-DNANOLANCE_BUILD_EXAMPLES=OFF", "-DNANOLANCE_BUILD_LANCE_C=OFF"],
        ["cmake", "--build", str(build_dir), "--target", "nanolance_rust_shim", "-j", str(os.cpu_count())],
    ]
    for cmd in steps:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if proc.returncode != 0:
            print(proc.stdout[-6000:])
            sys.exit("building libnanolance_rust_shim failed")
    # The test binary links zstd and lz4 of its own; exported copies of nanolance's would let calls
    # bind across versions (Lance's own compression tests crashed on a CI runner that way).
    shim = build_dir / "libnanolance_rust_shim.so"
    if shutil.which("nm") and shim.exists():
        exported = subprocess.run(["nm", "-D", "--defined-only", str(shim)], stdout=subprocess.PIPE, text=True).stdout
        leaked = [line.split()[-1] for line in exported.splitlines()
                  if re.search(r" (ZSTD|ZDICT|HUF|FSE|LZ4|XXH)", line)]
        if leaked:
            sys.exit(f"libnanolance_rust_shim exports compression symbols ({', '.join(leaked[:5])}, ...)")
    return build_dir


def run(crate: str, src: Path, shim_dir: Path, log: Path, name_filter: str | None,
        threads: int | None) -> dict[str, str]:
    """cargo test; the Rust result of each test that ran ("ok", "FAILED", "ignored")."""
    env = dict(os.environ)
    env["RUSTFLAGS"] = (env.get("RUSTFLAGS", "") + f" -L native={shim_dir}").strip()
    env["LD_LIBRARY_PATH"] = f"{shim_dir}:{env.get('LD_LIBRARY_PATH', '')}"
    env["NANOLANCE_RUST_LOG"] = str(log)
    env["NANOLANCE_RUST_CRATE"] = crate
    # Outside the crate directory, which fetch() recreates: a rerun rebuilds only the patched crate.
    env.setdefault("CARGO_TARGET_DIR", str(src.parent / "target"))
    tmp = Path(tempfile.mkdtemp(prefix="nanolance-rust-"))
    env["NANOLANCE_RUST_TMP"] = str(tmp)
    cmd = ["cargo", "test", "--lib", "--"]
    if name_filter:
        cmd.append(name_filter)
    if threads:
        cmd += ["--test-threads", str(threads)]
    proc = subprocess.run(cmd, cwd=src, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    shutil.rmtree(tmp, ignore_errors=True)
    results = {}
    for line in proc.stdout.splitlines():
        m = re.match(r"^test (\S+) \.\.\. (ok|FAILED|ignored)", line)
        if m:
            results[f"{crate}::{m.group(1)}"] = m.group(2)
    if proc.returncode != 0 and not results:
        print(proc.stdout[-6000:])
        sys.exit("cargo test did not run")
    if "test result:" not in proc.stdout:
        # The test binary died: a crash inside nanolance takes the process with it. Run the tests again
        # one at a time, where libtest names each test before running it, to say which one it was.
        print(proc.stdout[-3000:])
        Path(env["NANOLANCE_RUST_TMP"]).mkdir(parents=True, exist_ok=True)
        again = subprocess.run(cmd[:cmd.index("--") + 1] + ([name_filter] if name_filter else []) +
                               ["--test-threads", "1"], cwd=src, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
        started = [line for line in again.stdout.splitlines() if line.startswith("test ")]
        last = started[-1] if started else "(none)"
        print(again.stdout[-3000:])
        sys.exit(f"the test binary crashed; run one test at a time, it died in: {last}")
    return results


def summarize(results: dict[str, str], log: Path) -> tuple[dict, dict[str, str], list[dict]]:
    worst: dict[str, str] = {}
    mismatches = []
    reasons: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    for line in log.read_text().splitlines() if log.exists() else []:
        rec = json.loads(line)
        test = rec["test"]
        worst[test] = max(worst.get(test, "pass"), rec["status"], key=RANK.__getitem__)
        if rec["status"] == "mismatch":
            mismatches.append(rec)
        if rec["status"] in ("refused", "type"):
            reasons[rec["status"]][re.sub(r"\d+", "N", rec["detail"])[:160]] += 1
    per_test = {}
    for test, rust in results.items():
        if rust == "ignored":
            continue
        per_test[test] = worst.get(test, "no round trip")
    counts = collections.Counter(per_test.values())
    summary = {
        "crates": [f"{c} {VERSION}" for c in CRATES],
        "tests_run": len(per_test),
        "rust_failed": sorted(t for t, r in results.items() if r == "FAILED"),
        "nanolance": dict(sorted(counts.items())),
        "round_trips": dict(collections.Counter(json.loads(l)["status"] for l in log.read_text().splitlines())) if log.exists() else {},
        "top_refusals": [{"count": n, "reason": r} for r, n in reasons["refused"].most_common(25)],
        "top_type_differences": [{"count": n, "reason": r} for r, n in reasons["type"].most_common(10)],
    }
    return summary, per_test, mismatches


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update", action="store_true", help="rewrite the list of tests that pass")
    ap.add_argument("--cache", type=Path, default=ROOT / ".deps" / "rust-suite")
    ap.add_argument("--build-dir", type=Path, default=ROOT / "build-rust-shim")
    ap.add_argument("--filter", help="only tests whose name contains this")
    ap.add_argument("--threads", type=int, help="test threads (cargo's default: one per core)")
    ap.add_argument("--report", type=Path, help="write the summary here as JSON")
    ap.add_argument("--log", type=Path, help="keep the per-round-trip log (JSON lines) here")
    ap.add_argument("--crates", nargs="+", default=list(CRATES), choices=CRATES)
    args = ap.parse_args()

    shim_dir = build_shim(args.build_dir.resolve())
    log = args.log.resolve() if args.log else Path(tempfile.mkstemp(suffix=".jsonl")[1])
    log.write_text("")
    results: dict[str, str] = {}
    for crate in args.crates:
        src = fetch(crate, args.cache)
        patch(crate, src)
        results.update(run(crate, src, shim_dir, log, args.filter, args.threads))
    summary, per_test, mismatches = summarize(results, log)
    if not args.log:
        log.unlink()

    print(f"{', '.join(args.crates)} {VERSION}: {summary['tests_run']} tests ran; "
          f"nanolance per test: {summary['nanolance']}")
    print(f"round trips read back by nanolance: {summary['round_trips']}")
    for r in summary["top_refusals"][:10]:
        print(f"  refused x{r['count']}: {r['reason']}")
    if summary["rust_failed"]:
        print(f"{len(summary['rust_failed'])} test(s) fail in Rust itself: {summary['rust_failed'][:5]}")

    known = set(KNOWN_MISMATCH.read_text().split()) if KNOWN_MISMATCH.exists() else set()
    new_mismatch = sorted({m["test"] for m in mismatches} - known)
    for m in mismatches[:20]:
        print(f"  MISMATCH {m['test']} [{m['encoding']}, {m['type']}]: {m['detail'][:300]}")
    passing = sorted(t for t, s in per_test.items() if s in ("pass", "type"))
    if args.report:
        summary["mismatched_tests"] = sorted({m["test"] for m in mismatches})
        args.report.write_text(json.dumps(summary, indent=2) + "\n")

    status = 0
    listed = set(EXPECTED.read_text().split()) if EXPECTED.exists() else set()
    ran = tuple(f"{c}::" for c in args.crates)
    expected = {t for t in listed if t.startswith(ran)}  # the listed tests of the crates that ran
    if args.update and not args.filter:
        EXPECTED.write_text("".join(t + "\n" for t in sorted((listed - expected) | set(passing))))
        print(f"wrote {len(passing)} tests of {', '.join(args.crates)} to {EXPECTED.relative_to(ROOT)}")
    elif not args.filter:
        lost = sorted(expected - set(passing))
        if lost:
            print(f"{len(lost)} listed test(s) no longer pass: {lost[:20]}")
            status = 1
        new = set(passing) - expected
        if new:
            print(f"{len(new)} test(s) pass that the list does not have yet (--update adds them)")
        if not lost:
            print(f"all {len(expected)} expected tests passed")
    if new_mismatch:
        print(f"{len(new_mismatch)} test(s) with a MISMATCH not in {KNOWN_MISMATCH.relative_to(ROOT)}")
        status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
