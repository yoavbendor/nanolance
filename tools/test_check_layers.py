# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/check_layers.py, on small fixture trees with a three-layer tools/layout.py."""
import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parent / "check_layers.py"

FIXTURE_LAYOUT = '''
LAYERS = [
    ("low", "low", "bottom"),
    ("mid", "mid", "middle"),
    ("high", "high", "top"),
]
FILES = {
    "low": ["base.cpp", "base.hpp", "util.hpp"],
    "mid": ["mid.cpp"],
    "high": ["top.hpp"],
}
HEADERS = {"low": [], "mid": [], "high": []}


def module_layer():
    out = {}
    for layer, files in FILES.items():
        for f in files:
            out[f.rsplit(".", 1)[0]] = layer
    for layer, names in HEADERS.items():
        for n in names:
            out.setdefault(n, layer)
    return out


def rank():
    return {name: i for i, (name, _, _) in enumerate(LAYERS)}
'''

# module -> (layer dir, file name, text). Moved layout unless flat=True.
CLEAN = {
    "low/base.cpp": '#include "base.hpp"\n',
    "low/base.hpp": "// base\n",
    "mid/mid.cpp": '#include "base.hpp"\n',
    "high/top.hpp": '#include "base.hpp"\n#include "mid.cpp"\n',
}
UPWARD = dict(CLEAN, **{"low/base.cpp": '#include "base.hpp"\n#include "top.hpp"\n'})


def make_tree(root, files, flat=False):
    (root / "tools").mkdir(parents=True, exist_ok=True)
    (root / "tools" / "layout.py").write_text(FIXTURE_LAYOUT, encoding="utf-8")
    for rel, text in files.items():
        if flat:
            rel = rel.split("/", 1)[1]
        path = root / "src" / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return root


def check(root, *extra):
    return subprocess.run([sys.executable, str(SCRIPT), "--root", str(root), *extra],
                          capture_output=True, text=True)


def allow(root, text):
    (root / "tools" / "layers_allow.txt").write_text(text, encoding="utf-8")


@pytest.mark.parametrize("flat", [False, True], ids=["moved", "flat"])
def test_clean_tree_passes(tmp_path, flat):
    root = make_tree(tmp_path, CLEAN, flat=flat)
    r = check(root)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "OK" in r.stdout


def test_upward_include_fails_and_is_named(tmp_path):
    root = make_tree(tmp_path, UPWARD)
    r = check(root)
    assert r.returncode == 1
    assert "base (low) -> top (high)" in r.stdout
    assert "src/low/base.cpp:2" in r.stdout


def test_allowlisted_violation_passes(tmp_path):
    root = make_tree(tmp_path, UPWARD)
    allow(root, "# known\nbase -> top  # low -> high\n")
    r = check(root)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "allowed" in r.stdout


def test_stale_allowlist_entry_fails(tmp_path):
    root = make_tree(tmp_path, CLEAN)
    allow(root, "base -> top  # low -> high\n")
    r = check(root)
    assert r.returncode == 1
    assert "stale" in r.stdout
    assert "base -> top" in r.stdout


def test_file_in_wrong_layer_dir_fails(tmp_path):
    files = dict(CLEAN, **{"mid/base.cpp": "// base, filed under mid\n"})
    root = make_tree(tmp_path, files)
    r = check(root)
    assert r.returncode == 1
    assert "src/mid/base.cpp" in r.stdout
    assert "lives in src/low/" in r.stdout


def test_path_not_in_files_fails(tmp_path):
    files = dict(CLEAN, **{"low/sub/base.cpp": "// module base, path not in FILES\n"})
    root = make_tree(tmp_path, files)
    r = check(root)
    assert r.returncode == 1
    assert "'src/low/sub/base.cpp' is not the FILES['low'] entry" in r.stdout


def test_unknown_module_fails(tmp_path):
    files = dict(CLEAN, **{"low/mystery.cpp": "// no layer for this module\n"})
    root = make_tree(tmp_path, files)
    r = check(root)
    assert r.returncode == 1
    assert "'mystery' is not in tools/layout.py" in r.stdout


def test_unknown_includes_are_skipped(tmp_path):
    files = dict(CLEAN, **{"high/top.hpp": '#include <vector>\n#include "zstd.h"\n'
                                           '#include "nanoarrow/nanoarrow.h"\n'})
    root = make_tree(tmp_path, files)
    assert check(root).returncode == 0


def test_flat_layout_upward_fails(tmp_path):
    root = make_tree(tmp_path, UPWARD, flat=True)
    r = check(root)
    assert r.returncode == 1
    assert "base (low) -> top (high)" in r.stdout


def test_only_src_and_public_headers_are_scanned(tmp_path):
    root = make_tree(tmp_path, CLEAN)
    (root / "tests").mkdir()
    (root / "tests" / "x.cpp").write_text('#include "top.hpp"\n', encoding="utf-8")
    assert check(root).returncode == 0


def test_cycle_is_reported_but_not_a_failure(tmp_path):
    files = dict(CLEAN, **{"low/base.cpp": '#include "base.hpp"\n#include "util.hpp"\n',
                           "low/util.hpp": '#include "base.hpp"\n'})
    root = make_tree(tmp_path, files)
    r = check(root)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "{base, util} (size 2)" in r.stdout


def test_write_allowlist_round_trip(tmp_path):
    root = make_tree(tmp_path, UPWARD)
    r = check(root, "--write-allowlist")
    assert r.returncode == 0, r.stdout + r.stderr
    path = root / "tools" / "layers_allow.txt"
    first = path.read_text(encoding="utf-8")
    assert "base -> top  # low -> high" in first
    assert check(root).returncode == 0
    check(root, "--write-allowlist")
    assert path.read_text(encoding="utf-8") == first


def test_usage_error_exits_2(tmp_path):
    assert check(tmp_path / "missing").returncode == 2
