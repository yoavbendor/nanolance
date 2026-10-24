#!/usr/bin/env python3
"""Checks that each layer's README says, truthfully, what of the Lance spec it implements and how it is tested.

Every layer in tools/layout.py has a src/<layer>/README.md with a `## Spec coverage` table:

    | ID | Spec | Status | Implementation | Tests | Verified by | Notes |

- ID: unique across the repository, starting with the layer's name (`table.transaction.append`).
- Spec: a link into the Lance format specification (https://lance.org/format/...), or for a layer the
  specification does not cover, the reference the behavior follows (`ref: pylance ...`).
- Status: done, partial, missing or n/a.
- Implementation: `path::symbol` items (path below the repository root); the file must exist and contain the
  symbol as a word.
- Tests: ctest names (`nano_lance_rle`), or pytest ids (`bindings/python/tests/test_x.py::test_name`, or just the
  file); each must exist.
- Verified by: unit (nanolance's own tests), interop (against files pylance writes or reads), or both
  (`unit+interop`).

A done or partial row names an implementation and at least one test. Separately, every module of a layer (its
files in tools/layout.py) must have a C++ test under tests/<layer>/ that includes it, unless it is listed in
tools/tests_allow.txt as `<module>  <reason>` (a list that may only shrink). Prints the table of each layer's
coverage counts, and exits 1 with every problem listed.
"""
from __future__ import annotations

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import layout  # noqa: E402

STATUSES = ("done", "partial", "missing", "n/a")
VERIFIED = ("unit", "interop", "unit+interop", "-")
SPEC_PREFIX = "https://lance.org/format/"
COLUMNS = ["ID", "Spec", "Status", "Implementation", "Tests", "Verified by", "Notes"]


def ticked(cell: str) -> list[str]:
    """The `backticked` items of a cell."""
    return re.findall(r"`([^`]+)`", cell)


def parse_table(text: str) -> tuple[list[list[str]], str | None]:
    """The rows of the `## Spec coverage` table, or an error saying why there is none."""
    m = re.search(r"^## Spec coverage\s*$", text, re.M)
    if not m:
        return [], "no '## Spec coverage' section"
    lines = text[m.end():].splitlines()
    rows: list[list[str]] = []
    header_seen = False
    for line in lines:
        if line.startswith("## "):
            break
        if not line.startswith("|"):
            if rows or header_seen:
                if line.strip() == "":
                    continue
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if not header_seen:
            if cells != COLUMNS:
                return [], f"table header is {cells}, expected {COLUMNS}"
            header_seen = True
            continue
        if all(set(c) <= set("-: ") for c in cells):
            continue  # the separator row
        rows.append(cells)
    if not header_seen:
        return [], "the Spec coverage section has no table"
    return rows, None


def registered_ctests() -> set[str]:
    names: set[str] = set()
    for cm in [os.path.join(ROOT, "CMakeLists.txt")] + glob.glob(os.path.join(ROOT, "tests", "**", "CMakeLists.txt"),
                                                                 recursive=True) + \
            glob.glob(os.path.join(ROOT, "tests", "**", "*.cmake"), recursive=True):
        names.update(re.findall(r"add_test\(\s*NAME\s+([A-Za-z0-9_.\-]+)", open(cm, encoding="utf-8").read()))
    return names


def test_exists(name: str, ctests: set[str]) -> bool:
    if "/" not in name and not name.endswith(".py"):
        return name in ctests
    path, _, func = name.partition("::")
    full = os.path.join(ROOT, path)
    if not os.path.isfile(full):
        return False
    if not func:
        return True
    func = func.split("[", 1)[0].split("::")[-1]
    return re.search(rf"^\s*(async\s+)?def {re.escape(func)}\b", open(full, encoding="utf-8").read(), re.M) is not None


def implementation_exists(item: str) -> str | None:
    """None when `path::symbol` exists, else why not."""
    path, _, symbol = item.partition("::")
    full = os.path.join(ROOT, path)
    if not os.path.exists(full):
        return f"{path} does not exist"
    if symbol and os.path.isfile(full):
        if not re.search(rf"\b{re.escape(symbol.split('::')[-1])}\b", open(full, encoding="utf-8", errors="ignore").read()):
            return f"{path} has no '{symbol}'"
    return None


