"""The packaging config's file references, checked without building a wheel.

`cibuildwheel`'s `test-command` runs only inside a wheel job, so a wrong path in it is invisible
until CI has already built and repaired the wheel -- the failure lands at the very last step of the
longest job, on every platform at once. That is what happened:

    can't open file '/Users/runner/work/nanolance/nanolance/tests/wheel_smoke.py'

The command said `{project}/tests/wheel_smoke.py`. Those braces are cibuildwheel placeholders, and
the two that matter are different directories:

    {project}  the working directory cibuildwheel was called in -- here, the repository root
    {package}  the package directory -- here, bindings/python

(See `prepare_command(..., project=Path(".").resolve(), package=build_options.package_dir.resolve())`
in cibuildwheel's `macos.py` / `linux.py`.) The smoke test lives under the package, so the reference
had to be `{package}`.

This resolves the placeholders the same way cibuildwheel does and asserts the file is really there.
It costs nothing and turns a 40-minute CI round trip into a local failure.
"""

from __future__ import annotations

import pathlib
import re

import pytest

PACKAGE_DIR = pathlib.Path(__file__).resolve().parent.parent
PROJECT_DIR = PACKAGE_DIR.parent.parent
PYPROJECT = PACKAGE_DIR / "pyproject.toml"


def _load_pyproject():
    try:
        import tomllib
    except ModuleNotFoundError:  # Python < 3.11
        tomllib = pytest.importorskip("tomli", reason="needs tomllib (3.11+) or tomli to parse TOML")
    with PYPROJECT.open("rb") as handle:
        return tomllib.load(handle)


def test_cibuildwheel_test_command_points_at_a_file_that_exists():
    config = _load_pyproject().get("tool", {}).get("cibuildwheel", {})
    command = config.get("test-command")
    assert command, "cibuildwheel has no test-command; the wheels would ship unverified"

    resolved = command.replace("{package}", str(PACKAGE_DIR)).replace("{project}", str(PROJECT_DIR))
    assert "{" not in resolved, f"unresolved cibuildwheel placeholder in test-command: {command!r}"

    # Every path-looking argument in the command must exist. Anything else (flags, `python`) is left
    # alone; only the placeholder-derived paths are being checked, which is where the bug was.
    referenced = [
        pathlib.Path(token)
        for token in re.split(r"\s+", resolved)
        if token.startswith(str(PROJECT_DIR))
    ]
    assert referenced, f"test-command references no file under the project: {command!r}"
    for path in referenced:
        assert path.is_file(), (
            f"cibuildwheel test-command references {path}, which does not exist. "
            f"Check whether it should be {{package}} (bindings/python) rather than "
            f"{{project}} (the repository root)."
        )
