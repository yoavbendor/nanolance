# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/check_spec.py, on small fixture trees with a two-layer tools/layout.py."""
import shutil
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "check_spec.py"

LAYOUT = '''
LAYERS = [("low", "low", "bottom"), ("high", "high", "top")]
FILES = {"low": ["codec.cpp", "codec.hpp"], "high": ["api.cpp"]}
HEADERS = {"low": [], "high": []}
'''

HEADER = "| ID | Spec | Status | Implementation | Tests | Verified by | Notes |\n|---|---|---|---|---|---|---|\n"


def readme(rows):
    return "# layer\n\n## Spec coverage\n\n" + HEADER + "".join(rows) + "\n## Known gaps\n\nnone\n"


def tree(tmp_path, low_rows, high_rows, tests=True, allow=""):
    (tmp_path / "tools").mkdir()
    shutil.copy(SCRIPT, tmp_path / "tools" / "check_spec.py")
    (tmp_path / "tools" / "layout.py").write_text(LAYOUT)
    (tmp_path / "tools" / "tests_allow.txt").write_text(allow)
    for layer, rows in (("low", low_rows), ("high", high_rows)):
        (tmp_path / "src" / layer).mkdir(parents=True)
        if rows is not None:
            (tmp_path / "src" / layer / "README.md").write_text(readme(rows))
    (tmp_path / "src" / "low" / "codec.cpp").write_text("int encode_block() { return 0; }\n")
    (tmp_path / "src" / "low" / "codec.hpp").write_text("int encode_block();\n")
    (tmp_path / "src" / "high" / "api.cpp").write_text("void open_api() {}\n")
    (tmp_path / "tests" / "low").mkdir(parents=True)
    (tmp_path / "tests" / "high").mkdir(parents=True)
    if tests:
        (tmp_path / "tests" / "low" / "test_codec.cpp").write_text('#include "low/codec.hpp"\n')
        (tmp_path / "tests" / "high" / "test_api.cpp").write_text('#include "high/api.hpp"\n')
    (tmp_path / "CMakeLists.txt").write_text("add_test(NAME nl_codec COMMAND x)\nadd_test(NAME nl_api COMMAND y)\n")
    (tmp_path / "bindings" / "python" / "tests").mkdir(parents=True)
    (tmp_path / "bindings" / "python" / "tests" / "test_py.py").write_text("def test_roundtrip():\n    pass\n")
    return tmp_path


def run(root):
    p = subprocess.run([sys.executable, str(root / "tools" / "check_spec.py")], capture_output=True, text=True)
    return p.returncode, p.stdout


GOOD_LOW = ["| `low.block` | [Encoding](https://lance.org/format/file/encoding/) | done | `src/low/codec.cpp::encode_block` "
            "| `nl_codec`, `bindings/python/tests/test_py.py::test_roundtrip` | unit+interop | |\n",
            "| `low.other` | [Encoding](https://lance.org/format/file/encoding/) | missing | | | - | not yet |\n"]
GOOD_HIGH = ["| `high.open` | ref: pylance's open | done | `src/high/api.cpp::open_api` | `nl_api` | unit | |\n"]


def test_a_truthful_tree_passes(tmp_path):
    code, out = run(tree(tmp_path, GOOD_LOW, GOOD_HIGH))
    assert code == 0, out
    assert "OK" in out
    assert "low" in out and "1/2" not in out  # both modules of low are tested


def test_a_missing_readme_fails(tmp_path):
    code, out = run(tree(tmp_path, GOOD_LOW, None))
    assert code == 1 and "src/high/README.md: missing" in out


def test_a_done_row_needs_an_implementation_and_a_test(tmp_path):
    rows = ["| `low.block` | [E](https://lance.org/format/file/encoding/) | done | | | unit | |\n"]
    code, out = run(tree(tmp_path, rows, GOOD_HIGH))
    assert code == 1
    assert "done but names no implementation" in out and "done but names no test" in out


