#!/usr/bin/env python3
"""Publish one benchmark run: a summary, the full results, and the history, for GitHub Pages.

    python tools/bench_publish.py --matrix M.json --scalar S.json --vector V.json --fts F.json \\
        --gate pass|fail|skipped --gate-log gate.txt --site OUT_DIR [--readme README.md]

Writes into OUT_DIR (published as bench/ on the gh-pages branch, so the MkDocs deploy keeps it):

  index.html         the latest run, in full, and the history of runs
  latest.json        the latest run's record (summary and raw results)
  summary.svg        the summary table as an image, for the README (it changes with every run)
  runs/<id>.json     every run's record; runs/index.json lists them, newest first

With --readme, the summary table is also written into README.md between the bench-summary markers. The
workflow passes that only on the default branch, so feature-branch runs never change a tracked file.

Speed is always the reference's median time over nanolance's: above 1x, nanolance is faster. The matrix
(27 data types, reads and writes) compares with pylance, as before; the index suites compare with Rust
Lance (the lance crate, no Python), as docs/PERFORMANCE_GATE.md describes.
"""

from __future__ import annotations

import argparse
import html
import json
import math
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MARK_START = "<!-- bench-summary:start -->"
MARK_END = "<!-- bench-summary:end -->"
HISTORY_LIMIT = 200  # runs listed on the page; every run's record stays in runs/


def geomean(values):
    values = [v for v in values if v]
    return math.exp(sum(math.log(v) for v in values) / len(values)) if values else None


def speed(reference_ms, nanolance_ms):
    if not reference_ms or not nanolance_ms:
        return None
    return reference_ms / nanolance_ms


def fx(v):
    return "—" if v is None else f"{v:.2f}×"


def matrix_summary(matrix: dict) -> dict:
    """Geometric means of the matrix, pylance as the reference, and how many data types nanolance wins."""
    datasets = matrix["datasets"]

    def ratios(get):
        return [s for s in (get(rec) for rec in datasets.values()) if s]

    def wins(get):
        xs = ratios(get)
        return sum(1 for s in xs if s > 1), len(xs)

    def read(key_ref, key_nl):
        return lambda r: speed(r["read_ms"].get(key_ref), r["read_ms"].get(key_nl))

    def write(key_ref, key_nl):
        return lambda r: speed(r["write_ms"].get(key_ref), r["write_ms"].get(key_nl))

    rows = {
        "read_cpp_mt": (read("rust-lance <- rust-lance", "nanolance-cpp-mt <- nanolance")),
        "read_cpp_1c": (read("rust-lance-1c <- rust-lance", "nanolance-cpp <- nanolance")),
        "write_cpp_mt": (write("rust-lance", "nanolance-cpp-mt")),
        "write_cpp_1c": (write("rust-lance-1c", "nanolance-cpp")),
        "read_py_mt": (read("rust-lance <- rust-lance", "nanolance-py <- nanolance")),
        "write_py_mt": (write("rust-lance", "nanolance-py")),
    }
    out = {}
    for name, get in rows.items():
        values = ratios(get)
        won, total = wins(get)
        out[name] = {"speed": geomean(values), "wins": won, "of": total}
    failures = sum(len(r.get("errors", {})) for r in datasets.values())
    out["failures"] = failures
    return out


def index_summary(scalar: dict | None, vector: dict | None, fts: dict | None) -> dict:
    """Geometric mean speed over each index suite's aspects, against Rust Lance, and recall deltas."""
    out = {}

    def stats(items):
        xs = [rec["rust"]["median_ms"] / rec["nanolance"]["median_ms"] for rec in items
              if rec["rust"]["median_ms"] and rec["nanolance"]["median_ms"]]
        won = sum(1 for x in xs if x > 1)
        return {"speed": geomean(xs), "wins": won, "of": len(xs)}

    if scalar:
        out["scalar_build"] = stats(scalar["builds"].values())
        out["scalar_query"] = stats(scalar["queries"].values())
    if vector:
        out["vector_build"] = stats(vector["builds"].values())
        out["vector_search"] = stats(vector["searches"].values())
        out["vector_recall"] = {
            kind: {"rust": rec["rust"], "nanolance": rec["nanolance"]} for kind, rec in vector["recall@10"].items()
        }
    if fts:
        out["fts_build"] = stats(fts["builds"].values())
        out["fts_search"] = stats(fts["searches"].values())
    return out


