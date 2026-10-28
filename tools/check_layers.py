#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Layering check for nanolance. The layers, and where each file belongs, are in tools/layout.py.

A file may include modules of its own layer or of a layer below it. An include of a higher layer is a
violation. Known violations are listed in tools/layers_allow.txt: the check fails on a violation that is
not listed there, and on a listed entry that is no longer a violation, so the list can only shrink.
Dependency cycles between modules are reported, but do not fail the check.

The example programs examples/layers/<layer>.cpp are checked too: each belongs to the layer its name gives,
and may include only headers of that layer or of a layer below. Nothing in them is allowlisted.

Usage: check_layers.py [--root PATH] [--write-allowlist]
Exit codes: 0 clean, 1 failure, 2 usage error.
"""
import argparse
import importlib.util
import re
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ALLOW_NAME = "layers_allow.txt"
SRC_SUFFIXES = {".cpp", ".hpp", ".h", ".inc"}
PUBLIC_SUFFIXES = {".h", ".hpp"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
# Examples include public headers as <nanolance/x.hpp> or "nanolance/x.hpp", and internal ones as "layer/x.hpp".
EXAMPLE_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+[<"]([^>"]+)[>"]')
ALLOW_RE = re.compile(r"^(\S+)\s*->\s*(\S+)$")


def load_layout(root):
    """The fixture's own tools/layout.py when it has one, else the real one next to this script."""
    local = root / "tools" / "layout.py"
    path = local if local.is_file() else HERE / "layout.py"
    spec = importlib.util.spec_from_file_location("layout_for_check", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def scan_files(root):
    """Files the check reads: src/**/*.{cpp,hpp,h,inc} and include/nanolance/*.{h,hpp}."""
    out = []
    src = root / "src"
    if src.is_dir():
        out += sorted(p for p in src.rglob("*") if p.is_file() and p.suffix in SRC_SUFFIXES)
    inc = root / "include" / "nanolance"
    if inc.is_dir():
        out += sorted(p for p in inc.glob("*") if p.is_file() and p.suffix in PUBLIC_SUFFIXES)
    return out


def scan_examples(root):
    """The example programs the check reads: examples/layers/<layer>.cpp."""
    d = root / "examples" / "layers"
    return sorted(p for p in d.glob("*.cpp") if p.is_file()) if d.is_dir() else []


def check_examples(root, layout, layer_of):
    """Checks each example. Returns (errors, violations, files, includes): violations are (layer, target layer,
    target module, file, line) for an include of a module above the example's layer. Includes that name no
    module of tools/layout.py (the standard library, nanoarrow, the generated protobuf) are not counted."""
    rank = layout.rank()
    errors, violations, files, includes = [], [], 0, 0
    for path in scan_examples(root):
        rel = path.relative_to(root).as_posix()
        layer = path.stem
        files += 1
        if layer not in rank:
            errors.append(f"{rel}: '{layer}' is not a layer in tools/layout.py (an example is named for its layer)")
            continue
        for n, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            m = EXAMPLE_INCLUDE_RE.match(line)
            if not m:
                continue
            target = m.group(1).rsplit("/", 1)[-1].rsplit(".", 1)[0]
            if target not in layer_of:
                continue
            includes += 1
            if rank[layer_of[target]] > rank[layer]:
                violations.append((layer, layer_of[target], target, rel, n))
    return errors, violations, files, includes


def read_allowlist(path):
    """Set of (A, B) pairs from the allowlist, plus any malformed-line errors."""
    entries, errors = set(), []
    if not path.is_file():
        return entries, errors
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        text = raw.split("#", 1)[0].strip()
        if not text:
            continue
        m = ALLOW_RE.match(text)
        if m:
            entries.add((m.group(1), m.group(2)))
        else:
            errors.append(f"{path.name}:{n}: malformed entry {raw.strip()!r} (want 'A -> B')")
    return entries, errors


def write_allowlist(path, pairs, layer_of):
    lines = [
        "# Known layering violations: 'A -> B' means module A includes module B from a higher layer.",
        "# Maintained by tools/check_layers.py --write-allowlist. This list may only shrink.",
        "",
    ]
    for a, b in pairs:
        lines.append(f"{a} -> {b}  # {layer_of[a]} -> {layer_of[b]}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def strongly_connected(nodes, adj):
    """Tarjan's algorithm. Returns the components with more than one module, sorted."""
    index, low, on_stack, stack, out = {}, {}, set(), [], []
    counter = [0]

    def visit(v):
        index[v] = low[v] = counter[0]
        counter[0] += 1
        stack.append(v)
        on_stack.add(v)
        for w in sorted(adj.get(v, ())):
            if w not in index:
                visit(w)
                low[v] = min(low[v], low[w])
            elif w in on_stack:
                low[v] = min(low[v], index[w])
        if low[v] == index[v]:
            comp = []
            while True:
                w = stack.pop()
                on_stack.discard(w)
                comp.append(w)
                if w == v:
                    break
            if len(comp) > 1:
                out.append(sorted(comp))

    for v in sorted(nodes):
        if v not in index:
            visit(v)
    return sorted(out)


def collect(root):
    """Read the layout and every file the check scans. Returns (layout, layer_of, errors, per_layer, modules,
    edges), where edges are (includer module, included module, file, line) for each include between known
    modules. tools/gen_layer_docs.py reads the same edges."""
    layout = load_layout(root)
    layer_of = layout.module_layer()
    layer_dir = {name: d for name, d, _ in layout.LAYERS}
    errors = []
    per_layer = {name: [0, 0] for name, _, _ in layout.LAYERS}  # files, lines
    modules = set()
    edges = []  # (includer, included, file, line)

    for path in scan_files(root):
        rel = path.relative_to(root).as_posix()
        module = path.name.rsplit(".", 1)[0]
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        if module not in layer_of:
            errors.append(f"{rel}: module '{module}' is not in tools/layout.py (add it to FILES or HEADERS)")
            continue
        layer = layer_of[module]
        modules.add(module)
        per_layer[layer][0] += 1
        per_layer[layer][1] += len(lines)

        if rel.startswith("src/"):
            parts = rel[len("src/"):].split("/")
            if len(parts) > 1:  # moved state: src/<layer dir>/<FILES entry>
                if parts[0] != layer_dir[layer]:
                    errors.append(f"{rel}: under src/{parts[0]}/, but layer '{layer}' lives in "
                                  f"src/{layer_dir[layer]}/")
                elif "/".join(parts[1:]) not in layout.FILES.get(layer, []):
                    errors.append(f"{rel}: 'src/{layer_dir[layer]}/{'/'.join(parts[1:])}' is not the "
                                  f"FILES['{layer}'] entry for module '{module}'")

        for n, line in enumerate(lines, 1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            target = m.group(1).rsplit("/", 1)[-1].rsplit(".", 1)[0]
            if target == module or target not in layer_of:
                continue  # self-edge, or third-party / system header
            edges.append((module, target, rel, n))

    return layout, layer_of, errors, per_layer, modules, edges


def run(root, write):
    layout, layer_of, errors, per_layer, modules, edges = collect(root)
    rank = layout.rank()
    ex_errors, ex_violations, ex_files, ex_includes = check_examples(root, layout, layer_of)
    errors += ex_errors

    occurrences = [(a, b, rel, n) for a, b, rel, n in edges if rank[layer_of[b]] > rank[layer_of[a]]]
    pairs = sorted({(a, b) for a, b, _, _ in occurrences})

    allow_path = root / "tools" / ALLOW_NAME
    allowed, allow_errors = read_allowlist(allow_path)
    errors += allow_errors
    if write:
        write_allowlist(allow_path, pairs, layer_of)
        allowed = set(pairs)
    pair_set = set(pairs)
    new = [p for p in pairs if p not in allowed]
    stale = sorted(p for p in allowed if p not in pair_set)

    adj = defaultdict(set)
    for a, b, _, _ in edges:
        adj[a].add(b)
    cycles = strongly_connected(modules | {b for _, b, _, _ in edges}, adj)

    # ---- report ----
    print(f"files: {sum(v[0] for v in per_layer.values())}, lines: {sum(v[1] for v in per_layer.values())}")
    print(f"{'layer':<10}{'dir':<12}{'files':>6}{'lines':>9}")
    for name, d, _ in layout.LAYERS:
        print(f"{name:<10}{d:<12}{per_layer[name][0]:>6}{per_layer[name][1]:>9}")
    print(f"include edges between known modules: {len(edges)} ({len({(a, b) for a, b, _, _ in edges})} distinct)")
    print(f"examples/layers: {ex_files} file(s), {ex_includes} include(s) of known modules, "
          f"{len(ex_violations)} violation(s)")
    print(f"violations: {len(occurrences)} occurrence(s) in {len(pairs)} module pair(s); "
          f"allowlist: {len(allowed)} entr(y/ies)")

    if errors:
        print("\nerrors:")
        for e in errors:
            print(f"  {e}")
    if occurrences:
        print("\nviolations:")
        for a, b, rel, n in occurrences:
            tag = "allowed" if (a, b) in allowed else "NEW"
            print(f"  {a} ({layer_of[a]}) -> {b} ({layer_of[b]})  [{rel}:{n}]  {tag}")
    if ex_violations:
        print("\nexample violations (an example may include only its own layer and the layers below):")
        for layer, tlayer, target, rel, n in ex_violations:
            print(f"  example {layer} includes {target} ({tlayer})  [{rel}:{n}]")
    if new:
        print(f"\nnot in {ALLOW_NAME} (fix the include, or add the pair with --write-allowlist):")
        for a, b in new:
            print(f"  {a} -> {b}")
    if stale:
        print(f"\nstale {ALLOW_NAME} entries (no longer violations; remove them):")
        for a, b in stale:
            print(f"  {a} -> {b}")
    if cycles:
        print("\ndependency cycles (informational):")
        for comp in cycles:
            print(f"  {{{', '.join(comp)}}} (size {len(comp)})")
    else:
        print("\ndependency cycles (informational): none")

    failed = bool(errors or new or stale or ex_violations)
    if write:
        print(f"\nwrote {allow_path.relative_to(root).as_posix()} with {len(pairs)} entr(y/ies)")
    print("\nFAIL" if failed else "\nOK")
    return 1 if failed else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Check nanolance's layering (tools/layout.py).")
    ap.add_argument("--root", type=Path, default=HERE.parent,
                    help="tree to check (default: the repository this script is in)")
    ap.add_argument("--write-allowlist", action="store_true",
                    help="rewrite tools/layers_allow.txt with the current violations")
    args = ap.parse_args(argv)
    root = args.root.resolve()
    if not root.is_dir():
        ap.error(f"--root {args.root} is not a directory")
    return run(root, args.write_allowlist)


if __name__ == "__main__":
    sys.exit(main())