def test_an_implementation_that_is_not_there_fails(tmp_path):
    rows = ["| `low.block` | [E](https://lance.org/format/file/encoding/) | done | `src/low/codec.cpp::decode_block` "
            "| `nl_codec` | unit | |\n"]
    code, out = run(tree(tmp_path, rows, GOOD_HIGH))
    assert code == 1 and "has no 'decode_block'" in out


def test_a_test_that_is_not_registered_fails(tmp_path):
    rows = ["| `low.block` | [E](https://lance.org/format/file/encoding/) | done | `src/low/codec.cpp::encode_block` "
            "| `nl_nope`, `bindings/python/tests/test_py.py::test_gone` | unit | |\n"]
    code, out = run(tree(tmp_path, rows, GOOD_HIGH))
    assert code == 1
    assert "test 'nl_nope' does not exist" in out and "test_py.py::test_gone' does not exist" in out


def test_ids_are_prefixed_by_layer_and_unique(tmp_path):
    rows = [GOOD_LOW[0], GOOD_LOW[0].replace("`low.block`", "`high.block`")]
    high = [GOOD_HIGH[0].replace("`high.open`", "`low.block`")]
    code, out = run(tree(tmp_path, rows, high))
    assert code == 1
    assert "does not start with 'low.'" in out and "also used in" in out


def test_spec_must_link_the_spec_or_name_a_reference(tmp_path):
    rows = ["| `low.block` | [E](https://example.com/spec) | missing | | | - | |\n"]
    code, out = run(tree(tmp_path, rows, GOOD_HIGH))
    assert code == 1 and "neither a https://lance.org/format/ link" in out


def test_status_and_verified_by_are_checked(tmp_path):
    rows = ["| `low.block` | [E](https://lance.org/format/file/encoding/) | almost | | | - | |\n",
            "| `low.b2` | [E](https://lance.org/format/file/encoding/) | done | `src/low/codec.cpp::encode_block` "
            "| `nl_codec` | eyeballed | |\n"]
    code, out = run(tree(tmp_path, rows, GOOD_HIGH))
    assert code == 1 and "status 'almost'" in out and "'Verified by' is 'eyeballed'" in out


def test_a_module_without_a_test_in_its_layer_fails_unless_allowed(tmp_path):
    root = tree(tmp_path, GOOD_LOW, GOOD_HIGH, tests=False)
    code, out = run(root)
    assert code == 1 and "module 'codec' has no test in tests/low/" in out
    (root / "tools" / "tests_allow.txt").write_text("codec  covered by the Python suite\napi  later\n")
    code, out = run(root)
    assert code == 0, out


def test_the_allowlist_only_shrinks(tmp_path):
    code, out = run(tree(tmp_path, GOOD_LOW, GOOD_HIGH, allow="codec  stale\nghost  not a module\n"))
    assert code == 1
    assert "codec now has a test in tests/low/; remove it" in out and "'ghost' is not a module" in out


def test_a_wrong_table_header_fails(tmp_path):
    root = tree(tmp_path, GOOD_LOW, GOOD_HIGH)
    (root / "src" / "low" / "README.md").write_text("## Spec coverage\n\n| ID | Status |\n|---|---|\n")
    code, out = run(root)
    assert code == 1 and "table header is" in out


def test_the_readme_summary_must_be_current(tmp_path):
    root = tree(tmp_path, GOOD_LOW, GOOD_HIGH)
    (root / "README.md").write_text("# x\n\n<!-- spec-summary:start -->\n<!-- spec-summary:end -->\n\nafter\n")
    code, out = run(root)
    assert code == 1 and "spec summary is stale" in out
    p = subprocess.run([sys.executable, str(root / "tools" / "check_spec.py"), "--write-summary"],
                       capture_output=True, text=True)
    assert p.returncode == 0, p.stdout
    text = (root / "README.md").read_text()
    assert "| [low](src/low/README.md#spec-coverage) | 1 | 0 | 1 | 0 | 1/1 |" in text and text.endswith("after\n")
    code, out = run(root)
    assert code == 0, out
