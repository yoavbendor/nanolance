# Validating nanolance on your own machine (and a real S3 bucket)

One command builds everything from scratch in a new virtualenv and a new build directory, runs every suite CI
runs, and writes one report to send back. Nothing outside the checkout is touched; the existing
`bindings/python/build` is not used (a stale one there is a known cause of odd failures).

```bash
git fetch origin claude/nanolance-optimization-review-h3rxi7-highrisk
git checkout --detach origin/claude/nanolance-optimization-review-h3rxi7-highrisk   # or a commit you were given

# Everything but real S3 (about an hour on 8 cores; --quick skips the stress repeat, about 15 minutes less):
python3 tools/local_validate.py

# Plus a real bucket: an EMPTY scratch prefix, credentials from your usual AWS environment.
export AWS_PROFILE=...            # or AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY (/ AWS_SESSION_TOKEN), AWS_REGION
python3 tools/local_validate.py --s3 s3://your-bucket/scratch/nanolance-check

# S3-compatible stores (MinIO, R2, ...):
python3 tools/local_validate.py --s3 s3://bucket/prefix --s3-endpoint https://<account>.r2.cloudflarestorage.com --s3-region auto
```

Requirements: CMake 3.20+, a C++20 compiler (GCC 12+ or a recent clang), Python 3.10+ with `venv`, libcurl
development headers (for S3), git, and network access (pip packages, CMake's fetched dependencies, pylance's
test suite). `--python /path/to/python3.12` picks the interpreter the venv is made from.

What it runs, in order (each step's log in `.validate/<time>/`):

| Step | What |
|---|---|
| env | compilers, CMake, Python, uv, OS, CPUs, memory, commit, uncommitted files |
| venv | a new virtualenv, nothing from the system site-packages |
| cpp | a clean Release build and `ctest` |
| s3build | lance-c's S3 reader (`-DNANOLANCE_ENABLE_S3=ON`), used by the object-store tests |
| install | the Python binding, built in `.validate/<time>/build-python`, with its test dependencies and pylance 12.0.0 |
| moto | a local S3 server for the object-store tests (its own venv; skipped if it cannot be installed) |
| pytest | the binding's tests, as CI runs them |
| stress | the same with every read cut into 1 KiB pieces on 8 threads (`--quick` skips it) |
| pylance | pylance 12's own test suite against nanolance |
| lance_c | lance-c's C/C++ tests against liblance_c |
| s3 | `tools/s3_real_check.py --large --faults` against your prefix (only with `--s3`) |

The real-S3 step writes small datasets and one ~100 MiB one under the prefix, then deletes what it wrote.
`--skip cpp,lance_c` skips steps.

## The short run: `--fast`

```bash
python3 tools/local_validate.py --fast
```

A subset for the loop of changing code and checking it: every area has at least one test, but most tests are not
run. The steps keep their order, each with less in it:

| Step | In `--fast` |
|---|---|
| cpp | the 46 executables behind the 58 `smoke` tests, then `ctest -L smoke`; lance-c and the examples, and the tools the smoke tests do not use, are not built |
| s3build, install, moto | as in the full run (the object-store tests need them) |
| pytest | the entries of `tools/validate_fast.txt` [pytest]: 2,282 tests, one after another, as CI runs them |
| stress | skipped |
| pylance | 10 of pylance 12's test files (`tools/pylance_suite.py --files`); the verdict covers their expected-pass tests |
| lance_c | skipped |
| s3 | only with `--s3`, as before |

`tools/validate_fast.txt` lists what each entry covers and what it costs. The report's header says `profile: fast`.

Measured here, on a 4-core machine that other work shared (the build steps were slower for it): 1036 s in all steps,
about 17 minutes. The builds are most of it: cpp 269 s, s3build 357 s, install 188 s. The rest: moto 32 s, pytest
148 s, pylance 35 s. The full binding suite alone takes about 17 minutes here (serially), before the stress repeat.

Use `--fast` while changing code. Before you send a report back, run the full run (or `--quick`, which skips only the
stress repeat).

## What to send back

`.validate/<time>/report.txt`: at most 190 KB, an attachment. Its first section (the step table and the
environment) is short enough to paste on its own. The values of `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`
and `AWS_SESSION_TOKEN` are scrubbed from it, but read it before sending: bucket names and paths are in it.
The full logs stay in the same directory in case more is needed.