def check_readme(layer: str, ctests: set[str], seen_ids: dict[str, str], problems: list[str]) -> dict[str, int]:
    path = os.path.join(ROOT, "src", layer, "README.md")
    rel = os.path.relpath(path, ROOT)
    counts = {s: 0 for s in STATUSES}
    if not os.path.isfile(path):
        problems.append(f"{rel}: missing")
        return counts
    rows, error = parse_table(open(path, encoding="utf-8").read())
    if error:
        problems.append(f"{rel}: {error}")
        return counts
    for cells in rows:
        if len(cells) != len(COLUMNS):
            problems.append(f"{rel}: row {cells[:1]} has {len(cells)} cells, expected {len(COLUMNS)}")
            continue
        rid, spec, status, impl, tests, verified, _notes = cells
        rid = rid.strip("`")
        where = f"{rel}: {rid}"
        if not rid.startswith(layer + "."):
            problems.append(f"{where}: the ID does not start with '{layer}.'")
        if rid in seen_ids:
            problems.append(f"{where}: the ID is also used in {seen_ids[rid]}")
        seen_ids[rid] = rel
        links = re.findall(r"\((https?://[^)\s]+)\)", spec)
        if not any(l.startswith(SPEC_PREFIX) for l in links) and not spec.lower().startswith("ref:"):
            problems.append(f"{where}: Spec is neither a {SPEC_PREFIX} link nor 'ref: ...'")
        if status not in STATUSES:
            problems.append(f"{where}: status '{status}' is not one of {STATUSES}")
            continue
        counts[status] += 1
        if verified not in VERIFIED:
            problems.append(f"{where}: 'Verified by' is '{verified}', not one of {VERIFIED}")
        impls, names = ticked(impl), ticked(tests)
        if status in ("done", "partial"):
            if not impls:
                problems.append(f"{where}: {status} but names no implementation")
            if not names:
                problems.append(f"{where}: {status} but names no test")
            if verified == "-":
                problems.append(f"{where}: {status} but 'Verified by' is '-'")
        for item in impls:
            why = implementation_exists(item)
            if why:
                problems.append(f"{where}: implementation {why}")
        for name in names:
            if not test_exists(name, ctests):
                problems.append(f"{where}: test '{name}' does not exist")
    return counts


def allowed_untested() -> dict[str, str]:
    path = os.path.join(ROOT, "tools", "tests_allow.txt")
    out: dict[str, str] = {}
    if os.path.isfile(path):
        for line in open(path, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if line:
                module, _, reason = line.partition(" ")
                out[module] = reason.strip()
    return out


def check_module_tests(problems: list[str]) -> dict[str, tuple[int, int]]:
    """Each layer's modules with a test under tests/<layer>/ that includes them: (tested, total)."""
    allow = allowed_untested()
    seen_allow: set[str] = set()
    out: dict[str, tuple[int, int]] = {}
    for layer, _, _ in layout.LAYERS:
        modules = sorted({f.rsplit("/", 1)[-1].rsplit(".", 1)[0] for f in layout.FILES.get(layer, [])
                          if f.endswith((".cpp", ".hpp"))} | set(layout.HEADERS.get(layer, [])))
        texts = [open(p, encoding="utf-8", errors="ignore").read()
                 for p in glob.glob(os.path.join(ROOT, "tests", layer, "**", "*.cpp"), recursive=True)]
        tested = 0
        for m in modules:
            pattern = re.compile(r'#include\s+"[^"]*\b' + re.escape(m) + r'\.h(pp)?"')
            if any(pattern.search(t) for t in texts):
                tested += 1
                if m in allow:
                    problems.append(f"tools/tests_allow.txt: {m} now has a test in tests/{layer}/; remove it")
            elif m in allow:
                seen_allow.add(m)
            else:
                problems.append(f"{layer}: module '{m}' has no test in tests/{layer}/ (or a line in tools/tests_allow.txt)")
        out[layer] = (tested, len(modules))
    for m in sorted(set(allow) - seen_allow):
        if not any(m in (f.rsplit("/", 1)[-1].rsplit(".", 1)[0] for f in fs) for fs in layout.FILES.values()) and \
                not any(m in hs for hs in layout.HEADERS.values()):
            problems.append(f"tools/tests_allow.txt: '{m}' is not a module")
    return out


def main() -> int:
    problems: list[str] = []
    ctests = registered_ctests()
    seen: dict[str, str] = {}
    print(f"{'layer':10s} {'done':>5s} {'partial':>8s} {'missing':>8s} {'n/a':>5s}   modules tested")
    module_counts = check_module_tests(problems)
    for layer, _, _ in layout.LAYERS:
        c = check_readme(layer, ctests, seen, problems)
        t, n = module_counts[layer]
        print(f"{layer:10s} {c['done']:5d} {c['partial']:8d} {c['missing']:8d} {c['n/a']:5d}   {t}/{n}")
    if problems:
        print(f"\n{len(problems)} problem(s):")
        for p in problems:
            print("  " + p)
        return 1
    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