# The rows of the summary table: label, which summary, and the key within it.
TABLE = [
    ("Read, C++ library, all cores", "matrix", "read_cpp_mt", "pylance"),
    ("Read, C++ library, 1 core", "matrix", "read_cpp_1c", "pylance"),
    ("Write, C++ library, all cores", "matrix", "write_cpp_mt", "pylance"),
    ("Write, C++ library, 1 core", "matrix", "write_cpp_1c", "pylance"),
    ("Read, Python binding, all cores", "matrix", "read_py_mt", "pylance"),
    ("Write, Python binding, all cores", "matrix", "write_py_mt", "pylance"),
    ("Scalar indexes, build", "index", "scalar_build", "Rust Lance"),
    ("Scalar indexes, query", "index", "scalar_query", "Rust Lance"),
    ("Vector indexes, build", "index", "vector_build", "Rust Lance"),
    ("Vector indexes, search", "index", "vector_search", "Rust Lance"),
    ("Full-text index, build", "index", "fts_build", "Rust Lance"),
    ("Full-text index, search", "index", "fts_search", "Rust Lance"),
]


def table_rows(summary: dict) -> list[tuple[str, str, str, str]]:
    rows = []
    for label, group, key, reference in TABLE:
        if group == "matrix":
            rec = summary["matrix"].get(key)
        else:
            rec = summary["index"].get(key)
        if not rec:
            rows.append((label, reference, "not measured", ""))
            continue
        wins = f"{rec['wins']} of {rec['of']}"
        rows.append((label, reference, fx(rec["speed"]), wins))
    return rows


def markdown_table(summary: dict) -> str:
    lines = ["| Benchmark | Reference | nanolance speed | Faster in |", "|---|---|---:|---:|"]
    for label, reference, speed_text, wins in table_rows(summary):
        lines.append(f"| {label} | {reference} | {speed_text} | {wins} |")
    return "\n".join(lines)


def svg_table(summary: dict, gate: str, run: dict) -> str:
    """The summary table as an image: width fixed, one line per row, plain text (no fonts to load)."""
    rows = table_rows(summary)
    width, row_h, top = 760, 26, 92
    height = top + row_h * (len(rows) + 1) + 40
    cols = [16, 300, 470, 600, 740]
    esc = html.escape
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
           f'viewBox="0 0 {width} {height}" font-family="-apple-system, Segoe UI, Helvetica, Arial, sans-serif">',
           f'<rect width="{width}" height="{height}" rx="10" fill="#ffffff" stroke="#d0d7de"/>',
           f'<text x="16" y="30" font-size="17" font-weight="600" fill="#1f2328">nanolance benchmarks</text>',
           f'<text x="16" y="50" font-size="12" fill="#57606a">{esc(run["summary_line"])}</text>',
           f'<text x="16" y="70" font-size="12" fill="#57606a">Speed = reference time / nanolance time '
           f'(above 1× is faster). Gate: {esc(gate)}.</text>']
    header = ["Benchmark", "Reference", "nanolance speed", "Faster in"]
    y = top
    out.append(f'<rect x="8" y="{y - 18}" width="{width - 16}" height="{row_h}" fill="#f6f8fa"/>')
    for x, text in zip(cols[:4], header):
        out.append(f'<text x="{x}" y="{y}" font-size="12" font-weight="600" fill="#1f2328">{esc(text)}</text>')
    for label, reference, speed_text, wins in rows:
        y += row_h
        out.append(f'<line x1="16" x2="{width - 16}" y1="{y - 18}" y2="{y - 18}" stroke="#eaeef2"/>')
        values = [label, reference, speed_text, wins]
        for x, text in zip(cols[:4], values):
            out.append(f'<text x="{x}" y="{y}" font-size="13" fill="#1f2328">{esc(text)}</text>')
    out.append(f'<text x="16" y="{height - 14}" font-size="11" fill="#57606a">{esc(run["id"])} · '
               f'{esc(run["branch"])} · {esc(run["commit"])}</text>')
    out.append("</svg>")
    return "\n".join(out) + "\n"


def summary_line(run: dict) -> str:
    return f"{run['date']} · {run['cpu']} · {run['cores']} cores · branch {run['branch']} · commit {run['commit']}"


def html_page(record: dict, history: list[dict], gate_log: str) -> str:
    esc = html.escape
    s = record["summary"]
    run = record["run"]
    m = record["matrix"]

    def cell(v):
        return esc(str(v))

    summary_rows = "".join(
        f"<tr><td>{cell(label)}</td><td>{cell(reference)}</td><td class='n'>{cell(speed_text)}</td>"
        f"<td class='n'>{cell(wins)}</td></tr>"
        for label, reference, speed_text, wins in table_rows(s))

    matrix_rows = []
    for name, rec in m["datasets"].items():
        def f(a, b, what):
            x = speed(rec[what].get(a), rec[what].get(b))
            return fx(x)
        matrix_rows.append(
            f"<tr><td><code>{cell(name)}</code></td>"
            f"<td class='n'>{f('rust-lance <- rust-lance', 'nanolance-cpp-mt <- nanolance', 'read_ms')}</td>"
            f"<td class='n'>{f('rust-lance <- rust-lance', 'nanolance-py <- nanolance', 'read_ms')}</td>"
            f"<td class='n'>{f('rust-lance', 'nanolance-cpp-mt', 'write_ms')}</td>"
            f"<td class='n'>{f('rust-lance', 'nanolance-py', 'write_ms')}</td></tr>")

    def index_rows(section: dict | None):
        if not section:
            return "<tr><td colspan='4'>not measured</td></tr>"
        out = []
        for name, rec in section.items():
            out.append(f"<tr><td><code>{cell(name)}</code></td>"
                       f"<td class='n'>{rec['rust']['median_ms']:.3f}</td>"
                       f"<td class='n'>{rec['nanolance']['median_ms']:.3f}</td>"
                       f"<td class='n'>{fx(rec['rust']['median_ms'] / rec['nanolance']['median_ms'])}</td></tr>")
        return "".join(out)

    idx = record.get("index", {})
    recall_rows = "".join(
        f"<tr><td>{cell(kind)}</td><td class='n'>{rec['rust']:.3f}</td><td class='n'>{rec['nanolance']:.3f}</td></tr>"
        for kind, rec in (s["index"].get("vector_recall") or {}).items())

    history_rows = "".join(
        f"<tr><td>{cell(h['date'])}</td><td>{cell(h['branch'])}</td><td><code>{cell(h['commit'])}</code></td>"
        f"<td>{cell(h['gate'])}</td><td class='n'>{cell(fx(h['matrix_read_cpp_mt']))}</td>"
        f"<td class='n'>{cell(fx(h['scalar_query']))}</td><td class='n'>{cell(fx(h['vector_search']))}</td>"
        f"<td class='n'>{cell(fx(h['fts_search']))}</td>"
        f"<td><a href='{cell(h['record'])}'>json</a>"
        + (f" · <a href='{cell(h['run_url'])}'>run</a>" if h.get("run_url") else "") + "</td></tr>"
        for h in history)

    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>nanolance benchmarks</title>
<style>
:root {{ color-scheme: light dark; --fg:#1f2328; --muted:#57606a; --line:#d0d7de; --bg:#ffffff; --band:#f6f8fa; }}
@media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{ --fg:#e6edf3; --muted:#9198a1; --line:#3d444d; --bg:#0d1117; --band:#161b22; }} }}
:root[data-theme="dark"] {{ --fg:#e6edf3; --muted:#9198a1; --line:#3d444d; --bg:#0d1117; --band:#161b22; }}
body {{ background:var(--bg); color:var(--fg); font:15px/1.5 -apple-system,Segoe UI,Helvetica,Arial,sans-serif; margin:0; padding:24px 16px; }}
main {{ max-width:980px; margin:0 auto; }}
h1 {{ font-size:24px; margin:0 0 4px; }} h2 {{ font-size:18px; margin:32px 0 8px; border-bottom:1px solid var(--line); padding-bottom:4px; }}
p, .muted {{ color:var(--muted); }}
table {{ border-collapse:collapse; width:100%; margin:8px 0 12px; font-size:14px; }}
th, td {{ border-bottom:1px solid var(--line); padding:6px 8px; text-align:left; vertical-align:top; }}
th {{ background:var(--band); font-weight:600; }}
td.n, th.n {{ text-align:right; font-variant-numeric:tabular-nums; }}
code {{ font-size:13px; }} a {{ color:#0969da; }}
.wrap {{ overflow-x:auto; }}
</style></head>
<body><main>
<h1>nanolance benchmarks</h1>
<p class="muted">{esc(summary_line(run))} · <a href="{esc(run['run_url'] or '#')}">CI run</a> · gate: <strong>{esc(record['gate'])}</strong></p>
<p>Speed is the reference's median time over nanolance's: above 1× nanolance is faster. The matrix compares with
pylance; the index suites compare with Rust Lance (the <code>lance</code> crate, no Python in the process).
Each sample repeats until 10 seconds of timed work. <a href="runs/index.json">All runs</a> ·
<a href="latest.json">latest.json</a></p>

<h2>Summary</h2>
<div class="wrap"><table><thead><tr><th>Benchmark</th><th>Reference</th><th class="n">nanolance speed</th><th class="n">Faster in</th></tr></thead>
<tbody>{summary_rows}</tbody></table></div>
<p class="muted">Matrix: {len(m['datasets'])} data types, geometric means over the types.
Reads that returned wrong data or failed: {s['matrix']['failures']}.</p>

<h2>Matrix, per data type (pylance as the reference)</h2>
<div class="wrap"><table><thead><tr><th>Data type</th><th class="n">C++ read, all cores</th><th class="n">Python read</th>
<th class="n">C++ write, all cores</th><th class="n">Python write</th></tr></thead><tbody>{''.join(matrix_rows)}</tbody></table></div>

<h2>Scalar indexes (Rust Lance = 1.00×)</h2>
<div class="wrap"><table><thead><tr><th>Aspect</th><th class="n">Rust Lance ms</th><th class="n">nanolance ms</th><th class="n">speed</th></tr></thead>
<tbody>{index_rows(idx.get('scalar', {}).get('builds'))}</tbody></table></div>
<div class="wrap"><table><thead><tr><th>Query</th><th class="n">Rust Lance ms</th><th class="n">nanolance ms</th><th class="n">speed</th></tr></thead>
<tbody>{index_rows(idx.get('scalar', {}).get('queries'))}</tbody></table></div>

<h2>Vector indexes</h2>
<div class="wrap"><table><thead><tr><th>Aspect</th><th class="n">Rust Lance ms</th><th class="n">nanolance ms</th><th class="n">speed</th></tr></thead>
<tbody>{index_rows(idx.get('vector', {}).get('builds'))}{index_rows(idx.get('vector', {}).get('searches'))}</tbody></table></div>
<div class="wrap"><table><thead><tr><th>Index</th><th class="n">recall@10, Rust Lance</th><th class="n">recall@10, nanolance</th></tr></thead>
<tbody>{recall_rows}</tbody></table></div>

<h2>Full-text</h2>
<div class="wrap"><table><thead><tr><th>Aspect</th><th class="n">Rust Lance ms</th><th class="n">nanolance ms</th><th class="n">speed</th></tr></thead>
<tbody>{index_rows(idx.get('fts', {}).get('builds'))}{index_rows(idx.get('fts', {}).get('searches'))}</tbody></table></div>

<h2>Performance gate</h2>
<pre class="wrap" style="white-space:pre-wrap">{esc(gate_log.strip() or 'not run')}</pre>

<h2>History</h2>
<div class="wrap"><table><thead><tr><th>Date (UTC)</th><th>Branch</th><th>Commit</th><th>Gate</th>
<th class="n">Matrix read, C++</th><th class="n">Scalar query</th><th class="n">Vector search</th><th class="n">Full-text search</th><th>Links</th></tr></thead>
<tbody>{history_rows}</tbody></table></div>
<p class="muted">Machine: {esc(run['cpu'])}, {run['cores']} cores. Each row's JSON holds the raw results of that run.</p>
</main></body></html>
"""


def write_readme(path: Path, table: str, run: dict) -> bool:
    text = path.read_text()
    if MARK_START not in text or MARK_END not in text:
        raise SystemExit(f"{path}: no {MARK_START} ... {MARK_END} markers")
    head, rest = text.split(MARK_START, 1)
    _, tail = rest.split(MARK_END, 1)
    block = (f"{MARK_START}\n\nLatest run: {run['date']} on `{run['branch']}` (`{run['commit']}`), "
             f"[full results](https://yoavbendor.github.io/nanolance/bench/).\n\n{table}\n\n{MARK_END}")
    new = head + block + tail
    if new == text:
        return False
    path.write_text(new)
    return True


def load(path: str | None):
    return json.loads(Path(path).read_text()) if path else None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--matrix", required=True)
    ap.add_argument("--scalar")
    ap.add_argument("--vector")
    ap.add_argument("--fts")
    ap.add_argument("--gate", default="not run", help="pass, fail, or skipped")
    ap.add_argument("--gate-log", type=Path)
    ap.add_argument("--site", type=Path, required=True)
    ap.add_argument("--readme", type=Path, help="update the summary table in this README")
    ap.add_argument("--branch", default=os.environ.get("GITHUB_REF_NAME", "local"))
    ap.add_argument("--run-url", default=(
        f"{os.environ.get('GITHUB_SERVER_URL', 'https://github.com')}/{os.environ.get('GITHUB_REPOSITORY', '')}"
        f"/actions/runs/{os.environ.get('GITHUB_RUN_ID', '')}" if os.environ.get("GITHUB_RUN_ID") else ""))
    args = ap.parse_args(argv)

    matrix = load(args.matrix)
    scalar, vector, fts = load(args.scalar), load(args.vector), load(args.fts)
    env = matrix.get("environment", {})
    commit = os.environ.get("GITHUB_SHA", "")[:7] or subprocess_commit()
    now = datetime.now(timezone.utc)
    run_id = now.strftime("%Y%m%d-%H%M%S") + f"-{commit or 'local'}"
    index_env = next((x["environment"] for x in (scalar, vector, fts) if x), {})
    run = {
        "id": run_id,
        "date": now.strftime("%Y-%m-%d %H:%M UTC"),
        "branch": args.branch,
        "commit": commit or "local",
        "cpu": env.get("cpu") or index_env.get("cpu", "unknown"),
        "cores": env.get("cores") or index_env.get("cores", "?"),
        "run_url": args.run_url,
    }
    run["summary_line"] = summary_line(run)
    gate_log = args.gate_log.read_text() if args.gate_log and args.gate_log.exists() else ""

    summary = {"matrix": matrix_summary(matrix), "index": index_summary(scalar, vector, fts)}
    index = {}
    if scalar:
        index["scalar"] = {"builds": scalar["builds"], "queries": scalar["queries"]}
    if vector:
        index["vector"] = {"builds": vector["builds"], "searches": vector["searches"], "recall@10": vector["recall@10"]}
    if fts:
        index["fts"] = {"builds": fts["builds"], "searches": fts["searches"]}
    record = {"run": run, "gate": args.gate, "summary": summary, "matrix": matrix, "index": index, "gate_log": gate_log}

    site: Path = args.site
    (site / "runs").mkdir(parents=True, exist_ok=True)
    record_path = site / "runs" / f"{run_id}.json"
    record_path.write_text(json.dumps(record, indent=1))
    (site / "latest.json").write_text(json.dumps(record, indent=1))

    index_file = site / "runs" / "index.json"
    history = json.loads(index_file.read_text()) if index_file.exists() else []
    s = summary
    entry = {
        "id": run_id, "date": run["date"], "branch": run["branch"], "commit": run["commit"], "gate": args.gate,
        "record": f"runs/{run_id}.json", "run_url": run["run_url"],
        "matrix_read_cpp_mt": s["matrix"]["read_cpp_mt"]["speed"],
        "scalar_query": (s["index"].get("scalar_query") or {}).get("speed"),
        "vector_search": (s["index"].get("vector_search") or {}).get("speed"),
        "fts_search": (s["index"].get("fts_search") or {}).get("speed"),
    }
    history = [entry] + [h for h in history if h["id"] != run_id]
    index_file.write_text(json.dumps(history[:HISTORY_LIMIT * 5], indent=1))

    (site / "index.html").write_text(html_page(record, history[:HISTORY_LIMIT], gate_log))
    (site / "summary.svg").write_text(svg_table(summary, args.gate, run))

    if args.readme:
        changed = write_readme(args.readme, markdown_table(summary), run)
        print(f"README summary {'updated' if changed else 'unchanged'}")
    print(f"published {run_id} to {site}")
    return 0


def subprocess_commit() -> str:
    import subprocess
    out = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    return out.stdout.strip()


if __name__ == "__main__":
    sys.exit(main())
